# DP-0 R4 experiment protocol — FROZEN before first matrix run

Frozen at: 2026-10-23 (WSL2 host, Ubuntu, GCC 15.2.0, Linux 6.18 WSL2 kernel)
Baseline commit: be28f275 (master after PR #569 merge). Arms: base, dp1, dp2, dp2b, dp3a —
all at be28f275 + the arm's single research patch; all 5 pass 76/76 conformance tests.

Microbench binary sha256 (same harness source, per-arm link):
- base e2560b4a9435656be6cd54f752b162de0a95788ab3745c40efe8bed5f9b00173
- dp1  172ff51533ba14518ed768b687d8361f5126a3fe69d25133f8ca7b0b353b184f
- dp2  555fc70d7febb62faff2ca6eba07990decb2f9fb548151baa7cbce70ad0bb579
- dp2b d283d8c5a5966bcbdea860b24416dfe4689f0a07ce8cb746fa87857989d78207
- dp3a 5472b3a7fa48ee36df6b7d0067d5f1558d5bc403f59848cef236ab4609979757

## Fixed matrix (no post-hoc changes permitted)

Cases and iterations (per process run; 10% extra warmup inside the process, excluded from timing):
- zop 200000, cons4k 20000, thr64 10000, write4k 20000, finfo 20000, constp 20000, w01ctl 200000

Configs = {zop, cons4k, thr64, write4k, finfo, constp, w01ctl} × {dp1, dp2, dp2b, dp3a}
= 28 configs, plus one noise-floor control: cons4k × base (self-pair, identical binary).
Per config: exactly 15 interleaved AB/BA pairs = 30 process runs. Pair i (0-based):
even i → [base, arm]; odd i → [arm, base]. Config order shuffled with seed 0xd0d0.

perf invocation (wraps the whole process):
`perf stat -x, -e instructions:u -e cycles:u -e task-clock -e context-switches -e cpu-migrations -o <tmp> -- ./bin/dp0b_<arm> <case> <iters>`

Environment: SLUICE_BENCH_TMP_DIR=/tmp/dp0/tmp, SLUICE_DP0_REUSE_FIXTURE=1
(fixture written once by a priming run; excluded from every measured process).

## Counter semantics and validity (carried from calibration)

- instructions:u, cycles:u are user-mode PMI-based counts, calibrated linear and
  reliable on this WSL2 kernel earlier in this study.
- L1-dcache-*, dTLB-* events: unavailable on this host → NOT collected; any cache-line
  attribution in this study is analytical (assembly + layout), never a PMU count.
- cache-references / cache-misses: earlier probing showed host-specific inconsistency →
  NOT collected. If any document cites them, mark INVALID.
- Per-run validity gates (else the run is excluded from paired statistics and the
  exclusion is counted in the results): perf exit 0; bench JSON printed; instructions > 0;
  0 < cycles/instructions < 6.

## Counter semantic validation built into the matrix

1. w01ctl (blocking read_at loop) executes byte-identical measured code in all 5 arms
   (W-01 path untouched; blocking_file codegen identical). Therefore the spread of
   w01ctl per-iteration instructions:u across all 5×30 runs measures the counter noise
   floor; it must also show NO_EFFECT for every arm per the thresholds below.
2. cons4k × base self-pairs (identical binary on both sides) give a direct paired
   noise floor: median |delta| per iteration and pair sign balance.

## Frozen decision rule (fixed BEFORE the matrix; primary metric first)

Primary metric: instructions:u per iteration. Paired per-config statistics use the
15 within-pair differences (base − arm), each pair contributing one difference
(position-balanced AB/BA).

- REDUCED_CONFIRMED: ≥ 12/15 pairs show a positive difference (arm < base)
  AND median relative reduction ≥ 0.5%.
- REDUCED_HINT: median relative reduction ≥ 0.2% but the pair count test above not met,
  or median ≥ 0.5% with < 12/15 pairs.
- NO_EFFECT: |median relative change| < 0.2%.
- REGRESSED: median relative increase ≥ 0.5%.

Secondary metrics (cycles:u per iteration, wall seconds per iteration, task-clock):
reported as medians with the same pairing; they do not gate the verdict. On this host
wall/cycle noise is expected to exceed the small per-op deltas; that is recorded as a
host limitation, not as evidence of absence.

## Pre-registered mechanism expectations (from assembly evidence, written before runs)

- dp1: removes ~120 B of dead PublicationPayload stores (12×16 B moves) per inline
  publication → expect REDUCED_CONFIRMED instructions on uring cases (zop strongest);
  w01ctl NO_EFFECT; threadpool constp smaller effect (publish_one also patched).
- dp2/dp2b: removes metadata double-copy only for non-metadata result types
  (std::size_t carriers) → expect effects on zop/cons4k/thr64/write4k/constp;
  finfo (Request<FileInfo>) must show NO_EFFECT for dp2/dp2b (metadata path unchanged);
  dp2b additionally removes the 72 B local RequestResultData initialization.
- dp3a: removes ~13 stores (~131 B) per slot acquire/release cycle → small instruction
  reduction on all slot-lifecycle cases; w01ctl NO_EFFECT.
- w01ctl × every arm: NO_EFFECT mandatory (negative control).

Any deviation from this protocol after the first run starts (iteration changes,
threshold changes, added/removed configs) must be recorded as a protocol violation
in the results, with the reason and the offending diff.

## AMENDMENT-1 (recorded 2026-10-23T05:41+08:00, before the relaunched matrix; no
## statistics from the aborted run were produced or consumed)

The first matrix launch (05:26) hung on its first config (constp × base): the constp
case's wait loop never called ctx.poll(), and threadpool-backend completion delivery
requires the caller to drive the context — try_result() never reported ready, so the
run spun indefinitely (verified: >2.5 CPU-minutes at 20000 iterations; a one-shot probe
with poll() completes in 190 spins / 0.057 ms). This is a harness defect, not an arm
difference. Changes, applied uniformly to all five arms:

1. constp wait loop now polls the context inside the readiness loop, matching the
   structure of every other async case (bench_dp0_movement.cpp; all five binaries
   rebuilt; sha256 re-recorded in results_bin_hashes_final.txt).
2. constp iterations 20000 → 10000 purely for wall-budget (per-iteration metrics,
   pairing, thresholds and all other cases unchanged; measured constp runtime
   ≈ 0.2 s per run at 10000 iterations).
3. The aborted run's empty JSONL files were deleted; the matrix restarts from zero
   with the same seed and config shuffling rules.

Microbench binary sha256 after amendment:
- base bb44b946ae38fad9b8ccfcb0f762def978c34920d01d4d76c0a9c66b78d55665
- dp1  af07ff12438e35dc339be98edfa1f6ec3e3b0978227718f20bba656de46070e9
- dp2  6b5b92bc3ffd3bd8d2644c9c9b5005c3b6b58c264569c8b5de7ee34aec7b2a3f
- dp2b c645906e14e40c490bdab5115fc82d6e49f065ece455cdc800fb8bbc7eeed19f
- dp3a 37f90587225f524843db454e5cd86211bd18a0920bcd9d66744df86ca20a3882
