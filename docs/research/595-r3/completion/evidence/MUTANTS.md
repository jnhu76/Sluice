# #595 R3 续跑 — 变异体杀伤与同源码强制重建（MUTANTS）

| 字段 | 值 |
|---|---|
| 角色 | R3 完成轮裁决与交付员（本 session 实际复跑） |
| 变异宏 | `SLUICE_READY_BATCH_MUTANT_INCLUDES_PENDING_TRANSPORT` / `..._RESIDUE_UNADVERTISED` / `..._REDISPATCHES_CONSUMED`（xmake 目标 `uring_ready_batch_det_mut_*`，与基目标同源码、仅注入变异宏） |
| 载体 | `tests/uring_ready_batch_deterministic_test.cpp`（26 场景确定性 oracle） |
| 锁 | 全部命令持 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（会话见 §1；记录 `raw/mutants/session.log`） |
| 结论 | **BASE GREEN → 3×MUTANT RED → RESTORED GREEN**（同最终源码在干净 builddir 强制重建，二进制 sha256 与冻结产物逐位相同） |

## 1. 本 session 实际执行的序列（2026-11-05T12:43:02Z–12:43:56Z，UTC）

### 1.1 预状态（持锁窗口内记录）

```text
HEAD = 0a22a79195f1b913145fcd443496e7f70b08cdcf
git status --porcelain = 15 行（10 M + 5 ??）
git diff HEAD | sha256 = c1a31a30a9f02db99fac2ab2d0a8af028245a6fff0220314b8119c26e30d3003
uring_ready_batch_deterministic_test        c54a9222ed7121935b264417e6b7a65c2df6c8a2f943e98f71c01a602d59e700
uring_ready_batch_det_mut_includes_not_ready 2dc7b1233d4f602201f0510a4c8f64322409b55cfe4e301432fec781d057bef1
uring_ready_batch_det_mut_redispatch_consumed 4d5f4390890fa7a01ac85b1e52b3fa23ba51c67a799157e2e77c8113e29c00ad
uring_ready_batch_det_mut_residue_dropped    a90b4916071ba745a8b16ca9795853fbb7b2b5e851ef3d2666809640186d7117
```

### 1.2 BASE GREEN（冻结二进制）

```text
$ ./build/linux/x86_64/release/uring_ready_batch_deterministic_test
uring_ready_batch_deterministic: all deterministic ready-batch oracles passed   # rc=0
```

（`raw/mutants/base-green.out`；BUILD_COMPLETION §8.3 曾连跑 2 次全绿。）

### 1.3 MUTANT RED（三个变异体全部被杀）

| 变异体 | rc | 失败摘要 |
|---|---|---|
| `..._MUTANT_INCLUDES_PENDING_TRANSPORT` | **1** | `uring_ready_batch_deterministic: 17 failure(s)`（如 "the rejected batch attempts no flush / adds no transport submit / leaves the dispatch queue empty"） |
| `..._MUTANT_RESIDUE_UNADVERTISED` | **1** | `uring_ready_batch_deterministic: 9 failure(s)`（"the held batch advertises its transport residue" 等 residue 广告断言） |
| `..._MUTANT_REDISPATCHES_CONSUMED` | **134** | `sluice::async::UringAsyncBackend: router exhaustion (invariant violation)` + `terminate called without an active exception`（已消费成员被重发导致路由耗尽 fail-fast） |

原始输出：`raw/mutants/mut-{includes-not-ready,residue-dropped,redispatch-consumed}.out`；
与 BUILD_COMPLETION §8.6（rc=1/17、rc=1/9、rc=134）一致。

### 1.4 RESTORED GREEN —— 同一最终源码强制重建

```text
$ xmake f -m release --liburing=y --builddir=/tmp/r3c-mutant-rebuild -y   # 全新空 builddir
$ xmake build -y uring_ready_batch_deterministic_test                     # build ok（强制从源码全量编译）
$ sha256sum /tmp/r3c-mutant-rebuild/linux/x86_64/release/uring_ready_batch_deterministic_test
c54a9222ed7121935b264417e6b7a65c2df6c8a2f943e98f71c01a602d59e700           # 与冻结产物逐位相同
$ /tmp/r3c-mutant-rebuild/linux/x86_64/release/uring_ready_batch_deterministic_test
uring_ready_batch_deterministic: all deterministic ready-batch oracles passed   # rc=0
```

干净 builddir 强制重建得到**逐位相同**的二进制（sha256 `c54a9222…`）且全绿——证明
(a) 冻结产物正是当前 worktree 最终源码构建的（PROV 绑定），(b) 变异体 RED 是源码注入
所致而非环境/陈旧产物，(c) 恢复未变异源码后 oracle 立即回到 GREEN。

### 1.5 后状态（不变式）

```text
HEAD/status/diff-sha 不变（15 行 / c1a31a30…）
冻结 build/ 五个关键二进制 sha256 全部不变：
  uring_ready_batch_deterministic_test c54a9222… / bench_w03_external_loop 0ebfb7f0… /
  uring_ready_batch_research_test 1372b346… / uring_ready_batch_fault_test 18395c0a… /
  bench_w02_pipeline 4e719b1e…
worktree xmake 配置已复原为 `xmake f -m release --liburing=y`
```

## 2. 有效性边界

- 变异体只证明 26 场景确定性 oracle 能区分三类已命名缺陷（组门包含未就绪传输、residue
  不广告、已消费成员重发）；它不是穷尽变异测试，不证明语义完备。
- 第一次 mutant 会话因 `set -e` 在预期 rc=1 处中断（mut1 输出已捕获）；同锁窗口内立即
  redo 全部三个变异（`raw/mutants/session.log` NOTE；偏差 D-15）。
- 变异体运行是正确性车道，不进入任何计时统计。
- `semanticsOk=true`（BUILD_COMPLETION §1）是实现者自检；本 session 的复跑只复核
  BASE/MUTANT/RESTORED 三态与重建绑定，不构成 §14.3 的独立 verifier 语义回执
  （后者还要求 26 场景列表、A0/suffix 回执、OPEN-1.4/1.14 处置等，`A0_AND_SUFFIX_RECEIPT.md`
  与 completion 树内 `correctness/` 仍缺失）。
