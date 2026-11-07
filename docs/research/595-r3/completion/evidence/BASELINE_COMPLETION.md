# #595 R3 续跑 — E5 平台与输入基线冻结（BASELINE_COMPLETION）

| 字段 | 值 |
|---|---|
| 角色 | R3 续跑基线核验员（只读核验 + 新建续跑证据目录） |
| 生成时间（UTC） | 2026-11-05T08:38:18Z（`date -u +%Y%m%dT%H%M%SZ` → `20261105T083818Z`） |
| completionRoot | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z` |
| baseSha | `0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| R2 锚点 | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| **frozen** | **true** |
| 未执行 | 未跑 benchmark 计时；未构建；未删除/重建/rebase R3 worktree；未提交；未改主树/远端/Issue/PR/R1/R2 原始证据 |

原始输出目录：`<completionRoot>/raw/baseline/`。

---

## 1. 平台（原生 E5 判定）

| 项 | 实测值 |
|---|---|
| `lscpu` Model name | `Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz` |
| `/proc/cpuinfo` model name（去重） | `model name	: Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz` |
| 拓扑 | x86_64；20 logical（10 core × 2 threads/socket，1 socket）；NUMA node0 = 0-19 |
| 原生 Linux | `/proc/cpuinfo` 无 `hypervisor` flag；`systemd-detect-virt` → `none`；DMI `sys_vendor=JUXIESHI`、`product_name` 为空 |
| `uname -a` | `Linux E5 7.2.11-200.fc44.x86_64 #1 SMP PREEMPT_DYNAMIC Thu Oct 15 15:58:58 UTC 2026 x86_64 GNU/Linux` |
| g++ | `g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)` |
| xmake | `xmake v3.1.2+HEAD.5d21e9d` |
| python3 | `Python 3.14.8` |
| liburing | `pkg-config --modversion liburing` → `2.13`；`/usr/include/liburing.h`、`/usr/lib64/liburing.so.2.13` |
| cpu2 governor | `schedutil` |
| cpu2 `scaling_cur_freq` | `1200000`（1.2 GHz） |
| cpu2 `thread_siblings_list` | `2,12`（SMT 兄弟存在） |
| `/proc/sys/kernel/perf_event_paranoid` | `2` |
| `/proc/sys/kernel/unprivileged_bpf_disabled` | `2` |
| `df -h /tmp` | `tmpfs 32G 783M 31G 3% /tmp` |
| 挂载摘要 | `/tmp` = tmpfs（`rw,nosuid,nodev,size=32838152k`）；`/home` = `/dev/sda3[/home]` btrfs（`rw,relatime,compress=zstd:1,ssd,discard=async,space_cache=v2`） |

判定：**原生 Intel Xeon E5-2666 v3 Linux，E5 条件成立**；未出现 `BLOCKED_E5`。
（注：cpu2 当前为 `schedutil`，`scaling_cur_freq=1200000`，不是 performance governor/固定最高频；这是本机既有状态，与 `PROTOCOL.md` §2.1 记录一致，本轮未改任何 governor/turbo/SMT 设置。）

原始文件：`raw/baseline/platform.txt`。

---

## 2. 主工作树

命令与输出：

```text
$ git -C /home/jnhu/Source/Sluice rev-parse HEAD
0a22a79195f1b913145fcd443496e7f70b08cdcf
$ git -C /home/jnhu/Source/Sluice status --porcelain
（0 行）
$ git -C /home/jnhu/Source/Sluice branch --show-current
research/595-causal-r2
```

结论：HEAD **等于期望基线**；主树在续跑开始与冻结复核时均 **clean**（两次 `status --porcelain` 均为 0 行）。原始文件：`raw/baseline/main-tree.txt`、`raw/baseline/freeze-check.txt`。

---

## 3. R3 隔离 worktree

```text
$ git -C /home/jnhu/sluice-595-e5/r3/worktree rev-parse HEAD
0a22a79195f1b913145fcd443496e7f70b08cdcf
$ git -C /home/jnhu/sluice-595-e5/r3/worktree branch --show-current
research-595-r3
$ git -C /home/jnhu/sluice-595-e5/r3/worktree status --porcelain
 M bench/bench_common.hpp
 M bench/bench_w03_external_loop.cpp
 M include/sluice/async/detail/test_access.hpp
 M include/sluice/async/detail/uring_backend.hpp
 M src/async/uring_backend.cpp
 M xmake/tests.lua
?? bench/r3_ready_batch.hpp
?? tests/uring_ready_batch_deterministic_test.cpp
?? tests/uring_ready_batch_fault_test.cpp
?? tests/uring_ready_batch_research_test.cpp
$ git -C ... diff --stat 0a22a79195f1b913145fcd443496e7f70b08cdcf
 bench/bench_common.hpp                        |  40 ++++
 bench/bench_w03_external_loop.cpp             | 161 ++++++++++++---
 include/sluice/async/detail/test_access.hpp   |  11 ++
 include/sluice/async/detail/uring_backend.hpp |  40 ++++
 src/async/uring_backend.cpp                   | 271 +++++++++++++++++++++++---
 xmake/tests.lua                               |  42 ++++
 6 files changed, 514 insertions(+), 51 deletions(-)
```

- `worktreeHead`：`0a22a79195f1b913145fcd443496e7f70b08cdcf`（**等于实际基线**）。
- 脏文件：6 个已跟踪修改（见上）；未跟踪 4 个：`bench/r3_ready_batch.hpp`、`tests/uring_ready_batch_deterministic_test.cpp`、`tests/uring_ready_batch_fault_test.cpp`、`tests/uring_ready_batch_research_test.cpp`。
- `git worktree list --porcelain` 中路径属于 `/r3/` 的 worktree 只有 `/home/jnhu/sluice-595-e5/r3/worktree` **一个**；其余为 main、`archive-wt`、`causal-r2/worktree`、`causal-r2/worktree-timing`、R2 `worktree`。
- 本轮**未删除、未重建、未 rebase、未提交**该 worktree；源文件 mtime 均为 2026-11-05 12:59–14:25（+0800），未被本次核验改动。
- 内部 detail 头 `include/sluice/async/detail/{test_access,uring_backend}.hpp` 有改动；公开 async 头 `include/sluice/async/*.hpp` 的 diff 为**空**（复核见 `raw/baseline/r3-worktree.txt`）。v2-r1 文档 diff 亦为空。

原始文件：`raw/baseline/r3-worktree.txt`、`raw/baseline/freeze-check.txt`。

---

## 4. 前轮证据读取与清单校验

已读取（路径均在 `/home/jnhu/sluice-595-e5/r3/evidence/`）：

- `MANIFEST.sha256.hash`：`8cae1ec7096989d9a22b80071ab30aee0c432d8311b893ea7d16106f99d4de16  .../evidence/MANIFEST.sha256`
- `BUILD.md`、`PROTOCOL.md`、`VERDICT.md`、`census.json`、`READY_EXPOSURE.md`
- `correctness/CORRECTNESS.md`、`correctness/MANIFEST.sha256`

校验（实际执行）：

```text
$ sha256sum -c /home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256.hash
.../evidence/MANIFEST.sha256: OK                        # exit 0
$ sha256sum -c /home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256
246 OK / 0 FAILED                                        # exit 0
$ cd /home/jnhu/sluice-595-e5/r3 && sha256sum -c evidence/correctness/MANIFEST.sha256
33 OK / 0 FAILED                                         # exit 0
```

前轮 patch：

```text
10ce088a4b3219ba57ee0e63fa8f46d506ac763abc73eea073f833ecb8ce1622  evidence/r3-final.patch
a9eeeac56acf34a3f9857993058b319df5b2e01b4f01643aead98b776e4cf63f  evidence/r3.patch
```

结论：前轮主清单（246 项，覆盖 evidence/raw/harness/logs/lock/worktree 源与二进制）、清单 sidecar 与正确性清单（33 项）**全部可校验**。前轮结论可复用（见 `CONTEXT_COMPLETION.md`）。

原始文件：`raw/baseline/prior-evidence-check.txt`。

---

## 5. 产品源码相对 R2 锚点

```text
$ git -C /home/jnhu/Source/Sluice diff --stat 271b81bfc9d64eecc0266fe7066b9040525dd710..0a22a79195f1b913145fcd443496e7f70b08cdcf
 2577 files changed, 65557 insertions(+)        # 无删除
$ git -C ... diff --name-status 271b81bf...
 A  <2577 entries，全部在 docs/research/595-causal-r2/ 下>
$ git -C ... diff --name-status ... | 非 docs/research/595-causal-r2/ 路径
 (none)
$ git -C ... diff --stat ... -- include src apps bench tests xmake xmake.lua
 (空)
$ git -C ... log --oneline 271b81bf..0a22a791
0a22a791 research(595): archive causal-r2 adaptive cross-layer attribution evidence (#595)
```

结论：R2 锚点到基线之间只有 **1 个文档/证据提交**，新增 2577 个文件全部位于 `docs/research/595-causal-r2/`；**产品相关源码（include/src/apps/bench/tests/xmake）与 R2 锚点完全相同**。`sameProductCodeAsR2 = true`。

原始文件：`raw/baseline/product-diff.txt`（含完整 2577 行 name-status）。

---

## 6. 当前 worktree 二进制

构建目录：`/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/`

| 二进制 | sha256 | 状态 |
|---|---|---|
| `bench_w02_pipeline` | `00a5ca6257e5592013ea47a464abb149003ede3f6f0452a34c7323eea450983f` | 存在 |
| `bench_w03_external_loop` | `af6e8afd22b4f5b2e797f5b5b1501b8efa02da5e4f70d2a3a702f524aacdda21` | 存在 |
| `uring_ready_batch_research_test` | `e16d1379fee2093cc06ff99de7e01cee4ca8fac98f1ed5bfc2284e0c9f762a54` | 存在 |
| `uring_ready_batch_fault_test` | `88c7418867d49ecbb54a1190d2dd89ac2e155ce85c31bbea485a139413a3b55c` | 存在 |
| `uring_ready_batch_deterministic_test` | `0ecb27e2ca8b7a6cb0b2b1d76ca2035c7c5ea9f574d3e238660e2bbd60a0104e` | 存在 |

与 `evidence/MANIFEST.sha256` 当前值一致（该清单 246/246 OK），即前轮最终态未被改动。

原始文件：`raw/baseline/binaries.txt`。

---

## 7. 共用测量锁

路径：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`

| 项 | 结果 |
|---|---|
| 文件存在 | 是（0 字节，inode 6941075，mtime 2026-11-05 11:49:21 +0800） |
| bench/perf/bpftrace 计时进程 | **无**（`ps -eo pid,comm,args` 过滤 `bench_w0|uring_ready_batch|bpftrace|/perf ` → `NO_BENCH_PERF_BPFTRACE_PROC`） |
| 其它 flock 持有者 | **无**（`/proc/locks` 中无 `:6941075` 条目；`lslocks` 无匹配） |
| 非阻塞探测 | `flock -n <lock> -c 'echo ACQUIRED_AND_RELEASED'` → `ACQUIRED_AND_RELEASED`，exit 0 |

结论：锁存在但**当前无人持有**，可立即用于续跑测量。锁存在本身不计为持锁。

原始文件：`raw/baseline/lock.txt`。

---

## 8. R1/R2 只读核验与远端

| 项 | 实测 |
|---|---|
| `/home/jnhu/sluice-595-e5/evidence` 文件数 | **1854** |
| 其中 mtime 晚于 `2026-11-05T00:00:00Z` | **0** |
| `/home/jnhu/sluice-595-e5/worktree` 文件数 | **5451**（排除 `.git` 后 5450） |
| 其中 mtime 晚于 `2026-11-05T11:00:00Z` | **0**（排除 `.git` 后仍 0） |
| R2 worktree HEAD | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| R2 worktree 脏状态 | 6 行（5 M + 1 ??；为前轮 R2 原型既有状态，mtime 无晚于 11:00Z 者） |
| `gh issue view 595 --json number,state,updatedAt` | `{"number":595,"state":"OPEN","updatedAt":"2026-11-04T16:59:52Z"}` |
| `gh issue view 604 --json number,state,body` | `number=604, state=OPEN`；body 中未发现 `READY_FOR_595` handoff |
| `gh pr view 625 --json number,state` | `{"number":625,"state":"OPEN"}` |

只读执行；未评论、未改状态、未 push。

原始文件：`raw/baseline/r1-r2-readonly.txt`、`raw/baseline/gh-readonly.txt`。

---

## 9. 冻结判定

| 条件 | 结果 |
|---|---|
| 原生 E5-2666 v3 Linux | 通过 |
| 主树 clean | 通过（0 行） |
| worktree HEAD = 实际基线 | 通过（两者均为 `0a22a791…`） |
| 前轮主清单可校验 | 通过（246/246；sidecar OK） |
| 前轮正确性清单可校验 | 通过（33/33） |
| 共用锁可用 | 通过（无持锁者，非阻塞获取成功） |

**frozen = true**。

已知限制/未决项（不影响本轮冻结）：

1. 前轮 R3 **没有任何计时数据**（A/A、A/B、负控、保护门、资源门全部 `NOT_RUN`）；`PREREGISTERED_R3.md` 与 measurement 目录不存在（前轮 P-2/P-3）。
2. cpu2 为 `schedutil`，`scaling_cur_freq=1200000`；不是 performance/固定最高频。
3. R2 隔离 worktree 处于前轮遗留脏状态（5 M + 1 ??），本轮未触碰；其 mtime 无晚于 2026-11-05T11:00Z 的文件。
4. 本轮未构建、未运行任何 benchmark 或正确性测试；二进制哈希仅来自既有构建产物。
