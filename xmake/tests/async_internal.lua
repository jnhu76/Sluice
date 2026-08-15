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

-- phase_g_backend_progress_wake_test — Phase G backend-ready progress wake
-- integration (docs/design/phase-g-backend-progress-wake.md): the unified
-- MW-S2 backend-domain park with the Scheduler wake bridge, the deadline-
-- driven unbounded wake-domain park, and the E9-LIFE-8 termination-
-- convergence corrective (not-last idle worker signals the domain).
-- Deterministic (test clock + PhaseTag causal seams); NO sleep_for.
-- Gated to x86_64 (fiber_ctx::supported).
sluice_internal_async_test("phase_g_backend_progress_wake_test")

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

-- threadpool_backend_scheme_b_race_test — Phase E ThreadPoolBackend Scheme-B
-- race regressions. Drives the real backend through the
-- SLUICE_ASYNC_INTERNAL_TESTING-only pause gates (A/B/C/D) to prove:
--   A: enqueue and dispatch push share one work_mtx_ critical section;
--   B: enqueued cancel wins before dequeue and the syscall does not run;
--   C: running cancel records intent only and the real syscall result wins;
--   D: terminal publication happens after worker bookkeeping is observable.
-- Cases A and D fail on the pre-fix code; cases B and C are conformance proofs.
sluice_internal_async_test("threadpool_backend_scheme_b_race_test", {platform_gate = {"linux", "macosx"}})

-- threadpool_wait_drain_deadlock_test — issue #67 drain-starvation regression.
-- Deterministically drives the captured production deadlock state (a
-- participant parked in the backend ready wait after an empty reap) using the
-- running-gate seam + the wait-phase flag, and proves a second participant can
-- poll/reap while the first is parked, that close_admission wakes the parked
-- waiter as a control wake (0, no fabricated completion), and that the final
-- request drains. Fails (bounded) on the pre-fix code where wait_one held
-- access_mtx_ across the backend wait and starved every other poll/reap path.
sluice_internal_async_test("threadpool_wait_drain_deadlock_test", {platform_gate = {"linux", "macosx"}})

-- application_runtime_drain_starvation_test — issue #67 end-to-end Runtime
-- regression: the final backend-ready request must drain at shutdown. A task
-- awaits one real read while the MW-S2 participant parks in the backend ready
-- wait; request_stop() interrupts the park (control wake), the final request
-- completes and is reaped by the re-entered run, and drain()/join() return
-- with backend_ready == 0 and outstanding == 0. On the pre-fix code the run
-- parks forever and drain() never returns (bounded join -> clean failure).
sluice_internal_async_test("application_runtime_drain_starvation_test", {platform_gate = {"linux", "macosx"}})

-- async_stats_wait_race_test — issue #67 P1 follow-up regression for the
-- AsyncStats data race the split-wait fix introduced. wait_calls / completed_ops
-- are plain std::uint64_t and access_mtx_ is their only accounting domain, but
-- the fix moved both the park AND (incorrectly) the stats bumps out of the
-- lock. Case A races multiple wait_one() callers (wait_calls write/write);
-- case B races wait_one()'s reap-path completed_ops bump against a concurrent
-- poll()'s. Both assert EXACT final counters, not just liveness. Under the
-- pre-fix code TSan flags the non-atomic concurrent writes; the fix puts every
-- accounting access back inside access_mtx_. Run under TSan for the race proof
-- (AGENTS.md §16.3).
sluice_internal_async_test("async_stats_wait_race_test", {platform_gate = {"linux", "macosx"}})

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

-- request_waiter_borrow_lease_test — Phase C2c arena-level waiter/borrow/
-- delivery-lease matrix (Issue #68 rows 11-14). Proves at the RequestArena
-- authority layer: the borrow lifecycle across every state (prepare inactive,
-- commit active, survives pending/enqueued/running/backend_ready and every
-- cancel/wait-cancel path, reap ends it before completion-ready, rollback
-- never borrows, stale handles cannot touch a new occupant), the single-waiter
-- registration matrix + no-overwrite cardinality (final delivery = first
-- waiter), waiter-cancel vs I/O-cancel independence, the move-only lease
-- transfer chains (caller -> slot -> ReadyEvent / cancel_waiter return), the
-- by-value ReadyEvent across slot reuse, and the register-vs-terminal and
-- cancel_waiter-vs-reap races (std::barrier; exactly-one lease ownership).
-- Links sluice_async_internal_testing for the generation-validated
-- borrow_for_test / waiter_for_test observation seams.
sluice_internal_async_test("request_waiter_borrow_lease_test")

-- scheduler_identity_wake_test — Phase F1 (issue #98): the production
-- Scheduler consumes identity-bearing reap. Proves: await_completion registers
-- a REAL arena waiter (token + routing lease) + Scheduler routing record and
-- the wake arrives through the Scheduler-owned ReadyRoutingSink (no
-- Completion*-keyed map, no O(N) ready() re-scan on the production path);
-- completion-before-registration inline return (Race A, no lost wake);
-- exactly-once routing with no double wake on repeat reap; cancel_waiter
-- removes ONLY the waiter (I5) while the I/O still terminals + reaps;
-- cancel_waiter-vs-reap exactly-once (200-iteration soak); stale record
-- generation cannot wake a new occupant (Race C); duplicate waiter ->
-- synchronous invalid_state (I13); shutdown convergence (registry empty at
-- destruction); and the same identity contract on Fake/Sync/ThreadPool.
-- Links sluice_async_internal_testing for the ReadySink route counters and
-- the legacy-map probe (AsyncTestAccess).
sluice_internal_async_test("scheduler_identity_wake_test")

-- backend_c2c_waiter_borrow_test — Phase C2c FakeAsyncBackend integration
-- (rows 11-14). Proves the REAL Fake submit path carries the exact borrow
-- metadata active, that the waiter seam routes a real accepted Completion
-- through the REAL arena register_waiter/cancel_waiter authorities (no
-- side-band waiter map), that complete_*/cancel only produce backend_ready
-- while the borrow stays active until poll() reaps, that the production sink
-- delivers the registered token + lease exactly once, wait-cancel vs I/O-cancel
-- independence, and that a stale-generation waiter authority cannot touch a
-- live N+1 occupant.
sluice_internal_async_test("backend_c2c_waiter_borrow_test")

-- threadpool_backend_c2c_waiter_borrow_test — Phase C2c ThreadPoolBackend
-- integration (rows 11-14). Deterministic pause gates prove: the RUNNING
-- window borrow is active with the exact submitted fd/address/length and a
-- registered waiter survives enqueued -> running -> backend_ready; running
-- cancel intent ends neither the borrow nor the waiter; the backend_ready-
-- before-reap window still shows the borrow active (a worker finishing its
-- syscall is NOT the borrow lifetime end; only reap releases it); wait-cancel
-- removes only the waiter (the real syscall still executes); enqueued I/O
-- cancel keeps the waiter (canceled result + waiter delivered together); and
-- a stale waiter authority is harmless against a live N+1 occupant.
sluice_internal_async_test("threadpool_backend_c2c_waiter_borrow_test", {platform_gate = {"linux", "macosx"}})

-- threadpool_backend_c2d_failure_test — Phase C2d ThreadPoolBackend failure
-- injection / accepted-terminal under allocator failure (Issue #68 rows 9-10).
-- Deterministic injection seams prove: (0) ADR Gate-4 per-stage pre-commit
-- injection at reserve (injected would_block; Completion idle; zero residue),
-- prepare (candidate slot rolled back; capacity recyclable), and the
-- COMMIT-BOUNDARY (the binding CAS wins, then commit is injected to fail —
-- the submit path executes the REAL rollback_binding_before_accept + slot
-- rollback, the only executable instance of that branch in the corpus; the
-- Completion returns to fully reusable idle); (1) pre-commit rejection on the
-- REAL backend is transactional (binding-CAS loss -> invalid_state, zero
-- residue, capacity immediately recyclable); (2) partial worker-startup
-- failure stops and joins the already-started workers and rethrows
-- synchronously (finding P1-04); (3) a post-commit permanent dispatch failure
-- (injected between enqueue and dispatch push, inside work_mtx_, with no
-- worker ever able to see the handle) leaves submit successful, drives the
-- request to exactly ONE defined backend_error terminal, publishes once via
-- reap, keeps the borrow active until reap, and never executes a worker or
-- syscall — for BOTH the size and void operation paths; (4) the accepted
-- submit -> enqueue/terminal -> reap -> reset path performs ZERO heap
-- allocations under an always-throw operator new (ADR Decision 14 / I9) on
-- the real worker path and on the injected failure path; (5) the
-- dispatch-failure terminal vs cancel has exactly one winner, no overwrite,
-- no double publication, and at most one tally in every interleaving
-- (canceled_ops == 1 iff cancel won — the injected backend_error terminal
-- contributes no tally). Gated to linux/macosx (POSIX syscalls).
sluice_internal_async_test("threadpool_backend_c2d_failure_test", {platform_gate = {"linux", "macosx"}})

-- threadpool_backend_c2e_close_drain_test — Phase C2e ThreadPoolBackend
-- close/drain/destruction deterministic tests (Issue #68 rows 15-16; ADR
-- Decision 15). Deterministic pause gates prove: close while the submit path
-- is paused between commit and enqueue (`pending`) / while the request is
-- `enqueued` on the ring / while the worker is `running` the syscall — in
-- every window the accepted request completes with its REAL result verbatim
-- (close never retroactively rejects, cancels, or discards; the void path
-- too); void submit after close -> invalid_state with idle Completion; close
-- then pending cancel still WINS the canceled terminal (Scheme B, no dispatch
-- linkage, no syscall); close then running cancel records intent only (real
-- result verbatim); close wakes a parked wait_one as a ONE-SHOT control wake
-- (0, no fabricated completion) and a FUTURE wait_one parks normally again and
-- is woken by real progress (no busy-spin); close || final record_terminal in
-- BOTH orderings (close first: the interrupted wait_one's final reap returns
-- 0 and the NEXT wait_one reaps the final ready — the control interrupt never
-- swallows it; terminal first: close does not affect an already-stored
-- terminal); an invariant-only close-vs-workers race drain (every accepted
-- request reaches exactly one verbatim terminal, accounting zero); and the
-- submit || close concurrent linearization invariant (every attempt is
-- accepted-then-terminal or synchronously invalid_state-idle — never
-- half-accepted). Gated to linux/macosx (POSIX syscalls).
sluice_internal_async_test("threadpool_backend_c2e_close_drain_test", {platform_gate = {"linux", "macosx"}})

-- fake_backend_c2e_close_drain_test — Phase C2e FakeAsyncBackend
-- admission-transaction deterministic test (Issue #68 rows 15-16; ADR
-- Decision 15 + §"Commit / accept" :453-462). The backend admission
-- transaction domain serializes close_admission() against an in-flight
-- submit's acceptance protocol: the fake's SubmitPauseGate pauses the submit
-- path AFTER the slot commit (Step 4) and BEFORE the `binding -> outstanding`
-- release-store (Step 5 — the commit/accept linearization point), INSIDE the
-- transaction; close_admission() must block there (a returned close would
-- permit a NEW acceptance LP after close). The resumed submit completes its
-- LP (submit wins), close returns after, the accepted request completes
-- normally, and a new submit after close rejects invalid_state. Mutant
-- M11-fake detector.
sluice_internal_async_test("fake_backend_c2e_close_drain_test")

-- async_io_context_split_wait_c2e_test — Phase C2e context-level detectors.
-- Case 1 (Issue #68 rows 15-16; mutant M12): the interrupted-branch final
-- poll. Drives the REAL AsyncIoContext::wait_one split-wait path (snapshot ->
-- poll -> wait_for_change -> interrupted -> ONE final poll) with a TEST-ONLY
-- split-wait backend + wait source (public AsyncBackend / BackendWaitSource
-- interfaces): wait_for_change() pauses AFTER observing a control interrupt
-- and BEFORE returning `interrupted`; the test records backend-ready in that
-- window; the context's interrupted-branch final poll is the ONLY path that
-- can reap it. Deleting that poll (mutant M12) makes wait_one return 0 —
-- deterministic RED on the L1 production context code.
--
-- Case 2 (round-3 P0, mutant D4-RM13): the inter-iteration control-wake
-- detector. The CONTROL baseline belongs to the whole external wait_one()
-- invocation, not one internal progress iteration. A test-only context-level
-- pause seam (AsyncIoContext::WaitSourceProgressPauseGate, compiled out of
-- production) parks the waiter in the exact window between wait_for_change()
-- returning `progress` and the next internal snapshot; the test fires
-- interrupt_all() there. Under the fix the invocation-level control baseline
-- is preserved and the waiter returns interrupted (0). Under the D4-RM13
-- mutant (control rebaselined per internal iteration) the fresh snapshot
-- absorbs the control bump, the stale event is drained, and the waiter
-- reparks forever (bounded watchdog -> RED).
sluice_internal_async_test("async_io_context_split_wait_c2e_test")

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

-- capacity_validity_test — Phase C2a capacity-case validity fixture (issue #68,
-- CORRECTION 6). A deliberately nonconforming capacity backend (guarded by
-- SLUICE_ASYNC_INTERNAL_TESTING; NOT registered in the conformance manifest)
-- proves run_capacity_cases() returns the SPECIFIC failing case name for each
-- violation: over_accept -> capacity_rejects_with_idle_completion,
-- bind_rejected -> capacity_rejects_with_idle_completion,
-- late_complete -> capacity_rejection_never_completes,
-- late_complete_after_drain -> capacity_rejection_never_completes,
-- misclassify_invalid -> capacity_stats_are_exact,
-- inflate_outstanding -> capacity_accepts_exact_limit,
-- no_recycle -> capacity_recycles_after_reset; the None control passes all.
-- Compiles the shared-suite implementation (backend_conformance_test.cpp)
-- alongside the validity cases so run_capacity_cases() resolves.
--
-- FAIL-FAST (review finding): both sources are fixed in-repo source files with
-- NO platform/feature condition. The old `if os.isfile(...) then` guard made
-- the target silently DISAPPEAR if either file were renamed/deleted, while
-- `xmake test` could still go green with validity coverage silently absent
-- (the validity target is not in the conformance manifest). Unconditional
-- registration + explicit raise() (xmake's sandbox exposes `raise`, not plain
-- `assert`) makes a missing source a hard xmake error instead of a silent
-- coverage drop.
do
    local validity = R .. "tests/capacity_validity_test.cpp"
    local impl = R .. "tests/backend_conformance_test.cpp"

    if not os.isfile(validity) then
        raise("capacity_validity_test source missing: " .. validity)
    end
    if not os.isfile(impl) then
        raise("capacity_validity_test shared-suite impl missing: " .. impl)
    end

    target("capacity_validity_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core", "sluice_async_internal_testing")
        add_includedirs(R .. "include", R .. "tests")
        add_files(validity, impl)
        add_tests("capacity_validity_test")
end
