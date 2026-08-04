-- Tests linking sluice_async_internal_testing (NOT production sluice_async).
-- These exercise deterministic causal seams exposed by SLUICE_ASYNC_INTERNAL_TESTING.
-- Built/run via `xmake -g test`.

local R = SLUICE_ROOT

-- E7-C coordination tests (sluice-CORE-E7-C). Serialized backend access probe,
-- quiescence, MW-S3. Gated to x86_64.
sluice_internal_async_test("multi_worker_coord_test")

-- Issue #50: deterministic worker-topology authority judge. Pauses run_impl
-- at topology mutation and proves global_mtx_ excludes a concurrent spawn
-- reader, then verifies the admitted Fiber executes exactly once.
sluice_internal_async_test("scheduler_worker_topology_race_test")

-- Issue #50 ApplicationRuntime regression. Drives the real Runtime submit /
-- stop / drain / join path while its Scheduler is paused at initial topology
-- publication, proving every admitted task reaches terminal execution.
sluice_internal_async_test("application_runtime_worker_topology_test")

-- E15-P2-02 Group::async_threaded exception-safety regression. Uses the
-- test_set_tasks_throw_on_nth() seam (only available under
-- SLUICE_ASYNC_INTERNAL_TESTING) to force tasks_ push_back to throw, proving
-- the join-on-failure path keeps no joinable thread stranded. The production
-- sluice_async build compiles the seam out.
sluice_internal_async_test("group_exception_safety_test")

-- external_wake_test — Scheduler park admission + unified wake-source
-- protocol (sluice-CORE-E9). Proves external-thread flag completion wakes a
-- parked Scheduler (no caller re-entry), MIXED-WAKE closure, wake coalescing,
-- the pre-park race, wake-handle lifetime, and E7/E8 runnable/shutdown wake.
-- Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("external_wake_test")

-- wake_handle_lifetime_test — SchedulerWakeHandle callback-lifetime lease
-- (sluice-CORE-E9 LIFETIME-CORRECTIVE). Proves notify() holds Control::mtx
-- (the callback lease) through the Scheduler wake callback, so destruction
-- cannot interleave with an in-flight callback. Deterministic T1 (notifier
-- wins) / T2 (destructor wins) / T3 (stale handle) + concurrent T4 stress.
-- Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("wake_handle_lifetime_test")

-- timer_wait_test — Deadline / Timer Wait Integration (sluice-CORE-E11).
-- Deterministic production tests: already-due deadline, resource-wins/timer-
-- wins/cancel-wins races at the resolve_ seam, losing-timer cannot publish,
-- stale timer cannot resolve a later wait epoch, storage-reuse epoch isolation,
-- timer retirement closes WaitNode dereference, deadline park liveness,
-- RunMode classification. Uses a controllable monotonic clock + explicit timer
-- driver (NO sleep_for causal proof). Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("timer_wait_test")

-- event_primitive_test — Async Event synchronization primitive (sluice-CORE-E12-A).
-- Persistent manual-reset Event on the E10/E11 substrate: basic semantics,
-- lost-set admission closure, set-all broadcast, deadline/cancel composition,
-- set/reset epoch isolation, external-thread set, E8 steal, Drain STALLED,
-- destruction contract. Deterministic causal tests (NO sleep_for proof).
-- Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("event_primitive_test")

-- semaphore_primitive_test — Async counting Semaphore (sluice-CORE-E12-B).
-- Counting Semaphore on the E10/E11/E12-A substrate: construction/available,
-- try_acquire (no barging), immediate + queued acquire, FIFO release transfer
-- /store/overflow, deadline precedence (permit-first), queue-identity-safe
-- cancel, external-thread release, Drain STALLED, destruction contract.
-- Deterministic causal tests (NO sleep_for proof). Gated to x86_64
-- (fiber_ctx::supported).
sluice_internal_async_test("semaphore_primitive_test")

-- async_mutex_primitive_test — Fiber-suspending Async Mutex (sluice-CORE-E12-C).
-- AsyncMutex on the E10/E11/E12-A/E12-B substrate: construction/try_lock,
-- immediate + queued lock, FIFO direct handoff, deadline precedence
-- (resource-first), queue-identity-safe cancel, external-thread cancel,
-- migration, destruction contract, 500/500 coordination.
-- Deterministic causal tests (NO sleep_for proof). Gated to x86_64
-- (fiber_ctx::supported).
sluice_internal_async_test("async_mutex_primitive_test")

-- async_condition_primitive_test — Fiber-suspending async condition variable
-- (sluice-CORE-E12-D). AsyncCondition bound to one AsyncMutex: two-epoch
-- (Condition wait + mandatory reacquire) protocol, register-before-release
-- lost-notify closure, notify_one FIFO, notify_all snapshot/drain,
-- notify/cancel/expire winner matrix, Ordinary<->Reacquire FIFO mixing,
-- owner-before-publication, inline-Expired retains ownership, destruction
-- contract. Deterministic causal tests via E12ConditionSeam phase seams
-- (NO sleep_for proof). Gated to x86_64 (fiber_ctx::supported). The authority
-- probe (async_condition_authority_probe.cpp) is NOT a target: it is a
-- negative-compile probe driven by the verify script's compile-probe gate.
sluice_internal_async_test("async_condition_primitive_test")

-- async_rwlock_test — Fiber-suspending Async Read-Write Lock (sluice-CORE-E12-F).
-- Writer-fair phase-batched RwLock: try/read/write/unlock, FIFO fairness,
-- reader batch grant, writer starvation prevention, cancel + head reconcile,
-- deadline (read_lock_until / write_lock_until), timer expiry routing.
-- Deterministic causal tests (NO sleep_for proof). Gated to x86_64.
sluice_internal_async_test("async_rwlock_test")

-- async_queue_primitive_test — AsyncQueue (sluice-CORE-E12-E).
-- P2+P3 scope: QueuePort fast paths (try_push / try_pop / close / snapshot),
-- capacity/FIFO, failed-payload identity, one-shot lease, close idempotency,
-- closed+empty terminal. Exercised via the non-template QueuePort authority +
-- QueueItemFactory (the public AsyncQueue<T> wrapper lands in P8). The
-- blocking/timed wait-admission paths (P4-P6) and Scheduler reconciliation
-- land later; this target covers only the no-Scheduler fast paths. Links
-- sluice_async_internal_testing (the authority lives in the non-template
-- QueuePort, which is in sluice_async; the internal-testing variant keeps
-- the option open for the deterministic phase seams added in P5/P6).
sluice_internal_async_test("async_queue_primitive_test")

-- async_mutex_nothrow_authority_probe — positive-compile + run probe for
-- the Mutex noexcept contract (ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1
-- §I1). Holds the static_asserts over noexcept(...) and
-- std::is_nothrow_invocable_v<...> for lock/try_lock/unlock so a regression of
-- the noexcept function-type is caught at compile time. NOT a substitute for
-- the death tests (those verify runtime fail-fast behavior). Depends on the
-- internal_testing variant so the seam header resolves, though the probe
-- itself exercises the production Mutex entries.
sluice_internal_async_test("async_mutex_nothrow_authority_probe")

-- async_sync_api_contract_probe — cross-primitive compile-time contract probe
-- (E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1).
-- Verifies that every public async synchronization primitive is non-copyable
-- AND non-movable (D5), that WaitOutcome is the four-value vocabulary enum,
-- and that the typed Queue result types remain move-assignable even when
-- T is NOT move-assignable (PR #12 corrective). PURE compile-time probe: all
-- verification is static_assert; main() is trivial. Does NOT replace the
-- per-primitive authority probes — this gates the cross-primitive parity
-- contract only. This normal xmake target is the POSITIVE compile/run probe;
-- scripts/verify-async-api-negative-compile.sh separately defines each
-- NEG_* macro and requires that compilation fail for the intended deleted
-- special member. Depends on sluice_async_internal_testing so the seam header
-- resolves (the positive probe itself exercises the public production surface).
sluice_internal_async_test("async_sync_api_contract_probe")

-- async_sync_cross_primitive_parity_test — cross-primitive semantic parity tests
-- (E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1). Directly verifies D3
-- resource-first deadline precedence and D4 queue-identity cancellation for
-- Event / Semaphore / AsyncMutex, plus pairwise WaitOutcome enum distinctness
-- and a fresh unresolved WaitNode. AsyncCondition/Queue and dynamic terminal
-- publication remain per-primitive evidence; this TU does not claim them.
-- Gated to x86_64 (fiber_ctx::supported).
do
    local p = R .. "tests/async_sync_cross_primitive_parity_test.cpp"
    if os.isfile(p) and is_arch("x86_64") then
        target("async_sync_cross_primitive_parity_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs(R .. "include", R .. "tests")
            add_files(p)
            add_tests("async_sync_cross_primitive_parity_test")
    end
end

-- select_type_test — E13 Select type construction and compile-fail gates (P1).
-- Tests public value types, internal type graph, and constraint gates.
-- Deterministic, NO sleep_for. Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("select_type_test")

-- select_event_registry_test — E13 Event Select private registry (P2).
-- Sealed per-Event SelectPort: link/unlink/scan operations, Event SET Phase-1
-- scan, idempotence, serialization, and destruction contract.
-- Deterministic, NO sleep_for. Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("select_event_registry_test")

-- select_timer_registration_test — E13 Select Timer stable registration (P3).
-- Verifies the Select timer substrate: state transitions, address stability
-- after splice, tagged deadline-heap ordering, ordinary timer regression,
-- RETIRED/CONSUMED stale-skip, state-before-arm instrumentation, earliest-
-- active-deadline participation, lazy reclamation, mixed stale+ordinary pump,
-- Scheduler identity, no-premature-Select. Deterministic (test clock + causal
-- phase seams); NO sleep_for. Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("select_timer_registration_test")

-- select_claim_test — E13 P4 Select central claim + winner/loser finalization
-- tests. Drives select_process_group_locked + select_all_authority_closed_locked
-- via guarded internal-testing seams. Covers C1-C12 (first-claim-wins, second-
-- claim-loses, winner stability, Event/Timer winner+loser, mixed, same-Event-
-- twice, claim-lost no-mutation, authority-closed invariant, no-publication,
-- Timer loser ordering SN-9). Deterministic (test clock + causal phase seams);
-- NO sleep_for. Gated to x86_64 (fiber_ctx::supported) for parity with E13.
sluice_internal_async_test("select_claim_test")

-- select_inline — E13 P5 inline Select admission tests (ST-1..ST-8 + T1/T2/T3).
-- Drives the PUBLIC variadic select() entry from a real running Fiber on the
-- target Scheduler: Event already-set, Timer already-due, Event/Timer tie
-- (lowest-index), duplicate Event, Event winner + Timer loser (stale pump skip),
-- Timer winner + Event loser; plus the template/link matrix (T1), all-arms-
-- registered-before-snapshot (T2), and the inline Completed->Consumed lifecycle
-- (T3). Deterministic (test clock + AdmissionArmed/Consumed causal phase seams);
-- NO sleep_for. Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("select_inline_test")

-- select_suspended — E13 P6 suspended Select publication tests (ST-9, ST-10,
-- ST-13 + P6-D1 same-Event-twice + PUB boundary snapshots + P6-LW1/LW2 wake-
-- before-physical-switch). Drives the PUBLIC variadic select() entry from a
-- real Fiber on the target Scheduler for the no-ready branch. Deterministic
-- (test clock + e13_select_suspend_before_switch / e13_publish_* /
-- e13_suspended_before_consume causal phase seams); NO sleep_for. Gated to
-- x86_64 (fiber_ctx::supported).
sluice_internal_async_test("select_suspended_test")

-- select_multi_worker — E13 P6 multi-worker owner routing + external-thread
-- Event set + exactly-one-runnable tests (ST-15, ST-16, ST-17 + PUB-1..4
-- publication boundary snapshots). Drives the PUBLIC variadic select() entry
-- from a real Fiber; resolves from an external OS thread and across workers.
-- Deterministic (test clock + waiting_select_count liveness + PhaseTag causal
-- seams); NO sleep_for. Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("select_multi_worker_test")

-- select_registration_rollback_test — E13 P7 registration-failure rollback
-- tests (ST-14 + P7-T1..T11). Drives the PUBLIC variadic select() entry from a
-- real running Fiber; the controller-only synthetic registration-failure seam
-- (E13SelectRollbackSeam, absent from production) injects a
-- SelectRegistrationFailure after N successful registrations. select_admit's
-- catch runs the rollback transaction and rethrows. Deterministic (test clock +
-- rollback observation); NO sleep_for. ASan is load-bearing for P7-T8 (stale
-- Timer after caller-frame unwind). Gated to platforms where the test file is
-- present; fiber work is gated at runtime via fiber_ctx::supported.
sluice_internal_async_test("select_registration_rollback_test")

-- select_call_context_contract_test — E13 P7 Select contract coverage
-- (ST-18..ST-23) for the genuinely-missing positive contract cases. Reuses
-- existing tests where they already prove the exact contract; this target adds
-- only missing coverage.
sluice_internal_async_test("select_call_context_contract_test")

-- threaded_evented_internal_test — Threaded/Evented RT-F3 real init_fiber failure
-- regression test. Uses SLUICE_ASYNC_INTERNAL_TESTING seam to force init_fiber
-- failure. Links against sluice_async_internal_testing (NOT production sluice_async).
sluice_internal_async_test("threaded_evented_internal_test")

-- group_evented_admission_exception_safety_test — P2-01 Group transactional
-- admission seam regression (§13.5). Uses the test_set_evented_admission_fail()
-- seam (only available under SLUICE_ASYNC_INTERNAL_TESTING) to force each of the
-- three reserve boundaries in async_evented to throw std::bad_alloc, proving the
-- admission is a complete transaction: a reserve failure leaves no partial
-- Fiber/stack/Future record, no Scheduler publication, and the Group remains
-- destructible and reusable. The production sluice_async build compiles the seam
-- out. Gated to x86_64 (fiber_ctx::supported) at runtime.
sluice_internal_async_test("group_evented_admission_exception_safety_test")

-- threadpool_backend_reap_test — ThreadPoolBackend persistent-worker regression
-- (Phase E). Proves the fixed worker pool never grows under load: the
-- SLUICE_ASYNC_INTERNAL_TESTING-only workers_spawned_for_test() seam equals the
-- configured worker_count for the backend's whole life, no matter how many ops
-- are submitted/drained, and every op terminates with the real result. This is
-- the Phase-E restatement of the original DIV-03/DIV-12 resource-bound
-- regression (the per-op-thread model is gone; the pool is bounded by
-- construction).
sluice_internal_async_test("threadpool_backend_reap_test")

-- backend_scheme_b_race_test — Phase B backend-level Scheme-B race regression
-- (review test-gap 1). Drives the raw FakeAsyncBackend with the
-- SLUICE_ASYNC_INTERNAL_TESTING-only SubmitPauseGate seam: a submit thread is
-- paused deterministically between commit and enqueue, a cancel thread wins
-- the pending terminal transition (Scheme B), the resumed enqueue no-ops and
-- acknowledges the pin, and poll() reaps the canceled Completion through the
-- slot-bound publication binding. This proves the REAL reference-backend
-- integration (submit thread + Completion binding + commit/enqueue barrier
-- pause), which the public API's access_mtx_ serialization hides.
sluice_internal_async_test("backend_scheme_b_race_test")

-- reference_backend_arena_lifecycle_test — Phase B reference backend migration
-- regression (commit 4). Proves FakeAsyncBackend + SyncBackend are actually
-- driven by the bounded RequestArena + five-stage admission + the synchronous
-- identity-bearing ReadySink, by asserting the OBSERVABLE consequences
-- (slot_in_use lifecycle, capacity rejections, exactly-once sink deliveries,
-- generation-on-reuse). Without these the migration would be indistinguishable
-- from a no-op rename. Lives in the internal-testing group because the
-- exactly-once delivery counter (sink_deliveries) is a
-- SLUICE_ASYNC_INTERNAL_TESTING-only seam (CodeRabbit finding / AGENTS §8).
sluice_internal_async_test("reference_backend_arena_lifecycle_test")
