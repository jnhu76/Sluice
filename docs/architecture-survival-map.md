# Sluice Architecture Survival Map

> **状态**：FROZEN（SLUICE-ARCH-SURVIVAL-1 / PR1；Commit 5 含三轮对抗审计纠正）
>
> - BASE：`baa6c91ce240b0890bfb3e6c12e917ba619be700`（`origin/master`，施工时重新 `git fetch origin && git rev-parse origin/master` 验证）
> - 方法：clean-room。矩阵冻结前未读取 #316–#320 的任何 verdict/diff/body，未用旧测试反推当前架构。
> - 本文只做 discovery / classification / evidence。不删除任何代码，不修改任何 production 语义。
> - 冻结后经三轮独立对抗审计（质量审计 / false-deletion 对抗者 / zombie-code 对抗者）。主要纠正：K15/K18 降为 OPTIONAL（争议）、S02/S03/S04/S06/S08 改判 UNKNOWN、G2 当前不可激活的事实、kept→legacy 编译耦合表（§8.1）、多处证据字符串修正。

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
| G2 异步测试缝 | 无构建定义点，且**当前不可激活** | `SLUICE_ASYNC_INTERNAL_TESTING` 门控的缝 TU 与头内门控块；编译进库但未激活。定义该宏会立即破坏编译：11 个生产 TU 在门内 include 的 `async_test_control_internal.hpp` 不存在于树中，`sluice_async_test::test_phase` 等无 provider | xmake 中无宏定义点（`rg SLUICE_ASYNC_INTERNAL_TESTING xmake*` 唯一命中为 `apps.lua:4` 的注释，佐证应用不定义它）；`src/async/scheduler.cpp:18` 等 11 处 include 缺失头 |
| G3 核心文件缝 | 无构建定义点 | `SLUICE_FILE_INTERNAL_TESTING` 门控 `src/file.cpp` 的 close 脚本缝 | 同上方法 |
| G4 应用缝 | 无构建定义点 | `SLUICE_COPY_INTERNAL_TESTING` 门控 `apps/sluice-copy/safe_output.cpp` 的目录 fsync 脚本缝 | 同上方法 |
| 未登记行为门 | 无定义点 | 两个生产 TU 内嵌行为突变门：`SLUICE_TV1_C001_MUTANT`（`src/async/scheduler_park_wake.cpp:757`）、`SLUICE_TV1_C012_MUTANT`（`src/async/uring_backend.cpp:1371`）；均不引用 §9.2 簇 | rg 实证 |
| 不在任何 world | — | `src/experimental/*.cpp`（两个 glob 均不覆盖该子目录）；`include/sluice/experimental/` 头公开但实现无构建归属 | `xmake/libraries.lua:6-8,25` |

基线验证（本 campaign 实测）：W1 debug 构建通过；W2+W3（`xmake -g apps`）构建通过；四应用 smoke（copy+cmp / hash / grep / tail）通过。

## 3. Roots

### 3.1 Public semantic roots

公共 API 可以独立成为 root，即使 apps 当前不用它；但位于 `include/` 不自动获得 root 资格，必须对应 REQUIRED capability（§4/§5 逐条证明）。本文认定的 public semantic roots：

```text
sluice::IoError / sluice::Result<T>                     （错误模型，两库共享）
sluice::Reader / Writer / IoSlice / ConstIoSlice        （copy_all 组合契约的载体类型面）
sluice::copy_all 家族 / CopyLimit                        （已赚到的组合契约，ADR-0001 §6 Copy）
sluice::BufferedReader / BufferedWriter / BufferedReadable（copy 快路径 load-bearing）
sluice::async 四操作（ReadOp/WriteOp/SyncDataOp/SyncAllOp）+ AsyncIoContext 提交/轮询/等待/取消
sluice::async::Completion<T>                            （调用方持有的完成槽，六态权威）
sluice::async::AsyncBackend / BackendWaitSource          （后端契约；mission 原则 5）
sluice::async::ThreadPoolBackend                         （当前唯一真实执行后端）
sluice::async::ApplicationRuntime / RuntimeBuilder / RuntimeTaskContext
sluice::async::await_op_helpers（await_take/await_drain/await_read_once/await_read_fill/await_write_exact）
sluice::async::CancelToken                               （取消令牌）
sluice::async::TaskResultSlot / run_task_to_result / translate_task_exception（任务结果搬运）
```

对抗审计裁决（zombie-code 对抗者）：同步面中 `SyncableWriter`/`FileReader`/`FileWriter`/`IoContext`/`BlockingIoContext`/`CopyStrategy`/`CopyDecision`/统计结构的 LIVE_PUBLIC 归因证据不足（K15 争议，见 §4/§9.3-4），root 资格待人工裁决，分类改判 UNKNOWN（§7.2）。上表只保留归因存活的 root。

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
| K15 | 同步核心 I/O（Reader/Writer/File/IoContext 阻塞面） | OPTIONAL（**争议**） | mission 语义清单不区分同步/异步面；ADR-0001 唯一涉及 blocking 的文字（ADR:173，§5 执行可换）把 threaded blocking 定位为**机制**而非契约；"sluice_core 默认构建"是 build 事实（libraries.lua）而非 mission 授权；四应用零消费同步面。zombie-code 对抗者据此推翻 REQUIRED 归因；保留为公共库表面属合法但未赚满的状态，最终裁决属人工（§9.3-4） |
| K16 | copy 组合契约（copy_all 家族） | REQUIRED（已赚到） | ADR-0001 §6 Copy 正向结论：合法 transformation boundary（注意： earns 的是 thin local branch，见 S06 争议） |
| K17 | 同步 buffer participation（BufferedReader/Writer 快路径） | REQUIRED | copy_all 的 buffered fast path 依赖 `BufferedReadable` 探测（`src/copy.cpp:57`）；作为 K16 载体派生 |
| K18 | durability 同步面（SyncableWriter） | OPTIONAL（**争议**） | durability 语义本体由 K02（异步载体，REQUIRED）独立成立；同步面载体与 K15 同属争议；FileWriter 是其唯一消费者 |
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
Internal owner  : detail::submit_transaction（reserve→validate→prepare→write_scratch
                  →install_publication_binding→begin_binding→commit→install_binding
                  →commit_binding）；入队是事务之后的独立步骤
                  ThreadPoolBackend::enqueue_after_commit（threadpool_backend.cpp:253，
                  在 admission_mtx_ 锁外执行）
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

### K15–K19 同步核心（载体争议见 §4 K15/K18）

```text
K15 Reader/Writer/IoContext : read_some/read_exact/read_vec*/stream_to、write_some/write_all/
                              write_vec*/flush；BlockingIoContext::open_reader/open_writer
                              产出 FileReader/FileWriter（open 错误延迟报告）
K16 copy_all                : buffered 快路径（BufferedReadable 探测）与 scratch 路径；
                              CopyLimit 限量
K17 BufferedReader/Writer   : 调用方提供缓冲；写侧析构断言无脏数据；peek/consume_buffered
K18 SyncableWriter          : sync_data/sync_all；FileWriter 实现映射 fdatasync/fsync
K19 IoError/Result          : 11 Code + os_errno；from_errno_value 映射；两库共享
Correctness（同步面）        : Result 即时返回；EINTR 重试；64 位 off_t 静态断言；
                              打开失败延迟报告（open_error()）
Resource bound（同步面）     : 调用方提供 span/scratch——无隐藏缓冲预算
在树消费者                  : 四应用与异步库均零消费同步语义面（仅 error/result/
                              io_validation/posix_retry/measurement 五个基础头被跨面消费）
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
      reserve → validate_op → prepare → write_scratch → install_publication_binding
      → begin_binding → commit → install_binding → commit_binding
      （事务至此结束；随后 ThreadPoolBackend::enqueue_after_commit 在 admission_mtx_ 锁外入队）
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

## 7. 组件簇生存分类

每个重要 abstraction / public symbol / backend / helper cluster 恰好进入一个主状态。证据缩写：`rg`=全树精确文本搜索（含 apps/、xmake、条件编译块），`nm`=构建产物符号表（W1/W2 debug 实测），`caller`=构造点/调用点搜索。详细命令与输出见 §8。

### 7.1 异步运行时（W2）

| ID | 组件簇 | 分类 | 证据要点 |
| --- | --- | --- | --- |
| A01 | `error.hpp`/`result.hpp`（IoError/Result） | LIVE_PUBLIC | 两库全部 TU + 四应用 include |
| A02 | 四操作结构 + `AsyncIoContext` 提交/轮询/等待/取消/split-wait 面 | LIVE_PUBLIC | apps→runtime→facade 全链（§6.1） |
| A03 | `Completion<T>` 六态权威 | LIVE_CORRECTNESS | 所有提交/发布/复用路径的所有权边界 |
| A04 | `detail::RequestArena/RequestSlot/RequestKey/submit_transaction` | LIVE_RESOURCE_BOUND + LIVE_CORRECTNESS | threadpool 提交事务与 reap 发布的唯一状态权威 |
| A05 | `detail::ready_sink.hpp`（SynchronousReadySink/ReadyEvent/WaiterToken/RoutingLease） | LIVE_CORRECTNESS | arena→scheduler 就绪路由契约 |
| A06 | `AsyncBackend` 契约（submit/poll/wait/cancel/register_waiter/wait_source） | LIVE_BACKEND | 门面唯一依赖；RuntimeBuilder 注入点 |
| A07 | `RequestHandle` + `submit_*_request` + `request_state` + 契约 identity 钩子（`resolve_identity_state`/`supports_request_identity`/`RequestHandleState`）+ `RuntimeTaskContext::submit_*_request` 转发 | LEGACY_UNJUSTIFIED | `request_state(` 全树零调用者；`submit_*_request` 零调用者（出现点限于簇自身文件：async_io_context.{hpp,cpp}、application_runtime.{hpp,cpp}、request_handle.cpp 的 identity 链）。注意删除需同步修剪保留头中的契约钩子（async_io_context.hpp:125-136、threadpool_backend.hpp:46-51、uring_backend.hpp:76-81）；公共 API——source-compat 见 §9.3-1 |
| A08 | `BackendWaitSource` 契约 | LIVE_BACKEND | split-wait 权威；RuntimeBuilder 构建校验消费 |
| A09 | `detail::ReadyWaitSource`/`ReferenceReadySink` | LIVE_BACKEND | ThreadPoolBackend 内部件 |
| A10 | `detail::UringWaitSource` | LIVE_CONFIG_GATED | 唯一消费者 UringAsyncBackend（G1 world） |
| A11 | `ThreadPoolBackend`（含 BoundedDispatchQueue） | LIVE_BACKEND | 四应用唯一真实执行后端 |
| A12 | `SyncBackend` | LEGACY_UNJUSTIFIED | rg：仅自身头文件；无 TU（header-only）；nm：libsluice_async.a 0 符号 |
| A13 | `FakeAsyncBackend` + `src/async/fake_test_seams.hpp` | LEGACY_UNJUSTIFIED | rg：仅自身头 + 其缝头（缝头由 fake_backend.hpp:499 在 G2 门内自 include）；nm：0 符号；G2 当前不可激活（§2）；apps README 中的 fault-test 描述为无实现对应的文档 |
| A14 | `UringAsyncBackend` + `src/async/uring_test_seams.hpp` | LIVE_CONFIG_GATED | G1 world 未激活；降级实现编译进 W2（nm 23 符号）；mission 原则 5 点名 io_uring 为合法机制 |
| A15 | `ApplicationRuntime`/`RuntimeBuilder`/`RuntimeTaskContext`（存活性部分） | LIVE_PUBLIC | 四应用入口 |
| A16 | `Scheduler` 核心（run/park/wake/await_completion/waiter 路由/wake handle） | LIVE_RUNTIME | runtime 与 fiber 调度的中央权威 |
| A17 | `Scheduler` deadline/timer 机制（deadline heap + TimerRegistration） | LIVE_RUNTIME | K10 唯一 owner；heap 的入口类型 `DeadlineHeapEntry` 定义于 A29 簇头 `detail/select_registration.hpp:74`（编译耦合见 §8.1）；在树能 arm deadline 的代码全部位于 LEGACY 簇（A29–A34）——live 树中 heap 可证明恒空（GAP-1） |
| A18 | `Fiber` + `fiber_ctx`（汇编切换） | LIVE_RUNTIME | Group evented 路径 + worker 执行 |
| A19 | `wait_node`/`wait_queue`/`timer_registration` | LIVE_RUNTIME | scheduler 等待/定时登记 |
| A20 | `mutex.hpp`/`lock_guard.hpp`/`thread_annotations.hpp` | LIVE_RUNTIME | scheduler/queue_port 内部锁 + TSA |
| A21 | `Group` | LIVE_RUNTIME | ApplicationRuntime 根组（application_runtime.cpp:114）。死半边记录：默认构造 `Group()`（无 Scheduler）与线程路径 `async_threaded`、`await()`/析构的线程分支（group.cpp:60-73,90-103）在树内不可达——树内唯一构造点恒为 Scheduler 绑定构造 |
| A22 | `Future<T>` | LIVE_RUNTIME | 唯一消费者 Group（K21 DERIVED）。死半边记录：`Future::await()`/`cancel()`/`cancel_token()` 零调用（evented 路径只轮询 `ready()` 并驱动调度器，group.cpp:30-39）；`Future<void>` 只承载成功值（任务体吞异常） |
| A23 | `wait_policy`/`evented_wait_policy` | LIVE_RUNTIME | Group/Future 等待策略。死半边记录：策略接口的定义操作 `wait_until_ready` 在 live 树中执行零次（唯一活分发是 `EventedWaitPolicy::notify_ready`）；`ThreadedWaitPolicy`/`default_wait_policy()` 仅被死的线程路径引用——僵尸审计指其为假插拔 |
| A24 | `CancelToken` | LIVE_PUBLIC | sluice-tail 消费；runtime 根取消 |
| A24b | `CancelState`/`CancelGuard`/`check_cancel` | LEGACY_UNJUSTIFIED | `check_cancel`/`CancelGuard` 零调用者；`CancelState` 的机制（`acknowledge`/`swap_protection`）仅被 `check_cancel` 调用而后者零调用。**编译耦合**：保留的 `Fiber` 内嵌 `CancelState cstate_` 与访问器（fiber.hpp:3,55,74，机制功能上死）——删除须先摘除该成员；见 §8.1 |
| A25 | `task_result.hpp`（TaskResultSlot/translate_task_exception） | LIVE_PUBLIC | 四应用 |
| A26 | `await_op_helpers` | LIVE_PUBLIC | 四应用任务体 |
| A27 | `op_helpers`（read_all/write_all/sync_data_all/sync_all_all） | LEGACY_UNJUSTIFIED | rg：自身 TU 外零引用；nm：编译符号存在但零调用 |
| A28 | `Batch` | LEGACY_UNJUSTIFIED | mission 明文否定（K23）；rg：仅 batch.{hpp,cpp} + completion.hpp friend（friend 声明本身合法，删除后需卫生清理）；零消费者 |
| A29 | select 簇（`select.hpp`/`select_fwd`/`select*.cpp`/`detail::select_port`/`select_registration` + Scheduler 内 select_* 机制） | LEGACY_UNJUSTIFIED | `select(` 自由函数全树零调用者。**编译耦合**（false-deletion 对抗者核实）：保留的 `scheduler.hpp:8-9` 无条件 include `select_fwd.hpp`/`select_registration.hpp`；A17 deadline heap 入口类型 `DeadlineHeapEntry` 定义在本簇头 select_registration.hpp:74；保留的 `~Scheduler()` fail-fast 与 park 记账读写 select 状态（scheduler.cpp:84-117、scheduler_park_wake.cpp:137,342，无门控）——删除需雕刻保留的 scheduler 核心（§8.1）；与 K10 表达交叉（GAP-1） |
| A30 | `Semaphore` + `Scheduler::sem_*` + `scheduler_semaphore.cpp` | LEGACY_UNJUSTIFIED | `Semaphore` 类型自身头外零引用（nm 0 符号）；`Scheduler::sem_*` 仅被 semaphore.hpp 消费；簇含调度器侧机制与 TU（僵尸审计补界）；K10 交叉（GAP-1） |
| A31 | `AsyncMutex` + `AsyncCondition` + `Scheduler::mutex_*`/`condition_*` + `scheduler_mutex.cpp`/`scheduler_condition.cpp` | LEGACY_UNJUSTIFIED | 两头互引，外部零引用（其余出现仅为 assert 消息字符串）；nm：各 0 符号；簇含调度器侧机制与 TU；K10 交叉 |
| A32 | `AsyncRwLock` + `Scheduler::rwlock_*` + `scheduler_rwlock.cpp` | LEGACY_UNJUSTIFIED | 公共类型零外部消费者（nm 0 符号）。**编译耦合**：保留的 timer pump 对 `AsyncRwLock::ExpireCtx` 硬编码分发（scheduler_timer.cpp:155-167，无门控；ExpireCtx 定义 async_rwlock.hpp:78,90）；全部 11 个调度器 TU include async_rwlock.hpp；K10 交叉 |
| A33 | `AsyncQueue` + `detail::queue_port`/`queue_item` + `queue_port.cpp`/`queue_detail.hpp` + `Scheduler::queue_*` + `scheduler_queue.cpp` | LEGACY_UNJUSTIFIED | `AsyncQueue` 类型零外部引用（nm 0 符号）。**编译耦合**：保留的 `scheduler.hpp:6` 无条件 include `detail/queue_port.hpp`；K10 交叉 |
| A34 | `Event` + `Scheduler::event_*` + `scheduler_event.cpp` | LEGACY_UNJUSTIFIED | 消费者：select 簇与本簇自有的调度器侧 `event_*`/`select_event_*` 成员（scheduler_event.cpp:21,44,88,124——不构成用户路径）；nm：4 符号（事件 TU 编译）；删除需雕刻保留调度器头/TU 面 |
| A35 | async 测试缝（3 个缝 TU + `scheduler_test_access` + `tax0_ablation_seams` + 各头内 G2 门控块 + 独立缝头 async_io_context_test_seams/threadpool_test_seams/uring_test_seams/fake_test_seams） | LIVE_CONFIG_GATED | G2 world；**当前不可激活**（§2：门内 include 的 `async_test_control_internal.hpp` 不存在、`sluice_async_test` 无 provider）。**依赖耦合**：fe2 seam 与 scheduler_test_access 消费 A29/A32/A33/A34（scheduler_fe2_test_seam.cpp:5-11,15-307）——GAP-1/GAP-3 裁决前整组冻结 |

### 7.2 同步核心（W1）

| ID | 组件簇 | 分类 | 证据要点 |
| --- | --- | --- | --- |
| S01 | `Reader`/`Writer`/`iovec.hpp` | LIVE_PUBLIC | copy_all（K16，ADR 已赚到）的载体契约类型面；buffer/copy 实现消费。证据范围修正（僵尸审计）：原证据引用的 wal/fault/observed 实现者本身是 LEGACY 簇，不得作为存活证据 |
| S02 | `SyncableWriter` | UNKNOWN | K18 争议载体（durability 语义本体由 K02 异步面独立 REQUIRED）；树内唯一消费者是同样 UNKNOWN 的 FileWriter；零应用消费 |
| S03 | `FileReader`/`FileWriter` + `src/file.cpp` | UNKNOWN | K15 争议：mission 对同步面无授权文字，四应用零消费，树内构造点仅 io_context.cpp 工厂；POSIX 实现事实完整 |
| S04 | `IoContext`/`BlockingIoContext` + `src/io_context.cpp` | UNKNOWN | 单实现虚工厂（第二实现 MemoryIoContext 已判 LEGACY），零外部调用者——僵尸审计：为假想未来用户的投机多态；属 K15 争议的一部分 |
| S05 | `copy_all` 家族 + `src/copy.cpp` + `CopyLimit` | LIVE_PUBLIC | K16 已赚到组合契约（ADR-0001 §6 正向结论）；树内零调用者但授权来自研究结论而非消费者 |
| S06 | `CopyStrategy`/`CopyOptions`/`CopyDecision` | UNKNOWN | 僵尸审计：Copy 只赚到 thin local branch；`Auto` 与 `BufferedFirst` 行为等价（copy.cpp:23-28 仅 reason 字符串不同），`Scratch` 是关闭快路径的 execution-policy 旋钮（ADR §7 把 cache strategy 列为 policy）——超出授权的公共控制面，且与本图 §3.1 的 REQUIRED 规则冲突（K20 OPTIONAL 不能支撑 LIVE_PUBLIC）；裁决归人工 |
| S07 | `BufferedReader`/`BufferedWriter`/`BufferedReadable` + `src/buffer.cpp` | LIVE_PUBLIC | K17；copy 快路径 load-bearing（src/copy.cpp:57），随 K16 存活 |
| S08 | `measurement.hpp` 统计结构 + `AsyncStats` 钩子（AsyncIoContext 构造参数/`attach_stats`/后端 `stats_` 字段） | UNKNOWN | K20 OPTIONAL 与本图 §3.1 LIVE_PUBLIC 规则直接冲突（僵尸审计）；零读者（GAP-2）；AsyncStats 钩子渗入 AsyncBackend 契约本体（async_io_context.hpp:80,146,184）——mission 原则 5 要求保护的边界对象；裁决归人工 |
| S09 | `wal.hpp` + `src/wal.cpp` | LEGACY_UNJUSTIFIED | rg：apps/async 零引用；W1 内仅自身 TU |
| S10 | `memory_io_context.hpp`（MemoryIoContext；注意 MemoryReader/MemoryWriter 实际定义于 fault.hpp:16-50） | LEGACY_UNJUSTIFIED | rg：零外部引用；nm：0 符号；与 S11 编译互链（memory_io_context.hpp:3 include fault.hpp）——删除须联合或先 S10 后 S11 |
| S11 | `fault.hpp` + `src/fault.cpp`（含 MemoryReader/MemoryWriter 定义） | LEGACY_UNJUSTIFIED | rg：零外部引用；测试机制置于公共面；nm 17 符号（编译）但零调用 |
| S12 | `observed.hpp` + `src/observed.cpp`（包装器） | LEGACY_UNJUSTIFIED | rg：零外部引用；nm 19 符号（编译）但零调用；S08 统计结构本体另行裁决 |
| S13 | `blocking_io_pool.hpp` + `src/blocking_io_pool.cpp` + `detail/blocking_io_pool_impl.hpp` | LEGACY_UNJUSTIFIED | rg：零外部引用；nm 202 符号（编译）但零调用；异步域有独立派发机制（A11） |
| S14 | `detail/io_validation.hpp` posix 部分（checked_posix_offset + 64 位断言） | LIVE_CORRECTNESS | file.cpp/threadpool_backend.cpp 消费 |
| S15 | `detail/io_validation.hpp` uring 部分 | 拆分：`checked_uring_length`/`retry_uring_wait_on_eintr` LIVE_CONFIG_GATED（uring_backend.cpp:286,302,1194,1199 消费；后者另有未构建的 experimental 消费点 uring_write_batch.cpp:89）；`uring_chunk_length` LEGACY_UNJUSTIFIED（唯一消费者 S18）；`classify_uring_submit` LEGACY_UNJUSTIFIED（**全树零消费者**，仅定义于 io_validation.hpp:49） | rg 逐符号 |
| S16 | `detail/posix_retry.hpp` | LIVE_RUNTIME | file.cpp/threadpool_backend.cpp/sluice-copy safe_output 消费 |
| S17 | `src/file_test_seams.hpp` | LIVE_CONFIG_GATED | G3 world |
| S18 | 实验性 uring 层（`include/sluice/experimental/*` + `src/experimental/*`） | LEGACY_UNJUSTIFIED | build membership：无 glob 覆盖（§2）；rg：零消费者；与 A14 双向无依赖（false-deletion 对抗者双向核实，S15a 独立存活） |

### 7.3 构建定义与应用

| ID | 组件簇 | 分类 | 证据要点 |
| --- | --- | --- | --- |
| B01 | `xmake/helpers.lua::sluice_one_file_target` | CONFIRMED_DEAD | 架构证据：W1–W3 无任何 test/example/bench target；可达性证据：全部 xmake 文件 rg 零调用 + `xmake` 目标清单无该组目标 |
| B02 | `xmake.lua` 头部注释提及的 `bench_common` | 卫生项（非代码） | libraries.lua 无此 target；文档性失实，随 PR3 清理 |
| P01–P04 | 四应用 | Application roots（LIVE） | §3.2；smoke 通过 |

## 8. 可达性证据（双方法）

禁止单工具判死。对每个 LEGACY/DEAD 候选至少两种独立方法：

| 候选 | 方法 1（rg 精确文本） | 方法 2 | 结论 |
| --- | --- | --- | --- |
| A07 RequestHandle 链 | `request_state(` → 仅声明/定义（async_io_context.hpp:204、async_io_context.cpp:154），零调用；`submit_*_request` → 出现点限于簇自身文件 | caller 搜索（apps+src 全量构造点）→ 0；nm：request_handle.cpp 编译存在（链完整但无入口）；identity 链 async_io_context.cpp:118-156 → request_handle.cpp:7-25 自闭合 | 无合法 path（公共 API，需 compat 决策） |
| A12 SyncBackend | `SyncBackend` → 仅 sync_backend.hpp | nm libsluice_async.a → 0 符号（header-only 无实例化 TU） | 无合法 path |
| A13 FakeAsyncBackend | `FakeAsyncBackend` → 自身头 + 缝头（缝头由其自身在 G2 门内 include） | nm → 0 符号；G2 无定义点且当前不可激活（§2） | 无合法 path |
| A24b CancelState/Guard/check_cancel | `check_cancel`/`CancelGuard`/`cstate_`/`cancel_state()` 使用点 → cancel.{hpp,cpp} + fiber.hpp 的内嵌成员（后者零使用） | nm → check_cancel 1 符号（编译）+ caller 搜索 0 | 无合法 path；删除需先摘 Fiber 内嵌成员（§8.1） |
| A27 op_helpers | `read_all(`/`write_all(`/`sync_data_all(`/`sync_all_all(`（限定 async 命名空间）→ 0 外部引用 | nm → 4 符号编译 + caller 0 | 无合法 path |
| A28 Batch | `Batch` → batch.{hpp,cpp} + completion.hpp friend 声明 | caller 搜索（`Batch{`/`Batch `构造/`await_one`）→ 0；nm 155 符号（编译进 W2 但无入口） | 无合法 path |
| A29 select 簇 | `select(`（sluice::async 限定）→ 仅定义 | caller 搜索 → 0；nm select_admit 1 符号（编译）；保留 scheduler.hpp 对本簇头/类型的无条件依赖见 §8.1 | 无合法用户 path；GAP-1 交叉 + §8.1 编译耦合阻断直接删除 |
| A30–A33 原语簇 | 各类型名 → 自身头/互引/自簇调度器方法（A31 其余出现仅 assert 字符串） | nm → AsyncRwLock/Semaphore/AsyncCondition/AsyncQueue 均 0 符号（header-only 从未实例化）；调度器侧方法/TU 并入簇边界（§7.1） | 无合法 path；GAP-1 交叉 + §8.1 编译耦合 |
| A34 Event | `Event` → select 簇 + 自簇调度器侧 event_* 成员签名 | nm → 4 符号（事件 TU 编译）+ caller 0 | 无用户 path；删除需雕刻保留调度器 TU 面 |
| S09 WAL | `WalWriter`/`WalReader`/`write_record` → apps/async 0 | nm → 8 符号（编译进 W1）+ caller 0 | 无合法 path |
| S10 Memory 面 | `MemoryIoContext`/`MemoryReader`/`MemoryWriter` → 0 外部 | nm → 0 符号 | 无合法 path |
| S11 Fault 面 | `FaultPlan`/`FaultReader`/`FaultWriter` → 0 外部 | nm → 17 符号（含弱符号与模板实例）+ caller 0 | 无合法 path；含 MemoryReader/Writer 定义，与 S10 联合删除 |
| S12 Observed 包装器 | `ObservedReader`/`ObservedWriter` → 0 外部 | nm → 19 符号（含弱符号与模板实例）+ caller 0 | 无合法 path |
| S13 BlockingIoPool | `BlockingIoPool`/`Task<` → 0 外部 | nm → 202 符号 + caller 0 | 无合法 path |
| S18 experimental | build membership：两 glob 均不含 src/experimental | rg → 0 消费者；include 面无实现支撑 | 不在任何 supported world |
| B01 helpers.lua | xmake 全文件 rg `sluice_one_file_target` → 仅定义 | 目标清单无 test/example/bench 组 | CONFIRMED_DEAD |

工具局限声明：`nm` 对 header-only 模板类型只能证明"未实例化"，不能证明外部源码兼容性；公共头（A07/A12/A13/A27/A28/A29/A30–A34、S09–S13）按 taskbook §28 需在删除前声明 source-compatibility policy（见 §9 决策项）。虚调用/函数指针面（AsyncBackend 契约）不构成上述候选的隐藏路径：后端注入面共三处公共入口——`RuntimeBuilder::backend`（application_runtime.hpp:73）、`AsyncIoContext` 公共构造（async_io_context.hpp:184）、`run_task_to_result`（task_result.hpp:85，copy/hash/grep 三应用消费）——外加应用任务模块的 backend 形参（copy_task.cpp:326,360、hash_task.cpp:108、grep_task.cpp:134，为测试注入预留）；**全树所有注入点实际传入的仅 `ThreadPoolBackend`**，故 A12/A13 判定不变（false-deletion 对抗者核实）。

### 8.1 Kept→LEGACY 编译耦合（删除时的悬空引用风险）

按当前簇边界直接删除 §9.2 名单会破坏以下**保留**代码的编译；PR3 执行删除时必须逐项处理（雕刻保留头/TU 或同步缩减）：

| 保留方 | 被删方 | 耦合点 |
| --- | --- | --- |
| A18 Fiber | A24b | fiber.hpp:3,55,74 内嵌 `CancelState cstate_` 与访问器（机制功能上死，编译依赖真实） |
| A16/A17 Scheduler 核心/timer | A29 | scheduler.hpp:8-9 无条件 include select_fwd/select_registration；deadline heap 类型 `DeadlineHeapEntry` 定义于 select_registration.hpp:74；~Scheduler() fail-fast 与 park 记账读写 select 状态（scheduler.cpp:84-117、scheduler_park_wake.cpp:137,342） |
| A17 timer pump | A32 | scheduler_timer.cpp:155-167 对 `AsyncRwLock::ExpireCtx` 硬编码分发（无门控）；11 个调度器 TU include async_rwlock.hpp |
| A16 Scheduler 核心 | A33 | scheduler.hpp:6 无条件 include detail/queue_port.hpp |
| A06 契约 + A11/A14 后端 | A07 | identity 钩子声明在 async_io_context.hpp:125-136，覆写于 threadpool_backend.hpp:46-51、uring_backend.hpp:76-81 |
| A35 缝（G2） | A29/A32/A33/A34 | scheduler_fe2_test_seam.cpp:5-11,15-307 与 scheduler_test_access.hpp 消费四簇 |
| S10 | S11 | memory_io_context.hpp:3 include fault.hpp（MemoryReader/Writer 定义处） |
| A03 Completion | A28 | completion.hpp:32,215 的 `friend class Batch;`（合法死文本，卫生清理） |

反向核查：A12/A13/S09/S12/S13/S15b/S18 没有任何保留方引用（只有簇自身或其余删除候选引用它们），可按簇直接删除；A14（保留）与 S18（删除）双向无依赖。

## 9. 两个方向的 Gap

### 9.1 Architecture → no path（required capability 缺完整执行/见证路径）

| ID | Gap | 内容 |
| --- | --- | --- |
| GAP-1 | deadline/timer 表达缺口 | K10 为 mission 明文语义，机制完整（A17），但：唯一公共表达载体是零消费的原语/select 面（A29–A34）；在树能 arm deadline 的代码全部位于 LEGACY 簇（scheduler_semaphore.cpp:82、scheduler_mutex.cpp:94、scheduler_condition.cpp:31、scheduler_queue.cpp:38,110、scheduler_rwlock.cpp:153,211、scheduler_event.cpp:163、select.cpp:691）——**live 树中 deadline heap 可证明恒空**，`pump_deadlines_locked()` 永不触发。删除整簇将使 K10 失去公共表达——本 campaign 不得擅自裁决，列为 PR3 决策项：要么 PR2 为 K10 建最小 witness 并保留最小载体，要么人工 review 重划 K10 为未暴露。 |
| GAP-2 | observation/statistics 零读者 | K20 OPTIONAL；统计钩子附着于 live API（S08）但无任何读取者。不阻断，仅记录。 |
| GAP-3 | 测试世界缺失 | 全树无 test target/无 specification witness。PR2 的任务本体。 |

### 9.2 Implementation → no architecture（存在但无 owner）

§7 中全部 LEGACY_UNJUSTIFIED + CONFIRMED_DEAD 条目（§8.1 列出与保留方的编译耦合；GAP-1 交叉簇 A29–A34 在裁决前保留）：

```text
A07  RequestHandle 公共查询链
A12  SyncBackend
A13  FakeAsyncBackend（+其缝头）
A24b CancelState/CancelGuard/check_cancel
A27  op_helpers
A28  Batch
A29  select 簇（GAP-1 交叉）
A30  Semaphore（GAP-1 交叉）
A31  AsyncMutex+AsyncCondition（GAP-1 交叉）
A32  AsyncRwLock（GAP-1 交叉）
A33  AsyncQueue+queue detail（GAP-1 交叉）
A34  Event（随 select）
S09  WAL
S10  Memory I/O 面
S11  Fault 注入面
S12  Observed 包装器
S13  BlockingIoPool
S15b io_validation 的 experimental-only uring 助手（uring_chunk_length/classify_uring_submit）
S18  experimental uring 层
B01  sluice_one_file_target
```

### 9.3 需人工裁决的决策项（不阻断 PR1 冻结）

1. **Source-compatibility policy**：仓库无外部消费者证据也无兼容承诺文档；master 处于 clean-room reset 后的重建期。本 campaign 按"无外部消费者、重建期内不承诺源兼容"工作，PR3 删除公共面时在 PR body 明示破坏性。
2. **G1（io_uring）world**：保留（A14 LIVE_CONFIG_GATED）或删除，取决于 mission"执行可换"对未激活机制的表达价值 vs"机制最小"。本 campaign 倾向保留（ADR-0001 明文点名 io_uring 为合法机制示例），PR3 不动。
3. **GAP-1 deadline 裁决**：见 9.1。
4. **K15 同步语义面**（僵尸审计升级为决策项）：mission 对"库是否应保有第二个（同步）公共语义面"无文字；四应用零消费；ADR 把 threaded blocking 定位为机制。S02/S03/S04/S06/S08 改判 UNKNOWN 待人工裁决：保留为库表面、降级为 internal、或删除。本 campaign 的 PR3 不动任何 UNKNOWN 簇。
5. **统计钩子（S08/AsyncStats）**：mission 允许 HINT/OBSERVATION 类别存在，但零读者且渗入 AsyncBackend 契约本体；保留、收缩或移除归人工。

## 10. Closure gate

计数口径：每个组件簇按**主状态单一归属**（JSON 同口径）；`LIVE_*` 细分互斥；A04 同时是 correctness owner，主状态记 LIVE_RESOURCE_BOUND；A03 主状态记 LIVE_CORRECTNESS（公共性记录在字段中）。

```text
TOTAL CAPABILITIES                 : 32
  REQUIRED                         : 17（K01–K14,K16,K17,K19；K10 带 witness gap GAP-1）
  DERIVED                          : 2（K21/K22）
  OPTIONAL                         : 3（K15 争议,K18 争议,K20；对抗审计降级）
  NOT_EARNED                       : 9（K23–K31；K25/K26 与 GAP-1 交叉）
  OUT_OF_SCOPE                     : 1（K32）

TOTAL COMPONENT/SYMBOL CLUSTERS    : 56（async 36 + sync 19 + build 1；
                                      另有 4 个 application roots（P01–P04，§3.2）
                                      与 1 个非代码卫生项 B02 不计入）

  LIVE_PUBLIC                      : 9（A01,A02,A15,A24,A25,A26,S01,S05,S07）
  LIVE_CORRECTNESS                 : 3（A03,A05,S14）
  LIVE_RESOURCE_BOUND              : 1（A04）
  LIVE_BACKEND                     : 4（A06,A08,A09,A11）
  LIVE_RUNTIME                     : 9（A16–A23,S16）
  LIVE_CONFIG_GATED                : 5（A10,A14,A35,S15a,S17）
  LIVE_TEST_ONLY                   : 0（测试世界缺失，GAP-3；缝归入 LIVE_CONFIG_GATED）
  LEGACY_UNJUSTIFIED               : 19（A07,A12,A13,A24b,A27,A28,A29,A30,A31,A32,A33,A34,
                                       S09,S10,S11,S12,S13,S15b,S18）
  CONFIRMED_DEAD                   : 1（B01）
  DORMANT_FUTURE_EARNED            : 0（无条目满足四证据要求；A14 按配置门控保留而非 dormant）
  UNKNOWN                          : 5（S02,S03,S04,S06,S08——僵尸审计推翻 LIVE_PUBLIC 归因，
                                      待 §9.3-4/5 人工裁决；UNKNOWN != DEAD，PR3 一律保留）

ARCHITECTURE_GAPS                  : 3（GAP-1 deadline 公共表达缺口 / GAP-2 观测零读者 / GAP-3 测试世界缺失）
```

注：`UNKNOWN_INDEX_LIMITED`（§1 词汇）在 §10 计入 UNKNOWN 口径；本轮该子类计数为 0，5 个 UNKNOWN 全部为归因证据不足型。

PR1 不删除任何东西。§9.2 名单即 PR3 候选输入；每簇删除仍需 architecture + reachability + behavioral 三重证明，GAP-1 交叉簇（A29–A34）在裁决前一律保留。
