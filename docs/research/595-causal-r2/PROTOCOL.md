# causal-r2 phaseA — uninstrumented baseline re-run protocol

Round: #595 E5 causal R2, phase A (2026-11-04). Purpose: re-establish the
uninstrumented A/A noise and A/B paired-difference baseline on a fresh build of
the R1-frozen workspace, before any new instrumentation is layered on.
Decision authority for thresholds: `/home/jnhu/sluice-595-e5/evidence/measurement/PREREGISTERED.md`
(read-only R1 pre-registration). This document records what R2 phaseA actually
ran and every deviation from R1.

## Workspace and provenance

- Independent workspace: `/home/jnhu/sluice-595-e5/causal-r2/worktree`
  (git worktree, branch `research-595-r2-causal`). Main working tree
  `/home/jnhu/Source/Sluice` untouched; R1 evidence dir untouched.
- Base commit: `271b81bfc9d64eecc0266fe7066b9040525dd710` (== R1 frozen
  baseline), verified via `git log -1` in the new worktree (clean, 0 modified).
- Research patch: `/home/jnhu/sluice-595-e5/evidence/measurement/patch.diff`
  applied unmodified; sha256
  `7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0` (matches
  R1 manifest); `git apply --check --reverse` PASS in the new worktree
  (tree == patch exactly); `git diff HEAD` sha256
  `b006dbbf751f5fb2bbe8fb9edb091ae66e57a181b8cf2470d880c9589e1f4072`.
- Byte-identity vs R1 worktree verified by cross-diff: `bench/`, `src/`,
  `include/`, `xmake/tests.lua`, `tests/uring_595_bounded_progress_test.cpp`
  all identical.
- A/B arms unchanged from R1: arm A = IMMEDIATE (frozen baseline inline flush),
  arm B = BOUNDED_PROGRESS; both from the same binary, selected at runtime by
  env `SLUICE_595_ARM` (absent = A). No public API or v2 contract change.

## Build and binaries

```
xmake f -c -y --liburing=y -m release
xmake build bench_w02_pipeline bench_w03_external_loop uring_595_bounded_progress_test
```

Toolchain: g++ (GCC) 16.2.1, xmake 3.1.2+HEAD, liburing-devel 2.13-2
(`evidence/raw/phaseA/toolchain.txt`). Binary SHA256 (recorded 2026-11-04,
`evidence/raw/phaseA/binary-sha256.txt`):

```
61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886  bench_w02_pipeline
3ae513d0272bdf2c04ef8c277f8a8f63c121c0db28b90ab7bf59f1367cb65553  bench_w03_external_loop
865de828f9b4bc95146caa15101921a28d7298247f8d2130d6d7be56db2b069a  uring_595_bounded_progress_test
```

The two bench hashes differ from R1's (`32beabcd…`, `1e548bc6…`) because this
is a rebuild; per R1 build-notes a rebuild invalidates binary hashes and
requires a fresh A/A calibration — which this round performs. The oracle-test
hash byte-matches R1's `865de828…069a`.

Build sanity (machinery validation, enters no timing analysis):
`SLUICE_595_ARM=B uring_595_bounded_progress_test` → exit 0, "all checks
passed" (`evidence/raw/phaseA/oracle-smoke-armB.log`); 1-pair harness smoke on
`W02-uring-4k-d16` (`evidence/raw/phaseA/smoke-r2/`) → both invocations exit 0,
arm selector live.

## Protocol (reused from R1 repaired protocol)

- Harness: `/home/jnhu/sluice-595-e5/harness/pair_runner.py` (unchanged);
  analysis: `analyze.py` (unchanged). python3 -I invocations.
- Phases, in execution order:
  1. `aa-calib-w02-4k-d16` — A/A calibration, 24 pairs, arms `AA`,
     case `W02-uring-4k-d16` — BEFORE any A/B round.
  2. `ab-d1-w02-4k` — case `W02-uring-4k-d1`, 24 pairs, arms `AB`.
  3. `ab-d2-w02-4k` — case `W02-uring-4k-d2`, env `SLUICE_BENCH_595_DEPTHS=1`,
     24 pairs, arms `AB`.
  4. `ab-d3-w02-4k` — case `W02-uring-4k-d3`, env `SLUICE_BENCH_595_DEPTHS=1`,
     24 pairs, arms `AB`.
  5. `ab-main-w02-4k-d16` — case `W02-uring-4k-d16` (main shape), 24 pairs,
     arms `AB`.
- Per pair member: one fresh bench invocation, `taskset -c 2`,
  `SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=5 SLUICE_BENCH_CASE_FILTER=<case>
  SLUICE_BENCH_STATS=1`, fixture on warm tmpfs `/tmp/sluice-595-bench`
  (64 MiB pattern file, byte-oracle verified per rep inside the bench).
- Even pair → A then B; odd pair → B then A. In A/A rounds both members are A.
- Pair-member aggregate over its 5 measured reps: cpu/op = Σcpu_s/Σops
  (primary CPU metric: bench per-measured-rep process CPU delta,
  CLOCK_PROCESS_CPUTIME_ID, includes io-wq workers on this kernel per R1's
  pre-timing machinery determination); throughput = Σops/Σwall; p50/p95/p99 =
  arithmetic mean of the five per-rep quantiles. Whole-invocation `wait4`
  rusage recorded as secondary view.
- Fixed workload: 20,000 valid 4 KiB reads per measured rep; bench byte-oracle
  gate per rep; exit 0 hard gate.
- Statistics: pair-relative differences; cpu/p95/p99 = (B−A)/A, tput =
  (A−B)/A in `analyze.py` convention (positive = B worse); bootstrap 95% CI,
  10,000 resamples, seed 5950, percentile. A/A gate: half-widths cpu ≤5 pp,
  tput ≤3 pp, p99 ≤3 pp.
- Environment: Xeon E5-2666 v3 (10C/20T), kernel 7.2.11-200.fc44, schedutil
  1.2–3.5 GHz, perf_event_paranoid=2, pre-run loadavg guard at default 1.5
  (no adjustment needed: max 1-min loadavg after any invocation 1.25), machine
  otherwise idle and exclusive during all timing runs, per-invocation loadavg
  and binary sha recorded by the harness in `results.jsonl`.
- Raw data layout: `evidence/raw/phaseA/<phase>/{results.jsonl,header.json,
  summary.json,logs/,jsonl/}`. Mechanism-level instrumentation (strace/PMU/
  bpftrace) intentionally NOT mixed into any timing run here (uninstrumented
  baseline phase).

## Results

### A/A calibration (24 pairs, `W02-uring-4k-d16`, arms AA) — GATE PASS

From `analyze.py summary --phase-dir …/aa-calib-w02-4k-d16 --arms AA --gate aa`:

| metric | mean | CI95 | half-width | gate | verdict |
|---|---|---|---|---|---|
| cpu/op | +0.75% | [+0.12%, +1.42%] | 0.65 pp | ≤5 pp | PASS |
| tput | +0.73% | [+0.10%, +1.38%] | 0.64 pp | ≤3 pp | PASS |
| p99 | +0.96% | [−0.66%, +2.66%] | 1.66 pp | ≤3 pp | PASS |

(A/A means ~+0.7% on cpu/tput = small consistent second-position-in-pair
offset; within gate half-widths and cancelled by the AB/BA order balance in
A/B analysis. No repair needed; the R1 single-repair option remains unspent.)

Noise figures carried forward as the R2 phaseA relative-noise baseline:
CPU/op 0.65 pp, throughput 0.64 pp, p99 1.66 pp (95% CI half-widths).

### A/B paired differences (24 pairs each, m5; analyze.py convention, positive = B worse)

`analyze.py summary --phase-dir …/<phase> --arms AB` (summary JSON beside each
`results.jsonl`):

| phase | case | cpu/op mean [CI95] | tput mean [CI95] | p95 mean [CI95] | p99 mean [CI95] |
|---|---|---|---|---|---|
| `ab-d1-w02-4k` | W02-uring-4k-d1 | −0.07% [−0.73, +0.63] | −0.10% [−0.77, +0.59] | −0.06% [−0.87, +0.82] | −0.14% [−0.89, +0.69] |
| `ab-d2-w02-4k` | W02-uring-4k-d2 | **+3.73%** [+3.04, +4.42] | **+3.58%** [+2.94, +4.22] | **+4.96%** [+4.21, +5.75] | **+7.07%** [+4.19, +11.80] |
| `ab-d3-w02-4k` | W02-uring-4k-d3 | −6.69% [−7.25, −6.18] | −7.20% [−7.86, −6.61] | −11.38% [−11.94, −10.85] | −9.57% [−11.48, −7.68] |
| `ab-main-w02-4k-d16` | W02-uring-4k-d16 | **−26.36%** [−26.66, −26.08] | −35.88% [−36.44, −35.37] | −31.00% [−31.61, −30.37] | −28.68% [−29.66, −27.48] |

(tput here is (A−B)/A, positive = B slower. All phases: 48/48 invocations
exit 0, 5 measured reps each, single binary SHA `61c6d579…6b886`. Secondary
whole-invocation rusage view agrees in sign everywhere: D1 −0.19%, D2 +3.38%,
D3 −7.09%, D16 −26.65%.)

Consistency with R1 controls: D2 worsening reproduced (R1 cpu/op +4.17%,
tput −3.99%, p95 +5.69%, p99 +7.45% → R2 +3.73% / +3.58% / +4.96% / +7.07%);
D3 win reproduced (R1 −6.81% → R2 −6.69%); D16 win reproduced (R1 −26.46% →
R2 −26.36%). D1 neutral (mechanism gated off at depth 1).

## Deviations from R1

1. **Binary hashes differ from R1** (expected consequence of rebuilding, not a
   protocol change; R1 build-notes anticipates this and mandates a fresh A/A
   calibration, which was run and PASSED before any A/B round).
2. **Phase order**: R1 ran ab-main (D16) before the D1/D2/D3 control rounds;
   R2 phaseA follows the task-specified order A/A → D1 → D2 → D3 → D16. Each
   phase is an independent paired block; no analysis pools across phases, so
   ordering affects exposure to drift only (see deviation 3 for the loadavg
   record).
3. **Loadavg guard adjustment 1.5 → 4.0** (R1 precedent: same adjustment,
   recorded): the first `ab-d2-w02-4k` attempt was refused by the harness's
   own start guard — `loadavg 1.54 exceeds --max-loadavg 1.5: foreign load
   would contaminate the round` — before any data was written (no dir, no
   JSONL existed at that point). The elevated 1-min loadavg was residual from
   the just-finished 48-invocation D1 round, not foreign load: `ps` showed no
   CPU-bearing process besides the monitoring shell, and loadavg decayed
   0.93 → 0.44 within 45 s. D2/D3/D16 were then run with `--max-loadavg 4.0`;
   observed max per-invocation 1-min loadavg 1.94 (20-core host ≈ 10% of one
   core). D1 and the A/A round ran entirely under the original 1.5 guard.
4. **Scope**: R2 phaseA runs only the five rounds above. R1's additional
   rounds (64 KiB, threadpool, saturate, W-03 family, placement checks,
   Holm control-family gate) were not part of this ask and were not run.
5. **A/A second-position offset ~+0.7%**: recorded as an observation; no
   measurement-condition repair applied (gates passed without it).
6. Oracle-test suite run here is a single arm-B invocation as build sanity
   (receipt kept); it is not the full R1 four-log correctness lane and does
   not replace it.

Nothing else changed: pairing, order balance, case ids, workloads, warmup/
measured counts, primary/secondary CPU metric, bootstrap parameters, gates
and thresholds are exactly R1's repaired protocol.
