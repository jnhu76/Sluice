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

* Application Runtime (E16)：

  * 设计已完成（`docs/design/e16-application-runtime.md`）；
  * ADR 状态：**Proposed**，实现未授权；
  * **未实现** — 主分支无 `ApplicationRuntime` / `RuntimeBuilder` / `RuntimeTaskContext`；
  * 阻塞前提：Group 事务性准入 seam（P2-01）、TLA+ lifecycle 模型（MODEL_REQUIRED, P2-03）；
  * `public_api_acceptance` 排除 Group/Scheduler/Fiber，不能替代 Runtime acceptance。

* Fuzz Foundation：

  * WAL record decode
  * WAL round-trip
  * `copy_all` fault model
  * deterministic corpus replay
  * mutation proof

## 尚未完成

* E16 Application Runtime（设计已完成，未实现 — 见 §3）
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

# 3. Milestone 0 — E16 Definition Complete; Implementation Not Started

## 当前状态

E16 Application Runtime 已完成了架构设计和 ADR 撰写，但**实现尚未开始**。

### 已存在

* **设计文档** — `docs/design/e16-application-runtime.md`（2461 行，含完整架构、API 定义、验收契约 A1-A20、变异测试矩阵）；
* **ADR** — `docs/adr/ADR-application-runtime.md`（736 行，状态：**Proposed**）；
* **基础组件** — `Scheduler`、`Group`、`Future`、`Completion<T>`、`AsyncIoContext`、`AsyncBackend`、`FakeAsyncBackend`、`ThreadPoolBackend`、全 E10-E15 同步原语。

### 不存在

* `ApplicationRuntime`、`RuntimeBuilder`、`RuntimeTaskContext` — 无 public header，无 `.cpp` 实现，无构建目标；
* Group 事务性准入 seam（P2-01） — `Group::async_evented` 的三次独立 `push_back` 无 reservation，E16 admission rollback 不正确；
* TLA+ lifecycle 模型（MODEL_REQUIRED, P2-03） — ADR 接受的前提条件；
* Runtime public acceptance consumer — 现有的 `public_api_acceptance` 明确排除 Group/Scheduler/Fiber，不能证明 Runtime API；
* 独立设计审查 — ADR 接受的前提条件。

### Fuzz 范围说明

```text
Fuzz foundation complete (sluice_core):
    ✓ WAL record decode
    ✓ WAL round-trip
    ✓ copy_all fault model

Async Runtime fuzz:
    ✗ NOT started — E16 implementation prerequisite
```

不得把 core fuzz foundation 描述成"整个异步 Runtime 已完成 fuzz"。

### 阻塞前提（必须先完成才能实现 E16）

1. **Group 事务性准入 seam（P2-01）** — `Group::async_evented` 必须提供 reservation/commit 或原子的 aggregate task-record 插入；
2. **TLA+ lifecycle 模型（P2-03）** — 含故意引入错误的 negative/broken 模型；
3. **独立设计审查** — 无未解决的 P0/P1 findings；
4. **ADR 从 Proposed 升级为 Accepted**。

## 后续工作

前提满足后，实际实现包括：

* 实现 `ApplicationRuntime` / `RuntimeBuilder` / `RuntimeTaskContext`（头文件 + 源码）；
* 新增 Runtime public acceptance consumer（见 §3.2）；
* TLA+ 模型 + 验证脚本；
* 确定性因果测试 + death tests；
* ASan / UBSan / TSan 门控；
* 更新 ADR 状态 → Accepted/Implemented；
* 同步所有文档。

### 3.2 Runtime Public Acceptance（未来实现后的验收标准）

新增只依赖 installed/public headers 的 Runtime acceptance consumer，至少覆盖：

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

---

# 4. Milestone 1 — Small Application Validation Suite

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

# 9. Milestone 6 — Observability and Performance Baseline

## 原则

在这一阶段之前，不进行大规模“性能优化”。

首先建立可重复、可解释的基线。

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

# 10. Milestone 7 — Evidence-Driven I/O Optimization

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

不得因为某个技术“理论上更快”就直接重写架构。

---

## 10.1 Backend Work Model

当前 ThreadPoolBackend 的 operation/thread 模型应首先被应用级数据审视。

可能方向：

* bounded worker pool；
* persistent workers；
* operation queue；
* completion queue；
* batch reap；
* reduced thread creation；
* configurable queue depth。

只有在真实 workload 证明 thread-per-operation 是瓶颈时实施。

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

---

# 15. Immediate Next Work

当前只开始两个 milestone：

## A. Runtime Truth Sync

把 Runtime 完成状态、ADR、closeout、README、roadmap 和 verification 同步。

## B. `sluice-copy` Version A

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

Version A 通过后，再增加 bounded pipeline。

暂不实现 stackless，暂不开始大应用，暂不进行大范围优化。

