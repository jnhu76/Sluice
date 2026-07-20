# E13 Select Formal Safety Layered Design

**Task:** `E13-SELECT-FORMAL-MODEL-SAFETY-1` (PR #18)
**Builds on:** PR #17 (`E13-SELECT-FORMAL-MODEL-CORE-1`)

## Decision

PR #18 closes the safety foundation of the E13 Select formal core.  It
keeps the three-layer architecture from PR #17 stable and adds, alongside
the PR #17 canonical aggregates, the full named layered safety invariant
sets, focused single-fault negative models, a per-law non-vacuity witness
matrix, a bounded two-group non-interference model, and a reproducible
source-safe verifier.

```text
E13SelectContract           (Layer C)  ContractSafetyInv  + NEG-C1..C8
        ^ refinement (PR #17 mapping, unchanged)
E13SelectCentralClaim       (Layer S)  CentralSafetyInv   + NEG-S1..S6
        ^ refinement (PR #17 mapping, unchanged)
E13SelectEventTimer         (Layer A)  AdapterSafetyInv   + NEG-E1..E6
                                                          + NEG-T1..T5
                                                          + NEG-A1..A4
        ^
E13SelectMultiGroup         (bounded O) MGSafetyInv      (direct model)
```

The PR #17 canonical `*Inv` aggregates (`ContractInv`, `CentralInv`,
`EventTimerInv`) are preserved unchanged side-by-side with the new
`*SafetyInv` aggregates so PR #17 metrics still reproduce exactly.

## Scope

PR #18 is formal-specification only.  It adds:

1. **Layered safety invariants.**  `ContractSafetyInv` (32 laws, H1-H5 + I),
   `CentralSafetyInv` (14 laws, J), `AdapterSafetyInv` (K + L + M + N, 42
   laws).  Each law is named, scoped, and commented in the spec.

2. **Refinement safety.**  The PR #17 INSTANCE-WITH mappings remain
   unchanged.  PR #18 adds a widened-domain refinement check
   (`E13SelectCentralClaim.refine3.cfg`) and documents that adding the M
   accounting and N history variables does not affect the mapping.

3. **Focused negative models.**  29 single-fault mutations, one per named
   law, plus three FAULT="None" restoration configs.  Each fault is
   reachable from a legal state, mutates exactly the variables its target
   law constrains, and is reported by TLC as `Invariant <LawName> is
   violated`.

4. **Non-vacuity evidence.**  20 inverse-reachability predicates proving
   each named law's premise is reachable in the canonical model.

5. **Bounded multi-group non-interference.**  `E13SelectMultiGroup.tla`
   composes two 1-arm SelectGroups sharing a single Event identity space
   and proves each group's winner, publication, classification, authority
   closure, and TimerRegistration are isolated.

6. **Repeatable verification tooling.**  `tools/formal/verify-e13-select-safety.sh`
   runs the full PR #18 suite in an isolated mktemp workspace with a
   defensive cleanup trap.

## Architectural decisions

### A1 — Side-by-side aggregates, not replacement

The new `*SafetyInv` aggregates are *additional* operators, not
replacements for the PR #17 canonical `*Inv`.  This means:

- PR #17 canonical metrics (state count, search depth, runtime) reproduce
  exactly on the unchanged cfgs.
- A regression in the canonical model cannot be silently masked by an
  unrelated strengthening.
- The new laws can evolve independently of the canonical well-formedness
  checks.

### A2 — INSTANCE-WITH wrapper for negative models

Each negative module wraps the canonical spec by INSTANCE-WITH.  The
canonical `*Next` is reused unchanged via `BaseNextFrozen`, and the
wrapper's Next disjoins it with ONE focused fault selected by the constant
`FAULT`.  This guarantees:

- The canonical spec is never modified.
- Each fault is the only mutation in its run.
- Restoration (`FAULT = "None"`) is just the canonical model, proving the
  fault is the sole cause of any violation.

### A3 — Reachability witnesses as inverse invariants

For each law `L` whose premise is `P`, we add `Reach_<X> == P` and
`NotReach_<X> == ~P`.  A TLC run with `INVARIANT NotReach_<X>` reports
`Invariant NotReach_<X> is violated` iff `P` is reachable — a clean,
non-vacuous witness that uses only TLC's invariant mechanism (no separate
liveness or temporal property machinery).

### A4 — M and N as pure extensions

The per-arm accounting counters (M) and step-indexed history (N) are
*additive* state: they shadow canonical variables but never replace them.
The PR #17 WITH refinement clause is unaffected by their addition, so the
temporal `RefinesCentralClaim` / `RefinesContract` properties still hold.

### A5 — Bounded two-group model, not unbounded concurrency

`E13SelectMultiGroup.tla` models exactly two groups (Groups = {g0, g1})
sharing one Event identity.  It is a bounded non-interference proof, not
an unbounded concurrency or liveness proof.  It does not claim
cross-Scheduler correctness.

## Boundaries (what this PR does NOT do)

- **No production C++** under `include/`, `src/`, `tests/`, `examples/`,
  `benchmarks/`.
- **No public API change.**
- **No production build or CI policy change.**
- **No `Waiting -> Running` silent fix action.**  Post-suspension
  cancellation is not modelled (PR #17 boundary preserved).
- **No alternative strategy** (Candidate B etc.).  Central Claim remains
  the only strategy.
- **No liveness / fairness.**  Safety only.
- **No unbounded concurrency proof.**  Bounded two-group only.

## Deferred

The following are explicitly deferred beyond PR #18:

- Production C++ implementation of Event/Timer Select.
- Post-suspension cancellation protocol (real wake/publication).
- Unbounded multi-group concurrency proof.
- Alternative strategies beyond Central Claim.
- Liveness / fairness gates.
