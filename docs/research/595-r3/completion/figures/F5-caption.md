# F5 — 实测适用范围裁决矩阵（无插值）

**图**：`F5_observed_regimes.svg` / `.png`；数据：`figure-data/F5_observed_regimes_matrix.csv`。

**口径**：每格按**全部相关预注册保护门**裁决（CPU、吞吐、p95、p99 四列的单侧界 +
Holm；机制/资源列按机制存在性与资源门），不凭 CPU 单项着色。格值：PASS=预注册界
通过；REGR=越界（已证实恶化）；INCON=仅诊断证据（统计功效不足/无正式门）；
N/R=未运行（被阻塞或无该臂）；DIAG=仅带仪器诊断窗证据（非门）。
**禁止项已遵守**：未对未测试区域插值；D3/D16 为旧 B_R2 的结果，不外推为
"D≥3 全部有效"，更不代表 R3 B；真实应用（#604）单独裁决，不与合成结果合并。

**主要发现**：实测得到门级结论的只有 R2 同 SHA 复现行——旧 B_R2 在 D2 为
REGR（吞吐/p95/p99），在 D3/D16 为 PASS（且为改善）。R3 新 B 的全部性能格
（含主形状 W-03 d16 主门）为 N/R：预注册冻结的 semanticsReady=false 未翻转
（缺 §14.3 独立 verifier 回执），一次未测。机制存在性（enter ≥50% 减少）与
D2 正/负控在诊断口径下成立（DIAG）。资源预算仅诊断证据（RSS +48 KB、+6 次一次性
malloc、0 新线程/timer/队列；valgrind 斜率子门不可测）→ INCON。
产品（#604）：HOLD_NO_REAL_APPLICATION（独立裁决，不在本矩阵内合并）。
