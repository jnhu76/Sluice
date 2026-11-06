# #595 R4 — DESIGN（信息审计 / 状态-动作表 / S 算法与语义义务 / 消融差异 / 参考规则映射）

配套 `PREREGISTERED.md`（同刻冻结）。本文件是设计描述，不承载门槛；行号引用
worktree @f3b25329。

## 1. 信息审计表（决策输入 → 权威绑定 → 可读臂）

决策函数 `budget::evaluate(Input)`（`bench/r3_ready_batch.hpp:252-289`）为纯表求值；
Adapter 内无 getenv、无 case/depth/文件名引用（臂选择仅在 `main` 读一次环境变量，
`bench_w03_external_loop.cpp:862`、`bench_w02_pipeline.cpp:865`，经 `effective_arm`
强制规则 ：756-758）。

| 输入 | 权威/来源 | 性质 | 读取者 | 禁止/限制 |
|---|---|---|---|---|
| `remaining` | caller 本次呈现的 ready 跨度长度 | 调用方自有事实 | 全部 budgeted 臂 | — |
| `eligible_prefix` | `batch_shaped_prefix`：对 caller 自己的 op 按 `len==4096 && dst!=nullptr` 镜像谓词（:187-196） | 冻结形态镜像；入口内重新权威求值 | 全部 budgeted 臂 | 镜像不是权威；入口可接受更少 |
| `active_data` | `AsyncIoContext::outstanding()`（公共权威 API；`async_io_context.hpp:273`、`async_io_context.cpp:837-840` = 持锁转发 backend 的 accepted-未收集计数） | 库侧唯一合法的 in-flight 事实 | S、SNP、SHADOW | SNI 不读；任何人不得读 SQ/CQ 环私有状态 |
| `pressure` | driver 最近一次 owner pass 快照（`last_pass_completed>0 ∨ last_pass_immediate_work`，`pressure_known` 门控），经既有 `shared.mutex` 通道由 harness 携带（字段 `bench_w03_external_loop.cpp:154-158`、发布 `:244-258`、submitter 持锁读取 `:415-422`） | 调用方域的完成事实；不为 hint 发起新 poll | S、SNI、SHADOW | SNP 不读；pressure_known=false 时恒 false |
| `caller_window` | caller 侧未收集 window 的无锁镜像（`DecisionFacts::caller_window`） | 仅观察（shadow 记录），永不作决策权威 | SHADOW | 不进任何决策 |
| prior stop（control/recovery）/ `residue_advertised` | 上一次呈现的入口返回值 | authority 边界事实 | 全部 budgeted 臂 | — |
| `pinned_quantum` | 臂策略常量 | 消融控制量 | Q2、Q8 | — |
| SQ 剩余空间 | backend 私有 | **不可读**（审计明令禁止调用方读 SQ space） | 无人 | 诊断只记录 `sq_budget` stop 投影；容量界由入口权威执行（stop=cap） |
| 实际 enter 提交尺寸 | 内核/backend 私有 | 不可读 | 无人 | 以 presented/accepted/flush 报告字段为代理 |
| case id / depth / request_count / fixture / env / 文件名 | harness 私有 | **算法不得读**（结构保证：不在决策输入枚举内） | 无人 | 诊断输出可用，决策不可用 |

## 2. 状态-动作表（冻结；`kTable`，`r3_ready_batch.hpp:222-244`）

| 行 | 谓词 | 动作 | quantum 效果 | 语义 |
|---|---|---|---|---|
| `remaining_single` | `remaining==1` | immediate | 1 | 单条不值得批量入口 |
| `front_ineligible` | `eligible_prefix==0` | immediate | 1 | 首条不合批形态 → 走产品路径由入口权威判定 |
| `authority_signal` | prior stop∈{control,recovery} ∨ residue | authority | 0 | authority 边界后不重入，余量走产品 singles |
| `quantum_base` | 恒真 | batch | 置 8 | 默认/静态回退 |
| `inflight_zero` | inflight hint active ∧ `active_data==0` | batch | min(·,2) | 空管时小量子：早提交保 CPU 忙 |
| `completion_pressure` | pressure hint active ∧ pressure | batch | min(·,2) | 完成积压时小量子：尽快收割 |

求值规则：ACTION 首个非 batch 匹配行获胜（非 batch 优先）；quantum 效果叠加
（base=8，clamp2 取 min）；batch 时 `present_len=min(quantum, eligible_prefix)`
（0→1）；`pinned_quantum≠0` 覆盖为 pinned（行名 `quantum_pinned`）。SHADOW 对同一
表求值并记录 counterfactual，但呈现按 R（整跨度+cap 重入）。

## 3. S 最终算法与语义义务

算法（一句话）：表驱动 quantum（默认 8；`outstanding()==0`→2；合法 completion
pressure→min(·,2)），batch_limit=min(quantum, SQ 容量, eligible prefix)，静态回退
8；cap stop 且有进展时 untouched 后缀经同一批量入口重入；input_exhausted 且有余量
时 chunk 继续；authority 信号（control/recovery/residue）时退出到产品 singles；
remaining==1 或首条不合形态时 immediate。

语义义务（全部由确定性 oracle + 变异证据锚定，`evidence/correctness/RECEIPT.md`）：

1. **不丢工作**：每次成员恰一次 accept、恰一个内核 SQE（M1 重复接受变异被杀）；
   untouched 后缀永不与已接受前缀重叠。
2. **无零进展循环**：无进展/卡门时呈现恰一次并有界返回（M2 变异被杀）；
   prefilled_residue 下零接受 stop 呈现恰 1 次、无重入计数。
3. **authority 边界精确**：control/recovery/residue 边界呈现恰 1 次、退出到
   authority（M3 变异被杀）；shutdown/poison 封闭、并发下 id 唯一且旧 key 不复现。
4. **数据/通知正确**：real-kernel 场景逐字节 pattern；overflow flush 后逐一发布、
   完成数单调恰等；borrow 逐 payload 释放。
5. **hint 只影响量度不影响义务**：SNI/SNP/Q2/Q8 的差异仅是输入位/覆盖常量；
   一切语义义务与 S 相同（同套件全过）。
6. **信息边界**：S 的一切决策输入见 §1；不得读 SQ 空间/case/环境（结构保证）。

## 4. 消融臂差异表（`arm_policy`，`r3_ready_batch.hpp:160-182`）

| 臂 | 呈现动作 | 读 inflight | 读 pressure | pinned | 用途（冻结读法） |
|---|---|---|---|---|---|
| S | 表驱动 quantum + cap 重入 + chunk | ✓ | ✓ | — | 完整候选 |
| SNI | 同 S（表内 inflight 行永不激活） | ✗ | ✓ | — | S−SNP 的补：隔离 inflight hint 动作价值 |
| SNP | 同 S（pressure 行永不激活） | ✓ | ✗ | — | 隔离 pressure hint 动作价值 |
| Q2 | quantum 恒 2 | ✗ | ✗ | 2 | 固定小量子对照 |
| Q8 | quantum 恒 8 | ✗ | ✗ | 8 | 固定默认量子对照；与 R 提交等价（16-op refill 冒烟同计数） |
| SHADOW | 按 R（整跨度+cap 重入） | ✓（记录） | ✓（记录） | — | SHADOW−R = 纯读取成本；counterfactual 量子入计数器/诊断行 |
| R | 整跨度呈现 + cap 重入 | ✗ | ✗ | — | 主候选之一（动作基线） |
| B | 整跨度一次呈现 + suffix singles | ✗ | ✗ | — | R3 轮候选（对照背景） |
| A0 | 产品逐条（无 adapter） | — | — | — | 一切对照的锚 |
| A1 | adapter 单条 | — | — | — | 仅归因（PREREG §11.4） |

计数器（流经 bench Record printf+JSONL，不可被优化消除）：
`r4_batch_presentations / cap_reentries / chunk_reentries / immediate_decisions /
authority_exits / quantum{1,2,8} / active_zero_decisions / pressure_decisions /
shadow_decisions / shadow_outstanding_sum / shadow_window_sum`。

诊断行（仅 `SLUICE_595_R4_DIAG_LINES` 诊断运行；落盘逻辑
`bench_w03_external_loop.cpp:341-358`、行格式 `format_decision_record`
`r3_ready_batch.hpp:337-358`）：每 phase 一行 `# phase <case>/rep<N>/<measured|oracle> arm=<A>`
+ 每决策一行 `ready/eligible/present/quantum/row/active_read/active/pressure_read/
pressure/pressure_known/pass_completed/pass_immediate/caller_window/accepted/stop/
untouched_begin/untouched/flush/residue/sq_budget_bound/cause(first|chunk|cap_reentry)`。
样例 64 行：`logs/r4_diag_lines_sample.log`（可见 `inflight_zero`→`quantum_base`→
`completion_pressure` 的状态轨迹与 active 生长 2,4,6,…）。

## 5. 参考规则映射（参考规则 → Sluice 可用信息 → 可执行动作 → 不适用前提）

### 5.1 IX: A Protected Dataplane Operating System（OSDI 2014）

原文原则："(i) we never wait to batch requests and batching only occurs in the
presence of congestion; (ii) we set an upper bound on the number of batched
packets"（§3；批大小上界实验 B=64/iteration，§5.1）；批的存在理由是摊销与局部性，
不是攒请求；run-to-completion 消除中间缓冲、每个包只走一遍全部处理阶段。

| 参考规则 | Sluice 可用信息 | 可执行动作 | 不适用前提 |
|---|---|---|---|
| 永不为批而等待（只消费已存在的工作） | ready 跨度 `remaining`（caller 已构造完的 op） | 只呈现已就绪前缀；无任何等待/攒批路径（W03 submitter 在 completion 驱动下 refill） | IX 的"congestion 才批"针对网络收包路径；Sluice 的合发触发是 caller 一次递交多条 ready——**等待判据不适用**，但"不攒请求"义务保留（S 的决策从不阻塞） |
| 批大小有上界 | quantum（表驱动 8/2）+ 入口 cap | batch_limit=min(quantum, SQ 容量, eligible prefix)；量子即上界 | IX 的 B=64 是包路径调参值；Sluice 上界须与 SQ 容量/eligible 联合，不照搬常数 |
| run-to-completion（一遍过、无重复遍历） | 入口权威的 untouched 报告 | accepted 前缀一次过；untouched 后缀仅经同一入口重入（R/S），不做重复扫描已消费前缀 | IX 是协议阶段流水；Sluice 对应义务＝**无零进展循环/无重复 accept**（M1/M2 变异锚定） |

### 5.2 Linux 6.12 NAPI documentation（docs.kernel.org/6.12/networking/napi.html）

原文要点：poll 带 budget；预算耗尽时驱动 "return exactly budget" 并留在 poll list
被再次 poll——剩余工作不丢失、无需重新调度；`napi_complete_done` 只在全部完成时
调用（预算 0 时绝不）；polling 期间 IRQ masked；『还剩工作』不是无限循环许可，
poller 返回让出 CPU，残留由下一轮继续。

| 参考规则 | Sluice 可用信息 | 可执行动作 | 不适用前提 |
|---|---|---|---|
| 本轮预算与残留义务分开 | quantum（本轮预算）＋ untouched/residue 报告（残留义务） | cap stop 时 untouched 后缀保留、有进展即经同一入口重入（残留不丢失）；residue 边界则退出 authority（不假装完成） | NAPI 的预算由中断子系统给定；Sluice 的 quantum 是调用方侧策略值，且 SQ 容量界在入口内部（调用方不可读环） |
| 预算耗尽返回 exactly budget、留在队列 | 入口 stop=cap + accepted>0 | `stop=cap 且 accepted>0` → 重入（R/S 的 reuse_on_cap）；返回值如实携带 stop 原因 | NAPI poller 挂在 softirq 上下文；Sluice 重入发生在同一提交调用内，无调度点 |
| 完成才 complete / 轮询期不重复中断 | owner pass 快照（pressure facts） | pressure_known 门控：没有 pass 事实就不声称压力；hint 不发起新 poll | NAPI 的 IRQ masking 无对应物——Sluice 的 completion 通知机制（borrowed fd/epoll）不在本轮改动面 |

### 5.3 io_uring for High-Performance DBMSs（Jasny et al., PVLDB 19(1) 2026）

原文（§3.3.3）：adaptive batching "adjusting the batch size based on the ratio of
outstanding I/Os to waiting fibers. When many I/Os are in flight, additional
submissions are deferred to increase amortization; when few are pending, batches
are flushed earlier to keep the CPU busy"——提交策略由库侧 outstanding 与调用方
runtime 侧 waiting fibers 两域信息共同决定。

| 参考规则 | Sluice 可用信息 | 可执行动作 | 不适用前提 |
|---|---|---|---|
| 库侧可感知 outstanding | `AsyncIoContext::outstanding()`（公共权威 API） | `active_data` 输入：`outstanding()==0` → quantum 2（早提交保忙），否则默认 8 | 论文场景是 Boost.fibers DBMS runtime；Sluice 调用方无 fiber 概念 |
| 库内 outstanding 不能推断调用方 runnable/waiting 执行体或急迫性（waiting/ready 属调用方 runtime） | 调用方域的完成事实经 harness 作为参数传入（pressure facts），**库/adapter 不自 poll** | `pressure = pressure_known && (last_pass_completed>0 ∨ immediate_work_remains)` → min(·,2) | 论文用 outstanding/waiting **比值**调批量；Sluice 冻结为表驱动阈值形态（0/压力→2，默认 8）——比值连续调节不做（无调用方 waiting 计数的合法来源，且会引入调参自由度） |
| many in flight → 推迟提交以摊销 | outstanding 高 + SQ 容量界 | 高 in-flight 时不缩量（默认 8），容量界由入口执行 | Sluice 无"defer"原语（不做时间性推迟）；摊销由量子上界承载 |

### 5.4 Shenango（NSDI 2019）

原文：拥塞信号是『旧队列成员跨采样周期仍未被处理』，不是队长、更不是队长导数
（Algorithm 1："if runq contains threads in prev_runq or inq contains packets in
prev_inq"；排队时长 ≥ 采样周期 5μs 才算 congested）；"the duration of queuing is a
more robust signal than the length of a queue, because using queue length requires
carefully tuning a threshold parameter"；实现=比较当前 head 与上一轮 tail 指针。

| 参考规则 | Sluice 可用信息 | 可执行动作 | 不适用前提 |
|---|---|---|---|
| 用『成员滞留时长』而非队长/队长导数作拥塞信号 | owner pass 快照的**事实性**字段：`last_pass_completed`（本轮 pass 完成了多少——有成员被处理的事实）与 `immediate_work_remains` | pressure = 完成>0 ∨ 尚有立即工作：是"消费者正在产出/仍有义务"的布尔事实，非长度阈值 | Shenango 有 5μs 采样周期与 prev 快照可比较成员身份；Sluice 无请求排队时间戳的公共权威（Request ready 查询是逐成员的，跨 span 聚合会引入新状态），故不用滞留时长——这是**不适用前提下的降级选择**，并以此换取零新增运行时状态 |
| 队长需要调阈值、不稳健 | outstanding() 是合法事实但作**长度**使用需阈值 | 本轮**不用** outstanding 的绝对值当拥塞判据（只判 ==0 的空管边界）；非零区间一律默认量子，避免阈值调参 | 若未来引入"outstanding ≥ k 即拥塞"，须先解决 k 的来源与审计（本轮明示不做） |
| 信号可实现为两次观察的指针比较（廉价） | pressure facts 由 driver 在既有 pass 内顺带发布（零新增 poll、零新增锁） | harness 经既有 shared.mutex 携带；adapter 端为两次字段读 | Shenango 信号在调度器内闭环；Sluice 的等价物必须跨线程边界携带——以调用方参数而非库内自 poll 实现（信息归属：完成事实属 progress owner） |

## 6. 与预注册的关系

本文件的表与算法描述若与 `PREREGISTERED.md` 的冻结口径有出入，以 PREREGISTERED
为准；源码行为以 worktree @f3b25329 为准（`bench/r3_ready_batch.hpp` 内容 sha256
`363f346a…`）。
