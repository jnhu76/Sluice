# E12-A — Async Event (Persistent Manual-Reset)

> Status: CLOSED
>
> Authority baseline: E10 CLOSED
> ([`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)); E11 CLOSED at
> `7715808` ([`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md)).
> Preparation: [`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md)
> (E12-PREP READY).
>
> This document is the authoritative E12-A Event specification, now refined by
> the verified implementation. Formal model:
> [`docs/spec/e12_event/`](spec/e12_event/). Formal gate:
> [`scripts/verify-e12-event-formal.sh`](../scripts/verify-e12-event-formal.sh).

---

## 1. Scope

E12-A is the first user-facing asynchronous synchronization primitive built on
the closed E10/E11 wait substrate. It establishes:

```text
Event persistent readiness
        +
multi-waiter Event registration
        +
lost-set admission closure
        +
set-all wait resolution
        +
deadline composition
        +
cancellation composition
        +
Scheduler park/wake liveness
```

without a second ready-flag wait map, a private Event timer, a private Event
cancellation winner, direct Fiber resume, direct runnable enqueue, or a second
WaitNode winner authority.

### 1.1 Out of scope (explicitly deferred)

```text
auto-reset Event
pulse Event
edge-triggered Event
one-shot Event type
Event close()
Event cancel_all()
task-level cancellation token integration
generic predicate waiting
generic Waitable
generic wake-many public WaitQueue API
Semaphore / Async Mutex / Condition / Queue / RwLock / Select
winner-before-publication grant framework
```

A manual-reset Event never reset by the caller naturally behaves as a latch; no
separate latch type is needed.

---

## 2. Semantic model

A **persistent manual-reset Event**:

```text
UNSET ──set()──> SET
 SET ──reset()──> UNSET
```

- `SET` is **level-triggered persistent readiness**: a late waiter that calls
  `wait()` while `SET` observes readiness immediately and does NOT suspend.
- While `SET`, `wait()` returns without suspending.
- `reset()` returns to `UNSET`; subsequent `wait()` registers a WaitNode and
  suspends.
- `set()` is **idempotent**: `set()` on an already-SET Event is a no-op (no
  extra wake).
- `reset()` does NOT cancel a registered waiter; a waiter already in the queue
  remains governed by future `set()`, deadline, or cancellation.

### 2.1 Set is persistent readiness, NOT wait resolution

`SET` is resource readiness. A wait epoch still has its own resolution
competition. The Event readiness state is NOT the wait-resolution winner
authority — `WaitNode::resolve_` is. For example:

```text
W1 waits, W2 waits, W3 waits
Event set()
W1 RESOURCE_WAKE wins
W2 TIMER_EXPIRE wins
W3 CANCEL wins
```

is legal if the three races linearize that way. The final outcomes may differ
per waiter. Event state remains `SET`.

### 2.2 set() is broadcast semantics, NOT a multi-wait winner

`set()` transitions `UNSET -> SET` exactly once per set epoch and attempts
RESOURCE_WAKE resolution for **every** currently registered Event wait epoch.
Conceptually:

```text
ResolveResource(W1)
ResolveResource(W2)
ResolveResource(W3)
...
```

Each `WaitNode` remains independent — one CAS per waiter, one runnable
publication per winning epoch. This is NOT one multi-wait winner, NOT a shared
wake CAS, NOT Select, NOT a broadcast ticket.

---

## 3. Public API

```cpp
namespace sluice::async {

class Event {
public:
    explicit Event(Scheduler& scheduler, bool initially_set = false);

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    ~Event();

    [[nodiscard]] bool is_set() const noexcept;

    void set();
    void reset();

    void wait(WaitNode& node);
    void wait_until(WaitNode& node, Scheduler::deadline_t deadline);

    // Test/integration seam: the underlying WaitQueue. Used by tests to call
    // Scheduler::cancel_wait (the inherited E10 cancellation surface).
    WaitQueue& wait_queue() noexcept;
};

}  // namespace sluice::async
```

### 3.1 Constructor / initial state

`Event(scheduler, initially_set)`: binds to `scheduler` for the Event's lifetime.
`set_` is initialized to `initially_set`. The Event borrows `Scheduler&`; the
Scheduler must outlive the Event (matches the existing borrowed-Scheduler
integration style).

### 3.2 Scheduler binding

Event is bound to one Scheduler for its lifetime. `set()` may be called from an
external OS thread; wait resolution reaches that Scheduler's
`waiting_waitq_count_`, runnable routing, wake source, and deadline service. One
Event's wait epochs must NOT be registered with multiple Scheduler instances.

### 3.3 is_set

Lock-free `set_.load(acquire)`. Returns the persistent readiness state.

### 3.4 set

```text
linearization: global_mtx_ critical section
    set_ <- SET (acq_rel)
    drain: loop wake_wait_one_locked until empty
        each winner: resolve_(Woken) + unlink + retire timer + dec count
                     + make_runnable + route_runnable_locked
```

`set()` on SET is idempotent (the store is a no-op; the drain finds an empty
queue only if no waiters remain — waiters admitted before the store are drained).
The drain is atomic w.r.t. `reset()` and admission (all under `global_mtx_`).

### 3.5 reset

```text
linearization: global_mtx_ critical section
    set_ <- UNSET (acq_rel)
```

`reset()` does NOT resolve a WaitNode, does NOT cancel a waiter, does NOT expire
a waiter, does NOT unlink a waiter, does NOT publish a runnable Fiber. A waiter
already registered before `reset()` remains governed by future `set()`, deadline,
or cancellation. `reset()` must NOT revoke an already-won Woken outcome (R2).

### 3.6 wait

```text
admission (global_mtx_ + q.mtx()):
    register_wait_locked(node, me)     // always register
    ++waiting_waitq_count_
    if set_.load(acquire):             // admission check (I5 analogue)
        wake_node_locked(node)         // resolve_(Woken) + unlink
        --waiting_waitq_count_
        retire_timer_for_node_locked   // no-op if no deadline
        return                         // node.outcome() == woken, no suspend
    me->make_waiting()
release locks
context_switch                         // only if still waiting
```

Result is queried via `node.outcome()`:
- `WaitOutcome::woken` — set() broadcast or admission-time SET
- `WaitOutcome::cancelled` — `Scheduler::cancel_wait`
- `WaitOutcome::expired` — deadline elapsed (via `wait_until`)

### 3.7 wait_until (deadline composition)

Same admission as `wait`, plus an E11 `TimerRegistration` created for this wait
epoch. The wait resolves when EXACTLY ONE cause wins the `resolve_` CAS:
- `set()` broadcast -> `Woken` (RESOURCE_WAKE)
- `cancel_wait` -> `Cancelled` (CANCEL)
- deadline elapsing -> `Expired` (TIMER_EXPIRE)

If the deadline is already due at admission, the E11 I5 path resolves `Expired`
inline (no suspend). If `set_` is observed at admission, the wait resolves
`Woken` inline (no suspend). The deadline registration is retired by a non-timer
winner in the same critical section (E11 I4 lifetime closure).

### 3.8 Cancellation exposure

`CANCEL` is an inherited WaitNode resolution cause. E12-A does NOT invent a new
public task-level cancellation token. `Event::wait` internally uses one ordinary
E10/E11 WaitNode epoch; `Scheduler::cancel_wait(event.wait_queue(), node)` remains
capable of cancelling that epoch at the substrate level.

### 3.9 Destruction contract

```text
Destroying Event while one or more Event wait epochs remain registered
is a caller contract violation.
```

Consequences:
- `~Event()` does NOT cancel waiters.
- `~Event()` does NOT wake waiters.
- `~Event()` does NOT synthesize RESOURCE_WAKE.
- `~Event()` does NOT create primitive-local cancellation.
- Caller must ensure all Event waits are terminal / unregistered before Event
  lifetime ends.

A debug assertion (`~WaitQueue`'s existing `assert(head_ == nullptr)`) is the
enforcement. In release builds, no recovery/cancel-all protocol is required. The
destructor does not own wait-resolution policy. This matches the existing
E10/WaitQueue lifetime-contract style.

---

## 4. Event state authority

| State                | Owner      | Writer                          | Reader              | Synchronization                          |
| -------------------- | ---------- | ------------------------------- | ------------------- | ---------------------------------------- |
| Event SET/UNSET      | Event      | `event_set_broadcast`/`event_reset` under `global_mtx_` | `is_set` (lock-free) + admission under `global_mtx_` | `std::atomic<bool>` acq_rel; mutations serialized by `global_mtx_` |
| Event WaitQueue      | Event      | `register_wait_locked`/`wake_*_locked`/`cancel_locked`/`expire_locked` under `q.mtx()` | structural queries under `q.mtx()` | `WaitQueue::mtx_` (taken under `global_mtx_`) |
| WaitNode outcome     | WaitNode   | `resolve_` CAS (one authority)  | lock-free `outcome()` | `std::atomic<State>` acq_rel CAS         |
| TimerRegistration    | Scheduler  | `await_event_wait_deadline` (create) / `retire_timer_for_node_locked` / `try_claim_expiry` | `is_active()`/`is_retired()` lock-free | `std::atomic<State>` + `global_mtx_` for heap |
| waiting_waitq_count_ | Scheduler  | admission (++), resolution (--) under `global_mtx_` | `classify_locked` under `global_mtx_` | `global_mtx_`                            |
| runnable publication | Scheduler  | `route_runnable_locked` under `global_mtx_` | worker loop drain | `global_mtx_` + `inbox_mtx_`             |

No load-bearing row has ambiguous writers.

---

## 5. Lost-set admission closure

The load-bearing Event protocol. A naive `if (!set_) { await_wait(...); }` is
FORBIDDEN — it admits a lost-set race (W checks UNSET; setter sets + wakes empty
queue; W registers + suspends forever).

### 5.1 Admission ordering

```text
Scheduler/Event readiness serialization (global_mtx_ + q.mtx()):

    register WaitNode
    establish wait accounting (++waiting_waitq_count_)

    final Event readiness decision (set_.load(acquire))

    if SET:
        close this wait as RESOURCE_WAKE (wake_node_locked -> resolve_(Woken))
        retire timer if a deadline registration exists
        do not commit suspension
        return Woken

    otherwise:
        commit Fiber waiting (make_waiting)

release serialization

context switch only if Fiber is still waiting
```

### 5.2 Required interleavings

**A — set before admission:**
```text
set -> SET
wait -> observes SET -> returns Woken -> never registers -> never suspends
```

**B — set during admission:**
```text
wait begins while UNSET
set occurs in the check/register/admission window
wait MUST either:
    observe SET during the final admission decision and not suspend
  or
    finish registration/suspension commitment and be resolved by set's drain
No third outcome. No stranded waiter.
```
Under `global_mtx_` serialization, B reduces to: either set runs first (W
observes SET at admission) or admission runs first (W registers, then set's
drain wakes it).

**C — set after suspension commit:**
```text
wait registered, Fiber waiting
set -> SET
RESOURCE_WAKE attempts resolution
winner: Woken, unlink, count closure, runnable publication
```

### 5.3 Linearization points

- `set()` linearization: `set_.store(true)` under `global_mtx_` (the store +
  drain are one atomic critical section).
- `reset()` linearization: `set_.store(false)` under `global_mtx_`.
- wait admission linearization: the `set_.load(acquire)` under `global_mtx_` +
  `q.mtx()` (register + check + commit are one atomic critical section).

---

## 6. set() broadcast protocol

### 6.1 Public set-all semantics

`set()` attempts RESOURCE_WAKE resolution for every currently registered Event
wait epoch whose readiness condition is satisfied by SET.

### 6.2 Implementation mechanism

```text
loop wake_wait_one_locked(waiters_) until no winner
```

This reuses the E10 Scheduler resolution seam: one WaitNode CAS per waiter, no
new WaitQueue wake-many authority, no Select-like abstraction. The loop is atomic
under `global_mtx_` (set's drain boundary is serialized with reset/admission).

### 6.3 Per-WaitNode RESOURCE_WAKE path

Each winner: `wake_one_locked` (resolve_(Woken) CAS + unlink) ->
`retire_timer_for_node_locked` (E11 I4) -> `--waiting_waitq_count_` ->
`make_runnable` + `route_runnable_locked` (exactly-one publication).

### 6.4 Mixed resource/deadline/cancel behavior

Each waiter's `RESOURCE_WAKE` vs `TIMER_EXPIRE` vs `CANCEL` competes through
that waiter's OWN `resolve_`. A timer that wins one waiter does not defeat
`set()`'s attempts on the other waiters; each epoch is resolved independently.
Wake-all is NOT a multi-wait winner.

---

## 7. set/reset epoch isolation (OLD_SET_WAKES_POST_RESET_WAITER)

### 7.1 The race

```text
S calls set() -> SET -> begins draining old waiters
R calls reset() -> UNSET
Wnew begins wait after reset -> registers
S continues old set() drain
```

Question: Can the old SET epoch incorrectly RESOURCE_WAKE Wnew?

### 7.2 Answer: NO

The synchronization domain is `global_mtx_`. `set()`'s entire drain (store +
loop) completes under one `global_mtx_` critical section. `reset()` and
admission also take `global_mtx_`. Therefore:

- If `set()` runs first: the drain completes (all old waiters woken) BEFORE
  `reset()` or Wnew's admission can run. Wnew registers after the drain; it is
  not in the queue during the drain.
- If `reset()` runs first: `set_` is UNSET; Wnew's admission observes UNSET and
  suspends. A later `set()` (S2) drains Wnew correctly.
- There is no interleaving where `set()`'s drain sees Wnew, because the drain
  holds `global_mtx_` for its entire duration.

No generation counter or snapshot is needed — `global_mtx_` serialization makes
the topology mechanically impossible.

### 7.3 T18 causal trace

```text
Event = UNSET
Wold registered
setter enters set epoch S1 (acquires global_mtx_)
S1 linearizes SET (set_ <- true)
S1 drains: wake_wait_one_locked(Wold) -> Wold Woken
S1 releases global_mtx_

reset attempts (acquires global_mtx_)
reset linearizes UNSET (set_ <- false)
reset releases global_mtx_

Wnew begins Event wait (acquires global_mtx_)
Wnew observes UNSET -> registers + suspends

(new set epoch S2)
S2 -> Wnew Woken
```

S1 cannot RESOURCE_WAKE Wnew (Wnew is not registered during S1's drain). This is
the runtime causal proof.

---

## 8. RunMode behavior

### 8.1 Live

```text
UNSET Event, registered Event waiter, external set producer possible
```
Live may park and later progress when Event is set (via the Scheduler wake
source: `route_runnable_locked` calls `signal_wake_locked`).

### 8.2 Drain

```text
UNSET Event, registered Event waiter, no current runnable/backend work
```
Drain preserves the existing MW-S3/STALLED policy. It MUST NOT park forever
waiting for Event merely because Event may be externally set later. The run
returns STALLED; the Event waiter remains unresolved; no hang. A later allowed
Scheduler invocation plus Event set may progress.

---

## 9. Formal invariants

### E1 — Single Resolution Winner
```text
one Event wait epoch -> at most one terminal resolution
```
Inherited from E10; rechecked in Event composition. The `resolve_` CAS is the
single authority.

### E2 — Single Runnable Publication
```text
one winning wait epoch -> at most one runnable publication
```
`make_runnable` is the publication guard (E7-T2 exactly-once).

### E3 — Event Admission Closure
```text
a wait whose final Event admission decision observes SET
cannot commit suspension as an unresolved Event waiter
```
Modeled via the admission phase: if `set_` is observed after registration, the
wait resolves `Woken` inline (no `make_waiting`).

### E4 — Persistent SET Liveness
```text
Event remains SET
and
an Event waiter remains Registered
->
under accepted Scheduler/resource-wake fairness,
the waiter eventually becomes terminal
```
The formal expression of set-releases-all. A safety implication pretending to
prove liveness is NOT acceptable.

### E5 — Reset Non-Resolution
```text
Reset alone does not change a WaitNode from Registered to terminal
```
`reset()` only flips `set_`; it does not touch any WaitNode.

### E6 — Set-Epoch Isolation (if load-bearing mechanism exists)
```text
a completed old SET epoch cannot resource-wake a waiter admitted after a later
RESET epoch
```
Production uses `global_mtx_` serialization (no generation counter); the correct
model represents this directly — the drain and reset/admission are mutually
exclusive critical sections.

---

## 10. Negative models

### NEG-EVENT-1 — Lost Set During Admission
```text
Broken: waiter checks UNSET; set -> SET (sees no waiters); waiter registers +
suspends; no final readiness closure.
Counterexample: Event SET + WaitNode Registered + waiter suspended forever.
Violated: EventAdmissionClosure (E3).
```

### NEG-EVENT-2 — Wake-One Manual-Reset Strands Waiter
```text
Broken: W1 + W2 registered; set -> SET; resource wake only W1; set-on-SET
idempotent; reset absent; deadline absent; cancel absent.
Counterexample: W2 remains Registered forever while Event remains SET.
Violated: EventSetDrainLiveness (E4).
Defect: SET resolves only one registered waiter instead of making all
registered SET-ready epochs progress.
```

### NEG-EVENT-3 — Old Set Wakes Post-Reset Waiter (if applicable)
```text
Broken: S1 set begins broadcast; reset -> UNSET; Wnew registers after reset;
stale S1 broadcast resolves Wnew.
Violated: SetEpochIsolation (E6).
```
If production serialization mechanically makes this topology impossible and the
correct model represents it directly, this negative model is added to confirm
the serialization is load-bearing. Runtime causal proof (T18) is mandatory
regardless.

---

## 11. Deterministic causal gates

| Test  | Forced interleaving                                  | Expected          |
| ----- | ---------------------------------------------------- | ----------------- |
| T0    | initially UNSET, wait suspends                       | needs waker       |
| T1    | initially SET, wait returns immediately              | Woken, no suspend |
| T2    | set() on SET                                         | idempotent no-op  |
| T3    | reset() clears readiness                             | UNSET             |
| T4    | late waiter after SET                                | Woken, no suspend |
| T5    | set before admission                                 | Woken, no suspend |
| T6    | set during admission                                 | Woken (one path)  |
| T7    | set after suspension                                 | Woken             |
| T8    | 3+ registered waiters, set                           | all Woken         |
| T9    | one expires during set broadcast                     | mixed outcomes    |
| T10   | one cancels during set broadcast                     | mixed outcomes    |
| T11   | RESOURCE_WAKE wins TIMER_EXPIRE                      | Woken             |
| T12   | TIMER_EXPIRE wins RESOURCE_WAKE                      | Expired           |
| T13   | RESOURCE_WAKE wins CANCEL                            | Woken             |
| T14   | CANCEL wins RESOURCE_WAKE                            | Cancelled         |
| T15   | TIMER_EXPIRE wins CANCEL                             | Expired           |
| T16   | three-way race                                       | one terminal      |
| T17   | reset does not cancel registered waiter              | waiter remains    |
| T18   | OLD_SET_WAKES_POST_RESET_WAITER                      | S1 cannot wake Wnew |
| T19   | external OS thread set wakes parked Live Scheduler   | Woken             |
| T20   | woken waiter resumes after E8 steal                  | Woken             |
| T21   | Drain unresolved Event wait                          | STALLED, no hang  |
| T22   | destruction after terminal waits                     | safe              |
| T23   | repeated multi-waiter mixed-outcome stress           | all consistent    |

---

## 12. Explicit deferrals

See §1.1. E12-A does NOT implement E12-B..F. Only E12-A closes.

---

## 13. As-built topology

Verified implementation details (CLOSED):

```text
Event state representation:
    std::atomic<bool> set_  (lock-free acquire/release)
    WaitQueue waiters_      (intrusive FIFO, private structural ops)

Scheduler ownership/binding:
    Event borrows Scheduler& for its lifetime; bound to one Scheduler.

set linearization point:
    set_.store(true, release) under global_mtx_ + atomic drain loop

reset linearization point:
    set_.store(false, release) under global_mtx_

wait admission seam:
    Scheduler::await_event_wait — register + check SET + (if SET) resolve Woken
    inline via WaitQueue::wake_node_locked, else commit suspension.
    All under global_mtx_ + q.mtx() (one atomic critical section).

set-all implementation mechanism:
    Scheduler::event_set_broadcast — loop wake_wait_one_locked until empty,
    atomic under global_mtx_ (store + drain in one critical section).

set/reset epoch isolation mechanism:
    global_mtx_ serialization — set()'s drain completes before reset() or a new
    admission can run. No generation counter needed.

deadline path:
    Scheduler::await_event_wait_deadline — composes admission with E11
    TimerRegistration. SET precedence at admission; else E11 I5 already-due path.

cancellation path:
    Scheduler::cancel_wait(event.wait_queue(), node) — inherited E10 seam.

destruction contract:
    ~Event asserts waiters_ empty in debug (~WaitQueue). No cancel/wake.

RunMode behavior:
    Live: parks on MW-S3 + external-wake-capable; external set() wakes via
          route_runnable_locked -> signal_wake_locked.
    Drain: MW-S3 returns STALLED (no hang); unresolved waiter left for caller.
```

## 14. Exit condition — VERIFIED

E12-A is CLOSED. All exit conditions verified:

- All deterministic tests T0–T23 pass (debug + release).
- TLA+ formal model passes: safety (E1,E2,E3,E5,E6) + liveness (E4) + both
  negative models (NEG-EVENT-1, NEG-EVENT-2) produce counterexamples violating
  their expected named properties. TLC v1.8.0 (build 2026.07.09).
- ASan+UBSan clean on Event tests (3 consecutive runs).
- TSan clean on Event tests (no data races, no lock-order-inversion, no UAF).
  The known raw fiber-asm DEADLYSIGNAL at high concurrent-worker counts is
  classified separately (T23 uses 3 workers to stay within TSan's fiber budget).
- Clang TSA clean (sluice_async target compiles with -Werror=thread-safety).
- E7–E11 regression suite passes (e7_worker, e8_steal, e9_external_wake,
  e10_wait_queue, e10_scheduler_wait, e10_corrective_c1/c2_c3, e11_timer_wait).
- Production bypass audit: RESOURCE_WAKE/TIMER_EXPIRE/CANCEL all converge on
  WaitNode::resolve_ — no parallel authority (see §8 call graphs).
