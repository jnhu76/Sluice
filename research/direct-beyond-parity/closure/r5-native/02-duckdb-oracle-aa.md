# R5 DuckDB Native — Oracle re-verification and A/A (serial lane) [RUN]

Date: 2026-10-29, 09:56–10:18 +08:00. Serial lane, load logged per run.
Binary: `build/duckdb-native/069cc9f9b5be802405797faecc284961b07c70ef/duckdb`
and its `benchmark/benchmark_runner` (upstream CMake build, GCC 16.2.1);
`duckdb --version` prints `v0.0.1 (Unknown Version) 069cc9f9` (depth-1 tag
display artifact — identity is the SHA, per prep record).
Dataset: `duckdb_benchmark_data/lineitem.parquet`, 207,130,013 B,
SHA-256 `b7583e65…0ef911` re-verified at lane start.

## SQL oracle re-verification [RUN]

Two fresh runs (`lane-r1`, `lane-r2`) of `benchmarks/duckdb-r0/oracle.sql`,
per protocol `README.md`:

| Query | lane-r1 SHA-256 | lane-r2 SHA-256 | prep-record |
|---|---|---|---|
| q1 | `54adfd2036d70b5566b591068599d4fd4e14deab0ab59fce000ac73b32258dca` | same | same |
| q2 | `6b7830ddc1bba06406e183ecf9938b005bac1deac33556bc18f49af3037c9189` | same | same |
| q3 | `673ce9f44c8f998b30b70c993bebfaf2cd7c68723c2c61c07b23807a6896d1c6` | same | same |
| q4 | `26788f24487c49629265bf0e227ebbeeb0d2a87f0a59ecd7639cea4b3a75b41e` | same | same |

All four hashes byte-identical across runs and equal to the committed
prep-record hashes. q2 groups match canonical TPC-H sf=1
(A/F 1,478,493 · N/F 38,854 · N/O 3,004,998 · R/F 1,478,870;
total 6,001,215 rows). Small outputs archived at `duckdb/oracle-csv/`;
q3 hash at `duckdb/oracle-csv/q3.csv.sha256`.

Byte/value oracle [RUN]: every A/A and cold-timing invocation of the official
benchmark re-verified its upstream `result` oracle
(`226829357828.867781`, `benchmark/tpch/parquet/parquet_projection_direct.benchmark`);
v1.5.6 prints a `…\tINCORRECT` line on result mismatch
(`third_party/duckdb/benchmark/benchmark_runner.cpp:122`) — zero INCORRECT
lines across all **62** benchmark-runner invocations in this lane
(30 W1 + 30 W1b + 2 cold-timing). The CLI aggregate also printed the
exact expected value in the strace runs.

## A/A protocol and results [RUN]

Three pre-declared workloads, all `taskset -c 2-9` except W1 (see below),
serial, sleep 1 s between invocations, per-invocation oracle gate
(exit code + INCORRECT scan / CSV hash equality):

| Workload | Definition | n invocations | adjacent pairs |
|---|---|---|---|
| W1 | official benchmark, upstream default config (threads = hardware concurrency = 20, all cores) | 30 (× 5 internal repeats) | 29 |
| W1b | official benchmark, controlled config: `taskset -c 2-9 --threads=4` | 30 (× 5 internal repeats) | 29 |
| W2 | full 4-query oracle suite through the CLI (scan + sort + CSV write), `taskset -c 2-9`, whole-process wall time, hash-gated | 16 | 15 |

Raw outputs: `duckdb/aa-w1/`, `duckdb/aa-w1b/`, `duckdb/aa-w2/run-log.md`
(each includes per-run start time and load).

| Metric | W1 (default) | W1b (pinned, of record) | W2 (oracle suite wall) |
|---|---|---|---|
| unit | ms, median of 5 repeats | ms, median of 5 repeats | s, whole process |
| mean | 20.813 | 35.887 | 0.296 |
| median | 20.919 | 35.641 | 0.296 |
| sd | 1.537 | 1.602 | 0.005 |
| CV | 7.39% | 4.46% | 1.67% |
| min–max | 17.4–23.8 | 33.0–39.0 | 0.288–0.305 |
| adjacent-pair spread mean | 8.71% | 4.90% | 1.49% |
| adjacent-pair spread max | 23.87% | 13.86% | 4.08% |
| pairs | 29 | 29 | 15 |

**A/A verdict.** ≥15 pairs achieved on every workload (29/29/15; 73 total).
The controlled baseline of record is W1b: invocation-level CV 4.5%, adjacent
pair spread mean 4.9% (max 13.9%). W2 shows the same host is much quieter at
the ~0.3 s scale (CV 1.7%, max pair 4.1%), so W1b's per-invocation jitter is
dominated by the 20–40 ms scale of a 4-thread run on shared cores, not by a
noisy-storage or noisy-host problem (the fio buffered section, a pure storage
shape, sat at CV ≤1.6%). Statistical limits: with W1b pair noise (sd of
relative pair deltas ≈ 5%), a paired A/B at n=15 resolves effects of roughly
±3–5%; the W2 suite resolves ~±1%. Anything smaller needs more pairs or the
W2-style larger workload. The unpinned default config (W1) is the worst case:
CV 7.4%, max pair 23.9% — future adapter A/Bs on this host should pin cores
and declare thread counts, or they inherit this noise.

Honest gap: no A/B was run here (this is the R0 noise assessment only, per
§8.3), and W1b's MDE statement is derived from observed pair noise, not from a
power analysis with declared α against a preregistered effect size.

## Page-cache timing impact [RUN]

Two cold invocations of the W1b configuration, each immediately after
`sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'` (measurement-only sudo,
see 00-environment.md), vs the warm W1b distribution:

- cold run 1 repeats (ms): 48.7, 41.1, 38.5, 35.7, 35.4 (0 INCORRECT)
- cold run 2 repeats (ms): 43.5, 43.1, 36.6, 35.4, 35.1 (0 INCORRECT)
- warm W1b repeats settle at ≈ 34–37 ms (medians 33.0–39.0)

First repeat after drop_caches pays ≈ +25–40% (8–14 ms) over warm steady
state; by the third repeat the timing is back inside the warm band. The
37.17 MB the query reads (below) move at memory speed once cached; the cold
penalty is the device pass. Raw: `duckdb/cold-timing/`.
