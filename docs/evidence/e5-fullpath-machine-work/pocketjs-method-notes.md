# PocketJS 方法研读笔记（e5-fullpath-machine-work）

研究性质：方法论提取，不是技术栈复制。研究对象是一个 TypeScript 宿主 + no_std Rust
核心 + 每目标原生后端的"一线程一进程"渲染运行时；Sluice 是 C++20/io_uring/无 GC/
有 v2 显式契约的异步 IO 库。以下只提取对 Sluice 热点与机工审查可迁移的方法，
技术栈、域机制与硬件前提一律不复制。

## 来源与可达性记录

- https://github.com/pocket-nexus/pocketjs （README，WebFetch 可达）
- https://pocketjs.dev/docs/architecture/ （WebFetch 可达）
- https://pocketjs.dev/docs/ （索引页返回空内容——已如实记录，改用仓库入口）
- 仓库 docs/ 目录清单（GitHub 树页面可达），并取原文三篇（raw.githubusercontent.com，均可达）：
  `docs/DESIGN.md`、`docs/DETERMINISM.md`、`docs/PSP_ALLOCATOR.md`

## 一、可迁移方法论要点

1. **跨边界只传变更，且批量过界；边界成本摊薄进已存在的窗口。**
   PocketJS 在 JS 侧维护镜像树，reconciler 的读从不跨 FFI（"Only mutations
   cross into native"）；每帧一次跨界产出 DrawList；GE 画第 N 帧与第 N+1 帧的
   JS/tick/layout 重叠，使帧时间从 CPU+GPU 之和变为二者之 max（DESIGN.md
   "Hot Path Decisions"）。迁移读法：热路径上每 op 固定一次的边界穿越
   （syscall enter、独立锁收购）先问"这次穿越的变更能否积攒到已存在的批窗口"，
   摊薄固定骨架成本，而不是优化单次穿越本身。

2. **回传数据自带寻址；禁止在热路径重新推导已知信息。**
   世代标签句柄使"stale handle resolves to nothing"（DESIGN.md State
   Ownership）；DrawList diff 只产出逻辑 damage。迁移读法：内核回传的
   user_data / 完成事件应能自证路由归属；任何"收到事件后反向扫描本地表找
   归属"的 O(D)/O(R) 扫描，都是对同一信息第二次付费。凡信息已随事件到达，
   归属应在编码里，不在扫描里。

3. **运行期不变量在构建/装配期烘焙。**
   样式表编译进 baked style table、字体烘焙成 coverage atlas（"no glyph
   rasterization at runtime"）、关键帧烘焙为 segment timeline、tick 率首
   tick 后锁定并随 bundle 校验（DESIGN.md / 架构页）。迁移读法：每 ring、
   每后端配置不变的结构（路由表形状、user_data 编码布局）在后端构造期定形，
   运行期只做逐 op 增量，不逐 op 重算常量。

4. **每个目标/形态一张实测成本表；用户态持不出的边界成本如实入档，不强行消除。**
   PSP 实测：Solid 3.66 ms vs vDOM Vue 90.75 ms、worst frame 9.7 ms、8 MB
   arena 占 PSP RAM 四分之一；内核/FS 条件性行为（如 io-wq、分区耗尽）记录
   为形态证据与失败判据（PSP_ALLOCATOR.md 的设备验收流程）。迁移读法：
   不是所有热点都有用户态解——内核侧机工应作为后端形态选择与分叉文档的
   证据保存，而不是发明伪候选。

5. **确定性观察是完成定义；预算是硬门槛；证据通道与生产路径分开门控。**
   字节级 PNG goldens、FNV 帧哈希、chaos 注入（sleep/分配抖动/强制 GC）后
   轨迹逐位不变（DETERMINISM.md）；DrawList 预算 ≤40 draw calls / ≤2000
   quads / 48 KB 顶点；不支持面在编译期大声失败。迁移读法：移除或摊薄热
   路径机工时，证据必须仍有独立复现通道（Sluice 已有先例：
   `include/sluice/async/detail/reference_ready_sink.hpp:9-14` 生产空体 +
   `SLUICE_ASYNC_INTERNAL_TESTING` 门控计数）；"每 op 机器成本"（enters/op、
   锁/op）应与延迟一起成为 conformance 证据。

## 二、明确不适用或不复制的内容

1. **guest/宿主双引擎结构**（QuickJS guest + Rust core、TS spec 生成
   Rust+C 双头文件防漂移）：Sluice 的边界是内核 ABI 与 C++ 公共 API，没有
   解释器薄膜与跨语言漂移问题；mirror-tree、多语言头文件字节比对无对应物。

2. **frame-transaction 单线程模型**（"一帧是外部不可打断的事务"、
   `state[n+1] = F(state[n], input[n])`）：成立前提是单线程渲染核心。Sluice
   的 v2 契约要求合法并发交错（claim/retire、publication、取消竞态、
   borrow/touch）被显式建模；不能把"事务内无并发"当作简化或删义务的理由。

3. **arena/硬分区内存前提**（PSP 单 kernel block、8 MB 预算、permanent
   tail 到进程退出）：这是硬件约束产物。Sluice 无 GC 但内存与中间结果复用
   必须落在 v2 所有权语义（borrow/retire/保留结果）内；引入新的运行时内存
   权威需 root 修订，不是研读别家能授权的。

4. **降级语义换覆盖面**（PSP 上 setTimeout "lowers to a microtask and
   ignores its delay"、编译期禁用 createResource/startTransition）：PocketJS
   用"大声失败"管理它自己的缺 API 环境；Sluice 的显式契约禁止隐藏降级
   （AGENTS.md："Do not add hidden blocking fallback"），File 语义必须跨
   调用形态与后端一致——"运行时悄悄改语义"是反模式本身。

5. **渲染域机制本身**（DrawList、damage rectangles、vblank tick、物理
   step、baked 字体图集）：图形域概念。可迁移的只是其分层思想（逻辑差分
   归一层、差分策略归后端），Sluice 的 transport_ledger_/RouterEntry/
   发布协议已是各自等价物，不需要新机制。

## 三、透镜套用：本轮 Top 热点逐条结论

每条一句话；追问模板（为什么存在/持有什么独立责任/信息是否他处已有/
能否并入/删后最坏交错是否仍过）的答案浓缩在句内。引用均为本会话读到的
当前工作树代码。

1. **H1 提交批形态（M-34/35/42）**：per-op 的 `io_uring_submit`
   （`src/async/uring_transport.cpp:228-231`，accept 路径每轮 dispatch 后即
   触发：87 行；poll 路径 631 行）是纯固定内核机工，待提交事实已由
   transport_ledger_ 持有、EAGAIN/EBUSY  reconcile（262-278 行）已兜住批化
   后的最坏交错，且 #622 C1 已给出摊薄先例——有空间，批窗口摊薄而非缩短单次
   enter。

2. **M-28 syscall_count_（TP 路径）**：每物理 syscall 一次
   `fetch_add`（`src/async/threadpool_backend.cpp:434`）的唯一读者是测试缝
   `syscall_count_for_test`（`include/sluice/async/detail/threadpool_backend.hpp:72`、
   `src/async/threadpool_test_seams.hpp:108`）——不持任何独立语义/物理责任，
   纯 test-only evidence 落在生产热路径，同一证据可在测试内经缝推导，
   ReferenceReadySink 的测试宏门控是仓库内现成模式——有空间（DAG #1：
   门控/移除）。

3. **M-53 cookie O(R) 逆向扫描**：完成路径（283 行）与 ledger reconcile
   （388 行）调用 `find_live_router_cookie_`（`src/async/uring_transport.cpp:551-557`）
   对内核已回传 user_data 的归属信息再做一次 O(R) 反向扫描，正是"回传数据
   自带寻址"要点反面的教科书案例——user_data 编码路由索引（H1 方案）后
   归属在编码里，in_use/世代校验不变——有空间，但依赖 H1 编码方案落地。

4. **段 7 io-wq punt 内核 handoff**：成本在内核侧、FS 条件性（tmpfs
   +7304 cycles:k，btrfs warm=0），Sluice 用户态持不出可消除的对应信息
   ——无新空间，按要点 4 作为 uring 形态/FS 分叉的成本证据入档，不发明候选。

5. **M-62 deliver_event claim/retire 空转**：每 op 两次核心锁（
   `src/async/uring_backend.cpp:610-614`）向生产空体 sink
   （`include/sluice/async/detail/reference_ready_sink.hpp:9-14`）投递，
   但 claim/retire 对持的是"观察者在发布与投递之间注册"这一合法交错的义务，
   不能删义务本身；可并入点是 `complete_publication` 已在锁内计算的
   armed-observer 判定（`src/async/detail/request_core.cpp:477-481`）——由
   权威（core）在同一锁段给出"无人观察"，而非后端缓存派生标志——有条件空间
   （并入现有操作；后端侧缓存方案违反派生非权威）。

6. **M-61 begin/complete 两锁融合**：`begin_publication`/`complete_publication`
   各锁一次（`src/async/detail/request_core.cpp:438-460`、462-484），调用形状
   （`src/async/uring_backend.cpp:583-598`）中间夹着 `DeliveryRecord::kind`
   的 pin 下读（589-593 行注释：complete 后槽位可并发回收）与测试暂停缝
   （596 行）——融合为单核心操作必须在同一锁段内完成 kind 拷贝并重新安置
   epilogue 暂停语义——有空间（锁融合），但这是融合不是删协议。

7. **TP w8 work_mtx_ 竞争（M-21/22/29/64）**：同一把
   `work_mtx_` 护两个独立事实（`active_workers_` 递减 436-438 行、
   `publication_pending_` 入队 462-464 行，`src/async/threadpool_backend.cpp`），
   但实测增量在等待不在指令（w8 CPU/op 10.35 µs vs uring 1.71 µs 而
   instr:u 仅差 5%）——主杠杆是 worker 超配/调度形态，锁拆分是次级小空间，
   lock-free 重写无测量依据——主要无新空间。

8. **M-67 sticky 双标志每 pass 3 次锁读**：三个互斥读
   （`core_->health_failed()` `src/async/detail/request_core.cpp:246-250`、
   `progress_->wait_health_failed()` `include/sluice/async/detail/progress_source.hpp:63-67`、
   `progress_->exhausted()` 266-270 行，调用点 `src/async/async_io_context.cpp:433-434,450`）
   读两个确为永不复零的 sticky 标志（health_failed_ 仅置 true、
   progress_exhaustion_ 仅增），其存在有 lost-wake 语义注释背书
   （progress_source.hpp:59-62）——标志本身无新空间，可摊薄的只是"读的
   时机"（随 pass 深度摊薄，与 DAG 候选 #19 一致）。

9. **M-74/M-01 就绪扫描族**：`reserve_slot_` 线性扫 free 槽
   （`src/async/request_scope.cpp:46-55`）与 wait_for/take 消费侧
   （246-262 行）都在重付"哪些槽 free"这一已完整持有于 `slots_[i].state`
   的信息；仓库内 uring 侧 `cookie_free_list_`（`src/async/uring_transport.cpp:580`）
   已是 free-list 先例，语义不改——有空间（O(S)→O(1) 摊薄），增长形状与
   DAG H3 INVESTIGATE 一致。

10. **M-42/M-40 原子冗余**：`submit_flushes_`（
    `src/async/uring_transport.cpp:222`）唯一读者是测试缝
    （`src/async/uring_test_seams.hpp:75-77`），`live_cookies_`（195/581 行
    维护）的仅有的两个读者（`src/async/uring_backend.cpp:350,371`）都在
    `dispatch_mtx_` 锁内——两者都不代表独立语义事实：前者是 test-only
    计数器，后者与路由器 in_use 占用同处一个互斥域——有空间（门控/降级为
    普通成员），不影响任何交错。

## 覆盖说明

- 两个主来源与三篇仓库文档均在本会话实际取回；`pocketjs.dev/docs/` 索引页
  为空，已改用 GitHub 仓库 docs/ 原文入口，未使用任何未读来源。
- 热点透套基于 ask 给定的 MEASURED/PARTIAL 数据 + 本会话对 6 个源文件的
  实读与 grep 读者核验（syscall_count_/submit_flushes_/live_cookies_ 读者、
  ReferenceReadySink 空体、sticky 标志实现）；未运行任何基准或测试——
  本任务是方法研读，不是复测。
