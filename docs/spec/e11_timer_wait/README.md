# E11 Deadline / Timer Wait Integration — Formal Model

Narrow TLA+ model of the E11 deadline/timer wait integration (sluice-CORE-E11),
extending the E10 WaitNode protocol
([`docs/spec/e10_waitnode/`](../e10_waitnode/)) with a THIRD resolution cause
`TIMER_EXPIRE` and the three new state dimensions E11 introduces over E10:

```text
timer-registration lifetime   (TimerRegistration control-block state:
                               ACTIVE / RETIRED / CONSUMED)
deadline park liveness        (monotonic time + deadline-due predicate +
                               Scheduler parked/executable state)
wait admission phase          (NoAdmission / AdmissionOpen / Suspended) — the
                               final-admission-decision boundary that closes I5
                               (Deadline Admission Closure). Without it I5 can
                               only be stated as P => TRUE (a tautology); this
                               dimension makes I5 non-vacuous.
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
  liveness + wait-admission-phase boundary). Preserves I1–I7.
- `E11TimerWait.cfg`                            — TLC config (safety invariants).
- `E11TimerWaitLiveness.cfg`                    — TLC config (I6 park liveness).
- `E11TimerWaitNeg1DoublePublication.tla/.cfg`  — NEG-1: timer expiry with no
  winner CAS -> double publication. Baseline is the E11 model BEFORE the I5
  admission-phase dimension (no admissionPhase/suspendedDue); omits the
  admission-closure state but is structurally equivalent for the double-
  publication defect (NEG-1 does not touch the admission boundary).
- `E11TimerWaitNeg2TimerCancelDoublePublication.tla/.cfg` — NEG-2: cancel with
  independent completion authority -> double publication. Same pre-I5 baseline
  as NEG-1; omits admission-phase state, which is orthogonal to the
  cancel-vs-timer race NEG-2 targets.
- `E11TimerWaitNeg3StaleCrossEpoch.tla/.cfg`    — NEG-3: stale timer keys on a
  reusable storage slot -> resolves epoch E+1 (the address-reuse boundary).
- `E11TimerWaitNeg4CallbackAfterRetirement.tla/.cfg` — NEG-4: node destroyed
  while a bound registration is still ACTIVE -> callback outlives the node (I4).
- `E11TimerWaitNeg5DeadlineLostParked.tla/.cfg` — NEG-5: Scheduler parks past a
  due deadline -> deadline lost, wait unresolved forever (I6 liveness).
- `E11TimerWaitNeg6DeadlineLostAtAdmission.tla/.cfg` — NEG-6: the final
  admission decision commits suspension even when the deadline is already due ->
  the wait is parked on an already-due deadline (I5 violation).
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
admissionPhase[n] : NoAdmission/AdmissionOpen/Suspended (I5 admission boundary)
suspendedDue[n]   : deadline-due fact recorded at the final admission decision
```

## Results

**TLC WAS EXECUTED.** The gate runs against the TLC version actually reported by
the jar — currently `Version 2026.07.09.134028 (rev: 227f61b)` from
`tla2tools.jar` (fetched from the TLA+ GitHub release). The reproducible runner
is `scripts/verify-e11-formal.sh` (M8); it asserts every model's expected
verdict AND that each negative model violates its EXPECTED NAMED property (not
just any generic "Invariant violated"). To run it yourself:

```bash
# one-time: fetch the jar the E7-E10 READMEs also expect at /tmp/tla2tools.jar
curl -sSL -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
# then:
scripts/verify-e11-formal.sh
```

Actual executed results (all models exhaustive over the finite domain
Nodes={N0,N1}, Regs={R0,R1}, Time=0..3, DeadlineVal=0..3):

| model | config | result | states (distinct) | depth | counterexample |
|-------|--------|--------|-------------------|-------|----------------|
| `E11TimerWait` | `E11TimerWait.cfg` | all invariants PASS | 13528 | 11 | — |
| `E11TimerWait` | `E11TimerWaitLiveness.cfg` | `DeadlineParkLiveness` PASS | 13528 | 11 | — |
| `E11TimerWaitNeg1DoublePublication` | `.cfg` | `InvSingleResolutionWinner` FAIL | 144 | 4 | timer publishes without the resolve_ CAS, then wake publishes again: `resolvedCount[N0]=2` |
| `E11TimerWaitNeg2TimerCancelDoublePublication` | `.cfg` | `InvSingleResolutionWinner` FAIL | 133 | 4 | cancel publishes without the resolve_ CAS, then timer/admission publishes again: `resolvedCount[N0]=2` |
| `E11TimerWaitNeg3StaleCrossEpoch` | `.cfg` | `InvWaitEpochIsolation` FAIL | 1395 | 6 | stale slot-keyed timer for N0 resolves N1 (N1 reused N0's slot): `regState[R0]=Consumed /\ nodeState[N0]#Expired` |
| `E11TimerWaitNeg4CallbackAfterRetirement` | `.cfg` | `InvTimerLifetimeClosure` FAIL | 129 | 4 | non-timer winner does not retire the reg; node destroyed while reg ACTIVE: `nodeAlive[N0]=FALSE /\ regState[R0]=Active` |
| `E11TimerWaitNeg5DeadlineLostParked` | `.cfg` | `DeadlineParkLiveness` FAIL | 10888 | 11 | parked forever with a due deadline (Tick + pump disabled while parked, no Unpark) |
| `E11TimerWaitNeg6DeadlineLostAtAdmission` | `.cfg` | `InvDeadlineAdmissionClosure` FAIL | 25 | 3 | admission commits suspension while the deadline is already due: `admissionPhase[N0]=Suspended /\ suspendedDue[N0]=TRUE /\ nodeState[N0]=Registered` |

### I5 admission-closure corrective (the load-bearing formal fix)

The original I5 invariant was a **tautology** — `(...) => TRUE` — so the formal
gate contributed zero checking power for Deadline Admission Closure while the
documentation claimed the corrected model preserved I1–I7. The corrective adds
the wait-admission-phase state dimension (`admissionPhase`:
NoAdmission/AdmissionOpen/Suspended) and a history fact `suspendedDue[n]`
recording the deadline-due predicate value at the final admission decision, then
states I5 non-tautologically:

```tla
InvDeadlineAdmissionClosure ==
    \A n \in Nodes :
        admissionPhase[n] = "Suspended" => ~suspendedDue[n]
```

I5 is **non-vacuous**: TLC reaches both admission branches and the
post-suspension timer path (witnessed by inverse-invariant counterexamples —
see the corrective commit report). It does NOT reject the legitimate
post-suspension state (suspended while not due, then time advances and the
deadline becomes due): `suspendedDue` is frozen at the `CommitSuspend` step and
does not track later time progress, so a node suspended while not-due stays
`suspendedDue=FALSE` even after its deadline becomes due (that later-due state
belongs to I6 timer-progress semantics). NEG-6 demonstrates the I5 defect the
old tautology could never surface.

### I3 strengthening (carried alongside the I5 authority reopening)

I3 was classified as weak (`regState[r]="Consumed" => regEpoch[r] \in Nodes`,
primarily proving a model value belongs to its declared domain). It is
strengthened to the semantic law "a timer registration may resolve only its
immortally bound logical wait epoch": `regState[r]="Consumed" =>
nodeState[regEpoch[r]]="Expired"` — a Consumed registration's bound epoch is the
node that became Expired. NEG-3 (slot-keyed cross-epoch expiry) violates this
directly: R0 (bound to N0) is Consumed while `nodeState[N0]="Woken"` (resolved
by wake, not the timer), so the strengthened I3 fires without weakening I1.

**Corrective history.** The first-committed E11 models did NOT parse or were
vacuous/non-counterexample-producing under real TLC: the correct model had an
invalid `EXCEPT ![r \in Regs |-> ...]` subscript and a `SumResolvedCount`
forward reference; every model's `Register` required `nodeAlive[n]=TRUE` while
`Init` set all `nodeAlive=FALSE` (registration unreachable -> safety PASS
vacuous, NEG counterexamples unreachable); the liveness property used the `~>`
form TLC rejects as a standalone PROPERTY wrapped under `[]`-fairness; and the
negative models' defects were unreachable because retirement was atomic with
the resolve. These were all corrected so the gate is now actually executed.
A later formal-closure review then found I5 was still a tautology (`P => TRUE`)
and I3 merely checked domain-membership; this corrective adds the
admission-phase dimension, the non-tautological I5, NEG-6, the strengthened I3,
and hardens `verify-e11-formal.sh` to assert each negative model's EXPECTED
NAMED property. The E10 README's claim that "§12 permits an executable fallback
when TLC is unavailable" is withdrawn — no such §12 exists in the E10 spec (it
ends at §11), and M8 requires the gate be runnable from committed artifacts
(which it now is, via `scripts/verify-e11-formal.sh`).

TLC IS run (see Results above + `scripts/verify-e11-formal.sh`). The formal
gate and the **refinement map + explicit causal proof below** are complementary:
the gate proves the finite model preserves I1–I7 and that each broken variant
has a reachable defect; the refinement map ties each formal concept to its
production authority; the deterministic production tests in
`tests/e11_timer_wait_test.cpp` (T0–T17) exercise the production seams under
ASan/UBSan. The causal proof below carries the I3/I4 argument that does not
reduce to a finite-state sweep (object-identity vs numeric-address reuse).

## Refinement map (TLA+ → production)

| Formal concept/action | Production path / seam | authority / domain |
| --------------------- | ---------------------- | ------------------ |
| `nodeState` | `WaitNode::state_` (atomic `State`) | winner authority |
| `Register(n)` + bind `R` | `Scheduler::await_wait_deadline` → `register_wait_locked` + `timer_pool_.emplace_back` + `heap_push_locked` under `global_mtx_` + `q.mtx()` | structural + timer registration |
| `AdmissionExpire(n)` | `await_wait_deadline` I5 recheck → `expire_locked` inline (clock >= deadline); admission phase closes without suspension | winner authority (same CS) |
| `CommitSuspend(n)` | `await_wait_deadline` final branch → `Fiber::make_waiting` (deadline NOT due at the recheck); admission phase -> Suspended | suspension commit (I5 boundary) |
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
| I3 Wait-Epoch Isolation | `regState[r]="Consumed" => nodeState[regEpoch[r]]="Expired"` (a Consumed reg's bound epoch is the Expired node) | `TimerRegistration::node_` captures `WaitNode&` (object identity, never a reusable address) | T7; NEG-3 (same-slot reuse CE) |
| I4 Timer Lifetime Closure | `ResolveTimer` gates on `regState = Active`; `DestroyNode` requires retirement | `try_claim_expiry` before deref; `retire_timer_for_node_locked` in winner CS | T8/T9/T10 (retirement-state gate); NEG-4 (lifetime CE) |
| I5 Deadline Admission Closure | `admissionPhase[n]="Suspended" => ~suspendedDue[n]` (non-tautological; admission-phase dimension) | `await_wait_deadline` I5 recheck -> `expire_locked` inline, no `make_waiting` | T0; NEG-6 (suspend-on-due CE) |
| I6 Deadline Park Liveness | `DeadlineParkLiveness` under fairness | bounded `park_on_wake_source` + worker-loop pump | T11, T12, T13, T15 |
| I7 Cleanup Closure | winner retires R; terminal => no Active reg | `retire_timer_for_node_locked` | T4, T5 |

## What this model does NOT cover

- The Scheduler MW admission / steal protocol (closed by E7/E8/E9).
- Fiber lifecycle / context-switch asm (closed by E2/E4).
- Backends, io_uring, timerfd, networking (out of E11 scope).
- Mutex/Event/Condition/Semaphore/Queue/RwLock/Select (explicitly deferred).
- Timer-wheel / hierarchical-timing-wheel optimization (deferred to E15).
