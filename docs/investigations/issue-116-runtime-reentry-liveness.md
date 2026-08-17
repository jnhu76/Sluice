# Issue #116 — Runtime/Scheduler Lost Re-entry Liveness: Root-Cause Investigation

**Status:** ROOT_CAUSE_PROVEN_AND_FIXED
**Baseline master SHA:** `ff003fd8f266eb561e9f4f9062bb73ac71e81ff8` (post-#117)
**Fix branch:** `fix/issue-116-runtime-reentry-liveness`
**Compliance gate:** `docs/architecture/issue-116-reentry-liveness-gate.md`
**Classification:** post-freeze application-derived correctness fix (Issue #116)

---

## 1. Executive summary

`Scheduler::run_live()` legitimately returns at an interrupted MW-S2
no-progress boundary while accepted I/O is still outstanding — the
caller-owned re-entry contract tested by `phase_g_closeout_tp_g5`. The
`ApplicationRuntime` driver failed its half of that contract: it re-enters
`run_live` only on a `control_epoch_` change, and `control_epoch_` cannot
observe Scheduler-side/backend-side progress. When the control event that
carried the interrupt had already been consumed as the driver's re-entry
baseline, the driver parked on `runtime_cv_` forever with a stranded
obligation: the backend's terminal was later recorded into a ready-ring with
no observer, the suspended task never resumed, and the application hung
permanently.

**Repair:** `driver_main()` now re-enters `run_live` immediately instead of
parking whenever `io_ctx_->outstanding() > 0` (a public context-level
predicate; no new API, no Scheduler internals exposed). The re-entered
invocation re-elects the MW-S2 participant and parks in `ctx_.wait_one()` —
a true, event-driven park — so the fix introduces no polling and no busy
spin.

## 2. Historical CI evidence

- Issue #116: copy-pipeline completion lost-wake window under extreme
  starvation (the primary report; CI `sluice_copy_pipeline_integration_test`
  watchdog kills).
- Issue #67: the earlier copy-pipeline CI hang family (D3/D4 remediations:
  split-wait, bridge, D4-RM13/RM14 baselines — all correct individually;
  #116 is the residual composition hole).
- Issue #117: watchdog survivor forensics (merged; assumed as baseline).

## 3. Local stress reproduction (Phase 1)

Recipe: 4-CPU affinity with 3 CPU spinners (`taskset -c 0-3` +
`bash -c 'while true; do :; done'` on CPUs 0–2), repeated bounded-watchdog
executions of `build/linux/x86_64/debug/sluice_copy_pipeline_integration_test`
(Clang Debug, 20 s per-run bound).

| Metric | Value |
| --- | --- |
| Total executions | 40 |
| Passes | 37 |
| Hangs | **3 (7.5%)** |
| Failing cases | `pipeline_integration_edge_sizes_per_depth`, `pipeline_integration_sync_policies` (all workers=1) |
| Normal case time | milliseconds (whole binary ≈1–2 s) |
| Topology | 20-CPU host, process pinned to CPUs 0–3 shared with 3 spinners |
| Worker count | 1 (Scheduler single-worker inline path on the driver thread) |
| Pipeline depth | 1–8; buffer 4096 (edge cases) and 1–65536 (buffer matrix) |
| File sizes | edge-size matrix, 0 .. 3·depth·B+7 |
| Build | Clang Debug, master `ff003fd` |

A hang is a permanent park until SIGKILL, not slowness: thread stacks show
the main thread in `done_cv.wait` (`apps/sluice-copy/copy_task.cpp:540`),
the driver in `runtime_cv_.wait` (`src/async/application_runtime.cpp:713`),
and 2–4 ThreadPoolBackend workers idle in `work_cv_.wait`.

## 4. State capture at the hang (Phase 2)

gdb on a SIGSTOP-preserved live hang (all threads frozen — race-free),
plus an in-process internal-testing dump facility added for this
investigation (`ApplicationRuntime::test_dump_forensics`, guarded by
`SLUICE_ASYNC_INTERNAL_TESTING`; the probabilistic reproducer
`tests/issue116_liveness_forensics_test.cpp` carries a 20 s watchdog that
dumps the same state and exits 42).

```text
ApplicationRuntime: state=Running driver=between_invocations
                    control_epoch=2 observed_epoch=2     ← EQUAL
                    admitted=1 terminal=0 drain_complete=0 stop=0
Scheduler:          active_worker_count=0 running_fibers=0 live_loop_workers=0
                    global_terminate=1 in_coordinated_run=0 admission=none
                    pending_spawn=0 all legacy wait containers empty
Wait registry:      wait_record_live_count=1
                    rec[0] state=registered fiber=... fiber_state=waiting
                    wait_delivered_head=nil
Arena/backend:      slot_in_use=1 accepted_outstanding=1 backend_ready=1
                    ready_wait: ready_epoch=1 control_epoch=1 armed=0
                    dispatch=0 (high_water=1)
```

Readings:

- The task fiber is **Waiting** on an identity Completion wait that was
  **registered but never delivered** — its op was accepted and its terminal
  was **recorded (backend_ready=1) but never reaped**.
- The coordinated run ended via a **global_terminate path**
  (`global_terminate=1`); with `outstanding=1` the only reachable
  terminator is the **MW-S2 no-progress terminate**.
- The wait source saw exactly one `interrupt_all` (control_epoch=1) and one
  `signal_progress` (ready_epoch=1, the stranded op's terminal).
- The Runtime epochs are equal — the driver's park predicate can never fire.

## 5. Exact violating interleaving (Phase 2/3, diagnostic trace)

A throwaway instrumented build (bridge interrupt / wait_one-return /
no-progress-terminate / runtime notify / driver park trace; reverted after
diagnosis) captured the full causal chain with thread IDs and epochs:

```text
main  : start()                                  control_epoch_ → 1
driver: run_live#1 (empty) → quiescent → return  observed_epoch_ := 1, parks
main  : submit(task)                             control_epoch_ → 2;
        runtime_cv_.notify_all(); wake_handle_.notify()
driver: wakes on epoch delta → re-enters         observed_epoch_ := 2  ← CONSUMED
        run_live#2: runs task; op#1 accepted (outstanding=1); fiber awaits;
        MW-S2 elect → commit (arm) → parks in ctx_.wait_one()
main  : (descheduled between its two notifies) wake_handle_.notify() lands HERE
        → Scheduler::signal_wake_locked → bridge (backend_wait_active_=true)
        → ReadyWaitSource::interrupt_all (control epoch 0→1)
driver: wait_one returns 0 (interrupted; final poll empty — op#1 mid-flight)
        made_progress=false → classify=mw_s2(outstanding=1), extwake=false
        → MW-S2 NO-PROGRESS TERMINATE (global_terminate_=true) → run_live#2 returns
driver: between_invocations: control_epoch_(2) == observed_epoch_(2)
        → parks on runtime_cv_ FOREVER
backend worker (later): finishes op#1 → record_terminal (backend_ready=1)
        → signal_ready_progress (ready_epoch 0→1) → NO OBSERVER
task  : never resumes → never publishes → done_cv never fires → PERMANENT HANG
```

Diagnostic trace (exact lines from the captured run):

```text
[DIAG] driver PARK epoch=1 observed=1
[DIAG] submit notify tid=MAIN
[DIAG] bridge interrupt from retaddr=notify_external_wake tid=MAIN
[DIAG] wait_one interrupted-return 0: outstanding=1 tid=DRIVER
[DIAG] MW-S2 NO-PROGRESS TERMINATE: outstanding=1 extwake=0 tid=DRIVER
[DIAG] driver PARK epoch=2 observed=2
```

## 6. Stranded-state classification (Phase 3)

**H4 (primary mechanism) + H5 (consequence).**

- **H4 — backend progress exists but no Scheduler participant remains**:
  at the park, `outstanding=1` with the single elected participant as the
  only possible reaper; the MW-S2 no-progress terminate removed it. The
  brief's rule "never equate an interrupt with quiescence" is violated by
  the *caller's* handling of the (contractual) terminate.
- **H5 — Scheduler work exists but the Runtime cannot observe it**: the
  obligation is invisible to `control_epoch_`, so the driver's park
  predicate is permanently false.
- H1 (runnable on dead owner), H2 (pending_spawn at return), H3 (delivered
  waiter undrained) were investigated and are **structurally closed** at the
  invocation boundary for the runtime topology: every reap site drains and
  routes under `global_mtx_` before any terminating classify; worker
  epilogues rescue queues under the same domain; `active_worker_count_`
  stays > 0 until `run_impl`'s tail, after which no runtime-context routing
  thread exists. (See §12 for residual theoretical windows outside the
  application runtime's reachable interleavings.)

## 7. Violated invariants

- **INV-R2 (safe run_live return):** `run_live` returned while it still
  owned an immediate progress obligation (accepted outstanding I/O with no
  observer). The return itself is the Accepted TP-G5 contract; the violation
  was the caller's failure to honor the transferred obligation.
- **INV-R3 (runtime park handshake):** the driver parked without proving
  "no Scheduler re-entry obligation"; `control_epoch_` is not a complete
  encoding of Scheduler-side progress.
- **AC-4** (accepted operation must reach a reapable terminal): the
  terminal was recorded but publication (reap) depended on an observer that
  no longer existed.
- **AC-6** (explicit wake obligation): the interrupt that legitimately
  forces participant re-evaluation was, at the runtime layer, allowed to
  masquerade as quiescence.

## 8. Root cause

`ApplicationRuntime::driver_main()`'s re-entry predicate is
`control_epoch_ != observed_epoch_`. Every producer that can interrupt a
parked MW-S2 participant also advances a Scheduler wake publication — but
NOT every wake publication advances `control_epoch_` (Scheduler-internal
signals), and, decisively, the one control event that does (submit) can have
its epoch delta consumed as the very re-entry baseline that precedes the
parked invocation. After the MW-S2 no-progress terminate transfers the
observation obligation to the caller, no remaining signal can wake the
driver: the obligation is stranded between the two layers.

## 9. Chosen repair (Phase 5)

`src/async/application_runtime.cpp`, `driver_main()`, between-invocations
section (after the drain branch and the epoch check, before the park):

```cpp
if (io_ctx_ && io_ctx_->outstanding() > 0) {
    driver_state_ = DriverState::in_run_live;
    lk.unlock();
    continue;   // re-enter run_live; the invocation re-elects the MW-S2
                // participant and parks in ctx_.wait_one() — an event-driven
                // park with its own closed wake protocol
}
```

Why this shape:

- **Smallest change at the correct authority.** The obligation is the
  caller's by contract (TP-G5); the caller now discharges it. No Scheduler
  state is exposed; no public API changes; no new internal API.
- **No polling, no busy spin.** Each re-entry consumes one control-plane
  interrupt (D4-RM13 one-shot invocation baseline; D4-RM14 armed commit).
  Between interrupts the driver is parked inside `wait_one`, unbounded.
- **Park handshake closure (INV-R3).** The check runs under
  `lifecycle_mtx_` — the same domain that guards the park. While
  `outstanding == 0`, a new obligation can appear only via
  submit/start/stop/terminal publication, each of which bumps
  `control_epoch_` under this mutex and wakes the parked driver.
  Backend-side progress cannot create a Runtime-visible obligation from
  zero (it requires an accepted op, and accepting is a submit). No
  check-to-park window remains.
- **Convergence preserved.** `request_stop` → interrupt → terminate →
  driver re-enters (stop bumped the epoch; now also via outstanding>0) →
  parks until the accepted I/O completes (AC-4: real syscalls terminate) →
  quiescent → E14-F1 stop predicate ends the run → drain completes.

### Alternative repairs considered and rejected

- **Candidate A (Scheduler-side re-park while outstanding>0, live+split-wait
  only):** implemented first; correctly closes the hole but **changes the
  Accepted TP-G5 contract** ("the interrupted no-progress park terminates
  the run") — 4 Phase-G/uring closeout tests fail by design. Rejected as a
  larger semantic change than the evidence requires; the contract is sound,
  the runtime consumer was not.
- **Candidate B (never route to a dead owner):** addresses H1, which is not
  the proven strand; no effect on this defect.
- **Candidate C full form (RunOutcome/reentry_generation API):** unnecessary
  once `AsyncIoContext::outstanding()` — already public, already the
  drain-complete predicate's own obligation term — serves as the persistent
  obligation state.

## 10. Deterministic regression proof (Phase 4)

`tests/issue116_interrupt_reevaluation_regression_test.cpp`
(`issue116_interrupt_is_reevaluation_not_quiescence`), internal-testing
target. Construction (no sleep-ordering; persistent-state rendezvous only):

1. The first, empty `run_live` is held at the `run_impl` tail seam
   (`worker_topology_joined_before_unpublish`) and released only after it
   has provably returned — forcing the fatal startup ordering (the ~7.5%
   CI race condensed to one schedule).
2. The task submits one read gated by the backend `WorkerRunningPauseGate`
   (op frozen in `running`, terminal absent; `outstanding` stable at 1).
3. The wait-source park-entry latch proves the MW-S2 participant is inside
   the (unbounded) backend park.
4. A test-owned `SchedulerWakeHandle::notify()` delivers the fatal
   component: a Scheduler wake publication carrying no `control_epoch_`
   advance.
5. The gate is released; the test asserts the task completes.

| | Result |
| --- | --- |
| Pre-fix (fix hunk removed, same tree) | **8/8 fail-closed** (exit 70) with the full forensics dump reproducing the captured hang state byte-for-byte (`epoch==observed==2`, `between_invocations`, `outstanding=1`, `mw_s2_no_progress_terminate`, terminal recorded with no observer) |
| Post-fix | **8/8 pass** (5 + 3 runs), clean teardown (drain + join + backend destructor quiescence fail-fast all pass) |

## 11. Historical stress reproducer, before/after

Same recipe as §3 (taskset 0–3 + 3 spinners, 20 s per-run bound):

| | Runs | Hangs |
| --- | --- | --- |
| Before (master `ff003fd`) | 40 | **3 (7.5%)** |
| After (fix) | 120 (60 + 60) | **0** |

## 12. Exit-path audit (Phase 6)

| Exit path | Persistent state checked? | Runnable rescue? | Outstanding I/O? | Delivered waiter? | Wake/re-entry transferred? |
| --- | --- | --- | --- | --- | --- |
| mw_s1 terminate observed | reads `global_terminate_` (atomic) | n/a (no local work by construction: classify saw mw_s1 elsewhere) | possible (set by below rows) | impossible (drained under G before any terminate classify) | epilogue signal + driver obligation check (this fix) |
| **mw_s2 no-progress terminate** | classify under G (mw_s2/mw_s3/quiescent) + final poll | tickets → mw_s1 → continue | **yes — the #116 obligation** | no (Phase-D drain ran first) | **TP-G5 contract → caller; driver now honors via `outstanding()>0` re-entry (this fix)** |
| E14/F1 stop-predicate terminate | stop predicate (lock-free snapshots) + final classify under G | as last-idle | possible if tasks submitted-then-returned without awaiting | no | driver re-entry on stop's epoch bump + outstanding check |
| last-idle terminate | final classify under G (quiescent/mw_s3 only) | yes (mw_s1 → re-loop) | no (quiescent ⟹ outstanding==0) | no (identity records invisible to quiescent ⟹ outstanding>0 ⟹ not quiescent; delivered drained under G) | caller; no obligation by construction |
| final park terminate | reads `global_terminate_` | as above | follows the setter's row | — | as above |
| worker epilogue | under G: `--live_loop_workers_`, `active=false`, inbox→`pending_spawn_`, signal | **yes (queue rescue)** | n/a | n/a | signal + next invocation |
| all-workers-joined boundary | `active_worker_count_.store(0)` under G | pending_spawn_ persists | n/a | n/a | next invocation (driver-gated; this fix covers outstanding obligations) |
| run_live return | — | — | — | — | **driver: epoch check ∪ outstanding>0 re-entry (this fix); exit/fatal checks unchanged** |

Residual theoretical windows (documented, not repaired — outside the
application runtime's reachable interleavings, per §6): a runnable ticket
routed onto a worker that already passed its epilogue requires an active
routing thread after the last worker's exit, which the runtime topology
does not produce (all fibers are suspended at that point; external
`cancel_waiter` is not exercised by any runtime caller today). This is the
#115 family's territory — see §13.

## 13. Relationship to issue #115

**RELATED BUT DISTINCT.**

- #115: a *live* invocation's parked wake-domain worker never re-scans
  victim queues, and `spawn_on` notifies only the target inbox (no wake
  epoch) — strands a **runnable** fiber on a busy owner's queue. Repair
  class: Scheduler wake-protocol change (frozen; deferred to application
  evidence per its disposition).
- #116 (this): a *returned* invocation's caller parked without discharging
  the transferred observation obligation — strands a **waiting** fiber and
  an **un-reaped backend-ready terminal**. Repair class: caller-side
  re-entry (this patch; freeze-compatible).

The #116 fix does not close #115 (single-worker topology; no `spawn_on`);
#115 remains open with its existing disposition. Noteworthy symmetry: the
E9 wake-handle notify that #115 lists as a live escape hatch is exactly the
publication whose bridge interrupt was the fatal component here — a wake
transport is a lifesaver for a parked *worker* and merely a re-evaluation
signal for a terminated *invocation*; the defect was treating the latter as
quiescence.

## 14. Test and gate results (all executed on the fix branch)

Default (CI) configuration is the experimental-liburing stub. The real
liburing path (`--with-liburing=true`) is a separate feature gate; the
liburing-only `uring_submit_failure_test` target registers only under that
gate, while the other uring targets register in both modes (stub evidence
bodies / real path).

| Gate | Command(s) | Result |
| --- | --- | --- |
| Baseline (pre-change, master `ff003fd`) | Clang Debug `xmake test` | **166/166** (166 default-gate targets, all pass; mechanically counted from the run's `running.test` lines) |
| Full Debug, default config (post-fix) | `xmake f -m debug --toolchain=clang; xmake build sluice_core sluice_async -g test; xmake test` | **167/167** (166 baseline + 2 new issue116 tests − 1 forensics test moved OUT of the default gate: probabilistic diagnostic tooling, see below) |
| `test:default-gate-targets` | 167 | default-gate test targets (== the `running.test` line count of a Linux Clang Debug stub `xmake test -v`); derived mechanically by `scripts/gates/mechanical-facts.py` from the xmake lua registrations, and every `test:default-gate-targets` doc row must equal it |
| Deterministic regression | pre-fix / post-fix | **8/8 fail-closed(70) → 8/8 pass** (§10); stays in the deterministic merge gate |
| Liveness forensics test | manual forensic path (§3/§11 recipe; `xmake run issue116_liveness_forensics_test`) + nightly hardening soak | **out of the default `xmake test` gate** — probabilistic diagnostic tooling with in-process 20 s watchdog (exit 42 + state dump on stall) |
| Stress before/after | §3 recipe | **3/40 hangs → 0/120** (§11) |
| Release | `xmake f -m release --toolchain=clang ...; xmake test` | 167/167 (5/5 isolated clean runs) |
| TSan | `xmake f -m tsan ...; xmake run -g test` | **full group pass, 0 warnings** (a test-side seam-disarm ordering bug was caught by TSan first and fixed: disarm backend gates before `join()`) |
| ASan+UBSan | `xmake f -m asanubsan ...; xmake run -g test` | **full group pass, 0 errors** |
| Sanitizer race classes for this change | driver re-entry vs submit/stop publication (lifecycle_mtx_ domain); re-park vs interrupt (D4-RM13/RM14) | exercised by the regression + forensics tests under TSan and the full copy suite |
| Backend matrix | ThreadPoolBackend (observed trigger; full suite), Fake/Sync (full suite; non-split path unchanged) | pass |
| Mechanical/doc gates | `check-doc-links.py` (+self-test), `verify-architecture-docs.py`, `mechanical-facts.py` (+self-test, incl. the test-total claim), `git diff --check` | all PASS |
| Real liburing | `xmake f --with-liburing=true ...` | **pre-existing master failure, out of scope**: 4 uring tests (`uring_backend_test`, `phase_g_closeout_uring_test`, `uring_d2_failure_noalloc_test`, `backend_conformance_test`) fail identically with this branch's changes fully stashed (master code segfaults, rc 139, at `uring_available_matches_build_mode`) — environment/master uring-path issue, not reachable from this change (production delta is `application_runtime.cpp` only; the stash run is the proof). Recorded, not bisected (freeze policy: no uring work in this fix). |

Documented flakes observed during gated runs (all consistent with the
known concurrent-starvation flake classes on record):

- One Release-run failure while a background stress batch (3 CPU spinners)
  saturated the pinned CPUs concurrently; 5/5 subsequent isolated runs
  clean; name not captured.
- `runnable_steal_test` hung twice inside full parallel suite executions
  (killed at 22–50 min, state R); standalone sampling — including
  CPU-pinned — passes 10/10. This is the #115-family steal-stranding
  flake (Scheduler-level; no ApplicationRuntime in that test; this
  change's production delta cannot execute there).

Mechanical/documentation gates: executed individually (table above);
`bash scripts/gates/pre-push.sh` reproduces the same set.

## 15. Remaining risks

1. **Startup-ordering family beyond the observed component.** Any future
   wake publication that neither bumps `control_epoch_` nor leaves
   `outstanding > 0` while an obligation exists could re-open a variant.
   The outstanding>0 predicate covers every obligation class that requires
   a runtime observer today (accepted I/O); runnable/delivered obligations
   cannot survive an invocation boundary in the runtime topology (§6, §12).
2. **#115** remains open (distinct mechanism; §13) — observed twice more
   during this investigation's suite runs (§14).
3. **Real-liburing master failure** (§14, pre-existing) — needs its own
   issue if not already tracked; not touched by this change.
4. **The one uncaptured Release-run flake** (§14) — attributed to
   deliberate concurrent CPU saturation during that run; watch CI.

## 16. Files changed (this investigation)

- `src/async/application_runtime.cpp` — the fix (driver re-entry on
  outstanding>0) + `test_dump_forensics` (internal-testing).
- `include/sluice/async/application_runtime.hpp` — guarded declaration.
- `src/async/scheduler_park_wake.cpp` — forensics dump extension
  (internal-testing only).
- `tests/issue116_interrupt_reevaluation_regression_test.cpp` — the
  deterministic merge-gate regression (new); flag rendezvous via
  std::atomic::wait/notify (hard timeout as escape hatch only).
- `tests/issue116_liveness_forensics_test.cpp` — probabilistic starvation
  reproducer with race-free state dump (new; investigation tooling,
  EXPLICITLY OUT of the default `xmake test` gate — manual forensic path,
  also driven by the nightly hardening soak).
- `include/sluice/async/detail/ready_wait_source.hpp` /
  `include/sluice/async/detail/uring_wait_source.hpp` — the wait-phase
  entry seam (internal-testing) now notifies atomic-wait consumers after
  the store (persistent state first, then notify; no lost wake).
- `xmake/tests/async_internal.lua` — registrations (regression in the
  gate; forensics target out of the gate).
- `scripts/hardening/phases.py` — forensics round in the Version B soak.
- `scripts/gates/mechanical-facts.py` — test-total claims verified
  mechanically (`test:default-gate-targets` rows vs. lua registrations).
- `docs/architecture/issue-116-reentry-liveness-gate.md` — compliance gate.
- This document.
