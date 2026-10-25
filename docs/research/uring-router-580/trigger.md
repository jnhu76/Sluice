# R2 — Trigger discovery and gate decision (WSL2 exploratory)

Instrumented binary: `build-research/lookup_probe` (sources compiled with
`SLUICE_URING_LOOKUP_INSTRUMENT` + `SLUICE_ASYNC_INTERNAL_TESTING`; counts only,
its timings are never performance evidence). Production-shaped attribution:
`build-research/router_ab_baseline{,_dbg}` — the same async sources compiled
**without** the instrumentation define (`-O3 -DNDEBUG`, `-g` only on the `_dbg`
attribution build, which does not change codegen). Fixture: tmpfs, per-run
unique path, byte-pattern oracle verified on every read. Pinning `taskset -c 2`.

Raw data: `results/trigger-matrix.jsonl` (2 reps per scenario, both included),
perf data retained at `/tmp/perf580_{b,c2,d}.data` (not committed; sizes
0.037-0.147 MB, SHA-256 in `results/manifest.json`).

## R2-A. Measured lookup frequency and probe distributions (real kernel)

Cells are the mean of both repetitions (WSL data; the E5 campaign in
`wsl-results.md` reproduces them structurally, widest cell ~1 probe apart).

| Scenario | lookups/op | op-CQE avg probes | P99 | S2 cancel avg probes | avg live at lookup | misses | installs=retires/op |
|---|---:|---:|---:|---:|---:|---:|---:|
| pipeline C16/D16 | 1.00 | 8.5 | 16 | — | 9.7 | 0 | 1.00 |
| pipeline C64/D16 | 1.00 | 8.5 | 16 | — | 9.7 | 0 | 1.00 |
| pipeline C1024/D1 | 1.00 | **1.0** | 1 | — | 1.0 | 0 | 1.00 |
| pipeline C1024/D16 | 1.00 | 8.5 | 16 | — | 9.7 | 0 | 1.00 |
| pipeline C1024/D64 | 1.00 | 33.7 | 64 | — | 37.5 | 0 | 1.00 |
| pipeline C1024/D16 64 KiB | 1.00 | 8.5 | 16 | — | 9.2 | 0 | 1.00 |
| pipeline C1024/D16 1 MiB | 1.00 | 8.6 | 16 | — | 9.4 | 0 | 1.00 |
| churn C1024 (64↔1) | 1.00 | 32.5 | 64 | — | 32.0 | 0 | 1.00 |
| cancel C64/D64 64 KiB r=0.2 | 1.60 | 32.5 | 64 | 32.3 | 34.4 | 0 | 1.00 |
| cancel C1024/D64 64 KiB r=0.2 | 1.60 | 32.5 | 64 | **992.4** | 34.5 | 0 | 1.00 |
| metadata C64/D16 (statx) | 1.00 | 8.5 | 16 | — | 8.5 | 0 | 1.00 |
| miss-inject C1024 (SEAM) | 10.99* | 930.9 (misses) | 1024 | — | 0.1 | 999 | 1.00 |
| miss-inject C16 (SEAM) | 10.99* | 14.6 (misses) | 16 | — | 0.1 | 999 | 1.00 |

\* miss scenarios replay 1000 stale cookies around 100 real ops; the ratio is a
construction artifact, the per-miss probe count is the measurement.

Structural findings (all real-kernel rows):

1. **The operation-CQE lookup is exactly 1.00/op in every steady workload** —
   one lookup per logical operation, hits only, no misses in any real-kernel
   run (2000-20000 ops per row × 2 reps).
2. **Average probes track D/2 and are independent of C.** C1024/D16 = 8.5
   probes = C16/D16; C1024/D1 = 1.0 probe. The LIFO free-list plus the
   reverse scan direction keep the hot path O(live), never O(capacity).
   Churn at 64↔1 keeps 32.5 avg (bounded by high-water D, still not C).
3. **The only capacity-scaled scan is `find_live_router_index_` (S2, by
   handle, forward)**: 32.3 probes at C=64 (D=64) but **992.5 probes per
   cancel at C=1024** — matching the R1 model (C−D+D/2). It fires once per
   running cancel; at 20% cancel mix it contributes ~198 probes/op at C=1024
   vs ~6.5 at C=64.
4. **Stale/late CQE misses are O(C) full sweeps** (930.9 avg at C1024,
   14.6 at C16) — confirmed only via the seam (marked SEAM); zero natural
   misses occurred in any real-kernel run.
5. Poison-recovery lookups (S6): zero occurrences in all runs (fault path,
   not exercised by healthy workloads); same reverse-scan family as S3/S4.
6. Control-CQE ratio at r=0.2 cancel mix: 2000 control / 12000 total CQEs;
   `mark_consumed` (S1) avg 32.5-32.9 probes (reverse, O(D)).

## R2-C. CPU attribution and the trigger gate

`perf record -e cycles:u` on the **uninstrumented** production-shaped runner,
symbols resolved from the binary's symtab (perf displayed raw offsets; they
were mapped through `nm` — sample counts 905-3802 per run; a ±0.8pp binomial
sampling interval on the 2.28% E5 share below still leaves it far from any
materiality threshold). The shares are measured against a harness-inclusive
denominator (the runner's wait/verify loop is part of the timed region in both
the perf and A/B views; identical across arms):

| Shape | `find_live_router_cookie_` share | S2 site (inside `issue_running_cancel_locked_`, same-TU inlined) | dominant symbol |
|---|---:|---:|---|
| pipeline C64/D64 4 KiB (production default, C==D) | **1.57%** | — | harness `run_pipeline` 68.2% (public wait/take spin) |
| pipeline C1024/D64 4 KiB (worst steady scan shape) | **1.69%** | — | harness `run_pipeline` 65.3% |
| cancel C1024/D64 64 KiB r=0.2 | <0.1% (below cutoff) | `issue_running_cancel_locked_` **0.48%** (contains the 992-probe forward scan) | harness `run_cancel_mix` 94.2% (kernel-bound waits) |

Counter totals (`perf stat`, C64/D64, 65k ops incl. warmup): 7,136 cycles/op
user, 29,421 instructions/op user. Attribution caveats, disclosed: WSL2 shows
16.2% of `cycles:u` samples at hypervisor/kernel addresses in the pipeline
shape (misattributed under `:u`; excluded from shares); `-F 1999`, 905-3802
samples per run; kernel-side time is not attributed (paranoid=2).

Gate metrics:

| Metric | Value (worst measured shape) |
|---|---:|
| `lookup_frequency_per_op` | 1.00 steady / 1.60 at 20% cancel mix |
| `average_probes` (hot op-CQE path) | D/2 (8.5 @ D16, 33.8 @ D64) — **independent of C** |
| `lookup_cost_per_op` (bottom-up, 1-2 cycles/probe hot) | ~35-70 cycles ≈ 11-22 ns @ D64 vs ~7,100 cycles/op user total |
| `lookup_cpu_share` (perf, user cycles) | 1.57% (default shape) … 1.69% (C1024/D64) |
| `lookup_cpu_share` (of total CPU, ×user/(user+sys)≈0.28) | ~0.44% |
| `theoretical_maximum_gain` (lookup made free) | <2% user cycles, <0.5% total CPU — **below the 5% materiality threshold at every measured shape** |

Cross-check with native E5 history (H1, #561): paired A/B there showed ≤3.7%
end-to-end CPU at C=1024 for a strictly better lookup; this round explains the
mechanism — at d=1 the scan is a single probe, and at all production depths the
scan is O(D/2) on hot cache lines. The WSL attribution and the E5 native
ceiling agree in order of magnitude.

## Gate decision

`NO_TRIGGER_NO_CHANGE` for secondary-index candidates (R3 Candidate A/B):

- No #578 real-app evidence exists (issue open, no baseline produced).
- E5 native admission is out of scope this round by protocol.
- WSL real-kernel probes show no material lookup hotspot at any measured
  shape: the hot-path scan is O(D/2) independent of C, 1 lookup/op, ~1.6% of
  user cycles at the worst artificial shape, ~0.4% of total CPU.
- The only C-scaled costs are (a) the forward by-handle cancel scan — a scan
  **direction** accident relative to LIFO layout, eliminable without any new
  state (R1 WE-2); (b) stale-CQE full sweeps, which occurred zero times
  naturally in 100k+ measured real-kernel completions.

Per the issue's stop rule ("Stop if average probes are already ~1 or
incremental maintenance outweighs lookup savings" / hotspots only in synthetic
C=1024 worst cases do not admit product candidates), R3's index designs are
**not** implemented. The mechanism-level work-elimination findings (WE-1/WE-2)
are carried to R4 as analysis, and WE-2 is additionally exercised as a
`EXPLORATORY_ONLY / NOT_ADMITTED` A/B in R6 to answer the "does any change
reduce total lifecycle work" question with measurements rather than assertion.
