// Tests for async cancellation (sluice-CORE-021 spike, ADR §7 X2/X3).
//
// ADR §7 minimal model: cancel(completion) REQUESTS cancellation; the op
// completes (exactly-once) at the next poll/wait_one with either its real
// result OR IoError::canceled. This file locks that contract via FakeAsyncBackend
// (where cancel is deterministic) and checks the AsyncStats counters (021 adds
// canceled_ops / completion_errors accounting via attach_stats).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>

using namespace sluice::async;
using sluice::Result;
using sluice::IoError;

// ---- Slice 1: cancel an outstanding read completes it as canceled (X2/X3) ---
SLUICE_TEST_CASE(cancel_outstanding_read_completes_canceled) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c;
    (void)ctx.submit_read(ReadOp{0, buf, 8, 0}, c);
    SLUICE_CHECK(c.outstanding());

    ctx.cancel(c);
    // The canceled completion is applied at the next poll (exactly-once, X3).
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    auto r = c.result();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::canceled);
    (void)raw;
}

// ---- Slice 2: cancel targets any outstanding op, not just the oldest -------
SLUICE_TEST_CASE(cancel_targets_any_outstanding_op) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte b[4]{};
    Completion<std::size_t> c1, c2, c3;
    (void)ctx.submit_read(ReadOp{0, b, 4, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, b, 4, 4}, c2);
    (void)ctx.submit_read(ReadOp{0, b, 4, 8}, c3);

    // Cancel the MIDDLE op (c2), not the oldest.
    ctx.cancel(c2);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c2.ready());
    SLUICE_CHECK(c2.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(!c1.ready() && !c3.ready());  // siblings untouched

    // The others can still complete normally.
    raw->complete_oldest_with_bytes(4);   // now applies to c1 (oldest remaining)
    raw->complete_oldest_with_bytes(4);   // applies to c3
    SLUICE_CHECK(ctx.poll() == 2);
    SLUICE_CHECK(c1.ready() && c3.ready());
}

// ---- Slice 3: cancel on an idle (never-submitted) completion is a no-op ----
SLUICE_TEST_CASE(cancel_on_idle_completion_is_noop) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Completion<std::size_t> c;  // idle, never submitted
    ctx.cancel(c);              // must not complete it or assert
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(ctx.poll() == 0);
}

// ---- Slice 4: cancel is exactly-once — a second cancel does not double-complete
SLUICE_TEST_CASE(cancel_is_exactly_once) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    std::byte b[4]{};
    Completion<std::size_t> c;
    (void)ctx.submit_read(ReadOp{0, b, 4, 0}, c);
    ctx.cancel(c);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    // Second cancel after ready: no-op (the op is no longer outstanding).
    ctx.cancel(c);
    SLUICE_CHECK(ctx.poll() == 0);  // nothing to complete again
}

// ---- Slice 5: AsyncStats.canceled_ops increments on cancel (021) ----------
SLUICE_TEST_CASE(cancel_increments_canceled_ops_stat) {
    sluice::AsyncStats s;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>(), &s);
    std::byte b[4]{};
    Completion<std::size_t> c1, c2;
    (void)ctx.submit_read(ReadOp{0, b, 4, 0}, c1);
    (void)ctx.submit_read(ReadOp{0, b, 4, 4}, c2);
    SLUICE_CHECK(s.canceled_ops == 0);

    ctx.cancel(c1);
    ctx.cancel(c2);
    SLUICE_CHECK(ctx.poll() == 2);
    SLUICE_CHECK(s.canceled_ops == 2);
    SLUICE_CHECK(s.completed_ops == 2);  // cancel-completions count as completed too
}

SLUICE_MAIN()
