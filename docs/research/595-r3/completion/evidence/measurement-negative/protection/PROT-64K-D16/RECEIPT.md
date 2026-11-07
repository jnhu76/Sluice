# PROT-64K-D16 — blocked B; A0-only baseline and eligibility check

| field | value |
|---|---|
| phase status | **BLOCKED_SEMANTICS_NOT_READY** |
| prereg case | `W03-uring-64k-d16` |
| actual case id | `W03-uring-64k-d16-r3d16-64k` (the frozen R3 binary only has the research 64 KiB case; label_suffix '-r3d16-64k') |
| comparison | `B - A0 (NOT RUN: BLOCKED_SEMANTICS_NOT_READY)` |
| A0 binary | R3 `bench_w03_external_loop` `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1` |
| A0 runs | 3 x `/usr/bin/time -v` (WARMUP=1 MEASURED=5, 2,000 reads/rep) + 2 plain sampler runs |
| raw | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/SAFE-A0-W03-64K-D16/`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/SAFE-A0-W03-64K-D16-threads/`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/DIAG-64K-A0/` |

## A0 descriptive baseline (2,000 reads/rep; values are per-invocation aggregates)

| metric | mean |
|---|---|
| CPU/op | 12351.555 ns |
| throughput | 81802.827 ops/s |
| p50 | 184793.5 ns |
| p95 | 228429.5 ns |
| p99 | 265278.9 ns |
| max user threads | 3 |
| `/usr/bin/time -v` max RSS | [5948, 5840, 5592] KiB |
| io_uring_enter | total=6004, pure_submit=6000, to_submit_hist={'1': 6004} (3 passes x 2,000; 4 setup probes) |

## Why B cannot batch 64 KiB

`src/async/uring_backend.cpp:437-439`:
`batch_eligible_read(op) = (op.len == 4096 && op.dst != nullptr)`.
For a 64 KiB front member the batch path admits one member, sets
`stop=ineligible`, leaves the suffix untouched, and the adapter submits the
suffix as span-1 calls. This is a source-shape statement, not a measured B run.

## Boundary

- B is blocked by `semanticsReady=false`; no 24-pair A0-vs-B comparison exists.
- The A0 enter count is the expected value if B falls back immediate; it is not
  evidence that B did so.
