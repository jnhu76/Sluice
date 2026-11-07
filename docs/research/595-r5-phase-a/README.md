# #595 R5 Phase A — Call-Pattern Visualization, Code Fixes, and p99 Localization (research archive)

Round R5 Phase A of the #595 submission-policy research line, executed on the
native E5 host 2026-11-06/07. Predecessor: R4 state-aware experiment, archived
in PR #629 (`docs/research/595-r4-state-aware/`, still open at archive time).
This round is analysis/fix/design only — **no Phase-B scheduling experiment was
executed, no product code was merged**; it stops at `READY_FOR_JOINT_ANALYSIS`
per the round contract.

## What this round established (one paragraph)

The policy's only real scheduling control is the presentation-loop prefix
length plus its re-entry rhythm (`bench/r3_ready_batch.hpp:442-576` at frozen
`f3b25329`); chunk boundaries are not yields (C5) and A0 does not even go
through the adapter on W03. All seven audited code findings C1–C7 were
CONFIRMED; the two provably-equivalent mechanical fixes (C1 bounded
eligible-prefix scan `c55165a2`, C2 owner-pass-activity rename `b191bb89`)
were implemented on branch `research-595-r5` and proven behavior-identical
against the R4 18-scenario deterministic transport oracle (byte-identical
outputs across baseline/C1/C2) plus the full in-repo regression set. A
pre-registered 7-timestamp tail diagnostic ran under the measurement lock and
produced complete 480,000/480,000 joined traces, but **failed its frozen
on/off perturbation window (+3.39/+3.96 pp > ±3 pp), so the tail gap is not
formally attributed**; at lead level it localizes R's d16 tail to
submission-side serialization of restart refills (five-interval decomposition:
+20.2/+3.2/−3.9/−0.2/−14.5 µs, net +4.9 µs; the d4 sign flip −2.29 µs
reproduces under instrumentation). Three candidates (A lighter execution
chain, B residue-aware submission budget, C prepare/observe decoupling) are
designed field-by-field in `NEXT_EXPERIMENT.md`; the single recommended next
experiment is **Candidate A on top of the verified cleanup**, under the R4
complete gate caliber.

## File map

| Path | Content |
|---|---|
| `CALL_PATTERNS.md` | G1–G5 execution graphs (Mermaid source, node index `f3b25329:file:function:line`, verified vs hypothesized edges, this round's problems marked on the figures) |
| `CODE_FINDINGS_AND_FIXES.md` | C1–C7 verdicts (all CONFIRMED), fix/clarify/candidate classification, equivalence evidence |
| `TAIL_ANALYSIS.md` | Evidence inventory, corrected literature mechanism matrix (IX/ZygOS/Shinjuku/Shenango/Perséphone/NAPI), open p99 sources, R4 failure classification |
| `NEXT_EXPERIMENT.md` | Candidates A/B/C field-by-field, recommendation, Phase-B execution contract (design only) |
| `EVIDENCE-INDEX.md` | sha256 manifest of every artifact + the revision pass record (27 independent-review items fixed) |
| `evidence/pasteBlock.txt` | The `BEGIN_SLUICE_595_R5_PHASE_A` machine-readable handoff block |
| `evidence/cleanup-c1.patch`, `evidence/cleanup-c2.patch` | The two R4_CLEANUP commits (also on branch `research-595-r5`: `c55165a2`, `b191bb89`, base `f3b25329`) |
| `evidence/equivalence-c1-c2.md` | Three-state equivalence method and results |
| `analysis/` | All six investigation tracks' working reports (tracers, C1–C4, C5–C7, literature, evidence review) + frozen `diagnostic-protocol.md` (pre-collection) + `diagnostic-results.md` + `corrections/` (C7-1..6) |
| `raw-diag/` | Lock receipts, diagnostic manifests/summaries, per-run logs (non-binary), the `equiv/` three-state oracle outputs, session scripts |
| `r4-archive/TAIL-LATENCY.md` | R4 TAIL-LATENCY.md (branch-only in R4; extracted for self-containment) |

Not committed (kept on the E5 host under `/home/jnhu/sluice-595-e5/r5/`):
the per-run arena dump binaries (`raw-diag/matrix/**/dump/*.bin`, ~82 MB, and
`raw-diag/smoke/*.bin`) — binary trace arenas whose parsed forms are
`onmode-trace-analysis.json` / `aggregate-summary.json` in this archive.

## Branches

- `research/595-r5-phase-a` (this archive; docs only)
- `research-595-r5` — R4_ORIGINAL `f3b25329` + R4_CLEANUP `c55165a2` (C1) + `b191bb89` (C2)
- `research-595-r5-diag` — diagnostic instrumentation commit `21374a40` on `f3b25329` (diagnostic-only build, never mixed with formal timing)

## Post-review update (r5-repair round, 2026-11-07)

- **Backfill (F6):** `raw-diag/scripts/` (4 files) and `raw-diag/equiv/`
  (64 files) are now present — the EVIDENCE-INDEX §4 listing was accurate
  about the E5 originals but the reviewed head `828b10b4` did not carry
  them. All 181 non-binary `raw-diag` files are now in-tree; the ~82 MB
  `.bin` arena dumps remain E5-local as declared below.
- **Corrections (F2/F4):** the frozen "≥50% of the p99 gap" attribution
  rule is abolished (quantiles are not additive); the "driver cannot
  publish mid-group", "truly parks", "sole inflight authority" and
  "kernel straggle ruled out" wordings are corrected in
  `TAIL_ANALYSIS.md`, `NEXT_EXPERIMENT.md` and `CALL_PATTERNS.md` with
  dated markers. Full inventory: `../595-r5-repair-round/CORRECTIONS.md`.
- **F1:** the v1 diagnostic instrument in commit `21374a40` had real
  data races (plain `g_cursor`/record fields read by the driver outside
  the dispatch lock, phase reset/export vs in-flight writers, 64→32-bit
  generation truncation, positional built→admitted pairing misalignment
  under front rejection). Fixed and validated in the repair round; v1
  traces remain frozen evidence under the v1 analyzer.
- The repair round's three-arm measurement (A0 / R_original / R_cleanup)
  and verdicts: `../595-r5-repair-round/` (product gate still fails on
  p99 only; the repair round's own post-review revision corrected five
  analysis/reporting defects — protection-gate family and one-sided
  bounds, F5 field semantics with true-position recovery, calibration
  wording, instrument/analyzer failure paths; see that directory's
  CORRECTIONS.md "Review corrections"). Candidate B carries a dated
  execution warning in NEXT_EXPERIMENT.md (T0 re-stamping changes the
  latency origin; not green-lit).
