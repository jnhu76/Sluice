




















#include <sluice/async/scheduler.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "queue_detail.hpp"
#include "scheduler_internal.hpp"
#include "scheduler_test_access.hpp"

namespace sluice::async {

bool Scheduler::AsyncTestAccess::event_wait_deferred_for_test(
    Scheduler& s, Event& event, WaitNode& node, FeDeferredRecord& record) {
    LockGuard lk(s.global_mtx_);
    LockGuard qlk(event.waiters_.mtx());
    if (s.event_wait_admit_locked(event.waiters_, event.set_, node,
                                  WaitResume::deferred(&record),
false,
                                  deadline_t{}) !=
        Scheduler::WaitAdmitDisposition::authorized) {
        return false;
    }
    record.arm();
    return true;
}

bool Scheduler::AsyncTestAccess::event_wait_deferred_deadline_for_test(
    Scheduler& s, Event& event, WaitNode& node, FeDeferredRecord& record,
    deadline_t deadline) {
    LockGuard lk(s.global_mtx_);
    LockGuard qlk(event.waiters_.mtx());
    if (s.event_wait_admit_locked(event.waiters_, event.set_, node,
                                  WaitResume::deferred(&record),
true, deadline) !=
        Scheduler::WaitAdmitDisposition::authorized) {
        return false;
    }
    record.arm();
    return true;
}

bool Scheduler::AsyncTestAccess::event_cancel_deferred_for_test(
    Scheduler& s, Event& event, WaitNode& node) {
    return s.event_cancel_wait(event.waiters_, node);
}




























bool Scheduler::AsyncTestAccess::queue_push_deferred_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record) {

    detail::QueueItemControl* c = lease.control_;
    if (c == nullptr || c->owner_port_ != &port ||
        c->location_ != detail::QueueItemControl::Location::detached) {
        detail::queue_lease_fail_fast();
    }
    c->location_ = detail::QueueItemControl::Location::producer_operation;
    ctx = QueueWaitCtx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    return queue_push_core_(s, port, lease, node, record,
false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::queue_push_deferred_until_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
    deadline_t deadline) {
    detail::QueueItemControl* c = lease.control_;
    if (c == nullptr || c->owner_port_ != &port ||
        c->location_ != detail::QueueItemControl::Location::detached) {
        detail::queue_lease_fail_fast();
    }
    c->location_ = detail::QueueItemControl::Location::producer_operation;
    ctx = QueueWaitCtx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    return queue_push_core_(s, port, lease, node, record,
true, deadline);
}

bool Scheduler::AsyncTestAccess::queue_pop_deferred_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& out,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record) {
    ctx = QueueWaitCtx{&port, detail::QueueRole::consumer,
                       nullptr, nullptr, &out};
    node.set_user(&ctx);
    return queue_pop_core_(s, port, out, node, record,
false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::queue_pop_deferred_until_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& out,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
    deadline_t deadline) {
    ctx = QueueWaitCtx{&port, detail::QueueRole::consumer,
                       nullptr, nullptr, &out};
    node.set_user(&ctx);
    return queue_pop_core_(s, port, out, node, record,
true, deadline);
}

bool Scheduler::AsyncTestAccess::queue_push_core_(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
    WaitNode& node, FeDeferredRecord& record, bool timed,
    deadline_t deadline) {
    LockGuard glk(s.global_mtx_);
    LockGuard slk(port.state_mtx_);


    if (port.lifecycle_ != detail::QueueLifecycle::operational) {
        detail::queue_lease_fail_fast();
    }



















    ++port.active_port_calls_;
    QueueAdmitDisposition disp;
    bool grant = false;
    try {
        LockGuard qlk(port.waiters_[0].mtx());
        disp = s.queue_push_admit_locked(port, lease, node,
                                         WaitResume::deferred(&record), timed,
                                         deadline);
        if (disp == QueueAdmitDisposition::authorized) {


            record.arm();
        }
        grant = disp == QueueAdmitDisposition::resolved_inline_grant;
    } catch (...) {
        if (port.active_port_calls_ > 0) --port.active_port_calls_;
        throw;
    }


    if (grant) (void)s.queue_grant_consumer_locked(port);

    return disp == QueueAdmitDisposition::authorized;
}

bool Scheduler::AsyncTestAccess::queue_pop_core_(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& out,
    WaitNode& node, FeDeferredRecord& record, bool timed, deadline_t deadline) {
    LockGuard glk(s.global_mtx_);
    LockGuard slk(port.state_mtx_);
    if (port.lifecycle_ != detail::QueueLifecycle::operational) {
        detail::queue_lease_fail_fast();
    }



    ++port.active_port_calls_;
    QueueAdmitDisposition disp;
    bool grant = false;
    try {
        LockGuard qlk(port.waiters_[1].mtx());
        disp = s.queue_pop_admit_locked(port, out, node,
                                        WaitResume::deferred(&record), timed,
                                        deadline);
        if (disp == QueueAdmitDisposition::authorized) {
            record.arm();
        }
        grant = disp == QueueAdmitDisposition::resolved_inline_grant;
    } catch (...) {
        if (port.active_port_calls_ > 0) --port.active_port_calls_;
        throw;
    }
    if (grant) (void)s.queue_grant_producer_locked(port);

    return disp == QueueAdmitDisposition::authorized;
}

void Scheduler::AsyncTestAccess::queue_release_deferred_pin_for_test(
    detail::QueuePort& port) {








    LockGuard glk(port.scheduler_.global_mtx_);
    LockGuard slk(port.state_mtx_);
    if (port.active_port_calls_ == 0) {
        detail::queue_lease_fail_fast();
    }
    --port.active_port_calls_;
}











bool Scheduler::AsyncTestAccess::rwlock_read_deferred_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record) {
    return rwlock_read_core_(s, lock, node, actor_token, ctx, record,
false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::rwlock_read_deferred_until_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline) {
    return rwlock_read_core_(s, lock, node, actor_token, ctx, record,
true, deadline);
}

bool Scheduler::AsyncTestAccess::rwlock_write_deferred_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record) {
    return rwlock_write_core_(s, lock, node, actor_token, ctx, record,
false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::rwlock_write_deferred_until_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline) {
    return rwlock_write_core_(s, lock, node, actor_token, ctx, record,
true, deadline);
}

bool Scheduler::AsyncTestAccess::rwlock_read_core_(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record, bool timed, deadline_t deadline) {
    ctx = RwWaitCtx{RwWaitCtx::Mode::read, ActorId::frontend(actor_token)};
    node.set_user(&ctx);
    LockGuard glk(s.global_mtx_);
    LockGuard qlk(lock.waiters_.mtx());
    if (s.rwlock_read_admit_locked(lock.waiters_, lock.active_readers_,
                                   lock.writer_active_, node,
                                   WaitResume::deferred(&record), timed,
                                   deadline, &lock.expire_ctx_) !=
        Scheduler::WaitAdmitDisposition::authorized) {
        return false;
    }
    record.arm();
    return true;
}

bool Scheduler::AsyncTestAccess::rwlock_write_core_(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record, bool timed, deadline_t deadline) {
    const ActorId actor = ActorId::frontend(actor_token);
    ctx = RwWaitCtx{RwWaitCtx::Mode::write, actor};
    node.set_user(&ctx);
    LockGuard glk(s.global_mtx_);
    LockGuard qlk(lock.waiters_.mtx());
    if (s.rwlock_write_admit_locked(lock.waiters_, lock.active_readers_,
                                    lock.writer_active_, lock.writer_owner_,
                                    node, WaitResume::deferred(&record), actor,
                                    timed, deadline, &lock.expire_ctx_) !=
        Scheduler::WaitAdmitDisposition::authorized) {
        return false;
    }
    record.arm();
    return true;
}

bool Scheduler::AsyncTestAccess::rwlock_try_write_deferred_for_test(
    Scheduler& s, AsyncRwLock& lock, void* actor_token) {
    LockGuard glk(s.global_mtx_);
    LockGuard qlk(lock.waiters_.mtx());
    return s.rwlock_try_write_admission_locked(
        lock.waiters_, lock.active_readers_, lock.writer_active_,
        lock.writer_owner_, ActorId::frontend(actor_token));
}

void Scheduler::AsyncTestAccess::rwlock_unlock_write_deferred_for_test(
    Scheduler& s, AsyncRwLock& lock, void* actor_token) {
    LockGuard lk(s.global_mtx_);
    s.rwlock_unlock_write_core_locked(lock.waiters_, lock.active_readers_,
                                      lock.writer_active_, lock.writer_owner_,
                                      ActorId::frontend(actor_token));
}

void Scheduler::AsyncTestAccess::rwlock_unlock_read_for_test(
    Scheduler& s, AsyncRwLock& lock) {
    s.rwlock_unlock_read(lock.waiters_, lock.active_readers_,
                         lock.writer_active_, lock.writer_owner_);
}

bool Scheduler::AsyncTestAccess::rwlock_try_read_for_test(
    Scheduler& s, AsyncRwLock& lock) {
    return s.rwlock_try_read_lock(lock.waiters_, lock.active_readers_,
                                  lock.writer_active_);
}

bool Scheduler::AsyncTestAccess::rwlock_cancel_deferred_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node) {
    return s.rwlock_cancel(lock.waiters_, lock.active_readers_,
                           lock.writer_active_, lock.writer_owner_, node);
}

bool Scheduler::AsyncTestAccess::rwlock_writer_active_for_test(
    AsyncRwLock& lock) {



    LockGuard lk(lock.scheduler_.global_mtx_);
    return lock.writer_active_;
}

bool Scheduler::AsyncTestAccess::rwlock_owned_by_for_test(
    AsyncRwLock& lock, const void* actor_token) {
    LockGuard lk(lock.scheduler_.global_mtx_);
    return lock.writer_owner_ ==
           ActorId::frontend(const_cast<void*>(actor_token));
}



bool Scheduler::AsyncTestAccess::condition_wait_deferred_for_test(
    Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
    WaitQueue& mutex_waiters, Fiber*& owner, FeDeferredRecord& record,
    bool& released) {
    return condition_wait_deferred_core_(s, cond_waiters, cond_node,
                                         mutex_waiters, owner, record,
false, deadline_t{},
                                         released);
}

bool Scheduler::AsyncTestAccess::condition_wait_deferred_until_for_test(
    Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
    WaitQueue& mutex_waiters, Fiber*& owner, deadline_t deadline,
    FeDeferredRecord& record, bool& released) {
    return condition_wait_deferred_core_(s, cond_waiters, cond_node,
                                         mutex_waiters, owner, record,
true, deadline, released);
}

bool Scheduler::AsyncTestAccess::condition_wait_deferred_core_(
    Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
    WaitQueue& mutex_waiters, Fiber*& owner, FeDeferredRecord& record,
    bool timed, deadline_t deadline, bool& released) {




    LockGuard lk(s.global_mtx_);
    const auto disp = s.condition_wait_admit_locked(
        cond_waiters, cond_node, WaitResume::deferred(&record),
        mutex_waiters, owner, timed, deadline);
    switch (disp) {
    case Scheduler::ConditionAdmitDisposition::authorized:
        released = true;
        record.arm();
        return true;
    case Scheduler::ConditionAdmitDisposition::resolved_inline_released:
        released = true;
        return false;
    default:
        released = false;
        return false;
    }
}

}

#endif
