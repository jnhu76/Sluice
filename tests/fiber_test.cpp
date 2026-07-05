// Tests for sluice::async::Fiber state model (sluice-CORE-E1).
//
// Minimal task state machine, source-derived from Zig Uring.Fiber
// (Io/Uring.zig:149-248) and the E0 ADR §9 E4 cycle. Each case asserts ONE
// transition or invariant, TDD-vertical. No context switch yet (E2); the
// state machine is exercised directly.
#include "harness.hpp"

#include <sluice/async/cancel.hpp>
#include <sluice/async/fiber.hpp>

using namespace sluice::async;

// ---- Slice 1 (tracer): created -> runnable -> running -> done -------------
// The happy-path lifecycle: a freshly-created fiber becomes runnable, then
// running (a worker picks it up), then done (the entry returns).
SLUICE_TEST_CASE(fiber_lifecycle_created_runnable_running_done) {
    Fiber f;
    SLUICE_CHECK(f.state() == FiberState::created);

    f.make_runnable();
    SLUICE_CHECK(f.state() == FiberState::runnable);

    f.make_running();
    SLUICE_CHECK(f.state() == FiberState::running);

    f.make_done();
    SLUICE_CHECK(f.state() == FiberState::done);
}

// ---- Slice 2: done is absorbing (terminal) --------------------------------
// A finished fiber never transitions out of done — make_runnable/make_running/
// make_waiting on a done fiber are no-ops. (E4's scheduler must never resurrect
// a finished fiber.)
SLUICE_TEST_CASE(fiber_done_is_absorbing) {
    Fiber f;
    f.make_runnable();
    f.make_running();
    f.make_done();
    SLUICE_CHECK(f.state() == FiberState::done);

    f.make_runnable();   // must NOT resurrect
    SLUICE_CHECK(f.state() == FiberState::done);
    f.make_running();
    SLUICE_CHECK(f.state() == FiberState::done);
    f.make_waiting();
    SLUICE_CHECK(f.state() == FiberState::done);
}

// ---- Slice 3: waiting -> runnable (the wakeup half of the E4 cycle) -------
// A running fiber suspends (waiting); a completion handler wakes it (runnable).
// This is the runnable<->waiting cycle that makes suspension reversible.
SLUICE_TEST_CASE(fiber_waiting_to_runnable_wakeup) {
    Fiber f;
    f.make_runnable();
    f.make_running();
    f.make_waiting();
    SLUICE_CHECK(f.state() == FiberState::waiting);

    f.make_runnable();   // woken by a completion handler (E4)
    SLUICE_CHECK(f.state() == FiberState::runnable);
}

// ---- Slice 4: entry + cancel state compose --------------------------------
// A Fiber carries an entry function and its own CancelToken/CancelState (027).
// The entry is invoked by the worker (E4); cancel flows through the token.
SLUICE_TEST_CASE(fiber_carries_entry_and_cancel_state) {
    Fiber f{[](Fiber& self) {
        // A trivial task body: observe cancellation, then finish.
        (void)self.cancel_state();
        self.make_done();
    }};
    SLUICE_CHECK(f.entry());
    SLUICE_CHECK(f.state() == FiberState::created);

    // The cancel token is observable and requestable.
    f.cancel_token().request();
    SLUICE_CHECK(f.cancel_token().is_requested());

    // Run the entry directly (no scheduler yet — E4 will drive it).
    f.make_runnable();
    f.make_running();
    f.entry()(*&f);  // invokes the body, which calls make_done
    SLUICE_CHECK(f.state() == FiberState::done);
}

SLUICE_MAIN()
