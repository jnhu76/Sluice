// Phase B — backend-level Scheme-B race regression (review test-gap 1).
//
// The 19-step arena trace (request_lifecycle_scheme_b_test.cpp) proves the
// ARENA's step semantics; the review correctly notes it does NOT prove the
// REAL reference-backend integration — no submit thread, no Completion
// binding, no barrier pause between commit and enqueue. Through the public
// AsyncIoContext API, cancel can never interleave between commit and enqueue
// (access_mtx_ serializes all backend entry points), so this test drives the
// raw FakeAsyncBackend directly and pauses the submit path with the
// SLUICE_ASYNC_INTERNAL_TESTING seam:
//
//   - submit thread: reserve -> prepare -> binding install -> CAS -> ring
//     push -> commit, then PAUSES (pin live, op accepted, not yet enqueued);
//   - main thread observes the pause, then cancel() wins the pending terminal
//     transition (Scheme B: pending -> backend_ready(canceled));
//   - main resumes the submit thread: enqueue observes backend_ready and
//     performs its SUCCESSFUL no-op, acknowledging the pin as its final slot
//     access (submit still returns success — commit already accepted);
//   - poll() reaps the canceled Completion through the slot-bound binding.
//
// Deterministic handshake (no sleep_for, no timing assumptions): the gate's
// `paused` flag reports the exact commit-enqueue pause; the test cancels only
// after observing it and resumes explicitly.
//
// Links sluice_async_internal_testing (the pause seam is guarded by
// SLUICE_ASYNC_INTERNAL_TESTING; production sluice_async has no seam).
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

SLUICE_MAIN()

SLUICE_TEST_CASE(backend_scheme_b_cancel_wins_between_commit_and_enqueue) {
    FakeAsyncBackend backend{/*request_capacity=*/2};
    FakeAsyncBackend::SubmitPauseGate gate;
    backend.set_submit_pause_after_commit(&gate);

    std::byte buf[8]{};
    Completion<std::size_t> c;

    Result<void> submit_result;
    std::thread submitter([&] {
        submit_result = backend.submit_read(ReadOp{0, buf, 8, 0}, c);
    });

    // Wait for the submit path to pause between commit and enqueue: the op is
    // accepted (pin live), not yet enqueued. Deterministic handshake — the
    // submit thread reports the exact pause point via the gate.
    while (!gate.paused.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    SLUICE_CHECK(backend.arena_slot_in_use() == 1);
    SLUICE_CHECK(backend.outstanding() == 1);
    SLUICE_CHECK(backend.arena_enqueue_pin_live(0));  // pin live mid-submit
    SLUICE_CHECK(backend.arena_state_is(0, sluice::async::detail::RequestState::pending));

    // Cancel wins the pending terminal transition (Scheme B): the canceled
    // terminal is stored under the leaf domain; the pin stays live until the
    // resumed enqueue acknowledges it.
    backend.cancel(c);

    // Resume the submit thread: enqueue observes backend_ready -> successful
    // no-op (submit still returns success; the commit already accepted), and
    // acknowledges the pin as its final slot access.
    gate.resume.store(true, std::memory_order_release);
    submitter.join();

    SLUICE_CHECK_MSG(submit_result.has_value(),
                     "submit must still return success (commit already accepted)");
    SLUICE_CHECK(!backend.arena_enqueue_pin_live(0));  // enqueue acked the pin
    SLUICE_CHECK(backend.arena_state_is(0, sluice::async::detail::RequestState::backend_ready));
    SLUICE_CHECK(backend.outstanding() == 1);  // accepted until reap

    // poll() reaps the canceled Completion through the slot-bound publication
    // binding (inside the leaf domain).
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.arena_slot_in_use() == 1);  // bound until the reset handshake

    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}
