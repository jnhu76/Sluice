# E9 SchedulerWakeHandle Callback-Lifetime — TLA+ Model

Narrow TLA+ model of the **SchedulerWakeHandle callback-lifetime lease**
protocol, closing the E9-LIFETIME-CORRECTIVE defect.

## The load-bearing question

```
May a SchedulerWakeHandle::notify() callback dereference the Scheduler
AFTER the Scheduler destructor has destroyed its wake members?
```

## Answer (LOCKED repair)

`Control::mtx` is the **callback lease**. `notify()` holds it from the
validity check through the entire Scheduler wake callback; `~Scheduler`
acquires the same mutex before invalidating. Exactly two linearizations
are legal:

```
Notify wins:    N holds the lease through the callback; D's mutex
                acquisition BLOCKS until the callback returns; invalidation
                and member destruction happen strictly after.

Destructor wins: D invalidates and releases; N acquires, observes
                dead/null, returns false — no Scheduler dereference.
```

No third result is permitted.

## Files

- `E9WakeHandleLifetime.tla`               — the CORRECT callback-lease protocol.
- `E9WakeHandleLifetime.cfg`               — TLC safety config (LIFE-Inv1..6).
- `E9WakeHandleLifetimeLiveness.cfg`       — TLC temporal config (LIFE-Inv7).
- `E9WakeHandleLifetimeBuggySnapshot.tla`  — NEGATIVE model: the shipped
  snapshot-release defect (HEAD 1e8333d). Differs ONLY in "lease released
  before callback."
- `E9WakeHandleLifetimeBuggySnapshot.cfg`  — TLC config reproducing the
  callback-after-destruction counterexample.
- `README.md`                              — this file + refinement map.

## What this model is NOT

- It does NOT extend `E9ParkWake.tla` with Scheduler destruction internals.
  The E9 park/wake model remains CLOSED.
- It does NOT model park admission, RunMode, the MW classifier, or the wake
  epoch. Those are unchanged by this corrective.
- It does NOT model `signal_wake_locked` / `notify_external_wake` semantics;
  the Scheduler wake callback body is one atomic lease-held step
  (`NotifyCallbackReturn`).

## State axes (spec 6)

```
schedulerState  in {Alive, Invalidated, Destroyed}
notifyPhase     in {Idle, Validating, Callback, ReturnedTrue, ReturnedFalse}
controlOwner    in {None, Notifier, Destructor}   -- Control::mtx holder
destroyRequested (bool)
```

The correct model distinguishes a **validated Scheduler snapshot** from a
**valid callback lease** by making the lease a first-class variable
(`controlOwner`). Validation and callback are NOT collapsed into one
atomic action: `NotifyBeginCallback`'s precondition is `controlOwner =
Notifier /\ schedulerState = Alive`, so the lease must be held for the
callback to begin.

## Protocol actions (spec 7)

| action               | precondition highlights                                        |
| -------------------- | -------------------------------------------------------------- |
| `NotifyAcquire`      | `Idle, controlOwner=None` → `controlOwner=Notifier, Validating`|
| `NotifyRejectDead`   | `Validating, controlOwner=Notifier, schedulerState # Alive` → `ReturnedFalse` (no Callback) |
| `NotifyBeginCallback`| `Validating, controlOwner=Notifier, schedulerState=Alive` → `Callback` |
| `NotifyCallbackReturn`| `Callback, controlOwner=Notifier` → `ReturnedTrue`           |
| `NotifyRelease`      | `Returned{True,False}, controlOwner=Notifier` → `None, Idle`   |
| `DestructorAcquire`  | `controlOwner=None, schedulerState=Alive` → `Destructor`       |
| `DestructorInvalidate`| `Destructor, Alive` → `Invalidated`                          |
| `DestructorRelease`  | `Destructor, Invalidated` → `None`                             |
| `DestroyMembers`     | `controlOwner=None, Invalidated` → `Destroyed`                 |

## Formal properties (spec 8)

| property | classification | statement |
| -------- | -------------- | --------- |
| LIFE-Inv1 `LifeInv1CallbackRequiresAlive` | state invariant | `notifyPhase=Callback => schedulerState=Alive` |
| LIFE-Inv2 `LifeInv2CallbackOwnsLease`     | state invariant | `notifyPhase=Callback => controlOwner=Notifier` |
| LIFE-Inv3 `LifeInv3NoDestructorDuringCallback` | state invariant | `notifyPhase=Callback => controlOwner # Destructor` |
| LIFE-Inv4 `LifeInv4DestroyedNoCallback`   | state invariant | `schedulerState=Destroyed => notifyPhase # Callback` |
| LIFE-Inv5 `LifeInv5PostInvalidationNoCallback` | state invariant | `schedulerState # Alive => notifyPhase # Callback` |
| LIFE-Inv6 `LifeInv6CallbackReleasesBeforeDestruction` | state invariant | `notifyPhase=Callback => schedulerState=Alive` (callback-vs-destruction binding; structural via `DestroyMembers` requiring `controlOwner=None`) |
| LIFE-Inv7 `Life7DestroyedAfterCallbackAndRequest` / `Life7bDestroyProgressesAfterRequest` | temporal | under fair mutex arbitration, `destroyRequested` (or `ReturnedTrue /\ destroyRequested`) leads-to `Destroyed` |

### LIFE-Inv7 fairness (honest classification)

The destructor steps use **strong fairness** (SF), the Notifier
lease-held steps use **weak fairness** (WF). Rationale (also inline in
the `.tla`):

- A spinning Notifier (repeated distinct `notify()` calls) makes the
  destructor's mutex acquisition *intermittently* enabled; weak fairness
  admits a starvation cycle. Spec 8 phrases the obligation as "legitimate
  fairness of the destructor's mutex acquisition," which is strong
  fairness: if enabled infinitely often, eventually occurs.
- The Notifier steps made fair are ONLY the ones fired AFTER the lease is
  acquired (Validate→Reject/Begin, Callback→Return, Returned→Release): a
  mutex holder always eventually releases. This is a mutex-semantics
  obligation, not producer fairness.
- We do NOT assume fairness on `NotifyAcquire` (a Notifier that never
  calls `notify()` does not block destruction) and do NOT use global
  `WF(Next)`.

## Running

```
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_wake_handle_lifetime/E9WakeHandleLifetime.cfg \
  docs/spec/e9_wake_handle_lifetime/E9WakeHandleLifetime

java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_wake_handle_lifetime/E9WakeHandleLifetimeLiveness.cfg \
  docs/spec/e9_wake_handle_lifetime/E9WakeHandleLifetime

java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_wake_handle_lifetime/E9WakeHandleLifetimeBuggySnapshot.cfg \
  docs/spec/e9_wake_handle_lifetime/E9WakeHandleLifetimeBuggySnapshot
```

## Results

| model | generated | distinct | depth | result |
| ----- | --------: | -------: | ----: | ------ |
| `E9WakeHandleLifetime` (correct, safety LIFE-Inv1..6) | 15 | 12 | 7 | **all invariants PASS — no error** |
| `E9WakeHandleLifetime` (liveness LIFE-Inv7) | 15 | 12 (24 total) | 7 | **all temporal properties PASS** |
| `E9WakeHandleLifetimeBuggySnapshot` (snapshot release) | 18 | 15 | 6 | **`LifeInv1`/`LifeInv4` counterexample** — callback-after-invalidation |

## Buggy counterexample (the shipped defect)

The trace reproduces the exact snapshot→release→invalidate→callback UAF
(spec 9). `LifeInv1 (Callback => Alive)` isolates the callback-after-
invalidation state:

```
S1: Init                              scheduler=Alive, phase=Idle, owner=None
S2: NotifyAcquire                     phase=Validating, owner=Notifier (lease HELD)
S3: NotifyValidateRelease   DEFECT    phase=SnapshotReleased, owner=None,
                                      buggySnapshotValid=TRUE (lease RELEASED)
S4: NotifyBeginCallback              phase=Callback, owner=None (NO lease held)
S5: DestructorAcquire               owner=Destructor (destructor grabs the
                                      now-free lease WHILE the callback is running)
S6: DestructorInvalidate            scheduler=Invalidated, phase STILL Callback
   -> DestroyMembers -> callback dereferences destroyed members (UAF)
```

The defect is precisely: **lease released before callback**. The buggy
model introduces NOTHING else — the `alive` check, destructor invalidation,
single destruction, and mutex semantics are all faithful to production.

### Correct-vs-buggy semantic diff

```
Correct: notify retains the Control lease (controlOwner=Notifier) through
         the entire callback; NotifyRelease is the only path back to None.
         DestructorAcquire is impossible while controlOwner=Notifier, so
         invalidation/destruction cannot interleave with a callback.

Buggy:   notify acquires the lease, validates, then RELEASES the lease
         (NotifyValidateRelease) before entering the callback. The captured
         Scheduler pointer (buggySnapshotValid) is dereferenced in the
         callback with NO lease. The destructor can acquire the freed lease,
         invalidate, and destroy members while the callback is in flight.
```

No other defect differences are required for the counterexample.

## Refinement map (spec 10)

| formal concept / action | production representation / path |
| ----------------------- | -------------------------------- |
| `schedulerState = Alive` | `Control::alive == true && scheduler != nullptr` |
| `Invalidated` | `~Scheduler()` stored `alive=false` / `scheduler=nullptr` (destructor body, under `Control::mtx`) |
| `Destroyed` | Scheduler wake members (`wake_mtx_`, `wake_epoch_`, `wake_cv_`) past their lifetime (destructor body returned; member destruction order) |
| `controlOwner` | `Control::mtx` ownership (acquired in `notify`, `bound`, `~Scheduler`) |
| `NotifyAcquire` | `SchedulerWakeHandle::notify()` `std::lock_guard<std::mutex> lk(control_->mtx)` |
| `NotifyRejectDead` | `if (!alive || sched == nullptr) return false;` (short-circuit) |
| `NotifyBeginCallback` / `NotifyCallbackReturn` | the `sched->notify_external_wake()` call (→ `signal_wake_locked()`) |
| `NotifyRelease` | `lock_guard` scope exit at the end of `notify()` |
| `DestructorAcquire` | `~Scheduler` `std::lock_guard<std::mutex> lk(wake_control_->mtx)` |
| `DestructorInvalidate` | `wake_control_->alive = false; wake_control_->scheduler = nullptr;` |
| `DestroyMembers` | Scheduler member destruction after the destructor body returns |

### Lifetime authority statement

```
Control::mtx does not extend Scheduler object ownership.

It serializes the callback-duration lease against Scheduler invalidation.

The Scheduler cannot begin member destruction until the destructor has
acquired the lease mutex and invalidated the control block.

Therefore every callback that validated Alive while owning the lease
finishes before invalidation can complete.
```

This is a **mutex-serialized callback lease**, NOT shared ownership and
NOT reference counting. A stale `SchedulerWakeHandle` may exist after its
Scheduler is destroyed; its later `notify()` acquires the (still-live)
`Control::mtx`, observes `alive=false`, and returns false — a safe no-op.

## What this model does NOT cover

- The E9 park/wake protocol (park admission, RunMode, MW classifier, wake
  epoch) — see `docs/spec/e9_park_wake/`. CLOSED; this corrective does not
  touch it.
- The Fiber / AsyncBackend / AsyncIoContext internals — signal-only external
  producer boundary (9.4.9) is preserved; `notify_external_wake` /
  `signal_wake_locked` semantics are unchanged.
- E10 WaitNode / cancellation-safe wait queue (next frontier).
