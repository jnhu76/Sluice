# #595 R3 continuation — E5 protection and resource measurement

| field | value |
|---|---|
| role | E5 protection and resource measurement |
| completion root | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z` |
| prereg | `evidence/PREREGISTERED_R3_COMPLETION.md`, sha256 `1a9c1e12c33a2f50e3bfac957c2ab25592d77e3ffae808dd7f92f08e16af3ab4` |
| context | `evidence/CONTEXT_COMPLETION.md`, sha256 `d6e3ec29f31c0c5c476a00bfaedcedd57c833bd552635030374dbb41c5f1fea2` |
| READY_EXPOSURE | `/home/jnhu/sluice-595-e5/r3/evidence/READY_EXPOSURE.md`, sha256 `0d0365c203ef1f6f28828ff636a03cb1a5c4b243fb9a16c13828fb6e0e2b3d10` |
| `semanticsReady` | **false** (frozen input) |
| mode | only R2 same-SHA and safe IMMEDIATE blocks were run; all new R3 B/A1 blocks are blocked |
| lock | `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`, exclusive `flock` for every build/timing/trace session |
| generated | 2026-11-05T11:35:18Z |

## Block verdicts

| phase | verdict | comparison |
|---|---|---|
| PROT-64K-D16 | BLOCKED_SEMANTICS_NOT_READY | B - A0 (NOT RUN: BLOCKED_SEMANTICS_NOT_READY) |
| PROT-PLACEMENT | NOT_ESTABLISHED | B - A0 under placement (NOT RUN: BLOCKED_SEMANTICS_NOT_READY) |
| PROT-SAT | NOT_ESTABLISHED | B_R2 - A_R2 / B - A0 (NOT RUN: NO_W02_R3_ARM; not in frozen enabledBlocks) |
| PROT-THREADPOOL | BLOCKED_SEMANTICS_NOT_READY | B - A0 (NOT RUN: BLOCKED_SEMANTICS_NOT_READY) |
| PROT-W02-D16 | PASS | B_R2 - A_R2 |
| PROT-W02-D3 | PASS | B_R2 - A_R2 |
| RESOURCE | NOT_ESTABLISHED | B - A0 resource gate (NOT RUN: BLOCKED_SEMANTICS_NOT_READY) |

The two PASS rows are **R2 same-SHA legacy reproductions**, not R3 B results.
No R3 B CPU/latency/resource claim is made. See `SUMMARY.json` for the
machine-readable record and each phase directory for per-block receipts.

## A/A calibration (prerequisite; executed in the prior calibration session)

| block | verdict | CPU hw | tput hw | p99 hw | source |
|---|---|---|---|---|---|
| AA-W03-D16 | PASS | 0.797 pp | 0.821 pp | 2.039 pp | `evidence/measurement-main/aa/aa-w03-d16/summary.json` |
| AA-W02-D2 | PASS | 0.862 pp | 0.853 pp | 1.420 pp | `evidence/measurement-legacy/aa-w02-d2/summary.json` |

This subagent verified the summaries but did not rerun the A/A blocks. No
measurement-condition repair was used (`repairUsed=false`).

## Provenance

- R3 worktree: HEAD `0a22a79195f1b913145fcd443496e7f70b08cdcf`, 15 dirty
  entries, `git diff HEAD` sha256
  `c1a31a30a9f02db99fac2ab2d0a8af028245a6fff0220314b8119c26e30d3003`.
- R2 tree: HEAD `271b81bfc9d64eecc0266fe7066b9040525dd710`, `git diff HEAD`
  sha256 `b006dbbf751f5fb2bbe8fb9edb091ae66e57a181b8cf2470d880c9589e1f4072`,
  reverse-check rc 0, patch sha256
  `7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0`.
- Binaries: R3 W03 `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1`;
  R3 W02 `4e719b1e4dc2f0575812f4310b54e96c443f8ce8d1c6dd39c81bf5ec91351497`;
  R2 W02 `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a`.
- Harness: `measure_pairs.py` `7ad405421046c9dbc63f5f46df7806a317eb9670827ca8420479ccbb32ca2dc1`;
  `analyze_pairs.py` `a91395d31e31cb4252f95d7725f54a06e11e5389cc00411f0b678fde239be7a3`;
  analysis self-test `ANALYSIS-SELFTEST/selftest.log` -> 8/8 PASS.
- Lock sessions: `raw/protection/lock-logs/` and the two saturation `lock.txt`
  records; the three main sessions' second open description `flock -n` failed
  (rc=1), confirming exclusivity.

## Blocked (frozen `blockedBlocks`)

All new R3 B/A1 blocks remain `BLOCKED_SEMANTICS_NOT_READY`/`NO_W02_R3_ARM`:
`MAIN-W03-D16-A1/B`, `CROSS-W03-D4/D64-A1-B`, `MECH-POS-D2POS`,
`MECH-NEG-D2SERIAL`, `MECH-ENTER-CQE`, `PROT-W03-D4/D16/D64-B`,
`PROT-64K-D16-B`, `PROT-THREADPOOL-B`, `PROT-SMT-2-12-B`,
`PROT-XPHYS-2-8-B`, `PROT-W02-D16-B (R3)`, `PROT-SATURATION-B`,
`RESOURCE-B`, `PRODUCT-604` (HOLD, no #604 handoff).

## Limitations

- semanticsReady=false: all new R3 B/A1 blocks remain blocked; no R3 B CPU/latency/resource claim is made.
- PROT-W02-D3/D16 are R2 same-SHA legacy reproductions, not R3 B results; cross-SHA comparisons are prohibited.
- The prereg case id W03-uring-64k-d16 is not present in the frozen binary; the actual research id is W03-uring-64k-d16-r3d16-64k.
- The Python posix_spawn wait4 RSS cross-check is invalid (CLONE_VM|CLONE_VFORK); /usr/bin/time -v values are authoritative.
- The allocation-slope sub-gate (valgrind 512/1024) was not run: the frozen bench has no request-count knob.
- Threadpool process total is 1 io_uring_enter per invocation (statx availability probe); workload-phase enters are 0.
- Placement is A0-only; {2,12} and {2,8} are much slower than {2} for A0, so no placement inference about R3 B is possible.
- Frequency/governor drift (schedutil 1.2-3.5 GHz) is recorded but not controlled; cpu12 background was <=1.1% in the sampled before-windows.

## Raw data

All raw invocations, logs, JSONL, strace files and the lock logs are under
`/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/` and are mirrored under
`/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/measurement-negative/protection/raw/`. The result paths in the E5 `MeasurementResult` point at
`/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection`.
