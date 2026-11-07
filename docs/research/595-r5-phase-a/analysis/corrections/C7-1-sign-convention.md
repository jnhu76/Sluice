# C7-1 — Unify R−B / B−R and throughput sign conventions

Status: correction record for R5 synthesis. The R4 archive is NOT modified
(no retroactive edits). Verified on 2026-11-06 from the frozen files cited
below.

## What the frozen code actually computes

`r4/harness/analyze_pairs.py:5-11` (frozen R1/R2 orientation, quoted):

```
Orientation matches the frozen R1/R2 convention (harness/analyze.py):
positive means the second member / B arm is worse.
  cpu/p50/p95/p99/ru: (m2 - m1) / m1
  tput:               (m1 - m2) / m1   (positive = second member slower)
```

`r4/scripts/matrix_analysis.py:6-9` (main matrix, same orientation):
`cpu/p95/p99 : (X - ref) / ref`, `tput : (ref - X) / ref`, "positive = worse".

`r4/scripts/bsupp_descriptive.py:36-43` (B supplement) applies exactly this
orientation with m1=R, m2=B:

- `paired()` = `(B - R) / R` for cpu/p95/p99  → JSON key container
  `paired_diff_B_minus_R_pct` (accurate for these three);
- `d_tput = (R - B) / R` → stored in the SAME container under the member
  name `tput_drop` (bsupp_descriptive.py:41-43,58). So one container key
  ("B_minus_R") holds three B−R members and one R−B member.

## Where the presentation contradicts the computation

1. `r4/RESULTS.md:188` row label:
   `| R−B（补充块，正=B 比 R 差） | +9.04% / +10.25% | +9.23% | +32.17% / +34.81% | +14.50% / +16.68% |`
   The label "R−B" reads algebraically as (R−B)/R, under which positive =
   R worse. The stored numbers are (B−R)/R for cpu/p95/p99 (positive = B
   worse) and (R−B)/R for throughput (positive = R faster). The
   parenthetical "正=B 比 R 差" is semantically true for all four numbers,
   but no single formula labelled "R−B" reproduces them.
2. `r4/RESULTS.md:394` (§16.1): "R−B 的 CPU 为 +9.04%（上界 +10.25%，正 = B 比
   R 差）、吞吐 +8.21%…" — same label defect; the throughput point +8.21%
   comes from `d_tput` mean 8.2123 with one-sided upper 9.2263
   (`r4/raw/bsupp/B-SUPP-W03-D16/DESCRIPTIVE.json`, `tput_drop`), i.e. it is
   R's throughput advantage, not a B−R quantity.
3. Figure axis note: `r4/figures/figure-data.csv` F3 rows use
   "positive = first-named worse" with series "R-B"
   (`r4/scripts/make_figures.py:231,245-247`), a third wording for the same
   direction. The figure-data VALUES are (B−R)/R for cpu/p99 — consistent
   with the script — but the "first-named worse" gloss over a label that
   names R first again invites the same misreading.

No numeric value is wrong anywhere; only the direction labels/keys are
self-contradictory.

## Corrected unified presentation (before/after, minimal)

Before (RESULTS.md:188 row label):

```
| R−B（补充块，正=B 比 R 差） | ...
```

After (one consistent convention; recommended wording for R5 documents):

```
| B−R（补充块；cpu/p95/p99 = 100·(B−R)/R，正 = B 更差；吞吐 = 100·(R−B)/R，正 = R 更快） | ...
```

Or, expressed from R's perspective under the main-matrix reading
(CPU/latency negative = better, throughput positive = better):

| quantity | formula | value (point / one-sided 95) |
|---|---|---|
| R−B CPU | 100·(R−B)/B_equivalently −(B−R displayed as B−R/R) | −9.04% / (B−R upper +10.25%) |
| R−B p95 | −(B−R p95) | −32.17% / −28.97%… (B−R CI [28.97, 35.33]) |
| R−B p99 | −(B−R p99) | −14.50% / (B−R CI [12.29, 17.17]) |
| R vs B throughput | 100·(R−B)/R | +8.21% / one-sided upper +9.23% |

Rule adopted for R5: every cross-arm table must state the formula per
metric family (cost: `(X−Y)/Y`, throughput: `(Y−X)/Y`) next to the row
label, or use a single "positive = second-named worse" convention with the
pair order in the label matching the formula's operand order. A bare
"A−B" label with a mixed-metric row is prohibited.
