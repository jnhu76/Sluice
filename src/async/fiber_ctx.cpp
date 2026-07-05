// Implementation of the x86_64 fiber context switch (sluice-CORE-E2/E3).
//
// ISOLATED. No I/O, no scheduler, no Future/WaitPolicy/Group/Batch. See
// include/sluice/async/fiber_ctx.hpp for the model and provenance (Zig
// lib/std/Io/fiber.zig x86_64 block).
#include <sluice/async/fiber_ctx.hpp>

#include <cstdint>

namespace sluice::async::fiber_ctx {

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
//     [resumed_by ptr]
//     [user_data ptr]
//   and rbp = 0, rip = &fiber_entry_trampoline.
//
//   The trampoline pops entry, then sets up rdi=resumed_by, rsi=user_data (the
//   Entry's two args per SysV AMD64: first arg in rdi, second in rsi) and
//   tail-calls entry. (No return address is on the stack — Entry must not
//   return.)
asm(
    ".text\n"
    ".globl fiber_entry_trampoline\n"
    ".type fiber_entry_trampoline, @function\n"
    "fiber_entry_trampoline:\n"
    "  popq %rax\n"          // rax = entry (Entry fn ptr), stashed by init_context
    "  popq %rdi\n"          // rdi = resumed_by (first arg, SysV AMD64)
    "  popq %rsi\n"          // rsi = user_data (second arg, SysV AMD64)
    "  jmpq *%rax\n"         // tail-call entry(rdi, rsi); no return
    ".size fiber_entry_trampoline, .-fiber_entry_trampoline\n"
);

Switch* context_switch(Switch* s) noexcept {
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

// ---- init_context (x86_64) -----------------------------------------------
// Set up a fresh Context so its first context_switch enters the trampoline,
// which tail-calls entry(resumed_by, user_data). The trampoline expects the
// init-time stack to hold [entry, resumed_by, user_data] (low -> high), with
// the initial rsp pointing at `entry` so the first `popq %rax` in the trampoline
// loads it. We also leave a zero return-slot below entry (i.e. at rsp-8 from
// the trampoline's perspective) so the SysV-AMD64 stack alignment invariant
// (rsp % 16 == 0 at a call) holds when the trampoline's jmp is reached: we
// arrange for rsp to be 16-aligned minus 8 at entry to the trampoline (mirroring
// a `call` having pushed a return address), then three pops (24 bytes) bring
// rsp to 16-alignment before the jmp.
bool init_context(Context& ctx, Entry entry, void* user_data,
                  std::byte* stack_base, std::size_t stack_size) noexcept {
    if (entry == nullptr || stack_base == nullptr || stack_size < 64) {
        return false;
    }
    // Top of the stack (highest address). Align down to 16 bytes (SysV AMD64).
    auto top = reinterpret_cast<std::uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<std::uintptr_t>(0xF);

    // Lay out the init frame high -> low:
    //   top-0x08:  user_data
    //   top-0x10:  resumed_by  (the Switch*; set by the FIRST context_switch
    //                            into this fiber — we leave a placeholder that
    //                            the trampoline will load; the real value is
    //                            whatever the resumer's rsi holds at the switch)
    //   top-0x18:  entry
    //   top-0x20:  dummy return slot (keeps 16-byte alignment at the jmp)
    //
    // Initial rsp points at top-0x18 (so the first popq in the trampoline
    // loads entry). resumed_by is conceptually "the Switch* that resumed me";
    // because context_switch passes s in rsi and the resumed fiber reads rsi at
    // label 0, but the trampoline runs BEFORE label 0 (it is the entry point),
    // we instead stash the resumed_by we want the entry to see at top-0x10.
    // For an isolated ping-pong test the value is arbitrary/unused; the real
    // scheduler (E4) will set it meaningfully.
    auto* p = reinterpret_cast<std::uint64_t*>(top);
    p[-1] = reinterpret_cast<std::uint64_t>(user_data);     // top-0x08
    p[-2] = 0;                                               // top-0x10: resumed_by placeholder
    p[-3] = reinterpret_cast<std::uint64_t>(entry);         // top-0x18
    p[-4] = 0;                                               // top-0x20: dummy return slot

    ctx.rsp = reinterpret_cast<std::uint64_t>(&p[-3]);      // initial rsp -> entry
    ctx.rbp = 0;
    ctx.rip = reinterpret_cast<std::uint64_t>(&fiber_entry_trampoline);
    return true;
}

#else  // non-x86_64: unsupported. init_context returns false so a caller that
       // gates on `supported` (the header constant) never reaches here in a
       // well-formed build; reaching it is a misuse and fails closed.
bool init_context(Context& /*ctx*/, Entry /*entry*/, void* /*user_data*/,
                  std::byte* /*stack_base*/, std::size_t /*stack_size*/) noexcept {
    return false;
}

// Provide a definition for the trampoline symbol on non-x86_64 so the TU links
// without an unresolved external (it is never called when supported == false).
extern "C" void fiber_entry_trampoline() {}

#endif

}  // namespace sluice::async::fiber_ctx
