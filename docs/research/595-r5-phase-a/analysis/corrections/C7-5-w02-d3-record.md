# C7-5 — W02-d3 missing case: harness gap (not a hardware limitation)

Status: record for R5 planning. No matrix expansion this round; R4 archive
unchanged. Verified 2026-11-06.

## Verified facts

1. The R4 task book's core matrix said "W02 d1/2/3/16"; the frozen W02
   object set has no d3 case. This was disclosed BEFORE the runs:
   `r4/PREREGISTERED.md:177-180` (§2.1, frozen): "任务书原文核心矩阵为
   'W02 d1/2/3/16'。冻结对象集内 **W02 无 d3 …**；W02-d3 记
   `NOT_RUN_WITH_REASON: no frozen W02 d3 case`；最近的冻结深度 3 形状是
   W03 `r3d3`（本矩阵未列入门槛，如跑只能作描述性补充）。W02-d64 存在于
   冻结对象集…"
2. `r4/RESULTS.md:335` records exactly that: W02-d3 NOT_RUN_WITH_REASON
   "no frozen W02 d3 case (prereg 2.1)".
3. Harness source (r5 worktree = frozen R4_ORIGINAL):
   `bench/bench_w02_pipeline.cpp` adds uring W02 cases at depths 1, 4, 16,
   64 (`:916-919` list: add_uring(4096,20000,{1,4,16,64},false)); depth 2
   exists only behind the opt-in `SLUICE_BENCH_595_DEPTHS` env
   (`:930-935`, comment: "shallow-depth control D2 absent from the frozen
   G0 list"); there is NO depth-3 case anywhere in the file. The W03
   harness DOES have a depth-3 shape: `bench_w03_external_loop.cpp:857`
   `add_r3(4096, 20000, 3, 3, "-r3d3")` (different workload: external
   loop, not the bounded RequestScope pipeline).

Conclusion: W02-d3 is absent because the frozen case set never defined it
(harness/case-set omission), NOT because the host cannot run it — the same
binary family runs W02 at depths 1..64, and nothing in the W02 pipeline
code special-cases depth 3.

## Is backfilling worth it? (assessment for a future round; not done now)

Arguments against priority:
- The neighboring points already FAIL: W02-d1 6/12 and W02-d2 6/12 and
  W02-d16 8/12 protective members crossed bounds (RESULTS.md:82-88,429).
  A d3 point sits between two failing regions; it is very unlikely to
  change any R/S/Q8 accept decision.
- The nearest depth-3 shape (W03 r3d3) exists and is runnable today as a
  descriptive supplement without touching the frozen W02 case set.

Arguments for (weak):
- Mapping the d2→d16 transition curve of the pipeline workload would need
  intermediate depths; d3 is the first point on that curve.

Recommendation: do NOT expand the frozen matrix for W02-d3 on its own. If
a future hypothesis specifically targets the shallow-depth transition of
the W02 pipeline, preregister a small descriptive sweep (d3, d8) next to
the existing W03-r3d3 descriptive slot, and state in the prereg that it is
supplementary (same rule PREREG §2.1 already applies to r3d3). Resource
note: each added W02 depth adds 4 arms × 24 blocks of measured time plus
an A/A cell — cost must be justified by the hypothesis, per the frozen
matrix rules.
