# BASELINE — #595 调度信息供给与缺口审计（2026-11-05）

审计对象冻结。本文件记录本轮实际读取的三个研究对象与一手资料。

## 1. PRODUCT

- origin/master = 本地 HEAD = `48d9be5e276ef4a978e4ee298e21a0a065f282f9`（merge of #624）。
- 工作树：除未跟踪目录 `research/609-async-zero-overhead/`（他人任务）与本轮
  审计目录 `research/595-information-audit/` 外干净；未改动任何跟踪文件。
- 契约：`docs/explicit-io-contract.md`（v2 根，重点读 TERM-01、W-01..W-03、
  ARCH-01/02、INV-01、REQ-02/05、OBS、PROG-01..04）。
- 产品源码行号引用均以该 SHA 为准（下记 `P:<file>:<line>`）。
- 已核实产品公开/内部头文件不含任何 ready-batch 入口
  （`context_submit_ready_reads` / `ready_batch_cap` / `submit_ready_reads` 零命中）。

## 2. R3（研究原型，仅 patch 工件，未进入 master）

PR 栈（origin 只读 fetch 为本地 `pr-625/626/627` 引用，GitHub 零写入）：

| PR | 分支 | head SHA | 内容 |
|---|---|---|---|
| #625 | research/595-causal-r2 | `0a22a79195f1b913145fcd443496e7f70b08cdcf` | causal-r2 归档 |
| #626 | research/595-r3-archive | `772a4329497eefdbccde6f28412a330f3c8e95eb` | R3 完成轮归档 |
| #627 | research/595-r3-continuation | `0ac2b582c36f0a9013cfc1b3d024fd3bf258d837` | 续跑轮（本轮读取版本） |

- 与 prompt 给定锚点 `0ac2b582…` 一致（实测核实）。merge-base = `271b81bf`
  = origin/master~1；三支为干净叠加，master 仅多出 #624 的 merge 提交。
- 隔离读取树：`/tmp/sluice-pr627`（`git worktree add --detach`，只读使用）。
- 原型工件（本机复算 sha256 前 16 位，与 PREREG/回执登记一致）：
  - 冻结原型：`completion/evidence/r3-completion.patch` = `2596f204035f9695…`
    （改 lib：uring_backend/async_io_context/uring_transport/initiation_precheck/
    progress_source/test_access；新增 bench/r3_ready_batch.hpp + 3 个测试）
  - 续跑 delta（bench-only）：`delta_w03.patch` = `57f21e022e7b9d0d…`、
    `delta_w02.patch` = `4aa7b306fc8469ef…`；全量 `r3-worktree-full-diff.patch`
    = `73fa116ff668ea58…`；PREREG 附录 = `e46038e47e402404…`。
- 读取的关键续跑材料：`VERDICT_CONTINUATION.md`、`RESULTS_CONTINUATION.md`、
  `PREREG_ADDENDUM_CONTINUATION.md`、`A0_AND_SUFFIX_RECEIPT.md`、
  `raw/TRIPLET-W03-D16/{results.jsonl,summary.json}`、`raw/AB-W02R3-D2/results.jsonl`、
  `raw/mechanism-9_3/enter_summary.json`。行号引用记 `R3:<path>:<line>`。
- 复算：`scripts/recalc_continuation_counts.py`（输出 `recalc_output.txt`）。
  核对结论：W03 B 24 invocation/120 rep/240 万请求、多成员组入口 153,700、
  `single−suffix=30`、**呈现组均 15.61 条**（= 前缀接受 8.00 = cap ＋ 后缀
  untouched；与完成轮 9,160==9,160 的"呈现≥2"修正同口径）、W02 B 每 rep
  `entry=19,999/group=1/prefix_max=2`、§9.3 enter {A0,A1: 60,000×{1}; B:
  33,750={1:30000,8:3750}，ΣSQE=60,000}、主门 CPU −14.13/上界 −13.68、
  p95 +25.75/上界 +27.95、p99 +37.85/上界 +39.94、AB CPU +2.17/单侧上界
  +2.72 全部与发布值一致（RECALCULATED）。

## 3. REFERENCE（一手资料；参考实现 ≠ E5 运行版本）

| 方法 | 版本/章节 | 获取 | 证据级 |
|---|---|---|---|
| IX | OSDI'14 全文（usenix PDF） | 全文读取 | SOURCE_VERIFIED |
| VLDB26 io_uring DBMS | PVLDB 19(1), arXiv:2512.04859v2, §3.3.3 逐字 | 全文读取 | SOURCE_VERIFIED |
| mq-deadline | linux v6.17 `block/mq-deadline.c` + deadline-iosched 文档 | 全文读取 | SOURCE_VERIFIED（内核 tag ≠ E5 内核） |
| Kyber | linux v6.17 `block/kyber-iosched.c` | 全文读取 | SOURCE_VERIFIED |
| NAPI | docs.kernel.org/6.12 networking/napi.html | 全文读取 | SOURCE_VERIFIED |
| Shenango | NSDI'19；PDF 拉取失败，经 ACM R4P 综述 + 论文摘要两份二手独立印证（5µs 调度核、队列长度导数为拥塞信号、动作=核分配） | 二手 | PARTIALLY_VERIFIED（有边界的比较） |

## 4. 本轮边界

- E5 原生运行检查：NOT_NEEDED / NOT_RUN（纯源码 + 仓内归档证据审计；
  复算只读归档 JSONL，不触 E5）。
- 未实施新算法，未建 runtime/观测框架，未跑任何性能矩阵。
- GitHub 零写入；主工作树跟踪文件零改动；历史 raw 树未复制
  （读取均走 `/tmp/sluice-pr627` 冻结树）。
