# 合并 Execution DAG：全路径机工审查（admission/threadpool + uring-submit + completion-publication）

- 合并：2026-11-18，DAG 综合审校人（e5-fullpath-machine-work）。
- 输入：同目录三份分段机工记录
  `dag-admission-threadpool.md`（40 边，下称 TP 段）、
  `dag-uring-submit.md`（26 边，下称 UR 段）、
  `dag-completion-publication.md`（33 边，下称 CP 段）。原始边合计 **99**。
- 基线：master `5d94f233`（与 repo-reality.md 冻结基线一致）。
- 方法：跨段去重合并 + 锚点抽查回源码核实。本会话只读源码，未编译、未运行任何
  测试/基准（同机频率测量约束，与三段一致）。本文件是本合并任务的唯一写入物。
- 抽查声明：本会话对 **20 组锚点**执行了回源码核实（`sed -n`/`grep` 只读命令，
  逐条命令与命中内容见 §7）；核实通过的锚点在 §2 标 `✓`。三段中其余锚点未逐一
  复核，以分段记录为准。
- 改判与更正：见 §6（三处，均为表述/归类/范围更正，未推翻任何边的最终裁决）。

## 0. 合并口径

1. **边 = 一次可指认的机工责任**（一段代码在请求生命周期某一相位的固定成本）。
   同一段代码被两个以上分段从不同调用侧描述时合并为一个合并边（M-xx），并在
   `来源` 字段保留全部原始边号；分段的完整机工明细（CPU/内核/内存/同步四列）
   以原分段文件为准，本文件只保留锚点 + 语义义务一句话 + 裁决 + 未证假设。
2. **裁决取值**沿用分段：`KEEP` / `ELIMINATE_CANDIDATE`（存在可指认的冗余工作，
   值得进入机器测量追问，不等于可安全删除）/ `UNPROVEN`（裁决本身悬置，需
   owner 或测量裁决）。KEEP 边内登记的候选子项与未证假设单独收集进 §4。
3. **候选计数口径**（供汇总）：§4 的候选清单按"去重后的候选条目"计数，分
   A 类（边级裁决为 ELIMINATE_CANDIDATE/UNPROVEN）与 B 类（KEEP 边内显式登记的
   候选子项/未证假设）。合计 **22 条**（A 类 10 + B 类 12）。任何按原始段边或
   按合并边的不同口径计数都在 §4 表头注明。
4. 所有候选均未满足 AGENTS 机制门的"measured hotspot → 单因素 A/B"前置；
   本 DAG 只登记假设与证据，不构成任何改动授权。

## 1. 跨段去重映射

| 合并边 | 来源段边 | 去重说明 |
|---|---|---|
| M-01 | TP E-01 + UR E01 + CP D1 | 同一 scope 提交代码（request_scope.cpp），三段各描述一次 |
| M-02/03/04/05/06 | TP E-02/E-03/E-04/E-05/E-06 + UR E02 | UR E02 是同一 async_io_context.cpp 入口的整合视图，按机工拆并入五条 |
| M-07 | TP E-09 + UR E04 | 同一 core reserve（request_core.cpp:165-182），两后端调用侧 |
| M-09 | TP E-12 + UR E07 | 同一 core accept（:184-210），两后端调用侧 |
| M-10 | TP E-14 + UR E08 + CP C6 | 同一零长内联发布机制（两后端 publish_zero_op_inline + 语义分析） |
| M-11 | TP E-35 + UR E24 + CP C8 | 同一 ProgressSource::signal/eventfd 通道 |
| M-12 | TP E-37 | CP 段对 uring close_admission 的注记并入此边 |
| M-19 | TP E-11 + UR E06 | 同一 DeliveryRecord 初始化（work_mtx_ / dispatch_mtx_ 两形态） |
| M-22 | TP E-17 + CP B4 | claim_execution core 边（TP 装载侧 + CP 协议侧） |
| M-30 | TP E-25 + CP B1 | offer_terminal core 边 |
| M-31 | TP E-26 + CP B3 | release_execution core 边（含 try_reclaim_ 挂点） |
| M-64 | TP E-28 + CP C5 | threadpool poll_progress 单锁快照（两段同判） |
| M-61 | TP E-29 + CP C2 | publish_one + begin/complete core 边 |
| M-62 | TP E-30 + CP C3 | deliver_event + 观察者 claim/retire core 边 |
| M-66 | TP E-32 + CP C7（threadpool 半边） | 残留判定；CP C7 的 uring 特有半边留在 M-69 |
| M-78/79 | TP E-38/E-39 + CP D6 | blocking read_at/write_at（两段同判 KEEP） |
| M-80 | TP E-40 + CP D7 | compose 组合循环（CP D7 的 KEEP 主体 + TP E-40 的候选） |
| M-24/25/26/27 | TP E-19..E-22 + CP D8 | CP D8 是 worker 终端路径的聚合视图，作为交叉引用并入，不单列 |
| 无合并 | 其余边 | 各段独有，一一对应 |

去重结果：99 条原始边 → **80 条合并边**（M-01..M-80）。

## 2. 合并后执行 DAG（80 边）

格式：**M-xx 标题** — 锚点（`✓`=本会话核实）；来源；裁决。`〔假设〕`为登记进
§4 的未证假设/候选子项。

### 阶段 0：共享准入（scope / context / core，两后端共用）

- **M-01** RequestScope 提交入口与槽位预留（read/write）— `request_scope.cpp:46-55,137-161,163-187` ✓（46-55,137-161）；来源 TP E-01 + UR E01 + CP D1；**KEEP**〔B-UNPROVEN：O(S) 扫描份额，H3 INVESTIGATE 开放；free-list 替换已被 m1-results REJECT〕
- **M-02** 启动校验 initiation_rejection — `async_io_context.cpp:24-26` ✓、六处 submit 入口；`file_semantics.hpp:178-185`；来源 TP E-02 + UR E02 前半；**KEEP**（后端无关统一守门：AsyncBackend 接口不要求后端自校验）。与 M-16 构成同一 (fd<0, access, op) 的双层校验，候选收敛方向见 M-16
- **M-03** 提交临界区：access_mtx_ 贯穿虚分发 — `async_io_context.cpp:254-259,276-279,86-93` ✓（86-93 注释义务）；来源 TP E-03 + UR E02 中段；**KEEP**
- **M-04** 统计 tally_submit + max_outstanding — `async_io_context.cpp:229-244,260-263`；来源 TP E-04 + UR E02 后段；**KEEP**（stats_==nullptr 时为两次可预测分支）
- **M-05** Request 句柄构造 — `async_io_context.cpp:266-268`；`request.hpp:174-175`；来源 TP E-05 + UR E02 末段；**KEEP**
- **M-06** 状态操作提交家族 sync_data/sync_all/file_info/size — `async_io_context.cpp:291-369` ✓（291-309 等）；来源 TP E-06 + UR E02；**KEEP**
- **M-07** core reserve — `threadpool_backend.cpp:228-239`、`uring_backend.cpp:447-458`；`request_core.cpp:165-182` ✓（同一 core 边）；来源 TP E-09 + UR E04；**KEEP**
- **M-08** accept 失败回滚 rollback — `threadpool_backend.cpp:282-284`；`request_core.cpp:212-224,599-631` ✓（release_slot_ :599-620）；来源 TP E-13；**KEEP**（关闭窗口交叠保护）
- **M-09** core accept（含 BorrowFacts 存储）— `request_core.cpp:184-210` ✓（:200 写、:204-207 零长终局）；`request_core.cpp:614` 清零 ✓；SlotObservation/CoreSnapshot（hpp:135-148,231-245）均不含 borrow ✓；`docs/explicit-io-contract.md:586` 列 borrow facts ✓；来源 TP E-12 + UR E07；边 **KEEP**；**〔A-UNPROVEN：`Slot::borrow` 只写不读（grep 全仓仅写/清/测试 driver 构造局部变量），root 把 borrow facts 列入请求记录域——是等待消费者的 root 义务存储还是死存储，需 owner 裁决〕**
- **M-10** 零长 op 内联发布 publish_zero_op_inline — `threadpool_backend.cpp:334-354` ✓（:338 begin nullptr）、`uring_backend.cpp:553-576` ✓（:558 begin nullptr）；core 侧 `request_core.cpp:411-422,438-460,462-484`；来源 TP E-14 + UR E08 + CP C6；**KEEP**（ owed 通道是 H2b 已裁决机制）〔B-UNPROVEN：三次 core 锁可融合为一次；步骤裁决值各异，属机制变更〕
- **M-11** ProgressSource::signal + eventfd — `progress_source.hpp:252-264` ✓（signal 饱和纪律）、`:466-489` ✓（wake 写抑制）、`:74-79`；`uring_backend.cpp:259-283`（io_uring_register_eventfd）；来源 TP E-35 + UR E24 + CP C8；**KEEP**（notification_pending_ 去重是 #621 已产品化机制）
- **M-12** close_admission 边（core 直连 vs backend 版）— context 直连点 `async_io_context.cpp:91,141,451,484,844-847,852` ✓（:842-847 与 backend 版同构）；backend 版 `threadpool_backend.cpp:834-845` ✓、`uring_backend.cpp:691-701`（另有空体 no-ring 变体 :72）；生产调用方 grep 仅 `tests/progress_source_ownership_test.cpp:468,471` ✓；来源 TP E-37（+CP 对 uring 的注记）；**UNPROVEN**（同一机制两份实现，保留哪份属所有权裁决；本会话把 TP 段的发现扩展到 uring backend 版——同样生产零调用）
- **M-13** core cancel 仲裁（won-before-execution / requested）— `request_core.cpp:260-275`；来源 CP B2；**KEEP**（CANCEL-01 在生产的主强制点）

### 阶段 1：校验与准备（两后端各自）

- **M-14** TP validate data ops — `threadpool_backend.cpp:135-149,195-209`；来源 TP E-07；**KEEP**（range/length/null 为本层独有增量）
- **M-15** TP validate state ops — `threadpool_backend.cpp:151-169`；来源 TP E-08；**KEEP**（与 M-02 的重复同 M-16 的收敛方向）
- **M-16** UR validate_* + have_ring_/fatal_error_ 状态门 — `uring_backend.cpp:94-123,430-438` ✓；`async_backend.hpp:17-19`（Uninstalled seam）✓；来源 UR E03；**ELIMINATE_CANDIDATE**（state-op 半边：与 M-02 参数完全相同的 constexpr precheck 每提交跑两遍，增量为零；data-op 的 range+空指针增量为后端独有 KEEP。收敛须先证"生产调用方必经 context 层"是结构保证，且与 seam 直连测试的义务冲突需 owner 裁决）
- **M-17** TP prepared op 槽内写 — `threadpool_backend.cpp:249-259`；来源 TP E-10；**KEEP**（无锁，mutex handoff 可见性）
- **M-18** UR PreparedUringOp 槽内写 — `uring_backend.cpp:468-486`；`uring_submit.hpp:11-14`；来源 UR E05；**KEEP**（length/native_length 双域非重复）
- **M-19** DeliveryRecord 初始化 — `threadpool_backend.cpp:261-267`、`uring_backend.cpp:488-500`；来源 TP E-11 + UR E06；**KEEP**（fail-fast 断言是契约诊断）

### 阶段 2：分发与执行

threadpool 侧：

- **M-20** TP dispatch_after_accept — `threadpool_backend.cpp:299-332,44-53`；来源 TP E-15；**KEEP**
- **M-21** worker_loop cv 等待出队 — `threadpool_backend.cpp:386-403`；来源 TP E-16；**KEEP**
- **M-22** claim_execution + op 装载 + active_workers_++ — `threadpool_backend.cpp:405-413`；`request_core.cpp:367-378` ✓（同一 core 边，UR 调用侧见 M-36）；来源 TP E-17 + CP B4；**KEEP**
- **M-23** retired_unclaimed 分支 — `threadpool_backend.cpp:415-424,856-860`；来源 TP E-18；**KEEP**（删除即 settlement 永久等待，死锁风险）
- **M-24** pread — `threadpool_backend.cpp:481-498` ✓（:483 errno=0、:486-487 const_cast）；`posix_retry.hpp`；来源 TP E-19（CP D8 聚合视图并入）；**KEEP**
- **M-25** pwrite — `threadpool_backend.cpp:499-514`；来源 TP E-20；**KEEP**
- **M-26** fdatasync/fsync — `threadpool_backend.cpp:515-526`；来源 TP E-21；**KEEP**
- **M-27** fstat — `threadpool_backend.cpp:527-541`；来源 TP E-22；**KEEP**
- **M-28** syscall_count_ 递增 — `threadpool_backend.cpp:434`（fetch_add，未被宏包裹）✓、`threadpool_backend.hpp:258`（成员未包裹）✓；唯一读者 `threadpool_test_seams.hpp:108-110`（全文件在 `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` 内，:4）✓；生产库目标不定义该宏（`xmake/libraries.lua:34-56` 无 define ✓，`xmake/tests.lua` 14 处定义 ✓，`xmake/apps.lua:4` 注释 ✓）；来源 TP E-23；**ELIMINATE_CANDIDATE**（成员+写侧+访问器同进同出宏门控，删生产侧每 syscall 一次 `lock xadd`，测试 oracle 不变）
- **M-29** active_workers_ 递减 — `threadpool_backend.cpp:435-438,856-860`；来源 TP E-24；**KEEP**（原子化会破坏 internal_work_retired 的同锁联合快照）
- **M-30** TP offer_terminal（终态选主）— `threadpool_backend.cpp:444-456`；`request_core.cpp:346-365` ✓（:353-355 duplicate、:356-358 zero_effect_cancel 臂）；zero_effect_cancel 生产者 grep：仅 `tests/request_core_test_driver.hpp:180` ✓，两后端 offer 点全发 physical_outcome ✓；来源 TP E-25 + CP B1；边 **KEEP**；**〔A-UNPROVEN：`TerminalCandidateKind::zero_effect_cancel` 仲裁臂生产零生产者，CANCEL-01 同一规则已由 M-13 在 cancel() 入口强制——seam 必备义务还是可删的第二强制点，需 owner 裁决（仅该臂；offer_terminal 其余 KEEP）〕**
- **M-31** release_execution（borrow-touch 归还）— `threadpool_backend.cpp:457-460`；`request_core.cpp:393-409` ✓、reclaim 谓词 `:578-590` ✓（六条件）；来源 TP E-26 + CP B3；**KEEP**（premature_rejected 是 root 合同诊断）
- **M-32** TP publication_pending 入队 + signal — `threadpool_backend.cpp:461-466`；来源 TP E-27；**KEEP**
- **M-33** TP cancel：ring 摘除 + core cancel + 终态注入 — `threadpool_backend.cpp:64-87` ✓（remove_exact 扫描+前移）、`:782-814`；来源 TP E-36；**KEEP**〔B-UNPROVEN：O(n) 摘除改惰性失效——removed_entry signal 与 internal_work_retired 依赖真实摘除，须加失效计数机制〕

uring SQE 侧：

- **M-34** UR dispatch_after_accept（内联清空 + 毒化退休 + 重试广告）— `uring_transport.cpp:43-102,104-128,211-216` ✓（:82 front）；来源 UR E09；**KEEP**
- **M-35** SQE 分配 io_uring_get_sqe + 满环冲刷重试 — `uring_transport.cpp:141-149`；来源 UR E10；**KEEP**
- **M-36** claim 失败 SQE 尾回退 — `uring_transport.cpp:151-159`；来源 UR E11；**KEEP**
- **M-37** dispatch 队列移除 remove_exact — `uring_internal.hpp:118-133` ✓（扫描+整队前移）、pop_front `:109-116` ✓；调用点事实：两处 dispatch 调用都传 `front()`（`uring_transport.cpp:82` ✓、`uring_backend.cpp:624` ✓），必中 i=0 仍付 (size-1)×16B 前移；任意位置删除只被 cancel_key（`uring_transport.cpp:589` ✓）真正需要；来源 UR E12；**ELIMINATE_CANDIDATE**（仅限两处 front 调用点换 pop_front，miss fail-fast 以"pop 前核对 front"替代；函数本身 KEEP）
- **M-38** cookie 分配 — `uring_transport.cpp:164,526-538`；`uring_internal.hpp:45-58`；来源 UR E13；**KEEP**
- **M-39** SQE prep + user_data — `uring_transport.cpp:166-189`；来源 UR E14；**KEEP**（fixed files/buffer ring 缺席为能力面观察点）
- **M-40** RouterEntry 布线 + live_cookies_ — `uring_transport.cpp:190-195,559-582`；`uring_backend.cpp:350-351,365-373`；`uring_test_seams.hpp:75-81` ✓（无锁测试缝读者）；来源 UR E15；**KEEP**〔B-EC 低优先：live_cookies_/live_control_sqes_ 的全部生产访问都在 dispatch_mtx_ 下，原子实现冗余于互斥锁（x86 relaxed ≈ 普通访存）〕
- **M-41** TransportLedger append — `uring_internal.hpp:146-240`（append :170-191）；来源 UR E16；**KEEP**（毒化内核可见性证明的有序身份副本）
- **M-42** submit_transport_locked → io_uring_submit → io_uring_enter(2) — `uring_transport.cpp:218-235` ✓（:219 ledger 空提前返回、:222 submit_flushes_ fetch_add）；唯一读者 `uring_test_seams.hpp:75-77`（文件在测试宏内，:4）✓；来源 UR E17；边 **KEEP**〔B-EC：submit_flushes_ 生产只写不读，与 M-28 同型宏门控候选〕；热路径锁内 syscall（dispatch_mtx_ 持有中 enter）为结构性事实
- **M-43** transport 结果记账 — `uring_transport.cpp:237-296`；来源 UR E18；**KEEP**
- **M-44** 内核头对账 — `uring_transport.cpp:298-335`；来源 UR E19；**KEEP**（与 M-43 是返回值/khead 两个对账权威）
- **M-45** 毒化与 Class-A 恢复 — `uring_transport.cpp:337-524`；来源 UR E20；**KEEP**
- **M-46** 运行中取消控制 SQE — `uring_transport.cpp:584-693`；`uring_completion.cpp:101-143`；来源 UR E21；**KEEP**
- **M-47** sticky intent 补投 — `uring_transport.cpp:695-711`；来源 UR E22；**KEEP**
- **M-48** UR poll() 分发驱动面 — `uring_backend.cpp:616-634` ✓（:623-626 dispatch 清空）；来源 UR E23；**KEEP**（PROG-02 重试义务执行面）
- **M-49** 构造期能力探针 — `uring_backend.cpp:161-256`；`xmake/uring.lua:44-82`；来源 UR E25；**KEEP**
- **M-50** 内核物理边 io_uring→VFS/PageCache — 锚点为 UAPI 语义（UR 段声明未运行验证）；来源 UR E26；**KEEP**

### 阶段 3：完成收割与终局（uring CQ + core 退休）

- **M-51** reap_cqes 批量收割 — `uring_completion.cpp:200-229,238-277`；来源 CP A1；**KEEP**
- **M-52** handle_one_cqe 帧锁与分派 — `uring_completion.cpp:99-151`；来源 CP A2；**KEEP**（每 CQE 一锁为 THREAD-02 线性化，flow-audit H2 已证结构）
- **M-53** find_live_router_cookie_ O(R) 逆向扫描 — `uring_transport.cpp:551-557` ✓（逆向线性）；调用点 `uring_completion.cpp:105,148`；来源 CP A3；**ELIMINATE_CANDIDATE**（user_data 编码路由索引替换，H1 已 PROPOSE_UPSTREAM：0.41x @R=64 / 0.05x @R=1024，`bench/mechanisms/h1_router_index_bench.cpp` 在库；迁移未证——编码必须保住 cookie 单调域的三项证明义务：TransportLedger 物理位置单调 `uring_internal.hpp:170-191`、控制 CQE exact-submitted 校验 `uring_completion.cpp:110-116`、cancel 定向 prep_cancel64）
- **M-54** control-CQE 处理 — `uring_completion.cpp:101-144`；来源 CP A4；**KEEP**
- **M-55** 物理结果解码 — `uring_completion.cpp:153-195`；来源 CP A5；**KEEP**（SEM-07 mask 门是义务不是防御）
- **M-56** finalize_operation_terminal_ — `uring_completion.cpp:23-97`；来源 CP A6；**KEEP**
- **M-57** acquire_execution — `request_core.cpp:380-391` ✓（:389 ++execution_refs）；声明 hpp:215；生产 grep 零调用 ✓（仅 `tests/request_core_protocol_test.cpp:544,573,593,604,612` 经 driver）；来源 CP B5；**UNPROVEN**（热路径成本为零，删除只是代码面收缩；BACKEND-01 seam 面 + 协议完备性测试面，是否"必需义务"需 owner 裁决）
- **M-58** reclaimable_/try_reclaim_/release_slot_ — `request_core.cpp:578-631` ✓（谓词六条件）；来源 CP B6；**KEEP**（DP-3A 初始化闭包，mutant 杀伤）
- **M-59** acquire/release_control（控制引用记账）— `request_core.cpp:411-436`；来源 CP B7；**KEEP**（REQ-05 槽位钉住）

### 阶段 4：发布 / 观察者 / owed / 驱动

- **M-60** UR poll() 发布排空 — `uring_backend.cpp:638-657` ✓（pass_bound 快照 + 逐条弹出锁）；来源 CP C1；**KEEP**（弹出可与快照合并为批量搬移，量级小于 M-63，未列候选）
- **M-61** publish_one + begin/complete — `uring_backend.cpp:578-608`；`request_core.cpp:438-460,462-484` ✓（queue_armed_observer :474-481）；生产 begin_publication 恒传 nullptr（TP :338/:358 ✓、UR :558/:584 ✓），PublicationPayload 快照仅测试填充 ✓；来源 TP E-29 + CP C2；**KEEP**〔B-UNPROVEN：begin/complete 两锁融合，步间夹记录读与测试暂停点〕〔B-EC 低优先：payload-out 快照下沉为测试专用通道〕
- **M-62** deliver_event + 观察者 claim/retire — `threadpool_backend.cpp:376-384` ✓（claim→on_ready→retire）；`uring_backend.cpp:610-614`；`request_core.cpp:513-531`（claim 快速返回仍是完整 mutex 往返）✓、`:504-508`（published 槽 already_terminal）✓；生产 sink 为 ReferenceReadySink，on_ready 生产构建空体（`reference_ready_sink.hpp:10-16`）✓；routing_sink_ 仅 TestAccess 注入 ✓；来源 TP E-30 + CP C3；**KEEP**〔B-UNPROVEN：complete_publication 已在锁内算出 queue_armed_observer，发布后 register_observer 返回 already_terminal，故"发布时无观察者 ⇒ 跳过 claim/retire"可证协议安全——但 on_ready 是 routing_sink 公共交付点、delivering 相位是 cancel_observer/delivery_retired 可观测状态，属机制变更，须 measured profile + 单因素 A/B。对应 history-digest §1.2 未决项 6 mutex family ~9-11% 候选〕
- **M-63** UR owed sweep 逐槽锁 — `uring_backend.cpp:658-686` ✓（owed_count_ 门 :658-663；**锁在循环体内 :668，continue 在锁内 :670-671**）；threadpool 对照形态 M-64 已是单锁快照；来源 CP C4；**ELIMINATE_CANDIDATE**（锁形态换单锁快照，H2a 已 PROPOSE_UPSTREAM "uring first"，既往 −370ns/pass @C=64 idle；#634 只产品化了 owed_count_ 门，代码与提案裁决的落差是现成单因素 A/B 候选。owed 机制本身——OBS-02 交付不得由提交线程内联 + PROG-01 残留诚实性——**KEEP**）
- **M-64** TP poll_progress 单锁快照 + owed 扫描 — `threadpool_backend.cpp:550-584` ✓（一次 work_mtx_ 装 publish_batch_/owed_batch_，锁外交付 :586-598）；来源 TP E-28 + CP C5；**KEEP**（capacity 64 时 O(capacity) 扫描无实测热点，不立项）
- **M-65** TP owed 事件投递 + release_control — `threadpool_backend.cpp:586-598` ✓；来源 TP E-31；**KEEP**〔B-UNPROVEN：release_control 第三锁与 retire 融合（同槽连续两步 core 操作、无回调间隔）〕
- **M-66** 残留判定 immediate_work_remains + outstanding_summary — `threadpool_backend.cpp:600-611` ✓（第二次 work_mtx_ 持有是有意的）；来源 TP E-32 + CP C7 threadpool 半边；**KEEP**
- **M-67** run_progress_pass_ 骨架 + sticky 读 — `async_io_context.cpp:429-439` ✓（每 pass 无条件 core_->health_failed() + progress_->wait_health_failed()）、`:449-453` ✓（exhausted 触发关 admission）；sticky 性质：`request_core.cpp:239-243`（health_failed_ 置位不清）✓、`progress_source.hpp:63-65,506`（health_failed_ 只置不清）✓、exhaustion 饱和不回绕 ✓；来源 TP E-33；**KEEP**〔B-UNPROVEN：两个永不复零的 sticky 标志每 pass 花 3 次互斥收购读——原子 sticky 或一次融合查询可减 2-3 次锁/pass；ProgressSource"一切经 mtx_"是其 eventfd 单读者协议一部分，改造须重新证明〕
- **M-68** drive guard — `async_io_context.cpp:179-196,441-447`；来源 TP E-34；**KEEP**
- **M-69** UR poll_progress 组装（含 has_immediate_physical_work）— `uring_backend.cpp:285-293,304-328`；来源 CP C7 uring 半边；**KEEP**（pass 尾与停靠握手 probe 各跑一次是 PROG-02 竞态要求）
- **M-70** wait_if_unchanged 停靠握手 — `progress_source.hpp:104-232`；来源 CP C9；**KEEP**
- **M-71** drain 单读（#621）— `progress_source.hpp:414-460`（:437-442 成功即清位返回）；来源 CP C10；**KEEP**（必然 EAGAIN 的第二次读已删）
- **M-72** wait_one 驱动等待 — `async_io_context.cpp:569-696,479-563`；来源 CP C11；**KEEP**（access_mtx_ 3-4 次获取均在无竞争 owner 路径）
- **M-73** 结算收敛见证 — `async_io_context.cpp:469-477`；`uring_backend.cpp:338-373`；`request_core.cpp:66-91`；来源 CP C12；**KEEP**（owed 结算面权威扫描与热路径 owed_count_ 门是 #634 既定分工）

### 阶段 5：应用消费

- **M-74** wait_for_slot_ + ready() 快路径 — `request_scope.hpp:146-185`；来源 CP D2；**KEEP**〔B-UNPROVEN：入口快路径与 wait_one 返回后重查的合并收益未证〕
- **M-75** observe/consume_public_result（消费 = buffer 复用许可）— `request.hpp:134-159,187-209`；`request_core.cpp:296-321,323-344`；来源 CP D3；**KEEP**
- **M-76** Request::discard — `request.hpp:161,177-185`；`request_core.cpp:277-294`；来源 CP D4；**KEEP**
- **M-77** 应用 buffer 复用（sluice-copy PipelineSlot）— `copy_task.cpp:46-56,66-88,106-149,221-240`；来源 CP D5；**KEEP**（全路径零拷贝）

### 阶段 6：直呼形态（Direct I/O = blocking_file 直呼，非 O_DIRECT）

- **M-78** blocking read_at — `blocking_file.cpp:65-84,87-106`；来源 TP E-38 + CP D6；**KEEP**（直呼形态是请求形态机工增量的对照底座）
- **M-79** blocking write_at — `blocking_file.cpp:86-127`；来源 TP E-39 + CP D6；**KEEP**
- **M-80** compose 组合循环 — `blocking_file.cpp:272-285`（prevalidated_pread_step 及"入口已证整段合法"注释）✓、`:299-333`、`:336-350`（read_exact_at 免重校验）、`:352-366` ✓（write_all_at 每步调 write_at 全量重校验）；来源 TP E-40 + CP D7；**ELIMINATE_CANDIDATE**（write_all_at 的逐步重校验：入口已验证 (offset,total) ⇒ 每子步必然合法，同文件 :272-274 注释已给出该论证且 read 侧有免重校验先例；补 prevalidated_pwrite_step 是有先例的最小机制；CPU-only、无锁无内核影响，仍需 measured hotspot + 机制门）〔B-UNPROVEN hygiene：`file_semantics.hpp:243-298` compose_progress/CompositionState 模型词表生产零调用，是否移入测试面属 hygiene 裁决〕

## 3. 端到端关键路径（一段话）

一次异步 read/write 的全路径机工链为：应用提交（M-01 scope 槽位 O(S) 预留，无锁）→ `AsyncIoContext::submit`（M-02 constexpr 启动校验 → M-03 access_mtx_ 临界区内虚分发 → M-04 统计 → M-05 Request 句柄）→ 后端 reserve（M-07，core mutex）→ prepared op 槽内写（M-17/M-18，无锁）→ DeliveryRecord 初始化（M-19）→ accept（M-09，core mutex）；此后分叉——threadpool 侧经 dispatch 入队+notify（M-20）→ worker 出队（M-21）→ claim（M-22，work→core 双锁）→ run_syscall 物理原语（M-24/25/26/27，每 op 1 次真实 syscall，外加 M-28 一次无读者的 `lock xadd`）→ offer_terminal（M-30）→ release_execution（M-31）→ 发布入队+signal（M-32）；uring 侧经 dispatch_after_accept（M-34）→ get_sqe（M-35）→ claim（M-36）→ remove_exact(front) O(队列深) 前移（M-37）→ cookie/SQE prep（M-38/39）→ router/ledger 登记（M-40/41）→ submit_transport_locked（M-42：submit_flushes_ RMW + dispatch_mtx_ 持有中的 1 次 io_uring_enter）→ 结果记账（M-43）；驱动侧每 pass 为 run_progress_pass_（M-67：3 次互斥收购读两个永不复零的 sticky 标志）→ poll_progress（threadpool M-64 单锁快照；uring M-48 + M-60 + M-63 逐槽锁 owed sweep）→ reap（M-51）→ handle_one_cqe（M-52，cookie 路由 O(R) 逆向扫描 M-53）→ finalize（M-56，offer+release 各 1 次 core 锁）→ publish_one（M-61，2 次 core 锁）→ deliver_event（M-62，2 次 core 锁向生产空体 sink 投递）→ 残留诚实上报（M-66/69）→ 停靠握手与单读 drain（M-70/71）；锁全序 access_mtx_ → work/dispatch_mtx_ → core mutex → ProgressSource::mtx_。端到端每请求固定机工 ≈ access 锁 1-2 次 + 后端锁 2-3 次 + core 锁 7-9 次 + progress 锁 1-2 次，外加（uring）1 次锁内 enter 与（threadpool）1 次物理 syscall；§4 全部 22 条候选恰好都落在这条链的重复校验、无读者 RMW、O(n) 搬移/扫描与锁往返四类位置上。

## 4. 候选清单（ELIMINATE_CANDIDATE / UNPROVEN 全量，去重后 22 条）

A 类（边级裁决）：6 ELIMINATE_CANDIDATE + 4 UNPROVEN；B 类（KEEP 边内显式登记）：3 ELIMINATE_CANDIDATE + 9 UNPROVEN。
（口径注：若按"合并边携带候选标记"计为 20 条合并边；若按原始段边计为 22 条（TP 10 + UR 4 + CP 8）。本清单按去重后候选条目计 22。）

| # | 类 | 合并边 | 候选内容 | 前置条件（AGENTS 机制门） |
|---|---|---|---|---|
| 1 | A-EC | M-28（TP E-23） | syscall_count_ 生产侧宏门控剔除（写侧/成员与访问器同进同出） | 单因素 A/B 证明生产路径受益；测试 oracle 不变 |
| 2 | A-EC | M-80（TP E-40） | write_all_at 逐步重校验 → prevalidated_pwrite_step（同文件 read 侧先例） | 直呼形态 measured hotspot；"compose 仅由已校验入口进入"不变量 |
| 3 | A-EC | M-16（UR E03） | uring 后端层 state-op 校验收敛为单层（保 context 层） | 证"生产调用方必经 context 层"为结构保证；seam 测试义务冲突须 owner 裁决 |
| 4 | A-EC | M-37（UR E12） | 两处 dispatch 调用点 remove_exact(front) → pop_front | 深队列派发的 memcpy 占比测量；pop 前核对 front 的替代 fail-fast |
| 5 | A-EC | M-53（CP A3） | cookie→O(R) 扫描换 user_data 路由索引编码（H1，PROPOSE_UPSTREAM 0.41x/0.05x） | 迁移证明：编码保住 cookie 单调域三项证明义务（ledger 单调/控制 CQE exact/cancel 定向） |
| 6 | A-EC | M-63（CP C4） | uring owed sweep 逐槽锁 → 单锁快照（H2a，PROPOSE_UPSTREAM −370ns/pass） | uring-first 单因素 A/B；交付顺序不变性确认 |
| 7 | A-UNPROVEN | M-09（TP E-12/UR E07） | Slot::borrow 只写不读：root 义务存储 vs 死存储 | owner 裁决（root explicit-io-contract.md:586 列 borrow facts） |
| 8 | A-UNPROVEN | M-30 臂（CP B1） | zero_effect_cancel 仲裁臂生产零生产者 | owner 裁决（CANCEL-01 已由 cancel() 入口强制；seam 协议完备性权衡） |
| 9 | A-UNPROVEN | M-57（CP B5） | acquire_execution 生产零调用 | owner 裁决（BACKEND-01 seam 面；热路径成本为零） |
| 10 | A-UNPROVEN | M-12（TP E-37） | close_admission 双实现二选一（TP 与 UR backend 版均生产零调用） | owner 裁决（progress_source_ownership_test 轨迹 oracle 归属） |
| 11 | B-EC | M-42（UR E17） | submit_flushes_ 生产只写不读原子计数 | 与 #1 同型宏门控 |
| 12 | B-EC | M-40（UR E15，低优先） | live_cookies_/live_control_sqes_ 原子冗余于 dispatch_mtx_ | 收益微小（relaxed ≈ 普通访存），需测试缝读者同步加锁 |
| 13 | B-EC | M-61（CP C2，低优先） | PublicationPayload 快照仅测试可达（生产恒 nullptr） | 代码面收缩；consumer probe 消费面重排 |
| 14 | B-UNPROVEN | M-01（CP D1） | scope O(S) 扫描份额（H3 INVESTIGATE 开放；free-list 已 REJECT） | 高重复 `-scope64` 隔离协议测量 |
| 15 | B-UNPROVEN | M-10 | 零长发布三次 core 锁融合 | 步间裁决值与 fail-fast 绑定，属机制变更；零长占比未测 |
| 16 | B-UNPROVEN | M-61（TP E-29） | begin/complete 两锁融合 | 同 #15 融合家族；步间夹记录读与测试暂停点 |
| 17 | B-UNPROVEN | M-62（TP E-30） | 无观察者跳过 claim/retire 协议空转（2 次 core 锁/完成事件投递空回调） | measured profile + 单因素 A/B；delivering 相位可观测性与 routing_sink 交付点重证 |
| 18 | B-UNPROVEN | M-65（TP E-31） | owed release_control 第三锁融合 | 同 #15 融合家族 |
| 19 | B-UNPROVEN | M-67（TP E-33） | sticky 标志原子化/融合查询（每 pass 3 次互斥读两个永不复零标志） | mutex family ~9-11% 候选对位测量；ProgressSource 单读者协议重证 |
| 20 | B-UNPROVEN | M-33（TP E-36） | TP cancel ring 摘除惰性失效 | 需失效计数机制；removed_entry signal 与队列空判定依赖真实摘除 |
| 21 | B-UNPROVEN | M-74（CP D2） | ready() 快路径与 wait_one 重查合并收益 | 收益方向因 ready 概率而异，无证据不裁决 |
| 22 | B-UNPROVEN | M-80 hygiene（CP D7） | file_semantics.hpp 模型词表生产零调用的位置 | hygiene 裁决（语义 oracle 共享词表面是否移入测试面） |

## 5. 文档 ↔ 代码不符项（单列）

1. **`include/sluice/async/detail/uring_backend.hpp:24-25` statx 注释过期**（UR 段 §4.1，本会话核实）：注释称 `struct statx` 完整定义"lives either in `<linux/statx.h>` or in the guarded mirror inside uring_backend.cpp"；实际 uring_backend.cpp 无 statx 镜像定义、未包含 `<linux/statx.h>`，定义来自 `<sys/stat.h>`（uring_backend.cpp:28 ✓）。
2. **注释政策偏差（AGENTS.md "C++ comments" 条 vs 代码）**（TP 段 §6.1，本会话核实 :18-22）：`async_io_context.cpp:18-22` 注释引用需求 ID（PROG-02）；`threadpool_backend.hpp:129-133,240-244`、`progress_source.hpp:25-29,37-41,96-103,271-273`、`request_core.hpp:180-183` 等为语义叙事型注释。AGENTS 明文禁止注释叙事语义/引用需求 ID。属政策与代码偏差，非行为不符；裁决归文档纪律流程。
3. **声明-使用错位（公共头声明为生产能力、实际消费者只有测试）**（TP 段 §6.2，本会话核实并扩展）：
   - `syscall_count_`：写侧 threadpool_backend.cpp:434 与成员 hpp:258 均无宏门控，读者全在测试宏后（§4 候选 #1）；
   - `ThreadPoolBackend::close_admission`（hpp:58）与 `UringAsyncBackend::close_admission`（hpp:87，本会话扩展）生产链零调用，仅 `tests/progress_source_ownership_test.cpp:468,471` 调用；
   - `ThreadPoolBackend::dispatch_occupancy()/active_workers()`（hpp:62-65 无条件公共声明）grep 全仓仅两个测试文件调用（tests/backend_contract_conformance_test.cpp:126、tests/public_request_test.cpp:117）。
   三者是错位，非行为 bug。
4. **progress_source.hpp 措辞张力**（CP 段 §5.1，本会话核实）：:521-523 类注释"never lends the context notification fd itself"与 :535-545 `register_notification_with_kernel` 实际把 fd 借给 install 调用帧（registration-only borrow）并存，只能读作"不提供持久 fd 访问器"；语义以 :535-539 成员注释为准。
5. **既往裁决与代码现状落差（非失配，登记为最直接追问入口）**（CP 段 §5.2，本会话核实）：m1-results.md:165-166 将 H1（路由索引）/H2a（uring 单锁快照）标 PROPOSE_UPSTREAM，当前代码两者均未产品化（M-53 仍 O(R) 扫描、M-63 仍逐槽锁）。
6. **时间性漂移（非失配）**（CP 段 §5.3）：repo-reality.md:37 记唯一未跟踪项 `docs/evidence/621-round2-R1/`，当前工作树另有本战役产物 `docs/evidence/e5-fullpath-machine-work/`。
7. **口径注记（合并更正，见 §6-3）**：UR 段 §4.2 将任务范围中的"Direct I/O 对应实现"按 O_DIRECT 解读并报告生产缺席（`grep -rnw O_DIRECT` C++ 零命中，核实为 O_DIRECTORY 子串假命中排除）。O_DIRECT 缺席属实；但任务与 TP/CP 段的"Direct I/O"指直呼形态（blocking_file.cpp，存在且已入 DAG M-78/79/80），故该项应记为解释口径差异，不列为文档↔代码不符。

## 6. 改判 / 更正记录

1. **CP C3 的"无注册零成本"表述更正（未改裁决）**：CP 段称 claim/retire "无注册时返回 none 快速路径"、"已是'无注册零成本'形态"。核实 `request_core.cpp:513-531`：快速返回确实存在，但每次仍是一次完整 core mutex 收购/释放；生产每完成事件固定 2 次 core 锁往返投递空回调（TP E-30 的表述准确）。合并边 M-62 采用 TP E-30 的机工表述，CP C3 的"零成本"应读作"快速返回"而非"零锁开销"。两段裁决（KEEP + 登记未证假设）不变。
2. **TP E-37 范围扩展（部分改判）**：TP 段仅登记 `ThreadPoolBackend::close_admission` 双实现问题；本会话 grep 证实 `UringAsyncBackend::close_admission`（uring_backend.hpp:87 声明，:691-701 实现，另有 :72 空体 no-ring 变体）同样生产零调用。M-12 的 UNPROVEN 裁决从"TP 双实现二选一"扩展为"两个 backend 版 vs context 直连版的所有权裁决"。
3. **UR §4.2 归类更正（未改裁决）**：UR 段把"任务范围声明与代码不符（Direct I/O 缺席）"列为文档↔代码不符。按 TP §8 与 CP D6/D7 的一致口径，任务所指"Direct I/O"是直呼形态（blocking_file.cpp），该形态存在且已完整入 DAG；O_DIRECT 缺席属实但属解读口径差异。已从 §5 不符清单移至口径注记（§5-7）。

无推翻整边裁决的情形：三段对各自边的最终裁决（KEEP/ELIMINATE_CANDIDATE/UNPROVEN）经抽查全部站得住；TP E-02 与 UR E03 关于双层校验的"方向相反"结论实为互补（前者保 context 层、后者候选 backend 层 state-op 半边），合并为 M-02 + M-16 的单一收敛方向。

## 7. 抽查核实记录（本会话执行的全部命令与命中）

只读命令（`sed -n` / `grep`），未编译未运行测试/基准，除本文件外无写入。

1. **E-23 syscall_count_**：`sed -n '425,440p' src/async/threadpool_backend.cpp` → :434 `syscall_count_.fetch_add(1, std::memory_order_relaxed);` 且不在任何 `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` 内；`sed -n '250,262p' include/sluice/async/detail/threadpool_backend.hpp` → :258 成员无门控；`sed -n '100,115p' src/async/threadpool_test_seams.hpp` → :108-110 `syscall_count_for_test`，文件头 :4 `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)`；`grep -n 'SLUICE_ASYNC_INTERNAL_TESTING' xmake/libraries.lua xmake/tests.lua xmake/apps.lua` → libraries.lua 零命中、tests.lua 14 处、apps.lua:4 注释。**属实**。
2. **E-12 borrow**：`sed -n '195,210p' request_core.cpp` → :200 `slot->borrow = borrow;`；`:610,620p` → :614 `slot.borrow = {};`；`grep -rn '\.borrow\b|->borrow\b|borrow\.' src/ include/ tests/ apps/`（排除 borrow_touch/BorrowFacts）→ 仅写/清/测试 driver 局部构造，无读者；`grep -n 'struct SlotObservation|struct CoreSnapshot' -A 14 request_core.hpp` → 两结构均无 borrow 字段；`sed -n '580,590p' docs/explicit-io-contract.md` → :586 "…borrow facts…"。**属实**。
3. **UR E12 remove_exact**：`sed -n '99,135p' uring_internal.hpp` → :118-133 扫描+前移、:109-116 pop_front O(1)；`sed -n '75,90p' uring_transport.cpp` → :82 `front()`；`sed -n '615,635p' uring_backend.cpp` → :624 `front()`；`sed -n '584,595p' uring_transport.cpp` → :589 cancel_key 任意位置删除。**属实**。
4. **CP A3 路由扫描**：`sed -n '545,560p' uring_transport.cpp` → :551-557 `find_live_router_cookie_` 逆向线性。**属实**。
5. **CP C4 owed sweep**：`sed -n '655,690p' uring_backend.cpp` → :658-663 门、:664 循环、**:668 锁在循环体内**、:670-671 `continue` 在锁内。**属实**。
6. **UR E17 submit_flushes_**：`sed -n '215,235p' uring_transport.cpp` → :219 ledger 空提前返回、:222 fetch_add 无门控；`sed -n '1,15p' uring_test_seams.hpp` + `sed -n '70,82p'` → 文件级测试宏 + :75-77 读者。**属实**。
7. **TP E-30/E-33 sticky 与空 sink**：`sed -n '22,28p' async_io_context.cpp`（initiation_rejection）、`:429,455p`（:433-435 health 双读、:449-453 exhaustion）；`sed -n '370,385p' threadpool_backend.cpp`（:377-383 claim→on_ready→retire）；`sed -n '1,30p' reference_ready_sink.hpp`（on_ready 生产空体）；`sed -n '470,490p' request_core.cpp`（:477 queue_armed_observer）；`:500,532p`（:504-508 already_terminal、:513-531 claim 快速返回仍持锁）；`sed -n '236,246p' request_core.cpp` + `grep wait_health_failed progress_source.hpp`（sticky 只置不清）。**属实**。
8. **CP B1/B5**：`sed -n '340,360p' request_core.cpp`（:356-358 臂）+ `grep -rn zero_effect_cancel`（生产零生产者）；`:378,393p`（acquire_execution）+ `grep -rn acquire_execution`（生产零调用）。**属实**。
9. **TP E-40 compose 不对称**：`sed -n '272,290p'`（prevalidated_pread_step + 入口论证注释）+ `:336,368p`（write_all_at 每步 `write_at` 全量重校验）。**属实**。
10. **UR E03 双层校验**：`sed -n '90,137p' uring_backend.cpp`（validate_* 同式 precheck_state_op）+ `sed -n '22,28p' async_io_context.cpp` + `sed -n '14,22p' async_backend.hpp`（Uninstalled seam）。**属实**。
11. **M-12 close_admission**：`sed -n '830,850p' threadpool_backend.cpp` + `grep -rn close_admission src/ include/ apps/ tests/`（backend 版仅测试调用）+ `sed -n '840,850p' async_io_context.cpp`（context 直连同构）。**属实（并扩展到 uring）**。
12. **TP E-28/CP C5 快照形态**：`sed -n '545,616p' threadpool_backend.cpp`（单 work_mtx_ 快照 + 锁外交付）。**属实**。
13. **TP E-36 ring 摘除**：`sed -n '60,90p' threadpool_backend.cpp`（remove_exact 扫描+前移）。**属实**。
14. **m1-results 裁决**：`sed -n '160,170p' docs/research/state-complexity/m1-results.md`（H1/H2 PROPOSE_UPSTREAM + 数据、H3 REJECT/INVESTIGATE）+ `:225,232p`（H2a 定义）。**属实**。
15. **不符项核实**：`sed -n '20,30p' uring_backend.hpp` + `grep -n 'statx|sys/stat' uring_backend.cpp`（注释过期）；`sed -n '16,22p' async_io_context.cpp`（PROG-02 注释）；`sed -n '55,66p' threadpool_backend.hpp` + `grep dispatch_occupancy|active_workers`（仅测试调用）；`sed -n '516,548p' progress_source.hpp`（port 措辞张力）；`grep -in 'lto|ipo' xmake.lua xmake/*.lua`（零命中，CP 段跨 TU 调用论断的前提成立）；`sed -n '330,345p' threadpool_backend.cpp` + `sed -n '553,576p' uring_backend.cpp`（begin_publication 恒 nullptr）；`sed -n '480,500p' threadpool_backend.cpp`（:483 errno=0）。**全部属实**。

## 8. 段边界遗留（登记不裁决）

- TP 段登记：`AsyncIoContext::wait_one` 在 `:581-587` 与 `:589-593` 存在背靠背两次 access_mtx_ 临界区（合并为单一临界区行为等价）——归 park/wait 段读者，本合并沿用登记。
- UR 段声明：liburing 以系统头 2.13 为引用基准，构建期 2.13/2.14 解析由 `xmake/uring.lua:203-299` probe 决定，未运行该 probe。
- UR 段声明：E26（M-50）内核侧行为为 io_uring UAPI 语义描述，非本会话对内核源码或运行的验证。
- CP 段登记：CP C1 的"弹出与 pass_bound 快照可合并为批量搬移"未列候选（量级小于 M-63），本合并沿用。
