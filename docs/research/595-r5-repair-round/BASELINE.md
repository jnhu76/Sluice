# #595 R5-repair — BASELINE (environment and frozen objects)

## Host (verified 2026-11-06T18:2xZ, fresh boot)

- Intel Xeon E5-2666 v3, 20 CPUs (10 cores × SMT2), single NUMA node,
  `systemd-detect-virt` = none (bare metal), kernel 7.2.11-200.fc44.x86_64,
  governor schedutil on the bench/monitor CPUs (2/12), 62 GiB RAM, /tmp
  tmpfs 32G, /home on /dev/sda3. Matches the R0-R5 frozen environment
  records; no WSL2 anywhere in the chain.
- gcc 16.2.1, xmake v3.1.2+HEAD.5d21e9d, liburing dev present (builds used
  `--liburing=y`).
- Boot context: the host was shut down after R5 Phase A (previous round's
  final instruction) and powered back on for this round; uptime at
  verification 9 min; both MEASUREMENT.lock files existed and were free
  (fuser rc=1); no competing bench/xmake/perf processes.

## Frozen objects (all verified by `git rev-parse` before any work)

| Object | Value |
|---|---|
| R_original | `f3b25329bc8a64a201961c426a58adacfb8fdf08` |
| C1 | `c55165a219a67058c97941c60ae520d1b1b45996` |
| C2 = R_cleanup | `b191bb893432c4df0551d24c85843d8513ed5a55` |
| Diagnostic original | `21374a4066713bf2b2c1bb19f9744818c02ab2a7` (full SHA resolved) |
| Diagnostic fixed | `research-595-r5rm-diag` @ `c0cdc3ea` + fast-path `61bf73d6` |
| Diag-cleanup | `research-595-r5rm-diag-cleanup` @ `01d3a252`+`1d25ad87`+`fdf5092a` |
| PR #630 reviewed head | `828b10b441ebc01406d8186d160054b0e0951f98` (unchanged, OPEN) |

No master-history mixing: all round work lived in
`/home/jnhu/sluice-595-e5/r5-repair-measure/` worktrees on research
branches; `/home/jnhu/Source/Sluice` master stayed clean; `r4/` and `r5/`
were read-only.

## Binaries (clean release builds, recorded in logs/binary-sha256.txt)

- original bench_w03 `7bf83eb9…`, bench_w02 `b07fd060…`
- cleanup bench_w03 `9442f02b…`, bench_w02 `971a28da…`
- diag-original (v2+fast-path) `bd7f46ab…`; diag-cleanup `027feb36…`
  (diagnostic binaries never used in clean timing segments; verified by
  the round checker)

## Lock discipline

All timing under flock of `/home/jnhu/sluice-595-e5/MEASUREMENT.lock`
(fd acquired first) then `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`
(fixed order, both held for the whole cell/diagnostic session);
receipts with UTC acquire/release, wait seconds, fuser -v on both locks,
second-fd exclusivity probes (rc=1 = blocked as expected), pre/post
load/cpu-busy/governor/tmp state — `logs/LOCK_RECEIPT.md`
(5 clean-cell sessions + 1 diagnostic + 1 RSS supplement; one aborted
diagnostic attempt recorded with its env-gate reason).

## Fix/equivalence evidence locations

- v1 race audit + v2 fix: `CORRECTIONS.md` §F1; commits on the diag
  branches; TSan harness `harness/tail_trace_tsan_harness.cpp`;
  negative-control run (v1 under the same harness reports races).
- Equivalence battery: `scripts/equivalence_battery.sh` outputs under
  `raw/equiv/{original,cleanup}/`; bounded-scan check
  `harness/bounded_scan_check.cpp` (5,720 spans PASS).
- Measurement: `raw/<cell>/{aa,main,calib,rssupplement}/results.jsonl`
  (every attempt retained), `raw/PROGRESS.json` (schedule sha
  `264991409e540ddbea38b3660e10b25bb2af9bfa49809a5da14463ad4d8fc1c6`),
  driver `scripts/run_round.py` + `harness/measure3.py`.
- Diagnostic: `raw/diag-W03-uring-4k-{d16,d4}/`, v2 dumps under
  `raw/diag-dumps/` (E5-local, large), aggregation
  `analysis/diag-perturbation.json`, joined ops `raw/diag-ops.csv`.
