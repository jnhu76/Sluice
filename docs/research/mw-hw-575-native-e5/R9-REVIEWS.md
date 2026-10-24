# R9 — Independent Reviews

Two independent reviews were performed after the implementation and
evidence were frozen, each with an explicit charter to distrust the
implementer's conclusions: Review A (semantics and concurrency) verified
claims against source, build files, disassembly and **re-executed the
mutation kills itself**; Review B (performance methodology) **recomputed
the principal reductions from the raw JSONL with its own code** and
audited pairing, authenticity, freeze discipline and statistics.

Outcomes: **Review A = APPROVE** (0 P1, 5 P2), **Review B = REVISE**
(3 P1, 6 P2). All P1 findings were resolved before this record was
finalized; every P2 was addressed or recorded. No affected gate required
re-running measurements: Review B's corrections are statistical (the raw
data was never in question — its integrity audit passed on all 1080 rows)
and its verdict statement is explicit that the product-level conclusion
"survives review unchanged — indeed strengthened".

## Review A — Semantics and Concurrency (verbatim)

> # Review A — Semantics and Concurrency (Sluice #575)
>
> ## Verdict: APPROVE
>
> No P1 (blocking) findings. Five P2 (non-blocking) findings, all
> documentation/evidence-hygiene; every load-bearing semantic, concurrency
> and isolation claim in R1/R3/R4 was independently verified against
> source, build files, disassembly and re-executed mutation kills, and
> every one held.
>
> **What was verified (summary of the reviewer's own checks):**
> 1. Full reference inventory re-derived by grep — exactly the documented
>    sites; no production reader of either counter exists (sole reads are
>    the macro-gated `*_for_test()` getters; seam headers included only
>    under the same gates).
> 2. Both increments are relaxed RMWs with no ordering role; worker/CV/
>    shutdown/terminal/publication paths never read the counters; removal
>    cannot change any interleaving.
> 3. All four arm patches read in full: they guard exactly the increment
>    (OP-ONLY) or increment+field (FULL); patch SHA-256 values match R3's
>    table exactly; nothing else changes.
> 4. Sibling protocol atomics (`active_workers_`, `live_cookies_`,
>    `live_control_sqes_`) untouched at source and binary level; the
>    reviewer's own disassembly reproduces R3's assembly table exactly,
>    including the UR-FULL relocation of the surviving protocol atomics;
>    sizeof table reproduced with the committed probe.
> 5. ODR/build isolation confirmed in `xmake/tests.lua` +
>    `xmake/libraries.lua` + `compile_commands.json`: every macro target is
>    `sluice_core`-only and compiles its own backend TUs; every
>    `sluice_async` consumer is macro-free and has no `src/` include; no
>    binary mixes layouts.
> 6. **Mutation kills independently re-executed**: TP-M1 and UR-M1 both
>    reproduced BASE GREEN → the exact three named RED failures → RESTORED
>    GREEN; the pre-existing UR suite staying green under UR-M1 was also
>    reproduced, confirming the documented oracle gap and that the new
>    oracle closes it.
> 7. Public `AsyncStats` cannot be affected (population sites enumerated;
>    `UringStats` never populated by async sources).
> 8. Arm-green re-run on both FULL arms (and BASE): all pass.
> 9. Hazard sweep: none found (no ordering/interleaving hazard, no silent-
>    pass test beyond the documented-and-closed UR gap, no consumer can mix
>    macro and non-macro layouts).
>
> **P2 findings:** (1) mutation-receipt diff artifacts were empty/prose;
> (2) the "78/78" count was stale (actual 79/79 after oracle registration);
> (3) assertion-site totals miscounted (TP 16 not 18; UR 6, listed
> correctly); (4) R1's UR call-DAG line numbers 87/143 transposed;
> (5) R3's "patch is the arm's complete identity" should mention the
> test-registration component present in live worktree diffs.
>
> **Summary:** "The campaign's core concurrency claims are correct and now
> independently evidenced … The five P2 items are documentation-arithmetic
> and evidence-artifact hygiene fixes that should be folded into the
> research records before productization is considered; none blocks the
> research conclusion."

Full verbatim text: this review's complete output is preserved in the
research branch history (commit message trail) and reproduced above in
substance; the five P2 items are resolved as follows.

## Review B — Performance Methodology (verbatim, abridged to findings; full text in the branch history)

> ## VERDICT: REVISE
>
> The raw data, pairing design, binary provenance, freeze discipline and
> negative controls all check out under independent recomputation, and the
> product-level verdict (level C not reached; no performance claim for
> removal) is unchanged — indeed strengthened — by my corrections. But the
> bootstrap-CI machinery is systematically wrong at n=15 in a way that
> flips one headline causal claim, and one cited line of evidence does not
> exist.
>
> **Reproduced exactly (reviewer's own code, exact percentile-bootstrap
> intervals derived in closed form):** all medians and direction counts of
> the principal reductions, including TP d1-w1 A/B/C (0.966/1.038/1.006),
> UR W-03 C (0.99246, 12/15), attribution contrasts, stats-on runs, D8,
> W-01, and 18 of 20 R8 inventory cells within 0.1 pp. Data-integrity
> audits all pass: exactly 15 rows per (campaign, case, arm) cell, oracle
> ok on all 1080 rows, no imputation, rotation verified in raw row order,
> every row's `bin_sha256` and every provenance SHA matches the recomputed
> binary hashes, git history and timestamps prove every A/B campaign
> postdates the v3 freeze and cal5 recalibration, and the v1→v2→v3 diffs
> are exactly WARMUP and the shared tmpdir path.
>
> **P1-1.** The UR d1 BASE→ur-ctrl wall "improvement −1.3 % [0.985,
> 0.994]" is not CI-separated: exact interval [0.978, 1.009] spans 1.0
> (sign-test p = 0.118). Claim void; the W-03 ctrl-cycles leg separates
> from 1.0 but overlaps the A/A band (see P2-1).
>
> **P1-2.** The Monte Carlo bootstrap CI systematically understates
> uncertainty at n=15 (exact endpoints are order statistics v4/v12; the MC
> estimator lands one statistic tight in ~36–38 % of endpoints). Every
> separated-vs-1.0 conclusion survives the exact recomputation except
> P1-1; several are knife-edge. Required: republish all CI columns with
> the exact intervals.
>
> **P1-3.** R7 cited "not reproduced by the stats-on W-03 runs" — no
> stats=1 W-03 rows exist anywhere in the evidence tree. Cited evidence
> must be removed.
>
> **P2-1.** R8's level-B claim for UR fails the campaign's own reading
> rule (W-03 ctrl cycles overlaps the A/A band; no W-03 A/A was run — the
> band was applied by proxy). Downgrade to "suggestive, unconfirmed
> trend".
>
> **P2-2.** R5's "freq_after ≈ 3.17–3.5 GHz in every record" and
> "load < 1.5" are contradicted by the raw data (7–58 records per campaign
> below 3 GHz, min 1.20 GHz; load maxima 1.89–2.98).
>
> **P2-3.** "≈1.000"/"≈0" cells in R6/R8 understate actual point estimates
> (TP d16-w4 C is +1.2 % slower; TP W-03 C is −1.0 %).
>
> **P2-4.** The control arms are themselves code-shape perturbations
> (different encoding than `lock add`), and the TP ctrl's plain field
> access is a data race at multi-worker configs.
>
> **P2-5.** The "≈1 instruction per syscall" quantification is not
> verifiable from the A/B data alone (59 instructions/op observed;
> per-event decomposition rests on static evidence); the −0.022 % clears
> the A/A band by 0.00002 — report as barely separated.
>
> **P2-6.** Hygiene: stale WARMUP comment; declared-but-unwritten
> meta.jsonl/perf.csv; d8 temp files; d8 warmup=1 undocumented.
>
> **Judgment of the central causal claim:** the code-shape-artifact
> attribution for the TP d1-w1 swings is "sound and well supported"
> (physical bound, instruction metric, ctrl arm, reproduction across two
> campaigns, W-01 byte-identical control, D8 opposite signs); the one
> overreach was level B for UR. "For both backends every deletion-arm
> contrast is null or offset under the corrected statistics, so
> NO_CHANGE_JUSTIFIED on the performance axis, and the prohibition on
> citing performance in a removal PR, survives review unchanged."

## Resolution log

| Finding | Resolution |
|---|---|
| A-P2-1 | Real mutant hunks written to `patches/mutant-tp-m1.diff` / `mutant-ur-m1.diff` |
| A-P2-2 | Counts corrected to "all green; 78/78 at arm time, 79/79 with oracle" in R3/R4/README |
| A-P2-3 | Totals corrected (TP 16, UR 6) in R1/R3 |
| A-P2-4 | R1 UR call-DAG attribution corrected (87 = dispatch_after_accept epilogue, 143 = SQ-full) |
| A-P2-5 | R3 arm-identity paragraph rewritten (production component + shared registration component) |
| B-P1-1 | UR d1 lock-cost claim **retracted** in R7/R8; corrected attribution section written |
| B-P1-2 | `reduce_575.py` replaced with exact order-statistic CI (new self-test fixtures 6–7); all CI columns in R5/R6/R7/R8 republished from the same raw rows |
| B-P1-3 | Nonexistent "stats-on W-03" citation removed from R7; discount rests on the A/A-band rule and the cycles offsets |
| B-P2-1 | R8 level B downgraded to "not established; suggestive W-03 trend only"; W-03 A/A gap recorded as a limitation |
| B-P2-2 | R5 frequency/load statements corrected |
| B-P2-3 | Actual medians printed (d16-w4 C = 1.012; W-03 C = 0.990) in R6/R8 |
| B-P2-4 | Control-arm caveats added to R6/R7/R8 (code-shape perturbation; TP ctrl race, research-only). Closeout delta: the multi-worker TP ctrl rows are now explicitly `INVALID_FOR_CAUSAL_INFERENCE` (undefined behavior, not "benign for timing"); causal statements rest on the single-worker legs and the race-free UR control |
| B-P2-5 | Quantification caveat added in R6 ("barely separated"; decomposition rests on static evidence) |
| B-P2-6 | Runner comment fixed; dead file declarations removed; d8 temp files deleted; d8 warmup=1 documented in R7 |

No re-measurement was required: every correction is to statistics or
documentation computed from the same frozen raw data, which Review B's
integrity audit confirmed row-for-row. The verdict-relevant conclusions
after revision: mechanism elimination real; no established runtime cost;
no material end-to-end product improvement for either backend.

## Closeout delta reviews (2026-10-24, post-merge)

At issue closeout the research records received a second revision pass
(evidence-integrity repairs; no raw data touched) and two additional
independent delta reviews ran against the initial closeout revision
(before the Review-C fixes below were folded in; the final revision is
the pushed one):

- **Review C — Evidence Consistency: REQUEST_CHANGES → resolved.** Verified
  the withdrawn "~0.4–1 % UR locked-RMW cost" exists only in retraction
  contexts; every multi-worker TP plain-increment row is marked
  `INVALID_FOR_CAUSAL_INFERENCE`; code-shape findings consistently carry
  `CODE_SHAPE_CONFOUNDING_SUPPORTED` with no proven layout mechanism; the
  oracle's productization (PR #585, `1df8b4b8`) is recorded in
  README/R4/VERDICT without rewriting the historical 78/78, 79/79 counts;
  all cross-document numbers reconcile; all verdict statements agree. Four
  findings (one P2, three P3), all fixed before the push: the W-03
  proxy-calibration limitation is now stated in full in R5 (and R7 gained
  the explicit `PROPOSE_UPSTREAM` clause); R4's stale "18 sites" TP
  assertion-site count was corrected to 16 (matching R1/R3's Review-A
  correction); R4-MUTATION-RECEIPTS' re-execution claim now cites the PR
  #585 receipts; this R9 section itself was rewritten to record actual
  outcomes instead of pre-recorded ones.
- **Review D — Research Integrity: APPROVE** (no P1/P2). Verified the raw
  measurement files are byte-identical to the pre-revision tree; every
  `manifest.json` checksum matches its committed file after the revision,
  with the two never-committed `ir-rebuild-*.o` declarations removed under
  an explicit erratum (`git log --all` confirms no commit ever carried
  them); the published medians/CIs are exactly reproducible from `raw/`
  with the frozen self-tested `scripts/reduce_575.py` (all spot-checked
  contrasts, including the five decision-bearing ones); the
  `INVALID_FOR_CAUSAL_INFERENCE` marking matches the attribution
  campaign's actual configs (d1-w1 single-worker valid; d16-w8 and W-03
  d16-w4 multi-worker UB) and the patch's race semantics (sole writer
  `worker_loop`, sole reader the test seam); the baseline identity
  (`3df10dc4`, GCC 16.2.1, liburing 2.13, E5 host) is unchanged and the
  revision is a linear one-commit append on `f209ce88`; no production
  code, script or raw file was touched.

No blocking finding remains. PR #584 remains OPEN and unmerged by design
(research evidence only).
