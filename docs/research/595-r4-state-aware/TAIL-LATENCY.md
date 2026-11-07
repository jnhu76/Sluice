# #595 R4 tail-latency data (p50 / p95 / p99)

Purpose: isolate what the candidate submission policies do to the request
latency distribution, independently of the CPU/throughput gains reported in
RESULTS.md. This file is generated from the same raw blocks the frozen
analysis used; the CSV and the extractor are committed next to it.

- Data: `analysis/tail-latency-p50-p95-p99.csv` (42 rows, one per cell x arm).
- Regenerate: `python3 analysis/extract_tail_table.py` (reads the campaign
  raw tree on the E5 server; paths inside the script).
- Metric level: invocation-level `p50_ns/p95_ns/p99_ns` — the exact level the
  preregistered gates consume (5 measured reps x 20,000 ops per invocation,
  24 matched blocks per arm unless noted). Deltas are paired per-block
  `(candidate/A0 - 1)` percentages, arithmetic mean over blocks; bracketed
  ranges are min..max over the 24 blocks.
- Positive delta = worse than A0. Negative = better.

## 1. Main shape W03-uring-4k-d16 (MAIN-W03-D16)

A0 absolute baseline: p50 = 36.86 us, p95 = 45.12 us, p99 = 51.94 us.

| arm | d p50 | d p95 | d p99 | p99 block range | d CPU/op | d tput |
|-----|-------|-------|-------|-----------------|----------|--------|
| R | +4.75 | +2.71 | **+21.85** | +6.4 .. +32.6 | −19.62 | +24.53 |
| Q8 (fixed 8) | +5.63 | +4.02 | **+23.33** | +9.1 .. +32.4 | −19.23 | +23.94 |
| SHADOW (R + hint reads) | +5.74 | +4.59 | **+23.31** | +5.0 .. +32.1 | −18.83 | +23.31 |
| SNP (S minus pressure hint) | +7.08 | +12.28 | **+34.48** | +12.1 .. +71.9 | −16.05 | +19.22 |
| S | +13.03 | +33.68 | **+34.57** | +25.3 .. +46.9 | −12.81 | +14.75 |
| SNI (S minus inflight hint) | +12.07 | +32.95 | **+34.19** | +22.4 .. +84.1 | −13.85 | +16.12 |
| Q2 (fixed 2) | +11.98 | +33.42 | **+36.24** | +23.1 .. +72.3 | −13.51 | +15.68 |

Readings:

1. R's regression is a **p99-only phenomenon at this shape**: p50/p95 stay
   within ~5% while the 99th percentile moves +22% and is positive in 24/24
   blocks. Batching at 8 moves ~1% of requests behind something; the median
   body is untouched.
2. S is statistically a **quantum-2 policy** (see counters below), and its
   whole distribution shifts right (p50 +13%) — consistent with Q2.
3. SHADOW tracks R within ~1.5pp on every percentile: reading the two hints
   is not what costs latency; the batching action is.

## 2. What each arm actually decided (MAIN-W03-D16, mean counters per invocation, ~50,000 decisions)

| arm | presentations | quantum=2 | quantum=8 | cap reentries | pressure reads | active-zero reads |
|-----|---------------|-----------|-----------|---------------|----------------|-------------------|
| R | 12,541 | — | — | 6,170 | — | — |
| Q8 | 12,535 | 0 | 12,518 | 0 | — | — |
| SHADOW | 12,534 | 12,517* | 0 | 6,156 | 12,524 | 6,285 |
| SNP | 19,172 | 6,630 | 12,523 | 0 | 0 | 6,631 |
| S | 50,002 | 49,998 | 0 | 0 | 49,962 | 6,692 |
| SNI | 49,976 | 49,954 | 10 | 0 | 49,966 | 0 |
| Q2 | 50,005 | 49,995 | 0 | 0 | 0 | 0 |

(*) SHADOW records the quantum a state-aware policy *would* pick while acting
as R.

Key observation — **SNP**: it mixes 6,630 quantum-2 presentations (driven by
`outstanding()==0`) with 12,523 quantum-8 ones, i.e. only ~35% of its
presentations are small, yet its p99 (+34.5%) lands at S level, far above
uniform-8 Q8 (+23.3%). The zero-outstanding moments are not random: they
coincide with drain/quiet phases, and shrinking the restart batch there
appears to hit exactly the requests that become the tail. This is the
strongest in-data signal that the state hints fire at the wrong moments for
tail latency, not merely with the wrong magnitude.

## 3. Depth sweep (W03 caller, R and S vs A0)

| shape | arm | d p50 | d p95 | d p99 (range) | d CPU/op | d tput |
|-------|-----|-------|-------|----------------|----------|--------|
| d2serial | R | +6.96 | +7.61 | +7.17 (−20.8..+23.6) | +6.94 | −6.47 |
| d2serial | S | +9.87 | +10.37 | +11.61 (+0.5..+33.3) | +11.57 | −10.36 |
| d4 | R | +15.17 | **−20.54** | **−12.92** (−21.7..−3.1) | −14.21 | +16.65 |
| d4 | S | +3.23 | −6.08 | **+18.39** (+12.9..+23.7) | −16.00 | +19.12 |
| d16 | R | +4.75 | +2.71 | **+21.85** (+6.4..+32.6) | −19.62 | +24.53 |
| d16 | S | +13.03 | +33.68 | **+34.57** (+25.3..+46.9) | −12.81 | +14.75 |
| d64 | R | +4.90 | +0.92 | **+21.17** (−15.8..+36.7) | −21.33 | +27.31 |
| d64 | S | +16.63 | +13.78 | **+25.06** (−4.0..+77.9) | −11.70 | +13.41 |

At d4 batching *improves* p95/p99 for R; the p99 regression appears from d16
up and its block-level spread widens at d64. There is a concurrency/batch-
opportunity threshold below which co-submission helps the tail and above
which it hurts it.

## 4. Other callers / filesystems / request sizes

| cell | arm | d p50 | d p95 | d p99 (range) | note |
|------|-----|-------|-------|----------------|------|
| CORE-W02-D1 | R | +4.01 | +2.97 | +5.25 (−4.3..+27.6) | sparse pipeline |
| CORE-W02-D1 | S | +4.93 | +3.77 | +5.41 (−1.7..+25.5) | |
| CORE-W02-D2 | R | +3.46 | +2.78 | +2.80 (−10.6..+14.1) | |
| CORE-W02-D2 | S | +5.16 | +4.09 | +5.80 (−6.9..+20.7) | |
| CORE-W02-D16 | R | +2.18 | +1.75 | +1.83 (−5.1..+11.7) | |
| CORE-W02-D16 | S | +7.25 | +6.76 | +6.61 (+0.7..+17.0) | |
| PROT-NONTMPFS (btrfs) | R | +0.67 | **−28.17** | **−30.43** (−35.4..−19.6) | CPU −54.5%, tput +120% |
| PROT-NONTMPFS (btrfs) | S | **+108.12** | +23.13 | +19.45 (+13.1..+38.5) | CPU −24.1%, tput +31.9% |
| PROT-64K-D16 | R | +3.87 | +3.21 | +3.35 (−24.3..+29.6) | all arms eligible=noop (immediate path) |
| PROT-64K-D16 | S | +4.50 | +1.75 | +3.77 (−23.9..+28.7) | |

On the slower completion path (btrfs, warm buffered) the sign flips for R:
fewer, larger submissions improve every percentile while CPU drops by half.
S on the same shape doubles the median (+108%) — under slow completions the
pressure hint fires even more often and quantum-2 starves the pipeline.

## 5. R vs R3's B (R3_FIXED_PREFIX, suffix singles), W03-d16, 24 matched blocks

Paired (R−B, negative = R better): p50 −7.77%, p95 −24.06%, p99 −12.44%
(range −27.1..−5.5), CPU/op −8.18%, tput +9.07%. Cap docking plus suffix
singles (B) is worse than suffix re-entry (R) on every percentile. Descriptive
only (no A0 in this cell; the RESULTS.md gate numbers remain authoritative).

## 6. Mechanism hypotheses for the R p99 regression (unresolved)

In-data facts any explanation must cover: p50/p95 near-flat, p99 +22% and
24/24 positive at d16; d4 improves the tail; d64 widens the spread; btrfs
improves everything; SNP's rare quantum-2 firings land at S-level p99.

Candidates, in the order RESULTS.md preregisters for a follow-up:

1. **Completion-order coupling** — members of a batch complete (and are
   harvested) together; the last-ready member of each 8-batch waits for the
   whole batch, and that wait lands past p95.
2. **Cap-truncation restart cost** — stop-at-cap plus suffix re-entry adds a
   presentation boundary on refills; drain-phase restarts pay it.
3. **io-wq / wakeup interaction** — larger, less frequent submissions change
   who wakes whom; 1% of wakeups may take a slow path.

Checks available without new measurement: per-rep `lat_p50/p95/p99_ns`,
`lat_max_ns`, `notifications`, `passes`, `r4_*` counters in the committed
jsonl paths (EVIDENCE-INDEX.md); F2/F5 diagnostics for decision-timing.

## 7. Known data discrepancy (does not affect this file)

`figures/figure-data.csv` has two documented errors found by the independent
numeric review (F5 state rows T1/T2 mean_ready/mean_present copied from T3,
and two quantum2_share rows labeled fraction but holding percentages). The
F5 figure itself and RESULTS.md §8 are correct; see RESULTS.md review notes.
The CSV in `analysis/` here is generated independently by
`extract_tail_table.py` and does not share that code path.
