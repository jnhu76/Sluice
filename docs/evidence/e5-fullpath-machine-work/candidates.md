# 候选裁决矩阵（e5-fullpath-machine-work，Owner 决策矩阵）

- 撰写：候选裁决官，2026-11-18。基线 master `5d94f233`（冻结）。
- 输入（全部只读）：同目录 `dag.md`（80 边/22 候选）、`p1a-cost-map.md`（本战役唯一可信
  instr/op 口径）、`measurement-audit.md`（§2 推翻/降级清单）、`p0c-cache-study.md`、
  `history-digest.md`（§5 复现资格审计、§2 KEEP_IMMEDIATE 基线）；佐证锚点回源码核实
  （见 §5 核实记录）。
- 本文件性质：裁决与选择，不构成任何改动授权；实验执行另有预注册协议
  （`experiment-protocol.md`，草案，未执行）。
- **修订 r2（独立挑战者缺陷修复）**：F2——#12（M-40）初版"relaxed≈普通访存、机制增量
  ≈0"对写侧 RMW 不成立，改判 NO_GO→INCONCLUSIVE 并登记 dag.md M-40 同源前提错误
  （§1/§6）；F3——tests.lua 重编译点实为 12 处（初版 grep 经 `head` 截断漏 ：1139/:1205，
  恰为三消费套件中的两个）（§6）；F6——io-wq 行撤出被审计判"口径漂移、不进裁决"的
  ≈7–10.5% 区间，改用逐值计数（§2）；F8——补 work_mtx_ settlement 联合快照绑定，
  闭合"无新空间"完备性（§5）。F1/F4/F5/F7 属协议文件，见其修订记录。
- 引用纪律：P0-C bench 表 instr/op 与 cyc/op 绝对值被 measurement-audit §2-1 推翻
  （×9/7 膨胀），本文件一律不引用；需要 per-op 指令口径时只用 P1-A §1/§4 的
  540000-op 分母口径。Top10 #9 的 submit_read 数字按 measurement-audit §2-7 修正
  （raw d4=0.93 / d16=1.32 / d64=1.68）。tp 内核锁机器总量引用 Top10 #7 的
  ≈7.1% 枚举（段 7 的 ≈8.3% 无枚举不可复核，measurement-audit §2-6）。
- **修订 r3（2026-11-18，Owner 归档复核后）**：#16/#19 的 r6 R2/R3 对位修正
  （S3 闭合）——R3（0.45% < 1% MME REJECTED，含 `publication_inflight`
  反例与确定性测试注入点）= #16 / M-61 发布锁融合，历史拒绝理由挂回 #16；
  R2（0.90%）= outstanding_/health_failed_ core 摘要原子化，与 #19 / M-67
  仅部分重叠；#19 原对 R3 的错引移除。依据：#621 r6 报告 §B.3/B.5 原文
  （`0e50612c` 证据分支 REPORT.md），Owner 复核 2026-11-18 确认。

## 0. 裁决纪律（口径）

- **GO**：证据足以进入单因素消融预注册（性能路径必须 MEASURED；机制正当性门独立路径
  须七问可答且正确性证明可完成）。本轮单因素槽位至多 1 个。
- **NO_GO**：证据支持不做（负裁决先例在身、无可消除机工、消除属他方所有权）。
- **INCONCLUSIVE**：证据不足（PARTIAL 族级、NOT_OBSERVABLE 形态、NOT_CONSTRUCTIBLE
  前提、未行使）——须补充指定证据后重裁。
- **ENVIRONMENT_BLOCKED**：被环境硬阻断（本轮无一条适用：所有"测不到"均为负载形态/
  协议缺口，皆可构造，见 §4 注记）。
- **SEMANTIC_MISMATCH**：所有权/语义裁决项（root 义务存储 vs 死存储、seam 义务归属），
  非性能候选，owner 裁决前置。
- 翻案条件一律写明"需要什么新证据"。

## 1. DAG §4 的 22 条候选

| # | 合并边 | 候选内容 | 裁决 | 依据 | 翻案条件 |
|---|---|---|---|---|---|
| 1 | M-28 | syscall_count_ 生产侧宏门控剔除（写侧 `threadpool_backend.cpp:434` + 成员 `hpp:258`，访问器/定义已在测试宏内） | **GO**（本轮唯一选定，见 §3） | MEASURED：指令级 `lock addq` 占 worker_loop 本地 30.24% × 1.83% ≈ 0.554% 全局 cycles ≈ 170 cyc/op @tp-w8（p1a §4-1）；唯一读者在测试宏内（dag.md §7-1，本会话复核 hpp:66-76/seams:4/seams:108-110）；测量 hot-spot 前置已落地（p1a §4-1）。预期效应 <1% 且 w8 竞争放大未分解 → 走 AGENTS 机制正当性门独立路径（test-only 证据不得在生产运行；#635 DrainObservation 同型先例），性能声明由协议实测并如实报小。C 类（不动提交时序） | 正确性门失败（语义 oracle/mutant/矩阵/TSAN/ASAN）或 A/B 显示劣化超保护界 → NO_GO；出现生产读者 → 重新物化为生产权威，宏门控不成立 |
| 2 | M-80 | write_all_at 逐步重校验 → prevalidated_pwrite_step | **INCONCLUSIVE** | 未行使：直呼形态不在 W-02 负载，本轮零测量（p1a §6"M-80 未行使；候选证据仍为 DAG 静态论证"）；仅有静态论证 + 同文件 read 侧免重校验先例（dag.md M-80）。无 measured hotspot，性能路径不可 GO | 直呼组合写形态（bench W-01/blocking_file 可构造）测得 hotspot + "compose 仅由已校验入口进入"不变量证明 → 单因素 A/B |
| 3 | M-16 | uring 后端层 state-op 校验收敛为单层 | **INCONCLUSIVE** | PARTIAL：uring submit_read 自份额 0.98%（含 validate+准备），内联限制无法行级分离 backend 层 state-op 半边（p1a §4-7）；收敛前证（"生产调用方必经 context 层"结构保证 + seam 直连测试义务冲突）未解，需 owner（dag.md M-16） | owner 裁决 seam 冲突 + 隔离 backend 层 state-op 半边的单因素 A/B |
| 4 | M-37 | 两处 dispatch 调用点 remove_exact(front) → pop_front | **INCONCLUSIVE** | 前提 NOT_CONSTRUCTIBLE：W-02 提交即清空 → 队列长 ≈1，front 删除的 (size−1)×16B 前移份额 ≈0（p1a §4-6）；机工事实成立（两处调用点恒 front，dag.md M-37）。深队列形态属 #595 批量概念 B 所有权 | 深队列派发形态实测 memmove 份额（deferred-submit 家族，#595 重开时）或证明生产全形态队列深 ≤1 → 塌缩为 NO_GO |
| 5 | M-53 | cookie O(R) 逆向扫描 → user_data 路由索引编码（H1） | **INCONCLUSIVE** | 热点 MEASURED（d4 0.14%/d16 1.03%/d64 1.74% cycles，≈63/116 cyc/op @d16/d64，O(D) 增长成立；p1a §4-2），但迁移收益 PARTIAL：三项证明义务未证（ledger 物理位置单调/控制 CQE exact-submitted/cancel 定向，dag.md M-53）；效应 1.0–1.7% 处于分辨力边界；d16 每条目 7.9 cyc vs d4/d64 3.6–4 cyc 的缓存效应未分辨（斜率 INCONCLUSIVE，p1a §4-2/§6） | 三项迁移义务证明完成 + d64 深度格 A/A 分辨力达标（半宽 ≤ 效应/2）→ 下一轮 GO 资格 |
| 6 | M-63 | uring owed sweep 逐槽锁 → 单锁快照（H2a） | **INCONCLUSIVE** | W-02 形态 NOT_OBSERVABLE：无零长 op → owed_count_ 恒 0 → 循环休眠，仅剩 gate 一次锁收购（p1a §4-3）；既往 H2a −370ns/pass @C=64 idle 为 idle/控制面形态证据，本 regime 集不可复测（p1a §4-3）；owed 机制本体 KEEP（OBS-02/PROG-01，dag.md M-63） | 零长 op 负载或 idle 形态协议激活循环 + 交付顺序不变性确认 → 单因素 A/B |
| 7 | M-09 | Slot::borrow 只写不读（root 义务存储 vs 死存储） | **SEMANTIC_MISMATCH** | 生产只写/清、无读者，SlotObservation/CoreSnapshot 均不含 borrow（dag.md §7-2 grep 回执）；root 把 borrow facts 列入请求记录域（docs/explicit-io-contract.md:586）——所有权裁决：等待消费者的 root 义务存储还是死存储，非性能候选（dag.md #7） | owner 裁决：修 root 摘除 borrow facts（→ 删除 PR）或出现真实消费者（→ KEEP 为义务存储） |
| 8 | M-30 臂 | zero_effect_cancel 仲裁臂生产零生产者 | **SEMANTIC_MISMATCH** | 该臂生产零生产者（dag.md §7-8 grep 回执），CANCEL-01 同一规则已由 cancel() 入口强制（M-13，request_core.cpp:260-275）；是否 seam 必备义务（协议完备性测试面）属 owner 裁决，仅该臂，offer_terminal 其余 KEEP（dag.md #8） | owner 裁决：认定 seam 义务（KEEP）或修 root 去掉冗余第二强制点并迁移 seam 测试 |
| 9 | M-57 | acquire_execution 生产零调用 | **SEMANTIC_MISMATCH** | 生产 grep 零调用，仅协议测试经 driver（dag.md §7-8）；热路径成本为零（p1a §2 段 9 只含 offer+release）；BACKEND-01 seam 面 + 协议完备性测试面的"必需义务"裁决属 owner（dag.md #9），删除只是代码面收缩、无性能声明空间 | owner 裁决 seam 必要性；若判可删，走代码面收缩 PR（无性能门） |
| 10 | M-12 | close_admission 双实现二选一 | **SEMANTIC_MISMATCH** | TP 与 UR 两个 backend 版均生产零调用（仅 tests/progress_source_ownership_test.cpp:468,471），context 直连版是活机制（dag.md §5-3/§7-11/§6-2）；保留哪份 + 轨迹 oracle 归属属 owner（dag.md #10） | owner 指定存活实现 + 测试 oracle 迁移方案 |
| 11 | M-42 | submit_flushes_ 生产只写不读原子计数 | **GO**（排队，不占本轮槽位） | MEASURED-small：fetch_add 在早退分支后（`uring_transport.cpp:222`，本会话复核），1 RMW/flush，符号自份额 0.01%（p1a §4-4）；读者全在测试宏（dag.md §7-6）；与 #1 同型宏门控路径（#635 DrainObservation 先例）。单机制规则禁止与 #1 捆绑 → 在 #1 回执落地后独立单因素推进 | #1 回执 + 独立单因素 A/B；机制门七问不成立或出现生产读者 → NO_GO/重新物化 |
| 12 | M-40 | live_cookies_/live_control_sqes_ 原子冗余于 dispatch_mtx_ | **INCONCLUSIVE**（r2 改判，原 NO_GO 前提错误） | 静态可消除机工存在：live_cookies_ fetch_add/fetch_sub（`uring_transport.cpp:195,581`）+ live_control_sqes_ fetch_add/fetch_sub（`:295,:427,:490`；`uring_completion.cpp:118`）= 每 read op 2 条 lock 前缀 RMW（control op 另 1 add+1 sub）——初版"x86 relaxed≈普通访存"只对 load/store 成立、对 fetch RMW 不成立（挑战更正，本会话 grep 复核）。但与 #11 的本质差别：M-40 计数器**有生产读者**（`uring_backend.cpp:350-351,371-372` 静默检查，dag.md M-40 断言全部访问在 dispatch_mtx_ 域内、降级前须重证）→ 宏门控不可用，降级 = dispatch_mtx_ 域内普通成员 + 缝读者（`uring_test_seams.hpp:79-80`）改加锁，属缝行为变更的机制改动；且无行级 measured share（p1a 无该指令归因；Top10 #10"无读者 RMW"伞标签对 M-40 不准确）。同源前提错误登记：dag.md M-40 B-EC"原子实现冗余于互斥锁（x86 relaxed ≈ 普通访存）"的括号论断对写侧 RMW 错误 | 行级 measured 归因 + 生产读者角色评估（quiescence 派生摘要的机制门作答）+ dispatch_mtx_ 覆盖重证 + 缝读者加锁设计 → 单因素 A/B |
| 13 | M-61 | PublicationPayload 快照仅测试可达（生产恒 nullptr） | **NO_GO** | 生产 begin_publication 四处恒传 nullptr（dag.md §7-15：TP :338/:358、UR :558/:584）→ 生产路径无快照成本、无 measured hotspot（p1a §4-8 仅计数族）；属代码面/测试通道卫生而非性能候选（dag.md #13） | 生产 payload 消费者出现（→ 重新物化为正当机制）或 owner 批准快照下沉测试专用通道（卫生路线，非性能门） |
| 14 | M-01 | scope O(S) 扫描份额（H3 INVESTIGATE 开放；free-list 已 REJECT） | **INCONCLUSIVE** | PARTIAL：wait_for 0.87→1.36→3.04%、take 0.62→1.08→2.34%、submit_read 0.93→1.32→1.68%（d4/d16/d64，按 measurement-audit §2-7 修正）但含 bench 驱动 window.erase O(D) 污染（run_pipeline_phase 2.5–3.0% 须剔除，p1a Top10 #9）；free-list 替换已被 m1-results REJECT（dag.md M-01） | 高重复 `-scope64` 隔离协议剔除驱动污染后给出机制可归因份额 → 单因素 A/B |
| 15 | M-10 | 零长发布三次 core 锁融合 | **INCONCLUSIVE** | PARTIAL 族级（core 族 ≈11 锁/op × mutex 家族 6.6% cycles，p1a §4-8）+ W-02 无零长 op 形态休眠（同 #6 前提）；步间裁决值与 fail-fast 绑定属机制变更，零长占比未测（dag.md #15） | 零长占比测量 + 步间语义映射 + 融合单因素 A/B |
| 16 | M-61 | begin/complete 两锁融合 | **INCONCLUSIVE** | PARTIAL：计数落 core 族但内联限制无法行级分离（p1a §4-8）；步间夹记录读与测试暂停点语义须保（dag.md M-61/#16）；S3 已闭合（r3）：r6 §B.3/B.5 的 R3（0.45% < 1% MME REJECTED，含 `publication_inflight` 反例与确定性测试注入点）即本候选，历史拒绝理由挂回本行 | 新 measured profile 推翻 r6 R3 拒绝 + 保步间语义的融合单因素 A/B（族 A/A 先行） |
| 17 | M-62 | 无观察者跳过 claim/retire 协议空转 | **INCONCLUSIVE** | PARTIAL：每 op 2 次 core 锁投递空回调已计数、未行级分离（p1a §4-8/Top10 #5；dag.md §6-1 更正"快速返回≠零锁开销"）；协议安全论证存在但 delivering 相位可观测性 + routing_sink 交付点须重证（dag.md M-62）；对位 history-digest §1.2 项 6 mutex family ~9-11% 候选（r6 F.6：需新 measured profile + 单因素 A/B） | 新 measured profile 行级分离 + 协议重证 + 单因素 A/B |
| 18 | M-65 | owed release_control 第三锁融合 | **INCONCLUSIVE** | 本轮零测量：TP owed 扫描被 #634 owed_count_ 门控，W-02 无零长 op → 不运行（与 #6 同激活前提）；同融合族语义绑定（dag.md M-65/#18） | 零长/owed 形态协议 + 第三锁与 retire 融合语义证明 → 单因素 A/B |
| 19 | M-67 | sticky 双标志原子化/融合查询 | **INCONCLUSIVE** | PARTIAL：每 pass 3 次互斥读两个永不复零标志，深度摊薄曲线 + progress 族 1.25/op 簇，行级未分离（p1a Top10 #8）；对位 r6 R2（outstanding_/health_failed_ core 摘要原子化，0.90% < 1% MME REJECTED，history-digest §1.2 项 6）——部分重叠非同机制；R3 不对位本候选（R3 = #16/M-61 发布锁融合，r3 修正原错引）；ProgressSource eventfd 单读者协议是机制一部分（dag.md M-67） | 行级 profile 分离 + A/A 分辨力 ≥1% MME 达标 + 单读者协议重证 → 单因素 A/B |
| 20 | M-33 | TP cancel ring 摘除惰性失效 | **INCONCLUSIVE** | 本轮零测量（cancel 形态不在 W-02）；替换须新增失效计数机制，removed_entry signal 与队列空判定依赖真实摘除（dag.md M-33 B-UNPROVEN）——加机制须过机制正当性门 | cancel 形态 measured hotspot + 失效机制设计证明（两语义保持）→ 单因素 A/B |
| 21 | M-74 | ready() 快路径与 wait_one 重查合并 | **INCONCLUSIVE** | 收益方向因 ready 概率而异，无证据不裁决（dag.md #21）；消费侧份额 PARTIAL 且含驱动污染（p1a Top10 #9 + measurement-audit §2-7） | ready 概率分层测量给出方向 + 合并收益隔离证据 → 单因素 A/B |
| 22 | M-80 hygiene | file_semantics.hpp compose_progress/CompositionState 模型词表生产零调用 | **SEMANTIC_MISMATCH** | 生产零调用的声明-使用错位（dag.md §5-3 家族、#22）；词表面归属（测试面 vs 生产头）是 hygiene/所有权裁决，与语义 oracle 共享词表面相关，非性能候选 | owner 裁决词表移入测试面（保持语义 oracle 共享）或出现生产消费者 |

## 2. 非候选热点（3 条）

| 热点 | 裁决 | 依据 | 翻案条件 |
|---|---|---|---|
| H1 提交批形态（per-dispatch flush，M-34/35/42） | **NO_GO**（本轮） | 现象 MEASURED（enter/op=1.000052 三 regime、内核 syscall 骨架 ≈23.2% cycles、#622 C1 −65%/−85% 定位；p1a Top10 #1/§7-1）；但消除机制 = 提交错峰策略变更，归 #595 系：KEEP_IMMEDIATE 在身（D2 FAIL 决定项幅度 4–7pp 非频率敏感，history-digest §5.1#1），重开仅按 RQ1 机制条件 + 整机重测；本轮属概念 B，不实现 | #595 RQ1 second-held-entry guard 机制 + 整机重测（R1–R5 原始数据未归档，history-digest §5.3-1）+ D1/D2/D3、64KiB、W-03、cancel falsifier 家族全过（round2 `A.4` 强制 falsifier） |
| io-wq punt FS 条件性 | **NO_GO** | 现象 MEASURED 且 FS 条件性成立（tmpfs punt 1.0000/op（540028/540028）、Δcycles:k +7304/op、Δtask-clock +2.87 µs/op、Δctx-sw +0.230，均为 measurement-audit §1.2 逐值复核的计数；btrfs warm = 0；u 侧 instr 差 0.5% → 全在内核；p1a Top10 #4/§7-2、p0c §4.2）。io_wq 内核符号合计份额为定位参考且两处口径漂移已登记（Top10 10.5% vs 段7 8.3%，measurement-audit §2-6 判"定位参考，不进裁决"），本行不引用其数值，NO_GO 方向不依赖它；消除属 FS/形态选择，非 Sluice 侧代码改动（p1a Top10 #4 明示） | 非 Sluice 代码候选，无翻案路径；产品准入换设备 FS 重测属 history-digest §5.3-2 的既列重测情形 |
| tp-w8 work_mtx_ 竞争（worker 超配） | **NO_GO** | 现象 MEASURED（11.16% acquisitions 竞争、futex 3.2/op 均值 42 µs、内核锁机器 ≈7.1%（Top10 #7 枚举，measurement-audit §2-6）、w8 CPU/op 10.35 µs vs uring 1.71 µs 而 instr:u 仅差 5% → 增量在等待；p1a Top10 #7）；补救 = 配置选择（w4 优于 w8，p0c §7-4）或分片队列类新权威（root 修订域），无最小可减机制；唯一减法交集是 M-28（该行是 shim 最大簇的物理根源之一，p1a §4-1），由候选 #1 承载 | 出现 worker 数被外部固定的 root 工作负载 + 最小机制提案过机制正当性门 → 单独预注册 |

## 3. 单因素消融槽位选择（产出三的输入）

选择规则（ask）：优先消除真实执行工作；profile 排名最高且正确性证明可完成；只改一个机制；
本轮槽位只给 C 类（不改提交时序减一项协调工作）或 NO_CHANGE_JUSTIFIED。

- **选定：#1（M-28 syscall_count_ 生产侧宏门控剔除）**。
- 排序理由：Top10 中候选成员排名 #1（M-28，Top10 #2）> #5（M-53，Top10 #3）——M-53 的
  消除依赖三项未证迁移义务（正确性证明不可即时完成）且斜率 INCONCLUSIVE；M-28 的消除是
  纯减法（无读者计数器移出生产 TU），正确性证明可完成（三个消费测试套件已用测试宏重编译
  后端 TU，构建模式现成——12 处重编译点含全部三消费目标，见协议 §3.1）；单机制、C 类
  （不动提交时序、不动批量拓扑）。
- #11（M-42）同型但排在 #1 回执之后，不占本轮槽位（单机制规则）。
- 拒绝 NO_CHANGE_JUSTIFIED 的理由：#1 有 MEASURED 指令级定位 + 可完成的正确性证明 +
  现成构建先例（#635 DrainObservation），符合全部三条选择规则。
- ENVIRONMENT_BLOCKED 使用注记：本轮 25 项无一适用——#4/#6/#18 的"测不到"是负载形态
  缺口（零长 op/深队列/直呼写组合负载均可构造），#2 直呼形态 bench（W-01）现成，均归
  INCONCLUSIVE 而非环境阻断。

## 4. P1-B 三概念裁决（产出二）

- **A = 维持 IMMEDIATE（默认）**：KEEP_IMMEDIATE 未被本轮测量动摇。本轮（kernel 7.3.3）
  只复现 H1 形状的定位证据（enter/op=1.000052，p1a §7-1），未产生任何批量化收益或危害的
  新可归因证据；D2 FAIL 决定项幅度 4–7pp 远超频率噪声且带 enters 不变的计数解释
  （history-digest §5.1#1），重开只按 RQ1 机制条件 + 整机重测，不因频率。
- **B = 后端本地自然/进度驱动批量**：本轮 **NO_GO/不实现**。负证据结构完整：K4-b 收益
  （d16 −28.07%/d64 −35.53%/d4 −13.06%/saturate −43.06%）被负例约束——64KiB p99
  +7.46% material regression（payload gate 必要性已证）、W-03 d4 +2.54%/D1 −2.50%/
  threadpool +1.19% 均 <3% 无归因资格（history-digest §2.1 no-mechanism controls）；
  D2 FAIL（+4.17% CPU、p95 +5.69%、p99 +7.45%、tput −3.99%，Holm-rejected 且复现）；
  R3/R4/R5 全 FAIL（p99 +37.85/+21.85/+17.97pp）。重开所需机制条件（本轮一个都不实现）：
  RQ1 batchability admission "second held entry" guard（round-e5 decision.md Next
  research entry）+ payload gate + 全 falsifier 家族（D1/D2/D3、64KiB、W-03、cancel，
  round2 `A.4`）+ 整机重测（R1–R5 原始数据未归档，history-digest §5.3-1）。
- **C = 不改提交时序减一项协调工作**：本轮唯一槽位归属 C 类。C 类候选清单 = 无读者
  RMW（#1 M-28、#11 M-42）+ 无观察者/锁融合协调面（#5 M-53 路由、#19 M-67 sticky 读、
  #15/16/17/18 融合族）中**不动提交时序**者；选定 #1。B 与 C 无交集（B 全部改提交
  时序/批量拓扑），不得混装；本轮单因素槽位只给 C 类，不给 B。

## 5. 减法透镜再审查（subtractiveFindings 依据）

1. **生产侧错位的 test-only 证据是本轮唯一干净减法空间**：#1/#11 两个无读者计数器的
   事实所有权（测试 oracle）与物化位置（生产二进制）错位；#635 DrainObservation 先例给
   出迁移形态，测试目标重编译后端 TU 的构建模式现成（本会话核实 tests.lua 三消费目标
   文件表 + 12 处重编译点，含全部三消费目标）。
2. **M-09/M-30 臂/M-57/M-12/#22 是所有权裁决空间而非减法空间**：root 把 borrow facts
   列入请求记录域、CANCEL-01 已由入口强制——"derived does not imply delete"同样适用
   于 seam 义务，单方删除会绕过 owner 裁决与 authority-contraction audit。
3. **融合族与 M-67 的减法空间是族级而非候选级**：core 族 ≈11 锁/op × mutex 家族 6.6%
   cycles 是 uring 形态最大用户态机工家族（p1a §4-8），但每个单融合被步间裁决值/测试
   暂停点/delivering 相位可观测性绑定，无免费删除。
4. **本轮最大的机器工作都在 Sluice 不可减的位置**：rep_movs 19.2%（payload 本体）、
   内核 enter 骨架 ≈23%（H1 = 概念 B/#595）、io-wq punt +7304 cyc:k（FS 形态）、
   tp-w8 竞争（配置/新权威）——减法透镜下均无 Sluice 侧最小机制。
5. **work_mtx_ 同锁护多个事实不是被遗漏的锁拆分空间（r2 补，闭合完备性）**：真实约束是
   settlement 联合快照 `internal_work_retired()`（`threadpool_backend.cpp:856-861`，本会话
   核实：同持 work_mtx_ 下联合判定 dispatch 空 + `active_workers_==0` +
   `publication_pending_` 空 + `!any_event_owed_locked_()`；dag.md M-29 同源"原子化会破坏
   internal_work_retired 的同锁联合快照"）；`pocketjs-method-notes.md` §三-7 同判——主
   杠杆是 worker 超配/调度形态（Top10 #7），锁拆分是受 settlement 快照绑定的次级小空间，
   拆分即破坏联合快照语义。tp-w8 非候选行的"无最小可减机制"以此为准，不是笼统概括。
6. **结论：无新空间**。除 #1/#11 的错位计数器与 DAG 已登记的 owner 裁决面外，本轮测量
   未打开任何新的减法机会（work_mtx_ 拆分经 §5-5 处置为绑定约束）；DAG 22 候选 + 3 非
   候选热点清单对本轮证据完备。

## 6. 本会话核实记录（裁决依赖的回源码检查，只读）

- `sed -n '425,440p' src/async/threadpool_backend.cpp` → :434 `syscall_count_.fetch_add(1, std::memory_order_relaxed);` 不在任何测试宏内；`sed -n '250,262p' include/sluice/async/detail/threadpool_backend.hpp` → :258 成员无门控；`sed -n '66,76p'` 同文件 → 访问器声明已在 `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` 内；`sed -n '1,10p'`+`sed -n '100,115p'` src/async/threadpool_test_seams.hpp → 文件级宏 :4、`syscall_count_for_test` :108-110。
- `grep -rn 'syscall_count' tests/` → 16 条计数断言全部在 tests/request_scope_test.cpp（:885,990,1133,1189）、tests/threadpool_request_core_integration_test.cpp（:265,289,380,422,463,487,493,522,569,943）、tests/public_request_test.cpp（:313,414）；apps/bench 零命中。
- `grep -n 'SLUICE_ASYNC_INTERNAL_TESTING' xmake/libraries.lua xmake/apps.lua` → 零命中（apps.lua:4 为注释）；`grep -c` xmake/tests.lua → 14。
- 构建模式（r2 更正为 12 处）：`grep -n 'threadpool_backend.cpp' xmake/tests.lua` 完整输出 **12 行**：:406,448,501,559,745,818,891,959,1015,1074,**1139,1205**——初版清单 10 处系该 grep 经 `| head` 截断所致（命令记录在案，如实更正）；:1139 属 `request_scope_target`（helper 定义 :1114、define :1122），:1205 属 `threadpool_request_core_integration_test`（define :1202）——恰为三消费套件中的两个，安全面清单必须完整。`request_scope_target`（tests.lua:1114 起）与 `public_request_target`（:731 起）均 `add_defines("SLUICE_ASYNC_INTERNAL_TESTING")` 且文件表含 `src/async/threadpool_backend.cpp`（awk 抽取核实）。
- **M-40 写侧/读者核实（r2，F2 依据）**：命令 `grep -n 'live_cookies_\|live_control_sqes_' src/async/uring_transport.cpp src/async/uring_completion.cpp src/async/uring_backend.cpp include/sluice/async/detail/uring_backend.hpp src/async/uring_test_seams.hpp` → 写侧 `uring_transport.cpp:195`（fetch_add）/`:581`（fetch_sub）/`:295`（fetch_add）/`:427,:490`（fetch_sub）+ `uring_completion.cpp:118`（fetch_sub），全部 relaxed 序但 fetch_* 在 x86 恒为 lock 前缀 RMW；生产读者 `uring_backend.cpp:350-351`（`!= 0` 静默检查）与 `:371-372`（`== 0 &&` 判定）；测试缝读者 `uring_test_seams.hpp:79-80`（load）与 `src/async/uring_transport.cpp:38-40`（`live_control_sqes_for_test`，文件级测试宏块内、#endif :41；同文件 `sq_ready_for_test` :33-36 已持 dispatch_mtx_，可作缝读者加锁先例）。**初版 #12"relaxed ≈ 普通访存、增量≈0"前提错误**；dag.md M-40 B-EC 括号论断同源错误，Top10 #10"无读者 RMW"伞标签对 M-40 不准确，均登记。
- **F1 case id 核实（协议修订依据）**：`sed -n '455,480p' bench/bench_w02_pipeline.cpp` → `make_meta` :462-467 `meta.case_id = "W02-" + params.backend->name + ... + (backend->name == "threadpool" ? "-w"+workers : "")`；:621-622 `BackendChoice threadpool{"threadpool",...} / uring{"uring",...}` → 真实 case_id 为 `W02-threadpool-4k-d16-w8` 等（P0-C §4.1 表内"W02-tp-…"为表格缩写非 case_id；P1-A §0 用全名）。`sed -n '490,502p' bench/bench_common.hpp` → `'='` 前缀为精确相等（`case_id == filter+1`）→ 缩写冻结值零匹配。
- **F8 联合快照核实**：`sed -n '846,866p' src/async/threadpool_backend.cpp` → `internal_work_retired()` :856-861 同锁联合判定（dispatch_.empty() && active_workers_==0 && publication_pending_.empty() && !any_event_owed_locked_()）；与 dag.md M-29、`pocketjs-method-notes.md` §三-7（该文件本会话在证据目录出现后实读，其条目 7"锁拆分是次级小空间"、条目 10"live_cookies_ 两读者都在 dispatch_mtx_ 锁内"与本节核实一致）互证。
- `sed -n '215,235p' src/async/uring_transport.cpp` → :222 `submit_flushes_.fetch_add` 在 ledger 空早退之后、无门控；`grep -n submit_flushes_` include/ → 声明/成员位置。
- `ls bench/mechanisms/` → h1_router_index_bench.cpp 在库（dag.md M-53 引用成立）。
- `grep -rn 'close_admission' tests/` → context 直连版由 request_error_domain_test 等使用；backend 版仅 progress_source_ownership_test（与 dag.md §5-3 一致）。
