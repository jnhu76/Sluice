# #595 R5-repair — three-arm measurement round (r5-repair-measure)

Executed 2026-11-06/07 (UTC) on the native E5 host, per the user task
"R5 修复与三臂实测执行" (EXECUTE round). Scope: repair the #630 audit
items (F1–F6), re-verify C1/C2 equivalence, run the frozen three-arm
experiment A0 / R_original / R_cleanup, an isolated fixed-instrument
diagnostic, and issue the verdicts. No policy changes, no candidate
implementations, no product merges; research worktrees only.

## One-paragraph outcome

The v1 tail-trace instrument's data races were confirmed and fixed
(per-writer buffers + quiesce-gated export, TSan-validated against a
negative control), C1/C2 equivalence was re-proven (18-scenario oracle
byte-identical, 5,720-span bounded-scan check), and the formal
three-arm matrix ran complete: 5 shapes × (24 matched blocks + 24 A/A
pairs + calibration), 712/712 valid invocations under the dual
measurement lock. **The product gate still fails on p99 only**: on
W03-d16, R_cleanup gives CPU/op −22.09% [−22.75,−21.41] and throughput
+28.49% (both PASS) with p95 +1.15% (PASS) but p99 +17.97%
[+15.18,+20.82] (FAIL, Holm p=1.0); W03-d4 passes every protection
bound, while W03-d2serial / W02-d1 / W02-d2 keep the shallow-queue
adapter tax (FAIL). The cleanup itself is never harmful after
binary-layout calibration and yields small real CPU savings at
d4/d2serial (≈−2%); its apparent −1.8% p99 gain on the main shape is
within the A0-original/A0-cleanup calibration band and is not claimed.
The repaired instrument still fails the ±3pp on/off perturbation
window, so tail attribution remains lead-level (back-of-refill
conditional tail risk ≈5× confirmed under the fixed instrument, marked
PERTURBED). Single recommended next step: stop cleanup-class work on
this line; if it continues, run the pre-designed Candidate B
(residue-aware submission budget) on top of R_cleanup under the same
gate caliber.

## Files

| Path | Content |
|---|---|
| `BASELINE.md` | host identity, frozen objects/binary SHAs, lock discipline |
| `CORRECTIONS.md` | F1–F6 verdicts with frozen-source evidence; PR-#630 wording corrections applied |
| `PREREGISTERED.md` | frozen plan (sha256 `7a412133…`), statistics validated on known samples pre-freeze |
| `RESULTS.md` | all contrasts, gates, A/A, calibration, resources, diagnostic round, verdicts |
| `analysis/` | `round-analysis.json` (bootstrap CIs/bounds/gates), `diag-perturbation.json` |
| `figures/` | F1–F5 (SVG+PNG) + `figure-data/*.csv` — every value data-derived |
| `harness/`, `scripts/` | measurement machinery, stats (validated), figures, checker, TSan harness |
| `raw/<cell>/{aa,main,calib,rssupplement}/results.jsonl` | per-invocation records (reps embedded); checker recomputes contrasts from these |
| `raw/equiv/` | this round's three-state oracle/suite outputs |
| `raw/diag/` | diagnostic on/off invocation records |

E5-local only (registered, not in this archive): per-invocation bench
jsonl/log trees, the v2 dump binaries `raw/diag-dumps/` (~863 MB) and
the joined `raw/diag-ops.csv` (276 MB), plus the round worktrees'
build outputs — under `/home/jnhu/sluice-595-e5/r5-repair-measure/`
(sha256 manifest: `MANIFEST-e5.txt` in the round directory on the
host). A summary-only copy cannot recompute per-request diagnostic
results; the dumps can, via `scripts/analyze_tail_trace_v2.py`.

## Relationship to PR #630

This round corrects the Phase-A archive in place (F2/F4/F6 — see the
edited files under `../595-r5-phase-a/` and `CORRECTIONS.md` here) and
backfills the 68 files the Phase-A EVIDENCE-INDEX claimed but the
reviewed head lacked (`raw-diag/scripts/`, `raw-diag/equiv/` — the
phase-a tree now carries all 181 non-binary E5 files). Frozen R4/R5
preregistrations and raw data were not modified.
