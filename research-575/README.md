# research-575 — Atomic RMW Counters on Production I/O Paths

Issue: jnhu76/Sluice#575 (parent #563). Status: CLOSED, `NO_CHANGE_JUSTIFIED`.

## Research Question

Two test-only atomic RMW counters — `ThreadPoolBackend::syscall_count_` and
`UringAsyncBackend::submit_flushes_` — sit on production I/O hot paths. Three
questions: (1) does their `lock addq` read-modify-write have a measurable cost
in Release builds; (2) can they be removed from Release (via the existing
`SLUICE_ASYNC_INTERNAL_TESTING` guard) without losing semantics; (3) does
removal produce a measurable end-to-end benefit?

## Structure

R0–R9 native-E5 campaign, all evidence produced in five isolated worktrees on
base `3df10dc4`:

- **R2 codegen proof**: the `lock addq` sites were proven to survive GCC
  16.2.1 `-O3` through GIMPLE dumps, object-file disassembly, and a
  linked-binary lock-add site census (archived under `assembly/base/`).
- **R3 arm design**: per backend (ThreadPool / io_uring), three arms —
  BASE / OP-ONLY (guard the increment only) / FULL (guard increment + field).
  No TP×uring combination arms. Workloads: TP and UR 4k d1/d4/d16, 64k, W03,
  plus UR d8 cap8/cap1024 and a W01 read_at shape.
- **Paired A/B protocol, evolved v1→v3**: the decisive methodological finding
  was that *tmpfs fixture path-name differences shift the same binary's wall
  time by ~1%* (isolated in cal4). v3 therefore shares one fixture path across
  arms, adds two warmup rounds per invocation (frequency-state carry-over was
  observed in A/A), pins cores with `taskset`, and takes 15 interleaved pairs
  per headline config with perf stat + `/usr/bin/time -v` and a fail-closed
  byte oracle. A/A noise floors reached: wall ~2%, instructions ~0.05%
  (single-worker) / ~1% (multi-worker), cycles 0.5–0.8%.
- **Correctness**: named mutants TP-M1 and UR-M1. TP-M1 was killed by the
  existing integration test; UR-M1 exposed a real oracle gap (the existing
  uring suite only asserted zero values, 21/21 green under the mutant) and a
  new flush-count oracle test was written to kill it.
- **Reduction**: paired-statistics reducer with a self-test
  (`code/reduce_575.py --selftest`).

## Code References

- Research branch `research/575-native-e5-atomic-rmw` @
  `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` — **E5-local only, never pushed;
  PR #584 CLOSED unmerged.** Base `3df10dc4` (v0.0.3 master). Recoverable on
  E5 from `~/sluice-593-backup/bundles/research-575-native-e5-atomic-rmw-e9baafb9-*.bundle`.
- Authoritative archive (reports R0–R9, verdict, 9 patches, 81 raw-sample
  files, codegen receipts, per-file sha256 manifest): **GitHub master**
  `docs/research/575-native-e5-atomic-rmw-archive/`, archived in commit
  `666942d2` (PR #615).
- The flush-count oracle was productized as test-only PR #585 (squash
  `1df8b4b8…`, registered in `xmake/tests.lua`).
- `code/` here holds the only research code that exists in no git history:
  the frozen A/B runner, its d8 supplement, and the self-tested reducer.
  (`layout_probe.cpp`, a one-off sizeof probe, was deliberately not kept.)

## Conclusions

- `THREADPOOL_VERDICT = NO_CHANGE_JUSTIFIED`
- `URING_VERDICT = NO_CHANGE_JUSTIFIED`
- `ISSUE_575_VERDICT = NO_CHANGE_JUSTIFIED`, `PRODUCT_CHANGE_AUTHORIZED = NO`
- Independent reviews: Review A (semantics/concurrency) **APPROVE** (mutation
  kills independently re-executed); Review B (measurement method) **REVISE**
  with three P1 findings — Monte-Carlo CI at n=15 systematically over-tight by
  one order statistic; a UR d1 control "−1.3%" that did not separate; one
  citation to nonexistent stats-on W-03 evidence. All resolved; the
  product-level verdict survived review unchanged.
- Net: neither counter demonstrated material Release cost; removal was not
  justified. The durable output is the A/B protocol discipline and the
  productized oracle test, not a product change.

## Next Steps

If counter overhead is ever re-examined: reuse the v3 paired protocol (shared
fixture path, WARMUP=2, ≥15 interleaved pairs, order-statistic-correct CI
rather than n=15 Monte-Carlo), confirm the counters still sit on a measured
hot path first, and keep the flush-count oracle as the semantic guard for any
counter refactor.
