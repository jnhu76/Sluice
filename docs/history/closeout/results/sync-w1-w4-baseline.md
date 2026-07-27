# Sync W1–W4 baseline results (sluice-CORE-022S)

**Status: scoped baseline, NOT a universal performance claim.** Every number
below is workload × mode × parameter × machine. It exists so the future async
bench compares against an *engineered* blocking baseline, not a naive one, and so
the optimization pass (023S) has a before-tuning reference.

## Scope

- **Machine:** WSL2 (linux 6.18 x86_64), single dev environment. NOT
  representative of production hardware. Treat numbers as *shape* (which mode
  wins, where the knee is), not as absolute throughput.
- **Build:** xmake debug (`xmake f -m debug`). Debug builds are NOT timing-clean
  (no optimization); these are correctness-of-shape baselines. Re-run in release
  for any absolute claim.
- **Filesystem:** Linux tmpfs-backed `/tmp` (the benches write temp files under
  `temp_directory_path()`). tmpfs has no spinning-disk/fsync cost, so **W4
  fsync numbers here understate real-disk cost** — see W4 caveat below.
- **Repro:** `xmake build -g bench && xmake run w1_write_bench` (and w2/w3/w4).

## How to read these

Per `docs/sync-bench-methodology.md`:
- `blocking_sequential` is the WEAK baseline (one thread, serial). Reference only.
- `blocking_bounded_pool` is the ENGINEERED PORTABLE baseline (BlockingIoPool).
  **This is the primary row async compares against.**
- `blocking_thread_per_stream` is the STRONG-but-EXPENSIVE upper bound.

## Observed shape (qualitative — see CSV for raw numbers)

Running the four benches in this environment shows the expected qualitative shape.
**No universal throughput/latency number is claimed.** What is observable and
*robust* across reruns (i.e. not noise):

- **W1 (writes):** `blocking_bounded_pool` matches or beats `blocking_sequential`
  once `streams >= 2`; the win grows with streams. This is the concurrency the
  pool exists to provide (closes gap G5). `one_file_many_offsets` (positional
  pwrite, job 018S) is competitive with `many_files` at equal bytes — positional
  I/O does not regress writes (decision Q3, partial answer).
- **W2 (reads):** Same shape — pool wins with multiple streams; positional reads
  (`read_at_exact`) are competitive with per-file reads.
- **W3 (copy):** Copy is reader+writer per stream; the pool parallelizes cleanly.
  At `streams == hardware_concurrency` the pool and thread_per_stream converge
  (expected — both saturate cores).
- **W4 (durability):** On tmpfs, `sync_data`/`sync_all` add measurable but small
  cost over `none` (decision Q4). **On a real disk these costs dominate** — the
  tmpfs numbers are a lower bound, not a representative value. Re-run on the
  target disk before drawing any durability conclusion.

## Caveats / what these numbers do NOT show

- Debug build → absolute MB/s is pessimistic. Re-run release for tuning (023S).
- tmpfs → W4 fsync cost is understated. NOT a real-disk durability number.
- Single machine → no generalization across hardware.
- No p50/p95/p99 yet (those are the "add where feasible" columns in
  `sync-bench-methodology.md §4`; left blank in this baseline).

## Decision questions status (per sync-bench-matrix.md §5)

- **Q1 (pool saturation):** Shape visible but needs release + hardware-concurrency
  sweep to nail the knee. Deferred to 023S.
- **Q2 (useful worker count):** Same — deferred to 023S sweep.
- **Q3 (positional I/O helps?):** Partial — positional is *competitive* with
  many_files at equal bytes (no regression). Whether it *helps* is workload-
  dependent; recorded as "no regression" for now.
- **Q4 (fsync dominates?):** On tmpfs, no. On real disk, expected yes — needs a
  real-disk run. Recorded as "tmpfs-understates."
- **Q5 (vector I/O syscall pressure):** Not exercised by W1–W4 default cells;
  existing single-stream `copy_strategy_bench` / `wal_write_bench` cover it.
- **Q6 (thread_per_stream context switches):** Needs the
  `involuntary_ctx_switches` column (not yet wired). Deferred.
- **Q7 (which baseline for async comparison?):** **Answered:**
  `blocking_bounded_pool` is the primary comparison row;
  `blocking_thread_per_stream` is the upper-bound reference. This is locked in
  `docs/sync-bench-methodology.md §1`.

## Raw CSV

Regenerate with `xmake run w1_write_bench` (and w2/w3/w4). A sample snapshot is
NOT committed here because debug + tmpfs numbers are misleading out of context;
the methodology + bench targets ARE the reproducible artifact.
