## Summary

<!-- One paragraph: what changed, why now, and the governing issue/contract. -->

## Governing issue / lifecycle

- Issue: #<!-- number -->
- Lifecycle before implementation: <!-- ACTIVE / READY->ACTIVE / ON-TOUCH / child of UMBRELLA -->
- [ ] This PR does not implement work from a DEFERRED issue without an explicit reopen/promotion record.
- [ ] Meaningful residual scope is re-tracked before any closing keyword is used.

## Change classification

- [ ] Bug / correctness fix
- [ ] Security fix
- [ ] New feature
- [ ] Refactor (intended no behavior change)
- [ ] Formal/model change
- [ ] Performance change
- [ ] Documentation / governance only
- [ ] Build / CI / tooling
- [ ] Test only

## Scope and causal evidence

<!-- State what is in scope and explicitly out of scope. For bug/race/liveness work, give the causal evidence rather than only the symptom. -->

### Pre-fix evidence (required for bug/correctness claims when feasible)

```text
command / deterministic schedule / static invariant violation:

result:
```

### Post-fix evidence

```text
same causal schedule or directly corresponding regression:

result:
```

- [ ] The regression can fail on the pre-fix behavior, or the PR explains why executable pre-fix RED is infeasible.
- [ ] Timing sleeps/retries/skips are not being used as correctness proof.

## Architecture impact

**Does this PR affect an architecture boundary?**

Examples: async I/O ownership, RequestKey/RequestSlot lifecycle, Completion publication, backend submission/dispatch/reap, cancellation, Scheduler wake/progress, synchronization primitives, capacity/bounds, Runtime ownership, public async API, shutdown/drain, io_uring ownership.

- [ ] **No** — no architecture boundary is changed.
- [ ] **Yes** — architecture compliance is required and the governing design/gate is linked below.

### Architecture gate (required only if Yes)

- Design / ADR / compliance gate: <!-- link -->
- Applicable AC-N rules: <!-- list -->
- [ ] Capability/layer/authority is identified.
- [ ] State machine and ownership transitions are documented.
- [ ] Lock/atomic authority and observable intermediate states are documented.
- [ ] Submit failure remains transactional where applicable.
- [ ] Completion/publication authority is unchanged or explicitly approved.
- [ ] Wake/progress obligations and fairness assumptions are documented.
- [ ] Capacity/resource bounds and overload behavior are documented.
- [ ] Cancellation layer/winner authority is identified.
- [ ] Shutdown/drain semantics are documented.
- [ ] Intentional divergence is recorded rather than hidden.

## C++ ↔ formal evidence (only if formal/model claims are involved)

- [ ] Current/repaired **as-built C++** state machine was recovered before finalizing the repository model.
- [ ] Each load-bearing TLA+ action names the C++ region/lock/atomic sequence it represents.
- [ ] C++ intermediate states observable by competitors are not incorrectly fused into one atomic model step.
- [ ] Pre-fix/mutant behavior produces the intended CEX or the PR explains why that negative control does not apply.
- [ ] Repaired as-built configuration passes the stated properties/non-vacuity witnesses.
- [ ] Formal verifier is fail-closed for parse/config/runtime failures.
- [ ] The PR does **not** claim "C++ implementation formally verified" solely because TLC/model checking is green.

### Formal commands/results

```text
<!-- exact commands actually run -->
```

## Testing and validation

<!-- Report only commands actually executed. "All tests pass" is insufficient. Mark N/A with a reason rather than checking an unrun gate. -->

### Required baseline / focused evidence

```text
<!-- exact commands + totals/results -->
```

### Additional applicable gates

- [ ] Release
- [ ] ASan + UBSan
- [ ] TSan
- [ ] Real liburing / backend conformance
- [ ] Negative compile
- [ ] Formal positive gates
- [ ] Formal negative controls / witnesses
- [ ] Repository mechanical / pre-push gates
- [ ] `git diff --check`

Unrun / not-applicable gates and reason:

```text
<!-- gate: reason -->
```

## Performance / cost of correctness

**Does this PR change a scheduler/atomic/hot path, allocation behavior, queue/capacity path, or algorithmic class?**

- [ ] No material performance-sensitive path changed.
- [ ] Yes — A/B evidence is provided below.

```text
workload/environment:
baseline:
candidate:
median/range:
interpretation:
```

- [ ] Correctness/output equivalence was checked before performance comparison.
- [ ] Noise/outliers are reported honestly; no unsupported Core attribution is claimed.

## Residual risks / follow-ups

<!-- List genuine residuals, accepted limitations, unknowns, and follow-up issues. "None" is acceptable only after checking. -->

- Residual:
- Follow-up issue:
- Deferred/reopen trigger (if applicable):

## Final checklist

- [ ] `git diff --stat` / final diff reviewed; only intended files changed.
- [ ] No unrelated formatting or cleanup.
- [ ] Comments explain authority/invariants/why, not narration.
- [ ] Public/architecture documentation updated when contracts changed.
- [ ] Historical docs/issues were preserved rather than rewritten to hide prior state.
- [ ] PR title/body accurately describe all material scopes actually present in the diff.
