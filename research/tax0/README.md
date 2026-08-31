# TAX-0 — Hot-path data-structure / synchronization topology audit

Research material for issues #227 (roadmap), #249 (paper ledger), #250
(TAX-0 audit). TAX-0A is a READ-ONLY static audit plus native
measurement-readiness check — no production code was modified and no
benchmark sweep was run. Frozen 2026-08-30 after human adversarial review
of #251 (freeze corrections FC-1..FC-7: report §18 / JSON `freeze` block);
this freeze commit is the preregistration baseline for TAX-0B/C — the
hypotheses here were registered BEFORE any causal experiment.

- `TAX0-A-HOTPATH-TOPOLOGY-AUDIT.md` — human report
- `TAX0-B-EXP0-CAPACITY-INVARIANCE.md` — EXP-0 measurement report (PR
  #253; verdict: capacity-dependent tax MATERIAL on the uring arm,
  ThreadPool capacity-invariant; canonical evidence in
  `docs/results/performance-attribution/tax0b-exp0-*.json`, kind
  `tax0capacity`)
- `TAX0-EXP-U0-ROUTER-CAUSALITY.md` — EXP-U0 causal-attribution report
  (PR #254; verdict: ROUTER CAUSALITY STRONGLY SUPPORTED — reversing only
  the `find_live_router_cookie_` scan direction removes ~100% of the
  measured capacity slope; canonical evidence in
  `docs/results/performance-attribution/tax0u0-*.json`, kinds
  `tax0u0router`/`tax0u0witness`; no production optimization implemented,
  no production fix selected — design selection deferred to #255)
- `TAX0-ROUTER-PRIOR-ART.md` — T0-U-ROUTER (#255) prior-art survey:
  user_data→completion resolution contracts across liburing, fio, Tokio,
  tokio-uring, Monoio, Glommio, Seastar, Boost.Asio; three families
  (raw pointer / recycled index / no-wrap key) compared as contracts,
  not as recommendations
- `TAX0-ROUTER-FIX-SELECTION.md` — T0-U-ROUTER (#255) fix-selection
  report: candidate admission (R0–R4, R4 shapes rejected pre-benchmark
  for stale-CQE/ABA contract weakening), frozen shootout methodology,
  official campaign results, §25 mechanical winner selection, and the
  §31 verdict (ROUTER SHOOTOUT PASS — PRACTICAL TIE, SIMPLEST
  CANDIDATE SELECTED (R1)); no production fix implemented
- `TAX0-ROUTER-REFREEZE-A2.md` — corrective-review re-freeze (PR #256
  review 5063072823): Layer A instrument correction only (per-window
  allocation removed + measured allocation gate, `--seed` 0x-prefix
  parser), validator sealing to the frozen matrix (cell/candidate/
  session deletion fails closed), micro cycles cross-check, §25 selector
  direction fix; Layer B not re-measured
- `TAX0-ROUTER-OPTIMIZATION-REVIEW.md` — T0-U-ROUTER 优化复盘：流程、
  方案确定与双层 bench 设计、统计/归一化方法及其如何反哺代码决策、
  当前生产状态（未变更）、优化上限与 AI 驱动优化路线、流程疏漏与
  改进清单
- `tax0a-hotpath-topology.json` — machine-readable census (every entry
  carries `"status": "FACT"` or `"status": "HYPOTHESIS"`)
- `TAX0-A2-CONTROL-PLANE-SEMANTIC-FLOOR.md` — round-2 control-plane audit
  under #259 (Zero-Cost Control Plane framing): Path A (standalone uring)
  and Path B (runtime continuation) re-recovered per-arrow at the new
  baseline, F01–F05 suspected seams registered (code facts FACT, costs
  HYPOTHESIS, no measurement), and the Z0–Z4 semantic-floor ladder with
  the Z1b Minimal-Semantic-Equivalent-Uring checklist preregistered; no
  production change, no measurement, no optimization recommendation
- `tax0a2-control-plane-topology.json` — schema-2.0 machine census
  superseding the frozen v1.1 file (which remains valid history for
  baseline `5537187`); production drift between the two baselines is
  uring research-seam files only
- `TAX0-B-SEMANTIC-FLOOR-LADDER.md` — TAX-0B report (PR #260 round 2):
  the preregistered Z1/Z1b/Z1bw/Z2/Z3 ladder built as one harness
  (`bench/tax0_z_ladder_bench.cpp`, production-linked for Z2/Z3),
  same-work fail-closed, double-difference perf protocol; canonical
  session `results/tax0b-zladder-wsl2-formal4` (60/60 combos); headline:
  capability cost ≈ +37 instr/op, L1 abstraction tax ≈ fixed 2015
  instr/op, continuation ≈ +800/op; historical cliff NOT REPRODUCED on
  the uring path; z3w4-write instability recorded as OBS-1. All numbers
  ENVIRONMENT-LIMITED (WSL2, virtualized PMU verified real)
- `TAX0-C-CONTROL-PLANE-PROFILE.md` — TAX-0C report: perf-record symbol
  attribution over representative regimes (z2/z3w1/z1b @ 4K d32, z2 @
  1M d8) via a codegen-equivalent symbolized twin; ~55% of z2 userspace
  samples in Sluice control-plane symbols; F01 site 0.64% of samples
- `TAX0-D-CAUSAL-ABLATIONS.md` — TAX-0D report: one-mechanism/one-A/B
  sessions through SLUICE_ASYNC_INTERNAL_TESTING seams
  (`src/async/tax0_ablation_seams.hpp`, R1 installable from
  `tax0_ablation_bench`); F01 PROVEN TAX (−75 instr/op, wall neutral),
  F02 NEGLIGIBLE (−4..+23 straddling zero; F02-B not run per its gate),
  F03 STRUCTURAL_ONLY, F04 REGIME_SPECIFIC, F05 partially MEASURED
  (hand-written continuation floor ≈ +210/op @ d32 vs Z3 +787/op)
- `TAX0-E-CONTROL-PLANE-VERDICT.md` — TAX-0E verdict: MIXED per regime,
  first optimization gate ranks F01 first; ONE production optimization
  authorized (F01 stats-gate, independent Draft PR), semantics unchanged
- `tax0-control-plane-measurements.json` — schema-2.1 measurement
  companion to the frozen 2.0 census: per-seam measured fields +
  hypothesis_status outcomes (PROVEN_TAX / NEGLIGIBLE /
  STRUCTURAL_ONLY / REGIME_SPECIFIC), ladder tables, observations
  OBS-1/OBS-2, and the first-optimization-gate record; additive to the
  preregistration, which it does not modify
- `bench/tax0_z_ladder_bench.cpp` — the Z-ladder harness (one file, two
  wirings: `tax0_z_ladder_bench` links PRODUCTION sluice_async;
  `tax0_ablation_bench` links sluice_async_internal_testing and adds the
  `--f01-r1` / `--f02-r1` mode flags; fail-closed stub without liburing)
- `scripts/tax0z.py` — session runner (env capture, pilot, formal with
  double-difference normalization, ablation, canonical artifact
  generation, report pivot); sessions under `results/` are append-only

Output placement note: the task template suggested
`docs/results/performance-attribution/`, but that directory's own README
restricts it to *runner-produced* benchmark evidence ("Never
hand-created") and is validated by `scripts/bench/perf-evidence-validate.py`.
A hand-collected static census is research material, not a perf-evidence
artifact, so it lives here under `research/` (same convention as
`research/rx1/`).
