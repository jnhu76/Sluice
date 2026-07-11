# E12-A Event — TLA+ Formal Model

> sluice::async::Event (sluice-CORE-E12-A, **E12-A-EVENT-CORRECTIVE-1**).
> Persistent manual-reset Event built on the closed E10/E11 wait substrate.
>
> Status: **CORRECTED — INDEPENDENT CLOSURE REVIEW REQUIRED.**

This directory contains the TLA+ / TLC formal model for the E12-A Event
synchronization primitive. The E12-A-EVENT-CORRECTIVE-1 refactor represents the
production fact that a set epoch is a MULTI-STEP SERIALIZED critical section
(store SET → drain each waiter → release), which the original single-atomic
`SetEvent` could not express.

## The load-bearing E12-A question

> Can persistent readiness be composed with E10 WaitQueue and E11 deadline
> semantics without duplicating the old `waiting_ready_` subsystem?

**Answer: YES.** Event owns a persistent `std::atomic<bool> set_` + a
`WaitQueue waiters_`. ALL Event operations (set, reset, wait admission, set's
drain) serialize under `Scheduler::global_mtx_` (the existing coordination
domain). The set epoch is a multi-step critical section (`StartSet` →
`DrainOne`* → `FinishSet`); `ResetEvent` and `Register` require the protocol to
be `Idle`, so they CANNOT complete while a set drain is active. This makes
`OLD_SET_WAKES_POST_RESET_WAITER` mechanically impossible: a waiter admitted
after the drain is not in the queue during the drain, and reset cannot
interleave mid-drain.

## Files

| File | Purpose |
|------|---------|
| `E12Event.tla` | Correct safety + liveness model (multi-step set protocol) |
| `E12Event.cfg` | Safety config (SPECIFICATION Spec, INVARIANT Inv) |
| `E12EventLiveness.cfg` | Liveness config (SPECIFICATION LivenessSpecFair, PROPERTY EventSetDrainLivenessNonVacuous) |
| `E12EventNeg1LostSet.tla` | NEG-EVENT-1: lost set during admission |
| `E12EventNeg1LostSet.cfg` | NEG-EVENT-1 config (INVARIANT InvEventAdmissionClosure) |
| `E12EventNeg2WakeOne.tla` | NEG-EVENT-2: wake-one strands waiter |
| `E12EventNeg2WakeOne.cfg` | NEG-EVENT-2 config (PROPERTY EventSetDrainLivenessNonVacuous) |
| `E12EventNeg3StaleSet.tla` | NEG-EVENT-3: old set wakes post-reset waiter |
| `E12EventNeg3StaleSet.cfg` | NEG-EVENT-3 config (INVARIANT InvSetEpochIsolation) |
| `E12EventNeg4ResetResolve.tla` | NEG-EVENT-4: reset resolves waiter |
| `E12EventNeg4ResetResolve.cfg` | NEG-EVENT-4 config (INVARIANT InvResetNonResolution) |

## Model domain

`Nodes = {N0, N1}`, exhaustive; `MaxGen = 2` (a model bound on the abstract
reset generation, NOT a production limit). Two nodes suffice to model the
stale-set/post-reset topology (Wold from the old epoch + Wnew admitted after
reset).

## State dimensions (E12-A-EVENT-CORRECTIVE-1)

| Variable | Type | Meaning |
|----------|------|---------|
| `nodeState` | `[Node -> NodeState]` | WaitNode lifecycle (Detached/Registered/Woken/Cancelled/Expired) |
| `linked` | `[Node -> BOOLEAN]` | Node is in the Event's WaitQueue |
| `resolvedCount` | `[Node -> 0..1]` | Terminal resolution count (E1: <= 1) |
| `wakeDispatched` | `Int` | Total runnable publications (E2) |
| `eventSet` | `{"UNSET","SET"}` | Persistent Event readiness state |
| `admissionPhase` | `[Node -> AdmissionPhase]` | Admission lifecycle (NoAdmission/AdmissionOpen/Suspended) |
| `admissionSawSet` | `[Node -> BOOLEAN]` | Was SET observed at admission? (E3) |
| `wokenBySetDrain` | `[Node -> 0..1]` | Woken by a SetEvent DRAIN (not admission)? |
| `protoPhase` | `{"Idle","SetDrain"}` | Global Event protocol owner (refinement of global_mtx_) |
| `resetGeneration` | `0..MaxGen` | Abstract monotonic reset generation (H1 refinement/history) |
| `registrationGeneration` | `[Node -> Nat]` | Reset generation at which node n registered (H1) |
| `activeSetGen` | `Int` | Reset generation observed by the active set epoch, or NoGen (H1) |
| `wakeEpochGen` | `[Node -> Int]` | Reset generation of the set epoch that woke n via DRAIN, or NoGen (H1) |
| `resolutionCause` | `[Node -> ResolutionCause]` | Cause written by every terminal-resolution action (J) |

The `resetGeneration` / `registrationGeneration` / `activeSetGen` /
`wakeEpochGen` variables are **refinement/history variables** (H1): they are
mechanically connected to real modeled actions (resetGeneration advances on
ResetEvent; activeSetGen is recorded on StartSet; registrationGeneration on
Register; wakeEpochGen on DrainOne) and clearly identified as refinement state.
They are NOT production fields — `global_mtx_` is the production serialization.

## Actions

| Action | Production refinement |
|--------|----------------------|
| `Register(n)` | `Scheduler::await_event_wait` admission (register + check); requires protoPhase=Idle |
| `AdmissionWake(n)` | `WaitQueue::wake_node_locked` (resolve_(Woken) at admission) |
| `CommitSuspend(n)` | `Fiber::make_waiting` + `context_switch` |
| `StartSet` | `Scheduler::event_set_broadcast` store SET + open epoch (acquire global_mtx_) |
| `DrainOne(n)` | `wake_wait_one_locked` (one drain step; records wakeEpochGen) |
| `FinishSet` | `event_set_broadcast` drain loop end + release global_mtx_ |
| `ResetEvent` | `Scheduler::event_reset` (store false); requires protoPhase=Idle |
| `ResolveCancel(n)` | `Scheduler::cancel_wait` (resolve_(Cancelled) + unlink) |

## Correct properties

| Property | Formal name | Meaning |
|----------|-------------|---------|
| E1 | `InvSingleResolutionWinner` | One wait epoch -> at most one terminal resolution |
| E2 | `InvSingleRunnablePublication` | One winning epoch -> at most one runnable publication |
| E3 | `InvEventAdmissionClosure` | A wait observing SET at admission resolves Woken inline (admissionSawSet => Woken) |
| E4 | `EventSetDrainLivenessNonVacuous` (liveness) | A Registered+Suspended node eventually becomes terminal |
| E5 | `InvResetNonResolution` | A terminal node's resolutionCause is never "Reset"; reset never creates a terminal resolution |
| E6 | `InvSetEpochIsolation` | A drain-woken node was registered at a generation <= the waking set epoch's generation (a post-reset waiter cannot be woken by an older set epoch) |

### E5 / E6 checking power (Correctives J / H3)

The E12-A-EVENT-CORRECTIVE-1 model strengthened the previously weak/vacuous
properties:

- **E5** (`InvResetNonResolution`): the `resolutionCause` history variable is
  written by EVERY modeled terminal-resolution action (AdmissionWake →
  AdmissionSet, DrainOne → SetBroadcast, ResolveCancel → Cancel). ResetEvent
  does NOT write a cause. A buggy reset that resolves a waiter would write
  "Reset", which the property forbids. This is real checking power, not a
  constant forced to pass (NEG-EVENT-4 confirms: a reset that writes "Reset" is
  caught).
- **E6** (`InvSetEpochIsolation`): replaces the vacuous
  `wokenBySetEpoch[n] <= 1` (a terminal Woken node is absorbing and could never
  exceed one regardless of staleness). The new law checks the actual semantic
  invariant: `wakeEpochGen[n] # NoGen => registrationGeneration[n] <=
  wakeEpochGen[n]`. A waiter admitted after a later reset
  (registrationGeneration > wakeEpochGen) cannot be woken by an older set
  epoch's drain. The `<=` (not `==`) form is correct: a waiter that survives a
  set/reset cycle (registered at gen 0, reset to gen 1, woken by the gen-1 set
  epoch) has registrationGeneration=0 <= wakeEpochGen=1 — a LEGAL wake.

## Negative models

| NEG | Single broken rule | Counterexample | Expected property | TLC result |
|-----|-------------------|----------------|-------------------|------------|
| NEG-EVENT-1 | `CommitSuspendBuggy` allows suspend while SET (no eventSet guard) | Registered + Suspended + eventSet=SET + protoPhase=Idle | `InvEventAdmissionClosure` | CEX (violated) |
| NEG-EVENT-2 | `SetEventBuggy` resolves only ONE Suspended node | W2 Registered + Suspended forever while SET | `EventSetDrainLivenessNonVacuous` | CEX (violated) |
| NEG-EVENT-3 | `RegisterBuggy`/`ResetBuggy` drop the protoPhase=Idle guard (lost serialization) | Wnew registrationGeneration=G1 woken by epoch G0 (G0<G1) | `InvSetEpochIsolation` | CEX (violated) |
| NEG-EVENT-4 | `ResetBuggy` resolves a Registered node + writes "Reset" cause | terminal node with resolutionCause="Reset" | `InvResetNonResolution` | CEX (violated) |

Each negative model differs from the correct model at exactly the production
serialization boundary / the one resolution cause. The verify script asserts
each negative model's EXPECTED NAMED property is the violated one, runs a
wrong-property gate (a defect must not be mis-attributed), and a compile-probe
gate (the raw WaitQueue bypass must fail to compile).

## Non-vacuity evidence (Corrective K)

For each final property, the model provides property-specific evidence:

| Property | Good premise/state reachable | Bad state expressible | Correct model prevents it | Negative/inherited evidence |
| -------- | ---------------------------- | --------------------- | ------------------------- | --------------------------- |
| E1 single resolution | Woken / Expired / Cancelled paths reachable | (a second resolution of a terminal node) | resolve_ CAS is single-shot; resolvedCount<=1 | NEG-1/2/3 still satisfy E1 (defect-specific); inherited E10 theorem |
| E2 single publication | publication count == 1 reachable | (a second publication for one winner) | make_runnable is the publication guard | NEG-1/2/3/4 still satisfy E2 |
| E3 admission closure | admission observes SET → Woken inline reachable | Registered+Suspended+SET+Idle (stranded) | CommitSuspend requires UNSET | NEG-EVENT-1 CEX (stranded while SET) |
| E4 persistent SET liveness | Suspended → eventually terminal reachable | Suspended forever while SET (wake-one) | StartSet/DrainOne/FinishSet fairness drains all | NEG-EVENT-2 CEX (stranded non-victim) |
| E5 reset non-resolution | terminal via wake/cancel reachable | terminal with cause "Reset" | ResetEvent writes no cause | NEG-EVENT-4 CEX (reset resolves) |
| E6 set-epoch isolation | post-reset Wnew woken by S2 reachable; old-epoch drain reachable | Wnew (gen G1) woken by old epoch (gen G0) | protoPhase serialization; registrationGeneration<=wakeEpochGen | NEG-EVENT-3 CEX (stale wake) |

Reachability evidence for the E6 stale topology in NEG-EVENT-3:
`StartSet` (activeSetGen=G0) → `ResetBuggy` (resetGeneration=G1, while drain
active) → `RegisterBuggy` (registrationGeneration=G1) → `CommitSuspend` →
`DrainOne(Wnew)` (wakeEpochGen=G0 < registrationGeneration=G1) → CEX.

E1/E2 cite the inherited E10/E11 single-resolution theorem; the Event refinement
still routes every resolution and publication through `WaitNode::resolve_` (one
authority), so the theorem's premises hold. NEG-EVENT-1 is NOT cited as generic
evidence for E1/E2 (it targets E3 specifically).

## Results (actual TLC execution)

TLC runtime version: `2026.07.09.134028 (rev: 227f61b)`.
TLA+ tools release tag (recorded separately): `v1.8.0`.

| Model | Result | States | Distinct | Depth |
|-------|--------|--------|----------|-------|
| E12Event safety (E1,E2,E3,E5,E6) | PASS | 5023 | 1928 | — |
| E12Event liveness (E4) | PASS | 5023 | 1928 | — |
| NEG-EVENT-1 LostSet | CEX (InvEventAdmissionClosure) | 76 | 43 | — |
| NEG-EVENT-2 WakeOne | CEX (EventSetDrainLivenessNonVacuous) | 158 | 65 | — |
| NEG-EVENT-3 StaleSet | CEX (InvSetEpochIsolation) | 295 | 141 | — |
| NEG-EVENT-4 ResetResolve | CEX (InvResetNonResolution) | 80 | 45 | — |

Plus: WRONG-PROPERTY gate OK (InvSetEpochIsolation not mis-flagged under a
wrong-property config); COMPILE-PROBE gate OK (raw WaitQueue bypass sealed).

Reproduce: `scripts/verify-e12-event-formal.sh`.

## What this model does NOT cover

- E11 timer/expiry state (the Event composes with E11 deadlines in production;
  the formal timer-lifetime properties are proven in the closed E11 model).
- MW admission/steal (E7-E9), Fiber lifecycle/asm (E2/E4).
- Backends / io_uring / timerfd / networking.
- Semaphore / Mutex / Condition / Queue / RwLock / Select (E12-B..F, deferred).
