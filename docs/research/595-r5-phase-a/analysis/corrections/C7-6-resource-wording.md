# C7-6 — Resource-evidence wording rules (missing evidence ≠ correctness error; untested ≠ zero cost)

Status: rule codification + verification that the R4 archive already
conforms. R4 archive unchanged. Verified 2026-11-06.

## Verified state of the R4 archive

`r4/RESULTS.md:159-166` (§6, resource gate):

- "RSS 子门：PASS" — backed by measured data: all (cell, arm)
  `(RSS_X−RSS_A0)/RSS_A0` points and one-sided uppers 0.00%; cross-check of
  192 invocations' wait4 `ru_maxrss` vs in-rep `peak_rss_kb` (delta 0 KB).
- "线程子门：PASS（候选臂 R/S/Q8 全部 24/24 块 max_user_threads ≤ A0…）"
  — measured, with the honest disclosure of a 1/24 non-candidate-arm
  anomaly (SNP, block 14).
- "整体资源门：NOT_ESTABLISHED —— §8.1 三个子门未运行：静态审计
  `logs/audit.txt` 不存在；…strace…未跑；valgrind/massif 分配斜率未跑。
  依 §8.1「NOT_RUN → NOT_ESTABLISHED」，并因此整体不得 GO。"

`r4/RESULTS.md:457` repeats "资源门 NOT_ESTABLISHED（3 个子门 NOT_RUN）" as
part of the HOLD basis. A grep for 零成本/无资源/zero-cost across
RESULTS.md, PREREGISTERED.md and the branch-extracted TAIL-LATENCY.md found
no zero-cost claim (no matches). The archive does NOT present missing
resource evidence as a correctness error and does NOT claim zero resource
cost from unmeasured dimensions.

## Rules adopted for R5 wording (before/after patterns)

R1 — Missing resource evidence is evidence absence, not a defect finding.

```
FORBIDDEN: "R/S/Q8 have a resource correctness problem (audit/strace/
           valgrind missing)."
ALLOWED:   "Static-audit, strace thread/fd, and valgrind allocation-slope
           sub-gates were NOT_RUN; the resource gate is therefore
           NOT_ESTABLISHED (frozen rule §8.1), which by itself blocks GO."
```

R2 — Untested dimensions must never be phrased as zero/absent cost.

```
FORBIDDEN: "R adds no threads/timers/allocations (none were observed)."
ALLOWED:   "Measured: RSS delta 0.00% and candidate-arm thread counts ≤ A0
           on 24/24 blocks. NOT measured: timer/fd/clone creation counts
           (strace), static audit, allocation slope (valgrind/massif);
           no claim is made about them."
```

R3 — Any sentence citing a measured resource PASS in an R5 summary must
name its sub-gate scope (RSS; threads) and carry the NOT_RUN tail, so a
reader cannot widen "RSS/threads PASS" into "resources PASS".

R4 — The 0.00% RSS delta is a MEASURED zero (same block, same binary,
wait4-vs-peak cross-check), which is a legitimate statement; it must not
be shortened to "no resource cost" (which would span unmeasured
dimensions).
