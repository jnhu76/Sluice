# C1 用户态执行深挖：TSC 聚合计数器（Sluice #595 causal-r2）

状态：ACTIVE EVIDENCE。轮次：causal-r2 C1（2026-11-04）。
范围：D2（`W02-uring-4k-d2`，4 KiB positional read，depth=2）A·IMMEDIATE vs B·BOUNDED_PROGRESS。
配套 trace：`evidence/traces/perf-callgraph/README.md`（perf 用户态调用栈，路径发现）。
Phase B 成本表：`evidence/counters/cost_table.md`。

## 0. 结论先行（直接测量 / 推断 / 未知）

**直接测量（本文件主结果）**

1. B 新增的执行次数（每 measured op，本块 100k op/invocation）：
   `signal_ready_progress()`→`ProgressSource::signal()` **0 → 0.5621/op**，
   实际 `::write(eventfd)` **0 → 0.2300/op**，合并（`notification_pending_` 已置位）**0.3320/op**；
   `defer_transport_flush_locked_()` 调用次数两臂均 **1.0000/op**，但 B 每次都执行完整的
   owner/payload/witness/cap 门（owner pass 1.0000/op、witness pass 0.5621/op）。
   `flush`（实际携带 SQE 的 `io_uring_submit`）两臂均 **1.0000/op**；`submit_transport_locked`
   调用 A 1.7900 → B 1.2301/op（少掉的是空 ledger 调用，不是 flush）。
2. 单次调用成本（in-situ TSC，`clean` 已剔除 >1.04 µs 的抢占尾部）：
   - `ProgressSource::signal()`：B **277.5 ns/call** [275.3, 280.0]（本块 n=12），
     其中 mutex+epoch 基座 **35.3 ns/call** [34.9, 35.8]（含 ~9.9 ns TSC pair），
     eventfd write **592.5 ns/call** [589.8, 595.4]（write 发生率为 0.230/0.562 = 40.9%）。
   - `defer_transport_flush_locked_()`：A **10.2 ns/call**（≈TSC pair 基线 9.9 ns，实际早退≈0）
     → B **16.8 ns/call** [16.8, 16.9]，**+6.6 ns/call（+64%）**，即新增状态检查的直接成本。
   - flush 提交（`io_uring_submit`，含 syscall 往返的 on-CPU 时间）：A **702.3 ns/call**
     [700.2, 704.8] → B **714.4 ns/call** [711.7, 717.7]，**+12.1 ns/call（+1.7%）**；
     flush 次数不变（1.0/op），所以这是“同一 flush 从 dispatch 挪到 poll”的每调用成本变化，
     不是新增 call。
   - `poll()`：clean A 366.8 → B 658.0 ns/call；B 的 poll 承担了 flush，**不是额外工作**
     （flush 总数不变），原始均值被 42% 的 >1 µs 尾部拉高（clean fraction B 0.579）。
3. 新增工作量对 CPU/op 增量的解释（同块、同二进制、无其他 instrumentation）：
   - 本块 OFF（无探针）CPU/op：A 5485.7 → B 5717.4 ns，**+231.7 ns/op（+4.243% [3.228,5.238]）**。
   - C1 直接测得新增路径：signal clean 155.8 ns/op [154.7,157.0] + defer 增量 6.6 ns/op
     = **162.4 ns/op（原始）**；扣除 signal/write 两个 TSC pair 基线（9.85 ns × (0.562+0.230)
     = 7.8 ns/op）后 **≈154.6 ns/op**。
   - 即 **≈67% 的本块效应 / ≈82% 的 Phase B 中央效应（+189.15 ns/op）** 可由新增
     signal+defer 工作量直接解释。跨三块（效应 158–232 ns/op）比例为 67–94%，中心约 80%。
   - **口径限制**：完整探针集的 B 开销为 +2.72%（≈155 ns/op），与效应同量级；虽然
     interval 内只含结束侧 fence（用 A defer 的 10.2 ns 作 in-situ 标定，已在 154.6 中扣除），
     按任务纪律仍把该比例视为**直接区间测量的上限口径**，精确份额标 CORRELATION_PARTIAL；
     无仪器的 OFF 块/Phase B 才是定量效应来源。
   - 分解：signal 的 write 部分 clean **136.0 ns/op** [134.9,137.1]（on-CPU 跨越用户/内核，
     以内核为主）；mutex+epoch 基座 clean **19.9 ns/op**；defer 增量 **6.6 ns/op**。
4. 用户/内核归属：**直接新增用户态执行 ≈24–27 ns/op**（mutex+epoch ≈14–17、defer ≈6.6、
   write 用户侧 stub ≈3.7〔trace 的 `__GI___libc_write` 8.5 cycles/op @2.3 GHz〕）。
   Phase B split-d2 的用户态增量 +95.24 ns/op 中，只有约 **25–28%** 是新增用户态指令执行；
   其余是 cycles 增长而 instructions 仅 +1.06% 的停顿/微架构部分（C5 介导）。
   eventfd write 的 on-CPU 时间 132–136 ns/op 与 Phase B 内核净增量 +79.37 ns/op
   [37.8,124.6]（按 CI 换算）处于同一上界附近：write 可能覆盖全部内核净增量但 `/proc` tick
   估计偏低，也可能存在其他内核路径的节省相互抵消。**C3 负责内核分解，本文件不下结论。**
5. 探针开销（同二进制 ON/OFF，独立组 bootstrap）：
   - 最小探针集（defer/signal/write/poll）：CPU/op A **+0.65%** [0.07,1.23]、B **+1.96%** [1.04,2.78]；
     吞吐 A −0.58%、B −1.88%；p95/p99 CIs 含 0；探针校正后 A/B 效应 +3.515%，与 OFF +3.518% 一致。
   - 完整探针集（+submit/flush）：CPU/op A **+3.05%** [2.13,3.93]、B **+2.72%** [2.04,3.37]；
     吞吐 A −2.90%、B −2.60%；p95 A −0.49%、B +1.18%（CI 含 0）；p99 CIs 含 0；
     校正后效应 +4.241%，与 OFF +4.243% 一致。
   - 完整探针开销已达效应量级的 64–72%（B 2.72%/4.24%），按任务纪律：**ON 块只用于
     路径/计数/单次成本，不作为定量效应估计**；定量效应取 OFF 块与 Phase B split-d2。
6. 未解释残差：Phase B 中央效应 189.15 − 直接新增 ≈154.6 = **≈35 ns/op（18%）**；
   本块残差 ≈77 ns/op（33%）。已测到的 flush 每调用 +12.1 ns/op 只能解释其中一部分；
   其余留在 C5（微架构/layout/缓存）与 C3（内核路径）候选，标 **UNOBSERVED**。

**反事实支持（非本分支直接测量）**

- Phase B 的计数账本 + 本文件的源码路径一致性 + 同块 OFF 效应，构成“defer 附带的 residue
  signal 是主要成本”的强证据；但**未运行 G10 的 signal-drop mutant**，严格因果闭环留给
  G10/相应分支（见 `EVIDENCE_GAPS.md` G10）。

**未知 / 不可观测**

- 内核态 PMU（`instructions:k`/`cycles:k`）不可用（paranoid=2、无 root/CAP_PERFMON）；
  write 的内核/用户切分只能从 TSC on-CPU 时间与 `/proc` stime 两侧夹逼。
- TSC 区间在极少数抢占样本中包含 off-CPU 时间；已用 clean 阈值剔除（signal/write clean
  fraction 0.999/0.998），但 poll/flush 的 clean fraction 只有 0.579/0.404，其 raw 均值不可用于定量。
- `pthread_mutex_lock` 单次锁成本未单独计时；基座值是 lock+unlock+epoch+分支的合并值。

---

## 1. 方法与实验设计

### 1.1 为什么用源码聚合计数器

perf 用户态调用栈（`traces/perf-callgraph/`）可定位 B 专属符号（`__GI___libc_write`、
`dispatch_after_accept` 自采样增加），但：

- release 二进制无帧指针/DWARF，caller 链含伪帧，调用者级归因不可靠；
- `defer_transport_flush_locked_` 只有 ~10–17 ns，采样 18k 样本/400k op 完全采不到（0 样本）；
- 采样无法给出执行次数与单次成本，只能给符号时间份额。

因此按任务要求对同一函数加**低开销聚合计数器**（count + TSC sum/max + clean 子集），而不是
输出全量事件。

### 1.2 探针点与源码位置（研究副本 `/home/jnhu/sluice-595-e5/causal-r2/worktree`）

| 探针 | 位置 | 语义 |
|---|---|---|
| `Agg::defer` scope | `src/async/uring_transport.cpp:92`（dispatch_after_accept 调用点） | 包住 `defer_transport_flush_locked_(h)` 调用 |
| `defer_owner_pass` | `src/async/uring_transport.cpp:249` | owner 门通过（B 才执行） |
| `defer_witness_pass` | `src/async/uring_transport.cpp:266` | witness 门通过（= 最终 deferral 数） |
| `Agg::signal` scope | `include/sluice/async/detail/uring_backend.hpp:337` | 包住 `progress_port_.signal()`（函数内联） |
| `signal_entry` | `include/sluice/async/detail/progress_source.hpp:261` | `ProgressSource::signal()` 入口 |
| `signal_coalesced` | `include/sluice/async/detail/progress_source.hpp:438` | `notification_pending_` 命中、跳过 write |
| `Agg::write` | `include/sluice/async/detail/progress_source.hpp:446-456` | 包住每次 `::write(eventfd)` 尝试 |
| `Agg::submit` scope | `src/async/uring_transport.cpp:279` | 整个 `submit_transport_locked()` 调用 |
| `Agg::flush` scope | `src/async/uring_transport.cpp:287` | 包住 `::io_uring_submit()`（含 syscall 往返） |
| `Agg::poll` scope | `src/async/uring_backend.cpp:627` | 整个 `poll()` |
| 计数头 | `include/sluice/async/detail/research_595_c1.hpp` | `Entry{count,tsc_sum,tsc_max,clean_count,clean_sum,huge_count}`、`Scope`、`record`、`calibrate`、`dump` |
| bench 接线 | `bench/bench_w02_pipeline.cpp:361/483/662` | measured 吞吐 pass 使能/reset、dump/disable；进程启动时 TSC 校准 |

- 头文件整体由 `SLUICE_595_C1_COUNTERS` 保护；未定义时只展开空宏，不产生 TLS/代码。
- 计数器为 `thread_local` 非原子（measured 路径全部在 bench driver 线程；Phase B
  `progress_handoffs=0`，同一线程即进度 owner），避免每事件 atomic RMW 偏置。
- TSC：`lfence;rdtsc` 起始、`rdtscp;lfence` 结束；`tsc_hz` 用 `CLOCK_MONOTONIC` 5 ms 忙等校准，
  本机实测 **2.893 GHz**；空 pair 实测 **9.8–9.9 ns**（28.4–28.6 ticks）。
- clean 阈值 3000 ticks ≈ 1.04 µs；`huge` >30000 ticks（≈10.4 µs）。
- 只在 measured 吞吐 pass 使能（oracle/latency/warmup 不计），每 rep reset，dump 到 stderr
  （`SLUICE595_C1 ...`），由 `tools/run_c1_blocks.py` 解析。

### 1.3 实验设计（交错 ON/OFF）

`tools/run_c1_blocks.py`：每个 pair 4 个 invocation：

- 偶数 pair：`OFF-A, OFF-B, ON-B, ON-A`
- 奇数 pair：`OFF-B, OFF-A, ON-A, ON-B`

这样 OFF/ON 两个 A/B 对比各自平衡臂序，且每臂的 ON/OFF 对比在时间上相邻（同 pair）。
12 pairs × 4 = 48 invocations，全部 rc=0。full 模式（warmup=1、measured=5，保留 oracle/latency
语义；tail 来自 latency pass，计数器在该 pass 关闭）；无 perf stat/其他 instrumentation
（不与 C1 源码探针叠加）；外部 sampler 每 1 ms 读 `/proc/<pid>/task/*` 做线程 CPU 分解。

运行环境：bench `taskset -c 2`；sampler/perf 不在 bench 核；机器独占（run 期间无其他负载）；
cpu2 `scaling_cur_freq` 1.2 GHz（schedutil 请求值，实测频率不可得）；loadavg 见各 raw 目录
`env-before/after.txt`。

## 2. 结果：计数账本（final run，`raw/c1-interleaved-d2`，每 measured op）

| 计数 | A | B | 说明 |
|---|---|---|---|
| `defer_count` | 1.00000 | 1.00000 | `defer_transport_flush_locked_` 调用数 |
| `defer_owner_pass` | 0 | 1.00000（99990/100000） | B owner 门通过 |
| `defer_witness_pass` | 0 | 0.56208 | witness 门通过 = 最终 deferral |
| `signal_count` | 0 | 0.56208 | `ProgressSource::signal()` 入口 |
| `write_count` | 0 | 0.22997 | 实际 eventfd write 尝试 |
| `signal_coalesced` | 0 | 0.33211 | pending 位已置位而跳过 write |
| `poll_count` | 0.78998 | 0.79215 | +0.27%（Phase B：+0.63% [0.19,1.08]） |
| `flush_count` | 1.00000 | 1.00000 | 实际携带 SQE 的 submit |
| `submit_count` | 1.78998 | 1.23007 | 含空 ledger 调用；B −31.3%（Phase B −30.96%） |

守恒：`flush = 1.0/op` 两臂相同；B 的 `write + coalesced = 0.5621 = signal = witness pass`；
`submit 减少量` 对应 dispatch 端不再调用 submit（0.4411/op 非 defer 仍 submit）与 poll 端新增
flush 调用。**计数与 Phase B `cnt-on-full-d2` 在 1–2% 内一致**（0.5589/0.2353/0.3236），
说明本块机制未漂移。`poll` 0.7900→0.7922/op（+0.27%；Phase B +0.63% [0.19,1.08]）；
`wait_attempts` 不在 C1 探针内（Phase B +1.39% [1.04,1.70]）。

## 3. 单次调用成本（TSC，clean；final run 的 CI 为 12 个 invocation 的 bootstrap）

| 探针 | A ns/call | B ns/call | Δ | 备注 |
|---|---|---|---|---|
| `defer_transport_flush_locked_` | 10.2 [10.2,10.3] | 16.8 [16.8,16.9] | **+6.6（+64%）** | A 的 10.2 ≈ 空 pair 9.9，实际早退≈0 |
| `ProgressSource::signal()`（含 write） | —（不调用） | **277.5** [275.3,280.0] | — | clean fraction 0.999 |
| ├ mutex+epoch 基座（signal−write） | — | **35.3** [34.9,35.8] | — | 含一个 TSC pair；实际 work ≈25.5 |
| └ `::write(eventfd)` | — | **592.5** [589.8,595.4] | — | 40.9% 的 signal 发生 write；clean 0.998 |
| flush 提交（`io_uring_submit`） | 702.3 [700.2,704.8] | 714.4 [711.7,717.7] | **+12.1（+1.7%）** | 同一 flush 从 dispatch→poll |
| `poll()` | 366.8 [363.9,369.5] | 658.0 [655.0,661.2] | +291.2 | B poll 承担 flush；非新增工作 |
| 每 op 的 signal 总成本 | 0 | **155.8** [154.7,157.0] | +155.8 | signal clean ns/op |
| 每 op 的 write 成本 | 0 | **136.0** [134.9,137.1] | +136.0 | 其中内核为主 |
| 每 op 的基座成本 | 0 | **19.9** | +19.9 | mutex+epoch |
| 每 op 的 defer 增量 | — | **+6.6** | +6.6 | 1 次/op |

复现（prev2 run，最小探针集，`raw/c1-interleaved-d2-prev2`）：signal 275.9 [272.6,280.0]、
write 562.6 [560.0,565.5]、base 39.4 [38.5,40.5]、defer A 10.2 / B 16.2 [15.7,17.0]。
两次独立运行的 per-call 成本差 ≤5%，计数差 ≤1.2%。

### 3.1 哪些状态检查/原子/锁增加（按源码与计数）

| 机制 | 次数/op（B） | 说明 |
|---|---|---|
| `defer_transport_flush_locked_` 调用 | 1.0（A 也 1.0） | A 只检查 `bounded_progress_` 后返回 |
| owner 门（`std::this_thread::get_id()` + compare） | 1.0（A 0） | 通过率 1.0000 |
| payload 门（`prep.kind`/`native_length` 两次 load） | 1.0（A 0） | 4 KiB read，全部通过 |
| witness 门（`live_cookies_.load(relaxed)` 原子 load + `ledger->size()`） | 1.0（A 0） | 通过率 0.5621 |
| cap 门（`ledger->size()`） | 0.5621（A 0） | witness 通过后才评估 |
| `std::mutex::lock/unlock`（ProgressSource） | 0.5621（A 0） | lock cmpxchg + unlock store；无 futex（未竞争） |
| progress epoch 饱和检查/自增 | 0.5621（A 0） | mutex 内非原子 |
| `notification_pending_` 读 | 0.5621（A 0） | mutex 内非原子 |
| eventfd `::write` | 0.2300（A 0） | syscall；40.9% 的 signal |
| 新增原子操作合计 | ≈2.1/op | 0.562 lock RMW + 0.562 unlock store + 1.0 relaxed load |

`poll`/`dispatch` 的既有 `dispatch_mtx_` 调用次数只随 poll +0.27% 轻微变化；没有新增内核锁。

## 4. 探针开销（同二进制 ON/OFF；独立组 bootstrap，10,000×，seed 5950）

| 探针集 | 指标 | A ON vs OFF | B ON vs OFF | 备注 |
|---|---|---|---|---|
| 最小（defer/signal/write/poll，prev2） | CPU/op | **+0.65%** [0.07,1.23] | **+1.96%** [1.04,2.78] | 效应 3.52%；校正后 3.515% |
| | 吞吐 | −0.58% [−1.17,+0.01] | −1.88% [−2.68,−0.98] | |
| | p95 / p99 | CI 含 0 | CI 含 0 | 探针对 tail 无可测影响 |
| 完整（+submit/flush，final） | CPU/op | **+3.05%** [2.13,3.93] | **+2.72%** [2.04,3.37] | 效应 4.24%；校正后 4.241% |
| | 吞吐 | −2.90% [−3.73,−2.03] | −2.60% [−3.23,−1.95] | |
| | p95 / p99 | p95 −0.49% [−1.30,0.27]；p99 CI 含 0 | p95 +1.18% [−0.29,3.55]；p99 CI 含 0 | |

结论：
- 最小探针集 ~0.7–2.0% CPU/op，约为效应的 1/5–1/2；完整集 ~2.7–3.1%，**已接近效应量级**
  （B 2.72% vs 4.24%）。按任务纪律，ON 块不作为定量效应估计；定量效应取 OFF 块
  （+4.243% [3.228,5.238]）与 Phase B split-d2（+3.70% [2.93,4.55]）。
- **OFF 块的残余编译开销未单独同块量化**：C1 二进制的 OFF 仍有 `if (g_enabled)` 分支与
  `thread_local` 访问（无 TSC）。C1 OFF 与冻结二进制同设计的绝对 CPU/op 为
  A 5485.7 vs 5154.7 ns（+6.4%）、效应 +4.243% vs +2.994%，差值处于已知块间漂移
  （Phase B 2.67–4.52%，见 `cost_table.md` §2/G5/G6）量级内，无法把绝对差归给残余代码。
  因此本表把 **ON−OFF（同二进制）作为探针开销口径**，OFF 残余标为未单独量化；
  OFF 效应与 Phase B 中央值的一致性（+4.24% vs +3.70%）是残余开销不改变结论的旁证。
- ON/OFF 每臂校正后效应与 OFF 完全一致（4.241% vs 4.243%；3.515% vs 3.518%），说明
  探针偏置可按臂减去；但该校正本身依赖 ON/OFF 块无漂移，仍只作交叉验证。
- 探针对 p95/p99 无可测影响（CI 含 0）；tail 统计取自 latency pass（计数器关闭）。

## 5. 与 CPU/op 增量的对账

| 量 | ns/op | 来源 |
|---|---|---|
| Phase B split-d2 CPU/op 增量（无仪器，12 对） | **+189.15** [+150.3,+233.5]（由 +3.70% [2.93,4.55] 换算） | `cost_table.md` |
| C1 直接新增：signal clean | +155.8 [154.7,157.0] | 本文件 §3 |
| C1 直接新增：defer 增量 | +6.6 | 本文件 §3 |
| 小计（原始） | **+162.4** | |
| 扣除 signal+write TSC pair 基线（7.8） | **≈154.6** | pair 9.85 ns |
| 占 Phase B 中央效应 | **82%**（范围 66%–103% 对应效应 CI） | |
| 同块 OFF 效应（final） | +231.7（+4.243%） | 本块 |
| 同块解释比例 | **67%**（原始 70%） | |
| 同块 OFF 效应（prev2） | +182.2（+3.518%） | 上一块 |
| 同块解释比例 | **84%**（原始 89%） | |
| 同块 OFF 效应（prev，无 clean） | +158.3（+3.104%） | 首块 |
| 同块解释比例 | **94%**（原始 signal 149.8 + defer 7.6 ≈ 157.4） | |

结论：**新增 signal+defer 工作量是 CPU/op 增量的主项，直接区间测量约 152–155 ns/op（pair 校正后），
相当于 Phase B 中央效应的 ~80–82%（上限口径；本轮三块的 67–94%）。** 由于探针开销与效应
同量级，该比例不作为精确分解，精确份额标 **CORRELATION_PARTIAL**；残差 25–35 ns/op（中央）
不能由本分支的探针解释；flush 每调用 +12 ns/op 已测到，剩余候选为 C5 微架构/layout 与
C3 内核路径。

### 5.1 用户/内核切分

| 组成 | ns/op | 归属 | 与 Phase B 对照 |
|---|---|---|---|
| signal 基座（mutex+epoch，扣 pair） | ≈14–17 | 用户态 | Phase B 用户 +95.24；只解释 ~15–18% |
| defer 增量 | +6.6 | 用户态 | 同上 |
| write 用户侧 stub（trace 8.5 cycles/op ≈3.7 ns @2.3 GHz） | ≈3.7 | 用户态 | |
| 直接新增用户态合计 | **≈24–27** | 用户态 | **≈25–28% 的 +95.24** |
| write on-CPU（TSC，跨 syscall） | **132–136** | 用户+内核，内核为主 | Phase B 内核 +79.37 [37.8,124.6]；处于上界附近 |

解释：Phase B 用户态 +95.2 ns/op 中，新增用户指令执行只占约 1/4；其余与 `instructions:u`
仅 +1.06%、`cycles:u` +5.71%、IPC −4.36%、cache-references +23%、branch-misses +15% 一致，
是新增路径带来的停顿/缓存/分支成本（C5 介导），而不是“更多用户指令”。
内核侧：write 的 on-CPU 时间足以覆盖 Phase B 的内核净增量上界；但 net kernel 小于 write 成本，
说明可能存在其他内核路径的节省（或 `/proc` 10 ms tick 低估）。**C3 才能定论。**

## 6. 观测边界（本分支）

- **内核态 PMU 不可用**（OBSERVABILITY_LIMIT_REACHED）：`perf_event_paranoid=2`、无 sudo/
  CAP_PERFMON；`cycles:k`/`instructions:k` 打不开。内核时间只有 `/proc` stime 与 TSC on-CPU
  区间两种间接量。
- **TSC 区间的抢占污染**：signal/write 的 clean fraction 0.999/0.998，均值稳健；poll/flush
  的 clean fraction 0.579/0.404，raw 均值不可用，本文只用 clean 比较。
- **mutex 单次成本未拆分**：基座是 lock+unlock+epoch+分支的合并值；未单独测 `lock cmpxchg`。
- **未运行 G10 mutant**：signal-drop 反事实不在本分支执行；本文的因果强度是“直接测量 +
  源码路径一致 + 同块对账”，不是反事实闭环。
- **二进制 layout**：C1 二进制（sha `7553…`）含空宏脚手架与多组 TSC 分支，OFF 块效应
  （+3.10/+3.52/+4.24%）与冻结二进制同设计对照（+2.99% [1.63,4.33]）、Phase B
  （+3.70% [2.93,4.55]）一致但块间有漂移；定量结论按“同块效应 + Phase B 中央值 + 范围”报告。
- **块间漂移**：三块效应 158–232 ns/op；per-call 成本稳定（≤5%），解释比例随效应块变化
  67–94%，不能把单块比例当作精确常数。

## 7. Provenance

| 项 | 值 |
|---|---|
| 源码 | `/home/jnhu/sluice-595-e5/causal-r2/worktree`，commit `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| 研究补丁 + C1 探针 | `git diff HEAD` sha256 `3483ec5dfa7300a0006b721e501476720b67c26ebbf068068fc0f592543ba0ef` |
| C1 二进制 | `evidence/counters/bin/bench_w02_pipeline_c1_final`，sha256 `755346410fd11f39d8f713e2fad32e0e5b1c8cd6de52f16960eeca41033709af`（当前源码重建复现同 sha） |
| 首次 C1 二进制（无 clean 统计） | sha256 `c0ee142315d534e0a29b52183f89aa8f7c9672082ffef711db4fa4377debd834`（未保留文件） |
| 第二次 C1 二进制（加 clean 统计） | sha256 `e0b56e2ead24ec7fcce5f68be635852dbd2d9d18c2bb9ac3c98cafe0d8facbb5`（未保留文件） |
| 冻结二进制（同设计对照） | `evidence/counters/bin/bench_w02_pipeline_frozen`，sha256 `61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886` |
| 构建 | `SLUICE_595_COUNTER_BUILD=1 SLUICE_595_C1_BUILD=1 SLUICE_595_STRIP_NONE=1 xmake f -P . -c -y --liburing=y -m release && xmake build -P . -y -j 16 bench_w02_pipeline` |
| 工具链 | g++ 16.2.1、xmake 3.1.2+HEAD、perf 7.2.11-200.fc44 |
| 权限/环境 | paranoid=2、unprivileged_bpf_disabled=2、tracefs root-only、无 sudo；bench cpu2、perf/sampler cpu3；loadavg 见各 raw 目录 |
| 统计 | pair 相对差/ON-OFF 相对差；bootstrap 10,000、seed 5950、percentile 95% CI；12 pairs（frozen 对照 6 pairs） |
| 样本量 | 12 pairs × 4 invocations = 48；每 invocation warmup=1 + measured=5，每 measured rep 20k op → **100,000 measured op/invocation**；每 condition×arm 12 个 invocation 均值 |
| 实验顺序 | 偶数 pair `OFF-A, OFF-B, ON-B, ON-A`；奇数 pair `OFF-B, OFF-A, ON-A, ON-B`（见 §1.3） |

运行命令：

```
python3 -I tools/run_c1_blocks.py \
  --label c1-interleaved-d2 \
  --bin /home/jnhu/sluice-595-e5/causal-r2/worktree/build/linux/x86_64/release/bench_w02_pipeline \
  --pairs 12 --outdir raw/c1-interleaved-d2
```

runner 每 invocation 设置 `SLUICE_BENCH_595_DEPTHS=1`、`SLUICE_BENCH_CASE_FILTER=W02-uring-4k-d2`、
`SLUICE_BENCH_WARMUP=1`、`SLUICE_BENCH_MEASURED=5`、`SLUICE_BENCH_STATS=1`、
`SLUICE_BENCH_REUSE_FIXTURE=1`、`SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-cost`、
`SLUICE_595_COUNTERS=0`、`SLUICE_595_C1={0,1}`，按 arm 设置/取消 `SLUICE_595_ARM=B`，
`taskset -c 2 <bench>`；sampler 每 1 ms 读 `/proc`。

原始数据：

| 内容 | 路径 |
|---|---|
| final 12 对 OFF/ON（48 invocations） | `raw/c1-interleaved-d2/`（`results.jsonl`、`analysis.txt`、`analysis.json`、`c1_summary.json`） |
| 第二次（最小探针集，12 对） | `raw/c1-interleaved-d2-prev2/` |
| 第一次（无 clean 统计，12 对） | `raw/c1-interleaved-d2-prev/` |
| 冻结二进制同设计对照（6 对） | `raw/control-frozen-interleaved-d2/` |
| runner / analyzer | `tools/run_c1_blocks.py`、`tools/analyze_c1.py` |
