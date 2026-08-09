// Shared AsyncBackend conformance suite (sluice-CORE-024, B1).
//
// One parameterized harness exercising every genuinely-shared backend semantic
// against every backend (Fake, ThreadPool, Uring-real, Uring-stub). Backend-
// specific mechanism (io_uring SQE pressure, ThreadPool worker count) stays in
// backend-specific tests. Per task §6: assert shared SEMANTIC outcomes, never
// require backend-specific PHYSICAL behavior to be identical.
//
// Usage: a backend test instantiates this with a factory + a temp-fd provider
// and calls run_conformance(). Cases that need a real fd (read/write content
// verification) are skipped when the backend is not "real" (Fake has no kernel;
// Uring stub returns backend_error). The skip is queried via `real_mode`.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>  // ::close (ScopedTempFd)
#include <utility>   // std::move (CapacityFixture ctor)
#include <vector>

namespace sluice_test::conformance {

// A factory that produces a fresh backend, plus a way to open a temp fd the
// backend can do real I/O on (only used when real_mode). real_mode=false means
// the backend cannot do real I/O (Fake, or Uring stub without liburing) — the
// fd-backed cases are skipped cleanly.
//
// Phase C1 classification fields (profile + mode): these are the stable,
// machine-readable identity the aggregate gate reads via [conformance-meta]
// lines. They are HIGHER-LEVEL than real_mode (which still controls per-case
// skip logic inside the suite) and exist so the gate does not have to infer a
// backend's family/mode from display names or skip text. Values:
//   profile: one of ReferenceProfile / BlockingIoProfile / KernelIoProfile.
//   mode:    deterministic | real | stub.
//
// Phase C2a capacity seam: the OPTIONAL make_backend_with_capacity constructs a
// backend at a CHOSEN request_capacity. It is the authoritative way the shared
// capacity cases (run_capacity_cases) build a small bounded backend. Every
// current real backend supplies this seam. Uring stub mode leaves it null and
// the aggregate gate classifies that run as INCOMPLETE; there is no
// driver-side skip-as-pass. The default zero-arg make_backend is preserved so
// the existing shared cases are unchanged.
struct BackendFactory {
    using MakeBackend =
        std::function<std::unique_ptr<sluice::async::AsyncBackend>()>;
    using MakeBackendWithCapacity =
        std::function<std::unique_ptr<sluice::async::AsyncBackend>(std::size_t)>;
    using MakeTempFd = std::function<int()>;  // returns an open rw fd; -1 if unsupported

    const char* name;
    MakeBackend make_backend;                       // default-capacity path
    MakeBackendWithCapacity make_backend_with_capacity = nullptr;  // optional
    MakeTempFd make_temp_fd;  // may be nullptr when not real_mode
    bool real_mode;           // can do real kernel I/O
    const char* profile;      // Phase C1 closed-profile classification
    const char* mode;         // Phase C1 execution mode classification
};

// Whether a factory can construct a backend at a chosen capacity. A false
// result is never a semantic PASS: the aggregate gate classifies a Uring stub
// capacity run as INCOMPLETE.
inline bool factory_supports_capacity(const BackendFactory& f) {
    return f.make_backend_with_capacity != nullptr;
}

// The conformance-failure exception type. A CONF_CHECK throws this to bail out
// of a case; run_conformance / run_capacity_case catch it and record the case
// as failed. Defined in the header so C++ regression tests that drive
// run_capacity_case directly can name the type.
struct case_bail {};

// Records a conformance skip (not a failure). Printed for visibility.
inline void note_skip(const char* backend, const char* case_name, const char* reason) {
    std::printf("[conformance] skip %s :: %s (%s)\n", backend, case_name, reason);
}

// Phase C1: emit a stable machine-readable identity line for a backend BEFORE
// its shared-suite case runs. The aggregate gate parses ONLY these
// [conformance-meta] lines to classify backends. Must be printed exactly once
// per registered backend, before any [conformance] skip/FAIL line for it.
inline void emit_meta(const BackendFactory& factory) {
    std::printf("[conformance-meta] backend=%s profile=%s mode=%s\n",
                factory.name, factory.profile, factory.mode);
}

// The shared suite. Returns 0 on full pass, 1 on any failure. Skips are not
// failures. Each case is self-contained: it builds its own context + buffers.
// Implemented out-of-line in backend_conformance_test.cpp so adding a case is
// one vertical slice (RED on Fake -> GREEN -> parameterize to other backends).
int run_conformance(const BackendFactory& factory);

// Phase C2a — shared capacity/admission/accounting cases. Drives ONLY the
// capacity cases against a backend built at a chosen small request_capacity via
// factory.make_backend_with_capacity. Returns:
//   * the empty string  — all capacity cases passed;
//   * a non-empty string — the stable name of the FIRST failing capacity case
//     (diagnosable; never just a bool or a whole-suite exit code, so a validity
//     fixture can assert the SPECIFIC case that caught a defect).
//
// Precondition: factory_supports_capacity(factory). A factory without the seam
// MUST NOT call this. Implemented out-of-line in backend_conformance_test.cpp.
std::string run_capacity_cases(const BackendFactory& factory);

// ===========================================================================
// Phase C2a capacity fixture + uniform case wrapper (defined in the header so
// C++ regression tests in capacity_validity_test.cpp can drive them directly).
//
// CapacityFixture owns the AsyncIoContext + AsyncStats + the tracked-Completion
// list. It is the cleanup authority: cleanup_or_abort() is explicit,
// time-bounded, and abort()s on timeout (never lets the AsyncIoContext
// destructor fail-fast mask a capacity assertion). submit_and_track() registers
// EVERY Completion a case attempts to submit BEFORE calling submit_read — a
// normal accept, a rejection that leaves it idle (no-op for cleanup), a
// Completion a broken backend ILLEGALLY binds before returning a rejection
// error, and a Completion a broken backend binds/publishes LATER during
// backend progress — so cleanup can terminalize it (Issue #68 Rev 3 cleanup
// principle).
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

// Temp-fd setup guard used by every capacity case. A real_mode backend
// (ThreadPool) rejects fd < 0 with invalid_argument during PRE-COMMIT
// descriptor validation, i.e. BEFORE the capacity path is reached — so a
// failed temp-file setup would be misreported as a capacity rejection. On a
// non-real backend (Fake, validity fixture) fd == -1 is the NORMAL form the
// backend accepts at capacity pressure, and the guard is a no-op. Returns a
// non-empty diagnostic (distinct from any capacity case name) when a real_mode
// run could not create its temp fd; the caller reports it separately.
inline std::string capacity_temp_fd_setup_error(const BackendFactory& f,
                                                int fd) {
    if (f.real_mode && fd < 0) {
        return "temp_fd_setup_failed";
    }
    return {};
}

struct CapacityFixture {
    // stats is declared BEFORE ctx so it is constructed first and destroyed
    // LAST: AsyncIoContext holds a pointer to it for its whole lifetime, so it
    // must outlive ctx (member destruction is reverse declaration order; a
    // stats declared after ctx would be destroyed before ctx).
    sluice::AsyncStats stats;
    sluice::async::AsyncIoContext ctx;
    // Raw pointers into caller-owned Completions; the test owns the storage
    // and MUST keep the Completions alive until after cleanup_or_abort().
    //
    // `tracked` holds EVERY Completion a case attempted to submit (not only
    // accepted ones): cleanup only acts on outstanding (cancel) / ready
    // (reset) Completions, so tracking a rejected-but-idle Completion is a
    // no-op for cleanup — and it closes the window where a broken backend
    // returns an error while the Completion is still idle, then ILLEGALLY
    // binds/publishes it later during backend progress (late_bind_only /
    // late_complete violations). Such a Completion would otherwise be
    // unreachable by cleanup and its destructor/context fail-fast would mask
    // the real capacity assertion.
    std::vector<sluice::async::Completion<std::size_t>*> tracked;

    explicit CapacityFixture(std::unique_ptr<sluice::async::AsyncBackend> backend)
        : ctx(std::move(backend), &stats) {}

    // Build a ReadOp that the backend will accept under capacity pressure.
    // real_mode backends need a real fd; Fake accepts any fd form.
    sluice::async::ReadOp make_read_op(int fd, std::byte* dst, std::size_t len) const {
        return sluice::async::ReadOp{fd, dst, len, 0};
    }

    // Track a Completion into `tracked` exactly once. A repeat registration
    // (e.g. an invalid-state resubmit of an already-tracked Completion) MUST
    // NOT push a second entry, or cleanup would cancel/reap/reset the same
    // Completion twice.
    void track_once(sluice::async::Completion<std::size_t>& c) {
        if (std::find(tracked.begin(), tracked.end(), &c) == tracked.end()) {
            tracked.push_back(&c);
        }
    }

    // EVERY capacity-case submit — including the ones that EXPECT a rejection —
    // MUST go through this helper (Issue #68 Rev 3 cleanup principle: before a
    // case inspects the submit result, every Completion that may be claimed by
    // the backend must be reachable by cleanup). The Completion is registered
    // BEFORE submit_read (not after, and not conditionally on the result):
    //   * a normal accept is tracked so cancel/reap/reset can terminalize it;
    //   * a rejection that leaves the Completion idle is tracked as a no-op —
    //     cleanup ignores idle Completions;
    //   * a deliberately-broken backend that ILLEGALLY binds the Completion and
    //     THEN returns an error (bind_rejected-would_block, the non-idle c1
    //     resubmit in stats_are_exact), or that binds/publishes it LATER during
    //     backend progress (late_bind_only / late_complete) is still tracked,
    //     so cleanup terminalizes it instead of letting a destructor fail-fast
    //     mask the real capacity assertion.
    // Register FIRST (track_once), assert LATER.
    sluice::Result<void> submit_and_track(sluice::async::Completion<std::size_t>& c,
                                          sluice::async::ReadOp op) {
        track_once(c);
        return ctx.submit_read(op, c);
    }

    // Cleanup is explicit and un-ignorable. It is NOT built on SLUICE_CHECK
    // (which returns out of the current function) and does NOT use a fixed
    // `guard < 10000` loop. It is time-bounded; if the deadline passes with
    // outstanding work, it prints a precise diagnostic and abort()s so the
    // failure cause is the capacity case, not a context-destructor violation.
    void cleanup_or_abort(const char* backend_name, const char* case_name);
};

// Uniform case wrapper: cleanup runs on BOTH the success and exception paths,
// so a case-failure exception (case_bail from CONF_CHECK) cannot leave
// outstanding work behind for the context destructor to detect. The Completions
// live in the CALLER's frame (the case function), which is still alive when
// cleanup runs.
//
// CATCH-ALL (review finding): the wrapper MUST NOT only catch `case_bail`. If
// the case body throws a DIFFERENT exception (std::bad_alloc,
// std::runtime_error, ...) a bare `catch (const case_bail&)` would unwind
// without cleanup; the AsyncIoContext destructor's fail-fast would then fire
// and mask the original exception. A `catch (...)` runs cleanup FIRST and then
// rethrows, so the original exception still propagates while no destructor
// masks it. The catch-all does NOT convert an unknown exception into a normal
// case failure (case_bail returns the failing case name; the catch-all
// rethrows).
template <typename Body>
std::string run_capacity_case(CapacityFixture& fx, const char* backend_name,
                              const char* case_name, Body&& body) {
    try {
        body();
    } catch (const case_bail&) {
        fx.cleanup_or_abort(backend_name, case_name);
        return case_name;  // failure: return the failing case name
    } catch (...) {
        // Unknown exception: cleanup FIRST (so no outstanding Completion can
        // trip the AsyncIoContext destructor's fail-fast and mask the cause),
        // then rethrow so the driver records the original failure.
        fx.cleanup_or_abort(backend_name, case_name);
        throw;
    }
    fx.cleanup_or_abort(backend_name, case_name);
    return {};  // success
}

// ===========================================================================
// Phase C2e — shared close/drain/destruction cases (Issue #68 rows 15-16).
//
// CloseDrainFixture owns the AsyncIoContext + AsyncStats + the tracked-
// Completion list for the shared close/drain cases. The DRIVER wires two
// per-backend seams as instance-level closures (AGENTS.md §15 — production
// builds carry nothing; the suite itself stays backend-agnostic):
//   close       — routes to the concrete backend's public close_admission()
//                 (FakeAsyncBackend / ThreadPoolBackend both expose it;
//                 ADR Decision 15 reference semantics).
//   slot_in_use — returns the backend arena's slot_in_use count, the ONLY
//                 way to observe "reaped-but-unreset still holds the slot"
//                 (drained != releasable backend destruction) at the shared
//                 boundary.
// The cases assert ONLY AsyncIoContext-observable state plus those two wired
// counts; no downcast, no backend-specific mechanism.
//
// CLEANUP MODEL (CapacityFixture parity): cleanup_or_abort() is explicit,
// time-bounded, and abort()s on timeout so the failure cause is the C2e case,
// not a context-destructor fail-fast. Every submit (successful or
// expected-rejection) goes through submit_and_track so a broken backend that
// illegally binds a rejected Completion is still cleaned up. Cancel remains
// legal after close (ADR Decision 15), so cleanup can always terminalize.
// ===========================================================================

struct CloseDrainFixture {
    // stats declared BEFORE ctx (constructed first, destroyed LAST — the
    // context holds a pointer to it for its whole lifetime; see
    // CapacityFixture's declaration-order note).
    sluice::AsyncStats stats;
    sluice::async::AsyncIoContext ctx;
    // Per-backend seams wired by the driver (see the struct header).
    std::function<void()> close;
    std::function<std::size_t()> slot_in_use;
    std::vector<sluice::async::Completion<std::size_t>*> tracked;

    explicit CloseDrainFixture(std::unique_ptr<sluice::async::AsyncBackend> backend)
        : ctx(std::move(backend), &stats) {}

    sluice::async::ReadOp make_read_op(int fd, std::byte* dst, std::size_t len) const {
        return sluice::async::ReadOp{fd, dst, len, 0};
    }

    // Track a Completion into `tracked` exactly once (CapacityFixture parity:
    // a repeat registration must not push a second entry, or cleanup would
    // act on the same Completion twice).
    void track_once(sluice::async::Completion<std::size_t>& c) {
        if (std::find(tracked.begin(), tracked.end(), &c) == tracked.end()) {
            tracked.push_back(&c);
        }
    }

    // EVERY C2e-case submit MUST go through this helper (CapacityFixture
    // cleanup principle: register BEFORE submit_read so a broken backend that
    // binds a Completion before returning a rejection is still reachable by
    // cleanup).
    sluice::Result<void> submit_and_track(sluice::async::Completion<std::size_t>& c,
                                          sluice::async::ReadOp op) {
        track_once(c);
        return ctx.submit_read(op, c);
    }

    // Explicit cleanup (CapacityFixture parity): cancel everything still
    // outstanding, drive poll/reap with a real deadline, reset every ready
    // Completion so the slots are released, then re-check. Abort()s with a
    // precise diagnostic if the deadline passes with outstanding work — the
    // AsyncIoContext destructor must never be the one to fire.
    void cleanup_or_abort(const char* backend_name, const char* case_name);
};

// Uniform C2e-case wrapper: cleanup runs on BOTH success and exception paths
// (same catch-all rationale as run_capacity_case).
template <typename Body>
std::string run_close_drain_case(CloseDrainFixture& fx, const char* backend_name,
                                 const char* case_name, Body&& body) {
    try {
        body();
    } catch (const case_bail&) {
        fx.cleanup_or_abort(backend_name, case_name);
        return case_name;
    } catch (...) {
        fx.cleanup_or_abort(backend_name, case_name);
        throw;
    }
    fx.cleanup_or_abort(backend_name, case_name);
    return {};
}

// The shared close/drain suite: drives the C2e cases against a FRESH fixture
// per case (close_admission is irreversible, so a case can never reuse a
// backend whose admission is already closed — CapacityFixture parity: each
// case owns its fixture + Completions in the same frame). The DRIVER supplies
// a fixture factory that builds a fresh backend at a small capacity and wires
// the `close` / `slot_in_use` closures over the concrete backend's PUBLIC
// close_admission() and arena_slot_in_use() (AGENTS.md §15 instance-level
// seams; the suite itself stays backend-agnostic). Returns the empty string
// on full pass, or the stable name of the FIRST failing case. Implemented
// out-of-line in backend_conformance_test.cpp.
//
// The factory returns a unique_ptr<CloseDrainFixture> (NOT a value) so the
// fixture — and therefore its `AsyncStats stats` member, which the
// AsyncIoContext holds a pointer to — has a STABLE address. Returning a value
// would move the fixture, and AsyncIoContext's move ctor copies the stats_
// pointer from the dying source, leaving the context/backend with a dangling
// AsyncStats* (NRVO is a permitted, not guaranteed, optimization; correctness
// must not rely on it). Returning a unique_ptr pins the address for the
// fixture's whole lifetime, matching CapacityFixture's direct in-frame
// construction.
using MakeCloseDrainFixture = std::function<std::unique_ptr<CloseDrainFixture>()>;
std::string run_close_drain_cases(const BackendFactory& factory,
                                  const MakeCloseDrainFixture& make_fx);

}  // namespace sluice_test::conformance
