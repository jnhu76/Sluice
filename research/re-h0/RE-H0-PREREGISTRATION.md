# RE-H0 PREREGISTRATION — frozen before formal measurement

Campaign #277 (authority #227, Host-0). This document is the frozen
protocol. Formal measurement starts only after this file and the analysis
module (with its diagnostics green) are committed. No threshold, cell,
rep count, metric or ordering rule may change after formal data exists.
Feasibility probes (RE-H0-AUDIT.md §5) preceded this freeze; nothing in
this file was chosen after seeing formal numbers.

## P1. Question

After removing already-known implementation taxes (router tax #274,
#258 cleanup), how close is Sluice to a competent hand-written
semantic-equivalent io_uring / ThreadPool floor on Host-0, where does any
remaining tax live, and in which tested regimes is it PARITY vs
MATERIAL_TAX vs GRAY under the frozen vocabulary?

## P2. Decomposition authority (inherited, frozen)

```text
CAPABILITY COST           C_sem     = Z1b  / Z1
BACKEND ABSTRACTION TAX   T_backend = Z2   / Z1b
CONTINUATION COST         C_cont    = Z1bw / Z1b
RUNTIME/MEDIATION TAX     T_runtime = Z3   / Z1bw
THREADPOOL EXECUTION COST T_pool    = L1   / L0
SLUICE POOL INCREMENT     T_sluice  = L2   / L1
```

`Z1 -> anything` is NOT a valid tax measure. Ratios are candidate/baseline
medians; absolute per-op deltas are reported alongside. `Z3 / Z1bw` is a
matched rep-envelope comparison (RE-H0-AUDIT.md §3.1 boundary note), not a
claim of identical substrates.

## P3. Cells (RE-1U and RE-1)

| cell | request size | depth | workers | useful bytes / rep | rationale |
| --- | --- | --- | --- | --- | --- |
| S | 4 KiB | 8 | 1 (z3) / d (e1) | 256 MiB | historical TAX/Z-ladder canonical; control-plane tax visible |
| L | 2 MiB | 2 | 1 (z3) / d (e1) | 1 GiB | CHUNK-E0 Host-0 engineering sweet-region representative |

No mid cell is preregistered. If attribution later needs the S→L
transition point, 64 KiB × d4 may be added ONLY as a separately
preregistered attribution diagnostic, never retroactively into this
matrix.

## P4. Arms

- RE-1U: `z1 z1b z1bw z2 z3` (tax0_z_ladder_bench, unchanged).
- RE-1: `L0 L1 L2` (e1_abstraction_tax_bench, unchanged), `--workers = depth`.
- Operations: READ and WRITE only. No fsync, O_DIRECT, registered buffers,
  fixed files, SQPOLL, splice, copy_file_range (RBUF-E0 and ALIGN-E1 are
  closed as NOT MATERIAL; their knobs stay out).

## P5. Filesystem regimes

- Primary: btrfs on the SATA SSD (`build/re-h0-data`), compress=zstd:1.
- Control: tmpfs (`/tmp/re-h0-data`) — storage-latency/control-plane
  sensitivity control. tmpfs results NEVER support a real-I/O claim.

## P6. Repetition and launch protocol (frozen)

- W launch: `--reps 11 --warmup 2` — 11 wall/user/sys per-rep samples.
- perf launches: TWO independent (R=7, R=14) double-difference pairs,
  `--warmup 2`; per-rep work = (total(R14) − total(R7)) / 7; two
  independent instruction/op estimates per combo.
- Every launch pinned `taskset -c 2-9`; `LC_ALL=C`.
- Ordering: blocked-interleaved — per (fs, cell, op) block the arm order
  is shuffled with frozen seed `20260903` (recorded in the manifest);
  an arm's launches stay adjacent within its slot.
- WRITE arms: `sync` + 0.3 s settle after every write launch; durability
  policy is buffered writeback only (no fsync), identical everywhere.
- perf events: `instructions:u`, `cycles:u`. If `instructions:u` proves
  unreliable, the campaign is INDETERMINATE for instruction claims (never
  silently demoted to wall-only); cycles may be DEMOTED by probe evidence.
- Session layout per RE-1U / RE-1: `environment.json`, `manifest.json`
  (every launch + rc + arm order), `raw/` verbatim, `summary.json/.csv`,
  `gates.json` (qual), `analysis.json` — O(10) files, no per-run dirs.

## P7. Metrics (frozen priority)

1. instructions/op (double-difference, two independent estimates) and
   instructions/byte — primary.
2. wall/op (median of 11), throughput/IOPS, CPU user/sys.
3. cycles/op (demote if probe-unstable), RSS (e1 arms emit `maxrss_kb`;
   z arms do not — recorded as not-emitted), context switches NOT
   collected (no reliable per-combo source; declared, not hidden).
Tail percentiles: NOT collected (no tiny-sample histograms).

## P8. Materiality vocabulary (frozen before measurement)

For every ratio R = median(cand)/median(base), with robust intervals
median ± 1.5·MAD of the 11 wall samples:

```text
PARITY        R <= 1.05 AND no strong interval separation AND every
              instruction estimate <= 1.05
MATERIAL_TAX  R >= 1.10 AND strong interval separation AND every
              instruction estimate >= 1.10
GRAY          everything else, including any R < 1.0 (direction anomaly,
              reported, never folded into parity)
```

"Strong separation" = disjoint intervals. Fully degenerate constant
series (MAD = 0 both sides) defer to the ratio band alone (no variance
evidence either way); the analysis module implements exactly this and its
diagnostics pin the behavior (`check_re_h0_analysis.py`, 26 cases).
Instruction estimates gate the verdict so a claim can never quietly
degrade to wall-only: missing estimates ⇒ `IndeterminateMetric`.

## P9. Decision tree (automatic, frozen)

- CASE A — T_backend PARITY and T_runtime PARITY:
  `RE-1U HOST-0: PASS — NEAR SEMANTIC FLOOR IN TESTED CELLS`. No
  optimization. Proceed to RE-2.
- CASE B — T_backend MATERIAL_TAX: backend residual tax exists ⇒
  preregister `RE-1U-ATTR-B` causal decomposition (census → rank → ONE
  causal ablation → remeasure; router slope #274 is CLOSED and out of
  scope).
- CASE C — T_backend not material, T_runtime MATERIAL_TAX: runtime/
  continuation residual ⇒ `RE-1U-ATTR-R`.
- CASE D — both material: separate attributions; NEVER one mega-fix.
- CASE_GRAY — no material residual but not clean parity: report
  per-component verdicts; no optimization authorized by gray.

Attribution may select a research-only candidate under frozen rules; a
production fix is OUT OF SCOPE for this campaign without prior complete
fix-selection authority (#255 discipline) and human adversarial review.

## P10. #262 stop law (frozen)

Pre-measurement gate (`re_h0.py qual262`): perf-wrapped Z2/Z3 ×
READ/WRITE × cell S and L, N = 20 launches each (160 total). Requirement:
0 unexpected -ECANCELED, 0 named drain stall, 0 wait error, 0 teardown
failure, 0 same-work mismatch. During ALL formal work, any such event in
any cell ⇒ CELL INVALID / CAMPAIGN PAUSE, exact evidence preserved to the
session `gates.json` and posted to #262. No retry-until-clean, ever.

## P11. RE-2 initial envelope (frozen shape; frozen cells now)

Axes (representative cells, NOT the cartesian product):
`4K×d1, 4K×d8, 64K×d2, 2M×d1, 2M×d2` × READ/WRITE ×
{Z1b vs Z2 (uring ladder), L1 vs L2 (pool ladder, W = depth)} ×
btrfs primary + tmpfs diagnostic. Same protocol (P6), same vocabulary
(P8). Mechanism comparisons (Sluice-ThreadPool vs Sluice-Uring) are
reported as MECHANISM facts, never as abstraction tax. RE-2 stops when
these cells answer "residual tax stable / not stable across Host-0
regimes" — no matrix growth for optics. Z-value zones use #227
vocabulary (Z1 RAW WINS / Z2 PARITY BREADTH / Z3 CROSSOVER / Z4 EXPLICIT
VALUE); this campaign classifies Z1/Z2 performance zones only, no
fabricated Z4 claims.

## P12. Claim vocabulary (frozen)

Only HOST-LOCAL claims: `HOST-LOCAL PARITY / NEAR-FLOOR / MATERIAL TAX /
MIXED (on Host-0, tested cells)`. Forbidden without an explicit Host-0
limitation: "near-native", "matches native io_uring", "Linux parity".
General G1-PERFORMANCE: NOT YET ADJUDICABLE (second machine class, modern
NVMe, ARM64 unavailable; #270 DEFERRED / NOT EXECUTED, stays OPEN).

## P13. Analysis authority

All medians, MADs, ratios, verdicts and cases are computed by
`scripts/re_h0_analysis.py` from session `summary.json`; this document
and RE-H0-REPORT.md never act as data authority. Diagnostics:
`python3 research/re-h0/scripts/check_re_h0_analysis.py` (26 cases,
red-green authored before implementation). Analysis refuses to aggregate
any session whose manifest records failures, any block failing
fail-closed validation (missing/duplicate arm, failed rep, unverified
write, word_sum mismatch, binary sha mismatch, any error text), and any
missing instruction estimates (IndeterminateMetric).

## P14. Adversarial self-review checklist (applies to the report)

Z1 semantics < Z1b? Z1bw/Z3 continuation obligations comparable? Z3 span
extras declared? queue depths identical across arms? buffers identical?
validation identical? tmpfs leaked into real-I/O claims? 2M×d2 treated as
universal? ECANCELED retried away? stale evidence reused across changed
code paths? cells added after results? Host-0 converted into Linux/x86
claims? missing second host treated as success? — any YES ⇒ fix the
report or downgrade the verdict.
