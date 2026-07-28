// E14 RT-F2a — Evented Group destructor fail-fast death test.
//
// Pre-fix causal path:
//   ~Group -> pending Evented Future::await -> EventedWaitPolicy ->
//   Scheduler::await_ready_flag -> null g_worker / ws->current dereference
//   (segfault or UB, NOT a clean terminate).
//
// Post-fix causal path:
//   ~Group -> intentional D-E14-F2a fail-fast helper
//   (group_lifetime_fail_fast -> std::terminate, exit code 86).
//
// The test distinguishes the intentional invariant failure (clean exit 86)
// from the old accidental null dereference (signal or non-86 exit).
//
// Also contains:
//   RT-F5b: unsupported-target admission death test (deterministic seam
//   passes false to the guard; verifies fail-fast fires).
//
// POSIX only (fork/exec/waitpid). Gated to x86_64 + linux/macOS.
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace sluice::async;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// ---- Child case: RT-F2a destructor with pending Evented task ----
// Creates a Group(Scheduler&), spawns a task that suspends on an unready
// Future, then destroys the Group WITHOUT awaiting. Post-fix: the
// destructor detects the pending Evented Future and calls
// group_lifetime_fail_fast (std::terminate, exit 86).
void child_f2a_destructor_pending() {
    sluice_death_test::install_deterministic_terminate_handler();

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    EventedWaitPolicy policy(sched);
    Future<int> inner{policy};  // never completed

    {
        Group g{sched};
        g.async([&](CancelToken&) {
            (void)inner.await();  // would suspend the Fiber
        });
        // Drive once so the task Fiber starts and suspends.
        sched.run_until_idle();
        // Destroy g WITHOUT calling g.await(). The task Future is pending.
        // Post-fix: fail-fast. Pre-fix: null deref in await_ready_flag.
    }
    // If we reach here, the destructor did NOT fail-fast.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child case: RT-F5b unsupported-target admission ----
// Calls the require_evented_supported seam with false. Post-fix: the
// guard fires fail-fast (std::terminate, exit 86).
// Pre-fix: no guard exists; this child would not compile or would not
// exercise the path.
void child_f5b_unsupported_admission() {
    sluice_death_test::install_deterministic_terminate_handler();

    // The detail helper is tested with false to simulate unsupported target.
    // Post-fix: this calls std::terminate.
    sluice::async::detail::require_evented_supported(false);
    // If we reach here, the guard did NOT fire.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child case: control (normal exit 0) ----
void child_control() {
    // A normal Evented Group lifecycle: spawn, await, destroy. Must exit 0.
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    {
        Group g{sched};
        int ran = 0;
        g.async([&](CancelToken&) { ++ran; });
        g.await();
        if (ran != 1) std::_Exit(sluice_death_test::kChildTestFailExit);
        if (g.size() != 0) std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    std::_Exit(0);
}

}  // namespace

// Parent-side test dispatch.
SLUICE_TEST_CASE(te_death_f2a_destructor_pending_evented) {
    if constexpr (!fiber_ctx::supported) return;

    auto r = sluice_death_test::run_death_case("F2a");
    // Post-fix: expect clean terminate (exit 86).
    // Pre-fix: the child crashes with a signal (SIGSEGV) or exits with a
    // non-86 code (the null deref is UB, not a clean terminate).
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "RT-F2a: ~Group with pending Evented task must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(te_death_f5b_unsupported_admission) {
    // This test does not require fiber_ctx::supported because it tests the
    // guard itself (passing false deterministically).
    auto r = sluice_death_test::run_death_case("F5b");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "RT-F5b: require_evented_supported(false) must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(te_death_control_normal_lifecycle) {
    if constexpr (!fiber_ctx::supported) return;

    auto r = sluice_death_test::run_death_case("control");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_normal_exit_zero(r),
        "Control: normal Evented Group lifecycle must exit 0");
}

// Child dispatch entry point.
int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "F2a") {
            child_f2a_destructor_pending();
        } else if (child_case == "F5b") {
            child_f5b_unsupported_admission();
        } else if (child_case == "control") {
            child_control();
        } else {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // Unreachable (child functions _Exit or terminate).
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Parent: run all registered test cases.
    return sluice_test::run_all();
}

#else  // !defined(__unix__)

SLUICE_TEST_CASE(te_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}
SLUICE_MAIN()

#endif  // defined(__unix__)
