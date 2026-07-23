// async_test_control_internal.hpp — NON-INSTALLED internal-testing control.
// (ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1)
//
// This header is reachable ONLY when SLUICE_ASYNC_INTERNAL_TESTING is defined.
// It is included by scheduler.cpp (under the guard) so the causal phase call
// sites resolve to the controller, and by the test-support objects that define
// the controller. The production `sluice_async` target does NOT define the
// macro, so this header is never seen by a production TU.
//
// Architecture:
//   - scheduler.cpp calls sluice_async_test::test_phase(scheduler, tag) at each
//     causal boundary (E7 admission, E9 park candidate/commit, E12 set-store/
//     admission). The function looks up pre-registered per-Scheduler controller
//     state by pointer, marks the phase reached, and (if armed) blocks until
//     released. It performs NO allocation, NO insertion/erase/rehash on the hot
//     path (the lookup is into a pre-populated map), executes NO arbitrary
//     callback, calls NO Scheduler/Event operation, and throws nothing.
//   - The controller state (mutex/cv/atomic-reached/armed flags) is owned by
//     the test-support object, keyed on Scheduler*. It is NOT a Scheduler field.
//   - Tests register a controller for a Scheduler via register_controller(),
//     arm/wait/release phases, then unregister before Scheduler destruction.
//
// No Scheduler/Event friend is involved. The dual-use clock/timer access goes
// through Scheduler::AsyncTestAccess (also guarded by the macro).
#pragma once

#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/scheduler.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace sluice_async_test {

// The causal phase tags. Each corresponds to a call site in scheduler.cpp.
// Adding a tag requires adding the corresponding call site; removing a call
// site requires removing the tag (the controller asserts on unknown tags).
enum class PhaseTag : unsigned char {
    // E7-T11: worker paused at MW-S2 Phase-B commit boundary.
    e7_admission_phase_b,
    // E9-CORRECTIVE: worker paused at ParkCandidate boundary (pre-physical-wait).
    e9_park_candidate,
    // E9-CORRECTIVE: worker paused at park commit boundary (pre-wake_cv.wait).
    e9_park_commit,
    // E12-A: setter paused after SET store, holding global_mtx_, before drain.
    e12_set_store_before_drain,
    // E12-A-EVENT-CORRECTIVE-2 (T31): admission attempt marker, BEFORE taking
    // global_mtx_. Marks that wait() was called but has not entered its CS.
    e12_admission_attempt_before_global_lock,
    // E12-A: admission paused after registration, holding global_mtx_+q.mtx(),
    // before the final SET check.
    e12_admission_before_final_check,
    // E12-C: MUTEX-HANDOFF-ONE paused AFTER owner commit (owner_ == winner
    // Fiber), BEFORE make_runnable / route_runnable_locked publication. Holding
    // global_mtx_ (+ waiters_.mtx() inside). Proves owner-before-publication.
    e12_mutex_handoff_before_publication,
    // E12-D: CONDITION-WAIT-PREPARE paused AFTER the Condition node is Registered
    // + linked in the Condition queue, holding global_mtx_, BEFORE the bound
    // Mutex is released/handed off. A test observing this phase proves the
    // register-before-release ordering (InvNoLostNotifyWindow / NEG-C8) and that
    // a concurrent notify sees the registered node while the Mutex is still
    // owned. The Condition queue mtx has been released; only global_mtx_ held.
    e12_condition_register_before_handoff,
    // E12-D: notify_all paused AFTER acquiring global_mtx_ authority, BEFORE the
    // drain loop begins. A test observing this phase proves late registration /
    // cancel / expiry serialize AFTER the snapshot (they need global_mtx_).
    e12_condition_notify_before_drain,
    // E12-D-CLOSURE: mutex_lock queuing path. Fires AFTER register_wait_locked
    // succeeds and the fiber WILL suspend (no immediate ownership). Proves this
    // fiber's WaitNode is registered in the Mutex waiter queue (T15a/T15b).
    e12_mutex_waiter_registered_before_grant,

    // E13 P3: Select timer pump paused AFTER the ACTIVE check, BEFORE the
    // arm dereference / fail-fast. A due ACTIVE Select entry is unreachable in
    // valid P3 (no admission); observing this phase proves the pump reached
    // an ACTIVE Select entry and is about to fail fast (stage-boundary guard,
    // NOT supported production behavior).
    e13_timer_pump_active,
    // E13 P3: Select timer pump observing a stale (non-ACTIVE) entry being
    // skipped — the deterministic proof of the I4 closure: the pump observed
    //    RETIRED/CONSUMED and did NOT read arm_ (arm-load delta == 0).
    e13_timer_pump_skip,

    // E13 P4: Timer loser arm classified Retired, with the registration STILL
    // ACTIVE, immediately BEFORE the ACTIVE->RETIRED retire CAS. The load-
    // bearing SN-9 ordering: a test observing this phase proves arm.state
    // became Retired WHILE the registration was still ACTIVE, then the retire
    // CAS (below this seam) flipped it to RETIRED. No wall-clock timing.
    e13_timer_loser_arm_classified,

    // E13 P5: admission armed — AFTER every arm is registered AND group phase
    // becomes Selecting, BEFORE the readiness snapshot. A test observing this
    // phase proves all arms were registered before the snapshot was taken (no
    // early-registration shortcut) and that no winner/result/runnable exists
    // yet. The seam blocks the admission worker under global_mtx_, so a
    // coordinator thread can inspect the registered group deterministically.
    e13_admission_armed,
    // E13 P5: admission claimed — AFTER the winner CAS succeeds (fresh claim),
    // BEFORE winner/loser finalization. Fires only on a real claim attempt that
    // won (not on claim-lost). A test observing this phase proves the snapshot's
    // chosen candidate index was committed to the group before any arm was
    // finalized. Reached inside select_process_group_locked; does NOT change P4
    // production semantics (it is a pure observation with no blocking unless
    // armed by a registered controller).
    e13_admission_claimed,
    // E13 P5: admission consumed — AFTER inline phase becomes Completed,
    // BEFORE phase becomes Consumed. A test observing this phase proves the
    // inline result is committed, every authority is closed, completion_mode is
    // Inline, and runnable delta is 0 — the inline lifecycle ordering.
    e13_admission_consumed,

    // E13 P6: caller Fiber paused AFTER the no-ready suspension commit
    // (make_waiting + phase Armed + waiting_select_count_++), AFTER global_mtx_
    // is released, BEFORE the physical context_switch. A coordinator thread can
    // deterministically resolve the group (Event::set / clock advance) and
    // prove the wake-before-physical-switch window is closed (the caller is
    // queued exactly once even if it has not yet switched away). The seam runs
    // OUTSIDE global_mtx_ (the caller has released it).
    e13_select_suspend_before_switch,
    // E13 P6: select_publish_locked entry, BEFORE any result mutation. A test
    // observing this phase proves publication-entry preconditions hold (winner
    // exists, all authority closed, result not yet written, completion_mode
    // none, phase Selecting/Armed). Holds global_mtx_ (use a snapshot).
    e13_publish_entry,
    // E13 P6: AFTER phase becomes Completed and, for suspended mode, AFTER
    // successful make_runnable + route_runnable_locked. A test observing this
    // phase proves the suspended publication committed result + runnable once
    // (or inline published result once with no runnable). Holds global_mtx_.
    e13_publish_done,
    // E13 P6: AFTER the resumed caller reacquires global_mtx_ and validates the
    // result, BEFORE phase becomes Consumed. A test observing this phase proves
    // phase==Completed, mode==Suspended, result present, winner stable, all
    // authority closed, caller Running, runnable publication count==1.
    // Holds global_mtx_.
    e13_suspended_before_consume,

    count
};

// Per-PhaseTag controller state. Owned by the controller registry, keyed on
// Scheduler*. All fields are guarded by `mtx` (the phase's own coordination
// mutex — distinct from any production lock).
struct PhaseState {
    std::mutex mtx;
    std::condition_variable cv;
    bool armed = false;
    bool reached = false;  // the phase call site was reached (set under mtx)
    bool paused = false;   // the phase is blocked waiting for release
};

// ---- E13 P5 CORRECTIVE: admission boundary snapshot ----
// Captured by the admission worker under global_mtx_ immediately before each
// seam, then read by the coordinator thread under the controller's own mutex
// (no global_mtx_ acquisition). Only the two expected PhaseTag values are
// valid: e13_admission_armed and e13_admission_consumed.
struct AdmissionSnapshot {
    sluice::async::detail::GroupPhase phase{sluice::async::detail::GroupPhase::building};
    sluice::async::detail::CompletionMode completion_mode{sluice::async::detail::CompletionMode::none};
    std::uint32_t winner{sluice::async::detail::kNoWinner};
    std::size_t arm_count{0};
    std::array<sluice::async::detail::ArmState, sluice::async::kSelectMaxArms> arm_states{};
    std::array<sluice::async::detail::ArmKind, sluice::async::kSelectMaxArms> arm_kinds{};
    std::array<bool, sluice::async::kSelectMaxArms> event_linked{};
    std::array<sluice::async::detail::SelectTimerRegistration::State,
               sluice::async::kSelectMaxArms> timer_states{};
    bool all_authority_closed{false};
};

// ---- E13 P6: publication boundary snapshot ----
// Captured by the publication / resume paths under global_mtx_ immediately
// before each P6 seam, then read by the coordinator under the controller's own
// mutex. Value-initialized (every field has a default member initializer) so an
// unused union payload slot is never read (task §13: "Do not read an inactive
// union member when capturing diagnostics"). Result kind is captured as a
// plain enum (no payload union) so the snapshot never dereferences a SelectArm
// union member — only the publication path reads the active member.
struct PublicationSnapshot {
    sluice::async::detail::GroupPhase phase{sluice::async::detail::GroupPhase::building};
    sluice::async::detail::CompletionMode completion_mode{sluice::async::detail::CompletionMode::none};

    std::uint32_t winner{sluice::async::detail::kNoWinner};
    std::size_t arm_count{0};

    bool result_has_winner{false};
    std::size_t result_index{0};
    sluice::async::SelectKind result_kind{sluice::async::SelectKind::event};

    bool all_authority_closed{false};

    sluice::async::FiberState caller_state{sluice::async::FiberState::created};
    unsigned caller_owner_id{0};

    std::size_t waiting_select_count{0};
    std::size_t result_publication_count{0};
    std::size_t runnable_publication_count{0};
};

// The controller entry for one Scheduler. Holds one PhaseState per tag. The
// array is indexed by PhaseTag (cast to size_t). Lookups are O(1).
struct SchedulerController {
    PhaseState phases[static_cast<std::size_t>(PhaseTag::count)]{};
    // E13 P5 CORRECTIVE: fixed-size boundary snapshots, populated by the
    // admission worker under global_mtx_ before each seam, read by the test
    // coordinator under the controller's own mutex. Only valid when the
    // corresponding phase has been reached.
    AdmissionSnapshot admission_armed_snapshot{};
    AdmissionSnapshot admission_consumed_snapshot{};

    // E13 P6: publication boundary snapshots, populated by the publication /
    // resume paths under global_mtx_ before each P6 seam, read by the test
    // coordinator under the controller's own mutex. Plus the two required
    // controller counters (task §13): result + runnable publication counts,
    // incremented once per publication inside select_publish_locked (under
    // global_mtx_; read here under the controller's own mutex).
    PublicationSnapshot publish_entry_snapshot{};
    PublicationSnapshot publish_done_snapshot{};
    PublicationSnapshot suspended_before_consume_snapshot{};
    std::size_t result_publication_count{0};
    std::size_t runnable_publication_count{0};
};

// --- Called from scheduler.cpp (under SLUICE_ASYNC_INTERNAL_TESTING) ---
// Marks `tag` reached for `s`. If the phase is armed, blocks the caller until
// the phase is released by the test. No allocation; the controller for `s` must
// already be registered (register_controller). If unregistered, this is a no-op
// (the phase was reached but no test is observing — safe for production paths
// that happen to be compiled into the variant without a test driver).
void test_phase(sluice::async::Scheduler& s, PhaseTag tag) noexcept;

// E13 P5 CORRECTIVE: capture an admission boundary snapshot into the
// controller's snapshot storage. Must be called from the admission worker
// under global_mtx_, immediately before the corresponding test_phase() call.
// The snapshot is read by the test coordinator under the controller's own
// mutex (no global_mtx_ acquisition). No-op if `s` has no registered controller.
void capture_admission_snapshot(sluice::async::Scheduler& s, PhaseTag tag,
                                const AdmissionSnapshot& snap) noexcept;

// Read the admission boundary snapshot for a given phase tag. The snapshot
// must have been populated by a prior capture_admission_snapshot call (the
// caller should verify the phase was reached first). Returns a default-
// constructed snapshot if no controller is registered for `s`.
AdmissionSnapshot read_admission_snapshot(sluice::async::Scheduler& s,
                                           PhaseTag tag) noexcept;

// E13 P6: capture a publication boundary snapshot (same discipline as the
// admission snapshot: captured under global_mtx_ before the P6 seam, read by
// the coordinator under the controller's own mutex). Valid tag values:
// e13_publish_entry, e13_publish_done, e13_suspended_before_consume. No-op if
// `s` has no registered controller.
void capture_publication_snapshot(sluice::async::Scheduler& s, PhaseTag tag,
                                  const PublicationSnapshot& snap) noexcept;

// Read a publication boundary snapshot. Returns a default-constructed snapshot
// if no controller is registered for `s` or the phase has not been reached.
PublicationSnapshot read_publication_snapshot(sluice::async::Scheduler& s,
                                              PhaseTag tag) noexcept;

// E13 P6: increment the result / runnable publication counters (task §13).
// Called once per publication inside select_publish_locked (under global_mtx_).
// No-op if `s` has no registered controller.
void increment_result_publication(sluice::async::Scheduler& s) noexcept;
void increment_runnable_publication(sluice::async::Scheduler& s) noexcept;

// Read the result / runnable publication counters. Returns 0 if `s` has no
// registered controller.
std::size_t result_publication_count(sluice::async::Scheduler& s) noexcept;
std::size_t runnable_publication_count(sluice::async::Scheduler& s) noexcept;

// Reset the result / runnable publication counters (for delta-based tests).
void reset_publication_counts(sluice::async::Scheduler& s) noexcept;

// Release ALL armed phases for `s` (used by run-termination paths so a paused
// test worker observes termination). No-op if `s` has no controller.
void release_all_phases(sluice::async::Scheduler& s) noexcept;

// --- Called from the test-support controller (test TUs) ---
// Register/unregister a controller for `s`. register_controller MUST be called
// before any test_phase call site can fire for `s` during a test; unregister
// MUST be called before `s` is destroyed. The registry is a small fixed map.
void register_controller(sluice::async::Scheduler& s) noexcept;
void unregister_controller(sluice::async::Scheduler& s) noexcept;

// Arm/wait/observe/release a specific phase. The test coordinator thread calls
// these; the worker thread calls test_phase (which blocks on the same state).
void arm(sluice::async::Scheduler& s, PhaseTag tag) noexcept;
void wait_reached(sluice::async::Scheduler& s, PhaseTag tag) noexcept;
bool is_reached(sluice::async::Scheduler& s, PhaseTag tag) noexcept;
void wait_paused(sluice::async::Scheduler& s, PhaseTag tag) noexcept;
bool is_paused(sluice::async::Scheduler& s, PhaseTag tag) noexcept;
void release(sluice::async::Scheduler& s, PhaseTag tag) noexcept;
void disarm(sluice::async::Scheduler& s, PhaseTag tag) noexcept;
void clear_reached(sluice::async::Scheduler& s, PhaseTag tag) noexcept;

}  // namespace sluice_async_test
