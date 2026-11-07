# E5 观测能力矩阵（Sluice #595 causal-r2）

状态: ACTIVE EVIDENCE（本文件为 E5 主机实测探测结果，全部条目在本会话实际执行验证，未假设任何能力存在）
任务引用: causal-r2 任务 §2 清单（perf stat/record/sched/lock、bpftrace/BTF、ftrace/tracefs、
io_uring/sched/workqueue/block/syscalls tracepoints、uprobe/USDT、PMU 事件、时钟/频率、PSI、/proc 线程记账）

## 0. 探测元信息（实验记录纪律）

| 项 | 值 | 证据 |
|---|---|---|
| 主机 | E5 | `uname -a` |
| CPU | Intel Xeon E5-2666 v3 @ 2.90GHz（Haswell-EP, family 6 model 63 stepping 2），10C/20T，单 NUMA，CPU 0–19，flags 含 arch_perfmon pebs bts constant_tsc nonstop_tsc | `lscpu` |
| 内核 | Linux 7.2.11-200.fc44 #1 SMP PREEMPT_DYNAMIC Thu Oct 15 2026，x86_64；HZ=1000；# CONFIG_PREEMPT is not set + CONFIG_PREEMPT_DYNAMIC=y | `uname -a`, `/boot/config-7.2.11-200.fc44.x86_64` |
| 探测日期 | 2026-11-04 | 会话时间 |
| 探测轮次 | 两轮独立探测，本文件为合并版：S1（15:36–15:50，raw/ 存档）+ S2（15:34–16:05 复核轮，关键声明全部复算，修正 §1 计数器数量错误声明，新增 io_uring 端到端触发/fdinfo/PEBS/turbostat/trace_clock 实测） | 本文件各"实测"标注 |
| 工具版本 | perf 7.2.11-200.fc44.x86_64；bpftrace v0.24.2；trace-cmd 3.3.1；bpftool v7.6.0 (libbpf v1.6)；turbostat 存在；stap/strace/ltrace/readelf/objdump 存在；gcc + clang-22 + xmake v3.1.2；**liburing 2（头文件 + liburing.so.2，io_uring 负载探针可直接编译）** | `--version`、`command -v`、`ldconfig -p \| grep uring` |
| 权限 | tracefs `/sys/kernel/tracing` 为 root-only（`drwx------`，mount 显示 tracefs 已挂载 rw）；perf_event_paranoid=2；**kernel.unprivileged_bpf_disabled=2（bpftrace 无 root 完全不可用，`bpftrace -l` 仅返回 1 行）**；kptr_restrict=0；yama.ptrace_scope=0；nmi_watchdog=1；sudo 可用（仅观测用途） | `stat`、`mount`、`cat /proc/sys/kernel/{perf_event_paranoid,kptr_restrict,unprivileged_bpf_disabled,nmi_watchdog}`、`sysctl kernel.yama.ptrace_scope` |
| 原始输出 | 本目录 `raw/`（S1: perf-list-full.txt、bpftrace-lv-io_uring.txt、bpftrace-lv-sched-wq-block.txt、perf-stat-multiplex.txt、perf-record-test.txt、perf-sched-test.txt、perf-lock-test.txt、perf-lock-contention-test.txt、bpftrace-futex-test.txt、tracefs-instance-test.txt、trace-cmd-test.txt 等；S2: s2-iouring-e2e-trigger.txt、s2-iouring-create-fields.txt、s2-iouring-fdinfo.txt、s2-pmu-generic-counters.txt、s2-perf-trace-read.txt、s2-perf-mem.txt、s2-turbostat-short.txt、s2-trace-clock.txt、s2-overhead-selftest.txt、s2-proc-latency.txt、s2-sysctls.txt、s2-cpufreq-info.txt） | 本目录 |

## 1. 总矩阵

| # | 工具/机制 | 事件/能力 | 关键字段 | 权限 | 开销量级（未实测，见 §7） | 实测状态 | 限制 |
|---|---|---|---|---|---|---|---|
| 1 | perf stat | PMU 硬件计数（cycles/instructions/branches/cache/L1D/dTLB 等 7000+ 事件, `perf list` S1: 7285 行 / S2: 7093 行，均为实测，口径差异为版本过滤） | 计数值 + (复用百分比) | 非 root 自动降级 `:u`（S1: cycles:u 229,130；S2: 217,090 cycles:u + 147,700 instr:u）；内核态计数需 root（paranoid=2；S2 实测 cycles:k=1,515,277、cs、msr/tsc/=2,687,341） | 每事件 ~µs 级；事件组复用见 §2 | 实测通过 | **Haswell-EP 每逻辑核 4 generic + 3 fixed（S2 复核修正 S1 的"2 generic"误写）：实测 3 与 4 个 generic 事件并发均 100% runtime 无复用，5 个并发即复用（~79–81%）；nmi_watchdog=1 的 hardlockup detector 占用 fixed counter（内核 5.x+ 行为），不挤占 generic**；>4 generic 事件必复用 |
| 2 | PMU 复用 | 8 事件并行时实测每事件 (37.49%–62.50%) 时间被启用；S2: 6 事件 (63.44%–84.75%) | `(xx.xx%)` 后缀 | root（内核态） | 复用引入计数噪声，比例越大误差越大 | 实测通过（perf-stat-multiplex.txt：3.5s busy loop 8 事件；S2: dd 4k×20k 6 事件） | Haswell Topdown v1 需 4 组计数；结果须携带复用比例引用 |
| 3 | perf record + report | cycles 采样（含 callgraph、符号解析）；**S2: `perf record -g` 用户+内核调用链完整（do_syscall_64/vfs_write 等内核符号解析）；`perf record -k 1:0`（CLOCK_MONOTONIC clockid）实测通过，可与 ftrace/bpftrace 时间对齐** | 样本、符号、调用栈 | root（内核采样；非 root 仅用户态） | -F 999 ~1kHz 默认；max_sample_rate=79000 | 实测通过（S1: 1014 samples, Lost 0, 符号解析正常；S2: -g 调用链含内核符号） | Haswell 支持 PEBS（flags 有 pebs）；bts 存在 |
| 4 | perf sched record/latency | sched_switch/sched_wakeup/sched_wakeup_new/sched_migrate_task/sched_stat_runtime/sched_stat_iowait/sched_stat_blocked/sched_stat_sleep/sched_stat_wait | 每任务 Runtime/Count/Avg delay/Max delay/时间戳 | root | sched 全事件 ~每 CPU 每切换 1 条记录；实测 0.4s→1078 样本 | 实测通过（perf-sched-test.txt） | 调度延迟 = wakeup→run 路径；无 per-task vruntime in events（/proc/PID/sched 有） |
| 5 | perf lock | **仅** `lock:contention_begin`/`lock:contention_end`（header 实证）；`lock:lock_acquire/lock_release` **不存在**（LOCKDEP=n） | contended/total wait/max wait/type/caller | root | 与 tracepoint 采样同量级 | 实测通过但数据为空的陷阱（见 OBSERVABILITY_LIMITS §1.1；S2 复测 rc=0、report 表头正常、表体空，同结论） | 只覆盖内核 mutex/rwsem/spinlock 慢路径；futex(用户态锁)完全不可见 |
| 6 | perf trace | syscall/tracepoint 打印 | - | root | ~每事件一次格式化 | **S2 实测通过**：`perf trace -e syscalls:sys_enter_read -- dd ...` 逐调用打印 fd/buf/count 参数 | 仅适合低频快照，不适合 bench 期间常驻 |
| 7 | bpftrace tracepoint | tracepoint:io_uring:*（17 个）、tracepoint:syscalls:*io_uring*（6 个）、tracepoint:sched:*（29）、workqueue（4）、block（25）、lock（3）、irq:softirq_*（3） | 见 §3 字段表 | root（tracefs root-only） | 每 CPU 每事件 1 次 BPF 程序执行；实测 1s 1143 sched_switch 正常归零丢弃 | 实测通过 | **必须用 `tracepoint:` 前缀**；裸 `io_uring:*`/`syscalls:*` 报 "Invalid probe type"（v0.24 行为，旧一行脚本不兼容） |
| 8 | bpftrace kfunc/fentry | kfunc:*io_uring* 可列（含 `__x64_sys_io_uring_enter`、`__probestub_*`）；attach 实测通过 | BTF 原型参数 | root | fentry ~接近 tracepoint 开销 | 实测通过（`kfunc:vmlinux:__io_uring_add_tctx_node_from_submit` Attached） | 裸 `kfunc:io_uring_enter` 失败 "No BTF found"（该名字不在可附加集合）；需用列表确认的名字 |
| 9 | bpftrace kprobe/kretprobe | kprobe:*io_uring_enter* → `__x64_sys_io_uring_enter`、`__ia32_sys_io_uring_enter`；任意 kallsyms 符号 | pt_regs | root | kprobe 单点 ~百 ns 级；高频热路径慎用 | 列表实测通过 | kallsyms 可用（KALLSYMS_ALL=y）；__probestub_* 名可作 tracepoint 触发点 |
| 10 | bpftrace rawtracepoint | rawtracepoint:vmlinux:sched_* 等可列 | 原始 args | root | 低于 tracepoint（少一份格式化） | 列表实测通过 | 字段需按 args 索引手工解析 |
| 11 | bpftrace uprobe/uretprobe | 任意二进制符号；`uprobe:/usr/lib64/libc.so.6:*open*` 可列；Sluice bench 二进制（未 strip + debug_info）符号可挂 | arg0..N | root | uprobe ~µs 级（softint trap + 单步）；比 tracepoint 贵一个量级 | 实测通过（S1: /bin/echo 31 次 malloc 捕获；S2: `uprobe:libc:malloc` 1s 窗口 6081 次） | 不支持内联/静态符号 unless 通过符号表偏移 |
| 12 | bpftrace usdt | `usdt:/usr/lib64/libc.so.6:*` 列出 libc 探针（cond_* 等） | 按 note | root | ~与 uprobe 同（+静态使能检查便宜） | 语法实测通过 | **Sluice 源码无任何 stapsdt/SDT 探针**（grep exit 1）；bench 二进制无 USDT note（readelf -n 无 NT_STAPSDT）→ 加 USDT 须改研究副本源码 + 重编 |
| 13 | bpftrace profile/interval | profile:hz:N 定时采样；interval 定时聚合 | - | root | 采样频率决定 | 语法实测通过 | — |
| 14 | ftrace/tracefs | available_tracers: `timerlat osnoise hwlat blk mmiotrace function_graph wakeup_dl wakeup_rt wakeup function nop`；kprobe_events/uprobe_events/synthetic_events/dynamic_events 文件存在 | format 文件 | root（目录 drwx------） | function graph 较贵；单 event 使能便宜（每事件一条缓冲记录） | 实测通过（隔离 instance 使能 sched_switch 0.4s → 337 events, 0 drop, 已清理复原） | 必须用隔离 instance（`instances/<name>`）避免污染全局状态 |
| 15 | trace-cmd 3.3.1 | record/report/list；20 CPU 多事件录制 | report 格式化输出 | root | 同 ftrace | 实测通过（sched:sched_switch + io_uring:io_uring_submit_req 双事件 0.4s 录制） | — |
| 16 | BTF | /sys/kernel/btf/vmlinux（6,983,958 字节）；CONFIG_DEBUG_INFO_BTF=y, DEBUG_INFO_BTF_MODULES=y | 内核结构定义 | 读无需特权 | 0（编译期） | 存在性实测通过 | bpftrace 结构体访问依赖它 |
| 17 | PMU Topdown v1 | `-M TopdownL1`（435 行 metric 事件在 perf list） | frontend-bound/bad-speculation/… | root | 需多组复用 | 机制实测存在；1ms 短进程 `<not counted>` | 短 run 必须加长窗口或用 `-I`；nmi_watchdog=1 挤占计数器，关闭属全局改动需另行决策 |
| 18 | RAPL 能耗 | `power/energy-pkg/`（Kernel PMU event）；sysfs /sys/class/powercap/intel-rapl 存在；rapl/intel_rapl_msr 模块已加载 | 能量 µJ（域累积） | root | 读取便宜；域计数器更新粒度粗（~1ms 级） | 存在性实测通过 | per-package 粒度，不能归因到单线程；仅可作总量对照 |
| 19 | uncore | 1119 个 uncore 事件（intel_uncore 模块加载） | - | root | 各计数器独立，便宜 | 存在性实测（perf list 计数） | IMC/PCU 等域级数据，适合带宽/命中率总量对照 |
| 20 | turbostat | 每核 Bzy_MHz（APERF/MPERF 实测平均频率）、PkgTmp；**S2 实测全列输出：Avg_MHz/Busy%/Bzy_MHz/TSC_MHz(TSC≈2897MHz)/IPC/C1%–C7%/CoreTmp/PkgTmp/Pkg%pc6/PkgWatt(实测 34.97W)/RAMWatt** | Bzy_MHz/Core/PkgTmp | root | 1s 间隔轮询，几乎 0 开销；S2 用 `--interval 0.2 --num_iterations 2 sleep 0.6` 短窗验证 | 实测通过 | 因果实验的频率权威源（scaling_cur_freq 只是调度器请求值）；该版本不支持 `--output` 选项（实测报 unrecognized option），输出走 stdout 重定向 |
| 21 | cpufreq sysfs | /sys/devices/system/cpu/cpu*/cpufreq/{scaling_cur_freq,scaling_driver,scaling_governor}；driver=intel_cpufreq（passive 模式），governor=schedutil | kHz 整数 | 读无需特权 | 0 | 实测通过（0–19 核可读；实测范围 1.2–3.5GHz） | scaling_cur_freq ≠ 实测频率；归因频率漂移用 turbostat |
| 22 | 时钟/TSC | clocksource=tsc；constant_tsc + nonstop_tsc flags；NO_HZ_FULL=y, HIGH_RES_TIMERS=y, HZ=1000；**S2: tracefs trace_clock 可选 `[local] global counter uptime perf mono mono_raw boot tai x86-tsc`（实测）；perf record `-k 1:0`（CLOCK_MONOTONIC）实测通过；bpftrace 内建 `nsecs`（ktime）实测可用；`power:pstate_sample`/`power:cpu_frequency`/`power:cpu_idle`/`power:cpu_idle_miss` tracepoints 均存在** | - | 读无需特权；trace_clock/perf -k 需 root | 0 | 实测通过 | TSC 稳定，多核时间戳可比（constant/nonstop）；跨工具时间戳对齐规范见 OBSERVABILITY_LIMITS §2.9 |
| 23 | PSI | /proc/pressure/{cpu,memory,io,irq}，some/full avg10/avg60/avg300/total | avg10/avg60/avg300/total | 读无需特权 | 0（内核压力跟踪持续运行） | 实测通过（读数正常，io 域有历史压力记录） | PSI total = 压力总时长 µs；粒度 10s/60s/300s 窗口，不适合作细粒度归因，适合实验前后对照 |
| 24 | /proc 线程记账 | /proc/PID/stat（utime/stime 实测）、/proc/PID/schedstat（on-cpu ns/runqueue-wait ns/timeslices 实测：1677278 0 1）、/proc/PID/status（voluntary/nonvoluntary ctxt 实测）、/proc/PID/wchan（实测 hrtimer_nanosleep）、/proc/PID/sched（se.exec_start/se.vruntime/se.sum_exec_runtime 实测；S2 复核 se.* 输出正常）、/proc/PID/task/TID/io（rchar/wchar/syscr/syscw 实测）、statm、**S2: /proc/PID/latency（CONFIG_LATENCYTOP=y，文件存在性已实测确认，原 UNVERIFIED 项关闭）** | 见左 | 读无需特权 | 0 | 实测通过（后台 sleep 进程验证全部字段） | **/proc/PID/delay 不存在**（见 OBSERVABILITY_LIMITS §1.3；S2 复测仍不存在）；SCHEDSTATS=y、SCHED_INFO=y |
| 25 | perf mem（PEBS） | `cpu/mem-loads,ldlat=30/P` 采样 | load latency/地址采样 | root | 采样式 | **S2 实测通过**：`perf mem record -- dd ...` 12 samples, Lost 0；另一短窗测试 68 samples, Lost 5.88%（raw/s2-perf-mem.txt） | 仅 precise_ip 支持的 load/store 事件；短窗口有样本丢失（`perf mem report` 会显示 Lost %，须核查）；样本含侧信道噪声，勿用于因果 timing |
| 26 | sched/dev PMU 功率 | `power/energy-pkg/` 已在 #18；uncore 频率/功率事件（unc_p_freq_band*、unc_m_power_*）存在（perf list S2 实测） | 域级累积 | root | 便宜 | 存在性实测 | 包/域级，仅总量对照 |

## 2. IO_URING 观测专项（#595 直接相关）

全部 17 个 io_uring tracepoint 实际存在于本内核（`bpftrace -l 'tracepoint:io_uring:*'` 实测输出 = tracefs available_events 一致）：

```
io_uring_local_work_run  io_uring_short_write   io_uring_task_work_run
io_uring_cqe_overflow    io_uring_req_failed    io_uring_task_add
io_uring_poll_arm        io_uring_submit_req    io_uring_complete
io_uring_fail_link       io_uring_cqring_wait   io_uring_link
io_uring_defer           io_uring_queue_async_work  io_uring_file_get
io_uring_register        io_uring_create
```

关键字段（`bpftrace -lv` 实测，全量见 raw/bpftrace-lv-io_uring.txt）：

| 事件 | 字段 | 归因用途 |
|---|---|---|
| io_uring_submit_req | ctx, req, user_data, u8 opcode, flags, bool sq_thread, __data_loc op_str | 提交点；user_data 可关联应用请求 |
| io_uring_complete | ctx, req, user_data, int res, unsigned cflags, u64 extra1, extra2 | 完成点；res/cflags 判定成败与 whether inline |
| io_uring_task_work_run | **void *tctx**, unsigned int count | task work 批量执行（注意字段名 tctx 不是 ctx） |
| io_uring_local_work_run | ctx, int count, unsigned int loops | SQPOLL/本地 work 执行 |
| io_uring_cqring_wait | ctx, int min_events | 阻塞等待入口（等待时长需配对 sched 延迟或 kprobe/return） |
| io_uring_task_add | ctx, req, user_data, u8 opcode, int mask, op_str | 转入 task work（多轮延迟的起点） |
| sys_enter_io_uring_enter | unsigned int fd, u32 to_submit, min_complete, flags, const void *argp, size_t argsz | syscall 入参 |
| sys_exit_io_uring_enter | long ret | 返回值（实际提交/完成数） |

补充：`CONFIG_IO_URING_BPF=y`（io_uring SQE BPF 过滤能力在内核中编译但属内核功能，非观测手段）。内核 io_uring 源码级探针点可用 fentry 挂 `__probestub_io_uring_*`（所有 tracepoint 的 stub 均可列出）。

### 2.1 io_uring tracepoint 端到端触发验证（S2，真实负载）

用 liburing 编译一次性探针 `/tmp/probe_iouring.c`（setup(4)→submit nop(user_data=0x595)→wait_cqe→exit），bpftrace 监听下运行，实测捕获（完整输出含其他进程 opcode=29 的 ring 流量）：

```
submit_req ctx=0xffff8c99172b7000 req=0xffff8c988c41c400 user_data=1429 opcode=0
complete   ctx=0xffff8c99172b7000 user_data=1429 res=0 cflags=0
```

结论：`io_uring_submit_req → io_uring_complete` 经 `(ctx, user_data)` 二元组配对成立；`ctx` 指针区分多 ring 实例；`io_uring_create` 事件携带 `fd, ctx, sq_entries, cq_entries, flags`（S2 实测字段），可在 setup 时刻建立 ctx↔fd 映射供后续归因（fdinfo 侧无 ctx 指针，见 LIMITS §2.10）。

### 2.2 io_uring fdinfo（无探针的 ring 状态视图，S2 实测）

存活 ring 的 `/proc/<pid>/fdinfo/<fd>`（fd 目标 `anon_inode:[io_uring]`，读他人进程需 root）实测全字段：
`SqMask/SqHead/SqTail/CachedSqHead/CqMask/CqHead/CqTail/CachedCqTail/SQEs/CQEs/SqThread/SqThreadCpu/SqTotalTime/SqWorkTime/UserFiles/UserBufs/PollList/CqOverflowList/NAPI`。
用途：SQ/CQ 头尾差分给出积压；`SqThreadCpu` 给出 SQPOLL 线程 CPU 归属；`SqTotalTime/SqWorkTime` 给 SQPOLL 时间片累计（µs）。适合 run 前后对账，不适合逐请求归因。

## 3. sched / workqueue / block 字段（实测）

| 事件 | 字段 |
|---|---|
| sched:sched_switch | prev_comm[16], prev_pid, prev_prio, prev_state, next_comm[16], next_pid, next_prio |
| sched:sched_wakeup / sched_waking | comm[16], pid, prio, target_cpu |
| sched:sched_stat_runtime | comm, pid, u64 runtime |
| sched:sched_stat_iowait | comm, pid, u64 delay |
| sched:sched_stat_blocked / sleep / wait | comm, pid, u64 delay |
| sched:sched_migrate_task | comm, pid, prio, orig_cpu, dest_cpu |
| sched:sched_process_exit | comm, pid, prio, bool group_dead |
| sched:sched_kthread_work_execute_start/end | work, function |
| workqueue:workqueue_queue_work | work, function, __data_loc workqueue, int req_cpu, int cpu |
| workqueue:workqueue_activate_work / execute_start / execute_end | work, function |
| block:block_rq_issue | dev, sector, nr_sector, bytes, ioprio, rwbs[10], comm[16], __data_loc cmd |
| block:block_rq_complete | dev, sector, nr_sector, int error, ioprio, rwbs[10], __data_loc cmd |

## 4. lock / futex 可观测（实测差异）

- 存在：`lock:contention_begin`、`lock:contention_end`、`lock:contended_release`（3 个）
- 不存在：`lock:lock_acquire` / `lock:lock_release` / `lock:lock_contended` / `lock:lock_acquired`（CONFIG_LOCKDEP=n、CONFIG_LOCK_STAT=n、CONFIG_DEBUG_LOCK_ALLOC=n，/boot config 实证 12049/12055 行）
- 不存在子系统：`futex:`、`mutex:`、`rwsem:`、`rtmutex:`（available_events grep 为空）
- futex 替代路径（实测）：`syscalls:sys_enter_futex` + `sys_enter_futex_wait/wake/requeue/waitv`（均存在；bpftrace 计数实测 2s 窗口 1397 次 ops 在 4 线程 Python 锁竞争中）
- 用户态锁等待时长重建：sys_enter_futex→sys_exit_futex 配对，或 sched:sched_switch prev_state/prev_pid 重建

## 5. ftrace tracer 与高级事件机制

- available_tracers（实测）：timerlat osnoise hwlat blk mmiotrace function_graph wakeup_dl wakeup_rt wakeup function nop
- 动态事件：kprobe_events、uprobe_events、synthetic_events、dynamic_events 文件存在（CONFIG_KPROBE_EVENTS=y、UPROBE_EVENTS=y）→ 任意内核/用户符号探针 + 合成事件可用于跨事件配对
- **S2 实测 kprobe_events 写入**：`echo 'p:mytest io_uring_enter' >> /sys/kernel/tracing/kprobe_events` 成功（符号存在）+ `-:mytest` 清理成功；ftrace 侧动态 kprobe 可用
- CONFIG_LATENCYTOP=y（/proc/PID/latency 文件 S2 实测存在，原 UNVERIFIED 项关闭）
- tracefs 隔离实例可用（S1 与 S2 均实测 `mkdir instances/<name>`；S2 建/删 captest 成功）
- buffer 默认 per-CPU 1410 KB（S2 读数）；`available_filter_functions` 96,819 个（S2）；全局 `tracing_on=1`、`current_tracer=nop` 未改动

## 6. 权限要求汇总

| 操作 | 需要的权限 | 依据 |
|---|---|---|
| 读 /proc, PSI, cpufreq sysfs, /sys/kernel/btf/vmlinux | 无 | 实测 |
| perf stat 用户态计数（:u） | 无（自动降级） | 实测 paranoid=2 → cycles:u |
| perf 内核计数/tracepoint/record/sched/lock | root | paranoid=2 + 实测 |
| bpftrace 一切 tracepoint/kprobe/uprobe/usdt（含 `-l` 列举） | root | **unprivileged_bpf_disabled=2（S2 实测读数）+ tracefs drwx------；无 root 时 `bpftrace -l` 仅返回 1 行不可用** |
| ftrace 使能/读 buffer | root | 实测（sudo 下用隔离 instance） |
| turbostat | root | 实测 |
| 关闭 nmi_watchdog / 调 paranoid | 全局系统改动——本研究默认**不做**，若实验阶段必需须单独决策记录 | 观察纪律 |

## 7. 开销量级声明（重要）

本探测**未运行 benchmark**（任务约束：轻量探测）。§1"开销量级"列全部为**文献/机制级估计，标注为未实测**，
仅用于实验设计阶段的采样密度与工具选择参考；正式因果 run 若需精确开销（如 bpftrace 每事件 ns 成本），
必须以"开/关观测"的对照实验在 bench 阶段独立测量，且不同类型 instrumentation 不叠加进同一次 timing run（任务纪律）。
本会话实测到的只有计数与行为验证（如 1s 1143 sched_switch、0.4s 337 events、31 malloc），不构成开销测量。

S2 附加的两组轻量自测（仍非 benchmark，仅界定探测自身干扰的量级）：

1. **空闲态 tracepoint 观测的 wall-clock 影响**：bash busy-loop 3,000,000 次迭代，基线 4.61s；同机挂着 bpftrace（sched_switch + sys_enter_read 双 tracepoint，事件率 ~1200/s）时 4.65s（+0.9%，噪声级）。结论：**空闲/低事件率下观测对 wall time 不可测**；此结果不能外推到高事件率（bench 负载）场景。
2. **bpftrace 进程自身 CPU**：3 探针（sched_switch、sys_enter_read、io_uring_submit_req）2s 窗口，`/usr/bin/time` 实测 U+S = 0.16s（~8% 单核，主要含启动时 BPF 编译/attach 成本；运行态更低）。结论：bpftrace 会话的启动成本不可忽略，bench timing run 必须在 attach 完成后再开始计时。

探测产物/状态变更清单（S2，均已复原或留于 /tmp 一次性位置）：kprobe_events `mytest` 已写入并清理；tracefs `instances/captest` 已建并删除；/tmp 下 probe_iouring.c、probe_iouring、bt*.out、perf 数据文件为一次性探针产物。全局 ftrace（current_tracer=nop、tracing_on=1）未改动。

## 8. C1 补充探测（2026-11-04，无 root）

本节为 C1 深挖会话的实测补充；条件与 S1/S2 相同（`perf_event_paranoid=2`、
`unprivileged_bpf_disabled=2`、tracefs root-only、无 sudo）。

| 能力 | 实测结果 | 命令/证据 |
|---|---|---|
| perf 用户态 callgraph（**无 root**） | **可用**：`perf record -e cycles:u -F 8000 -g --user-callchains -k 1` 在 bench 负载上 6/6 run 成功，Lost=0，样本 17,891–18,696/run，实测 ~8,000 samples/s，`clockid: monotonic (1)` | `traces/perf-callgraph/` |
| 普通 `perf record -g`（无 `--user-callchains`） | **失败**：`Failure to open event 'cycles:u' on PMU 'cpu'`（paranoid=2 拒绝 kernel callchain）；`--user-callchains` 设 `exclude_callchain_kernel=1` 后可开 | 同一命令去掉 `--user-callchains` |
| 符号化 release 二进制 | `set_strip("none")`（研究副本 `xmake/bench.lua` 环境开关 `SLUICE_595_STRIP_NONE=1`）可保留 symtab；无 DWARF，`fp` unwinder 调用者链含伪帧，caller 归因不可靠 | `.text`/`nm`、trace README §5 |
| uprobe / USDT | 仍**不可用**（bpftrace root-only、tracefs root-only）；Sluice 无 USDT note；本轮未使用，改由符号化采样 + 源码 TSC 计数器完成 | `bpftrace -l` 无 root 仅 1 行；`readelf -n` 无 NT_STAPSDT |
| 源码 TSC 聚合计数器（研究副本） | 可用；开销实测（同二进制 ON/OFF）：最小探针集 CPU/op A +0.65%、B +1.96%，完整集 A +3.05%、B +2.72%；p95/p99 CI 含 0 | `counters/C1_COUNTERS.md` §4 |
| 空 TSC pair 基线 / 校准 | 空 pair 9.8–9.9 ns（28.4–28.6 ticks）；`CLOCK_MONOTONIC` 5 ms 校准得 TSC 2.893 GHz | `counters/C1_COUNTERS.md` §1.2 |

结论：在无 root 的本机，**用户态**函数级观测路径为“符号化 perf 采样（路径发现）+
源码聚合计数器（次数/单次成本）”；内核态 callchain/PMU、uprobe/USDT 仍受权限限制。

## 9. C2 补充探测（2026-11-04，无 root；syscall 级）

本节为 C2 深挖会话的实测补充。**与前两会话的差异：`sudo -n true` 实测要求密码
（rc=1）**——S1/S2 会话记载的"sudo 可用（仅观测用途）"依赖交互式口令，
不可交互的 agent 会话中 root 路径全部不可用；证据 `traces/raw-c2/obs-02-perf-ftrace-noroot.txt`。

| 能力 | 实测结果 | 命令/证据 |
|---|---|---|
| bpftrace 一切探针（无 root） | **失败**：`ERROR: bpftrace currently only supports running as the root user.`（rc=1；`-lv` 与 `-l` 同） | `obs-01-bpftrace-noroot.txt` |
| perf 内核态 / tracepoint | **失败**：`cycles:k` → `No supported events found`；`perf trace -e syscalls:...` → `No permissions to read /sys/kernel/tracing/...` | `obs-02` |
| BTF 验证 tracepoint 存在性（无 root） | **可用**：`bpftool btf dump file /sys/kernel/btf/vmlinux format c` 列出全部 17 个 `trace_event_raw_io_uring_*` 结构（含字段）——tracepoint 在本内核编译在内，attach 需 root | `obs-03/04` |
| strace（ptrace，无 root） | **可用**（`ptrace_scope=0`）：`io_uring_enter/setup/register` 参数与返回值全解码（fd,to_submit,min_complete,flags,argp,argsz,ret）；无采样无丢失（停止式）。**代价实测**：CPU/op +147~158%、吞吐 −86%、bench p50 ×4.3~8.5；且对 #595 D2 B 臂有行为掩蔽（eventfd write 塌缩为每 pass 1 次，因无 drain） | `traces/c2-syscall/README.md` §0/§2、`counters/C2_PROBE_OVERHEAD.md` |
| liburing LD_PRELOAD interposition（无 root） | **可用**：`io_uring_submit`/`io_uring_peek_batch_cqe`/`io_uring_queue_init`/`io_uring_get_events` 为 JUMP_SLOT 动态符号（`readelf -r` 实测）；`io_uring_get_sqe` 自 liburing 2.2 头文件内联，**不可** interpose。开销实测 CPU/op +1.6~2.3%、吞吐 −1.6~2.3%（TSC 事件环 ~3.1 事件/op） | `traces/raw-c2/c2_liburing_shim.c`、`shim_fractions.py` 输出 |
| 跨仪器时钟一致性 | shim TSC 每进程自校准 2,893,334–2,893,359 kHz（6 次极差 <25 kHz）与 C1 的 2.893 GHz 一致；strace 用 CLOCK_REALTIME µs。跨仪器不做时间轴对齐，A/B 比较限单仪器内 | `.shim.meta`、`C2_PROBE_OVERHEAD.md` §4 |

结论：无 root 的 syscall 级观测路径为“strace（参数/返回值/计数不变量）+
liburing interposer（原生速度时点/时长结构）”；tracepoint/kprobe/uprobe 仍 root-only。
strace 对时序敏感的用户态机制（defer/通知合并）有**行为改变**风险，须与源码计数器交叉验证。

## 10. C3 补充探测（2026-11-04，无 root；内核/io-wq 域）

本节为 C3 深挖会话的实测补充；权限条件与 C1/C2 会话相同（`perf_event_paranoid=2`、
`unprivileged_bpf_disabled=2`、tracefs root-only、`sudo -n` 需密码）。

| 能力 | 实测结果 | 命令/证据 |
|---|---|---|
| io_uring/wq/sched tracepoint、ftrace、kprobe、`:k` PMU | **全部仍不可用**（root-only）；io-wq（内核线程）函数级归因无无-root 路径 → io-wq work 内容 UNOBSERVED | `traces/c3-kernel/README.md` §5；C2 obs-01/02 复核 |
| `/proc/<pid>/task/<tid>/io`（syscr/syscw/rchar/wchar，内核 syscall 记账） | **可用**（同 uid 子进程，1 ms 采样单调末值）；可区分用户态发起 read/write 与 io_uring 内部工作（后者不过记账层）；kthread 线程（iou-wrk）可读且恒 0 | `tools/run_sysio_blocks.py`、`raw/sysio-d2/`、`C3_COUNTERS.md` §3 |
| 源码 TSC 探针扩展到内核路径 syscall 面（drain read/poll/write/返回分类） | 可用；开销 CPU/op A +0.93% [−1.34,+2.96]、B +0.57% [−0.14,+1.35]（CI 含 0），内核态时长读数不受探针污染（内核 ON/OFF 差 CI 含 0） | `research_595_c3.hpp`、`C3_COUNTERS.md` §2.3 |
| strace -f -c（按臂对照的进程级 syscall 面） | 可用（有 C2 已证的行为掩蔽，只用于排除性结论：两臂 enter 总数逐次相等、无其他 syscall 差异面） | `traces/c3-kernel/strace-c-sum-{A,B}.txt` |
| /proc/<tid>/wchan + state 采样（kthread park 点） | 可用；实测 iou-wrk park 于 `io_wq_worker`（io-wq worker wait queue） | `traces/c3-kernel/iowq_samples.txt` |
| liburing 行为学探针（inline vs 异步完成判定） | 可用（submit 后用户态 peek CQ + sched_yield 分桶）；实测 warm tmpfs/ext4 4k buffered read 99.6–100% 在 submit 内完成 | `traces/c3-kernel/probe_read_ctx.c`、`probe-inline-*.txt` |

结论：无 root 的内核域观测路径为"/proc io 内核记账（syscall 面次数）+ 源码 TSC
探针（单次 on-CPU 时长）+ /proc wchan/ctxsw（kthread 行为学）+ liburing 行为学探针
（完成上下文判定）"；io_uring tracepoint/内核调用图/io-wq work 内容保持 UNOBSERVED。

## 11. C4 补充探测（2026-11-04，无 root；调度域）

本节为 C4 深挖会话的实测补充；权限条件与 C1/C2/C3 会话相同（`perf_event_paranoid=2`、
`unprivileged_bpf_disabled=2`、tracefs root-only、`sudo -n` 需密码）。原始输出
`../traces/raw-c4/obs-01-sched-tracepoint-noroot.txt`。

| 能力 | 实测结果 | 命令/证据 |
|---|---|---|
| sched tracepoint / perf sched / perf trace / bpftrace sched:* | **失败**（复核 C2 obs-01/02 同结论：paranoid=2 + tracefs root-only + bpftrace root-only）→ sched_wakeup/waking/switch 逐事件、sched_stat_sleep/blocked/iowait、抢占者身份 UNOBSERVED | `obs-01-sched-tracepoint-noroot.txt` |
| `/proc/<tid>/schedstat`（per-task on_cpu_ns / runqueue_wait_ns / timeslices，ns 精度，CONFIG_SCHEDSTATS） | **可用**（同 uid 子进程，含 kthread iou-wrk）；这是无 root 下调度域最强直接观测——runnable delay 总量、on-CPU 总量、调度段数 | `counters/C4_SCHED.md` §1/§2 |
| `/proc/<tid>/sched`（nr_switches×3 + `se.nr_migrations`） | **可用**；与 /proc status ctxsw 双口径逐对一致，与 schedstat timeslices 逐对相等（136,720==136,720 smoke 验证）；迁移计数无需 root | `C4_SCHED.md` §1 交叉验证 |
| `/proc/<tid>/stat` `processor` 字段（最后运行 CPU） | **可用**；解析陷阱：comm 后第 36 索引（第 39 字段）——首轮误用 38（policy 字段），smoke 发现 cpus_seen=[0] 与 taskset -c 2 矛盾后修复重验 | `tools/run_schedstat_blocks.py` parse_stat |
| `/proc/pressure/cpu` some total（系统级 CPU 压力累计 µs） | **可用**（前后差分）；粒度系统级，仅辅助（本负载 A/B +2.14% n.s.） | `C4_SCHED.md` §4 |
| 1 ms /proc 采样器（5 文件/tid：schedstat/stat/status/sched + PSI）的开销 | **实测远超 D2 效应**：bench CPU/op +5~8%、吞吐 −6%、p99 +30%+（ON vs OFF 非配对，含 ±1.2% 块间漂移）。采样器 arm-blind 时 A/B 相对差与无采样 Phase B 一致（+3.11/+3.88/+3.88% vs +3.70%）→ 绝对值禁用于定量，相对差可用；不排除对 runwait 绝对值的系统性抬高 | `C4_SCHED.md` §4 |
| wakeup→run 逐事件延迟 | **无无-root 路径**（需要 tracepoint 配对）；代理 = runwait/timeslices（≈1.7 µs/段，A/B 不变） | `traces/c4-sched/README.md` §1 |

结论：无 root 的调度域观测路径为"/proc per-task 调度记账（schedstat 三元组 + sched
迁移/切换 + status 交叉口径）+ 源码论证（唤醒目标结构性证明）+ PSI 总量对照"；
逐事件 wakeup/switch 序列与尾部分布保持 UNOBSERVED。

## 12. C5 补充探测（2026-11-04，root 恢复；跨层关联会话）

本节为跨层关联会话的实测补充；**与前四轮的差异：sudo 口令管道可用
（`echo <pw> | sudo -S`），root 观测路径全部恢复**。证据 `traces/xlayer/`。

| 能力 | 实测结果 | 命令/证据 |
|---|---|---|
| bpftrace root（tracepoint 全域） | **可用**：15 探针 attach，io_uring/syscalls/sched tracepoint 实采 35k+ 行/run 零丢失 | `traces/xlayer/*/kernel.txt` |
| bpftrace v0.24.2 陷阱 1 | positional 参数 `$1` 为 str，`pid == $1` 静默永假（无编译错、零事件）；须按 pid 实例化脚本或 `pid == 12345` 字面量 | `traces/xlayer/README.md` §7.1 |
| bpftrace v0.24.2 陷阱 2 | 默认 perf ring（64 页/CPU）高事件率丢 19k–22k 事件（stderr `Lost N events`）；`BPFTRACE_PERF_RB_PAGES=2048` 后零丢失；16384 页/CPU `std::bad_alloc` | 同上 §7.2 |
| io_uring tracepoint 全字段实采 | submit_req/qasync/complete/task_add/task_work_run/defer/cqe_overflow/cqring_wait/create/req_failed 按 `(ctx,user_data)` 与用户态事件环逐请求 join 成功（6000/6000） | `tool_join_analysis.py` |
| io-wq worker 归属 | complete/qasync 可按 `@ring_owner[ctx]` map 过滤到发起进程；worker（PF_IO_WORKER）tgid=发起进程、独立 tid | `traces/xlayer/README.md` §5.2(d) |
| 跨仪器时钟对齐 | 用户 clock_gettime(CLOCK_MONOTONIC) 与 bpftrace nsecs 同源：6000 对 flush↔enter 差 p50=773 ns p99=878 ns | 同上 §2.3 |
| 内核 io_uring 行为直接观测 | warm tmpfs 4k read **100% 经 io-wq**（qasync 6004/6004，complete 全由 worker tid 投递）——修正 C3 行为学"inline 完成"的解释（数据不矛盾） | 同上 §5.3 |

C1–C4 会话的"无 root"结论在其会话条件下仍成立；本节不推翻，仅记录本会话
权限条件变化后的可用路径。
