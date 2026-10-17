# v2 release acceptance record (RG1–RG7 convergence)

Status: VERIFIED_AT_PR_HEAD / PENDING_MERGE — every claim below was
executed at this branch's head; the gates close when the PR merges.
Baseline for the campaign: `c1facc52` (master at branch-out). Environment:
`environment.txt` (Fedora 44, kernel 7.2.9-201.fc44.x86_64, x86_64, 20
CPUs, GCC 16.2.1, clang 22.1.8, xmake 3.0.9, liburing 2.13, btrfs root,
tmpfs /tmp, Python 3.14.8, OpenJDK 25.0.4).

Commits of this campaign, in order:

1. `ff7d6dde` — RG1: fork-isolated unresolved-borrow lifetime witness.
2. `a0874ae2` — RG4: identity-domain boundary evidence on both backends.
3. `6411cff3` — RG5: cancel-race oracle + registered final-probe mutant.
4. `5fd8e109` — RG5 fix: wake-delivery discrimination replacing the
   transient in-flight observation (defect caught by the matrix round 0;
   `matrix-round0-cancel-race-defect/`).
5. `7efd86ec` — RG7: CHANGELOG breaking-change disclosure (D3).
6. `72acc209` — review fixes: witness exit-code channels, heap-core
   address-reuse observation, uring retired-slot assertion parity.
7. Evidence, ledger and record commits (this directory).
8. Post-review revision: the failure-path release in the uring race
   oracles (`9b7bfa63`) and the evidence-accuracy/record update (this
   commit), recorded in `post-review-revision/`.

## 7. Independent review record

Four independent reviews were run against the exact commits; all
P0/P1 findings were fixed directly and re-verified:

- **Lifetime & Request Identity** (RG1 `ff7d6dde`, RG4 `a0874ae2`):
  APPROVE-WITH-FIXES, no P0. Fixed: witness exit-code conflation (P1,
  `72acc209`), vacuous stack-address instrumentation (P2, `72acc209`),
  uring retired-slot assertion asymmetry (P2, `72acc209`), stale
  comment + narration comments (nits, `72acc209`), evidence-doc wording
  (this directory's `req01-identity-domain-evidence.md` and
  `lsan-poisoned-shutdown-disposition.md`).
- **io_uring Progress & Protocol** (RG5 `6411cff3`+`5fd8e109`):
  APPROVE-WITH-FIXES, no P0; the oracle, bound, staging and mutant were
  independently re-verified (the reviewer reproduced 23/23 oracle passes
  in release and re-killed the mutant; the artifact-backed stress figures
  are the defect-note's release 10/10, debug 10/10, ASan+UBSan 5/5 under
  concurrent model-checker load, plus the matrix cells). Fixed: coverage-table discipline column relabeled to the
  real ring/staging mechanisms (P1, doc), oracle count 24→23 and kill-log
  path (P2, doc + ledger G6 row), defect-note mislabel (P2, doc).
- **Release Evidence & Compatibility** (RG6/RG7, full PR):
  APPROVE-WITH-FIXES, no P0; independently re-verified all matrix
  digests/exit codes/test counts, the 24/24 package manifests, the TLA
  verdicts, the 36/36 consumer extraction, and the D3 disclosure against
  the tree. Fixed: three ledger rows still citing the G10 matrix as
  outstanding, the stale header overall-status, the RG1 cell attribution
  (liburing=y only), the VERIFIED_AT_PR_HEAD vocabulary definition, the
  TLA detail attribution, the W-02/W-03 label, the reviewer-2 figure
  attribution, and CHANGELOG/doc nits — all in the final review-fix
  commit.
- **Pre-merge re-review** (whole PR, post-publication): REVISE_BEFORE_MERGE,
  P0 = 0; two P1 evidence-accuracy findings and one P2 test failure-path
  finding. Fixed in the revision this record's §8 covers: the SEM-06
  ledger row's discharged-but-rewritten G10 gap, the identity evidence
  doc's non-existent "live context registry" claim (replaced with the real
  global-cursor mechanism), and the cancel-race oracle's unreleased frozen
  owner on an unexpected disposition (generalized to every live-owner
  failure exit in the file). The reviewer's commit count (9 on the branch)
  is the correct one.

Per-gate records in this directory:

- RG1: `lsan-poisoned-shutdown-disposition.md`
- RG4: `req01-identity-domain-evidence.md`
- RG5: `prog02-settlement-notification-oracle.md`
- RG6: this record + `matrix/` + `w01-w02-w03-consumers.txt`
- RG7: CHANGELOG.md `[Unreleased]`

## 1. Release acceptance matrix (PROD-02, gate RG6)

Matrix head: the content head `72acc209` recorded in `environment.txt`
(the evidence commits themselves do not change code).

Eight cells, recorded per cell with configure/build/test argv, exit codes,
log digests and `xmake.conf` in `matrix/matrix.json`; the driver commands
are in each `*.argv` file (the one-shot driver script itself is not part of
the repository, matching the PR B matrix precedent):

| Cell | Toolchain | Mode | liburing | Tests | Failed |
|---|---|---|---|---|---|
| debug-n | gcc | debug | n | 56 | 0 |
| debug-y | gcc | debug | y | 74 | 0 |
| release-n | gcc | release | n | 56 | 0 |
| release-y | gcc | release | y | 74 | 0 |
| asanubsan-n | clang | asanubsan | n | 56 | 0 |
| asanubsan-y | clang | asanubsan | y | 74 | 0 |
| tsan-n | gcc | tsan | n | 56 | 0 |
| tsan-y | gcc | tsan | y | 74 | 0 |

Toolchain notes (honest configuration record):

- The sanitizer cells pin the clang toolchain for ASan+UBSan because GCC's
  shared `libasan.so` hard-exits the tail CLI oracle's LD_PRELOAD
  interposer child ("ASan runtime does not come first in initial library
  list"); full capture and disposition in
  `gcc-libasan-preload-collision/`. This is a test-harness/sanitizer
  runtime incompatibility, not a product defect; GCC's shared TSan runtime
  performs no such check, so the TSan cells run on the default GCC
  toolchain and pass.
- RG1's LSan scenario (`shutdown_lifecycle_uring_test`,
  `poison_with_in_flight_cqe`) passes leak-free in `asanubsan-y`. The
  scenario's test target is compiled only under `SLUICE_SHUTDOWN_URING`,
  so it exists only in the liburing=y cells; the liburing=n sanitizer
  cells cover the ThreadPool shutdown suite instead.

Matrix round 0 (at `6411cff3`, before `5fd8e109`) failed exactly one
oracle — the RG5 cancel-race test's transient observation — in debug-y and
release-y. The defect, its root cause and the fix are recorded in
`matrix-round0-cancel-race-defect/defect-note.md`. The table above is the
post-fix record at the fixed head.

### W-01/W-02/W-03 consumers in the matrix

`w01-w02-w03-consumers.txt` extracts, per cell: W-01
(`direct_w01_consumer_probe`, core-only link — passes in all 8 cells),
W-02 (`request_scope_test` in all 8; `request_scope_uring_test` in the 4
liburing=y cells; `threadpool_external_loop_test` in all 8), W-03
(`uring_external_loop_test` and `uring_public_consumer_probe` in the 4
liburing=y cells). 36/36 consumer-oracle runs passed.

### Real-kernel io_uring paths

Every liburing=y cell runs the io_uring backend against the real kernel
7.2.9 ring (74-test suites include the uring seam builds, the
control-race/overflow/saturation oracles, and the new real-kernel
cancel-race oracle). io_uring construction itself reports the explicit
`not_supported` outcome in the no-liburing profile via the clean-room
verifier's w03 probe.

### Reference applications (built in both release cells, behavior-accepted
in release-y, copy re-verified in release-n)

`app-fixtures.txt` + `app-*.log`: `sluice-copy` default atomic pipeline,
bounded-buffer pipeline (`--pipeline-depth 4`), and `--sync all` variants
produce byte-identical output to an 8 MiB random source (`cmp` exit 0);
`sluice-hash` digest equals `sha256sum` on the same bytes; `sluice-grep`
matches `grep -c` (111 = 111); `sluice-tail` last-20 output equals
`tail -n 20` and follow mode observes an appended line.

## 2. Mutation kills (affected targets re-run at the fixed head)

`matrix/mutation-kills.txt` — all 16 mutation targets whose owner files
this campaign touched are killed (exit 1 or the expected fail-fast
SIGABRT), including the newly registered
`progress_source_uring_mut_final_probe_dropped` killed by
`k1_k2_cqe_before_drain_recovered_by_final_probe`. Per-target run logs are
alongside (`mut-<target>-run.log`). M6/M7/M8 remain TLA-mirror kills
(`scripts/verify_tla.sh`), not C++ builds.

## 3. Clean-room package verification (G9/RG2/RG3 refresh)

`scripts/verify_v2_package.py` both profiles at the clean head
(`bbc98fae`, the evidence commit): **24/24 gates PASS in each profile**,
exit 0, recorded in `matrix/package-verify-{liburing,noliburing}.log`
with argv/exit/sha256 alongside; manifests:
`v2-package-manifest-{noliburing,liburing}.json`; archive baselines
alongside. Gates: 11-header exact install set, sole-include compiles,
clean-room consumers (core-only and async workloads) compiled, linked and
executed, ODR probe across opposite macro views, seam negative probes
compiling the real installed headers with pinned diagnostics,
`w03 uring` reporting `not_supported` (exit 2) without liburing and real
io_uring selection (exit 0) with it. Verifier behavior self-test
refreshed at the same head: `matrix/package-verifier-selftest.log`
(14 tests, exit 0). Earlier attempts that failed only the
`V2_B_PROVENANCE_RECORDED` worktree-cleanliness gate (by design, before
the tree was committed and when sibling-profile outputs sat untracked in
the tree) are preserved in `package-first-run-dirty-tree/`; the recorded
manifests are the clean-head run.

## 4. Formal models (G8 refresh)

All four models re-verified at the campaign head:
`matrix/verify-tla.log` (RequestCore safety + ObserverCore + ShutdownCore,
including their mutation/negative-control gates) and
`matrix/verify-progress-source-model.log` (ProgressSource verdict
catalogue). Results: `VERIFY_TLA: PASS` (exit 0) and `ProgressSource
model: all expected outcomes reproduced.` (exit 0).

RequestCore liveness (`rcore-live`, the largest gate): complete space
31,289,609 states generated / 5,359,732 distinct / 0 left on queue —
byte-identical to the counts recorded in the #524 convergence evidence,
re-confirming the carried G8 row against the current tree. Depth 53 and
the 12-branch complete-space figure (64,316,784 = 12 × 5,359,732) are
attributed to the #524 record: this campaign's script prints only the
verdict and final state lines per gate, not the temporal-checking
summary line. Runner configuration for this campaign:
`SLUICE_TLA_JOBS=2 SLUICE_TLA_WORKERS=8` with the machine otherwise idle.
Note for future runs: an earlier attempt at `nice 19` concurrent with the
release matrix stalled in the liveness phase (no progress for ~50 min at
low CPU); the dedicated re-run completed cleanly and the recorded logs
are from that run.

## 5. Ledger closure

`docs/roadmap/v2-conformance.md` updated with GOV-02 vocabulary: G2 REQ-01
row VERIFIED (evidence `a0874ae2` + this directory), G6 PROG-02 io_uring
row VERIFIED (`6411cff3`+`5fd8e109` + coverage table), G7 LSan row
VERIFIED (`ff7d6dde` + disposition), G10 release-acceptance row
VERIFIED_AT_PR_HEAD (this matrix; gate closes at merge), RG1–RG7 gate
table updated, W-03 release-evidence note discharged by the matrix cells.

## 6. Remaining limitations (recorded, non-blocking)

- Single host, single compiler pair (GCC 16.2.1 primary, clang 22.1.8
  sanitizer), kernel 7.2.9: configurations outside `environment.txt` +
  `matrix.json` are untested by this release (disclosed in CHANGELOG).
- Bounded TLA+ instances unchanged from their carried records; model
  results are not production execution claims.
- GCC shared-libasan cannot run the tail CLI pending-signal harness
  (documented toolchain finding, `gcc-libasan-preload-collision/`).
- The RG5 mutant kill currently lives in the registered mutant target
  (never a regular test); it must be re-run whenever the probe or its
  oracle changes (VERIFY-06 obligation).

## 8. Post-review revision

The pre-merge re-review's two P1 evidence-accuracy findings (the SEM-06
row's stale discharged gap; the identity doc's non-existent registry claim)
and its P2 test failure-path finding (an unreleased frozen owner on the
cancel-race oracle's unexpected-disposition exit, generalized to every
live-owner failure exit in `tests/uring_progress_race_test.cpp`) are fixed
and re-verified in `post-review-revision/`. That directory records the
finding-to-fix disposition and the re-runs at the revision commit: debug-y
and release-y suites 74/74 passed, the race binary 23/23 across five
repeats, and the registered final-probe mutant still RED on
`k1_k2_cqe_before_drain_recovered_by_final_probe`. The revision's only
executable delta after the matrix content head `72acc209` is that test
file's failure-path release; no production code or configuration changed,
so the matrix, package, TLA and mutation records above stand as recorded.
