# #595 E5 round — pre-registered measurement and decision rules

Frozen BEFORE the first timing run (2026-11-04). Nothing below may be revised after
unblinding except by the one sanctioned measurement-condition repair, which keeps both
receipts and the original thresholds.

## Fixtures and binaries

- Worktree `/home/jnhu/sluice-595-e5/worktree` at `271b81bf` + the arm-B research
  patch (diff archived as `patch.diff`; binary SHA256s in `build-notes.md`).
- Bench programs: `bench_w02_pipeline` (frozen case list; D2/D3 cases enabled only via
  `SLUICE_BENCH_595_DEPTHS=1`), `bench_w03_external_loop` (frozen cases).
- One binary per program; arms selected by env `SLUICE_595_ARM` (absent = A, `B` = B).
  Same code layout for both arms — no binary-layout confound.
- Fixture: 64 MiB pattern file on warm tmpfs (`/tmp/sluice-595-bench`), byte-oracle
  verified per rep by the bench itself; every run must exit 0 (oracle + stats wiring
  gates inside the bench invalidate a failed rep entirely).
- Per-timing-rep accounting: fixed 20,000 valid reads (the bench's request_count);
  D64 uses its frozen 20,000; 64 KiB case uses its frozen 2,000 (delivered data
  reported as-is, never re-scaled).

## Windows and metrics

- Wall/throughput/latency: the bench's measured-rep throughput phase and its
  instrumented latency pass (submit→take, includes consumer scheduling because take
  is the observable consumption point). p50/p95/p99 computed per fixed-length rep
  then paired.
- CPU (decision metric, whole-executor): wrapper-measured child total CPU
  (`wait4` rusage, utime+stime of every process thread) PLUS the delta of
  `iou-wrk-*` kernel-thread CPU sampled at ≤2 ms from /proc during the child window
  (per-tid monotone last-value sums; exit events counted; error bound = period ×
  live workers). Both arms identical measurement machinery. Unfavorable-end
  adjudication: B's iou-wrk at its sampled maximum, A's at its sampled minimum.
- Latency window: from before the submit call to take returning (bench instrumented
  pass), disclosed as including consumer scheduling.
- Mechanism ledger (separate runs, never mixed with timing): `strace -f -c -e
  io_uring_enter` counts per arm and shape; `SLUICE_BENCH_STATS=1` poll/wait counters
  from the JSONL; PMU round (`perf stat` with sudo, paranoid=2 noted) for
  user/kernel instructions and cycles with time-enabled/time-running recorded.

## Pairing protocol

- Seeds/order: the bench's internal per-round rotation continues to order cases; the
  wrapper alternates arm order ABBA/BAAB-balanced across 24 pairs per shape
  (pair i: even → A then B, odd → B then A).
- Each pair member = one bench invocation with `SLUICE_BENCH_WARMUP=1
  SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_CASE_FILTER=<case>`; CPU window = whole
  invocation (identical structure both arms; per-phase process CPU also recorded from
  JSONL as a secondary view).
- A/A calibration: 24 pairs of A-vs-A on W-02 4k d16 BEFORE any A/B. Pass gates
  (pair-relative-difference 95% CI half-widths): CPU ≤5 pp, throughput ≤3 pp,
  p99 ≤3 pp. If exceeded: exactly one measurement-condition repair allowed (e.g.
  pinning, isolation), both receipts kept, thresholds unchanged. If still failing:
  report INCONCLUSIVE_NOISE for affected metrics.
- Pair statistic: relative difference per pair = (B−A)/A for CPU and tails (negative
  = improvement), (A−B)/A for throughput. Bootstrap 95% CI over pairs (10,000
  resamples, percentile). Negative-control verdicts: Holm–Bonferroni across the
  negative-control family at family α=0.05, one-sided (worsening).
- Exclusion rule (frozen): a rep is invalid only if the bench's own oracle/stats
  gate fails (non-zero exit) — no post-hoc outlier removal. Background load and
  loadavg recorded per rep; any rep overlapping a foreign load spike is marked in
  the log, never dropped silently.

## Placement

- Default pinning: cpu2 (whole process, `taskset -c 2`). SMT sibling pair {2,12},
  cross-physical pair {2,8}.
- W-03 placement checks: (main,worker) pinned (2,12) [SMT siblings] and (2,8)
  [cross-physical]; 8 pairs each, safety-only unless both agree.
- W-02 placement checks: pinned cpu2 vs pinned cpu8, 8 pairs, safety-only.
- Uncontrollable placement is reported as not accepted.

## Pre-timing measurement-machinery determination (2026-11-04; taken before any timing receipt)

A setup probe under a live bench run established, before the first timing receipt, that on
kernel 7.2.11-200.fc44 the io-wq workers are members of the ring owner's thread group:
`/proc/<pid>/task/` lists `iou-wrk-<n>` threads next to the bench thread, and an unpinned
run reaches 180–235 % process CPU with no user-level threads (per-thread utime+stime sums
match the child total). Consequences, fixed here:

1. `wait4` child rusage and the bench's `CLOCK_PROCESS_CPUTIME_ID` per-phase delta already
   include every io-wq worker's CPU. The originally registered additive `iou-wrk-*`
   /proc delta would double-count and is therefore NOT added. The /proc thread
   enumeration is retained as a consistency check on one run per shape (sum of per-thread
   utime+stime must not exceed the child rusage; any CPU-bearing thread outside the
   thread group is recorded as a measurement defect).
2. Whole-invocation rusage is dominated by fixture setup (measured 2.3–3.1 s CPU, of
   which the measured phase is ~0.06 s; setup spread ~30 % run to run). The primary CPU
   decision metric is therefore the bench's per-measured-rep process-CPU delta (`cpu_s`
   in JSONL): the whole-executor CPU of the measured window on this kernel. Whole-
   invocation `wait4` rusage is recorded for every pair as a secondary view; where the
   main net-win gate is claimed, its between-arm sign must agree with the primary metric.
3. Attribution: the metric has a single accounting source, so the unfavorable-end
   sampling bound has nothing to shift; the pair bootstrap CI upper bound is used as
   registered. This is recorded, not re-interpreted after seeing a difference.
4. Thresholds, pairing protocol, exclusions and decision gates below are unchanged. This
   is a pre-timing machinery determination (no timing receipt existed when it was taken),
   not the A/A measurement-condition repair; that repair option remains available if the
   calibration below fails.

## Measurement-condition repair (the single sanctioned repair; recorded before the A/B round)

A/A calibration on the main shape (24 pairs, `W02-uring-4k-d16`, cpu2) failed the frozen
p99 half-width gate: CPU 2.44 pp PASS, throughput 2.50 pp PASS, **p99 3.71 pp FAIL**
(limit 3 pp). Receipt: `raw/aa-calib-w02-4k-d16/`. Diagnostics:

- No order effect: the sign of the second-minus-first p99 difference is symmetric across
  pairs; CPU/tput pair differences are tight (≈2.5 pp half-width).
- Within-invocation rep-to-rep p99 spread is 6.6 % (sd), while the between-invocation
  spread of a 10-rep mean is only 2.5 %: single-rep p99 sampling, not slow drift,
  dominates the pair difference.
- Placement probes recorded as receipts but not used as repairs: SMT pair {2,12}
  (`raw/aa-calib2-w02-4k-d16-smt/`, p99 5.35 pp FAIL) and cpu8
  (`raw/aa-probe-cpu8/`, 12 pairs, p99 8.05 pp FAIL) — the noise is not core-specific.

Repair (one, thresholds unchanged, both the pre-repair and repaired receipts kept):
each pair-member invocation runs `WARMUP=1 MEASURED=5`. The pair-member metric is the
aggregate over its 5 measured reps: CPU per op = Σcpu_s / Σops; throughput =
Σops / Σwall; p50/p95/p99 = arithmetic mean of the five per-rep quantiles (each rep is
still a fixed 20,000-read pass; disclosed as a per-invocation summary, not a pooled
histogram). Everything else — pairing, order balance, arm selector, decision gates,
negative-control family — is unchanged. If the repaired A/A calibration still exceeds a
half-width gate, that metric is reported INCONCLUSIVE_NOISE.

For transparency: two 2-pair harness-smoke runs on the main shape preceded this repair
(`raw/harness-smoke/`); they are machinery validation only and enter no analysis.

## Decision gates (frozen; from the issue text)

| Gate | Condition |
|---|---|
| Correctness | all mandatory oracles pass; data/result/cancel/lifetime/resource semantics equivalent (Step-3 receipts) |
| Mechanism | W-02 4k d16: pure submit enters per valid read reduced ≥50% vs A; data SQE/CQE ledger reconciles (strace enters vs completed ops) |
| Net win | whole-executor CPU per useful task: point estimate ≤ −10%, bootstrap 95% CI upper bound ≤ −5%, at the unfavorable attribution end |
| Performance protection | main shape: throughput drop ≤3%, p95/p99 worsening ≤5%. Negative controls (D1/D2/D3, 64 KiB d16, W-03 d4/d16/d64, threadpool, saturate): CPU/p95/p99 worsening ≤5% each, throughput drop ≤3% (Holm family) |
| Resource | zero new queue/thread/timer/per-request allocation; peak RSS growth ≤2% |

Verdicts: GO_MECHANISM (only for passing E5 shapes) / KEEP_IMMEDIATE / INCONCLUSIVE.
PRODUCT=HOLD this round regardless (#604 has not handed off READY_FOR_595; verified
2026-11-04 — its latest comment is the #622 cost-map reference).
