# RECEIPT — bucket-probe（线程分桶，描述性）

| 字段 | 值 |
|---|---|
| 目的 | 解析计时轮无法拆分的线程桶：app/submitter、driver、io-wq、线程内 kernel、IRQ |
| 方法 | 每 20 ms 采样 `/proc/<pid>/task/*/stat`；每个 tid 取 **first-seen / last-seen** 计数差 |
| 配置 | R3 `W03-uring-4k-d16` A0；R2 `W02-uring-4k-d2` A_R2；R2 `W02-uring-4k-d2` B_R2 |
| 参数 | `WARMUP=0 MEASURED=1`、`taskset -c 2`、3 次/配置、`SLUICE_BENCH_STATS=1` |
| 锁 | 2026-11-05T11:15:13Z 获取，11:15:17Z 释放；排他性检查：`EXCLUSIVITY_CHECK: blocked as expected` |
| 性质 | 描述性诊断；不进任何计时/门槛统计 |

## 结果（3 次均值）

| 配置 | main ticks | driver ticks | io-wq ticks | in-thread stime | ru_total (s) | bench cpu_s (timed pass) | 未归因 ticks | IRQ cpu2 / cpu12 |
|---|---|---|---|---|---|---|---|---|
| R3-W03-D16-A0 | 13.3 | 0.7 | 7.3 | 15.0 | 0.276 | 0.0701 | +6.3 | 307 / 3 |
| R2-W02-D2-A | 22.3 | 0.0 | 10.0 | 23.7 | 0.347 | 0.1005 | +2.3 | 631 / 3 |
| R2-W02-D2-B | 22.7 | 0.0 | 10.0 | 24.0 | 0.358 | 0.1040 | +3.2 | 646 / 2 |

## 口径（防止重复计数）

1. **main** = `tid == pid`（submitter）。
2. **driver** = 其它非 `iou-*` 用户线程（R3 的 driver thread；每 pass 创建/join 一次，故按
   first-seen/last-seen 聚合）。
3. **io-wq** = comm 以 `iou-wrk-`/`iou-wq-`/`iou-sqp-` 开头的线程。它们出现在
   `/proc/<pid>/task` 内，属于进程线程组，因此其 CPU **已经包含在** bench 的
   `CLOCK_PROCESS_CPUTIME_ID`（进程 CPU）中；本桶是**份额**，不是加数。
4. **in-thread kernel** = 各线程 stime 之和，是 1–3 的子集，不另加。
5. **IRQ** = `/proc/interrupts` 的 cpu2/cpu12 列增量；整机口径上界，无法在不做 tracing 的
   前提下归因到本 workload（tmpfs 无块设备 IRQ，主要是 timer/IPI）。
6. **未归因** = `wait4` rusage ticks − 分桶 ticks；来自 10 ms tick 粒度与采样窗口外的短线程，
   本轮为 +2…+7 ticks（约 0.02–0.07 s）。

## 与计时轮的关系

- 计时轮 `measure_pairs.py` 的 sampler 只比较**全局首末**样本；driver thread 的生命周期在
  invocation 内部，因此被记录为 0 ticks。该缺陷在会话后已修正为逐 tid first-seen/last-seen，
  但已采集的计时轮 `buckets` 字段无法重算；本探针替代其 driver/io-wq 拆分。
- 计时轮的 CPU 主指标是 bench 的**进程 CPU**（`CLOCK_PROCESS_CPUTIME_ID`，包含 main/driver/io-wq
  全部线程，每个线程只计一次）；A/A 与 R2-D2 的 CPU 结论不受 sampler 缺陷影响。
