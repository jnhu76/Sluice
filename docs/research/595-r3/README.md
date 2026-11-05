# #595 R3 研究记录归档（完成轮 + 续跑轮）

研究记录（research record），不是产品变更。本目录按 #595 R3 各轮原样冻结证据归档，
供评审与追溯；不构成任何契约、路线图或合并授权。完成轮归档 2026-11-05，续跑轮
（翻转 + 主门实测 + 修正）归档 2026-11-05（`continuation/`）。

## 状态标签（以续跑轮为最新）

- **执行状态**：完成轮 `INCOMPLETE_BLOCKED` → 续跑轮 `COMPLETE_MEASURED`
  （§14.3 独立 verifier 回执出具并翻转 `semanticsReady=true` 后，全部计划块
  24/24 有效完成）。
- **机制性能**：`FAIL_MAIN_GATE` —— W-03 d16 主门实测：CPU −14.13pp（上界
  −13.68）与吞吐 +16.52pp 达标，但**统一完整请求口径**下 p95 +25.75（上界
  +27.95）/ p99 +37.85（上界 +39.94）越 +5% 门；本轮 §9.3 kernel 基准 enter
  减少 43.75% 亦未达 50% 存在性门。
- **产品**：`HOLD_NO_REAL_APPLICATION`（#604 无 READY_FOR_595 handoff；未变）。

## 三问状态（续跑轮更新）

| 问题 | 回答 |
|---|---|
| W-02 D2 新算法能否守住 IMMEDIATE | **守住**（新 B−A0：CPU +2.17pp 上界 +2.72、吞吐 −2.11%、p95 +1.75、p99 +1.49，全部界内 + Holm；p99 附面功效注记：AA p99 半宽 3.431pp>3pp。旧 B_R2 同判据 FAIL） |
| W-03 d16 完整成本下 B 降 CPU 且守尾延迟 | **否**（CPU/吞吐改善成立且功效充足；完整请求尾延迟不守；enter −43.75%<50%） |
| 旧 B 同 SHA 在 D2 是否仍差、新算法是否消除 | 前半已证（完成轮）；后半已证（上行"守住"） |

## 目录

```
595-r3/
├── README.md                  本文件（归档说明与验证入口）
├── ARCHIVE_MANIFEST.sha256    仓库相对路径清单（in-repo 校验用；含 continuation/）
├── completion/                R3 完成轮证据根（与 E5 原始目录逐字节一致，diff -r 验证；
│                              3 个分析文件 + CORRECTIONS.md 为续跑轮更正，见下）
│   ├── evidence/              结论性文档、预注册、patch、测量块副本、VERDICT/RESULTS
│   ├── raw/                   原始输出（校准/保护/机制/变异体，含 per-invocation JSONL）
│   ├── analysis/              论文式分析 + CORRECTIONS.md（2026-11-05 所有者四点修正落实）
│   ├── figures/               F2–F5（SVG+PNG+figure-data+图注；F3 图注已同步更正）
│   └── harness/               本轮测量/分析 harness（measure/analyze/bucket/diag）
├── continuation/              R3 续跑轮证据根（与 E5 r3-flip-20261105T1428Z 逐字节一致）
│   ├── evidence/              PREREG_ADDENDUM（计时前冻结）、FLIP_RECEIPT_14_3、
│   │                          A0_AND_SUFFIX_RECEIPT、correctness/、delta patch、
│   │                          VERDICT_CONTINUATION、FLIP_MANIFEST
│   ├── raw/                   AA×2 / TRIPLET-W03-D16 / AB-W02R3-D2（全部 24/24 有效）
│   │                          + mechanism-9_3（strace 3×/臂）
│   ├── analysis/              RESULTS_CONTINUATION（两行结果与判定）
│   └── harness/               measure_triplets / analyze_triplets / 会话与诊断脚本
└── prior/                     前一轮 R3 基线轮 evidence（CONTEXT/BUILD/PROTOCOL/清单）
```

注意：`completion/evidence/MANIFEST.sha256`（2451 项）与 `prior/MANIFEST.sha256`
（246 项）记录的是 **E5 主机绝对路径**（会话时点的完整性凭证）；在仓库内校验请用
本目录的 `ARCHIVE_MANIFEST.sha256`（相对路径）。prior 轮的 raw/harness 未归档
（保留在 E5 主机），其清单在仓内只能部分通过，属预期。

## 完成轮分析文件更正（续跑轮 PR 内提交）

按所有者 2026-11-05 四点修正，以下完成轮**分析文本**在本归档内更正（原始数据、
统计输出、QA 回执零改动；登记于 `completion/analysis/CORRECTIONS.md`）：
`completion/analysis/RESULTS.md` §4、`completion/figures/F3-caption.md` 主要发现段、
`completion/analysis/issue-comment-paper-analysis.md` 机制段。核心更正：enter 直方图
是拆批后提交尺寸（`single_calls==suffix_single_calls==9,160` → 调用者呈现
ready≥2），不能反推 ready 组小；"30% 物理上限 / cycles-per-enter"类表述核查仅存在
于会话对话，已撤回并禁止。

## 验证

```bash
cd docs/research/595-r3
sha256sum -c ARCHIVE_MANIFEST.sha256        # 应全 OK（含 continuation/）
cd completion && sha256sum -c analysis/MANIFEST_PAPER_ANALYSIS.sha256  # 38 项 OK
# 与分析结论一致性：
#   completion/analysis/NUMERIC_QA.md（qa_gate.py 50/50）
#   completion/analysis/VISUAL_QA.md（4/4 PASS）
#   continuation/evidence/VERDICT_CONTINUATION.md（机器可读裁决）
# 重建统计与图（只读 raw/）：
#   bash completion/analysis/rebuild_all.sh  # 在含 Python3+matplotlib 的环境
```

## 评审入口（按序）

1. `continuation/evidence/VERDICT_CONTINUATION.md` 与
   `continuation/analysis/RESULTS_CONTINUATION.md`（最新两行结果与主门判定）
2. `continuation/evidence/FLIP_RECEIPT_14_3.md`（§14.3 翻转：7 项内容、
   semanticsReady=true、适用范围）与 `continuation/evidence/PREREG_ADDENDUM_CONTINUATION.md`
   （统一延迟口径 + W02 新臂的计时前冻结）
3. `continuation/raw/TRIPLET-W03-D16/summary.json`（24-triplet 主门 bootstrap，
   B−A0/B−A1/A1−A0）与 `continuation/raw/mechanism-9_3/enter_summary.json`
4. `completion/evidence/VERDICT.md` 与 `completion/analysis/RESULTS.md`（完成轮结论）
5. `completion/evidence/PREREGISTERED_R3_COMPLETION.md` §9/§14（门槛与翻转条件）
6. `completion/analysis/CORRECTIONS.md`（四点修正落实记录）
7. `completion/evidence/PROTOCOL.md` §5（16 项登记偏差 D-1..D-16）
8. `completion/analysis/DATA_INVENTORY.md` / `MISSING.md`（数据准入与缺失）
9. `completion/figures/`（F2–F5 及图注）、`completion/evidence/BUILD_COMPLETION.md` §11

产品代码零改动：R3 原型以 `completion/evidence/r3-completion.patch`
（sha256 `2596f204…`，reverse-check 通过）+ `continuation/evidence/delta_w03/w02.patch`
（bench-only；lib 与全部正确性二进制与冻结逐位相同）作为工件归档，
均未应用于任何分支。
