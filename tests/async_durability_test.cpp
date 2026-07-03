// Tests for async durability ops (sluice-CORE-018B, W4 overlapped durability).
// Exercises sync_data_all/sync_all_all against FakeAsyncBackend (auto mode):
// completion, W4 overlap (a sync op outstanding concurrently with writes on the
// same fd without forcing serialization at submit time), and sync error
// propagation. The ordering-composition contract (P3) is documented structurally:
// to durably persist writes, await the writes' Completions THEN submit the sync.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>
#include <span>

using namespace sluice::async;
using sluice::Result;
using sluice::IoError;

// ---- Slice 1: sync_data_all completes void ---------------------------------

SLUICE_TEST_CASE(sync_data_all_completes_void) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    // Sync ops auto-complete as void success (auto mode non-err/non-eof).
    backend->auto_bytes(0);  // any auto mode makes sync void-succeed
    AsyncIoContext ctx(std::move(backend));
    auto r = sync_data_all(ctx, /*fd=*/0);
    SLUICE_CHECK(r.has_value());
}

// ---- Slice 2: sync_all_all completes void ----------------------------------

SLUICE_TEST_CASE(sync_all_all_completes_void) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_bytes(0);
    AsyncIoContext ctx(std::move(backend));
    auto r = sync_all_all(ctx, 0);
    SLUICE_CHECK(r.has_value());
}

// ---- Slice 3: sync error propagates (EIO / backend_error) ------------------

SLUICE_TEST_CASE(sync_error_propagates) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::no_space});
    AsyncIoContext ctx(std::move(backend));
    auto r = sync_data_all(ctx, 0);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::no_space);
}

// ---- Slice 4: W4 overlap — a sync op can be outstanding concurrently with
// writes on the same fd (no serialization at submit time). ADR §6 P3. --------
SLUICE_TEST_CASE(w4_sync_overlaps_writes_at_submit_time) {
    // Submit a write, then a sync, WITHOUT awaiting the write — both outstanding.
    // The fake holds both; neither is forced to complete before the other.
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[4];
    Completion<std::size_t> cw;
    Completion<void> cs;
    (void)ctx.submit_write(WriteOp{0, buf, 4, 0}, cw);
    (void)ctx.submit_sync_data(SyncDataOp{0}, cs);
    // Both outstanding simultaneously — this is the W4 overlap.
    SLUICE_CHECK(raw->outstanding() == 2);
    SLUICE_CHECK(cw.outstanding());
    SLUICE_CHECK(cs.outstanding());

    // Now complete both (write first, then sync — caller composes order).
    raw->complete_oldest_with_bytes(4);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(cw.ready());
    raw->complete_oldest_sync_ok();
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(cs.ready());
}

// ---- Slice 5: P3 ordering — to durably persist writes, AWAIT writes then
// submit sync. Submitting sync BEFORE writes complete does NOT imply those
// writes are durable. (Structural: the test demonstrates the awaited-then-sync
// sequence is expressible and that out-of-order submit is observable.) --------
SLUICE_TEST_CASE(p3_await_writes_then_sync_is_durable_sequence) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[4];

    // Correct durable sequence: write, await it, THEN sync.
    Completion<std::size_t> cw;
    (void)ctx.submit_write(WriteOp{0, buf, 4, 0}, cw);
    raw->complete_oldest_with_bytes(4);
    (void)ctx.poll();
    SLUICE_CHECK(cw.ready());  // write fully acknowledged before sync submitted

    Completion<void> cs;
    (void)ctx.submit_sync_data(SyncDataOp{0}, cs);
    raw->complete_oldest_sync_ok();
    (void)ctx.poll();
    SLUICE_CHECK(cs.ready());  // sync covers the already-acknowledged write
}

SLUICE_MAIN()
