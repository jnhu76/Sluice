# C7-3 — shadow_outstanding_sum==0 as a possibly legal observation:
why the frozen oracle fired on T1, and the corrected protocol for R5+

Status: protocol correction for the NEXT round. The R4 TRAJECTORY verdict
(INCOMPLETE, 0/24) is NOT retro-flipped; it was a faithful application of
the frozen criterion. Verified 2026-11-06.

## The frozen criterion and its intent

`r4/PREREGISTERED.md:100-105` (§1.4, frozen):

> shadow 防编译器消除（冻结）：shadow 读取并入已导出的
> `r4_shadow_outstanding_sum` / `r4_shadow_window_sum` 计数器…
> 运行时证据：W03 SHADOW 冒烟 `shadow_outstanding_sum=76`…
> **冻结判据**：凡 SHADOW 计时 invocation 报
> `shadow_outstanding_sum==0` → 该 invocation 无效（oracle 失败类，§10.2）。

Intent: the sum is an anti-compiler-elimination witness — a non-zero sum
proves the per-decision `outstanding()` reads actually executed. The
criterion was calibrated on W03-d16, where the smoke run produced 76 and
the state is non-zero at many decision points.

## Why it structurally fires on T1 (d2serial)

Source facts (r5 worktree = frozen R4_ORIGINAL, HEAD f3b25329):

- The adapter reads `record.active_data = ctx_.outstanding()` at EVERY
  budget decision (`bench/r3_ready_batch.hpp:467-471`) and accumulates it
  (`:483-485`).
- d2serial is `add_r3(4096, 20000, 2, 1, "-r3d2serial")` — depth 2,
  max_refill 1 (`bench/bench_w03_external_loop.cpp:856`): the refill loop
  builds exactly one op (`want = min(depth - window, remaining, 1)`,
  `:397-398`), breaks after one round (`:509-511`), the submitter then
  parks until a completion is published (`:522-539`) and settles the front
  entry (`:541-572`) before the next refill. At each decision point the
  previous op is therefore already published (and consumed) — `outstanding()`
  is structurally 0. Same for `caller_window` at the decision instant.

Run evidence (this session, read-only):

- `r4/raw/matrix/TRAJECTORY/attempts.jsonl`: 72 attempt records, every one
  carrying `reasons: ["shadow_outstanding_sum==0 (prereg 1.4 oracle)"]` —
  3 attempts × 24 blocks, all SHADOW/T1.
- `r4/raw/matrix/TRAJECTORY/jsonl/b00-p7-SHADOW-T1-a1.jsonl`: the
  "failed" invocation actually completed normally — `exit: 0`, every
  record `ok: True`, 5 measured reps × 20000 ops, and per-rep
  `r4_shadow_decisions=20000, r4_shadow_outstanding_sum=0,
  r4_shadow_window_sum=0`. The reads DID execute (20000 decisions per rep);
  the observed value was legitimately zero every time.
- `r4/analysis/MAIN_SUMMARY.json` `trajectory_status.reason` already states
  the mechanism: "in the sparse-serial T1 shape outstanding() is 0 at every
  decision point, so the SHADOW oracle cannot pass".

The criterion therefore conflates two propositions:
(a) "the shadow read path executed" (liveness — what the oracle is FOR), and
(b) "the observed state was non-zero at least once" (a shape property).
On d2serial, (b) is structurally false while (a) is true, so the oracle has
zero discriminating power for elimination on this shape: sum==0 is exactly
what a correct, live instrument must report.

## Corrected protocol for the new round (pre-registration text)

Do not change R4's archived FAIL. For R5+ pre-registration, replace the
single-condition oracle with a shape-aware pair:

1. Liveness (all shapes): the shadow counters must be consistent with the
   run's own op count — `r4_shadow_decisions == submitted_ops` for
   shapes where every decision is a shadow decision (serial shapes), and
  `r4_shadow_decisions > 0` otherwise; both counters must be present in
   the exported record sink. (Compiler elimination of a locked public-API
   read whose result flows into an exported counter would break this
   equality.)
2. State expectation (shape-conditional):
   - shapes where outstanding() can be non-zero at decision points
     (e.g. d16-class): keep `shadow_outstanding_sum > 0` as a validity
     condition — a zero sum there still indicates a dead instrument;
   - shapes with structural zero (depth ≤ 2 serial / refill 1, where the
     previous op is published-and-consumed before the next decision):
     `shadow_outstanding_sum == 0` is a LEGAL OBSERVATION and must be
     recorded as such (class `STRUCTURAL_ZERO`, not oracle failure);
     validity rests on clause 1 plus the independent per-decision diag
     evidence (`active_read=1` lines, `r3_ready_batch.hpp:468`, diagnostic
     runs only).
3. Any future oracle that asserts non-constant behavior must be justified
   against a structural analysis of the shape BEFORE freezing (state, for
   each frozen shape, whether the observable can take each value).

Under this protocol the R4 T1 SHADOW invocations would be valid
(20000==20000 per rep), TRAJECTORY blocks could complete, and the R4
finding "T1 changed-action rate 0% (remaining==1 forces both strategies to
immediate)" would become testable instead of lost to INCOMPLETE.
