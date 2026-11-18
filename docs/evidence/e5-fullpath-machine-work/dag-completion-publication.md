# DAG 段：CQE 获取/路由 → RequestCore terminal/execution 退休 → 发布/观察者/ProgressSource → 应用消费与 buffer 复用

- 勘察时间：2026-11-18（DAG 段读者会话，e5-fullpath-machine-work）
- 基线：master `5d94f233`（任务给定的冻结基线；本会话未执行任何 git 写操作，仅读取工作树）
- 方法：**纯源码阅读**。未编译、未运行任何测试/基准（同机正在进行频率测量，任务禁止）。
  因此所有"内联/编译器行为"判断均为源码层推断，**未经编译验证**；本文件中没有任何
  一条结论来自运行结果。读取手段：Read 工具 + `grep`/`wc`/`sed` 只读命令。
- 事实来源：当前源码为唯一事实。文档（root/ledger/m1-results/history-digest）只用于
  (a) 给语义义务编号、(b) 提供此前已裁决的机制证据作为冗余假设的上下文；凡文档与代码
  对照处均单独标注（见 §5）。
- 调用次数≠指令数纪律：每条边记录**实际每事件 CPU 工作**（mutex 获取/原子 RMW/真实
  跨 TU 调用），不是函数调用计数。关键事实：`xmake.lua` 全文无 LTO/IPO 配置
  （xmake.lua:1-89 无任何 lto/ipo 标志），所以 backend TU（uring_*.cpp）对
  `RequestCore` 方法（src/async/detail/request_core.cpp）的调用是**真实跨 TU 函数调用
  + 每次调用内含一次 mutex 获取/释放对**（core_ 是普通指针，request_core.hpp:166 无虚
  函数，非虚直调）。同 TU 内的调用（如 handle_one_cqe→finalize_operation_terminal_）
  有内联可能但未经编译验证，逐边标注。

## 0. 阅读文件清单

主读：`src/async/uring_completion.cpp`、`src/async/detail/request_core.cpp`、
`include/sluice/async/detail/progress_source.hpp`。
顺调用链：`src/async/uring_backend.cpp`、`src/async/uring_transport.cpp`、
`src/async/uring_internal.hpp`、`include/sluice/async/detail/uring_backend.hpp`、
`include/sluice/async/detail/async_backend.hpp`、`include/sluice/async/detail/request_core.hpp`、
`include/sluice/async/detail/request_key.hpp`、`include/sluice/async/detail/ready_sink.hpp`、
`include/sluice/async/detail/reference_ready_sink.hpp`、`include/sluice/async/detail/observer_protocol.hpp`、
`include/sluice/async/detail/fail_fast.hpp`、`src/async/async_io_context.cpp`、
`include/sluice/async/async_io_context.hpp`、`include/sluice/async/request.hpp`、
`include/sluice/async/request_scope.hpp`、`src/async/request_scope.cpp`、
`src/async/threadpool_backend.cpp`（同 DAG 边的对照实现）、
Direct I/O 形态：`src/blocking_file.cpp`、`include/sluice/detail/file_semantics.hpp`、
`include/sluice/detail/posix_retry.hpp`、`include/sluice/detail/uring_submit.hpp`、
应用消费：`apps/sluice-copy/copy_task.cpp`、`apps/sluice-hash/hash_task.cpp`（grep/tail 抽查）、
支撑：`include/sluice/effect.hpp`（EffectReport）、`include/sluice/measurement.hpp`（AsyncStats）、
`include/sluice/error.hpp`（from_errno_value）、`xmake.lua`（内联/LTO 配置）、
`xmake/tests.lua`（mutant 注册，仅作为证据指针）、
`bench/mechanisms/run_mechanisms.sh`（既有机制实验入口，仅作为证据指针）。

段边界：SQ 侧（reserve/accept/dispatch/submit、ledger、poison 起源）属上游段；本段从
CQE 可见开始。worker 侧执行（threadpool worker_loop / run_syscall）因是 terminal 的
生产端且是本路径上 Direct 原语的 async 对应实现，纳入 D 段记录。

---

## A 段：CQE 获取/路由（src/async/uring_completion.cpp）

### A1. reap_cqes 批量收割主循环
- 锚点：src/async/uring_completion.cpp:200-229（BATCH=32 :201；peek :210；cqe_seen :220；入口界 :229；overflow flush :238-277）
- 语义义务：PROG-01 有界非阻塞 pass 的 CQ 侧收割；pass 边界 = 入口可见 CQ 状态
  （:204-206 注释，代码一致）；overflow 停靠完成的 flush 义务（:240-250 注释）+ 失败
  flush 的 sticky health（:268-274）。
- 当前操作：`io_uring_cq_ready` 定界 → `io_uring_peek_batch_cqe`（≤32/批）→ 逐条
  `cqe_get_data64`/`cqe_seen` → `handle_one_cqe`；overflow 分支 `io_uring_get_events`
  （可被 test hook 替换 :259-267）+ `reconcile_ledger_with_kernel_locked_` + 二次收割 :276。
- CPU 工作：liburing 头内 inline helper（共享内存读 khead/ktail；`peek_batch_cqe`
  循环无系统调用）；每 CQE 一次 `cqe_seen`（向共享页 khead 的 store，relaxed）。
  `handle_one_cqe` 同 TU（99 行起），可内联（未验证）。收割侧本身无锁；锁在 A2。
  overflow 分支一次 `dispatch_mtx_`（:269）+ ledger reconcile（含 O(ledger) 弹出）。
- 内核工作：peek/cq_ready/cq_has_overflow 零系统调用；仅 `io_uring_get_events` 是一次
  enter（flush overflow 时）。
- 内存搬移：每 CQE 读 16B 头（user_data+res）；无 payload 搬移（payload 由内核已在
  C 阶段直写用户 buffer）。
- 同步：无收割侧锁；khead store 对内核为发布点（REQ-04 借出终止的用户侧证明起点，
  真正的 acquire 在 D3 消费端 mutex 路径上完成）。
- 冗余假设：无独立候选；BATCH=32 与环深无关的定值是常量而非机制。
- 证据：本会话直读上述行。
- 删除的正确性风险：删除入口定界（:229 的 bound）使 pass 长度在持续到达下无界，违反
  PROG-01 finite bound；删除 overflow flush 使停靠完成永久不可见（借出永不能退休）。
- 裁决：KEEP

### A2. handle_one_cqe 帧：分发锁与 cookie 分派
- 锚点：src/async/uring_completion.cpp:99-151（锁 :100；control 分支 :101-144；零 cookie 丢弃 :145-146；路由查找 :148-150；prep 读取 :153-157）
- 语义义务：THREAD-02（backend 私有同步；每 CQE 线性化）；完成身份与路由解耦
  （cookie 单调域 :526-538 携带身份，CONTROL_TAG :45 分隔控制面）；未知/零 cookie
  丢弃是探针残留 CQE 的合同诊断（probe_statx_support_ 用 cookie 0，uring_backend.cpp:245）。
- 当前操作：获取 `dispatch_mtx_` → 控制位判断（1 条 AND）→ 非控制路径查路由 →
  读 `prepared_ops_` 判 byte/metadata 类。
- CPU 工作：每 CQE 一次 mutex 获取/释放（无竞争时约 1 对 atomic RMW）；2 次
  `live_control_sqes_` 相关 relaxed 原子（仅控制分支）；一次 O(router_) 扫描（见 A3）。
- 内核工作：无。
- 内存搬移：无（prep 指针读取）。
- 同步：`dispatch_mtx_`（与 submitter/canceler 线程互斥，THREAD-01 并发矩阵要求）。
- 冗余假设：锁粒度——每 CQE 一锁 vs 批收割共享一锁；但 finalize 内要做 core 交互与
  队列操作，粗化锁只会延长临界区（flow-audit H2 已把"submitter 侧长临界区 vs driver
  每 CQE 短临界区"记为结构已证，docs/research/595-flow-audit/HYPOTHESES.md H2）。
  无候选。
- 证据：直读。
- 删除的正确性风险：去锁则 dispatch/ledger/router 与并发 submitter 撕裂。
- 裁决：KEEP

### A3. find_live_router_cookie_ 路由查找（O(R) 逆向线性扫描）
- 锚点：src/async/uring_transport.cpp:551-557（实现）；调用点 uring_completion.cpp:105、:148
- 语义义务：把内核回填的 user_data（操作 cookie）映射回 RouterEntry。cookie 是单调
  计数（allocate_cookie_ :526-538，0 保留、≥CONTROL_TAG fail-fast），不是路由槽索引；
  控制面 cookie 在同一位域上打 tag（uring_internal.hpp:45-58）。
- 当前操作：逆向线性扫描 `router_`，比较 `in_use && cookie`，未命中返回 size 哨兵
  （调用方 :106/:149 判哨兵后静默返回）。
- CPU 工作：每 CQE 最多 capacity_ 次比较；逆向扫描（:552 `i--`）偏好近期条目。
- 内核工作：无。内存搬移：无。同步：调用方持 `dispatch_mtx_`。
- 冗余假设：**扫描可被 user_data 编码路由索引替换（H1）**：m1-results.md:165
  PROPOSE_UPSTREAM（O(R)→O(1)，实测 0.41x @R=64、0.05x @R=1024、平坦于 R；
  oracle+碰撞负控 PASS），机制基准已存在 bench/mechanisms/h1_router_index_bench.cpp
  （run_mechanisms.sh:35 编译）。编码义务（15-bit/48-bit 域界+config 校验+fail-fast）
  在 m1-results.md:165 同行列出。本会话未复测（任务禁止运行）。
- 证据：源码直读 + 上述既有机制文档（作为假设上下文，非本会话测量）。
- 删除的正确性风险：cookie 域承担多重证明：TransportLedger 物理位置单调检查
  （uring_internal.hpp:170-191）、控制 CQE 必须命中 exact submitted 引用
  （uring_completion.cpp:110-116）、cancel 定向（io_uring_prep_cancel64 :679 以
  cookie 为目标）。索引编码必须保持"cancel 目标不会跨代重用别名"，迁移未证。
- 裁决：ELIMINATE_CANDIDATE（扫描本身；替换机制已有 PROPOSE_UPSTREAM 既往裁决，
  但迁移对 cookie 域证明义务的影响未证）

### A4. control-CQE 处理（控制退休）
- 锚点：src/async/uring_completion.cpp:101-144（invariant :110-116；状态转移 :117-118；
  release_control :119-122；mutant :123-135；路由退休 :136-142）
- 语义义务：每条控制 CQE 退休恰好其提交过的控制引用（BACKEND-03 引用记账；REQ-05
  第三条"no backend or internal control reference"；CANCEL-01 sticky intent 必须被
  服务，不留悬空控制）。
- 当前操作：解码目标 cookie → 查路由 → 校验 `control_state==submitted` 且
  `live_control_sqes_!=0`（否则 terminate）→ 状态归 none → `live_control_sqes_`
  relaxed fetch_sub → core `release_control` → 若原操作已交付终局则退休路由条目。
- CPU 工作：A3 的扫描 + 1 对 mutex（`dispatch_mtx_` 持有中）+ 1 次 core mutex
  （release_control，跨 TU 调用）+ 1 次 relaxed RMW。
- 内核工作：无。内存搬移：RouterEntry 重置（几十字节）。
- 同步：`dispatch_mtx_` + core mutex（锁序 dispatch→core，与 submit 路径一致）。
- 冗余假设：`live_control_sqes_`（原子）是 router 扫描的派生摘要（∈{count(control_state
  ==submitted)}），用作 :111 的 O(1) 不变量 witness 与结算见证（uring_backend.cpp:351、371）；
  与 owed_count_（#634/H2b）同构的 justified derived summary，保留权威扫描于结算。
- 证据：直读；ledger "Dated 621 ThreadPool owed-count" 行（docs/roadmap/v2-conformance.md:454+）
  为同构机制的既往裁决。
- 删除的正确性风险：释放 release_control 则 control_refs 泄漏 → reclaimable_ 永假
  （request_core.cpp:584）→ 槽位与结算永不能收敛。
- 裁决：KEEP（含 live_control_sqes_ 派生摘要）

### A5. 物理结果解码（errno 映射 / 字节结果 / statx metadata）
- 锚点：src/async/uring_completion.cpp:153-195（errno 分支 :161-171；字节 :172-173；
  metadata :174-192；mask 门 :175-176、:184-189）
- 语义义务：ERR-01 物理 errno→IoError 域映射（from_errno_value，error.hpp:92-94）；
  字节操作成功即 confirmed_bytes=res；SEM-07 最小 metadata：只在内核真的报了
  STATX_SIZE/TYPE/MODE 才成功、只在报了 STATX_INO 才发布身份（:182-184 注释声明
  "statx 未请求的 mask 字段未定义"，代码一致）。
- 当前操作：按 res 符号与 prep.kind 分支；metadata 时从 `statx_buffers_[slot]`（submit
  时挂接，uring_backend.cpp:479-486）填充 `sluice::FileInfo`（kind/size/identity）。
- CPU 工作：少量分支 + 从 errno 到枚举的映射；FileInfo 构造 ≈ 数十字节字段写。
- 内核工作：无（statx 结果由内核在 C 阶段写入该缓冲）。
- 内存搬移：metadata 操作每事件 statx 缓冲 → FileInfo ≈ 40-56B 拷贝。
- 同步：调用方持锁；statx_buffers_ 按 slot 索引，路由仍在用时无他人写。
- 冗余假设：无。mask 门是 SEM-07 义务不是防御。
- 证据：直读。
- 删除的正确性风险：删 STATX_INO 门会把未定义值当身份发布（SEM-07 违约）。
- 裁决：KEEP

### A6. finalize_operation_terminal_（终局交付 + 借出退休 + 发布入队）
- 锚点：src/async/uring_completion.cpp:23-97（invariant :26-31；候选构造 :33-69；
  offer :70-75；stats :77-83；delivered 标志 :85；release_execution :89-92；入队 :92；路由退休 :94-96）
- 语义义务：REQ-04 发布边界的后端侧：终局经 core 权威一次性选定（offer 非 chosen 即
  terminate，:70-75）；借出退休必须拿到 `borrow_touch_fully_retired` 见证（:89-91，
  uring_core_handoff_fail_fast）；交付后立即把 reservation 排进 publication_pending_
  （kernel borrow 在下一阶段正式发布前先完成 execution 退休）；控制面未清时保留路由
  （:94-96）。
- 当前操作：构 RequestKey；按 kind/结果构 TerminalCandidate（byte 成功带 confirmed_bytes；
  失败分 kernel-cancel 与普通错误，分别走 canceled_racing_in_flight_attempt(0)
  /failed_dispatched_attempt（file_semantics.hpp:334-345，uncertain 语义=前缀效应未知）；
  metadata 带 FileInfo）；core offer_terminal；stats bump（stats_ 可空，uring_internal.hpp:60-63）；
  core release_execution；`publication_pending_->push_back`；路由退休（含 live_cookies_
  relaxed fetch_sub，uring_transport.cpp:581）。
- CPU 工作：同 TU 调用（23 行定义在 handle_one_cqe 前）可内联（未验证）；2 次 core
  mutex（offer_terminal + release_execution，均为跨 TU 真调用）；BoundedDispatchQueue
  push（环形回绕算术+满队 terminate，uring_internal.hpp:99-108）；reclaim 判定挂在
  release_execution 内（B6）；1 次 relaxed fetch_sub；stats 3 分支。
- 内核工作：无。
- 内存搬移：TerminalCandidate（IoOutcome ≈40B，metadata 时 +FileInfo）拷入 slot；
  slot 若即时 reclaim 则再清一遍（B6）。
- 同步：`dispatch_mtx_`（持有中）+ core mutex ×2 + relaxed 原子。
- 冗余假设：(i) `route.terminal_delivered` 是 slot.terminal_chosen 的 backend 本地镜像
  （每次 offer 必 chosen 才继续），避免为读 core 状态再加锁——justified derived
  summary，非冗余。(ii) :26-31 的调用方已验证前提再验一次，属 root 要求的 fail-fast
  诊断（BACKEND-03），非防御性冗余。
- 证据：直读；`#621 progress-drain` merge（git log 27d907d6）确认该路径近期唯一改动
  在 ProgressSource 侧而非本函数。
- 删除的正确性风险：release_execution 删除则借出引用永不退休——reclaim 前置
  execution_refs==0 永假，槽位泄漏、settlement 不收敛；publication 入队删除则结果
  永不 published（app 侧 ready() 永假）。
- 裁决：KEEP

---

## B 段：RequestCore terminal/execution 退休（src/async/detail/request_core.cpp）

### B1. offer_terminal（终局选定权威）
- 锚点：src/async/detail/request_core.cpp:346-365（duplicate :353-355；zero_effect_cancel 仲裁 :356-358；写入 :359-363）
- 语义义务：REQ-04 terminal 恰好一次；CANCEL-01"一旦执行可能有效应，core 等物理结果，
  intent 不能选定虚构的零效应取消结果"（root docs/explicit-io-contract.md:1057-1059）。
- 当前操作：mutex；resolve_internal_（界/context/phase/generation 四查 :121-132）；
  duplicate 拒绝；`zero_effect_cancel && execution_claimed → rejected_inadmissible`；
  写 terminal_chosen + outcome（+可选 metadata）。
- CPU 工作：1 次 core mutex；4 项校验；IoOutcome（≈40B）拷贝，metadata 时 +FileInfo。
- 内核工作：无。内存搬移：如上。同步：core mutex。
- 冗余假设：**`TerminalCandidateKind::zero_effect_cancel` 仲裁臂在当前源码中没有生产
  生产者**：grep 全仓（src/include/apps/tests）仅 tests/request_core_test_driver.hpp:180
  构造该 kind；两个 backend 的全部 offer 点都只发 `physical_outcome`
  （uring_completion.cpp:38、uring_transport.cpp:57/116/354/404、threadpool_backend.cpp:310/446）。
  CANCEL-01 同一规则在生产上由 `cancel()`（B2，:269-274）在另一入口强制。该臂是否
  是 seam 必备义务（供未来 backend / 协议完备性）还是可删的第二强制点，需 owner 裁决。
- 证据：grep 结果如上（本会话运行）；root 文本 explicit-io-contract.md:1057-1059。
- 删除的正确性风险：删除仲裁臂后，若任何 offer 路径以 zero_effect_cancel 命中
  execution_claimed 的槽，会覆盖出虚构零效应取消终局——当前无生产路径能触达，
  但 seam 协议完备性损失。
- 裁决：UNPROVEN（仅 zero_effect_cancel 臂；offer_terminal 其余部分 KEEP）

### B2. cancel（won-before-execution 终局）
- 锚点：src/async/detail/request_core.cpp:260-275（execution_claimed→requested :269-271；选定 canceled_before_dispatch :272-274）；调用方 uring_transport.cpp:584-618 / threadpool_backend.cpp:782-814
- 语义义务：CANCEL-01 预派发取消只能在证明无派发/效应时获胜；胜出后 publication 仍按
  REQ-04 走（uring_transport.cpp:591-596 release_execution+入队）。
- 当前操作：mutex；resolve_public_；terminal 已选→already_terminal；execution_claimed→
  requested（物理中断另由 backend 决定，uring 侧 issue_running_cancel）；否则写
  canceled 终局并返回 won。
- CPU 工作：1 次 core mutex + resolve 校验；生产路径 win 后由调用方继续
  release_execution（B3）。
- 内核工作：无（本函数）；物理取消 SQE 属上游段。
- 内存搬移：IoOutcome 一份。
- 同步：core mutex。
- 冗余假设：与 B1 的 zero_effect_cancel 臂互为同一规则的两个强制点（见 B1）。
- 证据：直读。
- 删除的正确性风险：删 execution_claimed 检查会让取消覆盖可能已生效的 I/O（CANCEL-01
  核心违约，ERR/SEM 效应报告错误）。
- 裁决：KEEP

### B3. release_execution（借出退休见证）
- 锚点：src/async/detail/request_core.cpp:393-409（underflow :399-401；premature :402-404；递减+fully_retired 见证 :405-408）
- 语义义务：BACKEND-03 引用记账：退休必须在 terminal 已选后发生
  （premature_rejected 是违约诊断）；返回值 `borrow_touch_fully_retired` 是所有
  backend 交付点 fail-fast 的前提（A6/B2/poison 路径统一断言）。
- 当前操作：mutex；resolve；`execution_refs==0 → underflow`；
  `refs==1 && !terminal_chosen → premature`；递减；fully_retired = refs==0 && chosen；
  try_reclaim_（B6）。
- CPU 工作：1 次 core mutex；小算术；reclaim 谓词 6 项（:586-589）；可能 slot 清理
  （B6）。
- 内核工作：无。内存搬移：reclaim 时 slot 清理（B6 计）。
- 同步：core mutex。
- 冗余假设：premature_rejected 在生产交错中不可达（所有 release 点都先 offer_terminal：
  A6、uring_transport.cpp:121-123/:366-368/:417-420、cancel :592-595、threadpool :457-459），
  属合同诊断而非可达状态防御——按 AGENTS.md 机制门槛属"root 要求的合同诊断"，保留。
- 证据：直读；枚举定义 request_core.hpp:103-109。
- 删除的正确性风险：删 underflow/premature 拒绝会把引用记账错误静默化（退化为
  回绕/提前退休），REQ-05 第四条的"no execution refs"前提失真。
- 裁决：KEEP

### B4. claim_execution（执行主张，退休协议对端）
- 锚点：src/async/detail/request_core.cpp:367-378；生产调用 uring_transport.cpp:113/:152、threadpool_backend.cpp:406
- 语义义务：把"借出已派发"与"可获胜取消"线性化：claim 晚于取消取消胜、claim 后取消
  只能 requested（CANCEL-01/THREAD-01 交错）。
- 当前操作：mutex；resolve；`execution_claimed || terminal_chosen → late`；置位。
- CPU 工作：1 次 core mutex；两次 bool 判定。
- 同步：core mutex。
- 冗余假设：无（A2 的 dispatch 失败回滚 SQE tail 正依赖 late 结果，uring_transport.cpp:152-158）。
- 证据：直读。
- 删除风险：取消与派发竞态失去仲裁点。
- 裁决：KEEP

### B5. acquire_execution（生产零调用）
- 锚点：src/async/detail/request_core.cpp:380-391；声明 request_core.hpp:215
- 语义义务：（意图上）多执行方借出的引用累加，护栏：terminal 已选或已发布则拒绝。
- 当前操作：**生产源码零调用**。grep src/include/apps：无生产调用者；仅
  tests/request_core_protocol_test.cpp:544/:573/:593/:604/:612 经 test driver 调用
  （driver 转发 request_core_test_driver.hpp:187-188）。生产生命周期是 accept 置 1
  （:201）→ release 减 1，refs>1 从未在生产发生。
- CPU 工作：热路径零（无生产执行边）。
- 冗余假设：**生产无消费者的 seam 操作**。属 BACKEND-01 封闭 seam 的 API 面 +
  协议完备性测试面；删除节省仅为代码面，不影响任何执行边。按 AGENTS.md"不得因无
  当前消费者删除必需义务"——是否"必需"未证。
- 证据：grep（本会话）；测试引用行如上。
- 删除的正确性风险：seam 测试失去多借出协议的表达；若未来 backend 需要，需重加。
- 裁决：UNPROVEN（热路径成本为零；义务归属需 owner 裁决）

### B6. reclaimable_/try_reclaim_/release_slot_（REQ-05 回收谓词与重初始化）
- 锚点：src/async/detail/request_core.cpp:580-597（谓词 :586-589）、:599-631（release_slot_；generation 边界 :621-630）
- 语义义务：REQ-05 五条件回收谓词（public-terminal；binding 已消费/丢弃；无控制引用；
  无执行引用；无在途发布；无活观察者投递）+ 代际纪律重初始化（"Slot reuse must
  reinitialize the publication state under the generation discipline"）。
- 当前操作：谓词判定；release_slot_：清 7 个状态位/observer 相位/refs、清
  descriptor/borrow/outcome/metadata、generation+1 或 retired（饱和进 retired_count_
  并不再入 free list :621-628）、free_slots_.push_back（构造时 reserve 过，:41，无重分配）。
- CPU 工作：6 项谓词；约 12 次 store + 1 次 vector push（摊销 O(1)）；全在调用方
  持有的 core mutex 内。
- 内核工作：无。内存搬移：slot 状态清零 ≈ 数十字节（descriptor/borrow/outcome/metadata）。
- 同步：core mutex。
- 冗余假设：outcome/metadata/descriptor 的显式清除看似冗余（发布协议保证覆盖先于观察：
  observe/consume 只在 published 后读 outcome，:308/:330；accept 总是重写 descriptor/borrow，
  :199-200）——但它们是 **DP-3A 初始化闭包**的载荷：reserve() 信任 free-list 前置条件，
  release_slot_ 是唯一重初始化点；注册 mutant `request_core_mut_release_skips_{outcome,
  metadata,observer}_clear` 等由 request_core_init_closure_test 杀伤
  （xmake/tests.lua:339-363 注释与注册）。KEEP。
- 证据：直读 + xmake/tests.lua:339-363（mutant 注册与注释，作为证据指针）。
- 删除的正确性风险：脏槽重用（stale outcome/metadata 混入下一代请求的诊断面）+
  reserve 前置条件破坏（闭包失败）。
- 裁决：KEEP

### B7. acquire_control / release_control（控制引用记账）
- 锚点：src/async/detail/request_core.cpp:411-422（拒绝条件 :417）、:424-436；调用点 uring_transport.cpp:675（cancel 控制）、uring_backend.cpp:555（owed）、:681（owed 释放）、uring_completion.cpp:120（控制 CQE 释放）、threadpool_backend.cpp:335/:594
- 语义义务：REQ-05 第三条——槽位在控制引用存在期间不可回收（cancel 控制可迟到；
  owed 事件在交付前必须钉住槽位）。
- 当前操作：mutex；resolve；acquire 侧拒绝"published && !binding_live && refs==0"（该
  槽即将回收，复活引用非法）；加减 refs；release 侧 try_reclaim_。
- CPU 工作：各 1 次 core mutex + 小算术。
- 同步：core mutex。
- 冗余假设：无独立候选；owed 控制引用与 cancel 控制引用共用一套 refs（单计数器两种
  来源），若拆分反而加状态。
- 证据：直读。
- 删除风险：控制 CQE/owed 交付触碰已回收重用槽（代际别名错终局）。
- 裁决：KEEP

---

## C 段：发布 / 观察者 / ProgressSource

### C1. poll() 发布队列排空循环（uring）
- 锚点：src/async/uring_backend.cpp:616-689（dispatch/submit :620-634 为上游段边界；
  pass 界快照 :640-643；逐条弹出 :646-657，锁 :650）
- 语义义务：PROG-01（发布属 pass 义务）；pass 边界 = 入口 publication_pending_ 尺寸
  （:645-646 注释与代码一致）；条目在 pass 中新产生则留给下一 pass 并由
  immediate_work_remains 诚实上报。
- 当前操作：取 pass_bound（1 锁）→ 循环：每条 1 次 `dispatch_mtx_` 弹出（:650）→
  publish_one（C2）。
- CPU 工作：每发布条目 1 次 mutex（弹出）+ C2 的 2-4 次 core mutex；无拷贝（只传
  reservation 24B）。
- 内核工作：无（收割在 A 段）。内存搬移：无。
- 同步：`dispatch_mtx_` 每条一次。
- 冗余假设：弹出锁逐条获取——发布本体（core 调用）必须在锁外（THREAD-02/OBS-02），
  但"弹出"可以与 pass_bound 快照合并为一次锁下批量搬移（threadpool 的 publish_batch_
  形态，threadpool_backend.cpp:562-568）。属机器工时候选而非语义冗余；量级小于 C4。
- 证据：直读。
- 删除风险：不适用（结构性）。
- 裁决：KEEP

### C2. publish_one + begin_publication / complete_publication
- 锚点：src/async/uring_backend.cpp:578-608；request_core.cpp:438-460（begin）、:462-484（complete）
- 语义义务：REQ-04 发布边界：begin 校验资格（terminal 已选、无执行引用、binding 活）
  并在 publication_inflight 钉住下快照；complete 置 published（release 边界）、
  减 outstanding、把 armed 观察者置 queued、尝试回收。交付（C3）必须在锁外。
- 当前操作：begin（core mutex）；读 `record.kind`（依赖 publication_inflight 钉住 +
  队列转移已加锁的交接协议，uring_backend.hpp:193-197 注释，代码一致）；complete
  （core mutex，含 --outstanding_ :473、observer 入队 :474-481、try_reclaim_ :482）；
  deliver_event（C3）。
- CPU 工作：2 次 core mutex（跨 TU 真调用）；kind 字段读；核心态写 ≈ 几十字节。
- 内核工作：无。内存搬移：无大搬移——**生产路径 begin_publication 一律传 nullptr**
  （uring_backend.cpp:558/:584、threadpool_backend.cpp:338/:358），PublicationPayload
  快照拷贝（≈80B，request_core.cpp:451-458）只被测试填充
  （tests/request_core_protocol_test.cpp:635 等、request_core_consumer_probe.cpp:117）。
- 同步：core mutex ×2。
- 冗余假设：**PublicationPayload out 参数无生产消费者**——快照拷贝分支仅测试可达，
  生产每次发布只付出一次空指针判定。候选：payload 快照可下沉为测试专用通道；量级
  极小（1 分支/发布），追问价值在代码面而非热路径。
- 证据：grep begin_publication/PublicationPayload（本会话，见 §5 前的检索记录）。
- 删除风险：payload 快照删除需重排 seam 测试（consumer probe 以 payload 为消费面）。
- 裁决：KEEP（payload-out 子项 ELIMINATE_CANDIDATE，低优先）

### C3. deliver_event + 观察者 claim/retire
- 锚点：src/async/uring_backend.cpp:610-614；request_core.cpp:513-531（claim）、:560-572（retire）；sink：reference_ready_sink.hpp:9-28（生产 no-op）
- 语义义务：OBS-02 至多一次投递、hook 在全部 core/backend 锁外、不由提交或 worker
  内联调用；OBS-03 armed→queued→delivering→retired 状态机（observer_protocol.hpp:7-13）。
- 当前操作：claim_observer_delivery（core mutex；无注册时返回 none 快速路径）→
  `on_ready(ReadyEvent)`（生产 sink_ 为 ReferenceReadySink：生产构建下函数体为空，
  测试构建下计数）→ retire_observer_delivery（core mutex）。
- CPU 工作：2 次 core mutex + 1 次虚调用（生产为空函数）。无观察者时 claim/retire
  都是快速返回。
- 内核工作：无。内存搬移：ReadyEvent ≈24B 按引用传递。
- 同步：core mutex ×2（必须不持 dispatch_mtx_ —— 两种 poll 的调用点都在锁外）。
- 冗余假设：生产中无路由 sink 挂接（routing_sink_ 仅 tests/support 与 test_access
  挂接，grep 见 async_backend.hpp:27/:91、test_access.hpp:35、pipelined_copy_oracle_support.hpp:543），
  `on_ready` 是无消费者义务——但观察者协议是 root 必备（OBS-01/02/03；AGENTS.md：
  不得删无消费者义务）。claim/retire 已是"无注册零成本"形态。
- 证据：直读 + grep attach_ready_sink（本会话）。
- 删除风险：删除 claim/retire 使 OBS-02 状态机失效（registered observer 永不投递）。
- 裁决：KEEP

### C4. owed sweep（uring 逐槽锁形态）
- 锚点：src/async/uring_backend.cpp:658-686（gate `owed_present` :661；扫描循环 :664；
  **每槽一次锁 :668**；交付+release_control :680-684）
- 语义义务：PROG-01 诚实残留："published 但交付事件仍 owed"是 immediate work
  （root explicit-io-contract.md PROG-01 段，explicit-io-contract.md:845-851 附近）；
  OBS-02 交付不得由提交线程内联调用——零长操作发布在 submit 线程完成（C6），
  交付事件由本 sweep 在 driver pass 兑现，控制引用（B7）全程钉住槽位。
- 当前操作：`owed_count_ != 0` 门（H2b 已产品化，uring_backend.hpp:261-267
  owed_visible_locked_）→ 非 0 时全容量扫描 delivery_，**每个槽位索引都获取/释放
  `dispatch_mtx_`（含无 owed 的槽：`continue` 在锁内 :670）**；owed 槽：清 flag、
  减 owed_count_、取 key/kind、锁外交付（C3）+ release_control。
- CPU 工作：owed 非空时 = capacity_ 次 mutex 获取对 + 每 owed 1 次 core mutex
  （release_control）；owed 为空时 = 1 次锁下读 gate。
- 内核工作：无。内存搬移：key/kind 复制。
- 同步：`dispatch_mtx_` ×capacity_（非空时）。
- 冗余假设：**逐槽锁是已提案未落地的 H2a**：m1-results.md:166 H2 "PROPOSE_UPSTREAM
  (uring first…)"，其 H2a 子项 = "uring sweep snapshots under one lock"（同文件 :227-231）；
  threadpool 侧已是单锁快照形态（C5），uring 侧代码仍是逐槽锁。既往机制证据
  −370 ns/pass（uring，C=64 idle，m1-results.md:166）。本会话未复测。
- 证据：直读（668 行锁在循环体内、continue 在锁内）+ m1-results.md 引用（假设上下文）。
- 删除风险：owed 机制本身删除则违反 OBS-02 内联禁令与 PROG-01 残留诚实性；仅
  锁形态改造（单锁快照）风险为"交付顺序变化"（key 顺序不变：数组序保持）。
- 裁决：ELIMINATE_CANDIDATE（锁形态，H2a；owed 机制本身 KEEP）

### C5. owed sweep（threadpool 单锁快照形态，对照边）
- 锚点：src/async/threadpool_backend.cpp:550-598（快照 :560-584 在单一 work_mtx_ 下
  装 owed_batch_；交付 :592-598 锁外）
- 语义义务：同 C4（PROG-01/OBS-02）；#634（PR #634，merge 3f22d2d0）产品化了
  `owed_count_` 派生摘要门控空扫与残留判定（ledger docs/roadmap/v2-conformance.md:71、:454+）。
- 当前操作：一次 work_mtx_ 下：publication 快照进 publish_batch_ + owed 扫描装
  owed_batch_（固定容量，构造期分配 :97，pass 内无动态分配，:556-558 注释）；
  锁外 publish_one ×n + deliver/release_control ×k。
- CPU 工作：1 次 mutex（快照）+ 每 owed 1 次 core mutex；容量扫描仍在（非空时）但
  无逐槽锁。
- 同步：work_mtx_ ×1。
- 冗余假设：无（本形态即 H2a 目标形态，作为 C4 的对照证据）。
- 证据：直读；ledger :71/:454-496（既往裁决）。
- 裁决：KEEP

### C6. publish_zero_op_inline（零长操作内联发布 + owed 事件）
- 锚点：src/async/uring_backend.cpp:553-576（控制引用 :555-557；begin/complete :558-563；
  owed 记录 :564-572；signal :573-575）；threadpool 对应 :334-354；accept 侧零长终局 request_core.cpp:203-207
- 语义义务：SEM/REQ-02 侧零长数据操作是逻辑 no-op（root explicit-io-contract.md:414）：
  accept 事务内选定终局、发布在提交线程完成；交付事件必须推迟到 driver（OBS-02
  内联禁令）→ owed 通道存在的原因；PROG-02：动作化状态出现必须留持久可观察就绪
  或安全竞过停靠握手（signal_ready_progress）。
- 当前操作：acquire_control + begin + complete（3 次 core mutex）→ work/dispatch 锁下
  写 delivery_ 记录（event_owed/owed_key）+ owed_count_++（uring:571/TP:350）→
  signal（C8）。
- CPU 工作：3 次 core mutex + 1 次 dispatch/work mutex + signal（1 次 progress mutex +
  可能 1 次 write 系统调用）。
- 内核工作：signal 时可能 write(eventfd)。
- 内存搬移：owed_key（24B）。
- 同步：core ×3、dispatch ×1、progress ×1。
- 冗余假设：**owed 通道 vs 直接入 publication_pending_**——若零长不内联完成发布而
  是推入发布队列，则可删除 owed 机制整体（C4/C5/delivery 记录/owed_count_/控制引用
  对）。但语义代价：published（app ready()）会推迟到下一 pass，改变"提交返回即
  published"的当前语义；且 owed 记录是 H2b（#564）正式裁决过的机制（ledger
  v2-conformance.md:84-85，formal E5 A/B PASS 2026-10-22，headline 0.495）。不作为
  候选，仅记录为"存在的替代形状"。
- 证据：直读；ledger :84-85。
- 删除风险：见上——语义变化而非纯机制替换。
- 裁决：KEEP

### C7. poll_progress 组装（含 has_immediate_physical_work）
- 锚点：src/async/uring_backend.cpp:304-328（锁 :311；immediate_work :312-325；physical
  probe :285-293）；threadpool_backend.cpp:604-611
- 语义义务：PROG-01 后置事实四元组（completed/immediate/accepted/health）+ 派发重试
  残留；CQ 可见性跟随内核发布协议而非 eventfd，overflow 标志覆盖停靠完成
  （:286-287 注释，代码一致）。
- 当前操作：1 次 dispatch 锁（队列/owed/重试/health）→ has_immediate_physical_work
  （cq_ready + cq_has_overflow，两次共享内存读）→ outstanding()（1 次 core mutex，
  outstanding_summary :93-96）。
- CPU 工作：1 dispatch mutex + 1 core mutex + 2 共享内存读。
- 内核工作：无系统调用。
- 同步：如上。
- 冗余假设：has_immediate_physical_work 在 pass 尾（:325）与停靠握手 probe
  （async_io_context.cpp:536-542/:637-639）各跑一次——后者是 PROG-02 第 4/5 步的竞态
  要求（drain 消费通知后只有物理探测能兜底，progress_source.hpp:68-72 注释），非冗余。
- 证据：直读。
- 裁决：KEEP

### C8. ProgressSource::signal / interrupt / wake_notification_nolock_
- 锚点：include/sluice/async/detail/progress_source.hpp:252-264（signal，饱和纪律）、:234-250（interrupt）、:466-489（wake 写抑制）；调用点：uring_backend.cpp:574、:698-703（close_admission）、uring_transport.cpp:100/:604/:614、threadpool_backend.cpp:328/:353/:466/:614/:841
- 语义义务：PROG-02——每个动作化转移要么留持久可观察就绪要么安全竞过握手；token 是
  通知 epoch 不是请求数（:26-29 注释；root PROG-02）；饱和而非回绕（:255-257 注释）；
  抑制仅允许"pending 未读 wake 存续期间"（root PROG-02 明文许可的物理写优化）。
- 当前操作：progress mutex；epoch/exhaustion 饱和递增；若 fd 已退役或 notification_pending_
  已立则跳过写；否则 write(eventfd, 8B)（EINTR 重试；EAGAIN=已挂起→置 pending）。
- CPU 工作：1 mutex + 可能 1 次 write 系统调用（合并率取决于消费速率）。
- 内核工作：write(eventfd)。
- 内存搬移：8B 计数。
- 同步：progress mutex。
- 冗余假设：notification_pending_ 位是 fd 可读状态的派生摘要（避免每次 signal 一次
  write）；root 明文许可；drain（C10）清位。无候选。
- 证据：直读；#621 round2 的 eventfd write 频率证据在 history-digest.md:83
  （262,135→131,073 reads，−50%）为 drain 侧既往证据。
- 删除风险：删抑制位 → 每 signal 一次 write 系统调用（W-02 每请求多次）。
- 裁决：KEEP

### C9. wait_if_unchanged 停靠握手
- 锚点：include/sluice/async/detail/progress_source.hpp:104-232（health :122-127；时限 :128-147；revalidate+drain 单次持锁 :153-166；probe :175-181；二次 revalidate :183-192；poll :198-203；post-poll :204-230）
- 语义义务：PROG-02 第 1-5 条的完整实现：armed、先 acknowledge 陈旧就绪、pass 后无
  动作化残留、token 未变、其后到达的工作必使等待返回。revalidate 与 drain 共享一次
  mutex 持有（:96-99 注释，代码 :153-166 一致）；probe 在锁外、报物理工作则不泊；
  probe 之后的 revalidate 不得再 drain（:98-103 注释，:183-192 一致）。
- 当前操作：每轮：health 检查（1 锁）→ 时限算术（steady_clock::now() vdso 1-2 次）→
  revalidate+drain（1 锁，可能 1 次 eventfd read）→（测试期 gate）→ probe（backend
  回调：2 共享内存读）→ 二次 revalidate（1 锁）→ poll(2)（真正泊时）→ post-poll
  判定（1 锁）。
- CPU 工作：3-4 次 progress mutex + poll 系统调用 + probe。
- 内核工作：poll(2)（泊）；drain read（见 C10）。
- 内存搬移：pollfd 1 项。
- 同步：progress mutex ×3-4/轮。
- 冗余假设：两次 revalidate（:154-166 与 :184-192）看似重复——第二次是 probe 窗口的
  竞态闭合（PROG-02 第 4 步），结构性必需。无候选。
- 证据：直读。
- 删除风险：删任一 revalidate → 泊后丢唤醒窗口。
- 裁决：KEEP

### C10. drain_notification_nolock_（#621 单读排空）
- 锚点：include/sluice/async/detail/progress_source.hpp:414-460（成功即清 pending 并返回 :437-442；EINTR 重试 :443-446；EAGAIN 清位 :447-456；fail-fast :458）
- 语义义务：文档化的陈旧就绪应答操作；contexts/hosts 只经此排空，fd 不是第二排空
  权威（:272-274 注释）。PR #635（27d907d6，merge 5d94f233）删除了成功 read 后必然
  EAGAIN 的第二次探测读。
- 当前操作：一次 read(eventfd)（非信号量 eventfd 读走全计数=排空态，:412-413 注释）；
  成功即置 `notification_pending_=false` 并返回（单读）；EINTR 重试；EAGAIN 清位。
- CPU 工作：1 次 read 系统调用（pending 时）/1 次裸 EAGAIN read（不 pending 时）。
- 内核工作：read(eventfd)。
- 同步：调用方持有 progress mutex（写方全被该 mutex 挡住，读全计数即排空）。
- 冗余假设：无残余——必然 EAGAIN 的第二次读已在 #621 删除（与 history-digest §1.2、
  CHANGELOG 一致）。
- 证据：直读；git log 快照（27d907d6 "async: remove guaranteed-EAGAIN drain probe read"）。
- 删除风险：不排空则信号累积、eventfd 恒可读 → 停靠握手退化为自旋。
- 裁决：KEEP

### C11. wait_one 驱动等待（观察/确认控制、重试小睡、probe 注入）
- 锚点：src/async/async_io_context.cpp:569-696（capability :578-580；token :609；pass :611；health :619-621；observe control :627-629；无工作返回 :631-634；重试小睡 :644-652；park :654-664；wake 分派 :666-694）；drive_settlement_ 的对应循环 :479-563（token :510；probe :536-542；ack :543-547）
- 语义义务：PROG-04 等待结果四类；CANCEL-02 时限只界本次等待；PROG-02（经 C9）；
  控制打断后的确认义务由调用方承担（wait_take/ack 交错，copy_task.cpp:98-100）。
- 当前操作：每轮：snapshot（1 progress mutex）→ run_progress_pass_（C7+stats+exhaustion
  检查 :429-439）→ health/控制判定（observe_pending_control 1 mutex）→ 无残留则
  wait_if_unchanged（C9，probe 注入 backend_has_immediate_physical_work_）。
  access_mtx_ 每次 wait_one 获取 3-4 次（:582/:591/:595 drive_entry、DriveGuard 构/析）。
- CPU 工作：如上 + 2 次 steady_clock::now()（:600-607、:658-659）。
- 内核工作：经 C9 的 poll/read。
- 同步：access_mtx_ ×3-4、progress mutex ×5-6、dispatch/core mutex ×2（pass 内）。
- 冗余假设：无独立候选。注意 access_mtx_ 的多次获取是 owner-only 路径（PROG-01 单
  owner），锁始终无竞争。
- 证据：直读。
- 裁决：KEEP

### C12. 结算收敛见证（settlement_converged_ / internal_work_retired / occupancy）
- 锚点：src/async/async_io_context.cpp:469-477（收敛谓词）、:479-563（drive_settlement_ 循环）；uring_backend.cpp:361-373（internal_work_retired：队列/owed 权威扫描/原子 witness/ledger）、:338-359（析构同判）；request_core.cpp:66-91（occupancy 全槽扫描）
- 语义义务：REQ-06/SHUT-02/SHUT-04：结算必须收敛到无未结事实；析构非静默 fail-fast；
  occupancy() 是 settlement 权威（request_core.hpp:180-183 注释声明 outstanding_summary
  非权威，代码一致：settlement_converged_ 用 occupancy()）。
- 当前操作：每结算轮：internal_work_retired（1 dispatch 锁 + delivery_ 全扫 any_event_owed
  + 2 原子读）+ occupancy（1 core mutex + 全槽扫）。
- CPU 工作：shutdown 专属；每轮 O(capacity_) ×2 扫描。
- 冗余假设：owed 的**结算面权威扫描**（any_event_owed_locked_）与热路径 owed_count_
  门并存——这是 #634/H2b 的既定分工（非权威派生摘要 + 权威扫描，ledger :85、
  threadpool_backend.cpp:847-859 同构）。无候选。
- 证据：直读。
- 删除风险：删 occupancy/权威扫描 → 带活引用销毁（REQ-05/LIFE 违约）。
- 裁决：KEEP

---

## D 段：应用消费与 buffer 复用

### D1. RequestScope submit/reserve/commit（scope 槽管理）
- 锚点：include/sluice/async/request_scope.hpp:67-111（Slot/Reservation/commit_）、:114-129（resolve_ O(S) 扫描）；src/async/request_scope.cpp:46-60（reserve/release 线性扫）、:137-235（submit 族）；承载层 async_io_context.cpp:249-289（access_mtx_ + 后端提交）
- 语义义务：scope 对 Request 责任的持有/释放（HANDLE 系）；THREAD-01 提交串行化
  （access_mtx_）；ticket 解析到 owning 槽。
- 当前操作：reserve_slot_ 线性找 free（O(S)）；ctx.submit_*（1 access mutex + 后端
  reserve/accept 2 次 core mutex + dispatch 路径）；Reservation 析构兜底释放；commit_
  variant emplace。
- CPU 工作：O(S) reserve 扫描 + O(S) resolve 扫描（每次 wait_for/take）+ 上述锁。
- 内核工作：（提交侧，属上游段边界）。
- 内存搬移：Request 移入 variant（≈32B）。
- 同步：access_mtx_、core mutex。
- 冗余假设：**resolve_ 的 O(S) 扫描 share 是 H3 遗留开放项**：m1-results.md:167
  H3 裁决 REJECT 自由列表（W-02 FIFO 形状 +15–21% 回归）但 "INVESTIGATE the
  scan-specific share"（scope 机器 ≈15.9% of tp W02 CPU，扫描份额未证）；
  m1-results.md:247 对应 RQ。本会话未复测。
- 证据：直读 + m1-results.md（假设上下文）。
- 删除风险：自由列表替换已被既往证据否决；无新候选。
- 裁决：UNPROVEN（扫描份额；替换形状已有 REJECT 既往裁决）

### D2. wait_for_slot_ + ready() 快路径
- 锚点：include/sluice/async/request_scope.hpp:146-185（ready 探测 :156；时限 :159-168；
  wait_one :169-170；状态分派 :174-183）
- 语义义务：CANCEL-02 时限界等待尝试；PROG-04 状态映射；interrupted→调用方 ack
  （copy_task.cpp:98-100、request_scope.cpp:110-111）。
- 当前操作：每轮 Request::ready()（core mutex，lookup :251-258）→ 必要时 ctx.wait_one
  （C11 全套）→ 醒后重查 ready()。
- CPU 工作：每轮 1 次 core mutex（ready）+ C11 全套。
- 冗余假设：入口 ready() 快路径与 wait_one 返回 progress 后的重查看似重复——快路径
  避免已就绪时跑完整 pass；重查是消费前的最后校验。两者都可由"wait_one 后单查"
  替代，但入口快路径在命中时省一整个 pass（含 dispatch 锁+收割+发布）；收益方向
  因 ready 概率而异，无证据不裁决。
- 证据：直读。
- 裁决：KEEP（快路径）/ 冗余假设记录为 UNPROVEN（合并形状收益未证）

### D3. take_result/try_result → observe/consume_public_result（消费 = buffer 复用许可）
- 锚点：include/sluice/async/request.hpp:134-159（try/take）、:187-209（result_of_）；src/async/detail/request_core.cpp:296-321（observe）、:323-344（consume）、:710-752（RequestBridge）
- 语义义务：REQ-04 发布读取的 acquire 语义（mutex 即"equivalent synchronized path"）
  与借出终止：**caller 只有在 acquire 该发布后才能复用内存/关 File**——consume 的
  binding_live=false + try_reclaim_ 正是该许可的记账动作；stale 消费 fail-fast
  （request_binding_invariant_fail_fast）。
- 当前操作：core mutex；resolve_public_；published 判定；outcome+metadata 拷出
  （≈40-90B 进 RequestResultData）；consume 侧清 binding + try_reclaim_；result_of_
  按 T 构造 Result（read/write 为 static_cast<T>(confirmed_bytes)）。
- CPU 工作：1 次 core mutex（跨 TU 真调用）+ 结构拷贝。
- 内存搬移：IoOutcome+FileInfo → RequestResultData ≈ 90B/请求。
- 同步：core mutex。
- 冗余假设：observe 与 consume 函数体近乎重复（:296-321 vs :323-344）——语义有意
  分裂（非破坏观察 vs 消费释放；OBS-01"Polling ready() needs no observer allocation"
  与 HANDLE 消费并存），不可合并。无候选。
- 证据：直读。
- 删除风险：consume 不清 binding 则槽位直到 discard/析构才回收（保留结果语义另有
  用途，语义不可自动化）。
- 裁决：KEEP

### D4. Request::discard / release_responsibility_
- 锚点：include/sluice/async/request.hpp:161、:177-185；src/async/detail/request_core.cpp:277-294（discard_public_result）、:754-771（bridge discard）
- 语义义务：非终局释放/陈旧绑定释放 fail-fast（request_nonterminal_release_fail_fast，
  request_core.cpp:22-28）；非可见（未发布）时拒绝而非静默。
- 当前操作：core mutex；resolve_public_；published（publication_inflight 不算，mutant
  对照 :283-287）判定；binding_live=false + try_reclaim_。
- CPU 工作：1 次 core mutex。
- 裁决：KEEP

### D5. 应用 buffer 复用（sluice-copy PipelineSlot）
- 锚点：apps/sluice-copy/copy_task.cpp:46-56（每 slot 固定 buffer，构造一次）、:66-88（submit_slot_read/write 直接以 slot 内偏移作 buffer 指针）、:106-149（短读/短写续提交）、:221-240（写毕回收 slot：chunk_offset += depth*size、filled/written/eof 归零、立即再提交读）
- 语义义务：REQ-04/LIFE-01 借出纪律的消费者侧：buffer 在 read 请求发布（take）之后
  才进入下一轮复用；无跨请求重叠（每 slot 状态机 reading→read_done→writing→idle）。
- 当前操作：应用级状态机；无库级拷贝——payload 指针贯穿
  PreparedUringOp/PreparedBlockingOp（uring_backend.hpp:159-167、threadpool PreparableOp），
  内核/worker 直写用户 buffer（**全路径零拷贝**）；复用 = 指针复位，无 realloc。
- CPU 工作：状态机扫描 O(depth)（每轮找最早 read_slot/write_slot，:178-184/:196-204）。
- 同步：无应用侧锁（单任务）；库侧锁见上。
- 冗余假设：无。
- 证据：直读。
- 裁决：KEEP

### D6. Direct I/O 形态：blocking read_at / write_at（W-01 直通；hash/grep/tail 的消费形态）
- 锚点：src/blocking_file.cpp:65-84（read_at）、:86-127（write_at）、:129-163（read/write）；precheck file_semantics.hpp:157-176（constexpr）；EINTR 重试 include/sluice/detail/posix_retry.hpp:11-21；errno 映射 error.hpp:92-94
- 语义义务：SEM-03 合法性前置（先 closed/access/range 后长度零判定）；SEM-04/05 短
  I/O 原语；Direct 形态与 async 形态共享 File 语义（ARCH）。
- 当前操作：constexpr precheck（零运行时成本的分支序列）→ complete_empty 早退 →
  retry_on_eintr{ native_pread/pwrite } → from_errno_value。
- CPU 工作：无锁、无原子；1 次 syscall 重试包装。
- 内核工作：1 次 pread/pwrite（每步）。
- 内存搬移：内核直写/直读调用方 span——零额外拷贝；应用侧单 buffer 跨迭代复用
  （hash_task.cpp:44/:55-57/:74，grep/tail 同构）。
- 同步：无。
- 冗余假设：无；mutant（SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY/:309
  STEP_REPORTS_FULL_COUNT）钉住 EINTR 重试与短计数两臂。
- 证据：直读。
- 裁决：KEEP

### D7. Direct 组合循环 compose（read_exact/write_all）
- 锚点：src/blocking_file.cpp:275-295（prevalidated step）、:299-333（compose 主循环：短步推进 :308-320、零进展停 :321-328）、:336-392（四入口）
- 语义义务：SEM-05 组合：eof_before_full / write_no_progress / primitive_error(unknown
  certainty) 区分（failure_publish_certainty :244-250）。
- 当前操作：循环 pread/pwrite 子步；零进展即停；impossible-count 检查 :314-319。
- CPU 工作：分支+算术；每步 1 syscall。
- 冗余假设：file_semantics.hpp:243-298 另有一套 compose_progress/CompositionState/
  classify_primitive 模型词表，**生产无调用者**（grep src/apps 零命中；唯一消费者
  tests/semantic_short_io_reference_test.cpp、semantic_durability_reference_test.cpp、
  direct_composition_fault_test.cpp、semantic_reference_case_test.cpp）——生产
  blocking_file.cpp 用自己的 compose 循环。这是"生产头文件承载模型词汇"的现状
  （语义 oracle 的共享词表面），非热路径成本；是否应移入测试面属 hygiene 问题。
- 裁决：KEEP（compose 循环）；模型词表位置 UNPROVEN（hygiene，非本段热路径）

### D8. threadpool run_syscall（async 路径上的 Direct 原语对应实现）
- 锚点：src/async/threadpool_backend.cpp:481-544（pread/pwrite/fdatasync/fsync/fstat + retry_on_eintr + errno 映射）；worker 终局路径 :431-466（offer→release→入队→signal）
- 语义义务：async 形态的物理原语与 Direct 形态同源（同一 pread/pwrite/retry_on_eintr
  家族），terminal 走 B1/B3 同一协议；worker 内联调用 sink 被禁止（OBS-02），signal
  唤醒 driver（:466）。
- CPU 工作：1 syscall + 2 work_mtx_ 短临界区（active 计数 :436-438、publication 入队
  :461-464）+ 3 次 core mutex（claim 在 ：406、offer :454、release :457）+ signal。
- 内核工作：1 pread/pwrite/fdatasync/fsync/fstat。
- 内存搬移：payload 直写用户 buffer（零拷贝）；metadata stat→FileInfo ≈ 48B。
- 同步：work_mtx_ ×2、core ×3、progress ×1（signal）。
- 冗余假设：无独立候选；与 uring 完成路径（A4-A6）结构同构，锁名不同。
- 证据：直读。
- 裁决：KEEP

---

## 4. 汇总

边总数：**33 条**（A6 + B7 + C12 + D8）。
裁决分布：KEEP 28（D7 为主体 KEEP，另带一个 UNPROVEN 子注记）；ELIMINATE_CANDIDATE 2
（A3 路由线性扫描-H1；C4 uring owed sweep 逐槽锁形态-H2a；均已有既往机制证据
PROPOSE_UPSTREAM，迁移证明未做）；UNPROVEN 3（B1 的 zero_effect_cancel 仲裁臂、
B5 acquire_execution、D1 scope 扫描份额；D2 的快路径合并收益作为 UNPROVEN 子注记
不计入边裁决）。

冗余假设总表（含未入选 submit 的）：
1. A3：cookie→O(R) 逆向扫描可换 user_data 索引编码（H1，PROPOSE_UPSTREAM，基准在
   bench/mechanisms/h1_router_index_bench.cpp）。
2. C4：uring owed sweep 逐槽锁可换单锁快照（H2a，PROPOSE_UPSTREAM；threadpool 侧
   已是目标形态）。
3. B1/B5：zero_effect_cancel 仲裁臂与 acquire_execution 是生产零生产者/零调用者的
   seam 面（热路径成本≈0，义务归属需 owner 裁决）。
4. C2：begin_publication 的 PublicationPayload 快照仅测试可达（生产恒 nullptr）。
5. D1/D2：RequestScope resolve_/reserve_ O(S) 扫描份额（H3 INVESTIGATE 开放）；
   ready() 入口快路径与 wait_one 的合并收益未证。
6. D7：file_semantics.hpp 的 compose_progress/durability 模型词表生产零调用（测试
   oracle 词表面在生产头内，hygiene 问题）。

## 5. 文档-代码对照

发现的不一致/张力（全部为文档层，无代码缺陷断言）：

1. include/sluice/async/detail/progress_source.hpp:520-523 类注释"never lends the
   context notification fd itself"与同文件 :535-545
   `register_notification_with_kernel` 实际把 fd 借给 install 调用帧（文档自述
   "registration-only borrow"）存在措辞张力——只能读作"不提供持久 fd 访问器"。
   两处注释并存易误读；语义以 :535-539 成员函数注释为准。
2. docs/research/state-complexity/m1-results.md:165-166 H1/H2a 标注
   PROPOSE_UPSTREAM 而当前代码两者均未产品化（A3 仍线性扫描、C4 仍逐槽锁）——
   这不是失配（提案未落地），但属于"既往裁决与代码现状的落差"，是本段最直接的
   追问入口。
3. docs/evidence/e5-fullpath-machine-work/repo-reality.md:37 记"唯一未跟踪项
   docs/evidence/621-round2-R1/"，当前工作树另有本战役产物
   docs/evidence/e5-fullpath-machine-work/——时间性漂移，非失配。

抽查一致项（声明与代码相符，列出以证对照做过）：
- request_core.hpp:180-183 "outstanding_summary 与 accepted/published 转移同临界区
  更新" = request_core.cpp:202（accept ++）/473（complete --），均持 mutex_。
- uring_backend.hpp:193-197 DeliveryRecord 交接协议 = publish_one 读 kind 的时序
  （uring_backend.cpp:589-593）与 owed 记录写入（:564-572）。
- progress_source.hpp:96-103 probe/再验证/不再 drain 的时序 = :153-192 代码。
- uring_completion.cpp:204-206 pass 边界 = :229 入口定界 + :645-646 发布同界。
- threadpool owed gate 的 ledger 描述（v2-conformance.md:71）= threadpool_backend.cpp:569-583/:608。
- #635 drain 单读（history-digest §1.2）= progress_source.hpp:437-442。

## 6. 本会话执行过的命令（全部只读）

- `find src/async include/sluice/async -type f`（文件清单）
- `wc -l`（各文件行数）
- Read：上述 §0 全部文件
- `grep`：zero_effect_cancel / layout_pad / attach_ready_sink / begin_publication /
  PublicationPayload / acquire_execution / acquire_control / claim_execution /
  compose_progress 家族 / from_errno_value / DIRECT mutant / lto|ipo（xmake）
- `sed -n`：explicit-io-contract.md 相关节（REQ-04/05/06、PROG-01/02、OBS-01/02/03、
  THREAD-01/02、CANCEL-01）、xmake/tests.lua:330-375、bench/mechanisms/run_mechanisms.sh:1-60、
  m1-results.md 检索、apps/sluice-hash/hash_task.cpp 全文、blocking_file.cpp:120+
- 未运行：任何编译、测试、基准、git 写操作（任务约束；同机频率测量进行中）。
