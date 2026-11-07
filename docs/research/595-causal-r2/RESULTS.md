# RESULTS — Sluice #595 causal-r2 实测结果总表

状态：FINAL（归档汇总，2026-11-05）。本文件只汇总既有实测与已归档分析，**不引入新数字、
不重跑 benchmark**。每条数字的原始文件路径见 §9；方法定义见 `PROTOCOL.md`，最终裁决见
`ROOT_CAUSE.md`，归档结构见 `README.md`。

## 0. 读数约定（跨表引用时必须携带）

- **CPU/op**：每 measured rep 的进程 CPU 差值（`CLOCK_PROCESS_CPUTIME_ID`，本内核上
  包含 io-wq worker），按 `Σcpu_s/Σops` 聚合；主口径。
- **配对差**：块内 pair 级 `(B−A)/A`；正 = B 更差。bootstrap 10,000 次、seed 5950、
  percentile 95% CI。吞吐用 `(A−B)/A`，正 = B 更慢。
- **延迟**：每 rep 的 p50/p95/p99 取 5 个 measured rep 的算术平均；pair 级相对差。
- **有效性**：所有定量对比均为**块内 A/B 同二进制、AB/BA 平衡配对**；块间绝对 CPU/op
  不可直接相减（块间漂移 2.67–4.52%，`ROOT_CAUSE.md` §7 #9）。
- **仪器分离**：计数器 / trace / strace / PMU / split / deferoff 各自独立块，
  **未叠加进同一次 timing run**；性能读数取无仪器冻结协议臂。

## 1. 协议、基线与有效性

| 项 | 值 | 证据 |
|---|---|---|
| 工作负载 | `bench_w02_pipeline`：64 MiB pattern file（warm tmpfs），4 KiB positional read，窗口 depth=2（主结论对象），20,000 valid reads/rep | `PROTOCOL.md` §Protocol |
| 每 invocation | `WARMUP=1 MEASURED=5` = **100,000 measured op**；bench 内部字节 oracle 每 rep 校验；`exit 0` 硬门 | `PROTOCOL.md`；各 `results.jsonl` |
| 亲和/环境 | `taskset -c 2`；Xeon E5-2666 v3（10C/20T）；kernel `7.2.11-200.fc44.x86_64`；schedutil 请求频率 1.2–3.5 GHz；`perf_event_paranoid=2` | `PROTOCOL.md`；`counters/cost_table.md` §1 |
| 基线 commit | `271b81bfc9d64eecc0266fe7066b9040525dd710`（主工作树 clean） | `PROTOCOL.md` §Workspace；`ROOT_CAUSE.md` §10.1 |
| phaseA 五轮 | A/A → D1 → D2 → D3 → D16，各 24 对（48 invocation/轮），共 240 invocation；全部 exit 0、100k op | `raw/phaseA/*/results.jsonl`；`PROTOCOL.md` §Results |
| phaseA A/A 门 | cpu 半宽 0.65 pp、tput 0.64 pp、p99 1.66 pp（门 ≤5/≤3/≤3 pp）→ **PASS** | `PROTOCOL.md` §A/A |
| CF-1 / CF-2 | 各四块 ×12 对 = 96 invocation/组；全部 exit 0、恰 100k op；块间 ≥45 s | `counterfactuals/CF1_SIGDROP.md` §6；`counterfactuals/CF2_DEFEROFF.md` §6.7 |
| CF-0 三臂 | 204/204 invocation exit 0，每臂每 invocation 恰 100k op | `CF0_THREE_ARM.md` §6.4 |
| depth 补测 | 计数器 12 对/深度；trace 每 depth 每臂 1 次 ×3 phase ×20k dispatch；strace 3 次/臂/深度；PMU/split 8–12 对；deferoff 8 对/深度 | `analysis/depth-analysis/DEPTH_MECHANISM.md` §9 |

## 2. 无仪器基线（phaseA，24 对/深度；相对差 B−A）

| 深度 | CPU/op | 吞吐（正=B 慢） | p50 | p95 | p99 | 机制状态 |
|---|---|---|---|---|---|---|
| D1 | **−0.07%** [−0.73,+0.63] | −0.10% [−0.77,+0.59] | −0.72% | −0.06% | −0.14% | `sqes_deferred=0`，机制精确关闭 |
| **D2** | **+3.73%** [+3.04,+4.42] | **+3.58%** [+2.94,+4.22] | **+6.91%** [+5.03,+9.23] | **+4.96%** [+4.21,+5.75] | **+7.07%** [+4.19,+11.80] | defer 但零 enter 摊销 |
| D3 | **−6.69%** [−7.25,−6.18] | −7.20% [−7.86,−6.61] | −3.07% | −11.38% | −9.57% | SQE/enter 1.448 |
| D16 | **−26.36%** [−26.66,−26.08] | −35.88% [−36.44,−35.37] | −32.31% | −31.00% | −28.68% | SQE/enter 5.066 |

来源：`raw/phaseA/{ab-d1-w02-4k,ab-d2-w02-4k,ab-d3-w02-4k,ab-main-w02-4k-d16}/results.jsonl`；
`PROTOCOL.md` §A/B；`analysis/depth-analysis/DEPTH_MECHANISM.md` §2。裁决撰写时的独立重算：
D2 +3.725% [+3.036,+4.423]、D3 −6.687% [−7.253,−6.176]、D16 −26.358% [−26.661,−26.085]
（`ROOT_CAUSE.md` §10.3）。

R1（不同会话/二进制）对照，**只作同域参考、不可与新数字互换**：D2 CPU/op +4.17%
[+3.53,+4.83]、p95 +5.69%、p99 +7.45%；复跑 +3.05/+4.98/+5.81%
（`/home/jnhu/sluice-595-e5/evidence/report-595-e5.md`）。

## 3. D2 CPU 成本分解（+3.70% / +189.15 ns/op，split-d2）

| 层 / 分量 | 数值（ns/op） | 证据等级 | 来源 |
|---|---|---|---|
| C3 eventfd `write` on-CPU | +132~138（raw +138.25；clean +135.2） | 直接测量（TSC × 密度；syscw/wchar 内核记账） | `counters/C3_COUNTERS.md` §0、§2–§5 |
| C1 mutex+epoch + write 用户 stub + defer 门 | +26~33（执行 ≈24–27 + defer 6.6） | 直接测量（in-situ TSC） | `counters/C1_COUNTERS.md` §3、§5.1 |
| C1 flush 单次变贵 | +12.1（702.3→714.4 ns/call，1.0/op） | 直接测量 | `counters/C1_COUNTERS.md` §3 |
| C5 用户微架构停顿（cycles−instructions 差） | ≈68–71 | CORRELATION_PARTIAL（PMU 事件级） | `counters/cost_table.md` §3.1；`EVIDENCE_GAPS.md` G7 |
| 残差（TSC/tick 口径差 + 未逐项归因） | ≈27–35 | UNOBSERVED | `ROOT_CAUSE.md` §3.3 |
| **合计** | **+189.15（+3.70%）** | — | `counters/cost_table.md` §2–§4 |

**口径声明**：C1 完整探针集的 B 开销为 +2.72%，与效应同量级，故 **ON 块不作定量效应**；
定量效应取无仪器 OFF 块与 Phase B split-d2。write 的 132–138 是 on-CPU 墙钟（含内核），
不是 `:k` 指令级分解。**82% 是中心解释比例的上限口径，不是 100% 精确分解**（三块比例
67–94%；`ROOT_CAUSE.md` §3.2–§3.3）。

### 3.1 线程 CPU（split 块，窗口 420k op）

| 分量（D2） | Δ ns/op | Δ% | 显著性 |
|---|---|---|---|
| 提交线程用户 | +95.24 | +7.39% | 显著 |
| 提交线程内核 | +79.37 | +3.99% | 显著 [37.8,124.6] |
| io-wq 内核 | +7.94 | +0.46% | n.s. [−0.44,+1.36] |
| **bench 总差** | **+189.15** | **+3.70% [2.93,4.55]** | 显著 |

来源：`counters/raw/split-d2/`；`counters/analysis/summary.json`；`counters/cost_table.md` §4。

### 3.2 用户态 PMU（u-only，100% enabled）

| 事件 | A→B Δ | 备注 |
|---|---|---|
| `cycles:u` | **+5.71%** | 显著 |
| `instructions:u` | **+1.06%**（+50/op） | 只解释 18.6% 的 cycles 增量 |
| IPC | **−4.36%** | 停顿增加 |
| `cache-references` | **+23.31%** | CORRELATION_PARTIAL |
| `branch-misses` | **+14.94%** | CORRELATION_PARTIAL |
| `cache-misses` | +0.65% | n.s. [−3.74,+5.11] |
| `page-faults` | +0.15% | n.s. [−0.35,+0.69] |

来源：`counters/raw/pmu-d2-ab/`、`counters/raw/pmu-d2-aa/`；`counters/cost_table.md` §3.1。

### 3.3 计数器账本（D2，cnt-on-full-d2，12 对，每 measured op）

| 计数器 | A | B |
|---|---|---|
| `sqes_deferred` | 0 | **0.5589** |
| `wakeups` | 0 | 0.5589 |
| `wakeup_writes` | 0 | 0.2353 |
| `wakeup_coalesced` | 0 | 0.3236 |
| `flushes` = `actual_enters` | 1.0000 | **1.0000** |
| `submit_attempts` | 1.7893 | 1.2354 |
| `poll_passes` | — | 0.7942 |
| `progress_handoffs` | 0 | 0 |

来源：`counters/raw/cnt-on-full-d2/`、`cnt-off-full-d2/`；`counters/cost_table.md` §3.2；
`ROOT_CAUSE.md` 附录 B 原始行重算。

### 3.4 syscall 面（C3，sysio-d2，12 对）

| 指标 | B−A |
|---|---|
| `syscw` / invocation | **+86,912 [+86,541,+87,263]** |
| `wchar` / `syscw` | **8.005 B/次**（8 字节 eventfd write） |
| measured 密度 | 0.24078/op |
| 单次 write | 562.7–592.5 ns/call |
| per-op raw | **+138.25 ns/op** [+136.8,+139.8] |
| read/poll/drain | −0.00718/op；−13.03 ns/op [−17.61,−8.45] |
| io-wq read/write syscall | 12/12 invocation 恒 0 |
| futex | 2/进程常量 |

来源：`counters/C3_COUNTERS.md` §0、§2–§5；`counters/raw/sysio-d2/`、`counters/raw/c3-interleaved-d2/`。

### 3.5 调度域（C4，/proc schedstat，18 对合并）

| 指标 | 值 |
|---|---|
| 提交线程 on-CPU 增量 | +474.8 ns/op |
| runnable delay | +36.4 ns/op（+0.94% [+0.38,+1.50]）= on-CPU 增量的 **7.7%** |
| timeslices | +0.12% n.s. |
| migrations | 0 |
| `progress_handoffs` | 0 |
| eventfd write 是否产生 sched_wakeup | 否（写者=唯一可能 park 者；写时 waitqueue 为空；源码论证 + 计数） |

来源：`counters/C4_SCHED.md` §0、§5；`counters/raw/schedstat-d2/`、`schedstat-off-d2/`、`schedstat-d2-br/`。

## 4. D2 延迟与尾部

| 分位 | 相对差（phaseA 24 对） | 绝对位移 | 备注 |
|---|---|---|---|
| p50 | **+6.91% [+5.03,+9.23]** | **+532 ns** | 相对位移 ≥ p95 ⇒ 非纯尾部加宽 |
| p95 | **+4.96% [+4.23,+5.74]** | **+634 ns** | — |
| p99 | **+7.07% [+4.24,+11.78]** | **+955 ns** | p99 比 p95 多 ~+320 ns（L2） |

三个独立冻结块复现同一形态：phaseA (+6.91/+4.96/+7.07)、CF-1 frozen 24 对
(+5.38/+4.73/+5.85)、CF-2 frozen 24 对 (+6.26/+5.19/+6.03)（`ROOT_CAUSE.md` §4.1）。

**L1 结构前提（每请求级直接观测）**：deferred SQE 直到下一次 `poll()` 才 flush；A 臂在
dispatch 现场 submit。xlayer：E2 中约 52% 请求被 defer（1045/1035/1038），每 phase
flush 数仍 2000，`enter to_submit≡1`；C2 shim：B 有 35,921/120,000 = **29.93%** 的 submit
后 <250 ns 即 `peek_batch_cqe`，A 为 **0/120,000**。trace 构建（单仪器）defer→flush
delay p50/p90/p99 = **617/1234/2777 ns**、mean 820 ns（D2，20,000 dispatch/phase）。

**L2 边界**：p99 超出 p95 的 ~+320 ns 的**归属**在 defer 链内（CF-2 使 p99 总差归零，
保守 ≥96.2%——该界是 p99 **总效应**的界，不是 p99−p95 附加尾部的界）；**段级机制
UNOBSERVED**（G12 未运行）。

## 5. 反事实

### 5.1 CF-0 三臂（A=IMMEDIATE / B=BOUNDED_PROGRESS / C=GATE_ONLY_IMMEDIATE）

| 对比 | D2 | D16 |
|---|---|---|
| C−A CPU/op（门链本身成本） | **−0.52% [−1.28,+0.22]** | **−0.13% [−1.45,+0.82]** |
| B−A CPU/op | +3.21% | −26.80% |
| A/A 12 对门 | cpu 1.47 pp / tput 1.51 pp / p99 2.95 pp → PASS | 同 |

机制读数：C 与 A 的提交面一致（`sqes_deferred=0`、`wakeups=0`、`flushes/enters=1.0`、
`submit_attempts≈1.79`）⇒ 成本来自 **defer 动作及其通知**，不是准入判断。
来源：`CF0_THREE_ARM.md` §6–§8；`counterfactuals/three_arm/raw/`。

### 5.2 CF-1 `sigdrop`（只移除 residue 通知）

| 块 | 二进制 | CPU/op | p50 | p95 | p99 |
|---|---|---|---|---|---|
| base（frozen） | `0091d259` | **+3.34% [+2.30,+4.35]** | +2.56 | +4.42 | +5.03 |
| sigdrop（mutant） | `4de23299` | **−0.58% [−1.11,−0.10]** | +3.87 | +3.90 | +5.29 |
| sigdroprep | mutant | +0.06% [−0.54,+0.69] | +4.73 | +4.67 | +4.45 |
| restore（frozen） | `0091d259` | **+3.34% [+2.40,+4.38]** | +8.21 | +5.04 | +6.68 |
| **合并 frozen（24 对）** | | **+3.34% [+2.62,+4.07]** | **+5.38%** | +4.73% | **+5.85%** |
| **合并 sigdrop（24 对）** | | **−0.26% [−0.67,+0.16]** | **+4.30%** | +4.28% | **+4.87%** |

- 机制读数：`wakeups`/`wakeup_writes`/`wakeup_coalesced` 0.5463/0.2472/0.2991 → **0**；
  `sqes_deferred` 0.5448（同轮 frozen B 0.5463，**保持不变**）；`flushes`/`actual_enters`
  仍 1.0000/op；`syscw` B−A **+86,344 → 0**。
- CPU 消失比例：点估计 **107.8%**，保守 ≥**95.2%**（≥60% 门槛）。
- 裁决：CPU 源 = residue signal/eventfd write；**延迟 ≠ 通知主导**（去通知后
  p50 +4.30%、p99 +4.87% 仍显著；相对 frozen 仅回落约 1pp，CI 重叠）。
- 正确性：13 个 uring B-mode target **13/13 rc=0**；96 invocation exit 0/100k op。
  来源：`counterfactuals/CF1_SIGDROP.md` §5–§8。

### 5.3 CF-2 `deferoff`（`defer_transport_flush_locked_` 入口 return false）

| 块 | 二进制 | CPU/op | p50 | p95 | p99 |
|---|---|---|---|---|---|
| base（frozen） | `0091d259` | **+3.86% [+2.38,+5.40]** | +7.46 | +5.42 | +6.81 |
| deferoff（mutant） | `64189aa8` | **−0.47% [−1.63,+0.67]** | +0.52 | −0.95 | −1.04 |
| deferoffrep | mutant | −1.04% [−2.26,+0.09] | −1.94 | −0.79 | −0.62 |
| restore（frozen） | `0091d259` | **+3.94% [+2.60,+5.26]** | +5.05 | +4.95 | +5.25 |
| **合并 frozen（24 对）** | | **+3.90% [+2.91,+4.91]** | **+6.26%** | **+5.19%** | **+6.03%** |
| **合并 deferoff（24 对）** | | **−0.76% [−1.61,+0.06]** | **−0.71% [−2.57,+1.90]** | **−0.87% [−1.71,−0.10]** | **−0.83% [−2.12,+0.23]** |

- 消失比例：CPU 119.4% / 保守 ≥98.6%；p50 111.4% / ≥69.7%；p95 116.7% / ≥101.9%；
  p99 113.8% / ≥96.2%。
- 裁决：CPU 与延迟源 = defer 决策链（含 deferral）；第二干预点与 CF-1 互证；
  延迟侧 L1 确认、L0 主导排除。
- 正确性：BASE GREEN rc=0；mutant receipt 22 FAIL + rc=134（**设计内拒绝**）；
  12/13 B-mode + 595 rc=134 ⇒ 结论限定于无 held-batch 停滞的 W02-4k-d2 形态。
  来源：`counterfactuals/CF2_DEFEROFF.md` §7–§9。

## 6. 深度边界（D1/D2/D3/D16）

| 指标（per op） | D2 | D3 | D16 |
|---|---|---|---|
| `sqes_deferred` | 0.5589 | 0.6904 | 0.8762 |
| `flushes` = `actual_enters` | 1.0000 | **0.6905** | **0.1974** |
| **SQE/enter** | 1.000 | **1.448** | **5.066** |
| trace flush-size 直方图（20k dispatch，phase 1） | `{1:20000}` | `{1:7128, 2:6436}` | `{1:1343, 7:1166, 8:1311}`（+2 个 3/4 相位边界小 flush） |
| dispatch 时 ledger 非空 | **0/20000** | **6436/20000（32.2%）** | **16178/20000（80.9%）** |
| strace `to_submit` 直方图 | A 全 1 | B `{2:39991, 1:40018}` | B `{8:7494, 7:7494, 1:7590}` |
| defer→flush delay p50/p90/p99（ns） | 617/1234/2777 | 706/3210/5419 | 8363/14462/18919 |
| phaseA CPU/op Δ | **+3.73%** | **−6.69%** | **−26.36%** |
| 每避免一次 enter 的内核节省 | — | **628 ns** | **623 ns** |

**strace 口径限定**：strace 是受 ptrace 扰动的块，enter 密度低于计数器块（D3 B
0.667/op vs 计数器 0.6905；D16 B 0.188/op vs 0.1974，低 3.4–4.7%）；只用于 syscall
类别与 `to_submit` 参数直方图，**不用于 enter 率定量**。

### 6.1 线程 CPU 分解（split-d3/split-d16）

| 深度 | 用户 | 提交线程内核 | io-wq 内核 | bench 总差 | 内核占比 |
|---|---|---|---|---|---|
| D3 | −59.5 ns/op（−4.74%） | **−194.4（−11.00%）** | −103.2（−6.39%） | **−354.8（−7.67%）** | 54.8% |
| D16 | −285.7（−29.42%） | **−500.0（−49.89%）** | −208.3（−17.24%） | **−993.1（−31.0%）** | 50.3% |

io-wq 机制 **UNOBSERVED**；用户分量只是与更少 submit 准备一致的**间接**解释。
⇒ “全部改善由 enter 摊销承担”超出证据。

### 6.2 深度反事实（同一 `deferoff` 二进制 `64189aa8`，8 对/深度，同二进制 ABBA）

| 深度 | CPU/op B−A | 对照 phaseA | p50 | p95 | p99 |
|---|---|---|---|---|---|
| D3 | **+1.07% [−0.47,+2.80]** | −6.69% | +0.49% [−0.30,+1.42] n.s. | **+1.56% [+0.16,+3.23] 显著** | **+4.60% [+0.83,+8.98] 显著** |
| D16 | **+0.26% [−0.69,+1.04]** | −26.36% | +0.37% n.s. | +0.81% n.s. | +0.61% n.s. |

- 改善在 defer 链内（干预后消失）；**没有 restore 块**（phaseA 干预前、不同会话/
  二进制，不构成 restore）⇒ DEPTH_CAUSE **PARTIAL**。
- 正确性面：不同构建，**未在 depth 二进制上重跑 595 oracle**；只有 exit 0 + 字节
  oracle + 400k measured op/inv。
- D3 的 p95/p99 残差如实登记（`DEPTH_MECHANISM.md:51` 旧文“p50/p95 n.s.”低报了 p95，
  已在 `ROOT_CAUSE.md` §8 #11 更正）。

## 7. 跨层关联（xlayer，单仪器结构验证；**不用于性能归因**）

| 项 | E1（A/IMMEDIATE） | E2（B/DEFER） |
|---|---|---|
| 用户事件 | accept=sqe_prep=reap=published=take=2000/phase；immediate=2000；defer=0 | defer=1045/1035/1038（≈52%）；immediate=955/965/962 |
| 内核事件 | enter=enter_ret=submit_req=qasync=complete=6004 | 同 |
| 跨层 join | sqe_prep↔submit_req **6000/6000**；res 一致性 6000/6000；孤儿 complete 0 | 同 |
| complete→reap | p50 5367 ns / p99 18712 ns | p50 5619 ns / p99 18923 ns |
| 用户环 dropped | 0 | 0 |
| 时钟对齐 | 用户 FLUSH vs 内核 enter：p50 **773 ns**、p99 878 ns、max 8171 ns | — |
| 完成投递 | 单一 io-wq worker 投递全部 6004 complete | 同构 |

正确性审查四项：user_data 零重复、`(slot,generation)` 无重复、ring identity 一致、
无用户态线程交接；CPU 全部 = 2（worker 继承 cpumask）。内核 `io_uring_defer`/
`task_add`/`local_work_run` 三轮均 0 次（用户态 defer 与内核 defer 不同层）。
来源：`traces/xlayer/README.md` §2–§6。

## 8. 证据等级地图

| 命题 | 等级 | 依据 |
|---|---|---|
| D2 CPU +3.70% 的因果源 = residue signal → eventfd write | **ESTABLISHED（源级）** | 直接测量 + CF-1 + CF-2 + restore；82% 为上限口径 |
| D2 延迟分布整体抬升的源 = deferral 时序（L1） | **ESTABLISHED（源级）** | 直接测量 + CF-1（L0 主导排除）+ CF-2（归零）+ restore 同向/同域 |
| p99−p95 附加尾部（~+320 ns）的段级机制 | **PARTIAL / UNOBSERVED** | CF-2 归属；G12 段级直方图未运行 |
| D3/D16 改善在 defer 链内 | **PARTIAL** | 同二进制干预消除；无 restore；enter 摊销是最大单项而非全部 |
| C5 微架构停顿（≈68–71 ns/op）的指令级来源 | **CORRELATION_PARTIAL** | PMU 事件级，无 PEBS/LBR |
| eventfd write 内部阶段拆分 | **UNOBSERVED** | 无 root `:k` PMU/fentry |
| D3/D16 的 io-wq 内核 CPU 下降机制 | **UNOBSERVED** | split 只记录伴随量 |
| D3/D16 native submit delay | **受限** | 只有 trace 构建（扰动 ~0.7 µs/op） |
| 内核态 PMU（`:k`）/ 实测频率 | **OBSERVABILITY_LIMIT / 未运行** | paranoid=2，无 root turbostat |

## 9. 原始数据索引

| 用途 | 路径 |
|---|---|
| phaseA 五轮 | `raw/phaseA/{aa-calib-w02-4k-d16,ab-d1-w02-4k,ab-d2-w02-4k,ab-d3-w02-4k,ab-main-w02-4k-d16}/` |
| 计数器 / PMU / split / schedstat / sysio | `counters/raw/{pmu-d2-ab,pmu-d2-aa,pmu-d1x-ab,split-d2,split-d1x,cnt-on-full-d2,cnt-off-full-d2,frozen-full-d2,cnt-on-full-d1x,cnt-off-full-d1x,cnt-on-pmu-d2,cnt-off-pmu-d2,schedstat-d2,schedstat-off-d2,schedstat-d2-br,sysio-d2,c1-interleaved-d2,c3-interleaved-d2}/` |
| 计数器分析 | `counters/analysis/{summary.json,c3-interleaved-d2.json,c4-schedstat.json,latency-percentiles.md}` |
| CF-0 | `counterfactuals/three_arm/{raw,analysis,correctness,tools}/`、`CF0_THREE_ARM.md` |
| CF-1 | `counterfactuals/{CF1_SIGDROP.md,raw/ab-d2-*,raw/cnt-companion,raw/sysio-frozen,raw/sysio-sigdrop,raw/trace-companion,raw/correctness,raw/meta,bin}/` |
| CF-2 | `counterfactuals/{CF2_DEFEROFF.md,raw/ab-d2-*-cf2,raw/cnt-companion-cf2,raw/sysio-cf2,raw/shim-companion-cf2,raw/trace-companion-cf2,raw/meta-cf2,bin}/` |
| depth 补测 | `analysis/depth-analysis/{DEPTH_MECHANISM.md,depth_ledger_summary.json,raw,tools}/` |
| 跨层 | `traces/xlayer/{e1-armA-immediate,e2-armB-defer,e3-fsync-iowq}/` + 工具三件套 |
| C2 syscall | `traces/{c2-syscall,raw-c2}/`；`counters/C2_PROBE_OVERHEAD.md` |
| C1 调用栈 | `traces/perf-callgraph/`（run1 unpinned 保留为方法学记录） |
| 内核/调度探针 | `traces/{c3-kernel,c4-sched,raw-c4}/` |

## 10. 明确未建立 / 未运行

- 没有重跑 R1 的 64 KiB / threadpool / saturate / W-03 / placement / Holm 家族；
  R2 的适用范围是本文件 §2 的 D1/D2/D3/D16 形状。
- 没有 root `:k` PMU；没有 G7 的 CF-1 后 PMU 回落；没有 G12 段级直方图。
- depth timing 二进制未重跑 595 oracle；depth 无反事实 restore 块。
- 不覆盖真实块设备 / cold cache / O_DIRECT / 其他 block size（C6 不适用）。
