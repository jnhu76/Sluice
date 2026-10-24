# R5 — Native E5 Performance Calibration

## Measurement infrastructure (frozen)

- **Runner:** `scripts/run_575_ab.sh` (v3, committed before any A/B
  measurement). Per repetition it rotates config order and arm order; each
  invocation runs the product benchmark (`bench_w02_pipeline` /
  `bench_w03_external_loop` / `bench_w01_direct`) with one case selected
  (`SLUICE_BENCH_CASE_FILTER` exact match), `WARMUP=2 MEASURED=1`, pinned
  via `taskset`, wrapped in `perf stat -x,` and `/usr/bin/time -v`. The
  benchmark's built-in byte oracle runs inside every invocation; any failure
  aborts the campaign (fail-closed).
- **Metrics per invocation:** bench-internal `wall_s`/`ops_per_sec` (primary
  wall/throughput), perf `instructions`/`cycles`/`context-switches`/
  `cpu-migrations` (kernel+user), time-v `user`/`sys`/max-RSS/voluntary+
  involuntary context switches, CPU frequency before/after, loadavg, binary
  SHA-256. All rows preserved verbatim in `ab.jsonl`.
- **Reduction:** `scripts/reduce_575.py` — paired per-rep ratios, median
  ratio, per-pair direction counts, bootstrap 95% CI of the median ratio
  (10 000 resamples, fixed seed), median absolute difference. Self-tested
  against 5 synthetic fixtures (exact-ratio, alternating-direction,
  missing-rep drop, zero-value drop, group isolation) — all pass
  (`--selftest`). No imputation of missing pairs, no exclusion of
  inconvenient repetitions.
- **Pinning:** io_uring/W-01 → cpu2; TP W1 → cpu2; W4 → cpus 2-5; W8 →
  cpus 2-9. Both arms of every pair use the identical pin set.
- **PMU:** `perf_event_paranoid` was relaxed 2 → 1 with the owner's explicit
  mid-campaign authorization (password supplied by the owner for exactly this
  purpose; recorded here as an owner override of the static server-safety
  list). This enables kernel+user counting for the benchmark process.
  Multiplexing: instructions/cycles are fixed counters; the two software
  events share one slot; no enabling-fraction warning appeared in any record.
  Governor/system state otherwise untouched (`schedutil` throughout).

## Calibration protocol evolution (all A/A runs preserved)

A/A = the same Release binary run under two labels (`base`, `base2`) with
the identical interleaving intended for A/B; 15 pairs per headline config.

### v1 (`cal-aa-tp`, `cal-aa-ur`): WARMUP=1, arm-derived fixture path

Exposed two problems:
1. Frequency-state carry-over: with schedutil, the fixture write between
   pair members lets cores downclock (observed 1.2 → 3.2 GHz ramps inside
   invocations), inflating wall variance.
2. **A reproducible ~1–2% systematic bias on the identical binary at the
   TP W8 config** (median wall ratio 0.982, 13/15 pairs B<A, CI excluding
   1.0) — correlated with the arm label, *not* with pair position.

### v2 (`cal2-aa-tp`, `cal2-aa-ur`, `cal3-aa-tp`): WARMUP=2

More warmup tightened the single-worker configs (TP d1-w1 wall CI
[0.991, 1.005]) but the W8 label bias persisted and reproduced across
three independent runs (cal2: 0.988, 12/15; cal3: 0.986, 11/15) — proving
it systematic, not drift.

### Root-cause experiment (`cal4-aa-tp-shared`): shared fixture path

With the tmpfs fixture path made identical for both labels (everything else
unchanged), the bias vanished: median wall ratio 1.005, 6/15 B<A, means
0.05985 vs 0.05998. **The arm-derived path string itself (one character
longer for `base2_…` than `base_…`, entering the child environment and
every file path) shifted the identical binary's measured wall by ~1%.**
The plausible channel is environment-block size → initial process/stack
layout effects that survive within a path-length class. A one-character
difference in a *name* is therefore a first-order confounder at the effect
sizes this campaign studies — a methodological finding recorded as a
campaign deliverable.

### v3 (adopted, frozen): WARMUP=2 + per-rep shared fixture path

Both arms of a config share one fixture directory (recreated fresh per
invocation), so every arm runs under byte-identical path/environment shapes.
Final calibration (`cal5-aa-tp`, `cal5-aa-ur`, 15 pairs each):

| Config | wall_s median [exact CI95] | instructions median [exact CI95] | cycles median [exact CI95] |
|---|---|---|---|
| TP d1-w1 | 0.9964 [0.9848, 1.0039] | 0.99993 [0.99986, 1.00008] | 0.9973 [0.9937, 0.9991] |
| TP d16-w8 | 1.0021 [0.9833, 1.0153] | 1.0016 [0.9991, 1.0154] | 0.9982 [0.9932, 1.0074] |
| UR d1 | 0.9975 [0.9775, 1.0199] | 0.99987 [0.9965, 1.0030] | 1.0046 [0.9939, 1.0097] |
| UR d16 | 0.9942 [0.9849, 1.0077] | 1.0003 [0.9956, 1.0067] | 0.9972 [0.9875, 1.0084] |

(The first published version of this table used a Monte Carlo bootstrap CI
that is systematically one order statistic too tight at n = 15; these are
the exact percentile-bootstrap intervals from the same raw rows — see R6's
statistics note. Under the exact intervals the UR d16 wall upper bound
moves from 0.9997 to 1.0077, i.e. the earlier "residual drift" reading of
that cell dissolves; no A/A band conclusion changes materially.)

**Noise floors** (minimum reproducible effect a contrast must exceed):
- wall/throughput: ~2% (all configs)
- instructions: ~0.05% at single-worker configs (d1-w1, UR d1); ~1% at
  multi-worker / deeper-queue configs (kernel-path scheduling variance)
- cycles: ~0.5–0.8%

Point estimates wander up to ~0.5 % inside these bands (e.g. TP d1-w1
cycles median 0.9973) — the A/A CIs are the reference against which every
R6/R7 contrast is read; a contrast is only reported as an effect when its
CI separates from both 1.0 **and** the corresponding A/A calibration band.
Recorded limitation: no W-03 A/A calibration was run; W-03 contrasts are
read against the same backend's W-02 bands by proxy.

## Warm-up and environment behavior

- Two warmup rounds per invocation bring the pinned CPUs to ~3.19 GHz for
  the measured round at most configs, but **DVFS sag is real**: 7–58 of the
  per-campaign records ended below 3 GHz (minimum observed 1.20 GHz), so
  frequency variance inside measured invocations remains a plausible
  contributor to the ±2 % wall floor. The paired/rotated design exposes
  both arms of every pair to the same environment distribution.
- Concurrent activity during calibration and campaigns: only this session
  plus the desktop environment; 1-minute load reached 1.9–3.0 on the
  20-CPU host during measurement bursts (medians ≈ 2). No competing
  benchmarks were run.
- W-01 negative control design: `bench_w01_direct` links only
  `sluice_core`, which no arm modifies — its binary is byte-identical
  (SHA `f578f047…`) across all five arms, so any measured arm "difference"
  on W-01 is by construction pure noise and directly estimates the
  end-to-end noise floor of the full stack (runner + perf + time + bench).

## Raw data

`raw/calibration/{cal-aa-tp,cal-aa-ur,cal2-aa-tp,cal2-aa-ur,cal3-aa-tp,cal4-aa-tp-shared,cal5-aa-tp,cal5-aa-ur}/ab.jsonl`
plus `provenance.txt` per directory (compiler, kernel, governor, paranoid
level, per-arm binary SHAs, timestamps).
