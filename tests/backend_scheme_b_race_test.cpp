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
#include <sluice/measurement.hpp>
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

// ---- C2b row 5 (Fake): canceled_ops tallies ONLY on a confirmed canceled ----
// terminal win (terminal_won). A terminal loser (complete_* after cancel won,
// or cancel after an ordinary winner) and a late cancel after the terminal
// never tally; cancel of an unbound Completion resolves nothing. The Fake
// never enters `running`, so intent_recorded is dormant here by honest profile
// (running-cancel accounting is proven on the ThreadPoolBackend).
SLUICE_TEST_CASE(fake_cancel_disposition_counts_exactly_once) {
    FakeAsyncBackend backend{/*request_capacity=*/2};
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    std::byte buf[8]{};

    // 1. enqueued cancel WINS the terminal -> exactly one canceled_ops.
    Completion<std::size_t> c1;
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c1).has_value());
    SLUICE_CHECK(backend.arena_state_is(0, detail::RequestState::enqueued));
    backend.cancel(c1);  // terminal_won
    SLUICE_CHECK(stats.canceled_ops == 1);
    // A complete_* AFTER cancel won is a terminal LOSER: no overwrite, no tally.
    backend.complete_oldest_with_bytes(8);
    SLUICE_CHECK(stats.canceled_ops == 1);
    // Publication boundary (row 8): the canceled terminal is backend_ready, but
    // the Completion is NOT ready before poll().
    SLUICE_CHECK(!c1.ready());
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c1.ready());
    SLUICE_CHECK(!c1.result().has_value());
    SLUICE_CHECK(c1.result().error().code == IoError::Code::canceled);
    // Late cancel after the terminal: already_terminal -> no second tally.
    backend.cancel(c1);
    SLUICE_CHECK(stats.canceled_ops == 1);
    c1.reset();

    // 2. ordinary error wins first; cancel is the LOSER -> completion_errors
    //    tallied once, canceled_ops never.
    Completion<std::size_t> c2;
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c2).has_value());
    backend.complete_oldest_with_error(IoError{IoError::Code::backend_error});
    SLUICE_CHECK(stats.completion_errors == 1);
    backend.cancel(c2);  // already_terminal: loser, no tally
    SLUICE_CHECK(stats.canceled_ops == 1);        // unchanged
    SLUICE_CHECK(stats.completion_errors == 1);   // unchanged
    SLUICE_CHECK(!c2.ready());                    // poll gates publication (row 8)
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c2.ready());
    SLUICE_CHECK(c2.result().error().code == IoError::Code::backend_error);
    c2.reset();

    // 3. cancel of an UNBOUND Completion resolves nothing -> no tally.
    Completion<std::size_t> c3;
    backend.cancel(c3);
    SLUICE_CHECK(stats.canceled_ops == 1);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- C2b row 8 (Fake): binding identity A->A B->B + publication boundary ---
// Each terminal publishes to ITS OWN slot-bound Completion even when the
// terminal-winner order differs from the submit order — no queue-head guessing,
// no op-kind guessing, no side-band pointer FIFO. Swapped/mis-bound bindings
// would deliver B's canceled result to A and A's byte count to B (the C2b
// validity fixture proves that mutant goes RED). The case also pins the
// publication boundary: complete_*/cancel only produce backend_ready; the
// Completions are NOT ready until poll()/wait_one() reaps, and a second poll
// returns 0 (exactly-one publication, row 7).
SLUICE_TEST_CASE(fake_binding_identity_and_publication_boundary) {
    FakeAsyncBackend backend{/*request_capacity=*/2};
    std::byte buf[8]{};
    Completion<std::size_t> ca;
    Completion<std::size_t> cb;
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, ca).has_value());
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, cb).has_value());

    // B wins the terminal FIRST (terminal-winner order != submit order).
    backend.cancel(cb);                      // B: canceled terminal
    backend.complete_oldest_with_bytes(4);   // A: ordinary success terminal

    // Publication boundary: both terminals are backend_ready, neither
    // Completion is ready before poll().
    SLUICE_CHECK(!ca.ready());
    SLUICE_CHECK(!cb.ready());
    SLUICE_CHECK(backend.poll() == 2);

    // Binding identity: A's ordinary result lands on ca; B's canceled terminal
    // lands on cb — each through its own slot binding.
    SLUICE_CHECK(ca.ready());
    SLUICE_CHECK(ca.result().has_value());
    SLUICE_CHECK(ca.result().value() == 4);
    SLUICE_CHECK(cb.ready());
    SLUICE_CHECK(!cb.result().has_value());
    SLUICE_CHECK(cb.result().error().code == IoError::Code::canceled);

    // Exactly-one publication: a second poll publishes nothing.
    SLUICE_CHECK(backend.poll() == 0);

    ca.reset();
    cb.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- C2b row 4 (Fake integration): stale-generation events are harmless -----
// After a slot is released (Completion reset) and reused by a NEW request on
// the SAME physical slot, stale-generation cancel attempts cannot act on the
// new occupant: the pointer-keyed resolution of a released binding fails, and
// the new request's Completion, result, and counters stay exactly intact. The
// Fake's only stale-event entry is the pointer-keyed cancel (its complete_*
// helpers key on the CURRENT enqueued key), so the attempts go through it.
SLUICE_TEST_CASE(fake_stale_generation_event_harmless) {
    FakeAsyncBackend backend{/*request_capacity=*/1};
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    std::byte buf[8]{};
    Completion<std::size_t> c;

    // Generation N: full lifecycle to ready + reset (slot released, gen++).
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    backend.complete_oldest_with_bytes(3);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.result().value() == 3);
    // Late cancel while completion_ready (still bound): already_terminal.
    backend.cancel(c);
    SLUICE_CHECK(stats.canceled_ops == 0);
    c.reset();  // release handshake: slot freed, generation advances

    // Stale attempt: cancel after release — the pointer no longer resolves to
    // any slot binding, so nothing happens.
    backend.cancel(c);
    SLUICE_CHECK(stats.canceled_ops == 0);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);

    // The same physical slot is reused by a NEW request (generation N+1).
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    SLUICE_CHECK(backend.arena_slot_in_use() == 1);
    backend.complete_oldest_with_bytes(7);
    SLUICE_CHECK(!c.ready());  // publication gated by poll (row 8)
    SLUICE_CHECK(backend.poll() == 1);
    // The new occupant completes with ITS OWN result; the stale attempt left
    // no residue in the result or the counters.
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 7);
    SLUICE_CHECK(stats.canceled_ops == 0);
    SLUICE_CHECK(stats.completion_errors == 0);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}
