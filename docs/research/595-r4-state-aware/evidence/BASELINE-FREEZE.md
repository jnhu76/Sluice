# #595 R4 — Frozen Baseline（BASELINE-FREEZE）

冻结目标：重建 #627 冻结实现（0a22a791 + r3-completion.patch + delta_w02 + delta_w03），
证明可构建、可执行、正确性 smoke 通过。全部步骤在本机实际执行，UTC 时间戳为准。

## 0. 结论

| 字段 | 值 |
|---|---|
| ok | true（三补丁双源 sha256 核验、终态三重 hash 验收门、持锁构建、correctness、W02/W03 smoke 全部通过） |
| worktree | `/home/jnhu/sluice-595-e5/r4/worktree`（干净树，porcelain=0） |
| branch | `research-595-r4` |
| sourceSha | `d9d140eea180f29f629992aa293109acdeff1e03`（冻结提交，父=baseCommit） |
| baseCommit | `0a22a79195f1b913145fcd443496e7f70b08cdcf`（=271b81bf 直接子提交） |
| buildResult | build_rc=0（release/liburing，13 目标）；4 correctness rc=0；W02 d1 / W03 d4 smoke rc=0 |
| 主仓 / R3 证据 | master（271b81bf，porcelain=0）与 r3 worktree（0a22a791，diff sha 73fa116f…）均未改动 |

## 1. 环境摘要

- host=`E5`，kernel=`7.2.11-200.fc44.x86_64`，CPU=`Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz`，20 CPU（10 core × 2 thread）。
- 工具链：`xmake v3.1.2+HEAD.5d21e9d`；`g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)`。
- liburing：xmake config 探测 `ok (/usr/include/liburing.h + …/lib64/liburing.so)`。
- 测量锁：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（inode 6941075，dev 0:53，size=0）。

## 2. 补丁核验（第 1 步，双源逐位比对）

`sha256sum <disk>` 与 `git show <blob> | sha256sum` 均实际运行，与核验过的完整 sha256 逐一相等：

| 补丁 | 磁盘位置 | git blob | sha256（期望=实测） |
|---|---|---|---|
| r3-completion.patch | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/r3-completion.patch` | `772a4329:docs/research/595-r3/completion/evidence/r3-completion.patch` | `2596f204035f9695db47184b20ad8ece7b2168c54d8589b2060782aa7959bbc0` |
| delta_w02.patch | `/home/jnhu/sluice-595-e5/r3-flip-20261105T1428Z/evidence/delta_w02.patch` | `0ac2b582:docs/research/595-r3/continuation/evidence/delta_w02.patch` | `4aa7b306fc8469ef0b597af3def761f340221dcb86503a135bf6caa4d4dec88e` |
| delta_w03.patch | `/home/jnhu/sluice-595-e5/r3-flip-20261105T1428Z/evidence/delta_w03.patch` | `0ac2b582:docs/research/595-r3/continuation/evidence/delta_w03.patch` | `57f21e022e7b9d0df97ea74398f37717902c1fe97586efe0849c2d19b5d42736` |

锁前置探测（2026-11-05T19:53:05Z）：`stat` size=0 inode=6941075；`flock -n … -c 'echo probe-acquired'` → 立即成功（rc=0），无持有者。

## 3. 重建命令与输出（日志：`r4/logs/1*.log`、`2*.log`）

目录：`mkdir -p r4/{build,raw,analysis,evidence/smoke,logs,figures}`；分支名 `research-595-r4` 此前不存在。

```bash
git -C /home/jnhu/Source/Sluice worktree add /home/jnhu/sluice-595-e5/r4/worktree \
  -b research-595-r4 0a22a79195f1b913145fcd443496e7f70b08cdcf
# HEAD is now at 0a22a791 …；porcelain=0（logs/10-worktree-add.log）
```

补丁 A（每个先 `git apply --check` 再应用；`logs/20-apply-r3-completion.log`）：

```bash
git -C <WT> apply --check <PA>   # OK
git -C <WT> apply --stat  <PA>   # 15 files changed, 3999 insertions(+), 64 deletions(-)
git -C <WT> apply        <PA>    # OK
# 中间检查点：porcelain=15 行（10 M + 5 ??）
# git diff HEAD | sha256sum = c1a31a30a9f02db99fac2ab2d0a8af028245a6fff0220314b8119c26e30d3003
#   → 与 FLIP_RECEIPT_14_3.md 登记的步骤-2 中间态前缀 c1a31a30 一致
```

补丁 B（`logs/21-apply-delta-w02.log`）：

```bash
git -C <WT> apply --check <PB>   # OK；stat: bench/bench_w02_pipeline.cpp | 287 ±（+277/−10）
git -C <WT> apply        <PB>    # OK
git -C <WT> hash-object bench/bench_w02_pipeline.cpp  # 95cc4b79f158a63b213b376163d5c11d7215401b
```

补丁 C（GNU patch -p3；先正向 dry-run 再应用；`logs/22-apply-delta-w03.log`）：

```bash
patch -f --dry-run -p3 -d <WT> < <PC>   # checking file bench/bench_w03_external_loop.cpp → OK
patch -f         -p3 -d <WT> < <PC>     # patching file bench/bench_w03_external_loop.cpp → OK
git -C <WT> hash-object bench/bench_w03_external_loop.cpp  # 43623787b3489be79168a2d9637c514d2026aff6
```

终态三重 hash 验收门（`logs/23-final-gate.log`，2026-11-05T19:55:43Z，全部通过）：

```bash
git -C <WT> status --porcelain | wc -l        # 16（11 M + 5 ??，逐行记录于日志）
git -C <WT> diff HEAD | sha256sum             # 73fa116ff668ea58143ab6666d24684f0eea7f39fcb9e6c03572ae00a7df5560
git -C <WT> hash-object bench/bench_w02_pipeline.cpp   # 95cc4b79f158a63b213b376163d5c11d7215401b
git -C <WT> hash-object bench/bench_w03_external_loop.cpp  # 43623787b3489be79168a2d9637c514d2026aff6
```

即重建树与 #627 冻结登记（FLIP_RECEIPT_14_3 §1.2、r3 worktree 实时脏状态、0ac2b582 归档）逐位一致。
无 `.orig`/`.rej` 产生（`bench/research/key-path-593/patches/…rejected-hunk.rej` 为基线提交 0a22a791
自带的 #594 历史归档文件，`git ls-files --error-unmatch` 确认 tracked，与本轮补丁无关）。

## 4. 冻结提交（`logs/24-frozen-commit.log`）

`git add -A && git commit`，消息注明 R4 frozen baseline、三补丁来源（磁盘路径 + git blob）与全部 sha256、
验收门四项实测值、research-only 不合入。提交后 porcelain=0（干净 SHA，非脏树）。

- sourceSha：`d9d140eea180f29f629992aa293109acdeff1e03`（`research-595-r4`）
- 对基线 diffstat：16 文件，`+4286 / −76`（10 M 基线文件 + delta 触碰的 bench_w02/bench_w03 + 5 新文件）。

## 5. 持锁构建与 smoke（`r4/logs/run_build_smoke.sh`、`r4/logs/30-build-smoke-lock-session.log`）

构建目录 `r4/build`（隔离 builddir，未污染 worktree）。命令（在 worktree 内、持锁执行）：

```bash
xmake f -y -m release --liburing=y --builddir=/home/jnhu/sluice-595-e5/r4/build   # config_rc=0
xmake build -y bench_w03_external_loop bench_w02_pipeline \
  uring_ready_batch_research_test uring_ready_batch_fault_test \
  uring_ready_batch_deterministic_test uring_submit_boundary_test \
  uring_progress_race_test uring_flush_count_oracle_test uring_external_loop_test \
  uring_public_consumer_probe uring_request_core_integration_test shutdown_lifecycle_test
# build ok, spent 11.314s（xmake cache 辅助），build_rc=0
```

correctness（聚合运行，输出存 `r4/evidence/smoke/*.out`）：

| 目标 | rc | 末行 |
|---|---|---|
| uring_ready_batch_deterministic_test | 0 | `all deterministic ready-batch oracles passed` |
| uring_ready_batch_research_test | 0 | `all research oracles passed` |
| uring_ready_batch_fault_test | 0 | `all research fault oracles passed` |
| shutdown_lifecycle_test | 0 | `PASS [withheld_completion_fails_by_deadline_and_late_completion_passes]` |

W02/W03 最小 case（`SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_SKIP_DISK=1`，
`SLUICE_BENCH_TMP_DIR=/tmp/sluice-r4-smoke`；W02 取矩阵最小 depth=1，W03 取默认矩阵最小 uring depth=4）：

- `=W02-uring-4k-d1` → rc=0，`[W02-uring-4k-d1 rep=0] ops=20000 …`，`=== W-02 complete, failing repetitions=0 ===`
- `=W03-uring-4k-d4` → rc=0，`[W03-uring-4k-d4 rep=0] ops=20000 …`，`=== W-03 complete, failing repetitions=0 ===`

关键产物 sha256（`r4/build/linux/x86_64/release/`）：

| 产物 | sha256 |
|---|---|
| bench_w03_external_loop | `dccde70c51cd05e971d2c902c7a599b8998c545294064383f40f9d3ca6e00c7b` |
| bench_w02_pipeline | `ded2ddefd669dfb85d421eed9d02a9589ebc3caa3c1c64612b641dbf52a3f0c5` |
| uring_ready_batch_deterministic_test | `c54a9222ed7121935b264417e6b7a65c2df6c8a2f943e98f71c01a602d59e700` |
| uring_ready_batch_research_test | `b7ddaab4f5586587b80580de13bf1b14a298a6677775eab2f895a3d9a80ed6b6` |
| uring_ready_batch_fault_test | `18395c0a570862147436ff11624eec7276ed1851546df8ebb4ad6590a4fe3bba` |
| libsluice_async.a | `831112f0b93933b014aa2a0bb1abab805193a63cfd5ecb9330629718040dd715` |

诚实观察（非验收门）：deterministic/fault 两个测试二进制与 r3-completion
BUILD_COMPLETION.md §8.1 登记值逐位一致；`uring_ready_batch_research_test`（R4=b7ddaab4…）与 §8.1 登记值
（1372b346…）不一致——两个 delta 均不触碰该源文件，差异来源本轮未归因（§8.1 数字可能登记于完成轮内
较早重建）；两个 bench 二进制与 §8.1 不同是预期行为（delta_w02/w03 恰好只改这两个 bench 源）。
`libsluice_async.a` 与 §8.1 不同（静态库装配非本轮验收门）。配方验收门为源码级 hash（§3），已全部通过。

## 6. LOCK_RECEIPT — MEASUREMENT.lock 使用记录（格式照 r3-completion 轮）

- 锁文件：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（inode 6941075，dev 0:53）。
- 会话获取：2026-11-05T19:58:19Z（`flock -w 1800` fd 9，等待 0s）；释放：2026-11-05T19:58:36Z。
- 获取时 `fuser -v` 输出：`jnhu 8790 F…. run_build_smoke`（持锁进程，`r4/logs/lock-fuser.txt`）。
- 排他性检查：第二个 open file description 的 `flock -n` → `EXCLUSIVITY_CHECK: blocked as expected`
  （`r4/logs/lock-exclusivity.txt`）。
- 获取与释放之间执行：xmake config + 13 目标构建 + 4 correctness + W02/W03 smoke，全部串行独占，
  无并行构建/bench/大 IO。
- 释放后复核（2026-11-05T19:59:17Z）：`flock -n` 探测立即成功（probe-acquired-after-release, rc=0），
  锁文件 size=0 inode 不变 → 当前无持有者。
- #604 无冲突：其历史从未 handoff READY_FOR_595，无并发计时（任务书互斥协议段）。

## 7. 时间线（UTC）

| 时间 | 事件 |
|---|---|
| 2026-11-05T19:53:05Z | 补丁双源 sha256 核验 + 锁空置探测 |
| 2026-11-05T19:54Z | r4 目录、worktree、补丁 A/B/C、终态验收门 |
| 2026-11-05T19:55:43Z | 终态三重 hash 验收门通过（logs/23） |
| 2026-11-05T19:56Z | 冻结提交 d9d140ee |
| 2026-11-05T19:58:19Z–19:58:36Z | 持锁构建 + correctness + smoke 会话 |
| 2026-11-05T19:59:17Z | 锁释放复核（可立即再获取） |

## 8. 边界遵守

- master 主工作树（271b81bf，porcelain=0）零改动；r3/ 证据 worktree（0a22a791，diff sha 73fa116f…）零改动
  （冻结后实测复核）；未推送远程、未写 GitHub；未动 R1/R2/R3 历史证据。
- 新建物仅限 `r4/` 目录树、分支 `research-595-r4`、git worktree 元数据与冻结提交 d9d140ee。
