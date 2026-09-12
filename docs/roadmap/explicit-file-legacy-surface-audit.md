# Explicit File Legacy Surface Audit

Proof root: `ff916c37c6bab0f9a2bc7555a19bd8a2080af639`

Authority: ADR-0002 §13 / §14

Scope: Post-Phase-A legacy I/O/composition disposition

本文不修改 ADR 权威。它只记录从当前 master 推导的 §14 disposition。
若本文与 ADR 冲突，以 ADR 为准。

Tracking: [#355](https://github.com/jnhu76/Sluice/issues/355)

Revision: Corrective-2（第二轮人审 REQUEST_CHANGES：2 MAJOR / 1 MINOR 全部闭合，见 §15
corrective 处置记录。Corrective-1 的 3 MAJOR / 1 CORRECTNESS / 2 MINOR 闭合记录同节保留。
I21 Future 经零基础重裁改为 DELETE（§7/§10/§14 Reviewer E），盘点计数变为
TOTAL=30 / DELETE=29 / CONVERGE=1；每符号唯一归属 ledger 见 §8.1。）

---

## 1. Executive verdict

30 个盘点面（I01–I30）在 ff916c37 上逐一重derive，最终 verdict：

```text
DELETE    = 29
CONVERGE  = 1   (I20 Group)
KEEP      = 0
ADD_MINIMAL = 0
RESEARCH  = 0
OUT_OF_SCOPE（行级）= 0   （非行级 OUT_OF_SCOPE 处置见 §6 checklist 与 §13）
```

一句话结论：**Phase A 收敛完成后，legacy 同步 I/O 簇（含本审计盘点的 Copy/Buffered/
Memory/Fault/SyncableWriter/legacy stats 六个显式责任面）不再承载任何 canonical 之外的
语义责任；全树唯一活的 legacy 消费链是 `ApplicationRuntime → Group → Future`：Group 的
spawn/token 能力合法但形状错误（CONVERGE），Future 经零基础重裁无独立存活责任（DELETE，
Corrective-2）；fiber/stack storage 的物理释放权威是 ApplicationRuntime teardown barrier
（scheduler quiescence + driver join，§10 T 链），`Future::ready` 与 `terminal_count_` 均
不构成物理释放权威。其余 29 个表面全部满足 §14 DELETE 四要件。**

四个结构性事实支撑本结论（全部在 proof root 上机器验证）：

1. **Canonical include 图对 legacy 簇零传递依赖**。四个 app 与全部 22 个测试只 include
   canonical 头；唯一共享头是 `measurement.hpp`（canonical 消费者 `AsyncIoContext`/
   uring backend 只用 `AsyncStats`）。
2. **legacy 同步簇是一个零外部根的 SCC**（§28 模式）：`BlockingIoContext → FileReader/
   FileWriter → Reader/Writer → {WAL, Observed*, Buffered*, copy, fault/memory}` 相互支撑，
   全树（含测试）对其中任何一个的生产级调用为零。全树唯一的 vec 外部调用是
   `src/wal.cpp:105 → Writer::write_all_vec`，而 WAL 自身消费者为零。**Corrective-1 补充
   的簇内编译边**：`Reader::stream_to(CopyLimit)` 两个重载调用 `copy_all`（reader.cpp），
   `copy.cpp` 经 `dynamic_cast` 消费 `BufferedReadable`——这些边全部终点也在删除集内，
   不改变"零外部根"事实，但决定 slice 拓扑序（§17 DAG）。
3. **八个隔离删除探针全绿**：P1–P7（experimental/、Batch、op_helpers、BlockingIoPool、
   WAL、IoContext 三件套、整个 29 文件同步簇）全 22/22 GREEN；**P8（Corrective-1 新增）**
   在 P7 的 29 文件删除集之上把 `measurement.hpp` 修剪到仅余 `AsyncStats`，liburing=y
   22/22 GREEN——六个显式新 DELETE 面（Copy/Buffered/Memory/Fault/SyncableWriter/
   legacy stats）的文件集恰被 P7∪P8 覆盖。
4. **双配置基线（Corrective-1/MINOR-1）**：从 proof root 干净 configure+build——
   默认配置（liburing=n）**20/20 passed, 0 failed**；`--liburing=y` 配置 **22/22 passed,
   0 failed**（`uring_backend_smoke_test` 与 `uring_public_consumer_probe` 真实 liburing
   路径编译并执行通过）。

canonical promotion 问题（vectored 是否进入 canonical File surface）**不被本审计重开**：
A7 裁决维持不变，reopen 条件见 vectored-decision §5（其"reopen 语义"的机制性表述含一处
与本审计 MAJOR-3 同源的不准确括注，处置记录见 §9 vectored fact corrective）。

---

## 2. Authority / method

- 权威顺序：`docs/mission.md` → ADR-0001 → ADR-0002（§2 resource root、§3 open contract、
  §5 canonical operations + §5.3 vectored gate + §5.5 access-legality、§6 two API levels、
  §8 replaceable execution、§11 composition boundaries（含 §11.1 Copy 正例）、§13 遗留问题
  清单、§14 audit contract）。
- 审计从 ADR capability tree 出发，不从 class tree 出发（§14）。每个表面先问"它拥有什么
  责任"，再问"哪个文件定义它"。
- Verdict 词汇冻结为 §14 六值；`TECHNICAL_DEBT` 等不作为 verdict。
- 输入事实（继承并复核）：A7 vectored-decision 的 interim KEEP 无生存权（本审计重derive）；
  legacy vec 零测试覆盖；WAL 零外部消费者；#355 评论（A8 input）要求把 `experimental/`
  纳入盘点（本报告 I22–I24）。
- **Corrective-1 方法补充**：ADR-0002 §13 逐项 checklist（§6）；对每个"此前作为 SCC 残余
  隐式处置"的面补独立 §14 责任判定（I25–I30）；Copy 按 §11.1 正例地位独立裁决（§9 C1–C3）；
  OS 机制属性与 Sluice 语义权威两分（§9 vectored fact corrective）；每个未来删除符号映射到
  恰一个盘点行与恰一个实现 slice（§8 表 traceability 列 + §17 文件枚举）。
- **Corrective-2 方法补充**：每个计划移除符号的 audit owner 与 implementation slice owner
  唯一化，以机器可核对的 ledger 固化（§8.1）；Group/Future 终端-生存期权威弃用报告转述、
  直接从代码重建（§10 T1–T8 链，逐箭头 file:line）；I21 Future 零基础重裁（不预设维持
  CONVERGE、不保计数）；slice 依赖 DAG 按真实编译依赖重derive（§17）；一个 fresh
  lifetime adversary 独立复核最早合法释放点（§14 Reviewer E）。
- 证据类型：符号级 census（含反 false-zero 检查：factory indirection / virtual dispatch /
  template / alias / 继承 / `dynamic_cast` 全部人工排除）、include 图机器验证、xmake 构建
  图验证、compile_commands/目标文件交叉验证、8 个隔离删除探针、git 历史为次要证据。
- 基线状态（双配置均从干净 build dir 重derive，Corrective-1/MINOR-1）：
  - 默认（liburing=n）：`xmake -br` 全目标构建成功，`xmake test` **20/20 passed, 0 failed**；
  - `--liburing=y`：`xmake -br` 全目标构建成功（含 `uring_backend.cpp` 真实编译），
    `xmake test` **22/22 passed, 0 failed**（`uring_backend_smoke_test` +
    `uring_public_consumer_probe` = 真 liburing 路径）。
  - 两值均为实测记录，非预期值回填。

### 历史次要证据（不推翻现状，仅解释来源）

- `experimental/` 引入于 pre-ADR baseline（`b880f9fb`，CPPIO-CORE-013D stub）。
- `wal.cpp` / `file.cpp`（FileReader/FileWriter）来自最初 core commit `42ad240f`。
- `BlockingIoPool` 来自旧 campaign（`c83a0acd`，"promote production blocking pool"）。
- `Batch`/`Group`/`Future` 来自旧 async 研究（`cc4e33f2` 030-T4 / `758fc3b5` 029-T3）。
- `wal` 在 v0.0.1 tag 中随 "complete synchronous core" 发布（`git show v0.0.1:src/wal.cpp`
  存在；CHANGELOG.md "Unreleased" 记录义务适用于后续删除 slice）。

---

## 3. Capability tree（从 ADR 派生）

```text
RESOURCE
  identity / ownership / lifetime / close / access legality / observable state
      → canonical sluice::File（file_resource.hpp：open 三轴 + close authority + native_handle escape）  [OWNED]
      → FileReader/FileWriter：竞争性 resource identity（path/fd ctor + 自有 close + 隐藏 O_TRUNC）      [重复]
      → UringWriteBatch(int fd) / UringIoContext(path)：raw fd/path 权威                                  [违反 §2.1]

OPERATIONS
  sequential   → blocking::read/write（canonical）| Reader::read_some / Writer::write_some（legacy 重复）
  positional   → blocking::read_at/write_at + await_read_at/await_write_at（canonical）| FileReader/FileWriter::_at（重复）
  durability   → blocking::sync_data/sync_all + await_sync_data/sync_all（canonical）
                 | FileWriter::sync_* + SyncableWriter（重复；Corrective-1 显式行 I29）
  composition  → await_read_fill/await_write_exact（canonical async exact/all）
                 blocking 侧无 composition primitive（§5.2.2 允许 caller 自行组合）
                 | read_exact/read_at_exact/write_all/write_at_all/vec_all（legacy，见 §9 AA-3 修正）

EXECUTION
  blocking   → sluice::blocking（canonical，caller 线程直呼 syscall）
  evented    → ApplicationRuntime/AsyncIoContext + await_*（canonical）
  outstanding→ explicit ops + Completion + NativeFileRef（canonical）
  pool       → ThreadPoolBackend（canonical async offload）| BlockingIoPool + Task<T>（legacy 重复执行权）

COMPOSITION
  exact/all loops   → await_read_fill/await_write_exact（canonical）| op_helpers read_all/write_all/sync_*_all（重复，零消费者）
  buffering         → BufferedReader/BufferedWriter/BufferedReadable + BufferStats（显式行 I26；唯一消费者链 = copy fast path）
  bounded transfer  → ADR-0001 §6 / ADR-0002 §11.1 Copy 正例：责任合法；责任现由 sluice-copy app 在
                       canonical File API 上自实现；library 面 copy_all 族 = 显式行 I25（零消费者）
  scatter/gather    → vec 方法族（evidence-gated，§5.3；唯一根 = WAL；机制属性与语义权威两分见 §9）
  grouping          → Group（任务组，活）| Batch（操作组，零消费者，ADR-0001 §6 否定的 generic layer）
  WAL/record framing→ wal 命名空间 + WalWriter（零消费者；DELETE 语义澄清见 §9 WAL recheck）

TEST SUBSTRATE
  memory streams    → MemoryReader/MemoryWriter/MemoryIoContext（显式行 I27；零消费者）
  fault injection   → FaultPlan/FaultReader/FaultWriter（显式行 I28；零消费者）

OBSERVABILITY
  AsyncStats（canonical，AsyncIoContext::attach_stats）
  VectorStats/SyscallStats/SyncStats/BufferStats/CopyStats/UringStats
  （仅 legacy/experimental 消费；显式行 I11/I30）

MECHANISM
  backend transport → ThreadPoolBackend / UringAsyncBackend（canonical）
  raw fd            → NativeFileRef{int, declared access}（显式命名 interop，canonical）
                       FileReader(int fd)、UringWriteBatch(int fd)（非 canonical 形态）
```

---

## 4. Consumer / build census

方法：全树符号 grep + include 图提取 + 反 false-zero 人工排除。关键事实（全部可在 proof
root 复跑）：

```text
C1  四个 app 的 sluice include 集合 = {application_runtime, async_io_context, await_op_helpers,
    async/file, task_result, threadpool_backend, blocking/file, file_resource, error, result,
    detail/posix_retry}（copy 另有 detail/posix_retry；tail 另有 blocking/file）→ legacy 表面 app 消费 = 0
    Corrective-1 复核：apps/sluice-copy 不 include <sluice/copy.hpp>；app 内 CopyStats 是
    sluice_copy 命名空间的自有 struct（apps/sluice-copy/copy_task.hpp:28），与 library
    sluice::CopyStats（measurement.hpp:35）无包含/别名关系
C2  全部 22 个测试 include 集合同样全 canonical（+ uring_backend）→ legacy 表面测试见证 = 0
    （与 vectored-decision §5 记录的"零覆盖"输入事实一致）
C3  canonical 头（file_resource/blocking/file/async 全家）的传递 include 对 legacy 头零依赖；
    唯一共享头 measurement.hpp：AsyncStats 被 AsyncIoContext/uring 消费（canonical），
    VectorStats/SyscallStats/SyncStats/BufferStats/CopyStats 仅被 legacy 消费，UringStats 仅被 experimental 消费
C4  WalWriter/wal:: 全部函数的调用者 = 0（仅 src/wal.cpp 自身；无测试）；全树唯一 vec 调用 =
    src/wal.cpp:105 write_record_vec → Writer::write_all_vec
C5  read_vec_at / write_vec_at / read_vec_all：全树调用者 = 0
C6  ObservedReader/ObservedWriter/ReaderStats/WriterStats：全树消费者 = 0（连测试都没有）
C7  Batch/BatchOp/BatchResult：batch.{hpp,cpp} 之外消费者 = 0
C8  async::op_helpers（read_all/write_all/sync_data_all/sync_all_all）：消费者 = 0
    （canonical await_op_helpers.hpp 是另一个文件、另一组签名，7 个消费文件）
C9  BlockingIoPool/Task<T>/PoolStats：自身 3 文件之外消费者 = 0
C10 IoContext 抽象：实现者仅 BlockingIoContext + MemoryIoContext（后者 header-only，include 者 = 0）；
    open_reader/open_writer 调用点 = 0
C11 Group 消费者 = ApplicationRuntime（root_group_：ctor:114 / async:224 / group_token:278 /
    dtor-move:555 / close_resources:558；测试缝:615）；await()/cancel()/size()/group_stop_predicate
    全树调用者 = 0；线程模式（Group() 默认 ctor）实例化 = 0
C12 Future 消费者 = Group（complete_with group.hpp:103/154；ready group.cpp ×4；await 仅死线程
    路径；cancel/cancel_token 调用者 = 0）
C13 experimental/：include 者仅自身 .cpp；无任何构建目标编译 src/experimental/*.cpp
    （compile_commands.json 与 build/.objs 双重验证）；headers 在 {public=true} include 路径上被导出
C14（Corrective-1）copy_all 五个重载：全树消费者 = 0；Reader::stream_to(Writer&, span, CopyLimit,
    CopyStats*) 与 stream_to(Writer&, CopyLimit) 两个重载（reader.cpp:49-58）是仅有的内部调用点，
    其自身外部调用者 = 0；stream_to(Writer&)（无 limit 变体）自含、不依赖 copy 面，外部调用者亦 = 0
C15（Corrective-1）BufferedReader/BufferedWriter：自身 2 文件外消费者 = 0；BufferedReadable 唯一
    消费者 = copy.cpp:57（dynamic_cast fast path）；BufferStats 唯一消费者 = buffer.hpp
C16（Corrective-1）MemoryIoContext：include 者 = 0（header-only）；MemoryReader/MemoryWriter
    （定义于 fault.hpp）：消费者仅 memory_io_context.hpp 与 fault.hpp 自身
C17（Corrective-1）FaultReader/FaultWriter/FaultPlan：自身 2 文件（fault.hpp/fault.cpp）外消费者 = 0
C18（Corrective-1）SyncableWriter：实现者 = {FileWriter（file.hpp:67 多重继承）}；消费者 =
    {WalWriter（wal.hpp:27/43）}；两者均在本盘点删除集内；canonical durability 权威在 File
    （blocking::sync_* / await_sync_*，§5.4）
C19（Corrective-1）legacy stats structs 消费面：SyscallStats/SyncStats → file.hpp/io_context.hpp 管线；
    BufferStats → buffer.hpp；CopyStats → copy.hpp/reader.hpp/reader.cpp/copy.cpp；VectorStats →
    file.hpp/io_context.hpp/observed.hpp；UringStats → experimental/ 两头文件；全部为死管线；
    AsyncStats 为唯一 canonical 消费 struct
C20（Corrective-2）wait-policy 链消费面：WaitPolicy/ThreadedWaitPolicy/default_wait_policy
    （wait_policy.{hpp,cpp}）消费者 = Future 默认 ctor（唯一）；EventedWaitPolicy
    （evented_wait_policy.hpp）构造者 = Group ctor（唯一，group.cpp:22）；Future::await 是
    wait_until_ready 的唯一调用者（死线程路径）。整条链无 Group/Future 之外的消费者
```

构建图事实：

```text
sluice_core   = src/*.cpp（非递归 glob）→ 13 个 TU，其中 11 个属 legacy 簇
                （file/reader/writer/observed/wal/buffer/copy/copy_strategy/fault/io_context/blocking_io_pool）
                canonical 仅 blocking_file.cpp + file_resource.cpp
sluice_async  = src/async/*.cpp → group.cpp（活）、batch.cpp/op_helpers.cpp（零消费者死代码）
experimental  = 不被任何目标编译；headers 经 add_includedirs(public=true) 导出给每个消费者
安装/分发     = 无 install/package/发布工件；"shipped" 在本仓库语义上是"编译进静态库 + 头在导出 include 路径"
公共头负担    = include/sluice/ 共 74 个头在同一 include 根上无差别导出，无结构性 legacy/canonical 区分
```

false-zero 排除记录：Reader/Writer 的 virtual dispatch 只能经 `Reader&/Writer&` 到达（唯一入口
WAL/copy/Buffered/Observed/Memory/Fault，全死）；模板只有 `Group::async<Fn>`（活，经
ApplicationRuntime）与 `Task<T>`/`try_submit`（零调用）；factory 只有 `IoContext::open_*`
（零调用点）；**Corrective-1 补充**：`copy.cpp:57` 经 `dynamic_cast<BufferedReadable*>`
到达 Buffered 面——虚路径之外的 RTTI 通路，已纳入 census；无 type-alias/宏分发路径。

---

## 5. Dependency graph（真实代码边）

```text
[SYNC SCC — 根消费者：无]
  BlockingIoContext ──manufacture──> FileReader / FileWriter
  FileReader/FileWriter ──inherit──> Reader / Writer ──类型──> IoSlice / ConstIoSlice
  FileWriter ──inherit──> SyncableWriter                      [C18]
  FileReader/FileWriter ──fields──> SyscallStats / VectorStats / SyncStats*
  WalWriter / wal:: ──> Writer::write_all_vec / write_all / Reader::read_exact；SyncableWriter*
  ObservedReader/Writer ──wrap──> Reader/Writer + VectorStats
  BufferedReader/Writer ──wrap──> Reader/Writer + BufferedReadable + BufferStats
  copy_all / Reader::stream_to(CopyLimit) ──> Reader/Writer + CopyLimit + CopyStrategy
                                              + CopyStats + BufferedReadable(dynamic_cast)
                                              [C14/C15：Corrective-1 显式边]
  Reader::stream_to(Writer&)（自含变体）──> Reader/Writer  [不依赖 copy 面]
  MemoryIoContext ──> IoContext + MemoryReader/MemoryWriter (fault.hpp)
  FaultReader/FaultWriter ──> Reader/Writer
  BlockingIoPool（孤立：Task<T>/PoolStats）

[ASYNC]
  Batch ──> AsyncIoContext::submit_*（孤立，零消费者；持 Completion 私有 reap_seq 唯一读权 + friend 授权）
  op_helpers(read_all/...) ──> AsyncIoContext + NativeFileRef（孤立，零消费者）
  ApplicationRuntime ──> Group ──> Future / Fiber / CancelToken / Scheduler ──被──> 4 apps
    终端/生存期程序序（Corrective-2 从代码重建，§10 T1–T8 逐箭头 file:line）：
    task body 返回 → terminal_count_++（T1，仍在 fiber 栈上）→ wrapper 收尾并返回
      → Group fiber entry: Future::complete_with()（T2 发布，仍在 fiber 栈上）→ entry 返回（T3）
      → Fiber::make_done()（T4）→ context_switch_final 回 scheduler（T5，此后 fiber 栈不再执行）
      → worker loop 退出（idle 路径）→ run_impl join 全部 worker 线程（T6 quiescence）
      → driver_state_=exited → driver_thread_.join()（T7）
      → close_resources → group.reset() → ~Group：ready() 逻辑 fail-fast 扫描 →
        clear evented_stacks_/evented_fibers_/futures_（T8 storage release）
    FUTURE_READY ≠ FIBER_QUIESCENT（T2 严格先于 T5）；T8 的物理门是 T6→T7，
    ready() 扫描是 T7 之后的逻辑误用 fail-fast（§10）
  measurement.hpp：AsyncStats（canonical 边）｜其余 stats structs（仅 SYNC SCC / experimental 边）

[ISLAND — 未编译]
  UringIoContext(path) ──> UringWriteBatch(int fd) ──> liburing / raw syscall
```

分类：ROOT RESPONSIBILITY = Group/Future（经 ApplicationRuntime）；DEAD LEAF = 其余全部
（Corrective-1 起 Copy/Buffered/Memory/Fault/SyncableWriter/legacy stats 为显式 DEAD LEAF
行，不再是"残余"）；无 SUPPORTING-MECHANISM-only 存活者（没有死节点仅因指向另一死节点而
存活的反向依赖——SCC 内部互指但外部根为零，见 §28 判定）。

---

## 6. ADR-0002 §13 逐项 checklist

§13 明文"故意不决定"的问题 → 本审计处置。成功条件：每项要么 A 显式 disposition（给出行号），
要么 B 显式 OUT_OF_SCOPE（命名 owner）。不存在"隐式删除"类。

| # | §13 item | 处置 | 位置 |
| - | -------- | ---- | ---- |
| 1 | FileReader/FileWriter KEEP/CONVERGE/DELETE | A → I01/I02 DELETE | §7 |
| 2 | Reader/Writer 是否继续作为 canonical byte-stream composition layer | A → I03/I04 DELETE（canonical 载体 = blocking:: 原语 + await composition；多态流 owner 未被证明） | §7 |
| 3 | IoContext `unique_ptr<Reader/Writer>` capability erasure | A → I15 DELETE（erasure 实证成立） | §7 |
| 4 | BlockingIoContext 是否仍有 owner | A → I16 DELETE | §7 |
| 5 | BlockingIoPool 独立 execution owner vs ThreadPool 重复 | A → I17 DELETE（重复） | §7 |
| 6 | `Buffered*` 是否有 product owner | A → I26 DELETE（无 product owner，零消费者） | §7 |
| 7 | MemoryIoContext/Fault*/Observed* 是 test/observation、public capability 还是无 owner | A → I27/I28/I12/I13 DELETE（test-substrate、零使用、无 ADR 义务） | §7 |
| 8 | WAL 属于 Core、consumer/workload，还是移出 | A → I14 DELETE（非 Core；无 retained 责任；语义澄清见 §9） | §7/§9 |
| 9 | Batch/Future/Group 哪些仍与 File I/O architecture 有关 | A → I19 DELETE；I20 CONVERGE；I21 DELETE（Corrective-2 零基础重裁：无独立存活责任，随 slice O 落地；权威模型见 §10） | §7/§10 |
| 10 | current async ReadOp/WriteOp 从 raw fd 收敛到 canonical File resource | B OUT_OF_SCOPE —— 已由 Phase A 解决：ops 持 `NativeFileRef{fd, declared access}`（async_io_context.hpp:29-46）；owner = A8 conformance ledger（PR #365 后 CONFORMANT） | 本节 |
| 11 | RequestHandle / stats / synthetic backend 最终处置 | 拆三：stats → A（I11/I30 DELETE）；synthetic backend → 已不在树（前序 subtraction 删除，本审计无对象）；RequestHandle → B OUT_OF_SCOPE（canonical §7.2 explicit-op authority，活；owner = Phase A conformance） | §7/本节 |
| 12 | direct I/O / preallocation / fadvise / zero-copy / NOWAIT 产品化 | B OUT_OF_SCOPE —— 树内零代码（grep 实证）；owner = ADR-0002 §10 capability backlog（未来 evidence 流程） | 本节 |
| 13 | append / permission-mode / directory resource / rename-remove 的 Core 地位 | B OUT_OF_SCOPE —— 树内无对应 Core surface（app 侧 safe_output 的 POSIX rename/dir-fd 是 app 代码非库面）；owner = 未来 ADD_MINIMAL evidence 流程 | 本节 |

无"隐式删除"。本表替代初版"§13 只被 I01–I24 覆盖"的不完整声明（MAJOR-1 闭合）。

---

## 7. Per-surface audit cards

字段缩写：LOC=位置，PUB=public header？，BLD=是否被编译进库，DIR/TRANS=直接/传递消费者，
TEST=测试见证，APP=app 消费，RESP=拥有责任，CANON=canonical 等价物，UNIQ=独有语义，
AUTH=resource/correctness 权威，COST=机制成本，IMPACT=删除影响，F/U=follow-up（= §17
slice 字母）。

### I01 FileReader

```text
LOC=include/sluice/file.hpp + src/file.cpp   PUB=Y   BLD=Y（sluice_core）   DIR=BlockingIoContext::open_reader
TRANS=无（factory 零调用点）   TEST=0   APP=0
RESP=path/fd → fd 资源身份 + sequential/positional read + read_at_exact + vec（I07）
CANON=File::open(read_only 三轴) + blocking::read/read_at + await_read_at
UNIQ=无。short-I/O/EOF 语义与 §5.2 一致但 canonical 已同语义覆盖；read_at_exact 见 §9 AA-3 修正
AUTH=无（§2.2 明文：resource-identity 类型无默认生存权）；int fd ctor 绕过 access-legality 权威
COST=与 canonical 平行的第二套 open/close/read 语义维护面；隐藏 open 语义（O_RDONLY|O_CLOEXEC）
IMPACT=P7 GREEN；canonical contract 无损
VERDICT=DELETE
RATIONALE=§14 四要件全满足：无 owner（canonical File 是 root）；无独有 correctness 语义；无产品/测试/构建角色
（编译进库但零消费者）；删除保持 retained contracts（P7）。能力与身份分离裁决：能力已在 canonical，
身份（class 本身）即 §2.2 否定的对象——Phase A 已完成"能力收敛"，本面不存在需要 CONVERGE 搬运的残余责任。
F/U=H（FileReader/FileWriter identity slice）
```

### I02 FileWriter

```text
LOC=include/sluice/file.hpp + src/file.cpp   PUB=Y   BLD=Y   DIR=BlockingIoContext::open_writer
TRANS=无   TEST=0   APP=0
RESP=fd 资源身份 + sequential/positional write + write_at_all + sync_data/sync_all（经 SyncableWriter，I29）+ vec（I08）
CANON=File::open(三轴) + blocking::write/write_at/sync_data/sync_all + await_write_at/await_sync_*
UNIQ=无。注意：ctor 硬编码 O_WRONLY|O_CREAT|O_TRUNC|0644 = §3.1 明文禁止的"truncate 隐藏在打开 writer
默认构造语义"；canonical File::open 把三轴显式化，是修复而非等价替换
AUTH=无（同 I01）；truncate 语义被隐藏本身就是违反 §3.1 的权威问题
COST=第二套 durability/durability-stats 维护面；flush() 为空实现
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I01（+§3.1 违反加重）   F/U=H
```

### I03 Reader（抽象接口）

```text
LOC=include/sluice/reader.hpp + src/reader.cpp   PUB=Y   BLD=Y   DIR=WAL/Observed/Buffered/copy/FileReader（全在 SCC 内）
TRANS=无外部   TEST=0   APP=0
RESP=polymorphic byte-stream read 抽象 + read_exact/stream_to/copy 组合 + read_vec/read_vec_all 默认实现
CANON=blocking::read 原语（caller 组合）+ await_read_fill/await_write_exact（async exact/all）
UNIQ=§9 AA-3 修正：read_exact 的 EOF-as-error 组合语义在删除后将无 canonical 载体
（await_read_fill 是 fill-with-partial）；§5.2.1/§5.2.3 只定义"若存在 composition 的语义"，不强制存在；
§5.2.2/§6.1 明文允许 caller 自行组合 → 该能力合法缺席，不构成 DELETE 阻力，但必须记录
AUTH=无（非 File 资源权威；无 correctness invariant）
COST=虚接口 + 全部实现者（FileReader/Observed/Buffered/Memory/Fault）都是死 SCC 成员
IMPACT=P7 GREEN；实现者集合随本 DELETE 一并消失
VERDICT=DELETE
RATIONALE=§13 问题"是否是伪装的 File 抽象/通用组合层"的裁决：generic byte-stream owner 未被证明——
非文件实现者全部是零消费者测试替身，组合责任已有 canonical owner，虚接口形式无根。四要件满足。
F/U=I（generic byte-stream interface slice；若未来真实 consumer 需要多态流，走新 ADD_MINIMAL 节点）
```

### I04 Writer（抽象接口）

```text
LOC=include/sluice/writer.hpp + src/writer.cpp   PUB=Y   BLD=Y   DIR=同 I03 族 + WalWriter
TRANS=无外部   TEST=0   APP=0
RESP=polymorphic write 抽象 + write_all + write_vec/write_all_vec 默认实现；flush() 纯虚
CANON=blocking::write + await_write_exact（write 侧 exact/all 已有 canonical 载体）
UNIQ=无（write_all 的 zero-progress→invalid_state 语义 = §5.2.2，await_write_exact 同语义已实现）
AUTH=无   COST=同 I03
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I03   F/U=I
```

### I05 Reader vectored 方法（read_vec / read_vec_all 默认实现）

```text
LOC=include/sluice/reader.hpp:31-33 + src/reader.cpp:58-103   PUB=Y   BLD=Y   DIR=FileReader(override)/ObservedReader(override)
TRANS=无（read_vec_all 调用者=0）   TEST=0   APP=0
RESP=逐-slice fallback 组合（短读即停、跨 buffer 累计推进）
CANON=无 canonical vec（A7 维持：canonical promotion 未被 earn；本审计不重开）
UNIQ=跨 buffer short-I/O 推进语义（vectored-decision §3）；性能型单 syscall 计数不是 semantic authority
（ADR-0001 §3/§8）
AUTH=无 owner：§5.3 把 vectored 冻结为 evidence-gated extension
COST=接口面积 + fallback 实现维护 + VectorStats 管线
IMPACT=P7 GREEN（随簇删除）；gate chain 满足：WAL（唯一链根）已先裁决为 DELETE（本表 I14）
VERDICT=DELETE
RATIONALE=§14 四要件：§5.3 下 vectored 无 canonical authority（无 owner）；跨 buffer 推进语义无存活消费者
依赖；零测试/产品角色；删除保持全部 retained scalar contracts。裁决顺序遵守 vectored-decision §5
"先裁 WAL 再裁消费链"；依据是 §14 四要件而非裸零消费者。机制属性澄清见 §9 vectored fact corrective
（OS 单操作属性存在 ≠ Sluice 语义权威存在）。
F/U=F（vectored operation family slice，在 E 之后）
```

### I06 Writer vectored 方法（write_vec / write_all_vec 默认实现）

```text
LOC=include/sluice/writer.hpp:21-23 + src/writer.cpp:26-98   PUB=Y   BLD=Y   DIR=FileWriter/ObservedWriter(override) + wal.cpp:105（唯一外部调用）
TRANS=WAL→(死)   TEST=0   APP=0
RESP=逐-slice fallback + head-offset 重提交 all 组合
CANON=无（同 I05）
UNIQ=WAL scatter-write（header|payload|trailer 单 syscall）消费的唯一载体；WAL 裁决为 DELETE 后该
composition 需求本身消失
AUTH=无（同 I05）   COST=同 I05 + 60 行 head-offset 状态机
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I05（gate chain 依赖已满足）   F/U=F
```

### I07 FileReader vectored 方法（read_vec → ::readv / read_vec_at → ::preadv）

```text
LOC=include/sluice/file.hpp:50-54 + src/file.cpp:114-285   PUB=Y   BLD=Y   DIR=虚经 Reader*（无外部根）
TEST=0   APP=0   RESP=真 syscall scatter/gather read（IOV_MAX 分块、跨 buffer 推进）
CANON=无   UNIQ=多 buffer 单 syscall（syscall-count 机制，非语义权威——ADR-0001 §3）
AUTH=无   COST=readv/preadv/iov_max 分块机器 ~170 行；零测试覆盖
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I05 + read_vec_at 全树零调用   F/U=F
```

### I08 FileWriter vectored 方法（write_vec → ::writev / write_vec_at → ::pwritev）

```text
LOC=include/sluice/file.hpp:103-107 + src/file.cpp:357-533   PUB=Y   BLD=Y   DIR=虚经 Writer*（唯一根 WAL，死）
TEST=0   APP=0   RESP=真 syscall scatter/gather write（IOV_MAX 分块、跨 iovec head-offset 推进）
CANON=无
UNIQ=（Corrective-1/MAJOR-3 改写，原句作废）OS/fd 对象层可提供有意义的单操作/非交错机制属性
（如 POSIX 对 regular-file write 家族的互原子性要求、pipe 上总量 ≤PIPE_BUF 的原子 writev、
O_APPEND 整组追加）——但（i）这些属性属于 OS 对象语义且有条件（对象类型/总量限），（ii）Sluice
公共面从未把任何此类属性冻结为 retained Writer/FileWriter contract（无文档/测试/ADR 声明
writev 单操作或非交错语义），（iii）无 retained consumer 证明依赖（唯一 vec 消费者 WAL 自身
verdict DELETE 且无下游），（iv）§5.3 evidence gate 未满足。机制属性存在与语义权威缺席不矛盾。
AUTH=无   COST=同 I07
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I05（DELETE 不依赖虚假 syscall 断言；完整两分论证见 §9）   F/U=F
```

### I09 IoSlice

```text
LOC=include/sluice/iovec.hpp   PUB=Y   BLD=Y（header）   DIR=仅 vec 方法签名 + wal.cpp
TEST=0   APP=0   RESP=可写 span 包装（IoSlice{span<byte>}）
CANON=无（canonical 全 scalar，直接用 span）   UNIQ=无（纯别名级包装）
AUTH=无   COST=一个公共类型名 + 语义歧义（与 std::span 并存）
IMPACT=随 vec 删除   VERDICT=DELETE
RATIONALE=唯一消费者是 I05–I08；无独立责任   F/U=F
```

### I10 ConstIoSlice

```text
LOC=include/sluice/iovec.hpp   PUB=Y   BLD=Y   DIR=同 I09
TEST=0   APP=0   RESP=只读 span 包装   CANON=无   UNIQ=无   AUTH=无
IMPACT=随 vec 删除   VERDICT=DELETE   RATIONALE=同 I09   F/U=F
```

### I11 VectorStats

```text
LOC=include/sluice/measurement.hpp:72-81   PUB=Y   BLD=Y   DIR=file.hpp 成员 + OpenReader/WriterOptions + Observed*（全死）
TEST=0   APP=0   RESP=vec 调用/字节/iovec/fallback 计数
CANON=AsyncStats（canonical 观测权威：AsyncIoContext::attach_stats）
UNIQ=无（纯 legacy vec 计数）
AUTH=无（hint/observation，非 authority）   COST=8 个字段 + 全部包装管线的存在理由
IMPACT=P7 GREEN（P7 未动 measurement.hpp，文件级修剪由 P8 覆盖）
VERDICT=DELETE
RATIONALE=观测责任已由 canonical AsyncStats 拥有；本 struct 只服务死 vec 管线；"statistics sound useful"
不构成 KEEP 证据。删除指 struct 字段，不指 measurement.hpp 文件。
F/U=G（Corrective-2 归属修正：VectorStats struct 的删除 owner 是本行、落地于 slice G；原
"并入 slice P"与 slice G 文件枚举构成双 owner，废除。编译依赖：file.hpp/io_context.hpp 的
VectorStats 引用者先删（H、J 先于 G，§17）。P8 探针作为 struct 级终态证据复用，§9。）
```

### I12 ObservedReader

```text
LOC=include/sluice/observed.hpp + src/observed.cpp（含 ReaderStats/WriterStats structs）   PUB=Y   BLD=Y
DIR=0（全树含测试零消费者）
RESP=Reader 包装 + ReaderStats/VectorStats 计数   CANON=AsyncStats 管线（观测已 canonical 化）
UNIQ=无   AUTH=无   COST=公共包装类型 + 观测面（且从未被用过）
IMPACT=P7 GREEN   VERDICT=DELETE
RATIONALE=零消费者 + 责任已由 canonical 观测面承担；包装层级本身是"measurement 是否值得
一整个 wrapper hierarchy"的否定答案。   F/U=G
```

### I13 ObservedWriter

```text
LOC=include/sluice/observed.hpp + src/observed.cpp   PUB=Y   BLD=Y   DIR=0   TEST=0   APP=0
RESP=Writer 包装 + WriterStats/VectorStats 计数   CANON=同 I12   UNIQ=无   AUTH=无
IMPACT=P7 GREEN   VERDICT=DELETE   RATIONALE=同 I12   F/U=G
```

### I14 WAL（wal:: 自由函数 + WalWriter + record surface）

```text
LOC=include/sluice/wal.hpp + src/wal.cpp + include/sluice/sync.hpp（SyncableWriter 持久钩子，I29）
PUB=Y   BLD=Y（sluice_core，死对象码在 .a 内）   DIR=0（全树含测试零消费者）   TEST=0   APP=0
RESP=record framing（magic|len|payload|additive-checksum）+ LSN 三层（written/flushed/durable）+
SyncableWriter durability 门
CANON=无（canonical surface 不提供 record framing——这是真实的独有能力，但无任何依赖它的 correctness
contract 存在）
UNIQ=record framing/LSN 语义（真实、自洽、零依赖者）
AUTH=无 architecture owner（ADR-0002 §13 把"WAL 属于 Core、consumer/workload，还是移出"列为 open
问题——open 不是 grant；mission/ADR/README/RESULTS 均无 WAL 产品意图）；安全输出（sluice-copy safe_output）
解决 materialization 原子性，不依赖 record integrity
COST=206 行实现 + 公共头 + 编译进每个 sluice_core 链接的死对象码 + 校验和为弱 additive checksum
（§9 recheck 复核确认；不构成"durable correctness asset"论证）
IMPACT=P5 GREEN（22/22）；唯一 vec 消费链随之消亡（I05–I08 的 gate chain 根）
VERDICT=DELETE
RATIONALE=§14 四要件：D1 无 owner（§13 open question 由本审计裁决为非 Core）；D2 独有语义存在但无
依赖它的 retained correctness contract；D3 无资源权威（Writer 装饰器）；D4/D6 零产品/测试角色；
D5 仅"被编译"不是合法构建角色；D7 公共头 + v0.0.1 发布事实记录为删除 slice 的 CHANGELOG/文档级联
义务而非保留理由；D8 retained contracts 不受影响（P5）；D9 无隐性 ADD_MINIMAL（scalar write_all 路径
仍存在于 Writer，且 Writer 本身也 DELETE——framing 若未来需要，按当时 evidence 重建）；D10 探针 GREEN。
**DELETE 语义澄清（Corrective-1 §10 强制）**：DELETE WAL 意味着"Core 不再声称提供 WAL"，
不意味着"record framing 无用"。framing/LSN 是真实语义能力（§9 逐项 recheck），但独有能力 ≠
architecture owner；实现规模大不构成降级 RESEARCH 的理由。本表最高风险 DELETE，单独标注供人审
重点复核；git 历史可完整恢复。
F/U=E（WAL slice，含 CHANGELOG "Removed" 条目 + architecture.md WAL 提及清理）
```

### I15 IoContext（抽象 factory）

```text
LOC=include/sluice/io_context.hpp:24-33   PUB=Y   BLD=Y   DIR=0（open_reader/open_writer 调用点=0）
实现者=BlockingIoContext + MemoryIoContext   TEST=0   APP=0
RESP=path→Reader/Writer 对象 factory（capability erasure 已证实：open_writer 返回 unique_ptr<Writer>，
sync_data/sync_all 不可达；open_reader 抹除 positional 能力）
CANON=File::open + blocking::/await_* 自由函数（无 factory）
UNIQ=无；capability erasure 正是 §13 列出的 open audit 问题，答案=是
AUTH=负权威：§2.2（按 access 方向派生两个 resource identity 类型被否定）+ §3.1（factory 签名无法
表达三轴）+ conformance §2.1 边界 1（execution 不得各有一套 File identity）+ §6.1/§6.3（common API
必须以 File& 为语义形状、不得隐藏能力收缩）
COST=抽象 factory + OpenReader/WriterOptions 结构 + 观测指针管线
IMPACT=P6 GREEN   VERDICT=DELETE
RATIONALE=factory 即重复本身：被抹除的能力已有正确 canonical owner，CONVERGE 无物可搬。四要件满足。
F/U=J
```

### I16 BlockingIoContext

```text
LOC=include/sluice/io_context.hpp:35-42 + src/io_context.cpp   PUB=Y   BLD=Y   DIR=0
RESP=Blocking execution 的对象 factory：manufacture FileReader/FileWriter（隐藏 O_TRUNC 的 open_writer）
CANON=blocking:: 自由函数族（first-class blocking，ADR §8.1）
UNIQ=无   AUTH=负权威（同 I15 四条 + §3.1 hidden truncate 具体违反）
COST=32 行 + 错误映射分支   IMPACT=P6 GREEN
VERDICT=DELETE   RATIONALE=同 I15   F/U=J
```

### I17 BlockingIoPool

```text
LOC=include/sluice/blocking_io_pool.hpp + include/sluice/detail/blocking_io_pool_impl.hpp + src/blocking_io_pool.cpp
PUB=Y   BLD=Y   DIR=0（全树零消费者，含 Task<T>/try_submit/submit/wait_idle/shutdown/PoolStats）
TEST=0   APP=0（sluice-tail 的唯一 offload 需求用裸 pthread 自建，见 apps/sluice-tail/main.cpp:88-124）
RESP=通用有界线程池（worker_count/queue_depth 双资源界 + Task<T> future + PoolStats）
CANON=ThreadPoolBackend（async 执行的 offload 机制）+ blocking::（caller 线程直呼）
UNIQ=独立于 async runtime 的通用池形状（真实机制，但 §13 问句裁决：与 ThreadPool 重复执行权且无 File
语义）
AUTH=无（execution policy/机制层，未定义任何 File semantics）
COST=224 行 + 双条件变量 + thread_local scope + 模板 impl 头
IMPACT=P4 GREEN   VERDICT=DELETE
RATIONALE=§13"是否拥有独立 execution owner，还是与 async ThreadPool 重复"→ 裁决为重复；零消费者
+ 四要件满足。   F/U=D
```

### I18 op_helpers

```text
LOC=include/sluice/async/op_helpers.hpp + src/async/op_helpers.cpp   PUB=Y   BLD=Y（sluice_async）
DIR=0（全树零消费者）   TEST=0   APP=0
RESP=AsyncIoContext+NativeFileRef 上的 read_all/write_all/sync_data_all/sync_all_all exact/all 组合
CANON=await_op_helpers（await_read_fill/await_write_exact，File-facing，7 个消费文件）——同样的 all
loop 语义已在该 canonical 文件拥有
UNIQ=无（one_step 轮询循环 vs canonical 的 RuntimeTaskContext await 语义，后者才是 §6 认可的 initiation
形态）
AUTH=无；把 NativeFileRef 当一等参数的组合层与 §6.2"explicit op reference 必须指向 File semantics"
的收敛方向相悖
COST=94 行重复组合机器   IMPACT=P3 GREEN
VERDICT=DELETE   RATIONALE=组合责任已被 canonical await_op_helpers 收敛；本文件是收敛前的旧载体。
四要件满足。   F/U=C（独立 slice，不与 Batch 合并——两者仅同为死叶，无共享不变式）
```

### I19 Batch

```text
LOC=include/sluice/async/batch.hpp + src/async/batch.cpp   PUB=Y   BLD=Y（sluice_async）   DIR=0（全树零消费者）
TEST=0   APP=0（sluice-copy 的 multi-op outstanding pipeline 用裸 ReadOp/WriteOp + Completion，无 Batch）
RESP=操作组组织器：add/await_one/next/pending_count（completion 按 reap_seq 排序弹出）
CANON=explicit ops + Completion（多 outstanding 本就一等支持）
UNIQ=无 group-admission authority（ADR-0001 §6 冻结的否定控制：group membership 不产生 fused/atomic
admission）；本类也确实不提供——它只组织调用，不拥有资源界、不隐藏执行语义
AUTH=无（这正是它该 DELETE 而非 KEEP 的原因：无赚到 authority 的机制无生存权）
COST=141 行 + Completion 私有 reap_seq 的唯一读权 + completion.hpp 两处 `friend class Batch;` +
tax0 f02_skip_reap_seq ablation seam 的存在理由（A7 §11：reap_seq 是每次 completion 的共享缓存行 RMW
生产成本，只有 Batch 死亡才可移除）
删除范围独立记录（Corrective-1 §16）：batch.hpp；batch.cpp；completion.hpp `friend class Batch;` ×2
（:32/:215）；Completion::reap_seq_ 成员 + reap_seq() 访问器（:176/:349）；next_reap_seq() 自由函数
（:25）；tax0_f02_skip_reap_seq seam（tax0_ablation_seams.hpp:12/:25）的处置。friend-of-undefined-class
合法 C++，故 P2 绿不代表耦合不存在。
IMPACT=P2 GREEN
VERDICT=DELETE
RATIONALE=零消费者 + 零 authority（机制只组织调用）+ ADR-0001 §6 否定其 generic 层资格。四要件满足。
F/U=B（Batch 独占 slice；与 op_helpers slice C 分离）
```

### I20 Group

```text
LOC=include/sluice/async/group.hpp + src/async/group.cpp   PUB=Y   BLD=Y（sluice_async）
DIR=ApplicationRuntime（root_group_：ctor:114 / async:224 / group_token:278 / close_resources move-out:555-558；
测试缝:615）
TRANS=四个 app（经 ApplicationRuntime）   TEST=0（直接）   APP=Y（传递，活）
RESP=任务组：evented spawn（fiber+stack+Future）、组级 CancelToken、终端检查
CANON=ApplicationRuntime 本身（runtime 已有 admitted/terminal 记账 + drain + lifecycle fail-fast）
UNIQ=spawn/token 组合（活）；await()/cancel()/size()/group_stop_predicate/线程模式（全零调用者）
AUTH=任务集生命周期（ADR §9 async correctness 域）；不触碰 File 语义（对抗审 AA-1 确认无 File 权威）
COST=203 行中约半数死亡：双执行模式（threaded 零实例化）、冗余终端记账（futures_ 扫描 vs
runtime 计数——跟踪不同事实，Corrective-2 措辞）、
双 fail-fast（group dtor vs runtime lifecycle）、EventedAdmissionFailPoint 测试缝（除 runtime 内联链外
零测试调用者）
**存储-释放权威修正（Corrective-2，替代 Corrective-1 的"终端权威修正"段落）**：~Group 的
futures_ 扫描（Future::ready()，group.cpp:78-82）**不是** storage-release 见证。complete_with
是 fiber entry 的语句（group.hpp:154，在 entry 内、先于 entry 返回），ready 见证的是 §10 的
T2（发布点），不是 T3（entry 返回）/T4（make_done）/T5（final context switch）——
FUTURE_READY ≠ FIBER_QUIESCENT。物理释放权威是 ApplicationRuntime teardown barrier：
join() 等 driver_state_==exited → driver_thread_.join() → close_resources() → group.reset()
→ ~Group clear（T8；run_impl 返回前 join 全部 worker 线程，T6 ⇒ 每个 fiber 的 T5 已发生，
§10 逐箭头 file:line）。~Group ready() 扫描的精确分类：**逻辑组完成 fail-fast / pending-task
误用检测**——它在 T7 之后求值，任务全部到达发布点时恒真；它不证明、也不需要证明栈已停。
IMPACT=不可直接删除（活根）；P7 未触碰
VERDICT=CONVERGE
RATIONALE=§14 CONVERGE 定义逐条命中：能力正确（spawn/token 被唯一合法根消费），但存在
wrong layer（公共类 + 死执行模式 + 死公共方法）与 fail-fast 权威重复（Group dtor 检查 vs
runtime lifecycle 检查——两个可观察终端概念并存于两层）。Corrective-2 措辞终版：不再使用
"双终端跟踪"或"终端-生存期见证重复"表述；futures_ 记账与 runtime 计数跟踪的是不同事实，
重复的是权威面而非事实本身。
F/U=O（Group/Future convergence slice；验收标准 = §10 INV-O1..O4）
```

### I21 Future（Corrective-2 零基础重裁：CONVERGE → DELETE）

```text
LOC=include/sluice/async/future.hpp；死链：include/sluice/async/wait_policy.hpp +
    src/async/wait_policy.cpp（ThreadedWaitPolicy/default_wait_policy）、
    include/sluice/async/evented_wait_policy.hpp（EventedWaitPolicy）
PUB=Y   BLD=Y   DIR=Group（全树唯一消费者；C12/C20）
TRANS=四个 app（经 Group/ApplicationRuntime，仅 spawn 链）   TEST=0   APP=0（直接）
RESP 逐成员分解（Corrective-2，全部 file:line 可核）：
  complete_with（唯一活调用 group.hpp:154）/ ready（唯一活消费 group.cpp:79）——发布+查询对，
    消费者是 ~Group 的逻辑 fail-fast 扫描（§10：T7 后求值的误用检测，非物理释放门）
  await（group.cpp:72/102，死线程路径）/ cancel / cancel_token（全树零调用者）/ 默认 ctor
    （死线程路径）——全死
  wait-policy notify 链——wait_until_ready 唯一调用者是死 await；notify_ready 在活路径触发
    （group.hpp:154 完成时）但与 wrapper 的 wake_handle_.notify()（application_runtime.cpp:241）
    重复，无独立活消费者
CANON=无单一 canonical 等价物。收敛后运行时真正需要的两个事实 runtime 均已拥有，且都强于
  Future::ready：（i）逻辑终端记账 terminal_count_（application_runtime.cpp:236）；（ii）物理
  quiescence barrier（run_impl join 全部 worker + driver join，§10 T6→T7）
UNIQ=caller-completable cell 形状；收敛后无公共 caller，形状失去主体
AUTH=任务终端事实的 publication（§9 域）；File 中立（对抗审 AA-1 确认）
**Corrective-2 零基础重裁（不预设维持 CONVERGE、不保计数）**：
  问句：死线程 Group/await/cancel 移除、Group 收敛进 ApplicationRuntime 之后，公共 Future
  还拥有什么 retained 责任？
  逐项裁决：complete_with/ready 的唯一活消费者是 Group 收敛后即消失的 fail-fast 扫描；
  逻辑终端事实 runtime 已有（terminal_count_）；物理释放由 quiescence barrier 承担（更强）；
  通知边与 wrapper wake 重复；线程兼容面全死；caller publication 无 caller。
  → **无独立存活的 Future 责任**。
  Corrective-1 的"complete_with/ready 同时是 fiber 终端-生存期见证的可执行载体（§10 fact D），
  收敛后必须以某内部机制保留该见证（INV-G1）"表述**作废**：ready 见证 T2，不见证
  T3/T4/T5；INV-G1 由 INV-O1/O2 的 quiescence barrier 取代（更强，且现实现已满足）。
  Future 区分（Corrective-2 §15 义务）：不是"Future 无用"，是"Future 的每项候选责任
  （组级完成状态/通知/线程兼容/caller publication）或死、或由 runtime 既有机制以更强形式
  承担"。
COST=每 spawn 一次 shared_ptr<Future> 分配 + mutex/cv/WaitPolicy 指针/CancelToken 成员 +
  wait-policy 死链三个文件
IMPACT=随 slice O 删除：group 收敛后无消费者；future.hpp、wait_policy.{hpp,cpp}、
  evented_wait_policy.hpp 从树中消失（C20）
VERDICT=DELETE
F/U=O（Group/Future convergence slice 内以删除落地；验收标准 = §10 INV-O1..O4）
```

### I22 include/sluice/experimental/uring_io_context.hpp

```text
LOC=include/sluice/experimental/   PUB=Y（{public=true} include 路径导出）   BLD=N（无任何目标编译其实现）
DIR=0（include 者仅 src/experimental 自身）   TEST=0   APP=0
RESP=path→write_file_all（内部 open(O_WRONLY|O_CREAT|O_TRUNC)+write+close）
CANON=File::open + await_write_at/blocking::write_at（+ safe_output 的原子输出职责）
UNIQ=无；path-based 资源身份 = §2.1 明文非 canonical 形态 + §3.1 hidden truncate
AUTH=负权威（与 canonical File 竞争 resource identity）
COST=编译成本为零但 API 负担为正：headers 编译干净、链接必然 undefined-reference（仅 inline set_stats
可链接）；一次 glob 改动（src/**.cpp）即静默再武装——默认无 liburing 配置下无任何编译错误
IMPACT=P1 GREEN   VERDICT=DELETE
RATIONALE=首要依据是边界违反（§2.1/§6.2/§3.1），未编译/零消费者为支持事实；任何权威文档均未为
experimental/ 命名空间保留地位（全 docs 零提及，A8 评论已要求纳入盘点）。四要件满足。
F/U=A（最高置信度）
```

### I23 include/sluice/experimental/uring_write_batch.hpp

```text
LOC=include/sluice/experimental/   PUB=Y   BLD=N   DIR=0   TEST=0   APP=0
RESP=raw-fd batch write（write_all(int fd, bytes, offset)，SLUICE_HAS_LIBURING guard，无 liburing 时诚实
返回 backend_error）
CANON=UringAsyncBackend（canonical uring execution）+ explicit ops
UNIQ=raw fd 公共参数 = §2.1/§6.2 明文否定形态；batch-shaped 机制 = ADR-0001 §6 已否定的 generic
Batch 层的又一实例
AUTH=负权威（同 I22）   COST=同 I22（已发布头 + 静默再武装风险）
IMPACT=P1 GREEN   VERDICT=DELETE   RATIONALE=同 I22   F/U=A
```

### I24 src/experimental/*（uring_io_context.cpp + uring_write_batch.cpp）

```text
LOC=src/experimental/   PUB=N（.cpp）   BLD=N（xmake globs 为单层 src/*.cpp 与 src/async/*.cpp；
compile_commands/.objs 双验证零条目）   DIR=0   TEST=0   APP=0
RESP=I22/I23 的实现（含 retry_uring_wait_on_eintr 调用——该 helper 本体在生产 uring_backend 侧，删除
本目录不影响生产）
CANON=无   UNIQ=无   AUTH=无   COST=零构建成本；风险在"被递归 glob 意外激活"（A8 评论），
且激活在默认配置下是静默的（对抗审 D 证实）
IMPACT=P1 GREEN   VERDICT=DELETE   RATIONALE=I22/I23 的实现体；同 slice 删除。   F/U=A
```

### I25 COPY SURFACE（Corrective-1 新增；ADR-0002 §11.1 独立裁决）

```text
成员={copy.hpp, copy.cpp, copy_strategy.hpp, copy_strategy.cpp, limit.hpp} ×
     {copy_all ×5 重载, CopyOptions, CopyStrategy(Auto/Scratch/BufferedFirst), CopyDecision,
      CopyLimit}
     （sluice::CopyStats 不在本行成员集：其删除 owner 是 I30 → slice P；本行仅为支持性
      引用/依赖，Corrective-2 唯一归属修正）
PUB=Y   BLD=Y   DIR=0（copy_all 全树消费者 = 0；C14）   TEST=0   APP=0
RESP（C1 责任判定）：
  a) EOF-driven bounded byte transfer（copy_all 核心循环）——ADR-0001 §6/ADR-0002 §11.1 接受的
     composed-transformation 正例责任；合法。
  b) mechanism-selection authority（CopyStrategy 三值 + CopyDecision 报告 + dynamic_cast 探测
     BufferedReadable fast path）——超出"thin local branch"形态的机制簿记；ADR-0001 §6 明文
     "Copy 只赚到 thin local mechanism，没有赚到 generic capability framework"；策略枚举/
     decision 报告/15 字段 stats 是 framework 形状，未被任何 ADR 授予 authority。
  c) copy statistics（CopyStats 15 字段）——hint/observation，非 authority。
  本面不拥有 local transformation authority（无加密/压缩/转码——纯字节搬运）。
DIR 依赖=Reader/Writer（I03/I04）+ BufferedReadable（I26）+ CopyStats（I30）；被依赖=Reader::stream_to
  两个 CopyLimit 重载（reader.cpp:49-58，C14）
CANON=copy 责任的 retained 实现已存在于 app 层：sluice-copy 在 canonical File + NativeFileRef +
  submit_read/submit_write/submit_sync_data/submit_sync_all + Completion drain 上自实现
  sequential/pipelined copy（apps/sluice-copy/copy_task.cpp），自带 sluice_copy::CopyStats（C1 复核）
C2 消费者裁决：library copy_all 零消费者。sluice-copy app ≠ library copy_all API——app 不 include
  copy.hpp，不调用任何 copy_all 重载；两者仅有"CopyStats"同名（不同 namespace、不同定义）。
C3 canonical replacement 裁决：是——retained Copy 责任已由 app 在 canonical API 上直接实现。
  因此：**RESPONSIBILITY = KEEP（app 层 + ADR §11.1 正例地位）；CURRENT LEGACY SURFACE = DELETE**。
  §14 verdict 依附于实现面而非责任：删除的是零消费者的 library 载体（framework 形状），
  不是 Copy 责任本身。未来若有真实 consumer 证明需要 library 级 bounded transfer，
  按当时 evidence 走新 ADD_MINIMAL 节点（thin 形态），本删除不构成其障碍。
AUTH=无 owner（零消费者；ADR 授予的是"composed operation 可为合法 transformation boundary"
  的设计论点，不是对 master 现存 library 面的生存权——§14：从 capability tree 出发，
  class 存在不构成 authority）
UNIQ=无（transfer 语义与 §5.2 短 I/O 组合可由 caller 以 canonical 原语表达；sluice-copy 即实证）
COST=3 头 2 TU + strategy/decision/stats 簿记 + 对 BufferedReadable 的 RTTI 耦合
IMPACT=P7 GREEN（文件集恰被包含）；Reader 侧连带：reader.{hpp,cpp} 的 stream_to 两个 CopyLimit
  重载随本 slice 修剪（编译边，C14）
VERDICT=DELETE
F/U=M（Copy surface slice）
```

### I26 BUFFERED SURFACE（Corrective-1 新增；§13 item 6 显式裁决）

```text
成员={buffer.hpp, buffer.cpp, buffered_readable.hpp} × {BufferedReader, BufferedWriter,
     BufferedReadable}
     （BufferStats 不在本行成员集：其删除 owner 是 I30 → slice P；本行仅为支持性引用，
      Corrective-2 唯一归属修正）
PUB=Y   BLD=Y   DIR=BufferedReader/Writer：0（C15）；BufferedReadable：唯一消费者 copy.cpp:57
     dynamic_cast（I25 的 fast path）   TEST=0   APP=0
RESP=真实缓冲语义：read refill/seek/end 管理 + write 聚合/flush 契约（dtor 断言未 flush 即失败）+
     peek/consume 协议（BufferedReadable）+ BufferStats
问句裁决（Corrective-1 §6）：
  谁消费？——BufferedReader/Writer 零消费者；BufferedReadable 仅 copy_all fast path。
  Copy 是否消费 BufferedReadable？——是（dynamic_cast）；该消费随 copy_all 删除而消失
  （依赖序：Copy 先裁，见 §17 DAG）。
  是否拥有真实缓冲语义？——是（非空壳）。
  当前产品是否需要 library 级缓冲？——否：四个 app 零使用；sluice-copy 的 pipeline 用固定
  buffer 槽自管理，不经 Buffered*。
  缓冲是否为无 owner 的合法机制？——是。合法 ≠ 保留：§14 无 owner + 无 product role → DELETE；
  "Buffered* 是否有 product owner"（§13 item 6）答案 = 无。
AUTH=无   CANON=无（canonical 面无缓冲层；缓冲属 caller 策略）
UNIQ=缓冲语义真实但无 retained 消费者；不以"Reader/Writer 删除→Buffered* 删除"为唯一论证
  （本行独立于 I03/I04 成立：即便 Reader/Writer 保留，零消费者 + 无 product owner 仍成立）
COST=2 头 1 TU + flush 契约 + stats
IMPACT=P7 GREEN（文件集恰被包含）
VERDICT=DELETE
F/U=K（Buffered surface slice，在 M 之后）
```

### I27 MEMORY TEST SUBSTRATE（Corrective-1 新增；§13 item 7 拆分）

```text
成员={memory_io_context.hpp} × {MemoryIoContext} + fault.hpp 内 {MemoryReader, MemoryWriter}
PUB=Y   BLD=Y（fault.cpp 编译；memory_io_context.hpp header-only）   DIR=MemoryIoContext include 者=0；
     MemoryReader/Writer 消费者仅 memory_io_context.hpp/fault.hpp 自身（C16）   TEST=0   APP=0
RESP=无文件系统的 in-memory byte-stream substrate + path→Reader/Writer 内存 factory（seed/store）
问句裁决（Corrective-1 §7）：
  当前测试使用？——否（C2：22 测试全 canonical，对真实 File/uring 验证）。
  外部示例使用？——否。
  是否提供他处不可得的 correctness-injection 能力？——否：本行是数据源替身，非注入器；
    canonical 测试以真实临时文件获得更强的真实-semantics 覆盖。
  删除是否移除 ADR 强制的测试权威？——否：无任何 ADR 强制 in-memory substrate；
    AGENTS clean-room 纪律=测试从当前行为重建，禁止 hypothetical-future-user 保留。
AUTH=无   CANON=无（canonical 测试不需要替身）
UNIQ=无 retained 依赖者
COST=1 头（+fault.hpp 内两个类）+ IoContext 实现耦合
IMPACT=P7 GREEN（文件集恰被包含；物理文件 fault.hpp 与 I28 共享，删除在同一 slice 落地）
VERDICT=DELETE
A7 supersession：A7 §M 曾按 RESEARCH 保留（"为测试重建提供不可替代 substrate"）——本审计显式
supersede：其保留条件已被现状否定（canonical 测试零使用 + clean-room 政策 + 基类随 I03/I04 消失）。
F/U=L（Memory/Fault substrate slice）
```

### I28 FAULT INJECTION SUBSTRATE（Corrective-1 新增；§13 item 7 拆分）

```text
成员={fault.hpp（FaultPlan/FaultReader/FaultWriter 部分）, fault.cpp}
PUB=Y   BLD=Y   DIR=0（自身 2 文件外消费者 = 0；C17）   TEST=0   APP=0
RESP=计划式故障注入：fail_after_read/write_calls、fail_after_bytes、max_read/write_size（短 I/O
塑形）、fail_flush、可配置 error code——经 Reader/Writer 装饰器注入
问句裁决（Corrective-1 §7）：
  correctness-injection 能力是否他处不可得？——能力真实（短-I/O 与错误序列注入是
    retained-contract 边界测试的真实工具）。
  该能力当前是否被要求？——否：canonical 22 测试零使用；当前无任何重建中测试声明需要它。
  删除是否移除 ADR 强制权威？——否（同 I27：clean-room 政策 + 无 ADR 义务）。
  唯一保留论证="未来测试可能需要"= AGENTS 明文禁止的 hypothetical-future-user。
AUTH=无   CANON=无   UNIQ=能力真实、要求为零
COST=1 头 1 TU + 装饰器对（与 I27 物理同文件）
IMPACT=P7 GREEN（文件集恰被包含）
VERDICT=DELETE
（若未来 retained 测试真实需要注入器，按当时行为走新 ADD_MINIMAL 节点；git 历史可恢复。）
F/U=L（与 I27 同 slice——物理文件共享使拆分不可编译；责任已在两行分别 traceable）
```

### I29 SYNCABLEWRITER（Corrective-1 新增）

```text
LOC=include/sluice/sync.hpp   PUB=Y   BLD=Y（header）   DIR=实现者={FileWriter}；消费者={WalWriter}
     （C18）   TEST=0   APP=0
RESP=durability 能力的 side-interface：sync_data()/sync_all() 虚对
问句裁决（Corrective-1 §8）：
  谁实现？——仅 FileWriter（file.hpp:67 多重继承 Writer, SyncableWriter）。
  谁消费？——仅 WalWriter（构造注入 SyncableWriter*，sync() 经它门控 durability）。
  是否编码 File 已有的 durability 权威？——是：canonical File 面 §5.4 已拥有 sync_data/sync_all
    全套（blocking:: + await_*）；SyncableWriter 无新增语义。
  是否只是围绕 Writer 的 capability-erasure 修复？——是：Writer 接口只含 write/flush，
    durability 被类型系统抹除，SyncableWriter 以第二接口把能力重新挂回——与 I15 IoContext
    factory 的 erasure 是同一模式（§13 item 3 的接口级重演）。
  WAL 删除后是否随之消失？——实现者（I02）与消费者（I14）均在删除集内；本行必须独立成行：
    "WAL 删除 ⇒ SyncableWriter 删除"不是传递推理，独立依据是上面三条（无新语义 + erasure
    修复形状 + 实现者/消费者全死）。
AUTH=无（durability authority 归 File，§5.4）   CANON=File durability 面
UNIQ=无   COST=1 公共头 + 继承耦合
IMPACT=P7 GREEN（sync.hpp 在 P7 的 29 文件集内，精确包含）
VERDICT=DELETE
F/U=N（在 E 与 H 之后）
```

### I30 LEGACY STATS CLUSTER（Corrective-1 新增；§13 item 11 stats 部分）

```text
成员={SyscallStats(:7-14), BufferStats(:16-33), sluice::CopyStats(:35-54), SyncStats(:56-61),
     UringStats(:63-70)}（measurement.hpp）
     （Corrective-2 归属修正：UringStats 的删除 owner 由 slice A 的"顺带"改归本行 → slice P；
      slice A 只删 4 个 experimental 文件。VectorStats 为 I11 → slice G；ReaderStats/WriterStats
      为 I12/I13（observed.hpp 内定义）——支持性引用，非本行成员。）
PUB=Y   BLD=Y   DIR=C19：SyscallStats/SyncStats→file.hpp/io_context.hpp 管线（死）；
     BufferStats→buffer.hpp（死）；CopyStats→copy.hpp/reader.hpp/reader.cpp/copy.cpp（死）；
     UringStats→experimental/ 两头文件（死，未编译）；全部为死管线   TEST=0   APP=0
问句裁决（Corrective-1 §9：逐责任簇，不用"AsyncStats 存在⇒其余重复"的偷懒论证）：
  SyscallStats/SyncStats（read/write syscall 与 durability 计数）：live consumer=0；
    test role=0；perf/research role=0（tax0 census 已由 canonical 侧取代）；observation owner=
    AsyncStats 管线（canonical 观测权威）。观察对象（FileWriter 管线）本身 DELETE——
    无被观察者即无观察责任。
  BufferStats：同上，观察对象 = I26。
  sluice::CopyStats：同上，观察对象 = I25；app 的 sluice_copy::CopyStats 独立存活于 app
    （不随本行删除）。
  UringStats：同上，观察对象 = I22/I23（set_stats 注入，唯一消费点）。
  区分记录：不同观察可以合法不同——本裁决不否认 syscall 计数与 async 计数是不同观察；
  否定的是"无存活观察对象 + 无 retained 消费者"的 struct 的生存权（§14：hint/observation
  非 authority；零产品/测试角色）。
AUTH=无   CANON=AsyncStats（唯一 canonical 观测 struct，保留）
COST=5 struct（measurement.hpp:7-70）+ 它们给 legacy/experimental 头提供的存在理由
IMPACT=P8 GREEN（P7 之上的 measurement.hpp 修剪；文件保留，仅余 AsyncStats——
     该终态同时覆盖 I11 的 VectorStats 删除，见 §9 证据复用声明）
VERDICT=DELETE（struct 级；measurement.hpp 文件保留）
F/U=P（residual stats trim slice，最后执行；入边 {A,H,J,K,M}，§17）
```

---

## 8. Final verdict table

| ID | Surface | Root consumer/role | Unique authority/semantics | Verdict | Evidence | Implementation slice |
| -- | ------- | ------------------ | -------------------------- | ------- | -------- | -------------------- |
| I01 | FileReader | none（仅死 factory 引用） | none（read_at_exact 见 §9 AA-3） | DELETE | P7 | H |
| I02 | FileWriter | none | none（隐藏 truncate = §3.1 违反） | DELETE | P7 | H |
| I03 | Reader | none（SCC 内部互指） | read_exact EOF-as-error（唯一载体，合法缺席可记录） | DELETE | P7 | I |
| I04 | Writer | none（SCC 内部互指） | none | DELETE | P7 | I |
| I05 | Reader vec defaults | none | cross-buffer advancement（无存活依赖） | DELETE | P7 | F |
| I06 | Writer vec defaults | WAL（已裁 DELETE） | scatter 组合载体 | DELETE | P7 | F |
| I07 | FileReader read_vec/read_vec_at | none | 单 syscall 机制（非 authority） | DELETE | P7 | F |
| I08 | FileWriter write_vec/write_vec_at | WAL（已裁 DELETE） | OS 机制属性 ≠ Sluice 语义权威（§9 两分） | DELETE | P7 | F |
| I09 | IoSlice | vec 方法族（死） | none | DELETE | P7 | F |
| I10 | ConstIoSlice | vec 方法族（死） | none | DELETE | P7 | F |
| I11 | VectorStats | legacy vec 管线（死） | none（canonical=AsyncStats） | DELETE | P7+P8 | G |
| I12 | ObservedReader | none（全树零） | none | DELETE | P7 | G |
| I13 | ObservedWriter | none（全树零） | none | DELETE | P7 | G |
| I14 | WAL | none（全树含测试零） | record framing/LSN（无依赖 contract；DELETE 语义澄清见 §9） | DELETE | P5 | E |
| I15 | IoContext | none（调用点=0） | none（capability erasure 实证） | DELETE | P6 | J |
| I16 | BlockingIoContext | none | none（隐藏 O_TRUNC） | DELETE | P6 | J |
| I17 | BlockingIoPool | none（全树零） | 通用有界池形状（与 ThreadPool 重复） | DELETE | P4 | D |
| I18 | op_helpers | none（全树零） | none（await_op_helpers 已收敛该责任） | DELETE | P3 | C |
| I19 | Batch | none（全树零） | none（无 group-admission authority） | DELETE | P2 | B |
| I20 | Group | ApplicationRuntime → 4 apps | spawn/token（活）+ 半数死亡成员；存储释放权威 = teardown barrier（§10），ready() 扫描 = 逻辑 fail-fast | CONVERGE | 活根（探针不适用） | O |
| I21 | Future | Group（唯一，随收敛消失） | 无独立存活责任（Corrective-2 零基础重裁：逻辑记账/物理屏障 runtime 均已有且更强；wait-policy 死链 C20） | DELETE | 活根（随 slice O 删除；行为等价+INV-O 见证测试，探针不适用） | O |
| I22 | experimental/uring_io_context.hpp | none（未编译不可链接） | none（raw path 权威 = 边界违反） | DELETE | P1 | A |
| I23 | experimental/uring_write_batch.hpp | none（未编译不可链接） | none（raw fd + 已否定 batch 机制） | DELETE | P1 | A |
| I24 | src/experimental/* | none（不在构建图） | none | DELETE | P1 | A |
| I25 | COPY SURFACE（copy_all 族 + Options/Strategy/Decision/Limit） | none（C14；app 自实现 canonical copy） | bounded-transfer 责任合法但 app 层已持有；library 面 framework 形状无 owner | DELETE | P7（复用） | M |
| I26 | BUFFERED SURFACE（BufferedReader/Writer/Readable） | copy.cpp dynamic_cast（唯一，死） | 真实缓冲语义、无 product owner（§13 item 6 答案） | DELETE | P7（复用） | K |
| I27 | MEMORY TEST SUBSTRATE（MemoryIoContext/MemoryReader/MemoryWriter） | none（C16） | in-memory 替身；无 ADR 义务（supersede A7 §M） | DELETE | P7（复用） | L |
| I28 | FAULT INJECTION SUBSTRATE（FaultPlan/FaultReader/FaultWriter） | none（C17） | 注入能力真实、当前要求为零 | DELETE | P7（复用） | L |
| I29 | SyncableWriter | 实现=FileWriter、消费=WalWriter（均死） | durability side-interface = capability-erasure 修复；权威归 File §5.4 | DELETE | P7（复用，sync.hpp 精确在集内） | N |
| I30 | LEGACY STATS CLUSTER（SyscallStats/SyncStats/BufferStats/CopyStats/UringStats） | 死管线观察 struct（C19） | 无存活观察对象、无 retained 消费者（AsyncStats 保留） | DELETE | P8 | P |

每行恰好一个 verdict；无缺行；无重复行；每个未来将被删除/收敛的文件与符号恰好映射到一行
（grouped 行的成员显式枚举于卡片）。计数：**TOTAL=30，DELETE=29，CONVERGE=1，KEEP=0，
ADD_MINIMAL=0，RESEARCH=0，OUT_OF_SCOPE 行=0**（§13 非行级 OUT_OF_SCOPE 处置 4 项见 §6）。

Traceability 链：AUDIT VERDICT（本表）→ IMPLEMENTATION SLICE（§17，含逐 slice 文件枚举）→
FILE/SYMBOL DIFF（未来实现 PR 的 diff 须恰好覆盖其所引行的成员枚举）。
**Corrective-2 唯一归属**：每个计划移除符号有且只有一个 audit owner、一个 implementation
slice deletion owner；支持性/依赖性行可以引用该符号但不得声称删除它。机器可核对的逐符号
ledger 见 §8.1；计数器：DUPLICATE_AUDIT_OWNER_COUNT=0、DUPLICATE_SLICE_OWNER_COUNT=0、
UNAUDITED_PLANNED_REMOVAL_COUNT=0。

### 8.1 Traceability ledger（Corrective-2 强制输出；M1 闭合）

格式：| Symbol / surface | Audit owner | Supporting rows（仅引用，不拥有） | Implementation slice | Deletion owner |
每个符号的 Audit owner 列与 Implementation slice 列各自全列唯一。

| Symbol / surface | Audit owner | Supporting rows | Implementation slice | Deletion owner |
| ---------------- | ----------- | --------------- | -------------------- | -------------- |
| VectorStats（measurement.hpp:72-81） | I11 | I01, I02, I12, I13, I15, I16 | G | slice G |
| SyscallStats（measurement.hpp:7-14） | I30 | I01, I02, I15, I16 | P | slice P |
| SyncStats（measurement.hpp:56-61） | I30 | I02, I15, I16 | P | slice P |
| BufferStats（measurement.hpp:16-33） | I30 | I26 | P | slice P |
| sluice::CopyStats（measurement.hpp:35-54） | I30 | I25（app 侧 sluice_copy::CopyStats 独立存活） | P | slice P |
| UringStats（measurement.hpp:63-70） | I30 | I22, I23（set_stats 唯一消费点） | P | slice P |
| ReaderStats（observed.hpp:11） | I12 | — | G | slice G |
| WriterStats（observed.hpp:18） | I13 | — | G | slice G |
| copy_all ×5 重载（copy.hpp:15-26） | I25 | I03（stream_to 两重载调用，随 M 修剪） | M | slice M |
| CopyOptions（copy_strategy.hpp:16） | I25 | — | M | slice M |
| CopyStrategy（copy_strategy.hpp:10） | I25 | — | M | slice M |
| CopyDecision（copy_strategy.hpp:21） | I25 | — | M | slice M |
| CopyLimit（limit.hpp） | I25 | I03（stream_to 签名引用） | M | slice M |
| BufferedReader（buffer.hpp:15） | I26 | I25（dynamic_cast fast path） | K | slice K |
| BufferedWriter（buffer.hpp:42） | I26 | — | K | slice K |
| BufferedReadable（buffered_readable.hpp:10） | I26 | I25 | K | slice K |
| FileReader（file.hpp） | I01 | I16（factory 制造）、I29（实现 SyncableWriter） | H | slice H |
| FileWriter（file.hpp） | I02 | I16、I29 | H | slice H |
| Reader（reader.hpp） | I03 | I14, I12, I26, I27, I28, I25（实现者/装饰器，全死） | I | slice I |
| Writer（writer.hpp） | I04 | I14, I13, I26, I25 | I | slice I |
| Reader::read_vec / read_vec_all（reader.hpp:31-33） | I05 | I12（override） | F | slice F |
| Writer::write_vec / write_all_vec（writer.hpp:21-23） | I06 | I13（override）、I14（唯一外部调用 wal.cpp:105） | F | slice F |
| FileReader::read_vec / read_vec_at（file.hpp:50-54） | I07 | — | F | slice F |
| FileWriter::write_vec / write_vec_at（file.hpp:103-107） | I08 | I14（链根，已裁 DELETE） | F | slice F |
| IoSlice（iovec.hpp:8） | I09 | I05–I08 | F | slice F |
| ConstIoSlice（iovec.hpp:12） | I10 | I05–I08 | F | slice F |
| SyncableWriter（sync.hpp） | I29 | I02（实现者）、I14（消费者） | N | slice N |
| WalWriter + wal:: record surface（wal.hpp / wal.cpp） | I14 | I29（durability 门注入） | E | slice E |
| Batch / BatchOp / BatchResult（batch.{hpp,cpp}） | I19 | — | B | slice B |
| Completion::reap_seq_ 成员 + reap_seq()（completion.hpp:176,180 及第二类 :343 区段） | I19 | — | B | slice B |
| next_reap_seq()（completion.hpp:25，detail） | I19 | — | B | slice B |
| friend class Batch ×2（completion.hpp:32,215） | I19 | — | B | slice B |
| tax0_f02_skip_reap_seq seam（tax0_ablation_seams.hpp:12,25-26） | I19 | — | B | slice B |
| IoContext 抽象（io_context.hpp:24） | I15 | I27（MemoryIoContext 实现者） | J | slice J |
| BlockingIoContext（io_context.hpp:35） | I16 | I01, I02（制造对象） | J | slice J |
| OpenReaderOptions / OpenWriterOptions（io_context.hpp:13,18） | I15 | — | J | slice J |
| BlockingIoPool / Task<T> / PoolStats / try_submit / wait_idle / shutdown（blocking_io_pool.hpp + detail impl + .cpp） | I17 | — | D | slice D |
| async::op_helpers read_all / write_all / sync_data_all / sync_all_all（op_helpers.{hpp,cpp}） | I18 | — | C | slice C |
| MemoryIoContext（memory_io_context.hpp:14） | I27 | — | L | slice L |
| MemoryReader / MemoryWriter（fault.hpp:39,16） | I27 | — | L | slice L |
| FaultPlan / FaultReader / FaultWriter（fault.hpp:70,80,98） | I28 | — | L | slice L |
| ObservedReader（observed.hpp:27） | I12 | — | G | slice G |
| ObservedWriter（observed.hpp:46） | I13 | — | G | slice G |
| experimental uring_io_context.hpp（include/sluice/experimental/） | I22 | I30（UringStats 支持性引用） | A | slice A |
| experimental uring_write_batch.hpp（include/sluice/experimental/） | I23 | I30（UringStats 支持性引用） | A | slice A |
| experimental src 两 TU（src/experimental/{uring_io_context,uring_write_batch}.cpp） | I24 | — | A | slice A |
| Group 类型（group.hpp / group.cpp） | I20 | — | O | slice O（CONVERGE：类型随收敛消亡） |
| Group 死成员：await(:70) / cancel(:72) / size(:77) / group_stop_predicate(:83) / async_threaded(:85) / 线程模式 Group()(:24) / EventedAdmissionFailPoint 缝(:48-55) | I20 | — | O | slice O |
| Future 类型（future.hpp） | I21 | I20（唯一消费者，收敛即消失） | O | slice O |
| Future::complete_with(:27) / ready(:58) | I21 | I20（消费点 group.hpp:154 / group.cpp:79） | O | slice O |
| Future::await(:46) / cancel(:53) / cancel_token(:44) / 默认 ctor(:19) | I21 | — | O | slice O |
| WaitPolicy / ThreadedWaitPolicy / default_wait_policy（wait_policy.hpp + wait_policy.cpp:5-6） | I21 | I20（evented_policy_ 成员类型引用） | O | slice O |
| EventedWaitPolicy（evented_wait_policy.hpp） | I21 | I20（构造者，group.cpp:22） | O | slice O |

Ledger 计数（机械核对口径：每行 Audit owner 列引用的盘点行存在且唯一；Implementation slice
列值 ∈ A–P 且与 §8 表 F/U 列一致；supporting rows 不出现在任何 slice 的删除文件枚举中
作为 owner）：

```text
DUPLICATE_AUDIT_OWNER_COUNT      = 0
DUPLICATE_SLICE_OWNER_COUNT      = 0
UNAUDITED_PLANNED_REMOVAL_COUNT  = 0
```

说明两处多行同 slice 的合法形态：I22/I23/I24 同落 slice A（同一 island）；I21 与 I20 同落
slice O（同一权威变更：任务组责任收敛）。"同 slice"不产生双 owner——owner 是盘点行，
slice 是实现载体；每个符号仍恰有一个 owner 行与一个 slice。

---

## 9. DELETE proof packets（Corrective-1 修订）

每个 DELETE 按 §22 D1–D10 记录。共同事实（适用于全部）：canonical 头/app/test 对删除集合零依赖
（§4 C1–C3、C14–C19）；基线双配置（默认 20/20、liburing=y 22/22，§2）。

### 通用证据基座

- **D8/D10（契约保持 + 探针）**：P1–P7 七个隔离 worktree（均基于 ff916c37、detach、未提交、
  已清理）删除候选后 `xmake -br` 全目标构建成功、`xmake test` **22/22 passed**（含 4 个 app
  消费测试、uring smoke/probe 真实 liburing 路径）。**P8（Corrective-1 新增）**：P7 的 29 文件
  删除集 + `measurement.hpp` 修剪至仅 AsyncStats——liburing=y **22/22 GREEN**，默认配置
  **20/20 GREEN**。
- **D4/D6（产品/测试角色）**：§4 C1/C2/C14–C19 census；对抗审独立复核确认 app/test include
  集合精确为 14 个 canonical 头。
- **D9（无隐性 ADD_MINIMAL）**：全部删除面的能力均有 canonical 载体、由 retained app 实现、
  或经 §14 合法缺席（显式记录的三处例外：read_exact EOF 组合见 AA-3；WAL framing 见 I14 D9
  与 §9 recheck；Copy bounded-transfer 责任见 I25 C3——责任存活于 app，非能力消灭）。

### P7 精确 29 文件删除集（pin，供人审与未来 slice 复用）

```text
headers (17): include/sluice/{file,reader,writer,iovec,observed,wal,sync,memory_io_context,fault,
              buffer,buffered_readable,copy,copy_strategy,limit,io_context,blocking_io_pool}.hpp
              + include/sluice/detail/blocking_io_pool_impl.hpp
srcs (12):    src/{file,reader,writer,observed,wal,buffer,copy,copy_strategy,fault,io_context,
              blocking_io_pool}.cpp + src/file_test_seams.hpp
结果：sluice_core 仅余 blocking_file.cpp + file_resource.cpp 编译；22/22 GREEN
```

### 证据复用声明（Corrective-1 §18 强制）

新显式 DELETE 行对 P7 的复用逐行声明如下。复用条件：(i) 该行全部文件/符号可证明包含于 P7
精确文件集；(ii) 该行 verdict 不依赖 P7 未测试的更窄行为。两条件逐行满足——verdict 的证据是
零消费者 census（§4）+ 无 owner 的权威论证（各卡片）+ 删除后编译/测试全绿，而非任何仅被删除
面提供的运行时行为：

```text
I25 Copy：文件集 {copy,copy_strategy,limit}.hpp + {copy,copy_strategy}.cpp ⊆ P7 ✓（P7 第 1/5 行）
I26 Buffered：{buffer,buffered_readable}.hpp + buffer.cpp ⊆ P7 ✓
I27/I28 Memory/Fault：memory_io_context.hpp + fault.hpp + fault.cpp ⊆ P7 ✓
I29 SyncableWriter：sync.hpp ⊆ P7 ✓（实现者 FileWriter/消费者 WalWriter 亦在集内）
I11 VectorStats：struct 不在 P7 内（P7 未动 measurement.hpp）→ P8 终态覆盖 ✓（Corrective-2：
   该 struct 的实现载体是 slice G；P8 证明的是"引用者清零后 struct 可整体删除"这一终态，
   编译依赖由 §17 的 {H,J}→G 边承载）
I30 stats trim（5 struct）：不在 P7 内（P7 未动 measurement.hpp）→ 由新探针 P8 单独覆盖 ✓
```

### 单表面 DELETE 包（逐条差异项）

```text
I01/I02 FileReader/FileWriter：D1=§2.2 否定 resource-identity 生存权 + canonical File 是唯一 root；
  D2=语义全同 §5.2/§5.4 canonical；D3=int fd ctor 绕过 access-legality 权威 + O_TRUNC 隐藏（违反 §3.1，
  是删除的加重理由而非保留理由）；D7=公共头 + v0.0.1 发布事实→记录 CHANGELOG 级联义务。
I03/I04 Reader/Writer：D1=generic byte-stream owner 未被证明（非文件实现者全部为零消费者替身）；
  D2=read_exact EOF 组合为唯一独有语义，合法缺席记录见 AA-3；D7=公共头负担记录。
I05–I08 vectored：D1=§5.3 evidence-gated 无 authority；gate chain（vectored-decision §5）满足——
  I14 先裁；D2=cross-buffer 推进/单 syscall 机制无存活依赖者，reopen 条件不因删除失效（未来真实
  consumer 证据仍可开新 narrow node）；D7=零测试覆盖输入事实记录在案；I08 的机制属性两分见下节。
I09–I13 slices/stats/observed：全部仅服务死 vec 管线；D2/D3 无。
I14 WAL：D1=§13 open question 由本审计裁决为非 Core（open≠grant）；D2=framing/LSN 真实但无依赖
  contract（safe_output 解决 materialization 原子性而非 record integrity）；D5=死对象码编译进 .a；
  D7=CHANGELOG "[Unreleased] Removed" + architecture.md:63 提及清理为删除 slice 义务；
  D10=P5 GREEN。最高风险 DELETE——建议人审优先复核本行。
I15/I16 IoContext/BlockingIoContext：D2=capability erasure 实证（open_writer 抹除 durability、
  open_reader 抹除 positional）；D1=§2.2/§3.1/conformance §2.1 边界 1/§6.1/§6.3 多重边界否定。
I17 BlockingIoPool：D1=与 ThreadPoolBackend 执行权重复且无 File 语义；D5=死 TU 在 .a。
I18 op_helpers：D1=组合责任已由 await_op_helpers 在 canonical 面拥有；D2=NativeFileRef 一等组合层
  与 §6.2 收敛方向相悖。
I19 Batch：D1=ADR-0001 §6 否定 generic Batch 层；D2=无 group-admission authority（机制只组织调用）；
  删除范围含 completion.hpp friend×2 + reap_seq/f02 seam 处置（A7 §M）。
I22–I24 experimental：D1=任何权威文档零保留；D2=§2.1/§6.2/§3.1 边界违反（首要依据）；D5=不在构建图；
  D7=已发布头（编译干净/链接失败/静默再武装）作为删除（而非弃置）的决定性支持。
I25 Copy：D1=零消费者 + §11.1 授予的是设计论点非 library 面生存权；D2=bounded-transfer 责任由
  retained app 实现（C3），strategy/decision/stats framework 形状超出 thin-branch 已 earn 形态；
  D4/D6=C14；D8=P7；D9=责任不灭（app 层 + 未来 ADD_MINIMAL 通路显式保留）。
I26 Buffered：D1=无 product owner（§13 item 6 裁决）；D2=缓冲语义真实但无 retained 依赖；
  唯一消费者链随 I25 消失；D4/D6=C15；D8=P7。
I27/I28 Memory/Fault：D1=无 owner + AGENTS clean-room 禁止 hypothetical-user 保留；D2=替身/注入
  能力真实但当前要求为零；D4/D6=C2/C16/C17；D8=P7；A7 §M supersession 显式记录于 I27。
I29 SyncableWriter：D1=无独立权威（durability 归 File §5.4）；D2=capability-erasure 修复形状
  （同 I15 模式）；D3=实现者/消费者全在删除集；D8=P7。
I30 stats：D1=hint/observation 非 authority；观察对象全死；D4/D6=C19；D8=P8；AsyncStats 区分保留。
```

### Vectored fact corrective（MAJOR-3；适用于 I05–I13，尤其 I08）

初版报告在 I08 写有"常规 fd 上的 writev 不提供跨 buffer 原子性承诺"，该断言被**撤销**。
它混淆了两个不同层面的命题（Corrective-1 §12 一般化：API contract ≠ OS capability）：

```text
1. OS 机制属性（事实层）：writev/readv 作为单次 syscall，在相关 OS 对象语义下确实可以提供
   有意义的单操作/非交错属性——POSIX 将 regular file 上的 write 家族规定为互原子（一次
   write(v) 的字节不与他者交错），pipe 上总量 ≤ PIPE_BUF 的 writev 是原子的，O_APPEND 使
   整组追加原子地发生在文件尾。这些是真实的机制能力，本审计不再否认。
2. Sluice 语义权威（契约层）：Sluice 公共面从未把上述任一属性冻结为 retained
   Writer/FileWriter contract——无文档声明、无测试见证、无 ADR 条文（§5.3 反而把 vectored
   冻结为 evidence-gated extension）；且无 retained consumer 证明依赖（唯一 vec 消费者 WAL
   自身 verdict DELETE、无下游）。
1 ≠ 2；"机制存在"为真与"契约缺席"为真不矛盾（§12）。
```

DELETE verdict 因此**维持**，但论证替换为语义权威分析：无 owner（§5.3 gate 未满足）+ 无
依赖契约的 retained consumer + 无测试/产品角色 + 删除保持 retained scalar contracts（P7）。
未来若真实 consumer 证明需要单操作/非交错语义（如 pipe 上多 buffer 记录完整性），该证据
恰好是 §5.3 gate 的满足形态——reopen 条件不因本删除失效，反而被本两分精确化。

同源问题记录：`docs/roadmap/explicit-file-vectored-decision.md` §5 的 reopen-condition 括注
（"readv/writev 本身不提供跨 buffer 原子性"）含同一不准确表述。该文件不在本 corrective 的
允许文件集内（本审计 docs-only、单文件），不作修改；已开独立 docs corrective tracking issue
[#370](https://github.com/jnhu76/Sluice/issues/370)（Corrective-2 §19：范围=该括注改写为
上述两分表述；不变更 A7 verdict、不推广 canonical vectored；#355 关闭前须完成）。reopen
条件的运作部分（真实 consumer + scalar-composition 不足证据 + §5.3 parity 义务）不受影响。

### WAL human-review recheck（Corrective-1 §10）

按最高风险假设逐项复核，verdict 维持 DELETE：

```text
record framing：magic(0x57414C)|len(u32 LE)|payload|checksum(u32 LE)——真实、自洽（wal.cpp）
checksum：additive 32-bit 弱校验（wal.cpp:42-50）——非 durable correctness asset
LSN 三层：written/flushed/durable（wal.hpp:36-38）——真实语义能力
SyncableWriter hook：WalWriter(Writer&, SyncableWriter*) durability 门（wal.hpp:27/43）——真实
公共头负担：wal.hpp 在 {public=true} include 根导出
v0.0.1 发布：git show v0.0.1:src/wal.cpp 存在（实证，非转录）
zero consumer：C4 维持；zero test：C2 维持
```

关键区分（强制语句）：**独有语义能力存在 ≠ architecture owner 存在**。framing/LSN 是真实
能力，但 mission/ADR/产品均无 retained WAL 责任 → DELETE 维持。显式声明：DELETE WAL 意味着
"Core 不再声称提供 WAL"，不意味着"record framing 无用"。不因"实现规模可观"降级 RESEARCH
（§14：RESEARCH 需要"可能有价值"的证据问题，而非对沉没成本的敬意）。

### AA-3 修正（适用于 I01/I02/I03 rationale）

对抗审发现：`read_at_exact`（positional）与 `read_exact`（sequential）的 **EOF-as-error exact 组合**
在删除后将成为无 canonical 载体的能力（`await_read_fill` 是 fill-with-partial；`await_write_exact`
只覆盖 write 侧；blocking 面无 composition primitive）。§5.2.1/§5.2.3 只冻结"若存在 composition 则
语义如何"，§5.2.2/§6.1 明文允许 caller 自行组合——因此该能力**合法缺席**，不构成 DELETE 阻力，
但本审计明确将其记录为未来 ADD_MINIMAL/RESEARCH 的候选证据问题（需真实 consumer 证明需要
library 级 exact-read 组合），并要求 DELETE slice 的 review 不得声称"语义已全部 canonical 覆盖"。

### A7 §M/§N supersession 记录（I27/I28 显式化）

A7 曾将 Memory/Fault（及 Buffered/Fake）按 RESEARCH 保留，保留条件为"为测试重建提供不可替代
substrate"。本审计显式 supersede 该条件：AGENTS clean-room 纪律要求测试从当前行为重建；当前
canonical 测试零使用此类替身；Reader/Writer 删除后替身失去基类；"未来测试可能需要"属于 AGENTS
明文禁止的 hypothetical-future-user 保留。Memory 与 Fault 已拆为两个显式行（I27/I28）各自承载
该 supersession；若人审希望在 slice L 前对某行行使 RESEARCH 改判权，改判不影响其余行。

---

## 10. Group/Future 生命周期权威（Corrective-2 从代码全面重建；替代 Corrective-1 版本）

Corrective-2 裁决：**I20 Group = CONVERGE，I21 Future = DELETE**。本节把"fiber/stack storage
何时可合法销毁"的全部权威从代码逐箭头重建，替代 Corrective-1 基于"Future::ready 是当前
storage-release 见证"的错误前提（该前提撤销，依据即本节 T 链与 I20/I21 卡片改写）。

### T 链：终端/释放的八个事实（逐箭头 file:line，proof root 实测）

```text
T1  logical task terminal
      src/async/application_runtime.cpp:234-239 —— task wrapper 内 {lifecycle_mtx_;
      terminal_count_++; recompute; control_epoch_++}。仍在 fiber 栈上执行。
  ↓ （wrapper 收尾：runtime_cv_/wake_handle_ notify，set_current_fiber_tag(prev_tag)，
     application_runtime.cpp:240-243；wrapper lambda 返回）
T2  Future publication/ready
      include/sluice/async/group.hpp:150-155 —— Group fiber entry lambda 的最后语句
      fut->complete_with(Result<void>{})；future.hpp:27-42 —— ready_.store(true, release)
      （:34）+ cv notify + policy notify。仍在 fiber 栈上执行。
  ↓ （entry lambda 返回，group.hpp:155）
T3  fiber entry returned
      src/async/scheduler.cpp:25-33 —— fiber_entry_bridge 在 entry()(*fiber) 返回后续行。
  ↓
T4  Fiber::make_done
      scheduler.cpp:31 —— fiber->make_done()；src/async/fiber.cpp:41-43 ——
      state_.store(done, release)。
  ↓
T5  final context switch back to scheduler
      scheduler.cpp:33 —— context_switch_final（src/async/fiber_ctx.cpp:161-170，noreturn）；
      控制回到 worker 的 run_next_on（scheduler.cpp:731-750：running_fiber_count_--，
      ws->current=nullptr）。此后该 fiber 栈永不再次执行。
  ↓ （worker 回到 worker_loop；仅经 idle 路径观察 stop 谓词后退出；
     run_impl 在多 worker 时 join 全部 worker 线程、单 worker 内联返回 —— scheduler.cpp
     run_impl 出口，:305-309 一段/:266）
T6  worker/scheduler invocation quiescence
      application_runtime.cpp:464 —— driver 的 sched_->run_live(worker_count_, ...) 返回。
  ↓ （driver loop between_invocations → driver_exit_requested_ → exited，
     application_runtime.cpp:467-472）
T7  driver thread exited/joined
      application_runtime.cpp:347 —— join() 等 driver_state_==exited；:351-353 ——
      driver_thread_.join()。（~ApplicationRuntime :129-131 为后备 join。）
  ↓
T8  Group/fiber/stack storage released
      application_runtime.cpp:355→546-562 —— close_resources()（锁内 move 出
      root_group_/sched_/io_ctx_，锁外 reset）：group.reset()（:560）→ ~Group（group.cpp:75）
      → ready() 扫描（:78-82，逻辑 fail-fast）→ futures_/evented_fibers_/evented_stacks_
      .clear()（:84-86）。
```

关键推导（T6 ⇒ T5，对每个 admitted fiber）：`running_fiber_count_` 在 context_switch 进入
fiber 之前递增、在切换回 worker 之后递减（scheduler.cpp run_next_on），覆盖 wrapper 收尾、
发布、make_done、final switch 的整个 epilogue，期间无暂停点；而 worker loop 的每条退出
路径都被 classify_locked 以 running_fiber_count_==0 为前置门控——因此每个 fiber 的 T5
严格先于其 runner 的循环退出，从而先于 run_impl 返回（多 worker：thread join 完成），
从而先于 T7。**在 T7 之前，不存在任何仍可能执行 fiber 栈的线程。**

### FROZEN DISTINCTION

```text
FUTURE_READY ≠ FIBER_QUIESCENT
T2 发布点在 fiber entry 内部（group.hpp:154），严格先于 T3 entry 返回、T4 make_done、
T5 final switch。ready()==true 不证明 fiber 已完全返回、不证明 final switch 已发生、
不证明 fiber 栈已停止执行。
LOGICAL TERMINAL ≠ PUBLICATION ≠ FIBER RETURN ≠ SCHEDULER QUIESCENCE ≠ STORAGE RELEASE
（T1 ≠ T2 ≠ T3/T4/T5 ≠ T6 ≠ T7/T8）
```

### 权威事实表（Corrective-2 修订版）

| Fact | 内容 | Current owner | Consumer / 释放中的角色 | 可与另一 fact 合并？ |
| ---- | ---- | ------------- | ------------------------ | -------------------- |
| A | admission count（`admitted_count_`，提交侧前向计数） | ApplicationRuntime（lifecycle_mtx_ 下，submit:217） | submit 记账、terminal 谓词输入、drain | 不可与 B 合并——前向计数不能见证终止 |
| B | logical task terminal（`terminal_count_++`，T1） | ApplicationRuntime（application_runtime.cpp:236，仍在 fiber 栈上） | drain()/stop 谓词（task_set_terminal_snapshot_）、lifecycle fail-fast、(admitted,terminal) 观测（:593-594） | **仅逻辑权威**：不得用作存储释放门（INV-O3） |
| C | fiber terminal fact（entry 返回 + make_done + final switch，T3–T5） | fiber_entry_bridge + Fiber state | 由 T6 蕴含（对全部 fiber）；C 类见证若收敛后需要，可由 runtime 私有 retire 记录承载 | 不可由 B 承载；被 T6 蕴含 |
| D | Future publication/ready（T2） | Future（group.hpp:154 发布；group.cpp:79 唯一活消费） | **逻辑** fail-fast：~Group 扫描在 T7 后求值，检测"任务从未到达发布"的误用；**不门控 E** | 死路径消费者：group_stop_predicate/await；E 的物理安全性不依赖 D |
| E | stack/fiber storage release（T8） | Group（close_resources → group.reset() → ~Group clear） | **物理门 = T6→T7 teardown barrier**（INV-O1/O2），不是 D | 必须保持被 T6/T7 门控；不得改为被 B 或 D 门控 |
| F | group cancellation token ownership（`token_`/`group_token()`） | Group | request_stop（application_runtime.cpp:278）、spawned fn（token& 参数） | 可迁移（收敛后由 runtime 持有并与 spawn 合并）——与 A–E 正交 |
| G | scheduler runnable/liveness state（run 队列、park 集、run_live stop 谓词） | Scheduler | drain 驱动、runtime lifecycle | 独立执行层事实；T6 的载体 |
| H | driver thread（driver_thread_ / driver_state_） | ApplicationRuntime | T7 的载体：exited 观测 + join（:347/:351-353） | 与 G 配对构成 teardown barrier |

### close_resources 调用路径普查（T8 只在 T7 后接触非空 group）

```text
join()（唯一 tasks 可非空的路径）   :347 等 exited → :351 join driver → :355 close_resources
                                    —— T6/T7 已发生（T 链）。
start() 两失败路径 / shutdown(Constructed|StartFailed)
                                    driver 从未执行 run_live（startup-abort 提前 exited）且
                                    admission_open_ 仅在 :198 置 true、submit() 是唯一 spawn
                                    入口 → root_group_ 必空 → T8 平凡安全。
~ApplicationRuntime                 :125-127 fail-fast 除非 state ∈ {Constructed, StartFailed,
                                    Stopped}；且 close_resources 已 move 走成员——活 runtime 的
                                    成员 ~Group 不可达。
Group::await()（死公共路径，全树零调用）:39 run_live(1, group_stop_predicate) 返回后 clear。
                                    即便此路径，物理安全同样来自 worker-loop 退出（T6 形态），
                                    不来自 ready() 本身。
```

### 冻结收敛/删除不变式（Corrective-2；slice O 验收标准，取代 INV-G1/G2/G3）

```text
INV-O1  任何 fiber/stack storage 不得在 scheduler 执行对该 fiber quiesce 之前销毁。
        当前载体：run_impl 返回前 join 全部 worker 线程（T6 ⇒ 每个 fiber 的 T5），
        join() 先等 driver_state_==exited 再 driver_thread_.join()（T7），close_resources
        只在其后执行（T8）。
INV-O2  ApplicationRuntime teardown 必须保持 driver/scheduler quiescence 与 Group/task
        storage 销毁之间的可执行屏障：application_runtime.cpp:347→351-353→355 的顺序
        不可倒置、不可抽掉任一层。
INV-O3  terminal_count_ 仅是逻辑任务终端权威（T1，仍在 fiber 栈上递增）：可用于 drain/
        lifecycle 谓词与逻辑 fail-fast，不得单独授权 fiber storage 销毁。
INV-O4  Future::ready 不得被当作物理 fiber-quiescence 证明（发布点在 fiber entry 内，
        T2 严格先于 T5）；除非实现改为在最终 fiber 返回边界之后发布，否则任何收敛/删除
        方案不得引入该等价假设。
```

四条均直接由上表代码事实支撑。INV-G1/G2/G3（Corrective-1 冻结）**作废**：其"必须保留
ready 等价见证"的要求建立在被撤销的前提上；INV-G2 的核心事实（terminal_count_ 不授予
释放权威）由 INV-O3 承接。独立复核见 §14 Reviewer E（fresh lifetime adversary）。

### slice O 落点（不实现，仅边界）

```text
可移除（随收敛/删除）：Group 公共类（类型消亡）+ 线程模式 + await/cancel/size/
  group_stop_predicate + EventedAdmissionFailPoint 缝 + Future 全体成员（complete_with/
  ready/await/cancel/cancel_token/默认 ctor）+ wait-policy 死链（wait_policy.{hpp,cpp}、
  evented_wait_policy.hpp；C20：消费者仅 Future/Group）
必须保留（runtime 既有，非新增机制）：terminal_count_ 逻辑记账（INV-O3 的正确角色）、
  teardown barrier 顺序（INV-O1/O2）、scheduler quiescence（T6）
行为等价义务：spawn/token 语义不变；四个 app 消费测试全绿；新增测试见证 epilogue 窗口
  与屏障顺序（如人为拉长 wrapper 收尾路径时 teardown 不得提前释放存储）。
```

---

## 11. KEEP proof packets

无 KEEP verdict。这是证据的结果而非遗漏：唯一有存活根的候选（Group/Future）因携带死亡机制与
重复权威而不满足 §14 KEEP 的"当前 mechanism 已是足够小的实现"要件 → CONVERGE；其余全部候选
要么责任已被 canonical 收敛、由 retained app 持有（→DELETE，能力不灭），要么从未建立 owner
（→DELETE）。

---

## 12. RESEARCH questions

无 RESEARCH verdict。对全部 30 面均能在现有证据上定案。

显式记录的未来证据问题（不是本审计的 RESEARCH verdict，仅为候选问题的备案）：

```text
Q1（AA-3 派生）read 侧 exact-EOF-as-error 组合是否获得真实 consumer 证据，使 library 级
   exact-read composition 值得 ADD_MINIMAL？（当前：合法缺席）
Q2（vectored reopen，既有条件不变，机制表述按 §9 两分修正；陈旧括注的改写由
   tracking issue #370 承载）真实 consumer 是否需要
   multi-buffer canonical I/O 且能证明 scalar composition 不足——即需要 OS 单操作/非交错
   属性（pipe ≤PIPE_BUF 原子组、O_APPEND 整组追加等）作为 correctness 需求，而 scalar
   序列无法表达？reopen 语义与 parity 义务仍按 ADR-0002 §5.3。
Q3（Copy 派生，I25 C3）是否出现需要 library 级 bounded-transfer（而非 app 自实现）的真实
   consumer？若有，按 thin-branch 形态走新 ADD_MINIMAL 节点。
```

---

## 13. OUT_OF_SCOPE ownership

行级 OUT_OF_SCOPE = 0。§13 checklist（§6）中的 4 项非行级处置及其 owner：

```text
async ReadOp/WriteOp raw-fd 收敛     —— 已由 Phase A 解决（NativeFileRef{fd, declared access}）；
                                         owner：A8 conformance ledger
RequestHandle                        —— canonical §7.2 explicit-op authority，活，无 legacy 发现；
                                         owner：Phase A conformance
synthetic backend                    —— 已不在树（前序 subtraction 删除）；无对象可处置
direct-I/O/prealloc/fadvise/zero-copy/NOWAIT、append/permission/dir/rename/remove
                                     —— 树内零代码/零 Core 面；owner：ADR §10 capability
                                         backlog 与未来 ADD_MINIMAL evidence 流程
```

---

## 14. Adversarial review

五个 fresh-context 评审（互不共享结论、未被告知偏好），报告全文要点及裁定：

### Reviewer A — KEEP adversary（攻击全部 DELETE）

```text
发现 5 项，全部采纳为修正，0 项推翻 verdict：
A1 Batch 删除范围不完整（friend class Batch ×2 / reap_seq 唯一读权 / f02 seam）→ 并入 slice B 范围 ✅
A2 I05–I08 rationale 误引"A7 unchanged"（A7 实为 CONVERGE/RESEARCH 建议 + §Q1 adverse finding）
   → 改引 §5.3 + gate chain，依赖 I14 先裁 ✅
A3 附录 Memory/Fault 与 A7 §M/§N RESEARCH 判定冲突且未声明 supersession → 补 supersession 记录 ✅
A4 WAL 证据包缺 CHANGELOG/architecture.md 级联与 v0.0.1 发布事实 → 补齐 ✅
A5 P7 文件集未 pin → 本报告 §9 pin 29 文件清单 ✅
不可推翻清单（A 自己复核后认可）：I01–I04/I06–I13/I15–I18/I22–I24 全部成立；hidden truncate
（src/file.cpp:311）确认；sluice-tail 用裸 pthread 满足其唯一 offload 需求确认。
```

### Reviewer B — DELETE adversary（攻击全部 KEEP/CONVERGE 候选）

```text
发现 4 项：
B1 I20 Group KEEP 不成立（await/cancel/size 零调用、双终端跟踪、双 fail-fast）→ 采纳：改判 CONVERGE ✅
B2 Group 线程模式无处置（零实例化 + 拖拽 Future 默认 ctor/default_wait_policy 死链）→ 并入 I20
   收敛消失清单 ✅
B3 I21 Future KEEP 不成立（半数成员死亡；unmodified KEEP not defensible）→ 采纳：改判 CONVERGE ✅
B4 遗漏面：Group EventedAdmissionFailPoint 死测试缝 → 并入 I20 收敛消失清单 ✅
B 对其余 22 项 DELETE 与附录清扫未找到进一步遗漏面（附录各头零根均独立验证）。
本审计对 B1–B3 的独立复核：root_group_ 仅 5 处使用（ctor/async/token/dtor-move/测试缝），
Group() 默认 ctor 全树零调用，Future::cancel/cancel_token 全树零调用——证据成立，改判。
（Corrective-1 注：B 时代的 SCC 边界被其接受了；Corrective-1 的人审随后发现 Copy/Buffered/
Memory/Fault/SyncableWriter/stats 未被正式盘点——见 §15 HR-1。）
```

### Reviewer C — authority adversary（无视消费者数，纯边界审查）

```text
6 项 findings，0 项推翻 verdict：
AA-1 I20/I21 不触碰 File 语义（File 中立确认）——与 CONVERGE 改判兼容（CONVERGE 的依据是
    runtime 内部形状/权威重复，不是 File 边界）✅
AA-2 I05–I08 DELETE 与 §5.3 不冲突（gated≠forbidden；conformance ledger 反向支持）；条件=gate
    顺序 + §14 四要件表述 → 已采纳 ✅
AA-3 I01/I02 capability gap：exact-EOF 组合将无 canonical 载体（rationale 修正）→ §9 AA-3 修正 ✅
AA-4 I14 无任何 authority grant；SyncableWriter 无 canonical 引用 → 成立 ✅
AA-5 I15/I16 依据扩展至 §2.2/§3.1/conformance §2.1 边界 1 → 已采纳 ✅
AA-6 I22–I24 依据改为边界违反为主、弃置为辅 → 已采纳 ✅
```

### Reviewer D — public API / build adversary

```text
11 项 findings，0 项改判，全部为证据强化/措辞精确化：
74 公共头在同一 include 根无差别导出；sluice_core=13 TU（11 legacy）；batch/group/op_helpers 角色确认；
experimental 不在任何构建图（globs/compile DB/objects 三重一致）；include/ 以 {public=true} 导出
→ experimental headers 是"已发布"而非"沉睡"；编译干净/链接失败（仅 inline set_stats 可链）；
递归 glob 再武装风险真实且默认配置下静默；P1/P7 日志 22/22 复核 + P7 sluice_core 精确重建于
2 个幸存 TU；无 install/package（"shipped"="编译并导出、仅树内"）；compile_commands.json 为本地
工件无机制意义；任务文档的"ADR-0002 §20"编号不存在（burden 语言在 ADR §13/§14——本报告
已按正确编号引用）。
```

### Reviewer E — lifetime adversary（Corrective-2 §17 新增；独立于本报告结论，仅给代码与 ADR）

任务：找 fiber/stack storage 最早可合法销毁点，攻击 terminal_count_/Future::ready()/
~Group 检查/Scheduler run 完成/driver exit-join 各候选见证。结果：

```text
VERDICT = LIFETIME_AUTHORITY_PASS（无可执行反例）
最早合法释放点 = run_impl 返回（多 worker 全部 thread join 完成；单 worker 内联返回），
  经 driver_state_=exited 观测 + driver_thread_.join() 后由 close_resources 执行；
  首个可物理 free 的代码 = ~Group 的 evented_fibers_/evented_stacks_.clear()。
逐见证裁决：
  terminal_count_ → 不授权（在 fiber 栈上递增，仅记账；消费它的 drain/stop 谓词仍须
    经 run_impl 返回才触及销毁）
  Future::ready() → 不授权（发布点后 fiber 仍执行 cv notify/notify_ready/lambda 返回/
    state_ store/final switch；单凭 ready 销毁即 data race；仅经 quiescence 链传递安全）
  ~Group ready() 扫描 → 逻辑契约 fail-fast（未完成任务 fail-fast 而非 UAF）；
    非 T7 后不可达非空组的物理证明
  run_impl/run_live 完成 ⇒ 全部 worker loop 终止：两路径均 PROVEN——
    running_fiber_count_ 覆盖 epilogue 全窗（无暂停点）且门控一切退出路径
  driver join → close_resources：无竞争（close_resources 全部调用点或先 join driver
    或无 driver；start 失败/shutdown(Constructed|StartFailed) 路径 group 必空）
  Group::await()（死路径，零调用）→ 同一 scheduler 退出协议承载，非 ready 谓词
对抗性反向核查（quiescence 自身）：run_live 不能在 epilogue 中途返回；coordinated run
  结束后无任何 make_runnable/route 通路可复活 fiber（done 终态不可转出，恢复冻结
  context 将 abort）；完整 happens-before 链从 fiber 内 release store 到 ~Group acquire 读。
```

Reviewer E 的独立结论与本审计 §10 的 T1–T8 链、INV-O1..O4、I20 CONVERGE / I21 DELETE
重裁完全一致；其 running_fiber_count_ epilogue 门控推导已并入 §10 关键推导。

---

## 15. Human-review corrective-1 处置记录

人审 verdict REQUEST_CHANGES（3 MAJOR / 1 CORRECTNESS CORRECTIVE / 2 MINOR）逐项闭合位置：

```text
MAJOR-1 §13 inventory 不完整（SCC residual/collateral/slice F residue 不足以承载删除）
  → 新增显式行 I25–I30（§7）+ §6 §13 checklist + §8 traceability 列 + 无"隐式删除"类 ✅
MAJOR-2 Copy 有显式 architecture authority（§11.1 正例），不得随 Reader/Writer 连坐
  → §7 I25 卡片独立裁决（C1 责任 / C2 消费者 / C3 canonical replacement；
    RESPONSIBILITY=KEEP, LEGACY SURFACE=DELETE 显式两分）；F/U=slice M ✅
MAJOR-3 vectored/writev 事实错误（"无跨 buffer 原子性"断言撤销）
  → §9 vectored fact corrective（OS 机制属性 vs Sluice 语义权威两分）；I08 卡片改写；
    I05–I13 论证替换；vectored-decision §5 同源括注记录为后续独立 docs corrective ✅
CORRECTIVE-1 Group futures_ vs runtime terminal_count_ 非同一权威
  → §10 权威事实表 A–G + 程序序 + INV-G1/G2/G3 冻结；I20/I21 卡片与 rationale 修正；
    禁止以 terminal_count_ 替换 Future 见证 ✅
MINOR-1 缺默认构建证据
  → §1/§2 双配置基线（默认 20/20 实测、liburing=y 22/22 实测含真实 uring 路径）✅
MINOR-2 follow-up slices 过宽（29 文件巨型 slice F）
  → §17 全量重解：A–P 十六 slice，一个独立可评审权威变更 per issue，附依赖 DAG 与
     文件级枚举 ✅
```

### Human-review corrective-2 处置记录（第二轮人审 REQUEST_CHANGES：2 MAJOR / 1 MINOR）

```text
MAJOR-1（M1）planned-removal traceability 不唯一（CopyStats/BufferStats/VectorStats/UringStats
  被多行多 slice 声称删除）
  → §8.1 唯一归属 ledger：每符号恰 1 audit owner + 1 slice deletion owner；I25/I26 的
    stats 成员移出（支持性引用保留）；UringStats owner 从 slice A"顺带"改归 I30→P；
    VectorStats owner 从 P 改归 I11→G（I11 卡片 F/U 同步改 G）；三计数器全 0 ✅
MAJOR-2（M2）Group/Future 终端-生存期权威建模错误（Future::ready 发生在 fiber entry 完整
  返回之前，不能单独证明 storage 可销毁）
  → §10 从代码全面重建 T1–T8 链（逐箭头 file:line）；FUTURE_READY ≠ FIBER_QUIESCENT
    冻结；物理释放权威改判为 ApplicationRuntime teardown barrier（T6→T7）；~Group ready()
    扫描精确分类为逻辑 fail-fast；INV-G1/G2/G3 作废、INV-O1..O4 冻结；I21 Future 零基础
    重裁改判 DELETE（I20 CONVERGE 维持、理由重建）；独立 fresh lifetime adversary 复核
    （§14 Reviewer E）✅
MINOR（已知 vectored-decision 陈旧括注的追踪义务）
  → tracking issue [#370](https://github.com/jnhu76/Sluice/issues/370)（docs-only；§9
    同源记录与 §12 Q2 同步指向）✅
```

---

## 16. Proposed follow-up issue decomposition（Corrective-1 全量重写；MINOR-2）

原则：一个 issue = 一个独立可评审的架构动作（一个权威变更）。全部为建议，待人审确认后开
issue（本审计不开实现 issue）。每 slice 列出精确文件/符号集（traceability 终点）。
字母沿用人审建议粒度 A–P。

```text
slice A  DELETE experimental/ island（I22–I24）
         文件：include/sluice/experimental/{uring_io_context,uring_write_batch}.hpp、
               src/experimental/{uring_io_context,uring_write_batch}.cpp
         （Corrective-2：UringStats struct 的删除已移出本 slice、归 slice P——本 slice
          只删 4 个 island 文件；UringStats 在 A 与 P 之间成为无引用 struct，可编译。）
         证据：P1。可选加固：杜绝递归 glob 静默再武装。
slice B  DELETE Batch（I19）
         文件：include/sluice/async/batch.hpp、src/async/batch.cpp
         连带：completion.hpp friend class Batch ×2 trim；Completion::reap_seq_ + reap_seq()
               + next_reap_seq() 死链处置；tax0_f02_skip_reap_seq seam 处置（A7 §M）
         证据：P2。
slice C  DELETE op_helpers（I18）
         文件：include/sluice/async/op_helpers.hpp、src/async/op_helpers.cpp
         证据：P3。（与 B 分离：两者仅同为死叶，无共享不变式。）
slice D  DELETE BlockingIoPool（I17）
         文件：include/sluice/blocking_io_pool.hpp、include/sluice/detail/blocking_io_pool_impl.hpp、
               src/blocking_io_pool.cpp
         证据：P4。
slice E  DELETE WAL（I14）
         文件：include/sluice/wal.hpp、src/wal.cpp
         级联：CHANGELOG "[Unreleased] Removed" 条目；architecture.md WAL 提及清理
         证据：P5。人审重点复核行。
slice F  DELETE vectored operation family（I05–I10）
         位置：reader.{hpp,cpp} / writer.{hpp,cpp} 内 vec 默认实现 + iovec.hpp
               （IoSlice/ConstIoSlice 整文件删除）；file.{hpp,cpp} 的 vec 方法族随 slice H
               的文件删除消失（Corrective-2：FileReader/FileWriter override 者先删，
               本 slice 的 file 部分为空或与 H 合并执行均可，H 是硬前置，§17）
         证据：P7（文件级含于 P7 集合；方法级 trim 编译等价）。
         依赖：E（wal.cpp:105 唯一外部调用）、G（Observed* override）、H（FileReader/
               FileWriter override）先合并。
slice G  DELETE observed/vector statistics（I11–I13）
         文件：include/sluice/observed.hpp、src/observed.cpp（含 ReaderStats/WriterStats
               structs）；measurement.hpp VectorStats struct（I11 的删除 owner=本 slice，
               Corrective-2）
         证据：P7（observed 文件级）+ P8（VectorStats struct 级终态）。
         依赖：H、J 先合并（file.hpp/io_context.hpp 是 VectorStats 仅余引用者，§17）；
               本 slice 先于 I（observed.hpp include reader/writer）。
slice H  DELETE FileReader/FileWriter（I01/I02）
         文件：include/sluice/file.hpp、src/file.cpp、src/file_test_seams.hpp
         证据：P7。
slice I  DELETE Reader/Writer generic byte-stream interface（I03/I04）
         文件：include/sluice/reader.hpp、src/reader.cpp、include/sluice/writer.hpp、src/writer.cpp
         证据：P7。
slice J  DELETE IoContext/BlockingIoContext（I15/I16）
         文件：include/sluice/io_context.hpp、src/io_context.cpp
         证据：P6。
slice K  DELETE Buffered surface（I26）
         文件：include/sluice/buffer.hpp、src/buffer.cpp、include/sluice/buffered_readable.hpp
         （Corrective-2：BufferStats struct 删除归 slice P，移出本 slice 枚举。）
         证据：P7（文件级含于 P7 集合）。依赖：M 先合并（copy.cpp 是 BufferedReadable
               唯一消费者）。
slice L  DELETE Memory/Fault test substrate（I27/I28）
         文件：include/sluice/memory_io_context.hpp、include/sluice/fault.hpp、src/fault.cpp
         证据：P7。（I27/I28 两行同 slice 落地：物理文件共享；责任仍分行 traceable。）
slice M  DELETE Copy surface（I25）
         文件：include/sluice/copy.hpp、src/copy.cpp、include/sluice/copy_strategy.hpp、
               src/copy_strategy.cpp、include/sluice/limit.hpp；连带修剪 reader.{hpp,cpp} 的
               stream_to 两个 CopyLimit 重载与 copy.hpp include（编译边 C14）
         （Corrective-2：sluice::CopyStats struct 删除归 slice P，移出本 slice 枚举。）
         证据：P7。（若 slice I 先合并，则连带修剪为空。）
slice N  DELETE SyncableWriter / legacy durability interface（I29）
         文件：include/sluice/sync.hpp
         证据：P7（sync.hpp 精确在 P7 集内）。依赖：E（wal.hpp include）与 H（file.hpp 继承）
               先合并。
slice O  CONVERGE Group（I20）+ DELETE Future（I21）——单一权威变更：任务组责任收敛
         范围：spawn/token 折入 ApplicationRuntime 私有；Group 公共类随之消亡（删除线程
               模式/await/cancel/size/group_stop_predicate/EventedAdmissionFailPoint 缝）；
               Future 整体删除（I21，Corrective-2：complete_with/ready 的唯一活消费者
               即 ~Group fail-fast 扫描，随收敛消失；逻辑终端记账 terminal_count_ 与
               物理 quiescence barrier runtime 均已拥有且更强）；wait-policy 死链一并删除
               （wait_policy.{hpp,cpp}、evented_wait_policy.hpp；C20）。
               保留 teardown barrier 顺序（INV-O1/O2）与 terminal_count_ 的逻辑角色
               （INV-O3）；禁止以 ready 或 terminal_count_ 充当物理释放门（INV-O4）。
               async runtime 行为类测试随行重建（含 epilogue 窗口与屏障顺序见证测试）。
         文件：include/sluice/async/group.hpp、src/async/group.cpp、
               include/sluice/async/future.hpp、include/sluice/async/wait_policy.hpp、
               src/async/wait_policy.cpp、include/sluice/async/evented_wait_policy.hpp、
               application_runtime.{hpp,cpp} 收敛性修改
         证据：活根——行为等价 + 新增 INV-O 见证测试，非删除探针。
slice P  DELETE residual legacy measurement structs（I30）
         文件：measurement.hpp 修剪至仅 AsyncStats——删除 SyscallStats/SyncStats/
               BufferStats/sluice::CopyStats/UringStats（Corrective-2：五 struct 均为本
               slice 的删除 owner；VectorStats 不在其中，已归 slice G；终态"仅 AsyncStats"
               = G ∧ P 两 slice 合取）
         证据：P8。依赖：A（UringStats 引用者）、H+J（SyscallStats/SyncStats 引用者）、
               K（BufferStats 引用者）、M（CopyStats 引用者）先合并（§17）。
（无 slice 对应 Q1/Q2/Q3——它们是未来证据问题，非实现 issue。）
```

---

## 17. Implementation dependency DAG（Corrective-2 按 ownership 重derive；替代 Corrective-1 版本）

编译/评审依赖边（"X → Y" = X 必须先于 Y 合并）。Corrective-2 原则：边从 §8.1 的唯一
ownership 与真实引用集（§4 C19/C20 grep 实证）derive，不为表面连续性保留旧边。

```text
E(WAL) → F(vectored family)          WAL 是 write_all_vec 唯一消费者（gate chain）
E(WAL) → N(SyncableWriter)           wal.hpp include sync.hpp
H(FileReader/FileWriter) → I(Reader/Writer)      派生类先于基类删除
H → N                                file.hpp 继承 SyncableWriter
H → G(observed)                      file.hpp/file.cpp 引用 VectorStats（G 删该 struct 须先无引用者）[新]
H → P                                file.hpp 引用 SyscallStats/SyncStats                    [新显式]
J(IoContext 族) → I                  io_context.hpp include reader/writer
J → G                                io_context.hpp OpenReader/WriterOptions 引用 VectorStats [新]
J → P                                io_context.hpp 引用 SyscallStats/SyncStats               [新显式]
L(Memory/Fault) → I                  fault.hpp include reader/writer
M(Copy) → K(Buffered)                copy.cpp dynamic_cast<BufferedReadable*> 是唯一消费者
M → I                                reader.{hpp,cpp} 的 stream_to/copy.hpp 编译边（C14）
M → P                                copy.hpp/reader.hpp/reader.cpp/copy.cpp 引用 CopyStats   [新显式]
K(Buffered) → I                      buffer.hpp include reader/writer
K → P                                buffer.hpp 引用 BufferStats                              [新显式]
G(observed) → I                      observed.hpp include reader/writer                       [新]
G → F(vectored)                      ObservedReader/Writer override read_vec/write_vec，      [新]
                                     虚方法删除前 override 者必须已清（硬边）
H → F                                FileReader/FileWriter override vec 方法（同上，硬边；    [新]
                                     旧"F→H 软边"方向作废）
E → F                                wal.cpp:105 是 write_all_vec 唯一外部调用（E 边并入此）
A(experimental) → P                  UringStats 引用者清零（owner=I30→P，引用在 A 的头里）    [新显式]
{A, H, J, K, M} → P                  measurement.hpp 五 struct 修剪的完备引用清零集
（废除 F→G 旧软边：reader.cpp/writer.cpp 不引用 VectorStats（grep 实证），vec 默认实现
  不是该 struct 的引用者。）
O(Group CONVERGE + Future DELETE)    独立；仅需测试配套
B, C, D                              独立
```

slice 入边汇总（编译依赖闭环核对）：F: {E, G, H}；G: {H, J}；I: {H, J, K, L, M, G}；
N: {E, H}；P: {A, H, J, K, M}。G 与 P 互不依赖（删除集合不相交：VectorStats vs 其余五 struct）。

一个合法拓扑序（非唯一，逐 slice 全树可编译）：

```text
A, B, C, D, E, H, J, G, F, L, M, K, I, N, P   （O 任意位置；建议与测试重建同批）
```

（Corrective-2 顺序变化说明：G 从第 7 位移至 H/J 之后——旧序在 VectorStats 仍被
file.hpp/io_context.hpp 引用时删除该 struct，不可编译；F 从第 6 位移至 G/H 之后——vec 虚
方法被 FileReader/FileWriter/ObservedReader/ObservedWriter override（I05/I06 卡片 DIR 实证），
先删虚方法则 override 者不可编译，旧"F→H/F→G 软边"方向作废；旧 DAG 缺 G→I 边——
observed.hpp include reader/writer，I 之前必须清零，现已显式。）

禁止：任意字母序执行（初版隐含顺序把 29 文件并入单 slice F 的做法废弃）。
合并准绳：仅当"两者无法各自编译/评审成完整权威变更"时才并 slice——目前 A–P 无此情形。

---

## 18. Stop state

```text
#355                       OPEN（未关闭）
本报告                     docs-only，单文件（corrective-2），proof root = ff916c37
Draft PR                   OPEN（audit/legacy-surface-355）
production diff            0
implementation deletion    0（全部删除仅发生于 8 个已清理的隔离探针 worktree：
                             P1–P7 + P8；P8=P7 集 + measurement trim）
build/test 基线            默认（liburing=n）20/20 GREEN；liburing=y 22/22 GREEN
                             （均干净 configure+build 实测；含真实 liburing 路径；
                             Corrective-2 未改任何生产/测试/构建文件，基线不失效）
vectored 措辞追踪          issue #370（docs corrective；#355 关闭前须完成）
worktree                   CLEAN
状态                       READY_FOR_THIRD_HUMAN_REVIEW（corrective-2）
```

人审裁决要点建议（按风险排序）：

1. **I14 WAL DELETE**（最高风险：唯一真实独有能力的删除；§9 recheck 逐项复核 +
   "Core 不再声称提供 WAL"语义澄清；恢复途径=git）；
2. **I20 Group CONVERGE + I21 Future DELETE 的重裁与 INV-O1..O4**（涉及 async runtime
   公共面收缩、Future 类型消亡、teardown barrier 成为验收标准——§10 T 链与 §14
   Reviewer E 独立复核一并审）；
3. **I25 Copy 裁决形状**（RESPONSIBILITY=KEEP / LEGACY SURFACE=DELETE 两分是否成立）；
4. **I01–I04 公共抽象类删除**（v0.0.1 已发布的类型；§9 AA-3 修正与 A7 supersession 一并复核）；
5. **I19 Batch 删除范围**（含 canonical 头 completion.hpp 的 friend/reap_seq/f02 seam 处置）；
6. **§8.1 ledger 与 §17 DAG**（唯一归属与编译依赖序为 Corrective-2 新增机械结构，宜抽查
   两三个符号走完整链：行 → slice → 文件枚举）。
