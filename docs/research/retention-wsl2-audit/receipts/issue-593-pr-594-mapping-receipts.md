# Issue #593 ↔ PR #594 职责映射审计 — 命令凭据

审计时间：2026-10-29（本会话）。执行者：交付映射审查员（子代理）。
本文件只记录取证命令与关键输出；结论在 submit_result 中。全部操作只读（唯一的写操作是本 receipts 目录）。

## 1. 工具可用性

```
$ which gh && gh --version && gh auth status
/usr/bin/gh ; gh version 2.46.0 ; logged in as jnhu76 (scopes: repo, workflow, ...)
```

## 2. Issue #593 原文

- `gh issue view 593 --repo jnhu76/Sluice --comments` 失败：
  `GraphQL: Projects (classic) is being deprecated ... (repository.issue.projectCards)`（退出码 1）。
- 改用 `gh issue view 593 --json number,title,state,createdAt,updatedAt,author,labels,milestone,body` 成功：
  - title: `research(perf/method): PocketJS-inspired execution-path slicing and deterministic replay pilot`
  - state OPEN；createdAt `2026-10-25T08:26:11Z`；updatedAt `2026-10-25T21:05:25Z`
  - body 规定 R0–R4 工作流、F/O/R/C 模式、§7 验收证据、closeout 词汇
    `METHOD_EARNED | PARTIAL_OBSERVATION_ONLY | REPLAY_NOT_ADMITTED | NO_EXTRA_DIAGNOSTIC_VALUE | BLOCKED_CORRECTNESS`。

## 3. #593 评论与时间线

```
$ gh api repos/jnhu76/Sluice/issues/593/comments --paginate ...
→ 仅 1 条评论 id=6403266258 user=jnhu76 created=2026-10-25T21:05:25Z
  （"Research Checkpoint — #593 E5 native key-path campaign"，报告 K1–K4 判定，brief §13 词汇）

$ gh api repos/jnhu76/Sluice/issues/593/timeline --paginate
→ 共 11 个事件：1 commented + 10 cross-referenced（来源 594/595/596/597/598/599/563/600/578/609）
→ 0 个 edited / renamed / closed 事件（正文与标题从未改动）
```

## 4. PR #594

```
$ gh pr view 594 --repo jnhu76/Sluice --json ...
→ title: research(593): E5 native key-path discovery & mechanism-elision campaign
→ state OPEN, MERGEABLE, head 2714aa65fe4837d30f5fa3fcdb27db38c23b7136
→ headRefName research/593-key-path, base master
→ createdAt 2026-10-25T21:04:56Z, updatedAt 2026-10-25T21:08:19Z
→ body: "This implements the #593 K1–K4 campaign"；判定用 "brief §13 vocabulary"

$ gh api repos/jnhu76/Sluice/issues/594/timeline （filtered edited/renamed/closed/reopened）
→ 空（PR 正文从未编辑，未被关闭/重开）

$ gh api repos/jnhu76/Sluice/issues/594/comments
→ 仅 CodeRabbit bot 汇总评论（id=6403261926），无人工评审评论

$ gh api "repos/jnhu76/Sluice/pulls/594/files?per_page=100" --paginate
→ 56 个文件，全部 added，全部位于 docs/research/key-path-593/ 与 bench/research/key-path-593/

$ gh api "repos/jnhu76/Sluice/pulls/594/commits"
→ 7bf55a87 2026-10-25T20:20:32Z research(key-path): archive #593 E5 critical-path campaign
→ 2714aa65 2026-10-25T21:08:06Z research(593): anchor final block to PR #594
```

## 5. 分支内容词汇核验（git grep @2714aa65，只读）

```
#593 专属词汇在 #594 树中的命中数：
  PocketJS: 1（仅 README.md 上下文指针 "Issue: #593 (PocketJS-inspired execution-path slicing)"）
  R0 / REQUEST_REPLAY / COMPLETION_REPLAY / REPLAY_NOT_ADMITTED /
  METHOD_EARNED / PARTIAL_OBSERVATION_ONLY / NO_EXTRA_DIAGNOSTIC_VALUE /
  "feasibility matrix" / MutationTape / "logical corpus": 全部 0 命中
  "replay"（不区分大小写，docs/research/key-path-593）: 0 命中
  SLUICE_OBSERVE: README.md / counterfactuals.md / mechanism-ledger.md（observe 二进制存在，
  属 R2 类似的 measure/observe 分离，但无 R0/R1/R3/R4 对应物）

#595 引用数字在 #594 树中的存在性（全部命中）：
  0.50017 / 0.12577 / 0.03447（K4-b enters/op）→ counterfactuals、atlas、cross-path、k4 文档等
  3.098（K1 acquisitions/op）、4.79（campaign band）→ 多文件
  k4_residue_wake_test、k4_retry_residue_wake_test → correctness-review/decision/k4/measurement-review
  + 已提交补丁 k4b-final.diff、combo-k4b-k1ab.diff 等
```

## 6. brief 位置

```
docs/research/key-path-593/evidence-index.md §1/§3（@2714aa65）:
  Research drafts = /home/jnhu/sluice-593-research/mapping 和 brief.md
  brief.md SHA-256 = c750a314807934b696d9e109f7545da7f62115c6ff58647dba59b587303bc198
→ brief.md 不在 #594 的 56 个文件中，未提交入库。

$ gh search issues --repo jnhu76/Sluice "ELISION_PERF_SUPPORTED" → 仅 #593（其评论）
$ gh search issues --repo jnhu76/Sluice "key-path campaign brief" → 无结果
#563 body 扫描 K[1-4]/key-path/brief/elision → 无命中（#563 不承载 brief）
```

## 7. E5 原始证据可达性

```
$ whoami → hoo
$ ls /home/jnhu/sluice-593-research → No such file or directory
→ 本会话（WSL2, 用户 hoo）无法验证 1.4 GB 原始证据；brief.md 原文亦不可读。取证局限。
```

## 8. master / #610 归档对照

```
$ git ls-tree master docs/research/ bench/research/
→ 无 key-path-593 目录（仅有 mw-hw-1, native-linux-m2, state-complexity, uring-router-580 等）
$ git log --all -- docs/research/key-path-593 bench/research/key-path-593
→ 仅 #594 分支的两个提交（2714aa65, 7bf55a87）
$ git diff --stat bdfd0d8d..2714aa65 → 56 files changed, 12245 insertions(+)
$ git diff --name-only bdfd0d8d..2714aa65 | grep -v '^…key-path-593/' | wc -l → 0
$ gh api pulls/610/files → 819 个文件全部位于 research/direct-beyond-parity/（grep -c key-path → 0）
$ git ls-tree master research/ → 含 direct-beyond-parity（#610 已并入）与 600-direct-excess-cost
→ #594 的全部 56 个文件在 master 与 #610 中均不存在，是该 OPEN PR 的独有内容。
```

## 9. #595 与 #610 原文

- `gh issue view 595 --json`：#595 自述 "convert the independently measured #593 K2/K4 submission gap"，
  `Evidence: #593 / research PR #594`，引用 K4-b/K2-a/K1/K3 数字与 #594 的两个失败 oracle（§4 "must be repaired"），
  W0 要求不动 /home/jnhu/sluice-593-research/evidence/，§7 要求新 workspace 存放新原始数据。
- `gh pr view 610 --json`：MERGED 2026-10-29T05:30:53Z；归档 #603 的 research/direct-beyond-parity/（613+manifest）；
  base bdfd0d8d（与 #594 同一锚点）；与 key-path-593 无交集。
