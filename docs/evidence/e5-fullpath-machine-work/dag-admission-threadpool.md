# DAG 分段机工记录：应用入口→校验→RequestCore 准入→后端分发→ThreadPool 队列/worker/pread/pwrite

- 撰写：2026-11-18，DAG 读者子代理（admission-threadpool 段）。
- 方法：纯源码阅读。未编译、未运行任何测试/基准/构建（同机频率测量进行中，任务约束）。
  本次执行的命令仅为 `find`、`grep`、`sed -n` 文件定位与交叉引用核对，不产生任何仓库写入；
  本文件是本段唯一写入物。
- 基线：master `5d94f233`（repo-reality.md 冻结的同一基线）。
- 以当前源码为唯一事实；文档/注释声明一律逐条对到代码后才采信，不符单独列于 §6。

## 0. 段事实基线（先于边记录的三个构建/结构事实）

| # | 事实 | 证据 |
|---|---|---|
| F1 | 生产库目标 `sluice_async` **不定义** `SLUICE_ASYNC_INTERNAL_TESTING`；只有 tests.lua 的各测试目标定义它 | `xmake/libraries.lua:34-56`（无该 define）、`xmake/tests.lua:320,402,441,495,553,577,614,738,811,884,1067,1122,1202,1225`、`xmake/apps.lua:4`（注释明示 apps 无该宏）。因此 `threadpool_backend.cpp`/`submit_request`/`worker_loop`/`poll_progress` 里全部 `wait_*_pause_()`、`injected_precommit_stage_failure_`、`dispatch_failure_injection_` 分支在生产构建中编译为空，机工记零 |
| F2 | 调用链：app → `RequestScope::submit_read/write` → `AsyncIoContext::submit_*` →（虚调用）`ThreadPoolBackend::submit_request` → `core_->reserve/accept` → `dispatch_after_accept` → worker `run_syscall`(pread/pwrite) → `offer_terminal/release_execution` → `publication_pending_` → 驱动方 `poll_progress` → `publish_one/deliver_event` | `apps/sluice-copy/copy_task.cpp:70,82,299`；`src/async/request_scope.cpp:137-161`；`src/async/async_io_context.cpp:249-269`；`src/async/threadpool_backend.cpp:211-297,386-479,550-612` |
| F3 | 三把锁的层级与顺序（全部路径实测）：`AsyncIoContext::access_mtx_`（上下文序）→ `ThreadPoolBackend::work_mtx_`（队列/投递序）→ `RequestCore::mutex_`（槽位态）；另有 `ProgressSource::mtx_`（独立叶域，eventfd 的唯一用户态写入者约束） | 提交链 `async_io_context.cpp:255→259→threadpool_backend.cpp:228,281`（access→core）；worker `threadpool_backend.cpp:394→406`（work→core）；驱动 `threadpool_backend.cpp:561→589→358`（work 释放后→core）；signal 叶域 `progress_source.hpp:252-264` |

调用次数≠指令数的三处内联/编译期事实（后文边记录引用）：
- `validate_op`（threadpool_backend.cpp:195-209）是 `if constexpr` 模板：运行期零分发，`validate_read/write/...` 内联进 `submit_request`。
- `retry_on_eintr`（posix_retry.hpp:7-15）是模板：lambda 内联，非 EINTR 时循环体执行一次（一次比较的开销）。
- `precheck_data_op/precheck_state_op/rejection_of/accept_or_reject`（file_semantics.hpp:131-141,157-185）全 `constexpr`：纯分支，无调用开销。

---

## 1. 应用入口（RequestScope → AsyncIoContext::submit_*）

### E-01 RequestScope 提交入口与槽位预留
- 锚点：`src/async/request_scope.cpp:137-161`（read）、`:163-187`（write）、槽位扫描 `:46-55`
- 语义义务：把一次性 Request 责任放进 scope 的固定容量槽位表；scope 槽耗尽时返回 `would_block`，不向下游传递压力。
- 当前操作：`reserve_slot_` 线性扫描 free 槽 → `ctx_.submit_read/write` → `commit_`。
- CPU：O(capacity) 扫描（每个槽一次状态比较；提交高峰最坏 capacity 次比较/提交，本基准配置 depth≤64）；`Request` 移动三指针交换（request.hpp:110-112）。
- 内核：无。
- 内存：无堆分配（`slots_` 为 `make_unique<Slot[]>` 一次性，request_scope.cpp:34）。
- 同步：无锁（scope 单线程使用约定，未加锁保护 slots_）。
- 冗余假设：scope 槽位扫描可用 free-list 代替 O(capacity) 扫描；但 depth 小且该边每提交才走一次，收益未测。
- 证据：`request_scope.cpp:46-55,143-152`；app 深度来自 `copy_task.cpp:299`（ThreadPoolConfig{64, workers}）。
- 删除风险：删除预留即丢失 scope 的"容量上限内不丢责任"语义，`settle_and_release_` 的未决槽枚举（:91-99）失去依据。
- 裁决：KEEP

### E-02 AsyncIoContext 启动前校验（initiation_rejection）
- 锚点：`src/async/async_io_context.cpp:249-253`（read）、`:271-275`（write）、状态操作 `:292-294,312-314,332-334,352-354`；实现 `:24-26`；`include/sluice/detail/file_semantics.hpp:178-185,131-135`
- 语义义务：在进入后端之前统一拒绝已关闭 fd / 访问权不符的启动（File 语义跨调用形态与后端共享的统一守门）。
- 当前操作：`precheck_state_op(fd<0, access, op)` 两分支 + `rejection_of` 包装。
- CPU：内联 constexpr，约 2-4 次比较；锁外执行。
- 内核：无。内存：无。同步：无。
- 冗余假设：对 ThreadPoolBackend 该检查与后端校验（E-07/E-08）的 closed/access 分支**完全重复**且为后者子集（`precheck_data_op` 含 closed+access+range+length，file_semantics.hpp:157-176）；仅就 threadpool 路径删掉它可观测结果不变。删除的真正约束是它是**后端无关**的统一守门——`AsyncBackend` 接口不要求后端自校验（async_backend.hpp:74-79 的 submit 纯虚无校验义务）。
- 证据：`async_io_context.cpp:24-26` vs `threadpool_backend.cpp:135-149,151-169`。
- 删除风险：任何不自校验的后端会让 closed-fd op 进入 reserve/accept 并占用槽位后才在 worker 侧失败；跨后端 File 语义不再由上下文保证。
- 裁决：KEEP（重复是每次提交几次 constexpr 分支；如做删除须先给 AsyncBackend 补校验义务条款）

### E-03 提交临界区：access_mtx_ 贯穿后端分发
- 锚点：`src/async/async_io_context.cpp:254-258`（read 锁+null 检查）、`:259`（虚分发）、write `:276-279`
- 语义义务：把 reserve→accept→入队→（零长）发布串行化进上下文 admission 序；`SLUICE_THREAD01_MUTANT_SUBMIT_UNSERIALIZED` 变体证明该串行化是合同性的（条件编译标记，:254-256）。
- 当前操作：`lock_guard<mutex>(access_mtx_)` → `backend_->submit_read/write(...)` 虚调用。
- CPU：锁获取（无竞争 ~20 cycle，竞争走 futex）；虚调用间接跳转；`backend_` null 分支。
- 内核：仅竞争时 futex。内存：无。同步：access_mtx_ 串行化 submit/cancel/lookup/observer/outstanding/stats 全部上下文操作；**注意** `work_cv_.notify_one()`（E-15）与零长路径的 eventfd write（E-14）都发生在此锁内。
- 冗余假设：无（该锁的保护对象是"admission 与 settle 冻结窗口"，见 `:86-89` 注释与析构路径 `:78-93` 的同一锁配合）。
- 证据：`async_io_context.cpp:254-259,86-93`。
- 删除风险：accept 与 close_admission（析构/stop 同锁）之间出现 late-accept 被冻结进 settled 集、槽位在 live Request 下被回收——正是 `:86-89` 注释描述的丢失窗口。
- 裁决：KEEP

### E-04 统计计数：tally_submit + max_outstanding（stats 挂载时）
- 锚点：`src/async/async_io_context.cpp:229-244,260-263`；下游 `threadpool_backend.cpp:820-822`；`request_core.cpp:93-96`
- 语义义务：AsyncStats 的提交侧证据（submit_calls/submitted_ops/queue_full_retries/max_outstanding）。
- 当前操作：stats_ 非空时分支计数 + `backend_->outstanding()` 虚调用（→ `outstanding_summary()` 取 core 锁）。
- CPU：stats_==nullptr 时整条边为两次可预测分支（app 默认如此：`copy_task.cpp:299` 不传 stats）。stats 挂载时：access_mtx_ **之内**再取一次 core mutex。
- 内核：无。内存：无。同步：stats 挂载时 access_mtx_→core mutex 嵌套。
- 冗余假设：max_outstanding 的每提交采样不可事后推导（需要逐事件峰值），但可用原子派生计数替代锁内查询——属于派生摘要机制变更，需按 AGENTS 机制门评估。
- 证据：`async_io_context.cpp:260-263`（条件在 stats_）；`measurement.hpp:83-97`。
- 删除风险：删除统计即删除可复现证据面（AGENTS 允许保留有消费者的诊断）。
- 裁决：KEEP

### E-05 Request 句柄构造
- 锚点：`src/async/async_io_context.cpp:266-268`；`include/sluice/async/request.hpp:174-175`
- 语义义务：把 (core, backend, RequestId) 唯一责任绑定交给调用方。
- 当前操作：三成员私钥构造（编译器内联，3 次拷贝）。
- CPU：~几条 store。内核：无。内存：栈上 24B。同步：无（结果身份已在 core 锁内确定）。
- 冗余假设：无。
- 证据：`request.hpp:174-175,211-213`。
- 删除风险：无（结构必需）。
- 裁决：KEEP

### E-06 状态操作提交家族（sync_data/sync_all/file_info/size）
- 锚点：`src/async/async_io_context.cpp:291-309,311-329,331-349,351-369`
- 语义义务：与 E-02/E-03 同型，无 zero-op 内联分支；`submit_size` 的启动校验按 `FileOperation::file_info` 校验（`:352`），与后端 `validate_size` 一致（threadpool_backend.cpp:166-169）。
- CPU/内核/内存/同步：同 E-02/E-03；每提交一次 access_mtx_。
- 冗余假设：四个函数体重复同一模板形状（可模板化收编）；纯代码体积问题，非机工问题。
- 证据：`:291-369`。
- 删除风险：无（各自公开 API）。
- 裁决：KEEP

---

## 2. 校验（ThreadPoolBackend::validate_*）

### E-07 数据操作校验 validate_read / validate_write
- 锚点：`src/async/threadpool_backend.cpp:135-141,143-149`；判定核心 `file_semantics.hpp:157-176`（closed→access→length==0→range→kMaxNativeTransfer）；分发 `threadpool_backend.cpp:195-209`（if constexpr，零运行期开销）
- 语义义务：启动前一次性裁定 execute / complete_empty（零长） / reject_closed / reject_access / reject_range；零长 op 在这里放行为 `zero_op` 快完成路径（E-14）。
- CPU：全 constexpr 分支内联，~6-10 次比较；`op.dst/src==nullptr` 一次比较（:139,146）。
- 内核：无。内存：无。同步：无（在 core 锁之前、work 锁之外执行）。
- 冗余假设：与 E-02 的 closed/access 重复（见 E-02 假设）；range/length/null 检查为本层独有，不冗余。
- 证据：`threadpool_backend.cpp:135-149` vs `async_io_context.cpp:24-26`。
- 删除风险：range 越界 offset 会以截断 `off_t` 直达 pread/pwrite（worker 信任提交侧校验，`threadpool_backend.cpp:488,502` 的 cast 无二次防线）；null 缓冲会传给内核。
- 裁决：KEEP

### E-08 状态操作校验 validate_sync×2 / file_info / size
- 锚点：`src/async/threadpool_backend.cpp:151-169`
- 语义义务：closed/access 校验（无 range 维度）；size 复用 file_info 操作语义。
- CPU：constexpr 分支。内核：无。内存：无。同步：无。
- 冗余假设：与 E-06 启动校验重复 closed/access（同 E-02）。
- 证据：`:151-169`。
- 删除风险：同 E-02。
- 裁决：KEEP

---

## 3. RequestCore 准入（reserve → prepared → accept / rollback）

### E-09 core_->reserve()
- 锚点：`src/async/threadpool_backend.cpp:228-239`；`src/async/detail/request_core.cpp:165-182`
- 语义义务：槽位预留（reserved 相），返回 (slot, generation) 预订票；admission 关闭→invalid_state，槽耗尽→would_block（threadpool_backend.cpp:230-237 的映射）。
- 当前操作：core mutex 下：admission_open_ 检查 → free_slots_（LIFO vector）pop → slot.phase=reserved。
- CPU：锁 + ~5 次比较 + vector pop（无分配：构造时 `reserve(capacity)` 且每个槽至多一个票在册，free_slots_ 不会超容量重分配——request_core.cpp:41-44,621-630）。锁外仅错误码映射（threadpool_backend.cpp:230-237）。
- 内核：无。内存：无堆分配。同步：core mutex；运行于 access_mtx_ 内（嵌套序 access→core）。
- 冗余假设：LIFO free-list 复用刚释放槽——对 cache 局部是收益不是冗余；无假设。
- 证据：`request_core.cpp:41-44,165-182`。
- 删除风险：即失去 admission/容量两种拒绝语义，RequestCore 的所有 resolve 路径以 phase+generation 为根基。
- 裁决：KEEP

### E-10 prepared_ops_[slot] 就地写（锁外）
- 锚点：`src/async/threadpool_backend.cpp:249-259`
- 语义义务：把 (kind, fd, buffer, length, offset) 放进 worker 将按槽读取的准备表；zero-op 标志在此从 length==0 派生。
- 当前操作：`length/offset/zero_op` 计算（数据 op 才有）+ PreparedBlockingOp 5 字段 store。
- CPU：~5-8 条 store/比较。内核：无。内存：~40B 槽内 store。
- 同步：**本边无锁**；可见性由 E-15 的 dispatch push（work_mtx_ 释放）与 worker 出队（同锁获取）构成的 mutex handoff 提供——写入在 `submit_request` 程序序中先于 push（:257 先于 :324）。
- 冗余假设：core 槽位里还存了一份 BorrowFacts（E-12），fd/buffer/length 双份；见 E-12 假设。
- 证据：`:249-259`；worker 读 `:409`。
- 删除风险：worker 失去操作载荷来源。
- 裁决：KEEP

### E-11 DeliveryRecord 初始化（work_mtx_）
- 锚点：`src/async/threadpool_backend.cpp:261-267`
- 语义义务：清零该槽的投递记录（kind/event_owed=false/owed_key），建立"提交前记录归零"的投递不变量（记录由提交线程在锁内初始化、驱动线程消费，threadpool_backend.hpp:129-133 的锁域声明）。
- 当前操作：work_mtx_ 下 3 字段 store。
- CPU：锁 + 3 store。内核：无。内存：16B 槽内。同步：work_mtx_。
- 冗余假设：记录里 kind 对数据 op 与 prepared_ops_.kind 冗余（同一值存两处）；但投递事件需要 kind 且 owed 路径（E-31）不经过 prepared_ops_，两消费者两份是各取所需，不判冗余。
- 证据：`:261-267`；投递读 `:361-365`、`:570-582`。
- 删除风险：投递事件失去 kind 来源；记录复用后残留上一请求状态。
- 裁决：KEEP

### E-12 core_->accept()（含 BorrowFacts 存储）
- 锚点：`src/async/threadpool_backend.cpp:279-285`；`src/async/detail/request_core.cpp:184-210`
- 语义义务：把预订票升级为 accepted 相：写描述符/借约事实、置 binding_live、execution_refs=1、++outstanding_；零长 op 在此直接选定终态（`terminal_chosen`、outcome=success(0)、execution_refs=0，:203-207）。
- 当前操作：core mutex 下 resolve_reserved_（3 检查）→ admission 复查 → ~12 字段 store → 计数。
- CPU：锁 + ~20 条 store/比较。内核：无。内存：descriptor 32B + borrow 24B + outcome 初始化，槽内 ~80B store。
- 同步：core mutex（access_mtx_ 内嵌套）。
- 冗余假设（段内最重之一）：**`Slot::borrow` 只写不读**——`slot->borrow = borrow`（request_core.cpp:200）与 `slot.borrow = {}`（:614）之外，src/、include/、tests/ 全部源码（grep 核对）无任何读者；`SlotObservation`/`CoreSnapshot` 也不含它（request_core.hpp:231-253）。后端各存自己的操作载荷（prepared_ops_ / uring SQE）。而 root contract 把 "borrow facts" 列入请求记录域（docs/explicit-io-contract.md:586），AGENTS 亦禁止"因无消费者而删义务"。需要裁决：它是等待消费者的 root 义务存储，还是可随 root 修订移除的死存储。
- 证据：`request_core.cpp:200,614`；grep "borrow" 于 src/include/tests 仅命中写/清/类型声明；`docs/explicit-io-contract.md:586`。
- 删除风险：若 borrow facts 是结算期借用审计的既定义记录组件，删除即丢义务；若确为死存储，删除省每 accept 24B store 与槽空间。
- 裁决：KEEP（存储边）/ 冗余假设 UNPROVEN

### E-13 失败回滚 rollback（错误路径）
- 锚点：`src/async/threadpool_backend.cpp:282-284`；`src/async/detail/request_core.cpp:212-224,599-631`
- 语义义务：accept 被拒（bad_reservation / admission_closed）或注入失败时把 reserved 槽退回 free 相（generation+1、free_slots_ 回推、~15 字段清零）。
- 当前操作：core mutex 下 release_slot_。
- CPU：锁 + ~15 store（仅错误路径执行；生产路径 accept 拒绝的合法来源是 reserve 与 close_admission 之间的窗口——request_core.cpp:192-194）。
- 内核：无。内存：槽内清零。同步：core mutex。
- 冗余假设：无——保护的是真实交叠（另一线程在 reserve 后、accept 前关闭 admission）。
- 证据：`request_core.cpp:188-194,212-224`；析构/stop 关闭点 `async_io_context.cpp:91,141,451,484,852`。
- 删除风险：关闭窗口期的失败 accept 留下永久 reserved 泄漏槽，容量不可恢复。
- 裁决：KEEP

---

## 4. 后端分发（dispatch）

### E-14 零长 op 内联发布 publish_zero_op_inline
- 锚点：`src/async/threadpool_backend.cpp:287-288,334-354`；core 侧 `request_core.cpp:411-422`（acquire_control）、`:438-460`（begin_publication）、`:462-484`（complete_publication）
- 语义义务：零长度数据 op 不进 worker：提交线程同步完成终态并登记 owed 事件（event_owed + owed_key + owed_count_++，配一枚 control 引用），下一次驱动 pass 投递（E-31）。
- 当前操作：acquire_control → begin_publication → complete_publication（三次独立 core mutex 临界区，每步 fail-fast 校验）→ work_mtx_ 下 owed 登记 → signal_ready_progress。
- CPU：3 次 core 锁 + 1 次 work 锁 + signal；每步间有 `threadpool_core_handoff_fail_fast` 判定。
- 内核：signal 触发 eventfd write（1 次 write(2)，若 notification_pending_ 未清）——**发生在 access_mtx_ 之内**（E-03 临界区未出）。
- 内存：outcome/metadata 经 begin_publication 的 out=nullptr 路径**不复制**（threadpool_backend.cpp:338 传 nullptr；消费者直接从槽读，E-30 无 payload 拷贝）。同步：core×3、work×1、ProgressSource::mtx_×1；handoff：eventfd。
- 冗余假设：三次 core 锁可在协议内融合为一次（三步同槽同线程、步间无回调），省 2 次锁往返/零长 op；但每步返回独立裁决值（PublicationGrant/Completion/布尔）且 fail-fast 语义绑定各步，融合属机制变更。零长 op 在真实负载中占比未测。
- 证据：`:334-354`；配对释放 `:594`（release_control）。
- 删除风险：取消零长路径则零长 op 进 worker 空转一次 pread(0)/pwrite(0)，吞吐与延迟都劣化且与"complete_empty 判定"（E-07）的语义分层冲突。
- 裁决：KEEP（融合假设 UNPROVEN）

### E-15 dispatch_after_accept：ring 入队 + notify_one
- 锚点：`src/async/threadpool_backend.cpp:299-332`（push 在 :324，notify 在 :330，锁作用域 :301-326）
- 语义义务：accepted 非零长 op 进入 dispatch ring，唤醒一个 worker；ring push 超容量 fail-fast（push_back :44-53——不变量：每槽至多一票在队，容量=槽容量，溢出即协议不可达）。
- 当前操作：work_mtx_ 下 `dispatch_.push_back(h)`（环形取模 + store）；解锁后 `work_cv_.notify_one()`。
- CPU：锁 + ~6 条指令 push + 解锁 + notify。内核：有等待者时 futex wake（1 次 syscall；无等待者为纯用户态）。内存：16B 票入 ring。同步：work_mtx_ + cv wake；**notify 在 access_mtx_ 内发生**（E-03）。
- 冗余假设：notify_one 在"一定有空闲 worker"时是冗余 futex 往返——但空闲判定本身需要持锁读 worker 状态，现状的 notify 代价已是最小形态；无假设。
- 证据：`:299-332,44-53`。
- 删除风险：worker 永不醒来，dispatch 积压直到 stop。
- 裁决：KEEP

---

## 5. ThreadPool 队列 / worker / pread / pwrite

### E-16 worker_loop：cv 等待与出队
- 锚点：`src/async/threadpool_backend.cpp:386-403`
- 语义义务：worker 休眠至有工作或停止；`stopping_ && dispatch_.empty()` 才退出（先排空后退出）。
- 当前操作：`unique_lock` + `work_cv_.wait(pred)`（伪唤醒循环）→ `pop_front`。
- CPU：空闲时一次 futex sleep；每出队一次锁往返 + ring pop ~6 指令。
- 内核：等待时 futex wait；唤醒时 futex wake（对称于 E-15）。
- 内存：16B 票出 ring。同步：work_mtx_ + cv。
- 冗余假设：无。
- 证据：`:388-403`。
- 删除风险：即删除 worker 线程本体。
- 裁决：KEEP

### E-17 claim_execution + 操作装载 + active_workers_++
- 锚点：`src/async/threadpool_backend.cpp:405-413`；`src/async/detail/request_core.cpp:367-378`
- 语义义务：执行权仲裁——已被取消（terminal_chosen）的出队票 claim 失败走 E-18；成功则 claim 置位 `execution_claimed`（封锁 E-36 的 won-before-execution 窗口）并装载 op。
- 当前操作：work_mtx_ 内嵌套 core mutex（work→core 序）：claim_execution → `op = prepared_ops_[slot]`（40B 拷贝）→ `++active_workers_`。
- CPU：2 锁 + ~15 指令。内核：无。内存：40B 拷贝。同步：work_mtx_、core mutex；claim/release 构成执行期 borrow-touch 配对（release 必须 borrow_touch_fully_retired，:457-460）。
- 冗余假设：claim 与 op 装载两次锁往返（work 已在手、core 再取一次）——core 态必须在 core 锁内读，融合会把 core 锁并进 work 锁破坏锁序；无假设。
- 证据：`:405-413`；`request_core.cpp:367-378`。
- 删除风险：无仲裁则取消与执行竞态：已取消 op 仍执行物理 I/O（违反 cancel 语义）。
- 裁决：KEEP

### E-18 retired_unclaimed 分支（出队票 claim 失败）
- 锚点：`src/async/threadpool_backend.cpp:415-424`
- 语义义务：worker 已 pop、取消方来不及 remove_exact 的交叠窗口：丢弃票（不执行 I/O）并 signal 驱动方，使 `internal_work_retired()` 的在途见证归零（:416-418 注释即此义务）。
- 当前操作：signal_ready_progress → continue。
- CPU：1 次 ProgressSource 锁 + 可能 1 次 eventfd write。内核：同 E-35。内存：无。同步：ProgressSource::mtx_ + eventfd。
- 冗余假设：无——删除则该交叠窗口的票既不执行也不产生"retired"见证，settlement 驱动会在 internal_work_retired 上永久等待。
- 证据：`:415-424`；`internal_work_retired` `:856-860`。
- 删除风险：见上；确定死锁/live-lock 风险。
- 裁决：KEEP

### E-19 run_syscall::read（pread）
- 锚点：`src/async/threadpool_backend.cpp:481-498`；重试 `posix_retry.hpp:7-15`；errno 映射 `error.hpp:66-94`
- 语义义务：物理读：`retry_on_eintr(::pread)`，n<0 时 `failed_dispatched_attempt(from_errno_value(errno))`（EffectCertainty::unknown——失败时已发生效果不明），否则 success(n)。
- 当前操作：errno=0 → 内联 lambda 循环调用 pread → 失败时线性映射表（8 项 kNativeErrorMappings）→ IoOutcome 构造。
- CPU：热路径 = 1 次调用帧 + 1 次比较（非 EINTR）+ Outcome 构造 ~几条指令；失败路径 + 映射扫描。锁：无。
- 内核：**pread(2) 一次**（EINTR 时重入；这是全段唯一必然内核数据通路）。
- 内存：内核→用户缓冲 op.length 字节（真实搬移；缓冲指针经 `const_cast<std::byte*>(p.buffer)` 去常量——读方向 dst 本可写，PreparedBlockingOp 用 const 指针统一存两方向，:487）。
- 同步：无。
- 冗余假设：errno=0（:483）是防御性赋值——失败路径必然由内核写 errno，成功路径不读它；删除它语义不变，但它是 1 条 store，无测量热点不动（AGENTS：无热点不立项）。
- 证据：`:483-497`。
- 删除风险：即删除物理读能力。
- 裁决：KEEP

### E-20 run_syscall::write（pwrite）
- 锚点：`src/async/threadpool_backend.cpp:499-514`
- 语义义务：物理写，同 E-19；失败同样 uncertain（ mutant `SLUICE_BACKEND_CONTRACT_MUTANT_WRITE_FAILURE_ACCOUNTED` 标记"accounted"是错误变体，:508-509）。
- CPU/内核/内存/同步：同 E-19（方向相反：用户→内核 op.length 字节）。
- 冗余假设：无。
- 证据：`:499-513`。
- 删除风险：即删除物理写能力。
- 裁决：KEEP

### E-21 run_syscall::sync_data / sync_all（fdatasync/fsync）
- 锚点：`src/async/threadpool_backend.cpp:515-526`
- 语义义务：持久化原语；失败映射 accounted（与数据 op 的 uncertain 不同——sync 无部分效果）。
- CPU：锁无；Outcome 构造。内核：fdatasync(2)/fsync(2) 一次（EINTR 重入）。
- 内存：无用户态搬移。同步：无。
- 冗余假设：无。
- 证据：`:515-526`。
- 删除风险：即删除持久化能力。
- 裁决：KEEP

### E-22 run_syscall::file_info / size（fstat）
- 锚点：`src/async/threadpool_backend.cpp:527-541`
- 语义义务：元数据原语；成功时把 st_mode/st_size/st_dev/st_ino 译成 FileInfo（kind/size/identity），经 candidate.metadata → offer_terminal 入槽（:451-453）。
- CPU：栈上 stat 结构清零（~144B memset）+ 4 字段翻译。内核：fstat(2) 一次。
- 内存：内核 statx 缓冲 → 栈 st → FileInfo ~40B 两跳拷贝。
- 同步：无。
- 冗余假设：无（stat 栈清零是 ::fstat 接口要求）。
- 证据：`:527-541`。
- 删除风险：即删除元数据能力。
- 裁决：KEEP

### E-23 syscall_count_ 递增（生产构建无读者）
- 锚点：`src/async/threadpool_backend.cpp:434`（fetch_add，**未被 SLUICE_ASYNC_INTERNAL_TESTING 包裹**）；成员 `include/sluice/async/detail/threadpool_backend.hpp:258`（同样未包裹）；唯一读者 `src/async/threadpool_test_seams.hpp:108-109`（在测试宏内，仅测试目标编译）
- 语义义务：（声称）syscall 计数证据；实际消费者全部经 `syscall_count_for_test()`，用于测试 oracle（如 `tests/threadpool_request_core_integration_test.cpp:380` "the physical syscall has completed"、`public_request_test.cpp:313` "no worker ever ran"）。
- 当前操作：每次成功执行 syscall 后一次 relaxed atomic RMW。
- CPU：x86 上 1 条 `lock xadd`（无论 relaxed 与否）+ 成员 cache 行占用；每个完成 syscall 一次。
- 内核：无。内存：8B 原子。同步：原子 RMW（无锁语义需求——无读者）。
- 冗余假设：**生产库里这是一条无读者的锁指令**：写侧未用测试宏门控，读侧在宏后。把 fetch_add 与成员一起移入 `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` 即删生产成本且测试 oracle 不变。
- 证据：cpp:434 与 hpp:258 在宏外、threadpool_test_seams.hpp:108-109 与 hpp:72 在宏内（grep 全量核对读者）；F1 构建事实。
- 删除风险：直接删除会断测试 oracle；正确形态是宏门控（成员+写侧与访问器同进同出）。
- 裁决：**ELIMINATE_CANDIDATE**（生产侧剔除，测试侧保留）

### E-24 active_workers_ 递减（work_mtx_）
- 锚点：`src/async/threadpool_backend.cpp:435-438`
- 语义义务：在途 worker 计数归位；与 E-17 的 ++ 构成 `internal_work_retired()` 的执行侧见证（:858）。
- CPU：1 锁 + 1 递减。内核：无。内存：8B。同步：work_mtx_。
- 冗余假设：可用原子代替锁内计数——但该计数与 dispatch_/publication_pending_/owed 的清空判定需要在**同一把锁下**联合成立（internal_work_retired 一次持锁四查，:856-860），原子化反而引入联合快照问题；无假设。
- 证据：`:435-438,856-860`。
- 删除风险：settlement 收敛判定失去执行侧事实，可能带着在途 worker 释放资源。
- 裁决：KEEP

### E-25 offer_terminal（终态选主）
- 锚点：`src/async/threadpool_backend.cpp:444-456`；`src/async/detail/request_core.cpp:346-365`
- 语义义务：把物理结果一次性选定进槽（terminal_chosen 单票：重复/过期/不可采纳各有拒绝裁决）；file_info/size 成功附 metadata。
- 当前操作：core mutex 下 resolve_internal_ + 裁决链 + outcome（~32B）/metadata（~40B，仅元数据 op）拷入槽。
- CPU：锁 + ~20 指令 + 拷贝。内核：无。内存：≤72B 槽内。同步：core mutex。
- 冗余假设：candidate 是栈上中间结构再拷入槽（一次中转拷贝）；可传引用就地写——但 offer_terminal 的裁决须先于写入（拒绝时不落任何字段），现状是裁决与写原子化的最简形态；无假设。
- 证据：`:444-456`；`request_core.cpp:353-364`。
- 删除风险：终态丢失/双写。
- 裁决：KEEP

### E-26 release_execution（borrow-touch 归还）
- 锚点：`src/async/threadpool_backend.cpp:457-460`；`src/async/detail/request_core.cpp:393-409,580-597`
- 语义义务：执行引用 1→0；与 terminal_chosen 同满时报告 borrow_touch_fully_retired（worker fail-fast 校验，:458-459）；try_reclaim_ 检查 6 项回收条件（此点 binding_live 仍活，通常不回收）。
- 当前操作：core mutex 下 refcount 递减 + reclaimable_ 六条件判定。
- CPU：锁 + ~10 指令。内核：无。内存：无（不回收时）。同步：core mutex。
- 冗余假设：无——六条件里每项对应一个独立在途责任（control 引用/发布在飞/观察者相位），AGENTS 明示"派生不等于可删"。
- 证据：`request_core.cpp:393-409,580-590`。
- 删除风险：执行引用泄漏 → settlement_converged_ 的 execution_refs==0（async_io_context.cpp:473-476）永不满足。
- 裁决：KEEP

### E-27 publication_pending_ 入队 + signal_ready_progress
- 锚点：`src/async/threadpool_backend.cpp:461-466`
- 语义义务：终态已定的票移交发布队列（驱动 pass 的 E-28 快照对象）；signal 通知驱动方有可发布工作。
- 当前操作：work_mtx_ 下 ring push（独立作用域，:461-464），**锁外** signal（:466）。
- CPU：锁 + push + signal 锁。内核：eventfd write（E-35）。内存：16B。同步：work_mtx_、ProgressSource::mtx_。
- 冗余假设：signal 与 push 之间无锁窗口——signal 先于驱动方看到 push 也不丢（驱动方 pass 会快照到，且下一次 signal 会补），eventfd 语义容忍合并通知（progress_source.hpp:26-29 "notifications may coalesce"）；无假设。
- 证据：`:461-466`。
- 删除风险：发布永不被驱动方取走。
- 裁决：KEEP

---

## 6. 完成发布 / 事件投递（驱动 drain——worker 产物的唯一出口）

### E-28 poll_progress：发布快照 + owed 扫描
- 锚点：`src/async/threadpool_backend.cpp:550-584`（快照块 :560-584；owed 扫描 :570-583，由 `owed_count_ != 0` 门控）
- 语义义务：以入口快照为 pass 边界：一次持锁取走当前全部待发布票与全部 owed 事件（上限各 capacity），pass 不吸收之后产生的消息（:551-558 注释即此义务）；publish_batch_/owed_batch_ 构造期预分配，pass 内零堆分配。
- 当前操作：work_mtx_ 一次持锁：pop 全部 publication_pending_ → publish_batch_；owed_count_ 非零时 O(capacity) 线性扫 delivery_ 收 event_owed 记录（清位、--owed_count_、拷出 key/kind）。
- CPU：1 锁 + O(pending) pop + （owed 非零时）O(capacity) 扫描——capacity 64 时上限 64 次分支；owed_count_ 门控是 #621 R1 已产品化机制（生产消费者：:570 门控 + owed_visible_locked_ hpp:205-211 用于 :608 残留判定）。
- 内核：无。内存：每事件 16B×2 拷贝。同步：work_mtx_ 一次。
- 冗余假设：delivery_ 的 O(capacity) 扫描即使被门控，在 owed 非零的 pass 仍整表扫——可用 owed 索引链替代；capacity 小（64）时无意义，属未测热点，不立项。
- 证据：`:550-584`；`threadpool_backend.hpp:240-244`（owed_count_ 派生摘要声明）；`docs/evidence/621-threadpool-owed-count-20261112/REPORT.md`（§5 GO 裁决，历史证据）。
- 删除风险：pass 边界破坏（无界吸收/有界谎报）；owed 事件漏投。
- 裁决：KEEP

### E-29 publish_one（锁外发布）
- 锚点：`src/async/threadpool_backend.cpp:356-374`；core 侧 begin/complete `request_core.cpp:438-460,462-484`
- 语义义务：发布三步：begin_publication 置 publication_inflight（封锁重复发布者与并发槽回收）→ 读 DeliveryRecord.kind（**须在 complete 之前**——complete 后槽可被回收并重初始化，:362-364 注释即此约束）→ complete_publication（published=true、--outstanding_、armed 观察者置 queued、try_reclaim_）。
- 当前操作：2 次 core mutex + 1 次记录字段读 + fail-fast 判定×2。
- CPU：2 锁 + ~15 指令。内核：无。内存：kind 1B 读；payload 不经 PublicationPayload 拷贝（out=nullptr，消费者读槽）。同步：core mutex×2；发布与投递的锁链见 F3。
- 冗余假设：begin/complete 两次锁往返同 E-14 的融合假设（UNPROVEN，此处步间夹着记录读与测试暂停点）。
- 证据：`:356-374`；`request_core.cpp:444-459,462-484`。
- 删除风险：发布丢失或双发布；complete 前读记录的约束破坏会读到重初始化后的 kind。
- 裁决：KEEP（融合假设 UNPROVEN）

### E-30 deliver_event：观察者 claim/retire + 惰性 sink
- 锚点：`src/async/threadpool_backend.cpp:376-384`；core 侧 `request_core.cpp:513-531,560-572`；sink `reference_ready_sink.hpp:9-28`；routing 注入仅测试 `include/sluice/async/detail/test_access.hpp:35`
- 语义义务：把 ready 事件按观察者协议投递：claim（queued→delivering，封锁交付期取消）→ `sink.on_ready` → retire（delivering→retired，解锁槽回收与 `delivery_retired()` 查询）。
- 当前操作：claim_observer_delivery（core mutex）→ 虚调用 on_ready → retire_observer_delivery（core mutex）。
- CPU：2 次 core mutex + 1 次虚调用。**生产里 sink_ 是 ReferenceReadySink（on_ready 生产构建为空体，reference_ready_sink.hpp:11-15），routing_sink_ 仅 TestAccess 注入**（grep 核对：生产 src 无 attach_ready_sink 调用方）。所以生产每完成事件固定 2 次 core 锁往返投递一个空回调。
- 内核：无。内存：ReadyEvent 16B 值传递。同步：core mutex×2。
- 冗余假设（段内最值得追问之二）：无观察者时 claim/retire 对是纯协议空转——complete_publication 已在 core 锁内计算过 `queue_armed_observer`（request_core.cpp:477-481），发布后不可能再有合法注册（register_observer 对 published 槽返回 already_terminal，request_core.cpp:504-508），因此"发布时无观察者 ⇒ 跳过 claim/retire"在协议内可证安全。但 on_ready 同时是 routing_sink 的公共交付点、delivering 相位是 cancel_observer/delivery_retired 的可观测状态，跳过属机制变更，须 measured profile + 单因素 A/B（history-digest §1.2 未决项 6 的 mutex family ~9-11% 候选与此直接相关）。
- 证据：`threadpool_backend.cpp:376-384`；`reference_ready_sink.hpp:9-28`；`test_access.hpp:35`；`request_core.cpp:477-481,504-508`。
- 删除风险：观察者交付协议（delivery_in_progress/retired 语义）失效；公共 attach_observer 的可观测行为改变。
- 裁决：KEEP（跳过假设 UNPROVEN）

### E-31 owed 事件投递 + release_control
- 锚点：`src/async/threadpool_backend.cpp:592-598`
- 语义义务：零长/取消胜出事件的补投递：deliver_event（同 E-30）→ release_control 归还配对 control 引用（E-14/E-36 各 acquire 一枚），fail-fast 校验。
- 当前操作：每 owed 事件 3 次 core mutex（claim/retire/release_control）。
- CPU：3 锁/事件。内核：无。内存：无。同步：core mutex×3。
- 冗余假设：release_control 独立第三次锁往返可与 retire 融合（同一槽连续两步 core 操作、无回调间隔）——同 E-14/E-29 融合家族，UNPROVEN。
- 证据：`:592-598`；配对 acquire `:335`（zero-op）与 `:792`（cancel 胜出不 push owed——注意 cancel 胜出只 push publication_pending_，control 引用由 worker lane/驱动经 publish_one→deliver_event 路径对上吗？见 E-36 证据行）。
- 删除风险：control 引用泄漏 → occupancy.control_refs ≠ 0 → settlement 不收敛（async_io_context.cpp:473-476）。
- 裁决：KEEP（融合假设 UNPROVEN）

### E-32 残留判定：immediate_work_remains + outstanding_summary
- 锚点：`src/async/threadpool_backend.cpp:604-611`
- 语义义务：pass 末尾的诚实残留报告：发布/投递**之后**重新持锁看队列是否又有存货（第二次 work_mtx_ 持有是有意的——发布在锁外进行，入口快照已失效）；accepted_work_remains 用 core 的 outstanding_summary。
- 当前操作：work_mtx_ 一次 + core mutex 一次。
- CPU：2 锁 + O(1)。内核：无。内存：无。同步：work_mtx_、core mutex。
- 冗余假设：与 E-28 入口锁合并会读到过期残留（发布发生在两锁之间），不冗余；outstanding_summary 用派生计数而非 occupancy 全扫，已是派生摘要的正确用法（request_core.hpp:180-183 声明其非权威性）。
- 证据：`:604-611`；`request_core.cpp:93-96`。
- 删除风险：wait_one/park 依据的 pass 报告失真（空闲误报/漏报）。
- 裁决：KEEP

---

## 7. 上下文驱动侧与信号通道

### E-33 run_progress_pass_（context 侧 pass 骨架）
- 锚点：`src/async/async_io_context.cpp:429-439`；exhaustion 检查 `:449-453`；`progress_source.hpp:266-269`
- 语义义务：一次驱动的统一封装：poll_calls 统计 → backend poll_progress → 健康合并（backend pass 或 core 或 progress 源任一 sticky 失败）→ exhaustion 触发关 admission。
- 当前操作：每次 pass 无条件执行：`core_->health_failed()`（core mutex）+ `progress_->wait_health_failed()`（ProgressSource mutex）+ `progress_->exhausted()`（ProgressSource mutex）。
- CPU：3 次互斥锁收购/pass（+E-34 的 access_mtx_×2 与 E-28/E-32 的 work/core 锁——pass 固定锁开销合计 ≥6 次）；计数 2 次。
- 内核：无。内存：ProgressPass 值拷贝。同步：core mutex×1、ProgressSource::mtx_×2。
- 冗余假设（段内最值得追问之三）：health_failed_ 与 progress_exhaustion_ 都是 sticky 标志（只在 note_health_failure/exhaustion 饱和时置位，永不复零——request_core.cpp:240-244；progress_source.hpp:244-263），却每 pass 花 3 次锁收购读它们。若以原子 sticky 标志承载（或一次融合查询返回三元组），pass 固定开销可减 2-3 次锁。属机制变更：ProgressSource 当前"一切经 mtx_"是其 eventfd 单读者协议的一部分（progress_source.hpp:412-414），改造须重新证明。对应 history-digest §1.2 未决项 6 的 mutex family 候选。
- 证据：`async_io_context.cpp:429-439,449-453`；sticky 性质 `request_core.cpp:240-244`、`progress_source.hpp:252-264`。
- 删除风险：健康判定漏检 → park 期间吞掉失败；exhaustion 不关 admission → epoch 饱和后信号失去新鲜度。
- 裁决：KEEP（原子化/融合假设 UNPROVEN）

### E-34 drive guard：drive_entry_admitted_ / drive_exit_
- 锚点：`src/async/async_io_context.cpp:179-196,441-447`（poll_progress）；wait_one `:595-598`；shutdown `:878-881`
- 语义义务：驱动域单飞与属主线程绑定（THREAD-01：首个 drive 或 claim 绑定属主线程；他线程/并发驱动拒绝）。
- 当前操作：access_mtx_ 下 4 判定 + drive_active_ 置位；DriveGuard 析构再取一次锁清位。
- CPU：2 次 access_mtx_ 往返/pass。内核：无。内存：无。同步：access_mtx_×2。
- 冗余假设：入口/出口两次锁往返可减为一次（exit 在锁内也只写一个 bool）——但 entry/exit 必须包住整个 pass，两次是结构必需；无假设。
- 证据：`:179-196,328-334`。
- 删除风险：并发 drive 破坏 pass 串行化与属主语义。
- 裁决：KEEP

### E-35 ProgressSource::signal + eventfd 唤醒
- 锚点：`src/async/threadpool_backend.cpp:614-616`（backend 端）→ `async_backend.hpp:93`（port 成员）→ `progress_source.hpp:528-532`（BackendProgressPort::signal）→ `:252-264`（signal）→ `:466-489`（wake_notification_nolock_）
- 语义义务：用户态进度纪元推进 + 内核通知：epoch 饱和推进（不回绕，防止活跃 token 与后续纪元别名）；pending 未读时**跳过 write(2)**（一个未读字节已使所有 poller 就绪）。
- 当前操作：ProgressSource::mtx_ 下 2 分支饱和递增 + 条件 write(2)。
- CPU：1 锁 + 2 分支。内核：非 pending 时 1 次 write(2)（EAGAIN 分支=已满=等价 pending，:483-486）；EINTR 重入。内存：8B 计数写。同步：ProgressSource::mtx_；eventfd 是 park 域（wait_if_unchanged E-33 之外的兄弟段）的唯一内核通知面。
- 冗余假设：notification_pending_ 去重（跳过 write）是 #621 r2 已产品化的内核工作削减；饱和分支 2 次比较/signal 是纪元不回绕义务的最小成本；无新假设。
- 证据：`:252-264,466-489`；port 封装 `:524-554`。
- 删除风险：丢唤醒（park 永眠）或纪元别名（陈旧 token 被当新鲜）。
- 裁决：KEEP

### E-36 cancel：ring 摘除 + core cancel + 终态注入
- 锚点：`src/async/async_io_context.cpp:372-396` → `threadpool_backend.cpp:816-818` → `:782-814`；ring 摘除 `:64-87`；core cancel `request_core.cpp:260-275`
- 语义义务：取消仲裁：queued 票 O(n) 精确摘除；won_before_execution 时注入 canceled 终态 + 归还执行引用 + 入发布队列；claimed 中途 → 物理中断不支持（threadpool 的 pwrite/pread 无法安全中断，:797-803）；removed_entry 的非胜出取消也要 signal（在途见证归零，:808-812）。
- 当前操作：work_mtx_ 一次：remove_exact（O(n) 扫描 + O(n) 前移搬移）→ core_->cancel（core mutex 嵌套）→ 胜出时 release_execution + publication push；锁外按裁决 signal。
- CPU：work+core 双锁；remove_exact 最坏 O(capacity) 比较 + O(capacity) 16B 前移。内核：signal 的 eventfd（条件）。内存：ring 内搬移 ≤ capacity×16B。同步：work_mtx_→core mutex（与 E-17 同序，无反转）。
- 冗余假设：O(n) 摘除可改为惰性失效（入队时标记、出队跳过）——但 removed_entry 的 signal 语义与 internal_work_retired 的队列空判定都依赖真实摘除（:808-812、:856-860），惰性化须给 ring 加失效计数机制；UNPROVEN。
- 证据：`:64-87,782-814`。
- 删除风险：取消竞态三态（won/requested/physical_unsupported）语义崩塌；控制引用/执行引用配对破坏。
- 裁决：KEEP（惰性化假设 UNPROVEN）

### E-37 close_admission 边（core 直连 + backend 旁路）
- 锚点：core 边 `src/async/detail/request_core.cpp:226-233`，调用点 `async_io_context.cpp:91,141,451,484,844,852`；backend 边 `threadpool_backend.cpp:834-845`（生产调用方 grep 核对仅 `tests/progress_source_ownership_test.cpp:468,481,495,496`）
- 语义义务：admission 单向关闭；backend 版额外在 open→closed 迁移时 signal 进度源。
- 当前操作：core mutex + 置位（core 边）；backend 边同 + signal_ready_progress。
- CPU：各 1 锁。内核：backend 边可能 1 次 eventfd write。内存：无。同步：core mutex / ProgressSource::mtx_。
- 冗余假设：**生产链从不经过 `ThreadPoolBackend::close_admission`**——context 六个关闭点全部直连 `core_->close_admission()` 并自行 `progress_->signal()`（async_io_context.cpp:842-847 与 backend 版逐句同构）。backend 版是公共头上的无条件声明（threadpool_backend.hpp:58），生产读者仅测试。它与 context 版是同一机制的两份实现，需要裁决保留哪份（或 backend 版降为测试 seam）。
- 证据：`async_io_context.cpp:842-847` vs `threadpool_backend.cpp:834-845`；调用方 grep（上文）。
- 删除风险：删除 backend 版断 `progress_source_ownership_test` 的 open→closed-signal 轨迹 oracle；两份并存的风险是行为漂移。
- 裁决：UNPROVEN（二选一裁决属所有权决定，非本段可单方裁决）

---

## 8. Direct I/O 对应实现（直呼形态；同一 File 语义的零机器底座）

### E-38 blocking read_at
- 锚点：`src/blocking_file.cpp:87-106`；校验 `:26-30` + `file_semantics.hpp:157-176`
- 语义义务：直呼定位读——同一段 precheck → pread → errno 映射，无锁无线程无队列。
- 当前操作：admit_data_op → complete_empty/reject 短路 → `retry_on_eintr(native_pread)` → from_errno_value。
- CPU：constexpr 校验 + 1 次调用帧；失败时映射扫描。锁：**无**。
- 内核：pread(2) 一次（EINTR 重入）。内存：内核→用户 op 字节（与 E-19 同一内核通路，同一语义域）。
- 同步：无。
- 冗余假设：与 E-19 相比，请求路径的全部增量机工（E-01~E-17, E-24~E-36 的锁/队列/信号）就是"请求形态相对直呼形态"的测量对象；本边是对照底座，无自身冗余假设。
- 证据：`blocking_file.cpp:87-106` vs `threadpool_backend.cpp:481-498`（同一 precheck/errno 语义两形态）。
- 删除风险：即删除直呼形态。
- 裁决：KEEP

### E-39 blocking write_at
- 锚点：`src/blocking_file.cpp:108-127`
- 语义义务：直呼定位写，同 E-38 镜像。
- CPU/内核/内存/同步：同 E-38（方向相反）。
- 冗余假设：无。
- 证据：`:108-127`。
- 删除风险：即删除直呼写。
- 裁决：KEEP

### E-40 组合原语 compose：write_all_at 逐步重校验（与 read_exact_at 不对称）
- 锚点：`src/blocking_file.cpp:299-333`（compose 循环）、`:336-350`（read_exact_at 用 `prevalidated_pread_step`，:275-285，免重校验，:274-277 注释给出"入口已证整段合法，子步 off_t cast 无损"论证）、`:352-366`（write_all_at 每步调 `write_at` 全量重校验）
- 语义义务：组合读全/写满：入口一次 admit，逐步推进 confirmed_bytes；零进度停（eof_before_full / write_no_progress）、超量停（impossible_count）。
- 当前操作：每步 admit_data_op（write_all_at 路径）+ pread/pwrite + compose 状态机。
- CPU：read_exact_at 每步 ~0 次校验分支（免重校验）；write_all_at 每步多付 ~10 次 constexpr 校验分支 + 1 次 null 检查。锁：无。内核：每步 1 次 pwrite(2)（短写循环必需）。内存：每步 subspan 头构造。同步：无。
- 冗余假设：**不对称本身**：入口已验证 (offset, total) 合法 ⇒ 每个子步 (offset+confirmed, total-confirmed) 是其子区间必然合法（与 read_exact_at 侧 :274-277 注释同一论证），write_all_at 的逐步重校验在该论证下是冗余纯 CPU。同文件已有免重校验先例（prevalidated_pread_step），补一个 `prevalidated_pwrite_step` 是有先例的最小机制；改动仅直呼形态、无跨层影响。
- 证据：`:274-285,336-350,352-366` 逐行对照。
- 删除风险：若无入口校验先行的前提重构（例如 compose 改为可直接被外部调用），逐步校验是唯一防线；故删除必须以"compose 仅由已校验入口进入"为不变量前提。
- 裁决：**ELIMINATE_CANDIDATE**（CPU-only、同文件先例、无锁无内核影响；按 AGENTS 仍需 measured hotspot + 机制门后方可动）

---

## 9. 文档/注释与代码的核对结果（不符单独列出）

逐条核对过的注释/文档声明（均**一致**，此处登记核对动作本身）：
- `threadpool_backend.hpp:129-133`："投递记录经 work_mtx_ 交接、on_ready 不在锁内运行、worker 不触碰投递记录" —— 代码核对一致（deliver_event 仅在锁外被调 :373,593；worker 只触 prepared_ops_/dispatch_/publication_pending_）。
- `threadpool_backend.hpp:240-244`：owed_count_ 是派生摘要、结算路径保留权威扫描 —— 一致（:570 门控 + :608 owed_visible_locked_ vs :847-854 权威扫描）。
- `request_core.hpp:180-183`：outstanding_summary 非权威、occupancy 是 —— 一致。
- `async_io_context.hpp:342-345`：stop_policy_ 可无锁读取 —— 一致（drive_settlement_ :487,506 atomic acquire 读）。
- `progress_source.hpp:412-414`：非信号量 eventfd 读整计数 ⇒ 成功读即排空态 —— 一致（write 恒为 1，:471-476）。

发现的不符/偏差（三类）：
1. **注释政策偏差（AGENTS.md "C++ comments" 条 vs 代码）**：`async_io_context.cpp:18-22` 注释引用需求 ID（PROG-02）；`threadpool_backend.hpp:129-133,240-244`、`progress_source.hpp:25-29,37-41,96-103,271-273`、`request_core.hpp:180-183` 为语义叙述型注释。AGENTS 明文禁止注释叙事语义/引用需求 ID。属政策与代码的偏差，非行为不符；裁决归文档纪律流程，本段仅登记。
2. **声明-使用错位（生产 API 无生产消费者）**：`ThreadPoolBackend::close_admission`（hpp:58 公共声明）生产链零调用（E-37）；`syscall_count_` 成员与写侧无宏门控而读者全在测试宏后（E-23）；`dispatch_occupancy()/active_workers()`（hpp:62-65 无条件公共声明）grep 全仓仅两个测试文件调用（tests/backend_contract_conformance_test.cpp:126、tests/public_request_test.cpp:117）。三者是"头文件声明为生产能力、实际消费者只有测试"的错位，非行为 bug。
3. **无行为级不符**：本段范围内未发现文档声明与代码行为相矛盾的实例（上表 5 项注释声明全部核对通过）。

段边界登记（属相邻段，本段不裁决）：`AsyncIoContext::wait_one` 在 `:581-587` 与 `:589-593` 存在**背靠背两次 access_mtx_ 临界区**（第一次查 execution_closed_，第二次仅 ++wait_calls），中间无任何工作，合并为单一临界区行为等价——归 park/wait 段的 DAG 读者裁决。

## 10. 裁决汇总

| 裁决 | 边 |
|---|---|
| KEEP | E-01 ~ E-22, E-24 ~ E-30, E-31 ~ E-36（KEEP 边内标注的融合/原子化/惰性化假设均为 UNPROVEN，未据此改判） |
| ELIMINATE_CANDIDATE | E-23（syscall_count_ 生产侧宏门控剔除）、E-40（write_all_at 逐步重校验，同文件先例） |
| UNPROVEN（整边） | E-37（close_admission 双实现二选一） |

按 AGENTS 的门：以上两个 ELIMINATE_CANDIDATE 均未满足"measured hotspot → 单因素 A/B"前置，本记录只登记假设与证据，不构成改动授权。
