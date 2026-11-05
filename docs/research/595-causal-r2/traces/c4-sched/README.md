# C4 调度器域深挖 — 观测路径与不可观测清单（2026-11-04，无 root）

任务：sched_wakeup/sched_switch/sched_waking（如可用）、perf sched、off-CPU 分析；
runnable delay / sleep duration / wakeup-to-run / ctxsw / executor 迁移 / 唤醒目标 /
抢占。定量结果在 `../../counters/C4_SCHED.md`；本文件记录观测路径选择、每条路径的
实测状态、以及仍不可观测的事件与它们阻止的结论。

## 0. 权限实测（本会话，全部执行）

`raw-c4/obs-01-sched-tracepoint-noroot.txt` 原始输出：

| 路径 | 命令 | 结果 |
|---|---|---|
| perf sched / tracepoint | `perf stat -e sched:sched_switch -a -- sleep 0.1`；`perf sched record` | 拒绝（paranoid=2，事件不存在于可开集合） |
| tracefs | `ls /sys/kernel/tracing/events/sched/` | Permission denied（root-only 挂载） |
| bpftrace | `bpftrace -l 'tracepoint:sched:*'` | `only supports running as the root user`（unprivileged_bpf_disabled=2） |
| sudo（非交互） | `sudo -n true` | 需要密码（rc=1）→ 与 C2/C3 会话记录一致，root 路径全部关闭 |
| 内核态 PMU | `perf stat -e cycles:k` | Error（同 Phase B） |
| **/proc per-task 调度记账** | `/proc/<tid>/schedstat`（on_cpu_ns/runqueue_wait_ns/timeslices）、`/proc/<tid>/sched`（nr_switches×3、se.nr_migrations）、`status`、`stat.processor` | **可用（同 uid 子进程），ns 精度，本轮主仪器** |
| PSI | `/proc/pressure/cpu` some total | 可读（全系统口径，辅助） |

结论：按任务给定的降级顺序（tracepoint → perf/ftrace → uprobe/fentry → 源码探针 →
反事实），本分支停在 **/proc per-task 内核记账**（无 root 下的调度域最强直接观测）；
uprobe/fentry 对内核 sched 符号同样 root-only；源码探针对调度器本身无意义（调度器
不在 Sluice 源内）；受控反事实属 G10（mutant 宏已存在，本分支未动）。

## 1. sched tracepoint 逐事件观测 — UNOBSERVED

`sched:sched_waking`/`sched:sched_wakeup`/`sched:sched_switch`/`sched:sched_stat_sleep`/
`sched:sched_stat_blocked`/`sched:sched_stat_iowait`/`sched:sched_migrate_task` 的逐事件
序列与时间戳在本会话不可获得（root-only，§0）。它们阻止的结论：

- **逐事件 wakeup→run 延迟分布**（每次唤醒的排队时长直方图、p99 尾部）：只有总量代理
  `runwait/timeslices` = 1,714→1,721 ns/段（合并 18 对，A→B 基本不变；
  `../../counters/C4_SCHED.md` §2）。无法回答"B 的 signal 写是否偶发制造长排队尾"。
- **唤醒目标身份**（sched_wakeup 的 target_cpu/任务）：以源码论证替代（§2），无逐事件证实。
- **抢占者身份**（sched_switch prev/next 对）：只有 nonvoluntary ctxsw 计数（+0.36%
  合并 n.s.；Phase B +1.72% 显著），无法指认谁抢占了 bench 线程。
- **park 原因分解**（sleep vs blocked vs iowait）：只有推导桶 lifetime−oncpu−runwait
  （DERIVED，`C4_SCHED.md` §6），"等待提交"与"等待 I/O"在本负载（tmpfs、submit-only
  enter、poll 立即返回）下推断为非关键路径，但逐 park 原因 UNOBSERVED。

## 2. 唤醒目标 — 源码级论证（直接读码，非 trace）

- B 臂新增通知链：`UringAsyncBackend::dispatch_after_accept_locked_()` 尾部
  `advertise_residue` → `signal_ready_progress()`（`src/async/uring_transport.cpp:115-120`）
  → `ProgressSource::signal()` → `wake_notification_nolock_()` → `::write(eventfd, 1)`
  （`include/sluice/async/detail/progress_source.hpp:472-518`，写点 :492）。
  写者是**正在执行 dispatch 的提交线程本身**（持 transport 锁、on-CPU）。
- eventfd 的唯一 poll 点是 `ProgressSource::park()`（progress_source.hpp:208-218），
  park 者是进度 owner；Phase B `progress_handoffs=0` ⇒ owner ≡ 同一提交线程。
- **⇒ 写时目标不可能在 eventfd waitqueue 上睡眠**：写者与唯一 park 者是同一线程的
  不同时刻。B 的 0.5589/op "wakeups"（Phase B 计数器）是用户态通知计数，不映射为
  内核 sched_wakeup 事件。此论证与计数证据闭环：C3 实测 poll 100% POLLIN 立即返回、
  零超时（两臂），drain 面 B−A read +1.12/drain = write/drain 1.000（counter 闭环）。
- 逐事件证实（trace 下看 write 时刻 waitqueue 是否真空）UNOBSERVED；但可观测后果
  （本分支直接测量）与"无唤醒目标"一致：timeslices +0.12% n.s.、sleep 推导 −0.39%
  n.s.、runwait +0.94%、migrations 0。

## 3. off-CPU 分析纪律

runnable delay（schedstat[1]）与 park 推导桶是 **wall 侧时间**，不计为 CPU 工作；
本分支未把任何 off-CPU 量累加进 CPU 归因（D2 的 +189 ns/op 全部在 on-CPU 内，由
C1/C3/C5 分解）。io-wq park 拉长（sleep 推导 +8.76%，lifetime 仅 +3.09%）登记为
**DERIVED 伴随量**：worker on-CPU/timeslices 不变，无 CPU 后果。

## 4. 时钟源与采样覆盖率

- schedstat/sched：内核 ns（TSC 基，constant_tsc）；status/stat 计数：事件计数；
  PSI：µs；bench CPU：CLOCK_PROCESS_CPUTIME_ID；wall：steady。跨仪器不对齐时间轴，
  A/B 比较限单仪器内（与 C2 `C2_PROBE_OVERHEAD.md` §4 同一纪律）。
- 采样覆盖率：1 ms 轮询，中位 3,608 样本/invocation（span 6.64 s）；iou-wrk kthread
  18/18 invocation 均捕获（threads=1）；读取错误 ≤1/invocation（进程退出竞态）。
- 探针开销：见 `C4_SCHED.md` §4——采样器对 bench 绝对值 +5~8% CPU / −6% 吞吐 /
  p99 +30%+，**绝对读数禁用于定量**；相对 A/B 差三块互证一致后保留。

## 5. 产物

- `../../counters/C4_SCHED.md`：定量主文档（四分类账、A/B 表、开销、限制）。
- `../../counters/raw/schedstat-d2/`、`raw/schedstat-off-d2/`、`raw/schedstat-d2-br/`：
  48 invocations 原始（results.jsonl 每 invocation 含全部线程末值、PSI 前后值、bench
  JSONL 路径；stdout/stderr 留存）。
- `../../counters/analysis/c4-schedstat.json`：分块 bootstrap 结果。
- `../raw-c4/obs-01-sched-tracepoint-noroot.txt`：权限探测原始输出。
