// Tests for UringAsyncBackend (sluice-CORE-020B). In the default build
// (liburing absent) this exercises the STUB contract: submit_* returns
// backend_error synchronously, poll()/wait_one() reap nothing, outstanding()
// stays 0 (no L11 violation), and available() is false. The real io_uring path
// is gated behind SLUICE_HAS_LIBURING and exercised only where liburing is
// linked (ADR §11 D4).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>

using namespace sluice::async;
using sluice::Result;
using sluice::IoError;

// ---- Slice 1: stub mode reports unavailable + submit rejects cleanly -------
SLUICE_TEST_CASE(uring_stub_reports_unavailable) {
    UringAsyncBackend backend;
#if !defined(SLUICE_HAS_LIBURING)
    SLUICE_CHECK(!backend.available());   // stub: no real ring
#else
    SLUICE_CHECK(backend.available());    // real liburing build
#endif
}

SLUICE_TEST_CASE(uring_stub_submit_returns_backend_error) {
#if !defined(SLUICE_HAS_LIBURING)
    AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
    std::byte buf[4]{};
    Completion<std::size_t> c;
    // submit must reject synchronously in stub mode; the Completion stays idle.
    auto r = ctx.submit_read(ReadOp{0, buf, 4, 0}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(c.idle());          // not recorded outstanding
    SLUICE_CHECK(ctx.outstanding() == 0);
    SLUICE_CHECK(ctx.poll() == 0);   // nothing to reap
#else
    // Real-liburing assertions live in a separate environment-specific test.
    SLUICE_CHECK(true);
#endif
}

SLUICE_TEST_CASE(uring_stub_wait_one_reaps_nothing) {
#if !defined(SLUICE_HAS_LIBURING)
    AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
    // wait_one on an empty/stub backend returns 0 without blocking.
    auto r = ctx.wait_one();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 0);
#else
    SLUICE_CHECK(true);
#endif
}

SLUICE_MAIN()
