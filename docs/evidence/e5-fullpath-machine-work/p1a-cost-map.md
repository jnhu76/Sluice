# P1-A io_uring 全路径分段归因（Execution Cost Map，e5-fullpath-machine-work）

- 执行：归因测量员（P1-A），2026-11-18 02:49–04:20（本地时间）
- 基线：master `5d94f233967c3aef993eb19ba9e3252c532b0951`（与 repo-reality.md / P0-C 冻结基线一致；
  会话前后 `git status --porcelain` 一致 = 两个既有未跟踪目录，未修改任何被跟踪文件）
- 性质：纯测量研究（RESEARCH_ONLY；无任何生产/API 改动；未新建 profiler/框架，
  复用 #622 已证观测路径与 `bench/attribution.sh` 分层方法）
- 原始数据与临时脚本：全部在 `/tmp/sluice-p1a/`（不入仓库；§10 清单）
- 前置：P0-C（p0c-cache-study.md）、合并 DAG（dag.md，80 边 / 22 候选）、history-digest §3（#622）
- 本内核 7.3.3-200.fc44，机器同 P0-C（E5-2666 v3 10C/20T、62 GB、/home=btrfs SATA SSD
  `compress=zstd:1`（scratch +C）、/tmp=tmpfs、perf_event_paranoid=2、nmi_watchdog=1、bpftrace 0.24.2）

## 0. Freeze 与口径

| 项 | 值 |
|---|---|
| 计时构建 | `build-bench-ring/linux/x86_64/release/bench_w02_pipeline` sha256 `03dbef09…`（与 P0-C build-profiles.log 逐字节一致；本轮全部计数层用它） |
| 符号化构建 | `/tmp/sluice-p1a/profiling-build/bench_w02_pipeline` sha256 `e929c1fe…`（attribution.sh 同配方 `-O3 -g -DNDEBUG`，链接同一 release 静态库；**绝对吞吐与基线不可比，仅用于 perf record/report/annotate 定位**） |
| regime 集（5） | `W02-uring-4k-d4` @btrfs warm；`W02-uring-4k-d16` @btrfs warm；`W02-uring-4k-d16` @tmpfs；`W02-uring-4k-d64` @btrfs warm；`W02-threadpool-4k-d16-w8` @btrfs warm（P0-C §10 推荐 1/2/4/5 + 必含 btrfs-warm-inline 与 tmpfs-punt 对照；推荐 2/3 的 uring 侧合并为同一格，**tp-w4 未单列**——5-regime 上限下 TP 形态对照由 w8 承担，见 §6 未列入行） |
| bench env | `SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7`（perf stat/strace/bpftrace/JSONL 层）；perf record 用 `1/3` 与重录大样本 `2/20`；锁 shim 用 `0/1`（G0/attribution.sh 约定）；`SLUICE_BENCH_CASE_FILTER='=<case>'` 精确匹配 |
| ops 分母 | 非饱和格每轮 20000 ops × 3 phases；perf stat/strace/bpftrace 窗口 = 9 rounds = **540000 ops**；shim = 1 round = 60000 ops；perf record 大样本 = 22 rounds = 1320000 ops |
| governor | 两轮独立存证/恢复（campaign 与 E-redo）：逐 CPU 存原值 → performance（读回 20/20）→ 计时 → 恢复并读回 **20/20 match=yes**（`/tmp/sluice-p1a/governor/{original,set-verify,restore-verify}.txt`，两轮各三列） |
| 频率伴随 | freq_sampler 0.2s×20CPU：3612 样本，负载中位 **3,500,000 kHz**、min 1,168,676 kHz（空闲间隙，36.4% 样本 <3.3 GHz 为轮间空闲）；turbostat 全程伴随（interval 5，3520 行，active CPU Bzy_MHz 3.1–3.4 GHz，LLC 命中 63–97%） |
| 主口径 | **instructions/op（u 与 u+k 两列分开）为主计数**；cycles (u/k)、task-clock、branches、branch-misses、LLC-misses(cache-misses)、context-switches 为辅列；**每列独立报告，三量纲不相加** |
| perf stat 事件集 | A: `instructions,cycles,task-clock,context-switches,cpu-migrations`（固定计数器，0 复用）；B: `branches,branch-misses,cache-misses`（3 GP，**66% enable 下的 scaled 计数**，CV 0.5–5%）；C1: `instructions:u,cycles:u`；C2: `instructions:k,cycles:k`（均固定计数器 0 复用）。sudo 全内核态；null（`=ZZNULL`，仅 fixture 写 + CAL）按 Δ 法扣除 |
| 正确性门 | 每 regime 全部 bench 层（JSONL 1 + perf stat 24 + strace 1 + E1 1 + E2 1 + perf record 2 + shim 1 ≈ 30 次）逐层断言 `failing repetitions=0`，全部通过，无一层失败 |
| warm 判据 | bench 构造性 warm（fixture 写驻留）+ `diskstats` Δ=0 直接验证（§3 段 6） |

**总口径声明（读数前必读）**：本报告所有"份额"是 perf 采样**定位证据**，
**段与段、份额与份额之间不相加**；每 op 计数（instr/cycles/锁/syscall）各列独立成立；
u 与 u+k 是不同口径；profiling 构建的采样份额不可折算为吞吐。

## 1. regime × 主量化总表（每 op；Δ full−null / 540000 ops；CV 为 3 重复的样本 CV）

| 列 | d4-btrfs | d16-btrfs | d16-tmpfs | d64-btrfs | tp-d16-w8-btrfs |
|---|---|---|---|---|---|
| ops/s（JSONL 中位，7 reps） | 600,914（CV 7.1%） | 584,580（2.9%） | 410,477（3.0%） | 537,942（1.6%） | 345,805（5.5%） |
| CPU/op µs（rusage） | 1.666 | 1.711 | 4.857 | 1.862 | 10.345 |
| p50 延迟 ns | 5,440 | 25,953 | 37,513 | 119,951 | 44,077 |
| instructions u+k | **6498.6**（CV 0.06%） | 6492.0（0.5%） | 9698.3（5.5%） | **7541.2**（0.3%） | 15757.4（0.4%） |
| instructions:u | 3858.5 | 3842.7 | 3862.2 | 4901.0 | 4033.9（CV 均 ≤0.02%） |
| instructions:k | 2639.5 | 2630.5 | 5731.6 | 2628.9 | 11431.6 |
| cycles u+k | 5955.5（0.6%） | 6119.4（0.7%） | 13870.5（2.9%） | 6689.2（0.4%） | 30653.5（1.1%） |
| cycles:u | 2105.6 | 2190.5 | 2635.7 | 2584.9 | 8226.0 |
| cycles:k | 3876.2 | 3909.4 | 11213.1 | 4102.0 | 22068.4 |
| task-clock µs/op | 1.751 | 1.807 | 4.676 | 1.964 | 12.366 |
| branches | 1441.8（1.5%ᴮ） | 1442.8（0.1%ᴮ） | 2292.8（2.2%ᴮ） | 1819.6（0.2%ᴮ） | 3237.4（0.3%ᴮ） |
| branch-misses ᴮ | 8.92 | 12.92 | 21.98 | 14.50 | 40.59 |
| LLC-misses ᴮ | 21.60 | 20.96 | 18.88 | 19.41 | 18.56 |
| context-switches | 0.0001 | 0.0003 | **0.231** | 0.0002 | **1.295** |
| 锁 acquisitions/op（shim） | **19.50**（0% 竞争） | **16.88**（0%） | 16.95（0%） | **16.22**（0%） | **18.85（11.16% 竞争）** |
| enter/op（strace） | 1.000052 | 1.000052 | 1.000052 | 1.000052 | n/a（启动 1 次 ≈0.000002/op） |
| pread64/op | 0.000011 | 0.000011 | 0.000011 | 0.000011 | **1.000011** |
| futex/op（strace） | ~0 | ~0 | ~0 | ~0 | **3.200**（均值等待 42 µs） |
| punt 率（E1） | 5.2e-5 | 5.2e-5 | **1.0000** | 5.2e-5 | n/a |
| dso 份额：kernel/bench/libc | 64.8%/21.3%/10.9% | 65.4%/22.1%/9.8% | **83.1%/9.7%/5.8%** | 61.2%/26.0%/10.7% | **74.3%/6.6%/18.6%** |

ᴮ = 66% enable 的 scaled 事件（nmi_watchdog 占 1 GP），绝对值经 scale 校正，CV ≤5%。
注：instructions:u 列与 P0-C §4 表 instr/op 列不可直比——比值恒 9/7，见 §7-3。

## 2. Execution Cost Map：11 段 × regime（非相加）

每格给"该段在本 regime 的测得证据"；**行内证据类型不同（计数/份额/时延）不得纵向求和**；
份额列是 cycles 采样定位证据，仅作相对定位。

| # | 段（DAG 锚） | 证据 | d4-btrfs | d16-btrfs | d16-tmpfs | d64-btrfs | tp-w8-btrfs |
|---|---|---|---|---|---|---|---|
| 1 | 准入 reserve/accept（M-01/02/03/07/09/16） | cycles 份额：`RequestScope::submit_read`+`AsyncIoContext::submit_read`+`RequestCore::reserve/accept` 合计（进程全线程分母） | 2.03% | 2.16%（1.32+0.98+0.38+0.14） | 0.82%（分母含 iou-wrk 内核样本，稀释，见下） | 2.63%（1.68+1.05+0.52+0.14） | 0.59% |
| | | 锁：core reserve+accept 各 1 次/op（shim 核心族内）；access_mtx_ 1-2 | 同左 | 同左 | 同左 | 同左 | 同左 |
| 2 | 后端分发+队列（M-20/34/37） | `dispatch_after_accept` cycles 份额；dispatch 队列长≈1 → M-37 memmove 份额≈0 | 0.88% | 0.82% | 0.40% | 0.82% | 0.10% |
| 3 | SQE 分配/准备（M-35/18/39） | `dispatch_one_locked` 内 get_sqe/prep（份额整体 2.2-2.3%，行级无单热点）；SQE/op=1.000052 | 2.34% | 2.22% | 1.37% | 2.16% | n/a |
| 4 | cookie/Router/TransportLedger（M-38/40/41） | 含在 dispatch_one_locked 行级分布（无单条 >8%）；cookie 单调分配 | 同上 | 同上 | 同上 | 同上 | n/a |
| 5 | io_uring_enter（M-42） | enter/op=1.000052（strace ×3 regime 一致）；enter 在 dispatch_mtx_ 持有中（结构事实，:44-89 本会话核实）；`submit_transport_locked` 自身份额 0.01%；submit_flushes_ fetch_add 1/op 在早退分支后 | 23.8%ᴷ | 23.2%ᴷ | 9.9%ᴷ | 21.3%ᴷ | 30.5%ᴷ（pread64 syscall 骨架，同为"每 op 1 次 syscall"形态） |
| 6 | 内核 issue/FS/PageCache（M-50） | **s2c 延迟直方图 p50**（bpftrace submit_req→complete，n=540028 精确） | [512,1K)ns 96.9% | [512,1K) 95.2% | [2K,4K) 77.1% | [512,1K) 90.1% | n/a |
| | | kernel cycles:k/op；数据搬运+FS 命中符号合计（rep_movs + filemap_read/shmem + filemap_get_*） | 3876 cyc；≈24.1%（19.14+3.10+1.12+0.72） | 3909；≈24.2%（19.19+3.30+1.02+0.66） | 11213；≈13.9%（9.68+2.61+1.65） | 4102；≈23.6% | 22069；≈7.5%（5.52+1.16+0.85） |
| | | 设备 I/O（diskstats Δ，全程） | **0 读 0 扇区** | 0/0 | 0/0（tmpfs） | 0/0 | 0/0 |
| 7 | inline 执行 vs io-wq punt（#622 H2） | punt 率；punt 内核代价 = d16 tmpfs−btrfs 同格差 | 0 | 0 | **1.0000/op**；Δcycles:k **+7304/op**、Δtask-clock **+2.87 µs/op**、Δctx-sw +0.23/op；io_wq*/task_work*/cq_unlock_post 机器 ≈8.3%（1.65+0.99+1.76+1.00+1.87+ spin_lock 份额部分） | 0 | n/a（物理 pread 形态：1 pread64/op + futex 3.2/op；内核 futex/锁机器 qsl+futex_wake+__futex_hash+psi+smp_call ≈8.3%） |
| 8 | CQE 收割/路由/seen（M-51/52/53/55） | `find_live_router_cookie_`（M-53）**份额随 D 增长**；`handle_one_cqe`+`reap_cqes`+`finalize_operation_terminal_` 合计；CQE 批形（twr） | **0.14%**；1.68% | **1.03%**；2.36% | 0.30%ᵈ；1.19% | **1.74%**；2.06% | n/a（完成经 pread 返回，无 CQE 通道） |
| 9 | terminal/release_execution（M-30/31/56/58） | `offer_terminal`+`release_execution` 份额；core 锁 2 次/op | 0.59% | 0.63% | 0.26% | 0.52% | 0.22% |
| 10 | 发布/观察者/结果（M-61/62/75；M-63 门） | `publish_one`（begin+complete 2 core 锁）+`take`（M-75 consume）；deliver_event（claim/retire 2 core 锁，空体 sink）内联于 publish_one；**uring owed sweep 循环休眠**（§4-3） | 0.55+0.62 | 0.43+1.08 | 0.4+1.1ᵈ | 0.44+2.34 | 0.23+0.39 |
| 11 | 通知/driver wake/idle（M-11/67/70/71） | evfd_signals/op（kprobe:io_eventfd_signal）；evfd_reads/op（kprobe:eventfd_read）；wakeup→switch p50；futex/op | **1.000**；0；n≈32 事件 | 1.000；0 | **1.906**；4；iou-wrk p50 [512,1K) | 1.000；0 | 0；4；worker p50 [1K,2K)；**futex 3.2/op** |

ᴷ = 段 5 的"每 op 一次 syscall"内核机器份额 = `entry_SYSCALL_64+entry_SYSRETQ_unsafe_stack+syscall_return_via_sysret+do_syscall_64+io_submit_sqes+__io_prep_rw+__io_submit_flush_completions` 之和（d16-btrfs：3.82+11.56+3.21+0.79+0.94+0.75+0.68 = 23.16%；**跨符号求和仅作段级定位，仍非相加口径**）。ᵈ = uring-tmpfs 的用户态份额以进程全线程 cycles 为分母（含 iou-wrk worker 的内核样本），同符号份额系统性低于 btrfs 格（bench dso 9.7% vs 22.1%），跨 FS 比较份额无意义，只比 per-op 计数。

段内结构事实（本会话读码核实）：
- 段 5 enter-under-lock：`uring_transport.cpp:44-89` dispatch_mtx_ 持有中调 submit_transport_locked（M-42 结构事实复核通过）。
- 段 8 逐 CQE 一锁：`uring_completion.cpp:99-151` handle_one_cqe 每次 dispatch_mtx_（M-52 结构事实，未重跑 flow-audit）。
- 段 10 owed 门：`uring_backend.cpp:657-663` 单次 dispatch_mtx_ 收购读 owed_count_；**逐槽循环仅在 owed_count_≠0 时执行**。

## 3. 端到端关键路径（一次异步 read 的实测机工链）

W-02 滑窗驱动：每 op = 1 次 `scope.submit_read` + 1 次 `wait_for(oldest)` + 1 次 `take`
（bench_w02_pipeline.cpp:336-440）；无观察者注册（take 在发布后消费）→ 全部完成走
publish_one→deliver_event 直投（**owed 路径不动**）。

```
scope.submit_read
  └ reserve_slot_ O(S) 扫描（request_scope.cpp:46-55）
  └ AsyncIoContext::submit_read（M-02 constexpr 校验 → M-03 access_mtx_ 临界区 ×1 → M-04 统计 → M-05）
      └ UringAsyncBackend::submit_read（M-16 validate）
          └ core reserve（M-07，core 锁 ×1）→ accept（M-09，core 锁 ×1）
          └ dispatch_after_accept（dispatch 锁 ×1，持有中）：
              dispatch_one_locked（M-34→35 get_sqe → 37 front 移除 → 38 cookie → 39 prep → 40/41 router/ledger）
              → submit_transport_locked（M-42：dispatch 锁内 1 次 io_uring_enter；submit_flushes_ +1）
内核（同一次 enter 内联完成）：io_submit_sqes → filemap_read（页缓存命中，rep_movs 拷贝 4KiB）
  → io_eventfd_signal ×1.0/op → CQE 就位（无 task_work：twr 稳态 0 次）
wait_for（M-74）→ wait_one（M-72：access ×2；run_progress_pass_ M-67：
  core health ×1 + progress snapshot/wait_health_failed ×2）
  └ backend poll（M-48/60/63）：dispatch 锁（清空队列，0 SQE 早退）→ reap_cqes（M-51）
      → handle_one_cqe（dispatch 锁 ×1/CQE；M-53 O(R) 逆向扫描）
      → finalize_operation_terminal_（M-56）→ offer_terminal（core ×1）+ release_execution（core ×1）
      → pass_bound 弹出（dispatch 锁）→ publish_one（M-61：begin+complete core ×2）
      → deliver_event（M-62：claim+retire core ×2，空体 sink）→ owed 门（dispatch 锁 ×1，恒 0 跳过）
      → immediate_work_remains（dispatch 锁 ×1）+ outstanding（core ×1）
take（M-75：consume_public_result，core ×1）
```

**每请求锁账验证（DAG §3 vs shim 实测）**：DAG 断言 access 1-2 + 后端 2-3 + core 7-9 +
progress 1-2 + uring 锁内 1 enter / TP 每 op 1 syscall。实测（LD_PRELOAD 逐地址计数，
G0-proven 方法的逐地址变体，行为不变仅聚合键变更）：
- 总量：uring **16.22–19.50 锁/op**（DAG 段 3 给出 11-16 + pass 级 3-6 = 14-22 → **实测落带内**）；
  每 pass 后端机器（dispatch 门×2 + owed 门 + immediate + progress 对）随深度摊薄：
  d4 19.50 → d16 16.88 → d64 16.22 锁/op（**测得的单调摊薄**）。
- 竞争：uring 三格 **contended=0**（G0 的 0.0000% 复现）；tp-w8 **contended 11.16%**
  （126,203/1,131,104），最大簇 11.21/op @9.1% 竞争 + 5.14/op @20.7% 竞争。
- futex 交叉验证：tp-w8 futex syscall 3.200/op（strace，均值等待 42 µs）；uring ≈ 0。
- 族级映射（11.19/op≈core 族、4.25/op≈dispatch 族、1.25/op≈progress/access 族）按
  每 op 速率与 DAG 带匹配，**无调用点级证据，判 PARTIAL**。
- TP 每 op 1 syscall：pread64 = 1.000004/op（bpftrace 540002/540000）/ 1.000011/op（strace
  540006/540000，两源启动残差口径不同）✓；
  uring 每 op 1 enter：1.000052/op（540028/540000，28 为构造残差）✓——DAG §3 尾注全部成立。

## 4. 重点候选边量化结果（逐条对账 DAG §4 候选）

### 4-1 M-28 syscall_count_（候选 #1）——MEASURED，本轮最大单指令级定位
- 指令级：`worker_loop` 内 `lock addq $0x1,0x198(%r15)`（threadpool_backend.cpp:434 编译产物）
  占 worker_loop 本地样本 **30.24%**；worker_loop 符号占全进程 cycles 1.83%
  → **≈0.554% 全局 cycles ≈ 170 cyc/op ≈ 48 ns/op**（tp-w8，30653 cyc/op 口径）。
  无读者（唯一读者在测试宏内，DAG §7-1 已核实，本会话未重复 grep）。
- 机制门证据补齐：写侧/成员无宏门控 + 测试 oracle 在测试宏内 → 候选 #1 的
  "单因素 A/B 证明生产路径受益"前置中的 measured hotspot 一项已落地。
- 8 worker 同行竞争放大（30% 本地份额 >> 其他指令）；该行同时是 shim 最大簇（11.21/op @9.1% 竞争）的物理根源之一。

### 4-2 M-53 cookie O(R) 逆向扫描（候选 #5）——MEASURED（3 深度标定）
- 符号自份额（全进程 cycles）：**d4 0.14% / d16 1.03% / d64 1.74%**（12K/12K/13K 样本）。
- 行级：循环体（`cmpb live 标志` 28.5% + `cmpq cookie` 8.7% + 回边 jne 23.8% + `subq $0x20`
  步进 9.6%）≈ 符号内 79%——逆向线性扫描本体。
- 折算 cycles/op：d4 ≈8、d16 ≈63、d64 ≈116（随 D 单调；d16 每条目 ≈7.9 cyc 高于 d4/d64
  的 3.6-4 cyc/条，缓存效应方向性未分辨——**增长形状 O(D) 成立，精确斜率 INCONCLUSIVE**）。
- H1（user_data 编码路由索引）目标即此份额；迁移三义务（ledger 单调/控制 CQE exact/cancel 定向）不变。

### 4-3 M-63 uring owed sweep 逐槽锁（候选 #6）——本形态测不到循环成本（gate-only）
- `poll()` 中 owed_count_ 门每 pass 1 次 dispatch 锁收购（`uring_backend.cpp:657-663`）；
  逐槽循环仅在 owed_count_≠0 执行。W-02 bench 无零长 op → publish 链无
  publish_zero_op_inline → **owed_count_ 全程 0，循环从未执行**。
- 结论：候选 #6 的"逐槽锁→单锁快照"在 W-02 读取形态的机工增量 ≈ 0（循环休眠）；
  其既证数据（H2a −370ns/pass @C=64 idle）来自 idle/控制面形态，本 regime 集不可复测。
- **判 NOT_OBSERVABLE（bench 形态），非零成本证明**。

### 4-4 M-42 submit_flushes_（候选 #11）——MEASURED-small
- fetch_add 位于早退分支之后（`uring_transport.cpp:218-235` 本会话核实）→ 每 flush 1 次
  RMW，flush=1/op；符号 `submit_transport_locked` 自份额 0.01%（d16-btrfs，12K 样本）。
  无读者（测试宏内）。可消除机工 ≈ 每次提交 1 条 lock-free xadd（非锁内长指令），
  cycles 量级 << M-28（提交线程独占该行，无竞争放大）。

### 4-5 M-03/M-07/M-09 锁往返计数——MEASURED（总量）、PARTIAL（族映射）
- 见 §3 锁账段：总量落 DAG 带；族映射按速率匹配无调用点证据。

### 4-6 M-37 remove_exact(front)（候选 #4）——前提在本形态不成立
- dispatch 队列在提交路径即时清空（dispatch_after_accept 循环）→ 测量窗口内队列长 ≈1
  → front 删除的 (size−1)×16B 前移份额 ≈0（dispatch_one_locked/dispatch_after_accept
  行级无 memmove 类热点）。候选 #4 的"深队列派发 memcpy 占比"测量在 W-02 无法构造
  （NOT_CONSTRUCTIBLE in this shape）；其替换的 fail-fast 语义论证仍按 DAG 原文。

### 4-7 M-16 双层校验（候选 #3）——PARTIAL
- uring `submit_read` 自份额 0.98%（d16-btrfs，含 validate+准备）；context 层校验内联于
  `AsyncIoContext::submit_read`。两层的行级分离受内联限制，无法给出"backend 层 state-op
  半边"的独立份额——候选 #3 需的收敛前证（结构保证/seam 冲突）不变，测量仅确认
  该路径整体 ~1% cycles、无单指令热点。

### 4-8 M-61/M-62 发布/交付 4 core 锁/op（候选 #15/16/17）——PARTIAL
- 计数侧：shim 核心族 11.05-11.19 锁/op 覆盖 reserve+accept+offer+release_execution+
  begin+complete+claim+retire+health+outstanding（10 次）✓ 与 DAG 7-9 带 + pass 级读一致。
- share 侧：claim/retire/begin/complete 全部内联进 publish_one（0.43-0.55%）与
  `RequestCore::*` 小符号，无法行级分离——融合类候选（#15/16/17）的 measured profile
  前置仅获得到族级（core 族 ≈11 锁/op × pthread_mutex_lock+unlock 合计 6.6% cycles
  为 uring 形态最大用户态机工家族），单候选增量须 A/B 裁决。

## 5. Top 10 热点（按 可消除机器工作 × 归因可信度 × 负载价值 排序；非按采样占比）

| # | where | 可消除的机器工作 | 证据（口径） | 可信度 |
|---|---|---|---|---|
| 1 | **H1 提交批形态**（M-34/35/42，`uring_transport.cpp:228-231` per-dispatch flush） | 每 op 1 次 `io_uring_enter` 的固定内核机工（syscall 骨架+io_submit_sqes ≈23.2% cycles 定位、段级）；窗口批填后单次 enter 可摊薄（#622 C1：enters/op 1.0→0.0625 时 instr/op −65%、cyc/op −85%） | enter/op=1.000052（strace，540028/540000，3 regime 一致）；enter 在 dispatch_mtx_ 内（结构）；kernel dso 65-83% | **MEASURED**（形状+份额；消除机制属提交错峰策略变更，归 #595 系） |
| 2 | **M-28**（tp 路径，threadpool_backend.cpp:434） | 每物理 syscall 一次无读者 `lock xadd`：48 ns/op（0.554% 全局 cycles），8-worker 同行竞争放大为 worker_loop 30.24% 本地份额 | 指令级 annotate + shim 簇 + strace futex 交叉；读者全在测试宏 | **MEASURED** |
| 3 | **M-53**（`uring_transport.cpp:551-557`） | O(R) 逆向路由扫描：d64 ≈116 cyc/op（1.74% cycles），随 D 单调；H1 编码可换 O(1) 路由 | 3 深度份额标定 + 行级循环体 79%；迁移三义务待证 | **MEASURED**（增长）/ 迁移收益 PARTIAL |
| 4 | **段 7：io-wq punt 内核 handoff**（tmpfs 形态，对照 #622 H2） | punt 形态每 op 多付 +7304 cycles:k（+2.87 µs task-clock）、s2c p50 512-1K→2K-4K、io_wq 机器+内核自旋 ≈10.5% cycles（io_worker_handle_work 1.65+io_wq_enqueue 0.99+tctx_task_work_run 1.76+task_work_add 1.00+io_cq_unlock_post 1.87+qsl 3.21）——FS 条件性形态成本（btrfs warm=0） | punts 1.0000/op（540028/540028 精确对账）；同格 u 侧 instr/op 差 0.5%（3843 vs 3862）→ 差异全在内核侧 | **MEASURED**（消除属 FS/形态选择，非 Sluice 侧改动） |
| 5 | **M-62 claim/retire 空转**（`uring_backend.cpp:610-614`） | 每 op 2 次 core 锁向生产空体 sink 投递（"无观察者跳过"候选 #17）：core 族 11.05-11.19 锁/op × mutex 家族 6.6% cycles 的组成部分 | 计数实测（shim 族）+ 内联限制无法行级分离 | **PARTIAL** |
| 6 | **M-61 begin/complete 两锁**（候选 #16，同族） | 每 op 2 次 core 锁可融合（步间夹记录读与测试暂停点语义须保） | 同上（族级计数实测） | **PARTIAL** |
| 7 | **TP w8 work_mtx_ 竞争**（M-21/22/29/64，worker 超配） | 8-worker 争用：11.16% acquisitions 竞争、3.2 futex syscalls/op（均值等待 42 µs）、内核 futex/锁机器 ≈7.1% cycles（qsl 2.65+futex_wake 1.40+__futex_hash 1.14+psi_group_change 1.04+__smp_call_single_queue 0.86）；w8 CPU/op 10.35 µs vs uring 1.71 µs 而 instr:u 仅差 5%（4034 vs 3843）→ 增量在等待不在指令 | shim 竞争率 + strace futex + 内核符号 + stat 三源 | **MEASURED** |
| 8 | **M-67 sticky 双标志每 pass 3 次互斥读**（`async_io_context.cpp:429-439`） | 每进度 pass 固定 3 次锁收购读两个永不复零标志；pass 级成本随深度摊薄（d4 19.5→d64 16.2 锁/op 的主要可摊薄项之一） | 深度摊薄曲线（shim）+ progress 族 1.25/op 簇；行级未分离 | **PARTIAL** |
| 9 | **M-74/M-01 就绪扫描族**（`request_scope.cpp:46-55` O(S) + wait_for/take 消费侧） | 就绪/reserve/consume 的 O(S) 扫描随 D 增长：wait_for 0.87→1.36→3.04%、take 0.62→1.08→2.34%、submit_read 1.32→1.68%（d4/d16/d64）；H3 INVESTIGATE 开放项 | 3 深度份额；含 bench 驱动 window.erase O(D) 污染（同符号 run_pipeline_phase 2.5-3.0% 需剔除） | **PARTIAL** |
| 10 | **M-42 submit_flushes_ + M-40 原子冗余**（候选 #11/#12） | 无读者 RMW 卫生类：submit_flushes_ 1 xadd/op（符号 0.01%）；live_cookies_ 原子冗余于 dispatch_mtx_（DAG 判低优先） | 符号份额 + 读者位置核实 | **MEASURED-small**（机工小，卫生价值） |

不入榜的最大采样项：`rep_movs_alternative` 19.2%（d16-btrfs）= 4KiB payload 拷贝本体、
`__memcmp_avx2_movbe` 3.2% = bench verify pass（复合口径污染，非请求路径）、
`run_pipeline_phase` 2.7% = bench 驱动循环——三者**可消除机工 ≈ 0**（数据搬运/测试口径/装置），
按"可消除机工"排序规则不入 Top 10。

## 6. NOT_OBSERVABLE / 未行使 / 口径限制清单

| 项 | 状态 | 原因 |
|---|---|---|
| M-63 uring owed sweep 逐槽锁成本 | **NOT_OBSERVABLE（本形态）** | W-02 无零长 op → owed_count_ 恒 0 → 循环休眠（§4-3）；需零长 op 负载或 idle 形态 A/B |
| M-70/71 事件单读 drain 路径 | **NOT_OBSERVABLE（本形态）** | 驱动线程在 inline 形态几乎不 park：evfd_reads 0-4 次/run、E2 唤醒样本 n≈32（统计弱）；tmpfs 仅 4 次 |
| 内核源码行级 annotate | NOT_OBSERVABLE | 无 kernel-debuginfo（#622 残余缺口）；内核归因仅到函数级 |
| publication→observation 逐请求时戳 | NOT_OBSERVABLE | 产品 API 形状（#622 交付缺口，`request.hpp` 无字段） |
| M-37 深队列 memmove 份额 | NOT_CONSTRUCTIBLE（本形态） | dispatch 队列长 ≈1（提交即清空）；候选前提不成立（§4-6） |
| 块层 per-op join | N/A（正向判据） | warm 全程 diskstats Δ=0（0 读 0 扇区）——无块层事件可 join，同时是 warm 判据的直接验证 |
| bench 归因 writeback | 无（机器背景） | E1 窗口 wb_dirty_folio ~3.5K/60s 为全机背景（空闲对照窗同样有）；bench 读-only + fixture 已 sync，无可归因 writeback |
| task_work 成本（inline 形态） | ≈0（测得） | twr 稳态 0 次（28 残差）；tmpfs punt 形态 489600 次、批形 {1:89.7%, 2-4:10.2%} |
| strace 时延列 | 永不计时 | +947%..+2032%（#622）；本轮 strace 仅计数 |
| B-set 事件（branches/branch-misses/LLC） | scaled | 66% enable（nmi_watchdog 占 1 GP）；scale 校正后 CV ≤5%，绝对值可信、口径注明 |
| P0-C 推荐格 tp-w4、64k、saturate、W-03、ODirect | 未列入 | regime 上限 5（重叠合并）；tp 形态对照由 w8 承担；P0-C 已有 w4 数字可引用 |
| M-80 write_all_at 重校验 | 未行使 | 直呼形态不在 W-02 读取工作负载；候选证据仍为 DAG 静态论证 |
| 深度↔路由扫描精确斜率 | INCONCLUSIVE | d16 每条目 7.9 cyc vs d4/d64 3.6-4 cyc/条，缓存效应未分辨（§4-2） |

## 7. 与 P0-C / #622 的交叉验证

1. **#622 H1 精确复现（7.2.11→7.3.3）**：enter/op = 1.000052（540028/540000，d4/d16/d64 一致）；
   enter 批形 = 每 enter 1 SQE（submit_req 与 enter 计数相等 + dispatch 即时 flush 结构）。
2. **#622 H2 精确复现**：tmpfs punts = 1.0000/op（540028/540028），btrfs warm = 5.2e-5（28 残差）；
   completion 计数精确对账；**FS 条件性成立**。同格用户态机工 FS 无关：instr:u 3843 vs 3862（+0.5%）。
3. **P0-C §4 复现**：ops/s（d4 600,914 vs 600,421；d16 584,580 vs 571,785；d64 537,942 vs 526,703；
   tp-w8 345,805 vs 342,228）、CPU/op（1.666/1.711/1.862/10.345 vs 1.67/1.75/1.90/10.66）、
   p50（5440/25953/119951/44077 vs 5427/26462/118567/44809）全部在 P0-C 噪声带内复现。
4. **P0-C instr/op 列分母差**：本报告原始 instructions:u 计数与 P0-C 原始日志逐字节差 0.04%
   （full 2,081,281,422 vs 2,082,072,982；null 同级），但 P0-C 表值 = Δ/420000（其 analyze.py
   `env_reps=max(rep)+1=7`，JSONL rep 字段在 measured 段重新从 0 计数）而其 perf 窗口为
   9 rounds（task-clock 995 ms ≈ 540000 ops × 1.81 µs 为证）→ P0-C 表值恰为本报告 ×9/7。
   本报告统一用 Δ/540000。**此为 P0-C 报告的口径注记发现，本文件不改写其正文。**
5. **G0 锁账对照**：uring contended=0 复现（G0 0.0000%）；G0 18.6 锁/op（v1-era 构建）
   vs 本轮 16.88（v2 master）——构建代差异，记录不裁决。
6. **#622 唤醒延迟对照**：iou-wrk（tmpfs）p50 [512,1K)ns、tp-w8 worker [1K,2K)ns——与
   #622 "p50 768-1536 ns、调度延迟 NOT material" 同带（本轮样本弱，n≈小，方向一致）。
7. **锁账验证结论**：DAG §3 的每请求锁账（access 1-2 + 后端 2-3 + core 7-9 + progress 1-2 +
   uring 锁内 1 enter / TP 1 syscall）在本形态全部成立；pass 级附加锁（owed 门、immediate、
   outstanding、health、progress snapshot/wait）另计 3-6 锁/op，实测总量 16.22-19.50 落带内。

## 8. 执行纪律存证

- governor：两轮 save-set/restore，读回均 20/20 match=yes（governor/ 三列 ×2）。
- 频率：负载中位 3,500,000 kHz（同 P0-C intel_cpufreq 3.25-3.5 GHz 带）；轮间空闲下探
  1.17 GHz 属预期（空闲间隙），instr 主口径不受影响；turbostat 3520 行全程伴随。
- 正确性门：每 regime 全部 bench 层（JSONL 1 + perf stat 24 + strace 1 + E1 1 + E2 1 + perf record 2 + shim 1 ≈ 30 次）逐层断言 `failing repetitions=0`，零失败。
- 不变量：未修改被跟踪文件；未碰 drop_caches；未改 nmi_watchdog；profiling 构建
  与基线吞吐分层报告（§0）。
- 装置偏差：两处（同 P0-C §1）/home=SATA 非 NVMe、compress=zstd:1 挂载（+C 排除读路径解压）。

## 9. 复现入口

```bash
# 构建
bash bench/build_profiles.sh release          # ring 构建（sha 03dbef09…）
# profiling 构建（attribution.sh 同配方）
gcc -c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -O3 -g -std=c++20 \
    -Ibench -Iinclude -DSLUICE_LIBURING_PROFILE=on -DSLUICE_HAS_LIBURING -DNDEBUG \
    -o /tmp/sluice-p1a/profiling-build/bench_w02_pipeline.o bench/bench_w02_pipeline.cpp
g++ -g -o /tmp/sluice-p1a/profiling-build/bench_w02_pipeline \
    /tmp/sluice-p1a/profiling-build/bench_w02_pipeline.o -m64 \
    -Lbuild-bench-ring/linux/x86_64/release -lsluice_async -luring -lsluice_core

# 单格全层（governor 包裹）
bash /tmp/sluice-p1a/governor.sh save-set
bash /tmp/sluice-p1a/cell.sh P1A-uring-4k-d16-btrfs W02-uring-4k-d16 /home/jnhu/p0c-scratch/bench-p1a
bash /tmp/sluice-p1a/redo_e.sh               # E1/E2 bpftrace 层（attach 等待修正版）
bash /tmp/sluice-p1a/governor.sh restore

# 分析
python3 -I /tmp/sluice-p1a/analyze_stats.py <label>...   # perf stat Δ/op + CV
perf report --stdio -i /tmp/sluice-p1a/raw/record/<label>-big.data --no-children -g none --sort symbol
python3 -I /tmp/sluice-p1a/ann_top.py /tmp/sluice-p1a/raw/report/ann-full-<label>.txt 1.5
```

原始数据：`/tmp/sluice-p1a/logs/`（perf×24 格/regime、strace、e1/e2、shim、irq/diskstats、
campaign-header）、`raw/bench/*.jsonl`（10 records/格）、`raw/record/*.data`（5 大样本 +
5 小样本）、`raw/report/*`（symbol/dso/annotate 全量）、`governor/`、`freq-samples.csv`、
`turbostat.log`、`stat-analysis.txt`。

## 10. 证据文件清单（/tmp/sluice-p1a/）

- `logs/campaign-header.txt`：git head/dirty、双 sha256、paranoid=2、nmi=1
- `governor/{original,set-verify,restore-verify}.txt` ×2 轮：governor 三列存证
- `logs/perfA|B|C1|C2-<label>-{full,null}{1,2,3}.txt`：perf stat -x, 原始输出（24 文件/regime）
- `logs/strace-<label>.txt` + `strace-null-{bench-p1a,tmpfs-bench}.txt`：syscall 计数
- `logs/e1-<label>.txt|.err`：submits/punts/comps/s2c 直方图/twr 批形/evfd/futex/writeback
- `logs/e2-<label>.txt|.err`：wakeup→sched_switch 直方图（bench/iou-wrk/tp worker 分列）
- `logs/shim-<label>.txt`：MUTEX_SHIM_ADDR 总量 + 逐地址 top24
- `logs/symbols-<label>.txt`、`raw/report/<label>-{symbol,dso}.txt`、
  `raw/report/ann-full-<label>.txt`、`logs/ann-top-<label>.txt`：perf 三层定位链
- `logs/irq-<label>-{before,after}.txt`、`logs/diskstats-<label>-A.txt`
- `freq-samples.csv`（3612×20）、`turbostat.log`（3520 行）
- 装置：`src/e1-uring.bt`、`src/e2-sched.bt`、`src/mutex_probe_addr.c`（G0 shim 逐地址变体，
  /tmp 一次性探针）、`shim/mutex_probe_addr.so`、`cell.sh`、`redo_e.sh`、`campaign.sh`、
  `governor.sh`、`analyze_stats.py`、`ann_top.py`
- 数据集：bench 自建 64 MiB fixture（btrfs /home/jnhu/p0c-scratch/bench-p1a +C；tmpfs
  /tmp/sluice-p1a/tmpfs-bench）
