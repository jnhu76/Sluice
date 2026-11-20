# 643 AUD-002 — admission-health probe synchronization

Deterministic remediation record for audit finding AUD-002 (#643) at base
`9c630d1f5ff14da01a09d5c9cada8a2aa5ca2279`.

- `REPORT.md` — full round report: revalidated protocol, failure
  interleaving, root cause, minimal fix and rejected alternatives,
  RED/GREEN evidence, matrix, compatibility, limitations.
- `production.diff` — the reviewed src diff (the test file and xmake
  registration are in the branch, not repeated here).
- `tsan-red-prefix.log` — symbolized TSan report on the base SHA with the
  reproducer only: `uring_backend.cpp:436` unlocked probe read vs
  `uring_transport.cpp:340` poison write, plus the adjacent
  `uring_backend.cpp:401` retirement write.
- `tsan-green-postfix.log` — the fixed-tree TSan run (0 race reports).
- `package-verify-clean.log` — `verify_v2_package.py --mode release
  --liburing` in a clean worktree of the PR head: all gates PASS including
  `V2_B_PROVENANCE_RECORDED`.

Reproducer: `tests/uring_submit_poison_race_test.cpp` (xmake target
`uring_submit_poison_race_test`, liburing profiles only).
