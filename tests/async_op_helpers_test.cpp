// Tests for the async "all" helpers (sluice-CORE-018). Exercises read_all/
// write_all against FakeAsyncBackend (job 019) in AUTO-COMPLETE mode (the
// coordinators submit+poll internally and cannot have the test stage results
// between their loop steps, so the fake auto-completes each outstanding op).
// Verifies: positional default (P1), positional independence, short-completion
// retry (O5), EOF before/within full (E4), zero-progress invalid_state.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
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

// ---- Slice 1: read_all completes when the full buffer transfers ------------

SLUICE_TEST_CASE(read_all_completes_full_buffer) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_bytes(8);  // each outstanding op completes with 8 bytes
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8];
    auto r = read_all(ctx, /*fd=*/0, std::span<std::byte>(buf), /*offset=*/0);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 8);
}

// ---- Slice 2: read_all retries across short completions (O5) ---------------

SLUICE_TEST_CASE(read_all_retries_short_completions) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    // First op short (3), subsequent ops complete their FULL remaining length.
    backend->auto_short_then_full(3);
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[10];
    auto r = read_all(ctx, 0, std::span<std::byte>(buf), 0);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 10);
}

// ---- Slice 3: read_all EOF before full -> IoError::eof (E4) -----------------

SLUICE_TEST_CASE(read_all_eof_before_full_returns_eof) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_eof();  // every op completes with 0 bytes => EOF at first step
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[10];
    auto r = read_all(ctx, 0, std::span<std::byte>(buf), 0);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::eof);
}

// ---- Slice 4: read_all error propagates immediately ------------------------

SLUICE_TEST_CASE(read_all_error_propagates) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::backend_error});
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8];
    auto r = read_all(ctx, 0, std::span<std::byte>(buf), 0);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
}

// ---- Slice 5: write_all completes and retries across shorts ----------------

SLUICE_TEST_CASE(write_all_retries_short_completions) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_short_then_full(2);
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[7];
    auto r = write_all(ctx, 0, std::span<const std::byte>(buf), 0);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 7);
}

// ---- Slice 6: write_all zero progress -> invalid_state ---------------------

SLUICE_TEST_CASE(write_all_zero_progress_is_invalid_state) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_bytes(0);  // write returns 0 on non-empty input
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[4];
    auto r = write_all(ctx, 0, std::span<const std::byte>(buf), 0);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

// ---- Slice 7: positional independence — two raw ops on one fd at different
// offsets complete independently (P1, no implicit-cursor coupling). -----------
SLUICE_TEST_CASE(positional_independence_two_offsets) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte a[4], b[4];
    Completion<std::size_t> ca, cb;
    (void)ctx.submit_read(ReadOp{0, a, 4, /*offset=*/0}, ca);
    (void)ctx.submit_read(ReadOp{0, b, 4, /*offset=*/100}, cb);  // disjoint offset
    raw->complete_oldest_with_bytes(4);
    raw->complete_oldest_with_bytes(4);
    SLUICE_CHECK(ctx.poll() == 2);
    SLUICE_CHECK(ca.ready() && cb.ready());
    SLUICE_CHECK(ca.result().value() == 4);
    SLUICE_CHECK(cb.result().value() == 4);  // independent of ca's offset
}

// ---- Slice 8: empty buffer is immediate success ----------------------------

SLUICE_TEST_CASE(read_all_empty_buffer_is_success) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    auto r = read_all(ctx, 0, std::span<std::byte>{}, 0);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 0);
}

SLUICE_MAIN()
