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

- **R2, R3, R11, R12, R13, R14, R15, R17, R18:** REACH.

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
SubmitRollback (both enabled in Draining), so accounting converges and
drain_complete remains reachable.

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
