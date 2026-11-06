# #595 R5-repair — preregistered three-arm measurement plan (frozen)

Frozen 2026-11-07 (UTC), before any formal timing cell of this round ran.
Research question (task §4.1, verbatim scope): does removing the repeated
eligible-prefix scan and the mechanical rename, with policy, inputs, order,
resource budget and lifecycle unchanged, improve R's CPU and tail latency,
and does the improvement let R meet the full performance gate against A0?

## 1. Frozen objects

| Object | Value |
|---|---|
| R_original source | `f3b25329bc8a64a201961c426a58adacfb8fdf08` |
| R_cleanup source | `b191bb893432c4df0551d24c85843d8513ed5a55` (= f3b25329 + C1 `c55165a2` + C2 `b191bb89`) |
| Diagnostic fixed | `research-595-r5rm-diag` @ `c0cdc3ea` (21374a40 + v2 instrument) |
| Toolchain | gcc 16.2.1, xmake v3.1.2, `-m release --liburing=y`, clean worktree builds |
| bench_w03 original binary sha256 | `7bf83eb93fa589628649931a1a5cdba6a9e9375083e8ced38e09a73c455b9da5` |
| bench_w02 original binary sha256 | `b07fd060629e5baa25e20aaab14ec6212ccf8e6e7160b41f9f06300d8534047c` |
| bench_w03 cleanup binary sha256 | `9442f02b24cec81a7040eea9a6e79e221a5fc9a7ac264b6db9c1fdae28cb6c6b` |
| bench_w02 cleanup binary sha256 | `971a28da2fcfb3ab6099453aabd56a5a6f3524fdc956ec967245f72753a27131` |
| Diagnostic binary (v2 trace) | recorded in `logs/binary-sha256.txt` after build; never used for clean timing |

Arms: A0 = original binary, `SLUICE_595_R3_ARM=A0`; R = original binary,
arm R; C = cleanup binary, arm R. No other arm tokens. C−R answers the
cleanup increment, C−A0 the net effect (the formal gate), R−A0 the
same-session negative control/mechanism reproduction. No summing of the
two percentages.

## 2. Correctness prerequisites (completed before this freeze)

- 18-scenario deterministic ready-reuse transport oracle (R4 archived source
  sha256 `5c10be9e…`; cleanup run via the frozen mechanical-rename
  adaptation): 18/18 rc=0 both trees, full outputs byte-identical.
- In-repo suites (10) rc=0 both trees; byte-identical except `r4_arm_smoke`
  (57/57 PASS both; 14 label-only lines, the C2 renames) and two suites
  proven nondeterministic by same-binary reruns this round
  (`shutdown_lifecycle_test` same-binary rerun differs;
  `uring_request_core_integration_test` produces the identical two-variant
  output set on both binaries, 5:1 split, single racy INFO line).
- `bounded_scan_check`: 5,720 mixed-eligible spans, lengths 0..64,
  `batch_shaped_prefix_bounded == min(full, bound)` and
  `min(q, bounded) == min(q, full)` for q ∈ {1,2,8}; front-ineligible
  zero-test identical. PASS.
- W03 real-kernel smoke rc=0 for A0/R on both binaries.
- Environment anomaly recorded: direct `g++ -D…` invocations from the agent
  shell intermittently drop specific define flags (worked around with
  `#define`-wrapper TUs; xmake builds unaffected — verified by the
  instrument activating in the diag build and by distinct binary SHAs).

CLEAN_BENCH_READY = YES.

## 3. Cells, order, sample size

Five cells (main + 4 protections), all warm tmpfs fixture via
`SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-r5rm`, 20,000 ops, taskset -c 2:

| Cell | case | extra env |
|---|---|---|
| MAIN-W03-D16 | `W03-uring-4k-d16` | — |
| PROT-W03-D4 | `W03-uring-4k-d4` | — |
| PROT-W03-D2SERIAL | `W03-uring-4k-d2-r3d2serial` | `SLUICE_595_R3_CASES=1` |
| PROT-W02-D1 | `W02-uring-4k-d1` | — |
| PROT-W02-D2 | `W02-uring-4k-d2` | `SLUICE_BENCH_595_DEPTHS=1` |

Per invocation: warmup=1, measured=5, stats=0 (same as R4 clean matrix).
Per cell: 24 matched three-arm blocks + 24 A/A pairs + calibration pairs
(24 on MAIN, 8 on protections; members A0(original) vs A0(cleanup), order
alternating by pair parity). Block arm order: the 6 permutations of
(A0,R,C), each 4 times; perms shuffled with `random.Random(5954)`, the
24-slot sequence (perms*4) shuffled with `random.Random(5955)` (frozen in
`harness/measure3.py:block_orders`; schedule sha recorded in
`raw/PROGRESS.json` and must match across restarts).

Invalid-invocation rule: any of exit≠0, missing/extra warmup/measured
records, per-rep ok=false, ops≠20000, binary sha mismatch, arm readback
mismatch → up to 2 retries; every attempt (valid or not) stays in
`results.jsonl`/`attempts.jsonl`. Anomalies are reported, never deleted;
post-hoc outlier removal is not performed.

Measurement exclusivity: every timing segment runs under flock of BOTH
`/home/jnhu/sluice-595-e5/MEASUREMENT.lock` (acquired first) and
`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` (second), receipts in
`logs/LOCK_RECEIPT.md` (UTC, fuser -v, second-fd probe, pre/post state).
No builds, compression, sanitizer or tracing run concurrently with timing.

## 4. Metrics and definitions

Per invocation (R4 caliber, computed identically in `harness/measure3.py`):
- `cpu_ns_per_op` = 1e9·Σ(measured cpu_s)/Σ(ops); process CPU from the
  bench's own `process_cpu_seconds` per rep plus wait4 rusage totals.
- `ops_per_sec` = Σops/Σwall.
- Run-level p50/p95/p99 = **mean over the 5 measured reps of each rep's
  percentile** (percentiles computed by the frozen harness over that rep's
  20k per-op latencies). This is never a pooled per-request percentile and
  is never compared against one.
- Latency origin (timestamp origin, F3): unchanged frozen R4 semantics —
  T0 = instant the op's ReadOp is fully built (per-item build), before
  arm-specific submission work; A0 builds per op, R builds the group first:
  descriptor-ready time is therefore call-pattern-dependent. This is the
  historical caliber's scope limitation, not application arrival latency.
  No suffix re-stamping, no origin changes (candidate B is not implemented).

CPU accounting (§4.6): primary = process CPU (rusage user+sys, which
includes all joined threads). Per-thread buckets and kernel io-wq worker
ticks are sampled separately (`buckets` in each record). io-wq kernel
threads are NOT inside the process rusage → reported as a coverage gap,
never added to process CPU, never called whole-system CPU. PMU counters
are UNAVAILABLE this round (perf_event_paranoid=2 recorded in R5); no PMU
number is reported or substituted.

## 5. Statistics (implementation validated on known samples before freeze)

- Inference unit: the matched block (one invocation per arm per block).
  Neither the 20k requests nor the 5 in-invocation reps are independent
  samples.
- Contrast per block: (X−Y)/Y on the block's values, for {C−A0, C−R,
  R−A0}. Throughput reported in the same sign convention (positive = X
  slower for cost metrics, positive = X faster only for tput where stated).
- Bootstrap: 10,000 resamples, seed 5950, resampling whole blocks (the
  (A0,R,C) triple stays together). Percentile CI 2.5/97.5; one-sided 95%
  bounds = 5th/95th percentiles; frozen quantile convention
  `sorted_means[int(p*n)]` (validated: values 1..100 → ci95=[3,98],
  lower95=6, upper95=96).
- A/A gate per cell: half-width (ci97.5−ci2.5)/2 of the pos1−pos0 paired
  effect ≤ 5pp (cpu), 3pp (tput), 3pp (p99); p95 reported without a gate.
  A/A failure → the affected cell's precision qualification is
  NOT_ESTABLISHED and no GO verdict is issued from it; the frozen sample
  plan continues; no re-running until pass. Mean drift and order effects
  are reported alongside.
- Holm family (main gate, Holm across the 4 hypotheses below; protections
  Holm within each cell across their 4): p-value = bootstrap fraction of
  resampled means on the failing side of the gate threshold, floored at
  1/10000. "Significantly different from 0" is never used as non-inferiority.

## 6. Gates (frozen thresholds)

C vs A0 on MAIN-W03-D16:
- CPU/op: point ≤ −10% AND one-sided 95% upper bound ≤ −5%
- throughput improvement (C/A0 − 1): one-sided 95% lower bound ≥ −3%
- p95 and p99 cost (C/A0 − 1): one-sided 95% upper bounds ≤ +5%

Protections (each cell), C vs A0: cpu upper ≤ +5%; tput lower ≥ −3%;
p95/p99 upper ≤ +5%.

C vs R (cleanup effectiveness, both reported on MAIN and protections):
CPU upper bound < 0 required to claim "reduces CPU"; p99 upper bound < 0
required to claim "reduces p99"; CI crossing 0 → benefit not established
(not "no effect"); no arbitrary percentage demanded; C−R improvement never
substitutes for the C−A0 gate.

Conditional branches: d64 and btrfs cells run ONLY if the main gate AND
all four protections pass, with the same three-arm 24-block protocol;
otherwise recorded NOT_RUN_CONDITIONAL. The S/SNI/SNP/Q2/Q8 matrix is not
run this round.

Resource gates: per-invocation `buckets` thread counts and io-wq ticks
compared across arms; RSS gate +2% (absolute values reported) on MAIN.
"无新增线程/timer/每请求分配" is concluded only from source identity
(C1/C2 diff scope) plus these runtime observations.

## 7. Diagnostic round (separate session, after clean timing)

Fixed instrument (v2, `c0cdc3ea`), shapes W03 d16 (main) + W03 d4
(control), arms A0/R/C with on/off matched: 12 four-tuples per shape = for
each block, each arm once with trace on and once with trace off, order
alternating. Interaction quantity per arm-pair: (X−A0)_on − (X−A0)_off in
percentage points; ±3pp equivalence window with bootstrap CI; window not
established → PERTURBED, diagnostic numbers remain leads, no formal
attribution. Wake association only within the same writer thread (F4);
negative intervals preserved and classified, never clamped. Analysis uses
`scripts/analyze_tail_trace_v2.py` (offline join). p99 attribution rule of
R5 (stage-p99 ≥ 50% of total-p99 gap) is ABOLISHED (F2): no
percentile-contribution attribution is computed; tail statements are made
only as conditional means / conditional tail risk per real group position,
and causal claims only from the intervention contrasts above.

## 8. Outcome states

MAIN_COMPLETE / TRACE_NOT_ESTABLISHED separation maintained: a failed
trace never blocks the clean verdict, and clean timing never uses
instrumented binaries. Gate FAIL is a valid completed outcome.
