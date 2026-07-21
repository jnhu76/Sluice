// e13_select_timer_pump_death_test — E13 Select timer pump ACTIVE-due
// stage-boundary fail-fast (P3).
//
// Verifies the P3 stage-boundary guard: a due ACTIVE SelectTimerRegistration is
// unreachable in valid P3 (there is no admission path). If the pump pops an
// ACTIVE Select entry, it MUST fail fast (select_timer_pump_active_fail_fast)
// rather than claim a winner, mark CandidateReady, retire/consume, erase, or
// busy-loop. This is an invariant GUARD, NOT supported production Select
// behavior — P4 (claim/finalization) is denied pending independent P3 review.
//
// Each case runs in a forked child that re-execs this binary; the child
// installs a deterministic terminate handler so std::terminate (invoked by
// select_timer_pump_active_fail_fast) becomes a fixed exit code. The parent
// asserts the exact exit code (see death_test_runner_posix.hpp).
//
// Cases:
//  PA  Pump ACTIVE-due — register ACTIVE Select, advance clock to its deadline.
//  CTL Control — register + retire before advancing; stale skip, exit 0.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using SelectTimerReg = sad::SelectTimerRegistration;
using Scheduler = sa::Scheduler;

void install_death_handlers() noexcept {
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
    std::set_terminate([]() noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
}

// PA — pump ACTIVE-due. Register an ACTIVE synthetic Select entry, then advance
// the clock to its deadline. The pump pops the ACTIVE entry and must fail fast
// (a due ACTIVE Select entry is unreachable in valid P3).
void child_pa_pump_active_due() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);
    sluice_async_test::E11TimerControl::enable_test_clock(sched);

    // Register an ACTIVE Select entry with a future deadline.
    sluice_async_test::E13SelectTimerSeam::register_synthetic(
        sched, nullptr, /*deadline=*/50);

    // Advance the clock to the deadline: the pump pops the ACTIVE entry and
    // invokes select_timer_pump_active_fail_fast() -> std::terminate().
    sluice_async_test::E13SelectTimerSeam::advance_clock(sched, 50);

    // If control reaches here the fail-fast did NOT fire — invariant broken.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CTL — control. Register + retire (via the Scheduler helper) while the
// deadline is still future, then advance. The pump observes the stale RETIRED
// entry and skips it; the process exits 0 normally.
void child_ctl_control() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);
    sluice_async_test::E11TimerControl::enable_test_clock(sched);

    SelectTimerReg* r = sluice_async_test::E13SelectTimerSeam::register_synthetic(
        sched, nullptr, /*deadline=*/50);
    bool ok = sluice_async_test::E13SelectTimerSeam::retire_synthetic(sched, *r);
    if (!ok) std::_Exit(sluice_death_test::kChildTestFailExit);

    sluice_async_test::E13SelectTimerSeam::advance_clock(sched, 50);
    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    if (name == "PA") child_pa_pump_active_due();
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

    must_term("PA");   // ACTIVE-due Select entry -> fail fast
    must_zero("CTL");  // stale skip -> normal exit

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (PA pump-active fail-fast / "
                     "CTL stale-skip control)\n";
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
    std::cout << "e13_select_timer_pump_death_test: NOT RUN on this platform "
                 "(POSIX fork/exec harness only; see death_test_runner_posix.hpp)\n";
    return 0;
}

#endif  // defined(__unix__)
