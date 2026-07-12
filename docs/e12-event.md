# E12-A — Async Event (Persistent Manual-Reset)

> Status: **CLOSED** — E12-A-EVENT-CORRECTIVE-2 + ASYNC-TEST-SEAM-AUTHORITY-
> CORRECTIVE-1. Both independent reviews PASS
> (ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1-REVIEW,
> E12-A-EVENT-CORRECTIVE-2-REVIEW). Fresh formal gate PASS (0-ed).
> (Built on E12-A-EVENT-CORRECTIVE-1 + ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1.)
>
> Authority baseline: E10 CLOSED
> ([`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)); E11 CLOSED at
> `7715808` ([`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md)).
> Preparation: [`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md)
> (E12-PREP READY).
>
> This document is the authoritative E12-A Event specification, refined by:
> - **ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1**: all forgeable namespace-level
>   test friends removed from installed headers; tests driven by an isolated
>   internal-testing runtime variant (`sluice_async_internal_testing`).
> - **E12-A-EVENT-CORRECTIVE-1**: sealed public authority, narrow cancellation,
>   multi-step formal set protocol, NEG-EVENT-3/4.
> - **E12-A-EVENT-CORRECTIVE-2**: exact `Event::cancel` queue-membership
>   validation (wrong-Event cancel → false without mutation; cross-Scheduler
>   safe), CANCEL-1..8 tests, T28/T29/T31/T32 evidence corrections, formal
>   Expired-not-reachable / TLC reporting accuracy.
>
> Formal model: [`docs/spec/e12_event/`](spec/e12_event/). Formal gate:
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

    // Narrow per-wait-epoch CANCELLATION authority (E12-A-EVENT-CORRECTIVE-1 A2).
    // Resolves `node` with Cancelled through the Scheduler cancellation path on
    // THIS Event's private WaitQueue, WITHOUT exposing that queue. Returns true
    // iff this call won.
    [[nodiscard]] bool cancel(WaitNode& node);
};

}  // namespace sluice::async
```

The raw `WaitQueue& wait_queue()` accessor was REMOVED in
E12-A-EVENT-CORRECTIVE-1 (F-EVENT-AUTH): it leaked the Event's underlying
WaitQueue to ordinary production code, which could combine it with
`Scheduler::wake_wait_one` to resolve an Event waiter as `Woken` while the Event
remained UNSET and `set()` was never called. A compile-negative probe
(`tests/e12_event_authority_probe.cpp`, gated by the formal verify script)
mechanically verifies the bypass no longer compiles. The Event's private
WaitQueue is NOT reachable from ordinary production code: ASYNC-TEST-SEAM-
AUTHORITY-CORRECTIVE-1 removed the forgeable `E12EventTestHooks` friend from the
installed headers, so no production TU can name or define a type that grants
access to it. Deterministic test phase seams are driven only by the non-installed
internal-testing runtime variant (`sluice_async_internal_testing`).

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
- `cancel` / `cancel_wait` -> `Cancelled` (CANCEL)
- deadline elapsing -> `Expired` (TIMER_EXPIRE)

If the deadline is already due at admission, the E11 I5 path resolves `Expired`
inline (no suspend). If `set_` is observed at admission, the wait resolves
`Woken` inline (no suspend). The deadline registration is retired by a non-timer
winner in the same critical section (E11 I4 lifetime closure).

**Deadline precedence (F-EVENT-DEADLINE, normative):** during Event deadline
admission, Event SET readiness is checked BEFORE the already-due deadline
predicate. Therefore Event SET + already-due deadline -> `Woken` (the resource
is ready; the deadline is moot). The complete matrix:

| Event at admission | Deadline at admission | Result                     |
| ------------------ | --------------------- | -------------------------- |
| SET                | future                | Woken                      |
| SET                | already due           | Woken (SET precedence)     |
| UNSET              | already due           | Expired                    |
| UNSET              | future                | RESOURCE/TIMER/CANCEL race |

The combined SET + already-due case is asserted directly by T33.

### 3.8 Cancellation authority (E12-A-EVENT-CORRECTIVE-2)

`CANCEL` is an inherited WaitNode resolution cause. E12-A does NOT invent a new
public task-level cancellation token. `Event::wait` internally uses one ordinary
E10/E11 WaitNode epoch. The narrow per-wait-epoch cancellation surface is:

```cpp
[[nodiscard]] bool Event::cancel(WaitNode& node);
```

It routes through `Scheduler::event_cancel_wait`, which validates EXACT
queue-membership before cancelling. The final contract (Corrective C — returns
true ONLY if ALL hold):

```text
node is currently Registered, AND
node is currently linked in THIS Event's private WaitQueue, AND
CANCEL wins node.resolve_(Cancelled).
Otherwise returns false WITHOUT mutation.
```

The membership check (`WaitQueue::contains_locked`, a private Scheduler-friend
method) scans THIS Event's own intrusive list for `&node` while holding this
Scheduler's `global_mtx_` + this Event's `q.mtx()`. It does NOT read a foreign
node's `home_` and does NOT lock a foreign Event/Scheduler, so cross-Scheduler
wrong-Event cancel is synchronized and structurally safe. This includes:

```text
detached node                 -> false
already Woken                 -> false
already Cancelled             -> false
already Expired               -> false
live node in another Event    -> false (same OR different Scheduler)
```

Wrong-Event cancellation is a safe false-return case (NOT UB, NOT a debug
assertion, NOT a caller-contract violation). A foreign node is NOT resolved and
NOT unlinked; both Event queues remain structurally intact and the original
Event can later wake the node normally. This is NOT a task cancellation token,
NOT cancel-all, NOT an Event close, NOT destructor cancellation. Generic
`Scheduler::cancel_wait` is unchanged (its caller contract already guarantees
membership; the Event-specific gate is the path reached from untrusted
`Event::cancel` callers).

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

### 7.3 Causal traces (E12-A-EVENT-CORRECTIVE-1 Correctives C/D)

The set/reset epoch isolation is proven by mechanically-gated causal tests
(NOT timing). The `EVENT_SET_AFTER_SET_STORE_BEFORE_DRAIN` and
`EVENT_ADMISSION_AFTER_REGISTER_BEFORE_FINAL_SET_CHECK` test phase seams pause
a thread at the load-bearing boundary while the production lock
(`global_mtx_`, and for admission `q.mtx()`) is STILL HELD.

- **T27 (set-drain blocks reset):** S1 stores SET then pauses mid-drain holding
  `global_mtx_`; a reset contender records `reset_attempted` but
  `reset_completed` stays false until S1 releases. Lock held: `global_mtx_`.
- **T28 (set-drain blocks admission):** a new-admission contender (modeled via
  `cancel`, which also needs `global_mtx_`) records `admission_attempted` but
  `admission_completed` stays false while S1's drain is active.
- **T29 (post-reset Wnew waits for S2):** Wold registered → S1 drains Wold →
  reset → Wnew admitted under UNSET → S2 wakes Wnew. Wold woken by S1; Wnew woken
  by S2 (NOT by S1's drain).
- **T30 (admission-first ordering):** W's admission pauses holding
  `global_mtx_`+`q.mtx()` before the final SET check; a setter cannot complete
  until W releases. W commits waiting under UNSET, releases; the setter then
  drains W.
- **T31 (set-first ordering):** the setter stores SET and pauses mid-drain; an
  admission cannot complete until the setter releases; admission then observes
  SET and returns Woken inline (no suspend), proving Wnew was NOT registered
  during the drain.

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

S1 cannot RESOURCE_WAKE Wnew (Wnew is not registered during S1's drain). T27/T28
prove the active-drain serialization mechanically; T29 proves the full topology;
together they prove `OLD_SET_WAKES_POST_RESET_WAITER -> impossible`.

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

### E5 — Reset Non-Resolution (E12-A-EVENT-CORRECTIVE-1 Corrective J)
```text
Reset alone does not change a WaitNode from Registered to terminal
```
`reset()` only flips `set_`; it does not touch any WaitNode. The formal property
`InvResetNonResolution` checks that a terminal node's `resolutionCause` (written
by every terminal-resolution action) is never "Reset" — real checking power, not
a constant. NEG-EVENT-4 confirms a buggy reset that resolves a waiter is caught.

### E6 — Set-Epoch Isolation (E12-A-EVENT-CORRECTIVE-1 Corrective H3)
```text
a set-epoch DRAIN may RESOURCE_WAKE only a wait epoch that was already
registered when that set epoch's drain linearized
```
Production uses `global_mtx_` serialization (no generation counter). The formal
model represents the set epoch as a MULTI-STEP serialized critical section
(`StartSet` → `DrainOne`* → `FinishSet`); `ResetEvent` and `Register` require
the protocol to be `Idle`. The property
`InvSetEpochIsolation` checks `registrationGeneration[n] <= wakeEpochGen[n]`:
a waiter admitted after a later reset cannot be woken by an older set epoch's
drain. This replaces the vacuous `wokenBySetEpoch[n] <= 1`. NEG-EVENT-3 confirms
the stale-set/post-reset defect is caught.

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

### NEG-EVENT-3 — Old Set Wakes Post-Reset Waiter (E12-A-EVENT-CORRECTIVE-1)
```text
Broken: the global serialization between old set drain, reset, and admission is
LOST (RegisterBuggy/ResetBuggy drop the protoPhase=Idle guard).
S1 StartSet (epoch gen G0); ResetBuggy (gen G1, while drain active); Wnew
RegisterBuggy (registrationGeneration=G1); stale DrainOne(Wnew) wakes Wnew by
epoch G0 (G0 < G1).
Counterexample: Wnew registrationGeneration=G1, wakeEpochGen=G0.
Violated: SetEpochIsolation (E6).
```
The single broken rule is the lost `protoPhase` guard on Reset + Register. The
negative model does NOT initialize a stale wake directly, does NOT disable all
progress, does NOT reuse a terminal node, does NOT change WaitNode single-
resolution semantics, and does NOT remove cancel generally.

### NEG-EVENT-4 — Reset Resolves Waiter (E12-A-EVENT-CORRECTIVE-1 Corrective J)
```text
Broken: ResetBuggy changes one Registered waiter to terminal (Woken) and records
the "Reset" resolution cause.
Counterexample: a terminal node with resolutionCause="Reset".
Violated: ResetNonResolution (E5).
```
The `resolutionCause` variable is written by every modeled terminal-resolution
action; the bug writes the forbidden "Reset" value, which the property catches.

---

## 11. Deterministic causal gates

| Test  | Forced interleaving                                  | Expected          |
| ----- | ---------------------------------------------------- | ----------------- |
| T0    | initially UNSET, wait suspends                       | needs waker       |
| T1    | initially SET, wait returns immediately              | Woken, no suspend |
| T2    | set() on SET                                         | idempotent no-op  |
| T3    | reset() clears readiness                             | UNSET             |
| T4    | late waiter after SET                                 | Woken, no suspend |
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
| T16   | three-way race (STRESS, not causal winner proof)     | one terminal      |
| T17   | reset does not cancel registered waiter              | waiter remains    |
| T18   | OLD_SET_WAKES_POST_RESET_WAITER (sequential)         | S1 cannot wake Wnew |
| T19   | external OS thread set wakes Live Scheduler (park-independent) | Woken  |
| T20   | woken waiter resumes after E8 ownership transfer     | Woken (outcome Worker-independent; steal correctness inherited from E8) |
| T21   | Drain unresolved Event wait                          | STALLED, no hang  |
| T22   | destruction after terminal waits                     | safe              |
| T23   | repeated multi-waiter mixed-outcome STRESS           | all consistent    |
| T24   | compile-negative: raw WaitQueue bypass sealed        | fails to compile  |
| T25   | Event::cancel correct Event/node wins Cancelled once | Cancelled         |
| T26   | Event::cancel detached/terminal/empty loses safely   | false             |
| T27   | causal set-drain BLOCKS reset (seam, global_mtx_)    | reset blocked     |
| T28   | set-drain BLOCKS another Scheduler-global op (global-mutex proxy, NOT admission proof) | contender blocked |
| T29   | sequential post-reset epoch sanity (NON-CAUSAL)       | Wnew woken by S2  |
| T30   | causal admission-FIRST ordering (seam)               | setter blocked    |
| T31   | causal set-FIRST ordering (two-phase admission marker) | admission blocked |
| T32   | truly parked Live Worker awakened by external set (joined jthread watchdog) | Woken (park seam) |
| T33   | SET + already-due deadline -> Woken (precedence)     | Woken             |
| CANCEL-1 | correct Event + live Registered node -> true/Cancelled once | Cancelled |
| CANCEL-2a | wrong Event, SAME Scheduler -> false, node intact on A | false; A wakes later |
| CANCEL-2b | wrong Event, DIFFERENT Scheduler -> false, node intact | false; A wakes later |
| CANCEL-3 | detached node -> false                             | false             |
| CANCEL-4 | Woken node -> false                                | false             |
| CANCEL-5 | Expired node -> false                              | false             |
| CANCEL-6 | already-Cancelled node -> false                    | false             |
| CANCEL-7 | RESOURCE_WAKE wins before cancel -> false, Woken final | false          |
| CANCEL-8 | CANCEL wins before set -> Cancelled; later set leaves it | Cancelled     |

T16 and T23 are STRESS tests (supplementary), NOT causal winner proofs — the
causal two/three-way winner proofs are T11–T15. T20 proves the Event outcome and
wait identity are Worker-independent; actual steal correctness is inherited from
the closed E8 proof (T20 does NOT independently force a steal — it runs with two
Workers and a steal MAY occur). T19 is park-independent (its bounded sleeps are
registration-sync only, NOT the park-entry proof); T32 is the mechanical
park-entry proof via the E9 park-commit seam.

E12-A-EVENT-CORRECTIVE-2 evidence corrections:
- **T28** is re-documented as a global-mutex proxy (set-drain blocks another
  Scheduler-global operation). It does NOT prove Event admission (an admission
  fiber cannot be driven from an external thread). Actual admission blocking is
  proven by T31.
- **T29** is classified as a SEQUENTIAL post-reset epoch sanity test (non-causal;
  does not arm the set-drain seam). The causal active-drain serialization is
  proven by T27 and T31.
- **T31** uses a TWO-PHASE admission marker (admission_attempt_before_global_lock
  + admission_before_final_check) instead of yield()-based blocked inference.
  The pre-lock phase is armed+blocking so the setter acquires global_mtx_ first;
  the post-lock marker proves admission could NOT enter its CS while the drain
  was active.
- **T32** uses a JOINED `std::jthread` watchdog (NOT a detached thread) as a
  bounded failure guard. On timeout it releases any armed gate + drives set() so
  the run terminates and all threads join before the test returns (no UAF). The
  park phase itself is observed mechanically via the E9 park-commit seam.
- **CANCEL-1..8** prove the exact `Event::cancel` queue-membership contract,
  including wrong-Event cancellation across the SAME and DIFFERENT Schedulers
  (the structural-safety fix: the membership scan never reads a foreign node's
  `home_` and never locks a foreign Event/Scheduler).

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
    Event::cancel(node) -> Scheduler::event_cancel_wait — validates EXACT
    queue-membership (contains_locked) under global_mtx_+q.mtx() before
    cancelling; wrong-Event/detached/terminal nodes return false WITHOUT
    mutation (cross-Scheduler safe). The raw wait_queue() accessor is REMOVED
    (F-EVENT-AUTH); no forgeable test friend remains.

destruction contract:
    ~Event asserts waiters_ empty in debug (~WaitQueue). No cancel/wake.

RunMode behavior:
    Live: parks on MW-S3 + external-wake-capable; external set() wakes via
          route_runnable_locked -> signal_wake_locked.
    Drain: MW-S3 returns STALLED (no hang); unresolved waiter left for caller.
```

## 14. Exit condition — CORRECTED (E12-A-EVENT-CORRECTIVE-2)

E12-A is **CLOSED**. Both independent reviews
(ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1-REVIEW and
E12-A-EVENT-CORRECTIVE-2-REVIEW) returned PASS, and the fresh formal gate
returned 0-ed (all expected verdicts + named properties + gates). The roadmap
may now say:

```
E12-A EVENT CLOSED — E12-B SEMAPHORE MAY BEGIN
```

The layered correctives that closed E12-A:
- ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1 (test friends isolated).
- E12-A-EVENT-CORRECTIVE-1 (sealed authority, narrow cancel, formal protocol).
- E12-A-EVENT-CORRECTIVE-2 (cancel queue-membership validation, CANCEL-1..8,
  T28/T29/T31/T32 evidence corrections, formal Expired/TLC accuracy).

Verification:

- All deterministic tests T0–T33 + CANCEL-1..8 pass (debug + release).
- TLA+ formal model passes: safety (E1,E2,E3,E5,E6) + liveness (E4) + all four
  negative models (NEG-EVENT-1..4) produce counterexamples violating their
  EXPECTED NAMED properties; WRONG-PROPERTY gate OK; COMPILE-PROBE gate OK.
- TLC runtime version (exact): `TLC2 Version 2026.07.09.134028 (rev: 227f61b)`.
  TLA+ tools release tag: not associated with a verified release tag (the jar is
  a 2026 development build; no v1.8.0 association asserted without jar-metadata
  proof). Counterexample depth: `not reported by this invocation` where TLC
  provides none.
- Formal model accuracy: Expired is in the shared state domain but NOT produced
  by any E12 TIMER_EXPIRE action (deadline correctness inherited from E11).
- Clang TSA clean (both `sluice_async` and `sluice_async_internal_testing`
  compile with -Werror=thread-safety).
- E7–E11 regression suite passes (e7_worker, e7_coord, e8_steal, e9_external_wake,
  e10_wait_queue, e10_scheduler_wait, e10_corrective_c1/c2_c3, e11_timer_wait).
- **`e10_corrective_c5_test` is a PROVEN BASELINE DEFECT**: it fails to compile
  (`error: unused variable 'order_bad' [-Werror,-Wunused-variable]`) on the
  baseline — the defect is OUTSIDE this corrective diff and is NOT silently
  "fixed" here. The suite is NOT "all green" while this baseline compile defect
  remains.
- Production authority audit (ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1): installed
  headers declare NO test friend; production `sluice_async` exports NO test
  phase / seam / controller symbol; the variant `sluice_async_internal_testing`
  is the only artifact that contains them. No binary links both variants.
- Cancellation ownership audit (E12-A-EVENT-CORRECTIVE-2): `Event::cancel`
  validates exact queue-membership via `contains_locked` under
  global_mtx_+q.mtx(); wrong-Event (same OR different Scheduler), detached, and
  terminal nodes return false WITHOUT mutation (never reads foreign `home_`,
  never locks foreign Event/Scheduler). RESOURCE_WAKE (Event::set / admission
  observing SET), TIMER_EXPIRE (E11 deadline service), CANCEL (Event::cancel)
  all converge on `WaitNode::resolve_` — no parallel authority.
