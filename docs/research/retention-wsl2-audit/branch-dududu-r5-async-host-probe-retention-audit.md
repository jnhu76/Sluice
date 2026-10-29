# 分支保留审计：`r5-async-host-probe`（dududu）

审计人：分支保留审计子代理（动态工作流）。日期：2026-10-29。
性质：纯审计与非破坏性验证；未修改任何产品代码，未 merge/close 任何 PR，未删除任何
分支/Worktree/证据目录。本阶段只出结论，不执行删除。
结论：**DELETE_CANDIDATE**。收据见 `receipts/dududu-r5-host-probe-01…04-*.txt`
（本会话实际执行的命令与输出）。

## 0. 审计环境

- dududu 本地副本：`/tmp/dududu-audit`（已存在，origin = `https://github.com/jnhu76/dududu.git`，
  默认分支 `main`）。审计前先 `git -C /tmp/dududu-audit fetch origin`（exit=0）刷新引用。
- Sluice 仓库（`/home/hoo/Projects/Sluice`）仅用于报告落盘与 worktree 扫描，不涉及本分支。

## 1. HEAD 核对（必查项 1）

```
$ git -C /tmp/dududu-audit rev-parse origin/r5-async-host-probe
4eafbc06d6a2e476055fe45fc75656e7e28a2183
$ git -C /tmp/dududu-audit ls-remote origin r5-async-host-probe main
12a8712eb5ff5f48e570b49b1e01ad5821837dca	refs/heads/main
4eafbc06d6a2e476055fe45fc75656e7e28a2183	refs/heads/r5-async-host-probe
```

完整 HEAD = `4eafbc06d6a2e476055fe45fc75656e7e28a2183`，与预期值**一致**；远程引用与
本地 remote-tracking 引用逐位一致。receipts/01。

## 2. Associated PR（必查项 2）

ask 提供的 10 条 PR 列表按 headRef 匹配 `r5-async-host-probe`：**无命中 → None**。
dududu 侧两条 PR 分别是 #7（headRef `sluice-r0-e5-verdict`，MERGED）与 #8（headRef
`r1-request-geometry`，OPEN），均非本分支。本分支未以 PR 形式进入 main（见 §3，其
提交以线性历史直接位于 main 之中）。

## 3. Commit reachability（必查项 3）

- `git rev-list --count main..origin/r5-async-host-probe` → **0**（零独有提交）。
- `git merge-base main origin/r5-async-host-probe` → `4eafbc06…`（= 分支 tip 本身）；
  `git merge-base --is-ancestor origin/r5-async-host-probe main` → **true**。即分支
  tip 是 main 的祖先，分支已完全包含于默认分支。
- `git branch -a --contains 4eafbc06…` → `main`、`origin/HEAD`、`origin/main`、
  `origin/r1-request-geometry`、`origin/r5-async-host-probe`、`origin/sluice-r0-e5-verdict`
  （receipts/02）。引用该 tip 的权威 ref：`origin/main`（12a8712e）、
  `origin/r1-request-geometry`（47cd1976，OPEN PR #8 head）、`origin/sluice-r0-e5-verdict`
  （cc321a49，MERGED PR #7 head）。删除 `r5-async-host-probe` 引用不影响其余三者——
  它们各自独立包含这些提交。
- 分支段的四个提交（`bbce768` fio r0 基线冻结、`732a8fb`/`cc053cf`/`4eafbc0` duckdb
  async-host probe）均以线性历史直接出现在 `git log main` 中（receipts/02）。

## 4. Blob reachability（必查项 4）

- `git rev-list --objects origin/r5-async-host-probe ^main` → **0 个独有对象**（空输出）。
- `git diff --name-only main...origin/r5-async-host-probe` → **0 个文件**（空 diff，
  merge-base = tip，无需逐文件比对；等价地，全部内容已进默认分支）。
- 直接验证：分支段新增的 9 个文件（`benchmarks/duckdb-r0/`、`benchmarks/fio-r0/`）
  逐一 `git cat-file -e origin/main:<path>` → **9/9 IN-MAIN**（receipts/03）。

## 5. Unique evidence（必查项 5）

- 分支自身产物：`benchmarks/duckdb-r0/`（async-host probe C++ 源、oracle.sql、prep
  receipt）与 `benchmarks/fio-r0/`（fio job set）——全部已在 origin/main 中（§4），
  且 main 自身在 `docs/DUCKDB_FIO_R0_PREREG.md` 中以名称 + SHA 锚定了该战役
  （见 §6）。**无独有未归档数据**；dududu main 即权威备份（远程 GitHub 仓库本体）。
- Sluice 侧 #610 归档目录覆盖的是 #603 P0–P6（另一仓库另一战役），与 dududu 本分支
  无重叠，亦无需要经由本分支保全的内容。

## 6. Active dependency（必查项 6）

- **OPEN PR head：否**。dududu 唯一 OPEN PR #8 的 head 是 `r1-request-geometry`
  （47cd1976），不是本分支；该 head 包含 4eafbc0 为祖先，但删除本分支引用不影响
  #8 的 head ref。
- **脚本/CI 引用：无**。`git grep -rn "r5-async-host-probe" origin/main` 全树仅 4 处
  命中：3 处为文档溯源性文字引用，1 处为 probe 自身的运行期临时文件路径（见下方
  另注，与分支引用无关）（receipts/04）。文档引用：
  - `benchmarks/duckdb-r0/async-host-probe/README.md:40`（"dududu: branch
    `r5-async-host-probe` (based on `sluice-r0-closure`)"）
  - `docs/DUCKDB_FIO_R0_PREREG.md:225`（"branch `r5-async-host-probe` @ `4eafbc0`"，
    已同时锚定完整可取回的 SHA）与 `:300`（战役表行）。
  这些不是可执行依赖；删除引用后文字指向的 SHA 仍可从 main 历史到达。另注：
  `async_host_probe.cpp:405` 的 `/tmp/sluice-r5-async-host-probe.bin` 是 probe 的运行期
  临时文件路径，与分支名同形但与分支去留无关。
  `.github/` 仅 `workflows/repo-policy.yml`，无命中。
- **Worktree 检出：无**。`git -C /tmp/dududu-audit worktree list` 仅
  `/tmp/dududu-audit [main]`；克隆内无本地分支 `r5-async-host-probe`；
  `ls /home/hoo/Projects/` 与 `/tmp` 扫描无其他 dududu 检出（receipts/04）。

## 7. Working tree（必查项 7）

分支未被任何 worktree 检出 → dirty 检查不适用（N/A）。唯一的 dududu 工作树
（/tmp/dududu-audit）检出的是 main 且 `git status --porcelain` 为空（receipts/04）。

## 8. Recovery（必查项 8）

- 完整 HEAD SHA 已记录：`4eafbc06d6a2e476055fe45fc75656e7e28a2183`；本会话
  `ls-remote` 确认其仍在远程，`git fetch origin` exit=0 证明可 fetch（receipts/01）。
- **即使删除分支引用也不丢失任何对象**：tip 是 `origin/main`（12a8712e）的祖先，
  同时被 `origin/r1-request-geometry`、`origin/sluice-r0-e5-verdict` 包含（receipts/02）。
  恢复步骤（任一即可）：
  - `git fetch origin && git branch r5-async-host-probe 4eafbc06d6a2e476055fe45fc75656e7e28a2183`；
  - `git push origin 4eafbc06d6a2e476055fe45fc75656e7e28a2183:refs/heads/r5-async-host-probe`。
- 溯源：无专属 PR（None）；main 内 `docs/DUCKDB_FIO_R0_PREREG.md:225` 已锚定
  `4eafbc0`，四个分支段提交可经 `git log main` 直接检索。

## 9. 决策

DELETE_CANDIDATE 条件逐项核对：

| 条件 | 结果 |
|---|---|
| 非 OPEN PR head | ✅ 无关联 PR（None）；OPEN PR #8 head 为其他分支 |
| 无独有未归档数据 | ✅ 0 独有提交、0 独有对象、空 diff、9/9 文件 IN-MAIN |
| 无未推送产品变更 | ✅ 远程 ref 与本地 remote-tracking 逐位一致，无本地独有提交 |
| 相关原始数据已有可靠备份 | ✅ 全部内容在远程 origin/main（GitHub jnhu76/dududu） |
| 无运行中 Agent/Worktree 依赖 | ✅ 无 worktree 检出、无本地分支、脚本/CI 零引用（仅文档溯源性提及，非阻塞） |
| 已记录完整 HEAD SHA 与恢复步骤 | ✅ 见 §1 与 §8 |

所有条件成立 → **DELETE_CANDIDATE**。本轮不执行删除。

## 10. 独立复核（第二轮，2026-10-29）

复核人对最要命的三个字段（uniqueEvidence / activeDependency / recovery）做了独立
重验：重新 `git -C /tmp/dududu-audit fetch origin --prune`（exit=0）后重跑全部关键
命令。收据见 `receipts/dududu-r5-host-probe-05…08-reval-*.txt`（本会话实际执行的
命令与输出）。

- **HEAD/远程基准**（receipts/05）：`git ls-remote origin refs/heads/r5-async-host-probe
  refs/heads/main` → `4eafbc06d6a2e476055fe45fc75656e7e28a2183` / `12a8712eb5ff5f48e570b49b1e01ad5821837dca`，
  与原审计逐位一致；`rev-list --count origin/main..origin/r5-async-host-probe` → 0；
  merge-base = tip；`--is-ancestor` 对 `origin/main`、`origin/r1-request-geometry`、
  `origin/sluice-r0-e5-verdict` 三条 ref 全部 exit=0；`git log --oneline -6` 确认
  分支段 4 提交（bbce768/732a8fb/cc053cf/4eafbc0），`tip~4` = `b94041d918672d113…`。
- **uniqueEvidence 复核通过**（receipts/06）：`git rev-list --objects
  origin/r5-async-host-probe ^origin/main` → **0 行**（0 个独有 commit/tree/blob，
  覆盖分支 tip 全部对象闭包）；分支段 diff `--name-status b94041d..4eafbc0` 恰
  9 个 `A` 文件、0 个 M/D/R 文件；**逐文件 blob SHA 比对**（比原审计的
  `cat-file -e` 存在性检查更强）→ **9/9 MATCH**：每个文件在分支 tip 与
  `origin/main` tip 的 blob SHA 逐位一致（例：`async_host_probe.cpp`
  blob=`a6cdfc0f979e03fa2ae813f20ca4e2e72d9a3b84` 两侧相同）。main 侧无该分支
  独有内容的任何载体。
- **activeDependency 复核通过**（receipts/07）：GitHub API 实测 dududu 全部 5 条
  PR（#1/#2/#3/#7 MERGED，#8 OPEN head `r1-request-geometry@47cd1976`），
  `headRefName=r5-async-host-probe` 命中数 0（`gh pr list --head
  r5-async-host-probe --state all` 空输出）；`git grep -n "r5-async-host-probe"
  origin/main` 全树恰 4 处命中，与原审计一致（3 处文档溯源 +
  `async_host_probe.cpp:405` 运行期 `/tmp` 路径），无脚本/CI 引用；
  `.github/` 仅 `workflows/repo-policy.yml` 且零命中；`worktree list` 仅
  `/tmp/dududu-audit [main]`，克隆无本地分支，`status --porcelain` 空，
  `stash list` 空；`/tmp` 下唯一其他 dududu 相关文件
  `/tmp/r0e5-prereg-dududu.md` 为非 git 的纯文本 prereg 草稿，非检出。
- **recovery 复核通过**（receipts/08）：记录的 SHA 与远程活引用逐位一致
  （ls-remote）；`cat-file -t` → `commit`，`cat-file -p` 首行
  `bench(duckdb): probe async host protocol boundary` 证实即被审计 tip；tip 被
  `origin/main`、`origin/r1-request-geometry`、`origin/sluice-r0-e5-verdict`
  三条 ref 独立包含 → 删除本分支 ref 后对象仍可达。文档化恢复命令的两个前提
  （对象存在且可 fetch、删除后三条 ref 冗余可达）均已直接验证；**恢复命令本身
  未执行**——执行即会在克隆内创建 ref，违反本轮非破坏性纪律，此为如实标注
  而非缺口。

三项复核全部成立，无需放宽、无 UNKNOWN 项 → **维持 DELETE_CANDIDATE**。
