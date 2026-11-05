# C4 调度域定量结果（schedstat 无 root 直接测量）

Round: #595 E5 causal-r2 C4 deep-dive（2026-11-04）。分支任务：sched_wakeup/sched_switch/
sched_waking、runnable delay、sleep duration、wakeup-to-run、上下文切换、迁移、唤醒目标、
抢占；区分等待提交/等待 I/O/等待 CPU/主动执行；off-CPU 时间不计为 CPU 工作。

## 0. 结论摘要

- **D2 退化在本块完整复现**（frozen 二进制、full 模式、measured=20）：CPU/op
  +3.11% [2.40,3.78]（ON 12 对）、无采样 OFF 块 +3.88% [2.64,5.37]、ON bracket 尾块
  +3.88% [3.04,4.75]，与 Phase B split-d2 +3.70% [2.93,4.55] 一致。
- **提交线程（=进度 owner，progress_handoffs=0）runnable delay：+36.4 ns/op
  （+0.94% [+0.38,+1.50]，18 对合并）**。方向为正但量级是同窗口 on-CPU 增量
  +474.8 ns/op 的 7.7%。调度排队不是 D2 主体。
- **退化在执行态，不在等待态**：on-CPU +4.62% [4.06,5.23] 显著；timeslices（调度段数）
  +0.12% [−0.04,+0.30] 不变；sleep（park/block）推导 −0.74% [−2.15,+0.55] 不变；
  migrations 提交线程 1/invocation 两臂相同、io-wq 0、CPU 恒为 cpu2。
- **B 的 0.5589/op "wakeups" 不产生调度唤醒**：源码证明 eventfd 写点在 dispatch 持锁
  路径（`src/async/uring_transport.cpp:119` → `include/sluice/async/detail/
  progress_source.hpp:492`），写者=提交线程且必然在运行中，而唯一 poll 者也是该线程
  （ProgressSource::park，progress_source.hpp:208-218）→ 写时 waitqueue 上没有睡眠者，
  无 sched_wakeup 目标。该计数是用户态通知计数，不是内核调度事件（§5）。
- io-wq：runwait +0.64% [+0.07,+1.17] / on-CPU +0.60% [−0.03,+1.18]（CI 含 0）/
  timeslices 不变 / voluntary ctxsw +0.73% [+0.22,+1.27] / nonvol −2.09% [−3.26,−0.96]。
  worker 行为轮廓不变；sleep 推导 +8.76% 中 lifetime 仅 +3.09%（=wall 伴随），纯 park
  拉长是伴随量，不是 CPU 成本。
- **G2 停止条件判定**：runnable delay 总量（+36.4 ns/op 点估计）远小于可解释 CPU 增量
  所需的量级，且 off-CPU 时间不计为 CPU 工作 → **C4 定级为"通知路径（mediating）"，
  调度级延迟不是 D2 根因**（预注册停止条件原文：<5% 的 CPU 增量则降级；实测 off-CPU
  排队增量按最宽松口径也只有同块 wall 增量的 ~23%、on-CPU 增量的 7.7%，无独立根因证据）。
- UNOBSERVED（root-only，本会话实测不可用，见 `../traces/raw-c4/obs-01-sched-tracepoint-noroot.txt`）：
  sched_wakeup/sched_waking/sched_switch 逐事件序列、逐事件 wakeup→run 延迟分布、
  sched_stat_sleep/blocked/iowait、抢占者身份（prev/next 谁）。代理：runwait/timeslices
  = 1714→1721 ns/段（A→B，基本不变）。

## 1. 方法

- 二进制：`counters/bin/bench_w02_pipeline_frozen`，sha256
  `61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886`（与 phaseA/provenance
  一致，frozen build 无任何研究探针）。运行：`taskset -c 2`，fixture 复用
  `/tmp/sluice-595-cost`（warm tmpfs），`SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=20
  SLUICE_BENCH_STATS=1 SLUICE_BENCH_REUSE_FIXTURE=1`，case `W02-uring-4k-d2`，
  arm 由 `SLUICE_595_ARM` 选择。偶对 A→B、奇对 B→A。
- 采样器（`tools/run_schedstat_blocks.py`）：独立线程 1 ms 轮询，逐 tid 单调末值：
  `/proc/<tid>/schedstat`（on_cpu_ns / runqueue_wait_ns / timeslices，ns 精度内核记账，
  CONFIG_SCHEDSTATS）、`/proc/<tid>/stat`（utime/stime tick + processor）、
  `/proc/<tid>/status`（vol/nonvol ctxsw）、`/proc/<tid>/sched`（nr_switches 三元组 +
  `se.nr_migrations`）；runner 进程整体 pin cpu5（避开 bench 的 cpu2 与其 SMT 兄弟）。
  `/proc/pressure/cpu` some total 每进程前后各读一次。`--no-sampler` 为零读取 OFF 臂。
- 窗口口径（与 Phase B split-d2 相同）：/proc 累计值 ÷ ops_window=(1+20)×20000=420k
  （/proc 计数覆盖含 warmup 的整个 invocation）；bench 自报告 CPU/吞吐/分位数用
  measured-only（400k op）。
- 块（顺序执行，机器独占；48/48 invocations rc=0；loadavg 起 0.17 终值见 raw）：
  1. `raw/schedstat-d2/`：12 对，采样 ON。
  2. `raw/schedstat-off-d2/`：6 对，采样 OFF（探针开销对照）。
  3. `raw/schedstat-d2-br/`：6 对，采样 ON（时间漂移 bracket）。
- 统计：pair 相对差 (B−A)/A，bootstrap 95% CI（10,000 重采样、seed 5950、percentile，
  与 analyze_cost.py 同约定）。合并 18 对 = 主块+bracket（同日、同协议、同二进制）。
- 时钟源：schedstat 为内核 ns（TSC 基）；/proc stat 为 10 ms tick；PSI µs；bench CPU 为
  CLOCK_PROCESS_CPUTIME_ID；wall 为 steady。跨仪器不做时间轴对齐，A/B 比较限单仪器内。
- 交叉验证：timeslices == nr_switches（smoke 实测 136,720==136,720，逐对一致）；
  status ctxsw 与 sched nr_{vol,}involuntary 双口径逐对一致；utime+stime tick 与
  on_cpu_ns 口径一致（10,301 vs 10,326 ns/op，0.2% 内）；processor 字段实测恒 cpu2
  （首轮脚本 index 错位读成 policy 字段，已修复并重跑 smoke 验证后才开始正式块）。

## 2. 提交线程（bench_w02 单线程）调度分解 — 主结果（合并 18 对）

| 指标（每 op，窗口口径） | A | B | Δ绝对 | rel% [CI95] | 判定 |
|---|---|---|---|---|---|
| on-CPU（schedstat[0]，含 user+kernel） | 10,287.4 | 10,762.2 | +474.8 [+418,+535] | +4.62 [+4.06,+5.23] | 显著：执行变贵 |
| runnable delay（schedstat[1]） | 3,919.3 | 3,955.7 | +36.4 [+14.5,+58.2] | +0.94 [+0.38,+1.50] | 显著但极小 |
| timeslices（schedstat[2]，调度段数） | 2.298 | 2.301 | +0.003 | +0.12 [−0.04,+0.30] | 不变（n.s.） |
| 平均时间片（oncpu/slices） | 4,477 | 4,677 | +200 | +4.5 | oncpu 增、段数不增 |
| voluntary ctxsw（status==sched 双口径） | 0.703 | 0.700 | −0.003 | −0.40 [−0.97,+0.14] | n.s. |
| nonvoluntary ctxsw（双口径） | 1.595 | 1.601 | +0.006 | +0.36 [−0.12,+0.88] | n.s.（Phase B +1.72% 同向） |
| sleep/park 推导（lifetime−oncpu−runwait） | 1,285.5 | 1,280.2 | −5.3 | −0.39 [−1.42,+0.56] | n.s.（DERIVED） |
| utime（tick） | 4,168.0 | 4,456.3 | +288.4 | +6.96 [+5.79,+8.17] | 显著（同 Phase B 用户 +7.39% 方向） |
| stime（tick） | 6,095.2 | 6,284.4 | +189.2 | +3.13 [+2.14,+4.14] | 显著 |
| migrations（se.nr_migrations，绝对/inv） | 1.0 | 1.0 | 0 | 0 | 两臂相同，CPU 恒 cpu2 |
| wakeup→run 代理（runwait/slice，ns/段） | 1,714 | 1,721 | +7 | +0.4 | 代理量，不变 |

主块（12 对）单独：runwait +22.2 [−0.06,+1.17]（CI 含 0）、on-cpu +4.44 [+3.85,+5.07]；
bracket 块（6 对）runwait +64.8（同向）。合并后显著。两块同向、与 Phase B ctxsw
nonvol +1.72% [1.30,2.12] 方向一致（本块 full 模式 + 5 文件采样下幅度缩小到 +0.53%
n.s.——模式与干扰稀释，方向不冲突）。

## 3. io-wq worker 调度分解（合并 18 对）

| 指标 | A | B | Δ绝对 | rel% [CI95] |
|---|---|---|---|---|
| on-CPU | 5,164.0 | 5,194.3 | +30.3 [−2.6,+60.6] | +0.60 [−0.03,+1.18]（n.s.） |
| runnable delay | 5,102.5 | 5,134.7 | +32.1 [+2.7,+59.3] | +0.64 [+0.07,+1.17] |
| timeslices | 2.297 | 2.301 | +0.004 | +0.18 [−0.01,+0.41]（n.s.） |
| 平均时间片 | 2,248 | 2,258 | +10 | +0.4（不变） |
| voluntary ctxsw | 1.78 | 1.79 | +0.01 | +0.73 [+0.22,+1.27] |
| nonvoluntary ctxsw | 0.517 | 0.507 | −0.010 | −1.93 [−3.08,−0.85] |
| migrations（绝对） | 0 | 0 | 0 | CPU 恒 cpu2 |
| stime（tick） | 5,150.8 | 5,179.9 | +29.1 | +0.58 [−0.04,+1.17]（n.s.） |
| sleep 推导 | 5,155.1 | 5,605.9 | +450.8 | +8.76 [+8.02,+9.55]（DERIVED，见下） |

io-wq sleep 推导 +8.76% 的分解：worker lifetime B/A=+3.09%（=invocation elapsed +3.02%，
wall 变慢的伴随——worker 生存期被拉长）；扣除后平均 park 时长拉长 ~5.5%。worker 的
park/wake 次数（timeslices、vol ctxsw）与 on-CPU 均不变，故这是 D2 退化的**结果**
（整体节奏变慢，worker 等下一次唤醒更久），不是原因；off-CPU park 时间**不计为
CPU 工作**。

## 4. 系统级与探针开销

- PSI cpu some-total：6.14 → 6.25 µs/op（+2.14% [−1.54,+6.15]，n.s.；全系统口径，
  含采样器自身，仅辅助）。
- **采样器开销（1 ms /proc 轮询 × 5 文件/tid）远超 D2 效应，绝对值禁用于定量**：
  跨块非配对对比（ON 12 对 vs OFF 6 对，含块间漂移 ±1.2%，见 ON-br A 臂 −1.2%）：
  bench measured CPU/op A 臂 4,989 vs 4,669（+6.9%）、B 臂 5,144 vs 4,849（+6.1%）；
  吞吐 A 200.2k vs 214.0k（−6.5%）、B 194.2k vs 206.1k（−5.8%）；p95 +6.0%；p99
  A 18,535 vs 13,589（+36%）。即采样本身对 CPU/op 的扰动 +5~8%、对 tail +30%+，
  均大于 D2 效应（3.7%/7%）。
- 因此本文件所有 /proc 绝对值（如 runwait 3,919 ns/op）是**被采样器干扰下的读数**；
  定量结论只使用块内 A/B 相对差。采样器 arm-blind（不知道臂、两臂线程拓扑相同：
  bench 1 线程 + iou-wrk 1 线程，18/18 对验证），三块 A/B 相对差互相一致
  （CPU +3.11/+3.88/+3.88%）且与无采样 Phase B（+3.70%）一致——支持"干扰对称、
  相对差无 >0.8pp 偏倚"；不能排除采样器对 runwait 绝对值的系统性抬高（读
  /proc/<tid>/schedstat 需要内核侧 task 统计同步，可能与目标线程调度路径竞争），
  runwait 的 +36.4 ns/op 应视为上界读数。

## 5. 唤醒目标与抢占（源码级论证 + 计数一致性）

- 写点：`UringAsyncBackend::dispatch_after_accept_locked_()` 尾部
  `advertise_residue` 分支调 `signal_ready_progress()`（`src/async/uring_transport.cpp:115-120`，
  调用点 :119）；
  下沉到 `ProgressSource::signal()` → `wake_notification_nolock_()`（progress_source.hpp:472-518），
  `::write(notification_fd_, 1)` 在 :492。执行者是 dispatch 线程本身，且此刻持有
  transport 锁、正在运行——**不可能同时 park 在 poll 上**。
- 唯一 poll 点：`ProgressSource::park()`（progress_source.hpp:208-218）`::poll(eventfd,
  POLLIN, timeout)`；poll 者是进度 owner。Phase B `progress_handoffs=0`：owner ≡ bench
  提交线程。**⇒ 写者与唯一可能 park 者是同一线程的不同时刻：write 发生时 eventfd
  waitqueue 上没有睡眠任务，内核 wake_up 无目标，不产生 sched_wakeup。**
- B 的信号效果因此是计数器式的：eventfd counter 非零 → 下一次 park 的 drain 多一次
  read（C3 已证 drain 面 B−A read 次数 +1.12/drain ≈ write/drain=1.000 闭环）→ poll
  100% 立即 POLLIN 返回（C3 实测零超时）→ **park 从不因信号而睡眠**。与本文件
  sleep 推导不变（−0.74% n.s.）、timeslices 不变（+0.12% n.s.）一致。
- 抢占（nonvoluntary ctxsw）+0.36~+1.72%（本块合并 n.s./Phase B 显著）绝对量 ~0.006/op；
  若按每抢占 1–2 µs wall 粗算 ≤18 ns/op——只够解释 runwait 增量的一部分，且方向
  是时间片竞争的伴随，不是 189 ns/op CPU 的来源（CPU 增量在 on-CPU 内，C1/C3/C5 域）。
- 内核侧 eventfd 写者（io_uring_register_eventfd 的 CQE 完成通知，progress_source.hpp:563
  注册借用）两臂等量（A counter 4.03/drain vs B 5.15，差 = B 用户写），不是 A/B 差异源。

## 6. 四分类账（提交线程，窗口口径，18 对合并）

| 状态 | A ns/op | B ns/op | Δ | 说明 |
|---|---|---|---|---|
| 主动执行（on-CPU） | 10,287 | 10,762 | **+475（+4.62%）** | D2 退化全部在此（C1/C3/C5 分解） |
| 等待 CPU（runnable） | 3,919 | 3,956 | +36（+0.94%） | 显著但极小；非 CPU 工作 |
| 等待提交/等待 I/O（off-CPU park，推导） | 1,290 | 1,280 | −10（n.s.） | DERIVED=lifetime−oncpu−runwait；D2 下 poll 立即返回、submit-only enter 从不阻塞（C2），此桶≈非关键 park |
| 迁移 | 0 | 0 | 0 | 1/invocation 固定值（进程启动期），稳态 0 |

io-wq 的对应账：执行 5,164→5,194（n.s.）、等 CPU 5,103→5,135、park 推导 +451（伴随量）。
等待 I/O 与等待提交在本负载下不可再分（无 block 层、tmpfs page_cache_resident、C6 不适用；
逐 park 原因需 sched_stat_blocked/iowait，root-only，UNOBSERVED）。

## 7. 原始数据与复现

```
python3 -I evidence/counters/tools/run_schedstat_blocks.py --label schedstat-d2 \
  --bin evidence/counters/bin/bench_w02_pipeline_frozen --pairs 12 --measured 20 \
  --outdir evidence/counters/raw/schedstat-d2
# 同：--no-sampler --pairs 6 (schedstat-off-d2)；--pairs 6 (schedstat-d2-br)
python3 -I evidence/counters/tools/analyze_c4.py \
  "evidence/counters/raw/LABEL/results.jsonl"   # -> analysis/c4-schedstat.json（分块）
```

合并 18 对统计与上表由 `analysis/c4-schedstat.json` 加一段合并重采样（同 seed 5950）
得出；本文件表格数字直接来自该 JSON。环境：kernel 7.2.11-200.fc44；g++ 无关（不重编）；
loadavg 起 0.17、块内最大 ~2.0（48 invocations 串行自身）；cpu2 请求频率 1.197 GHz
（schedutil 请求值，非实测频率，G6 已登记）。
