# CAUSAL_DAG — Sluice #595 D2（causal-r2）因果图与综合判定

状态：ACTIVE EVIDENCE。撰写：综合分析师（2026-11-04，Phase C 机制综合；
**Phase D 跨层关联后修订**，修订记号【D】）。
输入：`counters/cost_table.md`（Phase B 成本表）、分支深挖发现 C1/C2/C3/C4（任务给定 +
`counters/C1_COUNTERS.md`、`traces/c2-syscall/README.md`、`counters/C3_COUNTERS.md`、
`counters/C4_SCHED.md`）、本轮延迟分布重算
`counters/analysis/latency-percentiles.md`（pair 配对 bootstrap，24 对，seed 5950）、
**跨层事件关联**（Phase D）`traces/xlayer/README.md`（E1/E2/E3 三轮，11 节点链、
用户态 7 点事件环 + root bpftrace 13 tracepoint、全量 join 6000/6000、零丢失；
root 能力恢复记录 `CAPABILITY_MATRIX.md` §12）。
研究副本源码实证：`worktree/src/async/uring_transport.cpp`、
`worktree/include/sluice/async/detail/progress_source.hpp`（本轮 sed 读取；行号含研究探针位移，
与冻结基线行号的对应见 `counters/cost_table.md` §3.2 与 `EVIDENCE_GAPS.md` C1 节注）。

---

## 1. 总判定

| 问题 | 判定 | 依据摘要 |
|---|---|---|
| **CPU 退化（+3.70% [2.93,4.55]，+189.15 ns/op）** | **主体已解释（直接测量），精确份额 CORRELATION_PARTIAL** | defer+residue-signal 新增路径直接测量 155–162 ns/op（原始）/ ≈154.6（扣 TSC pair），覆盖三块无仪器效应（158/182/232 ns/op）的 **67–94%**（中心约 80%，上限口径）；分层：write syscall on-CPU +132–138（C3 直接）、mutex/epoch+defer 门 ≈26（C1 直接）、微架构停顿 ≈68–71（C5 推断，延期）、残差 27–35 UNOBSERVED。严格因果闭环待 G10 反事实。 |
| **tail/延迟退化（p95 +4.96 [4.23,5.74]、p99 +7.07 [4.24,11.78]，并含 p50 +6.91 [5.03,9.23]）** | **仍未解释（NOT EXPLAINED）；【D】L1 结构前提升级为每请求级直接确认，裁决路径已收敛到反事实** | 直接测量显示**分布整体抬升**（p50 相对位移 ≥ p95），延迟位移 +532/+634/+955 ns 超出 CPU 增量 +189 ns 达 +340~770 ns/op——该超额是 wall/等待分量，现有任一单层聚合指标（CPU 分解、计数器账本、syscall 面、schedstat 总量）都不能分解它。【D】跨层关联已建成并验证（10/11 节点、join 6000/6000、零丢失）：defer 52% 请求、flush 次数不变、enter to_submit 恒 1、deferred SQE 由下一 poll 的无条件 flush 吸收——L1 的结构前提升级为每请求级直接观测；但其**延迟后果**仍未在 clean（单仪器）run 中量化，L0 vs L1 裁决由 CF-1 `sigdrop` 承担（§6）。关联 run 为双仪器，全部时延数字只作次序/关联验证（`traces/xlayer/README.md` 头部声明），不构成归因。 |

判定纪律：CPU 侧的解释比例以"直接测量 + 同块对账"为上限口径，未运行反事实前
精确份额只标 CORRELATION_PARTIAL（探针 ON 开销与效应同量级：B +2.72% vs +4.24%）。
延迟侧不以"吞吐 −3.55% ≈ CPU +3.70%"推称延迟已解释——延迟是独立 pass 的独立测量，
其位移是 CPU 位移的 1.8–2.7 倍（相对口径）。

---

## 2. 因果图（DAG）

```
                    [O1] B·BOUNDED_PROGRESS 的 defer 决策 + residue 通知
                         defer_transport_flush_locked_ 五门全过（owner/payload/witness/cap/bounded）
                         → 0.5589/op SQE 被持有（sqes_deferred 0→0.5589，确定性计数）
                         → advertise_residue(unsubmitted_after_dispatch) → signal_ready_progress()
                         （源码：worktree src/async/uring_transport.cpp，dispatch_after_accept 尾部；
                          mutant 宏 SLUICE_595_MUTANT_DEFER_RESIDUE_SIGNAL_DROPPED 实证存在）
                         [originating；证据等级：确定性计数器 + 源码路径，唯一 B 特有决策]
              ┌──────────────────────────────┬───────────────────────────────┐
              ↓                              ↓                               ↓
      [C1-m] ProgressSource::signal        [C3-m] write(eventfd)           [O2] flush 时点迁移
      0.562/op；mutex+epoch 35.3 ns/call   0.230–0.241/op × 562.7–592.5    dispatch → 下一次 poll
      + defer 门增量 6.6 ns/op；           ns/call = on-CPU +132–138        （C2 直接观测：B 29.93%
      新增用户态指令执行仅 ≈24–27 ns/op    ns/op；超内核净增量上限          submit 后 <250 ns 即
                                          [+37.8,+124.6]，~46 ns 为         peek；A 0/120,000）
      [mediating，用户态执行层]             TSC/tick 口径差                 [originating（时序）/
                                          [mediating，内核 syscall 层]      结构直接观测、成本中性]
              │                              │                               │
              ↓                              ↓                               ↓
      [C5-m] 微架构停顿（延期分支）    （并入提交线程内核 stime             [L1] 逐 op 完成可见
      cycles:u +172.56/op vs             +79.37 ns/op [1.87,6.17]）          时序改变【D 修正】：
      instructions:u +50.04/op；                                                  deferred op 的内核工作
      IPC −4.36%、cache-ref +23.31%、                                              起点（submit_req→qasync）
      branch-miss +14.94%；≈68–71 ns/op                                           晚一个 poll 周期；完成由
      用户增量非新增指令执行                                                        io-wq worker 投递（两臂
      [mediating，微架构层]                                                        同构，X1）
                                                                                    [结构前提每请求级直接确认【D】；
                                                                                     延迟后果 clean-run 未量化]
                                                                                    │
              │                                                                    ↓
              └──────────────→ [S-CPU] 提交线程 CPU/op +189.15 ns（+3.70%）← [C4-m] 通知/调度路径
                                ├─ 用户态 +95.24（+7.39%）                        （runnable delay +36.4 ns/op，
                                ├─ 内核态 +79.37（+3.99%）                        不产生 sched_wakeup：写时
                                └─ io-wq +7.94（n.s.，排除）                       waitqueue 空；off-CPU 不计
                                          │                                        CPU。[mediating，已降级]）
                                          ↓
                                [S-THR] 吞吐 −3.55% [−4.34,−2.84]
                                （observed symptom；量级 ≈ CPU 位移，无独立机制需求）

      [S-CPU] ──（部分：均匀 CPU 抬升 ~+189 ns 理论上应整体平移延迟分布）──→ [S-LAT]
                                                                          ↓
      [L1] ─────（候选主路径；结构已每请求级证实【D】，后果未量化）────→ [S-LAT]
      [L2] 罕见调度/收割形态交互（nonvoluntary ctxsw +1.72%；单 CQE 收割
            B ~20% vs A ~15%；wakeup→run 尾部 clean-run 未测【D：root 能力
            已恢复并实测，CAPABILITY_MATRIX §12】）──────────────→ [S-LAT p99 分量]

      [S-LAT] 延迟分布整体抬升：p50 +6.91% [+5.03,+9.23]（+532 ns）、p95 +4.96%（+634 ns）、
              p99 +7.07% [+4.24,+11.78]（+955 ns）（observed symptom；
              本轮重算 evidence/counters/analysis/latency-percentiles.md）
```

## 3. 节点证据与可证伪条件

### O1 defer 决策 + residue 通知（originating，最高置信）

- 证据：`sqes_deferred` 0→0.5589/op、`wakeups` 0→0.5589/op（确定性，CI 为绝对区间
  [0.5572,0.5605]）与 CPU/内核增量同块共现（cost_table §3.2 #20/#28）；C1 门通过率
  （owner pass 1.000/op、witness pass 0.562/op）证明 B 执行完整门链；D1 对照
  `sqes_deferred=0` 且 CPU 中性（§3.4）——机制随形状开关，非环境伪影。
- 源码：`defer_transport_flush_locked_`（bounded→owner→payload(read,4096)→witness→cap 五门，
  本轮 sed 实证）；`advertise_residue` 含 `unsubmitted_after_dispatch` 分支 → `signal_ready_progress()`。
- 可证伪条件：G10 `sigdrop` 反事实（去掉 signal，保留 deferral）——若 CPU 增量不降
  （>2.5% 残留），O1 的 signal 分支降级、defer 分支本身/微架构升级（见 §7 CF-1）。

### C1-m 用户态 signal 执行（mediating，直接测量）

- 证据：signal 277.5 ns/call × 0.562/op = 155.8 ns/op（clean，12 invocation CI
  [154.7,157.0]）；其中 mutex+epoch 35.3、write 用户侧 592.5（发生率 40.9%）；
  defer 门 10.2→16.8 ns/call（+6.6）；合计 162.4 raw / ≈154.6 扣 TSC pair
  （`counters/C1_COUNTERS.md`）。新增**用户态指令执行**仅 ≈24–27 ns/op——其余用户
  cycles 是停顿（→C5-m）。
- 可证伪条件：perf `__GI___libc_write` 0→8.5 cycles/op 与 `dispatch_after_accept`
  自采样 +30.8 cycles/op（6 run，Lost=0）应随 sigdrop 消失。

### C3-m eventfd write syscall（mediating，直接测量，覆盖内核增量）

- 证据：write 是唯一 B 专属新增 syscall（syscw +86,912 [86,541,87,263]/inv、
  8.005 B/次全为 8 字节 eventfd write；探针密度 0.24078/op [+0.23858,+0.24289]）；
  单次 562.7–592.5 ns/call；per-op +138.25 raw [+136.8,+139.8] **超过**提交线程内核净增量
  上限 +124.6；read/poll/drain 面 B−A 为 −0.00718/op、−13.03 ns/op（负贡献）；
  enter 无 A/B 差（C2 双仪器）；io-wq 12/12 零 syscall、futex 2/进程常量。
- 口径差声明：TSC on-CPU vs /proc tick 的 ~46 ns/op 中心差不可分解（特权级切换归属
  root-only），G1/G3 登记 OBSERVABILITY_LIMIT_REACHED。【D】root bpftrace 能力已在关联
  会话恢复（CAPABILITY_MATRIX §12），:k PMU/fentry 的指令级拆分从"能力不可达"降为
  "未运行"——G1 维持开放但不再是 OBSERVABILITY_LIMIT_REACHED。
- 【D】解释修正（`traces/xlayer/README.md` §5.3）：C3 行为学探针的"99.6–100% submit 内
  完成"是用户态可见性判定；内核侧真相是 warm tmpfs 4k read 100% 经 io-wq 排队、完成
  全部由 io-wq worker 投递（两臂同构）。**C3 的成本结论不受影响**（io-wq CPU +0.46%
  n.s.、write 唯一 B 专属 syscall 均为独立证据）；受影响的只是"完成上下文"的叙述。
- 可证伪条件：sigdrop 后 bench 线程 syscw B−A 差应归零、内核 stime 增量应 ≈0。

### C5-m 微架构停顿（mediating，CORRELATION_PARTIAL，**本轮延期分支**）

- 证据：cycles:u +5.71% vs instructions:u +1.06%（instructions 只解释 18.6% 的 cycles
  增量）；IPC −4.36 [−5.42,−3.28]、cache-references +23.31%、branch-misses +14.94%、
  cache-misses/page-faults 不显著（cost_table §3.1 #5–11）。用户增量 +95.24 中
  ≈68–71 ns/op 不是新增指令执行（C1 新增执行仅 24–27）。
- 延期说明：分支深挖上限 4，C5 触发但延期（工作流 `Sluice-595-因果归因.dwf.ts`
  `capped/deferred` 逻辑）；问题域由 G7 + 本次新增 G12/C5 节承接（EVIDENCE_GAPS.md）。
- 可证伪条件：sigdrop 后 cache-ref/branch-miss/IPC 应回落（G7 停止条件原文）；
  若不回落，停顿源在 B 其余路径/layout，与 signal 无关。

### C4-m 通知/调度路径（mediating，已按停止条件降级）

- 证据：runnable delay +36.4 ns/op（+0.94% [+0.38,+1.50]，ns 精度 schedstat 直接测量），
  是同窗口 on-CPU 增量 +474.8 的 7.7%；四分类账：主动执行 +475、等 CPU +36、park 推导
  −5、迁移 0（`counters/C4_SCHED.md` §6）；B 的 0.5589/op "wakeups" 是用户态通知计数，
  写时 eventfd waitqueue 空（写者=提交线程=唯一 park 者，progress_handoffs=0，源码级论证），
  不产生 sched_wakeup；poll 100% POLLIN 零超时（C3）。
- 结论：调度级延迟不构成 D2 的 CPU 根因（G2 停止条件触发）；off-CPU 时间不计 CPU 工作。
  **注意：这不免除它在延迟（wall）路径上的角色**——延迟超额 +340~770 ns/op 是 wall 分量，
  逐事件 wakeup→run/park 分解 root-only UNOBSERVED（见 §5、G12）。

### O2 flush 时点迁移（originating-时序 / 成本中性）

- 证据：C2 原生速度直接观测——B 29.93% [29.7,30.4] 的 submit 后 <250 ns 即 peek_batch
  （p10 74 ns），A 0/120,000（p10 322 ns）；flush（实际 io_uring_submit）两臂 1.0000/op、
  enter 100% submit-only (1,0,0) ret≡1、零 batching；单次 flush 成本 702.3→714.4 ns
  （+12.1，同物挪位）；submit_attempts −30.96% 只减空 ledger 调用。**D2 零 enter 节省**
  （cost_table §3.2 守恒关系）。
- 【D】每请求级确认：跨层关联 E2（B 臂，2000 op × 3 phase）defer=1045/1035/1038
  （≈52% 请求）、immediate=955/965/962，flush 2000/phase 不变，enter to_submit 恒 1
  （6004/6004）；1045/2000 的 FLUSH 直接前驱事件是 defer——deferred SQE 由下一 poll 的
  **无条件 submit_transport 吸收**，不形成批量提交（`traces/xlayer/README.md` §5.3）。
  与 Phase B 计数器 0.5589/op 同量级（不同 run：2000 op、warmup=0、measured=1）。
- 结论：O2 对 CPU 增量的直接贡献 ≈ +12.1 ns/op（flush 单次变贵）+ poll 路径承载迁移
  （366.8→658.0 ns/call，非新增）；O2 的主要未观测后果在**延迟路径**（L1）。

### L1 deferred-op 完成可见时序（延迟候选主路径；【D】结构前提升级为每请求级直接确认，后果仍 CORRELATION_PARTIAL）

- 证据链：(i) 结构：deferred SQE 直到下一次 poll 才 flush/enter——C2 syscall 级直接观测，
  【D】升级为每请求级 join 直接观测（O2 节【D】行：defer→FLUSH 前驱关系、to_submit≡1）；
  (ii) 后果形态：延迟分布整体抬升且 p50 位移（+532 ns）为 CPU 位移（+189 ns）的 2.8 倍——
  存在 CPU 之外的逐 op wall 分量；(iii) 旁证：C2 strace 下 B latency-pass p50 为 A 的
  2 倍（77.0 vs 38.6 µs）且 syscall 计数逐条相同（ptrace 扭曲 4–9×，只作方向旁证）。
- 【D】机制表述修正（内核侧）：完成并非"submit 现场闭环"——warm tmpfs 4k read 100%
  经 io-wq 排队（qasync 6004/6004）且全部 complete 由 io-wq worker 线程投递（两臂同构，
  X1）。L1 的准确机制是：**deferral 把该 op 的内核工作起点（submit_req→qasync）推迟一个
  poll 周期**，从而推迟 worker 完成/事件可见的时刻；A 立即 submit → 内核工作立即开始。
  原"(iv) A 的 op 在 dispatch 内闭环"的表述作废；C3 行为学探针的数据（CQE 在 peek 时
  已就绪）不矛盾——它测的是用户侧可见性，不是投递上下文。
- 缺失：deferred 状态 × 逐 op 延迟分位数的 clean（单仪器）直方图；关联 run 是双仪器，
  其 complete→reap 差（E1 p50 5367 vs E2 5619 ns）只作次序验证，禁止归因引用
  （`traces/xlayer/README.md` 头部声明）。L0 vs L1 裁决由 CF-1 `sigdrop` 的 p50/p99
  读数承担（§6），或由单仪器用户态环直方图承担（G12）。

### L2 尾部分量（p99 比 p95 多 ~+320 ns，UNOBSERVED）

- p99 绝对位移 +955 ns vs p95 +634 ns：存在附加尾部分量。候选：罕见调度交互
  （nonvoluntary ctxsw +1.72% [1.30,2.12]）、收割形态（单 CQE 收割 B ~20% vs A ~15%）、
  通知合并/写模式的长尾组合。逐事件 wakeup→run 分布、抢占者身份：root 能力【D】已
  恢复并实测（bpftrace 15 探针零丢失，`CAPABILITY_MATRIX.md` §12），剩余是 run 预算
  与隔离问题，不再是能力不可达。

### X1 跨层关联事实（Phase D，2026-11-04；`traces/xlayer/README.md`）

- 关联基础设施：用户态 7 点事件环（64 B × 1 Mi，编译宏+env 门控，三轮 dropped=0）+
  root bpftrace 13 io_uring/syscalls tracepoint（`@ring_owner[ctx]` 归属，含 io-wq
  内核线程上下文）；跨仪器时钟同源（flush↔enter 差 p50=773 ns）。11 节点链 10 个
  直接测量全覆盖，join 6000/6000（E1/E2）与 64/64（E3），零孤儿 complete。
- 正确性审查通过：cookie 每 ring 严格单调零复用、(ctx,user_data) 全局唯一、内核 req
  指针跨 submit_req→qasync→complete 零违例、slot 复用全伴随 generation 递增、每请求
  7 用户事件恰好各一条；io_uring↔block 请求明确 N:M 不假设一一对应。
- 直接观测（修正/确认）：(a) warm tmpfs 4k read **100% 经 io-wq 排队且完成全部由
  io-wq worker 投递**（两臂同构）——修正 C3"inline 完成"的**解释**（数据不矛盾），
  并进一步支持"io-wq 不承载 B 专属成本"（+0.46% n.s.）：同一机制两臂等量行使；
  (b) B 臂 defer 52% 请求、flush 不变、to_submit 恒 1（O2【D】行）；
  (c) 内核 io_uring_defer / task_add / local_work_run / cqe_overflow / req_failed
  **0 次行使**——用户臂 B 的 defer 是用户态 transport flush defer，与内核
  io_uring_defer 是不同层的不同机制，**不得混用同名归因**；
  (d) 用户态 7 类事件 tid 全部=bench 主线程，无用户态线程交接（progress_handoffs=0
  的每请求级印证）；io-wq worker 与 bench 同 cpumask 同 cpu=2，无跨 CPU 迁移
  （C4 migrations=0 的印证）。
- 边界（CORRELATION_PARTIAL，README §6）：block 层未关联（tmpfs 无 block 请求；
  E3 btrfs 块活动 N:M 未采集）；io-wq 执行段内部（qasync→complete 之间）无中间事件；
  statx 构造探针（user_data=0）已剔除；task_work_run 按 pid 过滤的漏检风险无反证。
  E3（btrfs dirty-fsync 64 op）complete→reap p50 23,183 ns / max 201,477 ns——独立
  场景数据，不进入 D2 归因。

### 已排除的竞争路径（负结果，防回归引用）

| 竞争解释 | 排除证据 |
|---|---|
| io-wq CPU 增加 | io-wq +0.46% [−0.44,1.36] n.s.；12/12 零 read/write syscall；futex 2/进程常量（C3） |
| enter 参数/内核路径变化 | enter 100% (1,0,0) ret≡1；时长 B−A 双仪器 CI 含 0（C2） |
| batching 掩盖 | 240,000 submit 全 to_submit=1、零部分提交（C2） |
| 缺页/内存足迹 | page-faults +0.15% n.s.；cache-misses +0.65% n.s.（§3.1 #9/#11） |
| 设备 I/O（C6） | warm tmpfs、page_cache_resident；分支未触发（§6） |
| 调度等待构成 CPU 根因 | runnable +36.4 ns = on-CPU 增量的 7.7%；off-CPU 不计 CPU（C4） |
| 通知产生真实 sched_wakeup | 写者=唯一 park 者，写时 waitqueue 空（C4 源码论证 + timeslices 不变） |
| read/poll/drain 面新增 | B−A = −0.00718/op（显著为负，C3） |
| 内核 io_uring_defer / task_add / local_work_run 参与 | 【D】三轮 0 次行使（tracepoint 全量实采，X1）；用户臂 defer 是 transport flush defer，与内核 defer 不同层不同机制，禁止混用同名归因 |
| 用户态线程交接/跨 CPU 迁移参与 | 【D】7 类用户事件 tid 全部=bench 主线程；worker 同 cpumask 同 cpu=2（X1 §5.2(d)），与 C4 migrations=0/progress_handoffs=0 互证 |

## 4. CPU 路径对账（闭环表）

| 层 | 分量 | ns/op | 证据等级 |
|---|---|---|---|
| C3 | eventfd write on-CPU | +132~138 | 直接测量（TSC × 密度，双块） |
| C1 | mutex+epoch+write 用户 stub+defer 门 | +26~33（执行 ≈24–27 + defer 门 6.6） | 直接测量（in-situ TSC） |
| C1 | flush 单次变贵 | +12.1 | 直接测量 |
| C5 | 用户停顿（cycles 超出指令部分） | ≈68–71 | CORRELATION_PARTIAL（PMU 差值，延期） |
| U | 残差（口径差+未归因） | ≈27–35（中央） | UNOBSERVED |
| 合计 | （对 +189.15；各块效应 158/182/232） | | write 单项已超内核净增量上限（TSC vs tick 口径差 ~46 在残差内） |

解释比例：155–162 / 158–232 = **67–94%（中心约 80%，上限口径）**。
不宣称单一精确百分比（无仪器 OFF 效应块间漂移 + 探针扰动同量级）。

## 5. 延迟路径与关联需求（【D】修订：关联已建成，剩余需求收敛到反事实/单仪器直方图）

单层指标为何到头（维持原判）：
1. CPU 分解（/proc 三分法）只解释 +189 ns/op；延迟位移 +532~955 ns/op，超额
   +340~770 ns/op 在任何现有单层**聚合**账本（CPU、计数器、syscall、schedstat 总量）中
   无对应行。
2. C4 的 wall 侧总量（runnable +36.4）比超额小一个量级；park 推导 n.s.——但 schedstat
   是窗口总量，无法给出"deferred op 的 park/等待比非 deferred op 长多少"。
3. 分布形态（p50 ≥ p95 相对位移）排除"纯尾部机制"，但 L0（CPU 均值平移）与 L1
   （deferral 逐 op 等待）的裁决需要逐请求数据。

【D】关联阶段的产出与判定：
- 已建成并验证（X1）：11 节点链 10 节点直接测量、join 6000/6000 零丢失、正确性审查
  全过、跨仪器时钟同源。它把 L1 的**结构前提**从 syscall 级升到每请求级（O2【D】行），
  并修正内核侧完成上下文（io-wq worker 投递，两臂同构）。
- 但关联 run 是**双仪器豁免 run**（用户环 + bpftrace 同开），全部时延数字只作次序/
  关联验证，不构成性能归因——L1 的延迟后果、L0/L1 裁决、L2 分量在 clean run 中
  仍然未测（G12 维持开放）。
- **剩余需求不再要求新的跨层构建**：L0/L1 裁决由 CF-1 `sigdrop` 的 frozen 协议
  p50/p99 读数承担；若需段级分解，可复用**已验证的用户态 7 点环**（单仪器、
  macro+env 门控、dropped=0 已证）跑 deferred × 延迟分层直方图——这是单层
  instrumentation，不与 perf 叠加、单独量化环开销即可。内核 tracepoint（root 已恢复）
  仅在需要 worker 投递时刻的精化时作为可选增强。
- 据此本轮综合的 needCorrelation 判定从 true 翻转为 **false**：所需跨层关联已存在，
  剩余归因缺口由反事实与单仪器直方图关闭，不再阻塞在"没有跨层关联"上。

## 6. 反事实规格（证据驱动、单因素、已冻结）

### CF-1 `sigdrop`（对应 G10；mutant 宏已存在于研究副本）

- **冻结假设**：O1 的 residue 通知（`advertise_residue` 因 deferral 成立 →
  `signal_ready_progress()`）是 D2 CPU 增量的主因（≥60% 消失）。
- **单因素**：仅定义 `SLUICE_595_MUTANT_DEFER_RESIDUE_SIGNAL_DROPPED`——从
  `advertise_residue` 中移除 `unsubmitted_after_dispatch` 一项；deferral 本身、
  flush 时点、全部门链不变。
- **机制预测**：`wakeups` 0.5589→0、`wakeup_writes` 0.2353→0；bench 线程 syscw 的
  B−A 差（+86,912/inv）→0；内核 stime 增量 +79.37→≈0；`sqes_deferred` 仍 0.5589、
  `flushes`/`actual_enters` 仍 1.0000/op；perf `__GI___libc_write` 符号份额回落为 0。
- **性能预测**：CPU/op +3.70% → ≤+1.0%；同时记录 p50/p95/p99：
  (i) 若 p50/p99 随 CPU 等比例回落至 ≈+2%——延迟主要是 CPU 均值平移（L0 主导）；
  (ii) 若 p50/p99 仍 ≈+5~7%——延迟位移与 signal 无关，L1（deferral 时序）升级为
  延迟主因候选。**本反事实一箭双雕：同时裁决 CPU 主因与延迟 L0/L1。**
- 【D】关联级扩展（不改冻结假设/预测）：(a) 机制读数可复用已验证的用户态 7 点环
  （单仪器 companion run）：sigdrop 后 U.defer 仍 ≈52%、U.flush 前驱=defer 的比例不变、
  用户态 write 事件消失——验证"只去掉通知、保留 deferral"的单因素纯度；
  (b) root 可用时以 syscalls:sys_exit_write tracepoint 计 write 次数→0（关联脚本已验证
  attach 路径，`traces/xlayer/tool_bpftrace_kernel.bt`）；(c) 性能读数仍取 ring/bpftrace
  均关闭的 frozen full 协议臂——**仪器 run 与 timing run 不混块**（研究纪律原文）。

### CF-2 `deferoff`（G10 竞争假设 (b)(c) 的裁决器）

- **冻结假设**：D2 的全部效应（CPU 与延迟）来自 defer 决策链及其连带通知；B 其余
  代码路径（bounded_progress 状态、其余门、进度语义）不构成可测成本。
- **单因素**：`defer_transport_flush_locked_` 入口直接 `return false`（单点改动）——
  deferral、门链内段、residue 通知全部不再触发，flush 回到 dispatch 点。
- **机制预测**：`sqes_deferred`→0、`wakeups`→0、`submit_attempts` 回到 ≈1.79/op、
  syscw 差→0；C2 shim 下 submit 后 <250 ns 即 peek 的比例 →0（poll-pass flush 群体消失）。
- **性能预测**：CPU/op Δ→0（|Δ|<1%，CI 含 0）；p50/p95/p99 Δ→0。若 CF-1 后 CPU 仍
  >2.5% 而 CF-2 后归零——defer 分支本身/微架构 layout（G10 假设 (b)(c)）承担残差，
  C5 问题域升格。
- 【D】关联级扩展（不改冻结假设/预测）：机制读数下 U.defer→0、U.immediate=100%、
  K.submit_req 的时间位置全部回到 submit 现场（FLUSH 前驱不再是 defer）；性能读数
  同样取无仪器 frozen 协议臂。

两反事实均：研究副本构建、与 A/B 同轮 ABBA 平衡、12 对、单独 run、不与 perf stat 叠加；
mutant 二进制代码布局变化风险已在 G10 登记。

## 7. 裁决门槛（预注册引用）

- CPU：CF-1 使 ≥60% CPU 增量消失 → O1→C1/C3/C5 因果链闭合（G10 停止条件原文）；
  残量 >2.5% → 启 CF-2。
- 延迟：CF-1 的 p50/p99 读数按 §6 分叉裁决 L0 vs L1；若 (ii) 成立，用已验证的用户态
  7 点环（单仪器）跑 deferred × 延迟分层直方图完成段级分解（G12 停止条件：≥60% 的
  p50/p95 超额归到具体段）；在此之前延迟归因保持 CORRELATION_PARTIAL。
- 【D】结构前提不再作为待证项：L1 的 defer→下一 poll 无条件 flush 吸收、to_submit≡1、
  完成由 io-wq worker 投递（两臂同构）已是每请求级直接观测（X1）——反事实只需裁决
  **后果**（CPU 份额与延迟分叉），不需再证结构。
