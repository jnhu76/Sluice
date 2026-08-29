# Issue #116 — Invocation-Boundary Lost Re-entry Liveness: Compliance Gate

**Change class:** post-freeze application-derived correctness fix (Issue #116).
**Baseline master SHA:** `ff003fd8f266eb561e9f4f9062bb73ac71e81ff8` (post #117).
**Generic gate:** `docs/architecture/design-compliance-gate.md` (this document
covers every Gate 0–4 field for the fix and links back).

**One-line defect:** `Scheduler::run_live()` legitimately returns at an
interrupted MW-S2 no-progress boundary while accepted I/O is still
outstanding (the caller-owned re-entry contract, `phase_g_closeout_tp_g5`);
`ApplicationRuntime`'s driver re-enters only on a `control_epoch_` change,
and that epoch cannot observe the transferred obligation — so the driver
parks forever while the backend's terminal is recorded into a ready-ring
nobody reaps.

---

## Gate 0 — Architecture Classification

```text
Affected capability:    Runtime (driver re-entry predicate); Scheduler is UNCHANGED
Affected layer:         E16 ApplicationRuntime driver_main between-invocations section
Classification:         Corrective (evidence-derived; restores AC-4/AC-6 at the runtime layer)
Governing ADR:          ADR-application-runtime.md (driver re-entry loop, P1-07 control epoch);
                        ADR-execution-model.md §9.2.6 (MW-S2 classification) and §9.4.0
                        (LIVE vs DRAIN return contract) — both UNCHANGED by this fix
Conformance map change: no (no state machine, authority, or publication change;
                        one caller-side park predicate narrowed)
Constitution rules:     AC-4 (accepted operation must terminate, reapable — the
                        caller must keep an observer alive),
                        AC-6 (explicit wake obligation — an interrupt is a
                        re-evaluation signal, not a quiescence proof)
```

## Root cause (proven; see docs/history/issues/issue-116-runtime-reentry-liveness.md)

Failing interleaving (single-worker runtime, ThreadPoolBackend, diagnosed by
in-process diagnostic trace + gdb state dump on a live-hung process):

```text
main:   start()                       control_epoch_ -> 1
driver: run_live#1 quiescent return   (empty system) -> parks between_invocations
main:   submit(task)                  control_epoch_ -> 2; runtime_cv notify;
        wake_handle_.notify() ─────────┐ (thread descheduled between the two notifies)
driver: wakes on epoch 2, re-enters   observed_epoch_ := 2   [epoch delta consumed]
        run_live#2: runs task; op#1 accepted (outstanding=1); fiber awaits;
        MW-S2 elect -> commit (arm) -> ctx_.wait_one() parks
main:   └─────────────────────────────> wake_handle notify lands HERE
        -> Scheduler::signal_wake_locked -> bridge (backend_wait_active_)
        -> interrupt_all (wait-source control epoch +1)
driver: wait_one returns 0 (interrupted; final poll empty — op#1 still running)
        made_progress == false
        classify == mw_s2 (outstanding == 1), external_wake_possible == false
        -> MW-S2 NO-PROGRESS TERMINATE: global_terminate_ = true; run_live#2 returns
driver: between_invocations: control_epoch_(2) == observed_epoch_(2)
        -> parks on runtime_cv_ FOREVER (no submit/stop/terminal will ever come)
backend worker (later): finishes op#1 -> record_terminal (backend_ready=1)
        -> signal_ready_progress -> NO OBSERVER -> Completion never reaped
task:   never resumes -> never publishes -> done_cv never fires -> PERMANENT HANG
```

Captured state at hang (gdb, race-free by full-thread SIGSTOP):

```text
control_epoch_==observed_epoch_==2, admitted=1 terminal=0, driver=between_invocations
scheduler: global_terminate=1, active_worker_count=0, live_loop_workers=0,
           pending_spawn=0, all legacy wait containers empty, wake_epoch=6
registry:  wait_record_live_count=1, rec state=registered, fiber state=waiting
arena:     slot_in_use=1 accepted_outstanding=1 backend_ready=1
ready source: ready_epoch=1 control_epoch=1 armed=0 ; dispatch=0 (high_water=1)
```

Violated invariants: INV-R2 (safe `run_live` return — the obligation transfer
is contractual, the caller failed to discharge it), INV-R3 (Runtime park
handshake — `control_epoch_` is not a complete encoding of Scheduler-side
progress obligations), and the brief's H4 rule "never equate an interrupt
with quiescence". Classification: **H4 mechanism (backend progress exists,
no Scheduler participant remains) with H5 consequence (Runtime cannot
observe it)**.

## Gate 1 — Ownership and State Machine

No new lifecycle object; no Scheduler change. The modified transition:

```text
ApplicationRuntime driver, between_invocations, after the drain branch and
the epoch check, before the runtime_cv_ park:

  PRE-FIX:
    [control_epoch_ == observed_epoch_] -> park on runtime_cv_ (predicate:
        exit/fatal/epoch) — parks even when io_ctx_->outstanding() > 0,
        stranding the TP-G5-transferred observation obligation

  POST-FIX:
    [control_epoch_ == observed_epoch_]
      -> if io_ctx_->outstanding() > 0:
           re-enter run_live immediately (driver_state_ = in_run_live)
      -> else: park exactly as before

  Authority:      ApplicationRuntime driver thread under lifecycle_mtx_ (unchanged)
  Lock domain:    lifecycle_mtx_ for the decision; the re-entered invocation's
                  MW-S2 commit/park follows the unchanged Scheduler protocol
                  (global_mtx_ / D4-RM14 arm / D4-RM13 invocation baseline)
  Allocation:     none (predicate only)
  Failure:        none reachable (outstanding() is a lock-guarded read)
  Wake:           the re-entered invocation re-arms via arm_committed_wait at
                  its Phase-B commit — commit-to-park window closed by the
                  existing D4-RM14 handshake; interrupt is one-shot per
                  invocation (D4-RM13), so each re-entry consumes at most one
                  control event and the next park is unbounded and event-driven
  Shutdown:       request_stop -> interrupt -> (possible) terminate -> driver
                  re-enters (stop bumped the epoch; now also via outstanding>0)
                  -> parks until the accepted I/O completes (AC-4: real
                  syscalls terminate) -> quiescent -> E14-F1 stop predicate
                  ends the run -> drain/join converge — same outcome as
                  pre-fix, without the stranded window

  Park-handshake closure (INV-R3): the check runs under lifecycle_mtx_, the
  same domain that guards the park. While outstanding == 0, a NEW obligation
  can appear only via submit/start/stop/terminal publication — every one of
  which bumps control_epoch_ under this mutex and wakes the parked driver.
  Backend-side progress cannot create a Runtime-visible obligation from zero
  (it requires an accepted op; accepting is a submit). No check-to-park
  window remains.
```

Why the caller side and not the Scheduler side: the interrupted no-progress
terminate-and-return is an ACCEPTED, TESTED Scheduler contract
(`phase_g_closeout_tp_g5_close_admission_while_parked`: "the interrupted
no-progress park terminates the run; a parked-forever run is the failure" —
the caller re-enters and reaps). A Scheduler-side re-park (implemented and
evaluated first) breaks that contract and four Phase-G/uring closeout tests.
The runtime driver was the component that failed to honor the transferred
obligation; `AsyncIoContext::outstanding()` — already public and already the
obligation term inside the drain-complete predicate — is the correct
persistent-state check. No new API, no Scheduler internals exposed.

## Gate 2 — Resource and Failure Model

Unchanged by this fix (predicate only). For completeness:

- No new container, thread, queue, or allocation.
- Busy-spin analysis: each re-entry consumes one control-plane interrupt
  (D4-RM13 one-shot invocation baseline; D4-RM14 armed commit); between
  interrupts the driver is parked inside `ctx_.wait_one()` (unbounded,
  event-driven). There is no time-driven retry.
- Non-split-wait backends: unchanged behavior — the fix lives entirely in
  the runtime driver; it is backend-agnostic for backends conforming to the
  accepted AsyncBackend outstanding/reap lifecycle contract (the re-entered
  MW-S2 park domain selection keeps its existing rules).

## Gate 3 — Progress and Wake Model

```text
Blocking/suspension:
  Who may block?      the driver thread — either parked on runtime_cv_ (only
                      when outstanding == 0 and no epoch delta), or inside
                      ctx_.wait_one() as the re-elected MW-S2 participant
  Who may suspend?    Fibers (unchanged)
  What continues them? backend progress signal (ready epoch) or control
                      interrupt (control epoch) — persistent-state epochs
                      (ReadyWaitSource), closed commit-to-sleep windows

Backend -> Scheduler progress:
  Mechanism:          signal-based (ReadyWaitSource epochs + cv), not polling
  Worst-case latency: one interrupt-to-re-entry round trip per control event;
                      otherwise immediate on record_terminal's signal_progress

External wake coexistence:
  The bridge (signal_wake_locked -> interrupt_backend_waiters) still reaches
  a parked participant. The interrupt's post-fix meaning at the RUNTIME
  layer: a re-evaluation that may cost one invocation round trip, never a
  permanent park — the driver re-enters while the obligation (outstanding>0)
  exists, and the fresh invocation re-arms with a post-interrupt baseline.

Polling dependency: none introduced (none removed; the defect was a
  mis-classified interrupt, not a missing poll).

Single-worker liveness: the failing configuration itself (workers=1): the
  driver stays either inside a live invocation's wait_one or re-enters
  immediately; it never parks between invocations with an obligation.
```

## Gate 4 — Evidence Plan (executed; results in the investigation doc §10/§11/§14)

```text
Deterministic causal test:
  - issue116_interrupt_is_reevaluation_not_quiescence (internal-testing,
    IN the default merge gate):
    startup ordering forced via the run_impl tail seam; op frozen at the
    backend running gate; participant park proven by the wait-source
    park-entry latch (atomic wait/notify rendezvous, hard timeout as
    escape hatch only); fatal component = test-owned SchedulerWakeHandle
    notify (no control_epoch_ advance).
      Pre-fix:  8/8 fail-closed (exit 70 + forensics dump reproducing the
                captured hang state exactly).
      Post-fix: 8/8 pass with clean drain/join teardown.
  - issue116_liveness_forensics_test: probabilistic starvation stress with
    race-free state dump (Phase 2 tooling; 40 fresh runtimes per run).
    EXPLICITLY OUT of the default `xmake test` gate — manual forensic path
    (investigation §3/§11 recipe) + nightly hardening soak
    (scripts/hardening/phases.py); the in-process 20 s watchdog exits 42
    with the state dump on a stall.

Backend conformance: ThreadPoolBackend (observed trigger; full suite green);
  Sync/Fake regression via the full async suite (166 → 167 default-gate
  tests; see the mechanically verified row below).
```

Default-gate test target count (Linux Clang Debug stub build; derived
mechanically by `scripts/gates/mechanical-facts.py` from the xmake lua
registrations and equal to the `running.test` line count of
`xmake test -v`):

| `test:default-gate-targets` | 199 | (168 at issue-116 closeout + 12 file-tools application-track tests on 2026-08-18: sluice-copy safe-output, 4x sluice-hash, 4x sluice-grep, 3x sluice-tail + 1 performance-attribution `sluice_grep_matcher_differential_test`; live cross-reference — this row tracks the current mechanical count, see `scripts/gates/mechanical-facts.py`; 181 → 183 on 2026-08-19, the #135 Case B round added `failure_model_high_risk_test` + `failure_model_high_risk_death_test`; 183 → 184 on 2026-08-20, ADR-async-primitive-lifetime-failfast added `async_sync_lifetime_death_test`; 184 → 186 on 2026-08-20, the C7 runtime-await round added `runtime_await_helpers_test` + `task_result_submit_throw_test`; 186 → 187 on 2026-08-20, the #143 close-contract round added `file_close_test`); 187 → 188 on 2026-08-21, the #161 idle-dance orphan round added `issue161_idle_dance_orphan_test`; 188 → 189 on 2026-08-21, the #161 split-window round added `issue161_pub_erase_orphan_test; 189 → 190 on 2026-08-24, the #196 E9 trace-conformance pilot added `e9_trace_conformance_test`; 190 → 191 on 2026-08-27, the AC-1a observability round added `threadpool_resource_observability_test`; 191 → 192 on 2026-08-27, the AC-2b ordinary-deadline-authority round added `ordinary_deadline_authority_test`; 192 → 193 on 2026-08-28, the DST-PV-1 proof-of-value round added `dst_pv1_schedule_driver_test`; 193 → 194 on 2026-08-29, the FE-2 stackless-frontend proof-of-value round added `fe2_stackless_event_pov_test`; 194 → 195 on 2026-08-28, the FE-3 Queue vertical-slice round added `fe3_stackless_queue_slice_test`; 195 → 196 on 2026-08-28, the FE-3 RwLock vertical-slice round added `fe3_stackless_rwlock_slice_test`; 196 → 197 on 2026-08-28, the FE-3 Condition vertical-slice round added `fe3_stackless_condition_slice_test`; 197 → 198 on 2026-08-28, the FE-3 cross-frontend mixing round added `fe3_cross_frontend_mixing_test`; 198 → 199 on 2026-08-29, the FE-CORRECTIVE-1 publication-atomicity round added `fe2_publication_atomicity_death_test`) |

```text
Sanitizers: TSan full group — pass, 0 warnings (a test-side seam-disarm
  ordering bug was caught by TSan and fixed: disarm backend gates before
  join()). ASan+UBSan full group — pass, 0 errors. Race classes exercised:
  driver re-entry vs submit/stop publication (lifecycle_mtx_ domain);
  re-park vs interrupt (D4-RM13/RM14).

Normal gates: Clang Debug 167/167; Clang Release 167/167 (5/5 isolated
  runs; one first-run flake under deliberate concurrent CPU saturation —
  see investigation §14); stress before/after 3/40 hangs -> 0/120.
```

## Zig conformance / divergence

No Zig model change: the defect and fix live in the C++ ApplicationRuntime
driver's re-entry predicate, below any Zig-modeled protocol. No new
divergence; the divergence registry gains no row (corrective, restores
constitution conformance).

## Exit-path audit (Phase 6 of the investigation)

See `docs/history/issues/issue-116-runtime-reentry-liveness.md` §12 for the
full table over: mw_s1 terminate observed, mw_s2 no-progress (this fix),
E14/F1 stop-predicate, last-idle, final park, worker epilogue,
all-workers-joined boundary, run_live return.
