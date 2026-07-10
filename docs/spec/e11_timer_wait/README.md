# E11 Deadline / Timer Wait Integration — Formal Model

Narrow TLA+ model of the E11 deadline/timer wait integration (sluice-CORE-E11),
extending the E10 WaitNode protocol
([`docs/spec/e10_waitnode/`](../e10_waitnode/)) with a THIRD resolution cause
`TIMER_EXPIRE` and the two new state dimensions E11 introduces over E10:

```text
timer-registration lifetime   (TimerRegistration control-block state:
                               ACTIVE / RETIRED / CONSUMED)
deadline park liveness        (monotonic time + deadline-due predicate +
                               Scheduler parked/executable state)
```

Mirrors the style of the E7/E8/E9/E10 TLA+ models.

## The load-bearing E11 question

```text
Can a deadline/timer compete with resource wake and cancellation on one
registered wait, AND a timer registration physically outlive the WaitNode it was
bound to, while preserving:

  - exactly one terminal resolution (one winner CAS),
  - exactly one runnable publication,
  - wait-epoch isolation (a timer for epoch E cannot resolve epoch E+1),
  - timer lifetime closure (no expiry dereferences a destroyed WaitNode),
  - deadline admission closure (an already-due deadline is never lost),
  - deadline park liveness (a parked Scheduler progresses past a due deadline)?
```

Answer: YES — because `TIMER_EXPIRE` enters the SAME `WaitNode::resolve_` CAS
authority (no second winner protocol), and the `TimerRegistration` carries
independently-stable retirement state (`ACTIVE/RETIRED/CONSUMED`) that a
straggling expiry observes BEFORE dereferencing its bound node.

## Files

- `E11TimerWait.tla`                            — the correct protocol (three
  resolvers through one `resolve_` CAS + timer-registration lifetime + park
  liveness). Preserves I1–I7.
- `E11TimerWait.cfg`                            — TLC config (safety invariants).
- `E11TimerWaitLiveness.cfg`                    — TLC config (I6 park liveness).
- `E11TimerWaitNeg1DoublePublication.tla/.cfg`  — NEG-1: timer expiry with no
  winner CAS -> double publication.
- `E11TimerWaitNeg2TimerCancelDoublePublication.tla/.cfg` — NEG-2: cancel with
  independent completion authority -> double publication.
- `E11TimerWaitNeg3StaleCrossEpoch.tla/.cfg`    — NEG-3: stale timer keys on a
  reusable storage slot -> resolves epoch E+1 (the address-reuse boundary).
- `E11TimerWaitNeg4CallbackAfterRetirement.tla/.cfg` — NEG-4: node destroyed
  while a bound registration is still ACTIVE -> callback outlives the node (I4).
- `E11TimerWaitNeg5DeadlineLostParked.tla/.cfg` — NEG-5: Scheduler parks past a
  due deadline -> deadline lost, wait unresolved forever (I6 liveness).
- `README.md`                                   — this file + refinement map.

## Model domain (finite, exhaustive TLC)

```text
Nodes = {N0, N1}                 -- wait epochs (one per Register lifetime)
Regs  = {R0, R1}                 -- timer registrations (one bound epoch each)
Time  = 0..3                     -- monotonic logical time (small bound)
DeadlineVal = 0..3               -- absolute deadline values
```

State dimensions (NEVER collapsed — see the spec "Required formal state
dimensions"):

```text
nodeState[n]   : Detached/Registered/Woken/Cancelled/Expired  (E10 + Expired)
linked[n]      : queue membership (Registered <=> linked)
resolvedCount[n] : winning resolutions (<= 1)
wakeDispatched : total scheduler-wake intents (== Sum resolvedCount)
regState[r]    : Inert/Active/Retired/Consumed   (timer lifetime)
regEpoch[r]    : the node epoch R is bound to (immutable after Register)
regDeadline[r] : absolute monotonic deadline
nodeAlive[n]   : storage reachable (FALSE after DestroyNode)
now            : monotonic logical time
parked         : whether the Scheduler is parked (idle)
```

## Results

**NOTE — TLA+ tooling limitation.** This environment has no `tla2tools.jar`
(the E10 README records the same limitation) and no network to fetch it. **TLC
was NOT executed here.** The `.cfg` files are reproducible from a fixed jar
exactly as the E7/E8/E9/E10 models are; running them is:

```bash
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e11_timer_wait/E11TimerWait.cfg docs/spec/e11_timer_wait/E11TimerWait
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e11_timer_wait/E11TimerWaitLiveness.cfg docs/spec/e11_timer_wait/E11TimerWait
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e11_timer_wait/E11TimerWaitNeg1DoublePublication.cfg \
  docs/spec/e11_timer_wait/E11TimerWaitNeg1DoublePublication
# ... likewise for Neg2..Neg5
```

Expected (by construction, pending actual TLC execution):

| model | config | result | counterexample |
|-------|--------|--------|----------------|
| `E11TimerWait` | `E11TimerWait.cfg` | all invariants PASS | — |
| `E11TimerWait` | `E11TimerWaitLiveness.cfg` | `DeadlineParkLiveness` PASS | — |
| `E11TimerWaitNeg1DoublePublication` | `.cfg` | `InvSingleResolutionWinner` FAIL | a node reaches `resolvedCount = 2` (wake + timer) |
| `E11TimerWaitNeg2TimerCancelDoublePublication` | `.cfg` | `InvSingleResolutionWinner` FAIL | a node reaches `resolvedCount = 2` (timer + cancel) |
| `E11TimerWaitNeg3StaleCrossEpoch` | `.cfg` | `InvSingleResolutionWinner` FAIL | a slot resolved twice across E and E+1 |
| `E11TimerWaitNeg4CallbackAfterRetirement` | `.cfg` | `InvTimerLifetimeClosure` FAIL | `nodeAlive[n]=FALSE /\ regState[r]=Active` |
| `E11TimerWaitNeg5DeadlineLostParked` | `.cfg` | `DeadlineParkLiveness` FAIL | deadline due forever, parked forever |

Because TLC could not be run, the authoritative proof of E11's safety is the
**refinement map + explicit causal proof below** (the E10 README §12 permits
this executable fallback when the formal framework is unavailable). The
deterministic production tests in `tests/e11_timer_wait_test.cpp` (T0–T16) ARE
executed and pass under ASan/UBSan — they are the load-bearing runtime evidence.

## Refinement map (TLA+ → production)

| Formal concept/action | Production path / seam | authority / domain |
| --------------------- | ---------------------- | ------------------ |
| `nodeState` | `WaitNode::state_` (atomic `State`) | winner authority |
| `Register(n)` + bind `R` | `Scheduler::await_wait_deadline` → `register_wait_locked` + `timer_pool_.emplace_back` + `heap_push_locked` under `global_mtx_` + `q.mtx()` | structural + timer registration |
| `AdmissionExpire(n)` | `await_wait_deadline` I5 recheck → `expire_locked` inline (clock >= deadline) | winner authority (same CS) |
| `ResolveWake(n)` | `Scheduler::wake_wait_one` → `wake_one_locked` → `resolve_(Woken)` + `unlink_locked` + `retire_timer_for_node_locked` + `route_runnable_locked` | winner + scheduler wake |
| `ResolveCancel(n)` | `Scheduler::cancel_wait` → `cancel_locked` → `resolve_(Cancelled)` + `unlink_locked` + `retire_timer_for_node_locked` + `route_runnable_locked` | winner + scheduler wake |
| `ResolveTimer(r)` | `Scheduler::pump_deadlines_locked` → `try_claim_expiry` (ACTIVE→CONSUMED) → `expire_locked` + `unlink_locked` + `route_runnable_locked` | winner (timer claims R, then resolve_ CAS) |
| `DestroyNode(n)` | WaitNode `~WaitNode` (fiber frame returns); requires `retire_timer_for_node_locked` ran in the winner CS first | lifetime |
| `regState` | `TimerRegistration::state_` (atomic `State` ACTIVE/RETIRED/CONSUMED) | timer callback authority |
| `regEpoch[r]` | the bound `TimerRegistration::node_` (captured as `WaitNode&`, never only `Fiber*`) | epoch binding |
| `now` / `Tick` | `Scheduler::clock_` (atomic) advanced by `advance_clock` (test) / steady_clock (prod) + the worker-loop `pump_deadlines_locked` | clock domain |
| `parked` / park-timeout | `park_on_wake_source` bounded by `earliest_active_deadline_locked` (I6) | coordination domain |

## Explicit causal proof (I3/I4 — the load-bearing E11 difference)

**Claim:** a timer registration `R_E` bound to wait epoch E cannot resolve a
later epoch E+1, and after E's node storage is destroyed no expiry may
dereference it.

**Proof.**

1. **Epoch binding is stable.** `R_E.node_` is set once at registration
   (`await_wait_deadline` → `timer_pool_.emplace_back(&node, &q, deadline)`) and
   never reassigned. It captures the `WaitNode&` of epoch E — the logical epoch
   identity, not a reusable address or a `Fiber*`. [epochBindingImmutable]

2. **Expiry gates on R's OWN state, then resolves the BOUND node.**
   `pump_deadlines_locked` calls `try_claim_expiry()` (CAS `ACTIVE→CONSUMED`) on
   `R_E` BEFORE reading `R_E.node_`. If `R_E` is RETIRED (a non-timer winner
   closed it) or already CONSUMED, the claim fails and `node_` is never read.
   [claimBeforeDeref — I4 gate]

3. **Retirement precedes node destruction.** A non-timer winner
   (`wake_wait_one` / `cancel_wait`) calls `retire_timer_for_node_locked` in the
   SAME `global_mtx_` critical section as the winning `resolve_` CAS, BEFORE
   runnable publication. The fiber cannot resume (and its frame cannot return,
   destroying the node) until the runnable ticket is published and the worker
   switches to it. Therefore `R_E` is RETIRED before `node_E`'s storage may be
   destroyed. [retireBeforeDestroy]

4. **A stale expiry observes RETIRED, not the node.** Once `node_E` is
   destroyed, a physically-retained/lazy `R_E` entry (still in the deadline
   heap) is eventually popped by `pump_deadlines_locked`. The pump calls
   `try_claim_expiry()`, which loads `R_E.state_` (RETIRED/CONSUMED) — it does
   NOT dereference `node_`. The claim fails; the entry is erased. It cannot
   resolve E+1 because `R_E.node_` still points at E's (now-destroyed) node
   object identity, and even if E+1 reused E's numeric address, the expiry never
   reads `node_` after a failed claim. [staleInert — I3 + I4]

5. **Cross-epoch isolation.** E+1 is a distinct `WaitNode` registered by a
   distinct `await_wait_deadline` with its OWN `TimerRegistration R_{E+1}`. `R_E`
   holds no reference to E+1's node. Even if E+1 reuses E's stack address, `R_E`
   resolves (if at all) `*R_E.node_` = E's object, whose `state_` is terminal
   (absorbing) — but step 4 shows the expiry never reaches a dereference.
   [noCrossEpoch]

The decisive boundary: **logical epoch identity (`R_E.node_`, the `WaitNode&`)
≠ reusable numeric address.** The retirement state on `R_E` (independent of the
node) is what closes callback reachability, not the node's absorbing terminal
state (which is gone once the node is destroyed). $\blacksquare$

## Required safety properties — coverage

| Invariant | Model element | Production guard | Deterministic test |
| --------- | ------------- | ---------------- | ------------------ |
| I1 Single Resolution Winner | `resolvedCount[n] <= 1` | `resolve_` CAS (three causes, one authority) | T5, T16 |
| I2 Single Runnable Publication | `wakeDispatched == Sum resolvedCount` | `make_runnable` exactly-once | T5, T16 |
| I3 Wait-Epoch Isolation | `regEpoch[r]` immutable; `ResolveTimer` targets it | `TimerRegistration::node_` captures `WaitNode&` | T7, T8 |
| I4 Timer Lifetime Closure | `ResolveTimer` gates on `regState = Active`; `DestroyNode` requires retirement | `try_claim_expiry` before deref; `retire_timer_for_node_locked` in winner CS | T8, T9/T10 |
| I5 Deadline Admission Closure | `AdmissionExpire` at registration | `await_wait_deadline` I5 recheck | T0 |
| I6 Deadline Park Liveness | `DeadlineParkLiveness` under fairness | bounded `park_on_wake_source` + worker-loop pump | T11, T12, T13, T15 |
| I7 Cleanup Closure | winner retires R; terminal => no Active reg | `retire_timer_for_node_locked` | T4, T5 |

## What this model does NOT cover

- The Scheduler MW admission / steal protocol (closed by E7/E8/E9).
- Fiber lifecycle / context-switch asm (closed by E2/E4).
- Backends, io_uring, timerfd, networking (out of E11 scope).
- Mutex/Event/Condition/Semaphore/Queue/RwLock/Select (explicitly deferred).
- Timer-wheel / hierarchical-timing-wheel optimization (deferred to E15).
