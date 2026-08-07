// Phase B reference lifecycle — RequestArena unit tests.
//
// Tests the bounded RequestSlot arena in isolation (the narrowest seam of the
// request lifecycle) before any backend consumes it. The arena is an internal
// detail:: type, but its capacity / reserve / release / generation contract is
// a deliberate test seam documented in docs/design/phase-b-request-slot-reference.md
// (the design's tested seam declaration).
//
// ADR-explicit-io-request-contract (Accepted):
//   Decision 2  — one bounded RequestSlot arena per context/backend pair; no two
//                 independently oversubscribable stores.
//   Decision 13 — request_capacity is fixed at construction; full -> would_block;
//                 genuine init failure -> no_space; slot_in_use and accepted_outstanding
//                 are distinct counters.
//   I6          — slot reuse increments generation; stale key cannot act.
//   I8          — in-use slots never exceed configured capacity.
//   I9          — accepted terminal path does not depend on new unbounded allocation
//                 (reserve pre-allocates terminal storage).
//
// No sleep_for, no timing assumptions: the arena is single-threaded-deterministic by
// construction (the leaf slot-lifecycle mutex exists for the backend's later
// multi-threaded use; these tests exercise it under a single thread).
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/error.hpp>

#include "harness.hpp"

SLUICE_MAIN()

// ---- Slice 1: capacity bounded ----------------------------------------------
// I8: in-use slots never exceed configured request_capacity. A reserve past the
// configured capacity returns would_block synchronously and leaves no side effect
// (no slot marked in-use, no accepted_outstanding increment).
SLUICE_TEST_CASE(arena_capacity_bounded) {
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(1), /*request_capacity=*/2};
    SLUICE_CHECK(arena.capacity() == 2);

    auto r1 = arena.reserve();
    auto r2 = arena.reserve();
    SLUICE_CHECK(r1.has_value());
    SLUICE_CHECK(r2.has_value());
    SLUICE_CHECK(arena.slot_in_use() == 2);

    // Capacity exhausted -> synchronous would_block; nothing mutated.
    auto r3 = arena.reserve();
    SLUICE_CHECK(!r3.has_value());
    SLUICE_CHECK(r3.error().code == sluice::IoError::Code::would_block);
    SLUICE_CHECK(arena.slot_in_use() == 2);          // unchanged
    SLUICE_CHECK(arena.accepted_outstanding() == 0); // reserve does not accept
    SLUICE_CHECK(arena.capacity_rejections() == 1);  // distinct metric (P1-05)

    // Release both reservations so the arena destructs quiescently (ADR
    // Decision 15: destruction with slot_in_use != 0 fails fast). The
    // pre-commit rollback authority returns the non-accepted slots.
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(r1.value()).has_value());
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(r2.value()).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Slice 2: generation advances on release; stale key rejected -------------
// I6: slot reuse increments the generation BEFORE the next key can become
// visible. A previously-released handle (stale generation) cannot act on the
// reused slot — release returns not_found and does not touch the new occupant.
SLUICE_TEST_CASE(arena_generation_advances_on_release) {
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(7), /*request_capacity=*/1};

    auto first = arena.reserve();
    SLUICE_CHECK(first.has_value());
    auto first_key = arena.key_of(first.value().slot);
    SLUICE_CHECK(first_key.context.value == 7);
    SLUICE_CHECK(first_key.slot.value == 0);
    SLUICE_CHECK(first_key.generation.value == 0);

    // Release the only slot; its generation must increment before reuse.
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(first.value()).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);

    // Re-reserve the same physical slot — generation now differs.
    auto second = arena.reserve();
    SLUICE_CHECK(second.has_value());
    SLUICE_CHECK(second.value().slot.value == 0);  // same physical slot
    auto second_key = arena.key_of(second.value().slot);
    SLUICE_CHECK(second_key.generation.value == first_key.generation.value + 1);

    // The OLD handle is stale: its generation no longer matches. Releasing it
    // again must NOT act on the reused slot (I6) and returns not_found.
    auto reresult = arena.rollback_reserved_or_prepared(first.value());
    SLUICE_CHECK(!reresult.has_value());
    SLUICE_CHECK(reresult.error().code == sluice::IoError::Code::not_found);
    // The second (current) reservation is untouched.
    SLUICE_CHECK(arena.slot_in_use() == 1);
    SLUICE_CHECK(arena.key_of(second.value().slot).generation.value ==
                 second_key.generation.value);
    // Release the live reservation so the arena destructs quiescently.
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(second.value()).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

SLUICE_TEST_CASE(arena_stale_key_rejected) {
    // Independent case: a handle whose slot index is out of range, or whose
    // generation never existed, is rejected with not_found (never UB, never acts
    // on an unrelated slot).
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(1), /*request_capacity=*/2};

    sluice::async::detail::SlotHandle bogus_out_of_range{
        sluice::async::detail::SlotIndex{999}, sluice::async::detail::Generation{0}};
    auto r1 = arena.rollback_reserved_or_prepared(bogus_out_of_range);
    SLUICE_CHECK(!r1.has_value());
    SLUICE_CHECK(r1.error().code == sluice::IoError::Code::not_found);

    // A handle to a slot that was never reserved: slot 1 is free, so its state is
    // free and the (generation 0, free) check rejects with not_found.
    sluice::async::detail::SlotHandle bogus_free{
        sluice::async::detail::SlotIndex{1}, sluice::async::detail::Generation{0}};
    auto r2 = arena.rollback_reserved_or_prepared(bogus_free);
    SLUICE_CHECK(!r2.has_value());
    SLUICE_CHECK(r2.error().code == sluice::IoError::Code::not_found);
    SLUICE_CHECK(arena.slot_in_use() == 0);  // nothing mutated
}

// ---- Slice 3: distinct counters (slot_in_use vs accepted_outstanding) -------
// I3/I8, P1-05. slot_in_use tracks reserve -> release (admission capacity);
// accepted_outstanding tracks commit -> completion-ready publication (terminal
// accounting). They MUST NOT be merged into one counter. Commit 1 has no commit/
// accept path yet, so accepted_outstanding stays 0 while slot_in_use moves with
// reserve/release — which is exactly the property that proves they are separate
// fields and separate accounting, not one number.
SLUICE_TEST_CASE(arena_accounting_tracks_slot_in_use_vs_accepted_outstanding) {
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(1), /*request_capacity=*/3};

    SLUICE_CHECK(arena.slot_in_use() == 0);
    SLUICE_CHECK(arena.accepted_outstanding() == 0);

    auto a = arena.reserve();
    auto b = arena.reserve();
    SLUICE_CHECK(a.has_value() && b.has_value());

    // reserve raises slot_in_use; accepted_outstanding is a different counter and
    // does NOT move on reserve (commit is the accept point, not reserve).
    SLUICE_CHECK(arena.slot_in_use() == 2);
    SLUICE_CHECK(arena.accepted_outstanding() == 0);
    SLUICE_CHECK(arena.high_water_mark() == 2);

    // Releasing one drops slot_in_use; accepted_outstanding is still 0 (no commit
    // happened). If these were the same counter, releasing a non-accepted slot
    // would underflow or corrupt accepted accounting.
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(a.value()).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 1);
    SLUICE_CHECK(arena.accepted_outstanding() == 0);
    SLUICE_CHECK(arena.high_water_mark() == 2);  // high-water mark does not decay

    SLUICE_CHECK(arena.rollback_reserved_or_prepared(b.value()).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
    SLUICE_CHECK(arena.accepted_outstanding() == 0);
}

// ---- Slice 4: reserve is allocation-free for the accepted terminal path -----
// I9. The slot array is fixed at construction; reserve claims a free slot and
// never heap-allocates. We assert the observable contract: after construction,
// repeatedly reserve+release does not change capacity, never reports a rejection
// attributable to allocation, and slots remain addressable by index. (A rigorous
// allocation-free proof is the ASan/UBSan run of the full lifecycle tests in
// commit 3; this case asserts the deterministic observable property.)
SLUICE_TEST_CASE(arena_no_post_accept_allocation) {
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(1), /*request_capacity=*/4};

    // Reserve + release every slot many times over. Capacity is unchanged; no
    // rejection accumulates from allocation failure (only would_block on a
    // genuinely-full arena, which we never hit here).
    for (int round = 0; round < 1000; ++round) {
        auto h = arena.reserve();
        SLUICE_CHECK_MSG(h.has_value(), "reserve must not fail on a non-full arena");
        SLUICE_CHECK(arena.rollback_reserved_or_prepared(h.value()).has_value());
    }
    SLUICE_CHECK(arena.capacity() == 4);                // unchanged
    SLUICE_CHECK(arena.slot_in_use() == 0);             // all released
    SLUICE_CHECK(arena.capacity_rejections() == 0);     // no allocation-driven reject
}

// ---- Slice 5: fd/buffer borrow lifecycle (ADR Decision 8 / I7 / I18) ---------
// Borrowing begins at commit (borrow_active == true) and ends at completion-
// ready publication (reap clears it). An acquire observer of Completion-ready
// therefore sees the ended borrow — one of the I18 bookkeeping effects.
SLUICE_TEST_CASE(arena_borrow_lifecycle) {
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(1), /*request_capacity=*/1};
    std::byte buf[16]{};

    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    sluice::async::detail::SlotHandle h = rh.value();
    // Borrow metadata is written at prepare but NOT yet active.
    SLUICE_CHECK(arena.prepare(h, sluice::async::detail::OperationKind::read,
                               sluice::async::detail::BorrowMetadata{7, buf, 16})
                     .has_value());
    // Every reaped slot must carry a publication binding (review C2 — reap
    // fail-fasts on a missing binding). Install a no-op binding. A non-null
    // completion is required (CodeRabbit finding — the thunk dereferences it);
    // the no-op lambda never does, so a dummy address keeps the binding valid.
    static int dummy_completion = 0;
    auto br = arena.install_publication_binding(
        h, &dummy_completion, 16, [](void*, const sluice::async::detail::TerminalResult&) noexcept {});
    SLUICE_CHECK(br.has_value());
    SLUICE_CHECK(!arena.borrow_active(h.slot));
    // Borrow begins at commit (I7).
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.borrow_active(h.slot));

    // Terminal + enqueue (acks the pin) + reap: borrow ends at completion-ready
    // publication (I18).
    SLUICE_CHECK(arena.record_terminal(h, sluice::async::detail::TerminalResult::ok_bytes(16)));
    SLUICE_CHECK(arena.enqueue(h) == sluice::async::detail::EnqueueOutcome::terminal_noop);
    struct NoopSink : sluice::async::detail::SynchronousReadySink {
        void on_ready(sluice::async::detail::ReadyEvent) noexcept override {}
    } sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(!arena.borrow_active(h.slot));       // borrow ended
    SLUICE_CHECK(arena.slot_in_use() == 1);           // slot still bound until release

    arena.release_completed_binding(h);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- Slice 6: reap preserves backend-known (terminal-winner) order -----------
// PR #63 review finding #3 / ADR Decision 9: "Reap must preserve identity and
// backend-known order." The prior reap iterated the slot array by index
// (for i in capacity), so delivery order was physical slot order, not the order
// terminals were recorded. The arena now threads backend_ready slots onto a
// ready-ring in terminal-winner order; reap pops from the head. This test fails
// on a slot-index scan and passes only on the ready-ring.
//
// Sequence: the free list is LIFO, so the FIRST two reserves take slot 0 then
// slot 1. We record slot 1's terminal FIRST, then slot 0's. A slot-index reap
// would deliver slot 0 (D) before slot 1 (B); the ready-ring delivers B (the
// first terminal winner) before D.
SLUICE_TEST_CASE(arena_reap_preserves_terminal_winner_order) {
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(1), /*request_capacity=*/2};

    auto ra = arena.reserve();
    auto rb = arena.reserve();
    SLUICE_CHECK(ra.has_value() && rb.has_value());
    sluice::async::detail::SlotHandle a = ra.value();
    sluice::async::detail::SlotHandle b = rb.value();
    // LIFO free list: a is slot 0, b is slot 1 (both generation 0).
    SLUICE_CHECK(a.slot.value == 0);
    SLUICE_CHECK(b.slot.value == 1);

    auto bind = [](sluice::async::detail::RequestArena& ar,
                   sluice::async::detail::SlotHandle h) {
        SLUICE_CHECK(ar.prepare(h, sluice::async::detail::OperationKind::read, {}).has_value());
        // A non-null completion is required (CodeRabbit finding — the thunk
        // dereferences it). The no-op lambda never dereferences, so a thread-
        // local dummy address keeps the binding valid across the two binds.
        static thread_local int dummy_completion = 0;
        SLUICE_CHECK(ar
                         .install_publication_binding(
                             h, &dummy_completion, 0,
                             [](void*, const sluice::async::detail::TerminalResult&) noexcept {})
                         .has_value());
        SLUICE_CHECK(ar.commit(h).has_value());
    };
    bind(arena, a);
    bind(arena, b);

    // Record b's terminal FIRST, then a's. The ready-ring must deliver b, a.
    SLUICE_CHECK(
        arena.record_terminal(b, sluice::async::detail::TerminalResult::ok_bytes(2)));
    SLUICE_CHECK(
        arena.record_terminal(a, sluice::async::detail::TerminalResult::ok_bytes(1)));
    // Ack both enqueue pins (terminal_noop: backend_ready observed).
    SLUICE_CHECK(arena.enqueue(b) == sluice::async::detail::EnqueueOutcome::terminal_noop);
    SLUICE_CHECK(arena.enqueue(a) == sluice::async::detail::EnqueueOutcome::terminal_noop);

    // Capture the delivery order via the slot index in each ReadyEvent key.
    struct OrderSink : sluice::async::detail::SynchronousReadySink {
        void on_ready(sluice::async::detail::ReadyEvent e) noexcept override {
            order[delivered++] = e.key.slot.value;
        }
        std::uint32_t order[2]{};
        std::size_t delivered = 0;
    } sink;
    SLUICE_CHECK(arena.reap(sink) == 2);
    SLUICE_CHECK_MSG(sink.delivered == 2, "reap delivered both backend_ready slots");
    // b (slot 1) won the terminal first -> it must be delivered FIRST.
    SLUICE_CHECK_MSG(sink.order[0] == 1,
                     "reap must deliver slot 1 (first terminal winner) before slot 0");
    SLUICE_CHECK_MSG(sink.order[1] == 0, "reap must deliver slot 0 second");

    arena.release_completed_binding(a);
    arena.release_completed_binding(b);
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

// ---- C2b row 4: generation advances exactly +1 on BOTH release authorities --
// I6: both the pre-commit rollback authority and the completed-binding release
// authority increment the slot generation by EXACTLY one, and the increment is
// observable (generation_of) immediately after release — BEFORE the slot can
// re-enter a reserve — so a stale key can never collide with the next
// occupant. The rollback half is asserted here next to the completed-binding
// half so the two authorities are pinned side by side.
SLUICE_TEST_CASE(arena_generation_plus_one_on_both_release_authorities) {
    sluice::async::detail::RequestArena arena{
        sluice::async::detail::ContextIdentity::for_testing(1), /*request_capacity=*/1};

    // Authority 1: pre-commit rollback (reserved never accepted).
    auto r1 = arena.reserve();
    SLUICE_CHECK(r1.has_value());
    const auto gen_before_rollback = arena.generation_of(r1.value().slot);
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(r1.value()).has_value());
    // The increment is visible immediately after release, before any re-reserve.
    SLUICE_CHECK(arena.generation_of(r1.value().slot).value ==
                 gen_before_rollback.value + 1);

    // Authority 2: completed-binding release (the caller handshake after reap).
    auto r2 = arena.reserve();
    SLUICE_CHECK(r2.has_value());
    sluice::async::detail::SlotHandle h = r2.value();
    SLUICE_CHECK(h.generation.value == gen_before_rollback.value + 1);
    SLUICE_CHECK(arena.prepare(h, sluice::async::detail::OperationKind::read, {}).has_value());
    static int dummy_completion = 0;
    SLUICE_CHECK(arena
                     .install_publication_binding(
                         h, &dummy_completion, 0,
                         [](void*, const sluice::async::detail::TerminalResult&) noexcept {})
                     .has_value());
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.record_terminal(h, sluice::async::detail::TerminalResult::ok_bytes(1)));
    SLUICE_CHECK(arena.enqueue(h) == sluice::async::detail::EnqueueOutcome::terminal_noop);
    struct NoopSink : sluice::async::detail::SynchronousReadySink {
        void on_ready(sluice::async::detail::ReadyEvent) noexcept override {}
    } sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    const auto gen_before_release = arena.generation_of(h.slot);
    arena.release_completed_binding(h);
    // Exactly +1, visible before the slot can become visible to a new reserve.
    SLUICE_CHECK(arena.generation_of(h.slot).value == gen_before_release.value + 1);
    // The next reserve observes the incremented generation.
    auto r3 = arena.reserve();
    SLUICE_CHECK(r3.has_value());
    SLUICE_CHECK(r3.value().generation.value == gen_before_release.value + 1);
    SLUICE_CHECK(arena.rollback_reserved_or_prepared(r3.value()).has_value());
    SLUICE_CHECK(arena.slot_in_use() == 0);
}
