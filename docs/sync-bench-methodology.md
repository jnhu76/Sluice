# Sync benchmark methodology

**Status: SYNC-IO-COMPLETE Phase 9 (sync doc reconciliation).** Defines how the
blocking W1–W4 benchmark matrix is run and interpreted. It extends (does not
replace) `docs/bench-methodology.md` and `docs/bench-decision-matrix.md`, which
cover the existing single-stream benches. **No universal performance claim is
made or implied.** Results are per-workload, per-machine, per-parameter.

## 1. Core principles

```text
- NO universal performance claims. Every number is scoped: workload x mode x
  parameter set x machine.
- blocking_sequential is a WEAK baseline (one thread, serial). It is shown for
  reference, not as the basis of an async comparison.
- blocking_bounded_pool is the ENGINEERED PORTABLE baseline (BlockingIoPool, fixed
  std::thread workers). This is the primary row future async compares against.
- blocking_thread_per_stream is a STRONG-but-EXPENSIVE baseline (one worker per
  stream). Useful as an upper bound; pays context-switch cost.
- future async MUST compare against the engineered blocking baselines
  (bounded_pool, optionally thread_per_stream), NOT only sequential. Sequential
  rows may be shown for reference but must not anchor the comparison.
- benchmark conclusions REQUIRE a concurrency dimension (streams / pool_threads).
  Single-stream numbers cannot answer "does async help?"
```

## 2. Modes (execution models)

Every workload supports the relevant modes (see `docs/sync-bench-matrix.md`):

```text
blocking_sequential        one caller thread, serial jobs
blocking_bounded_pool      fixed std::thread workers (BlockingIoPool)
blocking_thread_per_stream one worker thread per stream
sync_uring_spike           only if present + build-gated (SLUICE_HAS_LIBURING);
                           a comparison row, NEVER the default and NEVER async
```

`sync_uring_spike` is the existing synchronous-over-uring spike (job 013). It is
**not async** (it blocks per op). It appears only as a build-gated comparison row.

## 3. Workloads

The four workloads (defined in `docs/async-problem-statement.md` W1–W4, mirrored
here on the blocking side):

- **W1 — many independent writes.** `streams` writers, each writing
  `blocks_per_stream` blocks of `block_size`. `file_layout` =
  `many_files` | `one_file_many_offsets` (the latter exercises positional I/O,
  job 018S). `sync_policy` from `docs/sync-durability-model.md`.
- **W2 — many independent reads.** `streams` readers reading pre-written files.
- **W3 — many copy streams.** `streams` independent reader→writer copies,
  `bytes_per_stream` with `buffer_size`.
- **W4 — durability / sync policy.** Like W1 but the variable of interest is
  `sync_policy`; blocking "overlap" is via pool workers (see
  `docs/sync-durability-model.md` §5).

Full parameter tables are in `docs/sync-bench-matrix.md`.

## 4. CSV output fields

Required:

```csv
mode,workload,streams,pool_threads,block_size,buffer_size,total_bytes,total_ops,total_ms,mbps,ops_per_sec,threads_used,sync_policy,file_layout
```

Add where feasible (omit consistently or leave blank + documented if unavailable):

```csv
p50_us,p95_us,p99_us,user_cpu_ms,sys_cpu_ms,voluntary_ctx_switches,involuntary_ctx_switches,jobs_submitted,jobs_completed,max_queue_depth
```

These extend the existing `BenchResult`/CSV shape (`bench/bench_common.hpp`) with
concurrency fields (gap G6: active-streams / per-cell concurrency, which the
required `streams`/`pool_threads`/`threads_used` columns provide).

## 5. What is NOT measured

```text
- async / evented backends           (gated behind sync-first; async bench is its own job)
- networking, timers, mmap           (out of scope, 016B O1-O5)
- physical disk durability          (filesystem/hardware dependent)
- universal throughput/latency claims
```

## 6. Reproducibility

```text
- Each run records: workload, mode, all parameters, machine info, sluice build
  (debug/release, sanitizer mode if any).
- Results notes live under docs/results/ (per the convention in
  docs/io-uring-liburing-validation.md).
- Sanitizer runs (ASan/TSan) are for correctness of the pool/concurrency code,
  NOT for performance numbers (sanitizers skew timing).
```

## 7. Cross-links

- Matrix (workloads × modes × parameters × metrics × decision questions): `docs/sync-bench-matrix.md`.
- Architecture (modes defined): `docs/sync-io-architecture.md` §3.
- Durability policy names: `docs/sync-durability-model.md` §4.
- Existing single-stream methodology: `docs/bench-methodology.md`, `docs/bench-decision-matrix.md`.
- Optimization notes (observations → future work): `docs/sync-optimization-notes.md`.
- Jobs: `docs/sync-io-next-jobs.md` (022S matrix, 023S optimization).
