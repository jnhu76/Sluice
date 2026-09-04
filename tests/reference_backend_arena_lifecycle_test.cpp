// Phase B regression: prove the reference backends (FakeAsyncBackend, SyncBackend)
// are actually driven by the bounded RequestArena + five-stage admission + the
// synchronous identity-bearing ReadySink, NOT merely refactored to compile.
//
// Without these assertions the migration in commit 4 would be indistinguishable
// from a no-op rename: every existing test passes whether or not the arena is
// wired. These tests assert the OBSERVABLE consequences of the arena lifecycle:
//
//   - arena_slot_in_use() rises on submit and returns to 0 after reap
//     (the slot-release handshake ran; generation advanced).
//   - arena_capacity_rejections() increments when the bounded arena is full
//     (I8: admission is bounded; oversubscription is impossible).
//   - sink_deliveries() counts exactly-once ReadyEvent delivery per reaped op
//     (Decision 9: synchronous identity-bearing non-escaping reap).
//   - generation advances on slot reuse (I6): two ops on the same Completion
//     observe distinct generations (observed via distinct sink deliveries and a
//     clean reuse lifecycle).
//
// These tests do NOT duplicate the contract suite; they assert the Phase B
// reference lifecycle was actually plumbed through.
#include "harness.hpp"
#include "support/waiter_error_vocabulary_cases.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/sync_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---- SyncBackend: slot_in_use rises on submit, stays bound through reap, and
// returns to 0 only when the caller resets the ready Completion ----------------
// ADR Decision 4/15 + design §9: the slot remains bound (slot_in_use == 1) from
// commit until reset()/ready-destruction releases it. Reap publishes
// completion-ready but does NOT free the slot.
SLUICE_TEST_CASE(sync_arena_slot_in_use_tracks_lifecycle) {
    auto backend = std::make_unique<SyncBackend>();
    SyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    SLUICE_CHECK(raw->arena_slot_in_use() == 1); // reserved -> committed
    SLUICE_CHECK(ctx.outstanding() == 1);        // accepted_outstanding

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    // Reap published completion-ready but did NOT release the slot: it stays
    // bound (capacity accounting) until the caller resets the Completion.
    SLUICE_CHECK(raw->arena_slot_in_use() == 1);
    SLUICE_CHECK(ctx.outstanding() == 0);

    // reset() is the slot-release handshake (generation++ under the leaf domain).
    c.reset();
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
}

// ---- SyncBackend: generation advances across slot reuse (I6) ---------------
// Submit+drain one op on a Completion, reset, reuse the SAME Completion. The
// second op MUST get a fresh generation (the slot was released and re-reserved).
// We observe the lifecycle via sink deliveries (exactly-once per reaped op) and
// a clean reuse (no stale-key interference).
SLUICE_TEST_CASE(sync_arena_slot_reuse_advances_generation) {
    auto backend = std::make_unique<SyncBackend>();
    SyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[4]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 4, 0}, c).has_value());
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.result().value() == 4);
    c.reset();
    SLUICE_CHECK(raw->sink_deliveries() == 1);

    // Reuse the same Completion for a second op: generation advances (I6).
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 4, 0}, c).has_value());
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.result().value() == 4);
    c.reset();
    SLUICE_CHECK(raw->sink_deliveries() == 2); // exactly-once per reaped op
}

// ---- SyncBackend: bounded arena rejects oversubscription (I8) --------------
SLUICE_TEST_CASE(sync_arena_bounded_admission_rejects_full) {
    // Capacity 2: only two ops can be outstanding at once.
    auto backend = std::make_unique<SyncBackend>(2);
    SyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte b[4]{};
    Completion<std::size_t> c1, c2, c3;
    SLUICE_CHECK(raw->arena_capacity_rejections() == 0);

    // Submit two (capacity 2); both accepted.
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 4, 0}, c1).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 4, 0}, c2).has_value());
    // Third must be rejected with would_block: capacity pressure is a retryable
    // admission rejection, NEVER invalid_state (ADR Decision 6/13).
    auto r3 = ctx.submit_read(ReadOp{0, b, 4, 0}, c3);
    SLUICE_CHECK(!r3.has_value());
    SLUICE_CHECK(r3.error().code == IoError::Code::would_block);
    SLUICE_CHECK(c3.idle());  // rejected Completion stays idle (I3)
    SLUICE_CHECK(raw->arena_capacity_rejections() == 1);

    // Drain so the destructor is clean.
    ctx.poll();
    c1.reset();
    c2.reset();
}

// ---- FakeAsyncBackend: slot lifecycle under explicit staging ----------------
SLUICE_TEST_CASE(fake_arena_slot_lifecycle_explicit_staging) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    SLUICE_CHECK(raw->arena_slot_in_use() == 1);
    SLUICE_CHECK(ctx.outstanding() == 1);

    // No staging -> poll reaps nothing; op stays outstanding, slot stays in use.
    SLUICE_CHECK(ctx.poll() == 0);
    SLUICE_CHECK(raw->arena_slot_in_use() == 1);
    SLUICE_CHECK(ctx.outstanding() == 1);

    // Stage + poll: Completion becomes ready; the slot stays bound (slot
    // release is the caller's reset handshake, not part of reap).
    raw->complete_oldest_with_bytes(8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 8);
    SLUICE_CHECK(raw->arena_slot_in_use() == 1);  // still bound until reset
    SLUICE_CHECK(raw->sink_deliveries() == 1);
    c.reset();
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);  // reset released the slot
}

// ---- FakeAsyncBackend: cancel drives the Scheme-B terminal path -------------
// Cancel (pointer-keyed -> SlotHandle) wins the terminal transition under
// Scheme B at cancel() time; poll reaps the canceled Completion. The slot is
// released by the caller's reset, not by reap.
SLUICE_TEST_CASE(fake_arena_cancel_drives_scheme_b_terminal) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    SLUICE_CHECK(raw->arena_slot_in_use() == 1);

    ctx.cancel(c);
    // Cancel won the terminal transition (Scheme B: canceled result stored);
    // the slot is not yet reaped and the Completion is not yet ready.
    SLUICE_CHECK(raw->arena_slot_in_use() == 1); // not yet reaped
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(raw->arena_slot_in_use() == 1); // still bound until reset
    SLUICE_CHECK(raw->sink_deliveries() == 1);
    c.reset();
    SLUICE_CHECK(raw->arena_slot_in_use() == 0); // reset released the slot
}

// ---- FakeAsyncBackend: bounded arena rejects oversubscription (I8) ----------
SLUICE_TEST_CASE(fake_arena_bounded_admission_rejects_full) {
    auto backend = std::make_unique<FakeAsyncBackend>(2);
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte b[4]{};
    Completion<std::size_t> c1, c2, c3;
    SLUICE_CHECK(raw->arena_capacity_rejections() == 0);

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 4, 0}, c1).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 4, 0}, c2).has_value());
    auto r3 = ctx.submit_read(ReadOp{0, b, 4, 0}, c3);
    SLUICE_CHECK(!r3.has_value());
    SLUICE_CHECK(r3.error().code == IoError::Code::would_block);  // capacity, never invalid_state
    SLUICE_CHECK(raw->arena_capacity_rejections() == 1);

    // Drain via cancel so the context destructs cleanly.
    ctx.cancel(c1);
    ctx.cancel(c2);
    ctx.poll();
    c1.reset();
    c2.reset();
}

// S0B-CORRECTIVE-1 W1 — the adjudicated register/cancel error-vocabulary
// split (unbound / cross-context / duplicate / post-reap / stale / no-
// registration), driven through the PUBLIC SyncBackend interface (ops settle
// at the next poll).
SLUICE_TEST_CASE(sync_waiter_error_vocabulary_split) {
    auto rc = waiter_error_vocabulary::run_waiter_error_vocabulary_cases<
        SyncBackend>(
        [] { return std::make_unique<SyncBackend>(); },
        /*fd=*/0,
        [](SyncBackend& b, Completion<std::size_t>& c) {
            return b.poll() == 1 && c.ready();
        });
    if (rc != nullptr) SLUICE_FAIL(rc);
}

SLUICE_MAIN()
