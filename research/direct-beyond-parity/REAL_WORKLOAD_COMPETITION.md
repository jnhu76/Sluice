# Real Workload Competition (P4 Synthesis)

- Mission: `research/direct-beyond-parity/MISSION.md` §6 (P4 — Real Workload
  Performance Competition; layers §6.1/P4.1, workloads §6.2/P4.2, statistics
  §6.3/P4.3, decision rows §6.4/P4.4). Preregistered plan of record:
  `research/direct-beyond-parity/P4_WORKLOAD_COMPETITION_PLAN.md`
  (preregistered 2026-10-28, Amendment 1 pre-measurement). This document is
  the §6 deliverable: one section per workload (admission status and
  evidence, arms and cells, results, §6.4 decision rows, boundaries), then
  the cross-workload conclusions and the MISSION §11 prefill for
  W01/PARQUET/DUCKDB.
- Candidate inputs (frozen, plan §0): P2C1 (read-composition prevalidated
  register-resident steps; KEEP, wall claim −16.565 ns/op CI-excluding at
  `read_exact_at` 4 KiB seq, claim capped "measurable improvement with
  re-attributed residual", tail-mix subclaim restricted — `P2_CANDIDATE_P2C1.md`
  §12.5 `[TRANSCRIBED]`); P2C2 (write-composition exit staging; KEEP —
  instruction caliber only, no wall claim — `P2_CANDIDATE_P2C2.md` §9
  `[TRANSCRIBED]`); BASELINE B1 for all comparisons.
- **Status: MEASURED.** All numbers below were produced by the P4 measurement
  session of 2026-10-28 on host E5 and are transcribed from
  `p4-raw/ROWS-SUMMARY.md` and the per-cell raw files. Writer verification
  scope, stated exactly: every quoted wall median/CI was re-aligned to the
  archived order-aware analysis blocks inside `p4-raw/*.txt` (all 15 W01
  cells and all 6 PARQUET paired cells re-read; none re-derived, none
  altered). Two evidence classes could **not** be aligned to archived
  analysis blocks and were writer-recomputed from the raw arm lines instead:
  (a) the four warm A/A quiesce-rerun sd_AA values — the `w01-aa-*-4k-*.txt`
  files carry the attempt-1 analysis plus the rerun's raw sample lines only
  (the cold A/A does carry its rerun-invariant analysis block,
  `w01-aa-cold.txt:39`); recomputation from the rerun raw lines reproduces
  23.301 / 13.144 / 47.360 / 38.290 exactly; (b) the cursor cell's rusage
  figure — see the post-review correction list below. Reproducibility
  caveat, disclosed: the analysis blocks were produced by an order-aware
  analyzer that lived at `/tmp/p4-run/analyze2.py` (traceback preserved at
  `p4-raw/w01-meta-fi.txt:40`, `p4-raw/w01-meta-oc.txt:40`) and was not
  archived; `p4-scripts/` contains only `p4-smoke.sh`. Plan §8 names
  `python3 $KIT/analyze_pairs.py` as the re-run path, but the archived
  `analyze_pairs.py` is name-paired and cannot reproduce the two-binary
  same-arm blocks. The blocks themselves are archived in the cell files and
  are reproducible from the raw arm lines plus the `paired2.sh` emission
  order (even pairs emit A then B, odd pairs B then A) — this writer
  reproduced the cursor cell's archived wall statistics exactly by that
  method (mean −13.577 / median −15.130; ladderA 713.276 / ladderB 726.853)
  and the four A/A rerun sds, before the corrections below.
- Post-review corrections applied to this document (independent review
  2026-10-28; no verdict changed, no measured value altered — a mirror
  addendum was appended to `p4-raw/ROWS-SUMMARY.md`): (1) cursor row rusage:
  the session summary's "+10.2 ns/op" is untraceable to any archived block
  and unreproducible from the raw lines; replaced by the recomputed paired
  user+sys delta (§1.4). (2) §2.1 smoke figures re-quoted from the archived
  smoke file. (3) Host-quiescence wording reduced to what the pin record
  archives. (4) MDE′ derivation gaps dual-reported. (5) Negative-control PMU
  directions annotated. (6) Strategy-arm oracle statements downgraded to
  session narrative where no oracle output is archived. (7) Gather-arm
  syscall composition asymmetry disclosed. (8) Tail-language scoped to
  claims, not observations. Legend as P0 §0:
  - `[RUN]` — measured or certified in the P4 measurement session; raw
    evidence file cited.
  - `[TRANSCRIBED]` — copied from an archived prior-phase record (P0/P1/P2,
    frozen dududu R1 receipts), source cited.
  - `[UNCERTAIN]` — not pinned by any executed check; stated as such, never
    guessed.

## Execution identity and session conditions

- Identity `[RUN]` (`p4-raw/identity.txt`, re-verified at session open
  `p4-raw/env-pin-2026-10-28.txt:10-28`): all three kit binaries built from
  the one kit source commit `2364be4a` (Amendment 1 common-source clause) —
  `ladder` (B1-linked) `89450367…010e8` (byte-exact the P2 session's
  B1-linked binary), `ladder-c1` (P2C1) `8305d22c…0381`, `ladder-c2` (P2C2)
  `8bb6473b…0065`. Frozen archives: B1 `d1e351a5…3007f0` (worktree HEAD
  `77e5399a`), P2C1 `07d06803…2049e` (HEAD `deaf0301`), P2C2
  `67251028…20a807` (HEAD `476bd9ce`). Warm fixture `/tmp/sluice600/data64m.bin`
  sha256 `bc020fd1…b2993a`; cold btrfs fixtures persist under
  `/var/tmp/sluice600-p1/`. The P3-era kit build (`5a53a9ad…`, source
  `4455d98d`) is preserved as `ladder-4455d98d` and excluded from every P4
  cell.
- Correctness preceded performance `[RUN]`: kit smoke gate 244 checks / 0
  failures (`p4-raw/kit-smoke.txt`, `SMOKE_PASS`; cross-binary checksum and
  `content_ok=1` equality groups across the 23-arm × 3-binary matrix);
  `ctest -L parquet` 3/3 per PARQUET arm build
  (`p4-raw/parquet-ctest-admission.txt`); per-run checksum/`content_ok`/
  replay-oracle gates green on every cell. The one oracle failure during the
  campaign (strategy-arm overlap bug, §2.3 — session narrative; no oracle
  output is archived under p4-raw) voided that arm's numbers until
  fixed and rebuilt; no sample was deleted and no gate weakened.
- Environment drift, disclosed, not absorbed `[RUN]`
  (`p4-raw/env-pin-2026-10-28.txt:3-8`): kernel `7.2.11-200.fc44.x86_64`
  (P4) vs frozen R1 kernel `7.2.9-201.fc44.x86_64` (`[TRANSCRIBED]`
  `build/parquet-results/run-manifest.txt:9`); `scaling_governor=schedutil`
  (post-reboot default; the plan §3.1 pin `performance` was outside this
  session's sudo scope); `perf_event_paranoid=2` (`instructions:u` verified
  usable unprivileged). Disposition per plan §6: fresh A/A floors were taken
  under the actual environment, every claim gate uses fresh MDE′ with the
  one-way downgrade rule (a claim that flips under MDE′ downgrades to
  NO_CLAIM), pinned inherited floors are dual-reported, and **no
  cross-session or cross-kernel comparability is claimed** (P4 PARQUET
  numbers are fresh measurements, never deltas against frozen R1).
- Fresh floors `[RUN]` (`p4-raw/w01-aa-*.txt`, `p4-raw/pq-aa-*.txt`; the one
  §6-item-7 quiesce rerun per A/A cell was applied; rerun values stand):
  read sd_AA B1 23.301 ns / C1 13.144 ns → cross-binary read MDE′ =
  2.131·23.301/√16 = **12.413 ns, recorded 12.414** (max-of-pair,
  conservative); write 47.360/38.290 → write MDE′ **recorded 25.235 ns,
  recomputed 25.231**; cold sd 2242.535 (`w01-aa-cold.txt:39`) → cold MDE′
  **recorded 1194.06 ns, recomputed 1194.71**. Derivation note, disclosed:
  the read and PARQUET thresholds recompute exactly from their archived
  inputs; the recorded write and cold values differ from
  2.131·sd_AA/√16 of the archived sds at the third/fourth significant digit
  (implied sds 47.367 and 2241.31 vs archived 47.360 and 2242.535) — the
  session's derivation inputs for those two digits are not archived. No
  recorded verdict changes under either value (cold-4k median +112.956 and
  w01-wcomp-4k −11.099 are spans-0/sub-MDE under both; w01-comp-read-4k
  −19.048 exceeds both 12.413 and 12.414). The four warm rerun sd_AA values
  themselves recompute exactly from the archived rerun raw lines (writer
  verification, § header). Pinned inherited floors dual-reported: read
  5.535, write 15.695, cold 1416.185 ns `[TRANSCRIBED]` (P1/P2 records).
  PARQUET replay track: native sd 0.4779 ms (MDE′ 0.2545), sluice sd
  1.1287 ms (MDE′ 0.6013); Goal-A PARITY band 1% of N1 median ≈ 2.34 ms.
  Persistent noise, disclosed, never absorbed: all four warm W01 A/A floors
  remain above the 10 ns band after the rerun, and the write track's first
  pairs show first-invocation outliers ≈131–189 ns
  (ABBA-median-neutralized, but they inflate sd_AA).
- Host quiescence, stated to exactly what the pin record archives `[RUN]`
  (`env-pin-2026-10-28.txt:9,46-47`): load 0.06/0.11/0.09 at session open
  and `concurrent_measure_procs_close=0` at close; strictly serial cells,
  `taskset -c 2`. **Gap disclosed:** plan §6 item 2 required the pin record
  to show 0 matching measurement processes *before* a session's cells count;
  no per-segment pre-cell check was archived — the only archived
  concurrent-process observation is the closing one (= 0). The P3 session's
  unresolved `concurrent_measure_procs=3` blemish
  (`STRUCTURAL_IO_MECHANISM_STUDY.md` host-quiescence caveat) is therefore
  not re-committed on the archived evidence, but neither is it positively
  excluded at every segment boundary; wall magnitudes carry this caveat as
  disclosed limitation, and (as in P3) no cell shows an integrity-gate
  failure and all paired CIs are tight.
- Caliber discipline (plan §6 item 5): wall claims come exclusively from
  uninstrumented runs; PMU `instructions:u` appears as corroborating signal
  (cycles:u is noisy under schedutil and is not cited), rusage is coarse
  ±30 ns corroboration; strace -c integral counts are count caliber, never a
  wall source. P99 was not measurable anywhere (see §1.4/§2.4 notes).

## 0. Verdict summary (candidate × workload; local verdicts retained, never merged — plan §7.3 rule 1)

| Workload | Candidate | Cell verdicts (retained individually) | Composite reading |
|---|---|---|---|
| W01 | P2C1 | `WIN_CONFIRMED` ×3 (comp-read, comp-eofmix, comp-cursor); `NO_CLAIM` ×7 (hf-1b, hf-4k, seq-1m, rand-4k, meta-oc, meta-fi, cold-4k); `PARITY` ×2 (neg-1b, neg-4k) | Composition-shape win survives load testing at fresh MDE′; no effect on public ops, metadata, streaming or cold; nothing generalized beyond the composition shape |
| W01 | P2C2 | `NO_CLAIM` ×1 (wcomp-4k, wall; instruction caliber −20 instr/op retained); `PARITY` ×2 (write-4k, negw-4k) | P2 KEEP scope (instruction caliber only) confirmed at workload shape; no wall claim admitted |
| PARQUET | P2C1 | `PARITY` ×1 (pq-neg-p2c1, workload-scale negative control) | Machine-identical public op confirmed at real-workload scale; no candidate delta |
| PARQUET | P2C2 | `PARITY` ×1 (pq-neg-p2c2, supplementary symmetry control) | Same for the write candidate |
| PARQUET | B1 (Goal-A transfer, no candidate) | `NO_CLAIM` ×1 (pq-r1-transfer-b1; preregistered expectation CONFIRMED) | Zero-cost-abstraction question unresolvable at this shape (≪ 1% band) — unrefuted, not proven |
| PARQUET | Layer C strategy arms (AC-1/AC-2 opportunity evidence, not product claims) | `REGRESSION` ×3 (gather-native, gather-sluice, arrowprebuf); `NOT_APPLICABLE_SHAPE` ×1 (pq-cfr); descriptive ×1 (pq-t3-context-b1, no claim) | Both sides' best legal planning/gather implementations pay at this trace; AC-1/AC-2 real-trace opportunity negative, AC-3 has no shape here |
| DUCKDB | — | `BLOCKED` (§3) | Status only; not evidence |
| HF_SMALL_IO | — | `BLOCKED` (§4) | Status only; not evidence |

No cell in this campaign claims, relies on, or adjudicates a
tail-for-throughput trade: every win above is a median/mean wall win with
PHYSICAL_WORK_DELTA = 0, and no verdict rests on a latency-distribution
effect. Tail latency itself was not measured anywhere
(NOT_SUPPORTABLE_WITH_REASON on every row), so no tail claim — favorable or
unfavorable — is made anywhere in P4.

---

## 1. W01 — direct-cost-ladder kit workload (shape-roster coverage)

### 1.1 Admission status and evidence

**ADMITTED** (plan §1.1, identity record revised by Amendment 1) `[RUN]`.
Kit branch `direct-cost-ladder` (local-only, 0 remote refs), HEAD `4455d98d`;
P4 measurement binaries built from kit source commit `2364be4a` (the P2 kit
source; `ladder.cpp` sha256 `75c19bcc…`). Amendment 1 closed the blocking
review finding that the original on-disk binary set was split across kit
sources (`ladder` from `4455d98d`, +852/−31 lines vs `2364be4a`, vs
`ladder-c1`/`ladder-c2` from `2364be4a`), which violated the P2C1 §2.2
common-mode harness-TU discipline for every `paired2.sh` cross-binary cell:
`ladder` was rebuilt from a scratch `git archive 2364be4a` export with the
same `build.sh` flags → `89450367…010e8`, **byte-exact the P2 session's
B1-linked binary**; `ladder-c1`/`ladder-c2` were already the literal
P2-measured `2364be4a` artifacts and are unchanged. Post-fix smoke: 7 read
arms × 2 binaries identical checksum 1310720; write arms `content_ok=1`
(`p4-raw/kit-smoke.txt`, 244/0). Frozen archives and warm fixture re-hashed
to pinned values (`p4-raw/identity.txt`); `paired.sh`/`paired2.sh`/
`analyze_pairs.py` harnesses present and used for every cell.

### 1.2 Comparator layers and cells

- **Layer A (mechanism floor):** kit arms `a0`/`w0`/`m0oc`/`m0fi` (bare
  `pread`/`pwrite`/`open+close`/`fstat`), recorded unpaired as context in the
  cell files (P1 §4 convention). **Layer B (semantic parity):** kit arms
  `a1`/`w1`/`a1x`/`w1x`/`a1c`/`w1c`/`m1oc`/`m1fi` — hand-matched native
  mirrors of the B1 public path. **Layer C (optimized native competitor):**
  by the preregistered structural statement (plan §2), on single-span shapes
  the best legal native execution is the single `pread`/`pwrite` form Layer B
  already uses — **Layer C collapses onto Layer B for every W01 cell**; this
  is recorded as a structural fact, not an omission (P3: M1 Case A needs
  k ≥ 2 caller buffers, M2 planning needs a request set, M3 needs a second
  fd). The Layer C distinction is exercised only in the PARQUET strategy arms
  (§2). The P3-era probe arms (`preadv(av1)`, planner `rplt/rpld`,
  `copy_file_range(cp2)`, `sendfile(cp3)`) were re-verified runnable on
  `ladder-4455d98d` at kernel 7.2.11 during preparation (content_ok=1,
  digests equal) but are diagnostics only and enter no P4 cell.
- Cells (16 ABBA pairs, `taskset -c 2`, warm tmpfs unless stated; full roster
  and rationale plan §3.2): A/A floors `w01-aa-read-4k-b1/-c1`,
  `w01-aa-write-4k-b1/-c2`, `w01-aa-cold`; shape cells `w01-hf-1b` (1 B ×
  4096 ops), `w01-hf-4k` (4 KiB, doubles as P2C1 public-op negative-control
  shape), `w01-seq-1m` (1 MiB × 256, amortization boundary),
  `w01-rand-4k` (seed 42), `w01-write-4k` (4 KiB write, P2C2 negative
  control, Amendment-1 per-cell scratch, `content_ok=1` gate),
  `w01-comp-read-4k` (primary P2C1 benefit unit; per-binary a1x/ax on both
  binaries + cross-binary ax), `w01-comp-eofmix-4k` (tail rotation; the P2
  capped-residual carrier — mandatory while the KEEP claim stays capped),
  `w01-comp-cursor-4k` (cursor twin ac/a1c, optional-but-listed, not waived),
  `w01-meta-oc` (open+close cycles), `w01-meta-fi` (file_info × 16384),
  `w01-cold-4k` (btrfs cold ABBA, drop before every invocation, reps=1);
  mandatory negative controls `w01-neg-1b`, `w01-neg-4k` (a1 vs a2 @cand),
  `w01-negw-4k` (w1 vs w2 @cand).

### 1.3 Results

- **P2C1 primary claim carrier reproduces under load** `[RUN]`
  (`p4-raw/w01-comp-read-4k.txt`, cross-binary series 3 analysis): median
  **−19.048 ns/op**, CI95 [−21.376, −10.723] excludes 0, |median| ≥ fresh
  MDE′ 12.414 (and ≥ the pinned 5.535 under the dual report), sign 2/16
  positive (candidate faster in 14/16), AB −17.976 / BA −20.933 consistent.
  The P2 KEEP evidence class (CI-excluding favorable wall delta at
  `read_exact_at` 4 KiB seq) therefore survives load testing in the P4
  environment; magnitudes are not compared across sessions (drift disclosure
  above). PHYSICAL_WORK_DELTA = 0: `strace -c` integral counts identical
  (`pread64` 7170 = 7170, ×3 exact repeats; `p4-raw/w01-physical-work-strace.txt`),
  bytes equal by construction and by checksum 25165824 uniform across all 32
  paired samples plus a1x/ax context. CPU corroboration: PMU −62 instr/op
  (`p4-raw/w01-pmu-diagnostic.txt`), rusage −14.2 ns/op (coarse ±30 ns
  caliber).
- **Layer A/B context at the same cell** `[RUN]` (per-binary analysis blocks,
  same file): within the B1 binary the native mirror leads sluice `ax` by
  −20.247 ns/op CI [−30.226, −10.267]; within the P2C1 binary that gap
  collapses to −2.976 ns/op CI [−9.488, +2.257] (spans 0). Read as context
  only (harness caliber, P1 §4 convention): the candidate's cross-binary win
  and the within-binary mirror-gap collapse are the same composition shape
  measured two ways.
- **The P2 eofmix cap did not reproduce — direction reversed, recorded as
  falsified-direction** `[RUN]` (`p4-raw/w01-comp-eofmix-4k.txt`): median
  **−15.907 ns/op**, CI [−17.130, −9.639] excludes 0, ≥ MDE′; PMU −69
  instr/op; PHYS 0; checksum 12595200 uniform. P2 §6.3 had recorded this unit
  at +10.228 ns EXCESS, capping the KEEP claim
  ("measurable improvement with re-attributed residual") `[TRANSCRIBED]`.
  Per plan §7.3 rule 4 the reversal is recorded as falsified-direction —
  not as an upgrade of the P2 claim; the P2 cap remains the conservative
  claim bound, and no cross-kernel comparability is claimed (P2 ran under the
  pinned governor/kernel; P4 under schedutil/7.2.11).
- **Cursor twin also wins** `[RUN]` (`p4-raw/w01-comp-cursor-4k.txt`):
  median −15.130 ns/op, CI [−22.305, −4.849] excludes 0; PMU −52 instr/op;
  PHYS 0 (`read` 7172 + `lseek` 6 on both sides); checksum 25165824.
- **No transfer to non-composition shapes** `[RUN]`: hf-1b +4.459 CI
  [−0.553, +10.214] (PMU 0, 908=908 instr/op); hf-4k +1.137 CI [−12.177,
  +11.062] (public-op machine-identical expectation met); seq-1m −175.668
  (0.13% of ~137.5 µs/op) CI [−1504, +3716]; rand-4k −1.805 CI [−6.675,
  +9.157]; meta-oc −0.166 CI [−10.885, +22.561]; meta-fi +1.476 CI [−1.636,
  +4.751] (fstat 83975 = 83975; PMU 0, 275 = 275 — the preregistered
  metadata-PARITY expectation for machine-identical symbols confirmed);
  cold-4k +112.956 CI [−71, +714] ≪ cold MDE′ 1194 (preregistered cold
  NO_CLAIM expectation confirmed; the row bounds the claim). All `NO_CLAIM`.
- **Negative controls gate the wins and pass** `[RUN]`: neg-1b (a1−a2)
  −3.765 CI [−15.926, +6.963]; neg-4k −5.929 CI [−11.305, +10.366]; PHYS
  identical (28674 / 7170 preads). Both PARITY → the P2C1 WIN_CONFIRMED rows
  stand per plan §7.3 rule 3.
- **P2C2 stays instruction-caliber at workload shape** `[RUN]`
  (`p4-raw/w01-wcomp-4k.txt`): median −11.099 ns/op, CI [−15.097, +2.553]
  spans 0, sub-MDE′ 25.235 → wall `NO_CLAIM` (preregistered expectation
  confirmed; consistent with P2C2 §6.2's −8.812 sub-MDE finding
  `[TRANSCRIBED]`); PMU −20 instr/op retained as the only evidence class;
  PHYS 0 (`pwrite64` 22528 = 22528); `content_ok=1` all samples. Negative
  controls: write-4k +3.088 CI [−11.142, +30.309]; negw-4k (w1−w2) −0.709 CI
  [−13.333, +12.991] — both PARITY.
- Session-noise anomalies, disclosed in place: the four warm A/A floors'
  persistent >10 ns sd bands and write-track first-pair outliers are recorded
  above; the cursor strace section was rerun once because a display filter
  dropped the `read` row (CORRECTED-2; raw counts elsewhere unaffected;
  `p4-raw/w01-physical-work-strace.txt:1084,1097`), and a zsh word-splitting
  defect voided an intermediate correction pass (CORRECTED-1, noted in-file).

### 1.4 §6.4 decision rows (MISSION P4.4 format; plan §7.1)

Common to every W01 row: P99_DELTA = NOT_SUPPORTABLE_WITH_REASON (candidate
effects < 10× the ≈26 ns/op per-op sampling overhead, plan §3.2 note);
PHYSICAL_WORK_DELTA = 0 syscalls/op and 0 bytes (strace -c integral counts,
≥3 exact repeats, identical at every measured arm/shape; single
kernel→user copy per op on both sides, structural); no family-wise
correction across cells (disclosed, P1 §5.6 precedent).

| WORKLOAD | BASELINE | CANDIDATE | CPU_DELTA | WALL_DELTA | P99_DELTA | PHYSICAL_WORK_DELTA | CORRECTNESS | UNCERTAINTY | VERDICT |
|---|---|---|---|---|---|---|---|---|---|
| w01-comp-read-4k (shape 5, primary carrier) | B1 `d1e351a5…` / `ladder` `89450367…` | P2C1 `07d06803…` / `ladder-c1` `8305d22c…` | rusage −14.2 ns/op (±30 coarse) + PMU −62 instr/op | median −19.048 ns/op, CI [−21.376, −10.723] excl 0, ≥ MDE′ 12.414 | NOT_SUPPORTABLE_WITH_REASON | 0 syscalls/op (pread64 7170=7170), 0 bytes | checksum 25165824 uniform (32 samples + context); kit smoke 244/0 | sd_AA c1 13.144 / b1 23.301 dual-report; MDE′ 12.414 vs pinned 5.535 (one-way rule: survives both); sign 2/16; AB −17.976 / BA −20.933 | **WIN_CONFIRMED** (negatives PARITY → stands, §7.3 r3) |
| w01-comp-eofmix-4k (shape 5b, tail rotation) | B1 | P2C1 | rusage −11.5 (coarse) + PMU −69 instr/op | median −15.907 ns/op, CI [−17.130, −9.639] excl 0, ≥ MDE′ | NOT_SUPPORTABLE_WITH_REASON | 0 | checksum 12595200 uniform | as row 1; P2 cap +10.228 EXCESS not reproduced — falsified-direction recorded (§7.3 r4); P2 cap remains the claim bound | **WIN_CONFIRMED** |
| w01-comp-cursor-4k (shape 5c, cursor twin) | B1 | P2C1 | rusage user+sys −9.6 mean / −8.9 median ns/op (writer-recomputed from raw arm lines; the session summary's "+10.2" is untraceable and direction-inconsistent — corrected post-review; corroborating caliber only) + PMU −52 instr/op | median −15.130 ns/op, CI [−22.305, −4.849] excl 0, ≥ MDE′ | NOT_SUPPORTABLE_WITH_REASON | 0 (read 7172 + lseek 6 both sides) | checksum 25165824 uniform | as row 1 | **WIN_CONFIRMED** |
| w01-hf-1b (shape 1, 1 B × 4096) | B1 | P2C1 | PMU 0 (908=908 instr/op) | median +4.459 ns/op, CI [−0.553, +10.214] spans 0, sub-MDE | NOT_SUPPORTABLE_WITH_REASON | 0 | checksum 24576 uniform | sd 10.105 | **NO_CLAIM** |
| w01-hf-4k (shape 1b, 4 KiB; negative-control shape) | B1 | P2C1 | PMU 0 (35627=35627) | median +1.137 ns/op, CI [−12.177, +11.062] spans 0 | NOT_SUPPORTABLE_WITH_REASON | 0 | checksum 25165824 uniform | — | **NO_CLAIM** (public-op machine-identical expectation met) |
| w01-seq-1m (shape 2 boundary, 1 MiB) | B1 | P2C1 | PMU 0 | median −175.668 ns/op (0.13%), CI [−1504, +3716] spans 0 | NOT_SUPPORTABLE_WITH_REASON | 0 (pread64 1474=1474) | checksum 1476395008 uniform | — | **NO_CLAIM** (small-cell win does not transfer to streaming; boundary row) |
| w01-rand-4k (shape 3, seed 42) | B1 | P2C1 | PMU −1 instr/op | median −1.805 ns/op, CI [−6.675, +9.157] spans 0 | NOT_SUPPORTABLE_WITH_REASON | 0 | checksum 25165824 uniform | — | **NO_CLAIM** |
| w01-meta-oc (shape 6, open+close cycles) | B1 | P2C1 | PMU 0 (1554=1554) | median −0.166 ns/op, CI [−10.885, +22.561] spans 0 | NOT_SUPPORTABLE_WITH_REASON | 0 (openat 10759=10759, close 10758=10758) | checksum 10752 uniform | — | **NO_CLAIM** |
| w01-meta-fi (shape 6b, file_info × 16384) | B1 | P2C1 | PMU 0 (275=275) | median +1.476 ns/op, CI [−1.636, +4.751] spans 0 | NOT_SUPPORTABLE_WITH_REASON | 0 (fstat 83975=83975) | checksum uniform | — | **NO_CLAIM** (metadata PARITY expectation confirmed) |
| w01-cold-4k (shape 7, btrfs cold, drop/invocation) | B1 | P2C1 | not claimed (cold rusage I/O-wait dominated) | median +112.956 ns/op, CI [−71.237, +714.157] spans 0, ≪ cold MDE′ 1194 | NOT_SUPPORTABLE_WITH_REASON | 0 syscalls (same arm/shape as hf-4k strace) | checksum 67108864 uniform | cold sd_AA 2242.535; pinned floor 1416.185 dual-report | **NO_CLAIM** (cold expectation confirmed; bounds the claim) |
| w01-neg-1b (negative control @c1: a1 vs a2) | n/a | P2C1 | PMU +42 instr/op in the (a2−a1) direction, i.e. −42 in this row's (a1−a2) wall-statistic direction (archived 3,546,323 vs 3,719,369 over 4096 ops; direction annotated post-review; harness caliber) | (a1−a2) median −3.765 ns/op, CI [−15.926, +6.963] spans 0 | NOT_SUPPORTABLE_WITH_REASON | pread counts identical (28674) | checksum 24576 uniform | — | **PARITY** (passes) |
| w01-neg-4k (negative control @c1) | n/a | P2C1 | PMU +43 instr/op in the (a2−a1) direction, i.e. −43 in this row's (a1−a2) direction (archived 36,437,704 vs 36,481,648 over 1024 ops) | (a1−a2) median −5.929 ns/op, CI [−11.305, +10.366] spans 0 | NOT_SUPPORTABLE_WITH_REASON | identical (7170) | checksum 25165824 uniform | — | **PARITY** (passes) |
| w01-wcomp-4k (P2C2 benefit class, wx) | B1 `d1e351a5…` | P2C2 `67251028…` / `ladder-c2` `8bb6473b…` | rusage −2.9 (coarse) + PMU −20 instr/op | median −11.099 ns/op, CI [−15.097, +2.553] spans 0, sub-MDE′ 25.235 | NOT_SUPPORTABLE_WITH_REASON | 0 (pwrite64 22528=22528, pread64 66=66) | content_ok=1 all samples; checksum 25165824 | write sd_AA c2 38.290 / b1 47.360; P2C2 §6.2 −8.812 sub-MDE consistent | **NO_CLAIM** (wall expectation confirmed; instruction caliber remains the only P2C2 evidence class) |
| w01-write-4k (shape 4, negative control) | B1 | P2C2 | PMU 0 (1117760=1117760) | median +3.088 ns/op, CI [−11.142, +30.309] spans 0 | NOT_SUPPORTABLE_WITH_REASON | 0 | content_ok=1 | — | **PARITY** (passes) |
| w01-negw-4k (negative control @c2: w1 vs w2) | n/a | P2C2 | PMU +22 instr/op in the (w2−w1) direction, i.e. −22 in this row's (w1−w2) direction (archived 1,144,564,190 vs 1,144,586,500, whole-window caliber) | (w1−w2) median −0.709 ns/op, CI [−13.333, +12.991] spans 0 | NOT_SUPPORTABLE_WITH_REASON | identical | content_ok=1 | — | **PARITY** (passes) |

Minimum coverage (plan §7.4): benefit-shape cells ✓ (comp-read/eofmix/cursor;
wcomp), negative controls ✓ (neg-1b/neg-4k/negw-4k + hf-4k/write-4k shapes),
boundary rows ✓ (seq-1m, cold-4k), blocked-workload rows ✓ (§3/§4).

### 1.5 Boundaries

1. W01 is kit-shape evidence: a synthetic ladder on deterministic fixtures —
   the W-01 workload the mission names, not an application workload.
   Application-shape claims rest on PARQUET only (plan §9.1).
2. Layer C collapses onto Layer B on every W01 single-span shape (§1.2):
   W01 adjudicates Goal A and candidate load behavior but cannot, by
   structure, show beyond-parity against a better native mechanism.
3. The three WIN_CONFIRMED cells are the composition shape only. By §7.3
   rule 1 the seven NO_CLAIM cells and two PARITY controls stand beside
   them; no cell's best value generalizes to a whole-Sluice conclusion.
4. The eofmix reversal is recorded, not adjudicated into a claim upgrade;
   the P2 cap stays the conservative bound until re-measured under a pinned
   environment (no cross-kernel comparability claimed).
5. Wall magnitudes carry the disclosed environment caveat (schedutil,
   paranoid=2, kernel 7.2.11): every gate used fresh MDE′ under the actual
   environment; nothing is compared numerically against P1/P2 sessions.
6. P2C1+P2C2 composition is forbidden in P4 (plan §0 rule 1); the
   interaction cell belongs to P5. Independent gains are never added.

---

## 2. PARQUET — frozen Reader Trace replay (the only admitted real workload)

### 2.1 Admission status and evidence

**ADMITTED** (plan §1.2, evidence statement corrected by Amendment 1)
`[RUN]`. Frozen Reader Trace `build/parquet-results/trace-medium-t3.jsonl`
(sha256 `a059e58f…`) and its p2 twin (`4c675626…`), hash-equal to the frozen
run-manifest; dataset `medium-none.parquet` re-hashed `6116e69c…` (181,966,794
B) — equal to the trace's self-declared digest, closing the P0 §3.5
file-level uncertainty. Three-arm harness (P0 arrow native / P1 POSIX
adapter / P2 sluice direct; `benchmarks/parquet/protocol.md`) on dududu
branch `r1-parquet-sluice-bench` (dududu PR #3 MERGED); binaries re-hash to
the manifest and the replay path ran in this measurement session
(`p4-raw/parquet-smoke-replay.txt:16-17`: Replay native 235.3 ms / sluice
233.1 ms, replay_bytes=181.996M, replay_requests=57 — same magnitude as the
frozen R1 receipts, native 234.4 / sluice 234.8 ms `[TRANSCRIBED]` plan §1.2;
no band assertion is made beyond that). The 234.6/232.7 ms figures quoted in
plan §1.2 are the readiness session's run, not this measurement session's.
Corrected trace geometry (Amendment 1, direct point-coverage query): 57
requests, 33,277–16,030,016 B, median 1,390,820 B, total requested
181,996,415 B; covered set = **one contiguous segment [4, 181966794)** with
exactly **one 4-byte hole** ([0,4) never requested); overlap 29,625 B
(0.016%); union 181,966,790 + overlap 29,625 = 181,996,415 ✓; the review's
suspected second hole at [1550843,1550847) was disproved (those bytes are
covered by request [4,1550847); the next request starts exactly at 1550847).
An earlier draft's "zero gaps" clause was false and is superseded. Kernel
citation corrected by Amendment 1: frozen receipts read
`KERNEL=7.2.9-201.fc44.x86_64` (`run-manifest.txt:9`; protocol.md:187 agrees)
`[TRANSCRIBED]`; host drift to 7.2.11 is disclosed, and no cross-kernel
comparability is claimed. Historical R1 numbers are quoted only, never
modified. P4 measured its arms by re-staging the SDK per arm from the frozen
archives into fresh prefixes (`p4-b1`, `p4-p2c1`, `p4-p2c2`; §4.1) with a new
P4 run manifest (`p4-raw/parquet-run-manifest.txt`); the strategy-arm TUs
live on local-only dududu research branch `p4-strategy-arms` (`f8b0967`,
never pushed), dududu base branch untouched and clean at session close
(`env-pin-2026-10-28.txt:48-49`).

### 2.2 Comparator layers and cells

- **Layer A:** P1-native replay (validation-thin `pread` loop). **Layer B:**
  P2-sluice@B1 through `read_at` (full v2-r1 semantics) vs the P1 adapter
  parity control — the R1 arm structure; Layer B adjudicates Goal A.
  **Layer C:** the three strategy arms below plus the `pq-cfr` row; every
  strategy arm is a **NEW APPLICATION STRATEGY** — its request plan differs
  from the frozen trace by declaration (MISSION §6.2), the per-request byte
  oracle still gates correctness, and its results are **workload-opportunity
  evidence for AC-1/AC-2 (and AC-3's absence of shape) only — never a Sluice
  product claim** (AC candidates remain Awaiting Owner,
  `API_CAPABILITY_CANDIDATES.md`).
- Cells (16 ABBA process pairs where paired; GB-protocol arm descriptive):
  `pq-aa-replay-native`/`pq-aa-replay-sluice` (A/A floors);
  `pq-r1-transfer-b1` (P2-sluice@B1-SDK vs P1-native, interleaved ABBA —
  Goal-A transfer at real shape, preregistered NO_CLAIM: reads ≥ 33 KiB,
  median 1.39 MB, amortize any per-call wrapper cost);
  `pq-neg-p2c1` / `pq-neg-p2c2` (candidate vs B1 at the workload scale;
  adapter uses `read_at`, whose public-op instruction stream is
  machine-identical in both candidates → predicted PARITY; p2c2 is the
  supplementary symmetry control, added because §4.1 mandated the `p4-p2c2`
  staging and §7.4 requires per-candidate minimum coverage — the
  preregistered §4.2 table listed only pq-neg-p2c1; labeled supplementary
  in the row); `pq-t3-context-b1` (full R1 three-arm protocol, 10 reps —
  descriptive only, R1 deviation (a) unpaired, no CI claim);
  `pq-strategy-arrowprebuf` (Arrow `pre_buffer=true` planner baseline);
  `pq-strategy-gather-native` (native sort/merge/dedup + `preadv` replay of
  the frozen trace — both sides' best acceptable implementations,
  MISSION §6.1); `pq-strategy-gather-sluice` (same planning through Sluice's
  current public API: k × `read_at` into app-side staging + slicing,
  head-to-head with gather-native under matched footprint);
  `pq-cfr` (NOT_APPLICABLE_SHAPE).

### 2.3 Results

- **Goal A at real shape: unresolvable, as preregistered** `[RUN]`
  (`p4-raw/pq-r1-transfer-b1.txt`): P2-sluice@B1 vs P1-native, median
  **−0.194 ms/pass** (sluice favorable, ~235 ms/pass), CI [−0.546, +0.196]
  spans 0 — sub-MDE′ and ≪ the 1% PARITY band ≈ 2.34 ms → `NO_CLAIM`, the
  §4.2 prediction CONFIRMED (per-call wrapper ≈ +44-instr class vs ≈4.2
  ms/read median). CPU: PMU +9,637 instr/process (≈ +3.2K instr/pass ≈ 57
  ops × ~56 instr); rusage user 1.753 vs 1.740 s. PHYS: `pread64` 230 = 230,
  `read` 2823 = 2823; `fstat` +4/process — the open-time `file_info`+`size`
  pair, preregistered disclosure (plan §1.2), symmetric by protocol design.
  The zero-cost-abstraction question at this shape is therefore unrefuted,
  not proven.
- **Candidates are invisible at the workload scale, as predicted** `[RUN]`:
  pq-neg-p2c1 median **+0.111 ms/pass**, CI [−0.225, +0.638] spans 0,
  sub-MDE′ 0.601 → `PARITY` (PMU −137 instr/process ≈ 0; syscalls identical:
  pread64 230=230, read 2823=2823, fstat 34=34, ×3 repeats; ctest 3/3 per
  arm; oracle data_sha `8d2a752c…` identical across all three arms). The
  replay counters are archived per arm in
  `p4-raw/parquet-physical-work-pmu.txt` (replay-sluice-b1, -p2c1 and -p2c2
  blocks: replay_bytes=181.996M, replay_requests=57 each) — the paired cell
  files themselves carry wall series and analysis only (citation corrected
  post-review). pq-neg-p2c2 (supplementary): +0.027 ms, CI [−0.453, +0.736]
  → `PARITY` (PMU +139 ≈ 0; PHYS identical; ctest 3/3). A resolvable delta
  here would have been an environment alarm, not a candidate effect; none
  occurred.
- **Layer C: both sides' best planning pays at this trace — recorded as
  REGRESSION, AC-opportunity evidence only.**
  - `pq-strategy-gather-native` `[RUN]` (`p4-raw/pq-strategy-gather-native.txt`):
    dedup/merge + `preadv` vs P1-native replay: median **+216.229 ms/pass**,
    CI [215.146, 216.557] excludes 0, pos 16/16 → `REGRESSION`. Arm counters
    (`p4-raw/parquet-physical-work-pmu.txt` gather-native blocks):
    strategy_syscalls=2/pass, strategy_requests=57,
    strategy_bytes_issued=181.996M, staging_copies=0; the native composition
    is 1 `preadv` + 1 `pread` per pass (ROWS-SUMMARY phrases the plan as
    "56 requests → 1 preadv + 1 pread" — the 56-vs-57 discrepancy with the
    archived strategy_requests=57 counter is recorded here, not silently
    resolved; both forms agree on 2 syscalls/pass; archived 3-run strace
    windows: pread64 119 + preadv 3). The preregistered §4.3 merge
    prediction (≤56 saved entries ≈ 13–40 µs ⇒ NO_CLAIM) is **falsified as
    measured**: the arm delivers into 57 live per-request buffers (182 MB
    working set, DRAM-resident) while plain replay reuses one 16 MB
    L3-resident buffer — the destination-shape cost (~+1.2 ns/B) dominates
    and hides the syscall saving. Footprint confounder disclosed in the row;
    bytes issued 181,996,415 equal; rusage user 2.717 vs 1.740 s; PMU
    20,891,533,824 vs 15,946,048,776 instr/process. Correctness status,
    stated exactly: the session reports a per-request FNV oracle green
    (57/57 digests equal the reference replay) and that this oracle caught,
    before any number was recorded, a real bug — overlapping requests
    (29,625 B) make a sequential `preadv` unable to re-deliver already-passed
    bytes; the planner was changed to the preregistered dedup semantics, the
    arm rebuilt, oracle green, no sample deleted, no gate weakened. **No
    oracle output is archived under p4-raw** (the strategy cell files carry
    wall series + analysis only; the manifest carries ORACLE_T3_DATA_SHA for
    the T3 decode path; the archived ctest 3/3 is the R1 three tests) — the
    oracle statements therefore carry session-narrative status in this
    record, disclosed post-review. The REGRESSION verdict rests on the
    archived wall series and counters, not on the narrative.
  - `pq-strategy-gather-sluice` `[RUN]` (`p4-raw/pq-strategy-gather-sluice.txt`):
    the same dedup plan through the current public API (one `read_at` per
    extent into staging + app-side slice), head-to-head with gather-native
    under matched footprint: median **+40.094 ms/pass**, CI [38.933, 41.001]
    excludes 0 → `REGRESSION`. The §4.3 staging-copy prediction (≈22 ms
    class) is direction-CONFIRMED at ≈1.8× the pinned estimate;
    staging_copies=57, staging_bytes=181,996,415 (structural user-space
    copies). Syscalls are equal only at the counter level
    (strategy_syscalls=2/pass both sides); the **composition differs**,
    disclosed: the native side composes its 2 syscalls as 1 `preadv` + 1
    `pread`, the sluice side as 2 `pread64` (`read_at` per extent) —
    archived 3-run strace windows: native pread64 119 + preadv 3 vs sluice
    pread64 122 + preadv 0 (`p4-raw/pq-diag-summary.txt:2,6`;
    whole-process caliber, shared harness cancels). PMU 20,947,682,546 vs
    20,891,533,824 instr/process; oracle statements carry the same
    session-narrative status as gather-native (no oracle output archived).
  - `pq-strategy-arrowprebuf` `[RUN]` (`p4-raw/pq-strategy-arrowprebuf.txt`):
    Arrow's own coalescing planner (`pre_buffer=true`) vs the frozen P0
    whole-scan decode: median **+23.839 ms/pass**, CI [19.231, 24.184]
    excludes 0 → `REGRESSION`. `pread64` 173 → 29 per process (strace ×3 —
    the planner's coalescing is real and confirmed) yet the pass is slower;
    rusage sys 0.627 vs 0.523 s; request plan differs by declaration; same
    decode-oracle path verified by ctest. "What does planning buy" on the
    native side at this trace: nothing — it pays.
  - `pq-cfr`: `NOT_APPLICABLE_SHAPE` — a Parquet reader returns bytes to
    user buffers; no file-to-file transfer exists in this workload. AC-3's
    workload opportunity requires a copy-shaped workload, which no admitted
    workload provides (`p4-raw/blocked-and-na-rows.txt:7-8`). Recorded row,
    never silently dropped.
  - `pq-t3-context-b1` `[RUN]` (`p4-raw/pq-t3-context-b1.txt`), descriptive
    only (R1 deviation (a), unpaired): T3 medium medians p0/p1/p2 =
    312.877 / 315.470 / 319.854 ms; T2 medium 62.055 / 61.735 / 61.740 ms;
    T1 medium 100.4 / 98.5 / 99.4 µs; Replay 233.336 / 233.417 ms. No CI
    claim; continuity context with the frozen R1 receipts.
- Interpretation boundary for the strategy arms: these are measurements of
  **application strategies over this one trace shape** — they close the
  AC-1/AC-2 real-trace opportunity question honestly at the only admitted
  real trace ("no workload opportunity at this shape" is a valid P4 outcome
  and feeds the Owner decision), and they say nothing about Sluice product
  behavior, which never changed (public API untouched; product sources
  clean at session close, `env-pin-2026-10-28.txt:50`).

### 2.4 §6.4 decision rows

P99_DELTA = NOT_SUPPORTABLE_WITH_REASON on every PARQUET row: no per-request
logging exists in the replay arms (adding it would itself be an
amendment-noted change; §6 item 4/plan §7.1 — the change was not made).
Calibers: PMU/rusage are corroborating only; strace -c counts are count
caliber.

| WORKLOAD | BASELINE | CANDIDATE | CPU_DELTA | WALL_DELTA | P99_DELTA | PHYSICAL_WORK_DELTA | CORRECTNESS | UNCERTAINTY | VERDICT |
|---|---|---|---|---|---|---|---|---|---|
| pq-neg-p2c1 (workload-scale negative control, trace replay) | sluice@B1-SDK (`p4-b1`, manifest ref) | sluice@P2C1-SDK (`p4-p2c1`, `07d06803…`) | PMU −137 instr/process (≈0); rusage user 1.743 vs 1.753 s | median +0.111 ms/pass (~235 ms), CI [−0.225, +0.638] spans 0, sub-MDE′ 0.601 | NOT_SUPPORTABLE_WITH_REASON | syscalls identical (pread64 230=230, read 2823=2823, fstat 34=34, ×3); replay counters per arm archived in parquet-physical-work-pmu.txt (replay_bytes=181.996M / requests=57 each side) | ctest parquet 3/3 per arm; oracle data_sha 8d2a752c… identical ×3 arms | sluice-track sd_AA 1.129 ms | **PARITY** (prediction confirmed: machine-identical public op ⇒ no workload-scale delta; not an environment alarm) |
| pq-neg-p2c2 (supplementary symmetry control — §4.1 staging mandated, §4.2 table listed only p2c1; labeled supplementary) | sluice@B1-SDK | sluice@P2C2-SDK (`67251028…`) | PMU +139 instr/process (≈0) | median +0.027 ms/pass, CI [−0.453, +0.736] spans 0 | NOT_SUPPORTABLE_WITH_REASON | syscalls identical | ctest 3/3; oracle identical | as pq-neg-p2c1 | **PARITY** |
| pq-r1-transfer-b1 (Goal-A transfer at real shape, no candidate) | P1-native pread replay | P2-sluice read_at@B1 | PMU +9,637 instr/process (≈+3.2K/pass); rusage user 1.753 vs 1.740 s | median −0.194 ms/pass, CI [−0.546, +0.196] spans 0, sub-MDE′, ≪ 1% band 2.34 ms | NOT_SUPPORTABLE_WITH_REASON | pread64 230=230, read 2823=2823; fstat +4/process (open-time file_info+size pair, preregistered disclosure) | replay oracle identical; ctest green | native sd_AA 0.478 ms | **NO_CLAIM** — preregistered expectation CONFIRMED (wrapper cost unresolvable at real shape) |
| pq-strategy-gather-native (NEW APPLICATION STRATEGY — AC-1/AC-2 evidence only; plan differs by declaration) | P1-native replay (57 preads) | native dedup/merge + preadv (strategy_syscalls=2/pass, strategy_requests=57; composition 1 preadv + 1 pread — ROWS-SUMMARY's "56 requests" phrasing recorded, not resolved) | rusage user 2.717 vs 1.740 s; PMU 20.89 vs 15.95 G instr/process | median +216.229 ms/pass, CI [215.146, 216.557] excl 0, pos 16/16 | NOT_SUPPORTABLE_WITH_REASON | syscalls 57→2/pass (arm counters + strace windows pread64 119 + preadv 3); bytes 181,996,415 equal; staging_copies=0 | ctest parquet 3/3 per arm (R1 three tests); "per-request FNV oracle green 57/57 / oracle rejected the overlap bug pre-number" = session narrative, no oracle output archived in p4-raw (disclosed) | footprint confounder disclosed: 57 live buffers (182 MB, DRAM) vs one 16 MB L3-resident buffer; §4.3 merge prediction FALSIFIED as measured | **REGRESSION** (AC-1/AC-2 real-trace opportunity negative at this shape on the native side too; not a product claim) |
| pq-strategy-gather-sluice (NEW APPLICATION STRATEGY — AC evidence only) | pq-strategy-gather-native (footprint-matched) | same plan via current public API (read_at + staging/slice) | PMU 20.95 vs 20.89 G instr/process; rusage user 2.787 vs 2.717 s | median +40.094 ms/pass, CI [38.933, 41.001] excl 0, pos 16/16 | NOT_SUPPORTABLE_WITH_REASON | strategy_syscalls=2/pass both sides at counter level; composition differs (native 1 preadv + 1 pread vs sluice 2 pread64; windows pread64 119+preadv 3 vs 122+0); staging_copies=57, staging_bytes=181,996,415 (structural) | oracle statements = session narrative (as gather-native); ctest 3/3 per arm | both sides' best under matched footprint: preadv fills k buffers in one syscall vs read_at one span + 57 memcpys | **REGRESSION** — §4.3 staging prediction (≈22 ms) direction CONFIRMED, magnitude ≈1.8× |
| pq-strategy-arrowprebuf (NEW APPLICATION STRATEGY — native planner baseline) | P0 frozen (pre_buffer=false) | P0 with Arrow pre_buffer=true | PMU 13.789 vs 13.789 G instr/process; rusage sys 0.627 vs 0.523 s | median +23.839 ms/pass, CI [19.231, 24.184] excl 0, pos 16/16 | NOT_SUPPORTABLE_WITH_REASON | pread64 173→29/process (strace ×3 — coalescing real) | same decode-oracle path (ctest); plan differs by declaration | Arrow 23.0.1 / kernel 7.2.11, fresh measurement (no R1 comparison) | **REGRESSION** — native planning pays at this trace |
| pq-cfr | — | — | — | — | — | — | — | copy_file_range requires file-to-file transfer; absent in a Parquet reader | **NOT_APPLICABLE_SHAPE** |
| pq-t3-context-b1 (R1 three-arm protocol, 10 reps) | descriptive | descriptive | descriptive | medians recorded (§2.3); unpaired GB reps — R1 deviation (a) | NOT_SUPPORTABLE_WITH_REASON | descriptive | ctest green | no CI claim by preregistration | **descriptive context, no claim** |

### 2.5 Boundaries

1. One trace, one dataset, one host, one session: PARQUET conclusions are
   about this 57-request shape (≥33 KiB, median 1.39 MB, one contiguous
   segment + one 4-byte hole + 0.016% overlap) — the only admitted
   real-workload trace — and are not generalized to other query or decode
   patterns (plan §9.3).
2. Strategy-arm results are new-application-strategy evidence for
   AC-1/AC-2/AC-3, capped by the P3 §9 admission regime; they are not
   product claims, not comparable to frozen R1 numbers (kernel drift,
   different request plans by declaration), and do not measure any Sluice
   code change (public API untouched).
3. The gather-native falsification is disclosed with its footprint
   confounder: the measured statement is "multi-buffer gather destination
   costs far more than the syscalls it saves **at this destination shape**";
   it does not preclude a different conclusion for workloads whose consumer
   already requires scattered live buffers.
4. Kernel drift 7.2.9-201 → 7.2.11 disclosed; no cross-kernel comparability
   claimed; historical R1 numbers quoted only.
5. Per plan §9.4, P99 supportability would require per-request logging added
   by amendment; it was not added, and no tail statement is made.

---

## 3. DUCKDB — BLOCKED (status row only, not evidence)

- **Admission status: BLOCKED** (plan §1.3, §5) `[RUN]`: `gh issue view 578`
  re-verified in the measurement session — state OPEN, updatedAt
  2026-10-26T15:18:37Z, exactly one comment (the #600 handoff); R0 items 1–4
  (freeze SHAs + fio/DuckDB A/A baseline; seam analysis;
  `DUCKDB_FIO_R0_PREREG` deliverable) all unexecuted — the issue body itself
  states it is "not an assertion that the study was completed". Local state
  re-verified `[RUN]` (`p4-raw/blocked-and-na-rows.txt:2-4`): `fio`
  NOT_INSTALLED; DuckDB binary absent; dududu DuckDB submodule uninitialized
  (`-069cc9f9…`); `experiments/duckdb/*` contains README scaffolding only
  (no runner, no oracle).
- **Row:** `DUCKDB — BLOCKED` (ROWS-SUMMARY row 24): recorded in the §7
  output table with this evidence; no arm measured; no number produced.
  MISSION §6.2 forbids treating the unfinished D1–D4 as existing evidence,
  and the fio leg of R0 is equally unstarted.
- **Boundary:** revisit only via a numbered amendment naming landed #578 R0
  admission evidence (plan §5; P3 plan §9.4 rule). The blocked row is a
  coverage statement, never a performance verdict.

## 4. HF_SMALL_IO — BLOCKED (status row only, not evidence)

- **Admission status: BLOCKED** (plan §1.4, §5) `[RUN]`: admission requires
  a documented real-application source with the eight MISSION §6.2 fields
  (request cause; input distribution; offset/size; file type; sequence
  dependency; data correctness; original application scenario; legal
  request-combination support). Searched evidence: P0 §4.4 records "No
  admitted high-frequency small-I/O workload exists yet" `[TRANSCRIBED]`;
  #578 carries no workload record; dududu #4 is a state/indexing-cost audit
  question, not an application I/O source; R1 Parquet is 57 large reads
  (median 1.39 MB — the opposite shape); the G0 W-01 matrix is synthetic.
- **Row:** `HF_SMALL_IO — BLOCKED` (ROWS-SUMMARY row 25). The
  high-frequency-small-read *shape* stays covered inside W01 by its small
  cells (`w01-hf-1b`, `w01-hf-4k` — kit evidence, not an application-workload
  claim); those cells' NO_CLAIM verdicts are recorded at §1.4 and are not
  re-labeled here.
- **Boundary:** revisit requires the eight-field record + admission before
  any cell runs (plan §5); unblocking mid-campaign by anything short of a
  numbered amendment naming that evidence is forbidden.

---

## 5. Cross-workload conclusions and MISSION §11 prefill

### 5.1 Cross-workload findings (local verdicts retained; no merging)

1. **Goal A (zero-cost abstraction) at the real workload: unrefuted,
   unproven.** The only real-shape transfer cell (pq-r1-transfer-b1) is
   NO_CLAIM with |median| ≈ 0.08% of pass time, ≪ the 1% band — the
   per-call wrapper cost that P0/P1 measured at small ops is amortized to
   unresolvability by ≥33 KiB reads. Both candidates are machine-identical
   PARITY at workload scale. Nothing in P4 contradicts zero-cost parity; P4
   also does not elevate it to a proven claim at application shape.
2. **The P2C1 KEEP wall claim survives load testing, in its declared shape
   only.** Three composition cells WIN_CONFIRMED at fresh MDE′ (comp-read
   −19.048, eofmix −15.907, cursor −15.130 ns/op, all CI-excluding), with
   all physical-work deltas exactly zero and negative controls PARITY. The
   seven non-composition W01 cells are NO_CLAIM: the win does not transfer
   to public ops, metadata, random offsets, 1 MiB streaming, or cold btrfs.
   No single cell's value is generalized.
3. **P2C2 remains instruction-caliber.** Wall NO_CLAIM at its benefit shape
   under write-track MDE′ 25.235; −20 instr/op retained; both negative
   controls PARITY. The P2 KEEP scope is confirmed, not widened.
4. **No beyond-parity win exists at any admitted real workload, and the best
   legal native competition at the Parquet trace pays, not wins.** All three
   Layer C strategy arms are REGRESSION (gather-native +216.229 ms — the
   §4.3 merge NO_CLAIM prediction falsified by the destination-footprint
   cost; gather-sluice +40.094 ms vs footprint-matched native — staging
   prediction direction confirmed at ≈1.8×; arrowprebuf +23.839 ms — Arrow's
   own planner pays). AC-1/AC-2's real-trace opportunity at this shape is
   negative; AC-3 has no shape (NOT_APPLICABLE_SHAPE). These are AC-candidate
   opportunity findings for the Owner, not Sluice product claims.
5. **Preregistered predictions: eight confirmed instances, two falsified
   (recorded, never re-derived).** Confirmed: transfer NO_CLAIM; candidate
   PARITY ×2 (counted separately); wcomp wall NO_CLAIM; cold NO_CLAIM;
   seq-1m NO_CLAIM; hf-4k public-op NO_CLAIM; metadata machine-identical
   (meta-fi PMU 0, PHYS 0). Falsified: §4.3 merge NO_CLAIM (measured
   REGRESSION, confounder disclosed); P2 eofmix cap direction (this session
   WIN where P2 recorded +10.228 ns EXCESS — falsified-direction recorded;
   the P2 cap remains the conservative claim bound). The §4.3 staging-copy
   REGRESSION prediction is direction-confirmed at ≈1.8× the pinned
   magnitude (recorded in the gather-sluice row, counted with neither side).
6. **No tail-for-throughput trade is claimed, relied on, or hidden anywhere
   in P4.** Every win/regression above is a median wall effect with
   PHYSICAL_WORK_DELTA 0 (W01) or an explicitly structural
   destination/staging cost (PARQUET strategy arms); P99 was
   NOT_SUPPORTABLE_WITH_REASON on every row (not measured: no per-op logging
   at W01 scale; the PARQUET per-request-logging amendment was not made).
   No cell's verdict depends on a latency-distribution effect. Because the
   tail was not measured, this record makes no tail statement at all,
   favorable or unfavorable — the non-measurability itself is disclosed in
   every row, so nothing about tail behavior is left implicit.
7. **Coverage boundary (plan §9.5):** P4's real-workload coverage is exactly
   W01 (kit shapes) + PARQUET (one frozen trace). DUCKDB and HF_SMALL_IO are
   BLOCKED status rows. No conclusion here implies broader coverage; E5
   single host, no cross-host or cross-kernel comparability.

### 5.2 MISSION §11 prefill (proposed values; final wording owned by P5/P6 closeout)

```
W01_RESULT = CONDITIONAL_WORKLOAD_WIN (P2C1 only, composition shape:
  w01-comp-read-4k wall -19.048 ns/op CI[-21.376,-10.723] >= fresh MDE' 12.414,
  eofmix -15.907 CI[-17.130,-9.639], cursor -15.130 CI[-22.305,-4.849];
  PHYS delta 0 everywhere; negatives PARITY; hf/seq/rand/meta/cold NO_CLAIM;
  P2C2 wall NO_CLAIM, instruction caliber only; kit-shape evidence, not an
  application-workload claim; Layer C collapses onto Layer B by structure)
PARQUET_RESULT = NO_CLAIM (Goal-A transfer -0.194 ms/pass CI[-0.546,+0.196]
  << 1% band 2.34 ms; P2C1/P2C2 workload-scale PARITY (machine-identical
  public op); all three Layer C strategy arms REGRESSION: gather-native
  +216.229 ms (AC-1/AC-2 real-trace opportunity negative at this shape,
  footprint confounder disclosed), gather-sluice +40.094 ms vs
  footprint-matched native, arrowprebuf +23.839 ms; pq-cfr
  NOT_APPLICABLE_SHAPE; no beyond-parity win and no Sluice-side regression
  at the only admitted real trace; strategy findings are AC-candidate
  opportunity evidence, not product claims)
DUCKDB_RESULT = BLOCKED (#578 R0 incomplete: issue OPEN, R0 items 1-4
  unexecuted, fio absent, duckdb submodule uninitialized, scaffolding
  READMEs only; status row only — not performance evidence)
```

HF_SMALL_IO has no §11 field of its own; its BLOCKED status row is carried
in §4 and in the §7 output table (`p4-raw/ROWS-SUMMARY.md` rows 24–25,
`p4-raw/blocked-and-na-rows.txt`).
