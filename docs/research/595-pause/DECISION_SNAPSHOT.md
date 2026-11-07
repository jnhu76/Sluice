# DECISION_SNAPSHOT — #595 暂停时点的有效结论（2026-11-07）

来源：各轮冻结文档（分支内，见 `ARCHIVE_SNAPSHOT` 引用路径），不是会话
记忆。区分：支持的结果 / 未识别的原因 / 已撤回的表述 / 未批准的干预。
历史 NEXT_EXPERIMENT 的建议**不构成**恢复后的自动授权。

## 0. 产品状态（未变）

- 产品默认 **A0 / IMMEDIATE**（master 271b81bf，无研究代码合入）。
- 没有任何已批准的感知算法产品候选。#604（真实应用准入）未推进：合成
  benchmark 与真实应用准入不是同一件事，查询状态见 flow-audit
  `DECISION.md`（`HOLD_NO_REAL_APPLICATION`）。

## 1. 支持的结果（含口径限定）

- **R1（E5 submission，已并入 master，3492dd3d）**：W02-uring-4k 形状上
  KEEP_IMMEDIATE 判定；A/A 校准、变异体回执、机制台账在
  `bench/research/uring-submit-policy/round-e5-20261104/`。
- **R2 causal-r2（PR #625，0a22a791）**：A·IMMEDIATE vs B·BOUNDED_PROGRESS
  在 D2 的 CPU（+3.70%/op）与延迟抬升由 defer 决策链承担（源级
  ESTABLISHED：CF-1 信号通知成本、CF-2 deferral 时序；CF-0 三臂锚定）；
  深度机制（D3/D16 改善分解）PARTIAL。原型三变体代码在本检查点
  `worktree-state/causal-r2_*`。
- **R3（PR #626/#627，772a4329/0ac2b582）**：completion 轮语义 oracle
  GREEN；continuation 轮正式裁决 `VERDICT_CONTINUATION.md`：
  MAIN_GATE_W03_D16_B_A0 = **FAIL**（p99 +37.85pp 上界 +39.94 > +5 门；
  CPU −14.13 / 吞吐 +16.52 达标）；MECH_9_3 门 FAIL_THIS_ROUND（enter
  −43.75% < 50%）；Q1_W02_D2 新 B = NONINFERIOR_PASS_WITH_POWER_NOTE
  （AA 面 p99 半宽 3.431>3 → 该面 NOT_ESTABLISHED）；旧 B_R2 同判据
  FAIL。机制性能裁决 FAIL_MAIN_GATE（不再是 INCONCLUSIVE）。
- **R4 state-aware（PR #629，ec8c8fea）**：R/S/Q8 对 A0——R 主门 p99
  +21.85% FAIL（CPU −19.6%/吞吐 +24.5% PASS）；S/SNP 更差；保护格
  d4 改善、d2serial/W02 FAIL。实现 patch `evidence/impl-f3b25329.patch`
  （原型 f3b25329）。trajectory/资源门按 RESULTS.md §16–§17 终审。
- **R5 phase-A（PR #630，e265aa7c）**：G1–G5 执行图、C1–C7 代码裁决
  （C1/C2 修复=cleanup 分支 c55165a2+b191bb89，18 场景 oracle 逐字节
  等价 PROVEN）、文献机制矩阵（11 行）、候选 A/B/C 预设计。
- **R5 repair round（PR #630 内 595-r5-repair-round）**：诊断仪器 v2
  修复+TSan 验证（含阴性对照），`TRACE_READY`；正式尾归因
  **NOT_ESTABLISHED**（on/off 扰动 −4.9/+9.2pp 超 ±3pp 窗口）。三臂
  正式门：C vs A0 **FAIL**（p99 +17.97% 上界 +20.34 > +5，Holm p=1.0）；
  过程 rusage CPU −22.09% [−22.75,−21.41]、吞吐 +28.49%、p95 +1.15%
  PASS；W03-d4 保护格全 PASS；d2serial/W02-d1/W02-d2 FAIL（浅队列税，
  cleanup 不变性）。C1/C2 保留依据=源级等价证明，不是已证明的普适
  性能收益。
- **flow audit（PR #631，6d29acdd）**：结构事实（研究层额外入口工作：
  seam 三重校验、整批临界区、DecisionRecord）；净效应复算确认 R5 数字；
  75 项离线校验与 F5 闭合（`OFFLINE-RUNS.md`）；CPU 口径三窗口分离
  （`cpu_scope_recalc.py`）。

## 2. 未识别的原因（UNRESOLVED / NOT_ESTABLISHED）

- **io-wq 内核 CPU 是否计入**：所有 "CPU" 数字为 process-rusage 口径；
  io-wq ticks 单独采样（R/C ≤ A0）但不在 rusage 内——−22% 不能称全系统
  收益。
- **d16 p99 差的研究层 vs 产品共享路径贡献**：入口工作贡献未由单因素
  反事实识别（flow-audit DECISION 归因限定）。
- **内核侧完成瞬时（SQE 消费/CQE 写入）**：构造上不可观测；内核完成
  尾未被排除。
- **F5 量子-8 阶跃**：PERTURBED 线索级（0-7 位 ≈1.2% vs 8-15 位 ≈9.0%
  条件尾风险），**不是因果归因**；分位数不可相加。
- **诊断二进制与干净二进德的 p99 偏移、SMT/IRQ 放置**：未归因。
- **PMU 级指令归因**：perf_event_paranoid=2，UNAVAILABLE。

## 3. 已撤回/修正的表述（不得复写）

- "校准后已证明无害/真实获益" —— 已撤回（review R3）：A0c−A0o 只证明
  同路径二进制/运行敏感，不是可转移校正；cleanup 的 d4/d2serial 原始
  节省超出校准点但未与二进制布局混杂正式分离。
- F5 首版 "真实窗口内位置" 解释 —— 已撤回（caller_window 是组构建时
  占用，非成员位置）；真位置离线恢复见 corrected `f5-position.json`。
- R5 首版保护格判定错误（用了主门阈值+双侧界）—— 已修正：d2serial
  CPU 项 PASS，各格裁决不变。
- "≥50%-of-gap" p99 归因规则 —— 已废除（分位数不可相加）。
- R3 早期 "未测" 报告 —— 以 continuation 正式裁决为准（§1 R3 条目）。

## 4. 未批准的干预（恢复后不自动授权）

- **候选 B**：NOT_RELEASED（T0 起点/预戳等待缺陷未修复）。
- **C-A（transport 层 Q=8 延迟 flush）**：HOLD_DESIGN_CONFOUNDED——
  C-A vs A0 可测 flush 策略净效应；但 C-A vs R_cleanup 归因混杂
  （多条件同变）；相对 #625 的增量假设尚未成文。
- **观察解耦（C-variant2）**：暂缓。
- 当前 **NO_COUNTERFACTUAL_SELECTED**（flow-audit NEXT_EXPERIMENT §7）：
  研究路径剩余步骤（seam precheck、DecisionRecord/计数器、整批 dispatch
  临界区）均无语义等价证据，不得为交付候选而删除。
- 任何新性能矩阵前置：明确的增量假设/单因素可删除干预 + 语义等价证明
  + 固定公平口径 + 预注册 + fresh A/A（继承项见 flow-audit
  NEXT_EXPERIMENT §8）。
