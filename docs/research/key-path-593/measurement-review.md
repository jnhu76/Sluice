# Measurement review

Review of the environment, protocol, statistics, PMU boundaries, and the defects
found while assembling and independently reviewing this record. Everything here
is based on archived evidence; this session did not run a benchmark (see §8).
§10 is the disposition table for every independent-review finding.

## 1. Environment verification

Re-observed on the host before the first measurement, not inherited:

| Item | Verified value |
|---|---|
| Host / CPU | E5, Xeon E5-2666 v3 @2.90 GHz, 1 socket, 10 cores, SMT2, 20 logical CPUs, 1 NUMA node |
| Cache | L1d 32 KiB/core, L2 256 KiB/core, L3 25 MiB |
| Kernel | `7.2.11-200.fc44.x86_64` |
| Toolchain | gcc 16.2.1, glibc 2.43, liburing 2.13, xmake 3.1.2 |
| Frequency | `intel_cpufreq` / `schedutil`, 1200–3500 MHz, turbo on; not modified |
| Pinning | `taskset -c 2`; sibling cpu12 **not** isolated |
| Storage | `/tmp` tmpfs; real-fs arm is btrfs/page-cache-resident; **no device I/O** |
| io_uring | enabled; capability probe passed all required opcodes |
| PMU (user) | cycles, instructions, branches, cache, LLC, dTLB available |
| PMU (kernel) | blocked at `perf_event_paranoid = 2`; obtained only via transient `sudo` |

Baseline binary identity was re-verified against the freeze before every arm
class: w02 `e07a1bd2…bf484`, w03 `80992f4b…a3ab`. Variant identities:

| Arm | w02 SHA-256 | w03 SHA-256 |
|---|---|---|
| K1 elision | `d8f02126…01b1` | `9cf2034a…b8a813` |
| K2-a / E18 | `6b68c19b…b6256` | `08445eec…c20514` |
| K3 fusion | `038bc732…141400` | `fca48296…7bdae` |
| K4 defer (discrimination) | `af2b65f0…9c24e8` | `48f4ea46…27acc` |
| K4-b final | `776d1937…cc2690` | `39581b64…e819fa3` |
| Combo | `d09b0f03…649ed` | `02c4c415…53615` |

The K2-a and K4-b builds produced bit-identical binaries across independent
sessions, which is a useful environment-stability check.

## 2. Protocol

| Element | Setting |
|---|---|
| Design | interleaved AB/BA; even pairs baseline-first, odd pairs variant-first |
| Pairs per case | 15 (formal), 3–5 (survey/census) |
| Per run | 2 warmup + 7 measured repetitions, fresh process |
| Isolation | `taskset -c 2`, single-case filter, no other research workload concurrently |
| Stats | `SLUICE_BENCH_STATS=1` (actual outstanding depth observable) |
| Oracle per run | frozen G0 checker: exit code, case/rep set, no NOT_RUN, `max_outstanding ≤ depth`, CAL-oracle self-test |
| Oracle per repetition | 64 MiB byte verification |
| Cost authority | full binary |
| Frequency authority | observe binary (with disclosed overhead) |

Valid runs: K2-a 390/390, K4-b 390/390. The combo has **210/210 total run
logs, of which 60/60 are the increment comparison** (7 pair directories × 30
runs = 210; 2 of those directories are the combo-vs-K4-b increment). The
combo's oracle gate therefore records 7 case sets, 105 valid pairs and 210 valid
runs, not 270. An earlier version of this record wrote "210/210 + 60/60", which
double-counted the increment; it is corrected here and in every other document.

## 3. Statistics

- **CI method is not uniform and must be read per artifact.**
  - K2-a, K4-b and combo `case-summary.tsv` CI columns are **bootstrap 95 %
    CIs**: 10,000 resamples of the 15 per-pair deltas with fixed seed 593,
    computed by `analyze_pairs.py:72-83` (archived, identical in all three
    directories). For example the K2-a d16 CPU CI [−32.63,−31.62] is a
    bootstrap interval; the pair range is [−33.75,−26.46].
  - The K1/K3/K4 discrimination `pair-results.txt` report the **min–max pair
    range** (their reducers do not bootstrap).
  - The `p99` pair ranges are much wider than the CPU ranges; p99 conclusions
    rely on the sign count, not on the point estimate. No formal noise-band
    test is claimed for p99.
- Reported statistic: median of per-pair deltas, with the pair range/count of
  negative pairs. A/A controls are run in the same session for every primary
  form.
- p95 is available: `analyze_pairs.py` includes `p95_ns` in
  `pair-results.json` (and the raw JSONL carries `lat_p95_ns`). The printed
  summary and `case-summary.tsv` omit p95; this record reports it in the lane
  documents and in §5.
- **Materiality band.** The campaign uses 4.79 % as its materiality band. Its
  provenance and limits are in §4.
- Derived analyses (cycles/op arithmetic, p95 medians, enters/op ratios) are
  labelled as derived and separated from primary measurements.
- Percentages from different arms are never summed.

## 4. The 4.79 % band — provenance (correction)

The record previously called 4.79 % "pre-registered". That label is
**retracted**; it cannot be verified from the archive.

Verified facts:

| Fact | Evidence |
|---|---|
| `brief.md` defines no materiality threshold; `grep -c '4.79' brief.md` = 0 | run this session |
| The first archived occurrence of 4.79 is a **measured A/A drift**: `W03-uring-4k-d4` max drift 4.79 % (`wall_us` row), in the baseline round | `evidence/baseline/baseline-summary.md:126` (written 2026-10-25 22:52) |
| The K1 arm ran later (23:58–00:11) | file mtimes and `evidence/discrimination/k1/README.md` |
| The first archived text calling it "pre-registered" is the K1 README itself (00:11), i.e. **after** the arm | `evidence/discrimination/k1/README.md:101,125,138-143` |
| The W-03-d16 primary's own session A/A max is 1.88 % (baseline round 3.72 %) | `ab/…/case-summary.tsv`, `evidence/baseline/baseline-summary.md` |

So: the **value** existed before the arm as a measured worst-case baseline
drift, and the same band was applied consistently to every lane, but there is no
artifact showing it was designated as the decision threshold before the
experiments. The band is also ≈2.5× the W-03-d16 session noise. Consequences:

- K1's `−2.29 %` is below the band → `NO_CHANGE_JUSTIFIED`.
- K1's effect is above the W-03-d16 session band and reproducible 15/15 →
  rejecting the campaign band flips the verdict to `INCONCLUSIVE`.
- The combo's `−2.57 %` W-03 effect has the same property.
- The K4-b W-03-d4 `+2.54 %` is below the band and inside the only measured
  W-03-d4 A/A [−6.14,+6.51] → **not** a material regression under the campaign
  rule (corrected; it was previously called a productization blocker).

The K1 and combo verdicts therefore carry an explicit threshold-dependence
statement rather than an unqualified "pre-registered" authority.

## 5. Reported latency (p50/p95/p99)

Brief §3 asks for p50/p95/p99 where sample size allows. The archived reducers
printed p50/p99 only; p95 exists in `pair-results.json` and the raw JSONL. The
lane documents now include the p95 medians. Summary for the primary forms
(median of per-pair medians; negative = lower):

| Arm / case | p50 | p95 | p99 |
|---|---:|---:|---:|
| K2-a W02-d16 | −35.91 % | −46.66 % | −45.14 % |
| K2-a W02-d64 | −42.49 % | −52.12 % | −50.29 % |
| K2-a W03-d16 | +10.55 % | +4.77 % | +15.70 % |
| K2-a W02-d1 | +1297 % | +14.73 % | +16.24 % |
| K4-b W02-d16 | −34.08 % | −39.27 % | −39.97 % |
| K4-b W02-d64 | −41.74 % | −49.94 % | −48.49 % |
| K4-b W02-d4 | −10.58 % | −14.12 % | −17.10 % |
| K4-b W02-64k-d16 | −2.61 % | **+7.50 %** | **+7.46 %** |
| K4-b W03-d16 | +0.13 % | +0.46 % | +0.38 % |
| K4-b W03-d4 | +1.35 % | +2.99 % | −0.89 % |
| Combo W02-d16 | −33.48 % | −39.24 % | −38.99 % |
| Combo W02-d64 | −41.55 % | −49.68 % | −47.99 % |
| Combo W03-d16 | −2.38 % | −1.98 % | −3.95 % |
| Combo vs K4-b W03-d16 | −2.35 % | −1.77 % | −2.00 % |
| K1 W03-d16 | −1.95 % | −1.54 % | −2.23 % |
| K1 W02-d16 | 0.00 % | +0.82 % | +3.07 % |
| K3 W02-d1 | −1.97 % | −1.50 % | −1.43 % |

Saturate forms have `lat_n = 0` and no latency values.

## 6. PMU boundaries

| Boundary | Consequence |
|---|---|
| A/B and census sessions: user-space PMU only (`paranoid = 2`) | The primary K4-b session has no kernel PMU. Its `cycles:u` falls 4.2 % while process CPU falls 28.07 %; the difference is sys/kernel time |
| Kernel attribution inherited from the K2 discrimination session's transient-sudo `perf record` | Worker absolute cycles −33.0 % is a **K2 ungated 0/1 run**, not a K4-b measurement. K4-b's enter frequency is twice K2-a's. It is used as order-of-magnitude mechanism evidence only |
| The two 0/1 `perf record` runs disagree on the worker share | Phase C: main 66.29 % / worker 32.52 %; K2 arm: 58.43 % / 40.35 %. The record quotes 32.5–40.4 %, and `rep_movs` 12.0–14.2 % |
| `context-switches` perf counter counts 0 per-process at paranoid=2 | Substitutes: `/usr/bin/time -v` and `perf sched` (232 switches per 60,000 ops, 0.004/op) |
| Tracepoints blocked as user | Substitute: `strace -c -f` (counts only) and transient-sudo `raw_syscalls` |
| strace distorts timing (10× slowdown) | strace wall time is never a result; only counts are used, and futex counts under strace are explicitly not used |
| Observe binary perturbs cadence | Counters-on costs +3.31–15.35 % CPU/op; park incidence moves from 0 (full) to 0.042/op (observe-on). Park counts are taken from the full binary; the `parks`/`empty passes` rows in the census table are flagged as observe-only in [mechanism-ledger.md](mechanism-ledger.md) §4 |

Privileged use: all `sudo` invocations were single commands recorded in the
brief addenda; `perf_event_paranoid`, `kptr_restrict` and `io_uring_disabled`
were read identical before and after; the governor, `drop_caches` and all
security settings were untouched. Root-run benches used a separate fixture
directory.

## 7. Required investigations: executed vs not

| Brief requirement | Status |
|---|---|
| K1 full source lifecycle DAG | done |
| K1 per-completion queue/lock cost | done (census + ledger) |
| K1 normal read vs cancellation/reuse comparison | **NOT executed** — source-level table only; all census arms `control_cqe = 0` |
| K1 two competing explanations + distinguishing counterfactual | done (H1–H4, merged arm, W-02 vs W-03) |
| K1 test-only fusion variant and end-to-end effect | done (with the owed-scan lock-hold limitation, §9.9) |
| K2 submission state ledger | done |
| K2 rejection/capacity/rollback | source + mutation receipts; **no load-level capacity/rollback arm** |
| K2 concurrent submission/cancel comparison | **NOT executed** — no load arm; poison-window mutation only |
| K2 lock/state cost across D/C | per-op lock table across D; C>D only a diagnostic capacity point (no cost) |
| K3 passes/productive/empty/wakes/retries/syscalls/CPU per op | done, with the return-value caveat on "productive" ([k3](k3-progress-wakeup.md) §3) |
| K3 all eight state distinctions | busy/no-work/idle/saturation/W-03 measured; cancel-pending and shutdown **not load-measured** |
| K4 per-D prepared/submitted SQEs, CQEs/reap, locks/CQE, CPU/op | done; p95 added; latency for saturate not instrumented |
| K4 batch-size wait/cancel trade-off | **NOT measured** (no payload/batch sweep; no cancel load) |
| §9 Step 3 regression checks in other standard forms | K4-b: 13 forms incl. 64 KiB, saturate, threadpool, W-03, A/A. K1: W-02/W-03 d16 only. K3: d1/d4 only. Combo: W-02 d16/d64, W-03 d16 only. Disclosed in each lane |

## 8. What was verified in this session

Executed here (documentation/PR assembly and this review response):

- read the archived `case-summary.tsv`, `pair-results.txt/json`, `oracle-gate.txt`,
  `identity.txt`, `mutation-receipts-verified.txt`, `mutants/status.txt`,
  `merge-provenance.txt`, census summaries and mutation logs;
- compared the three census summaries row-by-row: found the draft's K2-a W-03
  enters/op error and the stale banners (§9);
- recomputed the `k`/`u` cycles/op arithmetic from the raw logs:
  `(604,791,795 − 107,235,627)/60,000 = 8,292.6` kernel and
  `(171,478,269 − 17,133,918)/60,000 = 2,572.4` user cycles/op, total 10,865.0,
  kernel share 76.3 %, `3.4 % × 10,865 = 369` cycles/op;
- verified the K1 trace invariant with
  `awk '$1=="TRACE"{n++; if($6!=$3) bad++} END{print n, bad+0}'` on the W-02
  trace: 70,061 lines, 0 mismatches; confirmed there is no W-03 trace file;
- reproduced all CPU bootstrap CIs with seed 593 (`analyze_pairs.py`) and
  recomputed the CPU negative-pair counts for every arm;
- computed p95 medians from the raw per-rep JSONL for all reported forms;
- verified the reviewer's micro-probe finding: `batched run1` has no cycles
  event; core-cycle differentials are 1,723 (run 2) and 1,862 (run 3), median
  1,792.5; 1,815 is the run-3 TSC differential; the N=64 TSC run gives
  f = 0.910 but the core-cycle fraction is ≈0.37;
- verified the 4.79 provenance (`grep -c '4.79' brief.md` = 0;
  `baseline-summary.md:126`; K1 README first "pre-registered" text after the
  arm);
- verified the combo run count: 7 pair dirs × 30 = 210, of which 2 dirs = 60
  are the increment;
- verified the worker-share discrepancy (66.29/32.52 vs 58.43/40.35) and the
  `rep_movs` range (12.03 vs 14.21);
- verified the cancel path: `issue_running_cancel_locked_` only preps and
  ledgers the SQE (`uring_transport.cpp:620-693`); its only `submit_transport_locked`
  is the SQ-full branch (`:647`); `cancel_key` only signals (`:601-615`);
- read the K1 patch and verified the O(capacity) owed scan under one lock
  (`bench/research/key-path-593/patches/k1-elision.patch:201-216`), the
  owed-before-publish reorder, the new `publish_batch_`/`owed_batch_`/
  `observer_armed` state, and the `DeliveryRecord` re-init that does not clear
  `observer_armed` (`uring_backend.cpp:497-499`);
- verified `register_observer` returns `already_terminal` after publication
  (`request_core.cpp:508-511`), which is the real stability argument for the
  observer decision;
- computed SHA-256 for every archived summary and patch listed in
  [evidence-index.md](evidence-index.md); verified the archive checksum
  manifests with `sha256sum -c`;
- generated `combo-k4b-k1ab.diff` from the surviving merge worktree (1,103
  lines) and hashed it;
- verified the source anchors cited in the documents by reading the base tree
  in the PR worktree;
- verified the worktree inventory: all arm worktrees detached at `bdfd0d8d`;
  the PR worktree is on branch `research/593-key-path` (clean after the amend).

Not executed here: builds, tests, benchmarks, perf, strace, mutation runs, or a
rerun of the reducers over all 6,870 files. No *new* performance number was
produced in this session; p95 and negative-pair values are reductions of
archived raw JSONL, labelled as derived.

## 9. Defects found, corrections, and disclosures

1. **Ledger header ambiguity (`k`/`u`).** An earlier draft read the
   `k-cycles/op` / `u-cycles/op` columns as thousands. They mean kernel/user.
   Recomputed from raw logs (values in §8). No 1000× inconsistency; headers
   clarified in [mechanism-ledger.md](mechanism-ledger.md) §8.
2. **Draft synthesis, K2-a W-03 enters/op was wrong.** The draft listed
   1.00007 for "all W-03". The archived K2-a census shows 0.06257 (d16),
   0.25015 (d4), 0.01572 (d64). K2-a does defer in the split-owner form;
   K4-b's topology gate does not. Corrected in
   [cross-path-interactions.md](cross-path-interactions.md) §1 and
   [k2-submit-dispatch.md](k2-submit-dispatch.md) §9.1.
3. **Census banner labels.** `ab/K4-b/census/census-summary.txt` and the combo
   census summary both begin with `# K2-a census medians`. The values match
   each arm's own README (e.g. K4-b d16 `enters/op 0.12577`, vctx 5,098); this
   is a copy-pasted banner, not cross-contaminated data. The K2-a file's banner
   is correct.
4. **Pair-runner banner labels.** `pair-run-k4b.sh` says "K2-a native paired
   experiment" and "K2-a paired run"; it also reads the variant worktree HEAD
   from `worktrees/K2-a` in its `git_var=` line even though `var_bin` points at
   `worktrees/K4-b`. Both worktrees are at the same base SHA, so the recorded
   value is correct; the path is not. The arm *binary* paths and hashes are
   K4-b's.
5. **K4-b d1 wording.** The K4-b README calls d1's −2.50 % CPU/wall "within the
   ±1.2 % null band", but its own CI is [−3.54,−1.45], outside that band and
   excluding zero. The gate cannot produce a mechanism gain at d1 (it still
   reads `outstanding_summary()` and cannot defer). d1 is treated here as a
   **small unexplained favourable deviation (layout/clock)**, and the
   discrimination round's d1 under the same gate is +1.88 %, so the sign is not
   stable across sessions. The conclusion "d1 has no enter reduction and is not
   a deferral regression" stands.
6. **W-03 d4 +2.54 %.** It is below the 4.79 % band and inside the only
   measured W-03-d4 A/A [−6.14,+6.51] (`evidence/discrimination/k4/README.md`
   §4, `pair-results.txt`). It was previously called a productization blocker
   because its bootstrap CI excludes zero; under the campaign rule that is
   wrong. It is now recorded as an unattributed sub-threshold deviation, and
   the absence of a same-session W-03-d4 A/A is disclosed.
7. **Saturate latency.** `lat_n = 0`; the compact `case-summary.tsv` and
   `pair-results.txt` encode p50/p99 as `+0.00 %` / `[+0.00,+0.00]` / `0/15`
   because the reducer's `pct_delta` returns 0 when both sides are zero. This is
   a **data-integrity trap**: a reader of the TSV/JSON must not read it as a
   measured zero change. This record reports "not instrumented".
8. **K2-a final round vs discrimination round.** The final A/B (390 valid runs)
   and the earlier 5-case round differ slightly in magnitude (e.g. d16 −32.20 %
   vs −32.58 %, d1 +14.12 % vs +16.14 %, W-03 d16 −11.17 % vs −11.83 %). Same
   arm, different sessions; the final round is authoritative and the earlier
   round is reproducibility evidence (same direction and magnitude).
9. **K1 owed-scan lock hold and reorder.** Verified from the patch; see
   [k1-completion-publication.md](k1-completion-publication.md) §6. The arm
   holds `dispatch_mtx_` across an O(`request_capacity`) scan when
   `owed_count_ != 0`, and collects owed events before publishing. Neither was
   exercised by the measured loads (all `owed_count_ == 0`). This is a
   limitation of the arm, and it is one more reason the K1 result is not a
   candidate.
10. **`observer_armed` invariant.** The arm's comment ("stable while
    `event_owed` pins the slot") is imprecise: `event_owed` is zero-op-only, and
    the real argument is that `published` is the linearization point and
    `register_observer` cannot arm after it. The field is also not cleared on
    slot re-initialization; it is masked but is stale derived state. Documented
    as a maintenance hazard.
11. **Baseline worktree state.** The frozen baseline worktree carries an
    uncommitted C>D diagnostic patch (`bench/bench_w02_pipeline.cpp`). One build
    verification in the K2-a session relinked the authority binary from that
    patched source; the authority binary was rebuilt from clean source and the
    hash `e07a1bd2…` reproduced exactly, and the diagnostic patch was
    re-applied. The authority hash is intact and every arm re-verified it.
12. **`evidence/ab/K2-a/base-suite`** contains 13 logs from the first suite
    pass; `base-suite-end` contains 7 logs plus the final status. Both were
    read; the final status is used. The K2-a README's "before and after" wording
    is imprecise about the count (13 vs 7), not about the result.
13. **Reducers.** The K2-a/K4-b/combo reducer is committed
    (`harness/analyze_pairs.py`, sha256 `684baae9…`). K1 and K3 each have a
    different external reducer (`evidence/discrimination/k1/analyze_pairs.py`,
    `k3/analyze_pairs.py`), now copied into the archive. **K4 has no archived
    reducer**: its `pair-results.txt` format cannot be regenerated from the
    committed scripts. The earlier claim "the scripts are exactly the ones that
    produced the archived results" is retracted for K1/K3/K4.
14. **Harness protocol claim.** Only `pair-run-k2a.sh`, `pair-run-k4b.sh` and
    `pair-run-combo*.sh` implement `timeout 300` and quarantine invalid runs in
    `invalid/`. The K1/K2/K3/K4 discrimination runners have neither; they echo
    `CHECK_FAILED`. All their `.check` files are OK (K1 120/120, K2
    discrimination 180/180, K3 120/120, K4 240/240), and every workload run
    passed the G0 checker, so no invalid data is included — but the protocol
    claim is not true of every runner.
15. **Evidence-index counts.** The per-directory table is **top-level** file
    counts; the recursive total is 6,870 files / 1,422,946,592 bytes (verified
    with `find`/`du -sb`). The 55 pair directories (390 JSONL per final arm)
    are not counted in the per-directory rows. The worktree inventory now
    records that the arm worktrees are detached at `bdfd0d8d` and the PR
    worktree is on `research/593-key-path`.
16. **Raw-data hashing.** The SHA-256 manifest covers summaries, patches,
    identities and census summaries — **not** the per-rep JSONL or perf
    `.data`. The earlier claim that "paths and SHA-256 are recorded" for the
    raw evidence is retracted: only volume/count and the summary hashes are
    anchored.

## 10. Independent-review disposition

Every finding from the independent review is listed with its disposition. "Fixed"
means the document now states the verified fact; "disclosed" means the record
carries it as a limitation because it cannot be resolved from the archive.

| # | Finding | Disposition | Where |
|---|---|---|---|
| 1 | K4-b "D≥2" over-claim; D=2 has no enter gain under 2/D and was unmeasured | **Fixed** — scope is D=4/16/64; D=2/D=3 unmeasured; gate validation added as a precondition | [k4](k4-sqe-cqe-batching.md) §7/§11, [decision](decision.md) §4 |
| 2 | K2 `ELISION_PERF_SUPPORTED` attached to ungated K2-a; double-count | **Fixed** — K2 verdict is `ELISION_CANDIDATE_FOUND`; one mechanism, `ELISION_PERF_SUPPORTED` recorded once under K4 | [k2](k2-submit-dispatch.md) §11, [decision](decision.md) §1 |
| 3 | Cancel flush misattributed to `issue_running_cancel_locked_` | **Fixed** — verified: only pass-start/SQ-full flush; cancel load unmeasured | [k4](k4-sqe-cqe-batching.md) §6, [execution-dag](execution-dag.md) |
| 4 | K1-a O(capacity) owed scan under one lock | **Fixed** — verified in the patch; lock-hold and unmeasured owed load disclosed; it disqualifies the arm as a candidate | [k1](k1-completion-publication.md) §6/§8 |
| 5 | K1 mutation receipts insufficient; "twelve" count wrong | **Fixed** — exact inventory; K1 has no dedicated mutant; K1 lost-publication/`publish_batch_`/false-negative unmutated | [correctness](correctness-review.md) §3, [k1](k1-completion-publication.md) §10 |
| 6 | PR status contradiction | **Fixed** — branch is prepared; PR not opened in this run; `PR_STATUS: OPEN_NOT_MERGED` is the required field value and is explained | [README](README.md) §9, [decision](decision.md) §7 |
| 7 | K4-b d1 / threadpool / W-03 d4 attribution inconsistencies | **Fixed** — d1 −2.50 CI outside ±1.2 and unexplained; threadpool +1.19 is a null control (diff does not touch ThreadPoolBackend); W-03 d4 sub-threshold and inside A/A | [ranking](opportunity-ranking.md) D7, [measurement](measurement-review.md) §9 |
| 8 | 4.79 "pre-registered" unverifiable | **Fixed by disclosure** — label retracted; provenance (baseline A/A max, pre-arm value, post-arm label), band sensitivity and verdict flip documented | §4 above, [k1](k1-completion-publication.md) §8 |
| 9 | Micro 1,815 cycles/f≈0.91 not reproducible | **Fixed** — counter-resolved table; core-cycle median 1,792.5, TSC 1,814; core fixed fraction ≈0.37; 1,815/f≈0.91 retracted | [k2](k2-submit-dispatch.md) §9.4 |
| 10 | CI method misstated | **Fixed** — bootstrap 95 % (seed 593) for K2-a/K4-b/combo; range for K1/K3/K4 | §3 above |
| 11 | Combo run count double-counted | **Fixed** — 210/210 total, 60 of them increment, 105 valid pairs | §2 above and all references |
| 12 | W-03 d4 +2.54 treated as blocker though sub-threshold and inside A/A | **Fixed** — reclassified as unattributed sub-threshold deviation; same-session A/A gap disclosed | §9.6 above, [k4](k4-sqe-cqe-batching.md) §9 |
| 13 | d1 ±1.2 % claim false | **Fixed by disclosure** — evidence README remains as archived; the record treats d1 as unexplained | §9.5 above |
| 14 | K1 "hard-capped ≈2.3 %" circular | **Fixed** — independent bound is the all-mutex 3.4 % share; 3.1×22.8 ns is retracted as a cap | [k1](k1-completion-publication.md) §7 |
| 15 | K4-b kernel amortization inherited, not measured on K4-b | **Fixed** — labelled inherited from the K2 ungated 0/1 record; K4-b's own evidence is the CPU/`cycles:u` divergence | [k4](k4-sqe-cqe-batching.md) §8 |
| 16 | Worker share 32.5 % vs 40.35 % | **Fixed** — quoted as 32.5–40.4 %; `rep_movs` 12.0–14.2 % | §6 above, [ledger](mechanism-ledger.md) §2 |
| 17 | K2 −33.0 % from 0/1 records, not paired | **Fixed** — labelled 0/1 in §9.3, not only in limitations | [k2](k2-submit-dispatch.md) §9.3 |
| 18 | 70,061 passes is W-02 only | **Fixed** — W-02 qualifier everywhere; W-03 aggregate parity only | [k1](k1-completion-publication.md) §3, [README](README.md) §4 |
| 19 | K1/K3/combo regression coverage narrower than Step 3 | **Fixed** — per-lane coverage statements; "non-regressing" limited to measured forms; K1 W-02 p99 +3.07 % no longer called non-regressing | [ranking](opportunity-ranking.md) D7, [k1](k1-completion-publication.md) §7 |
| 20 | Saturate +0.00 % trap | **Fixed by disclosure** — the TSV/JSON zero is a reducer artifact | §9.7 above |
| 21 | K4 reducer absent | **Fixed by disclosure** — K1/K3 reducers copied; K4 reducer missing; claim retracted | §9.13 above, [bench README](../../../bench/research/key-path-593/README.md) |
| 22 | Harness protocol overstated | **Fixed by disclosure** — timeout/quarantine only in the three A/B runners; check counts given | §9.14 above |
| 23 | Evidence-index counts/worktree HEAD stale | **Fixed** — top-level vs recursive stated; worktree inventory updated | §9.15 above, [evidence-index](evidence-index.md) |
| 24 | K1-a new state not accounted in D5 | **Fixed** — state table + mechanism-justification classification; D5 updated | [k1](k1-completion-publication.md) §6, [ranking](opportunity-ranking.md) D5 |
| 25 | K4-b added work missing the residue signal | **Fixed** — per-deferred-submit `signal_ready_progress()` listed in added work | [counterfactuals](counterfactuals.md) §4, [k4](k4-sqe-cqe-batching.md) §6 |
| 26 | "Productive passes" definition violates brief §6 | **Fixed by disclosure** — renamed/caveated as `completed > 0`, a return-value criterion | [ledger](mechanism-ledger.md) §4, [k3](k3-progress-wakeup.md) §3 |
| 27 | K4-a upper bound does not cover `cqe_seen` | **Fixed** — the mutex bound is partial; the store part is unbounded by measurement | [k4](k4-sqe-cqe-batching.md) §4 |
| 28 | BACKEND-03 deferral-shaped receipts missing | **Fixed by disclosure** — no partial-submit/overflow/shutdown receipt with `ledger.size()>1`; productization precondition | [correctness](correctness-review.md) §5, [k4](k4-sqe-cqe-batching.md) §10 |
| 29 | Early reuse/stale generation N/A not a receipt | **Fixed by disclosure** — no oracle for the accepted+claimed-but-unflushed window | [correctness](correctness-review.md) §5 |
| 30 | K1-b false-negative mutant missing | **Fixed by disclosure** — no mutant flips `observer_armed` or skips a queued claim | [correctness](correctness-review.md) §3 |
| 31 | Raw evidence not hashed | **Fixed by disclosure** — manifest covers summaries/patches only; `EVIDENCE_PRESERVATION: PARTIAL` | §9.16 above, [evidence-index](evidence-index.md) §7 |
| 32 | K1-a publication/owed reorder | **Fixed** — described; PROG-01 permits it; no oracle covers hook-created owed events | [k1](k1-completion-publication.md) §6 |
| 33 | `observer_armed` stability rationale wrong | **Fixed** — published linearization point; stale field noted | [k1](k1-completion-publication.md) §6 |
| 34 | K1 owed scan lock hold | **Fixed** — see #4 | [k1](k1-completion-publication.md) §6 |
| 35 | Missing required investigations (K1 cancel/reuse, K2 concurrent submit/cancel) | **Fixed by disclosure** — unmet items table | §7 above |
| 36 | Missing p95 | **Fixed** — p95 computed from raw JSONL/`pair-results.json` and tabulated | §5 above, all lane docs |
| 37 | Locks/CQE metric missing | **Fixed** — added per-CQE lock count and D-dependence | [ledger](mechanism-ledger.md) §7 |
| 38 | `pair-run-k4b.sh` `git_var` path | **Fixed by disclosure** — same base SHA, wrong path label | §9.4 above |
| 39 | K3 "inside the A/A band" | **Fixed** — median inside, range wider | [k3](k3-progress-wakeup.md) §9 |
| 40 | p99 claims not bounded by null range | **Fixed** — sign counts are the evidence; no formal p99 band test claimed | §3 above, [k2](k2-submit-dispatch.md) §9.2 |
| 41 | `k4_residue_wake_test` is racy (100 ms sleep, no prepark proof, `woken` accepts any progress wake) | **Fixed by disclosure** — verified in the patch; recorded as a non-deterministic oracle that cannot prove the residue signal is the only wake source | [correctness](correctness-review.md) §3/§5, [k4](k4-sqe-cqe-batching.md) §10 |
| 42 | `k4_retry_residue_wake_test` tests inline-flush EAGAIN, not a deferred multi-entry pass-start retry | **Fixed by disclosure** — verified from the patch comment; no deferral-shaped retry test exists | [correctness](correctness-review.md) §3/§5, [k4](k4-sqe-cqe-batching.md) §10 |

Findings that cannot be resolved from the archive are marked **disclosed**:
raw-data off-host integrity and hashing, K4 reducer, cancel/partial-submit
load, D=2/D=3, mixed-driver load, the deferred pass-start retry shape, the
racy missed-wake oracle, and the pre-run designation of the 4.79 % band.

## 11. Reproducibility assessment

| Level | Status |
|---|---|
| Patches | complete and rebuildable from `bdfd0d8d`; K4-b and combo were round-trip verified; combo patch generated here |
| Harness | archived in `bench/research/key-path-593/harness/`; K2-a/K4-b/combo reducer shared; K1/K3 reducers copied; **K4 reducer missing**; hard-coded workspace paths; one-shot provenance, not product tooling |
| Environment | fully recorded, re-observable |
| Raw data | persists on the E5 host only; single copy; not replicated; not in CI artifacts; **not hash-anchored** |
| Statistics | protocol and reducers recorded; bootstrap parameters (10,000, seed 593) documented |
| Identities | baseline and every variant binary SHA-256 recorded |

The weakest links are raw-data persistence and the missing K4 reducer, not the
measurement procedure: another team with access to the host can re-run and
re-reduce; one without it can audit the summaries and hashes only.
