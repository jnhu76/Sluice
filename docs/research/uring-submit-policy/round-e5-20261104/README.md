# uring-submit-policy — E5 native round, 2026-11-04 (bounded progress submission)

Research archive of one completed, pre-registered experiment round. It records
what was designed, what actually ran, what the numbers were, why the frozen
verdict is `KEEP_IMMEDIATE`, and how an independent reviewer can recompute every
decision statistic from the committed data alone.

- Issue: `jnhu76/Sluice#595` (shape-aware submission policy)
- Frozen base: `271b81bfc9d64eecc0266fe7066b9040525dd710` (== #622 anchor)
- Host: E5-2666 v3 bare metal, kernel 7.2.11-200.fc44, warm tmpfs (`platform.txt`
  copy in the full archive; identity in [protocol.md](protocol.md))
- Experiment execution: isolated worktree
  `/home/jnhu/sluice-595-e5/worktree` (branch `research-595-bounded-progress`,
  never pushed), arm selected by `SLUICE_595_ARM` in one binary per bench program
- Product change: **NONE**. Public API change: **NONE**. Merged as product: **NO**.

## Verdict

**`KEEP_IMMEDIATE`** — `PRODUCT = HOLD`, `PRODUCT_PR = NONE`, `MERGED = NO`.

The bounded-progress mechanism is real and large on the target shape (W-02
4 KiB d16: whole-executor CPU/op −26.46 % [−26.89, −26.02], throughput +36.07 %,
p99 −29.23 %, enters/op 3.0002 → 0.5630), and every negative-control gate holds
except **D2** (depth 2): p95 +5.69 %, p99 +7.45 %, throughput −3.99 % — each
beyond its frozen performance-protection bound, replicated, with a syscall-level
explanation (enters unchanged at 3.0002: one held request per pass with zero
amortization). D2's CPU +4.17 % is *within* the frozen 5 % CPU bound; the
protection failure is triggered by the tail-latency and throughput conditions.
**A supported mechanism is not a deployable policy**: D3, D16 and saturation
cannot outweigh the D2 protection failure under the frozen gates. The immediate
path stays the default.

## Documents

| File | Responsibility |
|---|---|
| [protocol.md](protocol.md) | Frozen hypothesis, measurement design, A/A calibration failure and the single sanctioned repair, exclusion rules, negative-control family membership, frozen decision gates |
| [mechanism.md](mechanism.md) | The B-arm predicate and flush points as built (from the patch), progress guarantees, correctness oracles and mutants, the D2 failure mechanism |
| [results.md](results.md) | Every measured cell with point estimates, CIs, absolute values, power-limited items, mechanism ledger, PMU, resource gate |
| [decision.md](decision.md) | Gate-by-gate adjudication against the frozen thresholds, the frozen verdict block, why this is not a product candidate |
| [limitations.md](limitations.md) | Single host, tmpfs, uninstrumented submit delay, enter submit/wait scope, accounting boundary, backup locality |
| [reproduction.md](reproduction.md) | Rebuild on E5, offline statistical recompute from committed data, hash verification |
| [evidence-index.tsv](evidence-index.tsv) | Claim → artifact → sha256 → original E5 location |
| [provenance.json](provenance.json) | Machine-readable source/binary/patch/dataset/analysis-script identity |

## Artifacts (bench side)

`bench/research/uring-submit-policy/round-e5-20261104/` holds the research
patch, build record, the verbatim pre-registered protocol, the analysis
scripts, all decision-bearing raw pair data, correctness receipts and the
mechanism ledger. See its
[README](../../../../../bench/research/uring-submit-policy/round-e5-20261104/README.md)
for the map, and `FULL_ARCHIVE.md` there for the Layer-A full-evidence record.

## Prior work this round builds on

- #593/#594 key-path round: located the per-dispatch inline flush
  (`io_uring_transport.cpp` dispatch path) and measured K2/K4 counterfactuals;
  its K4-b shape (owner gate + `outstanding > 1`) is the direct ancestor of this
  round's arm B. Those numbers are priors, not evidence for this round.
- #622 E5 observability round: attribution-grade proof that the product submit
  shape is structurally 1-SQE/enter, plus the io-wq/eventfd cost map handed to
  #595 on 2026-11-03.

## Next research entry

[RQ1–RQ4 in decision.md](decision.md#next-research-entry) — batchability
admission (RQ1) is the leading candidate; all four are `FUTURE_HYPOTHESIS /
NOT_TESTED`.
