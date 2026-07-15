// sluice::async::AsyncCondition — Fiber-suspending async condition variable
// (sluice-CORE-E12-D).
//
// The fourth user-facing async synchronization primitive built on the closed
// E10/E11/E12-A/E12-B/E12-C wait substrate. An AsyncCondition is bound to
// exactly one AsyncMutex at construction (C-H2) and composes:
//
//   - AsyncMutex& mutex_        (the bound ownership authority, borrowed)
//   - Scheduler& scheduler_     (the SAME Scheduler as the bound AsyncMutex,
//                                derived from mutex_ at construction to avoid
//                                drift — C-H2)
//   - a private WaitQueue waiters_ (the CONDITION queue; intrusive FIFO of
//                                suspended Condition waiters)
//   - std::atomic_size_t active_waits_  (lifetime diagnostic; debug-asserted
//                                zero in the destructor)
//
// Two-epoch protocol (docs/e12-condition.md §3). Every wait() creates TWO
// sequential single-wait epochs, NEVER a multi-wait:
//
//   Condition epoch  : the caller-supplied Condition WaitNode is registered in
//                      this AsyncCondition's private waiters_ (the Condition
//                      queue). It resolves via notify_one/notify_all (Woken),
//                      timer expiry (Expired), or cancel (Cancelled).
//   Reacquire epoch  : after the Condition node resolves, the Fiber resumes
//                      WITHOUT ownership and creates a STACK-LOCAL reacquire
//                      WaitNode, then calls the bound AsyncMutex's ordinary
//                      lock(reacquire_node). This is the closed E12-C admission
//                      path verbatim — untimed and NON-cancellable (C-H5).
//
// The two nodes are distinct WaitNode instances; they are NEVER registered
// simultaneously (the Condition node is terminal+unlinked before the reacquire
// node is created). This is InvNoDualQueueMembership (§13.2).
//
// Lost-notify closure (docs §6): register the Condition node BEFORE releasing
// the bound Mutex, both under one global_mtx_ critical section (the
// CONDITION-WAIT-PREPARE combined seam). A notify cannot interleave between
// registration and Mutex release because every Condition notify/cancel/expire
// path also requires global_mtx_.
//
// Synchronization domain: ALL authoritative decisions occur under
// Scheduler::global_mtx_ (the existing coordination domain). The Condition
// queue mtx and the bound Mutex queue mtx are taken SEQUENTIALLY under
// global_mtx_, never simultaneously (docs §6.3). The Mutex release/handoff
// reuses the ONE accepted mutex_handoff_one_locked authority (M-H1); there is
// no second Mutex handoff.
//
// SEALED PUBLIC AUTHORITY (mirrors E12-A/B/C). The AsyncCondition's private
// WaitQueue is NOT publicly reachable: there is no wait_queue() accessor, no
// mutex() accessor, no waiting_count(), no notify_n(), and no reacquire_node().
// The Condition queue is reached ONLY through this AsyncCondition's methods,
// which forward to Scheduler private Condition seams without exposing the
// queue. Ordinary downstream code CANNOT call wake_wait_one on an
// AsyncCondition queue (the required bypass
// `scheduler.wake_wait_one(cond.wait_queue())` does not compile).
//
// Mutex authority (§1.1 of the construction authorization): AsyncCondition is a
// friend of AsyncMutex SOLELY so the combined wait-prepare seam can obtain the
// SAME Scheduler as the bound Mutex and reach the Scheduler's private Condition
// seams. AsyncCondition does NOT write AsyncMutex::owner_ directly, does NOT
// register/wake/unlink on AsyncMutex::waiters_, does NOT implement its own
// Mutex handoff, and does NOT grant ownership. All Mutex state transitions are
// performed by the Scheduler (the authoritative Mutex state-machine executor)
// via the existing mutex_handoff_one_locked / mutex_lock seams. No PUBLIC Mutex
// accessor is added.
//
// Destruction contract: destroying an AsyncCondition while any wait() call has
// not returned (Condition node Registered OR a reacquire epoch in flight) is a
// CALLER CONTRACT VIOLATION. The destructor debug-asserts active_waits_ == 0
// (acquire load) and does NOT notify, cancel, force-reacquire, or release the
// Mutex. A reacquire-phase destruction is caught by active_waits_ (the Condition
// queue being empty is insufficient because the reacquire node lives in the
// Mutex's queue, not the Condition queue — docs §14). No cancel-all, no
// wake-all. Release builds need no automatic recovery semantics.
//
// Misuse contracts (debug asserts; no recovery semantics):
//   wait()/wait_until() by a non-owner Fiber -> caller precondition violation
//   destroy while active_waits_ > 0           -> caller precondition violation
//   notify_one/notify_all                     -> safe from any thread (no Fiber)
//   cancel                                    -> safe from any thread; wrong-
//                                               Condition / detached / terminal
//                                               node returns false (not UB)
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>

#include <sluice/async/async_mutex.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

// A Fiber-suspending async condition variable bound to one AsyncMutex.
//
// Non-copyable AND non-movable: the WaitQueue is non-movable (intrusive list
// with pointer identity), the bound AsyncMutex& + Scheduler& borrow identity
// matters for wait resolution routing. An AsyncCondition is constructed once
// and lives at one address for its lifetime.
class AsyncCondition {
public:
    // Bind to `mutex` (which already borrows a Scheduler). AsyncCondition uses
    // the SAME Scheduler as its bound AsyncMutex (C-H2), derived from `mutex`
    // at construction to avoid drift. Does not require a running Fiber
    // (construction is a caller-lifetime operation). `mutex` must outlive this
    // AsyncCondition.
    // `mutex.scheduler_` is reached via the AsyncCondition-friends-AsyncMutex
    // grant (the ONLY use of that friendship). This guarantees AsyncCondition
    // uses the SAME Scheduler as its bound Mutex (C-H2) without a second,
    // drift-prone Scheduler reference.
    explicit AsyncCondition(AsyncMutex& mutex) noexcept
        : mutex_(mutex), scheduler_(mutex.scheduler_) {}

    // Destruction contract: no wait() call may be in flight. The destructor
    // debug-asserts active_waits_ == 0 (acquire load) and does NOT perform any
    // notify/cancel/force-reacquire/release. The underlying ~WaitQueue asserts
    // head_ == nullptr in debug (caller must drain first). A reacquire-phase
    // destruction is caught by active_waits_ (the Condition-queue-empty check
    // alone is insufficient — docs §14).
    ~AsyncCondition() {
        assert(active_waits_.load(std::memory_order::acquire) == 0 &&
               "AsyncCondition destroyed while a wait() call is in flight "
               "(Condition epoch OR reacquire epoch)");
    }

    AsyncCondition(const AsyncCondition&) = delete;
    AsyncCondition& operator=(const AsyncCondition&) = delete;
    AsyncCondition(AsyncCondition&&) = delete;
    AsyncCondition& operator=(AsyncCondition&&) = delete;

    // Condition wait while owning the bound Mutex. `condition_node` must be
    // Detached (fresh) and outlive this call's suspend until it is resumed. The
    // calling Fiber MUST currently own the bound Mutex (caller precondition;
    // non-owner wait is a debug assert).
    //
    // Under one global_mtx_ critical section (CONDITION-WAIT-PREPARE):
    //   1. register `condition_node` into this AsyncCondition's Condition queue;
    //   2. release the bound Mutex via the accepted direct handoff (or
    //      owner_ = nullptr if the Mutex queue is empty) — owner-before-
    //      publication already load-bearing in mutex_handoff_one_locked;
    //   3. make the calling Fiber Waiting.
    // Then context_switch. On resume (Condition node terminal with reason r),
    // create a stack-local reacquire WaitNode and call mutex_.lock(reacquire_node)
    // (the ordinary E12-C admission path). Return r only after the reacquire
    // returns (mutex_owner == current Fiber, C-H1).
    [[nodiscard]] WaitOutcome wait(WaitNode& condition_node);

    // Deadline-aware Condition wait. The deadline governs ONLY the Condition
    // epoch (C-H4). If the deadline is ALREADY due at admission, the Condition
    // node resolves Expired INLINE: the Mutex is NOT released, the Fiber does
    // NOT suspend, no reacquire epoch is created, and the caller RETAINS
    // ownership (WaitDueInline / InvDueInlineRetainsOwnership). Otherwise the
    // deadline competes with notify/cancel through the exactly-once WaitNode
    // resolve_ CAS (an E11 TimerRegistration is installed for the Condition
    // node). After a suspended resolution (Woken/Expired/Cancelled), the
    // mandatory reacquire epoch runs as in wait(). The original deadline never
    // governs the reacquire (C-H4).
    [[nodiscard]] WaitOutcome wait_until(WaitNode& condition_node,
                                         Scheduler::deadline_t deadline);

    // Queue-identity-safe Condition-node cancellation (mirrors
    // AsyncMutex::cancel / Event::cancel). Resolves `condition_node` with
    // Cancelled ONLY if it is currently Registered AND linked in THIS
    // AsyncCondition's Condition queue AND the CANCEL CAS wins. Otherwise
    // returns false WITHOUT mutation (wrong-Condition, detached, Woken,
    // Expired, Cancelled, repeated cancel). May be called from ANY OS thread.
    // Late Condition cancel cannot affect the reacquire epoch (C-H5): the
    // reacquire node is never exposed to cancel().
    [[nodiscard]] bool cancel(WaitNode& condition_node);

    // Resolve the eligible FIFO head of the Condition queue with Woken and
    // publish the winner runnable (Condition-epoch publication). The winner
    // subsequently performs ordinary Mutex reacquire on resume (C-H3). Non-
    // persistent: a notify before any wait is lost (no token stored). Empty
    // queue is a no-op. Safe from any OS thread. Does NOT mutate Mutex state.
    void notify_one();

    // Atomic snapshot-and-drain of every eligible Registered Condition waiter:
    // resolve each Woken exactly once, publish each runnable, all under one
    // continuous global_mtx_ critical section (C-H10). Waiters registered after
    // the snapshot linearization point are excluded (the continuous global_mtx_
    // hold IS the atomic snapshot; admission also needs global_mtx_). Safe from
    // any OS thread. Does NOT mutate Mutex state.
    void notify_all();

private:
    friend class Scheduler;  // reaches waiters_ for the private Condition seams

    AsyncMutex& mutex_;
    Scheduler& scheduler_;
    WaitQueue waiters_;  // the CONDITION queue (NOT the Mutex queue)
    // Lifetime diagnostic: incremented at wait()/wait_until() entry, decremented
    // at every return path (RAII guard), including the inline-Expired path.
    // Debug-asserted zero in ~AsyncCondition(). This is a DIAGNOSTIC ONLY: it
    // performs no cancel/notify/cleanup. Atomic so the destructor's acquire
    // load observes a consistent value even if a notifier thread last touched
    // it (the construction authorization §1.2 forbids a non-atomic counter).
    std::atomic<std::size_t> active_waits_{0};

    // ---- method definitions (inline; forward to Scheduler E12-D seams) ----

    // RAII guard for active_waits_ (construction authorization §1.2). Incremented
    // at wait()/wait_until() entry; decremented on EVERY exit path (normal
    // return, inline-Expired, and any early C8-contract-violation return). The
    // counter is a DIAGNOSTIC ONLY: it performs no cancel/notify/cleanup. It
    // stays > 0 across the entire wait() call, INCLUDING the reacquire epoch
    // (so destruction-during-reacquire is caught — docs §14). Atomically
    // incremented/decremented so the destructor's acquire load observes a
    // consistent value.
    struct ActiveWaitGuard {
        std::atomic<std::size_t>& cnt;
        explicit ActiveWaitGuard(std::atomic<std::size_t>& c) noexcept : cnt(c) {
            cnt.fetch_add(1, std::memory_order::acq_rel);
        }
        ~ActiveWaitGuard() noexcept {
            cnt.fetch_sub(1, std::memory_order::acq_rel);
        }
        ActiveWaitGuard(const ActiveWaitGuard&) = delete;
        ActiveWaitGuard& operator=(const ActiveWaitGuard&) = delete;
    };
};

inline WaitOutcome AsyncCondition::wait(WaitNode& condition_node) {
    ActiveWaitGuard guard(active_waits_);  // (§1.2.1/§1.2.2) covers all paths
    // CONDITION-WAIT-PREPARE: register Condition node + release bound Mutex +
    // make_waiting, under one global_mtx_ CS; then context_switch. Returns the
    // latched Condition outcome. `released_mutex` distinguishes the C8
    // registration-failure path (the Mutex was NOT released — the caller
    // retains ownership and runs NO reacquire epoch) from every other path
    // (Woken/Cancelled/suspended-Expired, all of which released the Mutex and
    // MUST run the mandatory reacquire, C-H1). This mirrors wait_until's
    // handling of its inline-Expired-at-admission path.
    bool released_mutex = false;
    WaitOutcome reason = scheduler_.condition_wait_prepare(
        waiters_, condition_node, mutex_.waiters_, mutex_.owner_, released_mutex);
    // The C8 registration-failure path (node already registered/terminal) did
    // NOT release the Mutex: the caller RETAINS ownership and runs NO reacquire
    // epoch. Every other path released the Mutex and MUST reacquire (C-H1).
    if (!released_mutex) {
        return reason;  // caller still owns the Mutex; no reacquire
    }
    // Mandatory reacquire epoch (C-H1/C-H3/C-H5): create a stack-local
    // reacquire WaitNode and call the bound Mutex's ordinary lock(). The Fiber
    // is stackful, so this local object remains alive across the reacquire
    // suspension and is destroyed only after reacquire returns terminal
    // (C-H7). This node is NEVER registered simultaneously with the Condition
    // node (the Condition node is terminal+unlinked before this point). It is
    // untimed and NON-cancellable (C-H4/C-H5): no cancel/expire is offered.
    WaitNode reacquire_node;
    mutex_.lock(reacquire_node);
    // wait() returns only when mutex_owner == current Fiber (C-H1). The return
    // value is the LATCHED Condition reason; the reacquire cannot alter it.
    return reason;
}

inline WaitOutcome AsyncCondition::wait_until(WaitNode& condition_node,
                                              Scheduler::deadline_t deadline) {
    ActiveWaitGuard guard(active_waits_);  // (§1.2.1-§1.2.4) covers all paths
    bool released_mutex = false;
    WaitOutcome reason = scheduler_.condition_wait_prepare_until(
        waiters_, condition_node, mutex_.waiters_, mutex_.owner_, deadline,
        released_mutex);
    // The inline-Expired-at-admission path (WaitDueInline) did NOT release the
    // Mutex: the caller RETAINS ownership and runs NO reacquire epoch (docs
    // §10.2). Every other path (Woken/Cancelled/suspended-Expired) released the
    // Mutex and MUST run the mandatory reacquire (C-H1). Both an inline-Expired
    // and a suspended-Expired return Expired; released_mutex distinguishes them.
    if (!released_mutex) {
        return reason;  // caller still owns the Mutex; no reacquire
    }
    WaitNode reacquire_node;
    mutex_.lock(reacquire_node);
    return reason;
}

inline bool AsyncCondition::cancel(WaitNode& condition_node) {
    // Queue-identity-gated Condition-node cancel. May be called from ANY thread.
    // The reacquire epoch is unaffected (C-H5): the reacquire node is never
    // exposed to cancel().
    return scheduler_.condition_cancel_wait(waiters_, condition_node);
}

inline void AsyncCondition::notify_one() {
    // Resolve the eligible FIFO Condition head Woken + publish the winner.
    // Non-persistent; empty queue is a no-op. Does NOT mutate Mutex state.
    scheduler_.condition_notify_one(waiters_);
}

inline void AsyncCondition::notify_all() {
    // Atomic snapshot-and-drain of every eligible Registered Condition waiter.
    // Does NOT mutate Mutex state.
    scheduler_.condition_notify_all(waiters_);
}

}  // namespace sluice::async
