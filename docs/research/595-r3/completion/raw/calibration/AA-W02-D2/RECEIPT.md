# RECEIPT — AA-W02-D2 / R2-AA（R2 A_R2 vs A_R2）

| 字段 | 值 |
|---|---|
| block | `AA-W02-D2` |
| case | `W02-uring-4k-d2` |
| arms | `AA` (A='A', B='A') |
| pairs | 24 / 24 |
| binary | `/home/jnhu/sluice-595-e5/worktree/build/linux/x86_64/release/bench_w02_pipeline` |
| binary sha256 | `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a` |
| expect sha ok | True |
| env | `['SLUICE_BENCH_595_DEPTHS=1']` |
| warmup / measured / stats | 1 / 5 / 0 |
| cpu set | `2` (monitored ['2', '12']) |
| order rule | AA: pair even m1,m2 / odd m2,m1 |
| started / ended (UTC) | 2026-11-05T11:04:47Z / 2026-11-05T11:06:26Z |
| valid pairs | 24 / 24 |
| fixture unchanged | True (`/tmp/sluice-595-r3c/pipeline.bin`) |
| lock file | `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` inode 6941075 (/proc/locks holders: []) |

## 结果（pair-relative，正 = 第二个成员/B 更差）

| metric | mean | median | CI95 | half-width | one-sided 95% upper |
|---|---|---|---|---|---|
| cpu | +0.584% | +0.369% | [-0.270%, +1.454%] | 0.862pp | +1.314% |
| tput | +0.539% | +0.375% | [-0.310%, +1.397%] | 0.853pp | +1.263% |
| p50 | +0.057% | -0.056% | [-0.421%, +0.559%] | 0.490pp | +0.475% |
| p95 | -0.028% | -0.041% | [-0.649%, +0.619%] | 0.634pp | +0.504% |
| p99 | -0.407% | -0.459% | [-1.863%, +0.978%] | 1.420pp | +0.761% |
| ru_secondary | +0.334% | +0.310% | [-0.242%, +0.949%] | 0.595pp | +0.854% |
| rss | +0.032% | +0.000% | [+0.000%, +0.095%] | 0.047pp | +0.095% |

## A/A 门（prereg §8.2：只有半宽门）

| metric | half-width | 门槛 | 判定 |
|---|---|---|---|
| cpu | 0.862pp | 5pp | PASS |
| p99 | 1.420pp | 3pp | PASS |
| tput | 0.853pp | 3pp | PASS |

**A/A 总判定：PASS**

## 判定

A/A PASS：CPU 半宽 0.862pp ≤ 5pp，tput 0.853pp ≤ 3pp，p99 1.420pp ≤ 3pp。该块与 R2-AA 是同一 24 对运行（同一二进制、同一 case、同一 env），不是两次独立采集；两个 block id 引用同一份原始数据。

## 限制

- 本块只含两个成员/臂；无 PMU、无 ftrace、无 perf；计时轮 `SLUICE_BENCH_STATS=0`。
- 背景负载不构成无效理由；每次 invocation 前后记录 loadavg/cpu2/cpu12/governor/freq/df。
- 只有 exit/oracle/stats/ops/SHA/arm 读回失败才无效；无离群点剔除。
- 线程分桶以 `bucket-probe/` 为准（计时轮的 sampler 只比较全局首末样本，短生命周期的 driver 线程被低估；见 CALIBRATION_SUMMARY §5）。
