# CAMPAIGN-LEDGER — #621 Progress/Wait 路径战役台账

| 字段 | 值 |
|---|---|
| 战役 | #621 Minimum Necessary Machine Work — Progress/Wait 路径（应用路径权威 #604） |
| 台账日期 | 2026-11-18 |
| `CAMPAIGN_BASE_SHA` | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| 战役目录 | `docs/evidence/621-progress-wait-path-20261118/`（本目录为 2026-11-18 新建） |
| 台账性质 | 后续多轮实验者的工作文档。它不是合同、不是测量证据、不是合并授权，也不替代 `docs/explicit-io-contract.md` 或 `docs/roadmap/v2-conformance.md` |
| 环境资格 | **PASS**（频率门禁通过；PMU/eBPF/介质可用；风险见 §7） |
| 本轮实验数量 | **0**。本轮只合并两份 P0 审计并做只读核验；未运行构建、测试、基准、TLC、perf 或 eBPF 采集 |

> **阅读顺序**：先读 §1（基线）、§9（纪律）、§10（矩阵），再按 §4/§5/§6 选择候选与归属 issue。任何候选开工前必须重新核验 §1 的 BASE 与 §7 的环境；任何数字进入产品结论前必须按 §9 从原始数据独立复算。

---

## 0. 来源、核验与未运行清单

### 0.1 两份 P0 审计

| 来源 | 内容 | 本台账中的标注 |
|---|---|---|
| 仓库审计（2026-11-18，base `5d94f233`） | HEAD/worktree 状态、W1–W6 源码路径、`#634`/`#635` 已合并事实、`#637` 25 条候选处置、`#595`/`#604`/`#617` 归属、基准/oracle 入口、完整构建测试矩阵 | “仓库审计” |
| 环境审计（2026-11-18） | 频率门禁、PMU/eBPF 能力、测试介质、噪声源、`ENVIRONMENT_BLOCKED` 风险 | “环境审计” |

两份审计的原始结论直接进入本台账；本台账不重新裁决 `#637` 的候选终态，也不把审计中的“未运行”改写成“通过”。

### 0.2 本轮记录员实际执行的只读核验

以下命令在本轮实际执行，输出用于校正/确认台账中的基线、路径、归档完整性与环境事实。未列出的检查一律视为“未运行”。

| 核验 | 实际命令（摘录） | 结果 |
|---|---|---|
| BASE 与工作树 | `git rev-parse HEAD`；`git rev-parse origin/master`；`git status --porcelain`；`git worktree list --porcelain`；`git stash list` | HEAD = origin/master = `5d94f233…`；仅两个未跟踪目录；仅主 worktree；stash 为空 |
| 已跟踪差异 | `git diff --stat`；`git diff --cached --stat` | 均无输出（无已修改/已暂存跟踪文件） |
| 合并归属 | `git cat-file -p 3f22d2d0`；`git cat-file -p 5d94f233`；`git merge-base --is-ancestor <sha> HEAD` | `#634` merge `3f22d2d0`（父 `f2c42535`）与 `#635` merge `5d94f233`（父 `3f22d2d0`）均为 HEAD 祖先；`98825793` 亦为祖先；`0e50612c` **不是**祖先 |
| 未跟踪清单 | `git status --porcelain=v1 --untracked-files=all` | 17 项 = `621-round2-R1/REPORT.md` + `e5-fullpath-machine-work/` 16 个文件 |
| 源码行号抽查 | `grep -n` / `sed -n` / `Read`（见 §2、§5 各路径） | 关键函数/区域的行号范围与文件内容一致；个别审计行号边界差 1–2 行，已在台账中按实际源码校正（例：`progress_source.hpp:104-232`、`request_core.cpp:202`、`threadpool_backend.cpp:550-611`、`uring_transport.cpp:222`） |
| `#637` 本地文档与归档一致 | 对 `candidates.md`、`final-report.md`、`experiment-protocol.md` 分别执行 `sha256sum` 与 `git show e2f41acb:<path> \| sha256sum` | 三份均 **MATCH**；协议哈希 `cb62400b74eafe0f762534df11caa6b35caae26e479eb9cc8c7f8ac11d3192d8` |
| `#637` 清单路径完整性 | `git show e2f41acb:EVIDENCE-MANIFEST.sha256 \| wc -l`，并逐项 `git cat-file -e e2f41acb:<path>` | 清单 **2127** 条，路径缺失 **0**。**未**执行 2127 项字节级 `sha256sum -c` |
| `#637` 判词数字复算 | 从 `git show e2f41acb:results/e5m28-s1-20261118/stats/main-cell-per-pair.csv` 读取，按 `order` 分组取 `d_instr_u_op(B-A)` 均值 | AB 12 对 **−1.944350**、BA 12 对 **−2.246725**、全体 24 对 **−2.0955375**，与 `archive-entry.md` 预期一致 |
| 环境只读复核 | `uname -r`；`cat /proc/sys/kernel/perf_event_paranoid`；`cat .../scaling_governor`；`cat .../scaling_driver`；`ls -l /sys/kernel/btf/vmlinux`；`stat -f -c '%T' /tmp`；`findmnt -no FSTYPE,SOURCE,OPTIONS /`；`cat .../intel_pstate/no_turbo`；`lsblk`；`cat /sys/block/*/queue/scheduler`；`free -g` | 与审计一致：`7.3.3-200.fc44.x86_64`、paranoid=2、cpu0 `schedutil` + `intel_cpufreq`、BTF 7,023,892 B、`/tmp` tmpfs、根 btrfs `compress=zstd:1`、`no_turbo=0`、sda/sdb bfq、nvme none、内存 62 GiB |
| 历史目录 | `ls -ld /home/jnhu/sluice-621-r2 /home/jnhu/sluice-e5-elim /home/jnhu/sluice-e5-elim-base` | `sluice-621-r2` 存在但已非 Git worktree；两个 `e5-elim` 目录不存在 |
| 构建配置漂移 | `cat .xmake/linux/x86_64/xmake.conf`；`ls -l build-bench-ring/.../bench_w02_pipeline` | `mode=release, liburing=true, builddir=build-bench-ring`；W-02/W-03 旧产物存在（2026-11-18 01:23） |

### 0.3 本轮明确未运行

- 任何 `xmake f` / `xmake build` / `xmake test`。
- 任何 `bench/build_profiles.sh`、`bench/run_matrix.sh`、`bench/check_run.py`、W-02/W-03 单格运行、`bench/mechanisms/run_mechanisms.sh`。
- 任何 `sluice-copy` A/A 或 A/B、perf、strace、bpftrace、perf probe、TLC。
- 环境审计中的 turbostat/APERF-MPERF/PMU/eBPF 实测由环境审计执行，本轮**没有复跑**；本轮只重读了 §0.2 列出的 sysfs/procfs/块设备事实。
- 当前 master 的测试总数 `N` 未确认（没有运行 `xmake test`）。

---

## 1. 战役基线

### 1.1 Git 基线

| 项 | 值 |
|---|---|
| `CAMPAIGN_BASE_SHA` | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| HEAD | 同 BASE；提交标题 `Merge pull request #635 from jnhu76/product/621-progress-drain-single-read` |
| `origin/master` | 同 BASE |
| 分支 | `master`，跟踪 `origin/master` |
| worktree | 仅 `/home/jnhu/Source/Sluice`（主树，master @ `5d94f233`）；`git worktree list --porcelain` 无其他条目 |
| stash | 空 |
| 已跟踪工作树状态 | 干净：无 modified/staged 文件 |
| 未跟踪内容 | 仅以下两个目录（共 17 个文件），**全程不得触碰** |

本轮核验时的原始输出：

```text
$ git status --porcelain
?? docs/evidence/621-round2-R1/
?? docs/evidence/e5-fullpath-machine-work/

$ git worktree list --porcelain
worktree /home/jnhu/Source/Sluice
HEAD 5d94f233967c3aef993eb19ba9e3252c532b0951
branch refs/heads/master

$ git stash list
(空)
```

### 1.2 未提交内容清单（标注「全程不得触碰」）

| 路径 | 文件数/大小 | 内容与状态 | 处置 |
|---|---|---|---|
| `docs/evidence/621-round2-R1/` | 1 文件 / 136K | `REPORT.md`，933 行；`#635` 的 R1 工作稿，`BASE_SHA = HEAD_SHA = 3f22d2d0`，`MECHANISM_VERDICT=INCONCLUSIVE`；已被归档的 R2 r6 最终报告（`0e50612c`，1054 行）取代 | **全程不得触碰**；不得引用为最终结论，不得改写、移动、删除 |
| `docs/evidence/e5-fullpath-machine-work/` | 16 文件 / 492K | `#637` 战役文档本地未跟踪副本：`candidates.md`、`final-report.md`、`experiment-protocol.md`、`dag*.md`、`env-census.md`、`history-digest.md`、`measurement-audit.md`、`p0c-cache-study.md`、`p1a-cost-map.md`、`pocketjs-method-notes.md`、`repo-reality.md`、`dossier-review.md`、`experiment-audit.md`。其中三份已抽样比对与 `e2f41acb` 归档逐字相同 | **全程不得触碰**；不得提交、改写、覆盖或删除 |

> **本台账自身**：`docs/evidence/621-progress-wait-path-20261118/CAMPAIGN-LEDGER.md` 是本轮新建文件。写入后 `git status` 会额外显示该目录；它不属于 BASE 的未提交清单，也不属于“不得触碰”的两个既有目录。

### 1.3 与基线相关的文档漂移

| 事实 | 记录 |
|---|---|
| v2 root header 的 `Implementation baseline` 仍写 `b870a7c0`（`docs/explicit-io-contract.md:10`） | 不包含 master `5d94f233` 的 `#634`/`#635`；当前行为以源码 + ledger dated 621 节为准，不能把 root header 当现状 |
| v2 ledger 的 `Assessed source commit` 仍为 `c001bb1e`（`docs/roadmap/v2-conformance.md` header） | `#634`/`#635` 只以 dated 621 节进入 ledger，不改变 root baseline |
| `.xmake` 当前配置 | `mode=release, liburing=true, builddir=build-bench-ring`（非默认 `build/`）；`build/` 与 `build-bench-*` 均有旧产物 |
| 构建复用风险 | 正式实验必须显式 `-o` 新目录并 clean build；不得跨 `--liburing` 复用 build dir（#520 混链风险；`bench/build_profiles.sh:4-13`） |
| `#635` 证据不在 master | `docs/evidence/621-progress-drain-single-read-20261112/` 在 BASE 不存在；只在归档提交 `0e50612c`（PR #636 closed/unmerged）。master 的 CHANGELOG/ledger 只给 SHA 链接 |
| `#637` 证据不在 master | 原始证据在 evidence 分支 `e2f41acb`（父 `a09df602`）；`#637` 正文头部仍写 `a09df602`，最新 closeout 评论改为 `e2f41acb`，**以 `e2f41acb` 为准** |

---

## 2. Backend 状态、线程与锁所有权

### 2.1 总览

| Backend / 路径 | 执行形态与线程所有权 | 锁所有权 | 当前状态 |
|---|---|---|---|
| **UringAsyncBackend**（`src/async/uring_backend.cpp`、`uring_transport.cpp`、`uring_completion.cpp`） | 无自有 worker 线程；由调用线程/宿主线程驱动（W-02 由 bench 线程驱动，W-03 由 app-owned epoll 外部循环驱动）。提交、reap、publication、owed sweep 都在驱动线程上执行 | `dispatch_mtx_`（`include/sluice/async/detail/uring_backend.hpp:320`）保护 `dispatch_`、`publication_pending_`、`router`、`TransportLedger`、`delivery_`、`owed_count_`、`pending_control_count_`；`RequestCore::mutex_` 为独立单一 mutex；`ProgressSource::mtx_` 为通知域 | 生产路径；PROG-01/PROG-02 已 VERIFIED（ledger 行 + `#564` H2b）；W1 等待路径仍是未证实候选 |
| **ThreadPoolBackend**（`src/async/threadpool_backend.cpp`） | 自有 `std::vector<std::thread> workers_`（`threadpool_backend.hpp:256`），`worker_loop()` 消费 `dispatch_`；调用线程做 accept/publish/progress pass；`active_workers_` 记录执行中 worker | `work_mtx_`（`threadpool_backend.hpp:247`）保护 `dispatch_`、`publication_pending_`、`delivery_`、`owed_count_`、`active_workers_`、`stopping_`；`RequestCore::mutex_` 独立；`ProgressSource::mtx_` 独立 | 生产路径；`#634` owed-count gate 已合并；权威扫描仍保留在 settlement 路径 |
| **Direct / blocking**（`src/blocking_file.cpp`、`include/sluice/blocking/file.hpp`） | 纯阻塞调用；无 AsyncIoContext、无 worker、无 request table、无 progress source（`bench/bench_w01_direct.cpp:1-2` 的 TU 约束）。线程即调用线程 | 无 backend/progress 锁；文件资源生命周期由 `File`/`FileResource` 的调用约定管理。`src/blocking_file.cpp` 无 mutex/thread 使用 | W-01 支持面；本战役的对照/负控制路径，不参与 async backend 候选 |
| **RequestCore**（共享） | 无自有线程；由 backend/调用线程调用 | 单一 `mutex_`（`include/sluice/async/detail/request_core.hpp:295`）；`Slot` 表、`outstanding_`、publication/reclaim 状态都在此 mutex 下；每个公开方法各自取锁，未合并跨方法临界区 | `#634`/`#635` 的共同语义底座；W3 候选集中在这里 |
| **ProgressSource**（共享） | 无自有线程；owner/driver 调用 `wait_if_unchanged`；fd 由宿主借用 | 单一 `mtx_` 域：control 检查、epoch 重验、`notification_pending_`、drain、wake 写入 | `#635` 单读 drain 已合并；`poll(-1)` 等待链路是 W1 |

### 2.2 UringAsyncBackend 现状（源码坐标）

| 机制 | 坐标 | 现状 |
|---|---|---|
| `poll_progress` | `src/async/uring_backend.cpp:304-328` | entry snapshot + bounded pass；`immediate_work_remains` 用 `publication_pending_` 与 `owed_visible_locked_()` |
| `poll` | `src/async/uring_backend.cpp:616-689` | dispatch 循环 620-634、reap 636、publication pass_bound 638-657、owed sweep 658-686 |
| `publish_one` | `src/async/uring_backend.cpp:578-608` | `begin_publication` 583-585、`complete_publication` 598、deliver 610-614 |
| zero-op inline | `src/async/uring_backend.cpp:553-576` | `owed_count_++` 在 `dispatch_mtx_` 域 |
| terminal finalize | `src/async/uring_completion.cpp:23-97` | `offer_terminal` 70、`release_execution` 89、`publication_pending_->push_back` 92 |
| CQE 处理 | `src/async/uring_completion.cpp:99-198` | 控制 CQE 101-144，exact `live_control_sqes_` 校验 110-116、`release_control` 120；普通 CQE 148-197 |
| reap | `src/async/uring_completion.cpp:200-278` | BATCH=32、`io_uring_peek_batch_cqe`、逐 CQE `io_uring_cqe_seen`、overflow flush |
| transport dispatch | `src/async/uring_transport.cpp:43-102`、`130-209` | get_sqe、claim、cookie、router、ledger append |
| submit | `src/async/uring_transport.cpp:218-235` | `submit_flushes_` 222、`io_uring_submit` 228/231 |
| router lookup | `src/async/uring_transport.cpp:540-557` | `find_live_router_index_` / `find_live_router_cookie_`；M-53 的逆向 O(R) 扫描，调用点 `uring_completion.cpp:105,148` |
| RouterEntry / DeliveryRecord / counters | `include/sluice/async/detail/uring_backend.hpp:178-205`、`304-305`、`322` | `owed_count_`、`pending_control_count_`、`publication_pending_` |

### 2.3 ThreadPoolBackend 现状（源码坐标）

| 机制 | 坐标 | 现状 |
|---|---|---|
| zero-op owed 标记 | `src/async/threadpool_backend.cpp:344-353` | `record.event_owed = true`、`++owed_count_`（mutant 可去掉 increment） |
| progress pass | `src/async/threadpool_backend.cpp:550-612` | `publication_pending_` 快照 559-568；`owed_count_` 门控 sweep 569-583；deliver + `release_control` 585-598；`immediate_work_remains` 604-609；`accepted_work_remains` 610 |
| 权威 owed 扫描 | `src/async/threadpool_backend.cpp:847-854` | `any_event_owed_locked_()` 逐记录扫描；`internal_work_retired()` 856-860 同锁联合快照 |
| 资源退休 | `src/async/threadpool_backend.cpp:875-899` | join 后用权威扫描 + occupancy 判 quiescence |
| `owed_visible_locked_` | `include/sluice/async/detail/threadpool_backend.hpp:203-212` | `owed_count_ != 0`；派生摘要，非权威 |
| `owed_count_` 注释/成员 | `include/sluice/async/detail/threadpool_backend.hpp:239-244` | 注释明确“derived summary... gates”；成员 `std::size_t owed_count_ = 0` |
| parity oracle | `tests/threadpool_owed_counter_parity_test.cpp:375-407` | 8 用例；`xmake/tests.lua:518-537` 注册 parity + 4 mutants |

### 2.4 Direct / 阻塞面

- `bench/bench_w01_direct.cpp:1-2` 明确 W-01 的 TU 不得出现 `AsyncIoContext`、worker、request table 或 progress source。
- `src/blocking_file.cpp` 只 include `sluice/blocking/file.hpp`，无 `mutex`/`thread` 使用。
- W-01 是本战役的“ direct 对照”：它不承载 Request/Backend 协调成本，不能把 W-01 的 wall/CPU 变化直接解释为 async 路径收益。

### 2.5 锁所有权结论（供后续候选使用）

1. **Uring**：单驱动线程 + `dispatch_mtx_`；`RequestCore::mutex_` 与 `ProgressSource::mtx_` 是两个独立的、不可合并的语义域。
2. **ThreadPool**：N worker + `work_mtx_`；`RequestCore::mutex_` 独立；`ProgressSource::mtx_` 独立。`delivery_` 与 `owed_count_` 只在 `work_mtx_` 下读写。
3. **Direct**：调用线程 + 无 async 锁。
4. **RequestCore**：单一 mutex；任何“融合锁”候选都必须证明步间语义（特别是 publication_inflight 与 slot reuse）不被合并临界区改变。
5. **ProgressSource**：`mtx_` 是通知与 suppression 的唯一域；eventfd 只是借用句柄，**不是第二 drain authority**（PROG-03）。

---

## 3. 已合并工作与它们在 BASE 中的事实

| 工作 | 合并/归档 SHA | 在 BASE 中？ | 生产改动 | 证据与关键数字 | 备注 |
|---|---|---|---|---|---|
| **#543** io_uring owed-delivery report fix | 基线 `b870a7c0` 的组成部分（PR #543） | 是（作为更早基线） | `uring_backend.cpp` owed-delivery residue 报告 | ledger PROG-01 uring owed-delivery residue 行：3/3 PASS | 本台账不重述其实现细节；只作为 uring `owed_count_` 先例 |
| **#564 / H2b** owed-count productization | E5 pinned `c8970702`/`eb465c4f`；ledger 行记录的 2026-10-21/22 证据 | 是（作为更早基线） | uring `owed_count_` + empty-sweep skip + submit-reset fail-fast | formal E5 A/B PASS：870 行、0 reasons、11 configs median ≤0.994、headline 0.495；5 mutants killed | `#634` 的 ThreadPool 机制镜像此生产 uring 机制 |
| **#609 / PR #618** ELIDE 系列 | merge `98825793`（`feat(async): remove write-only slot state and redundant settle polling`） | 是（已核验为 HEAD 祖先） | ELIDE-010/001/002/003/004/016 | 终态 `E5_MECHANISM_VERIFIED / END_TO_END_WIN_NOT_DEMONSTRATED`；四个 native W02 格在噪声内 | 历史负控制仍是重开抑制先例（见 §6） |
| **#634** ThreadPool `owed_count_` gate | merge `3f22d2d0`；base `f2c42535`；PR #634 | 是（merge 为 `#635` 的父） | `include/sluice/async/detail/threadpool_backend.hpp`（+20）、`src/async/threadpool_backend.cpp`（43 行变动）、`src/async/threadpool_test_seams.hpp`（+30）；新增 `tests/threadpool_owed_counter_parity_test.cpp`（403 行，8 用例）+ 4 mutants（`xmake/tests.lua:527-537`） | `sluice-copy` 256 MiB、4 KiB/d4/1 worker、btrfs warm、24 对：CPU/task −5.58% [−6.45,−4.71]，wall −6.53% [−7.55,−5.53]，user instr −22.1%；负控制 64 KiB/d1 +0.10% [−1.56,+1.77]；`PARTIAL_CPU_ACCOUNTING` | 证据目录 `docs/evidence/621-threadpool-owed-count-20261112/` **在 BASE 中且已跟踪**（本轮核验） |
| **#635** ProgressSource 单读 drain | merge `5d94f233`；base `3f22d2d0`；PR #635 | 是（HEAD 本身） | 仅 `include/sluice/async/detail/progress_source.hpp`（+47/−3）；新增 `tests/progress_source_drain_semantic_oracle_test.cpp`（539 行，9 用例）与 `tests/progress_source_drain_mechanism_test.cpp`（129 行，不注册为测试）；mutant `progress_source_mut_drain_success_keeps_pending`（`xmake/tests.lua:569-570`） | 36 对：instructions −5.0180% [−5.7603,−4.2987]，cycles −5.1111%，task-clock −5.4653%，rusage CPU −5.3199%；A/A +0.7713% 宽度 2.4090 | 证据 `docs/evidence/621-progress-drain-single-read-20261112/` **不在 BASE**，只在未合并归档 `0e50612c`（PR #636 closed/unmerged）；master 产品代码与测试已合并 |

### 3.1 `#634` 的事实边界

- `owed_count_` 是 `delivery_` 中 `event_owed` 记录的派生摘要，**不是** admission/settlement/publication 的权威。
- `internal_work_retired()` 与 `retire_execution_resources()` 仍使用 `any_event_owed_locked_()` 权威扫描；`owed_count_` 只门控 per-pass collection sweep 与热路径 `immediate_work_remains`。
- parity oracle 与 4 个 mutant 是回归证据；任何后续修改 `owed_count_` 的候选必须保留它们，并保留权威扫描。
- **负控制口径**：PR #634 body 写 64 KiB 为 “non-inferior, as pre-registered”，但 `docs/evidence/621-threadpool-owed-count-20261112/REPORT.md §4` 与 #621 评论 3 明确说本轮只预注册了该格与方向性预期、**未预注册非劣效界值**；CI 上界 +1.77% 不能排除小幅 CPU 回归。**不得**作形式化非劣效声明。

### 3.2 `#635` 的事实边界

- 成功 non-semaphore eventfd 单次 read 后，在同一 `mtx_` 临界区清 `notification_pending_` 并返回；保证 EAGAIN 的第二次 read 被删除；`EINTR` 重试、`EAGAIN` 分支、`PhysicalProbe` 全部保留。
- `DrainObservation` 与 EINTR 注入是 `SLUICE_ASYNC_INTERNAL_TESTING` 测试面；release 预处理与 `nm` 无 seam 符号（ledger 记录）。
- io_uring 内核写者在成功 read 后重新抬升 counter 的窗口**没有确定性注入**；`PhysicalProbe` 是安全网。TP 证据不外推到 io_uring 或其他 backend。
- 归档报告是测量证据，不是产品语义权威；`0e50612c` 不是 master 祖先，干净 clone 无法直接读取原始回执。

---

## 4. `#637` 全部历史候选处置表

**规则（先于表格）**

1. 终态按 `#637` 最终归档（`e2f41acb`）原样记录；本台账**不**重新裁决。
2. `NO_GO` / `NO_GO_ABORT`：无满足其翻案条件的新证据不得复活。**M-28 标 `NO_GO_ABORT`，不默认重做**。
3. `SEMANTIC_MISMATCH`：属于 owner 裁决面，不是性能实验；在 owner 裁决前不开工。
4. `GO`：已排队但**未启动**；是否启动属 owner。开工仍需预注册、A/A、单因素 A/B 与机制门。
5. `INCONCLUSIVE`：未准入；必须满足表中重开条件才可进入下一轮。
6. 归属 issue 为 `#621`，除表中明确标注 `#595`、`#604`、`#617` 的行。
7. `#13` 与 `#16` 在归档中都记作 `M-61`，但指向不同候选；**不要合并**这两个编号。

**Issue 状态（2026-11-18 核验）**：`#621` OPEN、`#637` CLOSED、`#595` OPEN、`#604` OPEN、`#617` OPEN。`#637` 的关闭不改变其候选终态；`#621` 仍停在 Owner Gate 之前。

| # | 候选（`#637` 归档 ID） | 终态 | 翻案 / 重开条件 | 归属 issue |
|---|---|---|---|---|
| 1 | **M-28** `syscall_count_` 生产侧宏门控剔除 | **NO_GO_ABORT**（预注册中止通道判词 1b + 1c；性能端点 `NOT_POWERED`） | 新证据把机制约束为可分离的“单指令差”（消除/隔离 TU 重编译布局变化、成员移除的对象布局变化、RMW 移除对 `work_mtx_` 竞争时序的影响）并重过全协议；新 session、新冻结；动态回执须自带接受带与隔离格判据 | #621（应用路径权威 #604） |
| 2 | M-80 `write_all_at` → `prevalidated_pwrite_step` | INCONCLUSIVE | 直呼组合写形态测得 measured hotspot + “compose 仅由已校验入口进入”不变量证明 → 单因素 A/B | #621（矩阵未逐条指派；直通写组合路径） |
| 3 | M-16 uring 后端层 state-op 校验收敛 | INCONCLUSIVE | owner 裁决 seam 冲突 + 隔离 backend 层 state-op 半边的单因素 A/B | #621（矩阵未逐条指派） |
| 4 | M-37 `remove_exact(front)` → `pop_front` | INCONCLUSIVE | 深队列派发形态实测 `remove_exact` 的 memmove 份额（deferred-submit 家族）或证明生产全形态队列深 ≤1 → 塌缩为 NO_GO | **#595**（批量概念 B） |
| 5 | M-53 cookie O(R) 逆向扫描 → `user_data` 路由索引编码 | INCONCLUSIVE | 完成三项迁移义务证明（TransportLedger 物理位置单调、控制 CQE exact-submitted、cancel 定向）+ d64 深度格 A/A 分辨力达标（半宽 ≤ 效应/2）→ 下一轮 GO 资格 | #621（矩阵未逐条指派） |
| 6 | M-63 uring owed sweep 逐槽锁 → 单锁快照 | INCONCLUSIVE | 零长 op 负载或 idle 形态协议激活 owed 循环 + 交付顺序不变性确认 → 单因素 A/B | #621（矩阵未逐条指派；owed 机制本体 KEEP） |
| 7 | M-09 `Slot::borrow` 只写不读 | SEMANTIC_MISMATCH | owner 裁决：修 root 摘除 borrow facts（→ 删除 PR）或出现真实消费者（→ KEEP 为义务存储） | #621（owner 裁决面；root 义务） |
| 8 | M-30 臂（`zero_effect_cancel` 生产零生产者） | SEMANTIC_MISMATCH | owner 裁决：认定 seam 义务（KEEP）或修 root 去掉冗余第二强制点并迁移 seam 测试 | #621（owner 裁决面；CANCEL-01） |
| 9 | M-57 `acquire_execution` 生产零调用 | SEMANTIC_MISMATCH | owner 裁决 seam 必要性；若判可删，走代码面收缩 PR（无性能门） | #621（owner 裁决面；BACKEND-01） |
| 10 | M-12 `close_admission` 双实现二选一 | SEMANTIC_MISMATCH | owner 指定存活实现 + 测试 oracle 迁移方案 | #621（owner 裁决面） |
| 11 | M-42 `submit_flushes_` 只写不读原子计数 | **GO**（排队，未启动；#1 回执已落地 `NO_GO_ABORT`，是否启动属 owner） | 独立单因素 A/B + 机制门七问；协议必须自带 §4-2 型动态回执带 + 隔离格判据（不能只做静态三列）；出现生产读者 → NO_GO/重新物化 | #621（#595 P4 引用 `submit_flushes_` 作为 submit-attempt 计数） |
| 12 | M-40 `live_cookies_`/`live_control_sqes_` 原子冗余 | INCONCLUSIVE（r2 由 NO_GO 改判） | 行级 measured 归因 + 生产读者角色评估（quiescence 派生摘要的机制门作答）+ `dispatch_mtx_` 覆盖重证 + 缝读者加锁设计 → 单因素 A/B | #621（矩阵未逐条指派） |
| 13 | M-61 `PublicationPayload` 快照仅测试可达 | NO_GO | 生产 payload 消费者出现（→ 重新物化为正当机制）或 owner 批准快照下沉测试专用通道（卫生路线，非性能门） | #621（矩阵未逐条指派） |
| 14 | M-01 scope O(S) 扫描，H3 开放 | INCONCLUSIVE | 高重复 `-scope64` 隔离协议剔除驱动污染后给出机制可归因份额 → 单因素 A/B；free-list 是否重开属 owner（S2） | #621（矩阵未逐条指派） |
| 15 | M-10 零长发布三锁融合 | INCONCLUSIVE | 零长占比测量 + 步间语义映射 + 融合单因素 A/B | #621（矩阵未逐条指派） |
| 16 | M-61 `begin`/`complete` 两锁融合 | INCONCLUSIVE | 新 measured profile 推翻 r6 R3 拒绝（0.45% < 1% MME REJECTED，含 `publication_inflight` 反例与确定性测试注入点）+ 保步间语义的融合单因素 A/B（族 A/A 先行） | #621（S3 已闭合：R3 = 本候选） |
| 17 | M-62 无观察者跳过 claim/retire | INCONCLUSIVE | 新 measured profile 行级分离 + 协议重证 + 单因素 A/B；对位 mutex family ~9–11%（r6 F.6） | #621（矩阵未逐条指派） |
| 18 | M-65 owed `release_control` 第三锁融合 | INCONCLUSIVE | 零长/owed 形态协议（与 #6 同激活前提）+ 第三锁与 retire 融合语义证明 → 单因素 A/B | #621（矩阵未逐条指派） |
| 19 | M-67 sticky 双标志原子化/融合查询 | INCONCLUSIVE | 行级 profile 分离 + A/A 分辨力 ≥1% MME 达标 + ProgressSource 单读者协议重证 → 单因素 A/B | #621（矩阵未逐条指派） |
| 20 | M-33 TP cancel ring 摘除惰性失效 | INCONCLUSIVE | cancel 形态 measured hotspot + 失效机制设计证明（两语义保持）→ 单因素 A/B；新增机制须过机制正当性门 | #621（矩阵未逐条指派） |
| 21 | M-74 `ready()` 快路径合并 | INCONCLUSIVE | ready 概率分层测量给出方向 + 合并收益隔离证据 → 单因素 A/B | #621（矩阵未逐条指派） |
| 22 | M-80 hygiene `file_semantics` 模型词表位置 | SEMANTIC_MISMATCH | owner 裁决词表移入测试面（保持语义 oracle 共享）或出现生产消费者 | #621（owner 裁决面） |
| 非候选 1 | H1 提交批形态（per-dispatch flush） | NO_GO（本轮） | #595 RQ1 second-held-entry guard + payload gate + D1/D2/D3、64 KiB、W-03、cancel falsifier 家族全过 + 整机重测（R1–R5 原始数据未归档） | **#595** |
| 非候选 2 | io-wq punt FS 条件性 | NO_GO | 无翻案路径（非 Sluice 代码）；换设备 FS 的产品准入属既列重测情形 | **#604 / #617**（FS 条件性；报告称非 Sluice 代码） |
| 非候选 3 | tp-w8 `work_mtx_` 竞争（worker 超配） | NO_GO | 出现 worker 数被外部固定的 root 工作负载 + 最小机制提案过机制正当性门 → 单独预注册；唯一减法交集 M-28 已 `NO_GO_ABORT` | **#604**（配置/工作负载面） |

### 4.1 M-28 特别标注

**M-28 = `NO_GO_ABORT`，不默认重做。** 其性能端点是 `NOT_POWERED`，且预注册中止通道判词 1b/1c 已落地。任何重启必须同时满足：

1. 新证据把机制约束为可分离的“单指令差”；
2. 消除/隔离 TU 重编译布局变化、成员移除的对象布局变化、RMW 移除对 `work_mtx_` 竞争时序的影响；
3. 新 session、新冻结；动态回执自带接受带与隔离格判据。

仅“再测一次”或“换一个 workload”不构成翻案。

### 4.2 已否决候选的不复活规则

- `NO_GO`（M-61 payload、H1 本轮、io-wq punt、tp-w8）与 `NO_GO_ABORT`（M-28）在无新证据时不得作为“顺手重试”对象。
- `SEMANTIC_MISMATCH` 的候选先走 owner 裁决；owner 未裁决前，性能实验没有意义。
- `GO` 的 M-42 也只是“排队”，不是“已准入性能门”；启动需 owner 确认。

---

## 5. 尚未证实的候选与 W1–W6

### 5.1 W1–W6 源码路径与历史证据状态

| 工作包 | 源码路径与关键坐标 | 历史证据状态 | 当前优先级 | 实验费用（规划级） | 准入门禁 |
|---|---|---|---|---|---|
| **W1** ProgressSource poll 等待路径 | `include/sluice/async/detail/progress_source.hpp:104-232` `wait_if_unchanged`；deadline/毫秒向上取整 111-147；`mtx_` 内 control 检查、epoch 重验、deadline 返回与 stale readiness drain 153-166；`PhysicalProbe` 175-181；探针后二次 epoch 重验 183-192；`::poll(pfds,1,timeout_ms)` 198-203；EINTR 206-214；POLLNVAL terminate 216-222；POLLIN 223-228。上下文驱动：`src/async/async_io_context.cpp:573-705` `wait_one`（token 609、pass 611、control 627、probe 637-639、10ms transport retry 22/646-652、wait 654-664、deadline 终 pass 674-690） | PROG-02 drain 单读已 VERIFIED（`#635`）；**等待路径本体未被验证为候选**。`docs/evidence/621-round2-R1/REPORT.md:148,176`（R1 工作稿，已被 R2 r6 取代）记录 profile rank 1：`poll(-1)` 可由同一 eventfd 的阻塞 read 取代，约 5.0–5.5% cycles 可删，但结构化 candidates 未给出落选理由；`#637 §J/r6 F.6` 也指向该路径 | **P1（先 profile 准入，不直接开 A/B）** | **M/L**：真实消费者 profile + 有界等待/非阻塞 drain/dup fd 设计 + A/A 36 对 + A/B ≥36 对；若改等待语义需模型/协议重证 | #621 最新评论要求先对现行 `poll(-1)` 等待链路重新做 hotspot/语义准入；必须有界等待、drain 非阻塞语义与 dup fd 方案的独立设计；不得把 R1 工作稿的 5.0–5.5% 当已证实收益 |
| **W2** 通知/drain/rearm | 同 `progress_source.hpp`：`signal` 252-264、`interrupt` 234-250、`wake_notification_nolock_` 466-489（`notification_pending_` 抑制物理写；EAGAIN 视为已 pending）、`drain_notification_nolock_` 414-460（成功单读清 pending 439；EINTR 444-446；EAGAIN 447-455）、`acknowledge_notification` 274-277、`observe_pending_control` 42-50、`acknowledge_control` 54-58、`control_pending_nolock_` 380-386、`retire_notification` 284-290；`BackendProgressPort` 524-554（fd 仅注册借用，不转交 drain 权） | `#635` 已合并；PROG-02/03 语义 oracle 与 mutant 已落地；io_uring 内核写者窗口无确定性注入，`PhysicalProbe` 保留 | **NOT_APPLICABLE（本战役无新增 W2 候选）** | N/A；任何后续改动沿用既有 9 用例语义 oracle + mutant + 模型 | 任何新改动必须保持“单读清 pending”、EINTR/EAGAIN 分支、fd 借用非第二 drain authority；语义 FAIL 即 NO_GO |
| **W3** RequestCore | `src/async/detail/request_core.cpp`：`reserve` 165-182、`accept` 184-210（`outstanding_++` 202、zero-op terminal 203-207）、`offer_terminal` 346-365、`claim_execution` 367-378、`release_execution` 393-409、`begin_publication` 438-460、`complete_publication` 462-484（published/`outstanding_--` 471-473、observer queued 474-481）、`reclaimable_` 580-590、`try_reclaim_`/`release_slot_` 592-620、`occupancy` 66-91、`outstanding_summary` 93-96、`collect_outstanding` 98-108；`include/sluice/async/detail/request_core.hpp:257-277` Slot 字段、292 `outstanding_`、295 单一 `mutex_` | 每个公开方法各自取同一 mutex，未合并跨方法临界区；mutex family 在 r6 F.6 约占 ~9–11%，但这是**族级线索**，不是已证实的单候选。M-09/M-12/M-57 等语义项仍是 SEMANTIC_MISMATCH；publication/reclaim 语义由 PROG-01/PROG-04 约束 | **P1（行级 profile 分离）/ P2（单个融合）** | **M**：row-level profile 分离 + 2–4 个候选共享一次 A/A + A/B 72–144 对；融合候选需步间语义映射 | 必须先给出 measured 行级归因；任何跨方法临界区合并必须证明 publication_inflight/slot reuse/observer 注册不被改变；机制门七问 + 单因素 A/B |
| **W4** UringAsyncBackend | `src/async/uring_backend.cpp` `poll_progress` 304-328、`poll` 616-689、`publish_one` 578-608、zero-op inline 553-576；`src/async/uring_completion.cpp` finalize 23-97、`handle_one_cqe` 99-198、`reap_cqes` 200-278；`src/async/uring_transport.cpp` dispatch 43-102/130-209、submit 218-235、account 237-279、reconcile 298-335、cookie 526-538、router 540-557、retire 559-582；`src/async/uring_internal.hpp:45-57` control tag、146-240 TransportLedger；`include/sluice/async/detail/uring_backend.hpp:178-205` RouterEntry/DeliveryRecord、304-305 counters、322 publication_pending_ | PROG-01/PROG-02 与 H2b 已 VERIFIED；`owed_count_` 派生子计数在产。M-53 仍 INCONCLUSIVE，三项迁移义务未证；M-63/M-65 依赖 owed 协议激活；M-40/M-42 是计数/测试面候选 | **P2** | **M/L**：M-53 需 d64 深队列 A/A 分辨力 + 单因素 A/B；owed 家族需零长/owed 协议 + 2–4 个融合臂；M-42 为 S | d64 A/A 半宽 ≤ 效应/2；TransportLedger 物理位置单调、控制 CQE exact-submitted、cancel 定向三项证明；不得改 per-Request terminal/publication；机制门七问 |
| **W5** ThreadPool owed-count | `src/async/threadpool_backend.cpp:344-353`、`550-612`、`847-860`、`875-899`；`include/sluice/async/detail/threadpool_backend.hpp:203-212`、`239-244`；`tests/threadpool_owed_counter_parity_test.cpp:375-407`；`xmake/tests.lua:518-537` | **已合并（`#634`），BASE 中**；证据目录已跟踪；数字见 §3 | **NOT_APPLICABLE（无新增 W5 候选）** | 后续任何改动至少重跑 parity 8 用例 + 4 mutants + 完整矩阵 | 保留权威 `any_event_owed_locked_()` 扫描；counter 只能作派生摘要；语义 FAIL 即 NO_GO |
| **W6** test-only 计数器 | M-42 `submit_flushes_` 无条件 `fetch_add` 在 `src/async/uring_transport.cpp:222`（ledger 空早退 219 之后），成员 `include/sluice/async/detail/uring_backend.hpp:324`，唯一读者 `src/async/uring_test_seams.hpp:75-76`（该文件第 4 行整体受 `SLUICE_HAS_LIBURING && SLUICE_ASYNC_INTERNAL_TESTING` 保护）；测试引用 `tests/uring_flush_count_oracle_test.cpp:119-173`、`tests/uring_request_core_integration_test.cpp:406,755-768,904`。M-28 `syscall_count_` 仍在生产：`src/async/threadpool_backend.cpp:434` fetch_add、`include/sluice/async/detail/threadpool_backend.hpp:258` 成员无门控、`hpp:72` 访问器在测试宏内、`src/async/threadpool_test_seams.hpp:108-110`。其他测试面计数：`live_cookies_` hpp:325、`live_control_sqes_` hpp:326、`owed_count_for_test`/`owed_scan_count_for_test` | M-42：**GO（排队，未启动）**；M-28：**NO_GO_ABORT**；M-40：**INCONCLUSIVE（r2 由 NO_GO 改判）**。release 预处理与 `nm` 需证明 seam 符号不出现（`#635` 已用同一纪律） | **P2（M-42）/ NO_REDO（M-28）/ P2（M-40）** | **S**（M-42：单因素 A/B + 机制门 + 动态回执带 + 隔离格）；M-28 不重做；M-40 需行级归因，M | M-42 必须证明无生产读者、过机制门七问、动态回执带 + 隔离格判据、mutant kill、release 无 seam 符号；M-28 只在满足 §4.1 三项时重开；M-40 见 §5.2 |

### 5.2 尚未证实的候选优先级（不含已合并/已否决）

> 优先级是**战役规划标签**，不是 root/ledger 状态：P1 = 已有 measured hotspot 线索且可先做语义/环境准入；P2 = 需先 profile 分离、协议激活或 runner 扩展；P3 = owner 裁决/卫生路线。费用级定义：S ≤ 1 个单因素臂组（A/A 36 对 + A/B ≤36 对，1 个格组）；M = 2–4 个臂组（A/A + A/B 72–144 对，需行级 profile 或 2 个格组）；L = ≥5 个臂组或跨后端矩阵（>144 对，需新 workload/oracle/协议改造）。墙钟耗时未标定。

| 优先 | 候选 | 终态 | 费用 | 入口门禁 | 备注 |
|---|---|---|---|---|---|
| P1 | **W1 `poll(-1)` 等待链路**（Profile rank 1，~5.0–5.5% cycles 的历史线索） | 未纳入 #637 22 条；未证实 | M/L | 先重做 hotspot/语义准入；给出有界等待、drain 非阻塞、dup fd 设计；A/A + A/B | `docs/evidence/621-round2-R1/REPORT.md:148,176` 是**被 R2 r6 取代的工作稿**；数字只能作为线索 |
| P1 | **W3 mutex family 行级分离** | 族级线索 ~9–11%（r6 F.6）；未有单候选 | M | 行级 measured 归因；每候选单因素；机制门七问 | 含 M-61(两锁)/M-62/M-65/M-67/M-10/M-12；先分离再裁决 |
| P2 | M-53 cookie 索引 | INCONCLUSIVE | M/L | 三项迁移义务 + d64 A/A 分辨率 | 详见 §4 行 5 |
| P2 | M-42 `submit_flushes_` | GO（排队） | S | 机制门 + 动态回执带 + 隔离格 + 无生产读者 | 启动属 owner |
| P2 | M-63 / M-65 owed sweep 融合 | INCONCLUSIVE | M | 零长/owed 协议激活 + 交付顺序不变性 | 与 T9 的 zero-op 子单元绑定 |
| P2 | M-10 零长发布三锁融合 | INCONCLUSIVE | M | 零长占比 + 步间语义映射 | 与 T9 的 zero-op 子单元绑定 |
| P2 | M-33 TP cancel ring | INCONCLUSIVE | M | cancel 形态 measured hotspot + 失效机制设计 | 与 T9 的 cancel 子单元绑定 |
| P2 | M-74 `ready()` 快路径 | INCONCLUSIVE | S/M | ready 概率分层 + 合并收益隔离 | 与 T9 的 zero-op 子单元绑定 |
| P2 | M-40 live counters | INCONCLUSIVE | M | 行级归因 + 读者角色 + `dispatch_mtx_` 覆盖 | 见 §4 行 12 |
| P2 | M-37 `remove_exact` → `pop_front` | INCONCLUSIVE | M | 深队列 memmove 份额或证明队列深 ≤1 | **归属 #595**，不是 #621 自启动项 |
| P2 | M-80 `write_all_at` 预校验组合 | INCONCLUSIVE | M | measured hotspot + compose 入口不变量 | **归属 #604**；直通写组合路径 |
| P2 | M-01 scope 扫描 | INCONCLUSIVE | M | `-scope64` 高重复隔离协议 + 机制份额 | free-list 重开属 owner |
| P3 | M-16 state-op 校验收敛 | INCONCLUSIVE | S/M | owner 裁决 seam 冲突 | 先裁决再实验 |
| P3 | M-09 / M-30 / M-57 / M-12 / M-80 hygiene | SEMANTIC_MISMATCH | 代码面 PR（无性能门） | owner 裁决 | 不是性能实验 |
| P3 | M-61 payload 快照 | NO_GO | 卫生路线 | 出现生产消费者或 owner 批准测试专用通道 | 不复活为性能候选 |
| HOLD | H1 / io-wq punt / tp-w8 | NO_GO | N/A | 见 §4 非候选行 | 无新证据不重开 |

---

## 6. 应归属 #595 / #604 / #617 的工作

### 6.1 #595 — 提交批形态与 submission cadence

**状态：维持 `KEEP_IMMEDIATE`。**

| 事实 | 记录 |
|---|---|
| Experiment 1 归属 | `#621` §XI/XII 与评论 1：Experiment 1 由 **#595** 做单因素 submission cadence（immediate vs progress-driven + fallback） |
| R1 D2 保护 | 失败：CPU +4.17%、p95 +5.69%、p99 +7.45%、tput −3.99%（Holm 且复现） |
| R2 归因 | defer 决策链的 residue signal→eventfd write 与 flush 时点迁移，**不是**通知主导 |
| R3/R4/R5 main gate | FAIL：p99 +37.85 / +21.85 / +17.97 pp |
| 重开条件 | RQ1 second-held-entry guard + payload gate + D1/D2/D3、64 KiB、W-03、cancel falsifier 家族全过 + 整机重测；R1–R5 原始数据未归档，只能重测 |
| 归属工作 | H1 提交批形态（非候选 1，NO_GO 本轮）；M-37（批量概念 B，INCONCLUSIVE）；`submit_flushes_` 作为 submit-attempt 计数被 #595 P4 引用（M-42 启动属 owner） |
| 纪律 | 不得把 `#635` 的 drain 收益或 `#634` 的 owed-count 收益当作 #595 的 submission-cadence 证据 |

### 6.2 #604 — 真实 workload 与数据搬运

**状态：OPEN + `HOLD_NO_REAL_APPLICATION`；`READY_FOR_595=false`。**

| 事实 | 记录 |
|---|---|
| G0 真实 workload | **尚未准入**；`#637 §J` 记录 #604 维持 OPEN + HOLD_NO_REAL_APPLICATION |
| `sluice-copy` 定位 | 是参考应用；其收益被 #604 评论定位为 **backend coordination**，不是 application-level 收益；不能把 sluice-copy A/B 当作 #604 G0 通过 |
| Experiment 3 | 数据搬运由 **#604** 做；Experiment 2（单一 coordination elimination，A/B/C/D 四臂）仅在 profile 暴露后做 |
| 归属工作 | M-80 `write_all_at` → `prevalidated_pwrite_step`（INCONCLUSIVE，直通写组合路径）；tp-w8 worker 超配（NO_GO，配置/工作负载面）；应用路径权威覆盖 #621 的性能结论 |
| 纪律 | 任何应用级收益声明必须有真实 workload 准入；没有 `READY_FOR_595=true` 前，不用 sluice-copy 结果替代 #595/#604 的门 |

### 6.3 #617 — kernel boundary 与通知深挖

**状态：OPEN；G1 仍未满足。**

| 事实 | 记录 |
|---|---|
| G1 条件准入 | **不变**。issue #617 body:10,82：需 admitted real workload 证明 material kernel-boundary cost；#622 的 cost map 是 trigger-grade 但条件性（io-wq punt 是 FS 条件行为、最高杠杆 H1 不跨边界、产品 ring flags=0），**不自动触发** #617 |
| P1-A 通知深挖 | 归 **#617**；#621 的最新评论要求先对现行 `poll(-1)` 等待链路重新做 hotspot/语义准入，再决定是否开窄单因素实验 |
| P2 kernel-boundary | 归 **#617**，且 G1 未满足前不得启动 |
| 归属工作 | io-wq punt（非候选 2，NO_GO，非 Sluice 代码）；W1 的等待路径若升级为 kernel-boundary 研究，需 #617 的 G1 准入；`#622` 的 cost map 只能作 trigger-grade 线索 |
| 纪律 | 不得把 io-wq punt 的 FS 条件性观察包装成跨 FS 结论；不得在 G1 未满足时把 kernel-boundary 候选放入 #621 的 A/B 队列 |

### 6.4 三 issue 的共同边界

- **#621** 是 umbrella/decision 与实验执行纪律；**#595** 是 submission cadence；**#604** 是真实 application workload/数据搬运；**#617** 是 kernel-boundary/通知深挖。
- `#621` 可以记录候选、冻结协议、执行 A/B，但不能替代 #595/#604/#617 各自的门。
- `G1 条件准入不变`：在 #617 的 G1 未满足前，kernel-boundary 候选不进入正式 A/B；`KEEP_IMMEDIATE` 在 #595 未重开门前维持；`HOLD_NO_REAL_APPLICATION` 在 #604 未准入前维持。

---

## 7. 环境资格

### 7.1 频率门禁：PASS

| 场景 | 实测 | 结论 |
|---|---|---|
| 空闲 | cpu0 Busy 0.15–0.63%，C6 93.4–99.2%（C3=0），PkgTmp 47–48°C，CoreTmp 39–40°C | 可进入受控负载 |
| 单核（`taskset -c 0 yes`，15×1s） | Busy 98.94–99.89%（均值 99.58%），Bzy_MHz 均值 3478（3408–3496），Avg 3455，CoreTmp 49–51°C，PkgTmp 49–51°C，CoreThr=0 | 单核频率可解释、无掉频 |
| 4 物理核（`taskset -c 0,2,4,6`，避开 SMT 兄弟 10/12/14/16，20×1s） | 四核 Busy 均 100.00%，Bzy_MHz 20 个采样点全部恒为 3200，Avg 3193，CoreTmp 50.1–52.8°C，PkgTmp 52–54°C，thermal_zone0 55–56°C，CoreThr=0 | 多核频率稳定 |
| APERF/MPERF 交叉验证 | 单核 34,653,506,826 / 28,831,697,459 → 3477 MHz（TSC 2893 MHz）；4 核 127,726,045,761 / 115,751,730,428 → 3192 MHz | 与 turbostat 吻合 |
| 与 #637 对照 | 单核 ≈3.44–3.46 GHz，≥4 核 ≈3.20 GHz | 3.5 GHz 仅作单核参考；实测 3.48 未把它当硬门槛 |
| Turbo/节流 | `intel_pstate/no_turbo=0`，cpupower `Active: yes`，turbo_pct=25；core/package throttle_count 前后均 0；dmesg 无 thermal throttle（仅启动期 TM1/governor 注册）；`msr/cpu_thermal_margin` 37°C | Turbo 未禁用，未观察到节流 |
| 功耗计数 | RAPL energy_uj 空闲 35.28 W、4 核负载 35.20 W（5s 平均）；`power/energy-pkg` 3s 均 104.7 J；MSR 0x613/0x64f 经 perf 返回 Invalid argument | 功耗计数**不可信**；但频率在负载下精确停在 3200/3480 MHz 且无掉频，未观察到功耗节流迹象 |

**门禁结论：PASS。** 可解释的单/多核频率行为、Turbo 正常、无未解释锁频/节流。功耗结论不得用于能效声明。

### 7.2 Governor 与 cpuidle 现状

- `intel_pstate=passive`（`driver=intel_cpufreq`）；20 个 CPU 全部 `schedutil`；`scaling_min=1200000`、`scaling_max=3500000`（硬件范围 1.20–3.50 GHz）；无 `energy_performance_preference`；`no_turbo=0`、`max_perf_pct=100`、`min_perf_pct=34`。
- cpuidle：`intel_idle` + `menu`；空闲 C6 93–99%，负载核 C1/C6=0。
- 临时运行时验证：cpu0 governor `schedutil→performance`（回读 performance），cpu0 `scaling_max_freq 3500000→2000000`（回读 2000000），随后立即恢复并回读 cpu0 `schedutil/3500000`；再复核全部 20 CPU 均 `schedutil`、min=1200000、max=3500000；块设备 scheduler 未变。
- 原值备份：`/tmp/sluice621-env-orig.txt`（含 cmdline、intel_pstate、20 CPU governor/min/max、sda/sdb/nvme scheduler）。
- 结束时无遗留 `yes`/`turbostat`/`perf`/`sudo` 进程；uprobe 已删除；btrfs/tmpfs 测试目录已清理。
- 正式 A/B 必须在测量前后记录 governor、min/max、no_turbo、频率采样；若 governor/上限被外部改动，记录 `ENVIRONMENT_BLOCKED`。

### 7.3 PMU 能力

| 项 | 事实 |
|---|---|
| paranoid | `kernel.perf_event_paranoid=2`（本轮复核） |
| perf | `7.3.3-200.fc44` |
| 可用事件（受控负载实测返回计数） | `cycles`、`instructions`、`task-clock`、`context-switches`、`cpu-migrations`、`cache-misses`、`branch-misses`、`bus-cycles`、`ref-cycles` |
| 无缩放验证 | 8 事件（含 bus/ref-cycles）在 `taskset -c 0` 的 5M 循环上无缩放标注；ref-cycles 46.6e9 ≈ TSC 2.89 GHz；IPC≈2.38；单/4 核 APERF/MPERF 与 turbostat 3478/3200 MHz 吻合 |
| 多路复用 | 12 事件强制测试出现缩放：cycles 50.06%、instructions 60.07%、cache-references 60.22%、bus-cycles 39.78%（范围 39.78–60.22%），说明超限计数按比例外推；4 事件无缩放 |
| 不支持事件 | `stalled-cycles-frontend/backend` 返回 `<not supported>` |
| 权限 | paranoid=2 下非特权只能计自身用户态事件；CPU-wide/kernel/MSR 事件需 root。sudo 可用，已成功用 `-C 0` 计 `msr/aperf`、`msr/mperf`、`msr/tsc`、`msr/cpu_thermal_margin`、`power/energy-pkg` |
| 纪律 | 正式结论只使用**无缩放**的事件集；若决定性指标出现任何缩放，该 run 标 `NOT_POWERED` 或 `ENVIRONMENT_BLOCKED`，不得把外推值当实测 |

### 7.4 eBPF 能力

| 项 | 事实 |
|---|---|
| BTF | `/sys/kernel/btf/vmlinux` 存在（7,023,892 B）；`CONFIG_DEBUG_INFO_BTF=y` |
| bpftool feature probe（root） | `bpf()` syscall available；JIT enabled；program types `kprobe`/`tracepoint`/`perf_event`/`sched_cls`/`xdp`/`cgroup_*` 等 available；`CONFIG_BPF_SYSCALL/BTF/BTF_MODULES/BPF_EVENTS=y` |
| 非特权 | `unprivileged_bpf_disabled=2`（仅特权可加载 BPF） |
| tracefs | 挂载于 `/sys/kernel/tracing`，root 可写 |
| tracepoints（实测 PRESENT） | `sched:sched_switch`、`block:block_rq_issue`、`block:block_rq_complete`、17 个 io_uring（含 `io_uring_submit_req`、`io_uring_queue_async_work`、`io_uring_complete`、`io_uring_cqring_wait` 等） |
| futex | 无独立 tracepoint class；仅 `syscalls:sys_enter/exit_futex`（及 wait/wake/waitv/requeue） |
| 计数验证（root） | `sched:sched_switch=1`、`syscalls:sys_enter_futex=1`（sleep 0.5） |
| uprobe/kprobe | `perf probe -x /usr/bin/true --add sluice621_probe=main` 成功、`--list` 可见、`--del` 后复核为 0；kprobe 可用（`available_filter_functions` 97,888 行） |

### 7.5 测试介质

| 介质 | 事实 | 使用注意 |
|---|---|---|
| 根 FS / `/home` | `/dev/sda3` btrfs（subvol `/root` 与 `/home`），`rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2`；btrfs device stats 全 0；8 MiB 写测试成功并清理 | btrfs + SATA SSD，与 #637 的 filesystem-dependent io-wq 观察环境一致；`compress=zstd:1` 参与 I/O 路径，必须在冻结协议中固定 |
| `/tmp` | tmpfs，31.3G 可用；8 MiB 写测试成功并清理 | W-02/W-03 默认 `SLUICE_BENCH_TMP_DIR=/tmp/sluice-bench`（`bench_common.hpp:437`），即默认落在 tmpfs；tmpfs 结果不能外推为磁盘 I/O |
| sda | 447.1G SATA SSD（rotational=0，GS-480），scheduler `bfq` | 根 FS 所在设备；存储 IRQ 可能被 irqbalance 迁移（见 §7.6） |
| sdb | 3.6T SATA HDD（rotational=1，ST4000HKVS002），scheduler `bfq` | 未用于本战役；若用作冷介质需单独冻结 |
| nvme0n1 | 476.9G NVMe，未挂载，scheduler `none` | 当前不可直接作为测试介质；挂载需要 root 与显式介质决策 |
| clocksource | `tsc` | 与 perf 时间基准一致；记录到 meta |

> **记录员注**：W-02/W-03 的默认 tmp_dir 是 `/tmp`（tmpfs）。若实验声称“磁盘/文件系统路径”收益，必须把 `SLUICE_BENCH_TMP_DIR` 显式指向 btrfs，并在协议中固定压缩、COW、dirty/writeback 状态；否则只能声明 tmpfs 路径结果。sda 是 SATA SSD，不是 NVMe；不要把 #637 的 btrfs 观察直接外推到 NVMe。

### 7.6 噪声源

| 源 | 观测 | 处置 |
|---|---|---|
| 系统空闲 | 5s `/proc/stat` 采样 idle=99.21%（total 10006 ticks，idle 9927） | 基线可用；正式测量前后复采 |
| 审计工具链 | zcode-cli 3.5%、node 1.2%、fprintd 0.2%、zsh/systemd-oomd/sshd 各约 0.1%；load average 起始 0.95 来自审计命令 | 正式测量期间关闭/静止非必要 agent 工具；记录活跃进程 |
| tuned | `tuned 'balanced'` 与 `tuned-ppd` 运行中（未改变 schedutil/频率上限） | 记录版本/配置；不要在实验中途切换 |
| irqbalance | active，44 条 IRQ；实测 `ahci IRQ32→CPU14`、`nvme0q0 IRQ34→CPU7`、`IRQ38→6-7,16`；workqueue cpumask=`fffff`；存储 IRQ 可能被迁移 | 正式 A/B 建议停 irqbalance 或固定存储 IRQ 亲和性，并在结果目录记录亲和性快照 |
| 系统服务 | docker/containerd/abrt/mcelog 等；`dnf-makecache.timer` 下次 15:50 | 正式测量窗口避开 dnf/容器 I/O；记录服务状态 |
| 内核 tainted | tainted=4；dmesg: `msr: Write to unrecognized MSR 0x17f by mcelog … CPU_OUT_OF_SPEC`（启动期既有，非本次审计造成） | 记录；不因此判环境失败，但需在 provenance 中披露 |
| 本次审计负载 | `yes` 负载已全部清理（最终 `pgrep` 无结果）；governor/scheduler/背压状态已复原 | 正式测量前再次确认无遗留进程 |
| 电源/温度 | 频率稳定，无节流；功耗计数不可信 | 不以功耗计数作结论；记录温度/频率 |

### 7.7 `ENVIRONMENT_BLOCKED` 风险登记

环境审计的 `environmentBlocked` 字段为空，因此本轮结论是 **PASS**。但以下任一条件出现时，正式实验必须判 `ENVIRONMENT_BLOCKED`，不得降级为 GO/NO_GO：

1. **频率门禁复测失败**：负载 Busy <99%、Bzy_MHz 出现未解释下探、throttle_count >0、或 governor/上限被外部改动。
2. **决定性 PMU 事件出现缩放**：cycles/instructions/task-clock 等关键计数被 multiplexing；外推值不能作为 A/B 判据。
3. **权限/凭据不可用**：需要 CPU-wide/kernel/MSR 事件但无法以受控方式取得 root；或 sudo 凭据无法通过环境变量/受控通道提供。
4. **介质/背压不可控**：irqbalance 迁移存储 IRQ、dnf/容器产生背景 I/O、dirty/writeback 未稳定、btrfs 压缩/COW 状态未冻结。
5. **介质与声明不符**：tmpfs 结果被用于磁盘声明；SATA SSD 结果被外推到 NVMe；btrfs 结果被外推到其他 FS。
6. **cold 格不可构造**：G0 冻结集不含 btrfs cold；若无法通过显式 drop_caches/fadvise/新文件构造并验证 cold 状态，该格记 `ENVIRONMENT_BLOCKED`/`NOT_CONSTRUCTIBLE`，不得用 warm 数字冒充。
7. **噪声/温度异常**：背景负载、温度或频率在 run 中漂移，且无法通过重复/隔离消除。

### 7.8 凭据与脚本卫生（重要）

- 仓库审计发现：归档 `e2f41acb` 的 `results/e5m28-s1-20261118/scripts/driver/e5harness.py` 第 18 行、以及本机 `/home/jnhu/sluice-621-r2/runner_aa_r2.py` 与 `runner_ab_r2.py` 第 39 行均含**明文 sudo 口令**；`e5harness.py` 还硬编码已不存在的 `BIN_A/BIN_B/SESSION/SCRATCH` 绝对路径。
- 这些脚本**不得提交、不得公开、不得粘贴到 issue/PR/本台账**。复跑前必须参数化路径，并从环境（如受控 env var / sudoers / askpass）取凭据。
- 环境审计期间发生过一次 sudo 密码文件被误当作输入读取的事故（内容未写盘、未记录于本台账）；后续所有 sudo 均把重定向绑定到 sudo 本身。该事故作为操作纪律警示保留，**不重复任何内容**。
- 本台账及后续证据目录不得包含口令、token、私钥或可复用的凭据。

---

## 8. 基准 / oracle 入口与完整构建测试矩阵

> **状态：以下入口均由仓库审计整理；仓库审计未运行，本台账会话也未运行。** 记录它们是为了让后续实验者可复现，不代表任何检查已通过。

### 8.1 基准与 oracle 入口

| 入口 | 命令 | 用途 / oracle | 本轮状态 |
|---|---|---|---|
| **G0 冻结矩阵**（W-01/W-02/W-03，#403） | `bash bench/build_profiles.sh release && bash bench/run_matrix.sh /tmp/621-aa-g0 2 7 measured` | 构建 release × {liburing=n,ring} 到独立 `build-bench-{noring,ring}` 并断言 profile（`bench/build_profiles.sh:25-72`）；按冻结参考 `docs/evidence/g0-403-performance-baseline-20261015/raw` 跑 W-01 15 case、W-02 16 case、W-03 6 case，每组 2 warmup + 7 measured；每 run 由 `bench/check_run.py` 校验精确 case 集、每 case warmup/measured 数、exit 0、NOT_RUN 集与 `max_outstanding≤queue_depth`（`check_run.py:80-160`），任何偏差 abort。JSONL 指标：ops、wall_s、ops_per_sec、cpu_s、rss/peak_rss、lat_p50/p95/p99/max_ns、stats_poll_calls/stats_wait_calls、notifications、passes、would_block、stats_max_outstanding、stats_completion_errors、stats_invalid_state。正确性 oracle：每 case 先跑 CAL-oracle-self-test（`bench_common.hpp:214-251`），每个 read 用 `verify_pattern` 按 offset 逐字节校验（`:179-205`），再跑一次无计时 latency pass（`bench_w02_pipeline.cpp:583-596`），并做 stats wiring 检查（`:553-570`）。注意冻结 case 集不含 64K×d1、d2/d3、btrfs cold 格 | **未运行** |
| **W-02 单格过滤**（P1 A/A 最小入口） | `SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 SLUICE_BENCH_CASE_FILTER='=W02-threadpool-4k-d16-w8' SLUICE_BENCH_JSONL=/tmp/w02-tp-4k-d16-w8.jsonl SLUICE_BENCH_TMP_DIR=/tmp/621-fixture build-bench-ring/linux/x86_64/release/bench_w02_pipeline` | 只跑一个冻结 case，仍是完整 oracle：CAL 自检 + 每 op byte pattern 校验 + latency pass + stats wiring；输出同一 JSONL schema。A/A 时对同一 build 产物使用相同 binary sha256 的两次调用/两份副本；case id 规则见 `bench_w02_pipeline.cpp:460-484`（`W02-<backend>-<4k\|64k>-d<depth>[-w<workers>][-saturate]`）。二进制的 profile 由 `build_profiles.sh` 构建并已被 readelf 断言（ring 版链 liburing，noring 版不链） | **未运行** |
| **W-03 单格外部事件循环**（split-owner） | `SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 SLUICE_BENCH_CASE_FILTER='=W03-uring-4k-d16' SLUICE_BENCH_JSONL=/tmp/w03-uring-4k-d16.jsonl SLUICE_BENCH_TMP_DIR=/tmp/621-fixture build-bench-ring/linux/x86_64/release/bench_w03_external_loop` | app-owned epoll loop 在借用的 progress eventfd 上等待，submits 与 driver split-owner；20k×4k reads。case id `W03-<backend>-4k-d<depth>[-w<workers>]`（`bench_w03_external_loop.cpp:472-480`），冻结 d4/d16/d64。oracle 同 W-02（byte verify + stats wiring），并经 `check_run.py` 对照 `w03.jsonl`；用于 PROG-02/03 的端到端驱动形态 | **未运行** |
| **M1 机制仪器 H1/H2/H3** | `bash bench/mechanisms/run_mechanisms.sh /tmp/621-mechanisms 9 1` | 自建 `g++ -O2` 三个独立仪器（H1 router scan vs direct index；H2 owed sweep vs maintained counter；H3 scope first-free vs free-list），默认 `taskset` CPU2；每 rep 先跑 reference-model oracle，负控制必须打印 `CAUGHT`，runner 对 ORACLE FAIL/SELFCHECK FAIL/oracle divergence 失败；输出 `mechanisms.jsonl`、decision 日志、perf-stat 与 provenance。只测机制、不链 Sluice 库，不能替代产品路径证据 | **未运行** |
| **sluice-copy 真实消费者 paired A/A**（off-repo r2 runner） | `xmake f -m release -o build-621-aa --liburing=y -y && xmake build sluice-copy && cp build-621-aa/linux/x86_64/release/sluice-copy /tmp/621-armA && cp /tmp/621-armA /tmp/621-armB && python3 /home/jnhu/sluice-621-r2/runner_aa_r2.py --binary-a /tmp/621-armA --binary-b /tmp/621-armB --src /home/jnhu/sluice-621-r2/data/src-256m.bin --dst-a /tmp/621-dst-a --dst-b /tmp/621-dst-b --pairs 36 --buffer-size 4096 --pipeline-depth 4 --workers 1 --tsv /tmp/621-aa.tsv --meta /tmp/621-aa-meta.json --label aa-4k-d4` | 真实 256 MiB copy，A/A 两臂为同一二进制；每臂独立进程 `taskset -c 2,3`，sync 后等待 `Writeback==0 kB/Dirty≤4096 kB`，AB/BA 交错，performance governor 测量后恢复，`rusage(RUSAGE_CHILDREN)` CPU + wall + `scaling_cur_freq` + ps guard。oracle：exit 0、stdout `copied N bytes (read_ops=… write_ops=… short_writes=…)` 解析、dst sha256==src sha256、跨臂冻结 read_ops/write_ops 完全一致（`runner_ab_r2.py:418-495`）。限制：runner 在仓外、第 39 行含明文 sudo 口令，**不能提交**；`#634` 的 `/home/jnhu/sluice-621-round/run_paired.sh` 已不存在 | **未运行** |
| **ProgressSource / ThreadPool 语义与 mutant oracle** | `xmake f -m release -o build-621-tests --liburing=y -y && xmake test -y progress_source_drain_semantic_oracle_test threadpool_owed_counter_parity_test threadpool_progress_race_test threadpool_progress_pass_bound_test progress_notification_suppression_test` | 运行 9 用例 ProgressSource drain 语义 oracle、8 用例 ThreadPool owed parity（counter↔scan↔sink、zero-op、slot reuse、cancel、concurrent producers）、progress race/pass-bound/suppression。oracle 判定：进程 exit 0 且每用例 ok；mutant 目标（`progress_source_mut_drain_success_keeps_pending`、`threadpool_owed_mut_*`）必须 exit 1 并被命名断言杀死（`xmake/tests.lua:527-537,568-570`）；机制目标 `progress_source_drain_mechanism_test` 未注册，需 `xmake build -y progress_source_drain_mechanism_test && xmake run progress_source_drain_mechanism_test` | **未运行** |
| **ProgressSource TLA+ 模型检查** | `bash scripts/verify_progress_source_model.sh` | TLC 1.7.4（固定 sha256）验证 ProgressSource 安全/无丢唤醒/控制代际/耗尽/截止/宿主绑定/liveness 与 mutant/证书场景；脚本在 jar 缺失时 curl 下载（需 java+curl+网络，或用 `SLUICE_TLA2TOOLS_JAR` 预置）。这是 PROG-02 的模型级 oracle，不等价于 C++ 生产实现证明（VERIFY-06） | **未运行** |

### 8.2 完整构建测试矩阵（原样记录）

```sh
# Release/liburing=y
xmake f -m release -o build-matrix-rel-y --liburing=y -y && xmake test -y

# Debug/liburing=y
xmake f -m debug -o build-matrix-dbg-y --liburing=y -y && xmake test -y

# Release/liburing=n
xmake f -m release -o build-matrix-rel-n --liburing=n -y && xmake test -y

# Debug/liburing=n
xmake f -m debug -o build-matrix-dbg-n --liburing=n -y && xmake test -y

# ASAN（liburing=y，#621 受影响并发套件）
xmake f -m asan -o build-matrix-asan-y --liburing=y -y && xmake test -y progress_source_drain_semantic_oracle_test threadpool_owed_counter_parity_test threadpool_progress_race_test app_copy_pipeline_oracle_test

# TSAN（liburing=y，同样套件）
xmake f -m tsan -o build-matrix-tsan-y --liburing=y -y && xmake test -y progress_source_drain_semantic_oracle_test threadpool_owed_counter_parity_test threadpool_progress_race_test app_copy_pipeline_oracle_test
```

**Sanitizer 插桩自检（每个目标）**：`ldd <binary> | grep -E 'libasan|libtsan'` 且 `nm -D <binary> | grep -c '__asan_|__tsan_'` > 0。注意：`xmake f -m tsan` 不显式带 `--liburing=y` 会静默重置 `liburing=false`，且产物在 `build/<...>/tsan/` 而非 `release/`（`docs/evidence/e1-pending-control-productization-20261023/review-A-concurrency.md:25`）。

**测试计数纪律**：测试总数必须从实际输出读取——解析 `0 test(s) failed out of N` 与 `TEST_EXIT=0`；`100% tests passed` 是百分比，不是 N（#621 评论 4 的验证勘误；本轮未运行，因此当前 master 的 N 未确认）。`#634` 历史矩阵记录为 80/80 & 59/59 × {release,debug} × {liburing y,n}；`#635` 历史矩阵记录为 81/81 ×2 + 60/60 ×2。这些是历史回执，不是当前 master 的实测。

### 8.3 入口使用的额外限制

- G0 冻结 case 集**不含** 64K×d1、d2/d3、btrfs cold 格（#637 §D 记录为 `NOT_CONSTRUCTIBLE`）。若 #621 需要这些形状，必须修改 `bench/bench_w02_pipeline.cpp` 的 case 列表并更新 runner/oracle（methodology 要求新 workload 有 runner 配置或显式 runner 更新），不能把 G0 数字外推到这些格。
- W-02/W-03 的 `canceled_ops` 是 stats 字段，但 bench 不主动发出 cancel；zero-op 也不在 W-02/W-03 的 workload 中。T9 的 zero-op/cancel 必须走协议 oracle 或新增专用 runner（见 §10）。
- `bench/run_matrix.sh` 与 `check_run.py` 只对 G0 冻结矩阵负责；新 workload 不得直接套用其“通过”结论。
- 所有正式 run 必须记录 binary sha256、build dir、liburing 链状态、governor、IRQ 亲和性、tmp_dir 介质、warmup/measured 数、case 集与 oracle 结果。

---

## 9. 战役执行纪律

### 9.1 硬规则

1. **正式测量串行**：同一时间只跑一个正式测量 session；不得并行跑 A/B、不得在测量期间跑 profile/测试/构建。profile 与 clean timing 分离（methodology）。
2. **候选独立 worktree、独立冻结 SHA、独立结果目录**：每个候选从冻结 BASE 新建 worktree；候选 SHA 在第一次计时前登记；结果目录独立，不与其它候选共用；构建用显式 `-o` 新目录并 clean build；不得跨 `--liburing` 复用 build dir。
3. **统计从原始数据独立复算**：保留原始 TSV/JSONL/meta/日志；聚合值必须由第二条路径从原始数据重算（本轮已用 `#637` 的 CSV 复算示范：AB −1.944350 / BA −2.246725 / 全体 −2.0955375）。不得只手抄 runner 的 summary。
4. **语义 FAIL 即 NO_GO**：语义 oracle、mutant、协议断言任何失败即 `NO_GO`；不得削弱断言、跳过测试、加 sleep、扩大 retry 或改 workload 来“修绿”。正确性门优先于性能数字。
5. **不自动 merge、不自动关闭 #621**：实验完成后停在 **Owner Gate**；merge/close 由 owner 决定。PR 只提交产品改动/测试/必要证据摘要，不夹带一次性 runner 或明文凭据。
6. **已否决候选无新证据不复活**：`NO_GO`/`NO_GO_ABORT` 候选必须有满足翻案条件的新证据；M-28 不默认重做。
7. **环境门先行**：每个正式 session 前复测频率门禁、governor、irqbalance/IRQ 亲和性、背景负载、介质状态；任何 §7.7 条件触发即 `ENVIRONMENT_BLOCKED`。
8. **同机配对交错 A/A、A/B**：正式性能判断要求同机 A/A、≥15 个有效交错 AB/BA pairs（历史轮次用 36 对/24 对）；clean runs 判收益，instrumented runs 只归因；不做统一“容许 5% 回归”，不对未预注册的格作非劣效声明。
9. **新 workload 需 runner 配置或显式 runner 更新**：不得从 G0 冻结集外推；不得把 tmpfs 结果当磁盘结果；不得把 SATA SSD 结果外推到 NVMe。
10. **不写密码/敏感信息**：所有脚本参数化路径、从环境取凭据；off-repo runner 不提交；证据目录不含口令/token/私钥。
11. **文档边界**：实验完成后按 AGENTS.md 更新 v2 ledger 的 exact evidence（日期、SHA、配置、命令、原始结果）；root 仍是规范权威；不得从代码反写 contract。
12. **两个未跟踪目录全程不得触碰**：`docs/evidence/621-round2-R1/` 与 `docs/evidence/e5-fullpath-machine-work/` 不读改、不提交、不移动。需要引用时只读；引用其内容必须标明“未跟踪工作副本/已被取代”。

### 9.2 处置词汇（本战役统一）

| 词 | 含义 | 使用约束 |
|---|---|---|
| **GO** | 候选通过语义门、机制门与性能门，可进入产品 PR/合并讨论 | 只是战役判词，不等于自动 merge；Owner Gate 仍须确认 |
| **NO_GO** | 候选被否决（性能不成立、语义不成立或收益不值得） | 记录原因与原始数据；无新证据不复活 |
| **INCONCLUSIVE** | 证据不足以支持任一方（A/A 不安静、样本不足、归因不清、回执缺失等） | 必须写明缺什么；不得当“轻微 GO”使用 |
| **NOT_POWERED** | 实验没有足够分辨力/样本/事件计数来检测目标效应 | 与 INCONCLUSIVE 区分：先修功率再谈结论 |
| **SEMANTIC_MISMATCH** | 候选与 root/契约语义冲突，或需要 owner 先裁决 | 不进入性能实验；走 owner 裁决/代码面 PR |
| **NOT_APPLICABLE** | 候选在 current BASE 无适用对象（已被合并/删除/非本战役范围） | 记录依据；不要重开 |
| **ENVIRONMENT_BLOCKED** | 环境/权限/介质/频率/噪声条件不满足，测量无效 | 不得降级为 GO/NO_GO；修环境后重跑 |
| **NO_CHANGE_JUSTIFIED** | 成本真实存在但移除/优化的收益不足以证明风险与复杂度 | 记录负结论，避免重复研究 |

### 9.3 Owner Gate

每个候选在完成以下全部项后停止，等待 owner：

1. 冻结协议（BASE、候选 SHA、host、编译器/flag、workload、oracle、统计方案）在第一次计时前登记；
2. 语义/协议 oracle + mutant 结果（含 exit code 与命名断言）；
3. A/A 安静度门；
4. 配对交错 A/B 原始数据 + 独立复算；
5. 机制门七问与 authority-contraction 审计（若移动生产权威）；
6. 完整矩阵/套件/sanitizer 回执（按受影响 claim 的范围）；
7. 判词（GO/NO_GO/INCONCLUSIVE/NOT_POWERED/SEMANTIC_MISMATCH/NOT_APPLICABLE/ENVIRONMENT_BLOCKED/NO_CHANGE_JUSTIFIED）与剩余限制。

**#621 在 Owner Gate 前保持 OPEN；不得自动关闭。**

---

## 10. 工作负载短矩阵 T1–T10 与扩展轴

### 10.1 设计原则

- 轴：backend ∈ {ThreadPool, io_uring}；block ∈ {4 KiB, 64 KiB}；depth ∈ {1, 4, 16, 64}；另有 zero-op/cancel 协议单元与 split-owner 外部循环。
- 短矩阵 **T1–T10** 是 10 个格/单元，不是完整的 2×2×4 笛卡尔积。每个 backend 在短矩阵中覆盖 D1/D4/D16/D64 四个深度（跨两种 block），以便先看深度趋势；剩余 8 个组合放入扩展轴 E1–E8。
- 每个计时格必须是 W-02（或 W-03）的**完整 oracle**：CAL 自检 + 每 op byte pattern 校验 + 无计时 latency pass + stats wiring；任何 oracle 失败使该 run 无效。
- 当前 G0 冻结集只含 `4k d1/d4/d16/d64`、`64k d16`、TP 4k d16 w1/w8、4k d16 saturation，以及 W-03 的 4k d4/d16/d64。**T3/T4/T7/T8 的 64K×D4/D64 不在冻结集**，需要先更新 `bench/bench_w02_pipeline.cpp` 的 case 列表与 runner/oracle；在这些格被构造前，不得用 64K×D16 的数字替代。

### 10.2 T1–T10 短矩阵

| 单元 | Backend | Block | Depth | 形态 | 当前 bench 入口 / 构造性 | 主要观察 |
|---|---|---|---|---|---|---|
| **T1** | ThreadPool | 4 KiB | 1 | 单 worker、无饱和 | `W02-threadpool-4k-d1`（冻结存在） | 最低并发、协调成本占比最高 |
| **T2** | ThreadPool | 4 KiB | 16 | 4 workers | `W02-threadpool-4k-d16`（冻结存在；`-w1`/`-w8` 变体也存在） | 主工作负载参考格；owed/cancel 交互 |
| **T3** | ThreadPool | 64 KiB | 4 | 4 workers | **不在冻结集**；需 runner 扩展 | 64K 中等深度；与 T4/T8 比较深度效应 |
| **T4** | ThreadPool | 64 KiB | 64 | 4 workers | **不在冻结集**；需 runner 扩展 | 64K 深队列；`remove_exact`/dispatch 压力 |
| **T5** | io_uring | 4 KiB | 1 | 单驱动线程 | `W02-uring-4k-d1`（冻结存在） | 单请求提交/完成路径 |
| **T6** | io_uring | 4 KiB | 16 | 单驱动线程 | `W02-uring-4k-d16`（冻结存在） | 主 uring 参考格；CQE batch/owed sweep |
| **T7** | io_uring | 64 KiB | 4 | 单驱动线程 | **不在冻结集**；需 runner 扩展 | 64K 中等深度；`io_uring_enter/op` 摊薄 |
| **T8** | io_uring | 64 KiB | 64 | 单驱动线程 | **不在冻结集**；需 runner 扩展 | 64K 深队列；cookie O(R) 与 CQE reap 压力 |
| **T9** | TP + io_uring | 4 KiB | 16（建议） | **zero-op / cancel**（两个子单元：T9-zero-op、T9-cancel） | **不是 W-02/W-03 workload 格**。zero-op：`threadpool_owed_counter_parity_test` 的 zero-op 用例、`uring_owed_residue_test`/`uring_external_loop_test` 的 owed 事件；cancel：`backend_contract_conformance_test`、`request_core_ownership_test`、`progress_source_drain_semantic_oracle_test` 的 cancel/shutdown settle、uring cancel-race oracle；若需计时，新增专用 runner | zero-op：owed 事件产生、drain、publication exactly-once、抑制延迟；cancel：sticky intent、pre-dispatch cancel-win、already-terminal、retire/reuse |
| **T10** | io_uring（+ ThreadPool 对照） | 4 KiB | 16（W-03 冻结 d16） | **split-owner**：app-owned epoll 在借用 progress eventfd 上等待 | `W03-uring-4k-d16`（冻结存在；W-03 还有 d4/d64 与 TP 对照格） | 外部循环、通知/ack、wait_one、宿主绑定 |

> **T9 的定位**：zero-op 与 cancel 是**协议单元**，不是吞吐格。它们首先回答 `PROG-01/02`、`CANCEL-01`、`REQ-03` 的语义与 liveness；只有协议通过且确认需要计时时，才扩展 runner 做单因素 A/B。不得用 W-02 的 `stats_canceled_ops` 字段冒充 cancel workload。

### 10.3 扩展轴

| 轴 | 取值 | 当前可构造性 | 使用规则 |
|---|---|---|---|
| **backend × block × depth 剩余组合** | E1 TP 4K D4；E2 TP 4K D64；E3 TP 64K D1；E4 TP 64K D16；E5 UR 4K D4；E6 UR 4K D64；E7 UR 64K D1；E8 UR 64K D16 | E1/E2/E4/E5/E6/E8 已有冻结格（4K D4/D64、64K D16）；E3/E7（64K D1）需 runner 扩展 | 仅在短矩阵给出方向后扩展；每格仍走完整 oracle；不得跳过 A/A |
| **workers** | 1 / 4 / 8（ThreadPool） | 冻结集只有 4K D16 的 w1/w4/w8；其他深度需 runner 扩展 | 固定 backend/block/depth，仅变 workers；记录 `worker_count` 与 `active_workers_`；不得把 worker 超配（tp-w8）当特性 |
| **burst / smooth** | 饱和/突发 vs 平滑（saturating round） | 冻结集只有 4K D16 的 `-saturate`；其他格需 runner 配置 | 饱和形态用于暴露排队/积压；平滑形态用于稳态；两者分开判词，不得混合 |
| **warm / cold** | warm（page cache 命中） vs cold（drop cache / 新文件 / fadvise） | warm 是默认；**cold 不在 G0 冻结集**，`#637 §D` 记 `NOT_CONSTRUCTIBLE` | cold 必须显式构造并验证（例如 drop_caches、新文件、posix_fadvise），记录 root 操作与验证方法；不能构造就记 `ENVIRONMENT_BLOCKED`/`NOT_CONSTRUCTIBLE` |
| **btrfs / tmpfs** | 根 btrfs（sda SATA SSD，`compress=zstd:1`） vs `/tmp` tmpfs | 两者都可写；bench 默认 `/tmp` tmpfs；btrfs 需 `SLUICE_BENCH_TMP_DIR` 指向 btrfs 路径 | 同一比较内固定介质；记录 mount options、compression、COW、dirty/writeback、free space；不得跨介质直接比较或外推 |

### 10.4 每个计时格的必带元数据

- `case_id`、backend、block_size、depth、workers、saturating、warm/cold、介质路径与 mount options；
- BASE SHA、候选 SHA、binary sha256、build dir、liburing 链状态、编译器版本/flag；
- warmup/measured 数、run 顺序/rotation、AB/BA 交错、丢弃的 warmup 数；
- governor/min/max/no_turbo、频率采样、IRQ 亲和性、背景负载、温度；
- oracle 结果（CAL 自检、byte verify、latency pass、stats wiring）、exit code、`0 test(s) failed out of N`（若跑测试）；
- 原始 JSONL/TSV 路径、独立复算脚本与结果、判词与剩余限制。

---

## 11. 接力检查单（下一轮实验者）

- [ ] 重新核验 `git rev-parse HEAD`、`origin/master`、`git status --porcelain`、`git worktree list`；确认 BASE 仍是 `5d94f233…`，或显式记录新 BASE。
- [ ] 确认两个未跟踪目录仍未被触碰；新结果写入新的独立目录。
- [ ] 复测频率门禁、governor、irqbalance/IRQ 亲和性、背景负载、介质状态；把快照写入结果目录。
- [ ] 选择 §4/§5 中**已准入**的候选；先写冻结协议与 falsifier，再登记候选 SHA（计时前）。
- [ ] 先跑语义 oracle/mutant；FAIL 即 NO_GO，停止。
- [ ] 先跑 A/A 安静度门；再跑 ≥15 个有效交错 AB/BA pairs（按判词需要决定是否 36 对）。
- [ ] 从原始数据独立复算统计；保留所有原始文件与命令。
- [ ] 按 §9.2 给出判词；按 §9.3 停在 Owner Gate；不自动 merge、不自动关闭 #621。
- [ ] 若移动生产权威，补 authority-contraction 审计与机制门七问；更新 v2 ledger 的 dated 621 节（如有产品改动）。

---

## 12. 本台账的已知限制

1. **没有运行任何构建/测试/基准/TLC/perf/eBPF**；所有性能数字都是历史证据或审计转述，不是本轮实测。
2. **当前 master 的测试总数 `N` 未确认**；不得把 `100% tests passed` 当 N。
3. **环境审计的 turbostat/APERF-MPERF/PMU/eBPF 实测未复跑**；本轮只重读了 sysfs/procfs/块设备事实。环境资格 PASS 的依据是环境审计的原始记录。
4. **`#637` 归档未做 2127 项字节级 `sha256sum -c`**；已做 2127/2127 路径存在性核验与三份文档抽样哈希比对，以及 CSV 判词数字复算。
5. **`docs/evidence/621-round2-R1/REPORT.md` 是已被取代的 R1 工作稿**；不得作为最终结论。最终 `#635` 报告在 `0e50612c`，不在 master。
6. **`#637` 的本地 16 份文档是未跟踪工作副本**；三份抽样与 `e2f41acb` 一致，但未逐份比对全部 16 份。需要权威引用时使用 `e2f41acb`。
7. **候选终态是 `#637` 归档的原样转写**；本台账未重新做实验、未重新裁决。若归档与 issue 正文冲突，以最新归档/closeout 为准；本台账已标注 `#637` 头部 SHA 与 closeout SHA 的差异。
8. **实验费用是规划级**：用配对轮次/矩阵格计数，未标定墙钟耗时；不得当作排期承诺。
9. **优先级是战役规划标签**，不是 root/ledger 状态；不得用它替代 owner 裁决。
10. **`#13`/`#16` 的 `M-61` 重号**按归档保留；不得合并或重编号。

---

*台账结束。任何后续修改请追加“日期 + 变更 + 依据”，不要覆盖历史记录。*
