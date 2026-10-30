# INTEGRATION_PLAN.md — E5 Product Integration（Phase 0 产出）

- 生成时间：2026-10-29（Phase 0 执行时实时读取）
- 集成 worktree：`/home/jnhu/Source/Sluice-integration`，分支 `integration/e5-product-merge` @ `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`
- 基准：Sluice `origin/master = bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`；dududu `origin/main = b94041d918672d113ea0ca667ba8ccdabf329c80`
- 本文件是操作前计划；所有状态在执行各 Phase 时必须重读（Head SHA Guard）。

## 1. 合并约定（依据实证）

- **Sluice = squash（现行）**。证据：
  - 最近 4 个 PR 合并 #585/#586/#587/#592（2026-10-24 之后）在 `git log --first-parent origin/master` 中均为单父提交、subject 带 `(#NNN)`、committer=GitHub（`git cat-file -p bdfd0d8d` 仅 1 个 parent）。
  - `gh api repos/jnhu76/Sluice/pulls/592` → `merge_commit_sha=bdfd0d8d…`（=master 顶端单提交）。
  - 历史（≤#583，2026-10-23 前）为双亲 merge commit（`git cat-file -p 3df10dc4` 有 2 个 parent），已不再使用。
- **dududu = merge commit**。证据：`git log --first-parent origin/main` 全部为 "Merge pull request #N"；`git cat-file -p b94041d` 有 2 个 parent。

含义：Sluice 上 #602 squash 合并后，C1 的原始 commit **不会**成为 master 祖先 → stacked PR #612 必须按 §4 重锚。dududu #7 用 merge commit 方式合并。

## 2. 分支保护（实测）

- 两仓库均无 Branch Protection / Rulesets：
  - `gh api repos/jnhu76/Sluice/branches/master/protection` → 404 "Branch not protected"；`rulesets` → `[]`；GraphQL `branchProtectionRules` → `nodes: []`。
  - dududu `main` 同样 404 / `[]`。
- 因此 **没有 GitHub required checks**；全部门禁为章程程序性门禁（Head SHA Guard、证据核验、CI 通过），执行时不得把 "MERGEABLE" 当作 "门禁通过"。

## 3. PR 实时快照（2026-10-29 Phase 0 实读）与职责

| PR | head 分支 | head SHA（Guard 值） | base | 状态 | mergeable | CI（实读） | unresolved review threads |
|---|---|---|---|---|---|---|---|
| Sluice #601 C1 Research Evidence | `sluice-600-direct-research` | `2fcc79646ee42d1699468e0a50c0269f405b92d3` | master | OPEN | MERGEABLE/CLEAN | 仅 CodeRabbit skipping；无 Actions | 0 |
| Sluice #610 Direct Beyond-Parity Archive | `research/direct-beyond-parity-evidence` | `d40a811dae783837c4749718a2a17f895d46d117` | master | OPEN | MERGEABLE/CLEAN | 仅 CodeRabbit skipping；无 Actions | 0 |
| Sluice #602 C1 Product（Owner 已批准采纳） | `sluice-600-direct-c1` | `77e5399a45998dea396699fa2e063c94a665945a` | master | OPEN | MERGEABLE/CLEAN | 6/6 pass（comment guard/asan-oracle/debug/release/package verifier/CodeRabbit） | 0 |
| Sluice #611 AC-1 Evidence（仅去重关闭） | `research/r6-ac1-vectored-semantics` | `999b5305ae7d413a2a71acde926f8c6cfef6876a` | master | OPEN | MERGEABLE/CLEAN | 仅 CodeRabbit skipping | 0 |
| Sluice #612 P2C1 Product（条件合并） | `sluice-608-p2c1-read-compose` | `70d48f5eb55a664544b5160cd4675d32150bb21e` | **sluice-600-direct-c1**（stacked） | OPEN | MERGEABLE/CLEAN | **无 GitHub Actions**（仅 CodeRabbit skipping） | 0 |
| dududu #7 fio/DuckDB R0 | `sluice-r0-e5-verdict` | `cc321a4994ee638493fecda0c437481528f5cea9` | main | OPEN | MERGEABLE/CLEAN | Repository Policy PASS + CodeRabbit skipping | 0 |

职责与包含关系：

- **#601**：仅 `research/600-direct-excess-cost/` 5 个文件（实读 diff 证实），C1 研究证据。与 #602/#610 零文件交集。
- **#610**：`research/` 下 819 个文件，#603 P0–P6 权威研究归档（含 EVIDENCE_MANIFEST 818 条）。与 #601/#602 零交集。
- **#611**：34 个 AC-1 文件，路径与 #610 完全重叠（`comm -12` 计 34/34）。**不合并**，Phase 1.3 逐文件核验后 SUPERSEDED_BY_PR610 关闭。
- **#602**：产品代码 5 文件：`docs/roadmap/v2-conformance.md`、`include/sluice/detail/file_semantics.hpp`、`src/blocking_file.cpp`、`tests/semantic_range_test.cpp`、`xmake/tests.lua`。C1 产品实现。
- **#612**：相对其 base `77e5399a` 自身 4 个 commit（`42f5eef9` test mutation seams → `cc8ad2ab` perf prevalidated steps → `b12fb551` refactor error outcomes → `70d48f5e` docs ledger），产品文件 5 个。**stacked 于 #602 分支**；不含 P2C2。
- **dududu #7**：R0 研究（fio/DuckDB 冻结基线），独立于 Sluice。

## 4. 合并顺序与每次合并后 master 预期变化

顺序：**#601 → #610 → #602 →（条件性，仅当章程 §3.5 全满足）#612 重锚后合并**；dududu #7 独立执行（互不依赖，可并行）。

Sluice 用 squash，合并结果 commit SHA 不可预知；以下用 **tree hash** 作为确定性预期（squash 后 master 树 == PR head 树叠加先行合并；已用 `git commit-tree` + `git merge-tree --write-tree` 模拟验证，merge-base 均为 `bdfd0d8d`）：

| 动作 | 方式 | Head SHA Guard（合并前一刻必须等于） | 预期 master 树（合并后 `git rev-parse origin/master^{tree}`） |
|---|---|---|---|
| A1 合并 #601 | `gh pr merge 601 --squash` | `2fcc79646ee42d1699468e0a50c0269f405b92d3` | `96118fa4c01864c7ee39bb5f78b06b3465603dc9`（= #601 head tree） |
| A2 合并 #610 | `gh pr merge 610 --squash` | `d40a811dae783837c4749718a2a17f895d46d117` | `59e6cc30bfa2014fb80e6feb52d6a0eb4098c49d`（#610 树 + #601 的 5 文件；无交集故为并集） |
| A3 合并 #602 | `gh pr merge 602 --squash` | `77e5399a45998dea396699fa2e063c94a665945a` | `f29f0bd753b8df31d8753eda6cc5593b8bf571db`（A2 树 + #602 的 5 文件） |
| A4（条件）#612 重锚后由 GitHub 合并 | 按届时仓库约定 | 重锚后新 head（届时实读） | 届时模拟计算并记录 |
| A5 合并 dududu #7 | `gh pr merge 7 --merge`（仓库约定 merge commit） | `cc321a4994ee638493fecda0c437481528f5cea9` | `731e518af903fe49beb798d80d269eeaceb53794`（= #7 head tree；main 是 head 祖先） |

每个动作的前置门禁（缺一即停，不伪造）：
- A1：Phase 1.1 全部证据核验通过；无阻断 review（已确认 0）。
- A2：Phase 1.2 Manifest/SHA256/819 文件核验通过；无秘密；原始结果未改。
- A3：Phase 2 全部 Correctness/CI/独立复审通过；最终 CI 为绿。
- A4：**仅当** §3.5 全部条件满足（CI 全绿含新触发的 Actions、双独立复审 APPROVE、ABI 不变、无材料回归、性能文案限定两形状、comp-read-4k 保持 INCONCLUSIVE）。
- A5：Phase 5 证据核验 + 受控 Smoke 通过。

## 5. 可能冲突的文件

- A1/A2/A3 相互之间：**零交集，无冲突**（实测 `comm -12` 均为 0）。
- **#612 × #602（重锚冲突源）**：`docs/roadmap/v2-conformance.md`、`include/sluice/detail/file_semantics.hpp`、`src/blocking_file.cpp`、`xmake/tests.lua`（#612 的 `tests/direct_composition_fault_test.cpp` 为新增，不冲突）。#602 squash 合并后这 4 个文件在 master 与 #612 分支上分叉，cherry-pick 时需手工融合，且 `tests/semantic_range_test.cpp`（#602 新增）与 `tests/direct_composition_fault_test.cpp`（#612 新增）都要在 `xmake/tests.lua` 中共存。
- 主工作树未跟踪 `research/direct-beyond-parity/` 与 #610 将合入的 `research/direct-beyond-parity/` 内容同名：**合并不受影响**（git 不覆盖未跟踪文件以外的东西——merge 在远端进行；本地 worktree 拉取 master 时若同路径未跟踪文件存在，fetch/pull 会报 untracked overwrite。集成 worktree 是干净 checkout，无此问题；主工作树不做 pull）。

## 6. #612 重锚方法（squash 情形，章程 §3.1）

前置事实：Sluice 现行 squash ⇒ #602 合并后 `77e5399a…` 不在 master 历史中，#612 的 PR diff 将重新包含全部 C1，违反"最终 #612 只携带 P2C1 自身"目标。

步骤：
1. **先备份**：`git push origin 70d48f5eb55a664544b5160cd4675d32150bb21e:refs/heads/backup/pr-612-original-head-70d48f5e`（命名沿用既有 `backup/pr-601-original-head-e4bb8833` 模式）。备份 ref 推送成功之前禁止任何 force-push。
2. fetch 最新 master，验证 `git merge-base --is-ancestor 77e5399a origin/master` 为**假**（确认 squash 生效）。
3. 在集成 worktree：`git switch -c sluice-608-p2c1-reanchor origin/master`，然后按序 cherry-pick #612 自身 4 个 commit：`42f5eef9` → `cc8ad2ab` → `b12fb551` → `70d48f5e`。
4. 预期冲突点为 §5 所列 4 文件；解决原则：保留 C1（#602）语义 + 叠加 P2C1 机制，不删除 C1 代码、不降低测试范围；`xmake/tests.lua` 必须同时收录 `semantic_range_test` 与 `direct_composition_fault_test`。
5. 等价性验证：`git diff 77e5399a 70d48f5e -- src include tests xmake`（原 P2C1 产品 diff）与重锚候选在同 base 下的 diff 逐 hunk 对照，机制等价；ledger docs hunk 允许按新基线改写。
6. 推送：`git push --force-with-lease=sluice-608-p2c1-read-compose:<届时远端实读 SHA> origin sluice-608-p2c1-reanchor:sluice-608-p2c1-read-compose`（lease 值推送前一刻实读）。
7. `gh pr edit 612 --repo jnhu76/Sluice --base master` 迁移 base；确认 PR Workflow 触发（章程 §3.2：旧 #612 无 Actions，未触发则查明事件过滤并受控修复；**不得取消 CI 要求**）。
8. 若分支无法安全重写：创建替代 PR（base=master，携带 P2C1-only diff），#612 保持 OPEN 并注明关系，历史可访问。

## 7. 全部分支备份与恢复方法

- **通用规则**：任何分支被 force-push/重写前，先 `git push origin <原SHA>:refs/heads/backup/<用途>-<原短SHA>`；backup ref 只增不删。
- 既有备份：`refs/heads/backup/pr-601-original-head-e4bb8833 → e4bb8833`（`git ls-remote origin refs/heads/backup/*` 实测存在，对应附录 A；#601 当前 head 已是收缩后的 `2fcc7964`，无需再动）。
- 本轮新增计划备份：
  - `backup/pr-612-original-head-70d48f5e → 70d48f5e`（重锚前，必建）。
  - 若重锚候选推送前又产生中间态，命名 `backup/pr-612-reanchor-<短SHA>`。
  - dududu #7 无需重写；若 Phase 5 需要修订，先建 `backup/pr-7-<短SHA>`。
- **恢复方法**：分支损坏时 `git push origin --force-with-lease backup/pr-X-<短SHA>:refs/heads/<分支名>` 恢复到备份 SHA；PR 关联不受分支重写影响。冻结 worktree（`Sluice-600-base`、`Sluice-600-wt`）与其分支不在操作范围内，禁止触碰。
- 所有 backup ref 完成使命后保留至 Owner 明确批准清理；本计划不授权删除任何 backup ref。

## 8. Head SHA Guard 执行程序（每个合并动作）

1. 动作执行前一刻：`gh pr view <N> --repo … --json headRefOid,state,mergeable`。
2. `headRefOid` ≠ 本计划 Guard 值 → **停止该动作**，记录新 SHA 与差异，重新走证据链，不合并。
3. `state != OPEN` 或 `mergeable != MERGEABLE` → 停止并记录。
4. 合并命令显式指定方式（squash/merge 按本计划 §4）；合并后立即 `git fetch` 并核对 master 树 hash == 预期树（§4 表）；不符即停并记录。

## 9. 与章程附录 A 快照的比对

- 一致：master/dududu main 起点、#601 `2fcc7964`、#602 `77e5399a`、#610 `d40a811d`、#612 `70d48f5e` + base `sluice-600-direct-c1`、#611 与 #610 的 34 文件重叠、主树未跟踪 `research/direct-beyond-parity/`、`backup/pr-601-original-head-e4bb8833` 存在、#602 CI 6/6。
- 新增记录（附录未给出）：#611 head `999b5305`、dududu #7 head `cc321a49`。
- 无 PR 已合并/关闭/换 head 的严重偏差 → 计划无需因快照漂移调整。
