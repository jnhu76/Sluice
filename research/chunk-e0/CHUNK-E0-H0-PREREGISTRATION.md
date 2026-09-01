# CHUNK-E0 Phase H0 — chunk-size × depth sweet-spot map, Host-0 (#270)

Execution authority: issue #270 (roadmap #259; source finding = PR #269 /
ALIGN-E1 #268 — "chunk_size is a materially stronger lever than alignment;
1 MiB was only the tested boundary, not a proven sweet spot").

Status: **FROZEN** (preregistered BEFORE any formal measurement of the
16K..4M campaign). Allowed changes after this point: additive AMENDMENTs
only (appended, never rewritten). The scientific question, scope, host,
workload, matrices, metrics, same-work contract, ordering, repetitions,
sweet-spot definitions, Pareto rule, stop/extension rule, verdict rules and
production stop gates are frozen below.

The only pre-registration activity was a tiny smoke/feasibility probe
(recorded in this file, §5.1) that selected the total workload bytes — as
authorized by #270 Phase H0 ("在 preregistration 前允许做极小
smoke/feasibility test 来决定").

## 1. Question

For Sluice's CURRENT production buffered READ + WRITE copy engine on the
Host-0 bare-metal x86-64 machine, what is the real performance surface of
`chunk_size × pipeline_depth`, and where — if anywhere — does it plateau /
show a knee / form a Pareto sweet region?

H0 establishes ONLY the host-local reference surface and method. It makes
NO cross-host claim, NO production default change, NO public API knob, NO
runtime adaptation.

## 2. Scope — hard exclusions (frozen)

This campaign measures ONLY:

```text
production buffered READ + WRITE copy engine
   (apps/sluice-copy/copy_task.cpp,
    run_pipelined_copy_with_backend + ThreadPoolBackend)
workers = 1   (production CLI default)

variables:
   chunk_size    (16 KiB .. 4 MiB)
   pipeline depth (1, 2, 4, 8)
```

Prohibited in this campaign (any violation invalidates the run):

```text
alignment experiment            registered/fixed buffers
BufferPool redesign             splice
copy_file_range                 O_DIRECT
SIMD                            multi-worker (workers != 1)
runtime autotuner               production default modification
```

#267 (methodology-only environment/amplification track) and #262
(io_uring multi-worker correctness) are NOT touched. H0 workers=1
buffered-copy does not depend on #262.

## 3. Host / environment (Host-0)

Captured per session in `environment.json` (driver-side; values below are
the Host-0 baseline facts, as collected on 2026-09-01):

```text
HOST-LOCAL RESULT ONLY — this verdict belongs to the combination:
  Haswell-EP Xeon E5-2666 v3 (10C/20T, 1 socket, 62 GiB RAM, 1 NUMA node)
  × Fedora 44 (kernel 7.1.9-200.fc44.x86_64)
  × glibc 2.43 / clang (release) / xmake
  × btrfs (compress=zstd:1, ssd, discard=async, space_cache=v2, subvol=/home)
  × SATA SSD (GS-480, 480 GB) — /home on /dev/sda3
  × intel_cpufreq schedutil governor, turbo ENABLED (no_turbo=0)
  × bare metal (no virtualization)
  64 B cache line / 4 KiB page
  L1d 32K/core · L1i 32K/core · L2 256K/core · L3 25 MiB (20-way)
```

H0 does NOT attribute the surface to any single layer. No "x86 sweet
spot", "Linux sweet spot" or "Haswell universal optimum" wording is
allowed anywhere in the report. The report reserves future cross-host
schema fields (H1 newer x86-64, H2 ARM64) but does not implement them.

## 4. Workload (frozen)

- READ + WRITE file copy of a fixed total of **1 GiB = 1 073 741 824 B**
  (1073741824 B) per run, from the per-run O_TRUNC destination to the src.
- Source: deterministic pseudo-random bytes (the TAX-0 / ALIGN-E1-line
  generator: 4 KiB splitmix64 master block, seed
  `0xE1E1E1E121212121`), generated once per session by `--generate`;
  same bytes for EVERY cell and every run.
- Buffered (page-cache) I/O, warm page cache, native Host-0.
- Fail-closed integrity: driver computes `sha256(src)` once per session
  (cached) and `sha256(dst)` after every run; `dst != src` fails the run
  closed (see §7).
- Sync policy: `SyncPolicy::none` (no fdatasync/fsync in the timed span —
  this campaign maps the buffered-copy surface, not durability).
- Every formal cell copies the SAME total useful bytes; no cell "cheats"
  by processing less data.

### 5.1 Smoke/feasibility probe (pre-registration, 2026-09-01)

Authorized by #270 ("允许做极小 smoke/feasibility test 来决定 [512 MiB /
1 GiB / 2 GiB]"). Ran the engine at the candidate totals for the slowest
(16K) and fastest (4M) cells, plus two interaction points; measured the
IN-PROCESS engine span (`total_ns`, the metric):

```text
chunk    depth  512 MiB      1 GiB        2 GiB
16K      d1     2.36 s       4.50 s       9.54 s     (215-228 MiB/s all)
4M       d1     0.71 s       1.50 s       2.91 s     (682-719 MiB/s all)
1M       d4      —           1.27 s       —          (809 MiB/s)
64K      d8      —           1.64 s       —          (624 MiB/s)
```

Engine span scales linearly with total bytes; 1 GiB gives every cell a
span of 0.7-4.5 s, all comfortably above startup/teardown (the fixed
Runtime build/start/join is ~1-3 ms, < 0.5% even of the 4M d1 cell).
Driver-side sha256 of 1 GiB ≈ 1.2 s, uniform per run. 512 MiB would leave
the fast cells at ~0.7 s (perf measurement edge); 2 GiB doubles campaign
wall time with no accuracy gain. **TOTAL WORKLOAD BYTES FROZEN = 1 GiB.**

## 6. Matrices (frozen)

- **Chunk sweep** (primary x-axis), 15 values:

```text
16K 32K 64K 96K 128K 192K 256K 384K 512K 768K 1M 1.5M 2M 3M 4M
(16384, 32768, 65536, 98304, 131072, 196608, 262144, 393216,
 524288, 786432, 1048576, 1572864, 2097152, 3145728, 4194304)
```

  16K / 32K / 64K are historical continuity anchors only (they overlap
  the ALIGN-E1 range). The 4K/6K/8K/12K ALIGN-E1 matrix is NOT re-run.
  The focus search region is **64K → 4M**.

- **Depth sweep**: 1, 2, 4, 8 (pipeline depth — pre-submitted read
  window). Workers = 1 for every cell.
- Cell count per round: 15 chunks × 4 depths = **60 cells**.
- **in_flight_bytes = chunk_size × depth** is a first-class resource
  metric (recorded per cell, used in the Pareto analysis §11), NOT a
  side statistic. Maximum cell exposure: 4M × 8 = 32 MiB (well under the
  production `kMaxPipelineBytes` = 512 MiB cap; all cells pass the
  production resource limits by construction, checked fail-closed in the
  bench).

## 7. Metrics (frozen)

Primary:

```text
wall time          total_ns (in-process, full engine span: Runtime
                   build/start/submit/wait/drain/join + copy)
throughput         median MiB/s = bytes / total_ns
instructions/byte  perf instructions:u / bytes
instructions/chunk perf instructions:u / chunks
in_flight_bytes    chunk × depth (deterministic per cell)
```

Secondary (recorded per run, reported per cell as median):

```text
RSS                ru_maxrss (KiB, peak)
user/sys CPU time  getrusage utime_us + stime_us delta
minor faults       ru_minflt
major faults       ru_majflt
```

PMU rule (frozen): `instructions:u` is the quantitative instruction pair
(stable on this host per ALIGN-E0/ALIGN-E1). `cycles:u` is recorded but
**DEMOTED by default** — the host runs intel_cpufreq schedutil with turbo
ENABLED (no_turbo=0), the known frequency-scaling confounder. The
validation session runs a 3-rep consecutive stability probe; ONLY if the
probe shows a non-negative consecutive per-op double-difference at both
{16K, 4M} × d1 is cycles upgraded to secondary evidence. No IPC story is
written with demoted counters.

## 8. Same-work contract (fail-closed)

Bench-side gates (any violation → exit 3, semantic failure):

```text
bytes_copied == 1073741824
write_ops   == ceil(1073741824 / chunk)
read_ops    in [ceil(1073741824 / chunk), ceil + depth]
short_writes == 0
```

Driver-side gates (any violation → run fails closed, NOT aggregated):

```text
bench exit 0
perf exit 0
instructions:u present and > 0
dst_sha256 == src_sha256
run not already recorded
```

`INVALID` / `FAIL CLOSED` runs never enter performance aggregation.

## 9. Ordering / repetitions (frozen)

- **R = 7 rounds.** In every round the 60 cells run in a fresh seeded
  Fisher–Yates permutation (seed = `0xE1E1E1E121212121 + round`), so no
  chunk/depth block drifts against another in time and adjacent-run
  machine drift is interleaved across all cells.
- Per-cell statistics over n = 7 samples: **median** and **MAD** (median
  absolute deviation). Raw repetitions are preserved in `runs.jsonl`.
- One run = one invocation of the bench under `perf stat -x, -e
  instructions:u,cycles:u,task-clock`. Wall is measured in-process around
  the full engine span; perf attach cost affects every cell uniformly and
  never enters the timed span.
- No post-hoc repetition tuning: if the feasibility probe had shown
  noise too high to resolve the frozen rules, the change to R=9 had to be
  made before the campaign; the probe (1 GiB, §5.1) shows the fast cells
  at 0.7-1.5 s with 256-1024 chunks — resolution is adequate at R=7.
  After the campaign starts, repetitions are NOT adjusted by looking at
  results.

## 10. Sweet-spot definitions (frozen, per depth)

Over the 15 tested chunks (16K..4M) with median MiB/s:

- **TESTED_RANGE_PEAK**: the chunk with the highest median MiB/s (ties →
  smallest chunk). If the peak sits at the 4 MiB boundary, it is named
  TESTED-RANGE PEAK — NEVER "optimum".
- **SMALLEST_WITHIN_95_PERCENT_OF_PEAK** (95% point): the SMALLEST chunk
  whose median MiB/s >= 0.95 × the depth's tested peak.
- **PLATEAU_ENTRY** (deterministic, no eyeballing): for adjacent tested
  chunks c_i < c_{i+1}, let `gain(c_i) = MIBPS(c_{i+1})/MIBPS(c_i) - 1`;
  a pair is FLAT iff `gain(c_i) < 0.03` (3% material threshold). The
  plateau-entry candidate c* is the SMALLEST chunk such that at least TWO
  consecutive FLAT pairs exist at and after (c*, next). If no such c*
  exists (the curve is still materially rising at the 3M→4M boundary),
  PLATEAU_ENTRY is reported as NOT LOCATED — which feeds the stop /
  extension rule (§13). Any plateau claim is bounded to the tested range;
  flatness beyond 4M is never asserted from this grid alone.
- **KNEE_POINT**: deterministic two-segment least-squares fit of
  (log2(chunk), median MiB/s); breakpoint = interior point (>= 2 points
  per segment) minimizing total SSE. Labeled KNEE only if SSE reduction
  >= 10% vs the single-line fit; else **KNEE NOT LOCATED**. No forced
  knee.

## 11. Pareto rule (frozen)

For every cell, form the 3-tuple over the PRIMARY resource axes:

```text
throughput (median MiB/s)     — maximize
instructions/byte (median)    — minimize
in_flight_bytes (chunk × depth) — minimize
```

Cell A dominates cell B iff A is >= on throughput, <= on
instructions/byte, <= on in_flight_bytes, and strictly better on at
least one. The **PARETO FRONTIER** = the set of non-dominated cells over
the full 60-cell grid. The report separates:

```text
ABSOLUTE PERFORMANCE PEAK      (TESTED-RANGE PEAK cell)
NEAR-PEAK LOW-RESOURCE POINT   (a frontier point near the peak with
                                materially lower in_flight_bytes / CPU)
PARETO SWEET REGION            (the frontier points, named per depth)
```

These are NOT forced into one number. In particular, a configuration
like 512K × d4 vs 1M × d8 is compared explicitly: if the latter is only
1-2% faster but exposes 4× the bytes in flight, the former is reported
as the better engineering operating point.

## 12. Verdict rules (frozen)

Verdict vocabulary (priority order, exactly one primary status):

1. If the tested-range peak sits at the 4M boundary AND the last pair
   (3M→4M) gain >= 0.03 for >= 2 depths → **PLATEAU NOT REACHED —
   EXTENSION REQUIRED** (stop the current campaign; additive AMENDMENT +
   frozen extension grid {6M, 8M, 12M, 16M}; run it; then re-evaluate).
2. Else if PLATEAU_ENTRY is located for >= 2 depths →
   **HOST-LOCAL SWEET REGION LOCATED** (name the region per depth).
3. Else if PLATEAU_ENTRY is located for exactly 1 depth →
   **DEPTH-SPECIFIC REGIMES** (plateau exists only at that depth; report
   per-depth regimes explicitly).
4. Else → **NO STABLE SWEET REGION**.

Production stop gates (frozen, ALL verdicts):

```text
PRODUCTION DEFAULT CHANGE:  NO
CHUNK_SIZE CONTROL-SURFACE PROMOTION: NOT YET
RUNTIME ADAPTATION:         NO
```

No production change is authorized merely by locating H0's optimum
(#270 promotion gate: stable curves → stable regime boundaries →
cross-host portability → control-surface decision → only then bounded
runtime selection).

## 13. Stop / extension rule (frozen)

If 4 MiB is still on a clearly rising limb (verdict rule 1 fires):

1. STOP the current formal campaign (no silent widening to 8M/16M/32M).
2. Write an additive AMENDMENT to this file (grid + rule + reason).
3. FREEZE the extension grid: {6M, 8M, 12M, 16M} (2 097 152 / 4 194 304
   — i.e. 6 291 456 / 8 388 608 / 12 582 912 / 16 777 216 B).
4. Run the extension as a NEW immutable session, same workload
   (1 GiB), same R=7, same metrics, same fail-closed contract.
5. Re-evaluate the frozen rules over the UNION of sessions.

Whether the extension grid is adopted is decided by the existing H0 data
+ the amendment at that time.

## 14. Evidence / session rules (frozen)

- Sessions are immutable: `results/<session-id>/{environment.json,
  manifest.json, gates.json, notes.md, summary.csv, summary.json,
  analysis.json, raw/runs.jsonl, raw/perf.csv}`.
- Raw evidence = ONE append-only `runs.jsonl` (one JSON object per run,
  values preserved) + ONE `perf.csv` (perf `-x,` lines). NO per-run
  files, NO per-chunk/per-depth directory trees — chunk and depth are
  CSV/JSONL columns. The ALIGN-E0 5000+ file pattern is not repeated.
- Claim vocabulary: DIRECTLY MEASURED / CAUSALLY ISOLATED / INFERRED /
  UNRESOLVED; "no effect" is never claimed — only "no consistent effect
  established in the tested range".
- Derived plots are regenerable from summary data; SVG only.

## 15. Structure commitment (frozen)

One new top-level campaign directory `research/chunk-e0/`:

```text
research/chunk-e0/
├── README.md
├── CHUNK-E0-H0-PREREGISTRATION.md
├── CHUNK-E0-H0-REPORT.md
├── scripts/
│   ├── chunk_e0.py
│   └── plot_chunk_e0.py
├── results/<session-id>/...
└── plots/
```

No duplicate campaign dirs (no chunk-bench / chunk-results / chunk-native
/ chunk-final). Bench source lives in the unified `bench/`
(`bench/chunk_e0_bench.cpp`), wired in `xmake/benchmarks.lua` with the
ALIGN-E1 convention (add_deps sluice_core+sluice_async, apps/sluice-copy
include path). Production code untouched.
