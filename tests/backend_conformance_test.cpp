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
struct case_bail {};
#define CONF_CHECK(backend, case_name, cond)                                            \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            conf_failures().push_back({(backend), (case_name), #cond, __FILE__, __LINE__}); \
            throw case_bail{};                                                          \
        }                                                                               \
    } while (0)

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
        for (std::size_t i = before; i < conf_failures().size(); ++i) {
            const auto& fl = conf_failures()[i];
            std::printf("[conformance] FAIL %s :: %s : %s (%s:%d)\n",
                        fl.backend.c_str(), fl.case_name.c_str(),
                        fl.expr.c_str(), fl.file.c_str(), fl.line);
        }
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
// successful submit is funneled through submit_and_track(), which registers
// the Completion into `accepted` BEFORE the case inspects the result, so a
// deliberately-nonconforming backend that wrongly accepts the (N+1)th op is
// still cleaned up even if the case throws right after.
// ===========================================================================

// A small RAII fd holder for real_mode capacity cases. ThreadPool rejects
// fd < 0 with invalid_argument (pre-commit descriptor validation), so a real
// open fd is required to reach the capacity path on a real-syscall backend.
// Fake has no descriptor validation; it accepts fd=-1 at capacity pressure.
struct ScopedTempFd {
    int fd = -1;
    explicit ScopedTempFd(int f) : fd(f) {}
    ~ScopedTempFd() { if (fd >= 0) ::close(fd); }
    ScopedTempFd(const ScopedTempFd&) = delete;
    ScopedTempFd& operator=(const ScopedTempFd&) = delete;
    operator int() const noexcept { return fd; }
};

// Open a temp fd for a real_mode capacity run; -1 if the factory is non-real.
inline int capacity_temp_fd(const BackendFactory& f) {
    return f.make_temp_fd ? f.make_temp_fd() : -1;
}

struct CapacityFixture {
    AsyncIoContext ctx;
    AsyncStats stats;
    // Raw pointers into caller-owned Completions; the test owns the storage
    // and MUST keep the Completions alive until after cleanup_or_abort().
    std::vector<Completion<std::size_t>*> accepted;

    explicit CapacityFixture(std::unique_ptr<AsyncBackend> backend)
        : ctx(std::move(backend), &stats) {}

    // Build a ReadOp that the backend will accept under capacity pressure.
    // real_mode backends need a real fd; Fake accepts any fd form.
    ReadOp make_read_op(int fd, std::byte* dst, std::size_t len) const {
        return ReadOp{fd, dst, len, 0};
    }

    // Every successful submit MUST be tracked BEFORE the case inspects it, so a
    // deliberately-nonconforming backend that wrongly accepts the (N+1)th op is
    // still cleaned up even if the case throws right after (CORRECTION 7b).
    // Register FIRST, assert LATER.
    Result<void> submit_and_track(Completion<std::size_t>& c, ReadOp op) {
        auto r = ctx.submit_read(op, c);
        if (r.has_value()) accepted.push_back(&c);
        return r;
    }

    // Cleanup is explicit and un-ignorable. It is NOT built on SLUICE_CHECK
    // (which returns out of the current function) and does NOT use a fixed
    // `guard < 10000` loop. It is time-bounded; if the deadline passes with
    // outstanding work, it prints a precise diagnostic and abort()s so the
    // failure cause is the capacity case, not a context-destructor violation.
    void cleanup_or_abort(const char* backend_name, const char* case_name) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);

        // 1. Cancel everything still outstanding.
        for (auto* c : accepted) {
            if (c->outstanding()) ctx.cancel(*c);
        }
        // 2. Drive poll/reap until outstanding hits 0 or the deadline expires.
        //    interrupt_backend_waiters() re-arms waiters; yield() lets workers
        //    progress. This is liveness, not ordering proof — cancel may still
        //    yield the real syscall result on ThreadPool (a valid terminal).
        while (ctx.outstanding() != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            (void)ctx.poll();
            ctx.interrupt_backend_waiters();
            std::this_thread::yield();
        }
        if (ctx.outstanding() != 0) {
            // Precise diagnostic — do NOT let the AsyncIoContext destructor's
            // fail-fast fire and mask this as an unrelated L11 violation.
            std::fprintf(stderr,
                "CapacityFixture: cleanup deadline expired "
                "(backend=%s case=%s outstanding=%zu accepted=%zu)\n",
                backend_name, case_name, ctx.outstanding(), accepted.size());
            for (std::size_t i = 0; i < accepted.size(); ++i) {
                const auto* c = accepted[i];
                std::fprintf(stderr,
                    "  accepted[%zu]: idle=%d outstanding=%d ready=%d\n",
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
        for (auto* c : accepted) {
            if (c->ready()) c->reset();
        }
    }
};

// Each capacity case is a SELF-CONTAINED scope that owns, in ONE stack frame,
// its CapacityFixture + Completions + cleanup. The macro CAPACITY_CASE opens a
// brace + declares `cname` + builds the fixture + opens a try; END_CAPACITY_CASE
// calls cleanup_or_abort() (success path), closes the try, adds the catch that
// ALSO calls cleanup_or_abort() and returns the failing case name, and closes
// the outer brace. The case statements go BETWEEN the two macros WITHOUT an
// extra brace — an extra `{ ... }` would put the Completions in an inner block
// that destructs BEFORE END_CAPACITY_CASE runs cleanup, defeating the design.
//
//   CAPACITY_CASE(f, "case_name", CAP)
//       <statements using `fx` and `cname`; declarations live in the try scope>
//   END_CAPACITY_CASE(f)
//
// This is the only scope-correct shape: fixture and Completions share the try
// frame, cleanup runs before either destructs (so the accepted-pointers stay
// valid and the context is quiescent before destruction), and a CONF_CHECK
// case_bail still reaches cleanup via the catch.
#define CAPACITY_CASE(f, case_name_str, cap_value)                                   \
    {                                                                                \
        const char* cname = (case_name_str);                                         \
        CapacityFixture fx((f).make_backend_with_capacity(cap_value));               \
        try {

// Closes the CAPACITY_CASE block: runs cleanup_or_abort() on the success path
// (no exception) and on the exception path (case_bail), then returns the
// failing case name on failure or empty on success. The macro must be matched
// 1:1 with CAPACITY_CASE.
#define END_CAPACITY_CASE(f)                                                         \
        fx.cleanup_or_abort((f).name, cname);                                        \
        } catch (...) {                                                              \
            fx.cleanup_or_abort((f).name, cname);                                    \
            return cname;                                                            \
        }                                                                            \
    }

// The capacity-case driver. Returns the empty string on full pass, or the
// stable name of the FIRST failing case. Drives ONLY the capacity cases against
// a backend built at a chosen small request_capacity via
// factory.make_backend_with_capacity. Precondition: factory_supports_capacity.
//
// Each case is a CAPACITY_CASE...END_CAPACITY_CASE block: the fixture + the
// Completions + cleanup all live in ONE scope, so cleanup_or_abort() runs while
// the Completions are still on the stack (the accepted-pointers stay valid) and
// on BOTH the success and exception paths (a CONF_CHECK case_bail still reaches
// cleanup before the fixture destructs).
std::string run_capacity_cases(const BackendFactory& f) {
    // ---- Case A: accepts exact capacity -----------------------------------
    // capacity=2: c1 accept, c2 accept, outstanding==2, submit_calls==2,
    // submitted_ops==2, max_outstanding==2, queue_full_retries==0,
    // invalid_state_rejections==0.
    CAPACITY_CASE(f, "capacity_accepts_exact_limit", 2)
        ScopedTempFd fd(capacity_temp_fd(f));
        std::byte buf1[4]{}, buf2[4]{};
        Completion<std::size_t> c1, c2;
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
    END_CAPACITY_CASE(f)

    // ---- Case B: the (N+1)th submit is rejected with would_block -----------
    // c1, c2 accept; c3 returns would_block; c3 stays idle; outstanding==2;
    // submitted_ops==2; submit_calls==3; queue_full_retries==1;
    // invalid_state_rejections==0; max_outstanding==2.
    CAPACITY_CASE(f, "capacity_rejects_with_idle_completion", 2)
        ScopedTempFd fd(capacity_temp_fd(f));
        std::byte buf1[4]{}, buf2[4]{}, buf3[4]{};
        Completion<std::size_t> c1, c2, c3;
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4)).has_value());
        // c3 must be REJECTED for capacity. NOT tracked (not accepted).
        auto r3 = fx.ctx.submit_read(fx.make_read_op(fd, buf3, 4), c3);
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
    END_CAPACITY_CASE(f)

    // ---- Case C: a rejected request never produces a late completion -------
    // cap=1: c1 accept, c2 rejected (would_block). Then drive backend progress,
    // clean up c1 (cancel -> reap). c2 must remain idle/not-outstanding/not-
    // ready throughout and after — no completion publication belongs to c2.
    CAPACITY_CASE(f, "capacity_rejection_never_completes", 1)
        ScopedTempFd fd(capacity_temp_fd(f));
        std::byte buf1[4]{}, buf2[4]{};
        Completion<std::size_t> c1, c2;
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        auto r2 = fx.ctx.submit_read(fx.make_read_op(fd, buf2, 4), c2);
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
        CONF_CHECK(f.name, cname, c2.idle());
        CONF_CHECK(f.name, cname, !c2.ready());
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 1);
    END_CAPACITY_CASE(f)

    // ---- Case D: rejection classification is precise ----------------------
    // Deterministic sequence: c1 accept; resubmit on non-idle c1 ->
    // invalid_state; c2 accept; c3 -> would_block. Exact stats (CORRECTION 5):
    //   submit_calls==4, submitted_ops==2, invalid_state_rejections==1,
    //   queue_full_retries==1, max_outstanding==2, outstanding==2.
    CAPACITY_CASE(f, "capacity_stats_are_exact", 2)
        ScopedTempFd fd(capacity_temp_fd(f));
        std::byte buf1[4]{}, buf1b[4]{}, buf2[4]{}, buf3[4]{};
        Completion<std::size_t> c1, c2, c3;
        // 1. c1 accept.
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c1, fx.make_read_op(fd, buf1, 4)).has_value());
        // 2. Resubmit on the still-non-idle c1 -> invalid_state (caller
        //    lifecycle violation, NOT capacity). c1 already tracked.
        auto r1b = fx.ctx.submit_read(fx.make_read_op(fd, buf1b, 4), c1);
        CONF_CHECK(f.name, cname, !r1b.has_value());
        CONF_CHECK(f.name, cname,
                   r1b.error().code == IoError::Code::invalid_state);
        // 3. c2 accept.
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4)).has_value());
        // 4. c3 -> would_block (capacity full).
        auto r3 = fx.ctx.submit_read(fx.make_read_op(fd, buf3, 4), c3);
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
    END_CAPACITY_CASE(f)

    // ---- Case E: capacity recycles after cancel -> reap -> reset ----------
    // cap=1: c1 accept (fills capacity). Cancel + reap c1, then reset c1
    // (releases the slot). A fresh c2 submit MUST succeed (capacity recycled).
    CAPACITY_CASE(f, "capacity_recycles_after_reset", 1)
        ScopedTempFd fd(capacity_temp_fd(f));
        std::byte buf1[4]{}, buf2[4]{};
        Completion<std::size_t> c1, c2;
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
        // c1 is no longer tracked as accepted (it was reset); drop it so
        // cleanup does not touch it again.
        for (auto it = fx.accepted.begin(); it != fx.accepted.end(); ++it) {
            if (*it == &c1) { fx.accepted.erase(it); break; }
        }
        // A fresh submit MUST succeed (capacity recycled).
        CONF_CHECK(f.name, cname,
                   fx.submit_and_track(c2, fx.make_read_op(fd, buf2, 4)).has_value());
        CONF_CHECK(f.name, cname, fx.ctx.outstanding() == 1);
        CONF_CHECK(f.name, cname, fx.stats.submit_calls == 2);
        CONF_CHECK(f.name, cname, fx.stats.submitted_ops == 2);
        CONF_CHECK(f.name, cname, fx.stats.max_outstanding == 1);
    END_CAPACITY_CASE(f)

    return {};
}

}  // namespace sluice_test::conformance