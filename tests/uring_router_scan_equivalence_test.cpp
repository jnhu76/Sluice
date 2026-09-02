// TAX-0 EXP-U0 — router scan-direction semantic-equivalence gates.
//
// PROVES mechanically, over REACHABLE router states on a REAL ring, the
// invariant the SHIPPED reverse scan (T0-U-ROUTER R1 production landing,
// #255) depends on:
//
//     for every reachable router state R and every probed cookie k:
//         forward_lookup(R, k) == reverse_lookup(R, k)
//
// in found/not-found AND matched router index AND retirement behavior.
// The argument for why this must hold (live cookies are unique within
// backend lifetime — allocate_cookie_ is no-wrap — so the matching
// predicate `in_use && cookie == k` selects at most one entry regardless
// of traversal order) is NOT trusted alone: every case checks the REAL
// production find_live_router_cookie_ through the seam, in both
// directions (reverse_production — the shipped default — and
// forward_ablation, the pre-fix traversal kept as causal comparator).
//
// Also binds the EXP-0 static placement fact empirically (free-list
// back() allocation places the first D live entries at the HIGHEST
// indices when C >> D; forward scan therefore examines ~C entries per
// CQE while reverse examines ~D) and the exact scan-iteration accounting
// the U0-A witness reads.
//
// Real mode exercises the authoritative production uring_backend.cpp;
// stub mode proves build/API honesty only (cases are no-ops when the
// kernel provides no ring).
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <unistd.h>
#if defined(__unix__)
#include "death_test_runner_posix.hpp"
#endif
#endif

using namespace sluice::async;

#if defined(SLUICE_HAS_LIBURING)

namespace {

class TempFile {
  public:
    TempFile() {
        char path[] = "/tmp/sluice_uring_u0_eq_XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ >= 0)
            (void)::unlink(path);
    }
    ~TempFile() {
        if (fd_ >= 0)
            (void)::close(fd_);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    int fd() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

  private:
    int fd_ = -1;
};

// The cookie the k-th dispatch on a FRESH backend carries (no-wrap
// counter starts at 1).
constexpr std::uint64_t kth_cookie(std::size_t k) noexcept { return k + 1; }

// Exhaustive forward==reverse equivalence over one reachable router
// state, plus the exact placement/iteration witness for that state.
// `live` = the cookies of the K dispatched (not yet reaped) requests, in
// allocation order. On a fresh backend the k-th allocated cookie sits at
// router index C-1-k (free-list back() allocation), so a forward scan
// examines C-k entries and a reverse scan k+1 before the match.
void check_state_equivalence(UringAsyncBackend& backend, std::size_t capacity,
                             const std::vector<std::uint64_t>& live) {
    const std::size_t k_count = live.size();
    for (std::size_t k = 0; k < k_count; ++k) {
        const std::uint64_t cookie = live[k];

        backend.set_router_scan_mode_for_test(
            UringAsyncBackend::RouterScanModeForTest::forward_ablation);
        const std::size_t fwd = backend.find_live_router_cookie_for_test(cookie);
        const std::uint64_t fwd_iters =
            backend.router_scan_diagnostics_for_test().last_call_iterations;

        backend.set_router_scan_mode_for_test(
            UringAsyncBackend::RouterScanModeForTest::reverse_production);
        const std::size_t rev = backend.find_live_router_cookie_for_test(cookie);
        const std::uint64_t rev_iters =
            backend.router_scan_diagnostics_for_test().last_call_iterations;

        // THE invariant: identical semantic answer in both directions.
        SLUICE_CHECK(fwd == rev);
        SLUICE_CHECK(fwd < capacity);
        // Placement fact (fresh-backend back() allocation): allocation k
        // lives at index C-1-k.
        SLUICE_CHECK(fwd == capacity - 1 - k);
        // Exact iteration witness for the placement above.
        SLUICE_CHECK(fwd_iters == capacity - k);
        SLUICE_CHECK(rev_iters == k + 1);
    }

    // Probed misses: identical not-found in both directions.
    const std::uint64_t probes[] = {
        0,                                        // never emitted by this backend
        kth_cookie(k_count),                      // allocated next, not yet live
        (std::uint64_t{1} << 63u) | live[0],      // control-tagged value of a live
                                                  // cookie: different user_data space
        live[0] + 1000000,                        // unknown distant value
    };
    for (const std::uint64_t p : probes) {
        backend.set_router_scan_mode_for_test(
            UringAsyncBackend::RouterScanModeForTest::forward_ablation);
        const std::size_t fwd = backend.find_live_router_cookie_for_test(p);
        backend.set_router_scan_mode_for_test(
            UringAsyncBackend::RouterScanModeForTest::reverse_production);
        const std::size_t rev = backend.find_live_router_cookie_for_test(p);
        SLUICE_CHECK(fwd == capacity);
        SLUICE_CHECK(rev == capacity);
    }
}

// Drain every accepted request on the backend (bounded loop; poll drives
// dispatch + reap). Returns false on a stall.
bool drain(UringAsyncBackend& backend, int rounds = 10000) {
    while (backend.outstanding() > 0 && rounds-- > 0) {
        (void)backend.poll();
    }
    return backend.outstanding() == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Matrix: C x K reachable live-density states on fresh backends. Equivalence
// is checked for every live cookie AND probed misses; after the drain the
// same cookies must MISS in both modes (retirement equivalence — stale
// cookies resolve to nothing in either direction), and the real op-CQE path
// accounting must be exactly K operation lookups, all hits, zero control /
// transport lookups (no cancel, no transport failure in this workload).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_u0_scan_direction_equivalence_matrix) {
    const std::size_t capacities[] = {1, 2, 4, 8, 32};
    for (const std::size_t capacity : capacities) {
        const std::size_t k_max = capacity < 5 ? capacity : 5;
        for (std::size_t k_count = 1; k_count <= k_max; ++k_count) {
            UringAsyncBackend backend{UringConfig{capacity, 8}};
            if (!backend.available())
                return;
            // Seam default must be the shipped production scan (reverse).
            SLUICE_CHECK(backend.router_scan_mode_for_test() ==
                         UringAsyncBackend::RouterScanModeForTest::reverse_production);
            backend.reset_router_scan_diagnostics_for_test();

            TempFile file;
            SLUICE_CHECK(file.valid());
            std::vector<std::byte> buf(k_count * 4096, std::byte{0x55});
            std::vector<Completion<std::size_t>> comp(k_count);

            std::vector<std::uint64_t> live;
            for (std::size_t k = 0; k < k_count; ++k) {
                // Distinct offsets: the k-th live cookie is independently
                // enumerable via live_cookie_for_offset_for_test.
                const std::uint64_t off = static_cast<std::uint64_t>(k) * 4096;
                SLUICE_CHECK(backend
                                 .submit_write(WriteOp{file.fd(), buf.data() + off,
                                                       4096, off},
                                               comp[k])
                                 .has_value());
                live.push_back(kth_cookie(k));
            }
            SLUICE_CHECK(backend.live_cookies_for_test() == k_count);
            // Cross-check the predicted cookies against the REAL router.
            for (std::size_t k = 0; k < k_count; ++k) {
                const auto got = backend.live_cookie_for_offset_for_test(
                    static_cast<std::uint64_t>(k) * 4096);
                SLUICE_CHECK(got.has_value() && *got == live[k]);
            }

            check_state_equivalence(backend, capacity, live);

            // Consume the diagnostic probes, then check the real CQE path.
            backend.reset_router_scan_diagnostics_for_test();
            SLUICE_CHECK(drain(backend));
            SLUICE_CHECK(backend.live_cookies_for_test() == 0);

            const auto& diag = backend.router_scan_diagnostics_for_test();
            SLUICE_CHECK(diag.operation_cookie_lookup_calls == k_count);
            SLUICE_CHECK(diag.lookup_hits == k_count);
            SLUICE_CHECK(diag.lookup_misses == 0);
            SLUICE_CHECK(diag.control_cookie_lookup_calls == 0);
            SLUICE_CHECK(diag.transport_cookie_lookup_calls == 0);

            // Retirement equivalence: every former live cookie now misses
            // in BOTH directions through the real lookup.
            for (const std::uint64_t cookie : live) {
                backend.set_router_scan_mode_for_test(
                    UringAsyncBackend::RouterScanModeForTest::forward_ablation);
                SLUICE_CHECK(backend.find_live_router_cookie_for_test(cookie) ==
                             capacity);
                backend.set_router_scan_mode_for_test(
                    UringAsyncBackend::RouterScanModeForTest::reverse_production);
                SLUICE_CHECK(backend.find_live_router_cookie_for_test(cookie) ==
                             capacity);
            }
            for (auto& c : comp) {
                SLUICE_CHECK(c.ready());
                c.reset();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Real-CQE path equivalence + stale/duplicate drop: the identical semantic
// scenario (one write, reap, stale re-injection of the retired cookie) in
// forward and reverse modes must produce identical outcomes and identical
// accounting SHAPES — only the iteration magnitudes differ (that
// difference is the ablation's whole point).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_u0_cqe_path_and_stale_drop_equivalence) {
    const std::size_t capacity = 32;
    for (const auto mode : {UringAsyncBackend::RouterScanModeForTest::forward_ablation,
                            UringAsyncBackend::RouterScanModeForTest::reverse_production}) {
        UringAsyncBackend backend{UringConfig{capacity, 8}};
        if (!backend.available())
            return;
        backend.set_router_scan_mode_for_test(mode);
        backend.reset_router_scan_diagnostics_for_test();

        TempFile file;
        SLUICE_CHECK(file.valid());
        std::byte buf[8]{std::byte{0x5A}};
        Completion<std::size_t> c;
        SLUICE_CHECK(backend.peek_next_cookie_for_test() == 1);
        SLUICE_CHECK(
            backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());

        SLUICE_CHECK(drain(backend));
        SLUICE_CHECK(c.ready());
        const auto res = c.result();
        SLUICE_CHECK(res.has_value() && res.value() == 8);
        c.reset();

        const auto& diag = backend.router_scan_diagnostics_for_test();
        SLUICE_CHECK(diag.operation_cookie_lookup_calls == 1);
        SLUICE_CHECK(diag.lookup_hits == 1);
        SLUICE_CHECK(diag.lookup_misses == 0);
        SLUICE_CHECK(diag.control_cookie_lookup_calls == 0);
        SLUICE_CHECK(diag.transport_cookie_lookup_calls == 0);
        // Direction-specific exact iteration witness at C=32 (live entry at
        // index 31 before retirement).
        SLUICE_CHECK(diag.operation_lookup_iterations_max ==
                     (mode == UringAsyncBackend::RouterScanModeForTest::forward_ablation
                          ? 32
                          : 1));

        // Stale duplicate of the retired cookie: dropped through the SAME
        // operation-CQE path, no second terminal, accounted as a miss.
        backend.inject_cqe_for_test(1, 8);
        SLUICE_CHECK(backend.poll() == 0);
        SLUICE_CHECK(!c.ready());
        const auto& diag2 = backend.router_scan_diagnostics_for_test();
        SLUICE_CHECK(diag2.operation_cookie_lookup_calls == 2);
        SLUICE_CHECK(diag2.lookup_misses == 1);
        SLUICE_CHECK(backend.outstanding() == 0);
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    }
}

// ---------------------------------------------------------------------------
// Cancel / tagged-control path equivalence. The running-cancel intent +
// original-CQE-wins scenario (mirrors uring_c2b_running_cancel_intent_real_
// result and uring_c2b_original_cqe_before_control_cqe) must hold in BOTH
// scan modes with identical dispositions: intent only while running, the
// original kernel result verbatim, exactly one publication, control
// reference retired, and the control-CQE cookie lookup accounted on the
// control side in both modes.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_u0_cancel_control_path_equivalence) {
    for (const auto mode : {UringAsyncBackend::RouterScanModeForTest::forward_ablation,
                            UringAsyncBackend::RouterScanModeForTest::reverse_production}) {
        // A fresh pipe per mode: the scenario needs a kernel-BLOCKED read
        // (empty pipe) whose write end is closed later to deliver the
        // deterministic EOF original result.
        int pipe_fds[2] = {-1, -1};
        if (::pipe(pipe_fds) != 0)
            return;
        UringAsyncBackend backend{UringConfig{8, 8}};
        if (!backend.available()) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            return;
        }
        backend.set_router_scan_mode_for_test(mode);
        backend.reset_router_scan_diagnostics_for_test();

        std::byte buf[4]{};
        Completion<std::size_t> c;
        SLUICE_CHECK(
            backend
                .submit_read(ReadOp{pipe_fds[0], buf, 4, 0}, c)
                .has_value());
        SLUICE_CHECK(backend.poll() == 0); // kernel blocks the read

        auto h = backend.handle_for_completion_for_test(&c);
        SLUICE_CHECK(h.has_value());
        SLUICE_CHECK(backend.cancel_handle_for_test(*h) ==
                     detail::CancelDisposition::intent_recorded);
        SLUICE_CHECK(backend.live_control_entries_for_test() == 1);
        SLUICE_CHECK(!c.ready()); // intent published nothing

        // Original kernel result wins verbatim (EOF = 0 bytes).
        SLUICE_CHECK(::close(pipe_fds[1]) == 0);
        SLUICE_CHECK(backend.poll() == 1);
        SLUICE_CHECK(c.ready());
        const auto res = c.result();
        SLUICE_CHECK(res.has_value() && res.value() == 0);
        c.reset();
        SLUICE_CHECK(backend.outstanding() == 0);
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
        SLUICE_CHECK(backend.live_cookies_for_test() == 0);
        SLUICE_CHECK(backend.live_control_entries_for_test() == 0);
        SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);

        // Exact per-family accounting in BOTH modes: one operation-CQE
        // lookup, one tagged-control-CQE lookup, one transport-accounting
        // lookup for the consumed cancel-control SQE, zero misses.
        const auto& diag = backend.router_scan_diagnostics_for_test();
        SLUICE_CHECK(diag.operation_cookie_lookup_calls == 1);
        SLUICE_CHECK(diag.control_cookie_lookup_calls == 1);
        SLUICE_CHECK(diag.transport_cookie_lookup_calls == 1);
        SLUICE_CHECK(diag.lookup_misses == 0);
        ::close(pipe_fds[0]);
    }
}

// ---------------------------------------------------------------------------
// #262 S2 harness-robustness regression: a stalled CQE delivery must surface
// as a NAMED drain failure, never as the silent teardown abort.
//
// Failure mode under investigation (#262 S2): when the kernel cannot deliver
// a CQE within the bounded non-blocking spin window (slow or emulated storage
// under load), the historical harness returned false from drain() and let the
// case continue; the still-outstanding completions then destructed through
// the SILENT completion-authority fail-fast — "terminate called without an
// active exception", the misleading teardown abort reported on #262.
//
// The child below makes that state deterministic on any host: one accepted
// read on an empty pipe with no writer can NEVER complete. It installs a
// terminate handler that pins the silent-fail-fast exit code, runs the
// harness drain, and reports:
//   pre-escalation: drain returns false -> scope exit destroys the
//                   outstanding completion -> SILENT fail-fast
//                   (kExpectedTerminateExit, no diagnostic) — the RED state.
//   post-escalation: drain never returns false; its watchdog names the stall
//                   and exits kDrainStallLoudExit — the GREEN state.
// ---------------------------------------------------------------------------
constexpr int kDrainStallLoudExit = 89;

int run_drain_stall_child() {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0)
        return sluice_death_test::kChildTestFailExit;
    UringAsyncBackend backend{UringConfig{8, 8}};
    if (!backend.available()) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return sluice_death_test::kChildTestFailExit;
    }
    std::byte buf[4]{};
    Completion<std::size_t> c;
    if (!backend.submit_read(ReadOp{pipe_fds[0], buf, 4, 0}, c).has_value()) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return sluice_death_test::kChildTestFailExit;
    }
    (void)backend.poll(); // dispatch + submit; the kernel owns the blocked read
    std::set_terminate(
        [] { std::_Exit(sluice_death_test::kExpectedTerminateExit); });
    const bool drained = drain(backend);
    if (drained)
        return sluice_death_test::kUnexpectedReturnExit; // impossible: no writer
    // Scope exit below destroys the still-outstanding completion.
    // PRE-ESCALATION this is the silent fail-fast (the pinned exit 86 fires
    // before this return value can materialize). POST-ESCALATION this line is
    // unreachable: drain's watchdog already named the stall and exited with
    // kDrainStallLoudExit from inside drain().
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return sluice_death_test::kChildTestFailExit;
}

SLUICE_TEST_CASE(uring_u0_drain_stall_is_loud_not_silent) {
    bool timed_out = false;
    const int status =
        sluice_death_test::fork_exec_child("drain-stall-equivalence", timed_out);
    SLUICE_CHECK_MSG(!timed_out,
                     "drain-stall child exceeded the 60s parent watchdog");
    SLUICE_CHECK(WIFEXITED(status));
    const int code = WEXITSTATUS(status);
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "expected the loud drain-stall exit (%d), got %d (%d = the "
                  "SILENT completion-authority fail-fast on outstanding-"
                  "completion destruction: the pre-escalation harness gap)",
                  kDrainStallLoudExit, code,
                  sluice_death_test::kExpectedTerminateExit);
    SLUICE_CHECK_MSG(code == kDrainStallLoudExit, msg);
}

#else

// Stub builds: build/API honesty only (no ring -> no router to scan).
SLUICE_TEST_CASE(uring_u0_scan_equivalence_stub_build_compile) {}

#endif

#if defined(SLUICE_HAS_LIBURING) && defined(__unix__)
// Self-exec child dispatch (death_test_runner_posix.hpp convention): the
// forked child re-runs this binary with --death-child=<case> and executes
// only the named child body.
int main(int argc, char** argv) {
    const std::size_t prefix_len =
        std::strlen(sluice_death_test::kChildArgPrefix);
    if (argc == 2 && std::strncmp(argv[1], sluice_death_test::kChildArgPrefix,
                                  prefix_len) == 0) {
        const char* which = argv[1] + prefix_len;
        if (std::strcmp(which, "drain-stall-equivalence") == 0)
            return run_drain_stall_child();
        return sluice_death_test::kChildTestFailExit;
    }
    return ::sluice_test::run_all();
}
#else
SLUICE_MAIN()
#endif
