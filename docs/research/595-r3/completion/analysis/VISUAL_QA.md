# VISUAL_QA — 逐图渲染与视觉校验回执

方法：`analysis/scripts/build_figures.py` 以 220 dpi 渲染 PNG（SVG 为矢量交付版），
每版经外部视觉模型按固定布局 QA 提示词审查（文字裁切/重叠、图例遮挡、误差条越界、
单位与面板标签），发现缺陷→修改脚本→重渲染→复审，直到该图 PASS。
数值一致性由 `analysis/scripts/qa_gate.py` 独立核验（50/50 OK，见 NUMERIC_QA.md）。

| 图 | 版本 | 发现的缺陷 | 修复 | 终审 |
|---|---|---|---|---|
| F2_d2_regression | v1 | 无 | — | PASS |
| F3_batch_mechanism | v1（1×4 布局） | (a) 柱 B 顶部两个标注互相重叠；"50% 门限"文字压柱；(c)(d) 标题在窄面板截断、(c) 标注与图例重叠、(d) 图例压柱基 | 改 2×2 布局；合并柱 B 标注为一个；门限文字改为图例项；(d) 加 y 余量并把图例移右上 | v3 PASS |
| F4_tail_guardrails | v1 | 图例压 D16 p50 数据点（左下）；"+5%" 标注压 R2-D2 p99 点（右上）；−5% 线未标注 | 图例移到轴下方；界标注移左缘；补 −5% 参考线标注；固定 ylim | v2 PASS |
| F5_observed_regimes | v1 | "REGRESSION" 单词超出格宽互相串行；p99 表头与末列表头相触；标题右缘截断；placement 行标签挤压首格 | 格内改短词（PASS/REGR/INCON/N/R/DIAG）+图例全称映射；末列表头缩短 "mech / res"；标题两行；行标签缩短 | v2 PASS |

复查记录（渲染哈希以 analysis/MANIFEST_PAPER_ANALYSIS.sha256 为准）：
- F2 v1（5fadb821…）："no defects"。
- F3 v3（最终版）："Verdict: PASS — no overlapping legends, no clipped or cut-off text in any panel."
- F4 v2（最终版）："PASS … No defects found."
- F5 v2（最终版）："PASS … No defects found."

F1（净收益与 adapter 成本）无 gate-grade 数据，按预注册登记 NOT_RUN，不绘制占位图，
不以诊断窗冒充（见 MISSING.md A 节）。
