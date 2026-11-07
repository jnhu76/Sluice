# #595 研究暂停检查点（pause-20261107）

研究暂停收口：保存**研究过程与算法设计**，原始测量数据不归档（用户决定
2026-11-07：数据之后重跑）。产品 master 保持 A0 / IMMEDIATE，未合并任何
研究原型。#595 保持 OPEN（研究暂停，先阅读资料）。

## 本检查点包含什么

1. **全部研究轮次的文档、协议、分析器与再跑脚本**：R1（E5 submission，
   已并入 master）、R2 causal-r2、R3 completion/continuation、R4
   state-aware、R5 phase-A + repair round、flow audit（R1–R6 复核 +
   R7–R10 修正）、information audit——各自冻结在对应 PR 分支（见
   `ARCHIVE_INDEX.json`），并且全部包含在离线 bundle
   `/home/jnhu/sluice-research-archive/595/pause-20261107/source/sluice-595-research.bundle`
   （自包含完整历史，sha256 见同目录 `.sha256`）。
2. **不在任何 git 分支里的算法设计**：R2 causal-r2 三个原型变体
   （worktree / worktree-timing / worktree-trace，基线 master@271b81bf）
   与 bounded-progress 原型的未提交 diff + 未跟踪源文件，逐字保存在
   `worktree-state/`（含 `status.porcelain`、`tracked-modifications.patch`、
   `untracked/`）。R3 原型 diff 已在 PR #627
   （`docs/research/595-r3/continuation/evidence/r3-worktree-full-diff.patch`）。
3. **结论快照与继续研究入口**：`DECISION_SNAPSHOT.md`（当前有效结论 /
   未识别原因 / 已撤回表述 / 未批准干预）、`RESUME.md`（三个命令入口 +
   重跑数据指南 + 阅读入口）。
4. **环境快照**：`environment/env.txt`（OS/kernel/CPU/编译器/xmake/
   liburing/Python 包及位置）、`pip-user-freeze.txt`。

## 不包含什么（按用户 2026-11-07 决定）

- 原始测量数据（results.jsonl、.bin dump、trace、perf/strace 原始件、
  历史矩阵 raw）**不归档**；E5 本机目录 `/home/jnhu/sluice-595-e5/`
  暂未删除，但恢复不以它为前提。重跑方法见 `RESUME.md` §3。
- 测量期二进制不归档（重跑即重建，配方在各轮分支内）。

## 恢复三级（按本范围验收）

- **L1 源码/文档恢复**：`git clone sluice-595-research.bundle` 后全部
  21 个 ref 可检出；`worktree-state/` patch 可在 master@271b81bf 逐字
  重放（`git apply --check` 通过，见 `RESTORE_RECEIPT.json`）。
- **L2 分析复算**：原始数据未归档，本轮不做离线复算验收；复算/重测
  入口见 `RESUME.md` §3（各轮分析器与冻结参数都在分支内）。
- **L3 研究环境恢复**：按 `environment/` + `RESUME.md` §2 重建
  （python3.14 + user-site numpy/matplotlib、gcc/xmake/liburing 版本
  已记录；能重建编译与分析环境，不承诺复现同性能数字）。
