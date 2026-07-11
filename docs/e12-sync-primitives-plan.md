# E12-PREP — Async Synchronization Primitive Frontier

> Read-only architecture / API audit and documentation preparation for
> **E12 — Async Synchronization Primitives**, performed before any E12
> production, runtime-test, or formal-model implementation begins.
>
> **Status:** PREPARATION COMPLETE. Verdict: `E12-PREP: READY`.
>
> Authority baseline: E10 is CLOSED (as-built
> [`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)). E11 is
> CLOSED at `7715808` (spec
> [`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md); formal model
> [`docs/spec/e11_timer_wait/`](spec/e11_timer_wait/)). This document does NOT
> reopen E10 or E11; it builds on them as authoritative.
>
> Bound by the project construction method
> [`docs/async-runtime-construction-method.md`](async-runtime-construction-method.md)
> (M1–M9).

This document is **normative preparation only**. No E12 primitive code, no
runtime tests, and no formal models are produced here. Each primitive's
semantic authority is resolved to one of:

```text
RESOLVED                 — production/protocol evidence fixes the policy
HUMAN DECISION REQUIRED   — materially different valid choices exist; deferred
DEFERRED                  — out of first E12 scope by explicit decision
```

---

## 1. Frontier drift found and resolved

### 1.1 The inconsistency

The roadmap ([`docs/async-runtime-plan.md`](async-runtime-plan.md)) contains
two E12 orderings that disagree:

- **Decomposition list** (§"Planned decomposition"):

  ```text
  E12-A Mutex
  E12-B Event
  E12-C Condition
  E12-D Queue
  E12-E Semaphore
  E12-F RwLock
  E12-G Cross-Primitive Cancellation Audit
  ```

- **Dependency trunk** (§"Summary" → SYNCHRONIZATION):

  ```text
  Event
  Semaphore
  Mutex
  Condition
  Queue
  RwLock
  ```

These disagree on **every** position except the last (RwLock). The roadmap
itself says *"The exact order may be adjusted after API audit."* This audit
is that adjustment.

### 1.2 Resolution

The canonical E12 order is **dependency-trunk order**, not decomposition-list
order. Evidence is in §3 (dependency graph). The decomposition-list order was
not justified by any protocol dependency; it is superseded.

Because **no E12 implementation has begun** (no E12-A..G code, tests, or formal
models exist — verified: the only `mutex.hpp` in tree is the CPP-STATIC-1
TSA-annotated `std::mutex` wrapper at `include/sluice/async/mutex.hpp`, which
is a *synchronous* thread-blocking lock and is explicitly NOT an async mutex;
see §4.4), the `E12-A..F` identifiers are **renumbered** to match the
dependency trunk. Historical closed IDs (E10-*, E11-*) are NOT rewritten.

### 1.3 Canonical order

```text
E12-A Event
E12-B Semaphore
E12-C Mutex
E12-D Condition
E12-E Queue
E12-F RwLock
E12-G Cross-Primitive Cancellation / Deadline Audit
```

The roadmap document is updated in the same commit that adds this file so the
two active authority documents agree (see §10).

---

## 2. Existing persistent-readiness reconstruction (Task A)

This is the decisive finding for Event: **the runtime already contains the
persistence half of a manual-reset Event** (a level-triggered, persistent,
non-consumed ready-flag), but **NOT the multi-waiter broadcast half** (the
ready-flag map is single-registrant-per-flag). Both halves must be understood
before Event is designed, and neither must be duplicated.

### 2.1 Sources audited

```text
include/sluice/async/future.hpp             Future<T>::ready_  (std::atomic<bool>)
include/sluice/async/completion.hpp         Completion<T>::ready()  (state machine)
include/sluice/async/evented_wait_policy.hpp  EventedWaitPolicy -> await_ready_flag
include/sluice/async/group.hpp              Evented Group (Fiber-spawning)
include/sluice/async/wait_node.hpp          WaitNode (E10)
include/sluice/async/wait_queue.hpp         WaitQueue (E10)
src/async/scheduler.cpp                     await_ready_flag, wake_ready_flags_locked,
                                            wake_ready_completions_locked, await_wait,
                                            await_wait_deadline, wake_wait_one,
                                            cancel_wait, expire_wait
```

### 2.2 Answers (1–8)

1. **What state is persistent?**
   - `Future<T>::ready_` (`std::atomic<bool>`) — caller-owned, set once by
     `complete_with`, never cleared by the runtime. *Persistent.*
   - `Completion<T>::state_ == ready` — persistent until an explicit
     `reset()` returns it to `idle`. *Persistent with an explicit reset.*
   - `WaitNode::state_` — NOT persistent readiness; it is wait-*resolution*
     state (registered → terminal). A terminal node is *absorbing* (one-shot).
   - There is no standalone "event flag" object today.

2. **Level-triggered or edge-triggered?** **Level-triggered.** The Scheduler's
   wake path is `wake_ready_flags_locked`
   (`src/async/scheduler.cpp:878`): it scans `waiting_ready_` and resumes any
   fiber whose flag `load(acquire)` is true. A flag that is true when scanned
   causes a wake; the wake is not consumed by the *act* of waking, only by the
   flag later becoming false. `await_ready_flag` (`scheduler.cpp:1002`) checks
   the flag *before* registering and again *under the lock* before suspending —
   a level recheck, not an edge latch.

3. **Can a late waiter observe already-published readiness?** **Yes.**
   `await_ready_flag` returns immediately if `ready.load(acquire)` is true on
   entry (`scheduler.cpp:1005`), and the under-lock recheck
   (`scheduler.cpp:1013`) undoes a speculative registration and returns without
   suspending if the flag became true between the first check and registration.
   This is exactly the "SET remains ready for late waiters" property of a
   manual-reset Event.

4. **One-shot or repeatable?**
   - `Future<T>::ready_`: **one-shot terminal** (no reset API; once set it
     stays set for the Future's lifetime).
   - `Completion<T>::ready`: **repeatable via `reset()`** (returns to `idle`).
   - The ready-flag wait substrate (`await_ready_flag` / `wake_ready_flags_*`):
     the *flag itself* is repeatable — the same `atomic<bool>` can be set, then
     cleared by the producer, then set again, and waiters re-arm.

5. **Is reset represented?** In the flag substrate: **yes, by the producer
   clearing the `atomic<bool>`**. There is no reset *protocol*; it is just a
   store. `Completion::reset()` is the only formalized reset.

6. **Does one publication wake one / all / a fixed number?**
   - **ready-flag wake is single-registrant-per-flag.** This is a critical
     correction: `waiting_ready_` is declared
     `std::unordered_map<const std::atomic<bool>*, WaitReg>`
     (`scheduler.hpp:459`) where `WaitReg` is a single `{Fiber*,
     WorkerState*}` (`scheduler.hpp:368`) — NOT a list of waiters per flag.
     `await_ready_flag` does `waiting_ready_[&ready] = {me, ws}`
     (`scheduler.cpp:1012`), which **overwrites** any prior registrant on the
     same flag address. So the substrate supports at most ONE Fiber waiting on
     a given flag object at a time; `wake_ready_flags_locked` scans the map
     and resumes the single fiber (if any) whose flag is true. There is no
     multi-waiter broadcast on a flag today.
   - **WaitQueue wake is single-waiter (FIFO head).** `wake_wait_one` /
     `wake_one_locked` resolves exactly the FIFO head (one winner). Note a
     WaitQueue CAN hold many waiters (an intrusive list); it is the ready-flag
     map that is single-registrant-per-key, not the WaitQueue.
   - `Future::complete_with` calls `cv_.notify_all()` on the Threaded path;
     the Evented path routes through the single-registrant flag scan above
     (one Fiber per Future's flag).

7. **Does current Future readiness have consumptive semantics?** **No.**
   `Future::ready_` is monotonic; `await()` is idempotent ("returns the cached
   result without waiting" on a second call). Readiness is *observed*, not
   *consumed*. `Completion` is closer to consumable (`reset()` is required to
   reuse it), but `result()` does not consume readiness either.

8. **Which existing mechanism must NOT be duplicated by Async Event?** The
   **`Future<T>::ready_` + `await_ready_flag` + `wake_ready_flags_locked`
   level-triggered flag-wait substrate**. An Async `Event` must NOT re-add a
   second ready-flag wait map. But — importantly — the existing substrate is
   **single-registrant-per-flag** (Q6), so it does NOT already provide
   multi-waiter broadcast. Event's multi-waiter wake must instead be built on
   the **E10/E11 WaitQueue + WaitNode** substrate (which holds many waiters in
   an intrusive FIFO list), with Event's OWN persistent boolean `set_`
   providing the level/persistence property. Event thus composes the
   persistent-bool idea from the flag substrate with the multi-waiter capacity
   of WaitQueue — it does not duplicate either wholesale. See §4.1, §4.4.

### 2.3 Semantic overlap table

| Existing mechanism | Persistent state | Wake cardinality | Consumption | Reset | E12 relevance |
| ------------------ | ---------------- | ---------------- | ----------- | ----- | ------------- |
| `Future<T>::ready_` (`atomic<bool>`) | persistent (monotonic) | single-registrant on its flag (Evented path); `notify_all` only on the Threaded cv path | none (idempotent `await`) | none (lifetime-terminal) | **Med-high — provides the persistent-bool idea, but NOT multi-waiter broadcast on the Evented path.** Event must add the multi-waiter capacity the Future lacks. |
| `Completion<T>::ready` (state machine) | persistent until `reset()` | one (the single awaiter registered by address) | none (`result()` non-consuming) | explicit `reset()` | Low. Completion is an I/O op handle, not a broadcast signal. |
| `await_ready_flag` / `wake_ready_flags_locked` | persistent (flag is external `atomic<bool>`) | **single-registrant per flag** (`WaitReg` value, not a list; `waiting_ready_[&ready] = {me, ws}` overwrites) | none (level-triggered) | producer clears the flag | **Med — provides level-triggered persistence, but NOT multi-waiter wake.** Event must NOT add a second flag-wait map; it must use a WaitQueue for multi-waiter capacity. |
| `WaitQueue` `wake_wait_one` (E10) | none (no readiness state; wake is an action) | **one** per call (FIFO head winner), but the queue holds many waiters | n/a (action, not state) | n/a | **High — Event's wake path is a WaitQueue + `wake_wait_one` (for wake-one) or repeated for wake-all.** |
| `TimerRegistration` (E11) | deadline value | one (bound node) | n/a | retire/consume | Low directly; provides the deadline resolver Event will compose. |
| `EventedWaitPolicy` | delegates to `await_ready_flag` | single-registrant (via the flag substrate) | none | n/a | Med — only relevant to *Future* await; Event is a new primitive, not a Future policy. |
| Evented `Group` | per-task `Future<void>::ready_` | per-task single-registrant | none | none | Low. Group is a cancel-propagation boundary, not a signal. |

**Conclusion of Task A (corrected):** the runtime already contains a
**level-triggered, persistent, non-consumed readiness** substrate (the
ready-flag wait path) — the *persistence* half of a manual-reset Event is
already present. It does **NOT** contain multi-waiter broadcast: the
ready-flag map is single-registrant-per-flag. A first-scope Async **Event** is
therefore best modeled as a **manual-reset Event**: Event's OWN persistent
boolean `set_` (the level/persistence property, borrowed from the flag
substrate's *idea* but not its single-registrant *mechanism*) + an **E10
WaitQueue** (which holds many waiters) for the blocking path. `set()` makes
late-waiters observe readiness immediately; while SET, `wait()` does not
park. `reset()` clears the boolean. This composes the two existing mechanisms
without duplicating either, and without re-adding a flag-wait map. The Event
wake-cardinality decision (§4.4) is a *genuinely new* decision because the
existing substrate never had to make it. See §4.1, §4.4.

---

## 3. Canonical dependency order (Task B)

### 3.1 Per-primitive protocol deltas

| Primitive | New state dimension | New resource accounting | New ownership law | Depends on | New cancel/deadline cleanup obligation | State-space complexity |
| --------- | ------------------- | ----------------------- | ----------------- | ---------- | -------------------------------------- | ---------------------- |
| **Event** | one persistent bool `set_` | none | none | WaitNode (E10) + deadline (E11) | unlink registered node on cancel/expire; no permit/ownership to reclaim | low (2 states × queue) |
| **Semaphore** | permit count | `permits` + `granted` + `queued demand` conservation | none | Event's persistent-bool pattern (optional); WaitNode + deadline | **grant vs cancel/expire race: who owns a granted permit** | medium (counter × queue) |
| **Mutex** | locked bool + owner identity | none (one lock) | **Fiber-identity ownership + migration law** | Semaphore (permits=1 is a near-subset; but Mutex adds ownership) | **handed-off ownership loss on cancel/expire after grant** | medium (locked × owner × queue) |
| **Condition** | none new (delegates to Mutex) | none | none (Mutex owns it) | **Mutex** (mandatory) | release/register atomic window; reacquire on wake/timeout/cancel | medium-high (two queues + mutex reacquire) |
| **Queue** | buffer + closed bool | **item slots** (bounded) or unbounded list; not-empty + not-full waiters | none | Event (not-empty/not-full are Event-like); WaitNode + deadline | **reserved item / reserved slot under cancel/close** | high (buffer × 2 queues × closed) |
| **RwLock** | reader count + writer bool + (upgrade state) | reader count accounting | writer owner identity | Semaphore (read permits) + Mutex (write) | reader/writer starvation; upgrade downgrade cancellation | **highest** |

### 3.2 Dependency graph

```text
                         E10 WaitNode + WaitQueue (CLOSED)
                                     │
                                     ▼
                         E11 deadline / timer (CLOSED)
                                     │
          ┌──────────────────────────┼──────────────────────────┐
          ▼                          ▼                          ▼
   E12-A Event           E12-B Semaphore                 (Mutex needs BOTH
   persistent bool       permit accounting                the queue substrate
   + WaitQueue           conservation invariant            and ownership law)
          │                          │
          │                          ▼
          │                   E12-C Mutex
          │                   owner identity
          │                   + handoff/barging
          │                          │
          │                          ├──────────┐
          │                          ▼          ▼
          │              E12-D Condition   (Mutex is also a
          │              release+register   direct dependency
          │              + reacquire       for any primitive
          │                          │      that needs exclusive
          │                          │      ownership semantics)
          │                          │
          ▼                          ▼
   E12-E Queue ──────uses Mutex (internally for buffer mutex)
   bounded buffer + not-empty / not-full (Event-like) + close
          │
          ▼
   E12-F RwLock ──────uses Semaphore-pattern (read permits) + Mutex-pattern (write)
   reader count + writer + (upgrade/downgrade)
          │
          ▼
   E12-G Cross-Primitive Cancellation / Deadline Audit
   (uses the Task-I matrix as baseline)
```

### 3.3 Edge-by-edge evaluation (protocol dependency, not API popularity)

- **Event before Semaphore.** Event introduces exactly one new state
  dimension — a persistent boolean — and reuses the closed wait substrate.
  Semaphore introduces *accounting* (a conservation invariant over
  permits/granted/queued). The simpler, state-lighter primitive comes first
  and validates the Event→WaitQueue integration. **Event → Semaphore.**
- **Semaphore before Mutex.** A Mutex is, at the accounting layer, a
  Semaphore with permit count 1 PLUS an ownership identity law and a handoff
  discipline. Building Semaphore first establishes the permit-conservation
  invariant and the grant-vs-cancel/expire race resolution that Mutex then
  specializes with ownership. (Mutex is NOT merely "Semaphore with count 1"
  — but Semaphore's accounting is a strict prerequisite pattern.)
  **Semaphore → Mutex.**
- **Mutex before Condition.** Condition is *defined* in terms of Mutex:
  `wait` = release-mutex + register-condition-wait + suspend + reacquire-mutex.
  There is no Condition without a Mutex to release and reacquire. This is a
  hard protocol dependency, not a preference. **Mutex → Condition.**
- **Condition before Queue.** Queue does not strictly depend on Condition
  (a Queue's not-empty/not-full waiters are Event-like single-winner waits,
  not broadcast condition waits). However Condition comes before Queue in the
  trunk because Queue is the first primitive with *two* wait queues
  (producer/consumer) and a `close` lifecycle, which is a state-complexity
  step beyond Condition's two-queue-but-no-buffer shape. Ordering
  Condition before Queue keeps state-complexity monotonic increasing.
  **Condition → Queue.** (This edge is the weakest; it is justified by
  complexity monotonicity, and the trunk already records it.)
- **Queue before RwLock.** RwLock combines a reader-count (Semaphore-pattern)
  with a writer-exclusive (Mutex-pattern) and optional upgrade/downgrade. It
  has strictly the highest state-space complexity. It is last among the
  data-bearing primitives. **Queue → RwLock.**
- **All primitives before E12-G.** The cross-primitive cancellation/deadline
  audit (E12-G) is explicitly a *retrospective* audit across all E12-A..F. It
  uses the Task-I matrix (§10) as its baseline. It must be last.

### 3.4 Renumbering note

Because no E12-A..F implementation exists (§1.2), renumbering the active
identifiers to match the dependency trunk rewrites **no completed historical
work** — only the *planned* decomposition labels in the active roadmap. The
closed E10-*/E11-* IDs are untouched. This is the minimum frontier change
that makes one authority consistent.

---

## 4. Event semantic authority (Task C)

### 4.1 Chosen first-scope semantics: **persistent manual-reset Event**

```text
UNSET ──set()──> SET
 SET ──reset()──> UNSET
```

- `SET` is **level-triggered persistent readiness**: a late waiter that calls
  `wait()` while `SET` observes readiness immediately and does NOT suspend
  (mirrors `await_ready_flag`'s under-lock recheck, §2.2 Q3).
- While `SET`, `wait()` drains without parking (or parks-and-immediately-wakes
  through `wake_wait_one` of the head, preserving the single-winner seam).
- `reset()` returns to `UNSET`; subsequent `wait()` registers a WaitNode and
  suspends.

### 4.2 Evaluation against alternatives

| Alternative | Verdict | Reason |
| ----------- | ------- | ------ |
| **manual-reset Event** (chosen) | **SUPPORTED BY EXISTING RUNTIME SEMANTICS** | Directly matches the level-triggered, persistent, late-waiter-observable ready-flag substrate (§2). Minimal new state (one bool + one WaitQueue). |
| auto-reset Event | VALID BUT NEW POLICY | Requires consuming the wake (wake-one then auto-clear). The existing flag substrate is non-consumptive; auto-reset would be a new semantic. Defer. |
| single-consumer notification | CONTRADICTS ROADMAP INTENT | Roadmap frames Event as the simplest *persistent readiness* primitive (broadcaster-ish), and as a Mutex/Condition substrate. Single-consumer is too narrow. |
| pulse / edge-triggered Event | VALID BUT NEW POLICY | Edge semantics (wake only waiters registered at pulse time, no persistence) contradict the level-triggered substrate and would lose the "late waiter observes SET" property. Defer. |
| one-shot latch | SUPPORTED AS A SUBSET | A manual-reset Event that is never reset IS a one-shot latch. The chosen semantics subsume it; no separate primitive needed. |

### 4.3 Required decisions

| Decision | Resolution | Basis |
| -------- | ---------- | ----- |
| `set()` idempotence | **RESOLVED: idempotent.** `set()` on an already-SET Event is a no-op (no extra wake). | Matches `Future::complete_with` exactly-once; level-triggered substrate. |
| `reset()` semantics | **RESOLVED: clear the bool.** Does NOT wake anyone; future `wait()` blocks. | Manual-reset definition. |
| `wait()` on already-set | **RESOLVED: returns without suspending.** | §2.2 Q3; under-lock recheck idiom. |
| `wait()` + deadline | **RESOLVED: compose E11 `await_wait_deadline`.** Expired → `WaitOutcome::expired`. | E11 closed substrate. |
| `wait()` + cancellation | **RESOLVED: `cancel_wait` → `WaitOutcome::cancelled`.** | E10 closed substrate. |
| `set()` vs registration race | **RESOLVED: under Event mutex, `set()` flips the bool and, if any waiters, wakes one (or all — see cardinality) via `wake_wait_one`; a concurrent registrar observes SET and does not suspend.** | The register+recheck+make_waiting atomic-window idiom (E10 §4). |
| `reset()` vs already-registered waiter | **RESOLVED: `reset()` only clears the bool; it does not cancel a registered waiter.** A waiter already in the queue remains until woken, cancelled, or expired. This is the manual-reset contract. | Avoids a primitive-local cancellation winner (Hard Rule 3). |
| wake cardinality | **HUMAN DECISION REQUIRED — see §4.4.** |
| destruction with waiters | **HUMAN DECISION REQUIRED — see §4.4.** |

### 4.4 Event — HUMAN DECISION REQUIRED (wake cardinality + destruction)

Two materially different valid choices production evidence does NOT fix:

1. **Wake cardinality of `set()` while waiters are registered.**
   - Option E1: **wake-one** (FIFO head) via `wake_wait_one`, leaving others
     registered; a subsequent `wait()` by a new arrival sees SET and does not
     suspend. This keeps Event on the pure WaitQueue single-winner substrate.
   - Option E2: **wake-all** currently-registered waiters (drain the Event's
     WaitQueue), matching the intuitive "broadcast readiness" meaning of a
     manual-reset Event. This requires either a loop of `wake_wait_one` or a
     new multi-winner wake operation on the WaitQueue (which must NOT become a
     Select/multi-wait feature — Hard Rule 6).
   Both are internally consistent. The choice is *genuinely new*: the existing
   ready-flag substrate never faced it because it is single-registrant-per-flag
   (§2.2 Q6), so neither "wake-one" nor "wake-all" has prior art in the
   runtime. It cannot be silently picked (Hard Rule 10).

2. **Destruction with registered waiters.**
   - The E10 `~WaitQueue` asserts empty in debug (caller must drain). An Event
     destroyed with waiters registered is the same class of contract. But the
     *Event-level* policy (cancel-all on destruction? assert? UB?) is a new
     decision because Event owns its WaitQueue.

**Event verdict: `HUMAN-DECISION-REQUIRED`** on those two points. All other
Event semantics are RESOLVED.

> Naming-collision note (fact, not a decision): the existing
> `sluice::async::Mutex` (`include/sluice/async/mutex.hpp`) is a **synchronous**
> TSA-annotated `std::mutex` wrapper (CPP-STATIC-1). The async Mutex primitive
> (E12-C) must NOT silently reuse this name; either a distinct type name
> (e.g. `AsyncMutex`) or an explicit documented coexistence decision is
> required at E12-C implementation time. This is flagged here so it is not
> discovered late.

---

## 5. Semaphore semantic authority (Task D)

### 5.1 Conservation invariant (candidate)

The roadmap requires the accounting relationship:

```text
permits + granted permits + queued demand
```

to reconcile. The exact candidate invariant:

```text
(initial_permits)
    == (free permits)            // available now
      + (granted permits)        // handed to a waiter whose resolve_ CAS
                                 //   won but who has NOT yet resumed
      + (queued demand)          // waiters registered in the semaphore's
                                 //   WaitQueue, each demanding 1 permit
```

with the side condition that `granted` and `queued` are disjoint (a waiter is
in exactly one of: queued-demanding, granted-not-resumed, resumed-and-done).

### 5.2 The load-bearing race

```text
release() grants a waiter
    ↓
waiter's wake_wait_one resolve_ CAS wins
    ↓ (waiter is now "granted", permit decremented)
vs
the same waiter's deadline expires / cancel wins BEFORE the grant's CAS
```

**Who owns the granted permit during that race?** This is the Semaphore
analogue of E11's timer-lifetime closure. The grant and the cancel/expire
compete on the SAME `resolve_` CAS (one authority, Hard Rule 4). Therefore:

- The permit is **reserved for the waiter** only at the moment the grant's
  `resolve_(woken)` CAS succeeds.
- If cancel/expire's CAS wins instead, the waiter is NOT granted; the permit
  is **refunded** (returned to `free permits`). There is no "cancelled waiter
  keeps a permit."
- If the grant's CAS wins, cancel/expire is the loser (no-op, E10 truth table)
  and the permit stays with the waiter (granted → resumed → consumed).

This requires the grant path to perform the permit decrement and the
`resolve_(woken)` in the **same critical section**, and the cancel/expire path
to perform the refund and `resolve_(cancelled|expired)` in its same critical
section — exactly the E10/E11 winner-only authority shape.

### 5.3 Required policy decisions

| Decision | Candidate | Status |
| -------- | --------- | ------ |
| initial permits | constructor arg `initial_permits` | RESOLVED (convention) |
| maximum permits | **HUMAN DECISION** — bounded (reject overflow) vs unbounded | HUMAN DECISION REQUIRED |
| acquire one vs acquire N | first scope: **acquire-one only**; `acquire(N)` deferred | RESOLVED (scope) |
| release one vs release N | first scope: **release-one only**; `release(N)` deferred | RESOLVED (scope) |
| waiter ordering | FIFO (WaitQueue is FIFO, E10 §5) | RESOLVED |
| permit reservation | grant-at-CAS-win (§5.2) | RESOLVED |
| barging | **HUMAN DECISION** — barging (a new acquirer steals a just-released permit ahead of a queued waiter) vs strict FIFO handoff | HUMAN DECISION REQUIRED |
| cancelled waiter refund | refund (waiter not granted) | RESOLVED (§5.2) |
| expired waiter refund | refund (waiter not granted) | RESOLVED (§5.2) |
| overflow on release | coupled to the max-permits decision | HUMAN DECISION REQUIRED (with max) |
| destruction with waiters | **HUMAN DECISION** (same class as Event §4.4) | HUMAN DECISION REQUIRED |

**Semaphore verdict: `HUMAN-DECISION-REQUIRED`** (max permits / overflow +
barging + destruction).

---

## 6. Mutex semantic authority (Task E)

### 6.1 Fiber identity and execution ownership after E8 stealing

E8 established that a stolen Fiber may resume on the thief Worker; wake routes
by `WaitReg.owner` / `fiber_owner_` (the RUNNABLE ownership record), not by
pinning. `owner_of(f)` / `owner_id_of(f)` exist as diagnostics
(`scheduler.hpp:350`). TLS `g_worker` / `WorkerState::current` is the
*executing* worker; `fiber_owner_` is the *runnable-ticket* owner. A Fiber's
*identity* is the `Fiber` object address (stable, non-movable —
`fiber.hpp`); it is independent of which Worker executes it.

### 6.2 Answers (1–8)

1. **Is Mutex ownership bound to:** **Fiber identity.** The lock is acquired
   and released by a logical task (Fiber), and the ownership check must be
   stable across E8 worker migration. Binding to Worker or OS thread would
   break the moment a Fiber is stolen — a Fiber that locked on W0 and is
   rescheduled to W1 must still be able to unlock. The only identity that
   survives migration is the Fiber.

2. **Can a Fiber unlock after migration to a thief Worker?** **Yes.** This is
   the direct consequence of Fiber-identity ownership + E8 steal. The unlock
   checks the owner *Fiber*, not the *Worker*.

3. **Is unlock ownership checked?** **RESOLVED: yes, debug-assert + release
   error.** Unlock by a non-owner Fiber is a contract violation (mirrors
   `Completion::result()` before ready — debug assert, release
   `invalid_state`). This is required for correctness; an unchecked unlock
   breaks mutual exclusion silently.

4. **Recursive locking:** **FORBIDDEN in first scope.** A second `lock()` by
   the owner Fiber is a contract violation (debug assert / release error).
   Recursive locking multiplies the state space (per-owner recursion count)
   and is not needed for the first scope. Detection = assertion on the owner
   identity. (Deferred, not supported — same as the existing synchronous
   `Mutex` which is non-recursive.)

5. **On unlock with waiters:** **HUMAN DECISION — direct handoff vs
   competitive reacquire vs hybrid.** This is the roadmap's explicit
   "fairness / handoff / barging" decision. See §6.3.

6. **Timeout meaning if a waiter has been granted ownership but not resumed:**
   The grant's `resolve_(woken)` CAS already won; the deadline/expire path is
   the loser (no-op). The waiter resumes owning the lock. **RESOLVED: a grant
   that won the CAS is final; a late timer expiry cannot un-grant it.** (Same
   shape as Semaphore §5.2 and E11's loser semantic.)

7. **Can cancellation reclaim a handed-off lock?** Symmetric to (6): if
   handoff won the CAS, cancel is the loser. **RESOLVED: no.** A handed-off
   lock is not reclaimable by a cancellation that arrives after handoff.
   (Cancelling a *queued* waiter — before grant — is fine and just unlinks it.)

8. **Fairness guarantee:** **HUMAN DECISION** — coupled to (5). Options: FIFO
   strict (no barging, direct handoff), barging-allowed (higher throughput,
   possible starvation), or FIFO-with-barging-fallback.

### 6.3 Mutex — HUMAN DECISION REQUIRED (the roadmap's three)

The roadmap explicitly names: **fairness, handoff, barging.** These are one
decision in three facets:

- **Fairness:** FIFO (WaitQueue is already FIFO) vs barging-allowed.
- **Handoff vs competitive reacquire on unlock-with-waiters:**
  - direct handoff: unlock transfers ownership to the FIFO head waiter (via a
    grant CAS), no barging window.
  - competitive: unlock just marks free + wakes one; the wakee must re-acquire
    (a third Fiber could barge in).
  - hybrid: handoff when waiters exist, barging otherwise.
- **Barging:** whether a new acquirer may take a just-released permit ahead of
  a queued waiter.

These materially change observable behavior (throughput vs starvation
guarantees) and production evidence does not pick one. Plus the §4.4-style
destruction-with-waiters question.

**Mutex verdict: `HUMAN-DECISION-REQUIRED`** (fairness/handoff/barging +
destruction). All identity, recursive-lock, grant-vs-cancel, and
ownership-check questions are RESOLVED.

> Do not copy `std::mutex` (roadmap). The existing synchronous
> `sluice::async::Mutex` is `std::mutex`; the async primitive is a different
> object with Fiber-identity ownership and Fiber-suspending `lock()`. See the
> naming note in §4.4.

---

## 7. Condition semantic authority (Task F)

Condition hard-depends on Mutex (§3.3). The atomic protocol under audit:

```text
hold Mutex
    ↓
[atomic window]
release Mutex
+
register Condition WaitNode
+
commit suspension
    ↓
... notify ...
    ↓
wake wins
    ↓
Fiber runnable
    ↓
Mutex reacquire
```

### 7.1 The lost-notify window

The release-Mutex + register-WaitNode pair must be **one atomic transition
w.r.t. a `notify`**, or a notify that fires between the release and the
register is lost (the classic condition-variable hazard). The closed E10
substrate already proves the shape: `await_wait` does register+recheck+
make_waiting under `global_mtx_ + q.mtx()` with only `context_switch` outside
(E10 §4). For Condition, the "condition predicate" is the caller's
responsibility (E10 §4 note), but the release+register atomicity is the
primitive's — it must hold the Condition's mutex across the register step
(or use an equivalent handoff) so a `notify` cannot slip into the gap.

### 7.2 Answers (1–7)

1. **Is notify-one FIFO?** **RESOLVED: yes** (Condition's WaitQueue is FIFO,
   E10 §5; `wake_wait_one` resolves the head).
2. **Is notify-all required in first scope?** **HUMAN DECISION.** `notify_one`
   is trivially the WaitQueue `wake_wait_one`. `notify_all` requires either a
   multi-winner wake (a new operation — currently `wake_wait_one` is
   single-winner) OR a loop of `wake_wait_one`. notify-all is the canonical
   condition-variable broadcast and is likely required for correctness of
   standard patterns, but the *mechanism* (loop vs native multi-wake) and
   whether first-scope ships it is a decision. Coupled to the "no multi-wait /
   no Select" Hard Rule (do NOT build a multi-wake that looks like Select).
3. **Does `wait` return only after Mutex reacquisition?** **RESOLVED: yes.**
   `condition.wait(mtx)` returns only when the calling Fiber holds `mtx` again.
   This is the defining Condition contract.
4. **Deadline expires at various phases:**
   - **before notify:** waiter's expire wins; waiter resumes with
     `expired`, then attempts reacquire (may queue on the Mutex). Observable:
     wait returned `expired`, Mutex eventually reacquired.
   - **after notify but before reacquire:** notify's `resolve_(woken)` already
     won; expire is the loser (no-op). Waiter resumes woken, then reacquires.
     Observable: wait returned `woken` (notify), not `expired`.
   - **during Mutex queueing (after wake, while reacquiring):** the waiter is
     now on the *Mutex's* WaitQueue, not the Condition's. A deadline here is a
     *new* wait epoch (E10 epoch isolation) on the Mutex; it may expire there.
     Observable: the reacquire may itself time out. **RESOLVED by composition**
     (Condition wait = condition-wait epoch + mutex-acquire epoch; each has its
     own E11 deadline).
5. **Does the original wait deadline include reacquisition time?**
   **HUMAN DECISION.** Two valid models: (a) one deadline covers both the
   condition wait and the reacquire (reacquire time eats into the condition
   deadline); (b) the condition deadline governs only the condition-wait
   epoch, and reacquire has its own (possibly infinite) deadline. These differ
   observably when the Mutex is contended at wake time.
6. **Can cancellation win while reacquiring?** **RESOLVED: yes, on the Mutex
   epoch** — a cancel during reacquire cancels the *Mutex acquire* (a separate
   wait epoch), not the (already-won) condition wait. The caller observes a
   cancelled reacquire and does NOT hold the Mutex (symmetric to a cancelled
   `mutex.lock()`).
7. **Spurious wake permitted?** **RESOLVED: no, not as a first-class
   semantic.** The closed substrate does not have spurious wakes — every wake
   is a real `resolve_(woken)` winner. Condition's predicate-recheck is still
   required (the caller must re-check the predicate on resume, as in all
   condition variables), but the runtime does not inject spurious wakes.

### 7.3 Can Condition be specified without E13 multi-wait?

**Yes.** Condition = one Condition-WaitQueue + the Mutex. Each `wait` is a
single wait epoch on the Condition queue, followed by a single wait epoch on
the Mutex queue. No Fiber ever waits on both simultaneously — the
release+register atomic window guarantees the Condition wait is committed
before the Mutex is released (or, equivalently, the Mutex is reacquired
before the Condition wait is considered resolved). This is **not** multi-wait
(E13); it is two sequential single-waits. **RESOLVED: Condition does not
require E13.**

**Condition verdict: `HUMAN-DECISION-REQUIRED`** (notify-all mechanism/scope
+ whether the deadline spans reacquire). The release/register atomic window,
FIFO notify-one, reacquire-before-return, and no-E13-dependence are RESOLVED.

---

## 8. Queue semantic authority (Task G)

### 8.1 First-scope minimum

```text
bounded vs unbounded          — HUMAN DECISION (see below)
capacity zero semantics       — derived from the bounded decision
send / push                   — producer op; may block on not-full
recv / pop                    — consumer op; may block on not-empty
close                         — lifecycle terminal
producer wait queue           — not-full waiters (WaitQueue)
consumer wait queue           — not-empty waiters (WaitQueue)
```

The roadmap requires: `not-empty waiters`, `not-full waiters`, `close
semantics`.

### 8.2 Policy decisions

| Decision | Resolution |
| -------- | ---------- |
| bounded vs unbounded | **HUMAN DECISION REQUIRED.** Bounded enforces backpressure (needs slot accounting + not-full waiters); unbounded drops not-full entirely (simpler, no backpressure). Materially different APIs. |
| capacity zero | **RESOLVED conditionally:** if bounded, capacity-0 means rendezvous (send blocks until a recv is registered). Derived, not independent. |
| close with buffered items | **HUMAN DECISION REQUIRED** — drain-on-close (consumers may still recv remaining items, producers reject) vs immediate-discard. |
| close with blocked producers | **RESOLVED: producers wake with a `closed` outcome** (analogous to `cancelled`/`expired`); their `send` returns an error. No deadlock. |
| close with blocked consumers | **HUMAN DECISION REQUIRED** — wake with `closed` (recv returns "no more data") vs let them drain buffered items first then `closed`. Coupled to the close-with-buffered decision. |
| push after close | **RESOLVED: error** (`closed`), no enqueue. |
| pop after close but data remains | coupled to close-with-buffered decision — **HUMAN DECISION REQUIRED**. |
| timeout after item reservation | **RESOLVED by §5.2 analogue:** if the consumer's `resolve_(woken)` won, the item is reserved to it; a late expire is the loser. If expire won before reservation, the item stays for another consumer. |
| cancel after slot reservation | **RESOLVED by §5.2 analogue:** symmetric — cancel losing to a grant leaves the slot reserved; cancel winning refunds the slot. |

### 8.3 Stable grant object (analogous to E11 TimerRegistration)?

The reservation question: does an item or slot reservation require a stable
grant object like E11's `TimerRegistration`? **RESOLVED: no new primitive-
level grant object is required in first scope.** The "reservation" is
captured by the `resolve_(woken)` CAS itself: the winner is the unique
consumer/producer that gets the item/slot. There is no post-destruction
deference hazard as with timers (the buffer outlives the wait epoch — it is
owned by the Queue, not the waiter's frame), so the E11 lifetime-closure
obligation does not transfer. A stable grant object would only be needed if
we later support item/slot *transfer across primitive boundaries* (deferred).

**Queue verdict: `HUMAN-DECISION-REQUIRED`** (bounded/unbounded +
close-with-buffered/close-with-consumers, which are one cluster). Reservation
under cancel/timeout is RESOLVED by composition.

---

## 9. RwLock semantic authority (Task H)

RwLock is last (highest state complexity). Minimum first-scope:

### 9.1 Fairness policy

| Policy | Verdict |
| ------ | ------- |
| reader preference | DEFER — starvation of writers under continuous readers |
| writer preference | DEFER — starvation of readers |
| phase fairness (reader/writer batches) | **HUMAN DECISION REQUIRED** — the canonical anti-starvation choice |
| FIFO (strict arrival order) | **HUMAN DECISION REQUIRED** — simplest fair option |

First scope must pick **exactly one** anti-starvation policy; production
evidence does not choose. (The existing WaitQueue is FIFO, which biases
toward the FIFO option, but RwLock's reader-merge makes pure FIFO
non-obvious.)

### 9.2 Classification

| Feature | Verdict | Reason |
| ------- | ------- | ------ |
| upgrade (read → write) | **DEFER** | Adds a third state (upgrade-candidate); high complexity. First scope: a reader that wants write must release then re-acquire write. |
| downgrade (write → read) | **DEFER** | Symmetric to upgrade; defer. First scope: release write then acquire read. |
| recursive read | **FORBID** (first scope) | Reader-count accounting with recursive reads requires per-Fiber counting; defer. |
| recursive write | **FORBID** | Same as Mutex §6.2 Q4 (forbidden). |

### 9.3 Starvation

Any single policy from §9.1 must be proven not to starve a class under
realistic schedules. This is the RwLock formal gate (§11).

**RwLock verdict: `HUMAN-DECISION-REQUIRED`** (fairness policy). All
upgrade/downgrade/recursive classifications are RESOLVED (FORBID/DEFER). Do
NOT implement RwLock in this preparation task.

---

## 10. Cross-primitive semantic matrix (Task I)

### 10.1 State / cleanup matrix

| Primitive | Resource state | WaitQueue count | Grant/reservation state | Ownership identity | Deadline cleanup | Cancellation cleanup |
| --------- | -------------- | --------------- | ----------------------- | ------------------ | ---------------- | -------------------- |
| Event | bool `set_` | 1 (waiters) | none | none | unlink node (expire loses to set's wake) | unlink node |
| Semaphore | `permits` counter | 1 (demand) | permit granted at CAS-win | none | refund permit if expire wins before grant | refund permit if cancel wins before grant |
| Mutex | locked bool | 1 (waiters) | ownership granted at CAS-win | **Fiber identity** | none (grant is final; expire loses to grant) | none (cancel loses to grant); cancel of a *queued* waiter just unlinks |
| Condition | none (delegates to Mutex) | 1 (its own) + Mutex's | none | via Mutex | expire on condition epoch → then reacquire epoch (may expire separately) | cancel on condition epoch → reacquire epoch |
| Queue | buffer + closed bool | 2 (not-empty, not-full) | item/slot reserved at CAS-win | none | item stays / slot refunded (§8.2) | symmetric |
| RwLock | reader count + writer bool (+ policy) | 1–2 (readers, writers) | read-permit / write at CAS-win | writer: Fiber identity | refund read permit if expire wins before grant | refund / unlink |

### 10.2 Resolution-cause matrix

| Primitive | `RESOURCE_WAKE` means | `TIMER_EXPIRE` means | `CANCEL` means |
| --------- | --------------------- | -------------------- | -------------- |
| Event | `set()` woke the head; late `set()` observed without parking | deadline elapsed while UNSET-and-waiting → `expired` | wait-cancelled → `cancelled`; Event state unchanged |
| Semaphore | a permit was granted to this waiter → `woken` | deadline elapsed before grant → `expired`; permit refunded | cancel before grant → `cancelled`; permit refunded |
| Mutex | ownership granted to this waiter → `woken` | deadline before grant → `expired`; no ownership | cancel before grant → `cancelled`; no ownership |
| Condition | notify woke this waiter → proceed to reacquire | condition-wait deadline → `expired`; then reacquire epoch | condition wait cancelled → `cancelled`; then reacquire epoch |
| Queue (consumer) | an item reserved to this waiter → `woken` | deadline before reservation → `expired`; item stays | cancel before reservation → `cancelled`; item stays |
| Queue (producer) | a slot reserved to this waiter → `woken` | deadline before reservation → `expired`; slot refunded | cancel before reservation → `cancelled`; slot refunded |
| RwLock (reader) | read-permit granted → `woken` | deadline before grant → `expired`; permit refunded | cancel before grant → `cancelled`; permit refunded |
| RwLock (writer) | write granted → `woken` | deadline before grant → `expired` | cancel before grant → `cancelled` |

**E12-G will use this matrix as its baseline** (the retrospective
cross-primitive audit).

---

## 11. Formal verification requirements (Task J)

Per primitive: the correct invariant, minimum formal state dimensions, and
one required negative model. These are proof *targets* — no models are
implemented in this preparation.

### 11.1 Event

- **Correct invariant:** `set_ == SET` ⇒ no waiter is indefinitely parked
  (liveness); `set_ == UNSET` ⇒ a registered waiter is parked unless
  woken/cancelled/expired. The register+recheck window is closed (no
  set-between-check-and-register loss).
- **Minimum state dimensions:** `set_ ∈ {UNSET, SET}`; WaitQueue contents;
  per-node `resolve_` state; (deadline dimension via E11).
- **Required negative model — broken lost-set protocol:**
  ```text
  waiter checks UNSET
  set() occurs
  waiter registers
  waiter sleeps forever
  ```
  The correct model's register+recheck (the `await_wait` idiom) prevents this.

### 11.2 Semaphore

- **Correct invariant:** the conservation invariant §5.1:
  `initial == free + granted + queued` at all times; `granted ∩ queued = ∅`.
- **Minimum state dimensions:** `permits` counter; `granted` count; WaitQueue
  contents; per-node resolution state; deadline dimension.
- **Required negative model — broken permit double-consumption or permit
  loss:** a release that grants a waiter AND increments `free` (double-
  consumption), or a cancel that neither refunds nor consumes (permit loss).
  Counterexample: `free + granted + queued ≠ initial`.

### 11.3 Mutex

- **Correct invariant:** mutual exclusion (at most one owner); the ownership
  identity is a single Fiber; unlock-by-non-owner rejected; grant is final
  (expire/cancel after grant are losers).
- **Minimum state dimensions:** `locked` bool; `owner ∈ Fiber ∪ {none}`;
  WaitQueue contents; per-node resolution state; deadline dimension.
- **Required negative model — broken double ownership or handed-off
  ownership loss:** two Fibers both believing they own the lock (double
  ownership), or a handed-off grant lost when a straggler cancel/expire
  "reclaims" it. Counterexample: `owner` set to two distinct Fibers, or
  `owner` cleared by a losing cancel.

### 11.4 Condition

- **Correct invariant:** the release-Mutex + register-Condition-WaitNode
  window is atomic w.r.t. notify (no lost notify); `wait` returns only with
  Mutex reacquired.
- **Minimum state dimensions:** Mutex state (locked/owner/queue); Condition
  WaitQueue contents; two-epoch resolution (condition epoch + reacquire
  epoch); deadline dimension.
- **Required negative model — broken release/register lost-notify window:**
  ```text
  release Mutex
  notify (Condition queue empty → lost)
  register Condition WaitNode
  suspend forever
  ```
  The correct model holds the Mutex (or equivalent) across register.

### 11.5 Queue

- **Correct invariant:** item/slot reservation is exactly-once per
  `resolve_(woken)` winner; close is monotonic (once closed, stays closed);
  no item is both buffered and reserved; no slot is both free and reserved.
- **Minimum state dimensions:** buffer contents; `closed` bool; not-empty
  WaitQueue; not-full WaitQueue; per-node resolution state; deadline
  dimension.
- **Required negative model — broken item/slot reservation under
  cancellation or close:** a consumer is reserved an item AND the item
  remains in the buffer (double-consumption), or a closed Queue still accepts
  sends. Counterexample: an item reachable after close-send, or a slot
  counted both free and reserved.

### 11.6 RwLock

- **Correct invariant:** at most one writer OR one-or-more readers (never
  both); writer identity is a single Fiber; the chosen fairness policy
  (§9.1) is not starvable.
- **Minimum state dimensions:** `reader_count`; `writer ∈ Fiber ∪ {none}`;
  readers/writers WaitQueue(s); fairness-policy state (e.g. phase, batch);
  per-node resolution state; deadline dimension.
- **Required negative model — broken writer/readers coexistence or
  starvation-policy violation:** a writer and a reader both active
  simultaneously, or a class (readers or writers) starved indefinitely under
  the chosen policy. Counterexample: `writer ≠ none ∧ reader_count > 0`, or a
  fairness-liveness property violated.

---

## 12. Semantic decision summary

| Primitive | Verdict | Resolved items | Open human-authority items |
| --------- | ------- | -------------- | -------------------------- |
| **E12-A Event** | `HUMAN-DECISION-REQUIRED` | manual-reset choice; idempotent set; reset; wait-on-set; deadline/cancel composition; set-vs-register race; reset-vs-waiter | wake cardinality (wake-one vs wake-all on `set`); destruction-with-waiters |
| **E12-B Semaphore** | `HUMAN-DECISION-REQUIRED` | conservation invariant; grant-at-CAS; refund on cancel/expire; FIFO; acquire/release-one scope | max permits / overflow; barging; destruction-with-waiters |
| **E12-C Mutex** | `HUMAN-DECISION-REQUIRED` | Fiber-identity ownership; migration-safe unlock; ownership-checked unlock; recursive FORBID; grant final vs cancel/expire | fairness / handoff / barging; destruction-with-waiters; naming coexistence with sync `Mutex` |
| **E12-D Condition** | `HUMAN-DECISION-REQUIRED` | release/register atomic window; FIFO notify-one; reacquire-before-return; no-E13-dependence; no spurious wake | notify-all mechanism/scope; deadline spanning reacquire? |
| **E12-E Queue** | `HUMAN-DECISION-REQUIRED` | close-with-blocked-producers; push-after-close; reservation under cancel/timeout; no new grant object needed | bounded/unbounded; close-with-buffered / close-with-consumers |
| **E12-F RwLock** | `HUMAN-DECISION-REQUIRED` | upgrade/downgrade DEFER; recursive read/write FORBID | fairness policy (reader-pref/writer-pref/phase/FIFO) |
| **E12-G Audit** | `DEFERRED` (runs after A–F) | uses the §10 matrix as baseline | — |

---

## 13. Implementation stop conditions

E12 implementation (any subphase) MUST stop and request human authority when:

1. A primitive needs a wake cardinality the closed substrate does not
   natively offer (e.g. Event wake-all, Condition notify-all) — the mechanism
   choice (loop vs native multi-wake) must not silently become a multi-wait/
   Select-like feature (Hard Rule 6).
2. A primitive's cancellation/deadline cleanup would require a
   primitive-local timeout protocol or primitive-local cancellation winner
   (Hard Rule 3) — it must instead refine through `resolve_`.
3. A primitive's ownership identity cannot be expressed as Fiber identity
   stable across E8 migration (Mutex/RwLock writer).
4. The formal model (per §11) would omit a load-bearing dimension (M2) —
   specifically the resource/accounting/ownership dimension the primitive
   introduces.
5. The grant-vs-cancel/expire race (§5.2, generalized) cannot be closed on
   the single `resolve_` CAS.

---

## 14. Cross-links

- E10 as-built (authoritative wait protocol):
  [`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)
- E11 spec (CLOSED, authoritative deadline/timer):
  [`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md)
- E11 insertion audit (style precedent for this document):
  [`docs/e11-arch-recon-audit.md`](e11-arch-recon-audit.md)
- Construction method (M1–M9, binding):
  [`docs/async-runtime-construction-method.md`](async-runtime-construction-method.md)
- Roadmap (E12 placement; updated in the same commit):
  [`docs/async-runtime-plan.md`](async-runtime-plan.md)
- Formal model directories: `docs/spec/e10_waitnode/`, `docs/spec/e11_timer_wait/`
