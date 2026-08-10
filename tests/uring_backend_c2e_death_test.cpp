// Phase D4 — Uring C2e non-quiescent destruction death matrix (Issue #68 row
// 16; ADR Decision 15; AGENTS.md §14).
//
// POSIX-only (fork/exec/waitpid). Each child case re-execs this binary via
// death_test_runner_posix.hpp, installs a deterministic terminate handler, and
// either terminates with exit 86 (the backend destructor preflight fail-fast
// fired) or exits 87 (the destructor returned without terminating — the
// regression). The quiescent control case exits 0.
//
// Linkage: this target compiles the authoritative production uring_backend.cpp
// + fail_fast.cpp under SLUICE_ASYNC_INTERNAL_TESTING (the deterministic pause
// gates / CQE injection are needed to reach the pending / enqueued /
// backend-ready / live-control windows). The fail-fast under test is the exact
// production destructor preflight (`uring_non_quiescent_destruction_fail_fast`,
// active in BOTH Debug and Release) that runs before io_uring_queue_exit().
//
// Matrix (each state must fail-fast BEFORE ring teardown):
//   pending            — committed, between the accept LP and enqueue;
//   enqueued           — on the dispatch ring, no SQE installed;
//   running            — SQE installed, kernel-blocked (live router cookie);
//   ledger-residue     — SQE installed but NOT yet flushed (transport ledger
//                        entry live);
//   backend-ready      — terminal recorded, not yet reaped;
//   completion-ready   — reaped but the caller never reset the Completion;
//   live-control       — running + a prepared AsyncCancel (control reference);
//   quiescent control  — close_admission + drain + reset -> exit 0.
//
// Evidence discipline (P0-C): the evidence-mode case is registered in BOTH
// real and stub builds (stub emits mode=stub — build/API honesty only), and
// the stub build registers the SAME eight semantic case names as empty
// bodies, so the manifest's exact pinned case-set holds in every mode. The
// death matrix is a MANDATORY real-mode evidence record
// (uring_c2e_quiescent_destruction) consumed by the aggregate verdict — a
// missing/failing death target fails the real KernelIo gate.
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

// The Uring public header compiles in BOTH modes (real ring vs stub); the
// evidence-mode case at the bottom is registered in both builds (G2).
#include <sluice/async/uring_backend.hpp>

#if defined(__unix__) && defined(SLUICE_HAS_LIBURING) && \
    defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr int kWaitTimeoutMs = 5000;

bool wait_paused(const std::atomic<bool>& flag, int timeout_ms = kWaitTimeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

class TempFile {
  public:
    TempFile() {
        char path[] = "/tmp/sluice_uring_d4_death_XXXXXX";
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

class PipePair {
  public:
    PipePair() { valid_ = ::pipe(fds_) == 0; }
    ~PipePair() {
        if (fds_[0] >= 0)
            (void)::close(fds_[0]);
        if (fds_[1] >= 0)
            (void)::close(fds_[1]);
    }
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    bool valid() const noexcept { return valid_; }
    int read_fd() const noexcept { return fds_[0]; }
    int write_fd() const noexcept { return fds_[1]; }

  private:
    int fds_[2] = {-1, -1};
    bool valid_ = false;
};

} // namespace

// 1) Destroy with a genuinely `pending` request (committed, before enqueue).
//
// Genuine-state proof (review correction): the backend is destroyed WHILE the
// submitter is still paused at the AfterCommitBeforeEnqueuePauseGate, so the
// request is deterministically in the `pending` state (committed, no SQE, no
// dispatch entry). The submitter thread is intentionally `new`-allocated and
// NEVER joined or destroyed in this death child — a joinable automatic
// std::thread whose destructor runs std::terminate would mask the backend-
// preflight authority. The leaked pointer is acceptable: the process exits
// via _Exit(86) from the preflight terminate handler (or 87 if the destructor
// unexpectedly returned). The Completion c outlives the backend (declared
// before it) so the backend's preflight sees a still-bound slot.
void child_destroy_with_pending() {
    sluice_death_test::install_deterministic_terminate_handler();

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{1, 1});
    if (!backend->available()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    UringAsyncBackend::AfterCommitBeforeEnqueuePauseGate gate;
    backend->set_after_commit_before_enqueue_pause_gate(&gate);
    TempFile file;
    if (!file.valid()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    std::byte buf[1]{};
    Completion<std::size_t> c; // outlives backend (declared before reset)

    // Intentionally leaked: never joined, never destroyed in this child. The
    // thread spins on the gate, holding the submit inside its admission
    // transaction with the request at `pending`.
    auto* submitter = new std::thread([&] {
        (void)backend->submit_write(WriteOp{file.fd(), buf, 1, 0}, c);
    });
    if (!wait_paused(gate.paused)) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Deterministic state assertion: accepted, pending, no SQE, no dispatch.
    auto h = backend->handle_for_completion_for_test(&c);
    if (!h.has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    auto obs = backend->observe_for_test(*h);
    if (!obs.has_value() || obs->state != detail::RequestState::pending) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Destroy the backend NOW, while the request is genuinely `pending`. The
    // destructor preflight sees slot_in_use != 0 / accepted_outstanding != 0
    // -> fail-fast (exit 86). The submitter thread is leaked (never joined).
    (void)submitter;
    backend.reset();
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 2) Destroy with a genuinely `enqueued` request (dispatch ring, no SQE
// installed). Same genuine-state discipline as the pending case: the backend
// is destroyed while the submitter is paused at BeforeDispatchTransferPauseGate
// (request on the dispatch ring, no SQE). Leaked thread, never joined.
void child_destroy_with_enqueued() {
    sluice_death_test::install_deterministic_terminate_handler();

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{1, 1});
    if (!backend->available()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    UringAsyncBackend::BeforeDispatchTransferPauseGate gate;
    backend->set_before_dispatch_transfer_pause_gate(&gate);
    TempFile file;
    if (!file.valid()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    std::byte buf[1]{};
    Completion<std::size_t> c;

    auto* submitter = new std::thread([&] {
        (void)backend->submit_write(WriteOp{file.fd(), buf, 1, 0}, c);
    });
    if (!wait_paused(gate.paused)) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    auto h = backend->handle_for_completion_for_test(&c);
    if (!h.has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    auto obs = backend->observe_for_test(*h);
    if (!obs.has_value() || obs->state != detail::RequestState::enqueued) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    if (backend->dispatch_size_for_test() != 1) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Destroy while genuinely `enqueued` (dispatch ring, no SQE). Preflight
    // sees the live dispatch entry + slot_in_use -> fail-fast. Leaked thread.
    (void)submitter;
    backend.reset();
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 3) Destroy with a `running` (ring-owned) request — live router cookie.
void child_destroy_with_running() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        UringAsyncBackend backend{UringConfig{1, 1}};
        if (!backend.available()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        PipePair pipe;
        if (!pipe.valid()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!backend.submit_read(ReadOp{pipe.read_fd(), buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        (void)backend.poll(); // flush: the kernel blocks the read
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        auto obs = backend.observe_for_test(*h);
        if (!obs.has_value() || obs->state != detail::RequestState::running) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.live_cookies_for_test() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here while ring-owned -> fail-fast.
    }
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 4) Destroy with a transport-ledger residue (SQE installed, NOT flushed).
void child_destroy_with_ledger_residue() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        UringAsyncBackend backend{UringConfig{1, 1}};
        if (!backend.available()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        TempFile file;
        if (!file.valid()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!backend.submit_write(WriteOp{file.fd(), buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // NO poll(): the SQE sits in the SQ; the transport ledger holds the
        // entry (never flushed).
        if (backend.transport_ledger_size_for_test() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here with a live ledger entry -> fail-fast.
    }
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 5) Destroy with a `backend-ready` unreaped terminal.
void child_destroy_with_backend_ready() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        UringAsyncBackend backend{UringConfig{1, 1}};
        if (!backend.available()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        TempFile file;
        if (!file.valid()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!backend.submit_write(WriteOp{file.fd(), buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        backend.inject_cqe_for_test(1, 1); // record_terminal, NOT reaped
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        auto obs = backend.observe_for_test(*h);
        if (!obs.has_value() || obs->state != detail::RequestState::backend_ready) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here while backend_ready unreaped -> fail-fast.
    }
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 6) Destroy with a completion-ready but UNRESET Completion (drained !=
// releasable).
void child_destroy_with_completion_ready() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        UringAsyncBackend backend{UringConfig{1, 1}};
        if (!backend.available()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        TempFile file;
        if (!file.valid()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!backend.submit_write(WriteOp{file.fd(), buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        backend.inject_cqe_for_test(1, 1);
        if (backend.poll() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!c.ready()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.arena_slot_in_use() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here while slot_in_use != 0 -> fail-fast.
    }
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 7) Destroy with a live control reference (running + prepared AsyncCancel).
void child_destroy_with_live_control() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        UringAsyncBackend backend{UringConfig{1, 1}};
        if (!backend.available()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        PipePair pipe;
        if (!pipe.valid()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!backend.submit_read(ReadOp{pipe.read_fd(), buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        (void)backend.poll(); // kernel blocks the read
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.cancel_handle_for_test(*h) !=
            detail::CancelDisposition::intent_recorded) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.live_control_entries_for_test() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here with a live control reference -> fail-fast.
    }
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 8) Control: quiescent destroy — close_admission + drain + reset -> exit 0.
void child_control_quiescent_destroy() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        UringAsyncBackend backend{UringConfig{1, 1}};
        if (!backend.available()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        TempFile file;
        if (!file.valid()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!backend.submit_write(WriteOp{file.fd(), buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // Explicit lifecycle: close admission -> continue progress -> reap ->
        // callers reset ready Completions -> destroy (ADR Decision 15).
        backend.close_admission();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMs);
        while (backend.outstanding() != 0) {
            (void)backend.poll();
            if (std::chrono::steady_clock::now() > deadline) {
                std::_Exit(sluice_death_test::kChildTestFailExit);
            }
            std::this_thread::yield();
        }
        if (!c.ready()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        c.reset();
        if (backend.arena_slot_in_use() != 0) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here quiescently -> normal exit 0.
    }
    std::_Exit(0);
}

// 9) D4-RM11 detector child: non-quiescent destroy with a BeforeQueueExit hook
// that _Exit(90). Under the fix the preflight terminates first (exit 86) and
// the hook is NEVER reached; under a mutant that removes/bypasses the
// preflight the hook IS reached (exit 90). Uses a simple non-quiescent state
// (a submitted, never-reset write — slot_in_use != 0 at destroy).
void queue_exit_hook_signal(void* /*ctx*/) {
    std::_Exit(sluice_death_test::kQueueExitHookExit);
}

void child_preflight_order() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;
    {
        UringAsyncBackend backend{UringConfig{1, 1}};
        if (!backend.available()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // Install the teardown-boundary probe: reached ONLY if the preflight
        // above did NOT fire. _Exit(90) distinguishes this from the preflight
        // fail-fast (86) and the unexpected-return (87) paths.
        backend.set_before_queue_exit_hook_for_test(queue_exit_hook_signal, nullptr);
        TempFile file;
        if (!file.valid()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!backend.submit_write(WriteOp{file.fd(), buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here with a bound slot -> preflight MUST fire
        // FIRST (exit 86). The hook at the teardown boundary must NOT run.
    }
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

SLUICE_TEST_CASE(uring_c2e_death_destroy_with_pending) {
    auto r = sluice_death_test::run_death_case("pending");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with a pending request must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(uring_c2e_death_destroy_with_enqueued) {
    auto r = sluice_death_test::run_death_case("enqueued");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with an enqueued request must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(uring_c2e_death_destroy_with_running) {
    auto r = sluice_death_test::run_death_case("running");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with a ring-owned request must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(uring_c2e_death_destroy_with_ledger_residue) {
    auto r = sluice_death_test::run_death_case("ledger");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with transport-ledger residue must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(uring_c2e_death_destroy_with_backend_ready) {
    auto r = sluice_death_test::run_death_case("backend-ready");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with backend-ready unreaped must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(uring_c2e_death_destroy_with_completion_ready) {
    auto r = sluice_death_test::run_death_case("completion-ready");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with completion-ready unreset must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(uring_c2e_death_destroy_with_live_control) {
    auto r = sluice_death_test::run_death_case("live-control");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with a live control reference must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(uring_c2e_death_control_quiescent_destroy) {
    auto r = sluice_death_test::run_death_case("control");
    SLUICE_CHECK_MSG(sluice_death_test::expect_normal_exit_zero(r),
                     "quiescent destroy after close + drain + reset must exit 0");
}

// 9) D4-RM11 detector: prove the destructor preflight fires BEFORE
// io_uring_queue_exit. The child installs a BeforeQueueExit hook that
// _Exit(90), then destroys a non-quiescent backend. Under the fix the
// preflight terminates first (exit 86) and the hook is NEVER reached; under a
// mutant that removes/bypasses the preflight, the hook IS reached (exit 90).
// This case asserts the CORRECT behavior (exit 86 = preflight won); the
// hook-reached path (90) is the mutant-only detector signal, verified by the
// mutation ledger (not a pinned case). Without this case the death suite
// could only prove "process terminated" — not that the preflight authority
// specifically won before teardown.
SLUICE_TEST_CASE(uring_c2e_death_preflight_before_queue_exit_order) {
    auto r = sluice_death_test::run_death_case("preflight-order");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destructor preflight must fire BEFORE io_uring_queue_exit "
                     "(exit 86; the BeforeQueueExit hook must NOT be reached)");
}

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "pending") {
            child_destroy_with_pending();
        } else if (child_case == "enqueued") {
            child_destroy_with_enqueued();
        } else if (child_case == "running") {
            child_destroy_with_running();
        } else if (child_case == "ledger") {
            child_destroy_with_ledger_residue();
        } else if (child_case == "backend-ready") {
            child_destroy_with_backend_ready();
        } else if (child_case == "completion-ready") {
            child_destroy_with_completion_ready();
        } else if (child_case == "live-control") {
            child_destroy_with_live_control();
        } else if (child_case == "control") {
            child_control_quiescent_destroy();
        } else if (child_case == "preflight-order") {
            child_preflight_order();
        } else {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else  // !defined(__unix__) || !SLUICE_HAS_LIBURING || !internal-testing

// Stub / non-POSIX build: the SAME pinned semantic case names register as
// empty build/API-only bodies so the manifest's exact case-set holds in EVERY
// mode (G2). The death matrix itself requires the real liburing
// internal-testing build; there is deliberately NO unrelated skip case
// substituting for the pinned corpus (P0-C).
SLUICE_TEST_CASE(uring_c2e_death_destroy_with_pending) {}
SLUICE_TEST_CASE(uring_c2e_death_destroy_with_enqueued) {}
SLUICE_TEST_CASE(uring_c2e_death_destroy_with_running) {}
SLUICE_TEST_CASE(uring_c2e_death_destroy_with_ledger_residue) {}
SLUICE_TEST_CASE(uring_c2e_death_destroy_with_backend_ready) {}
SLUICE_TEST_CASE(uring_c2e_death_destroy_with_completion_ready) {}
SLUICE_TEST_CASE(uring_c2e_death_destroy_with_live_control) {}
SLUICE_TEST_CASE(uring_c2e_death_control_quiescent_destroy) {}
SLUICE_TEST_CASE(uring_c2e_death_preflight_before_queue_exit_order) {}

SLUICE_MAIN()

#endif

// ---------------------------------------------------------------------------
// Evidence-meta (G2): exactly one [evidence-meta] line per gate-driven run.
// Registered in BOTH real and stub builds (the internal #if/#else picks the
// emitted mode), so the manifest's pinned case-set holds in every mode; a
// stub run emits mode=stub (build/API honesty only) and is classified
// INCOMPLETE by required_modes=("real",), never PASS.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_d4_c2e_death_evidence_mode) {
#if defined(SLUICE_HAS_LIBURING)
    sluice::async::UringAsyncBackend backend{sluice::async::UringConfig{4, 4}};
    std::printf("[evidence-meta] evidence=uring_c2e_quiescent_destruction "
                "mode=real\n");
    SLUICE_CHECK(backend.available());
#else
    std::printf("[evidence-meta] evidence=uring_c2e_quiescent_destruction "
                "mode=stub\n");
#endif
}