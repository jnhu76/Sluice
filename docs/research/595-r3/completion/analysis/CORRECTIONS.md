# CORRECTIONS — 所有者 2026-11-05 四点修正的落实记录

更正时间：2026-11-05T14:5xZ（续跑轮进行中；更正只涉及解释性文字，不触碰任何
测量数据、统计输出或门槛）。更正原则：错误推断删除或改写，并登记原因；数据与
QA 回执（NUMERIC_QA 50/50）不受影响。

## 修正 #3：不能由单条 enter 推出 ready 组很小

**原错误**（3 处，均已改写）：
- `analysis/RESULTS.md` §4 条目 1（原文"稳态窗口 ready 多为 1–2 条
  （24,826/29,225 个 enter 为单条）"）；
- `figures/F3-caption.md` 主要发现段（同一表述的图注版）；
- `analysis/issue-comment-paper-analysis.md` 机制诊断窗段（"差距来自稳态 ready
  多为 1–2 条与 cap=8 拆批"）。

**错误性质**：enter 直方图计量的是**拆批后**的每次 `io_uring_enter` 提交尺寸；
cap=8 拆批的 suffix 成员逐条提交（设计如此），故单条 enter 同时包含"调用者只呈现
1 条"与"算法拆批产物"两种来源，不能区分。

**正确表述（据既有计数器，无需新数据）**：计时相位 `single_calls ==
suffix_single_calls == 9,160` —— span=1 的 adapter 入口**全部**是 cap 拆批的
suffix 成员，即该相位调用者每次入口呈现的 ready 组 ≥2 条；进入算法前的组大小
分布**未被直接测量**（如需归因，须在 adapter 入口按呈现尺寸分桶——后续独立
候选）。census 差距的主因改记为 cap=8 拆批 + SQE/enter 粘连。

## 修正 #4：删除"物理收益上限不到 30%"与"≈3,900 cycles/enter"类表述

**核查结果**：这两条只出现在 2026-11-05 会话对话中，**未写入任何归档文档**
（`grep -rn "3,900|cycles/enter|上限不到|30%" analysis/ figures/` 无命中；
RESULTS.md §7 结论亦无）。予以**撤回**并禁止再次出现：cycles 差 ÷ 少掉的 enter
是**相关平均成本变化**（含通知/等待/锁/调度），不是 enter 的独立因果成本，
不能据此建立任何上限；诊断窗 −14~−18% 也不能证明主门余量。

## 修正 #1/#2（登记，非本文档更正对象）

- #1 尾延迟起点两臂不一致：已在续跑轮以统一"应用已 ready → 应用观察到完成"
  口径修复（`r3-flip-20261105T1428Z/evidence/delta_w03.patch`、`delta_w02.patch`；
  新 bench 二进制 SHA 见 `FLIP_RECEIPT_14_3.md`）。旧 `submitted_ns` 口径的延迟
  数字不可跨二进制比较。
- #2 W02 D2 新 B 需实际运行新臂：已在续跑轮预注册附录 §2 登记并执行
  （`PREREG_ADDENDUM_CONTINUATION.md`）。

## 一致性说明

- 本轮更正不改变：`recomputed.json`、`effects.csv`、`gates.csv`、图数据 CSV、
  NUMERIC_QA、VISUAL_QA（图内文字未含被更正表述；F3 图注为交付文本，已同步改写）。
- 归档副本（PR #626 分支 `docs/research/595-r3/completion/analysis/`）仍为更正前
  版本；如所有者需要，可在该分支追加本更正的 commit（非静默改写历史证据）。
