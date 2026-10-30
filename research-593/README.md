# research-593 — PocketJS-Inspired Execution-Path Slicing (Method Pilot)

Issue: jnhu76/Sluice#593. Status: CLOSED. **Method design only — the pilot
R0–R4 was never executed; no experimental data exists.**
`EXECUTION_STATUS = NOT_RUN`, `RESEARCH_RESULT = NOT_PLANNED_CURRENT_PHASE`.

## Research Question

Can PocketJS-style execution-path slicing — reconstruct the full request
path, observe it with attribution instrumentation, and replay/slice it into
elision candidates — identify mechanisms on Sluice's real critical path that
perform no semantic obligation, such that removing them beats accelerating
them?

## Structure (the retained method)

- **Phase A — Full execution-path reconstruction**: map the mechanism graph
  of one request across four lanes —
  K1: CQE → terminal → publication → observer → `take`;
  K2: `RequestScope` submit → admission → dispatch → kernel SQE;
  K3: progress pass → wakeup → bounded retry;
  K4: SQE/CQE batching and the kernel boundary.
- **Phase B — Native full-path measurement**: build a Full binary and an
  Observe binary. **The Full binary is the performance authority; the
  Observe binary is for attribution only** — its counters gate off to zero
  cost when absent, and test-only replay performance must never be treated
  as real kernel I/O performance.
- Counterfactual elision: for each mapped mechanism, remove it in a
  test-only arm and measure; a mechanism with no semantic obligation and no
  measured regression becomes an elision candidate.

## Code References

- Issue #593 and its design brief (E5 `~/sluice-593-research/brief.md`,
  retained in place; not reproduced here). Methodology deliberately
  inherited from `docs/performance/methodology.md` and the existing #591
  hardware-aware tooling — no second benchmark spec was invented.
- Context issues: #563 (minimal necessary work), #591 (hardware-aware
  benchmark), #580 (CQE router lookup, closed `NO_TRIGGER_NO_CHANGE`).
- Successor: the method's Phase A/B was realized natively by the **#594
  K1–K4 campaign** (see research-594), which is the authority for all four
  lanes' outcomes. The #593 pilot itself produced no data and no code.

## Conclusions

- Nothing was measured; no slicing result, replay trace, or candidate list
  exists, and none may be fabricated.
- The issue closed with the method handed over to #594's direct campaign:
  reconstruct-and-observe was subsumed by reconstruct-and-counterfactually-
  elide on native E5 hardware.
- Durable rule validated by the successor: attribution instrumentation must
  be structurally incapable of becoming the measured configuration
  (Full/Observe split), or its overhead contaminates the verdict.

## Next Steps

If path-slicing is revisited: do not build a replay harness first. Go
straight to the #594 pattern — per-lane mechanism map, Observe-only
instrumentation, counterfactual elision arms, paired native measurement.
The unsolved methodological gap #594 hit was K1's missing mutation-based
correctness receipt; any future slicing campaign should define the per-lane
mutation oracle before measuring.
