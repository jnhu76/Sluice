// Shared AsyncBackend conformance suite — implementation (sluice-CORE-024, B1).
//
// VERTICAL SLICES per the tdd skill: one case -> verify on Fake -> add next.
// Do NOT write all cases up front. Each case asserts ONE shared semantic.
//
// Cases live behind run_conformance() so each backend test target instantiates
// the same suite. Backend-specific mechanism stays in the per-backend files.
#include "backend_conformance.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace sluice_test::conformance {

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::Result;
using sluice::IoError;

// Conformance cases run inside plain functions (not SLUICE_TEST_CASE), so they
// need their own check macro that records a failure and bails out of the case
// via an exception. run_conformance catches it; a failure marks the suite
// failed but does not crash the process.
struct ConformanceFailure {
    std::string backend;
    std::string case_name;
    std::string expr;
    std::string file;
    int line;
};
inline std::vector<ConformanceFailure>& conf_failures() {
    static std::vector<ConformanceFailure> v;
    return v;
}
// case_bail is now declared in backend_conformance.hpp so C++ regression tests
// can drive run_capacity_case / CapacityFixture directly.
#define CONF_CHECK(backend, case_name, cond)                                            \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            conf_failures().push_back({(backend), (case_name), #cond, __FILE__, __LINE__}); \
            throw case_bail{};                                                          \
        }                                                                               \
    } while (0)

// Print every conformance failure recorded since `since` (used by the C2e
// close/drain driver so a failing case names its exact assertion, like
// run_conformance's tail does for the shared suite).
inline void print_conf_failures_since(std::size_t since) {
    for (std::size_t i = since; i < conf_failures().size(); ++i) {
        const auto& fl = conf_failures()[i];
        std::printf("[conformance] FAIL %s :: %s : %s (%s:%d)\n",
                    fl.backend.c_str(), fl.case_name.c_str(),
                    fl.expr.c_str(), fl.file.c_str(), fl.line);
    }
}

// Helper: drain any outstanding ops on a context so its destructor's L11
// assert is clean. Used at the end of cases that may leave ops in flight.
inline void drain(AsyncIoContext& ctx) {
    while (ctx.outstanding() > 0) {
        auto r = ctx.wait_one();
        if (!r.has_value()) break;
    }
}

// ---- Case 1 (tracer bullet): submit reaps exactly once in poll/wait_one ----
// Shared semantic (ADR §6 O1, §5 L4): a submitted op becomes ready ONLY inside
// poll/wait_one, exactly once, carrying its result. This is the foundation
// every other case rests on, so it is the tracer bullet.
//
// On real_mode backends we verify byte content via a write->read round-trip on
// a temp fd. On non-real_mode (Fake, Uring-stub) we only assert the
// submit->outstanding->poll->ready->exactly-once shape, because there is no
// kernel to round-trip bytes through.
static void case_submit_reaps_exactly_once(const BackendFactory& f) {
    AsyncIoContext ctx(f.make_backend());
    Completion<std::size_t> c;
    const char* const cname = "submit_reaps_exactly_once";

    if (f.real_mode) {
        const int fd = f.make_temp_fd();
        CONF_CHECK(f.name, cname, fd >= 0);
        const std::byte payload[4]{std::byte{0xAB}, std::byte{0xCD},
                                   std::byte{0xEF}, std::byte{0x12}};
        // Write 4 bytes at offset 0.
        CONF_CHECK(f.name, cname,
                   ctx.submit_write(WriteOp{fd, payload, 4, 0}, c).has_value());
        CONF_CHECK(f.name, cname, c.outstanding());
        CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
        CONF_CHECK(f.name, cname, c.ready());
        CONF_CHECK(f.name, cname, c.result().has_value());
        CONF_CHECK(f.name, cname, c.result().value() == 4);
        // Read them back.
        Completion<std::size_t> r;
        std::byte got[4]{};
        CONF_CHECK(f.name, cname,
                   ctx.submit_read(ReadOp{fd, got, 4, 0}, r).has_value());
        CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
        CONF_CHECK(f.name, cname, r.ready());
        CONF_CHECK(f.name, cname, r.result().value() == 4);
        CONF_CHECK(f.name, cname, std::memcmp(got, payload, 4) == 0);
        ::close(fd);
    } else {
        std::byte buf[4]{};
        // Non-real backend: submit may legitimately return backend_error
        // (Uring stub) or succeed and hold outstanding (Fake). Either way the
        // shape we assert is: if submit succeeded, the op is outstanding and
        // poll does not spuriously complete it. Then drain so the context
        // destructs cleanly.
        auto submit_res = ctx.submit_read(ReadOp{-1, buf, 4, 0}, c);
        if (submit_res.has_value()) {
            CONF_CHECK(f.name, cname, c.outstanding());
            CONF_CHECK(f.name, cname, ctx.poll() == 0);  // nothing ready yet
            CONF_CHECK(f.name, cname, c.outstanding());
            ctx.cancel(c);
            drain(ctx);
        }
        // If submit returned an error (Uring stub), there is nothing to reap;
        // outstanding stays 0 and the context destructs cleanly.
    }
}

// ---- Case 2: positional independence (ADR §6 P1) --------------------------
// Two ops on the same fd at different offsets complete independently — no
// implicit-cursor coupling. real_mode only (needs a real fd). Verifies the
// defining property of positional async I/O.
static void case_positional_independence(const BackendFactory& f) {
    if (!f.real_mode) {
        note_skip(f.name, "positional_independence", "non-real_mode");
        return;
    }
    AsyncIoContext ctx(f.make_backend());
    const char* const cname = "positional_independence";
    const int fd = f.make_temp_fd();
    CONF_CHECK(f.name, cname, fd >= 0);

    // Lay down 8 bytes: [10,20,30,40, 50,60,70,80] at offsets 0..7.
    const std::byte seed[8]{std::byte{10}, std::byte{20}, std::byte{30}, std::byte{40},
                            std::byte{50}, std::byte{60}, std::byte{70}, std::byte{80}};
    {
        Completion<std::size_t> w;
        CONF_CHECK(f.name, cname,
                   ctx.submit_write(WriteOp{fd, seed, 8, 0}, w).has_value());
        CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
        CONF_CHECK(f.name, cname, w.result().value() == 8);
    }

    // Two reads at different offsets, submitted back-to-back. Each must return
    // ONLY its own window — proving the offsets are independent (no shared
    // cursor advanced by the first op).
    Completion<std::size_t> r_lo, r_hi;
    std::byte got_lo[4]{}, got_hi[4]{};
    CONF_CHECK(f.name, cname,
               ctx.submit_read(ReadOp{fd, got_lo, 4, 0}, r_lo).has_value());  // bytes 0..3
    CONF_CHECK(f.name, cname,
               ctx.submit_read(ReadOp{fd, got_hi, 4, 4}, r_hi).has_value());  // bytes 4..7
    // Reap both (order unspecified per ADR O2/O3).
    std::size_t reaped = 0;
    while (reaped < 2) {
        auto n = ctx.wait_one();
        CONF_CHECK(f.name, cname, n.has_value());
        reaped += n.value();
    }
    CONF_CHECK(f.name, cname, r_lo.ready());
    CONF_CHECK(f.name, cname, r_hi.ready());
    CONF_CHECK(f.name, cname, r_lo.result().value() == 4);
    CONF_CHECK(f.name, cname, r_hi.result().value() == 4);
    CONF_CHECK(f.name, cname, std::memcmp(got_lo, seed + 0, 4) == 0);  // 10,20,30,40
    CONF_CHECK(f.name, cname, std::memcmp(got_hi, seed + 4, 4) == 0);  // 50,60,70,80
    ::close(fd);
}

// ---- Case 3: EOF surfaces as IoError::eof after partial progress ----------
// A read past end-of-file returns 0 bytes (EOF). read_all maps a 0-length read
// to IoError::eof, mirroring blocking read_some (ADR §6 O5, §8 E4). real_mode
// only.
static void case_eof_after_partial(const BackendFactory& f) {
    if (!f.real_mode) {
        note_skip(f.name, "eof_after_partial", "non-real_mode");
        return;
    }
    AsyncIoContext ctx(f.make_backend());
    const char* const cname = "eof_after_partial";
    const int fd = f.make_temp_fd();
    CONF_CHECK(f.name, cname, fd >= 0);

    // Write 3 bytes; read_all requesting 8 -> gets 3 then EOF.
    const std::byte seed[3]{std::byte{1}, std::byte{2}, std::byte{3}};
    {
        Completion<std::size_t> w;
        CONF_CHECK(f.name, cname,
                   ctx.submit_write(WriteOp{fd, seed, 3, 0}, w).has_value());
        CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
    }

    std::byte got[8]{};
    auto res = read_all(ctx, fd, {got, 8}, 0);
    // read_all must surface EOF (3 bytes partial progress then n==0 -> eof).
    CONF_CHECK(f.name, cname, !res.has_value());
    CONF_CHECK(f.name, cname, res.error().code == IoError::Code::eof);
    // The 3 partial bytes were transferred before EOF.
    CONF_CHECK(f.name, cname, std::memcmp(got, seed, 3) == 0);
    ::close(fd);
}

// ---- Case 4: short-completion retry via read_all/write_all (ADR §6 O5) ----
// A backend may complete fewer bytes than requested (short read/write); the
// derived helpers loop until all bytes transfer or an error occurs. real_mode
// only (the kernel may legitimately short a large transfer).
static void case_short_completion_retried(const BackendFactory& f) {
    if (!f.real_mode) {
        note_skip(f.name, "short_completion_retried", "non-real_mode");
        return;
    }
    AsyncIoContext ctx(f.make_backend());
    const char* const cname = "short_completion_retried";
    const int fd = f.make_temp_fd();
    CONF_CHECK(f.name, cname, fd >= 0);

    // Write 4096 bytes via write_all; read_all them back; verify identity.
    std::vector<std::byte> payload(4096);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(0x61 + (i % 26));
    }
    auto wres = write_all(ctx, fd, {payload.data(), payload.size()}, 0);
    CONF_CHECK(f.name, cname, wres.has_value());

    std::vector<std::byte> got(4096);
    auto rres = read_all(ctx, fd, {got.data(), got.size()}, 0);
    CONF_CHECK(f.name, cname, rres.has_value());
    CONF_CHECK(f.name, cname, std::memcmp(got.data(), payload.data(), 4096) == 0);
    ::close(fd);
}

// ---- Case 5: exactly-once terminal state (ADR §7 X3) ---------------------
// A completion is marked ready EXACTLY ONCE. Submitting one op, reaping it,
// and re-polling must not produce a second completion for the same op. The
// Completion's state machine enforces this structurally (idle->outstanding->
// ready is monotonic); this case asserts the OBSERVABLE consequence across the
// backend boundary: after one reap, further polls report nothing for that op.
// real_mode (uses a real fd so the op has something to complete with).
static void case_exactly_once_terminal(const BackendFactory& f) {
    if (!f.real_mode) {
        note_skip(f.name, "exactly_once_terminal", "non-real_mode");
        return;
    }
    AsyncIoContext ctx(f.make_backend());
    const char* const cname = "exactly_once_terminal";
    const int fd = f.make_temp_fd();
    CONF_CHECK(f.name, cname, fd >= 0);

    const std::byte payload[4]{std::byte{0x77}, std::byte{0x88},
                               std::byte{0x99}, std::byte{0xAA}};
    {
        Completion<std::size_t> w;
        CONF_CHECK(f.name, cname,
                   ctx.submit_write(WriteOp{fd, payload, 4, 0}, w).has_value());
        CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
        CONF_CHECK(f.name, cname, w.ready());
        CONF_CHECK(f.name, cname, w.result().value() == 4);
    }

    // One outstanding read; reap exactly once.
    Completion<std::size_t> r;
    std::byte got[4]{};
    CONF_CHECK(f.name, cname,
               ctx.submit_read(ReadOp{fd, got, 4, 0}, r).has_value());
    CONF_CHECK(f.name, cname, ctx.outstanding() == 1);
    CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
    CONF_CHECK(f.name, cname, r.ready());
    CONF_CHECK(f.name, cname, r.result().value() == 4);
    CONF_CHECK(f.name, cname, ctx.outstanding() == 0);

    // A subsequent non-blocking poll must NOT report a second completion.
    // (There is nothing outstanding, so poll must reap 0.)
    CONF_CHECK(f.name, cname, ctx.poll() == 0);
    CONF_CHECK(f.name, cname, ctx.outstanding() == 0);
    ::close(fd);
}

// ---- Case 6: cancel yields a defined terminal result (ADR §7 X2/X3) -------
// Cancel is best-effort and asynchronous: the op may still complete with its
// real result, an error, OR IoError::canceled. The contract is that AFTER
// cancel + reap, the Completion is ready EXACTLY ONCE with a DEFINED result
// (one of the three). It must never remain outstanding forever (caller can
// always reap a terminal result after cancel). This case does NOT assert
// WHICH result — that is backend-specific (ThreadPool: real result; Uring:
// may be canceled or real; Fake: controllable).
static void case_cancel_yields_defined_terminal(const BackendFactory& f) {
    AsyncIoContext ctx(f.make_backend());
    const char* const cname = "cancel_yields_defined_terminal";

    int fd = -1;
    if (f.real_mode) {
        fd = f.make_temp_fd();
        CONF_CHECK(f.name, cname, fd >= 0);
        // Lay down some bytes so a read has something to return.
        const std::byte seed[4]{std::byte{0x01}, std::byte{0x02},
                                std::byte{0x03}, std::byte{0x04}};
        Completion<std::size_t> w;
        CONF_CHECK(f.name, cname,
                   ctx.submit_write(WriteOp{fd, seed, 4, 0}, w).has_value());
        CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
    }

    Completion<std::size_t> r;
    std::byte got[4]{};
    auto submit_res = ctx.submit_read(ReadOp{fd, got, 4, 0}, r);
    if (!submit_res.has_value()) {
        // Non-real backend rejected submit (Uring stub: backend_error). Nothing
        // to cancel; outstanding is 0; the contract holds vacuously.
        CONF_CHECK(f.name, cname, ctx.outstanding() == 0);
        if (fd >= 0) ::close(fd);
        return;
    }
    CONF_CHECK(f.name, cname, r.outstanding());

    // Request cancel, then reap. The terminal result must be one of the three
    // defined outcomes (ADR X3).
    ctx.cancel(r);
    // Reap until the op is ready (cancel does not synchronously free the op).
    std::size_t guard = 0;
    while (!r.ready() && guard < 1000) {
        auto n = ctx.wait_one();
        CONF_CHECK(f.name, cname, n.has_value());
        if (n.value() == 0) break;  // spurious wake; loop
        ++guard;
    }
    CONF_CHECK(f.name, cname, r.ready());
    const auto res = r.result();
    const bool defined_outcome =
        res.has_value() ||
        res.error().code == IoError::Code::canceled ||
        res.error().code == IoError::Code::eof ||
        res.error().code == IoError::Code::backend_error;
    CONF_CHECK(f.name, cname, defined_outcome);
    if (fd >= 0) ::close(fd);
}

// ---- Case 7: AsyncStats accounting (ADR §10b) ----------------------------
// When a caller-owned AsyncStats is attached, the submit counter increments on
// every submit call (shared across every backend — the context tallies). The
// completed-ops counter increments on reap. On real_mode we drive a full
// submit->reap cycle and assert both; on non-real_mode the op may not complete
// without backend-specific staging, so we assert only the submit-side counter
// and then drain to keep the destructor clean.
static void case_stats_accounting(const BackendFactory& f) {
    AsyncStats stats;
    AsyncIoContext ctx(f.make_backend(), &stats);
    const char* const cname = "stats_accounting";

    const std::uint64_t submit_before = stats.submit_calls;
    const std::uint64_t submitted_before = stats.submitted_ops;

    int fd = -1;
    if (f.real_mode) {
        fd = f.make_temp_fd();
        CONF_CHECK(f.name, cname, fd >= 0);
    }
    Completion<std::size_t> w;
    const std::byte payload[4]{std::byte{0x55}, std::byte{0x66},
                               std::byte{0x77}, std::byte{0x88}};
    auto submit_res = ctx.submit_write(WriteOp{fd, payload, 4, 0}, w);
    CONF_CHECK(f.name, cname, stats.submit_calls == submit_before + 1);

    if (submit_res.has_value()) {
        CONF_CHECK(f.name, cname, stats.submitted_ops == submitted_before + 1);
        if (f.real_mode) {
            const std::uint64_t completed_before = stats.completed_ops;
            CONF_CHECK(f.name, cname, ctx.wait_one().value() == 1);
            CONF_CHECK(f.name, cname, stats.completed_ops == completed_before + 1);
            CONF_CHECK(f.name, cname, stats.wait_calls >= 1);
        } else {
            // Non-real backend accepted the op but won't complete without staging.
            // Drain via cancel so the context destructs cleanly.
            ctx.cancel(w);
            drain(ctx);
        }
    }
    if (fd >= 0) ::close(fd);
}

// ---- Case 8: clean shutdown with no outstanding ops (ADR §5 L11) ----------
// Destroying an AsyncIoContext with zero outstanding ops must be clean (no
// assertion, no leak). This is the L11 happy path. Every backend must satisfy
// it because the caller's destructor runs unconditionally.
static void case_clean_shutdown_no_ops(const BackendFactory& f) {
    const char* const cname = "clean_shutdown_no_ops";
    {
        AsyncIoContext ctx(f.make_backend());
        CONF_CHECK(f.name, cname, ctx.outstanding() == 0);
        // Submit nothing; just destruct.
    }
    // If we reached here, the destructor did not assert. PASS.
}

int run_conformance(const BackendFactory& f) {
    const std::size_t before = conf_failures().size();
    try {
        case_submit_reaps_exactly_once(f);
    } catch (const case_bail&) {
        // failure recorded; continue to next case
    }
    try {
        case_positional_independence(f);
    } catch (const case_bail&) {
        // failure recorded
    }
    try {
        case_eof_after_partial(f);
    } catch (const case_bail&) {
        // failure recorded
    }
    try {
        case_short_completion_retried(f);
    } catch (const case_bail&) {
        // failure recorded
    }
    try {
        case_exactly_once_terminal(f);
    } catch (const case_bail&) {
        // failure recorded
    }
    try {
        case_cancel_yields_defined_terminal(f);
    } catch (const case_bail&) {
        // failure recorded
    }
    try {
        case_stats_accounting(f);
    } catch (const case_bail&) {
        // failure recorded
    }
    try {
        case_clean_shutdown_no_ops(f);
    } catch (const case_bail&) {
        // failure recorded
    }
    const bool failed = conf_failures().size() != before;
    if (failed) {
        print_conf_failures_since(before);
    }
    return failed ? 1 : 0;
}

// ===========================================================================
// Phase C2a — shared capacity/admission/rejection/accounting cases.
//
// These cases prove (Issue #68 Rev 3, requirements 1-2):
//   * a bounded backend accepts exactly `capacity` requests;
//   * the (capacity+1)th submit synchronously returns would_block;
//   * a rejected Completion stays idle (no async from a reject);
//   * capacity rejection does not increment outstanding;
//   * submitted_ops counts committed requests only;
//   * submit_calls counts every submit attempt;
//   * max_outstanding never exceeds capacity;
//   * capacity rejection and invalid_state rejection are classified precisely;
//   * after accepted requests cancel -> reap -> reset, the capacity recycles.
//
// SHARED-OBSERVABLE BOUNDARY: the cases use ONLY
//   AsyncIoContext::submit / cancel / poll / wait_one / outstanding / stats
//   + Completion's public state/reset. They MUST NOT downcast, touch
//   complete_*, arena_*, dispatch_size_for_test, or any backend-specific
//   internals. The arena/mechanism evidence stays in the Phase B/E tests.
//
// CLEANUP MODEL (Issue #68 Rev 3, CORRECTION 3a/7a): cleanup is an EXPLICIT
// method cleanup_or_abort(), NOT a destructor. SLUICE_CHECK expands to
// record_failure(); return; — returning out of a destructor lets the
// AsyncIoContext member destruct and fire its own L11 fail-fast, masking the
// real cause. cleanup_or_abort() is never built on SLUICE_CHECK; it is
// time-bounded with a real deadline and abort()s on timeout so the failure
// cause is the capacity case, not a context-destructor violation. Every
// submit (successful or expected-rejection) is funneled through
// submit_and_track(), which registers the Completion into `tracked` BEFORE
// calling submit_read, so a deliberately-nonconforming backend that wrongly
// accepts the (N+1)th op, or that ILLEGALLY binds/publishes a rejected
// Completion later during backend progress, is still cleaned up even if the
// case throws right after.
// ===========================================================================

// cleanup_or_abort out-of-line definition. CapacityFixture, submit_and_track,
// run_capacity_case, ScopedTempFd, and capacity_temp_fd live in
// backend_conformance.hpp so C++ regression tests in capacity_validity_test.cpp
// can drive the fixture directly (the header is test-only, never installed).
void CapacityFixture::cleanup_or_abort(const char* backend_name,
                                       const char* case_name) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 1. Cancel everything still outstanding. Idle tracked Completions
    //    (rejected submits) are skipped — they have no async state.
    for (auto* c : tracked) {
        if (c->outstanding()) ctx.cancel(*c);
    }
    // 2. Drive poll/reap until outstanding hits 0 or the deadline expires.
    //    interrupt_backend_waiters() re-arms waiters; yield() lets workers
    //    progress. This is liveness, not ordering proof — cancel may still
    //    yield the real syscall result on ThreadPool (a valid terminal).
    //
    //    A deliberately-broken backend may ILLEGALLY bind a tracked-but-idle
    //    Completion during ITS OWN poll progress (the late_bind_only validity
    //    mutant), AFTER the upfront cancel pass skipped it (it was still
    //    idle). Re-cancel after every poll so such a Completion is resolved
    //    too instead of starving the loop to the deadline. Cancelling an
    //    already-ready/idle Completion is a no-op (guarded by outstanding()).
    while (ctx.outstanding() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)ctx.poll();
        for (auto* c : tracked) {
            if (c->outstanding()) ctx.cancel(*c);
        }
        ctx.interrupt_backend_waiters();
        std::this_thread::yield();
    }
    if (ctx.outstanding() != 0) {
        // Precise diagnostic — do NOT let the AsyncIoContext destructor's
        // fail-fast fire and mask this as an unrelated L11 violation.
        std::fprintf(stderr,
            "CapacityFixture: cleanup deadline expired "
            "(backend=%s case=%s outstanding=%zu tracked=%zu)\n",
            backend_name, case_name, ctx.outstanding(), tracked.size());
        for (std::size_t i = 0; i < tracked.size(); ++i) {
            const auto* c = tracked[i];
            std::fprintf(stderr,
                "  tracked[%zu]: idle=%d outstanding=%d ready=%d\n",
                i, (int)c->idle(), (int)c->outstanding(), (int)c->ready());
        }
        std::fprintf(stderr,
            "  stats: submit_calls=%llu submitted_ops=%llu "
            "canceled_ops=%llu completion_errors=%llu\n",
            (unsigned long long)stats.submit_calls,
            (unsigned long long)stats.submitted_ops,
            (unsigned long long)stats.canceled_ops,
            (unsigned long long)stats.completion_errors);
        std::abort();
    }
    // 3. Reset every ready Completion so the slot is released (capacity
    //    recycles). The context is now quiescent; the fixture destructor
    //    (default) is a no-op.
    for (auto* c : tracked) {
        if (c->ready()) c->reset();
    }
}

// ---- Case A: accepts exact capacity --------------------------------------
// capacity=2: c1 accept, c2 accept, outstanding==2, submit_calls==2,
// submitted_ops==2, max_outstanding==2, queue_full_retries==0,
// invalid_state_rejections==0.
std::string case_capacity_accepts_exact_limit(const BackendFactory& f) {
    constexpr const char* cname = "capacity_accepts_exact_limit";
    CapacityFixture fx(f.make_backend_with_capacity(2));
    ScopedTempFd fd(capacity_temp_fd(f));
    // real_mode only: a failed temp-fd setup must be reported separately, not
    // misread as a capacity rejection (see capacity_temp_fd_setup_error).
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf1[4]{}, buf2[4]{};
    Completion<std::size_t> c1, c2;
    return run_capacity_case(fx, f.name, cname, [&] {
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4)).has_value());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 2);
        CONF_CHECK(f.name, cname, fx.stats.submit_calls == 2);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 2);
        CONF_CHECK(f.name, cname, fx.stats.max_outstanding == 2);
        CONF_CHECK(f.name, cname, fx.stats.queue_full_retries == 0);
        CONF_CHECK(f.name, cname, fx.stats.invalid_state_rejections == 0);
    });
}

// ---- Case B: the (N+1)th submit is rejected with would_block --------------
// c1, c2 accept; c3 returns would_block; c3 stays idle; outstanding==2;
// submitted_ops==2; submit_calls==3; queue_full_retries==1;
// invalid_state_rejections==0; max_outstanding==2.
std::string case_capacity_rejects_with_idle_completion(const BackendFactory& f) {
    constexpr const char* cname = "capacity_rejects_with_idle_completion";
    CapacityFixture fx(f.make_backend_with_capacity(2));
    ScopedTempFd fd(capacity_temp_fd(f));
    // real_mode only: report a failed temp-fd setup separately (see
    // capacity_temp_fd_setup_error).
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf1[4]{}, buf2[4]{}, buf3[4]{};
    Completion<std::size_t> c1, c2, c3;
    return run_capacity_case(fx, f.name, cname, [&] {
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4)).has_value());
        // c3 must be REJECTED for capacity. A conforming backend returns
        // would_block and leaves c3 idle; a deliberately-broken validity
        // backend may bind c3 before returning the error. Either way the
        // submit goes through submit_and_track so a non-idle c3 is tracked
        // and cleanup can terminalize it instead of letting a destructor
        // fail-fast mask the capacity assertion.
        auto r3 = fx.submit_and_track(c3, fx.make_read_op(fd, buf3, 4));
        CONF_CHECK(f.name, cname, !r3.has_value());
        CONF_CHECK(f.name, cname,
                   r3.error().code == IoError::Code::would_block);
        // The rejected Completion stays idle throughout — no async from a reject.
        CONF_CHECK(f.name, cname, c3.idle());
        CONF_CHECK(f.name, cname, !c3.outstanding());
        CONF_CHECK(f.name, cname, !c3.ready());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 2);
        CONF_CHECK(f.name, cname, fx.stats.submit_calls == 3);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 2);
        CONF_CHECK(f.name, cname, fx.stats.queue_full_retries == 1);
        CONF_CHECK(f.name, cname, fx.stats.invalid_state_rejections == 0);
        CONF_CHECK(f.name, cname, fx.stats.max_outstanding == 2);
    });
}

// ---- Case C: a rejected request never produces a late completion ----------
// cap=1: c1 accept, c2 rejected (would_block). Then drive backend progress,
// clean up c1 (cancel -> reap). c2 must remain idle/not-outstanding/not-ready
// throughout and after — no completion publication belongs to c2.
std::string case_capacity_rejection_never_completes(const BackendFactory& f) {
    constexpr const char* cname = "capacity_rejection_never_completes";
    CapacityFixture fx(f.make_backend_with_capacity(1));
    ScopedTempFd fd(capacity_temp_fd(f));
    // real_mode only: report a failed temp-fd setup separately (see
    // capacity_temp_fd_setup_error).
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf1[4]{}, buf2[4]{};
    Completion<std::size_t> c1, c2;
    return run_capacity_case(fx, f.name, cname, [&] {
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        auto r2 = fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4));
        CONF_CHECK(f.name, cname, !r2.has_value());
        CONF_CHECK(f.name, cname,
                   r2.error().code == IoError::Code::would_block);
        CONF_CHECK(f.name, cname, c2.idle());
        // Drive backend progress and reap c1. c2 must stay idle the whole time.
        fx.ctx.cancel(c1);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fx.ctx.outstanding() != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            (void)fx.ctx.poll();
            fx.ctx.interrupt_backend_waiters();
            // c2 must NEVER become outstanding/ready as a side effect.
            CONF_CHECK(f.name, cname, c2.idle());
            CONF_CHECK(f.name, cname, !c2.outstanding());
            CONF_CHECK(f.name, cname, !c2.ready());
            std::this_thread::yield();
        }
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);

        // Post-drain progress probe: one more backend-progress turn AFTER all
        // accepted work drained. The loop above exits the instant outstanding
        // hits 0, so a backend that only fires the rejected op's ILLEGAL
        // completion on the NEXT progress turn would otherwise be missed
        // (false green). A rejected request must not have left a deferred
        // completion of any kind. Pinned by the late_complete_after_drain
        // validity mutant, whose publish lands here.
        const auto post_drain_reaped = fx.ctx.poll();
        CONF_CHECK(f.name, cname, post_drain_reaped == 0);
        CONF_CHECK(f.name, cname, c2.idle());
        CONF_CHECK(f.name, cname, !c2.outstanding());
        CONF_CHECK(f.name, cname, !c2.ready());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 1);
    });
}

// ---- Case D: rejection classification is precise --------------------------
// Deterministic sequence: c1 accept; resubmit on non-idle c1 -> invalid_state;
// c2 accept; c3 -> would_block. Exact stats (CORRECTION 5: no >= 1):
//   submit_calls==4, submitted_ops==2, invalid_state_rejections==1,
//   queue_full_retries==1, max_outstanding==2, outstanding==2.
std::string case_capacity_stats_are_exact(const BackendFactory& f) {
    constexpr const char* cname = "capacity_stats_are_exact";
    CapacityFixture fx(f.make_backend_with_capacity(2));
    ScopedTempFd fd(capacity_temp_fd(f));
    // real_mode only: report a failed temp-fd setup separately (see
    // capacity_temp_fd_setup_error).
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf1[4]{}, buf1b[4]{}, buf2[4]{}, buf3[4]{};
    Completion<std::size_t> c1, c2, c3;
    return run_capacity_case(fx, f.name, cname, [&] {
        // 1. c1 accept.
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        // 2. Resubmit on the still-non-idle c1 -> invalid_state (caller
        //    lifecycle violation, NOT capacity). c1 is already tracked; a
        //    conforming backend leaves it non-idle so submit_and_track's
        //    !c.idle() branch is a no-op track_once (c1 is already registered).
        //    A deliberately-broken backend that re-binds-and-errors still sees
        //    a no-op track_once.
        auto r1b = fx.submit_and_track(c1, fx.make_read_op(fd, buf1b, 4));
        CONF_CHECK(f.name, cname, !r1b.has_value());
        CONF_CHECK(f.name, cname,
                   r1b.error().code == IoError::Code::invalid_state);
        // 3. c2 accept.
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4)).has_value());
        // 4. c3 -> would_block (capacity full). Routed through submit_and_track
        //    so a broken backend that binds c3 before returning would_block is
        //    still cleaned up.
        auto r3 = fx.submit_and_track(c3, fx.make_read_op(fd, buf3, 4));
        CONF_CHECK(f.name, cname, !r3.has_value());
        CONF_CHECK(f.name, cname,
                   r3.error().code == IoError::Code::would_block);
        // Exact assertions — deterministic counters tallied exactly once.
        CONF_CHECK(f.name, cname, fx.stats.submit_calls == 4);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 2);
        CONF_CHECK(f.name, cname, fx.stats.invalid_state_rejections == 1);
        CONF_CHECK(f.name, cname, fx.stats.queue_full_retries == 1);
        CONF_CHECK(f.name, cname, fx.stats.max_outstanding == 2);
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 2);
        CONF_CHECK(f.name, cname, fx.stats.canceled_ops == 0);
        CONF_CHECK(f.name, cname, fx.stats.completion_errors == 0);
    });
}

// ---- Case E: capacity recycles after cancel -> reap -> reset --------------
// cap=1: c1 accept (fills capacity). Cancel + reap c1, then reset c1 (releases
// the slot). A fresh c2 submit MUST succeed (capacity recycled).
std::string case_capacity_recycles_after_reset(const BackendFactory& f) {
    constexpr const char* cname = "capacity_recycles_after_reset";
    CapacityFixture fx(f.make_backend_with_capacity(1));
    ScopedTempFd fd(capacity_temp_fd(f));
    // real_mode only: report a failed temp-fd setup separately (see
    // capacity_temp_fd_setup_error).
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf1[4]{}, buf2[4]{};
    Completion<std::size_t> c1, c2;
    return run_capacity_case(fx, f.name, cname, [&] {
        // Fill capacity.
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 1);
        // Terminalize c1 (cancel may win canceled or yield the real result;
        // either is a valid terminal — C2a does not care WHICH terminal wins,
        // only that the accepted op reaches exactly one and the slot recycles).
        fx.ctx.cancel(c1);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!c1.ready() && std::chrono::steady_clock::now() < deadline) {
            (void)fx.ctx.poll();
            fx.ctx.interrupt_backend_waiters();
            std::this_thread::yield();
        }
        CONF_CHECK(f.name, cname, c1.ready());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);
        // Reset c1 — releases the slot. Capacity must now be reusable.
        c1.reset();
        // c1 is no longer tracked (it was reset); drop it so cleanup does not
        // touch it again.
        for (auto it = fx.tracked.begin(); it != fx.tracked.end(); ++it) {
            if (*it == &c1) { fx.tracked.erase(it); break; }
        }
        // A fresh submit MUST succeed (capacity recycled).
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4)).has_value());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 1);
        CONF_CHECK(f.name, cname, fx.stats.submit_calls == 2);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 2);
        CONF_CHECK(f.name, cname, fx.stats.max_outstanding == 1);
    });
}

// The capacity-case driver. Returns the empty string on full pass, or the
// stable name of the FIRST failing case. Drives ONLY the capacity cases against
// a backend built at a chosen small request_capacity via
// factory.make_backend_with_capacity. Precondition: factory_supports_capacity.
//
// Each case is a self-contained function owning its CapacityFixture + its
// Completions in the same frame; run_capacity_case() runs cleanup_or_abort()
// on BOTH paths while that frame (and therefore the Completions) is still
// alive. See the run_capacity_case comment for why this is the only scope-
// correct shape.
std::string run_capacity_cases(const BackendFactory& f) {
    std::string failed;

    failed = case_capacity_accepts_exact_limit(f);
    if (!failed.empty()) return failed;

    failed = case_capacity_rejects_with_idle_completion(f);
    if (!failed.empty()) return failed;

    failed = case_capacity_rejection_never_completes(f);
    if (!failed.empty()) return failed;

    failed = case_capacity_stats_are_exact(f);
    if (!failed.empty()) return failed;

    failed = case_capacity_recycles_after_reset(f);
    if (!failed.empty()) return failed;

    return {};
}

// ===========================================================================
// Phase C2e — shared close/drain/destruction cases (Issue #68 rows 15-16).
//
// These cases prove, at the SHARED OBSERVABLE boundary (ADR Decision 15):
//   * close_admission() rejects new submit with invalid_state, deterministically
//     (the Completion stays idle, no borrow, no outstanding, no submitted_ops
//     increment — the reject is a synchronous no-side-effect admission refusal);
//   * close NEVER retroactively rejects or cancels an already-accepted request:
//     the accepted op stays outstanding, cancel/poll/reap remain legal, and the
//     request reaches EXACTLY ONE defined terminal, then the caller reset
//     releases the slot;
//   * drained != releasable backend destruction: after outstanding == 0 and
//     Completion-ready, the slot is still bound (slot_in_use == 1) until the
//     caller resets the ready Completion (slot_in_use == 0);
//   * slot-lifecycle release and admission-lifecycle close are ORTHOGONAL:
//     after drain + reset the slot is free but a fresh submit is still
//     invalid_state (admission stays closed);
//   * cancel remains legal after close (ADR Decision 15: "operation
//     cancellation remains legal") — the shared terminalize-and-reap path of
//     cases B/H/J runs AFTER close_admission.
//
// SHARED-OBSERVABLE BOUNDARY: the case bodies use ONLY
//   AsyncIoContext::submit/cancel/poll/wait_one/outstanding/interrupt_backend_waiters/stats
//   + Completion's public state/reset
//   + the two driver-wired closures (close, slot_in_use).
// They MUST NOT downcast, touch complete_*, arena_*, dispatch_*, or any
// backend-specific internals. The deterministic per-window evidence (close
// while pending/enqueued/running, close || final backend-ready, parked-waiter
// wake) lives in the per-backend targets.
//
// CLEANUP MODEL: identical to the capacity cases — every submit goes through
// submit_and_track; cleanup_or_abort() is explicit, time-bounded, and abort()s
// on timeout (never lets the AsyncIoContext destructor fail-fast mask a C2e
// assertion).
// ===========================================================================

void CloseDrainFixture::cleanup_or_abort(const char* backend_name,
                                         const char* case_name) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 1. Cancel everything still outstanding. Idle tracked Completions
    //    (rejected submits) are skipped — they have no async state. Cancel is
    //    legal after close (ADR Decision 15), so this always terminalizes.
    for (auto* c : tracked) {
        if (c->outstanding()) ctx.cancel(*c);
    }
    // 2. Drive poll/reap until outstanding hits 0 or the deadline expires.
    //    interrupt_backend_waiters() re-arms waiters; yield() lets workers
    //    progress. This is liveness, not ordering proof.
    while (ctx.outstanding() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)ctx.poll();
        for (auto* c : tracked) {
            if (c->outstanding()) ctx.cancel(*c);
        }
        ctx.interrupt_backend_waiters();
        std::this_thread::yield();
    }
    if (ctx.outstanding() != 0) {
        std::fprintf(stderr,
            "CloseDrainFixture: cleanup deadline expired "
            "(backend=%s case=%s outstanding=%zu tracked=%zu)\n",
            backend_name, case_name, ctx.outstanding(), tracked.size());
        for (std::size_t i = 0; i < tracked.size(); ++i) {
            const auto* c = tracked[i];
            std::fprintf(stderr,
                "  tracked[%zu]: idle=%d outstanding=%d ready=%d\n",
                i, (int)c->idle(), (int)c->outstanding(), (int)c->ready());
        }
        std::fprintf(stderr,
            "  stats: submit_calls=%llu submitted_ops=%llu "
            "invalid_state_rejections=%llu canceled_ops=%llu "
            "completion_errors=%llu\n",
            (unsigned long long)stats.submit_calls,
            (unsigned long long)stats.submitted_ops,
            (unsigned long long)stats.invalid_state_rejections,
            (unsigned long long)stats.canceled_ops,
            (unsigned long long)stats.completion_errors);
        std::abort();
    }
    // 3. Reset every ready Completion so the slot is released. The context is
    //    now quiescent; the fixture destructor (default) is a no-op.
    for (auto* c : tracked) {
        if (c->ready()) c->reset();
    }
}

// ---- Case A: close rejects future submit (deterministic invalid_state) -----
// backend/context open -> close_admission() -> submit => invalid_state;
// Completion idle; submitted_ops unchanged; outstanding unchanged; no
// borrow/dispatch/ready residue. Both Fake and ThreadPool run this.
std::string case_close_rejects_future_submit(const BackendFactory& f,
                                             const MakeCloseDrainFixture& make_fx) {
    constexpr const char* cname = "close_rejects_future_submit";
    // Hold the fixture via unique_ptr so its address (and therefore the
    // AsyncStats* the AsyncIoContext holds) is stable for the case's lifetime.
    auto fx_owner = make_fx();
    auto& fx = *fx_owner;
    ScopedTempFd fd(capacity_temp_fd(f));
    // real_mode only: report a failed temp-fd setup separately (a ThreadPool
    // submit needs a valid fd to reach the admission path — descriptor
    // validation precedes reserve).
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf[4]{};
    Completion<std::size_t> c;
    return run_close_drain_case(fx, f.name, cname, [&] {
        fx.close();  // close admission
        // A fresh submit MUST be rejected with invalid_state — deterministically,
        // synchronously, at the admission boundary (ADR Decision 15).
        auto r = fx.submit_and_track(c, fx.make_read_op(fd, buf, 4));
        CONF_CHECK(f.name, cname, !r.has_value());
        CONF_CHECK(f.name, cname,
                   r.error().code == IoError::Code::invalid_state);
        // The rejected Completion stays idle throughout — no async from a reject.
        CONF_CHECK(f.name, cname, c.idle());
        CONF_CHECK(f.name, cname, !c.outstanding());
        CONF_CHECK(f.name, cname, !c.ready());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);
        // Accounting: the submit call happened but nothing was accepted.
        CONF_CHECK(f.name, cname, fx.stats.submit_calls == 1);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 0);
        CONF_CHECK(f.name, cname, fx.stats.invalid_state_rejections == 1);
        CONF_CHECK(f.name, cname, fx.stats.queue_full_retries == 0);
        // poll after close with nothing outstanding: the no-progress boundary,
        // never a fabricated completion (I8).
        CONF_CHECK(f.name, cname, fx.ctx.poll() == 0);
    });
}

// ---- Case B: accepted-before-close still completes exactly once -------------
// submit succeeds -> close -> the request continues -> a defined terminal ->
// reap -> Completion ready -> reset -> slot released. close MUST NOT
// retroactively reject or cancel an already-accepted request; the request
// reaches exactly ONE defined terminal (the shared contract does not pin WHICH
// — canceled or the real result — that is backend-specific); poll/reap and
// cancel remain legal after close.
std::string case_close_preserves_accepted_terminal(const BackendFactory& f,
                                                   const MakeCloseDrainFixture& make_fx) {
    constexpr const char* cname = "close_preserves_accepted_terminal";
    // Hold the fixture via unique_ptr so its address (and therefore the
    // AsyncStats* the AsyncIoContext holds) is stable for the case's lifetime.
    auto fx_owner = make_fx();
    auto& fx = *fx_owner;
    ScopedTempFd fd(capacity_temp_fd(f));
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf[4]{};
    Completion<std::size_t> c;
    return run_close_drain_case(fx, f.name, cname, [&] {
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c, fx.make_read_op(fd, buf, 4)).has_value());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 1);
        CONF_CHECK(f.name, cname, c.outstanding());

        fx.close();  // close AFTER accept
        // close must not retroactively reject or cancel the accepted request.
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 1);
        CONF_CHECK(f.name, cname, c.outstanding());
        CONF_CHECK(f.name, cname, !c.ready());

        // Operation cancellation remains legal after close (ADR Decision 15).
        // Terminalize the accepted op (cancel may win canceled, or the real
        // syscall result may win verbatim — either is a defined terminal).
        fx.ctx.cancel(c);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!c.ready() && std::chrono::steady_clock::now() < deadline) {
            (void)fx.ctx.poll();
            fx.ctx.interrupt_backend_waiters();
            std::this_thread::yield();
        }
        CONF_CHECK(f.name, cname, c.ready());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);
        // Exactly one defined terminal: success or a defined error code
        // (canceled / eof / backend_error). Never a phantom or double result.
        const auto res = c.result();
        CONF_CHECK(f.name, cname,
                   res.has_value() ||
                       res.error().code == IoError::Code::canceled ||
                       res.error().code == IoError::Code::eof ||
                       res.error().code == IoError::Code::backend_error);
        // A further poll must NOT produce a second completion for the same op.
        CONF_CHECK(f.name, cname, fx.ctx.poll() == 0);
        c.reset();
    });
}

// ---- Case H: drained != releasable backend destruction ---------------------
// close -> the accepted request reaches terminal -> reap all ->
// accepted_outstanding == 0 (Completion ready) BUT slot_in_use == 1 until the
// caller resets the ready Completion (slot_in_use == 0). The slot stays bound
// by the caller-owned Completion's publication binding after reap.
std::string case_drain_then_reset_releases_slot(const BackendFactory& f,
                                                const MakeCloseDrainFixture& make_fx) {
    constexpr const char* cname = "drain_then_reset_releases_slot";
    // Hold the fixture via unique_ptr so its address (and therefore the
    // AsyncStats* the AsyncIoContext holds) is stable for the case's lifetime.
    auto fx_owner = make_fx();
    auto& fx = *fx_owner;
    ScopedTempFd fd(capacity_temp_fd(f));
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf[4]{};
    Completion<std::size_t> c;
    return run_close_drain_case(fx, f.name, cname, [&] {
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c, fx.make_read_op(fd, buf, 4)).has_value());
        // The slot is bound from reserve (slot_in_use == 1).
        CONF_CHECK(f.name, cname, fx.slot_in_use() == 1);

        fx.close();
        fx.ctx.cancel(c);  // cancel remains legal after close (Decision 15)
        // Drain: reap until accepted_outstanding == 0.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fx.ctx.outstanding() != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            (void)fx.ctx.poll();
            fx.ctx.interrupt_backend_waiters();
            std::this_thread::yield();
        }
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);
        CONF_CHECK(f.name, cname, c.ready());
        // DRAINED != RELEASABLE: reap published completion-ready but did NOT
        // release the slot. The ready-but-unreset Completion still holds the
        // binding, so the backend is NOT yet quiescent for destruction.
        CONF_CHECK(f.name, cname, fx.slot_in_use() == 1);
        // Caller reset releases the slot (allocation-free; generation++).
        c.reset();
        CONF_CHECK(f.name, cname, fx.slot_in_use() == 0);
        CONF_CHECK(f.name, cname, c.idle());
        // Post-reset poll: nothing reaped, nothing fabricated.
        CONF_CHECK(f.name, cname, fx.ctx.poll() == 0);
    });
}

// ---- Case J: slot released but admission still closed (orthogonal states) --
// close -> drain -> reset -> slot free (slot_in_use == 0) -> submit =>
// invalid_state. Slot-lifecycle release and admission-lifecycle close are two
// ORTHOGONAL states: a free slot does not re-open admission.
std::string case_slot_released_but_admission_stays_closed(const BackendFactory& f,
                                                          const MakeCloseDrainFixture& make_fx) {
    constexpr const char* cname = "slot_released_but_admission_stays_closed";
    // Hold the fixture via unique_ptr so its address (and therefore the
    // AsyncStats* the AsyncIoContext holds) is stable for the case's lifetime.
    auto fx_owner = make_fx();
    auto& fx = *fx_owner;
    ScopedTempFd fd(capacity_temp_fd(f));
    if (auto setup_err = capacity_temp_fd_setup_error(f, fd);
        !setup_err.empty()) {
        return setup_err;
    }
    std::byte buf1[4]{}, buf2[4]{};
    Completion<std::size_t> c1, c2;
    return run_close_drain_case(fx, f.name, cname, [&] {
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        fx.close();
        fx.ctx.cancel(c1);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!c1.ready() && std::chrono::steady_clock::now() < deadline) {
            (void)fx.ctx.poll();
            fx.ctx.interrupt_backend_waiters();
            std::this_thread::yield();
        }
        CONF_CHECK(f.name, cname, c1.ready());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);
        c1.reset();
        // The slot IS free (slot lifecycle released)...
        CONF_CHECK(f.name, cname, fx.slot_in_use() == 0);
        // ...but admission is still CLOSED: a fresh submit is rejected with
        // invalid_state and the Completion stays idle. Releasing the slot does
        // not re-open admission — the two lifecycles are orthogonal.
        auto r = fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4));
        CONF_CHECK(f.name, cname, !r.has_value());
        CONF_CHECK(f.name, cname,
                   r.error().code == IoError::Code::invalid_state);
        CONF_CHECK(f.name, cname, c2.idle());
        CONF_CHECK(f.name, cname, !c2.outstanding());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 0);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 1);
    });
}

// The close/drain-case driver. Returns the empty string on full pass, or the
// stable name of the FIRST failing case. Each case builds its OWN fixture via
// the driver-supplied factory (close_admission is irreversible), observes only
// through the shared boundary, and cleans up within its own frame.
std::string run_close_drain_cases(const BackendFactory& f,
                                  const MakeCloseDrainFixture& make_fx) {
    const std::size_t before = conf_failures().size();
    std::string failed;

    failed = case_close_rejects_future_submit(f, make_fx);
    if (!failed.empty()) {
        print_conf_failures_since(before);
        return failed;
    }

    failed = case_close_preserves_accepted_terminal(f, make_fx);
    if (!failed.empty()) {
        print_conf_failures_since(before);
        return failed;
    }

    failed = case_drain_then_reset_releases_slot(f, make_fx);
    if (!failed.empty()) {
        print_conf_failures_since(before);
        return failed;
    }

    failed = case_slot_released_but_admission_stays_closed(f, make_fx);
    if (!failed.empty()) {
        print_conf_failures_since(before);
        return failed;
    }

    return {};
}

}  // namespace sluice_test::conformance