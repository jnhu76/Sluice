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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
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

}  // namespace sluice_test::conformance
