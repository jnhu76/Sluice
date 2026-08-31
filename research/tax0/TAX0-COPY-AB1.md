# TAX-0 COPY-AB-1 — Application-Level Copy A/B (R0 vs R1 router)

- Experiment: COPY-AB-1 (task brief; design authority:
  `research/tax0/TAX0-COPY-AB1-DESIGN.md`)
- Base SHA: `c4f0b4aacaa55c5c204287b4d7798533af49535b`
- COPY-AB1-FREEZE SHA: `d428cef` (DRAFT PR #257)
- FINAL HEAD: recorded in the PR after the evidence commit
- Bench: `bench/tax0_copy_ab_bench` (real sluice-copy engine + injected
  backend); runner `scripts/bench/tax0-copy-ab1-run.py`; validator
  `scripts/bench/tax0-copy-ab1-validate.py`
- Host: Fedora 44, kernel 7.1.9, Xeon E5-2666 v3, clang 22.1.8 Release,
  liburing 2.13, `taskset 0,2,4,6` (verified non-SMT physical cores),
  schedutil governor (fingerprint in every artifact).
- EVIDENCE CORRECTIVE (2026-08-31): a post-evidence review found the
  freeze-era adjudication implementation did not compute the as-frozen
  statistics correctly (8 findings, listed in the Evidence Corrective
  section). The decision was RECOMPUTED from the unchanged raw rows; no
  measurement was rerun; all six official artifacts are byte-identical
  (sha256 recorded below). The verdict below is the CORRECTED verdict.

# 1 Verdict

**COPY-AB-1 PASS — BENEFIT ONLY IN CAPACITY-SKEWED REGIMES**
(verdict SURVIVED the evidence corrective, unchanged).

The primary metric (instructions / copied byte) improves materially —
under the corrected paired-median decision rule — exactly where EXP-U0's
causal model predicts: when `request_capacity` overprovisions the
pipeline's active depth (18/27 cells per filesystem, precisely the
C=128/C=512 strata). The benefit vanishes at near-capacity sizing (0/9;
btrfs additionally shows two small but formally material near-capacity
regressions). Cycles improve materially only in the most skewed cells
(6/27). Wall throughput is NOT material (0/27). Production R1 is NOT
implemented by this task.

# 2 Question

Does the #256-selected R1 reverse-scan router candidate materially improve a
REAL Sluice file-copy workload — the existing `sluice-copy` bounded pipeline
on the REAL `UringAsyncBackend` — when `request_capacity C` exceeds the
actual active I/O depth? Primary: instructions / copied byte. Secondary:
cycles / copied byte. Wall throughput is reported and separately classified.

# 3 Existing sluice-copy architecture

Verified from code at the base SHA (full trace in the design doc):

- The production CLI path (`run_pipelined_copy`) constructs
  `ThreadPoolBackend` and forwards to `run_pipelined_copy_with_backend()` —
  the SAME copy task body (`PipelinedCopyTask::run_body`) the CLI uses.
- `pipeline_depth` P = read-ahead slot count; up to P reads outstanding; at
  most ONE write outstanding (strictly ascending offsets). Code trace +
  smoke: peak of concurrently ACCEPTED backend requests = P.
- `UringConfig` exposes `request_capacity` and `queue_depth` independently;
  C < Q and C > Q are both legal by contract (validation rejects only
  C=0, oversized C, Q=0).
- The R0/R1 modes exist only under `SLUICE_ASYNC_INTERNAL_TESTING`
  (#256 research seam); the production router is untouched.

# 4 Why backend injection is a fair A/B

The research bench calls the public
`run_pipelined_copy_with_backend(...)` — the audited application entry —
with a caller-constructed backend as the ONLY variable: identical copy
algorithm, buffers, worker count (`workers=1`, the CLI default), Runtime
bridge (`run_task_to_result`), deterministic source bytes, and process
placement. R0 = `production_baseline`, R1 = `reverse_scan`, installed via
the #256 seam on the quiescent fresh backend before any runtime drives it.
The bench links `sluice_async_internal_testing`; the production build
compiles none of the seam. ThreadPool control uses `ThreadPoolBackend()`
default construction — the exact production object — and is never mixed
into the R0/R1 aggregates.

Router causality is INHERITED, not re-measured: EXP-U0/#254 causally
attributed the capacity tax to the router scan and #256 selected R1;
COPY-AB-1 is the APPLICATION-EFFECT validation of that candidate. The
official COPY-AB-1 artifacts do not record structural router-iteration
witnesses (lookup counts / iterations); no unavailable evidence is claimed.

# 5 A/A noise calibration

Both labels execute the IDENTICAL `production_baseline` mode; per-round
permutation over (label, cell) entries closes positional bias. 11 paired
reps × 3 cells × {tmpfs, btrfs} (66 rows per session).

Corrected envelope, recomputed from the official A/A raw rows under the
as-frozen rule (per-cell p90 of |log2 paired ratio|, nearest-rank index
`ceil(0.90·n)−1`; session noise = max over cells; campaign envelope = max
over both sessions):

| metric | tmpfs max-cell p90 | btrfs max-cell p90 | frozen envelope | threshold ratio |
|--------|--------------------|--------------------|-----------------|-----------------|
| instr  | 0.002030 (4K,P8,C32)  | 0.0000116 (4K,P8,C32)  | 0.002030 | ≤ 0.9800 (2% floor dominates) |
| cycles | 0.115284 (4K,P8,C32)  | 0.145216 (1M,P8,C512)  | 0.145216 | ≤ 0.8177 (2×noise dominates) |
| wall   | 0.398224 (4K,P8,C32)  | 0.367874 (4K,P8,C32)   | 0.398224 | ≤ 0.5758 (2×noise dominates) |

(The freeze-era recorded POOLED values 0.002212/0.146368/0.411414 are
superseded; their provenance is documented in the Evidence Corrective
section. The corrected envelope regenerates from the official raw rows at
every validation — the validator fail-closes on any mismatch.)

FROZEN materiality rule (committed at the freeze, before any R0/R1 run,
unchanged by the corrective): metric m improves MATERIALLY iff the
per-cell PAIRED median satisfies
`median_log2(R1/R0) <= -max(2 × envelope[m], log2(1/0.98))`:

- instructions: ratio ≤ 0.980 (2% floor dominates)
- cycles: ratio ≤ 0.8177 (2×noise dominates)
- wall: ratio ≤ 0.5758 (2×noise dominates)
  Wall is never required for a CPU-benefit PASS; it is separately
  classified with this envelope.

A/A provenance (LIMITATION, kept visible): the A/A sessions were measured
PRE-FREEZE on a DIRTY research tree — git sha `c4f0b4a`, `dirty=true`;
the dirty paths were the research bench/runner/validator and the artifact
output itself; the benchmark binary sha256 is recorded in the artifacts
and the production libraries were unchanged at that tree. This is a
provenance limitation, NOT a clean-tree freeze. Process lesson for future
experiments: TOOL-FREEZE → A/A CALIBRATION → THRESHOLD-FREEZE → OFFICIAL
CAMPAIGN. (The official campaign sessions ran after the freeze commit
`d428cef`, likewise with uncommitted research-only files, recorded in each
artifact's note.)

The §5 pre-registered 1 GiB amendment was NOT invoked: 1 MiB × 256 MiB
cells proved counter-stable (instr per-cell p90 |d| ≈ 5e-06/4e-06).

# 6 Frozen matrix

- File: 256 MiB deterministic source (splitmix64 master block, seed
  `0xE1E1E1E121212121`); sha256 bound per row; Q = 64 everywhere;
  workers 1.
- Cells: {4 KiB, 64 KiB, 1 MiB} × {P=1, 8, 32} × {C_near = P+1, 128, 512}
  × {tmpfs, btrfs} = 27 cells per session; labels {r0, r1}; 9
  blocked-randomized rounds; seed `0x434F5059`. 486 measured rows per
  session (972 total).
- ThreadPool control: B=4 KiB, P=8, both filesystems, 9 rounds each.
- C_near = P+1 justification: code trace (Phase 2 reaps one read before
  submitting the write; peak accepted = P) + smoke (C=P accepted; C<P is
  outside the envelope and hits the pre-existing Phase-3 drain defect —
  design doc §7.1 finding, recorded, NOT fixed here; governed by issue
  #258).

# 7 Correctness / same-work

Every measured row proves `bytes_copied == 268435456`, `write_ops == chunks`,
`short_writes == 0`, `chunks ≤ read_ops ≤ chunks + P` (the validator
fail-closes on all of these), and after EVERY measured process an unmeasured
`cmp` full byte comparison of destination vs source passed. No `cmp` failure
occurred in any session. uring rows additionally prove `real_uring == true`
(`SLUICE_HAS_LIBURING` compiled + `available()` verified before handover;
stub results are structurally impossible). VALIDATOR (corrected,
six-artifact campaign seal incl. cross-artifact consistency): PASS on all
six official artifacts; self-test: 40 planted mutations plus a
descriptive-vs-paired disagreement fixture all fail closed; the frozen A/A
envelope regenerates from the official A/A raw rows.

# 8 Raw application results

Artifacts (recomputed-and-verified by the validator):
`tax0-copy-ab1-tmpfs.json`, `tax0-copy-ab1-btrfs.json` (486 rows each),
`tax0-copy-ab1-aa-*.json` (66 rows each), `tax0-copy-ab1-control-*.json`
(9 rows each). read_ops = chunks + 1..P at the EOF window (bounded, as
derived); write_ops == chunks exactly; short_writes == 0 everywhere.

# 9 instructions/byte (PRIMARY)

Two statistics, never conflated:

- DESCRIPTIVE — GM of per-cell ratio-of-arm-medians: **0.8982 (tmpfs) /
  0.8886 (btrfs)**, a ~10–11% instruction reduction. Informs; does not
  classify.
- DECISION (authoritative) — per-cell PAIRED median of the 9 blocked-round
  log2(R1/R0) values against the frozen threshold (≤ 0.9800): **18/27
  cells MATERIAL IMPROVEMENT per filesystem — exactly the 18
  capacity-skewed cells; 0 of 9 near-capacity cells**. Stratified:
  C=128 → 9/9, C=512 → 9/9, C=P+1 → 0/9 (both fs).

Pooled blocked paired effect (descriptive context): median log2 −0.0825
(tmpfs) / −0.0806 (btrfs).

# 10 cycles/byte (SECONDARY)

DESCRIPTIVE GM ratio **0.9224 (tmpfs) / 0.9200 (btrfs)**. DECISION
(threshold ratio ≤ 0.8177): **6/27 cells MATERIAL per filesystem** — the
{4 KiB, 64 KiB} × C=512 cells (paired-median ratios 0.769–0.802 tmpfs,
0.760–0.815 btrfs). C=128 and all 1 MiB cells stay inside the calibrated
band. Near-capacity cells: ≈ 1.00.

# 11 wall throughput

DESCRIPTIVE GM ratio **0.9770 (tmpfs) / 0.9927 (btrfs)**. DECISION
(threshold ratio ≤ 0.5758): **0/27 cells material on both filesystems** —
the strongest paired-median improvement in any single cell is −0.201 log2
(ratio 0.85, tmpfs 4 KiB P=8 C=512). Copy throughput on this warm-cache
matrix is dominated by other work (memory traffic / filesystem path), not
by the router tax.

# 12 capacity-ratio effect

The causal prediction is confirmed with a clean dose-response shape.
DESCRIPTIVE per-stratum GM (ratio-of-arm-medians, both fs):

| C stratum | C / active depth | tmpfs instr GM | btrfs instr GM |
|-----------|------------------|----------------|----------------|
| C = P+1   | ~1               | 1.0024         | 1.0078         |
| C = 128   | 4–128            | 0.9381         | 0.9330         |
| C = 512   | 16–512           | 0.7707         | 0.7462         |

DECISION material-cell counts (paired-median rule) follow the same
monotone shape: 0/9 → 9/9 → 9/9 (instr). Benefit grows with C skew and
disappears at near-capacity sizing — the application-level signature of
the EXP-U0 capacity tax.

# 13 buffer-size effect

DESCRIPTIVE instr GM: 4 KiB **0.8752 / 0.8713**, 64 KiB **0.8939 /
0.8758**, 1 MiB **0.9262 / 0.9195** (tmpfs/btrfs). DECISION instr
material cells per B stratum: 6/9 / 6/9 / 6/9 — every stratum improves in
its C=128/C=512 cells. The router tax is per-I/O-op, so its per-byte
weight shrinks as B grows (1 MiB cells clear the bar only at C=512 with a
smaller margin: ratios 0.83–0.84) — exactly the pre-registered
prediction, confirmed as a trend.

# 14 pipeline-depth effect

DESCRIPTIVE instr GM: P=1 **0.9011 / 0.8878**, P=8 **0.8881 / 0.8750**,
P=32 **0.9055 / 0.9032**. DECISION instr material cells: 6/9 per P
stratum. P interacts with the C strata (P=32 forces C_near=33, which is
noise or slightly negative): the capacity strata (§12), not P alone, carry
the effect.

# 15 filesystem-strata consistency

tmpfs and btrfs agree on every decision: same 18/27 material instr cells
(same strata), same 6/27 cycles cells, same null wall result, same
monotone C-stratum ordering. One divergence: on btrfs the paired rule
classifies TWO near-capacity cells (4 KiB/P32/C33: +2.5%, 64 KiB/P32/C33:
+2.4%) as material regressions, while tmpfs's worst near-capacity cell
(+1.8%) stays inside the band. These are tested-strata consistency checks
on ONE host, NOT a universal-robustness claim.

# 16 ThreadPool control

The ThreadPool control is a HARNESS-STABILITY control, not an experimental
manipulation of C: `ThreadPoolBackend()` has no Uring router and no
request_capacity parameter at all, so it cannot probe a C-dependence. It
shows the same copy harness is stable under a backend with no
router/capacity dimension: B=4 KiB P=8, 9 rounds per fs, instr/byte median
3.078 (tmpfs, rel. sd 0.08%) / 3.097 (btrfs, 0.30%).

# 17 What improved

- instructions / copied byte: MATERIAL in every capacity-skewed cell
  (18/18 across both fs, paired-median rule). Overall scale (descriptive
  GM): −10.2% tmpfs, −11.1% btrfs.
- Largest single cell: 4 KiB, P=8, C=512 → paired-median instr ratio
  **0.6825 / 0.6860** (~31–32% fewer instructions per byte).
- cycles / byte: materially improved in the most skewed cells (6/27 per
  fs: {4 KiB, 64 KiB} × C=512; paired-median ratios 0.760–0.815).
- Wall (secondary): tmpfs skewed cells improve up to 15% (ratio 0.85),
  but 0/27 cells clear the frozen materiality bar.

# 18 What did NOT improve

- Near-capacity cells (C = P+1): zero material benefit — descriptive GM
  1.002–1.008. On btrfs, two deep-pipeline cells are formal MATERIAL
  REGRESSIONS under the paired rule (4 KiB/P32/C33 +2.5%, 64 KiB/P32/C33
  +2.4%; the +0.980 improvement bar is symmetric); tmpfs's worst
  near-capacity cell (+1.8%, 4 KiB/P32/C33) stays inside the band. This
  is the known R1 near-capacity property from #256 (reverse scan pays a
  small cost when live entries cluster at low router indices).
- cycles / byte at GM level and in C=128/1 MiB strata: inside the
  calibrated band.
- Wall throughput on both filesystems: 0/27 material; on btrfs
  essentially unchanged (descriptive GM 0.9927). Copy wall is dominated
  by other work in this warm-cache matrix.

# 19 What is NOT proven

- Nothing about production: R1 is NOT implemented on the production path;
  the CLI default backend remains ThreadPoolBackend; #250/#255 remain open
  under their own governance. Landing implications are a decision for the
  router-fix review, gated on its own compliance work.
- No claim beyond the tested matrix (this host/kernel, tmpfs + warm btrfs,
  these geometries). The optional real-storage sensitivity group was
  SKIPPED (no suitable dedicated persistent-storage setup isolated for this
  campaign) — device-latency-dominated wall behavior is unmeasured.
- The near-capacity regressions (+1.8% to +2.5%) are small; their
  practical significance is untested against differently shaped workloads.
- The A/A calibration ran on a pre-freeze dirty research tree (§5) — a
  provenance limitation on the noise envelope's tooling identity.
- Formal/whole-program claims: none (benchmarks are not proofs).
- The out-of-envelope Phase-3 drain defect (design doc §7.1) was recorded
  as issue #258, not fixed; it does not touch the frozen envelope
  (C ≥ P+1 never rejects) but it is real and needs its own authorized fix
  task.

# 20 Production-landing implications

- The R1 candidate's benefit is REAL at the application level, and its
  scope is precisely characterized: it scales with `C / active_depth` and
  vanishes when the arena is sized near the pipeline's actual concurrency.
  This upgrades the #256 conclusion from "microbenchmark-level win" to
  "application-level win, scope-bounded".
- A landing decision should therefore pair the router change with capacity
  SIZING guidance: with C near the real active depth, R1 ≈ R0 (no harm);
  with overprovisioned C, R1 recovers most of the tax. The measured cost
  is the small near-capacity corner at deep pipelines (+1.8% to +2.5% on
  2 of 18 near-capacity cells, both on btrfs).
- Copy wall throughput is NOT a landing argument on this evidence.
- Post-landing verification must reuse this A/B harness (it is validated,
  mutation-tested, and reproducible from the frozen seed).

# 21 Evidence Corrective

A corrective review of the evidence ADJUDICATION (not of the measurements)
found the freeze-era implementation deviated from the as-frozen analysis
plan. The physical campaign was NOT rerun; every decision statistic was
RECOMPUTED from the existing raw rows.

Raw evidence freeze (sha256, verified identical before and after the
corrective — OFFICIAL_ARTIFACTS_BYTE_IDENTICAL: YES):

```
19aff6b99c9614c62637a9017e409166671e39de8a403df0703cbfaf10b589a8  tax0-copy-ab1-aa-tmpfs.json
3d1427128a886d83fd2eeabc72d325c17e34186ad0a367188ee2af55b7ae03b3  tax0-copy-ab1-aa-btrfs.json
376dc9621b609725965208e42169e577ea918ec8828acccfae3e365402847940  tax0-copy-ab1-tmpfs.json
028fc04c3d30e820165f01c9679a1d00d91829737c3db002babda3fbd04d4c66  tax0-copy-ab1-btrfs.json
e7d44e40ac3dc33092026150058444e756a77f854d11d73f49e009c2d9400fd8  tax0-copy-ab1-control-tmpfs.json
8e7f76ff0615c52c5502c352166512620ac01a1519565b1d0d9e7a3fa6e54e1a  tax0-copy-ab1-control-btrfs.json
```

| # | Finding | Root cause | Fix | Raw data rerun? | Verdict impact? |
|---|---------|------------|-----|-----------------|-----------------|
| 1 | Materiality used ratio-of-arm-medians instead of the paired median | Validator classified from the descriptive ratio section | Validator recomputes per-cell paired effects from raw rows and classifies with the paired median; a fixture proves the two rules can disagree and the paired rule wins | NO | None: 18/27 instr cells survive; cycles/wall counts now decision-based (6/27, 0/27) |
| 2 | A/A envelope pooled all pairs instead of max per-cell p90 | Freeze-time envelope derived from pooled p90, contradicting §7's "max over cells" | `corrected_envelope()` recomputes per-cell nearest-rank p90; frozen constants must regenerate from official A/A raw rows at every validation | NO | Small: corrected envelope 0.002030/0.145216/0.398224; thresholds move to 0.9800/0.8177/0.5758 |
| 3 | p90 order statistic biased low (`int(0.9·n)−1`) | Off-by-one index choice in the helper | Nearest-rank `ceil(0.9·n)−1` everywhere the envelope/p90 is computed | NO | None beyond #2 (floor dominates instr) |
| 4 | Runner per-cell aggregate overwrote rounds (recorded A/A per_cell all n_pairs=1) | Accumulator keyed by (round, cell) but emitted under a round-less cell name | Helper collects ALL paired rounds per cell; corrupt recorded per_cell fields ignored (never authoritative) | NO | None: per_cell was never decision input |
| 5 | Six-artifact campaign was not top-level sealed | Validator accepted artifacts independently | Seal: exactly one tmpfs + one btrfs per category, exact kind/experiment, no duplicate paths/fs; cross-artifact source/binary/seed/matrix/fs-label checks; 14 seal mutations fail closed | NO | None: all six official artifacts pass the seal |
| 6 | Design claimed router witnesses not present in artifacts | Prose carried §4 intent ("witness") beyond what rows record | Docs state router causality is INHERITED from EXP-U0/#254 and #256; COPY-AB-1 is application-effect validation | NO | None: no unavailable evidence now claimed |
| 7 | ThreadPool control wording overstated ("no capacity-dependent effect observed") | Session has no C manipulation; wording implied one | Reworded: harness-stability control under a backend with no router/request_capacity dimension | NO | None |
| 8 | A/A provenance is a pre-freeze dirty research tree | A/A measured before the freeze commit | Provenance kept visible (§5): git sha c4f0b4a, dirty=true, dirty paths = research bench/runner/validator/artifact output, binary sha recorded, production libraries unchanged; NOT a clean-tree freeze; process lesson: TOOL-FREEZE → A/A CALIBRATION → THRESHOLD-FREEZE → OFFICIAL CAMPAIGN | NO | None: envelope regenerates from recorded rows |

Corrected decision result (authoritative; per-cell paired median):

| fs | C stratum | cells | material instr | material cycles | material wall |
|----|-----------|-------|----------------|-----------------|---------------|
| tmpfs | C = P+1  | 9  | 0  | 0 | 0 |
| tmpfs | C = 128  | 9  | 9  | 0 | 0 |
| tmpfs | C = 512  | 9  | 9  | 6 | 0 |
| tmpfs | all      | 27 | 18 | 6 | 0 |
| btrfs | C = P+1  | 9  | 0 (2 regressions) | 0 | 0 |
| btrfs | C = 128  | 9  | 9  | 0 | 0 |
| btrfs | C = 512  | 9  | 9  | 6 | 0 |
| btrfs | all      | 27 | 18 | 6 | 0 |

B-stratum (instr material): 6/9 per buffer group on both fs.
P-stratum (instr material): 6/9 per depth on both fs.

Descriptive results (unchanged numbers, clearly separated):
GM ratio-of-arm-medians — instr 0.8982/0.8886, cycles 0.9224/0.9200,
wall 0.9770/0.9927 (tmpfs/btrfs).

**ORIGINAL VERDICT: COPY-AB-1 PASS — BENEFIT ONLY IN CAPACITY-SKEWED
REGIMES. CORRECTED VERDICT: COPY-AB-1 PASS — BENEFIT ONLY IN
CAPACITY-SKEWED REGIMES. VERDICT CHANGED: NO** — the corrected rule
confirms the original headline with decision-grade evidence (the original
18/27 claim was directionally right for the right reason: the paired
median and ratio-of-medians agree on every instr improvement cell here;
the corrected computation adds the two formal btrfs near-capacity
regressions and removes reliance on the descriptive GM).

PERFORMANCE_MEASUREMENTS_RERUN: NO. PRODUCTION_BEHAVIOR_CHANGED: NO.
COPY_ALGORITHM_CHANGED: NO. R0/R1_IMPLEMENTATION_CHANGED: NO.
OFFICIAL_ARTIFACTS_CHANGED: NO.
