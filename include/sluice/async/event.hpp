// sluice::async::Event — persistent manual-reset async Event (sluice-CORE-E12-A).
//
// The first user-facing asynchronous synchronization primitive built on the
// closed E10/E11 wait substrate. A manual-reset Event composes:
//
//   - a persistent std::atomic<bool> set_ (the level/persistence property,
//     borrowed from the ready-flag substrate's IDEA but not its single-
//     registrant MECHANISM — see docs/e12-sync-primitives-plan.md §2.2)
//   - an E10 WaitQueue (which holds many waiters in an intrusive FIFO list)
//     for the blocking path
//
// WITHOUT duplicating the old waiting_ready_ subsystem, WITHOUT an Event-
// private timer, WITHOUT an Event-private cancellation winner, and WITHOUT
// direct Fiber manipulation or runnable enqueue.
//
// Semantic model (docs/e12-event.md):
//
//   UNSET ──set()──> SET
//    SET ──reset()──> UNSET
//
//   SET is level-triggered persistent readiness.
//   Late waiters observing SET return without suspension.
//   set() is idempotent.
//   reset() clears readiness; does NOT cancel registered waiters.
//   set() attempts RESOURCE_WAKE for every registered Event wait epoch.
//   Each wait epoch independently arbitrates RESOURCE_WAKE / TIMER_EXPIRE /
//   CANCEL through its own WaitNode::resolve_ authority.
//
// Synchronization domain: ALL Event operations (set, reset, wait admission,
// set's drain) are serialized under Scheduler::global_mtx_ (the existing
// coordination domain). This makes OLD_SET_WAKES_POST_RESET_WAITER mechanically
// impossible — set()'s drain completes atomically before reset() or a new
// admission can run. No generation counter or snapshot is needed.
//
// SEALED PUBLIC AUTHORITY (ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1 +
// E12-A-EVENT-CORRECTIVE-1 F-EVENT-AUTH). The Event's private WaitQueue is NOT
// publicly reachable: there is no wait_queue() accessor on the production-
// public surface, and no test friend grants access to it. The ONLY Event-
// specific RESOURCE_WAKE authorities are set() and admission observing SET.
// Cancellation is the narrow per-wait-epoch Event::cancel surface (CANCEL
// cause), which routes through Scheduler::event_cancel_wait on this Event's
// private WaitQueue WITHOUT exposing it. Ordinary downstream code CANNOT call
// wake_wait_one on an Event (the required bypass
// `scheduler.wake_wait_one(event.wait_queue())` does not compile). Deterministic
// test phase seams are reached ONLY through the internal-testing runtime
// variant (`sluice_async_internal_testing`); the production `sluice_async`
// target declares no test friend and exports no test phase symbol.
//
// Scheduler binding: Event borrows Scheduler& for its lifetime. set() may be
// called from an external OS thread; wait resolution reaches that Scheduler's
// waiting_waitq_count_, runnable routing, wake source, and deadline service.
//
// Destruction contract: destroying Event while wait epochs remain registered is
// a CALLER CONTRACT VIOLATION. The destructor does NOT cancel/wake/synthesize
// RESOURCE_WAKE. Caller must ensure all waits are terminal before Event lifetime
// ends. A debug assertion (~WaitQueue's existing assert) enforces this.
#pragma once

#include <atomic>
#include <cassert>

#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

// A persistent manual-reset async Event.
//
// Non-copyable AND non-movable: the WaitQueue is non-movable (intrusive list
// with pointer identity), and the Event borrows Scheduler& (identity matters for
// wait resolution routing). An Event is constructed once and lives at one
// address for its lifetime.
class Event {
public:
    // Construct an Event bound to `scheduler`. `initially_set` controls the
    // initial readiness state. The Scheduler must outlive the Event.
    explicit Event(Scheduler& scheduler, bool initially_set = false) noexcept
        : scheduler_(scheduler), set_(initially_set) {}

    // Destruction contract: all Event waits must be terminal / unregistered
    // before the Event is destroyed. The destructor does NOT cancel waiters,
    // does NOT wake waiters, and does NOT synthesize RESOURCE_WAKE. The
    // underlying ~WaitQueue asserts empty in debug (caller must drain first).
    // In release builds, no recovery/cancel-all protocol is required.
    ~Event() = default;

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    // The persistent readiness state. Lock-free atomic load. Returns true iff
    // the Event is SET (level-triggered readiness observable by late waiters).
    [[nodiscard]] bool is_set() const noexcept {
        return set_.load(std::memory_order::acquire);
    }

    // Transition to SET and attempt RESOURCE_WAKE resolution for every
    // currently registered Event wait epoch. Idempotent: set() on SET is a
    // no-op (no extra wake). Safe to call from an external OS thread. Each
    // winner is an independent resolve_(Woken) CAS + runnable publication.
    void set() {
        scheduler_.event_set_broadcast(waiters_, set_);
    }

    // Transition to UNSET. Does NOT resolve, cancel, expire, unlink, or publish
    // any WaitNode. A waiter already registered remains governed by future set(),
    // deadline, or cancellation. reset() must NOT revoke an already-won Woken
    // outcome.
    void reset() {
        scheduler_.event_reset(set_);
    }

    // Suspend the calling Fiber on this Event, registering `node`. The wait
    // resolves when EXACTLY ONE cause wins the resolve_ CAS:
    //   - set() broadcast         -> Woken   (RESOURCE_WAKE)
    //   - cancel_wait(q, node)    -> Cancelled (CANCEL)
    // If the Event is SET at admission, returns Woken immediately without
    // suspending. `node` must be Detached (fresh) and outlive this call's
    // suspend until it is resumed. Result via node.outcome().
    void wait(WaitNode& node) {
        scheduler_.await_event_wait(waiters_, set_, node);
    }

    // Deadline-aware Event wait. Composes wait() with E11 TimerRegistration.
    // The wait resolves when EXACTLY ONE cause wins:
    //   - set() broadcast         -> Woken   (RESOURCE_WAKE)
    //   - cancel_wait(q, node)    -> Cancelled (CANCEL)
    //   - deadline elapsing       -> Expired  (TIMER_EXPIRE)
    // If SET at admission, resolves Woken inline (no suspend). If the deadline
    // is already due at admission (and not SET), resolves Expired inline (E11 I5).
    //
    // Deadline precedence (F-EVENT-DEADLINE, normative): during Event deadline
    // admission, Event SET readiness is checked BEFORE the already-due deadline
    // predicate. Therefore Event SET + already-due deadline -> Woken (the
    // resource is ready; the deadline is moot). The matrix:
    //   SET    + future      -> Woken
    //   SET    + already due -> Woken   (SET precedence)
    //   UNSET  + already due -> Expired
    //   UNSET  + future      -> RESOURCE/TIMER/CANCEL race
    void wait_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.await_event_wait_deadline(waiters_, set_, node, deadline);
    }

    // Narrow per-wait-epoch CANCELLATION authority (E12-A-EVENT-CORRECTIVE-2).
    // Resolves `node` with Cancelled through the Scheduler's cancellation path on
    // THIS Event's private WaitQueue, WITHOUT exposing that queue.
    //
    // Contract (Corrective C — returns true ONLY if ALL hold):
    //   - node is currently Registered, AND
    //   - node is currently linked in THIS Event's private WaitQueue, AND
    //   - CANCEL wins node.resolve_(Cancelled).
    // Otherwise returns false WITHOUT mutation. This includes:
    //   detached node              -> false
    //   already Woken              -> false
    //   already Cancelled          -> false
    //   already Expired            -> false
    //   live node in another Event -> false (same OR different Scheduler)
    //
    // Wrong-Event cancellation is a safe false-return case (NOT UB, NOT a debug
    // assertion). The membership check scans THIS Event's own queue under this
    // Scheduler's global_mtx_ + this Event's q.mtx(); it never reads a foreign
    // node's home_ and never locks a foreign Event/Scheduler, so cross-Scheduler
    // wrong-Event cancel is synchronized and structurally safe.
    //
    // This call does NOT expose WaitQueue&, does NOT call RESOURCE_WAKE, and
    // does NOT change Event SET/UNSET. It is NOT a task cancellation token, NOT
    // cancel-all, NOT an Event close, and NOT destructor cancellation.
    [[nodiscard]] bool cancel(WaitNode& node) {
        return scheduler_.event_cancel_wait(waiters_, node);
    }

private:
    Scheduler& scheduler_;
    std::atomic<bool> set_;
    WaitQueue waiters_;
};

}  // namespace sluice::async
