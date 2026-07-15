# E12-D — Async Condition (Preparation Corrective-1)

> Status:
> ```text
> E12-D-CONDITION-PREPARATION-AUDIT-1: CORRECTIVE-COMPLETE
> E12-D-CONDITION-PREPARATION-CORRECTIVE-1: COMPLETE
> E12-D-PREPARATION: REVIEW-REQUIRED
> E12-D-IMPLEMENTATION: BLOCKED
> ```
>
> Authority baseline:
> - E10 CLOSED ([`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md))
> - E11 CLOSED (`7715808`, [`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md))
> - E12-A Event CLOSED ([`docs/e12-event.md`](e12-event.md))
> - E12-B Semaphore CLOSED ([`docs/e12-semaphore.md`](e12-semaphore.md))
> - E12-C AsyncMutex IMPLEMENTATION CLOSED
>   ([`docs/e12-async-mutex.md`](e12-async-mutex.md);
>   E12-C-IMPLEMENTATION-1-INDEPENDENT-REVIEW: PASS,
>   E12-C-IMPLEMENTATION: CLOSED)
> - E12-C: CLOSED
>
> Cross-primitive preparation:
> [`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md) §7.
>
> This document is the authoritative E12-D AsyncCondition preparation
> specification. It records the CLOSED human policy register, corrects the
> structural overclaims from audit-1, and establishes the complete formal,
> property, negative-model, and test preparation.

---

## 1. Authoritative human policy register (CLOSED)

The following are CLOSED by this corrective for E12-D first scope. No
load-bearing human policy remains open.

| ID | Decision | Resolution |
|----|----------|------------|
| C-H1 | **Return ownership** — Model A: wait returns only after the same Fiber reacquires the bound AsyncMutex. | **CLOSED** |
| C-H2 | **Mutex association** — AsyncCondition is bound to exactly one AsyncMutex at construction. | **CLOSED** |
| C-H3 | **Reacquire mechanism** — wake from Condition, then ordinary AsyncMutex lock/reacquire. No wait morph in first scope. | **CLOSED** |
| C-H4 | **Deadline** — deadline governs only the Condition wait epoch. | **CLOSED** |
| C-H5 | **Reacquire cancellation** — mandatory reacquire is untimed and non-cancellable. | **CLOSED** |
| C-H6 | **notify_all** — included in first scope. | **CLOSED** |
| C-H7 | **WaitNode API** — caller supplies one Condition WaitNode; the reacquire WaitNode is local to the active Fiber wait call. | **CLOSED** |
| C-H8 | **Reacquire ordering** — ordinary AsyncMutex FIFO-tail ordering; no notified priority. | **CLOSED** |
| C-H9 | **Wait morph** — deferred optimization. | **CLOSED** |
| C-H10 | **notify_all mechanism** — atomic Condition snapshot/drain within one Scheduler coordination critical section. | **CLOSED** |

---

## 2. Scope

### Implemented (IN scope for first-scope AsyncCondition)

```text
Condition wait epoch (WaitQueue of waiting Fibers)
notify_one (wake the eligible FIFO waiter)
notify_all (atomic snapshot-and-drain of all eligible waiters)
Mutex reacquire epoch (mandatory, untimed, non-cancellable)
lost-notify closure (register Condition BEFORE release Mutex, under one lock domain)
deadline on the Condition wait epoch only
cancel on the Condition wait epoch only
one caller-provided Condition WaitNode
stack-local reacquire WaitNode
```

### Explicitly excluded (DEFERRED / OUT of scope)

```text
wait-morph (DEFERRED — C-H9)
Select / multi-wait (E13 — Condition is sequential single-waits)
Spurious wake as a first-class semantic (no — all wakes are real resolves)
notified priority reacquire (DEFERRED — C-H8)
deadline or cancel on reacquire epoch (excluded by C-H4, C-H5)
public reacquire node, wait_queue(), mutex() accessor (frozen API, §5)
generic ConditionBase, Lockable, AwaitableLock, or grant framework
```

---

## 3. Protocol overview

Every `wait` or `wait_until` call on the Condition creates TWO sequential single
wait epochs:

```text
Hold Mutex
    ↓
[atomic under global_mtx_]                              ─┐
    register Condition WaitNode into Condition queue      │ register BEFORE
    (optionally install timer for wait_until)              │ release —
    release Mutex:                                        │ lost-notify
        queue empty -> owner_ = NoOwner                    │ closure (§7)
        queue non-empty -> FIFO direct handoff             │
    make_waiting()                                         │
    ↓                                                     ─┘
... Condition wait epoch (resolved by notify/expire/cancel) ...
    ↓
wait resolves — Fiber resumes WITHOUT ownership
    ↓
[mandatory reacquire epoch]
    stack-local WaitNode
    mtx_.lock(reacquire_node)  — ordinary FIFO-tail admission
    ↓
... Mutex reacquire epoch (resolved by handoff; NOT cancellable, NOT timed) ...
    ↓
caller resumes holding Mutex
return value = latched Condition outcome (Woken/Expired/Cancelled)
```

These are two **sequential** single-waits, NOT a multi-wait (E13). No Fiber ever
waits on both simultaneously — the Condition wait is registered BEFORE the Mutex
is released. **RESOLVED: Condition does not require E13.** After the Condition
epoch resolves, the reacquire epoch is a separate single-wait that uses the
existing AsyncMutex admission protocol.

---

## 4. WaitNode design — one caller node, one stack-local node

### 4.1 The single-shot constraint

E10 establishes a **single-shot** WaitNode lifecycle
([`e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md) §2):

```text
Detached ──register──> Registered ─┬─resolve(Woken)─────> Woken      [terminal]
                                   ├─resolve(Cancelled)─> Cancelled [terminal]
                                   └─resolve(Expired)───> Expired   [terminal]
```

Terminal states are **absorbing**. No `reset()` or `rearm()`. A single WaitNode
cannot represent two sequential wait epochs.

### 4.2 Corrected consequence

The two-epoch Condition protocol requires TWO distinct WaitNode instances:

| Epoch | WaitQueue | WaitNode origin | Resolver |
|-------|-----------|-----------------|----------|
| Condition wait | Condition private queue | **caller-provided** (in await frame) | notify_one / expire / cancel |
| Mutex reacquire | AsyncMutex private waiters_ | **stack-local** (inside wait method) | unlock handoff (only — non-cancellable) |

The public API accepts exactly **one** caller-provided Condition WaitNode
(C-H7). The mandatory reacquire phase creates a **stack-local** WaitNode inside
the active Fiber's wait call. The Fiber is stackful, so this local object
remains alive across the reacquire suspension and is destroyed only after the
reacquire returns terminal.

This is safe because:

```text
the caller cannot cancel or expire the mandatory reacquire epoch (C-H5);
no internal Condition-object member node (no cross-call sharing);
no heap allocation (stack-local in a stackful Fiber);
no reentrancy defect (each wait call has its own stack frame);
node lifetime is bounded by the wait call duration.
```

The reacquire node is not exposed through any public API — there is no
`reacquire_node()`, no way to cancel it, no way to time it out.

### 4.3 Wait morph (DEFERRED)

Wait-morph (direct Condition→Mutex queue transfer without waking the fiber) is
deferred (C-H9). The first-scope design uses wake-then-ordinary-lock
reacquire (C-H3). Wait-morph can be added later as a drop-in optimization
without changing the public API.

---

## 5. Frozen public API

First-scope authority:

```cpp
class AsyncCondition {
public:
    explicit AsyncCondition(AsyncMutex& mutex) noexcept;
    ~AsyncCondition();

    AsyncCondition(const AsyncCondition&) = delete;
    AsyncCondition& operator=(const AsyncCondition&) = delete;
    AsyncCondition(AsyncCondition&&) = delete;
    AsyncCondition& operator=(AsyncCondition&&) = delete;

    [[nodiscard]] WaitOutcome wait(WaitNode& condition_node);

    [[nodiscard]] WaitOutcome wait_until(
        WaitNode& condition_node,
        Scheduler::deadline_t deadline);

    [[nodiscard]] bool cancel(WaitNode& condition_node);

    void notify_one();
    void notify_all();
};
```

Private conceptual state:

```text
AsyncMutex& mutex_
Scheduler& scheduler_
WaitQueue waiters_
```

`AsyncCondition` is bound to one AsyncMutex and therefore one Scheduler (C-H2).

Not in the public API:

```text
mutex()
wait_queue()
waiting_count()
notify_n()
reacquire_node()
```

No generic ConditionBase, Lockable, AwaitableLock, or grant framework.

---

## 6. Corrected lock topology and lost-notify protocol

### 6.1 The lost-notify window

The classic condition-variable race: a `notify` that fires between Mutex release
and Condition registration is irrecoverably lost. The correction is:
**register the Condition node BEFORE releasing the Mutex.**

### 6.2 Protocol order (corrected)

```text
under global_mtx_:

    1. verify current Fiber owns the bound AsyncMutex
    2. lock Condition queue
    3. register Condition node (Detached -> Registered)
    4. register timer when wait_until uses a future deadline
    5. unlock Condition queue

    6. lock AsyncMutex queue
    7. release owner:
           queue empty -> owner_ = NoOwner
           queue non-empty -> reuse direct FIFO ownership handoff
             (mutex_handoff_one_locked or equivalent)
    8. unlock AsyncMutex queue

    9. make current Fiber Waiting
    10. release global_mtx_
    11. context_switch
```

A notify cannot interleave between steps 3 and 7 because all Condition
notify/cancel/expire paths also require `global_mtx_`. If a notify holds
`global_mtx_` before this wait prepare, the wait serializes after it
(sees the notify's effect or the notify sees the registered node). If the
notify runs after the wait releases `global_mtx_`, it sees the registered node.

### 6.3 Sequential queue lock topology (corrected)

The queue locks are NOT held simultaneously. The topology is:

```text
global_mtx_
    -> Condition queue mutex
release Condition queue mutex
    -> AsyncMutex queue mutex
release AsyncMutex queue mutex
```

There is no Condition-queue-to-Mutex-queue or Mutex-queue-to-Condition-queue
lock edge. The combined seam is atomic because `global_mtx_` remains held
throughout.

**Audit interaction with `mutex_handoff_one_locked`:** that helper acquires
`waiters_.mtx()` internally. The implementation design must not reacquire a
Mutex queue lock already held. A narrow caller-lock-held variant (the caller
already holds `waiters_.mtx()` and passes it) may be required — this is an
IMPLEMENTATION BOUNDARY, not a human semantic decision.

---

## 7. Combined seam classification (CONDITION-WAIT-PREPARE)

The combined seam requirement is **CLOSED**.

Semantic name: `CONDITION-WAIT-PREPARE`.

Required effects under `global_mtx_`:

```text
current Fiber owns bound AsyncMutex
Condition node is Detached

atomic:
    register Condition node
    optionally install timer (for wait_until)
    release / direct-handoff bound AsyncMutex
    commit current Fiber to Waiting
```

The seam must reuse the accepted AsyncMutex direct-handoff authority (M-H1 from
E12-C). It must NOT duplicate handoff logic. It must NOT add a generic
release-and-wait framework.

The exact C++ signature is an **IMPLEMENTATION BOUNDARY**, not a human semantic
decision. The seam surface is private to Scheduler.

---

## 8. notify_one authority

```text
non-persistent
empty queue -> no-op
resolves exactly the eligible FIFO head registered at its linearization point
late waiter (registered after notify) is not affected
one Condition publication (exactly once)
```

The selected waiter subsequently performs ordinary Mutex reacquire (C-H3).

Condition FIFO notification order does NOT guarantee Mutex return order — the
reacquire uses ordinary FIFO-tail Mutex admission (C-H8). A notified waiter may
reacquire the Mutex after a non-notified locker if the locker queued earlier on
the Mutex.

---

## 9. notify_all authority

Included in first scope (C-H6).

Required semantic:

```text
one notify_all linearization point
snapshot = every eligible Registered Condition waiter at that point
all snapshot waiters resolve Woken exactly once
waiters registered after that point are excluded
```

Required mechanism:

```text
one Scheduler-private Condition notify-all operation
    hold global_mtx_
    hold Condition queue mutex
    loop wake_one_locked / resolve / unlink over the captured queue contents
    publish each winner
    release locks
```

The loop-and-drain within one critical section is the atomic snapshot. No
public-loop implementation. No generic wake-many framework (unless an existing
private Event helper is proven semantically identical and can be reused without
widening public authority).

---

## 10. Deadline semantics

### 10.1 Condition wait epoch deadline (C-H4)

When a deadline is supplied to `wait_until`, it governs ONLY the Condition wait
epoch. The epoch resolves when exactly one of the following wins the resolve_
CAS:

- `notify_one` / `notify_all` → Woken (RESOURCE_WAKE)
- Timer expiry → Expired (TIMER_EXPIRE)
- `cancel` → Cancelled (CANCEL)

Timer registration follows E11 protocol: a `TimerRegistration` is created under
`global_mtx_` + the Condition WaitQueue's `mtx_`, and the deadline competes
through the same `pump_deadlines_locked` path as every other timed wait.

**RESOLVED (by E11 authority):** Condition wait timer registration follows the
same resource-first precedence — deadline-already-due at admission resolves
Expired before suspension; a non-timer winner retires the registration.

### 10.2 Due-inline Expired retains ownership

For an already-due deadline at admission:

```text
under global_mtx_ + Condition queue mutex:
    register Condition node
    resolve Expired inline
    unlink
    do NOT release Mutex
    do NOT suspend
    do NOT create reacquire epoch
return Expired while still owning the Mutex
```

### 10.3 The deadline never governs reacquire

The original deadline never governs the mandatory reacquire epoch. A deadline
passing after Woken/Cancelled/Expired wins cannot change the latched Condition
result.

---

## 11. Cancellation authority

### 11.1 Condition wait epoch cancellation

`cancel(condition_node)`:

```text
callable from any thread
targets only a Registered node in THIS Condition queue
returns true only when Cancelled CAS wins
returns false for wrong Condition, detached, Woken, Expired, Cancelled, repeated cancel
```

**RESOLVED:** Cancel follows the established E10 cancellation contract.

### 11.2 Reacquire epoch is non-cancellable (C-H5)

After any terminal Condition outcome (Woken/Expired/Cancelled), Condition
cancel cannot affect the mandatory reacquire epoch. The function returns only
when `mutex_owner == current Fiber`. Late Condition cancel/expire attempts
cannot alter the result or affect the reacquire node.

No persistent cancellation token or masking mechanism is required — the
cancellation authority is node-specific. The Condition node is terminal; no
second cancellation on the reacquire node is possible because the Condition
never exposes the reacquire node to `cancel()`.

---

## 12. Mandatory reacquire protocol (C-H1, C-H3, C-H5)

After the Condition node resolves with reason `r ∈ {Woken, Expired, Cancelled}`,
the Fiber resumes WITHOUT ownership and executes:

```cpp
WaitNode reacquire_node;
mutex_.lock(reacquire_node);
```

The reacquire epoch is:

```text
mandatory (C-H1)
untimed (C-H4)
non-cancellable (C-H5)
ordinary FIFO-tail AsyncMutex admission (C-H8)
```

The function returns only when `mutex_owner == current Fiber`. The return value
is the **latched** Condition reason `r`. Late Condition cancel/expire attempts
cannot alter the latched result (the Condition node is already terminal).

---

## 13. Wait-phase state machine

### 13.1 States

```text
RunningOwned
ConditionWaiting
ConditionResolved(reason)
ReacquireAdmission
MutexWaiting
ReturnedOwned(reason)
```

No stable public `ConditionAdmission` state is exposed; registration, Mutex
release, and `make_waiting` refine one atomic Scheduler action.

### 13.2 Safety statements

```text
ConditionWaiting => mutexOwner != waiting Fiber
ReturnedOwned(F, reason) => mutexOwner = F
ConditionResolved reason is final
Condition node and reacquire node are never registered simultaneously
Fiber may be published once for the Condition resolution and once for the Mutex
    reacquire (two distinct publications within one wait call)
Fiber is never published twice within either epoch
```

---

## 14. Destruction and lifetime

```text
destroy with Registered Condition waiter  -> caller contract violation
destroy while any wait call has not returned -> caller contract violation
destroy does NOT notify, cancel, force-reacquire, or release the Mutex
```

First scope needs an `active_waits_` debug/lifetime counter. Classification:

```text
IMPLEMENTATION BOUNDARY:
    either mechanically assert active_waits_ == 0 in ~AsyncCondition
    or document why existing queue/lifetime assertions are sufficient
```

The reacquire-phase destruction case must not be silently ignored. If a caller
destroys the AsyncCondition while a reacquire epoch is in progress, the
reacquire node lives in the AsyncMutex's queue — the Mutex may later hand off
to a destroyed Condition node. The `active_waits_` counter (or equivalent)
prevents this.

---

## 15. Formal-model preparation

### 15.1 State

Model one bound AsyncMutex.

```text
mutexOwner : Fiber | NoOwner
mutexQueue : Seq(ReacquireEpoch)
conditionQueue : Seq(ConditionEpoch)

conditionNodeState[F] : { Detached, Registered, Woken, Expired, Cancelled }
reacquireNodeState[F] : { Detached, Registered, Woken }

waitPhase[F] : {
    Idle,
    ConditionWaiting,
    ConditionResolved,
    ReacquirePending,
    MutexWaiting,
    Returned
}

conditionReason[F] : { None, Woken, Expired, Cancelled }

conditionResolutionCount[F] : Nat
conditionPublicationCount[F] : Nat
reacquireResolutionCount[F] : Nat
reacquirePublicationCount[F] : Nat

notifyAllSnapshot : Set(Fiber)
preConditionQueue : Seq(ConditionEpoch)
preMutexQueue : Seq(ReacquireEpoch)
deadlineDue : Bool
destroyed : Bool
```

### 15.2 Actions

```text
WaitDueInline
    — deadline already due at admission; resolve Expired, retain Mutex

WaitAdmissionSuspend
    — register Condition node + timer, release Mutex/handoff, suspend

NotifyOne
    — resolve FIFO head Woken, publish

NotifyAll
    — snapshot eligible, loop-resolve Woken, publish each

CancelCondition
    — resolve node Cancelled, publish

ExpireCondition
    — timer expires; resolve node Expired, publish

ConditionTerminalAttempt
    — late cancel/expire against already-terminal node (loser)

BeginReacquire
    — ConditionResolved -> ReacquirePending (Fiber resumes)

ReacquireImmediate
    — Mutex admissible at admission; inline Woken

ReacquireSuspend
    — Mutex not admissible; register + suspend

MutexUnlockNoWaiter
    — unlock, owner_ = NoOwner

MutexUnlockHandoff
    — unlock, direct FIFO head handoff

ReturnOwned
    — reacquire complete; return to caller

Destroy
    — destroy AsyncCondition (must be empty, no active waits)
```

### 15.3 FIFO queues, not sets

Condition and Mutex queues are modeled as `Seq` to enforce FIFO ordering.
`NotifyOne` selects `head(conditionQueue)`. `NotifyAll` snapshots the current
queue sequence at its linearization point.

Do not model FIFO queues as sets. Do not classify eventual departure as a safety
invariant.

---

## 16. Property preparation

### 16.1 State invariants

```text
InvConditionQueueWellFormed
    — every Fiber in conditionQueue has conditionNodeState = Registered
    — no duplicate fibers in conditionQueue

InvMutexQueueWellFormed
    — every Fiber in mutexQueue has reacquireNodeState = Registered
    — no duplicate fibers in mutexQueue

InvSingleConditionResolution
    — each Fiber's conditionNodeState transitions to terminal exactly once

InvSingleConditionPublication
    — each Fiber is made runnable at most once per Condition epoch

InvSingleReacquireResolution
    — each Fiber's reacquireNodeState transitions to terminal exactly once

InvSingleReacquirePublication
    — each Fiber is made runnable at most once per reacquire epoch

InvConditionReasonFinal
    — once conditionReason[F] is Woken/Expired/Cancelled, it never changes

InvConditionWaiterDoesNotOwnMutex
    — waitPhase[F] = ConditionWaiting => mutexOwner != F

InvNoDualQueueMembership
    — a Fiber is never simultaneously in conditionQueue AND mutexQueue

InvReturnedOwnsMutex
    — waitPhase[F] = Returned => mutexOwner = F

InvReacquireSameFiber
    — the Fiber that reacquires is the same Fiber that waited

InvNoOwnerlessMutexDemand
    — a Fiber in MutexWaiting implies mutexOwner != NoOwner
      (the Mutex is always owned by some Fiber when waiters exist)

InvDestructionPrecondition
    — destroyed => conditionQueue empty AND no Fiber in ReacquirePending/MutexWaiting
```

### 16.2 Transition properties (history-backed)

```text
InvNoLostNotifyWindow
    — no interleaving exists where a notify fires between Condition node
      registration and Mutex release, because both occur under global_mtx_

InvNotifyOneFIFO
    — NotifyOne resolves the FIFO head of conditionQueue at its linearization point

InvNotifyAllSnapshotComplete
    — every Fiber in conditionQueue at the NotifyAll linearization point
      eventually resolves Woken

InvNotifyAllExcludesLateWaiter
    — a Fiber registered after NotifyAll's snapshot is NOT resolved by that
      NotifyAll

InvCancelExpireLoserFinality
    — a late cancel/expire against an already-terminal Condition node is the
      loser (no state change, no second publication)

InvDueInlineRetainsOwnership
    — WaitDueInline returns Expired with mutexOwner unchanged (Mutex NOT released)
```

### 16.3 Production source-order refinement obligations

```text
InvMutexOwnerBeforePublication
    — in MutexUnlockHandoff, owner_ is committed to the winner Fiber BEFORE the
      winner is published runnable (inherited from E12-C M-H1)
```

### 16.4 Liveness assumptions

```text
Every Fiber in conditionQueue eventually leaves (every registered Condition
    wait eventually resolves — by notify, cancel, or expire)
Every Fiber in mutexQueue eventually leaves (every registered Mutex wait
    eventually resolves — by handoff, cancel, or expire)
```

---

## 17. Negative-model preparation

Each negative breaks exactly one rule. The identifiers, mutation names, and
expected-failure properties below are the **canonical register** and are kept in
sync with `scripts/verify-e12-async-condition-formal.sh` and the per-model
`E12AsyncConditionNegC{N}.cfg` files. Ten models (NEG-C1..NEG-C10) are
implemented; each TLC-runs its model and asserts the named invariant fails.

```text
NEG-C1  NonOwnerWait
        — removes the mutexOwner = actor precondition AND lets the actor KEEP
          the mutex after entering ConditionWaiting (breaks C-H3)
        Expected failure: InvConditionWaiterDoesNotOwnMutex

NEG-C2  NotifyAnyNonRegistered
        — CancelCondition/notify drops the Registered + InConditionQueue guards,
          allowing resolution of any epoch (including Detached)
        Expected failure: InvConditionResolvedFinality

NEG-C3  ReturnOwnedNoGrant
        — ReturnOwned does NOT give the mutex to the returning fiber
          (violates C-H1: caller must own the Mutex after wait returns)
        Expected failure: InvReturnedOwnsMutex

NEG-C4  CancelReacquireEpoch
        — a late terminal attempt mutates protected reacquire/terminal state
          instead of being a no-op loser (breaks C-H5 finality)
        Expected failure: InvTerminalAttemptFinality

NEG-C5  NotifyAllNoDrain
        — NotifyAll marks waiters resolved but does NOT drain them from the
          Condition queue (breaks queue well-formedness)
        Expected failure: InvConditionQueueWellFormed

NEG-C6  ReacquireNonFIFO
        — MutexUnlockHandoff corrupts expectedFIFOHead (records a wrong epoch
          while still granting the real FIFO head)
        Expected failure: InvFIFOGrant

NEG-C7  DestroyWithActiveWaiters
        — destroyed' = TRUE can fire even while fibers are waiting on
          conditions (no queue-empty / no-waiter guard)
        Expected failure: InvDestructionPrecondition

NEG-C8  WaitReleaseBeforeRegister
        — the mutex is released but the Condition node is NEVER registered
          (the lost-notify window)
        Expected failure: InvNoLostNotifyWindow

NEG-C9  HandoffNonFIFO
        — MutexUnlockHandoff always grants to epoch R1 regardless of FIFO
          order, skipping other eligible waiters (breaks C-H8)
        Expected failure: InvEligiblePreMutexQueue

NEG-C10 SeparateQueues
        — MutexUnlockHandoff grants only to ReacquireEpochs, skipping
          OrdinaryEpochs (breaks unified FIFO, C-H8)
        Expected failure: InvOrdinaryAndReacquireFIFO
```


---

## 18. Runtime-test preparation (T0–T32)

```text
T0   construction and bound-Mutex identity
T1   already-due inline Expired, Mutex remains owned
T2   notify before wait is lost (caller precondition check)
T3   wait register-before-release closure
T4   notify in release/register boundary does not strand
T5   Mutex direct handoff during Condition admission
T6   notify_one empty
T7   notify_one single waiter
T8   notify_one FIFO multiple waiters
T9   notify_one cancelled/expired head handling
T10  notify_all snapshot
T11  notify_all excludes late waiter
T12  notify_all cancel race
T13  notify_all expire race
T14  notified waiter does not return while notifier owns Mutex
T15  ordinary locker versus notified waiter FIFO-tail ordering
T16  mandatory reacquire after Woken
T17  mandatory reacquire after Expired
T18  mandatory reacquire after Cancelled
T19  late cancel during reacquire cannot abort return ownership
T20  late timer during reacquire cannot alter reason
T21  exactly-once Condition publication
T22  exactly-once reacquire publication
T23  wrong Condition cancel
T24  external-thread notify/cancel
T25  real Worker migration during Condition phase
T26  real Worker migration during reacquire phase
T27  safe destruction empty
T28  destruction with Condition waiter contract
T29  destruction during reacquire contract
T30  lost-notify coordination 1000/1000
T31  notify/cancel/expire coordination 500/500
T32  notify_all snapshot coordination 500/500
```

No sleep-based causal proof.

---

## 19. Required deterministic test phases

Evaluate the following candidate phases. Select only phases required to prove a
load-bearing ordering. Reuse internal test-control infrastructure. No
installed-header test hooks. No callbacks or allocations in hot paths.

```text
after Condition registration, before Mutex release
after Mutex release/handoff, before make_waiting
after Condition resolution, before publication
after Condition Fiber resumes, before reacquire admission
after reacquire queue registration
notify_all snapshot captured, before first winner publication
```

---

## 20. Policy register summary

| ID | Decision | Resolution |
|----|----------|------------|
| C-H1 | Return ownership | **Model A: CLOSED** |
| C-H2 | Mutex association | **Bound at construction: CLOSED** |
| C-H3 | Reacquire mechanism | **Wake then ordinary lock: CLOSED** |
| C-H4 | Deadline scope | **Condition epoch only: CLOSED** |
| C-H5 | Reacquire cancellation | **Non-cancellable: CLOSED** |
| C-H6 | notify_all first scope | **Included: CLOSED** |
| C-H7 | WaitNode API | **One caller node, one stack-local: CLOSED** |
| C-H8 | Reacquire ordering | **FIFO tail: CLOSED** |
| C-H9 | Wait morph | **Deferred: CLOSED** |
| C-H10 | notify_all mechanism | **Scheduler-private snapshot/drain: CLOSED** |
