# 分支 "sluice/master" 保留价值审计（Sluice 仓库默认分支）

审计人:分支保留价值审计员(动态工作流子代理)。日期:2026-10-29。
性质:纯审计与非破坏性验证。未修改任何产品代码,未删除任何分支/Worktree/证据目录,
未 merge/close 任何 PR,未 force push。写操作仅限本报告目录与 receipts/。

## 0. 对象澄清(关键前提)

标签 "sluice/master" **不是任何仓库中真实存在的 ref 名**。实证:

- `git -C /home/hoo/Projects/Sluice remote -v` → 仅有 `origin = git@github.com:jnhu76/Sluice.git`,
  没有名为 `sluice` 的 remote(receipts/sluice-master-02-remotes.txt)。
- `git rev-parse sluice/master` 与 `git rev-parse refs/remotes/sluice/master` 均报
  `fatal: ambiguous argument … unknown revision`(receipts/sluice-master-01-head-resolution.txt)。
- `git ls-remote origin`(Sluice)远端分支仅:`master`、`backup/pr-601-original-head-e4bb8833`、
  `backup/pr-612-original-head-70d48f5e`、`research/575-native-e5-atomic-rmw`、
  `research/593-key-path`、`research/product-integration-records`、`research/r6-ac1-vectored-semantics`;
  `git -C /tmp/dududu-audit ls-remote origin`(dududu)远端分支仅:`main`、`r1-request-geometry`、
  `r5-async-host-probe`、`sluice-r0-closure`、`sluice-r0-e5-verdict`。
  两个远端都没有字面 `sluice/master` 分支(receipts/sluice-master-02-remotes.txt)。

因此 "sluice/master" 只能解读为 **"Sluice 仓库的 master 分支" = `/home/hoo/Projects/Sluice` 的
`origin/master`**。任务给的预期 HEAD(84e1b7d578c2c58a3c46033487d9e1ac7c639b8b)与实测一致,
进一步证实该解读。以下所有检查按此对象执行。任务模板中的
`origin/master..origin/master`、`origin/master ^origin/master`、`origin/master...origin/master`
命令因此是平凡空集(被审分支就是默认分支本身),按原样执行并如实记录,另在备注中说明。

## 1. 必查项结果(全部本会话实际执行,git 均带 -C)

| 检查 | 命令 | 结果 | receipt |
|---|---|---|---|
| 完整 HEAD | `git -C …/Sluice rev-parse origin/master` | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`,与预期**一致**,无偏差 | receipts/sluice-master-01 |
| Associated PR | 任务给定的 PR 列表按 headRef 匹配 | 无任何 PR 的 headRef 为 `master`;逐个 headSha 比对 84e1b7d5… 亦无一命中(最近的 #612 headSha 是 e4c9588a…)→ **None** | 本节文字 + 任务材料 |
| Commit reachability(包含关系) | `git branch -a --contains 84e1b7d5…` | `master`、`remotes/origin/HEAD -> origin/master`、`remotes/origin/master` 三个本地 ref;远端 `HEAD` 与 `refs/heads/master` 均指向同一 SHA。`git tag --contains` 为空(4 个 tag:pre-rewrite-2026-09-07、v0.0.1–v0.0.3,均为历史快照,不含此提交) | receipts/sluice-master-03/04 |
| 独有提交数 | `git rev-list --count origin/master..origin/master` | `0`(平凡:两侧同一 ref;被审分支相对默认分支没有任何独有提交——它就是默认分支) | receipts/sluice-master-03 |
| Blob reachability | `git rev-list --objects origin/master ^origin/master \| wc -l` → `0`;`git diff --name-only origin/master...origin/master \| wc -l` → `0` | 独有对象 0、差异文件 0(平凡空集);分支内容 100% 已在默认分支(同一 ref) | receipts/sluice-master-03 |
| Unique evidence | 分支对默认分支 diff 为空 → 无任何独有文档/脚本/收据;且 master 自带 #610 归档:`research/direct-beyond-parity/`(MISSION、P0–P6、EVIDENCE_MANIFEST.json 等,来自提交 ac995641)与 `research/closure/` | 独有未归档数据:**无** | receipts/sluice-master-06 |
| Active dependency | `.github/workflows/ci.yml:6,18` push/pull_request 触发条件含 `- master`;`.github/ISSUE_TEMPLATE/*.yml` 以 `master@<sha>` 为报障占位;`CONTRIBUTING.md:38` 要求从 `master` 切分支 | 默认分支被 CI、issue 模板与贡献流程直接引用,**活跃**;scripts/、xmake 无分支名引用 | receipts/sluice-master-05 |
| Worktree 检出 | `git worktree list` | 主工作区 `/home/hoo/Projects/Sluice` 检出于 `[master]`(84e1b7d5);`/tmp/sluice-val-*` 三个均为 detached(两个在 84e1b7d5,一个在 2714aa65,属其他审计的验证 worktree,非分支检出) | receipts/sluice-master-04 |
| Working tree | `git status --porcelain` | 仅 `?? docs/research/retention-wsl2-audit/`(本审计族自身的报告目录,未跟踪);**无任何已跟踪文件改动**,产品代码干净 | receipts/sluice-master-04 |

## 2. 恢复路径(Recovery)

- 远端仍在:`git ls-remote origin` 显示 `refs/heads/master = 84e1b7d5…`,远端 `HEAD` 同值
  ——即便本地 ref 全删,`git fetch origin master` 即可恢复(receipts/sluice-master-02)。
- GitHub 侧默认分支本身不可被普通删除;`refs/pull/*/head` 全量保留于远端,PR 历史可溯源。
- 另有远端备份 ref `backup/pr-601-original-head-e4bb8833`、`backup/pr-612-original-head-70d48f5e`
  兜底两条近期合并的原 head。
- 本 HEAD SHA 已完整记录于本报告与 receipts/sluice-master-01。

## 3. 决策与依据

**decision = KEEP**(权威且活跃)。

- 该分支是仓库默认分支:`git symbolic-ref refs/remotes/origin/HEAD` → `refs/remotes/origin/master`,
  远端 HEAD 指向同一 SHA;本地 `master` 与其同值。
- 决策规则对照:DELETE_CANDIDATE 六条件中"非 OPEN PR head"虽成立(无 PR 匹配),
  但它是权威分支、被 CI(ci.yml:6,18)、issue 模板、CONTRIBUTING 直接引用,并被主工作区检出
  ——命中"权威或活跃 → KEEP",且"无独有数据/无未推送变更/有备份"等条件对默认分支本身
  概念不适用(它就是权威正本,不是待淘汰副本)。
- 未执行任何删除;本阶段仅出结论。

## 4. 局限与如实说明

- "sluice/master" 作为 ref 名不存在,审计对象按上文第 0 节解读执行;若发起方另有所指
  (例如某个其他克隆里的自定义 remote),本报告范围未覆盖,应以其真实 ref 重新审计。
- 模板命令 `origin/master..origin/master` / `^origin/master` / `...origin/master` 因被审对象
  即默认分支而为平凡空集,已按原样执行并记录;语义上"独有提交/独有对象/差异文件"均为 0。
- 未运行任何构建、未触碰 `/tmp/sluice-val-*` worktree、未修改 dududu(仅对其远端做只读
  `ls-remote`;`/tmp/dududu-audit` 克隆在会话开始前已存在,未重新克隆)。
