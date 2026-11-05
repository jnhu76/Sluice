# D2 低开销成本核算表（Sluice #595 causal-r2）

状态：ACTIVE EVIDENCE。轮次：causal-r2 Phase B（2026-11-04）。
范围：复现的 D2（`W02-uring-4k-d2`，4 KiB 读，depth=2）A·IMMEDIATE vs B·BOUNDED_PROGRESS；
D1（`W02-uring-4k-d1`，depth=1）作为精确过滤的对照。A/A 校准 8 对（PMU 块）。
决策阈值与冻结基线来自只读 R1 预注册：`/home/jnhu/sluice-595-e5/evidence/measurement/PREREGISTERED.md`；
本轮基线（任务给定 = phaseA 协议记录，24 对）：CPU/op **+3.73%** [3.04, 4.42]、
吞吐（B−A）/A **−3.58%**、p95 **+4.96%**、p99 **+7.07%**；A/A 噪声 0.65 pp。
从 `evidence/raw/phaseA/ab-d2-w02-4k/results.jsonl` 重算得到 +3.708% / −3.594% / +4.94% / +7.00%
（与任务给定值差 <0.1 pp，来自 p99 聚合与舍入；两者在本表中等价引用）。

**结论先行**：D2 复现为 CPU/op **+3.70%** [2.93, 4.55]（无计数器 split 块，12 对），与冻结基线 +3.73% 一致。
CPU 增量按线程分解为：提交线程/进度 owner 用户态 **+95.2 ns/op（+7.39%）**、同线程内核态
**+79.4 ns/op（+3.99%）**、io-wq 内核态 **+7.9 ns/op（+0.46%，不显著）**。
计数器账本显示 B 每 op 延迟 **0.5589** 个 SQE，但 flush 与 enter 仍为 **1.0000/op**（与 A 完全相同），
同时新增 **0.5589 次/op 进度唤醒**（其中 **0.2353 次/op 实际 eventfd write**，0.3236 次/op 被合并）。
即：D2 的深度 2 形状下，延迟 flush 没有节省任何 enter，只把同一 SQE 的 flush 从 dispatch 时刻挪到下一次
poll，并多付出一次进度通知；成本落在提交线程的用户态与内核态，不在 io-wq、不在缺页、不在设备路径。

---

## 1. 方法与 provenance

| 项 | 值 | 证据 |
|---|---|---|
| 主机 | E5，Xeon E5-2666 v3（10C/20T，family 6 model 63），单 NUMA，CPU 0–19 | `lscpu` |
| 内核 | Linux 7.2.11-200.fc44.x86_64，`#1 SMP PREEMPT_DYNAMIC` | `uname -a` |
| 工作区 | `/home/jnhu/sluice-595-e5/causal-r2/worktree`（独立副本；主工作树 `/home/jnhu/Source/Sluice` 未改动） | `git log -1` = `271b81bf…`；`git status` |
| 研究补丁 + 本轮计数器 | `git diff HEAD` sha256 `27a6b2a049554c995ad26a9a07699648dcaa5b31d9d01ba9a06534b426b76c9e`（含 R1 补丁 + 本轮计数器/成本模式） | `git diff HEAD \| sha256sum` |
| 冻结基线二进制 | `bench_w02_pipeline_frozen` sha256 `61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886`（与 phaseA 记录的 `61c6d579…6b886` 一致） | `sha256sum`；`evidence/counters/bin/` |
| PMU 成本二进制 | `bench_w02_pipeline_pmu` sha256 `6670d8dee37e8d8faa60549dfc5e1f6121cc4f56ee66cf58fa91cc678d16db09`（无计数器代码；含 PMU-only/复用 fixture 研究模式） | 同上 |
| 计数器二进制 | `bench_w02_pipeline_counters` sha256 `b9a4994631dbe6714e630fec838fd1d2b98604bb872007e6882bad96f1eb3a3a`（`-DSLUICE_595_COUNTERS`） | 同上 |
| 工具链 | g++ 16.2.1、xmake v3.1.2+HEAD、liburing-devel 2.13-2、perf 7.2.11-200.fc44 | `--version` |
| 权限/PMU 边界 | `perf_event_paranoid=2`，本会话无 sudo/CAP_PERFMON；**仅用户态 PMU 事件可用**（`:u`），内核态 PMU（`:k`）`No supported events found` | `perf stat -e cycles:k -- sleep 0.1` 实测失败 |
| 事件复用 | `cycles:u,instructions:u,cache-references:u,cache-misses:u,branch-misses:u`（2 fixed + 3 generic）+ 软件事件；**全部 run_time 100.00%**；无复用 | 原始 CSV 每行第 5 字段；见 `raw/pmu-d2-ab/*.perf.csv` |
| CPU 亲和 | bench 固定 `taskset -c 2`；harness/采样固定 cpu3；perf 进程在 cpu3 | 运行命令见下 |
| fixture | 64 MiB pattern file，warm tmpfs `/tmp/sluice-595-cost`；PMU/split 模式复用 fixture（`SLUICE_BENCH_REUSE_FIXTURE=1`），full 模式每次重写 | bench `write_fixture`；模式开关 |
| 运行环境 | 实验阶段机器独占；run 前 loadavg 0.42/0.47/0.42，run 后 0.86/1.35/1.22（20 核，≈1.3% 单核）；cpu2/3 `scaling_cur_freq` run 前后均为 1.2 GHz 请求值 | `cat /proc/loadavg`、`scaling_cur_freq` |
| 样本量 | 每块见 §2；pair 内 AB/BA 平衡（偶数对 A→B，奇数对 B→A），AA 块双 A；bootstrap 10,000 次、seed 5950、percentile 95% CI | `analyze_cost.py` |
| 统计约定 | CPU/计数器/p95/p99 的 Δ% = (B−A)/A（正 = B 更差）；吞吐 Δ% = (B−A)/A（负 = B 更慢）。CI 为 pair 相对差的 bootstrap 区间 | 同 `harness/analyze.py` 约定 |

运行命令（每 invocation）：

```
taskset -c 3 perf stat -x, -e <user-events> -o <csv> -- taskset -c 2 <bench>   # PMU 块
taskset -c 3 <bench>                                                          # direct 块
env SLUICE_595_ARM={A,B} SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER=<case> \
    SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED={1,5,20} SLUICE_BENCH_STATS=1 \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-cost SLUICE_BENCH_JSONL=<jsonl> \
    [SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1] [SLUICE_595_COUNTERS={0,1}]
```

成本模式说明（研究副本独有，不改公开 API/v2 契约）：
`SLUICE_BENCH_PMU_ONLY=1` 只运行 warmup+measured 吞吐 pass（跳过 oracle/latency pass），
使 perf 窗口不被 verify/时间戳 pass 稀释；`SLUICE_BENCH_REUSE_FIXTURE=1` 跳过 64 MiB 重写。
这两个模式对 A/B 对称；绝对 CPU/op 与 phaseA full 模式不可直接比（见 §2 注），A/B 差值是可用量。
计数器模式用 full 模式（warmup=1、measured=5，保留 oracle/latency 语义），计数器只在 measured pass
使能（`g_research_measured`），oracle/latency 与 warmup 不计。

---

## 2. D2 复现与冻结基线对照

| 块 | 模式 | 对 | CPU/op Δ% [CI95] | 吞吐 Δ% [CI95]（B−A）/A | p95 Δ% | p99 Δ% |
|---|---|---|---|---|---|---|
| phaseA（冻结基线，R1；任务给定值） | full，无仪器 | 24 | **+3.73** [3.04, 4.42] | **−3.58** [−4.22, −2.94] | +4.96 | +7.07 |
| `frozen-full-d2`（当轮冻结二进制） | full，无仪器 | 6 | **+2.67** [1.50, 3.80] | **−2.58** [−3.66, −1.47] | +4.25 | +6.98 |
| `split-d2`（**主复现**） | PMU-only，无计数器，measured=20 | 12 | **+3.70** [2.93, 4.55] | **−3.55** [−4.34, −2.84] | n/a | n/a |
| `pmu-d2-ab` | PMU-only，measured=1 | 12 | **+3.64** [0.74, 5.84] | **−3.25** [−5.42, −0.31] | n/a | n/a |
| `cnt-on-full-d2` | full，计数器 ON | 12 | +4.03 [3.13, 4.85] | −3.86 [−4.62, −3.01] | +5.48 | +9.69 |
| `cnt-off-full-d2` | full，计数器 OFF | 12 | +4.52 [3.05, 6.24] | −4.26 [−5.81, −2.95] | +5.89 | +10.46 |
| `pmu-d2-aa`（A/A 噪声；B 列 = 第二次 A） | PMU-only | 8 | **−0.95** [−2.02, 0.23] | **−1.00** [−2.13, +0.21] | n/a | n/a |

读法：
- 无计数器主复现 `split-d2` 的 +3.70% 与 phaseA 的 +3.73% 在 0.03 pp 内一致；`pmu-d2-ab` +3.64% 同向。
- A/A 噪声：注册口径为 phaseA full 模式 24 对的 0.65 pp；本轮 PMU 块 8 对的 A/A CPU 均值为
  −0.95%、CI 半宽约 1.1 pp（样本更少）。D2 的 CPU +3.70% 仍远超两者；PMU 的 instructions/cycles/
  branch-misses A/A 半宽分别约 0.26/1.7/1.4 pp，D2 的 +1.06/+5.71/+14.94% 均远超。
- 块间 CPU 幅度在 +2.67%…+4.52% 间漂移，大于块内 A/A 噪声（0.65 pp），说明存在**块级频率/热状态漂移**
  （无 root，turbostat 不可用；只能记录 `scaling_cur_freq` 请求值）。所有块方向一致；PMU-only 的
  CPU/op CI 较宽（+0.74…+5.84），其余块 CI 排除 0；定量结论取 `split-d2`（无计数器、20 个
  measured rep）与 `pmu-d2-ab`。
- `frozen-full-d2` 的绝对 CPU/op（A 5175.7 ns）比 phaseA（A 4776.7 ns）高约 8%；同一冻结二进制、
  同一 full 模式，说明是环境/频率层面的块级偏移，不是代码差异。因此**不把不同轮的绝对值相减**，
  只比较同一块内 A/B 的相对差。
- 绝对 CPU/op 在 `split-d2` 为 A 5131.5→B 5320.7 ns（+189.1 ns/op），高于 phaseA 的 4776.7→4953.9；
  原因是 PMU-only + 复用 fixture 的成本模式改变了缓存/频率状态（见 §6 缺口 G5），不影响 A/B 差值。

---

## 3. 归一化成本表（D2：A vs B → Δ/op → 置信度 → 可能域）

### 3.1 CPU 与用户态 PMU

| # | 指标（源） | A/op | B/op | Δ/op | Δ% [CI95] | 置信度 | 可能域 |
|---|---|---|---|---|---|---|---|
| 1 | 进程 CPU/op（`CLOCK_PROCESS_CPUTIME_ID`，含 io-wq；split-d2） | 5131.52 ns | 5320.66 ns | **+189.15 ns** | **+3.70** [2.93, 4.55] | 高（直接测量，CI 排除 0，> A/A 0.65 pp） | C1+C3（C4 介导） |
| 2 | 提交线程用户 CPU/op（`/proc/<pid>/task/*/stat` utime；split-d2） | 1319.44 ns | 1414.68 ns | **+95.24 ns** | **+7.39** [4.07, 10.55] | 中高（10 ms tick 量化；12 对一致） | **C1** |
| 3 | 提交线程内核 CPU/op（同上 stime；split-d2） | 2019.84 ns | 2099.21 ns | **+79.37 ns** | **+3.99** [1.87, 6.17] | 中高（10 ms tick；符号/量级与总 CPU 差一致） | **C3**（syscall 上下文） |
| 4 | io-wq 内核 CPU/op（`iou-wrk-*` stime；split-d2） | 1771.83 ns | 1779.76 ns | +7.94 ns | **+0.46** [−0.44, 1.36] | 高（不显著；排除 io-wq） | C3（io-wq：无变化） |
| 5 | instructions:u/op（perf，pmu-d2-ab） | 4709.94 | 4759.98 | **+50.04** | **+1.06** [0.97, 1.16] | 高（100% enabled；A/A +0.04%） | **C1** |
| 6 | cycles:u/op（perf，pmu-d2-ab） | 3028.53 | 3201.09 | **+172.56** | **+5.71** [4.55, 6.87] | 高（A/A −0.29%） | **C1/C5** |
| 7 | 用户态 IPC（instructions/cycles，pmu-d2-ab） | 1.5553 | 1.4874 | **−0.0679** | **−4.36** [−5.42, −3.28] | 高（A/A +0.39%） | **C5**（介导） |
| 8 | cache-references:u/op（pmu-d2-ab） | 18.0468 | 22.0532 | **+4.006** | **+23.31** [12.73, 35.18] | 中高（A/A −7.66% 宽；D2 远超噪声） | **C5**（介导） |
| 9 | cache-misses:u/op（pmu-d2-ab） | 0.3635 | 0.3644 | +0.0010 | +0.65 [−3.74, 5.11] | 不显著（无变化） | C5（无内存缺失增量） |
| 10 | branch-misses:u/op（pmu-d2-ab） | 7.6022 | 8.7324 | **+1.1302** | **+14.94** [12.88, 16.80] | 高（A/A +1.83% 有偏，但仍远超） | **C5**（介导） |
| 11 | page-faults:u/op（pmu-d2-ab） | 0.01184 | 0.01185 | +0.000017 | +0.15 [−0.35, 0.69] | 不显著（无变化） | C5/C6（无设备路径） |
| 12 | task-clock:u/op（perf 窗口 / 40k ops，含 startup+warmup） | 6.284 µs | 6.435 µs | +0.151 µs | +2.74 [−1.31, 6.41] | 低中（窗口含启动，不作定量） | U |
| 13 | 吞吐（ops/s，split-d2） | 194,662 | 187,728 | −6,934 | **−3.55** [−4.34, −2.84] | 高（observed symptom） | observed |
| 14 | p95（frozen-full-d2，同二进制） | 13,729 ns | 14,306 ns | +576 ns | **+4.25** [2.34, 5.84] | 高（observed symptom） | observed |
| 15 | p99（frozen-full-d2） | 18,963 ns | 20,175 ns | +1,212 ns | **+6.98** [0.93, 12.50] | 中（CI 宽；phaseA +7.07 同向） | observed |

### 3.2 提交 / 等待 / CQ 计数账本（计数器 ON，`cnt-on-full-d2`，12 对，每 measured op）

计数器定义与**真实源码位置**（研究副本；`-DSLUICE_595_COUNTERS` 时编译）：

| 计数器 | 源码位置 | 精确定义 |
|---|---|---|
| `dispatch_passes` | `src/async/uring_transport.cpp:44` | `dispatch_after_accept()` 入口；每个 accepted request 一次 |
| `poll_passes` | `src/async/uring_backend.cpp:624` | `UringAsyncBackend::poll()` 入口 |
| `empty_polls` | `src/async/uring_backend.cpp:698` | 本次 poll `reaped==0 && published==0` |
| `sqe_preparations` | `src/async/uring_transport.cpp:212` | `dispatch_one_locked()` 写完 SQE、ledger append 后；每个物理 SQE 一次 |
| `sqes_deferred` | `src/async/uring_transport.cpp:264` | `defer_transport_flush_locked_()` 返回 true；刚 dispatch 的 SQE 被持有 |
| `flushes` | `src/async/uring_transport.cpp:273` | `submit_transport_locked()` 通过非空 ledger 守卫（实际携带 ≥1 SQE 的 flush；与既有 `submit_flushes_` 同一事件） |
| `submit_attempts` | `src/async/uring_transport.cpp:269` | 每次 `submit_transport_locked()` 调用（含空 ledger 的 poll 调用） |
| `actual_enters` | `src/async/uring_transport.cpp:283` | 生产路径 `::io_uring_submit()` 调用；ledger 非空，liburing 必然发一次 `io_uring_enter` |
| `wait_attempts` | `src/async/async_io_context.cpp:600` | `AsyncIoContext::wait_one(max_park)`（统计 wait_calls 之后） |
| `cqe_reap_attempts` | `src/async/uring_completion.cpp:201` | `reap_cqes()` 入口 |
| `cqes_reaped` | `src/async/uring_completion.cpp:225` | 每个交给 `handle_one_cqe()` 的 CQE |
| `progress_handoffs` | `include/sluice/async/detail/uring_backend.hpp:317` | `note_progress_driver_locked_()` 观察到 most-recent driver 线程 id 变化 |
| `wakeups` | `include/sluice/async/detail/uring_backend.hpp:334` | `signal_ready_progress()` 调用（逻辑唤醒） |
| `wakeup_writes` | `include/sluice/async/detail/progress_source.hpp:440` | 实际 `::write(eventfd)` 尝试（支持计数器） |
| `wakeup_coalesced` | `include/sluice/async/detail/progress_source.hpp:434` | `notification_pending_` 已置位而跳过的 signal（支持计数器） |

`actual_enters` 的 strace 交叉验证（非计时运行，D2，A，warmup=0 measured=1，3 个 pass × 20k op）：
`strace -f -c -e trace=io_uring_enter` → **60,004** 次 enter；计数器 measured pass = 20,000/20,000 op。
即每次操作 1 次 submit enter；R1 记录的“3.0002 enters/op”是 3 个 pass 的总 enter 除以该 rep 的 20k op
（60,000/20,000 = 3.0），不是每 pass 3 次。该口径差异不影响 A/B 比较，但必须写清。

| # | 计数器（每 op） | A | B | Δ/op | Δ% [CI95] | 置信度 | 可能域 |
|---|---|---|---|---|---|---|---|
| 16 | `dispatch_passes` | 1.0000 | 1.0000 | 0 | 0 | 高（确定性） | C2 |
| 17 | `poll_passes` | 0.7893 | 0.7942 | +0.0049 | +0.63 [0.19, 1.08] | 高 | C4 |
| 18 | `empty_polls` | 0.2380 | 0.2353 | −0.0027 | −1.12 [−2.53, 0.34] | 不显著 | C4 |
| 19 | `sqe_preparations` | 1.0000 | 1.0000 | 0 | 0 | 高 | C2 |
| 20 | `sqes_deferred` | 0 | **0.5589** | **+0.5589** | [0.5572, 0.5605]（绝对） | 高（确定性） | **C2** |
| 21 | `flushes` | 1.0000 | 1.0000 | 0 | 0 | 高（确定性） | **C2** |
| 22 | `submit_attempts` | 1.7893 | 1.2354 | **−0.5540** | **−30.96** [−31.11, −30.80] | 高 | **C2** |
| 23 | `actual_enters` | 1.0000 | 1.0000 | 0 | 0 | 高（strace 交叉验证） | **C2** |
| 24 | `wait_attempts` | 0.5513 | 0.5590 | +0.0077 | +1.39 [1.04, 1.70] | 高 | C4 |
| 25 | `cqe_reap_attempts` | 0.7893 | 0.7942 | +0.0049 | +0.63 [0.19, 1.08] | 高 | C4 |
| 26 | `cqes_reaped` | 1.0000 | 1.0000 | 0 | 0 | 高 | C2 |
| 27 | `progress_handoffs` | 0 | 0 | 0 | 0 | 高（单一 progress driver） | C4 |
| 28 | `wakeups` | 0 | **0.5589** | **+0.5589** | [0.5572, 0.5605]（绝对） | 高（确定性） | **C4** |
| 29 | `wakeup_writes` | 0 | **0.2353** | **+0.2353** | [0.2334, 0.2371]（绝对） | 高 | **C4/C3** |
| 30 | `wakeup_coalesced` | 0 | **0.3236** | **+0.3236** | [0.3208, 0.3264]（绝对） | 高 | C4 |

关键守恒关系（同块、逐 op）：`dispatch_passes = sqe_preparations = flushes = actual_enters = 1.0000`
在 A、B 两臂完全相同；B 仅把 `submit_attempts` 降低 30.96%（0.554/op），把 0.5589/op 的 SQE 延迟到
poll 再 flush，并新增等量的 `wakeups`。因此 **D2 没有任何 enter 摊销**；B 的“延迟 flush”只改变了
flush 的发生位置/时刻，没有减少内核提交次数。

### 3.3 调度/上下文（`/proc` 每线程；split-d2，窗口 420k op）

| # | 指标 | A/op | B/op | Δ% [CI95] | 置信度 | 可能域 |
|---|---|---|---|---|---|---|
| 31 | 提交线程 voluntary ctxt switches/op | 0.25209 | 0.24781 | −1.70 [−2.09, −1.28] | 中（proc 计数） | C4 |
| 32 | 提交线程 nonvoluntary ctxt switches/op | 0.49580 | 0.50432 | +1.72 [1.30, 2.12] | 中 | C4 |
| 33 | io-wq voluntary ctxt switches/op | 0.55206 | 0.56304 | +1.99 [1.35, 2.57] | 中 | C4 |
| 34 | io-wq nonvoluntary ctxt switches/op | 0.19574 | 0.18900 | −3.42 [−4.64, −2.03] | 中 | C4 |
| 35 | migrations/invocation（`se.nr_migrations`） | 1 | 1 | 0 | 中（分辨率 1 次/调用） | U |

上下文切换的变化幅度小（±2-3%），与“通知唤醒并不必然触发调度切换”一致：D2 中 progress driver
始终是 bench 线程（`progress_handoffs=0`），eventfd 通知多数被合并（0.3236/0.5589 = 57.9% 合并），
真正 write 的 0.2353/op 主要计入内核 CPU，而非调度切换。

### 3.4 D1 精确对照（`=W02-uring-4k-d1`）

> 方法更正：首轮 D1 使用子串过滤 `W02-uring-4k-d1`，它同时匹配 `W02-uring-4k-d16`，
> 导致计数器/PMU 混入 d16；该批 `raw/pmu-d1-ab`、`raw/cnt-*-full-d1`、`raw/split-d1` 已标记 DISCARDED，
> 本节只使用精确过滤 `=W02-uring-4k-d1` 重跑的 `raw/*-d1x` 块。

| 指标 | A | B | Δ% [CI95] | 结论 |
|---|---|---|---|---|
| CPU/op（cnt-on-full-d1x，6 对） | 6243.8 ns | 6193.8 ns | −0.80 [−1.82, 0.18] | 中性 |
| 吞吐（同块） | 160,039 | 161,327 | +0.81% [−0.16, 1.85]（B 略快，不显著） | 中性 |
| p95 / p99（同块） | 7131.6 / 7651.4 ns | 7102.3 / 7629.5 ns | −0.40% / −0.28% | 中性 |
| CPU/op（pmu-d1x-ab，8 对） | 7620.0 ns | 7462.1 ns | −1.92 [−4.12, 0.19] | 中性/略优 |
| `sqes_deferred` / `wakeups` / `actual_enters` / `flushes` | 0 / 0 / 1.0 / 1.0 | **0 / 0 / 1.0 / 1.0** | 0 | **机制在 depth=1 精确关闭** |
| `submit_attempts` | 2.3419 | 2.3401 | −0.08 [−0.15, −0.01] | 无实质变化 |

D1 的精确对照确认：B 在 depth=1 不产生任何 deferral、不增加 wakeup、enter/flush 完全不变，
CPU/吞吐/尾延迟均在 ±2% 内（不同块符号不完全一致，属块级漂移）。这同时排除了“计数器本身或
arm 选择器在 D1 引入系统偏差”的解释。

---

## 4. CPU 分解（提交线程 / 进度 owner / io-wq，防止重复计算）

来源：`split-d2`（12 对，PMU-only，warmup=1 measured=20；每 invocation 21 pass × 20k = 420k op；
外部采样器每 1 ms 读 `/proc/<bench>/task/*/{stat,status,sched}`，按 comm 分类，保留每 tid 单调最后值；
bench 与 io-wq 是同一线程组的不同线程，分别求和，不叠加）。

| 组成 | A（ns/op） | B（ns/op） | Δ（ns/op） | Δ% [CI95] | 占 +189.1 ns 的比例 |
|---|---|---|---|---|---|
| 提交线程 = 进度 owner：用户态 | 1319.44 | 1414.68 | **+95.24** | +7.39 [4.07, 10.55] | ≈50.4% |
| 提交线程 = 进度 owner：内核态 | 2019.84 | 2099.21 | **+79.37** | +3.99 [1.87, 6.17] | ≈42.0% |
| io-wq worker：内核态 | 1771.83 | 1779.76 | +7.94 | +0.46 [−0.44, 1.36] | ≈4.2%（不显著） |
| 三者合计 | 5111.11 | 5293.65 | +182.54 | — | ≈96.5% |
| 进程 CPU/op（bench 自身计量） | 5131.52 | 5320.66 | +189.15 | +3.70 [2.93, 4.55] | 100% |

结论：
- 增量几乎全部在**提交线程（同时是本形状的进度 owner）**：用户态 +95.2 ns/op、内核态 +79.4 ns/op。
- **io-wq 没有被卷入**：+7.9 ns/op，CI 含 0；任何“io-wq CPU 问题”的解释在本轮 D2 证据下不成立。
- 采样器合计与 bench 的 `CLOCK_PROCESS_CPUTIME_ID` 差 6.6 ns/op（≈3.5%），来自 10 ms tick 量化、
  线程创建/退出边界与采样丢点；不影响“用户/内核/io-wq”三分法的符号与量级。
- 内核态增量的唯一 B 专属 syscall 路径是 `ProgressSource::signal()` 的 eventfd write：0.2353 次/op；
  79.4 ns / 0.2353 ≈ 337 ns/write（推断值，非直接 syscall 时长测量）。

---

## 5. 探针开销（单独运行、不与 perf stat 叠加）

计数器探针开销用同一二进制 `bench_w02_pipeline_counters` 的 ON/OFF 块（full 模式，各 12 对）量化；
另用 OFF 块与冻结二进制 full 块（6 对）比较“计数器代码存在但关闭”的残余开销。独立组 bootstrap
相对差 CI（10,000 次、seed 5950）。

| 比较 | CPU/op Δ% [CI95] | 吞吐 Δ% [CI95] | 用户 CPU Δ% | 内核 CPU Δ% | p95 / p99 Δ% |
|---|---|---|---|---|---|
| ON vs OFF（full D2，12+12 对） | **+0.93** [−0.58, 2.40] | −0.91 [−2.32, 0.59] | +2.91 [0.08, 5.76] | +0.11 [−1.96, 2.14] | +0.65 / +0.83 |
| ON vs OFF（PMU-only D2，8+8 对） | **+1.97** [0.44, 3.58] | −1.85 [−3.38, −0.36] | — | — | — |
| OFF vs 冻结（full D2，12+6 对） | +0.79 [−0.66, 2.35] | −0.72 [−2.23, 0.72] | +4.07 [0.55, 7.98] | −1.09 [−3.15, 0.95] | +0.70 / +1.11 |

结论：
- 计数器探针的**总开销约 1-2% CPU/op、1-2% 吞吐**，约为 D2 退化幅度（3.7%）的 27-53%。
  因此计数器块**只用于路径/计数发现，不用于定量 CPU 归因**；定量归因取无计数器的 `split-d2`
  与 `pmu-d2-ab`。
- ON/OFF 的差分对 A/B 近似对称（两臂都执行同样的 enabled 检查）；B 专属的额外 bump 为
  `sqes_deferred`、`wakeups`、`wakeup_writes/coalesced`，合计约 2.2 次/op 的 enabled 检查，
  按每次 ~2-5 ns 估算 ≈4-11 ns/op（0.1-0.2%），不能解释 +189 ns/op。
- OFF 块与冻结二进制在 CPU/op 上不可区分（+0.79%，CI 含 0），说明计数器代码“关闭”时没有
  可测的残余 CPU 开销；计数器 ON 的 +0.93~+1.97% 是探针本身的成本，已在解释中扣除。

---

## 6. C1..C6 分支触发判定（按任务 §6 触发条件）

判定依据的任务 §6 触发条件（读自工作流草稿
`/home/jnhu/Source/Sluice/.zcode/workflow-drafts/Sluice-595-因果归因.dwf.ts:390`，分支规格同文件
`:231-236`；等价重述）：
用户态指令/循环计数变化→C1；提交行为疑问→C2；内核/io-wq CPU 或 syscall 时长变化→C3；
ctxsw/wakeup/runnable 变化→C4；指令数解释不了 cycles 或内存差异→C5；仅当设备路径参与→C6。

| 分支 | 触发 | 判定理由（证据） |
|---|---|---|
| **C1 用户态执行** | **是** | `instructions:u/op` **+1.06%** [0.97,1.16]（A 4709.9→B 4760.0）；`cycles:u/op` **+5.71%** [4.55,6.87]；提交线程用户 CPU **+95.2 ns/op（+7.39%）**。A/A 噪声 instructions +0.04%、cycles −0.29%，D2 增量远超。计数器把 B 专属的新增调用定位到 `signal_ready_progress()`→`ProgressSource::signal()`（每次 deferral 一次；0.5589/op）：mutex + epoch + eventfd 写路径；其用户态成本由 +95.2 ns/op 与调用计数共同支持（严格因果仍需 G10 的 residue-signal 反事实）。非含糊命名，见 §3.2 源码位置表。 |
| **C2 提交路径** | **是** | “enter 不变”确实掩盖了提交行为变化：`actual_enters` 与 `flushes` 两臂均 **1.0000/op**（strace 交叉验证 60,004 enter / 3 passes × 20k op），但 B `sqes_deferred` **0→0.5589/op**、`submit_attempts` **−30.96%**、flush 从 dispatch 时刻挪到下一次 poll。enter 数不变不等于提交路径不变；D2 的“零 enter 节省”正是需要 C2 深挖的核心事实。 |
| **C3 内核/io-wq** | **是（仅 syscall 上下文；io-wq 排除）** | 提交线程内核 CPU **+79.4 ns/op（+3.99%）** [1.87,6.17]；io-wq 内核 CPU **+7.9 ns/op（+0.46%，CI 含 0）**。B 唯一新增的内核路径是 eventfd write（0.2353 次/op，0.3236 次/op 合并）；79.4/0.2353 ≈ 337 ns/write 是推断，syscall 时长本身未直接测量（无 root，见 §7）。不能把内核 CPU 增量统称 io-wq；io-wq 在本轮是平的。 |
| **C4 调度器/唤醒** | **是（通知唤醒；调度级 runnable/off-CPU 未观测）** | `wakeups` **0→0.5589/op**（其中实际 eventfd write 0.2353/op、合并 0.3236/op）；`wait_attempts` +1.39% [1.04,1.70]；`poll_passes` +0.63%；提交线程 nonvoluntary ctxsw +1.72% [1.30,2.12]，io-wq voluntary ctxsw +1.99% [1.35,2.57]；`progress_handoffs=0`（driver 未切换）、migrations 0。**sched_wakeup/sched_switch/runnable delay 未观测**（无 root tracepoint/perf sched），所以 C4 的“通知/唤醒路径”成立，但“调度级延迟因果”只能标 CORRELATION_PARTIAL。 |
| **C5 内存/CPU 微架构** | **是（介导域）** | `instructions:u` 只解释 cycles 增量的 1.06/5.71 ≈ 18.6%；IPC **−4.36%** [−5.42,−3.28]；`cache-references:u` **+23.31%** [12.73,35.18]、`branch-misses:u` **+14.94%** [12.88,16.80]；`cache-misses:u` +0.65%（不显著）、page faults +0.15%（不显著）。即“指令数解释不了 cycles”，符合 C5 触发条件。但这是新增 signal/syscall 路径的**微架构后果**，不是独立的内存足迹/缺页根因；C5 应归为 mediating，而非 originating。 |
| **C6 存储设备** | **否** | 负载是 warm tmpfs 上 4 KiB positional read（`filesystem=tmpfs`、`cache_policy=page_cache_resident`，bench meta）；page faults +0.15%（不显著），没有 block 层/设备路径证据；无 root 也未启用 block tracepoint。按任务 §6，不得宣称 SSD/设备导致 D2。 |

分类（originating / mediating / observed）：
- **originating（候选）**：C2 的 defer/flush 策略 + 其附带的 residue 进度通知（`signal_ready_progress`）。
- **mediating**：C1（用户态 signal/mutex/eventfd 代码）、C3（提交线程内核 syscall 上下文）、
  C4（通知唤醒；调度级部分未观测）、C5（IPC/branch/cache-ref 微架构后果）。
- **observed symptom**：CPU/op +3.70%、吞吐 −3.56%、p95 +4.25%、p99 +6.98%。
- **未观测/未知**：内核态 PMU（instructions:k/cycles:k）、syscall 时长、sched runnable/off-CPU、
  每次 eventfd write 的精确成本、C6 设备路径（不适用）。

**竞争解释与排除**：
1. “io-wq CPU 增加” → 排除：io-wq +0.46% CI 含 0（split-d2）。
2. “缺页/内存足迹” → 排除：page-faults +0.15% 不显著；cache-misses +0.65% 不显著。
3. “cache 缺失导致” → 部分排除：cache-misses 平，但 cache-references/branch-misses 升；
   更像新增代码路径的取指/分支/锁访问，而非数据缺失。
4. “enter 数减少带来收益/成本” → 排除：两臂 enter 均 1.0/op，D2 零 enter 节省。
5. “设备 I/O” → 排除/不适用：warm tmpfs。
6. “频率漂移解释全部” → 部分：块间 CPU 幅度 2.67-4.52% 有漂移，但块内 A/A 0.65 pp、
   方向一致；用户/内核 CPU 的线程级分解与计数器账本给出独立于频率的结构性证据。
   剩余不确定性见 §7 G5/G6。

---

## 7. 观测边界（UNOBSERVED / OBSERVABILITY_LIMIT_REACHED / CORRELATION_PARTIAL）

- **内核态 PMU 未观测**（OBSERVABILITY_LIMIT_REACHED）：`perf_event_paranoid=2`，本会话无 sudo/
  CAP_PERFMON；`cycles:k`/`instructions:k` 实测 `No supported events found`。因此 instructions/cycles/
  cache/branch 均为**用户态**；内核态 CPU 只能用 `/proc` stime（10 ms tick）。这是本轮最大的观测边界。
- **syscall 时长未观测**（UNOBSERVED）：只有内核 CPU 时间与 eventfd write 次数；未测单次
  `write(eventfd)`/`io_uring_enter` 时长。337 ns/write 是除法推断。
- **调度级 runnable/off-CPU 未观测**（OBSERVABILITY_LIMIT_REACHED）：无 root，sched tracepoint/
  perf sched 不可用；C4 只到 ctxsw 计数与通知计数（CORRELATION_PARTIAL）。
- **PMU 复用**：本方案 5 个硬件事件（2 fixed + 3 generic），实测 run_time 100.00%，无复用误差；
  8 事件方案的复用（37-62%）没有被使用。
- **R1 enter 口径**：R1 的 3.0002 enters/op 是 3 个 pass 的总 enter 除以 20k measured op；
  本轮的 `actual_enters` 是每次操作的 submit enter（1.0/op）。两者一致，但不可直接与“每 op 3 次”
  的直觉混用；已在 §3.2 用 strace 交叉验证。
- **D1 首轮污染**：子串过滤 `W02-uring-4k-d1` 同时匹配 `W02-uring-4k-d16`；首轮 D1 块作废
  （`raw/DISCARDED.md`），D1 结论只取精确过滤 `=W02-uring-4k-d1` 的 `*-d1x` 块。
- **成本模式与 phaseA 的差异**：PMU-only/复用 fixture 改变绝对 CPU/op（split A 5131.5 vs phaseA
  A 4776.7 ns）；A/B 差值可用，绝对值不可与 phaseA 直接相减。D1x/冻结块的块间漂移同样限制绝对值比较。

---

## 8. 证据路径

| 内容 | 路径 |
|---|---|
| 归一化表（本文件） | `/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/cost_table.md` |
| C1 深挖（perf 用户态调用栈 + TSC 聚合计数器；单次成本/次数/探针开销） | `evidence/traces/perf-callgraph/README.md`、`evidence/counters/C1_COUNTERS.md` |
| 统计汇总（bootstrap） | `/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/analysis/summary.json` |
| PMU D2 A/B + A/A | `evidence/counters/raw/pmu-d2-ab/`、`raw/pmu-d2-aa/` |
| PMU D1 精确对照 | `evidence/counters/raw/pmu-d1x-ab/` |
| 无计数器 split（主复现） | `evidence/counters/raw/split-d2/`、`raw/split-d1x/` |
| 计数器 ON/OFF full D2 | `evidence/counters/raw/cnt-on-full-d2/`、`raw/cnt-off-full-d2/` |
| 计数器 ON/OFF PMU-only D2 | `evidence/counters/raw/cnt-on-pmu-d2/`、`raw/cnt-off-pmu-d2/` |
| 计数器 ON/OFF D1 精确 | `evidence/counters/raw/cnt-on-full-d1x/`、`raw/cnt-off-full-d1x/` |
| 冻结二进制当轮对照 | `evidence/counters/raw/frozen-full-d2/` |
| 二进制 | `evidence/counters/bin/bench_w02_pipeline_{frozen,pmu,counters}` |
| runner / 分析脚本 | `evidence/counters/tools/run_blocks.py`、`tools/analyze_cost.py` |
| 作废的 D1 首轮 | `evidence/counters/raw/DISCARDED.md` 所列目录 |
| 计数器源码（研究副本） | `worktree/include/sluice/async/detail/research_595_counters.hpp`；插入点见 §3.2 |
| 构建 | `xmake f -c -y --liburing=y -m release`；`SLUICE_595_COUNTER_BUILD=1` 时定义 `SLUICE_595_COUNTERS`（`xmake.lua:24`） |
