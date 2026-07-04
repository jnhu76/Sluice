# Sync benchmark matrix

**Status: SYNC-IO-COMPLETE Phase 9 (sync doc reconciliation).** The blocking
W1–W4 benchmark matrix: workloads × modes × parameters × metrics × decision
questions. Companion to `docs/sync-bench-methodology.md` (which defines *how* to
run); this defines *what* cells exist and *what questions* they answer. **No
universal performance claim.** Results are per-workload, per-machine,
per-parameter.

## 1. Workloads

| ID | Workload | Description |
|---|---|---|
| W1 | many independent writes | `streams` writers × `blocks_per_stream` × `block_size` |
| W2 | many independent reads | `streams` readers on pre-written files |
| W3 | many copy streams | `streams` reader→writer copies, `bytes_per_stream` |
| W4 | durability / sync policy | W1 shape, variable of interest = `sync_policy` |

## 2. Modes (execution models)

```text
blocking_sequential          one caller thread, serial jobs (weak baseline)
blocking_bounded_pool        fixed std::thread workers (engineered portable baseline)
blocking_thread_per_stream   one worker per stream (strong/expensive baseline)
sync_uring_spike             build-gated comparison row only (NOT async, NOT default)
```

See `docs/sync-io-architecture.md` §3 for definitions.

## 3. Parameters per workload

### W1 — many independent writes
```text
streams                number of independent writers
pool_threads           BlockingIoPool size (for bounded_pool mode)
block_size             bytes per write
blocks_per_stream      writes per stream
file_layout            many_files | one_file_many_offsets
sync_policy            none | sync_data_every_file | sync_all_every_file |
                       sync_data_every_batch | sync_all_every_batch | manual
```

### W2 — many independent reads
```text
streams
pool_threads
block_size
blocks_per_stream
file_layout            many_files | one_file_many_offsets
```

### W3 — many copy streams
```text
streams
pool_threads
buffer_size            copy scratch buffer size
bytes_per_stream       total bytes each stream copies
```

### W4 — durability / sync policy
```text
streams
pool_threads
block_size
blocks_per_stream
sync_policy            (the variable of interest — see docs/sync-durability-model.md §4)
```

## 4. Metrics

Primary (required CSV columns per `docs/sync-bench-methodology.md` §4):
```text
total_bytes, total_ops, total_ms, mbps, ops_per_sec, threads_used
```

Add where feasible:
```text
p50_us, p95_us, p99_us,
user_cpu_ms, sys_cpu_ms,
voluntary_ctx_switches, involuntary_ctx_switches,
jobs_submitted, jobs_completed, max_queue_depth
```

Omit consistently or leave blank + documented if unavailable on a target.

## 5. Decision questions

The matrix exists to answer these. Each maps to a comparison across modes/params:

```text
Q1. When does blocking_bounded_pool saturate?
    (throughput vs pool_threads curve flattens)

Q2. How many worker threads are useful before throughput stops improving?
    (the knee of the pool_threads sweep)

Q3. Does positional I/O help one_file_many_offsets workloads?
    (compare many_files vs one_file_many_offsets at equal total bytes;
     exercises read_at/write_at from job 018S)

Q4. Does fsync/fdatasync dominate wall time?
    (W4: compare sync_policy = none vs sync_*_every_batch)

Q5. Does vector I/O reduce syscall pressure?
    (write_all_vec vs scalar write_all at equal bytes; uses existing VectorStats)

Q6. Does thread_per_stream pay too much in context switches?
    (compare blocking_thread_per_stream vs blocking_bounded_pool;
     involuntary_ctx_switches metric)

Q7. Which blocking baseline should future async compare against?
    (the engineered answer: blocking_bounded_pool primarily;
     blocking_thread_per_stream as an upper-bound reference)
```

## 6. Cell selection guidance

You cannot run the full cross-product. Recommended minimal coverage per workload:

```text
- modes: blocking_sequential (reference) + blocking_bounded_pool (primary)
  + blocking_thread_per_stream (upper bound) for at least one cell each.
- streams sweep: 1, 2, 4, 8 (or up to hardware concurrency x2).
- pool_threads sweep: 1, 2, 4, N(hardware concurrency).
- block_size sweep (W1/W2): small (e.g. 64B-4KB) and large (e.g. 64KB-1MB).
- file_layout (W1/W2): both many_files and one_file_many_offsets.
- sync_policy (W1/W4): none + at least one every_file + one every_batch.
```

Record the chosen cells under `docs/results/` with full parameters.

## 7. Cross-links

- Methodology (how to run): `docs/sync-bench-methodology.md`.
- Architecture (modes): `docs/sync-io-architecture.md` §3.
- Durability policy names: `docs/sync-durability-model.md` §4.
- Positional I/O contract (Q3 dependency): `docs/sync-io-model.md` (Positional I/O semantics).
- Optimization notes (observations → future work): `docs/sync-optimization-notes.md`.
- Existing single-stream matrix: `docs/bench-decision-matrix.md`.
- Jobs: `docs/sync-io-next-jobs.md` (022S builds the matrix, 023S tunes).
