# E12-B — Async Semaphore (Preparation Corrective-1)

> Status:
> ```text
> E12-B-PREPARATION-CORRECTIVE-1: COMPLETE
> E12-B-PREPARATION: REAUDIT-REQUIRED
> E12-B-IMPLEMENTATION: BLOCKED
> ```
>
> This corrective closes the E12-B Semaphore preparation authority's two open
> defect classes: (1) the five human policy decisions (A1–A5) and (2) the
> permit-accounting contradictions. It does **not** mark preparation CLOSED. Only
> a fresh adversarial re-audit returning PASS may change the status to
> `E12-B-PREPARATION: CLOSED` / `E12-B-IMPLEMENTATION: READY`.
>
> Authority baseline: E10 CLOSED
> ([`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)); E11 CLOSED
> at `7715808` ([`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md));
> E12-A Event CLOSED ([`docs/e12-event.md`](e12-event.md)). This document does
> NOT reopen E10/E11/E12-A; it builds on them as authoritative.
>
> Cross-primitive preparation:
> [`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md) §5 (updated
> in this corrective).
>
> Formal model: [`docs/spec/e12_semaphore/`](spec/e12_semaphore/). Formal gate
> (safety-only): [`scripts/verify-e12-semaphore-formal.sh`](../scripts/verify-e12-semaphore-formal.sh).
>
> Scope of this corrective: **documentation, design-authority, and formal-model
> only.** No production Semaphore code, no production tests, no new public API,
> no generic grant framework, no public WaitQueue access, no Scheduler refactor,
> no Scheduler seam extension. Production implementation remains BLOCKED.

---

## 1. Mission

Produce a mechanically coherent, implementation-ready E12-B Semaphore
preparation contract that closes, for first-scope acquire-one / release-one:

1. the five policy decisions (A1–A5); and
2. the permit-accounting contradictions in the prior preparation draft.

The prior draft's defects, named for traceability:

- **F-SEM-ACCT-1**: the supply-conservation law counted queued demand on the
  supply side, and the release model pre-decremented `available_` then
  "refunded" on a lost CAS — an `available_--` / refund model that is
  mechanically wrong (it underflows at `available_ == 0` and double-counts).
- **F-SEM-SEAM-1**: the FIFO-progress claim relied on a release-side loop
  skipping a failed head, but the production loop terminates on `nullptr` —
  so the proof was unjustified until the lock/unlink ordering was audited
  (closed as **Conclusion A** below).
- **F-SEM-FORMAL-1**: several "invariants" were written as transition
  predicates (using primed variables or asserting single-action effects) that
  TLC cannot check as state invariants, and a liveness property whose premise
  is unreachable under the stable-state invariant (vacuous).

This corrective does not introduce a generic grant framework, a `Waitable`
base class, public `WaitQueue` access, or a broad Scheduler refactor (Hard
Rules 5/6).

---

## 2. Policy decision register (A1–A5)

These are accepted by this corrective and recorded verbatim. They are the
authoritative first-scope Semaphore contract.

### A1. Capacity and overflow

```text
max_permits is mandatory
max_permits > 0
initial_permits <= max_permits
```

Constructor violations are **caller contract violations**, checked by debug
assertions. No exception or recoverable constructor-error API is introduced.

`release()` returns `false` with **no mutation** only when both hold:

```text
no eligible queued waiter accepts the released permit
AND
available_ == max_permits
```

### A2. Barging

Barging is **forbidden**. A newly arriving acquire may not bypass an
already-eligible queued waiter.

`try_acquire()` succeeds only when:

```text
at least one stored permit exists (available_ > 0)
AND
no eligible queued waiter has FIFO priority
```

### A3. Destruction

Destroying a Semaphore with registered wait epochs is a **caller contract
violation**. The destructor does **not** cancel, wake, expire, or synthesize
successful acquisition. Outstanding permits previously acquired by clients are
**not** runtime-tracked and do not prevent destruction.

### A4. Deadline precedence

Resource admission has precedence over an already-due deadline. For
`acquire_until`:

```text
immediately admissible permit (available_ > 0 AND no eligible queued waiter)
    -> Woken

no immediately admissible permit AND deadline already due
    -> Expired

otherwise
    register the timed wait and enter the normal wake/cancel/expire race
```

`try_acquire()` is **not** a zero-duration timed acquire. It has no deadline
semantics.

### A5. `available()` semantics

`available()` returns an **observational snapshot** of stored, unassigned
permits. It is **not** an admission guarantee:

```text
the value may become stale immediately because of concurrency
available() > 0 does NOT guarantee that a later try_acquire() succeeds
```

`available()` does not include permits already granted but not committed; the
first-scope production state has no such visible state.

---

## 3. Corrected permit conservation law

### 3.1 The wrong model — DELETED

Every statement modelling release grant by first executing `available_--`, then
granting on CAS-win or refunding on CAS-loss, is deleted. A failed grant
attempt does **not** "refund a permit previously removed from `available_`";
the release operation creates one *pending permit* that is either transferred,
stored, or rejected, never pre-decremented and never refunded.

The prior draft's `granted_not_yet_committed` / `granted_in_flight` supply
term is also removed: first-scope Semaphore has no such production state.

### 3.2 The accepted law

```text
available + acquiredCount == initialPermits + acceptedReleaseCount
```

where:

```text
available            stored, unassigned permits (the atomic counter)
acquiredCount        total permits acquired (immediate acquire OR queued grant)
initialPermits       permits created at construction
acceptedReleaseCount total release() calls that successfully contributed a permit
                     (transfer to a waiter, or store into available_)
```

A rejected overflow release is **not** an accepted release: it increments no
counter. Queued demand is **not** a term in this law (queued demand is a
separate dimension; see §3.4).

### 3.3 The four counter transitions

```text
Immediate acquire (available_ > 0, no eligible queued waiter):
    available--
    acquiredCount++

Release transfer to an eligible queued waiter:
    acceptedReleaseCount++
    acquiredCount++
    available unchanged

Release store (no eligible waiter, available_ < max_permits):
    acceptedReleaseCount++
    available++

Overflow rejection (no eligible waiter, available_ == max_permits):
    all counters unchanged
```

### 3.4 Queued demand — separate dimension

```text
queuedDemand == number of live Semaphore wait epochs that:
    still demand one permit
    AND have not won RESOURCE_WAKE grant
    AND have not won TIMER_EXPIRE
    AND have not won CANCEL
```

`QUEUED` and granted are disjoint waiter states. A grant winner is no longer
queued demand. Queued demand does **not** appear on the supply side and
creates/consumes/refunds no supply.

---

## 4. Exact release state machine

```text
release():
    one pending permit is created by this release call

    if an eligible queued waiter wins (FIFO head; see §5):
        transfer that pending permit directly to exactly one waiter
        available_ unchanged
        return true

    if no eligible queued waiter wins:
        if available_ == max_permits:
            return false  with no state mutation
        otherwise:
            available_++
            return true
```

A queued grant therefore works when `available_ == 0`: there is no decrement
and no unsigned underflow. The pending permit is transferred, not withdrawn
and re-deposited.

---

## 5. Scheduler seam evidence — Conclusion A (loser path unreachable)

### 5.1 The decision

The prior preparation claimed that a single `release()` could skip a failed
FIFO head and grant the next eligible waiter, citing the Event drain loop:

```cpp
while (wake_wait_one_locked(q) != nullptr) { }
```

This is **not** a proof of release-side skip-after-null: the loop terminates on
`nullptr`. Whether a release can grant W2 after W1 "loses" depends entirely on
whether a linked FIFO head can ever lose its `resolve_(Woken)` CAS while
`release` observes it.

An exhaustive call-site audit of every private `WaitQueue` method (all live in
exactly one production TU, `src/async/scheduler.cpp`; `Scheduler` is the sole
friend at `include/sluice/async/wait_queue.hpp:145`) proves **Conclusion A**:

```text
wake, cancel, and expire resolution are all serialized by the SAME
    Scheduler::global_mtx_;
a successful cancel or expire unlinks the node in the SAME critical section as
    its winning resolve_ CAS, before releasing global_mtx_;
a linked FIFO head observed by wake_wait_one_locked while holding global_mtx_
    AND q.mtx() is therefore necessarily Registered and eligible;
its resolve_(Woken) CAS cannot lose.
```

### 5.2 Decisive evidence (every resolver holds `global_mtx_`)

| Resolver (winning CAS site) | file:line | `global_mtx_` acquired |
| --------------------------- | --------- | ---------------------- |
| `wake_wait_one_locked` (REQUIRES) | `scheduler.cpp:1076`; `scheduler.hpp:476` | precondition; `wake_wait_one` `:1116`, `event_set_broadcast` `:1319` |
| `cancel_wait` | `scheduler.cpp:1129` | `LockGuard lk(global_mtx_);` `:1129` |
| `event_cancel_wait` | `scheduler.cpp:1370` | `LockGuard lk(global_mtx_);` `:1370` |
| `expire_wait` | `scheduler.cpp:1285` | `LockGuard lk(global_mtx_);` `:1285` |
| `await_wait_deadline` I5 admission expiry | `scheduler.cpp:1211` | `LockGuard lk(global_mtx_);` `:1211` |
| `await_event_wait[_deadline]` wake_node | `scheduler.cpp:1419 / :1491` | `LockGuard lk(global_mtx_);` `:1419 / :1491` |
| `pump_deadlines_locked` (REQUIRES) | `scheduler.cpp:1556`; `scheduler.hpp:707` | all 5 callers `:557 / :615 / :665 / :698 / :1176` |

Every winning `unlink_locked` runs in the same critical section as its winning
`resolve_` (`wait_queue.hpp:203 / :229 / :247 / :268`). The four defense-in-depth
"terminal recheck undo" sites (`scheduler.cpp:1064 / :1256 / :1458 / :1540`)
also hold both `global_mtx_` and `q.mtx()` and unlink a node the enclosing
admission path itself just registered (unreachable in production since no
other resolver can have won in the same critical section). No resolver path
and no unlink site lacks `global_mtx_`.

### 5.3 Consequences recorded under Conclusion A

```text
Under the production lock protocol, wake_wait_one_locked returns nullptr
ONLY when the queue is empty.
```

(Stated in this clean form: Conclusion A proves a terminal linked node is not
observed by a live release, so "or every remaining node is terminal awaiting
cleanup" is unnecessary and is not stated.)

- W1-cancelled / W2-live behavior is proven by **cancellation unlinking W1
  before release observes the queue**: cancel is `global_mtx_`-serialized and
  unlinks W1 in the same critical section; release acquires `global_mtx_` next,
  observes W1 already gone, and grants W2. This is NOT a release-side
  skip-after-null.
- Removed from production design, runtime scenarios, and formal model: any
  failed-head grant, failed-head refund, failed-head continue-to-W2 action, and
  any scenario whose precondition is "a linked eligible FIFO head loses its
  `resolve_(Woken)` CAS."
- The existing private seam is **sufficient**. No minimum narrow Scheduler
  extension is introduced (Conclusion B does not apply).

### 5.4 FIFO progress and the stable-state invariant

The accepted Semaphore behavior is:

```text
One release permit resolves at most one eligible waiter Woken, selecting the
FIFO head of the eligible set.
```

No successful release may return while both conditions remain true:

```text
an eligible queued waiter exists
AND
an unassigned stored permit exists
```

The stable-state invariant:

```text
EligibleQueuedWaiterExists => available_ == 0
```

Physical terminal nodes awaiting cleanup do not count as eligible waiters, and
under Conclusion A a live release does not observe them linked.

> **Reachability note (the `available_ == max_permits ∧ eligible waiter`
> case).** Because `max_permits > 0` and `EligibleQueuedWaiterExists ⇒
> available_ == 0`, the state `available_ == max_permits ∧ eligible waiter
> exists` is **not a reachable production state**. It is NOT reframed as a
> "transfer at max" release branch. It survives only as a formal negative
> model (`E12SemNeg6IdlePermitEligibleWaiter`, §10) that forces the
> `InvNoIdlePermitWithEligibleWaiter` violation. An invariant violation and an
> ordinary release branch cannot be the same state.

---

## 6. State representation

```cpp
using permit_count_t = std::uint32_t;
std::atomic<permit_count_t> available_;
```

`available()` (lock-free observation) reads `available_.load(acquire)`.
Authoritative admission, release, queue-priority, and mutation decisions are
serialized under `Scheduler::global_mtx_` and the Semaphore's queue mutex —
exactly the E12-A Event / E10 / E11 coordination domain. Atomicity supports
**observation**; it does **not** replace Scheduler locking.

The contradictory prior description that simultaneously called `available_` a
plain mutex-guarded integer and invoked `.load()` on it is removed.

---

## 7. Integer bounds

```cpp
using permit_count_t = std::uint32_t;
```

```text
0 <= initial_permits <= max_permits <= UINT32_MAX
max_permits > 0
```

No undocumented `< 2^31` limit is claimed. Machine-overflow is prevented by:

1. constructor preconditions (`0 ≤ initial ≤ max`, `max > 0`, debug-asserted);
2. the equality check before increment (`available_ < max_permits` guards
   `available_++`);
3. no decrement from zero (queued grant transfers the pending permit without
   touching `available_`; immediate acquire is guarded by `available_ > 0`);
4. each accepted release adds at most one stored permit (and only when
   `available_ < max_permits`).

---

## 8. Formal-invariant catalog (safety only)

The formal model (`docs/spec/e12_semaphore/`) is **safety-only**. The
liveness gate from the prior draft is removed: its premise (`Registered +
Suspended + available > 0`) is unreachable under the stable-state invariant
(would be vacuous), and release is modeled as an atomic external action with
no pending-release state (external-future-release fairness would be
unjustified). Genuine liveness would require explicit multi-step pending
state (`releaseRequested` / `wakePublicationPending` /
`schedulerDispatchPending`); that is deferred.

The named invariants (each a pure state predicate — no primed variable):

| Invariant | Meaning |
| --------- | ------- |
| `InvPermitBounds` | `0 ≤ available ≤ max_permits`; `max_permits > 0`; `0 ≤ initial ≤ max` |
| `InvPermitConservation` | `available + acquiredCount == initialPermits + acceptedReleaseCount` (the §3.2 law); distinguishes accepted releases / overflow rejections / immediate acquires / queued grants |
| `InvQueueWellFormed` | `queue` has no duplicate Node; every queued Node is `Registered` in an admissible admission phase; no `Woken`/`Cancelled`/`Expired`/`Detached` node is in `queue` |
| `InvSingleResolution` | each wait epoch resolves at most once (inherited E10; `resolvedCount ≤ 1`) |
| `InvSinglePublication` | one winning epoch → at most one runnable publication (`wakeDispatched == Σ resolvedCount`) |
| `InvGrantCommitCoupling` | `acquiredCount == Cardinality({n : nodeState[n] = "Woken"})` — holds because each modeled Node denotes **one wait epoch**, not a reusable production `WaitNode` across multiple epochs |
| `InvFIFOGrant` | the last release transfer granted the FIFO head of the eligible set (history-backed: `lastAction = "ReleaseTransfer" => lastGrantedNode = expectedFIFOHead`) |
| `InvAdmissionClosure` | a wait admitted while `available > 0` and no eligible waiter is queued resolves `Woken` inline (not left `Registered`/`Suspended`) |
| `InvOverflowNonMutation` | the last overflow release changed no authoritative state (history-backed: `lastAction = "ReleaseOverflow" => available = preAvailable /\ acceptedReleaseCount = preAcceptedReleaseCount /\ acquiredCount = preAcquiredCount`) |
| `InvNoIdlePermitWithEligibleWaiter` | no state has `available > 0 ∧ eligible queued waiter exists` (the stable-state invariant) |
| `InvReleaseDisposition` | every accepted release did exactly one of: transfer (`acceptedReleaseCount+1, acquiredCount+1, available unchanged`) or store (`acceptedReleaseCount+1, available+1`) (history-backed) |
| `InvPermitFirstDeadline` | a wait admitted while `available > 0` with a due deadline resolved `Woken`, not `Expired` (admission-evidence-backed: `admissionSawPermit[n] /\ admissionSawDue[n] => nodeState[n] = "Woken"`) |

### 8.1 Transition properties as state invariants (history evidence)

`InvFIFOGrant`, `InvOverflowNonMutation`, and `InvReleaseDisposition` are
properties of a single action's before/after state. To make them TLC-checkable
state invariants, the model carries minimal **ghost/history evidence** written
by every action:

```text
lastAction in {"Init","Acquire","ReleaseTransfer","ReleaseStore",
               "ReleaseOverflow","Cancel","Expire"}
lastGrantedNode      the node granted by the last ReleaseTransfer (or NoNode)
expectedFIFOHead     the eligible FIFO head latched BEFORE queue mutation (or NoNode)
preAvailable         available immediately before the last action
preAcceptedReleaseCount
preAcquiredCount
```

These are **refinement variables only**, not production fields; they exist
solely so transition properties are state predicates.

### 8.2 Admission precedence as a state invariant (latched evidence)

`InvPermitFirstDeadline` cannot reference a primed next-state variable. The
model latches admission evidence atomically with the resolution:

```text
admissionSawPermit : Node -> BOOLEAN
admissionSawDue    : Node -> BOOLEAN
```

`AcquireUntilImmediate(n)` writes `admissionSawPermit'[n] = TRUE`,
`admissionSawDue'[n] = deadlineDue[n]`, and resolves `Woken` in one atomic
step. The invariant is then the pure state predicate
`admissionSawPermit[n] /\ admissionSawDue[n] => nodeState[n] = "Woken"`.

### 8.3 Overflow "no mutation" — precise wording

`ReleaseOverflow` updates only ghost/history evidence (`lastAction`,
`preAvailable`, `preAcceptedReleaseCount`, `preAcquiredCount`). The accepted
wording is therefore:

```text
No authoritative Semaphore state, permit counter, queue, node outcome, or
accounting counter is mutated.

Formal ghost/history evidence may be updated solely to verify the
non-mutation property.
```

This avoids the contradiction of "proving non-mutation by forbidding all
writes including the history fields that record the non-mutation."

---

## 9. Runtime test plan (design only — no test files produced)

Reachable, Conclusion-A-consistent scenarios:

1. `available_ == 0`, one waiter, one `release()` → waiter `Woken` without
   unsigned underflow.
2. No eligible waiter → the release permit becomes stored (`available_++`).
3. No persistent `available_ > 0` with an eligible waiter (stable-state
   invariant).
4. Overflow only when **no eligible waiter accepts AND `available_ == max`**
   (the A1 `release()`-returns-false contract).
5. No-barging with real FIFO priority (`try_acquire()` fails while an eligible
   waiter is queued).
6. Cancel/grant and timer/grant accounting closure (the cancel/expire resolver
   is `global_mtx_`-serialized; a release arriving after observes the waiter
   already unlinked).
7. Constructor precondition assertions (`max > 0`, `initial ≤ max`).
8. `available()` snapshot is observational, not an admission promise.
9. Permit-first deadline precedence: `available > 0` and deadline already due
   → `acquire_until` returns `Woken`, not `Expired` (runtime mirror of
   `InvPermitFirstDeadline`).

**Removed entirely** (per Conclusion A and the reachability note):

- the `available_ == max_permits ∧ eligible waiter` scenario (not reachable;
  covered only by NEG-6);
- any failed-head skip/continue/refund runtime scenario;
- any scenario whose precondition is a linked eligible head losing its
  `resolve_(Woken)` CAS;
- any test expectation that W2 requires a second release after W1 loses.

---

## 10. Negative-model matrix

Seven negative models; each differs from the correct model at exactly one
rule and fails its **single** expected named invariant for the intended
reason. None assumes a linked eligible FIFO head can lose `resolve_(Woken)`.

| NEG | Broken rule | Counterexample shape | Expected named invariant |
| --- | ----------- | -------------------- | ------------------------ |
| NEG-1 `AdmissionClosure` | a waiter admitted while `available > 0` is left `Registered`/`Suspended` | `Registered + Suspended + available > 0 + no eligible waiter` | `InvAdmissionClosure` |
| NEG-2 `ReleaseLoss` | a release permit is neither transferred nor stored (lost) | `acceptedReleaseCount` grew but neither `acquiredCount` nor `available` did | `InvPermitConservation` |
| NEG-3 `DoubleStore` | one accepted release stores two permits | `available` grew by 2 while `acceptedReleaseCount` grew by 1 | `InvPermitConservation` |
| NEG-4 `NonFIFOGrant` | W2 granted before an eligible W1 | `lastGrantedNode # expectedFIFOHead` | `InvFIFOGrant` |
| NEG-5 `OverflowMutation` | release at `available == max` mutates `available` | `available = preAvailable + 1` under `ReleaseOverflow` | `InvOverflowNonMutation` |
| NEG-6 `IdlePermitEligibleWaiter` | `available > 0` while an eligible waiter remains queued | the moved-out runtime case | `InvNoIdlePermitWithEligibleWaiter` |
| NEG-7 `DeadlinePrecedence` | `available > 0 ∧ deadlineDue ∧ acquire_until` resolves `Expired` | latched `admissionSawPermit ∧ admissionSawDue` but `nodeState = Expired` | `InvPermitFirstDeadline` |

NEG-3 is scoped to a **single** defect (one accepted release stores two
permits) and expects **only** `InvPermitConservation`. A separate
double-publication negative is not added in this corrective.

---

## 11. Deferrals / non-goals

- `acquire(N)` / `release(N)` (multi-permit atomic grant) — deferred.
- Generic grant framework, `Waitable` base class, public `WaitQueue` access,
  broad Scheduler refactor — forbidden (Hard Rules 5/6).
- Scheduler seam extension — not introduced; Conclusion A makes it unnecessary.
- Multi-stage pending-release state and any associated liveness — deferred.
- Capacity-zero / rendezvous — belongs to E12-E Queue.
- E12-C..G work — out of scope.

---

## 12. Required report (H1–H11)

1. **Corrected verdict:** `E12-B-PREPARATION-CORRECTIVE-1: COMPLETE —
   REAUDIT-REQUIRED — IMPLEMENTATION BLOCKED`.
2. **Changed authority documents:** `docs/e12-semaphore.md` (this file, new);
   `docs/spec/e12_semaphore/` (new); `scripts/verify-e12-semaphore-formal.sh`
   (new); `docs/e12-sync-primitives-plan.md` §5.2/§5.3/§5.5/§11.2/§14.3.2/§12
   (edited — deleted pre-reservation/refund model, fixed conservation law,
   updated status + pointer).
3. **Accepted policy decision register:** A1–A5 (§2).
4. **Corrected permit conservation law:** §3.2
   (`available + acquiredCount == initialPermits + acceptedReleaseCount`); the
   `available_--`/refund and `granted_in_flight` models are deleted.
5. **Exact release state machine:** §4.
6. **Exact `wake_wait_one_locked` evidence:** §5 (Conclusion A; call-site
   table; clean `nullptr` iff empty statement; W1-cancelled/W2-live via
   pre-release unlinking).
7. **Stable-state invariants:** `EligibleQueuedWaiterExists ⇒ available_ == 0`
   (§5.4); `InvQueueWellFormed`, `InvNoIdlePermitWithEligibleWaiter` (§8).
8. **Revised runtime tests:** §9 (reduced/corrected; the max+eligible case
   removed and covered only by NEG-6).
9. **Revised formal invariants and liveness:** §8 (safety-only; the twelve
   state invariants; liveness gate deleted with stated reason).
10. **Remaining unresolved decisions:** none among A1–A5 + accounting + seam.
    The Scheduler-seam question is resolved Conclusion A. Preparation is
    REAUDIT-REQUIRED by independent authority.
11. **Implementation-readiness verdict:** **NOT READY.** Production
    implementation is BLOCKED. Only a fresh adversarial re-audit returning PASS
    may change the status to `E12-B-PREPARATION: CLOSED` /
    `E12-B-IMPLEMENTATION: READY`.

---

## 13. Cross-links

- E10 as-built (authoritative wait protocol):
  [`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)
- E11 spec (CLOSED, authoritative deadline/timer):
  [`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md)
- E12-A Event as-built (CLOSED):
  [`docs/e12-event.md`](e12-event.md)
- Cross-primitive preparation:
  [`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md)
- Formal model: [`docs/spec/e12_semaphore/`](spec/e12_semaphore/)
- Formal gate (safety): [`scripts/verify-e12-semaphore-formal.sh`](../scripts/verify-e12-semaphore-formal.sh)
