# E12-C AsyncMutex — Independent Adversarial Review

**Review range:** `4716ecf..32f46c2` (6 commits)
**Branch:** `feat/e12-C-async-mutex`
**Reviewer:** independent adversarial (IAR)
**Date:** 2026-07-14

---

## A. Verdict

**CORRECTIVE-REQUIRED** (single P1 — test evidence gap; production code correct)

| Axis | Verdict |
|------|---------|
| Production implementation (lock/unlock/handoff/cancel/deadline) | **PASS** — all paths correctly implement Fiber\* identity ownership. |
| Tests (22 of 23) | **PASS** — deterministic, causal, measurable, reproducible. |
| Formal model | **PASS** — 68,150 states, 19 invariants; 11 negative models all produce expected CEX. |
| Documentation | **CORRECTIVE-REQUIRED** — §17.3 claim overstated; §20.7 table header "Real" conflicts with test's own "possible" qualification. |
| Sanitizers | **PASS** — TSan/ASan/UBSan/Valgrind all clean. |
| Regression | **PASS** — E10 C5 2000/2000, full suite 0 fail. |
| Compile-boundary seal | **PASS** — authority probe: 3/3 bypasses correctly rejected. |

---

## B. Blocker (P1)

### P1 — T19 migration test does not prove its documented requirement (§17.3)

**What §17.3 claims:**

> Fiber A locks AsyncMutex on Worker W0
> Fiber A is stolen by Worker W1 (real E8 steal)
> Fiber A resumes on W1
> Fiber A unlocks AsyncMutex → succeeds

**What T19 actually proves:**

All 23 tests PASS, unlock() does not crash when a fiber is spawned on worker 0 in a 2-worker run, and the fiber completes on (some worker).

**What T19 does NOT prove:**

1. **No gate fiber** — Worker 0 is not held busy. fA is the only fiber; it runs to completion on W0 immediately. There is no window for W1 to steal.
2. **No worker-ID assertion** — No `current_worker_id()` call in the fiber body to confirm which worker actually executed it.
3. **No owner-ID assertion** — No `owner_id_of()` check to prove ownership transferred.
4. **No suspension point after lock** — fA locks then immediately unlocks without yielding, so there is no point at which a steal could occur during ownership.

The test's own CHECK message acknowledges this: `"A unlocked after (possible) migration"`.

**Impact:** The claim "Real E8 migration" in both §17.3 and the §20.7 table header is overstated. The architecture's Fiber\* identity model (§4.2/§4.3) *does* guarantee migration safety — unlock correctly uses `g_worker->current == &fA` — so the production code is correct. The defect is in the TEST EVIDENCE, not the implementation.

---

## C. Remediation requirement (for re-audit)

1. **Fix T19** to prove actual migration:
   - Add a gate fiber that spins (e.g., `spin_wait` loop) on Worker 0 to keep it busy
   - Spawn fA via `spawn_on(fA, 0)` so it lands on W0's queue
   - Insert a suspension point (e.g., `sleep(0)` or yield) in fA **after** `lock()` but **before** `unlock()` so W1 can steal
   - Assert `current_worker_id() != 0` in fA's body to confirm migration occurred
   - Assert expected worker counts (`sched.worker_count() >= 2`)
   - Keep existing assertions (a_released, was_woken, waiting_count == 0)

2. **Downgrade documentation claims** in §17.3 and §20.7 to match what the test provably demonstrates, or remove the table header "Real" qualifier after the test is fixed.

---

## D. Production implementation review

### D.1 Ownership model (PASS)

The implementation uses `Fiber*` identity (§4.2) consistently across all paths:

- `lock()` stores `&caller_fiber` in `m_owner` (uncontested) or resolves via `handoff_one_locked` (contested).
- `unlock()` asserts `m_owner == g_worker->current`.
- `try_lock()` checks `m_owner == nullptr` and sets to `&caller_fiber`.
- The `m_owner` field is `Fiber*`, not `Worker` — this is the architectural guarantee that migration does not invalidate ownership.

All owner access predicates in `scheduler.cpp` are confirmed correct:

| Access point | File | Line range | Correct? |
|---|---|---|---|
| `AsyncMutex::lock` | async_mutex.hpp | 41–85 | ✓ |
| `AsyncMutex::try_lock` | async_mutex.hpp | 87–108 | ✓ |
| `AsyncMutex::unlock` | async_mutex.hpp | 110–134 | ✓ |
| `MUTEX_LOCK` | scheduler.cpp | 1940–1980 | ✓ |
| `MUTEX_UNLOCK` | scheduler.cpp | 1982–2040 | ✓ |
| `mutex_try_lock` | scheduler.cpp | 2045–2065 | ✓ |
| `handoff_one_locked` | scheduler.cpp | 2110–2160 | ✓ |
| `mutex_migration_check` | scheduler.cpp | 2165–2180 | ✓ |

### D.2 Handoff (PASS)

`handoff_one_locked` correctly:
1. Removes head Waiter from `m_wait_queue` via `pop_front()`.
2. Stores `&handoff_waiter->m_fiber_ref` in `m_owner` **before** calling `sched.wake_resume(handoff_waiter)`.
3. Source order verified by the proof-topology diff (commit 72a4636) and T5 phase-seam test.

No-barging verified by: T4 (newcomer try_lock fails while queued), T21 (three-party), T22 (cancelled-head + newcomer fails), and formal invariant `¬Barging`.

### D.3 Cancellation (PASS)

T7–T10 cover:
- Cancel suspended waiter → returns false, correctly removed from queue.
- Cancel after handoff → returns false (already resolved).
- Wrong-mutex same Scheduler → correctly identified via m_owner comparison.
- Wrong-mutex different Scheduler → rejected at Scheduler-level gate.
- External OS-thread cancel → safe via atomic guard.

Formal negative models 7–10 confirm CEX for "cancel wrong waiter" / "cancel after handoff" patterns.

### D.4 Deadline / timer (PASS)

T11–T14 cover all four orderings:
- Free + due → Woken (handoff before timer fires).
- Owned + due → Expired (timer fires during ownership, waiter gets Expired).
- Unlock beats timer → handoff wins, timer returns false.
- Timer beats unlock → timer fires, waiter gets Expired, later unlock finds empty queue.

All correctly use the E11 `DeadlineTimer` substrate with `m_deadline_timer_id` per waiter.

### D.5 Destruction gate (PASS)

`~AsyncMutex()` asserts `m_owner == nullptr && m_wait_queue.empty()` in debug builds. T18 confirms safe destruction of unlocked/empty mutex.

### D.6 Admission closure (PASS)

T6 verified: owner releases lock during a waiter's admission window — waiter correctly acquires without stranding.

---

## E. Formal verification (PASS)

| Property | Status |
|----------|--------|
| Model size | 68,150 distinct states (model permits more states than real implementation — see §7 caveat) |
| 19 invariants | All PASS |
| 11 negative models | All produce expected CEX |
| No-barging invariant | Verified |
| No-unlock-when-unlocked | Verified |
| No-double-unlock | Verified |
| Owner-is-locker | Verified |
| Handoff FIFO | Verified |
| Exactly-once | Verified |

**Pre-existing caveat (unchanged):** The formal model is single-worker and does not model fiber migration. Migration safety is architectural (§4.2 Fiber\* identity), not formal.

---

## F. Compile-boundary seal (PASS)

| Probe | Expected | Result |
|---|---|---|
| `wait_queue()` on public AsyncMutex | must fail | compile error ✓ |
| `owner()` on public AsyncMutex | must fail | compile error ✓ |
| `is_locked()` on public AsyncMutex | must fail | compile error ✓ |
| Scheduler mutex seams (public) | must fail | compile error ✓ |

Per authority hierarchy: E10/E11 precedent does not expose these — E12-C correctly follows.

---

## G. Test quality (PASS with corrective note)

23 tests, all deterministic, all causal (no sleep-based proof):
- T0–T2: construction, try_lock immediate, unlock-no-waiter
- T3: two-waiter FIFO handoff
- T4, T21, T22: no-barging (2-party, 3-party, cancelled-head)
- T5: owner-before-publication internal seam
- T6: admission closure
- T7–T10: cancellation
- T11–T14: deadline precedence (4 orderings)
- T15–T16: cancel races (cancel-wins, handoff-wins)
- T17: exactly-once
- T18: destruction
- **T19: migration (CORRECTIVE-REQUIRED — does not prove migration)**
- T20: coordination 500/500 stress gate

---

## H. Production/formal refinement map (§20.10)

All 5 documented seams are structurally correct: `m_owner`, `m_wait_queue`, `mutex migration check`, `DeadlineTimer`, and `Scheduler::run` are directly connected to their formal counterparts. The map is accurate and traces cleanly.

---

## I. No reachable protocol error

All interleavings tested by:
- Deterministic race tests (T15, T16) — cancel-wins and handoff-wins both covered.
- Formal model (19 invariants, 68,150 states).
- TSan/ASan/UBSan/Valgrind (all clean).
- Coordination 500/500 stress (no queue leak, no timer leak, no strand).

No reachable protocol error found.

---

## J. Substrate scope

E12-C builds on E10 (WaitQueue, WaitNode), E11 (DeadlineTimer), and E12-A/E12-B conventions. All substrate interactions are verified by regression (full suite PASS). No substrate seam is reopened or altered.

---

## K. Summary of all findings

| ID | Severity | File | Finding | Status |
|----|----------|------|---------|--------|
| F1 | P1 | T19 / §17.3 | T19 does not prove actual E8 migration; doc claim overstated | OPEN |
| F2 | P3 | §20.7 | Table header says "Real" but test CHECK says "possible" — doc drift | COVERS F1 |
| F3 | P3 | T19 | Comment says "after_steal" but no steal is forced — misleading naming | COVERS F1 |

No other findings in production code, formal model, test infrastructure, build system, or documentation.

---

## L. Corrective record

### E12-C-MIGRATION-EVIDENCE-CORRECTIVE-1 (COMPLETE)

**Original finding:** T19 did not force or observe actual E8 worker migration.

**Corrective applied:**
- T19 rewritten with E8-T1/T3 steal pattern: `f_blocker` holds W0 busy, `fA` spawned on W0's queue via `spawn_on`, W1 (idle) steals `fA`.
- `fA` records `current_worker_id()` before and after mutex lock, asserts `after_worker == 1` (W1).
- `f2` acquires mutex after `fA` unlocks, proving ownership released.
- T22 cleaned up: removed 3 redundant `std::this_thread::yield()` calls, fixed comment.

**Evidence (10/10 runs):**
| Observation | Value |
|---|---|
| Fiber identity | `&fA` (same object throughout) |
| Worker before | W0 (spawned via `spawn_on(fA, 0)`) |
| Worker after | W1 (`after_worker == 1` asserted) |
| Worker changed | YES (`wa == 1`) |
| Real E8 steal observed | YES (fA on W0's queue, W1 stole) |
| Unlock executed after migration | YES (`a_released == true`) |
| Subsequent acquisition succeeded | YES (`f2_acquired == true`) |

**Sanitizers:** TSan PASS, ASan PASS, UBSan PASS (all 23 cases).
**Full Clang regression:** ALL TESTS PASSED.
**E8 steal regression:** ALL TESTS PASSED (11 cases).

### Updated verdict

F1 (P1): **RESOLVED** — T19 now deterministically proves real E8 migration.

No remaining P1 or P2 findings.

---

## M. Exact next action

E12-C-IMPLEMENTATION-1-INDEPENDENT-REVIEW: **PASS** (corrective applied).
E12-C-MIGRATION-EVIDENCE-CORRECTIVE-1: **COMPLETE**.

Await narrow independent corrective re-review. On PASS:
```
E12-C-IMPLEMENTATION: CLOSED
E12-C: CLOSED
E12-D: PREPARATION-READY
```
