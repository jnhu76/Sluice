# E12-C — Async Mutex (Preparation Corrective-5)

> Status:
> ```text
> E12-C-PREPARATION-CORRECTIVE-1: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-2: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-3: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-4: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-5: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-5-REAUDIT: PASS
> E12-C-PREPARATION: CLOSED
>
> E12-C-FORMAL-1: COMPLETE
> E12-C-IMPLEMENTATION-1: COMPLETE
> E12-C-IMPLEMENTATION-REVIEW: PASS — E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1, 2026-07-19
> E12-C-IMPLEMENTATION: CLOSED
> E12-C: CLOSED
> ```
>
> The migration/data-race micro-review closing Corrective-4 was completed by
> [`docs/reviews/E12-C-MIGRATION-DATA-RACE-MICRO-REVIEW-1.md`](reviews/E12-C-MIGRATION-DATA-RACE-MICRO-REVIEW-1.md)
> (2026-07-19), verdict PASS. The final governance effect is also recorded in
> the FINAL STATUS block at the end of
> [`docs/reviews/E12-C-REVIEW.md`](reviews/E12-C-REVIEW.md). E12-C is CLOSED.
>
> Authority baseline: E10 CLOSED
> ([`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)); E11 CLOSED
> at `7715808` ([`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md));
> E12-A Event CLOSED ([`docs/e12-event.md`](e12-event.md)); E12-B Semaphore
> CLOSED
> ([`docs/e12-semaphore.md`](e12-semaphore.md)). This document does NOT reopen
> E10/E11/E12-A/E12-B preparation; it builds on those authorities.
>
> Cross-primitive preparation:
> [`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md) §6 (updated
> by this corrective).
>
> This document is the authoritative E12-C Async Mutex preparation specification.
> It is the corrective that closes the HUMAN-DECISION-REQUIRED classification
> from the prior preparation document (§6 of e12-sync-primitives-plan.md) and
> establishes the design authority for implementation.

---

## 1. Status banner

```text
E12-C-PREPARATION-CORRECTIVE-1: COMPLETE
E12-C-PREPARATION-CORRECTIVE-2: COMPLETE
E12-C-PREPARATION-CORRECTIVE-3: COMPLETE
E12-C-PREPARATION-CORRECTIVE-4: COMPLETE
E12-C-PREPARATION-CORRECTIVE-5: COMPLETE
E12-C-PREPARATION-CORRECTIVE-5-REAUDIT: PASS
E12-C-PREPARATION: CLOSED

E12-C-FORMAL-1: COMPLETE
E12-C-IMPLEMENTATION-1: COMPLETE
E12-C-IMPLEMENTATION: REVIEW-REQUIRED
```

The preparation authority was closed by independent adversarial re-audit
returning PASS. Implementation may proceed.

---

## 2. Policy register

The following policies are closed by this corrective as human-selected first-scope
decisions:

| ID | Decision | Selected policy | Evidence |
| ---- | -------- | --------------- | -------- |
| M-H1 | Unlock-with-waiters handoff | **Direct ownership handoff** — unlock transfers ownership to the eligible FIFO head waiter atomically | E10 FIFO WaitQueue; E12-B no-barging precedent (Semaphore A2); no free lock window during handoff; simpler ownership finality |
| M-H2 | Queued waiter ordering | **FIFO** — eligible queued waiters use WaitQueue FIFO order | E10 WaitQueue is FIFO by construction (§5); no configurable policy |
| M-H3 | Barging | **Forbidden** — a newly arriving acquirer may not bypass an already-eligible queued waiter | E12-B A2 barging forbidden; Mutex is strict generalization of Semaphore(count=1) + ownership |
| M-H4 | try_lock vs queued waiter | **try_lock fails** while an eligible waiter has FIFO priority | Consequence of M-H3 (no barging); matches Semaphore try_acquire contract |

Rationale may cite E10 FIFO WaitQueue, E12-B no-barging precedent, smaller
starvation surface, no free lock window during handoff, and simpler ownership
finality.

```text
Semaphore is precedent, not authority for Mutex ownership policy.
This corrective is the human authority that selects direct handoff.
```

The permitted starvation claim is:

```text
eligible queued waiters cannot be overtaken by later arrivals,
assuming the current owner eventually unlocks and the Scheduler
eventually runs runnable Fibers.
```

Do not claim a wall-clock starvation bound or "bounded by queue length."

---

## 3. Naming authority

```text
New Fiber-suspending primitive: AsyncMutex
Existing synchronous structural lock: Mutex (retained unchanged)
```

The existing `sluice::async::Mutex` (`include/sluice/async/mutex.hpp`) is a
synchronous TSA-annotated `std::mutex` wrapper (CPP-STATIC-1). It blocks the OS
thread, does not participate in Scheduler/Fiber suspension, and is used
internally by WaitQueue (`mtx_`), Scheduler (`global_mtx_`, `wake_mtx_`), and
`LockGuard`. It is NOT used for application-level mutual exclusion.

`AsyncMutex` coexists in `sluice::async` without collision. The two types have
distinct semantics (thread-blocking vs Fiber-suspending), distinct APIs, and
distinct use cases. Renaming the existing `Mutex` would be source-breaking with
no offsetting benefit in first scope.

---

## 4. Fiber identity authority

### 4.1 Identity model

The authoritative identity model is state-indexed (ADR §9.3.5.1):

| Fiber phase | authoritative identity/owner state | writer | reader |
| ----------- | ---------------------------------- | ------ | ------ |
| Running | TLS `g_worker`; `WorkerState::current` | worker_loop (run_next_on) | running fiber |
| Runnable | `fiber_owner_[F]` (runnable ownership record) | spawn/spawn_on (initial); try_steal (victim→thief) | steal eligibility check |
| Waiting | `WaitReg.owner` (captured `g_worker` at suspend time) | `await_*` | wake routing |
| Finished | None — fiber is done; no owner | make_done | no reader |

### 4.2 Owner identity

Mutex ownership is bound to **`Fiber*`** identity:

- The `Fiber` object is non-movable (`fiber.hpp:69`), address-stable for its
  lifetime.
- `Fiber*` identity is independent of which Worker executes the Fiber.
- After E8 stealing, a Fiber may resume on a thief Worker; the Mutex owner
  check must use `Fiber*`, NOT Worker identity.
- `g_worker` / `WorkerState::current` is the *executing* Worker, NOT the
  Mutex owner.
- `fiber_owner_[F]` is the *runnable-ticket* owner, NOT the Mutex owner.
- `WaitReg.owner` is the *wake-route* owner, NOT the Mutex owner.

### 4.3 Migration safety

```text
Fiber A locks AsyncMutex on Worker W0
Fiber A is stolen by Worker W1 (E8 Model B)
Fiber A resumes on W1
Fiber A calls unlock()
→ unlock succeeds (owner check is Fiber* identity, not Worker identity)
```

This is load-bearing. The runtime test plan (§17) includes a migration test
using real E8 steal/migration.

### 4.4 ID-Q answers

| Question | Answer |
| -------- | ------ |
| What exact value identifies the Mutex owner? | `Fiber*` |
| Can the owner Fiber migrate before unlock()? | Yes (E8 stealing) |
| Can unlock() recover Fiber identity after migration? | Yes (`g_worker->current` at unlock time is the running Fiber) |
| Can external OS threads call unlock() or try_lock()? | No (no Fiber identity; caller precondition violation) |

### 4.5 Calling-context summary

```text
try_lock / lock / lock_until / unlock:
    require a currently running Fiber

cancel:
    may be called from any thread

construction / destruction:
    do not require a running Fiber
```

---

## 5. Public API authority

### 5.1 Minimal first-scope surface

```cpp
class AsyncMutex {
public:
    explicit AsyncMutex(Scheduler& scheduler);
    ~AsyncMutex();

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;

    [[nodiscard]] bool try_lock();
    void lock(WaitNode& node);
    void lock_until(WaitNode& node, Scheduler::deadline_t deadline);
    [[nodiscard]] bool cancel(WaitNode& node);
    void unlock();
};
```

### 5.2 Public observation decision

```text
No public is_locked().
```

Reason:

```text
it is only observational
it gives no admission guarantee
it requires a separate atomic mirror to be lock-free
it is not needed for lock correctness
```

A plain non-atomic lock-free `is_locked()` is forbidden. An
`std::atomic<bool> locked_snapshot_` is permitted solely as an observational
mirror but must not participate in admission, handoff, unlock authority, or
owner validation.

### 5.3 Explicitly rejected API

```text
owner()                   — no production use case
wait_queue()              — sealed public authority (Hard Rule 5)
lock_many()               — multi-wait is E13 territory
unlock_from_thread()      — ownership check requires Fiber identity
recursive mode            — complex state space, no first-scope need
RAII guard                — deferred to library adapter
generic Lockable interface — Hard Rule 5/6
```

---

## 6. Calling-context authority

### 6.1 Requires a currently running Fiber

```text
try_lock
lock
lock_until
unlock
```

The current Fiber identity is established by:

```text
g_worker != nullptr
AND
g_worker->current != nullptr
```

These operations use `g_worker->current` as the calling Fiber. If
`g_worker == nullptr`, the call is a caller precondition violation (debug
assert).

### 6.2 Cancel

`cancel(WaitNode&)` may be invoked from **any thread**.

Evidence:

- `Event::cancel` and `Semaphore::cancel` are Scheduler-serialized via
  `global_mtx_` + `q.mtx()`. They do not acquire Mutex ownership. They do not
  require Fiber identity.
- `AsyncMutex::cancel` follows the same pattern: membership gate
  (`contains_locked`) under `global_mtx_` + `q.mtx()`, then `cancel_locked`
  CAS. No ownership check.
- A cancel from an external OS thread is structurally safe: `g_worker` is null,
  `route_runnable_locked` pushes to `pending_spawn_`, `signal_wake_locked`
  wakes a parked Worker.

### 6.3 Object lifetime

Construction and destruction do not themselves require a running Fiber, but the
caller must satisfy the Scheduler and waiter lifetime contracts (§13).

---

## 7. Misuse contracts

### 7.1 Recursive try_lock

```text
returns false
debug assertion may additionally diagnose owner == current Fiber
```

### 7.2 Recursive lock / lock_until

```text
caller precondition violation
debug assertion
must not be described as successful acquisition or a normal release-mode no-op
```

Do not promise the caller may continue as though the lock was acquired.

### 7.3 Non-owner unlock

```text
caller precondition violation
debug assertion
must not alter owner or wake a waiter
```

Do not describe it as an ordinary supported no-op.

### 7.4 Unlock while unlocked

```text
caller precondition violation
debug assertion
no owner/queue mutation
```

### 7.5 External-thread try_lock / unlock

```text
caller precondition violation (g_worker == nullptr)
debug assertion
no owner mutation, no waiter wake
```

### 7.6 Destruction

```text
destroy while owner != NoOwner:  caller contract violation
destroy with queued Registered epochs: caller contract violation
destructor does not cancel, wake, force-release, or transfer ownership
```

`~AsyncMutex` debug-asserts `owner == NoOwner`. `~WaitQueue` debug-asserts
`head_ == nullptr`. No cancel-all, no wake-all, no force-release.

### 7.7 Owner abandonment

A Fiber finishing without unlock is caller misuse. Equivalent to failing to
unlock `std::mutex`. No global owned-lock registry is introduced.

Do not state the invariant as "owner always points to a live Fiber" across
misuse traces. In the supported protocol, owner denotes the acquiring Fiber
epoch; correct clients release before Fiber destruction.

---

## 8. Ownership state model

### 8.1 Authoritative state

```text
owner : Fiber* | NoOwner
```

Under the Scheduler coordination lock:

```text
owner == NoOwner   <=>  Mutex is unlocked
owner != NoOwner   <=>  Mutex is owned by exactly that Fiber
```

Do not require a second authoritative plain `bool locked_`. A redundant
ordinary bool is forbidden because it creates drift risk.

### 8.2 Ownership states

```text
Unlocked:
    owner = NoOwner

Owned(F):
    owner = F
```

### 8.3 Ownership lifecycle

1. **lock() attempt succeeds (immediate):** `owner := current Fiber` under
   `global_mtx_` + `q.mtx()`. Inline resolution: `resolve_(Woken)` +
   unlink. Do not suspend.
2. **lock() attempt succeeds (handoff):** `owner := winner Fiber` atomically
   with `resolve_(Woken)` + unlink + publication. See §9.
3. **unlock() called, queue empty:** `owner := NoOwner`. Transition to
   Unlocked.
4. **unlock() called, queue non-empty:** direct handoff to FIFO head (§9).
   `owner := new winner Fiber`. Transition to `Owned(F_new)`. There is no
   intermediate `owner := NoOwner` on this path.

### 8.4 What must never be externally visible

```text
Woken winner + owner = NoOwner
Woken winner + owner = F_old
winner runnable before owner = winner Fiber
Unlocked while an eligible handoff winner exists
```

Cancel or expiry after Woken cannot change owner (E10 loser truth table).

---

## 9. Direct-handoff decision

### 9.1 Selected model

**Model H1 — Direct ownership handoff.**

Unlock transition when queue is non-empty:

```text
Owned(F_old), eligible FIFO head W exists
    → atomically:
        W resolve_(Woken) wins
        W is unlinked
        winner Fiber F_new is obtained (non-null by invariant)
        owner := F_new
        timer/wait bookkeeping is retired
        F_new is published runnable exactly once
        resulting state = Owned(F_new)
```

Unlock transition when queue is empty:

```text
Owned(F_old), queue empty
    → owner := NoOwner
    → resulting state = Unlocked
```

### 9.2 No persistent intermediate state

There is no persistent `Reserved`, `HandoffPending`, or grant-in-flight state.
Handoff is one atomic transition under `global_mtx_` + `q.mtx()`.

### 9.3 Properties

| Property | H1 Direct Handoff |
| -------- | ----------------- |
| FIFO | Yes (WaitQueue head) |
| Barging | No |
| Starvation | Eligible queued waiters cannot be overtaken (assuming owner unlocks and Scheduler runs) |
| State-space complexity | Low (Unlocked / Owned(F)) |
| Cancellation behavior | Cancel on queued waiter: unlinks it. Cancel after Woken: loses (E10 loser). |
| Timeout behavior | Expire on queued waiter: unlinks it. Expire after Woken: loses (E10 loser). |
| Required seam | MUTEX-HANDOFF-ONE (§10) |
| Exactly-once ownership | Yes (CAS is single authority) |
| Owner visibility | Owner committed before publication |
| Testability | Deterministic via internal test seam |
| Formal-model complexity | Low (two persistent states + one atomic transition) |

---

## 10. Minimum private seam specification

### 10.1 Semantic boundary

```text
MUTEX-HANDOFF-ONE
```

### 10.2 Inputs

```text
the AsyncMutex private WaitQueue
the AsyncMutex ownership state (Fiber*& owner reference)
```

### 10.3 Preconditions

```text
Scheduler::global_mtx_ held
owner == currently executing Fiber
queue/owner lifetime valid
```

### 10.4 Required ordering

```text
1.  lock the AsyncMutex WaitQueue mutex
2.  identify and resolve the eligible FIFO head
3.  unlink the winner
4.  obtain the winner's non-null Fiber identity
5.  commit AsyncMutex owner = winner Fiber
6.  retire timer and wait-registration bookkeeping
7.  perform waiting → runnable exactly once
8.  route/publish runnable
9.  signal Scheduler wake obligation where required
```

### 10.5 Mandatory ordering constraint

```text
Step 5 (owner commit) MUST occur BEFORE step 7 (runnable publication).
```

The exact placement of timer retirement (step 6) relative to owner commit may
vary while `global_mtx_` remains held, provided no publication occurs before
owner commit and no stale timer can later win.

### 10.6 Invariant: non-null winner

A winning linked WaitNode must have a non-null Fiber by invariant. A null
winner Fiber is an internal invariant failure/assert, not an empty-queue
result.

### 10.7 Seam properties

```text
private
Mutex-specific
Scheduler-owned
non-generic
```

### 10.8 Forbidden

```text
public winner identity
generic callback between resolve and publication
WaitQueue public access
Grant framework
Select/multi-wait
```

---

## 11. Admission closure

### 11.1 lock / lock_until admission path

```text
initial ownership check
register epoch into AsyncMutex WaitQueue
under-lock ownership/FIFO recheck
    inline acquisition or suspension commit
```

### 11.2 Inline acquisition conditions

A newly registered epoch may acquire inline only when:

```text
owner == NoOwner
AND
the epoch is the eligible FIFO head (node.prev_ == nullptr under q.mtx())
```

### 11.3 Inline acquisition steps

```text
owner := current Fiber
resolve epoch Woken (wake_node_locked)
unlink / close speculative registration
retire timer if present
do not suspend
```

### 11.4 Suspension commit

Otherwise: `me->make_waiting()` + `context_switch`.

### 11.5 Accepted locks

```text
Scheduler::global_mtx_
    →
AsyncMutex WaitQueue mutex
```

### 11.6 Forbidden trace

No trace may end with:

```text
owner == NoOwner
eligible waiter suspended
newcomer able to barge
```

---

## 12. Deadline and cancellation semantics

### 12.1 Deadline precedence

```text
immediately admissible ownership beats an already-due deadline
```

Thus:

| State at admission | Deadline at admission | Result |
| ------------------ | --------------------- | ------ |
| free + caller has FIFO priority | due | Woken / owner = current Fiber |
| not admissible | due | Expired |
| free + caller has FIFO priority | future | Woken / owner = current Fiber |
| not admissible | future | register timed wait |

### 12.2 Grant finality

After direct handoff wins (Woken CAS succeeds), later expiry loses and cannot
clear owner. Same shape as Semaphore §5.2 and E11's loser semantic.

### 12.3 Cancellation authority

`cancel(WaitNode&)` affects only a still-Registered epoch linked in this
AsyncMutex's WaitQueue.

### 12.4 Cannot affect

```text
current owner
Woken handoff winner
owner field
unlocked/owned state
another Mutex queue
```

### 12.5 Safe false-return cases

```text
wrong Mutex, same Scheduler     → false
wrong Mutex, different Scheduler → false
repeated cancel                 → false
detached node                   → false
already Woken                   → false
already Cancelled               → false
already Expired                 → false
```

### 12.6 Membership safety

The membership check scans THIS AsyncMutex's own queue under
`global_mtx_` + `q.mtx()`. It never reads a foreign node's `home_` and never
locks a foreign Mutex or Scheduler. Cross-Scheduler wrong-Mutex cancel is
synchronized and structurally safe.

---

## 13. Destruction contract

```text
~AsyncMutex() {
    assert(owner == NoOwner && "AsyncMutex destroyed while locked");
    // ~WaitQueue asserts head_ == nullptr in debug
}
```

- `~AsyncMutex()` does NOT cancel waiters.
- `~AsyncMutex()` does NOT force-release.
- `~AsyncMutex()` does NOT wake waiters.
- `~AsyncMutex()` does NOT transfer ownership.
- Caller must ensure unlocked + all waits terminal before destruction.

Debug assertions are mechanically available: `owner` is a member field;
`~WaitQueue` asserts `head_ == nullptr`.
## 14. Formal model preparation (docs/spec/e12_async_mutex/)

### 14.1 Model directory

```text
docs/spec/e12_async_mutex/
```

### 14.2 Finite domains

```text
Fiber    — finite set of Fibers (includes at least F1, F2, F3)
Epoch    — finite set of wait epochs (includes at least E1, E2, E3)
NoOwner  — sentinel distinct from every Fiber
```

### 14.3 State variables

```text
owner : Fiber ∪ {NoOwner}

queue : Seq(Epoch)                    -- FIFO ordered; only Suspended or terminal epochs

nodeState :
    Epoch -> {Detached, Registered, Woken, Cancelled, Expired}

epochFiber : Epoch -> Fiber           -- the Fiber that registered this epoch

-- No admissionPhase field: the production register-recheck-suspend critical
-- section is ONE atomic step. A Registered epoch in the queue is always
-- Suspended (has committed make_waiting). An epoch not yet Suspended does
-- not exist as a stable queue member.

runnablePublished : Epoch -> BOOLEAN
resolutionCount : Epoch -> {0, 1}
publicationCount : Epoch -> {0, 1}

lastAction : {Init,
              TryLockSuccess, TryLockFailure,
              LockImmediate,
              LockAdmissionAcquire, LockAdmissionSuspend,
              LockUntilImmediate, LockUntilDue,
              LockUntilAdmissionAcquire, LockUntilAdmissionSuspend,
              UnlockNoWaiter, UnlockHandoff,
              CancelSuspended, ExpireSuspended,
              CancelAttemptTerminal, ExpireAttemptTerminal,
              Destroy}
lastActor : Fiber ∪ {None}
lastTargetEpoch : Epoch ∪ {None}
lastGrantedEpoch : Epoch ∪ {None}

-- Pre-state ghost evidence: snapshots of authoritative state immediately
-- before the most recent modeled action. preOwner is the PREVIOUS Mutex
-- ownership, NOT the acting Fiber.
preOwner : Fiber ∪ {NoOwner}
preQueue : Seq(Epoch)
preNodeState : Epoch -> {Detached, Registered, Woken, Cancelled, Expired}
prePublished : Epoch -> BOOLEAN
prePublicationCount : Epoch -> {0, 1}

admissionSawFree : Epoch -> BOOLEAN
admissionSawDue : Epoch -> BOOLEAN

destroyed : BOOLEAN
```

### 14.4 Derived state

```text
EligibleQueue    == SelectSeq(queue, LAMBDA e: nodeState[e] = Registered)
FIFOHead         == IF Len(EligibleQueue) > 0 THEN Head(EligibleQueue) ELSE None
IsOwned          == owner /= NoOwner
IsUnlocked       == owner = NoOwner
EligiblePreQueue == SelectSeq(preQueue, LAMBDA e: preNodeState[e] = Registered)
```

### 14.5 Production critical section refinement

The production `mutex_lock` / `mutex_lock_until` admission path holds
`global_mtx_` + `q.mtx()` across:

```text
register node into queue
++waiting_waitq_count_
recheck: if admissible (FIFO head + resource free):
    resolve Woken inline
    --waiting_waitq_count_
    make_runnable (no-op for running Fiber)
    return WITHOUT suspending
recheck: if not admissible and deadline due:
    resolve Expired inline
    return WITHOUT suspending
otherwise:
    make_waiting()
release locks
context_switch
```

No interleaving of another Fiber's unlock, cancel, or expire is possible
between registration and the admission decision. Therefore the formal model
MUST NOT expose a Registered-but-not-Suspended queue state.

Each formal action below refines the complete production critical section
as ONE atomic step. There is no standalone registration action and no
standalone suspension action.

### 14.6 Correct actions

Each action records pre-state ghost evidence BEFORE its mutations.

**Pre-state recording discipline:**
```text
For every behavior action, conceptually:
    preOwner'            = owner             (before mutation)
    preQueue'            = queue             (before mutation)
    preNodeState'        = nodeState         (before mutation)
    prePublished'        = runnablePublished (before mutation)
    prePublicationCount' = publicationCount  (before mutation)
```

Explicitly forbidden:
```text
preOwner := actor    -- preOwner is previous Mutex ownership, not the acting Fiber
```

**Key semantic distinction:**
```text
terminal WaitNode outcome  !=  runnable-ticket publication

A terminal immediate outcome (Woken/Expired) does NOT imply make_runnable()
succeeded or was required. Only UnlockHandoff, CancelSuspended, and
ExpireSuspended create runnable publications.
```

#### Non-epoch operations

**TryLockSuccess:** (no WaitNode, no Epoch)
```text
precondition: owner = NoOwner /\ FIFOHead = None /\ ~destroyed
effect:
    owner := actor
    lastAction := TryLockSuccess
    lastActor := actor
    lastTargetEpoch := None
    lastGrantedEpoch := None
    -- no nodeState mutation, no resolutionCount, no publication
```

**TryLockFailure:** (no WaitNode, no Epoch)
```text
precondition: owner /= NoOwner \/ FIFOHead /= None \/ destroyed
effect:
    lastAction := TryLockFailure
    lastActor := actor
    lastTargetEpoch := None
    lastGrantedEpoch := None
    -- no state mutation
```

#### Admission actions (atomic register-recheck-outcome)

Each admission action refines the entire production critical section:
register + recheck + inline-acquire or suspension-commit, all under
`global_mtx_` + `q.mtx()`.

**LockImmediate:** (free + no queued waiters → acquire without registration)
```text
precondition: owner = NoOwner /\ FIFOHead = None /\ ~destroyed
    /\ nodeState[epoch] = Detached
effect:
    owner := actor
    nodeState[epoch] := Woken
    resolutionCount[epoch] := 1
    lastAction := LockImmediate
    lastActor := actor
    lastGrantedEpoch := epoch
    lastTargetEpoch := epoch
    -- no runnable publication (Fiber is running, not suspended)
```

**LockAdmissionAcquire:** (registered, recheck finds free + FIFO head → acquire)
```text
precondition: nodeState[epoch] = Detached
    /\ owner = NoOwner /\ FIFOHead = None /\ ~destroyed
effect:
    -- atomic: register + recheck + acquire
    Append(queue, epoch)
    nodeState[epoch] := Woken
    epochFiber[epoch] := actor
    resolutionCount[epoch] := 1
    Remove(queue, epoch)
    owner := actor
    lastAction := LockAdmissionAcquire
    lastActor := actor
    lastGrantedEpoch := epoch
    lastTargetEpoch := epoch
    -- no runnable publication (Fiber is running, not suspended)
    -- queue ends empty (registered then immediately removed)
```

**LockAdmissionSuspend:** (registered, recheck finds owned or older waiter → suspend)
```text
precondition: nodeState[epoch] = Detached
    /\ ~destroyed
    /\ NOT (owner = NoOwner /\ FIFOHead = None)
    -- either owner exists, or an older eligible waiter is queued
effect:
    -- atomic: register + recheck + make_waiting
    Append(queue, epoch)
    nodeState[epoch] := Registered
    epochFiber[epoch] := actor
    lastAction := LockAdmissionSuspend
    lastActor := actor
    lastTargetEpoch := epoch
    lastGrantedEpoch := None
    -- epoch is now Registered in queue (= Suspended in production terms)
    -- no runnable publication
```

**LockUntilImmediate:** (free + due deadline → acquire without registration)
```text
precondition: owner = NoOwner /\ FIFOHead = None /\ ~destroyed
    /\ nodeState[epoch] = Detached /\ deadlineDue[epoch] = TRUE
effect:
    owner := actor
    nodeState[epoch] := Woken
    resolutionCount[epoch] := 1
    lastAction := LockUntilImmediate
    lastActor := actor
    lastGrantedEpoch := epoch
    lastTargetEpoch := epoch
    admissionSawFree[epoch] := TRUE
    admissionSawDue[epoch] := TRUE
    -- no runnable publication
```

**LockUntilAdmissionAcquire:** (registered, recheck finds free + FIFO head → acquire; deadline-aware)
```text
precondition: nodeState[epoch] = Detached
    /\ owner = NoOwner /\ FIFOHead = None /\ ~destroyed
effect:
    Append(queue, epoch)
    nodeState[epoch] := Woken
    epochFiber[epoch] := actor
    resolutionCount[epoch] := 1
    Remove(queue, epoch)
    owner := actor
    lastAction := LockUntilAdmissionAcquire
    lastActor := actor
    lastGrantedEpoch := epoch
    lastTargetEpoch := epoch
    admissionSawFree[epoch] := TRUE
    admissionSawDue[epoch] := deadlineDue[epoch]
    -- no runnable publication
    -- queue ends empty
```

**LockUntilAdmissionSuspend:** (registered, recheck finds not admissible + deadline NOT due → suspend with pending timer)
```text
precondition: nodeState[epoch] = Detached
    /\ ~destroyed
    /\ NOT (owner = NoOwner /\ FIFOHead = None)
    /\ deadlineDue[epoch] = FALSE
effect:
    Append(queue, epoch)
    nodeState[epoch] := Registered
    epochFiber[epoch] := actor
    lastAction := LockUntilAdmissionSuspend
    lastActor := actor
    lastTargetEpoch := epoch
    lastGrantedEpoch := None
    admissionSawFree[epoch] := (owner = NoOwner /\ FIFOHead = None)
    admissionSawDue[epoch] := FALSE
    -- epoch Registered in queue with pending deadline
```

**LockUntilDue:** (registered, recheck finds not admissible + deadline ALREADY due → expire at admission)
```text
precondition: nodeState[epoch] = Detached
    /\ ~destroyed
    /\ NOT (owner = NoOwner /\ FIFOHead = None)
    /\ deadlineDue[epoch] = TRUE
effect:
    -- atomic: register + recheck + expire inline
    Append(queue, epoch)
    nodeState[epoch] := Expired
    epochFiber[epoch] := actor
    resolutionCount[epoch] := 1
    Remove(queue, epoch)
    lastAction := LockUntilDue
    lastActor := epochFiber[epoch]
    lastTargetEpoch := epoch
    lastGrantedEpoch := None
    admissionSawFree[epoch] := FALSE
    admissionSawDue[epoch] := TRUE
    -- no runnable publication (Fiber has not suspended)
    -- queue ends empty (registered then immediately expired+removed)
```

#### Unlock operations

**UnlockNoWaiter:**
```text
precondition: owner = actor /\ FIFOHead = None /\ ~destroyed
effect:
    owner := NoOwner
    lastAction := UnlockNoWaiter
    lastActor := actor
    lastTargetEpoch := None
    lastGrantedEpoch := None
```

**UnlockHandoff:** (publishes ONLY a Suspended waiter)
```text
precondition: owner = actor /\ FIFOHead = W /\ W /= None /\ ~destroyed
    /\ nodeState[W] = Registered
    /\ epochFiber[W] /= None
    -- W is Registered in queue = Suspended in production terms
effect:
    nodeState[W] := Woken
    resolutionCount[W] := 1
    owner := epochFiber[W]          -- owner commit
    runnablePublished[W] := TRUE    -- publication AFTER owner commit
    publicationCount[W] := 1
    Remove(queue, W)
    lastAction := UnlockHandoff
    lastActor := actor
    lastGrantedEpoch := W
    lastTargetEpoch := W
```

#### Cancel/Expire — suspended waiter (publishes)

**CancelSuspended:**
```text
precondition: nodeState[epoch] = Registered /\ epoch \in queue
effect:
    nodeState[epoch] := Cancelled
    resolutionCount[epoch] := 1
    Remove(queue, epoch)
    runnablePublished[epoch] := TRUE    -- resumes the suspended Fiber
    publicationCount[epoch] := prePublicationCount[epoch] + 1
    lastAction := CancelSuspended
    lastActor := None
    lastTargetEpoch := epoch
    lastGrantedEpoch := None
```

**ExpireSuspended:**
```text
precondition: nodeState[epoch] = Registered /\ epoch \in queue
effect:
    nodeState[epoch] := Expired
    resolutionCount[epoch] := 1
    Remove(queue, epoch)
    runnablePublished[epoch] := TRUE    -- resumes the suspended Fiber
    publicationCount[epoch] := prePublicationCount[epoch] + 1
    lastAction := ExpireSuspended
    lastActor := None
    lastTargetEpoch := epoch
    lastGrantedEpoch := None
```

#### Late terminal attempt actions (non-vacuous losers)

**CancelAttemptTerminal:**
```text
precondition: nodeState[epoch] \in {Woken, Cancelled, Expired}
effect:
    lastAction := CancelAttemptTerminal
    lastActor := None
    lastTargetEpoch := epoch
    lastGrantedEpoch := None
    -- No state mutation (late attempt against terminal epoch)
```

**ExpireAttemptTerminal:**
```text
precondition: nodeState[epoch] \in {Woken, Cancelled, Expired}
effect:
    lastAction := ExpireAttemptTerminal
    lastActor := None
    lastTargetEpoch := epoch
    lastGrantedEpoch := None
    -- No state mutation (late attempt against terminal epoch)
```

#### Destruction

**Destroy:**
```text
precondition: owner = NoOwner /\ queue = <<>> /\ ~destroyed
effect:
    destroyed := TRUE
    lastAction := Destroy
    lastActor := None
    lastTargetEpoch := None
    lastGrantedEpoch := None
```

### 14.7 UnlockHandoff ghost evidence

`UnlockHandoff` publishes ONLY a Registered (=Suspended) waiter. The
`admissionPhase` field is eliminated; the Registered-in-queue state IS the
suspended state. This is a direct refinement of production: no Fiber is
published without having committed `make_waiting()`.

### 14.8 Queue membership discipline

After any correct action, every epoch in `queue` satisfies:

```text
nodeState[e] = Registered    (= Suspended in production terms)
```

No epoch in `queue` is Detached, Woken, Cancelled, or Expired. Terminal
epochs are immediately removed by the winning action.

---

## 15. Property classification

### 15.1 State invariants

```text
InvType:
    owner \in Fiber ∪ {NoOwner}

InvQueueWellFormed:
    queue has no duplicate Epochs
    \A e \in queue: nodeState[e] = Registered

InvSingleResolution:
    \A e: resolutionCount[e] <= 1

InvSinglePublication:
    \A e: publicationCount[e] <= 1

InvNoOwnerlessQueuedDemand:
    owner = NoOwner => FIFOHead = None
    (if no owner, queue has no eligible waiter; one-way, not biconditional)

InvPublishedEpochTerminal:
    \A e: runnablePublished[e] = TRUE => nodeState[e] \in {Woken, Cancelled, Expired}

InvPublicationConsistency:
    \A e: runnablePublished[e] = (publicationCount[e] = 1)
    (publication flag and count are tightly coupled)
```

### 15.2 Transition / history-backed properties

```text
InvUnlockAuthority:
    lastAction = UnlockNoWaiter \/ lastAction = UnlockHandoff
    => preOwner = lastActor

InvRecursiveForbidden:
    lastAction \in {TryLockSuccess, LockImmediate, LockUntilImmediate,
                    LockAdmissionAcquire, LockUntilAdmissionAcquire}
    => preOwner = NoOwner

InvFIFOGrant:
    lastAction = UnlockHandoff
    => lastGrantedEpoch = Head(preQueue)

InvEligiblePreQueue:
    lastAction \in {UnlockHandoff, CancelSuspended, ExpireSuspended}
    => lastTargetEpoch = Head(EligiblePreQueue)

InvNoBarging:
    lastAction \in {TryLockSuccess, LockImmediate, LockUntilImmediate,
                    LockAdmissionAcquire, LockUntilAdmissionAcquire}
    => preOwner = NoOwner /\ EligiblePreQueue = <<>>

InvAdmissionFIFO:
    lastAction \in {LockAdmissionAcquire, LockUntilAdmissionAcquire}
    => lastTargetEpoch = Head(EligiblePreQueue)

InvGrantOwnerCommit:
    lastAction = UnlockHandoff
    => lastGrantedEpoch /= None
       AND owner = epochFiber[lastGrantedEpoch]

InvGrantPublicationCoupling:
    lastAction = UnlockHandoff
    => owner = epochFiber[lastGrantedEpoch]
       AND runnablePublished[lastGrantedEpoch] = TRUE
       AND prePublished[lastGrantedEpoch] = FALSE
       AND publicationCount[lastGrantedEpoch] = prePublicationCount[lastGrantedEpoch] + 1

InvAdmissionClosure:
    lastAction = LockAdmissionSuspend \/ lastAction = LockUntilAdmissionSuspend
    => preOwner /= NoOwner \/ Head(EligiblePreQueue) /= lastTargetEpoch
    (suspension only when Mutex is owned OR an older eligible waiter exists)

InvDeadlinePrecedence:
    lastAction \in {LockUntilImmediate, LockUntilAdmissionAcquire}
    => admissionSawFree[lastTargetEpoch] = TRUE
       /\ admissionSawDue[lastTargetEpoch] = TRUE
       /\ nodeState[lastTargetEpoch] = Woken

InvGrantFinality:
    lastAction \in {CancelAttemptTerminal, ExpireAttemptTerminal}
    /\ preNodeState[lastTargetEpoch] = Woken
    => owner = preOwner
       /\ nodeState[lastTargetEpoch] = Woken
       /\ publicationCount[lastTargetEpoch] = prePublicationCount[lastTargetEpoch]

InvPublicationRequiresSuspensionOrHandoff:
    \A e: publicationCount[e] > 0
    => lastAction \in {UnlockHandoff, CancelSuspended, ExpireSuspended}
       /\ lastGrantedEpoch = e

InvDestructionPrecondition:
    lastAction = Destroy
    => preOwner = NoOwner /\ preQueue = <<>>
```

### 15.3 Classification notes

- `nodeState[e] = Woken => owner = epochFiber[e]` is **NOT** valid: owner
  may later unlock.
- No-barging is **history-backed**, not a plain state invariant.
- Deadline precedence is **history-backed** using latched admission evidence.
- TLA+ proves grant/publication **coupling**; production intra-action
  ordering is a separate refinement obligation (§15.4).
- Queue epochs are always Registered (= Suspended in production). The
  production `admissionPhase` field is eliminated because the model's
  atomic admission actions collapse register+recheck+suspend into one step.

### 15.4 Production structural refinement obligation

```text
MUTEX-HANDOFF-ONE must execute:

    owner = winner Fiber
        BEFORE
    Fiber::make_runnable / route_runnable_locked

Proven by:
    production source inspection
    deterministic internal test seam after owner commit and before publication
```

---

## 16. Negative-model matrix

| NEG | Broken action | Counterexample shape | Expected named invariant |
| --- | ----------- | -------------------- | ------------------------ |
| NEG-M1 NonOwnerUnlock | foreign Fiber F2 calls UnlockNoWaiter when owner = F1 | `preOwner = F1 /\ lastActor = F2 /\ lastAction = UnlockNoWaiter` | InvUnlockAuthority |
| NEG-M2 RecursiveAcquire | TryLockSuccess when owner = F1 and lastActor = F1 | `preOwner = F1 /\ lastAction = TryLockSuccess` | InvRecursiveForbidden |
| NEG-M3 NonFIFOGrant | UnlockHandoff selects epoch other than Head(preQueue) | `lastAction = UnlockHandoff /\ lastGrantedEpoch /= Head(preQueue)` | InvFIFOGrant |
| NEG-M4 HandoffFreeWindow | UnlockHandoff resolves FIFO head Woken but does not commit owner (owner := NoOwner) | `lastAction = UnlockHandoff /\ owner = NoOwner /\ EligibleQueue /= <<>>` | InvNoOwnerlessQueuedDemand |
| NEG-M5 GrantWithoutOwnerCommit | UnlockHandoff resolves E but owner = NoOwner or wrong Fiber | `lastAction = UnlockHandoff /\ owner /= epochFiber[lastGrantedEpoch]` | InvGrantOwnerCommit |
| NEG-M6 PublicationWithoutGrantCoupling | UnlockHandoff publishes W but owner != epochFiber[W] | `lastAction = UnlockHandoff /\ runnablePublished[lastGrantedEpoch] = TRUE /\ owner /= epochFiber[lastGrantedEpoch]` | InvGrantPublicationCoupling |
| NEG-M7 AdmissionLostWake | LockAdmissionSuspend when Mutex is free and epoch is FIFO head | `lastAction = LockAdmissionSuspend /\ preOwner = NoOwner /\ Head(EligiblePreQueue) = lastTargetEpoch` | InvAdmissionClosure |
| NEG-M8 CancelRevokesHandoff | late CancelAttemptTerminal of Woken epoch changes owner or republishes | `lastAction = CancelAttemptTerminal /\ preNodeState[lastTargetEpoch] = Woken /\ (owner /= preOwner \/ publicationCount[lastTargetEpoch] /= prePublicationCount[lastTargetEpoch])` | InvGrantFinality |
| NEG-M9 DeadlineRevokesHandoff | late ExpireAttemptTerminal of Woken epoch changes owner or republishes | `lastAction = ExpireAttemptTerminal /\ preNodeState[lastTargetEpoch] = Woken /\ (owner /= preOwner \/ publicationCount[lastTargetEpoch] /= prePublicationCount[lastTargetEpoch])` | InvGrantFinality |
| NEG-M10 ImmediatePublication | LockImmediate creates a runnable publication | `lastAction = LockImmediate /\ publicationCount[lastGrantedEpoch] > 0` | InvPublicationRequiresSuspensionOrHandoff |
| NEG-M11 DestructionWhileOwnedOrQueued | Destroy with owner != NoOwner or queue non-empty | `lastAction = Destroy /\ (preOwner /= NoOwner \/ preQueue /= <<>>)` | InvDestructionPrecondition |

### 16.1 Omission notes

- **DoubleOwner** structurally impossible (single-valued `owner`).
- **Worker identity ownership failure** is runtime-only (E8 migration test).
- Each NEG has exactly ONE broken rule and ONE expected violated invariant.
- NEG-M7 targets `LockAdmissionSuspend` (the suspension action), not a
  removed standalone `LockSuspend`.
- No stale `Cancel`/`Expire` action names remain.

---

## 17. Runtime test authority

### 17.1 Required deterministic tests

| Category | Tests |
| -------- | ----- |
| Construction / API | initial unlocked, copy/move forbidden, public WaitQueue inaccessible |
| Immediate ownership | try_lock success, try_lock failure while owned, lock immediate success |
| Ownership misuse | non-owner unlock, unlock while unlocked, recursive try_lock, recursive lock, unlock after Worker migration |
| FIFO / handoff | W1 before W2 → W1 acquires, W1 cancelled before unlock → W2 acquires, W1 expired before unlock → W2 acquires |
| No-barging | W1 queued + new try_lock → newcomer fails; three-party (W1+W2 queued + newcomer fails); cancelled-head + newcomer fails |
| Admission closure | A registers + owner unlocks in registration window → A does not strand |
| Deadline | free + due → Woken, owned + due → Expired, unlock beats timer, timer beats unlock |
| Cancellation | cancel suspended waiter, cancel after handoff, wrong Mutex same Scheduler, wrong Mutex different Scheduler |
| Migration | Fiber locks before migration, unlocks after real E8 steal/migration |
| Destruction | safe destruction unlocked/empty, debug assertion locked/queued |
| Exactly-once | handoff winner resumes once, late timer/cancel does not republish |
| Immediate no-publish | immediate lock does not create runnable ticket |

### 17.2 Causal proof requirements

- No sleep-based causal proof.
- Deterministic phase control for race tests.
- Owner-before-publication test via internal test seam.
- No test hooks in installed public headers.

### 17.3 Migration test

```text
Fiber A locks AsyncMutex on Worker W0
Fiber A is stolen by Worker W1 (real E8 steal)
Fiber A resumes on W1
Fiber A unlocks AsyncMutex → succeeds
```

#### 17.3.1 Corrective history (T19 blocker-execution race)

T19 passed through three correctives (full record: `docs/reviews/E12-C-REVIEW.md` §L):

- **Corrective-1** rewrote T19 with an E8-T1/T3 steal pattern but left the run
  hanging/nondeterministic (DRAIN + `pending_spawn_` routing).
- **Corrective-2** switched to `run_live(2)` and established the intended causal
  trace (acquire on W0 → migrate to W1 while owning → unlock on W1). It gated
  `flag_wake` on `a_suspended` + `waiting_count()>0`. **Defect:** both flip
  BEFORE W0 pops `f_blocker`, so `f_blocker` was still stealable in W0's queue
  when the coordinator freed W1. Passing repetitions only *inferred* that
  `f_blocker` was running on W0.
- **Corrective-3** closes the race with an explicit observed
  `blocker_running` handshake: `f_blocker` asserts `current_worker_id()==0` and
  release-stores `blocker_running=true` as its first meaningful act. The
  coordinator must observe `a_suspended` AND `waiting_count()>0` AND
  `blocker_running` before setting `flag_wake`. Observing `blocker_running`
  proves `f_blocker` is `ws->current` on W0 (written from W0's TLS context),
  hence popped from W0's runnable queue and no longer stealable. This state is
  OBSERVED, not inferred. T19's coordinator waits were also made bounded
  (test-local `bounded_wait`/`bounded_wait_pred`), so a failed gate fails the
  test instead of hanging.
- **Corrective-4** removes the unsynchronized `waiting_count()` observation from
  the T19 coordinator. `sched.waiting_count()` accessed Scheduler guarded
  containers (`waiting_ready_`) without `global_mtx_` — a genuine C++ data race
  (undefined behaviour) that TSan silence did not make valid. The coordinator
  now gates solely on `a_suspended` + `blocker_running`. `blocker_running` is
  the authoritative suspension and W0-occupancy proof: because `f_blocker` was
  queued behind `fA` on W0's `local_runnable`, it can execute only after `fA`
  has completed `await_ready_flag` (registered in `waiting_ready_`, committed
  Waiting via `make_waiting()`, and context-switched away). The unused
  `wake_released` diagnostic and `bounded_wait_pred` utility are also removed.

Ordered causal checkpoints now asserted by T19:

```text
A_LOCKED_ON_W0          fA acquires on W0
A_WAITING_WHILE_OWNING  fA suspends while owning (source-order marker)
BLOCKER_RUNNING_ON_W0   f_blocker is ws->current on W0 (anti-race handshake)
WAKE_RELEASED           coordinator sets flag_wake (a_suspended + blocker_running)
A_RESUMED_ON_W1         fA resumes on the thief W1
A_UNLOCKED_ON_W1        fA unlocks on W1 (unlock_worker == 1)
BLOCKER_RELEASED        coordinator releases f_blocker (after unlock observed)
F2_ACQUIRED             f2 acquires the now-free mutex
```

---

## 18. Non-goals

```text
E12-D Condition                    — separate E12 subphase
release-and-wait atomic API        — Condition territory
notify_one / notify_all            — Condition territory
RAII condition wait composition    — Condition territory
RwLock                             — E12-F
Queue                              — E12-E
Select / multi-wait                — E13
recursive Mutex                    — deferred
priority inheritance               — deferred
deadlock detection                 — deferred
lock graph runtime                 — deferred
owner abandonment recovery         — deferred
cross-process Mutex                — deferred
OS futex Mutex                     — deferred
generic Lockable / AwaitableLock   — Hard Rule 5/6
grant framework                    — Hard Rule 5/6
```

---

## 19. Implementation readiness verdict

```text
E12-C-PREPARATION-CORRECTIVE-1: COMPLETE
E12-C-PREPARATION-CORRECTIVE-2: COMPLETE
E12-C-PREPARATION-CORRECTIVE-3: COMPLETE
E12-C-PREPARATION-CORRECTIVE-4: COMPLETE
E12-C-PREPARATION-CORRECTIVE-5: COMPLETE
E12-C-PREPARATION-CORRECTIVE-5-REAUDIT: PASS
E12-C-PREPARATION: CLOSED
E12-C-IMPLEMENTATION: READY
```

All material semantic decisions are resolved:

```text
naming:              AsyncMutex (§3)
owner identity:      Fiber* (§4)
public API:          §5 (no is_locked; no RAII guard)
calling context:     Fiber for lock/unlock; any thread for cancel (§6)
misuse contracts:    §7 (all classified)
ownership model:     single Fiber* owner, no redundant bool (§8)
handoff model:       H1 direct (§9)
FIFO / barging:      FIFO, no barging (§2 M-H1–M-H4)
seam:                MUTEX-HANDOFF-ONE, owner before publication (§10)
admission closure:   §14.5 (atomic register-recheck-outcome, no interleaving)
deadline:            resource-first (§12)
cancellation:        queue-membership gate, any-thread (§12)
destruction:         caller contract violation (§13)
formal model:        §14 (collapsed admission, no admissionPhase,
                     queue = only Suspended epochs, publication discipline)
properties:          §15 (14 invariants, honest classification)
negative models:     §16 (11 focused models)
runtime tests:       ~27 deterministic (§17)
```

The architecture is viable. One private Scheduler seam (MUTEX-HANDOFF-ONE) is
required. No broad redesign is needed. Preparation was closed by independent
adversarial re-audit returning PASS.

---

## 20. As-built implementation evidence (E12-C-IMPLEMENTATION-1)

> This section records the as-built production + formal + test evidence. It is
> NOT preparation authority; preparation history (§§1–19) is preserved unchanged
> above. An independent adversarial implementation review is still required
> (E12-C-IMPLEMENTATION: REVIEW-REQUIRED).

### 20.1 Public API as built

```cpp
// include/sluice/async/async_mutex.hpp
namespace sluice::async {
class AsyncMutex {
public:
    explicit AsyncMutex(Scheduler& scheduler) noexcept;
    ~AsyncMutex();  // debug-asserts owner_ == nullptr
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;
    [[nodiscard]] bool try_lock();
    void lock(WaitNode& node);
    void lock_until(WaitNode& node, Scheduler::deadline_t deadline);
    [[nodiscard]] bool cancel(WaitNode& node);
    void unlock();
private:
    Scheduler& scheduler_;
    Fiber* owner_;        // SOLE ownership authority; nullptr == NoOwner
    WaitQueue waiters_;   // private; not publicly reachable
};
}
```

No `is_locked()`, `owner()`, or `wait_queue()` accessor. The existing
synchronous `Mutex` (`include/sluice/async/mutex.hpp`) is unchanged.

### 20.2 Production file topology

| File | Role |
| ---- | ---- |
| `include/sluice/async/async_mutex.hpp` | Thin-shell AsyncMutex (state + 6 one-line forwards) |
| `include/sluice/async/scheduler.hpp` | Private `mutex_*` seam declarations (lines ~466–559) |
| `src/async/scheduler.cpp` | `mutex_*` implementations (lines 1835–2126) |
| `tests/async_test_control_internal.hpp` | `PhaseTag::e12_mutex_handoff_before_publication` (phases[7]) |
| `tests/async_test_control.hpp` | `E12MutexSeam` facade |
| `tests/async_test_control.cpp` | `std::size(c->phases)` loop (no hardcoded count) |
| `tests/e12_async_mutex_test.cpp` | T0–T22 deterministic + 500/500 coordination |
| `tests/e12_async_mutex_authority_probe.cpp` | Negative compile probe (must-not-compile) |

### 20.3 Private Scheduler helpers

```text
mutex_try_lock              scheduler.cpp:1835   global_mtx_ -> waiters.mtx()
mutex_lock                  scheduler.cpp:1863   admission closure (register+recheck+suspend)
mutex_lock_until            scheduler.cpp:1939   + E11 timer; resource-first precedence
mutex_cancel                scheduler.cpp:2026   any-thread; queue-identity gate
mutex_handoff_one_locked    scheduler.cpp:2056   MUTEX-HANDOFF-ONE (owner before publication)
mutex_unlock                scheduler.cpp:2099   handoff or UnlockNoWaiter
```

### 20.4 MUTEX-HANDOFF-ONE — owner-before-publication source order

`mutex_handoff_one_locked` (`src/async/scheduler.cpp:2056–2097`), caller holds
`global_mtx_`:

```text
2072  LockGuard qlk(waiters.mtx());              // global_mtx_ -> waiters.mtx()
2073  WaitNode* won = waiters.wake_one_locked(); // resolve FIFO head Woken + unlink
2075  Fiber* f = won->fiber();
2076  assert(f != nullptr);                       // non-null winner (§10.6)
2079  owner = f;                                  // <-- owner commit (BEFORE publication)
2080–2088  [internal-testing phase seam: paused here]
2089  retire_timer_for_node_locked(*won);         // E11 I4 timer closure
2090  --waiting_waitq_count_;
2093  if (f->make_runnable()) {                    // <-- publication guard
2094      route_runnable_locked(f, g_worker);      // <-- runnable publication (AFTER owner)
2095  }
```

Load-bearing order: `owner = f` (line 2079) BEFORE `make_runnable` (line 2093)
and `route_runnable_locked` (line 2094). No intermediate `owner = nullptr` on
the handoff path. The deterministic test seam
(`PhaseTag::e12_mutex_handoff_before_publication`) pauses at lines 2080–2088,
proving owner==winner and winner-not-yet-published (test T5).

### 20.5 Lock order

Every `mutex_*` path acquires `Scheduler::global_mtx_` first, then
`waiters_.mtx()` inside it (unchanged E10/E11/E12-A/E12-B lock order). No
inverse acquisition. `mutex_handoff_one_locked` takes `waiters.mtx()` at line
2072 inside the caller-held `global_mtx_`. No `context_switch` while holding
`waiters.mtx()` (the switch in `mutex_lock`/`mutex_lock_until` is outside the
lock scope). No user callback under internal locks; no allocation or generic
callback in the handoff hot path.

### 20.6 Formal verification evidence

Gate: `scripts/verify-e12-async-mutex-formal.sh` (TLC2, jar `/tmp/tla2tools.jar`,
rev `227f61b`, TLC runtime `2026.07.09.134028`).

```text
PASS  E12AsyncMutex [safety, 19 invariants]  (490943 distinct states, depth 13)
CEX   NEG-M1  NonOwnerUnlock              -> InvUnlockAuthority
CEX   NEG-M2  RecursiveAcquire            -> InvRecursiveForbidden
CEX   NEG-M3  NonFIFOGrant                -> InvFIFOGrant
CEX   NEG-M4  HandoffFreeWindow         -> InvNoOwnerlessQueuedDemand
CEX   NEG-M5  GrantWithoutOwnerCommit     -> InvGrantOwnerCommit
CEX   NEG-M6  PublicationWithoutGrantCoupling -> InvGrantPublicationCoupling
CEX   NEG-M7  AdmissionClosureFailure     -> InvAdmissionClosure
CEX   NEG-M8  CancelRevokesHandoff        -> InvGrantFinality
CEX   NEG-M9  DeadlineRevokesHandoff      -> InvGrantFinality
CEX   NEG-M10 ImmediatePublication        -> InvPublicationRequiresSuspensionOrHandoff
CEX   NEG-M11 DestructionWhileOwnedOrQueued -> InvDestructionPrecondition
OK    WRONG-PROPERTY gate (NEG-M3 vs InvGrantOwnerCommit: passes, not mis-flagged)
OK    COMPILE-PROBE gate (raw WaitQueue/owner/is_locked bypass sealed)
```

### 20.7 Runtime test inventory

`tests/e12_async_mutex_test.cpp` — 23 cases (T0–T22):

| Category | Tests |
| -------- | ----- |
| Construction / immediate | T0–T2 (construct/destroy, try_lock immediate + recursive-fails, immediate lock Woken + unlock-no-waiter) |
| FIFO handoff | T3 (two-waiter Owned→Owned→Owned) |
| No barging | T4 (newcomer try_lock fails while queued), T21 (three-party: newcomer fails while W1+W2 queued) |
| Owner-before-publication | T5 (deterministic phase seam) |
| Admission closure | T6 (owner releases in admission window; no strand) |
| Cancellation | T7–T10 (cancel suspended + repeated-false, cancel-after-handoff false, wrong-mutex same/different Scheduler, external OS-thread cancel) |
| Deadline precedence | T11–T14 (free+due→Woken, owned+due→Expired, unlock-vs-timer both orderings) |
| Cancel races | T15–T16 (cancel-wins, handoff-wins; no republish) |
| Exactly-once | T17 (one resolve, one publication, one resume) |
| Destruction | T18 (safe unlocked/empty) |
| Real E8 migration | T19 (lock on W0, deterministic steal to W1, unlock on W1; explicit observed `blocker_running` handshake — Corrective-3/4 closure; no unsynchronized Scheduler reads) |
| Coordination 500/500 | T20 (500-iteration assertion-only gate) |
| Three-party no-barging | T21 (F0→W1→W2 handoff; newcomer cannot barge) |
| Cancelled-head handoff | T22 (W1 cancelled; newcomer cannot barge; W2 receives ownership) |

### 20.8 Coordination stress result

```text
E12-C coordination: 500 / 500 PASS
```

(T20: 500 iterations, K=2 waiters each, total_resolved == ITERS*K; cancel and
handoff each win at least once; no queue/timer leak per iteration. Per repo
convention the gate is assertion-only, matching e12_semaphore_test T30 /
e12_event_test.)

### 20.9 Sanitizer + regression results

```text
TSan     (clang -m tsan):     ALL TESTS PASSED (23 cases)
ASan     (clang -m asan):     ALL TESTS PASSED (23 cases)
UBSan    (clang -m ubsan):    ALL TESTS PASSED (23 cases)
Valgrind (clang -m valgrind): ALL TESTS PASSED, 0 errors, 0 leaks (5009 allocs / 5009 frees)

Re-confirmed after Corrective-4 (unsynchronized `waiting_count()` removed):
freshly rebuilt per mode (no reused binaries). TSan executes the corrected T19
handshake (no unsynchronized Scheduler state reads); full suite clean under
TSan/ASan/UBSan.

Regression:
  E10 (e10_wait_queue_test):      ALL TESTS PASSED
  E10 (e10_corrective_c5_test):   ALL TESTS PASSED
  E11 (e11_timer_wait_test):      ALL TESTS PASSED
  E12-A (e12_event_test):         ALL TESTS PASSED
  E12-B (e12_semaphore_test):     ALL TESTS PASSED
  E12-C (e12_async_mutex_test):   ALL TESTS PASSED (23 cases, 0 fail, 0 skip)
  E8 steal (e8_steal_test e8_t3): 500 / 500 PASS
  full suite:                     ALL TESTS PASSED (0 fail, 0 skip)
```

#### 20.9.1 Migration 500/500 gate (post Corrective-4)

```text
T19 ownership migration: 500 / 500 PASS   (e12_async_mutex_test e12_mtx_t19_real_migration)
E8 steal regression:     500 / 500 PASS   (e8_steal_test e8_t3)
E12-C coordination:      500 / 500 PASS   (e12_async_mutex_test e12_mtx_t20_coordination_500)
```

Runner: `scripts/verify-e8-stability.sh release <binary> <filter> 500`. Each T19
invocation started the binary, executed T19, asserted `blocker_running` on W0,
asserted `acquire_worker==0` and `unlock_worker==1`, and exited successfully:
0 launch failures, 0 retries, 0 skips.

### 20.9.1 No-barging formal closure

```text
No-barging is proved by a composition of four TLC-checked properties:

  InvNoOwnerlessQueuedDemand:
      owner = NoOwner => EligibleQueue = <<>>
  (no ownerless queued-demand state)

  InvImmediateAcquireRequiresEmptyEligiblePreQueue:
      lastAction in {TryLockSuccess, LockImmediate, LockUntilImmediate,
                     LockAdmissionAcquire, LockUntilAdmissionAcquire}
      => preOwner = NoOwner /\ EligiblePreQueue = <<>>
  (immediate acquire requires empty eligible pre-queue)

  InvFIFOGrant:
      lastAction = UnlockHandoff
      => lastGrantedEpoch = Head(EligiblePreQueue)
  (FIFO handoff)

  InvGrantOwnerCommit:
      lastAction = UnlockHandoff
      => owner = epochFiber[lastGrantedEpoch]
  (exact winner owner commit)

Theorem (NoBargingByTopology):
  InvNoOwnerlessQueuedDemand
  /\ InvImmediateAcquireRequiresEmptyEligiblePreQueue
  /\ InvFIFOGrant
  /\ InvGrantOwnerCommit
  imply an arriving Fiber cannot bypass an older eligible queued waiter.

NEG-M4 HandoffFreeWindow breaks the handoff topology by creating an
ownerless queued-demand state (owner := NoOwner after UnlockHandoff) and
is caught by InvNoOwnerlessQueuedDemand.
```

### 20.10 Production/formal refinement map

| Formal action | Production path | Linearization point | Publication? |
| ------------- | --------------- | ------------------- | -----------: |
| TryLockSuccess | `mutex_try_lock` (1835) | `owner = me` under global_mtx_+mtx | no |
| TryLockFailure | `mutex_try_lock` (1835) | return false (no mutation) | no |
| LockImmediate | `mutex_lock` (1863) | inline `wake_node_locked` + `owner=me` | no |
| LockAdmissionAcquire | `mutex_lock` (1863) | recheck `wake_node_locked` + `owner=me` | no |
| LockAdmissionSuspend | `mutex_lock` (1863) | `make_waiting` (Registered in queue) | not yet |
| LockUntilImmediate | `mutex_lock_until` (1939) | inline Woken + `owner=me` + retire timer | no |
| LockUntilAdmissionAcquire | `mutex_lock_until` (1939) | recheck Woken + `owner=me` | no |
| LockUntilAdmissionSuspend | `mutex_lock_until` (1939) | `make_waiting` (timed) | not yet |
| LockUntilDue | `mutex_lock_until` (1939) | inline `expire_locked` | no |
| UnlockNoWaiter | `mutex_unlock` (2099) | `owner = nullptr` | no |
| UnlockHandoff | `mutex_handoff_one_locked` (2056) | owner commit / wake coupling (2079/2094) | yes |
| CancelSuspended | `mutex_cancel` (2026) | `cancel_locked` CAS | yes |
| ExpireSuspended | `pump_deadlines_locked` → `expire_wait` | `expire_locked` CAS | yes |
| terminal attempts | `mutex_cancel` / timer (loser) | losing CAS/membership gate | no |
| Destroy | `~AsyncMutex` (async_mutex.hpp) | precondition assert | no |

Component map: owner authority = `AsyncMutex::owner_` (Fiber*, sole); current
Fiber lookup = `g_worker->current`; queue authority = `waiters_` (private
WaitQueue); timer retirement = `retire_timer_for_node_locked`; waiting-count
closure = `waiting_waitq_count_`; make_runnable = `Fiber::make_runnable`;
route_runnable = `route_runnable_locked`; wake signaling =
`signal_wake_locked`; test seam placement = `mutex_handoff_one_locked:2080–2088`
(`PhaseTag::e12_mutex_handoff_before_publication`, internal-testing only).

### 20.11 Commit attribution

```text
ba0459b  formal(async): model E12-C AsyncMutex protocol        (Commit C)
7a2f2ac  feat(async): implement Fiber-aware AsyncMutex         (Commit D)
26c2902  test(async): close E12-C handoff and race coverage    (Commit E)
(F)      docs(async): record E12-C implementation evidence     (this commit)
(G)      formal(async): close E12-C no-barging proof topology  (NEG-M4 HandoffFreeWindow)
(H)      test(async): restore full Clang regression gate       (E10 warning + T21/T22)
```

Preparation commits A (`c640d6e`) and B (`4716ecf`) are on the base branch.

### 20.12 Known non-goals (unchanged)

E12-D Condition, release-and-wait atomic API, notify_one/notify_all, RwLock,
Queue, Select/multi-wait, recursive Mutex, priority inheritance, deadlock
detection, owner-abandonment recovery, generic Lockable/AwaitableLock, and
grant framework remain deferred non-goals (§18). They are not defects.
