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

## Configurations

| Cfg | Purpose |
|-----|---------|
| `E16ApplicationRuntime.cfg` | Safety invariants (Inv1–Inv23 composite) |
| `E16ApplicationRuntimeLiveness.cfg` | Liveness (Live5DriverExits under FairSpec) |
| `E16ApplicationRuntimeWide.cfg` | Wide-domain safety (MaxIO=3) |

## Invariants (safety)

- Inv1: Typing
- Inv2: Admission authority
- Inv3: Stop closes admission
- Inv4: Accounting bounds (terminal ≤ admitted)
- Inv5: Terminal snapshot
- Inv6: Drain complete
- Inv7: No premature Stopped
- Inv8: Resource hierarchy
- Inv9: No cancel after group destroy
- Inv10: Startup abort excludes Running
- Inv11: Driver uniqueness
- Inv12: Close owner uniqueness
- Inv13: Close monotonicity
- Inv14: Submit publication
- Inv18: Post-drain no early exit
- Inv19: Driver exit before destruction
- Inv20: Rejected never executes
- Inv21: Task I/O lifetime
- Inv22: Safe destructor
- Inv23: Join return

## Liveness

- Live5: Driver exits after explicit exit request (under weak fairness)

## Reachability witnesses (R1–R16)

Each `NotReach_Ri` invariant asserts a state is unreachable; TLC violates it,
proving the state IS reachable in the correct model.

## Running

```bash
# Via repository verifier (recommended):
bash scripts/formal/verify-e16-application-runtime.sh

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
