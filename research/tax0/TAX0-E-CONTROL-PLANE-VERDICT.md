# TAX-0E — Control-plane verdict + first optimization gate

Issues: #250 · #259 · PR #260（research/evidence）
上游证据：[TAX0-B](TAX0-B-SEMANTIC-FLOOR-LADDER.md) ·
[TAX0-C](TAX0-C-CONTROL-PLANE-PROFILE.md) ·
[TAX0-D](TAX0-D-CAUSAL-ABLATIONS.md) ·
[measurements JSON](tax0-control-plane-measurements.json) · sessions under
`results/`

# VERDICT

**MIXED（per-regime）— capability cost ≈ ZERO（+37 instr/op），L1 abstraction
tax = FIXED ≈ 2015 instr/op（small-op 主导），continuation premium ≈ +800
instr/op（其中 hand-written floor 可证 ~+210）。confidence: MEDIUM-HIGH
（ENVIRONMENT-LIMITED：WSL2 虚拟化，instructions/op 为主指标）。**

# Tax stack（read，instructions/op，canonical formal4）

```text
4K d1    Z1 1154  → Z1b 1201 (+47 capability) → Z2 3827 (+2626 L1) → Z3 5796 (+1969 continuation)
4K d32   Z1 1043  → Z1b 1080 (+37)             → Z2 3112 (+2032)      → Z3 3899 (+787)
4K d64   Z1 1043  → Z1b 1080 (+37)             → Z2 3212 (+2132)      → Z3 4105 (+893)
64K d8   Z1 14499 → Z1b 14537 (+38)            → Z2 16552 (+2015)     → Z3 17380 (+828)
1M d8    Z1 229553→ Z1b 229590 (+37)           → Z2 231605 (+2015)    → Z3 232468 (+863)
```

结构结论：**L1 abstraction tax 是每-op 固定 ~2015 条指令**（与 request
size 无关）——正是 #259 "Zero-Cost Control Plane" 关心的控制面工作；
它在 small-op regime 占 ~65% 的 z2 用户指令，在 64K/1M 相对退到 12%/<1%。

# First optimization gate

```text
Rank（measured magnitude × causal confidence × semantic risk × size × breadth）:
1. F01  PROVEN −75 instr/op，单行级修改，语义零风险，覆盖所有 submit
2. F05 remainder（+577@d32）  大但多机制、缺逐组件消融，结构性改动
3. F03（mutex 热存在）        因果消融=ADR 级结构变更
4. F02  NEGLIGIBLE（−4..+23 跨零）
```

**AUTHORIZED FIRST OPTIMIZATION: F01** — `if (stats_)` 门控
`update_max_outstanding(stats_, backend_->outstanding())` 的实参求值。
SEMANTICS CHANGED: NO（stats 启用路径逐位相同；禁用路径无可观测 stats
对象；admission/completion 语义不动）。生产 PR：
`perf/tax0-f01-stats-gate`（independent Draft PR，链接本证据；
harness 不复制进生产 PR）。winning regimes：全部 Z2/Z3 小-op cells
（−2.3~−2.5% 用户指令）；neutral：64K/1M（绝对 −75 不变，相对 <0.1%）；
possible losing regimes：无（stats 启用路径不变）。
预期效果边界：MEDIUM-HIGH confidence 的 **CPU/control-plane improvement**
（wall 中性——kernel medium 主导，见 TAX0-B §5）。

# FINAL ANSWER（campaign §50 1–8）

1. **Capability Cost（Z1b−Z1）**：≈ **+37 instructions/op**，跨 size/depth
   恒定（4K d1 为 +47）。安全语义的显式用户态机制几乎免费。
2. **Abstraction Tax（Z2−Z1b）**：**固定 ≈ 2015 instructions/op**，与
   request size 无关（Z3−Z2 另有 ≈ +800/op 的 continuation 层）。
3. **Accidental（候选 incidental）**：F01 已 PROVEN（75/op，本轮修复授权）；
   F05 剩余 ~577/op（waiter/routing/scheduler 机器）是最大候选但缺逐组件
   消融；F03 mutex 结构热存在、因果未测；F02 排除（NEGLIGIBLE）。
4. **Required by semantics**：+37/op 的 floor 机制（bounded capacity/
   identity/stale/exactly-once/lifetime/accounting）与 Z3 对照 cell 所购买的
   continuation 语义本身（z1bw 已证明 ≥ 摊销 +210/op 可满足）；Z3 额外的
   cancel-race/cross-worker/TLA+ 路由能力在对照 cell 未行使——
   required-if-used，本轮未计价。
5. **Does unused capability disappear today?** NO——F01 即"未用能力仍然
   付费"的直接实例（stats 关闭仍付 arena 锁）；F02 形式存在但实测可忽略。
6. **First mechanism to optimize**：**F01**（唯一 PROVEN + material + 零风险
   + 广谱）。
7. **After F01, what remains**：z2 每-op 仍剩 ~1940 instr/op 的固定 L1
   控制面工作（准入阶梯/锁往返/publication/router/ledger——F03/F04 域）
   与 z3 的 ~+800/op continuation 层（F05 剩余）；两者都需要结构级
   消融/ADR，本轮证据已把它们列为下一梯队的候选与所需实验。
8. **CONTROL PLANE 是否足够 understood 以进入 #259 Buffer Boundary？**
   是（带边界）：控制面的"谁在付费"已按 regime 归因完毕，剩余问题
   （F03/F05 结构消融）属于优化路线而非认知空白；数据面（buffer
   boundary）可以开工。ENVIRONMENT-LIMITED 注记：全部结论为 WSL2
   虚拟化 pilot 量级，native 主机复测是任何 production 化扩展的前置。

# Negative results（一等结果，不得删）

- **F02 = NEGLIGIBLE**：进程级 seq_cst RMW 在单驱动/单任务 regime 实测
  −4..+23 instr/op 跨零；"sexy story" 被测量否决。
- **Historical cliff NOT REPRODUCED**：4K d≥32 w4 在 uring Z-ladder 上
  无阈值突变（+0.6% instr @ d32）。
- **Capability cost ≈ zero**：语义清单的完整显式实现只值 +37 instr/op——
  "safety is expensive" 的叙事不成立。
- **wall ≠ instructions**：4K d≥8 时 z2 wall 可优于 z1/z1b——控制面 CPU
  改进不自动等于吞吐改进（本介质下）。

# UNKNOWN / BLOCKED

- z3w4 write 的 instr/op（OBS-1 不稳定，NOT RUN — environment limitation；
  wall/user/sys + same-work 已测）；OBS-1 根因（canceled(EAGAIN) 终态 +
  teardown abort）待 #250 后续票；
- F02 在多 worker 争用下的行为；F03 因果量级；F05 逐组件分解；
- native Linux/NVMe 复测（本环境不可达）。
