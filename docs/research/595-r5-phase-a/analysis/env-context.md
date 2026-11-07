# #595 R5 — Environment Context and Anchor Verification (Phase A)

Role: one-stop environment + anchor record for the R5 round. Written by the
environment/anchor verifier on 2026-11-06 (UTC timestamps below). Everything
here was executed or read in this session on the native E5 host; no number is
copied from another round without an archive citation.

## 1. Native E5 environment facts (all verified this session)

| Item | Value | Command |
|---|---|---|
| CPU model | Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz (family 6, model 63, stepping 2) | `lscpu` |
| Topology | 20 CPUs, 10 cores/socket x 2 threads (SMT on), 1 socket, 1 NUMA node (node0 = CPUs 0-19) | `lscpu` |
| Cache | L1d/L1i 320 KiB (10 instances), L2 2.5 MiB (10), L3 25 MiB (1) | `lscpu` |
| SMT pairs (frozen in R0-IDENTITY) | core2: CPUs 2,12; core8: CPUs 8,18 (`r4/BASELINE.md:17`) | lscpu/lscpu -e (R0 record) |
| Virtualization | **none** — `systemd-detect-virt` printed `none` (exit 1 = no virt detected); `lscpu` shows `Virtualization: VT-x` (CPU capability only, no hypervisor detected, no "Hypervisor vendor" line) | `systemd-detect-virt` |
| Kernel | 7.2.11-200.fc44.x86_64 | `uname -r` |
| Compiler | g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2) | `g++ --version` |
| liburing | liburing / liburing-devel 2.13-2.fc44 (pkg-config 2.13, `/usr/include/liburing.h` present) | `rpm -q liburing liburing-devel`, `pkg-config --modversion liburing` |
| Governor | schedutil (cpu0 checked); freq range 1.2–3.5 GHz (`r4/BASELINE.md:23`) | `cat .../scaling_governor` |
| Shell affinity | pid 4837 affinity = 0-19 (unpinned) | `taskset -cp $$` |
| tmpfs /tmp | ~31-32G (R4 receipts: `tmpfs 32G ... 31G` free) | R4 LOCK_RECEIPT pre-states |
| perf (R0 record) | perf 7.2.11, `perf_event_paranoid=2` (no PMU per-thread attribution; `r4/RESULTS.md:439`) | R0/R4 archive |

Consistency vs frozen R0-IDENTITY (`/home/jnhu/sluice-595-e5/evidence/platform.txt`
as transcribed in `r4/BASELINE.md:12-30`): model/core-count/kernel/gcc/liburing/
governor/tmpfs all match. No anomaly.

Measurement-lock state at session time (non-blocking probes, 2026-11-06 ~22:48
local / 14:48Z):
- `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` (R4's lock, inode 6941075,
  size 0): **free**.
- `/home/jnhu/sluice-595-e5/MEASUREMENT.lock` (the lock named by THIS round's
  constraints): did **not exist** at session start; my probe
  `flock -n <path> -c 'echo probe-acquired'` created it as an empty file
  (size 0) as a side effect of flock. It is now present and free. See §8
  anomaly A1: R5's constraint names a different lock path than R4 used.

## 2. Main repo and worktree state

- `git -C /home/jnhu/Source/Sluice status --porcelain` → empty (exit 0);
  branch `master` at `271b81bf`. Clean before and after this session's
  worktree additions (only `.git/worktrees` metadata was written, which the
  ask explicitly permits).
- `git -C /home/jnhu/Source/Sluice worktree list` (full, in order):

| Path | HEAD | Branch |
|---|---|---|
| /home/jnhu/Source/Sluice | 271b81bf | master |
| /home/jnhu/sluice-595-e5/archive-wt | 3492dd3d | research/595-e5-submission-archive |
| /home/jnhu/sluice-595-e5/causal-r2/worktree | 271b81bf | research-595-r2-causal |
| /home/jnhu/sluice-595-e5/causal-r2/worktree-timing | 271b81bf | research-595-r2-timing |
| /home/jnhu/sluice-595-e5/r3/worktree | 0a22a791 | research-595-r3 |
| /home/jnhu/sluice-595-e5/r4/worktree | f3b25329 | research-595-r4 |
| /home/jnhu/sluice-595-e5/worktree | 271b81bf | research-595-bounded-progress |
| /home/jnhu/sluice-595-e5/r5/worktree | f3b25329 | research-595-r5 (new, this session) |
| /home/jnhu/sluice-595-e5/r5/worktree-diag | f3b25329 | research-595-r5-diag (new, this session) |

## 3. R5 workspace preparation (this session)

- `/home/jnhu/sluice-595-e5/r5` did **not** exist before this session; nothing
  had to be inventoried or preserved.
- Created with `git worktree add ... -b` (both branches did not exist
  beforehand, verified with `git rev-parse --verify`):
  - `research-595-r5` @ `f3b25329bc8a64a201961c426a58adacfb8fdf08`
  - `research-595-r5-diag` @ `f3b25329bc8a64a201961c426a58adacfb8fdf08`
- `mkdir -p r5/{analysis,evidence,raw-diag,r4-archive}` — all four exist.
- Post-check: both worktree HEADs = `f3b25329bc8a…`, both porcelain = 0.
- `r5/r4-archive/TAIL-LATENCY.md` extracted from the
  `research/595-r4-state-aware` branch (not present in local r4; see §6).
  sha256 `315e2cda137045f12d2b3e8e34995bcb0e27613a953b55ba188cc6071a72a9f3`.

## 4. Anchor verification (command + result, all run this session)

| # | Anchor | Command | Result | Verdict |
|---|---|---|---|---|
| 1 | PR 629 head/base | `gh pr view 629 --json state,headRefName,headRefOid,baseRefName` | `{"baseRefName":"master","headRefName":"research/595-r4-state-aware","headRefOid":"ec8c8feab20007b0156650e579434bdaee2bb17d","state":"OPEN"}` | **match** (head = ec8c8fea…, base = master) |
| 2 | Commit f3b25329 exists | `git rev-parse f3b25329bc8a64a201961c426a58adacfb8fdf08` | `f3b25329bc8a64a201961c426a58adacfb8fdf08` | match |
| 3 | Commit d9d140ee exists | `git rev-parse d9d140eea180f29f629992aa293109acdeff1e03` | `d9d140eea180f29f629992aa293109acdeff1e03` | match |
| 4 | Anchor diff sha256 | `git diff d9d140ee f3b25329 \| sha256sum` | `d66f3bcf52d01c5ee19ecd6285d67586d4467a44ecc8d608b1b5a8e9b267cf49` | **exact match** |
| 5 | Archived patch == anchor diff | `git show research/595-r4-state-aware:docs/research/595-r4-state-aware/evidence/impl-f3b25329.patch \| sha256sum` | `d66f3bcf52d01c5ee19ecd6285d67586d4467a44ecc8d608b1b5a8e9b267cf49` (59814 bytes) | **byte-identical** (same sha256 ⇒ same bytes; no ordering difference to explain) |
| 6 | Issue 595 | `gh issue view 595 --json title,labels,body` | title `perf(uring): shape-aware submission policy — amortize io_uring_enter without progress or tail regressions`; labels **[]** (none); body read in full (18,776 chars) | fetched |
| 7 | r4 worktree HEAD | `git -C r4/worktree rev-parse HEAD` + `status --porcelain` | `f3b25329…`, porcelain empty | match; r4 tree untouched this session (read-only) |

### 4.1 Issue 595 current boundary (from the fetched body)

- **Status**: sole active scoped submission-policy implementation owner;
  #609 CLOSED completed (generic Async baseline shipped by PR #618 @ master
  `98825793`); #596/#597/#599 being consolidated/closed not_planned with
  conditional questions moved to #617.
- **Required for #595 claims**: whole-system CPU/op (submitter + kernel +
  io-wq + SQPOLL poller if used), actual enters/SQEs, notification/eventfd/
  park costs, idle tax, placement/SMT effects, and p95/p99/split-owner/
  cancel/overflow/shutdown negative controls; unmeasured kernel-worker cost
  must be disclosed. No duplicate benchmark program; no code rewrite.
- **Three internal policies** to compare: IMMEDIATE / PROGRESS_DRIVEN /
  BOUNDED_BATCH (experiment until an enforceable bound + cost is shown).
  No timer thread, public batching knob, virtual scheduler, or global policy
  manager.
- **Hypotheses P1–P4**: shape-aware selection (primary), first-flush
  elimination, residue-notification coalescing, Core-lock decision cost.
- **Correctness**: deferral changes time, not semantics; repair the four
  disclosed #594 oracle gaps (sleep-based wake test, deferred multi-entry
  retry, deferred-shape cancel/overflow/teardown mutants, #585 counter
  reconciliation). BASE GREEN → NAMED MUTANT RED → RESTORED GREEN.
- **Workflow W0–W6** continuous, no ceremonial gates; W4 E5 native targeted
  matrix with same-host A/A + >=15 interleaved AB/BA pairs; W6 vocabulary
  POLICY_PERF_SUPPORTED / PRODUCT_CANDIDATE_READY / KEEP_IMMEDIATE /
  KEEP_K4_RESEARCH_ONLY / BLOCKED_CORRECTNESS / INCONCLUSIVE.
- **Labels**: none. **State**: open.

## 5. Path table (R5 round)

| Purpose | Path |
|---|---|
| Main repo (read-only this round) | /home/jnhu/Source/Sluice (master @ 271b81bf) |
| R5 main worktree (research changes only) | /home/jnhu/sluice-595-e5/r5/worktree (research-595-r5 @ f3b25329) |
| R5 diagnostics worktree | /home/jnhu/sluice-595-e5/r5/worktree-diag (research-595-r5-diag @ f3b25329) |
| R5 analysis outputs | /home/jnhu/sluice-595-e5/r5/analysis (this file) |
| R5 evidence | /home/jnhu/sluice-595-e5/r5/evidence |
| R5 raw diagnostics | /home/jnhu/sluice-595-e5/r5/raw-diag |
| R5 copies of missing r4 archive docs | /home/jnhu/sluice-595-e5/r5/r4-archive (TAIL-LATENCY.md) |
| R4 history evidence (READ-ONLY) | /home/jnhu/sluice-595-e5/r4 |
| R4 worktree (READ-ONLY, never write) | /home/jnhu/sluice-595-e5/r4/worktree (research-595-r4 @ f3b25329) |
| R3 measurement lock (R4-era) | /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock (inode 6941075) |
| R5 measurement lock (per this round's constraints) | /home/jnhu/sluice-595-e5/MEASUREMENT.lock (created empty this session, see A1) |
| Frozen contract | /home/jnhu/sluice-595-e5/r5/worktree/docs/explicit-io-contract.md (v2-r1) |
| Standing performance method | /home/jnhu/sluice-595-e5/r5/worktree/docs/performance/methodology.md (53 lines) |
| v2 conformance ledger | docs/roadmap/v2-conformance.md (in-repo) |
| R0 identity platform record | /home/jnhu/sluice-595-e5/evidence/platform.txt |

## 6. R4 archive inventory (top two levels, names only; nothing copied)

```
r4/
  BASELINE.md  DESIGN.md  PREREGISTERED.md  RESULTS.md
  analysis/    : analysis_run.log, extension/, extract_tail_table.py,
                 figure_rebuild.log, MAIN_SUMMARY.json, per_cell_table.md,
                 REBUILD_EVIDENCE.md, tail-latency-p50-p95-p99.csv
  build/       : .build_cache, .deps, .gens, linux, .objs   (frozen binaries; do not rebuild)
  evidence/    : AA.md, BASELINE-FREEZE.md, B-SUPPLEMENT-APPENDIX.md,
                 correctness/, EVIDENCE-INDEX.md, ext4-tmp/, frozen-binaries/,
                 pasteBlock.txt, PREREG_MANIFEST.txt, smoke/
  figures/     : CAPTIONS.md, F1_net_effect.{pdf,png,svg},
                 F2_behavior_change.{pdf,png,svg}, F3_decomposition.{pdf,png,svg},
                 F4_cpu_tail.{pdf,png,svg}, F5_trajectory.{pdf,png,svg},
                 figure-data.csv
  harness/     : analyze_pairs.py, drift_check.py, measure_pairs.py,
                 __pycache__, run_aa.sh
  logs/        : 10-worktree-add.log, 20-apply-r3-completion.log,
                 21-apply-delta-w02.log, 22-apply-delta-w03.log,
                 23-final-gate.log, 24-frozen-commit.log,
                 30-build-smoke-lock-session.log, bsupp-driver.log,
                 build_bench.log, build_config.log, build_default.log,
                 commit1.txt, commit2.txt, correctness/, extension-driver.log,
                 final_rebuild_smoke.log, lock-exclusivity.txt, lock-fuser.txt,
                 LOCK_RECEIPT.md, matrix-driver.{log,out,pid},
                 r4_arm_smoke.log, r4_diag_lines_sample.log,
                 run_build_smoke.sh, uring_ready_batch_deterministic_test.log,
                 w02_smoke_R.log, w02_smoke_S.log, w03_smoke_B.log,
                 w03_smoke_Q2.log, w03_smoke_Q8.log, w03_smoke_R.log,
                 w03_smoke_S_64k.log, w03_smoke_S_d2serial.log,
                 w03_smoke_S_diag.log, w03_smoke_SHADOW.log, w03_smoke_SNI.log,
                 w03_smoke_SNP.log, w03_smoke_S_stats.log, w03_smoke_S.log,
                 xmake_test_all.log
  raw/         : aa/, bsupp/, diagnostics/, extension/, matrix/
  scripts/     : bsupp_descriptive.py, b_supplement_driver.py,
                 extension_analysis.py, extension_driver.py, make_figures.py,
                 matrix_analysis.py, __pycache__, render_results.py,
                 render_status_table.py, tp_thread_pass.py
  worktree/    : research-595-r4 @ f3b25329 (READ-ONLY)
```

Missing from local r4 (fetched from branch `research/595-r4-state-aware`
instead): `TAIL-LATENCY.md` → copied to `r5/r4-archive/TAIL-LATENCY.md`.
Also on that branch only: `README.md` (not requested). Note `PREREGISTERED.md`
sha256 = `fbb2e7852e87f9fbe4bf0e1e12e110d9d2ad279f9d486141b16d61b79f01f0f6`,
matching the value RESULTS.md:4 declares as the gate authority.

## 7. R4 numbers digest — line-by-line verification against the archive

Convention of the digest: W03 d16 warm tmpfs 4KiB, candidate vs A0,
`100*(cand/ref-1)`; CPU/latency negative = better, throughput positive =
better. Sources: `r4/RESULTS.md` (R), `r5/r4-archive/TAIL-LATENCY.md` (T,
branch-extracted), `r4/analysis/MAIN_SUMMARY.json` (J),
`r4/analysis/extension/EXTENSION_SUMMARY.json` (E).

| # | Digest claim | Archive evidence | Status |
|---|---|---|---|
| 1 | R CPU −19.62% point, one-sided-95 upper −19.11% | R:58-59 (−19.62 / −19.11, both PASS) | confirmed |
| 2 | R throughput point ~+24.5%, one-sided lower bound +23.7% | T:24 (d tput +24.53); R:60 (throughput-drop upper −23.74% ⇒ improvement ≥ +23.74%) | confirmed |
| 3 | R p50 +4.75%, p95 +2.71% | T:24 | confirmed |
| 4 | R p99 point +21.85% / upper +23.58%, 24/24 blocks positive +6.4..+32.6 | R:62 (upper +23.58 FAIL), R:75 (24/24 positive, +6.4%…+32.6%), R:467 (independent recompute mean +21.8515%); T:24 (+21.85, range +6.4..+32.6) | confirmed |
| 5 | R main gate conjunctive FAIL, only p99 exceeds | R:74 (only the p99 condition failed; CPU/tput/power/caliber/RSS/threads passed) | confirmed, with the addition that §10.5 protocol preconditions were NOT_RUN (R:76, R:354), which independently bars any PASS declaration |
| 6 | S p95 upper +34.66%, p99 upper +36.25% FAIL | R:66-67 | confirmed |
| 7 | S−R CPU +8.51% (upper +9.31%), p95 +30.34%, p99 +10.52% (upper +11.63%), tput +8.50% | R:187 (S−R row); tput +8.50% is the one-sided upper (R:152 route A) | confirmed |
| 8 | S worse than R on all four metrics; S≈Q2 (CPU +0.83%), S−Q8 +7.98% | R:183, R:186 (+0.83/+1.29; +7.98/+8.70) | confirmed |
| 9 | S decisions: completion_pressure 29973/29998 = 99.92%, quantum=2 99.97%, mean ready 7.92 vs present 2.00 | R:199, R:204-206 (29,973/29,998; q2 29,989/29,998; ready 7.92 → present 2.00) | confirmed |
| 10 | W03 d4: all 8 protective members for R/Q8 pass; R p95 −20.5% / p99 −12.9% (tail improves) | R:88 (CORE-W03-D4: 11 PASS, 1 FAIL = S p99 only); T:73 (d4 R p95 −20.54, p99 −12.92) | confirmed |
| 11 | W03 d64: R/Q8/S p99 +20~33% FAIL | R:129-132 (points +20.52/+21.17/+25.06, uppers +24.92/+25.65/+32.73 — digest range spans point-min to upper-max) | confirmed |
| 12 | W03 d2serial: all arms all metrics +6.47%..+14.09% FAIL | R:116-127 (12/12 FAIL; extremes R tput +6.47 point, S p99 +14.09 upper) | confirmed |
| 13 | btrfs warm buffered: R CPU −54.5%, p99 −30.4% (better); S p95 +23.13% / p99 +19.45% FAIL | T:95-96 (R p99 −30.43, CPU −54.5; S p95 +23.13, p99 +19.45); R:133-134 (S FAIL members) | confirmed |
| 14 | SNP: only 35% small batches (6630/19172 presentations via outstanding()==0 → quantum2) yet p99 +34.5% ≈ S level, vs Q8 +23.3% | T:50-65 (SNP 6,630 q2 of 19,172 presentations ≈ 35%; SNP p99 +34.48; Q8 +23.33; interpretation "hints fire at the wrong moments") | confirmed |
| 15 | PROT-64K: eligibility all immediate, neutral (R p99 upper +6.8%) | E (PROT-64K-D16 R p99 point +3.35%, upper +6.84%); R:430 (eligibility gate PASS 24/24, S/Q8 all immediate, enters=A0) | confirmed |
| 16 | W02 d1/d2/d16 multiple protective members FAIL; W02-d3 no case (harness gap, not hardware) | R:84-86 (6/6/8 of 12 members FAIL), R:335 (W02-d3 NOT_RUN_WITH_REASON: no frozen W02 d3 case) | confirmed |
| 17 | TRAJECTORY 0/24 INCOMPLETE: SHADOW-T1 shadow_outstanding_sum≡0 structurally zero trips the frozen oracle | R:244-246 (0/24; SHADOW/T1 failed 3/3 attempts per block, 72 attempts.jsonl records) | confirmed |
| 18 | W03-d16 CPU buckets: submitter 38.4-45.7%, io-wq 31.8-36.1%, driver ≤0.5%, unattributed residual 22.0-25.7% (10ms tick); no PMU/IRQ attribution; stripped binary blocks ns entry→enter | R:212-224 (submitter 38.4-45.7, driver ≤0.5, io-wq 31.8-36.1, residual 22.0-25.7); R:439 (no PMU per-thread, perf paranoid=2, no IRQ attribution; stripped binary) | confirmed |
| 19 | SHADOW−R instrumentation cost: CPU +1.02% (upper +1.94%), p95 +1.95%, p99 +1.22% | R:182 (+1.02/+1.94 CPU; p95 +1.95/+3.83; p99 +1.22/+2.18) | confirmed |
| 20 | R−B (vs R3_FIXED_PREFIX suffix singles): CPU +9.04% (positive = B worse ⇒ R better), tput +8.21%, p95 +32.17%, p99 +14.50% | R:188 (points +9.04/+32.17/+14.50; tput upper +9.23), R:394 (R−B CPU +9.04%, tput +8.21%, p95 +32.17%, p99 +14.50%; descriptive, not gated) | confirmed |
| 21 | Resources: RSS/threads subgates PASS; static-audit/strace/valgrind NOT_RUN ⇒ overall NOT_ESTABLISHED | R:161-166 (RSS PASS, threads PASS, overall NOT_ESTABLISHED: audit.txt absent, clean-arm strace absent, valgrind slope absent) | confirmed |

Verdict: **all 21 digest items confirmed against the archive; none corrected,
none unfound.** Two precision notes, not corrections: (a) item 2's "+23.7%"
one-sided lower bound corresponds to RESULTS.md's throughput-drop upper of
−23.74%; (b) item 5's "only p99 exceeds" describes the statistical conditions
of §7.1 — the §10.5 protocol preconditions (positive control r3d2pos, W03-d16
enter/CQE reconciliation) were additionally NOT_RUN, which independently
blocks a PASS declaration (R:354-355).

## 8. Anomalies and discrepancies

- **A1 — R5 lock path differs from R4's.** This round's hard constraints name
  `/home/jnhu/sluice-595-e5/MEASUREMENT.lock`; R4's frozen protocol used
  `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` (inode 6941075, shared with
  #604; `r4/BASELINE.md:102-103`, `r4/PREREGISTERED.md:594`). The top-level
  file did not exist at session start and my non-blocking probe created it
  empty (size 0). R5 sessions must flock the top-level path per the round
  constraint; whether r3's lock should also be held for cross-round exclusion
  is a policy question for the round owner (recorded, not decided here).
- **A2 — TAIL-LATENCY.md absent from local r4.** Present only on branch
  `research/595-r4-state-aware`; extracted to `r5/r4-archive/`. All other
  four frozen docs exist locally in `r4/`.
- **A3 — issue 595 carries no labels.** `gh issue view 595 --json labels`
  returned `[]`. Fact only; no expected value was asserted.
- No other anchor mismatches: PR 629 head/base, both commit OIDs, diff
  sha256, archived patch (byte-identical), r4 worktree HEAD all match.

## 9. MEASUREMENT.lock LOCK_RECEIPT convention (from R4 evidence)

Primary record: `r4/logs/LOCK_RECEIPT.md` (read in full, 182 lines); frozen
protocol text at `r4/BASELINE.md:100-110` and `r4/PREREGISTERED.md:592-599`
(§14). Convention as practiced:

- **Acquire**: exclusive `flock` on the lock file (R4 used fd 9 style with
  `-w 1800` timeout per PREREG §14; the matrix/extension drivers held it via
  python). Receipt fields per session: UTC acquire timestamp, label,
  `waited_s`, `fuser -v` output on the lock file (shows the holder PID/
  command), a **second-fd exclusivity check** — a separate
  `flock -n <lockfile>` probe that MUST fail (rc != 0 = blocked = OK) — the
  list of commands run under the lock, and a pre-state JSON snapshot
  (cpu_busy_100ms per relevant CPUs {2,12}, cpufreq governor +
  scaling_cur_freq per CPU, `df -h /tmp`, loadavg, tmp_avail_bytes, utc).
- **Release**: UTC release timestamp, `held_since`, note (`normal` or the
  exception name, e.g. one session recorded `exc=KeyError`), post-state JSON
  (same fields), and a post-release probe (`flock -n` immediately
  reacquires: rc=0 out='reacquired' expected).
- **Invariants**: never write content into the lock file (size stays 0);
  ≥15-minute timed sessions are serial and exclusive to the whole machine
  (no parallel builds/compression/archives); environment invalidation
  criteria in PREREG §10.1 (governor change, foreign CPU-bearing processes
  on cpu2/cpu12, loadavg>4, /tmp <1GiB, lock unacquirable within 1800s).
- Example receipts: warmup-block (11 s), clean-matrix (2026-11-06T02:24:20Z →
  03:42:57Z, ~78 min), diag-matrix, bsupp, extension sessions — all in
  `r4/logs/LOCK_RECEIPT.md`.

## 10. Frozen material pointers for later R5 agents

- `AGENTS.md` (repo root): authority order (v2 root → conformance ledger →
  code → tests), mechanism-justification gate questions, testing discipline
  (oracle before implementation; no post-hoc mirror tests), C++ comment
  policy, script-entry criteria (one-shot probes live outside the repo;
  commit evidence, not collectors).
- `docs/explicit-io-contract.md` (v2-r1, adopted): sole normative root; GOV/
  PROD/W/TERM/ARCH/INV/SEM/ERR/REQ/HANDLE/OBS/PROG/THREAD/CANCEL/BACKEND/
  BOUND/VERIFY families; implementation baseline master @ b870a7c0 with
  conformance ledger `docs/roadmap/v2-conformance.md`.
- `docs/performance/methodology.md` (53 lines): Freeze → Baseline → Profile →
  Attribute → Optimize → Decide; paired same-host runs only; profiling
  separate from clean runs; no speculative change without a measured,
  attributed cause; no library-wide claims from workload-local data.
- #595 boundary: §4.1 above.
- R4 frozen docs: `r4/PREREGISTERED.md` (sha256 fbb2e785…, sole gate source),
  `r4/BASELINE.md`, `r4/DESIGN.md`, `r4/RESULTS.md`,
  `r5/r4-archive/TAIL-LATENCY.md`.
