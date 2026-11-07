# #595 R3 研究记录归档（完成轮 + 论文式分析）

研究记录（research record），不是产品变更。本目录按 #595 R3 完成轮的原样冻结证据
归档，供评审与追溯；不构成任何契约、路线图或合并授权。归档时间：2026-11-05。

## 状态标签

- **执行状态**：`INCOMPLETE_BLOCKED` —— 预注册（`completion/evidence/PREREGISTERED_R3_COMPLETION.md`，
  sha256 `1a9c1e12…`，计时前冻结）的 `semanticsReady=false` 全程未翻转（缺 §14.3
  独立 verifier 回执），全部 R3 B/A1 正式性能门一次未测。
- **机制性能**：`INCONCLUSIVE`（诊断窗方向有利但带仪器，禁入门判定）。
- **产品**：`HOLD_NO_REAL_APPLICATION`（#604 无 READY_FOR_595 handoff）。
- 前一轮 R3 基线轮材料见 `prior/`（其结论被本轮部分 supersede：本轮补上了
  预注册与配对测量，但 B 门仍未测）。

## 三问状态

| 问题 | 回答 |
|---|---|
| W-02 D2 新算法能否守住 IMMEDIATE | 无数据（R3 树无 W-02 研究臂，NOT_RUN） |
| W-03 d16 完整成本下 B 降 CPU 且守尾延迟 | INCONCLUSIVE（主门未测；诊断：enter −51.29%、PMU cycles −14.1%） |
| 旧 B 同 SHA 在 D2 是否仍差、新算法是否消除 | 前半已证（B_R2−A_R2：吞吐 −3.30%、p95 +5.23%、p99 +7.05%，单侧界越界）；后半无数据 |

## 目录

```
595-r3/
├── README.md                  本文件（归档说明与验证入口）
├── ARCHIVE_MANIFEST.sha256    仓库相对路径清单（in-repo 校验用）
├── completion/                R3 完成轮证据根（与 E5 原始目录逐字节一致，diff -r 验证）
│   ├── evidence/              结论性文档、预注册、patch、测量块副本、VERDICT/RESULTS
│   ├── raw/                   原始输出（校准/保护/机制/变异体，含 per-invocation JSONL）
│   ├── analysis/              论文式分析（DATA_INVENTORY/MISSING/metrics/effects/gates/
│   │                          RESULTS/NUMERIC_QA/VISUAL_QA/qa 脚本/rebuild_all.sh）
│   ├── figures/               F2–F5（SVG+PNG+figure-data+图注）
│   └── harness/               本轮测量/分析 harness（measure/analyze/bucket/diag）
└── prior/                     前一轮 R3 基线轮 evidence（CONTEXT/BUILD/PROTOCOL/清单）
```

注意：`completion/evidence/MANIFEST.sha256`（2451 项）与 `prior/MANIFEST.sha256`
（246 项）记录的是 **E5 主机绝对路径**（会话时点的完整性凭证）；在仓库内校验请用
本目录的 `ARCHIVE_MANIFEST.sha256`（相对路径）。prior 轮的 raw/harness 未归档
（保留在 E5 主机），其清单在仓内只能部分通过，属预期。

## 验证

```bash
cd docs/research/595-r3
sha256sum -c ARCHIVE_MANIFEST.sha256        # 应全 OK
# 与分析结论一致性：
#   completion/analysis/NUMERIC_QA.md（qa_gate.py 50/50）
#   completion/analysis/VISUAL_QA.md（4/4 PASS）
# 重建统计与图（只读 raw/）：
#   bash completion/analysis/rebuild_all.sh  # 在含 Python3+matplotlib 的环境
```

## 评审入口（按序）

1. `completion/evidence/VERDICT.md` 与 `completion/analysis/RESULTS.md`（结论与论证）
2. `completion/evidence/PREREGISTERED_R3_COMPLETION.md` §14（为何全部 B 门 BLOCKED）
3. `completion/evidence/PROTOCOL.md` §5（16 项登记偏差 D-1..D-16）
4. `completion/analysis/DATA_INVENTORY.md` / `MISSING.md`（数据准入与缺失）
5. `completion/figures/`（F2–F5 及图注）
6. `completion/evidence/BUILD_COMPLETION.md` §11（原型限制；公开安装头 0 行 diff）

产品代码零改动：R3 原型以 `completion/evidence/r3-completion.patch`
（sha256 `2596f204…`，reverse-check 通过）作为工件归档，未应用于任何分支。
