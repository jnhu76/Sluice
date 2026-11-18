# #621 P2 全链路剖析摘要（profile session）

- 日期：2026-11-18
- 战役 BASE：`5d94f233967c3aef993eb19ba9e3252c532b0951`
- 性质：**profile session**。本文件只做成本归因与收益上界，不产出任何“性能收益已成立”结论，不做 clean A/B，不替代语义 oracle / mutant / 完整矩阵。
- 纪律：严格串行，同一时间只运行一个 profile 命令；所有 perf/root 命令经 `sudo -S -p '' <命令> < /tmp/sluice621-sudo`（stdin 重定向，不读不记内容）。未修改产品代码；两个既有未跟踪目录 `docs/evidence/621-round2-R1/` 与 `docs/evidence/e5-fullpath-machine-work/` 全程未触碰。
- 原始数据根：`docs/evidence/621-progress-wait-path-20261118/p2/profile/`
- 本文件路径：`docs/evidence/621-progress-wait-path-20261118/p2/PROFILE-SUMMARY.md`

> **读数口径（先读）**：本文件中的“份额”是 perf 采样定位证据，**不是可直接删除的 cycles**。8 个生命周期阶段的自份额按“最深阶段 owner”每条样本只归一次，行和≈100%；inclusive 份额（任意调用帧出现该阶段）互相重叠，**不可相加**。非阶段桶（payload/FS、io-wq punt、shared sync、scheduler/IRQ、kernel syscall/FS overhead、syscall unattributed、bench harness、other）用于说明“这部分不是 Sluice 协调成本”或“无法归因”，也不能整体视为可删除。

---

## 0. 环境、二进制与符号化

### 0.1 环境冻结快照

采集前后快照：`p2/profile/env/snapshot-before.txt`、`snapshot-after.txt`。

| 项 | 实测 |
|---|---|
| kernel | `7.3.3-200.fc44.x86_64` |
| perf | `7.3.3-200.fc44.x86_64` |
| `perf_event_paranoid` | `2`（CPU-wide/kernel/tracepoint 采集全部以 root 执行） |
| CPU | 20 logical；governor 全部 `performance`（前后各 20/20） |
| irqbalance | `inactive`（前后一致） |
| `no_turbo` | `0` |
| clocksource | `tsc` |
| 温度 | 采集后 `thermal_zone0=51°C`；无节流记录 |
| 背景负载 | 采集后 loadavg `0.45 0.45 0.42`；无遗留 `perf`/uprobe（`perf probe -l` 计数 0） |
| git | HEAD = BASE；`git status --porcelain` 仅三个未跟踪 evidence 目录（本战役目录 + 两个既有目录） |

### 0.2 二进制与符号化

| 形态 | 二进制 | sha256 |
|---|---|---|
| P-A/P-B/P-D | `/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline` | `03dbef098d41e6d6a5280a46d46758ca7267326925ae95d1156ad8d8c3c6a896` |
| P-C | `/tmp/621-p1-build/ring/linux/x86_64/release/bench_w03_external_loop` | `43054fcd59f7000553d34a2f5b2909ff7ae6dae78910d4f379a8e8b0129937c1` |
| P-E | `/tmp/621-p1-build/copy/linux/x86_64/release/sluice-copy` | `db972b1946fcf2b282135ebdf600263b03492215b08846e867d856c6213fa0be` |

三个 release 二进制均为 stripped（无 `.symtab`、无 DWARF；`readelf -S` 仅 `.dynsym`/`.eh_frame`）。为解析采样 IP，使用同一 build 的 `.o` 与静态库按 xmake 链接顺序重新链接了**未 strip 的符号孪生**；孪生与 release 二进制的 `.text`、`.plt`、`.rodata`、`.eh_frame`、`.gcc_except_table` 逐节 sha256 全部 **MATCH**，地址布局一致。验证记录：`p2/profile/env/symbol-twin-verification.txt`；孪生 sha256 与符号表：`p2/profile/env/symbols-*.txt`。

- **采样对象仍是原始 release 二进制**；孪生只提供地址→符号映射。
- **行级归因 NOT_OBSERVABLE**：二进制无 DWARF；本文件只给符号级份额与源码函数坐标，不给采样行号。
- 内核符号来自会话内 dump 的 `/proc/kallsyms`（`/tmp/p2-621/kallsyms.txt`，438,448 行）；无 kernel-debuginfo，内核也只有函数级。

---

## 1. 形态、workload 与分母

所有 bench 形态使用 `SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7`、`SLUICE_BENCH_CASE_FILTER='=<case>'`、`SLUICE_BENCH_TMP_DIR=/tmp/621-p2-fixture`（tmpfs，64 MiB fixture，page-cache resident），`taskset -c 2,3`。每次进程调用包含 9 轮 × 3 个 phase（timed / oracle verify / latency）；每 phase 20,000 ops → **540,000 ops/进程**。per-op 计数按 540,000 归一。

| 形态 | 二进制 | 精确 case ID | backend / 形态 | 深度/worker | 介质 | 分母/运行 | 基线进程时长 |
|---|---|---|---|---|---:|---|---:|
| P-A | W-02 | `W02-uring-4k-d16` | io_uring，单驱动线程 | d16 / 0 | tmpfs fixture | 540,000 ops | 899.3 ms |
| P-B | W-02 | `W02-uring-4k-d1` | io_uring，单驱动线程 | d1 / 0 | tmpfs fixture | 540,000 ops | 2,398.9 ms |
| P-C | W-03 | `W03-uring-4k-d16` | io_uring，app-owned epoll split-owner | d16 / 0 | tmpfs fixture | 540,000 ops | 1,152.0 ms |
| P-D | W-02 | `W02-threadpool-4k-d16-w4` | ThreadPool | d16 / 4 | tmpfs fixture | 540,000 ops | 1,171.6 ms |
| P-E | sluice-copy | argv：`--buffer-size 4096 --pipeline-depth 4 --workers 1 --sync none --no-atomic <src> <dst>` | ThreadPool copy | d4 / 1 | src btrfs `/home/jnhu/sluice-621-r2/data/src-256m.bin`（sha256 `a1f8dd97…`）→ dst tmpfs | 65,536 chunks / 131,076 requests | 398.4 ms |

P-E 精确计数：`read_ops=65540`、`write_ops=65536`、`short_writes=0`；数据搬运单位为 **65536 个 4 KiB chunk**，每个 chunk 含 **2 个 request**（read + write）= 131,076 requests。本文件 P-E 的 per-op 数字默认按 **chunk** 归一；涉及 RequestCore 生命周期时另注 per-request（÷2）。

P-E 的 `block:block_rq_issue/complete` 全程 **0**，source 读命中 page cache；本文件不把 P-E 当作磁盘/NVMe/cold 结论。

---

## 2. PMU 计数（`perf stat -r 3`，无缩放）

原始文件：`P-*/raw/stat-pmu-r3.txt`、`stat-mem-r3.txt`。PMU 两遍均为 3 次运行取均值（`-r 3`）；tracepoint/uprobe 计数为单次运行。**全部 198 条 stat 行 `percent_enabled=100.00`，无 `<not counted>`/`<not supported>`/`scaled`**（`grep` 全量审计 0 命中）。`cycles:u` 与 `cycles:k` 分开两事件同组采集，未出现复用缩放。

| 形态 | cycles:u/op | cycles:k/op | cycles 合计/op | instr:u/op | instr:k/op | instr 合计/op | task-clock µs/op | ctx-sw/op | cache-miss/op | branch-miss/op |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| P-A d16 uring | 2,666.7 | 12,423.3 | **15,090.0** | 3,871.5 | 6,781.4 | 10,652.9 | 4.983 | 0.339 | 19.154 | 25.882 |
| P-B d1 uring | 5,248.0 | 23,455.2 | **28,703.2** | 6,676.6 | 17,565.1 | 24,241.7 | 11.793 | 2.000 | 23.320 | 43.677 |
| P-C W-03 d16 | 3,689.3 | 16,635.6 | **20,324.9** | 3,217.4 | 9,474.7 | 12,692.1 | 6.864 | 0.606 | 20.299 | 35.008 |
| P-D TP d16 w4 | 5,728.1 | 14,658.0 | **20,386.1** | 4,032.7 | 8,134.5 | 12,167.2 | 6.356 | 0.668 | 19.953 | 27.613 |
| P-E copy /chunk | 10,875.1 | 33,606.4 | **44,481.5** | 8,675.8 | 22,233.2 | 30,908.9 | 16.183 | 2.040 | 31.177 | 74.893 |
| P-E copy /request | 5,437.6 | 16,803.2 | 22,240.8 | 4,337.9 | 11,116.6 | 15,454.5 | 8.092 | 1.020 | 15.589 | 37.447 |

`stddev`（3 次运行的相对标准差，原文在 stat 文件）：P-A cycles:u 0.48%、cycles:k 4.80%、instr:k 8.05%、ctx-sw 25.53%；P-B 0.48/0.45/0.06/0.00；P-C 0.86/0.95/0.45/0.66；P-D **2.58**/0.40/0.58/0.68；P-E 1.57/0.75/3.26/0.00。P-D 的 cycles:u 与 P-A 的 instr:k、ctx-sw 波动较大，相关小份额不作强结论。

**用户/内核总账（PMU cycles）**：P-A 用户 17.7% / 内核 82.3%；P-B 18.3% / 81.7%；P-C 18.1% / 81.9%；P-D 28.1% / 71.9%；P-E 24.4% / 75.6%。内核侧始终是主体。

---

## 3. 每形态热点（perf record cycles，call graph）

记录方案：`perf record -e cycles -F 2999 -g --call-graph dwarf -- taskset -c 2,3 <binary>`；原始 `P-*/raw/record-cycles.data`，展开 `script-cycles-sym.txt`，报告 `report-cycles-dso-symbol.txt`。所有 cycles 记录 **`Total Lost Samples: 0`**。以下为**自份额（leaf）**，分母是该形态纳入 workload comm 的样本数。原始 perf.data 还包含 exec 前的 `taskset`/`sh`/`rm`/`mkdir` 样本；过滤后与原始的样本数为：P-A 8,369/8,390，P-B 24,585/26,484，P-C 16,472/16,490，P-D 15,182/15,199，P-E 34,736/35,385。

### P-A `W02-uring-4k-d16`（8,369 samples；single-frame 817）

| 份额 | 符号 | 类别 |
|---:|---|---|
| 11.94% | `rep_movs_alternative` | kernel，4 KiB payload 拷贝 |
| 6.36% | `_raw_spin_lock` | kernel，共享同步 |
| 3.81% | `entry_SYSRETQ_unsafe_stack` | kernel，syscall 返回 trampoline |
| 3.04% | `swapgs_restore_regs_and_return_to_usermode` | kernel，syscall 返回 |
| 2.83% | `shmem_file_read_iter` | kernel，tmpfs payload |
| 2.83% | `asm_sysvec_reschedule_ipi` | kernel，调度 IPI |
| 2.53% | `io_cq_unlock_post` | kernel，CQE 发布 |
| 1.88% | `__memcmp_avx2_movbe` | libc，bench verify |
| 1.86% | `__raw_spin_lock_irqsave` | kernel，共享同步 |
| 1.61% | `__io_read` | kernel，payload |
| 1.51% | `entry_SYSCALL_64` | kernel，syscall 入口 |
| 1.51% | `io_worker_handle_work` | kernel，io-wq punt |

Sluice 用户态自份额最高的函数：`UringAsyncBackend::dispatch_one_locked` 约 1.3%（`src/async/uring_transport.cpp:130-209`），`find_live_router_cookie_` 0.41%（`:551-557`），`RequestCore::*` 合计 0.88%。注意：`dispatch_one_locked` 的 1.3% 是用户态 leaf 份额；它的内核侧（SQE/enter）在 `submit` 与 `dispatch` 阶段。

### P-B `W02-uring-4k-d1`（24,585 samples；single-frame 2,751）

| 份额 | 符号 | 类别 |
|---:|---|---|
| 7.84% | `rep_movs_alternative` | kernel payload |
| 6.22% | `entry_SYSRETQ_unsafe_stack` | kernel syscall 返回 |
| 5.41% | `_raw_spin_lock` | kernel 共享同步 |
| 3.77% | `__raw_spin_lock_irqsave` | kernel 共享同步 |
| 2.10% | `__smp_call_single_queue` | kernel 调度/IPI |
| 2.06% | `syscall_return_via_sysret` | kernel syscall 返回 |
| 2.05% | `entry_SYSCALL_64` | kernel syscall 入口 |
| 1.96% | `psi_group_change` | kernel PSI/调度 |
| 1.66% | `io_wq_worker_running` | kernel io-wq |
| 1.63% | `shmem_file_read_iter` | kernel tmpfs payload |
| 1.54% | `io_wq_worker` | kernel io-wq |
| 1.53% | `io_cq_unlock_post` | kernel CQE 发布 |

Sluice 用户态：`AsyncIoContext::wait_one` 0.07%（`src/async/async_io_context.cpp:573-705`）；`pthread_mutex_lock` 1.53%、`pthread_mutex_unlock` 1.11%（libc，调用点分散在 wait/publish）。d1 的用户 leaf 很小，成本集中在内核 poll/wakeup/scheduler。

### P-C `W03-uring-4k-d16`（16,472 samples；single-frame 2,440）

| 份额 | 符号 | 类别 |
|---:|---|---|
| 6.87% | `entry_SYSRETQ_unsafe_stack` | kernel syscall 返回 |
| 6.78% | `rep_movs_alternative` | kernel payload |
| 5.15% | `_raw_spin_lock` | kernel 共享同步 |
| 3.96% | `__GI___pthread_mutex_unlock_usercnt` | libc 共享同步 |
| 3.60% | `pthread_mutex_lock@@GLIBC_2.2.5` | libc 共享同步 |
| 3.18% | `native_queued_spin_lock_slowpath` | kernel 共享同步 |
| 2.44% | `entry_SYSCALL_64` | kernel syscall 入口 |
| 2.22% | `syscall_return_via_sysret` | kernel syscall 返回 |
| 1.82% | `__raw_spin_lock_irqsave` | kernel 共享同步 |
| 1.59% | `asm_sysvec_reschedule_ipi` | kernel 调度 |
| 1.51% | `shmem_file_read_iter` | kernel payload |
| 1.44% | `psi_group_change` | kernel PSI |

Sluice 用户态：`UringAsyncBackend::dispatch_one_locked` 约 1.03%；`pthread_mutex_lock/unlock` 的 inclusive 份额 16.15%/16.24%，是 W-03 split-owner 形态最显眼的用户态同步族。

### P-D `W02-threadpool-4k-d16-w4`（15,182 samples；single-frame 4,138）

| 份额 | 符号 | 类别 |
|---:|---|---|
| 13.29% | `entry_SYSRETQ_unsafe_stack` | kernel syscall 返回 |
| 9.04% | `rep_movs_alternative` | kernel payload |
| 7.12% | `__GI___pthread_mutex_unlock_usercnt` | libc 共享同步 |
| 5.96% | `pthread_mutex_lock@@GLIBC_2.2.5` | libc 共享同步 |
| 4.69% | `entry_SYSCALL_64` | kernel syscall 入口 |
| 4.28% | `syscall_return_via_sysret` | kernel syscall 返回 |
| 2.50% | `native_queued_spin_lock_slowpath` | kernel 共享同步 |
| 2.24% | `ThreadPoolBackend::worker_loop()` | bench/产品 worker loop（`src/async/threadpool_backend.cpp:386`） |
| 1.63% | `_raw_spin_lock` | kernel 共享同步 |
| 1.50% | `psi_group_change` | kernel PSI |
| 1.47% | `shmem_file_read_iter` | kernel payload |
| 1.39% | `filemap_get_entry` | kernel page cache |

P-D/P-E 是 mutex 族最重的形态：`pthread_mutex_lock/unlock` 自份额合计分别为 13.1%/14.0%，inclusive 分别为 11.0%/21.0% 与 6.9%/10.8%。注意这些份额与 TP worker 的 `work_mtx_` 竞争、以及 worker_loop 的调度/等待重叠，不能整体当作可删除。

### P-E `sluice-copy-4k-d4-w1`（34,736 samples；single-frame 8,753）

| 份额 | 符号 | 类别 |
|---:|---|---|
| 13.28% | `entry_SYSRETQ_unsafe_stack` | kernel syscall 返回 |
| 7.60% | `__GI___pthread_mutex_unlock_usercnt` | libc 共享同步 |
| 6.35% | `pthread_mutex_lock@@GLIBC_2.2.5` | libc 共享同步 |
| 4.84% | `entry_SYSCALL_64` | kernel syscall 入口 |
| 4.40% | `syscall_return_via_sysret` | kernel syscall 返回 |
| 3.73% | `rep_movs_alternative` | kernel payload |
| 1.77% | `sluice_copy::run_pipelined_copy_with_context` | app copy driver（`apps/sluice-copy/copy_task.cpp:305-351`） |
| 1.48% | `clear_highpages_kasan_tagged` | kernel 内存分配 |
| 1.33% | `fput` | kernel 文件引用释放 |
| 1.17% | `ThreadPoolBackend::worker_loop()` | 产品 worker loop |
| 1.15% | `psi_group_change` | kernel PSI |
| 1.11% | `__smp_call_single_queue` | kernel 调度/IPI |

P-E 的 copy 主线程 `run_pipelined_copy_with_context` 自份额 1.77%，worker_loop 1.17%；数据路径内核侧（`rep_movs`/`shmem`/syscall trampoline）与 mutex 族占主体。

---

## 4. 阶段成本台账（self 份额）

**归因方法**：对每条 cycles 样本，从 leaf 向 root 扫描调用帧；若 leaf 是 payload/FS、io-wq、backend exec 等物理/非协调工作，按 leaf 分类；否则取**最深（最内层）出现 Sluice 阶段函数的帧**作为该样本的阶段 owner。每条样本只归一个阶段。`shared sync`/`scheduler` 等是“无阶段 owner”的兜底，不是阶段。阶段函数坐标见 §9。

| 阶段 / 桶 | P-A d16 | P-B d1 | P-C W-03 | P-D TP w4 | P-E copy |
|---|---:|---:|---:|---:|---:|
| admission（reserve/accept/submit_read） | 1.97% | 0.09% | 8.91% | 12.76% | 7.16% |
| dispatch（dispatch/queue/SQE prep） | 5.84% | 1.67% | 6.02% | 0.74% | 1.00% |
| submit（io_uring_enter/io_submit_sqes） | 21.35% | 10.91% | 12.49% | 0.00% | 0.00% |
| reap/CQE（reap/handle/router/seen） | 5.35% | 2.88% | 3.61% | 0.00% | 0.00% |
| terminal/release（offer/release/finalize） | 0.97% | 0.03% | 2.31% | 3.49% | 0.88% |
| publication（begin/complete/publish/deliver） | 1.46% | 0.07% | 2.82% | 4.13% | 3.30% |
| progress/wait（wait/poll/eventfd/drain） | 5.27% | **25.53%** | 16.29% | 3.52% | **25.12%** |
| observation（take/consume/lookup） | 1.60% | 0.05% | 1.77% | 2.05% | 1.74% |
| payload/FS（非阶段：rep_movs/filemap/shmem 等） | 19.87% | 13.28% | 11.45% | 13.85% | 10.44% |
| io-wq punt（非阶段：io_wq/io_worker/task_work） | 10.23% | 7.65% | 6.02% | 0.00% | 0.00% |
| backend exec (TP)（非阶段：worker_loop/pread/pwrite） | 0.00% | 0.00% | 0.00% | 2.38% | 1.35% |
| shared sync（非阶段：mutex/spinlock，无阶段 owner） | 5.10% | 7.43% | 5.43% | 13.27% | 7.86% |
| scheduler/IRQ（非阶段：EEVDF/PSI/IPI/futex wake） | 8.69% | 12.11% | 7.68% | 12.89% | 6.01% |
| kernel syscall/FS overhead（非阶段：fput/fdget/kmalloc 等） | 0.96% | 1.95% | 0.84% | 2.04% | 5.26% |
| **syscall unattributed**（入口/出口 trampoline，栈被截断） | 3.43% | 7.99% | 4.10% | 7.38% | 13.02% |
| bench harness（非阶段：run_*_phase/verify/memcmp） | 3.76% | 1.32% | 2.06% | 1.99% | 0.00% |
| other（未归类，EEVDF 残余/app 等） | 4.15% | 7.04% | 3.59% | 5.84% | 7.99% |

**不可相加/重叠说明**

0. 阶段 self 是**调用栈 owner 份额**，包含该阶段调用栈内的内核/调度/同步成本；它不是某个函数的指令级 self。P-B d1 的 admission/terminal/publication 数值很低，是因为这些函数被内联进 `wait_one`/`run_progress_pass_`，其开销被归到 progress/wait；**不要**用 P-B 的 0.09%/0.03%/0.07% 当作 d1 的 RequestCore 族成本（见 §11 W3 的 INCONCLUSIVE 标注）。
1. 上表 self 份额每条样本只归一次，行和≈100%；但 `submit` 只含 `io_uring_enter`/`io_submit_sqes` 等内部路径，**不含**通用 syscall 入口/出口 trampoline；后者单列为 `syscall unattributed`。因此“submit 的真实总 syscall 成本”应看 submit + 一部分 unattributed，但该拆分在本数据中不可精确解析。
2. inclusive 份额（任意帧出现该阶段）显著大于 self 且互相重叠：例如 P-A admission self 1.97% / inclusive 33.3%，submit self 21.35% / inclusive 28.0%，progress/wait self 5.27% / inclusive 10.6%；P-C admission self 8.91% / inclusive 32.9%；P-E progress/wait self 25.12% / inclusive 35.3%。**不得把 inclusive 行相加**。
3. `shared sync`、`scheduler/IRQ`、`kernel syscall/FS overhead` 是“未归到单一阶段”的桶；其中一部分可能属于某个阶段，但当前调用帧无法区分。它们不能整体视为可删除。
4. `payload/FS`、`io-wq punt`、`backend exec (TP)` 是产品路径之外的物理/后端工作；`io-wq punt` 在 tmpfs 形态为 1/op（见 §6），是 FS 条件性形态成本，不是 Sluice 协调机制。
5. `syscall unattributed` 在 TP/copy 形态高达 7.4–13.0%，原因是内核栈在 syscall 入口/出口被截断（见 §8 限制）；这部分不能强行分给 pread/pwrite 或 futex。

---

## 5. Off-CPU（`sched:sched_switch` + callchain）

方案：`perf record -e sched:sched_switch -g --call-graph dwarf -m 128M`，为控制开销使用 `SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1`（一个 measured round；P-E 为一次 256 MiB copy）。全部 **`Total Lost Samples: 0`**。样本按调用帧中的阻塞原语分类；`iou-wrk` 与 app 线程分列。

| 形态 | 样本 | 主要阻塞原语（count / share） | 说明 |
|---|---:|---|---|
| P-A d16 | 15,013 | io-wq worker idle 15,004（99.9%，iou-wrk）；io_uring cqring wait 4；thread create 2 | d16 下 bench 线程很少真正 park；`wait_one` 仅 0.0625/op，`poll` 0/op。off-CPU 主体是 io-wq worker 空闲。 |
| P-B d1 | 119,969 | poll wait 59,947（50.0%，app）；io-wq worker idle 60,010（50.0%，iou-wrk）；cqring wait 4 | d1 下 app 线程每 op 在 eventfd `poll` 上 park；与 `poll=1/op`、`eventfd read=1/op`、`wait_one=1/op` 对账。 |
| P-C W-03 | 31,697 | io-wq worker idle 14,193（44.8%，iou-wrk）；futex wait 12,829（40.5%，app，`futex_do_wait`）；epoll wait 4,645（14.7%，app） | split-owner：submitter 在 condvar futex 等完成，driver 在 epoll 等 eventfd；`futex=1.53/op`、`epoll_wait=0.111/op`。 |
| P-D TP w4 | 32,283 | futex wait 24,171（74.9%，app，`futex_do_wait`）；scheduler preempt/return 6,613（20.5%）；poll 798（2.5%） | TP worker/主线程的 work_mtx/condvar 等待；`futex=2.79/op`。scheduler 类样本是返回用户态时的抢占/调度，不是 futex 本身。 |
| P-E copy | 166,425 | poll wait 83,481（50.2%，app）；futex wait 82,940（49.8%，app，`futex_do_wait`） | copy 主线程在 ProgressSource poll 等完成，worker 在 futex/condvar 等 work；`poll=1.32/chunk`、`futex=3.84/chunk`。 |

**口径限制**：off-CPU 计数是 `sched_switch` **事件数**，不是阻塞时长；样本窗口只有一个 measured round，不能当作全进程时间占比。per-thread tracepoint 的 `sched_switch`/`sched_wakeup` 是 CPU-local 事件，`sched_wakeup` 由 waker 线程触发时可能不在被监控线程的事件里（例如 P-B wakeup 仅 110，而 switch 2/op）；进程级 context-switch 以 PMU `context-switches` 为准（P-B 2.000/op、P-E 2.040/chunk）。

---

## 6. Tracepoint 计数

原始：`P-*/raw/stat-tp-*.txt`。值为整个进程 540,000 ops（P-E 65536 chunks）的单次计数；`percent_enabled=100.00`。**注意**：per-thread tracepoint 的 `sched_*` 计数是 CPU-local，不等于进程级事件数；stat 计数与 tracepoint record 计数会因扰动不同，不能混用（见 §7）。

| 事件 | P-A d16 | P-B d1 | P-C W-03 | P-D TP w4 | P-E copy |
|---|---:|---:|---:|---:|---:|
| `io_uring_submit_req` | 540,028 / 1.000/op | 540,028 / 1.000 | 540,028 / 1.000 | 1（启动残差） | 0 |
| `io_uring_complete` | 540,028 / 1.000 | 540,028 / 1.000 | 540,028 / 1.000 | 1 | 0 |
| `io_uring_queue_async_work` | 540,028 / 1.000 | 540,028 / 1.000 | 540,028 / 1.000 | 1 | 0 |
| `io_uring_task_work_run` | 421,186 / 0.780 | 540,028 / 1.000 | 421,977 / 0.781 | 1 | 0 |
| `io_uring_cqring_wait` | 28 | 28 | 28 | 1 | 0 |
| `sys_enter_io_uring_enter` | 540,028 / 1.000 | 540,028 / 1.000 | 540,028 / 1.000 | 1 | 0 |
| `sys_enter_poll` | 0 | 539,964 / 1.000 | 0 | 5,763 / 0.011 | 86,309 / 1.317/chunk |
| `sys_enter_futex` | 2 | 2 | 825,214 / 1.528 | 1,508,402 / 2.793 | 251,489 / 3.837/chunk |
| `sys_enter_read` | 25 | 539,989 / 1.000 | 59,923 / 0.111 | 9,000 / 0.017 | 68,634 / 1.047/chunk |
| `sys_enter_write` | 11 | 11 | 38 | 9,015 / 0.017 | 68,624 / 1.047/chunk |
| `sys_enter_pread64` | 8 | 8 | 8 | 540,008 / 1.000 | 65,548 / 1.000/chunk |
| `sys_enter_pwrite64` | 64 | 64 | 64 | 64 | 65,536 / 1.000/chunk |
| `sys_enter_epoll_wait` | — | — | 59,900 / 0.111 | — | — |
| `sched_switch` | 422,753 / 0.783 | 1,080,001 / 2.000 | 559,700 / 1.036 | 413,674 / 0.766 | 132,935 / 2.028/chunk |
| `sched_wakeup` | 184,791 / 0.342 | 110 / 0.000 | 281,696 / 0.522 | 238,490 / 0.442 | 13 / 0.000 |
| `block:block_rq_issue/complete` | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

解读要点：

- **P-A d16 不阻塞在 ProgressSource poll**：`poll=0`、`read=25`、`wait_one=0.0625/op`；每 op 1 次 `io_uring_enter`，1 次 submit/complete，1 次 `queue_async_work`（tmpfs punt），`task_work_run=0.78/op`。
- **P-B d1 是等待路径最热形态**：`poll=1/op`、eventfd `read=1/op`、`wait_one=1/op`、`context-switches=2/op`；`io_uring_enter` 仍 1/op。
- **P-C split-owner**：`io_uring_enter=1/op`；driver `epoll_wait=0.111/op`、eventfd `read=0.111/op`；submitter `futex` 在两次 trace 方案中为 **1.05–1.53/op**（sched/syscall 组 1.05，epoll 组 1.53；方案间扰动，见 §7），量级为每 op 一次以上。
- **P-D TP**：数据路径 `pread64=1/op`；`futex=2.79/op`；`poll` 仅 0.011/op（d16 下 wait_one 0.093/op，只有少数阻塞等待）。
- **P-E copy**：数据路径 `pread/pwrite=1/chunk`；ProgressSource `poll=1.317/chunk`、eventfd `read/write≈1.047/chunk`；worker/futex `futex=3.837/chunk`；block 层 0（page cache）。

---

## 7. 采集开销与事件丢失

### 7.1 事件丢失

所有 `perf record` 数据的 `perf report --stdio` 头部均为 **`Total Lost Samples: 0`**（逐文件回执：`P-*/raw/lost-record-*.txt`，共 15 个 record 数据文件，15/15 为 0）：

| 数据 | 样本数 | Lost |
|---|---:|---:|
| P-A cycles | 8,390 | 0 |
| P-A tp-uring | 2,049,970 | 0 |
| P-A off-CPU | 15,014 | 0 |
| P-B cycles | 26,484 | 0 |
| P-B tp-uring | 2,160,112 | 0 |
| P-B off-CPU | 119,971 | 0 |
| P-C cycles | 16,490 | 0 |
| P-C tp-uring | 2,088,705 | 0 |
| P-C off-CPU | 31,698 | 0 |
| P-D cycles | 15,199 | 0 |
| P-D tp-sched/syscall | 2,634,851 | 0 |
| P-D off-CPU | 32,284 | 0 |
| P-E cycles（5 copies） | 35,385 | 0 |
| P-E tp | 907,509 | 0 |
| P-E off-CPU | 166,426 | 0 |

tracepoint record 的事件数与 stat 计数会因扰动不同（例：P-C futex stat 825,214 vs record 437,000；P-D futex stat 1,508,402 vs record 1,000,000），**不是丢失**，是 record 自身改变了执行/批形。计数结论以 `perf stat` 为准，record 只用于丢失率与事件流。逐 record 的丢失率 = `0 / 样本数 = 0.0%`（15/15 文件）。

### 7.2 采集附加开销（进程级运行时长比，基线=1.00）

基线取 `stat-pmu-r3` 的运行时（3 次均值）。`record-cycles` 取 `perf report --header-only` 的 sample duration；P-E 的 cycles record 是 5 次 copy，已按 1/5 折算。off-CPU 的 header duration 只覆盖首末 sched_switch，不是进程时长，**不作开销比**。

| 形态 | 基线 ms | tracepoint stat（最重组） | tracepoint record | cycles record | uprobe 组 |
|---|---:|---:|---:|---:|---:|
| P-A d16 | 899.3 | **3.41×**（tp-uring）/ 3.33× / 3.01× | 2.34× | 1.59× | 109.9× / 72.4×；waitpath 4.4× |
| P-B d1 | 2,398.9 | **2.29×** / 2.25× / 2.13× | 2.42× | 2.15× | 64.5× / 50.0×；waitpath 3.0× |
| P-C W-03 | 1,152.0 | **3.58×** / 3.63× / 3.68× / 3.63×（epoll） | 2.61× | 2.06× | 11.0× / 11.3× |
| P-D TP w4 | 1,171.6 | **3.00×** / 3.08× / 2.90× | 2.05× | 1.57× | 8.0× / 5.5× |
| P-E copy | 398.4 | **2.37×** / 2.62× / 3.21× | 2.78× | **1.93×**（5 copy 折算） | 77.7× / 48.7× |

- tracepoint stat 的进程级开销约 **2.0–3.7×**；`perf record` cycles 约 **1.6–2.2×**（P-E 1.93×/copy）；tracepoint record 约 **2.0–2.8×**。
- uprobe 开销 **5–110×**（每次 uprobe 命中约几十 µs 的 breakpoint 处理）；uprobe 的 **调用计数可信**，但该 run 的 wall/CPU/调度行为被严重扰动，不得用于性能判断。
- 所有 stat 行 100% enabled，无缩放；`perf record` 无 lost；因此本文件的**份额**可用于归因，**时长/吞吐**不可作为 A/B 或收益证据。
- JSONL measured-phase wall 比值因 run 间调度/扰动而噪（个别 <1，如 P-B tracepoint），本文件不把它作为开销或收益结论。

---

## 8. Uprobe / kprobe 计数

方法：`perf probe -x <release binary> --add <name>=<file offset>`；uprobe 的 file offset = 符号虚拟地址 − `0x400000`（ET_EXEC，`.text` vaddr `0x400940` ↔ file off `0x940`）。用 `perf stat -a -e probe_...` 计数；**uprobe 只支持 system-wide**，per-thread 会报 `Invalid event`。所有 probe 在会话结束前删除（`perf probe -l` 计数 0）。

| 探针 | P-A d16 | P-B d1 | P-C W-03 | P-D TP w4 | P-E copy |
|---|---:|---:|---:|---:|---:|
| `RequestCore::reserve` | 1/op | 1/op | 1/op | 1/op | 2/chunk |
| `RequestCore::accept` | 1/op | 1/op | 1/op | 1/op | 2/chunk |
| `RequestCore::offer_terminal` | 1/op | 1/op | 1/op | 1/op | 2/chunk |
| `RequestCore::release_execution` | 1/op | 1/op | 1/op | 1/op | 2/chunk |
| `RequestCore::begin_publication` | 1/op | 1/op | 1/op | 1/op | 2/chunk |
| `RequestCore::complete_publication` | 1/op | 1/op | 1/op | 1/op | 2/chunk |
| `AsyncIoContext::wait_one` | 0.0625/op（短窗） | **1.000/op（短窗）** | 0.0795/op（短窗） | 0.0933/op（短窗） | 1.122/chunk（单次 copy） |
| `run_progress_pass_` | 0.0626/op（短窗） | 1.0005/op（短窗） | 0.0795/op（短窗） | 0.0979/op（短窗） | 2.122/chunk（单次 copy） |
| `AsyncIoContext::poll` | 0 | 0 | 0 | 0 | — |
| `find_live_router_cookie_` | 1/op | 1/op | 1/op | 0 | — |
| `find_live_router_index_` | 0 | 0 | 0 | 0 | — |
| `ThreadPoolBackend::worker_loop` | 0 | 0 | 0 | 12（短窗，4 worker × 3 phase） | 1 |

- `wait_if_unchanged` 只存在 `[clone .part.0]` 符号（P-A `0x415ed0`），uprobe 命中 **0**；实际等待路径由 `wait_one`/`run_progress_pass_` 与 syscalls 计数，不能把该 0 当作“等待路径未执行”。
- `ProgressSource::signal`、`drain_notification_nolock_`、`PhysicalProbe` 在 release 符号中**不存在**（内联/优化掉）→ **NOT_OBSERVABLE**（符号级）。等待/通知路径以 `io_eventfd_signal`、`eventfd_read`、`poll`、`wait_one` 计数替代。
- P-E 的 RequestCore 方法为 **2/chunk**（read+write 两个 request）；`e_wait_one` 1.12/chunk、`e_pass` 2.12/chunk。
- P-D 的 `worker_loop` 12 次是短窗 1 round × 3 phase × 4 worker 的线程入口；不是 per-op 成本。

---

## 9. 源码坐标（符号级；行级 NOT_OBSERVABLE）

| 符号 | 坐标 |
|---|---|
| `UringAsyncBackend::dispatch_one_locked` | `src/async/uring_transport.cpp:130-209` |
| `UringAsyncBackend::submit_transport_locked` | `src/async/uring_transport.cpp:218-235` |
| `UringAsyncBackend::find_live_router_cookie_` | `src/async/uring_transport.cpp:551-557` |
| `UringAsyncBackend::handle_one_cqe` | `src/async/uring_completion.cpp:99-198` |
| `UringAsyncBackend::reap_cqes` | `src/async/uring_completion.cpp:200-278` |
| `UringAsyncBackend::finalize_operation_terminal_` | `src/async/uring_completion.cpp:23-97` |
| `UringAsyncBackend::publish_one` | `src/async/uring_backend.cpp:578-608` |
| `UringAsyncBackend::poll_progress` | `src/async/uring_backend.cpp:304-328` |
| `RequestCore::reserve/accept` | `src/async/detail/request_core.cpp:165-182` / `184-210` |
| `RequestCore::offer_terminal/claim_execution/release_execution` | `src/async/detail/request_core.cpp:346-365` / `367-378` / `393-409` |
| `RequestCore::begin_publication/complete_publication` | `src/async/detail/request_core.cpp:438-460` / `462-484` |
| `ProgressSource::wait_if_unchanged` / `::poll` / `drain_notification_nolock_` | `include/sluice/async/detail/progress_source.hpp:104-232` / `198-203` / `414-460` |
| `AsyncIoContext::wait_one` / `run_progress_pass_` | `src/async/async_io_context.cpp:573-705` / `429-446` |
| `ThreadPoolBackend::worker_loop` | `src/async/threadpool_backend.cpp:386` |
| `sluice_copy::run_pipelined_copy_with_context` | `apps/sluice-copy/copy_task.cpp:305-351` |

---

## 10. 请求身份映射分类（不得伪造逐 Request 因果链）

| 分类 | 本会话的落点 | 说明 |
|---|---|---|
| **aggregated** | §4 阶段 self 表、§2 PMU per-op、§3 热点 | 全 run 聚合；没有逐 Request 的 cycles 归属。 |
| **correlated** | §6/§8 的每 op 计数（submit=complete=1/op、enter=1/op、pread=1/op、wait_one=1/op 等） | 事件计数与生命周期阶段相关，可用来确认某阶段每 op 被执行；**不是**逐请求因果链，也不能把 1 次事件换算成该请求的 cycles。 |
| **ambiguous** | `syscall unattributed` 3.4–13.0%、`shared sync`、`scheduler/IRQ`、io-wq worker idle | 内核栈在入口/出口截断，或调用栈含多个阶段/无产品帧；无法归给单一 Request/阶段。 |

本会话**没有**建立 submit→complete 的逐请求配对（tracepoint 的 `user_data` 可支持该配对，但本轮未做）；因此不报告逐请求延迟/成本，也不把阶段份额写成“每个请求的因果成本”。

---

## 11. 候选收益上界（W1 / W3 / M-53 / M-42）

> 上界是**测量份额/计数算术**，不是预期收益；上界内部互相重叠，不能相加；未做单因素 A/B，不能据此声称任何候选 GO。

### W1 — ProgressSource poll 等待路径

| 形态 | progress/wait self | poll 相关 inclusive | 依据 |
|---|---:|---:|---|
| P-B d1（最热） | **25.53%** | `__x64_sys_poll` 15.77%、`do_sys_poll` 15.38%、`poll_schedule_timeout` 11.12%、`io_eventfd_signal` 8.67%、`wait_one` 2.85% | `stat-pmu` + `script-cycles-sym`；poll=1/op、eventfd read=1/op、ctx-sw=2/op、off-CPU poll wait 59,947 |
| P-E copy | **25.12%** | `__x64_sys_poll` 11.46%、`do_sys_poll` 11.43%、`poll_schedule_timeout` 8.43% | poll=1.317/chunk、off-CPU poll 83,481；`wait_one` inclusive 61.94%（与 poll 栈重叠，不是 poll 专属，见下） |
| P-A d16 | 5.27% | poll=0 | d16 下 wait_one 0.0625/op，等待路径不是热点 |
| P-C W-03 | 16.29% | poll=0；epoll_wait=0.111/op | split-owner 用 epoll，不是同一 `poll(-1)` 路径 |

- **上界**：P-B 的整个 progress/wait 阶段 ≤**25.5%** 进程 cycles；若只算包含 `__x64_sys_poll` 的调用栈，≤**15.8%** inclusive。P-E 类似 ≤25.1%。
- **扣除项**：阻塞 eventfd read 仍需等待/唤醒/eventfd 信号；`io_eventfd_signal` 8.67%、`eventfd_read` 0.46%、wakeup/scheduler 成本不会全部消失。因此可删除 cycles 必然显著低于上界；历史 R1 的 5.0–5.5% 线索落在此上界内，但本轮**未证实**。
- **置信等级**：份额 MEASURED（P-B/P-E）；候选可删除量 **PARTIAL/UNKNOWN**；P-A/P-C 不支持该候选为热点。

### W3 — RequestCore mutex/生命周期族

**族级 self 上界**（admission + terminal/release + publication + observation）：

| 形态 | 合计 self | 备注 |
|---|---:|---|
| P-A d16 | 6.00% | 单线程 submit；RequestCore 符号 inclusive 2.80% |
| P-B d1 | 0.24% | 严重嵌套进 `wait_one`，RequestCore 帧被内联/归入 progress/wait；**不能**把 0.24% 当作族成本 |
| P-C W-03 | 15.81% | admission 8.91 + terminal 2.31 + publication 2.82 + observation 1.77；pthread mutex inclusive 16.2% |
| P-D TP w4 | 22.43% | admission 12.76 + terminal 3.49 + publication 4.13 + observation 2.05；mutex inclusive 11–21% |
| P-E copy | 13.08% | admission 7.16 + terminal 0.88 + publication 3.30 + observation 1.74；RequestCore inclusive 6.23% |

**逐候选（self 份额，跨形态 P-A/P-B/P-C/P-D/P-E）**：

| 候选 | 对应阶段 | 上界 |
|---|---|---|
| M-61 `begin`/`complete` 两锁融合 | publication | 1.46% / 0.07% / 2.82% / **4.13%** / 3.30% |
| M-62 无观察者跳过 claim/retire | publication + `claim_execution` | publication 同上；`claim_execution` self 0.12% / 0.00% / 0.07% / **0.61%** / 0.40% |
| M-65 owed `release_control` 第三锁融合 | terminal/release | 0.97% / 0.03% / 2.31% / **3.49%** / 0.88% |
| M-67 sticky 双标志 | progress/wait | 5.27% / **25.53%** / 16.29% / 3.52% / **25.12%** |
| M-10 零长发布三锁融合 | publication + terminal | 零长 op **未在本 workload 出现** → **NOT_OBSERVABLE**；上表 publication+terminal 是形态上界，不是零长成本 |

- **置信等级**：阶段 self 是 MEASURED；单候选的“步间语义 + 融合后仍保留的锁”未测，逐候选可删除量 **PARTIAL**；P-B d1 的族成本与 progress/wait 不可分离，W3 在 d1 的上界 **INCONCLUSIVE**。r6 的 9–11% 族级线索与本次 P-C/P-D 的 15–22% 形态量级相容，但不能当单候选收益。

### M-53 — cookie O(R) 逆向扫描 → `user_data` 路由索引

| 形态 | `find_live_router_cookie_` self | inclusive | cycles/op（self × 总 cycles/op） |
|---|---:|---:|---:|
| P-A d16 | 0.41% | 0.59% | ≈61.9 cyc/op |
| P-C d16 | 0.29% | 0.29% | ≈58.9 cyc/op |
| P-B d1 | 0.01% | 0.01% | ≈2.9 cyc/op |
| P-D/P-E | 0（无 uring） | 0 | 0 |

- **上界**：d16 形态 ≤**0.6%** 进程 cycles / ≤**62 cycles/op**（self）；d1 几乎为 0（router 表仅 1 项）。
- **d64 NOT_OBSERVABLE**：本轮未采集 d64；P1-A 历史 d64 1.74% / ≈116 cyc/op 只能作线索，不能外推。
- **置信等级**：d16 MEASURED；d64 未测；M-53 的三项迁移义务（TransportLedger 位置单调、控制 CQE exact-submitted、cancel 定向）未在 profile 中验证。

### M-42 — `submit_flushes_` 只写不读原子计数

- 机制：`src/async/uring_transport.cpp:222` 在 ledger 空早退之后 `fetch_add`，每 submit 1 次 RMW；唯一读者在测试宏内（`uring_test_seams.hpp:75-76`）。
- 每 op 总 instructions：P-A 10,652.9、P-B 24,241.7、P-C 12,692.1、P-D 12,167.2、P-E 30,908.9/chunk。1 条 instruction/submit 的份额：P-A 0.0094%、P-B 0.0041%、P-C 0.0079%、P-D 0.0082%、P-E ≈0.0065%（2 submit/chunk）。
- 按 1 次 xadd 约 3–5 cycles、以实测 cycles/op 为分母：P-A 0.020–0.033%、P-B 0.010–0.017%、P-C 0.015–0.025%、P-D 0.015–0.025%、P-E 0.007–0.011%。
- **上界**：≤**0.04%** 进程 cycles（保守取整）；远低于 P1 A/A 的 16 对噪声带（P1 已将 M-42 记为 `NOT_POWERED`）。profile 无法单独观测该内联 RMW 的 cycles，本上界来自“1 条指令 × 实测每 op 总量”的算术。
- **置信等级**：上界算术；精确效应 **NOT_OBSERVABLE / NOT_POWERED**。

---

## 12. 限制与 NOT_OBSERVABLE 清单

1. **行级归因 NOT_OBSERVABLE**：release 二进制 stripped、无 DWARF；符号来自逐节 sha256 匹配的未 strip 孪生。内核亦无 debuginfo，只有函数级。
2. **采样开销**：DWARF callgraph 使进程时长 1.6–2.2×；tracepoint stat 2.0–3.7×；uprobe 5–110×。份额可用于归因，时长/吞吐不可用于收益判断。
3. **syscall unattributed 3.4–13.0%**：内核栈在 syscall 入口/出口截断；不能强行分给 pread/pwrite/futex/poll。
4. **shared sync / scheduler / kernel syscall-FS overhead**：无阶段 owner，不能整体视为可删除；其中一部分是真实必要同步/调度/内核工作。
5. **single-frame 样本**：P-A 817/8,369、P-B 2,751/24,585、P-C 2,440/16,472、P-D 4,138/15,182、P-E 8,753/34,736；这些样本只有 leaf，阶段归因只到 leaf。
6. **sched tracepoint 语义**：per-thread `sched_switch/sched_wakeup` 是 CPU-local，不等于进程级事件；进程级 context-switch 用 PMU。off-CPU 计数是切换事件数，不是阻塞时长；off-CPU 窗口只有一个 measured round。
7. **介质**：bench 是 tmpfs 64 MiB page-cache resident；P-E 是 btrfs page-cache warm → tmpfs，block 层 0 事件。本文件没有磁盘/NVMe/cold/btrfs 写回结论。
8. **未覆盖形态**：d64、64K、zero-op、cancel、saturation、TP w1/w8、W-03 d4/d64 未采集；不得外推。M-10（零长）因此 NOT_OBSERVABLE。
9. **P-E 基线差异**：本轮 P-E 单次 398 ms，快于 P1 C1 arm 的 867 ms（source 已 warm，且 profile 窗口不带 runner 的 per-arm 哈希/进程开销）；只作形态归因，不与 P1 数字作 A/B。
10. **请求级因果链缺失**：未做 submit→complete 逐请求 join；阶段表是 aggregated，事件计数是 correlated，截断栈是 ambiguous。没有伪造逐请求成本。
11. **函数份额不是可删除 cycles**：`pthread_mutex_lock` 13%、`submit` 21% 等份额包含必要同步/内核机器；上界不是收益预测，且 W1/W3/M-53/M-42 上界互相重叠、不可相加。
12. **uprobe 0 命中**：`wait_if_unchanged` 的 `.part.0` clone 0 命中；`signal`/`drain_notification_nolock_`/`PhysicalProbe` 无符号。等待路径以 `wait_one`/`run_pass`/syscalls 计数，不能把 0 当“未执行”。
13. **PMU 与采样 DSO 拆分的差异**：例如 P-A 的 PMU cycles:k 占 82.3%，cycles record 的 kernel 样本占 84.7%；P-B 为 81.7% vs 92.5%（io-wq 继承/采样偏差更大）。PMU 用于总量与用户/内核总账，record 只用于相对热点；两者不能互相替代。
14. **perf stat 计数包含 3 个 phase**：timed + oracle verify + latency，不是只 measured timed phase；JSONL 的 measured-only wall 仅作辅助，per-op cycles 统一按 540,000（或 P-E 65,536 chunk）归一。
15. **tracepoint stat 与 tracepoint record 计数不同**：record 自身改变执行/批形（如 P-C futex 825k vs 437k）；stat 计数用于 per-op，record 只用于丢失率。
16. **无 A/B / 无语义门**：本轮没有候选 build、mutant、A/A；任何“收益”必须另开单因素 A/B，并按 campaign ledger §9 的语义门与统计门执行。

---

## 13. 原始数据索引与复现

### 13.1 目录

```
p2/profile/
  env/
    snapshot-before.txt, snapshot-after.txt
    symbol-twin-verification.txt
    symbols-bench_w02.txt, symbols-bench_w03.txt, symbols-sluice-copy.txt
  analysis/
    analysis.json, analysis.txt, analyze_p2.py, SHA256SUMS
  P-A-w02-uring-4k-d16/raw/
  P-B-w02-uring-4k-d1/raw/
  P-C-w03-uring-4k-d16/raw/
  P-D-w02-tp-4k-d16-w4/raw/
  P-E-sluice-copy-4k-d4-w1/raw/
```

每个 `raw/` 含：

- `stat-pmu-r3.txt`、`stat-mem-r3.txt`：PMU 计数（`-x,`，`percent_enabled` 列）。
- `stat-tp-*.txt`：tracepoint 计数；P-C 另有 `stat-tp-epoll.txt`。
- `stat-uprobe-*.txt`：uprobe 计数（`-a`）。
- `record-cycles.data`、`record-tp*.data`、`record-offcpu.data`：perf.data 原始数据。
- `header-*.txt`：`perf report --header-only`（含 sample duration）。
- `lost-record-*.txt`：`perf report --stdio` 前 6 行（含 `Total Lost Samples` 回执）。
- `report-cycles-dso-symbol.txt`：`perf report --sort dso,symbol` 原始报告。
- `script-cycles.txt`、`script-cycles-sym.txt`、`script-offcpu.txt`、`script-offcpu-sym.txt`：`perf script` 展开；**`*-sym.txt` 是本分析使用的最终版本**（含 kernel/libc 符号与 bench IP）。
- `jsonl-*.jsonl`：bench JSONL；P-E 无 JSONL（copy 只输出 stdout）。

### 13.2 复现命令模板

```bash
# 公共
BIN=/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline
D=docs/evidence/621-progress-wait-path-20261118/p2/profile/P-A-w02-uring-4k-d16/raw
SUDO='sudo -S -p ""'

# PMU pass 1（用户/内核分开，6 事件，-r 3）
$SUDO env SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
  SLUICE_BENCH_CASE_FILTER='=W02-uring-4k-d16' \
  SLUICE_BENCH_JSONL=$D/jsonl-pmu-r3.jsonl \
  SLUICE_BENCH_TMP_DIR=/tmp/621-p2-fixture \
  perf stat -r 3 -x, -e cycles:u,cycles:k,instructions:u,instructions:k,task-clock,context-switches \
  -o $D/stat-pmu-r3.txt -- taskset -c 2,3 $BIN < /tmp/sluice621-sudo

# PMU pass 2（cache/branch/migrations）
$SUDO env ... perf stat -r 3 -x, -e cache-misses,branch-misses,cpu-migrations,task-clock \
  -o $D/stat-mem-r3.txt -- taskset -c 2,3 $BIN < /tmp/sluice621-sudo

# cycles + call graph
$SUDO env ... perf record -o $D/record-cycles.data -e cycles -F 2999 -g --call-graph dwarf \
  -- taskset -c 2,3 $BIN < /tmp/sluice621-sudo

# tracepoint stat（示例 io_uring 组）
$SUDO env ... perf stat -x, \
  -e io_uring:io_uring_submit_req,io_uring:io_uring_complete,io_uring:io_uring_cqring_wait,io_uring:io_uring_queue_async_work,io_uring:io_uring_task_work_run,io_uring:io_uring_cqe_overflow \
  -o $D/stat-tp-uring.txt -- taskset -c 2,3 $BIN < /tmp/sluice621-sudo

# off-CPU（一个 measured round）
$SUDO env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 ... \
  perf record -o $D/record-offcpu.data -e sched:sched_switch -g --call-graph dwarf -m 128M \
  -- taskset -c 2,3 $BIN < /tmp/sluice621-sudo

# uprobe（file offset = vaddr - 0x400000；只支持 -a）
$SUDO perf probe -x $BIN --add p621_wait_one=0x17d50
$SUDO env ... perf stat -a -x, -e probe_bench_w02_pipeline:p621_wait_one \
  -o $D/stat-uprobe-waitpath.txt -- taskset -c 2,3 $BIN < /tmp/sluice621-sudo
$SUDO perf probe -d p621_wait_one

# 展开
$SUDO perf script -i $D/record-cycles.data -F comm,pid,ip,dso,sym --no-inline > $D/script-cycles-sym.txt
$SUDO perf script -i $D/record-offcpu.data -F comm,pid,ip,dso,sym --no-inline > $D/script-offcpu-sym.txt

# 汇总（一次性分析脚本，非产品/框架）
python3 -I docs/evidence/621-progress-wait-path-20261118/p2/profile/analysis/analyze_p2.py
```

P-E 的 wrapper 为 `sh -c 'rm -f /tmp/621-p2-dst.bin; exec "$@"' sh taskset -c 2,3 sluice-copy ... <src> /tmp/621-p2-dst.bin`；cycles record 为 5 次 copy 循环（每次先 `rm -f` dst）。

### 13.3 已知未运行/未验证

- 未运行 `xmake test`、语义 oracle、mutant、A/A、A/B、TLC、`check_run.py`、`run_matrix.sh`。
- 未对 `perf.data` 做逐请求 join；未测 d64/64K/zero-op/cancel/saturation/w1/w8。
- 未做内核 debuginfo 行级归因；未做 cgroup/IRQ 亲和性逐 IRQ 记录（环境快照只有 irqbalance inactive）。
- 未对 P-E 做 btrfs 冷缓存、drop_caches、fsync/durability 形态。

---

*本文件结束。数字来自 `p2/profile/analysis/analysis.json` 与各 `raw/` 原始文件；任何引用请同时给出形态、分母（540000 ops / 65536 chunks）和“份额≠可删 cycles”的口径。*
