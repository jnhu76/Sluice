// e13_select_event_registry_death_test — E13 Select registry death tests (P2).
//
// Verifies the E13 registry contract violations must terminate via assert() or
// ~Event destructor assertion. Each case runs in a forked child that re-execs
// this binary. The child installs handlers for both SIGABRT (assert) and
// std::terminate and converts them to deterministic exit(86). The parent
// asserts the exact exit code (see death_test_runner_posix.hpp).
//
// Cases:
//  DL  Duplicate link — link same arm twice to same Event.
//  WE  Wrong Event unlink — link to Event A, unlink through Event B.
//  XS1 Cross-Scheduler link — link arm to Event bound to Sched B via Sched A.
//  XS2 Cross-Scheduler unlink — link through correct Sched, unlink through wrong Sched.
//  DC  Live-arm Event destruction — destroy Event with registered Select arm.
//  CTL Control — normal link + unlink, exit 0.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;

// Install handlers so that assert failures (SIGABRT) and std::terminate both
// call _Exit(kExpectedTerminateExit=86), matching the parent's expectation.
void install_death_handlers() noexcept {
    std::signal(SIGABRT, [](int) noexcept { std::_Exit(sluice_death_test::kExpectedTerminateExit); });
    std::set_terminate([]() noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
}

// --------------------------------------------------------------------------
// Child-mode bodies
// --------------------------------------------------------------------------

// DL — duplicate link. Link arm to Event, then link same arm again.
void child_dl_duplicate_link() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);

    sad::SelectArmSlot arm;
    arm.construct_event(ev);
    arm.state = sad::ArmState::prepared;
    sad::SelectGroup group;
    arm.group = &group;

    AsyncTestAccess::select_event_link(sched, ev, arm);
    AsyncTestAccess::select_event_link(sched, ev, arm);  // MUST assert

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// WE — wrong Event unlink. Link to Event A, unlink through Event B.
void child_we_wrong_event_unlink() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev_a(sched);
    sa::Event ev_b(sched);

    sad::SelectArmSlot arm;
    arm.construct_event(ev_a);
    arm.state = sad::ArmState::prepared;
    sad::SelectGroup group;
    arm.group = &group;

    AsyncTestAccess::select_event_link(sched, ev_a, arm);
    AsyncTestAccess::select_event_unlink(sched, ev_b, arm);  // MUST assert

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// XS1 — Cross-Scheduler link. Event bound to Sched B, link via Sched A.
void child_xs1_cross_scheduler_link() {
    install_death_handlers();
    sa::AsyncIoContext ctx_a(std::make_unique<sa::FakeAsyncBackend>());
    sa::AsyncIoContext ctx_b(std::make_unique<sa::FakeAsyncBackend>());
    sa::Scheduler sched_a(ctx_a);
    sa::Scheduler sched_b(ctx_b);
    sa::Event ev_b(sched_b);

    sad::SelectArmSlot arm;
    arm.construct_event(ev_b);
    arm.state = sad::ArmState::prepared;
    sad::SelectGroup group;
    arm.group = &group;

    AsyncTestAccess::select_event_link(sched_a, ev_b, arm);  // MUST assert

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// XS2 — Cross-Scheduler unlink. Link through correct Sched B, unlink through
// wrong Sched A.
void child_xs2_cross_scheduler_unlink() {
    install_death_handlers();
    sa::AsyncIoContext ctx_a(std::make_unique<sa::FakeAsyncBackend>());
    sa::AsyncIoContext ctx_b(std::make_unique<sa::FakeAsyncBackend>());
    sa::Scheduler sched_a(ctx_a);
    sa::Scheduler sched_b(ctx_b);
    sa::Event ev_b(sched_b);

    sad::SelectArmSlot arm;
    arm.construct_event(ev_b);
    arm.state = sad::ArmState::prepared;
    sad::SelectGroup group;
    arm.group = &group;

    AsyncTestAccess::select_event_link(sched_b, ev_b, arm);
    AsyncTestAccess::select_event_unlink(sched_a, ev_b, arm);  // MUST assert

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// DC — live-arm Event destruction. Destroy Event with linked Select arm.
void child_dc_destruction_contract() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);

    sad::SelectArmSlot arm;
    sad::SelectGroup group;
    group.set_phase(sad::GroupPhase::armed);
    {
        sa::Event ev(sched);
        arm.construct_event(ev);
        arm.state = sad::ArmState::prepared;
        arm.group = &group;

        AsyncTestAccess::select_event_link(sched, ev, arm);
        // ~Event fires while arm is still linked — MUST assert
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CTL — Control. Normal link + unlink, exit 0.
void child_ctl_control() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);

    sad::SelectArmSlot arm;
    arm.construct_event(ev);
    arm.state = sad::ArmState::prepared;
    sad::SelectGroup group;
    arm.group = &group;

    AsyncTestAccess::select_event_link(sched, ev, arm);
    AsyncTestAccess::select_event_unlink(sched, ev, arm);

    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    if      (name == "DL") child_dl_duplicate_link();
    else if (name == "WE") child_we_wrong_event_unlink();
    else if (name == "XS1") child_xs1_cross_scheduler_link();
    else if (name == "XS2") child_xs2_cross_scheduler_unlink();
    else if (name == "DC") child_dc_destruction_contract();
    else if (name == "CTL") child_ctl_control();
    std::cerr << "[death] unknown child case: " << name << "\n";
    std::_Exit(sluice_death_test::kChildTestFailExit);
}

// --------------------------------------------------------------------------
// Parent mode
// --------------------------------------------------------------------------

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

    must_term("DL");
    must_term("WE");
    must_term("XS1");
    must_term("XS2");
    must_term("DC");
    must_zero("CTL");

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (DL duplicate-link / WE wrong-Event "
                     "unlink / XS1 cross-Sched link / XS2 cross-Sched unlink / "
                     "DC destruction / CTL control)\n";
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
    std::cout << "e13_select_event_registry_death_test: NOT RUN on this platform "
                 "(POSIX fork/exec harness only; see death_test_runner_posix.hpp)\n";
    return 0;
}

#endif  // defined(__unix__)
