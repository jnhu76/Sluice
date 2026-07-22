// e13_select_claim_death_test — E13 P4 Select claim/finalization death tests.
//
// Verifies the P4 fail-fast assertions terminate the program:
//
// Pre-CAS preflight assertions (fire BEFORE the winner CAS, so an invalid group
// cannot become permanently claimed):
//   CG  candidate index out of range
//   CS  group.scheduler_ belongs to another Scheduler
//   CP  candidate arm is not CandidateReady
//   CA  candidate arm.group does not match group
//   EH  Event arm not in its Event port's intrusive list (stale-but-equality
//       home_: passes the home_ equality check, fails the mechanical scan)
//   TN  Timer arm has null stable registration
//   TF  Timer registration belongs to another Scheduler
//   TP  Timer registration is not owned by the Scheduler pool
//
// Post-claim publication-precondition assertion:
//   OA  all-authority-closed assert invoked while one authority remains open
//
// Normal-exit control:
//   CTL valid process completes, exit 0
//
// Totals: 9 expected-termination cases (CG/CS/CP/CA/EH/TN/TF/TP/OA) +
//         1 normal-exit control (CTL).
//
// Each case runs in a forked child that re-execs this binary; the child
// installs handlers so assert (SIGABRT) and std::terminate become a fixed exit
// code. The parent asserts the exact exit code (death_test_runner_posix.hpp).
// The parent process explicitly dispatches and executes every case.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
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
using SelectArmSlot = sad::SelectArmSlot;
using SelectGroup = sad::SelectGroup;
using SelectTimerReg = sad::SelectTimerRegistration;
using ArmState = sad::ArmState;
using GroupPhase = sad::GroupPhase;

void install_death_handlers() noexcept {
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
    std::set_terminate([]() noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
}

// Minimal registered-group builder for death tests. Mirrors ClaimFixture but
// stripped down: builds exactly the arms each case needs. Returns the group +
// arms by reference so a case can perturb one field before process.
struct DGroup {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    sluice_async_test::ControllerGuard ctrl;
    SelectGroup group;
    SelectArmSlot arms[2];
    Event ev;

    DGroup() : sched(ctx), ctrl(sched), ev(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
        group.scheduler_ = &sched;
        group.arms_ = arms;
        group.set_phase(GroupPhase::armed);
    }
    // NOTE: no teardown — death-test children _Exit() and never run ~DGroup.
    // The invalid group never claims (the assert fires first), so no ACTIVE
    // Timer reg / linked Event arm persists to matter for ~Scheduler.

    // Add one Event arm at index 0, CandidateReady.
    void add_event_candidate() {
        arms[0].construct_event(ev);
        arms[0].state = ArmState::prepared;
        arms[0].group = &group;
        AsyncTestAccess::select_event_link(sched, ev, arms[0]);
        arms[0].state = ArmState::candidate_ready;
        group.arm_count_ = 1;
    }
    // Add one Timer arm at index 0, CandidateReady, ACTIVE reg.
    // Mirrors ClaimFixture::add_timer_arm: register the synthetic block FIRST,
    // then bind it as the arm's stable back-pointer via construct_timer (the
    // same path the future admission protocol uses).
    void add_timer_candidate(Scheduler::deadline_t deadline) {
        SelectTimerReg* reg = sluice_async_test::E13SelectTimerSeam::register_synthetic(
            sched, &arms[0], deadline);
        arms[0].construct_timer(deadline, reg);
        arms[0].state = ArmState::prepared;
        arms[0].group = &group;
        arms[0].state = ArmState::candidate_ready;
        group.arm_count_ = 1;
    }
};

// ---------------------------------------------------------------------------
// Child bodies
// ---------------------------------------------------------------------------

// CG — candidate index out of range. group.arm_count_ == 1, candidate_index==5.
void child_cg_candidate_out_of_range() {
    install_death_handlers();
    DGroup g;
    g.add_event_candidate();
    AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/5);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CS — group.scheduler_ belongs to another Scheduler.
void child_cs_cross_scheduler_group() {
    install_death_handlers();
    DGroup g;
    g.add_event_candidate();
    sa::AsyncIoContext ctx_b(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched_b(ctx_b);
    // group.scheduler_ stays bound to g.sched (keeping the arms valid under
    // g.sched). We invoke select_process_group via sched_b, so inside the core
    // `this == &sched_b` while `group.scheduler_ == &g.sched`, and the preflight
    // asserts group.scheduler_ == this.
    AsyncTestAccess::select_process_group(sched_b, g.group, /*candidate=*/0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CP — candidate arm is not CandidateReady.
void child_cp_candidate_not_ready() {
    install_death_handlers();
    DGroup g;
    // Build an Event arm but leave it Registered (not CandidateReady).
    g.arms[0].construct_event(g.ev);
    g.arms[0].state = ArmState::prepared;
    g.arms[0].group = &g.group;
    AsyncTestAccess::select_event_link(g.sched, g.ev, g.arms[0]);
    // arms[0].state is Registered here.
    g.group.arm_count_ = 1;
    AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CA — arm.group does not match group.
void child_ca_arm_group_mismatch() {
    install_death_handlers();
    DGroup g;
    g.add_event_candidate();
    // Point the candidate arm's group at a DIFFERENT group.
    SelectGroup other;
    g.arms[0].group = &other;
    AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// EH — Event arm is not reachable from its Event port's intrusive list.
// This is the mechanical-membership-negative case (P1-2): the arm must pass the
// home_ equality check (arm.home_ == &event.select_port_) but NOT be reachable
// from the port's intrusive list, so the `found` scan in
// select_preflight_claim_locked fails. We unlink through the canonical helper
// (clearing home_/next_/prev_ and removing the arm from the list), then forge a
// stale-but-equality-passing home_ via the guarded test seam. This proves the
// intrusive-membership scan is load-bearing: a home_ that merely looks right is
// rejected when the arm is not actually linked.
void child_eh_event_arm_not_linked() {
    install_death_handlers();
    DGroup g;
    g.add_event_candidate();
    AsyncTestAccess::select_event_unlink(g.sched, g.ev, g.arms[0]);
    AsyncTestAccess::select_event_forge_stale_home(g.sched, g.ev, g.arms[0]);
    AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// TN — Timer arm has null stable registration.
void child_tn_timer_null_reg() {
    install_death_handlers();
    DGroup g;
    // Build a Timer arm with a null stable_reg_.
    g.arms[0].construct_timer(/*deadline=*/100, /*reg=*/nullptr);
    g.arms[0].state = ArmState::candidate_ready;
    g.arms[0].group = &g.group;
    g.group.arm_count_ = 1;
    AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// TF — Timer registration belongs to another Scheduler.
void child_tf_timer_other_scheduler() {
    install_death_handlers();
    DGroup g;
    // Register the Timer reg on a DIFFERENT scheduler, then bind the arm to it
    // and process under g.sched. Preflight asserts reg->scheduler() == this.
    sa::AsyncIoContext ctx_b(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched_b(ctx_b);
    sluice_async_test::ControllerGuard ctrl_b(sched_b);
    stest::E11TimerControl::enable_test_clock(sched_b);
    SelectTimerReg* reg_b = sluice_async_test::E13SelectTimerSeam::register_synthetic(
        sched_b, &g.arms[0], /*deadline=*/100);
    g.arms[0].construct_timer(100, reg_b);
    g.arms[0].state = ArmState::candidate_ready;
    g.arms[0].group = &g.group;
    g.group.arm_count_ = 1;
    AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// TP — Timer registration is not owned by the Scheduler pool.
void child_tp_timer_not_pool_owned() {
    install_death_handlers();
    DGroup g;
    // A detached local registration bound to g.sched by pointer but NOT spliced
    // into g.sched's pool. Preflight asserts pool_owns_select_block_locked.
    SelectTimerReg local(&g.arms[0], &g.sched, /*deadline=*/100);
    g.arms[0].construct_timer(100, &local);
    g.arms[0].state = ArmState::candidate_ready;
    g.arms[0].group = &g.group;
    g.group.arm_count_ = 1;
    AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// OA — the post-claim all-authority-closed publication precondition assert.
// select_process_group_locked asserts select_all_authority_closed_locked(group)
// after a successful claim; in valid P4 that always holds (claim+finalize
// closes all authority by construction). To prove the assert is mechanically
// enforced — not merely a bool predicate that could be ignored — this child
// claims+finalizes normally, then SYNTHETICALLY RE-OPENS the winner's Event
// authority (home_ != nullptr), and invokes the guarded
// assert_select_all_authority_closed seam. The assert MUST fire (SIGABRT -> 86).
// This is the publication-precondition negative case: a future P6 publication
// entry will gate on this exact assert, and it must terminate deterministically
// when an authority is open.
void child_oa_open_authority_terminates() {
    install_death_handlers();
    DGroup g;
    g.add_event_candidate();
    bool won = AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    if (!won) std::_Exit(sluice_death_test::kChildTestFailExit);
    // Confirm the invariant holds immediately after a valid process.
    if (!AsyncTestAccess::select_all_authority_closed(g.sched, g.group))
        std::_Exit(sluice_death_test::kChildTestFailExit);
    // Synthetically re-open the winner's Event authority. The predicate checks
    // arm.home_ != nullptr for Event arms; a non-null sentinel (never
    // dereferenced) flips the invariant to false.
    g.arms[0].home_ = reinterpret_cast<sad::SelectPort*>(0x1);
    // This assert must terminate the program (exit 86).
    AsyncTestAccess::assert_select_all_authority_closed(g.sched, g.group);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CTL — control. Valid process, exit 0.
void child_ctl_control() {
    install_death_handlers();
    DGroup g;
    g.add_event_candidate();
    bool won = AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    if (!won) std::_Exit(sluice_death_test::kChildTestFailExit);
    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    if      (name == "CG") child_cg_candidate_out_of_range();
    else if (name == "CS") child_cs_cross_scheduler_group();
    else if (name == "CP") child_cp_candidate_not_ready();
    else if (name == "CA") child_ca_arm_group_mismatch();
    else if (name == "EH") child_eh_event_arm_not_linked();
    else if (name == "TN") child_tn_timer_null_reg();
    else if (name == "TF") child_tf_timer_other_scheduler();
    else if (name == "TP") child_tp_timer_not_pool_owned();
    else if (name == "OA") child_oa_open_authority_terminates();
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

    // Pre-CAS preflight failures MUST terminate before the winner CAS.
    must_term("CG");  // candidate index out of range
    must_term("CS");  // group.scheduler_ != this
    must_term("CP");  // candidate arm not CandidateReady
    must_term("CA");  // candidate arm.group != &group
    must_term("EH");  // Event arm not in its port's intrusive list
    must_term("TN");  // Timer arm null stable_reg_
    must_term("TF");  // Timer registration on another Scheduler
    must_term("TP");  // Timer registration not pool-owned
    // OA: the post-claim all-authority-closed publication-precondition assert.
    // A valid claim closes all authority by construction, so this child claims
    // normally then synthetically re-opens one authority and invokes the
    // asserting seam — the assert MUST fire.
    must_term("OA");
    // CTL: valid process completes, exit 0.
    must_zero("CTL");

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (9 expected-termination: "
                     "CG candidate-range / CS cross-Sched group / CP candidate-"
                     "not-ready / CA arm-group-mismatch / EH event-arm-not-"
                     "intrusively-linked / TN timer-null-reg / TF timer-other-"
                     "Sched / TP timer-not-pool-owned / OA open-authority-"
                     "publication-precondition; 1 control: CTL)\n";
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
    std::cout << "e13_select_claim_death_test: NOT RUN on this platform "
                 "(POSIX fork/exec harness only; see death_test_runner_posix.hpp)\n";
    return 0;
}

#endif  // defined(__unix__)
