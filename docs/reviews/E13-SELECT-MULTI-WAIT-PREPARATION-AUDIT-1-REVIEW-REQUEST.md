# E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1 — Review Request

**Task**: `E13-SELECT-MULTI-WAIT-PREPARATION-DESIGN-1`
**Date**: 2026-07-19
**Status**: INDEPENDENT ADVERSARIAL REVIEW REQUESTED

---

## 1. Review Scope

This review covers the E13 Select / Multi-Wait preparation design: a
preparation-design-only task that produces design documents, state machines,
test plans, and protocol analysis. No production code, formal spec, or test
code was modified.

### In scope

| File | Description |
|------|-------------|
| `docs/e13-select-preparation.md` | Main preparation design (20 sections) |
| `docs/e13-select-state-machine.md` | State machine specification |
| `docs/e13-select-test-plan.md` | Test plan with 15 core tests |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md` | This document |

### Out of scope

- `include/**` — no production headers modified
- `src/**` — no production source modified
- `docs/spec/**` — no formal specs modified
- `tests/**` — no test code modified
- `xmake.lua` — no build config modified
- E13 production implementation (explicitly denied)
- E13 formal model implementation (denied pending this review)

---

## 2. Baseline

```text
REPOSITORY:     jnhu76/Sluice
BASE_BRANCH:    master
BASE_COMMIT:    be70fde (origin/master HEAD)
REVIEW_DATE:    2026-07-19
```

---

## 3. Governance Context

```text
E10–E12 cross-primitive semantic closure: CLOSED / AUTHORIZED (E12-G)
E13 Select: PREPARATION DESIGN COMPLETE — INDEPENDENT REVIEW REQUIRED
E13 production implementation: DENIED
E13 formal model: DENIED PENDING PREPARATION REVIEW
```

### Key constraint (from E10-E12 closure §10.3)

```text
primitive cancel is NOT Select-level loser authority

E13 requires a parent/group claim that orders:
    group winner selection
        BEFORE
    irreversible primitive resource commit
```

---

## 4. Design Summary

### 4.1 First scope

```text
INCLUDED: Event + Timer + Semaphore + AsyncMutex (Scope S2)
DEFERRED: Queue (payload reservation unproven), AsyncCondition (reacquire interaction unproven)
```

### 4.2 Selected protocol

```text
Candidate A: Central SelectGroup Claim Under Scheduler Authority

Key property: group claim CAS (OPEN → CLAIMED) happens BEFORE any
primitive irreversible commit, under global_mtx_. Loser arms are
retired by canceling their registered WaitNodes — which suffices
because no resource was committed to the loser.
```

### 4.3 Key design decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Deadline model | Per-arm (not group-level) | Composability with existing E11 |
| Fairness | Array-index order | Deterministic, simple, matches programming model |
| Winner linearization | Group claim CAS | Single authority, under global_mtx_ |
| Loser retirement | cancel(registered WaitNode) | Sufficient because resource not committed |
| Publication | Exactly once via group winner | Proven by group claim uniqueness |

---

## 5. Review Checklist

The reviewer should verify:

### 5.1 Protocol correctness

- [ ] Candidate A correctly orders group claim BEFORE primitive commit for all four first-scope primitives
- [ ] No execution trace exists where a loser arm commits a resource
- [ ] The global_mtx_ serialization domain is sufficient for all group claim operations
- [ ] Fast-path (arms already ready at registration) preserves all invariants
- [ ] Timer expiry race with resource readiness produces exactly one winner

### 5.2 State machine completeness

- [ ] SelectGroup state machine covers all transitions
- [ ] SelectArm state machine covers all transitions
- [ ] No unreachable states
- [ ] No dead-end states (except terminal)
- [ ] CAS specifications are precise (expected/desired/returns)

### 5.3 Invariant analysis

- [ ] All 10 invariants (I1–I10) are correctly stated
- [ ] Each invariant has a proof sketch or argument
- [ ] No missing invariants (particularly for lifetime, timer, publication)

### 5.4 Scope decisions

- [ ] Queue deferral is justified (no existing cancel API, payload reservation unproven)
- [ ] AsyncCondition deferral is justified (reacquire non-cancellable, C-H5)
- [ ] Scope S2 is the minimal useful scope (S1 too narrow, S3/S4 too broad)

### 5.5 Error taxonomy

- [ ] All misuse categories are covered
- [ ] Debug assert vs exception vs UB is correctly assigned
- [ ] Cross-Scheduler misuse is caught
- [ ] Empty select is caught

### 5.6 Test plan

- [ ] 15 core tests cover all invariants (see verification matrix)
- [ ] Edge cases are covered (partial registration, destruction, high contention)
- [ ] TSan verification is planned
- [ ] Future test obligations for Queue/Condition are documented

### 5.7 Negative model completeness

- [ ] All 9 negative models (NEG-1 through NEG-9) are plausible violation scenarios
- [ ] Each negative model is rejected by a specific invariant or design property
- [ ] No negative model is trivially dismissed without analysis

---

## 6. Specific Questions for Reviewer

1. **Is Candidate A's reliance on global_mtx_ acceptable for first scope?** The concern is that adding a group claim CAS to the already-busy global_mtx_ increases contention. Is this a real concern at the scale of first-scope Select, or is it premature optimization to worry about?

2. **Is array-index fairness sufficient?** The design chooses lowest-index-wins for ties. Should we require a more sophisticated fairness policy (e.g., primitive FIFO priority) even in first scope?

3. **Should the fast-path transition from Constructing → WinnerClaimed (skipping Arming/Armed) be specified as an optimization, or should it be the primary path?** The design describes it as an optimization; should it instead be the norm?

4. **Is the Queue deferral well-justified?** The design defers Queue because v1 has no cancel API and payload reservation is unproven. Is this sufficient, or should we commit to a Queue integration timeline?

5. **Is the AsyncCondition deferral well-justified?** The design defers AsyncCondition because the reacquire epoch is non-cancellable (C-H5). Is this the correct characterization, or is there a viable integration path?

6. **Are the 10 invariants (I1–I10) sufficient?** Are there missing invariants that a formal model should check?

---

## 7. Review Deliverables

The reviewer should produce:

1. **Verdict**: PASS / PASS WITH OBSERVATIONS / FAIL
2. **Findings**: Each finding categorized as:
   - MUST FIX (blocks preparation closure)
   - SHOULD FIX (strong recommendation)
   - CONSIDER (optional improvement)
3. **Verification**: Confirmation that the design is internally consistent
4. **Risk assessment**: Any residual risks not captured in the preparation doc

---

## 8. git diff --name-status

```text
A       docs/e13-select-preparation.md
A       docs/e13-select-state-machine.md
A       docs/e13-select-test-plan.md
A       docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md
```

```text
Production/tests/formal spec: zero modifications confirmed.
```
