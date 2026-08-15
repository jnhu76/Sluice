# Phase G Compliance Gate — backend-ready progress wake integration

**Phase:** G (final async-foundation phase; roadmap "backend-ready wake
integration")
**Design:** `docs/design/phase-g-backend-progress-wake.md`
**Status:** **COMPLETE (2026-08-15 closeout, branch
closeout-phase-g-foundation-freeze)** — G1 repaired by R1–R4 (§8.5 of the
design; deterministic reproducer GREEN), G2–G7 closed by the closeout
evidence: deterministic causal proofs (Cases A–D), the TP-G/UR-G race
matrices (real liburing, `mode=real`), mutation evidence M1–M3, the updated
`spec/tla/e9_park_wake` formal model (4 positive + 4 negative TLC gates), and
the full validation matrix below. P2-04 RESOLVED, DIV-04/DIV-05 amended,
ADR-execution-model §9.4.7.2 added. The async foundation is FROZEN
(`foundation-freeze.md`); no Phase H is planned.
**Authority:** ADR-execution-model §9.4.7/§9.4.7.1 (MIXED-WAKE; P5 seam
reserved); AGENTS.md §4.4/§10/§13.1/§13.2/§14; Constitution AC-6 (polling
justification), AC-7, AC-10, AC-13, AC-14, AC-15.

---

## Gate 0 — Architecture Classification

| Field | Decision |
|---|---|
| Affected capability | MW-S2 MIXED-WAKE progress park; Scheduler wake-domain bridge; backend wait-source bounded park; wake-domain park timeout policy |
| Affected layer | L1 AsyncIoContext (bounded wait_one + split-wait capability accessor); E7-E13 Scheduler (MW-S2 park domain, signal_wake_locked bridge, deadline hint); backend wait sources (bounded wait_for_change — ReadyWaitSource / UringWaitSource) |
| Classification | Corrective (closes the E9-reserved P5 seam; completes the roadmap's final wake-integration phase). **No new divergence:** DIV-04/DIV-05 reclassified per roadmap authorization ("the only phase that may reclassify DIV-04/DIV-05") |
| Governing ADR | ADR-execution-model §9.4.7.1 (bounded observation interval is protocol authority; P5 "reserved if P3 proves insufficient") — Phase G makes P3's MIXED-WAKE observation interval unnecessary for split-wait production backends |
| Conformance map change | No backend row reclassification (all four backends unchanged in contract); the reference backends' poll-driven classification is documented (G4) |
| Constitution rules | AC-6 (2ms re-justified: removed for production; retained only as the reference-backend observation interval), AC-7, AC-10, AC-13, AC-14, AC-15 |

### Zig conformance / divergence classification

- DIV-04 (decoupled wake domains — backend does not directly wake the
  Scheduler): **AMENDED** (Approved -> Approved with the Phase G bridge). The
  decoupling is preserved — the backend still never touches the Scheduler; the
  bridge runs through the backend-neutral `BackendWaitSource` interrupt seam
  that Phase D4 already defined. The "up to 2ms observation latency" cost term
  is removed for split-wait production backends.
- DIV-05 (2ms bounded observation interval as protocol authority):
  **AMENDED** (Approved -> Approved, scope narrowed). The interval remains
  protocol authority ONLY for poll-driven reference backends (Fake/Sync) whose
  readiness cannot self-notify; it is no longer a production-path authority.

---

## Gate 1 — Ownership and State Machine

### 1.1 MW-S2 park-domain decision (changed rule)

```text
MW-S2 participant COMMITTED (E7 two-phase admission, unchanged):
  split-wait backend (wait_source() != nullptr)
    -> park domain = BACKEND (ctx_.wait_one()), backend-only AND MIXED-WAKE
       (external wakes reach the park via the interrupt bridge)
  non-split-wait backend (reference/legacy)
    -> park domain = SCHEDULER when external_wake_possible_locked()
                     (MIXED-WAKE; bounded observation park — E9 behavior)
       park domain = BACKEND otherwise (unchanged)
```

| Transition | Authority | Lock domain | Alloc | Failure | Wake | Shutdown |
|---|---|---|---|---|---|---|
| MW-S2 commit -> arm_committed_wait | elected participant | G -> B (wait source arm) | none | n/a (arm is a registration) | control wakes after commit are observed by the next wait_one (D4-RM14) | request_stop -> interrupt observed (unchanged, now also MIXED) |
| park in wait_one (split-wait) | elected participant | B (wait source park) | none | wait_source failure -> fail-fast (UringWaitSource poll error) | backend progress epoch / ring fd; interrupt bridge (control epoch) | control interrupt (one-shot) |
| interrupted, no reap, external waits remain | participant (Phase D) | G (reclassify) | none | n/a | re-park (stay resident) — NEW rule | stop converges at MW-S3 via stop predicate |
| interrupted, no reap, backend-only | participant (Phase D) | G | none | n/a | terminate run (unchanged; driver re-entry) | unchanged |
| Scheduler wake publication -> bridge | signal_wake_locked (any producer) | W then B (leaf) | none | n/a | parked wait_one returns interrupted | control/stop semantics unchanged |
| park commit (wake domain) | parking worker | **G -> W (arm under state authority)** | none | n/a | **arm -> recheck protocol (R1): the epoch baseline is taken under `global_mtx_` AFTER the progress-predicate recheck; a publication in the arm window is either seen by the recheck or consumed by the baseline (Tokio `Notify::enable` discipline); a refused park signals an electable sleeper and re-loops** | unchanged |
| MW-S2 election | lowest-id ACTIVE worker | G | none | n/a | **transferable election (R2): the participant is the lowest-id active worker, not hardcoded worker 0; a worker that refuses (progress pending) signals an electable sleeper** | unchanged |
| worker terminate (loop exit) | exiting worker | **G -> I (inbox, released before signal)** | none | n/a | **terminate retire (R3): local runnables move to `pending_spawn_`, `live_loop_workers_` decrements, one unconditional `signal_wake_locked` AFTER the inbox lock is released (lock order stays G -> I then G -> W, never I -> W)** | stranded-runnable class eliminated |
| park predicate | parking worker | G -> W (arm under state authority; R1+R4 recheck in one G section) | none | n/a | **idle backstop (R4, commit-time, contribution-aware): a park is refused while `idle_workers_ > own_dance_contribution` — checked in the SAME `global_mtx_` section that arms the baseline, NOT in the cv predicate and NOT as a bare count (a predicate term or bare count observes the dancer's own contribution: the dancer's park becomes a no-op, each re-dance erases the count, and the not-last wake chain never damps — the Live-mw_s3 resident livelock; final form after the adversarial review); a counted dancer sleeps holding its count, a non-dancer refuses behind any live count, and the refusal signals an electable sleeper** | unchanged |

### 1.2 Lock-order table (AGENTS.md §13.1; new edges bold)

```text
G -> A        classify_locked / drain poll                         [unchanged]
G -> B        arm_backend_wait_commit (MW-S2 commit)               [unchanged; now also MIXED]
G -> W        signal_wake_locked                                   [unchanged]
G -> W -> B   signal_wake_locked -> interrupt_backend_waiters      [NEW]
G -> I        route_runnable_locked                                [unchanged]
A -> B        wait_one snapshot / poll / wait_for_change           [unchanged]
A -> L        backend reap / register / cancel                     [unchanged]
L -> (release) -> sink.on_ready                                    [arena contract]
B is a leaf: never acquires G, A, W, or L while held.
```

Cycle proof: B (backend wait source mutex) is a leaf with inbound edges only
from A and G (via arm and the new W edge); no wait-source path ever acquires a
Scheduler lock. W -> B is one-directional (signal_wake_locked acquires B only
after releasing nothing that B needs; B never acquires W). The eventfd write
under B (UringWaitSource) and the CV notify (ReadyWaitSource) are non-blocking
and call no Scheduler/user code.

### 1.3 The bridge is a notification, not a completion

`interrupt_backend_waiters` / `interrupt_all` semantics (unchanged from
Phase D4): bumps the control epoch under the wait-source mutex, then wakes
parked pollers. It never publishes a Completion, never touches a RequestSlot,
never records a terminal, never cancels I/O, never releases a borrow or slot,
and never routes a Fiber (G-I2). Reap remains the sole Completion-ready
publication boundary; the Scheduler drain remains the sole Fiber-routing
authority (G-I1).

---

## Gate 2 — Resource and Failure Model

| Resource | Capacity / allocation | Full behavior | Reclamation |
|---|---|---|---|
| BackendWaitSource bounded park | no new storage (epochs already exist) | wait_for_change(observed, max_park): ReadyWaitSource `cv.wait_for`; UringWaitSource poll timeout (clamped to INT_MAX ms) | none (no new container) |
| `backend_wait_active_` gate | one atomic bool | set at MW-S2 commit, cleared after wait_one returns; skip-bridge optimization only (epoch protocol is the authority) | n/a |
| interrupt path | **zero allocation** (mutex + epoch + notify/eventfd write) | never fails; never blocks | n/a |
| accepted terminal path | unchanged (allocation-free per Phase D/E proof) | the bridge adds no post-accept allocation dependency (G-I7) | n/a |

OOM: no new allocation site; the accepted -> terminal -> reap -> route path
remains allocation-independent (Phase E slice 12 / Phase D2 no-alloc evidence
re-run in Gate 4).

---

## Gate 3 — Wake / Progress / Shutdown Model

### 3.1 Progress notification protocol

```text
split-wait backend-ready publication (worker record_terminal / CQE reap)
  -> backend wait source progress epoch / ring fd        (prompt, per-backend)
  -> wait_one() returns progress                         (no polling)
  -> participant reaps -> ReadyRoutingSink -> drain -> Fiber

Scheduler wake publication (routing / flag / select / waitqueue / deadline /
wake handle / termination)
  -> signal_wake_locked (wake epoch + cv notify)
  -> interrupt_backend_waiters (control epoch + wake)    (THE BRIDGE)
  -> parked wait_one returns interrupted -> final poll -> re-drain -> re-park
```

- **Lost-wake closure:** the snapshot -> poll -> wait_for_change epoch
  protocol (existing); the D4-RM14 arm_committed_wait commit-to-park handshake
  (existing, now also MIXED); the D4-RM13 invocation-level control baseline
  (existing); the level-triggered ring/control fd (Uring, existing); and the
  G1-repair arm-recheck protocol (R1+R4) — the wake-domain park baseline is
  armed under `global_mtx_` after a single recheck that refuses BOTH on
  unguarded progress and on a live idle-dance count beyond the worker's own
  contribution (`idle_workers_ > idle_dance_contributed_`). Every refusal
  emits `signal_wake_locked` before re-looping, so a park refusal can never
  strand an electable sleeper; because the dancer's fetch_add + not-last
  signal serialize in the same domain as the recheck, a dance is either
  visible at the recheck (refuse) or advances the epoch past the baseline
  being recorded (predicate wake) — the
  notification-absorbed-by-baseline window (E9-LIFE-8) is closed with
  persistent state, while the counted dancer still sleeps holding its count
  (the damping the bare-count form destroyed — see Gate 1's R4 row).
- **Coalescing:** multiple backend-ready -> one or more epoch advances; the
  drain reaps all; 1:1 wake is never required (G-I3).
- **Progress vs control distinction:** progress epoch (backend readiness) and
  control epoch (interrupt) remain separate; a control wake never fabricates
  readiness and a progress wake never looks like shutdown (G-I5).

### 3.2 Shutdown convergence

```text
backend progress || runtime stop || close_admission || participant park
```

- request_stop -> interrupt_backend_waiters (direct, unchanged) + bridge
  wakes; participant re-drains, reaps remaining readiness, reaches MW-S3, and
  the stop predicate terminates the run; driver re-enters on the control
  epoch (unchanged).
- A worker whose loop exits (`mw_s2_no_progress_terminate` or stop) now
  retires before leaving (R3): its local runnables are handed to
  `pending_spawn_` (globally recoverable — the Runnable-ownership invariant
  of design §8.5), `live_loop_workers_` decrements under `global_mtx_`, and
  one unconditional wake is published. Idle-dance convergence compares
  against `live_loop_workers_`, so an exited participant no longer suppresses
  the survivor's convergence.
- The D4-RM14 armed baseline now also protects the MIXED commit-to-park
  window (previously the MIXED park had no backend arm; the stop-vs-commit
  race is closed for both domains).
- No busy-spin: the interrupted wait_one is one-shot per invocation; a future
  invocation snapshots the advanced control epoch and parks normally.

---

## Gate 4 — Evidence (executed commands)

Fill-in section — every row below is recorded from actually executed runs on
the implementation head.

| Evidence | Command | Result (executed 2026-08-14) |
|---|---|---|
| Production libraries | `xmake build sluice_core sluice_async` (Clang Debug) | build ok (guarded forensics excluded from production) |
| Internal-testing library | `xmake build sluice_async_internal_testing` | build ok |
| Clang Debug full suite | clean rebuild, 165 debug binaries, parallel (`xargs -P 16`, per-binary `timeout 300` — the §8 method; bare `xmake test` has no per-binary timeout and hangs forever on a stalled binary) | **165/165 PASS on the G1-repair head (2026-08-15)**, including the previously G1-stalling family (`sluice_copy_pipeline_stress_test`, `sluice_copy_pipeline_integration_test`, `application_runtime_drain_starvation_test`). On the pre-repair forensics head (2026-08-14): 162 PASS, non-PASS all G1-family (reproducer rc 70 EXPECTED RED, drain starvation rc 134, copy stress rc 124 timeout) — the fix flips all four |
| G1 deterministic reproducer (PR #108 review P2b; replaces the yield-ordered canary) | `SLUICE_TEST_FILTER=phase_g_g1_stranded_runnable_park_stall_reproducer`, per-binary `timeout 120`; construction holds run_impl at the run-entry seam (`worker_topology_ready_before_start`), submits the task while held, releases — the fiber is queued before any worker thread loops, so the run cannot converge before the task runs (a plain submit-after-start loses that race in Release builds) | pre-repair: 10/10 rc 70 on the original construction; on the seam construction, Release build pre-repair 5/5 rc 70 (5/5 the PRIMARY `stranded-runnable` dump) — post-repair (R1–R4): **5/5 GREEN (Release), 30/30 GREEN (Debug)**; the park-domain-pair pre-observation pins the interleaving before request_stop (role-based: participant/survivor by park_domain); any construction deviation fails closed EARLY with its own dump tag (uniform rc 70, no mid-stall teardown, caller resets the Completion before join) |
| G1 repair targeted stress | `application_runtime_drain_starvation_test` ×9 (6 under TSan), `sluice_copy_pipeline_stress_test` ×6, `sluice_copy_pipeline_integration_test` ×13 | all PASS on the G1-repair head (pre-repair: drain starvation rc 134 abort, copy stress rc 124 timeout — both timing-dependent G1 manifestations) |
| TSan full suite (AGENTS.md §16.3) | `xmake f -m tsan --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test`, then the §8 parallel method (re-run after the run-entry seam change) | **PASS on the final head (2026-08-15): 0 ThreadSanitizer reports, 0 non-zero binaries across two full parallel sweeps** — the gate itself found and forced the repair of one lock-order inversion introduced by R3 (`inbox -> wake` edge vs the predicate's `wake -> inbox` edge; fixed by narrowing the inbox critical section and signaling after release). One earlier sweep produced a single rc 134 from `threadpool_backend_scheme_b_race_test` under 16-way parallel TSan load; not reproducible across 2 clean full sweeps + 14 focused re-runs (sequential and 8-way concurrent), 0 TSan reports — recorded as a load flake of that binary's watchdog diagnostics, not a scheduler finding. Modified race classes covered per §16.3: submit/dequeue, enqueued cancel/dequeue, backend-ready/reap, wake signal/wait, reset/reuse, shutdown worker wake |
| Phase G regression | `SLUICE_TEST_FILTER=phase_g_quiescent_not_last_idle_signals_domain` ×30 | 30/30 PASS |
| Bounded-wait capability contract (PR #108 review P1b) | `SLUICE_TEST_FILTER=ctx_wait_one_bounded_cap_requires_capability xmake run async_io_context_split_wait_c2e_test` | PASS — a capability-less wait source receives a finite cap as synchronous `not_supported` (no park, no accounting), capability queries truthful, unbounded form unchanged |
| Adjacent sanity | `application_runtime_test`, `async_io_context_split_wait_c2e_test`, `async_stats_wait_race_test` | PASS |
| Clang Release full suite (AGENTS.md §16.1 — public headers changed) | `xmake f -m release --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test`, then the §8 parallel method (`xargs -P 16`, per-binary `timeout 300`); `sluice_copy_integration_test` exec's the `sluice-copy` CLI by a run-directory-relative path, so it must run with cwd = the build directory | **PASS (2026-08-15): all test binaries green** — `phase_g_backend_progress_wake_test` (both cases), `application_runtime_drain_starvation_test`, `sluice_copy_pipeline_stress_test`, `sluice_copy_pipeline_integration_test`, `sluice_copy_integration_test` individually re-verified; the initial sweep's two non-zero rows were runner artifacts (the `sluice-copy` CLI usage binary is not a test target; the copy-integration binary needs the build-dir cwd), and one real finding: the reproducer's submit-after-start placement lost the first-idle convergence race in Release (`no-participant-parked-pair` fail-closed) — fixed by the run-entry seam construction, then 5/5 pre-fix RED / 5/5 post-fix GREEN in Release |
| ASan/UBSan (AGENTS.md §16.2 — Fiber* ownership moves in the retire block) | `xmake f -m asanubsan --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test`, then the §8 parallel method (per-binary `timeout 420`) | **PASS (2026-08-15, the R4-redesign head): all binaries green, 0 AddressSanitizer reports, 0 UBSan `runtime error:` reports** |
| CI failure triage + adversarial review rework | pushed head e50731c CI run 31863170949: `sluice_copy_pipeline_integration_test` survived the 420s hang watchdog on the hosted runner (164 binaries concurrent; no local reproduction — pinned 2-core/1-core ×16, full-suite oversubscription ×3). The adversarial audit (coderabbit code-reviewer agent over 31a6ce6..e50731c) independently flagged R4's predicate term as P2: a counted dancer's park observes its own count → no-op park → re-dance erases the count → the not-last wake chain never damps (the resident-state livelock shape matching the CI stall) | R4 reworked to the contribution-aware commit check (see Gate 1/3 rows); review P2 #2 fixed (loop-top pending_spawn_ drain re-records fiber_owner_, removing the route-to-dead-worker hop); P3 #3 fixed (retire clears `active` under global_mtx_ with the live-count decrement); P3 doc items fixed (gate §3.1 overclaim, §8.5 R4 rewrite, stale inbox_cv comments, api-reference vtable-ABI note); P3 #4 documented in-code (drifted-ticket limitation). Re-verification on the rework head: reproducer 20/20 Debug, stress families 6/6 each, 2-core pinned ×10, full Debug suite 165/165, ASan/UBSan clean |
| Clang Debug full suite (liburing) — closeout | `xmake f -c -m debug --toolchain=clang --with-liburing=true -y && xmake build -g test`, then the §8 parallel method (`xargs -P 16`, per-binary `timeout 300`) | **167 binaries: 166/167 at -P 16; the single non-PASS is `threadpool_backend_scheme_b_race_test` rc 134 — its own watchdog aborting on a pre-existing test-gate protocol hole under parallel load (root-caused; see Known limits; production unchanged; standalone 6/6 green). Re-run at `-P 8`: 167/167 PASS.** |
| Clang Release full suite (closeout) | AGENTS §16.1 bare config + §8 method (`xargs -P 16`, per-binary `timeout 300`) | **PASS (2026-08-16): all 165 binaries green** (the two `if has_liburing`-gated targets are excluded by the bare config and covered by the Debug(liburing) run) |
| ASan+UBSan (closeout, AGENTS §16.2) | `xmake f -c -m asanubsan --toolchain=clang -y` (builds at `-j 12` after the mode churn), §8 method `-P 12`, per-binary `timeout 420` | **164/165 green, 0 AddressSanitizer reports, 0 UBSan `runtime error:` reports. The single non-PASS is `runnable_steal_test` (timeout) — a PRE-EXISTING ASan-timing probabilistic hang, reproduced on a fresh clean MASTER worktree (full-binary 4/10, case 3 3/10 standalone; no sanitizer report — it is a hang, not a detection); Debug/Release/TSan runs of the same binary are green; see Known limits.** |
| TSan full suite (closeout, AGENTS §16.3) | `xmake f -c -m tsan --toolchain=clang -y && xmake build -g test`, §8 method `-P 16`, per-binary `timeout 600` | **PASS (2026-08-16): all 165 binaries green, 0 ThreadSanitizer reports** (includes the modified race classes: park/bridge races via the closeout Case A–D and TP-G tests) |
| Real-liburing UR matrix (closeout) | `SLUICE_TEST_FILTER=phase_g_closeout_uring_*` on the Debug(liburing) build — every case prints an evidence-meta `mode=real\|stub` line and fails closed otherwise | **UR-G1..G7 GREEN, `mode=real` on all (kernel 7.1.8, liburing 2.14); stub-build classification case separately green on the stub config. M2U (uring armed-floor consume disabled) mutates RED.** |
| Mutation evidence (closeout) | M1 = bridge disabled in `signal_wake_locked`; M2 = R1 park-commit refuse removed; M3 = armed-floor consume disabled (`ready_wait_source.hpp`); M2U = uring-side armed-floor consume disabled | **All RED on the mutated build with the intended detector failures (Case A/B bridge-skipped, park-refuse, prepark-epoch detectors), GREEN after revert; focused reruns of the full closeout suites after each revert** |
| Formal models (closeout) | `python3 scripts/formal/verify.py check` + `bash scripts/formal/verify-e9-park-wake.sh` | **PASS: manifest check PASS; e9_park_wake 4 positive gates (split safety+liveness, reference safety+liveness) + 4 negative gates (bridge-disabled, no-refuse, reference-domain violations) all PASS** |
| Backend conformance (closeout) | `python3 scripts/verify-backend-conformance.py` on the Debug(liburing) build | **PASS** |
| Performance sanity (no fixed 2ms wake on split-wait) | wake-latency probe vs the Debug production libs: `ReadyWaitSource` parked `wait_for_change`, producer signals after 50ms, repeated ×3 | **transport overhead 170–184µs (park return ≈50.18ms after a 50.0ms scheduled signal) — far below any 2ms quantization; combined with the causal Case C / TP-G park tests and the `max_park` unbounded-sentinel derivation (no deadline, no ready-flag ⇒ `nanoseconds::max()`), the split-wait park is event-driven, not interval-driven** |
| Docs / architecture validation (closeout) | `python3 scripts/check-doc-links.py --self-test`, `python3 scripts/check-doc-links.py`, `python3 scripts/verify-architecture-docs.py`, `git diff --check` | **PASS (2026-08-16): self-test PASS; scan PASS (0 broken, 0 stale) after fixing backticked branch-name paths; architecture docs PASS; `git diff --check` clean** |

---

## Known limits / residual risk

- **G1 REPAIRED (was the primary risk)**: the unbounded wake-domain park's
  commit-to-sleep window is closed by R1–R4 (design §8.5): arm-after-recheck
  park commit, transferable lowest-active-id election, terminate-path retire
  to `pending_spawn_` + unconditional wake, and the `idle_workers_ > 0`
  predicate backstop. A timeout/cap re-arm was NOT used. Deterministic
  reproducer 28/28 GREEN; full Debug 165/165; TSan 0 reports.
- **Formal-model debt CLOSED (closeout)**: `spec/tla/e9_park_wake` now
  encodes the R1–R4 rules plus the split-wait bridge and control epochs
  (`bridgePending`, `workerAlive`, `terminateFlag`, domain-aware
  `LeaveParkEnabled`); 4 positive + 4 negative TLC gates PASS
  (`scripts/formal/verify-e9-park-wake.sh`; manifest counts updated). The
  pre-closeout coverage hole (a fixed-Drain Init making the Live properties
  vacuous) was found and fixed during the model update.
- **Pre-existing test-infrastructure finding (recorded, not fixed — issue
  candidate, NOT a Phase G regression)**: `tp_cancel_races_worker_terminal_
  exactly_one` (`tests/threadpool_backend_scheme_b_race_test.cpp`) can hang
  30s and abort its own watchdog under heavy parallel load (≈2/20 concurrent
  copies on an idle 20-core machine; bare `xmake test` / `-P 16` sweeps;
  standalone runs 6/6 green in ~4s). Root-caused by core dump + watchpoint
  trace: the test's `BeforeWorkerDequeuePauseGate` protocol synchronizes the
  worker's GATE EXIT (`exited=true`) but not the subsequent `pop_front`; a
  worker descheduled between the two can pop the NEXT iteration's dispatch
  entry without passing the pause gate (in cancel-win iterations the main
  thread's drain/reap/reset never waits for the worker, so `submit(N+1)`'s
  push can land before the deferred pop). The main thread then blocks in
  `wait_paused` forever while the stolen op completes normally
  (backend_ready, unreaped). Production code is UNCHANGED and behaves
  correctly throughout (legal state transitions, exactly-once terminal,
  correct accounting — verified in the trace); the file is identical to
  master. Repair belongs to the test harness (make the post-gate pop
  observable, or make `wait_paused` bounded with a steal-aware skip) and
  requires its own review — recorded as an application/load-triggered issue
  candidate under the freeze policy.
- **Pre-existing ASan-timing flake (recorded, not fixed — issue candidate, NOT a
  Phase G regression)**: `runnable_steal_test` hangs probabilistically under
  ASan (most often case `steal_steal_run_suspend_wake_resume_on_thief`,
  occasionally case 2; 40–60% standalone on this 20-core machine, effectively
  100% under a -P 12 ASan sweep; Debug/Release/TSan runs of the same binary
  are green). Reproduced on a fresh clean MASTER worktree (full-binary 4/10,
  case 3 3/10 standalone) — pre-existing and load/ASan-timing dependent, with
  no sanitizer report (it is a hang, not a detection). The closeout branch
  changes no production source, and a clean-dir rebuild reproduces it at the
  same rate (build corruption and ccache were ruled out: fresh-directory
  rebuild behaves identically; ccache-served objects verified byte-identical
  to direct compiles). Root-cause capture is pending (the hang window is
  bursty; 0/9 captures in low-load windows) — recorded as a post-freeze issue
  candidate requiring its own gated investigation.
- Reference backends (Fake/Sync) keep the bounded Scheduler-domain
  observation interval in MIXED-WAKE (their readiness is poll-driven and
  cannot self-notify). This is a documented reference-backend classification
  (G4), not a production path.
- The bridge adds one atomic load to every Scheduler wake publication when no
  backend participant is parked, and a control-epoch bump + notify when one
  is parked (the prompt-wake cost that replaces the 2ms observation interval).
