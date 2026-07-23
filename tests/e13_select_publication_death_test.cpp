// e13_select_publication_death_test — E13 P6 publication invariant death
// tests (production-test-plan.md §3 SN-2, SN-10 + FP suspended-caller-state +
// MG multi-group-P8-stage-boundary + CTL valid-publication control).
//
// Verifies the publication-entry preflight + the multi-group P8 stage gate
// fire fast (std::terminate) BEFORE any result rewrite / count decrement /
// make_runnable / route / group winner CAS. Each case runs in a forked child
// that re-execs this binary via death_test_runner_posix.hpp; the child installs
// handlers so assert (SIGABRT) and std::terminate become a fixed exit code.
// POSIX-only; gated to linux/macosx.
//
// Cases:
//   SN-2  duplicate publication — fully finalized group; first publication
//        reaches Completed; a second select_publish_locked call must fail fast
//        BEFORE result rewrite / count decrement / make_runnable / route.
//   SN-10 open authority at publication — winner exists but one Event arm still
//        linked (or one Timer registration still ACTIVE); select_publish_locked
//        must fail fast BEFORE the result write.
//   FP   suspended caller not Waiting — phase Armed, caller state Running,
//        all other publication preconditions valid; select_publish_locked must
//        fail fast BEFORE routing.
//   MG   multiple groups share one Event — two suspended caller Fibers, two
//        distinct SelectGroups, same Event; Event.set() detects >1 distinct
//        eligible group and the P8 stage-boundary fail-fast fires BEFORE any
//        group winner CAS.
//   CTL  valid publication control — a valid synthetic suspended publication
//        completes normally (exit 0). The parent explicitly executes every
//        child case.
//
// These reach the production publication entry through the guarded
// AsyncTestAccess::select_publish / select_resolve_event internal-testing
// drivers (real production methods; no parallel publication implementation).
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
namespace stest = sluice_async_test;
using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using Scheduler = sa::Scheduler;
using Event = sa::Event;
using Fiber = sa::Fiber;
using WorkerState = sa::WorkerState;
using SelectArmSlot = sad::SelectArmSlot;
using SelectGroup = sad::SelectGroup;
using SelectTimerReg = sad::SelectTimerRegistration;
using ArmState = sad::ArmState;
using GroupPhase = sad::GroupPhase;
using CompletionMode = sad::CompletionMode;

void install_death_handlers() noexcept {
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
    std::set_terminate([]() noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
}

// Drive a caller Fiber through the lawful state chain to Waiting: created ->
// runnable -> running -> waiting. The publication suspended branch requires
// caller state == Waiting (its make_runnable CAS waiting->runnable is the
// exactly-once publication guard). On a non-worker death-test child thread
// there is no scheduler running the fiber, so the chain is driven manually.
void force_caller_to_waiting(Fiber& f) noexcept {
    (void)f.make_runnable();   // created -> runnable
    f.make_running();          // runnable -> running
    f.make_waiting();          // running -> waiting
}

// A real caller Fiber + stack so make_runnable/make_waiting operate on a live
// Fiber state machine. The fiber does not run a body; it is a state object the
// publication path treats as the caller.
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::array<std::byte, kBytes> bytes;
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// ---- Lean assembly: build a finalized group via the REAL production path ----
// Build a registered armed group, mark the Event arm CandidateReady, then run
// select_process_group_locked (the real P4 claim + finalize + authority-close).
// The group is then in exactly the pre-publication shape (winner claimed, all
// authority closed, phase armed, no result). Used to drive select_publish_locked.
struct PreparedGroup {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    SelectGroup group;
    SelectArmSlot arms[2];
    Event ev;
    SelectTimerReg* timer_reg{nullptr};
    Fiber caller;
    FiberStack stack;
    // A valid WorkerState for the caller's owner (the publication preflight
    // requires caller_owner_ != nullptr; route_runnable_locked routes to the
    // owner's local_runnable). Stack-allocated; id is 0.
    WorkerState caller_owner;

    PreparedGroup() : sched(ctx), ctrl(sched), ev(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
        group.scheduler_ = &sched;
        group.arms_ = arms;
        group.arm_count_ = 2;
        group.caller_ = &caller;
        group.caller_owner_ = &caller_owner;
        sched.init_fiber(caller, stack.base(), stack.size());

        // Event arm (index 0).
        arms[0].construct_event(ev);
        arms[0].state = ArmState::prepared;
        arms[0].group = &group;
        AsyncTestAccess::select_event_link(sched, ev, arms[0]);

        // Timer arm (index 1).
        arms[1].construct_timer(/*deadline=*/100);
        arms[1].state = ArmState::prepared;
        arms[1].group = &group;
        timer_reg = stest::E13SelectTimerSeam::register_synthetic(
            sched, &arms[1], /*deadline=*/100);
        arms[1].timer.stable_reg_ = timer_reg;
        arms[1].state = ArmState::registered;

        // Arm the group + mark the Event arm CandidateReady, then run the real
        // P4 claim + finalize (select_process_group_locked). This finalizes the
        // Event winner (unlink + Retired) and retires the Timer loser.
        group.set_phase(GroupPhase::armed);
        AsyncTestAccess::set_arm_state(sched, arms[0], ArmState::candidate_ready);
        bool won = AsyncTestAccess::select_process_group(sched, group, /*idx=*/0);
        if (!won) std::_Exit(sluice_death_test::kChildTestFailExit);
        // Mirror the admission suspension commit (task §7.2): the real admission
        // path increments waiting_select_count_ when it commits Waiting + Armed.
        // PreparedGroup bypasses admission, so bump it here so the suspended
        // publication branch's underflow guard sees one Armed group.
        AsyncTestAccess::inc_waiting_select_for_test(sched);
        // Now: winner==0, every arm Retired, Event arm unlinked, Timer reg RETIRED,
        // all authority closed. Phase still Armed, completion_mode None, no result.
    }
};

// SN-2 — duplicate publication. First publish succeeds (inline-style on this
// finalized group is NOT valid since phase is Armed -> suspended branch needs a
// Waiting caller). Instead: drive the first publish with the caller Waiting so
// it completes, then drive a SECOND publish which must fail fast before any
// rewrite / decrement / make_runnable / route.
void child_sn2_duplicate_publication() {
    install_death_handlers();
    PreparedGroup g;
    // Put the caller into Waiting so the suspended branch's make_runnable
    // succeeds on the first publish.
    force_caller_to_waiting(g.caller);
    // First publication: valid (phase Armed, caller Waiting, authority closed).
    AsyncTestAccess::select_publish(g.sched, g.group);
    // Second publication: phase is now Completed + result has a winner + mode
    // set -> select_publish_locked must fail fast at entry (phase / mode /
    // result checks) BEFORE any rewrite / decrement / make_runnable / route.
    AsyncTestAccess::select_publish(g.sched, g.group);
    // Unreachable: the second publish must terminate.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// SN-10 — open authority at publication. Re-link the winner Event arm AFTER
// finalization (re-opening its authority), then select_publish_locked must fail
// fast BEFORE the result write (the all-authority-closed predicate fails).
void child_sn10_open_authority_at_publication() {
    install_death_handlers();
    PreparedGroup g;
    // Re-open the winner Event arm's authority: re-link it into the Event port.
    // The winner arm is currently Retired + unlinked (home_ == nullptr).
    AsyncTestAccess::set_arm_state(g.sched, g.arms[0], ArmState::registered);
    AsyncTestAccess::select_event_link(g.sched, g.ev, g.arms[0]);
    // Now the Event arm is linked again -> authority NOT closed. Publication
    // must fail fast at the all-authority-closed check (BEFORE result write).
    AsyncTestAccess::select_publish(g.sched, g.group);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// FP — suspended caller not Waiting. The caller is Running (not Waiting); all
// other publication preconditions valid. select_publish_locked must fail fast
// at the caller-state check BEFORE routing (make_runnable / route).
void child_fp_suspended_caller_not_waiting() {
    install_death_handlers();
    PreparedGroup g;
    // Leave the caller in Running (its default after init_fiber). The suspended
    // branch requires caller state == Waiting; Running fails the FP check.
    // (caller.make_waiting() is deliberately NOT called.)
    AsyncTestAccess::select_publish(g.sched, g.group);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// MG — multiple groups share one Event. Two armed groups, each with an arm on
// the same Event; Event.set() (via select_resolve_event) detects >1 distinct
// eligible group and the P8 stage-boundary fail-fast fires BEFORE any group
// winner CAS.
void child_mg_multi_group_event() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    stest::E11TimerControl::enable_test_clock(sched);
    Event ev(sched);

    SelectGroup ga, gb;
    SelectArmSlot arm_a, arm_b;
    // Group A: one Event arm on `ev`, Armed.
    ga.scheduler_ = &sched;
    ga.arms_ = &arm_a;
    ga.arm_count_ = 1;
    ga.set_phase(GroupPhase::armed);
    arm_a.construct_event(ev);
    arm_a.state = ArmState::prepared;
    arm_a.group = &ga;
    AsyncTestAccess::select_event_link(sched, ev, arm_a);
    // Group B: a second Event arm on the SAME `ev`, Armed.
    gb.scheduler_ = &sched;
    gb.arms_ = &arm_b;
    gb.arm_count_ = 1;
    gb.set_phase(GroupPhase::armed);
    arm_b.construct_event(ev);
    arm_b.state = ArmState::prepared;
    arm_b.group = &gb;
    AsyncTestAccess::select_event_link(sched, ev, arm_b);

    // select_resolve_event sees TWO distinct eligible groups -> P8 stage-
    // boundary fail-fast BEFORE any candidate mutation / winner CAS.
    AsyncTestAccess::select_resolve_event(sched, ev);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CTL — valid publication control. A valid synthetic suspended publication
// completes normally (exit 0). Proves the harness + publication path are sound
// for the normal case, so the SN-2/SN-10/FP/MG terminations are attributable to
// the invariant violations, not a harness fault.
void child_ctl_valid_publication() {
    install_death_handlers();
    PreparedGroup g;
    // Valid suspended publication: caller Waiting, authority closed, phase Armed.
    force_caller_to_waiting(g.caller);
    AsyncTestAccess::select_publish(g.sched, g.group);
    // Normal completion -> exit 0 (the harness expects exactly 0).
    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    if      (name == "SN-2")  child_sn2_duplicate_publication();
    else if (name == "SN-10") child_sn10_open_authority_at_publication();
    else if (name == "FP")    child_fp_suspended_caller_not_waiting();
    else if (name == "MG")    child_mg_multi_group_event();
    else if (name == "CTL")   child_ctl_valid_publication();
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

    must_term("SN-2");   // duplicate publication -> fail fast before rewrite
    must_term("SN-10");  // open authority -> fail fast before result write
    must_term("FP");     // caller not Waiting -> fail fast before routing
    must_term("MG");     // multi-group Event -> P8 stage fail-fast before CAS
    must_zero("CTL");    // valid publication -> normal exit 0

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (SN-2 duplicate-publish / "
                     "SN-10 open-authority / FP caller-not-waiting / "
                     "MG multi-group-event-stage / CTL valid-publication)\n";
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
    std::cout << "NOT RUN: e13_select_publication_death_test is POSIX-only "
                 "(fork/re-exec death-test runner).\n";
    return 0;
}

#endif  // defined(__unix__)
