




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

Scheduler::ConditionAdmitDisposition Scheduler::condition_wait_admit_locked(
    WaitQueue& cond_waiters, WaitNode& cond_node, const WaitResume& resume,
    WaitQueue& mutex_waiters, Fiber*& owner, bool timed, deadline_t deadline)
    SLUICE_REQUIRES(global_mtx_) {







    TimerRegistration* reg = nullptr;
    {
        LockGuard qlk(cond_waiters.mtx());
        if (timed) {



            reg = prepare_ordinary_deadline_locked(&cond_node, &cond_waiters,
                                                   deadline);
        }


        if (!cond_waiters.register_wait_locked(cond_node, resume)) {
            if (timed) erase_popped_registration_locked(reg);



            return ConditionAdmitDisposition::rejected_retain;
        }
        ++waiting_waitq_count_;
        if (timed) {





            publish_ordinary_deadline_locked(reg);
        }
    }





    if (timed && clock_now_unlocked() >= deadline) {
        LockGuard qlk(cond_waiters.mtx());
        if (cond_waiters.expire_locked(cond_node)) {


            (void)consume_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;

            return ConditionAdmitDisposition::resolved_inline_retain;
        }


    }





#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::condition_register_before_handoff);
#endif





    if (mutex_handoff_one_locked(mutex_waiters, owner) == nullptr) {
        owner = nullptr;
    }





    if (cond_node.is_terminal()) {
        return ConditionAdmitDisposition::resolved_inline_released;
    }
    return ConditionAdmitDisposition::authorized;
}

WaitOutcome Scheduler::condition_wait_prepare(WaitQueue& cond_waiters,
                                              WaitNode& cond_node,
                                              WaitQueue& mutex_waiters,
                                              Fiber*& owner,
                                              bool& released_mutex) {






    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncCondition::wait requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner == me && "AsyncCondition::wait by a non-owner Fiber is a "
                          "caller precondition violation");
    {
        LockGuard lk(global_mtx_);
        const ConditionAdmitDisposition disp = condition_wait_admit_locked(
            cond_waiters, cond_node, WaitResume::fiber(me), mutex_waiters,
            owner, false, deadline_t{});
        if (disp != ConditionAdmitDisposition::authorized &&
            disp != ConditionAdmitDisposition::resolved_inline_released) {
            released_mutex = false;
            return cond_node.outcome();
        }
        released_mutex = true;
        if (disp == ConditionAdmitDisposition::resolved_inline_released) {
            return cond_node.outcome();
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


    return cond_node.outcome();
}

WaitOutcome Scheduler::condition_wait_prepare_until(WaitQueue& cond_waiters,
                                                    WaitNode& cond_node,
                                                    WaitQueue& mutex_waiters,
                                                    Fiber*& owner,
                                                    deadline_t deadline,
                                                    bool& released_mutex) {














    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncCondition::wait_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner == me && "AsyncCondition::wait_until by a non-owner Fiber is a "
                          "caller precondition violation");
    {
        LockGuard lk(global_mtx_);
        const ConditionAdmitDisposition disp = condition_wait_admit_locked(
            cond_waiters, cond_node, WaitResume::fiber(me), mutex_waiters,
            owner, true, deadline);
        if (disp != ConditionAdmitDisposition::authorized &&
            disp != ConditionAdmitDisposition::resolved_inline_released) {
            released_mutex = false;
            return cond_node.outcome();
        }
        released_mutex = true;
        if (disp == ConditionAdmitDisposition::resolved_inline_released) {
            return cond_node.outcome();
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
    return cond_node.outcome();
}

void Scheduler::condition_notify_one(WaitQueue& cond_waiters) {






    LockGuard lk(global_mtx_);
    (void)wake_wait_one_locked(cond_waiters);
}

std::size_t Scheduler::condition_notify_all(WaitQueue& cond_waiters) {








    std::size_t woken = 0;
    LockGuard lk(global_mtx_);



#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::condition_notify_before_drain);
#endif
    while (wake_wait_one_locked(cond_waiters) != nullptr) {
        ++woken;
    }
    return woken;
}

bool Scheduler::condition_cancel_wait(WaitQueue& cond_waiters, WaitNode& cond_node) {









    LockGuard lk(global_mtx_);
    LockGuard qlk(cond_waiters.mtx());
    if (!cancel_primitive_wait_locked(cond_waiters, cond_node)) return false;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;






    publish_wait_winner_locked(cond_node);
    return true;
}

}
