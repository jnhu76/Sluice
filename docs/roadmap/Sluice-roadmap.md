# Sluice Roadmap

## 1. 项目方向

Sluice 的目标不只是实现一组 Reader、Writer、Scheduler 或异步原语，而是证明：

1. 这些组件能够组成真实、可停止、可恢复的 I/O 应用；
2. stackful 与 stackless 两种执行模型可以共享同一套 I/O 语义；
3. 性能优化来自真实应用和可重复测量，而不是微基准猜测；
4. backend 可以替换，但业务逻辑和错误语义保持稳定。

Roadmap 不再使用连续的 E-number 作为未来工作的主要名称。E10–E16 保留为历史阶段标识；后续工作使用描述性 milestone。

---

# 2. 当前状态

## 已完成

* 同步 I/O 核心：

  * `Result<T>` / `IoError`
  * Reader / Writer
  * Buffered / Observed / Fault wrappers
  * `copy_all`
  * positional file I/O
  * vector I/O
  * durability
  * WAL
  * BlockingIoPool

* Stackful 异步基础：

  * Scheduler
  * Fiber
  * multi-worker / work stealing
  * WaitNode / WaitQueue
  * Timer / deadline
  * Event / Semaphore / Mutex / Condition / Queue / RwLock
  * Select
  * Cancellation
  * Future / Group / Batch
  * AsyncIoContext
  * FakeAsyncBackend
  * ThreadPoolBackend
  * experimental io_uring backend

* Fuzz Foundation：

  * WAL record decode
  * WAL round-trip
  * `copy_all` fault model
  * deterministic corpus replay
  * mutation proof

## 已完成（新增）

* **E16 Application Runtime**

  * 架构设计：`docs/history/implementation-plans/e16-application-runtime.md`；
  * ADR 状态：**Accepted**（2026-07-29 升级，`docs/adr/ADR-application-runtime.md`）；
  * **已实现** — `ApplicationRuntime`、`RuntimeBuilder`、`RuntimeTaskContext` 位于
    `include/sluice/async/application_runtime.hpp` 与 `src/async/application_runtime.cpp`，
    随 `sluice_async` 构建，并通过全部 lifecycle/resource/identity 回归测试；
  * Runtime public acceptance consumer：`examples/runtime_acceptance.cpp`
    （仅依赖 installed/public headers，覆盖 build→start→submit→stop→drain→join→shutdown）；
  * post-merge 修正已落地：lifecycle/resource/identity corrective（PR #46）、
    multi-worker suspend-before-switch context race corrective（PR #48）；
  * TLA+ lifecycle 模型与形式证据见 `spec/tla/e16_application_runtime/` 与 `scripts/formal/`。

  > 历史上下文：本节早期文本（"尚未实现 / ADR Proposed / 实现未授权"）记录的是
  > Milestone 0 阶段的真实状态。随着 ADR Accepted、production 实现与完整验证落地，
  > 该状态已过时，故在此更新为当前事实，而非改写历史（详见 §3 末尾的状态转换记录）。

* **File-tools 应用轨道（第一轮 application-driven development，2026-08-18 合并，PR #122）**

  * 四个真实 CLI 应用：`sluice-copy`（Version C：临时文件 + 原子 rename + 目录持久化）、
    `sluice-hash`（流式 SHA-256）、`sluice-grep`（流式字面量搜索）、
    `sluice-tail`（向后 last-N + follow）；
  * 全部仅使用 `include/sluice/*` 公共头文件，无测试接缝、无 `src/` 包含；
  * 计划与实测证据（性能、内存上界、sanitizer、与系统工具对比）：
    `docs/applications/file-tools-plan.md`、`docs/applications/file-tools-findings.md`；
  * 已知遗留：`sluice-grep` 相对 grep/ugrep 的差距是 matcher 算法级别
    （SIMD/kwset 一类），V1 明确不修，作为后续候选记录。

## 尚未完成

* 真正的异步 reference application
* Stackless coroutine execution model
* Stackful/stackless 应用语义对照
* 应用级 benchmark
* 大型集成应用
* 基于真实 workload 的 I/O 优化
* io_uring production promotion
* 网络 I/O
* 完整跨平台证据

---

# 3. Milestone 0 — E16 Application Runtime（COMPLETE）

## 当前状态

> **当前事实（2026-07-30 更新）：E16 Application Runtime 已完成。**
> ADR 已 Accepted（2026-07-29）；production 实现已落地（`ApplicationRuntime`/
> `RuntimeBuilder`/`RuntimeTaskContext`）；Runtime public acceptance consumer
> 已存在（`examples/runtime_acceptance.cpp`）；post-merge lifecycle/resource/
> identity corrective（PR #46）与 multi-worker suspend-before-switch corrective
> （PR #48）均已合并；形式 lifecycle 模型与证据在 `spec/tla/e16_application_runtime/`。
>
> 下面的子节保留为历史记录，描述的是 Milestone 0 *定义阶段* 的状态与
> 阻塞前提清单（均已关闭）。这些文字不再代表当前生产真相；当前真相以本
> 状态横幅与本节末尾的"状态转换记录"为准。不要据此声称 E16 未实现。

E16 Application Runtime 最初完成了架构设计和 ADR 撰写，实现随后已落地。

### 已存在

* **设计文档** — `docs/history/implementation-plans/e16-application-runtime.md`（2461 行，含完整架构、API 定义、验收契约 A1-A20、变异测试矩阵）；
* **ADR** — `docs/adr/ADR-application-runtime.md`（736 行，状态：**Proposed**）；
* **基础组件** — `Scheduler`、`Group`、`Future`、`Completion<T>`、`AsyncIoContext`、`AsyncBackend`、`FakeAsyncBackend`、`ThreadPoolBackend`、全 E10-E15 同步原语。

### 不存在

> **当前事实：下列"不存在"项中的前两项现已存在。** 本清单保留以记录
> Milestone 0 定义阶段尚未实现的内容；当前生产真相见本节顶部状态横幅。

* ~~`ApplicationRuntime`、`RuntimeBuilder`、`RuntimeTaskContext` — 无 public header，无 `.cpp` 实现，无构建目标~~
  — **已存在**（`include/sluice/async/application_runtime.hpp`、
  `src/async/application_runtime.cpp`、`sluice_async` 目标）；
* ~~Group 事务性准入 seam（P2-01）~~ — **已完成**（见 §3.1）：`Group::async_evented` 现在在第一次 `push_back` 前完成全部三个 vector 的容量准备（`group.hpp:350-368`），一次 Evented task admission 成为完整事务；
* TLA+ lifecycle 模型（MODEL_REQUIRED, P2-03） — ADR 接受的前提条件；
* Runtime public acceptance consumer — 现有的 `public_api_acceptance` 明确排除 Group/Scheduler/Fiber，不能证明 Runtime API；
* 独立设计审查 — ADR 接受的前提条件。

### 3.1 已完成的前提 — Group 事务性准入 seam（P2-01）

`Group::async_evented` 的异常安全问题已修复（方案 B：reserve-all-before-first-push）。实现要点：

* 在同一个 `Group::mtx_` 临界区中、第一次 `push_back` 之前，对 `evented_fibers_`、`evented_stacks_`、`futures_` 三个 vector 全部执行 `reserve(size() + 1)`（`group.hpp:350-362`）；
* 三个 `push_back`（`group.hpp:366-368`）因容量已保证而不再分配；被移动类型 `unique_ptr<Fiber>`、`unique_ptr<std::byte[]>`、`shared_ptr<Future<void>>` 均为 noexcept-movable，由 `static_assert`（`group.hpp:329-334`）固化；
* reserve 失败直接传播 `std::bad_alloc`，不调用 `Scheduler::spawn`，用户任务不执行，Group 不保留部分 Fiber/stack/Future record；
* `mtx_` 在 `Scheduler::spawn`（`group.hpp:374`）之前释放，锁顺序不变；
* 确定性故障注入（仅在 `SLUICE_ASYNC_INTERNAL_TESTING` 下）覆盖每个 reserve 边界，并已在缺陷版本上确认会失败。

这仅关闭 E16 的一个 foundation 前提。下列仍未完成：

```text
TLA+ lifecycle 模型（MODEL_REQUIRED, P2-03）
独立 E16 设计/模型审查
ADR Proposed -> Accepted
E16 Application Runtime production 实现
```

不得据此声称 E16 已完成、ADR 已 Accepted 或 `ApplicationRuntime` 已实现。

### Fuzz 范围说明

```text
Fuzz foundation complete (sluice_core):
    ✓ WAL record decode
    ✓ WAL round-trip
    ✓ copy_all fault model

E16 Application Runtime fuzz/acceptance:
    ~ production implementation EXISTS; lifecycle/resource/identity coverage landed;
      app-level fuzz (sluice-copy) begins in Milestone 1
```

不得把 core fuzz foundation 描述成"整个异步 Runtime 已完成 fuzz"。历史文本"`E16 Application Runtime fuzz/acceptance: NOT started — production implementation does not exist`"描述的是 Milestone 0 定义阶段；当前 production 实现已存在，E16 层的 lifecycle/resource/identity 回归、sanitizer 与形式模型证据已落地，应用级 fuzz 随 Milestone 1 小应用展开。不代表 E10–E15 异步 runtime（Scheduler、同步原语、Future/Group/Batch、AsyncIoContext/AsyncBackend 等）缺少并发测试、sanitizer、形式模型或其他验证证据。

### 阻塞前提（必须在 E16 production implementation 之前完成）— 全部已关闭

以下四个阶段 A 步骤必须全部完成后，才授权 E16 production implementation。
**当前事实：四个前提均已关闭，E16 production implementation 已完成。**

1. **Group 事务性准入 seam（P2-01） — ✅ 已完成**
   `Group::async_evented` 采用方案 B（reserve-all-before-first-push）：在同一个 `mtx_` 临界区、第一次 `push_back` 之前完成全部三个 vector 的容量准备（`group.hpp:350-368`），一次 Evented task admission 成为完整事务。三个 `push_back` 因容量已保证而不会分配，且被移动类型为 noexcept-movable（由 `static_assert` 固化）。确定性故障注入覆盖每个 reserve 边界（`tests/group_evented_admission_exception_safety_test.cpp`），并在缺陷版本上确认会失败。E16 admission rollback 的 Group 内部所有权前提已满足；Runtime 级 `admitted_count` 回滚仍属 E16 production 工作。

2. **TLA+ lifecycle 模型（P2-03） — ✅ 已完成**
   E16 lifecycle 模型位于 `spec/tla/e16_application_runtime/`，保留故意损坏的
   negative/broken 模型及其 TLC counterexample；统一校验脚本
   `python3 scripts/formal/verify.py`。

3. **独立设计审查 — ✅ 已完成**
   独立 E16 设计/模型/Group prerequisite 审查已完成，无未解决的 P0/P1 finding。

4. **ADR 接受 — ✅ 已完成（2026-07-29 Accepted）**
   `ADR-application-runtime` 已从 `Proposed` 升级为 `Accepted`。Accepted 之后
   授权了 production implementation，后者现已落地。

## ADR Accepted 后的实现工作

ADR 进入 `Accepted` 后，E16 production implementation 才允许开始，实际实现与验证包括：

* 实现 `ApplicationRuntime`、`RuntimeBuilder` 和 `RuntimeTaskContext`（public headers + 源码）；
* 接入 public headers、production sources 和 xmake build graph；
* 新增 Runtime public acceptance consumer（见 §3.2）；
* 实现确定性 lifecycle / admission / stop / drain / join / shutdown 测试 + death tests + 错误路径验证；
* 运行 Debug、Release、ASan/UBSan、TSan 和完整 CI gate；
* 更新 API reference、architecture、verification、README 和 changelog。

### 实现完成后的状态转换

E16 production implementation、public acceptance、完整验证和实现审查全部通过后，再将 ADR 状态从 `Accepted` 更新为 `Implemented`，并新增 E16 closeout。`Accepted` 与 `Implemented` 对应不同时间点，不要把两者合写为一个含糊的状态。

### 3.2 Runtime Public Acceptance（验收标准）

> **当前事实：已落地。** `examples/runtime_acceptance.cpp` 是只依赖
> installed/public headers 的 Runtime acceptance consumer。

只依赖 installed/public headers 的 Runtime acceptance consumer，至少覆盖：

* build Runtime
* `start()`
* submit one task
* task 使用 Runtime I/O capability
* `request_stop()`
* `drain()`
* `join()` 或 `shutdown()`
* clean destruction
* rejected submit after stop
* task exception terminal accounting
* outstanding I/O 在 shutdown 前被 reap

### Milestone 0 状态转换记录（closeout）

E16 的 ADR 接受（Accepted，2026-07-29）→ production 实现 → public acceptance
（`examples/runtime_acceptance.cpp`）→ 完整验证（Debug/Release/ASan/UBSan/TSan
+ 形式模型）→ post-merge corrective（PR #46 lifecycle/resource/identity、
PR #48 multi-worker suspend-before-switch）全部完成。**Milestone 0 — COMPLETE。**

后续工作进入 **Milestone 1 — Small Application Validation Suite（ACTIVE）**，
首个 reference app 为 `apps/sluice-copy` Version A（见 §4.1）。M1-A 同时关闭
应用发现的 Runtime I/O wait API 缺口（`RuntimeTaskContext::await_completion`），
记录于 `docs/history/implementation-plans/m1-runtime-io-await-race.md` 与 App Feedback Ledger。

---

# 4. Milestone 1 — Small Application Validation Suite（ACTIVE）

## 目标

使用几个范围很小但真实运行的应用，验证 Sluice 是否真的能够被普通应用使用。

这些程序属于 `apps/`，不属于 `examples/`。

```text
examples/
    展示一个 API 怎么调用

apps/
    证明多个 API 能组成真实程序
```

所有 reference app 必须：

* 只使用 installed/public headers；
* 不使用 `SLUICE_ASYNC_INTERNAL_TESTING`；
* 不 include `src/` 或 `tests/`；
* 不直接访问 Scheduler、backend 或 Group 私有状态；
* 能在真实文件系统和真实 backend 上运行；
* 有独立 integration test；
* 能产生稳定、可比较的 workload。

---

## 4.1 App 1 — `sluice-copy`

一个真正的异步文件复制程序。

### Version A：最小异步复制

```text
open source
open destination
start runtime
submit copy task
read chunk
write chunk
repeat until EOF
flush
stop
drain
shutdown
compare source and destination
```

覆盖：

* Runtime 生命周期；
* Runtime task I/O；
* Completion 生命周期；
* positional read/write；
* EOF；
* partial read/write；
* error propagation；
* clean shutdown。

### Version B：有界流水线

```text
reader
   ↓
N reusable buffers
   ↓
bounded queue
   ↓
writer
```

覆盖：

* AsyncQueue；
* backpressure；
* 多个 outstanding I/O；
* bounded memory；
* producer/consumer cancellation；
* shutdown drain。

### Version C：安全输出

* 写入临时文件；
* copy 成功后 flush；
* 根据选项执行 `sync_data` 或 `sync_all`；
* 最后 rename；
* 失败时不得留下看起来完整的目标文件。

### 测试矩阵

* 空文件；
* 1 byte；
* 小于一个 buffer；
* 等于一个 buffer；
* 大于一个 buffer；
* 大文件；
* reader short read；
* writer short write；
* writer zero progress；
* read error；
* write error；
* cancellation；
* stop 与任务完成竞争；
* 不同 buffer size；
* 不同 in-flight depth。

---

## 4.2 App 2 — `sluice-wal`

一个小型 WAL 命令行工具。

### 命令

```text
sluice-wal append <file>
sluice-wal dump <file>
sluice-wal verify <file>
sluice-wal replay <file>
```

### 支持的 durability policy

```text
none
sync-data-every-record
sync-data-every-N-records
sync-all-on-close
```

### 覆盖

* WAL encoding / decoding；
* async write；
* batch；
* sync_data / sync_all；
* error propagation；
* truncated tail；
* corrupt record；
* restart/replay；
* cancellation during append；
* drain 时尚未完成的 durability operation。

该应用连接已有的 WAL fuzz foundation 和真实异步 Runtime。

---

## 4.3 App 3 — `sluice-mirror-mini`

一个有界并发的目录复制程序。

### 功能

* 遍历一个目录；
* 为每个普通文件提交 copy task；
* 限制同时复制的文件数量；
* 任一文件失败时记录错误；
* 可选择 fail-fast 或 continue-on-error；
* Ctrl+C 或外部 stop 后停止接收新文件；
* 已经 admitted 的文件必须完成或明确取消；
* 最终打印成功、失败、取消和字节总数。

### 覆盖

* 多任务 Runtime admission；
* stop 与 submit 的竞争；
* root cancellation；
* bounded concurrency；
* Group task terminal accounting；
* many-small-files workload；
* large-file workload；
* mixed workload；
* Runtime 长时间驻留；
* 多 worker 调度。

---

## 4.4 Small App Verification Gate

每个 app 必须通过：

### 功能

* golden-output integration tests；
* source/destination byte equality；
* deterministic error reporting；
* repeated execution；
* clean resource destruction。

### 故障

* fake backend fault injection；
* allocation/insertion failure；
* partial I/O；
* cancellation；
* premature EOF；
* backend error；
* shutdown race。

### 工具

* Debug；
* Release；
* ASan + UBSan；
* TSan；
* Valgrind，在环境允许时；
* bounded soak；
* clean-clone external consumer build。

## Exit Gate

* 三个 app 均可只通过公共 API 实现；
* app 不需要访问私有 Scheduler 或 backend；
* 不存在重复、危险的 Completion 生命周期样板代码；
* Runtime 的 stop/drain/shutdown 语义在真实程序中可用；
* 所有 API friction 被记录到 App Feedback Ledger。

---

# 5. Milestone 2 — Application-Driven API Consolidation

## 目标

不凭想象增加便利 API，而是根据三个小应用中重复出现的问题修正公共接口。

## 可能需要评估的方向

这些不是预先批准的功能，只是 app feedback 的分类：

* async file handle 的 RAII；
* op-state / Completion 生命周期包装；
* read-exact / write-all async helper；
* bounded pipeline helper；
* error context；
* shutdown guard；
* reusable buffer ownership；
* file-open 与 Runtime I/O capability 的边界；
* progress/metrics snapshot；
* application cancellation point。

任何 abstraction 至少需要被两个独立 app 重复需要，才允许进入 core。

## 不允许

* 因为一个 app 写起来不方便就增加通用框架；
* 将 CLI、日志、目录遍历放进 I/O core；
* 暴露 raw Scheduler 或 raw backend；
* 在没有生命周期证明的情况下隐藏 Completion；
* 使用 destructor 偷偷 flush 或 shutdown；
* 为 stackless 提前破坏 stackful API。

## Exit Gate

* small apps 的重复生命周期代码明显减少；
* public API reference 完整；
* external consumer test 完整；
* Runtime 与 file I/O 公共接口进入 v0.2 stability window；
* 未经 ADR 不再大幅修改生命周期语义。

---

# 6. Milestone 3 — Stackless Runtime

## 目标

增加基于 C++20 coroutine 的 stackless 执行模型，同时保留现有 stackful Fiber Runtime。

Stackless 是并列实现，不是替换。

```text
                    sluice_core
                         │
                 AsyncIoContext
                         │
                    AsyncBackend
                    /           \
        stackful Scheduler     stackless Executor
        Fiber / WaitNode       coroutine frame / awaiter
```

---

## 6.1 架构原则

### 必须共享

* `Result<T>`
* `IoError`
* async operation descriptors
* `AsyncIoContext`
* backend contract
* cancellation outcome
* deadline outcome
* partial I/O semantics
* durability semantics

### 不应共享

* Fiber stack；
* Fiber context switch；
* WaitNode object；
* Fiber owner-worker identity；
* stackful Scheduler private queues；
* test-only stackful phase seams。

Stackless 必须拥有自己的 continuation 和 resume authority。

---

## 6.2 独立库边界

初期新增独立实验库：

```text
sluice_stackless
```

而不是立即把 coroutine API 塞进 `sluice_async`。

原因：

* 生命周期尚未稳定；
* coroutine frame ownership 与 Fiber ownership 不同；
* stackless cancellation 和 structured concurrency 需要独立审查；
* 可以避免污染现有生产 runtime；
* 便于单独编译、测试和比较。

---

## 6.3 Stackless Stage A — Coroutine Core

最小类型：

```text
Task<T>
Task<void>
Executor
schedule()
run()
request_stop()
drain()
shutdown()
```

要求：

* coroutine frame 生命周期明确；
* suspended coroutine 不得被提前销毁；
* continuation exactly once；
* exception 明确映射；
* 不提供 fire-and-forget；
* 不允许 silent detach；
* Runtime destruction with suspended tasks 必须有明确 contract。

---

## 6.4 Stackless Stage B — I/O Awaitables

增加：

```text
co_await read_at(...)
co_await write_at(...)
co_await sync_data(...)
co_await sync_all(...)
```

操作状态应存放在地址稳定的 coroutine frame 或独立稳定块中。

必须证明：

* submit 后 awaiter 不移动；
* completion 只 resume 一次；
* cancel 与 completion 竞争只有一个 winner；
* coroutine frame 不会在 backend 仍持有操作时销毁；
* shutdown 会 reap outstanding I/O；
* error 与 stackful 版本一致。

---

## 6.5 Stackless Stage C — Cancellation and Deadline

支持：

* inherited cancel token；
* explicit cancellation point；
* deadline；
* cancellation protection；
* cancel/completion/deadline 三方竞争；
* parent stop propagation。

语义优先级必须与现有异步契约对齐，不得另造第二套错误模型。

---

## 6.6 Stackless Stage D — Structured Concurrency

最小组合能力：

```text
TaskGroup
when_all
race/select
bounded concurrency
child task ownership
```

禁止：

* 无 owner 的 detached task；
* child task 逃逸 scope；
* scope destructor 静默抛弃 unfinished work；
* 用 shared_ptr 无限延长 coroutine graph 来回避生命周期问题。

P2300、sender/receiver 和通用 actor framework 不属于本阶段。

---

## 6.7 Stackless Stage E — Application Runtime

初期采用独立类型：

```text
StacklessApplicationRuntime
```

暂不把 stackful 和 stackless 强行统一为一个模板 Runtime。

只有在两者的：

* lifecycle；
* admission；
* cancellation；
* drain；
* shutdown；
* error semantics；

经过应用 parity 验证后，才评估统一 facade。

---

## 6.8 Stackless Exit Gate

* Fake backend deterministic tests；
* ThreadPoolBackend real I/O tests；
* ASan + UBSan；
* TSan；
* coroutine frame destruction tests；
* cancellation race tests；
* allocation failure tests；
* stackful/stackless semantic parity；
* small app 全部可以运行在 stackless backend；
* 不依赖 x86_64 assembly；
* stackful Runtime 无回归。

---

# 7. Milestone 4 — Stackful / Stackless Application Parity

## 目标

同一个应用逻辑分别运行在：

```text
stackful Runtime
stackless Runtime
```

不能通过复制两份完整业务代码来“实现 parity”。

## 对照项目

* `sluice-copy`
* `sluice-wal`
* `sluice-mirror-mini`

## Parity Matrix

| 语义                     |       Stackful |      Stackless |
| ---------------------- | -------------: | -------------: |
| successful read/write  |           same |           same |
| EOF                    |           same |           same |
| partial I/O            |           same |           same |
| backend error          |           same |           same |
| cancellation           |           same |           same |
| deadline               |           same |           same |
| submit after stop      |           same |           same |
| drain                  |           same |           same |
| shutdown               |           same |           same |
| durability             |           same |           same |
| data output            | byte-identical | byte-identical |
| temp-file safety       |           same |           same |
| corrupted WAL handling |           same |           same |

## Exit Gate

* 同一组 integration vectors 同时驱动两套 runtime；
* 输出和错误分类一致；
* 生命周期不存在不可解释的差异；
* 默认 execution model 暂不由主观偏好决定；
* 两种模型都继续保留，等待性能和平台证据。

---

# 8. Milestone 5 — Large Reference Application

## 应用：`sluice-sync`

构建一个可恢复的本地目录同步／备份工具。

这不是为了重新实现完整 rsync，而是用一个真实程序同时验证：

* Runtime；
* copy pipeline；
* queue；
* WAL；
* cancellation；
* durability；
* crash recovery；
* bounded parallelism；
* stackful / stackless。

---

## 8.1 功能范围

### 文件复制

* 目录扫描；
* bounded parallel copy；
* large-file chunk pipeline；
* many-small-files scheduling；
* sparse/file metadata 作为后续扩展，不进入第一版。

### 安全提交

```text
destination.tmp
    ↓
write
    ↓
flush
    ↓
optional sync_data/sync_all
    ↓
rename to final path
```

### Manifest / WAL

记录：

* source path；
* destination path；
* size；
* copy state；
* completed offset；
* final verification；
* terminal error。

### Resume

程序重新启动后：

* replay manifest/WAL；
* 识别完整文件；
* 识别 incomplete temp file；
* 继续或重新开始；
* 不把未完成文件暴露成最终成功结果。

### Verification

* size verification；
* optional checksum verification；
* deterministic manifest；
* final summary。

### Cancellation

* stop receiving new files；
* 已 admitted work 按策略完成或取消；
* manifest 状态必须可 replay；
* shutdown 不得丢失已声明 durable 的记录。

---

## 8.2 Large App Failure Matrix

* 复制开始前退出；
* read 中退出；
* write 中退出；
* flush 前退出；
* flush 后 rename 前退出；
* rename 后 manifest 更新前退出；
* WAL 尾部截断；
* 单个文件权限错误；
* destination disk full；
* cancellation；
* backend error；
* one worker failure；
* Runtime stop/submit race；
* process restart。

## Exit Gate

* deterministic crash-point tests；
* resume 后结果正确；
* 不存在假的完整文件；
* WAL/manifest 可验证；
* bounded memory；
* many-small 和 large-file soak；
* stackful 和 stackless 都能驱动同一应用核心；
* app 不依赖任何测试私有接口。

---

# 9. Milestone 6 — Performance Engineering Foundation

> 2026-08 升级解读：原名 "Observability and Performance Baseline"。性能治理
> 方法论（Bidirectional Performance Funnel、APP/Boundary/Core 归因、scaling
> signature、microarchitecture drilldown、工程经济学、placement）已随
> PR #126 落地为
> `docs/verification/performance-engineering.md` 与
> `docs/verification/performance-attribution.md`；本 milestone 余下的工作是
> 把该基础设施扩展到 grep 之外的 workload，并建立回归语料。

## 原则

在这一阶段之前，不进行大规模“性能优化”。

首先建立可重复、可解释的基线。

### 已落地（PR #126，2026-08）

* 性能治理（Performance Change Gate，`AGENTS.md`）；
* Bidirectional Performance Funnel（方法论主文档）；
* workload signature 要求；
* benchmark evidence schema + 结构校验器
  （`scripts/bench/perf-evidence-validate.py`，pre-push/CI 强制）；
* environment fingerprint（git/build/system/WSL/filesystem mountinfo/tools）；
* 性能反馈台账（`docs/roadmap/performance-feedback-ledger.md`）；
* 比率化指标（Core Increment / Core Overhead Ratio / Core Share）；
* microarchitecture drilldown 分级（M0–M4，方法论文档 §6）；
* round-1 grep canonical evidence（`../results/performance-attribution/`）。

### 待做

* 回归语料（regression corpus）：跨 workload 的 before/after 复跑与
  结构化对比入库；
* copy/hash/tail 等下一轮 workload 的 ladder 化。

## 9.1 Benchmark Layers

### Layer 1：Micro

* Reader/Writer；
* copy strategy；
* WAL；
* Completion；
* submit/reap；
* queue；
* Fiber switch；
* coroutine resume；
* allocator。

### Layer 2：Component

* single-file copy pipeline；
* WAL append/replay；
* bounded multi-file copy；
* cancellation latency；
* shutdown drain。

### Layer 3：Application

* `sluice-sync` large-file；
* many-small-files；
* mixed workload；
* crash/restart；
* durability mode。

---

## 9.2 Baselines

必须同时测量：

```text
direct blocking
BlockingIoPool
ThreadPoolBackend + stackful
ThreadPoolBackend + stackless
io_uring + stackful
io_uring + stackless
```

io_uring 只在真实 liburing 环境下纳入结果。

---

## 9.3 Metrics

* throughput；
* total wall time；
* p50 / p95 / p99 latency；
* CPU user/system time；
* allocations per operation；
* peak RSS；
* bytes per task stack；
* coroutine frame size；
* voluntary/involuntary context switches；
* syscalls；
* backend submissions；
* reap batch size；
* scheduler wake count；
* queue depth；
* buffer occupancy；
* cancellation latency；
* shutdown drain latency；
* retry count；
* short I/O count；
* errors by class。

---

## 9.4 Workload Axes

* block size；
* file size；
* file count；
* concurrency；
* worker count；
* in-flight depth；
* buffer count；
* durability policy；
* file layout；
* runtime model；
* backend；
* real disk vs tmpfs；
* Debug vs Release。

所有性能结论必须附带：

* commit SHA；
* compiler；
* build mode；
* OS/kernel；
* CPU；
* filesystem；
* disk；
* workload parameters；
* repeated-run variance。

---

# 10. Milestone 7 — Evidence-Driven Optimization & Composability

> 2026-08 升级解读：原名 "Evidence-Driven I/O Optimization"。保持证据驱动
> 原则不变；新增 Core Cost Decomposition、Common Tax / Cliff Weakness、
> placement 决策与 composability 边界（方法论文档
> `docs/verification/performance-engineering.md`）。本节列出的全部条目都是
> **记录在案的未来候选**，不是已批准的实现。

## 原则

每个优化都必须具有：

```text
observed bottleneck
    ↓
explicit hypothesis
    ↓
before measurement
    ↓
small implementation
    ↓
after measurement
    ↓
semantic regression gate
```

不得因为某个技术“理论上更快”就直接重写架构。进入 default Core 的变更
另需 Common Tax 或 material Cliff Weakness 证据 + 工程经济学评估 +
placement 决策（方法论文档 §8–§10）。

---

## 10.1 Backend Work Model

> **当前事实（2026-08 更新）**：ThreadPoolBackend 已完成 bounded
> persistent-worker 迁移（Phase E：固定持久 worker、构造期有界 dispatch
> 存储、RequestArena/RequestSlot generation-safe 显式身份、有界
> accepted-terminal 存储）。早期 roadmap 中“可能的 bounded worker pool /
> persistent workers / operation queue / 减少 thread creation”等未来方向
> 已按 as-built 现实完成，不再是候选。

因此 ThreadPoolBackend 剩余的有趣成本不在 thread 创建，而在（**假设，
待分解实验证明**）：

```text
dispatch 路径
handoff（submit → worker 可见）
queueing
wake/reap
blocking worker 边界
```

在 Core Cost Decomposition（§10.8）给出数据之前，不得把上述任何一项
当作已证结论。

---

## 10.2 Submission and Reaping

评估：

* multi-op submission；
* completion batching；
* fewer poll/wait transitions；
* wake coalescing；
* reduced lock traffic；
* fewer atomic transitions。

---

## 10.3 Buffer Management

评估：

* reusable buffer pool；
* alignment；
* buffer size；
* buffer ownership；
* fixed-capacity in-flight ring；
* avoiding per-chunk allocation；
* avoiding unnecessary copy。

---

## 10.4 Vector and Copy Paths

评估：

* vector I/O；
* write coalescing；
* read-ahead；
* copy strategy selection；
* buffered-first threshold；
* short-I/O handling cost；
* possible platform fast paths。

平台 fast path 不得改变 Reader/Writer 语义。

---

## 10.5 Stackful Scheduler

评估：

* Fiber stack size；
* stack pool；
* worker count；
* work stealing；
* runnable routing；
* wake count；
* global lock contention；
* timer heap cost；
* queue hot paths。

---

## 10.6 Stackless Runtime

评估：

* coroutine frame allocation；
* frame size；
* symmetric transfer；
* continuation dispatch；
* executor queue；
* allocation elision；
* task composition overhead。

不得以牺牲清晰生命周期和 exactly-once resume 为代价追求微小收益。

---

## 10.7 io_uring Promotion

从 experimental 晋升必须满足：

* real liburing tests；
* application workload；
* cancellation；
* submit failure；
* partial submission；
* CQE error；
* bounded shutdown；
* sanitizer；
* kernel-version matrix；
* stackful/stackless parity；
* 对 BlockingIoPool 和 ThreadPoolBackend 的真实比较。

没有这些证据时，io_uring 继续保持 experimental。

---

## 10.8 Core Cost Decomposition（记录，未实现）

round-1 的 ladder 只测得**聚合** Core increment（L4 − L3）；其内部构成
（runtime 生命周期 / 准入 / submit / handoff / syscall / wait-wake / reap /
Fiber resume）在分解实验完成前一律是假设。建议分解：

```text
C0 Runtime construction/start/stop/drain/join
C1 task admission/scheduling
C2 request submit → backend acceptance
C3 backend dispatch → syscall → terminal publication
C4 terminal publication → reap
C5 wake/runnable publication
C6 Fiber resume/context switch
```

第一个实验（buffer-size / request-count scaling）：

```text
same total bytes, buffer sizes: 256 KiB / 1 MiB / 4 MiB / 16 MiB
derived: requests/GiB, Core ms/GiB, ns/request, cycles/request,
         context-switch/request, syscall/request, wake/request
hypothesis: 若 Core ms/GiB 随 request 数近似线性，则支持
            per-request 固定成本假设
```

测量前不得宣称该结论。实验通过 ladder 的 `--buffer-size` 参数即可运行。

## 10.9 Scheduler / Fiber Microbench（记录，未实现）

未来 `scheduler_microbench` 候选：

```text
fiber_create_destroy
fiber_spawn
fiber_yield
fiber_suspend_resume
same_worker_resume
cross_worker_resume
work_steal
completion_ready_inline
completion_ready_after_park
timer_park_wake
```

指标：ns/op、cycles/op、instructions/op、context-switch/op、
cache-misses/op、（有意义处）DTLB-misses/op、page-faults/op、RSS/Fiber。
未来的 Fiber stack/cache 工作必须由此 + 真实应用共同证明；
方法论文档的 funnel（real app → … → placement）先于任何实现。

## 10.10 Backend Comparison（记录，未实现）

在 normalized APP、同 buffer、同 workload、同 runtime shape 下对比：

```text
direct blocking
ThreadPoolBackend
io_uring
```

（stackless 变体存在后可加入。）ThreadPoolBackend 是 portable fallback；
其剩余成本假设见 §10.1，在 §10.8 分解前不得当作结论。

---

# 11. Milestone 8 — Backend, Platform, and Network Expansion

## 11.1 Platform

* Linux x86_64；
* Linux ARM64；
* macOS；
* Windows。

Stackless runtime 应优先提供不依赖 assembly 的可移植异步执行路径。

## 11.2 Network I/O

网络不进入 small-app 和 stackless 基础阶段。

文件 I/O 稳定后，再设计：

* non-positional stream read/write；
* socket ownership；
* connect；
* accept；
* shutdown read/write；
* half-close；
* cancellation；
* deadline；
* DNS 边界；
* backend mapping。

## 11.3 Network Validation Apps

按顺序：

```text
echo
  ↓
TCP copy / relay
  ↓
static file server
  ↓
bounded proxy
```

不得直接从文件 Runtime 跳到完整 Web framework。

---

# 12. Release Milestones

## v0.2 — Application Runtime Release

* Application Runtime closeout；
* Runtime public acceptance；
* `sluice-copy`；
* `sluice-wal`；
* `sluice-mirror-mini`；
* app-driven API consolidation。

## v0.3 — Stackless Experimental

* `sluice_stackless`；
* Task / Executor；
* I/O awaitables；
* cancellation/deadline；
* structured concurrency；
* small-app parity。

## v0.4 — Reference Application

* `sluice-sync`；
* manifest/WAL；
* resume；
* crash testing；
* stackful/stackless engines。

## v0.5 — Performance and Backend

* application benchmark matrix；
* evidence-driven optimization；
* ThreadPoolBackend evaluation；
* real io_uring validation；
* scoped performance results。

## v0.6 — Portability and Networking

* non-x86 stackless path；
* platform evidence；
* stream I/O；
* TCP applications。

## v1.0 — Stable Contract

* stable public lifecycle semantics；
* stable error semantics；
* documented support matrix；
* packaging/install consumer tests；
* production backend policy；
* long-running application evidence；
* no unresolved critical lifecycle or data-integrity findings。

---

# 13. Dependency Graph

```text
Runtime Truth Sync
        │
        ▼
Small Application Validation
        │
        ▼
Application-Driven API Consolidation
        │
        ▼
Stackless Runtime
        │
        ▼
Stackful / Stackless Parity
        │
        ▼
Large Reference Application
        │
        ▼
Observability and Performance Baseline
        │
        ▼
Evidence-Driven I/O Optimization
        │
        ▼
Backend / Platform / Network Expansion
```

Observability scaffolding可以提前加入 small app，但架构级性能优化必须等待应用基线。

---

# 14. Explicit Non-Goals for the Near Term

在完成 small apps 和 stackless parity 之前，不做：

* Web framework；
* HTTP server；
* plugin framework；
* P2300；
* actor system；
* universal executor abstraction；
* global runtime singleton；
* auto-detached task；
* dynamic backend hot swap；
* runtime restart；
* hot worker resizing；
* broad networking；
* 无数据支持的大规模性能重写。

## plugin framework 非目标的精确边界（2026-08 澄清）

**Composable mechanisms ≠ dynamic plugin framework。** 现有的组合示例是
`AsyncBackend` 与 experimental io_uring backend；未来的 policy（stack、
allocation、scheduling、NUMA 等）优先采用编译期或构造期选择
（方法论文档 §11 "Small Semantic Core + Composable Mechanisms"）。
在单独批准的设计出现之前，不引入动态二进制插件（so/dll）加载器或 ABI 插件复杂度；
正确性语义（request identity、Completion、cancellation、wait/wake、
shutdown）永远不可选件化。

---

# 15. Immediate Next Work

> **当前事实（2026-07-30）：A 已完成；B（M1-A）正在进行。**

## A. Runtime Truth Sync — ✅ 已完成

把 Runtime 完成状态、ADR、closeout、README、roadmap 和 verification 同步。
（本节 §2/§3 已反映 E16 COMPLETE。）

## B. `sluice-copy` Version A — 🔵 ACTIVE（M1-A）

实现最小真实异步文件复制：

```text
ApplicationRuntime
ThreadPoolBackend
one runtime task
positional read/write
real filesystem
byte-for-byte verification
clean stop/drain/shutdown
```

M1-A 同时关闭应用发现的 Runtime I/O wait API 缺口
（`RuntimeTaskContext::await_completion`，见
`docs/history/implementation-plans/m1-runtime-io-await-race.md`）。API 竞争候选 A/B/C 的结论与
winner 记录在该设计文档与 App Feedback Ledger 中。

Version A 通过后，再增加 bounded pipeline。

暂不实现 stackless，暂不开始大应用，暂不进行大范围优化。

