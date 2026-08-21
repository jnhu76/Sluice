# Issue Lifecycle and Triage

This document defines the repository-wide lifecycle vocabulary for GitHub issues.

The goal is simple: **Open means actionable or intentionally tracking active work.** Deferred, speculative, policy-blocked, or superseded work should not permanently occupy the active queue.

## Lifecycle states

### ACTIVE

**GitHub state:** Open

Use when work is currently being executed or has been explicitly selected as the next engineering/audit task.

Requirements:
- concrete current scope;
- known next action;
- closure criteria stated in the issue or governing design;
- implementation/audit authority limited to the issue scope.

### READY / EXPERIMENT

**GitHub state:** Open

Use when bounded evidence collection, benchmarking, or design work is useful now, but production modification is not yet authorized.

Promotion to ACTIVE requires the issue's own evidence/promotion gate to be satisfied.

### ON-TOUCH

**GitHub state:** Open

Use for an intentionally gradual maintenance ledger.

Rules:
- no big-bang cleanup campaign;
- address only the touched family/site when related work is already in that area;
- preserve unrelated residual families in the ledger;
- public-behavior changes still require their own review/ADR where applicable.

### UMBRELLA / TRACKING

**GitHub state:** Open

Use for roadmap, governance, or architecture umbrellas.

Rules:
- the umbrella itself does not authorize broad implementation;
- focused child issues/PRs carry production work;
- close only when acceptance criteria are met or the umbrella is explicitly re-scoped.

### DEFERRED

**GitHub state:** Closed

**State reason:** `not planned`

GitHub has no native Deferred state. `not planned` is the mechanical representation of "valid record, not actionable under the current scope".

Every deferred issue must record a concrete **reopen trigger**, for example:
- a product/security policy changes;
- a real bug or repeated maintenance failure demonstrates the concern;
- prerequisite audit/verification work lands;
- a supported platform requires the capability;
- performance attribution crosses an established promotion threshold.

Deferred does **not** mean invalid. Preserve the issue as the historical design/risk record and reopen it when the trigger is satisfied.

### COMPLETED

**GitHub state:** Closed

**State reason:** `completed`

Use only when the issue's accepted scope is actually delivered, or when any meaningful residual has already been explicitly re-tracked.

Do not let an accidental PR closing keyword turn an issue with residual work into a false `completed` record.

## Lifecycle transition rules

When changing lifecycle state:

1. preserve historical issue content;
2. add a short lifecycle comment with:
   - new lifecycle state;
   - reason;
   - execution authority;
   - next action or reopen/promotion trigger;
3. use GitHub state reasons consistently;
4. before implementing work from READY, DEFERRED, or an UMBRELLA, promote/reopen the focused issue and record why;
5. if the issue is an audit report, distinguish the report itself from implementation follow-ups.

## Classification is separate from lifecycle

Lifecycle answers **when/how this item is actionable**. It is not the same as technical classification.

Examples of technical classifications include:

```text
C++ implementation defect
model-only defect / model drift
coverage gap
refinement/documentation drift
security policy gap
performance attribution candidate
maintenance debt
design research
unknown
```

A model defect may be ACTIVE while a high-risk architecture refactor is DEFERRED. Do not overload lifecycle labels to mean technical severity.

## Re-triage triggers

Revisit READY/DEFERRED/UMBRELLA issues when:

- a CI/runtime failure maps to them;
- a C++-first audit produces a concrete defect;
- a major architecture phase closes;
- a supported-platform/product/security requirement changes;
- performance evidence crosses a promotion threshold;
- a repeatedly touched maintenance family demonstrates real friction.

## Relationship to repository authority

This file governs tracker lifecycle only.

Implementation semantics remain governed by:

1. explicit current task / approved issue scope;
2. accepted ADRs and active design/closeout documents;
3. architecture constitution;
4. `AGENTS.md`;
5. public API and production implementation;
6. tests and formal evidence.
