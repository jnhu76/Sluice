# #595 causal-r2 跨层事件关联构造（E5，2026-11-04）

状态：ACTIVE EVIDENCE（本目录全部数字来自本会话实测 run E1/E2/E3，原始文件在本目录各子目录）
任务：构造最小跨层事件关联，覆盖请求链 request accepted → SQE prepared →
defer/immediate → flush → io_uring_enter → 内核提交 → 内核执行/io-wq →
completion posted → CQE reaped → completion published → application take，
并对 user_data 复用、request 对象生命周期、ring identity、线程间交接做正确性审查。

本目录不是 timing run：用户态事件环与 bpftrace 同时开启（跨层关联的目的本身），
任何时延数字仅用于事件次序与关联验证，禁止用于性能归因（研究纪律：不同类型
instrumentation 不叠加进同一次 timing run；本 run 明确豁免且不产出性能结论）。

## 1. 关联键设计（最小，未新增运行时语义）

| 层 | identity | 来源 | 说明 |
|---|---|---|---|
| Sluice 请求 | `(slot, generation)` RequestKey | 现有 `detail::RequestKey`（include/sluice/async/detail/request_key.hpp:25） | 未新增 ID；slot 复用靠 generation 递增区分 |
| 用户态↔内核提交/完成 | `user_data == op_cookie` | 现有 cookie 分配 `allocate_cookie_()`（src/async/uring_transport.cpp:592，单调递增，CONTROL_TAG=1<<63 以下；耗尽 fail-fast 不回绕） | 未新增 correlation ID；user_data 即现有关联键 |
| ring | 内核 `ctx` 指针 ↔ 用户 `ring_fd` ↔ `io_uring_create` 事件 (fd, ctx) | tracepoint io_uring_create（本会话实测字段） | ring 建立时刻绑定；`(ctx, user_data)` 全局唯一 |
| 内核请求对象 | `req` 指针 | io_uring_submit_req/qasync/complete 同指针 | 跨内核事件一致性可验证 |

结论：**现有 identity 足够，未加任何新 correlation ID、未改任何公开 API/v2 契约**。
研究性源码改动仅限研究副本 worktree（`probe-source-diff.patch`，基线 271b81bf）。

## 2. 仪器

### 2.1 用户态：有界事件环（每进程 1 Mi 条 × 64 B，BSS 零初始化）
- 头文件：`tool_research_595_trace.hpp`（编译宏 `SLUICE_595_TRACE_BUILD`，运行时 env `SLUICE_595_TRACE=1` 门控；`sizeof(Record)=64`）
- 事件（打点位置见 §3 表）：`accept/sqe_prep/defer/immediate/flush/reap/published/take`
- 字段：seq、ts_ns（CLOCK_MONOTONIC，vDSO）、cookie、generation、aux、tid、ring_fd、slot、phase、evt、cpu（sched_getcpu）、flags（reap 的 control-cookie 位）
- 写入：原子 fetch_add 取位 + 单条 store；溢出覆盖最老并在 dump 自报 dropped（三次实验均 dropped=0）
- 打点均在 uring 后端已有锁区内或无锁路径，单 bench 线程写

### 2.2 内核态：bpftrace（root，一次性脚本）
- 脚本：`tool_bpftrace_kernel.bt`；按 pid 实例化（`TARGET_PID` 文本替换；bpftrace v0.24.2 无 `-D`，positional str 参数与 int 比较静默永假——实测踩坑，见 §7）
- 事件：io_uring_create / sys_enter+exit_io_uring_enter / io_uring_submit_req /
  io_uring_defer / io_uring_queue_async_work / io_uring_task_add / io_uring_task_work_run /
  io_uring_local_work_run / io_uring_complete / io_uring_cqe_overflow / io_uring_cqring_wait / io_uring_req_failed
- 过滤：enter/exit/create/task_work_run 按 `pid == TARGET`；io_uring:* 事件按
  `@ring_owner[ctx] == TARGET`（map 在 create 时建立）→ io-wq 内核线程上下文的
  complete/qasync 仍能归属到 bench 的 ring
- 缓冲：`BPFTRACE_PERF_RB_PAGES=2048`（默认 64 页时异步 printf 丢事件，见 §7）

### 2.3 跨仪器时钟
用户 clock_gettime(CLOCK_MONOTONIC) 与 bpftrace nsecs（bpf_ktime_get_ns）同源。
标定：以 6000 对 (用户 FLUSH 打点[syscall 调用前], 内核 sys_enter_io_uring_enter)：
差值 p50=773 ns、p99=878 ns、max=8171 ns（E1）→ 两仪器可直接对齐，无需偏移校正。

## 3. 请求链 11 节点：事件 schema 与实测覆盖

| # | 链节点 | 事件 ID（层.名） | 来源 | 关键字段 | ring identity | request identity | 置信度 | 丢失 |
|---|---|---|---|---|---|---|---|---|
| 1 | request accepted | U.accept | 源码打点 uring_backend.cpp（core_->accept 后） | ts/cpu/tid | ring_fd | (slot,gen) | 直接测量 | 0 |
| 2 | SQE prepared | U.sqe_prep | uring_transport.cpp（ledger append 后） | +cookie、SQ 物理位置 | ring_fd | (slot,gen)+cookie | 直接测量 | 0 |
| 3 | defer/immediate 决策 | U.defer / U.immediate | uring_transport.cpp（c1_deferred 判定后） | 决策值 | ring_fd | (slot,gen) | 直接测量 | 0 |
| 4 | flush | U.flush | submit_transport_locked_（io_uring_submit 前） | ledger 深度 | ring_fd | 批级（非单请求） | 直接测量 | 0 |
| 5 | io_uring_enter | K.enter / K.enter_ret | tracepoint syscalls:*_io_uring_enter | fd,to_submit,min_complete,flags,ret | fd | 批级 | 直接测量 | 0 |
| 6 | 内核提交 | K.submit_req | tracepoint io_uring_submit_req | ctx,req,user_data,opcode,sq_thread | ctx | user_data+req | 直接测量 | 0 |
| 7 | 内核执行/io-wq | K.qasync | tracepoint io_uring_queue_async_work | work,hashed | ctx | user_data+req | 直接测量 | 0（io_uring_defer 事件 0 次：路径未行使，见 §6） |
| 8 | completion posted | K.complete | tracepoint io_uring_complete | res,cflags,tid（投递者） | ctx | user_data+req | 直接测量 | 0 |
| 9 | CQE reaped | U.reap | uring_completion.cpp（peek_batch 循环） | user_data,res,control 位 | ring_fd | cookie | 直接测量 | 0 |
| 10 | completion published | U.published | uring_backend.cpp deliver_event | kind | ring_fd | (slot,gen) | 直接测量 | 0 |
| 11 | application take | U.take | request_core.cpp RequestBridge::consume | succeeded | —（-1） | (slot,gen) | 直接测量 | 0 |

关联方法：#2↔#6/#8 以 `cookie==user_data` join（同 phase 时间窗）；用户 phase ↔
内核第 p 个 io_uring_create 之后窗口（第 0 个 create 是构造探针 ring）。

## 4. 实验记录（研究纪律字段）

| 项 | 值 |
|---|---|
| 源码基线 | worktree = causal-r2 研究副本，基线 commit `271b81bf`；研究 diff `probe-source-diff.patch` sha256 `7aab60a330b0…` |
| 二进制 | `bin_bench_w02_trace` sha256 `4a379b9692f480d580cc83f7fc1272e5af3cd8c81390f8d3504b18f3a72967e6`（`SLUICE_595_TRACE_BUILD=1 xmake f -c -y --liburing=y -m release && xmake build bench_w02_pipeline`；release，非 strip） |
| 内核/工具 | Linux 7.2.11-200.fc44；bpftrace v0.24.2；liburing 2（头+so）；xmake v3.1.2 |
| CPU/环境 | bench `taskset -c 2`（用户事件 cpu 字段 3/3 run 全为 2）；E5 Xeon E5-2666 v3；loadavg 起始 0.30/0.27/0.20；串行独占 |
| 命令 | `python3 -I /tmp/s595_run.py <label> <A|B> <env…>`（工具已归档为 tool_run_experiment.py；脚本内部：fifo hold → bpftrace attach 确认后 release → 收集） |
| bench env | E1/E2: `SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER=W02-uring-4k-d2 SLUICE_BENCH_595_OPS=2000`，A/B 臂 env `SLUICE_595_ARM`；E3: `SLUICE_BENCH_595_FSYNC=1 SLUICE_BENCH_CASE_FILTER=fsync SLUICE_BENCH_595_OPS=64`；warmup=0 measured=1，fixture 复用 /tmp/sluice-595-cost（warm tmpfs）/ ~/.cache/sluice-bench（btrfs） |
| 运行顺序 | e3(失败一次 stats oracle)→修复重编→e3(8168,rc=0)→e1(8209)→e2(8230)；期间两次探索性 e1 因仪器问题废弃（§7），最终三轮全部 rc=0、bpftrace 零丢失 |
| 样本量 | E1/E2：2000 op × 3 phase（measured+oracle+instrumented 三 pass）×7 事件=42000 用户事件 + 35527/35551 内核行；E3：64 fsync op ×1 phase=448 用户事件 + 383 内核行 |
| 统计方法 | 全量逐事件 join（非采样）：`tool_join_analysis.py`；分布值（p50/p99/max）为顺序统计量 |

## 5. 实测结果

### 5.1 链路完整性与配对（全量 join，零丢失）
E1（A 臂 immediate，tmpfs 4k read d2）：
- 用户：accept=sqe_prep=reap=published=take=2000/phase，immediate=2000，defer=0
- 内核：enter=enter_ret=submit_req=qasync=complete=6004（6000 op + 4 statx 探针，user_data=0）
- 跨层：sqe_prep↔submit_req join 6000/6000；res 一致性（用户 reap.aux vs 内核 complete.res）6000/6000 全等；submit_req 无孤儿的 complete=0
- complete→reap（CQE posted→收割）p50=5367 ns，p99=18712 ns，max=40151 ns
- published→take p50=1845 ns（顺序违反 0）；reap→published p50=1459 ns（同 pass 内）

E2（B 臂 defer，同负载）：
- defer=1045/1035/1038（约 52% 请求被 defer），immediate=955/965/962；其余配对同 E1 全等（join 6000/6000，res 全等，孤儿 0）
- complete→reap p50=5619 ns p99=18923 ns

E3（fsync，btrfs dirty-write + async sync_data d4）：
- 64 op 全链配对（join 64/64，res 全等）；complete→reap p50=23183 ns max=201477 ns

### 5.2 正确性审查结论

**(a) user_data 复用**：每 ring（每 phase 一个 io_uring ctx）内 cookie 1..N 严格单调、
零重复（3 run × 3 phase 全量检查 issue=0）。跨 ring cookie 重启（1..2000）但 ring
identity 不同 → `(ctx, user_data)` 全局唯一，跨 ring 不构成复用混淆。分配器源码
在耗尽/wrap 前 fail-fast（uring_transport.cpp:596-604），无回绕复用路径。
内核侧 req 指针跨 submit_req→qasync→complete 一致性 0 违例（同 cookie 同 req 指针）。

**(b) request 对象生命周期**：(slot,generation) 在 accept/sqe_prep/published/take 各
出现一次、无重复；slot=2（d2）复用 2000 次/phase 全部伴随 generation 递增
（同 slot 同 generation 0 例）。用户链 8 事件每请求恰好各一条（42000=2000×3×7）。

**(c) ring identity**：io_uring_create 建立映射——E1 每 phase fd=4、ctx 各异、
sq_entries=2/cq_entries=4（queue_depth=2）；用户事件 ring_fd=4 与之一致；探针 ring
fd=3 独立可见。CQ overflow（io_uring_cqe_overflow）0 次；enter flags 全 0
（IORING_ENTER_GET_EVENTS 未用——收割靠共享内存 peek，与源码一致）。

**(d) 线程间交接**（直接观测）：
- 用户态：7 类用户事件 tid 全部 = bench 主线程（8209/8230/8168），**无用户态线程交接**（uring 后端无自有线程，poll 由驱动线程执行）
- 用户→内核：submit（enter/submit_req/qasync）tid=主线程；执行与 completion posted 由 io-wq worker 线程完成：E1 tid 8226（单一 worker 投递全部 6004 complete）、E3 由 4 个 worker（8192/8196/8195/8194 各 19/16/16/15）并发投递；worker tgid=bench（PF_IO_WORKER 挂进程线程组，与 C3 /proc iou-wrk 观察一致）
- CPU：bench taskset -c 2 → worker 亦全部 cpu=2（继承 cpumask），tid 变化但无跨 CPU 迁移

**(e) io_uring request ↔ block request**：明确不假设一一对应。本轮未采集 block 层
tracepoint：E1/E2 为 warm tmpfs read（无 block 请求）；E3 fsync 在 btrfs 上有真实
块活动但 block_rq_issue/complete 与 io_uring 请求为 N:M（合并/拆分），本轮未观测 →
见 §6 CORRELATION_PARTIAL。

### 5.3 附带直接观测（修正此前行为学推断）
- **tmpfs warm 4k read 100% 经过 io-wq**：qasync 6004/6004，且全部 complete 由
  worker 线程（非提交线程）投递。C3 的"99.6% submit 内完成"是用户态行为学判定
  （CQE 在 peek 时已就绪），与内核侧"io-wq 排队 + worker 极速完成"兼容——本观测
  为内核直接测量，修正其解释而非其数据。
- **B 臂 defer 机制**：defer 判定通过后 flush 并未减少（2000/phase 不变），deferred
  SQE 由紧随的路径吸收：1045/2000 的 FLUSH 直接前驱事件是 defer（判定后下一 poll
  的无条件 submit_transport 吸收），enter to_submit 恒 1（6004/6004）→ defer 在该
  负载上不产生批量提交，只把 flush 点从 submit 现场挪到 poll。
- io_uring_task_work_run 5495/5519 次（count 聚合批处理）；io_uring_defer（内核
  defer tracepoint）三轮均 0 次；io_uring_task_add 0 次；local_work_run 0 次
  （非 SQPOLL）。

## 6. CORRELATION_PARTIAL 声明（本关联构造的边界）

1. **block 层未关联**：tmpfs 无 block 请求；btrfs fsync 的 block 活动 N:M 未观测。
   不得据此对设备 I/O 段做逐请求延迟分解。
2. **内核执行段内部不可见**：qasync→complete 之间（io-wq worker 内部执行、
   锁等待、调度）无中间事件；E3 complete→reap p50 23µs/max 201µs 的构成
   （worker 调度 vs CQ 可见性）未分解——逐请求只到 enter→submit_req→qasync→
   complete→reap 的边界时刻。
3. **io_uring_defer / task_add / local_work_run / cqe_overflow / req_failed 路径未
   行使**（0 事件），其对应用户态决策（arm B defer）是**用户态 transport flush
   defer**，与内核 io_uring_defer 是不同层的不同机制，不得混用同名归因。
4. **statx 构造探针混入**：每 run 4（E3:2）个 user_data=0 的 submit（capability
   probe），已按 user_data=0+时间窗剔除，不计入请求链。
5. **bpftrace 谓词竞态**：create 前的 io_uring 事件（不可能存在——ring 先于一切请求）；
   task_work_run 以 pid 过滤，若 task work 在非 bench 线程上下文运行会漏（本轮无
   证据表明存在此类路径，计数 5495 与 enter 序列相容）。
6. 用户 take 打点在 RequestBridge::consume（library 公共取走点），代表"application
   take"的取走时刻；bench 侧结果校验（pattern verify）不在链上。

## 7. 仪器踩坑记录（影响复现，如实保留）

1. bpftrace v0.24.2：`pid == $1`（positional str 参数）与 int 比较静默永假（无编译
   错误、零事件）；改为脚本实例化（TARGET_PID 文本替换）后正常。
2. bpftrace 默认 perf ring（64 页/CPU）下异步 printf 丢 19221–21672 事件（两次
   探索 run，kernel.err 有 Lost 行，已废弃）；
   `BPFTRACE_PERF_RB_PAGES=2048` 后三轮零丢失。16384 页/CPU 会 std::bad_alloc
   （内存不足）。
3. fifo hold：非阻塞读端使 bench 的 fgets 立即返回 → bench 在 bpftrace attach 前
   跑完（表现为 kernel.txt 仅 BEGIN/END）；读端需转阻塞。
4. 首版 E3 bench 因 fsync phase 传了 stats 指针触发 stats wiring oracle 失败
   （rc=1），修复（传 nullptr）后 rc=0；该失败 run 未入证据（kernel 数据已覆盖）。

## 8. 复现

```
cd /home/jnhu/sluice-595-e5/causal-r2/worktree
SLUICE_595_TRACE_BUILD=1 xmake f -c -y --liburing=y -m release
SLUICE_595_TRACE_BUILD=1 xmake build -y bench_w02_pipeline
python3 -I tool_run_experiment.py <label> <A|B> \
    SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER=W02-uring-4k-d2 \
    SLUICE_BENCH_595_OPS=2000      # 需要 /tmp/s595_kernel.bt 与 sudo 密码管道
python3 -I tool_join_analysis.py <label>
```

每个实验目录内容：`kernel.txt`（bpftrace 全量行）、`kernel.err`（attach/丢失声明）、
`ev.bin`（用户事件环二进制，64 B/条，`S595_TRACE_DUMP` 行在 bench_stderr.txt）、
`bench_stdout.txt`/`bench_stderr.txt`、`run_meta.txt`（pid/arm/rc/env）。
