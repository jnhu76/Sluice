# EVIDENCE_GAPS — Sluice #595 causal-r2（Phase B 成本核算后；Phase C 综合更新 2026-11-04）

状态：ACTIVE。每项含：问题 / 现有证据 / 缺失观测 / 竞争假设 / 下一仪表 / 预测 / 开销风险 / 停止条件。
标签：UNOBSERVED（本会话未观测到）、OBSERVABILITY_LIMIT_REACHED（能力到头）、CORRELATION_PARTIAL
（只有相关证据，不能下直接因果）。本轮成本表：`counters/cost_table.md`；统计汇总：
`counters/analysis/summary.json`。Phase C 综合新增：因果图 `CAUSAL_DAG.md`、成本分类
`COST_CLASSIFICATION.md`、延迟分布重算 `counters/analysis/latency-percentiles.md`、
C5 延期分支节、G10 反事实规格冻结、G12（逐请求延迟分解）。
**Phase D 跨层关联更新（2026-11-04）**：新增"跨层关联本轮结果"节；G1/G2 能力状态
修订（root bpftrace 已恢复并实测，`CAPABILITY_MATRIX.md` §12）；G4/G12 按每请求级
关联结果修订；C3 节追加"inline 完成"解释修正。

前置事实（本轮已直接测量，不列为缺口）：D2 CPU/op +3.70% [2.93,4.55]（无计数器 split 块，12 对），
与冻结基线 +3.73% 一致；提交线程用户 +95.2 ns/op、内核 +79.4 ns/op、io-wq +7.9 ns/op（CI 含 0）；
`sqes_deferred` 0→0.5589/op，`wakeups` 0→0.5589/op（实际 eventfd write 0.2353/op），
`flushes`/`actual_enters` 两臂均 1.0000/op。综合阶段新增直接测量（phaseA 原始数据 pair 配对
bootstrap，24 对，seed 5950，`counters/analysis/latency-percentiles.md`）：延迟分布整体抬升——
p50 +6.91% [+5.03,+9.23]（+532 ns）＞ p95 +4.96% [+4.23,+5.74]（+634 ns），p99 +7.07%
[+4.24,+11.78]（+955 ns）；延迟位移超出 CPU 增量 +343~766 ns/op（wall 分量，缺口 G12）。

---

## C2 本轮深挖结果（2026-11-04，syscall 级直接测量）

证据：`traces/c2-syscall/README.md`、`counters/C2_PROBE_OVERHEAD.md`、原始 `traces/raw-c2/`。
方法：strace（ptrace，无需 root；2 进程/臂 × 3 measured rep × 20k op）+ liburing
LD_PRELOAD interposer（原生速度 TSC 事件环；同构 8-invocation 块）。bpftrace/perf/ftrace
tracepoint 路径本会话无 root 全部不可用（实测失败输出 `raw-c2/obs-0*.txt`）；
tracepoint 确实存在于本内核（BTF 17 个 `trace_event_raw_io_uring_*` 全在）。

- **enter 分类（两臂、双仪器一致）**：D2 稳态 100% enter 为 submit-only
  `(to_submit=1, min_complete=0, flags=0, ret=1)`；零 wait-only、零 submit-and-wait、
  零 to_submit>1（无 batching）、零部分提交、零错误。每 invocation 80,005 enter 守恒。
  **enter 总数 == 纯提交次数 == 1.0000/op，两臂相同**。
- **submit 时长无 A/B 差异**：strace B−A +67.7 ns [−154.1,+287.5]；原生速度 shim
  B−A +37.9 ns [−33.7,+110.4]（`io_uring_submit` 全调用 mean A 2,512 / B 2,550 ns，
  分布宽 p50≈1.2 µs p99≈5.3 µs，段间 p50 1.2/3.3 µs 双档=核频漂移痕迹）。
  ⇒ **B 的内核 +79.4 ns/op 不在 io_uring_enter 内**。
- **flush 位置迁移直接观测（原生速度）**：B 有 29.93% [29.7,30.4] 的 submit 之后
  <250 ns 即跟随 peek_batch（p10 74 ns）；A 为 0/120,000（p10 322 ns）——poll-pass 内
  flush 群体为 B 专属。B 的 peek 0.550–0.562/op vs A 0.537–0.544/op，单 CQE 收割占比
  B ~20% vs A ~15%。defer/signal 结构在 shim 下存活。
- **strace 掩蔽 B 通知路径**：B 的 eventfd write 塌缩为每 pass 恰 1 次（无 drain：
  0 read/0 park/0 futex，两臂 syscall 序列不可区分），但 B latency-pass p50 为 A 的
  2 倍（77.0 vs 38.6 µs）且 syscall 计数相同——与 deferral 仍发生一致（不可计数）。
  **B 的 write/coalesce 混合以 Phase B 计数器（0.2353/0.3236/op）为准**。
- **探针开销**：strace CPU/op +147~158%、吞吐 −86%、p50 ×4.3~8.5（仅路径/不变量）；
  shim CPU/op +1.6~2.3%、吞吐 −1.6~2.3%（结构/时点发现，不作幅度定量）。
  无仪器 mini-OFF（PMU_ONLY+fixture 复用，非冻结协议）两块 +4.72%/+6.21%，在漂移带
  上缘附近，仅作开销比值分母。

---

## C1 本轮深挖结果（2026-11-04，直接测量）

证据：`counters/C1_COUNTERS.md`、`traces/perf-callgraph/README.md`。
方法：同一研究副本加 TSC 聚合计数器（count/sum/clean），12 对交错 ON/OFF（48 invocations，
无其他 instrumentation）；另有 6 个无仪器用户态 perf callgraph trace（A/B 各 3）。
注：加入 C1 探针后研究副本行号有位移；当前探针位置见 `C1_COUNTERS.md` §1.2，
下文 G1/G3/G7/G10 中引用的旧行号是加探针前的位置。

- **执行次数**：`signal_ready_progress`→`ProgressSource::signal` **0→0.562/op**，
  eventfd write **0→0.230/op**，合并 **0.332/op**；`defer_transport_flush_locked_` 调用两臂均
  1.000/op，但 B 执行完整 owner/payload/witness/cap 门（owner pass 1.000、witness pass 0.562）；
  flush（实际 `io_uring_submit`）两臂均 1.000/op；submit 调用 1.790→1.230/op（少掉的是空调用）。
- **单次调用成本（in-situ TSC，clean）**：`signal` B 277.5 ns/call（其中 mutex+epoch 35.3、
  eventfd write 592.5，write 发生率 40.9%）；`defer` A 10.2 → B 16.8 ns/call（**+6.6，+64%**）；
  flush 提交 702.3 → 714.4 ns/call（**+12.1，+1.7%**，同一 flush 挪到 poll）；
  `poll` 366.8→658.0 ns/call 是 flush 迁移，不是新增 call。
- **与 CPU/op 对账**：signal clean 155.8 ns/op + defer 增量 6.6 = **162.4 ns/op 原始**，
  扣 TSC pair 基线 ≈**154.6 ns/op**；占 Phase B 中央效应 +189.15 的 **≈82%（区间上限口径）**，
  占本轮三块同块效应（158/182/232 ns/op）的 **67–94%**。完整探针开销与效应同量级
  （B +2.72% vs 4.24%），故精确份额标 CORRELATION_PARTIAL，ON 块不作定量效应。用户态新增执行仅 **≈24–27 ns/op**
  （Phase B 用户增量 +95.2 的 25–28%），其余是 cycles 涨而 instructions 仅 +1.06% 的
  停顿/微架构部分（C5 介导）。eventfd write 的 on-CPU 132–136 ns/op 与 Phase B 内核净增量
  +79.4 [37.8,124.6] 处于同一上界附近，内核分解留给 C3。
- **探针开销**：最小探针集 CPU/op A +0.65% [0.07,1.23]、B +1.96% [1.04,2.78]；完整集
  （+submit/flush）A +3.05%、B +2.72%，吞吐 −2.6%~−2.9%，p95/p99 CI 含 0。完整集已接近
  效应量级，**ON 块只用于路径/计数/单次成本，定量效应取 OFF 块与 Phase B**；ON/OFF
  校正后的效应与 OFF 一致（4.241% vs 4.243%；3.515% vs 3.518%）。
- **trace**：6 run 全部 Lost=0、17,891–18,696 samples、实测 7,990–8,003/s（请求 8k）、
  `clockid: monotonic`；B 专属 `__GI___libc_write` 8.5 cycles/op、`dispatch_after_accept`
  自采样 100.6→131.4 cycles/op；但无帧指针/DWARF，caller 链有伪帧，只作路径发现。
- **残差**：中央 ≈25–35 ns/op（同块 67–94% 解释比例对应 10–77 ns/op）未由 C1 探针解释；
  已测到 flush 每调用 +12.1 ns/op，其余候选为 C5 微架构/layout 与 C3 内核路径，**UNOBSERVED**。

---

## C3 本轮深挖结果（2026-11-04，内核/io-wq 域）

证据：`counters/C3_COUNTERS.md`（定量：sysio 内核记账块 + C3 TSC 探针块，各 12 对）、
`traces/c3-kernel/README.md`（io-wq 画像、strace、liburing 行为学探针、UNOBSERVED）。
工具 `counters/tools/run_sysio_blocks.py`、`run_c3_blocks.py`、`analyze_c3.py`；
原始 `counters/raw/sysio-d2/`、`counters/raw/c3-interleaved-d2/`。

- **B 专属新增内核 syscall 唯一：write(eventfd)**。无仪器内核记账（/proc io，frozen
  二进制）：bench 线程 `syscw` B−A +86,912 [86,541,87,263]/invocation，
  `wchar`差/`syscw`差 = **8.005 B/次**（全部 8 字节 eventfd write）；measured 密度
  探针复核 **0.24078/op** [+0.23858,+0.24289]（=Phase B 0.2353）。
- **read/poll/drain 面两臂同构且 B 略少**：drain/read/park 次数 B−A = **−0.00718/op**
  [−0.01034,−0.00375]；read 面 on-CPU **−13.03 ns/op** [−17.61,−8.45]；poll 100%
  POLLIN 唤醒、零超时（两臂）；`drain_ack=0`（稳态 acknowledge 不执行）；
  每次 drain 恒 1 成功 read + 1 EAGAIN read；counter 值 A 4.034→B 5.154/drain
  （+1.12 ≈ write/drain=1.000，闭环：fd 同时被内核 CQE 完成信号与 B 用户 write 写）。
- **单次成本（TSC clean）**：write 562.7（C1 同点位 592.5）、read_ok A 557.6/B 539.1、
  read_eagain ~486-488 ns/call；write per-op **+138.25 raw [+136.8,+139.8]**。
- **内核对账**：eventfd 面 ≈+125 ns/op [≈118,132]（write +138、read −13）vs 提交线程
  内核净增量 +79.37 [37.8,124.6]（tick 口径）——write 单项已超净增量上限；中心差
  ~46 ns/op 归 TSC/tick 口径系统差（特权级切换归属等，不可观测）。**io-wq（内核记账
  零 syscall、+0.46% n.s.）、enter（C2）、completion（peek 无 syscall 差）、futex
  （两臂 2/进程）全部 0 或负——内核增量不需要这些路径贡献。**
- **io-wq work 内容**：UNOBSERVED（root-only tracepoint）。行为学画像：每 invocation
  1 个 iou-wrk kthread、park `io_wq_worker`、零 read/write syscall（12/12 记账）、
  零 futex；唤醒率 D2 0.55/0.56、D1 0.72/0.72 per op（A/B 等量，与 depth 相关）；
  "buffered read 强制走 io-wq"假说被 liburing inline 探针排除（tmpfs/ext4 warm
  99.6–100% submit 内完成）。
  **【D 修订（2026-11-04，内核直接测量）】**：跨层关联实测 warm tmpfs 4k read **100%
  经 io-wq 排队**（qasync 6004/6004）且完成**全部由 io-wq worker 线程投递**（E1 单一
  worker tid 投递全部 6004 complete；worker PF_IO_WORKER 挂 bench 线程组、同 cpu=2）。
  上一段"inline 探针排除"的表述按新证据**修正解释**：liburing 行为学测的是"CQE 在
  peek/submit 返回时已就绪"（用户侧可见性），与"io-wq 排队 + worker 极速完成"兼容；
  数据不矛盾，但"完成上下文=提交线程 inline"的推断作废。C3 的成本结论不变
  （io-wq CPU +0.46% n.s.、write 唯一 B 专属 syscall 均为独立证据）。io-wq 执行段
  **内部**（qasync→complete 之间：worker 执行/锁/调度）仍无中间事件——UNOBSERVED，
  但两臂同构行使，不影响 A/B 归因（`traces/xlayer/README.md` §5.2(d)/§5.3/§6.2）。
- **探针开销**：C3 探针 CPU/op A +0.93% [−1.34,+2.96]、B +0.57% [−0.14,+1.35]
  （CI 含 0，效应的 16–25%）；内核态 ON/OFF 差 +0.18%/−0.16%（CI 含 0，内核时长
  读数不受探针污染）；校正效应 +3.204% vs OFF +3.178%。

---

## C4 本轮深挖结果（2026-11-04，调度域 /proc schedstat 直接测量）

证据：`counters/C4_SCHED.md`（定量）、`traces/c4-sched/README.md`（观测路径与
UNOBSERVED）、原始 `counters/raw/{schedstat-d2,schedstat-off-d2,schedstat-d2-br}/`。
方法：frozen 二进制（无探针）+ 外部 1 ms /proc 采样器（schedstat/sched/status/stat/
PSI，runner pin cpu5），三块 12+6+6 对（ON/OFF/ON-bracket，measured=20，窗口 420k op）；
合并 18 对 bootstrap（seed 5950）。sched tracepoint/perf sched/bpftrace 无 root 全部
实测失败（`traces/raw-c4/obs-01-*.txt`），与 C2/C3 会话一致。

- **runnable delay（提交线程，schedstat[1]）：+36.4 ns/op（+0.94% [+0.38,+1.50]）**——
  统计显著但物理极小，是同窗口 on-CPU 增量 +474.8 ns/op（+4.62% [4.06,5.23]）的
  7.7%。调度排队不是 D2 主体；退化在执行态（C1/C3/C5 域），不在等待态。
- **调度结构不变**：timeslices +0.12% n.s.（段数不变）、平均时间片 +4.5%（执行段
  变长而非变多）、sleep/park 推导 −0.39% n.s.、voluntary −0.40% n.s.、nonvoluntary
  +0.36% n.s.（Phase B split-d2 +1.72% 显著，同向；本块 full 模式+5 文件采样下稀释）、
  migrations 提交线程 1/invocation 两臂相同 + io-wq 0 + CPU 恒 cpu2（processor 字段
  与 se.nr_migrations 双证据）。
- **唤醒目标（源码级）**：signal 写点在 dispatch 持锁路径（uring_transport.cpp:115-120
  → progress_source.hpp:492），写者=提交线程且必在运行；唯一 poll 者也是它
  （park, progress_source.hpp:208-218；progress_handoffs=0）→ **写时 waitqueue 空，
  B 的 0.5589/op "wakeups" 是用户态通知计数，不产生 sched_wakeup**。可观测后果一致
  （timeslices/sleep/迁移不变）；逐事件证实 UNOBSERVED（root-only）。
- **io-wq**：on-CPU +0.60% n.s.、runwait +0.64% [+0.07,+1.17]、timeslices 不变、
  vol +0.73% [+0.22,+1.27]、nonvol −1.93% [−3.08,−0.85]；sleep 推导 +8.76% 中
  lifetime 仅 +3.09%（wall 伴随），登记 DERIVED 伴随量，无 CPU 后果。
- **PSI cpu total**：+2.14% [−1.54,+6.15] n.s.（全系统口径，辅助）。
- **采样器开销远超效应**（绝对值禁用于定量）：1 ms×5 文件轮询使 bench CPU/op +5~8%、
  吞吐 −6%、p99 +30%+（ON vs OFF 非配对，含 ±1.2% 块间漂移）。采样器 arm-blind，
  三块 A/B 相对差互证一致（CPU +3.11/+3.88/+3.88%，与 Phase B +3.70% 一致），相对
  结论保留；runwait +36.4 视为上界读数（不排除采样器系统性抬高绝对值）。
- **G2 判定（停止条件触发）**：runnable delay 总量远低于解释 CPU 增量所需，且
  off-CPU 时间不计为 CPU 工作 → **C4 维持"通知路径（mediating）"，调度级延迟不构成
  D2 根因**；G2 的调度器证据部分关闭，逐事件 wakeup→run 分布/抢占者身份/逐 park
  原因仍 UNOBSERVED（root-only，阻止尾部分布与抢占归因，不再阻塞 D2 主结论）。

---

## C5 延期分支（触发但本轮未深挖）

状态：**DEFERRED**（工作流分支深挖上限 4，触发序 c1/c2/c3/c4 入选，c5 延期；
判定依据 `counters/cost_table.md` §6 C5 行：指令数解释不了 cycles——instructions:u
+1.06% vs cycles:u +5.71%，IPC −4.36%、cache-references +23.31%、branch-misses +14.94%，
cache-misses/page-faults 不显著）。分支规格见工作流草稿 BRANCH_SPEC.c5
（LLC 行为、dTLB/iTLB、cacheline 争用、memory stalls、page faults、CPU 频率、SMT 争用；
PMU 只能给相关证据时不得直接下因果结论）。

- **问题域现状**：C1 直接测量把新增用户态**指令执行**钉在 ≈24–27 ns/op，而用户增量
  +95.24 ns/op——**≈68–71 ns/op 的用户 cycles 无指令增长**，是新增路径（signal 门链/
  mutex/eventfd stub/flush 挪位后 poll 路径）的停顿/取指/分支代价。事件级来源
  CORRELATION_PARTIAL（G7 承接）。
- **延期不等于关闭的结论**：(i) CPU 增量解释比例的 27–35 ns/op 残差中，C5 是首要候选；
  (ii) CF-1 `sigdrop` 反事实（G10/CAUSAL_DAG §6）附带给出 C5 的回落证据——若去掉 signal
  后 cache-ref/branch-miss/IPC 不回落，停顿源在 B 其余路径/layout，与 signal 无关。
- **下一仪表（恢复 C5 时）**：用户态 L1-dcache/LLC 事件与 PEBS load-latency/LBR
  （无 root 可试，采样型单独 run）；受控 CPU placement（SMT 兄弟核占用对照）；
  或直接以 CF-1/CF-2 反事实的 PMU 差分替代事件级归因。
- **停止条件**（沿 G7）：≥50% 增量归到具体函数/代码行；或 CF-1 显示去掉 signal 后
  cache-ref/branch-miss 回落。

---

## 跨层关联本轮结果（2026-11-04，Phase D；root 会话）

证据：`traces/xlayer/README.md`（全文）、工具与原始数据 `traces/xlayer/{e1-armA-immediate,
e2-armB-defer,e3-fsync-iowq}/`；能力记录 `CAPABILITY_MATRIX.md` §12（本会话 sudo 口令
管道可用，root 观测路径恢复——与前四轮无 root 会话的差异已显式登记）。
方法：用户态 7 点有界事件环（64 B × 1 Mi，编译宏+env 门控，三轮 dropped=0）+ root
bpftrace 13 个 io_uring/syscalls tracepoint（`@ring_owner[ctx]` 归属，含 io-wq 内核线程
上下文）；跨仪器时钟同源（6000 对 flush↔enter 差 p50=773 ns）。三轮 E1/E2/E3 全量
逐事件 join（非采样），零丢失。**本目录不是 timing run：双仪器同开系关联目的豁免，
全部时延数字仅作次序/关联验证，禁止用于性能归因。**

- **链路覆盖**：11 节点中 10 个直接测量全覆盖；join sqe_prep↔submit_req 6000/6000、
  reap res 与内核 complete res 6000/6000 全等、submit 无孤儿 complete；E3（btrfs
  dirty-fsync 64 op）64/64。第 7 节点 io-wq 执行段**内部**（qasync→complete 之间）
  无中间事件——整体 CORRELATION_PARTIAL（README §6）。
- **正确性审查全过**：cookie 每 ring 严格单调零复用、(ctx,user_data) 全局唯一、内核
  req 指针跨 submit_req→qasync→complete 零违例、slot 复用全伴随 generation 递增、
  每请求 7 用户事件恰好各一条；io_uring↔block 请求 N:M，明确不假设一一对应。
- **直接观测（修正此前解释）**：warm tmpfs 4k read **100% 经 io-wq 排队且完成全部由
  io-wq worker 线程（非提交线程）投递**——修正 C3 行为学"inline 完成"的**解释**
  （数据不矛盾：行为学测的是 CQE 用户侧可见性）；C3 成本结论不变。B 臂 defer
  1045/1035/1038（≈52% 请求）、flush 2000/phase 不变、enter to_submit 恒 1——
  deferred SQE 由下一 poll 的无条件 submit_transport 吸收，不形成批量提交（G4 的
  每请求级确认）。
- **零行使路径**：内核 io_uring_defer / task_add / local_work_run / cqe_overflow /
  req_failed 三轮均 0 次——用户臂 defer 是 transport flush defer，与内核
  io_uring_defer 不同层不同机制，禁止混用同名归因。
- **用户态结构印证**：7 类用户事件 tid 全部=bench 主线程（无用户态线程交接，
  progress_handoffs=0 的每请求级印证）；io-wq worker 同 cpumask 同 cpu=2，零迁移
  （C4 migrations=0 印证）。
- **未关联**：block 层（tmpfs 无 block 请求；E3 btrfs 块活动 N:M 未采集）；io-wq
  执行段内部；task_work_run 按 pid 过滤的潜在漏检（无反证）。E3 complete→reap
  p50 23,183 ns / max 201,477 ns 为独立场景（fsync）数据，不进入 D2 归因。
- **对缺口体系的影响**：G1/G2 的"无 root"能力边界失效（改为"未运行"）；G4 剩余
  直方图项获得现成仪器；G12 的裁决路径收敛（CF-1 反事实 + 单仪器分层直方图），
  不再需要新的跨层构建。

---

## G1. 内核态 PMU 与 syscall 时长（C3 的直接指令级证据）

- **问题**：B 的提交线程内核 CPU +79.4 ns/op 是哪些内核指令/路径？`io_uring_enter` 与
  `write(eventfd)` 各自的 cycles/instructions 与时长是多少？
- **现有证据**：`split-d2` 的 `/proc` stime 三分法（bench kernel +3.99% [1.87,6.17]，io-wq +0.46%
  [−0.44,1.36]）；`wakeup_writes` 0.2353/op、`wakeup_coalesced` 0.3236/op（`cnt-on-full-d2`）；
  `actual_enters` 两臂 1.0/op（strace 60,004/3 passes 交叉验证）。79.4/0.2353 ≈ 337 ns/write 是除法推断。
- **缺失观测**：`cycles:k`、`instructions:k`、内核态 cache/branch；单次 `write(eventfd)` /
  `io_uring_enter` 的时长；syscall 返回路径的准确归属。**OBSERVABILITY_LIMIT_REACHED（C1–C4 会话）**：
  `perf_event_paranoid=2`，无 sudo/CAP_PERFMON，`perf stat -e cycles:k` 实测 `No supported events found`。
  **【D 修订】能力状态**：跨层关联会话 root 已恢复（sudo 口令管道，`CAPABILITY_MATRIX.md` §12，
  bpftrace 15 探针零丢失实测）——本 gap 从"能力不可达"降级为"**未运行**"：`:k` PMU /
  fentry/kprobe 指令级拆分现可执行，只是尚未在 clean timing run 中做过。OBSERVABILITY_LIMIT_REACHED
  标签撤回，改为 UNOBSERVED（待运行）。
- **C1 更新（部分关闭）**：C1 用 TSC 直接测得 eventfd write 的 on-CPU 往返 **592.5 ns/call**
  （clean，12 invocation CI [589.8,595.4]；prev2 562.6 [560.0,565.5]），折算 **132–136 ns/op**
  （0.230 write/op）。这直接给出了 syscall 时长（墙钟 on-CPU，含内核），不需要 `:k` PMU；
  但它不能替代内核指令级归因。`cycles:k`/`instructions:k` 仍不可用，G1 的“内核路径分解”仍开放。
- **C3 更新（2026-11-04，内核路径分解在 syscall 面层级关闭）**：两个独立块把 B 的
  内核 syscall 面完整钉死——write(eventfd) 是**唯一** B 专属 syscall（syscw 记账
  +86.9k/inv、8.005 B/次；探针 +0.24078/op [+0.23858,+0.24289]）；read/poll/drain
  面 B−A = **−0.00718/op**（显著为负，ns 口径 −13.03 [−17.61,−8.45]）；poll 100%
  POLLIN、零超时（两臂）。enter（C2）、io-wq（内核记账零 syscall + +0.46% n.s.）、
  futex（两臂 2/进程）均 0 或负。**竞争假设判定：(a) 成立（内核增量由 eventfd write
  单路径覆盖，write +138.25 ns/op raw > 净增量上限 124.6）；(b) 排除（read/poll/
  enter/io-wq 无正贡献）；(d) tick 量化不解释方向（write 单项已超上限，残差方向是
  TSC vs tick 记账口径差）。**仍开放：`:k` 指令级拆分（特权级切换 vs eventfd 内核
  路径 vs waitqueue 操作）——维持 OBSERVABILITY_LIMIT_REACHED。
- **竞争假设**：(a) 内核增量主要来自 eventfd write；(b) 来自 `io_uring_enter` 的参数/路径变化
  （但 enter 数不变）；(c) 来自 poll/调度路径（poll passes 仅 +0.63%，不太可能）；(d) `/proc`
  10 ms tick 量化把用户/内核边界记错。
  **C2 更新（2026-11-04）**：(b) 在 syscall 级被排除——enter 参数恒 `(1,0,0)`、ret≡1、
  submit/enter 时长无 A/B 差异（strace +67.7 [−154,+288] ns、原生 shim +37.9 [−33.7,+110.4] ns，
  均 CI 含 0，见 `traces/c2-syscall/README.md`）。这使 (a)/(c) 的相对权重进一步上升；
  poll/调度路径属 C3/C4 域。
- **下一仪表**：root 下 `perf stat -e cycles:k,instructions:k,task-clock` 或 bpftrace 的
  `kprobe:`/`fentry:` 探针于 `eventfd_write`/`__x64_sys_io_uring_enter`（带时长聚合）；必要时
  `perf record -g` 内核调用图。开销：perf :k 低；bpftrace 每事件百 ns 级，需单独开关对照。
- **预测**：若 (a) 成立，B 的内核 cycles/instructions 增量集中在 `eventfd_write`，约 0.235 次/op ×
  200-400 ns ≈ 47-94 ns/op，与 +79.4 ns/op 同量级；`io_uring_enter` 单次成本两臂相同。
- **开销风险**：root 权限不在本会话；bpftrace 启动/attach 成本不可与 timing 混跑；kprobe 高频路径
  可能改变时序，只能单独 run。
- **停止条件**：获得内核态 instructions/cycles 且能解释 ≥60% 的 +79.4 ns/op；或 G10 反事实已把
  wakeup 路径钉死到足以支持决策；或权限仍不可得且上层决策不再依赖该数字。

---

## G2. 调度级唤醒 / runnable / off-CPU（C4 的调度器证据）

- **问题**：B 的 0.5589 次/op 通知唤醒是否造成 runnable delay、off-CPU 时间或调度切换？
  还是仅 eventfd 自通知、owner 从未真正 park？
- **现有证据**：`progress_handoffs=0`（most-recent driver 始终是 bench 线程）；`wait_attempts`
  +1.39% [1.04,1.70]；`poll_passes` +0.63%；提交线程 nonvoluntary ctxsw +1.72% [1.30,2.12]，
  io-wq voluntary +1.99% [1.35,2.57]，io-wq nonvoluntary −3.42% [−4.64,−2.03]；migrations 0。
  **CORRELATION_PARTIAL**：这些是计数，不是 wakeup→run 延迟。
- **缺失观测**：`sched_wakeup`/`sched_waking`/`sched_switch`/`sched_stat_*` 时间戳；runnable delay；
  park 时长；唤醒目标 CPU；抢占。**OBSERVABILITY_LIMIT_REACHED（C4 会话）**：tracefs root-only，
  无 perf sched。**【D 修订】能力状态**：root bpftrace 已在跨层会话实测可用
  （`CAPABILITY_MATRIX.md` §12；sched tracepoint 与 io_uring tracepoint 同属可 attach 域）——
  逐事件 wakeup→run 分布、抢占者身份（L2/p99 附加尾部）从"能力不可达"降级为"未运行"；
  仅当需要 p99 段级归因时再启（单独 run，不与 timing 混跑）。
- **竞争假设**：(a) 通知多数被合并（0.3236/0.5589），owner 活跃，几乎不 park → 调度延迟可忽略；
  (b) 通知频繁打断 poll/等待，造成少量额外切换（ctxsw +1.7-2.0%）；(c) ctxsw 变化是 poll 次数
  增加的结果，不是原因。
  **C4 更新（2026-11-04，调度器证据直接测量，本 gap 基本关闭）**：/proc schedstat 18 对合并——
  提交线程 runnable delay **+36.4 ns/op（+0.94% [+0.38,+1.50]）**，是 on-CPU 增量 +474.8 的
  7.7%；timeslices +0.12% n.s.、sleep 推导 −0.39% n.s.、migrations 0、CPU 恒 cpu2。
  **(a) 成立**（owner 几乎不 park：poll 100% 立即 POLLIN + sleep 不变 + 源码证明写时
  waitqueue 空——写者=唯一 park 者=同一线程不同时刻）；(b) 的"少量切换"被证实但量级
  更小（本块 nonvol +0.36% n.s.，Phase B +1.72%）且不构成 CPU 成本（off-CPU 不计 CPU
  工作）；(c) 未单独检验但已被 (a)+(b) 的量级覆盖。**停止条件触发：C4 降级为"通知路径
  （mediating）"，调度级延迟非 D2 根因**（`counters/C4_SCHED.md` §0/§2）。
- **下一仪表**：root 下 `trace-cmd record -e sched:sched_switch -e sched:sched_wakeup`（隔离实例）
  或 bpftrace 配对 `sched_waking`→`sched_switch`；按 bench TID 与 iou-wrk TID 过滤。开销：每事件
  一条缓冲记录；单独 run，不与 perf 叠加。**（仅当需要逐事件 wakeup→run 尾部分布或抢占者身份
  时再启；不再阻塞 D2 主结论。）**
- **预测**：D2 中 bench 线程绝大多数时间处于 runnable/on-CPU，wakeup→run 延迟的均值 <5 µs，
  总量 <5% 的 +189 ns/op；ctxsw 增量不足以解释 CPU 增量。**（方向验证：runwait/slice
  ≈1.7 µs <5 µs 且 A/B 不变；runwait 增量 +36.4 ns/op = +189 的 19%（wall 侧最宽松口径）、
  on-CPU 增量的 7.7%，ctxsw n.s. —— 预测成立。）**
- **开销风险**：sched 全事件在高频 poll 下记录量大；tracefs 缓冲可能丢事件；必须用隔离 instance
  并记录 drop 计数。
- **停止条件**：trace 显示 runnable delay 总量 <5% 的 CPU 增量（则 C4 降级为“通知路径”而非
  “调度延迟根因”）；或 trace 显示显著 park/唤醒延迟（则 C4 升级为独立根因候选）。
  **（已触发降级分支：runnable delay 显著但极小（+0.94%），off-CPU 不计 CPU 工作，
  调度级延迟不构成独立根因。）**

---

## G3. eventfd write 的单次成本（C3/C4 的微观成本）

- **问题**：0.2353 次/op 的实际 eventfd write 每次花多少 user/kernel cycles？是否就是内核 +79.4 ns/op
  的全部？
- **现有证据**：`wakeup_writes=0.2353/op`、`wakeup_coalesced=0.3236/op`；`ProgressSource::signal()`
  在 `include/sluice/async/detail/progress_source.hpp:252-264`，write 在 `:426-446`；内核 +79.4 ns/op。
- **缺失观测**：单次 write 的 syscall 入口/返回时长、eventfd 内部路径、是否发生 wakeup 系统调用
  的调度副作用。**UNOBSERVED**。
- **C1 更新（直接测量部分关闭）**：TSC 包住每次 `::write(eventfd)` 得到 **592.5 ns/call**
  （final，clean，n=12 CI [589.8,595.4]）/ **562.6 ns/call**（prev2，CI [560.0,565.5]）；
  clean fraction 0.998（>1.04 µs 的抢占尾部已剔除）。这是 syscall 往返的 on-CPU 时间，
  不是内核内部拆分；sleep/wakeup 副作用仍需 C4/G2。**剩余未知**：write 的内部阶段
  （fd 检查/计数/wake_up）与内核/用户切分。
- **C3 更新（2026-11-04，配套面完成）**：同一点位 C3 块复核 write **562.7 ns/call**
  （clean，与 C1 prev2 一致）；补齐配套路径单次成本：drain 成功 read 539–558、
  EAGAIN read 486–488 ns/call（两臂同构，B 无新增 read 次数）；poll 100% POLLIN
  唤醒零超时。**预测验证**：原预测 write 200-400 ns、≈71 ns/op 覆盖大部分 +79.4——
  实测单次 562.7–592.5 ns 高于预测，per-op **+138.25 [+136.8,+139.8] 全额覆盖**
  内核净增量上限。**停止条件已满足**（超出覆盖而非不足）。write 内部阶段拆分仍
  UNOBSERVED（root-only）。
- **竞争假设**：(a) 每次 write ~337 ns（79.4/0.2353）；(b) write 更便宜，内核增量还包含 mutex
  系统调用外的其他路径；(c) `/proc` tick 把部分用户态时间记入内核态。
  **C3 更新（关闭）**：(a) 的除法推断被直接测量取代——write 单次 562.7–592.5 ns/call
  （C3/C1 两块）、+138.25 ns/op raw [+136.8,+139.8]，**write 单项超过内核净增量上限**
  （79.4/0.2353≈337 的"推断值"低估了单次成本，因为分母还应扣掉 read 面的节省
  −13.03 ns/op）；(b) 排除（read/poll/enter/io-wq/futex 全部 0 或负，见 C3 节）；
  (c) 部分成立方向（tick 口径中心值比 TSC 面低 ~46 ns/op，特权级切换归属不可观测）。
- **下一仪表**：root 下 fentry/kprobe `eventfd_write` 记录 entry/exit 时间戳并聚合；或
  `perf stat -e cycles:k` 包住单次 signal 微基准。开销：kprobe 百 ns 级；微基准需单独 run。
- **预测**：write 单次 200-400 ns；0.2353/op × 300 ns ≈ 71 ns/op，覆盖大部分 +79.4 ns/op。
- **开销风险**：kprobe 在 eventfd 高频路径上可能改变时序；微基准与真实负载的缓存/锁状态不同。
- **停止条件**：测得的 write 成本 ±30% 内覆盖内核增量；或 G10 反事实已足够。

---

## G4. D2 “零 enter 节省”的逐请求时序（C2 的机制闭环）

- **问题**：B 的 0.5589/op deferred SQE 是否全部在下一次 poll 被 flush？是否存在跨请求的
  batching（多个 SQE 合并）？为什么 depth=2 下 enter 仍为 1.0/op？
- **现有证据**：`dispatch_passes=sqe_preparations=flushes=actual_enters=1.0000/op` 两臂相同；
  `submit_attempts` B −30.96%；`sqes_deferred` 0.5589/op；depth=1 精确对照 `sqes_deferred=0`。
  **C2 更新（2026-11-04，核心已关闭）**：syscall 级双仪器（strace + 原生速度 liburing shim）
  直接测得：**零跨请求 batching**（240,000 次 submit（两臂合计）全部 `to_submit=1`、`ret=1`，
  无部分提交/错误），enter 总数=纯提交数=1.0000/op；**flush 位置迁移直接可见**
  （B 29.93% submit 后 <250 ns 即 peek_batch，A 0/120,000）。竞争假设 (b)(c) 排除；
  (a) 成立的syscall 级证据齐备。**剩余开放**：逐 SQE dispatch→flush 间隔直方图
  （deferral 距下一个 poll 的精确间隔分布）——对 D2 成本模型非必需
  （flush 总数、单次成本、enter 数均已闭），仅当需要逐请求时序模型时再做源码时间戳环。
  **【D 修订（2026-11-04，每请求级确认）】**：跨层关联 E2 直接 join 证实——defer
  1045/1035/1038（≈52% 请求）后 flush 2000/phase 不变、enter to_submit 恒 1（6004/6004），
  且 1045/2000 的 FLUSH 直接前驱事件是 defer（deferred SQE 由下一 poll 的**无条件
  submit_transport 吸收**，不形成批量）；跨请求持有占比由此每请求级钉死为零
  （`traces/xlayer/README.md` §5.3）。剩余"间隔直方图"项：现成仪器（用户态 7 点环，
  U.defer/U.flush 均带 TSC）可直接产出，仅未在 clean run 重测——优先级维持"非必需"。
- **缺失观测**（剩余部分）：每个 SQE 的 dispatch→flush 间隔分布；跨 pass 持有的占比
  （由 to_submit≡1 间接推为零，未逐 SQE 证明）。
- **竞争假设**：(a) depth=2 的 bench 在每次提交后必然 poll/take，因此 held SQE 在下一个 pass 前
  必被 flush（零 batching）；(b) 偶尔跨 pass 持有，但被 cap/owner 条件截断；
  (c) 计数器把“flush 调用”与“实际内核提交”混为一谈。
  **(b)/(c) 已被 C2 双仪器排除；(a) 的 syscall 级证据已闭合。**
- **下一仪表**（仅剩直方图需求时）：研究副本内加时间戳环（每 SQE 记录 dispatch/flush/poll 的
  TSC）+ root 下与 `io_uring_submit_req`/`io_uring_complete` tracepoint 对齐。开销：时间戳/TSC
  每事件 ~20-30 cycles，需单独 run。
- **预测**：≥99% 的 deferred SQE 在 1 个 poll 内被 flush；跨请求 batching 比例 <5%；因此
  depth=2 的 enter 摊销理论上限接近 0。**（batching=0 已证实；直方图预测维持。）**
- **开销风险**：时间戳探针本身会改变 poll 时序；直方图计数器比单计数器贵；不能与 perf stat 叠加。
- **停止条件**：时间线显示 ≥95% deferred SQE 在一个 poll 内 flush（则 C2 机制闭环）；或发现
  显著跨 pass batching（则需重新评估 D2 的 cost model）。**（batching=0、poll 内 flush 直接
  观测已达停止条件的 syscall 级等价；逐 SQE 直方图未做，如需逐请求时序再启。）**

---

## G5. 成本模式 vs phaseA 的绝对水平差（方法边界）

- **问题**：为什么 `split-d2` 的 A 绝对 CPU/op 5131.5 ns 比 phaseA A 4776.7 ns 高 ~7.4%？
  这是 fixture 复用、PMU-only（跳过 oracle/latency）还是频率/缓存状态造成？
- **现有证据**：同一冻结二进制、同一 full 模式的 `frozen-full-d2` A 5175.7 ns，也比 phaseA 高 ~8.4%；
  `split-d2` 与 `pmu-d2` 使用复用 fixture + PMU-only。A/B 差值在三种模式下一致（+2.67%…+4.52%）。
- **缺失观测**：fixture 重写 vs 复用、full vs PMU-only 的 2×2 对照；每块的实测频率（turbostat 需 root）。
- **竞争假设**：(a) 块级频率/热漂移（最可能）；(b) fixture 复用改变 tmpfs 页状态/缓存；
  (c) PMU-only 跳过 oracle/latency 改变了后续 rep 的缓存预热；(d) 计数器/PMU 二进制代码布局。
- **下一仪表**：同一二进制、同一 arm 的 2×2 块（full/PMU-only × rewrite/reuse），每块记录
  `scaling_cur_freq` 与（root 下）turbostat Bzy_MHz；每块 8-12 对 A/A。
- **预测**：2×2 的绝对 CPU/op 差异可解释 ≥70% 的 7-8% 偏移；A/B 差值在不同模式下保持 3-4%。
- **开销风险**：2×2 需要额外 ~1-2 分钟；频率记录需 root；fixture 复用可能引入页缓存状态差异。
- **停止条件**：绝对偏移被解释到 ±2% 内，或确认只影响绝对值、不影响 A/B 差值（则成本表继续
  只用 A/B 差值）。

---

## G6. 块间漂移与频率（方法边界）

- **问题**：D2 CPU/op 的块间幅度为 +2.67%…+4.52%，大于块内 A/A 0.65 pp；多少来自频率/热漂移？
- **现有证据**：`frozen-full-d2` +2.67%、`split-d2` +3.70%、`pmu-d2-ab` +3.64%、
  `cnt-on` +4.03%、`cnt-off` +4.52%；块内 A/A（PMU）CPU −0.95% [−2.02,0.23]；run 前后
  `scaling_cur_freq` 均为 1.2 GHz 请求值（不是实测频率）。
- **缺失观测**：每块实测 Bzy_MHz（APERF/MPERF）、PkgTmp、PkgWatt；块级频率与 CPU/op 的相关性。
  **OBSERVABILITY_LIMIT_REACHED（本会话）**：turbostat 需 root。
- **竞争假设**：(a) 频率漂移（schedutil 1.2-3.5 GHz）；(b) 热降频；(c) 块顺序/缓存状态；
  (d) 不同二进制代码布局（frozen vs counters vs pmu）。
- **下一仪表**：root 下 turbostat `--interval 1` 与每块并行记录；或用 `perf stat -e
  msr/tsc/,msr/aperf/,msr/mperf/`（root）逐 invocation 计算实测频率。开销：turbostat 轮询近 0；
  perf msr 事件低。
- **预测**：块间 CPU/op 与 Bzy_MHz 负相关（r < −0.7）；归一化后 D2 幅度收敛到 3.0-4.0%。
- **开销风险**：turbostat/perf msr 需 root；并行监控进程可能轻微扰动（放 cpu3 以外）。
- **停止条件**：记录到每块实测频率并给出归一化后的幅度；或确认漂移不改变 C1..C6 的触发判定。

---

## G7. cache-references +23% / branch-misses +15% 的微架构来源（C5 介导）

- **问题**：用户态 cache-references +4.0/op、branch-misses +1.13/op 来自哪些指令/数据？
  是新增 signal/mutex/eventfd 代码，还是数据布局/别名？
- **现有证据**：`pmu-d2-ab` 的 cache-references +23.31% [12.73,35.18]、branch-misses +14.94%
  [12.88,16.80]、cache-misses +0.65%（n.s.）、IPC −4.36%；A/A 噪声 branch-misses +1.83%、
  cache-refs −7.66%（宽）。**CORRELATION_PARTIAL**：事件级相关，非指令级归因。
- **缺失观测**：L1D/L2/LLC 分解、PEBS load-latency 地址、branch stack（LBR）、内核态 cache/branch。
  用户态 PEBS 可在无 root 下试，但采样会丢事件；内核态需 root。
- **C1 更新（部分）**：直接测量的新增用户态执行只有 **≈24–27 ns/op**（mutex+epoch 14–17、
  defer 6.6、write 用户 stub ≈3.7），而 Phase B 用户增量 **+95.24 ns/op**、instructions:u 仅
  **+1.06%（+50/op）**。即约 3/4 的用户增量不是新增指令执行，而是新增路径的停顿/缓存/分支
  成本；`cache-references +23.31%`、`branch-misses +14.94%`、`IPC −4.36%` 指向 C5 介导。
  **仍未归因**：这些额外 cycles 的具体指令/数据来源（需 PEBS/LBR/内核态 PMU）。
- **竞争假设**：(a) 新增 `signal()`/mutex/eventfd 代码的取指与数据访问；(b) 同一条代码因 poll
  时序变化导致更多 cache line 触碰；(c) SMT 兄弟核干扰（cpu2 的兄弟 cpu12 空闲，可能性低）；
  (d) 计数器复用/缩放伪影（本方案 100% enabled，基本排除）。
- **下一仪表**：`perf stat` 加 L1-dcache/LLC 事件（需确认本机可用且不超 5 硬件事件）；
  `perf mem record`（PEBS，用户态）短窗；root 下 `perf record -g` 内核调用图。开销：采样型，
  必须单独 run；PEBS 短窗有 Lost%。
- **预测**：cache-ref/branch-miss 增量集中在 `ProgressSource::signal()`、`std::mutex` 与 eventfd
  write 的内联代码；数据侧 cache-misses 不增（与观测一致）。
- **开销风险**：PEBS/采样改变时序；事件数增加会复用；不能与 timing 混跑。
- **停止条件**：能把 ≥50% 的增量归到具体函数/代码行；或 G10 反事实显示去掉 signal 后
  cache-ref/branch-miss 回落。

---

## G8. 探针开销的非对称性（方法边界）

- **问题**：计数器探针总开销 ~1-2% CPU/op；其中 B 专属的额外 bump（`sqes_deferred`、`wakeups`、
  `wakeup_writes/coalesced`）对 A/B 差的偏置是多少？
- **现有证据**：ON vs OFF full D2 CPU +0.93% [−0.58,2.40]（n.s.）、PMU-only +1.97% [0.44,3.58]；
  B 专属 bump 约 2.2 次/op；OFF vs frozen CPU +0.79% [−0.66,2.35]（n.s.）。计数器块 A/B 差
  +4.03% vs 无计数器 split +3.70%，在块间漂移范围内。
- **缺失观测**：逐计数器的单次 bump 成本；B 专属 bump 的精确偏置。**UNOBSERVED**。
- **竞争假设**：(a) B 专属偏置 ≤0.2% CPU/op（~4-11 ns/op），可忽略；(b) 原子 RMW 在共享
  cacheline 上更贵（但本负载单线程执行计数路径，竞争低）；(c) 代码布局/分支预测影响大于 bump 本身。
- **下一仪表**：独立微基准（busy loop 执行 N 次 `bump`，ON/OFF 同二进制，测 ns/bump）；或在
  计数器头里加 per-counter cycle 累加（会进一步增加开销，仅在微基准使用）。开销：微基准单独 run。
- **预测**：单次 enabled bump ≤5 ns；B 专属偏置 ≤11 ns/op（<0.25% CPU/op）。
- **开销风险**：微基准与真实热路径的 cache/branch 状态不同；cycle 累加本身改变被测对象。
- **停止条件**：测得单次 bump 成本并确认 B 专属偏置 <0.5% CPU/op；或定量结论已完全基于
  无计数器块（当前即是）。

---

## G9. D1 块间符号不一致（对照的统计功效）

- **问题**：D1 的 CPU/op 在 PMU 块 −1.92%、counter 块 −0.80%、split 块 +1.37%——符号不一致，
  是噪声、块顺序还是频率漂移？
- **现有证据**：精确过滤 `=W02-uring-4k-d1` 的 `*-d1x` 块；`sqes_deferred=0`、`wakeups=0`、
  `actual_enters=flushes=1.0/op` 两臂一致；pmu 8 对、counter 6 对、split 8 对；D1 A/A 噪声
  未单独测（PMU A/A 是 D2）。块间 CPU 幅度 ±2%，p95/p99 在 ±1.5% 内。
- **缺失观测**：D1 自己的 A/A 校准；D1 每块实测频率；更多对以提高功效。
- **竞争假设**：(a) 块间频率漂移（最可能）；(b) D1 的 B 有微小真实收益/成本（enter 不变，
  不应有）；(c) 采样/进程启动噪声（D1 的 CPU/op 更小，相对噪声更大）。
- **下一仪表**：D1 A/A 8 对 + 两块交替（ABBA/BAAB）24 对 + 频率记录；或直接以“机制关闭”
  （deferred=0、enter/flush 不变）作为 D1 对照的主证据，CPU 只作安全边界。
- **预测**：D1 真实效应 |ΔCPU| <1%，块间符号由漂移解释；A/A 半宽 ≤3 pp。
- **开销风险**：D1 每 invocation ~2.3 s；24 对 ~2 min；频率记录需 root。
- **停止条件**：D1 的 A/A 半宽 ≤3 pp 且两块符号一致；或确认 D1 只作机制关闭对照，不再用其
  CPU 点估计做定量声明。

---

## G10. residue signal 的反事实（C1/C3/C4 的因果闭环）

- **问题**：D2 的 +3.70% CPU/op 是否由 `defer_transport_flush_locked_()` 为 true 后
  `dispatch_after_accept()` 调用 `signal_ready_progress()`（residue 通知）造成？去掉该通知后
  退化是否消失？
- **现有证据**：`wakeups` 0→0.5589/op、`wakeup_writes` 0→0.2353/op 与 B 的 +95.2 ns/op 用户、
  +79.4 ns/op 内核同时出现；`progress_handoffs=0`（不是 driver 切换）；`actual_enters` 不变。
  **目前是强相关 + 源码路径一致性，尚无直接反事实**。
  **C1 更新**：TSC 计数器直接测得 signal 路径 **155.8 ns/op**（clean；其中 write 136.0、
  mutex+epoch 19.9）+ defer 增量 6.6 ns/op = **162.4 ns/op 原始 / ≈154.6 扣 pair**，占
  Phase B 效应 +189.15 的 **≈82%**。这使“residue signal 是主成本”从强相关升级为
  **直接测量 + 同块对账**；但 G10 的 signal-drop mutant 反事实**仍未运行**，严格因果闭环
  与“defer 分支本身/微架构/layout”竞争解释的排除仍留给 G10/相应分支。
- **缺失观测**：B 去掉 residue signal 后的 CPU/op、吞吐、p95/p99；`wakeups=0` 时内核/用户 split。
- **竞争假设**：(a) residue signal 是主因；(b) 主因是 defer 分支/状态检查本身（即使不发信号）；
  (c) 主因是 poll 时序改变导致的 cache/调度效应，与 signal 无关。
- **下一仪表**：研究副本已有 mutant 宏 `SLUICE_595_MUTANT_DEFER_RESIDUE_SIGNAL_DROPPED`
  （`src/async/uring_transport.cpp:102-108`）。构建 mutant PMU 二进制，跑 D2 12 对 A/B
  （B 仍 defer，但不 signal）。A 不变，B' 的 `wakeups` 应为 0。开销：一次构建 + ~1 min 计时；
  单独 run，不与 perf stat 叠加。
- **预测**：若 (a) 成立，B' 的 CPU/op 增量从 +3.70% 降到 ≤+1.0%，内核 +79 ns/op 基本消失，
  `wakeups=0`；若仍 >2.5%，则 (b)/(c) 需要重新评估。
- **开销风险**：mutant 二进制代码布局变化；只改一个布尔/删除一次调用，热路径其余不变；
  需要与 A/B 同轮平衡。
- **停止条件**：反事实显示 signal 去掉后 ≥60% 的 CPU 增量消失（则 C1/C3/C4 的因果链闭合）；
  或增量仍 >2.5%（则 C2 的 defer 分支本身成为下一候选）。
- **反事实规格冻结（2026-11-04 综合阶段，全文见 `CAUSAL_DAG.md` §6 / `COST_CLASSIFICATION.md` §5）**：
  - **CF-1 `sigdrop`**（即本 gap 的 mutant）：仅移除 `advertise_residue` 中的
    `unsubmitted_after_dispatch` 项；机制预测 `wakeups`/`wakeup_writes`→0、syscw 差→0、
    内核 stime 增量→≈0、`sqes_deferred` 仍 0.5589；性能预测 CPU/op +3.70%→≤+1.0%。
    **新增双用途预测（延迟）**：p50/p99 随 CPU 回落至 ≈+2% ⇒ 延迟是 CPU 均值平移（L0）；
    p50/p99 仍 +5~7% ⇒ 延迟与 signal 无关，deferral 完成时序（L1/G12）升级为延迟主因。
  - **CF-2 `deferoff`**（G10 竞争假设 (b)(c) 的裁决器）：`defer_transport_flush_locked_`
    入口 `return false`；机制预测 `sqes_deferred`/`wakeups`→0、`submit_attempts`→≈1.79/op、
    poll-pass flush 群体→0；性能预测 CPU 与 p50/p95/p99 Δ 全部→0。若 CF-1 残量 >2.5%
    而 CF-2 归零，defer 分支本身/微架构 layout 承担残差。
  - 运行规格：mutant 二进制（记录 sha256 与 `git diff HEAD` sha256）；D2 12 对 ABBA，
    frozen full 协议（保留 latency pass 以读 p50/p95/p99）；单独 run，不与 perf stat 叠加。
  - **【D】关联级扩展（Phase D 后，不改冻结假设/预测）**：机制纯度验证可复用已验证的
    用户态 7 点环（`traces/xlayer/tool_research_595_trace.hpp`）做单仪器 companion run——
    sigdrop 后 U.defer 仍 ≈52%、U.flush 前驱=defer 比例不变、用户态 write 事件消失；
    deferoff 后 U.defer→0、U.immediate=100%、K.submit_req 位置回到 submit 现场。
    root 可用时以 syscalls:sys_exit_write tracepoint 计 write→0（attach 路径已验证）。
    性能读数仍取无仪器 frozen 协议臂——仪器 run 与 timing run 不混块。
    另：L1 的结构前提（defer→下一 poll 无条件 flush 吸收、to_submit≡1、完成由
    io-wq worker 投递两臂同构）已是每请求级直接观测（`traces/xlayer/README.md` §5.3），
    反事实只需裁决后果（CPU 份额与延迟 L0/L1 分叉），不需再证结构。

---

## G11. C6 设备路径（范围边界，不是待办）

- **问题**：D2 退化是否与存储设备路径有关？
- **现有证据**：bench meta 明确 `filesystem=tmpfs`、`cache_policy=page_cache_resident`；
  page-faults +0.15%（n.s.）；无 block 层事件；主实验是 warm tmpfs。
- **缺失观测**：无（按任务 §6，C6 仅在设备路径参与时启用；本轮不适用）。**不适用/已排除**。
- **竞争假设**：若未来在真实文件系统上复现 D2，设备/文件系统/块队列可能改变成本结构。
- **下一仪表**：仅在需要真实 FS 验证时单独立场景：验证 block tracepoint 覆盖目标设备与窗口，
  区分 warm/cold buffered、O_DIRECT、FS/块队列/设备完成；块层 issue-to-complete 不得声称为
  SSD 内部 service time。
  **【D 修订】**：跨层关联 E3 已在 btrfs dirty-fsync 64 op 上走通请求链关联（join 64/64，
  complete→reap p50 23,183 ns / max 201,477 ns，双仪器 run 仅作次序验证）；block 层
  tracepoint（block_rq_issue/complete）本轮**未采集**，io_uring↔block 请求 N:M
  （合并/拆分）已显式声明不假设一一对应——真实 FS 场景的设备段逐请求分解仍属
  未做，且必须先验证 block 事件覆盖（root bpftrace 能力已实测可用，
  `CAPABILITY_MATRIX.md` §12）。
- **预测**：tmpfs 场景无设备成分；真实 FS 场景的 D2 幅度/尾延迟可能不同，不能直接外推。
- **开销风险**：block tracepoint 高频；真实 FS 的 cold/warm 状态需严格控制。
- **停止条件**：范围明确排除 C6（当前）；或单独立项并具备设备路径证据。

---

## G12. 延迟分布整体抬升的逐请求分解（wall 超额分量；综合阶段新增，2026-11-04）

- **问题**：D2 的延迟位移（p50 +532 / p95 +634 / p99 +955 ns，pair 配对 bootstrap
  `counters/analysis/latency-percentiles.md`）超出 CPU/op 增量 +189 ns 达 **+343~766 ns/op**。
  该超额 wall/等待分量由什么构成？是 deferred op 等下一次 poll 的 flush（L1），还是
  CPU 均值平移 + 其他等待（L0），还是罕见调度/收割形态长尾（L2，p99 比 p95 多 ~+320 ns）？
- **现有证据**：分布形态直接测量——p50 相对位移 +6.91% [+5.03,+9.23] **大于** p95
  +4.96%，与 p99 +7.07% 同量级 ⇒ 分布整体抬升，排除纯尾部机制；C2 直接观测 flush
  迁移结构（B 29.93% submit 后 <250 ns 即 peek，A 0/120,000）与 tmpfs warm 完成
  99.6–100% 在 submit 返回时就绪（A 的 op 闭环于 dispatch，B 的 deferred op 必等下一
  poll）——L1 的结构前提成立；C4 runnable delay +36.4 ns/op（比超额小一个量级）；
  C2 strace 下 B latency-pass p50 为 A 2 倍但 ptrace 扭曲 4–9×（只作方向旁证）。
  **L0 与 L1 在现有聚合粒度下不可裁决**——CORRELATION_PARTIAL。
- **缺失观测**：逐请求时间线（dispatch→deferred 标志→flush/enter→CQE 就绪→take 的
  TSC + op id）× 延迟分位数分层直方图；latency pass 窗口内的 park/等待分解；
  p99 附加尾部（~+320 ns）的逐事件来源（root-only sched_switch/wakeup 或长尾段分解）。
  **【D 修订】仪器已存在并验证**：跨层关联已建成用户态 7 点事件环（accept/sqe_prep/
  defer/immediate/flush/reap/published/take，每事件 TSC+cookie+(slot,gen)+tid+cpu；
  三轮 dropped=0、每请求恰好各一条、与内核 tracepoint join 6000/6000）——本 gap 的
  "缺失观测"从"仪器不存在"收敛为"**clean（单仪器）run 未做**"：关联 run 双仪器同开，
  其时延数字（如 E1/E2 complete→reap p50 5367 vs 5619 ns）只作次序验证，禁止归因引用。
- **竞争假设**：(a) L1 deferral 完成时序主导超额（预测：deferred 分层直方图中
  deferred op 的 dispatch→reap 段系统性更长，超额集中在该段）；(b) L0 CPU 均值平移
  主导（预测：各段等比例抬升，无 deferred 特异段；但 CPU 只有 +189 ns，需其余段
  凑出 +343~766——除非延迟 pass 每测 op 摊多 op CPU）；(c) L2 罕见调度/收割长尾
  仅贡献 p99 附加部（预测：p50/p95 超额由 (a)/(b) 解释后 p99 残余集中 rare 事件）。
- **下一仪表**：研究副本用户态逐请求时间戳环（每 op 5 个 TSC 点 + deferred 标志，
  ~20–30 cycles/事件，有界环形缓冲，单独 run 不与 perf 叠加）；输出分层直方图与
  分段耗时（dispatch→flush、flush→reap、reap→take）。内核侧 CQE 可见性/io-wq 事件
  root-only 为可选增强，缺失时标 CORRELATION_PARTIAL，不做完整逐请求内核分解声明。
  CF-1 `sigdrop` 的 p50/p99 读数可先行部分裁决 L0 vs L1（见 G10 冻结规格）。
  **【D 修订】执行路径**：直接复用已验证的 7 点环（`traces/xlayer/tool_research_595_trace.hpp`，
  编译宏+env 门控）跑单仪器 clean run（ring ON，无 bpftrace/perf），先 A/A 量化环开销，
  再 A/B 各 12 对；内核 tracepoint（root 已恢复）仅在需要 worker 投递时刻精化时叠加为
  独立 run。**不再需要新的跨层构建**（needCorrelation 翻转为 false 的依据）。
  【D】内核侧机制修正供直方图解读：完成 100% 由 io-wq worker 投递（两臂同构），
  L1 机制 = deferral 推迟内核工作起点（submit_req→qasync）一个 poll 周期，
  不是"A 在 dispatch 内闭环"（原表述作废，见 C3 节修正注）。
- **预测**：若 (a) 成立，deferred op 的 dispatch→reap 中位比非 deferred op 长
  ≥一个 poll-flush 周期量级（数百 ns），且超额的 ≥60% 落在该段；p50 与 p95 的超额
  应同源，p99 附加 ~+320 ns 可能有独立 rare 分量。
- **开销风险**：时间戳环改变 poll/dispatch 时序（探针效应），须 ON/OFF 对照估计偏置，
  不与 timing 结论混跑同块；直方图桶数有界，避免内存膨胀。
- **停止条件**：分层直方图把 ≥60% 的 p50/p95 超额归到具体段（L1 成立或排除）；
  或 CF-1 已裁决 L0/L0-排除且上层决策不再需要段级分解。

---

## 已关闭的缺口（本轮解决，保留记录）

- **D1 首轮子串过滤污染**：`W02-uring-4k-d1` 同时匹配 `W02-uring-4k-d16`，首轮 D1 计数器/PMU
  混入 d16；已用精确过滤 `=W02-uring-4k-d1` 重跑 `*-d1x`，并保留作废目录与
  `counters/raw/DISCARDED.md`。D1 结论只取 `*-d1x`。
- **enter 计数口径**：R1 的 3.0002 enters/op 是 3 个 pass 的总 enter / 20k measured op；
  本轮 `actual_enters=1.0/op` 是每操作 submit enter。strace 60,004/60,000 交叉验证，口径已登记。
- **PMU 复用比例**：本轮 5 个硬件事件 100% enabled；未使用 8 事件高复用方案。
- **计数器源码位置**：13 个请求计数器 + 2 个支持计数器均有真实源码位置（`cost_table.md` §3.2），
  无含糊命名。
- **C1 函数执行次数/单次成本不可得**（本轮关闭）：无帧指针/DWARF 的 release 二进制无法用采样
  区分短函数（`defer_transport_flush_locked_` 0 样本）；已用源码 TSC 聚合计数器直接得到
  `signal` 0.562/op、write 0.230/op、coalesced 0.332/op、defer 1.0/op 及单次成本
  （`counters/C1_COUNTERS.md`）。
- **C1 用户态调用栈无 root 不可得**（本轮关闭）：`perf record -g` 在 paranoid=2 下被拒绝，
  但 `-g --user-callchains`（`exclude_callchain_kernel=1`）可用；6 run 全部 Lost=0，
  `clockid: monotonic`。仅用于路径发现，不用于定量。
- **uprobe/USDT 不可用**（本轮登记为不需要）：tracefs root-only + `unprivileged_bpf_disabled=2`，
  bpftrace/uprobe/USDT 无 root 不可 attach；Sluice 源码无 USDT note。函数级归因已由符号化
  perf 采样 + 源码聚合计数器完成，未使用 uprobe。
