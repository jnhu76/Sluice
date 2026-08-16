// Scheduler Event primitive — implementation TU split from scheduler.cpp in the
// post-freeze R1 structural pass (docs/post-freeze/structural-audit.md §6).
//
// Pure relocation: every definition below is byte-identical to its pre-split
// text at d9184de; the class declaration, lock domains, atomic orderings,
// and wake contracts remain in include/sluice/async/scheduler.hpp.
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
std::size_t Scheduler::event_set_broadcast(Event& event) {
    LockGuard lk(global_mtx_);
    bool previous = event.set_.exchange(true, std::memory_order::release);
    if (previous) {
        return 0;
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::event_set_store_before_drain);
#endif
    std::size_t woken = 0;
    while (wake_wait_one_locked(event.waiters_) != nullptr) {
        ++woken;
    }
    // P6: the suspended-Event resolver. Replaces the P2 readiness-offer-only
    // select_event_scan_locked. select_resolve_event_locked walks this Event's
    // SelectPort, applies the single-group P6 gate (P8 multi-group DENIED ->
    // fail-fast before any CAS), marks eligible arms CandidateReady, chooses
    // the lowest INDEX ready arm, drives the P4 group processor exactly once,
    // and publishes exactly once. A zero-eligible return is a clean no-op (no
    // suspended Select arms on this Event).
    (void)select_resolve_event_locked(event);
    return woken;
}

void Scheduler::event_reset(std::atomic<bool>& set_flag) {
    // Transition `set_flag` to UNSET. Pure state flip: does NOT resolve, cancel,
    // expire, unlink, or publish any WaitNode. A waiter already registered
    // remains governed by future set(), deadline, or cancellation. Linearized
    // under global_mtx_ so it serializes with set()'s drain and wait admission
    // (the set/reset epoch isolation domain).
    LockGuard lk(global_mtx_);
    set_flag.store(false, std::memory_order::release);
}

// ---- E13 Select registry operations (private Scheduler authority) ----

void Scheduler::select_event_link_locked(Event& event,
                                         detail::SelectArmSlot& arm) {
    // Event must belong to this Scheduler.
    assert(&event.scheduler_ == this &&
           "select_event_link_locked: Event does not belong to this Scheduler");
    if (&event.scheduler_ != this) detail::select_invariant_fail_fast();
    // Precondition: arm is not already linked.
    // The caller (future select() admission) is responsible for setting
    // arm.state to Prepared and arm.group to the owning SelectGroup.
    assert(arm.home_ == nullptr &&
           "select_event_link_locked: arm already linked");
    if (arm.home_ != nullptr) detail::select_invariant_fail_fast();
    assert(arm.next_ == nullptr &&
           "select_event_link_locked: arm.next_ not null");
    if (arm.next_ != nullptr) detail::select_invariant_fail_fast();
    assert(arm.prev_ == nullptr &&
           "select_event_link_locked: arm.prev_ not null");
    if (arm.prev_ != nullptr) detail::select_invariant_fail_fast();
    assert(arm.kind == detail::ArmKind::event &&
           "select_event_link_locked: arm kind must be event");
    if (arm.kind != detail::ArmKind::event)
        detail::select_invariant_fail_fast();
    assert(arm.event.event_ == &event &&
           "select_event_link_locked: arm.event does not point to this Event");
    if (arm.event.event_ != &event) detail::select_invariant_fail_fast();
    assert((arm.state == detail::ArmState::detached ||
            arm.state == detail::ArmState::prepared) &&
           "select_event_link_locked: arm state must be Detached or Prepared");
    if (arm.state != detail::ArmState::detached &&
        arm.state != detail::ArmState::prepared) {
        detail::select_invariant_fail_fast();
    }
    assert(arm.group != nullptr &&
           "select_event_link_locked: arm.group must be set");
    if (arm.group == nullptr) detail::select_invariant_fail_fast();

    arm.home_ = &event.select_port_;
    arm.state = detail::ArmState::registered;

    // Insert at head of the doubly-linked list.
    detail::SelectPort& port = event.select_port_;
    arm.next_ = port.head_;
    if (port.head_ != nullptr) {
        port.head_->prev_ = &arm;
    }
    arm.prev_ = nullptr;
    port.head_ = &arm;
}

void Scheduler::select_event_unlink_locked(Event& event,
                                           detail::SelectArmSlot& arm) {
    assert(&event.scheduler_ == this &&
           "select_event_unlink_locked: Event does not belong to this Scheduler");
    if (&event.scheduler_ != this) detail::select_invariant_fail_fast();
    // Validate that the arm belongs to this Event's port.
    assert(arm.home_ == &event.select_port_ &&
           "select_event_unlink_locked: arm does not belong to this Event");
    if (arm.home_ != &event.select_port_)
        detail::select_invariant_fail_fast();
    assert((arm.state == detail::ArmState::registered ||
            arm.state == detail::ArmState::candidate_ready ||
            arm.state == detail::ArmState::retired) &&
           "select_event_unlink_locked: unexpected arm state");
    if (arm.state != detail::ArmState::registered &&
        arm.state != detail::ArmState::candidate_ready &&
        arm.state != detail::ArmState::retired) {
        detail::select_invariant_fail_fast();
    }

    detail::SelectPort& port = event.select_port_;

    // Repair predecessor link.
    if (arm.prev_ != nullptr) {
        arm.prev_->next_ = arm.next_;
    } else {
        // Arm is the head.
        port.head_ = arm.next_;
    }

    // Repair successor link.
    if (arm.next_ != nullptr) {
        arm.next_->prev_ = arm.prev_;
    }

    // Clear arm linkage.
    arm.next_ = nullptr;
    arm.prev_ = nullptr;
    arm.home_ = nullptr;
}

std::size_t Scheduler::select_event_scan_locked(Event& event) {
    assert(&event.scheduler_ == this &&
           "select_event_scan_locked: Event does not belong to this Scheduler");
    if (&event.scheduler_ != this) detail::select_invariant_fail_fast();
    // Walk the Event's SelectPort, marking eligible Event Select arms
    // CandidateReady. P2: readiness-offer only — no claim, no finalization,
    // no publication, no unlink, no worklist construction.
    std::size_t marked = 0;
    detail::SelectArmSlot* arm = event.select_port_.head_;
    while (arm != nullptr) {
        detail::SelectArmSlot* next = arm->next_;
        if (arm->kind == detail::ArmKind::event &&
            arm->state == detail::ArmState::registered &&
            arm->group != nullptr &&
            arm->group->phase() == detail::GroupPhase::armed &&
            arm->home_ == &event.select_port_ &&
            arm->event.event_ == &event) {
            arm->state = detail::ArmState::candidate_ready;
            ++marked;
        }
        arm = next;
    }
    return marked;
}


bool Scheduler::event_cancel_wait(WaitQueue& q, WaitNode& node) {
    // E12-A-EVENT-CORRECTIVE-2: the narrow Event cancellation authority with
    // EXACT queue-membership validation. Event::cancel passes its private
    // waiters_ here (NOT exposed to the caller). The contract (Corrective C):
    //   returns true ONLY if node is currently Registered AND currently linked
    //   in THIS Event's private WaitQueue AND CANCEL wins node.resolve_.
    //   Otherwise returns false WITHOUT mutation.
    //
    // The membership check scans THIS queue's own intrusive list for &node
    // while holding this Scheduler's global_mtx_ + this Event's q.mtx(). It
    // does NOT read a foreign node's home_, does NOT lock a foreign Event or
    // foreign Scheduler, and does NOT depend on cross-Scheduler
    // synchronization. Wrong-Event (same OR different Scheduler), detached,
    // Woken, Expired, and Cancelled nodes all return false safely.
    //
    // Generic Scheduler::cancel_wait is unchanged (its caller contract already
    // guarantees membership); this Event-specific path is the one reached from
    // untrusted Event::cancel callers. The resolve_ CAS remains the
    // terminal-winner authority; contains_locked is the membership gate, taken
    // BEFORE resolve_ so no mutation occurs on a non-member. This call CANNOT
    // synthesize a RESOURCE_WAKE and CANNOT change Event SET/UNSET.
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    // Membership gate: a node not linked in THIS queue is not cancellable here.
    // Covers wrong-Event (same/different Scheduler), detached, and (because a
    // terminal winner is already unlinked) Woken/Expired/Cancelled nodes.
    if (!q.contains_locked(node)) return false;
    if (!q.cancel_locked(node)) return false;  // concurrent resolver won (loser)
    retire_timer_for_node_locked(node);
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // The cancel CAS won: the node is terminal+unlinked and the count is closed.
    // Return true unconditionally — the winner identity is the resolve_ CAS, not
    // the runnable publication (mirrors wake_wait_one_locked). make_runnable is
    // the exactly-once publication guard; a false return (fiber already runnable
    // from a concurrent path, or null fiber) does NOT undo the cancel. Returning
    // false here would mislead the caller into retrying or thinking the wait is
    // still active (PR#6 review: gemini-code-assist + coderabbitai).
    // I47-F1: route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr) {
        publish_waiting_fiber_runnable_locked(f);
    }
    return true;
}

void Scheduler::await_event_wait(WaitQueue& q, const std::atomic<bool>& set_flag,
                                 WaitNode& node) {
    // E12-A Event wait admission. The lost-set closure: register + check SET +
    // (if SET) resolve Woken inline, OR commit suspension — all under one
    // global_mtx_ + q.mtx() critical section (the same domain set()/reset() use).
    // Only context_switch is outside the lock. This mirrors await_wait_deadline's
    // I5 already-due path: always register, then check the admission condition.
    //
    // If set_ is observed at admission (after registration), the wait resolves
    // Woken inline via wake_node_locked (resolve_(Woken) + unlink), the timer
    // (if any — none for this non-deadline overload) is retired, the count is
    // decremented, and the fiber does NOT suspend. Because the current Fiber has
    // not yet committed `waiting`, a successful admission-time Woken resolution
    // may cause make_runnable() to return false for the RUNNING Fiber — that is
    // expected and harmless (the fiber continues running and returns from wait).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    // E12-A-EVENT-CORRECTIVE-2 (T31): mark that an admission attempt has begun,
    // BEFORE acquiring global_mtx_. A causal test observes this marker is set
    // while a setter holds global_mtx_ mid-drain, proving the admission could
    // not have entered its critical section yet. Controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::event_admission_attempt_before_global_lock);
#endif
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, me)) {
            // Node already registered or terminal (C8): contract violation.
            return;
        }
        ++waiting_waitq_count_;
        // E12-A-EVENT-CORRECTIVE-1 (Corrective D): deterministic admission-before-
        // final-set-check phase seam. When armed, pause the admission thread
        // AFTER registration and while it STILL HOLDS global_mtx_+q.mtx(), BEFORE
        // the final SET check. This lets a causal test mechanically prove:
        //   - admission-first: set()'s drain cannot complete until admission
        //     releases serialization (a competing setter blocks on global_mtx_).
        //   - set-first: if the setter stores SET first, admission (paused here
        //     or about to run) cannot complete its drain until the setter
        //     releases; admission then observes SET and resolves Woken inline.
        // The seam blocks on its OWN mtx/cv (the production locks remain held),
        // which is precisely the guarantee under test.
        // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(*this,
            sluice_async_test::PhaseTag::event_admission_before_final_check);
#endif
        // Admission closure: if SET is observed after registration, resolve this
        // wait as Woken inline through the canonical resolve_ authority. The node
        // is unlinked in the same critical section (wake_node_locked). No suspend.
        if (set_flag.load(std::memory_order::acquire)) {
            if (q.wake_node_locked(node)) {
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING; make_runnable may return false.
                // That is not a reason to publish it. Return from wait normally.
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }
        // Defense-in-depth: if the node was resolved concurrently (it cannot be,
        // since register_wait_locked just moved it to Registered under both
        // locks and every resolver takes global_mtx_), undo and do not suspend.
        if (node.is_terminal()) {
            q.unlink_locked(node);
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

void Scheduler::await_event_wait_deadline(WaitQueue& q,
                                          const std::atomic<bool>& set_flag,
                                          WaitNode& node, deadline_t deadline) {
    // E12-A deadline-aware Event wait. Composes await_event_wait's admission
    // closure with E11 TimerRegistration. The wait resolves when EXACTLY ONE
    // cause wins the resolve_ CAS:
    //   - set() broadcast (event_set_broadcast -> wake_wait_one_locked) -> Woken
    //   - cancel_wait(q, node)                                   -> Cancelled
    //   - the deadline elapsing (pump_deadlines_locked)           -> Expired
    //
    // Admission precedence (under global_mtx_ + q.mtx()):
    //   1. If set_ is observed SET after registration: resolve Woken inline
    //      (no suspend). Event readiness wins over a due deadline at admission
    //      (the resource is ready; the deadline is moot).
    //   2. Else if the deadline is already due: resolve Expired inline (E11 I5).
    //   3. Else: commit suspension.
    // A non-timer winner retires the registration in the same CS (E11 I4).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, me)) {
            return;  // C8 contract violation
        }
        ++waiting_waitq_count_;
        // Create the timer registration control block for this wait epoch.
        timer_pool_.emplace_back(&node, &q, deadline);
        reg = &timer_pool_.back();
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();

        // Admission closure — Event SET takes precedence: if the resource is
        // ready, the wait resolves Woken inline (the deadline is moot).
        if (set_flag.load(std::memory_order::acquire)) {
            if (q.wake_node_locked(node)) {
                if (reg->retire()) {  // ACTIVE->RETIRED (closes timer authority)
                    --active_deadline_count_;
                }
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // E11 I5 admission closure: if the deadline is ALREADY due (and the
        // resource is NOT set), resolve Expired inline. The fiber must NOT
        // suspend and wait for a future timer scan merely because registration
        // happened after the deadline was due.
        if (clock_now_unlocked() >= deadline) {
            if (q.expire_locked(node)) {
                reg->try_claim_expiry();  // ACTIVE->CONSUMED (winner)
                --active_deadline_count_;
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING (has not called make_waiting());
                // make_runnable() returns false for a Running fiber (E7-T2).
                // Call it for state consistency but do NOT route - matches the
                // inline SET admission path above.
                if (me != nullptr) (void)me->make_runnable();
                return;  // resolved at admission; do NOT suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through.
        }

        // Defense-in-depth: if the node was resolved concurrently, undo + return.
        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            if (reg->retire()) {
                --active_deadline_count_;
            }
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

}  // namespace sluice::async
