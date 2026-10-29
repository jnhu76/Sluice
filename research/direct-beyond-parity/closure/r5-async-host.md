# R5 §8.4 — Async Host Admission（DuckDB v1.5.6 任务协议边界分析）

日期：2026-10-29。执行者：E5 closure workflow R5 Async-Host 分析子代理。
对象：DuckDB `v1.5.6`，submodule HEAD = `069cc9f9b5be802405797faecc284961b07c70ef`
（本 session `git rev-parse HEAD` 核验，porcelain 干净）。本文件只做源码边界
分析与非侵入协议探针；不改造 DuckDB Host，不宣称生产级异步接入，不涉及任何
正式计时测量（无 benchmark，无需串行 lane）。

标注：`[RUN]` = 本 session 实际执行；`[TRANSCRIBED]` = 从已提交文档转录（逐
条注明出处）；无 `[UNCERTAIN]` 项。

---

## 0. 结论（admission 裁决）

| 形态 | v1.5.6 现状 | 裁决 |
|---|---|---|
| Sync Direct（调用线程内完成 I/O） | 存在且是唯一生产 I/O 形态 | **可准入**（同步语义，R0 D2 范围） |
| Submit+Wait 同步包装（线程内等待请求完成） | 可构造（在 `AsyncTask::Execute()` 内阻塞），协议上正确 | **不是 True Async**；按每个 in-flight I/O 占用一个 DuckDB worker 计价 |
| 真完成驱动异步（任务先返回、外部完成源驱动恢复） | **不存在**：无 pending 任务状态、无外部完成驱动入口 | **REQUIRES_HOST_CHANGE**（与 dududu `docs/duckdb-stable-io-map.md` §2.4 D3/D4 一致，本分析独立复核） |

对任务书 §8.4 点名的 `ReadAheadIOTask`：**v1.5.6 全树不存在该符号** [RUN，
sweep 见 §7]。它属于 #556 v2.0-dev 谱系；v1.5.6 中实际的对应物是 parquet 的
同步 `ReadAheadBuffer`（§5）与 `AsyncTask`/`AsyncResult` 协议（§2–§3）。

---

## 1. 任务面（Task contract）

`src/include/duckdb/parallel/task.hpp`：

- `Task::Execute(TaskExecutionMode)` 返回四态 `TaskExecutionResult`
  （task.hpp:21-23,37）：`TASK_FINISHED / TASK_NOT_FINISHED / TASK_ERROR /
  TASK_BLOCKED`。
- 基类 `Deschedule()`/`Reschedule()` 直接抛 `InternalException`
  （task.hpp:42-49）——即**基类任务没有任何搁置/复活通道**；只有覆写者
  （`ExecutorTask`，executor_task.cpp:27-35）接入 Executor 的
  to-be-rescheduled 表。

## 2. TaskExecutor 生命周期

`src/parallel/task_executor.cpp` + `src/include/duckdb/parallel/task_executor.hpp`：

- TaskExecutor = 对 TaskScheduler 的 per-producer 门面 + 错误管理器 +
  完成计数（`total_tasks` 于 `ScheduleTask` 递增 :33；`completed_tasks` 于
  `FinishTask` 递增 :36-38）。
- `WorkOnTasks()`（:40-60）：**调用线程**循环执行任务直至
  `completed_tasks == total_tasks`，队列空时 `std::this_thread::yield()`
  自旋；并断言任务不得返回 `TASK_BLOCKED`（:48 `D_ASSERT`）。
  → TaskExecutor 本身是"提交后随同干到完"的同步机制：并行度来自 worker，
  对调用者的完成语义是同步的。
- `BaseExecutorTask::Execute`（:69-89）：`ExecuteTask()` **返回后**才
  `FinishTask()`；异常转入错误管理器，最终从 `WorkOnTasks` 抛出。
- 析构为空（:17-18）——无排空逻辑；排空责任在使用方（对比
  `Executor::CancelTasks` 的显式 drain，§6）。

## 3. 完成记账（Completion Accounting）——三分类的分水岭

`src/parallel/async_result.cpp`：

- `AsyncTask::Execute()` 是 **`void`、同步**（async_result.hpp:27-31）。
- `AsyncExecutionTask::ExecuteTask`（:34-40）：
  ```cpp
  async_task->Execute();
  if (counter->IterateAndCheckCounter()) { interrupt_state.Callback(); }
  ```
  批计数器 `Counter`（:14-25，atomic，size=批内任务数）在**任务函数返回时**
  递减；减到 0 即触发 `InterruptState::Callback()` 唤醒被阻塞的 PipelineTask。
- 因此**记账边 = 任务函数返回**，不是外部 I/O 完成。任务若提前返回，
  Callback 在 I/O in-flight 时即开火——探针 P1 实证（§8）。
- 无 per-completion 令牌、无失败通道（唯一失败路径：`Execute()` 抛异常 →
  `ExecutorTask::Execute` 捕获 → `executor.PushError`，executor_task.cpp:55-59）；
  计数器下溢仅有 debug 断言（async_result.cpp:18）。
- `AsyncResult::ScheduleTasks`（:86-101）经 `TaskScheduler::ScheduleTask`
  入队；`AsyncResult::ExecuteTasksSynchronously`（:103-119）在调用点同步执行
  ——两条分发路分别对应 Submit+Wait 与 Sync Direct。

## 4. Cancellation / Pipeline 恢复

`src/parallel/executor.cpp`、`src/include/duckdb/parallel/interrupt.hpp(+cpp)`、
`src/parallel/pipeline.cpp(+executor.cpp)`：

- 阻塞协议：`PipelineExecutor::SetTaskForInterrupts`（pipeline_executor.cpp:484）
  把 `InterruptState` 绑到当前 PipelineTask（`InterruptMode::TASK`）；算子返回
  `BLOCKED` → `PipelineTask::ExecuteTask` 返回 `TASK_BLOCKED`（pipeline.cpp:44-49/66）
  → `Executor::WorkOnTasks` 调 `task->Deschedule()`（executor.cpp:447-450）→
  任务进 to-be-rescheduled 表，worker 释放。
- 恢复：`InterruptState::Callback()`（interrupt.cpp，TASK 模式）→
  `Task::Reschedule()` → `Executor::RescheduleTask`（executor.cpp:485-501，
  自旋锁 + 重入队）。**BLOCKING 模式**（`InterruptDoneSignalState::Await/Signal`，
  interrupt.hpp:31-41）是调用线程上的条件变量阻塞——Submit+Wait 的原语。
- **v1.5.6 生产代码中的 Callback 驱动者全部是进程内事件**：
  - radix 聚合分区交接：radix_partitioned_hashtable.cpp:816（`BlockSource`）/903（`UnblockTasks`）；
  - 外存哈希批内存：batch_memory_manager.hpp:18/97（`StateWithBlockableTasks`）；
  - `physical_table_scan.cpp:186-195` 的 `ScheduleTasks` 臂（仅当表函数返回
    带任务的 AsyncResult）；
  - `DUCKDB_DEBUG_ASYNC_SINK_SOURCE` 测试线程（pipeline_executor.cpp:489-502/510-523，
    detached thread sleep 1ms 后 Callback——仅 debug 编译）。
  **没有任何外部 I/O 完成驱动者。**
- 取消：`Executor::CancelTasks`（executor.cpp:420-440）置 `cancelled`、清
  to-be-rescheduled（被阻塞任务**永不复活**）、随后**排空全部在飞任务**才
  销毁 pipelines/events/states（:423-433）。`RescheduleTask`/`AddToBeRescheduled`
  在 cancelled 时直接返回（:490-493/530-532）。排空保证今日"借用指针"安全
  （§5.3）的前提是任务会自行跑到完成——外部化完成源后该保证消失。
- Drain 半程：`PipelineExecutor::FinishProcessing`（pipeline_executor.cpp:278-293）
  `PreventBlocking` + `UnblockTasks`——晚到的 BLOCKED 请求被转成 `FINISHED`。
- 调度器层无 pending 状态：worker 循环遇 `TASK_BLOCKED` 即
  `task->Deschedule(); task.reset();`（task_scheduler.cpp:320-323），基类 Deschedule
  抛异常——无 Executor 介入时阻塞任务不可复活。探针 P3 实证（§8）。

## 5. Borrowed Buffer Lifetime 与 ReadAhead 机制

### 5.1 所有权链

`extension/parquet/include/parquet_reader.hpp:65-67`：`ParquetReaderScanState`
拥有 `unique_ptr<CachingFileHandle>`、`unique_ptr<ColumnReader>`、
`unique_ptr<TProtocol>`；protocol 持 `shared_ptr<ThriftFileTransport>`
（parquet_reader.cpp:44-45）。Transport 内部（thrift_tools.hpp:213-225）：

- `CachingFileHandle &file_handle` —— **借用引用**（owner 是同一 scan state，
  当前单线程使用下安全）；
- `ReadAheadBuffer ra_buffer`（值成员）→ `ReadHead::buffer_handle`
  （`BufferHandle`，buffer-manager 句柄，thrift_tools.hpp:30）。

### 5.2 ReadAhead 是同步读前取，不是异步任务

- `ReadAheadBuffer::Prefetch()`（thrift_tools.hpp:114-123）：对每个 ReadHead
  循环调 `file_handle.Read(...)`——**调用线程同步 pread**，无任务、无回调。
- `ThriftFileTransport::read`（:135-160）三路全部同步：预取缓冲 memcpy /
  fallback 1 MB 同步预取（:148-152）/ 直读。
- `ParquetReader::Scan` 内 Register→FinalizeRegistration→PrefetchRegistered
  全序列在扫描线程内（parquet_reader.cpp:1499-1508）。
- Register/Prefetch 两段式分离（row-group init 时注册、scan 时取数）正是
  外部预取任务的**注入缝**；但 ReadHead 借用 transport/scan state——任何把
  `PrefetchRegistered` 外移的方案都必须把生命周期钉在 scan state 上，并把
  完成折回 §4 的恢复协议，而该协议对 parquet 路径在 v1.5.6 中不存在。

### 5.3 生产 BLOCKED 路径与 buffer

- 生产 parquet scan **从不返回 BLOCKED**：`ParquetReader::Scan` 仅在
  `DUCKDB_DEBUG_ASYNC_SINK_SOURCE` 下生成 sleep/throw 测试任务
  （parquet_multi_file_info.cpp:728-736）。
- `MultiFileScan` 对 BLOCKED 的两条分支（multi_file_function.hpp:623-637）：
  `TASK_EXECUTOR` 模式透传；`SYNCHRONOUS` 模式 `ExecuteTasksSynchronously()`。
- BLOCKED 要求空 chunk（physical_table_scan.cpp:165-169 校验、
  pipeline_executor.cpp:533 断言）——scan 状态里挂起的 `scan_chunk`
  （`MultiFileLocalState` 所有）跨 BLOCKED 存活，恢复后重新进入 GetData。
- 探针结论与生命周期推论：**提前返回的任务会让计数器归零而
  `ReadHead::data_isset` 仍为 false**——即使把 ReadAheadBuffer 塞进任务，
  现有记账也会让 memcpy 读到未初始化的 `buffer_handle`。

## 6. 任务返回 vs 真实 I/O 完成（§8.4 最后一项）

- v1.5.6 中"任务返回 == Execute() 内做的工作已完成"恒成立（`void` 返回）。
  因此当前协议下记账正确性**依赖**任务体同步完成全部工作。
- 每个真实 I/O 完成边目前都在内核 syscall 返回处：`ThriftFileTransport::read`
  → `CachingFileHandle::Read`（caching_file_system.cpp:141-149）→
  `FileHandle::Read` → `LocalFileSystem::Read` → `pread(2)`
  （local_file_system.cpp:484-503，本 session 亲读；注意 `-1` 一律抛
  `IOException`，**无 EINTR 重试**）。
- 若把完成边外移（submit 即返），现有记账（§3）立即失真——P1 实证。
- 若把等待留在任务体内（wait_one / 阻塞 pread），正确但按 in-flight I/O
  逐个钉住 worker——P2 实证；这正是任务书禁止称为 True Async 的形态，
  也是 dududu AGENTS rule 3 明文禁止的伪异步。

## 7. `ReadAheadIOTask` 不存在性 sweep [RUN]

```
grep -rn "ReadAheadIOTask|ScanReadAhead|ReadAheadJobCompletion" <v1.5.6 tree>
（.cpp/.hpp/.h/.md，全树）→ 0 命中
```

```
grep -rn "return SourceResultType::BLOCKED" src/ → 仅
  physical_table_scan.cpp:192（函数返回带任务 AsyncResult 时，生产不可达）
  pipeline_executor.cpp:500（debug-only）
```

（另注：radix/batch-memory 路径经 `BlockSource`/`BlockSink` 辅助函数产生
BLOCKED，不在字面 grep 命中内，已按 §4 单列。）

## 8. 最小协议探针 [RUN]

载体：dududu 非侵入研究分支 **`r5-async-host-probe`**（基于
`sluice-r0-closure` @ `cc053cf`），commit `4eafbc0`
`bench(duckdb): probe async host protocol boundary`，已推送远程；未创建 PR，
未改任何 DuckDB 源文件/桥接/已有文件。代码：
`benchmarks/duckdb-r0/async-host-probe/{async_host_probe.cpp,README.md}`。

构建与运行（E5，GCC 16.2.1，链接 SHA-pinned `libduckdb.so`）：

```
g++ -std=c++17 -O1 -pthread -I third_party/duckdb/src/include \
  -I third_party/duckdb/third_party/libpg_query/include \
  -I build/duckdb-native/069cc9f9…/third_party/libpg_query/include \
  benchmarks/duckdb-r0/async-host-probe/async_host_probe.cpp \
  -L build/duckdb-native/069cc9f9…/src -lduckdb -o /tmp/async_host_probe
LD_LIBRARY_PATH=… /tmp/async_host_probe
```

结果（确定性断言，无 sleep 同步；1/1 首跑 + 5/5 稳定性复跑，全部 exit 0）：

- **P1 early-return**：计数器在任务返回时归零（resume 边开火），此刻
  `completed=no`、数据仍为毒化字节；外部完成后数据才正确。→ 现有记账下
  提前返回的任务会把垃圾 chunk 交给 pipeline，**不可准入**。
- **P2 submit+wait**：单 worker 被任务体内等待钉住 ≈3.0 ms（观察窗全长），
  排队心跳任务在窗口内**无法启动**；数据正确。→ Submit+Wait 同步包装，
  正确但逐 I/O 占用 worker，**非完成驱动**。
- **P3 TASK_BLOCKED**：执行一次即被 deschedule，队列排空后**永不复活**——
  调度器层无 pending 状态，复活仅属 Executor 机制，且今日驱动者全为进程内。→
  真完成驱动异步需要 host 变更（pending 任务态 + 外部完成驱动者）。

## 9. 与既有文档的关系

本分析独立复核并**一致于** dududu `docs/duckdb-stable-io-map.md` §2（该文档
[TRANSCRIBED] 预先指出 `ReadAheadIOTask` 属 #556 v2.0-dev 链、v1.5.6 协议是
`AsyncResult`/`AsyncTask`、D3/D4 为 `REQUIRES_HOST_CHANGE`）；§8 探针把其中
"task return 记账"与"无 pending 态"两条从源码推理升级为可复现的机器证据。
`docs/experiment-matrix.md` 的 D1–D4 admission 边界维持原状，本探针不改写
任何 arm 的裁决。

## 10. Host 变更最小形态（R2 设计素材，非本轮实现）

1. 任务合同三态化或新增"不递减计数器"的任务种类（pending 态归 host 所有）；
2. 把 `InterruptState`（TASK 模式）作为恢复令牌交给外部进度所有者，完成恰好
   一次 `Callback()`/批；
3. `poll_progress()/wait_one()` 由**非 DuckDB worker** 的固定所有者驱动
   （Sluice D4 合同要求单一 progress owner）；
4. 取消路径必须重新证明：今日 `CancelTasks` 的"排空后再销毁"借用指针保证
   依赖任务自行完成，外部完成源下需改为引用计数/shared_ptr 所有权；
   `FinishProcessing` 的 `PreventBlocking`+`UnblockTasks` 语义需覆盖外部驱动者；
5. D0'（原生 Host-Refactor 对照臂）是 D3/D4 归因前置条件（io-map §2.5 已列）。

## 11. 局限与边界声明

- 本分析覆盖 v1.5.6 的并行/任务/parquet-I/O 协议面；BufferManager 内部
  （block 回收、临时文件）仅在 BLOCKED 驱动者层面触及，未做逐行审计——与
  本轮 admission 结论无关（结论不依赖 BufferManager 细节）。
- 未运行任何 DuckDB 查询级异步实验（无 BLOCKED 生产路径可驱动）；探针是
  协议级、非查询级证据，不宣称端到端行为。
- 未 merge 任何 PR；未采纳任何公开 API；未修改 v2-r1 契约；未触碰
  #602/#601/冻结 worktree。
