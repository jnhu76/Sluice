// Tests for sluice::async::fiber_ctx — isolated x86_64 context switch
// (sluice-CORE-E2/E3).
//
// PROVES ONLY: context switching and trampoline correctness.
// DOES NOT PROVE: Evented I/O correctness, scheduler liveness, pending async
// operation behavior, Future/WaitPolicy integration, Group-on-Evented.
//
// No I/O, Future, WaitPolicy, AsyncBackend, Group, Batch, Reader, Writer, fd,
// or scheduler is involved. The main thread saves its own context, switches
// into a fresh fiber, the fiber runs and switches back, the main thread resumes
// at its suspension point. A two-fiber ping-pong variant proves message round-
// trip.
#include "harness.hpp"

#include <sluice/async/fiber_ctx.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace sluice::async::fiber_ctx;

namespace {

// A 16 KiB scratch stack for a fiber, 16-byte aligned so init_context's top
// alignment holds. Caller-owned; outlives the fiber that uses it.
struct ScratchStack {
    static constexpr std::size_t kBytes = 16 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Slice 1: the fiber increments a counter and switches back to the saved main
// context. `main_ctx` is filled by context_switch when the main thread calls
// into the fiber; the fiber switches back to it to return control.
struct Slice1State {
    Context main_ctx{};       // main thread's saved context
    Context fiber_ctx{};
    int ran = 0;
};

static void slice1_fiber(Switch* resumed_by, void* user_data) {
    (void)resumed_by;
    auto* st = static_cast<Slice1State*>(user_data);
    st->ran = 1;
    // Switch back to main. st->main_ctx was filled when main switched into us.
    Switch s;
    s.old = &st->fiber_ctx;     // our own context (unused on return — we end)
    s.new_ = &st->main_ctx;     // resume main
    (void)context_switch(&s);
    // Never reached: main does not switch back into us after this.
    __builtin_unreachable();
}

}  // namespace

// ---- Slice 1 (tracer): main -> fiber -> main ------------------------------
// The minimal proof: the main thread saves its context, switches into a fresh
// fiber, the fiber runs its body, switches back, the main thread resumes at the
// instruction after its context_switch call. If the trampoline or asm is wrong,
// we crash, hang, or never see ran==1.
SLUICE_TEST_CASE(fiber_context_switch_main_fiber_main) {
    if constexpr (!supported) {
        return;  // non-x86_64: skip cleanly (E0 ADR §7)
    }

    Slice1State st;
    ScratchStack fiber_stack;
    SLUICE_CHECK(init_context(st.fiber_ctx, slice1_fiber, &st,
                              fiber_stack.base(), fiber_stack.size()));

    // Switch into the fiber. context_switch fills st.main_ctx with our resume
    // state and jumps into st.fiber_ctx. When the fiber switches back to
    // &st.main_ctx, control resumes HERE (at label 0 inside context_switch).
    Switch s;
    s.old = &st.main_ctx;
    s.new_ = &st.fiber_ctx;
    Switch* resumed_by = context_switch(&s);

    // We're back. The fiber ran and switched into st.main_ctx.
    SLUICE_CHECK(st.ran == 1);
    // resumed_by is the Switch* the fiber used to wake us (&s in slice1_fiber,
    // whose .new_ = &st.main_ctx). Non-null proves the rsi return path works.
    SLUICE_CHECK(resumed_by != nullptr);
}

// ---- Slice 2: true two-fiber ping-pong with message round-trip -----------
// A genuine A<->B round trip. Main spawns fiber A; A switches into B; B
// observes the message A left, mutates a return value, switches back to A; A
// resumes, records the return; A switches back to main. Three contexts (main,
// A, B), two real fresh fibers (A and B), message flows A -> B -> A.
//
// This exercises resume-fidelity (locals across suspension) more than slice 1:
// fiber_a_body reads `msg_for_b` after resuming from B, proving its stack
// frame survived the round trip.
namespace {
struct PingPong {
    Context main_ctx{};
    Context a_ctx{};
    Context b_ctx{};
    int msg_for_b = 7;          // A sends this to B (B reads it via user_data)
    int b_ran = 0;
    int b_observed = 0;
    int b_returned = 0;         // B writes this; A reads it after B returns
    int a_resumed_after_b = 0;
    int a_observed_return = 0;
};

static void fiber_b(Switch* resumed_by, void* user_data) {
    (void)resumed_by;
    auto* pp = static_cast<PingPong*>(user_data);
    pp->b_ran = 1;
    pp->b_observed = pp->msg_for_b;       // observe A's message
    pp->b_returned = pp->b_observed * 10; // B's return message (70)

    // Switch back to A. A's context was saved when A called context_switch into
    // us; it lives in pp->a_ctx.
    Switch s;
    s.old = &pp->b_ctx;       // B's own context (unused; B ends)
    s.new_ = &pp->a_ctx;      // resume A
    (void)context_switch(&s);
    __builtin_unreachable();  // A does not switch back into B
}

static void fiber_a(Switch* resumed_by, void* user_data) {
    (void)resumed_by;
    auto* pp = static_cast<PingPong*>(user_data);

    // Switch into B. A's context is saved into pp->a_ctx.
    Switch to_b;
    to_b.old = &pp->a_ctx;
    to_b.new_ = &pp->b_ctx;
    (void)context_switch(&to_b);
    // ^ Control returns here when B switches into &pp->a_ctx. A's locals
    //   (e.g. the implicit `pp`) must survive — proving resume fidelity.

    pp->a_resumed_after_b = 1;
    pp->a_observed_return = pp->b_returned;   // A reads B's return (70)

    // Switch back to main. Main's context lives in pp->main_ctx.
    Switch to_main;
    to_main.old = &pp->a_ctx;
    to_main.new_ = &pp->main_ctx;
    (void)context_switch(&to_main);
    __builtin_unreachable();  // main does not switch back into A
}
}  // namespace

SLUICE_TEST_CASE(fiber_context_switch_two_fiber_ping_pong) {
    if constexpr (!supported) {
        return;  // non-x86_64: skip cleanly
    }

    PingPong pp;
    ScratchStack a_stack, b_stack;
    SLUICE_CHECK(init_context(pp.a_ctx, fiber_a, &pp,
                              a_stack.base(), a_stack.size()));
    SLUICE_CHECK(init_context(pp.b_ctx, fiber_b, &pp,
                              b_stack.base(), b_stack.size()));

    // Main switches into A. Main's context is saved into pp.main_ctx.
    Switch to_a;
    to_a.old = &pp.main_ctx;
    to_a.new_ = &pp.a_ctx;
    (void)context_switch(&to_a);
    // ^ Control returns here when A switches into &pp.main_ctx.

    // The whole round trip happened: main -> A -> B -> A -> main.
    SLUICE_CHECK(pp.b_ran == 1);
    SLUICE_CHECK(pp.b_observed == 7);          // A's message reached B
    SLUICE_CHECK(pp.b_returned == 70);         // B computed the return
    SLUICE_CHECK(pp.a_resumed_after_b == 1);   // A resumed after B
    SLUICE_CHECK(pp.a_observed_return == 70);  // A read B's return (locals survived)
}

SLUICE_MAIN()
