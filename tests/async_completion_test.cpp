// Tests for the async foundation (sluice-CORE-017). Exercises Completion<T>
// lifecycle, AsyncIoContext submit/poll/wait plumbing, and AsyncStats against
// the 017 SyncBackend (completes synchronously at poll time, no kernel/threads).
//
// Real backends (Fake/ThreadPool/Uring) are tested in their own jobs (019/020A/
// 020B). These tests are the foundation floor: if they fail, every later async
// test is meaningless.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/sync_backend.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstddef>

using namespace sluice::async;
using sluice::Result;

// ---- Slice 1: Completion<T> lifecycle --------------------------------------

SLUICE_TEST_CASE(completion_starts_idle) {
    Completion<std::size_t> c;
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(!c.outstanding());
    SLUICE_CHECK(!c.ready());
}

SLUICE_TEST_CASE(completion_mark_outstanding_then_complete_with_value) {
    Completion<std::size_t> c;
    c.mark_outstanding();
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(!c.ready());
    c.complete_with(Result<std::size_t>{std::size_t{42}});
    SLUICE_CHECK(c.ready());
    auto r = c.result();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 42);
}

SLUICE_TEST_CASE(completion_complete_with_error_surfaces_via_result) {
    Completion<std::size_t> c;
    c.mark_outstanding();
    c.complete_with(make_unexpected<std::size_t>(sluice::IoError{sluice::IoError::Code::eof}));
    SLUICE_CHECK(c.ready());
    auto r = c.result();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(completion_reset_returns_to_idle_and_reusable) {
    Completion<std::size_t> c;
    c.mark_outstanding();
    c.complete_with(Result<std::size_t>{std::size_t{7}});
    SLUICE_CHECK(c.ready());
    c.reset();
    SLUICE_CHECK(c.idle());
    // Reusable for a new op.
    c.mark_outstanding();
    c.complete_with(Result<std::size_t>{std::size_t{99}});
    SLUICE_CHECK(c.result().value() == 99);
}

// ---- Slice 2: result() before ready is a contract violation (L9) -----------
// ADR L9: result() before ready is a debug assertion failure. We do NOT call
// result() on an idle/outstanding Completion in a passing test (it would abort
// the debug harness). The release-mode contract (returns invalid_state) cannot
// be exercised in a debug build. The assertion itself is the debug guard; this
// slice documents that the lifecycle is enforced rather than testing the abort.
// (Sanitizer builds also run in debug and would trip the assert — by design.)

// ---- Slice 3: AsyncIoContext submit/poll with SyncBackend ------------------

SLUICE_TEST_CASE(submit_then_poll_completes_op) {
    auto backend = std::make_unique<SyncBackend>();
    SyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte buf[16]{};
    Completion<std::size_t> c;
    auto r = ctx.submit_read(ReadOp{0, buf, 16, 0}, c);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(raw->outstanding() == 1);

    std::size_t n = ctx.poll();
    SLUICE_CHECK(n == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 16);  // SyncBackend completes with len
    SLUICE_CHECK(ctx.outstanding() == 0);
}

SLUICE_TEST_CASE(submit_write_then_poll_completes) {
    AsyncIoContext ctx(std::make_unique<SyncBackend>());
    std::byte buf[32]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_write(WriteOp{0, buf, 32, 0}, c).has_value());
    SLUICE_CHECK(ctx.wait_one().value() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 32);
}

SLUICE_TEST_CASE(submit_sync_ops_complete_void) {
    AsyncIoContext ctx(std::make_unique<SyncBackend>());
    Completion<void> cd, ca;
    SLUICE_CHECK(ctx.submit_sync_data(SyncDataOp{0}, cd).has_value());
    SLUICE_CHECK(ctx.submit_sync_all(SyncAllOp{0}, ca).has_value());
    SLUICE_CHECK(ctx.poll() == 2);
    SLUICE_CHECK(cd.ready());
    SLUICE_CHECK(ca.ready());
    SLUICE_CHECK(cd.result().has_value());
    SLUICE_CHECK(ca.result().has_value());
}

// ---- Slice 4: submit into non-idle Completion -> invalid_state (L8) --------

SLUICE_TEST_CASE(submit_into_outstanding_completion_is_invalid_state) {
    AsyncIoContext ctx(std::make_unique<SyncBackend>());
    std::byte buf[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    // c is now outstanding; a second submit must reject (L8).
    auto r = ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::invalid_state);
    // Drain before the context is destroyed (L11: no outstanding at destroy).
    ctx.poll();
}

// ---- Slice 5: poll on empty context reaps nothing --------------------------

SLUICE_TEST_CASE(poll_with_no_outstanding_returns_zero) {
    AsyncIoContext ctx(std::make_unique<SyncBackend>());
    SLUICE_CHECK(ctx.poll() == 0);
    SLUICE_CHECK(ctx.wait_one().value() == 0);
}

// ---- Slice 6: multiple ops complete in one poll ----------------------------

SLUICE_TEST_CASE(multiple_ops_complete_in_one_poll) {
    AsyncIoContext ctx(std::make_unique<SyncBackend>());
    std::byte b1[4], b2[4], b3[4];
    Completion<std::size_t> c1, c2, c3;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b1, 4, 0}, c1).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b2, 4, 4}, c2).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b3, 4, 8}, c3).has_value());
    SLUICE_CHECK(ctx.outstanding() == 3);

    SLUICE_CHECK(ctx.poll() == 3);
    SLUICE_CHECK(c1.ready() && c2.ready() && c3.ready());
    SLUICE_CHECK(c1.result().value() == 4);
    SLUICE_CHECK(c2.result().value() == 4);
    SLUICE_CHECK(c3.result().value() == 4);
}

// ---- Slice 7: AsyncStats counters increment correctly ----------------------

SLUICE_TEST_CASE(async_stats_increment_on_submit_poll_wait) {
    sluice::AsyncStats s;
    AsyncIoContext ctx(std::make_unique<SyncBackend>(), &s);
    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 8, 0}, c).has_value());

    SLUICE_CHECK(s.submit_calls == 1);
    SLUICE_CHECK(s.submitted_ops == 1);
    SLUICE_CHECK(s.max_outstanding == 1);

    ctx.poll();
    SLUICE_CHECK(s.poll_calls == 1);
    SLUICE_CHECK(s.completed_ops == 1);

    // Submit-into-outstanding rejection increments queue_full_retries.
    Completion<std::size_t> c2;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 8, 0}, c2).has_value());
    // Now submit into c2 again — outstanding -> rejected.
    auto rej = ctx.submit_read(ReadOp{0, b, 8, 0}, c2);
    SLUICE_CHECK(!rej.has_value());
    SLUICE_CHECK(s.queue_full_retries >= 1);
    ctx.poll();  // drain so destructor is clean
}

// ---- Slice 8: cancel marks ready with canceled (minimal model) -------------

SLUICE_TEST_CASE(cancel_outstanding_op_completes_canceled) {
    AsyncIoContext ctx(std::make_unique<SyncBackend>());
    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 8, 0}, c).has_value());
    ctx.cancel(c);
    SLUICE_CHECK(c.ready());
    auto r = c.result();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::canceled);
}

SLUICE_MAIN()
