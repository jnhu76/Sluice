# Independent Review B — Performance/Evidence Audit, E1 Pending-Control Productization

- Reviewer: Independent Reviewer B (performance/evidence). Not the evidence author.
- Date: 2026-10-23
- Candidate: `/home/jnhu/sluice-e1-product`, branch `perf/e1-pending-control-zero-scan`, HEAD `92b42cfb`, base `859e4b56` (master)
- Evidence dir: `/home/jnhu/sluice-e1-product/docs/evidence/e1-pending-control-productization-20261023/`
- All numbers below were recomputed by me from raw jsonl/perf/strace files with independently
  written Python (`/tmp/reviewB/recompute.py`, `/tmp/reviewB/compare_verdict.py`,
  `/tmp/reviewB/replay_research.py`). The author's reducer was read before use and its logic
  verified; the published `verdict.json` was never trusted as input to any check except to be
  compared against my own recompute.

## Files and rows audited

| File | Rows | Result |
|---|---|---|
| `product-ab/results/ab.jsonl` | 360 (= 4 cfg x 2 arms x 3 phases x 15 reps) | no malformed JSON, no duplicate rep IDs, no missing reps, AB/BA rotation (odd=AB, even=BA) holds on every row |
| `product-ab/results/w03.jsonl` | 180 (= 3 cfg x 2 arms x 2 phases x 15) | same integrity checks pass; `loop_kind` is `epoll+eventfd` on 180/180 rows |
| `product-ab/results/w03-verify.jsonl` | 10 (5 base + 5 e1) | all `ok=true`, all `no_lost_wake=true` (note duplicated `arm` key, P3-4) |
| `seam-bound/results/seam.jsonl` | 240 (= 4 phases x 15 reps x 2 arms x 2 repeats) | all `ok=true`, `parity_violations=0` on all 240 rows, 2 repeats per arm/rep everywhere |
| `native-p5-probe/p5-native-probe.jsonl` | 4 runs | sum `cancel_attempts` = 80 000, `samples` = 844, `p_positive_samples` = 0 on every run, `p_max` = 0 on every run |
| `product-ab/results/perf-4096-1024-1-{base,e1}.txt`, `strace-...` | raw perf CSV + strace census | recomputed (see F-1, F-2) |
| `e1-production.diff` (705 lines) | — | byte-identical to `git diff 859e4b56..HEAD -- src/ include/ xmake/ tests/` |

## Commands run (all read-only on the candidate tree; scratch work in /tmp/reviewB)

- `python3 /tmp/reviewB/recompute.py` — independent row-integrity + per-rep paired medians/wins.
- `python3 e1_product_verdict_selftest.py` — 16/16 pass (exit 0).
- Reducer overwrite test: ran `e1_product_verdict.py ... --out ./verdict.json` over an existing
  copy — it rewrote the file silently (exit 0); only `--out == one of the input paths` is refused.
- Research replay: my own recompute + `reduce_w02` of the new reducer over
  `docs/research/mw-hw-1/native-linux-e1/evidence/ab.jsonl` (990 rows); full CLI replay to
  `/tmp/reviewB/research-replay-verdict.json` (correctly FAILS identity: research provenance has
  no lib_async hashes; research `verdict.json` untouched, md5 `fef4e7c6...` unchanged).
- Adversarial mutation: set `p_max=0` in both repeats of rep 7, phase `high_p`, arm `e1` of a
  copy of `seam.jsonl`; real CLI run → exit 1, `overall: FAIL`, issue
  `seam: phase=high_p observed P>0 in only 14/15 E1 reps; ... would be fabricated`.
- `perf stat -x ','` calibration on this host (sleep 0.1/0.2/1/3 and a 10.6 s awk loop) to pin
  the perf CSV column semantics used in F-1.
- Greps: `NATIVE_GO`, `P_POSITIVE_NATIVE` across `docs/`; `552|410 779|0.743` across the
  evidence dir and ledger to trace the attribution claim.
- `git diff 859e4b56..HEAD` file-list/stat checks; `git cat-file -t` on both `e1_sha` commits;
  `git diff d59a266b..81e7e865 -- src/ include/ tests/ xmake/` (empty).

## Reducer and selftest assessment

- `paired_ratios` pairs strictly by replication ID within (cfg, phase): `base.keys() & e1.keys()`,
  ratio = e1/base `cpu_ns_per_op`. Not rank-paired. Verified in source and empirically
  (spot check rep 1 of `4096-1024-1`: base 7934.894 / e1 5512.374 = 0.6947 = verdict ratios[0]).
- Every ratio array in `verdict.json` (W-02 x4, W-03 x3, seam x4) matches my independent
  recompute exactly, element by element.
- Rotation, duplicate, missing-rep, ok!=true, unknown-phase/arm rows are all rejected
  (`check_arms_and_cells`); I verified none of these conditions exist in the raw data.
- Selftest 16/16. The pairing discriminator is genuine: base marginals geometric (1.06^i),
  e1 = 0.9 x base cyclically shifted by 5 → every rank-paired ratio exactly 0.9 (rank reducer
  would PASS the 0.95 gate) while the rep-paired median is ~1.204 (must FAIL); the test asserts
  both `not v.ok()` and that the reported median equals the rep-paired value. Case 16 repeats
  the construction for W-03. A regression to rank pairing cannot pass this selftest.
- Seam anti-fabrication gate verified in code (`p_observed_e1 != 15` → reject with "would be
  fabricated"), in the selftest (all-e1-zero case), and adversarially on real data (single rep
  zeroed → CLI exit 1, FAIL). BASE-arm retention proven per rep via `sticky_transitions > 0`
  (15/15 all phases) — and the harness source (`pending_control_seam_ab.cpp` lines 200-205,
  224-232) really does fail the run if `live_control_sqes_for_test() != 0` during hold or if
  `transport_ledger_size_for_test() != queue_depth` (ledger cardinality), matching the README's
  BASE-arm P>0 proof claim.

## Findings

### P1 — fabrication / blocking

None found. Every headline number reproduces from raw data; pairing is by rep ID; the seam P>0
observation is real and enforced; the native probe result is honestly recorded as NOT_MEASURED
with no upgraded claim anywhere.

### P2 — must fix

**F-1. Attribution "instructions" figures are a perf-CSV field misread; the 0.743 ratio is the
run-duration ratio, not an instruction ratio. Propagates into the ledger.**

- Files: `docs/evidence/e1-pending-control-productization-20261023/product-ab/README.md` lines
  60-61; `docs/roadmap/v2-conformance.md` G6 row (line 86: "instructions ratio 0.743") and dated
  section MJG answer 4 (line ~359: "instructions/op ratio 0.743"); also the task brief's claimed
  headline, which quotes the README.
- Raw evidence (`product-ab/results/perf-4096-1024-1-base.txt`, `-e1.txt`), verbatim perf CSV
  (`perf stat -e instructions:u,cycles:u -x,`, runner line 184):
  - base: `1845898997,,instructions:u,552804305,100.00,,` / `668188248,,cycles:u,552804305,100.00,,`
  - e1: `350946663,,instructions:u,410779352,100.00,,` / `234099483,,cycles:u,410779352,100.00,,`
- Proof of misread: field 4 is byte-identical (552804305 / 410779352) across the instructions
  and cycles lines of the same file — two different hardware counters cannot share a count to 9
  digits; a shared time-enabled column can and does. I calibrated `perf stat -x ','` (perf 7.2.9)
  on this host: field 1 is the counter value (awk 2e8-iteration loop: 107 002 333 335
  instructions ≈ 535/iter — sensible; field 4 = 10 633 939 795 ≈ the 10.63 s run), and field 4
  scales with wall time (e.g. 0.5528 s / 0.4108 s here, matching the embedded per-phase wall
  sums 0.5485 s / 0.4069 s).
- Therefore the actual instruction counts are 1 845 898 997 (base) → 350 946 663 (e1), and the
  quoted "instructions 552 804 305 → 410 779 352, ratio 0.743" is the time-enabled column. The
  cycles quote (668 188 248 → 234 099 483, ratio 0.350) IS correctly read from field 1.
- What still stands, independently verified: `utime` 4136.6 → 1598.2 ns/op (from the perf files'
  embedded timed rows), `io_uring_enter` = 60 003 on both arms (strace, delta 0 — both files'
  full census sums are even identical at 60 142/2 errors), and the cycle-count drop. So the
  mechanism story (kernel boundary unchanged; user-space work removed) survives on utime +
  syscall census + cycles — but the "instructions ratio 0.743" sentence is wrong as written and
  must be corrected in README + the two ledger spots (replace with cycle ratio 0.350 and/or
  utime ratio, or re-derive the claim from field 1).
- Additional caution for the fix: the true instruction counts imply ratio 0.190, which is not
  self-consistent with the 0.74 CPU-time ratio (implausible IPC shift base 2.76 vs e1 1.50), so
  the perf instruction capture itself looks unreliable on this run (frequency/scoping artifact);
  do not "fix" the sentence by quoting 0.190 without explaining that. Cycles and rusage are the
  defensible attribution numbers.

### P3 — notes

**P3-2. Known identity-provenance defect confirmed, disclosed, non-load-bearing.** The runner's
final line collapsed `base_sha=... e1_sha=...` onto one `key=value` line; `read_provenance`
partitions on the first `=`, so `verdict.json` shows `"e1_sha": null` and `base_sha` carries the
concatenation. Confirmed in `product-ab/results/provenance.txt` (last sha line) and
`verdict.json`. Disclosed verbatim in `product-ab/README.md` ("Provenance note ..."). The
load-bearing identity checks all hold (independently verified): `lib_async_base_sha256`
76e6104... != `lib_async_e1_sha256` bb6ccd6..., `pipeline_base` != `pipeline_e1`,
`extloop_base` != `extloop_e1`, fixture present; `lib_core` hashes identical across arms as
expected (E1 touches only async). The embedded `e1_sha` value `81e7e865...` is a real commit on
this branch; classification: P3 (cosmetic, disclosed, recoverable, no gate depends on it).

**P3-3. The reducer does not refuse to overwrite an existing `--out` verdict.json.** It only
refuses when `--out` resolves to one of the four input paths (verified empirically: re-running
with `--out` on an existing verdict.json rewrote it silently, exit 0). The refuse-overwrite
property does exist where it was claimed for the campaign runner (`run_e1_product_ab.sh` line
27: `die "$f already exists: refusing to overwrite a finished campaign"`), and the README's
careful wording ("refuses to write over any input path") is accurate — but a reviewer replaying
the reducer with `--out` pointed at a published verdict.json would clobber it. Low severity:
verdict.json is git-tracked and reducer output is deterministic for given inputs. Recommend an
existence guard on `--out` in a follow-up.

**P3-4. `w03-verify.jsonl` rows emit the `"arm"` key twice** (e.g. `{"arm":"base","arm":"base",...}`).
Both copies are identical, JSON last-wins parsing is deterministic, and the reducer's per-arm
counting is unaffected (verified 5+5). Cosmetic harness-emission defect.

**P3-5. Cross-campaign commit drift (verified benign).** The seam campaign ran at
`e1_sha=d59a266b` (16:52 UTC) and product-ab at `e1_sha=81e7e865` (17:00 UTC).
`git diff d59a266b..81e7e865 -- src/ include/ tests/ xmake/` is empty — only evidence-docs
commits in between — so both campaigns measured identical production code. The single production
code commit is `f62adb91` ("perf(uring): skip zero-pending control router scans via derived
count"); every other commit in `859e4b56..HEAD` touches only `docs/`.

**P3-6. R8 review files listed in the evidence README do not exist yet**
(`review-A-concurrency.md`, `review-B-performance.md` — `ls` finds nothing). Expected to land
with the review process (this document is the performance one); the index row should not be
merged as "done" before they exist.

## Claim-by-claim recompute table

| Claim | My recomputed value (from raw data) | Match |
|---|---|---|
| ab.jsonl 360 rows / w03 180 / w03-verify 10 / seam 240 | 360 / 180 / 10 / 240, all integrity checks pass | YES |
| W-02 4096-1024-1 median 0.6980, 15/15 wins | 0.6980, 15/15 (pairs 15) | YES |
| W-02 4096-1024-8 median 0.6196, 15/15 wins | 0.6196, 15/15 | YES |
| W-02 negative controls 0.9758 / 1.0004 | 4096-16-1: 0.9758 (12/15 wins); 1048576-64-8: 1.0004 (6/15) — both ≤ 1.03 gate | YES |
| W-03 medians 0.7990 / 0.6085 / 0.9202 | 0.7990 / 0.6085 / 0.9202 (15 pairs each, all ≤1.03) | YES |
| W-03 loop kind epoll+eventfd | 180/180 rows `epoll+eventfd`; reducer rejects anything else (selftest case) | YES |
| W-03 verify 5+5 no_lost_wake rows | 5 base + 5 e1, all `no_lost_wake=true`, `ok=true` | YES |
| Seam medians 0.9382 / 0.9596 / 0.9568 / 0.9613 (gate ≤1.05) | 0.9382 / 0.9596 / 0.9568 / 0.9613, all ≤ 1.05 | YES |
| Seam p_max>0 in 15/15 E1 reps, all phases | 15/15 in all four phases; reducer FAILS if any rep lacks it (adversarially verified: 14/15 → exit 1 "would be fabricated") | YES |
| BASE-arm P>0 via live_control_sqes==0 + ledger cardinality == queue_depth | checks present and fail-closed in harness source (lines 200-205, 224-232); base sticky_transitions>0 in 15/15 reps all phases | YES |
| Native probe: 4 x 20000 attempts, 844 samples, zero P>0, NOT_MEASURED | 80 000 attempts, 844 samples, p_positive_samples=0 and p_max=0 on all 4 runs; `P_POSITIVE_NATIVE: NOT_MEASURED` hardcoded in reducer output; all 4 doc occurrences say NOT_MEASURED; zero NATIVE_GO in productization evidence (only in frozen research round, where it names a verdict the exit rule excludes) | YES |
| Instructions 552 804 305 → 410 779 352, ratio 0.743 | misread perf CSV column: those are time-enabled (ns) values, shared by both counters; true instruction counts 1 845 898 997 → 350 946 663; 0.743 is the run-duration ratio | NO (F-1) |
| io_uring_enter 60003 both arms, delta 0 | 60 003 / 60 003, delta 0 (strace censuses; total syscalls even identical 60 142) | YES |
| utime 4136 → 1598 ns/op | 4136.6 → 1598.2 (timed rows embedded in perf files) | YES |
| Cycles 668 188 248 → 234 099 483 | 668 188 248 → 234 099 483 (ratio 0.350) | YES |
| Selftest 16/16, catches rank-pairing regression | 16/16 exit 0; cyclic-shift discriminator flips gate between pairings (rank 0.9 PASS vs rep ~1.204 FAIL) and asserts exact rep-paired median | YES |
| Research replay reproduces 0.6930 / 0.6239 | my recompute and `reduce_w02` on the 990-row research ab.jsonl: 0.6930 / 0.6239 (rank-paired would be 0.6899 / 0.6213 — published values are genuinely per-rep); max other config 0.9962 as README states | YES |
| Identity: distinct lib/binary/fixture hashes | lib_async, pipeline, extloop all distinct per arm; fixture present; e1_sha null defect disclosed (P3-2) | YES (with P3-2) |
| Scope: no public API / root-spec change | `e1-production.diff` byte-identical to `git diff 859e4b56..HEAD -- src/ include/ xmake/ tests/`; only header touched is `include/sluice/async/detail/uring_backend.hpp` (detail header: test-snapshot seam + private counter); `docs/explicit-io-contract.md` unchanged (0 files) | YES |
| Ledger: G6 row + dated section, MJG + authority-contraction audit, no native overclaim | G6 row VERIFIED with scope-limited admission and native NOT_MEASURED; dated section has 8 MJG answers + substantive authority-contraction audit; overclaim absent — except the 0.743 propagation (F-1) | YES (with F-1) |

## Gate summary

- W-02 headline (0.95 / 12 wins): 0.6980 x15, 0.6196 x15 — PASS, reproduced.
- W-02 other (1.03): 0.9758, 1.0004 — PASS, reproduced.
- W-03 (1.03, epoll+eventfd, verify rows): 0.7990 / 0.6085 / 0.9202, 5+5 verify — PASS, reproduced.
- Seam (1.05 AND P>0 in 15/15): 0.9382–0.9613 with P>0 in 15/15 E1 reps and BASE retention in
  15/15 — PASS, reproduced; anti-fabrication rejection verified adversarially.
- Native honesty: NOT_MEASURED preserved end to end — no upgrade anywhere.
- Verdict.json `overall: PASS` is consistent with the raw data; every ratio array matches.

The single must-fix (F-1) is an attribution sentence in two documents (README + two ledger
spots); it does not affect any gate arithmetic, any pairing, any identity check, or the honesty
admissions, and the remaining attribution evidence (utime, cycles, syscall census) is correctly
read and supports the same mechanism conclusion.

VERDICT: APPROVE_WITH_CONDITIONS
