// ScriptedAsyncBackend self-tests.
//
// Proves the backend itself is correct before using it to test higher-level
// components. Covers:
//   1. Multiple pending reads
//   2. Out-of-order completion
//   3. Short read with data
//   4. Short write
//   5. Completion error (read, write, sync)
//   6. Submit failure injection
//   7. Double-completion rejection
//   8. Invalid completion kind rejection
//   9. Multi-threaded submit + control
//  10. No-pending check
//  11. Cancel
//  12. Pending inspection
//  13. Sync operations
//  14. Max outstanding statistics
//
// IMPORTANT: AsyncIoContext's destructor fail-fasts if any Completion is
// outstanding. Every test MUST complete all pending operations before the
// context goes out of scope. The Cleanup RAII helper must be declared AFTER
// all Completion objects so it is destroyed first (reverse construction order).

#include "harness.hpp"

#include "support/scripted_async_backend.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// RAII helper: drains all pending ops and polls the context on destruction.
// MUST be declared AFTER all Completion objects so it is destroyed first.
struct Cleanup {
    ScriptedAsyncBackend* backend;
    AsyncIoContext* ctx;

    ~Cleanup() {
        while (backend->pending_count() > 0) {
            auto ops = backend->pending_operations();
            for (auto& op : ops) {
                switch (op.kind) {
                case OpKind::read:
                case OpKind::write:
                    backend->complete_bytes(op.id, op.length);
                    break;
                case OpKind::sync_data:
                case OpKind::sync_all:
                    backend->complete_sync_success(op.id);
                    break;
                }
            }
        }
        ctx->poll();
    }
};

// ---------------------------------------------------------------------------
// Slice 1: multiple pending reads
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_multiple_pending_reads) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{raw, &ctx};  // AFTER Completions

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf[2], 8, 8192}, c3).has_value());

    SLUICE_CHECK(raw->pending_read_count() == 3);
    SLUICE_CHECK(raw->max_outstanding_reads() == 3);
    SLUICE_CHECK(raw->pending_count() == 3);

    auto ops = raw->pending_operations();
    SLUICE_CHECK(ops.size() == 3);
    SLUICE_CHECK(ops[0].id == 1);
    SLUICE_CHECK(ops[1].id == 2);
    SLUICE_CHECK(ops[2].id == 3);
    SLUICE_CHECK(ops[0].kind == OpKind::read);
    SLUICE_CHECK(ops[0].offset == 0);
    SLUICE_CHECK(ops[0].length == 8);
    SLUICE_CHECK(ops[1].offset == 4096);
    SLUICE_CHECK(ops[2].offset == 8192);
}

// ---------------------------------------------------------------------------
// Slice 2: out-of-order completion
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_out_of_order_completion) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2);
    (void)ctx.submit_read(ReadOp{0, buf[2], 8, 8192}, c3);

    raw->complete_bytes(3, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c3.ready());
    SLUICE_CHECK(c3.result().value() == 8);
    SLUICE_CHECK(!c1.ready());
    SLUICE_CHECK(!c2.ready());
    SLUICE_CHECK(raw->pending_count() == 2);

    raw->complete_bytes(1, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c1.ready());
    SLUICE_CHECK(c1.result().value() == 8);
    SLUICE_CHECK(raw->pending_count() == 1);

    raw->complete_bytes(2, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c2.ready());
    SLUICE_CHECK(c2.result().value() == 8);
    SLUICE_CHECK(raw->pending_count() == 0);
}

// ---------------------------------------------------------------------------
// Slice 3: short read with data
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_short_read_with_data) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[16]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 16, 0}, c);

    std::byte data[] = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    raw->complete_read_with_data(1, data, 3);

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 3);
    SLUICE_CHECK(buf[0] == std::byte{0xAA});
    SLUICE_CHECK(buf[1] == std::byte{0xBB});
    SLUICE_CHECK(buf[2] == std::byte{0xCC});
}

// ---------------------------------------------------------------------------
// Slice 4: short write
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_short_write) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[16]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_write(WriteOp{0, buf, 16, 0}, c);
    raw->complete_bytes(1, 7);

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 7);
}

// ---------------------------------------------------------------------------
// Slice 5: completion error (read, write, sync)
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_read_error) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    raw->complete_error(1, IoError{IoError::Code::eof});

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::eof);
}

SLUICE_TEST_CASE(scripted_write_error) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_write(WriteOp{0, buf, 8, 0}, c);
    raw->complete_error(1, IoError{IoError::Code::no_space});

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::no_space);
}

SLUICE_TEST_CASE(scripted_sync_error) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);
    raw->complete_sync_error(1, IoError{IoError::Code::backend_error});

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::backend_error);
}

// ---------------------------------------------------------------------------
// Slice 6: submit failure injection
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_submit_failure_by_kind) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c1, c2;
    Cleanup cleanup{raw, &ctx};

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c1).has_value());

    raw->fail_next_submit(OpKind::read, IoError{IoError::Code::backend_error});

    auto r = ctx.submit_read(ReadOp{0, buf, 8, 4096}, c2);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(raw->pending_count() == 1);
    SLUICE_CHECK(raw->pending_read_count() == 1);
    SLUICE_CHECK(c2.idle());
}

SLUICE_TEST_CASE(scripted_submit_failure_by_number) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{raw, &ctx};

    raw->fail_submit_number(2, IoError{IoError::Code::backend_error});

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c1).has_value());

    auto r2 = ctx.submit_read(ReadOp{0, buf, 8, 4096}, c2);
    SLUICE_CHECK(!r2.has_value());

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 8192}, c3).has_value());
    SLUICE_CHECK(raw->pending_count() == 2);
}

// ---------------------------------------------------------------------------
// Slice 7: double-completion rejection
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_double_completion_rejected) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    raw->complete_bytes(1, 8);
    ctx.poll();

    bool threw = false;
    try {
        raw->complete_bytes(1, 8);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Slice 8: invalid completion kind rejection
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_invalid_completion_kind_rejected) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);

    bool threw = false;
    try {
        raw->complete_bytes(1, 0);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

SLUICE_TEST_CASE(scripted_complete_bytes_too_large) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);

    bool threw = false;
    try {
        raw->complete_bytes(1, 100);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Slice 9: multi-threaded submit + control
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_multi_threaded_submit_and_control) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::atomic<bool> submit_done{false};
    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    std::thread submitter([&] {
        (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
        submit_done.store(true, std::memory_order::release);
    });

    raw->wait_until_pending(1);
    SLUICE_CHECK(raw->pending_count() == 1);

    raw->complete_bytes(1, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());

    submitter.join();
    SLUICE_CHECK(submit_done.load());
}

// ---------------------------------------------------------------------------
// Slice 10: no-pending check
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_expect_no_pending_passes_when_empty) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    Cleanup cleanup{raw, &ctx};

    raw->expect_no_pending();
}

SLUICE_TEST_CASE(scripted_expect_no_pending_fails_when_pending) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);

    bool threw = false;
    try {
        raw->expect_no_pending();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Slice 11: cancel
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_cancel_pending_op) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    SLUICE_CHECK(raw->pending_count() == 1);

    ctx.cancel(c);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(raw->pending_count() == 0);
}

// ---------------------------------------------------------------------------
// Slice 12: pending inspection helpers
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_find_by_offset) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2);
    (void)ctx.submit_write(WriteOp{0, buf[2], 8, 8192}, c3);

    auto r0 = raw->find_read_by_offset(0);
    SLUICE_CHECK(r0.has_value());
    SLUICE_CHECK(*r0 == 1);

    auto r4096 = raw->find_read_by_offset(4096);
    SLUICE_CHECK(r4096.has_value());
    SLUICE_CHECK(*r4096 == 2);

    auto missing = raw->find_read_by_offset(9999);
    SLUICE_CHECK(!missing.has_value());

    auto w8192 = raw->find_write_by_offset(8192);
    SLUICE_CHECK(w8192.has_value());
    SLUICE_CHECK(*w8192 == 3);
}

// ---------------------------------------------------------------------------
// Slice 13: sync operations
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_sync_data_success) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);
    SLUICE_CHECK(raw->pending_sync_count() == 1);

    raw->complete_sync_success(1);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().has_value());
}

SLUICE_TEST_CASE(scripted_sync_all_success) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_sync_all(SyncAllOp{0}, c);
    SLUICE_CHECK(raw->pending_sync_count() == 1);

    raw->complete_sync_success(1);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().has_value());
}

// ---------------------------------------------------------------------------
// Slice 14: max outstanding statistics
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_max_outstanding_tracks_peak) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2);
    (void)ctx.submit_read(ReadOp{0, buf[2], 8, 8192}, c3);

    SLUICE_CHECK(raw->max_outstanding_reads() == 3);
    SLUICE_CHECK(raw->max_outstanding_total() == 3);

    raw->complete_bytes(1, 8);
    ctx.poll();
    SLUICE_CHECK(raw->pending_count() == 2);
    SLUICE_CHECK(raw->max_outstanding_reads() == 3);
    SLUICE_CHECK(raw->max_outstanding_total() == 3);
}

// ---------------------------------------------------------------------------
// Slice 15: captured write bytes
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_captured_write_bytes) {
    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Completion<std::size_t> c;
    Cleanup cleanup{raw, &ctx};

    (void)ctx.submit_write(WriteOp{0, buf, 4, 0}, c);

    auto captured = raw->captured_write_bytes(1);
    SLUICE_CHECK(captured.size() == 4);
    SLUICE_CHECK(captured[0] == std::byte{1});
    SLUICE_CHECK(captured[1] == std::byte{2});
    SLUICE_CHECK(captured[2] == std::byte{3});
    SLUICE_CHECK(captured[3] == std::byte{4});

    raw->complete_bytes(1, 4);
    ctx.poll();

    bool threw = false;
    try {
        raw->captured_write_bytes(1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

SLUICE_MAIN()