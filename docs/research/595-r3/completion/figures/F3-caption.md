# F3 — 批量机制与实际工作（机制诊断窗，非门证据）

**图**：`F3_batch_mechanism.svg` / `.png`；数据：`figure-data/F3a…F3d*.csv`（4 个）。

**任务与平台**：W-03 外部循环主形状 d16（case `W03-uring-4k-d16`：64 MiB warm
tmpfs、4 KiB positional reads、queue_depth=16、20,000 ops/phase、3 phase/run），
R3 树（0a22a791+completion patch，二进制 sha `0ebfb7f0…`），臂经
`SLUICE_595_R3_ARM`（A0=原始逐条、A1=同 adapter 逐条、B=就绪感知批量）。
**全部四个面板均为带仪器诊断窗**（bpftrace 21 probes 0 lost / perf root u+k
time-running=100% / 20 ms 逐 tid 分桶），单窗口，**禁入预注册门槛**。

**(a)** 纯提交 `io_uring_enter` 计数：A0=60,000、A1=60,000、**B=29,225（−51.29%**，
超过 50% 机制存在性门限，虚线 30,000）；单位为次数/运行（3×20,000 ops）。
**(b)** B 的每-enter 提交条数分布（按 **enter 次数加权**的直方图；y 对数轴，理由：
计数跨 4 个数量级 {1:24,826, 2:1, 3:2, 6:1, 8:4,395}；A0=A1 全部为 1）。
**(c)** B 的 cap/suffix 对账（timed phase）：20,000 ops = 1,355 组×前缀 8 + 9,160
单成员/suffix；enters 10,515 = 1,355 组调用 + 9,160 单条；`flush==entry`（无 defer）。
**(d)** PMU cycles 与 instructions（单位：百万次）：B cycles 733.9M vs A0 854.1M
（−14.1%）、A1 852.4M（−0.2%）；instructions A1 +5.0%（adapter 簿记可见于指令数）。

**主要发现**：减少的 enter 来自**真实的 span>1 成批提交**而非工作转移——A1 走与 B
完全相同的 adapter 准备路径但 enter 数与 A0 完全相同（60,000）。理论潜力（census 预测
91.5–93.3%）未全部实现：单条 enter（24,826）是**拆批后**的提交尺寸——计时相位
`single_calls == suffix_single_calls == 9,160` 说明 span=1 入口全部为 cap 拆批成员
（调用者每次呈现 ready ≥2，不能由单条 enter 反推组小，2026-11-05 所有者修正）；主因
是 cap=8 把 16 深窗口拆为前缀 8 + 逐条 suffix（设计如此，suffix 不二次成组）——与
census 的 d16_full 整窗假设不符（已登记偏差；口径更正见 `analysis/CORRECTIONS.md`）。
