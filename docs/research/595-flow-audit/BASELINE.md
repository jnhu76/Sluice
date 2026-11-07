# #595 Flow Audit — BASELINE (objects, binaries, datasets)

Round: `flow-audit` (this round), 2026-11-07, native E5 host, read-only on
GitHub (PR #630 inspected via `gh pr view` only; no commit/push/comment).

## Host (verified this session; deltas vs r5-repair BASELINE only)

- Same bare-metal host `E5`: Xeon E5-2666 v3 (10C/20T, 1 NUMA node),
  kernel 7.2.11-200.fc44.x86_64, Fedora 44, governor `schedutil`, 20 CPUs
  visible, affinity 0-19. gcc 16.2.1, xmake v3.1.2+HEAD.5d21e9d.
- Both measurement locks free at session start (`fuser` rc=1 on
  `/home/jnhu/sluice-595-e5/MEASUREMENT.lock`); no bench/xmake/perf processes.
- This round ran NO timing segments; every number cited is either read from
  archived raw or recomputed offline (see scripts/). The dual-lock protocol
  was therefore not exercised and no LOCK_RECEIPT is produced.

## Object table (object → SHA/patch → binary → dataset → usable for)

| Object | Source identity | Binary | Dataset | Usable conclusions |
|---|---|---|---|---|
| **PRODUCT** (this round's master) | `271b81bfc9d64eecc0266fe7066b9040525dd710` (`/home/jnhu/Source/Sluice`, clean). **Structural fact: master is an ancestor of every research branch below — the batch entry `submit_ready_reads`, `bench/r3_ready_batch.hpp`, `include/sluice/async/detail/test_access.hpp`, `initiation_precheck.hpp` and the ready-batch tests DO NOT EXIST in PRODUCT.** Shared product files are byte-identical to R_original except `uring_backend.cpp/hpp` (research adds the batch entry), `async_io_context.cpp` (research extracts `initiation_rejection` into a shared header, behavior identical) and test-only hooks. | none built this round | none new | Source-level statements about the product's only submission path (`ctx.submit_read` per op → 1 `io_uring_submit` per op), lifecycle, locks, publication, bench callers. |
| **RESEARCH R_original** | `f3b25329bc8a64a201961c426a58adacfb8fdf08` = master + 5 commits (0a22a791, d9d140ee, 6a13d2d5, f3b25329 adds r3 arms + batch entry) | `bench_w03 7bf83eb9…`, `bench_w02 b07fd060…` (logs/binary-sha256.txt) | R4 matrix + r5-repair raw | What the R arm measured: research adapter path. |
| **RESEARCH R_cleanup (C)** | `b191bb893432c4df0551d24c85843d8513ed5a55` = R_original + c55165a2 + b191bb89 (equivalence-proven cleanups) | `bench_w03 9442f02b…`, `bench_w02 971a28da…` | r5-repair three-arm raw (712/712) | All R5-repair C−A0 / C−R numbers. |
| **Diagnostic v2** | `research-595-r5rm-diag` @ `61bf73d6` + quiesce-safety `492086ac`; cleanup mirror `3fd6476a`. **These post-round fixes did NOT change the timed binaries.** | diag binaries `bd7f46ab…`/`027feb36…` (never in clean timing) | raw/diag-* | Lead-level phase decomposition only (PERTURBED, ±3pp window failed). |
| **Archive head (PR #630)** | OPEN, head `e265aa7c` on `research/595-r5-phase-a`; local mirror `/home/jnhu/sluice-595-e5/r5/archive-wt`. Master has not moved since (`271b81bf` is merge-base) → **no divergence to list this round.** | — | `docs/research/595-r5-phase-a/`, `docs/research/595-r5-repair-round/` | Frozen narrative of G1-G5, C1-C7, R5-repair corrections. |
| **BENCH W02/W03** | PRODUCT variants at `271b81bf`: `bench/bench_w02_pipeline.cpp` (751 L), `bench/bench_w03_external_loop.cpp` (671 L). RESEARCH variants = master + r3 arm integration (+243/+277 L). **T0 semantics differ: PRODUCT W03 stamps `submitted_ns` AFTER each op's `submit_read` returns (own submit excluded); RESEARCH stamps before the op's own submit (A0) or at whole-group build (R/S/C arms). Research A0-vs-R is same-origin; research-A0-vs-product-A0 latency endpoints are NOT directly comparable (± own-submit cost per op).** | per-arm binaries above | 64 MiB tmpfs fixture, 4 KiB reads, warm page cache, request_count=20000/rep, 3 phases per rep (measured/oracle/instrumented) | Feedback-loop structure, driver/wait mechanics, lock topology; quantitative latency only within the research pair. |

## Environment inheritance

R0-R5 frozen environment records apply (host, kernel, governor, tmpfs). This
round verified only: CPU model/topology, kernel release, governor value,
toolchain versions, lock freedom, process table. No drift found.

## CPU/metric caliber (verified this round)

- Primary CPU/op = `CLOCK_PROCESS_CPUTIME_ID` delta around the measured
  phase (`bench/bench_common.hpp:45-47`, W03 `bench_w03:519-522`),
  normalized per op. Recomputed from raw per-rep records
  (scripts/cpu_scope_recalc.py + inline check): A0 3208.6 / R 2519.1 /
  C 2499.4 ns/op and p99 53.92/64.63/63.44 µs (MAIN) — **bit-identical to
  RESULTS.md**.
- Process-CPU covers submitter+driver threads; it does NOT cover io-wq
  kernel workers (`iou-wrk-<pid>` kthreads). Whole-invocation /proc tick
  buckets (10 ms granularity, invocation-scoped — indicative only,
  RECALCULATED): io-wq share of (app+io-wq) ticks ≈ 40% (A0) vs ≈ 48%
  (R/C) on MAIN; io-wq ticks R/C ≤ A0 in every cell. "CPU −22%" is
  process-rusage scope, never whole-system.
- `ru_utime/ru_stime` in round summaries are invocation-cumulative
  (getrusage at end, harness/measure3.py:161) — not the primary metric.

## Where this round's outputs live

`/home/jnhu/sluice-595-e5/flow-audit/worktree` = isolated worktree, branch
`research-595-flow-audit` at `271b81bf`, all files under
`docs/research/595-flow-audit/`. The audit itself was executed fully
offline; the archival commit/PR on this branch is a follow-up submission
explicitly authorized by the round owner after the audit closed. No probes
required server programs; the only executed code is offline Python over
archived raw (`scripts/cpu_scope_recalc.py`,
`scripts/verify_inherited_evidence.py`). Historical trees
(`/home/jnhu/sluice-595-e5/r5*`, `r4/`, master) untouched.
