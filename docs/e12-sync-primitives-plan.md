# E12-PREP — Async Synchronization Primitive Frontier

> Read-only architecture / API audit and documentation preparation for
> **E12 — Async Synchronization Primitives**, performed before any E12
> production, runtime-test, or formal-model implementation begins.
>
> **Status:** PREPARATION COMPLETE. Verdict: `E12-PREP: READY`.
>
> **E12-B Semaphore per-primitive authority (E12-B-SEMAPHORE-PREPARATION-
> CORRECTIVE-1):** the §5 Semaphore authority is superseded by
> [`docs/e12-semaphore.md`](e12-semaphore.md) (policy register A1–A5, corrected
> permit conservation, release state machine, **Conclusion A** Scheduler-seam
> proof) and the safety-only formal model in
> [`docs/spec/e12_semaphore/`](spec/e12_semaphore/) (gate:
> [`scripts/verify-e12-semaphore-formal.sh`](../scripts/verify-e12-semaphore-formal.sh)).
> E12-B status:
> ```text
> E12-B-PREPARATION-CORRECTIVE-1: COMPLETE
> E12-B-PREPARATION-DOC-AUTHORITY-CORRECTIVE-2: COMPLETE
> E12-B-PREPARATION-REAUDIT: PASS
> E12-B-PREPARATION: CLOSED
> E12-B-IMPLEMENTATION-1: COMPLETE
> E12-B-IMPLEMENTATION: REVIEW-REQUIRED
> ```
> Preparation is CLOSED. The production Semaphore implementation is COMPLETE
> and committed (see [`docs/e12-semaphore.md`](e12-semaphore.md) §14 As-Built);
> status remains REVIEW-REQUIRED pending an independent adversarial
> implementation review. This document does NOT self-declare `E12-B: CLOSED`.
> The cross-primitive preparation (this document) is otherwise unchanged.
>
> **E12-C Async Mutex per-primitive authority (E12-C-PREPARATION-CORRECTIVE-1
> through CORRECTIVE-5):**
> the §6 Mutex authority is superseded by
> [`docs/e12-async-mutex.md`](e12-async-mutex.md) (policy register M-H1–M-H4,
> ownership state model, naming authority `AsyncMutex`, minimum private seam
> `MUTEX-HANDOFF-ONE`, collapsed atomic admission actions (no interleaving
> window), formal model with no admissionPhase field, queue = only Suspended
> epochs, publication discipline, property classification, negative-model
> matrix). E12-C status:
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
> E12-C-IMPLEMENTATION: REVIEW-REQUIRED
> ```
> The cross-primitive preparation (this document) §6, §10.1, §10.2, §11.3,
> §12, §14.3.3, and §14.5 are updated to reflect the corrective closure.
>
> **E12-E Queue Corrective-2 authority:**
> ```text
> E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1:
> SUPERSEDED — REQUEST-CHANGES
> E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1-REVIEW:
> REQUEST-CHANGES
> E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2:
> PASS — INDEPENDENT ADVERSARIAL REVIEW PASS (B2)
> E12-E IMPLEMENTATION AUTHORIZATION: DENIED — B4 OPEN
>
> Gate status (current):
>   B1 Mutex no-throw substrate:          PASS  (independent review complete)
>   B2 Corrective-2 independent review:   PASS  (independent adversarial review complete)
>   B3 Condition T25 migration/reacquire: PASS  (W1 corrective db656b5)
>   B4 Queue formal model:                OPEN
> ```
> Queue depends on `ASYNC-MUTEX-NOTHROW-AUTHORITY-1`, whose design is accepted
> and whose production substrate is now implemented and independently reviewed
> (B1 PASS — `docs/reviews/ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-REVIEW.md`).
>
> **Condition baseline isolation:**
> ```text
> E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1:
> PASS — closed by W1 corrective (db656b5)
> Condition build: PASS
> Condition runtime suite: PASS (Clang Debug/ASan/TSan full suite green)
> T25 migration/reacquire: PASS (deterministic rewrite)
> ```
> Evidence: `docs/async-runtime-hang-and-gcc-corrective.md` §B.1/§C/§E.1. The
> T25 hang was root-caused to a test-harness defect (unbounded coordinator
> waits); no production code was changed.
>
> Corrective history: `E12-PREP-CORRECTIVE-1-REVIEW` returned
> `CORRECTIVE-REQUIRED` with six accepted defects (F-EVENT-1, F-SEM-1,
> F-COND-1, F-GRANT-1, F-QUEUE-1, F-DEP-1). This revision is the
> **E12-PREP-CORRECTIVE-2** closure of all six: the Event wake cardinality is
> fixed to set-releases-all (public semantic resolved; only the loop-vs-
> wake-many *mechanism* remains an implementation boundary), the Semaphore
> accounting law is corrected (supply separated from queued demand), the
> Condition return contract is exposed as one Model-A/Model-B human-decision
> cluster, a normative Primitive Grant Commit Insertion Boundary section is
> added, Queue capacity-zero/rendezvous is reclassified as DEFERRED, and the
> Queue internal structural lock is distinguished from the future E12-C async
> Mutex. See §4, §5, §7, §8, §14, and the cross-section matrices §10–§13.
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

A fourth label is used for cross-cutting concerns that are fixed at the
semantic level but whose realization substrate is not yet chosen:

```text
IMPLEMENTATION BOUNDARY  — public semantic is fixed; the mechanism that
                           realizes it is to be audited/resolved during the
                           relevant E12 subphase, not in this preparation
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
see §4.4 and §8.4), the `E12-A..F` identifiers are **renumbered** to match the
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
     map that is single-registrant-per-key, not the WaitQueue. (No
     `wake_wait_all` / `wake_many` operation exists in the codebase — §14.)
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
| `WaitQueue` `wake_wait_one` (E10) | none (no readiness state; wake is an action) | **one** per call (FIFO head winner), but the queue holds many waiters | n/a (action, not state) | n/a | **High — Event's wake path resolves each registered waiter through its own `resolve_(Woken)` attempt. The *public semantic* (set releases all registered waits) is fixed (§4.4); the *mechanism* (loop `wake_wait_one` vs a narrow Scheduler-owned wake-many) is an IMPLEMENTATION BOUNDARY to audit during E12-A (§14), not a human semantic decision.** |
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
wake-cardinality *public semantic* is now fixed in §4.4 (set releases all
registered waits satisfied by SET); the remaining open item is the
*implementation mechanism* (loop vs narrow wake-many), classified in §14 as an
IMPLEMENTATION BOUNDARY for E12-A. See §4.1, §4.4.

---

## 3. Canonical dependency order (Task B)

### 3.1 Per-primitive protocol deltas

| Primitive | New state dimension | New resource accounting | New ownership law | Depends on | New cancel/deadline cleanup obligation | State-space complexity |
| --------- | ------------------- | ----------------------- | ----------------- | ---------- | -------------------------------------- | ---------------------- |
| **Event** | one persistent bool `set_` | none | none | WaitNode (E10) + deadline (E11) | unlink registered node on cancel/expire; no permit/ownership to reclaim | low (2 states × queue) |
| **Semaphore** | stored permit count (`available_`) | **permit accounting** (§5.1): `available_ + acquiredCount == initial_permits + accepted_release_count`; explicit FIFO queued demand is a *separate* dimension, NOT supply; no grant-in-flight state | none | Event's persistent-bool pattern (optional); WaitNode + deadline | cancel/expire serialized before release observes the queue (Conclusion A, §5.2) | medium (counter × queue) |
| **Mutex** | `owner : Fiber* \| NoOwner` | none (one lock) | **Fiber-identity ownership + migration law** | Semaphore (permits=1 is a near-subset; but Mutex adds ownership) | cancel/expire before grant unlink queued demand; cancel/expire after Woken handoff lose and cannot change owner | medium (owner × queue) |
| **Condition** | none new (delegates to Mutex) | none | none (Mutex owns it) | **Mutex** (mandatory) | release/register atomic window; reacquire on wake/timeout/cancel — **return contract is one open Model-A/Model-B cluster (§7)** | medium-high (two queues + mutex reacquire) |
| **Queue** | fixed-capacity FIFO ring + closed bool | bounded capacity >= 1; producer + consumer wait epochs; operation-owned payload handles; no independent reservation owner | suspended operation epoch | WaitNode + deadline; fixed QueuePort; **a synchronous structural lock (NOT E12-C async Mutex) for internal state** | expiry/close/grant winner commits payload/completion before intrusive runnable publication; external cancellation deferred (§8, §14.3.4) | high (ring × 2 queues × closed × publication) |
| **RwLock** | reader count + writer bool + (upgrade state) | reader count accounting | writer owner identity | Semaphore (read permits) + Mutex (write) | reader/writer starvation; upgrade downgrade cancellation; **writer winner-before-publication commit (§14)** | **highest** |

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
   persistent bool       permit supply accounting          the queue substrate
   + WaitQueue           (queued demand separate)          and ownership law)
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
          │              (return contract  that needs exclusive
          │               = open cluster)  ownership semantics)
          │                          │
          ▼                          ▼
   E12-E Queue ──────uses a SYNCHRONOUS structural lock internally
   bounded buffer + not-empty / not-full (Event-like) + close    (NOT E12-C async Mutex — §8.4)
          │   (winner-before-publication commit seam — §14)
          ▼
   E12-F RwLock ──────uses Semaphore-pattern (read permits) + Mutex-pattern (write)
   reader count + writer + (upgrade/downgrade)
   (writer winner-before-publication commit seam — §14)
          │
          ▼
   E12-G Cross-Primitive Cancellation / Deadline Audit
   (uses the Task-I matrix as baseline)
```

### 3.3 Edge-by-edge evaluation (protocol dependency, not API popularity)

- **Event before Semaphore.** Event introduces exactly one new state
  dimension — a persistent boolean — and reuses the closed wait substrate.
  Semaphore introduces *bounded permit accounting* — a stored-permit
  conservation invariant (`available_ + acquiredCount == initial_permits +
  accepted_release_count`, §5.1) with an explicit FIFO queued-demand
  dimension, an atomic release disposition (transfer / store / reject),
  FIFO-with-no-barging selection, and deadline/cancel composition. The
  simpler, state-lighter primitive comes first and validates the
  Event→WaitQueue integration. **Event → Semaphore.**
- **Semaphore before Mutex.** A Mutex is, at the accounting layer, a
  Semaphore with permit count 1 PLUS an ownership identity law and a handoff
  discipline. Building Semaphore first establishes the supply-accounting
  invariant and the grant-vs-cancel/expire race resolution that Mutex then
  specializes with ownership. (Mutex is NOT merely "Semaphore with count 1"
  — but Semaphore's accounting is a strict prerequisite pattern.)
  **Semaphore → Mutex.**
- **Mutex before Condition.** Condition is *defined* in terms of Mutex:
  `wait` = release-mutex + register-condition-wait + suspend + reacquire-mutex.
  There is no Condition without a Mutex to release and reacquire. This is a
  hard protocol dependency, not a preference. **Mutex → Condition.**
- **Condition before Queue.** Queue does NOT depend on Condition (a Queue's
  not-empty/not-full waiters are Event-like single-winner waits, not broadcast
  condition waits), and Queue does NOT depend on the future E12-C async Mutex
  for its internal state (§8.4 — it uses a short synchronous structural lock).
  Condition comes before Queue in the trunk because Queue is the first
  primitive with *two* wait queues (producer/consumer), a `close` lifecycle,
  AND a one-shot payload lease plus winner-before-publication ownership-transfer
  obligation (§14) — a state-space / protocol-complexity step beyond
  Condition's two-queue-but-no-buffer shape. Ordering Condition before Queue
  keeps state-space / protocol complexity monotonically increasing.
  **Condition → Queue.** (This edge is justified by complexity progression,
  not by any "Queue requires AsyncMutex" claim — that claim is false, see §8.4.)
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
- While `SET`, `wait()` returns without suspending (the under-lock recheck
  observes SET and returns, undoing any speculative registration).
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
| `reset()` semantics | **RESOLVED: clear the bool.** Does NOT wake anyone; does NOT cancel a registered waiter; future `wait()` blocks. | Manual-reset definition. |
| `wait()` on already-set | **RESOLVED: returns without suspending.** | §2.2 Q3; under-lock recheck idiom. |
| `wait()` + deadline | **RESOLVED: compose E11 `await_wait_deadline`.** Expired → `WaitOutcome::expired`. | E11 closed substrate. |
| `wait()` + cancellation | **RESOLVED: `cancel_wait` → `WaitOutcome::cancelled`.** | E10 closed substrate. |
| `set()` vs registration race | **RESOLVED: under Event mutex, `set()` flips the bool to SET and attempts RESOURCE_WAKE resolution for every currently registered waiter; a concurrent registrar observes SET and does not suspend.** | The register+recheck+make_waiting atomic-window idiom (E10 §4); cardinality fixed in §4.4. |
| `reset()` vs already-registered waiter | **RESOLVED: `reset()` only clears the bool; it does not cancel a registered waiter.** A waiter already in the queue remains until woken, cancelled, or expired. This is the manual-reset contract. | Avoids a primitive-local cancellation winner (Hard Rule 3). |
| **wake cardinality** | **RESOLVED: `set()` releases ALL currently registered Event waits whose readiness condition is satisfied by SET.** See §4.4. Not a human decision. | Manual-reset liveness; a SET Event must not strand a registered waiter. |
| set-releases-all mechanism | **IMPLEMENTATION BOUNDARY (audit during E12-A):** loop `wake_wait_one` until drained vs a narrow Scheduler-owned wake-many. See §4.4, §14. | Public semantic is fixed; mechanism is a substrate choice. |
| destruction with waiters | **HUMAN DECISION REQUIRED — see §4.4.** |

### 4.4 Event — wake cardinality RESOLVED; destruction open; mechanism deferred

**F-EVENT-1 closure.** The previous draft treated *wake-one vs wake-all* as two
equally-valid public semantic choices. That is incorrect for a persistent
manual-reset Event: under wake-one, two waiters registered while UNSET can
leave one stranded forever while the Event remains SET (the late-arrival path
returns, `set()` is idempotent, and `reset()` does not wake the stranded
waiter) — a direct violation of the Event liveness target. The normative
public semantic is therefore fixed:

```text
set()
    ->
Event becomes SET
    ->
every currently registered Event waiter whose readiness condition
is satisfied by SET becomes eligible for RESOURCE_WAKE resolution
```

In practical first-scope terms:

```text
set() attempts RESOURCE_WAKE resolution for every currently
registered Event wait epoch.
```

Each waiter remains an independent E10/E11 wait epoch. Therefore:

```text
W1 RESOURCE_WAKE
W2 RESOURCE_WAKE
W3 RESOURCE_WAKE
```

are **separate `WaitNode::resolve_(Woken)` attempts** — one CAS per waiter, one
runnable publication per winning epoch. Wake-all MUST NOT mean one multi-wait
winner, one shared winner CAS, Select, or any multi-wait feature (Hard Rule 6).
For each waiter, `RESOURCE_WAKE` vs `TIMER_EXPIRE` vs `CANCEL` still compete
through that waiter's OWN `resolve_`; a waiter whose deadline/cancellation
already won is skipped/no-op by the existing resolution authority (E10 loser
truth table).

**Required counterexample closure (V1):**

```text
UNSET
W1 register
W2 register

set -> SET

W1 resolves RESOURCE_WAKE
W2 resolves RESOURCE_WAKE

W3 arrives after SET
W3 returns without suspension
```

No registered waiter remains blocked solely because `set()` woke only one.
`set()` attempts resolution for *every* registered epoch.

**Race with a per-waiter timer (set races W2 TIMER_EXPIRE):**

```text
W1/W2/W3 registered
set races W2 TIMER_EXPIRE
```

Correct result:

```text
set attempts RESOURCE_WAKE for each live wait epoch

W2's TIMER_EXPIRE may win W2 (W2's own resolve_ CAS)

W1/W3 resource wake may win independently

one result per WaitNode
one runnable publication per winning epoch
```

A timer that wins one waiter does not defeat `set()`'s attempts on the other
waiters; each epoch is resolved independently.

**Implementation mechanism classification.** How the runtime realizes
"resolve every registered waiter" is NOT a public semantic decision. It is:

```text
IMPLEMENTATION BOUNDARY TO AUDIT DURING E12-A
```

The two candidate mechanisms are:

```text
loop Scheduler::wake_wait_one until the Event WaitQueue is drained

vs

add a narrow Scheduler-owned wake-many operation
```

The public semantic (`manual-reset SET releases all registered waits satisfied
by SET`) is already fixed above; this preparation does NOT choose the
mechanism and does NOT implement wake-many (Hard Rule 5; no generic
synchronization base class, no Select-like multi-wake). The as-built substrate
has no `wake_many` today (§14), so the loop is the only currently-available
realization; whether a narrow wake-many seam is added is an E12-A
implementation decision. This boundary is recorded in §14 alongside the grant
seam analysis.

**Destruction with registered waiters — HUMAN DECISION REQUIRED.** This is the
*only* remaining open Event policy. The E10 `~WaitQueue` asserts empty in
debug (caller must drain). An Event destroyed with waiters registered is the
same class of contract, but the Event-level policy (cancel-all on destruction?
assert? UB?) is a new decision because Event owns its WaitQueue. Keep it as
explicit HUMAN DECISION REQUIRED unless existing production authority already
fixes the public contract (it does not). Do NOT silently choose cancel-all; do
NOT make the destructor a primitive-local cancellation winner (Hard Rule 3).
Do NOT conflate destruction policy with wake cardinality.

Event may therefore remain:

```text
EVENT SEMANTICS RESOLVED EXCEPT DESTRUCTION CONTRACT
```

The open destruction contract does NOT block E12-A implementation of the
non-destruction Event semantics (set/reset/wait/late-waiter/cardinality); it
blocks only the destruction path, which can be specified before the destructor
is exercised. This is recorded so it is not discovered late.

**Event verdict (E12-A CLOSED - two independent corrective reviews passed):**

```text
PUBLIC SEMANTICS RESOLVED + IMPLEMENTED:
    manual-reset persistent Event
    set() releases all registered Event waits satisfied by SET

IMPLEMENTATION BOUNDARY RESOLVED:
    loop wake_wait_one_locked until drained (event_set_broadcast), atomic
    under global_mtx_. No native wake-many WaitQueue API added.

PUBLIC AUTHORITY SEALED (E12-A-EVENT-CORRECTIVE-1 F-EVENT-AUTH):
    raw wait_queue() accessor REMOVED; narrow Event::cancel(node) added.
    Ordinary production code CANNOT obtain the Event's WaitQueue (compile probe).

DESTRUCTION CONTRACT RESOLVED:
    caller contract violation; ~Event asserts empty in debug, no cancel/wake.

E12-A CORRECTED — INDEPENDENT CLOSURE REVIEW REQUIRED — see docs/e12-event.md
    (as-built topology) + docs/spec/e12_event/ (formal model) +
    scripts/verify-e12-event-formal.sh (formal gate).
```

> Naming-collision note (fact, not a decision): the existing
> `sluice::async::Mutex` (`include/sluice/async/mutex.hpp`) is a **synchronous**
> TSA-annotated `std::mutex` wrapper (CPP-STATIC-1). The async Mutex primitive
> (E12-C) must NOT silently reuse this name; either a distinct type name
> (e.g. `AsyncMutex`) or an explicit documented coexistence decision is
> required at E12-C implementation time. This is flagged here so it is not
> discovered late.

---

## 5. Semaphore semantic authority (Task D)

### 5.1 Permit accounting (F-SEM-1 closure; E12-B-PREPARATION-CORRECTIVE-1
###      final authority)

The accepted first-scope permit conservation law is:

```text
available_ + acquiredCount == initial_permits + accepted_release_count
```

The terms:

```text
available_              stored, unassigned permits (the atomic counter)
acquiredCount           permits acquired by immediate acquisition OR by queued
                        grant transfer
initial_permits         permits created at construction
accepted_release_count  release() calls that contributed one permit
                        (transfer to a waiter, or store into available_)
```

A rejected overflow release increments no counter and is not an accepted
release. Queued demand is NOT a term in this law (it is a separate dimension;
see §5.1.2). There is NO `grant-in-flight` / `granted_not_yet_committed`
production state in first-scope Semaphore.

The four counter transitions (mirroring §5.2's atomic release disposition):

```text
Immediate acquire (available_ > 0, no eligible queued waiter):
    available_-- ; acquiredCount++

Release transfer to an eligible queued waiter:
    accepted_release_count++ ; acquiredCount++ ; available_ unchanged

Release store (no eligible waiter, available_ < max_permits):
    accepted_release_count++ ; available_++

Overflow rejection (no eligible waiter, available_ == max_permits):
    all counters unchanged
```

The law preserves:

```text
initial_permits = 0
one waiter queues
```

with supply **unchanged** (queued demand is not on the supply side). It also
represents:

```text
initial_permits = 0
release
release
release
```

as **supply growth** (if the max-permits policy allows the releases):
`accepted_release_count` grows by 3, so the RHS grows by 3 across
`available_` / `acquiredCount` as permits are stored and acquired. Queued
waiter count is NEVER used on the supply side.

> **Historical / OBSOLETE model (non-authoritative).** Earlier drafts used a
> three-term candidate law
> `initial_supply + successful_release_count == free_permits +
> granted_not_yet_committed + successful_acquire_count` and described a
> `grant-in-flight` / `granted_not_yet_committed` state. **That model is
> obsolete and non-authoritative.** First-scope Semaphore has no such
> production state, and the simplified two-term law above is the accepted
> authority. The obsolete terms may appear ONLY in this labelled historical
> passage and in name-and-reject passages; they do not describe any
> first-scope Semaphore state. (See also §5.2 and
> [`docs/e12-semaphore.md`](e12-semaphore.md) §3.)

#### 5.1.1 (merged into §5.1)

The candidate-law sub-section (§5.1.1) is merged into §5.1 above as the final
accepted authority. No separate candidate law remains.

#### 5.1.2 Queued demand law

Separately define:

```text
queued_demand
    ==
number of live Semaphore wait epochs that:

    still demand one permit
    and
    have not won RESOURCE_WAKE grant
    and
    have not won TIMER_EXPIRE
    and
    have not won CANCEL
```

`QUEUED` and granted are **disjoint** waiter states. A grant winner is no
longer queued demand (it has moved to `acquiredCount` on the supply side).
Queued demand does not create, consume, or refund supply; it is purely a
count of unsatisfied wait epochs.

### 5.2 The release disposition (F-SEM-ACCT-1 closure, E12-B-PREPARATION-CORRECTIVE-1)

The previous draft modelled release grant by first executing `available_--`
and then refunding on a lost CAS. That model is **deleted** (it underflows at
`available_ == 0` and double-counts). First-scope Semaphore has **no**
`granted_not_yet_committed` / `granted_in_flight` production state.

The accepted release disposition is atomic (release runs in one
`Scheduler::global_mtx_` critical section):

```text
release():
    one pending permit is created by this release call

    if an eligible queued waiter wins (FIFO head):
        transfer that pending permit directly to exactly one waiter
        available_ unchanged                 (no decrement, no underflow)
        return true

    if no eligible queued waiter wins:
        if available_ == max_permits:
            return false  with no state mutation
        otherwise:
            available_++
            return true
```

**Permit conservation law (simplified):**

```text
available_ + acquiredCount == initial_permits + accepted_release_count
```

The four counter transitions:

```text
Immediate acquire (available_ > 0, no eligible queued waiter):
    available_-- ; acquiredCount++

Release transfer to an eligible queued waiter:
    accepted_release_count++ ; acquiredCount++ ; available_ unchanged

Release store (no eligible waiter, available_ < max_permits):
    accepted_release_count++ ; available_++

Overflow rejection (no eligible waiter, available_ == max_permits):
    all counters unchanged
```

A rejected overflow release is not an accepted release. Queued demand is not a
term in the supply law (§5.1.2). There is **no refund**: the release-created
pending permit is either transferred, stored, or rejected — never
pre-decremented and re-deposited.

**Conclusion A — the Scheduler seam (F-SEM-SEAM-1 closure).** Whether a
`release()` can grant W2 after W1 "loses" depends on whether a linked FIFO
head can lose its `resolve_(Woken)` CAS while `release` observes it. An
exhaustive audit of every private `WaitQueue` resolver call-site
(`src/async/scheduler.cpp`; `Scheduler` sole friend at
`include/sluice/async/wait_queue.hpp:145`) proves it cannot:

```text
wake, cancel, and expire are ALL serialized by Scheduler::global_mtx_;
a winning cancel/expire unlinks the node in the SAME critical section as its
resolve_ CAS, before releasing global_mtx_;
therefore a linked FIFO head observed by wake_wait_one_locked under
global_mtx_ + q.mtx() is necessarily Registered and eligible, and its
resolve_(Woken) CAS cannot lose.
```

Under the production lock protocol, `wake_wait_one_locked` returns `nullptr`
**only when the queue is empty**. W1-cancelled/W2-live behavior is therefore
proven by **cancellation unlinking W1 before release observes the queue**
(cancel is `global_mtx_`-serialized; release acquires `global_mtx_` next,
observes W1 gone, grants W2) — NOT by a release-side skip-after-null. The
existing private seam is **sufficient**; no Scheduler extension is introduced.
Full evidence + the stable-state invariant
(`EligibleQueuedWaiterExists => available_ == 0`) are in
[`docs/e12-semaphore.md`](e12-semaphore.md) §5. (The grant-commit insertion
boundary classification is unchanged: §14.3.2.)

### 5.3 Required policy decisions (A1–A5 closed by E12-B-PREPARATION-CORRECTIVE-1)

| Decision | Candidate | Status |
| -------- | --------- | ------ |
| initial permits | constructor arg `initial_supply`; `0 ≤ initial ≤ max` | RESOLVED (A1) |
| maximum permits | **bounded; `max_permits > 0` mandatory; constructor violations are debug-asserted caller contract violations** | RESOLVED (A1) |
| acquire one vs acquire N | first scope: **acquire-one only**; `acquire(N)` deferred | RESOLVED (scope) |
| release one vs release N | first scope: **release-one only**; `release(N)` deferred | RESOLVED (scope) |
| **waiter ordering** | **selection among already-queued waiters uses E10 WaitQueue FIFO order** | RESOLVED (§5.4) |
| **barging** | **forbidden** — a newly arriving acquire may not bypass an already-eligible queued waiter; `try_acquire` succeeds iff `available_>0` AND no eligible waiter has FIFO priority | RESOLVED (A2) |
| permit reservation | none — release creates a pending permit that is transferred/stored/rejected, never pre-decremented (§5.2) | RESOLVED (A1/§5.2) |
| cancelled waiter disposition | the cancel resolver is `global_mtx_`-serialized; no refund path exists (no permit was removed) | RESOLVED (A3/§5.2 Conclusion A) |
| expired waiter disposition | symmetric to cancel; `acquire_until` permit-first precedence over a due deadline (A4) | RESOLVED (A4) |
| overflow on release | `release()` returns `false` with no mutation iff no eligible waiter accepts AND `available_==max_permits` | RESOLVED (A1) |
| `acquire_until` deadline precedence | permit admissible + deadline due → Woken; no permit + deadline due → Expired; `try_acquire` has no deadline semantics | RESOLVED (A4) |
| `available()` semantics | observational snapshot; may stale; `available()>0` ⇒ nothing about a later `try_acquire` | RESOLVED (A5) |
| destruction with waiters | registered wait epochs = caller contract violation; dtor cancels/wakes/synthesizes nothing; outstanding acquired permits not runtime-tracked | RESOLVED (A3) |

### 5.4 FIFO + no-barging (A2 closure)

The first-scope contract is exact:

```text
selection among ALREADY-QUEUED waiters uses E10 WaitQueue FIFO order
AND
barging is forbidden: a newly arriving acquire may not bypass an
already-eligible queued waiter.
```

`try_acquire()` succeeds only when both hold: `available_ > 0` AND no eligible
queued waiter has FIFO priority. An immediate acquire consumes a stored permit
only when no eligible waiter is queued. There is no global "acquisition FIFO"
claim to qualify: barging is forbidden, so FIFO selection governs the
admission order unconditionally.

### 5.5 Required trace accounting (S1–S4)

The simplified law
(`available_ + acquiredCount == initial_permits + accepted_release_count`)
reconciles:

**S1.**

```text
initial = 0
W1 queues
```

- Supply unchanged: `available_=0`, `acquiredCount=0`,
  `accepted_release_count=0`. Law holds: `0 + 0 == 0 + 0`.
- Queued demand = 1 (W1, a live wait epoch). Queued demand is not a supply
  term.

**S2.**

```text
initial = 1
W1 acquires successfully
W2 queues
W3 queues
```

- Supply: `available_=0`, `acquiredCount=1`, `accepted_release_count=0`. Law
  holds: `0 + 1 == 1 + 0`.
- Queued demand = 2 (W2, W3). Queued demand does not create supply.

**S3.**

```text
W1 queued (available_ = 0)
release one
CANCEL wins before grant
```

- Under Conclusion A, cancel is `global_mtx_`-serialized and unlinks W1 before
  the release's `wake_wait_one_locked` observes the queue. The release's
  pending permit sees no eligible waiter, so it is stored: `available_: 0→1`,
  `accepted_release_count: 0→1`, `acquiredCount` unchanged. Law holds:
  `1 + 0 == 0 + 1`. There is no refund path (no permit was ever removed from
  `available_`).

**S4.**

```text
W1 queued (available_ = 0)
release one
RESOURCE_WAKE grant wins
```

- The release's pending permit is transferred directly to W1:
  `accepted_release_count: 0→1`, `acquiredCount: 0→1`, `available_` unchanged
  (no decrement, no underflow). Law holds: `0 + 1 == 0 + 1`. There is no
  `granted_not_yet_committed` state (first-scope has none); `acquiredCount`
  counts the grant directly.

**Semaphore verdict (E12-B-PREPARATION-CORRECTIVE-1):**

```text
permit conservation corrected (available_ + acquiredCount ==
    initial_permits + accepted_release_count; no granted_in_flight term)
queued demand separate (QUEUED ∩ GRANTED = ∅)
queued waiter selection FIFO; barging FORBIDDEN (A2)
max permits / overflow RESOLVED (A1)
destruction-with-waiters RESOLVED (A3)
deadline precedence RESOLVED (A4)
available() observational RESOLVED (A5)
Scheduler seam: Conclusion A (sufficient; no extension)
PREPARATION: REAUDIT-REQUIRED — IMPLEMENTATION BLOCKED
    (see docs/e12-semaphore.md + docs/spec/e12_semaphore/ +
     scripts/verify-e12-semaphore-formal.sh)
```

---

## 6. Mutex semantic authority (Task E)

> **E12-C-PREPARATION-CORRECTIVE-1 through CORRECTIVE-5:** §6 is
> superseded by
> [`docs/e12-async-mutex.md`](e12-async-mutex.md) (policy register M-H1–M-H4,
> ownership state model, naming authority, seam specification, collapsed
> atomic admission actions (no interleaving window), formal model with no
> admissionPhase field, queue = only Suspended epochs, property classification,
> negative-model matrix). The per-primitive authority is the new document.
> This section is retained as the cross-primitive preparation record and
> updated to reflect the corrective closures.
>
> E12-C status:
> ```text
> E12-C-PREPARATION-CORRECTIVE-1: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-2: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-3: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-4: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-5: COMPLETE
> E12-C-PREPARATION-CORRECTIVE-5-REAUDIT: PASS
> E12-C-PREPARATION: CLOSED
> E12-C-IMPLEMENTATION: READY
> ```

### 6.1 Fiber identity and execution ownership after E8 stealing

E8 established that a stolen Fiber may resume on the thief Worker; wake routes
by `WaitReg.owner` / `fiber_owner_` (the RUNNABLE ownership record), not by
pinning. `owner_of(f)` / `owner_id_of(f)` exist as diagnostics
(`scheduler.hpp:350`). TLS `g_worker` / `WorkerState::current` is the
*executing* worker; `fiber_owner_` is the *runnable-ticket* owner. A Fiber's
*identity* is the `Fiber` object address (stable, non-movable —
`fiber.hpp`); it is independent of which Worker executes it.

### 6.2 Answers (1–8) — CLOSED by E12-C-PREPARATION-CORRECTIVE-1

1. **Is Mutex ownership bound to:** **Fiber identity.** The lock is acquired
   and released by a logical task (Fiber), and the ownership check must be
   stable across E8 worker migration. Binding to Worker or OS thread would
   break the moment a Fiber is stolen — a Fiber that locked on W0 and is
   rescheduled to W1 must still be able to unlock. The only identity that
   survives migration is the Fiber.

2. **Can a Fiber unlock after migration to a thief Worker?** **Yes.** This is
   the direct consequence of Fiber-identity ownership + E8 steal. The unlock
   checks the owner *Fiber*, not the *Worker*.

3. **Is unlock ownership checked?** **RESOLVED: yes.** Unlock by a non-owner
   Fiber is a caller contract violation; debug assertion; must not mutate owner
   or queue; not a supported successful no-op contract. This is required for
   correctness; an unchecked unlock breaks mutual exclusion silently.

4. **Recursive locking:** **FORBIDDEN in first scope.** A second `lock()` by
   the owner Fiber is a contract violation (debug assert). Recursive locking
   multiplies the state space (per-owner recursion count) and is not needed
   for the first scope. Detection = assertion on the owner identity.

5. **On unlock with waiters:** **CLOSED — direct ownership handoff (M-H1).**
   Unlock resolves the eligible FIFO head with Woken, commits ownership to the
   winner Fiber BEFORE publishing it as runnable, and publishes the winner
   AFTER ownership is committed. See [`docs/e12-async-mutex.md`](e12-async-mutex.md)
   §8.3 and §9.

6. **Timeout meaning if a waiter has been granted ownership but not resumed:**
   The grant's `resolve_(woken)` CAS already won; the deadline/expire path is
   the loser (no-op). The waiter resumes owning the lock. **RESOLVED: a grant
   that won the CAS is final; a late timer expiry cannot un-grant it.** (Same
   shape as Semaphore §5.2 and E11's loser semantic.)

7. **Can cancellation reclaim a handed-off lock?** Symmetric to (6): if
   handoff won the CAS, cancel is the loser. **RESOLVED: no.** A handed-off
   lock is not reclaimable by a cancellation that arrives after handoff.

8. **Fairness guarantee:** **CLOSED — FIFO + no barging (M-H2, M-H3, M-H4).**
   Eligible queued waiters use FIFO order; barging is forbidden; try_lock fails
   while an eligible waiter has FIFO priority. See
   [`docs/e12-async-mutex.md`](e12-async-mutex.md) §2.

### 6.3 Mutex — CLOSED by E12-C-PREPARATION-CORRECTIVE-1

The roadmap's three decisions — **fairness, handoff, barging** — are closed
by E12-C-PREPARATION-CORRECTIVE-1 (policy register M-H1–M-H4 in
[`docs/e12-async-mutex.md`](e12-async-mutex.md) §2):

```text
M-H1:  unlock-with-waiters uses direct ownership handoff
M-H2:  eligible queued waiters use FIFO order
M-H3:  barging is forbidden
M-H4:  try_lock fails while an eligible waiter has FIFO priority
```

The Semaphore (E12-B A2) is precedent, not authority for Mutex ownership
policy. This corrective is the human authority that selects direct handoff.

**Grant-seam coupling (F-GRANT-1) — CLOSED.** Direct handoff requires the
exact winner identity before runnable publication. The minimum private seam
`MUTEX-HANDOFF-ONE` is specified in
[`docs/e12-async-mutex.md`](e12-async-mutex.md) §10. The seam classification
is now:

```text
MUTEX GRANT SEAM:
    MINIMUM MUTEX-SPECIFIC PRIVATE SEAM REQUIRED (§10 of e12-async-mutex.md)
```

The conditional dependency on unresolved handoff policy is removed.

**Mutex verdict: `PREPARATION CORRECTIVE COMPLETE — REAUDIT REQUIRED`** —
all identity, recursive-lock, grant-vs-cancel, ownership-check,
fairness/handoff/barging, naming, and seam questions are CLOSED. A fresh
adversarial re-audit may change preparation to CLOSED and implementation to
READY.

> The existing synchronous `sluice::async::Mutex` is a TSA-annotated
> `std::mutex` wrapper (structural lock). The async primitive is named
> `AsyncMutex` to coexist without collision. See
> [`docs/e12-async-mutex.md`](e12-async-mutex.md) §3.

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

### 7.2 F-COND-1 closure — ONE semantic-policy cluster (Model A vs Model B)

**The defect.** The previous draft simultaneously said (Q3) "`wait` returns
only after Mutex reacquisition" / "caller holds Mutex on return" AND (Q5/Q6)
"reacquire is a separate deadline/cancellable Mutex epoch that may
expire/cancel and return without Mutex." These are **incompatible public
contracts**. This revision removes the silent hybrid and exposes the cluster
as one HUMAN DECISION between two explicitly modeled alternatives.

The following are treated as ONE semantic-policy cluster:

```text
Condition outcome
Mutex reacquisition
return-with-Mutex guarantee
deadline span
cancellation during reacquisition
observable result shape
```

#### 7.2.1 Model A — Mandatory Reacquisition

```text
Condition wait epoch resolves:

    WOKEN
    EXPIRED
    CANCELLED

then:

    Mutex reacquisition is mandatory

then:

    wait returns holding Mutex

return value preserves the Condition epoch outcome
```

Consequences:

```text
the original deadline may pass during reacquisition
Condition wait may return after the original deadline
reacquisition cannot independently turn the returned result into "Mutex not held"
```

This model must answer the **persistent cancellation problem**:

```text
Condition epoch CANCEL wins
cancel request remains active
mandatory reacquire begins
```

A normal cancellable Mutex acquire may immediately cancel again. Therefore
Model A requires an explicit semantic statement such as:

```text
mandatory reacquisition is non-cancellable with respect to the
already-observed Condition cancellation
```

or equivalently:

```text
cancellation scope is consumed / masked for mandatory reacquisition
```

This is a **protocol consequence**, not an implementation mechanism. Do NOT
invent an implementation mechanism here (Hard Rule 8): identify only that, in
Model A, the cancellation that won the Condition epoch must not re-win the
mandatory reacquire epoch.

A plain `WaitOutcome` is *sufficient* for Model A (the return carries the
Condition epoch outcome: `woken`/`expired`/`cancelled`, and the Mutex is held
in all three cases).

#### 7.2.2 Model B — Independently Abortable Reacquisition

```text
Condition epoch resolves

then:

    deadline/cancellable Mutex acquire epoch

wait may return without Mutex
```

This requires a result model capable of representing:

```text
Condition outcome
Reacquire outcome
Mutex-held state
```

A plain Condition-style `WaitOutcome` is **insufficient** for Model B: the
caller must observe both whether the Condition epoch was woken/expired/cancelled
AND whether the reacquire succeeded/failed (and thus whether the Mutex is
held).

### 7.3 Correct classification

Production/codebase evidence does NOT choose A or B. (The current substrate
has no Condition primitive; E11's two-epoch composition is consistent with
either model depending on whether the reacquire epoch inherits the Condition
epoch's cancellation state.) Therefore:

```text
CONDITION CONTRACT:
HUMAN DECISION REQUIRED
```

That is the expected acceptable preparation result. Do NOT silently choose A.
Do NOT silently choose B. Do NOT retain any statement saying
`RESOLVED: wait returns holding Mutex` while also retaining independently
abortable reacquisition. (The previous Q3 "RESOLVED: yes" is removed; Q5/Q6
"separate epoch" language is retained only as the *definition* of the
reacquire step, not as a resolved public contract.)

### 7.4 Answers retained as RESOLVED

The following are independent of the Model-A/Model-B choice and remain
RESOLVED:

1. **Is notify-one FIFO?** **RESOLVED: yes** (Condition's WaitQueue is FIFO,
   E10 §5; `wake_wait_one` resolves the head).
2. **Is notify-all required in first scope?** **HUMAN DECISION (mechanism).**
   `notify_one` is trivially the WaitQueue `wake_wait_one`. `notify_all`
   requires either a multi-winner wake (a new operation — currently
   `wake_wait_one` is single-winner; no `wake_many` exists, §14) OR a loop of
   `wake_wait_one`. notify-all is the canonical condition-variable broadcast
   and is likely required for correctness of standard patterns, but the
   *mechanism* (loop vs native multi-wake) and whether first-scope ships it is
   a decision. Coupled to the "no multi-wait / no Select" Hard Rule (do NOT
   build a multi-wake that looks like Select). (Like Event's set-releases-all,
   the *public semantic* of notify-all is "release all registered condition
   waiters"; the *mechanism* is an implementation boundary. Unlike Event,
   whether first-scope *ships* notify-all at all is part of the decision.)
7. **Spurious wake permitted?** **RESOLVED: no, not as a first-class
   semantic.** The closed substrate does not have spurious wakes — every wake
   is a real `resolve_(woken)` winner. Condition's predicate-recheck is still
   required (the caller must re-check the predicate on resume, as in all
   condition variables), but the runtime does not inject spurious wakes.

### 7.5 Required trace matrix (C1–C4) under both models

Because the policy remains HUMAN DECISION REQUIRED, outcomes are shown under
both Model A and Model B:

**C1 — deadline before notify.**

```text
Model A: Condition epoch EXPIRED wins. Mandatory reacquire (non-cancellable
         w.r.t. this expiry). wait returns EXPIRED, holding Mutex.
Model B: Condition epoch EXPIRED. Reacquire epoch (its own deadline/cancel).
         If reacquire succeeds: returns EXPIRED, holding Mutex.
         If reacquire deadline/cancel wins: returns EXPIRED, NOT holding Mutex.
```

**C2 — notify wins; original deadline passes during Mutex contention.**

```text
Model A: Condition epoch WOKEN (notify's CAS won; expire is loser).
         Mandatory reacquire: the original deadline may pass during
         reacquisition; reacquire is non-cancellable w.r.t. the (already-lost)
         Condition expiry. wait returns WOKEN, holding Mutex (possibly after
         the original deadline).
Model B: Condition epoch WOKEN. Reacquire epoch: the original deadline (or a
         new one) governs reacquire. If reacquire times out: returns WOKEN,
         NOT holding Mutex.
```

**C3 — cancellation arrives during reacquisition.**

```text
Model A: Condition epoch outcome stands (WOKEN/EXPIRED/CANCELLED). Mandatory
         reacquire masks the already-observed Condition cancellation; the
         reacquire does not re-cancel. wait returns the Condition outcome,
         holding Mutex.
Model B: Reacquire is a cancellable epoch. If cancel wins reacquire: returns
         the Condition outcome, NOT holding Mutex.
```

**C4 — reacquisition completes after original deadline.**

```text
Model A: Allowed. wait returns the Condition outcome, holding Mutex, even
         though the return time is after the original deadline.
Model B: Depends on whether reacquire's own deadline allowed it. If reacquire
         succeeded: holding Mutex. If reacquire's deadline expired first: NOT
         holding Mutex.
```

### 7.6 Can Condition be specified without E13 multi-wait?

**Yes.** Condition = one Condition-WaitQueue + the Mutex. Each `wait` is a
single wait epoch on the Condition queue, followed by a single wait epoch on
the Mutex queue. No Fiber ever waits on both simultaneously — the
release+register atomic window guarantees the Condition wait is committed
before the Mutex is released (or, equivalently, the Mutex is reacquired
before the Condition wait is considered resolved). This is **not** multi-wait
(E13); it is two sequential single-waits. **RESOLVED: Condition does not
require E13.**

**Condition verdict: `HUMAN-DECISION-REQUIRED`** — the return-contract cluster
(Model A mandatory-reacquisition vs Model B independently-abortable) is one
open decision; notify-all mechanism/scope is a second open decision. The
release/register atomic window, FIFO notify-one, no-E13-dependence, and
no-spurious-wake are RESOLVED.

---

## 8. Queue semantic authority (Task G) — Corrective-2 binding override

> **Current authority:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2:
> PASS — AUTHOR SELF-ASSESSMENT`
>
> **Applied disposition:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1:
> SUPERSEDED — REQUEST-CHANGES`
>
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1-REVIEW:
> REQUEST-CHANGES`
>
> **Implementation authorization:** `DENIED`
>
> This section supersedes every older Queue-specific statement elsewhere in
> this preparation document. Any remaining Queue reference to
> `HUMAN-DECISION-REQUIRED`, reusable item-reference admission, explicit
> item/slot reservation, active-owner steal prohibition, ordinary quiet drain,
> cancellation in v1, direct handoff, terminal skip, 21/8 transitions, or the
> original integration PASS is `NON-BINDING HISTORICAL ANALYSIS`.

### 8.1 Queue v1 scope

```text
bounded MPMC, runtime capacity >= 1
FIFO ring + FIFO producer/consumer WaitQueues
no barging
result-bearing selected-waiter grant
producer -> ring -> consumer only
one-shot move-only QueueItemLease admission
failed push returns the complete original opaque lease
drain-on-close; close monotonic and idempotent
deadline expiry retained
external cancellation deferred
own-oldest ticket selection, otherwise global-oldest
active admission owner does not block stealing
irreversible QueueTeardownSession destruction protocol
```

Rendezvous, unbounded capacity, direct handoff, close-with-cause, Select,
overflow drop/conflation, and emplace are deferred.

### 8.2 Atomic move-only lease ownership, not reservation

Queue v1 has no independent item/slot reservation state and accepts no reusable
`QueueItemBase&`/pointer. QueuePort consumes one unforgeable lease by value.
Entry requires a non-empty lease, matching immutable port identity, and
`location == detached`; it then establishes:

```text
detached -> producer operation
producer operation -> ring on commit
producer operation -> detached + exact lease return on failure
ring -> consumer operation on pop
consumer operation -> released during typed extraction
ring -> teardown -> released during destruction
```

The fixed ring stores move-only lease slots. Move empties the source and
requires an empty destination, so one control cannot occupy two positions.
Committed opaque push results contain no lease; Closed/Expired/WouldBlock
results contain the complete original lease. Typed conversion validates
port/type and moves `T` exactly once through `failed(Status,T&&)` outside locks.

`resolve_(Woken)` identifies the winner; Queue-specific payload transfer and
completion execute immediately afterward under the same lock-held CommitGap.
The integration's embedded operation state is the final owner, not an
outstanding permit/reservation. Tokio-style Permit remains a Queue-v2
candidate and is not mixed into this machine.

```text
SUPERSEDED FOR QUEUE V1

Queue v1 uses atomic move-only lease ownership transfer into the
fixed ring. Explicit slot reservation/Permit remains a Queue v2
candidate only.
```

Timed admission retains `global_mtx_` continuously from deadline-heap capacity
reservation through immediate PREPARED discard or PREPARED->ACTIVE linkage;
the reserved vector slot therefore cannot be consumed by another admission.
State/resource outcomes precede an already-due deadline: Closed/immediate
capacity for push, and an immediate item or Closed+empty for pop; expiry is the
inline result only when the operation would otherwise register.

### 8.3 Ticket stealing, teardown, counters, cancellation, and targets

Winner publication uses the stable `fiber_owner_` mapped-value address captured
before registration. The referenced entry may not be erased until the ticket
is removed, the Fiber resumes, and the operation releases the slot. Rehash may
invalidate iterators but not the stored element address; steal writes only the
existing mapped value under G.

```text
worker has own Queue ticket -> choose own oldest
otherwise                   -> choose global oldest
admission owner active      -> still stealable
```

Ordinary `take_quiescent_item` is removed. Quiet destruction transitions
irreversibly to `tearing_down` and returns one non-copyable
`QueueTeardownSession`. The session drains unique leases outside ordinary
CallGuard. `active_port_calls_` counts only ordinary operations inside the
non-template QueuePort; it does not prove the complete typed method returned.

Current `CancelToken` is polling-only and cannot safely wake a suspended Queue
operation. Queue v1 therefore ships no cancellation API, private Scheduler
cancel seam, or cancelled result.

```text
P8/C7: RESERVED — DEFERRED — NOT IN QUEUE V1
PUB-P-CANCEL/PUB-C-CANCEL: RESERVED — DEFERRED

TARGET COVERAGE:
19 canonical transitions
6 publication transitions

VERIFIED COVERAGE — AUTHOR SELF-ASSESSMENT:
19/19 canonical transitions
6/6 publication transitions
```

Expiry remains P9/C8; numbering is preserved.

### 8.4 Structural synchronization and dependency

Queue uses synchronous TSA-annotated `Mutex` for short structural critical
sections; it never awaits the Fiber-aware `AsyncMutex` internally. Lock order:

```text
Scheduler global -> Queue state -> at most one role WaitQueue
Scheduler global -> wake mutex
```

The current `Mutex::lock()` can propagate `std::system_error`. Queue therefore
depends on [`ASYNC-MUTEX-NOTHROW-AUTHORITY-1`](async-mutex-nothrow-authority.md),
which selects no-throw/fail-fast authoritative acquisition. No production
Queue work may begin until that substrate passes independent review and is
separately authorized.

### 8.5 Exact architecture references

Binding details live in:

* [`docs/e12-queue.md`](e12-queue.md) current semantic authority;
* [`docs/e12-queue-state-machine.md`](e12-queue-state-machine.md) corrected
  Corrective-2 lease/control state machine and 19/6 target;
* [`docs/e12-queue-scheduler-integration.md`](e12-queue-scheduler-integration.md)
  fixed QueuePort access graph, exact public results, one-shot lease,
  teardown session, concrete PREPARED guard, active-victim runnable route,
  counter ledger, transition matrix, and adversarial traces;
* [`docs/async-mutex-nothrow-authority.md`](async-mutex-nothrow-authority.md)
  dependent lock-failure policy.

```text
E12-E IMPLEMENTATION AUTHORIZATION: DENIED
```

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

### 9.3 Grant commit boundary (F-GRANT-1)

Reader and writer grants are classified separately (§14):

```text
WRITER:
    winner W resolve_(Woken)
        ->
    writer owner = W.fiber
        ->
    publish W
    requires exact winner identity before publication.
    REQUIRES WINNER-BEFORE-PUBLICATION COMMIT SEAM

READER:
    reader grant may pre-increment anonymous reader accounting if rollback is safe
    (a losing reader grant refunds the increment).
    Current Scheduler seam may suffice for anonymous reader accounting;
    winner identity is not required for the count itself.
    (Audit at E12-F; coupled to the chosen fairness policy, e.g. reader-merge.)
```

### 9.4 Starvation

Any single policy from §9.1 must be proven not to starve a class under
realistic schedules. This is the RwLock formal gate (§11).

**RwLock verdict: `HUMAN-DECISION-REQUIRED`** (fairness policy). All
upgrade/downgrade/recursive classifications are RESOLVED (FORBID/DEFER). The
writer winner-before-publication commit seam is identified (§14). Do NOT
implement RwLock in this preparation task.

---

## 10. Cross-primitive semantic matrix (Task I)

### 10.1 State / cleanup matrix

| Primitive | Resource state | WaitQueue count | Grant/reservation state | Ownership identity | Deadline cleanup | Cancellation cleanup |
| --------- | -------------- | --------------- | ----------------------- | ------------------ | ---------------- | -------------------- |
| Event | bool `set_` | 1 (waiters) | none | none | unlink node (expire loses to set's per-waiter wake) | unlink node |
| Semaphore | `available_` (stored-permit count) | 1 (demand) | release creates a pending permit transferred/stored/rejected atomically; no pre-reservation, no refund (§5.2); seam sufficient Conclusion A (§14.3.2) | none | no permit was removed; expire serialized before release observes queue (Conclusion A) | symmetric (cancel serialized before release observes queue) |
| Mutex | `owner : Fiber* \| NoOwner` | 1 (waiters) | ownership granted at CAS-win; **direct handoff REQUIRES minimum private MUTEX-HANDOFF-ONE seam (§14.3.3; E12-C-PREPARATION-CORRECTIVE-1 closed)** | **Fiber identity** | cancel/expire before grant unlink queued demand; cancel/expire after Woken handoff lose and cannot change owner | none (cancel loses to grant); cancel of a *queued* waiter just unlinks |
| Condition | none (delegates to Mutex) | 1 (its own) + Mutex's | none | via Mutex | condition epoch expire → then reacquire epoch (Model A: mandatory non-cancellable; Model B: separate, may expire — §7) | condition epoch cancel → reacquire epoch (Model A: masked; Model B: may cancel — §7) |
| Queue | FIFO ring + closed bool | 2 (producers, consumers) | no independent reservation owner; winner transfers `producer operation -> ring` or `ring -> consumer operation` before publication (§8.2, §14.3.4) | operation epoch while suspended | expiry winner retains producer payload / leaves ring item in place | external cancellation deferred in v1 |
| RwLock | reader count + writer bool (+ policy) | 1–2 (readers, writers) | read-permit (anonymous, may pre-increment) / write at CAS-win (**writer needs winner-before-publication seam, §14**) | writer: Fiber identity | refund read permit if expire wins before grant | refund / unlink |

### 10.2 Resolution-cause matrix

| Primitive | `RESOURCE_WAKE` means | `TIMER_EXPIRE` means | `CANCEL` means |
| --------- | --------------------- | -------------------- | -------------- |
| Event | `set()` attempted RESOURCE_WAKE for EVERY registered waiter (one `resolve_` per epoch); late `wait()` observes SET without parking | deadline elapsed while UNSET-and-waiting → `expired` | wait-cancelled → `cancelled`; Event state unchanged |
| Semaphore | a permit granted to this waiter → `woken` (the release's pending permit is transferred directly; `available_` unchanged) | deadline elapsed before grant → `expired`; no permit was removed (no refund path — Conclusion A) | cancel before grant → `cancelled`; serialized before release observes the queue (Conclusion A) |
| Mutex | ownership granted to this waiter → `woken` (direct handoff: owner = winner Fiber BEFORE publication) | deadline before grant → `expired`; no ownership | cancel before grant → `cancelled`; no ownership |
| Condition | notify woke this waiter → proceed to reacquire (Model A: mandatory; Model B: separate epoch — §7) | condition-wait deadline → `expired`; then reacquire (§7 trace C1–C4) | condition wait cancelled → `cancelled`; then reacquire (§7 trace C1–C4) |
| Queue (consumer) | ring head atomically transferred to this operation before publication → `woken` | expiry wins first → `expired`; item stays in ring | deferred/not reachable in Queue v1 |
| Queue (producer) | this operation's payload atomically transferred into ring before publication → `woken` | expiry wins first → `expired`; operation retains payload | deferred/not reachable in Queue v1 |
| RwLock (reader) | read-permit granted → `woken` | deadline before grant → `expired`; permit refunded | cancel before grant → `cancelled`; permit refunded |
| RwLock (writer) | write granted → `woken` (writer owner = winner Fiber) | deadline before grant → `expired` | cancel before grant → `cancelled` |

**E12-G will use this matrix as its baseline** (the retrospective
cross-primitive audit).

---

## 11. Formal verification requirements (Task J)

Per primitive: the correct invariant, minimum formal state dimensions, and
one required negative model. These are proof *targets* — no models are
implemented in this preparation.

### 11.1 Event

- **Correct invariant:** `set_ == SET` ⇒ **no registered waiter is
  indefinitely parked** (liveness — `set()` attempts RESOURCE_WAKE for every
  registered epoch, so none is stranded); `set_ == UNSET` ⇒ a registered
  waiter is parked unless woken/cancelled/expired. The register+recheck
  window is closed (no set-between-check-and-register loss). Each registered
  waiter is resolved by its own `resolve_` (one CAS, one runnable publication
  per winning epoch).
- **Minimum state dimensions:** `set_ ∈ {UNSET, SET}`; WaitQueue contents;
  per-node `resolve_` state; (deadline dimension via E11).
- **Required negative model — broken stranded-waiter protocol:**
  ```text
  W1, W2 registered while UNSET
  set() -> SET
  set() wakes only W1
  W2 sleeps forever while Event is SET
  ```
  The correct model's `set()`-releases-all semantic prevents this (§4.4).
  Also covered: the register+recheck lost-set protocol (waiter checks UNSET,
  `set()` occurs, waiter registers, waiter sleeps forever) — prevented by the
  `await_wait` recheck idiom.

### 11.2 Semaphore (E12-B-PREPARATION-CORRECTIVE-1 — safety model implemented)

- **Correct invariant (simplified conservation law, §5.2):**
  `available_ + acquiredCount == initial_permits + accepted_release_count` at
  all times. There is NO `granted_in_flight` term (first-scope has no such
  production state); there is NO refund path (release creates a pending permit
  that is transferred / stored / rejected, never pre-decremented). Queued
  demand is NOT on the supply side. The full state-invariant catalog is in
  [`docs/e12-semaphore.md`](e12-semaphore.md) §8 and modelled in
  [`docs/spec/e12_semaphore/E12Semaphore.tla`](spec/e12_semaphore/E12Semaphore.tla):
  `InvPermitConservation`, `InvPermitBounds`, `InvQueueWellFormed`,
  `InvSingleResolution`, `InvSinglePublication`, `InvGrantCommitCoupling`,
  `InvFIFOGrant`, `InvAdmissionClosure`, `InvOverflowNonMutation`,
  `InvNoIdlePermitWithEligibleWaiter`, `InvReleaseDisposition`,
  `InvPermitFirstDeadline`. (The previous invalid `initial == free + granted +
  queued` and the `granted_not_yet_committed` supply term are both deleted.)
- **Minimum state dimensions:** `available` (the stored-permit counter); the
  explicit FIFO `queue : Seq(Node)` (ordering authority, not just set
  membership); per-epoch `nodeState` (Detached/Registered/Woken/Cancelled/
  Expired); `admissionPhase`; latched admission evidence
  (`admissionSawPermit` / `admissionSawDue`) so deadline precedence is a
  prime-free state invariant; history counters (`acceptedReleaseCount`,
  `acquiredCount`); and ghost/history evidence (`lastAction`,
  `lastGrantedNode`, `expectedFIFOHead`, `pre*`) so transition properties
  (release disposition / FIFO grant / overflow non-mutation) are state
  invariants. Each modeled Node denotes ONE wait epoch (no epoch reuse).
- **Liveness:** NONE (safety-only model). The prior "Registered + Suspended +
  available>0 eventually resolves" property was removed (premise unreachable
  under `InvNoIdlePermitWithEligibleWaiter`; release is atomic external, so
  future-release fairness would be unjustified).
- **Required negative models (implemented, each fails its single named
  invariant):** `E12SemNeg1AdmissionClosure` → `InvAdmissionClosure`;
  `E12SemNeg2ReleaseLoss` → `InvPermitConservation` (grant permit lost);
  `E12SemNeg3DoubleStore` → `InvPermitConservation` (one release stores two);
  `E12SemNeg4NonFIFOGrant` → `InvFIFOGrant`; `E12SemNeg5OverflowMutation` →
  `InvOverflowNonMutation`; `E12SemNeg6IdlePermitEligibleWaiter` →
  `InvNoIdlePermitWithEligibleWaiter`; `E12SemNeg7DeadlinePrecedence` →
  `InvPermitFirstDeadline`. Real TLC results in
  [`docs/spec/e12_semaphore/README.md`](spec/e12_semaphore/README.md); gate:
  [`scripts/verify-e12-semaphore-formal.sh`](../scripts/verify-e12-semaphore-formal.sh).

### 11.3 Mutex (E12-C-PREPARATION CLOSED — CORRECTIVE-1 through CORRECTIVE-5 + reaudit PASS)

- **Correct invariant:** mutual exclusion (at most one owner); the ownership
  identity is a single Fiber; unlock-by-non-owner rejected; grant is final
  (late terminal attempts against Woken epochs cannot change owner); direct
  handoff commits owner before publication; immediate-path outcomes resolve
  terminal but do NOT publish runnable; register-recheck-suspend is a single
  atomic admission action (no interleaving window); queue contains only
  Suspended epochs. See
  [`docs/e12-async-mutex.md`](e12-async-mutex.md) §14.
- **Minimum state dimensions:** `owner ∈ Fiber ∪ {NoOwner}` (NO redundant
  `locked` bool); FIFO `queue : Seq(Epoch)` (only Registered/Suspended
  epochs); per-epoch `nodeState`
  (Detached/Registered/Woken/Cancelled/Expired); latched admission evidence
  (`admissionSawFree`/`admissionSawDue`) for deadline precedence;
  `runnablePublished`/`publicationCount` with publication only from
  UnlockHandoff/CancelSuspended/ExpireSuspended; ghost/history evidence
  (`preOwner`, `preQueue`, `preNodeState`, `prePublished`,
  `prePublicationCount`, `lastGrantedEpoch`, `EligiblePreQueue`).
  No `admissionPhase` field — the production register-recheck-suspend
  critical section is ONE atomic step in the model.
  Late cancel/expire attempts against terminal epochs are explicit non-vacuous
  actions (CancelAttemptTerminal/ExpireAttemptTerminal).
  See [`docs/e12-async-mutex.md`](e12-async-mutex.md) §14.
- **Liveness:** NONE (safety-only model, matching Semaphore precedent).
- **Required negative models (11 total):** NEG-M1 through NEG-M11 as
  specified in [`docs/e12-async-mutex.md`](e12-async-mutex.md) §16. Each
  breaks one specific rule and fails one expected named invariant. Runtime-only
  negative (Worker identity ownership failure) is handled by a deterministic
  E8 migration test.

### 11.4 Condition

- **Correct invariant:** the release-Mutex + register-Condition-WaitNode
  window is atomic w.r.t. notify (no lost notify). The return-contract cluster
  is OPEN (Model A vs Model B, §7); the invariant is stated per model:
  - Model A: `wait` returns holding Mutex in all Condition-outcome cases;
    the already-observed Condition cancellation/expiry does not re-win the
    mandatory reacquire.
  - Model B: the result model distinguishes Condition-outcome from
    reacquire-outcome and Mutex-held state; `wait` may return without Mutex.
- **Minimum state dimensions:** Mutex state (locked/owner/queue); Condition
  WaitQueue contents; two-epoch resolution (condition epoch + reacquire
  epoch); deadline dimension. (Model B additionally requires a reacquire-
  outcome dimension in the result model.)
- **Required negative model — broken release/register lost-notify window:**
  ```text
  release Mutex
  notify (Condition queue empty → lost)
  register Condition WaitNode
  suspend forever
  ```
  The correct model holds the Mutex (or equivalent) across register. (Model B
  additionally must model the reacquire-epoch cancellation race C3.)

### 11.5 Queue

- **Correct invariant:** every ItemId has exactly one control location and one
  non-empty move-only lease until released. The fixed ring owns unique leases;
  the winning resolver commits the lease transfer and QueueCompletion before
  runnable publication. There is no reusable item reference or v1 reservation
  owner.
- **Minimum state dimensions:** ring lease contents; `closed` bool; independent
  `operational/tearing_down` lifecycle; producer and consumer WaitQueues;
  per-operation completion/publication; per-node resolution;
  PREPARED/ACTIVE/RETIRED/CONSUMED timer state; control location; stable owner
  slot and ticket eligibility.
- **Not represented by this target:** rendezvous (`capacity == 0`) —
  producer/consumer pairing + direct transfer + no buffer is a separate
  protocol (§8.1, DEFERRED). The `buffer / closed / not-empty / not-full`
  dimensions do not model pairing.
- **Required negative model — broken ownership/publication:** an ItemId appears
  in ring and consumer simultaneously, a closed Queue accepts a producer
  commit, an expired waiter receives a grant, or a runnable ticket is visible
  before QueueCompletion/payload ownership is final.
- **Required stealing trace:** while W0 remains active and runs a blocker, W1
  may steal F's global-oldest Queue ticket, update the existing owner slot under
  G, and resume F. Owner activity is not an eligibility predicate.
- **Formal status:** Corrective-2 changes no TLA+ artifact; formal PASS is not
  claimed until a separate formal normalization task is performed.
- **Queue v1 cancellation:** deferred. P8/C7 and cancellation publications are
  reserved but absent; the target is 19 canonical and 6 publication
  transitions, author-verified 19/19 and 6/6; independent review remains
  required.

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
  the chosen policy, or (direct writer handoff) writer ownership committed
  after publication. Counterexample: `writer ≠ none ∧ reader_count > 0`, or a
  fairness-liveness property violated, or two Fibers believing they are
  writer.

---

## 12. Semantic decision summary

| Primitive | Verdict | Resolved items | Open human-authority / boundary items |
| --------- | ------- | -------------- | ------------------------------------- |
| **E12-A Event** | `CLOSED` (two independent corrective reviews passed) | manual-reset choice; idempotent set; reset; wait-on-set; deadline/cancel composition; set-vs-register race; reset-vs-waiter; **wake cardinality = set releases all registered waits satisfied by SET (F-EVENT-1 closed)** | ~~destruction-with-waiters~~ (resolved: caller contract violation, debug assert); ~~IMPLEMENTATION BOUNDARY: loop wake-one vs narrow wake-many seam~~ (resolved: loop wake_wait_one_locked until drained, atomic under global_mtx_) |
| **E12-B Semaphore** | `PREPARATION CLOSED — IMPLEMENTATION-1 COMPLETE — REVIEW-REQUIRED` | **policy register A1–A5 closed**; **permit conservation corrected** (`available_ + acquiredCount == initial_permits + accepted_release_count`; no `granted_in_flight`, no refund); release atomic (transfer/store/reject); FIFO + no-barging (A2); deadline precedence permit-first (A4); **Scheduler seam Conclusion A** (sufficient; `nullptr` iff empty); safety-only formal model PASS (12 invariants) + 7 negative models each CEX on expected named invariant; **production implementation COMPLETE** (public API + private Scheduler seams mirroring E12-A; TSan/ASan/UBSan clean; 31 deterministic tests + NEG compile probe) — see [`docs/e12-semaphore.md`](e12-semaphore.md) §14 As-Built | independent adversarial implementation review still required before E12-B may be declared CLOSED (not self-declared) |
| **E12-C Mutex** | `PREPARATION CLOSED — IMPLEMENTATION-1 COMPLETE — REVIEW-REQUIRED` | Fiber-identity ownership; migration-safe unlock; ownership-checked unlock; recursive FORBID; grant final vs cancel/expire; **naming = AsyncMutex** (coexists with sync Mutex); **direct handoff (M-H1)**; **FIFO no-barging (M-H2–M-H4)**; destruction = caller violation; **minimum MUTEX-HANDOFF-ONE seam specified**; **collapsed atomic admission actions**; **formal model complete with 14 invariants + 11 negative models** (§14–§16 of e12-async-mutex.md) | independent implementation review remains required |
| **E12-D Condition** | `RUNTIME SUITE PASS — T25 CLOSED` | build PASS; release/register atomic window; FIFO notify-one; no-E13-dependence; no spurious wake | `E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1: PASS` — closed by W1 corrective `db656b5` (test-harness defect; no production change); Clang Debug/ASan/TSan full suite green |
| **E12-E Queue** | `CORRECTIVE-2 INDEPENDENT REVIEW PASS — IMPLEMENTATION DENIED — B4 OPEN` | one-shot unforgeable `QueueItemLease`; unique lease ring; exact opaque failed-lease return; no reservation owner; expiry retained; cancellation deferred; concrete PREPARED guard; own-oldest/global-oldest active-victim stealing; stable no-erase owner slot; irreversible teardown session; narrow `active_port_calls_`; author-verified 19/19 canonical and 6/6 publication targets | `ASYNC-MUTEX-NOTHROW` B1 PASS (implemented + independent review); Condition T25 B3 PASS (W1 corrective); **B2 Corrective-2 independent adversarial review PASS** (11/11 topics, 33/33 counterexamples, 6 compile probes, 0 blocking findings); **B4 Queue formal model OPEN**; `E12-E IMPLEMENTATION AUTHORIZATION: DENIED — B4 OPEN` |
| **E12-F RwLock** | `HUMAN-DECISION-REQUIRED` | upgrade/downgrade DEFER; recursive read/write FORBID | fairness policy (reader-pref/writer-pref/phase/FIFO); **writer winner-before-publication seam (§14)** |
| **E12-G Audit** | `DEFERRED` (runs after A–F) | uses the §10 matrix as baseline | — |

---

## 13. Implementation stop conditions

E12 implementation (any subphase) MUST stop and request human authority when:

1. A primitive needs a wake cardinality the closed substrate does not
   natively offer (e.g. Condition notify-all, or Event set-releases-all if
   realized without a loop) — the **mechanism** choice (loop vs native
   multi-wake) must not silently become a multi-wait/Select-like feature
   (Hard Rule 6). (Note: Event's *public semantic* — set releases all
   registered waits — is RESOLVED (§4.4); only the *mechanism* is an
   implementation boundary. Condition notify-all additionally has an open
   *whether-first-scope-ships-it* decision.)
2. A primitive's cancellation/deadline cleanup would require a
   primitive-local timeout protocol or primitive-local cancellation winner
   (Hard Rule 3) — it must instead refine through `resolve_`.
3. A primitive's ownership identity cannot be expressed as Fiber identity
   stable across E8 migration (Mutex/RwLock writer).
4. The formal model (per §11) would omit a load-bearing dimension (M2) —
   specifically the resource/accounting/ownership dimension the primitive
   introduces (e.g. queued demand for Semaphore, Queue ring/operation payload
   ownership plus publication state, reacquire-outcome for Condition Model B).
5. The grant-vs-cancel/expire race (§5.2, generalized) cannot be closed on
   the single `resolve_` CAS.
6. A grant-bearing primitive needs to commit primitive resource state
   (ownership / item / slot / writer identity) for the exact CAS winner
   BEFORE runnable publication, and the current `wake_wait_one` caller seam
   does not expose the winner (§14). Stop and resolve the
   winner-before-publication commit seam before implementing that
   primitive's grant path. (Note: E12-C Mutex seam is RESOLVED —
   `MUTEX-HANDOFF-ONE` specified in
   [`docs/e12-async-mutex.md`](e12-async-mutex.md) §9.)
   primitive's grant path.
7. A Queue is implemented without an operation-owned payload handle and the
   one-shot move-only lease transfer `detached -> producer operation -> ring`
   or `ring -> consumer operation` before runnable publication (§8.2). The
   `resolve_` CAS identifies the winner but does not transfer ownership.
8. A primitive's internal structural lock is held across Fiber suspension /
   `context_switch` / async Mutex acquire (§8.4), or a Queue is built on the
   E12-C async Mutex for internal structural state.
9. A bounded Queue is specified with `capacity == 0` as if it were a derived
   case (§8.1) — rendezvous is a separate DEFERRED protocol.
10. Queue implementation starts while the no-throw Mutex substrate remains
    unauthorized, Corrective-2 lacks independent adversarial acceptance, or
    the separate Condition T25 migration/reacquire hang remains unresolved.
    (Current gate status: B1 PASS, B2 PASS, B3 PASS — these three no longer
    trigger this stop condition. B4 Queue formal model remains OPEN and still
    triggers it.)

---

## 14. Primitive Grant Commit Insertion Boundary (F-GRANT-1)

This is a normative preparation section. It documents the as-built Scheduler
resolution seam, the required authority ordering when a primitive commits
resource state to a CAS winner, and the per-primitive classification. It does
NOT implement anything and does NOT prescribe a generic callback API.

### 14.1 The closed Scheduler resolution seam (as-built)

Reconstructed from the closed E10/E11 substrate (verified against current
production code):

```text
Scheduler::wake_wait_one(WaitQueue& q)   [returns bool]
    ->
global_mtx_
    ->
q.mtx()
    ->
WaitQueue::wake_one_locked()             [PRIVATE; friend Scheduler only]
    ->
WaitNode::resolve_(Woken) CAS winner     [the single winner authority]
    ->
winning WaitNode* available INSIDE Scheduler (before publication)
    ->
winner-only unlink_locked (same critical section as the CAS)
    ->
retire_timer_for_node_locked (E11 timer retirement)
    ->
waiting_waitq_count_ closure decrement
    ->
Fiber::make_runnable (exactly-once ticket)
    ->
route_runnable_locked
    ->
return bool to primitive caller
```

Record:

```text
Scheduler::wake_wait_one public/private callable seam returns bool.

The primitive caller does NOT receive the winning WaitNode* or Fiber*.

WaitQueue structural resolvers (wake_one_locked / cancel_locked /
expire_locked / unlink_locked / register_wait_locked / mtx()) are PRIVATE
and Scheduler-owned (friend Scheduler only; negative tests e10_nc1 /
e10_nc3 enforce non-escape).

No wake_many / wake_wait_all / wake_wait_many operation exists in the
codebase (single-winner at every layer).
```

Therefore current primitive code cannot generally execute:

```text
CAS winner
    ->
primitive grant commit using exact winner identity
    ->
runnable publication
```

through the current `wake_wait_one` caller boundary, because the winner
identity is consumed inside the Scheduler and never handed back.

### 14.2 Required authority ordering

For a grant-bearing primitive, distinguish these ordered moments:

```text
RESOURCE AVAILABLE

WaitNode RESOURCE_WAKE CAS WINS     (WaitNode::resolve_ — sole winner authority)

PRIMITIVE GRANT / OWNERSHIP COMMITTED   (primitive-specific state for the winner)

RUNNABLE TICKET PUBLISHED            (Fiber::make_runnable)

FIBER RESUMES
```

The required ordering when grant identity matters:

```text
resolve_(Woken) winner
    ->
primitive commit for exact winner
    ->
runnable publication
```

The primitive grant commit is **NOT** a second wait winner authority.
`WaitNode::resolve_` remains the sole winner (Hard Rule 3). The commit
*consumes* the already-decided winner identity; it does not re-decide it.

### 14.3 Per-primitive classification

#### 14.3.1 Event

```text
CURRENT SCHEDULER SEAM SUFFICIENT
```

Reason: no resource grant, no ownership, no payload reservation. Event `set_`
is published (flipped to SET) before per-waiter RESOURCE_WAKE attempts; each
waiter is resolved by its own `resolve_`, and no per-winner primitive state
must be committed between the CAS and publication. (The set-releases-all
*mechanism* — loop `wake_wait_one` vs a narrow wake-many — is the
IMPLEMENTATION BOUNDARY of §4.4, not a grant-commit boundary.)

```text
E12-A EVENT BLOCKED BY GRANT SEAM: NO
```

#### 14.3.2 Semaphore (E12-B-PREPARATION-CORRECTIVE-1)

For first-scope acquire-one/release-one, release creates one **pending permit**
that is disposed of atomically (no `free_permits -> granted_not_yet_committed`
pre-reservation, no refund):

```text
release():
    one pending permit is created
    -> if an eligible FIFO waiter exists: transfer the pending permit directly
       to that waiter (available_ unchanged; accepted_release_count++,
       acquiredCount++)
    -> else if available_ < max_permits: store it (available_++;
       accepted_release_count++)
    -> else (available_ == max_permits): reject (no mutation)
```

Under **Conclusion A** (§5.2): the wake/cancel/expire resolvers are all
serialized by `global_mtx_`, and a winning cancel/expire unlinks the node
before releasing the mutex; therefore a linked FIFO head observed by
`wake_wait_one_locked` under the lock is necessarily Registered and its
`resolve_(Woken)` cannot lose. The seam is sufficient; no rollback/refund
path exists (no permit was ever pre-decremented).

```text
CURRENT SCHEDULER SEAM SUFFICIENT (first-scope; Conclusion A)
```

Do NOT claim this for `acquire(N)` (deferred) — multi-permit atomic grant may
require winner-aware accounting and is out of first scope.

#### 14.3.3 Mutex (E12-C-PREPARATION-CORRECTIVE-1 through CORRECTIVE-5)

For direct handoff (M-H1):

```text
winner W resolve_(Woken)
    ->
owner = W.fiber
    ->
publish W
```

Current `wake_wait_one` caller seam does NOT expose W before publication.
The direct handoff policy is now CLOSED (M-H1). The minimum private seam
`MUTEX-HANDOFF-ONE` is specified in
[`docs/e12-async-mutex.md`](e12-async-mutex.md) §10. Classify:

```text
MINIMUM MUTEX-SPECIFIC PRIVATE SEAM REQUIRED (MUTEX-HANDOFF-ONE)
```

The seam is private, Mutex-specific, Scheduler-owned, and non-generic. The
conditional dependency on unresolved handoff policy is removed.

```text
E12-C MUTEX BLOCKED BY GRANT SEAM:
    NO — seam is specified; implementation is ready
```

#### 14.3.4 Queue

The `resolve_(Woken)` CAS identifies the winning wait epoch; it does not by
itself move a payload. Queue v1 therefore uses a Queue-specific Scheduler seam
that receives the exact sealed operation object and, while holding
`global_mtx_ -> state_mtx_ -> one role WaitQueue mutex`, performs:

```text
winner CAS
-> unlink
-> timer close
-> producer operation -> ring OR ring -> consumer operation
-> QueueCompletion write
-> embedded runnable-ticket publication
```

The operation's one-shot `QueueItemLease` is final ownership state, not an
independent reservation or refundable permit. Producer grants always enter the
ring; no direct producer-to-consumer handoff exists. The fixed non-template
QueuePort is the only Scheduler friend, and the full private seam is specified
by `docs/e12-queue-scheduler-integration.md` Corrective-2. Ticket publication
does not choose a Worker; consumption chooses own-oldest or global-oldest and
allows stealing from an active admission owner.

```text
QUEUE-SPECIFIC WINNER-BEFORE-PUBLICATION SEAM REQUIRED AND SPECIFIED
REUSABLE ITEM REFERENCE: FORBIDDEN
EXPLICIT SLOT RESERVATION/PERMIT: QUEUE V2 CANDIDATE ONLY
```

#### 14.3.5 RwLock

Writer handoff:

```text
winner W
    ->
writer owner = W.fiber
    ->
publish
```

requires exact winner identity. Classify:

```text
WRITER:
    REQUIRES WINNER-BEFORE-PUBLICATION COMMIT SEAM
```

Reader grant may pre-increment anonymous reader accounting if rollback is safe
(a losing reader grant refunds the increment); winner identity is not required
for the count itself. Reader and writer are classified separately (§9.3).

### 14.4 Narrow prerequisite boundary

The preparation document identifies a future narrow Scheduler capability
conceptually equivalent to:

```text
resolve one RESOURCE_WAKE winner
    ->
execute primitive-specific winner commit
       while Scheduler serialization and queue serialization remain held
    ->
perform normal E10/E11 cleanup
    ->
publish runnable
```

Do NOT prescribe a generic callback API unless code evidence makes that exact
mechanism necessary (Hard Rule 8). Do NOT create `UniversalGrant`,
`ResourceGrantFramework`, `Waitable`, or any generic synchronization base
class (Hard Rule 5). The preparation conclusion identifies only the required
authority ordering (§14.2), not a concrete API.

### 14.5 Phase blocking summary

```text
E12-A EVENT BLOCKED BY GRANT SEAM:
    NO

E12-B SEMAPHORE BLOCKED BY GRANT SEAM (first-scope anonymous):
    NO

E12-C MUTEX BLOCKED BY GRANT SEAM:
    NO — minimum MUTEX-HANDOFF-ONE seam specified
    (E12-C-PREPARATION CLOSED, CORRECTIVE-1 through CORRECTIVE-5 + reaudit PASS)

E12-D CONDITION:
    no grant state of its own (delegates to Mutex); not independently blocked.

E12-E QUEUE:
    requires winner-before-publication lease transfer and completion;
    Corrective-2 author self-assessment PASS; independent review required;
    implementation is denied.

E12-F RWLOCK:
    writer requires winner-before-publication seam; reader likely does not.
```

Earliest phase whose **selected** semantics **definitely** require the new
winner-before-publication seam:

```text
E12-C Mutex (MUTEX-HANDOFF-ONE — PREPARATION CLOSED, CORRECTIVE-1 through CORRECTIVE-5 + reaudit PASS)
    followed by E12-E Queue (one-shot lease ownership transfer)
```

---

## 15. Cross-links

- E10 as-built (authoritative wait protocol):
  [`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)
- E11 spec (CLOSED, authoritative deadline/timer):
  [`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md)
- E11 insertion audit (style precedent for this document):
  [`docs/e11-arch-recon-audit.md`](e11-arch-recon-audit.md)
- E12-A Event as-built (CLOSED):
  [`docs/e12-event.md`](e12-event.md)
- E12-B Semaphore (REVIEW-REQUIRED):
  [`docs/e12-semaphore.md`](e12-semaphore.md)
- E12-C Async Mutex preparation corrective:
  [`docs/e12-async-mutex.md`](e12-async-mutex.md)
- Construction method (M1–M9, binding):
  [`docs/async-runtime-construction-method.md`](async-runtime-construction-method.md)
- Roadmap (E12 placement; updated in the same commit):
  [`docs/async-runtime-plan.md`](async-runtime-plan.md)
- Formal model directories: `docs/spec/e10_waitnode/`, `docs/spec/e11_timer_wait/`
