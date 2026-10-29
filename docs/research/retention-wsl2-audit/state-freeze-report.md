# Sluice 仓库状态冻结报告（retention-wsl2-audit）

冻结时间：2026-10-29（会话内实时取证）
仓库：jnhu76/Sluice，本地路径 /home/hoo/Projects/Sluice，origin = git@github.com:jnhu76/Sluice.git
本轮性质：只读审计。未修改任何产品代码，未 merge/close PR，未删除任何分支/Worktree/证据，未 force push。
本轮写入仅限本报告目录（含 receipts/）。

## 1. 远程分支全集（step 1）

命令：`git -C /home/hoo/Projects/Sluice for-each-ref refs/remotes --format='%(refname) %(objectname)'`
回执：receipts/01-remote-refs.txt。共 8 个远程引用：

| ref | SHA | 备注 |
|---|---|---|
| refs/remotes/origin/HEAD | 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b | 符号引用 → origin/master |
| refs/remotes/origin/master | 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b | 默认分支 |
| refs/remotes/origin/backup/pr-601-original-head-e4bb8833 | e4bb8833699d0ca5dee191883f3fec6d41b221b2 | PR #601（MERGED）原 head 备份 |
| refs/remotes/origin/backup/pr-612-original-head-70d48f5e | 70d48f5eb55a664544b5160cd4675d32150bb21e | PR #612（MERGED）原 head 备份 |
| refs/remotes/origin/research/575-native-e5-atomic-rmw | e9baafb9d117cbaf387dde9f4433ebbacb4a73b3 | = CLOSED（未合并）PR #584 head |
| refs/remotes/origin/research/593-key-path | 2714aa65fe4837d30f5fa3fcdb27db38c23b7136 | = **OPEN** PR #594 head（活跃） |
| refs/remotes/origin/research/product-integration-records | 4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35 | 376 条 PR 中无任何 headRefName/SHA 匹配（grep 计 0） |
| refs/remotes/origin/research/r6-ac1-vectored-semantics | 999b5305ae7d413a2a71acde926f8c6cfef6876a | = CLOSED（未合并）PR #611 head |

PR↔分支交叉验证基于 receipts/pr-list-full.tsv（376 条全量）。

## 2. 本地分支（step 2）

命令：`git -C /home/hoo/Projects/Sluice branch -vv`
回执：receipts/02-branch-vv.txt。仅 1 个本地分支：

- `master` @ 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b，上游 origin/master，无 ahead/behind（与 origin/master 同步）。

## 3. Worktree（step 3）

命令：`git -C /home/hoo/Projects/Sluice worktree list --porcelain`
回执：receipts/03-worktree-list.txt。仅 1 个已注册 worktree：

- /home/hoo/Projects/Sluice — branch refs/heads/master @ 84e1b7d5，`git status --porcelain` 输出为空 → **clean**。

**不存在其他 Agent 持有的执行工作树**（git 注册的 worktree 仅主工作区一个；/tmp 下亦无 sluice-* 或 dududu-audit 目录，见 receipts/05-running-processes.txt）。

## 4. 主工作区状态（step 4）

命令：`git -C /home/hoo/Projects/Sluice status --porcelain` + `git branch --show-current`
输出为空 → **主工作区 clean，无未提交文件**；当前分支 master。dirtyWorkspace = false。

## 5. PR 全集（step 5）

命令：`gh pr list --repo jnhu76/Sluice --state all --limit 250 --json number,title,state,headRefName,headRefOid,baseRefName,url`
首次返回恰为 250 条上限 → 依指令分页取全：改用 `--limit 1000` 重取，返回 **376 条**（< 1000，无截断）。
完整性：编号范围 1–613（缺号为 issue），376 个互异编号；min=1，max=613。
状态分布：**361 MERGED / 14 CLOSED / 1 OPEN**。
- 唯一 OPEN：**#594** `research/593-key-path` @ 2714aa65fe4837d30f5fa3fcdb27db38c23b7136 → master。
- CLOSED（未合并）14 条：#58, #77, #317, #318, #319, #320, #322, #323, #354, #445, #535, #570, #584, #611。

回执：receipts/pr-list-full.json（gh 原始 JSON）、receipts/pr-list-full.tsv（制表符全表）。

## 6. 运行中进程（step 6）

命令：`ps aux`（两组过滤，见 receipts/05-running-processes.txt）。
唯一引用 /home/hoo/Projects/Sluice 的进程为 PID 2621 的 zcode workflow 运行时
（`.../runtime/node ... /home/hoo/Projects/Sluice/.zcode/workflow-runs/dwfrun-....mjs`），
即本审计工作流自身，**非构建/Benchmark/测试进程**。
未发现任何引用实验目录（如 sluice-593-research）的运行中进程。

## 7. auditTargets 组装（step 7）

去重池 = 远程分支（剔除 origin/master 与 origin/HEAD，剩 6 条）∪ 本地分支（master）。
本地 master SHA = origin/master SHA = defaultSha，但 origin/master 已被剔除出池，无合并目标，故按规则单列。
6 条远程分支 SHA 互异，无本地分支与之同 SHA → 均单列，alsoLocal 全部为空。共 **7 条**。

## 结论摘要

- 仓库处于干净稳定点：master @ 84e1b7d5 与 origin/master 同步，主工作区 clean，唯一 worktree。
- 活跃工作仅 1 项：OPEN PR #594（research/593-key-path）。
- 待审计的遗留远程分支：2 个 backup 备份引用、3 个 research 分支（其中 2 个对应已 CLOSED 未合并 PR，1 个无对应 PR）、本地 master。
- 无运行中的构建/测试/Benchmark 进程；无 /tmp 验证 worktree 残留。

UNKNOWN 项：无。全部 7 项检查均已在本会话实际执行并留存回执。
