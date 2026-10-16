# #536 correctness-fix receipt

BEGIN_536_CORRECTNESS_FIXES

ISSUE = 403 (slice: PR #536 correctness risks)
BRANCH = codex/403-g1-native-optimization (PR #536's original branch, OPEN)
REVIEW_BASE = 54631b5f (PR #536 head before these fixes)
FIX_COMMITS = b88750e3 (bounded, allocation-free progress pass) ;
              64e3241b (suppression-test lifetime) ;
              16e5d96a (oracle diagnostics + aligned-allocation counting) ;
              a6e72521 (keep the counter probe inside its guard)
HEAD = a6e72521
MASTER_BASE = 10aaf58b (frozen G1 baseline; untouched by this slice)

## What was fixed

| Risk | Fix | Evidence that the fix bites |
|---|---|---|
| 3.1 publication-batch allocation after accepted work left the publication queue | the pass takes its entry snapshot into construction-time storage (`publish_batch_` ring, `owed_batch_` vector sized at setup); no dynamic allocation remains on the pass path | `raw/negative-oracle/` (pre-fix: "pass allocated after accepted work was published (got 1, want 0)"), `raw/positive-oracle/`; `tests/threadpool_progress_pass_bound_test.cpp` case 0 asserts a zero allocation delta across the pass |
| 3.2 unbounded progress pass | the pass boundary is frozen at entry: at most the entry snapshot of queued publications plus one snapshot of owed events per call; residue (including owed events) is reported so the driver schedules the next pass instead of parking | same test, case 1: pre-fix "first pass absorbed events produced during its own deliveries (got 16, want 8)", fixed pass delivers exactly 8 and defers the rest; the oracle's producing sink creates one completion per delivery through a rendezvous, not a sleep |
| 3.3 test-owned resource leak | the suppression test holds its temporary file with an RAII `TmpFile` per case and asserts descriptor neutrality around each case | `raw/test-lifetime/`: the same assertion against the pre-fix test body fails 3/3 ("case 0 leaked descriptors (6 -> 10)"); the descriptor census shows the pre-fix body opening 8 temporary files and closing none, the fixed body 2 with both closed |

## Verification matrix at `a6e72521`

| Configuration | Command | Result | Log |
|---|---|---|---|
| ring, release, GCC 16.2.1 | `xmake f -m release -o build-536-ring --liburing=y -y && xmake test` | 74/74 | `raw/suites/ring-release-final.log` |
| no-ring, release, GCC | `--liburing=n` | 57/57 | `raw/suites/noring-release-final.log` |
| ring, debug, GCC | `-m debug` | 74/74 | `raw/suites/ring-debug-final.log` |
| ring, ASan+UBSan, clang 22.1.8 | `-m asanubsan --toolchain=clang`, `ASAN_OPTIONS=detect_leaks=0` | 74/74 | `raw/suites/asanubsan-ring-final.log` |
| ring, TSan, clang 22.1.8 | `-m tsan --toolchain=clang` | 74/74 (no reports) | `raw/suites/tsan-ring-final.log` |
| targeted LeakSanitizer, `detect_leaks=1` | four progress suites | clean (`rc=0` each) | `raw/suites/targeted-leak-check-final.log` |
| mutation campaign | 72 named mutants, 2 runs each | 0 build failures, 71 killed, 1 survived (the documented `uring_cutover_mut_drop_outcome`); both runs agree per mutant | `raw/mutation/` |
| TLC `RequestCore.cfg` (safety) | 16 workers, `-Xmx48g`, pinned jar | PASS rc=0, 31,289,609 generated / 5,359,732 distinct, depth 53, 48 s | `formal/raw/tlc/safety.log` |
| TLC `RequestCoreLive.cfg` (safety + liveness) | same | PASS rc=0, 12/12 temporal branches, 64,316,784 total distinct, 24 min 45 s | `formal/raw/tlc/liveness.log` |

Toolchain note: the default GCC toolchain cannot link the sanitizer modes on
this host (`ld: cannot find /usr/lib64/libasan.so.8.0.0` /
`libtsan.so.2.0.0`), which is the G1 record's `PERMISSION_REQUIRED` finding
reproduced; the sanitizer suites therefore use clang 22.1.8, whose runtimes are
present. Both attempts are kept in `raw/suites/` (`*-final.log` is the clang
one; the GCC link failure is recorded in the first-pass `asanubsan-ring.log`
and `tsan-ring.log`).

## Performance: did the fixes discard the adopted G1 gains?

Three paired, interleaved clean A/B sessions on the same host, one at a time,
using the frozen G1 runner and the frozen spec verbatim (`spec-ring.txt`),
with the frozen B0 and pre-fix arm binaries reused by hash.

**Session 1 — frozen B0 vs fixed head (ring)** (`ab-s1-b0-vs-fixed/AB.md`):

| case | reps | B0 ops/s | fixed ops/s | paired ratio | frozen G1 ratio |
|---|---|---|---|---|---|
| W02-tp-4k-d16-w4 | 15 | 239,828 | 277,157 | **1.1557** | 1.1934 |
| W02-tp-4k-d16-w1 | 15 | 224,569 | 206,854 | **0.9313** | 0.8898 |
| W02-tp-4k-d16-w4-saturate | 7 | 259,981 | 297,179 | **1.1277** | 1.1464 |
| W03-tp-4k-d16-w4 | 15 | 161,388 | 202,028 | **1.2385** | 1.2946 |
| W02-uring-4k-d16 | 15 | 356,456 | 355,374 | 0.9998 | 1.0196 |
| W03-uring-4k-d16 | 7 | 206,316 | 226,984 | 1.0454 | 1.0155 |
| W01-read_at-4k-seq | 7 | 1,001,991 | 1,005,462 | 1.0027 | 1.0065 |

**Session 2 — frozen pre-fix arm (`b5c687ea…`) vs fixed head (ring)**, i.e. the
fixes' own effect, isolated from the baseline (`ab-s2-prefix-vs-fixed/AB.md`):

| case | reps | pre-fix ops/s | fixed ops/s | paired ratio | paired min..max | cpu/op |
|---|---|---|---|---|---|---|
| W02-tp-w4 | 15 | 293,953 | 279,230 | 0.9909 | 0.8596..1.0877 | 11,247 → 11,782 ns |
| W02-tp-w1 | 15 | 211,904 | 208,609 | 1.0180 | 0.8857..1.3465 | 7,876 → 7,956 ns |
| W02-tp-w4-saturate | 7 | 262,891 | 278,026 | 1.0527 | 0.9135..1.1079 | 12,145 → 11,364 ns |
| W03-tp-w4 | 15 | 209,710 | 202,973 | 0.9588 | 0.8787..1.1165 | 14,946 → 15,745 ns |
| W02-uring | 15 | 337,050 | 346,999 | 1.0191 | 0.8756..1.1381 | 5,127 → 5,068 ns |
| W03-uring | 7 | 205,831 | 228,478 | 1.0060 | 0.9724..1.1321 | 9,918 → 9,761 ns |
| **W01 (null control)** | 7 | 970,268 | 1,012,930 | **1.0290** | 0.9948..1.1978 | identical binaries |

**Session 3 — frozen B0 no-ring vs fixed head no-ring** (`ab-s3-noring/`):
W02-tp-w4 1.1570, W02-tp-w1 0.9238, saturate 1.1870, W03-tp-w4 1.3506.

Verdict per the repository's living performance method:

* **ADOPT (retained):** the fixes keep the adopted G1 gains. W02-tp-w4
  1.1557/1.1570 against a frozen 1.1934, saturate 1.1277/1.1870 against 1.1464,
  W03-tp-w4 1.2385/1.3506 against 1.2946 — all inside the frozen record's own
  paired spread, and the io_uring controls stay in the ±3–5% cross-binary noise
  band (0.9998/1.0454 ring).
* **NO_CHANGE_JUSTIFIED for the fix's own cost:** session 2's only "cost" rows
  are W03-tp-w4 0.9588 and cpu/op +4.8% on W02-tp-w4, while the session's
  *null control* — W01, whose two binaries are byte-identical — reports 1.0290.
  A session whose identical binaries yield ±3% cannot resolve a 4% cost on a
  real pair; no fix-attributable regression is claimed or measured.
* **Disclosed regression, unchanged in kind:** W02-tp-w1 remains a regression
  (0.9313 ring, 0.9238 no-ring) as it was in the frozen G1 record (0.8898).
  These fixes neither cause nor repair it.
* **No A/B number in `ab/final` was rewritten.** The frozen directory is
  byte-identical (`git diff 54631b5f HEAD -- docs/evidence/g1-403-native-optimization/` is empty);
  the new sessions live in this directory with their own provenance headers,
  raw JSONLs and reports.

## Formal correspondence

`correspondence.md` holds the argument: the model text is byte-identical to the
G1 head (`git diff 0891be17 HEAD -- formal/` empty; the four asset hashes equal
the #524 `correspondence-after` table and the jar equals the pinned
`936a2620…`), the fixes change how many modeled actions one pass performs and
in what grouping but never what one of them does, and the fix extends
`immediate_work_remains` to owed events precisely so the weak-fairness
assumption the liveness configs use ("the driver keeps running passes while
work remains") still holds across the driver boundary. Both gates were re-run
at the fixed head anyway; their counters reproduce the G1/#524 record exactly
(safety 31,289,609 / 5,359,732 / depth 53; liveness 12 branches / 64,316,784).

Scope limits are in `correspondence.md` §6: bounded instances, no C++ memory
ordering, no kernel behavior, no wall-clock progress claim; the pass bound and
the residue report are C++ evidence, not model results.

## Mutation campaign

Reproduced at the fixed head with the frozen 72-mutant list and two runs per
mutant: see `raw/mutation/provenance.txt` and `raw/mutation/campaign.tsv` for
the tally and the per-mutant exit codes. The historical record this compares
against is the frozen G1 entry ("71/72 with only the documented F2 survivor");
it is a mutation-campaign statement, not a formal one, and this receipt does
not restate it as 72/72.

MUTATION_TALLY = 72 mutants, 0 build failures, 71 killed, 1 survived
SURVIVOR = `uring_cutover_mut_drop_outcome` (build_exit=0, run1_exit=0,
run2_exit=0 in `campaign.tsv`) — the frozen G1 record's documented
path-dependent F2 survivor (guard-caught, not assertion-caught). The fixed head
does not convert it and no other mutant survived, so the reproduction is 71/72,
not 72/72. Both runs agree on every mutant (exit 0/0 or nonzero/nonzero).

## Independent review

Two reviews were run by agents that did not implement the change, on different
worktrees:

* **PR #536 code review** — `APPROVE_WITH_FINDINGS`. Contract by contract:
  publication-batch safety SUPPORTED, pass bounding SUPPORTED (including
  re-entrancy: context-level re-entry is rejected with `invalid_state`),
  residue honesty / no lost wake SUPPORTED (shutdown closes admission before
  the drain, `internal_work_retired()` includes owed events, cancel/stop
  paths converge), C1 SUPPORTED (bit set only where a write succeeded or the
  counter is saturated, cleared only where the counter is provably zero, all
  under one mutex), C2 SUPPORTED (`outstanding_` ++ only at accept, -- only at
  `complete_publication`; settlement/teardown read `occupancy()`),
  test discrimination PARTIALLY_SUPPORTED at the time of review and now
  fully supported by the re-captured evidence and the aligned-allocation
  counting, test lifetime SUPPORTED, frozen evidence untouched SUPPORTED,
  build registration SUPPORTED (bare GCC ASan/TSan runs, clang TSan and clang
  ASan+UBSan builds all pass).
* **v2 architecture candidate review** — `DESIGN_READY_WITH_FINDINGS`; every
  finding is answered in the candidate's revision v2-c2 (`docs/README.md` and
  `docs/explicit-io-contract.md` in the v2 worktree / PR #542), whose major
  finding was baseline honesty in the decision tables.

A documentation review passing says nothing about PR #536's code, and this
code review says nothing about the v2 candidate; the two verdicts are
independent and recorded separately.

## Findings from the reviews that are recorded rather than fixed

1. **`shutdown_lifecycle_uring_test` LeakSanitizer finding (pre-existing,
   test-side).** `raw/suites/targeted-leak-check-uring-lifecycle.log`: 3,992
   bytes in 19 allocations, all rooted in the test's own
   `poison_with_in_flight_cqe` (`tests/shutdown_lifecycle_test.cpp:1008`,
   `std::make_unique<PoisonedState>`). Reproduced at the pre-fix head before
   these fixes existed; out of this slice's scope, recorded so it is not
   mistaken for a new defect. It is why the ASan suite runs with
   `detect_leaks=0`; the four progress suites are separately clean under
   `detect_leaks=1`.
2. **The two backends are not identical on residue reporting.** The verdict
   reviewer noted that `UringAsyncBackend::poll_progress` reports publications
   plus `has_immediate_physical_work()` and not the ThreadPool-style owed
   event, so it can return a quiescent wait outcome with a delivery owed. It
   is benign for liveness (the owed event's creation signals the source, so a
   parker wakes) but it is a real difference; `correspondence.md` states it
   instead of claiming the backends now agree.
3. **`retire_notification()` leaves `notification_pending_` set** after the fd
   is closed. Inert as reviewed (the fd is never reopened, the wake path
   early-returns, and no waiter exists once execution is closed), so it is
   recorded, not changed: changing it would touch the suppressed-write
   invariant the C1 fix protects.
4. **`owed_batch_[owed_bound]` has no runtime guard.** It is in bounds
   because `owed_batch_.size() == delivery_.size() == capacity_`, all three
   from `config.request_capacity`. A future capacity refactor would make this
   an out-of-bounds write; recorded as a constraint on that refactor.

## What is not claimed

* No merge, no issue closure: PR #536 stays OPEN, #403 stays open, #528 was not
  touched, v0.0.2 was not started.
* No new performance optimization; the A/B sessions only check retention.
* No universal correctness: the evidence covers the exercised configurations,
  on this host, at these commits.
* The mutation tally is a campaign statement with its documented survivor, not
  a proof; the frames are in `raw/mutation/`.

END_536_CORRECTNESS_FIXES
