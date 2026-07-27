# E13-SELECT-MULTI-WAIT-PR16-CORRECTIVE-1 — Review Request

**Task**: `E13-SELECT-MULTI-WAIT-PR16-CORRECTIVE-2`
**Date**: 2026-07-19
**Status**: DELTA RE-AUDIT REQUESTED — DOCUMENTATION CORRECTIVE ONLY

---

## 1. Review Scope

This corrective updates the E13 Select documentation to address PR #16 external
review findings and closes two remaining gaps: branch-specific Timer
lifecycle ordering and TimerRegistration pre-dereference authority.
No production code, test code, formal spec, or build policy was modified.

### In scope

| File | Description |
|------|-------------|
| `docs/e13-select-preparation.md` | Corrected preparation design (WaitNode finalization law, event/timer winner, loser, lifecycle, state machine, invariants, timer authority) |
| `docs/e13-select-state-machine.md` | Corrected state machine (inline SelectOperation path, phase-aware timer invariants, branch-specific lifecycle, timer authority) |
| `docs/e13-select-test-plan.md` | Corrected test plan (17 deterministic tests, T12a/T12b split, T17 timer authority lease, winner/loser checks) |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1.md` | Historical erratum (authorization, untracked provenance) |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REAUDIT-1.md` | Supersession notice |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md` | Historical count annotation |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REVIEW-REQUEST.md` | Updated test count reference |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PR16-CORRECTIVE-1-REVIEW-REQUEST.md` | This document |

### Out of scope

- `include/**` — no production headers modified
- `src/**` — no production source modified
- `docs/spec/**` — no formal specs modified
- `tests/**` — no test code modified
- `xmake.lua` — no build config modified
- E13 production implementation (denied)
- E13 formal model implementation (denied until delta re-audit passes)

---

## 2. Baseline

```text
REPOSITORY:     jnhu76/Sluice
BASE_BRANCH:    master
MERGE_BASE:     be70fde (origin/master HEAD)
WORKING_BRANCH: feature/E13-preparation
TASK:           E13-SELECT-MULTI-WAIT-PR16-CORRECTIVE-2
DATE:           2026-07-19
```

---

## 3. Delta Re-Audit Checklist

The delta re-audit must verify only:

- [ ] 1. Event winner: `wake_node_locked` resolves Woken and unlinks in one queue CS.
- [ ] 2. Timer winner: `try_claim_expiry` consumes registration, `expire_locked` resolves Expired and unlinks in one queue CS.
- [ ] 3. Losers: `cancel_locked` resolves Cancelled then unlinks (never unlink-before-resolve).
- [ ] 4. Waiting counters and active deadline counters close exactly once per arm.
- [ ] 5. Typed context clears only after WaitNode is terminal AND detached.
- [ ] 6. SelectOperation has an inline completion path (Ready -> Completed) that bypasses Waiting.
- [ ] 7. Timer formal invariants allow `Active` during registration/Armed phases, requiring `Retired`/`Consumed` only after completion.
- [ ] 8. Historical authorization errata in audit and supersession notice in reaudit are accurate.
- [ ] 9. Test count is consistently 17 across all documents; T12 is split into catchable (T12a) and fatal-assertion (T12b) variants; T17 covers ACTIVE authority lease serialization.
- [ ] 10. No production, test, formal-spec, or build-policy files were changed.
- [ ] 11. Branch-specific TimerRegistration ordering is consistent across all docs (Event winner, Timer winner, Event loser, Timer loser each have distinct sequences).
- [ ] 12. Timer pump checks ACTIVE authority before dereferencing WaitNode or queue (`select_timer_due_locked` protocol).

---

## 4. Corrective Summary

### 4.1 WaitNode finalization law

A new controlling section establishes:

```
resolve terminal outcome
    BEFORE
unlink from WaitQueue

resolve + unlink under same queue mutex critical section
```

### 4.2 Event winner correction

Phase 2 of the Event broadcast algorithm corrected:

```
acquire winner Event queue mutex
call wake_node_locked(winner.node)
    -> resolve Woken
    -> unlink in the same queue CS
release queue mutex
```

### 4.3 Timer winner correction

Corrected ordering:

```
group claim
TimerRegistration ACTIVE -> CONSUMED (try_claim_expiry)
acquire timer_waiters_ mutex
call expire_locked(winner.node)
    -> resolve Expired
    -> unlink in the same queue CS
release timer_waiters_ mutex
```

### 4.4 Loser correction

Replaced unlink-first with:

```
acquire arm.queue.mtx()
call cancel_locked(arm.node)
    -> resolve Cancelled
    -> unlink in the same queue CS
release arm.queue.mtx()
```

### 4.5 State machine correction

SelectOperation:

```
Ready -> CompletedInline (when arm ready at admission)
Ready -> Waiting -> CompletedSuspended (when no arm ready)
```

### 4.6 Timer invariant correction

```
InvTimerActiveOnlyBeforeCompletion:
    timer_state == Active => group_phase in {Constructing, Arming, Armed, WinnerClaimed}

InvTimerTerminalAfterCompletion:
    group_phase in {Completed, Destroyed} => forall timer arm: timer_state in {Retired, Consumed}
```

### 4.7 Formal actions added

```
ClearTypedContext
FinalizeEventWinner
FinalizeTimerWinner
FinalizeLoser
CloseWaitAccounting
```

### 4.8 New invariant

```
InvNoContextClearWhileRegistered
```

---

## 5. Prior Finding Disposition

| Finding | Status | Reference |
|---------|--------|-----------|
| Gemini-1: Event winner remains linked | CLOSED | preparation §F |
| Gemini-2: Timer winner remains linked | CLOSED | preparation §G |
| CodeRabbit-1: Winner/loser finalization incomplete | CLOSED | preparation §E, §F, §G, §H |
| CodeRabbit-2: Loser unlink-before-resolve | CLOSED | preparation §H |
| CodeRabbit-3: Timer invariant phase-invalid | CLOSED | preparation §K |
| CodeRabbit-4: Missing inline SelectOperation | CLOSED | preparation §J |
| CodeRabbit-5: Test count mismatch | CLOSED | preparation §L |
| CodeRabbit-6: T12 exception/assertion conflation | CLOSED | preparation §L |
| CodeRabbit-7: Untracked provenance | CLOSED | audit erratum §U.2, reaudit §T.5 |
| Independent-1: Historical authorization ambiguity | CLOSED | audit erratum §U.1, reaudit §T |
| Independent-2: Missing ClearTypedContext action | CLOSED | preparation §K, §16 |
| Independent-3: Missing InvNoContextClearWhileRegistered | CLOSED | preparation §K, §16 |
| Independent-4: Internal test seam integration | CLOSED | preparation §M |

---

## 6. Authorization

```text
E13 PREPARATION:
OPEN — DELTA RE-AUDIT REQUIRED

PR16 CORRECTIVE:
AUTHOR SELF-ASSESSMENT PASS

FORMAL MODEL IMPLEMENTATION:
DENIED PENDING DELTA RE-AUDIT

PRODUCTION IMPLEMENTATION:
DENIED
```

---

## 7. Review Deliverables

The delta re-auditor should produce:

1. **Verdict**: PASS / FAIL for each of the 12 checklist items (section 3)
2. **Overall verdict**: PASS / FAIL for the corrective
3. **Authorization effect**:
   - If PASS: Formal model implementation authorized for Event + Timer only
   - If FAIL: Specific corrective required before re-submission
4. **Confirmation**: No production/test/formal/build code was modified
