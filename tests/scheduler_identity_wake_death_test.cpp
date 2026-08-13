// Scheduler wait-registry quiescence death tests (Phase F1, issue #98).
//
// ADR Decision 10 + the F1 design (§6.4): a registered Scheduler waiter is a
// wake obligation — the fiber must be delivered (routed by the drain) or
// cancelled before the Scheduler is destroyed. ~Scheduler asserts the wait
// registry is empty and fail-fasts (scheduler_wait_registry_nonempty_fail_fast,
// Debug AND Release) when a record is abandoned:
//
//   1) destroy with a live registered waiter (fiber suspended, op never
//      completed, never cancelled) -> fail-fast (exit 86);
//   2) control: complete -> reap -> route -> fiber done -> destroy exits 0
//      (quiescent path).
//
// POSIX-only (fork/exec/waitpid via death_test_runner_posix.hpp).
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace sluice::async;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// 1) Destroy the Scheduler with a live registered waiter: the fiber suspended
// on an outstanding op that is never completed and never wait-cancelled. The
// record remains registered -> ~Scheduler fail-fasts (the abandoned wake
// obligation).
void child_destroy_with_live_waiter() {
    sluice_death_test::install_deterministic_terminate_handler();
    if constexpr (!fiber_ctx::supported) {
        std::_Exit(0);
    }

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        auto backend_up = std::make_unique<FakeAsyncBackend>();
        AsyncIoContext ctx(std::move(backend_up));
        Scheduler sched(ctx);

        Fiber fa;
        fa.set_entry([&](Fiber&) {
            if (!ctx.submit_read(ReadOp{-1, buf, 1, 0}, c).has_value()) {
                std::_Exit(sluice_death_test::kChildTestFailExit);
            }
            auto r = sched.await_completion_size(c);
            (void)r;  // must never resume: the op never completes
            std::_Exit(sluice_death_test::kUnexpectedReturnExit);
        });
        FiberStack stack;
        if (!sched.init_fiber(fa, stack.base(), stack.size())) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        sched.spawn(fa);
        sched.run(1);  // returns with the fiber suspended (no progress)

        // ~Scheduler here: a registered waiter was neither delivered nor
        // cancelled -> scheduler_wait_registry_nonempty_fail_fast.
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 2) Control: complete -> reap -> route -> fiber done -> registry drained ->
// destroy exits 0 (the quiescent path must NOT fail-fast).
void child_control_quiescent_destroy() {
    if constexpr (!fiber_ctx::supported) {
        std::_Exit(0);
    }

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        auto backend_up = std::make_unique<FakeAsyncBackend>();
        FakeAsyncBackend* backend = backend_up.get();
        AsyncIoContext ctx(std::move(backend_up));
        Scheduler sched(ctx);

        Fiber fa;
        fa.set_entry([&](Fiber&) {
            if (!ctx.submit_read(ReadOp{-1, buf, 1, 0}, c).has_value()) {
                std::_Exit(sluice_death_test::kChildTestFailExit);
            }
            auto r = sched.await_completion_size(c);
            if (!r.has_value()) {
                std::_Exit(sluice_death_test::kChildTestFailExit);
            }
        });
        FiberStack stack;
        if (!sched.init_fiber(fa, stack.base(), stack.size())) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        sched.spawn(fa);
        sched.run(1);
        backend->complete_oldest_with_bytes(1);
        sched.run(1);  // routes the delivery; the fiber finishes; registry 0
        c.reset();
        // ~Scheduler + ~AsyncIoContext: quiescent, must not fail-fast.
    }

    std::_Exit(0);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
void dispatch_child(const std::string& name) {
    sluice_death_test::install_deterministic_terminate_handler();
    // Debug: the registry-quiescence check is a plain assert() first, which
    // aborts (SIGABRT) rather than calling std::terminate; map it to the
    // expected terminate exit so both Debug and Release report exit 86.
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
    if      (name == "LW") child_destroy_with_live_waiter();
    else if (name == "CTL") child_control_quiescent_destroy();
    std::cerr << "[death] unknown child case: " << name << "\n";
    std::_Exit(sluice_death_test::kChildTestFailExit);
}

int run_parent() {
    int failures = 0;
    const auto must_term = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_terminated_via_fail_fast(r)) ++failures;
    };
    const auto must_zero = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_normal_exit_zero(r)) ++failures;
    };

    must_term("LW");   // live registered waiter at ~Scheduler -> fail-fast
    must_zero("CTL");  // control: drained registry destroys cleanly

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED\n";
        return 0;
    }
    std::cout << failures << " death-test case(s) FAILED\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        dispatch_child(child_case);  // never returns
        return sluice_death_test::kChildTestFailExit;  // unreachable
    }
    return run_parent();
}

#else  // !__unix__

int main() {
    return 0;
}

#endif  // __unix__
