// src/async/scheduler_fe2_test_seam.cpp
//
// Out-of-line definitions of the FE-2/FE-3 minimal stackless frontend seams
// declared in Scheduler::AsyncTestAccess (scheduler_test_access.hpp).
//
// The ENTIRE translation unit is gated on SLUICE_ASYNC_INTERNAL_TESTING:
// under the production `sluice_async` target (the macro is undefined) this
// file compiles to an empty TU, so the production archive carries NO FE seam
// symbol. Only the `sluice_async_internal_testing` variant compiles the real
// bodies.
//
// These definitions live OUT-OF-LINE (C4 / issue #135) because they touch
// Event's private state through the Scheduler friendship and therefore need
// the COMPLETE Event type; the installed scheduler.hpp must not gain that
// include footprint, so the seam header only declares.
//
// Semantic authority note: every body below runs PRODUCTION seams — the
// shared Event/Queue admission ladders (event_wait_admit_locked /
// queue_push_admit_locked / queue_pop_admit_locked), the production
// cancellation seams (event_cancel_wait / queue_cancel), and the deferred/
// take delivery split. This TU adds NO authority of its own.
#include <sluice/async/scheduler.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "queue_detail.hpp"
#include "scheduler_internal.hpp"  // RwWaitCtx (shared, non-installed)
#include "scheduler_test_access.hpp"

namespace sluice::async {

bool Scheduler::AsyncTestAccess::event_wait_deferred_for_test(
    Scheduler& s, Event& event, WaitNode& node, FeDeferredRecord& record) {
    LockGuard lk(s.global_mtx_);
    LockGuard qlk(event.waiters_.mtx());
    if (s.event_wait_admit_locked(event.waiters_, event.set_, node,
                                  WaitResume::deferred(&record),
                                  /*timed=*/false,
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
                                  /*timed=*/true, deadline) !=
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

// ---- FE-3 Queue deferred-frontend seams ----
//
// Each entry reproduces the QueuePort blocking-entry protocol for a deferred
// waiter and then runs the SHARED production ladder:
//   1. lease/entry validation + the detached->producer_operation (push) or
//      empty-out (pop) state QueuePort::push/pop establishes (the push
//      transition is folded into queue_push_deferred_for_test, the wrapper
//      the awaiter calls; the *_until_ entries below are the CS core and
//      assume it done — see the wrapper below);
//   2. QueueWaitCtx stashing on the WaitNode (frame-embedded ctx — the grant
//      winner writes through it after suspension);
//   3. the F.4 lifecycle gate + active_port_calls_ interval (G + S), so
//      begin_teardown serializes against the deferred admission exactly as
//      against a fiber admission;
//   4. the shared ladder under G + S + the direction's role mutex; on
//      `authorized` the PublicationEligibility commit (record.arm) lands
//      INSIDE that resolver-excluded CS (FE-1b L7);
//   5. the Q-LIV-1 opposite-role grant under G + S with the role mutex
//      released (the two role mutexes are NEVER held together).
//
// Returns true iff the caller must suspend (await_suspend -> true). Inline
// resolutions leave the outcome on the node and publish nothing (L6).
//
// active_port_calls_ is decremented at the single exit (the RAII CallGuard
// tail cannot be reused here: QueuePort::CallGuard is a private nested type
// and a namespace-scope helper struct has no friend access).

bool Scheduler::AsyncTestAccess::queue_push_deferred_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record) {
    // Entry validation + control transition (QueuePort::push verbatim shape).
    detail::QueueItemControl* c = lease.control_;  // friend access
    if (c == nullptr || c->owner_port_ != &port ||
        c->location_ != detail::QueueItemControl::Location::detached) {
        detail::queue_lease_fail_fast();
    }
    c->location_ = detail::QueueItemControl::Location::producer_operation;
    ctx = QueueWaitCtx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    return queue_push_core_(s, port, lease, node, record,
                            /*timed=*/false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::queue_push_deferred_until_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
    deadline_t deadline) {
    detail::QueueItemControl* c = lease.control_;  // friend access
    if (c == nullptr || c->owner_port_ != &port ||
        c->location_ != detail::QueueItemControl::Location::detached) {
        detail::queue_lease_fail_fast();
    }
    c->location_ = detail::QueueItemControl::Location::producer_operation;
    ctx = QueueWaitCtx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    return queue_push_core_(s, port, lease, node, record,
                            /*timed=*/true, deadline);
}

bool Scheduler::AsyncTestAccess::queue_pop_deferred_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& out,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record) {
    ctx = QueueWaitCtx{&port, detail::QueueRole::consumer,
                       nullptr, nullptr, &out};
    node.set_user(&ctx);
    return queue_pop_core_(s, port, out, node, record,
                           /*timed=*/false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::queue_pop_deferred_until_for_test(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& out,
    WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
    deadline_t deadline) {
    ctx = QueueWaitCtx{&port, detail::QueueRole::consumer,
                       nullptr, nullptr, &out};
    node.set_user(&ctx);
    return queue_pop_core_(s, port, out, node, record,
                           /*timed=*/true, deadline);
}

bool Scheduler::AsyncTestAccess::queue_push_core_(
    Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
    WaitNode& node, FeDeferredRecord& record, bool timed,
    deadline_t deadline) {
    LockGuard glk(s.global_mtx_);
    LockGuard slk(port.state_mtx_);
    // F.4 lifecycle gate (QueuePort::push verbatim): a deferred admission is
    // an ordinary port call.
    if (port.lifecycle_ != detail::QueueLifecycle::operational) {
        detail::queue_lease_fail_fast();
    }
    // NOTE-DRIFT-COUPLING: this hand-rolled F.4 entry interval mirrors
    // QueuePort::push's RAII CallGuard (CallGuard is a private nested type,
    // not reachable from a seam TU). If QueuePort's entry/teardown protocol
    // evolves, THIS text must move with it. Exception safety: the timed
    // ladder MAY throw (R2-ALLOC prepare_ordinary_deadline_locked, incl. the
    // test-only bad_alloc injection), and the catch below releases the pin —
    // await_resume never runs when await_suspend throws, so the caller
    // cannot share the release.
    //
    // PIN TRANSFER (FE-CORRECTIVE-1 P1-2): on every NON-throw return the pin
    // is TRANSFERRED to the caller (the coroutine awaiter). The fiber
    // frontend keeps its CallGuard alive on the suspended fiber STACK
    // through resume-side result conversion; the deferred frontend has no
    // such stack, so the awaiter holds the obligation and releases it in
    // await_resume AFTER the port-dependent conversion
    // (release_popped/release_failed validate owner_port_ against a LIVE
    // port). Releasing here — as the pre-corrective code did — opened a
    // window with every begin_teardown precondition satisfied while the
    // suspended continuation still needed the port.
    ++port.active_port_calls_;
    QueueAdmitDisposition disp;
    bool grant = false;
    try {
        LockGuard qlk(port.waiters_[0].mtx());
        disp = s.queue_push_admit_locked(port, lease, node,
                                         WaitResume::deferred(&record), timed,
                                         deadline);
        if (disp == QueueAdmitDisposition::authorized) {
            // Deferred-kind PublicationEligibility commit (FE-1b L7): in THIS
            // resolver-excluded critical section.
            record.arm();
        }
        grant = disp == QueueAdmitDisposition::resolved_inline_grant;
    } catch (...) {
        if (port.active_port_calls_ > 0) --port.active_port_calls_;
        throw;
    }
    // Q-LIV-1 grant (production authority; G + S only — the producer role
    // mutex was released above).
    if (grant) (void)s.queue_grant_consumer_locked(port);
    // NO pin release: the caller owns it now (see PIN TRANSFER above).
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
    // NOTE-DRIFT-COUPLING + PIN TRANSFER: see queue_push_core_ (same F.4
    // entry-interval mirror, exception-safe catch close, and pin transfer to
    // the awaiter on every non-throw return).
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
    // NO pin release: the caller owns it now.
    return disp == QueueAdmitDisposition::authorized;
}

void Scheduler::AsyncTestAccess::queue_release_deferred_pin_for_test(
    detail::QueuePort& port) {
    // Release the ordinary-call pin a deferred Queue admission transferred
    // to the awaiter (FE-CORRECTIVE-1 P1-2). Same synchronization domain as
    // the increment and the begin_teardown read: G + S (the production
    // CallGuard dtor's exact shape). The awaiter calls this from
    // await_resume with NO lock held, AFTER the port-dependent result
    // conversion (release_popped / release_failed) completed. An
    // over-release (counter already zero) is a lifecycle invariant
    // violation, not a tolerable drift.
    LockGuard glk(port.scheduler_.global_mtx_);
    LockGuard slk(port.state_mtx_);
    if (port.active_port_calls_ == 0) {
        detail::queue_lease_fail_fast();
    }
    --port.active_port_calls_;
}

// ---- FE-3 RwLock deferred-frontend seams ----
//
// Each entry stashes the frame-embedded RwWaitCtx (mode + ACTOR identity) on
// the WaitNode, runs the SHARED production ladder for a WaitResume::deferred
// token under G + W, and commits the PublicationEligibility (record.arm)
// INSIDE that resolver-excluded critical section on `authorized` (FE-1b L7).
// The write entries pass the caller's ActorId to the ladder so the inline
// claim (and, via ctx->actor, the grant claim) commits ACTOR ownership —
// never the ResumeTarget.

bool Scheduler::AsyncTestAccess::rwlock_read_deferred_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record) {
    return rwlock_read_core_(s, lock, node, actor_token, ctx, record,
                             /*timed=*/false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::rwlock_read_deferred_until_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline) {
    return rwlock_read_core_(s, lock, node, actor_token, ctx, record,
                             /*timed=*/true, deadline);
}

bool Scheduler::AsyncTestAccess::rwlock_write_deferred_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record) {
    return rwlock_write_core_(s, lock, node, actor_token, ctx, record,
                              /*timed=*/false, deadline_t{});
}

bool Scheduler::AsyncTestAccess::rwlock_write_deferred_until_for_test(
    Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
    RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline) {
    return rwlock_write_core_(s, lock, node, actor_token, ctx, record,
                              /*timed=*/true, deadline);
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
    // writer_active_ / writer_owner_ are G-serialized state; the observation
    // takes the same authority the resolvers use (FE-CORRECTIVE-1 P1-3: no
    // unsynchronized reads of ownership state remain, including test seams).
    LockGuard lk(lock.scheduler_.global_mtx_);
    return lock.writer_active_;
}

bool Scheduler::AsyncTestAccess::rwlock_owned_by_for_test(
    AsyncRwLock& lock, const void* actor_token) {
    LockGuard lk(lock.scheduler_.global_mtx_);
    return lock.writer_owner_ ==
           ActorId::frontend(const_cast<void*>(actor_token));
}

// ---- FE-3 Condition slice: deferred CONDITION-WAIT-PREPARE ----------------

bool Scheduler::AsyncTestAccess::condition_wait_deferred_for_test(
    Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
    WaitQueue& mutex_waiters, Fiber*& owner, FeDeferredRecord& record,
    bool& released) {
    return condition_wait_deferred_core_(s, cond_waiters, cond_node,
                                         mutex_waiters, owner, record,
                                         /*timed=*/false, deadline_t{},
                                         released);
}

bool Scheduler::AsyncTestAccess::condition_wait_deferred_until_for_test(
    Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
    WaitQueue& mutex_waiters, Fiber*& owner, deadline_t deadline,
    FeDeferredRecord& record, bool& released) {
    return condition_wait_deferred_core_(s, cond_waiters, cond_node,
                                         mutex_waiters, owner, record,
                                         /*timed=*/true, deadline, released);
}

bool Scheduler::AsyncTestAccess::condition_wait_deferred_core_(
    Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
    WaitQueue& mutex_waiters, Fiber*& owner, FeDeferredRecord& record,
    bool timed, deadline_t deadline, bool& released) {
    // One global_mtx_ CS for the shared ladder + the deferred
    // PublicationEligibility commit (FE-1b L7: the arm lands inside the
    // resolver-excluded CS, so a later resolver observes either
    // pre-registration (membership fail) or post-arm state — no lost wake).
    LockGuard lk(s.global_mtx_);
    const auto disp = s.condition_wait_admit_locked(
        cond_waiters, cond_node, WaitResume::deferred(&record),
        mutex_waiters, owner, timed, deadline);
    switch (disp) {
    case Scheduler::ConditionAdmitDisposition::authorized:
        released = true;  // the Mutex state was released/handed off
        record.arm();
        return true;  // the caller suspends; the body runs its OWN reacquire
    case Scheduler::ConditionAdmitDisposition::resolved_inline_released:
        released = true;  // resolved concurrently post-handoff; STILL reacquire
        return false;
    default:
        released = false;  // rejected / due-inline Expired: Mutex retained
        return false;
    }
}

}  // namespace sluice::async

#endif  // SLUICE_ASYNC_INTERNAL_TESTING
