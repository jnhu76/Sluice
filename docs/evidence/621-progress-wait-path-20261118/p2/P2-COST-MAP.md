# P2-COST-MAP — #621 Progress/Wait 路径机器工作成本地图

| 字段 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| 战役 BASE | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| 性质 | **profile session 的成本归因汇总**。不是 A/B，不是收益预测；不替代语义 oracle / mutant / 完整矩阵 |
| 本文件 | `docs/evidence/621-progress-wait-path-20261118/p2/P2-COST-MAP.md` |
| 输入 | `p2/PROFILE-SUMMARY.md`；`p2/profile/analysis/{analysis.json,analysis.txt,analyze_p2.py}`；`p2/profile/P-*/raw/*`；`CAMPAIGN-LEDGER.md` §4/§5；`p1/AA-CALIBRATION.md`、`p1/AA-CALIBRATION-2.md` 的噪声带 |
| 原始数据 | 5 个形态：P-A/P-B/P-C/P-D/P-E；每形态 `stat-pmu-r3.txt`、`stat-mem-r3.txt`、`stat-tp-*.txt`、`record-cycles.data`、`record-offcpu.data`、`script-cycles-sym.txt`、`script-offcpu-sym.txt`、`report-cycles-dso-symbol.txt` |
| 复算 | 本会话用 `python3 -I` 直接读取 `analysis.json` 与 `script-cycles-sym.txt` / `script-offcpu-sym.txt` / `stat-*.txt`，并复用 `analyze_p2.py` 的 `parse_script()`、`attribute()`、`make_resolver()` 逻辑复算；**没有重跑 perf/bench/test** |

> **一句话结论**：本 profile 测到的成本主体是**内核与 worker 的机器工作**，不是 Sluice 用户态协调代码。用户态产品函数 leaf 份额只有 0.5–7.6%；阶段 owner 份额中的大头是这些阶段调用栈内的内核拷贝、syscall、调度、io-wq、mutex。所有“上界”都是**采样份额/计数算术**，不是可删除 cycles，也不是候选收益。

---

## 0. 读数口径与输入校正（先读）

### 0.1 读数口径

1. **份额 ≠ 可删除 cycles**。perf cycles 采样定位的是“CPU 在哪里执行”，不是“删掉哪段代码就省下多少”。W1/W3/M-53/M-42 的上界内部互相重叠，**不能相加**。
2. **阶段 self 是“最深阶段 owner”份额**：对每条样本从 leaf 向 root 扫描，取最内层出现的 Sluice 阶段函数作为该样本的 owner；每条样本只归一次。它包含该阶段调用栈内的内核/调度/同步成本，**不是该函数的指令级 self**（`analysis/analyze_p2.py:250-290,454-490`）。
3. **inclusive 份额互相重叠**：任意调用帧出现该阶段即计数，不能把 inclusive 行相加，也不能把 inclusive 当作 self。
4. **非阶段桶**（payload/FS、io-wq punt、shared sync、scheduler/IRQ、kernel syscall/FS overhead、syscall unattributed、bench harness、other）用于说明“这部分不是 Sluice 协调成本”或“无法归因”，**不能整体视为可删除**。
5. **行级归因 NOT_OBSERVABLE**：release 二进制 stripped、无 DWARF；符号来自逐节 sha256 匹配的未 strip 孪生。内核无 debuginfo，只有函数级。本图只给符号级份额与源码函数坐标。
6. **PMU 与采样 DSO 拆分不是同一分母**：PMU 用于总量与 user/kernel 总账；`record` 的 DSO 份额只用于相对热点。P-B 的 PMU `cycles:k` 81.7% 与 record kernel 92.5% 的差异来自 io-wq 继承/采样偏差，不能互相替代。
7. **off-CPU 计数是 `sched_switch` 事件数，不是阻塞时长**；窗口只有一个 measured round。
8. **没有 clean A/B**：本轮没有候选 build、mutant、A/A、A/B；任何“收益已成立”的说法都不成立。

### 0.2 输入不一致：本图采用 `analysis.json`，并显式修正两处

本会话对照 `PROFILE-SUMMARY.md` §4 与 `analysis.json` / `script-cycles-sym.txt` 后发现两处不能直接沿用：

**(A) `PROFILE-SUMMARY.md` §4 表漏列 `startup/thread create` 行。** 按该表逐行相加：P-A 100.00%、P-B 100.00%、P-C 95.39%、P-D 86.33%、P-E 91.13%，与其“行和≈100%”的说明不符。`analysis.json` 的 `stage_self` 包含该行：P-C 4.61%、P-D 13.67%、P-E 8.87%。

更重要的是：**这一行不是线程创建成本。** `analyze_p2.py:273` 的 `syscall_owner()` 在整条调用栈上搜索 `clone3`；worker/driver 线程的调用栈根部包含 `start_thread` / `__GI___clone3`（线程创建点），因此这些样本被误标为 `startup/thread create`。原始栈显示 leaf 是 syscall return trampoline，其下第一个非 trampoline 被调符号是：

| 形态 | 被误标样本 | callee 分解（样本数，占该形态） | 实际归属 |
|---|---:|---|---|
| P-C | 759（4.61%） | `lll_lock_wait` 394（2.39%）、`lll_lock_wake` 208（1.26%）、`read` 100（0.61%）、`cond_broadcast` 38（0.23%）、其余 19 | driver/worker 的 futex/epoll/read trampoline |
| P-D | 2,075（13.67%；本会话按栈复算 2,031） | `__libc_pread` 951（6.26%）、`lll_lock_wake` 815（5.37%）、`lll_lock_wait` 224（1.48%）、mutex unlock 26（0.17%）、write 10（0.07%）、其余 5 | TP worker 的 pread + mutex trampoline |
| P-E | 3,082（8.87%；复算 3,081） | `__libc_pread` 907（2.61%）、`__libc_pwrite` 734（2.11%）、`__libc_write` 699（2.01%）、`lll_lock_wake` 689（1.98%）、`lll_lock_wait` 45（0.13%）、其余 7 | TP worker 的 pread/pwrite/write + mutex trampoline |

因此本图把该行单列为 **“worker-thread syscall trampoline（analysis 误标 startup）”**，不当作 startup，也不并入任何候选上界。按 callee 归位后的近似值（记录员复核，MEDIUM 置信）：

- P-C：shared sync ≈ +3.9pp、progress/wait ≈ +0.6pp（read 可能是 eventfd read）。
- P-D：shared sync ≈ +7.0pp、backend exec (TP) ≈ +6.3pp。
- P-E：shared sync ≈ +2.1pp、backend exec (TP) ≈ +6.7pp。

**(B) `PROFILE-SUMMARY.md` §3 的 `single-frame` 数不可复算。** 用 `script-cycles-sym.txt` 直接数“只有 1 个调用帧”的样本：P-A 46、P-B 1,207、P-C 173、P-D 139、P-E 362；summary 写的是 817/2,751/2,440/4,138/8,753。两者不是同一口径，本图不引用 summary 的 single-frame 数，只用 `analysis.json` 的 `samples` 与 `single_frame` 字段（46/1,207/173/139/362）。

**(C) 小项：`task-clock` 两遍 stat 不同。** P-A `stat-pmu-r3` 为 2,593.96ms，`stat-mem-r3` 为 2,690.60ms；本图沿用 `analysis.json` 的 `stat-mem-r3` 值，两遍均 `percent_enabled=100.00`。

### 0.3 归因方法与置信词

- 方法：`analyze_p2.py:250-290` 的 `attribute()`；`syscall_owner()` 把通用 syscall 入口/出口 trampoline 按栈内符号分给 submit / backend exec / progress/wait / shared sync / startup；`leaf_category()` 先把 payload/FS、io-wq、bench harness、backend exec、startup 等物理/非协调工作按 leaf 分类；否则取最内层阶段函数。
- **置信词**：`HIGH` = 原始采样份额/计数直接支持；`MEDIUM` = 由原始数据再分组或按栈复核，方向可靠但边界不精确；`LOW` = 候选级可删除量，未测；`NONE/NOT_OBSERVABLE` = 本数据无法观测。

---

## 1. 执行 DAG

DAG 由 `PROFILE-SUMMARY.md` §9 的源码坐标、`CAMPAIGN-LEDGER.md` §2 的锁/线程所有权、以及本会话实际读取的 `src/async/async_io_context.cpp:573-705`、`include/sluice/async/detail/progress_source.hpp:190-232`、`src/async/threadpool_backend.cpp:386-400`、`apps/sluice-copy/copy_task.cpp:305-351`、`src/async/uring_transport.cpp:218-235,540-557`、`src/async/uring_backend.cpp:171` 汇总。方括号内是该边的主要成本 owner。

### 1.1 W-02 io_uring（P-A d16 / P-B d1）

```text
app/bench thread
  submit_read
    -> RequestCore::reserve -> RequestCore::accept                    [user; admission]
    -> UringAsyncBackend::dispatch_after_accept
       -> dispatch_one_locked -> get_sqe / prep_sqe -> router/ledger  [user+kernel; dispatch]
       -> submit_transport_locked -> io_uring_submit
          -> sys_enter_io_uring_enter -> io_submit_sqes               [kernel; submit]
    -> kernel io_uring: io_uring_submit_req
       -> io_uring_queue_async_work (tmpfs: 1/op)
          -> iou-wrk: io_worker_handle_work -> io_read
             -> shmem_file_read_iter -> rep_movs_alternative          [kernel worker; payload/FS]
          -> io_cq_unlock_post -> CQE                                 [kernel; reap/CQE]
    -> wait path: AsyncIoContext::wait_one
       -> run_progress_pass_ -> poll_progress
          -> ProgressSource::wait_if_unchanged -> poll(notification_fd)  [kernel; progress/wait]
          -> eventfd signal / read / drain                            [kernel; progress/wait]
    -> reap_cqes -> handle_one_cqe
       -> find_live_router_cookie_ (O(R) reverse scan)                [user; M-53]
       -> finalize_operation_terminal_ -> offer_terminal
          -> release_execution                                        [user; terminal/release]
       -> publication_pending -> publish_one
          -> begin_publication -> complete_publication                [user; publication]
          -> deliver_event -> observer take/consume                   [user; observation]
```

### 1.2 W-02 ThreadPool（P-D d16 w4）与 sluice-copy（P-E d4 w1）

```text
app thread
  submit_read / submit_write
    -> RequestCore::reserve/accept                                    [user; admission]
    -> ThreadPoolBackend::dispatch_ -> work_mtx_ push                 [user; dispatch]
    -> poll_progress / run_progress_pass_                             [caller; progress/wait]
  worker thread (worker_loop)
    -> work_cv_.wait(work_mtx_)                                       [futex; shared sync]
    -> pread64 / pwrite64                                             [kernel; backend exec]
    -> rep_movs_alternative / shmem / page cache                      [kernel; payload/FS]
    -> completion -> caller publish_one -> deliver_event              [user; publication/delivery]

sluice-copy main
  run_pipelined_copy_with_context
    -> RequestScope::wait_for -> wait_one -> poll(notification_fd)    [kernel; progress/wait]
       -> worker_loop pread -> write/pwrite -> completion
```

### 1.3 W-03 split-owner（P-C）

```text
driver_thread -> epoll_wait(progress fd)                             [kernel; progress/wait]
  -> poll_progress -> reap_cqes -> publish_one -> deliver_event       [driver; reap+publication]
submitter -> submit_read -> io_uring_enter -> ...                     [kernel; submit]
  -> condvar futex wait (completion)                                  [kernel+libc; shared sync]
  -> observation / take                                               [user; observation]
```

### 1.4 DAG 边的成本归属（摘要）

| 边 | 主要 owner | 证据 |
|---|---|---|
| `submit_transport_locked -> io_uring_submit` | kernel | submit 阶段 97.6/98.7/98.2% 为 kernel（P-A/P-B/P-C）；`io_uring_enter` 1/op |
| `io_uring -> queue_async_work -> iou-wrk` | kernel worker | `io_uring_queue_async_work` 1/op（P-A/P-B/P-C）；io-wq punt 阶段 10.23/7.65/6.02% |
| `io_read -> shmem/rep_movs` | kernel | `rep_movs_alternative` 11.94/7.84/6.78/9.04/3.73%；`shmem_file_read_iter` 2.83/1.63/1.51/1.47 |
| `wait_one -> wait_if_unchanged -> poll` | kernel | progress/wait 阶段 kernel 占 66.9/97.9/87.7/46.5/64.1%；P-B `__x64_sys_poll` inclusive 15.77% |
| `publish_one -> begin/complete_publication -> deliver` | user/libc | publication 阶段 user 占 84/94/86/90/98%；P-D libc 62.5% |
| `worker_loop -> pread/pwrite` | TP worker | P-D/P-E `worker_loop` off-CPU futex 68.6%/48.6%；worker pread/pwrite trampoline 6.3/6.7pp |
| `driver_thread -> epoll_wait` | driver thread | P-C off-CPU epoll wait 4,645（14.7%） |
| IRQ entry / reschedule IPI | kernel IRQ | 仅 `asm_sysvec_reschedule_ipi`、`__irqentry_text_end` 等；无设备 IRQ 身份 |

---

## 2. 机器工作成本台账

### 2.1 PMU 总账（`perf stat -r 3`，无缩放）

原始：`P-*/raw/stat-pmu-r3.txt`、`stat-mem-r3.txt`。全部 198 条 stat 行 `percent_enabled=100.00`，无 `<not counted>` / `<not supported>` / `scaled`（`PROFILE-SUMMARY.md:69-82`）。分母：bench 540,000 ops；P-E 65,536 chunks（131,076 requests）。

| 形态 | cycles:u/op | cycles:k/op | cycles 合计/op | instr:u/op | instr:k/op | instr 合计/op | task-clock µs/op | ctx-sw/op | cache-miss/op | branch-miss/op |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| P-A d16 uring | 2,666.7 | 12,423.3 | **15,090.0** | 3,871.5 | 6,781.4 | 10,652.9 | 4.983 | 0.339 | 19.154 | 25.882 |
| P-B d1 uring | 5,248.0 | 23,455.2 | **28,703.2** | 6,676.6 | 17,565.1 | 24,241.7 | 11.793 | 2.000 | 23.320 | 43.677 |
| P-C W-03 d16 | 3,689.3 | 16,635.6 | **20,324.9** | 3,217.4 | 9,474.7 | 12,692.1 | 6.864 | 0.606 | 20.299 | 35.008 |
| P-D TP d16 w4 | 5,728.1 | 14,658.0 | **20,386.1** | 4,032.7 | 8,134.5 | 12,167.2 | 6.356 | 0.668 | 19.953 | 27.613 |
| P-E copy /chunk | 10,875.1 | 33,606.4 | **44,481.5** | 8,675.8 | 22,233.2 | 30,908.9 | 16.183 | 2.040 | 31.177 | 74.893 |
| P-E copy /request | 5,437.6 | 16,803.2 | **22,240.8** | 4,337.9 | 11,116.6 | 15,454.5 | 8.092 | 1.020 | 15.589 | 37.447 |

**用户/内核总账（PMU cycles）**：P-A 17.7% / 82.3%；P-B 18.3% / 81.7%；P-C 18.2% / 81.8%；P-D 28.1% / 71.9%；P-E 24.4% / 75.6%。**内核侧始终是主体。**

**PMU 总账 vs record DSO 拆分**（不同分母，不可互相替代）：

| 形态 | PMU u/k | record kernel | record lib | record bench | 备注 |
|---|---:|---:|---:|---:|---|
| P-A | 17.7 / 82.3 | 84.74% | 5.58% | 9.68% | record 内核略高于 PMU |
| P-B | 18.3 / 81.7 | 92.51% | 6.93% | 0.57% | io-wq 继承/采样偏差最大 |
| P-C | 18.2 / 81.8 | 82.84% | 10.57% | 6.59% | 与 PMU 接近 |
| P-D | 28.1 / 71.9 | 74.25% | 18.99% | 6.76% | TP worker 同 comm，不能按线程拆 |
| P-E | 24.4 / 75.6 | 73.60% | 19.19% | 7.21% | copy 主线程 + worker |

### 2.2 线程/worker 归属

`record` 的 comm 过滤见 `analyze_p2.py:359-364`（bench 形态含 `bench_w02*` / `bench_w03*` / `iou-wrk*`；copy 形态含 `sluice-copy*`）。

| 形态 | 过滤后样本 | app 线程样本 | `iou-wrk` 样本 | TP worker 证据 |
|---|---:|---:|---:|---|
| P-A d16 uring | 8,369 | 4,272（51.05%） | 4,097（48.95%） | — |
| P-B d1 uring | 24,585 | 12,246（49.81%） | 12,339（50.19%） | — |
| P-C W-03 | 16,472 | 11,010（66.84%，`bench_w03_exter`） | 5,462（33.16%） | driver 是 app-owned 线程 |
| P-D TP w4 | 15,182 | 15,182（100%，同一 comm） | — | `worker_loop` off-CPU futex 22,135（68.6%）；leaf 2.24% |
| P-E copy | 34,736 | 34,736（100%，同一 comm） | — | `worker_loop` off-CPU futex 80,918（48.6%）；leaf 1.17% |

- P-A/P-B/P-C 的 `iou-wrk` 份额是 **worker 侧采样的直接证据**；其 leaf 顶部是 `rep_movs_alternative`（P-A 24.09%、P-B 15.20%、P-C 20.10%），说明 worker 在跑 payload 拷贝，不是 Sluice 协调。
- P-D/P-E 的 TP worker 与主线程同 comm，**不能按 comm 拆 cycles**；只能用 `worker_loop` 的 inclusive/leaf、off-CPU 阻塞点和重标定的 trampoline 样本作证据。
- P-A off-CPU 99.9% 是 `iou-wrk` idle；P-B 是 app `poll` 50.0% + `iou-wrk` idle 50.0%；P-C 是 `iou-wrk` idle 44.8% + app futex 40.5% + app epoll 14.7%；P-D 是 futex 74.9% + mutex/condvar 19.2% + scheduler 3.4% + poll 2.5%；P-E 是 poll 50.2% + futex 49.8%（`analysis.json.offcpu.primitive`）。注：summary §5 把 P-D 的 mutex/condvar 与 scheduler 合记为 6,613/20.5%；本图按 `analysis.json` 分列。

### 2.3 阶段 owner self 台账（每条样本只归一次，行和 = 100%）

原始：`analysis.json[*].cycles.stage_self`；方法与 `analyze_p2.py:454-490` 一致。

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
| **worker-thread syscall trampoline**（analysis 误标 `startup/thread create`） | 0.00% | 0.00% | **4.61%** | **13.67%** | **8.87%** |
| shared sync（非阶段：mutex/spinlock，无阶段 owner） | 5.10% | 7.43% | 5.43% | 13.27% | 7.86% |
| scheduler/IRQ（非阶段：EEVDF/PSI/IPI/futex wake） | 8.69% | 12.11% | 7.68% | 12.89% | 6.01% |
| kernel syscall/FS overhead（非阶段：fput/fdget/kmalloc 等） | 0.96% | 1.95% | 0.84% | 2.04% | 5.26% |
| syscall unattributed（入口/出口 trampoline，栈被截断） | 3.43% | 7.99% | 4.10% | 7.38% | 13.02% |
| bench harness（非阶段：run_*_phase/verify/memcmp） | 3.76% | 1.32% | 2.06% | 1.99% | 0.00% |
| other（未归类，EEVDF 残余/app 等） | 4.15% | 7.04% | 3.59% | 5.84% | 7.99% |

**行和**：P-A 100.00%、P-B 100.00%、P-C 100.00%、P-D 100.00%、P-E 100.00%（含上表粗体的 trampoline 行）。`PROFILE-SUMMARY.md` §4 的版本漏列该行，故其 P-C/P-D/P-E 行和分别为 95.39/86.33/91.13；本图以 `analysis.json` 为准。

**该行的性质**：不是 startup。原始 `script-cycles-sym.txt` 的样本栈显示，leaf 为 `entry_SYSRETQ_unsafe_stack` / `syscall_return_via_sysret` / `entry_SYSCALL_64`，栈根为 `start_thread` / `__GI___clone3`；callee 是 `__libc_pread`、`__libc_pwrite`、`read`、`lll_lock_wait`、`lll_lock_wake`。这是**长寿命 worker/driver 线程的 syscall 返回 trampoline**，不是 clone 成本。把该行与 `syscall unattributed` 合并得到“syscall boundary 合计”：

| 形态 | syscall unattributed | 误标 trampoline | syscall boundary 合计 | 备注 |
|---|---:|---:|---:|---|
| P-C | 4.10% | 4.61% | **8.71%** | driver/worker 的 futex/read trampoline |
| P-D | 7.38% | 13.67% | **21.05%** | TP worker pread + mutex trampoline |
| P-E | 13.02% | 8.87% | **21.89%** | TP worker pread/pwrite/write + mutex trampoline |

该合计是**同一 stage 分区内的两个不相交行相加**，不是把不同口径混加；但它仍是记录员复核口径，不是 `analysis.json` 的原生字段。置信：机制 HIGH（栈直接可读），比例 MEDIUM（callee 分类有少量未归类样本）。

### 2.4 非阶段桶的含义

| 桶 | 含义 | 是否可删除 |
|---|---|---|
| payload/FS | 4 KiB payload 拷贝、tmpfs shmem、page cache、`__io_read` | 否；除非改变 workload/数据路径 |
| io-wq punt | 内核 io-wq 异步 punt（tmpfs 下 1/op） | 非 Sluice 代码；#637 记 NO_GO；FS 条件性 |
| backend exec (TP) | TP worker 的 `worker_loop`、pread/pwrite | 否；数据搬运本身 |
| shared sync | 无阶段 owner 的 mutex/spinlock/futex | 部分必要；不能整体删 |
| scheduler/IRQ | EEVDF、PSI、IPI、futex wake、信号返回 | 大部分必要；只有过度并发/唤醒可讨论 |
| kernel syscall/FS overhead | fput/fdget/kmalloc/security 等 | 必要内核对象管理 |
| syscall unattributed | 截断栈的 syscall 入口/出口 trampoline | 不能强分给 pread/pwrite/futex/poll |
| bench harness | 基准验证/计时/pattern 校验 | 不属于产品路径；A/B 应排除 |
| other | 未归类 | 不能当可删项 |

### 2.5 阶段内的 user/kernel 分解（关键阶段）

`analysis.json[*].cycles.user_kernel`；百分比是**该阶段样本内**的 kernel vs user（bench+lib）占比，不是全形态份额。

| 阶段 | P-A | P-B | P-C | P-D | P-E |
|---|---:|---:|---:|---:|---:|
| admission | k 12% / u 88% | k 0% / u 100% | k 71% / u 29% | k 76% / u 24% | k 70% / u 30% |
| dispatch | k 57% / u 43% | k 95% / u 5% | k 69% / u 31% | k 16% / u 84% | k 0% / u 100% |
| submit | k 98% / u 2% | k 99% / u 1% | k 98% / u 2% | — | — |
| reap/CQE | k 80% / u 20% | k 97% / u 3% | k 67% / u 33% | — | — |
| terminal/release | k 25% / u 75% | k 0% / u 100% | k 8% / u 92% | k 20% / u 80% | k 2% / u 98% |
| publication | k 16% / u 84% | k 6% / u 94% | k 14% / u 86% | k 10% / u 90% | k 2% / u 98% |
| progress/wait | k 67% / u 33% | k 98% / u 2% | k 88% / u 12% | k 47% / u 53% | k 64% / u 36% |
| observation | k 23% / u 77% | k 0% / u 100% | k 16% / u 84% | k 8% / u 92% | k 0% / u 100% |

**解读**：

- `submit` 与 `progress/wait` 在 uring 形态几乎全是 kernel（submit 97–99%，P-B wait 98%）。把它们叫作“Sluice 用户态成本”是错误的。
- `publication` / `terminal/release` / `observation` 在 TP/copy 形态 user 占 80–98%，主要是 libc mutex/condvar 与产品代码；这是 W3 最相关的面，但仍是**阶段 owner**，不是单候选成本。
- `admission` 在 P-C/P-D/P-E 的 kernel 占 70–76%，说明该阶段样本里大量是 `submit_read` 栈内的内核唤醒/调度/锁，不是 RequestCore 的几条用户指令。

### 2.6 不可相加矩阵（显式标记）

| 项 A | 项 B | 关系 | 规则 |
|---|---|---|---|
| 阶段 self | 阶段 inclusive | 同一调用栈的两种计数 | **不能相加**；inclusive 只说明“调用帧出现”，不是额外成本 |
| leaf symbol（如 `rep_movs` 11.94%） | 阶段 owner（如 io-wq punt 10.23%） | 同一样本可同时计入 leaf 与 stage owner | **不能相加**；二者是两种切法 |
| PMU cycles:u/k | record DSO kernel/lib/bench | 分母/采集方式不同 | **不能互相换算**；PMU 是总账，record 是热点 |
| shared sync 13.27%（P-D） | W3 的 publication/terminal/admission 阶段 | 阶段样本内的 mutex 与无阶段 mutex 是不同样本 | 若把“W3 族 + shared sync”相加，会把同一类 mutex 成本重复叙述；**不能相加** |
| progress/wait 25.53%（P-B） | `__x64_sys_poll` 15.77% / `__poll` 24.93% | 后者是前者的调用栈子集，且 inclusive 互相重叠 | **不能相加**；只能取“阶段 self”或“单个 inclusive 栈”之一作上界 |
| `wait_one` inclusive 61.94%（P-E） | `__x64_sys_poll` 11.46% / progress/wait 25.12% | `wait_one` 在大量样本的调用帧中出现，含非 poll 路径 | **不能当 poll 成本**；也不能与 poll 栈相加 |
| submit 21.35%（P-A） | syscall unattributed 3.43% | submit 只含 `io_uring_enter` 内部路径，不含通用 trampoline | 真实 syscall 总成本 = submit + 一部分 unattributed；**该拆分不可精确解析** |
| payload/FS 19.87%（P-A） | io-wq punt 10.23% | payload 样本可在 io-wq 栈内 | **不能相加**；二者是 leaf 分类 vs 栈 owner |
| off-CPU 事件数 | cycles 份额 | 事件数 vs 采样份额 | 单位不同；**不能当时间占比** |
| raw report（未过滤） | analysis（过滤 comm） | 分母不同（如 P-A 8,390 vs 8,369） | 份额不可直接对比；本图以 analysis 过滤后为准 |
| W1/W3/M-53/M-42 上界 | 彼此 | 同一进程 cycles 的不同子集/不同切法 | **不能相加**；候选之间也不可加 |

---

## 3. Top Hotspots

### 3.1 逐形态 leaf 前 8（`analysis.json[*].cycles.leaf_top`，过滤后样本）

| 形态 | #1 | #2 | #3 | #4 | #5 | #6 | #7 | #8 |
|---|---|---|---|---|---|---|---|---|
| P-A d16 uring | `rep_movs_alternative` 11.94% K | `_raw_spin_lock` 6.36% K | `entry_SYSRETQ_unsafe_stack` 3.81% K | `swapgs_restore_regs_and_return_to_usermode` 3.04% K | `shmem_file_read_iter` 2.83% K | `asm_sysvec_reschedule_ipi` 2.83% K | `io_cq_unlock_post` 2.53% K | `__memcmp_avx2_movbe` 1.88% L |
| P-B d1 uring | `rep_movs_alternative` 7.84% K | `entry_SYSRETQ_unsafe_stack` 6.22% K | `_raw_spin_lock` 5.41% K | `__raw_spin_lock_irqsave` 3.77% K | `__smp_call_single_queue` 2.10% K | `syscall_return_via_sysret` 2.06% K | `entry_SYSCALL_64` 2.05% K | `psi_group_change` 1.96% K |
| P-C W-03 d16 | `entry_SYSRETQ_unsafe_stack` 6.87% K | `rep_movs_alternative` 6.78% K | `_raw_spin_lock` 5.15% K | `__GI___pthread_mutex_unlock_usercnt` 3.96% L | `pthread_mutex_lock` 3.60% L | `native_queued_spin_lock_slowpath` 3.18% K | `entry_SYSCALL_64` 2.44% K | `syscall_return_via_sysret` 2.22% K |
| P-D TP d16 w4 | `entry_SYSRETQ_unsafe_stack` 13.29% K | `rep_movs_alternative` 9.04% K | `__GI___pthread_mutex_unlock_usercnt` 7.12% L | `pthread_mutex_lock` 5.96% L | `entry_SYSCALL_64` 4.69% K | `syscall_return_via_sysret` 4.28% K | `native_queued_spin_lock_slowpath` 2.50% K | `ThreadPoolBackend::worker_loop()` 2.24% B |
| P-E copy d4 w1 | `entry_SYSRETQ_unsafe_stack` 13.28% K | `__GI___pthread_mutex_unlock_usercnt` 7.60% L | `pthread_mutex_lock` 6.35% L | `entry_SYSCALL_64` 4.84% K | `syscall_return_via_sysret` 4.40% K | `rep_movs_alternative` 3.73% K | `run_pipelined_copy_with_context` 1.77% B | `clear_highpages_kasan_tagged` 1.48% K |

K = kernel，L = libc，B = bench/产品用户态。leaf 份额是分区；单个样本只归一个 leaf symbol。**不要**把同一行多个符号相加当作某个阶段成本（例如 `entry`/`ret`/`swapgs` 同属 syscall 边界，但还有其他内核工作）。

### 3.2 跨形态热点族

| 族 | P-A | P-B | P-C | P-D | P-E | 说明 |
|---|---:|---:|---:|---:|---:|---|
| payload 拷贝 / tmpfs / page cache | `rep_movs` 11.94 + `shmem` 2.83 + `__io_read` 1.61 | 7.84 + 1.63 | 6.78 + 1.51 | 9.04 + 1.47 + `filemap_get_entry` 1.39 | 3.73 + `clear_highpages` 1.48 | 数据搬运本身；不是 Sluice 协调 |
| syscall 边界（entry/ret/swapgs） | 3.81+3.04+1.51+1.25 ≈ 9.6 | 6.22+2.06+2.05 ≈ 10.3 | 6.87+2.44+2.22+1.11 ≈ 12.6 | 13.29+4.69+4.28 ≈ 22.3 | 13.28+4.84+4.40 ≈ 22.5 | 叶族求和；与 `syscall unattributed` / 误标 trampoline 有重叠叙述 |
| 用户态 mutex / kernel spin | spin 6.36+1.86 | spin 5.41+3.77 | libc mutex 7.56 + spin 10.15 | libc mutex 13.08 + spin 4.13 | libc mutex 13.95 + spin 0.79 | 共享同步；部分必要 |
| scheduler / PSI / IPI | resched IPI 2.83 | `smp_call` 2.10 + PSI 1.96 | PSI 1.44 + resched 1.59 | PSI 1.50 + `__schedule` 0.98 | PSI 1.15 + `smp_call` 1.11 | 不是设备 IRQ |
| io-wq | `io_worker_handle_work` 1.51 + `io_wq_put_work` 1.29 + `io_wq_worker_running` 1.05 | 1.66+1.54+1.29 | —（punt 在 stage 6.02） | — | — | tmpfs uring 的 FS 条件性 punt |

**注意**：上表的“族求和”只是为了阅读方便；叶族之间、叶族与阶段表之间都不能相加。

---

## 4. CPU / Kernel / Worker / IRQ / FS 成本归属

### 4.1 CPU

- **总 cycles/op**：P-A 15,090；P-B 28,703；P-C 20,325；P-D 20,386；P-E 44,482/chunk（22,241/request）。d1 是 d16 的约 1.9×；W-03 与 TP d16 接近；copy 每 chunk 最重。
- **user/kernel**：user 17.7–28.1%，kernel 71.9–82.3%。用户态不是主体。
- **context-switch**：P-A 0.339/op、P-B 2.000/op、P-C 0.606/op、P-D 0.668/op、P-E 2.040/chunk。P-B/P-E 的每 op/chunk 两次切换与 poll/futex 等待形态一致。
- **PMU 离散度限制**：P-D `cycles:u` 2.58%、P-A `instr:k` 8.05%、P-A `ctx-sw` 25.53%；相关小份额不作强结论（`PROFILE-SUMMARY.md:80`）。

### 4.2 Kernel

- 内核是 71.9–82.3% cycles。主要构成：payload 拷贝（11.94/7.84/6.78/9.04/3.73% `rep_movs`）、spinlock（6.36/5.41/5.15/1.63/—% `_raw_spin_lock`）、syscall 边界、tmpfs shmem/page cache、io-wq、调度/PSI/IPI。
- 阶段内的 kernel 占比：submit 97–99%；P-B progress/wait 98%；P-C progress/wait 88%；P-E progress/wait 64%。这些阶段不是“用户态协调”，而是**内核执行提交/等待/唤醒**。
- `syscall unattributed` 3.43/7.99/4.10/7.38/13.02%（summary 口径）；加上误标 trampoline 后，P-C/P-D/P-E 的 syscall boundary 合计约 8.7/21.1/21.9%。这是“内核 syscall 入口/出口被截断”的归因上限，不能强行分给 pread/pwrite/futex/poll。

### 4.3 Worker

- **uring worker（iou-wrk）**：P-A 48.95%、P-B 50.19%、P-C 33.16% 的采样；leaf 顶部是 `rep_movs`（24.09/15.20/20.10% of comm）、`_raw_spin_lock`（9.59/8.08/10.42%）、`io_cq_unlock_post`（4.95/—/3.55%）。worker 主要在做 payload 与内核完成处理。
- **TP worker**：P-D/P-E 同 comm，不能按 comm 拆；`worker_loop` inclusive 68.28/48.30%，leaf 2.24/1.17%；off-CPU `worker_loop` futex 22,135（68.6%）/ 80,918（48.6%）。这是 worker 侧等待/执行的直接证据。
- **重标定 trampoline**：P-D 的 `pread` trampoline 951 样本（6.26pp）、P-E 的 pread/pwrite/write 2,340 样本（6.74pp）应归 backend exec；P-D/P-E 的 `lll_lock_*` trampoline 1,039/734 样本应归 shared sync。见 §0.2(A)。
- **W-03 驾驶员/提交者**：driver `epoll_wait` 4,645 off-CPU（14.7%）；submitter futex 12,829（40.5%）；`poll_progress` inclusive 24.15% 是 progress pass（driver/调用线程）在一次 poll 中做的 reap/publish，不是纯等待。

### 4.4 IRQ

- **能看到的 IRQ**：`asm_sysvec_reschedule_ipi` 2.83%（P-A）、1.59%（P-C）；`sysvec_reschedule_ipi` 0.81%（P-A raw report）、0.23%（P-C raw report）；`__smp_call_single_queue` 2.10%（P-B）、1.11%（P-E）；`asm_sysvec_call_function_single` 0.18%（P-D）；`asm_sysvec_apic_timer_interrupt` 0.01%（P-B/P-E）。
- **`__irqentry_text_end`**：原始（未过滤）`report-cycles-dso-symbol.txt` 中 P-A 3.57%、P-C 1.35%、P-D 0.21%、P-E 0.03%、P-B 0.01%。这是 perf 的 section-boundary 符号，具体 IRQ 向量未解析；与 kallsyms 的 `asm_sysvec_*` 份额是不同分母，不能相加。
- **设备 IRQ NOT_OBSERVABLE**：本会话没有 per-IRQ 记录；`block:block_rq_issue/complete` 全为 0；bench 在 tmpfs；P-E source 是 btrfs warm page cache。`irqbalance` 前后 inactive（`PROFILE-SUMMARY.md:24`）。因此没有磁盘/NVMe/设备 IRQ 成本结论。
- **不要把 `__raw_spin_lock_irqsave` 当 IRQ**：它是关中断的自旋锁，P-A 1.86%、P-B 3.77%、P-C 1.82%、P-E 0.79%，属于 shared sync。

### 4.5 FS / 介质

- 所有 bench 形态在 tmpfs（64 MiB fixture，page-cache resident）；P-E 是 btrfs（`compress=zstd:1`）warm page cache → tmpfs。`block` 层 0 事件；没有磁盘、NVMe、cold、fsync/durability 结论。
- `payload/FS` 10.44–19.87% 是 tmpfs shmem/page cache 与 payload 拷贝，不是磁盘 FS。
- **io-wq punt 是 FS 条件性形态成本**：P-A/P-B/P-C `io_uring_queue_async_work` = 1/op，`io_uring_task_work_run` = 0.78/1.00/0.78/op；P-D/P-E 0。P-A/P-B/P-C io-wq punt 阶段 10.23/7.65/6.02%。这与 #637/#622 的“FS 条件性、非 Sluice 代码”一致，不能外推到其他 FS/设备。

### 4.6 Sluice 用户态协调成本（leaf 下界）

用 twin 符号解析后的 bench/sluice DSO leaf，按符号前缀分：

| 形态 | 产品 `sluice::`/`sluice_copy::` leaf | bench harness leaf | 其他 bench leaf |
|---|---:|---:|---:|
| P-A | 632（7.55%） | 136（1.63%） | 42（0.50%） |
| P-B | 120（0.49%） | 14（0.06%） | 5（0.02%） |
| P-C | 897（5.45%） | 138（0.84%） | 51（0.31%） |
| P-D | 854（5.63%） | 128（0.84%） | 45（0.30%） |
| P-E | 2,410（6.94%） | 0（0.00%） | 94（0.27%） |

单函数 leaf 例子：`dispatch_one_locked` 1.24/0.02/1.03/0/0%；`claim_execution` 0.12/0.00/0.07/0.61/0.40%；`worker_loop` 0/0/0/2.24/1.17%；`run_pipelined_copy_with_context` 1.77%（P-E）；`wait_one` 0.02/0.07/0/0.09/0.29%。

**结论**：Sluice 用户态产品 leaf 只有 0.5–7.6%，而阶段 owner 的 26.7–54.2% 中包含大量内核机器工作。任何“删掉 Sluice 某段用户代码就能省下阶段份额”的推断都不成立。

---

## 5. 候选收益上界

> 上界是**测量份额/计数算术**，不是预期收益；上界内部互相重叠，**不能相加**；未做单因素 A/B，不能据此声称任何候选 GO。

### 5.1 W1 — ProgressSource poll 等待路径

| 形态 | progress/wait self | poll 相关 inclusive | 计数证据 | 上界 | 置信 |
|---|---:|---|---|---|---|
| P-B d1（最热） | **25.53%** | `__poll` 24.93%、`__x64_sys_poll` 15.77%、`do_sys_poll` 15.38%、`poll_schedule_timeout` 11.12%、`io_eventfd_signal` 8.67%、`eventfd_signal` 16.24%、`wait_one` 2.85% | poll 1/op、eventfd read 1/op、wait_one 1/op、ctx-sw 2/op、off-CPU poll 59,947 | ≤**25.5%** 进程 cycles（阶段 self）；仅 poll syscall 栈 ≤**15.8%** inclusive | 份额 HIGH；可删量 LOW/UNKNOWN |
| P-E copy | **25.12%** | `__poll` 15.83%、`__x64_sys_poll` 11.46%、`do_sys_poll` 11.43%、`poll_schedule_timeout` 8.43%；`wait_one` inclusive 61.94%（**不可当 poll**） | poll 1.317/chunk、eventfd read/write ≈1.047/chunk、wait_one 1.122/chunk、off-CPU poll 83,481 | ≤**25.1%**；仅 poll 栈 ≤**11.5%** | 份额 HIGH；可删量 LOW/UNKNOWN |
| P-A d16 | 5.27% | poll=0 | wait_one 0.0625/op、cqring wait 4、没有 `poll` 计数 | ≤5.3%，但**不是** poll 路径 | 不支持候选 |
| P-C W-03 | 16.29% | poll=0；epoll_wait 0.111/op；futex 1.05–1.53/op | split-owner：driver epoll、submitter futex | **不是同一 `poll(-1)` 路径**；不可把 16.29% 当 W1 上界 | 不适用 |

**扣除项**：阻塞 eventfd read 仍需要等待/唤醒/eventfd 信号；`io_eventfd_signal` 8.67%、eventfd read 0.46%、`poll_schedule_timeout` 11.12%、wake/scheduler 成本不会全部消失。因此可删除 cycles 必然显著低于 15.8%/11.5% 的 poll 栈上界；历史 R1 的 5.0–5.5% 线索落在此上界内，但本轮**未证实**。

### 5.2 W3 — RequestCore mutex/生命周期族

**族级 self 上界**（admission + terminal/release + publication + observation）：

| 形态 | 族级 self | 其中 user（bench+lib） | 其中 kernel | 备注 |
|---|---:|---:|---:|---|
| P-A d16 | 6.00% | 4.92% | 1.08% | 单线程 submit；RequestCore 的 inclusive 份额随符号匹配口径变化，本图不以它作上界 |
| P-B d1 | 0.24% | 0.24% | 0.00% | **严重内联进 `wait_one`/`run_progress_pass_`，不能把 0.24% 当族成本；INCONCLUSIVE** |
| P-C W-03 | 15.82% | 8.63% | 7.19% | admission 8.91 + terminal 2.31 + publication 2.82 + observation 1.77 |
| P-D TP w4 | 22.43% | 11.50% | 10.93% | admission 12.76 + terminal 3.49 + publication 4.13 + observation 2.05 |
| P-E copy | 13.07% | 7.98% | 5.09% | admission 7.16 + terminal 0.88 + publication 3.30 + observation 1.74 |

**逐候选（stage self；括号内为该阶段的 user 部分）**：

| 候选 | 对应阶段 | P-A | P-B | P-C | P-D | P-E |
|---|---|---:|---:|---:|---:|---:|
| M-61 `begin`/`complete` 两锁融合 | publication | 1.46%（u 1.23） | 0.07%（u 0.07） | 2.82%（u 2.43） | **4.13%（u 3.71）** | 3.30%（u 3.23） |
| M-62 无观察者跳过 claim/retire | publication + `claim_execution` | 1.58%（u ~1.35） | 0.07% | 2.89%（u ~2.50） | **4.74%（u ~4.32）** | 3.70%（u ~3.63） |
| M-65 owed `release_control` 第三锁融合 | terminal/release | 0.97%（u 0.73） | 0.03% | 2.31%（u 2.13） | **3.49%（u 2.80）** | 0.88%（u 0.86） |
| M-67 sticky 双标志 | progress/wait | 5.27%（u 1.74） | **25.53%（u 0.54）** | 16.29%（u 2.00） | 3.52%（u 1.88） | **25.12%（u 9.02）** |
| M-10 零长发布三锁融合 | publication + terminal | **NOT_OBSERVABLE**（零长 op 未在本 workload 出现；上表是形态上界，不是零长成本） | — | — | — | — |

- `claim_execution` leaf self 0.12/0.00/0.07/0.61/0.40%（`analysis` 复算与 summary 一致）。
- 置信：阶段 self 是 MEASURED（HIGH）；阶段内 user/kernel 分解是 MEDIUM；**单候选可删除量 LOW/PARTIAL**。融合后仍保留至少一把锁/一次发布，不能把整个阶段当收益。
- **P-B d1 的 0.24% 不是族成本**：`RequestCore::reserve/accept` 等被内联进 `wait_one`/`run_progress_pass_`，其开销被归到 progress/wait（summary §4 注 0）。W3 在 d1 的上界 **INCONCLUSIVE**。

### 5.3 M-53 — cookie O(R) 逆向扫描 → `user_data` 路由索引

| 形态 | `find_live_router_cookie_` self | inclusive | cycles/op（self × 总 cycles/op） | 备注 |
|---|---:|---:|---:|---|
| P-A d16 | 0.406%（34/8,369；Wilson 95% CI 0.291–0.567%） | 0.59% | ≈61.3 cyc/op | 用 twin 解析未符号化 bench leaf |
| P-C d16 | 0.285%（47/16,472；Wilson 95% CI 0.215–0.379%） | 0.29% | ≈57.9 cyc/op | 同上 |
| P-B d1 | 0.00%（0 样本；summary 记 0.01%） | 0.00% | ≈0 | router 表仅 1 项 |
| P-D / P-E | 0（无 uring） | 0 | 0 | — |

- 上界：d16 形态 ≤**0.6%** 进程 cycles / ≤**62 cycles/op**（self）；d1 几乎为 0。
- **d64 NOT_OBSERVABLE**：本轮未采集 d64；summary 的 P1-A 历史 d64 1.74% / ≈116 cyc/op 只能作线索，不能外推。`AA-CALIBRATION-2.md:27,499` 的 C7 d64 A/A 只有 cycles 可用，16 对 t-hw 1.007% 对 2%/2=1.0% 差一对，建议 17–20 对。
- 置信：d16 份额 HIGH（但样本量小，34/47 个样本）；d64 NONE；三项迁移义务（TransportLedger 位置单调、控制 CQE exact-submitted、cancel 定向）未在 profile 中验证。

### 5.4 M-42 — `submit_flushes_` 只写不读原子计数

- 机制：`src/async/uring_transport.cpp:222` 在 ledger 空早退之后 `fetch_add`，每 submit 1 次 RMW；唯一读者在测试宏内（`src/async/uring_test_seams.hpp:75-76`，成员 `include/sluice/async/detail/uring_backend.hpp:324`）。
- 每 op 总 instructions：P-A 10,652.9、P-B 24,241.7、P-C 12,692.1、P-D 12,167.2、P-E 30,908.9/chunk。1 条 instruction/submit 的份额：P-A 0.0094%、P-B 0.0041%、P-C 0.0079%、P-D 0.0082%、P-E ≈0.0065%（2 submit/chunk）。
- 按 1 次 xadd 约 3–5 cycles、以实测 cycles/op 为分母：P-A 0.020–0.033%、P-B 0.010–0.017%、P-C 0.015–0.025%、P-D 0.015–0.025%、P-E 0.007–0.011%。
- **上界：≤0.04% 进程 cycles**（保守取整）；远低于 16 对噪声带。profile 无法单独观测这条 RMW 的 cycles，本上界来自“1 条指令 × 实测每 op 总量”的算术。
- 置信：上界算术 MEDIUM；精确效应 **NOT_OBSERVABLE / NOT_POWERED**。

### 5.5 非候选：io-wq punt / H1 提交批形态

- **io-wq punt**：P-A/P-B/P-C `io_uring_queue_async_work` = 1/op，stage self 10.23/7.65/6.02%；P-D/P-E 0。它是 tmpfs uring 的 FS 条件性内核行为，非 Sluice 代码；#637 记 NO_GO（非候选 2），本轮独立复现“条件性存在”，不产生产品收益上界。
- **H1 提交批形态**：`sys_enter_io_uring_enter` = 1/op（P-A/P-B/P-C）；submit 阶段 21.35/10.91/12.49%，其中 kernel 97.6/98.7/98.2%。这是 immediate per-dispatch submit 的现状，**不是** H1 的 A/B 证据；#637 的 H1 仍为 NO_GO，无新证据不复活。

### 5.6 上界 vs 可测性（只说明“上界 ≠ 效应”，不把上界当效应）

A/A 噪声带来自 `p1/AA-CALIBRATION.md` §5 与 `p1/AA-CALIBRATION-2.md` §5：

| 候选 | 本图上界 | 相关 A/A 16 对 MDE80 | 可测性判断 |
|---|---:|---|---|
| W1（P-B 阶段） | 25.5% | C5b CPU 0.72%、C3b cycles 0.99%、C7 cycles 1.42% | 上界远高于 MDE，但上界不是效应；P-B/P-E 都没有自己的 A/A 格，C5b/C3b/C7 只是相邻格代理。历史 5% 过程级效应在这些代理格可测；1.2% measured-loop 稀释效应在 36 对可测（C5b/C3b） |
| W1（P-E 阶段） | 25.1% | 同上 | 同上；P-E 没有自己的 A/A 格，不能直接把 C5b/C3b 的带搬来 |
| W3 单候选（计划 δ=2%） | 阶段 2.8–22.4% | C5b CPU 0.72%、C3b cycles 0.99%、C7 cycles 1.42% | 2% 在 36 对可测（C5b 7 对、C3b 10 对、C7 17 对到 hw≤1%）；但**行级归因缺失**，上界不能当候选效应 |
| M-53 d16 | 0.6% | C6 CPU 0.75%、C3b cycles 0.99% | 上界 ≤ MDE → d16 在当前 A/A 格**不可测**；d64 需 C7 cycles（16 对边缘，17–20 对） |
| M-42 | 0.04% CPU / 0.004–0.009% instr | C5b CPU 0.72%、C3b instr 0.088%、C6 instr 0.010% | CPU 不可测；1 条 instr 在 C3b/C6 16 对也不可测（C6 需约 61 对）；C5b TP instr MDE 0.002% 能分辨 0.005%，但 M-42 是 uring-only，格不匹配 → **NOT_POWERED** |

**使用规则**：这些数字只回答“如果效应是 X，需要多少对/哪个格”，不回答“效应是 X”。A/A 的 hw/MDE 不能搬到别的格/后端/深度；C7 的 d64 带是条件性诊断带，不是已认证基线（`AA-CALIBRATION-2.md:406-419,499`）。

### 5.7 上界总表与重叠标记

| # | 候选/形态 | 上界 | 依据 | 置信 | 可与谁相加？ |
|---|---|---:|---|---|---|
| B1 | W1 P-B | 25.5% 阶段 / 15.8% poll 栈 | stage self + `__x64_sys_poll` inclusive + poll 1/op | 份额 HIGH；可删 LOW | **不可**与 B2/B3/W3/M-53/M-42 相加 |
| B2 | W1 P-E | 25.1% 阶段 / 11.5% poll 栈 | stage self + poll 1.317/chunk | 份额 HIGH；可删 LOW | **不可**与 B1 相加（不同形态） |
| B3 | W1 P-A | 5.3% 阶段，但 poll=0 | stage self；wait_one 0.0625/op | 不支持候选 | 不可当 W1 上界 |
| B4 | W3 族 P-C/P-D/P-E | 15.8/22.4/13.1% 阶段；user 8.6/11.5/8.0% | stage self；阶段内 user/kernel | MEDIUM | **不可**与 M-61/M-62/M-65/M-67 各自相加 |
| B5 | M-61 publication | 4.13% P-D / 2.82 P-C / 3.30 P-E | publication stage self | LOW–MEDIUM | 属于 B4 的子集；不可与 B4 相加 |
| B6 | M-62 publication+claim | 4.74 P-D / 2.89 P-C / 3.70 P-E | publication stage + claim leaf | LOW | 子集；不可与 B5 相加 |
| B7 | M-65 terminal | 3.49 P-D / 2.31 P-C / 0.97 P-A | terminal stage self | LOW–MEDIUM | 子集；不可与 B5/B6 相加 |
| B8 | M-67 progress/wait | 25.53 P-B / 25.12 P-E / 16.29 P-C | progress/wait stage self | LOW（含 W1 等待路径） | 与 W1 上界重叠；不可相加 |
| B9 | M-53 d16 | 0.6% / 62 cyc/op | twin 解析 leaf self；34/47 样本 | d16 HIGH（小样本）；d64 NONE | 属于 reap/CQE 阶段子集；不可与 B10 相加 |
| B10 | M-53 d64 | NOT_OBSERVABLE | 未采集 d64 | NONE | — |
| B11 | M-42 | ≤0.04% cycles；1 instr/submit 0.004–0.009% | 1 条指令 × 实测 cycles/instr | 算术 MEDIUM；效应 NONE | 属于 submit 阶段子集；不可与 B1/B4 相加 |

**总规则：B1–B11 全部不能相加；同一候选的“阶段上界”与“user 部分”也不能相加（user 是阶段的子集）。**

---

## 6. 与 #637 / #622 历史结论对照

来源：`CAMPAIGN-LEDGER.md` §4/§5/§6.3（对 #637 终态与 #622 的转述）。**#622 原始 cost map 不在本仓库已跟踪/可引用范围内**（其在未跟踪目录 `docs/evidence/e5-fullpath-machine-work/`，台账 §9.1.12 要求不触碰）；本图未打开或引用其内容，只对照台账转述，置信受限。`docs/evidence/621-round2-R1/REPORT.md` 是已被 R2 r6 取代的工作稿，也只作线索。

| 历史结论 | 台账来源 | 本轮独立复现？ | 不同点 / 限制 |
|---|---|---|---|
| W1 `poll(-1)` 约 5.0–5.5% cycles 可删 | `:257,270`（R1 工作稿，已被取代） | **未复现为可删量**；只复现“等待路径是热点”：P-B 25.53%、P-E 25.12% 阶段 self，poll 栈 15.8/11.5% | 上界包含大量内核等待/唤醒；5.0–5.5% 落在上界内，但没有 A/B、没有逐请求/行级分离，不能称已证实 |
| W3 mutex family ~9–11% | `:259,271,223`（r6 F.6 族级线索） | **部分复现**：P-C 15.8%、P-D 22.4%、P-E 13.1%、P-A 6.0%；P-B 0.24% 不可用 | 量级随形态变化；P-D 明显更高、P-A 更低；9–11% 不是普适值。族级存在成立，单候选仍无 |
| M-61 R3 拒绝：0.45% < 1% MME REJECTED | `:222` | **未在同口径复现**：publication 阶段 1.46/0.07/2.82/4.13/3.30%；但这是 stage owner，不是候选的锁成本 | 阶段包含 publish/deliver 的内核/用户工作；不能拿 4.13% 推翻 0.45% 的候选级测量。不同测量范围 |
| M-53 P1-A d64 1.74% / ≈116 cyc/op | `PROFILE-SUMMARY.md:427` 转述 | **未复现 d64**；d16 self 0.406/0.285% / 61/58 cyc/op | d16 方向相同但更小；d64 未采集。C7 d64 A/A 边缘（17–20 对）也未把 profile 的 d64 效应测出来 |
| M-42 `submit_flushes_` NOT_POWERED | `:217`；`AA-CALIBRATION.md:475-500` | **独立复现结论**：≤0.04% cycles，1 instr/submit 0.004–0.009%，16 对不可分辨 | 本轮是算术复现，不是新测量；仍需隔离格 |
| H1 提交批形态 NO_GO | `:229` | **现状复现**：`io_uring_enter` 1/op、submit 阶段 10.9–21.4%、ring flags=0 | 未做 H1 A/B；不能因本轮 submit 份额大就复活 H1 |
| io-wq punt FS 条件性 | `:230,323`（#637 非候选 2 / #622 转述） | **独立复现条件性**：tmpfs uring `queue_async_work`=1/op、stage 6.0–10.2%；TP 0 | 非 Sluice 代码；不能外推到磁盘/NVMe/其他 FS |
| #622：最高杠杆 H1 不跨边界；产品 ring flags=0；cost map 是 trigger-grade | `:323-326` | **部分一致**：`io_uring_queue_init(..., 0)`（`src/async/uring_backend.cpp:171`）确认 flags=0；submit 阶段 97–99% kernel；设备 IRQ 不可见 | #622 原文未打开/未引用；不能把台账转述当作本轮独立复现。G1（真实 workload）仍未满足 |

**结论**：#637 的候选终态没有被本轮 profile 推翻。本轮新增的是“形态级 measured hotspot 地图”，不是候选级 A/B；`#637` 的 `INCONCLUSIVE`/`NO_GO`/`SEMANTIC_MISMATCH` 状态继续有效。

---

## 7. W1 / W3 / M-53 / M-42 是否达到「有 measured hotspot」准入

判定只针对“是否有 measured hotspot”这一门槛；**不是** A/B 准入，也不是 GO。

| 候选 | 有 measured hotspot？ | 证据 | 准入判定 | 还缺什么 |
|---|---|---|---|---|
| **W1** ProgressSource poll | **是（仅 P-B/P-E，phase 级）**；P-A/P-C 否 | P-B progress/wait 25.53%、poll 栈 15.77%、poll=1/op、off-CPU poll 50.0%；P-E 25.12%、poll 栈 11.46%、poll=1.317/chunk、off-CPU poll 50.2% | **PARTIAL PASS**：达到“等待路径有 measured hotspot”的 profile 准入；**未达到候选级准入**（可删量、行级/逐请求分离、语义设计、A/A、A/B 均未做） | 有界等待/非阻塞 drain/dup fd 设计；5% 分母预注册；A/A 36 对；A/B ≥36 对；若升级为 kernel-boundary 研究，走 #617 G1 |
| **W3** RequestCore 族 | **族级是**（P-C 15.82%、P-D 22.43%、P-E 13.07%）；**单候选不是**；P-B 否 | 阶段 self 与阶段内 user 分解；`RequestCore` 单函数 leaf 仅 0.12–0.61%（claim）等 | **未达到 W3 自身的准入**：ledger 要求“measured 行级归因”，本轮只有符号级；P-B d1 因内联不可分离；单候选（M-61/62/65/67）只有阶段上界，没有候选级 measured hotspot | 行级 profile（当前 NOT_OBSERVABLE，需 debug build/隔离格）；每候选的步间语义证明；族 A/A；单因素 A/B |
| **M-53** cookie O(R) | **d16 是，但很小**（P-A 0.406%、P-C 0.285%）；**d64 否** | twin 解析 34/47 样本；d1/TP/copy 0；d64 未采集 | **未达到 ledger 准入**：ledger 要求 d64 深度格 A/A 半宽 ≤ 效应/2 + 三项迁移义务；本 profile 没有 d64，三项义务未证 | d64 profile + d64 cycles-only A/A（17–20 对）；TransportLedger 单调、控制 CQE exact-submitted、cancel 定向证明 |
| **M-42** `submit_flushes_` | **否** | 1 RMW/submit；≤0.04% cycles；0.004–0.009% instr；profile 无法单独解析该内联 RMW | **未达到**：没有 measured hotspot；只有算术上界，且低于 uring 16 对噪声带 | 隔离格（submit-only 紧循环）+ 动态回执带 + 机制门七问；在隔离格 A/A 完成前保持 NOT_POWERED |

**一句话**：只有 W1 在 P-B/P-E 上达到了“等待阶段有 measured hotspot”的 profile 门槛；W3 只达到族级、未达到单候选/行级门槛；M-53 只达到 d16 小热点、未达到 d64 门槛；M-42 没有 measured hotspot。**四者都没有达到 A/B 准入。**

---

## 8. 缺口与不能下的结论

### 8.1 缺口

1. **行级归因 NOT_OBSERVABLE**：release 二进制 stripped、无 DWARF；内核无 debuginfo。所有阶段/候选上界都是符号级或阶段级。
2. **没有 A/B / 语义门**：没有候选 build、mutant、A/A、A/B、TLC；不能推出任何收益。
3. **采集开销**：DWARF callgraph 使进程时长 1.6–2.2×；tracepoint stat 2.0–3.7×；uprobe 5–110×。份额可用于归因，时长/吞吐不可用于收益。
4. **`syscall unattributed` 3.4–13.0%**：内核栈在 syscall 入口/出口截断；不能强分给 pread/pwrite/futex/poll。加上误标 trampoline 后，P-C/P-D/P-E 的 syscall boundary 约 8.7/21.1/21.9%，仍未解析到具体 syscall。
5. **`shared sync` / `scheduler/IRQ` / `kernel syscall/FS overhead` 无阶段 owner**：不能整体视为可删除；其中一部分是必要同步/调度/内核工作。
6. **输入 summary 的两处不一致**：§4 漏列 trampoline 行（行和 P-C/P-D/P-E 为 95.39/86.33/91.13）；§3 的 `single_frame` 数（817/2,751/2,440/4,138/8,753）与 `analysis.json`/独立复算（46/1,207/173/139/362）不符。本图以后者为准，不引用前者。
7. **`startup/thread create` 是栈根 artifact**：不是线程创建成本；已按 callee 重标定，但 callee 分类仍有 ≤0.12pp 未归类。
8. **P-B d1 的内联**：`reserve/accept/offer/release/begin/complete` 被内联进 `wait_one`/`run_progress_pass_`；W3 在 d1 的 0.24% 不能当族成本。
9. **IRQ 只有入口/向量级**：没有 per-IRQ 亲和性/设备 IRQ 身份；`block` 0 事件；tmpfs；不能下设备 IRQ/磁盘 FS 结论。
10. **未覆盖形态**：d64、64K、zero-op、cancel、saturation、TP w1/w8、W-03 d4/d64 未采集；M-10 零长因此 NOT_OBSERVABLE；M-53 d64 NOT_OBSERVABLE。
11. **P-E 介质**：btrfs warm page cache → tmpfs，单次 398.4ms；source 已 warm，且 profile 窗口不带 runner 的 per-arm 哈希/进程开销；不能与 P1 C1 arm 作 A/B。
12. **tracepoint stat vs record 计数不同**：P-C futex 825,214（epoll 组） vs 566,325（sched 组）；record 自身改变执行/批形。计数结论以 `perf stat` 为准。
13. **PMU vs record DSO 差异**：P-B 81.7%（PMU k） vs 92.5%（record kernel）；不能互相替代。
14. **`task-clock` 两遍 stat 不同**（P-A 2,593.96 vs 2,690.60ms）；本图沿用 `analysis.json` 的 stat-mem 值。
15. **M-53 小样本**：P-A 34、P-C 47 个 leaf 样本；Wilson 95% CI 0.291–0.567% / 0.215–0.379%。份额虽 measured，但不确定性不小。
16. **`wait_one` inclusive 61.94%（P-E）不可当等待成本**：它在大量样本的调用帧中出现，含非 poll 路径；只能作为“调用栈出现”证据。
17. **#622 原文未打开/未引用**：只对照 `CAMPAIGN-LEDGER.md:323-326` 的转述；未跟踪目录 `docs/evidence/e5-fullpath-machine-work/` 按其纪律未作为证据源引用。
18. **off-CPU 是事件数**：不是阻塞时长；窗口只有一个 measured round。

### 8.2 不能下的结论

- 不能说 W1 能删 5.0–5.5%（或任何百分比）；只能说 P-B/P-E 的等待阶段/ poll 栈有 measured hotspot，上界 25.5%/15.8%。
- 不能说 W3 的 15.8–22.4% 是单候选收益；它包含内核、libc、bench 和必要 mutex；单候选可删量未测。
- 不能说 M-61/M-62/M-65/M-67 任一候选 GO；#637 的 INCONCLUSIVE 未被本轮推翻。
- 不能说 M-53 在 d64 有 hotspot；d64 未测，且 A/A 分辨力边缘。
- 不能说 M-42 可测；当前 uring 格 16 对分辨不了 1 条 instruction/submit。
- 不能说 io-wq punt 是跨 FS 成本；它只在 tmpfs uring 形态 1/op。
- 不能说设备 IRQ、磁盘、NVMe、cold、btrfs 写回的成本；本 profile 没有这些。
- 不能把 PMU `cycles:k` 与 record kernel 份额互相换算；不能把 off-CPU 事件数当时间占比。
- 不能把 W1/W3/M-53/M-42 的上界相加；它们是同一进程 cycles 的不同子集/切法。

---

## 附录 A：证据路径与复现

### A.1 原始数据

```
docs/evidence/621-progress-wait-path-20261118/p2/
  PROFILE-SUMMARY.md
  P2-COST-MAP.md                      # 本文件
  profile/
    analysis/analysis.json            # 本图主数据源
    analysis/analysis.txt
    analysis/analyze_p2.py
    analysis/SHA256SUMS
    env/snapshot-before.txt, snapshot-after.txt, symbol-twin-verification.txt
    env/symbols-bench_w02.txt, symbols-bench_w03.txt, symbols-sluice-copy.txt
    P-A-w02-uring-4k-d16/raw/
    P-B-w02-uring-4k-d1/raw/
    P-C-w03-uring-4k-d16/raw/
    P-D-w02-tp-4k-d16-w4/raw/
    P-E-sluice-copy-4k-d4-w1/raw/
```

每个 `raw/` 的关键文件：

- `stat-pmu-r3.txt`、`stat-mem-r3.txt`：PMU（`percent_enabled` 100%）。
- `stat-tp-*.txt`：tracepoint 计数。
- `stat-uprobe-*.txt`：uprobe 计数（只用于调用计数，不用于性能）。
- `record-cycles.data`、`record-offcpu.data`：perf 原始数据。
- `script-cycles-sym.txt`：本图 leaf/stage/comm 复算的输入（twin 符号化后的最终脚本）。
- `script-offcpu-sym.txt`：off-CPU 阻塞点。
- `report-cycles-dso-symbol.txt`：未过滤的 perf report（含 `__irqentry_text_end` 等）。
- `header-record-*.txt`、`lost-record-*.txt`：采样时长/丢失回执（15/15 Lost=0）。

### A.2 复现命令

```bash
# 1) 原始汇总（会写 /tmp/p2-621/analysis.json，需要 kallsyms 与 twin；本会话没有重跑）
cd /home/jnhu/Source/Sluice/docs/evidence/621-progress-wait-path-20261118/p2/profile/analysis
python3 -I analyze_p2.py

# 2) 本图用到的复算（python3 -I 只读；按 analyze_p2.py 的 parse_script/attribute 逻辑）
#    - stage_self / stage_inclusive / user_kernel / leaf_top / offcpu.primitive
#    - comm 过滤：analyze_p2.py:359-364
#    - router/claim/wait 符号 leaf/inclusive：用 make_resolver() 解析 script-cycles-sym.txt
#    - startup 误标复核：对 stage_self=='startup/thread create' 的样本取 leaf 下第一个非 trampoline 符号
#    命令形态：python3 -I - <<'PY' ... PY（脚本不落盘；读取 analysis.json 与 P-*/raw/script-cycles-sym.txt）

# 3) 单点核验（示例）
grep -n 'submit_flushes_' /home/jnhu/Source/Sluice/src/async/uring_transport.cpp
grep -n 'io_uring_queue_init' /home/jnhu/Source/Sluice/src/async/uring_backend.cpp
```

### A.3 复现限制

- `analyze_p2.py` 依赖 `/tmp/p2-621/kallsyms.txt` 与 `/tmp/p2-621/symtwin/*.sym`；这些是会话内文件，不是仓库内容。若它们不存在，需按 `PROFILE-SUMMARY.md` §0.2/§13 重建。
- 本图没有把复算脚本入库；只记录方法、输入路径和关键数字。行级归因仍 NOT_OBSERVABLE。
- 任何引用本图的数字，请同时给出形态、分母（540,000 ops / 65,536 chunks）和“份额 ≠ 可删 cycles”的口径。

---

*本文件结束。*
