# E12-B Semaphore Production Implementation — Independent Adversarial Review

> **Reviewer:** Independent adversarial review (opencode)
> **Date:** 2026-07-19
> **Status:** `REVIEW-COMPLETE`
> **Verdict:** `E12-B-IMPLEMENTATION: ACCEPT (WITH OBSERVATIONS)`
>
> Implementation status at review: `E12-B-IMPLEMENTATION-1: COMPLETE`; document
> status `E12-B: REVIEW-REQUIRED`. This review closes `REVIEW-REQUIRED`.

## Scope

Review of the E12-B Semaphore production implementation (`sluice-CORE-E12-B`):

- **Production code**: `include/sluice/async/semaphore.hpp` (new), `include/sluice/async/scheduler.hpp` (seams), `src/async/scheduler.cpp` (implementation)
- **Tests**: `tests/e12_semaphore_test.cpp` (31 deterministic tests), `tests/e12_semaphore_authority_probe.cpp` (NEG compile probe)
- **Formal model**: `docs/spec/e12_semaphore/` (8 TLA+ modules: 1 correct + 7 negative)
- **Documentation**: `docs/e12-semaphore.md` (880-line authority + as-built), `docs/async-runtime-plan.md` E12 status
- **Build integration**: `xmake.lua` (test target)

## Executive Summary

The E12-B Semaphore implementation is clean, well-structured, and demonstrably correct. The 12-invariant TLA+ safety model passes, all 31 runtime tests pass (debug + release), all 7 negative models produce the expected named counterexample, and the authority-sealing compile probe successfully rejects the forbidden WaitQueue bypass. The implementation follows the E12-A Event pattern faithfully (same Scheduler seam design, same test discipline, same lock order), extends no Scheduler refactor, and introduces no generic grant framework.

**Verdict: ACCEPT.** No blocking defects were found. Three non-blocking observations are recorded below.

---

## 1. Verification Evidence

### 1.1 Runtime tests (31/31 pass, debug + release)

```
e12_sem_t0  construction invariants + available() initial snapshot         PASS
e12_sem_t1  try_acquire consumes exactly one                              PASS
e12_sem_t2  try_acquire failure at zero (no mutation)                     PASS
e12_sem_t3  immediate acquire resolves Woken (no suspend)                 PASS
e12_sem_t4  immediate acquisitions stop at zero (second suspends)         PASS
e12_sem_t5  zero-permit acquire suspends; one release grants              PASS
e12_sem_t6  release (no waiter) increments available                      PASS
e12_sem_t7  release at capacity returns false, no mutation                PASS
e12_sem_t8  one release never both wakes AND stores                       PASS
e12_sem_t9  queued grant from zero does not underflow                     PASS
e12_sem_t10 FIFO: W1 before W2 => W1 granted first                       PASS
e12_sem_t11 W2 cannot steal W1's release permit                           PASS
e12_sem_t12 try_acquire cannot bypass queued waiter                       PASS
e12_sem_t13 W1 cancelled before release CS => release grants W2           PASS
e12_sem_t14 permit + due deadline -> Woken (permit precedence)            PASS
e12_sem_t15 no permit + due deadline -> Expired (I5)                      PASS
e12_sem_t16 permit + future deadline -> immediate Woken                   PASS
e12_sem_t17 release wins before timer -> Woken                            PASS
e12_sem_t18 timer wins before release -> Expired                          PASS
e12_sem_t19 registered cancel -> true, Cancelled                          PASS
e12_sem_t20 cancel after grant (Woken) -> false                           PASS
e12_sem_t21 cancel after expiry (Expired) -> false                        PASS
e12_sem_t22 repeated cancel -> second false                               PASS
e12_sem_t23 detached node cancel -> false                                 PASS
e12_sem_t24 wrong Semaphore, same Scheduler -> false                      PASS
e12_sem_t25 wrong Semaphore, different Scheduler -> false                 PASS
e12_sem_t26 external-thread release wakes parked Live Scheduler           PASS
e12_sem_t27 terminal waits leave queue empty (no leak)                    PASS
e12_sem_t28 terminal timed waits leave no timer registration              PASS
e12_sem_t29 safe destruction after terminal closure                       PASS
e12_sem_t30 repeated mixed multi-waiter stress (100x K=3)                 PASS
```

All cases use mechanically gated phase seams + retry loops (not `sleep_for`
causal proof). The deadline cases use the controllable logical clock. Test
hygiene is excellent.

### 1.2 Formal model (TLA+/TLC)

The correct safety model checks all 12 invariants over 58,332 generated states
(12,214 distinct). No invariant uses a primed variable.

| Negative model | Expected violation | Verdict |
|---|---|---|
| NEG-1 AdmissionClosure | `InvAdmissionClosure` | CEX |
| NEG-2 ReleaseLoss | `InvPermitConservation` | CEX |
| NEG-3 DoubleStore | `InvPermitConservation` | CEX |
| NEG-4 NonFIFOGrant | `InvFIFOGrant` | CEX |
| NEG-5 OverflowMutation | `InvOverflowNonMutation` | CEX |
| NEG-6 IdlePermitEligibleWaiter | `InvNoIdlePermitWithEligibleWaiter` | CEX |
| NEG-7 DeadlinePrecedence | `InvPermitFirstDeadline` | CEX |

The wrong-property gate (NEG-3 checked against `InvFIFOGrant`) passes — proves
defect specificity. The compile-probe gate rejects the F-SEM-SEAM-1 bypass.

### 1.3 Sanitizer runs (as-built report)

TSan, ASan, UBSan all pass (0 warnings/errors/leaks). TSan regressions on E11,
E9, and E12-A tests also pass.

---

## 2. Critical Analysis

### 2.1 Permit-accounting correctness

The central defect the preparation corrective fixed — the `available_--`/refund
model that would underflow at `available_ == 0` — is provably absent. The
release disposition is exactly-one-of (transfer/store/reject). The acquire
admission recheck consumes a stored permit only when one exists and this node
is the FIFO head.

The implementation matches the TLA+ state machine exactly:
- `sem_try_acquire`: checks `waiters.empty_locked()` then `available > 0`
- `sem_acquire`: register → recheck admission (available > 0 AND prev_ == nullptr) → commit suspension
- `sem_release`: `wake_wait_one_locked` (transfer) → else store → else overflow reject
- `sem_cancel`: membership gate + `cancel_locked` → retire timer → route

### 2.2 Concurrency safety

Every authoritative decision runs under `global_mtx_` + `waiters_.mtx()` in the
correct lock order. The admission window is closed (register + recheck + commit
suspension under both locks). Only `context_switch` is outside the lock.

The lock-free `available_` is used only for observation (`available()`); every
mutation is serialized in the same Scheduler coordination domain as E10/E11/
E12-A. This is the correct pattern and matches the design document.

External-thread release (`g_worker == null`) routes through `pending_spawn_` +
`signal_wake_locked` — exactly the Event external-thread path.

### 2.3 Cancel safety (queue-identity)

`sem_cancel` scans its own intrusive list via `contains_locked` under both
locks. It never reads a foreign node's `home_` field and never locks a foreign
Scheduler. This makes wrong-Semaphore cancellation structurally safe (verified
by T24 and T25 in the test suite; the tests cover same-Scheduler and
cross-Scheduler wrong-Semaphore cases).

The membership gate is the right design: it avoids the ABA and use-after-free
hazards that a `home_`-field check would introduce on node reuse.

### 2.4 No-barging enforcement

`sem_try_acquire` checks `waiters.empty_locked()` under both locks before
consuming a stored permit. `sem_acquire` checks `node.prev_ == nullptr` (FIFO
head identity) before admitting. Both are correct per the design.

However, there is a notable asymmetry: `sem_acquire` reads `node.prev_` as a
FIFO-head check, while `sem_try_acquire` reads `waiters.empty_locked()`. Under
the lock protocol these are equivalent — a non-empty queue means someone is
queued ahead. But the `node.prev_` check is strictly more precise for the
acquire path (it checks THIS node's position relative to the head) while
`empty_locked()` is sufficient for try_acquire (which does not have a node to
check). This is correct, not a defect.

### 2.5 Timer retirement

`sem_acquire_until` correctly retires the timer registration in three exit
paths: Woken inline (admission), Expired inline (I5), and the defense-in-depth
terminal-recheck. The timer pool block is ACTIVE→RETIRED or ACTIVE→CONSUMED.
T28 verifies no timer registration leaks.

### 2.6 FIFO ordering proof

T10 (W1 before W2, two releases → W1 first) and T11 (W2 cannot steal W1's
release permit) prove FIFO ordering in the runtime. The TLA+ `InvFIFOGrant`
proves it in the model. The Conclusion A lock-protocol proof (every resolver
holds `global_mtx_`; `wake_wait_one_locked` returns nullptr only when empty)
is documented with a complete call-site table (§5 of the design doc).

---

## 3. Code Quality

### 3.1 Readability and structure

- **semaphore.hpp**: clean, well-commented, complete documentation of contract
  for every method. The SEALED PUBLIC AUTHORITY comment block is explicit about
  what downstream code can and cannot do.
- **scheduler.cpp `sem_*` methods**: each has a detailed block comment explaining
  the admission closure, the lock domain, the forbidden shapes, and the
  contraction map from the production code to the formal model.
- **Test file**: well-organized into slices (construction, FIFO, deadline,
  cancel, external-thread, stress). Each case has a header comment explaining
  the scenario and the invariants it asserts.

### 3.2 Documentation

`docs/e12-semaphore.md` is 880 lines and covers: policy decisions (A1-A5),
corrected permit conservation law, exact release state machine, Conclusion A
proof (complete call-site table), stable-state invariant, formal model catalog
with all 12 invariants, negative-model matrix, runtime test plan, implementation
as-built with file list, API documentation, test case table, verification
results, autonomous self-review, scope audit, and known limitations.

### 3.3 Build integration

The `xmake.lua` integration is minimal and correct: one test target that links
against `sluice_async_internal_testing` for the deterministic test hooks. The
authority probe is not a build target — it is exercised by the formal gate
script, which is the right approach (it must never compile).

---

## 4. Observations

### O1 (minor): Stress test cancellation retry loop

In T30, the cancel fiber retries up to 50 times with `std::this_thread::yield()`:
```cpp
for (int i = 0; i < 50; ++i) {
    if (sem.cancel(n[0])) { cancelled.store(true); break; }
    std::this_thread::yield();
}
```

This is described as a bounded retry loop for the cancel-or-grant race (the
waiter may have already been granted by a release before the cancel acquires
`global_mtx_`). The choice of 50 is arbitrary but adequate for a test. A
more principled approach would retry until `n[0].is_terminal()` or use the
existing test-phase seam mechanism, but this is a single-worker single-run
test and the loop is benign.

### O2 (minor): Defensive terminal check after registration

In `sem_acquire` (lines 1655-1659) and `sem_acquire_until` (lines 1739-1748),
there is a defense-in-depth check:
```cpp
if (node.is_terminal()) {
    waiters.unlink_locked(node);
    --waiting_waitq_count_;
    return;
}
```

The comment correctly observes this is unreachable — the node was just
registered under both locks and every resolver takes `global_mtx_`. The code is
harmless defensive programming but slightly increases the cognitive surface.
This is a pattern present in E12-A and E12-C as well (intentional consistency).

### O3 (informational): Future correctness dependency on E12-G

The review surface for this implementation included a cross-primitive audit
(`E12-G Cross-Primitive Cancellation / Deadline Audit`, listed in the
`async-runtime-plan.md` as `REVIEW-REQUIRED`). This audit will verify that
the cancellation and deadline semantics are coherent across Event, Semaphore,
Mutex, Condition, and Queue. The Semaphore implementation is consistent with
the Event and Mutex patterns, so no structural issue is expected, but this
review does not cover the cross-primitive audit itself.

---

## 5. Scope Audit

| Concern | Present? |
|---|---|
| Generic grant framework | NO |
| Public WaitQueue access | NO (no `wait_queue()` accessor; NEG probe) |
| New Scheduler winner seam | NO |
| Scheduler refactor | NO (mirrors E12-A Event pattern) |
| Production test hooks in release | NO (seams isolated to `internal_testing`) |
| Formal model weakening | NO (model unchanged; PASS + 7 NEG CEX) |
| Grant-in-flight state | NO |
| Refund path | NO |
| `available_--` from zero | NO (transfer does not touch `available_`) |
| E12-C..G changes | NO |

---

## 6. Verdict

```
E12-B-IMPLEMENTATION: ACCEPT (WITH OBSERVATIONS)
```

**No blocking defect was found.** The implementation is consistent with the
preparation authority, the TLA+ safety model, the Conclusion A lock-protocol
proof, and the E12-A Event pattern. The test suite is comprehensive and
deterministic. The authority-sealing compile probe is present and correct.

The implementation may proceed to CLOSED once:
1. The observations above are acknowledged (no change required for O1–O3).
2. The E12-G cross-primitive audit (separate review surface) passes.

**Document status updated:** `E12-B-IMPLEMENTATION: REVIEW-COMPLETE`.

---

*Review methodology: source code audit, adversarial scenario analysis, build
and execution of all test targets (debug + release), TLA+ model analysis
(safety, 7 negatives, wrong-property gate, compile-probe gate), lock-domain
analysis, permit-accounting trace verification, cancel-safety audit,
documentation review.*
