# Explicit File Legacy Surface Audit

Proof root: `ff916c37c6bab0f9a2bc7555a19bd8a2080af639`

Authority: ADR-0002 §13 / §14

Scope: Post-Phase-A legacy I/O/composition disposition

本文不修改 ADR 权威。它只记录从当前 master 推导的 §14 disposition。
若本文与 ADR 冲突，以 ADR 为准。

Tracking: [#355](https://github.com/jnhu76/Sluice/issues/355)

---

## 1. Executive verdict

24 个盘点面（I01–I24）在 ff916c37 上逐一重derive，最终 verdict：

```text
DELETE    = 22
CONVERGE  = 2   (I20 Group / I21 Future)
KEEP      = 0
ADD_MINIMAL = 0
RESEARCH  = 0
OUT_OF_SCOPE = 0
```

一句话结论：**Phase A 收敛完成后，legacy 同步 I/O 簇不再承载任何 canonical 之外的语义责任；全树唯一活的 legacy 消费链是 `ApplicationRuntime → Group → Future`，其能力合法但形状错误，应 CONVERGE 而非 KEEP。其余 22 个表面全部满足 §14 DELETE 四要件。**

三个结构性事实支撑本结论（全部在 proof root 上机器验证）：

1. **Canonical include 图对 legacy 簇零传递依赖**。四个 app 与全部 22 个测试只 include canonical 头；唯一共享头是 `measurement.hpp`（canonical 消费者 `AsyncIoContext`/uring backend 只用 `AsyncStats`）。
2. **legacy 同步簇是一个零外部根的 SCC**（§28 模式）：`BlockingIoContext → FileReader/FileWriter → Reader/Writer → {WAL, Observed*, Buffered*, copy, fault/memory}` 相互支撑，全树（含测试）对其中任何一个的生产级调用为零。全树唯一的 vec 外部调用是 `src/wal.cpp:105 → Writer::write_all_vec`，而 WAL 自身消费者为零。
3. **七个隔离删除探针全部 22/22 GREEN**：逐个删除 experimental/、Batch、op_helpers、BlockingIoPool、WAL、IoContext 三件套，以及整个 29 文件同步簇后，canonical build + 全部测试（含 4 个 app 消费测试与 2 个 liburing 真实 uring 测试）保持全绿。

canonical promotion 问题（vectored 是否进入 canonical File surface）**不被本审计重开**：A7 裁决维持不变，reopen 条件见 vectored-decision §5。

---

## 2. Authority / method

- 权威顺序：`docs/mission.md` → ADR-0001 → ADR-0002（§2 resource root、§3 open contract、§5 canonical operations + §5.3 vectored gate + §5.5 access-legality、§6 two API levels、§8 replaceable execution、§13 遗留问题清单、§14 audit contract）。
- 审计从 ADR capability tree 出发，不从 class tree 出发（§14）。每个表面先问"它拥有什么责任"，再问"哪个文件定义它"。
- Verdict 词汇冻结为 §14 六值；`TECHNICAL_DEBT` 等不作为 verdict。
- 输入事实（继承并复核）：A7 vectored-decision 的 interim KEEP 无生存权（本审计重derive）；legacy vec 零测试覆盖；WAL 零外部消费者；#355 评论（A8 input）要求把 `experimental/` 纳入盘点（本报告 I22–I24）。
- 证据类型：符号级 census（含反 false-zero 检查：factory indirection / virtual dispatch / template / alias / 继承全部人工排除）、include 图机器验证、xmake 构建图验证、compile_commands/目标文件交叉验证、7 个隔离删除探针、git 历史为次要证据。
- 基线状态：release + `--liburing=y`，`xmake -br` 全目标构建成功，`xmake test` **22/22 passed, 0 failed**（审计开始前即绿；无既有失败）。

### 历史次要证据（不推翻现状，仅解释来源）

- `experimental/` 引入于 pre-ADR baseline（`b880f9fb`，CPPIO-CORE-013D stub）。
- `wal.cpp` / `file.cpp`（FileReader/FileWriter）来自最初 core commit `42ad240f`。
- `BlockingIoPool` 来自旧 campaign（`c83a0acd`，"promote production blocking pool"）。
- `Batch`/`Group`/`Future` 来自旧 async 研究（`cc4e33f2` 030-T4 / `758fc3b5` 029-T3）。
- `wal` 在 v0.0.1 tag 中随 "complete synchronous core" 发布（CHANGELOG.md "Unreleased" 记录义务适用于后续删除 slice）。

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
  durability   → blocking::sync_data/sync_all + await_sync_data/sync_all（canonical）| FileWriter::sync_* + SyncableWriter（重复）
  composition  → await_read_fill/await_write_exact（canonical async exact/all）
                 blocking 侧无 composition primitive（§5.2.2 允许 caller 自行组合）
                 | read_exact/read_at_exact/write_all/write_at_all/vec_all（legacy，见 §8 AA-3 修正）

EXECUTION
  blocking   → sluice::blocking（canonical，caller 线程直呼 syscall）
  evented    → ApplicationRuntime/AsyncIoContext + await_*（canonical）
  outstanding→ explicit ops + Completion + NativeFileRef（canonical）
  pool       → ThreadPoolBackend（canonical async offload）| BlockingIoPool + Task<T>（legacy 重复执行权）

COMPOSITION
  exact/all loops   → await_read_fill/await_write_exact（canonical）| op_helpers read_all/write_all/sync_*_all（重复，零消费者）
  buffering         → BufferedReader/Writer + BufferStats（零消费者，无 product owner）
  scatter/gather    → vec 方法族（evidence-gated，§5.3；唯一根 = WAL）
  grouping          → Group（任务组，活）| Batch（操作组，零消费者，ADR-0001 §6 否定的 generic layer）
  WAL/record framing→ wal 命名空间 + WalWriter（零消费者）

OBSERVABILITY
  AsyncStats（canonical，AsyncIoContext::attach_stats）
  VectorStats/SyscallStats/SyncStats/BufferStats/CopyStats/UringStats（仅 legacy/experimental 消费）

MECHANISM
  backend transport → ThreadPoolBackend / UringAsyncBackend（canonical）
  raw fd            → NativeFileRef{int, declared access}（显式命名 interop，canonical）
                       FileReader(int fd)、UringWriteBatch(int fd)（非 canonical 形态）
```

---

## 4. Consumer / build census

方法：全树符号 grep + include 图提取 + 反 false-zero 人工排除。关键事实（全部可在 proof root 复跑）：

```text
C1  四个 app 的 sluice include 集合 = {application_runtime, async_io_context, await_op_helpers,
    async/file, task_result, threadpool_backend, blocking/file, file_resource, error, result,
    detail/posix_retry}（copy 另有 detail/posix_retry；tail 另有 blocking/file）→ legacy 表面 app 消费 = 0
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
C11 Group 消费者 = ApplicationRuntime（root_group_：ctor/async/group_token/dtor/测试缝）；
    await()/cancel()/size()/group_stop_predicate 全树调用者 = 0；线程模式（Group() 默认 ctor）实例化 = 0
C12 Future 消费者 = Group（complete_with/ready 活；await 仅死线程路径；cancel/cancel_token 调用者 = 0）
C13 experimental/：include 者仅自身 .cpp；无任何构建目标编译 src/experimental/*.cpp
    （compile_commands.json 与 build/.objs 双重验证）；headers 在 {public=true} include 路径上被导出
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

false-zero 排除记录：Reader/Writer 的 virtual dispatch 只能经 `Reader&/Writer&` 到达（唯一入口 WAL/copy/Buffered/Observed，全死）；模板只有 `Group::async<Fn>`（活，经 ApplicationRuntime）与 `Task<T>`/`try_submit`（零调用）；factory 只有 `IoContext::open_*`（零调用点）；无 type-alias/宏分发路径。

---

## 5. Dependency graph（真实代码边）

```text
[SYNC SCC — 根消费者：无]
  BlockingIoContext ──manufacture──> FileReader / FileWriter
  FileReader/FileWriter ──inherit──> Reader / Writer ──类型──> IoSlice / ConstIoSlice
  FileReader/FileWriter ──fields──> SyscallStats / VectorStats / SyncStats*
  WalWriter / wal:: ──> Writer::write_all_vec / write_all / Reader::read_exact；SyncableWriter
  ObservedReader/Writer ──wrap──> Reader/Writer + VectorStats
  BufferedReader/Writer ──wrap──> Reader/Writer + BufferedReadable + BufferStats
  copy_all / Reader::stream_to ──> Reader/Writer + CopyLimit + CopyStrategy + CopyStats
  MemoryIoContext ──> IoContext + MemoryReader/MemoryWriter (fault.hpp)
  FaultReader/FaultWriter ──> Reader/Writer
  BlockingIoPool（孤立：Task<T>/PoolStats）

[ASYNC]
  Batch ──> AsyncIoContext::submit_*（孤立，零消费者；持 Completion 私有 reap_seq 唯一读权 + friend 授权）
  op_helpers(read_all/...) ──> AsyncIoContext + NativeFileRef（孤立，零消费者）
  Group ──> Future / Fiber / CancelToken / Scheduler ──被──> ApplicationRuntime ──被──> 4 apps
  measurement.hpp：AsyncStats（canonical 边）｜其余 stats structs（仅 SYNC SCC / experimental 边）

[ISLAND — 未编译]
  UringIoContext(path) ──> UringWriteBatch(int fd) ──> liburing / raw syscall
```

分类：ROOT RESPONSIBILITY = Group/Future（经 ApplicationRuntime）；DEAD LEAF = 其余全部；无 SUPPORTING-MECHANISM-only 存活者（没有死节点仅因指向另一死节点而存活的反向依赖——SCC 内部互指但外部根为零，见 §28 判定）。

---

## 6. Per-surface audit cards

字段缩写：LOC=位置，PUB=public header？，BLD=是否被编译进库，DIR/TRANS=直接/传递消费者，TEST=测试见证，APP=app 消费，RESP=拥有责任，CANON=canonical 等价物，UNIQ=独有语义，AUTH=resource/correctness 权威，COST=机制成本，IMPACT=删除影响，F/U=follow-up。

### I01 FileReader

```text
LOC=include/sluice/file.hpp + src/file.cpp   PUB=Y   BLD=Y（sluice_core）   DIR=BlockingIoContext::open_reader
TRANS=无（factory 零调用点）   TEST=0   APP=0
RESP=path/fd → fd 资源身份 + sequential/positional read + read_at_exact + vec（I07）
CANON=File::open(read_only 三轴) + blocking::read/read_at + await_read_at
UNIQ=无。short-I/O/EOF 语义与 §5.2 一致但 canonical 已同语义覆盖；read_at_exact 见 §8 AA-3 修正
AUTH=无（§2.2 明文：resource-identity 类型无默认生存权）；int fd ctor 绕过 access-legality 权威
COST=与 canonical 平行的第二套 open/close/read 语义维护面；隐藏 open 语义（O_RDONLY|O_CLOEXEC）
IMPACT=P7 GREEN；canonical contract 无损
VERDICT=DELETE
RATIONALE=§14 四要件全满足：无 owner（canonical File 是 root）；无独有 correctness 语义；无产品/测试/构建角色
（编译进库但零消费者）；删除保持 retained contracts（P7）。能力与身份分离裁决：能力已在 canonical，
身份（class 本身）即 §2.2 否定的对象——Phase A 已完成"能力收敛"，本面不存在需要 CONVERGE 搬运的残余责任。
F/U=NARROW DELETE ISSUE（slice F）
```

### I02 FileWriter

```text
LOC=include/sluice/file.hpp + src/file.cpp   PUB=Y   BLD=Y   DIR=BlockingIoContext::open_writer
TRANS=无   TEST=0   APP=0
RESP=fd 资源身份 + sequential/positional write + write_at_all + sync_data/sync_all + vec（I08）
CANON=File::open(三轴) + blocking::write/write_at/sync_data/sync_all + await_write_at/await_sync_*
UNIQ=无。注意：ctor 硬编码 O_WRONLY|O_CREAT|O_TRUNC|0644 = §3.1 明文禁止的"truncate 隐藏在打开 writer
默认构造语义"；canonical File::open 把三轴显式化，是修复而非等价替换
AUTH=无（同 I01）；truncate 语义被隐藏本身就是违反 §3.1 的权威问题
COST=第二套 durability/durability-stats 维护面；flush() 为空实现
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I01（+§3.1 违反加重）   F/U=NARROW DELETE ISSUE（slice F）
```

### I03 Reader（抽象接口）

```text
LOC=include/sluice/reader.hpp + src/reader.cpp   PUB=Y   BLD=Y   DIR=WAL/Observed/Buffered/copy/FileReader（全在 SCC 内）
TRANS=无外部   TEST=0   APP=0
RESP=polymorphic byte-stream read 抽象 + read_exact/stream_to/copy 组合 + read_vec/read_vec_all 默认实现
CANON=blocking::read 原语（caller 组合）+ await_read_fill/await_write_exact（async exact/all）
UNIQ=§8 AA-3 修正：read_exact 的 EOF-as-error 组合语义在删除后将无 canonical 载体
（await_read_fill 是 fill-with-partial）；§5.2.1/§5.2.3 只定义"若存在 composition 的语义"，不强制存在；
§5.2.2/§6.1 明文允许 caller 自行组合 → 该能力合法缺席，不构成 DELETE 阻力，但必须记录
AUTH=无（非 File 资源权威；无 correctness invariant）
COST=虚接口 + 全部实现者（FileReader/Observed/Buffered/Memory/Fault）都是死 SCC 成员
IMPACT=P7 GREEN；实现者集合随本 DELETE 一并消失
VERDICT=DELETE
RATIONALE=§13 问题"是否是伪装的 File 抽象/通用组合层"的裁决：generic byte-stream owner 未被证明——
非文件实现者全部是零消费者测试替身，组合责任已有 canonical owner，虚接口形式无根。四要件满足。
F/U=NARROW DELETE ISSUE（slice F；若未来真实 consumer 需要多态流，走新 ADD_MINIMAL 节点）
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
VERDICT=DELETE   RATIONALE=同 I03   F/U=NARROW DELETE ISSUE（slice F）
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
"先裁 WAL 再裁消费链"；依据是 §14 四要件而非裸零消费者。
F/U=NARROW DELETE ISSUE（slice E，在 WAL slice 之后）
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
VERDICT=DELETE   RATIONALE=同 I05（gate chain 依赖已满足）   F/U=NARROW DELETE ISSUE（slice E）
```

### I07 FileReader vectored 方法（read_vec → ::readv / read_vec_at → ::preadv）

```text
LOC=include/sluice/file.hpp:50-54 + src/file.cpp:114-285   PUB=Y   BLD=Y   DIR=虚经 Reader*（无外部根）
TEST=0   APP=0   RESP=真 syscall scatter/gather read（IOV_MAX 分块、跨 buffer 推进）
CANON=无   UNIQ=多 buffer 单 syscall（syscall-count 机制，非语义权威——ADR-0001 §3）
AUTH=无   COST=readv/preadv/iov_max 分块机器 ~170 行；零测试覆盖
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I05 + read_vec_at 全树零调用   F/U=NARROW DELETE ISSUE（slice E）
```

### I08 FileWriter vectored 方法（write_vec → ::writev / write_vec_at → ::pwritev）

```text
LOC=include/sluice/file.hpp:103-107 + src/file.cpp:357-533   PUB=Y   BLD=Y   DIR=虚经 Writer*（唯一根 WAL，死）
TEST=0   APP=0   RESP=真 syscall scatter/gather write
CANON=无   UNIQ=O_APPEND/pipe 原子性在该实现中不存在（常规 fd 上的 writev 不提供跨 buffer 原子性承诺；
vectored-decision §5 reopen 条件所要求的 contiguity 语义并未被本实现建立）
AUTH=无   COST=同 I07
IMPACT=P7 GREEN
VERDICT=DELETE   RATIONALE=同 I05   F/U=NARROW DELETE ISSUE（slice E）
```

### I09 IoSlice

```text
LOC=include/sluice/iovec.hpp   PUB=Y   BLD=Y（header）   DIR=仅 vec 方法签名 + wal.cpp
TEST=0   APP=0   RESP=可写 span 包装（IoSlice{span<byte>}）
CANON=无（canonical 全 scalar，直接用 span）   UNIQ=无（纯别名级包装）
AUTH=无   COST=一个公共类型名 + 语义歧义（与 std::span 并存）
IMPACT=随 vec 删除   VERDICT=DELETE
RATIONALE=唯一消费者是 I05–I08；无独立责任   F/U=NARROW DELETE ISSUE（slice E）
```

### I10 ConstIoSlice

```text
LOC=include/sluice/iovec.hpp   PUB=Y   BLD=Y   DIR=同 I09
TEST=0   APP=0   RESP=只读 span 包装   CANON=无   UNIQ=无   AUTH=无
IMPACT=随 vec 删除   VERDICT=DELETE   RATIONALE=同 I09   F/U=NARROW DELETE ISSUE（slice E）
```

### I11 VectorStats

```text
LOC=include/sluice/measurement.hpp:72-81   PUB=Y   BLD=Y   DIR=file.hpp 成员 + OpenReader/WriterOptions + Observed*（全死）
TEST=0   APP=0   RESP=vec 调用/字节/iovec/fallback 计数
CANON=AsyncStats（canonical 观测权威：AsyncIoContext::attach_stats）
UNIQ=无（纯 legacy vec 计数）
AUTH=无（hint/observation，非 authority）   COST=8 个字段 + 全部包装管线的存在理由
IMPACT=P7 GREEN（measurement.hpp 本体保留——AsyncStats 是 canonical 消费者）
VERDICT=DELETE
RATIONALE=观测责任已由 canonical AsyncStats 拥有；本 struct 只服务死 vec 管线；"statistics sound useful"
不构成 KEEP 证据（任务 §16）。删除指 struct 字段，不指 measurement.hpp 文件。
F/U=NARROW DELETE ISSUE（slice E；文件级 trim 并入 slice F 的 measurement 收敛）
```

### I12 ObservedReader

```text
LOC=include/sluice/observed.hpp + src/observed.cpp   PUB=Y   BLD=Y   DIR=0（全树含测试零消费者）
RESP=Reader 包装 + ReaderStats/VectorStats 计数   CANON=AsyncStats 管线（观测已 canonical 化）
UNIQ=无   AUTH=无   COST=公共包装类型 + 观测面（且从未被用过）
IMPACT=P7 GREEN   VERDICT=DELETE
RATIONALE=零消费者 + 责任已由 canonical 观测面承担；包装层级本身是 §16 问句"measurement 是否值得
一整个 wrapper hierarchy"的否定答案。   F/U=NARROW DELETE ISSUE（slice E）
```

### I13 ObservedWriter

```text
LOC=include/sluice/observed.hpp + src/observed.cpp   PUB=Y   BLD=Y   DIR=0   TEST=0   APP=0
RESP=Writer 包装 + WriterStats/VectorStats 计数   CANON=同 I12   UNIQ=无   AUTH=无
IMPACT=P7 GREEN   VERDICT=DELETE   RATIONALE=同 I12   F/U=NARROW DELETE ISSUE（slice E）
```

### I14 WAL（wal:: 自由函数 + WalWriter + record surface）

```text
LOC=include/sluice/wal.hpp + src/wal.cpp + include/sluice/sync.hpp（SyncableWriter 持久钩子）
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
（不构成"durable correctness asset"论证）
IMPACT=P5 GREEN（22/22）；唯一 vec 消费链随之消亡（I05–I08 的 gate chain 根）
VERDICT=DELETE
RATIONALE=§14 四要件：D1 无 owner（§13 open question 由本审计裁决为非 Core）；D2 独有语义存在但无
依赖它的 retained correctness contract；D3 无资源权威（Writer 装饰器）；D4/D6 零产品/测试角色；
D5 仅"被编译"不是合法构建角色；D7 公共头 + v0.0.1 发布事实记录为删除 slice 的 CHANGELOG/文档级联
义务而非保留理由；D8 retained contracts 不受影响（P5）；D9 无隐性 ADD_MINIMAL（scalar write_all 路径
仍存在于 Writer，且 Writer 本身也 DELETE——framing 若未来需要，按当时 evidence 重建）；D10 探针 GREEN。
本表最高风险 DELETE，已单独标注供人审重点复核；git 历史可完整恢复。
F/U=NARROW DELETE ISSUE（slice D，含 CHANGELOG "Removed" 条目 + architecture.md WAL 提及清理）
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
F/U=NARROW DELETE ISSUE（slice F）
```

### I16 BlockingIoContext

```text
LOC=include/sluice/io_context.hpp:35-42 + src/io_context.cpp   PUB=Y   BLD=Y   DIR=0
RESP=Blocking execution 的对象 factory：manufacture FileReader/FileWriter（隐藏 O_TRUNC 的 open_writer）
CANON=blocking:: 自由函数族（first-class blocking，ADR §8.1）
UNIQ=无   AUTH=负权威（同 I15 四条 + §3.1 hidden truncate 具体违反）
COST=32 行 + 错误映射分支   IMPACT=P6 GREEN
VERDICT=DELETE   RATIONALE=同 I15   F/U=NARROW DELETE ISSUE（slice F）
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
+ 四要件满足。   F/U=NARROW DELETE ISSUE（slice C）
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
四要件满足。   F/U=NARROW DELETE ISSUE（slice B，与 Batch 同一评审切片）
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
IMPACT=P2 GREEN；注：批量删除 slice 必须同步 trim completion.hpp 两处 friend 声明并对 reap_seq_/
claim 死路径/f02 seam 作出处置（A7 §M 已枚举）——friend-of-undefined-class 合法 C++，故 P2 绿不代表
耦合不存在
VERDICT=DELETE
RATIONALE=零消费者 + 零 authority（机制只组织调用）+ ADR-0001 §6 否定其 generic 层资格。四要件满足。
F/U=NARROW DELETE ISSUE（slice B，含 friend/reap_seq/f02 seam 清理范围）
```

### I20 Group

```text
LOC=include/sluice/async/group.hpp + src/async/group.cpp   PUB=Y   BLD=Y（sluice_async）
DIR=ApplicationRuntime（root_group_：ctor:114 / async:224 / group_token:278 / dtor:555；测试缝:615）
TRANS=四个 app（经 ApplicationRuntime）   TEST=0（直接）   APP=Y（传递，活）
RESP=任务组：evented spawn（fiber+stack+Future）、组级 CancelToken、终端检查
CANON=ApplicationRuntime 本身（runtime 已有 admitted/terminal 记账 + drain + lifecycle fail-fast）
UNIQ=spawn/token 组合（活）；await()/cancel()/size()/group_stop_predicate/线程模式（全零调用者）
AUTH=任务集生命周期（ADR §9 async correctness 域）；不触碰 File 语义（对抗审 AA-1 确认无 File 权威）
COST=203 行中约半数死亡：双执行模式（threaded 零实例化）、双终端跟踪（futures_ 扫描 vs runtime 计数）、
双 fail-fast（group dtor vs runtime lifecycle）、EventedAdmissionFailPoint 测试缝（除 runtime 内联链外
零测试调用者）
IMPACT=不可直接删除（活根）；P7 未触碰
VERDICT=CONVERGE
RATIONALE=§14 CONVERGE 定义逐条命中：能力正确（spawn/token 被唯一合法根消费），但存在 semantic
duplication（双终端跟踪）、authority duplication（双 fail-fast）、wrong layer（公共类 + 死执行模式 +
死公共方法）。消费者数不决定本判定——死亡成员的存在决定。
F/U=NARROW CONVERGENCE ISSUE（slice G）
```

### I21 Future

```text
LOC=include/sluice/async/future.hpp   PUB=Y   BLD=Y   DIR=Group（唯一消费者）
TRANS=四个 app（经 Group/ApplicationRuntime）   TEST=0   APP=Y（传递）
RESP=caller-completable 结果单元：complete_with/ready（活）+ policy notify（唤醒边）；
await/cancel/cancel_token/默认 ctor（死：await 仅线程路径，cancel 族零调用者，默认 ctor 仅线程路径）
CANON=Completion 是 backend-publication 权威（publish_from_reap 私有）；Future 是 caller-publication
原语——两者语义不同，不重复（对抗审确认）
UNIQ=树中唯一 caller-completable cell（活）；死亡成员见上
AUTH=任务终端事实的 publication（§9 域）；File 中立
COST=模板头，但半数成员死亡 + 默认 ctor 拖拽 default_wait_policy 死链
IMPACT=随 Group 收敛收缩   VERDICT=CONVERGE
RATIONALE=同 I20：能力活、形状错（死成员 + 经收敛后应成为 runtime 内部信号而非公共 API）。
对抗审 B 的"unmodified KEEP not defensible"成立。   F/U=NARROW CONVERGENCE ISSUE（slice G）
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
F/U=NARROW DELETE ISSUE（slice A，最高置信度）
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
IMPACT=P1 GREEN   VERDICT=DELETE   RATIONALE=同 I22   F/U=NARROW DELETE ISSUE（slice A）
```

### I24 src/experimental/*（uring_io_context.cpp + uring_write_batch.cpp）

```text
LOC=src/experimental/   PUB=N（.cpp）   BLD=N（xmake globs 为单层 src/*.cpp 与 src/async/*.cpp；
compile_commands/.objs 双验证零条目）   DIR=0   TEST=0   APP=0
RESP=I22/I23 的实现（含 retry_uring_wait_on_eintr 调用——该 helper 本体在生产 uring_backend 侧，删除
本目录不影响生产）
CANON=无   UNIQ=无   AUTH=无   COST=零构建成本；风险在"被递归 glob 意外激活"（A8 评论），
且激活在默认配置下是静默的（对抗审 D 证实）
IMPACT=P1 GREEN   VERDICT=DELETE   RATIONALE=I22/I23 的实现体；同 slice 删除。
F/U=NARROW DELETE ISSUE（slice A）
```

---

## 7. Final verdict table

| ID | Surface | Root consumer/role | Unique authority/semantics | Verdict | Follow-up |
| -- | ------- | ------------------ | -------------------------- | ------- | --------- |
| I01 | FileReader | none（仅死 factory 引用） | none（read_at_exact 见 §8 AA-3） | DELETE | NARROW DELETE ISSUE |
| I02 | FileWriter | none | none（隐藏 truncate = §3.1 违反） | DELETE | NARROW DELETE ISSUE |
| I03 | Reader | none（SCC 内部互指） | read_exact EOF-as-error（唯一载体，合法缺席可记录） | DELETE | NARROW DELETE ISSUE |
| I04 | Writer | none（SCC 内部互指） | none | DELETE | NARROW DELETE ISSUE |
| I05 | Reader vec defaults | none | cross-buffer advancement（无存活依赖） | DELETE | NARROW DELETE ISSUE |
| I06 | Writer vec defaults | WAL（已裁 DELETE） | scatter 组合载体 | DELETE | NARROW DELETE ISSUE |
| I07 | FileReader read_vec/read_vec_at | none | 单 syscall 机制（非 authority） | DELETE | NARROW DELETE ISSUE |
| I08 | FileWriter write_vec/write_vec_at | WAL（已裁 DELETE） | 单 syscall 机制（非 authority） | DELETE | NARROW DELETE ISSUE |
| I09 | IoSlice | vec 方法族（死） | none | DELETE | NARROW DELETE ISSUE |
| I10 | ConstIoSlice | vec 方法族（死） | none | DELETE | NARROW DELETE ISSUE |
| I11 | VectorStats | legacy vec 管线（死） | none（canonical=AsyncStats） | DELETE | NARROW DELETE ISSUE |
| I12 | ObservedReader | none（全树零） | none | DELETE | NARROW DELETE ISSUE |
| I13 | ObservedWriter | none（全树零） | none | DELETE | NARROW DELETE ISSUE |
| I14 | WAL | none（全树含测试零） | record framing/LSN（无依赖 contract） | DELETE | NARROW DELETE ISSUE |
| I15 | IoContext | none（调用点=0） | none（capability erasure 实证） | DELETE | NARROW DELETE ISSUE |
| I16 | BlockingIoContext | none | none（隐藏 O_TRUNC） | DELETE | NARROW DELETE ISSUE |
| I17 | BlockingIoPool | none（全树零） | 通用有界池形状（与 ThreadPool 重复） | DELETE | NARROW DELETE ISSUE |
| I18 | op_helpers | none（全树零） | none（await_op_helpers 已收敛该责任） | DELETE | NARROW DELETE ISSUE |
| I19 | Batch | none（全树零） | none（无 group-admission authority） | DELETE | NARROW DELETE ISSUE |
| I20 | Group | ApplicationRuntime → 4 apps | spawn/token（活）+ 半数死亡成员 | CONVERGE | NARROW CONVERGENCE ISSUE |
| I21 | Future | Group（唯一） | caller-publication cell（活）+ 死成员 | CONVERGE | NARROW CONVERGENCE ISSUE |
| I22 | experimental/uring_io_context.hpp | none（未编译不可链接） | none（raw path 权威 = 边界违反） | DELETE | NARROW DELETE ISSUE |
| I23 | experimental/uring_write_batch.hpp | none（未编译不可链接） | none（raw fd + 已否定 batch 机制） | DELETE | NARROW DELETE ISSUE |
| I24 | src/experimental/* | none（不在构建图） | none | DELETE | NARROW DELETE ISSUE |

每行恰好一个 verdict；无缺行；无重复行。计数：DELETE=22，CONVERGE=2，KEEP=0，ADD_MINIMAL=0，RESEARCH=0，OUT_OF_SCOPE=0。

---

## 8. DELETE proof packets

每个 DELETE 按 §22 D1–D10 记录。共同事实（适用于全部）：canonical 头/app/test 对删除集合零依赖（§4 C1–C3）；探针基线 release+liburing=y 全 22 测试。

### 通用证据基座

- **D8/D10（契约保持 + 探针）**：P1–P7 七个隔离 worktree（均基于 ff916c37、detach、未提交、已清理）删除候选后 `xmake -br` 全目标构建成功、`xmake test` **22/22 passed**（含 4 个 app 消费测试、uring smoke/probe 真实 liburing 路径）。
- **D4/D6（产品/测试角色）**：§4 C1/C2 census；对抗审独立复核确认 app/test include 集合精确为 14 个 canonical 头。
- **D9（无隐性 ADD_MINIMAL）**：全部删除面的能力均有 canonical 载体或经 §14 合法缺席（唯二例外已显式记录：read_exact EOF 组合见 AA-3；WAL framing 见 I14 D9）。

### P7 精确 29 文件删除集（pin，供人审与未来 slice 复用）

```text
headers (17): include/sluice/{file,reader,writer,iovec,observed,wal,sync,memory_io_context,fault,
              buffer,buffered_readable,copy,copy_strategy,limit,io_context,blocking_io_pool}.hpp
              + include/sluice/detail/blocking_io_pool_impl.hpp
srcs (12):    src/{file,reader,writer,observed,wal,buffer,copy,copy_strategy,fault,io_context,
              blocking_io_pool}.cpp + src/file_test_seams.hpp
结果：sluice_core 仅余 blocking_file.cpp + file_resource.cpp 编译；22/22 GREEN
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
  consumer 证据仍可开新 narrow node）；D7=零测试覆盖输入事实记录在案。
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
```

### AA-3 修正（适用于 I01/I02/I03 rationale）

对抗审发现：`read_at_exact`（positional）与 `read_exact`（sequential）的 **EOF-as-error exact 组合**
在删除后将成为无 canonical 载体的能力（`await_read_fill` 是 fill-with-partial；`await_write_exact`
只覆盖 write 侧；blocking 面无 composition primitive）。§5.2.1/§5.2.3 只冻结"若存在 composition 则
语义如何"，§5.2.2/§6.1 明文允许 caller 自行组合——因此该能力**合法缺席**，不构成 DELETE 阻力，
但本审计明确将其记录为未来 ADD_MINIMAL/RESEARCH 的候选证据问题（需真实 consumer 证明需要
library 级 exact-read 组合），并要求 DELETE slice 的 review 不得声称"语义已全部 canonical 覆盖"。

### A7 §M/§N supersession 记录（适用于附录 Memory/Fault 行）

A7 曾将 Memory/Fault（及 Buffered/Fake）按 RESEARCH 保留，保留条件为"为测试重建提供不可替代
substrate"。本审计显式 supersede 该条件：AGENTS clean-room 纪律要求测试从当前行为重建；当前
canonical 测试零使用此类替身；Reader/Writer 删除后替身失去基类；"未来测试可能需要"属于 AGENTS
明文禁止的 hypothetical-future-user 保留。若人审希望保留测试 substrate 选项，可在 slice F 前把
Memory/Fault 降级为独立 RESEARCH 节点——该改判不影响的其余行。

---

## 9. CONVERGE proof packets

### I20 Group

```text
存续责任：evented spawn（fiber/stack 所有权 + 事务性 admission）+ 组级 CancelToken + 终端 ready 检查
现有形状错在：
  (a) 双执行模式——threaded 模式（Group() 默认 ctor、async_threaded、tasks_、join 路径）全树零实例化；
  (b) 双终端跟踪——futures_ 扫描 vs ApplicationRuntime 的 admitted/terminal 计数 + drain；
  (c) 双 fail-fast——group dtor 的 group_lifetime_fail_fast vs runtime lifecycle fail-fast；
  (d) 死公共方法——await()/cancel()/size()/group_stop_predicate 全树零调用者；
  (e) 死测试缝——EventedAdmissionFailPoint（除 runtime 内联链外零测试调用者）。
canonical owner：ApplicationRuntime 内部（私有 spawn/token 细节）
收敛后消失：group.hpp/future.hpp 的公共导出、线程模式、await/cancel/size/predicate、重复 fail-fast、
  测试缝
```

### I21 Future

```text
存续责任：caller-completable 结果单元（complete_with + ready + policy notify 唤醒边）——Group dtor
  终端检查与 runtime 驱动依赖它
现有形状错在：await()（仅死线程路径调用）、cancel()/cancel_token()（零调用者）、默认 ctor +
  default_wait_policy 死链
canonical owner：ApplicationRuntime 内部信号（收敛后不再是公共 API）
收敛后消失：上述死成员 + 公共导出
与 Completion 的边界（对抗审确认）：Completion=backend publication（publish_from_reap 私有），
Future=caller publication——不重复，均存活于各自层
```

---

## 10. KEEP proof packets

无 KEEP verdict。这是证据的结果而非遗漏：唯一有存活根的候选（Group/Future）因携带死亡机制与
重复权威而不满足 §14 KEEP 的"当前 mechanism 已是足够小的实现"要件 → CONVERGE；其余全部候选
要么责任已被 canonical 收敛（→DELETE），要么从未建立 owner（→DELETE）。

---

## 11. RESEARCH questions

无 RESEARCH verdict。任务 §25 要求可执行证据问题；对全部 24 面均能在现有证据上定案。

显式记录的未来证据问题（不是本审计的 RESEARCH verdict，仅为候选问题的备案）：

```text
Q1（AA-3 派生）read 侧 exact-EOF-as-error 组合是否获得真实 consumer 证据，使 library 级
   exact-read composition 值得 ADD_MINIMAL？（当前：合法缺席）
Q2（vectored reopen，既有条件不变）真实 consumer 是否需要 multi-buffer canonical I/O 且能证明
   scalar composition 不足（interleaving/contiguity correctness，而非 syscall 计数/性能）？
   reopen 语义与 parity 义务仍按 ADR-0002 §5.3。
```

---

## 12. OUT_OF_SCOPE ownership

无 OUT_OF_SCOPE verdict。盘点面全部属于本审计的 retained I/O/composition 决策范围。

---

## 13. Adversarial review

四个 fresh-context 评审（互不共享结论、未被告知偏好），报告全文要点及裁定：

### Reviewer A — KEEP adversary（攻击全部 DELETE）

```text
发现 5 项，全部采纳为修正，0 项推翻 verdict：
A1 Batch 删除范围不完整（friend class Batch ×2 / reap_seq 唯一读权 / f02 seam）→ 并入 slice B 范围 ✅
A2 I05–I08 rationale 误引"A7 unchanged"（A7 实为 CONVERGE/RESEARCH 建议 + §Q1 adverse finding）
   → 改引 §5.3 + gate chain，依赖 I14 先裁 ✅
A3 附录 Memory/Fault 与 A7 §M/§N RESEARCH 判定冲突且未声明 supersession → 补 supersession 记录 ✅
A4 WAL 证据包缺 CHANGELOG/architecture.md 级联与 v0.0.1 发布事实 → 补齐 ✅
A5 P7 文件集未 pin → 本报告 §8 pin 29 文件清单 ✅
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
```

### Reviewer C — authority adversary（无视消费者数，纯边界审查）

```text
6 项 findings，0 项推翻 verdict：
AA-1 I20/I21 不触碰 File 语义（File 中立确认）——与 CONVERGE 改判兼容（CONVERGE 的依据是
    runtime 内部形状/权威重复，不是 File 边界）✅
AA-2 I05–I08 DELETE 与 §5.3 不冲突（gated≠forbidden；conformance ledger 反向支持）；条件=gate
    顺序 + §14 四要件表述 → 已采纳 ✅
AA-3 I01/I02 capability gap：exact-EOF 组合将无 canonical 载体（rationale 修正）→ §8 AA-3 修正 ✅
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
工件无机制意义；任务文档的"ADR-0002 §20"编号不存在（ burden 语言在 ADR §13/§14——本报告
已按正确编号引用）。
```

### Reconcile 结论

- 无 majority-vote；每项分歧按 claim/counterclaim/evidence/adjudication 处理（上文 ✅ 项）。
- 最终相对初版的唯一 verdict 变更：**I20/I21 KEEP → CONVERGE**（B 的证据 + 本审计独立复核）。
- 全部 rationale 修正（A1–A5, AA-2/3/5/6）已并入本报告正文与 proof packets。

---

## 14. Proposed follow-up issue decomposition

原则：一个 issue = 一个独立可评审的架构动作；全部为建议，待人审确认后开 issue（本审计不开实现 issue）。

```text
slice A  DELETE experimental/ island（I22–I24；4 文件；零构建影响；P1 已证）
         —— 顺带在 libraries.lua 注释或 guards 上杜绝静默再武装（可选加固）
slice B  DELETE async 死叶：Batch（I19，含 completion.hpp friend×2 trim + reap_seq_/claim 死路径 +
         f02_skip_reap_seq seam 处置）+ op_helpers（I18）
slice C  DELETE BlockingIoPool（I17：hpp + detail impl hpp + cpp；P4 已证）
slice D  DELETE WAL（I14）+ CHANGELOG "[Unreleased] Removed" + architecture.md WAL 提及清理
         —— 人审重点复核行
slice E  DELETE vectored 链（I05–I13：vec 方法族 + IoSlice/ConstIoSlice + VectorStats + Observed*；
         依赖 slice D 已合并）+ measurement.hpp legacy-only structs trim（SyscallStats/SyncStats/
         BufferStats/CopyStats；UringStats 随 slice A）
slice F  DELETE Reader/Writer/FileReader/FileWriter/IoContext/BlockingIoContext（I01–I04/I15/I16）
         + SCC 残余附录（sync.hpp、buffer/buffered_readable、copy/copy_strategy/limit、fault/memory、
         MemoryIoContext）+ measurement.hpp 收敛完成（只余 AsyncStats）
         —— 最大公共面删除；A7 §M/§N supersession 已在 §8 记录，人审可在此 slice 前对 Memory/Fault
         行使 RESEARCH 改判权
slice G  CONVERGE Group/Future（I20/I21）：spawn/token 折入 ApplicationRuntime 私有；删除线程模式/
         await/cancel/size/predicate/EventedAdmissionFailPoint 缝/Future 死成员与默认 ctor 链；
         async runtime 行为类测试需随行重建见证
（无 slice 对应 Q1/Q2——它们是未来证据问题，非实现 issue）
```

依赖顺序：A/B/C 独立；E 依赖 D；F 建议 在 E 后（最小化单 slice 面积）；G 独立但需要测试配套。

---

## 15. Stop state

```text
#355                       OPEN（未关闭）
本报告                     docs-only，单文件，proof root = ff916c37
Draft PR                   OPEN（audit/legacy-surface-355）
production diff            0
implementation deletion    0（全部删除仅发生于 7 个已清理的隔离探针 worktree）
build/test 基线            22/22 GREEN（release + liburing=y）
worktree                   CLEAN
状态                       READY_FOR_HUMAN_REVIEW
```

人审裁决要点建议（按风险排序）：

1. **I14 WAL DELETE**（最高风险：唯一真实独有能力的删除；恢复途径=git）；
2. **I20/I21 CONVERGE 方向**（涉及 async runtime 公共面收缩 + 测试配套）；
3. **I01–I04 公共抽象类删除**（v0.0.1 已发布的类型；§8 AA-3 修正与 A7 supersession 请一并复核）；
4. **I19 Batch 删除范围**（含 canonical 头 completion.hpp 的 friend trim）。
