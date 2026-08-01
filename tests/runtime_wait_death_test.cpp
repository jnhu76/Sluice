// runtime_wait_death_test — M1-A RuntimeTaskContext::await_completion
// fail-fast boundary (idle-await is a caller contract violation).
//
// Verifies that calling await_completion on a default-constructed (idle)
// Completion triggers the Debug assertion documented in the M1-A design doc
// ("Debug asserts; Release documents"). The assertion is at the
// RuntimeTaskContext layer — the underlying Scheduler primitive has no idle
// check and would park the Fiber permanently.
//
// Each case runs in a forked child that re-execs this binary with
// --death-child=<case>; the child installs a deterministic terminate handler
// and the parent asserts the exact exit code. POSIX-only: gated to
// linux/macosx in xmake.lua.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

using namespace sluice::async;
using sluice::Result;

// ===========================================================================
// Idle-await contract violation (DEBUG-only; Release compiles out the assert)
// ===========================================================================
#if !defined(NDEBUG)

// Templated helper for idle-await death cases. Creates a Runtime, submits a
// task that call await_completion on a default-constructed (idle) Completion<T>,
// and verifies the assert fires. T is the Completion value type (std::size_t
// or void). Shared by I1 and I2.
template <class T>
void child_idle_await() {
    auto* raw = new FakeAsyncBackend();
    auto rt = RuntimeBuilder{}
        .backend(std::unique_ptr<AsyncBackend>(raw))
        .workers(1)
        .build()
        .value();
    auto sr = rt->start();
    if (!sr.has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }

    std::atomic<bool> barrier{false};
    auto sub_r = rt->submit([&](RuntimeTaskContext& ctx) {
        Completion<T> c;  // default-idle
        barrier.store(true, std::memory_order::release);
        ctx.await_completion(c);  // assert fires here; never returns
        std::_Exit(sluice_death_test::kUnexpectedReturnExit);
    });
    if (!sub_r.has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }

    // Spin until the task signals it is about to fire the assert.
    while (!barrier.load(std::memory_order::acquire)) {
        // spin
    }
    // The task is now a few instructions away from the assert. Give the driver
    // thread time to execute them. The assert kills the process before the
    // sleep expires; if it does not, the test fails below.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // If we reach here the assert did NOT fire — test failure.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// I1 — await_completion on a default-constructed (idle) Completion<std::size_t>.
void child_i1_idle_await_size() { child_idle_await<std::size_t>(); }

// I2 — await_completion on a default-constructed (idle) Completion<void>.
void child_i2_idle_await_void() { child_idle_await<void>(); }

#endif  // !defined(NDEBUG)

// ===========================================================================
// Control case — valid await completes, exit 0.
// ===========================================================================
void child_ctl_valid_await() {
    auto* raw = new FakeAsyncBackend();
    raw->auto_bytes(4);  // complete reads with 4 bytes
    auto rt = RuntimeBuilder{}
        .backend(std::unique_ptr<AsyncBackend>(raw))
        .workers(1)
        .build()
        .value();
    auto sr = rt->start();
    if (!sr.has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }

    std::atomic<bool> done{false};
    auto sub_r = rt->submit([&](RuntimeTaskContext& ctx) {
        std::byte buf[4]{};
        Completion<std::size_t> c;
        auto rsr = ctx.submit_read(ReadOp{-1, buf, 4, 0}, c);
        if (!rsr.has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        ctx.await_completion(c);  // valid: outstanding, completed by backend
        if (!c.ready()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        done.store(true, std::memory_order::release);
    });
    if (!sub_r.has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }

    // Wait for the task to complete (FakeAsyncBackend auto_bytes completes
    // the read immediately).
    while (!done.load(std::memory_order::acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::_Exit(0);
}

// ===========================================================================
// Dispatch
// ===========================================================================
void dispatch_child(const std::string& name) {
    sluice_death_test::install_deterministic_terminate_handler();
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
#if !defined(NDEBUG)
    if      (name == "I1") child_i1_idle_await_size();
    else if (name == "I2") child_i2_idle_await_void();
#endif  // !defined(NDEBUG)
    if      (name == "CTL") child_ctl_valid_await();
    std::cerr << "[death] unknown child case: " << name << "\n";
    std::_Exit(sluice_death_test::kChildTestFailExit);
}

int run_parent() {
    int failures = 0;
    // must_term is only invoked under !defined(NDEBUG); [[maybe_unused]] keeps
    // the Release (NDEBUG) build warning-clean (-Werror).
    [[maybe_unused]] const auto must_term = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_terminated_via_fail_fast(r)) ++failures;
    };
    const auto must_zero = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_normal_exit_zero(r)) ++failures;
    };

#if !defined(NDEBUG)
    must_term("I1");  // idle-await Completion<std::size_t>
    must_term("I2");  // idle-await Completion<void>
#endif  // !defined(NDEBUG)
    must_zero("CTL");  // control: valid await exits 0

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

#else  // !defined(__unix__)

#include <iostream>

int main() {
    std::cout << "runtime_wait_death_test: NOT RUN on this platform "
                 "(POSIX fork/exec harness only; see death_test_runner_posix.hpp)\n";
    return 0;
}

#endif  // defined(__unix__)