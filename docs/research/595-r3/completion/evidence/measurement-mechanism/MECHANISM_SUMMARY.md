# MECHANISM_SUMMARY — #595 R3 续跑机制测量（短诊断窗，不做正式计时）

生成（UTC）：2026-11-05T12:10:00Z（数据窗口 2026-11-05T11:53:16Z–12:01:38Z）

## 0. 性质与边界（先读）

- 本轮是 `PREREGISTERED_R3_COMPLETION.md` §12.1 的**短机制诊断轮**：bpftrace/perf/strace/PMU
  全部带仪器；所有 CPU/墙钟/计数差异**不是性能结论**，不得进入 §6 配对统计。
- `semanticsReady=false` 未翻转（本轮未发现 §14.3 独立 verifier 回执）。prereg §14 把
  `MECH-*`（A1/B）列为 `BLOCKED_SEMANTICS_NOT_READY`；本轮按 ask 指令以诊断口径执行并记录，
  **不解锁任何 prereg 门**；下文 PASS/FAIL 只针对机制存在性/负控机制形状，不针对性能门。
- 二进制：`/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/bench_w03_external_loop`
  sha256 `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1`（会话前后一致，
  `raw/mechanism/pre-state.json`）。臂经 `SLUICE_595_R3_ARM`（A0/A1/B）。
- 锁：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（inode 6941075）。主会话 11:53:16Z 获取
  （等待 0s，fuser 61676）→ 12:00:21Z 释放；phase2 12:01:27Z–12:01:38Z。全部构建/运行/tracing
  在持锁窗口内；与计时块零重叠（获取前后 `ps` 无 bench/perf/measure_pairs）。
  `raw/mechanism/lock-log.jsonl` 4 条记录（含 11:52:01Z 一次因脚本 bug 秒级失败会话，锁即时释放，
  无测量发生）。
- 仪器健康：每个 enter 追踪 attach 21 probes（`bpftrace.err` "Attached 21 probes"）；全部 9 个
  追踪 run **0 lost events**；逐事件 printf 计数 == `@enter_bench` map 计数（9/9 run 相等）。
  alloc 追踪 attach 6 probes，0 lost。

## 1. 纯 submit enter vs wait/GETEVENTS（ask 项 1）

bpftrace 脚本：`raw/mechanism/scripts/enter_trace.tmpl.bt`（实例化 `_session_enter_trace.bt`，
comm 过滤 `bench_w03_exter`，由 `/proc/<pid>/task/*/comm` 实测确认，`logs/comm-probe.json`）。
PID 过滤按 comm（bench 由 `taskset -c 2` exec，tgid 即 spawn pid；事件带 pid/tid，事后核对
全部落在同一 pid）。每个 run 1 次 invocation：`WARMUP=0 MEASURED=1 STATS=1`，
case filter 精确匹配，3 个 phase（timed/oracle/instrumented）。

| run | 总 enter | 纯 submit (ts>0,mc=0,fl=0) | submit+GETEVENTS | 纯 wait/GETEVENTS | @bucket |
|---|---|---|---|---|---|
| d16-A0 | 60,004 | 60,000 | 4 | **0** | {`1,0,0`:60000, `1,1,1`:4} |
| d16-A1 | 60,004 | 60,000 | 4 | **0** | 同 A0 |
| d16-B | 29,229 | 29,225 | 4 | **0** | {`1,0,0`:24826, `8,0,0`:4395, `3,0,0`:2, `2,0,0`:1, `6,0,0`:1, `1,1,1`:4} |
| d2pos-A0 | 10 | 6 | 4 | **0** | {`1,0,0`:6, `1,1,1`:4} |
| d2pos-A1 | 10 | 6 | 4 | **0** | 同 A0 |
| d2pos-B | 7 | 3 | 4 | **0** | {`2,0,0`:3, `1,1,1`:4} |
| d2serial-A0 | 60,004 | 60,000 | 4 | **0** | {`1,0,0`:60000, `1,1,1`:4} |
| d2serial-A1 | 60,004 | 60,000 | 4 | **0** | 同 A0 |
| d2serial-B | 60,004 | 60,000 | 4 | **0** | 同 A0 |

- 4 个 `to_submit=1,min_complete=1,flags=IORING_ENTER_GETEVENTS` 的 enter 是每次 invocation 的
  探针/setup（`uring_backend.cpp:224-231` 的 required-opcode probe，对应 4 个 STATX SQE）。
- `io_uring_enter` 返回值直方图：纯 submit 的 ret == to_submit（如 B：ret=8×4395、ret=1×24830），
  **零部分提交**；A0/A1/d2serial：ret=1×60,004。
- **驱动线程零纯 submit**：9/9 run 的纯 submit 全部在 submitter 主线程（tid==pid）；
  driver 纯 submit = 0。

## 2. SQE/CQE、批量形状、ready/约束、拆批、accepted/untouched/suffix（ask 项 2）

**data/control SQE/CQE**（内核 `io_uring:io_uring_submit_req` 按 opcode、`io_uring_complete` 按 res）：
- data SQE = opcode 22 (`IORING_OP_READ`)：所有臂均恰 60,000（d16/d2serial）/6（d2pos），
  与 `expected_data_sqe = 3×request_count` 相等。
- 本 bench 自带 control/setup SQE = opcode 21 (`IORING_OP_STATX`) 恰 4/invocation
  （availability probe）。**无 cancel control SQE**（W-03 无取消路径）。
- CQE：res=4096 恰 60,000（全部 4 KiB 全量读）；res=0 为探针/control CQE（B d16 恰 4）。
- 污染边界：`io_uring:*` tracepoint 无 pid 字段，整机口径。本机其它进程（node/chrome 等映射
  liburing）产生**突发性 opcode 29 (`IORING_OP_EPOLL_CTL`) 背景 SQE**（各 run 0–228 不等；
  baseline 39s 均值 0.13/s）。同型臂间计数方差（A0:76 / A1:5 / B:0；d2pos-A1:228）证明其与臂无关。
  **所有 per-arm 结论只使用 comm 过滤的 syscall 级计数与 bench 自身 stats**，内核级计数只作
  旁证并按上述边界标注。

**d16 主形状（B 的实际批量）**：
- B 纯 submit enter 29,225 vs A0 60,000 → **enter 减少 51.29%**（SQE/enter=2.053；
  to_submit>1 的 enter 4,399 个，max=8）。
- bench r3 计数（timed phase，20,000 ops）：`entry=10515, group=1355, single=9160,
  accepted=20000, prefix_max=8, suffix_single=9160, untouched=9160, flush=10515,
  ineligible=0, gate_stops=0, rejected=0`。
- ready 数与约束：组前缀=8（`ready_batch_cap=8`，`r3_ready_batch.hpp:115-135`）；
  1,355 个组 × 前缀 8 = 10,840；9,160 个单成员（steady-state 补发 1 条）+ 9,160 suffix。
  `accepted + untouched == 20,000`，`untouched == suffix_single_calls`（每个被 cap 截断的成员
  都逐条 span=1 补交）。`flush_attempts == entry_calls`（10,515）→ **每次入口调用恰一次 flush，
  无跨调用持有 SQE、无 defer**。
- 已有 SQE 拆批机会：d16 的 SQ 容量=queue_depth=16，理论上 8+8 可合一次 enter；B 的 cap=8 策略
  把后 8 条留作 suffix 逐条（设计如此，suffix 不二次成组）。观测与之自洽：to_submit 直方图无 >8，
  suffix enter 的 to_submit=1。steady-state 窗口内 ready 多为 1–2 条（24,826 个单条 enter 主导），
  这是外部循环按完成粒度补发的形状，不是 census `d16_full` 假设的整窗 16 条
  （census 预测 91.5–93.3% 节省 ≠ 观测 51.3%；机制存在性门槛 ≥50% 满足，量级差异如实登记）。
- A1：enter 与 A0 完全相同（60,000×1），`prefix_max=1`，`flush=20000`；A1 的适配器成本不改变
  enter 数（为 B−A1 归因提供同 enter 数基线）。

**d2pos 正控（2 条同时 ready）**：A0/A1 = 6 个纯 submit（每 phase 2×to_submit=1）；B = **3 个
纯 submit，全部 to_submit=2**（每 phase 1 次），`group=1, accepted=2, prefix_max=2, untouched=0,
flush=1`；6 data SQE/6 CQE(4096)；oracle 全对（exit=0，`ok=true`）。→ 机制正控存在性成立。

**d2serial 负控**：见 §3。

## 3. residue/eventfd、park/wake、锁与扫描、ready→提交等待、flush 位置（ask 项 3）

- **residue**：Σto_submit=60,004=60,000 data+4 探针（9/9 run `delta_all=4` 恰为探针）；
  bench `stats_submitted_ops=20,000`、`completed_ops=20,000`/phase；ret==to_submit 零部分提交；
  退出无未提交 SQE。内核 `io_uring:io_uring_defer` **全 run = 0**（含 baseline）→ 无 defer。
- **eventfd 通知**：`eventfd2` 4/invocation（3 phase 上下文 + 探针上下文）、
  `io_uring_register` 12/invocation，A0/A1/B **完全相同**；用户态无 eventfd 写
  （`sys_enter_eventfd`=0）。通知量（bench）：d16 A0=3,151 / A1=3,200 / **B=1,434**
  （B 更少的 completion 批）；`empty_wakes=0` 全臂。
- **park/wake**（syscall 计数，comm 过滤）：d16 futex：A0=33,242 / A1=32,685 / **B=14,878**；
  epoll_wait：A0=10,075 / A1=9,981 / **B=4,862**。d2serial：futex A0=263,254 / A1=250,998 /
  B=257,731（B 与 A0 差 −2.1%，与 A1 差 +2.7%）；epoll 三臂均 60,003。分离的
  futex_wait/futex_wake syscall 为 0（本 glibc 用传统 futex）。
- **锁与扫描增量**：内部 mutex/precheck 扫描计数不可观测（冻结二进制 stripped，bench 不暴露）。
  可观测代理：syscall futex 计数（上）与适配器 `entry_calls` 扫描计数：d2serial B `entry=20,000`
  全部 single、`prefix_max=1`（批量扫描退化为单条路径）；d16 B `entry=10,515 < 20,000`（组扫描
  替代逐条）。边界如实登记。
- **ready→提交等待**：B 的组调用在**同一 context 入口内** accept→prepare→dispatch→一次 flush
  （`bench/r3_ready_batch.hpp:110-135`；`flush_attempts==entry_calls`）。syscall 证据：4,395 个
  to_submit=8 的 enter 本身即 flush，之前无 wait 类 enter（纯 wait enter=0），且
  `submit_with_flags` 仅 4 个探针 enter → 组准备好到提交之间无 park/wait 边界。二进制 stripped
  无法测入口→enter 的纳秒级延迟（登记为不可观测项）。
- **flush 位置**：每入口调用恰一次 flush（计数 10,515==10,515；d2pos 1==1；d2serial 20,000==20,000），
  无延迟 flush、无 prepark residue（`io_uring_defer=0`）。
- **D2 负控（ask 要求"无新增 defer/residue/eventfd"）**：d2serial 三臂 enter/SQE/CQE/ret/直方图
  完全一致（60,000×`1,0,0` + 4 探针）；B `group=0, prefix_max=1, suffix=0, untouched=0,
  flush=20000`；`io_uring_defer=0`；`eventfd2=4`、`iouring_setup=4`、`register=12` 与 A0/A1 逐项
  相同；`timer_create/timerfd_create`（bench 进程口径，strace）= 0/0。→ **负控通过：B 在单条
  补发下不合发、不 defer、无 residue、无新增 eventfd**。

## 4. PMU（ask 项 4）

能力验证（`raw/mechanism/pmu-capability/`）：
- 用户态（perf_event_paranoid=2）：六事件全部自动 `:u` 限定可测（`user-true.csv`）；
  `context-switches:u` 可测。
- root：`cycles:u/cycles:k/instructions:u/instructions:k/context-switches:u/:k` 全部非零可测
  （`root-uk-true.txt`、`root-uk-true.csv`）→ 本轮用 root 全域（u+k）。

每臂 1 次 invocation（d16，`taskset -c 2`，bench 作为 perf 的 root 子进程；诊断口径）：

| 事件 | A0 | A1 | B | B−A0 |
|---|---|---|---|---|
| cycles | 854,149,299 | 852,427,314 (−0.2%) | 733,912,392 | **−14.1%** |
| instructions | 709,777,889 | 745,464,639 (+5.0%) | 651,993,702 | −8.1% |
| cache-references | 12,238,156 | 12,514,274 | 12,470,428 | +1.9% |
| cache-misses | 1,611,585 | 1,616,737 | 1,651,593 | +2.5% |
| branch-misses | 1,548,700 | 1,613,861 | 1,438,557 | −7.1% |
| context-switches | 20,090 | 23,125 | 14,830 | −26.2% |
| IPC | 0.831 | 0.875 | 0.888 | — |

- time-enabled/time-running：`perf -x,` 第 4/5 列；三臂全部事件 `time_running_ns` ≈ 217–288 ms、
  **percent=100.00（无 multiplexing，无缩放行）**。enabled=running/percent=running。
- 边界：root 全进程口径（含 setup/探针相位）；io-wq 内核 worker 若在 perf attach 之外创建则可能
  少计（进程级 rusage/分桶为旁证）；**单次带仪器运行，不是 §6 计时**。A1 指令数 +5% 而 cycles 持平
  （IPC 变化），只登记不解释。

## 5. CPU 分桶（ask 项 5）

`harness/bucket_probe.py`（与本会话同锁窗口；每 20ms 逐 tid first/last 采样；口径与
`measurement-legacy/bucket-probe/RECEIPT.md` 一致）。d16，3 次/臂（A0 因一次脚本崩溃后重跑含
6 个样本，均值口径不受影响）：

| 臂 | main(submitter) ticks | driver ticks | io-wq ticks | 线程内 kernel(stime) | bench cpu_s(进程口径) | ru_total s | 未归因 ticks |
|---|---|---|---|---|---|---|---|
| A0 | 13.7 | 0.8 | 7.3 | 7.7+0.0+7.3 | 0.0700 | 0.275 | +5.7 |
| A1 | 12.3 | 0.3 | 6.3 | 7.3+0.0+6.3 | 0.0652 | 0.257 | +6.7 |
| B | 11.0 | 0.0 | 6.0 | 6.7+0.0+6.0 | 0.0573 | 0.222 | +5.2 |

- 防重复计数：main/driver/io-wq 为**同一进程 CPU 的份额**（io-wq 线程在 `/proc/<pid>/task` 内，
  已含于 `CLOCK_PROCESS_CPUTIME_ID`），不相加成第二个总量；stime 为各桶子集，不另加。
- 可归因 IRQ：`/proc/interrupts` cpu2 增量 A0≈303 / A1≈273 / B≈258（cpu12≈0–2）为**整机上界**
  （无法归因到本 workload，tmpfs 无块设备 IRQ，主要为 timer/IPI）。
- 未覆盖残差：ru_ticks − Σ分桶 = +5.2…+6.7 ticks（10ms tick 粒度 + 采样窗外短线程）。
  按**最不利端**登记：整 invocation CPU 上界 = ru_total（A0 0.275s / B 0.222s），
  下界 = bench 进程 cpu_s（A0 0.0700s / B 0.0573s）+ 0（IRQ 不可归因部分不强制计入进程侧）。
  IRQ 上界叠加在最不利端（cpu2 增量全部计为潜在环境成本，不进入臂间差异）。
- max 用户线程 = 3（main+driver+1 io-wq）三臂相同。

## 6. 内存/线程/队列/timer/分配（ask 项 6）

| 项 | A0 | A1 | B | 结论 |
|---|---|---|---|---|
| `/usr/bin/time -v` 峰值 RSS | 5,204 KB | 5,268 KB | 5,252 KB | B +48KB(+0.9%) vs A0；非按请求增长 |
| malloc 调用（libc uprobe，comm 过滤） | 8,796 | 8,802 | 8,802 | B==A1；vs A0 +6 次（适配器 ready/accepted 向量，一次性） |
| malloc 字节 | 6,531,664 | 6,535,504 | 6,535,503 | 三臂相同量级；**无每请求分配**（60,000 ops 下 +6 次调用） |
| free 调用 | 8,840 | 8,846 | 8,846 | 与 malloc 一致 |
| calloc/realloc/memalign | 5/0/0 | 5/0/0 | 5/0/0 | 相同 |
| clone3（线程创建） | 4 | 4 | 4 | 无新增线程 |
| `/proc` 线程峰值 | 3 | 3 | 3 | 无新增线程 |
| timer_create / timerfd_create（bench 进程，strace） | 0 / 0 | 0 / 0 | 0 / 0 | **无新增 timer** |
| io_uring_setup（队列） | 4 | 4 | 4 | **无新增队列** |
| io_uring_register | 12 | 12 | 12 | 相同 |
| eventfd2 | 4 | 4 | 4 | 相同 |
| mmap / munmap / brk | 82/17/18 | 82/17/18 | 82/17/18 | 相同 |
| 自愿/非自愿 csw（time -v） | 13,895/9,182 | 16,533/10,936 | 9,406/5,218 | B 最少 |

- 每请求分配斜率（valgrind 512/1024）不可测：冻结 bench 无 request-count 旋钮
  （与 `measurement-negative/protection/SUMMARY.json` RESOURCE.allocation_slope 一致），以 libc
  分配计数 + RSS 代替并登记 NOT_RUN。

## 7. 与预注册的关系

- 本轮所有 A1/B 数据**不进入** prereg §6/§9 统计；`MECH-*`、`PROT-*-B`、`RESOURCE-B` 在
  §14 仍为 `BLOCKED_SEMANTICS_NOT_READY`。§5.5 正控要求的 3 次重复/臂本轮只做了 1 次
  invocation（含 3 个 phase，phase 粒度 3/3 全部呈现 to_submit=2）。
- 机制存在性（≥50% enter 减少，§9.3 的机制对账门槛）在本诊断窗内满足：B 29,225 ≤ 0.5×A0
  30,000；`to_submit>1`>0 且 max≤8；A0/A1=60,000 无 retry；driver 纯 submit=0；SQE/CQE/oracle
  全对。量级与 census 预测（91.5–93.3%）不符，已在 §2 登记。

## 8. 原始数据与脚本

- 脚本：`raw/mechanism/scripts/{enter_trace.tmpl.bt,alloc_trace.tmpl.bt,mech_session.py,
  mech_phase2.py,analyze_mech.py}`（实例化 `_session_*.bt` 含实测 comm）。
- 原始：`raw/mechanism/enter/<case>-<arm>/{bpftrace.out,bpftrace.err,bench.jsonl,bench.log,meta.json}`、
  `raw/mechanism/baseline/`、`raw/mechanism/pmu*/`、`raw/mechanism/alloc/`、
  `raw/mechanism/bucket/`、`raw/mechanism/resource/`、`raw/mechanism/analysis/`、
  `raw/mechanism/logs/`、`raw/mechanism/lock-log.jsonl`、`raw/mechanism/{pre,post}-state.json`。
- sudo 密码仅经 stdin（`sudo -S -v`），未写入任何文件/日志/结果。
