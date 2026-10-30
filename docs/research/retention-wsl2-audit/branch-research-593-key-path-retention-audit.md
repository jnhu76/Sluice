# 分支保留审计：`research/593-key-path`（Sluice）

审计人：分支保留审计子代理（动态工作流）。日期：2026-10-29。
性质：纯审计与非破坏性验证；未修改任何产品代码，未 merge/close 任何 PR，未删除任何分支/Worktree/证据目录。
结论：**KEEP**。报告收据见 `receipts/branch-593-key-path-01…07-*.txt`（本会话实际执行的命令与输出）。
前序轮次对本 PR 内容的研究价值评估见同目录 `key-path-593-retention-value.md`（独立报告，本报告的结论与其一致并互为补充）。

## 0. Ref 拼写更正（必查项 1）

ask 给定的命令 `git rev-parse origin/origin/research/593-key-path` **在本仓库失败**
（exit 128，`unknown revision`；receipts/01）。本仓库实际存在的 ref 是
`refs/remotes/origin/research/593-key-path`——即远程 `origin`（git@github.com:jnhu76/Sluice.git）
上的分支名 `research/593-key-path`，ask 拼写多了一层 `origin/`。以下全部检查按正确拼写
执行：

```
$ git -C /home/hoo/Projects/Sluice rev-parse refs/remotes/origin/research/593-key-path
2714aa65fe4837d30f5fa3fcdb27db38c23b7136
```

完整 HEAD = `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`，**与预期值一致**。

## 1. Associated PR（必查项 2）

PR 列表（ask 提供）按 headRef+headSha 双重匹配，唯一命中：

| 字段 | 值 |
|---|---|
| PR | **#594** — research(593): E5 native key-path discovery & mechanism-elision campaign |
| state | **OPEN** |
| headRef | `research/593-key-path`（与实际分支名逐字符一致） |
| headSha | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`（与完整 HEAD 逐位一致） |
| base | master |

独立复核（receipts/02）：`gh pr view 594 --repo jnhu76/Sluice --json ...` 返回
`state=OPEN, mergedAt=null, headRefName=research/593-key-path,
headRefOid=2714aa65fe4837d30f5fa3fcdb27db38c23b7136`。列表其余 9 个 PR 无一匹配本分支。

## 2. Commit reachability（必查项 3）

- `git branch -a --contains 2714aa65…` → 仅 `remotes/origin/research/593-key-path`；
  master 不包含，无任何本地分支包含（receipts/03）。
- `git tag --contains 2714aa65…` → 空（无 tag 包含）。
- `git rev-list --count origin/master..refs/remotes/origin/research/593-key-path` → **2**：
  - `2714aa65` research(593): anchor final block to PR #594
  - `7bf55a87` research(key-path): archive #593 E5 critical-path campaign
- merge-base = `bdfd0d8d`（research(uring): archive #580 evidence — NO_TRIGGER_NO_CHANGE #592），
  分支已落后 master 多个已合并研究/性能提交。
- 文本引用（非 git 可达）：master 上 `research/direct-beyond-parity/closure/E5_CLOSURE_SNAPSHOT.md`
  （#610 归档内容）第 150、167 行以名称+完整 SHA 锚定本分支（receipts/05）。

## 3. Blob reachability（必查项 4）

- `git rev-list --objects refs/remotes/origin/research/593-key-path ^origin/master` → **72 个
  独有对象**（2 commits + 树 + 全部文件 blob）；即分支全部文件内容按 blob SHA 均不可从
  master 到达（receipts/04）。
- `git diff --name-status origin/master...branch` → 56 个文件，**全部为 `A`（新增）**：
  - `docs/research/key-path-593/` 15 篇（README、execution-dag、critical-path-atlas、
    mechanism-ledger、k1–k4 lane 报告、counterfactuals、cross-path-interactions、
    opportunity-ranking、correctness-review、measurement-review、decision、evidence-index）
  - `bench/research/key-path-593/` 41 个（README；patches/ 14 补丁 + SHA256SUMS；
    harness/ 24 脚本 + SHA256SUMS）
- 逐文件 blob 比对：56/56 = `NOT_IN_MASTER`（对每个文件
  `git cat-file -e origin/master:<path>` 全部失败；receipts/04）。结合 rev-list --objects
  的 blob 级不可达性：**内容未进默认分支，master 任何路径下也无内容等价副本**。

## 4. Unique evidence（必查项 5）

- master 的研究目录仅 `docs/research/{mw-hw-1, native-linux-m2, state-complexity,
  uring-router-580}` + `bench/research/uring-router-580`（receipts/05）；#610 归档的是
  `research/direct-beyond-parity/`（#603 P0–P6，另一战役）。`git grep key-path-593
  origin/master` 零命中 → **56 个文件未被 master 或 #610 归档目录覆盖，属独有未归档数据，
  且本分支是这些工件的唯一 in-repo 副本**。
- 其中被后续工作依赖的关键工件（详见前序报告 `key-path-593-retention-value.md` §4–§5，
  该报告在同一审计目录）：K4-b 补丁（`patches/k4b-final.diff`）及两个反事实测试源码、
  复现 harness——OPEN issue #595 明文要求以它们为改编基线并修复其中 oracle；分支删除
  即同时销毁上述唯一副本并使 #595 中 `blob/research/593-key-path/...` 引用失效。
- 原始 1.33 GiB 测量数据不在仓内（E5 主机 `/home/jnhu/sluice-593-research/evidence`，
  owner 保留，#595 W0 禁止迁移），与本分支去留无直接耦合，但分支是仓内侧链锚点。

## 5. 未推送产品变更核查

`git diff --name-only bdfd0d8d..2714aa65` 过滤掉两个研究目录后为**空**（grep exit 1，
receipts/06）→ 分支不含任何产品代码（include/src/apps/tests/xmake）变更，无未推送产品变更。

## 6. Active dependency（必查项 6）

- **OPEN PR head：是**——#594 OPEN 且 head 即本分支（§1）。DELETE_CANDIDATE 的
  “非 OPEN PR head”条件不成立。
- 仓库脚本/CI：`git grep "pull/594|research/593-key-path" origin/master -- .github scripts
  xmake apps include src tests` 零命中（receipts/06）；唯一引用在研究文档
  E5_CLOSURE_SNAPSHOT.md（文本锚定，见 §2）。
- Worktree：`git worktree list` → 本机只有主工作区（master）+ 3 个 `/tmp/sluice-val-*`
  detached 验证 worktree；**本分支未被本机任何 worktree 以分支形式检出**。其中
  `/tmp/sluice-val-k594-retention` 为本工作流的专属验证 worktree（detached @ 2714aa65）。
  #610 快照另记录 E5 主机（`/home/jnhu/...`，非本 WSL2 机器 `/home/hoo/...`）上有
  `sluice-593-research/worktrees/pr` 以本分支检出——**本机无法验证该主机当前状态**，
  如实标注为历史记录、未验证。
- 跨仓库：dududu（/tmp/dududu-audit，origin=https://github.com/jnhu76/dududu.git，
  HEAD `12a8712` = 已合并 e5-r0-verdict PR #7 之后的 main）对 `593/594/key-path-593/
  593-key-path` 全部零命中（receipts/07）→ dududu 不依赖本分支，其内亦无本分支证据的备份。

## 7. Working tree（必查项 7）

本分支未被本机任何 worktree 检出，无分支级 dirty 状态可查（N/A）。
同 SHA 的验证 worktree `/tmp/sluice-val-k594-retention` `git status --porcelain` 为空
（clean，receipts/07）。E5 主机 worktree 在 #610 快照中记录为 clean (0)（历史快照，未在本机复核）。

## 8. Recovery（必查项 8）

- 远程分支仍在：`git ls-remote origin refs/heads/research/593-key-path` →
  `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`（receipts/01）。
- GitHub PR head ref 仍在：`git ls-remote origin refs/pull/594/head` → 同一 SHA。
  只要 PR #594 存在（且 GitHub 对 PR head ref 的保留期内），即使删除分支也可
  `git fetch origin pull/594/head` 或 `git fetch origin research/593-key-path` 恢复；
  完整 HEAD SHA 已记录于本报告。
- master 的 E5_CLOSURE_SNAPSHOT.md:150/167 也以完整 SHA 记录了该分支，可溯源。

## 9. Decision

**KEEP**。判定链：

1. DELETE_CANDIDATE 直接排除——本分支是 **OPEN PR #594 的 head**（第一条必要条件
   “非 OPEN PR head”不成立）。
2. KEEP 成立——“权威或活跃”：OPEN PR head 属活跃引用；且 master 上已合并的
   #610 归档证据（E5_CLOSURE_SNAPSHOT.md）以名称+SHA 锚定本分支为 E5 战役库存的一部分。
3. 同时存在**独有未归档研究数据**（56 文件、唯一 in-repo 副本、#595 依赖）——该条件
   本应指向 ARCHIVE_FIRST，但由于数据已在 OPEN PR #594 中提案入 trunk，正确的保全
   路径是**先解决 PR**（合并 → 内容入 master；无合并关闭 → 届时先归档再清理），
   而非本阶段单独归档后删除。若 #594 之后被无合并关闭，应重审为 ARCHIVE_FIRST。

本阶段仅出结论，未执行任何删除。
