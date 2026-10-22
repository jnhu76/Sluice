# DP-0 — compiler-first data-movement audit (research)

Research Issue: linked to #563. RESEARCH ONLY — no product change, no API
change, no root amendment. Baseline master `be28f275`.

## Reading order

1. [decision.md](decision.md) — verdicts, seven final answers, report block.
2. [results/analysis.md](results/analysis.md) — what the instruments measured
   and which controls establish trust.
3. [experiment-protocol.md](experiment-protocol.md) — frozen protocol,
   amendments, matrices, binary hashes.
4. Candidate analyses: [dp1-publication.md](dp1-publication.md),
   [dp2-result-bridge.md](dp2-result-bridge.md),
   [dp3-slot-layout.md](dp3-slot-layout.md).
5. Audit base: [dataflow-dag.md](dataflow-dag.md),
   [field-liveness-matrix.md](field-liveness-matrix.md).
6. Compilation evidence: [compiler-inventory.md](compiler-inventory.md),
   [gimple-sra-dse.md](gimple-sra-dse.md),
   [generated-assembly.md](generated-assembly.md).
7. [hardware-attribution.md](hardware-attribution.md) — what the PMU can
   support here and what is INVALID.
8. [external-design-comparison.md](external-design-comparison.md) — open-world
   matrix with pinned sources.
9. [results/](results/) — raw JSONL receipts, calibration, mutation-oracle
   logs, assembly excerpts, harness sources.
10. [independent-review.md](independent-review.md) — reviewer disposition.

## One-paragraph summary

At -O3 without LTO, Sluice's cross-TU data plane physically executes three
removable pieces of per-op work: a 120-byte publication payload whose only
consumed field is a key the publisher already holds (DP-1), a 72-byte result
bridge whose 40-byte metadata half is unread by `size_t`-carried results
(DP-2), and a double slot re-initialization (DP-3A). Research arms removed
each independently; interleaved A/B matrices on a deterministic instrument
confirm the assembly-predicted per-op instruction deltas to within a few
instructions (−27.5 / −8.8 / −16.5, additive to −36.3 for the DP-1+DP-2B
combo), with second-order 0.9–11.9 % whole-loop reductions on poll-spinning
workloads. W-01 (blocking) is already clean. DP-3B (slot split) is dominated;
DP-3C (producer-side metadata gating) is sketched but not implemented. All
cache/TLB attribution is analytical — the host exposes no usable cache
counters and its cache-miss counters are marked INVALID.
