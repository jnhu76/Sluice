# #595 Flow Audit — 完整 I/O 执行流程、反馈回路与最小机器工作审计

Round `flow-audit`, 2026-11-07, native E5. 隔离 worktree
`/home/jnhu/sluice-595-e5/flow-audit/worktree`（分支
`research-595-flow-audit`）。审计阶段全离线、无计时运行；归档提交与
历轮复核修订均经 round owner 分别授权。审阅锚点 `9d1bf648`（复核一
R1–R6）、`461cb267`（复核二 R7–R10 窄修，见 CORRECTIONS.md）。核心
问题：完整执行流程的问题是否被过早压缩为"感知调度"问题；应先改
调用者/adapter/提交/完成处理/结果观察哪一环。

## 文件

- `BASELINE.md` — 三对象（PRODUCT/RESEARCH/BENCH）→ SHA/patch →
  二进制 → 数据集 → 适用结论；三窗口 CPU 口径（R4 修订）。
- `FLOW_AUDIT.md` — 主要结论、六组 Mermaid 块（对照流程 / W02 vs W03 /
  Request 生命周期主图+合法分支 / refill 顺序 / 反馈回路）、R3 信息
  来源表、源码对应表、三对象差异。
- `WORK_AND_WAIT_LEDGER.csv` — 机器工作账本（21 项）+ 等待账本
  （11 项）；由 `scripts/ledger_csv.py build` 确定性生成并校验。
- `HYPOTHESES.md` — H1-H5 + H6/H7 增补的竞争解释表（R2 修订：撤回
  CPU→耗时换算、H3 反证、8×1 vs 2×8 示例）。
- `DECISION.md` — ≤500 字路线裁决（R1/R2 修订版）。
- `NEXT_EXPERIMENT.md` — C-A 状态 = **HOLD_DESIGN_CONFOUNDED**（令牌
  语义精确化：可测净效应，混杂在归因对照）；对照 = PR #625 causal-r2
  B·BOUNDED_PROGRESS（复核二 R8）；协议缺陷清单、NO_COUNTERFACTUAL_SELECTED。
- `CORRECTIONS.md` — R1–R6 逐项处置 + 复核二 R7–R10 窄修
  （原主张/核验/改动/验证/边界）。
- `scripts/cpu_scope_recalc.py` — CPU 三窗口复算（measured 主口径 /
  wait4 整次子进程 rusage 秒 / invocation ticks 采样归类下界；
  io-wq 归属各处统一 UNRESOLVED；支持 --root；缺字段显式失败）。
- `scripts/verify_inherited_evidence.py` — 准入驱动核验：5 格 × 3 臂 ×
  5 指标复算与冻结分析值**原始浮点精度一致**（75/75 实测计数）；F5
  完整性（精确位置集/整数尾数/480,000 对账 + 桶分母-恢复总量按冻结
  恢复脚本规则闭合）+ 冻结恢复脚本离线重跑逐位复现；CIs/Holm/门限
  不复算。
- `scripts/ledger_csv.py` — 账本确定性生成+校验（13 列对齐、受控词表、
  非零退出反例）。
- `scripts/test_offline_checks.py` — 确定性测试 37/37：准入负例（重复/
  失败尝试/缺块/ops 不符/空输入）、F5 负例（双位置/缺位/外来桶/总数
  不闭合/尾百分比不一致/桶分母不闭合/sub-MIN_N/无组 tag）、冻结比较
  负例（空 round-analysis.json/缺项）、CPU 合成窗口、CSV 截短/多列、
  mermaid 结构。

## 一句话结论（R2 修订）

研究路径确有额外入口/锁/记账工作（结构已证）；其对 singleton 税与
d16 p99 差（运行级均值差 C−A0 9.52µs / R−A0 10.71µs）的贡献**尚未由
单因素反事实独立识别**——现有证据既不能证明产品整体架构有问题，也
不能排除产品共享路径、完成服务或调用者反馈的作用。净效应已知：
R5 指定形状上过程 CPU −22%/吞吐 +28%/p99 +18% FAIL/浅队列 FAIL
（过程 CPU 口径，非全系统）。感知输入多数有产品权威来源（R3 表），
但策略接入产品的净收益未证明。**下一步未定**：在找到语义等价、
可识别的单一干预前不启动新性能矩阵；默认 A0 不变；C-A =
HOLD_DESIGN_CONFOUNDED；候选 B 不放行。

## 证据引用

历史 raw/分析全部按路径+SHA 引用（见 BASELINE.md），未复制归档；
无新计时数据，故无新性能图；F5 相关引用均标 PERTURBED（instrument-on
R 相，24 tag × 20000 = 480,000 行，对账见 verify 脚本）。
