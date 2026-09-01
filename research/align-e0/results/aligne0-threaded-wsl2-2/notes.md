# aligne0-threaded-wsl2-2 — SECONDARY TOPOLOGY DIAGNOSTIC (QUALIFIED_BUT_VIRTUALIZED)

Purpose: does the READ per-op copy benefit survive REAL overlap? workers =
in-flight depth {2,4,8,16,32}, arms a0/a7, sizes {64K,1M}, dirs both.
Rep wall = max thread span; per-op latency sampled per thread.

- 80 runs, 0 gate errors (after the thread_main fix; see threaded-wsl2-1).
- READ true per-op latency: a0 4x-6.5x slower than a7 at every worker count
  (64K: 22-28 us vs 5.4-8.7 us; 1M: 340-566 us vs 113-306 us) -> the uaccess
  cost does NOT disappear under concurrency; it persists per op.
- READ amortized wall/op: both improve with workers; a7 improves more (64K
  w32: a0 4.0 us vs a7 0.55 us). The benefit is NOT hidden by overlap in a
  pure-I/O harness.
- WRITE: no material a0 vs a7 difference at any worker count (both amortized
  and true per-op) -> confirms no WRITE alignment effect under concurrency.
- Interpretation for d1/d8: the BUF-E0 d8-null at APPLICATION level is not
  "uaccess cost disappears"; the per-op cost persists. At d8 the production
  engine's other factors (writeback/page-cache ceiling, control plane,
  scheduler) dominate the pipeline; see the amp session.
