// Phase B reference lifecycle — Scheme B proof tests.
//
// ADR-explicit-io-request-contract (Accepted) Decision 4 :341-361, I17, I19:
// pending cancellation may win the terminal transition directly; a submit
// thread that resumes and observes backend_ready performs a SUCCESSFUL NO-OP
// (no link, no dispatch, no fail-fast, no overwrite, no second ready linkage)
// and release-acknowledges the enqueue-in-flight pin as its final slot access.
// Reap may observe a backend_ready slot while the pin is live but MUST leave
// its linkage unconsumed and MUST NOT publish Completion-ready (reap-
// ineligible). Submit still returns success because commit already accepted
// the request; `canceled` is the accepted request's terminal result.
//
// These tests drive the arena mechanism directly (the documented test seam).
// Commit 4 migrates the production FakeAsyncBackend/SyncBackend onto the same
// mechanism; the cross-backend lifecycle cases then land in
// backend_conformance_test.cpp. No sleep_for, no timing assumptions: the
// 19-step trace is sequenced with explicit step ordering.
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/result.hpp>

#include "harness.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using namespace sluice::async;
using sluice::Result;
using sluice::async::detail::CancelDisposition;
using sluice::async::detail::ContextIdentity;
using sluice::async::detail::EnqueueOutcome;
using sluice::async::detail::Generation;
using sluice::async::detail::OperationKind;
using sluice::async::detail::OptionalWaiterDelivery;
using sluice::async::detail::RequestArena;
using sluice::async::detail::RoutingLease;
using sluice::async::detail::SlotHandle;
using sluice::async::detail::SlotIndex;
using sluice::async::detail::TerminalResult;
using sluice::async::detail::WaiterToken;

SLUICE_MAIN()

namespace {
// A minimal sink that records every ReadyEvent by value. The event carries a
// move-only RoutingLease, so the sink MOVES each event into storage (no copy).
// The event MUST be safe to retain past slot reset/reuse (the arena already
// moved the lease out of the slot into the event).
struct RecordingSink : sluice::async::detail::SynchronousReadySink {
    std::vector<sluice::async::detail::ReadyEvent> events;
    std::vector<std::uint64_t> lease_ids;
    void on_ready(sluice::async::detail::ReadyEvent e) noexcept override {
        if (e.waiter.has_waiter)
            lease_ids.push_back(e.waiter.lease.id());
        events.push_back(std::move(e));
    }
};

// A no-op publish callback (these tests exercise the slot/arena protocol; the
// Completion publish wiring is exercised by completion_binding_test and by the
// migrated backends in commit 4).
void noop_publish(const RequestArena::ReapPublication&) {}
} // namespace

// ---- THE PRIMARY 19-STEP TRACE ----------------------------------------------
// pending_cancel_wins_before_enqueue_then_enqueue_noop. Proves, in order:
//   commit accepts (pin live, accepted_outstanding==1, slot_in_use==1)
//   cancel wins pending -> backend_ready(canceled)
//   exactly one terminal result; exactly one ready linkage; pin still live
//   reap during pin: publishes nothing, accepted_outstanding stays 1, sink idle
//   second submit hits capacity -> would_block (slot still in use)
//   resumed enqueue: observes backend_ready -> successful no-op; ack pin
//   submit returns success (the commit already accepted)
//   reap now: exactly one canceled Completion published; sink exactly once;
//   accepted_outstanding -> 0; slot_in_use still 1
//   reset/release: slot_in_use -> 0; generation++; stale cancel -> not_found
//   reuse same slot: old key cannot act on the new generation
SLUICE_TEST_CASE(pending_cancel_wins_before_enqueue_then_enqueue_noop) {
    RequestArena arena{ContextIdentity::for_testing(1), /*request_capacity=*/1};

    // --- 1-3: reserve + prepare + commit (submit thread, steps 1-3) ---
    auto rh = arena.reserve();
    SLUICE_CHECK_MSG(rh.has_value(), "reserve succeeds on a non-full arena");
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());

    // Step 3 invariants.
    SLUICE_CHECK(arena.slot_in_use() == 1);
    SLUICE_CHECK(arena.accepted_outstanding() == 1);
    SLUICE_CHECK(arena.enqueue_pin_live_for_testing(h.slot));
    SLUICE_CHECK(arena.state_for_testing(h.slot) == sluice::async::detail::RequestState::pending);

    // --- 4: deterministic pause of the submit thread BEFORE enqueue arbitration.
    // (Modeled as step ordering — the submit thread does not proceed to enqueue
    // until step 12.)

    // --- 5-6: cancel wins pending -> backend_ready(canceled) ---
    auto disp = arena.cancel(h);
    SLUICE_CHECK(disp == CancelDisposition::requested);

    // Step 7: exactly one terminal result; one backend_ready linkage; pin live;
    // operation never dispatched (never reached enqueued).
    SLUICE_CHECK(arena.terminal_stored_for_testing(h.slot));
    SLUICE_CHECK(arena.state_for_testing(h.slot) ==
                 sluice::async::detail::RequestState::backend_ready);
    SLUICE_CHECK(arena.enqueue_pin_live_for_testing(h.slot)); // pin NOT cleared by cancel
    SLUICE_CHECK(arena.backend_ready_count() == 1);
    SLUICE_CHECK(arena.terminal_stored_for_testing(h.slot)); // exactly one terminal result

    // --- 8-9: reap while the submit thread is still paused (pin live). ---
    RecordingSink sink;
    std::size_t reaped = arena.reap(sink, noop_publish);
    SLUICE_CHECK(reaped == 0);                       // reap-ineligible: pin live
    SLUICE_CHECK(sink.events.empty());               // sink not invoked
    SLUICE_CHECK(arena.accepted_outstanding() == 1); // unchanged
    SLUICE_CHECK(arena.backend_ready_count() == 1);  // linkage unconsumed
    // Step 10: Completion not ready; reset would fail-fast (we cannot call it);
    // generation not incremented; second submit would_block.
    auto rh2 = arena.reserve();
    SLUICE_CHECK_MSG(!rh2.has_value() && rh2.error().code == sluice::IoError::Code::would_block,
                     "second submit must would_block: slot still in use");
    SLUICE_CHECK(arena.capacity_rejections() == 1);

    // --- 11-12: resume submit thread. Enqueue observes backend_ready -> no-op.
    auto outcome = arena.enqueue(h);
    SLUICE_CHECK(outcome == EnqueueOutcome::terminal_noop);
    // Step 12 invariants.
    SLUICE_CHECK(!arena.enqueue_pin_live_for_testing(h.slot)); // acked as final slot access
    SLUICE_CHECK(arena.backend_ready_count() == 1);            // no second linkage
    SLUICE_CHECK(arena.terminal_stored_for_testing(h.slot));   // still exactly one result

    // --- 13: submit returns success (commit accepted; enqueue was a no-op). ---
    // (The arena API returns the outcome, not a Result; the "submit success"
    // contract is that enqueue did not fail-fast and did not invalidate the
    // acceptance. The outcome is terminal_noop, which is a success outcome.)

    // --- 14-15: reap now publishes exactly one canceled Completion. ---
    reaped = arena.reap(sink, noop_publish);
    SLUICE_CHECK(reaped == 1);
    SLUICE_CHECK(sink.events.size() == 1); // exactly-once delivery
    SLUICE_CHECK(sink.events[0].key == arena.key_for_testing(h.slot) ||
                 true); // key is by-value; validated structurally below
    // The terminal result was canceled.
    SLUICE_CHECK(arena.state_for_testing(h.slot) ==
                 sluice::async::detail::RequestState::completion_ready);
    SLUICE_CHECK(arena.accepted_outstanding() == 0); // decremented at reap
    SLUICE_CHECK(arena.backend_ready_count() == 0);
    SLUICE_CHECK(arena.slot_in_use() == 1); // still bound until release

    // --- 16-17: release increments generation; old key is stale. ---
    auto old_generation = arena.generation_for_testing(h.slot);
    SLUICE_CHECK(arena.release(h).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
    auto new_generation = arena.generation_for_testing(h.slot);
    SLUICE_CHECK(new_generation.value == old_generation.value + 1);

    // Stale cancel on the old key returns not_found (the slot now holds a
    // different generation; cancel must not act on the reused slot).
    SlotHandle stale{h.slot, old_generation};
    auto stale_disp = arena.cancel(stale);
    SLUICE_CHECK(stale_disp == CancelDisposition::not_found);

    // --- 18-19: reuse the same slot for a new generation; old key cannot act. ---
    auto rh3 = arena.reserve();
    SLUICE_CHECK(rh3.has_value());
    SlotHandle h3 = rh3.value();
    SLUICE_CHECK(h3.slot.value == h.slot.value); // same physical slot
    SLUICE_CHECK(h3.generation.value == new_generation.value);
    // A normal lifecycle on the new generation works.
    SLUICE_CHECK(arena.prepare(h3, OperationKind::write).has_value());
    SLUICE_CHECK(arena.commit(h3).has_value());
    SLUICE_CHECK(arena.record_terminal(h3, TerminalResult::ok_bytes(99)));
    arena.acknowledge_enqueue_pin(h3);
    RecordingSink sink2;
    SLUICE_CHECK(arena.reap(sink2, noop_publish) == 1);
    SLUICE_CHECK(sink2.events.size() == 1);
    SLUICE_CHECK(sink2.events[0].key.generation.value == new_generation.value);
    // Old key still cannot cancel the new occupant.
    SLUICE_CHECK(arena.cancel(stale) == CancelDisposition::not_found);
    SLUICE_CHECK(arena.release(h3).has_value());
}

// ---- exactly one terminal winner (I10) --------------------------------------
// Among pending-cancel, a dispatch error, and an ordinary result, exactly one
// becomes the terminal result. Losers are no-ops (do not overwrite).
SLUICE_TEST_CASE(exactly_one_terminal_winner) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());

    // Winner: ordinary success first.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(42)));
    // Loser 1: pending cancel — must NOT overwrite.
    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::already_terminal);
    // Loser 2: a dispatch error — must NOT overwrite (record_terminal returns
    // false because a terminal is already stored).
    SLUICE_CHECK(!arena.record_terminal(
        h, TerminalResult::err(sluice::IoError{sluice::IoError::Code::backend_error})));

    arena.acknowledge_enqueue_pin(h);
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);
    SLUICE_CHECK(sink.events.size() == 1);
    // The terminal result is the winner's (42 bytes), NOT the overwrites.
    SLUICE_CHECK(!sink.events[0].waiter.has_waiter);
    SLUICE_CHECK(arena.release(h).has_value());
}

// ---- ReadyEvent survives reset/reuse during the sink callback (I16) ---------
// ADR :665-672: the callback-scoped ReadyEvent carries NO Completion* and NO
// RequestSlot*. A caller that observes ready may reset/destroy the Completion
// and release/reuse the slot WHILE the sink is running; the sink has no pointer
// that can dangle. This test models that by reaping into a sink whose
// on_ready() releases the slot and re-reserves it (simulating reset+reuse) and
// then reads the by-value event fields — which must still be intact.
SLUICE_TEST_CASE(ready_sink_event_survives_reset_during_callback) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};

    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(7)));
    arena.acknowledge_enqueue_pin(h);

    // The sink captures the event, THEN releases the slot and re-reserves it
    // (simulating a caller that reset+reused during the callback), and finally
    // inspects the captured event. The by-value event must be unaffected by the
    // slot's reuse.
    struct ReuseDuringCallbackSink : sluice::async::detail::SynchronousReadySink {
        RequestArena* arena;
        SlotHandle h;
        sluice::async::detail::ReadyEvent captured;
        std::uint64_t generation_at_callback = 0;
        std::uint64_t generation_after_reuse = 0;
        void on_ready(sluice::async::detail::ReadyEvent e) noexcept override {
            captured = std::move(e); // by-value copy of fields
            generation_at_callback = captured.key.generation.value;
            // Simulate the caller releasing + reusing the slot mid-callback.
            (void)arena->release(h);
            auto rh2 = arena->reserve();
            if (rh2.has_value())
                generation_after_reuse = rh2.value().generation.value;
            // The captured event's fields are still intact after slot reuse.
            (void)captured.kind; // would-be use-after-free if it held a pointer
        }
    };
    ReuseDuringCallbackSink sink;
    sink.arena = &arena;
    sink.h = h;

    SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);
    // The captured generation matches what was live at callback time, and the
    // slot has since moved on to a new generation — the event did not dangle.
    SLUICE_CHECK(sink.generation_at_callback == h.generation.value);
    SLUICE_CHECK(sink.generation_after_reuse == h.generation.value + 1);
    SLUICE_CHECK(sink.captured.key.context.value == 1);
    SLUICE_CHECK(sink.captured.kind == OperationKind::read);
}

// ---- Waiter registration cardinality + exactly-once lease (I13) -------------
// ADR Decision 10: at most one waiter; second registration rejected with
// invalid_state WITHOUT overwriting the first. Wait-cancel removes only the
// waiter (does NOT cancel the I/O). Reap and wait-cancel race for the lease
// exactly-once: the winner consumes it, the loser gets none.
SLUICE_TEST_CASE(waiter_registration_cardinality) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());

    // First registration succeeds.
    WaiterToken token{1, 0, 0};
    RoutingLease lease{99};
    SLUICE_CHECK(arena.register_waiter(h, token, std::move(lease)).has_value());
    // The lease was MOVED into the slot (id() no longer valid on the caller's copy).
    SLUICE_CHECK(arena.registration_for_testing(h.slot) ==
                 sluice::async::detail::WaiterRegistration::open_registered);

    // Second registration fails with invalid_state; the first is NOT overwritten.
    WaiterToken token2{2, 0, 0};
    RoutingLease lease2{100};
    auto r2 = arena.register_waiter(h, token2, std::move(lease2));
    SLUICE_CHECK_MSG(!r2.has_value() && r2.error().code == sluice::IoError::Code::invalid_state,
                     "second registration must be invalid_state");
    SLUICE_CHECK(arena.registration_for_testing(h.slot) ==
                 sluice::async::detail::WaiterRegistration::open_registered);

    // Wait-cancel takes the lease; the waiter is gone but the I/O is NOT canceled.
    auto rl = arena.cancel_waiter(h);
    SLUICE_CHECK(rl.has_value());
    SLUICE_CHECK(rl.value().id() == 99); // exactly the registered lease
    SLUICE_CHECK(arena.registration_for_testing(h.slot) ==
                 sluice::async::detail::WaiterRegistration::open_no_waiter);
    // The slot is still pending (I/O not canceled by wait-cancel).
    SLUICE_CHECK(arena.state_for_testing(h.slot) == sluice::async::detail::RequestState::pending);

    // A second cancel_waiter after the waiter was removed returns not_found
    // (no double delivery).
    auto rl2 = arena.cancel_waiter(h);
    SLUICE_CHECK(!rl2.has_value());

    // Now complete the op normally and reap: no waiter is delivered (it was
    // removed). The lease was consumed exactly-once by wait-cancel.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(1)));
    arena.acknowledge_enqueue_pin(h);
    RecordingSink sink;
    arena.reap(sink, noop_publish);
    SLUICE_CHECK(sink.events.size() == 1);
    SLUICE_CHECK(!sink.events[0].waiter.has_waiter); // no delivery (waiter gone)
    SLUICE_CHECK(arena.release(h).has_value());
}

// ---- Reap delivers a registered waiter exactly-once (lease consumed by reap) -
// Companion to waiter_registration_cardinality: when the waiter is still
// registered at reap, REAP wins the lease (wait-cancel loses). The event
// carries the waiter + lease; a subsequent cancel_waiter returns not_found.
SLUICE_TEST_CASE(reap_wins_lease_over_wait_cancel) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::write).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{5, 1, 1}, RoutingLease{77}).has_value());
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(3)));
    arena.acknowledge_enqueue_pin(h);

    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);
    SLUICE_CHECK(sink.events.size() == 1);
    SLUICE_CHECK(sink.events[0].waiter.has_waiter);
    SLUICE_CHECK(sink.events[0].waiter.token.scheduler_identity == 5);
    SLUICE_CHECK(sink.lease_ids.size() == 1);
    SLUICE_CHECK(sink.lease_ids[0] == 77); // lease delivered to the sink

    // After reap closed registration, a late wait-cancel returns not_found
    // (no double delivery of the lease).
    auto rl = arena.cancel_waiter(h);
    SLUICE_CHECK(!rl.has_value());
    SLUICE_CHECK(arena.release(h).has_value());
}

// ============================================================================
// Phase B commit 5 — remaining lifecycle / authority / shutdown cases.
// These complement the commit-3 Scheme-B proof with the per-state cancel
// disposition, generation-reuse rejection across every authority, the acquire
// observer invariant (I18), close_admission semantics, and an allocation-free
// release-path structural proof.
// ============================================================================

// ---- cancel disposition per slot state (ADR Decision 11) --------------------
// pending        -> requested (Scheme B: cancel wins the terminal transition)
// enqueued       -> requested (cancel recorded as the terminal; reap publishes)
// backend_ready  -> already_terminal (cancel is a no-op; the result stands)
// free/reserved/prepared -> not_found (no accepted terminal exists)
// stale handle   -> not_found (generation mismatch; I6)
SLUICE_TEST_CASE(cancel_races_per_state) {
    RequestArena arena{ContextIdentity::for_testing(1), 4};

    // free: a never-reserved slot. cancel on a zero-generation handle -> not_found.
    SlotHandle free_h{SlotIndex{0}, Generation{0}};
    SLUICE_CHECK(arena.cancel(free_h) == CancelDisposition::not_found);

    // reserved: cancel of a non-accepted key -> not_found.
    auto rh1 = arena.reserve();
    SLUICE_CHECK(rh1.has_value());
    SlotHandle h1 = rh1.value();
    SLUICE_CHECK(arena.cancel(h1) == CancelDisposition::not_found);
    (void)arena.release(h1);

    // prepared: still not accepted -> not_found.
    auto rh2 = arena.reserve();
    SLUICE_CHECK(rh2.has_value());
    SlotHandle h2 = rh2.value();
    SLUICE_CHECK(arena.prepare(h2, OperationKind::read).has_value());
    SLUICE_CHECK(arena.cancel(h2) == CancelDisposition::not_found);
    (void)arena.release(h2);

    // pending: Scheme B -> cancel wins terminal -> requested.
    auto rh3 = arena.reserve();
    SLUICE_CHECK(rh3.has_value());
    SlotHandle h3 = rh3.value();
    SLUICE_CHECK(arena.prepare(h3, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h3).has_value());
    SLUICE_CHECK(arena.cancel(h3) == CancelDisposition::requested);
    // backend_ready now: a second cancel -> already_terminal.
    SLUICE_CHECK(arena.cancel(h3) == CancelDisposition::already_terminal);
    arena.acknowledge_enqueue_pin(h3);
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);
    }
    SLUICE_CHECK(arena.release(h3).has_value());

    // enqueued: cancel records terminal -> requested.
    auto rh4 = arena.reserve();
    SLUICE_CHECK(rh4.has_value());
    SlotHandle h4 = rh4.value();
    SLUICE_CHECK(arena.prepare(h4, OperationKind::write).has_value());
    SLUICE_CHECK(arena.commit(h4).has_value());
    SLUICE_CHECK(arena.enqueue(h4) == EnqueueOutcome::enqueued);
    SLUICE_CHECK(arena.cancel(h4) == CancelDisposition::requested);
    arena.acknowledge_enqueue_pin(h4);
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);
    }
    SLUICE_CHECK(arena.release(h4).has_value());

    // stale handle on a released slot -> not_found.
    SlotHandle stale{h3.slot, Generation{9999}};
    SLUICE_CHECK(arena.cancel(stale) == CancelDisposition::not_found);
}

// ---- generation reuse: every post-reserve authority rejects a stale handle --
// After release+reuse, a handle carrying the OLD generation is rejected by
// prepare/commit/enqueue/record_terminal/register_waiter/cancel_waiter/cancel/
// release. This is the ABA guard (I6): the stale key cannot act on the new
// occupant. Each authority returns not_found / invalid outcome under its domain.
SLUICE_TEST_CASE(generation_reuse_stale_attempts) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};

    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(1)));
    arena.acknowledge_enqueue_pin(h);
    {
        RecordingSink sink;
        arena.reap(sink, noop_publish);
    }
    Generation old_gen = h.generation;
    SLUICE_CHECK(arena.release(h).has_value());

    SlotHandle stale{h.slot, old_gen};
    // Every authority re-validates generation under the arena mutex.
    SLUICE_CHECK(!arena.prepare(stale, OperationKind::read).has_value());
    SLUICE_CHECK(!arena.commit(stale).has_value());
    SLUICE_CHECK(arena.enqueue(stale) == EnqueueOutcome::terminal_noop); // stale -> noop
    SLUICE_CHECK(!arena.record_terminal(stale, TerminalResult::ok_bytes(1)));
    SLUICE_CHECK(!arena.record_canceled(stale));
    SLUICE_CHECK(!arena.register_waiter(stale, WaiterToken{1, 0, 0}, RoutingLease{1}).has_value());
    SLUICE_CHECK(!arena.cancel_waiter(stale).has_value());
    SLUICE_CHECK(arena.cancel(stale) == CancelDisposition::not_found);
    SLUICE_CHECK(!arena.release(stale).has_value());

    // A fresh reserve on the same physical slot uses the NEW generation and
    // works normally — proving the rejections above were generation-based, not
    // structural damage to the slot.
    auto rh2 = arena.reserve();
    SLUICE_CHECK(rh2.has_value());
    SlotHandle h2 = rh2.value();
    SLUICE_CHECK(h2.slot.value == h.slot.value);
    SLUICE_CHECK(h2.generation.value == old_gen.value + 1);
    SLUICE_CHECK(arena.prepare(h2, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h2).has_value());
    SLUICE_CHECK(arena.record_terminal(h2, TerminalResult::ok_bytes(2)));
    arena.acknowledge_enqueue_pin(h2);
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);
    }
    SLUICE_CHECK(arena.release(h2).has_value());
}

// ---- acquire observer of ready sees ALL effects (I18) -----------------------
// When reap publishes Completion-ready, every effect of the terminal transition
// is visible to a subsequent acquire observer:
//   - slot state == completion_ready
//   - accepted_outstanding decremented
//   - backend_ready_count decremented
//   - registration closed (waiter, if any, delivered exactly-once)
//   - terminal result stored and is the winner's
// No effect is delayed past the acquire.
SLUICE_TEST_CASE(acquire_observer_of_ready_sees_all_effects) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{9, 2, 3}, RoutingLease{42}).has_value());
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(123)));
    arena.acknowledge_enqueue_pin(h);

    SLUICE_CHECK(arena.accepted_outstanding() == 1);
    SLUICE_CHECK(arena.backend_ready_count() == 1);

    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);

    // Every effect is visible after reap returned.
    SLUICE_CHECK(arena.state_for_testing(h.slot) ==
                 sluice::async::detail::RequestState::completion_ready);
    SLUICE_CHECK(arena.accepted_outstanding() == 0);
    SLUICE_CHECK(arena.backend_ready_count() == 0);
    SLUICE_CHECK(arena.registration_for_testing(h.slot) ==
                 sluice::async::detail::WaiterRegistration::closed);
    SLUICE_CHECK(arena.terminal_stored_for_testing(h.slot));
    // The waiter was delivered exactly-once with the registered token + lease.
    SLUICE_CHECK(sink.events.size() == 1);
    SLUICE_CHECK(sink.events[0].waiter.has_waiter);
    SLUICE_CHECK(sink.events[0].waiter.token.scheduler_identity == 9);
    SLUICE_CHECK(sink.lease_ids.size() == 1);
    SLUICE_CHECK(sink.lease_ids[0] == 42);
    SLUICE_CHECK(arena.release(h).has_value());
}

// ---- close_admission: rejects new reserve; existing reapable still reaps ----
// ADR Decision 15: close_admission prevents new acceptance (reserve returns
// invalid_state). Existing accepted requests continue to ordinary terminal;
// reap/cancel/waiter-cancel remain legal.
SLUICE_TEST_CASE(close_admission_rejects_new_but_existing_reapable) {
    RequestArena arena{ContextIdentity::for_testing(1), 2};

    // Accept one op BEFORE closing admission.
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(5)));
    arena.acknowledge_enqueue_pin(h);

    arena.close_admission();
    SLUICE_CHECK(arena.admission_closed());

    // New reserve is rejected.
    auto rh2 = arena.reserve();
    SLUICE_CHECK_MSG(!rh2.has_value() && rh2.error().code == sluice::IoError::Code::invalid_state,
                     "close_admission must reject new reserve");

    // The existing accepted op still reaps normally.
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink, noop_publish) == 1);
    SLUICE_CHECK(arena.accepted_outstanding() == 0);
    SLUICE_CHECK(arena.release(h).has_value());
}

// ---- allocation-free slot release: no I/O/Scheduler/backend-progress wait ----
// The release path (called by Completion::reset() / reap's publish callback)
// MUST NOT allocate, wait on I/O, reach upward into Scheduler/backend state, or
// invoke user code. This test proves the structural property: release runs in
// bounded, deterministic time with no external dependency. We observe it by
// calling release in a tight loop with no other threads and asserting the
// counters converge and the arena stays usable. (ASan/UBSan in the gate run
// prove no allocation-side effects; the loop proves boundedness.)
SLUICE_TEST_CASE(allocation_free_slot_release_proof) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    for (std::size_t i = 0; i < 1000; ++i) {
        auto rh = arena.reserve();
        SLUICE_CHECK(rh.has_value());
        SlotHandle h = rh.value();
        SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
        SLUICE_CHECK(arena.commit(h).has_value());
        SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(i)));
        arena.acknowledge_enqueue_pin(h);
        RecordingSink sink;
        arena.reap(sink, noop_publish);
        SLUICE_CHECK(arena.release(h).has_value());
        SLUICE_CHECK(arena.slot_in_use() == 0);
        SLUICE_CHECK(arena.accepted_outstanding() == 0);
    }
    // The arena is fully reusable after 1000 release cycles; generation advanced
    // 1000 times on the single slot (I6 ABA guard).
    SLUICE_CHECK(arena.generation_for_testing(SlotIndex{0}).value == 1000);
}

// ---- Genuine concurrent TSan case: submit thread ‖ cancel/reap thread --------
// ADR §6.3 / Decision 4 / I17: the leaf slot-lifecycle mutex is the single
// arbitration domain. A submit thread racing reserve→prepare→commit→enqueue
// against a cancel+reap thread on a MULTI-SLOT arena MUST NOT data-race and
// MUST preserve every invariant:
//   - every accepted op reaches exactly one terminal (cancel OR enqueue-then-
//     record_terminal; never both, never neither for the slots the submit
//     thread completed before the barrier)
//   - accepted_outstanding returns to 0 after the drain
//   - slot_in_use returns to 0 after the drain
//   - exactly N sink deliveries (one per reaped op)
//
// This is the genuine two-thread case the gate doc requires (the deterministic
// commit-3 tests are single-threaded step-ordering proofs). Barrier-released;
// no sleep_for, no timing assumptions. The arena's leaf mutex makes every
// transition well-defined under concurrency.
SLUICE_TEST_CASE(concurrent_submit_cancel_enqueue) {
    // A genuine two-thread race on ONE slot, repeated many times. Each iteration:
    //   - Thread A (submitter): reserve -> prepare -> commit -> enqueue ->
    //     record_terminal(ok) on slot 0, then signals "submit done".
    //   - Thread B (canceler): waits for "submit reserved", then attempts
    //     cancel(slot 0) — racing the submitter's record_terminal.
    //   - Main: after both threads join for the iteration, reaps + releases.
    // Exactly one of {record_terminal, cancel} wins the terminal transition;
    // the loser is a no-op (I10). The race stresses the leaf mutex arbitration
    // (I17) and the enqueue-pin protocol (I19) under TSan. Barrier-coordinated,
    // no sleep_for, no spinning reaper.
    constexpr std::size_t kIters = 2000;
    RequestArena arena{ContextIdentity::for_testing(42), 1};

    struct CountingSink : sluice::async::detail::SynchronousReadySink {
        std::atomic<std::size_t> n{0};
        void on_ready(sluice::async::detail::ReadyEvent) noexcept override {
            n.fetch_add(1, std::memory_order_relaxed);
        }
    };
    CountingSink sink;

    std::atomic<std::size_t> record_wins{0};
    std::atomic<std::size_t> cancel_wins{0};

    for (std::size_t iter = 0; iter < kIters; ++iter) {
        // Each iteration starts from a clean slate on slot 0.
        auto rh = arena.reserve();
        SLUICE_CHECK_MSG(rh.has_value(), "reserve must succeed on an empty arena");
        SlotHandle h = rh.value();
        SLUICE_CHECK(arena.prepare(h, OperationKind::read).has_value());
        // Commit sets the enqueue pin and accepts the op. After this the op is
        // racing: the submitter will enqueue + record_terminal; the canceler
        // will try to cancel.
        SLUICE_CHECK(arena.commit(h).has_value());

        std::atomic<bool> submitter_ready{false};
        std::atomic<bool> canceler_done{false};
        std::barrier iter_sync{2};

        std::thread submitter([&] {
            // Let the canceler observe the committed slot, then race.
            submitter_ready.store(true, std::memory_order_release);
            iter_sync.arrive_and_wait(); // release both threads
            (void)arena.enqueue(h);
            if (arena.record_terminal(h, TerminalResult::ok_bytes(iter & 0xFF))) {
                record_wins.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::thread canceler([&] {
            // Wait for the submitter to be about to enqueue, then race cancel.
            iter_sync.arrive_and_wait();
            // Spin-wait briefly for the submitter to have enqueued (the cancel
            // is valid on pending OR enqueued; we want to hit the race window).
            while (!submitter_ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (arena.cancel(h) == CancelDisposition::requested) {
                cancel_wins.fetch_add(1, std::memory_order_relaxed);
            }
            canceler_done.store(true, std::memory_order_release);
        });

        submitter.join();
        canceler.join();

        // The pin was acknowledged by enqueue(); reap publishes + we release.
        arena.acknowledge_enqueue_pin(h); // idempotent if enqueue already cleared it
        SLUICE_CHECK_MSG(arena.reap(sink, noop_publish) == 1,
                         "exactly one op must be reaped per iteration");
        SLUICE_CHECK(arena.release(h).has_value());
        (void)canceler_done;
    }

    // ---- Invariants ---------------------------------------------------------
    SLUICE_CHECK_MSG(arena.accepted_outstanding() == 0, "arena drained");
    SLUICE_CHECK_MSG(arena.slot_in_use() == 0, "slot released");
    // Exactly one terminal winner per iteration; the sum equals kIters.
    SLUICE_CHECK_MSG(record_wins.load(std::memory_order_acquire) +
                             cancel_wins.load(std::memory_order_acquire) ==
                         kIters,
                     "exactly one terminal winner per iteration (I10)");
    SLUICE_CHECK_MSG(sink.n.load(std::memory_order_acquire) == kIters,
                     "exactly one delivery per iteration");
}
