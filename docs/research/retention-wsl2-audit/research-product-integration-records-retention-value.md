# 分支保留价值审计 — `research/product-integration-records`

- 审计时间：2026-10-29（WSL2 审计机，`/home/hoo/Projects/Sluice`）；同日二次独立复核
  （receipts/pir-05），全部关键命令重跑，结论不变
- 审计对象：远端分支 `refs/heads/research/product-integration-records`（GitHub `jnhu76/Sluice`），
  本地远端跟踪 ref `refs/remotes/origin/research/product-integration-records`
- 结论：**ARCHIVE_FIRST**（有独有未归档研究数据，归档后方可删除；本阶段未执行任何删除）

## 1. Head 解析

指令字面命令 `git rev-parse origin/origin/research/product-integration-records`
**失败**（exit 128，unknown revision，receipts/pir-01）。实际存在的 ref 是
`refs/remotes/origin/research/product-integration-records`（远端 `origin`，分支名
`research/product-integration-records`）。按实际 ref 解析：

```
git -C /home/hoo/Projects/Sluice rev-parse origin/research/product-integration-records
→ 4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35
```

与预期 `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35` **一致**。tip 提交
2026-10-29 20:44:37 +0800，"research(integration): record archive push SHA in Phase 8 log"。

## 2. Associated PR

**None**。按指令给定的 10 条 PR 列表（8 条 Sluice + 2 条 dududu）按 headRef 匹配：无任何
headRef 等于 `research/product-integration-records`；无任何 headSha 等于
`4d621d91692a…`。两个 OPEN PR（Sluice #594 `research/593-key-path` @ `2714aa65`、
dududu #8 `r1-request-geometry` @ `47cd1976`）均不指向本分支。与
`state-freeze-report.md:21` 的既有记录一致（376 条全量 PR 中 grep 计 0）。这与分支自身的
`INTEGRATION_LOG.md` §8.4 相符：该分支是**刻意不建 PR** 的归档推送（"推送至 jnhu76/Sluice，
不建 PR"）。

## 3. Commit reachability

- `git branch -a --contains 4d621d91…` → 仅 `remotes/origin/research/product-integration-records`
  自身；无本地分支、无其他远端 ref；`git tag --contains` 为空（receipts/pir-01）。
- `git rev-list --count origin/master..origin/research/product-integration-records` → **2**：
  - `8c3adf6c` research(integration): archive E5 product integration records and Phase 3.3
    final-head perf receipts（38 文件 +5025 行）
  - `4d621d91` research(integration): record archive push SHA in Phase 8 log（+3 行）
- merge-base = `4b962abb`（origin/master 祖先，master 当前顶 `84e1b7d5` 的父提交）。

## 4. Blob reachability — 全部 38 个文件内容独有

`git diff --name-only origin/master...origin/research/product-integration-records` 列出 38 个
文件（receipts/pir-02）：根目录 `INTEGRATION_LOG.md`、`INTEGRATION_PLAN.md`，以及
`research/direct-beyond-parity/closure/perf-p2c1-finalhead/` 36 个文件。

逐文件 blob 比对（receipts/pir-03，方法：blob 若不出现在
`git rev-list --objects origin/master` 即为分支独有）：

- **38/38 全部 UNIQUE_TO_BRANCH**——字节相同的内容在 origin/master 的任何路径都不存在
  （同路径与其他路径都查过，grep 计 NONE）。
- 分支独有对象共 43 个 = 2 commits + 7 trees + 34 blobs（38 个路径中 `machid/` 有 5 个
  路径与同目录兄弟文件共享 blob：`head-file_info.txt`=`base-file_info.txt`=ad30699a、
  `head-read.txt`=`base-read.txt`=3c24136f、`head-read_at.txt`=`head-write_at.txt`=
  `base-write_at.txt`=`base-read_at.txt`=680607bf；`rev-list --objects` 按对象去重）。
- master 根目录无 `INTEGRATION_LOG.md`/`INTEGRATION_PLAN.md`（master 根 ls-tree 证实）；
  master 的 `research/direct-beyond-parity/closure/`（#610 归档，ac995641）只有
  `perf-p2c1/`（R4 中段数据集），**没有** `perf-p2c1-finalhead/` 目录。

diff 中无 `include/`、`src/`、`apps/`、`xmake*` 路径（grep 计 NO_PRODUCT_PATHS_IN_DIFF）
→ **无未推送产品变更**。

## 5. Unique evidence（独有内容定性）

内容只存在于本分支（本地跟踪 ref + GitHub 远端 ref，两处是同一份）：

1. **`research/direct-beyond-parity/closure/perf-p2c1-finalhead/`（36 文件）** — Phase 3.3
   final-HEAD ABBA 性能回归原始收据：PREREGISTERED_PLAN.txt（采样前预注册）、ENV、SUMMARY、
   aa-*/ab-* 原始输出、machid base/head 机器指纹、run/build 脚本。SUMMARY 开头自述测量
   base=43cb8029（C1 合并后 master）、candidate=#612 重锚头 64508282，工具与 R4
   字节一致。这是 PR #612 性能结论（eofmix & cursor WIN_CONFIRMED、comp-read-4k
   INCONCLUSIVE、负控制 PARITY）的**原始证据**，且 `INTEGRATION_LOG.md` §8.4 明确记载
   这批收据"此前未提交"。#610 归档（master `perf-p2c1/`）是另一数据集（R4 中段），不覆盖它。
2. **`INTEGRATION_PLAN.md`** — E5 产品集成操作前计划：squash/merge 约定实证、PR 实时
   快照与 Head SHA Guard、合并顺序与预期树、#612 重锚方法、备份/恢复规则（§7）。
3. **`INTEGRATION_LOG.md`** — 集成收口日志，Phase 8 记录全部 PR/Issue 的终态
   （C1_IN_MASTER=VERIFIED、P2C1_IN_MASTER=VERIFIED、DUDUDU_R0_IN_MAIN=VERIFIED、
   #611 SUPERSEDED、#613 清单外偏差披露）及本分支自身的推送 SHA 记录。

三者均为研究/收口记录，不构成产品权威；按 AGENTS.md 证据纪律，其权威归档位置应在
master 的 research 归档（同 #610 模式），而不是仅存于一条无 PR 关联的分支。

## 6. Active dependency

- OPEN PR head：无（§2）。
- master 跟踪内容（含 `.github`、`scripts/`、`xmake/`）grep 分支名：`git grep -n
  "product-integration-records" origin/master --` → **NO_MATCH_ON_MASTER**（receipts/pir-04）。
- 工作树 grep：仅审计目录 `docs/research/retention-wsl2-audit/` 内的既有审计文档提及，
  无脚本/CI 引用。
- Worktree：`git worktree list` → 主工作树在 `master`，三个 `/tmp/sluice-val-*` 验证
  worktree 均 detached 在 `84e1b7d5`/`2714aa65`，**无人检出本分支**。
- 运行进程：`ps aux | grep -i product-integration` → NO_PROCESS。

## 7. Working tree

不适用：分支未被任何 worktree 检出（§6），无 dirty 状态可查。

## 8. Recovery（删除后恢复）

已记录完整 HEAD SHA：**`4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35`**（独有提交链
`4d621d91 → 8c3adf6c → 4b962abb`，父提交在 master 历史内，不随分支删除丢失）。

- 当前 `git ls-remote origin` 实测 GitHub 仍有
  `refs/heads/research/product-integration-records = 4d621d91…`（receipts/pir-04）。
- 仅删本地跟踪 ref：`git fetch origin` 即可原样恢复，零风险。
- 若连远端分支一起删除：因**无 PR 关联**，GitHub 上不存在保留对象的引用；提交
  `4d621d91`/`8c3adf6c` 只能在 GitHub 服务端 GC 前按显式 SHA 拉取
  （`git fetch origin 4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35`，需推送权限），
  或从仍持有对象的本地克隆恢复（本克隆在 `git gc`/prune 前保留全部对象；
  本审计 receipts 已固化关键命令输出）。**PR 溯源不可用**——分支按设计未建 PR。
- 因此：删除前必须先归档（见结论），归档后恢复需求消失。

## 9. 决策

| 条件（DELETE_CANDIDATE 需全部成立） | 结果 |
|---|---|
| 非 OPEN PR head | ✅ 满足 |
| 无独有未归档数据 | ❌ **不满足**：38/38 文件内容独有，master/#610 归档均不覆盖 |
| 无未推送产品变更 | ✅ 满足（diff 全为 research/docs） |
| 相关原始数据已有可靠备份 | ❌ 不满足：唯一持久副本就是本分支自身（本地+远端同一 ref） |
| 无运行中 Agent/Worktree 依赖 | ✅ 满足 |
| 已记录完整 HEAD SHA 与恢复步骤 | ✅ 满足（本文档 §1/§8 + state-freeze-report.md:21） |

**决策：ARCHIVE_FIRST** — 该分支是 E5 产品集成收口的唯一持久归档推送（final-head perf
原始收据 + 集成计划/日志），在把内容并入 master 研究归档（如 #610 模式的归档 PR）或
等效持久归档之前不得删除；归档完成并复核后，本分支即成为纯冗余，可进入删除候选。
本阶段仅出结论，未执行任何删除/合并/force-push。

## Receipts

- `receipts/pir-01-head-and-refs.txt` — ref 解析（含字面 ref 失败实录）、containing refs、tags
- `receipts/pir-02-commit-reachability.txt` — merge-base、独有提交、产品路径检查、diff stat
- `receipts/pir-03-blob-comparison.txt` — 独有对象计数、machid blob 去重、38 文件逐一比对
- `receipts/pir-04-remote-and-dependency.txt` — ls-remote、worktree、脚本/CI grep、进程检查
- `receipts/pir-05-second-pass-verification.txt` — 二次独立复核（本机重跑全部必查命令；
  另证 master 仅有的 4 个 "finalhead" 文件属 f2-authority-contraction 另一 campaign、blob 不同；
  §8.4 所述 workflow-drafts 副本在 host E5 的 `/home/jnhu/...` 下，本机不存在该路径，
  故从本机视角分支即唯一可验证持久副本）
