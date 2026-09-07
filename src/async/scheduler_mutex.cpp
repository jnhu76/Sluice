#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"

#include <utility>
#include <cstdio>
#include <cstdlib>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {
bool Scheduler::mutex_try_lock(WaitQueue& waiters, Fiber*& owner) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::try_lock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    if (owner == me) {
        return false;
    }
    if (owner == nullptr && waiters.empty_locked()) {
        owner = me;
        return true;
    }
    return false;
}

void Scheduler::mutex_lock(WaitQueue& waiters, Fiber*& owner, WaitNode& node) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::lock requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner != me && "AsyncMutex::lock recursive acquisition is a caller "
                          "precondition violation (not a successful acquisition)");
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, WaitResume::fiber(me))) {
            return;
        }
        ++waiting_waitq_count_;

        if (node.prev_ == nullptr && owner == nullptr) {
            if (waiters.wake_node_locked(node)) {
                owner = me;
                if (waiting_waitq_count_ > 0)
                    --waiting_waitq_count_;
            }
            return;
        }

        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            return;
        }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::mutex_waiter_registered_before_grant);
#endif
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::mutex_lock_until(WaitQueue& waiters, Fiber*& owner, WaitNode& node,
                                 deadline_t deadline) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::lock_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner != me && "AsyncMutex::lock_until recursive acquisition is a "
                          "caller precondition violation");
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());

        reg = prepare_ordinary_deadline_locked(&node, &waiters, deadline);
        if (!waiters.register_wait_locked(node, WaitResume::fiber(me))) {
            erase_popped_registration_locked(reg);
            return;
        }
        ++waiting_waitq_count_;

        publish_ordinary_deadline_locked(reg);

        if (node.prev_ == nullptr && owner == nullptr) {
            if (waiters.wake_node_locked(node)) {
                owner = me;

                (void)retire_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0)
                    --waiting_waitq_count_;
            }
            return;
        }

        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0)
                    --waiting_waitq_count_;

                return;
            }
        }

        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            return;
        }
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

bool Scheduler::mutex_cancel(WaitQueue& waiters, WaitNode& node) {
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());

    if (!cancel_primitive_wait_locked(waiters, node))
        return false;
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    publish_wait_winner_locked(node);
    return true;
}

WaitNode* Scheduler::mutex_handoff_one_locked(WaitQueue& waiters, Fiber*& owner) {
    LockGuard qlk(waiters.mtx());
    WaitNode* won = waiters.wake_one_locked();
    if (won == nullptr)
        return nullptr;
    Fiber* f = won->fiber();
    assert(f != nullptr && "MUTEX-HANDOFF-ONE winner has null Fiber "
                           "(internal invariant failure, NOT empty queue)");

    owner = f;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::mutex_handoff_before_publication);
#endif
    retire_timer_for_node_locked(*won);
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    if (f != nullptr) {
        publish_waiting_fiber_runnable_locked(f);
    }
    return won;
}

void Scheduler::mutex_unlock(WaitQueue& waiters, Fiber*& owner) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::unlock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    assert(owner == me && "AsyncMutex::unlock by non-owner is a caller "
                          "precondition violation (no owner/queue mutation)");
    (void)me;

    if (mutex_handoff_one_locked(waiters, owner) != nullptr) {
        return;
    }

    owner = nullptr;
}

} // namespace sluice::async
