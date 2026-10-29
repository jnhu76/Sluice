# 分支保留审计：`research/575-native-e5-atomic-rmw`（Sluice）

审计人：分支保留审计子代理（动态工作流）。日期：2026-10-29。
性质：纯审计与非破坏性验证；未修改任何产品代码，未 merge/close 任何 PR，未删除任何分支/Worktree/证据目录。
结论：**ARCHIVE_FIRST**（有独有未归档研究数据：114 个文件的 #575 E5 原生证据树仅存在于本分支）。
报告收据见 `receipts/branch-575-native-e5-atomic-rmw-01…08-*.txt`（本会话实际执行的命令与输出）。

## 0. Ref 解析与完整 HEAD（必查项 1）

```
$ git -C /home/hoo/Projects/Sluice rev-parse origin/research/575-native-e5-atomic-rmw
e9baafb9d117cbaf387dde9f4433ebbacb4a73b3        (receipts/01)
```

完整 HEAD = `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3`。远程 `origin`
（git@github.com:jnhu76/Sluice.git）上的分支 ref 仍然存在且指向同一 SHA
（`git ls-remote`，receipts/01）。全仓库（本地 clone）仅有一个 ref 指向该
SHA：`refs/remotes/origin/research/575-native-e5-atomic-rmw`（`for-each-ref`
grep，receipts/01）；无本地分支、无 tag。

## 1. Associated PR（必查项 2）

PR 列表（ask 提供）按 headRef 匹配，唯一命中：

| 字段 | 值 |
|---|---|
| PR | **#584** — research(575): native E5 atomic RMW elimination evidence — NO_CHANGE_JUSTIFIED |
| state | **CLOSED（未合并，MERGED=NO）** |
| headRef | `research/575-native-e5-atomic-rmw`（与实际分支名逐字符一致） |
| headSha | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3`（与完整 HEAD 逐位一致） |
| base | master |

列表其余 9 个 PR 无一匹配本分支；无任何 PR（OPEN 或其他）以本分支为 base。
GitHub 端 `refs/pull/584/head` 已实测解析到同一 SHA（receipts/01）——CLOSED
未合并 PR 的 head ref 仍然可达，这是一条独立于分支 ref 的恢复通道。

## 2. Commit reachability（必查项 3）

- `git branch -a --contains e9baafb9…` → 仅
  `remotes/origin/research/575-native-e5-atomic-rmw`；master 不包含，无任何
  本地分支、无任何 tag 包含该 SHA（receipts/02）。
- `git rev-list --count origin/master..branch` → **9**；反向
  （branch..origin/master）→ **10**（分支落后 master 已合并的
  #601/#602/#610/#612 等提交）。
- merge-base = `3df10dc4`（Merge pull request #583 from jnhu76/release/v0.0.3，
  2026-10-23），即分支基线 v0.0.3 master（manifest.json `baseline_sha` 一致）。
- 9 个独有提交全部为 `research(575): …` 系列（R0–R9 证据、runner v1–v3、
  attribution arms、R9 独立评审、closeout 证据完整性修订）。

## 3. Blob reachability（必查项 4）

- `git rev-list --objects branch ^origin/master` → **196 个独有对象**（9
  commits + 树 + 文件 blob）（receipts/03）。
- `git diff --name-status origin/master...branch` → **116 个文件：
  115 A（新增）+ 1 M**（完整清单 receipts/03b）。
  - `docs/research/mw-hw-575-native-e5/` **114 个文件**：13 篇研究文档
    （R0–R9、README、VERDICT）+ `manifest.json` + `assembly/base/` 6 个
    （GIMPLE/objdump/lockadd-sites）+ `patches/` 9 个（三臂 diff、mutation
    diff、plain-increment control diff）+ `raw/` 81 个（A/B、calibration
    A/A、parity 的 jsonl/csv/provenance）+ `scripts/` 4 个（run_575_ab.sh、
    run_575_d8.sh、reduce_575.py、layout_probe.cpp）。
  - `tests/uring_flush_count_oracle_test.cpp`（新增，测试 oracle）。
  - `xmake/tests.lua`（M，+2 行：注册该测试 target）。
- 逐文件 blob 比对（branch tip vs origin/master，`rev-parse --verify
  --quiet <ref>:<path>`）：**SAME=0，ABSENT_ON_MASTER=114，DIFFERENT=2**
  （receipts/03）。
- 全树 blob 交叉核对：178 个带路径的分支独有 blob 中，仅 2 个 SHA 同时存在
  于 origin/master 全历史对象集——`70327072`（分支版 `xmake/tests.lua`，作为
  master 历史祖先状态存在）与 `741eb64c`（`xmake/` 目录树对象，非文件内容）。
  **即：没有任何承载证据内容的独有 blob 在 master 上有备份。**

## 4. 两个共享路径文件的覆盖情况（产品侧变更已落地）

分支对产品/测试面的全部改动只有两处，均已通过 **#585**（squash-merged 为
`1df8b4b8` test(uring): pin positive io_uring transport flush accounting）
进入 master（receipts/04）：

- `tests/uring_flush_count_oracle_test.cpp`：分支 blob `28e77926` vs master
  blob `990cb80c`，`git diff` 全文 **13 行差异全部位于文件头注释**（master
  版将注释收窄到 deterministic fake-submit fixture 的适用范围），代码部分逐字
  一致。
- `xmake/tests.lua`：分支 +2 行注册 `uring_flush_count_oracle_test`；master
  `xmake/tests.lua:578-579` 已有同一注册（经 #585）。

分支 README 自述与实测一致："the merged file is this branch's oracle with
the leading comment scoped to the deterministic fixture … No
research-branch-only code is required by the merged test"（receipts/04）。
**无未推送产品变更。**

## 5. Unique evidence（必查项 5）：114 个文件的证据树为独有且未归档

- master `docs/research/` 下只有 `mw-hw-1`、`native-linux-m2`、
  `state-complexity`、`uring-router-580` 四个目录；**没有**
  `mw-hw-575-native-e5`（receipts/05）。
- #610 归档（commit `ac995641`）内容全部位于 `research/direct-beyond-parity/`
  与 `docs/evidence/`，属 #600/#603 direct campaign，**不覆盖 #575**
  （receipts/05）。
- master 上的 "#575" 引用仅是方法学出处提及（`CHANGELOG.md:108`、
  `bench/research/uring-router-580/run_ab.sh:2`、
  `docs/research/uring-router-580/environment.md:86`、`wsl-results.md:5`、
  测试文件头注释）；`docs/archive/README.md` 的命中是 blob SHA 子串巧合
  （`…0575`）。这些出处引用指向的原始数据只存在于本分支——**master 引用了
  #575 的方法学纪律，却不持有其证据**。
- dududu（/tmp/dududu-audit，origin=https://github.com/jnhu76/dududu.git）：
  `git grep "575"` 与 `575-native-e5-atomic-rmw|mw-hw-575|#584` 在 HEAD 上均
  无命中——dududu 无任何 #575 数据或备份（receipts/05）。
- 本轮保留审计目录中的 `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md` 无
  `575`/`584`/`native-e5`/`mw-hw`/`NO_CHANGE_JUSTIFIED` 命中——现有保存
  计划未覆盖本分支。
- 自洽性验证：`manifest.json`（分支自带 sha256 清单）与分支 tip 内容比对
  **checked=113 ok=113 mismatch=0 missing=0，无清单外文件**（receipts/08）
  ——证据树可直接整体归档，形态与 #610 的 EVIDENCE_MANIFEST.json 模式相同。

### 为什么 NO_CHANGE_JUSTIFIED 的负面研究仍须归档

VERDICT.md：三轴均为 `NO_CHANGE_JUSTIFIED`、`PRODUCT_CHANGE_AUTHORIZED=NO`、
`MERGED=NO`。但该记录的价值不在"改了什么"，而在：

1. **负面结果的因果证据**（R8）：TP/UR 两臂 ±0.7–3.8 % 的摆动被
   plain-increment control arms 证明为 binary 代码形状伪影、净效应 ≈0——
   没有这份归档，未来同样的"删 counter"想法只能重跑整个 campaign。
2. **方法学交付物**（R5、R9、R4-MUTATION-RECEIPTS）：单字符 tmpfs 路径名
   偏差（identical binaries 上 ~1 %）、n=15 精确 CI vs Monte-Carlo 陷阱、
   UR 正计数 oracle 缺口及其关闭测试。#580（uring-router-580）在 master 上
   明确声明继承这些纪律（见上文出处引用），但被继承的原始证据只在本分支。
3. **不可再生性**：81 个 raw 文件来自特定物理主机（E5 Xeon E5-2666 v3,
   Fedora 44, GCC 16.2.1, liburing 2.13）与当时的代码基线（v0.0.3
   `3df10dc4`），无法在当前 master 上复现重跑。
4. **R1 语义结论**（两个计数器为 observation-only derived summaries、唯一
   消费者是测试 seam）是未来任何 macro-guard/删除提案的直接输入。

## 6. Active dependency（必查项 6）

- OPEN PR head？否（#584 CLOSED；列表中 OPEN 的 #594 headRef 为
  `research/593-key-path`、#8(dududu) 为 `r1-request-geometry`，均非本分支；
  无 PR 以本分支为 base）。
- `git grep "575-native-e5-atomic-rmw"` 在 origin/master 全树无命中；
  `.github` 无命中（无 CI 引用）。
- 本地工作区 grep：仅 `docs/research/retention-wsl2-audit/` 的审计文档自身
  命中（审计记录，非运行时依赖）。
- OPEN PR #594 分支树内 grep `575-native-e5-atomic-rmw|mw-hw-575` 无命中
  ——#594 不依赖本分支（receipts/06）。

## 7. Worktree 与 Working tree（必查项 7）

- `git worktree list`：16 个 worktree（主区 master + /tmp/sluice-perf-*、
  /tmp/sluice-val-* 等全部 detached），**无一检出本分支**；本地不存在
  `research/575*` 分支。working-tree dirty 检查不适用（N/A：分支没有被任何
  worktree 检出）（receipts/06）。

## 8. Recovery（必查项 8）

完整 HEAD SHA：`e9baafb9d117cbaf387dde9f4433ebbacb4a73b3`。删除分支 ref 后的
恢复通道（receipts/07）：

1. 远程分支 ref 现仍存在：`git ls-remote origin refs/heads/research/575-native-e5-atomic-rmw`
   → 同一 SHA（删除前的状态备份）。
2. **GitHub PR head ref**：`refs/pull/584/head` 实测解析到同一 SHA
   （CLOSED 未合并 PR 的 head ref 由 GitHub 保留）：
   `git fetch origin pull/584/head`。
3. 本地 clone 当前持有全部对象（`cat-file -t` 对 HEAD commit 与 oracle blob
   均成功）；按完整 SHA fetch 需服务器 `uploadpack.allowAnySHA1InWant`
   （未验证服务器配置，故列第 1/2 条为主通道）。
4. PR #584（https://github.com/jnhu76/Sluice/pull/584）与 ask 提供的 PR 列表
   均记录 headSha，可溯源。

## 决策

按 ask 的 DELETE_CANDIDATE 六条件逐项判定：

| 条件 | 结果 |
|---|---|
| 非 OPEN PR head | ✅ #584 CLOSED（未合并） |
| 无独有未归档数据 | ❌ **114 文件证据树仅存在于本分支**（master/#610/dududu/保存计划均不覆盖） |
| 无未推送产品变更 | ✅ oracle 测试与 xmake 注册已经 #585 落地（代码逐字一致） |
| 相关原始数据已有可靠备份 | ❌ 无任何默认分支/外部备份；分支 ref 与 refs/pull/584/head 是唯一载体 |
| 无运行中 Agent/Worktree 依赖 | ✅ 无 worktree 检出、无本地分支、无 CI/脚本/PR 引用 |
| 完整 HEAD SHA 与恢复步骤已记录 | ✅ 本报告 + receipts/01、07 |

非 OPEN PR head、无产品变更滞留、无活跃依赖、恢复通道明确，但**独有研究
数据未归档**——按决策规则判 **ARCHIVE_FIRST**，不满足 DELETE_CANDIDATE。

建议的归档动作（本轮不执行）：将 `docs/research/mw-hw-575-native-e5/`（114
文件，含自校验 manifest.json，receipts/08 验证通过）按 #610 的 sha256-manifest
归档模式合并进 master（例如 `research/native-e5-575/` 或既有归档布局），
归档 PR 说明 #584 CLOSED 未合并、verdict NO_CHANGE_JUSTIFIED、oracle 已由
#585 单独产品化；归档合入后本分支满足全部六条件，可重新审计为
DELETE_CANDIDATE。
