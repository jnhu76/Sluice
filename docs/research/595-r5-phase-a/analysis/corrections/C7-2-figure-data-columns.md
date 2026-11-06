# C7-2 — figure-data.csv segment-index / unit correction record

Status: correction record + normalized copy. The frozen R4 artifact is NOT
modified. All checks run on 2026-11-06.

Frozen input: `/home/jnhu/sluice-595-e5/r4/figures/figure-data.csv`
(370 data rows, sha256 `c2c3294b9f8b124914602d16c9f93f178aafb14be2fb3b98e3f581aae02c8096`
— the same sha RESULTS.md:480 records for the post-§17.2-correction file).
Generator: `r4/scripts/make_figures.py`.

## Step 1 — numeric verification (this session, command output)

Cross-checked every one of the 370 rows against
`r4/analysis/MAIN_SUMMARY.json` (F1 contrasts, F2 awareness row/quantum
shares and counterfactual present means, F3 contrasts incl. R-B via
cells[B-SUPP-W03-D16], F4 raw_means, F5 state, F5 segment_cpu via
cells[TRAJECTORY].segments): **zero value mismatches**. R4's own §17.2
corrections (F5 T1/T2 rows previously copied T3's values via `ready[-1]`;
F5 quantum2_share previously stored percent values under `unit=fraction`)
are confirmed applied in the frozen file: T1 rows read 1.0/1.0/0.0, T2
8.3617…/2.0425…/0.98936…, T3 7.9233…/2.0003…/0.99973…
(figure-data.csv:336-344), matching MAIN_SUMMARY awareness exactly.

## Step 2 — remaining structural defects (index/unit), verified

P1 `cell` column is not a cell identifier in two figure families:
- F1/F4 use cell names (`MAIN-W03-D16`, `CORE-W02-D1`, …).
- F3 uses the CASE string `W03-uring-4k-d16` for BOTH source cells: the six
  MAIN contrasts and the B-SUPP contrast `R-B` (make_figures.py:231 writes
  `cell["case"]`, and per `r4/analysis/per_cell_table.md:75-76` both
  MAIN-W03-D16 and B-SUPP-W03-D16 have case `W03-uring-4k-d16`). A reader
  cannot tell that the R-B rows come from a different cell (different
  session, descriptive-only status).
- All 96 F2/F5 rows leave `cell` empty; the F5 segment_cpu rows' source
  cell is `TRAJECTORY` (the 0/24-INCOMPLETE cell), which is only recoverable
  from the note string "INCOMPLETE descriptive", not from any index column.

P2 `arm` column is overloaded: F3 and F5(panel=segment_cpu) write contrast
labels (`SHADOW-R`, `S-R`, `R-A0`, `R-B`, …) into `arm`
(make_figures.py:245-253, 358-366); F2(panel=present_size) writes
arm=`SHADOW` for `actual_R_present` (data-source run) but arm=`S` for
`counterfactual_S_present` whose data source is also the SHADOW run
(make_figures.py:179-197) — action-owner vs data-source flip inside one
panel. 69 rows carry a non-arm value in `arm`.

P3 `segment` column semantics: empty for F1/F3/F4; holds the awareness
dataset name (`W03-D16`,`T1`,`T2`,`T3`) for F2 and the TRAJECTORY phase for
F5 — i.e. it moonlights as the index column that `cell` fails to be, while
for F5(b) T1/T2/T3 additionally denote phases of one case (awareness
T1/T2/T3 come from `TRAJECTORY__S__T{1,2,3}.diag`,
matrix_analysis.py:962-967), not independent cells.

P4 `n` unit silently switches: 24 = matched blocks for F1/F3/F4/F5(b);
94..60000 = decision-line counts for F2/F5(a). Same column, two
populations, no marker.

## Step 3 — correction emitted (minimal, non-destructive)

`figure_data_normalize.py` (this directory) reads the frozen CSV and writes
`figure-data.normalized.csv` (sha256
`64ce3b463bcabad64bbaa491d55926801739a4eb4a25769282a219c6d4982b17`),
adding `source_cell`, `contrast`, `dataset`, `n_unit` columns; every
original column/value kept byte-for-byte. Run output:

```
rows=370 cell_corrections=42 arm_column_disambiguations=69
```

(42 = all F3 rows re-pointed to their true source cell; 69 = the contrast
labels moved out of `arm`.) No figure image changes: the plots consume
MAIN_SUMMARY directly (make_figures.py), and all values were already
consistent; this correction is index/metadata only, hence no new tests
(per round rule: no tests for pure wording changes).
