# 分支保留审计（第 2 轮独立复核）：`research/593-key-path`（Sluice）

审计人：分支保留审计子代理（动态工作流，run 标签
`分支审查·sluice/research/593-key-path`）。日期：2026-10-29。
性质：纯审计与非破坏性验证。本轮未修改任何产品代码，未 merge/close 任何 PR，
未删除任何分支/Worktree/证据目录，未 force push。唯一写操作为本报告目录及
`receipts/branch-593-key-path-r2-*.txt`。另执行过一次 `git fetch origin --prune`
（exit 0，无任何 ref 变化，非破坏性）。

**结论：KEEP。**

第 1 轮同对象报告见同目录 `branch-research-593-key-path-retention-audit.md`
（结论一致：KEEP）；研究价值独立评估见 `key-path-593-retention-value.md`；
E5 原始数据保全约束见 `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md`
（`E5_RAW_ARCHIVE_PENDING`）。本报告全部检查为本会话实际执行，收据为
`receipts/branch-593-key-path-r2-01…06-*.txt`。

## 0. Ref 拼写更正

ask 标题写 `sluice/research/593-key-path`；仓库中实际存在的 ref 是
`refs/remotes/origin/research/593-key-path`（远程 `origin` =
`git@github.com:jnhu76/Sluice.git`）。`origin/sluice/research/593-key-path`
不存在（`git rev-parse` exit 128，receipt r2-01）。`sluice/` 前缀是审计标签
的仓库语境前缀，非 ref 名。以下全部按真实 ref `origin/research/593-key-path` 执行。

## 1. HEAD 解析（必查项 1）

```
$ git rev-parse origin/research/593-key-path
2714aa65fe4837d30f5fa3fcdb27db38c23b7136
```

在 `git fetch origin --prune`（exit 0，无输出 = ref 无变化）之后重新解析，
值不变。receipt r2-01。

## 2. Associated PR（必查项 2）

按 ask 提供的 PR 列表按 headRef+headSha 匹配：**PR #594，state=OPEN**，
title "research(593): E5 native key-path discovery & mechanism-elision campaign"，
headRef `research/593-key-path`，headSha `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`
——与本地解析的分支 HEAD **逐位一致**。远端佐证：
`git ls-remote origin refs/pull/594/head` → `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`
（receipt r2-01）。

## 3. Commit reachability（必查项 3）

- `git branch -a --contains 2714aa65…` → **仅** `remotes/origin/research/593-key-path`。
  无本地分支、无 master、无 tag 包含该提交（receipt r2-02）。
- `git rev-list --count origin/master..origin/research/593-key-path` → **2**：
  - `2714aa65` research(593): anchor final block to PR #594（2026-10-26 05:08 +0800）
  - `7bf55a87` research(key-path): archive #593 E5 critical-path campaign（2026-10-26 05:01 +0800）
- merge-base = `bdfd0d8d`（2026-10-25）；master 自此前进 **5** 个提交
  （#600/#601/#610/#612 等 direct 系列后续工作）。分支落后于 master 但内容
  纯增量、无冲突性产品改动（见 §4）。

## 4. Blob reachability（必查项 4）

- `git diff --stat origin/master...origin/research/593-key-path` →
  **56 files changed, 12245 insertions(+), 0 deletions**。全部位于
  `docs/research/key-path-593/`（15 个文档）与 `bench/research/key-path-593/`
  （41 个 harness/patch 文件）。**无任何 `src/`、`include/`、`apps/`、
  `xmake*` 改动 → 不存在未推送产品变更**。
- `git rev-list --objects origin/research/593-key-path ^origin/master` →
  独有对象 = 2 个提交 + 树对象 + 56 个 blob（receipt r2-03 列出全部 56 个
  blob SHA）。
- **逐文件 blob 比对**：56/56 个 blob 在 origin/master 全树（任意路径）中
  **均不存在**（`comm -23` 计数 = 56）；两目录路径在 master 上也不存在
  （`git ls-tree origin/master -- <两目录>` 为空）。receipt r2-03。

## 5. Unique evidence（必查项 5）

**未被 master 或 #610 归档目录覆盖。**

- #610 归档在 master 的 `research/direct-beyond-parity/`，内容是 #603/#600
  direct 系列证据；其中 `closure/E5_CLOSURE_SNAPSHOT.md:150,167` 仅以
  **名称+SHA 引用**本分支（inventory 记录），不包含其内容。
- master `research/600-direct-excess-cost/PHASE_I_COST_ATTRIBUTION.md:41`
  复用了判定词汇 `ELISION_CANDIDATE_FOUND`（#600 自己的 verdict），属词汇
  复用而非 #593 内容整合。
- 分支自身内容（receipt r2-06）：README 自述 "research record intended as an
  open research PR"、"PRODUCT_CHANGE: NONE"；decision.md 记录 K2 =
  `ELISION_CANDIDATE_FOUND`（−32%/−38%/−26% 同线程 CPU，但 arm 未门控）、
  K4 = `ELISION_PERF_SUPPORTED`（D=4/16/64 门控，−28%/−36%/−13% CPU）——
  **存在已测得支撑的 elision 候选发现**，未进入 master 任何文档。
- **更关键**：evidence-index.md 明示 "Raw measurement data is **not** committed
  to the repository"——1.33 GiB / 6,870 文件的原始证据在 E5 主机
  `/home/jnhu/sluice-593-research/evidence`，**不在 git 内**。该主机从本
  WSL2 主机不可达（`ls /home/jnhu` → No such file or directory，receipt
  r2-04）；前序轮次 `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md` 确认其
  "never been hash-anchored in full"，状态 `E5_RAW_ARCHIVE_PENDING`，并明确
  要求在原始证据归档验证完成前 **不得 close/删除 PR #594 或改写其 head**。
  即：本分支是 #593 战役记录（文档+harness+patch+SHA256SUMS 清单）在 git 中
  的**唯一载体**。

## 6. Active dependency（必查项 6）

- **OPEN PR head：是** —— PR #594 OPEN，head 即本分支（§2）。这已单独
  排除 DELETE_CANDIDATE。
- 仓库脚本/CI：`git grep "593\|research/" origin/master --
  .github/workflows/ci.yml .github/workflows/open-code-review.yml` 无输出；
  分支自带 bench harness 自述 "not wired into CI"（bench README）。receipt r2-05。
- 文档依赖：master 归档 `E5_CLOSURE_SNAPSHOT.md` 以名称+SHA 引用本分支
  （receipt r2-05）。
- Worktree：本地 `git worktree list` 无任何 worktree 检出该分支（只有
  `/tmp/sluice-val-k594-retention` **detached** 于 2714aa65）。closure 快照
  记录 E5 主机上有 worktree `/home/jnhu/sluice-593-research/worktrees/pr`
  @ 2714aa65（分支检出，clean）——本机不可达，无法验证其当前状态（UNKNOWN，
  不猜测）。

## 7. Working tree（必查项 7）

- 本地无 worktree 检出该分支（§6），无需 dirty 检查。
- detached 验证 worktree `/tmp/sluice-val-k594-retention` @ 2714aa65：
  `git status --porcelain | wc -l` = **0（clean）**，且 detached，不受分支
  删除影响（receipt r2-04）。
- E5 主机 worktree 状态：UNKNOWN（不可达；最近记录为 clean）。

## 8. Recovery（必查项 8）

完整 HEAD SHA：`2714aa65fe4837d30f5fa3fcdb27db38c23b7136`。三层恢复锚点均已
在本会话验证（receipt r2-01）：

1. 远端分支仍在：`git ls-remote origin research/593-key-path` →
   `2714aa65…  refs/heads/research/593-key-path`。恢复：`git fetch origin
   research/593-key-path`。
2. PR pull ref（即使分支 ref 被删、PR #594 存续期间有效）：
   `git ls-remote origin refs/pull/594/head` → `2714aa65…`。恢复：
   `git fetch origin pull/594/head && git branch research/593-key-path FETCH_HEAD`。
3. 本地对象库（分支 ref 删除后、GC 前）：`git branch research/593-key-path
   2714aa65fe4837d30f5fa3fcdb27db38c23b7136`。
   另 PR #594 的 headSha 字段本身即可溯源到该 SHA。

## 9. Decision: KEEP

DELETE_CANDIDATE 六条件全部满足才可删；此处至少三项不成立：

1. **是 OPEN PR head**（#594）→ 第一条即否决；
2. **有独有未归档研究数据**（56/56 blob 不在 master；K2/K4 elision 候选
   发现未整合）→ 本应触发 ARCHIVE_FIRST，但分支当前活跃且承担打开中的 PR，
   归档动作由 OPEN PR 的正常评审/合并流程承载，无需先行旁路归档；
3. **原始数据备份不可验证**：1.33 GiB 原始证据仅在不可达的 E5 主机、未经
   完整哈希锚定，且前序轮次保全计划（`E5_RAW_ARCHIVE_PENDING`）明确禁止
   在归档验证前 close/删除本 PR 与分支。

其余条件（无未推送产品变更——成立；HEAD SHA 与恢复步骤已记录——成立）不足以
改变结论。本轮不执行任何删除。

## 10. 本轮 UNKNOWN 项

- E5 主机 `/home/jnhu/sluice-593-research`（worktree + 1.33 GiB 原始证据）的
  当前状态：本机不可达，未验证。不属于本机可判范围，已如实标注。
