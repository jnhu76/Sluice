# C7-4 — "39 protected members failed" ≠ 39 independent, significant regressions

Status: wording correction for R5 synthesis; R4 archive unchanged.
Verified 2026-11-06 against the frozen gate data.

## What the frozen data says

`r4/RESULTS.md:80` (§4) is already precise at the definition level:

> FAIL 只由点估计/上界越界定义，Holm 不显著故不能记 PASS，但也不等同 FAIL

and `RESULTS.md:95`: "FAIL 成员（点估计或单侧上界越界；p 均 ≥ 0.243，因此没有
任何 FAIL 成员依赖 Holm 拒绝——失败由界本身判定）".

`r4/analysis/MAIN_SUMMARY.json` `gates.F_PROT.members` (84 members) —
verified this session:

- 39 members class `FAIL_BOUNDS`; each has `point_ok=false` (mean > tol)
  and/or `upper_ok=false` (one-sided 95% upper > tol); `holm_reject=false`
  for all of them; member p values are the noninferiority p (H0: mean ≥
  tol) and are large (sample: CORE-W03-D2SERIAL/Q8/cpu p=1.0; minimum
  across FAIL members 0.2432 per RESULTS.md:468).
- 4 members class `NOT_ESTABLISHED`: `point_ok=true` AND `upper_ok=true`
  (WITHIN bounds) but `holm_reject=false` — insufficient evidence to
  certify PASS, NOT any indication of regression (sample:
  CORE-W02-D1/Q8/cpu mean +3.88%, upper +4.58% ≤ +5%, p=0.0051 < α but
  Holm-rank-adjusted threshold 0.00119 not met).

## The three distinctions the summary wording must carry

1. Gate violation vs statistical proof. The 39 FAILs are BOUND VIOLATIONS
   under the frozen rule (point or one-sided upper beyond the protective
   limit). None of them carries a Holm-rejected significance claim; by
   construction the FAIL class never depends on Holm rejection. Saying
   "39 members failed the gate" is correct; saying (or letting a reader
   infer) "39 significant regressions were proven" is not.
2. Members are not independent demonstrations. The 39 are
   (cell, arm, metric) tuples over only 7 cells, 3 arms and 4 metrics,
   sharing the same 24 matched blocks per cell and heavily correlated
   across metrics and arms (CORE-W03-D2SERIAL contributes all 12 of its
   members; CORE-W02-D16 contributes 8). Effective independent evidence
   clusters ≈ per-cell/per-arm-run, not 39.
3. NOT_ESTABLISHED (4) is evidence insufficiency on the SAFE side —
   in-bounds but not certifiable — and must never be counted toward, or
   phrased as part of, the regression tally.

## Corrected summary phrasing (before/after)

Before (acceptable but compressible to misreading — RESULTS.md:14, :358,
:457 style):

```
F-PROT has 39 bound FAIL members (4 NOT_ESTABLISHED)
F-PROT 39 个成员界越界
```

After (recommended for R5 summaries):

```
F-PROT: 39 of 84 (cell, arm, metric) members crossed their protective
bounds (frozen rule: point estimate or one-sided 95% upper beyond the
limit; no significance requirement attaches to FAIL — no FAIL member is
Holm-rejected, min p = 0.243). These 39 are correlated tuples from 7
cells sharing matched blocks, not 39 independent proofs of regression.
4 further members are within bounds but lack Holm significance to be
certified PASS (NOT_ESTABLISHED = insufficient evidence, not regression).
```
