# #643 Phase A validation matrix record

Date: 2026-11-21. The six configuration-matrix cells were re-executed at PR
#646 head `9d5c96236ad9de449ac3429522ef886cfbe05c80` (branch
`test/643-oracle-validation`, base `e16960ff`), replacing the AUD-001-head
count citations previously carried by the PR body: every count below is
measured at this head. The evidence commit carrying this directory adds only
these files; the tested code is the head named above.

## Environment (probed on site)

Recorded in `environment.txt`: Fedora 44, kernel `7.3.3-200.fc44.x86_64`,
20 logical cores; gcc 16.2.1 20261029; clang 22.1.8; xmake
`3.1.2+HEAD.5d21e9d`; system liburing 2.13.

## Per-cell procedure (canonical form)

Per cell: `xmake f -c -m <mode> --liburing=<y|n> --toolchain=<gcc|clang>
--ccache=n --builddir=<cell-dir> -y`, then `xmake build -y -g test`, then
`xmake test -y` — the form recorded by
`docs/evidence/v003-release-validation-20261023/validation-record.md`. Each
cell used a fresh `/tmp/sluice-643-matrix2-<cell>` build directory (removed
before the run, so no state carried between cells); the plain release profile
with the default builddir was restored afterwards (`RESTORE_EXIT=0`).

## Results: 6/6 PASS

| cell | configure | build | test | tests passed |
|---|---|---|---|---|
| release-y (gcc) | 0 | 0 | 0 | 84/84 |
| debug-y (gcc) | 0 | 0 | 0 | 84/84 |
| release-n (gcc) | 0 | 0 | 0 | 61/61 |
| debug-n (gcc) | 0 | 0 | 0 | 61/61 |
| asanubsan-y (clang) | 0 | 0 | 0 | 84/84 |
| tsan-y (gcc) | 0 | 0 | 0 | 84/84 |

Zero failed in every cell (`100% tests passed, 0 test(s) failed out of
84` / `out of 61`). `semantic_backend_conformance_test` and the uring-only
targets are not registered in the n cells (`xmake/tests.lua` gates them on
`has_config("liburing")`).

Count reconciliation against the AUD-001 record (83/61 at
`docs/evidence/643-aud001-canceled-count-20261121/REPORT.md:157-162`): the
+1 in the y cells is `uring_submit_poison_race_test`, added by #644
(`b25903ba`) after the AUD-001 recording head and before base `e16960ff`;
the n cells are unchanged at 61. This PR adds no test target — its diff
touches only `tests/` sources and `docs/evidence/`.

## Receipts

- `environment.txt` — head/branch/date/host/toolchain/liburing identity and
  the post-run profile restore log
- `summary.txt` — per-cell configure/build/test exit codes
- `cell-release-y.log`, `cell-debug-y.log`, `cell-release-n.log`,
  `cell-debug-n.log`, `cell-asanubsan-y.log`, `cell-tsan-y.log` — full
  configure/build/test stdout+stderr per cell, with stage exit codes and a
  build-directory landing check
