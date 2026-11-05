#595 R3 完成轮 · 论文式结果分析（可粘贴）

**分析口径**：全部统计从 `raw/` 逐 rep 记录独立重算（与测量时 summary.json 逐位一致，
0 diffs），遵循冻结预注册（sha256 `1a9c1e12…`，计时前冻结）：24 对 AB/BA、配对相对差、
bootstrap 10k seed 5950、百分位 95% CI、单侧界、Holm。未测单元一律 NOT_RUN，不以零显示。

**实测结论（gate-grade）**

| 块 | 对比 | CPU/op | 吞吐 | p95 | p99 | 判定 |
|---|---|---|---|---|---|---|
| AA-W03-D16（R3 A0/A0） | 半宽 | 0.797 pp | 0.821 pp | — | 2.039 pp | PASS |
| AA-W02-D2（R2 A/A） | 半宽 | 0.862 pp | 0.853 pp | — | 1.420 pp | PASS |
| LEGACY-W02-D2（B_R2−A_R2，同 SHA） | 效应 | **+3.43%** [2.67,4.16] | **−3.30%**（单侧 −3.89%） | **+5.23%**（+5.91%） | **+7.05%**（+9.07%） | **FAIL** |
| PROT-W02-D3（B_R2−A_R2） | 效应 | −7.33% | +7.98% | −12.80% | −11.51% | PASS |
| PROT-W02-D16（B_R2−A_R2） | 效应 | −28.11% | +39.24% | −32.59% | −32.40% | PASS |

→ 旧 B 的 D2 退化已复现并被证实（吞吐/p95/p99 越界；CPU 项在界内），且是形状特异的
（D3/D16 旧 B 为改善）。**新 B 对该退化的消除未测**（R3 树无 W-02 研究臂）。

**机制诊断窗（带仪器，禁入门判定）**：W-03 d16 纯提交 enter B 29,225 vs A0/A1 60,000
（−51.29%，过 ≥50% 存在性门）；批量为真实 span>1 提交（A1 同 adapter 逐条 =
enter 与 A0 完全相同）；cap/suffix 对账闭合（1,355×8+9,160 单=20,000；untouched==
suffix；flush==entry 无 defer）；两条就绪正控 B 3×to_submit=2；单条补发负控三臂
逐项相同。PMU cycles −14.1%、进程 CPU −18.1%——方向有利但单窗口带仪器，不作结论。
与 census 预测（91.5–93.3%）的差距：单条 enter 是拆批后提交尺寸（`single==suffix==
9,160` → span=1 入口全部为 cap 拆批成员，调用者呈现 ready≥2），主因 cap=8 拆批
（16→8+逐条 suffix；已登记，口径更正见 analysis/CORRECTIONS.md）。

**裁决**：机制性能 INCONCLUSIVE（W-03 d16 主门一次未测）；产品
HOLD_NO_REAL_APPLICATION（#604 无 READY_FOR_595）。阻塞链：变异体三态实际已通过
（BASE GREEN→3 RED→逐位相同重建 GREEN）但缺 §14.3 独立 verifier 回执 →
semanticsReady 未翻转 → 全部 R3 B/A1 门 BLOCKED。

**下一步（唯一先决项）**：补 §14.3 回执翻转 semanticsReady，不改任何门槛，重开
W-03 d16 24-triplet 主测量；随后为新 B 在 D2 增加 R3 臂（Q1）。

分析产物：`r3-completion-20261105T083818Z/analysis/{RESULTS.md,DATA_INVENTORY.md,
MISSING.md,metrics.csv,effects.csv,gates.csv,NUMERIC_QA.md,VISUAL_QA.md}` 与
`figures/F2–F5`（SVG+PNG+figure-data+图注）；重跑入口 `analysis/rebuild_all.sh`；
数值 QA 50/50、视觉 QA 4/4 PASS。
