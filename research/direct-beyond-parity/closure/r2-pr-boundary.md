# R2 — PR #601 / #602 交付边界整理记录

日期：2026-10-29（E5 native Linux）。执行者：R2 PR 边界整理员（E5 Closure workflow）。
所有命令均在本机 `/home/jnhu/Source/Sluice`（及指定临时 worktree）实测执行。

## 0. 结论

`pathTaken = FORCE_WITH_LEASE`（§5.3 路径 a）。

- PR #601 已整理为 evidence-only：diff vs master 仅含
  `research/600-direct-excess-cost/` 5 个证据文件，0 个生产路径。
- PR #602 未被任何方式改动（HEAD 保持 `77e5399a`），独立拥有全部产品改动。
- 两个 PR 均保持 OPEN；未 merge 任何 PR；未采纳新公开 API；未改 v2-r1 契约。
- 原始 HEAD 已建立远程备份且验证可检索后才执行 force-with-lease。

## 1. 前置实测（任务 1：blob 身份重新验证）

`git fetch origin master` 后：local HEAD = origin/master = 远端
`refs/heads/master` = `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`，无
ahead/behind。

`gh pr view` + `git ls-remote` 实测（与 R0 快照一致，无 Delta）：

| 项 | 值 |
| --- | --- |
| PR #601 | OPEN, base=master, head=`sluice-600-direct-research`, SHA=`e4bb8833699d0ca5dee191883f3fec6d41b221b2`, 非 draft |
| PR #602 | OPEN, base=master, head=`sluice-600-direct-c1`, SHA=`77e5399a45998dea396699fa2e063c94a665945a`, 非 draft |

四路径 blob 身份（`git rev-parse <head>:<path>`，2026-10-29 实测）：

| 路径 | #601 head `e4bb8833` | #602 head `77e5399a` | master `bdfd0d8d` |
| --- | --- | --- | --- |
| `src/blocking_file.cpp` | `6660a42367ef…76d0` | `6660a42367ef…76d0` | `b9c0def804dc…c351` |
| `include/sluice/detail/file_semantics.hpp` | `e39760ae534a…a7e0` | `e39760ae534a…a7e0` | `8f256df24aab…17e4` |
| `xmake/tests.lua` | `febe6515455b…db43` | `febe6515455b…db43` | `70327072db01…82f3` |
| `tests/semantic_range_test.cpp` | `0d3eb7e0151e…81d1` | `0d3eb7e0151e…81d1` | `f47ca6c1214a…7c44` |

结论：已知问题在当前 HEAD 重新成立 —— 两个 PR 携带完全相同的产品源码与测试。

进一步提交级实测：

- #601 含 2 提交：`ea2fc4fc`（`perf(direct): dispatch admission verdicts
  without Precheck materialization (#600 C1)`）+ `e4bb8833`（证据提交，仅新增
  `research/600-direct-excess-cost/` 5 文件，348 insertions）。
- #602 含 2 提交：`4c3e698d`（同名 C1 产品提交）+ `77e5399a`
  （`docs(ledger): record #600 C1 SEM-03/04 mutation evidence`）。
- `ea2fc4fc` 与 `4c3e698d`：`git patch-id --stable` 均为
  `8ed5d3c0ae157859c44548fe838824aecbfbf520`（patch 相同）；
  `git rev-parse <c>^{tree}` 均为
  `006c31d19cfd5013d9069a479163837234e2b894`（tree 相同）；
  两者 parent 均为 master `bdfd0d8d`。即同一 C1 产品改动被两个 PR 重复携带。

重写合适性检查（选择路径 a 的依据）：

- `gh api repos/jnhu76/Sluice/branches/sluice-600-direct-research` →
  `protected: false`；`gh api .../rules/branches/sluice-600-direct-research`
  → `[]`（无 ruleset）。
- PR #601 仅有 1 条 CodeRabbit 自动摘要评论，无任何人工 review
  （`gh pr view 601 --json reviews,comments`）。

## 2. 备份（任务 2）

```
git push origin e4bb8833699d0ca5dee191883f3fec6d41b221b2:refs/heads/backup/pr-601-original-head-e4bb8833
# * [new branch]  e4bb8833… -> backup/pr-601-original-head-e4bb8833
```

备份验证（push 后重新 ls-remote + fetch）：

- `git ls-remote origin refs/heads/backup/pr-601-original-head-e4bb8833` →
  `e4bb8833699d0ca5dee191883f3fec6d41b221b2`。
- `git fetch origin refs/heads/backup/...` 后 `git rev-parse
  origin/backup/pr-601-original-head-e4bb8833` = 同一 SHA。
- 备份 ref 下四生产路径 blob 与证据 blob（如
  `research/600-direct-excess-cost/EVIDENCE_INDEX.md` =
  `a5bbf1cfda93…527e`）与原 HEAD 完全一致。

备份内容记录（原始 #601 HEAD `e4bb8833`，patch-id 见 §1）：

- 提交 1 `ea2fc4fca1a65a63ef3664f71dab1014e98c3b25`
  patch-id `8ed5d3c0ae157859c44548fe838824aecbfbf520`。
- 提交 2 `e4bb8833699d0ca5dee191883f3fec6d41b221b2`
  patch-id `70a0899cf8d428c3bbee01163d8c34129df7b25c`。
- 全量 diff（master..e4bb8833）：9 文件，+463/−98 ——
  `include/sluice/detail/file_semantics.hpp`（15 行）、
  `src/blocking_file.cpp`（154 行）、`tests/semantic_range_test.cpp`（19 行）、
  `xmake/tests.lua`（25 行）+ `research/600-direct-excess-cost/` 5 文件
  （DIRECT_COST_DAG.md 52、DIRECT_MACHINE_COST_LEDGER.md 94、
  EVIDENCE_INDEX.md 41、PHASE3_VALIDATION.md 99、
  PHASE_I_COST_ATTRIBUTION.md 62）。

## 3. 整理执行（任务 3，路径 a）

前置复核：push 前再次 `git ls-remote origin
refs/heads/sluice-600-direct-research` → 仍为 `e4bb8833…`（远端未变）。

在指定临时 worktree 重建：

```
git worktree add /home/jnhu/Source/Sluice-e5-pr601 -b sluice-600-direct-research-evidence bdfd0d8d…
cd /home/jnhu/Source/Sluice-e5-pr601 && git cherry-pick e4bb8833…
# 新 HEAD = 2fcc79646ee42d1699468e0a50c0269f405b92d3
```

重建后校验（实测）：

- 新 head diff vs master：仅 5 个 `research/600-direct-excess-cost/` 文件，
  348 insertions；生产路径计数 0。
- 5 个证据文件 blob 与 `e4bb8833` 下逐一相同；新证据提交 patch-id
  = `70a0899cf8d428c3bbee01163d8c34129df7b25c` 与原提交相同。
- 提交信息保持原文
  `research(direct): #600 E5 excess-cost attribution and C1 validation evidence`。

安全推送：

```
git push origin --force-with-lease=refs/heads/sluice-600-direct-research:e4bb8833699d0ca5dee191883f3fec6d41b221b2 \
  sluice-600-direct-research-evidence:refs/heads/sluice-600-direct-research
# + e4bb8833...2fcc7964 sluice-600-direct-research-evidence -> sluice-600-direct-research (forced update)
```

lease 显式锚定远端期望值，若远端在备份后被他人移动，推送会被拒绝。

推送后状态（`git ls-remote` + `gh pr view 601 --json files`）：

- `refs/heads/sluice-600-direct-research` = `2fcc79646ee42d1699468e0a50c0269f405b92d3`。
- PR #601：OPEN，head=`2fcc7964`，files = 5 个证据文件。
- 本地分支 `sluice-600-direct-research` 同步为 `2fcc7964`；
  本地构建分支 `sluice-600-direct-research-evidence` 保留于临时 worktree。
- 冻结 worktree `Sluice-600-base`、`Sluice-600-wt` 未做任何读写。

## 4. §5.3 五项后置条件（任务 4，逐项实测）

1. **#601 不再引入生产源码** — PASS。
   `git diff --name-only bdfd0d8d..2fcc7964` = 5 个 research 文件；
   生产路径 grep 计数 = 0；GitHub API `pulls/601/files` 一致。
2. **#602 独立拥有全部产品改动** — PASS。
   `git ls-remote origin refs/heads/sluice-600-direct-c1` =
   `77e5399a…`（未变）；`git diff --name-only master..77e5399a` =
   `docs/roadmap/v2-conformance.md` + 四生产/测试路径；GitHub API
   `pulls/602/files` 一致。
3. **两个 PR 的证据链接仍可用** — PASS。
   PR #601 在 GitHub 渲染 5 个证据文件（blob 与原 HEAD 逐字节相同）；
   PR #602 文件列表未变；原始 #601 HEAD 及其全量 diff 可从远程备份分支
   `backup/pr-601-original-head-e4bb8833` 访问（fetch 实测通过）。
4. **原始 E5 Benchmark SHA 仍可追溯** — PASS。
   证据文档中出现的 git SHA：BASE `bdfd0d8d…`（= master，可达）；
   `PHASE3_VALIDATION.md` CANDIDATE_SHA `ea2fc4fca1a6…`（实测
   `git merge-base --is-ancestor` 通过：为备份分支祖先）；EVIDENCE_INDEX
   引用同候选。`6116e69c…` 为数据集内容 sha256（非 git 对象，
   `git cat-file -t` 实测确认），在证据文本中原样保留。
5. **合并顺序不造成重复/丢失** — PASS。
   在临时 worktree 模拟两种顺序：master→#601→#602 与
   master→#602→#601 的最终 tree 均为
   `a971c265c327c2c48ee46a65c27752e870c0880e`（相同）；合并结果中四
   生产路径 blob 均等于 #602 版本（逐路径 `git rev-parse` 比对），
   证据文件仅出现一次。模拟分支已删除，未遗留。

## 5. 回写（任务 5）

- PR #601 评论：
  https://github.com/jnhu76/Sluice/pull/601#issuecomment-6483552374
- Issue #600 评论：
  https://github.com/jnhu76/Sluice/issues/600#issuecomment-6483556783

## 6. 未解决事项 / 边界说明

- `backup/pr-601-original-head-e4bb8833` 与
  `backup/...` 分支的删除时机留给 Owner/R9 closeout：在 #602 合并且
  #601 合并之后方可考虑；本任务不删除任何远程分支。
- 本任务未运行任何 benchmark、未改动任何测试断言、未触碰
  `research/direct-beyond-parity/` 下其他 Agent 的既有文件（仅新增本记录与
  `closure/` 目录）。
- 未 merge 任何 PR；未采纳新公开 API；未修改 v2-r1 公共契约。
