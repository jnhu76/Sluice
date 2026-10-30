# 分支保留审计：`sluice-r0-e5-verdict`（dududu）

审计人：分支保留审计子代理（动态工作流）。日期：2026-10-29。
性质：纯审计与非破坏性验证；未修改任何产品代码，未 merge/close 任何 PR，未删除任何
分支/Worktree/证据目录，未 force push。本阶段只出结论，不执行删除。
结论：**DELETE_CANDIDATE**。收据见 `receipts/branch-sluice-r0-e5-verdict-01…06-*.txt`
（本会话实际执行的命令与输出）。

## 0. 审计环境

- dududu 本地副本：`/tmp/dududu-audit`（已存在，origin = `https://github.com/jnhu76/dududu.git`，
  默认分支 `main`）。本审计未做 fetch：以只读 `git ls-remote origin` 直接核对远程权威状态，
  其输出与本地 remote-tracking 引用逐位一致（`main` = 12a8712e，分支 = cc321a49），
  即本地引用即当前远程状态（receipts/01）。
- Sluice 仓库（`/home/hoo/Projects/Sluice`）仅用于 #610 归档覆盖比对、分支名引用扫描与
  报告落盘，不涉及本分支。

## 1. HEAD 核对（必查项 1）

```
$ git -C /tmp/dududu-audit rev-parse origin/sluice-r0-e5-verdict
cc321a4994ee638493fecda0c437481528f5cea9
```

完整 HEAD = `cc321a4994ee638493fecda0c437481528f5cea9`，与预期值**一致**。远程核对：

```
$ git -C /tmp/dududu-audit ls-remote origin
12a8712eb5ff5f48e570b49b1e01ad5821837dca	refs/heads/main
cc321a4994ee638493fecda0c437481528f5cea9	refs/heads/sluice-r0-e5-verdict
cc321a4994ee638493fecda0c437481528f5cea9	refs/pull/7/head
```

receipts/01。

## 2. Associated PR（必查项 2）

ask 提供的 10 条 PR 列表按 headRef 匹配 `sluice-r0-e5-verdict`：命中
**dududu PR #7 — MERGED（base main）**，headSha `cc321a4994…` 与 §1 实测一致：
"docs(bench): record e5 r0 verdict and owner-gate prereg"
（https://github.com/jnhu76/dududu/pull/7）。main tip 即其 merge commit：
`12a8712 Merge pull request #7 from jnhu76/sluice-r0-e5-verdict`
（parents = `b94041d9` + `cc321a49`，真合并，receipts/01）。
**非任何 OPEN PR 的 head**（列表中 OPEN 者为 sluice #594 `research/593-key-path`、
dududu #8 `r1-request-geometry`，均非本分支）。

## 3. Commit reachability（必查项 3）

- `git rev-list --count main..origin/sluice-r0-e5-verdict` → **0**（零独有提交）。
- `git merge-base main origin/sluice-r0-e5-verdict` → `cc321a4994…`（= 分支 tip 本身），
  分支 tip 是 main 的祖先。
- `git branch -a --contains cc321a4994…` → `main`、`origin/HEAD`、`origin/main`、
  `origin/r1-request-geometry`、`origin/sluice-r0-e5-verdict`（receipts/02）。引用该 tip
  的权威 ref：`origin/main`（经 merge commit 12a8712e 直接以 parent 关系包含）、
  `origin/r1-request-geometry`（47cd1976，OPEN PR #8 head，因基于合并后的 main 而间接
  包含）、`origin/sluice-r0-e5-verdict`（自身）。删除本分支引用不影响其余包含关系。
- 本地无同名分支（审计克隆仅 `main`），仓库无 tag（`git tag -l | wc -l` → 0）。

## 4. Blob reachability（必查项 4）

- `git rev-list --objects origin/sluice-r0-e5-verdict ^main` → **0 个独有对象**（receipts/03）。
- `git diff --name-only main...origin/sluice-r0-e5-verdict` → **0 个文件**（空 diff）。
- 分支生命周期内新增的 2 个文件逐文件 blob 比对（receipts/04）：

  | 文件 | 分支 blob | main blob | 结果 |
  |---|---|---|---|
  | `benchmarks/duckdb-r0/results/lane-record.md` | `34c17758…` | `34c17758…` | IDENTICAL |
  | `docs/DUCKDB_FIO_R0_PREREG.md` | `2d625d61…` | `2d625d61…` | IDENTICAL |

  内容已在默认分支，且 main 副本内容 SHA-256：
  lane-record `74500d79a1115cc5a1d9b062c41ba4a5b5899aff2901e83aaf707b5bd2b58596`、
  prereg `b617adf4a17fb01c547ddec9321d502c8d32281a9f7faad9dd2398b1bcdf839d`。

## 5. Unique evidence（必查项 5）

- 分支产物仅为两份 bench 收据文档（`docs/DUCKDB_FIO_R0_PREREG.md` = #578 R0 verdict 与
  owner-gate 预注册；`benchmarks/duckdb-r0/results/lane-record.md` = 测量道完成收据），
  **两份均逐字节保留于 dududu main**（§4）。无任何独有对象 → 不存在"独有未归档数据"。
- 与 Sluice #610 归档目录（`research/direct-beyond-parity/closure/`，PR #610，含 sha256
  manifest）的关系（receipts/05）：
  - sluice 归档的 `closure/DUCKDB_FIO_R0_PREREG.md` blob `6fe1a846…` 与 dududu 版本
    `2d625d61…` **不同**：sluice 副本是 sluice-tree 变体（295 行），dududu 版本（301 行）
    是 dududu 侧的对应记录，并指向 `benchmarks/duckdb-r0/results/lane-record.md`。两者是
    同一预注册的两个宿主副本，互为佐证；dududu 副本由 dududu main 完整保留。
  - `lane-record.md` 不在 sluice 仓库、也不在 #610 manifest 中——它只存在于 dududu，
    而 dududu main 已逐字节持有。
  - 原始运行输出（run logs/JSON/strace）按设计**不进入 dududu git**（lane-record 开头
    声明 rule 6），存放于 Sluice 研究树 `research/direct-beyond-parity/closure/r5-native/`，
    即由 #610 归档链管理——属另一审计对象，不构成本分支的独有数据。

## 6. Active dependency（必查项 6）

- **OPEN PR head**：否（§2，PR #7 MERGED）。
- **脚本/CI 引用**：`git grep 'sluice-r0-e5-verdict' main --`（dududu）与
  `git grep 'sluice-r0-e5-verdict' master --`（sluice）均无命中（exit=1）；dududu 工作
  树文件也无命中。Sluice 母仓库 CI/脚本未引用该分支名（receipts/06）。
- **Worktree 检出**：`git -C /tmp/dududu-audit worktree list` 仅显示本审计克隆自身
  （在 main 上）；`find /tmp /home/hoo/Projects -maxdepth 3 -type d -iname '*dududu*'`
  仅命中 `/tmp/dududu-audit`。磁盘上不存在其他 dududu 工作副本或 worktree（receipts/06）。
- **运行中进程**：`ps -eo pid,etime,cmd | grep -iE 'dududu|r0-e5'` 无命中（receipts/06）。

## 7. Working tree（必查项 7）

分支未被任何 worktree 检出 → dirty 检查**不适用（N/A）**。审计克隆位于 main
（12a8712e）且全程只读使用（receipts/06）。

## 8. Recovery（必查项 8）

1. 内容已在默认分支：dududu main 的 merge commit `12a8712eb5ff5f48e570b49b1e01ad5821837dca`
   （parents `b94041d9…` + `cc321a49…`），删除分支引用不丢失任何内容。
2. 远程当前仍直接持有该 tip：`refs/heads/sluice-r0-e5-verdict` = `cc321a4994…`（receipts/01）。
3. 即使日后删除远程分支，GitHub 为已合并 PR 保留 `refs/pull/7/head` = `cc321a4994…`
   （receipts/01 实测）。恢复命令：
   `git fetch origin pull/7/head:refs/heads/sluice-r0-e5-verdict`。
4. 历史可溯源：MERGED PR #7（headSha 与实测一致）https://github.com/jnhu76/dududu/pull/7。
5. 完整 HEAD SHA 与上述步骤已记录于本报告与 receipts/01、06。

## 9. 决策

| 判据 | 结果 |
|---|---|
| 非 OPEN PR head | ✅ PR #7 MERGED，无 OPEN PR 以此为 head |
| 无独有未归档数据 | ✅ 0 独有提交、0 独有对象，2 文件 blob 级等同 main |
| 无未推送产品变更 | ✅ 分支仅存在于远程且为纯 docs/bench 提交；审计克隆本地无该分支，无超前提交；tip 与远程逐位一致 |
| 相关原始数据已有可靠备份 | ✅ dududu main 逐字节持有全部产物；原始运行输出由 Sluice #610 归档链（r5-native/ + sha256 manifest）管理 |
| 无运行中 Agent/Worktree 依赖 | ✅ 无 worktree、无其他本地副本、无进程、无双仓库脚本/CI 引用 |
| 已记录完整 HEAD SHA 与恢复步骤 | ✅ 本报告 §1/§8 + receipts |

**结论：DELETE_CANDIDATE**。全部判据成立；后续如执行删除，仅删远程分支引用
（`git push origin --delete sluice-r0-e5-verdict` 或等价操作）即可，内容经由
main（12a8712e）与 `refs/pull/7/head`（cc321a4994…）双重保留。本阶段不执行删除。

## 10. 边界说明（如实标注）

- 未做 `git fetch origin`（避免远程删除被 --prune 镜像进本地引用导致验证材料丢失）；
  以只读 `ls-remote` 核对远程权威状态并确认本地引用与远程逐位一致，等效达成新鲜度验证。
- dududu 远端服务器侧的其他克隆（若有）无法从本机枚举；本机范围内（/tmp、
  /home/hoo/Projects）已穷举确认无其他检出。
- sluice 归档副本与 dududu 版本 blob 不同属预期（两个宿主各自的副本），已如实记录，
  不影响"无独有未归档数据"的判定（dududu main 即备份）。
