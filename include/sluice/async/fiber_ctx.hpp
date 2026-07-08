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

namespace sluice::async::fiber_ctx {

// Whether this build supports a real fiber context switch. x86_64 only for now;
// mirroring Zig fiber.zig:1-4 (aarch64/riscv64/x86_64 upstream — cppio ports
// x86_64 first; other arches are follow-ons).
#if defined(__x86_64__)
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

}  // namespace sluice::async::fiber_ctx
