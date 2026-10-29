# INTEGRATION_LOG.md — E5 Product Integration 执行日志（追加式，禁止覆盖他人段落）

---

## [Phase 0] Repository Snapshot & Merge Plan — 2026-10-29

执行者：Phase 0 集成计划员。全部命令在 E5 Native Linux 本机执行。

### P0.1 git fetch 与基准 SHA

```
git -C /home/jnhu/Source/Sluice fetch origin      # exit=0
git -C /home/jnhu/Source/Sluice rev-parse origin/master
→ bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
git -C /home/jnhu/Source/dududu fetch origin      # exit=0
git -C /home/jnhu/Source/dududu rev-parse origin/main
→ b94041d918672d113ea0ca667ba8ccdabf329c80
```
与附录 A 快照一致。

### P0.2 PR 实时状态（gh pr view / gh pr checks / GraphQL reviewThreads）

- #601：OPEN，head `2fcc79646ee42d1699468e0a50c0269f405b92d3`（`sluice-600-direct-research`），base master，MERGEABLE/CLEAN，reviewThreads 0，CI 仅 CodeRabbit skipping。
- #602：OPEN，head `77e5399a45998dea396699fa2e063c94a665945a`（`sluice-600-direct-c1`），base master，MERGEABLE/CLEAN，reviewThreads 0，CI 6/6 pass（C++ comment authority guard、CodeRabbit、Linux asan tail CLI oracle、Linux debug build + tests、Linux release build + tests、Package verifier surface self-tests；run 40363163278）。
- #610：OPEN，head `d40a811dae783837c4749718a2a17f895d46d117`（`research/direct-beyond-parity-evidence`），base master，MERGEABLE/CLEAN，reviewThreads 0，CI 仅 CodeRabbit skipping。
- #611：OPEN，head `999b5305ae7d413a2a71acde926f8c6cfef6876a`（`research/r6-ac1-vectored-semantics`），base master，MERGEABLE/CLEAN，reviewThreads 0。
- #612：OPEN，head `70d48f5eb55a664544b5160cd4675d32150bb21e`（`sluice-608-p2c1-read-compose`），base `sluice-600-direct-c1`（stacked，与章程一致），MERGEABLE/CLEAN，reviewThreads 0，**无 GitHub Actions 检查**（仅 CodeRabbit skipping）——与章程 §3.2 预警一致。
- dududu #7：OPEN，head `cc321a4994ee638493fecda0c437481528f5cea9`（`sluice-r0-e5-verdict`），base main，MERGEABLE/CLEAN，reviewThreads 0，CI = Repository Policy PASS（run 40807898425）+ CodeRabbit skipping。

命令模式：`gh pr view <N> --repo jnhu76/Sluice --json number,title,state,headRefName,headRefOid,baseRefName,mergeable,mergeStateStatus,reviewDecision,updatedAt`；`gh pr checks <N> --repo …`；GraphQL `reviewThreads(first:100)`。

### P0.3 分支保护

```
gh api repos/jnhu76/Sluice/branches/master/protection → 404 "Branch not protected"
gh api repos/jnhu76/Sluice/rulesets → []
gh api repos/jnhu76/dududu/branches/main/protection → 404 "Branch not protected"
gh api repos/jnhu76/dududu/rulesets → []
GraphQL branchProtectionRules → 两仓库 nodes: []
```
结论：无 required checks；门禁全部为章程程序性要求。

### P0.4 合并约定判定

- Sluice：现行 **squash**。`git log --first-parent -30 origin/master`：#585/#586/#587/#592 为单父 `(#NNN)` 提交；`git cat-file -p bdfd0d8d` 单 parent；`gh api pulls/592` merge_commit_sha=bdfd0d8d。#583（3df10dc4）及以前为双亲 merge commit，已废弃该方式。
- dududu：**merge commit**。3/3 PR 为 "Merge pull request #N"；`git cat-file -p b94041d` 双 parent。

### P0.5 Worktree 与主树只读检查

`git -C /home/jnhu/Source/Sluice worktree list`（13 项）：
```
/home/jnhu/Source/Sluice                    bdfd0d8d [master]
/home/jnhu/Source/Sluice-600-base           bdfd0d8d (detached)   ← 冻结，未触碰
/home/jnhu/Source/Sluice-600-wt             77e5399a [sluice-600-direct-c1] ← 冻结，未触碰
/home/jnhu/Source/Sluice-direct-compose-stack-readpreval-writeexit 4ab6ef17 [direct-compose-stack-readpreval-writeexit]
/home/jnhu/Source/Sluice-direct-cost-ladder 4455d98d [direct-cost-ladder]
/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps    deaf0301 [direct-read-compose-prevalidated-steps]
/home/jnhu/Source/Sluice-direct-write-compose-exit-staging         476bd9ce [direct-write-compose-exit-staging]
/home/jnhu/Source/Sluice-perf-p2c1-base     77e5399a (detached)
/home/jnhu/Source/Sluice-perf-p2c1-head     70d48f5e (detached)
/home/jnhu/Source/Sluice-r6-ac1             999b5305 [research/r6-ac1-vectored-semantics]
/home/jnhu/Source/Sluice-r9-audit-c1        77e5399a (detached)
/home/jnhu/Source/Sluice-r9-audit-p2c1      70d48f5e (detached)
/home/jnhu/sluice-593-research/worktrees/pr 2714aa65 [research/593-key-path]
```
- 主树 status：仅 `?? research/direct-beyond-parity/`（1 行，未跟踪；未做任何破坏性操作）。
- `Sluice-600-base` HEAD=bdfd0d8d（有既有未跟踪 `build-baseline/`，非本轮产物，未触碰）；`Sluice-600-wt` HEAD=77e5399a 干净。
- dududu 主树 status：0 行（干净）。

### P0.6 集成 worktree 创建

```
git -C /home/jnhu/Source/Sluice worktree add -b integration/e5-product-merge \
    /home/jnhu/Source/Sluice-integration origin/master
→ HEAD is now at bdfd0d8d …；目录此前不存在（ls 实证），为全新创建。
git -C /home/jnhu/Source/Sluice-integration rev-parse HEAD → bdfd0d8d…（status 0 行）
```

### P0.7 变更文件交集与冲突预判

`git diff --name-only` 统计（base=master bdfd0d8d；#612 用其 base 77e5399a）：
- #601：5 文件，全部 `research/600-direct-excess-cost/`。
- #602：5 文件（docs/roadmap/v2-conformance.md、include/sluice/detail/file_semantics.hpp、src/blocking_file.cpp、tests/semantic_range_test.cpp、xmake/tests.lua）。
- #610：819 文件，全部 research/。
- #611：34 文件；与 #610 交集 `comm -12` = 34（路径全重叠）。
- #612：5 文件；与 #602 交集 4：v2-conformance.md、file_semantics.hpp、blocking_file.cpp、xmake/tests.lua。
- #601∩#610 = 0；#601∩#602 = 0；#602∩#610 = 0 → A1/A2/A3 无冲突。

### P0.8 预期树哈希模拟（squash 输出 SHA 不可预知，以 tree hash 为门禁）

```
git version 2.56.0
T601 = 96118fa4c01864c7ee39bb5f78b06b3465603dc9   (#601 head tree)
simC1 = 9260ecac6d565714a24dd3bbfce65cac51248d1f   (commit-tree 模拟 A1 后 master)
EXPECTED-TREE-after-610 = 59e6cc30bfa2014fb80e6feb52d6a0eb4098c49d   (merge-tree --write-tree)
EXPECTED-TREE-after-602 = f29f0bd753b8df31d8753eda6cc5593b8bf571db
dududu #7 head tree = 731e518af903fe49beb798d80d269eeaceb53794（main 是 head 祖先，merge-commit 后树相同）
```

### P0.9 既有备份 ref

`git ls-remote origin 'refs/heads/backup/*'` → `e4bb8833699d0ca5dee191883f3fec6d41b221b2 refs/heads/backup/pr-601-original-head-e4bb8833`（与附录 A 一致）。

### P0.10 产出物

- `/home/jnhu/Source/Sluice-integration/INTEGRATION_PLAN.md`（新建）
- `/home/jnhu/Source/Sluice-integration/INTEGRATION_LOG.md`（本文件，新建）

### P0.11 快照比对结论

无严重偏差（无 PR 已合并/关闭/换 head）。新增记录：#611 head `999b5305`、dududu #7 head `cc321a49`（附录未载）。#612 无真实 Actions 与章程预警一致。

---

## [Phase 1.1] PR #601 校验与合并 — 2026-10-29

执行者：#601 校验合并员。所有状态在执行时实时重读。

### 1.1.1 实时状态与 Head Guard

```
gh pr view 601 --repo jnhu76/Sluice --json …
→ OPEN，head 2fcc79646ee42d1699468e0a50c0269f405b92d3（sluice-600-direct-research），
  base master，MERGEABLE/CLEAN，changedFiles=5，reviewDecision=""
statusCheckRollup：仅 CodeRabbit SKIPPED（非阻断）
reviews=[]，reviewThreads totalCount=0（GraphQL 实测）
branches/master/protection → 404 Branch not protected（重测，与 P0.3 一致）
```

### 1.1.2 diff 仅含 research/600-direct-excess-cost/（researchOnly=TRUE）

```
gh pr diff 601 --name-only → 5 文件全部 research/600-direct-excess-cost/
git diff --name-status bdfd0d8d..2fcc7964 → 5×A（纯新增，+348 行）
git diff --name-only bdfd0d8d..2fcc7964 | grep -v '^research/' | wc -l → 0
merge-base(origin/master, 2fcc7964) = bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57（= 当时 master）
```
无产品源码（src/include/apps/tests/xmake 零触及）、无编译目标、无 Installed Header、无 API 改动。

### 1.1.3 记录可读性与 Evidence Index 引用核验

5 个记录（EVIDENCE_INDEX.md、PHASE_I_COST_ATTRIBUTION.md、PHASE3_VALIDATION.md、
DIRECT_COST_DAG.md、DIRECT_MACHINE_COST_LEDGER.md）自 PR head 逐一 `git show` 读取，全部完整可读。
- Index「Artifacts in this directory」列出的 4 文件全部存在于同一 tree ✓；未提交 workspace 资产已在文中显式声明。
- `scripts/verify_v2_package.py`（复现骨架引用）在 master 存在（git cat-file -e → EXISTS）。
- dududu 外部引用逐字命中：`benchmarks/parquet/results/r1/summary.txt:18-19` 确为
  T3_fullscan/{p1_native,p2_sluice}/medium 行、57 次 / 181,996,415 B；
  `experiments/parquet/src/backend_sluice.cpp` 第 63 行 `file_info`、第 75 行 `size`（sed 实测）。

### 1.1.4 关键结果/编译器/SHA/收据一致性（consistencyCheck）

- BASE_SHA `bdfd0d8d…` 三处一致（Index/PHASE3/DAG），且=合并前 master ✓。
- CANDIDATE_SHA `ea2fc4fca1a6` 为真实提交（git cat-file -t=commit；位于
  backup/pr-601-original-head-e4bb8833 祖先），commit message 与 PHASE3 引文逐字一致 ✓。
- 库 sha256：baseline `489104b5…`、candidate `d1e351a5…` 在 Index/PHASE3（baseline 另见 DAG）一致；
  字节级复验本阶段未重跑（/tmp/sluice600 仅剩 data64m.bin，归档库已清理），以记录内声明的
  clean-room 复现路径为准——如实记录。
- 阶梯/配对数字跨文件一致：A2−A1（+12.8 t=148 / +19.3 / +19.0 / +16.1 / +15.8 / 64KiB +6.2 n.s.）；
  C1 效应（−13.3 t=−179.8 / −16.4 / −15.8 / −16.2 / −16.6、复测 −14.2、read_exact_at −33.8、64KiB n.s.）；
  dududu R1（−82.5 t=−0.20 / +536.0 t=+2.15 / −31.9 t=−0.07 / −429.1 t=−0.42）在
  LEDGER↔PHASE_I↔PHASE3↔INDEX 逐一吻合；互检（A2−A0=+19.3、AX−A2=+32.0）一致。
- 平台引用逐项实测命中：GCC 16.2.1（gcc --version）、GNU ld 2.46.1（ld --version，
  记录中「glibc ld 2.46.1」实指 binutils ld，值精确）、kernel 7.2.11-200.fc44.x86_64（uname -r）。
  perf_event_paranoid=1 为测量时声明值（当前=2，记录自身已注明"during measurement"，非矛盾）。
- 备注（非矛盾）：LEDGER E4 以 T3 全读 ≈135ms 为分母得 ≈0.0007%，PHASE3 以 replay 240ms
  为分母得 0.0004%，各自算术正确、结论同（arithmetically undetectable）；两系列（阶梯 vs V2）
  syscall 档绝对中位数差 ≤19ns，配对 delta 与结论自洽。
- V1 收据（58/58 ×4 配置、3 个命名 mutant kill、package verifier PASS）内部一致，
  与附录 A 快照的 C1 复审结论一致。

### 1.1.5 合并（Head SHA Guard 通过）

```
合并前一刻重读 headRefOid → 2fcc79646ee42d1699468e0a50c0269f405b92d3（= 已审 SHA）GUARD OK
gh pr merge 601 --repo jnhu76/Sluice --squash   # exit=0
→ state=MERGED，mergedAt=2026-10-29T04:54:26Z
PR601_MERGED_SHA = 84a0cc1dd958997c16e84fed233b74600a608964
master：bdfd0d8d → 84a0cc1d（单父 squash、committer=GitHub、subject "(#601)"，符合仓库约定）
master 树 = 96118fa4c01864c7ee39bb5f78b06b3465603dc9 = #601 head tree（与 A1 预期一致）
```

### 1.1.6 合并后核验（postMergeCheck）

```
git fetch origin master → 84a0cc1d
git ls-tree origin/master research/600-direct-excess-cost/ → 恰 5 文件
git diff --name-status bdfd0d8d..origin/master → 仅 5×A（同一目录，无其他变更）
blob SHA（master == PR head）5/5 MATCH；sha256 逐文件双端相同：
  DIRECT_COST_DAG.md            38fb7cfb804c90b2e9ad4dfc0bfb5d8f3edfb8bde4512a5077261dc6b77ba27c
  DIRECT_MACHINE_COST_LEDGER.md 2160cc565075a65ab8611621e2e163acc3ef1c5f092e795980dbb6ceddcdc318
  EVIDENCE_INDEX.md             13d68d2b706a1f08bacbeed6b360b6e758ef98aaa72f648cb17ec358477d5d74
  PHASE3_VALIDATION.md          19cc5f93a71faf2d579a9e75dfa0f20ebf2433d4c9d321c1a0ce58c7a06bb858
  PHASE_I_COST_ATTRIBUTION.md   b8412955aee35b2058c2d292244d0d5fc6ad26da1148843fd66e22a7b5cdd3a1
```

**Phase 1.1 结论：#601 校验通过并实际合并；PR601_MERGED_SHA=84a0cc1dd958997c16e84fed233b74600a608964；无阻断项。**

---

## [Phase 1.2] PR #610 校验（仅校验，不合并） — 2026-10-29

执行者：#610 校验员（本问只校验、不合并，合并由后续阶段执行 Head SHA Guard 后进行）。
所有状态在执行时实时重读。worktree：`/home/jnhu/Source/Sluice-integration`。

### 1.2.1 实时状态与检出

```
gh pr view 610 --repo jnhu76/Sluice --json …
→ OPEN，head d40a811dae783837c4749718a2a17f895d46d117（research/direct-beyond-parity-evidence），
  base master，changedFiles=819，additions=104524，deletions=0，updatedAt=2026-10-29T03:45:31Z
  （首读 mergeable=UNKNOWN，复读 MERGEABLE/CLEAN）
git fetch origin：网络悬挂 ~12 分钟后终止；但本地对象库已含全部所需对象，
  refs/heads/research/direct-beyond-parity-evidence 与 refs/remotes/origin/… 均 = d40a811d（与实时 API 一致），
  origin/master = 84a0cc1d（= Phase 1.1 合并后 master）。
git checkout --detach d40a811dae783837c4749718a2a17f895d46d117
→ HEAD is now at d40a811d；worktree 干净（仅既有未跟踪 INTEGRATION_LOG.md / INTEGRATION_PLAN.md 保留）。
检出后 worktree 保持 detached @ d40a811d（供编排层对 manifestPath 做全量 sha256 门禁）。
```

### 1.2.2 diff 范围（researchOnly）

```
git diff --name-status bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57..d40a811dae783837c4749718a2a17f895d46d117
→ 819 行，全部状态 A（纯新增），全部路径 research/direct-beyond-parity/
grep -vc '^A\s*research/' → 0
merge-base(origin/master, d40a811d) = bdfd0d8d…（#601 squash 后 master 仍为该 base 的后代）
```
无产品源码/SDK/Header/xmake/tests/docs 触及 → noProductCode = TRUE。

### 1.2.3 Manifest 自洽性（全量）

`research/direct-beyond-parity/EVIDENCE_MANIFEST.json`：JSON 数组，**818 条**（= 附录 A 记录），
字段 path/sha256/source_sha/host/arm/toolchain/status/report_ref/reproduction。
Python 全量校验：0 重复路径；0 缺失文件；**818/818 sha256 与检出树实际内容逐条一致**；
0 条目路径超出 research/direct-beyond-parity/；0 条目缺 sha256。
status 分布：RAW_RESULT 727、REPORT 38、RUNNER 20、AUDIT 8、LEDGER 4、其余为
MISSION/PLAN/HANDOFF/CLOSURE_*/ANALYSIS/HISTORICAL_PROBE_600。

### 1.2.4 超出 Manifest 的新增文件

```
added(819) − manifest(818) = {research/direct-beyond-parity/EVIDENCE_MANIFEST.json}（恰 1 个，manifest 自身不可自索引）
manifest − added = ∅（manifest 无指向 diff 之外文件的条目）
```
结论：无实质超出 manifest 的新增文件（beyondManifest = 仅 manifest 自身，符合其自述设计）。

### 1.2.5 秘密/凭据扫描

Python 扫描 819/819 个新增文件（跳过无二进制——实际全部为文本），
11 类模式：私钥块、AKIA、ghp_/gho_/ghu_/ghs_/ghr_、github_pat_、xox*、sk-*、glpat-、AIza、JWT、
通用 cred 键值赋值、SSH 公钥串 → **0 命中**。与归档记录自述 triage 结论一致。
非普通文件（symlink/socket/fifo）= 0；36 个零长 `mutants-*-base.err` 为变异运行的空 stderr 收据
（归档记录 §2 明示保留理由）。

### 1.2.6 原始测量结果未被改写（分支 git log 佐证）

```
git log --oneline bdfd0d8d..d40a811d → 5 commits：
  292e0734（初建归档+manifest）、c2a57a77、165af283、ed6a5f7f、d40a811d
逐提交 --name-status 汇总：819×A + 6×M；触碰>1 次的文件恰 4 个：
  1) EVIDENCE_MANIFEST.json ×3 —— c2a57a77/d40a811d 各 1 条目哈希刷新（对应被改的记录文件本身，
     实证：changed 条目 = closure/r1-evidence-archive.md 与 closure/r9-archive-increment.md），
     165af283 为 +2245/−1 纯增条目（closure 阶段增量 614→818）
  2) closure/r1-evidence-archive.md —— diff 仅为「§6 PR record」新增 #610 记录注释
  3) closure/r9-archive-increment.md —— diff 仅为增量提交身份注释（165af283 SHA + ls-remote 复核）
  4) WSL2_VALIDATION_HANDOFF.md —— 整文件整合重写（P6-era 草稿候选 deaf0301 → 最终候选栈 70d48f5e/PR #612）；
     新版第 6-9 行显式声明 Supersedes 并指明旧稿可回溯于分支提交 292e0734（历史未改写）；
     旧候选身份在 HEAD 的 14+ 个其他归档文件（含 p2-raw/P2C1/*）仍可追溯
819 个文件中其余 815 个单次触碰文件全部 A-only（程序化验证 single-touch all A-only: True）；
p*-raw/ + scripts/ + audit/ + probe/ 共 593 个原始/脚本/审计文件被改数 = 0。
```
结论：原始测量结果未被改写（rawUntouched = VERIFIED）；被修改的 4 个文件均为
manifest / 归档记录 / 交接记录，符合章程「仅修订 Manifest、归档索引或描述」的许可范围。

### 1.2.7 可复现入口

- `closure/r1-evidence-archive.md`：归档记录（冻结方法、逐目录清单、污染 triage、校验 snippet §4）。
- `closure/E5_CLOSURE_SNAPSHOT.md`：实时探测的 E5 环境/Git 身份快照（host/kernel/CPU/governor/SHAs）。
- `EVIDENCE_MANIFEST.json`：782/818 条有 report_ref、193/818 条有 reproduction 字段，727 条 RAW_RESULT。
- 实验脚本在库：p2-scripts/{P2C1-matrix.sh,P2C1-mutants.sh}、p4-scripts/p4-smoke.sh、
  p5-scripts/{p5-combined-matrix.sh,p5-combined-mutants.sh}；
  `probe-snapshot-600/`（ladder.cpp、mkfixture.cpp、paired.sh、paired2.sh、README.md、scripts/）。

### 1.2.8 PR 描述快照偏差（记录，非阻断）

PR body 仍描述早期 613 文件 / 614 manifest 条目快照；最终 HEAD 实际为 819 文件 / 818 条目。
本阶段全部核验以最终 HEAD 实际文件与 manifest 为准（与章程 §1.2 注记一致）。
合并阶段可按授权修订不准确的描述（不改 Raw Results）。

### 1.2.9 Phase 1.2 结论

**#610 校验通过，未合并（按任务边界留给合并阶段）。无阻断项。**
- head = `d40a811dae783837c4749718a2a17f895d46d117`（与附录 A 预期一致；合并时须重读 Head SHA Guard）
- manifestPath = `/home/jnhu/Source/Sluice-integration/research/direct-beyond-parity/EVIDENCE_MANIFEST.json`
- manifestEntries = 818；beyondManifest = 无（除 manifest 自身）；secrets = 0 命中；
  rawUntouched = VERIFIED；noProductCode = TRUE；repro 入口完备。
- 当前 PR mergeable=MERGEABLE / mergeStateStatus=CLEAN（本阶段末复读）。

---

## [Phase 1.2-b] 编排层 manifest sha256 门禁失败诊断 — 2026-10-29

执行者：#610 校验员（续）。编排层门禁脚本对我返回的 manifestPath 报 `MANIFEST_PROBLEMS 818`。

### 1.2b.1 门禁输出诊断（全部 818 条均为 FileNotFoundError，0 条哈希不匹配）

门禁报错路径样本（来自编排层输出原文）：
```
- research/direct-beyond-parity/API_CAPABILITY_CANDIDATES.md:
  [Errno 2] No such file or directory:
  '/home/jnhu/Source/Sluice-integration/research/direct-beyond-parity/research/direct-beyond-parity/API_CAPABILITY_CANDIDATES.md'
```
报错路径 = dirname(manifestPath) + 条目 path，即出现 `research/direct-beyond-parity/research/direct-beyond-parity/`
双重前缀 → 门禁脚本以 manifest 所在目录为基准拼接了 repo 根相对的条目路径。

### 1.2b.2 现场状态复核（执行时重读）

```
git -C /home/jnhu/Source/Sluice-integration rev-parse HEAD → d40a811dae783837c4749718a2a17f895d46d117（detached，未变）
git status --porcelain → 仅既有 2 个未跟踪日志/计划文件
git diff --stat d40a811d -- research/direct-beyond-parity/EVIDENCE_MANIFEST.json → 空（manifest 与 HEAD 逐字节一致）
ls research/direct-beyond-parity/API_CAPABILITY_CANDIDATES.md → 存在（21812 B）
ls research/direct-beyond-parity/research/direct-beyond-parity/ → No such file or directory（门禁所查路径）
```

### 1.2b.3 正确基准全量复验（与门禁同规模）

```
cd /home/jnhu/Source/Sluice-integration && python3 <逐条 sha256 校验，base=worktree 根>
→ entries=818 pass=818 problems=0
```

### 1.2b.4 归档自身记载的路径契约（为何不应改写 manifest）

`research/direct-beyond-parity/closure/r1-evidence-archive.md`:
- 第 85 行：「Every entry carries `path` **(repo-relative)** and `sha256`」
- 第 99-106 行：官方校验 snippet 以 repo 根为 cwd（`root = pathlib.Path(".")`，
  `open("research/direct-beyond-parity/EVIDENCE_MANIFEST.json")`），解析 `(root / e["path"])`。

即 repo 根相对是该索引的已归档契约。若按指示把 818 条 path 改写为 manifest 目录相对
（剥离前缀）并推送新提交：将（a）违背归档记录 §4 记载的复现程序（snippet 将失败），
（b）改变 PR head，使已审 SHA d40a811d 与 Head SHA Guard 基线失效、波及后续合并阶段，
（c）建立在误诊之上（内容零缺陷）。章程 1.2 的 manifest 修订许可针对「manifest 确有错误」
的情形；本例错误在门禁脚本的基准目录，而非 manifest 内容。

### 1.2b.5 处置

按硬边界 9（门禁不可能在不动已审产物的情况下通过 / 指令与证据矛盾 → 上报），
已 escalate 请求 Owner/编排层裁决：维持 repo-relative 契约、门禁以 worktree 根重跑（推荐），
或明确要求改写为目录相对并接受 head 变更与重验。未推送任何提交，未改写任何文件。

### 1.2.10 合并执行（Head SHA Guard 通过）— 2026-10-29

前置：编排层全量 sha256 清单门禁已通过（`MANIFEST_OK entries=818`）；本阶段实时重读确认无阻断：

```
gh pr view 610 → OPEN，head d40a811dae783837c4749718a2a17f895d46d117（未变），
  MERGEABLE/CLEAN，reviewDecision=""
gh pr checks 610 → 仅 CodeRabbit skipping（非阻断），checks_exit=0
GraphQL reviewThreads(first:100) → totalCount=0；reviews(first:20) → totalCount=0
branches/master/protection → 404 Branch not protected（无 required checks）
```

```
Guard：合并前一刻重读 headRefOid = d40a811dae783837c4749718a2a17f895d46d117 = 已审 SHA → GUARD OK
gh pr merge 610 --repo jnhu76/Sluice --squash   # exit=0（未删除分支，历史保留）
→ state=MERGED，mergedAt=2026-10-29T05:30:53Z
PR610_MERGED_SHA = ac995641e7394bfd82f7edb1caafca6ebea21c13
master：84a0cc1d → ac995641（git fetch origin master：84a0cc1d..ac995641；单父 squash、
  parent=84a0cc1d、committer=GitHub、subject "(#610)"，符合仓库约定）
master 树 = 59e6cc30bfa2014fb80e6feb52d6a0eb4098c49d
  = Phase 0 P0.8 预测树 EXPECTED-TREE-after-610 完全一致
```

### 1.2.11 合并后核验

```
git diff --name-status 84a0cc1d..origin/master → 恰 819×A，全部 research/direct-beyond-parity/，
  无任何其他变更（grep -vc '^A\tresearch/direct-beyond-parity/' → 0）
manifest blob：master 64a7e922115eefdf0f2ead8872fafa6d0c726005 == PR head（同 blob）
research/direct-beyond-parity/ 子树：master e9877989b9cec8f1223531bdaf300d60b58be1c5 == PR head
#601 的 5 文件未受影响（ls-tree research/600-direct-excess-cost/ → 5）
```

**Phase 1.2 收口：#610 已实际合并；PR610_MERGED_SHA=ac995641e7394bfd82f7edb1caafca6ebea21c13；
合并后 master=ac995641e7394bfd82f7edb1caafca6ebea21c13；无阻断项、无 blockerClass。
worktree /home/jnhu/Source/Sluice-integration 保持 detached @ d40a811d（manifestPath 门禁基准不变）。**

---

## Phase 1.3 — PR #611 去重核验（2026-10-29，Dedup611 agent）

前置重读（执行时）：
- `gh pr view 610` → state=MERGED，mergeCommit=ac995641e7394bfd82f7edb1caafca6ebea21c13，mergedAt=2026-10-29T05:30:53Z ✓（与前置条件一致）
- `git fetch origin master && git rev-parse origin/master` → ac995641e7394bfd82f7edb1caafca6ebea21c13 ✓
- `gh pr view 611` → state=OPEN，headRefOid=999b5305ae7d413a2a71acde926f8c6cfef6876a，34 files，base=master

34 文件逐一核对（命令 + 结果）：
1. `git ls-tree -r 999b5305 -- research/direct-beyond-parity/closure/` → 34 条；`git ls-tree -r origin/master -- 同路径` → 206 条（#610 归档更宽）。
2. path+blobSHA 集合比较 `comm -23`（#611 − master）→ **0 条缺失/不匹配**。
3. 内容级独立复核 `git diff --stat ac995641..999b5305 -- <34 路径>` → **空 diff（exit 0）**。
4. 提交扫描 `git log ac995641..999b5305` → 仅 1 个提交 999b5305；`git diff-tree --name-only` → 恰好 34 个文件，全部在 closure/ 内，**无 34 文件之外的增量路径**。

归档索引核验：
- master `research/direct-beyond-parity/EVIDENCE_MANIFEST.json`（818 条，blob 64a7e922）→ 34/34 路径收录；
- 逐条 `sha256(git cat-file blob origin/master:<path>)` 与 manifest 记录 sha256 比对 → **34/34 一致，0 mismatch**；
- 状态分布：7 REPORT / 2 RUNNER / 25 RAW_RESULT。

Owner Decision Packet 与原始探针结果：
- `OWNER_DECISION_PACKET.md`：`cmp` #611 vs master → BYTE-IDENTICAL；内容为 decision-support，无任何 adopted API。
- 25 个 raw/*.txt 探针结果：逐文件字节比较 #611 vs master → **25/25 byte-identical**。
- 探针身份闭环：master `probes/r6ac1_probe.c` sha256 = 9e364ff767cc716bba4d31d19d189d7f128eae1edabbab630660c9ec26cf56a9，与 `raw/env-pin.txt` 声明一致（E5，kernel 7.2.11-200.fc44.x86_64，base bdfd0d8d）。

增量扫描：
- 7 个 REPORT 文档的交叉引用扫描 → 4 个命中均为正则截断/包目录相对路径假阳性，无真实断链、无未归档引用。
- PR body 明确 "single commit; PR stays OPEN; no merge"；reviews 空；唯一评论为 coderabbitai 自动摘要，无阻断讨论。
- 关闭前一刻 Head SHA Guard：`gh pr view 611`（关闭前）→ headRefOid 仍为 999b5305（未变）；checks 仅 CodeRabbit COMPLETED。

处置：
- #611 核验报告评论：https://github.com/jnhu76/Sluice/pull/611#issuecomment-6488953673
- `gh pr close 611`（不合并）→ state=CLOSED，mergedAt=null，closedAt=2026-10-29T05:39:33Z；PR/Review/Commit 历史保留；标注 SUPERSEDED_BY_PR610。
- #605 body 更新（3 处替换）：头部、evidence 节、底部均改为引用 master 权威文件 `research/direct-beyond-parity/closure/r6-ac1.md` + `closure/r6-ac1/` @ ac995641e7394bfd82f7edb1caafca6ebea21c13 与 EVIDENCE_MANIFEST.json；追加 2026-10-29 去重收口记录与报告链接。Owner 待决状态（Awaiting Owner）保留未动。回读验证通过。

**Phase 1.3 收口：PR611_DEDUP_VERDICT=SUPERSEDED_BY_PR610；incrementFound=none；incrementPr=0；
issue605Updated=true；无阻断项。未执行任何合并、未触碰 AC-1 公共 API 实现、未修改冻结 worktree。**

---

## [Phase 2.1+2.2] PR #602 C1 产品终审与本地验证（只审不合并） — 2026-10-29

执行者：C1 产品采纳工程师（Phase 2 前半）。本问仅终审与本地/远端验证，**不执行合并**（合并留给 Phase 2.3，按 Head SHA Guard）。

### 2.0 实时状态与检出

```
gh pr view 602 → OPEN，head 77e5399a45998dea396699fa2e063c94a665945a（sluice-600-direct-c1），
  base master，mergeable=MERGEABLE，mergeStateStatus=CLEAN
GraphQL reviewThreads(first:100) → totalCount=0；reviews → totalCount=0（无未解决阻断 review）
git -C /home/jnhu/Source/Sluice-integration checkout --detach 77e5399a… → clean（仅既有 2 个未跟踪日志文件）
merge-base(origin/master, 77e5399a) = bdfd0d8d…；origin/master（Phase 1 后）= ac995641e7394bfd82f7edb1caafca6ebea21c13
PR diff（三点，base=merge-base）：5 文件 +116/−99：
  docs/roadmap/v2-conformance.md(+1/−1)、include/sluice/detail/file_semantics.hpp(+9/−6)、
  src/blocking_file.cpp(+72/−82)、tests/semantic_range_test.cpp(+9/−10)、xmake/tests.lua(+25/−0)
```

### 2.1 产品 Diff 逐项复审（10 项清单）

1. **DataOpVerdict 分发**：`admit_data_op` 返回 `detail::precheck_data_op(...)` 的 enum 判决
   （src/blocking_file.cpp:25-29）；各 op 以 `verdict == complete_empty` / `is_rejection(verdict)` 直接分发。
   enum 定义 include/sluice/detail/file_semantics.hpp:103-109。**通过**。
2. **Precheck 消除**：旧 `struct Precheck`（`std::optional<IoError>` + bool 物化）与
   `checked_posix_offset`（返回 `Result<off_t>`，头文件 ：45-50 旧版）整体删除（diff −82 行主体）；
   成功路径不再构造 Result/optional。**通过**。
3. **Offset Validation Fusion**：`range_is_valid`（file_semantics.hpp:32-39，防溢出尾字节检查）在
   `precheck_data_op` 内一次完成；execute 判决蕴含 `offset ≤ kMaxNativeOffset = off_t max`
   （file_semantics.hpp:26-27，static_assert off_t ≥63 位 :18-22），`checked_posix_offset` 二次校验消除。
   **通过**。
4. **Closed/Access/Range/Empty 优先序**：生产序为 closed → access → empty（零长跳过 range 检查）→
   range → transfer 上限（file_semantics.hpp:162-175），与根规范 §SEM-03 1–4 步
   （docs/explicit-io-contract.md:406-418：closed=invalid_state → access=invalid_argument →
   零长 no-op 跳过 range → range=invalid_argument）逐步一致；拒绝映射 closed→invalid_state、
   access/range→invalid_argument（file_semantics.hpp:116-129）。**通过**。
5. **static_cast<off_t>**：read_at/write_at/resize 在 execute 判决后直接
   `static_cast<off_t>(offset/new_size)`（src/blocking_file.cpp:88-92、109-113、207-209），
   安全性由第 3 项融合判决保证；仅留局部不变式注释（符合仓库 C++ 注释纪律）。**通过**。
6. **EINTR**：全部 syscall（pread/pwrite/read/write/ftruncate/fdatasync/fsync/fstat）仍包于
   `retry_on_eintr`（仅 EINTR 重试，include/sluice/detail/posix_retry.hpp:7-15；用法
   src/blocking_file.cpp:89-92、110-113、129-131、147-149、164、180、208-209、224）。**通过**。
7. **Error Mapping**：syscall 失败仍 `from_errno_value(errno)`（:94、115 等）；判决拒绝经
   `rejection_error`（closed→invalid_state，access/range→invalid_argument，
   file_semantics.hpp:116-129）。`is_rejection/rejection_error/rejection_of` 均为 master 既有实现
   （`git show origin/master:include/.../file_semantics.hpp` ：117/122/137 已存在），本 PR 未改映射。**通过**。
8. **Composition Outcome**：四个组合 op（read_exact_at/write_all_at/read_exact/write_all）保持
   empty→`CompositionOutcome{}`、拒绝→unexpected、compose 循环内 confirmed 偏移推进
   （src/blocking_file.cpp:286-342，compose :269-283 未改动）；null 缓冲检查仍在 compose 之前，
   语义与基线一致（同 invalid_argument 错误）。**通过**。
9. **ABI / Installed SDK**：`detail/file_semantics.hpp` 不在安装 allowlist
   （xmake/libraries.lua:17-22 仅 6 核心 + libraries.lua:37-41 5 async 头）；PR 变更 ∩ 11 安装头 = 0
   （comm 实测）；release 静态库全局符号集 bdfd0d8d vs 77e5399a 完全一致（`nm -g --defined-only`
   各 20 符号，diff 空）。**通过，无公共 API/ABI 变更**。
10. **Conformance Ledger**：docs/roadmap/v2-conformance.md:44 SEM-01..04 行追加 2026-10-27 live
    mutation 证据（3 named mutants + 58/58×4 + package verifier），状态仍为 VERIFIED (carried)，
    无夸大。**通过**。

附：`SLUICE_SEM_MUTANT_*` 仅出现于头文件守卫与 xmake/tests.lua:76-100 的 set_default(false)
手动 mutant 目标（grep 全树 6 处）；verifier 的 NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED 门禁
PASS 证明其不进入安装面。测试改动（tests/semantic_range_test.cpp:89-105）把对已删除
`checked_posix_offset` 的直接断言改写为经 `precheck_data_op` 的同一公共语义断言
（kMaxNativeOffset+1 → reject_range → invalid_argument），oracle 强度未降。

### 2.1b 源码身份核对（identityMatchesResearch）

研究权威记录（master `research/600-direct-excess-cost/PHASE3_VALIDATION.md`，经 #601 合并）：
BASE_SHA=bdfd0d8d…、CANDIDATE_SHA=ea2fc4fca1a6（branch sluice-600-direct-research，
commit "perf(direct): dispatch admission verdicts without Precheck materialization (#600 C1)"，
`git cat-file -t` = commit 实证）、候选库 sha256 d1e351a5…。

```
#602 含 2 commit：4c3e698d（产品，同 subject）+ 77e5399a（docs ledger）
git diff --stat ea2fc4fca1a6 4c3e698d      → 空（产品提交与实测候选树完全一致）
git diff --stat ea2fc4fca1a6 77e5399a      → 仅 docs/roadmap/v2-conformance.md 1 行（非代码）
git diff ea2fc4fca1a6 77e5399a -- src include → 空（exit 0，逐字节一致）
```

**结论：identityMatchesResearch = TRUE。** PR head 的 hot path（src/、include/）与 E5 实测候选
逐字节相同，差异仅 1 行非代码 ledger 文档 → 复用 PHASE3 V2 性能证据（read_at 0B −13.3 ns t=−179.8
… 4KiB −16.6 ns t=−12.4、read_exact_at −33.8 ns，16/16 等），无需 Delta Verification，无需重跑
#600 全部研究。

### 2.2 本地 Correctness / CI 矩阵（全部在 77e5399a 实际执行）

命令形态（沿用归档 P2C1-matrix.sh 的成熟形态）：
`xmake f -c -m <mode> --liburing=<n|y> -y && xmake -y && xmake test -y`；工具链
xmake 3.1.2 / GCC 16.2.1 / liburing 2.13 / kernel 7.2.11-200.fc44.x86_64。
日志：/tmp/sluice-602-matrix/matrix-uring{y,n}-{release,debug,asan,asanubsan}.log

| 格 | 结果 |
|---|---|
| uring=n release | 100% tests passed, 0 failed **out of 58** |
| uring=n debug | 0 failed **out of 58** |
| uring=n asan | 0 failed **out of 58** |
| uring=n asanubsan（asan+UBSan） | 0 failed **out of 58** |
| uring=y release | 0 failed **out of 79** |
| uring=y debug | 0 failed **out of 79** |
| uring=y asan | 0 failed **out of 79** |
| uring=y asanubsan | 0 failed **out of 79** |

8/8 格全绿（MATRIX_OVERALL=0）；58/58 与研究 V1 口径一致，79=58+21 uring 专属测试。

**3 个 C1 Admission Mutation**（release/uring=n，xmake/tests.lua:76-100 目标）：

```
blocking_file_mut_admit_invalid_range      → exit=1 FAIL: read_with_illegal_offset_reports_invalid_argument
blocking_file_mut_empty_before_closed      → exit=1 FAIL: sequential_read_on_write_only_file_rejected_upfront
blocking_file_mut_admit_unrepresentable_size → exit=1 FAIL: resize_overflow_reports_invalid_argument
```

3/3 以 PHASE3 记录的同名断言击杀（RED）；恢复正常 GREEN 由上表 release/uring=n 58/58 承担。

**Installed Package Verifier**（干净检出 /tmp/sluice-602-pkg-verify @ 77e5399a，worktree_clean=True）：

```
profile v2-noliburing：24 gates 全 PASS（含 V2_B_PACKAGE_PREFIX_READY 11/11 头、
  P0_W01_CORE_ONLY_CLEAN_ROOM、NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED、ODR macro-independence）
  → "v2 package verification: all gates PASS"，exit=0
profile v2-liburing（--uring-prefix /usr，系统 liburing 2.13）：24 gates 全 PASS
  （含 V2_P2_URING_RUNTIME 真实 uring 消费者 rc=0）→ all gates PASS，exit=0
manifest sha256：noliburing de74fc6584dc551736b7d4b0b2d6b8c785d2a2e72a5aaa41dfc6747ef3405628
  liburing  9b246a84f170101b61294e1461d054ec66c719db3d83a12eac61811c9bd428e8
```

**Header / ABI Surface**：PR 变更 ∩ 安装头 = 0；`nm -g --defined-only` 全局符号集
bdfd0d8d vs 77e5399a 完全一致（各 20 符号，diff 空）；verifier 11 头契约面 + 负探针
（removed headers / seam / async-symbols-not-in-core）双档 PASS。

**低成本门禁**：`check_cpp_comment_authority.py .` exit=0；
`test_check_cpp_comment_authority.py`、`test_verify_v2_package.py` 自测 exit=0。

### 2.2b GitHub Actions 与远端状态（执行时实读）

```
gh pr checks 602 → 6/6 pass（C++ comment authority guard / CodeRabbit skipping /
  Linux asan tail CLI oracle / Linux debug build + tests / Linux release build + tests /
  Package verifier surface self-tests），checks_exit=0
gh api actions/runs/40363163278 → head_sha=77e5399a45998dea396699fa2e063c94a665945a（与 PR head 一致），
  conclusion=success，event=pull_request
check-runs on head → 全部 success
GraphQL reviews=0，reviewThreads=0 → 无未解决阻断 review
收尾重读：OPEN，head 仍 77e5399a…，MERGEABLE/CLEAN
```

### 2.3 Phase 2 结论（本问范围）

**#602 全部本地与远端门禁通过（8 格矩阵、3 mutants、双档 package verifier、ABI 符号集、
注释守卫、远端 CI 6/6 on audited head、0 未解决 review），与 E5 C1 研究实现源码身份一致，
可复用研究性能证据。本问未执行合并（merged=false）；Phase 2.3 合并前须按章程重读 headRefOid
并确认仍为 77e5399a45998dea396699fa2e063c94a665945a。**

遗留物：/tmp/sluice-602-matrix/（矩阵/verifier 日志与 manifest）；临时 worktree
/tmp/sluice-602-pkg-verify、/tmp/sluice-602-abi-base 保留供后续阶段复核，可随时删除。
集成 worktree 保持 detached @ 77e5399a。未修改任何 PR/issue，未推送任何分支。

---

## [Phase 2.3] PR #602 实际合并与合并后验证 — 2026-10-29

执行者：C1 产品采纳工程师（Phase 2.3）。Head SHA Guard 与全部门禁在执行时实时重读。

### 2.3.1 合并前 Guard（合并前一刻）

```
gh pr view 602 → OPEN，head 77e5399a45998dea396699fa2e063c94a665945a（= Phase 2.1/2.2 已审 SHA）
  MERGEABLE/CLEAN
GraphQL reviews=0，reviewThreads=0；gh pr checks 602 → 6/6 pass（checks_exit=0，run 40363163278）
git fetch origin master → 仍 ac995641e7394bfd82f7edb1caafca6ebea21c13（无漂移）
git merge-tree --write-tree origin/master 77e5399a → f29f0bd753b8df31d8753eda6cc5593b8bf571db
  （== Phase 0 P0.8 预测 EXPECTED-TREE-after-602）
→ GUARD OK
```

### 2.3.2 合并执行（squash，仓库现行约定；不删分支保留 #612 base 可达）

```
gh pr merge 602 --repo jnhu76/Sluice --squash   # exit=0
→ state=MERGED，mergedAt=2026-10-29T06:08:37Z
PR602_MERGE_SHA = 43cb8029ca60b572c91a9aa9dccb17d4300e2f88
```

### 2.3.3 合并后核验

```
git fetch origin master → ac995641..43cb8029
git cat-file -p origin/master → 单父（parent=ac995641…）、committer=GitHub <noreply@github.com>、
  subject "perf(direct): dispatch admission verdicts without Precheck materialization (#600 C1) (#602)"
master 树 = f29f0bd753b8df31d8753eda6cc5593b8bf571db（与预测一致）
git diff --name-status ac995641..origin/master → 恰 5 文件（M×5，同 PR diff）
5/5 产品文件 blob == 77e5399a 对应 blob（逐一 git rev-parse 比对 MATCH）
C1_IN_MASTER = VERIFIED；新产品基线 = 43cb8029ca60b572c91a9aa9dccb17d4300e2f88
```

### 2.3.4 合并后 Direct Smoke / Regression（master 43cb8029，集成 worktree）

```
git checkout --detach origin/master（43cb8029）
xmake f -c -m release --liburing=n -y && xmake -y && xmake test -y
→ exit=0；"100% tests passed, 0 test(s) failed out of 58, spent 4.010s"
（全套件含 blocking_file_*、direct_composition_test、direct_w01_consumer_probe、app oracle 等 Direct 面）
log: /tmp/sluice-602-matrix/postmerge-config.log
```

### 2.3.5 合并后 Installed SDK 验证（干净检出 /tmp/sluice-master-pkg-verify @ 43cb8029）

```
verify_v2_package.py（noliburing 与 --liburing --uring-prefix /usr 两档）
→ 48 [PASS] / 0 [FAIL]，两档均 "v2 package verification: all gates PASS"，exit=0/0
manifest sha256：noliburing 7ba5b229b5066ea61fd468642b0a581f4cdf160eba6b30c5d4354e39dcb19f50
  liburing   5521239165facf59f28fdb32ccc20e713999bfdf4e0b1732f51b144d033f1b8e
log: /tmp/sluice-602-matrix/postmerge-package-verifier.log
```

### 2.3.6 合并推送触发的 master CI — 发现并彻查一项非 C1 红作业

```
gh run list --branch master → run 40827444870（head_sha=43cb8029…）conclusion=failure
作业分解：Linux asan tail CLI oracle ✓ / Linux debug build + tests ✓ /
  Package verifier surface self-tests ✓ / Linux release build + tests ✓ /
  C++ comment authority guard ✗（Scan C++ comments，exit 1）
本地复现（master 43cb8029，python3 scripts/check_cpp_comment_authority.py .）：exit=1，6 处违例：
  research/direct-beyond-parity/closure/r6-ac1/probes/r6ac1_probe.c:7
  research/direct-beyond-parity/closure/r7-ac2/exp2_merge_dispatch.c:8,10
  research/direct-beyond-parity/closure/r7-ac2/exp4_mixed_admission.cpp:4,10,18
  （均为 "SEM-03 …" pseudo-authority 注释）
```

根因链（全部实证）：
- 3 个文件随 **#610**（research 归档）进入 master（`git ls-tree ac995641 -- <3 files>` → 均存在，
  blob 690ec018/3ef3a2f0…），早于本次 C1 合并；
- `.github/workflows/ci.yml`（:6-15、:19-27）的 paths 过滤器不含 `research/**` → #610 的 PR 从未
  触发该扫描（Phase 1.2 实读"CI 仅 CodeRabbit skipping"一致）；
- #602 PR CI 通过是因为其 head 树（77e5399a）不含这 3 个文件（`git ls-tree 77e5399a -- <3 files>` → 0）；
- C1 合并推送触碰产品路径 → push 触发全树扫描首次覆盖归档 → 该作业红。
**非 C1 产品代码问题**（4 个构建/测试/verifier 作业全绿；违例全部为 #610 归档的既有内容）。
处置：修复涉及改写已归档研究证据（其 sha256 在 #610 EVIDENCE_MANIFEST 中登记）或调整扫描范围
——两者均属 Owner 决策（硬边界 3/4 相关），本阶段不改、不绕过、不伪装，如实上报并留给后续阶段/Owner。

### 2.3.7 回写

- #602 评论（合并记录 + 遗留项）：https://github.com/jnhu76/Sluice/pull/602#issuecomment-6489466307
- #600 评论（C1 采纳收口）：https://github.com/jnhu76/Sluice/issues/600#issuecomment-6489471683
- 评论中明确：本地 agent 终审记录 ≠ GitHub 正式 APPROVE review（符合章程 Phase 7 纪律）。

### 2.3.8 Phase 2.3 结论

**PR602_MERGE_SHA=43cb8029ca60b572c91a9aa9dccb17d4300e2f88；C1_IN_MASTER=VERIFIED；
新产品基线=43cb8029。合并门禁（PR CI 6/6 on audited head + 本地 8 格矩阵 + mutants + 双档 verifier）
全绿。遗留两项（均为非 C1 代码问题，已如实上报）：
(a) master push CI 的 comment authority 作业因 #610 归档注释红（根因与文件清单如上）；
(b) #612 因 squash 合并须按计划 §6 重锚（先建 backup/pr-612-original-head-70d48f5e）。**

---

# Phase 3.1 — P2C1 Re-anchor Stacked PR（#612 → master）

执行时间：2026-10-29（紧接 Phase 2.3 之后，全部 SHA 实时重读）。
执行目录：集成 worktree `/home/jnhu/Source/Sluice-integration`，候选分支 `sluice-608-p2c1-reanchor`。

## 3.1.1 实时状态重读（Head SHA Guard 输入）

```
gh pr view 612 → state=OPEN, headRefName=sluice-608-p2c1-read-compose,
  headRefOid=70d48f5eb55a664544b5160cd4675d32150bb21e, baseRefName=sluice-600-direct-c1,
  mergeable=MERGEABLE, mergeStateStatus=CLEAN
  commits: 42f5eef9(test seams) → cc8ad2ab(prevalidated steps) → b12fb551(error outcomes) → 70d48f5e(docs ledger)
git ls-remote origin：
  master=43cb8029ca60b572c91a9aa9dccb17d4300e2f88（#602 squash 后基线，与 Phase 2.3 记录一致）
  sluice-600-direct-c1=77e5399a…; sluice-608-p2c1-read-compose=70d48f5e…（未漂移）
  既有 backup 仅 backup/pr-601-original-head-e4bb8833=e4bb8833（与附录 A 一致）
```
与附录 A 快照零偏差。

## 3.1.2 备份 ref 先行（force-push 前置硬门禁）

```
git push origin 70d48f5eb55a664544b5160cd4675d32150bb21e:refs/heads/backup/pr-612-original-head-70d48f5e
→ [new branch]；git ls-remote 复核：
  70d48f5eb55a664544b5160cd4675d32150bb21e  refs/heads/backup/pr-612-original-head-70d48f5e
```
备份落地先于任何重写操作。本计划不授权删除任何 backup ref。

## 3.1.3 Squash 祖先判定（计划 §6 步骤 2）

```
git fetch origin
git merge-base --is-ancestor 77e5399a45998dea396699fa2e063c94a665945a origin/master → exit 1（假）
git merge-base --is-ancestor 77e5399a45998dea396699fa2e063c94a665945a 70d48f5e… → exit 0（真）
```
结论：C1 未以 commit 形式进入 master 历史（squash 生效），而 70d48f5e 历史包含 C1
——若直接迁 base，PR diff 将重新包含全部 C1。走计划 §6 移植路径。

## 3.1.4 候选构建（从 origin/master 移植 P2C1 自身 4 commits）

```
git switch -c sluice-608-p2c1-reanchor 43cb8029ca60b572c91a9aa9dccb17d4300e2f88
git cherry-pick 42f5eef9 → 3bf551bd（exit 0，无冲突）
git cherry-pick cc8ad2ab → f2011819（exit 0，无冲突）
git cherry-pick b12fb551 → c156efa2（exit 0，无冲突）
git cherry-pick 70d48f5e → 645082821a11ffcc1f835ab46088f54a5add4a64（exit 0，无冲突）
```
计划 §5 预测的 4 文件冲突未发生（3-way merge 的 ours 侧与 commit parent 侧内容相同所致，
见 3.1.5 blob 恒等证据）；应用顺利本身不作为正确性依据，等价性以 3.1.5 为准。

## 3.1.5 等价性验证（计划 §6 步骤 5）

(a) #602 五文件 blob 恒等（master 与 C1 head）：
```
docs/roadmap/v2-conformance.md    5e6a37fc… MATCH
include/sluice/detail/file_semantics.hpp e39760ae… MATCH
src/blocking_file.cpp             6660a423… MATCH
tests/semantic_range_test.cpp     0d3eb7e0… MATCH
xmake/tests.lua                   febe6515… MATCH
（git rev-parse 77e5399a:<f> vs 43cb8029:<f>，逐一相等）
```
(b) 原 P2C1 产品 diff 与重锚候选 diff **整 diff 字节恒等**：
```
git diff 77e5399a 70d48f5e            > /tmp/reanchor/orig-p2c1.diff
git diff 43cb8029 sluice-608-p2c1-reanchor > /tmp/reanchor/candidate.diff
sha256 两者均为 b790936cb4df7655844a8645f10f14c6b2d42836fcdd0724a87bb128eb74f345
5 files changed, 145 insertions(+), 38 deletions(-)（两侧相同，含 docs/ledger hunk 亦无改写）
```
(c) 候选 5 文件与原 P2C1 head blob 恒等（未删代码、未降测试范围）：
```
docs/roadmap/v2-conformance.md cfd8e7db… / file_semantics.hpp d0090897… /
blocking_file.cpp 8a27525d… / direct_composition_fault_test.cpp 1945fc79… /
xmake/tests.lua c55e97be…  — git rev-parse 70d48f5e:<f> vs 6450828:<f> 全 MATCH
```
(d) 双测试共存：候选 `xmake/tests.lua:37` 收录 C1 `semantic_range_test`，
`:73` 收录 P2C1 `direct_composition_fault_test`。
(e) 候选 diff 仅含 5 个 P2C1 产品文件（`git diff --name-status 43cb8029 6450828`），
无 C1 重携带、无 P2C2、无 research 文件。

## 3.1.6 机械有效性（快反馈集，非 §3.2 全矩阵）

```
xmake f -c -m release --liburing=n -y && xmake -y → exit 0
xmake test -y → exit 0；"100% tests passed, 0 test(s) failed out of 58, spent 4.017s"
（与 Phase 2.3.4 相同配置；ASan/UBSan/liburing 矩阵等留给 Phase 3.2 于最终 HEAD 执行）
```

## 3.1.7 Workflow 触发根因诊断与受控修复（计划 §6 步骤 7 前置）

```
.github/workflows/ci.yml:4-6（候选 head 实读）：
on: pull_request: branches: [master]
```
旧 #612 无任何 Actions 的根因：pull_request 触发器 branches 过滤器仅含 master，
旧 base `sluice-600-direct-c1` 不匹配。受控修复：synchronize 事件携带 PR「当时」的 base，
故将计划 §6 步骤 6/7 顺序调整为 **先迁 base 再 force-push**，使唯一一次 synchronize
即满足过滤器并在最终 head 上触发 CI（避免事后 close/reopen 或空提交等补救噪音）。

## 3.1.8 Base 迁移 + 带 lease 的 force-push

```
gh pr edit 612 --base master → 失败（gh CLI GraphQL bug：Projects classic 弃用错误，
  编辑未生效）；改用 REST：gh api -X PATCH repos/jnhu76/Sluice/pulls/612 -f base=master
  → {base:"master", head:"70d48f5e…", state:"open"}（此时远端分支仍为 70d48f5e，Guard 复读通过）
git push --force-with-lease=sluice-608-p2c1-read-compose:70d48f5eb55a664544b5160cd4675d32150bb21e \
    origin sluice-608-p2c1-reanchor:sluice-608-p2c1-read-compose
→ + 70d48f5e...64508282 (forced update)，PUSH_EXIT=0
  （lease 值为推送前一刻 git ls-remote 实读；backup ref 已在 3.1.2 落地）
```

## 3.1.9 终态核验

```
gh api repos/jnhu76/Sluice/pulls/612 → {base:"master", head:"645082821a11ffcc1f835ab46088f54a5add4a64",
  state:"open", mergeable_state:"unstable"（=CI pending）}
PR changed files（REST /files）：恰 5 文件 = P2C1 自身产品文件
gh run list --branch sluice-608-p2c1-read-compose → run 40829335647 触发（CI / pull_request）
```

## 3.1.10 触发的 CI 结果（如实记录，不粉饰）

```
gh run view 40829335647（headSha=645082821a11ffcc1f835ab46088f54a5add4a64，conclusion=failure）：
  ✓ Package verifier surface self-tests
  ✓ Linux release build + tests
  ✓ Linux asan tail CLI oracle
  ✓ Linux debug build + tests
  ✗ C++ comment authority guard
失败定位（候选 head 本地复现 python3 scripts/check_cpp_comment_authority.py . → exit 1）：
  6 处违例全部位于 research/direct-beyond-parity/closure/（r6ac1_probe.c:7;
  exp2_merge_dispatch.c:8,10; exp4_mixed_admission.cpp:4,10,18）
  ——与 Phase 2.3.6(a) 记录的 master 基线失败逐行相同（#610 归档内容，
  其 sha256 在 #610 EVIDENCE_MANIFEST 登记）。候选 diff 不含任何 research 文件，
  P2C1 五文件零违例。非重锚、非 P2C1 引入。
处置：与 2.3.6 相同——修复路径（改写归档证据 / 收窄扫描范围）分别触碰硬边界 4/3，
  本阶段不改、不绕过、不伪装，如实上报。§3.5「GitHub CI 全绿」门禁将因此受阻，
  需 Owner 决策或专门授权修复后方可满足。
```

## 3.1.11 Phase 3.1 结论

**REANCHOR_SUCCESS：#612 现 head=645082821a11ffcc1f835ab46088f54a5add4a64、base=master、
OPEN；PR diff 只携带 P2C1 自身 Composition 优化（5 文件，与原研究机制整 diff 字节等价）；
原 head 已备份（backup/pr-612-original-head-70d48f5e）；PR Workflow 已触发
（run 40829335647，4/5 绿，comment-authority 作业红为 master 既有基线问题，归属 #610
归档注释，非本阶段产物）。§3.2 全矩阵验证与独立复审由后续阶段在最终 HEAD 执行。**

# Phase 3.2 — Final Correctness & CI（#612 候选 head 645082821a11ffcc1f835ab46088f54a5add4a64，base master）

执行者：P2C1 验收工程师（Phase 3.2）。全部命令与输出在执行时实测；日志目录 `/tmp/sluice-612-matrix/`。

## 3.2.1 候选身份（Guard 输入）

```
gh pr view 612 → OPEN，headRefName=sluice-608-p2c1-read-compose，
  headRefOid=645082821a11ffcc1f835ab46088f54a5add4a64，base=master，
  mergeable=MERGEABLE，mergeStateStatus=UNSTABLE
git ls-remote origin → master=43cb8029ca60b572c91a9aa9dccb17d4300e2f88，
  sluice-608-p2c1-read-compose=645082821a11ffcc1f835ab46088f54a5add4a64
集成 worktree /home/jnhu/Source/Sluice-integration HEAD=6450828…（tracked 文件 clean）
PR changed files（REST）：恰 5 文件 = docs/roadmap/v2-conformance.md、
  include/sluice/detail/file_semantics.hpp、src/blocking_file.cpp、
  tests/direct_composition_fault_test.cpp、xmake/tests.lua
工具链：xmake 3.1.2+HEAD.5d21e9d / GCC 16.2.1-2 / liburing 2.13 /
  kernel 7.2.11-200.fc44.x86_64
```

## 3.2.2 构建/测试矩阵（10 格，全部实测通过）

命令形态：`xmake f -c -m <mode> --liburing=<n|y> -y && xmake -y && xmake test -y`
（脚本 /tmp/sluice-612-matrix.sh；逐格日志 matrix-uring{y,n}-{release,debug,asan,ubsan,asanubsan}.log）

| 格 | 结果 |
|---|---|
| uring=n release/debug/asan/ubsan/asan+ubsan | 5×"100% tests passed, 0 failed **out of 58**" |
| uring=y release/debug/asan/ubsan/asan+ubsan | 5×"100% tests passed, 0 failed **out of 79**" |
| UBSan "runtime error" grep | uring=n ubsan/asanubsan 与 uring=y ubsan/asanubsan 日志均 0 处 |

MATRIX_OVERALL=0（10/10 绿）。58=语义全套件（与研究 V1 口径一致），79=58+21 uring 专属。

## 3.2.3 P2C1 七个命名 Mutation（release/uring=n，全部 exit=1 击杀）

`xmake -y <target> && xmake run -y <target>`；直接跑二进制 exit=1（xmake run 包装为 255）：

```
blocking_file_mut_advance_by_requested        → FAIL: short_reads_report_eof_before_full_with_the_prefix
blocking_file_mut_retry_zero_progress         → FAIL: short_reads_report_eof_before_full_with_the_prefix
blocking_file_mut_short_io_retry_zero_progress→ FAIL: read_exact_accumulates_a_short_prefix_then_reports_eof
blocking_file_mut_failure_accounted           → FAIL: error_after_confirmed_prefix_keeps_the_prefix
blocking_file_mut_skip_impossible_count       → FAIL: impossible_count_stops_immediately
blocking_file_mut_step_reports_full_count     → FAIL: short_reads_report_eof_before_full_with_the_prefix
blocking_file_mut_step_skips_eintr_retry      → FAIL: primitive_retries_eintr_without_a_completed_count
```

7/7 与 ledger SEM-05 行"七个命名 mutation（retry-zero-progress 双杀腿）"逐一对应。

## 3.2.4 C1 三个 Admission Mutation（release/uring=n，全部 exit=1 击杀）

```
blocking_file_mut_admit_invalid_range         → FAIL: read_with_illegal_offset_reports_invalid_argument
blocking_file_mut_empty_before_closed         → FAIL: sequential_read_on_write_only_file_rejected_upfront
blocking_file_mut_admit_unrepresentable_size  → FAIL: resize_overflow_reports_invalid_argument
```

3/3 与 Phase 2.2 在 #602 上的同名击杀断言一致。

## 3.2.5 Composition Fault Tests

```
直接运行 build/.../direct_composition_fault_test → exit=0：
  "composition read_exact_at/write_all_at: 16 scenarios compared, 5 not drivable, 0 divergences"
  "composition read_exact/write_all: 11 scenarios compared, 10 not drivable, 0 divergences"
  "all 15 direct composition fault-injection tests passed"
（该 target 亦含于矩阵 58/79 口径内）
```

## 3.2.6 Package Verifier（干净检出 /tmp/sluice-612-pkg-verify @ 6450828…，worktree 新建，tracked clean）

```
profile v2-noliburing：24/24 [PASS]，"v2 package verification: all gates PASS"，exit=0
  manifest VERIFICATION_HEAD=6450828…，DIRTY=False，HEADERS=11
  安装前缀保留：/tmp/sluice-v2-prefix-9chjuof1
profile v2-liburing（--uring-prefix /usr，系统 liburing 2.13）：24/24 [PASS]，exit=0
  V2_E_ARCHIVE_MATCHES_PROFILE（io_uring refs=True）
  安装前缀保留：/tmp/sluice-v2-prefix-91yc61m4
```

## 3.2.7 Installed SDK Consumer（独立探针，仅尖括号包含，对安装前缀编链 -lsluice_core）

```
g++ -std=c++20 -I/tmp/sluice-v2-prefix-9chjuof1/include /tmp/sluice-612_sdk_consumer_probe.cpp
  -L.../lib -lsluice_core → 运行 exit=0，CHECKS=16 FAILED=0（16/16 PASS）
覆盖：write_all_at 23B、read_exact_at 23B+内容、write_all 顺序游标、read_exact 74B 游标、
  offset 覆写往返、past-EOF 31B/23B（eof_before_full+confirmed=23+accounted+无 error）、
  尾部跨界 8B@20（confirmed=3 前缀）、零长度×2（complete/0）、UINT64_MAX 读/写
  （invalid_argument）、closed 读/零长度（invalid_state，closed-before-empty 臂）、
  write-only 读/零长度（invalid_argument，access-before-empty 臂）、move-then-close。
探针首轮 1 FAIL（07 尾部跨界误写成恰抵 EOF 的 complete 语义）为探针自身缺陷，修正后全绿
（与 R4 记录的探针侧教训同类，非 SDK 缺陷；verifier 内置 P0/P1 消费者另行全 PASS）。
```

## 3.2.8 strace per-step 1-syscall oracle（ledger SEM-05 行声明，本 head 复现）

```
/tmp/sluice-612_strace_driver.cpp（每场景一进程），strace -f -e trace=pread64,pwrite64：
  read_full        → 场景恰 1×pread64(…,23,0)=23
  read_past_eof    → pread64(…,31,0)=23 + pread64("",8,23)=0（数据步+EOF 步，各 1 syscall）
  read_zero_length → 0 次 native 调用
  read_closed      → 0 次 native 调用（入口准入先于任何 native 调用）
  write_all_at     → 场景恰 1×pwrite64(…,23,0)=23
（各场景前 2 条 pread64(784,64) 为动态加载器 ELF 读取、1 条 pwrite64(23,0) 为驱动 setup，已扣除）
日志：/tmp/sluice-612-matrix/strace-*.txt
```

## 3.2.9 Public Header / ABI Diff（vs master 43cb8029）

```
git diff --stat origin/master HEAD -- include/ → 仅 include/sluice/detail/file_semantics.hpp +9
  （detail/ 非安装 allowlist；11 安装头 blob 零变更；PR 5 文件 ∩ 安装头 = ∅）
git diff --stat origin/master HEAD -- src/ → 仅 src/blocking_file.cpp +87/−37（async 源零变更）
release/uring=n libsluice_core.a 全局符号（xmake -r 强制重建后）：
  nm -g --defined-only | sort → master 43cb8029 与 head 6450828 各 26 符号，diff 空
  （sluice_async：源与公共头两树字节一致，符号面传递等价）
→ 无公共 API/ABI 变更
git diff --stat 70d48f5e 6450828 -- src/ include/ apps/ → 空（重锚后产品字节与原验证 head 逐字节一致）
```

## 3.2.10 SEM-05 Conformance Ledger 核对（docs/roadmap/v2-conformance.md:44 @ 64508282）

| Ledger 声明 | 本次实测 | 结论 |
|---|---|---|
| 七个命名 composition mutation 各被具名断言击杀 | 3.2.3：7/7 击杀，断言名逐一对应 | VERIFIED |
| release/debug/asan/ubsan/asan+ubsan 58/58 @ --liburing=n | 3.2.2：5 格全 58/58 | VERIFIED |
| release 79/79 @ --liburing=y | 3.2.2：release y=79/79（另 4 格 y 亦 79/79，超集） | VERIFIED |
| package verifier all gates | 3.2.6：两档 24/24，VERIFICATION_HEAD=6450828 | VERIFIED |
| installed-SDK consumer probe 16/16 | 3.2.7：16/16 PASS | VERIFIED |
| strace per-step 1-syscall oracle | 3.2.8：5 场景复现 R4 形状 | VERIFIED（本 head 新证据） |
| public read_at/read/write_at/write 机器流与 C1 基线 77e5399a 一致 | 未由本问重测；归属 R4/R9 研究记录，且 3.2.9 证明重锚产品字节与 70d48f5e 全等 | CARRIED（非本次独立复测） |
| SEM-01..04 行：三个 C1 admission mutation | 3.2.4：3/3 击杀 | VERIFIED |
| THREAD-01 披露（Missing proof or gap 列） | 行内披露与 R4/交接文档一致 | 一致 |

## 3.2.11 GitHub Actions 实际状态（执行时实读）

```
重锚 push 后 PR workflow 已触发：run 40829335647（head_sha=645082821a…，与候选一致）：
  ✓ Package verifier surface self-tests
  ✓ Linux release build + tests
  ✓ Linux asan tail CLI oracle
  ✓ Linux debug build + tests
  ✗ C++ comment authority guard（exit 1，6 处违例）
本地复现 python3 scripts/check_cpp_comment_authority.py . → exit=1，同 6 处：
  research/direct-beyond-parity/closure/r6-ac1/probes/r6ac1_probe.c:7
  research/direct-beyond-parity/closure/r7-ac2/exp2_merge_dispatch.c:8,10
  research/direct-beyond-parity/closure/r7-ac2/exp4_mixed_admission.cpp:4,10,18
根因链（全部实证）：3 个文件随 #610 进入 master（Phase 2.3.6 已证）；候选 diff 不含任何
  research 文件（REST /files 实读）；check_cpp_comment_authority.py 无任何 allowlist/抑制机制
  （仅 EXCLUDED_DIRS=build 目录，脚本实读）；本问另证 3 文件 sha256 与 #610 EVIDENCE_MANIFEST
  登记逐一相符（9e364ff7…/6dc821f5…/539c653b… 实测==manifest）→ 改写注释即破坏归档证据
  完整性（硬边界 4），将 research/ 加入 EXCLUDED_DIRS 即放宽扫描（硬边界 3）。
  master push CI run 40827444870 同作业同红（既有基线，非本候选引入）。
Branch Protection：gh api branches/master/protection → 404 "Branch not protected"
  → 红作业非 GitHub required check（mergeStateStatus=UNSTABLE 而非 BLOCKED），
  但章程 §3.5「GitHub CI 全绿」为任务级门禁，不因无保护而视为满足。
低成本自测本地实测：test_check_cpp_comment_authority.py exit=0；test_verify_v2_package.py exit=0。
```

## 3.2.12 Phase 3.2 结论

**CORRECTNESS（本地全矩阵+mutation+fault+SDK+verifier+ABI+ledger）= 全部通过，无失败项。
CI（远端）= 已在候选 head 实际触发并执行，4/5 绿；C++ comment authority guard 红为 #610 归档
内容在 master 的既有基线问题，非候选 diff 引入；两条修复路径分别触碰硬边界 3/4，本阶段不改、
不绕过、不伪装 → ciGreen=FALSE，§3.5「GitHub CI 全绿」门禁受阻，P2C1 有条件合并保持
BLOCKED，需 Owner 决策（授权对归档注释的一次性修订+manifest 同步更新，或授权扫描范围收窄）
后方可在新 head 重跑并满足全绿。未执行任何合并、未推送任何分支、未修改任何远端状态。**

遗留物：/tmp/sluice-612-matrix/（全部日志）、/tmp/sluice-612-pkg-verify（干净检出+保留前缀）、
/tmp/sluice-612_sdk_consumer_probe*、/tmp/sluice-612_strace_driver*（探针与驱动，后续阶段可复核）。

# Phase 3.3 — Final-HEAD Performance Regression Check（2026-10-29，E5 性能复检员）

会话目录：`/home/jnhu/Source/Sluice-perf-finalhead/`（SUMMARY.md、PREREGISTERED_PLAN.txt、
ENV-P2C1-FINALHEAD.txt、aa-*.txt、ab-*.txt、machid/ 全部原始收据）。

## 3.3.1 测量对象与方法身份（执行时重读）

```
git rev-parse 43cb8029ca60b572c91a9aa9dccb17d4300e2f88 → 存在（master，#602 squash 合并提交）
git rev-parse 645082821a11ffcc1f835ab46088f54a5add4a64 → 存在（#612 重锚后 head）
git merge-base --is-ancestor 43cb8029 64508282 → YES（真实祖先保留）
git diff --stat 43cb8029..64508282 → 5 文件 +145/−38
git diff 77e5399a..43cb8029 -- src/ include/ apps/ xmake.lua xmake/ → EMPTY
  （新 base 与 R4 基线代码逐字节相同，77e5399a→43cb8029 为纯 research/docs delta）
```

方法复用 kit @ 4455d98d；工具 hash 与 R4 收据记载逐一相符：ladder.cpp `fa26ac36…`、
paired2.sh `0f0e0316…`、paired.sh `be811ac8…`、mkfixture.cpp `63bd8858…`、
analyze2-p5.py `1dd959d5…`。

## 3.3.2 新 SHA 重建与身份（强结论）

```
worktree add --detach Sluice-perf-final-base @ 43cb8029；Sluice-perf-final-head @ 64508282
每个 worktree：xmake f -p linux -a x86_64 -m release --liburing=n --kind=static
               --hardened=n --sanitizers= --ccache=y -y && xmake -y   （均 EXIT=0）
base archive sha256 e65a252fb0040f3e913fd41a31a41f99977c7bf9e7661480f126a3f0d207ebcf
  == R4 base archive（/home/jnhu/Source/Sluice-perf-p2c1-base/... 现场重哈希一致）
head archive sha256 a536bc6ac37c5c680f2cbe5d1714df7e09dbaf1ef1945cb509969f9bf110055f
  == R4 head archive（/home/jnhu/Source/Sluice-perf-p2c1-head/... 现场重哈希一致）
ladder-base 89084b1d… == R4 记载；ladder-cand 3af44bf9… == R4 记载（字节相同）
```

→ 本次 Final-HEAD 检查所测机器码与 R4 会话**逐字节相同**；与 R4 结果的差异只能归于
会话条件而非代码。

## 3.3.3 预登记与环境（PREREGISTERED_PLAN.txt 先于任何采样写入）

- 预算：A/A 先行（每格 16 对 ≥ 要求 8）；A/B 每形状 16 对，硬上限 24；**扩展规则：
  仅当 16 对 CI95 排除 0 且方向为候选变慢（疑似回归）才允许扩到 ≤24 以确认回归；
  严禁为让 comp-read-4k 显著而扩展**。
- 判定规则预先固定（方向保持 / 回归=CI 排除 0 且候选变慢且 ≥MDE′ / 负控制 PARITY /
  写路径仅披露）。
- 环境：Xeon E5-2666 v3，schedutil，perf_event_paranoid=2，taskset -c 2 严格串行；
  loadavg 0.12（始）/0.10（终）；两次测量进程清点均 0；读 fixture sha256
  `bc020fd1…2993a`（=冻结 #600 身份）始末不变；写格使用独立副本
  data64m-write.bin（写臂写入 LCG 模式 ladder.cpp:1646，绝不接触读 fixture）。
- Smoke：ax seq 双二进制 checksum 69632 相等、ax tail 40960 相等、wx content_ok=1。

## 3.3.4 A/A 噪声（先行，7 格 × 16 对，cand×cand，order-aware）

| 格 | sd_AA | median | CI95 | 判定 |
|---|---|---|---|---|
| ax seq | 27.02 | +2.05 | [−10.9,+17.9] | null |
| ax tail | 13.60 | −2.00 | [−11.9,+2.6] | null |
| ac | 24.49 | **−12.83** | [−25.4,+0.7] | null（t=−2.02 勉强跨 0）— **漂移披露** |
| a2 | 18.78 | −3.00 | [−11.8,+8.2] | null |
| w2 | 45.34 | +5.81 | [−5.5,+42.8] | null（+181 离群） |
| wx | 14.69 | +0.07 | [−3.1,+12.5] | null |
| wc | 29.59 | +0.64 | [−9.9,+21.7] | null |

MDE′(16)=2.131·sd_AA/4：ax-seq 14.40 / ax-tail 7.24 / ac 13.04 / a2 10.00 /
w2 24.15 / wx 7.83 / wc 15.77 ns/op。机械总体无偏；ac 格存在 −12.8 ns 的 A 侧
有利漂移（归因[UNCERTAIN]：schedutil 频率行为或主机状态；披露不吸收）。

## 3.3.5 ABBA A/B（每格 16 对；无格触发扩展条件，均未扩展）

| 格 | median ns/op | CI95 | pos | 判定 |
|---|---|---|---|---|
| comp-read-4k (ax seq) | **−13.163** | [−32.62,+9.13] 跨 0 | 1/16 | **INCONCLUSIVE 保持**；方向有利、15/16 对候选有利、**无新确定性回归** |
| eofmix (ax tail) | **−13.012** | [−21.96,−4.01] 排除 0 | 4/16 | **方向保持**；本会话亦达 win 证据标准（13.01 ≥ MDE′ 7.24） |
| cursor (ac) | **−15.601** | [−30.80,+0.22] 勉强跨 0 | 5/16 | **方向保持**（AB/BA 子组 −14.2/−17.7 同负）；量级受该格 −12.8 A/A 漂移混杂，本会话不独立重立 win；R4 WIN_CONFIRMED 仍为权威；**无回归** |
| 负控制 read_at (a2) | **−11.282** | [−17.02,+2.82] 跨 0 | 6/16 | 无确定性偏差；机器相同代码上的有利 median=会话漂移披露；≈11 ns 以下本会话 wall 不可分辨 |
| 负控制 write_at (w2) | **−6.522** | [−21.33,+30.35] 跨 0 | 4/16 | 无确定性偏差（+183 离群） |
| write_all_at (wx) | **−12.519** | [−22.35,+2.53] 跨 0 | 6/16 | **无明显回归**；仅披露，无任何性能主张 |
| write_all (wc) | **−7.869** | [−25.46,+11.26] 跨 0 | 5/16 | **无明显回归**；仅披露，无任何性能主张 |

Checksum 门禁（实测）：读 seq 全格单一值 `25165824`、tail `12595200`（与 R4 完全一致）；
写格 content_ok=1 且单一 checksum；每收据 32 样本行。

**会话级披露**：两个负控制格 median 均为 A 侧有利（−11.3/−6.5）→ 本会话存在约
−6…−13 ns 的 A 侧有利机械漂移带。所有有利 A/B median 须对照该带解读。对"回归检查"
而言该偏置是保守方向（掩盖回归而非制造回归），且没有任何格比其负控制带更不利：
comp-read-4k −13.2/a2 −11.3；eofmix −13.0/a2 −11.3；cursor −15.6/a2 −11.3；
wx −12.5/w2 −6.5；wc −7.9/w2 −6.5。

## 3.3.6 机器身份（machid/，objdump 指令序列 diff，剥离偏移）

```
read_at      126 insns IDENTICAL
write_at     126 insns IDENTICAL
read         115 insns IDENTICAL
file_info    114 insns IDENTICAL   ← 全部非组合公共原语机器相同（负控制保证）
read_exact_at   240→174 DIFF（−27.5%，与 R4 完全一致）
read_exact      208→167 DIFF
write_all_at    240→211 DIFF      ← 共享 compose() 模板重构所致（见下）
write_all       208→194 DIFF      ← 同上
```

write 组合 DIFF 的解释（diff 43cb8029..64508282 实读）：共享 `compose()` 模板由
CompositionState 状态机改为 plain confirmed_bytes 循环+直接 outcome 构造器，四个组合
形态（读/写）均重新实例化；写 step 本身仍经由机器相同的 `write_at`/`write` 原语。
写格 wall 平价已在 3.3.5 实测确认。

## 3.3.7 Phase 3.3 判定

**NO_NEW_REGRESSION**：七个形状均无「CI95 排除 0 且候选变慢」结果；
comp-read-4k 保持 INCONCLUSIVE（CI 跨 0、方向有利、未扩展、未伪装获胜）；
eofmix/cursor 收益方向保持（eofmix 本会话亦 CI 排除 0）；read_at/write_at
非组合负控制无确定性偏差（机器身份 IDENTICAL + wall 跨 0，有利 median 披露为
会话漂移）；write_all_at/write_all 无未解释的明显回归（CI 跨 0 + 反汇编 DIFF
已由共享 compose() 重构解释；只披露不提主张）。配对数：每形状 16 对 × 7 形状
（硬上限 24 未动用），A/A 每格 16 对 × 7 格。无 GitHub 写入、无合并、未放宽
任何测试或阈值。R4 的 eofmix/cursor WIN_CONFIRMED 与 comp-read-4k INCONCLUSIVE
结论继续作为权威；本会话在最终 HEAD 对三者补充了方向佐证与负回归证据。

（附注：上述全部原始收据已另存镜像至集成 worktree
`research/direct-beyond-parity/closure/perf-p2c1-finalhead/`——新建目录，未覆盖任何既有文件；
会话原始目录 `/home/jnhu/Source/Sluice-perf-finalhead/` 保持不变。）

---

## 3.4 P2C1 独立复审 — PERFORMANCE_AND_INTEGRATION_REVIEW（2026-10-29）

评审对象：最终 master `43cb8029ca60b572c91a9aa9dccb17d4300e2f88`（origin/master）；
最终候选 head `645082821a11ffcc1f835ab46088f54a5add4a64`（PR #612，`sluice-608-p2c1-read-compose`）。
本节全部为只读检查，未修改任何仓库文件。判定：**REVISE**（性能与集成实质通过；§3.5 合并门禁当前不满足，见 3.4.7）。

### 3.4.1 状态重读（执行时）
- `git fetch origin master; git rev-parse origin/master` → `43cb8029…`；`git log --oneline -5`：#602=43cb8029、#610=ac995641、#601=84a0cc1d 可追溯（#594 未动）。
- `gh pr view 612 --json headRefOid,baseRefName,mergeable,mergeStateStatus,changedFiles` → headRefOid=`64508282…`（与任务给定一致），base=master，MERGEABLE / UNSTABLE，changedFiles=5，state=OPEN。

### 3.4.2 Diff 范围审查（master..candidate，仅 P2C1）
- `git merge-base 64508282 origin/master` = `43cb8029` → 候选已重锚于最终 master，仅携带 4 个 P2C1 提交（3bf551bd / f2011819 / c156efa2 / 64508282），无 C1 复制（C1 经 #602 已在 master 祖先中）。
- `git diff --stat origin/master 64508282`：5 文件 145+/38−（docs/roadmap/v2-conformance.md、include/sluice/detail/file_semantics.hpp、src/blocking_file.cpp、tests/direct_composition_fault_test.cpp、xmake/tests.lua）。无 async/request/P2C2 文件；无公共安装头——`xmake/libraries.lua:17-22` 安装 allowlist 仅 effect/error/file_resource/measurement/result/blocking-file，`detail/file_semantics.hpp` 不在集合。
- compose() 重构语义等价逐路核对（`git show 64508282:src/blocking_file.cpp` + `include/sluice/blocking/file.hpp:42-49` 默认值）：complete→end=complete/remaining=accounted；stalled→eof_before_full|write_no_progress/accounted；error 与 impossible-count→primitive_error/unknown/invalid_state——与旧 `composition_effect_certainty`（候选 file_semantics.hpp:347-360）三路映射逐一等价（ERR-02 效果报告不变）。Confirmed Bytes、单入口 `admit_data_op`（四形态入口未变）、写组合仍路由公共 `write_at`/`write`（write_all_at/write_all 体逐行核对）。
- 候选树 `git grep compose_progress|compose_error|CompositionState -- src include`：header 规则表已无生产调用方，保留为测试 oracle（tests/semantic_short_io_reference_test.cpp:61-121、tests/direct_composition_fault_test.cpp:450-522 直接调用）；六个 mutant 宏在生产内联 compose()（blocking_file.cpp:79,245,308-323）与 header 双侧成对；7 个 mutant 目标注册于 xmake/tests.lua:100-119。
- 新增 runtime state：无（plain `confirmed_bytes` 局部量 + 编译期宏 seam；CompositionOutcome 未变）。

### 3.4.3 性能收据独立复算（自实现 ABBA 配对，非复用 analyze2-p5.py）
- 方法：按 paired2.sh 发射序（偶对 A→B、奇对翻转，delta=cand−base）对 /home/jnhu/Source/Sluice-perf-finalhead/ab-*.txt 七格各 16 对原始样本独立重算（python3，独立实现）。
- 结果与给定结论及 SUMMARY.md 逐数字一致：eofmix −13.012 CI95[−21.957,−4.013] pos4/16（排 0，方向保持，|median|≥MDE′7.24）；cursor −15.601 [−30.80,+0.22] pos5/16（跨 0→仅方向，A/A 漂移 −12.83 已披露）；comp-read-4k −13.163 [−32.62,+9.13] pos1/16 sd39.18（跨 0→INCONCLUSIVE；扩展触发=候选变慢且排 0 未满足→未扩展）；a2 −11.282 [−17.02,+2.82]；w2 −6.522 [−21.33,+30.35]；wx −12.519 [−22.35,+2.53]；wc −7.869 [−25.46,+11.26]。
- PREREGISTERED_PLAN.txt（15:19，先于任何采样）预算与规则：A/A 16 对/格×7、A/B 16 对/格×7、cap 24 未用、win 不扩展——执行一致。
- ENV-P2C1-FINALHEAD.txt 与本会话 `sha256sum` 复现一致：lib base e65a252f…、lib head a536bc6a…、ladder-base 89084b1d…、ladder-cand 3af44bf9…；base worktree @43cb8029、head worktree @64508282。
- R4 权威结论核实（master 内 research/direct-beyond-parity/closure/perf-p2c1/SUMMARY.md:107-110,174-197）：eofmix WIN_CONFIRMED −12.04、cursor WIN_CONFIRMED −20.17、comp-read-4k INCONCLUSIVE、负控制 PARITY——本轮仅补方向佐证与负回归证据，权威语义未改写。

### 3.4.4 E5 机器级归因独立复核（自行 objdump，非读 machid/*.txt）
- `objdump -d --disassemble=<mangled>` 于 machid/{base,head}-blocking_file.cpp.o，剥离偏移比对 8 符号：read_at 126/126 IDENTICAL、write_at 126/126 IDENTICAL、read 115/115 IDENTICAL、file_info 114/114 IDENTICAL；read_exact_at 240→174、read_exact 208→167、write_all_at 240→211、write_all 208→194 DIFF——与 machid/*.txt 收据完全一致。
- 归因链闭合：`git diff --stat 77e5399a 43cb8029 -- src include apps xmake.lua xmake/` 为空；Sluice-perf-final-base(@43cb8029) 与 Sluice-perf-p2c1-base(@77e5399a) 的 libsluice_core.a sha256 同为 e65a252f…；Sluice-perf-final-head(@64508282) 与 Sluice-perf-p2c1-head(@70d48f5e) 同为 a536bc6a… → 重锚候选机器码与 R4 所测逐字节相同，最终 master 与 C1 研究基线逐字节相同。**E5 机器级成本消除归因仍成立。**
- 写组合 DIFF 归因核实：共享 compose() 模板四形态重实例化（源码 diff 逐行核对），写 step 仍调用机器相同的 write_at/write——与给定披露一致，无未解释差异。

### 3.4.5 重锚等价性与合并安全
- merge-base=最终 master；`git merge-tree --write-tree origin/master 64508282` exit 0（无冲突）；库级字节等价见 3.4.4。

### 3.4.6 CI / ABI 证据链（执行时重读）
- `gh api repos/jnhu76/Sluice/commits/64508282…/check-runs`：Linux release/debug build+tests SUCCESS、asan tail CLI oracle SUCCESS、Package verifier surface self-tests SUCCESS、CodeRabbit SKIPPED；**C++ comment authority guard FAILURE**（head_sha=64508282，run 40829335647 / job 123299669646）。
- 失败内容（job log）：6 处 "pseudo-authority pattern … rule: requirement-id"，全部位于 research 归档实验源（closure/r6-ac1/probes/r6ac1_probe.c、closure/r7-ac2/exp2_merge_dispatch.c、closure/r7-ac2/exp4_mixed_admission.cpp:4,10,18）；`git log --diff-filter=A` 显示经 #610（ac995641）入 master；master 43cb8029 自身 check-runs 同项 FAILURE（继承性红灯，非 P2C1 新增；P2C1 改动的 5 文件无一在列）。
- `gh api branches/master/protection` → 404 Branch not protected（无 required checks）；但章程 §3.5 要求「GitHub CI 全绿」，当前在已审 head 上不成立 → §3.5 合并条件未满足，不得以非必需检查为由绕过。
- ABI 链：公共头未触碰（5 文件 diff 不含安装头）；Package verifier CI 绿；ledger SEM-05 行载 installed-SDK consumer probe 16/16 与七 mutant 杀死记录（Phase 3.2 执行；本轮只读未重跑，判定不依赖重跑，如实注明）。
- PR #612 文案：正文明确无 P2C2、写侧保持公共原语、四公共原语机器恒等；性能节声明「Not re-measured in this PR…will be re-verified…and written back here」——**该回写尚未发生**（PR 评论仅 1 条 CodeRabbit）。正文历史数字「≈ −19.0/−15.9/−15.1（comp-read-4k, eofmix, cursor）」按枚举序会将 ≈−19.0 挂在 comp-read-4k 名下；合并前必须以范围化最终文案回写（eofmix/cursor 两收益形状 + comp-read-4k INCONCLUSIVE + 负控制/写路径披露），否则与 §3.5「性能文案仅声称已获支持的两个收益形状」存在误读风险。

### 3.4.7 判定：REVISE
- 性能实质：NO_NEW_REGRESSION 复核成立；主张范围合规；abiUnchanged=true；perfScopeCompliant=true。
- 合并前必须解决（均非候选代码缺陷）：
  1. C++ comment authority guard FAILURE——继承自 master/#610 的 6 处 research 归档注释违规（逐条文件:行号见 3.4.6）。不得放宽 guard/Branch Protection；修复方式（修归档注释 vs guard 范围调整）须 Owner/编排层决策，且不得触碰原始测量数据。
  2. PR #612 性能文案回写未完成（见 3.4.6 末条）。
- 声明：本判定为本地独立复审证据，非 GitHub 正式 APPROVE review；本轮未向 GitHub 写入任何内容；未重跑 Phase 3.2 已执行的套件矩阵/mutant/SDK probe（只读约束），相应主张以 ledger 与 CI 记录为准。

## 3.4B P2C1 独立复审 — CORRECTNESS_REVIEW（2026-10-29，独立于 3.4 性能评审执行）

评审对象（执行时重读）：最终 master `43cb8029ca60b572c91a9aa9dccb17d4300e2f88`（fetch 后 origin/master 实测）；
候选 head `645082821a11ffcc1f835ab46088f54a5add4a64`。`gh pr view 612 --json headRefOid` → headRefOid
与候选逐字节一致（Head SHA Guard 前置核对通过）；state=OPEN，mergeable=MERGEABLE。
本节只读评审：未修改任何仓库 tracked 文件；测试执行在 /tmp 独立 detached worktree（`git worktree add
--detach /tmp/sluice-p2c1-cand 64508282` / `/tmp/sluice-p2c1-base 43cb8029`，评审后已 `git worktree remove --force`）。
`git merge-base --is-ancestor 43cb8029 64508282` → master 为候选祖先；候选仅 4 个 P2C1 提交（3bf551bd/f2011819/c156efa2/64508282）。

### 3.4B.1 逐路语义等价核对（代码级，非复制上轮）
- `git diff 43cb8029..64508282` 仅 5 文件：v2-conformance.md、include/sluice/detail/file_semantics.hpp(+9 纯 mutant 宏)、
  src/blocking_file.cpp、tests/direct_composition_fault_test.cpp(+27)、xmake/tests.lua(+21)。
- outcome 映射逐路对照（master `direct_outcome`+`composition_effect_certainty`（file_semantics.hpp:347-360）↔ 候选
  `complete_outcome`/`stalled_outcome`/`primitive_error_outcome` + CompositionOutcome 默认值
  include/sluice/blocking/file.hpp:42-49）：complete→(complete, accounted, 无 error)；eof_before_full→(accounted)；
  write_no_progress→(accounted)；primitive_error→(unknown, 原样 error)；impossible_count→(primitive_error, unknown,
  invalid_state)。Confirmed Bytes 累加与停止顺序（先 ==total 判 complete、后 zero-progress 停止）与旧状态机逐步等价；
  impossible-count 上界检查（transferred > total−confirmed）保留。零长度入口四形态均仍入口短路 `CompositionOutcome{}`。
- Short Read/EOF/Partial Error：read_exact_at/read_exact step 与公共 read_at/read 同一 syscall 形态
  （native_pread/native_read + detail::retry_on_eintr + from_errno_value(errno)，blocking_file.cpp:88-160 候选），
  仅去除入口已覆盖的逐步再准入；步骤范围并为入口已验证范围，off_t cast 安全性由入口 verdict 保证（行内注释合规）。
- THREAD-01：root docs/explicit-io-contract.md:1003-1014 「File move/close/destruction | Caller serialization」；
  ledger Missing-proof 列披露的并发 close 中断差异（EBADF-class backend_error 替代 invalid_state 停止）与代码逐行核实
  一致，契约内行为无变化，无泄漏/无新 UB 路径。
- Error-path Store 收缩：生产路径 CompositionState 状态机移除（plain confirmed_bytes 局部量）；header 规则表保留为
  非权威 SEM-05 oracle（git grep：生产 src 零调用方；tests/direct_composition_fault_test.cpp:431-534 与
  tests/semantic_short_io_reference_test.cpp:21-122 直接驱动 oracle）。无新增 runtime state（diff 无新成员/全局/标志）；
  6 个 `SLUICE_COMPOSE_MUTANT_*`/`SLUICE_DIRECT_MUTANT_*` 为编译期 test-only seam
  （xmake/tests.lua:80-92 direct_mutant_target：set_default(false)、独立宏、只编 killer+两 src 副本，生产构建不定义）。
- 无 P2C2：P2C2=写组合 exit staging（research/direct-beyond-parity/P2_CANDIDATE_P2C2.md：分支 476bd9ce，+249/−40，
  含 P2C2-matrix.sh 等）；候选写路径 write_all_at/write_all 体与 master 逐行相同（step 仍调用公共 write_at/write），
  无 exit-staging 机制、无 P2C2 脚本文件。#608 issue 范围声明（candidate A alone）与 diff 一致。

### 3.4B.2 本问实际执行的测试矩阵（全部本机实跑，命令与计数如实）
- 候选 64508282：`xmake f -m release --liburing=n -y; xmake -y; xmake test -y` → **58/58 passed, 0 failed**；
  debug 58/58；asan 58/58；ubsan 58/58；asanubsan 58/58；release `--liburing=y` → **79/79**。
  （ledger SEM-05 行矩阵声明逐格复现。）
- 最终 master 43cb8029：release --liburing=n → 58/58 passed（基线无回归锚点）。
- direct_composition_fault_test：master 14 tests → 候选 15 tests（新增 write_all_impossible_count_stops_immediately，
  写方向 impossible-count killer，注册于 fault test main）；short-io 参考测试 9/9。
- Mutation 实跑（每目标独立构建后直接运行二进制，非零退出=击杀）：P2C1 七目标全部 KILLED ——
  advance_by_requested→FAIL short_reads_report_eof_before_full_with_the_prefix；
  retry_zero_progress→同上；short_io_retry_zero_progress→FAIL（oracle concrete 断言）；
  failure_accounted→FAIL error_after_confirmed_prefix_keeps_the_prefix；
  skip_impossible_count→FAIL impossible_count_stops_immediately（读）+ 新增写向 killer 同源；
  step_reports_full_count→FAIL short_reads_report_eof_before_full_with_the_prefix；
  step_skips_eintr_retry→FAIL primitive_retries_eintr_without_a_completed_count。
  C1 三 admission mutant（admit_invalid_range / empty_before_closed / admit_unrepresentable_size）全部 KILLED。
  击杀断言均为具体期望值（非 oracle 镜像比对），mutation 覆盖足以支撑 SEM-05 正确性主张：七 mutant 各对应一条
  可独立观察的错误模式（advance 高估、零进度重试、certainty 误报 accounted、超量转移、step 谎报满额、EINTR 泄漏），
  并由 15+9 个具体断言测试与 strace 1-syscall oracle（Phase 3.2 §3.2.8 记录）交叉约束。
- Mutation 覆盖充分性结论：充分。每条 killing 断言独立于被变异实现（脚本注入 NativeScript + 具体期望 outcome），
  非实现镜像；EOF/短读/部分错误/confirmed-bytes/error-path 各有 ≥1 个具名 mutant 与 ≥1 个具名 killer 锁定。

### 3.4B.3 ABI / 安装面 / 身份链（本问实测）
- scripts/verify_v2_package.py:89 `sluice/detail/file_semantics.hpp` 列于 UNINSTALLED_TREE_HEADERS（前缀负探针强制编译失败）；
  diff ∩ 安装头 = ∅；include/sluice/blocking/file.hpp（CompositionOutcome 公共定义）零变更。
- 本机两树 release 构建 `nm -g --defined-only libsluice_core.a` 全局符号面 diff 为空（24 符号，过滤后逐符号相同）。
- `git diff --stat 70d48f5e 64508282 -- src/ include/ apps/ xmake.lua xmake/` → 空：重锚候选产品字节与 R4/R9 已验证
  原 head 70d48f5e 完全一致 → 既有研究/性能证据仍附着于同一机器码。
- 无 Public ABI 变更 = 成立。

### 3.4B.4 收据核验（读真实文件，非转述）
- R4 权威收据在 master 树内实存：research/direct-beyond-parity/closure/perf-p2c1/ab-comp-eofmix-4k.txt
  （median −12.036 / CI95 [−26.389,−7.513]，与任务给定 −12.04 一致）。
- 最终 head 收据 /home/jnhu/Source/Sluice-perf-finalhead/：SUMMARY.md §5 七格数字与任务给定性能结论逐数字一致
  （eofmix −13.012 [−21.96,−4.01]、cursor −15.601 [−30.80,+0.22]、comp-read-4k −13.163 [−32.62,+9.13] INCONCLUSIVE、
  a2 −11.282、w2 −6.522、wx −12.519、wc −7.869）；PREREGISTERED_PLAN.txt 预算/扩展规则在案；
  各 ab-*.txt 头行绑定 ladderA=cand 64508282 / ladderB=base 43cb8029。
- machid/ 机器身份本问独立 cmp 复核：read_at 126、write_at 126、read 115、file_info 114 全部 BYTE-IDENTICAL
  （非组合负控制）；组合形态 DIFF 且行数收缩（read_exact_at 240→174、read_exact 208→167、write_all_at 240→211、
  write_all 208→194），与披露成因（共享 compose() 重实例化、写 step 路由机器相同 write_at/write）一致。
- 性能主张范围合规：仅 eofmix/cursor 两收益形状（方向保持；R4 WIN_CONFIRMED 保留为权威），comp-read-4k 保持
  INCONCLUSIVE 未伪装，写路径仅披露无主张，负控制由机器身份承载 → perfScopeCompliant=true（correctness 侧确认）。

### 3.4B.5 CI 与门禁事实（执行时实读）
- PR #612 head 64508282 check-runs：release/debug/asan-tail/package-verifier-selftests SUCCESS；
  C++ comment authority guard FAILURE（run 40829335647）；CodeRabbit SKIPPED。
- 本问本地复跑 scripts/check_cpp_comment_authority.py 于两树：违例集逐条相同（6 条，全在
  research/direct-beyond-parity/closure/r6-ac1、r7-ac2 归档实验源）；`git diff --name-only 43cb8029..64508282 | grep research`
  = 0 → 候选零新增违例；`gh run list --branch master` → 43cb8029 同作业 FAILURE（master 既有基线红）。
  修复路径受硬边界约束（改归档注释≈触碰归档证据边界 4；收窄扫描≈放宽门禁边界 3）→ 留 Owner/编排层决策，
  本评审不改、不绕过。该红灯非候选正确性缺陷；§3.5「GitHub CI 全绿」门禁在本 head 不可满足（与 §3.4.6/3.2.12 结论一致）。
- PR #612 无正式 GitHub APPROVE review（reviewDecision 空）；本判定为本地独立复审证据，不得描述为 GitHub 正式 review；
  本轮未向 GitHub 写入任何内容。

### 3.4B.6 判定：CORRECTNESS = APPROVE（aspect=correctness）
- abiUnchanged=true；perfScopeCompliant=true；候选对最终 master 的正确性全部通过（矩阵 6 格实跑 + 10/10 mutant 击杀 +
  fault/short-io/语义套件 + master 基线 58/58 + THREAD-01 披露核实 + 无 P2C2 + 无新增 runtime state + 无 ABI 变更）。
- 非正确性阻断（转呈编排层，与 §3.4 REVISE 项一致）：① comment-guard CI 红为 master/#610 既有基线问题，
  §3.5 全绿门禁受阻待 Owner 决策；② PR #612 性能文案回写未完成（§3.4.6 末条）。
- 评审后清理：/tmp/sluice-p2c1-cand、/tmp/sluice-p2c1-base worktree 已 git worktree remove --force。

---

## Phase 4 — Post-Merge Product Consolidation（产品合并后核验员，2026-10-29）

执行时重读的真实状态（gh + git，非快照）：
- Sluice origin/master = `43cb8029ca60b572c91a9aa9dccb17d4300e2f88`（= PR #602 squash mergeCommit）。
- PR #601 MERGED（mergeCommit `84a0cc1d`）；#602 MERGED（`43cb8029`）；#610 MERGED（`ac995641`）；
  #611 CLOSED（无 mergeCommit，去重未合并，closeout 评论 `SUPERSEDED_BY_PR610` 在案）；#612 OPEN
  head `645082821a11ffcc1f835ab46088f54a5add4a64` base=master mergeable=MERGEABLE mergeState=UNSTABLE；
  新出现开放 PR #613 `ci/comment-guard-research-evidence-scope`（不在本阶段授权合并清单，保持 OPEN 不动）。
- dududu PR #7 MERGED（main=`12a8712e`，Phase 5 范围，本阶段未触碰）。
- 集成 worktree `/home/jnhu/Source/Sluice-integration` 由 reanchor 分支 detach 检出 master `43cb8029`
  （未跟踪文件保留；分支 ref `sluice-608-p2c1-reanchor` 未改动）。EVIDENCE_MANIFEST.json 在
  `/home/jnhu/Source/Sluice-integration/research/direct-beyond-parity/EVIDENCE_MANIFEST.json`（818 条，
  随机抽样 5 条 sha256 与实际文件全 MATCH；该路径供编排层 sha256 门禁）。

### 4.1 情形判定：仅 C1 已合并（章程 Phase 4 情形二）
- C1 身份在 master 成立：`src/blocking_file.cpp` DataOpVerdict 直接分发（admit_data_op →
  detail::precheck_data_op，closed/access/empty/range/cap 优先序）、`checked_posix_offset` 已删除、
  三个具名 mutation seam + killer 目标在 `xmake/tests.lua:76-100`。
- `git diff --stat bdfd0d8d..43cb8029 -- include/ apps/ xmake.lua xmake/` → 仅
  `include/sluice/detail/file_semantics.hpp`（detail 头，未安装）与 `xmake/tests.lua`（测试注册）；
  `src/` 仅 `blocking_file.cpp`（154 行）。公共 API 头零变更。
- P2C1 未混入：`git diff 43cb8029..64508282 -- src/ include/` = 候选独有 3 文件 +124/−37；
  master `src/ include/` grep `prevalidated|PrevalidatedStep|SLUICE_COMPOSE_MUTANT_*` 零命中 →
  P2C1 预校验 step 机制不在 master；P2C2（stack 未准入部分）同样不在（#608 issue 自述 P2C2 不在 slice）。
- master 保留的 `composition_effect_certainty`（src/blocking_file.cpp:244）是既有 SEM-05 基线组合语义
  （bdfd0d8d 已存在），非 P2C1 机制。

### 4.2 master 完整相关 CI 矩阵（本机 E5 实跑，worktree /home/jnhu/Source/Sluice-matrix-master @ 43cb8029）
全部 `xmake f -m <mode> --liburing=<n|y> -y && xmake -y && xmake test -y`：
- release  liburing=n → **58/58 passed, 0 failed**（test_exit=0）
- debug    liburing=n → **58/58 passed, 0 failed**
- asan     liburing=n → **58/58 passed, 0 failed**
- ubsan    liburing=n → **58/58 passed, 0 failed**
- asanubsan liburing=n → **58/58 passed, 0 failed**
- release  liburing=y → **79/79 passed, 0 failed**（真实 io_uring 传输 21 项含入）
- asan tail CLI oracle（CI job 复现）：asan 产物 `build/linux/x86_64/asan/sluice-tail` readelf NEEDED
  `libasan.so.8` ✓；`xmake test "app_tail_cli_oracle_test/*"` → **1/1 passed**。
- Package verifier surface self-tests：`python3 scripts/test_verify_v2_package.py` → exit=0。
- C++ comment authority guard：自测 exit=0（28 tests OK）；扫描
  `python3 scripts/check_cpp_comment_authority.py .` → **exit=1**，6 处违例全部位于
  `research/direct-beyond-parity/closure/r6-ac1/probes/r6ac1_probe.c:7`、
  `closure/r7-ac2/exp2_merge_dispatch.c:8,10`、`closure/r7-ac2/exp4_mixed_admission.cpp:4,10,18`
  （#610 归档带入的既有基线红；远端 master push run 40827444870 同红；修复载体=开放 PR #613，
  不在本阶段授权合并范围；未放宽扫描、未改归档注释）。日志存 /tmp/phase4-guard-scan.txt。
- Installed SDK 验证：`xmake install -o /tmp/p4-sluice-prefix sluice_core sluice_async` 成功；
  安装面 = 恰好 11 个契约头（与 ledger Appendix B 一致）；
  `python3 scripts/verify_v2_package.py --manifest-out /tmp/p4-pkg-manifest-noliburing.json` →
  **exit=0，all gates PASS**（V2_B 前缀门、P0_W01 clean-room、P1 contract consumers×6、
  ODR layout、NEGATIVE×4：removed headers/seam/mutant defines 未安装/async 符号不在 core）。

### 4.3 C1 三个 admission mutation（master 43cb8029 实建实跑）
- blocking_file_mut_admit_invalid_range → run_exit=1，FAIL `read_with_illegal_offset_reports_invalid_argument`
- blocking_file_mut_empty_before_closed → run_exit=1，FAIL `sequential_read_on_write_only_file_rejected_upfront`
- blocking_file_mut_admit_unrepresentable_size → run_exit=1，FAIL `resize_overflow_reports_invalid_argument`
- 三个全部 KILLED（击杀断言为具体期望 IoError::Code::invalid_argument，非实现镜像）。

### 4.4 独立消费者测试（Installed SDK only，源码树零依赖）
- 程序 /tmp/p4-independent-consumer.cpp，仅 `#include <sluice/blocking/file.hpp>`（error.hpp）；
  `g++ -std=c++20 -I/tmp/p4-sluice-prefix/include … -L/tmp/p4-sluice-prefix/lib -lsluice_core` 编链，exit=0。
- **18/18 断言全部通过**：SEM-03 优先序 4 断言（closed>empty: closed 后空读 invalid_state；
  access>empty: write-only 空读 invalid_argument；closed/access 非空读）；empty>range:
  read_at(UINT64_MAX, 空 span) → ok 0 字节；SEM-04: 非法 offset 读 → invalid_argument、
  resize(UINT64_MAX) → invalid_argument；SEM-05: read_exact_at 短 EOF → end=eof_before_full、
  confirmed_bytes=2、error=∅、remaining=accounted（公共契约 oracle；`error` 仅承载 primitive_error，
  初版误把树内助手 composition_error() 行为当公共契约，已按实测探针修正）；write_all/read_exact 完成；
  读写回路。consumer_exit=0。

### 4.5 Public API / ABI 不变（bdfd0d8d 基线 vs master 43cb8029）
- 11 个安装头逐字节 `cmp` 全部 IDENTICAL。
- 全局符号面（`nm -g --defined-only`，过滤 TtWwBbDdRrVv，sort -u diff）：
  libsluice_core.a **20/20 相同**（diff 空）；libsluice_async.a **232/232 相同**（diff 空）。
- C1 唯一 include 变更在 detail/file_semantics.hpp（未安装面）；包验证器负面探针证明 detail 头不可从安装面编译。
- **DIRECT_PUBLIC_API_CHANGE = NONE；DIRECT_ABI_CHANGE = NONE。**

### 4.6 Ledger / 孤儿 / 失效引用 / 失效 base
- Ledger（docs/roadmap/v2-conformance.md @ master）：SEM-01..04 行含 "live mutation evidence 2026-10-27
  under the #600 C1 admission-dispatch restructure: three named admission mutants … release/debug/asan/asan+ubsan
  58/58; package verifier all gates"（该行由 #602 squash 内的 docs commit 一并并入 master）；SEM-05 行保持
  VERIFIED (carried) 未宣称 P2C1（与 P2C1 未合并一致）。行未引用合并 SHA 字符串 `43cb8029`（按 change
  身份关联；本阶段以在 43cb8029 实跑该行全部具名证据建立可复核关联）。
- 孤儿产品测试：矩阵 worktree `git status --short` 为空 → master 树无未跟踪产品测试。
- 失效引用：tests/ + docs/ 中 14 个指向 research/ 的路径缺失，逐一在基线 bdfd0d8d 同样缺失
  （git cat-file 验证）→ 全部为 dated evidence 文档的既有历史引用，非本轮合并引入。
- 失效 PR base：开放 PR #594 / #612 / #613 base 均=master，base 分支存在，无失效 base；
  #612 head 分支 `sluice-608-p2c1-read-compose` 在远端存在（645082821a11ffcc1f835ab46088f54a5add4a64）。
- #611：state=CLOSED、mergeCommit=null、closeout 评论在案 → 去重关闭未合并 ✓。

### 4.7 GitHub 回写（本阶段执行）
- #600：Phase 4 核验评论 + 关闭（C1 研究与产品采纳完成；剩余线索各归其主：P2C1→#608/#612、guard→#613、AC 决策→#605-607）。
- #603：Phase 4 产品集成结果评论 + 关闭（P0-P6 研究已归档 #610；产品结果：C1 in master，P2C1 OPEN 阻断中）。
- #608：状态评论（保持 OPEN——仍承载 P2C1 产品责任；#612 MERGEABLE/UNSTABLE，阻断=§3.5 全绿门禁受既有 guard 红制约，PR #613 待 Owner 决策；无正式 GitHub APPROVE review，本地复审 APPROVE 证据在集成日志）。
- #563：状态评论（保持 OPEN——#609 前置序列之 (1) C1 已 settled、P2C1 待定；#609 保持 PLANNED 未启动）。

### 4.8 环境落点
- 新建 worktree：/home/jnhu/Source/Sluice-matrix-master（矩阵）、/tmp/sluice-p4-abi-base（ABI 基线，/tmp）。
- 冻结 worktree（Sluice-600-base、Sluice-600-wt）未触碰；主工作树无破坏性操作；本阶段零 force-push、零合并。

### 4.9 回写收据（GitHub 评论 URL）
- #600：issuecomment-6495604698 → 已关闭（completed）
- #603：issuecomment-6495611961 → 已关闭（completed）
- #608：issuecomment-6495622151 → 保持 OPEN
- #563：issuecomment-6495622695 → 保持 OPEN
- 清理：本阶段 scratch worktree /home/jnhu/Source/Sluice-matrix-master 与 /tmp/sluice-p4-abi-base 已 git worktree remove（证据均已录入本日志与 /tmp 收据文件）。

### 4.10 执行期间 master 前进（实时捕获）
- 本阶段执行中 `git fetch` 发现 origin/master `43cb8029` → `4b962abbd26156bb15c4378b90811e9889795c03`：
  **PR #613 被另一执行方合并**（非本阶段；不在本阶段授权合并清单）。diff 仅
  `scripts/check_cpp_comment_authority.py`（+7/−2，EXCLUDED_DIRS 增 `research`）+
  `scripts/test_check_cpp_comment_authority.py`（+14）；`src/ include/ apps/ tests/ xmake*` 与
  `research/` 两 HEAD 间 diff 为空（`git diff --stat` = 0 行）。
- 新 HEAD 复核：集成 worktree detach 至 4b962abb；EVIDENCE_MANIFEST.json sha256 前 16 位不变
  （d5010cd1cb15401a）；guard 自测 exit=0、全库扫描 exit=0（/tmp/p4-guard-scan2.txt）；
  master push CI run **40866912039 = SUCCESS**（#610 以来首个全绿 master）。
- 新 HEAD 直接锚点（/tmp/sluice-p4-newmaster，已清理）：release --liburing=n 构建 exit=0、
  **58/58 passed, 0 failed**。产品矩阵于 43cb8029 实跑 + 产品面两 HEAD 零字节差异 + 4b962abb
  release/guard 直接复核 → 矩阵结论覆盖当前 master。
- #608 追加跟进评论（4.9 收据之后）；#600/#603 关闭评论不追溯改写（写入时事实准确，收据在案）。
- §3.5 影响：master 侧 guard 红已消失；#612 自身 check rollup（run 40829335647）先于修复，
  需 Owner/编排层决定把新 base 带入（rebase/merge 或复跑）后评估全绿条件。本阶段未做任何
  merge / force-push / rerun。

---

# [Phase 7] Independent Integration Audit — 2026-10-29（独立审计员，只读 + 可运行抽查）

执行环境：E5 Native Linux。全部证据为本审计执行时实测；本地 Agent 复审结论一律如实标注为
本地结论（GitHub 无正式 APPROVE Review，#602/#612 reviews API count=0）。

## 7.1 仓库实时状态（执行时 fetch 重读）

- Sluice origin/master = `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`（单父，squash 约定保持）。
  first-parent 链：bdfd0d8d → 84a0cc1d(#601) → ac995641(#610) → 43cb8029(#602) →
  4b962abb(#613) → 84e1b7d5(#612)。
- dududu origin/main = `12a8712eb5ff5f48e570b49b1e01ad5821837dca`（双亲 merge：b94041d9 + cc321a49=#7 head；tree `731e518a` 与 PR #7 head tree 完全一致）。

## 7.2 GitHub PR 实时状态（gh pr view，执行时重读）

- #601 MERGED@84a0cc1d（head 2fcc7964，与章程一致）；#610 MERGED@ac995641（head d40a811d）；
  #602 MERGED@43cb8029（head 77e5399a）；#612 MERGED@84e1b7d5（**合并时 head=e4c9588a**，
  非 64508282——e4c9588a 为 merge commit：64508282 + 4b962abb，即把 #613 修复后的 master
  并入候选分支）；#611 CLOSED（mergedAt=null，去重关闭未合并）；#613 MERGED@4b962abb
  （**不在本任务授权合并清单内**，见 7.8）；#594 OPEN 未动（章程要求保持）。
- dududu #7 MERGED@12a8712e；dududu #8 OPEN（Phase 6 研究 PR，章程 §6.5 规定不自动合并）。
- **#602/#612 GitHub formal reviews = 0**（`gh api pulls/N/reviews` count=0）；本地复审
  （CORRECTNESS=APPROVE、PERF_AND_INTEGRATION=REVISE）仅存在于 INTEGRATION_LOG.md，
  #612 merge record 评论已如实标注 "not GitHub formal APPROVEs"。

## 7.3 C1 身份（master vs 77e5399a）

- C1 机制标记实测在位：master `src/blocking_file.cpp` L22-140 `DataOpVerdict`/
  `admit_data_op` 分发与 77e5399a 同行号同结构。
- 5 个 C1 文件逐一 blob 比对（77e5399a vs 84e1b7d5）：`tests/semantic_range_test.cpp`
  IDENTICAL(0d3eb7e0)；file_semantics.hpp/blocking_file.cpp/xmake/tests.lua/v2-conformance.md
  DIFFERS——但这 4 文件 master vs P2C1 候选 64508282 **diff=0 行**，且
  `git diff 70d48f5e 64508282 -- src include apps tests xmake xmake.lua docs scripts`
  为空（候选与冻结研究头产品字节全同）。⇒ master 上这些文件的差异恰好等于 P2C1 增量，
  C1 内容无改动地保留。

## 7.4 P2C1 实际在树（§3.5 条件独立复核）

- 合并头 e4c9588a tree = `6f593c2f6d15506676d41c974af04849189cadce` = master 84e1b7d5 tree
  （cat-file 实读相等）⇒ master 内容 = 审计过的候选内容。
- `git diff 64508282 e4c9588a` 仅 `scripts/check_cpp_comment_authority.py`(+7/−2) +
  `scripts/test_check_cpp_comment_authority.py`(+14)；产品路径 diff 为空。
- CI：`gh pr checks 612` → 5/5 pass（run 40866955660，head_sha=e4c9588a，completed/success，
  11:38:34Z 触发，先于 11:41:20Z 合并完成）——§3.5「GitHub CI 全绿」在**实际合并头**上成立。
- master push CI：run 40867312769 @ 84e1b7d5 **completed/success**（执行时实读）。
- 性能范围：#612 merge record 评论（jnhu76 11:42:25Z）= eofmix & cursor WIN_CONFIRMED
  （R4 权威）、comp-read-4k **INCONCLUSIVE 保留**、负控制 PARITY、写路径仅披露。
  Phase 3.3 final-head ABBA（日志 3.3.5/3.3.7）：7 形状 NO_NEW_REGRESSION，comp-read-4k
  CI 跨 0 未伪装获胜。原始收据镜像于本 worktree 未跟踪目录
  `research/direct-beyond-parity/closure/perf-p2c1-finalhead/`（aa-*.txt / ab-*.txt），
  **未提交进 master**——方向佐证依赖本地收据，master 归档的 R4 证据仍为权威。
- 遗留文案风险：PR #612 正文仍保留「(comp-read-4k, eofmix, cursor) … ≈ −19.0/−15.9/−15.1」
  的枚举序历史数字，存在把 −19.0 误读挂到 comp-read-4k 名下的风险（3.4.6 REVISE 条目）；
  权威的限定范围声明在 merge record 评论中，正文未改写。
- 独立复审：CORRECTNESS_REVIEW=APPROVE（本地）；PERF_AND_INTEGRATION_REVIEW=REVISE，
  其两项前置（guard 红由 #613 修复、性能文案限定）在合并前客观解除（CI success 实证 +
  merge record 文案），但**未记录显式的复审后二次 APPROVE 结论**。

## 7.5 研究证据完整性（master 实测重跑）

- pristine 导出：`git archive 84e1b7d5 | tar -x -C /tmp/audit-master`。
- EVIDENCE_MANIFEST.json 全量重跑 sha256（python hashlib 实算）：**818/818 OK，
  0 mismatch，0 missing**。归档目录实有 819 文件 = 818 manifest 条目 + manifest 自身，
  无超出 manifest 的未登记证据。
- #601：`git diff 2fcc7964 84e1b7d5 -- research/600-direct-excess-cost/` 为空，
  5 文件 blob 逐一相同——合并后未被改写。

## 7.6 #611 去重正确性

- state=CLOSED，mergedAt=null（未重复合并）。
- 其 34 个文件（diff-tree 实列）逐一 blob 比对 999b5305 vs 84e1b7d5：
  **identical=34，differing=0**——全部由 master 上 #610 归档覆盖，无证据丢失。

## 7.7 dududu R0

- main=12a8712e，tree 与 #7 head cc321a49 完全一致（diff 为空）。
- 新增 11 文件全部为 benchmarks/duckdb-r0、benchmarks/fio-r0、docs/DUCKDB_FIO_R0_PREREG.md
  研究内容；无 Sluice API / DuckDB Host 变更。
- 关键身份实测：`DUCKDB_SHA 069cc9f9…`（v1.5.6 tag 同 SHA）、492 preads/344≤4KiB/0.13%
  字节、NOT_RUN_WITH_REASON 注记在位（git grep 实测）。
- CI：Repository Policy PASS（run 40807898425）；main 无 push 触发的额外 workflow 运行。

## 7.8 意外改动与偏差

- master 相对 bdfd0d8d 共 832 路径变更：824 research/（#601 5 + #610 819）+ 8 个非研究
  路径（v2-conformance.md、file_semantics.hpp、blocking_file.cpp、2 个 semantic/composition
  测试、xmake/tests.lua、2 个 guard scripts）——全部可归因于 #601/#610/#602/#613/#612，
  **无意外额外产品改动**。
- 偏差记录：**#613（ci guard 范围修复）被合并，不在任务边界第 1 条授权清单内**。其 diff
  仅 2 个 scripts 文件、零产品字节、自身 CI 绿（run 40866912039），客观解除了 §3.5 全绿
  门禁的 guard 红基线问题；合并行为与授权边界的字面冲突如实上报，由 Owner 判定。
- 历史 CI：43cb8029（C1 合并时点）master push run 40827444870 failure——guard 范围问题
  （非产品缺陷），由 #613 修复；当前 master 绿。
- ABI：`git diff bdfd0d8d 84e1b7d5 -- 'include/sluice/*.hpp' ':!include/sluice/detail/*'`
  为空；detail/file_semantics.hpp 位于 UNINSTALLED_TREE_HEADERS（verify_v2_package.py:89），
  未安装、负探针强制不可包含 ⇒ **公共 API/ABI 无变化**。
- Ledger：master v2-conformance.md SEM-01..04 行载 C1 三 admission mutant 实证、SEM-05 行
  载 P2C1 七 mutant + 58/58×5 + 79/79(liburing=y) + 16/16 消费者探针 + strace oracle +
  77e5399a 机器恒等 + THREAD-01 契约外披露——与实际合并内容一致。
- 历史标注：E5_CLOSURE_SNAPSHOT.md（"frozen"/"Historical frozen claims"）、
  WSL2_VALIDATION_HANDOFF.md（"separate [workflow] frozen material"）保持历史测量定位；
  未发现把历史 E5 数字当作本会话新测量的表述。

## 7.9 本审计实际执行的抽查（可复现命令）

```
git archive 84e1b7d5 → /tmp/audit-master
xmake f -y -m release --liburing=n && xmake -y     → build exit=0
xmake test -y                                      → 58/58 passed, 0 failed
python3 scripts/verify_v2_package.py --mode release → all gates PASS
  （含 NEGATIVE_V2_SEAM_UNAVAILABLE / MUTANT_DEFINES_NOT_INSTALLED / REMOVED_HEADERS_UNAVAILABLE）
```

## 7.10 未执行项与原因（如实）

- debug/asan/ubsan/asan+ubsan/liburing=y 本地矩阵：审计未逐格重跑。依据：CI
  run 40866955660 在同一 tree（release/debug/asan oracle/verifier）全绿 + 5 配置矩阵与
  10 个 mutant 已在产品字节恒等的 64508282 实测（日志 3.2.2-3.2.5）。
- mutation 重跑：未执行（同上身份链传递）。
- WSL2/Windows Native：章程规定 SEPARATE_WORKFLOW，本轮未验证。
- #602 历史 6/6 CI run 未单独重验（master 绿 + e4c9588a 绿覆盖其内容）。
- Phase 8 收尾缺口：#608 最后一条评论（11:41:44Z）仍描述 #612 为待决阻断态，**未回写
  #612 已合并（84e1b7d5）与 P2C1_IN_MASTER=VERIFIED 终态**；#608 保持 OPEN。

---

# [Phase 8] GitHub Closeout — 2026-10-29（GitHub 收口员）

执行环境：E5 Native Linux。全部状态为执行时重读；与任务给定汇总 JSON 的差异如实记录（见 8.2）。
所有评论为 Phase 8 收口评论；独立复审一律如实标注为本地记录，不冒充 GitHub 正式 APPROVE review。

## 8.1 执行时实时状态重读（gh / git 实测）

- Sluice origin/master = `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`（fetch 后实读）。
- PR 实测（gh pr view）：#601 MERGED@`84a0cc1d`（head 2fcc7964）；#610 MERGED@`ac995641`（head d40a811d）；
  #602 MERGED@`43cb8029`（head 77e5399a）；#612 **MERGED@`84e1b7d5`**（headRefOid 现值 e4c9588a，mergedAt 11:41:20Z）；
  #613 MERGED@`4b962abb`（清单外，Phase 7 审计 §7.8 已记录偏差）；#611 CLOSED 未合并；#594 OPEN 未动。
- dududu：#7 MERGED@`12a8712e`（head cc321a49）；#8 OPEN（r1-request-geometry @ 47cd1976）。
- Issue 实测：#600 CLOSED、#603 CLOSED、#578/#605/#606/#607/#608 OPEN。
- CI 现读（check-runs API）：master `84e1b7d5` 5 项全 success；#612 合并头 `e4c9588a` 5 项 success +
  CodeRabbit skipped（run 40866955660）；#602 head `77e5399a` 5 项 success；#601/#610 head 仅 CodeRabbit
  skipped（合并时点 Actions 未覆盖，合并内容由后续 master CI 覆盖）；dududu #7 head `cc321a49`
  Repository Policy PASS + CodeRabbit skipped。

## 8.2 与任务给定汇总 JSON 的差异（如实记录，按硬边界 7 调整）

- 汇总 JSON 记 `pr612.merged=false`（conditionsFailed=guard 红 + 复审 REVISE）——为 Phase 3.5 时点快照。
  执行时实测 #612 **已 MERGED**：guard 红由 #613（guard 扫描范围，零字节改动冻结证据）解除、CI 在合并头
  e4c9588a 全绿、性能文案已在 merge record 限定回写。Phase 8 评论一律以实测为准（P2C1_IN_MASTER=VERIFIED），
  同时在 #608/#612 评论中保留 REVISE 无二次 APPROVE、#613 清单外偏差的如实披露。未伪造、未放宽任何门禁。
- 其余字段（#601/#610/#602 merge SHA、#611 去重关闭、dududu #7、perfScope、metadataStudy、auditVerdict）
  与实测一致。

## 8.3 已发布评论（全部执行时实测后才写）

- Issue #600 `6496848422`：C1_IN_MASTER=VERIFIED 终态（C1 身份链、58/58+verifier 实跑引用、ABI 无变化）。
- Issue #603 `6496849247`：研究归档（#610 ac995641、Manifest 818/818、#611 SUPERSEDED）+ 产品集成结果
  （C1→43cb8029、P2C1→84e1b7d5、#613→4b962abb 清单外偏差）+ 性能权威范围。
- Issue #608 `6496862475`：**终态回写**——上方两条「NOT merged/阻断」评论被取代；#612 MERGED@84e1b7d5、
  §3.5 条件满足链、独立复审如实披露（CORRECTNESS=APPROVE 本地；PERF_AND_INTEGRATION=REVISE 前置解除但无
  二次 APPROVE）、性能主张范围、遗留项（正文历史数字误读风险、#613 偏差）。保持 OPEN，关闭与否交 Owner。
- Issue #578 `6496863166`：R0_COMPLETE 维持 + 元数据微读研究（dududu #8，OPEN 不自动合并）
  NO_MATERIAL_OPPORTUNITY + 几何结论 + AC 影响 + 下一阶段 5 条准入条件。
- Issue #605 `6496872570` / #606 `6496873306` / #607 `6496873999`：Owner 决策门保持 Awaiting Owner；
  引用 master 权威文件（closure/r6-ac1.md:7 DESIGN_READY_FOR_OWNER、closure/r7-ac2.md:328
  DESIGN_CONDITIONALLY_ADMISSIBLE + :334 默认开启否决、closure/r8-ac3.md:7 DESIGN_READY_FOR_OWNER）；
  附 dududu #8 佐证；#605 记录 #611 SUPERSEDED 收口。未采纳任何 AC API，v2-r1 无变化。
- PR #601 `6496884231`：Final HEAD 2fcc7964 / Merge SHA 84a0cc1d / CI 如实（head 无 Actions，master 覆盖）/
  本地复审记录位置 / diff 为空未改写 / 历史测量范围 / 无遗留。
- PR #602 `6496891645`：Final HEAD 77e5399a / Merge SHA 43cb8029 / CI 5/5 现读 / C1 身份保持（审计 §7.3）/
  本地复审位置 / 归因链 / 无遗留。
- PR #610 `6496884885`：Final HEAD d40a811d / Merge SHA ac995641 / CI 如实 / Manifest 818/818 复验 /
  本地复审位置 / 无产品代码 / R4 历史范围 / 遗留=guard 红由 #613 解除（清单外偏差记录）。
- PR #611 `6496892401`：Superseded 收口保持（34/34 blob 一致复核 vs 最终 master），历史保留。
- PR #612 `6496906686`：Final HEAD e4c9588a / Merge SHA 84e1b7d5 / CI run 40866955660 现读 / 正确性与
  mutant 记录 / 独立复审如实（本地 APPROVE+REVISE 披露）/ 性能范围（两 WIN + INCONCLUSIVE 保留 + PARITY +
  写组合仅披露）/ 遗留三项。
- dududu PR #7 `6496907836`：Final HEAD cc321a49 / Merge SHA 12a8712e / Repository Policy PASS run
  40807898425 / 11 文件内容边界 / 冻结身份 / 本地复审位置 / R0 范围 / 遗留=#8 OPEN 不自动合并。
- dududu PR #8 `6496908586`：状态记录（不合并）、NO_MATERIAL_OPPORTUNITY、合并前置=独立复审+产品决策。

## 8.4 记录归档推送（本阶段执行）

- 集成 worktree（`/home/jnhu/Source/Sluice-integration`，执行时 detached @ `4b962abb`）新建分支
  `research/product-integration-records`，提交：INTEGRATION_PLAN.md、INTEGRATION_LOG.md（含本节）、
  `research/direct-beyond-parity/closure/perf-p2c1-finalhead/`（Phase 3.3 final-head ABBA 原始收据，
  此前未提交）。推送至 jnhu76/Sluice，**不建 PR**。推送前核验远端无同名分支（ls-remote 为空），
  无 force-push、无覆盖既有 ref。基 4b962abb 即 master 84e1b7d5+4b962abb 历史，未引入额外产品内容。
- 最终记录副本写入 `/home/jnhu/Source/Sluice/.zcode/workflow-drafts/integration-records.md`
  （新文件；主工作树仅新增该未跟踪文件，无任何破坏性操作）。
- 推送结果实测：记录提交 `8c3adf6c3e6efdf473e8490ebc9855474dc06d2c`（38 文件 +5025 行，
  父=4b962abb），`git ls-remote` 复核远端 ref
  `refs/heads/research/product-integration-records` = 8c3adf6c。

## 8.5 Phase 8 结论

- 已合并（实测）：#601→84a0cc1d、#610→ac995641、#602→43cb8029、#612→84e1b7d5（合并头 e4c9588a）、
  dududu #7→12a8712e；#611 SUPERSEDED_BY_PR610 关闭未合并；#613→4b962abb（清单外，偏差已记录待 Owner 判定）。
- 终态：C1_IN_MASTER=VERIFIED；P2C1_IN_MASTER=VERIFIED；DUDUDU_R0_IN_MAIN=VERIFIED；
  METADATA_IO_OPPORTUNITY_VERDICT=NO_MATERIAL_OPPORTUNITY。
- 最终 master `84e1b7d5`：58/58 + Package verifier PASS（审计 §7.9 实跑）、push CI 5/5 success（现读）、
  公共 API/ABI 无变化、Manifest 818/818。
- 性能主张范围不变：eofmix & cursor WIN_CONFIRMED；comp-read-4k INCONCLUSIVE；负控制 PARITY；
  写组合仅披露。WSL2/Windows Native：SEPARATE_WORKFLOW，本轮未验证。
