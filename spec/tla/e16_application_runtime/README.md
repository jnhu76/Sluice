# E16 Application Runtime — TLA+ Formal Suite

## Overview

Models the E16 Application Runtime lifecycle state machine, covering:

- Runtime states: Constructed → Starting → Running → Stopping → Draining → Stopped
- Failure paths: StartFailed, Fatal
- Driver FSM: barrier_wait → in_run_live → between_invocations → drained_wait → exited
- Close ownership protocol (Open → InProgress → Closed)
- Admission reservation + group commit + rollback
- Task I/O submission and reaping
- Control epoch boundary wake protocol

## Models

| File | Role |
|------|------|
| `E16ApplicationRuntime.tla` | Correct model (safety + liveness + reachability) |
| `E16ApplicationRuntimeBuggyBoundaryWake.tla` | NEG-E16-1: missing epoch re-check at boundary |
| `E16ApplicationRuntimeBuggyStopClose.tla` | NEG-E16-2: cancel after group destroy |
| `E16ApplicationRuntimeBuggyStartupAbort.tla` | NEG-E16-3: startup abort never fires |
| `E16ApplicationRuntimeBuggyCloseOwner.tla` | NEG-E16-4: two close owners elected |
| `E16ApplicationRuntimeBuggyCloseOwnerBeforeDrain.tla` | NEG-E16-5: close before drain |
| `E16ApplicationRuntimeBuggyDirectStopped.tla` | NEG-E16-6: Stopped with resources alive |

## Configurations

| Cfg | Purpose |
|-----|---------|
| `E16ApplicationRuntime.cfg` | Safety invariants (composite Inv) |
| `E16ApplicationRuntimeLiveness.cfg` | Liveness (Live1/2/3/5/6 under FairSpec) |
| `E16ApplicationRuntimeWide.cfg` | Wide-domain safety (MaxIO=3) |

## Invariants (safety)

- Inv1: Typing
- Inv2: Admission authority
- Inv3: Stop closes admission
- Inv4: Accounting bounds (terminal ≤ admitted)
- Inv6: Drain complete
- Inv7: No premature Stopped
- Inv8: Resource hierarchy
- Inv9: No cancel after group destroy
- Inv10: Startup abort excludes Running
- Inv11: Driver uniqueness
- Inv12: Close owner uniqueness
- Inv13: Close monotonicity
- Inv14: Submit publication
- Inv19: Driver exit before destruction
- Inv20: Rejected never executes
- Inv21: Task I/O lifetime
- Inv22: Safe destructor
- Inv23: Join return
- Inv25: Stopped after drain

### Reclassified / documentation-only (E16-POST-MERGE-CORRECTIVE-1, C4)

A property counts as a TLC state invariant only if it checks independent
modeled state and can be falsified by an incorrect transition. These were
removed from the verified `Inv` set / cfg because they are tautological or
structural-only; they remain defined in the module as documentation:

- **Inv5 (TerminalSnapshot):** EXCLUDED — tautological. `task_set_terminal_snapshot`
  is a derived operator and `Inv5TerminalSnapshot` asserts it equals its own
  definition. Kept as a derived refinement definition, not independent safety
  evidence.
- **Inv17 (NoBusyLoop) == TRUE / Inv24 (ShutdownDispatched) == TRUE:**
  structural-only; never in `Inv` or the cfg.
- **Inv18 (PostDrainNoEarlyExit):** EXCLUDED as a state invariant — it was a
  same-state predicate (`drained_wait /\ ~exit_req /\ ~fatal => #exited`),
  unfalsifiable as a transition check. The load-bearing obligation is now the
  ACTION-LEVEL temporal property **Live6PostDrainExitCaused** (see Liveness).

## Liveness

- Live1: Admission observed
- Live2: Stop before commit
- Live3: Drain completes
- Live5: Driver exits after explicit exit request (under weak fairness)
- **Live6 (C4): PostDrainExitCaused** — action-level temporal property: any
  transition from `drained_wait` to `exited` must be caused by
  `driver_exit_requested'` or `fatal_snapshot'` (`[][A]_vars`). Replaces the
  tautological current-state `Live6PostDrainStable`.

## Reachability witnesses

Each `NotReach_Ri` invariant asserts a state is unreachable; TLC violates it,
proving the state IS reachable in the correct model.

- **R2, R3, R11, R12, R13, R14, R15, R17, R18, R19:** REACH.

### R19 — post-stop admission commit (E16-POST-MERGE-CORRECTIVE-2)

Production `submit()` reserves admission under `lifecycle_mutex` (the
linearization point of stop-vs-submit, ADR §5), then RELEASES the lock and
calls `root_group_->async(...)`; its success path does NOT re-check
`admission_open` (`application_runtime.cpp`). So the reachable ordering is

```
SubmitReserve -> RequestStopRunning (closes admission) -> SubmitGroupCommit
     -> TaskBodyExit -> PublishDrainComplete
```

The task's reservation won the race while `Running`, so it is genuinely
admitted and must be allowed to commit and terminate even after admission
closed. The previous `admission_open` guard on `SubmitGroupCommit` excluded
this path and over-approximated production (making Live3 under-cover the real
stop-vs-submit race). R19 witnesses that the corrected model reaches the
post-stop-commit state: `~admission_open /\ runtime_state = "Draining" /\
\E t: task_committed[t] /\ ~drain_complete`.

## Running

```bash
# Via repository verifier (recommended):
bash scripts/formal/verify-e16-application-runtime.sh

# Via the unified orchestrator:
python3 scripts/formal/verify.py suite e16-application-runtime

# Manual single check:
cd $(mktemp -d) && cp /path/to/spec/tla/e16_application_runtime/*.{tla,cfg} .
java -cp tla2tools.jar tlc2.TLC -workers 4 -config E16ApplicationRuntime.cfg E16ApplicationRuntime
```

## Negative model expected violations

| Model | Expected |
|-------|----------|
| NEG-E16-1 | Temporal property `NoStrandedSuccessfulAdmission` violated |
| NEG-E16-2 | Invariant `NoCancelAfterGroupDestroy` violated |
| NEG-E16-3 | Invariant `StartupAbortNeverRuns` violated |
| NEG-E16-4 | Invariant `AtMostOneCloseOwner` violated |
| NEG-E16-5 | Invariant `Inv25StoppedAfterDrain` violated |
| NEG-E16-6 | Invariant `Inv7NoPrematureStopped` violated (DirectStoppedWithResources) |

## DrainBegin refinement note (E16-POST-MERGE-CORRECTIVE-1)

Production `drain()` transitions Stopping → Draining unconditionally — it does
not wait for any in-flight admission reservation to resolve. A reservation made
by `submit()` may still be pending (admitted_count_ incremented, Group::async not
yet committed/rolled back) when Draining is entered. The model therefore does
NOT add a `~admission_reservation_active` guard to `DrainBegin`: doing so would
make the model stronger than production and exclude the real stop-vs-submit
race that Live3 must cover. The reservation resolves via SubmitGroupCommit or
SubmitRollback (both enabled in Stopping/Draining — see the SubmitGroupCommit
note below), so accounting converges and drain_complete remains reachable.

## SubmitGroupCommit refinement note (E16-POST-MERGE-CORRECTIVE-2)

`SubmitGroupCommit` is NOT gated on `admission_open`. ADR §5 and production
`ApplicationRuntime::submit()` define the admission RESERVATION
(`admission_open` checked TRUE + `admitted_count++` under `lifecycle_mutex`)
as the linearization point of stop-vs-submit. `submit()` then releases
`lifecycle_mutex` and calls `root_group_->async(...)`; its success path never
re-checks `admission_open`. A reservation that won the race while `Running`
must therefore be permitted to commit even after `request_stop()` closed
admission (the task is genuinely admitted and must terminate so accounting
converges). The previous `admission_open` guard on `SubmitGroupCommit`
excluded this production-reachable post-stop-commit path and over-approximated
production. The remaining guards — `admission_reservation_active /\
task_admitted[t] /\ ~task_committed[t]` — restrict the commit to exactly one
legitimately pending reservation per task, which is sufficient. R19 witnesses
the corrected reachability.

## Production / formal refinement map (E16-POST-MERGE-CORRECTIVE-1)

| Formal action/state                | Production path                           |
| ---------------------------------- | ----------------------------------------- |
| direct Constructed close election  | `ApplicationRuntime::shutdown`            |
| StartFailed close election         | `ApplicationRuntime::shutdown`            |
| startup-abort driver join          | `ApplicationRuntime::start` abort path    |
| DestroyGroup                       | unified `close_resources`                 |
| DestroyScheduler                   | unified `close_resources`                 |
| DestroyIoContext                   | unified `close_resources`                 |
| PublishStopped                     | final locked section of `close_resources` |
| Fiber Runtime identity             | `Fiber::execution_tag_`                   |
| identity install/restore           | Runtime task wrapper                      |
| identity write authority           | private Scheduler/ApplicationRuntime seam |

Stopped publication does not precede destruction (every Stopped transition
funnels through `close_resources`, which destroys Group/Scheduler/AsyncIoContext
before publishing Stopped). Fiber-local storage solves multiplexing correctness
(the tag survives Fiber suspend/resume). Private write authority prevents
application code from bypassing Runtime self-close detection.


## Audit notes (2026-08-18, C++/TLA+ realignment)

- **`Inv15RollbackComplete` is defined but deliberately NOT gated** (not in
  `Inv` nor in the safety cfg). The README's invariant-reclassification list
  previously omitted it. It restates `SubmitRollback`'s action effect
  (rollback restores admitted/task_admitted), which no other action
  contradicts — re-deriving it as a gate would be redundant with the
  transition relation itself.
- **`Live4CloseWaiterCompletes` is defined but NOT in `LifeProps`** — a Fatal
  transition would falsify it. Intentional; now documented.
- **`Inv14SubmitPublication` is weak**: it only asserts "some epoch bump ever
  happened" after a submit publication; the unobserved-change coupling is
  carried by Live1 instead. Do not cite Inv14 as a lost-wake proof.
- Dead inventory: `NotReach_R1, R4..R10, R16` are defined but ungated
  (scene loop covers R2,R3,R11..R15,R17..R19); the `exiting` driver state is
  in the TypeOK domain but assigned by no action in both the model AND the
  current C++ `DriverState` enum. Harmless; retained for enum parity.
- Fairness boundary: `WF(TaskBodyExit)` / `WF(CompleteTaskIO)` are documented
  environment assumptions (user task terminates; backend completes) — see the
  in-model comment. `WF(ReapTaskIO)` abstracts over scheduler workers the
  model does not contain; the #116 forced-re-entry disjunct of
  `DriverReenterRunLive` is the compensating mirror.
- The implementation binding is `src/async/application_runtime.cpp`
  (manifest). The suite's R19 corrective matches the as-built
  unlock-before-`Group::async` submit path exactly.
