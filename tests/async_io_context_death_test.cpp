// E15-P1-03 / E15-P2-06 — AsyncIoContext lifecycle fail-fast death tests.
//
// Pre-fix causal path (E15-P1-03):
//   ctx_a owns backend with N outstanding Completions
//   ctx_a = std::move(ctx_b)
//   ctx_a's old backend is silently destroyed
//   caller-owned Completions remain permanently outstanding (no Result channel,
//   no path to ready). The L11 assert that ~AsyncIoContext fires is BYPASSED
//   because operator= does not check before overwriting. In Release nothing
//   fires at all — the abandonment is silent.
//
// Pre-fix causal path (E15-P2-06):
//   documented contract claimed Release destruction of a context with
//   outstanding work returns invalid_state, but a destructor has no Result
//   channel. The actual Release behavior was silent abandonment (no assert).
//
// Post-fix causal path (BOTH):
//   AsyncIoContext::~AsyncIoContext() and operator=(AsyncIoContext&&) detect a
//   backend with >0 outstanding Completions and call
//   detail::async_context_outstanding_fail_fast (std::terminate, exit 86) in
//   BOTH Debug and Release. The test distinguishes the intentional fail-fast
//   (clean exit 86) from silent abandonment (child reaches
//   kUnexpectedReturnExit=87 because it observed the Completion never become
//   ready).
//
// POSIX only (fork/exec/waitpid). Gated to linux/macOS.
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdlib>
#include <utility>

using namespace sluice::async;

// ---- Child case E15-P1-03: move-assign over a destination with outstanding --
// work MUST fail-fast. The child arms a deterministic terminate handler; if the
// fail-fast does NOT fire (pre-fix), it observes the Completion never resolves
// and exits kUnexpectedReturnExit so the parent detects the regression.
void child_p1_03_move_assign_over_outstanding() {
    sluice_death_test::install_deterministic_terminate_handler();

    AsyncIoContext dst(std::make_unique<FakeAsyncBackend>());
    AsyncIoContext src(std::make_unique<FakeAsyncBackend>());

    std::byte b[8]{};
    Completion<std::size_t> c;
    if (!dst.submit_read(ReadOp{0, b, 8, 0}, c).has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    if (dst.outstanding() != 1) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Move-assign over dst while it has an outstanding op. Post-fix: fail-fast.
    dst = std::move(src);
    // If we reach here, the fail-fast did NOT fire (the bug). The caller's
    // Completion is now permanently abandoned — flag this to the parent.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child case E15-P2-06: destroy a context with outstanding work MUST -----
// fail-fast in BOTH Debug and Release (the documented contract that a
// destructor "returns invalid_state" is impossible — there is no Result
// channel). Silent abandonment in Release is the bug.
void child_p2_06_destroy_with_outstanding() {
    sluice_death_test::install_deterministic_terminate_handler();

    {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        std::byte b[8]{};
        Completion<std::size_t> c;
        if (!ctx.submit_read(ReadOp{0, b, 8, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (ctx.outstanding() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // Destroy ctx WITHOUT draining. Post-fix: fail-fast (L11 enforcement).
    }
    // If we reach here, the destructor did NOT fail-fast — the bug.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child case: control — valid move + clean destroy MUST exit 0 -----------
void child_control_valid_lifecycle() {
    AsyncIoContext src(std::make_unique<FakeAsyncBackend>());
    AsyncIoContext dst(std::move(src));
    std::byte b[8]{};
    Completion<std::size_t> c;
    if (!dst.submit_read(ReadOp{0, b, 8, 0}, c).has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    dst.cancel(c);
    if (dst.poll() != 1) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    if (!c.ready()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Both contexts destroy cleanly (src has no backend; dst has 0 outstanding).
    std::_Exit(0);
}

// ---- Child case: move-assigning an IDLE destination from a source with -------
// outstanding work is the SAFE-transfer path — must NOT fail-fast. The child
// observes the caller's Completion resolving via the new owner.
void child_p1_03_safe_transfer_not_fail_fast() {
    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend_up.get();
    AsyncIoContext src(std::move(backend_up));
    AsyncIoContext dst(std::make_unique<FakeAsyncBackend>());

    std::byte b[8]{};
    Completion<std::size_t> c;
    if (!src.submit_read(ReadOp{0, b, 8, 0}, c).has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // dst is IDLE — the move is permitted; src's outstanding work transfers.
    dst = std::move(src);
    if (dst.outstanding() != 1) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    if (!c.outstanding()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    raw->complete_oldest_with_bytes(8);
    if (dst.poll() != 1) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    if (!c.ready() || !c.result().has_value() || c.result().value() != 8) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    std::_Exit(0);
}

// ---- Parent-side test cases -------------------------------------------------

SLUICE_TEST_CASE(context_death_move_assign_over_outstanding) {
    auto r = sluice_death_test::run_death_case("P1-03");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "E15-P1-03: move-assign over a context with outstanding work must "
        "fail-fast (exit 86), not silently abandon the Completions");
}

SLUICE_TEST_CASE(context_death_destroy_with_outstanding) {
    auto r = sluice_death_test::run_death_case("P2-06");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "E15-P2-06: destroying a context with outstanding work must "
        "fail-fast (exit 86) in BOTH Debug and Release");
}

SLUICE_TEST_CASE(context_death_control_valid_lifecycle) {
    auto r = sluice_death_test::run_death_case("control");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_normal_exit_zero(r),
        "Control: valid AsyncIoContext move + clean destroy must exit 0");
}

SLUICE_TEST_CASE(context_death_safe_transfer_no_fail_fast) {
    auto r = sluice_death_test::run_death_case("safe-transfer");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_normal_exit_zero(r),
        "E15-P1-03 safe-transfer: move-assigning an IDLE destination from a "
        "source with outstanding work must NOT fail-fast (exit 0)");
}

// Child dispatch entry point.
int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "P1-03") {
            child_p1_03_move_assign_over_outstanding();
        } else if (child_case == "P2-06") {
            child_p2_06_destroy_with_outstanding();
        } else if (child_case == "control") {
            child_control_valid_lifecycle();
        } else if (child_case == "safe-transfer") {
            child_p1_03_safe_transfer_not_fail_fast();
        } else {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else  // !defined(__unix__)

SLUICE_TEST_CASE(context_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}
SLUICE_MAIN()

#endif  // defined(__unix__)
