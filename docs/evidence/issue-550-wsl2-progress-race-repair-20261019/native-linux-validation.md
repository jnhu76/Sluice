# Native Linux cross-validation — issue #550 / PR #552

Validation-only record: this round merges nothing, closes nothing and re-tags
nothing. The document itself is committed to the PR branch as the native
validation receipt; WSL2 results in `README.md` are unchanged and remain the
repair record. The native-Linux cross-check was performed on a second host
on 2026-10-19.

## Environment

| Field | Value |
|---|---|
| Host / access | `jnhu@192.168.31.75` (host `E5`), SSH key auth |
| Virtualization | bare metal (`systemd-detect-virt` = `none`) |
| OS | Fedora Linux 44 (KDE Plasma Desktop Edition) |
| Kernel | 7.2.9-201.fc44.x86_64 (independent kernel; not WSL — `/proc/version` is a Fedora mock build, no binfmt WSLInterop) |
| CPU | Xeon E5-2666 v3, 10 cores / 20 threads |
| RAM | 62 GiB |
| gcc | 16.2.1 20260819 (Red Hat 16.2.1-2) |
| clang | 22.1.8 (present; unused) |
| xmake | 3.0.6+20260117 |
| liburing | 2.13 (pkg-config) |

The xmake version differs from the WSL2 round (2.9.7 local there, 3.0.6
here). The complete build graph (test targets, mutant-free regular targets,
liburing switch, per-config isolated `-o` directories) configured and built
without modification under 3.0.6; all counts below match the WSL2 matrix, so
the build-version difference did not alter test registration or behavior.

## Sources and isolation

```
~/Source/Sluice                       owner checkout (master untouched; PR ref fetched in)
~/sluice-550-native/baseline          git worktree @ v0.0.2  = 7c6dbb3d0492087af1a03de11aa4f4a0f56ec496
~/sluice-550-native/candidate         git worktree @ PR #552 head = 0c0c3491873aac4b2691246afa571e3ee3176a31
~/sluice-550-native/builds/*          isolated per-config build dirs (xmake -o)
~/sluice-550-native/evidence/*        raw receipts (campaign outputs, DONE markers, logs)
~/sluice-550-native/mutants/*         isolated mutant sources/diffs/binaries/logs (never on the PR branch)
```

Diff scope re-checked on the candidate worktree:
`git diff --name-only master...HEAD` = `tests/threadpool_progress_race_test.cpp`
and `docs/evidence/issue-550-wsl2-progress-race-repair-20261019/{README.md,review.md}`
only. PRODUCTION_SOURCE_CHANGE = NONE, PUBLIC_API_CHANGE = NONE,
BUILD_GRAPH_CHANGE = NONE, FORMAL_MODEL_CHANGE = NONE.

Builds: both baseline and candidate with
`xmake f -m release --liburing=y --toolchain=gcc -o <isolated dir> -y && xmake build -y -g test`.

- baseline binary SHA256 `10e9843cd8423928a60c492da951a829a5a64045bd454d64e03dde0483dd23d2`
- candidate binary SHA256 `61071f731a772c8572bb82aaff618b712798dabae218fa4301a9c34061351b79`

## Baseline reproduction (v0.0.2)

| Campaign | Runs | Fails | Receipt |
|---|---:|---:|---|
| sequential | 360 | 0 | `evidence/baseline-seq/DONE` |
| 8-proc concurrent (60x8, real `&` + `wait`) | 480 | 0 | `evidence/baseline-c8/DONE` |
| 16-proc concurrent (30x16) | 480 | 0 | `evidence/baseline-c16/DONE` |
| 8-proc pinned to CPUs 0-3 (2x oversubscription) | 480 | 0 | `evidence/baseline-pinned4/DONE` |

Per-run receipts (id, rc, wall, first failing subtest, stdout/stderr) in
`evidence/baseline-*/runs/`; binary SHA per campaign in `binary.sha256`;
load average before/after in `loadavg.{before,after}`.

```
BASELINE_NATIVE_FLAKE = NOT_OBSERVED_IN_SAMPLE   (1800/1800 green)
```

The WSL2 flake (6/480 @8p, 15/480 @16p on the same three subtests) did not
reproduce on this host in sample. Per plan this is not evidence the defect is
absent — the defect is a missing test-side happens-before edge whose losing
interleaving depends on scheduler timing; the mutation campaign below is what
carries discriminative power on this host. No unbounded stress was attempted
to force a red baseline: one bounded amplification (the pinned 4-CPU sample)
was added and remained green.

Two earlier pinned-campaign attempts were discarded and are preserved as
`evidence/DISCARDED-baseline-pinned4-harness-bug` and
`evidence/DISCARDED-baseline-pinned4-empty-bin`: both failed with rc=127
before executing any test (a nested-`timeout` quoting bug, then an
unexported variable). They are excluded from all counts.

Run cost on this host: mean wall 0.01 s per full binary run (21 subtests),
stderr empty, exit 0 — verified on sampled runs (`ok` x21 + final summary
line).

## Candidate verification (PR #552 head)

Identical orchestration and machine conditions:

| Campaign | Runs | Fails | Receipt |
|---|---:|---:|---|
| sequential | 360 | 0 | `evidence/candidate-seq/DONE` |
| 8-proc concurrent | 480 | 0 | `evidence/candidate-c8/DONE` |
| 16-proc concurrent | 480 | 0 | `evidence/candidate-c16/DONE` |
| 8-proc pinned to CPUs 0-3 | 480 | 0 | `evidence/candidate-pinned4/DONE` |

```
CANDIDATE_SEQUENTIAL    = 360/360 PASS
CANDIDATE_CONCURRENT_8  = 480/480 PASS
CANDIDATE_CONCURRENT_16 = 480/480 PASS
CANDIDATE_PINNED_4CPU_8P = 480/480 PASS
```

0/N is sample evidence, not a probabilistic race-freedom proof.

Oracle checks per plan (assertions unchanged from the WSL2 record; none
loosened):

- r4: `worker_signaled && is_progress(result,1) && prepark==0 && ready()`
  — owner resumed only after the witness; owner never reaches the prepark
  counter (first epoch revalidation catches the signal).
- saturated: `saturated_readable && worker_signaled &&
  readable_after_saturated_signal && is_progress(result,1) && prepark==1 &&
  ready()` — test-injected eventfd readiness is asserted separately from the
  worker signal; readiness must survive the saturated signal.
- control: worker publication + signal witnessed, then a single causal
  `wait_one` returns `progress(1)`; the 1000x retry loop remains removed.

## Witness re-verification (code-level, on this candidate SHA)

Chain re-read line-by-line on the candidate worktree:

1. `worker_loop` (src/async/threadpool_backend.cpp:440-465): pause gate →
   `offer_terminal` → `release_execution` → `publication_pending_.push_back`
   under `work_mtx_` → `signal_ready_progress()`. Enqueue precedes signal in
   program order.
2. `signal_ready_progress` → `progress_port_.signal()` →
   `ProgressSource::signal()` (progress_source.hpp:252-264): epoch bump and
   `wake_notification_nolock_` (eventfd write) inside one `mtx_` hold;
   saturating, never-wrapping increments.
3. `snapshot()` (progress_source.hpp:91-94) takes the same `mtx_`; observing
   a changed two-field token therefore implies the signal critical section
   completed (mutex handoff happens-before), and by program order the
   publication enqueue happened before that.
4. Producer set in each scenario = {the gated worker's completion signal}:
   `make_pool_context(4, 1)` (single worker), one real read, no zero-op /
   cancel / second submit; `interrupt_progress_waiters` advances only
   control-domain fields which are not part of the progress `Token`; the
   worker's other signal sites (unclaimed retirement, cancel/publication
   paths) are unreachable in these scenarios.
5. No false witness from saturation/wrap: both token fields only increase
   (saturate) and the comparison uses both fields.
6. Owner stays gated until the witness holds in r4/saturated (owner gate
   resumed only after `wait_worker_completion_signal` returns); the control
   scenario performs its final causal `wait_one` after the witness.
7. Witness failure is safe: watchdog returns false with a diagnostic; gates
   are still resumed, the driver joined, `RequestPublicationDrain` and
   request discard still run — empirically exercised by the M1 runs below
   (bounded failure, clean exit, no leftover threads/processes).

The `yield()` loop polls a mutex-observable causal state; the 10 s bound is a
watchdog that only converts to a failure report. No sleep-for
synchronization, no retry inflation, no deadline extension, no loosened
assertions anywhere in the PR diff.

## Mutation campaign (native Linux)

All mutants are isolated copies under `~/sluice-550-native/mutants/` built by
direct `g++` with the same source set as the xmake target
(`run-mutants.sh`, `run-m23.sh`; the scripts contain a harmless duplicated
FLAGS line — both lines are identical after the typo was sed-corrected, so
the effective flags are exactly those recorded). A pristine build with the
same recipe passes 21/21 (`bin/pristine`, rc=0), validating the recipe.

Build command shape (pristine; mutants substitute one source or add one
include overlay):

```
g++ -std=c++20 -O2 -g -pthread -DSLUICE_ASYNC_INTERNAL_TESTING \
    -I<overlay include>? -I include -I src/async \
    tests/threadpool_progress_race_test.cpp src/async/threadpool_backend.cpp \
    src/async/async_io_context.cpp src/async/fail_fast.cpp src/async/request_scope.cpp \
    src/async/detail/context_identity.cpp src/async/detail/request_core.cpp \
    src/blocking_file.cpp src/file_resource.cpp -o bin/<name>
```

| Mutant | Injected defect (diff) | Runs | Result | Death signature | Verdict |
|---|---|---:|---|---|---|
| M1 | post-publication worker signal dropped (`m1.diff`) | 5 | 5 killed | rc=1, witness watchdog fires: `worker completion signal missing after 10000ms: token 0/0 -> 0/0`, `FAIL: r4_worker_completion_signal_before_final_recheck_is_caught`; wall 18.05 s bounded | KILLED 5/5 |
| M2 | first epoch revalidation dropped (`m2.diff`) | 5 | 5 killed | rc=1, `FAIL: r2_signal_between_token_and_ack_is_revalidation_caught` | KILLED 5/5 |
| M3 | EAGAIN tolerance removed from `wake_notification_nolock_` (`m3.diff`) | 5 | 5 killed | rc=134, `ProgressSource: write failed with errno=11 (wait-domain failure)`; abort occurs inside `saturated_notification_still_wakes_parked_owner` (17 subtests ok, the saturated subtest never prints ok) | KILLED 5/5 |
| M5 | signal/enqueue order swapped (`m5.diff`) | 20 | 0 killed | all rc=0; one run wall 8.05 s (typical 0.01 s) | NOT_KILLED 0/20 |
| M5P | M5 + 200 ms forced window between signal and enqueue, probe test copy prints r4 conjuncts (`m5p.diff`) | 5 | 5 killed | rc=1, `FAIL: r4_...`; probe line `worker_signaled=1 is_progress1=1 prepark=1 ready=1 kind=0 completed=1` | KILLED 5/5 (forced) |

Binary SHA256s: m1 `1dacf0e9…`, m2 `291e19c7…`, m3 `d122cd38…`, m5
`e44d92b7…`, m5p `9a3e1559…`, pristine `064a5ebb…`, tsan `d11b8015…`.
Diff SHA256s: m1 `3a0dd71e…`, m2 `45cad1c6…`, m3 `5027c9e7…`, m5
`d7904de4…`, m5p `b6caab79…`.

### M3 upgrades the EAGAIN evidence

On WSL2 the saturated scenario's EAGAIN traversal was proven by precondition
only. On native Linux M3 turns it into direct empirical evidence: with the
EAGAIN branch removed, the fail-fast fires with `errno=11` (EAGAIN) exactly
while the saturated subtest runs — the branch is genuinely traversed by
`saturated_notification_still_wakes_parked_owner`, and the pristine build's
tolerant branch is what preserves read-readiness there.

### M5 analysis (unforced miss is scheduling, not oracle blindness)

Erroneous DAG (M5): worker signals → main observes token → resumes owner →
owner's pass finds `publication_pending_` still empty → worker enqueues with
no further signal.

- Reachability of the interleaving: in the mutant it is a legal program
  order; the unforced window is the worker's remaining critical section
  (return from `signal()` + acquire `work_mtx_` + push) versus the owner's
  path (gate wakeup → revalidation → pass under `access_mtx_`/`work_mtx_`).
  The consumer path is orders of magnitude longer, so 20 unforced runs never
  land the owner's empty pass inside the window (0/20 killed, matching WSL2
  0/20).
- Footprint: one unforced M5 run took 8.05 s (vs 0.01 s typical) and still
  passed — consistent with the owner parking past the spent signal and
  recovering only via its 8 s bounded deadline → final pass → publication.
  The defect manifests as latency, not assertion failure, in the unforced
  test.
- Forced probe (M5P): with a 200 ms worker-side window, r4 fails 5/5 with
  `prepark=1` — the owner parked once more after the signal, and the exact
  oracle conjunct `prepark == 0` is what catches it. The oracle therefore
  discriminates the interleaving whenever it actually occurs.

```
M5              = NOT_KILLED (0/20 native; 0/20 WSL2)
M5_EQUIVALENCE  = REFUTED_BY_FORCED_PROBE (M5P killed 5/5, signature prepark=1)
M5_COVERAGE_GAP = YES (adopted from independent review) — measured as suite
                  coverage, not oracle discriminative power: the unforced
                  suite cannot schedule the window (producer remainder is a
                  few uncontended instructions; the consumer path to its
                  next park is microseconds including a futex wake), so 0/20
                  kills despite one real occurrence (the 8.05 s run, whose
                  deadline recovery landed in a park-tolerant subtest). The
                  oracle itself discriminates the interleaving whenever it
                  occurs (M5P kills 5/5 on the exact `prepark == 0`
                  conjunct), and the interleaving exists only in the
                  mutant's swapped program order — production's
                  enqueue-then-signal order makes it unreachable. Killing it
                  deterministically would require a new worker-side pause
                  seam between signal and enqueue, a mechanism addition that
                  needs its own justification and was not made in this PR.
                  Worst-case residual hazard of the mutant order (an
                  unbounded wait_one parked after a zero-completion wake)
                  is unobservable to any bounded test in this file.
```

M4 (control/publication baseline discriminator) has no native red sample:
the unfixed code stayed green in sample (see baseline section), so its
discriminating evidence remains the WSL2 record (6/480 @8p, 15/480 @16p
failing exactly the three repaired subtests). M1 is the stronger native
equivalent for the witness mechanism.

## Correctness matrix, TSan, hygiene (candidate HEAD)

| Config | Result |
|---|---|
| debug × liburing=n | 100% — 56/56 |
| debug × liburing=y | 100% — 74/74 |
| release × liburing=n | 100% — 56/56 |
| release × liburing=y | 100% — 74/74 |

Counts identical to the WSL2 matrix. Isolated `-o` build dir per config.
Real io_uring backend confirmed executing (not a compile-only stub):
`uring_progress_race_test` on the release/liburing build reports
`all 23 uring progress race tests passed` (`evidence/uring-real-backend-run.log`).

- `TSAN_STATUS = PASS` — `g++ -fsanitize=thread -O1 -g` build of the race
  test (`mutants/bin/tsan`), 5/5 runs rc=0, zero `WARNING: ThreadSanitizer`
  lines (`mutants/logs/tsan-{1..5}.err`).
- `git diff --check master...HEAD` clean.
- `scripts/test_check_cpp_comment_authority.py` → OK.
- `scripts/test_verify_v2_package.py` → `Ran 14 tests … OK` (the trailing
  `negative probe … unexpected reason` line is the negative-probe echo of
  the self-test, after the unittest verdict).

SIGABRT coredumps observed for `threadpool_external_loop_test` and
`app_copy_pipeline_oracle_test` during the debug matrix run are intentional
forked-child fault injection (both tests fork children that trigger
invariant violations; see `tests/threadpool_external_loop_test.cpp` around
the fork helper and `tests/app_copy_pipeline_oracle_test.cpp:912`); the
suite itself reported 100% pass. Not a failure.

## Failure paths and recycling

- M1 (witness can never be established) is the controlled watchdog-failure
  path: 5/5 runs end rc=1 after 18.05 s bounded (10 s witness watchdog + 8 s
  driver bound), printing the missing-token diagnostic; no hang, no
  std::terminate, no leftover processes (`pgrep` clean after the campaign).
- M3 is the controlled eventfd-write-abnormal path: fail-fast reports
  `write failed with errno=11` rather than corrupting wait state.
- Teardown order (gates resumed → driver joined → prepark counter detached →
  publication drain → discard) exercised green in all 1,800 candidate
  campaign runs (plus once per matrix config) and under failure in all 15
  M1/M3-failing runs.

## Independent review

Two independent reviewers (fresh contexts, not involved in the WSL2 repair
or this validation) audited before this document was committed.

Reviewer A — concurrency causality and oracles (verdict: APPROVE_WITH_NOTES):

- Reconstructed all three execution DAGs edge-by-edge (work_mtx_ program
  order; ProgressSource single-mutex signal; snapshot mutex handoff; gate
  release/acquire; driver snapshot-before-paused ordering proving the
  observed token predates the signal). Could not construct a legal
  interleaving that breaks the witness or the oracles in pristine code.
  Confirmed the producer-set enumeration (including that the unclaimed
  retirement, dispatch-injection, zero-op, cancel and close_admission signal
  sites are unreachable in the three scenarios) and that pause gates sit
  outside all mutex holds, so a paused driver cannot deadlock the witness.
- Endorsed M3 as direct EAGAIN-traversal proof (independently verified the
  kernel's strict eventfd admission arithmetic and cross-checked that
  `owner_parked_at_saturated_epoch_wakes_and_reparks` — which drains before
  its signal — passes under M3 exactly as the fd arithmetic predicts) and
  M2 as the exact dropped-first-revalidation death at r2.
- Findings: F1 (P2) M5_COVERAGE_GAP should be recorded YES by the
  suite-coverage definition — adopted above. F2 (P2) witness sufficiency
  conditional on pristine program order — already documented in the test's
  own comment; no change. F4 (P3) witness spin cost (bounded, test-only).
  F5 (P3) the 8.05 s M5 run cannot be attributed to a specific subtest from
  the receipts (mechanism certain, absorbing subtest unpinned). F6 (P3)
  native green baseline framing — matches the limitations below.

Reviewer B — experiment design, receipts and merge scope
(verdict: APPROVE_WITH_NOTES):

- Verified host is genuinely bare metal (kernel built by Fedora mockbuild,
  systemd-detect-virt none); baseline/candidate built with identical
  toolchain and isolated dirs; campaign scripts genuinely parallel (`&` +
  `wait`, taskset pinning); recomputed fail counts independently from raw
  rc files (all 8 campaigns 0 fails); both DISCARDED attempts confirmed as
  no-test-executed harness bugs excluded from counts; all five mutant diffs
  minimal and semantic; all quoted SHA256 prefixes match; TSan binary
  genuinely instrumented (libtsan linked); package-verifier trailing line
  traced to `verify_v2_package.py:337` (negative-probe echo, not a
  failure); merge scope exactly test + evidence docs; SIGABRT cores
  confirmed intentional fork-injection.
- Confirmed the honest framing: the native A/B campaigns carry candidate
  stability evidence, and discriminative power is carried by the mutation
  campaign.
- Findings: P2 disposition of this document (resolved: committed with the
  PR) and P2 blank inline summaries in `matrix-campaign.log` (ANSI-defeated
  grep; per-config test logs hold the authoritative 56/74/56/74 counts —
  corrected summary appended to the remote log). P3: subtest-name typo in
  an earlier draft (fixed), config logs not capturing exact `xmake f` argv
  (covered by the reproduction commands), first-round m2/m3 rc=127
  placeholder rows superseded by `m23-campaign.log` (disclosed), M3's stale
  comment left in the mutant copy (irrelevant to kill validity).

No P0/P1 findings. Both reviewers state the evidence supports native
validation passing and the PR being merge-ready from their standpoints; no
assertion was loosened and no production change was made to chase any
mutant.

## Limitations and honest boundaries

- Baseline flake not observed natively in 1800 samples; the cross-environment
  claim for the repair rests on (a) the WSL2 red→green record, (b) the
  native mutation kills (M1/M2/M3 5/5 each, M5P 5/5), and (c) the native
  matrix/TSan. A green native baseline does not weaken the WSL2 defect
  report.
- 0/N pass counts are sample evidence, not race-freedom proofs.
- M5 remains not killed by the unforced test on either host; the forced
  probe proves the oracle discriminates the interleaving, and the
  interleaving is unreachable in production program order. No production
  change was made to chase the mutant.
- xmake differs across hosts (3.0.6 native vs 2.9.7 WSL2); build graph and
  test counts are identical, and all reproduction here bypassed nothing
  (full xmake builds for campaigns and matrix; direct g++ only for isolated
  mutants/TSan, validated by a 21/21 pristine build).
- The `run-mutants.sh`/`run-m23.sh`/`run-matrix.sh` scripts kept on the
  validation host contain a cosmetic duplicated FLAGS line (identical after
  correction); effective flags are exactly those quoted above. Campaign
  harness bugs were discarded and preserved, never counted.
- Remote raw logs live under `~/sluice-550-native/evidence/` and
  `~/sluice-550-native/mutants/logs/`; only this document and the PR comment
  are committed to the repository.

## Reproduction (on the native host)

```
git -C ~/Source/Sluice fetch origin pull/552/head:fix/550-threadpool-progress-test-synchronization
git -C ~/Source/Sluice worktree add ~/sluice-550-native/baseline v0.0.2
git -C ~/Source/Sluice worktree add ~/sluice-550-native/candidate fix/550-threadpool-progress-test-synchronization
cd ~/sluice-550-native/<tree>
xmake f -m release --liburing=y --toolchain=gcc -o ~/sluice-550-native/builds/<dir> -y
xmake build -y -g test
~/sluice-550-native/campaign.sh <binary> <outdir> seq|c8|c16
~/sluice-550-native/pinned-campaign.sh <binary> <outdir>
~/sluice-550-native/run-mutants.sh   # after make-mutants.py
~/sluice-550-native/run-m23.sh
~/sluice-550-native/run-matrix.sh
```
