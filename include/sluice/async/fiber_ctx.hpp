// sluice::async::fiber_ctx — x86_64 fiber context switch (sluice-CORE-E2/E3).
//
// ISOLATED execution primitive, source-derived from Zig's
// lib/std/Io/fiber.zig x86_64 Context + contextSwitch (fiber.zig:18-24, 244-254,
// 257-318). This file ports ONLY the stack-switching seam. It does NOT touch
// I/O, AsyncBackend, Future, WaitPolicy, Group, Batch, Reader, Writer, fd, or
// any scheduler. It is gated to x86_64; other architectures get an explicit
// unsupported stub so non-Evented builds compile cleanly.
//
// WHAT E2/E3 PROVES:
//   - isolated context switching and trampoline correctness.
// WHAT E2/E3 DOES NOT PROVE (do not conflate):
//   - Evented I/O correctness.
//   - scheduler liveness (the E4 success criterion).
//   - pending async operation behavior.
//   - Future/WaitPolicy integration.
//   - Group-on-Evented semantics.
//
// Model (from fiber.zig):
//   Context = { rsp, rbp, rip }              (3 words; callee-saved frame chain)
//   Switch  = { old: *Context, new: *Context }
//   contextSwitch(s: *const Switch) -> *const Switch
//     - saves current rsp/rbp + resume-label into *old
//     - loads rsp/rbp from *new, jmp *new.rip
//     - returns (in rsi) the Switch* of whoever resumed us, so the resume site
//       can recover the message that woke it.
//   The switch is a FULL ABI call boundary: all GP/FP/MXCSR/rflags are
//   clobbered (declared in the asm clobber list). No lazy FP save.
//
// Trampoline: a freshly-created Context has rip = &entry_trampoline, and its
// rsp/rbp are set up so the first resume enters entry_trampoline, which tail-
// calls the user entry function. The entry must end by switching away forever
// (it never returns into the trampoline).
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__SANITIZE_ADDRESS__)
#define SLUICE_FIBER_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SLUICE_FIBER_ASAN_ENABLED 1
#endif
#endif

#ifndef SLUICE_FIBER_ASAN_ENABLED
#define SLUICE_FIBER_ASAN_ENABLED 0
#endif

#if defined(__SANITIZE_THREAD__)
#define SLUICE_FIBER_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SLUICE_FIBER_TSAN_ENABLED 1
#endif
#endif

#ifndef SLUICE_FIBER_TSAN_ENABLED
#define SLUICE_FIBER_TSAN_ENABLED 0
#endif

namespace sluice::async::fiber_ctx {

// Whether this build supports a real fiber context switch. E15-P2-04: the
// accepted Evented scope is Linux x86_64 ONLY. The previous gate checked
// architecture alone (__x86_64__), which would admit x86_64 macOS/BSD targets
// into the System V AMD64 fiber asm without a verified ELF/stack/asan story on
// those OSes. The corrected gate requires BOTH __x86_64__ AND __linux__; all
// other targets get supported=false and the Evented public admission boundary
// (detail::require_evented_supported) fails fast deterministically.
// Portable Threaded async (Group::async_threaded) does NOT consult this gate
// and remains available everywhere.
//
// Mirroring Zig fiber.zig:1-4 (aarch64/riscv64/x86_64 upstream — cppio ports
// x86_64-linux first; other arch/OS combos are follow-ons, not this task).
#if defined(__x86_64__) && defined(__linux__)
inline constexpr bool supported = true;
#else
inline constexpr bool supported = false;
#endif

// The CPU state of an inactive fiber. Mirrors Zig fiber.zig:18-22 (x86_64).
// Three words: stack pointer, frame pointer, instruction pointer. extern/C
// layout so the asm can address fields at fixed offsets (0/8/16).
struct Context {
    std::uint64_t rsp = 0;
    std::uint64_t rbp = 0;
    std::uint64_t rip = 0;

#if SLUICE_FIBER_ASAN_ENABLED
    // AddressSanitizer tracks each alternate stack and its fake stack. The
    // first three fields retain their fixed asm offsets.
    void* asan_fake_stack = nullptr;
    const void* asan_stack_bottom = nullptr;
    std::size_t asan_stack_size = 0;
#endif

#if SLUICE_FIBER_TSAN_ENABLED
    // ThreadSanitizer keeps a logical call stack per fiber.  The first three
    // fields intentionally retain their fixed asm offsets; this metadata is
    // used only by TSan builds and lives after the native context.
    void* sanitizer_fiber = nullptr;
    bool owns_sanitizer_fiber = false;
#endif

#if SLUICE_FIBER_ASAN_ENABLED || SLUICE_FIBER_TSAN_ENABLED
    Context() noexcept = default;
#if SLUICE_FIBER_TSAN_ENABLED
    ~Context() noexcept;
#else
    ~Context() noexcept = default;
#endif
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;
#endif
};

// A context-switch request: save the current CPU state into *old, restore the
// state stored in *new. Mirrors Zig fiber.zig:26.
struct Switch {
    Context* old;
    const Context* new_;
};

// The signature of the user-provided entry function a fresh fiber runs on first
// resume. It receives the Switch* that resumed it (so it can learn who woke
// it) and the user_data pointer the fiber was created with. The entry must NOT
// return: it must end by contextSwitch-ing away forever. (A return from entry
// is undefined behavior — the trampoline has no return address.)
using Entry = void (*)(Switch* resumed_by, void* user_data);

#if defined(__x86_64__)

// The core primitive. Saves current rsp/rbp + resume label into s->old; loads
// rsp/rbp from s->new and jumps to s->new->rip. Returns the Switch* that
// resumed the caller (in rsi) — which may differ from `s` if the caller is
// later resumed by a different switch site.
//
// Source: Zig fiber.zig:244-254 (x86_64 block) + 255-318 (clobbers). Ported to
// GCC/Clang extended asm (AT&T syntax). Defined out-of-line in
// src/async/fiber_ctx.cpp so the asm lives in one TU.
Switch* context_switch(Switch* s) noexcept;

#else  // non-x86_64: unsupported stub. Compiles; calling it aborts so a misuse
       // fails loudly rather than silently emulating threads-per-task (E0 ADR §7).
inline Switch* context_switch(Switch* /*s*/) noexcept {
    // No supported context switch on this architecture. The scheduler (E4+)
    // must gate on `supported` and fail/disable cleanly rather than call here.
    return nullptr;
}
#endif

// Permanently leave `old` and switch to `new`. Unlike context_switch(), this
// path tells ASan to discard the departing fiber's fake stack and never
// returns. Use only after the fiber has reached a terminal state.
//
// Declared unconditionally (like init_context / reset_context) so callers such
// as scheduler.cpp see the same symbol on every supported architecture; the
// definition is platform-specific (x86_64 performs the native handoff, other
// arches abort — see src/async/fiber_ctx.cpp). The [[noreturn]] / noexcept
// contract is identical for both definitions.
[[noreturn]] void context_switch_final(Context& old, const Context& new_) noexcept;

// Initialize a fresh Context so that its FIRST context_switch into it begins
// executing `entry(resumed_by, user_data)`. The context's stack is `[stack_base,
// stack_base + stack_size)`; on x86_64 the initial rsp is set per the
// entry-trampoline convention (see src/async/fiber_ctx.cpp). Returns false if
// the inputs are invalid (null entry, non-positive stack size, unaligned
// stack). Gated to x86_64; returns false on unsupported arches.
//
// Mirrors how Zig sets up a fresh fiber's initial registers (Uring.zig:1073-
// 1085 sets sp/fp/pc so the first resume enters AsyncClosure.entry -> .call).
bool init_context(Context& ctx, Entry entry, void* user_data,
                  std::byte* stack_base, std::size_t stack_size) noexcept;

// Forget saved native state and release any sanitizer-owned logical fiber.
// Scheduler contexts refer to the OS thread's TSan context and never own it.
void reset_context(Context& ctx) noexcept;

}  // namespace sluice::async::fiber_ctx
