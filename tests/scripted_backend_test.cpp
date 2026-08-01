// ScriptedAsyncBackend self-tests.
//
// Proves the backend + controller control plane is correct before using it to
// test higher-level components. Covers:
//   1. Multiple pending reads
//   2. Out-of-order completion
//   3. Short read with data
//   4. Short write
//   5. Completion error (read, write, sync)
//   6. Submit failure injection
//   7. Double-completion rejection
//   8. Invalid completion kind rejection
//   9. Multi-threaded submit + control (controller survives backend destruction)
//  10. No-pending check
//  11. Cancel
//  12. Pending inspection
//  13. Sync operations
//  14. Max outstanding statistics
//  15. Captured write bytes
//  16. Outstanding() includes staged completions
//  17. max_outstanding_reads counts only reads
//
// IMPORTANT: AsyncIoContext's destructor fail-fasts if any Completion is
// outstanding. Every test MUST complete all pending operations before the
// context goes out of scope. The Cleanup RAII helper must be declared AFTER
// all Completion objects so it is destroyed first (reverse construction order).
//
// All test-thread control goes through ScriptedBackendController — never a
// backend raw pointer.

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
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// RAII helper: drives the controller to complete any leftover pending ops and
// polls the context on destruction. MUST be declared AFTER all Completion
// objects so it is destroyed first.
struct Cleanup {
    ScriptedBackendController* ctrl;
    AsyncIoContext* ctx;

    ~Cleanup() {
        ctrl->complete_all_for_cleanup();
        // Drain staged results into Completions.
        ctx->poll();
    }
};

// ---------------------------------------------------------------------------
// Slice 1: multiple pending reads
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_multiple_pending_reads) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{&ctrl, &ctx};  // AFTER Completions

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf[2], 8, 8192}, c3).has_value());

    SLUICE_CHECK(ctrl.pending_read_count() == 3);
    SLUICE_CHECK(ctrl.max_outstanding_reads() == 3);
    SLUICE_CHECK(ctrl.pending_count() == 3);

    auto ops = ctrl.pending_operations();
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
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2);
    (void)ctx.submit_read(ReadOp{0, buf[2], 8, 8192}, c3);

    ctrl.complete_bytes(3, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c3.ready());
    SLUICE_CHECK(c3.result().value() == 8);
    SLUICE_CHECK(!c1.ready());
    SLUICE_CHECK(!c2.ready());
    SLUICE_CHECK(ctrl.pending_count() == 2);

    ctrl.complete_bytes(1, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c1.ready());
    SLUICE_CHECK(c1.result().value() == 8);
    SLUICE_CHECK(ctrl.pending_count() == 1);

    ctrl.complete_bytes(2, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c2.ready());
    SLUICE_CHECK(c2.result().value() == 8);
    SLUICE_CHECK(ctrl.pending_count() == 0);
}

// ---------------------------------------------------------------------------
// Slice 3: short read with data
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_short_read_with_data) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[16]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 16, 0}, c);

    std::byte data[] = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    ctrl.complete_read_with_data(1, data, 3);

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
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[16]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_write(WriteOp{0, buf, 16, 0}, c);
    ctrl.complete_bytes(1, 7);

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 7);
}

// ---------------------------------------------------------------------------
// Slice 5: completion error (read, write, sync)
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_read_error) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    ctrl.complete_error(1, IoError{IoError::Code::eof});

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::eof);
}

SLUICE_TEST_CASE(scripted_write_error) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_write(WriteOp{0, buf, 8, 0}, c);
    ctrl.complete_error(1, IoError{IoError::Code::no_space});

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::no_space);
}

SLUICE_TEST_CASE(scripted_sync_error) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    Completion<void> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);
    ctrl.complete_sync_error(1, IoError{IoError::Code::backend_error});

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::backend_error);
}

// ---------------------------------------------------------------------------
// Slice 6: submit failure injection
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_submit_failure_by_kind) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c1, c2;
    Cleanup cleanup{&ctrl, &ctx};

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c1).has_value());

    ctrl.fail_next_submit(OpKind::read, IoError{IoError::Code::backend_error});

    auto r = ctx.submit_read(ReadOp{0, buf, 8, 4096}, c2);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(ctrl.pending_count() == 1);
    SLUICE_CHECK(ctrl.pending_read_count() == 1);
    SLUICE_CHECK(c2.idle());
}

SLUICE_TEST_CASE(scripted_submit_failure_by_number) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{&ctrl, &ctx};

    ctrl.fail_submit_number(2, IoError{IoError::Code::backend_error});

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c1).has_value());

    auto r2 = ctx.submit_read(ReadOp{0, buf, 8, 4096}, c2);
    SLUICE_CHECK(!r2.has_value());

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 8192}, c3).has_value());
    SLUICE_CHECK(ctrl.pending_count() == 2);
}

// ---------------------------------------------------------------------------
// Slice 7: double-completion rejection
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_double_completion_rejected) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    ctrl.complete_bytes(1, 8);
    ctx.poll();

    bool threw = false;
    try {
        ctrl.complete_bytes(1, 8);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Slice 8: invalid completion kind rejection
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_invalid_completion_kind_rejected) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    Completion<void> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);

    bool threw = false;
    try {
        ctrl.complete_bytes(1, 0);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

SLUICE_TEST_CASE(scripted_complete_bytes_too_large) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);

    bool threw = false;
    try {
        ctrl.complete_bytes(1, 100);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Slice 8b: helper-type validation (Phase 0)
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_complete_eof_rejects_write) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_write(WriteOp{0, buf, 8, 0}, c);

    bool threw = false;
    try {
        ctrl.complete_eof(1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

SLUICE_TEST_CASE(scripted_complete_sync_error_rejects_read_write) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);

    bool threw = false;
    try {
        ctrl.complete_sync_error(1, IoError{IoError::Code::backend_error});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

SLUICE_TEST_CASE(scripted_submit_failure_op_not_outstanding) {
    // A submit failure must NOT register the op as outstanding.
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c1, c2;
    Cleanup cleanup{&ctrl, &ctx};

    ctrl.fail_next_submit(OpKind::read, IoError{IoError::Code::backend_error});
    auto r1 = ctx.submit_read(ReadOp{0, buf, 8, 0}, c1);
    SLUICE_CHECK(!r1.has_value());
    SLUICE_CHECK(ctrl.pending_count() == 0);  // failed submit: not outstanding
    SLUICE_CHECK(c1.idle());

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 4096}, c2).has_value());
    SLUICE_CHECK(ctrl.pending_count() == 1);
}

// ---------------------------------------------------------------------------
// Slice 9: multi-threaded submit + control (controller survives backend
// destruction — the core Phase 0 property)
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_multi_threaded_submit_and_control) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto ctrl = pair.controller;  // copy: survives ctx destruction

    std::atomic<bool> submit_done{false};
    std::byte buf[8]{};
    Completion<std::size_t> c;

    std::thread submitter([&] {
        (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
        submit_done.store(true, std::memory_order::release);
    });

    SLUICE_CHECK(ctrl.wait_until_pending(1) == WaitStatus::ready);
    SLUICE_CHECK(ctrl.pending_count() == 1);

    ctrl.complete_bytes(1, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());

    submitter.join();
    SLUICE_CHECK(submit_done.load());
}

// Slice 9b: the controller is safe AFTER the backend is destroyed by another
// thread, and waiting returns `closed` rather than `ready`.
SLUICE_TEST_CASE(scripted_controller_safe_after_backend_destruction) {
    auto pair = make_scripted_backend();
    auto ctrl = pair.controller;  // held across backend destruction

    {
        AsyncIoContext ctx(std::move(pair.backend));
        std::byte buf[8]{};
        Completion<std::size_t> c;
        (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
        SLUICE_CHECK(ctrl.pending_count() == 1);
        // Drain so the context destructor does not fail-fast on outstanding.
        ctrl.complete_bytes(1, 8);
        ctx.poll();
        SLUICE_CHECK(ctrl.pending_count() == 0);
    }  // ctx (and backend) destroyed here

    // After destruction the controller must be safe: no UAF, no hang.
    SLUICE_CHECK(ctrl.closed());

    // Waiting returns closed, never ready/timeout.
    SLUICE_CHECK(ctrl.wait_until_pending(1) == WaitStatus::closed);
    SLUICE_CHECK(ctrl.wait_until_pending_for(1, std::chrono::milliseconds(10)) ==
                 WaitStatus::closed);

    // Completion control throws ScriptedBackendClosed, not UAF.
    bool threw_closed = false;
    try {
        ctrl.complete_bytes(1, 0);
    } catch (const ScriptedBackendClosed&) {
        threw_closed = true;
    } catch (const std::runtime_error&) {
        threw_closed = true;  // ScriptedBackendClosed derives from runtime_error
    }
    SLUICE_CHECK(threw_closed);
}

// ---------------------------------------------------------------------------
// Slice 10: no-pending check
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_expect_no_pending_passes_when_empty) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;
    Cleanup cleanup{&ctrl, &ctx};

    ctrl.expect_no_outstanding();  // renamed from expect_no_pending
}

SLUICE_TEST_CASE(scripted_expect_no_pending_fails_when_pending) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);

    bool threw = false;
    try {
        ctrl.expect_no_outstanding();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Slice 11: cancel
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_cancel_pending_op) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    SLUICE_CHECK(ctrl.pending_count() == 1);

    ctx.cancel(c);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(ctrl.pending_count() == 0);
}

// ---------------------------------------------------------------------------
// Slice 12: pending inspection helpers
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_find_by_offset) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2);
    (void)ctx.submit_write(WriteOp{0, buf[2], 8, 8192}, c3);

    auto r0 = ctrl.find_read_by_offset(0);
    SLUICE_CHECK(r0.has_value());
    SLUICE_CHECK(*r0 == 1);

    auto r4096 = ctrl.find_read_by_offset(4096);
    SLUICE_CHECK(r4096.has_value());
    SLUICE_CHECK(*r4096 == 2);

    auto missing = ctrl.find_read_by_offset(9999);
    SLUICE_CHECK(!missing.has_value());

    auto w8192 = ctrl.find_write_by_offset(8192);
    SLUICE_CHECK(w8192.has_value());
    SLUICE_CHECK(*w8192 == 3);
}

// ---------------------------------------------------------------------------
// Slice 13: sync operations
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_sync_data_success) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    Completion<void> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);
    SLUICE_CHECK(ctrl.pending_sync_count() == 1);

    ctrl.complete_sync_success(1);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().has_value());
}

SLUICE_TEST_CASE(scripted_sync_all_success) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    Completion<void> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_sync_all(SyncAllOp{0}, c);
    SLUICE_CHECK(ctrl.pending_sync_count() == 1);

    ctrl.complete_sync_success(1);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().has_value());
}

// ---------------------------------------------------------------------------
// Slice 14: max outstanding statistics
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_max_outstanding_tracks_peak) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2);
    (void)ctx.submit_read(ReadOp{0, buf[2], 8, 8192}, c3);

    SLUICE_CHECK(ctrl.max_outstanding_reads() == 3);
    SLUICE_CHECK(ctrl.max_outstanding_total() == 3);

    ctrl.complete_bytes(1, 8);
    ctx.poll();
    SLUICE_CHECK(ctrl.pending_count() == 2);
    SLUICE_CHECK(ctrl.max_outstanding_reads() == 3);
    SLUICE_CHECK(ctrl.max_outstanding_total() == 3);
}

// ---------------------------------------------------------------------------
// Slice 14b: staged read statistics remain correct (Phase 0)
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_staged_read_statistics_correct) {
    // Submit 3 reads; stage one; verify max_reads still reflects the staged op.
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, buf[1], 8, 4096}, c2);
    (void)ctx.submit_read(ReadOp{0, buf[2], 8, 8192}, c3);
    SLUICE_CHECK(ctrl.max_outstanding_reads() == 3);

    // Stage c1's result (not yet polled). It must remain outstanding.
    ctrl.complete_bytes(1, 8);
    SLUICE_CHECK(ctrl.pending_count() == 3);   // staged still counted
    SLUICE_CHECK(ctrl.pending_read_count() == 3);
    SLUICE_CHECK(ctrl.max_outstanding_reads() == 3);  // peak unchanged

    // Now poll: c1 leaves outstanding.
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(ctrl.pending_count() == 2);
    SLUICE_CHECK(ctrl.pending_read_count() == 2);
    SLUICE_CHECK(ctrl.max_outstanding_reads() == 3);  // peak unchanged
}

// ---------------------------------------------------------------------------
// Slice 15: captured write bytes
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_captured_write_bytes) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_write(WriteOp{0, buf, 4, 0}, c);

    auto captured = ctrl.captured_write_bytes(1);
    SLUICE_CHECK(captured.size() == 4);
    SLUICE_CHECK(captured[0] == std::byte{1});
    SLUICE_CHECK(captured[1] == std::byte{2});
    SLUICE_CHECK(captured[2] == std::byte{3});
    SLUICE_CHECK(captured[3] == std::byte{4});

    ctrl.complete_bytes(1, 4);
    ctx.poll();

    bool threw = false;
    try {
        ctrl.captured_write_bytes(1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Slice 16: outstanding() includes staged completions
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_outstanding_includes_staged) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    SLUICE_CHECK(ctrl.pending_count() == 1);

    // Stage the completion (moves op to stage=staged; stays in the map).
    ctrl.complete_bytes(1, 8);

    // Outstanding MUST still be 1: the Completion has not been made ready yet.
    SLUICE_CHECK(ctrl.pending_count() == 1);
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(c.outstanding());

    // After poll, the Completion becomes ready and outstanding drops to 0.
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(ctrl.pending_count() == 0);
    SLUICE_CHECK(c.ready());
}

SLUICE_TEST_CASE(scripted_outstanding_staged_void) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    Completion<void> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);
    SLUICE_CHECK(ctrl.pending_count() == 1);

    ctrl.complete_sync_success(1);
    // Staged but not polled: still outstanding.
    SLUICE_CHECK(ctrl.pending_count() == 1);
    SLUICE_CHECK(!c.ready());

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(ctrl.pending_count() == 0);
    SLUICE_CHECK(c.ready());
}

// ---------------------------------------------------------------------------
// Slice 17: max_outstanding_reads counts only reads (not writes)
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_max_reads_excludes_writes) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[3][8]{};
    Completion<std::size_t> c1, c2, c3;
    Cleanup cleanup{&ctrl, &ctx};

    // Submit 1 read + 2 writes.
    (void)ctx.submit_read(ReadOp{0, buf[0], 8, 0}, c1);
    (void)ctx.submit_write(WriteOp{0, buf[1], 8, 0}, c2);
    (void)ctx.submit_write(WriteOp{0, buf[2], 8, 4096}, c3);

    // max_outstanding_reads must be 1 (only the read), NOT 3.
    SLUICE_CHECK(ctrl.pending_read_count() == 1);
    SLUICE_CHECK(ctrl.pending_write_count() == 2);
    SLUICE_CHECK(ctrl.max_outstanding_reads() == 1);
    SLUICE_CHECK(ctrl.max_outstanding_total() == 3);
}

// ---------------------------------------------------------------------------
// Slice 18: cancel is idempotent (exactly-once terminal result).
//
// The Runtime may call cancel() more than once for the same outstanding
// Completion (e.g. a defensive cleanup path). Staging a canceled result on
// every call would complete_with() the same Completion twice — a terminal-
// result contract violation. Only the FIRST cancel on a still-pending op may
// stage; later cancels are no-ops.
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(scripted_double_cancel_size_op_is_idempotent) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    SLUICE_CHECK(ctrl.pending_count() == 1);

    // Cancel twice BEFORE any poll.
    ctx.cancel(c);
    ctx.cancel(c);

    // A single poll applies exactly ONE canceled result.
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(ctrl.pending_count() == 0);

    // A second poll applies nothing more (no duplicate terminal result).
    SLUICE_CHECK(ctx.poll() == 0);
    SLUICE_CHECK(ctrl.pending_count() == 0);
}

SLUICE_TEST_CASE(scripted_double_cancel_void_op_is_idempotent) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    Completion<void> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_sync_data(SyncDataOp{0}, c);
    SLUICE_CHECK(ctrl.pending_count() == 1);

    ctx.cancel(c);
    ctx.cancel(c);

    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(ctrl.pending_count() == 0);
    SLUICE_CHECK(ctx.poll() == 0);
}

SLUICE_TEST_CASE(scripted_cancel_after_complete_is_noop) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);

    // Complete via the controller first (normal terminal result).
    ctrl.complete_bytes(1, 8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 8);
    SLUICE_CHECK(ctrl.pending_count() == 0);

    // A late cancel must be a no-op: the op is already terminal and no longer
    // in the outstanding map. It must NOT stage a second result.
    ctx.cancel(c);
    SLUICE_CHECK(ctx.poll() == 0);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 8);  // original result preserved
    SLUICE_CHECK(ctrl.pending_count() == 0);
}

SLUICE_TEST_CASE(scripted_cancel_never_submitted_is_noop) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    // The Completion was never submitted: cancel must be a silent no-op and
    // must not make the Completion ready.
    ctx.cancel(c);
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(ctx.poll() == 0);
    SLUICE_CHECK(ctrl.pending_count() == 0);
}

SLUICE_TEST_CASE(scripted_cancel_then_teardown_clean) {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;

    std::byte buf[8]{};
    Completion<std::size_t> c;
    Cleanup cleanup{&ctrl, &ctx};

    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    ctx.cancel(c);
    ctx.cancel(c);  // idempotent
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());

    // Nothing outstanding at teardown: the context destructor (and the
    // backend destructor's Debug invariant) must not fail-fast.
    SLUICE_CHECK(ctrl.pending_count() == 0);
    ctrl.expect_no_outstanding();
}

SLUICE_MAIN()
