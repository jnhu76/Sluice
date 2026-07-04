# W1–W4 blocking benchmark interpretation notes

**Status:** Reviewer-facing (sluice-CORE-024S §5). Explains what the W1–W4
benches measure and how to read them, WITHOUT making broad performance claims.
Companion to `docs/results/sync-w1-w4-baseline.md` (scoped baseline) and
`docs/sync-bench-methodology.md` (methodology).

## What each workload measures

| Workload | Bench target | Purpose |
|----------|--------------|---------|
| **W1** | `w1_write_bench` | Direct blocking **write** baseline. Streams of writers × blocks × block_size; both `many_files` and `one_file_many_offsets` (positional `pwrite`) layouts. |
| **W2** | `w2_read_bench` | Direct blocking **read** baseline + **positional I/O** behavior/overhead. Streams of readers on pre-written files; both layouts (positional via `read_at_exact`). |
| **W3** | `w3_copy_bench` | Reader→writer **copy** throughput; `buffer_size` sweep. Parallel copy streams via the pool. |
| **W4** | `w4_durability_bench` | **Durability operation cost** (`none` vs `sync_data` vs `sync_all`) + foreground/background overlap. The sync-policy axis is the variable of interest. |

Each bench emits CSV to stdout via `matrix::print_row` (columns defined in
`docs/sync-bench-methodology.md §4`).

## How to run

```bash
xmake f -m debug        # or -m release for tuning-grade numbers
xmake build -g bench    # builds all bench targets
xmake run w1_write_bench   # (and w2_read_bench / w3_copy_bench / w4_durability_bench)
```

## Three execution modes (for W1–W3; W4 sync policies run on bounded_pool)

- `blocking_sequential` — the **weak** baseline (one thread, serial). Reference only.
- `blocking_bounded_pool` — the **engineered portable** baseline (`BlockingIoPool`). **This is the primary row any future async comparison measures against.**
- `blocking_thread_per_stream` — the **strong-but-expensive** upper bound (one `std::thread` per stream).

## Key architectural conclusion (conservative)

> `BlockingIoPool` is useful for **isolating blocking I/O from caller threads**
> and for **overlap under mixed workloads**, but it is **not a universal
> performance win**. Direct blocking remains the simplest and often fastest path
> for single-stream synchronous I/O.

## What the benches do NOT prove

- **No universal throughput/latency claim.** Every number is workload × mode ×
  parameter × machine × build × filesystem. See the scoped-baseline caveats in
  `docs/results/sync-w1-w4-baseline.md`.
- **tmpfs understates W4.** On tmpfs `/tmp`, `fsync`/`fdatasync` return near-
  instantly; on a real disk they dominate. Re-run W4 on the target disk before
  any durability conclusion.
- **Debug builds cap absolute MB/s.** The shape (which mode wins) is robust; the
  magnitudes are not tuning-grade. Use `-m release` for optimization work.
- **No p50/p95/p99 yet.** The matrix CSV has the columns but they are left blank
  in this baseline (methodology §4 "add where feasible"). Deferred.

## Cross-links

- Scoped baseline + qualitative shape: `docs/results/sync-w1-w4-baseline.md`
- Durability baseline row: `docs/results/sync-durability-baseline.md`
- Methodology: `docs/sync-bench-methodology.md`
- Matrix spec: `docs/sync-bench-matrix.md`
- Optimization notes (evidence pass): `docs/sync-optimization-notes.md`
