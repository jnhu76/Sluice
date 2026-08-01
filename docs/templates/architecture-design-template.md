# [Title: Design of X]

**Author:** [name]
**Date:** [YYYY-MM-DD]
**Status:** [Draft | Proposed | Accepted | Superseded]
**Governing ADR:** [ADR name or "this document becomes the ADR"]
**Constitution rules touched:** [AC-N list]

---

## 1. Problem

What specific problem does this design solve? State it as an observable defect
or missing capability, not as a preference.

> Required: one paragraph, concrete. "The current system cannot..." or
> "When X happens, Y breaks because..."

---

## 2. Current Authority

Who currently owns the behavior being changed? Cite the header, ADR, and
implementation that establish the current contract.

> Required: file paths + line ranges or section numbers.

---

## 3. Affected Capabilities

Which capability objects and layers are affected?

```text
Capability:   [AsyncIoContext | Scheduler | Runtime | Backend | Group | Batch | primitive]
Layer:        [L0 | L1 | L2 | E7-E13 | E16]
Holder:       [who holds this capability at the call site]
```

---

## 4. As-Built Path

Describe the CURRENT code path that this design modifies. Use the format from
`docs/architecture/as-built-async-architecture.md`. Do not describe how you
wish it worked — describe how it actually works today.

> Required: sequence of calls with authority annotations.

---

## 5. Proposed Path

Describe the NEW code path. Same format. Highlight every difference from §4.

> Required: sequence of calls with authority annotations. Mark each new or
> changed step with `[CHANGED]` or `[NEW]`.

---

## 6. Zig Source-Derived Comparison

Which Zig concept does this relate to? Cite the Zig type/function and its
semantic purpose. State whether this design moves TOWARD or AWAY from the Zig
model.

```text
Zig concept:       [e.g., Operation.Storage]
Zig file:          [e.g., zig/lib/std/Io.zig:400-423]
Direction:         [toward | away | orthogonal]
Conformance class: [F | I | A | M | O | U]
```

---

## 7. Classification

Select ONE primary classification and justify:

- [ ] **Faithful** — preserves Zig core semantic, C++ expression differs
- [ ] **Intentional Divergence** — approved deviation with documented reason
- [ ] **Corrective** — fixes accidental drift back toward approved contract
- [ ] **New Direction** — deliberately extends beyond both Zig and current ADRs

> Required: 2-3 sentences justifying the classification.

---

## 8. Ownership

For each new or modified object:

```text
Object:         [name]
Owner:          [who constructs it]
Lifetime:       [construction → destruction scope]
Borrowers:      [who may hold references/pointers]
Stability:      [address-stable | movable | fixed-layout]
```

---

## 9. State Machine

Provide the full state machine per Gate 1 format. Every transition MUST have:
authority, lock domain, allocation, failure, wake, shutdown.

```text
States: [list]

Transitions:
  [state_a] → [state_b]
    Authority:   ...
    Lock domain: ...
    Allocation:  ...
    Failure:     ...
    Wake:        ...
    Shutdown:    ...
```

---

## 10. Linearization Points

For each operation, identify the exact linearization point — the single instant
at which the operation is considered to have taken effect.

```text
Operation:          [e.g., submit_read]
Linearization:      [e.g., "the CAS that transitions slot from idle to pending"]
Observable before:  [what callers see before this point]
Observable after:   [what callers see after this point]
```

---

## 11. Wake/Progress Model

Per Gate 3 format. Answer every question. Do not leave "TBD."

---

## 12. Resource Bounds

Per Gate 2 format. List every resource with capacity, allocation timing,
failure mode, and reclamation.

---

## 13. OOM/Failure Semantics

For each allocation point:

```text
Allocation:     [what is allocated]
When:           [construction | submit | completion | reap]
OOM behavior:   [synchronous error | operation error | terminate | retry]
Rollback:       [what is unwound if this fails]
Invariant:      [what remains true after failure]
```

---

## 14. Cancellation

Which cancellation layer does this design interact with?

```text
Layer:          [task | wait | operation | syscall | admission]
Authority:      [who may cancel]
Terminal:       [possible terminal results]
Exactly-once:   [yes/no — mechanism]
Syscall effect: [interrupted | completes normally | best-effort]
```

---

## 15. Shutdown

What happens during each shutdown phase?

```text
Admission closure:  [what rejects new work]
Drain:              [what waits for in-flight to complete]
Join:               [what joins threads]
Destruction order:  [list in order]
Fail-fast:          [what triggers terminate]
```

---

## 16. API Compatibility

Does this change any public header under `include/sluice/`?

```text
Public API change:  [yes/no]
Breaking:           [yes/no — if yes, migration path]
ABI:                [layout change | vtable change | none]
Documentation:      [api-reference.md section to update]
```

---

## 17. Alternatives Rejected

List at least two alternatives considered and why they were rejected.

| Alternative | Reason rejected |
|-------------|-----------------|
| ... | ... |
| ... | ... |

---

## 18. Required Tests

Per Gate 4 format. List every test that will prove this design correct.

---

## 19. Architecture Constitution Checklist

For each constitution rule touched, state compliance:

| Rule | Compliant? | Evidence |
|------|-----------|----------|
| AC-1 | [yes/exception] | [brief] |
| AC-2 | ... | ... |
| ... | ... | ... |

---

## 20. Revisit Triggers

Under what conditions should this design be revisited?

```text
Trigger:            [e.g., "if outstanding ops exceed 1000 in production"]
Revisit action:     [e.g., "evaluate preallocated slot pool"]
Owner:              [who is responsible for monitoring]
```
