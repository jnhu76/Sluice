// Scheduler Mutex primitive — implementation TU split from scheduler.cpp
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
bool Scheduler::mutex_try_lock(WaitQueue& waiters, Fiber*& owner) {
    // Authoritative try_lock under global_mtx_ + waiters_.mtx() (the SAME
    // domain every unlock / cancel / expire / admission path takes). owner is
    // passed by reference (the AsyncMutex's Fiber* owner_); nullptr == NoOwner.
    //
    // No barging: if a waiter is already queued (eligible FIFO head exists),
    // try_lock MUST fail even when owner_ == nullptr — a newcomer may not bypass
    // the queued waiter's priority.
    //
    // Recursive acquire (current Fiber == owner) returns false with no mutation
    // (recursive locking is forbidden, §7.1). A null current Fiber (external
    // thread / no g_worker) is a caller-precondition debug assert.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::try_lock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    if (owner == me) {
        // Recursive try_lock: forbidden. Return false, no mutation.
        return false;
    }
    if (owner == nullptr && waiters.empty_locked()) {
        owner = me;
        return true;
    }
    return false;  // owned, OR an eligible queued waiter has FIFO priority
}

void Scheduler::mutex_lock(WaitQueue& waiters, Fiber*& owner, WaitNode& node) {
    // Mutex lock admission. The lost-wake closure: register + recheck admission
    // + commit suspension — all under one global_mtx_ + waiters_.mtx() critical
    // section (the same domain unlock / cancel / expire / admission use). Only
    // context_switch is outside the lock. Mirrors sem_acquire's lost-set closure
    // but the admission predicate is "owner_ == nullptr AND this node is the
    // eligible FIFO head" instead of "a stored permit is admissible".
    //
    // The admission window is closed: an ownership-free observation during the
    // admission critical section leads to inline Woken, not a sleeping
    // registered waiter. The trace
    //     initial check sees owner_ != nullptr
    //     waiter registers
    //     owner unlocks (handoff or free)
    //     waiter attempts suspension
    // cannot strand the waiter because unlock() takes global_mtx_ to hand off
    // or free, and this admission recheck runs under the same global_mtx_ after
    // registration — unlock either completed before registration (its handoff
    // targeted the prior head / freed owner_, which the recheck observes) or
    // runs after this critical section (it sees this registered node and hands
    // off to it).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::lock requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner != me && "AsyncMutex::lock recursive acquisition is a caller "
                          "precondition violation (not a successful acquisition)");
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            // Node already registered or terminal: contract violation.
            return;
        }
        ++waiting_waitq_count_;

        // Admission recheck: ownership is admissible to THIS node only if owner_
        // is free (nullptr) AND it is the FIFO head (no earlier waiter has
        // priority). register_wait_locked links at the FIFO TAIL, so this node
        // is the head iff node.prev_ == nullptr (read under waiters_.mtx(), the
        // structural authority). wake_node_locked resolves THIS specific node
        // with Woken, so there is no head-identity ambiguity at the resolve CAS.
        //
        // If an earlier waiter is queued (node.prev_ != nullptr), this node MUST
        // NOT acquire even when owner_ == nullptr: the earlier waiter has FIFO
        // priority. A concurrent unlock hands off to the head (this node, if it
        // is the head) rather than freeing, so owner_ == nullptr with a queued
        // earlier waiter is a transient that this check correctly refuses to
        // admit.
        if (node.prev_ == nullptr && owner == nullptr) {
            if (waiters.wake_node_locked(node)) {
                owner = me;
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
        // This fiber's node is registered in the Mutex waiter
        // queue and the fiber will suspend (no immediate ownership). A test
        // observing this phase proves the node queued (T15a/T15b).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::mutex_waiter_registered_before_grant);
#endif
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

void Scheduler::mutex_lock_until(WaitQueue& waiters, Fiber*& owner,
                                 WaitNode& node, deadline_t deadline) {
    // Deadline-aware lock. Composes mutex_lock's admission closure with
    // TimerRegistration. The wait resolves when EXACTLY ONE cause wins the
    // resolve_ CAS:
    //   - unlock() handoff (mutex_unlock -> mutex_handoff_one_locked) -> Woken
    //   - cancel(node) (mutex_cancel)                              -> Cancelled
    //   - the deadline elapsing (pump_deadlines_locked)            -> Expired
    //
    // Admission precedence (resource-first, under global_mtx_ + waiters_.mtx()):
    //   1. If ownership is admissible (owner == nullptr AND node is FIFO head):
    //      resolve Woken inline (no suspend). Ownership admission wins over a
    //      due deadline (the resource is ready; the deadline is moot).
    //   2. Else if the deadline is already due: resolve Expired inline (the
    //      already-due closure).
    //   3. Else: commit suspension.
    // A non-timer winner retires the registration in the same CS (the
    // timer-lifetime closure).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::lock_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner != me && "AsyncMutex::lock_until recursive acquisition is a "
                          "caller precondition violation");
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

        // Admission precedence 1: ownership admission wins over a due deadline.
        // If owner_ is free AND this node is the FIFO head (no earlier waiter —
        // node.prev_ == nullptr, read under waiters_.mtx()), resolve Woken
        // inline, commit ownership, and retire the timer (the deadline is moot).
        if (node.prev_ == nullptr && owner == nullptr) {
            if (waiters.wake_node_locked(node)) {
                owner = me;
                // ACTIVE->RETIRED via the ordinary deadline authority (no
                // Mutex on_resolve hook exists; count decrement inside).
                (void)retire_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // Fiber is RUNNING and continues inline; no publication.
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // Admission precedence 2: already-due closure — if the deadline is
        // ALREADY due (and
        // ownership is not admissible), resolve Expired inline. The fiber must
        // NOT suspend and wait for a future timer scan merely because
        // registration happened after the deadline was due.
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

bool Scheduler::mutex_cancel(WaitQueue& waiters, WaitNode& node) {
    // Queue-identity-safe cancellation. Mirrors sem_cancel exactly.
    // AsyncMutex::cancel passes its private waiters_ here (NOT exposed to the
    // caller). The contract:
    //   returns true ONLY if node is currently Registered AND currently linked
    //   in THIS AsyncMutex's private WaitQueue AND CANCEL wins node.resolve_.
    //   Otherwise returns false WITHOUT mutation.
    //
    // May run from ANY OS thread (g_worker may be null): cancel does not acquire
    // Mutex ownership and does not require Fiber identity. The membership check
    // scans THIS queue's own intrusive list for &node while holding this
    // Scheduler's global_mtx_ + this AsyncMutex's waiters_.mtx(). It does NOT
    // read a foreign node's home_, does NOT lock a foreign Mutex or foreign
    // Scheduler. Wrong-Mutex (same OR different Scheduler), detached, Woken,
    // Expired, and Cancelled nodes all return false safely. This call CANNOT
    // synthesize a RESOURCE_WAKE and CANNOT change owner_.
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

WaitNode* Scheduler::mutex_handoff_one_locked(WaitQueue& waiters, Fiber*& owner) {
    // MUTEX-HANDOFF-ONE (docs §10): the narrow private seam that resolves the
    // eligible FIFO head Woken, commits owner_ = winner Fiber, retires any
    // bound timer, and publishes the winner runnable — in THAT source order
    // (owner commit BEFORE make_runnable / route_runnable_locked). This is the
    // load-bearing owner-before-publication refinement obligation (§10.5/§15.4).
    //
    // Mirrors wake_wait_one_locked EXCEPT it writes the winner's fiber() into
    // the caller's `owner` reference between resolution and publication. The
    // Semaphore has no ownership to commit; the Mutex does.
    //
    // The caller MUST hold global_mtx_. waiters_.mtx() is taken here (under
    // global_mtx_, consistent lock order). Returns the winning node (nullptr if
    // the queue is empty or the head lost to a concurrent resolver). A winning
    // linked node with null Fiber is an internal-invariant debug assert, NOT an
    // empty-queue result (§10.6).
    LockGuard qlk(waiters.mtx());
    WaitNode* won = waiters.wake_one_locked();  // resolve FIFO head Woken + unlink
    if (won == nullptr) return nullptr;  // empty, or head lost to a cancel
    Fiber* f = won->fiber();
    assert(f != nullptr && "MUTEX-HANDOFF-ONE winner has null Fiber "
                           "(internal invariant failure, NOT empty queue)");
    // ---- owner commit BEFORE publication (§10.5 load-bearing order) ----
    owner = f;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic test phase: after owner commit, BEFORE runnable publication.
    // A test observing this phase can prove owner == winner Fiber, winner not
    // yet published, old owner cannot reacquire, newcomer try_lock cannot barge.
    // No allocation, no callback, no lock held beyond global_mtx_ (already held
    // by the caller); compiled ONLY for the internal-testing variant.
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::mutex_handoff_before_publication);
#endif
    retire_timer_for_node_locked(*won);  // timer-lifetime closure (same CS)
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Exactly-once: publish a runnable ticket ONLY if waiting->runnable
    // succeeded. The node is terminal; make_runnable is the publication guard.
    // Route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr) {
        publish_waiting_fiber_runnable_locked(f);
    }
    return won;
}

void Scheduler::mutex_unlock(WaitQueue& waiters, Fiber*& owner) {
    // Unlock with direct ownership handoff. Under global_mtx_, the calling
    // (owner) Fiber releases ownership:
    //   - if the queue has an eligible FIFO head: MUTEX-HANDOFF-ONE resolves it
    //     Woken, commits owner_ = winner Fiber (BEFORE publication), retires the
    //     timer, and publishes the winner runnable exactly once. The ownership
    //     transition is Owned(F_old) -> Owned(F_new) with NO intermediate
    //     owner_ = nullptr.
    //   - otherwise (queue empty): owner_ = nullptr (UnlockNoWaiter).
    //
    // Non-owner unlock and unlock-while-unlocked are caller-precondition debug
    // asserts with no owner/queue mutation. Requires a running Fiber
    // (g_worker->current); the current Fiber must equal `owner`.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::unlock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    assert(owner == me && "AsyncMutex::unlock by non-owner is a caller "
                          "precondition violation (no owner/queue mutation)");
    (void)me;  // debug-only precondition check; release path does not need me
    // Handoff branch: mutex_handoff_one_locked acquires waiters_.mtx() inside
    // global_mtx_ (consistent lock order) and resolves the FIFO head. nullptr
    // means the queue is empty (Conclusion A).
    if (mutex_handoff_one_locked(waiters, owner) != nullptr) {
        return;  // ownership transferred to the FIFO head; owner_ = winner
    }
    // Empty-queue branch: release ownership. No waiter to hand off to.
    owner = nullptr;
}


}  // namespace sluice::async
