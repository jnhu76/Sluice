// async_test_control.hpp — NON-INSTALLED test-facing control facade.
// (ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1)
//
// Included by test TUs that need to drive deterministic causal seams or the
// test clock/timer. This header is NOT installed and NOT compiled into the
// production `sluice_async` target. Test binaries link the
// `sluice_async_internal_testing` variant (which defines
// SLUICE_ASYNC_INTERNAL_TESTING and compiles the controller).
//
// This facade wraps:
//   - sluice_async_test::test_phase controller (E7 admission, E9 park, E12 event
//     phase arms/releases) — declared in async_test_control_internal.hpp.
//   - Scheduler::AsyncTestAccess (E11 clock/timer) — the guarded accessor struct
//     on Scheduler (compiled only under the define).
//
// The old forgeable friend structs (SchedulerTestHooks, E9ParkSeamHooks,
// E11TimerTestHooks, E12EventTestHooks) are REMOVED. Tests use this facade.
#pragma once

#include "async_test_control_internal.hpp"

#include <sluice/async/detail/mutex_test_seam.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/timer_registration.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <array>
#include <cstddef>
#include <list>

namespace sluice_async_test {

// A RAII guard that registers a controller for a Scheduler on construction and
// unregisters it on destruction. Tests should hold one of these for the
// lifetime of the Scheduler under test. Failing to unregister before Scheduler
// destruction leaves a dangling map entry (benign until the address is reused).
struct ControllerGuard {
    explicit ControllerGuard(sluice::async::Scheduler& s) noexcept : s_(s) {
        register_controller(s_);
    }
    ~ControllerGuard() noexcept { unregister_controller(s_); }
    ControllerGuard(const ControllerGuard&) = delete;
    ControllerGuard& operator=(const ControllerGuard&) = delete;
private:
    sluice::async::Scheduler& s_;
};

// ---- E7 admission seam (was SchedulerTestHooks) ----
// Arm: the worker that reaches MW-S2 Phase-B commit pauses until released.
struct E7AdmissionSeam {
    static void arm(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::mw_admission_phase_b);
    }
    static void wait_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::mw_admission_phase_b);
    }
    static void release(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::mw_admission_phase_b);
    }
};

// ---- E9 park seams (was E9ParkSeamHooks) ----
struct E9ParkSeam {
    static void arm_candidate(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::scheduler_park_candidate);
    }
    static void arm_commit(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::scheduler_park_commit);
    }
    static void wait_candidate_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::scheduler_park_candidate);
    }
    static void wait_commit_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::scheduler_park_commit);
    }
    static bool is_candidate_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::scheduler_park_candidate);
    }
    static bool is_commit_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::scheduler_park_commit);
    }
    static void release_candidate(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::scheduler_park_candidate);
    }
    static void release_commit(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::scheduler_park_commit);
    }
};

// ---- E11 clock/timer control (was E11TimerTestHooks) ----
// Routes through Scheduler::AsyncTestAccess (guarded accessor). The clock/
// timer fields are dual-use production state; only the WRITE/observation
// accessors are test-variant-only.
struct E11TimerControl {
    static void enable_test_clock(sluice::async::Scheduler& s) noexcept {
        sluice::async::Scheduler::AsyncTestAccess::enable_test_clock(s);
    }
    static void set_clock(sluice::async::Scheduler& s,
                          sluice::async::Scheduler::deadline_t t) noexcept {
        sluice::async::Scheduler::AsyncTestAccess::set_clock(s, t);
    }
    static sluice::async::Scheduler::deadline_t now(
        const sluice::async::Scheduler& s) noexcept {
        return sluice::async::Scheduler::AsyncTestAccess::clock_now(s);
    }
    static std::size_t active_deadline_count(
        const sluice::async::Scheduler& s) noexcept {
        return sluice::async::Scheduler::AsyncTestAccess::active_deadline_count(s);
    }
    static std::size_t timer_pool_size(
        const sluice::async::Scheduler& s) noexcept {
        return sluice::async::Scheduler::AsyncTestAccess::timer_pool_size(s);
    }
    static std::size_t deadline_heap_size(
        const sluice::async::Scheduler& s) noexcept {
        return sluice::async::Scheduler::AsyncTestAccess::deadline_heap_size(s);
    }
    static std::size_t timer_pool_count_in_state(
        const sluice::async::Scheduler& s,
        sluice::async::TimerRegistration::State st) noexcept {
        return sluice::async::Scheduler::AsyncTestAccess::timer_pool_count_in_state(s, st);
    }
    static bool earliest_active_deadline(
        sluice::async::Scheduler& s,
        sluice::async::Scheduler::deadline_t& out) noexcept {
        return sluice::async::Scheduler::AsyncTestAccess::earliest_active_deadline(s, out);
    }
    // Register a test deadline from a non-worker thread (the coordinator).
    // Acquires global_mtx_ internally.
    static sluice::async::TimerRegistration* register_test_deadline(
        sluice::async::Scheduler& s,
        sluice::async::WaitNode* node,
        sluice::async::WaitQueue* q,
        sluice::async::Scheduler::deadline_t deadline) {
        return sluice::async::Scheduler::AsyncTestAccess::register_test_deadline(
            s, node, q, deadline);
    }
    // Park-commit seam delegation (E11 reuses the E9 park-commit seam).
    static void arm_park_commit(sluice::async::Scheduler& s) noexcept {
        E9ParkSeam::arm_commit(s);
    }
    static void wait_park_commit_paused(sluice::async::Scheduler& s) noexcept {
        E9ParkSeam::wait_commit_paused(s);
    }
    static void release_park_commit(sluice::async::Scheduler& s) noexcept {
        E9ParkSeam::release_commit(s);
    }
};

// ---- E12 event phase seams (was E12EventTestHooks) ----
struct E12EventSeam {
    static void arm_set_store_before_drain(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::event_set_store_before_drain);
    }
    static void wait_set_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::event_set_store_before_drain);
    }
    static bool is_set_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::event_set_store_before_drain);
    }
    static void release_set(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::event_set_store_before_drain);
    }

    static void arm_admission_before_final_check(
        sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::event_admission_before_final_check);
    }
    static void wait_admission_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::event_admission_before_final_check);
    }
    static bool is_admission_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::event_admission_before_final_check);
    }
    static void release_admission(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::event_admission_before_final_check);
    }

    // E12-A-EVENT-CORRECTIVE-2 (T31): the pre-global-lock admission attempt
    // marker. A causal test observes this is reached while the post-lock
    // admission phase is NOT, proving admission was blocked on global_mtx_.
    static bool admission_attempt_reached(
        sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_reached(
            s, PhaseTag::event_admission_attempt_before_global_lock);
    }
    static void reset_admission_attempt(sluice::async::Scheduler& s) noexcept {
        // Disarm + clear reached so the next wait() re-marks it.
        sluice_async_test::disarm(
            s, PhaseTag::event_admission_attempt_before_global_lock);
        sluice_async_test::clear_reached(
            s, PhaseTag::event_admission_attempt_before_global_lock);
    }

    // Park-commit seam delegation (E12 reuses the E9 park-commit seam for T32).
    static void arm_park_commit(sluice::async::Scheduler& s) noexcept {
        E9ParkSeam::arm_commit(s);
    }
    static void wait_park_commit_paused(sluice::async::Scheduler& s) noexcept {
        E9ParkSeam::wait_commit_paused(s);
    }
    static void release_park_commit(sluice::async::Scheduler& s) noexcept {
        E9ParkSeam::release_commit(s);
    }
};

// ---- E12-C AsyncMutex owner-before-publication seam ----
// The MUTEX-HANDOFF-ONE phase: paused AFTER owner_ = winner Fiber commit, BEFORE
// make_runnable / route_runnable_locked publication. A test observing this phase
// proves owner == winner Fiber, winner not yet published runnable, old owner
// cannot reacquire, and a newcomer try_lock cannot barge.
struct E12MutexSeam {
    static void arm_handoff_before_publication(
        sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(
            s, PhaseTag::mutex_handoff_before_publication);
    }
    static void wait_handoff_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(
            s, PhaseTag::mutex_handoff_before_publication);
    }
    static bool is_handoff_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(
            s, PhaseTag::mutex_handoff_before_publication);
    }
    static void release_handoff(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(
            s, PhaseTag::mutex_handoff_before_publication);
    }
};

// ---- E12-D-CLOSURE: mutex waiter-registered seam (T15a/T15b) ----
// The MUTEX-WAITER-REGISTERED phase: fired inside mutex_lock when a fiber's
// WaitNode has been successfully registered in the Mutex waiter queue AND the
// fiber will suspend (no immediate ownership). A test observing this phase
// proves the node entered the Mutex queue (not immediately granted).
struct E12MutexWaiterSeam {
    static void arm_waiter_registered(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(
            s, PhaseTag::mutex_waiter_registered_before_grant);
    }
    static void wait_waiter_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(
            s, PhaseTag::mutex_waiter_registered_before_grant);
    }
    static bool is_waiter_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(
            s, PhaseTag::mutex_waiter_registered_before_grant);
    }
    static bool is_waiter_registered_reached(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_reached(
            s, PhaseTag::mutex_waiter_registered_before_grant);
    }
    static void release_waiter(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(
            s, PhaseTag::mutex_waiter_registered_before_grant);
    }
};

// ---- E12-D AsyncCondition register-before-release + notify-before-drain seams
// CONDITION-WAIT-PREPARE phase: paused AFTER the Condition node is Registered +
// linked in the Condition queue (Mutex STILL owned by the waiter), BEFORE the
// bound Mutex is released/handed off. Proves InvNoLostNotifyWindow / NEG-C8 and
// that a concurrent notify observes the registered node.
// notify_all phase: paused AFTER acquiring global_mtx_ authority, BEFORE the
// drain loop begins. Proves late registration/cancel/expiry serialize AFTER the
// snapshot (C-H10).
struct E12ConditionSeam {
    static void arm_register_before_handoff(
        sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(
            s, PhaseTag::condition_register_before_handoff);
    }
    static void wait_register_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(
            s, PhaseTag::condition_register_before_handoff);
    }
    static bool is_register_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(
            s, PhaseTag::condition_register_before_handoff);
    }
    static void release_register(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(
            s, PhaseTag::condition_register_before_handoff);
    }

    static void arm_notify_before_drain(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(
            s, PhaseTag::condition_notify_before_drain);
    }
    static void wait_notify_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(
            s, PhaseTag::condition_notify_before_drain);
    }
    static bool is_notify_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(
            s, PhaseTag::condition_notify_before_drain);
    }
    static void release_notify(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(
            s, PhaseTag::condition_notify_before_drain);
    }
};

// ---- ASYNC-MUTEX-NOTHROW acquisition fail-fast seam ----
// (ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 §E/F)
//
// Thin test-facing facade over sluice::async::detail::test_hooks. This struct
// does NOT own fault state; it only arms/disarms the library-internal seam
// counters owned by sluice_async_internal_testing. It exists so death tests
// have a stable test-authority name (MutexFailSeam) that is NOT part of the
// public API (the underlying detail::test_hooks live only under the macro and
// are absent in the production target).
struct MutexFailSeam {
    // Arm the Nth-lock countdown. n==1 fails the next Mutex::lock;
    // n==2 fails the 2nd Mutex::lock (used by the condition_variable_any
    // reacquire death test, where the unique_lock ctor takes the 1st lock).
    static void arm_lock_countdown(unsigned n) noexcept {
        sluice::async::detail::test_hooks::arm_lock_countdown(n);
    }
    static void arm_next_lock_fail() noexcept {
        sluice::async::detail::test_hooks::arm_lock_countdown(1u);
    }
    static void arm_next_try_lock_fail() noexcept {
        sluice::async::detail::test_hooks::arm_next_try_lock_fail();
    }
    static void disarm() noexcept {
        sluice::async::detail::test_hooks::disarm();
    }
};

// ---- E13 P3 Select timer seams + access ----
// Phase seams for the Select timer pump branch (deterministic causal pause),
// reached through the non-installed controller. Plus thin facades over
// Scheduler::AsyncTestAccess for synthetic registration, accounting-helper
// transitions, and pool/heap/counter observation.
struct E13SelectTimerSeam {
    // Pump observing a stale (non-ACTIVE) Select entry being skipped (I4).
    static void arm_pump_skip(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::select_timer_pump_skip);
    }
    static bool pump_skip_reached(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_reached(s, PhaseTag::select_timer_pump_skip);
    }
    static void wait_pump_skip(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_reached(s, PhaseTag::select_timer_pump_skip);
    }
    static void clear_pump_skip(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::disarm(s, PhaseTag::select_timer_pump_skip);
        sluice_async_test::clear_reached(s, PhaseTag::select_timer_pump_skip);
    }

    // Pump paused AFTER the ACTIVE check, BEFORE the fail-fast. A due ACTIVE
    // Select entry is unreachable in valid P3 (no admission); this seam lets a
    // test prove the guard fires. NOT supported production behavior.
    static void arm_pump_active(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::select_timer_pump_active);
    }

    // Synthetic registration / transitions via Scheduler accounting authority.
    using Scheduler = sluice::async::Scheduler;
    using SelectTimerReg = sluice::async::detail::SelectTimerRegistration;
    using ArmSlot = sluice::async::detail::SelectArmSlot;

    static SelectTimerReg* register_synthetic(
        Scheduler& s, ArmSlot* arm, Scheduler::deadline_t deadline) {
        return Scheduler::AsyncTestAccess::register_synthetic_select_timer(
            s, arm, deadline);
    }
    static bool retire_synthetic(Scheduler& s, SelectTimerReg& reg) {
        return Scheduler::AsyncTestAccess::retire_synthetic_select_timer(s, reg);
    }
    static bool consume_synthetic(Scheduler& s, SelectTimerReg& reg) {
        return Scheduler::AsyncTestAccess::consume_synthetic_select_timer(s, reg);
    }

    // Splice a caller-owned temporary node via the REAL production helper, for
    // T2 pre/post-splice address-identity proof. `tmp_pool` loses the node;
    // the returned pointer is the now-Scheduler-owned stable address.
    static SelectTimerReg* splice_one(
        Scheduler& s,
        std::list<SelectTimerReg>& tmp_pool,
        std::list<SelectTimerReg>::iterator it) {
        return Scheduler::AsyncTestAccess::splice_one_for_test(s, tmp_pool, it);
    }

    // Detached-object CAS authority for T1 (reg must NOT be Scheduler-owned).
    // Routes through the guarded AsyncTestAccess entry; the CAS methods
    // themselves are private in the production target.
    static bool detached_try_claim_expiry(SelectTimerReg& reg) noexcept {
        return Scheduler::AsyncTestAccess::detached_try_claim_expiry(reg);
    }
    static bool detached_retire(SelectTimerReg& reg) noexcept {
        return Scheduler::AsyncTestAccess::detached_retire(reg);
    }
    static void advance_clock(Scheduler& s, Scheduler::deadline_t t) {
        Scheduler::AsyncTestAccess::advance_clock(s, t);
    }

    // Observation.
    static std::size_t pool_size(const Scheduler& s) noexcept {
        return Scheduler::AsyncTestAccess::select_timer_pool_size(s);
    }
    static std::size_t count_in_state(
        const Scheduler& s, SelectTimerReg::State st) noexcept {
        return Scheduler::AsyncTestAccess::select_timer_count_in_state(s, st);
    }
    static std::array<std::size_t, 2> heap_counts_by_kind(
        const Scheduler& s) noexcept {
        return Scheduler::AsyncTestAccess::tagged_heap_counts_by_kind(s);
    }
    // Does any Select-kind heap entry target `target` (by address)? For T2.
    static bool heap_has_select_target(
        const Scheduler& s, const SelectTimerReg* target) noexcept {
        return Scheduler::AsyncTestAccess::deadline_heap_has_select_target(
            s, target);
    }
    static std::size_t arm_load_count(const Scheduler& s) noexcept {
        return Scheduler::AsyncTestAccess::select_timer_arm_load_count(s);
    }
    static void reset_arm_load_count(Scheduler& s) noexcept {
        Scheduler::AsyncTestAccess::reset_select_timer_arm_load_count(s);
    }
};

// ---- E13 P5 Select admission seams ----
// Phase seams for the inline admission path (deterministic causal pause),
// reached through the non-installed controller. The admission worker calls
// test_phase at AdmissionArmed / AdmissionClaimed / AdmissionConsumed under
// global_mtx_; a coordinator thread arms + observes while the worker is paused
// there, proving registration-before-snapshot, snapshot-before-finalize, and
// the inline Completed->Consumed lifecycle ordering. No production symbol.
//
// P5 CORRECTIVE: admission boundary snapshots are captured by the admission
// worker under global_mtx_ before each seam, so the test can read the
// mechanical group/arm state without acquiring global_mtx_ (which would
// deadlock if the worker holds it). The snapshot accessor functions return
// the controller-owned copy; they acquire the controller's own mutex, not
// any production lock.
struct E13SelectAdmissionSeam {
    using Scheduler = sluice::async::Scheduler;
    using AdmissionSnapshot = sluice_async_test::AdmissionSnapshot;

    // Non-blocking reach observation. The admission seams fire under
    // global_mtx_; a coordinator thread cannot acquire that lock to observe
    // Scheduler-internal state while the worker is paused at the seam, so the
    // lifecycle tests observe whether each seam was REACHED (in-order) rather
    // than block-and-inspect. `is_reached` reads the controller state under its
    // own mutex (no global_mtx_ acquisition). The blocking arm/wait/release
    // accessors below remain available for seams that do NOT hold global_mtx_.
    static bool armed_reached(Scheduler& s) noexcept {
        return sluice_async_test::is_reached(s, PhaseTag::select_admission_armed);
    }
    static bool claimed_reached(Scheduler& s) noexcept {
        return sluice_async_test::is_reached(s, PhaseTag::select_admission_claimed);
    }
    static bool consumed_reached(Scheduler& s) noexcept {
        return sluice_async_test::is_reached(s, PhaseTag::select_admission_consumed);
    }

    // AdmissionArmed: AFTER every arm registered + phase==Selecting, BEFORE the
    // readiness snapshot. Proves all arms registered before the snapshot.
    static void arm_admission_armed(Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::select_admission_armed);
    }
    static void wait_admission_armed_paused(Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::select_admission_armed);
    }
    static bool is_admission_armed_paused(Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::select_admission_armed);
    }
    static void release_admission_armed(Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::select_admission_armed);
    }

    // AdmissionClaimed: AFTER the winner CAS, BEFORE finalization.
    static void arm_admission_claimed(Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::select_admission_claimed);
    }
    static void wait_admission_claimed_paused(Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::select_admission_claimed);
    }
    static bool is_admission_claimed_paused(Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::select_admission_claimed);
    }
    static void release_admission_claimed(Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::select_admission_claimed);
    }

    // AdmissionConsumed: AFTER phase==Completed, BEFORE Consumed.
    static void arm_admission_consumed(Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::select_admission_consumed);
    }
    static void wait_admission_consumed_paused(Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::select_admission_consumed);
    }
    static bool is_admission_consumed_paused(Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::select_admission_consumed);
    }
    static void release_admission_consumed(Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::select_admission_consumed);
    }

    // ---- P5 CORRECTIVE: admission boundary snapshot accessors ----
    // Read the AdmissionArmed boundary snapshot. The snapshot was captured by
    // the admission worker under global_mtx_ before the readiness snapshot, so
    // it reflects the exact group/arm state at the Armed seam. The test reads
    // it under the controller's own mutex (no global_mtx_ acquisition).
    static AdmissionSnapshot armed_snapshot(Scheduler& s) noexcept {
        return sluice_async_test::read_admission_snapshot(
            s, PhaseTag::select_admission_armed);
    }

    // Read the AdmissionConsumed boundary snapshot. Captured after phase==
    // Completed, before Consumed. Reflects the inline lifecycle state.
    static AdmissionSnapshot consumed_snapshot(Scheduler& s) noexcept {
        return sluice_async_test::read_admission_snapshot(
            s, PhaseTag::select_admission_consumed);
    }
};

// ---- E13 P6 Select publication / suspended-resolution seams ----
// Phase seams + snapshot accessors for the unified publication authority
// (select_publish_locked) and the suspended admission lifecycle (suspend-before-
// switch, suspended-before-consume). Plus the required controller counters
// (task §13): result + runnable publication counts, and the waiting_select_count
// liveness observation. All reached through the non-installed controller; no
// production symbol.
struct E13SelectPublicationSeam {
    using Scheduler = sluice::async::Scheduler;
    using PublicationSnapshot = sluice_async_test::PublicationSnapshot;
    using SelectTimerReg = sluice::async::detail::SelectTimerRegistration;
    using Event = sluice::async::Event;
    using SelectGroup = sluice::async::detail::SelectGroup;

    // ---- suspend-before-switch (caller side, runs OUTSIDE global_mtx_) ----
    static void arm_suspend_before_switch(Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::select_suspend_before_switch);
    }
    static void wait_suspend_before_switch_paused(Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::select_suspend_before_switch);
    }
    static bool is_suspend_before_switch_paused(Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::select_suspend_before_switch);
    }
    static void release_suspend_before_switch(Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::select_suspend_before_switch);
    }

    // ---- publish entry / done / suspended-before-consume (hold global_mtx_) ----
    // Non-blocking reach observation (the publication seams fire under
    // global_mtx_; a coordinator thread cannot acquire that lock).
    static bool publish_entry_reached(Scheduler& s) noexcept {
        return sluice_async_test::is_reached(s, PhaseTag::select_publish_entry);
    }
    static bool publish_done_reached(Scheduler& s) noexcept {
        return sluice_async_test::is_reached(s, PhaseTag::select_publish_done);
    }
    static bool suspended_before_consume_reached(Scheduler& s) noexcept {
        return sluice_async_test::is_reached(s, PhaseTag::select_suspended_before_consume);
    }

    static PublicationSnapshot publish_entry_snapshot(Scheduler& s) noexcept {
        return sluice_async_test::read_publication_snapshot(
            s, PhaseTag::select_publish_entry);
    }
    static PublicationSnapshot publish_done_snapshot(Scheduler& s) noexcept {
        return sluice_async_test::read_publication_snapshot(
            s, PhaseTag::select_publish_done);
    }
    static PublicationSnapshot suspended_before_consume_snapshot(
        Scheduler& s) noexcept {
        return sluice_async_test::read_publication_snapshot(
            s, PhaseTag::select_suspended_before_consume);
    }

    // ---- required controller counters (task §13) ----
    static std::size_t result_publication_count(Scheduler& s) noexcept {
        return sluice_async_test::result_publication_count(s);
    }
    static std::size_t runnable_publication_count(Scheduler& s) noexcept {
        return sluice_async_test::runnable_publication_count(s);
    }
    static void reset_publication_counts(Scheduler& s) noexcept {
        sluice_async_test::reset_publication_counts(s);
    }

    // ---- waiting_select_count liveness observation ----
    static std::size_t waiting_select_count(const Scheduler& s) noexcept {
        return Scheduler::AsyncTestAccess::waiting_select_count(s);
    }

    // ---- Select timer pool observation (mirror of E13SelectTimerSeam) ----
    static std::size_t select_timer_count_in_state(
        const Scheduler& s, SelectTimerReg::State st) noexcept {
        return Scheduler::AsyncTestAccess::select_timer_count_in_state(s, st);
    }
};

// ---- E13 P7 Select registration-rollback seam + observability (task §18/§19)
// Thin test-facing facade over the non-installed controller. The production
// library exposes NONE of this: the synthetic SelectRegistrationFailure type,
// the failure controller, and the rollback observation are controller-only and
// absent from production symbols (the production select_admit has no injection
// branch; this header is never seen by a production TU).
//
// fail_after semantics: N = "throw the synthetic exception immediately AFTER
// exactly N successful arm registrations, before the next one / before
// FinishRegistration". 0 = before the first registration; arm_count = all arms
// registered then fail before FinishRegistration (P7-T5, load-bearing).
struct E13SelectRollbackSeam {
    using Scheduler = sluice::async::Scheduler;
    using Observation = sluice_async_test::RollbackObservation;
    using ArmKind = sluice::async::detail::ArmKind;

    // Configure the synthetic failure boundary for the next select() admission.
    static void configure_fail_after(Scheduler& s, std::size_t n) noexcept {
        sluice_async_test::configure_rollback_fail_after(s, n);
    }
    // Disable injection.
    static void disable(Scheduler& s) noexcept {
        sluice_async_test::reset_rollback_injection(s);
    }
    static std::size_t configured_fail_after(Scheduler& s) noexcept {
        return sluice_async_test::rollback_fail_after(s);
    }
    static constexpr std::size_t disabled =
        sluice_async_test::kRollbackFailAfterDisabled;

    // Read the rollback observation captured by the last failing admission.
    static Observation observation(Scheduler& s) noexcept {
        return sluice_async_test::read_rollback_observation(s);
    }

    // Decode helpers for the raw arm-kind stored in the observation.
    static bool kind_is_event(std::uint8_t raw) noexcept {
        return raw == static_cast<std::uint8_t>(ArmKind::event);
    }
    static bool kind_is_timer(std::uint8_t raw) noexcept {
        return raw == static_cast<std::uint8_t>(ArmKind::timer);
    }

    // The synthetic exception type (rethrown to the test by select_admit's catch).
    using FailureException = sluice_async_test::SelectRegistrationFailure;
};

// Short alias used by the P6 tests.
using AsyncTestAccess = sluice::async::Scheduler::AsyncTestAccess;

}  // namespace sluice_async_test
