// e13_select_timer_pump_death_test — E13 Select timer pump + destruction +
// authority-seal death tests (P3 + P3 Corrective).
//
// Verifies four invariant families:
//  (1) Pump stage boundary: a due ACTIVE SelectTimerRegistration is unreachable
//      in valid P3 (no admission path). If the pump pops an ACTIVE Select entry
//      it MUST fail fast rather than claim/mark/retire/consume/erase.
//  (2) Destruction contract (Corrective closure 2): the Scheduler destructor
//      permits terminal (RETIRED/CONSUMED) lazy Select blocks whose deadlines
//      never elapsed, but MUST assert if an ACTIVE block remains.
//  (3) Accounting-authority seal (Corrective closure 5): the Scheduler helpers
//      select_timer_retire_locked / select_timer_consume_locked reject a cross-
//      Scheduler call and a pool-membership violation BEFORE any state/counter
//      mutation.
//
// Each case runs in a forked child that re-execs this binary; the child
// installs handlers so assert (SIGABRT) and std::terminate become a fixed exit
// code. The parent asserts the exact exit code (see death_test_runner_posix.hpp).
//
// Cases:
//  PA   Pump ACTIVE-due — register ACTIVE Select, advance clock to its deadline.
//  CTL  Control — register + retire before advancing; stale skip, exit 0.
//  SD1  Destruction with terminal lazy block — retire, destroy without pumping.
//  SD2  Destruction with ACTIVE block — destroy without retiring -> assert.
//  XR   Cross-Scheduler retire — Sched B retires a Sched-A-owned block -> assert.
//  XC   Cross-Scheduler consume — Sched B consumes a Sched-A-owned block -> assert.
//  NP   Non-pool member — retire a detached local via Sched A -> assert.
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

// PA — pump ACTIVE-due. Register an ACTIVE synthetic Select entry (arm ==
// nullptr, no real group), then advance the clock to its deadline. Under P6 the
// pump's ACTIVE path drives select_resolve_timer_locked, which validates the
// block and fail-fasts (select_invariant_fail_fast) because arm == nullptr (a
// synthetic entry has no real caller-frame arm). This still proves the pump
// reached the ACTIVE branch (the real resolver runs), now via the production
// ACTIVE-validation entry rather than the retired P3 stage guard.
void child_pa_pump_active_due() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);
    sluice_async_test::TimerTestControl::enable_test_clock(sched);

    // Register an ACTIVE Select entry with a future deadline (no real arm).
    sluice_async_test::SelectTimerSeam::register_synthetic(
        sched, nullptr, /*deadline=*/50);

    // Advance the clock to the deadline: the pump pops the ACTIVE entry and
    // invokes select_resolve_timer_locked, which fail-fasts on arm == nullptr.
    sluice_async_test::SelectTimerSeam::advance_clock(sched, 50);

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
    sluice_async_test::TimerTestControl::enable_test_clock(sched);

    SelectTimerReg* r = sluice_async_test::SelectTimerSeam::register_synthetic(
        sched, nullptr, /*deadline=*/50);
    bool ok = sluice_async_test::SelectTimerSeam::retire_synthetic(sched, *r);
    if (!ok) std::_Exit(sluice_death_test::kChildTestFailExit);

    sluice_async_test::SelectTimerSeam::advance_clock(sched, 50);
    std::_Exit(0);
}

// --------------------------------------------------------------------------
// Destruction contract (Corrective closure 2)
// --------------------------------------------------------------------------

// SD1 — destruction with a terminal lazy block. Register ACTIVE, retire via the
// Scheduler helper, then destroy WITHOUT advancing the clock (the deadline never
// elapses). A RETIRED block remains inert in the pool; the destructor MUST reap
// it and exit 0. (Does NOT use a fixture that drains — the destruction semantics
// are the subject under test.) Proves the destructor permits terminal lazy blocks.
//
// NOTE: ~Scheduler must run via a real scope exit. std::_Exit skips destructors,
// so the Scheduler and its controller are placed in an inner block whose closing
// brace runs ~Scheduler (and ~ControllerGuard) BEFORE the _Exit.
void child_sd1_destruction_terminal_lazy() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    {
        Scheduler sched(ctx);
        sluice_async_test::ControllerGuard ctrl(sched);
        sluice_async_test::TimerTestControl::enable_test_clock(sched);

        SelectTimerReg* r = sluice_async_test::SelectTimerSeam::register_synthetic(
            sched, nullptr, /*deadline=*/50);
        bool ok = sluice_async_test::SelectTimerSeam::retire_synthetic(sched, *r);
        if (!ok) std::_Exit(sluice_death_test::kChildTestFailExit);

        // Do NOT advance the clock: the RETIRED block stays in the pool (lazy).
        if (sluice_async_test::SelectTimerSeam::pool_size(sched) != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // ~Scheduler runs at the closing brace: must reap the inert block and
        // return normally (no assert).
    }
    std::_Exit(0);
}

// SD2 — destruction with an ACTIVE block. Register ACTIVE and destroy WITHOUT
// retiring. The destructor's `!any_active_select` assert MUST fire (live Select
// timer authority remains — caller contract violation). See SD1 for why the
// Scheduler lives in an inner block (std::_Exit skips destructors).
void child_sd2_destruction_active() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    {
        Scheduler sched(ctx);
        sluice_async_test::ControllerGuard ctrl(sched);
        sluice_async_test::TimerTestControl::enable_test_clock(sched);

        sluice_async_test::SelectTimerSeam::register_synthetic(
            sched, nullptr, /*deadline=*/50);
        // ~Scheduler runs at the closing brace: ACTIVE block remains -> assert
        // (SIGABRT) -> kExpectedTerminateExit.
    }
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// --------------------------------------------------------------------------
// Accounting-authority seal (Corrective closure 5)
// --------------------------------------------------------------------------

// XR — cross-Scheduler retire. A synthetic block owned by Scheduler A is
// retired through Scheduler B. select_timer_retire_locked asserts
// reg.scheduler() == this BEFORE any state/counter mutation -> assert.
void child_xr_cross_scheduler_retire() {
    install_death_handlers();
    sa::AsyncIoContext ctx_a(std::make_unique<sa::FakeAsyncBackend>());
    sa::AsyncIoContext ctx_b(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched_a(ctx_a);
    Scheduler sched_b(ctx_b);
    sluice_async_test::ControllerGuard ctrl_a(sched_a);
    sluice_async_test::ControllerGuard ctrl_b(sched_b);
    sluice_async_test::TimerTestControl::enable_test_clock(sched_a);

    SelectTimerReg* r = sluice_async_test::SelectTimerSeam::register_synthetic(
        sched_a, nullptr, /*deadline=*/50);

    // Retire the Sched-A-owned block through Sched B -> MUST assert.
    sluice_async_test::SelectTimerSeam::retire_synthetic(sched_b, *r);

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// XC — cross-Scheduler consume. Same shape as XR but via consume.
void child_xc_cross_scheduler_consume() {
    install_death_handlers();
    sa::AsyncIoContext ctx_a(std::make_unique<sa::FakeAsyncBackend>());
    sa::AsyncIoContext ctx_b(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched_a(ctx_a);
    Scheduler sched_b(ctx_b);
    sluice_async_test::ControllerGuard ctrl_a(sched_a);
    sluice_async_test::ControllerGuard ctrl_b(sched_b);
    sluice_async_test::TimerTestControl::enable_test_clock(sched_a);

    SelectTimerReg* r = sluice_async_test::SelectTimerSeam::register_synthetic(
        sched_a, nullptr, /*deadline=*/50);

    // Consume the Sched-A-owned block through Sched B -> MUST assert.
    sluice_async_test::SelectTimerSeam::consume_synthetic(sched_b, *r);

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// DG — detached-CAS guard. A Scheduler-owned (registered) block is passed to
// detached_retire (the test-only CAS accessor). The assert inside
// detached_retire fires because reg.scheduler() != nullptr, catching the
// misuse BEFORE any state/counter mutation. Proves the detached-only access
// guard rejects Scheduler-owned registrations.
void child_dg_detached_cas_guard() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);
    sluice_async_test::TimerTestControl::enable_test_clock(sched);

    SelectTimerReg* r = sluice_async_test::SelectTimerSeam::register_synthetic(
        sched, nullptr, /*deadline=*/50);

    // detached_retire on a Scheduler-owned block -> assert(scheduler==nullptr).
    sluice_async_test::SelectTimerSeam::detached_retire(*r);

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// NP — non-pool member. A detached stack-local block is bound to Scheduler A by
// pointer (scheduler() == &sched_a) but is NOT in A's select_timer_pool_.
// Retiring it via Sched A's helper trips pool_owns_select_block_locked ->
// assert. Proves the membership gate, not just the pointer-binding gate.
void child_np_non_pool_member_retire() {
    install_death_handlers();
    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);
    sluice_async_test::TimerTestControl::enable_test_clock(sched);

    // Detached local: scheduler() == &sched, but NOT spliced into the pool.
    SelectTimerReg local(nullptr, &sched, /*deadline=*/50);

    // retire_synthetic routes through select_timer_retire_locked, which asserts
    // pool_owns_select_block_locked(local) -> fails -> assert.
    sluice_async_test::SelectTimerSeam::retire_synthetic(sched, local);

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

void dispatch_child(const std::string& name) {
    if (name == "PA") child_pa_pump_active_due();
    else if (name == "CTL") child_ctl_control();
    else if (name == "SD1") child_sd1_destruction_terminal_lazy();
    else if (name == "SD2") child_sd2_destruction_active();
    else if (name == "XR") child_xr_cross_scheduler_retire();
    else if (name == "XC") child_xc_cross_scheduler_consume();
    else if (name == "NP") child_np_non_pool_member_retire();
    else if (name == "DG") child_dg_detached_cas_guard();
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
    must_zero("SD1");  // terminal lazy block destructed normally
    must_term("SD2");  // ACTIVE block at destruction -> assert
    must_term("XR");   // cross-Scheduler retire -> assert before mutation
    must_term("XC");   // cross-Scheduler consume -> assert before mutation
    must_term("NP");   // non-pool member retire -> membership assert
    must_term("DG");   // detached-CAS guard -> assert(scheduler==nullptr)

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (PA pump-active fail-fast / "
                     "CTL stale-skip control / SD1 terminal-lazy destruct / "
                     "SD2 active-destruct / XR cross-sched retire / XC "
                     "cross-sched consume / NP non-pool-member / "
                     "DG detached-CAS-guard)\n";
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
