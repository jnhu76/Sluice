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
