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


void Scheduler::queue_push_admit(detail::QueuePort& port, WaitNode& node,
                                 detail::QueueItemLease& lease) {
    // Blocking push. Register on the producer FIFO under G + S +
    // producer.mtx(); admission recheck commits inline if admissible (Open +
    // space + FIFO head) — else suspend. The reconciler (a consumer's try_pop
    // that freed a slot, or close) commits the lease into a ring slot in the
    // same critical section as the resolve CAS. On resume the caller reads
    // lease.control_: null => committed (ring owns it); non-null => closed/
    // cancelled/expired (caller returns the lease).
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
        LockGuard qlk(port.waiters_[0].mtx());
        if (!port.waiters_[0].register_wait_locked(node, me)) {
            return;  // registration contract violation
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        // Admission recheck: Open + space + FIFO head => commit inline (the
        // common no-contention case; the reconciler path handles the rest).
        if (!port.closed_ && !port.ring_full_locked() && node.prev_ == nullptr) {
            c->location_ = detail::QueueItemControl::Location::ring;
            const std::size_t tail =
                (port.ring_head_ + port.ring_count_) % port.capacity_;
            port.ring_[tail] = std::move(lease);  // caller lease now empty
            ++port.ring_count_;
            port.waiters_[0].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // committed inline
        }
        // Closed at admission: resolve Woken with the lease retained.
        if (port.closed_) {
            port.waiters_[0].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // lease retained; caller returns closed
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
    // Blocking pop. Symmetric. Admission recheck pops inline if the ring is
    // non-empty + FIFO head; else suspend. The reconciler (a producer's
    // try_push that added an item, or close) moves a ring item into `out`.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::pop requires a running Fiber");
    Fiber* me = ws->current;
    QueueWaitCtx ctx{&port, detail::QueueRole::consumer, nullptr, nullptr, &out};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        LockGuard qlk(port.waiters_[1].mtx());
        if (!port.waiters_[1].register_wait_locked(node, me)) {
            return;  // registration contract violation
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        if (!port.ring_empty_locked() && node.prev_ == nullptr) {
            const std::size_t head = port.ring_head_;
            out = std::move(port.ring_[head]);
            port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
            --port.ring_count_;
            out.control_->location_ =
                detail::QueueItemControl::Location::consumer_operation;
            port.waiters_[1].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // item granted inline
        }
        if (port.ring_empty_locked() && port.closed_) {
            port.waiters_[1].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // closed+empty
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
    // Timed push. Composes queue_push_admit with timer registration and
    // the already-due inline-Expired precedence (resource-first). On expired
    // the lease is retained (caller returns expired).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::push_until requires a running Fiber");
    Fiber* me = ws->current;
    detail::QueueItemControl* c = lease.control_;
    QueueWaitCtx ctx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        LockGuard qlk(port.waiters_[0].mtx());
        if (!port.waiters_[0].register_wait_locked(node, me)) {
            return;  // registration contract violation
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        // Intentionally LOCAL arming (AC-2b review corrective): NOT routed
        // through arm_ordinary_deadline_locked. The historical order bumps
        // active_queue_timers_ BEFORE the ACTIVE count / heap / cache
        // publication, and earliest_active_deadline_ is an atomic read by
        // parked workers WITHOUT global_mtx_ — so this interleaving is
        // externally observable in principle and is preserved verbatim.
        timer_pool_.emplace_back(&node, &port.waiters_[0], deadline);
        reg = &timer_pool_.back();
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;  // timer bookkeeping
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
        // Admission precedence 1: resource admissible => commit + resolve.
        if (!port.closed_ && !port.ring_full_locked() && node.prev_ == nullptr) {
            c->location_ = detail::QueueItemControl::Location::ring;
            const std::size_t tail =
                (port.ring_head_ + port.ring_count_) % port.capacity_;
            port.ring_[tail] = std::move(lease);
            ++port.ring_count_;
            port.waiters_[0].wake_node_locked(node);
            // ACTIVE->RETIRED via the ordinary deadline authority.
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        // Closed => resolve (lease retained).
        if (port.closed_) {
            port.waiters_[0].wake_node_locked(node);
            // ACTIVE->RETIRED via the ordinary deadline authority.
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        // Admission precedence 2: already-due => Expired inline.
        if (clock_now_unlocked() >= deadline) {
            if (port.waiters_[0].expire_locked(node)) {
                // ACTIVE->CONSUMED via the ordinary deadline authority; the
                // already-due inline path keeps its immediate cache recompute.
                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                reg->fire_on_resolve_locked(/*timer_won=*/true);
                if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                return;  // expired; lease retained
            }
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
    // Timed pop. Symmetric.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::pop_until requires a running Fiber");
    Fiber* me = ws->current;
    QueueWaitCtx ctx{&port, detail::QueueRole::consumer, nullptr, nullptr, &out};
    node.set_user(&ctx);
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        LockGuard qlk(port.waiters_[1].mtx());
        if (!port.waiters_[1].register_wait_locked(node, me)) {
            return;  // registration contract violation
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        // Intentionally LOCAL arming (AC-2b review corrective) — see
        // queue_push_admit_until for why Queue does not route through
        // arm_ordinary_deadline_locked. The consume/retire transitions below
        // DO go through the authority (uniform facts).
        timer_pool_.emplace_back(&node, &port.waiters_[1], deadline);
        reg = &timer_pool_.back();
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;  // timer bookkeeping
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
        if (!port.ring_empty_locked() && node.prev_ == nullptr) {
            const std::size_t head = port.ring_head_;
            out = std::move(port.ring_[head]);
            port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
            --port.ring_count_;
            out.control_->location_ =
                detail::QueueItemControl::Location::consumer_operation;
            port.waiters_[1].wake_node_locked(node);
            // ACTIVE->RETIRED via the ordinary deadline authority.
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        if (port.ring_empty_locked() && port.closed_) {
            port.waiters_[1].wake_node_locked(node);
            // ACTIVE->RETIRED via the ordinary deadline authority.
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        if (clock_now_unlocked() >= deadline) {
            if (port.waiters_[1].expire_locked(node)) {
                // ACTIVE->CONSUMED via the ordinary deadline authority; the
                // already-due inline path keeps its immediate cache recompute.
                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                reg->fire_on_resolve_locked(/*timer_won=*/true);
                if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                return;  // expired; out stays empty
            }
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
    // caller retains its lease / empty out on cancel).
    LockGuard lk(global_mtx_);
    const std::size_t roleIdx = static_cast<std::size_t>(role);
    LockGuard qlk(port.waiters_[roleIdx].mtx());
    if (!cancel_primitive_wait_locked(port.waiters_[roleIdx], node)) return false;
    Fiber* f = node.fiber();
    if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr) {
        publish_waiting_fiber_runnable_locked(f);
    }
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
    Fiber* f = won->fiber();
    // Route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr && f->make_runnable()) {  // publication LAST
        ++port.granted_not_resumed_;  // published suspended-winner ticket
        WorkerState* owner = owner_for_fiber_locked(f);
        route_runnable_locked(f, owner);
    }
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
    Fiber* f = won->fiber();
    // Route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr && f->make_runnable()) {
        ++port.granted_not_resumed_;  // published suspended-winner ticket
        WorkerState* owner = owner_for_fiber_locked(f);
        route_runnable_locked(f, owner);
    }
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
// OTHER role's fast-path success: e.g. a producer's try_push that commits an
// item to the ring WAKES the consumer FIFO head via wake_wait_one_locked and,
// in the SAME G + S + role.mtx() critical section, moves the just-committed
// item into that specific consumer's out-lease (read via won->user()). The
// consumer's admit did a SINGLE register + suspend; on wake its out-lease is
// already non-empty (item granted) — no re-check loop, no per-node reuse
// problem. The producer direction is symmetric: a consumer's try_pop that
// opened a slot wakes the producer FIFO head and commits the producer's lease
// into the freed slot. close() wakes every parked producer (closed outcome —
// lease retained) and every parked consumer (pop remaining items, else
// closed).
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
