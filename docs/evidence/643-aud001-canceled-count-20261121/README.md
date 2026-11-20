# 643 AUD-001 — canceled_ops lock-domain synchronization

Deterministic remediation record for audit finding AUD-001 (#643) at base
`9c630d1f5ff14da01a09d5c9cada8a2aa5ca2279`.

- `REPORT.md` — full round report: revalidated counter ownership, failure
  interleaving, root cause, minimal fix and rejected alternatives
  (including the relaxed-atomic assessment and the audit v1.1
  bench-reader correction), RED/GREEN evidence, matrix, compatibility,
  focused A/B, limitations.
- `production.diff` — the reviewed src diff (the test file and xmake
  registration are in the branch, not repeated here).
- `tsan-red-prefix.log` — symbolized TSan report on the base SHA with the
  reproducer only: the ThreadPool write-write pair
  (`threadpool_backend.hpp:234` via `cancel_key` outside `work_mtx_`)
  and the io_uring write-write pair (`uring_internal.hpp:62` via
  `cancel_key` outside `dispatch_mtx_`).
- `tsan-green-postfix.log` — the fixed-tree TSan run (0 race reports).
- `cancel-path-ab.tsv`, `cancel-path-ab.txt` — focused cancel-path A/B
  raw pairs and analysis (25 interleaved pairs, 200k iterations each).
- `bench-w02-stats.jsonl`, `bench-w03-stats.jsonl` — fixed-tree W-02
  (17 rows) and W-03 (7 rows) benchmark stats output; `stats_canceled_ops`
  present in every row, value 0, JSON parses.
- `package-verify-clean.log` — `verify_v2_package.py --mode release
  --liburing` in a clean worktree of the PR head: all gates PASS including
  `V2_B_PROVENANCE_RECORDED`.

Reproducer: `tests/cancel_stats_race_test.cpp` (xmake targets
`cancel_stats_race_test` on all profiles and `cancel_stats_race_uring_test`
on liburing profiles).
