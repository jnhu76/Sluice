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
std::size_t Scheduler::event_set_broadcast(Event& event) {
    LockGuard lk(global_mtx_);
    bool previous = event.set_.exchange(true, std::memory_order::release);
    if (previous) {
        return 0;
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::event_set_store_before_drain);
#endif
    std::size_t woken = 0;
    while (wake_wait_one_locked(event.waiters_) != nullptr) {
        ++woken;
    }

    (void)select_resolve_event_locked(event);
    return woken;
}

void Scheduler::event_reset(std::atomic<bool>& set_flag) {
    LockGuard lk(global_mtx_);
    set_flag.store(false, std::memory_order::release);
}

void Scheduler::select_event_link_locked(Event& event, detail::SelectArmSlot& arm) {
    assert(&event.scheduler_ == this &&
           "select_event_link_locked: Event does not belong to this Scheduler");
    if (&event.scheduler_ != this)
        detail::select_invariant_fail_fast();

    assert(arm.home_ == nullptr && "select_event_link_locked: arm already linked");
    if (arm.home_ != nullptr)
        detail::select_invariant_fail_fast();
    assert(arm.next_ == nullptr && "select_event_link_locked: arm.next_ not null");
    if (arm.next_ != nullptr)
        detail::select_invariant_fail_fast();
    assert(arm.prev_ == nullptr && "select_event_link_locked: arm.prev_ not null");
    if (arm.prev_ != nullptr)
        detail::select_invariant_fail_fast();
    assert(arm.kind == detail::ArmKind::event &&
           "select_event_link_locked: arm kind must be event");
    if (arm.kind != detail::ArmKind::event)
        detail::select_invariant_fail_fast();
    assert(arm.event.event_ == &event &&
           "select_event_link_locked: arm.event does not point to this Event");
    if (arm.event.event_ != &event)
        detail::select_invariant_fail_fast();
    assert((arm.state == detail::ArmState::detached || arm.state == detail::ArmState::prepared) &&
           "select_event_link_locked: arm state must be Detached or Prepared");
    if (arm.state != detail::ArmState::detached && arm.state != detail::ArmState::prepared) {
        detail::select_invariant_fail_fast();
    }
    assert(arm.group != nullptr && "select_event_link_locked: arm.group must be set");
    if (arm.group == nullptr)
        detail::select_invariant_fail_fast();

    arm.home_ = &event.select_port_;
    arm.state = detail::ArmState::registered;

    detail::SelectPort& port = event.select_port_;
    arm.next_ = port.head_;
    if (port.head_ != nullptr) {
        port.head_->prev_ = &arm;
    }
    arm.prev_ = nullptr;
    port.head_ = &arm;
}

void Scheduler::select_event_unlink_locked(Event& event, detail::SelectArmSlot& arm) {
    assert(&event.scheduler_ == this &&
           "select_event_unlink_locked: Event does not belong to this Scheduler");
    if (&event.scheduler_ != this)
        detail::select_invariant_fail_fast();

    assert(arm.home_ == &event.select_port_ &&
           "select_event_unlink_locked: arm does not belong to this Event");
    if (arm.home_ != &event.select_port_)
        detail::select_invariant_fail_fast();
    assert((arm.state == detail::ArmState::registered ||
            arm.state == detail::ArmState::candidate_ready ||
            arm.state == detail::ArmState::retired) &&
           "select_event_unlink_locked: unexpected arm state");
    if (arm.state != detail::ArmState::registered &&
        arm.state != detail::ArmState::candidate_ready && arm.state != detail::ArmState::retired) {
        detail::select_invariant_fail_fast();
    }

    detail::SelectPort& port = event.select_port_;

    if (arm.prev_ != nullptr) {
        arm.prev_->next_ = arm.next_;
    } else {
        port.head_ = arm.next_;
    }

    if (arm.next_ != nullptr) {
        arm.next_->prev_ = arm.prev_;
    }

    arm.next_ = nullptr;
    arm.prev_ = nullptr;
    arm.home_ = nullptr;
}

std::size_t Scheduler::select_event_scan_locked(Event& event) {
    assert(&event.scheduler_ == this &&
           "select_event_scan_locked: Event does not belong to this Scheduler");
    if (&event.scheduler_ != this)
        detail::select_invariant_fail_fast();

    std::size_t marked = 0;
    detail::SelectArmSlot* arm = event.select_port_.head_;
    while (arm != nullptr) {
        detail::SelectArmSlot* next = arm->next_;
        if (arm->kind == detail::ArmKind::event && arm->state == detail::ArmState::registered &&
            arm->group != nullptr && arm->group->phase() == detail::GroupPhase::armed &&
            arm->home_ == &event.select_port_ && arm->event.event_ == &event) {
            arm->state = detail::ArmState::candidate_ready;
            ++marked;
        }
        arm = next;
    }
    return marked;
}

bool Scheduler::event_cancel_wait(WaitQueue& q, WaitNode& node) {
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());

    if (!cancel_primitive_wait_locked(q, node))
        return false;
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    publish_wait_winner_locked(node);
    return true;
}

Scheduler::WaitAdmitDisposition
Scheduler::event_wait_admit_locked(WaitQueue& q, const std::atomic<bool>& set_flag, WaitNode& node,
                                   const WaitResume& resume, bool timed, deadline_t deadline) {
    TimerRegistration* reg = nullptr;
    if (timed) {
        reg = prepare_ordinary_deadline_locked(&node, &q, deadline);
    }
    if (!q.register_wait_locked(node, resume)) {
        if (timed)
            erase_popped_registration_locked(reg);
        return WaitAdmitDisposition::rejected;
    }
    ++waiting_waitq_count_;
    if (timed) {
        publish_ordinary_deadline_locked(reg);
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::event_admission_before_final_check);
#endif

    if (set_flag.load(std::memory_order::acquire)) {
        if (q.wake_node_locked(node)) {
            if (timed) {
                (void)retire_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
            }
            if (waiting_waitq_count_ > 0)
                --waiting_waitq_count_;
        }
        return WaitAdmitDisposition::resolved_inline;
    }
    if (timed) {
        if (clock_now_unlocked() >= deadline) {
            if (q.expire_locked(node)) {
                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0)
                    --waiting_waitq_count_;
                return WaitAdmitDisposition::resolved_inline;
            }
        }
    }

    if (node.is_terminal()) {
        q.unlink_locked(node);
        --waiting_waitq_count_;
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
        }
        return WaitAdmitDisposition::resolved_inline;
    }

    return WaitAdmitDisposition::authorized;
}

void Scheduler::await_event_wait(WaitQueue& q, const std::atomic<bool>& set_flag, WaitNode& node) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::event_admission_attempt_before_global_lock);
#endif
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (event_wait_admit_locked(q, set_flag, node, WaitResume::fiber(me), false,
                                    deadline_t{}) != WaitAdmitDisposition::authorized) {
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

void Scheduler::await_event_wait_deadline(WaitQueue& q, const std::atomic<bool>& set_flag,
                                          WaitNode& node, deadline_t deadline) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (event_wait_admit_locked(q, set_flag, node, WaitResume::fiber(me), true, deadline) !=
            WaitAdmitDisposition::authorized) {
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

} // namespace sluice::async
