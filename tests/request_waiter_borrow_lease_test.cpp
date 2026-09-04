// Phase C2c — arena-level waiter / borrow / delivery-lease conformance matrix.
//
// Issue #68 rows 11-14, proven at the RequestArena authority layer:
//   row 11  — fd/buffer borrow lifetime: prepare writes metadata inactive;
//             commit is the borrow linearization point (active); the borrow
//             stays active across pending/enqueued/running/backend_ready,
//             across record_terminal, Scheme-B cancel, running cancel intent,
//             and wait-cancel; reap ends it BEFORE the Completion-ready
//             publication (I18 — pinned by a deterministic publication-order
//             trace, not just post-reap observation); rollback never borrows
//             and clears metadata; generation++ makes a stale handle unable
//             to touch a new occupant's borrow.
//   row 12a — single-waiter registration: one registration authority; a
//             second register is invalid_state AND does not overwrite the
//             first (final delivery carries token A + lease A, never B);
//             per-state registration matrix pinned from ADR Decision 10 —
//             registration is ORTHOGONAL to execution state and only reap
//             closes it: legal in pending/enqueued/running/backend_ready
//             while registration is open; reserved/prepared (pre-commit
//             binding window) and completion_ready (reap closed) are
//             invalid_state; stale not_found. A failed registration consumes
//             the candidate lease at the by-value call boundary and releases
//             it inline — it is never transferred to the slot (ADR :661-662:
//             "Scheduler reclaims it or completes inline as appropriate" —
//             Phase B completes inline).
//   row 13  — waiter-cancel independence: cancel_waiter removes ONLY the
//             waiter; it never cancels the I/O, never picks a terminal,
//             never ends the borrow. I/O cancel removes NOTHING of the
//             waiter: a canceled terminal still delivers the registered
//             waiter at reap. (Two independent ownership domains.)
//   row 14a — delivery lease: move-only type properties; caller -> slot ->
//             ReadyEvent (or caller -> slot -> cancel_waiter return) transfer
//             chain with exactly one owner; ReadyEvent is by-value and stays
//             valid across slot release + reuse even with a waiter payload;
//             sinks consume the delivery (including the move-only lease)
//             INSIDE the callback and retain plain scalars only (ADR :625-636
//             callback-scoped consumption).
//
// Concurrency (row 13/14a): register_waiter vs reap (the terminal winner does
// NOT close registration — ADR Decision 10 — so reap is the only registration
// closer) and cancel_waiter vs reap are driven with std::barrier-released
// threads under the arena's single leaf domain — only the two ADR-legal
// outcomes can occur, and the lease ownership count is exactly one in every
// interleaving.
//
// Links sluice_async_internal_testing: the generation-validated
// borrow_for_test / waiter_for_test observation seams and the
// publication_order_for_test I18 trace are guarded by
// SLUICE_ASYNC_INTERNAL_TESTING (AGENTS.md §3.9; production carries nothing).
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

// Records every delivery as plain scalars (key/kind/token/lease-id) and drops
// the by-value ReadyEvent — including the move-only RoutingLease — INSIDE the
// callback: callback-scoped consumption (ADR :625-636 — the sink may copy the
// by-value key/kind and consume the move-only waiter delivery; it must not
// retain the event or lease past the call). This sink exists ONLY for
// event-content inspection; the allocation-independent sink contract is proven
// by the ReferenceReadySink observation in the backend integration targets.
struct RecordingSink : sluice::async::detail::SynchronousReadySink {
    struct Delivery {
        sluice::async::detail::RequestKey key{};
        sluice::async::detail::OperationKind kind =
            sluice::async::detail::OperationKind::read;
        bool has_waiter = false;
        sluice::async::detail::WaiterToken token{};
        std::uint64_t lease_id = 0;
    };
    std::vector<Delivery> deliveries;
    void on_ready(sluice::async::detail::ReadyEvent e) noexcept override {
        Delivery d;
        d.key = e.key;
        d.kind = e.kind;
        d.has_waiter = e.waiter.has_waiter;
        d.token = e.waiter.token;
        d.lease_id = e.waiter.has_waiter ? e.waiter.lease.id() : 0;
        deliveries.push_back(d);
        // e — including its move-only lease — is consumed and dropped here,
        // inside the callback; nothing escapes the call.
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
    {
        auto obs = arena.borrow_for_test(h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }

    // pending -> enqueued: borrow persists.
    SLUICE_CHECK(arena.enqueue(h) == EnqueueOutcome::enqueued);
    {
        auto obs = arena.borrow_for_test(h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }

    // enqueued -> running (dispatch shape): borrow persists.
    SLUICE_CHECK(arena.mark_running(h));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::running);
    {
        auto obs = arena.borrow_for_test(h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }

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
    {
        auto obs = arena.borrow_for_test(h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(!obs->active);
    }

    // release: slot free, no observable borrow.
    arena.release_completed_binding(h);
    SLUICE_CHECK(!arena.borrow_for_test(h).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 11: I18 publication-order (borrow ends BEFORE Completion-ready) ----
// A deterministic trace seam inside reap's leaf-domain critical section
// records borrow-end and the Completion-ready publication as sequence numbers.
// An acquire observer of Completion-ready must see the ended borrow (I18), so
// the publication sequence must follow the borrow-end sequence. A mutant that
// moves `borrow_.active = false` AFTER the publication call — same critical
// section, still before reap returns — flips the order and this case fails;
// the post-reap borrow observation alone cannot distinguish that defect.
SLUICE_TEST_CASE(arena_borrow_publication_order) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    std::byte buf[8]{};
    SlotHandle h = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));

    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    auto order = arena.publication_order_for_test();
    SLUICE_CHECK(order.borrow_end_seq > 0);
    SLUICE_CHECK_MSG(order.publish_seq > order.borrow_end_seq,
                     "borrow must end BEFORE the Completion-ready publication "
                     "(I18: an acquire observer of ready sees the ended borrow)");
    // The conventional post-reap observation still holds.
    {
        auto obs = arena.borrow_for_test(h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(!obs->active);
    }
    arena.release_completed_binding(h);
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
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK_MSG(obs.has_value() && obs->active,
                             "Scheme-B cancel must not end the borrow");
        }
        SLUICE_CHECK(arena.enqueue(h) == EnqueueOutcome::terminal_noop);
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK(obs.has_value());
            SLUICE_CHECK(obs->active);
        }
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK(obs.has_value());
            SLUICE_CHECK(!obs->active);
        }
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
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK_MSG(obs.has_value() && obs->active,
                             "running cancel intent must not end the borrow");
        }
        // The real syscall result wins verbatim (Decision 11).
        SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK(obs.has_value());
            SLUICE_CHECK(obs->active);
        }
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK(obs.has_value());
            SLUICE_CHECK(!obs->active);
        }
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
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK_MSG(obs.has_value() && obs->active,
                             "wait-cancel must not end the borrow");
        }
        SLUICE_CHECK(arena.state_of(h.slot) == RequestState::enqueued);
        SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        {
            auto obs = arena.borrow_for_test(h);
            SLUICE_CHECK(obs.has_value());
            SLUICE_CHECK(!obs->active);
        }
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
    {
        auto obs = arena.borrow_for_test(h0);
        SLUICE_CHECK_MSG(obs.has_value() && !obs->active, "prepared only");
    }
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
    {
        auto obs = arena.borrow_for_test(h1);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }

    // Drain the new occupant (reap ends its borrow at completion-ready).
    SLUICE_CHECK(arena.record_terminal(h1, TerminalResult::ok_bytes(16)));
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    {
        auto obs = arena.borrow_for_test(h1);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(!obs->active);
    }
    arena.release_completed_binding(h1);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 12a: waiter registration state matrix ------------------------------
// Pinned from ADR Decision 10: registration is ORTHOGONAL to execution state
// and only reap closes it (:668-698). Legal while the request is accepted and
// unreaped (pending/enqueued/running/backend_ready) with registration open; a
// second waiter is invalid_state WITHOUT overwriting the first; reserved/
// prepared (pre-commit binding window, :483-484) and completion_ready (reap
// closed registration) are invalid_state; a stale handle is not_found. A
// failed registration consumes the candidate lease at the by-value call
// boundary and releases it inline — never transferred to the slot (ADR
// :661-662: "Scheduler reclaims it or completes inline as appropriate" —
// Phase B completes inline).
SLUICE_TEST_CASE(arena_waiter_registration_state_matrix) {
    RequestArena arena{ContextIdentity::for_testing(1), 8};
    std::byte buf[8]{};

    // reserved -> invalid_state (pre-commit binding window); the candidate
    // lease is consumed at the call boundary and released inline.
    auto r1 = arena.reserve();
    SLUICE_CHECK(r1.has_value());
    SlotHandle h1 = r1.value();
    RoutingLease cand1{1};
    auto reg1 = arena.register_waiter(h1, WaiterToken{1, 0, 0}, std::move(cand1));
    SLUICE_CHECK(!reg1.has_value());
    SLUICE_CHECK(reg1.error().code == sluice::IoError::Code::invalid_state);
    SLUICE_CHECK_MSG(!cand1.valid(),
                     "failed registration must consume the candidate lease at "
                     "the by-value boundary (never transferred to the slot)");
    auto w1 = arena.waiter_for_test(h1);
    SLUICE_CHECK(w1.has_value());
    SLUICE_CHECK(w1->registration == WaiterRegistration::open_no_waiter);
    SLUICE_CHECK(!w1->delivery_present);
    SLUICE_CHECK(w1->lease_id == 0);
    (void)arena.rollback_reserved_or_prepared(h1);

    // prepared -> invalid_state (pre-commit); same inline lease consumption.
    auto r2 = arena.reserve();
    SLUICE_CHECK(r2.has_value());
    SlotHandle h2 = r2.value();
    SLUICE_CHECK(arena.prepare(h2, OperationKind::read, {}).has_value());
    RoutingLease cand2{2};
    auto reg2 = arena.register_waiter(h2, WaiterToken{1, 0, 0}, std::move(cand2));
    SLUICE_CHECK(!reg2.has_value());
    SLUICE_CHECK(reg2.error().code == sluice::IoError::Code::invalid_state);
    SLUICE_CHECK(!cand2.valid());
    auto w2 = arena.waiter_for_test(h2);
    SLUICE_CHECK(w2.has_value());
    SLUICE_CHECK(w2->registration == WaiterRegistration::open_no_waiter);
    SLUICE_CHECK(!w2->delivery_present);
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
    // Drain the pending slot (wait-cancel + terminal + enqueue no-op + reap).
    SLUICE_CHECK(arena.cancel_waiter(hp).has_value());
    SLUICE_CHECK(arena.record_terminal(hp, TerminalResult::ok_bytes(8)));
    SLUICE_CHECK(arena.enqueue(hp) == EnqueueOutcome::terminal_noop);
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
    }
    arena.release_completed_binding(hp);

    // enqueued -> allowed (the state a real backend submit leaves the slot in);
    // a second registration is invalid_state WITHOUT overwriting the first
    // (single-waiter cardinality, proven again with token/lease equality in
    // arena_single_waiter_first_registration_survives).
    SlotHandle h3 = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});
    SLUICE_CHECK(arena.register_waiter(h3, WaiterToken{3, 0, 0}, RoutingLease{3})
                     .has_value());
    RoutingLease cand_dup{4};
    auto dup = arena.register_waiter(h3, WaiterToken{4, 0, 0}, std::move(cand_dup));
    SLUICE_CHECK(!dup.has_value());
    SLUICE_CHECK(dup.error().code == sluice::IoError::Code::invalid_state);
    SLUICE_CHECK_MSG(!cand_dup.valid(),
                     "duplicate registration must consume the candidate lease "
                     "inline without overwriting the first");
    auto w3 = arena.waiter_for_test(h3);
    SLUICE_CHECK(w3.has_value());
    SLUICE_CHECK(w3->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w3->token == WaiterToken{3, 0, 0}));
    SLUICE_CHECK(w3->lease_id == 3);

    // running -> the syscall executing does NOT close registration: a second
    // waiter is still rejected by CARDINALITY (state-independent), and the
    // original registration is untouched.
    SLUICE_CHECK(arena.mark_running(h3));
    RoutingLease cand_run{5};
    auto reg_run = arena.register_waiter(h3, WaiterToken{5, 0, 0},
                                         std::move(cand_run));
    SLUICE_CHECK(!reg_run.has_value());
    SLUICE_CHECK(reg_run.error().code == sluice::IoError::Code::invalid_state);
    SLUICE_CHECK(!cand_run.valid());
    auto w4 = arena.waiter_for_test(h3);
    SLUICE_CHECK(w4.has_value());
    SLUICE_CHECK(w4->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w4->token == WaiterToken{3, 0, 0}));
    SLUICE_CHECK(w4->lease_id == 3);

    // backend_ready -> the terminal winner does NOT close registration either:
    // a duplicate is still rejected and the original waiter is delivered at
    // reap.
    SLUICE_CHECK(arena.record_terminal(h3, TerminalResult::ok_bytes(8)));
    RoutingLease cand_br{6};
    auto reg_br = arena.register_waiter(h3, WaiterToken{6, 0, 0},
                                        std::move(cand_br));
    SLUICE_CHECK(!reg_br.has_value());
    SLUICE_CHECK(reg_br.error().code == sluice::IoError::Code::invalid_state);
    SLUICE_CHECK(!cand_br.valid());
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        SLUICE_CHECK(sink.deliveries.size() == 1);
        SLUICE_CHECK(sink.deliveries[0].has_waiter);
        SLUICE_CHECK((sink.deliveries[0].token == WaiterToken{3, 0, 0}));
        SLUICE_CHECK(sink.deliveries[0].lease_id == 3);
    }

    // completion_ready -> invalid_state (reap closed registration; a
    // higher-level waiter consumer would observe ready instead — Phase F).
    RoutingLease cand_cr{7};
    auto reg_cr = arena.register_waiter(h3, WaiterToken{7, 0, 0},
                                        std::move(cand_cr));
    SLUICE_CHECK(!reg_cr.has_value());
    SLUICE_CHECK(reg_cr.error().code == sluice::IoError::Code::invalid_state);
    SLUICE_CHECK(!cand_cr.valid());
    arena.release_completed_binding(h3);

    // stale -> not_found (generation advanced past the captured handle); the
    // candidate lease is consumed at the boundary and never touches a later
    // occupant.
    RoutingLease cand_stale{8};
    auto reg_stale = arena.register_waiter(h3, WaiterToken{8, 0, 0},
                                           std::move(cand_stale));
    SLUICE_CHECK(!reg_stale.has_value());
    SLUICE_CHECK(reg_stale.error().code == sluice::IoError::Code::not_found);
    SLUICE_CHECK(!cand_stale.valid());
    SLUICE_CHECK(arena.slot_in_use() == 0);

    // --- Positive window proofs (ADR Decision 10): a FRESH registration is
    // accepted while running and at backend_ready, and reap delivers it
    // exactly once. ---
    // running -> success.
    SlotHandle h4 = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});
    SLUICE_CHECK(arena.mark_running(h4));
    RoutingLease lease_run{55};
    SLUICE_CHECK(arena.register_waiter(h4, WaiterToken{9, 1, 1},
                                       std::move(lease_run))
                     .has_value());
    SLUICE_CHECK(!lease_run.valid());  // transferred to the slot
    auto w5 = arena.waiter_for_test(h4);
    SLUICE_CHECK(w5.has_value());
    SLUICE_CHECK(w5->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w5->token == WaiterToken{9, 1, 1}));
    SLUICE_CHECK(w5->lease_id == 55);
    SLUICE_CHECK(arena.record_terminal(h4, TerminalResult::ok_bytes(8)));
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        SLUICE_CHECK(sink.deliveries.size() == 1);
        SLUICE_CHECK(sink.deliveries[0].has_waiter);
        SLUICE_CHECK((sink.deliveries[0].token == WaiterToken{9, 1, 1}));
        SLUICE_CHECK(sink.deliveries[0].lease_id == 55);
    }
    arena.release_completed_binding(h4);

    // backend_ready (terminal recorded, reap not yet run) -> success; reap
    // delivers the registered waiter.
    SlotHandle h5 = submit_enqueued(
        arena, OperationKind::read, sluice::async::detail::BorrowMetadata{0, buf, 8});
    SLUICE_CHECK(arena.record_terminal(h5, TerminalResult::ok_bytes(8)));
    SLUICE_CHECK(arena.state_of(h5.slot) == RequestState::backend_ready);
    RoutingLease lease_br{66};
    SLUICE_CHECK(arena.register_waiter(h5, WaiterToken{10, 2, 2},
                                       std::move(lease_br))
                     .has_value());
    SLUICE_CHECK(!lease_br.valid());
    auto w6 = arena.waiter_for_test(h5);
    SLUICE_CHECK(w6.has_value());
    SLUICE_CHECK(w6->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w6->token == WaiterToken{10, 2, 2}));
    SLUICE_CHECK(w6->lease_id == 66);
    {
        RecordingSink sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        SLUICE_CHECK(sink.deliveries.size() == 1);
        SLUICE_CHECK(sink.deliveries[0].has_waiter);
        SLUICE_CHECK((sink.deliveries[0].token == WaiterToken{10, 2, 2}));
        SLUICE_CHECK(sink.deliveries[0].lease_id == 66);
    }
    arena.release_completed_binding(h5);
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
    SLUICE_CHECK(sink.deliveries.size() == 1);
    SLUICE_CHECK(sink.deliveries[0].has_waiter);
    SLUICE_CHECK((sink.deliveries[0].token == WaiterToken{1, 7, 3}));
    SLUICE_CHECK(sink.deliveries[0].lease_id == 99);
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
    SLUICE_CHECK(sink2.deliveries[0].has_waiter);
    SLUICE_CHECK_MSG((sink2.deliveries[0].token == WaiterToken{2, 8, 4}),
                     "after wait-cancel + re-register, only B may be delivered");
    SLUICE_CHECK(sink2.deliveries[0].lease_id == 100);
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
    {
        auto obs = arena.borrow_for_test(h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }

    // I/O completes normally; reap delivers NO waiter; lease A never re-appears.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(8)));
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.deliveries.size() == 1);
    SLUICE_CHECK_MSG(!sink.deliveries[0].has_waiter,
                     "wait-canceled waiter must not be delivered at reap");
    SLUICE_CHECK(sink.deliveries[0].lease_id == 0);
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
    {
        auto obs = arena.borrow_for_test(h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }

    // Reap: canceled terminal + waiter A delivered together.
    RecordingSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.deliveries.size() == 1);
    SLUICE_CHECK(sink.deliveries[0].has_waiter);
    SLUICE_CHECK((sink.deliveries[0].token == WaiterToken{9, 0, 0}));
    SLUICE_CHECK(sink.deliveries[0].lease_id == 77);
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
    SLUICE_CHECK(sink.deliveries.size() == 1);
    SLUICE_CHECK(sink.deliveries[0].lease_id == 42);
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
    SLUICE_CHECK(sink.deliveries.size() == 1);
    SLUICE_CHECK(sink.deliveries[0].lease_id == 0);
    SLUICE_CHECK(!sink.deliveries[0].has_waiter);
    arena.release_completed_binding(h);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Row 14a: ReadyEvent with a waiter is by-value across slot reuse --------
// Inside the sink callback the slot is released AND re-reserved (generation
// advances). The sink copies ONLY plain scalars (key/kind/token/lease-id) and
// consumes the by-value event — including the move-only lease — inside the
// callback (ADR :625-636 callback-scoped consumption); nothing is retained
// past the call. The saved scalars stay valid after reap returns even though
// the slot was released + reused mid-callback — the delivery is by-value
// identity, not slot storage.
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
        sluice::async::detail::RequestKey captured_key{};
        sluice::async::detail::OperationKind captured_kind =
            sluice::async::detail::OperationKind::read;
        bool captured_has_waiter = false;
        sluice::async::detail::WaiterToken captured_token{};
        std::uint64_t captured_lease_id = 0;
        std::uint64_t generation_after_reuse = 0;
        void on_ready(sluice::async::detail::ReadyEvent e) noexcept override {
            // Copy plain scalars only; the move-only lease is consumed and
            // dropped here WITH e (callback-scoped, ADR :625-636).
            captured_key = e.key;
            captured_kind = e.kind;
            captured_has_waiter = e.waiter.has_waiter;
            captured_token = e.waiter.token;
            captured_lease_id = e.waiter.has_waiter ? e.waiter.lease.id() : 0;
            // Simulate the caller resetting + reusing the slot mid-callback.
            arena->release_completed_binding(h);
            auto rh2 = arena->reserve();
            if (rh2.has_value()) generation_after_reuse = rh2.value().generation.value;
        }
    };
    ReuseDuringCallbackSink sink;
    sink.arena = &arena;
    sink.h = h;

    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(sink.generation_after_reuse == h.generation.value + 1);
    SLUICE_CHECK(sink.captured_key.context.value == 1);
    SLUICE_CHECK(sink.captured_key.slot.value == h.slot.value);
    SLUICE_CHECK(sink.captured_key.generation.value == h.generation.value);
    SLUICE_CHECK(sink.captured_kind == OperationKind::read);
    SLUICE_CHECK(sink.captured_has_waiter);
    SLUICE_CHECK((sink.captured_token == WaiterToken{5, 2, 1}));
    SLUICE_CHECK(sink.captured_lease_id == 77);

    // The sink re-reserved the slot during the callback; roll it back so the
    // arena destructs quiescently. The generation is 64-bit (request_key.hpp:
    // a 32-bit wrap would re-introduce ABA) — no narrowing cast.
    SlotHandle reused{h.slot, Generation{sink.generation_after_reuse}};
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(reused).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Rows 12a/14a: register_waiter vs reap race -----------------------------
// ADR Decision 10 (:691-695): registration is orthogonal to execution state
// and ONLY reap closes it — the terminal winner does NOT close registration.
// The load-bearing race is therefore register vs REAP (not register vs
// record_terminal), driven through the arena's single leaf domain:
//   register wins -> token A + lease A are stored and reap delivers them
//                    exactly once; a late wait-cancel gets nothing.
//   reap wins      -> reap closed registration first; register returns
//                    invalid_state and stores nothing; the candidate lease is
//                    consumed at the by-value call boundary (moved-from,
//                    released inline — ADR :661-662) and never reaches the
//                    slot or the event.
// Lease ownership is exactly one in every iteration: delivered once by reap,
// or consumed inline by the failed registration — never both, never neither.
// (Correctness, not scheduler fairness: we assert the invariant, not that both
// outcomes occur in every run.)
SLUICE_TEST_CASE(arena_register_waiter_vs_reap_race) {
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
        RecordingSink sink;
        RoutingLease candidate{99};
        std::thread t1([&] {
            sync.arrive_and_wait();
            auto r = arena.register_waiter(h, WaiterToken{1, 0, 0},
                                           std::move(candidate));
            if (r.has_value()) register_won.store(true, std::memory_order_release);
            else register_failed.store(true, std::memory_order_release);
        });
        std::thread t2([&] {
            sync.arrive_and_wait();
            (void)arena.record_terminal(h, TerminalResult::ok_bytes(8));
            (void)arena.reap(sink);
        });
        t1.join();
        t2.join();

        // The candidate lease is consumed at the by-value boundary in BOTH
        // outcomes (transferred to the slot on success, released inline on
        // failure) — the caller can never observe it again.
        SLUICE_CHECK(!candidate.valid());
        SLUICE_CHECK(register_won.load() != register_failed.load());  // XOR
        SLUICE_CHECK(sink.deliveries.size() == 1);
        if (register_won.load()) {
            SLUICE_CHECK_MSG(sink.deliveries[0].has_waiter,
                             "register winner must be delivered at reap");
            SLUICE_CHECK((sink.deliveries[0].token == WaiterToken{1, 0, 0}));
            SLUICE_CHECK(sink.deliveries[0].lease_id == 99);
            SLUICE_CHECK(!arena.cancel_waiter(h).has_value());
        } else {
            SLUICE_CHECK_MSG(!sink.deliveries[0].has_waiter,
                             "reap winner must close registration: the event "
                             "carries no waiter and the candidate lease never "
                             "reaches the slot");
            SLUICE_CHECK(sink.deliveries[0].lease_id == 0);
            auto w = arena.waiter_for_test(h);
            SLUICE_CHECK(w.has_value());
            SLUICE_CHECK(w->registration == WaiterRegistration::closed);
            SLUICE_CHECK(!w->delivery_present);
            SLUICE_CHECK(w->lease_id == 0);
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
        const bool reap_delivered =
            sink.deliveries.size() == 1 && sink.deliveries[0].has_waiter;
        // Exactly one owner of lease 99: cancel XOR reap.
        SLUICE_CHECK_MSG(cancel_won != reap_delivered,
                         "lease ownership count must be exactly one "
                         "(cancel XOR reap)");
        if (cancel_won) {
            SLUICE_CHECK(cancel_result->id() == 99);
            SLUICE_CHECK(sink.deliveries.size() == 1);
            SLUICE_CHECK(!sink.deliveries[0].has_waiter);
            // The second reap after cancel-won delivers nothing.
            SLUICE_CHECK(arena.reap(sink) == 0);
        } else {
            SLUICE_CHECK(sink.deliveries.size() == 1);
            SLUICE_CHECK(sink.deliveries[0].has_waiter);
            SLUICE_CHECK((sink.deliveries[0].token == WaiterToken{7, 3, 2}));
            SLUICE_CHECK(sink.deliveries[0].lease_id == 99);
            // The second wait-cancel after reap-won gets nothing.
            SLUICE_CHECK(!arena.cancel_waiter(h).has_value());
            SLUICE_CHECK(arena.reap(sink) == 0);
        }
        arena.release_completed_binding(h);
        SLUICE_CHECK(arena.slot_in_use() == 0);
    }
}
