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

#include <sluice/async/scheduler.hpp>
#include <sluice/async/timer_registration.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <cstddef>

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
        sluice_async_test::arm(s, PhaseTag::e7_admission_phase_b);
    }
    static void wait_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::e7_admission_phase_b);
    }
    static void release(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::e7_admission_phase_b);
    }
};

// ---- E9 park seams (was E9ParkSeamHooks) ----
struct E9ParkSeam {
    static void arm_candidate(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::e9_park_candidate);
    }
    static void arm_commit(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::e9_park_commit);
    }
    static void wait_candidate_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::e9_park_candidate);
    }
    static void wait_commit_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::e9_park_commit);
    }
    static bool is_candidate_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::e9_park_candidate);
    }
    static bool is_commit_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::e9_park_commit);
    }
    static void release_candidate(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::e9_park_candidate);
    }
    static void release_commit(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::e9_park_commit);
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
        sluice_async_test::arm(s, PhaseTag::e12_set_store_before_drain);
    }
    static void wait_set_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::e12_set_store_before_drain);
    }
    static bool is_set_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::e12_set_store_before_drain);
    }
    static void release_set(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::e12_set_store_before_drain);
    }

    static void arm_admission_before_final_check(
        sluice::async::Scheduler& s) noexcept {
        sluice_async_test::arm(s, PhaseTag::e12_admission_before_final_check);
    }
    static void wait_admission_paused(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::wait_paused(s, PhaseTag::e12_admission_before_final_check);
    }
    static bool is_admission_paused(sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_paused(s, PhaseTag::e12_admission_before_final_check);
    }
    static void release_admission(sluice::async::Scheduler& s) noexcept {
        sluice_async_test::release(s, PhaseTag::e12_admission_before_final_check);
    }

    // E12-A-EVENT-CORRECTIVE-2 (T31): the pre-global-lock admission attempt
    // marker. A causal test observes this is reached while the post-lock
    // admission phase is NOT, proving admission was blocked on global_mtx_.
    static bool admission_attempt_reached(
        sluice::async::Scheduler& s) noexcept {
        return sluice_async_test::is_reached(
            s, PhaseTag::e12_admission_attempt_before_global_lock);
    }
    static void reset_admission_attempt(sluice::async::Scheduler& s) noexcept {
        // Disarm + clear reached so the next wait() re-marks it.
        sluice_async_test::disarm(
            s, PhaseTag::e12_admission_attempt_before_global_lock);
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

}  // namespace sluice_async_test
