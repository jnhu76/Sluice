# Sluice Architecture Survival Map

> **状态**：FROZEN（SLUICE-ARCH-SURVIVAL-1 / PR1）
>
> - BASE：`baa6c91ce240b0890bfb3e6c12e917ba619be700`（`origin/master`，施工时重新 `git fetch origin && git rev-parse origin/master` 验证）
> - 方法：clean-room。矩阵冻结前未读取 #316–#320 的任何 verdict/diff/body，未用旧测试反推当前架构。
> - 本文只做 discovery / classification / evidence。不删除任何代码，不修改任何 production 语义。

## 1. 权威与证据顺序

```text
1. current production code + build definitions（include/ src/ apps/ xmake.lua xmake/）
2. docs/mission.md（FROZEN）
3. docs/adr/0001-explicit-io-design-doctrine.md（Accepted/Frozen）
4. docs/architecture.md（仅导航；与代码冲突时以代码为准）
5. current apps（四个真实消费者）
6. current tests created by THIS campaign（当前不存在，见 GAP-3）
7. historical tests / git history（PR1 冻结前禁用）
8. old issues / old PR conclusions（不得作为当前事实 authority）
```

判定工具与双方法纪律见 §8。单工具结论不判死；工具可能漏报（模板、虚调用、函数指针、宏、公共头、外部消费者兼容）时标 `UNKNOWN_INDEX_LIMITED`，不写 DEAD。

## 2. Supported build worlds（从 xmake 真实配置恢复）

| World | 激活方式 | 成员 | 证据 |
| --- | --- | --- | --- |
| W1 默认核心 | `xmake`（默认 target） | `sluice_core` = `src/*.cpp`（非递归 glob） | `xmake/libraries.lua:10-13` |
| W2 异步运行时 | opt-in（`set_default(false)`，group `async`） | `sluice_async` = `src/async/*.cpp`，依赖 `sluice_core` | `xmake/libraries.lua:19-36` |
| W3 应用 | `xmake -g apps` | `sluice-copy` / `sluice-hash` / `sluice-grep` / `sluice-tail`，依赖 core+async | `xmake/apps.lua` |
| G1 io_uring 门 | 无构建定义点 | `SLUICE_HAS_LIBURING` 在 `xmake.lua`/`xmake/` 中不存在定义；`uring_backend.cpp` 无宏时编译为不可用降级实现 | `rg SLUICE_HAS_LIBURING xmake*` → 0；`src/async/uring_backend.cpp:16,28` |
| G2 异步测试缝 | 无构建定义点 | `SLUICE_ASYNC_INTERNAL_TESTING` 门控的缝 TU（`mutex_test_seam.cpp`/`queue_test_seam.cpp`/`scheduler_fe2_test_seam.cpp`）与头内门控块；编译进库但未激活 | `rg SLUICE_ASYNC_INTERNAL_TESTING xmake*` → 0 |
| G3 核心文件缝 | 无构建定义点 | `SLUICE_FILE_INTERNAL_TESTING` 门控 `src/file.cpp` 的 close 脚本缝 | 同上方法 |
| G4 应用缝 | 无构建定义点 | `SLUICE_COPY_INTERNAL_TESTING` 门控 `apps/sluice-copy/safe_output.cpp` 的目录 fsync 脚本缝 | 同上方法 |
| 不在任何 world | — | `src/experimental/*.cpp`（两个 glob 均不覆盖该子目录）；`include/sluice/experimental/` 头公开但实现无构建归属 | `xmake/libraries.lua:6-8,25` |

基线验证（本 campaign 实测）：W1 debug 构建通过；W2+W3（`xmake -g apps`）构建通过；四应用 smoke（copy+cmp / hash / grep / tail）通过。

## 3. Roots

### 3.1 Public semantic roots

公共 API 可以独立成为 root，即使 apps 当前不用它；但位于 `include/` 不自动获得 root 资格，必须对应 REQUIRED capability（§4/§5 逐条证明）。本文认定的 public semantic roots：

```text
sluice::IoError / sluice::Result<T>                     （错误模型，两库共享）
sluice::Reader / Writer / IoSlice / ConstIoSlice        （同步字节流契约）
sluice::SyncableWriter                                  （durability 同步契约）
sluice::FileReader / FileWriter                         （同步文件/位置 I/O 实现）
sluice::IoContext / BlockingIoContext                   （同步资源获取工厂）
sluice::copy_all 家族 / CopyLimit / CopyOptions         （已赚到的组合契约，ADR-0001 §6 Copy）
sluice::BufferedReader / BufferedWriter / BufferedReadable（buffer participation 同步面）
sluice::async 四操作（ReadOp/WriteOp/SyncDataOp/SyncAllOp）+ AsyncIoContext 提交/轮询/等待/取消
sluice::async::Completion<T>                            （调用方持有的完成槽，六态权威）
sluice::async::AsyncBackend / BackendWaitSource          （后端契约；mission 原则 5）
sluice::async::ThreadPoolBackend                         （当前唯一真实执行后端）
sluice::async::ApplicationRuntime / RuntimeBuilder / RuntimeTaskContext
sluice::async::await_op_helpers（await_take/await_drain/await_read_fill/await_write_exact）
sluice::async::CancelToken                               （取消令牌）
sluice::async::TaskResultSlot / translate_task_exception（任务结果搬运）
```

### 3.2 Application roots

```text
apps/sluice-copy   apps/sluice-hash   apps/sluice-grep   apps/sluice-tail
```

四应用的主执行路径一致消费：`RuntimeBuilder(ThreadPoolBackend)` → `ApplicationRuntime::start/submit/drain/join` → 任务体 `RuntimeTaskContext` + `await_op_helpers` + `TaskResultSlot`；错误经 `IoError/Result` 映射退出码。`sluice-copy/safe_output.cpp` 另有一处内部依赖：直接 include `<sluice/detail/posix_retry.hpp>`。

### 3.3 Runtime roots

被 public/application root 合法到达的内部 runtime authority：

```text
Scheduler（run/run_live/park/wake/await_completion_*/waiter 路由/wake handle/单调时钟与 deadline heap）
Fiber / fiber_ctx（状态机 created→runnable→running→waiting→done，手写汇编切换）
Group（ApplicationRuntime 根组；Scheduler 绑定时走 fiber/evented 路径）
Future<void>（Group 内部结果通道，经 WaitPolicy/EventedWaitPolicy 等待）
detail::RequestArena / RequestSlot / RequestKey / submit_transaction / ready ring
detail::SynchronousReadySink / ReadyEvent / WaiterToken / RoutingLease（就绪路由契约）
detail::ReadyWaitSource / ReferenceReadySink（ThreadPoolBackend 内部件）
sluice::detail::io_validation（posix 部分）/ posix_retry（EINTR 重试）
async::Mutex / LockGuard / thread_annotations（调度器内部 std::mutex 薄包装 + TSA）
wait_node / wait_queue / timer_registration（等待节点/队列/定时器登记）
```

### 3.4 Config-gated worlds（代码存在、当前构建不激活）

```text
G1  UringAsyncBackend + detail::UringWaitSource + uring_test_seams（SLUICE_HAS_LIBURING）
G2  async 内部测试缝（seam TU + scheduler_test_access + tax0_ablation_seams + 各头内门控块）
G3  file close 脚本缝（file_test_seams.hpp）
G4  sluice-copy 目录 fsync 脚本缝（safe_output_test_seams.hpp）
```

### 3.5 Test roots

当前树不存在任何 test target（`xmake/helpers.lua` 的 `sluice_one_file_target` 无调用者，见 §7 B01）。Test roots 将由 PR2 按 admission rule 建立；PR1 不预支任何 test root。

## 4. Capability 冻结清单

从 frozen mission（语义内容清单 §1 + 六原则 + ADR-0001）独立定义，不从现有 class 列表反推。状态调查后裁决：

| ID | Capability | 裁决 | 依据 |
| --- | --- | --- | --- |
| K01 | 异步位置 I/O：READ / WRITE | REQUIRED | mission"可观察 I/O effect"；四应用主路径 |
| K02 | durability 操作：SYNC_DATA / SYNC_ALL | REQUIRED | mission"durability"；`sluice-copy --sync*` |
| K03 | request lifecycle：admission→outstanding→terminalization→publication→reuse | REQUIRED | mission"accepted / terminal publication / reuse 等可观察异步语义" |
| K04 | resource identity / lifetime（fd、context identity、slot generation） | REQUIRED | mission"resource identity / lifetime" |
| K05 | buffer participation / lifetime（borrow 活动窗口） | REQUIRED | mission"buffer participation 与 lifetime" |
| K06 | resource bounds：request capacity、outstanding 记账、backend admission | REQUIRED | mission"资源有界/Named bounds" |
| K07 | completion：exactly-once publication + reset/reuse | REQUIRED | mission"terminal publication / reuse" |
| K08 | cancellation：token、completion cancel、waiter cancel | REQUIRED | mission"cancellation"；sluice-tail Ctrl-C 路径 |
| K09 | wait / wake（park/wake、split-wait、wake handle） | REQUIRED | 调度与后端等待的 correctness 基础 |
| K10 | deadline / timer（单调时钟、deadline heap、限时等待） | REQUIRED | mission 明文"deadline 等可观察异步语义"；**witness gap 见 GAP-1** |
| K11 | task execution / scheduling（Fiber、worker 拓扑、run_live） | REQUIRED | 应用任务执行的基础 |
| K12 | replaceable backend execution（AsyncBackend 契约） | REQUIRED | mission 原则 5"执行可换" |
| K13 | task result transfer（TaskResultSlot） | REQUIRED | 四应用任务结果搬运 |
| K14 | task composition：Group | REQUIRED | ApplicationRuntime 根组 |
| K15 | 同步核心 I/O（Reader/Writer/File/IoContext 阻塞面） | REQUIRED | ADR-0001：sluice_core 是默认库世界，async opt-in 正是为保持阻塞默认面不含异步 |
| K16 | copy 组合契约（copy_all 家族） | REQUIRED（已赚到） | ADR-0001 §6 Copy 正向结论：合法 transformation boundary |
| K17 | 同步 buffer participation（BufferedReader/Writer 快路径） | REQUIRED | copy_all 的 buffered fast path 依赖 `BufferedReadable` 探测（`src/copy.cpp:57`） |
| K18 | durability 同步面（SyncableWriter） | REQUIRED | mission durability；FileWriter 实现 |
| K19 | 错误模型（IoError/Result） | REQUIRED | 两库与四应用共同错误通道 |
| K20 | observation / statistics | OPTIONAL | mission 边界类别 HINT/OBSERVATION 被允许但未被要求；当前零读者（GAP-2） |
| K21 | Future 结果通道 | DERIVED | Group 内部机制，非独立 root |
| K22 | wait policy（Threaded/Evented） | DERIVED | Group/Future 的等待策略插拔 |
| K23 | 批量提交（Batch group submission） | NOT_EARNED | mission 明文：generalized Batch control layer 未被证明有价值；零消费者 |
| K24 | 请求身份查询（RequestHandle/request_state 公共链） | NOT_EARNED | mission 未授予查询授权；全链零调用者（§7 A07） |
| K25 | select 组合 | NOT_EARNED | mission 未授予该组合契约；零消费者；与 K10 表达交叉（GAP-1） |
| K26 | 异步同步原语公共面（Semaphore/AsyncMutex/AsyncCondition/AsyncRwLock/AsyncQueue/Event） | NOT_EARNED | mission 未授予；作为独立公共面零消费者；但它们是 K10 的唯一公共表达载体（GAP-1） |
| K27 | 合成后端（SyncBackend/FakeAsyncBackend） | NOT_EARNED | 零消费者；W1–W3 无任何调用 |
| K28 | WAL 记录格式 | NOT_EARNED | mission 未命名；零消费者 |
| K29 | 内存 I/O 面（MemoryIoContext/MemoryReader/MemoryWriter） | NOT_EARNED | 零消费者 |
| K30 | 故障注入面（FaultPlan/FaultReader/FaultWriter） | NOT_EARNED | 测试机制置于公共面；零消费者；PR2 将用 test-local 机制替代（taskbook §21） |
| K31 | 同步阻塞线程池（BlockingIoPool/Task\<T\>） | NOT_EARNED | 零消费者；异步域已有独立 ThreadPoolBackend 派发机制 |
| K32 | 实验性 uring 层（UringIoContext/UringWriteBatch） | OUT_OF_SCOPE | 不属于任何 supported build world（§2） |

## 5. Capability → Semantic API / Authority 映射

仅 REQUIRED 项。格式：capability → 公共 API root / 内部 owner / correctness invariant / resource bound / 执行机制 / 可观察结果。

### K01 异步 READ/WRITE

```text
Public API root : RuntimeTaskContext::submit_read/submit_write
                  AsyncIoContext::submit_read/submit_write（ReadOp{fd,dst,len,offset} / WriteOp）
                  await_op_helpers::await_read_once/await_read_fill/await_write_exact
Internal owner  : ThreadPoolBackend::submit_size → detail::submit_transaction
Correctness     : Completion 六态权威（idle→binding→outstanding→publishing→ready→resetting）
                  + RequestArena 槽位状态机（free→reserved→prepared→pending→enqueued→running→backend_ready→completion_ready）
Resource bound  : RequestArena capacity（默认 64）→ 超限拒绝 IoError::would_block
                  ThreadPoolBackend::BoundedDispatchQueue（容量 = request_capacity）
Execution       : worker 线程阻塞 pread/pwrite（EINTR 重试、64 位 off_t 校验）
Observable      : Result<size_t>（字节数或 IoError）恰好一次发布进调用方持有的 Completion
```

### K02 SYNC_DATA/SYNC_ALL

```text
Public API root : RuntimeTaskContext::submit_sync_data/submit_sync_all；AsyncIoContext 同名
Internal owner  : ThreadPoolBackend::submit_void → fdatasync/fsync 路径（src/async/threadpool_backend.cpp run_syscall）
Correctness     : 与 K01 同一 arena/completion 权威；Result<void>
Resource bound  : 同 K01（共享 arena 容量）
Observable      : 持久化成功的 Result<void>；sluice-copy --sync-data/--sync-all 消费
```

### K03 request lifecycle

```text
Public API root : AsyncIoContext::submit_* + Completion<T>（reset 复用）
Internal owner  : detail::submit_transaction（reserve→validate→prepare→install_publication_binding
                  →begin_binding→commit→install_binding→commit_binding→enqueue）
Correctness     : 提交事务任一阶段失败回滚到 idle 且槽位归还；reuse 经 generation+1 防陈旧认领
Resource bound  : reserve 失败即拒绝（capacity_rejections 计数）；admission_closed 终止新提交
Observable      : 每个被接受的请求恰好产生一次 terminal publication；Completion 可 reset 后复用
```

### K04 resource identity / lifetime

```text
Public API root : 操作结构体内 fd；FileReader/FileWriter 持有 fd 生命周期（同步面）
Internal owner  : RequestArena::ContextIdentity + RequestKey{context,slot,generation}
Correctness     : validate_ 拒绝跨 context / 陈旧 generation 的槽位操作
Observable      : fd 在请求存续期保持有效是调用方契约；库不复制 fd 语义
```

### K05 buffer participation / lifetime

```text
Public API root : ReadOp::dst / WriteOp::src（span 参与窗口 = 提交到 terminal）
Internal owner  : RequestSlot::BorrowMetadata{fd,address,length,active}
Correctness     : borrow.active 从 commit 置位、reap 内清零（arena 锁内有序：borrow 结束先于 publish）
Observable      : terminal 之前调用方不得释放缓冲；越窗使用被 arena 状态机拒绝
```

### K06 resource bounds

```text
Public API root : ThreadPoolConfig{request_capacity, worker_count}
Internal owner  : RequestArena（容量/记账/high_water_mark/capacity_rejections）
                  + BoundedDispatchQueue（容量/high_water）
Correctness     : 容量拒绝映射 IoError::would_block；不静默排队
Observable      : 饱和是可观察失败而非无界内存增长
```

### K07 completion exactly-once

```text
Public API root : Completion<size_t>/Completion<void>（ready/result/reset）
Internal owner  : publish_from_reap 仅接受 outstanding→publishing→ready 单向迁移
Correctness     : 违规生命周期（binding 析构、outstanding 析构/reset、重复发布）fail-fast 终结
                  release_completed_binding 归还槽位（generation+1）
Observable      : 恰好一次结果发布；reset 后槽位可安全复用
```

### K08 cancellation

```text
Public API root : CancelToken（request/is_requested/epoch/rearm）；AsyncIoContext::cancel
                  RuntimeTaskContext::cancel_waiter
Internal owner  : RequestArena::cancel（pending/enqueued→terminal canceled；running→cancel_intent）
                  Scheduler::cancel_waiter + WaitRecord cancelled 路由
Correctness     : cancel 与 terminal 竞争单胜（CancelDisposition）；已 terminal 的 cancel 为 no-op
Observable      : IoError::canceled；sluice-tail follow 模式 Ctrl-C→request_stop→token
```

### K09 wait / wake

```text
Public API root : Scheduler 公共面（await_completion_*、await_wait、wake_wait_one）
Internal owner  : park_on_wake_source + BackendWaitSource（split-wait：progress/control generation）
                  + SchedulerWakeHandle（外部唤醒）
Correctness     : park 前观察后验证（token 比较），唤醒不丢失；WaitRecord delivered 单次投递
Observable      : 等待中的 Fiber 在完成发布后被重新调度；RuntimeBuilder 校验后端必须提供
                  wait_source 或 nonblocking wait_one（application_runtime.cpp:101-104）
```

### K10 deadline / timer

```text
Public API root : Scheduler 单调时钟（monotonic_now/advance_clock）与限时等待
                  （sem/mutex/condition/queue/rwlock 的 *_until 变体、await_wait_deadline）
Internal owner  : deadline heap（heap_push/pop/sift、earliest_active_deadline、pump_deadlines）
                  + TimerRegistration 生命周期（arm/consume/retire）
Correctness     : 到期与手动唤醒单胜；earliest deadline 驱动 park 上界
Observable      : 限时等待超时返回（目前**无任何在树消费者**——GAP-1）
```

### K11 task execution / scheduling

```text
Public API root : ApplicationRuntime::start/submit/drain/join/shutdown
Internal owner  : Scheduler worker 拓扑 + Fiber 状态机 + 汇编上下文切换（fiber_ctx）
Correctness     : ApplicationRuntime 生命周期状态机
                  （Constructed→Starting→Running→Stopping/Draining→Stopped/StartFailed/Fatal）
                  + driver 线程 DriverState；任务经根 Group 以 fiber 运行
Observable      : 任务恰好执行一次；drain 后 join 干净退出；异常不逃逸任务边界
```

### K12 replaceable backend execution

```text
Public API root : AsyncBackend 契约（submit_*/poll/wait_one/cancel/register_waiter/wait_source）
                  + RuntimeBuilder::backend(unique_ptr<AsyncBackend>)
Internal owner  : 契约默认实现（不支持的返回 not_supported）；门面只依赖契约
Correctness     : 后端不得反向定义公共语义（mission 原则 5）；能力探测（split-wait/bounded park）
                  由门面查询而非类型判断
Observable      : W3 四应用全部经 RuntimeBuilder 注入 ThreadPoolBackend；UringAsyncBackend
                  在 G1 world 实现同一契约
```

### K13 task result transfer

```text
Public API root : TaskResultSlot<T>（publish/wait_and_take）+ translate_task_exception
Internal owner  : 互斥 + 条件变量搬运；异常翻译为 IoError
Correctness     : 恰好一次发布；任务线程到调用线程的所有权转移
Observable      : 四应用 main 取结果并映射退出码
```

### K14 Group

```text
Public API root : Group::async/await/cancel/group_token
Internal owner  : Scheduler 绑定时 evented 路径（Fiber+EventedWaitPolicy），否则线程路径
Correctness     : 事件化接纳为事务式（fiber/stack/future 存储先 reserve 后 commit，失败回滚）
Observable      : await 等待全部完成；cancel 发布 token 后等待
```

### K15–K19 同步核心（阻塞默认面）

```text
K15 Reader/Writer/IoContext : read_some/read_exact/read_vec*/stream_to、write_some/write_all/
                              write_vec*/flush；BlockingIoContext::open_reader/open_writer
                              产出 FileReader/FileWriter（open 错误延迟报告）
K16 copy_all                : buffered 快路径（BufferedReadable 探测）与 scratch 路径；
                              CopyLimit 限量；CopyDecision 观测选择
K17 BufferedReader/Writer   : 调用方提供缓冲；写侧析构断言无脏数据；peek/consume_buffered
K18 SyncableWriter          : sync_data/sync_all；FileWriter 实现映射 fdatasync/fsync
K19 IoError/Result          : 11 Code + os_errno；from_errno_value 映射；两库共享
Correctness（同步面）        : Result 即时返回；EINTR 重试；64 位 off_t 静态断言；
                              打开失败延迟报告（open_error()）
Resource bound（同步面）     : 调用方提供 span/scratch——无隐藏缓冲预算
```

## 6. 执行路径证明（retained semantic API 逐链）

路径必须用当前代码证明：caller/root → symbol references → build membership → concrete implementation → terminal observable effect。全部路径在 W2/W3 构建成员内（§2）。

### 6.1 异步 read 全链（write/sync 同构）

```text
caller            apps 任务体（如 apps/sluice-copy/copy_task.cpp:await_read_fill）
  → await_op_helpers::await_read_once（src/async/await_op_helpers.cpp）
  → RuntimeTaskContext::submit_read（src/async/application_runtime.cpp:25）
  → AsyncIoContext::submit_read（src/async/async_io_context.cpp；access_mtx_ 串行化）
  → ThreadPoolBackend::submit_read → detail::submit_transaction（include/sluice/async/detail/submit_transaction.hpp）
      reserve → validate_op → prepare → install_publication_binding → begin_binding
      → commit → install_binding → commit_binding → enqueue_after_commit
  → BoundedDispatchQueue::push_back + work_cv_.notify_one
  → backend worker：worker_loop → run_syscall（阻塞 pread；EINTR 重试、checked_posix_offset）
  → RequestArena::record_terminal（槽位→backend_ready，挂 ready ring）
驱动侧（三条合法驱动点，均已在树内激活）：
  (a) Scheduler worker 循环 ctx_.poll()（src/async/scheduler.cpp:764）
  (b) AsyncIoContext::wait_one(max_park)（split-wait 有界 park；src/async/async_io_context.cpp:173）
  (c) Scheduler::park_on_wake_source 经 BackendWaitSource 唤醒后再 poll
  → ThreadPoolBackend::poll → RequestArena::reap（arena 锁内 publish，锁外 sink.on_ready）
  → Completion::publish_from_reap（outstanding→publishing→ready）
  → ReadyRoutingSink::on_ready（WaitRecord delivered）
  → Fiber runnable → await_completion 返回 → 任务体读 result()
```

三阶段 correctness boundary（与 docs/architecture.md §7 一致，本 campaign 在代码中独立复核）：
backend 终结化（`record_terminal`，槽位 backend_ready）≠ Completion ready（`reap` 内 publish）≠ Fiber 恢复（就绪路由后）。reap 内顺序固定：锁内 publish、锁外路由。

### 6.2 durability（sync_data/sync_all）

```text
sluice-copy --sync-data/--sync-all → copy_task → RuntimeTaskContext::submit_sync_*
  → 同 6.1 事务 → run_syscall 的 sync 分支（fdatasync/fsync）→ Result<void> 发布
同步面：FileWriter::sync_data/sync_all（src/file.cpp）→ ::fdatasync/::fsync（EINTR 重试）
```

### 6.3 cancellation

```text
sluice-tail follow：sigwait 信号线程 → ApplicationRuntime::request_stop
  → runtime 取消发布（root_cancel_published）→ RuntimeTaskContext::cancel_token().is_requested()
  → 任务体协作退出（apps/sluice-tail/tail_task.cpp:141,198,236）
Completion 级：AsyncIoContext::cancel → ThreadPoolBackend::cancel → arena.cancel
  （pending/enqueued → terminal canceled；running → cancel_intent）
Waiter 级：RuntimeTaskContext::cancel_waiter → Scheduler::cancel_waiter → WaitRecord cancelled
```

### 6.4 wait / wake / split-wait

```text
Fiber park：await_completion_* → WaitRecord 注册（wait_capacity 上界，默认 256）
  → 无进展时 park_on_wake_source
  → BackendWaitSource::snapshot/wait_for_change（progress/control 双 generation token）
  → 唤醒后重观察；bounded park 需 has_bounded_split_wait_capability（src/async/scheduler.cpp:465-539）
外部唤醒：SchedulerWakeHandle（ApplicationRuntime 与 EventedWaitPolicy 持有）
关闭路径：close_admission → arena.close_admission → 提交拒绝 invalid_state
```

### 6.5 task lifecycle / result transfer

```text
main → RuntimeBuilder().backend(ThreadPoolBackend).workers(n).build()
  （校验：无 wait_source 且非 nonblocking 的后端拒绝构建）
  → start()（driver 线程 + worker 拓扑 + 根 Group）
  → submit(task)（admission 开关 + admitted/terminal 计数）
  → 任务体经 TaskResultSlot 发布 → main drain()/join() → wait_and_take()
```

### 6.6 同步核心路径

```text
BlockingIoContext::open_reader（src/io_context.cpp）→ FileReader（src/file.cpp：open/pread/read，统计挂钩）
copy_all（src/copy.cpp）：buffered 快路径（dynamic_cast<BufferedReadable*>，src/copy.cpp:57）
  或 scratch 路径（read_some/write_all 循环）至 EOF 或 CopyLimit
BufferedReader/Writer（src/buffer.cpp）：调用方缓冲；写侧析构断言无脏数据
WAL/内存/故障/观测包装：仅自实现 TU 消费，无外部调用者（§7 S09–S12）
```

### 6.7 backend 替换性

```text
W3 四应用 → RuntimeBuilder::backend(std::make_unique<ThreadPoolBackend>(...))
G1 world（未激活）：UringAsyncBackend 实现同一 AsyncBackend 契约
合成后端（SyncBackend/FakeAsyncBackend）：实现契约但零注入点（§7 A12/A13）
```
