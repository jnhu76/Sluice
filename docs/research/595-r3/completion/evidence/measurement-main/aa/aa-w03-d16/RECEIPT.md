# RECEIPT — AA-W03-D16（R3 A0 vs A0）

| 字段 | 值 |
|---|---|
| block | `AA-W03-D16` |
| case | `W03-uring-4k-d16` |
| arms | `AA` (A='A0', B='A0') |
| pairs | 24 / 24 |
| binary | `/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/bench_w03_external_loop` |
| binary sha256 | `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1` |
| expect sha ok | True |
| env | `[]` |
| warmup / measured / stats | 1 / 5 / 0 |
| cpu set | `2` (monitored ['2', '12']) |
| order rule | AA: pair even m1,m2 / odd m2,m1 |
| started / ended (UTC) | 2026-11-05T11:03:34Z / 2026-11-05T11:04:44Z |
| valid pairs | 24 / 24 |
| fixture unchanged | True (`/tmp/sluice-595-r3c/external.bin`) |
| lock file | `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` inode 6941075 (/proc/locks holders: []) |

## 结果（pair-relative，正 = 第二个成员/B 更差）

| metric | mean | median | CI95 | half-width | one-sided 95% upper |
|---|---|---|---|---|---|
| cpu | -0.816% | -0.831% | [-1.616%, -0.022%] | 0.797pp | -0.144% |
| tput | -0.870% | -0.856% | [-1.699%, -0.057%] | 0.821pp | -0.181% |
| p50 | -0.217% | -0.157% | [-0.650%, +0.214%] | 0.432pp | +0.147% |
| p95 | -0.504% | -0.027% | [-1.648%, +0.427%] | 1.037pp | +0.302% |
| p99 | +0.313% | +0.113% | [-1.785%, +2.292%] | 2.039pp | +2.014% |
| ru_secondary | -0.574% | -0.375% | [-1.327%, +0.157%] | 0.742pp | +0.046% |
| rss | +0.074% | +0.000% | [+0.000%, +0.221%] | 0.110pp | +0.221% |

## A/A 门（prereg §8.2：只有半宽门）

| metric | half-width | 门槛 | 判定 |
|---|---|---|---|
| cpu | 0.797pp | 5pp | PASS |
| p99 | 2.039pp | 3pp | PASS |
| tput | 0.821pp | 3pp | PASS |

**A/A 总判定：PASS**

## 判定

A/A PASS：CPU 半宽 0.797pp ≤ 5pp，tput 0.821pp ≤ 3pp，p99 2.039pp ≤ 3pp。本会话噪声底可用于后续同二进制 A/B；本块本身不产生机制结论。

## 限制

- 本块只含两个成员/臂；无 PMU、无 ftrace、无 perf；计时轮 `SLUICE_BENCH_STATS=0`。
- 背景负载不构成无效理由；每次 invocation 前后记录 loadavg/cpu2/cpu12/governor/freq/df。
- 只有 exit/oracle/stats/ops/SHA/arm 读回失败才无效；无离群点剔除。
- 线程分桶以 `bucket-probe/` 为准（计时轮的 sampler 只比较全局首末样本，短生命周期的 driver 线程被低估；见 CALIBRATION_SUMMARY §5）。
