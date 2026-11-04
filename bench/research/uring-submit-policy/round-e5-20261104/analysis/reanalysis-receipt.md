# reanalysis-receipt.md — independent recomputation and accounting audit

Produced 2026-11-04 during archival, before any Git commit. Method: fresh
copies of the raw per-invocation records were analyzed with the committed
`analyze.py` (unmodified) and the outputs diffed against the stored analysis
receipts; measurement accounting facts were re-derived from the raw records.

**REANALYSIS_MATCH = YES**

## 1. Statistical reconstruction

| Stored receipt | Recomputed from | Result |
|---|---|---|
| `aa-calib-w02-4k-d16.json` (pre-repair A/A, arms=AA, 24 pairs) | `raw-minimal/aa-calib-w02-4k-d16/results.jsonl` | all 6 metrics match to 1e-12 (mean/median/ci_lo/ci_hi/half_width) |
| `aa-calib2-w02-4k-d16-smt.json` (SMT alternative, arms=AA) | `raw-minimal/aa-calib2-w02-4k-d16-smt/` | all match |
| `aa-repair-w02-4k-d16-m5.json` (repaired A/A, arms=AA) | `raw-minimal/aa-repair-w02-4k-d16-m5/` | all match |
| `ab-main-w02-4k-d16.json` (main A/B, arms=AB) | `raw-minimal/ab-main-w02-4k-d16/` | all match |
| `controls-family.json` (34 Holm entries + 9 shape tables) | 9 registered `ctl-*` dirs | all entries and tables match; `all_pass = False` both |

Bootstrap resampling is deterministic (10,000 resamples, seed 5950) and pair
data is committed verbatim, so this match is bit-exact, not approximate.
A/A half-widths recomputed from the repair receipt: cpu 0.44 / tput 0.43 /
p99 1.08 pp (PASS). Main A/B: cpu −26.46 % [−26.89, −26.02]; control family
FAIL driven by `ctl-d2` p95/p99/tput, exactly as stored.

## 2. CPU accounting audit

- **Primary metric**: bench per-measured-rep `CLOCK_PROCESS_CPUTIME_ID` delta
  (`cpu_s` in `results.jsonl.metrics`), summed over the 5 measured reps,
  divided by Σops (100,000 reads per pair member). Window = measured reps only;
  the warmup rep is flagged `measured: false` in the bench JSONL and excluded
  by the driver (`measured_records: 5` in every repaired-phase invocation,
  verified).
- **Secondary metric**: whole-invocation `wait4` rusage (`ru_total`), recorded
  for all 894 invocations; agrees in sign on every reported shape.
- **io-wq inclusion**: `raw-minimal/threadcheck-w02/results.jsonl` thread
  samples contain `iou-wrk-*` threads under `/proc/<pid>/task/` (thread group),
  with per-thread tick sums (1.290 s) ≤ child rusage (1.315 s) — process CPU
  covers io-wq; the registered additive /proc sampler was dropped as
  double-counting and used only here as a consistency check. No CPU-bearing
  thread outside the thread group was observed.
- **No double counting**: primary and secondary are separate records of
  different windows; neither adds the /proc delta. RSS: `ru_maxrss_kb`
  19,764 in all 48 main-phase invocations, both arms.
- **Denominator**: ops = completed valid reads (20,000/rep × 5), byte-oracle
  gated.

## 3. Enter accounting audit (scope + worst-case bound)

- **Scope**: `mechanism/strace-ledger.tsv` "enters" = ALL `io_uring_enter`
  syscalls per valid read (`strace -f -c -e io_uring_enter`), submit and wait
  uses combined. A pure-submit-only split was **not** instrumented and cannot
  be recovered from the archived data; `SLUICE_BENCH_STATS` counters are
  API-level (AsyncIoContext submit/poll/wait calls) and do not decompose
  syscall-level enters (archived main-phase means per op: submit 1.0 both
  arms; poll 0.1511 → 0.1310; wait 0.1007 → 0.0746). Consequently the −81.2 %
  figure must be quoted as *total* enters/op; an equal pure-submit reduction is
  **not** claimed.
- **Worst-case bound on the Mechanism gate** (from archived data only): the
  frozen gate needs ≥ 50 % reduction in submit-family enters. With A total
  3.0002 and B total 0.5630 enters/op, the gate could fail only if A carried
  more than 1.8742 wait-type enters/op (algebra: `wB ≥ 0` requires
  `0.5·wA > 3.0002/2 − 0.5630 = 0.9371`). The archived PMU round bounds
  blocking wait enters by context switches: 27,335 switches per 20,000-op rep
  → ≤ 1.367 blocking transitions/op < 1.8742 (each blocking enter causes at
  least one switch, so blocking enters ≤ switches). Even attributing *every*
  context switch to a wait-type enter, the gate cannot fail. Residual
  assumption: non-blocking GETEVENTS enters are not separately bounded, but
  they are bounded above by the A-side poll+wait API-call rate (0.2518/op),
  which is likewise far below 1.8742 if each API call issues one enter.
- **Registered as an observation gap** (RQ3-adjacent): future rounds should
  separate submit/wait enter classes at the syscall level.

## 4. Holm robustness note (D2 CPU)

Registered family (9 shapes, 34 entries): `ctl-d2` cpu p = 0.0078 ≤ Holm
α = 0.00833 → reject = True, mean 4.17 % ≤ 5 % → CPU metric alone reads SAFE;
the FAIL comes from p95 (p = 0.9564), p99 (p = 0.9993), tput (p = 0.9995).
Exploratory (disclosed, not registered): adding `ctl-d2-repl` to the family
(38 entries) moves the D2 CPU adjusted α to 0.00556 < p = 0.0078 → D2 CPU
would formally FAIL too. The verdict is `FAIL on D2` under either membership;
the registered convention (replication = confirmation only, per
PREREGISTERED.md §Pairing protocol) is the one used in all decision documents.

## 5. Exclusion and validity audit

- 894 invocation records across all 23 archived phase dirs; **0** with
  `exit != 0` — no rep was ever invalidated, and no post-hoc outlier removal
  occurred (frozen rule).
- `binary_sha256` in every record matches `build-notes.md`:
  `32beabcd…` (bench_w02_pipeline) / `1e548bc6…` (bench_w03_external_loop).
- Repaired-phase invocations carry exactly 5 measured records; the pre-repair
  calibration carries 1 — the two receipts are structurally distinguishable.
- `harness-smoke` (2-pair machinery validation) enters no analysis, per the
  frozen protocol note.

## 6. Full-archive integrity

`sha256sum -c evidence/MANIFEST.sha256` over the Layer-A tree: **1,859/1,859
entries pass** (1,853 under `evidence/`, 3 under `harness/`, 3 release
binaries under the worktree build). Backup tarball extract-verified the same
way (see `FULL_ARCHIVE.md`).
