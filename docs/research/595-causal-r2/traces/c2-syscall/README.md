# C2 提交路径深挖：syscall 级 trace（Sluice #595 causal-r2）

状态：ACTIVE EVIDENCE。轮次：causal-r2 C2（2026-11-04）。
范围：D2（`W02-uring-4k-d2`）A·IMMEDIATE vs B·BOUNDED_PROGRESS 的 `io_uring_enter`
提交路径：syscall 参数、返回值、执行时长、flush 位置。
配套：`counters/C2_PROBE_OVERHEAD.md`（探针开销）；原始数据 `../raw-c2/`。

## 0. 结论先行（直接测量 / 推断 / 不可观测）

**直接测量（本文件主结果，来源见 §4 命令与文件）**

1. **D2 中 enter 总数 == 纯提交次数，两臂完全一致**（strace，ptrace 逐 syscall、
   无采样无丢失；每臂 2 次独立进程 × 3 个 measured rep × 20,000 op = 120,000 个
   measured enter）：
   - 稳态 enter **100% 为 submit-only**：`io_uring_enter(fd, to_submit=1, min_complete=0,
     flags=0, NULL, 8) = 1`。**零** wait-only enter（无 `to_submit=0`+`IORING_ENTER_GETEVENTS`）、
     **零** submit-and-wait enter（无 GETEVENTS）、**零** `to_submit>1`（无跨请求 batching）、
     **零**部分提交（`0<ret<to_submit`）、**零**错误（`ret<0`）、**零**空调用 enter。
   - 唯一非 submit-only enter 是每 pass 环建立时的 statx 能力探针
     `enter(fd, 1, 1, IORING_ENTER_GETEVENTS) = 1`（每 invocation 恰 4 个：1 warmup + 3 measured，
     另有 1 个自检环探针）；`io_uring_setup` 每 invocation 恰 5 个（1 个 sq_entries=8 自检 +
     4 个 sq_entries=2 pass 环），A/B 结构逐项相同。
   - 计数守恒：每 invocation 80,005 enter = 4×(20,000+1) + 1；无 unfinished/resumed。
   - 原生速度复核（LD_PRELOAD liburing interposer，无 ptrace）：每 20,000 op segment 恰
     20,000 次 `io_uring_submit`，**ret≡1（0/120,000 例外，两臂合计 240,000 次调用）**。
2. **submit syscall 自身时长无 A/B 差异**（两种仪器一致）：
   - strace（ptrace 分辨率 ~11–15 µs 地板）：pooled mean A 10,430 / B 10,498 ns；
     段均值 B−A = **+67.7 ns [−154.1, +287.5]**（bootstrap 10k，CI 含 0）。
   - shim（原生速度，TSC 包住整个 `io_uring_submit` 调用，含 liburing flush_sq 用户态部分
     + enter syscall + inline 完成）：mean A 2,512 / B 2,550 ns；
     段均值 B−A = **+37.9 ns [−33.7, +110.4]**（CI 含 0）。分布宽（p50 ≈1.2 µs，
     p99 ≈5.3 µs；段 p50 在 ~1.2/~3.3 µs 两模式间交替 = 核频档位漂移，G6 同源）。
   - ⇒ **B 的内核 CPU 增量（+79.4 ns/op，Phase B）不在 `io_uring_enter` 路径内**；
     与 C1 的 in-situ TSC（flush +12.1 ns/call，1.7%）和 C3 的 eventfd-write 归因方向一致。
3. **flush 位置迁移被直接观测**（shim，原生速度；这就是 D2 "submit 时间变化"）：
   - A：**0/120,000** 个 submit 之后 250 ns 内跟随 `io_uring_peek_batch_cqe`
     （submit→peek 间隔 p10 = 322 ns，p50 = 476 ns）。
   - B：**35,921/120,000 = 29.93% [29.7, 30.4]**（6 段 29.72–30.39%）的 submit 之后
     **250 ns 内**即跟随 peek_batch（p10 = 74 ns，p50 = 136 ns）——一个 A 中不存在的
     **poll-pass 内 flush 群体**（submit→reap 背靠背）。
   - B 的 peek 计数 0.550–0.562/op vs A 0.537–0.544/op；单次 peek 收割 1 个 CQE 的占比
     B ~20% vs A ~15%（B 更常"逐个完成"而非成对收割）——完成节奏随 flush 位置移动。
   - ⇒ **defer/flush 迁移在低开销 interposition 下保持存在**（与 Phase B 计数器
     `sqes_deferred=0.5589/op` 同向；本仪器直接看到的是其中紧邻 poll 的 ~30% 群体，
     其余 deferral 的 poll 内簿记较慢、间隔分布与 A 交叠，不能单靠 250 ns 阈值分离）。

**推断（有直接证据支持但非逐事件观测）**

4. **strace 会掩蔽 B 的通知路径**：ptrace 下 B 的 eventfd write 塌缩为**每 pass 恰 1 次**
   （首个 signal 写入后 `notification_pending_` 永置位——因为无 drain：0 次 eventfd read、
   0 次 park/poll、0 次 futex，两臂相同），B 的稳态 syscall 序列与 A **逐 syscall 不可区分**。
   但 B 的 latency-pass p50 在 strace 下为 A 的 2 倍（77.0 vs 38.6 µs）且 syscall 计数相同
   ——与"deferral 仍在发生、完成被批量化"一致（不可在 syscall 层计数）。
   ⇒ strace 只用于 §0.1 的参数/返回值不变量；**B 的 write/coalesce/park 混合以
   Phase B 计数器（0.2353/0.3236/op）为准**。
5. **enter 计数指标本身不掩盖参数或内核路径变化——因为两者都没变**：参数恒为
   (1,0,0)，ret 恒 1，enter 时长无差异；被掩盖的是**用户态 flush 的时点**（§0.3）
   与**通知路径**（计数器域），它们在 syscall 计数层不可见。

**不可观测（本会话 OBSERVABILITY_LIMIT_REACHED）**

- `syscalls:sys_enter/sys_exit_io_uring_enter` 与全部 17 个 io_uring tracepoint 的
  attach：bpftrace/perf/ftrace 均需 root（本会话无 root，实测失败输出见 §1）。
  本内核确实编译入了这些 tracepoint（BTF 中 17 个 `trace_event_raw_io_uring_*` 结构
  全部存在，实测）；字段名与 strace 解码的参数一致（fd/to_submit/min_complete/flags/argp/argsz/ret）。
- 每个 SQE 的 dispatch→flush 逐事件时间线（需要源码时间戳环或 root tracepoint）；
  本轮以 shim 的 submit→peek 拓扑替代（§0.3）。
- enter 内部内核路径分解（inline 完成路径、kernel 侧 eventfd 信号）属 C3 域，本轮未观测。

## 1. 观测能力核查（本会话实测，全部失败输出存档 `../raw-c2/obs-0*.txt`）

| 检查 | 命令 | 结果 |
|---|---|---|
| bpftrace tracepoint | `bpftrace -lv 'tracepoint:syscalls:sys_enter_io_uring_enter'` | `ERROR: bpftrace currently only supports running as the root user.`（rc=1；obs-01） |
| bpftrace io_uring 列举 | `bpftrace -l 'tracepoint:io_uring:*'` | 同上（obs-01） |
| perf 内核态 | `perf stat -e cycles:k -- sleep 0.2` | `No supported events found`（paranoid=2，无 CAP_PERFMON；obs-02） |
| perf trace tracepoint | `perf trace -e syscalls:sys_enter_io_uring_enter -- true` | `No permissions to read /sys/kernel/tracing/...`（obs-02） |
| ftrace/tracefs | `ls -ld /sys/kernel/tracing` | `drwx------ root root`（root-only；obs-02） |
| sudo | `sudo -n true` | `sudo: a password is required`（rc=1）——前一轮能力探测会话的交互式 sudo 在本会话不可用（obs-02） |
| BTF（无 root 可读） | `bpftool btf dump file /sys/kernel/btf/vmlinux format c` | 17 个 `trace_event_raw_io_uring_*` 全部存在（obs-03/04）→ tracepoint 在本内核存在，只是 attach 需 root |

回退链实际执行：tracepoint（不可）→ perf/ftrace（不可）→ uprobe/fentry（不可，同因 BPF/root）
→ **strace（ptrace，`ptrace_scope=0`，无需 root）** + **liburing LD_PRELOAD interposition**
（`io_uring_submit`/`io_uring_peek_batch_cqe`/`io_uring_queue_init`/`io_uring_get_events`
为 JUMP_SLOT 动态符号，实测可 interpose；`io_uring_get_sqe` 自 liburing 2.2 起为头文件内联，
**不可** interpose——dispatch 时刻因此无标记，改用 submit→peek 拓扑）。

## 2. 实验设计

- 二进制：研究副本 `worktree/build/linux/x86_64/release/bench_w02_pipeline`
  sha256 `755346410fd11f39d8f713e2fad32e0e5b1c8cd6de52f16960eeca41033709af`
  （含 env 门控的 595 计数器编译、本轮全部运行计数器 env 未设=关闭）。
  内核 7.2.11-fc44，liburing.so.2（LIBURING_2.0 符号），strace 由 `/usr/sbin/strace` 调用。
- 负载：`taskset -c 2`，`SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3
  SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2 SLUICE_BENCH_595_DEPTHS=1
  SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1
  SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench`，B 臂加 `SLUICE_595_ARM=B`（A 臂显式 unset）。
  PMU_ONLY=1 ⇒ 每 rep 一个主 pass（无 oracle/latency pass），strace 分段干净：
  setup 边界=pass 边界，每 invocation 4 段（1 warmup+3 measured），取末 3 段。
- 块结构（顺序平衡，8 invocation，全部 rc=0，loadavg<1.5 门通过）：
  `on-A1, on-B1, off-A1, off-B1, on-B2, on-A2, off-B2, off-A2`（`on`=strace）。
- strace 参数：`strace -f -ttt -T -e trace=io_uring_setup,io_uring_register,io_uring_enter,
  eventfd2,read,write,poll,ppoll,futex -o <file>`。
- shim：`LD_PRELOAD=/tmp/c2-shim/c2_liburing_shim.so SLUICE595_C2_SHIM_OUT=<base>`
  （源码已存档 `../raw-c2/c2_liburing_shim.c`；事件=kind+TSC+值，
  8M 上限，exit 时 dump `.bin/.meta`；运行脚本 `../raw-c2/run_shim_block.sh`）。shim 块同构：
  `shim-A1, shim-B1, noshim-A1, noshim-B1,
  shim-B2, shim-A2, noshim-B2, noshim-A2`。
- 时钟与丢失：shim TSC 每进程实测 `tsc_hz≈2,893,335,xxx`（6 次运行极差 <2 kHz，恒定 Tsc；
  与 C1 校准 2.893 GHz 一致）；`capped=0`（无事件丢失，100% 调用覆盖，非采样）。
  strace 为 ptrace 停止式（无丢失），实测 0 unfinished/resumed、enter 计数逐项守恒（§0.1）。
  strace `-ttt/-T` 用 CLOCK_REALTIME/gettimeofday（宿主 clocksource=tsc，恒定）；两种仪器
  时钟不同、未做跨仪器时间轴对齐——所有 A/B 比较均在同一仪器内进行。
- 已知仪器敏感度：B 的 defer/CQE 可见性处于时序刃口——strace（+8,000 ns/op 扰动）使
  B 通知路径塌缩（§0.4）、pass 级计数偏移（shim 下无 empty peek，peek 0.54–0.56/op vs
  Phase B cnt-on `poll_passes=0.79/op`）；**submit 侧计数在所有仪器下精确一致
  （每 20k op 恰 20,000 次 submit、ret≡1）**，本文件结论只取跨仪器一致的量。

## 3. 与 Phase B / C1 的对账

| 量 | Phase B（cnt-on 计数器） | 本轮 strace | 本轮 shim |
|---|---|---|---|
| enter/submit 每 op | `actual_enters=flushes=1.0000` | 1.0000 submit-only（80,005/invocation 守恒） | 20,000/20,000 每 rep |
| submit 返回 | 未测 | ret≡1（120,000×2 臂） | ret≡1（120,000×2 臂） |
| enter 参数 | 未测 | (1,0,0,NULL,8) 恒定，两臂相同 | —（syscall 参数不可见，由 strace 提供） |
| flush 位置 | 间接（poll ns/call 367→658、submit_attempts −31%） | 不可辨（B 被掩蔽） | **直接：29.93% submit 紧邻 peek（A=0%）** |
| eventfd write | 0→0.2353/op | B 每pass恰1（塌缩） | 不可见（write 不经 interposed 符号） |
| enter 时长 | —（flush TSC 702→714 ns/call） | B−A +67.7 [−154,+288] ns | B−A +37.9 [−33.7,+110.4] ns |

G4（"零 enter 节省"机制闭环）：**关闭其核心问题**——不存在跨请求 batching
（to_submit≡1、ret≡1、enter/op≡1），不存在部分提交或提交错误；deferred SQE 的
flush 位置迁移被原生速度直接观测（§0.3）。仍开放：逐 SQE dispatch→flush 延迟直方图
（需源码时间戳环；对 D2 成本模型非必要，见 EVIDENCE_GAPS G4 更新）。

## 4. 复现命令与文件

原始文件（`../raw-c2/`）：

- 观测核查：`obs-01-bpftrace-noroot.txt`、`obs-02-perf-ftrace-noroot.txt`、
  `obs-03-btf-tracepoint-structs.txt`、`obs-04-btf-syscall-prototype.txt`
- strace 块：`c2d2-{on,off}-{A,B}-{1,2}.{strace.txt,jsonl,stdout,stderr}`、`block-runlog.txt`
- shim 块：`c2shim-{shim,noshim}-{A,B}-{1,2}.{shim.bin,shim.meta,jsonl,stdout,stderr}`、
  `shim-runlog.txt`
- tail 子块（3-pass 模式测 strace 对 bench 自身尾延迟的影响）：
  `c2tail-{on,off}-{A,B}.{strace.txt,jsonl,stdout}`
- 分析：`analyze_c2_strace.py`→`c2-strace-analysis.json`；`analyze_c2_shim.py`→
  `c2-shim-analysis.json`；`dur_stats_c2.py`→`dur-stats.txt`；`shim_fractions.py`→
  `shim-fractions.txt`
- 冒烟：`smoke/`（含首轮误读 enter/op=3 的澄清证据：3-pass 结构 120,000 op/120,000 enter）

复现（顺序平衡 8 invocation 块，脚本逻辑内联于 `block-runlog.txt` 头部注释与本文 §2）：

```
env [SLUICE_595_ARM=B] SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
  SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2 SLUICE_BENCH_595_DEPTHS=1 \
  SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
  SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench SLUICE_BENCH_JSONL=<out>.jsonl \
  taskset -c 2 strace -f -ttt -T \
  -e trace=io_uring_setup,io_uring_register,io_uring_enter,eventfd2,read,write,poll,ppoll,futex \
  -o <out>.strace.txt <bench_w02_pipeline>
# shim 版：taskset 前 LD_PRELOAD=/tmp/c2-shim/c2_liburing_shim.so SLUICE595_C2_SHIM_OUT=<base>
```

## 5. 本轮未做 / 留给其他分支

- enter 内部内核路径（inline 完成、kernel 侧 eventfd 信号、io-wq 分派）→ C3。
- write(eventfd) 单次内核成本已在 C1 直接测量（592.5 ns/call on-CPU）；C2 无增量。
- 逐 SQE 时间戳环（G4 剩余）→ 如需再做源码探针，须单独 ON/OFF 块。
- sched 级 wakeup/runnable → C4。
