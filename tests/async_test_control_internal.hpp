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
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace sluice_async_test {

// The causal phase tags. Each corresponds to a call site in scheduler.cpp.
// Adding a tag requires adding the corresponding call site; removing a call
// site requires removing the tag (the controller asserts on unknown tags).
enum class PhaseTag : unsigned char {
    // Issue #50: run_impl paused at worker-topology mutation. The corrected
    // implementation reaches this while holding global_mtx_.
    worker_topology_mutation,
    // Issue #50: spawn reached the topology-reader attempt boundary,
    // immediately before acquiring global_mtx_.
    worker_topology_reader_attempt,
    // Issue #50: topology setup is complete and run_impl is paused before
    // starting any worker loop. Reached with global_mtx_ released.
    worker_topology_ready_before_start,
    // Issue #50: every worker loop has joined, but the active topology has not
    // yet been unpublished. Reached with global_mtx_ released.
    worker_topology_joined_before_unpublish,
    // E7-T11: worker paused at MW-S2 Phase-B commit boundary.
    mw_admission_phase_b,
    // D4-RM14 (P0-1): MW-S2 participant paused AFTER its commit-to-park
    // registration (arm under global_mtx_) and BEFORE entering ctx_.wait_one().
    // Runs OUTSIDE global_mtx_. A test injects request_stop() here and proves
    // the armed baseline makes the upcoming wait_one() observe the interrupt
    // (register -> publish -> release -> wait with the registered baseline).
    mw_s2_committed_before_wait_one,
    // E9-CORRECTIVE: worker paused at ParkCandidate boundary (pre-physical-wait).
    scheduler_park_candidate,
    // E9-CORRECTIVE: worker paused at park commit boundary (pre-wake_cv.wait).
    scheduler_park_commit,
    // Issue #115 deterministic reproducer: worker paused AFTER the wake-epoch
    // baseline is recorded (the G1 arm handshake under global_mtx_ + wake_mtx_)
    // and BEFORE it takes wake_mtx_ to enter cv.wait — with NO locks held.
    // This is the exact mirror of scheduler_park_commit: a publication issued
    // while this seam holds lands strictly AFTER the baseline, so it can only
    // be observed through the cv predicate (wake_epoch_ != observed_epoch),
    // never absorbed into the baseline. Post-fix runnable publication advances
    // the epoch and the predicate fires at wait entry; pre-fix nothing does
    // and the worker sleeps into the #115 strand. Included in
    // release_all_phases (unlike worker_park_returned): a paused worker here
    // holds no locks and its park has not begun, so a terminating sibling's
    // release lets it proceed to a predicate that observes global_terminate_.
    scheduler_park_baseline_recorded,
    // Phase G review P2b (G1 deterministic reproducer): worker paused
    // immediately AFTER park_on_wake_source returns in worker_loop — it has
    // woken from a wake-domain park and is about to re-enter the loop top
    // (pop own inbox -> try_steal -> global_mtx_ drain -> classify). A test
    // holding a worker here can publish backend readiness / routes /
    // terminate-flag changes deterministically BEFORE the worker re-checks
    // them. EXCLUDED from release_all_phases by design (see
    // async_test_control.cpp): a terminating sibling's release must not
    // destroy the reproducer's hold — the G1 stall is exactly a run where a
    // terminated worker leaves a survivor mid-recheck. A test that arms this
    // seam MUST release it (its own watchdog is the escape hatch).
    worker_park_returned,
    // E12-A: setter paused after SET store, holding global_mtx_, before drain.
    event_set_store_before_drain,
    // E12-A-EVENT-CORRECTIVE-2 (T31): admission attempt marker, BEFORE taking
    // global_mtx_. Marks that wait() was called but has not entered its CS.
    event_admission_attempt_before_global_lock,
    // E12-A: admission paused after registration, holding global_mtx_+q.mtx(),
    // before the final SET check.
    event_admission_before_final_check,
    // E12-C: MUTEX-HANDOFF-ONE paused AFTER owner commit (owner_ == winner
    // Fiber), BEFORE make_runnable / route_runnable_locked publication. Holding
    // global_mtx_ (+ waiters_.mtx() inside). Proves owner-before-publication.
    mutex_handoff_before_publication,
    // E12-D: CONDITION-WAIT-PREPARE paused AFTER the Condition node is Registered
    // + linked in the Condition queue, holding global_mtx_, BEFORE the bound
    // Mutex is released/handed off. A test observing this phase proves the
    // register-before-release ordering (InvNoLostNotifyWindow / NEG-C8) and that
    // a concurrent notify sees the registered node while the Mutex is still
    // owned. The Condition queue mtx has been released; only global_mtx_ held.
    condition_register_before_handoff,
    // E12-D: notify_all paused AFTER acquiring global_mtx_ authority, BEFORE the
    // drain loop begins. A test observing this phase proves late registration /
    // cancel / expiry serialize AFTER the snapshot (they need global_mtx_).
    condition_notify_before_drain,
    // E12-D-CLOSURE: mutex_lock queuing path. Fires AFTER register_wait_locked
    // succeeds and the fiber WILL suspend (no immediate ownership). Proves this
    // fiber's WaitNode is registered in the Mutex waiter queue (T15a/T15b).
    mutex_waiter_registered_before_grant,

    // E13 P3: Select timer pump paused AFTER the ACTIVE check, BEFORE the
    // arm dereference / fail-fast. A due ACTIVE Select entry is unreachable in
    // valid P3 (no admission); observing this phase proves the pump reached
    // an ACTIVE Select entry and is about to fail fast (stage-boundary guard,
    // NOT supported production behavior).
    select_timer_pump_active,
    // E13 P3: Select timer pump observing a stale (non-ACTIVE) entry being
    // skipped — the deterministic proof of the I4 closure: the pump observed
    //    RETIRED/CONSUMED and did NOT read arm_ (arm-load delta == 0).
    select_timer_pump_skip,

    // E13 P4: Timer loser arm classified Retired, with the registration STILL
    // ACTIVE, immediately BEFORE the ACTIVE->RETIRED retire CAS. The load-
    // bearing SN-9 ordering: a test observing this phase proves arm.state
    // became Retired WHILE the registration was still ACTIVE, then the retire
    // CAS (below this seam) flipped it to RETIRED. No wall-clock timing.
    select_timer_loser_arm_classified,

    // E13 P5: admission armed — AFTER every arm is registered AND group phase
    // becomes Selecting, BEFORE the readiness snapshot. A test observing this
    // phase proves all arms were registered before the snapshot was taken (no
    // early-registration shortcut) and that no winner/result/runnable exists
    // yet. The seam blocks the admission worker under global_mtx_, so a
    // coordinator thread can inspect the registered group deterministically.
    select_admission_armed,
    // E13 P5: admission claimed — AFTER the winner CAS succeeds (fresh claim),
    // BEFORE winner/loser finalization. Fires only on a real claim attempt that
    // won (not on claim-lost). A test observing this phase proves the snapshot's
    // chosen candidate index was committed to the group before any arm was
    // finalized. Reached inside select_process_group_locked; does NOT change P4
    // production semantics (it is a pure observation with no blocking unless
    // armed by a registered controller).
    select_admission_claimed,
    // E13 P5: admission consumed — AFTER inline phase becomes Completed,
    // BEFORE phase becomes Consumed. A test observing this phase proves the
    // inline result is committed, every authority is closed, completion_mode is
    // Inline, and runnable delta is 0 — the inline lifecycle ordering.
    select_admission_consumed,

    // E13 P6: caller Fiber paused AFTER the no-ready suspension commit
    // (make_waiting + phase Armed + waiting_select_count_++), AFTER global_mtx_
    // is released, BEFORE the physical context_switch. A coordinator thread can
    // deterministically resolve the group (Event::set / clock advance) and
    // prove the wake-before-physical-switch window is closed (the caller is
    // queued exactly once even if it has not yet switched away). The seam runs
    // OUTSIDE global_mtx_ (the caller has released it).
    select_suspend_before_switch,
    // E13 P6: select_publish_locked entry, BEFORE any result mutation. A test
    // observing this phase proves publication-entry preconditions hold (winner
    // exists, all authority closed, result not yet written, completion_mode
    // none, phase Selecting/Armed). Holds global_mtx_ (use a snapshot).
    select_publish_entry,
    // E13 P6: AFTER phase becomes Completed and, for suspended mode, AFTER
    // successful make_runnable + route_runnable_locked. A test observing this
    // phase proves the suspended publication committed result + runnable once
    // (or inline published result once with no runnable). Holds global_mtx_.
    select_publish_done,
    // E13 P6: AFTER the resumed caller reacquires global_mtx_ and validates the
    // result, BEFORE phase becomes Consumed. A test observing this phase proves
    // phase==Completed, mode==Suspended, result present, winner stable, all
    // authority closed, caller Running, runnable publication count==1.
    // Holds global_mtx_.
    select_suspended_before_consume,

    // E13 P7: fired inside select_admit's catch block, AFTER
    // select_rollback_registration_locked reached Aborted, BEFORE the original
    // exception is rethrown. A test observing this phase proves the rollback
    // completed (phase==Aborted) while global_mtx_ is still held. Reached only
    // via the synthetic failure-injection seam; absent in production.
    select_rollback_aborted,

    // I47-F1: generic suspension phase seam. Fired AFTER wait registration is
    // committed + suspend authority raised + Fiber state Waiting + global_mtx_
    // released, BEFORE the physical Fiber->Scheduler context_switch. Available
    // on ALL suspension paths (Completion, ready flag, WaitQueue, deadline).
    // A coordinator thread can resolve the wait (Event::set / wake) and prove
    // the suspend-before-switch authority window is closed: a thief cannot
    // steal the routed ticket while suspend_switch_pending is active.
    scheduler_suspend_before_physical_switch,

    // Issue #161 (idle-dance contribution orphaning): the worker popped a
    // runnable ticket (it is REMOVED from its inbox — invisible to steals and
    // to classify) but has NOT yet executed the unlocked
    // idle_workers_.store(0) erase nor entered run_next_on (running not yet
    // incremented). Holding a worker here realizes the exact M4 window: an
    // idle-dance contribution a peer makes now is orphaned by this worker's
    // erase on release, and this worker's own later not-last dance signal can
    // then be absorbed by the peer's late-armed park baseline. No locks are
    // held at this point. Fired through test_phase_worker (per-worker arming:
    // the reproducer must let the peer's pass through the same call site).
    worker_ticket_popped,

    // Issue #161 B4-reclassification round: the popped-ticket erase has
    // EXECUTED (the exchange(0) — and its conditional generation bump — has
    // landed) but run_next_on has not (running not yet incremented). The
    // difference from worker_ticket_popped is load-bearing for the
    // route-publication orphan reproducer: a peer contribution made while a
    // worker is held HERE survives this worker's pop-erase untouched, so the
    // next count write is the route-publication erase in
    // route_runnable_locked — the third genuine invalidation site (the
    // pub-site M4 variant). No locks are held at this point. Fired through
    // test_phase_worker (per-worker arming).
    worker_ticket_erase_done,

    count
};

// Issue #161 per-worker arming sentinel: a PhaseState whose armed_worker is
// kAnyWorker blocks EVERY worker reaching its call site (the pre-existing
// global-seam semantics). A concrete worker id pins the hold to that one
// worker's pass; other workers mark the phase reached and pass through.
// Two workers traverse the same worker_loop seams, so a role-pinned
// reproducer (the eraser held at the ticket seam while the dancer parks, or
// conversely) cannot be expressed with global arming alone.
inline constexpr unsigned kAnyWorker = static_cast<unsigned>(-1);

// ---- #196 E9 trace-conformance recorder vocabulary ----
//
// Minimal ARCHITECTURE-LEVEL semantic events for the E9 park/wake protocol
// ONLY (issue #196 pilot; NOT a general tracing facility). Every event is
// emitted from a guarded call site inside the production park/wake paths and
// recorded into a fixed-size ring on the per-Scheduler controller. Production
// builds compile none of the call sites (SLUICE_ASYNC_INTERNAL_TESTING off).
enum class TraceEventKind : unsigned char {
    park_committed = 1,  // wake-epoch baseline recorded (park admission commit)
    park_entered = 2,    // physical cv wait about to begin (predicate false)
    park_refused = 3,    // G1 refuse: unguarded progress / dance count -> no park
    wake_published = 4,  // signal_wake_locked advanced the wake epoch
    park_returned = 5,   // the cv wait returned (cause bits below)
};

// The producer-side attribution of a wake publication. Set by the guarded
// marker at each semantic call site immediately BEFORE signal_wake_locked on
// the SAME thread; consumed by the next wake_published record on that thread.
enum class WakeCause : unsigned char {
    none = 0,             // unattributed (validator fail-closes on it)
    external_notify = 1,  // SchedulerWakeHandle::notify -> notify_external_wake
    runnable_route = 2,   // spawn / spawn_on / route_runnable_locked
    park_refuse = 3,      // the G1 park-commit refusal signal
    terminate = 4,        // global_terminate_ publication signals
    retire_epilogue = 5,  // the worker-loop retire epilogue departure signal
    idle_dance = 6,       // the not-last idle-dance convergence signal
};

// ParkReturned cause bits: which cv-predicate terms (or the timeout) held at
// the wait's return, evaluated under wake_mtx_ (+inbox_mtx for runnable).
inline constexpr std::uint16_t kReturnCauseEpoch = 0x1;
inline constexpr std::uint16_t kReturnCauseTerminate = 0x2;
inline constexpr std::uint16_t kReturnCauseRunnable = 0x4;
inline constexpr std::uint16_t kReturnCauseTimeout = 0x8;

// Sentinel worker id for events with no worker attribution.
inline constexpr unsigned char kTraceNoWorker = 0xFF;

inline constexpr std::size_t kTraceCapacity = 64;

struct TraceEvent {
    unsigned char kind{0};        // TraceEventKind
    unsigned char worker{kTraceNoWorker};
    unsigned char cause{0};       // WakeCause (wake_published only)
    unsigned char immediate{0};   // park_returned: predicate true at wait entry
    std::uint16_t return_causes{0};  // park_returned: kReturnCause* bits
    std::uint16_t armed{0};       // park_committed: entry-armed observation
    std::uint64_t epoch{0};       // wake epoch at the event
};

// Per-PhaseTag controller state. Owned by the controller registry, keyed on
// Scheduler*. All fields are guarded by `mtx` (the phase's own coordination
// mutex — distinct from any production lock).
struct PhaseState {
    std::mutex mtx;
    std::condition_variable cv;
    bool armed = false;
    // Issue #161: kAnyWorker (default) keeps the global block-any-worker
    // semantics; a concrete id blocks only that worker (see kAnyWorker).
    unsigned armed_worker = kAnyWorker;
    // The worker currently holding the pause (the pauser owns `paused`:
    // only it sets and clears the flag, so a non-pinned peer's pass through
    // the same call site cannot overwrite an active hold — see
    // async_test_control.cpp test_phase_worker). kAnyWorker = no pauser.
    unsigned pauser = kAnyWorker;
    bool reached = false;  // the phase call site was reached (set under mtx)
    bool paused = false;   // the phase is blocked waiting for release
};

// ---- E13 P5 CORRECTIVE: admission boundary snapshot ----
// Captured by the admission worker under global_mtx_ immediately before each
// seam, then read by the coordinator thread under the controller's own mutex
// (no global_mtx_ acquisition). Only the two expected PhaseTag values are
// valid: select_admission_armed and select_admission_consumed.
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
//
// kRollbackFailAfterDisabled is declared here (forward) so the P7 observation
// fields below can value-initialize it; the full definition with semantics is
// further down in the §18 fault-injection section.
inline constexpr std::size_t kRollbackFailAfterDisabled =
    static_cast<std::size_t>(-1);

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

    // E13 P7 rollback observability (task §19). Populated by the rollback path
    // (select_admit catch + the per-arm/orchestrator helpers) under global_mtx_,
    // read by the test under the controller's own mutex. Fixed-size: the
    // rollback observation is reset per failing select() call. All fields are
    // diagnostic-only; none determines production policy.
    std::size_t rollback_configured_fail_after{kRollbackFailAfterDisabled};
    std::size_t rollback_successful_registrations{0};
    std::size_t rollback_begin_count{0};
    std::size_t rollback_finish_count{0};
    std::size_t rollback_arm_order_len{0};
    std::array<std::uint32_t, sluice::async::kSelectMaxArms>
        rollback_arm_order_indices{};
    std::array<std::uint8_t, sluice::async::kSelectMaxArms>
        rollback_arm_order_kinds{};  // ArmKind as raw (0=event,1=timer)
    std::array<bool, sluice::async::kSelectMaxArms>
        rollback_event_linked_before{};

    // ---- R2-ALLOC: ordinary timed-admission allocation-failure injection ----
    // One-shot arm: the NEXT prepare_ordinary_deadline_locked call on this
    // Scheduler throws std::bad_alloc at prepare entry (before either
    // container is touched), then the flag auto-clears so the very next
    // admission on the same Scheduler runs the healthy path — the regression
    // shape is "fail once, then prove zero residue by reusing everything".
    // Guarded by its own leaf controller mutex (never held while acquiring
    // any production lock; the guarded hook in prepare runs under
    // global_mtx_, the same discipline as the phase states).
    std::mutex ordinary_deadline_alloc_fail_mtx;
    bool ordinary_deadline_alloc_fail_armed{false};

    // ---- FE-CORRECTIVE-1 P1-1: deferred-publication storage-failure ----
    // injection. One-shot arm: the NEXT defer_publication_locked call on
    // this Scheduler throws std::bad_alloc at the transit-insertion edge
    // (after the terminal winner is committed), then the flag auto-clears.
    // The witness proves the named process-terminal fail-fast fires so the
    // delivery obligation can never be silently lost. Same leaf-mutex
    // discipline as the R2 flag (the guarded hook runs under global_mtx_).
    std::mutex deferred_publication_alloc_fail_mtx;
    bool deferred_publication_alloc_fail_armed{false};

    // ---- #196 E9 trace-conformance recorder ----
    // Fixed-size ring (no allocation on the record path). Guarded by
    // trace_mtx (the controller's own leaf mutex — never taken by, and never
    // holding, any production lock). trace_enabled defines the capture
    // window: the recording helpers are no-ops while disabled, so a test
    // records exactly the deterministic segment it controls.
    std::mutex trace_mtx;
    bool trace_enabled{false};
    bool trace_overflow{false};
    std::size_t trace_len{0};
    std::array<TraceEvent, kTraceCapacity> trace_events{};
    // One-shot pending wake-cause slot: the guarded producer-site marker sets
    // it immediately BEFORE signal_wake_locked; the next wake_published record
    // consumes it (resets to none). The marker and the signal are adjacent in
    // one function on one thread; a foreign interleaved signal in between
    // would surface as an unattributed (none) wake, which the validator and
    // the corpus tests fail closed on — never silently mis-binned.
    unsigned char pending_wake_cause{0};
    unsigned char pending_wake_worker{kTraceNoWorker};

    // ---- DST-PV-1 schedule script (test-only next-runnable choice) ----
    //
    // A deterministic schedule driver installs a bounded decision vector
    // BEFORE sched.run(); the worker_loop pop site consults it through
    // schedule_script_active/schedule_script_pick (guarded call sites).
    //
    // Vocabulary: Run(fiber_id) chooses WHICH already-runnable Fiber the
    // worker dequeues next; Invoke(action_id) executes a driver-registered
    // closure (the existing controllable seams: fake/scripted I/O completion,
    // advance_clock, cancel_wait) INLINE at the decision point. Script
    // exhaustion returns control to the normal FIFO pop — a free run.
    //
    // Capacities are fixed at install time; the pick path allocates nothing
    // (Invoke actions run through stable references into the fixed action
    // array — no std::function copy). Re-install / re-arm inside Invoke is
    // FAIL-CLOSED; uninstall inside Invoke remains supported, and the
    // post-action epilogue then records into the old state without
    // reactivating (activation is the TLS pair alone). `mtx` is a leaf
    // controller mutex (precedent: trace_mtx); it guards ONLY the script
    // records (step index, executed ring): it is never held while an action
    // runs and never held while acquiring any production lock.
    // Install must happen before run() starts, ON THE SAME THREAD (the PV
    // model is single-threaded inline run(1): driver and worker are one
    // thread). The worker pop gate is a thread-local activation — no lock,
    // no atomic — so a worker on any other thread (every multi-worker run)
    // never sees the script and stays on the FIFO pop: multi-worker
    // scheduling space is structurally untouched by the seam.
    static constexpr std::size_t kScheduleMaxSteps = 64;
    static constexpr std::size_t kScheduleMaxFibers = 8;
    static constexpr std::size_t kScheduleMaxActions = 8;
    static constexpr std::size_t kScheduleMaxExecuted = 64;

    struct ScheduleScriptStep {
        enum class Kind : std::uint8_t { run, invoke };
        Kind kind{Kind::run};
        unsigned char payload{0};  // run: fiber id; invoke: action id
    };
    struct ScheduleScriptState {
        std::mutex mtx;
        bool enabled{false};
        std::array<ScheduleScriptStep, kScheduleMaxSteps> steps{};
        std::size_t step_count{0};
        std::size_t next_step{0};

        // Test-assigned participant identity (small integers), bound before
        // run(). Replay syntax uses ONLY these ids — never Fiber addresses.
        std::array<sluice::async::Fiber*, kScheduleMaxFibers> fiber_ptrs{};
        std::size_t fiber_count{0};

        // Driver-registered action closures (invoke steps). Installed before
        // run(); invoked with no script-mutex and no production lock held.
        // Fixed-size storage inside the controller (retained until process
        // teardown): the pick invokes actions through STABLE REFERENCES
        // (no copy, no allocation); re-install / re-arm inside Invoke is
        // fail-closed, and uninstall never rewrites this array.
        std::array<std::function<void(sluice::async::Scheduler&)>,
                   kScheduleMaxActions>
            actions{};
        std::array<const char*, kScheduleMaxActions> action_labels{};
        std::size_t action_count{0};

        // Executed-step ring for the failure diagnostic (kind + payload).
        std::array<ScheduleScriptStep, kScheduleMaxExecuted> executed{};
        std::size_t executed_len{0};

        // Diagnostic context captured at install (single death-time reader;
        // the PV model installs before run and never mutates during run).
        const char* test_name{nullptr};
        const std::string* replay_vector{nullptr};

        // Failure record (filled once, immediately before the loud abort).
        bool failed{false};
        std::size_t failed_step{0};
        unsigned char requested_id{0};
    };
    ScheduleScriptState schedule_script{};
};

// --- Called from scheduler.cpp (under SLUICE_ASYNC_INTERNAL_TESTING) ---
// Marks `tag` reached for `s`. If the phase is armed, blocks the caller until
// the phase is released by the test. No allocation; the controller for `s` must
// already be registered (register_controller). If unregistered, this is a no-op
// (the phase was reached but no test is observing — safe for production paths
// that happen to be compiled into the variant without a test driver).
void test_phase(sluice::async::Scheduler& s, PhaseTag tag) noexcept;

// Issue #161 per-worker variant of test_phase: an armed phase whose
// armed_worker is a concrete id blocks ONLY that worker's pass; every other
// worker marks the phase reached and passes through. A globally armed phase
// (kAnyWorker) blocks all workers exactly like test_phase. Used by the
// worker_loop seams whose call sites are traversed by every worker (ticket
// pop, park candidate) where a reproducer must pin the two workers to
// different roles through the SAME site.
void test_phase_worker(sluice::async::Scheduler& s, PhaseTag tag,
                       unsigned worker_id) noexcept;

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
// select_publish_entry, select_publish_done, select_suspended_before_consume. No-op if
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

// Issue #161: arm a phase for ONE worker's pass only (see kAnyWorker). The
// wait_paused/is_paused/release observables are shared with global arming —
// with a pinned worker only that worker can be the pauser, so wait_paused is
// unambiguous. Plain arm() resets the phase to global (kAnyWorker) semantics.
void arm_worker(sluice::async::Scheduler& s, PhaseTag tag,
                unsigned worker_id) noexcept;

// ---- E13 P7: synthetic registration-failure injection (task §18) ----
//
// The production library MUST NOT expose SelectRegistrationFailure, failure
// counters, or any injection symbol (task §32). The seam is therefore owned by
// the non-installed controller: select_admit (compiled in the variant) queries
// the controller's configured fail_after on each successful registration under
// global_mtx_, and throws a non-installed SelectRegistrationFailure when the
// boundary is reached. The exception type and the controller fields live ONLY
// under SLUICE_ASYNC_INTERNAL_TESTING and are absent from production symbols.
//
// kRollbackFailAfterDisabled means "no injection": the configured value is the
// whole-machine "fail immediately AFTER this many successful registrations".
// 0 = fail before the first registration; N = arm_count = fail immediately
// before FinishRegistration (all arms fully registered). This is load-bearing
// (P7-T5): a fully-registered but still-Building group must roll back.
// (Declared above SchedulerController so the observation fields can use it.)

// E13 P7 (task §18): the non-installed synthetic registration-failure
// exception. select_admit (compiled ONLY in the variant) throws this when the
// configured fail_after boundary is reached, under global_mtx_. It is caught
// ONLY by select_admit's own catch, which runs rollback and rethrows it to the
// test. This type is absent from production symbols (the production select_admit
// has no injection branch, and this header is never seen by a production TU).
//
// The integer payload is an identifying code the test compares for P7-T6
// (original-exception-preservation). A distinct sentinel avoids any collision
// with a natural std::exception subtype.
class SelectRegistrationFailure : public std::runtime_error {
public:
    static constexpr int kIdentifyingCode = 0xE137;
    SelectRegistrationFailure()
        : std::runtime_error("sluice::async synthetic Select registration "
                             "failure (test seam)") {}
    int code() const noexcept { return kIdentifyingCode; }
};

// Configure the synthetic failure boundary for the NEXT select() admission on
// `s`. Records the configured N and resets the per-call rollback observation.
// The value persists until reset_rollback_injection is called (so repeated
// failing calls on the same Scheduler all inject at N). No-op if no controller.
void configure_rollback_fail_after(sluice::async::Scheduler& s,
                                   std::size_t fail_after) noexcept;

// Clear the synthetic failure boundary (next select() does not inject). Also
// resets the per-call rollback observation. No-op if no controller.
void reset_rollback_injection(sluice::async::Scheduler& s) noexcept;

// Read the configured fail_after boundary (kRollbackFailAfterDisabled if none).
std::size_t rollback_fail_after(sluice::async::Scheduler& s) noexcept;

// ---- E13 P7: rollback observability (task §19) ----
//
// The rollback observation is a fixed-size snapshot captured by the rollback
// path under global_mtx_ and read by the test under the controller's own mutex.
// All fields are diagnostic-only evidence; none determines production policy.
// Indices are recorded in the actual reverse registration order processed.
struct RollbackObservation {
    std::size_t configured_fail_after{kRollbackFailAfterDisabled};
    std::size_t successful_registrations{0};
    std::size_t begin_count{0};
    std::size_t finish_count{0};
    std::size_t arm_order_len{0};
    std::array<std::uint32_t, sluice::async::kSelectMaxArms>
        arm_order_indices{};
    std::array<std::uint8_t, sluice::async::kSelectMaxArms>
        arm_order_kinds{};  // ArmKind raw: 0=event, 1=timer
    std::array<bool, sluice::async::kSelectMaxArms>
        event_linked_before{};
};

// Internal hook called by select_admit's registration loop (under global_mtx_)
// after exactly `successful` arm registrations. Returns true iff the configured
// boundary has been reached, so select_admit throws SelectRegistrationFailure.
// The throw itself happens in select_admit (the controller never throws).
bool rollback_should_inject_after(sluice::async::Scheduler& s,
                                  std::size_t successful) noexcept;

// ---- R2-ALLOC: ordinary timed-admission allocation-failure injection ----
//
// The production library MUST NOT expose any injection symbol (same rule as
// the P7 select seam). prepare_ordinary_deadline_locked (compiled in the
// variant) queries this one-shot flag at entry, under global_mtx_, and throws
// std::bad_alloc when armed — simulating the pool-node / heap-growth
// allocation failing BEFORE any admission state is mutated. The flag, the
// mutex, and these entry points live ONLY under SLUICE_ASYNC_INTERNAL_TESTING
// and are absent from production symbols.

// Arm/disarm the one-shot failure (test side). Arming twice without an
// intervening prepare call keeps it armed (still one shot). No-op without a
// registered controller.
void arm_ordinary_deadline_alloc_failure(
    sluice::async::Scheduler& s) noexcept;
void disarm_ordinary_deadline_alloc_failure(
    sluice::async::Scheduler& s) noexcept;

// Guarded production hook (called at prepare entry, under global_mtx_):
// returns true iff the one-shot flag was armed; consuming it clears the flag.
bool ordinary_deadline_alloc_should_fail(
    sluice::async::Scheduler& s) noexcept;

// ---- FE-CORRECTIVE-1 P1-1: deferred-publication storage-failure injection --
// Arm/disarm the one-shot failure (test side); the guarded production hook is
// consumed at the defer_publication_locked insertion edge (under global_mtx_,
// after the terminal winner). Same contract as the R2 pair above.
void arm_deferred_publication_alloc_failure(
    sluice::async::Scheduler& s) noexcept;
void disarm_deferred_publication_alloc_failure(
    sluice::async::Scheduler& s) noexcept;
bool deferred_publication_alloc_should_fail(
    sluice::async::Scheduler& s) noexcept;

// ---- #196 E9 trace-conformance recorder (controller-owned; see the
// vocabulary block above SchedulerController). The recording entry points are
// called from guarded production park/wake call sites; the enable/disable/
// read entry points are called by the test coordinator. All are no-ops
// without a registered controller; recording is additionally a no-op while
// the window is disabled. No allocation on any record path.

// Test coordinator: open/close the capture window and reset the ring.
void trace_enable(sluice::async::Scheduler& s) noexcept;
void trace_disable(sluice::async::Scheduler& s) noexcept;
void trace_clear(sluice::async::Scheduler& s) noexcept;
bool trace_overflow(sluice::async::Scheduler& s) noexcept;

// Test coordinator: snapshot-copy the recorded events (allocation happens on
// the TEST side only).
std::vector<TraceEvent> trace_events(sluice::async::Scheduler& s) noexcept;

// Guarded production call sites: record one semantic event (drop, not crash,
// past capacity — the overflow flag fail-closes the test's shape assertion).
void record_trace_event(sluice::async::Scheduler& s,
                        const TraceEvent& ev) noexcept;

// Guarded producer-site marker: attribute the NEXT wake_published event.
void set_trace_wake_cause(sluice::async::Scheduler& s, WakeCause cause,
                          unsigned worker) noexcept;

// Guarded signal_wake_locked site: record wake_published (consuming the
// pending cause marker).
void record_trace_wake(sluice::async::Scheduler& s,
                       std::uint64_t new_epoch) noexcept;

// Internal hooks called by the production rollback path (under global_mtx_) to
// record the observation. All are no-ops without a registered controller.
void rollback_record_begin(sluice::async::Scheduler& s,
                           std::size_t successful_registrations) noexcept;
void rollback_record_arm(sluice::async::Scheduler& s,
                         std::uint32_t arm_index,
                         std::uint8_t arm_kind_raw,
                         bool event_linked_before) noexcept;
void rollback_record_finish(sluice::async::Scheduler& s) noexcept;

// Read the current rollback observation. Returns a default-constructed
// observation if no controller is registered for `s`.
RollbackObservation read_rollback_observation(
    sluice::async::Scheduler& s) noexcept;

// ---- DST-PV-1 schedule script (test-only next-runnable choice) ----
//
// Worker-side hooks (guarded call sites in scheduler.cpp worker_loop): the
// worker pop consults schedule_script_active FIRST; it is a no-lock fast gate
// — one thread-local activation pair + a Scheduler identity compare, with NO
// registry mutex, NO script mutex, and NO atomic on the inactive path (the
// registered-controller registry is NOT consulted per pop). A script is
// visible ONLY on its installing thread, so every other worker in every
// multi-worker run pays exactly one TLS load + compare and returns false —
// those runs stay on the FIFO pop, unperturbed.
bool schedule_script_active(sluice::async::Scheduler& s) noexcept;

// Consume script steps at the worker pop decision. Run(id) removes the bound
// Fiber from ws->local_runnable (under inbox_mtx) and returns it; Invoke(id)
// takes a STABLE REFERENCE to its action under the script mutex — no
// std::function copy, the pick path allocates nothing after install — and
// executes it with NO script mutex and NO production lock held. Actions may
// re-enter SUPPORTED NON-REPLACING test-control surfaces
// (uninstall_schedule_script, advance_clock, cancel_wait, fake/scripted I/O
// completion); installing or re-arming a schedule script inside Invoke is
// FAIL-CLOSED (install_schedule_script aborts). If the action uninstalled
// the script mid-run, the epilogue still records the step into the OLD
// controller state and never reactivates (activation is the TLS pair alone,
// which uninstall cleared). The step is then recorded and the hook visit
// TERMINATES (returns nullptr) — actions may stage
// effects that become runnable only after the worker's next drain, and the
// intervening normal pop -> drain -> FIFO path publishes them; a singleton
// publication needs no script step (Run(X) is a CHOICE among >= 2 legal
// runnables). Returns nullptr when the script is exhausted (free run). An
// illegal decision (requested Fiber not in ws->local_runnable) prints the
// deterministic diagnostic package (step index, requested id, legal runnable
// set, executed prefix, replay vector, logical clock) to stderr and aborts —
// it NEVER silently falls back to another runnable.
sluice::async::Fiber* schedule_script_pick(sluice::async::Scheduler& s,
                                           sluice::async::WorkerState* ws) noexcept;

// Test-side install/uninstall (BEFORE run(); the PV model is single-threaded
// inline run(1) — install and the worker loop must run on the SAME thread; a
// multi-worker or cross-thread run never activates the script and a test that
// scripts such a run fails its own deterministic assertions).
// `install_schedule_script` fills the controller state owned by `s`'s
// registered controller (register_controller must already have run), and
// FAILS LOUDLY if called from inside an Invoke action (re-install / re-arm
// is forbidden while an action runs — the old script's epilogue would
// corrupt a replaced replay vector).
struct ScheduleScriptInstall {
    std::array<SchedulerController::ScheduleScriptStep,
               SchedulerController::kScheduleMaxSteps>
        steps{};
    std::size_t step_count{0};
    std::array<sluice::async::Fiber*, SchedulerController::kScheduleMaxFibers>
        fiber_ptrs{};
    std::size_t fiber_count{0};
    std::array<std::function<void(sluice::async::Scheduler&)>,
               SchedulerController::kScheduleMaxActions>
        actions{};
    std::array<const char*, SchedulerController::kScheduleMaxActions>
        action_labels{};
    std::size_t action_count{0};
    const char* test_name{nullptr};
    const std::string* replay_vector{nullptr};
};

void install_schedule_script(sluice::async::Scheduler& s,
                             const ScheduleScriptInstall& plan) noexcept;
void uninstall_schedule_script(sluice::async::Scheduler& s) noexcept;

}  // namespace sluice_async_test
