// select_rollback_invariant_death_test — E13 P7 rollback-domain negative tests.
//
// Verifies the P7 fail-fast preconditions terminate the program:
//
//   N1 / SN-8  BeginRollback rejected after the caller is suspended
//              (group phase == Armed) — rollback is forbidden after suspension
//   N2         BeginRollback rejected after FinishRegistration (phase Selecting)
//   N3         BeginRollback rejected when a winner already exists
//   N4         double rollback rejected (BeginRollback on phase Rollback)
//   N5         per-arm rollback rejects an Event arm whose home_ points at the
//              WRONG Event's SelectPort (would unlink another Event's registry)
//   N6         per-arm rollback rejects a Timer arm whose stable block is NOT
//              Scheduler-pool-owned (local / cross-Scheduler)
//   N7         per-arm rollback rejects a Timer arm whose registration is
//              already terminal (RETIRED/CONSUMED) — NOT an idempotent success
//   N8         FinishRollback rejected while one registered authority remains
//              open (an arm not yet Retired)
//   N9         (structural + runtime) rollback never reaches publication —
//              proven by the positive suite (counters == 0); no death case.
//
// Totals: 8 expected-termination cases (N1/N2/N3/N4/N5/N6/N7/N8) +
//         1 normal-exit control (CTL).
//
// Each case runs in a forked child that re-execs this binary; the child
// installs handlers so assert (SIGABRT) and std::terminate become a fixed exit
// code. The parent asserts the exact exit code (death_test_runner_posix.hpp).
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
#include <memory>
#include <string>
#include <vector>

#include <sluice/async/fiber.hpp>
#include <sluice/async/select.hpp>

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

// Minimal rollback-domain fixture. The BeginRollback phase/winner checks fire
// BEFORE the caller checks, so the phase-rejection cases (N1..N4) need no
// caller. The per-arm / finish cases (N5..N8) drive the sub-helpers directly,
// which do not check phase/caller. NOTE: no teardown — death-test children
// _Exit() and never run destructors.
struct RGroup {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    SelectGroup group;
    SelectArmSlot arms[2];
    Event ev0;
    Event ev1;

    RGroup() : sched(ctx), ctrl(sched), ev0(sched), ev1(sched) {
        stest::TimerTestControl::enable_test_clock(sched);
        group.scheduler_ = &sched;
        group.arms_ = arms;
        group.set_phase(GroupPhase::building);
    }

    // Register one Event arm at index 0 (linked, Registered).
    void add_event_arm(std::size_t idx) {
        arms[idx].construct_event(ev0);
        arms[idx].state = ArmState::prepared;
        arms[idx].group = &group;
        AsyncTestAccess::select_event_link(sched, ev0, arms[idx]);
        // select_event_link sets Registered.
        group.arm_count_ = static_cast<std::size_t>(idx) + 1;
    }
    // Register one Timer arm at index `idx`, ACTIVE reg, Scheduler-pool-owned.
    void add_timer_arm(std::size_t idx, Scheduler::deadline_t deadline) {
        SelectTimerReg* reg =
            stest::SelectTimerSeam::register_synthetic(sched, &arms[idx],
                                                           deadline);
        arms[idx].construct_timer(deadline, reg);
        arms[idx].state = ArmState::prepared;
        arms[idx].group = &group;
        arms[idx].state = ArmState::registered;
        group.arm_count_ = static_cast<std::size_t>(idx) + 1;
    }
};

// ---------------------------------------------------------------------------
// Child bodies
// ---------------------------------------------------------------------------

// N1 / SN-8 — BeginRollback after suspension (phase Armed) must fail fast.
// Rollback is forbidden after caller suspension; there is no best-effort path.
void child_n1_rollback_after_suspension() {
    install_death_handlers();
    RGroup g;
    g.add_event_arm(0);  // arm_count == 1 so the phase check (not arm_count) fires
    g.group.set_phase(GroupPhase::armed);  // caller was suspended
    AsyncTestAccess::select_begin_rollback(g.sched, g.group);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// N2 — BeginRollback after FinishRegistration (phase Selecting) must fail fast.
void child_n2_rollback_after_finish_registration() {
    install_death_handlers();
    RGroup g;
    g.add_event_arm(0);
    g.group.set_phase(GroupPhase::selecting);
    AsyncTestAccess::select_begin_rollback(g.sched, g.group);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// N3 — BeginRollback when a winner already exists must fail fast BEFORE any
// cleanup mutation.
void child_n3_rollback_after_winner() {
    install_death_handlers();
    RGroup g;
    g.add_event_arm(0);
    g.arms[0].state = ArmState::candidate_ready;
    // Claim the winner through the real processor (sets group.winner_). The
    // processor's preflight asserts the phase is Selecting or Armed, so drive
    // it in the Selecting domain rather than Building — otherwise the child
    // would terminate on the preflight PHASE assert instead of reaching
    // BeginRollback, masking the winner-precondition we mean to prove.
    g.group.set_phase(GroupPhase::selecting);
    const bool claimed =
        AsyncTestAccess::select_process_group(g.sched, g.group, /*candidate=*/0);
    // The whole point of N3 is a PRESENT winner; if the claim did not land we
    // would be proving nothing. Bail to a distinct non-termination exit so a
    // regression (silent claim-lost) shows up as an explicit failure rather
    // than a false pass.
    if (!claimed || g.group.winner() == sad::kNoWinner) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Reset ONLY the phase to Building so BeginRollback passes its phase check
    // and reaches the winner precondition (the invariant under test). winner
    // is left in place on purpose.
    g.group.set_phase(GroupPhase::building);
    AsyncTestAccess::select_begin_rollback(g.sched, g.group);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// N4 — double rollback rejected (BeginRollback on phase Rollback).
void child_n4_double_rollback() {
    install_death_handlers();
    RGroup g;
    g.add_event_arm(0);
    g.group.set_phase(GroupPhase::rollback);  // already mid-rollback
    AsyncTestAccess::select_begin_rollback(g.sched, g.group);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// N5 — per-arm rollback rejects an Event arm whose home_ points at the WRONG
// Event's SelectPort. Forge home_ at ev1's port while the arm's event_ is ev0.
void child_n5_wrong_event_membership() {
    install_death_handlers();
    RGroup g;
    g.add_event_arm(0);  // arm[0].event_ == &ev0, linked into ev0's port
    // Forge: point home_ at ev1's port (wrong Event). The mechanical membership
    // check (arm.home_ == &arm.event.event_->select_port_) fails -> fail fast
    // BEFORE any unlink mutation.
    AsyncTestAccess::select_event_forge_wrong_home(g.sched, g.ev0, g.ev1,
                                                   g.arms[0]);
    AsyncTestAccess::select_rollback_arm(g.sched, g.group, g.arms[0]);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// N6 — per-arm rollback rejects a Timer arm whose stable block is NOT
// Scheduler-pool-owned. Build a Timer arm whose stable_reg_ is a LOCAL object
// (never spliced into the Scheduler pool).
void child_n6_timer_not_pool_owned() {
    install_death_handlers();
    RGroup g;
    // Local, never-Scheduler-owned stable block bound to arm[0].
    SelectTimerReg local_reg{&g.arms[0], &g.sched,
                             static_cast<sa::deadline_tick_t>(1000)};
    g.arms[0].construct_timer(Scheduler::deadline_t{1000}, &local_reg);
    g.arms[0].state = ArmState::registered;
    g.arms[0].group = &g.group;
    g.group.arm_count_ = 1;
    AsyncTestAccess::select_rollback_arm(g.sched, g.group, g.arms[0]);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// N7 — per-arm rollback rejects a Timer arm whose registration is already
// terminal (RETIRED). An already-terminal registration is an invariant
// violation, NOT an idempotent success.
void child_n7_already_terminal_timer() {
    install_death_handlers();
    RGroup g;
    g.add_timer_arm(0, Scheduler::deadline_t{1000});
    // Retire the registration first (ACTIVE -> RETIRED).
    (void)stest::SelectTimerSeam::retire_synthetic(g.sched,
                                                       *g.arms[0].timer.stable_reg_);
    // arm[0] is still Registered but its reg is now RETIRED. Rollback must
    // reject it (not silently succeed).
    AsyncTestAccess::select_rollback_arm(g.sched, g.group, g.arms[0]);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// N8 — FinishRollback rejected while one registered authority remains open
// (an arm not yet Retired). Build a registered prefix where arm[0] is still
// Registered, then attempt FinishRollback with registered_count=1.
void child_n8_open_authority_before_aborted() {
    install_death_handlers();
    RGroup g;
    g.add_event_arm(0);  // arm[0] Registered (open authority)
    // Attempt FinishRollback claiming the prefix [0,1) is closed — it is NOT
    // (arm[0] is Registered, not Retired). Fail fast before setting Aborted.
    g.group.set_phase(GroupPhase::rollback);
    AsyncTestAccess::select_finish_rollback(g.sched, g.group, g.arms,
                                            /*arm_count=*/1,
                                            /*registered_count=*/1);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CTL — a valid rollback completes without terminating. Drives the real
// production rollback path end-to-end through the PUBLIC select() entry with a
// synthetic failure injected after 1 registration. The catch rolls back to
// Aborted and rethrows; the caller catches it and exits 0. (Normal-exit control
// proving the legitimate path does not assert.)
void child_ctl_valid_rollback() {
    install_death_handlers();
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    stest::TimerTestControl::enable_test_clock(sched);
    stest::TimerTestControl::set_clock(sched, 0);
    Event ev(sched);
    stest::SelectRollbackSeam::configure_fail_after(sched, 1);

    sa::Fiber fb;
    fb.set_entry([&](sa::Fiber&) {
        try {
            (void)sa::select(sched, sa::EventSelectCase{ev},
                             sa::TimerSelectCase{sched, Scheduler::deadline_t{1000}});
            // Should not reach here.
            std::_Exit(sluice_death_test::kUnexpectedReturnExit);
        } catch (const stest::SelectRollbackSeam::FailureException&) {
            // The rollback completed (group Aborted) and rethrew. Exit 0.
        }
    });
    alignas(16) std::vector<std::byte> stack(64 * 1024);
    (void)sched.init_fiber(fb, stack.data(), stack.size());
    sched.spawn(fb);
    sched.run(1);
    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    if      (name == "N1") child_n1_rollback_after_suspension();
    else if (name == "N2") child_n2_rollback_after_finish_registration();
    else if (name == "N3") child_n3_rollback_after_winner();
    else if (name == "N4") child_n4_double_rollback();
    else if (name == "N5") child_n5_wrong_event_membership();
    else if (name == "N6") child_n6_timer_not_pool_owned();
    else if (name == "N7") child_n7_already_terminal_timer();
    else if (name == "N8") child_n8_open_authority_before_aborted();
    else if (name == "CTL") child_ctl_valid_rollback();
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

    must_term("N1");  // rollback after suspension -> fail fast
    must_term("N2");  // rollback after FinishRegistration -> fail fast
    must_term("N3");  // rollback after winner -> fail fast before cleanup
    must_term("N4");  // double rollback -> fail fast
    must_term("N5");  // wrong-Event membership -> fail fast before unlink
    must_term("N6");  // Timer not pool-owned -> fail fast before mutation
    must_term("N7");  // already-terminal Timer -> fail fast (not idempotent)
    must_term("N8");  // open authority before Aborted -> fail fast
    must_zero("CTL"); // valid rollback -> normal exit 0 (Aborted set)

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (N1 rollback-after-suspension / "
                     "N2 rollback-after-finish / N3 rollback-after-winner / "
                     "N4 double-rollback / N5 wrong-Event-membership / "
                     "N6 timer-not-pool-owned / N7 already-terminal-timer / "
                     "N8 open-authority-before-aborted / CTL valid-rollback)\n";
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
    std::cout << "NOT RUN: select_rollback_invariant_death_test is POSIX-only "
                 "(forked-child death-test runner)\n";
    return 0;
}

#endif  // defined(__unix__)
