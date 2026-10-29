# 分支 dududu/sluice-r0-closure 保留价值审计

审计人:分支保留价值审计员(动态工作流子代理)。日期:2026-10-29。
对象:`origin/sluice-r0-closure` @ `cc053cf9258b98546b16f9d2f08c5559fa73bc77`
("bench(duckdb): record dataset and oracle prep receipts",仓库 jnhu76/dududu,
本地审计副本 `/tmp/dududu-audit`,origin = `https://github.com/jnhu76/dududu.git`)。
性质:纯审计与非破坏性验证,未修改任何产品代码,未删除任何分支/Worktree/证据目录,
未执行 merge/close/force-push 类操作;本轮仅出结论。

## 0. 逐项必查结果(全部在本会话实际执行,git 均 -C 指向 /tmp/dududu-audit)

| 必查项 | 命令 | 结果 |
|---|---|---|
| HEAD SHA | `git rev-parse origin/sluice-r0-closure` | `cc053cf9258b98546b16f9d2f08c5559fa73bc77`,**与预期一致**;receipts/r0closure-01 |
| 远端引用交叉验证 | `git ls-remote origin 'refs/heads/*' 'refs/pull/*/head'` | `refs/heads/sluice-r0-closure` = 同一 SHA;receipts/r0closure-01 |
| Associated PR | 按 ask 提供的 PR 列表按 headRef 匹配 | **None**(列表无 headRef=`sluice-r0-closure` 条目;dududu 仅 PR #7 head=`cc321a49`(MERGED,sluice-r0-e5-verdict)、PR #8 head=`47cd1976`(OPEN,r1-request-geometry)) |
| PR refs 交叉验证 | `git ls-remote origin 'refs/pull/*/head'` | pull refs 仅 1/2/3/7/8,**无任何 `refs/pull/N/head` 指向 cc053cf9**;receipts/r0closure-01 |
| Commit reachability(包含该 SHA 的权威 ref) | `git branch -a --contains cc053cf9…` | `main`(本地)、`origin/main`、`origin/r1-request-geometry`、`origin/r5-async-host-probe`、`origin/sluice-r0-closure`(自身)、`origin/sluice-r0-e5-verdict` —— 即**除自身外 4 个远端分支 + main 全部包含**;receipts/r0closure-02 |
| 独有提交数 | `git rev-list --count main..origin/sluice-r0-closure` | **0**;receipts/r0closure-02 |
| 祖先关系 | `git merge-base --is-ancestor origin/sluice-r0-closure main` | 成立(tip 是 main 的祖先);receipts/r0closure-03 |
| 独有对象 | `git rev-list --objects origin/sluice-r0-closure ^main` | **空**(零独有 commit/tree/blob);receipts/r0closure-03 |
| 三点 diff | `git diff --name-only main...origin/sluice-r0-closure` | **空**(merge-base = branch tip,零变更文件);"逐文件 blob 比对"因此退化为对分支自身新增文件的直接比对,见下行;receipts/r0closure-03 |
| 分支自身新增文件 | `git diff --stat b94041d..cc053cf9` | 7 个文件、+431 行:`benchmarks/duckdb-r0/{README.md,oracle.sql,results/prep-record.md}`、`benchmarks/fio-r0/{README.md,r0-randrw-4k.fio,r0-read-4k.fio,r0-seqread-1m.fio}`;receipts/r0closure-05 |
| 逐文件 blob 比对(分支 tip vs main) | 对上述 7 文件逐一 `git rev-parse <sha>:path` 与 `git rev-parse main:path` | **7/7 SAME**(blob SHA 完全一致),内容已全部进入默认分支;receipts/r0closure-05 |
| 进入 main 的路径 | `git log --graph --oneline main` | cc053cf9(及其前驱 732a8fb、bbce768)经 `4eafbc0 → a57fb09 → cc321a4`(即 PR #7 分支 `sluice-r0-e5-verdict`)由合并提交 `12a8712`("Merge pull request #7 from jnhu76/sluice-r0-e5-verdict")进入 main;main 领先该 tip 4 个提交;receipts/r0closure-04 |
| 活跃依赖:OPEN PR head | 见上 PR 匹配 | 否(OPEN PR #8 的 head 是 `r1-request-geometry`,非本分支) |
| 活跃依赖:脚本/CI 引用 | 两仓库全树 `grep -rn 'sluice-r0-closure'`;`grep -rn 'r0-closure' /tmp/dududu-audit/.github` | 仅文档命中(dududu 4 处 .md、Sluice 3 处 .md,均为 provenance 记录且已记载 SHA);**无脚本/CI 命中**(dududu 唯一 workflow 为 repo-policy.yml);receipts/r0closure-06 |
| 活跃依赖:Worktree 检出 | `git -C /home/hoo/Projects/Sluice worktree list`;`git -C /tmp/dududu-audit worktree list`;`ps aux \| grep -i r0-closure` | 两仓库共 16 个 worktree,无一检出本分支;无运行中进程引用该分支;receipts/r0closure-07 |
| Working tree dirty | (前提:被 worktree 检出) | **不适用**:分支未被任何 worktree 检出,无可测量的 dirty 状态;receipts/r0closure-07 |
| Recovery:SHA 可拉取 | `git fetch origin cc053cf9258b98546b16f9d2f08c5559fa73bc77` | exit 0,`rev-parse FETCH_HEAD` = 同一 SHA —— 远端当前可按裸 SHA 拉取;receipts/r0closure-08 |

## 1. 分支内容定性

分支 tip `cc053cf9`(2026-10-29)及其独有于 `b94041d` 的两个前驱
(`bbce768` "bench(fio): freeze e5 native storage baseline job set"、
`732a8fb` "bench(duckdb): pin the sql oracle and io exposure protocol")
只新增 **基准收据与作业定义**(duckdb-r0 oracle/README/prep 收据、fio-r0 三个 fio
作业 + README),无任何产品代码。这些内容已 100% 进入 dududu `main`(7/7 blob
一致),并通过 PR #7 的合并提交 `12a8712` 成为 main 历史。

## 2. Unique evidence 检查结论

分支相对 main **不存在任何独有对象**(零 commit/tree/blob),因此不存在"独有
研究文档/脚本/收据"需要被 master 或 #610 归档目录覆盖——覆盖问题对本分支是空集。
分支内容所属的原始存储位置就是 dududu `main`(远端 GitHub),即已处于其长期权威
位置;Sluice 侧对应的 PREREG/prep 文档
(`research/direct-beyond-parity/closure/`、commit `ac995641` 归档)也仅引用分支名
与 SHA 作 provenance,引用的 SHA `cc053cf` 在分支删除后仍可从 main 历史解析。

## 3. Active dependency 检查结论

- 非 OPEN PR head:成立(无任何 PR 引用该 SHA;OPEN PR #8 head 为 `r1-request-geometry`)。
- 脚本/CI:无引用;两仓库仅 7 处 .md 文档按 provenance 记载分支名,且都同时记载了
  SHA,分支删除后文档语义不受影响。
- Worktree/进程:无检出、无运行中依赖。
- 注:`origin/r5-async-host-probe`、`origin/sluice-r0-e5-verdict`、
  `origin/r1-request-geometry`(OPEN PR #8 head)均以本分支 tip 为祖先 ——
  这些 ref 在本分支删除后继续维持 `cc053cf9` 在远端可达。

## 4. Recovery(删除后的恢复步骤)

完整 HEAD SHA(已记录):`cc053cf9258b98546b16f9d2f08c5559fa73bc77`

1. 该 commit 是 `origin/main` 的祖先(经 PR #7 合并提交 `12a8712` 进入 main 历史),
   删除 `refs/heads/sluice-r0-closure` 后**依然从 main 永久可达**,任何
   `git fetch origin` 都能取回全部对象。
2. 即使不依赖 main,远端另外 3 个分支(`r5-async-host-probe`、`sluice-r0-e5-verdict`、
   `r1-request-geometry`)也包含该 SHA。无 `refs/pull/N/head` 直接指向它,但不需要。
3. 重建分支(任意已 fetch 的克隆):
   `git fetch origin && git branch sluice-r0-closure cc053cf9258b98546b16f9d2f08c5559fa73bc77`
   然后 `git push origin sluice-r0-closure`;或直接
   `git push origin cc053cf9258b98546b16f9d2f08c5559fa73bc77:refs/heads/sluice-r0-closure`。
4. 本次已实测:删除前 `git fetch origin cc053cf9…`(裸 SHA 拉取)在远端成功
   (receipts/r0closure-08)。删除后该裸 SHA 路径不保证继续可用(GitHub 对不可达
   SHA 的按需拉取无承诺),故恢复路径以第 1 条(main 可达)为准。

## 5. decision 规则逐条核对(DELETE_CANDIDATE 六条件)

| 条件 | 结论 | 依据 |
|---|---|---|
| 非 OPEN PR head | ✓ | receipts/r0closure-01(pull refs 与 ask 列表双重确认) |
| 无独有未归档数据 | ✓ | 零独有对象 + 7/7 blob 一致;receipts/r0closure-03、-05 |
| 无未推送产品变更 | ✓ | 远端 `refs/heads/sluice-r0-closure` = 本地所见同一 SHA,无未推送提交;分支无产品代码(receipts/r0closure-01、-05) |
| 相关原始数据已有可靠备份 | ✓ | 内容与对象全部在远端 origin/main(及 3 个其他远端分支)持久可达 |
| 无运行中 Agent/Worktree 依赖 | ✓ | 16 个 worktree 无一检出;无进程引用;receipts/r0closure-07 |
| 已记录完整 HEAD SHA 与恢复步骤 | ✓ | 本报告 §4 |

**decision = DELETE_CANDIDATE。** 本阶段仅出结论,未执行任何删除。

## 6. 边界与未执行项

- 未检查(也无法在本会话枚举)机器上可能存在的其他 dududu 克隆是否检出该分支;
  已检查的两个权威位置(Sluice 主工作区 + 其全部 15 个 /tmp worktree、
  /tmp/dududu-audit)均无依赖。此项残余不确定性不改变结论:即使某克隆检出该分支,
  对象仍从 origin/main 可达,恢复路径完整。
- Working tree dirty 检查因分支未被任何 worktree 检出而不适用(非未执行,
  而是无被检对象)。
- 本审计只读访问远端(fetch/ls-remote);未触碰任何 PR 状态、未推送任何引用。

## 7. 收据索引

`receipts/r0closure-01-head-and-remote-refs.txt`(HEAD SHA + 远端分支/pull refs)、
`r0closure-02-commit-reachability.txt`(contains + 独有提交数)、
`r0closure-03-blob-reachability.txt`(祖先 + 零独有对象 + 空三点 diff)、
`r0closure-04-merge-path.txt`(PR #7 合并路径 + tip 元数据)、
`r0closure-05-perfile-blob-compare.txt`(7 文件 blob 逐一比对)、
`r0closure-06-branch-name-references.txt`(两仓库引用 + CI grep)、
`r0closure-07-worktrees.txt`(worktree/进程依赖)、
`r0closure-08-sha-fetch-recovery.txt`(裸 SHA 拉取实测)。
