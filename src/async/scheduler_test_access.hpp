// scheduler_test_access.hpp - NON-INSTALLED internal-testing seam header.
//
// Defines Scheduler::AsyncTestAccess OUT-OF-LINE (C4 / issue #135: the
// internal-testing control plane must not shape the installed production
// header). The installed <sluice/async/scheduler.hpp> keeps ONLY the nested
// forward declaration `struct AsyncTestAccess;` plus the layout-bearing test
// members, and includes this header at the bottom under
// #if defined(SLUICE_ASYNC_INTERNAL_TESTING) - so every internal-testing TU
// that includes scheduler.hpp sees the complete nested definition, while
// production TUs (macro undefined) compile neither the include nor any seam.
//
// The consumers are the non-installed test-support controller
// (tests/async_test_control.hpp) and internal-testing test binaries. This
// header is on the include path ONLY of the sluice_async_internal_testing
// target (its public includedirs contain src/async); the production
// sluice_async target cannot see it.
//
// Being an out-of-line definition of a NESTED class, AsyncTestAccess retains
// the enclosing class's access rights (it may name Scheduler private state);
// nothing here is reachable from, or linked into, production builds.
#pragma once

#include <sluice/async/scheduler.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "queue_detail.hpp"  // QueueWaitCtx (FE-3 queue deferred seams; non-installed)

namespace sluice::async {
// Defined in async_rwlock.hpp / the non-installed scheduler_internal.hpp;
// the rwlock deferred-seam signatures only need the names (all bodies are
// out-of-line in scheduler_fe2_test_seam.cpp — a complete-type include here
// would be circular: async_rwlock.hpp includes scheduler.hpp, whose guarded
// bottom include pulls this header).
class AsyncRwLock;
struct RwWaitCtx;
}

namespace sluice::async {

// Internal-testing access surface. Reached only via the non-installed
// test-support controller; not part of the public API.
struct Scheduler::AsyncTestAccess {
    // Post-freeze R1 (PR #114 review): TLS identity probe. Returns the
    // raw g_worker slot value as read by Scheduler::current_worker() —
    // a definition compiled in src/async/scheduler.cpp — so a test can
    // prove the split's `inline thread_local` entity is ONE per-thread
    // object shared across the scheduler implementation TUs. Pointer
    // VALUE only; callers must not dereference it.
    static WorkerState* tls_worker_probe() noexcept { return current_worker(); }
    // Phase G park-window forensics (G1 BLOCKED instrumentation): dump
    // the park ledger + live scheduler state for a stalled run. The
    // watchdog thread of a forensics test calls this on its bounded
    // timeout, so a permanent stall states the exact invariant violation
    // instead of hanging or aborting.
    static void dump_park_forensics(Scheduler& s, const char* tag) {
        s.dump_park_forensics_for_test(tag);
    }
    static std::size_t park_ledger_count(const Scheduler& s) {
        std::lock_guard<std::mutex> lk(s.park_ledger_mtx_);
        return s.park_ledger_count_;
    }
    // Arm/disarm the park-commit ledger. Off by default: the snapshot
    // locks shift park-path timing (they made a seam-driven regression
    // flaky when always-on); only the forensics case arms it.
    static void set_park_forensics(Scheduler& s, bool enabled) noexcept {
        s.park_forensics_enabled_.store(enabled, std::memory_order_release);
    }
    // Backend wait-source token (observe-only; no access_mtx_): the G1
    // deterministic reproducer observes the backend READY publication
    // (progress_generation advance) before releasing a held worker, so
    // the worker's re-drain deterministically reaps it.
    static BackendWaitToken backend_wait_token(const Scheduler& s) noexcept {
        return s.ctx_.backend_wait_token_for_test();
    }

    // Phase G review P2b (G1 deterministic reproducer): causal state
    // observations on live workers. `worker_loop_exited` is the
    // worker-is-dead point (worker_loop returned; the thread will never
    // touch this WorkerState again). `worker_local_runnable` reads the
    // owner-local runnable depth under inbox_mtx (a stranded continuation
    // on a DEAD worker's queue is the G1 violation being observed).
    // `worker_park_domain` / `worker_last_classify` expose the per-worker
    // diagnostic fields for assertions. All four take global_mtx_ for the
    // workers_ read (the vector is mutated only under it; the same
    // global_mtx_ -> inbox_mtx order try_steal uses — no inversion).
    // worker_id beyond the retained topology returns false / 0 / None.
    static bool worker_loop_exited(const Scheduler& s,
                                   unsigned worker_id) noexcept {
        LockGuard lk(s.global_mtx_);
        return worker_id < s.workers_.size() &&
               s.workers_[worker_id]->loop_exited.load(
                   std::memory_order_acquire);
    }
    static std::size_t worker_local_runnable(const Scheduler& s,
                                             unsigned worker_id) {
        LockGuard lk(s.global_mtx_);
        if (worker_id >= s.workers_.size()) return 0;
        std::lock_guard<std::mutex> ilk(s.workers_[worker_id]->inbox_mtx);
        return s.workers_[worker_id]->local_runnable.size();
    }
    static WorkerState::ParkDomain worker_park_domain(
        const Scheduler& s, unsigned worker_id) noexcept {
        LockGuard lk(s.global_mtx_);
        if (worker_id >= s.workers_.size())
            return WorkerState::ParkDomain::None;
        return s.workers_[worker_id]->park_domain.load(
            std::memory_order_acquire);
    }
    // Issue #123: watchdog-safe park-domain read. A case-level watchdog
    // MUST never block behind the defect it is diagnosing — a stalled
    // worker may hold global_mtx_ at a causal seam (e.g. paused at
    // mw_admission_phase_b), so a blocking read would deadlock the
    // watchdog itself. This variant uses try_lock (non-blocking) and
    // reports lock contention via `available`. Diagnostic-only. TSA is
    // suppressed: the conditional try_lock/unlock pair cannot be
    // expressed with the annotated RAII guards, and runtime safety comes
    // from the try_lock/unlock pairing in the function body.
    static WorkerState::ParkDomain worker_park_domain_try(
        const Scheduler& s, unsigned worker_id,
        bool& available) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        if (!s.global_mtx_.try_lock()) {
            available = false;
            return WorkerState::ParkDomain::None;
        }
        available = true;
        struct Unlock {
            Mutex& mu;
            ~Unlock() noexcept { mu.unlock(); }
        } unlock{s.global_mtx_};
        if (worker_id >= s.workers_.size())
            return WorkerState::ParkDomain::None;
        return s.workers_[worker_id]->park_domain.load(
            std::memory_order_acquire);
    }
    static int worker_last_classify(const Scheduler& s,
                                    unsigned worker_id) noexcept {
        LockGuard lk(s.global_mtx_);
        return worker_id < s.workers_.size()
                   ? s.workers_[worker_id]->last_classify.load(
                         std::memory_order_acquire)
                   : -1;
    }

    // Phase F1 (issue #98): identity-routing diagnostics. The
    // Scheduler-owned ReadyRoutingSink counts deliveries/routes under the
    // internal-testing build (layout cost accepted, AGENTS.md §15); the
    // legacy-map probe proves a registration took the identity path (no
    // Completion*-keyed fallback entry).
    static std::size_t ready_sink_deliveries(const Scheduler& s) noexcept {
        return s.ready_sink_.deliveries();
    }
    static std::size_t ready_sink_routed(const Scheduler& s) noexcept {
        return s.ready_sink_.routed();
    }
    static std::size_t ready_sink_stale_dropped(const Scheduler& s) noexcept {
        return s.ready_sink_.stale_dropped();
    }
    static std::size_t ready_sink_cancel_lost(const Scheduler& s) noexcept {
        return s.ready_sink_.cancel_lost();
    }
    static std::size_t legacy_completion_wait_count(Scheduler& s) {
        LockGuard lk(s.global_mtx_);
        return s.waiting_size_.size() + s.waiting_void_.size();
    }
    static std::size_t wait_registry_live_count(Scheduler& s) {
        LockGuard rlk(s.wait_registry_mtx_);
        return s.wait_record_live_count_;
    }
    static detail::SynchronousReadySink& ready_sink(Scheduler& s) noexcept {
        return s.ready_sink_;
    }
    static std::uint64_t scheduler_identity(const Scheduler& s) noexcept {
        return s.scheduler_identity_;
    }

    // Phase F1 P1-2: bounded WaitRecord pool introspection (test-only).
    static std::size_t configured_wait_capacity(const Scheduler& s) {
        LockGuard rlk(s.wait_registry_mtx_);
        return s.wait_capacity_;
    }
    static std::size_t wait_record_storage_size(const Scheduler& s) {
        LockGuard rlk(s.wait_registry_mtx_);
        return s.wait_records_.size();
    }

    // Enable the deterministic logical clock (test mode).
    static void enable_test_clock(Scheduler& s) noexcept {
        s.test_clock_mode_.store(true, std::memory_order::release);
        s.clock_.store(0, std::memory_order::release);
    }
    static void set_clock(Scheduler& s, deadline_t t) noexcept {
        s.clock_.store(t, std::memory_order::release);
    }
    static deadline_t clock_now(const Scheduler& s) noexcept {
        return s.clock_.load(std::memory_order::acquire);
    }

    // Issue #50 deterministic topology judge. Tests call this while the
    // topology-mutation phase is paused: a true result proves mutation is
    // not running under the shared Scheduler coordination authority.
    static bool worker_topology_lock_available(Scheduler& s) noexcept {
        if (!s.global_mtx_.try_lock()) {
            return false;
        }
        s.global_mtx_.unlock();
        return true;
    }

    // Register a test deadline from a non-worker thread (the coordinator).
    // Mirrors await_wait_deadline admission MINUS the fiber-suspend path.
    // Acquires global_mtx_ internally (the caller does NOT hold it).
    static TimerRegistration* register_test_deadline(Scheduler& s,
                                                     WaitNode* node,
                                                     WaitQueue* q,
                                                     deadline_t deadline);

    // Timer-pool observation. These read GUARDED_BY fields from a test
    // coordinator thread for diagnostics; defined out-of-line with a TSA
    // suppression (the pool sizes are not load-bearing for correctness).
    static std::size_t timer_pool_size(const Scheduler& s) noexcept;
    static std::size_t deadline_heap_size(const Scheduler& s) noexcept;
    static std::size_t deadline_heap_capacity(const Scheduler& s) noexcept;
    // Synchronized snapshot (issue #229): the active-deadline accounting
    // counter is read under global_mtx_ because the coordinator may poll it
    // while live workers mutate deadlines; the sizes above are quiescent-only
    // diagnostics.
    static std::size_t active_deadline_count(const Scheduler& s) noexcept;
    static std::size_t timer_pool_count_in_state(const Scheduler& s,
                                                 TimerRegistration::State st) noexcept;
    static bool earliest_active_deadline(Scheduler& s, deadline_t& out);

    // ---- E13 Select registry test accessors ----
    // Link an Event Select arm into the Event's private SelectPort.
    // Acquires global_mtx_ internally. The arm must be Prepared/Detached
    // with kind==Event and group set.
    static void select_event_link(Scheduler& s, Event& event,
                                  detail::SelectArmSlot& arm) {
        LockGuard lk(s.global_mtx_);
        s.select_event_link_locked(event, arm);
    }

    // Unlink an Event Select arm from the Event's private SelectPort.
    // Acquires global_mtx_ internally.
    static void select_event_unlink(Scheduler& s, Event& event,
                                    detail::SelectArmSlot& arm) {
        LockGuard lk(s.global_mtx_);
        s.select_event_unlink_locked(event, arm);
    }

    // Phase-1 scan: walk the Event's SelectPort and mark eligible arms
    // CandidateReady. Acquires global_mtx_ internally. Returns marked count.
    static std::size_t select_event_scan(Scheduler& s, Event& event) {
        LockGuard lk(s.global_mtx_);
        return s.select_event_scan_locked(event);
    }

    // Set an arm's state under global_mtx_. Used by tests to prepare
    // specific arm states before a scan.
    static void set_arm_state(Scheduler& s, detail::SelectArmSlot& arm,
                              detail::ArmState st);

    // P4 EH corrective: forge a stale-but-equality-passing Event home_ for
    // the event-membership death test. PRE: `arm` is NOT linked in `event`'s
    // SelectPort intrusive list and its home_/next_/prev_ are null (e.g. it
    // was unlinked through select_event_unlink). After this call
    // arm.home_ == &event.select_port_ (so the preflight home_ equality
    // check passes), but the arm remains ABSENT from the intrusive list
    // (so the mechanical `found` scan fails and the preflight asserts).
    // This is the exact shape the EH case must reach: a home_ that looks
    // right but cannot be mechanically confirmed, proving the intrusive-
    // membership scan is load-bearing. Acquires global_mtx_ internally.
    static void select_event_forge_stale_home(Scheduler& s, Event& event,
                                              detail::SelectArmSlot& arm);

    // E13 P7 N5: forge an Event arm whose home_ points at the WRONG Event's
    // SelectPort (arm.event.event_ is event_a, home_ is event_b's port). The
    // per-arm rollback membership check (arm.home_ == &arm.event.event_->
    // select_port_) fails -> fail fast BEFORE any unlink mutation, proving
    // rollback cannot unlink another Event's registry. Acquires global_mtx_.
    static void select_event_forge_wrong_home(Scheduler& s, Event& event_a,
                                              Event& event_b,
                                              detail::SelectArmSlot& arm);

    // ---- E13 P3 Select timer test accessors ----
    // All route through Scheduler authority. No forgeable test hook; the
    // production target has none of these symbols.
    //
    // Synthetic Select timer registration from a non-worker thread (the
    // test coordinator). Builds a one-node temporary list and splices it
    // into select_timer_pool_ under G, pushing its tagged heap entry. The
    // block starts ACTIVE. Returns a stable pointer to the Scheduler-owned
    // block. `deadline` must be in the future (now < deadline) or the
    // entry is immediately due — tests keep clocks before deadlines or
    // transition to terminal before advancing.
    static detail::SelectTimerRegistration* register_synthetic_select_timer(
        Scheduler& s, detail::SelectArmSlot* arm, deadline_t deadline) {
        std::list<detail::SelectTimerRegistration> tmp;
        tmp.emplace_back(arm, &s, deadline);
        // Reserve heap capacity BEFORE mutation (the admission protocol's
        // only allocation-under-G). Synthetic single-entry registration.
        LockGuard lk(s.global_mtx_);
        s.deadline_heap_.reserve(s.deadline_heap_.size() + 1);
        return s.select_timer_splice_one_locked(tmp, tmp.begin());
    }

    // Transition a registered synthetic ACTIVE block via the Scheduler
    // accounting helpers (NOT the registration CAS directly — Addendum C).
    // Acquire global_mtx_ internally. Return the helper's CAS result.
    static bool retire_synthetic_select_timer(
        Scheduler& s, detail::SelectTimerRegistration& reg) {
        LockGuard lk(s.global_mtx_);
        return s.select_timer_retire_locked(reg);
    }
    static bool consume_synthetic_select_timer(
        Scheduler& s, detail::SelectTimerRegistration& reg) {
        LockGuard lk(s.global_mtx_);
        return s.select_timer_consume_locked(reg);
    }

    // Splice ONE caller-owned temporary node into select_timer_pool_ via the
    // REAL production helper (select_timer_splice_one_locked), returning the
    // stable Scheduler-owned address. For T2's pre/post-splice address-
    // identity proof only: a test captures &*it before the call, splices,
    // then asserts the returned pointer equals the captured address, the
    // temporary pool is empty, and the heap entry's Select target is that
    // same address. Mirrors the future admission protocol (caller-frame tmp
    // -> Scheduler pool) exactly. Acquires global_mtx_ internally and
    // reserves heap capacity before mutation.
    static detail::SelectTimerRegistration* splice_one_for_test(
        Scheduler& s,
        std::list<detail::SelectTimerRegistration>& tmp_pool,
        std::list<detail::SelectTimerRegistration>::iterator it) {
        LockGuard lk(s.global_mtx_);
        s.deadline_heap_.reserve(s.deadline_heap_.size() + 1);
        return s.select_timer_splice_one_locked(tmp_pool, it);
    }

    // Detached-object CAS authority for T1 (E13 P3 Corrective closure 3).
    // PRE: `reg` is NOT Scheduler-owned (never spliced into any pool) — it
    // is a stack-local SelectTimerRegistration exercising the registration's
    // own CAS state machine. The CAS methods are private; this guarded
    // entry is the only non-Scheduler way to reach them, and it exists
    // solely so T1 can test ACTIVE->{RETIRED,CONSUMED} + failed-CAS
    // transitions on detached locals without exposing the CASes in the
    // production target. Registered blocks MUST go through
    // retire_synthetic_select_timer / consume_synthetic_select_timer (the
    // Scheduler accounting helpers).
    static bool detached_try_claim_expiry(
        detail::SelectTimerRegistration& reg) noexcept {
        assert(reg.scheduler() == nullptr &&
               "detached CAS accessor requires a never-registered registration");
        if (reg.scheduler() != nullptr) {
            detail::select_invariant_fail_fast();
        }
        return reg.try_claim_expiry();
    }
    static bool detached_retire(
        detail::SelectTimerRegistration& reg) noexcept {
        assert(reg.scheduler() == nullptr &&
               "detached CAS accessor requires a never-registered registration");
        if (reg.scheduler() != nullptr) {
            detail::select_invariant_fail_fast();
        }
        return reg.retire();
    }

    // E13 P4 detached-group winner-CAS test entry. PRE: `group` is a
    // structural object that was never admitted/registered with any
    // Scheduler (scheduler_ == nullptr, arms_ == nullptr, arm_count_ == 0).
    // The winner CAS (SelectGroup::claim_winner_locked) is PRIVATE so a
    // registered group cannot bypass Scheduler::select_process_group_locked;
    // this guarded entry is the only non-Scheduler way to reach the CAS,
    // and it exists solely so P1 structural tests can prove first-claim-
    // wins / second-claim-loses on detached objects. The mechanical
    // detached precondition is ENFORCED here (not merely documented): a
    // group carrying arms or a scheduler binding is rejected before the CAS
    // (P4 §5.1: "Do not repeat the P3 mistake of documenting a test-only
    // precondition without enforcing it"). Defined out-of-line: the body
    // touches SelectGroup's complete definition (select_port.hpp), which is
    // NOT included by this installed header.
    static bool detached_claim_winner(detail::SelectGroup& group,
                                     std::uint32_t arm_index) noexcept;

    // ---- E13 P4 Select central claim + finalization test accessors ----
    // The P4 core is test-driven via direct seam calls (production-test-
    // plan.md §4 / §7.4). These acquire global_mtx_ internally and dispatch
    // to the Scheduler-locked core. No forgeable test authority; absent in
    // the production target.

    // Drive select_process_group_locked: validate + claim + finalize the
    // whole group for `candidate_index` without publication. Returns
    // whether THIS call won the claim. `group` must be a registered group
    // (scheduler_ == &s, arms_/arm_count_ set, arms linked/registered by
    // the test harness exactly as a future admission would).
    static bool select_process_group(Scheduler& s,
                                     detail::SelectGroup& group,
                                     std::uint32_t candidate_index);

    // Read the all-authority-closed invariant predicate (SN-10). False
    // before processing (no winner / open authority); true after a
    // successful process. A guarded test may also invoke it directly to
    // prove an open authority is rejected (OA death test).
    static bool select_all_authority_closed(const Scheduler& s,
                                            const detail::SelectGroup& group);

    // P4 OA corrective: invoke the all-authority-closed invariant as a
    // fail-fast assert (the mechanical precondition a future P6 publication
    // entry will gate on). Acquires global_mtx_ and asserts
    // select_all_authority_closed_locked(group). Used by the OA death case:
    // after a valid process (all authority closed) the test re-opens one
    // winner authority, then this assert must terminate the program,
    // proving the publication precondition is mechanically enforced — not
    // merely a bool predicate that could be ignored.
    static void assert_select_all_authority_closed(
        const Scheduler& s, const detail::SelectGroup& group);

    // Advance the test clock deterministically (drives the timer pump).
    static void advance_clock(Scheduler& s, deadline_t t);

    // Observation (diagnostics; defined out-of-line with a TSA suppression).
    static std::size_t select_timer_pool_size(
        const Scheduler& s) noexcept;
    static std::size_t select_timer_count_in_state(
        const Scheduler& s,
        detail::SelectTimerRegistration::State st) noexcept;
    // Tagged heap counts by kind: returns {ordinary_count, select_count}.
    static std::array<std::size_t, 2> tagged_heap_counts_by_kind(
        const Scheduler& s) noexcept;

    // Does any Select-kind heap entry target `target` (by address)? For
    // T2: proves the heap stores exactly the spliced block's address as
    // its stable Select pointer (the heap-by-stable-address contract).
    // Reads GUARDED_BY fields from a test coordinator for diagnostics;
    // not load-bearing for correctness.
    static bool deadline_heap_has_select_target(
        const Scheduler& s,
        const detail::SelectTimerRegistration* target) noexcept;

    // arm-load instrumentation (Addendum E): the count of times the
    // pump branch read reg.arm_ on an ACTIVE entry (the exact production
    // dereference site). Stale pops observe delta 0.
    static std::size_t select_timer_arm_load_count(
        const Scheduler& s) noexcept {
        return s.select_timer_arm_load_count_;
    }
    static void reset_select_timer_arm_load_count(Scheduler& s) noexcept {
        s.select_timer_arm_load_count_ = 0;
    }

    // ---- E13 P6 Select publication / suspended-resolution test accessors ----
    // Read the suspended-Select liveness count (task §7). Reads a
    // GUARDED_BY field under global_mtx_.
    static std::size_t waiting_select_count(const Scheduler& s) noexcept {
        LockGuard lk(s.global_mtx_);
        return s.waiting_select_count_;
    }

    // Test-only: bump the suspended-Select liveness count under global_mtx_.
    // Used by publication death tests that assemble a finalized group via the
    // real P4 processor (which does NOT run the admission suspension commit)
    // and then drive select_publish_locked on the suspended branch, which
    // expects waiting_select_count_ to reflect one Armed group. Mirrors the
    // admission suspension commit (task §7.2). Absent in production.
    static void inc_waiting_select_for_test(Scheduler& s) noexcept {
        LockGuard lk(s.global_mtx_);
        ++s.waiting_select_count_;
    }

    // Read a SelectGroup's stored result_ (the post-publication winner).
    // Returns the default no-winner SelectResult if the group has not been
    // published. Defined out-of-line (touches SelectGroup's complete type).
    static SelectResult group_result(const Scheduler& s,
                                     const detail::SelectGroup& group);

    // Direct publication driver: call select_publish_locked under
    // global_mtx_. For SN-2 (duplicate publication) / SN-10 (open authority)
    // / FP (caller not Waiting) death tests that must reach the production
    // entry on a synthetic-but-finalized group. `group` must already be
    // claimed + finalized + authority-closed by the test harness (the
    // production publication entry validates this mechanically and fails
    // fast otherwise).
    static void select_publish(Scheduler& s, detail::SelectGroup& group) {
        LockGuard lk(s.global_mtx_);
        s.select_publish_locked(group);
    }

    // ---- E13 P7 rollback-domain negative drivers (task §21) ----
    // Call the private production rollback authorities under global_mtx_ on
    // a test-assembled group, to prove the fail-fast preconditions fire on
    // every forbidden domain / corrupted membership. Used ONLY by the
    // rollback death tests; absent in production.
    static void select_begin_rollback(Scheduler& s,
                                      detail::SelectGroup& group) {
        LockGuard lk(s.global_mtx_);
        s.select_begin_rollback_locked(group);
    }
    static void select_rollback_arm(Scheduler& s, detail::SelectGroup& group,
                                    detail::SelectArmSlot& arm) {
        LockGuard lk(s.global_mtx_);
        s.select_rollback_arm_locked(group, arm);
    }
    static void select_finish_rollback(Scheduler& s,
                                       detail::SelectGroup& group,
                                       detail::SelectArmSlot* arms,
                                       std::size_t arm_count,
                                       std::size_t registered_count) {
        LockGuard lk(s.global_mtx_);
        s.select_finish_rollback_locked(group, arms, arm_count,
                                        registered_count);
    }
    static void select_rollback_registration(
        Scheduler& s, detail::SelectGroup& group,
        detail::SelectArmSlot* arms, std::size_t arm_count,
        std::size_t registered_count) {
        LockGuard lk(s.global_mtx_);
        s.select_rollback_registration_locked(group, arms, arm_count,
                                              registered_count);
    }

    // Direct suspended-Event resolver driver: call select_resolve_event_locked
    // under global_mtx_. Reaches the real production P8-gate + claim +
    // finalize + publish path from a test.
    static bool select_resolve_event(Scheduler& s, Event& event) {
        LockGuard lk(s.global_mtx_);
        return s.select_resolve_event_locked(event);
    }

    // Direct suspended-Timer resolver driver: call select_resolve_timer_locked
    // under global_mtx_. Reaches the real production ACTIVE-path resolver
    // from a test.
    static bool select_resolve_timer(Scheduler& s,
                                     detail::SelectTimerRegistration& reg) {
        LockGuard lk(s.global_mtx_);
        return s.select_resolve_timer_locked(reg);
    }

    // Drain the Select timer pool to empty: retire every still-ACTIVE
    // block (via the Scheduler accounting helper, so counters stay
    // consistent), then advance the clock far past every deadline so the
    // pump physically reclaims each block. Used by test fixtures to honor
    // the ~Scheduler quiescence contract. Acquires/releases global_mtx_.
    static void drain_select_pool(Scheduler& s) {
        // Retire ACTIVE blocks under G (so advancing the clock later hits
        // the stale-skip path, not the ACTIVE fail-fast).
        {
            LockGuard lk(s.global_mtx_);
            for (auto& reg : s.select_timer_pool_) {
                if (reg.is_active()) s.select_timer_retire_locked(reg);
            }
        }
        // Pump every Select entry to physical reclamation. A large deadline
        // covers all test fixtures' deadlines.
        s.advance_clock(static_cast<deadline_t>(1) << 62);
    }

    // ---- E12-F AsyncRwLock Category B death-test accessors ----
    //
    // These exist ONLY to construct a deliberately-corrupted linked-node
    // topology and then invoke the SAME production grant path
    // (rwlock_grant_from_head_locked), so the fail-fast is the production
    // one (assert(false) + std::abort in BOTH Debug and Release). They do
    // NOT expose the production WaitQueue structural authority to
    // ordinary tests: each entry takes an AsyncRwLock& (the primitive),
    // not a WaitQueue&, and the forged user_ is installed through this
    // seam — the ONLY non-production code permitted to do so while a node
    // is linked. Acquires global_mtx_ internally. The forged node is left
    // linked; the grant invocation MUST terminate before any subsequent
    // use. Absent in production (compiled only under the define).
    //
    // B1: forge a head node whose user_ points at a context with an
    //     invalid mode (neither read nor write). The grant's switch
    //     default MUST abort.
    static void rwlock_death_forge_invalid_head_mode(Scheduler& s,
                                                     AsyncRwLock& rw);
    // B2: forge a head node whose user_ is null. The grant's null-user_
    //     check MUST abort.
    static void rwlock_death_forge_null_head_user(Scheduler& s,
                                                  AsyncRwLock& rw);
    // B3: forge a head reader prefix whose SECOND node has an invalid
    //     mode. The reader-batch per-node mode check MUST abort after
    //     claiming the (valid) head reader.
    static void rwlock_death_forge_invalid_batch_member(Scheduler& s,
                                                        AsyncRwLock& rw);

    // ---- I47-F1: Runnable publication owner-domain snapshot ----
    // Captures the placement of a Fiber's runnable ticket relative to its
    // recorded owner Worker and a resolver Worker. Used by the owner-domain
    // regression test to prove whether a woken Fiber is routed to its owner
    // or incorrectly to the resolver. Follows production lock order:
    // global_mtx_ -> relevant Worker inbox_mtx.
    struct RunnablePublicationSnapshot {
        unsigned fiber_owner_worker_id{static_cast<unsigned>(-1)};
        bool owner_queue_contains_fiber{false};
        bool resolver_queue_contains_fiber{false};
        bool pending_spawn_contains_fiber{false};
        bool owner_suspend_switch_pending{false};
        FiberState fiber_state{FiberState::created};
    };

    static RunnablePublicationSnapshot capture_runnable_publication(
        Scheduler& s, Fiber& fiber,
        unsigned owner_worker_id, unsigned resolver_worker_id) {
        RunnablePublicationSnapshot snap;
        LockGuard lk(s.global_mtx_);
        // Owner lookup.
        auto it = s.fiber_owner_.find(&fiber);
        if (it != s.fiber_owner_.end() && it->second != nullptr) {
            snap.fiber_owner_worker_id = it->second->id;
        }
        // Fiber state (atomic, no lock needed but read under G for consistency).
        snap.fiber_state = fiber.state();
        // Owner suspend authority.
        if (owner_worker_id < s.workers_.size()) {
            snap.owner_suspend_switch_pending =
                s.workers_[owner_worker_id]->suspend_switch_pending.load(
                    std::memory_order_acquire);
        }
        // Check owner's local_runnable.
        if (owner_worker_id < s.workers_.size()) {
            auto& w = *s.workers_[owner_worker_id];
            std::lock_guard<std::mutex> wlk(w.inbox_mtx);
            for (auto* f : w.local_runnable) {
                if (f == &fiber) { snap.owner_queue_contains_fiber = true; break; }
            }
        }
        // Check resolver's local_runnable.
        if (resolver_worker_id < s.workers_.size() &&
            resolver_worker_id != owner_worker_id) {
            auto& w = *s.workers_[resolver_worker_id];
            std::lock_guard<std::mutex> wlk(w.inbox_mtx);
            for (auto* f : w.local_runnable) {
                if (f == &fiber) { snap.resolver_queue_contains_fiber = true; break; }
            }
        }
        // Check pending_spawn_.
        for (auto* f : s.pending_spawn_) {
            if (f == &fiber) { snap.pending_spawn_contains_fiber = true; break; }
        }
        return snap;
    }

    // ---- E14 RT-F3: init_fiber failure injection ----
    // Force the NEXT Scheduler::init_fiber() call to return false,
    // simulating an invalid stack or unsupported architecture. The flag
    // is consumed (one-shot): after one init_fiber returns false, the
    // override resets to normal. Does NOT modify fiber_ctx state.
    static void force_next_init_fiber_fail(Scheduler& s) noexcept {
        s.force_init_fiber_fail_.store(true, std::memory_order::release);
    }
    static bool init_fiber_fail_armed(const Scheduler& s) noexcept {
        return s.force_init_fiber_fail_.load(std::memory_order::acquire);
    }

    // ---- E14 RT-F5: Evented admission override ----
    // Override the fiber_ctx::supported check so tests on x86_64 can
    // simulate an unsupported target. When set to false, the next
    // Scheduler construction (or Group(Scheduler&) construction) calls
    // evented_admission_fail_fast(). Reset to true restores normal.
    // Global (not per-Scheduler) because it gates construction.
    static void set_evented_admission_override(bool supported) noexcept;
    static bool evented_admission_override() noexcept;

    // ---- FE-2 minimal stackless frontend seams (fe2-frontend-seam
    // compliance gate; non-installed control plane per AGENTS.md §15) ----
    //
    // FeDeferredRecord is the experimental stackless frontend's continuation
    // record: frame-embedded (same address-stability rule as the WaitNode
    // itself, FE-1b L2), holding the exactly-once PUBLICATION guard that is
    // SUBORDINATE to the WaitNode terminal winner (the resolve_ CAS decides;
    // this record only dedups delivery of an already-decided terminal).
    //
    //   unarmed --arm(admission CS, under G, L7)--> armed
    //   armed   --try_consume(drain, no lock)-----> consumed   [exactly once]
    //
    // A try_consume on an unarmed record returns false and the drain loop
    // treats it as a loud contract violation (resume-before-armed = L8
    // broken); the PoV test asserts it is unreachable.
    struct FeDeferredRecord {
        enum class State : std::uint8_t { unarmed = 0, armed = 1, consumed = 2 };
        std::atomic<State> state{State::unarmed};
        void* handle_address = nullptr;  // bound BEFORE registration (L2)
        // PublicationEligibilityCommit (FE-1b L7). Called ONLY by
        // event_wait_deferred_*_for_test inside the resolver-excluded
        // admission critical section, after the ladder authorizes. Plain
        // release store: the single armer is the admission CS; G orders it
        // against every resolver.
        void arm(std::memory_order order = std::memory_order_release) noexcept {
            state.store(State::armed, order);
        }
        // Exactly-once delivery guard (FE-1b L8). acq_rel: the winner pairs
        // with arming; a losing discharger observes `consumed` and does
        // nothing. Returns false for unarmed (contract violation observed by
        // the caller) and for consumed (loser law).
        bool try_consume() noexcept {
            State expected = State::armed;
            return state.compare_exchange_strong(expected, State::consumed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
        }
    };

    // Deferred-kind Event wait admission over the SHARED ladder (the ONE
    // textual admission sequence — no duplicated admission law). Runs the
    // ladder for a WaitResume::deferred token; on `authorized` commits the
    // PublicationEligibility (record.arm) INSIDE the same resolver-excluded
    // critical section (FE-1b L7) and returns true (the caller suspends /
    // await_suspend returns true). `rejected` / inline `resolved_inline`
    // return false with the outcome on the node — the caller must NOT
    // suspend (FE-1b L6: inline resolution publishes nothing).
    //
    // Out-of-line (defined in scheduler_fe2_test_seam.cpp): these methods
    // touch Event's private state (via Scheduler friendship) and therefore
    // need the complete Event type, which the installed header's include
    // footprint must NOT gain (the seam header is included by every
    // internal-testing TU at the bottom of scheduler.hpp).
    static bool event_wait_deferred_for_test(Scheduler& s, Event& event,
                                             WaitNode& node,
                                             FeDeferredRecord& record);
    // Deadline-aware variant: same shared ladder with the ordinary deadline
    // authority (timed=true) — the SAME prepare/publish/already-due/retire
    // lifecycle the fiber entry uses. No second timer authority.
    static bool event_wait_deferred_deadline_for_test(Scheduler& s,
                                                      Event& event,
                                                      WaitNode& node,
                                                      FeDeferredRecord& record,
                                                      deadline_t deadline);
    // Event cancel through the SAME production seam the fiber frontend uses
    // (event_cancel_wait). The winner tail switches on the deferred kind and
    // commits the delivery obligation; the record must be armed (it is —
    // resolvers run only after the admission CS that armed it released).
    static bool event_cancel_deferred_for_test(Scheduler& s, Event& event,
                                               WaitNode& node);
    // Drain chunk: move out pending deferred delivery records under G. The
    // caller discharges each with NO lock held (FE-1b L9).
    static std::size_t take_deferred_for_test(Scheduler& s, void** out,
                                              std::size_t cap) {
        return s.take_deferred_publications(out, cap);
    }
    // Transit-list depth probe (teardown precondition observations).
    static std::size_t deferred_depth_for_test(Scheduler& s) {
        LockGuard lk(s.global_mtx_);
        return s.deferred_publications_.size();
    }
    // FE-2 §22 no-user-code-under-lock witness probe: non-blocking G
    // acquisition. A resumed continuation calls this from its body; true
    // proves G was FREE at that instant (the discharge path holds no
    // authoritative lock). If a mutated drain resumed under G, try_lock
    // would deterministically return false (and never deadlock — unlike a
    // blocking probe). TSA suppressed: the paired try_lock/unlock cannot be
    // expressed with annotated RAII guards (same shape as
    // worker_park_domain_try above).
    static bool try_lock_global_for_test(Scheduler& s) noexcept
        SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        return s.global_mtx_.try_lock();
    }
    static void unlock_global_for_test(Scheduler& s) noexcept
        SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        s.global_mtx_.unlock();
    }

    // ---- FE-3 Queue deferred-frontend seams (FE campaign slice; same
    // test-only scope as the Event seams above) ----
    //
    // The deferred QUEUE admission entries run the SHARED production push/pop
    // ladders (queue_push_admit_locked / queue_pop_admit_locked) for a
    // WaitResume::deferred token, reproduce the QueuePort ordinary-entry
    // protocol (lifecycle gate + active_port_calls_ interval + the
    // detached->producer_operation control transition + QueueWaitCtx stashing)
    // that QueuePort::push/pop perform for the fiber frontend, commit the
    // PublicationEligibility (record.arm) INSIDE the resolver-excluded
    // admission CS on `authorized`, and run the Q-LIV-1 opposite-role grant
    // under G + S after the role mutex is released. All semantic authority is
    // production code; this TU adds none.
    //
    // `ctx` is the COROUTINE-FRAME-EMBEDDED wait context (FE-1a lifetime rule:
    // the grant winner writes through ctx->prod_lease / ctx->cons_out AFTER
    // suspension, so it must be address-stable — never a C++ stack frame that
    // dies at await_suspend). Defined out-of-line in
    // scheduler_fe2_test_seam.cpp (needs the complete QueuePort + queue_detail
    // internals).
    static bool queue_push_deferred_for_test(Scheduler& s,
                                             detail::QueuePort& port,
                                             detail::QueueItemLease& lease,
                                             WaitNode& node, QueueWaitCtx& ctx,
                                             FeDeferredRecord& record);
    static bool queue_push_deferred_until_for_test(
        Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
        WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
        deadline_t deadline);
    static bool queue_pop_deferred_for_test(Scheduler& s,
                                            detail::QueuePort& port,
                                            detail::QueueItemLease& out,
                                            WaitNode& node, QueueWaitCtx& ctx,
                                            FeDeferredRecord& record);
    static bool queue_pop_deferred_until_for_test(
        Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& out,
        WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
        deadline_t deadline);
    // Queue cancel through the SAME production seam the fiber frontend uses
    // (queue_cancel). The winner tail switches on the deferred kind and
    // commits the delivery obligation.
    static bool queue_cancel_deferred_for_test(Scheduler& s,
                                               detail::QueuePort& port,
                                               detail::QueueRole role,
                                               WaitNode& node) {
        return s.queue_cancel(port, role, node);
    }
    // Lease-emptiness observation for the post-resume status mapping
    // (QueueItemLease::control_ is authority-private; the test coroutines map
    // outcome + emptiness to push/pop statuses exactly as QueuePort does).
    static bool queue_lease_empty_for_test(const detail::QueueItemLease& l) {
        return l.control_ == nullptr;
    }
    // An EMPTY lease for coroutine-frame pop out-parameters. QueueItemLease's
    // default ctor is authority-private; the fiber frontend gets its empty
    // out-lease inside QueuePort::pop (friend). The deferred frontend's frame
    // needs the same object without minting a control: returns an empty lease
    // by value (the public move ctor performs the transfer).
    static detail::QueueItemLease queue_make_empty_lease_for_test() {
        return detail::QueueItemLease{};
    }
    // CS cores shared by the blocking/timed entries above (defined in
    // scheduler_fe2_test_seam.cpp). Assume the entry protocol (validation +
    // control transition + ctx stash) already ran.
    //
    // PIN CONTRACT (FE-CORRECTIVE-1 P1-2): every NON-throw return transfers
    // one QueuePort ordinary-call pin (active_port_calls_) to the CALLER —
    // the fiber frontend's CallGuard stays on the suspended fiber stack
    // through resume-side conversion; the deferred frontend's awaiter holds
    // the obligation instead and MUST release it in await_resume, AFTER the
    // port-dependent result conversion (release_popped/release_failed
    // validate owner_port_ against a live port), via
    // queue_release_deferred_pin_for_test — exactly once per non-throw
    // call. A throw releases inside the core (await_resume never runs).
    // An abandoned suspended frame leaves the pin held: begin_teardown
    // fail-fasts on it (mechanically visible abandonment).
    static bool queue_push_core_(Scheduler& s, detail::QueuePort& port,
                                 detail::QueueItemLease& lease, WaitNode& node,
                                 FeDeferredRecord& record, bool timed,
                                 deadline_t deadline);
    static bool queue_pop_core_(Scheduler& s, detail::QueuePort& port,
                                detail::QueueItemLease& out, WaitNode& node,
                                FeDeferredRecord& record, bool timed,
                                deadline_t deadline);
    // Release one transferred ordinary-call pin under G + S (the production
    // CallGuard dtor's exact domain/shape). Call with NO lock held.
    // Over-release (counter already zero) fail-fasts.
    static void queue_release_deferred_pin_for_test(detail::QueuePort& port);
    // Ordinary-call pin observation (QPIN phase witnesses): the deferred-op
    // lifetime obligation is 1 from entry acceptance until resume-side
    // result consumption completes.
    static std::size_t queue_active_port_calls_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.active_port_calls_;
    }
    // Remaining QueuePort teardown counters (QPIN accounting-table
    // witnesses; all G+S-synchronized fields).
    static std::size_t queue_active_wait_associations_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.active_wait_associations_;
    }
    static std::size_t queue_active_queue_timers_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.active_queue_timers_;
    }
    static std::size_t queue_granted_not_resumed_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.granted_not_resumed_;
    }

    // ---- FE-3 RwLock deferred-frontend seams (FE campaign slice; same
    // test-only scope as the Event/Queue seams above) ----
    //
    // The deferred RWLOCK admission entries run the SHARED production
    // read/write ladders (rwlock_read_admit_locked / rwlock_write_admit_locked)
    // for a WaitResume::deferred token and commit the PublicationEligibility
    // (record.arm) INSIDE the resolver-excluded admission CS on `authorized`.
    // `actor_token` is the caller's stable ACTOR identity (ActorId::frontend;
    // FE-1b A1 — never the resume target, never a coroutine_handle): the
    // writer-grant commits it into writer_owner, and the checked release core
    // compares it. `ctx` is the COROUTINE-FRAME-EMBEDDED RwWaitCtx (the grant
    // reads ctx->actor after suspension). Defined out-of-line in
    // scheduler_fe2_test_seam.cpp (needs the complete AsyncRwLock + the shared
    // RwWaitCtx internal).
    static bool rwlock_read_deferred_for_test(Scheduler& s, AsyncRwLock& lock,
                                              WaitNode& node, void* actor_token,
                                              RwWaitCtx& ctx,
                                              FeDeferredRecord& record);
    static bool rwlock_read_deferred_until_for_test(
        Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
        RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline);
    static bool rwlock_write_deferred_for_test(Scheduler& s, AsyncRwLock& lock,
                                               WaitNode& node,
                                               void* actor_token,
                                               RwWaitCtx& ctx,
                                               FeDeferredRecord& record);
    static bool rwlock_write_deferred_until_for_test(
        Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
        RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline);
    // Inline try-write through the SHARED admission core (the ONE textual
    // ownership decision) with a frontend actor: recursive detection by the
    // same actor returns false; a DIFFERENT actor is refused while
    // writer_active — both independently of any ResumeTarget.
    static bool rwlock_try_write_deferred_for_test(Scheduler& s,
                                                   AsyncRwLock& lock,
                                                   void* actor_token);
    // Release through the SHARED checked core with a frontend actor: a
    // non-owner (or inactive) release fail-fasts on the ACTOR comparison.
    static void rwlock_unlock_write_deferred_for_test(Scheduler& s,
                                                      AsyncRwLock& lock,
                                                      void* actor_token);
    // CS cores shared by the blocking/timed rwlock entries above (defined in
    // scheduler_fe2_test_seam.cpp).
    static bool rwlock_read_core_(Scheduler& s, AsyncRwLock& lock,
                                  WaitNode& node, void* actor_token,
                                  RwWaitCtx& ctx, FeDeferredRecord& record,
                                  bool timed, deadline_t deadline);
    static bool rwlock_write_core_(Scheduler& s, AsyncRwLock& lock,
                                   WaitNode& node, void* actor_token,
                                   RwWaitCtx& ctx, FeDeferredRecord& record,
                                   bool timed, deadline_t deadline);
    // Read-share release through the production seam (no actor: v1 reader
    // ownership is a count). Head reconcile included.
    static void rwlock_unlock_read_for_test(Scheduler& s, AsyncRwLock& lock);
    // Try-read through the production seam (no barging observation; a true
    // result commits one read share).
    static bool rwlock_try_read_for_test(Scheduler& s, AsyncRwLock& lock);
    // RwLock cancel through the SAME production seam the fiber frontend uses
    // (rwlock_cancel; head reconcile included).
    static bool rwlock_cancel_deferred_for_test(Scheduler& s,
                                                AsyncRwLock& lock,
                                                WaitNode& node);
    // Writer-state observations for the ownership tests (authority-private
    // fields; the Fiber-free frontend has no other way to observe them).
    // Both read G-serialized ownership state under global_mtx_ — the same
    // authority the resolvers use (FE-CORRECTIVE-1 P1-3).
    static bool rwlock_writer_active_for_test(AsyncRwLock& lock);
    static bool rwlock_owned_by_for_test(AsyncRwLock& lock,
                                         const void* actor_token);

    // ---- FE-3 Condition slice: deferred CONDITION-WAIT-PREPARE entries -----
    //
    // The Condition epoch over the ONE shared admission ladder
    // (condition_wait_admit_locked): register -> [timed: R2-ALLOC prepare +
    // LOCAL publish] -> already-due inline Expired -> register-before-handoff
    // phase seam -> Mutex handoff -> terminal recheck -> authorized, all under
    // ONE global_mtx_ CS. On `authorized` the seam commits the deferred
    // PublicationEligibility (record.arm) in the SAME CS and returns true
    // (the caller suspends). On any other disposition it returns false (the
    // caller continues inline) and latches `released` from the disposition —
    // the released_mutex law (false = the presented Mutex state was NOT
    // released: no reacquire epoch; true = released/handed off: the resumed
    // body runs its OWN reacquire epoch; the notify/cancel/expire resolver
    // NEVER runs it for the winner).
    //
    // `cond_waiters` / `mutex_waiters` / `owner` are the presented Condition +
    // bound-Mutex state (the same by-reference shape the fiber seam takes).
    // The v1 PoV presents BARE WaitQueues: Mutex ownership identity re-typing
    // (FE-1b A1 §12: "Mutex/RwLock owner fields are re-typed") is its own
    // later slice — RwLock is done; until then a stackless coroutine cannot
    // lawfully OWN an AsyncMutex, so the full AsyncCondition choreography
    // composition stays covered by the unchanged fiber tests running over the
    // SAME ladder. With an empty presented mutex queue the handoff is the
    // documented UnlockNoWaiter no-op (`owner = nullptr`); the deferred entry
    // never dereferences `owner`.
    //
    // Returns true when the caller must suspend (record armed); false when the
    // ladder resolved inline (`node.outcome()` is terminal, `released` latched).
    static bool condition_wait_deferred_for_test(Scheduler& s,
                                                 WaitQueue& cond_waiters,
                                                 WaitNode& cond_node,
                                                 WaitQueue& mutex_waiters,
                                                 Fiber*& owner,
                                                 FeDeferredRecord& record,
                                                 bool& released);
    static bool condition_wait_deferred_until_for_test(
        Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
        WaitQueue& mutex_waiters, Fiber*& owner, deadline_t deadline,
        FeDeferredRecord& record, bool& released);

    // One-liner resolvers over the presented BARE Condition queue (the public
    // AsyncCondition::notify_* / cancel cannot be used: the PoV presents bare
    // WaitQueues, not a bound AsyncCondition). All three already route
    // publication through the ONE winner-kind tail.
    static void condition_notify_one_for_test(Scheduler& s,
                                              WaitQueue& cond_waiters) {
        s.condition_notify_one(cond_waiters);
    }
    static std::size_t condition_notify_all_for_test(Scheduler& s,
                                                     WaitQueue& cond_waiters) {
        return s.condition_notify_all(cond_waiters);
    }
    static bool condition_cancel_for_test(Scheduler& s,
                                          WaitQueue& cond_waiters,
                                          WaitNode& cond_node) {
        return s.condition_cancel_wait(cond_waiters, cond_node);
    }

    // Shared body of the two deferred condition entries (out-of-line; holds
    // G for the ladder + the arm commit, derives `released` from the
    // disposition).
    static bool condition_wait_deferred_core_(
        Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
        WaitQueue& mutex_waiters, Fiber*& owner, FeDeferredRecord& record,
        bool timed, deadline_t deadline, bool& released);
};  // struct Scheduler::AsyncTestAccess

}  // namespace sluice::async

#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
