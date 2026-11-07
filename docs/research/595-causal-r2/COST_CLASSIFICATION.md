# COST_CLASSIFICATION — Sluice #595 D2（causal-r2）成本归因分类与候选机制排序

状态：ACTIVE EVIDENCE。撰写：综合分析师（2026-11-04，Phase C 机制综合）。
**Phase D 修订指引（2026-11-04）**：跨层关联（`traces/xlayer/README.md`）之后的排序与
反事实修订见 `CAUSAL_DAG.md`【D】标记处与本文件对应行的增量说明：O2/L1 结构前提
升级为每请求级直接确认（defer 52%、flush 不变、to_submit≡1、deferred SQE 由下一
poll 无条件 flush 吸收）；C3 "inline 完成"解释修正为"io-wq worker 投递（两臂同构）"
（成本结论不变）；L2 的 root 能力已恢复（`CAPABILITY_MATRIX.md` §12）；反事实仍为
`sigdrop`/`deferoff` 两个，冻结假设/预测不变，仅追加关联级机制读数扩展。
CPU/tail 两问的判定不变：CPU 主体已解释（67–94%，中心 ≈80%）、延迟仍未解释。
配套因果图：`CAUSAL_DAG.md`（同目录）；数字来源：`counters/cost_table.md`（§ 编号引用）、
C1/C2/C3/C4 分支深挖（任务给定 + `counters/C1_COUNTERS.md`、`traces/c2-syscall/README.md`、
`counters/C3_COUNTERS.md`、`counters/C4_SCHED.md`）、延迟重算
`counters/analysis/latency-percentiles.md`。

分类法纪律：originating = B 特有的机制决策（没有它下游不发生）；mediating = 承载成本的
执行/微架构/调度层；observed symptom = 被测指标本身。**不凭占比定根因**：占比最高的
单项（write on-CPU +138 ns/op）被归为 mediating，因为它由 O1 的决策触发且在 D1 对照中
随机制整体消失；占比小不等于无关（C5 停顿 ≈68–71 ns/op 仍是必需解释项）。

---

## 1. 候选因果机制排序

| 序 | 机制（DAG 节点） | 域 | 角色 | 状态 | 支持证据（关键三件） | 可证伪条件 |
|---|---|---|---|---|---|---|
| 1 | **O1 defer 决策 + residue 通知**：`defer_transport_flush_locked_` 五门全过持有 SQE，`advertise_residue`→`signal_ready_progress()` | C2（决策） | **originating** | 直接测量（计数确定性）+ 源码路径一致；严格因果待 CF-1 | ① `sqes_deferred`/`wakeups` 0→0.5589/op（绝对 CI [0.5572,0.5605]，cost_table §3.2 #20/#28）；② C1 门通过率 owner 1.000/witness 0.562 per op；③ D1 精确对照 `sqes_deferred=0` 且 CPU 中性（§3.4）——机制随形状开关 | CF-1 `sigdrop`：≥60% CPU 增量消失则成立；残量 >2.5% 则 defer 分支本身升级（G10） |
| 2 | **C3-m write(eventfd) syscall**：0.230–0.241/op × 562.7–592.5 ns/call | C3 | mediating | **直接测量**（双块 TSC + 内核记账闭环） | ① syscw +86,912/inv、8.005 B/次全为 8 字节 write；② per-op +138.25 [+136.8,+139.8] 超内核净增量上限 [+37.8,+124.6]；③ read/poll/drain 面为负（−13.03 ns/op）——唯一正贡献内核路径 | CF-1 后 syscw 差与内核 stime 增量归零；否则存在其他内核路径（现无候选） |
| 3 | **C5-m 用户态微架构停顿**：cycles:u +172.56/op 中 ≈68–71 非指令执行 | C5（**延期**） | mediating | CORRELATION_PARTIAL（PMU 差值推断，事件级归因 UNOBSERVED） | ① instructions:u 仅 +1.06% vs cycles:u +5.71%（解释 18.6%）；② IPC −4.36% [−5.42,−3.28]、cache-ref +23.31%、branch-miss +14.94%；③ C1 直接测得新增用户执行仅 ≈24–27 ns/op（用户增量 +95.24 的 25–28%） | CF-1 后 cache-ref/branch-miss/IPC 回落（G7 停止条件）；或 PEBS/LBR 指令级归因（root 无关，用户态可试，本轮延期） |
| 4 | **C1-m signal/门链用户态执行**：mutex+epoch 35.3 ns/call、defer 门 +6.6 ns/op、write 用户 stub | C1 | mediating | 直接测量（in-situ TSC clean） | ① signal 155.8 ns/op [154.7,157.0]；② defer 10.2→16.8 ns/call；③ perf B 专属 `__GI___libc_write` 0→8.5 cycles/op、`dispatch_after_accept` +30.8 cycles/op（6 run Lost=0） | CF-1 后上述符号份额消失；`dispatch_after_accept` 自采样回落至 ≈100 cycles/op |
| 5 | **O2 flush 时点迁移（dispatch→poll）** | C2 | originating（时序）/ CPU 成本中性 | 结构直接观测（原生速度 shim） | ① B 29.93% submit 后 <250 ns 即 peek，A 0/120,000；② flush/enter 两臂 1.0000/op、零 batching、submit 时长无差；③ 单次 flush +12.1 ns（仅有的 CPU 项） | CF-2 `deferoff` 后 poll-pass flush 群体消失；D2 的 enter 摊销上限恒 0（已证） |
| 6 | **L1 deferred-op 完成可见时序**（延迟候选主路径） | C2/C4 交界（wall） | mediating（延迟） | **CORRELATION_PARTIAL**（结构直接观测 + 分布形态旁证；逐请求后果 UNOBSERVED） | ① O2 直接观测（同上）；② 延迟分布整体抬升、p50 +532 ns = CPU 位移 2.8×（本轮重算）；③ tmpfs warm 完成 99.6–100% 在 submit 返回时就绪（C3）→ A 闭环于 dispatch，B 的 deferred op 必等下一 poll | 跨层逐请求关联（G12）：deferred 分层直方图；或 CF-1 后延迟不回落（L0 排除） |
| 7 | **C4-m 通知/调度路径** | C4 | mediating（已按停止条件降级） | 直接测量（schedstat ns 精度）+ 源码论证 | ① runnable +36.4 ns/op（on-CPU 增量的 7.7%）；② 写时 waitqueue 空（写者=唯一 park 者）不产生 sched_wakeup；③ 四分类账 D2 全在执行态（+475/+36/−5/0） | G2 已触发降级；逐事件 wakeup→run 尾部仅当 L2 需要时再启（root） |
| 8 | **L2 p99 附加尾部分量**（p99 +955 vs p95 +634 ns） | C4/U | 候选（延迟） | **UNOBSERVED** | ① 绝对位移 p50→p99 单调增（532→634→955）；② nonvoluntary ctxsw +1.72% [1.30,2.12]；③ 单 CQE 收割 B ~20% vs A ~15%（C2） | root 逐事件 sched_switch/wakeup 或 G12 用户态关联的长尾段分解 |

**observed symptom（非机制）**：CPU/op +3.70% [2.93,4.55]；吞吐 −3.55% [−4.34,−2.84]；
p50 +6.91% [+5.03,+9.23]；p95 +4.96% [+4.23,+5.74]；p99 +7.07% [+4.24,+11.78]；
cycles:u +5.71%、IPC −4.36% 等全部 PMU 行（cost_table §3.1）。

## 2. CPU 退化（+189.15 ns/op）分类账

| 去向 | 类别 | ns/op | 角色 | 证据等级 |
|---|---|---|---|---|
| eventfd write on-CPU（含内核+用户往返） | C3 | +132~138 | mediating | 直接测量（覆盖内核净增量上限；~46 ns 口径差） |
| signal 门链/mutex/epoch/用户 stub + defer 门 | C1 | +26~33 | mediating | 直接测量（其中"新增指令执行"仅 ≈24–27） |
| flush 单次变贵（同物挪位） | C2 | +12.1 | mediating | 直接测量 |
| 用户停顿（cycles−指令差） | C5（延期） | ≈68~71 | mediating | CORRELATION_PARTIAL |
| 口径差 + 未归因残差 | U | ≈27~35（中央） | — | UNOBSERVED（部分即 TSC/tick 系统差） |
| io-wq / 调度等待 / 缺页 / 设备 / enter | C3/C4/C5/C6/C2 | 0 或负 | — | 排除（负结果，清单见 CAUSAL_DAG §3） |

**判定：CPU 退化主体已解释**——机制排序第 1–4 项直接测量合计 155–162 ns/op（原始），
覆盖三块无仪器效应（158/182/232 ns/op）的 67–94%（中心约 80%，上限口径）；但
(i) 精确份额受探针扰动与块间漂移限制（CORRELATION_PARTIAL），(ii) 严格因果闭环
（排除 defer 分支本身/微架构 layout 的竞争解释）依赖 CF-1/CF-2，(iii) 残差 27–35 ns/op
未逐项归因。三者均如实开放（G10、G7/G12、G1 口径差）。

## 3. tail/延迟退化（p50/p95/p99）分类账

| 观测 | ns/op（均值差） | 可归因分量 | 现有证据状态 |
|---|---|---|---|
| p50 | +532 | CPU 均值平移可解释 ≈+189；**超额 ≈+343 UNOBSERVED** | 分布形态直接测量；机制未分解 |
| p95 | +634 | 同上；**超额 ≈+445 UNOBSERVED** | 同上 |
| p99 | +955 | 同上；**超额 ≈+766 UNOBSERVED，其中含 L2 附加尾部 ≈+320（p99−p95）** | 同上 |

**判定：延迟退化未解释**。三点依据：
1. 超额 +343~766 ns/op 是 wall/等待分量，在现有全部单层账本（CPU 三分法、计数器、
   syscall 面、schedstat 总量）中无对应行；C4 wall 侧总量（+36.4）小一个量级。
2. 分布整体抬升（p50 相对位移 ≥ p95）排除纯尾部机制，但 L0（CPU 均值平移）与
   L1（deferral 逐 op 等待）在聚合粒度下不可裁决。
3. 逐请求事件时间线（dispatch→defer→flush→completion→take × deferred 状态）不存在；
   内核侧逐事件与 wakeup→run 长尾 root-only（G12 登记，OBSERVABILITY_LIMIT_REACHED 部分
   + UNOBSERVED 部分已分列）。

吞吐 −3.55% 单列：量级与 CPU +3.70% 一致（同一 pipelined throughput pass 的 CPU 瓶颈），
不构成需要独立机制的 symptom；不用于反推延迟已解释（不同 pass、不同测量）。

## 4. 跨层事件关联需求（needCorrelation = TRUE）

触发条件（任务规则：单层指标无法解释时才需要）已满足：延迟超额分量无单层解释。
最小方案与开销见 `CAUSAL_DAG.md` §5；缺口登记 G12（EVIDENCE_GAPS.md）。
关联范围声明：用户态逐请求时间戳环（无 root 可行）为必需；内核事件（CQE 可见性、
io-wq 上下文、sched 逐事件）为可选增强（root-only，缺失时标 CORRELATION_PARTIAL，
不做"完整逐请求内核分解"声明）。

## 5. 反事实规格（至多 2 个，均单因素、假设已冻结）

| name | 假设（冻结） | 单因素改动 | 机制预测 | 性能预测 |
|---|---|---|---|---|
| `sigdrop` | residue 通知是 D2 CPU 增量主因（≥60% 消失） | 定义 `SLUICE_595_MUTANT_DEFER_RESIDUE_SIGNAL_DROPPED`（从 `advertise_residue` 移除 `unsubmitted_after_dispatch`；deferral/flush 时点不变） | `wakeups`/`wakeup_writes`→0；syscw 差→0；内核 stime 增量→≈0；`sqes_deferred` 仍 0.5589 | CPU/op +3.70%→≤+1.0%；p50/p99 随之回落 → L0 主导；不回落（仍 +5~7%）→ L1 升级为延迟主因 |
| `deferoff` | defer 决策链（含连带通知）承载 D2 全部效应；B 其余路径无可测成本 | `defer_transport_flush_locked_` 入口 `return false`（单点） | `sqes_deferred`/`wakeups`→0；`submit_attempts`→≈1.79/op；poll-pass flush 群体→0；syscw 差→0 | CPU/op Δ→0（\|Δ\|<1%）；p50/p95/p99 Δ→0；若 CF-1 残量 >2.5% 而本项归零 → defer 分支/layout 承担残差（G10 假设 (b)(c)） |

运行规格（两反事实共用）：研究副本构建 mutant 二进制；D2 12 对、ABBA 平衡、同轮
含 A/B/B′；frozen 协议 full 模式（保 latency pass 以读 p50/p95/p99）；单独 run，
不与 perf stat 叠加；记录二进制 sha256 与 `git diff HEAD` sha256（同 cost_table §1 纪律）。

## 6. 与成本表 §6 分类的差异说明（复核口径）

cost_table §6 已给 C1..C6 触发判定与 originating/mediating 初分类；本文件在其基础上做
三处收紧，均有新证据支撑：
1. **延迟单列并判"未解释"**：cost table 只把 p95/p99 列为 observed symptom；本轮重算
   （pair 配对 bootstrap）新增 p50 直接测量，揭示分布整体抬升与 wall 超额——
   这是 §6 未含的判定依据。
2. **O2 拆为"时序 originating + CPU 中性"**：cost table 将 defer/flush+residue 合并列为
   originating；C2 深挖后，flush 时点迁移的 CPU 份额只有 +12.1 ns/op，其主体后果在
   延迟路径（L1），故拆分标注。
3. **C5 延期显式登记**：cost table §6 触发 C5 为介导域；本轮分支深挖上限 4（工作流
   `capped/deferred`），C5 深挖延期，问题域由 G7+G12 承接（EVIDENCE_GAPS.md 新增 C5 节）。
