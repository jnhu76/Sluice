# MISSING — 缺失与失效样本清单（论文分析口径）

依据 `PREREGISTERED_R3_COMPLETION.md` §14 与 `analysis/recomputed.json`。
未测单元在 metrics/effects/gates.csv 中一律为空值 + coverage 标注，**不显示为零**。

## A. 完全未测（NOT_RUN / BLOCKED）

| 预注册块 | 原因 | 对本分析的后果 |
|---|---|---|
| MAIN-W03-D16-B、MAIN-W03-D16-B-vs-A0、MAIN-W03-D16-B-vs-A1、MAIN-W03-D16-A1 | BLOCKED_SEMANTICS_NOT_READY（semanticsReady 未翻转，无 §14.3 独立 verifier 回执） | **F1（B−A0/B−A1/A1−A0 净效应）无 gate-grade 数据**；主门（CPU 点 ≤−10%、上界 ≤−5%）未检验 → 机制性能 INCONCLUSIVE |
| CROSS-W03-D4、CROSS-W03-D64（B/A1 与 A0 基线） | 同上；两个 enabled A0 基线块（SAFE-A0-W03-4k-d4/d64）也未执行且未记 NOT_RUN（偏差，见 PROTOCOL.md §5） | F1/F5 的 d4/d64 格子 NOT_RUN |
| NEG-W02-D2-NEW-B（R3 B 在 D2） | R3 树 bench_w02_pipeline 无 R3 研究臂/无 D2 case（NO_W02_R3_ARM） | **Q1（新算法守 D2）无数据**；F2(a) 只能展示 B_R2（旧策略） |
| PROT-64K-D16-B、PROT-THREADPOOL-B、PROT-SMT-2-12-B、PROT-XPHYS-2-8-B、PROT-SATURATION-B(R3) | BLOCKED_SEMANTICS_NOT_READY | F4/F5 对应格子 NOT_RUN（仅有 A0-only 参考） |
| RESOURCE-B 正式资源门 | bench 无 request-count 旋钮（valgrind 512/1024 斜率不可测）→ NOT_RUN；RSS/malloc/timer/线程以诊断口径代替 | F5 资源格 = 诊断证据（B +48 KB RSS、+6 次一次性 malloc、0 新线程/timer/队列），非正式门 |
| PRODUCT-604 | 无 READY_FOR_595 handoff | HOLD_NO_REAL_APPLICATION；F5 产品面板单独呈现 |
| SAFE-A0-W02-D16（R3 A0 在 W02-D16） | enabled 未执行 | 仅缺参考值；不影响任何 gate |

## B. 部分完成（诊断口径，禁入门判定）

| 项 | 实际 | 缺口 |
|---|---|---|
| MECH-POS-D2POS 正控 | 1 invocation × 3 phase（3/3 phase 全部 to_submit=2） | 预注册要求 3 次独立 invocation（偏差 D-1） |
| 机制 enter/PMU/分桶 | 单窗口带仪器 | 不是 §6 配对计时；CPU/cycles 差异只作机制旁证 |
| d2serial 负控 | enter/SQE/CQE/defer/eventfd 三臂逐项相同（1 invocation） | 无配对 CPU/尾延迟统计 |

## C. 失效/剔除样本

无。240/240 gate-grade invocation 有效；0 次替换（attempts.jsonl 为空或不存在）；
repairUsed=false（未动用唯一一次测量条件修复）。

## D. 前轮清单变化（非缺失，登记）

前轮 `r3/evidence/MANIFEST.sha256` 225/246 OK：21 项 FAILED 全为 completion patch
前进导致的 worktree 源/二进制条目（预期，偏差 D-11）；本轮主清单
`evidence/MANIFEST.sha256` 2451/2451 OK。
