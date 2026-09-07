




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
bool Scheduler::sem_try_acquire(WaitQueue& waiters,
                                std::atomic<std::uint32_t>& available) {












    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    if (waiters.empty_locked()) {
        const std::uint32_t cur = available.load(std::memory_order::acquire);
        if (cur > 0) {
            available.store(cur - 1, std::memory_order::release);
            return true;
        }
    }
    return false;
}

void Scheduler::sem_acquire(WaitQueue& waiters,
                            std::atomic<std::uint32_t>& available,
                            WaitNode& node) {





















    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, WaitResume::fiber(me))) {

            return;
        }
        ++waiting_waitq_count_;
















        if (node.prev_ == nullptr &&
            available.load(std::memory_order::acquire) > 0) {
            if (waiters.wake_node_locked(node)) {
                available.store(available.load(std::memory_order::acquire) - 1,
                                std::memory_order::release);
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;



            }
            return;
        }




        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            return;
        }
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::sem_acquire_until(WaitQueue& waiters,
                                  std::atomic<std::uint32_t>& available,
                                  WaitNode& node, deadline_t deadline) {
















    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
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





        if (node.prev_ == nullptr &&
            available.load(std::memory_order::acquire) > 0) {
            if (waiters.wake_node_locked(node)) {
                available.store(
                    available.load(std::memory_order::acquire) - 1,
                    std::memory_order::release);


                (void)retire_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;

            }
            return;
        }






        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {


                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;

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
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

bool Scheduler::sem_cancel(WaitQueue& waiters, WaitNode& node) {














    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());

    if (!cancel_primitive_wait_locked(waiters, node)) return false;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;





    publish_wait_winner_locked(node);
    return true;
}

bool Scheduler::sem_release(WaitQueue& waiters,
                            std::atomic<std::uint32_t>& available,
                            std::uint32_t max_permits) {
































    LockGuard lk(global_mtx_);




    if (wake_wait_one_locked(waiters) != nullptr) {
        return true;
    }

    const std::uint32_t cur = available.load(std::memory_order::acquire);
    if (cur >= max_permits) {
        return false;
    }
    available.store(cur + 1, std::memory_order::release);
    return true;
}

}
