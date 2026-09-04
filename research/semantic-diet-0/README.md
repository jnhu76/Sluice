# SEMANTIC-DIET-0 — remove speculative semantic surface before thesis rewrite

Bounded semantic-diet campaign between the G1-Control verdict ([#283], merged
PR #284) and the [#227] thesis rewrite.

**Verdict: PASS — SPECULATIVE SEMANTIC SURFACE REMOVED** (one K5 cluster;
everything else independently justified).

## Question

> Does Sluice's public / architectural / runtime semantic surface contain
> semantics kept mainly for hypothetical future Control / specialization that
> no current correctness, lifetime, ownership, boundedness, product behavior,
> or earned capability requires?

## Deliverables

```text
SEMANTIC-DIET-0-AUDIT.md       reality audit + 30-row inventory + K1-K6
                               classification + evidence + STOP decisions
SEMANTIC-DIET-0-REPORT.md      final report + closure questions Q1-Q8
SEMANTIC-DIET-0-SUMMARY.json   machine-readable verdict + keep/remove lists
```

## Outcome

Removed: the `CopyStrategy` deferred-slot cluster
(`VectorDeferred` / `FileRangeDeferred` / `SendfileDeferred` / `SpliceDeferred`
+ `UnsupportedStrategyPolicy` + deferred option/decision/stats fields + test/
fuzz/example/doc surface). These were reserved for future copy mechanisms
(vector copy, copy_file_range, sendfile, splice) that are not implemented and
not earned (COPY-X0: STOP, NO C1). The strategy-specific mechanisms were
unimplemented, but the reservation created a real public contract — a deferred
request observably returned `invalid_state` or explicitly fell back to Auto
(with decision/stats observation). Removal is an intentional public
source-surface reduction, accepted and disclosed on the tag-only v0.0.1
baseline (no declared stability promise); "no break" is not claimed.

Kept: all correctness, identity, lifetime, boundedness, cancellation, shutdown
and natural-product semantics (K1/K2/K3) and all mechanism seams with real
consumers (K4). The retained Auto/Scratch/BufferedFirst copy behavior and
CopyDecision observability are unchanged. No G1-Safety experimental target is
touched. Control specialization remains CLOSED BY DEFAULT (governance default,
not a runtime toggle).

## Rules honored

- Strong G1-Control remains retired; this campaign does NOT reopen Control,
  does NOT rewrite #227, and does NOT claim G1-Safety.
- Proof burden on deletion (D1–D7); adversarial checks A1–A6; STOP GATE A/B
  applied. Negative result was an allowed, fully acceptable outcome.

[#227]: https://github.com/jnhu76/Sluice/issues/227
[#283]: https://github.com/jnhu76/Sluice/issues/283
