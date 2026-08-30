// Scheduler queue admit/grant seams — implementation TU split from
// scheduler.cpp (docs/post-freeze/structural-audit.md §6).
//
// The class declaration, lock domains, atomic orderings, and wake contracts
// remain in include/sluice/async/scheduler.hpp.
#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"
#include "queue_detail.hpp"  // QueueWaitCtx (shared with queue_port.cpp; non-installed)

#include <utility>
#include <cstdio>
#include <cstdlib>

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls in
// the non-installed test-control header so the phase call sites below resolve to
// the controller. In the production build this include is absent and the call
// sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {
// static
void Scheduler::queue_timer_on_resolve(void* owner_ctx,
                                       bool /*timer_won*/) noexcept {
    // The on-resolve thunk for a Queue-bound TimerRegistration. Decrements
    // `active_queue_timers_` exactly once. Idempotent under the caller's
    // global_mtx_ + the registration's single ACTIVE->terminal transition.
    // Static-member form so it can reach QueuePort's private counter via
    // Scheduler's friend grant.
    auto* port = static_cast<detail::QueuePort*>(owner_ctx);
    if (port == nullptr) return;
    if (port->active_queue_timers_ > 0) --port->active_queue_timers_;
}


// ---- FE shared Queue admission ladders (FE-3) ------------------------------
//
// ONE textual admission sequence per direction, shared by EVERY frontend
// (stackful Fiber admit / deferred awaiter). The ladder is pure admission
// law: it touches no Fiber, no g_worker, and no coroutine knowledge — only
// the ResumeTarget token. The ENTRY owns its frontend-specific
// PublicationEligibility commit (fiber: commit_suspend_locked; deferred:
// frontend record arm) inside the SAME critical section when the ladder
// returns `authorized`, then suspends physically outside the lock (FE-1b
// L7/L10). Inline dispositions publish nothing — the entry never suspended
// (L6); resolved_inline_grant additionally obliges the entry to run the
// Q-LIV-1 opposite-role grant under G + S after the role mutex is released
// (the two role mutexes are NEVER held together).

Scheduler::QueueAdmitDisposition Scheduler::queue_push_admit_locked(
    detail::QueuePort& port, detail::QueueItemLease& lease, WaitNode& node,
    const WaitResume& resume, bool timed, deadline_t deadline)
    SLUICE_REQUIRES(global_mtx_) {
    // Push ladder. Caller holds G + S + producer.mtx().
    detail::QueueItemControl* c = lease.control_;
    TimerRegistration* reg = nullptr;
    if (timed) {
        // R2-ALLOC: allocations (heap slot reserve + pool node) BEFORE any
        // admission state mutation — a bad_alloc here leaves the node
        // Detached, the port counters untouched, and no timer orphan.
        reg = prepare_ordinary_deadline_locked(&node, &port.waiters_[0],
                                               deadline);
    }
    if (!port.waiters_[0].register_wait_locked(node, resume)) {
        if (timed) erase_popped_registration_locked(reg);  // never published
        return QueueAdmitDisposition::rejected;  // registration contract violation
    }
    ++port.active_wait_associations_;
    ++waiting_waitq_count_;
    if (timed) {
        // Intentionally LOCAL publish (AC-2b review corrective): NOT routed
        // through publish_ordinary_deadline_locked. The historical order
        // bumps active_queue_timers_ BEFORE the ACTIVE count / heap / cache
        // publication, and earliest_active_deadline_ is an atomic read by
        // parked workers WITHOUT global_mtx_ — so this interleaving is
        // externally observable in principle and is preserved verbatim
        // (only the allocation phase above was split out; the heap push
        // consumes the capacity reserved by prepare, allocation-free).
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;  // timer bookkeeping
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
    }
    // Admission precedence 1: resource admissible (Open + space + FIFO head)
    // => commit + resolve inline (the common no-contention case; the
    // reconciler path handles the rest).
    if (!port.closed_ && !port.ring_full_locked() && node.prev_ == nullptr) {
        c->location_ = detail::QueueItemControl::Location::ring;
        const std::size_t tail =
            (port.ring_head_ + port.ring_count_) % port.capacity_;
        port.ring_[tail] = std::move(lease);  // caller lease now empty
        ++port.ring_count_;
        port.waiters_[0].wake_node_locked(node);
        if (timed) {
            // ACTIVE->RETIRED via the ordinary deadline authority.
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
        }
        if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
        // Q-LIV-1: the slot-occupancy change must reconcile the parked
        // consumer head before this op returns; the grant runs in the ENTRY,
        // after the producer role mutex is released (the two role mutexes
        // are NEVER held together).
        return QueueAdmitDisposition::resolved_inline_grant;
    }
    // Closed at admission: resolve Woken with the lease retained.
    if (port.closed_) {
        port.waiters_[0].wake_node_locked(node);
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
        }
        if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
        return QueueAdmitDisposition::resolved_inline;  // lease retained
    }
    if (timed) {
        // Admission precedence 2: already-due => Expired inline (a losing
        // expire CAS falls through to suspension authorization, as before).
        if (clock_now_unlocked() >= deadline &&
            port.waiters_[0].expire_locked(node)) {
            // ACTIVE->CONSUMED via the ordinary deadline authority; the
            // already-due inline path keeps its immediate cache recompute.
            (void)consume_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/true);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return QueueAdmitDisposition::resolved_inline;  // expired; lease retained
        }
    }
    // Suspension authorized. The epoch is Registered, un-resolved, and every
    // resolver is excluded until THIS critical section releases; the entry
    // commits its PublicationEligibility now (contract L7) and suspends
    // physically afterwards (L10).
    return QueueAdmitDisposition::authorized;
}

Scheduler::QueueAdmitDisposition Scheduler::queue_pop_admit_locked(
    detail::QueuePort& port, detail::QueueItemLease& out, WaitNode& node,
    const WaitResume& resume, bool timed, deadline_t deadline)
    SLUICE_REQUIRES(global_mtx_) {
    // Pop ladder. Symmetric. Caller holds G + S + consumer.mtx(). Inline
    // admission pops the OLDEST ring item if non-empty + FIFO head (open OR
    // closed — remaining items stay poppable after close); closed+empty is
    // the terminal disposition.
    TimerRegistration* reg = nullptr;
    if (timed) {
        // R2-ALLOC: allocations before any admission state mutation — see
        // queue_push_admit_locked.
        reg = prepare_ordinary_deadline_locked(&node, &port.waiters_[1],
                                               deadline);
    }
    if (!port.waiters_[1].register_wait_locked(node, resume)) {
        if (timed) erase_popped_registration_locked(reg);  // never published
        return QueueAdmitDisposition::rejected;  // registration contract violation
    }
    ++port.active_wait_associations_;
    ++waiting_waitq_count_;
    if (timed) {
        // Intentionally LOCAL publish (AC-2b review corrective) — see the
        // push ladder. The consume/retire transitions below DO route through
        // the authority (uniform facts).
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;  // timer bookkeeping
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
    }
    // Admission precedence 1: item admissible (non-empty ring + FIFO head).
    if (!port.ring_empty_locked() && node.prev_ == nullptr) {
        const std::size_t head = port.ring_head_;
        out = std::move(port.ring_[head]);
        port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
        --port.ring_count_;
        out.control_->location_ =
            detail::QueueItemControl::Location::consumer_operation;
        port.waiters_[1].wake_node_locked(node);
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
        }
        if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
        // Q-LIV-1: the freed slot must reconcile the parked producer head
        // before this op returns; the grant runs in the ENTRY, after the
        // consumer role mutex is released (the two role mutexes are NEVER
        // held together).
        return QueueAdmitDisposition::resolved_inline_grant;
    }
    // Closed + empty: resolve Woken with `out` empty (the caller returns
    // closed).
    if (port.ring_empty_locked() && port.closed_) {
        port.waiters_[1].wake_node_locked(node);
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
        }
        if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
        return QueueAdmitDisposition::resolved_inline;
    }
    if (timed) {
        // Already-due => Expired inline (a losing expire CAS falls through
        // to suspension authorization, as before).
        if (clock_now_unlocked() >= deadline &&
            port.waiters_[1].expire_locked(node)) {
            (void)consume_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/true);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return QueueAdmitDisposition::resolved_inline;  // expired; out stays empty
        }
    }
    return QueueAdmitDisposition::authorized;
}

// FE winner publication for the Queue grant seams (the ONE textual tail; see
// scheduler.hpp for the granted_not_resumed_ deferred-kind rationale).
void Scheduler::queue_publish_winner_locked(detail::QueuePort& port,
                                            WaitNode& won)
    SLUICE_REQUIRES(global_mtx_) {
    const WaitResume& r = won.resume();
    switch (r.kind()) {
    case WaitResume::Kind::fiber: {
        Fiber* f = r.as_fiber();
        if (f->make_runnable()) {  // publication LAST
            ++port.granted_not_resumed_;  // published suspended-winner ticket
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
    // Blocking push — stackful (Fiber) frontend entry over the SHARED push
    // ladder. Per-op context: the caller stashes a QueueWaitCtx* on the
    // WaitNode (node.set_user) BEFORE registering; on a Fiber the ctx (and
    // the caller's lease) live on the fiber stack, which persists across the
    // context switch. The reconciler reads won->user() after the grant seam
    // returns the winner. On resume the caller reads lease.control_: null =>
    // committed (ring owns it); non-null => closed/cancelled/expired (caller
    // returns the lease).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::push requires a running Fiber");
    Fiber* me = ws->current;
    detail::QueueItemControl* c = lease.control_;
    assert(c != nullptr && c->location_ ==
           detail::QueueItemControl::Location::producer_operation);
    QueueWaitCtx ctx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        QueueAdmitDisposition disp;
        {
            LockGuard qlk(port.waiters_[0].mtx());
            disp = queue_push_admit_locked(port, lease, node,
                                           WaitResume::fiber(me),
                                           /*timed=*/false, deadline_t{});
        }
        // Q-LIV-1 liveness reconcile: an inline success changed ring
        // occupancy — exactly the resource transition that makes the opposite
        // role's FIFO head eligible — so grant that head BEFORE returning, the
        // same authority try_push's FastPushCommit performs. Held locks here
        // are G + S only (identical to the try_push FastPopCommit critical
        // section); every Queue actor that could touch either role FIFO or the
        // ring needs G, so no third party can interleave in this window.
        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_consumer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspended
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7): same critical
        // section, after authorization.
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
    // This fiber has now RESUMED from a suspended-winner
    // publication. Decrement the per-port `granted_not_resumed_` counter under
    // G (the publication side incremented it under G in the grant seam).
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
    // On resume the reconciler already finalized: lease.control_==null means
    // committed, non-null means closed/expired/cancelled.
}

void Scheduler::queue_pop_admit(detail::QueuePort& port, WaitNode& node,
                                detail::QueueItemLease& out) {
    // Blocking pop — stackful frontend entry over the SHARED pop ladder.
    // Symmetric. The reconciler (a producer's try_push that added an item, or
    // close) moves a ring item into `out`.
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
            disp = queue_pop_admit_locked(port, out, node,
                                          WaitResume::fiber(me),
                                          /*timed=*/false, deadline_t{});
        }
        // Q-LIV-1 liveness reconcile: identical authority and lock position
        // to the push entry above.
        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_producer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspended
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7).
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
    // This fiber has RESUMED from a suspended-winner
    // publication. Decrement granted_not_resumed_ under G.
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
}

void Scheduler::queue_push_admit_until(detail::QueuePort& port, WaitNode& node,
                                       detail::QueueItemLease& lease,
                                       deadline_t deadline) {
    // Timed push — stackful frontend entry over the SHARED push ladder with
    // the ordinary deadline authority (timed=true): the SAME
    // prepare/publish/already-due/retire lifecycle the blocking entry uses
    // through the ladder. Composes the resource-first precedence (an inline
    // commit wins over a due deadline) with the already-due inline-Expired
    // closure. On expired the lease is retained (caller returns expired).
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
            disp = queue_push_admit_locked(port, lease, node,
                                           WaitResume::fiber(me),
                                           /*timed=*/true, deadline);
        }
        // Q-LIV-1 liveness reconcile (timed push): identical authority and
        // lock position to the untimed entry above.
        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_consumer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspended
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7).
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
    // This fiber has RESUMED from a suspended-winner
    // publication (or a timer-expiry publication). Decrement
    // granted_not_resumed_ under G.
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
}

void Scheduler::queue_pop_admit_until(detail::QueuePort& port, WaitNode& node,
                                      detail::QueueItemLease& out,
                                      deadline_t deadline) {
    // Timed pop — stackful frontend entry over the SHARED pop ladder. Symmetric.
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
            disp = queue_pop_admit_locked(port, out, node,
                                          WaitResume::fiber(me),
                                          /*timed=*/true, deadline);
        }
        // Q-LIV-1 liveness reconcile (timed pop): identical authority and
        // lock position to the untimed entry above.
        if (disp == QueueAdmitDisposition::resolved_inline_grant) {
            (void)queue_grant_producer_locked(port);
        }
        if (disp != QueueAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspended
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7).
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
    // F.2 corrective: this fiber has RESUMED from a suspended-winner
    // publication (or a timer-expiry publication). Decrement
    // granted_not_resumed_ under G.
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
}

bool Scheduler::queue_cancel(detail::QueuePort& port, detail::QueueRole role,
                             WaitNode& node) {
    // Queue-identity-safe cancellation. Mirrors mutex_cancel. Resolves the
    // node Cancelled ONLY if Registered + linked in this port's role FIFO +
    // CANCEL CAS wins. Safe from any OS thread; no ring/lease mutation (the
    // caller retains its lease / empty out on cancel). The publication edge
    // switches on the winner's ResumeTarget kind (fiber route / deferred
    // obligation / none); its result does NOT undo the cancel.
    LockGuard lk(global_mtx_);
    const std::size_t roleIdx = static_cast<std::size_t>(role);
    LockGuard qlk(port.waiters_[roleIdx].mtx());
    if (!cancel_primitive_wait_locked(port.waiters_[roleIdx], node)) return false;
    if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    publish_wait_winner_locked(node);
    return true;
}

WaitNode* Scheduler::queue_grant_consumer_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    // Reconciler (producer-arrived or close-draining): grant the consumer FIFO
    // head the OLDEST ring item, winner-before-publication. Mirrors
    // mutex_handoff_one_locked's resolve -> commit -> retire -> publish order.
    // Caller holds G + S; we take consumer.mtx() here (under G).
    LockGuard qlk(port.waiters_[1].mtx());
    WaitNode* won = port.waiters_[1].wake_one_locked();  // resolve FIFO head Woken + unlink
    if (won == nullptr) return nullptr;  // no consumer parked / head lost
    auto* ctx = static_cast<QueueWaitCtx*>(won->user());
    // ---- retire BEFORE commit (§12 verbatim order) ----
    retire_timer_for_node_locked(*won);  // timer-lifetime closure (fires the on-resolve thunk if Queue timer)
    // ---- resource commit BEFORE publication ----
    if (!port.ring_empty_locked()) {
        const std::size_t head = port.ring_head_;
        *ctx->cons_out = std::move(port.ring_[head]);
        port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
        --port.ring_count_;
        ctx->cons_out->control_->location_ =
            detail::QueueItemControl::Location::consumer_operation;
    }  // else: ring empty (close race) => leave out empty; caller returns closed
    if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Publication LAST: the ONE textual winner-kind tail (fiber stackful
    // route verbatim / deferred delivery obligation / none).
    queue_publish_winner_locked(port, *won);
    return won;
}

WaitNode* Scheduler::queue_grant_producer_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    // Reconciler (consumer-freed-a-slot or close-draining): commit the producer
    // FIFO head's lease into a freed ring slot (or, if Closed, resolve it Woken
    // with the lease retained). Caller holds G + S; we take producer.mtx().
    // The producer admit stashed BOTH the control pointer AND a pointer to its
    // stack lease (QueueWaitCtx::prod_lease) so the grant can move the lease
    // whole into the slot in this critical section.
    LockGuard qlk(port.waiters_[0].mtx());
    WaitNode* won = port.waiters_[0].wake_one_locked();
    if (won == nullptr) return nullptr;
    auto* ctx = static_cast<QueueWaitCtx*>(won->user());
    // ---- retire BEFORE commit (§12 verbatim order) ----
    retire_timer_for_node_locked(*won);
    // ---- resource commit BEFORE publication ----
    if (!port.closed_ && !port.ring_full_locked()) {
        detail::QueueItemControl* c = ctx->prod_control;
        c->location_ = detail::QueueItemControl::Location::ring;
        const std::size_t tail =
            (port.ring_head_ + port.ring_count_) % port.capacity_;
        port.ring_[tail] = std::move(*ctx->prod_lease);  // winner lease -> slot
        ++port.ring_count_;
    }  // else: Closed (or race-full) => leave producer's lease retained; the
       // producer resume returns it as closed.
    if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Publication LAST: the ONE textual winner-kind tail (fiber stackful
    // route verbatim / deferred delivery obligation / none).
    queue_publish_winner_locked(port, *won);
    return won;
}

bool Scheduler::queue_role_waiters_empty_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    // Teardown precondition query. Caller holds global_mtx_; we take each
    // role mtx() SEQUENTIALLY under G (the canonical G -> exactly-one-role
    // lock order — the two role mutexes are NEVER held together). The
    // QueuePort is not a friend of WaitQueue (only the Scheduler is), so this
    // is the sole authority for "no Queue wait epoch is registered". Both
    // FIFOs must be empty for begin_teardown to perform the irreversible
    // operational -> tearing_down transition.
    {
        LockGuard qlk(port.waiters_[0].mtx());  // producer FIFO
        if (!port.waiters_[0].empty_locked()) return false;
    }
    {
        LockGuard qlk(port.waiters_[1].mtx());  // consumer FIFO
        if (!port.waiters_[1].empty_locked()) return false;
    }
    return true;
}

// AsyncQueue private seams.
//
// Blocking/timed wait admission + reconciliation. A QueuePort owns a producer
// and a consumer WaitQueue (waiters_[2]); the Scheduler is the authoritative
// resolution + publication executor, as for the other wait primitives. Lock order:
// G (global_mtx_) -> S (QueuePort::state_mtx_) -> exactly one role mtx();
// the two role mutexes are NEVER held together.
//
// DESIGN (atomic reconciler commit, single suspend). The reconciler is the
// OTHER role's resource-changing success — EITHER its fast-path success
// (try_push/try_pop) OR its blocking/timed admit's inline success (Q-LIV-1).
// E.g. a producer's try_push that commits an item to the ring WAKES the
// consumer FIFO head via wake_wait_one_locked and, in the SAME G + S +
// role.mtx() critical section, moves the just-committed item into that
// specific consumer's out-lease (read via won->user()). The consumer's admit
// did a SINGLE register + suspend; on wake its out-lease is already non-empty
// (item granted) — no re-check loop, no per-node reuse problem. The producer
// direction is symmetric: a consumer's try_pop that opened a slot wakes the
// producer FIFO head and commits the producer's lease into the freed slot.
// The blocking/timed admit inline successes perform the same reconcile (their
// own role mutex released first — the two role mutexes are NEVER held
// together; the grant then runs under G + S, the same lock shape as the fast
// paths). close() wakes every parked producer (closed outcome — lease
// retained) and every parked consumer (pop remaining items, else closed).
//
// Per-operation context: the admit caller stashes a QueueWaitCtx* on the
// WaitNode (node.set_user) BEFORE registering. The ctx carries the producer
// control pointer (push) or the consumer out-lease address (pop). The
// reconciler reads won->user() after wake_one_locked returns the winner.
//
// Winner-before-publication: the resolve_(Woken) CAS + the resource commit +
// timer retire all happen in the SAME G + S + role critical section, BEFORE
// make_runnable/route_runnable_locked (publication). A woken Fiber observes
// the final state on resume.
//
// Queue timer bookkeeping (Corrective-2 §8 supersession). The
// non-template QueuePort owns TWO per-port counters that bracket a Queue
// timed wait:
//   - `active_wait_associations_` (incremented on every successful
//     registration; decremented on every resolution path — inline admit,
//     grant seam, queue_cancel, AND the pump-driven timer expiry).
//   - `active_queue_timers_` (incremented at timer registration; decremented
//     when the timer is consumed or retired).
// `active_wait_associations_` is decremented manually at each resolution
// site that can name the port (the four admit seams, the two grant seams,
// queue_cancel). The pump_deadlines_locked path cannot otherwise reach the
// port, so for a Queue-bound registration it uses the registration's
// `owner_ctx_` to perform the `--active_wait_associations_` decrement.
// `active_queue_timers_` is decremented via the on-resolve thunk installed
// on the TimerRegistration (fired by pump on consume and by
// retire_timer_for_node_locked on retire); this keeps the timer-counter
// bookkeeping localized to the timer's ACTIVE->terminal transition.
// Non-Queue waits leave the hook null and the Scheduler's default
// `--waiting_waitq_count_` accounting applies unchanged.
//
// The §8 PreparedQueueTimer/prepare/activate/discard substrate is
// SUPERSEDED by this minimal model.
//
}  // namespace sluice::async
