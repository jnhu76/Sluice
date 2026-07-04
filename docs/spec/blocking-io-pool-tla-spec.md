# BlockingIoPool TLA+ specification (sluice-CORE-024S)

**Status: EXHAUSTIVELY model-checked for the modeled protocol.** This is the
authoritative formal model for `sluice::BlockingIoPool`'s internal admission /
bounded-queue / dequeue / completion / shutdown-drain protocol. Where
single-threaded tests and stress tests are PROBABILISTIC (they cover the
interleavings they happen to hit), this TLA+ spec + TLC model check is
EXHAUSTIVE over all reachable states for the modeled constants.

The model does **not** prove deadlock freedom for arbitrary user callables or
arbitrary task dependency graphs.

## What it proves (and the C++ tests cannot)

| Property | Class | TLC result | Why tests can't prove it |
|----------|-------|------------|--------------------------|
| **C1 no internal protocol stuck state** | safety | ✓ every reachable modeled state is legitimate quiescence or has a real protocol transition enabled | Stress tests run finite time; a stuck state at interleaving #1001 is invisible |
| **C3 queue bound** | safety | ✓ `Len(queue) ≤ MaxDepth` always | The exact window between notify and push is hard to hit in tests |
| **C4 happens-before** | safety | ✓ `getters ⊆ done` (linearizability) | TSan catches races but doesn't prove all memory-order visibility |
| **C2 modeled-task progress** | liveness | ✓ every accepted modeled task eventually reaches `done` under weak fairness | "Eventually" is a liveness property; finite tests can't establish it |
| lifecycle consistency | safety | ✓ accepted∩rejected=∅, done⊆submitted, ... | Structural invariants across all interleavings |

## Model

- **Constants (small, for exhaustive search):** `NumWorkers=2`, `MaxDepth=2`,
  `NumTasks=3`. TLC exhausts all reachable interleavings of these configured
  constants; this is not a mathematical induction proof over all possible sizes.
- **State:** `queue` (FIFO seq of task IDs), `accepting` (bool), `inFlight`
  (dequeued-not-done), `done` (Task state ready), `submitted`, `rejected`,
  `getters`.
- **Actions:** `Submit` (backpressure), `TrySubmit` (non-blocking), `Dequeue`
  (worker FIFO pop), `Complete` (worker sets Task ready), `Get` (submitter
  consumes value), `Shutdown`. `Next` contains only real modeled protocol
  transitions; ordinary TLA+ stuttering is allowed by `[][Next]_Vars`, not by an
  artificial no-op protocol action.
- **Legitimate quiescence:** admission is closed, the queue is empty, no modeled
  task is in flight, all accepted modeled tasks are done, all done tasks have
  been gotten, and every modeled task has either been accepted or rejected:
  `accepting = FALSE /\ queue = <<>> /\ inFlight = {} /\
  submitted \subseteq done /\ getters = done /\ submitted \cup rejected =
  AllTasks`.
- **Checked stuck-state property:** `NoInternalProtocolStuck ==
  LegitimateQuiescence \/ ProtocolTransitionEnabled`, where
  `ProtocolTransitionEnabled` is the disjunction of `ENABLED` for `Submit`,
  `TrySubmit`, `Dequeue`, `Complete`, `Get`, and `Shutdown`.

## Modeled environment / progress boundary

The progress proof assumes:

1. modeled accepted task execution is finite;
2. running tasks do not synchronously depend on queued work from the same
   saturated pool for forward progress;
3. same-pool cyclic `Task::get()` dependency graphs are outside the model;
4. same-pool recursive blocking `submit()` is outside the progress guarantee;
5. the specification's weak-fairness assumptions apply to the relevant modeled
   actions (`Dequeue` and `Complete`).

The model does not prove deadlock freedom for arbitrary user callables or
arbitrary task dependency graphs.

## How to run (TLC model checker)

```bash
# One-time: fetch the TLA+ tools (proxy may be needed; ~30MB)
curl -L -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.7.0/tla2tools.jar

# Safety (C1/C3/C4 + lifecycle; C1 is NoInternalProtocolStuck):
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config spec/tla/BlockingIoPool.cfg spec/tla/BlockingIoPool.tla

# Liveness (modeled-task progress C2, weak fairness on Dequeue+Complete):
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config spec/tla/BlockingIoPool_liveness.cfg spec/tla/BlockingIoPool.tla
```

## Results (recorded run)

- **Safety config:** `Model checking completed. No error has been found.`
  `NoInternalProtocolStuck`, `QueueBoundInvariant`, `Linearizable`, and lifecycle
  invariants all hold for the modeled constants. State counts are recorded from
  the latest run below.
- **Liveness config:** `Checking ... temporal properties ... No error has been
  found.` Modeled-task progress holds under weak fairness on `Dequeue` and
  `Complete`.

### Latest TLC run

Updated by the 024S B-1 closeout correction:

```text
Safety:
  Model checking completed. No error has been found.
  1187 states generated, 417 distinct states found, 0 states left on queue.
  Depth 14. Fingerprint collision probability (optimistic): 1.7E-14.
  NoInternalProtocolStuck, QueueBoundInvariant, Linearizable, StateConsistency,
  and TaskLifecycle all hold.

Liveness:
  Model checking completed. No error has been found.
  1187 states generated, 417 distinct states found, 0 states left on queue.
  Depth 14. Fingerprint collision probability (optimistic): 1.7E-14.
  TemporalProperty / modeled-task progress holds under weak fairness on
  Dequeue and Complete.
```

## A note on stuttering and terminal states

The earlier model included an explicit `Stutter == UNCHANGED Vars` action as a
disjunct of `Next`. That made `Next` contain an always-enabled no-op protocol
transition and made the prose around TLC deadlock checking too broad. The current
model removes `Stutter` from `Next`. Terminal states are handled by ordinary
TLA+ stuttering in `[][Next]_Vars`, while the safety config checks
`NoInternalProtocolStuck` to distinguish legitimate quiescence from a non-
terminal modeled protocol stuck state. TLC's built-in terminal-state deadlock
check is disabled in the configs; it is not reported as proof of production
deadlock freedom.

## Relationship to the C++ tests

- The **B-class invariant tests** (`tests/blocking_io_pool_invariants_test.cpp`)
  pin the same properties DETERMINISTICALLY at the C++ level for the common
  case (exactly-once, no-lost, FIFO, no-double-get).
- This **TLA+ spec** checks the modeled protocol EXHAUSTIVELY across all
  interleavings for the modeled size, including adversarial schedules a test
  runner will never produce.
- The **C9 data race** (fixed earlier) was the kind of bug this methodology
  targets: a property (`submit` reads `accepting` consistently with `shutdown`
  writes) that held in every test run but failed under an interleaving the tests
  didn't hit. The TLA+ spec's `accepting` is a single shared variable read/written
  under the model's atomic actions, so its consistency is part of what TLC proves.

## Files

- `spec/tla/BlockingIoPool.tla` — the specification
- `spec/tla/BlockingIoPool.cfg` — safety config (invariants + Spec)
- `spec/tla/BlockingIoPool_liveness.cfg` — liveness config (FairSpec + property)
