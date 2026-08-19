// async_sync_lifetime_death_test
//
// ADR-async-primitive-lifetime-failfast (issue #135 phase 6): destruction of
// AsyncMutex (while owned) and AsyncCondition (while a wait() is in flight)
// is a caller contract violation that MUST fail fast through the named
// per-authority boundary (async_mutex_lifetime_fail_fast /
// async_condition_lifetime_fail_fast), active in BOTH Debug and Release.
// The WaitQueue authority (wait_queue_lifetime_fail_fast) is exercised by
// async_rwlock_death_test A6 (destroy with queued waiter), which this ADR
// moves to the both-mode gate; AsyncRwLock's own authority by A4/A5.
//
// This binary has its OWN int main(int, char**) (NOT SLUICE_MAIN): the
// cooperative harness cannot survive std::terminate in-process, so each case
// runs in a forked child that re-execs this binary (death_test_runner_posix.hpp).
//
// Cases:
//   M1  destroy AsyncMutex while owned (fiber parked holding the lock)
//   C1  destroy AsyncCondition while a wait() is in flight (fiber parked in
//       the condition wait; active_waits_ != 0)
//   CTL control — full lock/wait/notify/reacquire/unlock cycle, then
//       quiescent destruction of both objects, exit 0.
//
// Debug fires the descriptive assert first (SIGABRT, converted by the signal
// handler to the terminate exit code); Release goes straight to the named
// fail-fast (std::terminate). Either way the parent must observe exit 86 in
// BOTH modes — the ADR's whole point.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_mutex.hpp>
#include <sluice/async/condition.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>  // std::_Exit
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using sluice::Result;
using sluice::async::AsyncBackend;
using sluice::async::AsyncCondition;
using sluice::async::AsyncIoContext;
using sluice::async::AsyncMutex;
using sluice::async::Completion;
using sluice::async::Fiber;
using sluice::async::ReadOp;
using sluice::async::Scheduler;
using sluice::async::SyncAllOp;
using sluice::async::SyncDataOp;
using sluice::async::WaitNode;
using sluice::async::WriteOp;

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything (the lifetime tests never need I/O).
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

// M1 — destroy the AsyncMutex while a parked fiber owns it. The fiber parks
// on a never-set ready flag while STILL holding the lock, so unlock is
// unreachable and owner_ != nullptr at destruction.
void child_m1_destroy_mutex_while_owned() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex* mtx = new AsyncMutex(sched);

    std::atomic<bool> owner_parked{false};
    std::atomic<bool> release{false};  // never set before delete mtx

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        WaitNode rn;
        mtx->lock(rn);
        owner_parked.store(true, std::memory_order_release);
        sched.await_ready_flag(release);  // park while holding
        mtx->unlock();
    });
    FiberStack s;
    if (!sched.init_fiber(owner, s.base(), s.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(owner);
    sched.run(1);
    // The owner Fiber is parked holding the mutex (owner_parked == true).
    delete mtx;  // MUST terminate (owner_ != nullptr); never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// C1 — destroy the AsyncCondition while a wait() is in flight. The fiber
// locks the bound mutex, parks in cond.wait (releasing the mutex), and no
// notifier ever runs, so active_waits_ == 1 at destruction.
void child_c1_destroy_condition_with_wait_in_flight() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex* mtx = new AsyncMutex(sched);
    AsyncCondition* cond = new AsyncCondition(*mtx);

    std::atomic<bool> waiter_parked{false};

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        WaitNode mlock, cn;
        mtx->lock(mlock);
        waiter_parked.store(true, std::memory_order_release);
        (void)cond->wait(cn);  // releases mtx, suspends; never resumed
    });
    FiberStack s;
    if (!sched.init_fiber(waiter, s.base(), s.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(waiter);
    sched.run(1);
    // The waiter Fiber is parked inside cond.wait (active_waits_ == 1).
    delete cond;  // MUST terminate; never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// CTL — control: a full lock/wait/notify/reacquire/unlock cycle, then
// quiescent destruction of BOTH objects (stack order: condition first).
// Proves the ADR changed only the violation path: quiescent destruction
// still succeeds with no side effects.
void child_ctl_valid_usage() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    {
        AsyncMutex mtx(sched);
        AsyncCondition cond(mtx);

        std::atomic<bool> waiter_suspended{false}, waiter_done{false};
        WaitNode mlock, cn;

        Fiber owner;
        owner.set_entry([&](Fiber&) {
            mtx.lock(mlock);
            waiter_suspended.store(true, std::memory_order_release);
            (void)cond.wait(cn);  // releases mtx, suspends
            mtx.unlock();
            waiter_done.store(true, std::memory_order_release);
        });
        Fiber notifier;
        notifier.set_entry([&](Fiber&) {
            sched.await_ready_flag(waiter_suspended);
            cond.notify_one();
        });
        FiberStack sa, sb;
        if (!sched.init_fiber(owner, sa.base(), sa.size()) ||
            !sched.init_fiber(notifier, sb.base(), sb.size())) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        sched.spawn(owner);
        sched.spawn(notifier);
        sched.run(1);
        if (!waiter_done.load(std::memory_order_acquire)) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (sched.waiting_count() != 0) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
    }  // ~AsyncCondition, then ~AsyncMutex: quiescent, must not terminate
    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    // The fail-fast boundaries use std::terminate (Release) and assert
    // (Debug) ahead of the same boundary. std::abort/assert raise SIGABRT;
    // the terminate handler does not catch signals. Convert SIGABRT to the
    // deterministic terminate exit code so the parent can assert it exactly
    // (mirrors async_rwlock_death_test).
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
    if      (name == "M1") child_m1_destroy_mutex_while_owned();
    else if (name == "C1") child_c1_destroy_condition_with_wait_in_flight();
    else if (name == "CTL") child_ctl_valid_usage();
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

    // ADR-async-primitive-lifetime-failfast: BOTH Debug and Release.
    must_term("M1");  // AsyncMutex destroyed while owned
    must_term("C1");  // AsyncCondition destroyed with wait() in flight
    must_zero("CTL");  // quiescent destruction after a full wait cycle

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

int main() {
    std::cout << "ALL DEATH TESTS PASSED (non-POSIX: not run)\n";
    return 0;
}

#endif  // __unix__
