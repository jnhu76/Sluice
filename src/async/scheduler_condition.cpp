// Scheduler Condition primitive — implementation TU split from scheduler.cpp
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
WaitOutcome Scheduler::condition_wait_prepare(WaitQueue& cond_waiters,
                                              WaitNode& cond_node,
                                              WaitQueue& mutex_waiters,
                                              Fiber*& owner,
                                              bool& released_mutex) {
    // CONDITION-WAIT-PREPARE (docs §7). One global_mtx_ critical section makes
    // register-Condition-node + release-Mutex + make_waiting ATOMIC w.r.t. every
    // Condition notify/cancel/expire path (which also need global_mtx_). This is
    // the lost-notify closure (docs §6): a notify CANNOT interleave between
    // Condition registration and Mutex release.
    //
    // `released_mutex` mirrors condition_wait_prepare_until: false on the
    // registration-failure path (the Mutex is NOT released — the caller retains
    // ownership and runs NO reacquire epoch), true after the Mutex has been
    // released/handed off (the caller MUST run the reacquire epoch). The untimed
    // path has no inline-Expired-at-admission branch, so every other path
    // releases the Mutex.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncCondition::wait requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner == me && "AsyncCondition::wait by a non-owner Fiber is a "
                          "caller precondition violation");
    {
        LockGuard lk(global_mtx_);
        // Step 1: register the Condition node into the Condition queue. This is
        // a DIFFERENT queue from the Mutex queue (InvNoDualQueueMembership).
        {
            LockGuard qlk(cond_waiters.mtx());
            if (!cond_waiters.register_wait_locked(cond_node, me)) {
                // Registration contract violation (node already
                // registered/terminal). Do NOT release the Mutex; the caller
                // retains ownership. Return the node's (terminal) outcome.
                released_mutex = false;
                return cond_node.outcome();
            }
            ++waiting_waitq_count_;
        }
        // Deterministic phase seam (test variant only): the Condition node
        // is now Registered AND linked in the Condition queue, while the bound
        // Mutex is STILL owned by `me`. A test observing this phase can prove
        // the register-before-release ordering (InvNoLostNotifyWindow / NEG-C8)
        // and that a concurrent notify sees the registered node.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::condition_register_before_handoff);
#endif
        // INTERNALLY (under global_mtx_); the Condition queue mtx was already
        // released above, so the two queue mtxes are NEVER held simultaneously
        // (docs §6.3 — sequential lock topology, no self-deadlock). A nullptr
        // return means the Mutex queue is empty -> owner = nullptr.
        if (mutex_handoff_one_locked(mutex_waiters, owner) == nullptr) {
            owner = nullptr;  // UnlockNoWaiter: no Mutex waiter to hand off to
        }
        // The Mutex has been released/handed off; the caller MUST run the
        // reacquire epoch regardless of the outcome below.
        released_mutex = true;
        // Defense-in-depth: if the Condition node was resolved concurrently
        // (notify/cancel/expire all need global_mtx_, so this cannot happen
        // while this CS holds it, but guard anyway), undo the registration and
        // do NOT suspend. The Mutex has already been released/handed off; the
        // caller will run the reacquire epoch regardless.
        if (cond_node.is_terminal()) {
            return cond_node.outcome();
        }
        // Step 3: commit the calling Fiber to Waiting (inside global_mtx_, so a
        // concurrent resolver's make_runnable is the publication guard).
        // Unified suspend protocol.
        commit_suspend_locked(ws, me);
    }
    // ONLY context_switch is outside global_mtx_ (mirrors await_wait /
    // mutex_lock). The switch-back target is the calling fiber's ctx; it resumes
    // here after the Condition node resolves (Woken/Expired/Cancelled) and the
    // winner's make_runnable+route publishes it.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // The Condition node is terminal+unlinked. The caller (AsyncCondition::wait)
    // latches this outcome and then runs the mandatory reacquire epoch.
    return cond_node.outcome();
}

WaitOutcome Scheduler::condition_wait_prepare_until(WaitQueue& cond_waiters,
                                                    WaitNode& cond_node,
                                                    WaitQueue& mutex_waiters,
                                                    Fiber*& owner,
                                                    deadline_t deadline,
                                                    bool& released_mutex) {
    // Deadline-aware CONDITION-WAIT-PREPARE (docs §10). The deadline governs
    // ONLY the Condition epoch (C-H4). Admission precedence (under global_mtx_):
    //   1. deadline ALREADY due -> resolve Expired INLINE (WaitDueInline): do
    //      NOT release the Mutex, do NOT suspend, do NOT create a reacquire
    //      epoch. The caller RETAINS ownership (InvDueInlineRetainsOwnership).
    //      released_mutex = false.
    //   2. else -> register node + timer, release/handoff Mutex, make_waiting,
    //      context_switch; return the latched outcome. released_mutex = true
    //      (the caller MUST run the reacquire epoch).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncCondition::wait_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner == me && "AsyncCondition::wait_until by a non-owner Fiber is a "
                          "caller precondition violation");
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        {
            LockGuard qlk(cond_waiters.mtx());
            if (!cond_waiters.register_wait_locked(cond_node, me)) {
                // Registration contract violation: do NOT release the Mutex.
                released_mutex = false;
                return cond_node.outcome();
            }
            ++waiting_waitq_count_;
            // Install the timer for the Condition epoch ONLY (C-H4). The
            // registration binds {cond_node, cond_waiters} so a later expiry
            // resolves the Condition node Expired through pump_deadlines_locked.
            // Arming = pool construction + ACTIVE count + heap push + park-cache
            // refresh via the ordinary deadline authority.
            reg = arm_ordinary_deadline_locked(&cond_node, &cond_waiters, deadline);
        }
        // Admission precedence 1: already-due admission closure — if the
        // deadline is ALREADY due, the
        // Condition node resolves Expired INLINE. The Mutex is NOT released
        // (the caller retains ownership), the Fiber does NOT suspend, and no
        // reacquire epoch is created. This is WaitDueInline /
        // InvDueInlineRetainsOwnership.
        if (clock_now_unlocked() >= deadline) {
            LockGuard qlk(cond_waiters.mtx());
            if (cond_waiters.expire_locked(cond_node)) {
                // ACTIVE->CONSUMED via the ordinary deadline authority; the
                // already-due inline path keeps its immediate cache recompute.
                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // Fiber is RUNNING and continues inline; no publication.
                released_mutex = false;  // Mutex NOT released; no reacquire
                return WaitOutcome::expired;  // resolved at admission; do NOT
                                             // release Mutex or suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through to
            // the terminal-recheck guard (the node is no longer Registered).
        }
        // Step 2: register-before-handoff phase seam (same as the untimed seam).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::condition_register_before_handoff);
#endif
        // Step 3: release the bound Mutex via the ONE accepted handoff.
        if (mutex_handoff_one_locked(mutex_waiters, owner) == nullptr) {
            owner = nullptr;
        }
        // Defense-in-depth: concurrent resolution guard. The Mutex has been
        // released; the caller MUST run the reacquire epoch regardless.
        released_mutex = true;
        if (cond_node.is_terminal()) {
            return cond_node.outcome();
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
    return cond_node.outcome();
}

void Scheduler::condition_notify_one(WaitQueue& cond_waiters) {
    // Resolve the eligible FIFO head of the Condition queue with Woken and
    // publish the winner runnable (Condition-epoch publication). Mirrors
    // wake_wait_one EXACTLY but operates on the Condition queue. The winner
    // subsequently performs its OWN reacquire epoch on resume; this seam does
    // NOT mutate Mutex state. Safe from any OS thread (g_worker may be null:
    // route_runnable_locked handles external-thread routing via pending_spawn_).
    LockGuard lk(global_mtx_);
    (void)wake_wait_one_locked(cond_waiters);
}

std::size_t Scheduler::condition_notify_all(WaitQueue& cond_waiters) {
    // Atomic snapshot-and-drain (C-H10): under one continuous global_mtx_ CS,
    // loop wake_wait_one_locked(cond_waiters) until nullptr. Mirrors
    // event_set_broadcast's drain loop EXACTLY. Each winner resolves Woken
    // exactly once, retires its timer, decrements waiting_waitq_count_, and is
    // published runnable. Waiters registered after the snapshot linearization
    // point are excluded (admission needs global_mtx_, which this holds). The
    // continuous global_mtx_ hold IS the atomic snapshot; no separate snapshot
    // container is needed. Does NOT mutate Mutex state.
    std::size_t woken = 0;
    LockGuard lk(global_mtx_);
    // Deterministic phase seam: global authority acquired, before the
    // drain begins. A test observing this phase can prove late registration /
    // cancel / expiry serialize AFTER the snapshot (they need global_mtx_).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::condition_notify_before_drain);
#endif
    while (wake_wait_one_locked(cond_waiters) != nullptr) {
        ++woken;
    }
    return woken;
}

bool Scheduler::condition_cancel_wait(WaitQueue& cond_waiters, WaitNode& cond_node) {
    // Queue-identity-safe Condition-node cancellation. Mirrors event_cancel_wait
    // / mutex_cancel EXACTLY: the membership gate (contains_locked) is taken
    // BEFORE the resolve CAS so no mutation occurs on a non-member. AsyncCondition
    // passes its private cond_waiters here (NOT exposed to the caller). The
    // contract: returns true ONLY if cond_node is Registered AND linked in
    // cond_waiters AND CANCEL wins. Otherwise returns false WITHOUT mutation.
    // Does NOT change Mutex `owner`. Safe from any OS thread. Safe against
    // wrong-Condition (same/different Scheduler), detached, Woken, Expired, and
    // Cancelled nodes.
    LockGuard lk(global_mtx_);
    LockGuard qlk(cond_waiters.mtx());
    if (!cond_waiters.contains_locked(cond_node)) return false;
    if (!cond_waiters.cancel_locked(cond_node)) return false;  // loser
    retire_timer_for_node_locked(cond_node);
    Fiber* f = cond_node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr) {
        publish_waiting_fiber_runnable_locked(f);
    }
    return true;
}

}  // namespace sluice::async
