# F2 — W-02 D2 退化是否消除：旧策略复现与两条就绪正控

**图**：`F2_d2_regression.svg` / `.png`；数据：`figure-data/F2a_d2_single_refill_effects.csv`、
`figure-data/F2b_d2_two_ready_positive_control.csv`。

**任务与平台**：W-02 流水线单条补发形状（case `W02-uring-4k-d2`，4 KiB 读、20,000
valid reads/rep、warm tmpfs、`taskset -c 2`，E5 原生，2026-11-05）。**基线/臂**：
(a) 为 R2 树（271b81bf+R2 patch 7be9d014，二进制 sha `32beabcd…`，与本 session 配对块
同 SHA）的 A_R2（IMMEDIATE）vs B_R2（旧 BOUNDED_PROGRESS defer 策略）；24 对
AB/BA 交替、每 invocation WARMUP=1 MEASURED=5。**样本数** n=24 对（48/48 invocation
有效）。**误差条**：bootstrap 10,000 次（seed 5950）对 24 个配对相对差的百分位 95% CI；
**方向**：CPU 与延迟 negative = 改善（更低），吞吐 positive = 改善（更高）；虚线为预注册
门槛（CPU/延迟 +5%、吞吐 −3%）。**单位**：%。**(b)** 为机制诊断窗（带仪器、
1 invocation×3 phase）的纯提交 `io_uring_enter` 计数：A0/A1 各 6 次×to_submit=1，
B 为 3 次×to_submit=2（两条同时就绪确实合发）。

**主要发现**：(a) 旧 B_R2 在 D2 **已证实恶化**：吞吐 −3.30% [−3.98, −2.57]（单侧
−3.89% 越过 −3% 界）、p95 +5.23% [4.43, 6.03]（单侧 +5.91% > 5%）、p99 +7.05%
[4.84, 9.51]（单侧 +9.07% > 5%）；CPU +3.43% [2.67, 4.16]（单侧 +4.06% 在 5% 界内）。
**新 B（R3 ready-aware）在 D2 未测**（R3 树无 W-02 研究臂，NOT RUN），因此
"新算法消除 D2 退化"尚未被检验；(b) 的两条就绪收益不能外推到 D2 稳态单条补发。
