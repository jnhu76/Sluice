# #595 Flow Audit — BASELINE (objects, binaries, datasets)

Round: `flow-audit` (this round), 2026-11-07, native E5 host. Review anchor
`9d1bf648`; this revision fixes review findings R1–R6 (see CORRECTIONS.md).
The audit phase itself ran offline; the archival PR and this repair were
each separately authorized by the round owner.

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

## CPU/metric caliber (verified this round; rewritten per CORRECTIONS R4)

Three windows, never interconverted (scripts/cpu_scope_recalc.py v2):

- **Primary = measured phase, process scope**: per invocation
  `cpu_ns_per_op = 1e9*sum(cpu_s)/sum(ops)` over the 5 case-matched
  measured reps (each rep's `cpu_s` is the bench's
  `CLOCK_PROCESS_CPUTIME_ID` delta around that measured phase); arm value =
  mean over 24 invocations (frozen harness/analyze_round definition).
  Recomputed A0 3208.6 / R 2519.1 / C 2499.4 ns/op (MAIN) reproduce
  analysis/round-analysis.json at raw float precision (max rel diff 0.0,
  scripts/verify_inherited_evidence.py). Scope = the bench child's threads
  (submitter+driver); NOT a whole-system number.
- **Invocation window**: `ru_utime/ru_stime` are the parent's **wait4**
  rusage of the whole child (harness/measure3.py:144,161) — covers warmup,
  oracle self-test, measured, instrumented phases and setup/teardown.
  Reported in seconds only; the v1 claim that dividing them by rep-op
  counts yields ns/op is withdrawn.
- **Tick window (invocation, 10 ms, first-to-last per task)**: app/driver/
  io-wq buckets from the frozen R4 sampler (measure_pairs.py). io-wq
  workers are scanned every 0.5 s → ticks_lower_bound; shares are
  **sampled-and-classified only** (MAIN: io-wq ≈40% of such ticks A0 vs
  ≈48% R/C; io-wq lower bound R/C ≤ A0). Whether io-wq worker CPU is
  inside the child's rusage/PROCESS_CPUTIME is **UNRESOLVED**: workers are
  observed as tasks under the child's `/proc/<tgid>/task` (1 per
  invocation, structural procfs evidence, not comm-only), while the
  sampler's own residual estimate exceeds its bound in 72/72 invocations
  (mean ≈26 vs ≈11 ticks), dominated by window mismatch — the records
  cannot adjudicate inclusion in either direction. The r5-repair round's
  "io-wq outside rusage" caveat is therefore treated as unverified on this
  host, not repeated as fact.
- p99 calibers are distinct and not interchangeable (machine-generated in
  verify output): MAIN run-level mean diff C−A0 **9.52 µs** / R−A0
  **10.71 µs**; per-block paired relative-effect means +17.97%/+20.14%
  (= frozen contrasts); pooled per-request p99 UNAVAILABLE (main runs
  store per-rep percentiles only).

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
