# 分支 "r1-request-geometry"（jnhu76/dududu）保留价值审计

审计人:分支保留价值审计员(动态工作流子代理)。日期:2026-10-29。
对象:`dududu` 仓库(GitHub `jnhu76/dududu`,经 `/tmp/dududu-audit` 克隆访问,默认分支 `main`)
的远端分支 `r1-request-geometry`。
性质:纯审计与非破坏性验证。未修改任何产品代码,未删除任何分支/Worktree/证据目录,
未 merge/close 任何 PR,未 force push,未重新克隆(克隆在会话开始前已存在)。
写操作仅限本报告目录与 receipts/。所有 git 命令均带 `-C /tmp/dududu-audit`(Sluice 侧检查除外,另行注明)。

## 1. 必查项结果(全部本会话实际执行)

| 检查 | 命令 | 结果 | receipt |
|---|---|---|---|
| 完整 HEAD | `git -C /tmp/dududu-audit rev-parse origin/r1-request-geometry` | `47cd1976701a2c9e73fc31470f45aec236a3e0b2`,与任务预期**完全一致**;`git ls-remote origin` 远端 `refs/heads/r1-request-geometry` 同值,无偏差 | receipts/…-01-head-resolution |
| Associated PR | 任务给定 PR 列表按 headRef 匹配 + `gh pr view 8 -R jnhu76/dududu --json …` | 命中 **dududu PR #8**(headRef `r1-request-geometry`,headSha `47cd1976…`,baseRef `main`,state **OPEN**,title "docs(bench): record metadata micro-read study and opportunity gate");Sluice 仓库列表中无 headRef 命中(该分支属 dududu,非 Sluice) | receipts/…-07-pr-state |
| Commit reachability(包含关系) | `git branch -a --contains 47cd1976…`;`git tag --contains`;`git for-each-ref --points-at` | 本克隆内仅 `remotes/origin/r1-request-geometry` 一个 ref 包含该提交;仓库无任何 tag;`refs/pull/8/head`(GitHub)也指向同一 SHA | receipts/…-02-commit-reachability |
| 独有提交数 | `git rev-list --count main..origin/r1-request-geometry` | **4**:`b2dced4`(feat(bench): add parquet pread request geometry analyzer)、`434416e`、`47c9a29`、`47cd197`(全部 docs(bench));merge-base = `12a8712` = 当前 main HEAD,分支严格领先 main | receipts/…-02 |
| Blob reachability(独有对象) | `git rev-list --objects origin/r1-request-geometry ^main` | 27 个独有对象(4 提交 + 树 + 8 个独有 blob 等) | receipts/…-03-blob-reachability |
| 差异文件 | `git diff --name-status main...origin/r1-request-geometry` | **8 个文件全部为新增(A),共 996 行**,全部位于 `benchmarks/duckdb-request-geometry/` | receipts/…-03 |
| 逐文件 blob 比对 | 对 8 文件逐个 `git cat-file -e main:<path>` + `git rev-parse <branch>:<path>` 后在 `git rev-list --objects main` 中查该 blob SHA | 8/8:路径在 main **ABSENT**,且相同 blob SHA 在 main 任何路径**不可达**(排除改名/搬移)→ **内容未进默认分支** | receipts/…-04-blob-comparison |
| Unique evidence | dududu 其他 ref `git grep "duckdb-request-geometry"`;Sluice master `git grep "NO_MATERIAL_OPPORTUNITY"` / `request geometry`;`ls` + `grep` Sluice `research/direct-beyond-parity/` | 独有未归档研究数据**成立**(详见 §2):结论文档、gate 裁决、AC 建议、analyzer、trace 溯源索引只存在于本分支(及 PR #8 的 refs/pull/8/head);dududu 其他 4 个 ref、Sluice master、#610 归档目录均无 | receipts/…-05、…-05b |
| Active dependency | OPEN PR head(gh 证实);`git grep "r1-request-geometry"` 于分支树、dududu CI(`.github/workflows/repo-policy.yml`)、Sluice 仓库 | 依赖 = **OPEN PR #8 本身**(GitHub 持有 `refs/pull/8/head` 同 SHA);仓库脚本/CI **无**分支名引用(CI 仅按 pull_request 事件触发,无分支 pin);Sluice 仓库(审计目录外)零引用 | receipts/…-06、…-07 |
| Worktree 检出 | `git worktree list`(克隆);`git branch -a`;`ls /home/hoo/Projects` | **本机无任何 worktree 检出该分支**:克隆自身检出 `main`;分支无本地头;`/home/hoo/Projects` 下无其他 dududu 工作副本 | receipts/…-06 |
| Working tree | 不适用 + `git status --porcelain`(克隆) | 分支未被本地任何 worktree 检出 → 无 dirty 观测对象;克隆自身 porcelain 输出为空(干净)。其他主机状态不可观测(见 §4) | receipts/…-06 |
| Recovery | `git ls-remote origin refs/heads/r1-request-geometry refs/pull/8/head` | 远端分支 ref 与 `refs/pull/8/head` **均为** `47cd1976…`:即便删除分支头,提交经 PR ref 仍可 fetch;PR #8 可溯源;完整 SHA 已记录于本报告与 receipts | receipts/…-01/02/06 |

## 2. 独有证据内容(未归档,8 文件 / 996 行)

`benchmarks/duckdb-request-geometry/`(PR #8 全部内容;DuckDB v1.5.6 pread 请求几何研究,
Sluice #578 R1 输入、mission Phase 6.2–6.5):

- `DUCKDB_METADATA_REQUEST_GEOMETRY.md`(250 行):492 preads/查询的请求归因、依赖分层、
  唯一合法合并窗口 W1(每 row group 投影 chunk 连续跨度,`prefetch_all_parquet_files` 可达 492→50)。
- `DUCKDB_METADATA_OPPORTUNITY_GATE.md`(257 行):Phase 6.3/6.4 门裁决
  `METADATA_IO_OPPORTUNITY_VERDICT = NO_MATERIAL_OPPORTUNITY`(收益上界 ~0.22–0.3 ms/query,
  低于全部已记录测量分辨率;按门规则不建 adapter、不花正式 A/B)。
- `SLUICE_AC_RECOMMENDATIONS.md`(127 行):对 Sluice AC-1(#605)/AC-2(#606)/AC-3(#607)
  **均无采纳驱动**的建议 + 下一阶段准入条件(2026-10-29 经 gh 复核三议题状态后写成)。
- `analyze_pread_geometry.py`(282 行,可复算 analyzer,分类不穷尽即 abort)、
  `results/trace-index.md`(48 行,5 个原始 strace 输入的路径+SHA-256+采集命令)、README、两个 .gitignore。

关键缓解事实(降低"原始数据丢失"风险,不消除"结论未归档"事实):trace-index 所列 5 个
原始 io-trace 输入在**本机** `/home/hoo/Projects/Sluice/research/direct-beyond-parity/closure/r5-native/duckdb/`
存在且 `sha256sum` 与 trace-index 记录**逐一匹配**,且这些文件**已被 Sluice master 跟踪**
(`git ls-files --error-unmatch` 通过,`ls-tree master` 可见 blob,worktree 对该目录 status 干净)
——原始输入有 git 持久副本,分析可用分支内 analyzer + 该副本复算;但**结论文档本身只在本分支**。
receipts/…-05b-raw-input-preservation。

## 3. 决策与依据

**decision = KEEP**。

- 规则对照:DELETE_CANDIDATE 六条件中第一条"**非 OPEN PR head**"即不成立——
  `gh pr view 8 -R jnhu76/dududu` 实测 `state=OPEN`、`headRefOid=47cd1976…`(receipts/…-07),
  `ls-remote` 亦见 `refs/pull/8/head` 同值(receipts/…-02/06)。
- 独有未归档研究数据成立(§2):该条件独立否定任何删除结论。因 PR #8 仍 OPEN、分支是其
  活跃审查对象,此时"归档后删分支"会直接废掉 PR——命中"权威或活跃 → **KEEP**"。
  若未来 owner 关闭 #8 且不合并,该分支在删除前**必须先归档**这 8 个文件(ARCHIVE_FIRST 语义);
  若 #8 合并,内容随 merge 进入 main,分支随后即可安全删除。
- 原始输入已被 Sluice master 跟踪(§2),研究原始数据不构成额外风险。
- 本阶段仅出结论,未执行任何删除/归档/PR 操作。

## 4. 局限与如实说明

- 未在本机运行 `analyze_pread_geometry.py` 复算结论(非本审计要求项);"可再生"判断依据是
  analyzer 在分支内、原始输入在 Sluice master 跟踪且哈希逐一匹配、trace-index 记录了再生成命令,
  本次未实际重放。
- 分支检出与 dirty 状态仅能在本机观测:本机确认无任何 worktree 检出该分支;trace-index 记录的
  分析主机路径 `/home/jnhu/Source/Sluice/…` 在本机不存在(`ls: cannot access '/home/jnhu'`),
  其他主机上是否存在检出该分支的 worktree/克隆**不可观测**,如实留为 UNKNOWN,不影响 KEEP 判定
  (OPEN PR head 已独立锁定结论)。
- dududu 仓库无 tag,"权威 ref 引用"检查以分支/PR ref 为准;`for-each-ref --points-at` 仅命中
  该 remote-tracking ref。
- 逐文件 blob 比对首次执行因 zsh 不做单词拆分而失败(整串路径被当作单一路径),
  已改用逐行 read 循环重跑并以 receipts/…-04 为准;失败的中间输出未留存。
