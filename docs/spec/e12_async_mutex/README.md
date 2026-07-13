# E12-C AsyncMutex — TLA+ Formal Model (safety)

> sluice::async::AsyncMutex (first-scope Fiber-suspending async Mutex),
> **E12-C-ASYNC-MUTEX-PREPARATION-CORRECTIVE-5**.
>
> Status: **E12-C-PREPARATION-CORRECTIVE-5-REAUDIT: PASS — E12-C-PREPARATION:
> CLOSED — E12-C-IMPLEMENTATION: READY.** This model is the formal half of the
> E12-C preparation; the authority document is
> [`docs/e12-async-mutex.md`](../../e12-async-mutex.md).

This directory contains the **safety-only** TLA+ / TLC formal model for the
E12-C AsyncMutex preparation. It proves the single-ownership-authority,
FIFO-handoff, no-barging, owner-before-publication, grant/publication-coupling,
admission-closure, deadline-precedence, grant-finality, and
publication-discipline properties of the first-scope design, and provides eleven
negative models that each fail a single named invariant for the intended reason.

## The load-bearing E12-C question

> Can the first-scope Fiber-suspending Mutex's single-`owner_` authority, direct
> FIFO handoff (no free-lock window), owner-before-publication ordering,
> grant/publication coupling, collapsed-admission closure, deadline precedence,
> and grant finality be modelled so that every property is a TLC-checkable state
> invariant (no primed variables in invariants), with NO redundant `locked_`
> field, NO grant-in-flight state, and NO standalone registration action?

**Answer: YES** — by (a) modelling `owner : Fiber ∪ {NoOwner}` as the SOLE
ownership authority (no `locked_`/`reserved_owner_`/`pending_owner_`), (b)
collapsing each admission (register + recheck + disposition) into ONE atomic
action so a Registered-in-queue epoch IS a Suspended (make_waiting-committed)
epoch (no `admissionPhase` field), (c) carrying minimal ghost/history evidence
(`preOwner` = PREVIOUS ownership, NOT the acting Fiber; `lastAction`;
`expectedFIFOHead`) so transition properties become state predicates, (d)
latching admission evidence (`admissionSawFree` / `admissionSawDue`) atomically
with the resolution so `InvDeadlinePrecedence` is prime-free, and (e) making the
model safety-only (no scheduler-fairness liveness).

## Files

| File | Purpose |
|------|---------|
| `E12AsyncMutex.tla` | Correct safety model (single-owner authority, atomic admission, FIFO handoff, owner-before-publication coupling, history-backed transition invariants) |
| `E12AsyncMutex.cfg` | Safety config (SPECIFICATION Spec, INVARIANT Inv) |
| `E12AsyncMutexNegM1.tla/.cfg` | NEG-M1: non-owner unlock → `InvUnlockAuthority` |
| `E12AsyncMutexNegM2.tla/.cfg` | NEG-M2: recursive acquire → `InvRecursiveForbidden` |
| `E12AsyncMutexNegM3.tla/.cfg` | NEG-M3: non-FIFO grant → `InvFIFOGrant` |
| `E12AsyncMutexNegM4.tla/.cfg` | NEG-M4: barging → `InvNoBarging` |
| `E12AsyncMutexNegM5.tla/.cfg` | NEG-M5: grant without owner commit → `InvGrantOwnerCommit` |
| `E12AsyncMutexNegM6.tla/.cfg` | NEG-M6: publication without grant coupling → `InvGrantPublicationCoupling` |
| `E12AsyncMutexNegM7.tla/.cfg` | NEG-M7: admission closure failure → `InvAdmissionClosure` |
| `E12AsyncMutexNegM8.tla/.cfg` | NEG-M8: late cancel revokes handoff → `InvGrantFinality` |
| `E12AsyncMutexNegM9.tla/.cfg` | NEG-M9: late expire revokes handoff → `InvGrantFinality` |
| `E12AsyncMutexNegM10.tla/.cfg` | NEG-M10: immediate publication → `InvPublicationRequiresSuspensionOrHandoff` |
| `E12AsyncMutexNegM11.tla/.cfg` | NEG-M11: destruction while owned/queued → `InvDestructionPrecondition` |
| `_gen_neg.py` | Build aid that generates NEG-M1..NEG-M11 from `E12AsyncMutex.tla` by single-rule action substitution |

The gate is [`scripts/verify-e12-async-mutex-formal.sh`](../../../scripts/verify-e12-async-mutex-formal.sh).

## State model

Authoritative state (`docs/e12-async-mutex.md` §14.3):

```
owner : Fiber ∪ {NoOwner}          -- SOLE ownership authority (no locked_)
queue : Seq(Epoch)                 -- FIFO; only Registered (= Suspended) epochs
nodeState : Epoch -> {Detached, Registered, Woken, Cancelled, Expired}
epochFiber : Epoch -> Fiber        -- the Fiber that registered this epoch
deadlineDue : Epoch -> BOOLEAN     -- env-chosen: was deadline due at admission?
runnablePublished : Epoch -> BOOLEAN
resolutionCount : Epoch -> {0,1}
publicationCount : Epoch -> {0,1}
destroyed : BOOLEAN
```

History / ghost evidence (NOT production fields; exist only so transition
properties are state predicates): `lastAction`, `lastActor`, `lastTargetEpoch`,
`lastGrantedEpoch`, `preOwner` (PREVIOUS ownership, NOT the acting Fiber),
`preQueue`, `preNodeState`, `prePublished`, `prePublicationCount`,
`expectedFIFOHead`, `admissionSawFree`, `admissionSawDue`.

## Action catalog (Init + 16 behavior actions)

Non-epoch: `TryLockSuccess`, `TryLockFailure`.
Admission (each refines the ENTIRE register+recheck+disposition critical section
as ONE atomic step — no `admissionPhase` field): `LockImmediate`,
`LockAdmissionAcquire`, `LockAdmissionSuspend`, `LockUntilImmediate`,
`LockUntilAdmissionAcquire`, `LockAdmissionSuspend`, `LockUntilAdmissionSuspend`,
`LockUntilDue`.
Unlock: `UnlockNoWaiter`, `UnlockHandoff` (owner commit BEFORE publication).
Suspended-waiter resolution: `CancelSuspended`, `ExpireSuspended`.
Late terminal attempts (make `InvGrantFinality` non-vacuous):
`CancelAttemptTerminal`, `ExpireAttemptTerminal`.
Destruction: `Destroy`.

## Publication discipline (M9)

Only `UnlockHandoff`, `CancelSuspended`, `ExpireSuspended` create a runnable
publication. Immediate / admission-acquire / terminal-attempt / no-waiter /
Destroy do NOT publish. `InvPublicationRequiresSuspensionOrHandoff` checks that
WHEN the last action is a publishing action, the targeted epoch received exactly
one fresh publication (flag FALSE→TRUE, count pre+1), and that non-publishing
actions leave all publicationCounts unchanged.

## Properties (19 invariants)

7 state invariants: `InvType`, `InvQueueWellFormed`, `InvSingleResolution`,
`InvSinglePublication`, `InvPublicationConsistency` (`runnablePublished[e] <=>
publicationCount[e]=1`), `InvNoOwnerlessQueuedDemand` (`owner=NoOwner =>
EligibleQueue=<<>>` — one-way; an owned Mutex MAY have an empty queue),
`InvPublishedEpochTerminal`.

12 history-backed transition properties: `InvUnlockAuthority`,
`InvRecursiveForbidden`, `InvFIFOGrant`, `InvEligiblePreQueue` (handoff-only),
`InvNoBarging`, `InvAdmissionFIFO`, `InvGrantOwnerCommit`,
`InvGrantPublicationCoupling`, `InvAdmissionClosure`, `InvDeadlinePrecedence`,
`InvGrantFinality`, `InvPublicationRequiresSuspensionOrHandoff`,
`InvDestructionPrecondition`.

## Negative models

Each NEG introduces exactly ONE focused defect in ONE action and fails exactly
ONE expected named invariant, from a reachable valid state (avoiding unrelated
malformed initialization). `_gen_neg.py` is the generator (a build aid; the
generated `.tla`/`.cfg` files are the committed authority).

## Refinement note (production)

The production admission critical section (`Scheduler::mutex_lock` /
`mutex_lock_until`) holds `global_mtx_` + `waiters_.mtx()` across
register + recheck + (inline-Woken | inline-Expired | make_waiting); only
`context_switch` is outside the lock. Therefore the formal model MUST NOT expose
a Registered-but-not-Suspension-committed queue member — a Registered epoch in
the queue IS a Suspended epoch. `MUTEX-HANDOFF-ONE` (`mutex_unlock`) commits
`owner = winner` BEFORE `make_runnable` / `route_runnable_locked`, satisfying
the owner-before-publication refinement obligation (M4/M5).

## Scope

This is a SAFETY-ONLY model. The permitted progress statement remains
conditional and is NOT asserted here: eligible queued waiters cannot be
overtaken by later arrivals, assuming the owner eventually unlocks and the
Scheduler eventually runs runnable Fibers.
