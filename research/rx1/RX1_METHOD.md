# RX-1 — Controlled Attribution Falsification Gate: Method

Status: **frozen protocol v1** (see `rx1_protocol_v1.json`; freeze commit recorded below).
Campaign: #234 RX-1, governed by #221 / #225 / #226 / #227 / #235.

## 1. Question and hypotheses

RX-1 is a falsification experiment, not an optimization or observability
campaign. Core question:

> Does Sluice's explicit internal resource information (the AC-1a accessors
> merged in PR #235) materially improve our ability to identify a
> deliberately injected performance bottleneck beyond ordinary external
> system telemetry?

- **H_RX1**: first-class explicit resource state materially improves
  bottleneck attribution over conventional process/kernel telemetry for
  controlled ThreadPool workloads.
- **H0** (null): external telemetry is sufficient, or the L1 signals do not
  improve attribution enough to justify deeper observability work.

The experiment is designed so a negative result is cheap, valid, and useful.

## 2. Hard preconditions (verified before any work)

- clean working tree; `origin/master` = `1e6af08` = the merge commit of PR #235
  (RX1_BASE_SHA = AC1A_MERGE_SHA);
- the four new AC-1a accessors present on master
  (`arena_high_water_mark`, `dispatch_occupancy`, `dispatch_high_water_mark`,
  `active_workers` in `include/sluice/async/threadpool_backend.hpp`);
- `ResourceSnapshot` / `resource_snapshot()` absent (dropped in PR #235 review).

## 3. Environment

WSL2 (kernel 6.18.33.2-microsoft-standard-WSL2), Ubuntu 26.04, AMD Ryzen 7
5800H, 8 logical CPUs, 16 GiB RAM, cgroup v2, PSI available for cpu/io/memory,
perf 7.0.12 with `perf_event_paranoid=2` (user-space-only counters;
context-switch/migration events read 0 — compensated by summing
`/proc/<pid>/task/*/status` and `schedstat` across all threads of the bench
process, which is exactly the process-level view an external observer gets),
iostat unavailable, workload files on tmpfs `/tmp`. CPU/Request/Worker/App
attribution is valid under WSL2 given sound interventions; **device-level
conclusions are not generalized beyond this environment**.

Ambient PSI measured before the experiment: cpu `some` ≈ 2.8k µs/s,
io `some` ≈ 0.5k µs/s, memory 0 (10 s idle window); under-load ambient stays
below ~90k µs/s including spikes. The frozen CPU gate (250k µs/s) sits ~2.7×
above the worst observed ambient and ~2.7× below the weakest I4 reading.

## 4. Design in one screen

```text
workload   : one ApplicationRuntime + ThreadPoolBackend pipeline task
             (rx1_workload_bench; E1-L2 shape, would_block-aware submitter)
interventions (one primary constrained resource each):
  I0 CONTROL      depth 16, capacity 64, workers 4, no restriction
  I1 APP          depth 2  (shallow application pipeline)
  I2 CAPACITY     capacity 4 < offered depth 32 (would_block + retry)
  I3 WORKER       workers 1 (arena generous, ample offered work)
  I4 CPU          bench + 4 pinned busy-loop stressors on the same 2 CPUs
  I5 IO           ENVIRONMENT INVALID / DEFERRED (tmpfs + no iostat + WSL2)
classifiers: interpretable rule systems, identical label set
  C = static config + workload outcome + OS accounting + PSI + perf
  E = C + AC-1a accessor aggregates sampled at OBS-LOW
primary comparison: accuracy(E) - accuracy(C), paired bootstrap 95% CI
```

Static workload configuration (request size, depths, capacities, worker
counts) is exposed to BOTH classifiers identically (task brief §6 I1/§7);
affinity and stress parameters are intervention metadata and never enter
classifier input. The caller-visible submit-rejection count is workload
outcome (any I/O library surfaces it) and is therefore legal for C; the
Sluice-authoritative `arena_capacity_rejections()` delta is E-only.

## 5. Sampling is not a snapshot

AC-1a is pull-based and intentionally has no combined snapshot. The observer
thread calls the accessors sequentially (slot_in_use at t1, outstanding at
t2, active_workers at t3, dispatch_occupancy at t4 …), stamped per row.
Derived quantities are therefore restricted to per-field statistics vs
immutable capacity (`frac_slot_at_capacity`, `frac_active_at_configured`,
means/max/high-water) and counter deltas — never cross-field instantaneous
equalities. Modes: OBS-OFF / OBS-LOW (10 ms) / OBS-HIGH (1 ms); the formal
attribution matrix runs at OBS-LOW; the dedicated tax block measures all
three modes on matched control workloads.

## 6. Pilot (calibration only — never formal data)

Calibration shape: 64 KiB read, 512 MiB, ThreadPool backend. Two pilot
rounds were executed; round 1 findings that reshaped the frozen protocol:

1. **System-wide PSI ambient under load is far above the idle reading** and
   the first run's PSI window was polluted by tmpfs file creation (127–158k
   µs/s) → files are now pre-created and cache-warmed by an unrecorded read
   pass before each phase; the CPU gate is 250k µs/s.
2. **`frac_dispatch_nonzero` is a stagger artifact at 64 KiB** (even balanced
   control shows 0.4–1.0 because µs-scale queue bursts outlive every sample)
   → replaced by mean dispatch occupancy in the worker rule (pilot:
   worker-limited ≈ 28 vs everything else ≤ 6).
3. Control rule needs positive evidence (healthy compute + no internal
   saturation), or CONTROL cells can never be predicted.
4. C's per-worker-utilization worker proxy calibrated to 1.10
   (worker-limited ≈ 1.5 vs balanced control ≈ 0.85 — an external observer
   cannot subtract the driver's CPU share; that asymmetry is part of what
   RX-1 measures).

Pilot verdict table (round 2, all runs correctness-clean):

| Intervention | Actually induced? | Ground truth clean? | Signal captured? | Keep in formal? |
|---|---|---|---|---|
| I0 CONTROL | yes (no restriction; ~12 GB/s) | yes (5/5 valid) | cores/PSI/internal non-saturation | YES |
| I1 APP_PIPELINE_LIMITED | yes (depth 2; ~1.5 GB/s; internals empty) | yes (3/3) | shallow-pipeline + low-compute | YES |
| I2 REQUEST_CAPACITY_LIMITED | yes (49128 rejections; high-water = capacity; frac_at_cap ≈ 1.0) | yes (3/3) | rejections + arena pinning | YES |
| I3 THREADPOOL_WORKER_LIMITED | yes (frac_active 0.78–0.89; queue mean 27–29) | yes (5/5) | worker pinning + deep queue | YES |
| I4 CPU_CONTENDED | yes (PSI 689–906k µs/s; stressor verified) | yes (3/3) | PSI (both classifiers) | YES |
| I5 IO_SERVICE_CONTENDED | ENVIRONMENT INVALID | — | — | NO (DEFERRED) |

Four non-control intervention families cleanly induced (gate requires ≥ 3).
At the calibration point both classifiers scored 19/19 — the calibration
point cannot distinguish C from E; the formal hold-out matrix (other five
shapes, where severity × shape interactions bite) is where the hypothesis
lives or dies.

## 7. Formal matrix (frozen)

- shapes: read/write × {4 KiB (512 MiB), 64 KiB (512 MiB), 1 MiB (1 GiB)};
- interventions: I0–I4; 8 recorded runs per cell (240 attribution runs,
  OBS-LOW); each run = one fresh bench process, 1 unrecorded warmup + 6
  measured internal repetitions;
- cell order randomized per workload block with the committed seed
  (1380708657); controls interleaved by construction;
- observability-tax block: I0 control × {64 KiB, 1 MiB} read ×
  {OBS-OFF, OBS-LOW, OBS-HIGH} × 8 (48 runs, separately randomized);
- validity gates (protocol `invalid_rules`) checked per run before scoring;
  invalid runs are excluded and reported, never scored as failures;
- correctness is fail-closed (word-sum/byte accounting, clean drain/join).

## 8. Analysis and verdict

Scoring: Top-1 accuracy, confusion matrices, per-class precision/recall,
macro-F1, UNKNOWN rate (separate from wrong-cause rate), paired C→E
transition table, paired bootstrap 95% CI for Δaccuracy (10 000 resamples).
Observability tax reported per mode (throughput, p50/p99, CPU cores,
context switches). Verdict gate: the four pre-registered outcomes in the
protocol (`verdict_thresholds`), applied without post-hoc adjustment. All
post-hoc exploration is labeled EXPLORATORY ONLY and never replaces the v1
score.

## 9. Reproduction

```sh
xmake f -m release --toolchain=clang -y && xmake build rx1_workload_bench
python3 research/rx1/scripts/rx1.py self-test    # classifier + scorer pipeline proof
python3 research/rx1/scripts/rx1.py run --phase pilot
python3 research/rx1/scripts/rx1.py freeze       # protocol consistency + SHA256
python3 research/rx1/scripts/rx1.py run --phase formal
python3 research/rx1/scripts/rx1.py classify --phase formal
python3 research/rx1/scripts/rx1.py analyze      # writes results/analysis/
```

Raw run artifacts are immutable; scoring writes `*.scored.json` siblings.
No production source, public API, or observability surface is modified by
RX-1 (scope statement in the PR).

## 10. Freeze record

- protocol file: `research/rx1/rx1_protocol_v1.json` (SHA256 printed by
  `rx1.py freeze` at commit time)
- freeze commit: see PR description (commit message
  `research(rx1): freeze attribution protocol v1`)
- after freeze: classifier rules/thresholds/intervals/severities/matrix/seed
  are immutable; a harness bug requires `protocol_version` bump and a full
  formal rerun.
