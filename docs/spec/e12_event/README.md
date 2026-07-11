# E12-A Event — TLA+ Formal Model

> sluice::async::Event (sluice-CORE-E12-A). Persistent manual-reset Event
> built on the closed E10/E11 wait substrate.

This directory contains the TLA+ / TLC formal model for the E12-A Event
synchronization primitive. It proves that persistent readiness composes with
the E10 WaitQueue + E11 deadline semantics without duplicating the old
`waiting_ready_` subsystem.

## The load-bearing E12-A question

> Can persistent readiness be composed with E10 WaitQueue and E11 deadline
> semantics without duplicating the old `waiting_ready_` subsystem?

**Answer: YES.** Event owns a persistent `std::atomic<bool> set_` + a
`WaitQueue waiters_`. ALL Event operations (set, reset, wait admission, set's
drain) serialize under `Scheduler::global_mtx_` (the existing coordination
domain). The drain (`SetEvent`) is atomic with the `set_` store, so
`OLD_SET_WAKES_POST_RESET_WAITER` is mechanically impossible: a waiter admitted
after the drain is not in the queue during the drain.

## Files

| File | Purpose |
|------|---------|
| `E12Event.tla` | Correct safety + liveness model |
| `E12Event.cfg` | Safety config (SPECIFICATION Spec, INVARIANT Inv) |
| `E12EventLiveness.cfg` | Liveness config (SPECIFICATION LivenessSpecFair, PROPERTY EventSetDrainLivenessNonVacuous) |
| `E12EventNeg1LostSet.tla` | NEG-EVENT-1: lost set during admission |
| `E12EventNeg1LostSet.cfg` | NEG-EVENT-1 config (INVARIANT InvEventAdmissionClosure) |
| `E12EventNeg2WakeOne.tla` | NEG-EVENT-2: wake-one strands waiter |
| `E12EventNeg2WakeOne.cfg` | NEG-EVENT-2 config (PROPERTY EventSetDrainLivenessNonVacuous) |

## Model domain

`Nodes = {N0, N1}`, exhaustive. Two nodes suffice to model multi-waiter
broadcast (both wait, one may be stranded by the buggy drain).

## State dimensions

| Variable | Type | Meaning |
|----------|------|---------|
| `nodeState` | `[Node -> NodeState]` | WaitNode lifecycle (Detached/Registered/Woken/Cancelled/Expired) |
| `linked` | `[Node -> BOOLEAN]` | Node is in the Event's WaitQueue |
| `resolvedCount` | `[Node -> 0..1]` | Terminal resolution count (E1: <= 1) |
| `wakeDispatched` | `Int` | Total runnable publications (E2) |
| `eventSet` | `{"UNSET","SET"}` | Persistent Event readiness state |
| `admissionPhase` | `[Node -> AdmissionPhase]` | Admission lifecycle (NoAdmission/AdmissionOpen/Suspended) |
| `admissionSawSet` | `[Node -> BOOLEAN]` | Was SET observed at admission? (E3) |
| `wokenBySetEpoch` | `[Node -> 0..1]` | Woken by a SetEvent drain? (E6) |

## Actions

| Action | Production refinement |
|--------|----------------------|
| `Register(n)` | `Scheduler::await_event_wait` admission (register + check) |
| `AdmissionWake(n)` | `WaitQueue::wake_node_locked` (resolve_(Woken) at admission) |
| `CommitSuspend(n)` | `Fiber::make_waiting` + `context_switch` |
| `SetEvent` | `Scheduler::event_set_broadcast` (store + drain loop) |
| `ResetEvent` | `Scheduler::event_reset` (store false) |
| `ResolveCancel(n)` | `Scheduler::cancel_wait` (resolve_(Cancelled) + unlink) |

## Correct properties

| Property | Formal name | Meaning |
|----------|-------------|---------|
| E1 | `InvSingleResolutionWinner` | One wait epoch -> at most one terminal resolution |
| E2 | `InvSingleRunnablePublication` | One winning epoch -> at most one runnable publication |
| E3 | `InvEventAdmissionClosure` | A wait observing SET at admission resolves Woken inline (no suspend); a Suspended+Registered node implies UNSET |
| E4 | `EventSetDrainLivenessNonVacuous` (liveness) | A Registered+Suspended node eventually becomes terminal |
| E5 | `InvResetNonResolution` | Reset alone does not change a Registered node to terminal |
| E6 | `InvSetEpochIsolation` | A node is woken by at most one SetEvent drain epoch |

## Negative models

| NEG | Single broken rule | Counterexample | Expected property | TLC result |
|-----|-------------------|----------------|-------------------|------------|
| NEG-EVENT-1 | `CommitSuspendBuggy` allows suspend while SET (no eventSet guard) | Registered + Suspended + eventSet=SET | `InvEventAdmissionClosure` | CEX (violated) |
| NEG-EVENT-2 | `SetEventBuggy` drains only ONE Suspended node | W2 Registered + Suspended forever while SET | `EventSetDrainLivenessNonVacuous` | CEX (violated) |

## Non-vacuity evidence

The liveness property `EventSetDrainLivenessNonVacuous` has the antecedent
`nodeState[n]="Registered" /\ admissionPhase[n]="Suspended"`. This state is
reachable: `Register` -> `CommitSuspend` (while UNSET) produces it. The correct
liveness model verifies that from every such state, `SetEvent` (under fairness)
or `ResolveCancel` eventually resolves the node. NEG-EVENT-2 confirms
non-vacuity: with the buggy drain, a non-victim Suspended node remains
Registered forever, violating the liveness property.

| Semantic state | Reachable? | Evidence |
|----------------|-----------|----------|
| UNSET Event | YES | Init |
| SET Event | YES | SetEvent from Init |
| Registered Event waiter while UNSET | YES | Register |
| Late wait while SET | YES | Register when eventSet=SET (then AdmissionWake) |
| Admission open | YES | Register -> AdmissionOpen |
| Suspension committed while UNSET | YES | CommitSuspend |
| RESOURCE_WAKE result | YES | SetEvent drain / AdmissionWake |
| CANCEL result | YES | ResolveCancel |
| Two or more registered waiters | YES | Register(N0) + Register(N1) |
| SET with broadcast work outstanding | YES | SetEvent drains atomically (modeled directly) |
| Reset after set | YES | SetEvent -> ResetEvent |
| Post-reset waiter | YES | ResetEvent -> Register -> CommitSuspend |

## Results (actual TLC execution)

TLC version: `2026.07.09.134028 (rev: 227f61b)` (v1.8.0).

| Model | Result | States | Distinct | Depth |
|-------|--------|--------|----------|-------|
| E12Event safety (E1,E2,E3,E5,E6) | PASS | 263 | 85 | 8 |
| E12Event liveness (E4) | PASS | 263 | 85 | 8 |
| NEG-EVENT-1 LostSet | CEX (InvEventAdmissionClosure) | 33 | 20 | 4 |
| NEG-EVENT-2 WakeOne | CEX (EventSetDrainLivenessNonVacuous) | 133 | 51 | 9 |

Reproduce: `scripts/verify-e12-event-formal.sh`.

## What this model does NOT cover

- E11 timer/expiry state (the Event composes with E11 deadlines in production;
  the formal timer-lifetime properties are proven in the closed E11 model).
- MW admission/steal (E7-E9), Fiber lifecycle/asm (E2/E4).
- Backends / io_uring / timerfd / networking.
- Semaphore / Mutex / Condition / Queue / RwLock / Select (E12-B..F, deferred).
