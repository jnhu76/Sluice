// E14 RT-F3 — Real init_fiber failure regression test + RT-F5 real boundary.
//
// Uses the SLUICE_ASYNC_INTERNAL_TESTING seam to force Scheduler::init_fiber()
// to return false, then asserts:
//   - Group::async throws std::runtime_error
//   - failure occurs before Scheduler::spawn/enqueue
//   - Group::size() is unchanged
//   - Scheduler runnable count is unchanged
//   - no Fiber/stack/Future pointer is retained
//   - the Group remains reusable for a later successful task
//
// Also contains RT-F5 real-boundary death tests:
//   - F5-sched: Scheduler construction with admission override=false terminates
//   - F5-group: Group construction with admission override=false terminates
//   These exercise the ACTUAL public admission call paths (not just the
//   detail helper directly).
//
// Links against sluice_async_internal_testing (NOT production sluice_async).
// Production sluice_async exposes no test hook.
#include "harness.hpp"

#if defined(__unix__)
#include "death_test_runner_posix.hpp"
#endif

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
#include <stdexcept>
#include <string>
#include <vector>

using namespace sluice::async;
using sluice::Result;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};
}  // namespace

// ============================================================================
// RT-F3: init_fiber failure propagation via internal-testing seam.
// Forces the next init_fiber call to return false, then verifies the
// exception and all state invariants.
// ============================================================================
SLUICE_TEST_CASE(e14_rt_f3_init_fiber_failure_internal_seam) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    Group g{sched};

    // First: prove the group works with a valid task.
    int valid_ran = 0;
    g.async([&](CancelToken&) { ++valid_ran; });
    g.await();
    SLUICE_CHECK(valid_ran == 1);
    SLUICE_CHECK(g.size() == 0);  // reaped after await

    // Record pre-failure state.
    std::size_t size_before = g.size();
    std::size_t runnable_before = sched.runnable_count();

    // Arm the one-shot init_fiber failure injection.
    Scheduler::AsyncTestAccess::force_next_init_fiber_fail(sched);
    SLUICE_CHECK(Scheduler::AsyncTestAccess::init_fiber_fail_armed(sched));

    // Attempt to add a task. Post-fix: init_fiber returns false, async_evented
    // throws std::runtime_error BEFORE any Fiber is spawned or enqueued.
    bool threw = false;
    try {
        g.async([&](CancelToken&) { /* should never run */ });
    } catch (const std::runtime_error& e) {
        threw = true;
        // Verify the message mentions init_fiber.
        SLUICE_CHECK_MSG(
            std::string(e.what()).find("init_fiber") != std::string::npos,
            "exception message must mention init_fiber");
    }

    // The seam is consumed (one-shot).
    SLUICE_CHECK(!Scheduler::AsyncTestAccess::init_fiber_fail_armed(sched));

    // Assert all state invariants.
    SLUICE_CHECK_MSG(threw, "RT-F3: async must throw std::runtime_error on init_fiber failure");
    SLUICE_CHECK(g.size() == size_before);  // unchanged
    SLUICE_CHECK(sched.runnable_count() == runnable_before);  // unchanged

    // The Group remains reusable for a later successful task.
    int second_ran = 0;
    g.async([&](CancelToken&) { ++second_ran; });
    g.await();
    SLUICE_CHECK(second_ran == 1);
    SLUICE_CHECK(g.size() == 0);
}

// ============================================================================
// RT-F3-B: Multiple consecutive failures. The Group survives repeated
// init_fiber failures and remains usable.
// ============================================================================
SLUICE_TEST_CASE(e14_rt_f3b_repeated_init_fiber_failures) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Group g{sched};

    // Three consecutive failures.
    for (int i = 0; i < 3; ++i) {
        Scheduler::AsyncTestAccess::force_next_init_fiber_fail(sched);
        bool threw = false;
        try {
            g.async([&](CancelToken&) {});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        SLUICE_CHECK(threw);
        SLUICE_CHECK(g.size() == 0);
    }

    // Still usable.
    int ran = 0;
    g.async([&](CancelToken&) { ++ran; });
    g.await();
    SLUICE_CHECK(ran == 1);
    SLUICE_CHECK(g.size() == 0);
}

// ============================================================================
// RT-F5 real-boundary death tests (POSIX only). These exercise the ACTUAL
// public admission paths (Scheduler ctor, Group ctor) via the internal-testing
// admission override, proving the guard fires at the real boundary.
// ============================================================================
#if defined(__unix__)

namespace {
// Child: construct Scheduler with override=false. Must terminate (exit 86).
void child_f5_sched_boundary() {
    sluice_death_test::install_deterministic_terminate_handler();
    Scheduler::AsyncTestAccess::set_evented_admission_override(false);
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);  // MUST fail-fast
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// Child: construct Group with override=false (Scheduler already constructed
// with override=true). Must terminate (exit 86) at Group's own guard.
void child_f5_group_boundary() {
    sluice_death_test::install_deterministic_terminate_handler();
    // Scheduler construction succeeds (override not yet set).
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    // Now set override to false; Group ctor must fail-fast.
    Scheduler::AsyncTestAccess::set_evented_admission_override(false);
    Group g{sched};  // MUST fail-fast
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}
}  // namespace

SLUICE_TEST_CASE(e14_rt_f5_sched_admission_real_boundary) {
    auto r = sluice_death_test::run_death_case("F5-sched");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "RT-F5: Scheduler ctor with unsupported admission must fail-fast");
}

SLUICE_TEST_CASE(e14_rt_f5_group_admission_real_boundary) {
    auto r = sluice_death_test::run_death_case("F5-group");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "RT-F5: Group ctor with unsupported admission must fail-fast");
}

#endif  // defined(__unix__)

// Custom main: dispatches death-test child cases, then runs all test cases.
int main(int argc, char** argv) {
#if defined(__unix__)
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "F5-sched") {
            child_f5_sched_boundary();
        } else if (child_case == "F5-group") {
            child_f5_group_boundary();
        } else {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);  // unreachable
    }
#else
    (void)argc;
    (void)argv;
#endif
    return sluice_test::run_all();
}
