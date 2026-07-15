// sluice::async::AsyncMutex — Fiber-suspending async Mutex (sluice-CORE-E12-C).
//
// The third user-facing async synchronization primitive built on the closed
// E10/E11/E12-A/E12-B wait substrate. A Fiber-suspending Mutex composes:
//
//   - a Fiber* owner_                  (the SOLE ownership authority; nullptr =
//                                       NoOwner = unlocked; NO redundant
//                                       locked_/reserved_owner_/pending_owner_)
//   - a private E10 WaitQueue waiters_ (intrusive FIFO of suspended waiters)
//
// WITHOUT duplicating the Scheduler coordination domain, WITHOUT a Mutex-
// private wake channel, WITHOUT grant-in-flight / reserved-owner state, WITHOUT
// a public is_locked()/owner()/wait_queue() accessor, and WITHOUT direct Fiber
// manipulation beyond the standard admission/handoff seams.
//
// Semantic model (docs/e12-async-mutex.md, docs/spec/e12_async_mutex/):
//
//   owner_ == nullptr   <=>  Mutex is unlocked
//   owner_ != nullptr   <=>  Mutex is owned by exactly that Fiber
//
//   A lock() call from a free Mutex with no queued waiter acquires inline
//   (owner_ := current Fiber; no suspension). A lock() call while owned
//   registers on the FIFO queue and suspends. An unlock() with a queued waiter
//   performs a DIRECT ownership handoff to the eligible FIFO head: the winner
//   resolves Woken, owner_ := winner Fiber, and the winner is published runnable
//   — in that order (owner commit BEFORE publication). There is NO intermediate
//   owner_ := nullptr window during handoff.
//
// Synchronization domain: ALL authoritative decisions occur under
// Scheduler::global_mtx_ -> AsyncMutex waiters_.mtx() (the existing coordination
// domain; same lock order as E10/E11/E12-A/E12-B). owner_ is passed by
// reference into the Scheduler seams and is mutated ONLY under global_mtx_.
//
// Identity model: ownership is bound to Fiber* identity, independent of which
// Worker executes the Fiber. After E8 stealing, a Fiber may resume on a thief
// Worker; the owner check uses Fiber*, NOT Worker identity. The current Fiber
// is g_worker->current (the executing fiber), NOT fiber_owner_ / WaitReg.owner.
//
// SEALED PUBLIC AUTHORITY (mirrors E12-A/E12-B). The AsyncMutex's private
// WaitQueue is NOT publicly reachable: there is no wait_queue() accessor, no
// owner(), no is_locked(), and no test friend grants access to them. The ONLY
// RESOURCE_WAKE authorities on an AsyncMutex queue are unlock() (FIFO head
// handoff) and admission observing an admissible free owner (inline Woken).
// Cancellation routes through Scheduler::mutex_cancel on this AsyncMutex's
// private WaitQueue WITHOUT exposing it. Ordinary downstream code CANNOT call
// wake_wait_one on an AsyncMutex (the required bypass
// `scheduler.wake_wait_one(mtx.wait_queue())` does not compile). Deterministic
// test phase seams are reached ONLY through the internal-testing runtime
// variant (`sluice_async_internal_testing`); the production `sluice_async`
// target declares no test friend and exports no test phase symbol.
//
// Scheduler binding: AsyncMutex borrows Scheduler& for its lifetime. cancel()
// may be called from any OS thread; lock/try_lock/unlock require a currently
// running Fiber (g_worker->current).
//
// Destruction contract: destroying an AsyncMutex while it is owned or while
// wait epochs remain registered is a CALLER CONTRACT VIOLATION. The destructor
// debug-asserts owner_ == nullptr and does NOT cancel/wake/synthesize
// RESOURCE_WAKE and does NOT force-release ownership. Caller must ensure
// unlocked + all waits terminal before AsyncMutex lifetime ends (~WaitQueue
// asserts head_ == nullptr in debug). No cancel-all, no wake-all.
//
// Misuse contracts (debug asserts; no recovery semantics):
//   recursive try_lock        -> returns false, no mutation
//   recursive lock/lock_until -> caller precondition violation (debug assert)
//   non-owner unlock          -> caller precondition violation, no mutation
//   unlock while unlocked     -> caller precondition violation, no mutation
//   external-thread lock/try_lock/unlock -> caller precondition violation
#pragma once

#include <cassert>

#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

// A Fiber-suspending async Mutex.
//
// Non-copyable AND non-movable: the WaitQueue is non-movable (intrusive list
// with pointer identity), and the AsyncMutex borrows Scheduler& (identity
// matters for wait resolution routing). An AsyncMutex is constructed once and
// lives at one address for its lifetime.
class AsyncMutex {
public:
    // Construct an AsyncMutex bound to `scheduler`, initially unlocked
    // (owner_ == nullptr). The Scheduler must outlive the AsyncMutex. Does not
    // require a running Fiber (construction is a caller-lifetime operation).
    explicit AsyncMutex(Scheduler& scheduler) noexcept
        : scheduler_(scheduler), owner_(nullptr) {}

    // Destruction contract: the AsyncMutex must be unlocked (owner_ == nullptr)
    // and its wait queue must be empty before destruction. The destructor does
    // NOT cancel waiters, does NOT wake waiters, does NOT force-release
    // ownership, and does NOT synthesize RESOURCE_WAKE. The underlying
    // ~WaitQueue asserts head_ == nullptr in debug (caller must drain first).
    // In release builds, no recovery/cancel-all protocol is required.
    ~AsyncMutex() {
        assert(owner_ == nullptr &&
               "AsyncMutex destroyed while locked (owner != NoOwner)");
    }

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;

    // Attempt to acquire ownership WITHOUT suspending. In one authoritative
    // critical section (under global_mtx_ + waiters_.mtx()):
    //   if owner_ == nullptr AND no eligible queued waiter has FIFO priority:
    //       owner_ = current Fiber
    //       return true
    //   otherwise:
    //       return false  (no mutation)
    // No barging: a queued waiter with FIFO priority cannot be bypassed. NOT a
    // standalone atomic compare — the authoritative decision is Scheduler-
    // serialized. Requires a currently running Fiber (g_worker->current);
    // recursive acquire (current Fiber already owns) returns false with no
    // mutation.
    [[nodiscard]] bool try_lock() {
        return scheduler_.mutex_try_lock(waiters_, owner_);
    }

    // Acquire ownership, suspending the calling Fiber if it is not immediately
    // admissible. `node` must be Detached (fresh) and outlive this call's
    // suspend until it is resumed. The admission closure closes the lost-wake
    // window:
    //   - immediately admissible (owner_ == nullptr AND no eligible queued
    //     waiter): resolve the epoch Woken inline, owner_ = current Fiber, do
    //     NOT suspend.
    //   - otherwise: register on this AsyncMutex's private FIFO WaitQueue,
    //     recheck ownership admission under the authoritative locks, resolve +
    //     acquire inline if ownership became free, commit suspension only if
    //     still not admissible.
    // Outcome is read from `node` (Woken / Cancelled / Expired). Recursive lock
    // by the current owner is a caller-precondition debug assert. Returns void.
    void lock(WaitNode& node) {
        scheduler_.mutex_lock(waiters_, owner_, node);
    }

    // Deadline-aware lock. Composes lock() with E11 TimerRegistration. The wait
    // resolves when EXACTLY ONE cause wins the resolve_ CAS:
    //   - unlock() handoff            -> Woken   (RESOURCE_WAKE)
    //   - cancel(node)                -> Cancelled (CANCEL)
    //   - the deadline elapsing       -> Expired  (TIMER_EXPIRE)
    //
    // Mandatory precedence (resource-first):
    //   1. authoritative ownership admission
    //   2. already-due deadline
    //   3. timed registration and normal race
    // Exact results at admission (under the authoritative locks):
    //   ownership admissible + deadline already due -> Woken
    //   not admissible + deadline already due -> Expired
    //   ownership admissible + future deadline -> Woken
    //   not admissible + future deadline -> register and wait
    // For a registered timed wait, RESOURCE_WAKE / TIMER_EXPIRE / CANCEL compete
    // through the existing exactly-once WaitNode resolution authority. Reuses
    // E11 timer registration/retirement/unlinking/publication. No Mutex-local
    // deadline mechanism. Outcome is read from `node`. Returns void.
    void lock_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.mutex_lock_until(waiters_, owner_, node, deadline);
    }

    // Narrow per-wait-epoch CANCELLATION authority (mirrors Event/Semaphore
    // cancel). Resolves `node` with Cancelled through the Scheduler's
    // cancellation path on THIS AsyncMutex's private WaitQueue, WITHOUT
    // exposing that queue.
    //
    // May be called from ANY OS thread (does not require a running Fiber).
    //
    // Contract (returns true ONLY if ALL hold):
    //   - node is currently Registered, AND
    //   - node is currently linked in THIS AsyncMutex's private WaitQueue, AND
    //   - CANCEL wins node.resolve_.
    // Otherwise returns false WITHOUT mutation. This includes:
    //   detached node                      -> false
    //   already Woken                      -> false
    //   already Cancelled                  -> false
    //   already Expired                    -> false
    //   live node in another AsyncMutex    -> false (same OR different Scheduler)
    //
    // Wrong-Mutex cancellation is a safe false-return case (NOT UB, NOT a debug
    // assertion). The membership check scans THIS AsyncMutex's own queue under
    // this Scheduler's global_mtx_ + this AsyncMutex's waiters_.mtx(); it never
    // reads a foreign node's home_ and never locks a foreign Mutex/Scheduler.
    [[nodiscard]] bool cancel(WaitNode& node) {
        return scheduler_.mutex_cancel(waiters_, node);
    }

    // Release ownership with a DIRECT ownership handoff. The calling Fiber must
    // be the current owner (g_worker->current == owner_); non-owner or unlock-
    // while-unlocked is a caller-precondition debug assert with no mutation.
    // Under global_mtx_:
    //   if the queue has an eligible FIFO head W:
    //       W resolves Woken
    //       owner_ = W.fiber   (BEFORE publication)
    //       W is published runnable exactly once
    //       resulting state = Owned(W.fiber)   -- NO intermediate owner_=nullptr
    //   otherwise:
    //       owner_ = nullptr   (UnlockNoWaiter)
    void unlock() {
        scheduler_.mutex_unlock(waiters_, owner_);
    }

private:
    // E12-D (construction authorization §1.1): AsyncCondition friends AsyncMutex
    // SOLELY so it can (a) read scheduler_ to reach the SAME Scheduler as this
    // Mutex (C-H2), and (b) pass waiters_/owner_ BY REFERENCE into Scheduler's
    // private Condition seams — exactly as this Mutex's own methods pass them
    // into Scheduler's Mutex seams (e.g. scheduler_.mutex_lock(waiters_,
    // owner_, node)). AsyncCondition does NOT write owner_ directly, does NOT
    // register/wake/unlink on waiters_, does NOT implement its own handoff, and
    // does NOT grant ownership. All Mutex state transitions are performed by the
    // Scheduler (the authoritative Mutex state-machine executor) via the ONE
    // accepted mutex_handoff_one_locked / mutex_lock seam. No PUBLIC Mutex
    // accessor is added (the authority probe still seals wait_queue()/owner()).
    friend class AsyncCondition;

    Scheduler& scheduler_;
    Fiber* owner_;
    WaitQueue waiters_;
};

}  // namespace sluice::async
