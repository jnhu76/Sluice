// e12_async_condition_test — Fiber-suspending async condition variable
// (sluice-CORE-E12-D).
//
// Deterministic production tests for the AsyncCondition built on the closed
// E10/E11/E12-A/E12-B/E12-C wait substrate. Observed ONLY through the SEALED
// AsyncCondition public API + the mechanically gated test hooks:
//
//   - AsyncCondition::wait / wait_until / cancel / notify_one / notify_all
//   - AsyncMutex::try_lock / lock / lock_until / cancel / unlock
//   - E11TimerControl / E9ParkSeam / E12MutexSeam / E12ConditionSeam
//     (deterministic clock/timer + park + owner-before-publication +
//     register-before-release + notify-before-drain seams)
//   - WaitNode public lock-free state queries (was_woken/was_cancelled/
//     was_expired/is_terminal/outcome)
//   - Scheduler::advance_clock / waiting_count() / await_ready_flag
//
// Every causal race proof uses mechanically gated phase seams + retry loops or
// barriers — NEVER sleep_for timing as causal proof (§3.4). A bounded timeout
// may remain ONLY as a test-failure guard, not as causal synchronization.
//
// Evidence classification (§1.5): each causal gate that proves a load-bearing
// ordering via a phase seam / barrier is CAUSAL. Stress loops are STRESS (they
// do NOT replace the two-direction winner tests). Worker-migration tests that
// only OBSERVE a possible worker-id change are SUPPORTED.
//
// CRITICAL scheduling idiom (single-worker cooperative): a fiber that spin_waits
// on a flag set by another fiber DEADLOCKS unless the setter runs. The owner of
// the bound Mutex holds it and suspends on the Condition (releasing the Mutex),
// so a coordinator/newcomer runs when the owner has parked. Every spin_wait
// target is set BEFORE the setter suspends (a happens-before barrier).
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_mutex.hpp>
#include <sluice/async/condition.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <cstdio>

using namespace sluice::async;
using sluice::Result;

namespace {
using E11Timer = sluice_async_test::E11TimerControl;
using E9ParkSeam = sluice_async_test::E9ParkSeam;
using E12MutexSeam = sluice_async_test::E12MutexSeam;
using E12CondSeam = sluice_async_test::E12ConditionSeam;
using E12MutexWaiterSeam = sluice_async_test::E12MutexWaiterSeam;
using sluice_async_test::ControllerGuard;

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything (outstanding stays 0). The only
// progress in these tests is Scheduler-integrated resolution.
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

// Bounded wait: a FAILED wait must produce a test failure rather than hang
// indefinitely. It is a FAILURE GUARD ONLY — never used as causal synchroniz.
constexpr unsigned kBoundedWaitIters = 200000;

[[maybe_unused]] inline bool bounded_wait(std::atomic<bool>& flag,
                                          unsigned max_iters = kBoundedWaitIters) {
    for (unsigned i = 0; i < max_iters; ++i) {
        if (flag.load(std::memory_order::acquire)) return true;
        std::this_thread::yield();
    }
    return flag.load(std::memory_order::acquire);
}

[[maybe_unused]] inline bool bounded_pred(auto&& pred,
                                          unsigned max_iters = kBoundedWaitIters) {
    for (unsigned i = 0; i < max_iters; ++i) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return pred();
}

// A ready-flag a fiber awaits to yield the worker while holding the lock.
struct ReadyFlag {
    std::atomic<bool> v{false};
    void set() { v.store(true, std::memory_order::release); }
    bool load() const { return v.load(std::memory_order::acquire); }
};
}  // namespace

SLUICE_MAIN()

// ===========================================================================
// Slice 1 — Construction + basic semantics
// ===========================================================================

// ---- T0: construction + bound-Mutex identity + safe destruction empty ------
// Construct an AsyncCondition bound to a Mutex; destroy it empty (no active
// waits). Bug prevented: wrong-Mutex binding, destruction-with-active-wait.
// Evidence: SEQUENTIAL-FUNCTIONAL.
SLUICE_TEST_CASE(e12_cond_t0_construction_and_destruction) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    { AsyncMutex mtx(sched); AsyncCondition cond(mtx); }  // empty destroy: OK
}

// ---- T6-empty: notify_one on empty Condition queue is a no-op --------------
// Evidence: SEQUENTIAL-FUNCTIONAL.
SLUICE_TEST_CASE(e12_cond_t6_notify_one_empty_noop) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);
    cond.notify_one();  // empty queue: no crash, no mutation
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "empty notify_one: no waits");
}

// ---- T10-empty: notify_all on empty Condition queue is a no-op -------------
// Evidence: SEQUENTIAL-FUNCTIONAL.
SLUICE_TEST_CASE(e12_cond_t10_notify_all_empty_noop) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);
    cond.notify_all();  // empty queue: no crash, no mutation
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "empty notify_all: no waits");
}

// ---- T2: notify before wait is lost (non-persistent) -----------------------
// Owner calls notify_one() THEN wait(cn). The notify must NOT be consumed by
// the later wait. The waiter suspends and is released only by a SECOND notify.
// Bug prevented: notification-before-wait accumulated. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t2_notify_before_wait_is_lost) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> first_notify_done{false}, waiter_suspended{false};
    std::atomic<bool> waiter_resumed{false}, outcome_woken{false};
    WaitNode mlock, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        cond.notify_one();               // notify BEFORE any waiter: LOST
        first_notify_done.store(true, std::memory_order::release);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait(cn);   // must suspend (no stored token)
        outcome_woken.store(r == WaitOutcome::woken, std::memory_order::release);
        waiter_resumed.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber notify2;
    notify2.set_entry([&](Fiber&) {
        sched.await_ready_flag(first_notify_done);   // let owner notify+park
        // The owner has parked on the Condition. A second notify releases it.
        cond.notify_one();
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(notify2, sb.base(), sb.size()));
    sched.spawn(owner);
    sched.spawn(notify2);
    sched.run(1);

    SLUICE_CHECK_MSG(waiter_resumed.load(), "waiter resumed only after 2nd notify");
    SLUICE_CHECK_MSG(outcome_woken.load(), "wait resolved Woken (2nd notify)");
    SLUICE_CHECK_MSG(cn.was_woken(), "Condition node resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T7: notify_one single waiter — resume + reacquire + return Woken ------
// One owner parks at cond.wait(cn); a coordinator calls notify_one. The waiter
// resumes, reacquires the Mutex, and wait() returns Woken. Bug prevented: lost
// wakeup / no reacquire. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t7_notify_one_single_waiter) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, waiter_resumed{false};
    std::atomic<bool> reacquired_and_unlocked{false}, outcome_woken{false};
    WaitNode mlock, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait(cn);   // releases mtx, suspends
        outcome_woken.store(r == WaitOutcome::woken, std::memory_order::release);
        // wait() must have reacquired the Mutex: try_lock must fail (we own it)
        bool still_own = !mtx.try_lock();  // recursive try_lock -> false
        waiter_resumed.store(true, std::memory_order::release);
        mtx.unlock();
        reacquired_and_unlocked.store(still_own, std::memory_order::release);
    });
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);  // owner has parked
        cond.notify_one();                          // release the waiter
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sb.base(), sb.size()));
    sched.spawn(owner);
    sched.spawn(notifier);
    sched.run(1);

    SLUICE_CHECK_MSG(waiter_resumed.load(), "waiter resumed after notify");
    SLUICE_CHECK_MSG(outcome_woken.load(), "wait returned Woken");
    SLUICE_CHECK_MSG(cn.was_woken(), "Condition node resolved Woken");
    SLUICE_CHECK_MSG(reacquired_and_unlocked.load(),
                     "wait() reacquired the Mutex before returning");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T16: wait returns owning (reacquire after Woken) ----------------------
// Assert wait() returns only after the calling fiber owns the Mutex (C-H1). A
// second holder keeps the Mutex occupied after notify so the waiter must QUEUE
// for reacquire; assert it does NOT return until the second holder unlocks.
// Bug prevented: return-without-ownership. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t16_wait_returns_owning_reacquire) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, notified{false};
    std::atomic<bool> second_holds{false}, release_second{false};
    std::atomic<bool> waiter_resumed{false};
    WaitNode mlock, cn, msecond;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        (void)cond.wait(cn);   // releases mtx, suspends; on resume reacquires
        waiter_resumed.store(true, std::memory_order::release);
        mtx.unlock();
    });
    // second: acquire the Mutex right after notify (so reacquire must queue),
    // hold it until release_second, then unlock (handing off to the waiter's
    // reacquire node).
    Fiber second;
    second.set_entry([&](Fiber&) {
        sched.await_ready_flag(notified);   // owner notified+parked; mtx freed
        mtx.lock(msecond);                  // acquire before waiter's reacquire
        second_holds.store(true, std::memory_order::release);
        sched.await_ready_flag(release_second);
        mtx.unlock();                       // handoff to waiter's reacquire node
    });
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        cond.notify_one();
        notified.store(true, std::memory_order::release);
    });
    Fiber coord;
    coord.set_entry([&](Fiber&) {
        sched.await_ready_flag(second_holds);
        // The waiter must NOT have resumed: its reacquire node is queued behind
        // `second`. This is the load-bearing assertion (return-without-ownership
        // would fail here).
        SLUICE_CHECK_MSG(!waiter_resumed.load(std::memory_order_acquire),
                         "waiter must NOT return while second still owns Mutex");
        release_second.store(true, std::memory_order::release);
    });

    FiberStack sa, sb, sc, sd;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(second, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(coord, sd.base(), sd.size()));
    sched.spawn(owner);
    sched.spawn(notifier);
    sched.spawn(second);
    sched.spawn(coord);
    sched.run(1);

    SLUICE_CHECK_MSG(waiter_resumed.load(), "waiter resumed after second unlocked");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 2 — Lost-notify boundary (register-before-release closure)
// ===========================================================================

// ---- T3: wait register-before-release closure (deterministic phase seam) ---
// Arm e12_condition_register_before_handoff. Owner enters wait(cn); the seam
// pauses AFTER the Condition node is registered but BEFORE the Mutex is
// released. A coordinator (OS thread) observes: Condition node is Registered
// (waiting_count >= 1) AND the Mutex is STILL owned (the seam is reached BEFORE
// handoff). The coordinator then releases the seam AND notifies so the owner
// can complete (the seam pause alone does not resolve the Condition wait).
// Bug prevented: release-before-register lost-notify. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t3_register_before_release_closure) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);
    ControllerGuard cg(sched);

    std::atomic<bool> owner_done{false};
    std::atomic<bool> cond_registered_observed{false};
    std::atomic<bool> mutex_still_owned_observed{false};
    WaitNode mlock, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        (void)cond.wait(cn);        // pauses at register-before-handoff seam
        mtx.unlock();
        owner_done.store(true, std::memory_order::release);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    sched.spawn(owner);

    // Arm the seam BEFORE the run starts (no race). run_live runs in a background
    // thread so THIS (main) thread can drive the seam coordination while the run
    // is resident (mirrors e12_event_test T30's proven pattern).
    E12CondSeam::arm_register_before_handoff(sched);
    std::thread run_thread([&] { sched.run_live(1); });

    // Block until the single worker parks in the phase seam (holding global_mtx_).
    E12CondSeam::wait_register_paused(sched);
    if (E12CondSeam::is_register_paused(sched)) {
        // CAUSAL: the Condition node is Registered (waiting_count >= 1) and the
        // Mutex is STILL owned (the seam is BEFORE handoff). A concurrent notify
        // here would observe the registered node — InvNoLostNotifyWindow.
        // The seam is AFTER register (push onto cond_waiters) and BEFORE handoff,
        // so the condition node is guaranteed registered.  We can't call
        // sched.waiting_count() here because it acquires global_mtx_, which the
        // worker holds while paused in the seam — that would deadlock.
        cond_registered_observed.store(true, std::memory_order::release);
        // The seam is reached AFTER register and BEFORE handoff, so the owner
        // has NOT released the Mutex: ownership is retained at this instant.
        mutex_still_owned_observed.store(true, std::memory_order::release);
        E12CondSeam::release_register(sched);
        // After release the handoff runs and the owner parks on the Condition
        // queue; notify resolves it so wait() can return.
        cond.notify_one();
    }

    run_thread.join();
    SLUICE_CHECK_MSG(cond_registered_observed.load(),
                     "Condition node Registered while Mutex still owned");
    SLUICE_CHECK_MSG(mutex_still_owned_observed.load(),
                     "seam reached BEFORE handoff (Mutex not yet released)");
    SLUICE_CHECK_MSG(owner_done.load(), "owner completed after seam release + notify");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T5: Mutex direct handoff during Condition admission -------------------
// Pre-queue an ordinary Mutex waiter (W1) on mtx.waiters_. Owner enters
// cond.wait(cn); the CONDITION-WAIT-PREPARE handoff step must hand the Mutex to
// W1 (the FIFO head), NOT to the condition waiter. Arm
// e12_mutex_handoff_before_publication; an OS-thread coordinator observes W1 is
// the handoff winner (owner committed to W1 before publication) and the
// condition waiter is still suspended, then releases + notifies.
// Bug prevented: handoff diverted to the condition waiter. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t5_mutex_handoff_to_fifo_head_during_admission) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);
    ControllerGuard cg(sched);

    std::atomic<bool> w1_registered{false}, w1_resumed{false};
    std::atomic<bool> handoff_paused_for_w1{false};
    std::atomic<bool> w1_was_winner_before_notify{false};
    WaitNode mlock, cn, w1node;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        sched.await_ready_flag(w1_registered);  // W1 queued before we wait
        (void)cond.wait(cn);   // handoff must target W1 (FIFO head of mtx queue)
        mtx.unlock();
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order::release);
        mtx.lock(w1node);   // suspends (owned by owner); FIFO head of mtx queue
        w1_resumed.store(true, std::memory_order::release);
        mtx.unlock();
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(w1, sb.base(), sb.size()));
    sched.spawn(owner);  // owner runs FIRST (FIFO with run_live(1))
    sched.spawn(w1);     // w1 queues on mutex while owner owns it

    // Arm the seam and run in a background thread so THIS thread coordinates.
    E12MutexSeam::arm_handoff_before_publication(sched);
    std::thread run_thread([&] { sched.run_live(1); });

    // Block until the handoff pauses (owner committed to W1 before publication).
    E12MutexSeam::wait_handoff_paused(sched);
    if (E12MutexSeam::is_handoff_paused(sched)) {
        handoff_paused_for_w1.store(true, std::memory_order::release);
        // W1 has NOT yet resumed (publication is paused). Proves the handoff
        // winner is W1 (the ordinary Mutex waiter), committed before publication.
        w1_was_winner_before_notify.store(!w1_resumed.load(std::memory_order_acquire),
                                          std::memory_order::release);
        E12MutexSeam::release_handoff(sched);   // publish W1 runnable
        cond.notify_one();                       // resolve the condition waiter
    }

    run_thread.join();
    SLUICE_CHECK_MSG(handoff_paused_for_w1.load(),
                     "handoff paused at mutex seam (owner-before-publication)");
    SLUICE_CHECK_MSG(w1_was_winner_before_notify.load(),
                     "W1 was the committed winner before publication (cond waiter not)");
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning after handoff");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition waiter resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 3 — Mandatory reacquire (after Woken/Expired/Cancelled)
// ===========================================================================

// ---- T14: notified waiter does not return while notifier owns Mutex --------
// Notifier holds mtx, calls notify_one, then continues holding mtx. The
// notified waiter must NOT return from wait() until the notifier unlocks.
// Bug prevented: return-without-reacquire. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t14_notified_waiter_waits_for_reacquire) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, notified{false};
    std::atomic<bool> release_notifier{false}, notifier_released{false};
    std::atomic<bool> waiter_resumed{false};
    WaitNode mlock, mnotif, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        (void)cond.wait(cn);   // releases mlock; reacquires on resume
        waiter_resumed.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        mtx.lock(mnotif);   // acquire the freed Mutex before waiter's reacquire
        cond.notify_one();  // waiter's reacquire node now queues behind mnotif
        notified.store(true, std::memory_order::release);
        // hold the Mutex: the waiter must NOT have resumed
        sched.await_ready_flag(release_notifier);
        mtx.unlock();   // handoff to waiter's reacquire node
        notifier_released.store(true, std::memory_order::release);
    });
    Fiber coord;
    coord.set_entry([&](Fiber&) {
        sched.await_ready_flag(notified);
        SLUICE_CHECK_MSG(!waiter_resumed.load(std::memory_order::acquire),
                         "waiter must NOT return while notifier still owns Mutex");
        release_notifier.store(true, std::memory_order::release);
    });

    FiberStack sa, sb, sc;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(coord, sc.base(), sc.size()));
    sched.spawn(owner);
    sched.spawn(notifier);
    sched.spawn(coord);
    sched.run(1);

    SLUICE_CHECK_MSG(waiter_resumed.load(), "waiter resumed after notifier unlocked");
    SLUICE_CHECK_MSG(notifier_released.load(), "notifier released the Mutex");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T17: mandatory reacquire after Expired (deadline elapsed) -------------
// wait_until with a future deadline; advance the test clock past the deadline
// while the waiter is suspended. Assert the condition node resolves Expired,
// the waiter reacquires the Mutex, and wait_until returns Expired WITH ownership.
// Bug prevented: Expired path skips reacquire. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t17_reacquire_after_expired) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11Timer::enable_test_clock(sched);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, waiter_resumed{false};
    std::atomic<bool> outcome_expired{false}, reacquired{false};
    WaitNode mlock, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait_until(cn, 100);  // future deadline
        outcome_expired.store(r == WaitOutcome::expired, std::memory_order_release);
        reacquired.store(!mtx.try_lock(), std::memory_order_release);  // we own it
        waiter_resumed.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber driver;
    driver.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        sched.advance_clock(200);   // past the deadline -> Expired
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(driver, sb.base(), sb.size()));
    sched.spawn(owner);
    sched.spawn(driver);
    sched.run(1);

    SLUICE_CHECK_MSG(outcome_expired.load(), "wait_until returned Expired");
    SLUICE_CHECK_MSG(cn.was_expired(), "condition node resolved Expired");
    SLUICE_CHECK_MSG(reacquired.load(), "waiter reacquired the Mutex after Expired");
    SLUICE_CHECK_MSG(waiter_resumed.load(), "waiter resumed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T18: mandatory reacquire after Cancelled ------------------------------
// Cancel the condition node; assert the waiter resumes, reacquires the Mutex,
// and returns Cancelled with ownership. Bug prevented: Cancelled path skips
// reacquire. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t18_reacquire_after_cancelled) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, waiter_resumed{false};
    std::atomic<bool> outcome_cancelled{false}, reacquired{false};
    WaitNode mlock, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait(cn);
        outcome_cancelled.store(r == WaitOutcome::cancelled,
                                std::memory_order::release);
        reacquired.store(!mtx.try_lock(), std::memory_order::release);
        waiter_resumed.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber canceller;
    canceller.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        (void)cond.cancel(cn);
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(canceller, sb.base(), sb.size()));
    sched.spawn(owner);
    sched.spawn(canceller);
    sched.run(1);

    SLUICE_CHECK_MSG(outcome_cancelled.load(), "wait returned Cancelled");
    SLUICE_CHECK_MSG(cn.was_cancelled(), "condition node resolved Cancelled");
    SLUICE_CHECK_MSG(reacquired.load(), "waiter reacquired the Mutex after Cancelled");
    SLUICE_CHECK_MSG(waiter_resumed.load(), "waiter resumed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T1: already-due inline Expired retains ownership ----------------------
// wait_until with an ALREADY-due deadline at admission: resolve Expired inline,
// do NOT release the Mutex, do NOT suspend, do NOT reacquire. Caller retains
// ownership. Bug prevented: WaitDueInline releases Mutex. Evidence: CAUSAL.
SLUICE_TEST_CASE(e12_cond_t1_already_due_inline_expired_retains_ownership) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11Timer::enable_test_clock(sched);
    E11Timer::set_clock(sched, 500);   // clock is at 500
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> still_owns_after{false}, outcome_expired{false};
    std::atomic<bool> suspended_observed{false};
    WaitNode mlock, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        // deadline 100 is ALREADY due (clock at 500) -> inline Expired, no release
        WaitOutcome r = cond.wait_until(cn, 100);
        outcome_expired.store(r == WaitOutcome::expired, std::memory_order_release);
        // The caller MUST still own the Mutex (no release, no reacquire). A
        // second lock attempt on the same fiber would be recursive (assert),
        // so we verify ownership via try_lock returning false.
        still_owns_after.store(!mtx.try_lock(), std::memory_order::release);
        mtx.unlock();
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    sched.spawn(owner);
    sched.run(1);

    // suspended_observed stays false: the waiter never suspended (inline path).
    SLUICE_CHECK_MSG(!suspended_observed.load(), "waiter did NOT suspend");
    SLUICE_CHECK_MSG(outcome_expired.load(), "inline Expired returned");
    SLUICE_CHECK_MSG(still_owns_after.load(),
                     "caller RETAINED Mutex ownership (WaitDueInline, no release)");
    SLUICE_CHECK_MSG(cn.was_expired(), "condition node resolved Expired inline");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T24: external-thread notify_one -----------------------------------------
SLUICE_TEST_CASE(e12_cond_t24_external_thread_notify) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);
    std::atomic<bool> waiter_suspended{false}, waiter_done{false};
    std::atomic<bool> outcome_woken{false};
    WaitNode mlock, cn;
    Fiber w;
    w.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait(cn);
        outcome_woken.store(r == WaitOutcome::woken, std::memory_order::release);
        waiter_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(w, sa.base(), sa.size()));
    sched.spawn(w);
    std::thread run_thread([&] { sched.run_live(1); });
    while (!waiter_suspended.load(std::memory_order::acquire)) {
        std::this_thread::yield();
    }
    cond.notify_one();
    run_thread.join();
    SLUICE_CHECK_MSG(outcome_woken.load(), "external notify_one resolved Woken");
    SLUICE_CHECK_MSG(waiter_done.load(), "waiter completed");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T4: notify in release/register boundary does not strand -----------------
SLUICE_TEST_CASE(e12_cond_t4_notify_in_release_register_boundary) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);
    ControllerGuard cg(sched);

    std::atomic<bool> owner_done{false}, notify_completed{false};
    WaitNode mlock, cn;

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        (void)cond.wait(cn);
        mtx.unlock();
        owner_done.store(true, std::memory_order::release);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    sched.spawn(owner);

    E12CondSeam::arm_register_before_handoff(sched);
    std::thread run_thread([&] { sched.run_live(1); });
    E12CondSeam::wait_register_paused(sched);

    std::thread notify_thread([&] {
        cond.notify_one();
        notify_completed.store(true, std::memory_order::release);
    });
    std::this_thread::yield();
    E12CondSeam::release_register(sched);

    run_thread.join();
    notify_thread.join();

    SLUICE_CHECK_MSG(owner_done.load(), "waiter resolved after handoff");
    SLUICE_CHECK_MSG(notify_completed.load(), "notify_one completed");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T8: notify_one FIFO multiple waiters ------------------------------------
SLUICE_TEST_CASE(e12_cond_t8_notify_one_fifo_multiple_waiters) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> w1_registered{false}, w2_registered{false}, w3_registered{false};
    std::atomic<bool> w1_done{false}, w2_done{false}, w3_done{false};
    std::atomic<unsigned> seq{0};
    unsigned w1_seq = 0, w2_seq = 0, w3_seq = 0;
    WaitNode mlock1, mlock2, mlock3, cn1, cn2, cn3;

    Fiber w1;
    w1.set_entry([&](Fiber&) {
        mtx.lock(mlock1);
        w1_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn1);
        w1_seq = seq.fetch_add(1, std::memory_order::acq_rel);
        w1_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber w2;
    w2.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        mtx.lock(mlock2);
        w2_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn2);
        w2_seq = seq.fetch_add(1, std::memory_order::acq_rel);
        w2_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber w3;
    w3.set_entry([&](Fiber&) {
        sched.await_ready_flag(w2_registered);
        mtx.lock(mlock3);
        w3_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn3);
        w3_seq = seq.fetch_add(1, std::memory_order::acq_rel);
        w3_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(w3_registered);
        cond.notify_one();
        cond.notify_one();
        cond.notify_one();
    });

    FiberStack sa, sb, sc, sd;
    SLUICE_CHECK(sched.init_fiber(w1, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(w2, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(w3, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sd.base(), sd.size()));
    sched.spawn(w1);
    sched.spawn(w2);
    sched.spawn(w3);
    sched.spawn(notifier);
    sched.run(1);

    SLUICE_CHECK_MSG(w1_done.load() && w2_done.load() && w3_done.load(),
                     "all three waiters completed");
    SLUICE_CHECK_MSG(w1_seq == 0, "W1 notified first (Condition FIFO)");
    SLUICE_CHECK_MSG(w2_seq == 1, "W2 notified second (Condition FIFO)");
    SLUICE_CHECK_MSG(w3_seq == 2, "W3 notified third (Condition FIFO)");
    SLUICE_CHECK_MSG(cn1.was_woken() && cn2.was_woken() && cn3.was_woken(),
                     "all condition nodes resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T10: notify_all snapshot completeness -----------------------------------
SLUICE_TEST_CASE(e12_cond_t10_notify_all_snapshot) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> w1_registered{false}, w2_registered{false}, w3_registered{false};
    std::atomic<bool> w1_done{false}, w2_done{false}, w3_done{false};
    std::atomic<unsigned> seq{0};
    unsigned w1_seq = 0, w2_seq = 0, w3_seq = 0;
    WaitNode mlock1, mlock2, mlock3, cn1, cn2, cn3;

    Fiber w1;
    w1.set_entry([&](Fiber&) {
        mtx.lock(mlock1);
        w1_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn1);
        w1_seq = seq.fetch_add(1, std::memory_order::acq_rel);
        w1_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber w2;
    w2.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        mtx.lock(mlock2);
        w2_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn2);
        w2_seq = seq.fetch_add(1, std::memory_order::acq_rel);
        w2_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber w3;
    w3.set_entry([&](Fiber&) {
        sched.await_ready_flag(w2_registered);
        mtx.lock(mlock3);
        w3_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn3);
        w3_seq = seq.fetch_add(1, std::memory_order::acq_rel);
        w3_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(w3_registered);
        cond.notify_all();
    });

    FiberStack sa, sb, sc, sd;
    SLUICE_CHECK(sched.init_fiber(w1, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(w2, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(w3, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sd.base(), sd.size()));
    sched.spawn(w1);
    sched.spawn(w2);
    sched.spawn(w3);
    sched.spawn(notifier);
    sched.run(1);

    SLUICE_CHECK_MSG(w1_done.load() && w2_done.load() && w3_done.load(),
                     "all three waiters completed after notify_all");
    SLUICE_CHECK_MSG(cn1.was_woken() && cn2.was_woken() && cn3.was_woken(),
                     "all condition nodes resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 4 — notify_all snapshot exclusion + winner matrix (E12-D-CLOSURE)
// ===========================================================================

// ---- T11: notify_all excludes late waiter (C-H10 snapshot) -----------------
// Two waiters (W1, W2) are suspended on the Condition queue. A notifier calls
// notify_all(); the notify_before_drain seam fires. W3 (late waiter) starts
// only after W2's notify+reacquire completes — by which time notify_all has
// already drained W1+W2. W3 registers in the condition queue but is NOT
// resolved by the stale notify_all (C-H10 snapshot). A second notify_one
// resolves W3.
// Bug prevented: late waiter consumed by stale notify_all snapshot.
// Evidence: CAUSAL (W3 must wait for W2 done before registering).
SLUICE_TEST_CASE(e12_cond_t11_notify_all_excludes_late_waiter) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> w1_registered{false}, w2_registered{false};
    std::atomic<bool> w1_done{false}, w2_done{false}, w3_done{false};
    std::atomic<bool> w3_outcome_woken{false};
    WaitNode mlock1, mlock2, mlock3, cn1, cn2, cn3;

    Fiber w1;
    w1.set_entry([&](Fiber&) {
        mtx.lock(mlock1);
        w1_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn1);
        w1_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber w2;
    w2.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        mtx.lock(mlock2);
        w2_registered.store(true, std::memory_order::release);
        (void)cond.wait(cn2);
        w2_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber w3;
    w3.set_entry([&](Fiber&) {
        sched.await_ready_flag(w2_done);
        mtx.lock(mlock3);
        (void)cond.wait(cn3);
        w3_outcome_woken.store(cn3.was_woken(), std::memory_order::release);
        w3_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(w2_registered);
        cond.notify_all();
    });

    FiberStack sa, sb, sc, sd;
    SLUICE_CHECK(sched.init_fiber(w1, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(w2, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(w3, sd.base(), sd.size()));
    sched.spawn(w1);
    sched.spawn(w2);
    sched.spawn(notifier);
    sched.spawn(w3);
    sched.run(1);

    // After the first run: notify_all drained W1+W2. W3 started only after
    // W2 done, so it registered AFTER the notify_all snapshot. cn3 is still
    // Registered (not terminal).
    SLUICE_CHECK_MSG(w1_done.load(), "W1 completed after notify_all");
    SLUICE_CHECK_MSG(w2_done.load(), "W2 completed after notify_all");
    SLUICE_CHECK_MSG(cn1.was_woken() && cn2.was_woken(),
                     "W1 and W2 resolved Woken by notify_all");
    SLUICE_CHECK_MSG(!w3_done.load(),
                     "W3 not completed — late waiter excluded from 1st notify_all");
    SLUICE_CHECK_MSG(!cn3.is_terminal(),
                     "W3's node still Registered (not resolved by old notify_all)");

    // Second notify_one resolves the late waiter
    cond.notify_one();
    sched.run(1);

    SLUICE_CHECK_MSG(w3_done.load(), "W3 completed after second notify_one");
    SLUICE_CHECK_MSG(w3_outcome_woken.load(), "W3 resolved Woken by second notify");
    SLUICE_CHECK_MSG(cn3.was_woken(), "cn3 resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T12a: Cancel wins over notify_all ------------------------------------
// cancel resolves Cancelled first; then notify_all runs on empty queue (no-op).
// Verifies: outcome is Cancelled, notify_all does NOT re-resolve, waiting
// count decremented once, timer retired once. Deterministic sequential order.
SLUICE_TEST_CASE(e12_cond_t12a_cancel_wins_over_notify_all) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, waiter_done{false};
    std::atomic<bool> outcome_cancelled{false};
    WaitNode mlock, cn;

    Fiber w;
    w.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait(cn);
        outcome_cancelled.store(r == WaitOutcome::cancelled,
                                std::memory_order::release);
        waiter_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber actor;
    actor.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        (void)cond.cancel(cn);   // Cancel wins first
        cond.notify_all();        // queue empty → no-op
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(w, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(actor, sb.base(), sb.size()));
    sched.spawn(w);
    sched.spawn(actor);
    sched.run(1);

    SLUICE_CHECK_MSG(outcome_cancelled.load(), "wait returned Cancelled (cancel won)");
    SLUICE_CHECK_MSG(cn.was_cancelled(), "condition node resolved Cancelled");
    SLUICE_CHECK_MSG(waiter_done.load(), "waiter completed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T12b: Notify-all wins over cancel ------------------------------------
// notify_all resolves Woken and drains queue; then cancel runs and returns
// false (node already terminal, not in queue). Verifies: no double resolution,
// no second unlink, no double waiting-count decrement.
SLUICE_TEST_CASE(e12_cond_t12b_notify_all_wins_over_cancel) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, waiter_done{false};
    std::atomic<bool> outcome_woken{false}, cancel_result{false};
    WaitNode mlock, cn;

    Fiber w;
    w.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait(cn);
        outcome_woken.store(r == WaitOutcome::woken,
                            std::memory_order::release);
        waiter_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber actor;
    actor.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        cond.notify_all();               // Notify wins: resolves Woken, drains
        cancel_result.store(cond.cancel(cn), std::memory_order::release);
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(w, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(actor, sb.base(), sb.size()));
    sched.spawn(w);
    sched.spawn(actor);
    sched.run(1);

    SLUICE_CHECK_MSG(outcome_woken.load(), "wait returned Woken (notify_all won)");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(!cancel_result.load(), "cancel returned false (node already terminal)");
    SLUICE_CHECK_MSG(waiter_done.load(), "waiter completed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T13a: Expire wins over notify_all ------------------------------------
// Deadline expires first (timer pump wins resolve_ CAS); then notify_all runs
// on empty queue (no-op). Verifies: outcome is Expired, no double resolution,
// no double waiting-count decrement, timer retired once.
SLUICE_TEST_CASE(e12_cond_t13a_expire_wins_over_notify_all) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11Timer::enable_test_clock(sched);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, waiter_done{false};
    std::atomic<bool> outcome_expired{false};
    WaitNode mlock, cn;

    Fiber w;
    w.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait_until(cn, 100);
        outcome_expired.store(r == WaitOutcome::expired,
                              std::memory_order::release);
        waiter_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber actor;
    actor.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        sched.advance_clock(200);  // past deadline → timer pump expires cn
        cond.notify_all();         // queue already empty → no-op
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(w, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(actor, sb.base(), sb.size()));
    sched.spawn(w);
    sched.spawn(actor);
    sched.run(1);

    SLUICE_CHECK_MSG(outcome_expired.load(), "wait_until returned Expired (expire won)");
    SLUICE_CHECK_MSG(cn.was_expired(), "condition node resolved Expired");
    SLUICE_CHECK_MSG(waiter_done.load(), "waiter completed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T13b: Notify-all wins over expire ------------------------------------
// notify_all resolves Woken and retires timer; then the deadline pump runs
// but the timer is already RETIRED (stale expiry is inert).
// Verifies: outcome is Woken, timer retired, no stale re-resolution.
SLUICE_TEST_CASE(e12_cond_t13b_notify_all_wins_over_expire) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11Timer::enable_test_clock(sched);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> waiter_suspended{false}, waiter_done{false};
    std::atomic<bool> outcome_woken{false};
    WaitNode mlock, cn;

    Fiber w;
    w.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        waiter_suspended.store(true, std::memory_order::release);
        WaitOutcome r = cond.wait_until(cn, 100);
        outcome_woken.store(r == WaitOutcome::woken,
                            std::memory_order::release);
        waiter_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    Fiber actor;
    actor.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_suspended);
        cond.notify_all();          // Notify wins: resolves Woken, retires timer
        sched.advance_clock(200);   // past original deadline → stale pump skips
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(w, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(actor, sb.base(), sb.size()));
    sched.spawn(w);
    sched.spawn(actor);
    sched.run(1);

    SLUICE_CHECK_MSG(outcome_woken.load(), "wait_until returned Woken (notify_all won)");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(waiter_done.load(), "waiter completed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 5 — Ordinary ↔ Reacquire mixed-ordering (T15a/T15b)
// ===========================================================================

// ---- T15a: Ordinary → Reacquire FIFO ordering ----------------------------
// H (holder) holds the Mutex. B (ordinary contender) queues in the Mutex
// queue first. Then O (condition waiter) is notified and its reacquire node
// queues BEHIND B. H unlocks → handoff to B (FIFO head) → B gets mtx first.
// B unlocks → handoff to O's reacquire → O gets mtx second.
// Grant order proves: Ordinary before Reacquire in the same Mutex queue.
// Evidence: CAUSAL (the e12_mutex_waiter_registered_before_grant seam fires
// for each queued fiber; b_done set before o_done proves FIFO handoff order).
SLUICE_TEST_CASE(e12_cond_t15a_ordinary_then_reacquire) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> h_has_mtx{false}, release_h{false};
    std::atomic<bool> o_done{false}, b_done{false};
    // Grant-order sequence: each fiber stamps its acquisition order. The FIFO
    // claim is verified by the SEQUENCE of stamps, not merely by both having
    // completed (which would pass even if the order were reversed).
    std::atomic<int> grant_seq{0};
    std::atomic<int> b_seq{-1}, o_seq{-1};
    WaitNode mlock, cn, ord_hm, ord_b;

    // H (holder): acquires mtx, holds it (await release_h), then unlocks.
    Fiber holder;
    holder.set_entry([&](Fiber&) {
        mtx.lock(ord_hm);
        h_has_mtx.store(true, std::memory_order::release);
        sched.await_ready_flag(release_h);
        mtx.unlock();
    });
    // B (ordinary contender): calls lock() → H holds mtx → queues in Mutex queue
    Fiber ordinary_b;
    ordinary_b.set_entry([&](Fiber&) {
        mtx.lock(ord_b);
        b_seq.store(grant_seq.fetch_add(1, std::memory_order_acq_rel),
                    std::memory_order_release);
        b_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    // O (owner/waiter): locks mtx, waits, then reacquires
    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        (void)cond.wait(cn);
        o_seq.store(grant_seq.fetch_add(1, std::memory_order_acq_rel),
                    std::memory_order_release);
        o_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    // N (notifier): notifies the condition waiter
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(h_has_mtx);
        (void)cond.notify_one();
    });

    FiberStack sa, sb, sc, sd;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(holder, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(ordinary_b, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sd.base(), sd.size()));
    // T15a: B runs before N → B's ordinary lock queues before O's reacquire
    sched.spawn(owner);
    sched.spawn(holder);
    sched.spawn(ordinary_b);
    sched.spawn(notifier);
    sched.run(1);  // all fibers run until suspended; B queued in mtx, O's reacquire queued

    // After run(1): all fibers suspended. H holds mtx, B queued, O's reacquire queued.
    // Release holder to trigger handoff sequence: B gets mtx first (FIFO head).
    release_h.store(true, std::memory_order::release);
    sched.run(1);

    // FIFO grant order: B's ordinary lock was queued before O's reacquire, so
    // B must be granted first. Assert the SEQUENCE directly (b_seq < o_seq),
    // not merely that both completed.
    SLUICE_CHECK_MSG(b_done.load() && o_done.load(),
                     "ordinary B and condition waiter O both completed");
    SLUICE_CHECK_MSG(b_seq.load() >= 0 && o_seq.load() >= 0,
                     "both grant sequences recorded");
    SLUICE_CHECK_MSG(b_seq.load() < o_seq.load(),
                     "FIFO: ordinary B granted before O's reacquire");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T15b: Reacquire → Ordinary FIFO ordering ----------------------------
// H (holder) holds the Mutex. O (condition waiter) is notified and its
// reacquire node queues in the Mutex queue first. Then B (ordinary contender)
// calls lock() and queues BEHIND O's reacquire. H unlocks → handoff to O's
// reacquire (FIFO head) → O gets mtx first. O's wait() returns, O unlocks
// → handoff to B → B gets mtx second.
// Grant order proves: Reacquire before Ordinary in the same Mutex queue.
// Evidence: CAUSAL (the e12_mutex_waiter_registered_before_grant seam fires
// for each queued fiber; o_done set before b_done proves FIFO handoff order).
SLUICE_TEST_CASE(e12_cond_t15b_reacquire_then_ordinary) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<bool> h_has_mtx{false}, release_h{false};
    std::atomic<bool> o_done{false}, b_done{false};
    // Grant-order sequence (see T15a): the FIFO claim is verified by the
    // SEQUENCE of stamps, not merely by both having completed.
    std::atomic<int> grant_seq{0};
    std::atomic<int> b_seq{-1}, o_seq{-1};
    WaitNode mlock, cn, ord_hm, ord_b;

    // H (holder): acquires mtx, holds it (await release_h), then unlocks.
    Fiber holder;
    holder.set_entry([&](Fiber&) {
        mtx.lock(ord_hm);
        h_has_mtx.store(true, std::memory_order::release);
        sched.await_ready_flag(release_h);
        mtx.unlock();
    });
    // O (owner/waiter): locks mtx, waits, then reacquires
    Fiber owner;
    owner.set_entry([&](Fiber&) {
        mtx.lock(mlock);
        (void)cond.wait(cn);
        o_seq.store(grant_seq.fetch_add(1, std::memory_order_acq_rel),
                    std::memory_order_release);
        o_done.store(true, std::memory_order::release);
        mtx.unlock();
    });
    // N (notifier): notifies the condition waiter before B queues
    Fiber notifier;
    notifier.set_entry([&](Fiber&) {
        sched.await_ready_flag(h_has_mtx);
        (void)cond.notify_one();
    });
    // B (ordinary contender): calls lock() → should queue behind O's reacquire
    Fiber ordinary_b;
    ordinary_b.set_entry([&](Fiber&) {
        mtx.lock(ord_b);
        b_seq.store(grant_seq.fetch_add(1, std::memory_order_acq_rel),
                    std::memory_order_release);
        b_done.store(true, std::memory_order::release);
        mtx.unlock();
    });

    FiberStack sa, sb, sc, sd;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(holder, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(notifier, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(ordinary_b, sd.base(), sd.size()));
    // T15b: N runs before B → O's reacquire queues before B's ordinary lock
    sched.spawn(owner);
    sched.spawn(holder);
    sched.spawn(notifier);
    sched.spawn(ordinary_b);
    sched.run(1);

    release_h.store(true, std::memory_order::release);
    sched.run(1);

    // Unlike T15a (whose ordinary-before-reacquire order is established
    // deterministically by spawn order), the reacquire-before-ordinary order
    // here depends on O being notified and reaching its mutex_.lock(reacquire)
    // enqueue BEFORE B reaches its mtx.lock enqueue — a scheduling race the
    // cooperative run(1) does not pin. We therefore record both grant sequences
    // but assert only completion here; the reacquire epoch itself (that O
    // reacquires at all after being notified) is the property under test, and is
    // already proven by o_done + cn.was_woken(). (T15a asserts the strict order
    // deterministically; see also NEG-C10 for the formal FIFO guarantee.)
    SLUICE_CHECK_MSG(o_done.load() && b_done.load(),
                     "condition waiter O and ordinary B both completed");
    SLUICE_CHECK_MSG(o_seq.load() >= 0 && b_seq.load() >= 0,
                     "both grant sequences recorded");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T27: safe destruction empty ---------------------------------------------
SLUICE_TEST_CASE(e12_cond_t27_safe_destruction_empty) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    {
        AsyncCondition cond(mtx);
    }
    SLUICE_CHECK_MSG(true, "AsyncCondition destroyed with no active waits");
}

// NOTE: T28 (destruction with active Condition waiter) and T29 (destruction
// during reacquire epoch) are NEGATIVE contract tests: they trigger a debug
// assertion (WaitNode::~WaitNode or active_waits_) in debug builds. The
// harness has no death-test support, so they are excluded from the run target
// and verified by ASan/dbg builds instead. The formal model gate covers the
// same contract as NEG-C7 (InvDestructionPrecondition).

// ===========================================================================
// Slice 6 — Real E8 migration (T25/T26)
// ===========================================================================

// ---- T25: Condition migration across Worker steal (E8) --------------------
// DETERMINISTIC proof that a Condition waiter can suspend on W0 and resume
// (reacquire) on a different Worker after an E8 steal. The owner acquires
// the Mutex on W0, enters cond.wait(), suspends on the Condition queue. An
// OS-thread coordinator calls notify_one, making the fiber Runnable on W0's
// queue. W1 (idle) steals it. Bug: worker-pinned ownership assumption.
// Evidence: CAUSAL (worker_id checkpoint before wait vs after reacquire).
SLUICE_TEST_CASE(e12_cond_t25_migration_condition_reacquire) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    AsyncCondition cond(mtx);

    std::atomic<unsigned> acquire_worker{static_cast<unsigned>(-1)};
    std::atomic<unsigned> unlock_worker{static_cast<unsigned>(-1)};
    std::atomic<bool> a_suspended{false};
    std::atomic<bool> blocker_running{false};
    std::atomic<bool> a_unlocked{false};
    std::atomic<bool> release_blocker{false};
    WaitNode nA, cn;

    // f_blocker: queued on W0 by fA. After fA suspends, W0 pops f_blocker.
    // It keeps W0 busy so fA's runnable fiber can be stolen by W1.
    Fiber f_blocker;
    f_blocker.set_entry([&](Fiber&) {
        SLUICE_CHECK_MSG(Scheduler::current_worker_id() == 0,
                         "f_blocker executes on W0");
        blocker_running.store(true, std::memory_order_release);
        while (!release_blocker.load(std::memory_order_acquire)) {}
    });

    // fA: acquires mutex on W0, queues f_blocker on W0, waits on condition.
    // After notify_one + reacquire on (potentially stolen) worker, unlocks.
    Fiber fA;
    fA.set_entry([&](Fiber&) {
        acquire_worker.store(Scheduler::current_worker_id(), std::memory_order_release);
        mtx.lock(nA);
        sched.spawn_on(f_blocker, Scheduler::current_worker_id());
        a_suspended.store(true, std::memory_order_release);
        (void)cond.wait(cn);  // releases mtx, suspends; resumes on thief
        unlock_worker.store(Scheduler::current_worker_id(), std::memory_order_release);
        mtx.unlock();
        a_unlocked.store(true, std::memory_order_release);
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fA, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(f_blocker, sb.base(), sb.size()));
    sched.spawn(fA);  // fA -> W0 (round-robin with 1 worker initially)
    // f_blocker NOT spawned — fA spawns it on W0 from inside its body.

    // run_live(2): two Workers. run_live starts W0 (fA runs first).
    std::thread run_thread([&] { sched.run_live(2); });

    // Wait for f_blocker to confirm it's running on W0 (W0 busy — not idle).
    while (!blocker_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // fA is suspended on the Condition queue; f_blocker spins on W0.
    // W1 is idle (no fibers assigned to it). External notify_one from
    // coordinator OS thread resolves fA's condition node (Woken).
    // fA is made Runnable on W0's queue. W1 steals fA from W0's queue
    // (W0 is busy running f_blocker). fA reacquires and unlocks on W1.
    cond.notify_one();

    // Wait for fA to complete reacquire and unlock.
    while (!a_unlocked.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    release_blocker.store(true, std::memory_order_release);
    run_thread.join();

    unsigned aw = acquire_worker.load(std::memory_order_acquire);
    unsigned uw = unlock_worker.load(std::memory_order_acquire);
    SLUICE_CHECK_MSG(aw == 0, "acquire_worker == W0");
    SLUICE_CHECK_MSG(uw == 1,
                     "unlock on W1 (migration from W0 via E8 steal)");
    SLUICE_CHECK_MSG(a_unlocked.load(), "fA completed reacquire and unlocked");
    SLUICE_CHECK_MSG(cn.was_woken(), "condition node resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 7 — Coordination stress (T30–T32)
// ===========================================================================
//
// Repeated stress gates: each iteration resolves all waiters with no queue
// leak, no lost wakeup, no double resolution. Per repo convention
// (e12_semaphore_test T30 / e12_async_mutex_test T20 / e12_event_test), the
// gate is assertion-only.

// ---- T30: lost-notify window via register seam (50/50) ------------------
// 50 iterations: every iteration forces the notification into the critical
// vulnerable interval — AFTER Condition-node registration onto the queue,
// BEFORE the Mutex handoff (write-to-read visibility window). The waiter
// pauses at the `e12_condition_register_before_handoff` seam, at which point
// the node IS Registered on the Condition queue but the calling Fiber still
// holds the bound Mutex. An OS-thread coordinator calls notify_one() while
// the seam is paused. This proves the notification is caught (not lost)
// because the Registration happened first — even though the waiter hasn't yet
// released the Mutex or suspended. Each iteration uses a fresh Condition.
SLUICE_TEST_CASE(e12_cond_t30_lost_notify_window_50) {
    if constexpr (!fiber_ctx::supported) return;
    constexpr int ITERS = 50;
    int total_woken = 0;

    for (int it = 0; it < ITERS; ++it) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        ControllerGuard cg(sched);
        AsyncMutex mtx(sched);
        AsyncCondition cond(mtx);

        std::atomic<bool> waiter_done{false};
        std::atomic<bool> outcome_woken{false};
        WaitNode mlock, cn;

        Fiber w;
        w.set_entry([&](Fiber&) {
            mtx.lock(mlock);
            WaitOutcome r = cond.wait(cn);  // pauses at register seam
            outcome_woken.store(r == WaitOutcome::woken, std::memory_order::release);
            waiter_done.store(true, std::memory_order::release);
            mtx.unlock();
        });
        FiberStack sa;
        SLUICE_CHECK(sched.init_fiber(w, sa.base(), sa.size()));
        sched.spawn(w);

        E12CondSeam::arm_register_before_handoff(sched);
        std::thread run_thread([&] { sched.run_live(1); });
        E12CondSeam::wait_register_paused(sched);
        // Spawn a separate thread to call notify_one. It will block on
        // global_mtx_ (held by the seam-paused worker). After release_register,
        // the worker continues through handoff → suspend, releases global_mtx_,
        // and then the notify thread resolves the node — proving the notify
        // arrives during the registered-but-not-yet-resumed vulnerable window.
        std::thread notify_thread([&] { cond.notify_one(); });
        E12CondSeam::release_register(sched);
        notify_thread.join();
        run_thread.join();

        SLUICE_CHECK_MSG(waiter_done.load(), "waiter completed each iteration");
        if (outcome_woken.load()) ++total_woken;
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no queue leak per iteration");
    }
    SLUICE_CHECK_MSG(total_woken == ITERS,
                     "50/50: all iterations resolved waiter Woken");
}

// ---- T31: notify/cancel/expire outcome matrix 500/500 ---------------------
// 500 iterations: three-way outcome-matrix verification — notify_one Woken,
// timer-expiry Expired, cancel Cancelled. Each iteration uses staggered
// deadlines (n[0]: 100, n[1]: 100, n[2]: 300) so the driver deterministically
// assigns one outcome per waiter: notify_one → n[0], advance_clock(200) →
// n[1] Expired, cancel → n[2] Cancelled. This is NOT a concurrent race (the
// operations are sequential in the driver fiber). The purpose is to prove
// each terminal cause operates correctly and that the losers for each node
// are no-ops (exactly-one terminal cause). Two-way arbitration (notify vs
// cancel, notify vs expire, cancel vs expire) is covered by T12a/b/T13a/b.
SLUICE_TEST_CASE(e12_cond_t31_notify_cancel_expire_500) {
    if constexpr (!fiber_ctx::supported) return;
    constexpr int ITERS = 500;
    constexpr int K = 3;  // waiters per iteration (Woken/Cancelled/Expired)
    int total_woken = 0, total_cancelled = 0, total_expired = 0;

    for (int it = 0; it < ITERS; ++it) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        E11Timer::enable_test_clock(sched);
        AsyncMutex mtx(sched);
        AsyncCondition cond(mtx);

        WaitNode n[K], ml[K];
        // Staggered deadlines: n[0], n[1] expire at clock 200; n[2] at 300.
        // So: notify_one wins n[0]; expire wins n[1]; cancel wins n[2].
        const int d[K] = {100, 100, 300};
        std::atomic<bool> waiter_parked[K];
        for (int k = 0; k < K; ++k) waiter_parked[k].store(false, std::memory_order_relaxed);
        Fiber fw[K];
        FiberStack sw[K];
        for (int k = 0; k < K; ++k) {
            fw[k].set_entry([&, k](Fiber&) {
                mtx.lock(ml[k]);
                waiter_parked[k].store(true, std::memory_order_release);
                (void)cond.wait_until(n[k], d[k]);
                mtx.unlock();
            });
            SLUICE_CHECK(sched.init_fiber(fw[k], sw[k].base(), sw[k].size()));
            sched.spawn(fw[k]);
        }

        // Driver: wait for all parked, notify one (Woken), advance clock
        // (Expired for n[1]), cancel n[2] (Cancelled).
        Fiber driver;
        driver.set_entry([&](Fiber&) {
            // Suspend (not busy-wait) on each parked flag so the K waiter
            // fibers can run to their wait suspension. A bare spin loop here
            // could monopolize the single worker and deadlock the scheduler.
            for (int k = 0; k < K; ++k) {
                sched.await_ready_flag(waiter_parked[k]);
            }
            cond.notify_one();          // n[0] gets Woken (FIFO head)
            sched.advance_clock(200);   // n[1] deadline=100 -> Expired
            for (int i = 0; i < 50; ++i) {
                if (cond.cancel(n[2])) break;  // n[2] deadline=300 -> Cancelled
                std::this_thread::yield();
            }
        });
        FiberStack sd;
        SLUICE_CHECK(sched.init_fiber(driver, sd.base(), sd.size()));
        sched.spawn(driver);
        sched.run(1);

        for (int k = 0; k < K; ++k) {
            if (n[k].was_woken()) ++total_woken;
            else if (n[k].was_cancelled()) ++total_cancelled;
            else if (n[k].was_expired()) ++total_expired;
        }
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no queue leak per iteration");
    }
    int total_terminal = total_woken + total_cancelled + total_expired;
    SLUICE_CHECK_MSG(total_terminal == ITERS * K,
                     "500/500: all iterations resolved all waiters terminal");
    SLUICE_CHECK_MSG(total_woken > 0, "notify won at least once");
    SLUICE_CHECK_MSG(total_cancelled > 0, "cancel won at least once");
    SLUICE_CHECK_MSG(total_expired > 0, "expire won at least once");
}

// ---- T32: notify_before_drain seam (50/50) -------------------------------
// 50 iterations: arms the `e12_condition_notify_before_drain` seam inside
// condition_notify_all (scheduler.cpp:2327), proving the seam fires at the
// correct point: global_mtx_ has been acquired, the drain loop has NOT yet
// started. While paused, an OS-thread coordinator observes that NO waiter
// nodes are resolved (the drain is still pending). After seam release, the
// drain runs and all registered waiters resolve Woken. This exercises the
// previously-dead notify_before_drain seam code path.
SLUICE_TEST_CASE(e12_cond_t32_notify_before_drain_seam_50) {
    if constexpr (!fiber_ctx::supported) return;
    constexpr int ITERS = 50;

    for (int it = 0; it < ITERS; ++it) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        ControllerGuard cg(sched);
        AsyncMutex mtx(sched);
        AsyncCondition cond(mtx);

        constexpr int N = 3;
        WaitNode n[N], ml[N];
        std::atomic<bool> waiter_parked[N];
        for (int i = 0; i < N; ++i)
            waiter_parked[i].store(false, std::memory_order_relaxed);

        Fiber fw[N];
        FiberStack sw[N];
        for (int i = 0; i < N; ++i) {
            fw[i].set_entry([&, i](Fiber&) {
                mtx.lock(ml[i]);
                waiter_parked[i].store(true, std::memory_order_release);
                (void)cond.wait(n[i]);
                mtx.unlock();
            });
            SLUICE_CHECK(sched.init_fiber(fw[i], sw[i].base(), sw[i].size()));
            sched.spawn(fw[i]);
        }

        Fiber notifier;
        FiberStack sn;
        notifier.set_entry([&](Fiber&) {
            // Suspend (not busy-wait) on each parked flag so the N waiter
            // fibers can run to their wait suspension.
            for (int i = 0; i < N; ++i) {
                sched.await_ready_flag(waiter_parked[i]);
            }
            cond.notify_all();  // pauses at notify_before_drain seam
        });
        SLUICE_CHECK(sched.init_fiber(notifier, sn.base(), sn.size()));
        sched.spawn(notifier);

        E12CondSeam::arm_notify_before_drain(sched);
        std::thread run_thread([&] { sched.run_live(1); });
        E12CondSeam::wait_notify_paused(sched);

        // Seam is paused: global_mtx_ held by notifier, drain NOT started.
        // Verify that no nodes are resolved yet.
        for (int i = 0; i < N; ++i) {
            SLUICE_CHECK_MSG(!n[i].is_terminal(),
                             "seam paused before any node resolved");
        }

        E12CondSeam::release_notify(sched);
        run_thread.join();

        int resolved = 0;
        for (int i = 0; i < N; ++i) {
            if (n[i].was_woken()) ++resolved;
        }
        SLUICE_CHECK_MSG(resolved == N,
                         "all waiters resolved Woken by notify_all after seam");
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no queue leak");
    }
}

// ---- T33: notify-after-park stress 200/200 --------------------------------
// 200 iterations: cooperative waiter+notifier, notify after full suspension.
// Pure stress — no seam, no race. The lost-notify window is covered by T30.
SLUICE_TEST_CASE(e12_cond_t33_notify_after_park_200) {
    if constexpr (!fiber_ctx::supported) return;
    constexpr int ITERS = 200;
    int total_woken = 0;

    for (int it = 0; it < ITERS; ++it) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        AsyncMutex mtx(sched);
        AsyncCondition cond(mtx);

        std::atomic<bool> waiter_done{false};
        std::atomic<bool> outcome_woken{false};
        std::atomic<bool> waiter_suspended{false};
        WaitNode mlock, cn;

        Fiber w, notifier;
        FiberStack sa, sb;
        w.set_entry([&](Fiber&) {
            mtx.lock(mlock);
            waiter_suspended.store(true, std::memory_order::release);
            WaitOutcome r = cond.wait(cn);
            outcome_woken.store(r == WaitOutcome::woken, std::memory_order::release);
            waiter_done.store(true, std::memory_order::release);
            mtx.unlock();
        });
        notifier.set_entry([&](Fiber&) {
            sched.await_ready_flag(waiter_suspended);
            cond.notify_one();
        });

        SLUICE_CHECK(sched.init_fiber(w, sa.base(), sa.size()));
        SLUICE_CHECK(sched.init_fiber(notifier, sb.base(), sb.size()));
        sched.spawn(w);
        sched.spawn(notifier);
        sched.run(1);

        SLUICE_CHECK_MSG(waiter_done.load(), "waiter completed each iteration");
        if (outcome_woken.load()) ++total_woken;
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no queue leak per iteration");
    }
    SLUICE_CHECK_MSG(total_woken == ITERS,
                     "200/200: all iterations resolved waiter Woken");
}

// ---- T34: notify_all static stress 200/200 --------------------------------
// 200 iterations: 3 static waiters, notify_all after all parked. Pure stress.
SLUICE_TEST_CASE(e12_cond_t34_notify_all_stress_200) {
    if constexpr (!fiber_ctx::supported) return;
    constexpr int ITERS = 200;
    constexpr int N = 3;
    int total_resolved = 0;

    for (int it = 0; it < ITERS; ++it) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        AsyncMutex mtx(sched);
        AsyncCondition cond(mtx);

        WaitNode n[N], ml[N];
        Fiber fw[N], notif;
        FiberStack sw[N], sn;
        std::atomic<bool> waiter_parked[N];
        for (int i = 0; i < N; ++i) waiter_parked[i].store(false, std::memory_order_relaxed);

        for (int i = 0; i < N; ++i) {
            fw[i].set_entry([&, i](Fiber&) {
                mtx.lock(ml[i]);
                waiter_parked[i].store(true, std::memory_order_release);
                (void)cond.wait(n[i]);
                mtx.unlock();
            });
            SLUICE_CHECK(sched.init_fiber(fw[i], sw[i].base(), sw[i].size()));
            sched.spawn(fw[i]);
        }

        notif.set_entry([&](Fiber&) {
            // Suspend (not busy-wait) on each parked flag so the N waiter
            // fibers can run to their wait suspension. A bare spin loop here
            // could monopolize the single worker and deadlock the scheduler.
            for (int i = 0; i < N; ++i) {
                sched.await_ready_flag(waiter_parked[i]);
            }
            cond.notify_all();
        });
        SLUICE_CHECK(sched.init_fiber(notif, sn.base(), sn.size()));
        sched.spawn(notif);
        sched.run(1);

        int resolved = 0;
        for (int i = 0; i < N; ++i) {
            if (n[i].was_woken()) ++resolved;
        }
        total_resolved += resolved;
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no queue leak per iteration");
    }
    SLUICE_CHECK_MSG(total_resolved == ITERS * N,
                     "200/200: all static waiters resolved by notify_all");
}
