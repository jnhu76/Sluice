# Sluice 架构概览

本文是**当前代码推导出的架构快照（snapshot）**，描述"现在的系统是什么"；它不是规范性架构权威。若本文与当前代码不一致，以当前代码为准（`include/`、`src/`、`apps/`、`xmake.lua` 与 `xmake/`）。

## 1. 项目当前定位

Sluice 是一个 C++20 I/O 库，由两个静态库目标与四个应用组成：

- `sluice_core` —— 同步 I/O 核心：`Result<T>`/`IoError` 错误模型、Reader/Writer 字节流抽象、文件与位置 I/O、copy、WAL、缓冲与包装器、阻塞线程池。
- `sluice_async` —— 可选启用的异步运行时：显式操作提交、调用方持有的完成槽、有界请求状态、Fiber 调度器、同步原语、取消、以及可替换的后端执行（线程池 / 同步 / io_uring / 仿真）。
- `apps/` —— 四个命令行应用（copy / hash / grep / tail），是公共 API 的真实消费者；主执行路径消费公共异步面与错误模型，另有一处内部依赖例外：`sluice-copy` 的 `safe_output` 直接使用 `sluice/detail/posix_retry.hpp`（见 §6）。

当前仓库是精简后的工程基线：只保留实现、应用与构建定义。测试、基准、示例、脚本、CI workflow、历史文档与形式化模型不在当前树中。

## 2. 仓库结构

| 路径 | 内容 |
| --- | --- |
| `include/sluice/` | 同步核心公共头（命名空间 `sluice`） |
| `include/sluice/async/` | 异步运行时公共头（命名空间 `sluice::async`），含 `detail/` 内部件 |
| `include/sluice/detail/`、`include/sluice/experimental/` | 核心层内部工具头；实验性 io_uring 头 |
| `src/` | `sluice_core` 实现（构建 glob 为 `src/*.cpp`，非递归） |
| `src/async/` | `sluice_async` 实现（构建 glob 为 `src/async/*.cpp`） |
| `src/experimental/` | 实验实现，当前不属于任何构建目标（见 §9） |
| `apps/sluice-copy` `apps/sluice-hash` `apps/sluice-grep` `apps/sluice-tail` | 应用，各自含 `main.cpp`、CLI 解析与任务模块及 README |
| `xmake.lua` + `xmake/libraries.lua` `xmake/apps.lua` `xmake/helpers.lua` | 构建定义：库、应用、共享辅助函数 |
| `research/RESULTS.md` | 仅保留研究结论 |
| 根目录 | `README.md` / `README.zh-CN.md` / `CONTEXT.md` / `AGENTS.md` / `CHANGELOG.md` / `LICENSE` / `.clang-format` / `.clang-tidy` / `lefthook.yml`；`.github/` 只含 issue/PR 模板与贡献指南，无 CI workflow |

构建目标一览（`xmake/`）：

| 目标 | 类型 | 默认构建 | 源 | 依赖 |
| --- | --- | --- | --- | --- |
| `sluice_core` | 静态库 | 是 | `src/*.cpp` | — |
| `sluice_async` | 静态库 | 否（opt-in） | `src/async/*.cpp` | `sluice_core` |
| `sluice-copy` / `sluice-hash` / `sluice-grep` / `sluice-tail` | 可执行 | 否（`xmake -g apps`） | 各 `apps/<name>/*.cpp` | `sluice_core` + `sluice_async` |

构建使用 C++20、`-Wall -Werror`；支持 debug/release/valgrind 模式与 asan/tsan/ubsan 策略开关及 `--hardened` 加固选项。`sluice_async` 在 Clang 前端额外启用 `-Wthread-safety`（线程安全注解，头文件中的 `SLUICE_CAPABILITY`/`SLUICE_GUARDED_BY` 宏在非 Clang 下退化为空）。`xmake/helpers.lua` 定义了单文件目标辅助函数 `sluice_one_file_target`，当前没有调用者（树中没有 test/example/bench 目标）。

## 3. 顶层组件图

```mermaid
graph TD
    subgraph APPS["apps/ 应用入口（可执行目标）"]
        COPY["sluice-copy<br/>文件复制 + 原子输出"]
        HASH["sluice-hash<br/>流式 SHA-256"]
        GREP["sluice-grep<br/>流式行搜索"]
        TAIL["sluice-tail<br/>last-N + follow"]
    end

    subgraph ASYNC["sluice_async —— 异步运行时（可选静态库）"]
        RT["ApplicationRuntime / RuntimeBuilder<br/>RuntimeTaskContext"]
        SCHED["Scheduler + Fiber<br/>park / wake / 定时器 / select"]
        PRIM["同步原语：AsyncMutex / AsyncRwLock /<br/>Semaphore / AsyncCondition / Event / AsyncQueue"]
        ORG["任务组织：Group / Batch / Future /<br/>TaskResultSlot / CancelToken"]
        CTX["AsyncIoContext / Completion&lt;T&gt; /<br/>RequestHandle（有界请求竞技场）"]
        subgraph BACKENDS["AsyncBackend 实现（可替换）"]
            TP["ThreadPoolBackend<br/>阻塞 syscall 线程池"]
            UB["UringAsyncBackend<br/>io_uring（条件编译，默认不可用）"]
        end
    end

    subgraph CORE["sluice_core —— 同步 I/O 核心（静态库）"]
        ERR["Result&lt;T&gt; / IoError"]
        RW["Reader / Writer / FileReader / FileWriter<br/>BlockingIoContext / MemoryIoContext"]
        COMB["copy_all / WAL / BufferedReader-Writer<br/>Fault* / Observed* / 统计"]
        POOL["BlockingIoPool + Task&lt;T&gt;"]
    end

    OS["Linux/POSIX syscalls<br/>（pread / pwrite / fsync …）"]

    COPY --> ASYNC
    HASH --> ASYNC
    GREP --> ASYNC
    TAIL --> ASYNC
    APPS -->|"核心错误模型 error/result；sluice-copy 另依赖 detail/posix_retry"| CORE
    ASYNC -->|"链接依赖 + IoError/Result"| CORE
    CTX --> BACKENDS
    RT --> SCHED
    RT --> ORG
    SCHED --> PRIM
    TP --> OS
    UB --> OS
    RW --> OS
```

两个库的关系：`sluice_async` 链接 `sluice_core`，二者共享 `IoError`/`Result` 错误模型；同步面不包含任何异步接口，异步能力整体是 opt-in 的独立库。

## 4. 公共接口层（include）

### 4.1 同步核心（`include/sluice/`，命名空间 `sluice`）

| 头 | 暴露内容 |
| --- | --- |
| `error.hpp` / `result.hpp` | `IoError`（11 个 `Code` + `os_errno`，`from_errno_value` 从 errno 映射）；`Result<T>`/`Result<void>` 与 `make_unexpected`。I/O 操作的可报告失败主要经由该通道传播；库中同时存在普通 C++ 异常路径（构造参数校验、分配、线程创建、接口误用抛出等），部分任务边界会把异常翻译为 `IoError`（见 §4.2 `task_result.hpp`） |
| `reader.hpp` / `writer.hpp` / `iovec.hpp` | `Reader`（`read_some`/`read_exact`/`read_vec`/`read_vec_all`/`stream_to`）、`Writer`（`write_some`/`write_all`/`write_vec`/`write_all_vec`/`flush`）、`IoSlice`/`ConstIoSlice` |
| `sync.hpp` | `SyncableWriter`（`sync_data`/`sync_all` 持久化接口） |
| `file.hpp` | `FileReader`/`FileWriter`：顺序 + 位置 I/O（`read_at`/`write_at` 及向量版本）、fd 语义、打开错误延迟报告、可挂接统计 |
| `io_context.hpp` | `IoContext` 工厂接口（`open_reader`/`open_writer`）+ `BlockingIoContext` 实现 |
| `memory_io_context.hpp` / `fault.hpp` | `MemoryIoContext`（内存存储的 IoContext）；`MemoryReader`/`MemoryWriter`、`FaultPlan`/`FaultReader`/`FaultWriter` 故障注入包装 |
| `buffer.hpp` / `buffered_readable.hpp` | `BufferedReader`/`BufferedWriter`（调用方提供缓冲区，写侧析构断言无脏数据）；`BufferedReadable`（`peek_buffered`/`consume_buffered`） |
| `copy.hpp` / `copy_strategy.hpp` / `limit.hpp` | `copy_all` 家族；`CopyStrategy`（Auto/Scratch/BufferedFirst）+ `CopyDecision` 观测选择了哪条路径；`CopyLimit` 限量 |
| `wal.hpp` | WAL 记录读写：帧 = magic + payload + checksum；`WalWriter` 跟踪 written/flushed/durable LSN |
| `observed.hpp` / `measurement.hpp` | `ObservedReader`/`ObservedWriter` 统计包装；`SyscallStats`/`BufferStats`/`CopyStats`/`SyncStats`/`VectorStats`/`UringStats`/`AsyncStats` |
| `blocking_io_pool.hpp` | `BlockingIoPool`（固定深度阻塞线程池，`submit`/`try_submit` 返回 `Task<T>`，`wait_idle`/`shutdown`） |
| `detail/io_validation.hpp` / `detail/posix_retry.hpp` | off_t/长度校验（要求 64 位 LFS）；EINTR 重试 |
| `detail/blocking_io_pool_impl.hpp` | `BlockingIoPool`/`Task<T>` 的实现模板（由 `blocking_io_pool.hpp` 引入、`blocking_io_pool.cpp` 使用；`Task::get()` 在空 Task 上抛 `std::logic_error`） |
| `experimental/uring_io_context.hpp` / `uring_write_batch.hpp` | `UringIoContext`/`UringWriteBatch`（`SLUICE_HAS_LIBURING` 条件；当前构建未定义该宏，且实现不在任何构建目标中，见 §9） |

面向对象：任何需要同步文件/字节流 I/O、显式错误处理、syscall 级观测或故障注入的用户程序。

### 4.2 异步运行时（`include/sluice/async/`，命名空间 `sluice::async`）

按职责分组：

- **操作与完成**：`async_io_context.hpp` 定义四种操作（`ReadOp`/`WriteOp` 为 fd + 缓冲 + 长度 + 位置，`SyncDataOp`/`SyncAllOp` 仅含 fd）、后端接口 `AsyncBackend`（`submit_*`/`poll`/`wait_one`/`cancel`/`register_waiter`/`wait_source`/请求身份）、后端等待源接口 `BackendWaitSource`，以及门面 `AsyncIoContext`（持有后端，提供提交/轮询/等待/取消/请求状态查询）。`completion.hpp` 定义 `Completion<T>` —— 调用方持有的完成槽，状态机为**六个独立状态** `idle`/`binding`/`outstanding`/`publishing`/`ready`/`resetting`（主路径 idle→binding→outstanding→publishing→ready→resetting→idle；另存在回滚边 binding→idle 与 outstanding→idle，以及跳过 binding 阶段的 idle→outstanding 直达认领），违规生命周期直接 fail-fast。`request_handle.hpp` 提供 `RequestHandle` 请求身份（context/slot/generation）。
- **应用运行时**：`application_runtime.hpp` 的 `RuntimeBuilder`（选后端、worker 数）→ `ApplicationRuntime`（`start`/`submit`/`request_stop`/`drain`/`join`/`shutdown` 显式生命周期，内部驱动线程与状态机）；任务体收到 `RuntimeTaskContext`，提供 `submit_*`、`await_completion`、`cancel_waiter` 与 `cancel_token()`。
- **调度**：`scheduler.hpp`（`Scheduler`：Fiber 注册/派发、`run`/`run_live` 两种驱动循环、`await_completion_*`、等待队列 `await_wait`/`wake_wait_one`/`cancel_wait`、单调时钟与截止时间、select 准入）；`fiber.hpp` + `fiber_ctx.hpp`（`Fiber` 状态机 created→runnable→running→waiting→done；上下文切换为手写汇编，含 ASAN 协作支持）；`wait_node.hpp`/`wait_queue.hpp`（等待节点/队列）；`timer_registration.hpp`。
- **同步原语**（构建在 Scheduler 的 park/wake 之上）：`async_mutex.hpp`、`async_rwlock.hpp`、`semaphore.hpp`、`condition.hpp`、`event.hpp`、`async_queue.hpp`（带 park/超时/关闭语义的并发队列）、`select.hpp` + `select_fwd.hpp`（最多 8 臂，Event/Timer 两类 case）。另有运行时内部使用的 `mutex.hpp`/`lock_guard.hpp`/`thread_annotations.hpp`（std::mutex 薄包装 + Clang TSA 注解）。
- **任务组织与结果传递**：`group.hpp`（`Group::async`，绑定 Scheduler 时走 Fiber/evented 路径，否则走线程路径；`await`/`cancel`）、`batch.hpp`（`Batch` 多操作批量提交 + `await_one`/`next`）、`future.hpp`（`Future<T>`，等待策略可插拔）、`task_result.hpp`（`TaskResultSlot<T>`：运行时 worker 向调用线程搬运结果；`translate_task_exception` 把异常翻译为 `IoError`）、`cancel.hpp`（`CancelToken`/`CancelState`/`CancelGuard`）、`wait_policy.hpp`/`evented_wait_policy.hpp`。
- **便捷层**：`op_helpers.hpp`（`read_all`/`write_all`/`sync_*_all`——直接驱动 `AsyncIoContext` 的阻塞式循环）；`await_op_helpers.hpp`（`await_take`/`await_drain`/`await_read_once`/`await_read_fill`/`await_write_exact`——任务体内使用的 await 风格 helper）。
- **后端**：`threadpool_backend.hpp`（`ThreadPoolBackend`：worker 线程执行阻塞 syscall，内部有界派发队列与就绪等待源）、`uring_backend.hpp`（`UringAsyncBackend`，`SLUICE_HAS_LIBURING` 条件编译；宏未定义时提交一律以错误拒绝（`backend_error`），不伪造执行结果）。
- **`detail/` 内部件**：`request_arena.hpp`/`request_slot.hpp`/`request_key.hpp`/`submit_transaction.hpp`（有界请求槽位竞技场 + 提交事务）、`ready_sink.hpp`（`SynchronousReadySink` 完成路由接口）、`ready_wait_source.hpp`/`reference_ready_sink.hpp`/`uring_wait_source.hpp`、`queue_item.hpp`/`queue_port.hpp`（AsyncQueue 内部端口）、`select_port.hpp`/`select_registration.hpp`、`fail_fast.hpp`（不可恢复违规的终结点）、`mutex_test_seam.hpp`/`queue_test_seam.hpp`（`SLUICE_ASYNC_INTERNAL_TESTING` 宏门控的测试缝）。

## 5. 实现层（src）

### 5.1 同步核心（`src/*.cpp` → `sluice_core`）

- `io_context.cpp` —— `BlockingIoContext` 工厂，产出 `FileReader`/`FileWriter`。
- `file.cpp` —— POSIX 实现（open/read/write/pread/pwrite/fsync，EINTR 重试，统计挂钩）。
- `reader.cpp` / `writer.cpp` —— 接口默认成员与组合逻辑（`read_exact`、`stream_to`、`write_all`、向量回退等）。
- `buffer.cpp` / `copy.cpp` / `copy_strategy.cpp` —— 缓冲读写、`copy_all` 策略循环（buffered 快路径 / scratch 路径）。
- `wal.cpp` —— 帧编码/解码与校验和。
- `fault.cpp` / `observed.cpp` —— 包装器实现。
- `blocking_io_pool.cpp` —— 阻塞线程池（队列 + 条件变量 + `Task<T>` 状态）。
- `file_test_seams.hpp` —— close 失败脚本缝（`sluice::file_testing::CloseScript`，拦截 `close_fd` 对 `::close` 的调用）；缝头本身无宏门控，仅在定义 `SLUICE_FILE_INTERNAL_TESTING` 时被 `file.cpp` 引入，同一文件的调用点也由该宏门控。

### 5.2 异步运行时（`src/async/*.cpp` → `sluice_async`）

- `application_runtime.cpp` —— 生命周期状态机（Constructed→…→Stopped/Fatal）、驱动线程 `driver_main`（循环调用 `Scheduler::run_live`，配合 drain/epoch 逻辑）、任务准入与根组。
- `async_io_context.cpp` —— 门面转发、等待语义（split-wait 能力探测、有界 park）、测试缝。
- `scheduler.cpp` + `scheduler_{condition,event,mutex,park_wake,queue,rwlock,semaphore,timer}.cpp` —— 调度器主体按关注点分文件：worker 拓扑与 Fiber 派发、park/wake、等待队列、各原语、定时器。
- `fiber.cpp` / `fiber_ctx.cpp` —— Fiber 状态与汇编上下文切换。
- `select.cpp` / `select_event.cpp` / `select_timer.cpp` —— select 组合执行。
- `group.cpp` / `batch.cpp` / `cancel.cpp` —— 任务组织。
- `op_helpers.cpp` / `await_op_helpers.cpp` / `request_handle.cpp` —— 便捷层实现。
- `queue_port.cpp`（+ `queue_detail.hpp`）—— AsyncQueue 内部端口。
- `threadpool_backend.cpp` / `uring_backend.cpp` —— 后端实现；`uring_backend.cpp` 以 `SLUICE_HAS_LIBURING` 条件编译，无该宏时编译为不可用的降级实现。
- `fail_fast.cpp` / `wait_policy.cpp`、由 `SLUICE_ASYNC_INTERNAL_TESTING` 门控的测试缝 TU（`mutex_test_seam.cpp`、`queue_test_seam.cpp`、`scheduler_fe2_test_seam.cpp`，编译进库，宏未定义时不激活）；`src/async/` 另有同宏门控的内部缝头（`*_test_seams.hpp`、`tax0_ablation_seams.hpp`、`scheduler_test_access.hpp`）。

### 5.3 experimental（`src/experimental/`）

`uring_io_context.cpp` 与 `uring_write_batch.cpp` 存在于源码树，但 `xmake` 的 glob（`src/*.cpp`、`src/async/*.cpp`）均不覆盖该子目录，当前不属于任何构建目标。

## 6. apps 层

四个应用结构一致：`cli_parse`（参数解析）→ 任务模块 → `main.cpp`。共同运行模式：

```text
RuntimeBuilder().backend(ThreadPoolBackend).workers(n).build()
→ ApplicationRuntime::start() → submit(task) → drain()/join()
→ 任务结果经 TaskResultSlot 发布，main 取回并映射 IoError 为退出码
```

| 应用 | 职责 | 使用的核心能力 |
| --- | --- | --- |
| `sluice-copy` | 文件复制：顺序与有界流水线两种引擎（位置 `submit_read`/`submit_write` + `await_read_fill`/`await_write_exact`/`await_drain`），可选 `sync_data`/`sync_all`；`safe_output` 提供原子输出（目标目录临时文件 + rename + 目录持久化），`file_domain` 做同文件防护 | 异步面全部来自 `ApplicationRuntime`/`ThreadPoolBackend`/`await_op_helpers`/`task_result`；原子输出逻辑在应用侧 |
| `sluice-hash` | 多文件流式 SHA-256（自带 sha256 引擎），有界读循环 + 取消检查 | 同上 |
| `sluice-grep` | 多文件字面量行搜索（自带 matcher） | 同上 |
| `sluice-tail` | 有界 last-N 反向扫描 + follow 长驻模式；引擎显式 `start`/`request_stop`/`wait` 生命周期；CLI 用 sigwait 信号线程把 Ctrl-C 转为取消 | 同上，另用 `cancel_token` 长驻取消 |

依赖边界事实：四个应用的主执行路径消费异步面（`application_runtime`/`async_io_context`/`await_op_helpers`/`task_result`/`threadpool_backend`）加错误模型（`error`/`result`）；同步 Reader/Writer 面是独立的库表面，应用未直接使用。当前存在一处越过公共面的内部依赖：`sluice-copy` 的 `safe_output.cpp` 直接 include `<sluice/detail/posix_retry.hpp>`（`sluice/detail/` 内部 POSIX retry helper），即 `sluice-copy` 除公共头外还直接依赖一个核心层内部头。

## 7. 典型调用路径 / 运行时关系图

以 sluice-copy 的一次异步 `read` 为例（写与 sync 同构）。完成路径分为三个不同阶段：**backend 终结化**、**Completion 发布**、**就绪路由 / waiter 唤醒**：

```mermaid
sequenceDiagram
    participant M as main 线程
    participant RT as ApplicationRuntime<br/>(driver 线程)
    participant S as Scheduler worker / Fiber
    participant C as RuntimeTaskContext<br/>(任务体)
    participant A as AsyncIoContext
    participant RA as RequestArena
    participant B as ThreadPoolBackend
    participant W as backend worker 线程
    participant CP as "Completion(size_t) 完成槽"
    participant TR as TaskResultSlot

    M->>RT: RuntimeBuilder(backend, workers).build() + start()
    RT->>S: driver_main → run_live(workers)
    M->>RT: submit(task)
    RT->>S: root Group → Fiber 入就绪队列
    S->>C: worker 执行任务体
    C->>A: submit_read(ReadOp, completion)
    A->>B: submit_read
    B->>RA: submit_transaction：reserve/prepare/<br/>install_publication_binding/commit → enqueue<br/>（槽位 pending → enqueued，Completion → outstanding）
    B->>W: dispatch 环入队 + notify_one
    C->>S: await_completion：注册 waiter → Fiber park（切出）

    rect rgb(235, 244, 255)
    Note over W,RA: 阶段 1 —— backend 终结化（backend worker 线程）
    W->>W: run_syscall：阻塞 pread
    W->>RA: record_terminal(terminal)<br/>（槽位 → backend_ready，挂入就绪环）
    W->>B: signal_ready_progress
    end

    rect rgb(235, 255, 240)
    Note over S,CP: 阶段 2 —— Completion 发布（由驱动 reap 的一侧同步执行）
    S->>RA: reap（scheduler worker 循环 drain → ctx.poll()<br/>或 AsyncIoContext::wait_one 路径）
    RA->>CP: publication_binding.publish →<br/>publish_from_reap（outstanding → publishing → ready）
    end

    rect rgb(255, 246, 235)
    Note over S,CP: 阶段 3 —— 就绪路由 / waiter 唤醒（同一 reap 调用内，publish 之后）
    RA->>S: sink.on_ready(ReadyEvent)<br/>（Scheduler ReadyRoutingSink 校验 waiter token → WaitRecord delivered）
    S->>S: 扫描 delivered 记录 → Fiber runnable → 恢复调度
    S->>C: await_completion 返回 completed
    end

    C->>CP: result() 读取 Result
    C->>TR: slot.publish(结果)
    M->>RT: drain() / join()
    M->>TR: wait_and_take() → 打印/退出码
```

三个阶段是不同的 correctness boundary，不能合并理解：

- **终结化 ≠ Completion ready**：backend worker 的 `record_terminal` 只把请求槽位标记为 `backend_ready` 并挂入就绪环；此时 `Completion` 仍是 `outstanding`，必须等某一侧驱动 `RequestArena::reap` 才会写入完成结果。
- **Completion ready ≠ Fiber 已恢复**：`publish_from_reap` 把 `Completion` 置为 `ready` 发生在执行 reap 的线程上；等待中的 Fiber 要等调度器完成就绪路由/扫描后才被重新调度。
- reap 内顺序固定：先在 arena 互斥锁内同步 publish `Completion`，锁释放后调用 `sink.on_ready` 路由就绪事件。

同步面的路径则直接得多：调用方持有 `Reader`/`Writer`（如 `FileReader`/`BufferedReader`），`copy_all` 在调用线程上循环 `read_some`/`write_some` 直至 EOF 或限额，`Result` 即时返回；`BlockingIoPool` 可把阻塞操作挪到池线程并以 `Task<T>::get()` 汇合。

## 8. 当前架构判断

**系统形态**：一个"小型 I/O 库 + 可选异步运行时 + 应用"的三层结构，而非单一框架。当前主要存留抽象（无排序）：

- `Result<T>` / `IoError` —— I/O 可报告失败的主要错误通道（两个库共享）；普通 C++ 异常路径仍存在，任务边界存在异常翻译（见 §4.1/§4.2）。
- `Reader` / `Writer` —— 同步字节流接口，文件/内存/缓冲/包装器都实现它。
- `AsyncBackend` —— 后端策略接口，四个存留实现（ThreadPool/Sync/Uring/Fake）可替换，`RuntimeBuilder` 注入。
- `Completion<T>` —— 调用方持有的完成槽：六态状态机、所有权与 fail-fast 边界都在这里。
- `ApplicationRuntime` / `RuntimeTaskContext` —— 应用进入运行时的入口与任务内 API。
- `Scheduler` / `Fiber` —— 协作式多 worker 调度（汇编上下文切换），等待即 park，完成经"终结化 → reap 发布 → 就绪路由"三阶段唤醒（见 §7）。

**同步能力**（`sluice_core`）：Reader/Writer、文件与位置 I/O、copy、WAL、缓冲、内存/故障/观测包装、阻塞线程池。
**异步能力**（`sluice_async`）：显式操作 + 调用方完成槽、有界请求状态、Fiber 调度、同步原语与 select、Group/Batch/Future/取消、四个存留后端实现。

**后端实现状态**（按当前构建与调用点事实）：四个真实应用的执行路径全部显式构造 `ThreadPoolBackend`；`UringAsyncBackend` 在默认构建中编译为不可用的降级实现（`SLUICE_HAS_LIBURING` 未定义），降级路径以错误拒绝而非合成成功。生产面不存在合成执行后端。

**当前主干与不在当前系统中的部分**：主干是上表中的库与应用。测试、基准、示例、脚本、CI workflow、历史文档、TLA+/形式化模型在当前树中不存在；`.github/` 只保留模板与贡献指南。

## 9. 已知边界

以下均为从当前代码与构建定义中直接观察到的事实：

- **无测试目标**：`xmake/helpers.lua` 的 `sluice_one_file_target` 存在但无调用者，树中没有 test/bench/example 目标。
- **测试缝按域分宏门控**（三个宏当前在 `xmake.lua`/`xmake/` 中均无定义点，默认全部不激活）：
  - 核心文件域：`src/file.cpp` 的 close 失败脚本缝由 `SLUICE_FILE_INTERNAL_TESTING` 门控——该宏同时控制 `file_test_seams.hpp` 的引入（缝头本身无宏，仅在被引入时参与编译）与 `file.cpp` 内的调用点。
  - 异步域：`src/async/` 与 `include/sluice/async/` 中的测试缝统一由 `SLUICE_ASYNC_INTERNAL_TESTING` 门控——包括编译进库的缝 TU（`mutex_test_seam.cpp`/`queue_test_seam.cpp`/`scheduler_fe2_test_seam.cpp`）与生产头/实现内的门控块；`xmake/apps.lua` 明确注释应用构建不定义该宏、不含 `src/` include 路径。
  - 应用域：`apps/sluice-copy/safe_output.cpp` 的目录 fsync 脚本缝（`DirFsyncScript`，拦截 `directory_fsync` 对 `::fsync` 的调用）由 `SLUICE_COPY_INTERNAL_TESTING` 门控——该宏同时控制 `safe_output_test_seams.hpp` 的引入（缝头本身无宏门控）与 `safe_output.cpp` 内的 seam 调用点。
- **文档重建中**：`docs/` 目前只有本文档。
- **experimental 未构建**：`src/experimental/*.cpp` 不被任何 glob 覆盖；`include/sluice/experimental/` 头文件公开，但其实现当前不编入任何库。
- **io_uring 默认不可用**：`SLUICE_HAS_LIBURING` 在 `xmake.lua`/`xmake/` 中没有定义点，`UringAsyncBackend` 在默认构建中编译为不可用的降级实现；源码保留了完整条件编译路径。
- **无 CI workflow**：`.github/` 只有 issue/PR 模板、`CONTRIBUTING.md` 与 `ISSUE_LIFECYCLE.md`。
- **平台假设**：实现使用 POSIX（`unistd.h`/`fcntl.h` 等），`detail/io_validation.hpp` 静态断言 64 位 `off_t`；Fiber 切换为手写汇编。当前代码事实面向 Linux/POSIX 环境。
