# E10 WaitNode / Cancellation-Safe WaitQueue — Formal Model

Narrow TLA+ model of the E10 wait-protocol (sluice-CORE-E10), realizing the
§2 Design Law (one winner transition) and §7 Unlink Law (removal is not an
independent protocol). Mirrors the style of the E7/E8/E9 TLA+ models
(`docs/spec/e7_publication/`, `e8_ownership_transfer/`, `e9_park_wake/`).

The load-bearing E10 question:

```text
Can wake and cancellation race on one registered wait and produce more than
one terminal resolution, more than one scheduler-wake intent, or a corrupted
queue?
```

Answer (§2/§7): NO. There is ONE canonical terminal resolver —
`WaitNode::resolve_(outcome)` = CAS `state_ Registered -> outcome`. That single
CAS is the state authority AND the unlink authority: the winner CAS-then-
unlinks in one critical section; every loser's CAS fails and it does nothing.

## Files

- `E10WaitNode.tla`                 — the correct protocol (one winner CAS).
- `E10WaitNode.cfg`                 — TLC config (safety invariants).
- `E10WaitNodeLiveness.cfg`         — TLC config (EventualResolution liveness).
- `E10WaitNodeBuggyNoWinner.tla/.cfg` — negative model: wake/cancel WITHOUT a
  winner CAS (each unconditionally rewrites state + re-unlinks). Produces an
  `InvNoDoubleCompletion` counterexample.
- `README.md`                       — this file + refinement map + proof.

## Model domain (finite, exhaustive TLC)

```text
Nodes = {N0, N1}
nodeState[n] in {Detached, Registered, Woken, Cancelled}
```

`linked[n]` mirrors queue membership (Registered ⇔ linked).
`resolvedCount[n]` counts winning resolutions (load-bearing ≤ 1).
`wakeDispatched` counts scheduler-wake intents (load-bearing == Σ resolvedCount).

The "winner CAS" is modeled as the guarded atomic transition: `ResolveWake(n)`
is enabled **only** when `nodeState[n] = "Registered"`, and moves it to
`"Woken"`. Because TLA+ actions are mutually exclusive on the `Registered`
state, at most one resolver transitions out of `Registered` per node — this IS
the single-winner linearization. A concurrent `ResolveCancel(n)` observes
`nodeState[n] = "Woken"` (terminal) and is NOT enabled.

## Results

**NOTE — TLA+ tooling limitation.** This environment has no `tla2tools.jar`
(the E9 README expects it at `/tmp/tla2tools.jar`) and no network to fetch it.
**TLC was NOT executed here.** The `.cfg` files are reproducible from a fixed
jar exactly as the E7/E8/E9 models are; running them is:

```bash
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e10_waitnode/E10WaitNode.cfg docs/spec/e10_waitnode/E10WaitNode
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e10_waitnode/E10WaitNodeLiveness.cfg docs/spec/e10_waitnode/E10WaitNode
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e10_waitnode/E10WaitNodeBuggyNoWinner.cfg docs/spec/e10_waitnode/E10WaitNodeBuggyNoWinner
```

Expected (by construction, pending actual TLC execution):
- `E10WaitNode` safety: all invariants PASS.
- `E10WaitNode` liveness: `EventualResolution` PASS under `FairResolve`.
- `E10WaitNodeBuggyNoWinner`: `InvNoDoubleCompletion` counterexample (a node
  reaches `resolvedCount = 2`).

Because TLC could not be run, the authoritative proof of E10's safety is the
**explicit state-transition table + linearization proof** below (§12 permits
this when the formal framework is unavailable).

## Refinement map (TLA+ → production)

| Formal concept/action | Production path / seam | authority / domain |
| --------------------- | ---------------------- | ------------------ |
| `nodeState` | `WaitNode::state_` (atomic `State`) | winner authority |
| `Register(n)` | `WaitQueue::register_wait_locked` → `WaitNode::register_` (CAS Detached→Registered) under `q.mtx()` | structural domain |
| `ResolveWake(n)` | `Scheduler::wake_wait_one` → `WaitQueue::wake_one_locked` → `WaitNode::resolve_(Woken)` (CAS) + `unlink_locked` + `route_runnable_locked` | winner + scheduler wake |
| `ResolveCancel(n)` | `Scheduler::cancel_wait` → `WaitQueue::cancel_locked` → `WaitNode::resolve_(Cancelled)` (CAS) + `unlink_locked` + `route_runnable_locked` | winner + scheduler wake |
| `linked[n]` | node is in the queue's intrusive list (`home_ != null`) | structural |
| `resolvedCount[n]` | (conceptual) the CAS returns true exactly once | winner |
| `wakeDispatched` | each winner calls `route_runnable_locked` once (E7-T2 exactly-once via `make_runnable`) | scheduler wake |
| Absent `Reset` action | E10 nodes are NOT resettable (terminal is absorbing) | absorbing terminal |
| `FairResolve` | enabled resolvers run under the scheduler worker loop | liveness |

## Explicit state-transition table (authoritative, executable fallback)

Single node, all transitions (the columns are pre-state → action → post-state;
`*` marks the winner CAS linearization point):

| pre-state | action | winner? | post-state | unlink? | scheduler wake? | notes |
|-----------|--------|---------|------------|---------|-----------------|-------|
| Detached  | register_(q, fiber) | — | Registered | link (tail) | no | single-shot (C8) |
| Registered | resolve_(Woken) **CAS succeeds*** | **WIN** | Woken | unlink (same CS) | one enqueue | §2/§7 linearization |
| Registered | resolve_(Woken) **CAS fails** | lose | (terminal) | no | no | a cancel won first |
| Registered | resolve_(Cancelled) **CAS succeeds*** | **WIN** | Cancelled | unlink (same CS) | one enqueue | §2/§7 linearization |
| Registered | resolve_(Cancelled) **CAS fails** | lose | (terminal) | no | no | a wake won first |
| Woken     | resolve_(Woken) | lose | Woken | no | no | absorbing (C2) |
| Woken     | resolve_(Cancelled) | lose | Woken | no | no | absorbing (C4/C5) |
| Cancelled | resolve_(Cancelled) | lose | Cancelled | no | no | absorbing (C3) |
| Cancelled | resolve_(Woken) | lose | Cancelled | no | no | absorbing (C4) |
| Woken/Cancelled | register_(q, fiber) | — | (terminal) | no | no | C8 reuse rejected |
| Detached  | ~WaitNode | — | destroyed | n/a | n/a | safe (not linked) |
| Registered | ~WaitNode | — | **assert fail (debug)** | n/a | n/a | C9 violation |

## Linearization proof (§7)

**Claim:** for any single registered node, exactly one of {wake, cancel}
performs the terminal resolution, the unlink, and the scheduler-wake enqueue.

**Proof.**

1. **The CAS is the authority.** `resolve_(outcome)` is a single
   `compare_exchange_strong(Registered → outcome)` on `state_`. A CAS succeeds
   for exactly one concurrent caller; every other caller's CAS fails (it
   reloads the now-terminal value). [SingleWinner]

2. **Unlink is bound to the winning CAS.** `wake_one_locked` / `cancel_locked`
   call `unlink_locked(node)` **only on the `resolve_` success path**, inside
   the same `q.mtx()` critical section. A loser returns before `unlink_locked`
   is reached. Therefore unlink happens exactly once, for the winner.
   [NoDoubleUnlink]

3. **Scheduler-wake is bound to the winning CAS + E7-T2.** The winner then
   calls `make_runnable()` (E7-T2 exactly-once: returns true only on the
   waiting→runnable transition) and `route_runnable_locked` (one enqueue). A
   loser never reaches this code (it returned at step 2). Even if two winners
   were hypothetically possible (they are not, by step 1), `make_runnable`
   would return false for the second, blocking the second enqueue.
   [NoDuplicateSchedulerWake]

4. **The linearization point** is the instant the winning CAS stores the
   terminal value. At that instant: (a) the node is terminally resolved
   (absorbing — no later transition can change it), (b) the winner is uniquely
   determined, (c) the winner owns the unlink right (executed immediately
   after, under the same lock). Every observable (outcome(), is_registered(),
   queue membership) is consistent with this instant because they read the
   same atomic `state_` with acquire.

5. **NoTerminalResurrection.** No production action moves a terminal node to a
   non-terminal state: `resolve_` requires `Registered`; `register_` requires
   `Detached`; terminal nodes are neither. The state machine is closed.

6. **LinkedImpliesLive / destruction.** `~WaitNode` asserts `!is_registered()`.
   A Registered node is linked (`home_ != null`); destroying it would dangle a
   queue pointer. The winner unlinks before the await frame may exit, so a
   terminal node's destruction is safe. [C9]

$\blacksquare$

## Required safety properties (§12) — coverage

| Property | Model element | Production guard |
|----------|---------------|------------------|
| SingleWinner | `resolve_` enabled only on Registered | CAS |
| NoDoubleCompletion | `resolvedCount[n] <= 1` | CAS + E7-T2 make_runnable |
| NoTerminalResurrection | no action moves Woken/Cancelled out | closed state machine |
| LinkedImpliesLive | Registered ⇔ linked | register_/unlink under mtx |
| TerminalEventuallyDetached | winner unlinks same-CS; EventualResolution under FairResolve | worker-loop drain |
| NoDuplicateSchedulerWake | wakeDispatched == Σ resolvedCount | make_runnable exactly-once |

## What this model does NOT cover

- The Scheduler, Fiber lifecycle, MW admission, park/wake (closed by E7/E8/E9).
- Backends, context-switch asm, stacks.
- Timers, deadlines, mutex/semaphore/select/multi-wait (explicitly out of E10).
- Chase-Lev / lock-free deques (E16).

## Reproducible verification

The committed `.cfg` files reproduce the gate from a fixed `tla2tools.jar`
(jar not present in this environment; no network). The production stress gates
(e10_wait_queue_test C12, e10_scheduler_wait_test C10c) ARE executed and pass —
see the final report §M for exact counts.
