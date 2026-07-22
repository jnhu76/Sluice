// e13_select_inline_death_test — E13 P5 no-ready stage guard (NI).
//
// After the registration transaction completes and the readiness snapshot is
// empty, the inline-only admission MUST fail fast: suspended completion is P6
// (denied at the P5 boundary). select_admission_no_ready_fail_fast terminates
// the process in the same global_mtx_ critical section, WITHOUT returning a
// no-winner result, unwinding live authority, fake-cancelling arms, performing
// a P7 rollback, or suspending the caller.
//
//   NI  running Fiber calls public select(), all arms unset / future, snapshot
//       empty -> stage-boundary fail-fast terminates
//   CTL control: a ready Event arm completes inline, exit 0
//
// Totals: 1 expected-termination case (NI) + 1 normal-exit control (CTL).
//
// Each case runs in a forked child that re-execs this binary; the child
// installs handlers so assert (SIGABRT) and std::terminate become a fixed exit
// code. POSIX-only. This is a temporary stage guard on a Draft PR, NOT final
// public API behavior.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace sa = sluice::async;
namespace stest = sluice_async_test;
using Scheduler = sa::Scheduler;
using Event = sa::Event;
using Fiber = sa::Fiber;
using EventSelectCase = sa::EventSelectCase;
using TimerSelectCase = sa::TimerSelectCase;

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

void install_death_handlers() noexcept {
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
    std::set_terminate([]() noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
}

// NI — no-ready admission reaches the stage-boundary fail-fast. A running Fiber
// calls the PUBLIC select() with all arms unset / future; the snapshot is empty;
// select_admission_no_ready_fail_fast terminates. The line after select() is
// reached ONLY if the guard failed to fire.
void child_ni_no_ready_terminates() {
    install_death_handlers();
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    stest::E11TimerControl::enable_test_clock(sched);
    // Clock at 0; Timer deadline in the FUTURE (not due). Event unset.
    Event ev(sched, /*initially_set=*/false);

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        // All arms NOT ready -> snapshot empty -> P5 stage-boundary fail-fast.
        sa::select(sched,
                   EventSelectCase{ev},
                   TimerSelectCase{sched, Scheduler::deadline_t{1000}});
    });
    FiberStack sw;
    (void)sched.init_fiber(fb, sw.base(), sw.size());
    sched.spawn(fb);
    sched.run(1);

    // Unreachable: the fail-fast terminated the process inside select().
    std::cerr << "[death] NI: select() returned without fail-fast (BUG)\n";
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CTL — control: a ready Event arm completes inline, exit 0. Proves the death
// harness + the public select() path itself work for the normal case (so the NI
// termination is attributable to the empty snapshot, not a harness fault).
void child_ctl_control() {
    install_death_handlers();
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    stest::E11TimerControl::enable_test_clock(sched);
    Event ev(sched, /*initially_set=*/true);

    sa::SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(sched, EventSelectCase{ev});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    if (!sched.init_fiber(fb, sw.base(), sw.size())) std::_Exit(2);
    sched.spawn(fb);
    sched.run(1);
    if (!ran_after.load() || !captured.has_winner()) std::_Exit(3);
    // Normal completion -> exit 0 (the harness expects exactly 0).
    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    if      (name == "NI")  child_ni_no_ready_terminates();
    else if (name == "CTL") child_ctl_control();
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

    // NI: empty snapshot -> P5 no-ready stage-boundary fail-fast.
    must_term("NI");
    // CTL: ready Event arm completes inline, exit 0.
    must_zero("CTL");

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (NI no-ready stage guard / "
                     "CTL control)\n";
        return 0;
    }
    std::cout << failures << " death-test case(s) FAILED\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        dispatch_child(child_case);
        return sluice_death_test::kChildTestFailExit;
    }
    return run_parent();
}

#else  // !defined(__unix__)

#include <iostream>

int main() {
    std::cout << "NOT RUN: e13_select_inline_death_test is POSIX-only (fork/"
                 "re-exec death-test runner).\n";
    return 0;
}

#endif  // defined(__unix__)
