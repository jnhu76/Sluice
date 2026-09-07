#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"
#include "queue_detail.hpp"

#include <utility>
#include <cstdio>
#include <cstdlib>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

void Scheduler::queue_timer_on_resolve(void* owner_ctx, bool) noexcept {
    auto* port = static_cast<detail::QueuePort*>(owner_ctx);
    if (port == nullptr)
        return;
    if (port->active_queue_timers_ > 0)
        --port->active_queue_timers_;
}

Scheduler::QueueAdmitDisposition
Scheduler::queue_push_admit_locked(detail::QueuePort& port, detail::QueueItemLease& lease,
                                   WaitNode& node, const WaitResume& resume, bool timed,
                                   deadline_t deadline) SLUICE_REQUIRES(global_mtx_) {
    detail::QueueItemControl* c = lease.control_;
    TimerRegistration* reg = nullptr;
    if (timed) {
        reg = prepare_ordinary_deadline_locked(&node, &port.waiters_[0], deadline);
    }
    if (!port.waiters_[0].register_wait_locked(node, resume)) {
        if (timed)
            erase_popped_registration_locked(reg);
        return QueueAdmitDisposition::rejected;
    }
    ++port.active_wait_associations_;
    ++waiting_waitq_count_;
    if (timed) {
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
    }

    if (!port.closed_ && !port.ring_full_locked() && node.prev_ == nullptr) {
        c->location_ = detail::QueueItemControl::Location::ring;
        const std::size_t tail = (port.ring_head_ + port.ring_count_) % port.capacity_;
        port.ring_[tail] = std::move(lease);
        ++port.ring_count_;
        port.waiters_[0].wake_node_locked(node);
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(false);
        }
        if (port.active_wait_associations_ > 0)
            --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0)
            --waiting_waitq_count_;

        return QueueAdmitDisposition::resolved_inline_grant;
    }

    if (port.closed_) {
        port.waiters_[0].wake_node_locked(node);
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(false);
        }
        if (port.active_wait_associations_ > 0)
            --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0)
            --waiting_waitq_count_;
        return QueueAdmitDisposition::resolved_inline;
    }
    if (timed) {
        if (clock_now_unlocked() >= deadline && port.waiters_[0].expire_locked(node)) {
            (void)consume_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(true);
            if (port.active_wait_associations_ > 0)
                --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0)
                --waiting_waitq_count_;
            return QueueAdmitDisposition::resolved_inline;
        }
    }

    return QueueAdmitDisposition::authorized;
}

Scheduler::QueueAdmitDisposition
Scheduler::queue_pop_admit_locked(detail::QueuePort& port, detail::QueueItemLease& out,
                                  WaitNode& node, const WaitResume& resume, bool timed,
                                  deadline_t deadline) SLUICE_REQUIRES(global_mtx_) {
    TimerRegistration* reg = nullptr;
    if (timed) {
        reg = prepare_ordinary_deadline_locked(&node, &port.waiters_[1], deadline);
    }
    if (!port.waiters_[1].register_wait_locked(node, resume)) {
        if (timed)
            erase_popped_registration_locked(reg);
        return QueueAdmitDisposition::rejected;
    }
    ++port.active_wait_associations_;
    ++waiting_waitq_count_;
    if (timed) {
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
    }

    if (!port.ring_empty_locked() && node.prev_ == nullptr) {
        const std::size_t head = port.ring_head_;
        out = std::move(port.ring_[head]);
        port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
        --port.ring_count_;
        out.control_->location_ = detail::QueueItemControl::Location::consumer_operation;
        port.waiters_[1].wake_node_locked(node);
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(false);
        }
        if (port.active_wait_associations_ > 0)
            --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0)
            --waiting_waitq_count_;

        return QueueAdmitDisposition::resolved_inline_grant;
    }

    if (port.ring_empty_locked() && port.closed_) {
        port.waiters_[1].wake_node_locked(node);
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(false);
        }
        if (port.active_wait_associations_ > 0)
            --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0)
            --waiting_waitq_count_;
        return QueueAdmitDisposition::resolved_inline;
    }
    if (timed) {
        if (clock_now_unlocked() >= deadline && port.waiters_[1].expire_locked(node)) {
            (void)consume_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(true);
            if (port.active_wait_associations_ > 0)
                --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0)
                --waiting_waitq_count_;
            return QueueAdmitDisposition::resolved_inline;
        }
    }
    return QueueAdmitDisposition::authorized;
}

void Scheduler::queue_publish_winner_locked(detail::QueuePort& port, WaitNode& won)
    SLUICE_REQUIRES(global_mtx_) {
    const WaitResume& r = won.resume();
    switch (r.kind()) {
    case WaitResume::Kind::fiber: {
        Fiber* f = r.as_fiber();
        if (f->make_runnable()) {
            ++port.granted_not_resumed_;
            WorkerState* owner = owner_for_fiber_locked(f);
            route_runnable_locked(f, owner);
        }
        break;
    }
    case WaitResume::Kind::deferred:
        defer_publication_locked(r.as_deferred());
        break;
    case WaitResume::Kind::none:
        break;
    }
}

void Scheduler::queue_push_admit(detail::QueuePort& port, WaitNode& node,
                                 detail::QueueItemLease& lease) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::push requires a running Fiber");
    Fiber* me = ws->current;
    detail::QueueItemControl* c = lease.control_;
    assert(c != nullptr && c->location_ == detail::QueueItemControl::Location::producer_operation);
    QueueWaitCtx ctx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        QueueAdmitDisposition disp;
        {
            LockGuard qlk(port.waiters_[0].mtx());
            disp = queue_push_admit_locked(port, lease, node, WaitResume::fiber(me), false,
                                           deadline_t{});
        }

        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_consumer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
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

    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0)
            --port.granted_not_resumed_;
    }
}

void Scheduler::queue_pop_admit(detail::QueuePort& port, WaitNode& node,
                                detail::QueueItemLease& out) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::pop requires a running Fiber");
    Fiber* me = ws->current;
    QueueWaitCtx ctx{&port, detail::QueueRole::consumer, nullptr, nullptr, &out};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        QueueAdmitDisposition disp;
        {
            LockGuard qlk(port.waiters_[1].mtx());
            disp =
                queue_pop_admit_locked(port, out, node, WaitResume::fiber(me), false, deadline_t{});
        }

        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_producer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
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

    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0)
            --port.granted_not_resumed_;
    }
}

void Scheduler::queue_push_admit_until(detail::QueuePort& port, WaitNode& node,
                                       detail::QueueItemLease& lease, deadline_t deadline) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::push_until requires a running Fiber");
    Fiber* me = ws->current;
    detail::QueueItemControl* c = lease.control_;
    QueueWaitCtx ctx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        QueueAdmitDisposition disp;
        {
            LockGuard qlk(port.waiters_[0].mtx());
            disp =
                queue_push_admit_locked(port, lease, node, WaitResume::fiber(me), true, deadline);
        }

        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_consumer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
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

    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0)
            --port.granted_not_resumed_;
    }
}

void Scheduler::queue_pop_admit_until(detail::QueuePort& port, WaitNode& node,
                                      detail::QueueItemLease& out, deadline_t deadline) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::pop_until requires a running Fiber");
    Fiber* me = ws->current;
    QueueWaitCtx ctx{&port, detail::QueueRole::consumer, nullptr, nullptr, &out};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        QueueAdmitDisposition disp;
        {
            LockGuard qlk(port.waiters_[1].mtx());
            disp = queue_pop_admit_locked(port, out, node, WaitResume::fiber(me), true, deadline);
        }

        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_producer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
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

    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0)
            --port.granted_not_resumed_;
    }
}

bool Scheduler::queue_cancel(detail::QueuePort& port, detail::QueueRole role, WaitNode& node) {
    LockGuard lk(global_mtx_);
    const std::size_t roleIdx = static_cast<std::size_t>(role);
    LockGuard qlk(port.waiters_[roleIdx].mtx());
    if (!cancel_primitive_wait_locked(port.waiters_[roleIdx], node))
        return false;
    if (port.active_wait_associations_ > 0)
        --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;
    publish_wait_winner_locked(node);
    return true;
}

WaitNode* Scheduler::queue_grant_consumer_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    LockGuard qlk(port.waiters_[1].mtx());
    WaitNode* won = port.waiters_[1].wake_one_locked();
    if (won == nullptr)
        return nullptr;
    auto* ctx = static_cast<QueueWaitCtx*>(won->user());

    retire_timer_for_node_locked(*won);

    if (!port.ring_empty_locked()) {
        const std::size_t head = port.ring_head_;
        *ctx->cons_out = std::move(port.ring_[head]);
        port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
        --port.ring_count_;
        ctx->cons_out->control_->location_ = detail::QueueItemControl::Location::consumer_operation;
    }
    if (port.active_wait_associations_ > 0)
        --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    queue_publish_winner_locked(port, *won);
    return won;
}

WaitNode* Scheduler::queue_grant_producer_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    LockGuard qlk(port.waiters_[0].mtx());
    WaitNode* won = port.waiters_[0].wake_one_locked();
    if (won == nullptr)
        return nullptr;
    auto* ctx = static_cast<QueueWaitCtx*>(won->user());

    retire_timer_for_node_locked(*won);

    if (!port.closed_ && !port.ring_full_locked()) {
        detail::QueueItemControl* c = ctx->prod_control;
        c->location_ = detail::QueueItemControl::Location::ring;
        const std::size_t tail = (port.ring_head_ + port.ring_count_) % port.capacity_;
        port.ring_[tail] = std::move(*ctx->prod_lease);
        ++port.ring_count_;
    }

    if (port.active_wait_associations_ > 0)
        --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    queue_publish_winner_locked(port, *won);
    return won;
}

bool Scheduler::queue_role_waiters_empty_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    {
        LockGuard qlk(port.waiters_[0].mtx());
        if (!port.waiters_[0].empty_locked())
            return false;
    }
    {
        LockGuard qlk(port.waiters_[1].mtx());
        if (!port.waiters_[1].empty_locked())
            return false;
    }
    return true;
}

} // namespace sluice::async
