# DAG 段记录：后端分发 → SQE 分配/准备 → cookie/TransportLedger → liburing → io_uring_enter → 内核 io_uring → VFS/PageCache

- 段读者：DAG读者·uring-submit
- 日期：2026-11-18
- 基线：master `5d94f233`（与 docs/evidence/e5-fullpath-machine-work/repo-reality.md 冻结的基线一致）
- 方法声明：纯源码阅读。本会话未编译、未运行测试/基准（同机频率测量约束）；除本文件外无任何写操作。
  全部证据来自本会话 Read/grep 读到的源码行；内核侧行为明确标注为 io_uring UAPI 语义（非仓库代码，本会话未运行内核路径）。
- 唯一事实来源：当前源码。文档声明一律不作为代码依据；文档与代码不符之处单独列于 §4。

## 1. 段边界与主读文件

| 边界 | 内容 | 归属 |
|---|---|---|
| 上游 | `File`/`Request`/调度器/等待域 | 其他段 |
| 本段 | `RequestScope::submit_*` → `AsyncIoContext::submit_*` → `UringAsyncBackend::submit_request` → `dispatch_after_accept` → `dispatch_one_locked`（SQE 分配/准备）→ cookie/RouterEntry/TransportLedger → `submit_transport_locked`（liburing `io_uring_submit` → `io_uring_enter`）→ 内核 io_uring → VFS/PageCache；含运行中取消控制 SQE、transport 结果对账与毒化恢复、构造期能力探针 | 本文 |
| 下游 | CQ 侧：`uring_completion.cpp` 的 `reap_cqes`/`handle_one_cqe`/`finalize_operation_terminal_`，以及 publication/observer 投递 | 其他段（跨段接口在 E15/E26 注明） |

主读文件（均整篇读过）：
- `src/async/uring_backend.cpp`（812 行）
- `src/async/uring_transport.cpp`（716 行）
- 顺调用链：`src/async/async_io_context.cpp`、`src/async/request_scope.cpp`、`src/async/detail/request_core.cpp`、`src/async/uring_completion.cpp`（仅作 CQ 侧接口上下文）、`src/async/uring_internal.hpp`、`include/sluice/async/detail/uring_backend.hpp`、`include/sluice/async/detail/request_core.hpp`、`include/sluice/async/detail/progress_source.hpp`、`include/sluice/async/detail/reference_ready_sink.hpp`、`include/sluice/async/detail/async_backend.hpp`、`include/sluice/detail/file_semantics.hpp`、`include/sluice/detail/uring_submit.hpp`、`include/sluice/measurement.hpp`、`include/sluice/async/uring_backend.hpp`、`include/sluice/async/async_io_context.hpp`（op 结构）、`src/async/uring_test_seams.hpp`、`include/sluice/async/detail/fail_fast.hpp`、`xmake/uring.lua`
- liburing UAPI：`/usr/include/liburing.h`（版本 2.13，见 `/usr/include/liburing/io_uring_version.h:5-6`；xmake 包缓存另有 2.14 目录 `/home/jnhu/.xmake/packages/l/liburing/2.14/`，构建期实际解析哪一个由 `xmake/uring.lua:203-299` 的工具链 trace 决定——本会话按只读约束未运行该 probe，故标注：以下 liburing 内联函数体引用以系统头 2.13 为准）

## 2. 锁与同步拓扑（本段）

```
access_mtx_ (AsyncIoContext, async_io_context.hpp:336)
  └─ serialize 所有 submit_*/cancel/析构结算（async_io_context.cpp:255,276,296,316,336,356,376）
       └─ dispatch_mtx_ (UringAsyncBackend, uring_backend.hpp:320)
            ├─ core mutex (RequestCore::mutex_, request_core.hpp:295)   ← 仅在 dispatch_mtx_ 内层获取
            └─ ProgressSource::mtx_ (progress_source.hpp:491)           ← signal_ready_progress 在持 dispatch_mtx_ 的
                                                                            poison_and_recover_locked(uring_transport.cpp:523)
                                                                            与释放后（dispatch_after_accept:98-101、
                                                                            cancel_key:601-615、publish_zero_op_inline:573-575）都可发生
ProgressSource 唤醒物：context 级 eventfd（progress_source.hpp:74-79），
后端经 io_uring_register_eventfd 把完成通知挂到同一 fd（uring_backend.cpp:263-281）
```

提交热路径（一次非零 read，经 RequestScope）锁获取次数（以实际代码计，非调用次数）：
1. `access_mtx_` ×1（async_io_context.cpp:255）
2. core mutex ×4：`reserve`（request_core.cpp:166）、`accept`（:187）、`claim_execution`（dispatch_mtx_ 内，:368）、`outstanding_summary`（统计，:94）
3. `dispatch_mtx_` ×2：delivery 记录初始化（uring_backend.cpp:489）、dispatch_after_accept（uring_transport.cpp:49）
4. 内核：`io_uring_submit` → 1 次 `io_uring_enter(2)`（flags=0 环无 SQPOLL，uring_backend.cpp:171；ledger 空时提前返回不进内核，uring_transport.cpp:219）

## 3. 范围事实：Direct I/O 缺席

任务说明将"该路径上的 Direct I/O 对应实现"列入范围。当前源码事实：**生产代码不存在 O_DIRECT/Direct I/O 实现**。

证据（本会话执行）：
- `grep -rnw "O_DIRECT" --include=*.cpp --include=*.hpp --include=*.h /home/jnhu/Source/Sluice`：C++ 源码零命中（仅 `docs/` 与 `research/` 的 markdown 命中）。
- 初次 `grep -rln "O_DIRECT"` 曾命中 `apps/sluice-copy/safe_output.cpp`、`research/direct-beyond-parity/closure/r8-ac3/ac3-sem.cpp`——复核为 `O_DIRECTORY` 的子串假命中（safe_output.cpp:170、ac3-sem.cpp:317 均为 `O_RDONLY | O_DIRECTORY`）。
- 因此本段 SQE 只会落在打开时的常规缓冲文件上：READ/WRITE 经 VFS→PageCache（页缓存命中复制/缺页回填），无 `IORING_OP_*` 直通块层的旁路实现。

## 4. 文档/注释与代码不符

1. `include/sluice/async/detail/uring_backend.hpp:24-25` 注释称 `struct statx` 完整定义"lives either in `<linux/statx.h>` or in the guarded mirror inside uring_backend.cpp"。实际 `uring_backend.cpp` 无任何 statx 镜像定义、未包含 `<linux/statx.h>`，定义来自 `<sys/stat.h>`（uring_backend.cpp:28）。注释过期。
2. 任务范围声明与代码不符：见 §3（Direct I/O 在生产路径不存在）。
3. （边界核对，非不符）`uring_transport.cpp:1-5` 与 `uring_completion.cpp:1-4` 的 SQ/CQ 分工注释与实际代码一致。

## 5. 执行边记录

约定：裁决取值 `KEEP` / `ELIMINATE_CANDIDATE` / `UNPROVEN`。
"ELIMINATE_CANDIDATE"指该边存在可指认的冗余工作（值得进入下一步机器测量的追问），不等于可安全删除；
"删除的正确性风险"给出删除/改写时需要另行证明的义务。

---

### E01 分发入口：RequestScope::submit_read/write（scope 槽位预留）
- 锚点：src/async/request_scope.cpp:137-161（write :163-187）
- 语义义务：把调用方的 op 绑定进 scope 的结算域；scope 析构时对未就绪槽位执行 cancel/drain 结算。
- 当前操作：`finished_` 检查 → `reserve_slot_` O(capacity) 线性扫槽位 → 构造 `Reservation` RAII 守卫 → `ctx_.submit_*` → `commit_` 写入 Request 句柄。
- CPU工作：O(capacity) 槽位数组扫描（无锁，scope 单 owner：构造时 `claim_owner_` 已占 ProgressOwner，request_scope.cpp:33-35）；`finished_` 布尔读。无内联歧义：`reserve_slot_` 为私有成员，预期内联。
- 内核工作：无。
- 内存搬移：无数据搬移；槽位状态字写。
- 同步：无锁（scope 单线程所有）；RAII guard 异常安全。
- 冗余假设：scope 槽位线性扫描 O(capacity) 在深 scope 下每提交付全扫；scope 容量通常远小于 core 容量，且该结构承担结算域，未见独立事实重复。
- 证据：request_scope.cpp:46-54（reserve_slot_ 线性扫）、:33-35（owner 占用）、:137-161。
- 删除的正确性风险：删除 scope 层会使析构结算/取消义务失去挂点（scope 是结算驱动）。
- 裁决：KEEP

---

### E02 后端分发：AsyncIoContext::submit_* → backend 虚调用
- 锚点：src/async/async_io_context.cpp:249-269（write :271-289、sync_data :291-309、sync_all :311-329、file_info :331-349、size :351-369）；分发表 include/sluice/async/detail/async_backend.hpp:74-79
- 语义义务：进程级后端分发唯一入口；在 `access_mtx_` 临界区内完成"校验→分发→统计→Request 句柄构造"，并与析构的 `close_admission`（async_io_context.cpp:86-92）互斥，防止 late accept 被冻结进结算集。
- 当前操作：`initiation_rejection`（async_io_context.cpp:24-26，constexpr `precheck_state_op`）→ `access_mtx_` → `backend_->submit_*(op)` 虚分派 → `tally_submit`（stats 三分类计数）+ `update_max_outstanding` → `backend_->outstanding()` → `core_->outstanding_summary()`（core mutex，request_core.cpp:93-96）→ 构造 `Request`。
- CPU工作：1 次 context 级互斥锁全程持有（所有提交/取消串行）；1 次虚调用；`initiation_rejection` 为 constexpr 分支链；统计 4 次原子性不存在的普通计数写 + 1 次 core mutex（outstanding_summary）。注意 `submit_size` 的 initiation_rejection 用 `FileOperation::file_info`（:352）与后端 `validate_size` 一致（uring_backend.cpp:120-123）。
- 内核工作：无。
- 内存搬移：无数据搬移；`Request` 句柄（3 个整数）构造。
- 同步：`access_mtx_`（context 级串行化）；core mutex ×1（outstanding_summary）。
- 冗余假设：`initiation_rejection` 与后端 `validate_op`（E03）对同一 (fd<0, access, op) 重复执行同一 `precheck_state_op`——见 E03；`update_max_outstanding` 每提交多付一次 core mutex 只为维护统计极值。
- 证据：async_io_context.cpp:24-26,249-269；request_core.cpp:93-96；async_io_context.hpp:336。
- 删除的正确性风险：`access_mtx_` 不可删——它与析构 `close_admission` 构成 admission 竞争窗口的唯一封闭手段（async_io_context.cpp:86-92 注释所述义务）；删除统计边只影响 AsyncStats 记账。
- 裁决：KEEP

---

### E03 submit_request 入口校验与门
- 锚点：src/async/uring_backend.cpp:430-438（validate 调用）、94-137（validate_read/write/sync/file_info/size 与 `validate_op` 分派）
- 语义义务：后端自身的契约门：closed/access/range/空指针校验 + `have_ring_`/`fatal_error_` 两道状态门（毒化后拒绝新工作）。
- 当前操作：`precheck_data_op`（file_semantics.hpp:157-176，constexpr：closed→access→len==0→range→kMaxNativeTransfer）/`precheck_state_op`（:178-185）；data-op 另查 buffer 空指针（uring_backend.cpp:97-98,104-105）。
- CPU工作：constexpr 分支链，无锁。**与 E02 重复**：state-op（sync/file_info/size）的 `precheck_state_op` 参数与 E02 的 `initiation_rejection` 完全相同，每提交跑两遍同一纯函数；data-op 的第二遍增量为 range 与空指针检查（E02 不查）。
- 内核工作：无。
- 内存搬移：无。
- 同步：无。
- 冗余假设：**生产唯一入口经 E02 已做同式校验，后端层 state-op 校验的每提交重复执行是可指认的冗余**；后端直连调用方仅存在测试缝（AsyncBackend 为未安装 seam，async_backend.hpp:17-19；`make_uring_context` 是唯一生产构造路径，uring_backend.cpp:146-148）。
- 证据：uring_backend.cpp:94-123、430；async_io_context.cpp:250-252（同式 `precheck_state_op`）；file_semantics.hpp:157-185。
- 删除的正确性风险：后端作为独立契约边界，直连 seam 测试依赖其后端级校验；若删除后端层 state-op 检查须先证明"所有调用方必经 context 层"是结构保证而非约定。
- 裁决：ELIMINATE_CANDIDATE（state-op 重复半边；data-op 的 range/空指针增量为后端独有，KEEP）

---

### E04 RequestCore 槽位预留（core_->reserve）
- 锚点：src/async/uring_backend.cpp:447-458；src/async/detail/request_core.cpp:165-182
- 语义义务：容量与 admission 所有权：从 `free_slots_` 弹出槽位并置 `reserved` 世代；`admission_closed`/`capacity`/`exhausted` 三类拒绝映射为 would_block/invalid_state（uring_backend.cpp:449-457，含 ERR01 mutant 反证分支）。
- 当前操作：core mutex 临界区内 pop 自由栈 + 状态写。
- CPU工作：1 次 core mutex 加解锁 + O(1) 栈操作。`request_capacity` 上限校验在构造期（uring_backend.cpp:150-159）。
- 内核工作：无。
- 内存搬移：无（槽位索引/世代各 4/8 字节值传递）。
- 同步：core mutex。
- 冗余假设：reserve 与 accept 分两段锁获取（中间夹 prepared_ops_ 写与 delivery 锁）是三阶段失败注入（SubmitStage::reserve/prepare/commit，uring_backend.cpp:442-465,504-509）与毒化窗口的语义边界；未见独立事实重复。
- 证据：request_core.cpp:165-182；uring_backend.cpp:447-458。
- 删除的正确性风险：槽位世代是全链身份机制（RequestKey/取消/对账），不可删。
- 裁决：KEEP

---

### E05 PreparedUringOp 槽位写入（SQE prep 的数据源）
- 锚点：src/async/uring_backend.cpp:468-486；结构定义 include/sluice/async/detail/uring_backend.hpp:159-167；长度截断 include/sluice/detail/uring_submit.hpp:11-14
- 语义义务：把 op 的物理参数（kind/fd/buffer/length/native_length/offset/statx 缓冲）落到 per-slot 向量，供 E14 准备 SQE 与 CQ 侧 `handle_one_cqe`/`finalize_operation_terminal_` 读回（uring_completion.cpp:34,153-156）。`offset` 对零长 op 强制 0（:473）。
- 当前操作：一次性聚合写 `prepared_ops_[slot]`；`uring_chunk_length` 把 size_t 长度截到 `unsigned`（SQE 域），>4GiB 传输以 native_length=UINT32_MAX 进入内核，全量长度仅保留在 core descriptor（E07）供短完成统计（uring_completion.cpp:81-82）。
- CPU工作：约 7 字段结构写 + 1 次 min 截断（constexpr 内联）。**注意**：该写发生在 reserve 之后、dispatch 之前，槽位此时已被本线程独占（世代保护），无锁。
- 内核工作：无。
- 内存搬移：~64B/操作 到 `prepared_ops_` 向量槽位（预分配，构造期一次 reserve，uring_backend.cpp:162）。
- 同步：无锁（槽位世代独占）。
- 冗余假设：`length`（size_t）与 `native_length`（unsigned 截断）双份存储——native_length 是 SQE 域的必要投影，length 是短完成统计的基准；两者域不同，非重复。
- 证据：uring_backend.cpp:468-486；uring_submit.hpp:11-14；uring_completion.cpp:34,81-82,153-156。
- 删除的正确性风险：CQ 侧依赖 prepared_ops_ 判定 byte/metadata 操作与统计长度；删除即断完成段。
- 裁决：KEEP

---

### E06 delivery_ 槽位记录初始化（dispatch_mtx_）
- 锚点：src/async/uring_backend.cpp:488-500；结构 include/sluice/async/detail/uring_backend.hpp:198-204
- 语义义务：在 dispatch_mtx_ 下初始化该槽位的投递记录（kind + owed 状态清零），并以 fail-fast 断言"预留槽位不得携带 owed 事件"——owed 控制引用把槽位钉住防回收（uring_backend.hpp:201-203 注释与 request_core.cpp:584-590 的 reclaim 条件互为印证），该断言是毒化/取消竞争下发布协议的契约诊断。
- 当前操作：持锁检查 `record.event_owed`（违例→`uring_core_handoff_fail_fast`）、写 kind/event_owed=false/owed_key={}。
- CPU工作：1 次 dispatch_mtx_ 加解锁 + 3 字段写 + 1 布尔检查。
- 内核工作：无。
- 内存搬移：DeliveryRecord（~24B）字段写。
- 同步：dispatch_mtx_。
- 冗余假设：不变量证明（owed 事件 ⇒ control_refs>0 ⇒ 槽位不可回收 ⇒ reserve 只会拿到已清零槽位）成立时，`event_owed=false; owed_key={}` 两写恒为重写已零值；仅 kind 是有效载荷。但该检查是根要求的契约诊断（机制门槛允许），且把"交付所有权在锁内移交"显式化——按"复杂度不是冗余证据"规则不裁删。
- 证据：uring_backend.cpp:488-500；request_core.cpp:584-590（reclaimable_ 含 control_refs==0）；uring_backend.hpp:198-204。
- 删除的正确性风险：删除 fail-fast 断言会丢失对"owed 引用钉槽位"协议的运行期见证；删除 kind 写会破坏 publish_one 的 kind 读取（uring_backend.cpp:589-593）。
- 裁决：KEEP

---

### E07 RequestCore accept（所有权转移 + 借出事实登记）
- 锚点：src/async/uring_backend.cpp:512-518；src/async/detail/request_core.cpp:184-210；BorrowFacts/RequestDescriptor 定义 request_core.hpp:19-30
- 语义义务：把 reserved 槽位升为 accepted：登记 descriptor（op/offset/length/zero_op）与 borrow（fd/buffer/length——内核借出的用户态事实）、`execution_refs=1`、`outstanding_++`；零长 op 在 accept 内即定终局（terminal_chosen+success(0)+execution_refs=0，request_core.cpp:203-207）。
- 当前操作：core mutex 临界区：resolve_reserved_ 校验世代 → 字段写。
- CPU工作：1 次 core mutex + O(1) 字段写。
- 内核工作：无。
- 内存搬移：Slot 内 descriptor/borrow/事件字（~80B）写。
- 同步：core mutex。
- 冗余假设：borrow 三元组与 PreparedUringOp（E05）的 fd/buffer/length 内容重复——但 borrow 是 core 的"谁被借出"权威（毒化/取消结算按它裁定执行引用退休，如 uring_transport.cpp:113-124），prep 是传输投影；属两 authority 各自代表同一物理事实，按规则不合并。
- 证据：request_core.cpp:184-210；request_core.hpp:19-30；uring_transport.cpp:113-124。
- 删除的正确性风险：accept 是终局仲裁（offer_terminal/claim_execution）的对象载体，不可删。
- 裁决：KEEP

---

### E08 零长度短路发布（publish_zero_op_inline）
- 锚点：src/async/uring_backend.cpp:520-521（分派）、553-576（实现）
- 语义义务：零长 data-op 不进内核（无 SQE/CQE），但必须仍完成"控制引用→发布→owed 事件→唤醒"闭环，使等待方在无内核事件时也能被唤醒。
- 当前操作：`acquire_control` → `begin_publication` → `complete_publication`（三次 core mutex）→ 持 dispatch_mtx_ 置 owed 记录 + `owed_count_++` → `signal_ready_progress`。
- CPU工作：3 次 core mutex + 1 次 dispatch_mtx_ + 1 次 ProgressSource::signal（mutex + eventfd write，见 E24）。
- 内核工作：无（这是本边存在的理由）。
- 内存搬移：DeliveryRecord 字段写。
- 同步：core mutex ×3、dispatch_mtx_ ×1、ProgressSource::mtx_ ×1 + eventfd write(2)。
- 冗余假设： owed 路径复用 poll() 的 owed 扫描（uring_backend.cpp:663-686）而非直接投递，是为把 on_ready 回调隔离在锁外（uring_backend.hpp:196-204 注释）；若在 submit 线程直接 deliver_event 可省 owed 记账，但会把用户回调引入提交路径并打破"回调绝不持锁"约束。
- 证据：uring_backend.cpp:520-521,553-576,663-686；request_core.cpp:411-436,438-484。
- 删除的正确性风险：删除短路会让零长 op 请求永不发布（无 CQE 可消费）。
- 裁决：KEEP

---

### E09 dispatch_after_accept（入队 + 内联清空 + 毒化退休 + 重试广告）
- 锚点：src/async/uring_transport.cpp:43-102；毒化退休 :104-128；重试判定 :211-216
- 语义义务：把 accepted 请求推进 SQ 机器：持 dispatch_mtx_ 入 `dispatch_` 队列，随后内联清空（毒化未发生时逐个 `dispatch_one_locked`，含 E10-E17）；若本次或此前 submit 失败遗留 transport（`dispatch_retry_remains_locked_`：队列非空或 ledger 有未退休项）则在锁外 `signal_ready_progress` 广告重试义务（:93-101）。毒化后新入队项走 `retire_undispatched_after_poison_locked_`：claim→offer_terminal(failure)→release_execution→入 publication_pending_→统计（:104-128），前提是这些项未取过 SQE（毒化后 dispatch 拒绝一切 ring 访问，uring_transport.cpp:131-132）。
- 当前操作：1 次 dispatch_mtx_；循环 front()/dispatch_one_locked 直至队空或失败；条件信号。
- CPU工作：1 次 dispatch_mtx_ 加解锁；每项派发递归摊派到 E10-E17；`remove_exact`/pop 在 E12 记；BoundedDispatchQueue push O(1)（uring_internal.hpp:99-108）。
- 内核工作：经 E17 间接发生（本边内联清空即提交）。
- 内存搬移：Reservation（16B）入队/出队拷贝。
- 同步：dispatch_mtx_（全程）；锁外 ProgressSource::signal（条件触发）。
- 冗余假设：内联清空与 poll() 的清空（E23）重复——两者都是"派发后立即尝试提交"与"重试驱动"两种触发面，共享同一 dispatch_one_locked；单线程使用时 poll() 的 dispatch 分支只在 submit 失败遗留时才有活干，属重试义务非重复。
- 证据：uring_transport.cpp:43-102,104-128,211-216；uring_backend.cpp:623-634。
- 删除的正确性风险：删除重试广告会违反 PROG-02 的"停车须有后续 pass"义务（async_io_context.cpp:18-22 的 10ms 重试节奏依赖该广告）；删除毒化退休会泄漏未派发请求的终局。
- 裁决：KEEP

---

### E10 SQE 分配（io_uring_get_sqe + 满环即时冲刷重试）
- 锚点：src/async/uring_transport.cpp:141-149；liburing 内联 /usr/include/liburing.h:1917-1925（`_io_uring_get_sqe`）、1683-1697（`io_uring_load_sq_head`，flags=0 非 SQPOLL 时为普通共享内存读）、570-581（`io_uring_initialize_sqe`：8 字段清零）
- 语义义务：从 SQ 环取一个空 SQE；满环时先冲刷（E17）腾位重试，仍满则返回 false 留在队列等下一 pass。
- 当前操作：liburing：读内核 khead（共享内存）→ tail-head==ring_entries 判满 → 计算 `sqes[(tail&mask)<<shift]` → `sqe_tail++` → 8 字段清零。满环分支：调用 `submit_transport_locked`（毒化检查后二次 get_sqe）。
- CPU工作：O(1)；共享内存读 + 指针算术 + 8 字段清零（SQE 是 mmap 共享环内存，非 malloc）。满环分支多一次完整 enter。
- 内核工作：无（仅读 khead 映射页）。
- 内存搬移：SQE 槽 64B 清零（用户态写共享环）。
- 同步：无锁（dispatch_mtx_ 持有中；内核经 khead 单向发布，非 SQPOLL 无需 acquire——liburing.h:1687-1691 注释）。
- 冗余假设：每 SQE 8 字段清零（initialize_sqe）是 liburing 通用路径，对"每次都完整覆盖 opcode/fd/off/addr/len"的 prep 而言其中 5 字段清零后被立即覆盖；但 user_data 之外的字段（flags/ioprio/buf_index/addr3/pad）不写会泄漏上一个操作的残留，清零是内核 ABI 正确性要求——非冗余。
- 证据：uring_transport.cpp:141-149；liburing.h:1917-1925,1683-1697,570-581。
- 删除的正确性风险：满环重试是"派发不丢工作"义务；删除会把可即时腾出的队列项留给 10ms 后的 poll 重试（正确但更慢）。
- 裁决：KEEP

---

### E11 执行权仲裁：claim_execution + SQE 尾回退
- 锚点：src/async/uring_transport.cpp:151-159；src/async/detail/request_core.cpp:367-378
- 语义义务：派发前仲裁"谁拥有执行权"：公开取消若在 accept 与派发之间赢得终局（core_->cancel 置 terminal_chosen，request_core.cpp:269-274），claim 返回 late——此时 SQE 槽已被取但**未写入任何字段、未提交**，回退 `--sq.sqe_tail` 保持环记账精确，remove_exact 移除后继续处理下一项（uring_transport.cpp:153-158 注释明示该理由）。
- 当前操作：core mutex（claim_execution）+ 1 次 tail 自减 + remove_exact（见 E12）。
- CPU工作：1 次 core mutex + O(1) 回退。
- 内核工作：无（关键：回退发生在任何内核可见性之前）。
- 内存搬移：无。
- 同步：dispatch_mtx_（外层）+ core mutex。
- 冗余假设：仲裁与 E20 取消路径的 `won_before_execution` 分支共享同一终局事实（core_->cancel 的 terminal_chosen），单一 authority，无重复。
- 证据：uring_transport.cpp:151-159；request_core.cpp:260-275,367-378。
- 删除的正确性风险：删除仲裁会让"取消先于执行赢得"的请求仍被提交给内核——borrow 已被取消语义释放，造成 use-after-callback 义务破坏。
- 裁决：KEEP

---

### E12 dispatch 队列移除（remove_exact）
- 锚点：src/async/uring_transport.cpp:157（claim 失败路径）、201-206（成功路径，miss 即 fail-fast）；实现 src/async/uring_internal.hpp:118-133；全部调用点：cancel_key :589、retire_undispatched_after_poison_locked_ 用 pop_front 循环（:107）
- 语义义务：把已派发的 Reservation 从 `dispatch_` 队列摘除；miss 是不变量违例（成功路径 fail-fast，uring_transport.cpp:201-206）。
- 当前操作：线性扫描匹配 (slot,generation) 后**整段前移一格**（uring_internal.hpp:123-128 的拷贝循环）。
- CPU工作：O(队列深)。**调用点事实**：dispatch_one_locked 的两个调用方都传 `dispatch_->front()`（uring_transport.cpp:82、uring_backend.cpp:624），匹配必中 i=0，随后把其余 size-1 个 16B Reservation 各拷贝一次。front 场景与 `pop_front`（O(1)，uring_internal.hpp:109-116）完全等价；remove_exact 的"任意位置删除"能力只被 cancel_key（:589，元素可在任意位置）真正需要。
- 内核工作：无。
- 内存搬移：每派发 1 个操作付 (size-1)×16B 的 Reservation 拷贝（深队列下显著）。
- 同步：dispatch_mtx_ 内。
- 冗余假设：**dispatch 两个调用点的 remove_exact(front) 可替换为 pop_front，把每派发 O(n) 搬移降为 O(1)**；remove_exact 本身保留给 cancel_key。此为可指认的纯 CPU 冗余（正确性等价，miss fail-fast 语义需以"pop 前已核对 front"替代）。
- 证据：uring_transport.cpp:82,157,201-206,589；uring_backend.cpp:623-626；uring_internal.hpp:109-133。
- 删除的正确性风险：若不改调用点而直接删 remove_exact，cancel_key 的任意位置移除失去实现；改用 pop_front 需保证两处调用点的 front 对应关系（当前由构造保证）不被未来改动破坏。
- 裁决：ELIMINATE_CANDIDATE（仅限两处 front 调用点换 pop_front；函数本身 KEEP）

---

### E13 cookie 分配（allocate_cookie_）
- 锚点：src/async/uring_transport.cpp:164（调用）、526-538（实现）；标签算术 src/async/uring_internal.hpp:45-58
- 语义义务：为操作分配 CQE 路由身份：单增 64 位、域 [1, 2^63) 与控制标签域（bit63）隔离；耗尽/回绕即 fail-fast（:530-536）。cookie 是 SQE `user_data`→RouterEntry→RequestKey 的完成路由键（CQ 侧 `find_live_router_cookie_`，uring_completion.cpp:148）。
- 当前操作：区间检查 + `next_cookie_++`。
- CPU工作：O(1)，2 条比较 + 1 自增，无锁（dispatch_mtx_ 内）。
- 内核工作：无。
- 内存搬移：8B user_data 写入 SQE（E14）。
- 同步：dispatch_mtx_ 内。
- 冗余假设：cookie 与 RequestKey(slot,generation) 信息上可互推（cookie→router→handle→key），但 CQE 只能携带 64 位 user_data，cookie 是内核 ABI 与用户路由间的必要翻译层；cookie_free_list_/router_ 索引与 slot 索引不同（router 槽独立分配），因为一个 slot 的生命周期可跨越多个 router 槽（控制 CQE 滞留）。未见可删冗余。
- 证据：uring_transport.cpp:164,526-538；uring_internal.hpp:45-58；uring_completion.cpp:99-155。
- 删除的正确性风险：删除 cookie 域检查会在 2^63 后静默进入控制标签域，CQ 侧把操作 CQE 误判为控制 CQE。
- 裁决：KEEP

---

### E14 SQE 准备（io_uring_prep_* + user_data 标记）
- 锚点：src/async/uring_transport.cpp:166-189；liburing /usr/include/liburing.h:583-595（`io_uring_prep_rw`：opcode/fd/off/addr/len 5 字段写）、1020-1029（prep_read）、1038-1043（prep_write）、789-794（prep_fsync）、1047-1054（prep_statx）、883-890（prep_cancel64）、531-535（`io_uring_sqe_set_data64`：1 字段写）
- 语义义务：按 op 类型把 E05 的物理参数写入 SQE：READ/WRITE（fd/buf/len/offset）、FSYNC（DATASYNC 标志或 0）、STATX（fd+空路径+AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW+四掩码位+statx 缓冲）；`user_data=cookie` 是内核回程路由的唯一载体。
- 当前操作：5-6 个字段写 + 1 个 user_data 写，全部落在 mmap SQ 环内存。
- CPU工作：O(1) 纯存储（~7×8B），无锁；switch 分派按 E05 的 kind。
- 内核工作：无（准备仅为内存写）。
- 内存搬移：~48-64B 写入共享 SQ 环。
- 同步：dispatch_mtx_ 内（内核只在 enter 后可见）。
- 冗余假设：未使用固定文件表/缓冲环（每次传 fd 与裸指针），每操作多付内核 fd→file 映射与缓冲校验——这是能力面选择（未注册 fixed files/buffer ring），按"无实测热点不立项"规则不列为删除候选，留作内核工作侧观察点。
- 证据：uring_transport.cpp:166-189；liburing.h 上述行；uring_backend.cpp:171（flags=0）。
- 删除的正确性风险：user_data 是完成路由的根，缺它 CQE 无法归属（CQ 侧对 user_data==0 直接丢弃，uring_completion.cpp:145-146）。
- 裁决：KEEP

---

### E15 RouterEntry 布线 + live_cookies_ 计数
- 锚点：src/async/uring_transport.cpp:190-195；RouterEntry 定义 include/sluice/async/detail/uring_backend.hpp:178-191；退休 src/async/uring_transport.cpp:559-582；cookie 自由表 :161-162
- 语义义务：在 router_ 槽登记 cookie→handle 映射与控制状态机（none/prepared/submitted）；`terminal_delivered` 保证原操作 CQE 到达前路由不退休（:436-441 注释）；live_cookies_ 供析构/`internal_work_retired` 的静止见证（uring_backend.cpp:350-351,371-372）。
- 当前操作：RouterEntry 重置+4 字段写；cookie_free_list_ pop/push；live_cookies_ relaxed fetch_add/fetch_sub。
- CPU工作：O(1)；1 次 relaxed 原子 RMW（x86 上 ≈ 普通 add/mov）。**访问面事实**：live_cookies_ 的全部生产读写（:195、:581、uring_backend.cpp:350,371）都在 dispatch_mtx_ 临界区内（retire_router_entry_ 的所有调用方均在锁内：uring_transport.cpp:440,517、uring_completion.cpp:95,140→handle_one_cqe:100 持锁）；唯一锁外读者是测试缝 `live_cookies_for_test`（uring_test_seams.hpp:79-81）。
- 内核工作：无。
- 内存搬移：RouterEntry（~48B）写。
- 同步：dispatch_mtx_（原子计数冗余于锁，见冗余假设）。
- 冗余假设：**live_cookies_/live_control_sqes_ 用原子实现，但生产访问全部被 dispatch_mtx_ 覆盖；原子性仅为两个无锁测试缝读取服务**（live_control_sqes_for_test uring_transport.cpp:38-40 无锁读）。改普通字段需同步改测试缝加锁——收益微小（relaxed 原子在 x86 ≈ 普通 mov），列为低优先追问。
- 证据：uring_transport.cpp:190-195,559-582；uring_backend.hpp:324-326；uring_backend.cpp:350-351,365-373；uring_test_seams.hpp:75-81。
- 删除的正确性风险：live_cookies_ 是 `internal_work_retired` 的 O(1) 派生摘要（替代 router_ 全扫），删除需改回 O(n) 扫描或证明其调用频率可忽略。
- 裁决：KEEP（原子实现为 ELIMINATE_CANDIDATE 低优先子项）

---

### E16 TransportLedger 登记（append）
- 锚点：src/async/uring_transport.cpp:196-199（调用）；实现 src/async/uring_internal.hpp:146-240（append :170-191）
- 语义义务：维护"已放入 SQ 环但内核尚未消费"的物理队列的 userspace 有序副本：每项带物理 SQ 槽位、cookie、handle、kind，单调序列+物理位置连续性由 append 内不变量校验（违例 fail-fast）；是 E17 成功消费计数、E18 对账、E19 Class-A 恢复的唯一依据。
- 当前操作：期望物理位置推导（last+1 mod capacity）与实参比对 → 槽位写 Entry{sequence++,kind,cookie,handle,false} → size++。
- CPU工作：O(1) + 2 组不变量比较；dispatch_mtx_ 内。
- 内核工作：无。
- 内存搬移：Entry（~40B）写。
- 同步：dispatch_mtx_。
- 冗余假设：ledger 与 liburing 的 sqe_tail/khead 共享内存对账目标重复——但 userspace 侧的"哪些 SQE 已入环但毒化时内核不可见"判定需要**有序**句柄副本（khead 只给数量，不给身份），毒化恢复（E19）与重试正确性（uring_transport.cpp:263-266 注释：部分内核取数后重试计数会失同步）依赖它，属独立物理义务的派生摘要。
- 证据：uring_internal.hpp:170-191,223-229；uring_transport.cpp:237-279,298-335,337-443。
- 删除的正确性风险：删除 ledger 即删除毒化时"内核可见性证明"（E19 的前提：头未越过即不可见），Class-A 恢复失去身份与顺序依据。
- 裁决：KEEP

---

### E17 提交冲刷：submit_transport_locked → io_uring_submit → io_uring_enter(2)
- 锚点：src/async/uring_transport.cpp:218-235（ledger 空/毒化提前返回 :219，计数 :222，test hook :225-229，真提交 :231）；调用点：dispatch_one_locked 满环 :143、dispatch_after_accept :87、poll :631、issue_running_cancel :647；ring flags=0（uring_backend.cpp:171）
- 语义义务：把 SQE 尾环交给内核；返回值交 E18 记账。ledger 空则零开销返回（不进内核）。
- 当前操作：`submit_flushes_` relaxed fetch_add（仅测试读者，见 E15 类冗余）→ `::io_uring_submit(&ring)` → `account_transport_result_locked(rc, true)`。
- CPU工作：liburing 库函数（非头内联；/usr/include/liburing.h:244 声明）：取 `sqe_tail - khead` 差、必要时 sqring_wait（SQPOLL 才触发，本环无）、发起 `io_uring_enter(2)` 系统调用（to_submit=待提交数，flags=0）。用户态部分为轻量；主体是 1 次系统调用往返。
- 内核工作（E17 内核半边）：`io_uring_enter(2)`：校验 fd/权限 → 取 khead→sq_tail 区间 SQE → 逐个拷贝/校验（opcode 合法性、fd 解引用、I/O 向量与缓冲访问检查）→ 排入内核工作队列（可内联执行或 task_work/workqueue）→ 返回成功提交数。
- 内存搬移：SQE 由内核从共享环读取（无需用户态拷贝到内核；io_uring 相对 aio 的核心收益点）；无数据搬运。
- 同步：dispatch_mtx_ 持有中发起系统调用——**这是热路径锁内 syscall**：所有提交在 dispatch_mtx_ 与 access_mtx_ 双重串行下进入内核。
- 冗余假设：`submit_flushes_` fetch_add 每次提交执行而其唯一读者 `submit_flushes_for_test()` 仅存在于 SLUICE_ASYNC_INTERNAL_TESTING（uring_test_seams.hpp:75-77；生产构建无读者）——**生产只写不读的原子计数**。
- 证据：uring_transport.cpp:218-235；uring_test_seams.hpp:75-77；liburing.h:244,1701-1706；内核侧行为为 io_uring_enter(2) UAPI 语义（本会话未运行/未验证内核源码）。
- 删除的正确性风险：提交即本段存在目的；可删的只有 `submit_flushes_` 计数（删之生产无影响，测试断言失去数据源）。
- 裁决：KEEP（`submit_flushes_` 为 ELIMINATE_CANDIDATE）

---

### E18 transport 结果记账（account_transport_result_locked）
- 锚点：src/async/uring_transport.cpp:237-279；控制消费 :281-296
- 语义义务：把 enter 返回值对回 ledger：rc>0 逐项 pop_front（把 cancel_control 项转 `mark_consumed_control_locked_`：exact-prepared 校验→submitted→live_control_sqes_++）；rc==0 无事；EINTR/EAGAIN/EBUSY 先对账再等重试（部分内核取数会令下次计数失同步，:263-266 注释）；其他负值对账后毒化（:271-278）。
- 当前操作：rc>0：O(consumed) 次 pop_front（每次带 retired_prefix_sequence 连续性校验，uring_internal.hpp:201-207）；负值：E19 对账 + 可能 E20 毒化。
- CPU工作：O(consumed)；dispatch_mtx_ 内。
- 内核工作：无。
- 内存搬移：Entry 拷贝出栈（~40B/项）。
- 同步：dispatch_mtx_。
- 冗余假设：`mark_consumed_control_locked_` 的 exact-prepared 三重比对（slot/generation/control_state，:284-293）是对"ledger handle 与 router 布线漂移"的契约诊断；两份身份登记（ledger.handle 与 router.handle）互为对账锚，毒化路径需要它们不一致时 fail-fast——单一事实两投影，保留。
- 证据：uring_transport.cpp:237-296；uring_internal.hpp:193-211。
- 删除的正确性风险：删除对账会让部分消费（rc<consumed 的内核短取）在下次 submit 时把错误数量的 ledger 项当作已消费——毒化/恢复与控制引用计数全盘失同步。
- 裁决：KEEP

---

### E19 内核头对账（reconcile_ledger_with_kernel_locked_）
- 锚点：src/async/uring_transport.cpp:298-335；liburing `io_uring_sq_ready` /usr/include/liburing.h:1701-1706（`sqe_tail - load_sq_head`，自由计数不取模）
- 语义义务：以内核 SQ khead 为准裁断 ledger 前缀哪些已被内核消费（consumed = size − sq_ready），逐项出栈并把 cancel_control 转 E18 半边；两条上界违例（unconsumed>ring_entries、unconsumed>size）fail-fast（:313-326）。掩码别名陷阱由自由计数差避免（:305-311 注释与 MASKED mutant 反证分支）。
- 当前操作：1 次共享内存 khead 读 + 差值算术 + O(consumed) 出栈。
- CPU工作：O(consumed)；dispatch_mtx_ 内。
- 内核工作：无（只读 khead 映射页）。
- 内存搬移：Entry 出栈拷贝。
- 同步：dispatch_mtx_。
- 冗余假设：与 E18 rc>0 的 pop 计数重复——不重复：E18 用**返回值**计数（权威），E19 用 **khead 差**计数（对账权威）；两者在毒化路径上互为校验（khead 已越过的项保留真实完成路径，其余证明不可见）。若 rc 恒等于 E19 差值则 E19 冗余，但这恰是 EAGAIN/部分消费场景不能假设的。
- 证据：uring_transport.cpp:298-335；liburing.h:1701-1706。
- 删除的正确性风险：删除则毒化时无法区分"内核已消费但 CQE 未达"与"内核未消费"——后者才能安全 Class-A 退休（uring_transport.cpp:343-349 注释）。
- 裁决：KEEP

---

### E20 毒化与 Class-A 恢复（poison_and_recover_locked）
- 锚点：src/async/uring_transport.cpp:337-524（触发点：E18 :278、overflow flush 失败 uring_completion.cpp:271-273）；队列退休循环 :350-372；ledger 循环 :377-443；信号 :523
- 语义义务：submit 级失败 → 粘性 `fatal_error_` + `note_health_failure`（关 admission）→ 三组退休：(a) dispatch_ 内全部项以 failure 终局退休（内核不可见证明：毒化后无 submit）；(b) ledger 剩余项逐项 Class-A 处理：operation 项若未交付终局则以 failure 退休（offer_terminal→release_execution→publication push→stats），cancel_control 项释放控制引用；(c) 原 CQE 仍可能到达的项**必须保留路由**（:436-441 注释：只有 terminal_delivered 且 control none 才 retire_router_entry_）。完成后 signal。生产构建中 :466-521 的 overflow-缺席结算块为 mutant 专属（#if 保护），不执行。
- 当前操作：O(dispatch_+ledger) 次 core mutex（每项 offer_terminal/release/release_control 各 1 次）+ 每项 publication_pending_ push + stats。
- CPU工作：失败路径一次性代价；持 dispatch_mtx_ 全程。
- 内核工作：无。
- 内存搬移：TerminalCandidate/IoOutcome 拷贝（每项 ~80B）。
- 同步：dispatch_mtx_（内层 core mutex ×3N）→ ProgressSource::signal。
- 冗余假设：失败终局对 (a)(b) 重复构造相同 IoOutcome candidate——两群体（未入环/已入环不可见）的退休义务不同（(a) 无执行引用问题，(b) 需 Class-A 身份核对），代码形态相近是巧合非重复。
- 证据：uring_transport.cpp:337-524；uring_completion.cpp:268-274。
- 删除的正确性风险：删除 Class-A 身份核对（:388-397）会让 ledger 项与 router 项漂移时静默退休错误请求；删除"等待原 CQE 项保留路由"（:436-441）会令迟到的真实 CQE 无法落地（stranded borrow）。
- 裁决：KEEP

---

### E21 运行中取消控制 SQE（cancel_key → issue_running_cancel_locked_）
- 锚点：src/async/uring_transport.cpp:584-618（cancel_key）、620-693（issue_running_cancel_locked_）；控制 cookie 标签 uring_internal.hpp:45-58；内核准备 liburing.h:883-890（prep_cancel64：addr=target user_data）
- 语义义务：对已 claim 执行权的在途请求发出物理取消：`core_->cancel` 返回 requested（execution_claimed，request_core.cpp:269-271）→ 分配/复用 SQE → `io_uring_prep_cancel64(target_cookie)` + `user_data=CONTROL_TAG|target_cookie`（回程识别为控制 CQE）→ `acquire_control` 引用钉住 → ledger 登记 cancel_control。SQE 取不到时转 sticky intent（control_pending + pending_control_count_）等后续 pass 补投（:653-671），intent 永不静默丢弃（uring_internal.hpp:74-77 `sticky_intent_retained`）。
- 当前操作：O(router_) 查找 `find_live_router_index_`（:540-549 线性扫）+ SQE 准备 + ledger append；可能内联触发一次 enter（:647）。
- CPU工作：O(router_) 线性查找（router 容量=request_capacity，默认 64，uring_backend.hpp:11）；dispatch_mtx_ 内。
- 内核工作：enter 时内核接收 IORING_OP_ASYNC_CANCEL：按 user_data 找到在途请求，取消其 kiocb；原操作/控制各自回 CQE（原操作 -ECANCELED；控制 CQE 标记取消结果）。
- 内存搬移：SQE 字段写 + ledger Entry 写。
- 同步：dispatch_mtx_；core mutex（acquire_control）；取消赢得时（won_before_execution）锁外 signal + stats（:601-615）。
- 冗余假设：控制引用（core control_refs）与 RouterEntry.control_state 双份跟踪同一控制 SQE 生命周期——前者是 core 侧槽位回收义务（reclaimable_ 要求 control_refs==0），后者是内核侧回程路由状态；毒化路径要求两者失配时 fail-fast（E19/E20），属对账双投影。
- 证据：uring_transport.cpp:584-693；request_core.cpp:260-275；uring_completion.cpp:101-143（控制 CQE 处理）。
- 删除的正确性风险：删除物理取消会让"取消"退化为逻辑标记——用户缓冲借出继续被内核持有，borrow 义务破坏。
- 裁决：KEEP

---

### E22 sticky intent 补投（service_pending_controls_locked_）
- 锚点：src/async/uring_transport.cpp:695-711；调用点：dispatch_after_accept :88、poll :632
- 语义义务：把上次取不到 SQE 的取消 intent 在后续 pass 补投为控制 SQE；`pending_control_count_==0` 时零开销返回（:701-703），计数为 O(1) 门。
- 当前操作：count 门 + O(router_) 扫描（仅当有 pending 时）→ 对每项复用 E21。
- CPU工作：常例 O(1)（count 为 0 直接返回）；pinned 时 O(router_)。
- 内核工作：经 E21 的 enter 间接。
- 内存搬移：无（经 E21）。
- 同步：dispatch_mtx_。
- 冗余假设：`pending_control_count_` 是 router_.control_pending 的派生计数（snapshot 测试缝已验证两者一致性，uring_test_seams.hpp:101-118）——派生摘要避免每次 pass 全扫，符合机制门槛的派生分类；无独立事实冲突。
- 证据：uring_transport.cpp:695-711,573-578,682-687。
- 删除的正确性风险：删除补投会让 sticky intent 永久滞留——取消义务静默失效（SLUICE_BACKEND_CONTRACT_MUTANT_STICKY_INTENT_DROPPED 反证分支存在，uring_internal.hpp:69-77）。
- 裁决：KEEP

---

### E23 poll() 的分发驱动面（重试驱动）
- 锚点：src/async/uring_backend.cpp:616-689（本段相关：dispatch 清空 :620-634、publication/owed 扫描 :638-686 属下游 CQ/发布段，仅注记）；pass 边界语义 :644-646 注释
- 语义义务：进度所有者的每 pass 驱动：清 dispatch_ 遗留（submit 失败重试）→ submit_transport → 补投控制 → reap CQE（下游段）→ 逐项发布 → owed 投递。
- 当前操作：dispatch_mtx_ #1（dispatch 清空+submit+控制补投）；reap（下游）；publication 循环锁 #2..N（每项 1 次锁）；owed 扫描（owed_count_ 门 + O(capacity) 扫描，每项 1 次锁）。
- CPU工作：常例（无遗留、无 owed）= 2 次 dispatch_mtx_ 加解锁 + 1 次 ledger 空检查。owed 扫描为 O(capacity)：由 owed_count_ 门控（:661-663），零 owed 时零扫描。
- 内核工作：见 E17（若 dispatch 遗留则本 pass 再 enter；无遗留时本边不进内核）。
- 内存搬移：Reservation 出入队拷贝。
- 同步：dispatch_mtx_ ×(2+每发布项)（发布/投递细节属下游段）。
- 冗余假设：与 E09 的内联清空重复的怀疑已在 E09 澄清：poll() 分支只在"submit 留了尾巴"时有活干，是 PROG-02 重试义务的执行面。owed 的 O(capacity) 扫描在大量零长 op 场景退化——有 count 门，列为观察点非候选。
- 证据：uring_backend.cpp:616-689；async_io_context.cpp:18-22。
- 删除的正确性风险：删除 dispatch/submit/补投段则 submit 失败遗留与 pending intent 永不重试（EAGAIN/EBUSY 后请求滞留队列）。
- 裁决：KEEP

---

### E24 进度唤醒（signal_ready_progress → ProgressSource::signal → eventfd write）
- 锚点：src/async/uring_backend.hpp:297-299；src/async/async/detail/progress_source.hpp:252-264（signal：mutex + 饱和 epoch 递增 + wake_notification_nolock_）、466-489（eventfd write(2)，pending 位合并、EAGAIN 即已就绪）、74-79（eventfd 生命周期）；后端注册面 uring_backend.cpp:259-283（io_uring_register_eventfd）
- 语义义务：把"有可推进的工作/事实变化"通知进度所有者；epoch 饱和而非回绕，令旧 token 永不错配新 epoch（:253-263 注释）。
- 当前操作：mutex → epoch 递增（未 pending 时 1 次 eventfd write(2) 系统调用）。
- CPU工作：1 mutex + O(1) + 条件 syscall；合并：已有 pending 未读时跳过 write。
- 内核工作：write(2) 到 eventfd（计数器 +1，唤醒 poller）；以及 io_uring 完成侧内核对同一 eventfd 的通知注册（uring_backend.cpp:276）。
- 内存搬移：8B 计数器写。
- 同步：ProgressSource::mtx_；与完成侧共享同一 notification fd（后端只注册不读写 fd——BackendProgressPort 契约，progress_source.hpp:524-545）。
- 冗余假设：本段提交侧信号点（dispatch 重试广告/毒化/取消/零长发布）都是"userspace 控制面变化"；内核完成侧的唤醒由 io_uring eventfd 直达，不经 signal()——两路同一 fd 是唤醒域统一的设计，非重复。
- 证据：progress_source.hpp:252-264,466-489,74-79；uring_backend.cpp:259-283；触发点 uring_transport.cpp:100,523,603,614；uring_backend.cpp:574,698,701。
- 删除的正确性风险：删除信号面会让睡眠中的 owner 在"提交失败重试/毒化/零长完成"上错过唤醒（PROG-02 停车义务）。
- 裁决：KEEP

---

### E25 构造期能力探针（opcode + statx 实弹探针）
- 锚点：src/async/uring_backend.cpp:161-208（构造序列）、210-231（probe_required_opcodes_：4 opcode 探测）、233-256（probe_statx_support_：真实 statx SQE submit_and_wait + peek_cqe + cqe_seen）；开关 xmake/uring.lua:44-82
- 语义义务：required-profile 能力证据：构造时证明 READ/WRITE/FSYNC/STATX opcode 与 statx 机制在真实内核上可用，不可用即抛（不静默降级，uring_backend.hpp:15-18 契约）。构造期一次，非热路径。
- 当前操作：io_uring_get_probe_ring + 4 次 opcode_supported + free；一次真实 statx 提交/等待/消费。
- CPU工作：一次性：1 次额外 enter（statx 探针）+ probe 分配/释放。
- 内核工作：1 次 io_uring_enter（statx：AT_FDCWD "."、STATX_TYPE）→ vfs_statx。
- 内存搬移：栈上 statx 探针缓冲（~256B）。
- 同步：构造期单线程。
- 冗余假设：statx 探针与 opcode 探针部分重叠（statx opcode 已在必查表）——但 statx **机制**（AT_EMPTY_PATH 语义、掩码回填）在旧内核即使 opcode 可用也可能不工作，探针是独立能力面（uring_backend.cpp:198-206 的独立错误消息印证）。fail-fast 注入点（:88,195-199 的 mutant 编译开关）表明测试依赖其可单独失败。
- 证据：uring_backend.cpp:161-256；xmake/uring.lua:44-82。
- 删除的正确性风险：删除探针会把 required-profile 的建立失败推迟到首次提交（契约要求构造期显式失败）。
- 裁决：KEEP

---

### E26 内核物理边：io_uring 消费 → VFS/PageCache（按 opcode，非仓库代码）
- 锚点：SQE 来源 uring_transport.cpp:166-187；ring 参数 uring_backend.cpp:171（flags=0，无 SQPOLL/IOPOLL/fixed files/buffer ring）；本会话无法引用内核源码——以下为 io_uring_enter(2) 与 fs/io_uring 架构的 UAPI 语义，**未在本会话执行验证**。
- 语义义务：内核对每 SQE 的物理执行与完成回执（CQE res/user_data），是全部完成侧义务的物理来源。
- 当前操作（内核，按 opcode）：
  - IORING_OP_READ（file-_regular，缓冲 I/O）：VFS `read_iter` → filemap：逐 folio 查 PageCache——命中：folio→用户缓冲 CPU 复制（res 字节）；未命中：回填（readahead/bio→块层），完成可异步，CQE 携带实际字节（短读/EOF）。
  - IORING_OP_WRITE：VFS `write_iter` → 用户缓冲→PageCache folio 复制、置脏、balance_dirty 节流；CQE 在页缓存复制完成后即发（**不代表落盘**）；落盘由内核回写线程随后进行。
  - IORING_OP_FSYNC（fsync_flags：DATASYNC 或 0）：回写 + 设备 flush，CQE 在持久化后——本段最重内核工作。
  - IORING_OP_STATX（AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW）：按 fd 取 inode 属性，按掩码回填用户 statx 缓冲（ Completion 侧按 stx_mask 校验，uring_completion.cpp:175-192）。
  - IORING_OP_ASYNC_CANCEL：按 user_data 找在途请求取消 kiocb；原操作 CQE -ECANCELED（ECANCELED 映射 canceled，uring_internal.hpp:65-67）。
- CPU工作（内核）：每操作 fd→file 引用、缓冲 IO 向量校验、页表/ folio 管理、复制循环；这些是内核职责，本段用户态无法减少（除 E14 注记的 fixed files/buffer ring 能力面）。
- 内存搬移：READ：存储→PageCache（DMA）+ PageCache→用户（CPU，res 字节）；WRITE：用户→PageCache（CPU）+ 回写；STATX：~256B statx 结构→`statx_buffers_[slot]`（预分配，uring_backend.cpp:164,484-486）。
- 同步：内核与用户经 SQ/CQ 共享环 + khead/ktail 发布关系（liburing 侧 acquire/release 对应 liburing.h:1683-1697、1755-1759）。
- 冗余假设：无用户态可删项；观察点：WRITE 的 CQE≠落盘、FSYNC 的重内核代价——任何"省 enter/省 fsync"类假设须以测量说话（本会话不跑基准）。
- 证据：uring_transport.cpp:166-187；uring_completion.cpp:161-192（回程消费）；liburing.h:1020-1054；内核行为：io_uring UAPI（未运行验证）。
- 删除的正确性风险：物理边不可删；误配（如把 WRITE 的 CQE 当落盘证明）是语义风险而非冗余。
- 裁决：KEEP

---

## 6. 最值得追问的冗余假设（≤3）

1. **提交校验双层重复（E02/E03）**：`AsyncIoContext::initiation_rejection`（async_io_context.cpp:24-26，六处入口各一次）与 `UringAsyncBackend::validate_*`（uring_backend.cpp:94-123）对同一 (fd<0, access, op) 每提交执行两遍同一 constexpr `precheck_state_op`；state-op 增量为零，data-op 增量仅为 range+空指针。值得追问：后端层校验是否只为 seam 测试直连而保留（AsyncBackend 是未安装 seam，async_backend.hpp:17-19），能否收敛为单层。
2. **dispatch 热路径 remove_exact 的 O(n) 前移（E12）**：两个 dispatch 调用点都传 `front()`（uring_transport.cpp:82、uring_backend.cpp:624），remove_exact 必中 i=0 后仍把整队 (size-1)×16B 前移一格（uring_internal.hpp:118-133）——每派发一个操作付 O(队列深) 搬移，与 O(1) `pop_front` 完全等价。值得追问：机器测量下深队列派发的 memcpy 占比。
3. **生产只写不读的原子计数（E17，低优先同类 E15）**：`submit_flushes_` 在每次 `io_uring_submit` 前 relaxed fetch_add（uring_transport.cpp:222），唯一读者 `submit_flushes_for_test()` 仅存在于 SLUICE_ASYNC_INTERNAL_TESTING 构建期间；生产构建为纯热路径写。同类：`live_cookies_`/`live_control_sqes_` 的全部生产访问都在 dispatch_mtx_ 下（除两个无锁测试缝读者），原子实现冗余于互斥锁（x86 relaxed ≈ 普通访存，收益微小）。

## 7. 未执行/未验证声明

- 未编译、未运行任何测试/基准/探针（同机频率测量约束）；§3 的 grep 与 liburing 版本读取是本会话仅有的执行类证据。
- E26 全部内核侧行为是 io_uring UAPI 语义描述，非本会话对内核源码或运行的验证。
- liburing 以系统头 2.13 为引用基准（`/usr/include/liburing/io_uring_version.h:5-6`）；构建期实际解析的 2.13/2.14（xmake 缓存存在 2.14 目录）由 `xmake/uring.lua:203-299` 的 probe 决定，本会话未运行该 probe。
- 裁决基于源码结构与机制门槛论证，均未经机器测量证实；ELIMINATE_CANDIDATE 仅为下一步测量的追问对象。
