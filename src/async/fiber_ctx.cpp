// Implementation of the x86_64 fiber context switch (sluice-CORE-E2/E3).
//
// ISOLATED. No I/O, no scheduler, no Future/WaitPolicy/Group/Batch. See
// include/sluice/async/fiber_ctx.hpp for the model and provenance (Zig
// lib/std/Io/fiber.zig x86_64 block).
#include <sluice/async/fiber_ctx.hpp>

#include <cstdlib>
#include <cstdint>

#if SLUICE_FIBER_ASAN_ENABLED
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save,
                                    const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                     const void** bottom_old,
                                     std::size_t* size_old);
}
#endif

#if SLUICE_FIBER_TSAN_ENABLED
extern "C" {
void* __tsan_get_current_fiber();
void* __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void* fiber);
void __tsan_switch_to_fiber(void* fiber, unsigned flags);
}
#endif

namespace sluice::async::fiber_ctx {

#if SLUICE_FIBER_TSAN_ENABLED
namespace {

void release_sanitizer_fiber(Context& ctx) noexcept {
    if (ctx.owns_sanitizer_fiber && ctx.sanitizer_fiber != nullptr) {
        __tsan_destroy_fiber(ctx.sanitizer_fiber);
    }
    ctx.sanitizer_fiber = nullptr;
    ctx.owns_sanitizer_fiber = false;
}

}  // namespace

Context::~Context() noexcept {
    release_sanitizer_fiber(*this);
}
#endif

void reset_context(Context& ctx) noexcept {
#if SLUICE_FIBER_ASAN_ENABLED
    ctx.asan_fake_stack = nullptr;
    ctx.asan_stack_bottom = nullptr;
    ctx.asan_stack_size = 0;
#endif
#if SLUICE_FIBER_TSAN_ENABLED
    release_sanitizer_fiber(ctx);
#endif
    ctx.rsp = 0;
    ctx.rbp = 0;
    ctx.rip = 0;
}

// ---- The entry trampoline ------------------------------------------------
// A freshly-initialized fiber's `rip` points here, with rsp/rbp arranged so
// the first instruction the fiber executes is the trampoline's prologue. The
// trampoline recovers (resumed_by, user_data) — passed via the init-time stack
// layout set up by init_context — and tail-calls the user Entry. The Entry must
// NOT return (it has no return address); if it did, behavior is undefined.
//
// We pass the args in registers (rdi = resumed_by, rsi = user_data) per the
// System V AMD64 ABI. init_context stores them on the fresh fiber's initial
// stack and arranges rip so the trampoline loads them.
extern "C" void fiber_entry_trampoline();

// The actual trampoline body: reads the init-time-stashed (entry, resumed_by,
// user_data) and calls entry(resumed_by, user_data). Defined in asm below.
// extern "C" linkage so the asm can name it.

#if defined(__x86_64__)

#if SLUICE_FIBER_ASAN_ENABLED
__attribute__((no_sanitize("address")))
#endif
void finish_asan_switch(Switch* resumed_by, void* fake_stack) noexcept {
#if SLUICE_FIBER_ASAN_ENABLED
    const void* old_bottom = nullptr;
    std::size_t old_size = 0;
    __sanitizer_finish_switch_fiber(fake_stack, &old_bottom, &old_size);
    resumed_by->old->asan_stack_bottom = old_bottom;
    resumed_by->old->asan_stack_size = old_size;
#else
    (void)resumed_by;
    (void)fake_stack;
#endif
}

#if SLUICE_FIBER_ASAN_ENABLED
__attribute__((no_sanitize("address")))
#endif
extern "C" void fiber_entry_trampoline_bridge(
    Switch* resumed_by, void* user_data, Entry entry) {
#if SLUICE_FIBER_ASAN_ENABLED
    finish_asan_switch(resumed_by, resumed_by->new_->asan_fake_stack);
#endif
    entry(resumed_by, user_data);
}

// ---- context_switch (x86_64) ---------------------------------------------
// AT&T port of Zig fiber.zig:244-254. Extended asm with the full clobber list
// (fiber.zig:257-318): all GP regs, xmm/ymm/zmm, mmx, mxcsr, fpsr, fpcr,
// rflags, dirflag, memory. The switch is a full ABI call boundary.
//
// Inputs:  {rsi} = s   (the Switch*)
// Outputs: {rsi} = the Switch* that resumed us (may differ from s)
//
//   movq 0(%rsi), %rax      ; rax = s->old  (Context*)
//   movq 8(%rsi), %rcx      ; rcx = s->new_ (Context*)
//   leaq 0f(%rip), %rdx     ; rdx = address of resume label 0
//   movq %rsp, 0(%rax)      ; *old.rsp  = current rsp
//   movq %rbp, 8(%rax)      ; *old.rbp  = current rbp
//   movq %rdx, 16(%rax)     ; *old.rip  = resume label
//   movq 0(%rcx), %rsp      ; rsp = *new.rsp
//   movq 8(%rcx), %rbp      ; rbp = *new.rbp
//   jmpq *16(%rcx)          ; jmp *new.rip
// 0:                        ; (resume point — control returns here when this
//                           ;  context is switched back into)
//
// The "={rsi}" output captures rsi at the resume site, returning the Switch*
// of whoever resumed us.
//
// The trampoline is defined here too because it shares the TU with the asm and
// must agree on the init-time stack convention with init_context.
//
// Init-time stack convention (System V AMD64, 16-byte aligned at call):
//   init_context sets the fresh fiber's rsp to point at a frame that holds, in
//   order (low -> high addresses):
//     [entry fn ptr]    <- initial rsp points here on first resume
//     [alignment slot]
//     [user_data ptr]
//   and rbp = 0, rip = &fiber_entry_trampoline.
//
//   The trampoline preserves the incoming rsi (the real Switch*), pops entry
//   and user_data, then calls a C++ bridge. The bridge completes ASan's first
//   switch onto a fresh stack before calling entry(resumed_by, user_data).
//   `callq` gives the bridge a normal SysV return address. Entry must switch
//   away via context_switch before returning; `ud2` traps if it does.
asm(
    ".text\n"
    ".globl fiber_entry_trampoline\n"
    ".type fiber_entry_trampoline, @function\n"
    "fiber_entry_trampoline:\n"
    "  popq %rdx\n"          // rdx = entry (third bridge arg)
    "  addq $8, %rsp\n"      // discard the alignment slot
    "  movq %rsi, %rdi\n"    // rdi = incoming Switch* (first bridge arg)
    "  popq %rsi\n"          // rsi = user_data (second bridge arg)
    "  callq fiber_entry_trampoline_bridge\n"
    "  ud2\n"                // entry must NOT return; trap if it does
    ".size fiber_entry_trampoline, .-fiber_entry_trampoline\n"
);

namespace {

#if SLUICE_FIBER_ASAN_ENABLED || SLUICE_FIBER_TSAN_ENABLED
__attribute__((disable_sanitizer_instrumentation))
#endif
Switch* native_context_switch(Switch* s) noexcept {
    Switch* resumed_by;
    __asm__ volatile (
        "movq 0(%%rsi), %%rax\n\t"
        "movq 8(%%rsi), %%rcx\n\t"
        "leaq 0f(%%rip), %%rdx\n\t"
        "movq %%rsp, 0(%%rax)\n\t"
        "movq %%rbp, 8(%%rax)\n\t"
        "movq %%rdx, 16(%%rax)\n\t"
        "movq 0(%%rcx), %%rsp\n\t"
        "movq 8(%%rcx), %%rbp\n\t"
        "jmpq *16(%%rcx)\n\t"
        "0:\n\t"
        : "=S"(resumed_by)                 // output: rsi (the {rsi} constraint)
        : "S"(s)                            // input: rsi = s
        : "rax", "rcx", "rdx", "rbx", "rdi", "r8", "r9", "r10", "r11",
          "r12", "r13", "r14", "r15",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)",
          "cc", "memory"
        // GCC/Clang x86 clobber names: "cc" covers rflags; "memory" covers the
        // memory fence. mxcsr/fpsr/fpcr have no GCC register-clobber name on
        // x86; the SysV AMD64 ABI treats the FP control/status words as part
        // of the call boundary the xmm/"st" clobbers + "cc" already establish.
        // (Zig's fiber.zig clobbers them explicitly because Zig's asm model
        // names them; the practical effect — full-ABI-call boundary — is the
        // same.) ymm/zmm upper halves are covered by the ABI's call-clobbered
        // treatment of AVX registers; cppio does not emit AVX-512 across a
        // switch.
    );
    return resumed_by;
}

#if SLUICE_FIBER_TSAN_ENABLED
__attribute__((disable_sanitizer_instrumentation))
#endif
void prepare_tsan_switch(Switch* s) noexcept {
#if SLUICE_FIBER_TSAN_ENABLED
    // Match compiler-rt's fiber_asm test protocol: save the current logical
    // fiber on the context being suspended, then tell TSan which logical
    // stack will become active immediately before the native stack switch.
    s->old->sanitizer_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(s->new_->sanitizer_fiber, 0);
#else
    (void)s;
#endif
}

}  // namespace

#if SLUICE_FIBER_TSAN_ENABLED
__attribute__((no_sanitize("thread")))
#endif
Switch* context_switch(Switch* s) noexcept {
#if SLUICE_FIBER_ASAN_ENABLED
    __sanitizer_start_switch_fiber(
        &s->old->asan_fake_stack, s->new_->asan_stack_bottom,
        s->new_->asan_stack_size);
#endif
    prepare_tsan_switch(s);
    Switch* resumed_by = native_context_switch(s);
#if SLUICE_FIBER_ASAN_ENABLED
    finish_asan_switch(resumed_by, s->old->asan_fake_stack);
#endif
    return resumed_by;
}

#if SLUICE_FIBER_ASAN_ENABLED || SLUICE_FIBER_TSAN_ENABLED
__attribute__((disable_sanitizer_instrumentation))
#endif
void context_switch_final(Context& old, const Context& new_) noexcept {
    // This Switch intentionally lives in an uninstrumented frame. ASan may
    // destroy the departing fiber's fake stack in start_switch_fiber(nullptr),
    // so the native handoff must not retain a pointer into that fake stack.
    Switch s{&old, &new_};
#if SLUICE_FIBER_ASAN_ENABLED
    __sanitizer_start_switch_fiber(nullptr, new_.asan_stack_bottom,
                                   new_.asan_stack_size);
#endif
    prepare_tsan_switch(&s);
    (void)native_context_switch(&s);
    std::abort();  // A terminal context must never be resumed.
}

// ---- init_context (x86_64) -----------------------------------------------
// Set up a fresh Context so its first context_switch enters the trampoline,
// which `callq`s entry(resumed_by, user_data). The init frame holds
// [entry, resumed_by, user_data] high->low; initial rsp points at `entry` so
// the trampoline's first `popq %rax` loads it. See the alignment-math comment
// inside the function body for the SysV-AMD64 invariant.
bool init_context(Context& ctx, Entry entry, void* user_data,
                  std::byte* stack_base, std::size_t stack_size) noexcept {
    if (entry == nullptr || stack_base == nullptr || stack_size < 64) {
        return false;
    }
#if SLUICE_FIBER_TSAN_ENABLED
    release_sanitizer_fiber(ctx);
    ctx.sanitizer_fiber = __tsan_create_fiber(0);
    if (ctx.sanitizer_fiber == nullptr) {
        return false;
    }
    ctx.owns_sanitizer_fiber = true;
#endif
#if SLUICE_FIBER_ASAN_ENABLED
    ctx.asan_fake_stack = nullptr;
    ctx.asan_stack_bottom = stack_base;
    ctx.asan_stack_size = stack_size;
#endif
    // Alignment math (System V AMD64, rsp must be ≡ 8 (mod 16) at a callee's
    // first instruction — i.e. immediately after a `call`'s implicit push):
    //
    //   The trampoline is entered via `jmpq *16(%rcx)` (no push), so its entry
    //   rsp = whatever init set = R0.
    //   The trampoline does 3 `popq`s (+24) then a `callq` (push, -8) into the
    //   entry function. So the entry function's entry rsp = R0 + 24 - 8 = R0+16.
    //   We need (R0 + 16) ≡ 8 (mod 16)  →  R0 ≡ 8 (mod 16).
    //
    //   With top 16-aligned (top ≡ 0 (mod 16)), setting R0 = top - 24 gives
    //   R0 ≡ -24 ≡ 8 (mod 16). ✓
    //
    // Init frame layout (high -> low), 3 slots, no dummy:
    //   top-0x08:  user_data      (p[-1])
    //   top-0x10:  alignment slot (p[-2]); the real resumed_by Switch* arrives
    //                              in rsi from context_switch.
    //   top-0x18:  entry          (p[-3])  <- initial rsp points here
    auto top = reinterpret_cast<std::uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<std::uintptr_t>(0xF);  // 16-byte align down (SysV AMD64)
    auto* p = reinterpret_cast<std::uint64_t*>(top);
    p[-1] = reinterpret_cast<std::uint64_t>(user_data);     // top-0x08
    p[-2] = 0;                                               // top-0x10: alignment slot
    p[-3] = reinterpret_cast<std::uint64_t>(entry);         // top-0x18

    // R0 = &p[-3] = top-24. As shown above, R0 ≡ 8 (mod 16); after the
    // trampoline's 3 pops + callq, the entry function sees rsp ≡ 8 (mod 16).
    ctx.rsp = reinterpret_cast<std::uint64_t>(&p[-3]);
    ctx.rbp = 0;
    ctx.rip = reinterpret_cast<std::uint64_t>(&fiber_entry_trampoline);
    return true;
}

#else  // non-x86_64: unsupported. init_context returns false so a caller that
       // gates on `supported` (the header constant) never reaches here in a
       // well-formed build; reaching it is a misuse and fails closed.
[[noreturn]] void context_switch_final(Context& /*old*/,
                                       const Context& /*new_*/) noexcept {
    std::abort();
}

bool init_context(Context& /*ctx*/, Entry /*entry*/, void* /*user_data*/,
                  std::byte* /*stack_base*/, std::size_t /*stack_size*/) noexcept {
    return false;
}

// Provide a definition for the trampoline symbol on non-x86_64 so the TU links
// without an unresolved external (it is never called when supported == false).
extern "C" void fiber_entry_trampoline() {}

#endif

}  // namespace sluice::async::fiber_ctx
