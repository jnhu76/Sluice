# RECEIPT — R2-D2（A_R2 vs B_R2，同 SHA）

| 字段 | 值 |
|---|---|
| block | `R2-D2` |
| case | `W02-uring-4k-d2` |
| arms | `AB` (A='A', B='B') |
| pairs | 24 / 24 |
| binary | `/home/jnhu/sluice-595-e5/worktree/build/linux/x86_64/release/bench_w02_pipeline` |
| binary sha256 | `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a` |
| expect sha ok | True |
| env | `['SLUICE_BENCH_595_DEPTHS=1']` |
| warmup / measured / stats | 1 / 5 / 0 |
| cpu set | `2` (monitored ['2', '12']) |
| order rule | AB: pair even A,B / odd B,A |
| started / ended (UTC) | 2026-11-05T11:06:26Z / 2026-11-05T11:08:07Z |
| valid pairs | 24 / 24 |
| fixture unchanged | True (`/tmp/sluice-595-r3c/pipeline.bin`) |
| lock file | `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` inode 6941075 (/proc/locks holders: []) |

## 结果（pair-relative，正 = 第二个成员/B 更差）

| metric | mean | median | CI95 | half-width | one-sided 95% upper |
|---|---|---|---|---|---|
| cpu | +3.435% | +3.745% | [+2.673%, +4.160%] | 0.743pp | +4.065% |
| tput | +3.298% | +3.616% | [+2.574%, +3.976%] | 0.701pp | +3.889% |
| p50 | +2.294% | +2.190% | [+1.807%, +2.786%] | 0.489pp | +2.706% |
| p95 | +5.233% | +5.123% | [+4.433%, +6.030%] | 0.799pp | +5.906% |
| p99 | +7.052% | +6.519% | [+4.844%, +9.514%] | 2.335pp | +9.067% |
| ru_secondary | +3.462% | +3.786% | [+2.832%, +4.059%] | 0.613pp | +3.976% |
| rss | +0.032% | +0.000% | [+0.000%, +0.095%] | 0.048pp | +0.095% |

## 负控门（prereg §5.4a / §9.5：点估计 + 单侧 95% 上界 + Holm）

| metric | mean | 单侧 95% 上界 | 门槛 | point in bound | upper in bound | p (P(mean>=thr)) | Holm alpha | Holm reject | 判定 |
|---|---|---|---|---|---|---|---|---|---|
| cpu | +3.435% | +4.065% | 5% | True | True | 0.00010 | 0.0125 | True | PASS |
| p95 | +5.233% | +5.906% | 5% | False | False | 0.72013 | 0.016666666666666666 | False | FAIL |
| p99 | +7.052% | +9.067% | 5% | False | False | 0.96470 | — | False | FAIL |
| tput | +3.298% | +3.889% | 3% | False | False | 0.80442 | — | False | FAIL |

**负控总判定：FAIL**

## 判定

**LEGACY_D2_NEGATIVE_CONTROL_FAIL**：B_R2 相对 A_R2 的 tput 单侧 95% 上界 3.889% > 3%、p95 5.906% > 5%、p99 9.067% > 5%（CPU 上界 4.065% ≤ 5% 通过）；点估计 tput +3.298%、p95 +5.233%、p99 +7.052% 均在界外。该结论只描述旧 R2 B（BOUNDED_PROGRESS）在 W-02 D2 的退化，不得外推到 R3 B。

## 限制

- 本块只含两个成员/臂；无 PMU、无 ftrace、无 perf；计时轮 `SLUICE_BENCH_STATS=0`。
- 背景负载不构成无效理由；每次 invocation 前后记录 loadavg/cpu2/cpu12/governor/freq/df。
- 只有 exit/oracle/stats/ops/SHA/arm 读回失败才无效；无离群点剔除。
- 线程分桶以 `bucket-probe/` 为准（计时轮的 sampler 只比较全局首末样本，短生命周期的 driver 线程被低估；见 CALIBRATION_SUMMARY §5）。
