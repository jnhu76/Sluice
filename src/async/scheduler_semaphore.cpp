// Scheduler Semaphore primitive — implementation TU split from scheduler.cpp
// (docs/post-freeze/structural-audit.md §6).
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
bool Scheduler::sem_try_acquire(WaitQueue& waiters,
                                std::atomic<std::uint32_t>& available) {
    // Lock-free try_acquire is FORBIDDEN: it would bypass the FIFO queue and
    // admit a newcomer ahead of an eligible head (barging). The authoritative
    // decision is made under global_mtx_ + waiters_.mtx(), the SAME domain every
    // release / cancel / expire / admission path takes. `available` is atomic
    // ONLY to support lock-free observation via Semaphore::available(); it does
    // NOT authorize lock-free acquisition.
    //
    // No barging: if a waiter is already queued (eligible FIFO head exists),
    // try_acquire MUST fail even when available_ > 0 — a newcomer may not bypass
    // the queued waiter's priority. This preserves the stable-state invariant
    // (EligibleQueuedWaiterExists => available_ == 0): an eligible waiter is
    // never left stranded while a stored permit exists.
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    if (waiters.empty_locked()) {
        const std::uint32_t cur = available.load(std::memory_order::acquire);
        if (cur > 0) {
            available.store(cur - 1, std::memory_order::release);
            return true;
        }
    }
    return false;  // no stored permit, OR an eligible queued waiter has priority
}

void Scheduler::sem_acquire(WaitQueue& waiters,
                            std::atomic<std::uint32_t>& available,
                            WaitNode& node) {
    // Semaphore acquire admission. The lost-wake closure: register + recheck
    // admission + commit suspension — all under one global_mtx_ + waiters_.mtx()
    // critical section (the same domain release / cancel / expire / admission
    // use). Only context_switch is outside the lock. This mirrors
    // await_event_wait's lost-set closure (the canonical lost-wake-closed idiom)
    // but the admission predicate is "a stored permit is
    // admissible to THIS newly-registered FIFO head" instead of "SET observed".
    //
    // The admission window is closed: a permit observed during the admission
    // critical section leads to inline Woken, not a sleeping registered waiter
    // with stored supply. The trace
    //     initial check sees no permit
    //     waiter registers
    //     release occurs
    //     waiter attempts suspension
    // cannot strand the waiter because release() takes global_mtx_ to transfer
    // its permit, and this admission recheck runs under the same global_mtx_
    // after registration — release either completed before registration (its
    // transfer targeted the prior head / stored the permit, which the recheck
    // observes) or runs after this critical section (it sees this registered
    // node and transfers to it).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            // Node already registered or terminal: contract violation.
            return;
        }
        ++waiting_waitq_count_;

        // Admission recheck: a stored permit is admissible to THIS node only if
        // it is the FIFO head (no earlier waiter has priority). register_wait_
        // locked links at the FIFO TAIL, so this node is the head iff it has no
        // predecessor (node.prev_ == nullptr). The link field prev_ is read here
        // while holding waiters_.mtx() (the structural authority). wake_node_
        // locked resolves THIS specific node with Woken, so we target it
        // directly — there is no head-identity ambiguity at the resolve CAS.
        //
        // If an earlier waiter is queued (node.prev_ != nullptr), this node MUST
        // NOT consume a stored permit even when available_ > 0: the earlier
        // waiter has FIFO priority. The stable-state invariant
        // (EligibleQueuedWaiterExists => available_ == 0) holds in production
        // because a release transfers to the head rather than storing when a
        // waiter is queued, so available_ > 0 with a queued earlier waiter is a
        // transient that this check correctly refuses to admit.
        if (node.prev_ == nullptr &&
            available.load(std::memory_order::acquire) > 0) {
            if (waiters.wake_node_locked(node)) {
                available.store(available.load(std::memory_order::acquire) - 1,
                                std::memory_order::release);
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING (it has not called
                // make_waiting()) and continues inline without suspending;
                // no runnable publication is needed.
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // Defense-in-depth: if the node was resolved concurrently (it cannot be,
        // since register_wait_locked just moved it to Registered under both
        // locks and every resolver takes global_mtx_), undo and do not suspend.
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
    // Deadline-aware acquire. Composes sem_acquire's admission closure
    // with TimerRegistration. The wait resolves when EXACTLY ONE cause wins
    // the resolve_ CAS:
    //   - release() transfer (sem_release -> wake_wait_one_locked) -> Woken
    //   - cancel(node) (sem_cancel)                              -> Cancelled
    //   - the deadline elapsing (pump_deadlines_locked)          -> Expired
    //
    // Admission precedence (A4, under global_mtx_ + waiters_.mtx()):
    //   1. If a permit is admissible (available > 0 AND node is FIFO head):
    //      resolve Woken inline (no suspend). Permit admission wins over a due
    //      deadline (the resource is ready; the deadline is moot).
    //   2. Else if the deadline is already due: resolve Expired inline (the
    //      already-due closure).
    //   3. Else: commit suspension.
    // A non-timer winner retires the registration in the same CS (the
    // timer-lifetime closure).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            return;  // registration contract violation
        }
        ++waiting_waitq_count_;
        // Arm the timer registration control block for this wait epoch (pool
        // construction + ACTIVE count + heap push + park-cache refresh).
        reg = arm_ordinary_deadline_locked(&node, &waiters, deadline);

        // Admission precedence 1: permit admission wins over a due deadline. If
        // a stored permit is available AND this node is the FIFO head (no earlier
        // waiter has priority — node.prev_ == nullptr, read under waiters_.mtx()),
        // resolve Woken inline and retire the timer (the deadline is moot).
        if (node.prev_ == nullptr &&
            available.load(std::memory_order::acquire) > 0) {
            if (waiters.wake_node_locked(node)) {
                available.store(
                    available.load(std::memory_order::acquire) - 1,
                    std::memory_order::release);
                // ACTIVE->RETIRED via the ordinary deadline authority (no
                // Semaphore on_resolve hook exists; count decrement inside).
                (void)retire_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // Fiber is RUNNING and continues inline; no publication.
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // Admission precedence 2: already-due closure — if the deadline is
        // ALREADY due (and
        // no permit is admissible), resolve Expired inline. The fiber must NOT
        // suspend and wait for a future timer scan merely because registration
        // happened after the deadline was due.
        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                // ACTIVE->CONSUMED via the ordinary deadline authority; the
                // already-due inline path keeps its immediate cache recompute.
                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // Fiber is RUNNING and continues inline; no publication.
                return;  // resolved at admission; do NOT suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through.
        }

        // Defense-in-depth: if the node was resolved concurrently, undo + return.
        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            (void)retire_ordinary_deadline_locked(*reg);  // ACTIVE->RETIRED
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
    // Queue-identity-safe cancellation. Mirrors event_cancel_wait exactly.
    // Semaphore::cancel passes its private waiters_ here (NOT exposed to the
    // caller). The contract:
    //   returns true ONLY if node is currently Registered AND currently linked
    //   in THIS Semaphore's private WaitQueue AND CANCEL wins node.resolve_.
    //   Otherwise returns false WITHOUT mutation.
    //
    // The membership check scans THIS queue's own intrusive list for &node
    // while holding this Scheduler's global_mtx_ + this Semaphore's
    // waiters_.mtx(). It does NOT read a foreign node's home_, does NOT lock a
    // foreign Semaphore or foreign Scheduler. Wrong-Semaphore (same OR different
    // Scheduler), detached, Woken, Expired, and Cancelled nodes all return false
    // safely. This call CANNOT synthesize a RESOURCE_WAKE and CANNOT change
    // available_.
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    // Membership gate: a node not linked in THIS queue is not cancellable here.
    if (!cancel_primitive_wait_locked(waiters, node)) return false;
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr) {
        publish_waiting_fiber_runnable_locked(f);
    }
    return true;
}

bool Scheduler::sem_release(WaitQueue& waiters,
                            std::atomic<std::uint32_t>& available,
                            std::uint32_t max_permits) {
    // Release disposition. Under global_mtx_, this call contributes
    // exactly ONE pending permit. The disposition is exactly one of:
    //   - transferred to the FIFO head waiter (available_ UNCHANGED)
    //   - stored into available_ (available_++)
    //   - rejected: queue empty AND available_ == max_permits_ (no mutation)
    //
    // Transfer branch: wake_wait_one_locked takes global_mtx_ (held here) and
    // waiters_.mtx() (acquired INSIDE it), resolves the FIFO head with Woken,
    // and routes the winner. By Conclusion A, a linked FIFO head observed under
    // global_mtx_ + waiters_.mtx() is Registered and eligible; its
    // resolve_(Woken) cannot lose, and wake_wait_one_locked returns nullptr ONLY
    // when the queue is empty. Therefore a non-empty queue produces exactly one
    // winner and the transfer branch returns true with available_ UNCHANGED.
    //
    // Empty-queue branch: wake_wait_one_locked returned nullptr (queue empty at
    // the moment of the check). Store the permit, or reject overflow. A later
    // acquire that registers after this store is handled by its admission
    // recheck (which observes the stored permit and resolves Woken inline), so
    // no permit is stranded. available_ is bounded by max_permits_; no mutation
    // on overflow.
    //
    // Forbidden shapes (NOT present): available_-- before waking a waiter;
    // refund after a lost wake; reserve-then-commit; a grant-in-flight field;
    // increment available_ AND wake a waiter in one release; retry after a null
    // wake; skip-after-null. A queued grant from available_ == 0 succeeds
    // without decrement or integer underflow (the permit is transferred, not
    // withdrawn and re-deposited).
    //
    // Safe to call from an external OS thread: g_worker is null on a non-worker
    // thread, so route_runnable_locked routes the winner through pending_spawn_
    // and signal_wake_locked wakes a parked Scheduler worker — exactly the
    // event_set_broadcast external-thread path.
    LockGuard lk(global_mtx_);
    // Transfer branch: wake_wait_one_locked acquires waiters_.mtx() inside
    // global_mtx_ (consistent lock order) and resolves the FIFO head. nullptr
    // means the queue is empty (Conclusion A). One release never both wakes a
    // waiter AND stores: a non-empty queue fully consumes this release.
    if (wake_wait_one_locked(waiters) != nullptr) {
        return true;  // permit transferred to the FIFO head; available_ unchanged
    }
    // Empty-queue branch: store the permit, or reject overflow.
    const std::uint32_t cur = available.load(std::memory_order::acquire);
    if (cur >= max_permits) {
        return false;  // overflow: no authoritative mutation
    }
    available.store(cur + 1, std::memory_order::release);
    return true;
}

}  // namespace sluice::async
