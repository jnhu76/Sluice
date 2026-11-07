# #595 Flow Audit — 完整 I/O 执行流程、反馈回路与最小机器工作审计

Round `flow-audit`, 2026-11-07, native E5. 隔离 worktree
`/home/jnhu/sluice-595-e5/flow-audit/worktree`（分支
`research-595-flow-audit` @ `271b81bf`；审计阶段全离线、无计时运行，
本 PR 为审计结束后经明确授权的归档提交）。核心问题：完整执行流程
的问题是否被过早压缩为"感知调度"问题；应先改调用者/adapter/提交/
完成处理/结果观察哪一环。

## 文件

- `BASELINE.md` — 三对象（PRODUCT/RESEARCH/BENCH）→ SHA/patch →
  二进制 → 数据集 → 适用结论；环境与 CPU 口径。
- `FLOW_AUDIT.md` — 主要结论、五组 Mermaid（对照流程 / W02 vs W03 /
  Request 生命周期 / refill 顺序 / 反馈回路）、源码对应表、三对象差异。
- `WORK_AND_WAIT_LEDGER.csv` — 机器工作账本（21 项）+ 等待账本
  （11 项），含执行者/锁/频率/证据等级/分类/删除条件/正确性义务。
- `HYPOTHESES.md` — H1-H5 + H6/H7 增补的竞争解释表与组合判断。
- `DECISION.md` — ≤500 字路线裁决：候选排序与唯一下一项。
- `NEXT_EXPERIMENT.md` — C-A（产品 transport 层 enter 摊销）完整协议。
- `scripts/cpu_scope_recalc.py` — CPU 口径复算（io-wq 份额，
  invocation-scope ticks，带告警）。
- `scripts/verify_inherited_evidence.py` — 继承证据核验：主指标 9 格
  逐位复现 + F5 修正后位置尾风险 4 项（13/13 PASS，exit 0）。

## 一句话结论

批量入口与感知策略整体是 research-only 代码层；产品唯一提交路径的
唯一稳健可摊销工作是每 op 一次 `io_uring_submit`（两轮矩阵 −22% CPU/
+28% 吞吐复现）。p99 +18% 与浅队列税的证据结构与"研究呈现路径
成本 × 后块位置/调用税"一致，"batching 必然付 p99"未确立；感知
决策的输入全是 harness 状态，无产品落点。唯一下一项 = C-A：在
transport 层（已拥有该义务的层）做 Q=8 延迟 flush 摊销的产品路径
A/B，正式门 = R4 全口径，保护格含 d4/d2serial/W02-d1/d2；失败即停，
不做量子搜索。默认 A0 不变；候选 B 不放行。

## 证据引用

历史 raw/分析全部按路径+SHA 引用（见 BASELINE.md），未复制归档；
无新计时数据，故无新性能图；F5 相关引用均标 PERTURBED。
