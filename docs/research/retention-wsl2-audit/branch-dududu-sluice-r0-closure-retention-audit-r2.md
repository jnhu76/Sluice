# dududu/sluice-r0-closure DELETE_CANDIDATE 复核（r2）

- 日期：2026-10-30 00:04–00:09 (+08:00)
- 对象：repo `jnhu76/dududu`，分支 `sluice-r0-closure`，tip `cc053cf9258b98546b16f9d2f08c5559fa73bc77`
- 范围：只读重验原审计（`branch-dududu-sluice-r0-closure-retention-audit.md`，2026-10-29 23:24）中最要命的三个字段：
  `uniqueEvidence`、`activeDependency`、`recovery`。本轮未执行任何删除/merge/close/force-push，
  未改动 Sluice/dududu 产品代码；仅 fetch 更新 /tmp/dududu-audit 引用并新增一份 /tmp 验证克隆。
- 结论：**三项全部成立，维持 DELETE_CANDIDATE**。

## 1. uniqueEvidence — 通过（比原审计更强）

| 检查 | 命令 | 结果 |
|---|---|---|
| 独有提交 | `git rev-list --count origin/main..origin/sluice-r0-closure` | `0` |
| 祖先关系 | `git merge-base --is-ancestor origin/sluice-r0-closure origin/main` | 成立；merge-base = tip 本身 |
| 独有对象（对全部 refs，非仅 main） | `git rev-list --objects origin/sluice-r0-closure --not --all` | **空输出**（零独有 commit/tree/blob） |
| 三点 diff | `git diff --name-only origin/main...origin/sluice-r0-closure` | 空 |
| 分支自身新增文件 | `git diff --name-status b94041d cc053cf9` | 恰为原审计所列 7 个 A（benchmarks/duckdb-r0 3 个 + fio-r0 4 个） |
| 逐文件 blob 比对 | 对 7 文件各跑 `git rev-parse <tip>:path` vs `git rev-parse origin/main:path` | **7/7 SAME**（如 prep-record.md = `19ffa28c`，fio README = `4b6a2b7e`） |
| main 树在位 | `git ls-tree -r --name-only origin/main benchmarks/` | 7 路径全部存在 |

收据：`receipts/r0closure-rr-02-unique-objects.txt`、`receipts/r0closure-rr-03-perfile-blob-compare.txt`。
"是否被 main 或归档覆盖"对本分支仍是空集问题：分支没有任何 main 之外的对象。

## 2. activeDependency — 通过

| 检查 | 命令 | 结果 |
|---|---|---|
| PR head | `git ls-remote origin 'refs/pull/*/head'` | 仅 pull/{1,2,3,7,8}/head，`grep -c cc053cf9…` = 0 |
| PR 状态 | `gh pr list -R jnhu76/dududu --state all --json number,state,headRefName,headRefOid` | 无 headRefName=sluice-r0-closure；唯一 OPEN = #8（r1-request-geometry @ 47cd1976）；#7 MERGED @ cc321a49 |
| dududu 全树引用 | `git grep -n 'sluice-r0-closure' origin/main` | 仅 4 处 .md provenance（README.md:17、async-host-probe/README.md:40、results/prep-record.md:9、docs/DUCKDB_FIO_R0_PREREG.md:226），均同时记载 SHA/基线 |
| dududu CI | `git grep -n 'sluice-r0-closure' origin/main -- .github`（唯一 workflow `.github/workflows/repo-policy.yml`） | 零命中 |
| Sluice 全树引用 | `grep -rn --exclude-dir=.git 'sluice-r0-closure' /home/hoo/Projects/Sluice/`（剔除审计目录自身产物后） | 仅 3 处研究文档 provenance（research/direct-beyond-parity/closure/r5-async-host.md:185、r5-prep.md:10、DUCKDB_FIO_R0_PREREG.md:220），与原审计同文件同行号 |
| Sluice CI/构建 | `grep -rn 'sluice-r0-closure' .github xmake.lua xmake` | 零命中 |
| Worktree | 两仓库 `git worktree list --porcelain` + 逐 worktree `git branch --show-current` | dududu 副本在 main；Sluice 16 个 worktree 仅主区在 master（其余 detached），无一检出本分支；dududu 克隆无本地 sluice-r0-closure 分支 |
| 进程 | `ps aux \| grep sluice-r0-closure \| grep -v grep` | 无 |

收据：`receipts/r0closure-rr-04-pr-refs.txt`、`receipts/r0closure-rr-05-branch-name-references.txt`
（该收据含审计目录自身 92 行自指命中，已附 classification note 说明）、`receipts/r0closure-rr-06-worktrees-processes.txt`。

## 3. recovery — 通过（实测删除后状态）

- 远端 tip 仍为记录值：`git ls-remote origin 'refs/heads/*'` 中 `refs/heads/sluice-r0-closure` =
  `cc053cf9258b98546b16f9d2f08c5559fa73bc77`，与原审计一致（receipts/r0closure-rr-01）。
- 包含 tip 的远端分支：`git branch -r --contains cc053cf9` → origin/main、origin/r1-request-geometry、
  origin/r5-async-host-probe、origin/sluice-r0-e5-verdict（及自身），与原审计一致。
- **关键加强实验**：`git clone --single-branch --branch main https://github.com/jnhu76/dududu.git
  /tmp/sluice-val-dududu-recovery-clone`（clone exit=0，分支列表只有 main）——模拟分支已删、
  其他分支不可见的状态。在该 main-only 克隆中：
  - `git cat-file -t cc053cf9…` → `commit`；
  - `git log -1` 元数据正确（2026-10-29 08:15:25 +0800, jnhu, "bench(duckdb): record dataset and oracle prep receipts"）；
  - `git merge-base --is-ancestor cc053cf9… main` 成立；
  - 抽查 blob `main:benchmarks/duckdb-r0/results/prep-record.md` 与 `main:benchmarks/fio-r0/README.md`
    的 SHA 与 rr-03 逐文件比对结果完全一致。
  即：**删除分支后任何 `git fetch origin` 都能恢复全部对象与内容**，恢复不依赖被删 ref。
- 裸 SHA fetch 重跑：`git fetch origin cc053cf9…` exit=0（分支尚在时）；删除后 GitHub 对裸 SHA
  按需拉取无承诺——维持原审计口径，恢复以 main 历史为准（本节已实测）。

收据：`receipts/r0closure-rr-07-recovery.txt`。

## 复核总评

原审计三个最要命字段的每一项关键声明均在本会话以命令重跑证实，无一需要修正；
`rev-list --objects --not --all` 与 main-only 克隆恢复实验比原审计口径更强。
维持 **DELETE_CANDIDATE**。本轮仅审计与写报告，未做任何破坏性操作。
