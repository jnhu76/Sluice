# causal-r2 cost-accounting provenance

Round: #595 E5 causal-r2 phase B (2026-11-04), low-overhead cost accounting.
This file records the machine-readable provenance; the method is in
`counters/cost_table.md` §1.

## Binaries (sha256)

```
61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886  bench_w02_pipeline_frozen
6670d8dee37e8d8faa60549dfc5e1f6121cc4f56ee66cf58fa91cc678d16db09  bench_w02_pipeline_pmu
b9a4994631dbe6714e630fec838fd1d2b98604bb872007e6882bad96f1eb3a3a  bench_w02_pipeline_counters
```

The frozen binary byte-matches the phaseA record
(`evidence/raw/phaseA/binary-sha256.txt`). The pmu binary has no counter code
(`SLUICE_595_COUNTERS` undefined); the counters binary is built with
`SLUICE_595_COUNTER_BUILD=1`.

Reproducibility check (run after the measurements): a clean
`xmake f -c -y --liburing=y -m release` + `xmake build -y bench_w02_pipeline`
without the counter env produced `6670d8de…db09` (byte-identical to the saved
pmu binary); the same with `SLUICE_595_COUNTER_BUILD=1` produced
`b9a49946…3a3a` (byte-identical to the saved counters binary). The two build
configurations were verified to produce zero `-DSLUICE_595_COUNTERS` in
`compile_commands.json` (pmu) and 959 occurrences (counters), respectively.

## Source

- Worktree: `/home/jnhu/sluice-595-e5/causal-r2/worktree`
- Base: `271b81bfc9d64eecc0266fe7066b9040525dd710`
- `git diff HEAD` sha256 (R1 research patch + this round):
  `27a6b2a049554c995ad26a9a07699648dcaa5b31d9d01ba9a06534b426b76c9e`
- Counter header: `include/sluice/async/detail/research_595_counters.hpp`
- Insertion points: see `cost_table.md` §3.2.
- Research-only bench modes: `SLUICE_BENCH_PMU_ONLY`, `SLUICE_BENCH_REUSE_FIXTURE`
  (`bench/bench_common.hpp`, `bench/bench_w02_pipeline.cpp`). Public API and v2
  contract unchanged; nothing outside the causal-r2 copy was modified.

## Environment

- Kernel `7.2.11-200.fc44.x86_64`; Xeon E5-2666 v3; 20 logical CPUs; one NUMA node.
- perf `7.2.11-200.fc44.x86_64`; g++ 16.2.1; xmake v3.1.2+HEAD; liburing 2.13-2.
- `perf_event_paranoid=2`; no sudo/CAP_PERFMON in this session. PMU runs use
  user-mode events only. Kernel-mode PMU is UNOBSERVED (see cost_table §7).
- Bench pinned `taskset -c 2`; harness/sampler pinned cpu3; perf process cpu3.
- Fixture: 64 MiB pattern file on `/tmp/sluice-595-cost` (warm tmpfs).
- Loadavg before first block 0.42/0.47/0.42; after final block 0.86/1.35/1.22.

## Blocks (all raw results under `raw/<label>/results.jsonl`)

| label | binary | mode | case | pairs | config |
|---|---|---|---|---|---|
| `pmu-d2-ab` | pmu | perf stat (`:u` events) | W02-uring-4k-d2 | 12 | warmup=1 measured=1, PMU-only, reuse fixture |
| `pmu-d2-aa` | pmu | perf stat | W02-uring-4k-d2 | 8 | A/A, same |
| `pmu-d1x-ab` | pmu | perf stat | =W02-uring-4k-d1 | 8 | warmup=1 measured=1, PMU-only |
| `split-d2` | pmu | direct + /proc sampler | W02-uring-4k-d2 | 12 | warmup=1 measured=20, PMU-only, reuse |
| `split-d1x` | pmu | direct + /proc sampler | =W02-uring-4k-d1 | 8 | warmup=1 measured=10, PMU-only |
| `cnt-on-full-d2` | counters | direct + /proc sampler | W02-uring-4k-d2 | 12 | warmup=1 measured=5, counters=1 |
| `cnt-off-full-d2` | counters | direct + /proc sampler | W02-uring-4k-d2 | 12 | warmup=1 measured=5, counters=0 |
| `cnt-on-pmu-d2` | counters | direct + /proc sampler | W02-uring-4k-d2 | 8 | PMU-only, counters=1 |
| `cnt-off-pmu-d2` | counters | direct + /proc sampler | W02-uring-4k-d2 | 8 | PMU-only, counters=0 |
| `frozen-full-d2` | frozen | direct + /proc sampler | W02-uring-4k-d2 | 6 | warmup=1 measured=5 |
| `cnt-on-full-d1x` | counters | direct + /proc sampler | =W02-uring-4k-d1 | 6 | warmup=1 measured=5, counters=1 |
| `cnt-off-full-d1x` | counters | direct + /proc sampler | =W02-uring-4k-d1 | 6 | warmup=1 measured=5, counters=0 |

Discarded blocks (D1 substring filter): see `raw/DISCARDED.md`.

## Commands

```
python3 -I evidence/counters/tools/run_blocks.py --label <label> --mode {pmu,direct} \
  --bin evidence/counters/bin/<binary> --pairs N --arms {AB,AA} --case <case> \
  [--pmu-only] [--counters {0,1}] [--warmup 1 --measured M] \
  --outdir evidence/counters/raw/<label>
python3 -I evidence/counters/tools/analyze_cost.py > evidence/counters/analysis/summary.json
```

Analysis: pair-relative differences, bootstrap 95% CI (10,000 resamples, seed
5950, percentile). CPU/counter/tail Δ% = (B−A)/A; throughput Δ% = (B−A)/A.
A/A block: second-minus-first.

## C3 round additions (2026-11-04, kernel/io-wq branch)

Binaries:

```
17463539a54d3e68157fc486a84531ea3b793276a2b98892b70fd269ebe2cae8  bench_w02_pipeline_c3
61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886  bench_w02_pipeline_frozen (unchanged, reused for sysio-d2)
```

- `bench_w02_pipeline_c3`: built in the same research worktree with
  `SLUICE_595_C3_BUILD=1 xmake f -c -y --liburing=y -m release && xmake build bench_w02_pipeline`
  (C1 probe macro off). Probe header
  `include/sluice/async/detail/research_595_c3.hpp`; wiring at the same bench
  anchors as C1 (`bench/bench_w02_pipeline.cpp` enable/reset/dump/init); probe
  diff: `../traces/c3-kernel/probe-source-diff.txt`. Env gate `SLUICE_595_C3=1`.
- Blocks (each run serially, machine otherwise idle, cpu2, warmup=1 measured=5,
  100k measured op/invocation, fixture reuse):
  - `raw/sysio-d2/`: 24 invocations (12 A/B pairs), frozen binary, no probes;
    external 1 ms sampler additionally reads `/proc/<tid>/io` (run by
    `tools/run_sysio_blocks.py`).
  - `raw/c3-interleaved-d2/`: 48 invocations (12 pairs × on/off × A/B, C1-style
    interleave), c3 binary, env-gated (`tools/run_c3_blocks.py`,
    analysis `tools/analyze_c3.py`, JSON `analysis/c3-interleaved-d2.json`).
- All 72 invocations rc=0; sampler errors ≤2 per invocation; TSC calibration
  2,893,269–2,893,289 kHz per invocation.
- Findings: `C3_COUNTERS.md`; trace-level observations: `../traces/c3-kernel/`.
