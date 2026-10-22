# MW-HW-1 Review B — Performance / Attribution (independent)

Reviewer: isolated adversarial agent; recomputed from `rows.jsonl` (648 rows
verified), strace censuses, raw `perf script` re-parse of both perf.data
files, nm symbol extents, the runner, and Sluice sources.

## Findings (condensed; severities theirs) and in-round dispositions

- **F1 (P2) Rank-paired intervals were artifacts; "whole CI below 1.0" false
  under true temporal pairing** (64k cell has one rep at 1.017; e3's sorted
  range proved the method unsound). Survives: rep-paired medians 0.858 /
  0.731 / 0.917; Wilcoxon p = 0.0039 / 0.0039 / 0.0078; null cells p ≥ 0.43.
  → Fixed: performance.md now uses rep-paired statistics + exact tests +
  bootstrap CI; all "whole interval" phrasing removed.
- **F2 (P2) Kernel leakage under cycles:u is 22.9% (base) / 53.5% (E1), not
  ~5.9%; "41% of user cycles" used the wrong denominator — true user-only
  scan share ~55.4%**; E1-arm `dispatch_one_locked` is ~10.7% user-only. The
  vanish claim survives and strengthens (scan PCs 1203→0; non-scan code
  persists; sample-count cross-check 3000−1203=1797≈1794). → Fixed:
  syscall-census.md §4 renormalized; all dependent documents updated.
- **F3 (P2) "user −61%, system unchanged" had no receipts, contradicted its
  own table (+14–16% system), and the magnitude was one of three inconsistent
  instrument readings.** → Fixed in-round: 7-pair interleaved `time -v` rerun
  with receipts (`time-v-paired.txt`): user ratio 0.438 [0.294,0.471] 7/7;
  system ratio 0.983 [0.967,1.055] — no detectable paired change (the draft's
  +14–16% was sequential noise). Magnitudes restated with uncertainty.
- **F4 (P2) W1 "2 scans/op" wrong at 4/6 configs; C-sweep reconciliation hid
  a 34% (D=1) and ~2x (D=8) gap.** → Fixed: per-config scans/op from the
  rows' `poll_calls` counters (2.48/2.64/1.31/1.04/2.81); C-sweep reframed
  as D=1-only order-of-magnitude with both gaps quantified.
- **F5 (P3) Census nits**: 1.00005 not 1.000 (one teardown enter per phase);
  "read(eventfd)" rows are process-startup artifacts; **the zero-op census is
  invalid** (the harness rejected block=0; the 176 syscalls were startup).
  → Fixed: enter/op arithmetic corrected with the per-phase decomposition;
  read rows relabeled; zero-op census retracted — the zero-op statement is
  now explicitly source-derived + suite-covered only (path-dag §5).
- **F6 (P3) E2/E3 "≈1.00" must be resolution-bounded** (e2 1MiB
  ratio-of-medians 1.16 from 500-op runs; a ~10% regression is not
  excluded). → Fixed: "no detectable win at the ≥5–10% noise floor;
  regressions up to ~10% at short-run configs not excluded."
- **F7 (P3) Numeric hygiene**: C-sweep values are means not medians;
  CPU/wall deviation ≤0.0066 not 0.005; sample counts 3,892/3,856.
  → Fixed throughout.
- **F8 (note) Harness overhead separation adequate**: arm-invariant harness
  work cannot contaminate any ratio/delta claim; vdso stable across arms.
- **F9 (note) Failed attacks recorded**: rotation correct; rep-1 cold outlier
  moves medians ≤0.005; joint C-signature probability ≈1.5e-6; symbol
  extents verified — the E1 result is robust.

## Verdicts (reviewer's)

- Claim 1 (E1 wall ratios / C-scaling): **SOUND_WITH_REQUALIFICATIONS**
  (qualifications = F1 presentation fixes, applied).
- Claim 2 (user/system split): **UNSOUND as stated** → superseded in-round
  by the paired rerun; the salvageable core (large user drop, no system
  drop, identical census) is what the documents now claim.
- Claim 3 (syscall census 1.000 enter/op, park cold): **SOUND**.
- E1 PROPOSE_UPSTREAM recommendation: **SOUND_WITH_REQUALIFICATIONS** — all
  four requalification items (F1–F4) were applied to the deliverables in
  this round; the native-Linux + W-03 gates remain owner work.
