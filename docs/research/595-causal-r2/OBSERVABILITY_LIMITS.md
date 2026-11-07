# E5 观测能力边界与缺失清单（Sluice #595 causal-r2）

状态: ACTIVE EVIDENCE。与 CAPABILITY_MATRIX.md 配对：矩阵列"有什么"，本文件列"没有什么/在哪里到头"。
每条含：缺失/受限事实 → 实测证据 → 对 #595 归因的影响 → 替代路径及开销估计（未实测，文献级）。
标签: UNOBSERVED = 本会话未观测到；OBSERVABILITY_LIMIT_REACHED = 能力到头；CORRELATION_PARTIAL = 只能拿到相关证据不能拿直接因果证据。

## 1. 已确认缺失（UNOBSERVED / OBSERVABILITY_LIMIT_REACHED）

### 1.1 `lock:lock_acquire` / `lock:lock_release` tracepoint 不存在 → perf lock record 静默空数据【陷阱】

- 事实：tracefs available_events 中 lock 子系统只有 `contention_begin`/`contention_end`/`contended_release` 3 个事件。
- 证据：`grep ^lock: /sys/kernel/tracing/available_events` → 仅 3 行；`/boot/config-7.2.11-200.fc44.x86_64:12049` `# CONFIG_LOCK_STAT is not set`、`:12055` `# CONFIG_DEBUG_LOCK_ALLOC is not set`、无 `CONFIG_LOCKDEP=` 行。
- 实测陷阱：`perf lock record -- sleep 0.3` **不报错**，写出 0.036MB 文件，`perf lock report` 表头正常但**表体全空**；`perf report --header-only` 显示实际记录的是 `lock:contention_begin`+`lock:contention_end`。并发 Python 锁竞争下重测仍空——因为 `threading.Lock` 是 futex，`lock:contention_*` 只在内核 mutex/rwsem 慢路径触发。
- 另一陷阱：`perf lock contention record ...` 不是有效子命令组合，报 "incompatible file format ... Initializing perf session failed"（误导性报错）；正确流程是 `perf lock record` + `perf lock contention report`。
- 对 #595 影响：若 Sluice 内部用内核可见的 mutex（futex 路径**不算**）且竞争发生在慢路径，只有 contention_begin/end 可看；锁持有时长（acquire→release 区间）**不可直接观测**。
- 替代路径（开销估计为文献级，未实测）：
  a. futex 配对：`syscalls:sys_enter_futex`→`sys_exit_futex`（ret<0 为阻塞）+ `sched:sched_stat_blocked`——每事件 1 条缓冲记录，成本 ~tracepoint 常规量级；**这是用户态锁等待的直接证据源**（futex syscall tracepoints 全部存在，bpftrace 实测 2s 窗口 1397 ops）。
  b. `kprobe:futex_wait`/`futex_wake` 或 fentry `futex_wait`——更高精度，kprobe 每命中 ~百 ns 级，高频路径需采样。
  c. sched:sched_switch prev_pid/prev_state 重建等待窗——已有事件，零新增开销，但只能到 runqueue 粒度（CORRELATION_PARTIAL：区分不出锁等待 vs 其他睡眠原因，需与 futex 事件交叉）。
  d. 临时源码探针（研究副本加 SDT/计时打点）——最强因果证据，成本见 §1.6。

### 1.2 `futex:` / `mutex:` / `rwsem:` / `rtmutex:` tracepoint 子系统不存在

- 证据：`grep -E "^(futex|mutex|rwsem|rtmutex):" available_events` → 空输出。
- 影响：锁竞争细粒度（wait_start/wake 路径）无专用事件。
- 替代：§1.1 的 a/b/c。

### 1.3 `/proc/PID/delay` 不存在 → per-task delay accounting 不可用【OBSERVABILITY_LIMIT_REACHED】

- 事实：CONFIG_TASK_DELAY_ACCT=y、CONFIG_TASKSTATS=y（/boot config 实证），但 `/proc/<pid>/delay` 返回 "No such file or directory"，且不存在 `/proc/sys/kernel/task_delay_acct` sysctl，/proc/cmdline 无 delayacct 相关参数。原因（该内核 7.2.11-fc44 的导出策略）UNOBSERVED——不猜测，只记录现象。
- 影响：per-task blkio/iowait/swapin/freepages 延迟聚合**没有**这个直接来源。
- 替代（实测存在的字段）：
  a. `/proc/PID/schedstat` 字段 2 = runqueue 等待 ns（实测 sleep 进程输出 `1677278 0 1`；CONFIG_SCHEDSTATS=y）——CPU 延迟有直接来源，iowait/blkio 延迟没有。
  b. `sched:sched_stat_iowait`/`sched_stat_blocked`/`sched_stat_sleep`/`sched_stat_wait` tracepoints（存在，字段 comm/pid/delay u64）。
  c. `block:block_rq_issue`→`block_rq_complete` 配对。
- 开销：/proc 读取 0；tracepoint 常规量级。

### 1.4 裸 `io_uring_enter` 函数不可 fentry 附加

- 事实：`bpftrace -l 'kfunc:*io_uring_enter*'` 只返回 `fentry:vmlinux:__ia32_sys_io_uring_enter` 与 `__x64_sys_io_uring_enter`；`bpftrace -e 'kfunc:io_uring_enter ...'` 失败："No BTF found for io_uring_enter."（实测）。
- 影响：不能直接挂在（可能内联/static 的）`io_uring_enter` 实现函数入口。
- 替代：`tracepoint:syscalls:sys_enter_io_uring_enter`（存在，字段 fd/to_submit/min_complete/flags/argp/argsz 全确认）或 fentry `__x64_sys_io_uring_enter`；子路径用其他 17 个 io_uring tracepoint 覆盖。开销：tracepoint 常规量级。

### 1.5 bpftrace v0.24.2 不再接受裸类别名【脚本兼容性陷阱】

- 事实：`bpftrace -l 'io_uring:*'`、`bpftrace -l 'syscalls:*io_uring*'` 均报 `ERROR: Invalid probe type: io_uring/syscalls`；必须 `tracepoint:io_uring:*`、`tracepoint:syscalls:*io_uring*`（实测）。
- 影响：网上/旧文档常见一行脚本直接搬用会失败；不代表 tracepoint 缺失。
- 对策：所有研究脚本统一 tracepoint: 前缀；raw/perf 配方里的 `syscalls:` 写法同样调整。

### 1.6 Sluice 无 USDT 探针 → 用户态语义事件不可直接探【OBSERVABILITY_LIMIT_REACHED（当前形态）】

- 事实：`grep -rIln -E 'stapsdt|SDT_PROBE_DEFINE|__dtrace' src include apps`（/home/jnhu/Source/Sluice）→ 无匹配（exit 1）；bench 二进制 `readelf -n build-bench-prof/linux/x86_64/releasedbg/bench_w01_direct` → 无 NT_STAPSDT note（有 debug_info、未 strip、192 个 FUNC 符号）。
- 影响：请求提交/发布/生命周期这类**应用语义**边界只能靠符号 uprobe（函数级）或读源码定位，没有稳定 ABI 级探针。
- 替代与代价：
  a. 符号 uprobe：现在即可用（实测 uprobe 机制在 /bin/echo 上工作正常，31 次 malloc 捕获）。限制：函数名/内联变化即失效；每命中 ~µs 级（文献），高频提交路径上可能改变被测时序——研究 run 必须单独做开/关对照。
  b. 研究副本加 `<sys/sdt.h>` USDT 探针后重编（允许：写入 /home/jnhu/sluice-595-e5/causal-r2 独立副本，不动主工作树）。USDT 未使能时开销 ≈ 1 条 nop/分支（近 0），使能时 uprobe 量级——这是"临时源码探针"的最佳形式，因为不采时近零开销。
  c. 尾部方案：研究副本内加显式计时/计数导出（违反"不叠加进 timing run"纪律的风险高，仅在 a/b 不够时考虑）。

### 1.7 Topdown 指标在短进程上计不满【已修正计数器表述（S2 复核）】

- 事实：`perf stat -M TopdownL1 -- /bin/true`（1ms）→ `<not counted> (0.00%)` + 提示 "Some events weren't counted. Try disabling the NMI watchdog"；`/proc/sys/kernel/nmi_watchdog`=1。
- **S2 复核修正**：S1 曾写"Haswell-EP 每逻辑核 3 fixed + 2 generic，watchdog 占 1 generic → 仅 1 个可用"。**该表述错误**。S2 实测：3 个 generic 事件并发 100% runtime 无复用，4 个并发仍 100%，5 个并发即复用（~79–81%）→ **每逻辑核 4 generic + 3 fixed，全部 4 个 generic 可自由分配**；nmi_watchdog=1 的 hardlockup detector 占用的是 fixed counter（内核 5.x+ 行为），不挤占 generic。S1 误写的真正后果仅是高估了复用严重度，实测复用数据（8 事件 → 37.49%–62.50%）不受影响。
- 仍然成立的约束：TopdownL1 需要 4 个计数器分组轮转，**短窗口（<~50ms）下分组未轮转完即退出 → `<not counted>`**。这不是计数器数量不足，而是窗口太短。
- 对策：因果 run 用 ≥ 数百 ms 窗口或 `-I` interval；复用比例必须随结果记录。关闭 nmi_watchdog 属全局系统改动，本探测**未做**；若 bench 阶段判定必需，须作为显式决策记录（含复原）。
- 实测复用参考：8 事件 3.5s busy loop → 37.49%–62.50%（raw/perf-stat-multiplex.txt）；S2: 6 事件 → 63.44%–84.75%。

### 1.8 /proc/PID/latency【S2 已验证，原 UNVERIFIED 项关闭】

- CONFIG_LATENCYTOP=y；S2 实测 `ls /proc/self/latency` → 文件存在。内容是否在当前内核配置下持续更新未验证（需 latencytop 使能场景）——使用前先读非空校验。

### 1.9 bpftrace v0.24.2 时间内建只有 `nsecs`【S2 新增防呆】

- 事实：`nsecs_monotonic` / `nsecs_boot` 等内建在本机 bpftrace v0.24.2 上实测不存在（脚本内使用即语法报错，错误点落在该内建处）；`nsecs` 实测可用（ktime，CLOCK_MONOTONIC 系）。
- 影响：跨工具脚本里不能按新版 bpftrace 文档写 `nsecs_monotonic`。
- 对策：统一用 `nsecs`；它与 ftrace `trace_clock=mono`、perf `-k 1:0` 的对齐规范见 §2.9。

### 1.10 bpftrace tracepoint 脚本中 printf 三元/正则组合有语法限制【S2 新增防呆】

- 事实：`printf(..., str(probe) !~ "..." ? "-" : "", ...)` 形式实测报 "syntax error, unexpected !"（S2）。bpftrace 的 map key/printf 参数位置对类型推导严格。
- 影响：复杂格式化脚本初稿易失败，属脚本问题而非能力缺失。
- 对策：按 probe 类型拆分成多个 probe 块（S2 用 4 个独立块成功完成 io_uring 四事件联合监听）。

## 2. 受限项（CORRELATION_PARTIAL / 使用约束）

### 2.1 tracefs root-only（drwx------）

- 一切 ftrace、bpftrace、perf 内核态事件都需 sudo；非 root 下 perf 自动降级 `:u`（实测 cycles:u 229,130）。研究 run 全部按 root 执行记录权限上下文。

### 2.2 perf_event_paranoid=2 / max_sample_rate=79000

- 非 root 无内核事件；perf record 上限 79000 samples/s（默认 -F 999 远低于上限，够用）。改 paranoid 属全局改动，不做。

### 2.3 `scaling_cur_freq` 不是实测频率

- driver=intel_cpufreq（passive）+ schedutil：sysfs 读数是调度器**请求**频率；频率漂移归因必须用 turbostat 的 Bzy_MHz（APERF/MPERF 实测平均，1s 间隔实测通过）。否则会把"调度器没请求高频"和"硬件没上到请求频率"混为一谈——对 E5-2666 v3（max 3.5GHz）这类归因是直接错误源。

### 2.4 PSI 粒度粗

- some/full avg10/60/300 + total（µs 累计）。适合实验前后总量对照与"是否存在资源压力"判定，不能定位到线程/请求（CORRELATION_PARTIAL）。线程级压力走 schedstat + sched tracepoints。

### 2.5 RAPL/uncore 是包/域级

- power/energy-pkg 只能给整个 socket 能量对照；单线程能耗归因不可行（物理限制，非工具缺失）。

### 2.6 `io_uring_task_work_run` 字段名 tctx（非 ctx）

- `bpftrace -lv` 实测字段 `void *tctx, unsigned int count`。脚本按 ctx 写会编译失败——不是内核缺事件，是字段命名差异，列入防呆。

### 2.7 PMU 复用使计数带误差

- 任何跨多事件（>每逻辑核 free 计数器数）的 perf stat，事件值只覆盖启用时间比例，实测 8 事件 → 37.5%–62.5%。归因规则：事件值与 (复用百分比) 必须成对引用；比较实验必须保证两组复用比例相近，否则不能归因差异。

### 2.8 探测造成的系统状态变更（已全部复原）

- ftrace 探测使用隔离 instance `instances/capprobe`（S1）/ `instances/captest`（S2），测后均已删；全局 current_tracer=nop、tracing_on=1 未动（实测复原值）。/tmp 下 perf 数据文件与 probe 程序为探测产物（S2: probe_iouring.c/probe_iouring、bt*.out、perf 数据）。S2 kprobe_events `mytest` 已写入并清理。tracefs 挂载本来就在（非本次变更）。无其他全局变更。

### 2.9 跨工具时间戳对齐需显式配置【S2 新增规范】

- 事实（S2 实测）：各工具默认时间戳基准不同——ftrace 默认 `trace_clock=local`（不可跨 CPU 对齐）；perf 默认 clockid=CLOCK_MONOTONIC 之外的 per-event 时间基（可用 `-k` 覆盖）；bpftrace `nsecs` 是 ktime（CLOCK_MONOTONIC 系）；trace-cmd report 显示的是 uptime 基秒数（raw/trace-cmd-test.txt 时间戳 18403.x 实证）。
- 规范（因果 run 必须统一，写进 run 记录）：
  - ftrace/trace-cmd：`echo mono > /sys/kernel/tracing/trace_clock`（可选值实测有 `local global counter uptime perf mono mono_raw boot tai x86-tsc`）；
  - perf：`perf record -k 1:0`（CLOCK_MONOTONIC）；
  - bpftrace：用 `nsecs`；
  - 对齐基准可用 `x86-tsc`（TSC constant/nonstop，本机 TSC_MHz≈2897 由 turbostat 实测）或 CLOCK_MONOTONIC 系（mono/nsecs/-k 1:0）统一。
- 不做对齐时，"tracepoint 时序 vs perf 采样 vs ftrace 时序"之间只有定性相关（CORRELATION_PARTIAL），不能做严格区间配对。

### 2.10 io_uring tracepoint 的 ctx↔fd 映射需经 io_uring_create 桥接【S2 新增】

- 事实：io_uring tracepoint 全族以内核 `ctx` 指针标识 ring（submit_req/complete/cqring_wait/task_work_run/local_work_run/queue_async_work 实测均有 ctx 或 tctx）；`/proc/<pid>/fdinfo/<fd>` 提供头尾状态但**不含 ctx 指针**；`io_uring_create` 事件字段 `fd, ctx, sq_entries, cq_entries, flags`（S2 实测）是唯一同时携带两者的点。
- 对策：长会话归因 run 先监听 `io_uring_create` 记录 (pid, fd, ctx) 三元组，后续事件按 ctx 归 ring、按 fd 归应用；`io_uring_register` 同样带 ctx 可作对账。
- 影响：不做此桥接时，多 ring 进程的事件只能按 ctx 分组、无法直接映射到应用层 fd（CORRELATION_PARTIAL）。

## 3. 明确未做的事（如实声明）

- 未运行任何 benchmark 或正式开销测量（任务约束）；§ 开销量级均为文献级估计并已在矩阵中标注"未实测"。S2 附加的两组轻量自测（空闲态 wall-clock 对照、bpftrace 自身 CPU）只界定探测自身干扰量级，见 CAPABILITY_MATRIX §7，不构成每事件开销测量。
- 未测试 stap/SystemTap（工具存在；bpftrace 已覆盖需求，且 SystemTap 依赖额外 debuginfo 包，不投入）。
- perf trace、perf mem（PEBS load-latency）S2 已实测通过（更新原"未测"声明）；**perf c2c、bpftrace socket:* 等网络方向能力仍未测**（不在 §2 清单内）。
- /proc/PID/latency 文件存在性 S2 已验证；其内容在当前配置下是否持续更新未验证（需 latencytop 使能场景）。
- 未验证 `CONFIG_HZ` 之外的 timer 精度实测（clocksource=tsc + constant/nonstop flags 已记录）。

## 4. 对 #595 因果归因的观测路径结论（直接测量 vs 反事实支持 vs 未知）

- 可直接测量（本机已验证存在且可用）：io_uring 提交/完成/task_work/cqring_wait 全链路 tracepoint（S2 端到端触发验证：liburing nop 经 (ctx,user_data) 配对成功）；sched 延迟链（wakeup→switch）；futex syscall 序列；per-task CPU/runqueue 记账（/proc + schedstat）；实测频率（turbostat）；PMU 计数（含复用比例）；PSI 总量；block 层 IO；ring 积压对账（fdinfo）；PEBS load-latency 采样（perf mem）。
- 只能反事实/间接支持（CORRELATION_PARTIAL）：锁持有时长（无 lock_acquire）；用户态语义边界（无 USDT，需研究副本加探针或符号 uprobe）；单线程能耗与频率因果（包级计数器 + 实测频率对照）；iowait/blkio per-task 聚合（无 /proc/PID/delay，用 tracepoint 配对替代）；多 ring 事件的 fd 归属（需 io_uring_create 桥接，见 §2.10）。
- 未知（UNOBSERVED，不编造）：/proc/PID/delay 缺失的内核侧原因；该内核对 fentry 附加面的完整白名单（按需用 `bpftrace -l 'kfunc:<名字>'` 逐个确认）；内核是否对 io_uring tracepoint 做了 static key 折叠（不影响字段存在性，影响未使能时开销，未实测）；/proc/PID/latency 内容更新频率（文件存在性已确认，更新行为未验证）。
