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

#include <sluice/async/event.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "queue_detail.hpp"
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
        Scheduler::EventAdmitDisposition::authorized) {
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
        Scheduler::EventAdmitDisposition::authorized) {
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
    ++port.active_port_calls_;
    QueueAdmitDisposition disp;
    bool grant = false;
    {
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
    }
    // Q-LIV-1 grant (production authority; G + S only — the producer role
    // mutex was released above).
    if (grant) (void)s.queue_grant_consumer_locked(port);
    if (port.active_port_calls_ > 0) --port.active_port_calls_;
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
    {
        LockGuard qlk(port.waiters_[1].mtx());
        disp = s.queue_pop_admit_locked(port, out, node,
                                        WaitResume::deferred(&record), timed,
                                        deadline);
        if (disp == QueueAdmitDisposition::authorized) {
            record.arm();
        }
        grant = disp == QueueAdmitDisposition::resolved_inline_grant;
    }
    if (grant) (void)s.queue_grant_producer_locked(port);
    if (port.active_port_calls_ > 0) --port.active_port_calls_;
    return disp == QueueAdmitDisposition::authorized;
}

}  // namespace sluice::async

#endif  // SLUICE_ASYNC_INTERNAL_TESTING
