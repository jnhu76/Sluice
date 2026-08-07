// Phase C2c — arena-level waiter / borrow / delivery-lease conformance matrix.
//
// Issue #68 rows 11-14, proven at the RequestArena authority layer:
//   row 11  — fd/buffer borrow lifetime: prepare writes metadata inactive;
//             commit is the borrow linearization point (active); the borrow
//             stays active across pending/enqueued/running/backend_ready,
//             across record_terminal, Scheme-B cancel, running cancel intent,
//             and wait-cancel; reap ends it BEFORE the Completion-ready
//             publication; rollback never borrows and clears metadata;
//             generation++ makes a stale handle unable to touch a new
//             occupant's borrow.
//   row 12a — single-waiter registration: one registration authority; a
//             second register is invalid_state AND does not overwrite the
//             first (final delivery carries token A + lease A, never B);
//             per-state registration matrix pinned from the as-built
//             contract (pending/enqueued allowed; reserved/prepared/running/
//             backend_ready/completion_ready invalid_state; stale not_found).
//   row 13  — waiter-cancel independence: cancel_waiter removes ONLY the
//             waiter; it never cancels the I/O, never picks a terminal,
//             never ends the borrow. I/O cancel removes NOTHING of the
//             waiter: a canceled terminal still delivers the registered
//             waiter at reap. (Two independent ownership domains.)
//   row 14a — delivery lease: move-only type properties; caller -> slot ->
//             ReadyEvent (or caller -> slot -> cancel_waiter return) transfer
//             chain with exactly one owner; ReadyEvent is by-value and stays
//             valid across slot release + reuse even with a waiter payload.
//
// Concurrency (row 13/14a): register_waiter vs record_terminal and
// cancel_waiter vs reap are driven with std::barrier-released threads under
// the arena's single leaf domain — only the two ADR-legal outcomes can occur,
// and the lease ownership count is exactly one in every interleaving.
//
// Links sluice_async_internal_testing: the generation-validated
// borrow_for_test / waiter_for_test observation seams are guarded by
// SLUICE_ASYNC_INTERNAL_TESTING (AGENTS.md §15; production carries nothing).
//
// All waits are bounded and every case restores slot_in_use == 0 before the
// arena destructs (fail-path discipline, issue #68 §13: an outstanding slot
// at arena destruction would fail-fast and mask the real assertion).
#include "harness.hpp"

#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
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
using sluice::async::detail::RequestState;
using sluice::async::detail::RoutingLease;
using sluice::async::detail::SlotHandle;
using sluice::async::detail::SlotIndex;
using sluice::async::detail::TerminalResult;
using sluice::async::detail::WaiterRegistration;
using sluice::async::detail::WaiterToken;

SLUICE_MAIN()

namespace {

// Records every ReadyEvent by value (the event carries a move-only lease, so
// the sink MOVES each event into storage). Heap-allocating (std::vector), so
// it is used ONLY for event-content inspection; the allocation-independent
// sink contract is proven by the ReferenceReadySink observation in the
// backend integration targets.
struct RecordingSink : sluice::async::detail::SynchronousReadySink {
    std::vector<sluice::async::detail::ReadyEvent> events;
    std::vector<std::uint64_t> lease_ids;
    void on_ready(sluice::async::detail::ReadyEvent e) noexcept override {
        if (e.waiter.has_waiter) lease_ids.push_back(e.waiter.lease.id());
        events.push_back(std::move(e));
    }
};

// No-op publication thunk + installer (every reaped slot MUST carry an
// installed binding — reap fail-fasts on a missing one). Mirrors
// request_lifecycle_scheme_b_test.cpp.
void noop_binding_publish(void*, const TerminalResult&) noexcept {}

void install_noop_binding(RequestArena& arena, SlotHandle h) {
    static int dummy_completion = 0;
    auto r = arena.install_publication_binding(h, &dummy_completion, 0,
                                               &noop_binding_publish);
    SLUICE_CHECK_MSG(r.has_value(),
                     "noop binding install must succeed on a prepared slot");
}

// One full submit-stage sequence up to (and including) enqueue: reserve ->
// prepare -> binding -> commit -> enqueue. Returns the handle. This helper is
// a test-invariant prerequisite: a failure here means the case cannot proceed,
// so it reports the failure and returns a default handle (the case then fails
// at its own assertions) instead of using SLUICE_CHECK, whose bare `return;`
// is illegal in a non-void function.
SlotHandle submit_enqueued(RequestArena& arena, OperationKind kind,
                           sluice::async::detail::BorrowMetadata borrow,
                           std::uint64_t requested_bytes = 0) {
    auto rh = arena.reserve();
    if (!rh.has_value()) {
        ::sluice_test::record_failure(__FILE__, __LINE__,
                                      "submit_enqueued: reserve failed");
        return {};
    }
    SlotHandle h = rh.value();
    if (!arena.prepare(h, kind, borrow).has_value()) {
        ::sluice_test::record_failure(__FILE__, __LINE__,
                                      "submit_enqueued: prepare failed");
        return {};
    }
    static int dummy_completion = 0;
    if (!arena.install_publication_binding(h, &dummy_completion, requested_bytes,
                                           &noop_binding_publish)
             .has_value()) {
        ::sluice_test::record_failure(__FILE__, __LINE__,
                                      "submit_enqueued: binding install failed");
        return {};
    }
    if (!arena.commit(h).has_value()) {
        ::sluice_test::record_failure(__FILE__, __LINE__,
                                      "submit_enqueued: commit failed");
        return {};
    }
    if (arena.enqueue(h) != EnqueueOutcome::enqueued) {
        ::sluice_test::record_failure(__FILE__, __LINE__,
                                      "submit_enqueued: enqueue failed");
        return {};
    }
    return h;
}

}  // namespace

// ---- Row 11: borrow lifecycle across the FULL state walk --------------------
// prepare writes fd/address/length with active==false; commit is the borrow
// linearization point (active==true); pending/enqueued/running/backend_ready
// all keep the borrow active; record_terminal does NOT end it; reap ends it
// before completion-ready; release frees the slot (and a released slot has no
// observable borrow). A stale handle cannot read (or touch) the new occupant.
SLUICE_TEST_CASE(arena_borrow_lifecycle_full_matrix) {
    RequestArena arena{ContextIdentity::for_testing(1), /*capacity=*/1};
    std::byte buf[16]{};

    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();

    // reserved: no borrow metadata yet.
    auto r0 = arena.borrow_for_test(h);
    SLUICE_CHECK(r0.has_value());
    SLUICE_CHECK(!r0->active);

    // prepare: metadata written, borrow NOT yet active (I7).
    SLUICE_CHECK(arena.prepare(h, OperationKind::read,
                               sluice::async::detail::BorrowMetadata{7, buf, 16})
                     .has_value());
    auto p = arena.borrow_for_test(h);
    SLUICE_CHECK(p.has_value());
    SLUICE_CHECK_MSG(p->fd == 7, "prepare must write the exact fd");
    SLUICE_CHECK_MSG(p->address == buf, "prepare must write the exact caller address");
    SLUICE_CHECK_MSG(p->length == 16, "prepare must write the exact length");
    SLUICE_CHECK_MSG(!p->active, "prepare must NOT begin the borrow");

    // commit: borrow becomes active — the linearization point.
    install_noop_binding(arena, h);
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::pending);
    SLUICE_CHECK(arena.borrow_for_test(h)->active);

    // pending -> enqueued: borrow persists.
    SLUICE_CHECK(arena.enqueue(h) == EnqueueOutcome::enqueued);
    SLUICE_CHECK(arena.borrow_for_test(h)->active);

    // enqueued -> running (dispatch shape): borrow persists.
    SLUICE_CHECK(arena.mark_running(h));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::running);
    SLUICE_CHECK(arena.borrow_for_test(h)->active);

    // running -> backend_ready via record_terminal: the terminal result being
    // known does NOT end the borrow (worker finishing the syscall != borrow
    // lifetime end). This is the boundary that must stay active until reap.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(16)));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::backend_ready);
    auto bt = arena.borrow_for_test(h);
    SLUICE_CHECK(bt.has_value());
    SLUICE_CHECK_MSG(bt->active, "borrow must still be active at backend_ready");

    // backend_ready -> completion_ready via reap: borrow ends (I18 — an
    // acquire observer of Completion-ready sees the ended borrow).
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::completion_ready);
    SLUICE_CHECK(!arena.borrow_for_test(h)->active);

    // release: slot free, no observable borrow.
    arena.release_completed_binding(h);
    SLUICE_CHECK(!arena.borrow_for_test(h).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 11: borrow survives every cancel/wait-cancel path ------------------
// Scheme-B pending cancel, running cancel intent, and wait-cancel must never
// end the borrow; only reap does.
SLUICE_TEST_CASE(arena_borrow_survives_cancel_and_wait_cancel) {
    // Path 1: Scheme-B pending cancel wins -> backend_ready(canceled), pin
    // live; enqueue no-op acks the pin; borrow stays active through all of it.
    {
        RequestArena arena{ContextIdentity::for_testing(1), 1};
        std::byte buf[8]{};
        auto rh = arena.reserve();
        SLUICE_CHECK(rh.has_value());
        SlotHandle h = rh.value();
        SLUICE_CHECK(arena.prepare(h, OperationKind::write,
                                   sluice::async::detail::BorrowMetadata{3, buf, 8})
                         .has_value());
        install_noop_binding(arena, h);
        SLUICE_CHECK(arena.commit(h).has_value());
        SLUICE_CHECK(arena.cancel(h) == CancelDisposition::terminal_won);
        SLUICE_CHECK(arena.state_of(h.slot) == RequestState::backend_ready);
        SLUICE_CHECK_MSG(arena.borrow_for_test(h)->active,
                         "Scheme-B cancel must not end the borrow");
        SLUICE_CHECK(arena.enqueue(h) == EnqueueOutcome::terminal_noop);
        SLUICE_CHECK(arena.borrow_for_test(h)->active);
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        SLUICE_CHECK(!arena.borrow_for_test(h)->active);
        arena.release_completed_binding(h);
        SLUICE_CHECK(arena.slot_in_use() == 0);
    }
    // Path 2: running cancel intent records intent only; the borrow stays
    // active (the real syscall may still touch the buffer).
    {
        RequestArena arena{ContextIdentity::for_testing(1), 1};
        std::byte buf[8]{};
        SlotHandle h = submit_enqueued(
            arena, OperationKind::read,
            sluice::async::detail::BorrowMetadata{4, buf, 8});
        SLUICE_CHECK(arena.mark_running(h));
        SLUICE_CHECK(arena.cancel(h) == CancelDisposition::intent_recorded);
        SLUICE_CHECK(!arena.terminal_stored(h.slot));
        SLUICE_CHECK_MSG(arena.borrow_for_test(h)->active,
                         "running cancel intent must not end the borrow");
        // The real syscall result wins verbatim (Decision 11).
        SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
        SLUICE_CHECK(arena.borrow_for_test(h)->active);
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        SLUICE_CHECK(!arena.borrow_for_test(h)->active);
        arena.release_completed_binding(h);
        SLUICE_CHECK(arena.slot_in_use() == 0);
    }
    // Path 3: wait-cancel removes only the waiter — the borrow and the I/O
    // stay fully active.
    {
        RequestArena arena{ContextIdentity::for_testing(1), 1};
        std::byte buf[8]{};
        SlotHandle h = submit_enqueued(
            arena, OperationKind::read,
            sluice::async::detail::BorrowMetadata{5, buf, 8});
        SLUICE_CHECK(arena.register_waiter(h, WaiterToken{1, 0, 0},
                                           RoutingLease{99})
                         .has_value());
        auto rl = arena.cancel_waiter(h);
        SLUICE_CHECK(rl.has_value());
        SLUICE_CHECK_MSG(arena.borrow_for_test(h)->active,
                         "wait-cancel must not end the borrow");
        SLUICE_CHECK(arena.state_of(h.slot) == RequestState::enqueued);
        SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        SLUICE_CHECK(!arena.borrow_for_test(h)->active);
        arena.release_completed_binding(h);
        SLUICE_CHECK(arena.slot_in_use() == 0);
    }
}

// ---- Row 11: rollback never borrows; stale handles cannot touch a new -------
// occupant's borrow. Pre-commit rollback clears the metadata, increments the
// generation, and the borrow was never active. A captured stale handle reads
// nullopt and cannot mutate the new occupant's borrow metadata.
SLUICE_TEST_CASE(arena_borrow_rollback_and_stale_protection) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf1[8]{};
    std::byte buf2[16]{};

    // Generation N: prepare -> rollback (no commit, no borrow ever active).
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h0 = rh.value();
    SLUICE_CHECK(arena.prepare(h0, OperationKind::read,
                               sluice::async::detail::BorrowMetadata{9, buf1, 8})
                     .has_value());
    SLUICE_CHECK(!arena.borrow_for_test(h0)->active);  // prepared only
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(h0).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);

    // Generation N+1: the SAME physical slot is reused with DIFFERENT borrow
    // metadata. The stale N-handle must observe nullopt and leave the new
    // occupant's borrow untouched.
    SlotHandle h1 = submit_enqueued(
        arena, OperationKind::read,
        sluice::async::detail::BorrowMetadata{11, buf2, 16});
    SLUICE_CHECK(h1.slot.value == h0.slot.value);
    SLUICE_CHECK(h1.generation.value == h0.generation.value + 1);
    SLUICE_CHECK(arena.borrow_for_test(h0) == std::nullopt);  // stale handle
    auto b = arena.borrow_for_test(h1);
    SLUICE_CHECK(b.has_value());
    SLUICE_CHECK(b->fd == 11);
    SLUICE_CHECK(b->address == buf2);
    SLUICE_CHECK(b->length == 16);
    SLUICE_CHECK(b->active);

    // The stale handle cannot end the new occupant's borrow either: every
    // arena authority validates generation (borrow_for_test returns nullopt),
    // and the occupant's own borrow is still active after the stale probe.
    SLUICE_CHECK(arena.borrow_for_test(h0) == std::nullopt);
    SLUICE_CHECK(arena.borrow_for_test(h1)->active);

    // Drain the new occupant (reap ends its borrow at completion-ready).
    SLUICE_CHECK(arena.record_terminal(h1, TerminalResult::ok_bytes(16)));
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(!arena.borrow_for_test(h1)->active);
    arena.release_completed_binding(h1);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 12a: waiter registration state matrix ------------------------------
// Pinned from the as-built contract (arena register_waiter accepts only
// pending/enqueued — the pre-terminal registration window the ADR's Decision
// 10 state machine describes; a second waiter is invalid_state without
// overwriting the first; closed registration observes ready and stores
// nothing). reserved/prepared/running/backend_ready/completion_ready are
// invalid_state; a stale handle is not_found.
SLUICE_TEST_CASE(arena_waiter_registration_state_matrix) {
    RequestArena arena{ContextIdentity::for_testing(1), 5};
    std::byte buf[8]{};

    // reserved -> invalid_state.
    auto r1 = arena.reserve();
    SLUICE_CHECK(r1.has_value());
    SlotHandle h1 = r1.value();
    auto reg1 = arena.register_waiter(h1, WaiterToken{1, 0, 0}, RoutingLease{1});
    SLUICE_CHECK(!reg1.has_value());
    SLUICE_CHECK(reg1.error().code == sluice::IoError::Code::invalid_state);
    (void)arena.rollback_reserved_or_prepared(h1);

    // prepared -> invalid_state.
    auto r2 = arena.reserve();
    SLUICE_CHECK(r2.has_value());
    SlotHandle h2 = r2.value();
    SLUICE_CHECK(arena.prepare(h2, OperationKind::read, {}).has_value());
    auto reg2 = arena.register_waiter(h2, WaiterToken{1, 0, 0}, RoutingLease{1});
    SLUICE_CHECK(!reg2.has_value());
    SLUICE_CHECK(reg2.error().code == sluice::IoError::Code::invalid_state);
    (void)arena.rollback_reserved_or_prepared(h2);

    // pending (committed, not yet enqueued) -> allowed.
    auto rp = arena.reserve();
    SLUICE_CHECK(rp.has_value());
    SlotHandle hp = rp.value();
    SLUICE_CHECK(arena.prepare(hp, OperationKind::read,
                               sluice::async::detail::BorrowMetadata{0, buf, 8})
                     .has_value());
    install_noop_binding(arena, hp);
    SLUICE_CHECK(arena.commit(hp).has_value());
    SLUICE_CHECK(arena.state_of(hp.slot) == RequestState::pending);
    SLUICE_CHECK(arena.register_waiter(hp, WaiterToken{2, 0, 0}, RoutingLease{2})
                     .has_value());
    // Drain the pending slot (terminal + enqueue no-op + reap + release).
    SLUICE_CHECK(arena.cancel_waiter(hp).has_value());
    SLUICE_CHECK(arena.record_terminal(hp, TerminalResult::ok_bytes(8)));
    SLUICE_CHECK(arena.enqueue(hp) == EnqueueOutcome::terminal_noop);
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
    }
    arena.release_completed_binding(hp);

    // enqueued -> allowed (the state a real backend submit leaves the slot in);
    // a second registration on the same slot is invalid_state WITHOUT
    // overwriting the first (single-waiter cardinality, proven again in
    // arena_single_waiter_first_registration_survives with token/lease
    // equality).
    SlotHandle h3 = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});
    SLUICE_CHECK(arena.register_waiter(h3, WaiterToken{3, 0, 0}, RoutingLease{3})
                     .has_value());
    auto dup = arena.register_waiter(h3, WaiterToken{4, 0, 0}, RoutingLease{4});
    SLUICE_CHECK(!dup.has_value());
    SLUICE_CHECK(dup.error().code == sluice::IoError::Code::invalid_state);
    auto w = arena.waiter_for_test(h3);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w->token == WaiterToken{3, 0, 0}));
    SLUICE_CHECK(w->lease_id == 3);
    // Drop the first waiter, then register a second one for the running case.
    auto dropped = arena.cancel_waiter(h3);
    SLUICE_CHECK(dropped.has_value());
    SLUICE_CHECK(dropped.value().id() == 3);
    SLUICE_CHECK(arena.register_waiter(h3, WaiterToken{4, 0, 0}, RoutingLease{4})
                     .has_value());

    // running -> invalid_state (registration is a pre-terminal window; the
    // syscall is already executing).
    SLUICE_CHECK(arena.mark_running(h3));
    auto reg_running =
        arena.register_waiter(h3, WaiterToken{5, 0, 0}, RoutingLease{5});
    SLUICE_CHECK(!reg_running.has_value());
    SLUICE_CHECK(reg_running.error().code == sluice::IoError::Code::invalid_state);

    // backend_ready -> invalid_state (terminal already won; registration is
    // closed to new waiters).
    SLUICE_CHECK(arena.record_terminal(h3, TerminalResult::ok_bytes(8)));
    auto reg_br = arena.register_waiter(h3, WaiterToken{6, 0, 0}, RoutingLease{6});
    SLUICE_CHECK(!reg_br.has_value());
    SLUICE_CHECK(reg_br.error().code == sluice::IoError::Code::invalid_state);

    // completion_ready -> invalid_state (reap closed registration; a
    // higher-level waiter consumer would observe ready instead — Phase F).
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    auto reg_cr = arena.register_waiter(h3, WaiterToken{7, 0, 0}, RoutingLease{7});
    SLUICE_CHECK(!reg_cr.has_value());
    SLUICE_CHECK(reg_cr.error().code == sluice::IoError::Code::invalid_state);
    arena.release_completed_binding(h3);

    // stale -> not_found (generation advanced past the captured handle).
    auto reg_stale = arena.register_waiter(h3, WaiterToken{8, 0, 0}, RoutingLease{8});
    SLUICE_CHECK(!reg_stale.has_value());
    SLUICE_CHECK(reg_stale.error().code == sluice::IoError::Code::not_found);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 12a: single-waiter cardinality with NO overwrite -------------------
// The second registration is invalid_state AND the first waiter survives
// untouched: the final reap delivery carries token A + lease A, never B.
// After wait-cancel reopens registration, a NEW waiter B registers cleanly and
// is the one delivered — no registration residue.
SLUICE_TEST_CASE(arena_single_waiter_first_registration_survives) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf[8]{};
    SlotHandle h = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});

    // First waiter: token A + lease 99.
    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{1, 7, 3}, RoutingLease{99})
                     .has_value());
    // Second waiter: must fail AND must not overwrite A.
    auto r2 = arena.register_waiter(h, WaiterToken{2, 8, 4}, RoutingLease{100});
    SLUICE_CHECK_MSG(!r2.has_value() &&
                         r2.error().code == sluice::IoError::Code::invalid_state,
                     "second registration must be invalid_state");
    auto w = arena.waiter_for_test(h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK(w->delivery_present);
    SLUICE_CHECK_MSG((w->token == WaiterToken{1, 7, 3}),
                     "first waiter's token must survive the second registration");
    SLUICE_CHECK_MSG(w->lease_id == 99,
                     "first waiter's lease must survive the second registration");

    // Reap: the delivery is A's token + A's lease, exactly once.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.events.size() == 1);
    SLUICE_CHECK(sink.events[0].waiter.has_waiter);
    SLUICE_CHECK((sink.events[0].waiter.token == WaiterToken{1, 7, 3}));
    SLUICE_CHECK(sink.lease_ids.size() == 1);
    SLUICE_CHECK(sink.lease_ids[0] == 99);
    SLUICE_CHECK(arena.reap(sink) == 0);  // exactly-once
    // A late wait-cancel after reap gets nothing (reap consumed the lease).
    SLUICE_CHECK(!arena.cancel_waiter(h).has_value());
    arena.release_completed_binding(h);

    // Re-registration after wait-cancel: B is the one delivered; A never
    // reappears (registration lifecycle has no residue).
    SlotHandle h2 = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});
    SLUICE_CHECK(arena.register_waiter(h2, WaiterToken{1, 7, 3}, RoutingLease{99})
                     .has_value());
    auto rl = arena.cancel_waiter(h2);
    SLUICE_CHECK(rl.has_value());
    SLUICE_CHECK(rl.value().id() == 99);
    SLUICE_CHECK(arena.register_waiter(h2, WaiterToken{2, 8, 4}, RoutingLease{100})
                     .has_value());
    SLUICE_CHECK(arena.record_terminal(h2, TerminalResult::ok_bytes(8)));
    RecordingSink sink2;
    SLUICE_CHECK(arena.reap(sink2) == 1);
    SLUICE_CHECK(sink2.events[0].waiter.has_waiter);
    SLUICE_CHECK_MSG((sink2.events[0].waiter.token == WaiterToken{2, 8, 4}),
                     "after wait-cancel + re-register, only B may be delivered");
    SLUICE_CHECK(sink2.lease_ids.size() == 1);
    SLUICE_CHECK(sink2.lease_ids[0] == 100);
    arena.release_completed_binding(h2);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 13: waiter-cancel independence (wait-cancel first) -----------------
// cancel_waiter returns lease A, reopens registration, and does NOT cancel the
// I/O, does NOT pick a terminal, does NOT end the borrow. The I/O then
// completes normally; the ReadyEvent has no waiter; lease A never appears
// again.
SLUICE_TEST_CASE(arena_waiter_cancel_removes_only_the_waiter) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf[8]{};
    SlotHandle h = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});

    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{1, 0, 0}, RoutingLease{99})
                     .has_value());
    auto rl = arena.cancel_waiter(h);
    SLUICE_CHECK(rl.has_value());
    SLUICE_CHECK_MSG(rl.value().id() == 99,
                     "cancel_waiter must return the registered lease");
    auto w = arena.waiter_for_test(h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_no_waiter);
    SLUICE_CHECK(!w->delivery_present);

    // The I/O is untouched: still enqueued, no terminal stored, borrow active.
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::enqueued);
    SLUICE_CHECK(!arena.terminal_stored(h.slot));
    SLUICE_CHECK(arena.borrow_for_test(h)->active);

    // I/O completes normally; reap delivers NO waiter; lease A never re-appears.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.events.size() == 1);
    SLUICE_CHECK_MSG(!sink.events[0].waiter.has_waiter,
                     "wait-canceled waiter must not be delivered at reap");
    SLUICE_CHECK(sink.lease_ids.empty());
    SLUICE_CHECK(!arena.cancel_waiter(h).has_value());  // no double delivery
    arena.release_completed_binding(h);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 13: I/O cancel independence (I/O cancel first) ---------------------
// cancel_waiter and I/O cancel are two independent authority domains: an I/O
// cancel that WINS the canceled terminal does NOT delete the waiter
// registration. Reap delivers BOTH the canceled result AND the registered
// waiter token/lease exactly once.
SLUICE_TEST_CASE(arena_io_cancel_keeps_waiter_registration) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf[8]{};
    SlotHandle h = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});

    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{9, 0, 0}, RoutingLease{77})
                     .has_value());
    // I/O cancel wins the terminal (enqueued, Scheme B).
    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::terminal_won);
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::backend_ready);
    // The waiter registration was NOT touched by the I/O cancel.
    auto w = arena.waiter_for_test(h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w->token == WaiterToken{9, 0, 0}));
    SLUICE_CHECK(w->lease_id == 77);
    SLUICE_CHECK(arena.borrow_for_test(h)->active);

    // Reap: canceled terminal + waiter A delivered together.
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.events.size() == 1);
    SLUICE_CHECK(sink.events[0].waiter.has_waiter);
    SLUICE_CHECK((sink.events[0].waiter.token == WaiterToken{9, 0, 0}));
    SLUICE_CHECK(sink.lease_ids.size() == 1);
    SLUICE_CHECK(sink.lease_ids[0] == 77);
    SLUICE_CHECK(arena.terminal_stored(h.slot));  // canceled terminal stands
    arena.release_completed_binding(h);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 14a: lease type properties -----------------------------------------
// One move-only authority: not copyable, nothrow-movable, valid() tracks the
// moved-from state. The ADR's ReadyEvent delivery is noexcept (Decision 9
// sink), so the move operations must be noexcept for the by-value event to be
// sink-safe.
SLUICE_TEST_CASE(arena_lease_type_properties) {
    static_assert(!std::is_copy_constructible_v<RoutingLease>);
    static_assert(!std::is_copy_assignable_v<RoutingLease>);
    static_assert(std::is_nothrow_move_constructible_v<RoutingLease>);
    static_assert(std::is_nothrow_move_assignable_v<RoutingLease>);

    RoutingLease l{42};
    SLUICE_CHECK(l.valid());
    SLUICE_CHECK(l.id() == 42);
    RoutingLease moved{std::move(l)};
    SLUICE_CHECK(moved.valid());
    SLUICE_CHECK(moved.id() == 42);
    SLUICE_CHECK_MSG(!l.valid(), "a moved-from lease must be invalid");
    SLUICE_CHECK(!RoutingLease{}.valid());  // default lease is invalid
}

// ---- Row 14a: lease transfer chain (reap path) ------------------------------
// caller -> slot -> ReadyEvent: the caller's lease is moved-from after
// registration; the slot owns it while registered; reap moves it into the
// event; the slot no longer owns anything; a second reap produces nothing.
SLUICE_TEST_CASE(arena_lease_transfer_chain_reap_path) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf[8]{};
    SlotHandle h = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});

    RoutingLease caller_lease{42};
    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{1, 0, 0},
                                       std::move(caller_lease))
                     .has_value());
    SLUICE_CHECK_MSG(!caller_lease.valid(),
                     "caller lease must be moved-from after registration");
    // The slot owns the lease while registered.
    auto w = arena.waiter_for_test(h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->lease_id == 42);

    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    // The slot no longer owns the lease: registration closed, no delivery
    // present, no stored lease.
    auto w2 = arena.waiter_for_test(h);
    SLUICE_CHECK(w2.has_value());
    SLUICE_CHECK(w2->registration == WaiterRegistration::closed);
    SLUICE_CHECK(!w2->delivery_present);
    SLUICE_CHECK(w2->lease_id == 0);
    // The event owns the lease.
    SLUICE_CHECK(sink.lease_ids.size() == 1);
    SLUICE_CHECK(sink.lease_ids[0] == 42);
    // A second reap produces nothing; a late wait-cancel gets nothing.
    SLUICE_CHECK(arena.reap(sink) == 0);
    SLUICE_CHECK(!arena.cancel_waiter(h).has_value());
    arena.release_completed_binding(h);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 14a: lease transfer chain (wait-cancel path) -----------------------
// caller -> slot -> cancel_waiter return value: the returned lease is the
// EXACT registered one; the ReadyEvent never gets it.
SLUICE_TEST_CASE(arena_lease_transfer_chain_wait_cancel_path) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf[8]{};
    SlotHandle h = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});

    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{1, 0, 0}, RoutingLease{43})
                     .has_value());
    auto rl = arena.cancel_waiter(h);
    SLUICE_CHECK(rl.has_value());
    SLUICE_CHECK(rl.value().id() == 43);
    auto w = arena.waiter_for_test(h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_no_waiter);
    SLUICE_CHECK(!w->delivery_present);
    SLUICE_CHECK(w->lease_id == 0);

    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.lease_ids.empty());
    SLUICE_CHECK(!sink.events[0].waiter.has_waiter);
    arena.release_completed_binding(h);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 14a: ReadyEvent with a waiter is by-value across slot reuse --------
// Inside the sink callback the slot is released AND re-reserved (generation
// advances); the captured event's key/kind/token/lease stay intact — the event
// owns by-value identity + lease, not slot storage.
SLUICE_TEST_CASE(arena_ready_event_waiter_survives_slot_reuse) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf[8]{};
    SlotHandle h = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});
    SLUICE_CHECK(arena.register_waiter(h, WaiterToken{5, 2, 1}, RoutingLease{77})
                     .has_value());
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));

    struct ReuseDuringCallbackSink : sluice::async::detail::SynchronousReadySink {
        RequestArena* arena;
        SlotHandle h;
        sluice::async::detail::ReadyEvent captured;
        std::uint64_t generation_after_reuse = 0;
        void on_ready(sluice::async::detail::ReadyEvent e) noexcept override {
            captured = std::move(e);  // by-value copy of all fields
            // Simulate the caller resetting + reusing the slot mid-callback.
            arena->release_completed_binding(h);
            auto rh2 = arena->reserve();
            if (rh2.has_value()) generation_after_reuse = rh2.value().generation.value;
            // The captured waiter payload must still be intact here.
            (void)captured.waiter.token;
        }
    };
    ReuseDuringCallbackSink sink;
    sink.arena = &arena;
    sink.h = h;

    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.generation_after_reuse == h.generation.value + 1);
    SLUICE_CHECK(sink.captured.key.context.value == 1);
    SLUICE_CHECK(sink.captured.key.slot.value == h.slot.value);
    SLUICE_CHECK(sink.captured.key.generation.value == h.generation.value);
    SLUICE_CHECK(sink.captured.kind == OperationKind::read);
    SLUICE_CHECK(sink.captured.waiter.has_waiter);
    SLUICE_CHECK((sink.captured.waiter.token == WaiterToken{5, 2, 1}));
    SLUICE_CHECK(sink.captured.waiter.lease.id() == 77);

    // The sink re-reserved the slot during the callback; roll it back so the
    // arena destructs quiescently.
    SlotHandle reused{h.slot,
                      Generation{static_cast<std::uint32_t>(sink.generation_after_reuse)}};
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(reused).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Rows 12a/14a: register_waiter vs record_terminal race ------------------
// Both go through the arena's single leaf domain, so exactly two legal
// outcomes exist:
//   register wins  -> register succeeds; terminal follows; reap delivers
//                     token A + lease A exactly once; a late wait-cancel
//                     gets nothing.
//   terminal wins  -> register returns invalid_state; the event carries no
//                     waiter; nothing was stored.
// Never: register success with lost delivery, or register failure with a
// stored waiter. (Correctness, not scheduler fairness: we assert the
// invariant, not that both outcomes occur in every run.)
SLUICE_TEST_CASE(arena_register_waiter_vs_terminal_race) {
    constexpr std::size_t kIters = 32;
    for (std::size_t iter = 0; iter < kIters; ++iter) {
        RequestArena arena{ContextIdentity::for_testing(1), 1};
        std::byte buf[8]{};
        SlotHandle h = submit_enqueued(
            arena, OperationKind::read,
            sluice::async::detail::BorrowMetadata{0, buf, 8});

        std::atomic<bool> register_won{false};
        std::atomic<bool> register_failed{false};
        std::barrier sync{2};
        std::thread t1([&] {
            sync.arrive_and_wait();
            auto r = arena.register_waiter(h, WaiterToken{1, 0, 0}, RoutingLease{99});
            if (r.has_value()) register_won.store(true, std::memory_order_release);
            else register_failed.store(true, std::memory_order_release);
        });
        std::thread t2([&] {
            sync.arrive_and_wait();
            (void)arena.record_terminal(h, TerminalResult::ok_bytes(8));
        });
        t1.join();
        t2.join();

        SLUICE_CHECK(register_won.load() != register_failed.load());  // XOR
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        if (register_won.load()) {
            SLUICE_CHECK_MSG(sink.events[0].waiter.has_waiter,
                             "register winner must be delivered at reap");
            SLUICE_CHECK((sink.events[0].waiter.token == WaiterToken{1, 0, 0}));
            SLUICE_CHECK(sink.lease_ids.size() == 1);
            SLUICE_CHECK(sink.lease_ids[0] == 99);
            SLUICE_CHECK(!arena.cancel_waiter(h).has_value());
        } else {
            SLUICE_CHECK_MSG(!sink.events[0].waiter.has_waiter,
                             "terminal winner must close registration with no "
                             "stored waiter");
            SLUICE_CHECK(sink.lease_ids.empty());
        }
        // A second reap delivers nothing in both outcomes.
        SLUICE_CHECK(arena.reap(sink) == 0);
        arena.release_completed_binding(h);
        SLUICE_CHECK(arena.slot_in_use() == 0);
    }
}

// ---- Rows 13/14a: cancel_waiter vs reap race --------------------------------
// The C2c centerpiece: a registered waiter's token/lease must be moved out
// EXACTLY ONCE under any interleaving. Both paths go through the same leaf
// domain, so exactly two legal outcomes exist:
//   cancel_waiter wins -> it returns lease A; the ReadyEvent has no waiter.
//   reap wins          -> the ReadyEvent carries token A + lease A;
//                         cancel_waiter returns not_found.
// The ownership count of lease A is exactly one in EVERY iteration: never
// both, never neither. A second attempt after the race cannot deliver again.
SLUICE_TEST_CASE(arena_cancel_waiter_vs_reap_race) {
    constexpr std::size_t kIters = 32;
    for (std::size_t iter = 0; iter < kIters; ++iter) {
        RequestArena arena{ContextIdentity::for_testing(1), 1};
        std::byte buf[8]{};
        SlotHandle h = submit_enqueued(
            arena, OperationKind::read,
            sluice::async::detail::BorrowMetadata{0, buf, 8});
        SLUICE_CHECK(arena.register_waiter(h, WaiterToken{7, 3, 2}, RoutingLease{99})
                         .has_value());
        // Terminal recorded + enqueue pin acked -> backend_ready, reap-eligible.
        SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));

        std::optional<RoutingLease> cancel_result;
        std::barrier sync{2};
        RecordingSink sink;
        std::thread t1([&] {
            sync.arrive_and_wait();
            auto r = arena.cancel_waiter(h);
            if (r.has_value()) cancel_result = std::move(r.value());
        });
        std::thread t2([&] {
            sync.arrive_and_wait();
            (void)arena.reap(sink);
        });
        t1.join();
        t2.join();

        const bool cancel_won = cancel_result.has_value();
        const bool reap_delivered = sink.lease_ids.size() == 1;
        // Exactly one owner of lease 99: cancel XOR reap.
        SLUICE_CHECK_MSG(cancel_won != reap_delivered,
                         "lease ownership count must be exactly one "
                         "(cancel XOR reap)");
        if (cancel_won) {
            SLUICE_CHECK(cancel_result->id() == 99);
            SLUICE_CHECK(sink.events.size() == 1);
            SLUICE_CHECK(!sink.events[0].waiter.has_waiter);
            // The second reap after cancel-won delivers nothing.
            SLUICE_CHECK(arena.reap(sink) == 0);
        } else {
            SLUICE_CHECK(sink.events.size() == 1);
            SLUICE_CHECK(sink.events[0].waiter.has_waiter);
            SLUICE_CHECK((sink.events[0].waiter.token == WaiterToken{7, 3, 2}));
            SLUICE_CHECK(sink.lease_ids[0] == 99);
            // The second wait-cancel after reap-won gets nothing.
            SLUICE_CHECK(!arena.cancel_waiter(h).has_value());
            SLUICE_CHECK(arena.reap(sink) == 0);
        }
        arena.release_completed_binding(h);
        SLUICE_CHECK(arena.slot_in_use() == 0);
    }
}
