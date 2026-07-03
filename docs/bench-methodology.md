# Core microbench methodology

**Status: SLUICE-CORE-010A.** This is the *first* task allowed to produce
measurement data, but it still makes no broad claims.

```text
Microbench results are local, workload-specific observations.
They are not general performance claims.
```

## 1. Scope

A separate core microbenchmark harness (`bench/`, wired as the `bench` xmake
group) measures the core paths built so far: small writes, copy strategies, the
buffered fast path, vector WAL write, and a sync smoke. It is **separate from
existing benchmark logic** and must not rewrite old benchmarks. Results are CSV.

## 2. What is measured

- **small_writes_bench** — raw `FileWriter` vs `BufferedWriter` vs observed-
  buffered vs vector, across chunk sizes (1B/8B/64B/512B/4KB).
- **copy_strategy_bench** — Scratch / BufferedFirst / Auto across input sizes
  (4KB/64KB/1MB/16MB) and limit cases (unlimited/small/exact).
- **wal_write_bench** — scalar `write_record` vs vector `write_record_vec`,
  buffered variants, payload sizes (16B/128B/1KB/16KB), durability modes
  (no_sync / flush_each_batch).
- **sync_smoke_bench** — flush_only / sync_data / sync_all, low iterations.

## 3. What is not measured

- Not io_uring (not implemented).
- Not async/evented backends (not implemented).
- Not broad application workloads — these are isolated core-path microbenches.
- Not cross-machine comparability — see §6.

## 4. Data output format

CSV. Columns (010B):

```text
case,mode,bytes,iterations,elapsed_ns,
read_syscalls,write_syscalls,
read_syscall_bytes,write_syscall_bytes,
buffered_fast_path_bytes,scratch_path_bytes,
copy_loop_iterations,
read_vec_calls,write_vec_calls,
sync_data_calls,sync_all_calls
```

Each bench prints a header row then one row per (mode, size, limit) combination.

## 5. Warmup/repetition policy

Modest, fixed policy to keep runtime low and results stable enough to reason
about, not to reach statistical rigor:

- A small warmup pass is run and discarded before timed iterations (primes
  caches/allocators).
- Each measured cell runs a fixed iteration count chosen so the bench finishes
  in seconds, not minutes.
- Median across a few timed repetitions is computed by the summary script
  (011B) when rows repeat; the bench itself emits raw rows.

## 6. Filesystem/cache caveats

- Results depend heavily on the filesystem (tmpfs vs ext4 vs WSL2 overlay), page
  cache state, and disk. **Record the filesystem in the runbook** (011A).
- File benches write to temp files; OS caching means "write" time may not
  reflect stable storage. Sync benches isolate the sync cost separately.
- Re-running on the same machine is the only fair comparison; absolute numbers
  are not portable across machines.

## 7. Durability caveats

- `flush()` is NOT durability (it only drains buffered bytes).
- `sync_data()` / `sync_all()` request OS persistence; actual persistence
  depends on OS/filesystem/disk and may lie (e.g. fsync on some configs).
- The WAL bench's `flush_each_batch` mode measures flush cost, NOT durable
  commit cost. An fsync-heavy mode is deliberately omitted/optional because it
  is slow and environment-sensitive.

## 8. No broad claims rule

```text
After SLUICE-CORE-010, performance words are allowed only in benchmark
docs/results and must be phrased as measured, local, workload-specific
observations.
```

Do not claim one strategy "is faster" in general. Use "on this machine, in this
run, for this workload, mode X observed Y ns". The optimization decision matrix
(011) turns these observations into cautious, evidence-linked rules — never
universal claims.
