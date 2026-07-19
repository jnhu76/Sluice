# E12-D AsyncCondition — Preparation Audit 1 (Corrective-1)

> **Historical-status notice (2026-07-19).** This artifact's statements that
> E12-C implementation was CLOSED record the prerequisite assumption used by
> this earlier E12-D preparation audit; they are not a current E12-C review
> verdict. The controlling later artifact is `docs/reviews/E12-C-REVIEW.md`,
> whose final next action still requires the migration data-race micro-review.
> Current status is E12-C IMPLEMENTATION COMPLETE / REVIEW-REQUIRED.

**Audit range:** `721cf48` (master, E12-C merged) — `feat/e12-D-condition-prep` (preparation branch)
**Auditor:** independent adversarial
**Date:** 2026-07-14

---

## A. Verdict

**CORRECTIVE-COMPLETE — PREPARATION REVIEW-REQUIRED**

| Axis | Verdict |
|------|---------|
| Authority document review | **PASS** — all 6 authority documents read and mapped |
| Production code search | **PASS** — zero E12-D references exist |
| Human policy register | **PASS** — all 10 policies (C-H1–C-H10) CLOSED |
| Structural constraint (WaitNode) | **PASS** — corrected: stack-local reacquire node, not two caller nodes |
| Protocol closure (lost-notify) | **PASS** — combined CONDITION-WAIT-PREPARE seam CLOSED; register-before-release order correct |
| Lock topology | **PASS** — sequential queue-lock release (no simultaneous nesting); global_mtx_ provides atomicity |
| Frozen public API | **PASS** — bound to AsyncMutex at construction; one caller WaitNode; no exposed accessors |
| notify_one authority | **PASS** — FIFO, non-persistent, single publication |
| notify_all authority | **PASS** — first-scope included; Scheduler-private snapshot/drain; excludes late waiters |
| Deadline semantics | **PASS** — Condition epoch only; due-inline retains ownership; reacquire untimed |
| Cancel semantics | **PASS** — Condition node only; reacquire non-cancellable; node-specific authority |
| Mandatory reacquire protocol | **PASS** — wake-then-ordinary-lock (C-H3); FIFO tail (C-H8) |
| Wait-phase state machine | **PASS** — 6 states; safety statements documented |
| Destruction and lifetime | **PASS** — active_waits_ counter required; reacquire-phase destruction case identified |
| Formal-model preparation | **PASS** — complete state, actions, Seq queues, 15 actions |
| Property preparation | **PASS** — 19 properties classified (state invariant / transition / refinement / liveness) |
| Negative-model preparation | **PASS** — 14 negative models (NEG-C1–C14) with expected failure properties |
| Runtime-test preparation | **PASS** — 33 tests (T0–T32) across all categories |
| Deterministic seam phases | **PASS** — 6 candidate phases identified |
| Prerequisite status corrected | **PASS** — E12-C-IMPLEMENTATION: CLOSED; stale REVIEW-REQUIRED claims removed |

---

## B. Structural findings

### F1 — WaitNode single-shot: stack-local reacquire node (corrected)

The E10 WaitNode is single-shot (Detached→Registered→terminal, no reset). The
two-epoch Condition protocol (Condition wait → Mutex reacquire) therefore
requires two WaitNode instances. **Corrected conclusion:** the public API
accepts one caller-provided Condition WaitNode; the reacquire phase creates a
stack-local WaitNode inside the active Fiber's wait call (C-H7). This is safe
because:

- The reacquire is non-cancellable and untimed (C-H5) — no external resolver
  can touch the stack-local node
- The Fiber is stackful — the local outlives the reacquire suspension
- No Condition-object member node — no cross-call sharing or reentrancy defect
- No heap allocation

The previous draft's claim that the public API must accept two caller-provided
WaitNodes is **corrected**. The one-caller-node model is authoritative.

### F2 — Lost-notify closure: combined CONDITION-WAIT-PREPARE (corrected order)

The corrective establishes the definitive protocol order:

```text
register Condition node BEFORE releasing the Mutex, under global_mtx_
```

The queue locks are sequential (not simultaneous):

```text
global_mtx_
    -> Condition queue mutex
release Condition queue mutex
    -> AsyncMutex queue mutex
release AsyncMutex queue mutex
```

The combined seam (`CONDITION-WAIT-PREPARE`) is atomic because `global_mtx_`
remains held throughout.

### F3 — No Condition code exists

Confirmed: zero Condition references in the codebase. Clean slate.

### F4 — All precedents identified

E10 (WaitQueue FIFO), E11 (timer registration), E12-A (Event loop-drain for
notify_all), E12-B (admission closure), E12-C (direct handoff, owner-before-
publication). No new substrate required beyond CONDITION-WAIT-PREPARE.

---

## C. Corrective record

| Corrective | Status | Description |
|------------|--------|-------------|
| C-H1–C-H10 | **CLOSED** | Human policy register — all 10 decisions resolved |
| §2.1 | **CORRECTED** | WaitNode conclusion: stack-local reacquire node replaces two-caller-node |
| §2.2 | **CORRECTED** | Lost-notify protocol order: register BEFORE release |
| §2.3 | **CORRECTED** | Lock topology: sequential queue locks, no simultaneous nesting |
| §2.4 | **CORRECTED** | notify_all: included in first scope; snapshot/drain mechanism |
| §2.5 | **CORRECTED** | Deadline: due-inline retains ownership; reacquire untimed |
| §2.6 | **CORRECTED** | Cancel: node-specific; reacquire non-cancellable |
| §2.7 | **FROZEN** | Public API: bound AsyncMutex, one caller WaitNode, no accessors |
| §2.8 | **ADDED** | Wait-phase state machine (6 states, safety statements) |
| §2.9 | **ADDED** | Destruction: active_waits_ counter required |
| §2.10 | **ADDED** | Formal model: complete state, 15 actions, Seq queues |
| §2.11 | **ADDED** | Properties: 19 invariants classified |
| §2.12 | **ADDED** | Negative models: 14 NEG-C1–C14 with expected failures |
| §2.13 | **ADDED** | Tests: 33 tests T0–T32 |
| §2.14 | **ADDED** | Deterministic phases: 6 candidates |
| §2.15 | **CORRECTED** | Prerequisite status: E12-C CLOSED |

---

## D. Remaining work before implementation

The following are NOT blocked — they are refinements that happen during or
before implementation:

| Item | Classification | Notes |
|------|---------------|-------|
| CONDITION-WAIT-PREPARE C++ signature | IMPLEMENTATION BOUNDARY | Semantic name fixed; exact params are impl detail |
| active_waits_ counter | IMPLEMENTATION BOUNDARY | Either assert or rely on existing queue assertions |
| Deterministic test phases | IMPLEMENTATION BOUNDARY | Select from 6 candidates during impl |
| Mutex handoff helper locking | IMPLEMENTATION BOUNDARY | Caller-lock-held variant of handoff_one_locked |
| Formal model | PREPARATION COMPLETE | Add TLA+ files in implementation phase |
| Runtime tests | PREPARATION COMPLETE | Write T0–T32 during implementation |

---

## E. Status transition

```text
E12-D-CONDITION-PREPARATION-AUDIT-1: CORRECTIVE-COMPLETE
E12-D-PREPARATION: REVIEW-REQUIRED
E12-D-IMPLEMENTATION: BLOCKED
```

Exact next action:

```text
RUN INDEPENDENT E12-D CONDITION PREPARATION REVIEW
```

Do not implement E12-D yet.

---

## F. Authority cross-reference (updated)

| Authority document | Section mapped | Condition relevance |
|--------------------|---------------|-------------------|
| E10 WaitNode/WaitQueue (§2, §5, §6, §8) | Single-shot lifecycle, FIFO, one-winner CAS, Scheduler-only friend | Two-node requirement (F1); notify_one; cancellation; lock order |
| E11 Deadline/Timer (§I4-I5) | Timer registration, resource-first precedence, loser semantic | Condition deadline; due-inline retains ownership |
| E12-A Event (§set) | Loop wake-one-til-empty for release-all | notify_all mechanism precedent (C-H10) |
| E12-B Semaphore (§admission) | Admission closure under global_mtx_+q.mtx() | Reacquire admission pattern |
| E12-C AsyncMutex (§8, §9, §10) | Direct handoff, owner-before-publication, FIFO no-barging, MUTEX-HANDOFF-ONE | Reacquire epoch (C-H3); handoff reuse; lock order; owner-before-publication refinement |
| E12 plan §7 (F-COND-1) | Model A vs B cluster, trace matrix C1-C4 | Return contract cluster now CLOSED as Model A (C-H1) |
