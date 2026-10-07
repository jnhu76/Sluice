# M0 Consumer Migration Census (live consumer census)

Issue #402 Phase M / M0 — 消费者迁移前提的普查工件。本文件是语义解读；机器可读全量数据在
[`docs/review/m-consumer-edges.json`](m-consumer-edges.json)（EDGES 29 个边键、216 条 EDGES
消费者记录（含 R2 审查轮补录 c-new02-901，见 §7.2）、APPS 四 app 深审 47 条消费者记录、
BLOCKERS、UNKNOWN_EXTERNAL_USE U-01..U-08）。

- **BASE_SHA**: `c782099f56c6afd62daad2929944673059aef573`（master，`git rev-parse HEAD` 实测；= F1 adopted baseline）
- **GENERATED_AT**: `2026-10-07T14:22:14Z`（`date -u +%FT%TZ` 实测）
- **F0 adopted**: `a34d96c64f5ac8647b7d0f57c5511e68301b0f0e`；**F1 adopted**: `c782099f56c6afd62daad2929944673059aef573`
- **性质声明**: 本文件与 JSON 是计划工件（planning artifact）。不改生产代码；不裁决任何 F2..F5 收缩；
  不据此从名字推断处置（`legacy`≠删、`detail/`≠内部、`compatibility`≠可破坏——M 铁律）。
  所有结论处于**待审查**状态：边判定、BLOCKERS、UNKNOWN_EXTERNAL_USE 均为待评审的扫描声明。

## 1. 方法与归一规则

方法：六路并行扫描结果的机械汇总 —— 边界重扫两流（`live-rescan`：X-01..X-18 逐边 live
重验证，26 份边报告；`f0-hunter`：对 F0 census/断言在 HEAD 复核，21 份边报告）+ 四路 app
深审（sluice-copy / sluice-hash / sluice-grep / sluice-tail，各一份含消费者记录的深审），
外加 hunter 的 4 条 FALSE_OLD_ASSUMPTION。汇总只做规范化/去重/分组，**不发明扫描数据之外
的 consumer**：

- X 边按 `edge_id` 合并为单条目：`reports[]` 逐流保留 status + evidence 原文；`consumers[]`
  合并并带 `stream` 标签。两流 status 不一致时**不裁决**，原样并列（见 §2 与 §8）。
- NEW 边在输入中按发现流重复编号（`NEW-01`/`NEW-02` 多次出现）；按出现序赋唯一
  `edge_key`（NEW-01a..NEW-01e、NEW-02a..NEW-02e、NEW-03），`subject` 标签取自各自
  evidence 的主题。
- 转写规范化仅两处：evidence 内嵌双引号按 JSON 转义；清除消息文本中混入的零宽字符。
  evidence/consumer 字段逐字保留。
- 已知唯一跨流路径级重复：X-01 的 `tests/package/consumers/contract_observer.cpp` 由两路
  各记一条（c-x01-041 aux/TEST_ORACLE/COMPAT_TEST_RETAINED vs c-x01-901
  M-R/CALL/UNMIGRATED，行号集合与 family 判定不同）。两条记录都保留，family 归属留待
  M1 裁定；其余同路径多记录是输入数据的 per-symbol 记录设计（如 X-03 的
  stackful_io_host.hpp ×4、X-11 的 ObserverCore.tla ×3），非重复。
- R2 审查轮（三视角评审后回写，2026-10-07）为修正两条已证伪/遗漏的断言，追加了第七路
  输入 `review-r2`（1 条消费者记录 c-new02-901 + U-05 文本翻案 + U-08 补录 + B-01
  gate 范围细化），修订命令与输出见 §7.2 与 §10。

## 2. X-01..X-18 逐边结论

状态列 = `live-rescan / f0-hunter`。18/18 条边两流均覆盖；12 条双流一致 UNCHANGED，
6 条 SPLIT（两流判的是不同谓词，见各行说明与 §8）。

| 边 | 状态 (live/hunter) | 语义结论（本轮核实） | 证据指引 |
|---|---|---|---|
| X-01 Completion\<T\> 载体面 | UNCHANGED / CHANGED¹ | 边完整在位：4 app 任务体持有/结算 Completion；`Batch::Slot` 按值存储（batch.hpp:56-68）；`AsyncBackend::submit_*(op, Completion<T>*) = 0` 迫使全部后端与测试 fake 拼写（async_io_context.hpp:144-150）；busy-poll 仍活（op_helpers.cpp:18-22 `while(!c.ready()) ctx.poll()`，D-15/MIG-02 行**非历史**）；StackfulIoHost 已无 completion.hpp include、经 `Request<T>` await（:233,243,254,267）——替换形态已由代码证明。¹hunter：F0 "25 test files" 计数过时（`grep -rlw Completion` = 27；新增 contract_observer.cpp 实际使用 + w02_pipeline.cpp 注释提及；15 个直接 include 文件与 4 app 不变） | c-x01-001..045；c-x01-901 |
| X-02 RequestArena fallback | UNCHANGED / UNCHANGED | 边在位但**不可达**：arena binding 从未被安装到任何 Completion（`install_binding` 零调用者；submit_transaction.hpp 全仓零引用的孤岛模板；`release_completed_binding` 仅 completion.hpp:169,358 调用）→ release/identity fallback 编译保留但 master 上休眠；故判 CONFIRMED_UNCHANGED 而非 ALREADY_MIGRATED | c-x02-001..009；X-02 live evidence (5) |
| X-03 Fiber 内嵌 StackfulIoHost | UNCHANGED / UNCHANGED | 在位；细化：fiber.hpp/fiber_ctx.hpp 为 **DIRECT** include（F0 KIND 记 TRANSITIVE 需修正）；Fiber 按值公开头内嵌唯一 stackful_io_host.hpp:181；live includer 恰 3（impl TU / stackful_host_test / w04）；两份 F1 manifest 装有 fiber*.hpp（noliburing.json:56 + ODR 注 ：146） | c-x03-001..007 |
| X-04 ApplicationRuntime→Scheduler | UNCHANGED / UNCHANGED | 在位；include 为 DIRECT（细化）；`sched_` unique_ptr + `wake_handle_` 按值公开布局（:182,:184）；w01_direct/direct_w01 防火墙确认 4 个核心类型保持 incomplete——是**负向**引用，checked but not counted | c-x04-001..009 |
| X-05 await helpers→runtime 链 | UNCHANGED / UNCHANGED | 在位；helper 家族无法脱离 runtime 世界（await_op_helpers.hpp:3→application_runtime.hpp:6→scheduler.hpp:10-11）；async/file.hpp 桥把 File 家族绑进 runtime 退役序；grep/hash/tail 对 await_op_helpers 的 include 为符号未用的直接 include | c-x05-001..010 |
| X-06 FileReader/FileWriter 第二权威 | UNCHANGED / UNCHANGED | 负判定保持（canonical File 无数据操作、无 vectored；root 不要求 vectored，PROD-03 root:127 / ledger:56 DEFERRED_OUT_OF_SCOPE）；细化：oracle 共享降为 **PARTIAL**——legacy 路径在关闭 fd 上伪造 permission_denied（src/file.cpp:93-94），与 oracle 要求的 invalid_state 相悖，已是 ledger GAP 行（v1-conformance.md:149）；live consumer 计数本轮实测（io_context factory + wal.cpp + 6 测试文件，apps=0） | c-x06-001..008 |
| X-07 IoContext factory | UNCHANGED / UNCHANGED | 在位；factory 仍是 legacy file 面唯一生产 CALL consumer，且自身零下游（BlockingIoContext 零 consumer）；MemoryIoContext 走 fault.hpp 不触 legacy；两份 F1 manifest 装 sluice/file.hpp 与 io_context.hpp（liburing.json:93,:95） | c-x07-001..002 |
| X-08 四 app 类型/调用面 | UNCHANGED / UNCHANGED | 在位；include census 负断言全过（0 app 直接 include completion.hpp / sluice/file.hpp / scheduler/fiber/group 头；sluice-copy 不含 async/file.hpp；blocking/file.hpp 仅 tail）；符号级取证：await_read_at（grep:53/hash:51/tail:114-146）、run_task_to_result（copy:358/grep:151/hash:128）、RuntimeBuilder+build（tail:328-330）、NativeFileRef（copy main:67,125）、blocking::size（tail:183,267） | c-x08-001..012 |
| X-09 helper→runtime 序锚 | UNCHANGED / UNCHANGED | 在位；扩大发现：run_task_to_result/RuntimeBuilder 测试侧消费者 live = **8 个测试文件**（F0 锚点只记四 app）；task_result.hpp 直接 includer = 4 app cpp + 8 测试文件；application_runtime.hpp 计数（4 apps, 1 test, 1 src）与本轮一致 | c-x09-001..012 |
| X-10 现行文档词汇 | UNCHANGED / CHANGED¹ | retained-baseline 段落原样在位（README.md:109-111；zh :42-44）；无新增 source/ABI 兼容承诺（仅否定式，root:127）；ADR-0002 supersession banner 在位（:3-15）；台账行 v1-conformance.md:54 仍 NOT_ASSESSED。¹hunter：F1 新增现行文档层（f1-clean-room-package-baseline.md、f1-requirement-evidence-matrix.md、f1-package-manifest-*.json、tests/package/README.md）把包面 DOC_CLAIM 绑在 compat 词汇上——必须与被描述 surface 同批迁移 | c-x10-001..005；c-x10-901 |
| X-11 形式模型对应 | UNCHANGED / CHANGED¹ | 实测 12 .tla / 192 .cfg（formal map 存量 10/158 已漂移——ProgressSource/ShutdownCore 由 C2-E/E2 在 map 最后更新后加入）；四个活跃模型对应头逐一读码核实，引用文件全部存在；8 个 disposition 标注模型是历史 census 对应，不得计为活跃对应。¹hunter：X-11 两断言被翻案——RequestCore.tla:114/:432 `ReleaseBind` = compatibility Completion release flavor（map:268 明文）、ObserverCore.tla:28,:62,:364 host waiter actor ↔ RuntimeTaskContext waiter path（C1-D）；"none maps to Scheduler" 半句仍成立（全为排除性注记；SelectCore.tla:231 GroupResolveEv 是命名巧合） | c-x11-001..008；c-x11-901/902 |
| X-12 SLUICE_HAS_LIBURING ODR | UNCHANGED / CHANGED¹ | 在位：public define+link 单点（libraries.lua:50-51；`xmake show -t uring_public_consumer_probe` 传递验证非手抄）；双守卫布局面（uring_backend.hpp:165-359；experimental/uring_write_batch.hpp:36-40 不安装）；sizeof 两 profile 不同（F1 baseline 48/304 + negative probe）经包规则承载。¹hunter：F1 兑现了"包规则消费者"——odr_two_tu_{a,b}（同宏视图一致）、negative_odr_view_{a,b,main}（异视图必须发散失败）、verify_f1_package.py:516-526 F1_E gate、manifest CONFIG_REQUIREMENTS ODR 条款 | c-x12-001..005；c-x12-901..903 |
| X-13 detail/posix_retry | UNCHANGED / UNCHANGED | 在位；关键细化（`detail/`≠内部）：该头是**已安装头**（xmake/libraries.lua:20；两份 F1 manifest :88）——收缩它即收缩 F1 包面；threadpool_backend.hpp:8 构成公开头→detail 头传递边；core 侧 20+ `retry_on_eintr` 调用点；app 侧唯一直接消费者 safe_output.cpp:180（directory_fsync EINTR 重试） | c-x13-001..005 |
| X-14 seam 构建拓扑 | CHANGED¹ / UNCHANGED² | 机制在位且承重：4 个测试直接 quote-include src 头；include 侧 17 个安装头带 SLUICE_ASYNC_INTERNAL_TESTING 守卫区；tests 侧 18 文件用 seam API；seam 与生产库不同二进制是隔离设计。¹live 按声明点展开计：12 处 `add_includedirs(... src/async)` 声明点 → 81+2 target（liburing）/ 43+2（noliburing），判 F0 "14 declarations" 口径失效；²hunter：xmake 自 989d5c3f 字节不变，13 个 define sites 与 census 的 14 一致（生成器 helper 一处定义盖多 target）——**计数口径差，非文件变更**（见 §8） | c-x14-001..007 |
| X-15 ReadyRoutingSink | UNCHANGED / UNCHANGED | 在位；implementor census 恰 2（Scheduler::ReadyRoutingSink + ReferenceReadySink）——Scheduler 只加路由（wait registry 投递 + stale/cancel 丢弃），不加第二 progress 权威；协议有自身 Scheduler 无关 oracle（progress_source_ownership_test，双 profile 变体） | c-x15-001..003 |
| X-16 Scheduler await 机制 | UNCHANGED / UNCHANGED | 全部 F0 锚点逐行核实（scheduler.hpp:153-163/:460；park_wake:415-505；fiber.hpp:22-26,59-76；application_runtime.cpp:55-73）；新结构精确化：await 机制经 attach_observer→`backend_->identity_of`（async_io_context.cpp:970,985,1000,1015）把 X-02 的 fallback 传递包含进身份解析；仅 size/void 可 await/cancel；`Batch::await_one` 不走此路径（ProgressOwner/wait_one） | c-x16-001..019 |
| X-17 Fiber 词汇暴露/Group | UNCHANGED / UNCHANGED | 在位；Group 是唯一 complete-type 阻塞点（group.hpp:139-199 inline 模板内 make_unique\<Fiber\>）；AsyncMutex/AsyncCondition/AsyncRwLock/AsyncSemaphore 零 app/test consumer；condition.hpp 零 repo includer（仅安装面）；scheduler_mutex/rwlock 仅在 assert 消息串命名 Async 词汇（NAME_ONLY）；hunter 复跑 dormant-wrapper 计数全保持（condition/semaphore/async_queue 0 直接 include；async_rwlock 10 src TU） | c-x17-001..009 |
| X-18 app README DOC_CLAIM | CHANGED¹ / UNCHANGED² | ¹live：'installed' 半句的**事实底座被 F1 改变**——app 全部 11 个去重 sluice include 逐一比对 noliburing manifest（79 HEADERS）= 11/11 INSTALLED（含 detail/posix_retry.hpp），但 app 构建仍走源码树 include/（apps.lua:24,42），声明描述的是"所用头属于安装集"而非"针对安装前缀构建"；copy README 引 docs/history 死路径。²hunter：四份 README 文本自 989d5c3f 未动。DOC_CLAIM 迁移义务全部未履行 | c-x18-001..007 |

## 3. NEW 边（扫描新发现，F0 X-01..X-18 之外）

| edge_key | 主题 | 关键证据 | 消费者 |
|---|---|---|---|
| NEW-01a | `evented_wait_policy.hpp` 自带 Scheduler/SchedulerWakeHandle 公开暴露（第三块安装头级 scheduler 暴露，X-04/X-17 未覆盖） | evented_wait_policy.hpp:3,15,19,22,26；唯一 repo 消费者 group.cpp:22 | 2 |
| NEW-01b | vectored-ABI 分发不对称：`read_vec_all` 逐 slice 走 read_some、**不**派发虚 read_vec；对称的 `write_all_vec` 却派发 write_vec——vectored 决策（B-07）必须计入 | reader.cpp:81-96 vs writer.cpp:48-56；read_vec_all 全仓零调用者 | 0 |
| NEW-01c | tests→apps 引擎耦合：4 个 `app_*_consumption_test` 目标把 app 引擎 TU 编进测试二进制（仅公开头）；F0/F1 文档零记录；app 引擎迁移时共同迁移 | xmake/tests.lua:113-174（copy:170-171 等） | 4 |
| NEW-01d | CHANGELOG.md 三处 `docs/history/archive/changelog-v0.1.0-era.md` 悬空指针（随 5f62b55b 删除）；另录 :22-23 历史符号提及（符号仍活 scheduler.hpp:772，非现行契约） | CHANGELOG.md:17,41,52；`ls docs/history` → 不存在 | 4 |
| NEW-01e | F1 新增干净室 OPTIONAL_CANDIDATE host 消费者并冻结其 H-30/H-31 闭包（F0 X-03 只记头侧布局边，未记已实现的干净室消费者集） | w04_stackful_candidate.cpp:12,:99..332；两份 manifest HEADERS 含 fiber/fiber_ctx/cancel.hpp；verify_f1_package.py:516-520,559-565 | 3 |
| NEW-02a | `src/async/fail_fast.cpp` 编译期耦合 `fiber_ctx::supported`（evented admission 门）；src 内耦合、非头暴露；fiber_ctx 内部化时常量须随迁 | fail_fast.cpp:3,:257,:268,:272 | 1 |
| NEW-02b | Reader/Writer 抽象族的实现耦合：file-backed 实现仅 FileReader/FileWriter（另 Memory \*测试替身、Observed \*装饰器零消费者）——L-01 退役而无 canonical 适配器会静默搁浅 copy.hpp/wal.hpp/memory_io_context.hpp | file.hpp:16,:67；fault.hpp:39,:16；observed.hpp:38,:57；BlockingIoPool 查证不耦合 | 3 |
| NEW-02c | `SLUICE_COPY_INTERNAL_TESTING` 未接线：无任何 target/脚本/CI/文档定义该宏 → safe_output 的 DirFsyncScript 注入 oracle 在当前全部构建配置下不可达（X-13 迁移缺对照证据，见 B-02） | safe_output.cpp:14,:52；全仓 grep（排除 build/）仅此两处 | 1 |
| NEW-02d | visuals/sluice-v1 对"backend 不知道 Scheduler/Fiber"的现行为断言——可检验且今日为真（backend 文件 include 检查零命中）；DERIVED/NON-NORMATIVE，根优先 | index.html:1743；visuals README:3-13 | 1 |
| NEW-02e | 干净室不完全类型防火墙：w01_direct.cpp:19-34（post-F0 新增）+ 在树孪生 direct_w01_consumer_probe.cpp:18-46（F0 DAG 未记录）——core-only 面不得传递完备化 runtime/arena 类型 | w01:31-33,:23,:34；probe:30-46（tests.lua:48-54，core-only 无 seam 宏） | 3 |
| NEW-03 | F1 安装/包基线把旧家族头集合变成冻结契约：add_headerfiles 双 glob、install+walk+79 头/2 库硬门（:598-600）、standalone-compile 门成为四个零消费者头的首个"消费者"、negative_async_symbols_in_core 冻结 core/async 归档切分（LINK 契约） | libraries.lua:18-21,:36-38；verify_f1_package.py:435-443,:280,:598-600 | 4 |

## 4. consumer 统计（family × edge_kind）

EDGES 消费者记录 216 条（live-rescan 198 + f0-hunter 17 + review-r2 1）；
APPS 深审消费者记录 47 条。

EDGES（216）：

| family | BUILD_CONFIG | CALL | DIRECT_INCLUDE | DOC_CLAIM | FORMAL_CORRESPONDENCE | HISTORICAL_REFERENCE | LINK | MACRO_LAYOUT | NAME_ONLY | OWNERSHIP | TEST_ORACLE | TRANSITIVE_INCLUDE | TYPE | TYPE_LAYOUT | total |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M-F | 1 | 10 | 1 | 1 | . | 1 | . | . | . | . | 15 | . | 6 | . | **35** |
| M-H | . | 18 | 3 | 8 | 5 | 1 | . | 3 | . | . | 16 | . | 10 | 2 | **66** |
| M-R | . | 20 | 9 | 3 | 3 | . | . | . | 1 | 1 | 19 | 1 | 10 | 8 | **75** |
| aux | 5 | 2 | 7 | 4 | . | . | . | 3 | 3 | . | 9 | 5 | . | . | **38** |
| none | . | . | . | 1 | . | . | 1 | . | . | . | . | . | . | . | **2** |
| **total** | **6** | **50** | **20** | **17** | **8** | **2** | **1** | **6** | **4** | **1** | **59** | **6** | **26** | **10** | **216** |

APPS（47）：

| family | BUILD_CONFIG | CALL | DIRECT_INCLUDE | DOC_CLAIM | HISTORICAL_REFERENCE | OWNERSHIP | TEST_ORACLE | TYPE | TYPE_LAYOUT | total |
|---|---|---|---|---|---|---|---|---|---|---|
| M-F | . | 1 | 1 | . | . | . | . | 4 | . | **6** |
| M-H | . | 4 | 6 | 4 | . | 1 | . | 1 | . | **16** |
| M-R | . | 4 | 1 | . | . | . | . | 3 | 1 | **9** |
| none | 4 | 4 | . | . | 4 | . | 4 | . | . | **16** |
| **total** | **4** | **13** | **8** | **4** | **4** | **1** | **4** | **8** | **1** | **47** |

分布（EDGES / APPS）：

- migration_state：UNMIGRATED 161/30；COMPAT_TEST_RETAINED 37/–；NOT_A_CONSUMER 9/17；POLICY_ISOLATED 9/–
- owner：F3 97/12；M 51/12；F2 48/9；F4 14/6；none 6/8
- runtime_relevance：runtime 99/31；test-only 87/4；docs-only 19/8；build-only 11/4
- profile：both 199/47；liburing 10/–；noliburing 7/–

读法提示（非处置）：M-R 最大（75）反映 Completion/RequestArena 携带面横跨
类型/调用/测试 oracle；TEST_ORACLE 合计 59+4 条——语义 oracle 是迁移证据主体而非可改写
transport；`none` family 多为后端选择/构建事实/已删除引用的历史记录（NOT_A_CONSUMER）。

## 5. BLOCKERS（迁移阻塞/前置项，全部可溯源到扫描记录）

| id | 阻塞 | 依据（consumer/边记录） | owner |
|---|---|---|---|
| B-01 | F1 冻结包基线：任何公共头增/删/改名需同 slice 重冻结（glob 收缩、79 头门保持为真、双 manifest + 归档基线 sha256、`verify_f1_package.py --check-frozen` 复现）。R2 细化：production_diff 范围为 `git diff {baseline}..HEAD -- src include`（verify_f1_package.py:401-402），F1_B_PROVENANCE_VERIFIED 要求其为空（:462-463）——已安装头的内容编辑（含私有段）或任何生产 src 变更同样使 --check-frozen 复现失败，触已安装头内容的 slice 须同 slice 重冻结，即使无增/删/改名 | c-new03-901/902/903、c-x10-901、c-new01-902/903、c-x12-901..903 | 各收缩 slice（F2/F3/F4）；harness 归 M |
| B-02 | `SLUICE_COPY_INTERNAL_TESTING` 未接线 → DirFsyncScript EINTR 注入 oracle 不可达，X-13 迁移缺对照证据 | NEW-02c、c-new02-001、c-x13-001 required_evidence | M（先接线）再 F4 |
| B-03 | 本机缺系统 liburing：liburing=y app 链接失败（cannot find -luring），liburing profile app 构建/链接未端到端验证 | c-copy-013、c-hash-011、c-grep-011、c-tail-012 | F5 证据义务 |
| B-04 | 宿主替换未决：StackfulIoHost 为 ADR-0003 PROPOSED / D2 IMPLEMENTED_UNVERIFIED，未采纳；multi-worker→single-owner 静默转换被 M 禁令禁止 → 四 app 迁移无目标拼写前不可开工 | c-x08-001..008 / c-x09-001..004 replacement 字段；c-copy-001/002、c-hash-001/002、c-grep-001/002、c-tail-001/002 | F3 决策（走 root/ADR 流程） |
| B-05 | RequestHandle 最终拼写 UNDECIDED（D-17）挡住 identity_of arena fallback 移除（fallback 今日休眠但为指定 release owner） | c-x02-002/003；X-02 live evidence (5) | F2 |
| B-06 | 形式对应重映射义务：ReleaseBind↔R-07（F2 改 release 路径时）；ObserverCore host actor↔RuntimeTaskContext（F3 动 RuntimeTaskContext 时）；ObserverCore:14-17 点名 scheduler 两文件（F3 移动时须改对应头并重跑 C1） | c-x11-901、c-x11-902、c-x11-003 | F2 / F3 |
| B-07 | vectored 能力决策（PROD-03，root:127；ledger:56 DEFERRED_OUT_OF_SCOPE）挡 L-01/退役与 wal/copy_all/Observed\* 命运；NEW-01b 的分发不对称必须进入该决策 | c-x06-002、c-new02-001/002（NEW-02b）、c-x06-003、NEW-01b | F4 + root 修订 |
| B-08 | X-16 收口顺序未决（await 重定型 vs runtime 退役，DAG 明示未决）+ helper-before-runtime 顺序未证明（helpers 需先脱离 scheduler/fiber 头编译） | c-x16-001、c-x05-001 | F3 |

## 6. UNKNOWN_EXTERNAL_USE（安装面无 in-repo 证据，外部使用不可知）

M 铁律：安装头收缩 = 收缩 F1 包面；下列 surface 的处置必须经包面政策/manifest 证据，不能
以"in-repo 零消费者"推定安全。

| id | surface | 依据 |
|---|---|---|
| U-01 | include/sluice/async/condition.hpp（+ semaphore.hpp、async_queue.hpp） | c-x17-007（condition.hpp 零 repo includer）；X-17 hunter 证据；c-new03-902（standalone-compile 门是首个消费者）；F5 manifest 证据 + 外部消费者政策未决 |
| U-02 | include/sluice/memory_io_context.hpp | c-new02-003（零用户实测；f1-package-manifest-liburing.json:95,:99 约束） |
| U-03 | include/sluice/io_context.hpp（BlockingIoContext） | X-07 live evidence (3)(6)（零 consumer）；c-x07-001 |
| U-04 | include/sluice/file.hpp（FileReader/FileWriter legacy 面） | X-06/X-07 live 证据（in-repo 消费者已全列）；c-x07-001 required_evidence；c-new03-901 |
| U-05 | include/sluice/wal.hpp、copy.hpp、observed.hpp | R2 修订（§7.2）：copy 半句翻案——copy_all 非仅自身 TU：src/reader.cpp:3 include copy.hpp、Reader::stream_to 两重载即 copy_all 调用（reader.cpp:48-56，公开 API reader.hpp:23-28，reader.hpp 在冻结 manifest HEADERS 内）= 生产消费者 c-new02-901；tests 0/apps 0 仍成立。wal/observed 零下游经 R2 复核为真（grep -rln sluice/wal.hpp|sluice/observed.hpp → 仅 wal.cpp/observed.cpp）。NEW-02b 静默搁浅对 copy.hpp 在 in-repo 已实现：退役 copy.hpp 须同批切断 reader.hpp/reader.cpp→copy.hpp 反向边（公开 API + B-01）。安装经 sluice_core glob（c-new03-901）；B-07 门控 |
| U-06 | include/sluice/detail/posix_retry.hpp | X-13 live 证据（两份 manifest :88 均安装）；c-x13-002；c-new03-901 |
| U-07 | 待收缩的 async 旧家族头（completion.hpp、detail/request_arena.hpp、fiber.hpp、fiber_ctx.hpp、scheduler.hpp、batch.hpp、…） | NEW-03 hunter 证据（79 头冻结集全含）；c-new03-902/903——外部包消费者只受 B-01 重冻结契约约束 |
| U-08 | Scheduler/Fiber 耦合的 select/event/queue/future/wait_policy 安装面：include/sluice/async/{select,event,future,wait_policy,wait_queue,timer_registration,select_fwd,lock_guard,mutex}.hpp + include/sluice/async/detail/{queue_item,queue_port,select_registration}.hpp（R2 补录，见 §7.2） | 12 头全在两份 F1 manifest 79 头集内，M0 六路扫描零消费者路径记录（生成时对本文件+JSON 精确匹配 0 命中——M 铁律：零 grep≠无用）。live include 图（R2 实读）：select.hpp:10 include scheduler.hpp、TimerSelectCase(Scheduler&,:92、friend :58/:85/:96；event.hpp:16 `Event(Scheduler&,…)`；wait_queue.hpp:32、timer_registration.hpp:70 friend；select_fwd.hpp:23 `select(Scheduler&…)`；scheduler.hpp:3-17 安装 detail/{queue_port,select_registration}+select_fwd+lock_guard+mutex+timer_registration+wait_queue（另 fiber/fiber_ctx）；group.hpp:5 安装 future.hpp（X-17/M-R 记了 Group 本体未记此边）；evented_wait_policy.hpp:4 安装 wait_policy.hpp（NEW-01a 记了 evented 本体未记随之安装的 WaitPolicy/Future 面）。in-repo 消费者=scheduler 机制 TU 自身（src/async/scheduler*.cpp、select*.cpp、queue_port.cpp）+ 安装头闭包（async_mutex/semaphore/condition/async_rwlock 骑 wait_queue.hpp）；外部使用不可排除。F0 adopted census 已定角色：H-14 Future『only consumer is Group』、H-15 WaitPolicy、H-17/H-18 dormant-primitives family、H-19 wait_queue INTERNAL（f0-role-profile-census.md:206-211）+ :98-99 mutex_test_seam 生产传递闭包。处置=U-01 同类 F5 包面/外部使用政策决定；M-H host 裁决（B-04）的耦合安装面输入须按 ~17 头计 |

## 7. FALSE_OLD_ASSUMPTION（F0/旧记录被推翻的断言）

1. **X-11 "none maps to Completion" 不成立**：RequestCore.tla:114-116/:432 将 ReleaseBind
   定义为 compatibility release flavor，formal map:268 明文 "the compatibility Completion
   release stays ReleaseBind"——Completion 释放路径存在正式 correspondence 记录。
2. **X-11 "active models map to core machinery only" 不成立（RuntimeTaskContext 半）**：
   ObserverCore.tla:28,:62,:364 把 Host waiter actor 对应到 RuntimeTaskContext waiter path
   （C1-D，root-DEFERRED 的 H-08 面）；"none maps to Scheduler" 半句仍成立（排除性注记）。
3. **F0 census §1.2/FA-1 "无任何 install/export/package 规则、INSTALLED=NO" 在 HEAD 已被
   F1 推翻**：libraries.lua:18-21,:36-38 安装集 + verify_f1_package.py install/79 头门 +
   冻结 manifest（HEADERS=79，含全部旧家族头）。F1 记录自证该零规则状态在 a34d96c6 时仍真
   ——是 F0 之后被新代码推翻，非 F0 取证错误。
4. **X-01 "25 test files name the type" 过时**：`grep -rlw Completion tests/` = 27
   （+contract_observer.cpp 实用、+w02_pipeline.cpp 注释）；"15 includers" 与 "4 apps" 不变。

### 7.1 其他计数/措辞漂移与细化（记录在案，非翻案）

- X-03/X-04：F0 KIND 记 TRANSITIVE_INCLUDE，实为 DIRECT include（细化）。
- X-06：oracle 共享降为 PARTIAL——legacy 关闭 fd 伪造 permission_denied（file.cpp:93-94），
  ledger GAP 行 ：149 在案。
- X-11/map：formal evidence map 存量 10 模块/158 cfg → 实测 12 .tla/192 .cfg（C2-E/E2 后新增）。
- X-14：F0 "14 declarations" 与 live 展开计数（12 声明点 → 81+2 liburing / 43+2 noliburing）
  是不同口径；hunter 证 xmake 文件字节不变（见 §8）。
- X-18/FA-1：'installed' 半句的事实底座被 F1 改变（11/11 app include 已属安装集，但构建仍走
  源码树 include/）。
- 四 app README 引用的测试目标（sluice_copy_\* ×9、sluice_hash_\* ×3、sluice_grep_\* ×3+变体、
  sluice_tail_\* ×3）与 scripts/hardening.py 已随 5f62b55b 删除——**被删除的 consumer 不是被
  迁移的 consumer**，不得计为迁移证据；现存唯一 app oracle 是四个 app_\*_consumption_test。
- copy README:253-254 与 CHANGELOG 的 docs/history 悬空路径（X-18/NEW-01d）。
- wal 下游计数漂移：census L-06 'tst 1' → 实测 0（c-new02-002 内记录）。
- copy/hash README "usage error (exit 1)" 对 --buffer-size 不成立（实测 exit 2；仅 tail 对
  buffer-size 封顶成立，cli_parse.cpp:99-103）。

### 7.2 R2 审查轮修订（三视角评审后回写，2026-10-07）

本节记录对两条 M0 断言的翻案/补录。修订仅落在计划工件（本文件 + m-consumer-edges.json +
#458），不改任何生产代码、不改写任何 f1-*.json 冻结基线；JSON 编辑脚本与输出存
/tmp/m-r2-fixes/（apply_r2.py + stats-after.txt），§4 统计由该脚本重算打印。

1. **U-05『copy_all: own TU / zero downstream users』半句翻案**：`grep -rn copy_all src
   include tests apps` → src/copy.cpp（自身重载族）+ src/reader.cpp:51,:55 两个生产调用点
   （reader.cpp:3 `#include <sluice/copy.hpp>`；Reader::stream_to 两重载声明于公开头
   reader.hpp:23-28，reader.hpp 在 F1 冻结 manifest HEADERS 内）。『tests 0/apps 0』经复核
   仍成立；wal/observed 零下游复核为真。补录消费者记录 c-new02-901（NEW-02b/M-F/CALL/
   UNMIGRATED/owner F4/stream review-r2）；NEW-02b 的静默搁浅对 copy.hpp 在 in-repo 已实现。
2. **Scheduler 耦合安装面补录（U-08）**：12 个已安装头（9 公共 + 3 已安装 detail）在 M0
   生成时对 census MD+JSON 精确匹配 0 命中，系扫描流未产出该面消费者路径，非该面不存在。
   M-H exposureCounts 的『公共头 5』由此系统性少算 Scheduler 世界的耦合安装面（实测 ~17 头，
   计入 U-08 的 9 公共 + 3 detail + 原记录 5）。家族裁定（M-H B-04、U-01 类包面政策）输入
   以 U-08 为准。

## 8. 数据歧义与统计口径（不裁决，留 M1/评审）

- **X-01 计数方法差**：`grep -rl 'Completion<'` = 26 文件（25 .cpp + semantic_path_probes.hpp）
  vs `grep -rlw Completion` = 27 文件——不同 pattern 的不同口径，两数均如实记录。
- **X-14 状态 SPLIT 的实质**：live 流以"展开 target 数"为口径判 F0 旧数失效（CHANGED）；
  hunter 流证明 xmake/tests.lua 自 989d5c3f 字节不变、define-site 口径（13）与 census 14 一致
  （UNCHANGED）。文件未变，变的是计数口径——两份报告都是对的，合并条目不择一。
- **X-18 状态 SPLIT 的实质**：live 流判"事实底座变化"（安装集成立但构建仍走源码树）；
  hunter 流判"文档文本未变"。同为真，谓词不同。
- **contract_observer.cpp 双记录**（c-x01-041 vs c-x01-901）：family/edge_kind/migration_state
  判定不一致（aux/TEST_ORACLE/COMPAT_TEST_RETAINED vs M-R/CALL/UNMIGRATED）。两条均保留；
  §4 统计按记录计（含此重复），M1 定 family 时须先裁定。
- **c-x15-003 的 owner=M 与 family=M-R 并存**为输入原貌，未改动。

## 9. 四 app 深审摘要（全量行见 JSON `APPS`）

- **sluice-copy**：异步面全在 legacy multi-worker runtime（copy_task.hpp:3-5；
  run_task_to_result :358）。caller-owned Completion 成员 read_c/write_c/sync_c（:37-38,55，
  地址稳定 = README:126 L7 纪律）；任务退出前 drain outstanding()||ready()（:273-284）；
  submit 失败不等待。风险注记（代码读取，未复现）：await 助手失败路径提前返回不 reset +
  drain 遇错即 return，若可达且仍有 outstanding 会命中析构 fail-fast。直接内部头 1 处
  （posix_retry，X-13）。README 死引用：docs/history 路径 + 9 个已删测试目标 + 不存在的
  hardening.py。实测（noliburing releasedbg）：10MiB、--workers 4 --pipeline-depth 4 --sync
  data → exit 0、cmp 字节等价、read_ops=14；--no-atomic → exit 0。
- **sluice-hash**：run_task_to_result :128；栈上 Completion :43（单 op 生存期）；await_read_at
  :51。per-file 错误隔离 + engine 级 fail_all；退出码 3(canceled)>2(error)>0；错误标签粒度粗
  （invalid_state/no_space 均显示 "read error"，实测 exit 2）。实测 --workers 3 双 10MiB 摘要
  与 sha256sum 一致；--workers 65 → usage exit 1。
- **sluice-grep**：run_task_to_result :150-151；栈上 Completion :45；MatchSink 在 task 上下文
  同步流出（单 task 假设下输出确定）。退出码为 grep 传统 0/1/2（canceled 计 2，已文档化偏离）；
  pattern 含换行被拒 exit 2。chunk-invariance 声明当前无在树证据载体（旧 oracle 已删）。
- **sluice-tail**：四 app 中唯一直接拥有 ApplicationRuntime（Impl::rt :292,335 +
  RuntimeBuilder :328-330），不经 run_task_to_result；`wait()` 是唯一安全收尾
  （~ApplicationRuntime 对非 Stopped 状态 fail-fast，application_runtime.cpp:134-140；引擎无
  析构侧安全网）。信号桥：pthread_sigmask + 专用 sigwait 线程 → request_stop（不能在信号
  处理器内调用）。实测：tail -f 追加两行均输出、SIGINT → exit 0；-n 2 正确。
- **共同事实**：四 app 均无 submit_\*_request/RequestHandle/Request\<T\>/StackfulIoHost 命名；
  noliburing 构建链接全成功；liburing=y 因本机缺系统库链接失败（B-03）。

## 10. 复现

```bash
git rev-parse HEAD            # c782099f56c6afd62daad2929944673059aef573（BASE_SHA）
date -u +%FT%TZ               # 生成时间戳（JSON GENERATED_AT）
python3 /tmp/m0-census/build_census.py   # 由 /tmp/m0-census/{edges-live-1..4,edges-hunter,apps-1,apps-2,hunter}.json
                                         # 机械生成 docs/review/m-consumer-edges.json 并输出 §4 统计
python3 -m json.tool docs/review/m-consumer-edges.json > /dev/null   # JSON 合法性
```

R2 审查轮修订（2026-10-07）复现：

```bash
# U-05 翻案 + U-08 补录 + B-01 细化（脚本同时重算 §4 统计）
python3 /tmp/m-r2-fixes/apply_r2.py                      # 输出存 /tmp/m-r2-fixes/stats-after.txt
grep -rn copy_all src include tests apps                 # → src/copy.cpp 重载族 + src/reader.cpp:51,:55
grep -rln "sluice/wal.hpp\|sluice/observed.hpp" src include tests apps   # → 仅 wal.cpp/observed.cpp
python3 - <<'EOF'   # 12 头 vs 冻结 manifest 与两份 census 工件的精确匹配（生成时 0 命中）
import json
hdrs = json.load(open('docs/review/f1-package-manifest-noliburing.json'))['HEADERS']
targets = ["async/select.hpp","async/event.hpp","async/future.hpp","async/wait_policy.hpp",
"async/wait_queue.hpp","async/timer_registration.hpp","async/select_fwd.hpp","async/lock_guard.hpp",
"async/mutex.hpp","async/detail/queue_item.hpp","async/detail/queue_port.hpp",
"async/detail/select_registration.hpp"]
print(all(any(h.endswith(t) for h in hdrs) for t in targets))   # True = 全部在 79 头集内
EOF
```

扫描输入快照（47 份边报告 + 4 份 app 深审 + 4 条 hunter 假设）保存在 /tmp/m0-census/，
JSON 工件的每个字段均由其生成；本文件的计数由同一脚本打印，非手抄。
