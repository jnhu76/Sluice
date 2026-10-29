# 分支保留复核（r2）：`sluice-r0-e5-verdict`（dududu）

复核人：分支保留复核子代理（动态工作流）。日期：2026-10-30。
性质：对原审计
[`branch-sluice-r0-e5-verdict-retention-audit.md`](branch-sluice-r0-e5-verdict-retention-audit.md)
（2026-10-29，结论 DELETE_CANDIDATE）的**只读复核**。仅重验 ask 指定的三个最要害字段：
`uniqueEvidence`、`activeDependency`、`recovery`。未修改任何产品代码，未 merge/close 任何
PR，未删除任何分支/Worktree/证据目录，未 force push，未执行 fetch。

结论：**DELETE_CANDIDATE 维持不变**。三个字段全部复核通过，且 uniqueEvidence 的证据强度
比原审计更高（树级逐位等同）。收据：`receipts/branch-sluice-r0-e5-verdict-r2-01…05-*.txt`。

## 0. 复核环境与新鲜度

- 审计克隆 `/tmp/dududu-audit` 仍在（origin = `https://github.com/jnhu76/dududu.git`），
  位于 main（12a8712e），`git status --porcelain` 为 0 行，全程只读使用。
- 未 fetch；以只读 `ls-remote` 核对远程：`refs/heads/main` = `12a8712eb5…`、
  `refs/heads/sluice-r0-e5-verdict` = `cc321a4994…`、`refs/pull/7/head` = `cc321a4994…`，
  与本地 remote-tracking 引用逐位一致（receipts/r2-01）→ 本地比对等效于对当前远程状态的
  比对。HEAD 与原审计一致：`cc321a4994ee638493fecda0c437481528f5cea9`。

## 1. uniqueEvidence 复核 ✅（结论不变，证据更强）

重跑 ask 指定检查（receipts/r2-03）：

```
$ git rev-list --objects origin/sluice-r0-e5-verdict --not origin/main
→ 0 行（零独有对象）
$ git rev-list --count origin/main..origin/sluice-r0-e5-verdict
→ 0（零独有提交）
$ git diff --name-status origin/main origin/sluice-r0-e5-verdict
→ 空（exit=0）
$ git rev-parse origin/main^{tree} origin/sluice-r0-e5-verdict^{tree}
→ 731e518af903fe49beb798d80d269eeaceb53794 == 731e518af903fe49beb798d80d269eeaceb53794
```

**对原审计口径的修正（不影响结论，仅加强）**：原审计称"分支新增的 2 个文件"。实测相对
合并前 main（`b94041d9`），PR #7 分支经 **6 个提交**引入 **11 个文件**（整个
duckdb-r0/fio-r0 bench 脚手架 + 2 份收据文档）。本次将 11 个文件**全部**逐一做 blob 级
比对：**11/11 IDENTICAL**（receipts/r2-03），且分支 tip 树 SHA 与 origin/main 树 SHA
逐位相同——即分支的全部内容（含原审计未逐一列出的 9 个脚手架文件）字节级保留于 main。
两份收据文档的 SHA-256 双侧一致：
prereg `b617adf4…`、lane-record `74500d79…`。

交叉佐证复核：sluice #610 归档副本 `research/direct-beyond-parity/closure/DUCKDB_FIO_R0_PREREG.md`
= 295 行，dududu 版本 = 301 行——与原审计 receipts/05 记录一致（两个宿主副本，dududu 副本
由 dududu main 保留，sluice 变体差异不影响本判定）。原始运行输出归档目录
`research/direct-beyond-parity/closure/r5-native/` 实测存在（未进 dududu git 系设计，
lane-record 头部 rule 6）。**无独有未归档内容，判定成立。**

## 2. activeDependency 复核 ✅

全部重跑（receipts/r2-04）：

| 检查 | 命令 | 结果 |
|---|---|---|
| 脚本/CI 引用（dududu） | `git grep -n 'sluice-r0-e5-verdict' origin/main --` | 无命中（exit=1） |
| 脚本/CI 引用（sluice） | `git -C /home/hoo/Projects/Sluice grep -n … master --` | 无命中（exit=1） |
| Worktree | `git -C /tmp/dududu-audit worktree list` | 仅审计克隆自身（main） |
| 其他本地副本 | `find /tmp /home/hoo/Projects -maxdepth 3 -type d -iname '*dududu*'` | /tmp/dududu-audit、/tmp/sluice-val-dududu-recovery-clone |
| 进程 | `ps -eo pid,etime,cmd \| grep -iE 'dududu\|r0-e5'` | 无命中（exit=1） |
| PR 状态 | `gh pr view 7 --repo jnhu76/dududu --json …` | `state=MERGED`，headRefOid=cc321a4994…（逐位一致），merged 2026-10-29T04:47:38Z |
| OPEN PR head | `gh pr list --repo jnhu76/dududu --state open` | 仅 #8 `r1-request-geometry`，非本分支 |

**环境变化（如实记录）**：`/tmp/sluice-val-dududu-recovery-clone` 是原审计之后新出现的
目录（并行验证子代理的恢复验证克隆）。只读检查：位于 main（12a8712，工作区干净），仅含
main 分支，`rev-parse refs/heads/sluice-r0-e5-verdict` 失败——**不持有被审计分支**，
非活跃依赖。原审计"磁盘上无其他 dududu 工作副本"在原时点为真；现时点多出此临时验证克隆，
与本分支无依赖关系。**无活跃依赖，判定成立。**

## 3. recovery 复核 ✅

三层恢复路径全部实测存活（receipts/r2-05、r2-01、r2-02）：

1. **内容已在默认分支**：`merge-base --is-ancestor cc321a49 origin/main` → exit=0；
   真 merge commit `12a8712e`（parents `b94041d9` + `cc321a49`，cat-file 实测）；
   且分支 tip 树 = main 树（731e518a 双侧一致）。
2. **远程分支引用仍直接持有 tip**：`ls-remote` → `refs/heads/sluice-r0-e5-verdict`
   = `cc321a4994ee638493fecda0c437481528f5cea9`。
3. **已合并 PR 的 pull 引用**：`ls-remote` → `refs/pull/7/head` =
   `cc321a4994ee638493fecda0c437481528f5cea9`；恢复命令
   `git fetch origin pull/7/head:refs/heads/sluice-r0-e5-verdict`。
   另实测：仓库无 tag（`ls-remote --tags` 空）；`cat-file -t cc321a49` = commit；
   `branch -a --contains cc321a49` 含 main、origin/main、origin/r1-request-geometry、
   origin/sluice-r0-e5-verdict。**SHA 可恢复性成立。**

## 4. 决策

三个字段全部复核通过，无一项需要放宽或收紧；修正仅涉及原审计对分支文件数的描述口径
（2 → 11，全部已在 main），使结论更稳健。**decision = DELETE_CANDIDATE（维持）**。

## 5. 边界说明

- 未执行 `git fetch`（沿用原审计做法：只读 `ls-remote` 核对远程且与本地引用逐位一致，
  等效达成新鲜度验证）；本复核所有比对均基于内容寻址 SHA，不受引用变化影响。
- 远端服务器侧的其他克隆无法从本机枚举；本机范围（/tmp、/home/hoo/Projects）已枚举，
  唯一额外项为 §2 所述的并行验证克隆（不持有本分支）。
- 未实际执行 fetch 恢复命令演练（避免改动克隆引用）；`refs/pull/7/head` 由 ls-remote
  实测存活，恢复命令与原审计报告 §8 一致。
