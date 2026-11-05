# C3 内核/io-wq 深挖：trace 与行为学观测（Sluice #595 causal-r2）

状态：ACTIVE EVIDENCE。轮次：causal-r2 C3（2026-11-04）。
范围：D2（`W02-uring-4k-d2`，4 KiB positional read，depth=2，warm tmpfs）A·IMMEDIATE vs
B·BOUNDED_PROGRESS。配套计数块：`evidence/counters/C3_COUNTERS.md`（sysio 差分块 +
C3 TSC 探针块的定量结果）。本文件记录 trace 类观测（strace、wchan、liburing 行为学
探针）与权限受限下的 UNOBSERVED 清单。

## 0. 环境与 provenance

| 项 | 值 |
|---|---|
| 主机/内核 | E5，Linux 7.2.11-200.fc44（详见 `env.txt`） |
| 权限 | `perf_event_paranoid=2`、`unprivileged_bpf_disabled=2`、tracefs root-only、`sudo -n` 需密码（无 root，与 C1/C2 会话一致） |
| bench 二进制（无仪器） | `counters/bin/bench_w02_pipeline_frozen`，sha256 `61c6d579…6886`（= phaseA 冻结基线二进制，`evidence/PROTOCOL.md` §Build） |
| bench 二进制（C3 探针） | `counters/bin/bench_w02_pipeline_c3`，sha256 `17463539…cae8`；构建：`SLUICE_595_C3_BUILD=1 xmake f -c --liburing=y -m release && xmake build bench_w02_pipeline` |
| 源码 | 研究副本 `causal-r2/worktree`（HEAD `271b81bf…`，C1 已有探针 + 本轮 C3 探针；C3 部分 diff 见 `probe-source-diff.txt`） |
| 运行形态 | `taskset -c 2`（cpu2），fixture warm tmpfs 复用；观测 run 与计数块 run 分离（见 `C3_COUNTERS.md`） |
| 时钟 | TSC 2.893 GHz（探针自校准 2,893,269–2,893,289 kHz，6 次极差 <25 kHz；与 C1/C2 校准一致）；strace 用 CLOCK_REALTIME µs；跨仪器不做时间轴对齐 |

## 1. 观测一：io_uring eventfd 注册（源码事实）

`src/async/uring_backend.cpp:272-287`：backend 安装进度端口时执行
`io_uring_register_eventfd(&ring, fd)`（`uring_backend.cpp:282/285`），把
ProgressSource 的 eventfd 同时注册为 ring 的完成信号 fd。因此该 fd 有**两个写者**：

1. 内核：每次 CQ tail 推进（每 enter 批次的 CQE commit）由内核 `eventfd_signal` 写
   （不经过 syscall 记账层，`/proc/<tid>/io` 的 syscw 不含它）；
2. 用户态（仅 B）：`ProgressSource::signal()` → `wake_notification_nolock_()` →
   `::write(fd, &one, 8)`（`include/sluice/async/detail/progress_source.hpp` 的
   `wake_notification_nolock_`，B 专属）。

读者只有一个：`drain_notification_nolock_()`（`::read` 循环到 EAGAIN），调用点两处——
park 前（`wait_if_unchanged` 内，`progress_source.hpp` 的 drain 调用行）与
`acknowledge_notification()`。C3 探针计数证实稳态 `drain_ack=0`（两臂），drain 全部
走 park 路径（`C3_COUNTERS.md` §2）。

**含义**：B 的新增内核路径不只有用户态 write；write 落在同一个 fd 上，与内核完成
信号共享 counter/wakeq 状态。`read_value_sum`（drain 读到的 counter 值总和）A 4.034
→ B 5.154 每次 drain，差 +1.12 ≈ B 的 write/drain 比 1.000，闭环对账（探针块）。

## 2. 观测二：io-wq 线程画像（行为学，非 timing run）

数据源：`iowq_samples.txt`（wchan/状态采样）、`strace-c-sum-{A,B}.txt`（`strace -f -c`，
MEASURED=2 短 run，路径发现用途）、Phase B split 块的 /proc 采样（见 `C3_COUNTERS.md` §4 引用）。

1. **线程数**：每个 bench invocation 恰 1 个 `iou-wrk-*` 线程（split-d2 12/12、
   本轮 sysio 块 12/12 invocations，`threads:1`）。
2. **park 点**：wchan 采样多次命中 `io_wq_worker`（io-wq worker 的内核 wait queue
   睡眠点），并采到 R 状态样本（`iowq_samples.txt`）。它是内核线程，无用户态地址空间。
3. **syscall 面**：`strace -f -c` 全程（两臂同跑）：`futex=2`（进程级固定开销）、
   `read=18`、`write=1/10`（strace 掩蔽下的塌缩值，非真实密度——C2 已证掩蔽）、
   `clone3=1`（创建该 worker）。**内核记账级证据**（无 strace、无掩蔽）：
   `/proc/<pid>/task/<tid>/io` 的 syscr/syscw 在 iou-wrk 线程上 **A/B 两臂 12/12
   invocations 恒为 0**（`C3_COUNTERS.md` §3）——io-wq 从不做 read/write syscall。
4. **唤醒率**：voluntary ctxsw D2 A 0.5521 / B 0.5630 per op（Phase B split）；
   D1（depth=1，`sqes_deferred=0`、`wakeups=0`）A 0.7175 / B 0.7199 per op
   （split-d1x 重算）——**io-wq 唤醒率与 arm 无关**，与 depth/等待节奏相关。
5. **内核时间**：D2 1,771.8→1,779.8 ns/op（A/B，Phase B split）；D1 2,068→2,102
   ns/op（split-d1x 重算）。两臂等量。

**io-wq work 内容（它在执行什么）**：UNOBSERVED。阻止的观测：`io_uring:io_uring_queue_async_work`
/`io_uring_submit_sqe` 等 17 个 tracepoint 本内核编译在内（CAPABILITY_MATRIX §2，
BTF 实证）但 attach 需 root；`perf :k` callgraph、ftrace function_graph、kprobe 同因
权限不可用。无 root 下 io-wq（内核线程）的函数级归因无可用路径。

**部分排除的假说**：裸 liburing 行为学探针（`probe_read_ctx.c`，3000 次 submit+
立即 peek，区分 inline/异步完成）显示 warm page cache 的 4 KiB buffered read 在
submit 返回时 CQE 已就绪：tmpfs 99.60–99.70%（两次主 run；归档时一次重跑 90.87%，
展示 run 间波动）、ext4 warm 100.00%/99.67%；>4 次 yield 仍无 CQE（giveup，真正
离开提交上下文完成的上界）0.10–0.37%。**"buffered read 被强制走 io-wq"假说不成立**
（若成立 giveup 应接近 100%）。因此 io-wq 的 1.78 µs/op 内核时间的具体内容保持
UNOBSERVED；其 A/B 等量性（§3、Phase B）不受影响。

## 3. 观测三：strace 下的两臂 syscall 面（路径发现，非定量）

`strace -f -c`（MEASURED=2，frozen 二进制）两臂关键计数（`strace-c-sum-{A,B}.txt`）：

| syscall | A | B |
|---|---|---|
| io_uring_enter | 180,010 | 180,010 |
| read | 18 | 18 |
| write | 1 | 10 |
| futex | 2 | 2 |
| clone3 | 1 | 1 |

两臂 enter 总数逐次相等（全程 3.0002/op = 各 pass 类型各 1 enter/op；measured 窗口
1.0000/op 见 Phase B/C2）。strace 掩蔽（无 drain、write 塌缩，C2 已证）使 read/write
密度不可用；本表只用于确认：进程级无其他 syscall 差异面（无 epoll/futex 流、无
mmap/brk 差异），与 sysio 块（无仪器）结论一致。

## 4. 本目录文件

| 文件 | 内容 |
|---|---|
| `README.md` | 本文件 |
| `env.txt` | 内核/工具版本、paranoid/bpf_disabled、二进制 sha256 |
| `probe-source-diff.txt` | 研究副本 HEAD、C3 探针 diff（progress_source.hpp 部分） |
| `strace-c-sum-A.txt` / `strace-c-sum-B.txt` | `strace -f -c` 汇总（MEASURED=2，观测 run） |
| `iowq_samples.txt` | iou-wrk 线程 wchan/state/stime 采样（47 样本） |
| `probe_read_ctx.c` | liburing inline/异步完成行为学探针源码 |
| `probe-inline-tmpfs.txt` / `probe-inline-ext4.txt` | 探针输出（tmpfs 4 run、ext4 2 run，含波动） |

## 5. UNOBSERVED / OBSERVABILITY_LIMIT_REACHED（C3 域）

| 缺口 | 状态 | 阻止的结论 |
|---|---|---|
| `cycles:k`/`instructions:k`、内核态 cache/branch | OBSERVABILITY_LIMIT_REACHED（paranoid=2、无 root；C2 obs-02 实证） | write/read 的内核指令级拆分；内核 cycles 精确对账 |
| io_uring tracepoints（queue_async_work/complete/task_add 等 17 个） | 存在（BTF 实证）但 attach 需 root → UNOBSERVED | io-wq work 内容；CQE 完成 ctx 的事件级确认（用行为学探针+源码替代） |
| ftrace function_graph / kprobe(eventfd_write 等) | root-only → UNOBSERVED | eventfd write 内部阶段（fd 检查/计数/wake_up）拆分 |
| workqueue:sched_kthread_work / workqueue:* | root-only → UNOBSERVED | io-wq 与 workqueue 子系统的关系事件 |
| sched_wakeup/sched_switch 时间戳 | root-only → UNOBSERVED（C4 域，G2） | wakeup→run 延迟 |
| lock:contention（LOCKDEP=n） | tracepoint 不存在（CAPABILITY_MATRIX §4） | 内核锁竞争（本负载两臂 futex=2、io-wq 无 syscall，竞争面无证据） |
| io-wq work 内容 | UNOBSERVED（见 §2） | "io-wq 的 1.78 µs/op 具体做什么"（不影响 A/B 等量结论） |
| syscall 时长的 `:k` 口径 | 用源码 TSC on-CPU 往返替代（write 574/562.7、read_ok 552/539、read_eagain 497/486 ns/call，`C3_COUNTERS.md` §2） | 无法区分特权级切换时间在 utime/stime 的精确归属 |
