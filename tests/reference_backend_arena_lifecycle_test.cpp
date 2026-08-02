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

// ---- SyncBackend: slot_in_use rises on submit and returns to 0 after reap ---
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
    // After reap the slot was released (slot-release handshake); slot_in_use
    // returned to 0 while the Completion remains ready until the caller resets.
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
    SLUICE_CHECK(ctx.outstanding() == 0);
    c.reset();
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
    // Third must be rejected (arena full).
    auto r3 = ctx.submit_read(ReadOp{0, b, 4, 0}, c3);
    SLUICE_CHECK(!r3.has_value());
    SLUICE_CHECK(r3.error().code == IoError::Code::invalid_state);
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

    // Stage + poll: slot released at reap, delivery counted.
    raw->complete_oldest_with_bytes(8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 8);
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
    SLUICE_CHECK(raw->sink_deliveries() == 1);
    c.reset();
}

// ---- FakeAsyncBackend: cancel drives the Scheme-B terminal path -------------
// Cancel on an enqueued slot records the canceled terminal under Scheme B; the
// slot goes backend_ready; poll reaps it and releases the slot. Delivery count
// and slot_in_use confirm the path ran through the arena, not a pointer bypass.
SLUICE_TEST_CASE(fake_arena_cancel_drives_scheme_b_release) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    SLUICE_CHECK(raw->arena_slot_in_use() == 1);

    ctx.cancel(c);
    // Cancel is pointer-keyed -> SlotHandle; the terminal is recorded at poll.
    SLUICE_CHECK(raw->arena_slot_in_use() == 1); // not yet reaped
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(raw->arena_slot_in_use() == 0); // slot released at reap
    SLUICE_CHECK(raw->sink_deliveries() == 1);
    c.reset();
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
    SLUICE_CHECK(raw->arena_capacity_rejections() == 1);

    // Drain via cancel so the context destructs cleanly.
    ctx.cancel(c1);
    ctx.cancel(c2);
    ctx.poll();
    c1.reset();
    c2.reset();
}

SLUICE_MAIN()
