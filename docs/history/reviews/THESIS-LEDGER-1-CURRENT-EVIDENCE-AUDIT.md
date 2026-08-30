# THESIS-LEDGER-1

**Current Evidence / Claim-Status Audit** — untracked audit artifact（按任务约束不 commit、不开 PR）。

- 审计日期：2026-08-30
- 审计对象：https://github.com/jnhu76/Sluice @ origin/master `7437c8c`
- 审计方法：机械证据审计（claim → evidence chain），非实现、非重构、非 roadmap 任务
- 证据状态词严格限于：`PROVEN — SCOPE-BOUNDED` / `SUPPORTED / PARTIAL` / `HYPOTHESIS` / `NOT YET TESTED` / `FALSIFIED / NOT SUPPORTED` / `OUT OF SCOPE / NOT A THESIS CLAIM`（另用 `NOT EARNED`、`UNAUTHORIZED` 标注未获授权/被路线图禁止项）

---

## CANONICAL THESIS

英文原句（#227 Thesis lock，未改写）：

> Sluice seeks the smallest explicit I/O boundary that makes
> correctness- and performance-relevant lifecycle state controllable
> and mechanically checkable, converts distributed asynchronous hazards
> into bounded protocol obligations, and does so without an unjustified
> abstraction tax.

固定中文表述（#227 comment 2026-08-30T02:04:52Z 锁定，未改写）：

> Sluice 寻找最小的显式 I/O 边界，使与正确性和性能有关的生命周期状态
> 变得可控制、可机械检查，把分散的异步危险迁移为有边界的协议义务，
> 同时不付出未经证明合理的抽象税。

本审计只评估证据，不改 thesis、不加 north star、不重定义 Sluice。

---

## AUDIT BASELINE

机械获取（2026-08-30）：

```text
git fetch origin                 # 完成
origin/master = 7437c8c58209f239051a8e814fd7ab44eabaada5
HEAD          = 7437c8c58209f239051a8e814fd7ab44eabaada5（branch: master）
git status --short：
  ?? docs/history/reviews/FE-FINAL-MULTI-FRONTEND-CLOSEOUT-REPORT.md   （人类 untracked，未动）
  ?? docs/history/reviews/R2-WAIT-LIFECYCLE-FINAL-CLOSEOUT-REPORT.md   （人类 untracked，未动）
```

PR #243（gh pr view 机械验证）：

```text
state = MERGED
mergedAt = 2026-08-30T01:34:59Z
mergeCommit = 7437c8c58209f239051a8e814fd7ab44eabaada5
parents = 4bee61f（master 侧）+ 19c1bde（PR head）
headRefOid = 19c1bde；human review 5059658754 = APPROVE FOR MERGE
git diff 19c1bde 7437c8c = 空（merge 树与 CI 双绿的 PR head 树逐字节一致）
```

补充机械事实：`.github/workflows/ci.yml` 触发器仅 `pull_request` + `workflow_dispatch` —— 本仓库设计上无 master push CI；7437c8c 树等于已通过 CI 的 19c1bde 树，故 master 树有 CI 双绿证据链。

**结论：MASTER 层与 ACTIVE-BRANCH 层合并。FE 证据（PR #243）为 MASTER-ESTABLISHED。**

v0.0.1 基线 = `a38df5e`（#227 参考基线，tag-only 版本裁决）。

---

## EVIDENCE RULES

四层事实分离（每条证据必须标注属于哪层）：

| 层 | 定义 | 例 |
| --- | --- | --- |
| A. ARCHITECTURE FACT | 代码结构/authority ownership（如 `resolve_` 是唯一 terminal CAS） | 证明结构，不证明"用户更安全" |
| B. VERIFICATION FACT | 变异/测试/模型对 defect 敏感（如删 winner law 后测试 RED） | 证明当前 witness 敏感，不证明所有同类 defect 可检 |
| C. PRODUCT FACT | 真实程序只用公共 header 可工作（四个 app） | 支持 usable，不自动支持 simpler/faster/AI-friendly |
| D. RESEARCH CLAIM | near-native 等比较级结论 | 只有公平、多环境、matched baseline 实验可支持 |

证据等级（审计中逐条标注）：

- **E0** current code / direct execution；**E1** current tracked evidence（docs/architecture、docs/verification、checked-in artifacts、ADR）；**E2** merged change evidence（merged PR、精确 commit、issue closeout、mutation evidence）；**E3** active PR/unmerged（本次不存在未合并的 thesis 相关 PR）；**E4** untracked human review artifact（可用于审计，不冒充 repository-established）；**E5** roadmap/design intent（#221/#225/#227 定义要证明什么，不是证明）。

promotion law（§28，本次对全 ledger 适用，不跳级）：

```text
HYPOTHESIS → direct experiment exists → SUPPORTED / PARTIAL
           → replicated across required scope / fair baselines → PROVEN — SCOPE-BOUNDED
negative:  HYPOTHESIS → fair falsification → FALSIFIED / NOT SUPPORTED → STOP / narrow
```

architecture intention、test count、formal model 存在性、one benchmark、one app、one host 不能跳级。

---

## EXECUTIVE THESIS LEDGER

| ID | Claim | Scope | Status | Strongest evidence | Biggest gap |
| --- | --- | --- | --- | --- | --- |
| T-B0 | 显式 I/O lifecycle 边界存在且可定位 | async request + wait lifecycle（sluice_async） | PROVEN — SCOPE-BOUNDED（A/E0） | RequestArena 5-stage + submit_transaction + reap-only publication；WaitNode `resolve_` CAS（源码核verified） | 边界存在 ≠ 边界有价值；同步面刻意近裸（#237 DR-11） |
| T-B1a | Request authority 跨 backend 共享 | ThreadPool/Uring/Sync/Fake 四 backend 构造路径 | PROVEN — SCOPE-BOUNDED（A/E0+B） | 四 backend 均经 `detail::submit_transaction`；reap 为唯一 publication（各 backend 源内注释+代码） | application-visible backend neutrality（E2）未测 |
| T-B1b | Wait authority 跨 frontend 共享 | Event/Queue/RwLock/Condition 的 admit/winner/deadline/cancel/ownership | PROVEN — WAIT/REPRESENTATIVE-FRONTEND SCOPE（A+B/E0+E2，MASTER） | PR #243 merged；F3=0、FD4=NONE；M1–M5+C1–C4 全 RED；人审 APPROVE | 第二 frontend 仅为 internal-testing（DIV-16）；concurrent discharge 未证（DIV-17）；Completion/backend 半区未声明 |
| T-B1c | Backend neutrality 在真实 workload 成立 | 真实 app 经多 backend 无语义改写 | NOT YET TESTED（E5） | 仅 architecture reuse（T-B1a） | E2 未执行；无 Uring 真实环境 |
| T-B2 | 边界是"smallest" | 全局最小性 | HYPOTHESIS / DESIGN OBJECTIVE | 局部 semantic compression 有证据（AC-2b 30→3、AC-2c 6→1、FE F3=0） | 全局最小性不可达也无实验；"API 少/LOC 少"≠minimal |
| T-C1 | lifecycle state 可显式控制 | 容量/deadline/取消/durability/completion/身份 | PROVEN — SCOPE-BOUNDED（A+C/E0） | 公共 API 面 + 四 app 实际使用 + 测试执行；容量拒绝 `would_block` 有实测 | 部分项仅 observation-only（AC-1a）；控制"有用性"归 T-C2 |
| T-C2 | 控制启用有价值的执行选择 | thesis-linked specialization | NOT YET TESTED | 无 | RE-3a A/B 不存在；sendfile/splice/registered buffers 均未实现（grep-verified） |
| T-S0 | 机械可检查（开发保证层） | 当前 assurance stack 实际检出能力 | PROVEN — SCOPE-BOUNDED（B/E0+E1+E2） | TLA+ 26 suites、GenMC 2 kernels、21 death-test 文件、8 negative-compile、mutation census（192 门 + M1–M5 + C1–C4 + D2/D4）、TSan/ASan/UBSan、DST | 每层只对被建模/被执行/被变异的 defect 敏感；TLA+ ≠ 实现证明 |
| T-S1 | hazard 已迁移（silent → 检出/不可表示/可复现） | 逐 hazard 配对 | PROVEN — PER-HAZARD SCOPE（A+B+E2） | 具体 hazard→mechanism 对（见 SAFETY S1 表），多个有真实检出史 | 覆盖面 = 已配对项；不构成全 hazard 面 |
| T-S2 | 净安全价值优于 conventional | 全仓 hazard corpus 对比 | NOT YET TESTED | 无 | SE-1/SE-2 未执行；Sluice-induced hazards 已计数但无对比语料 |
| T-P1 | abstraction tax 可测且已分离 | WSL2/tmpfs 单机 E1 ladder cells | PROVEN — SPECIFIC WSL2 CELLS ONLY（D-partial/E1） | schema-2 artifacts（binary-sha256 绑定）；L2−L1 ≈ 1.3–2.0 µs/op @4K d1；two-tax 分解（control ≈5% vs medium ≈95%） | 单机/单环境/异常 cell（+266%）与负 tax cell 并存；不得推广 |
| T-P2 | near-native（一般化） | 多真实 regime | NOT YET TESTED | 无 | RE-0H/RE-1/RE-2 未执行；文档自设边界 "tmpfs numbers must not be generalized"（explicit-io-abstraction-tax.md:140） |
| T-P3 | raw liburing vs Sluice uring 同机会比较 | L3/L4 ladder | NOT YET TESTED | 无 ladder（uring ladder 明确 deferred，:193） | real-mode backend 存在且有本地 mutation 证据，但无性能比较 |
| T-R1 | 资源由显式配置决定而非输入规模 | 1 GiB app configs + v6 overload artifact | SUPPORTED / PARTIAL（C+B/E1） | RSS 8.2/5.2/5.3/4.5 MB @1 GiB 输入 flat；overload refuse p50 40ns、sustained RSS Δ 0 kB | E3 系统化/E4 formal overload 对比未执行；hidden 结构有已知增长面（group.hpp 未修） |
| T-D1 | 显式边界帮助复现 timing bug | DST-PV-1 单案例 | PROVEN — ONE BUG-DISCOVERY CASE（B+E2） | 确定性 replay 命中 Q-LIV-1（确定性 liveness 缺陷，非数据竞争） | 单 seam 维度（next-runnable）；无 DPOR/随机/穷举；#239 全矩阵未完成 |
| T-U1 | 人类可编程 | 四 app 可行性 | PARTIAL（C/E1） | 四 app 仅公共 header 构建并运行 | E7 LOC/摩擦记账未执行；摩擦清单已记录未量化 |
| T-AI1 | AI 编程更正确/更省修复 | controlled E8 | NOT YET TESTED | 无 | E8 未执行；"AI 能维护仓库"不构成 E8 证据 |
| T-SP1 | 显式信息启用安全优化 | thesis-linked capability | NOT YET TESTED | 无（legality 论证已有候选：registered buffers 要求显式 lifetime） | RE-3a 未执行 |

---

## BOUNDARY

### Request（B1）

**Authority 存在（A/E0，当前树核验）：**

- `RequestKey { ContextIdentity, SlotIndex, Generation }` — `include/sluice/async/detail/request_key.hpp:58`
- `RequestSlot` — `detail/request_slot.hpp:122`（friend RequestArena）；`RequestArena` — `detail/request_arena.hpp:131`，五阶段生命周期：Stage 1 reserve(:241) / Stage 2 prepare(:274) / Stage 2.5 publication binding(:306) / Stage 3 commit(:343) / Stage 4 enqueue(:384) / Stage 5 reap(:468)
- `submit_transaction`（pre-accept 阶梯，noexcept）— `detail/submit_transaction.hpp:95`
- `Completion<T>` caller-owned — `include/sluice/async/completion.hpp:115`；`publish_from_reap`（friend AsyncBackend only）— :404
- Reap-only publication：`src/async/threadpool_backend.cpp:555` "reap is the SOLE Completion-ready publication authority"；`src/async/uring_backend.cpp:12` 同句；`RequestArena::reap` :500

**Backend reuse（A/E0）：** ThreadPoolBackend（threadpool_backend.hpp:87）、UringAsyncBackend（uring_backend.hpp:104，双模 SLUICE_HAS_LIBURING）、FakeAsyncBackend（fake_backend.hpp:72，直接调 `detail::submit_transaction` :509/:527）、SyncBackend（sync_backend.hpp:58，:230/:242 调 submit_transaction）—— 四条构造路径共享同一 identity/admission/publication authority。

**分类结论：** architecture authority existence = PROVEN — SCOPE-BOUNDED；ThreadPool/Fake/Sync reuse = PROVEN（代码级）；Uring reuse = PROVEN（real-mode 代码级 + 本地 mutation 证据 E2-local）；application-visible backend neutrality = NOT YET TESTED（E2 未执行）。不把 "backends use common core" 升级为 "backend neutrality proven in real workloads"。

### Wait（B2）

R2 + FE 后的当前 authority map（全部在当前树核验，E0）：

- terminal winner：`WaitNode::resolve_` — `include/sluice/async/wait_node.hpp:350`（CAS `registered → outcome`；头注 "no second winner protocol"）
- deadline：`arm/consume/retire_ordinary_deadline_locked` — scheduler.hpp:2294/2305/2314（AC-2b：原 ~30 处 inline 位点 → 3 helpers；8/10 arming 迁移，2 个 Queue-local 位点因外部可观察顺序**有意保留**）
- cancellation：`cancel_primitive_wait_locked` — scheduler.hpp:1576（AC-2c：6 条 primitive 路径共享 exact-membership + Cancelled winner/unlink + AC-2b retirement；13 条路径盘点中其余刻意分域）
- admission ladders（唯一文本阶梯）：`event_wait_admit_locked` :585、`condition_wait_admit_locked` :870、`queue_push/pop_admit_locked` :1006/:1013、`rwlock_read/write_admit_locked` :1193/:1202
- publication：`publish_wait_winner_locked` :1520 + `deferred_publications_` transit :1559（teardown 非空 → named fail-fast）
- resource reconciliation：Q-LIV-1 opposite-role FIFO reconcile（#242 修复 + 4 regression witnesses）；QueuePort lifetime pin transfer（FE-CORRECTIVE-1 P1-2）
- ownership：`ActorId`（wait_node.hpp:139，kind-tagged equality；owner 判定不检查 delivery token）

**Frontend 共享（MASTER-ESTABLISHED，PR #243 merged 7437c8c）：**

- Fiber frontend（公共 stackful）与 internal stackless frontend 消费**同一** production law；seam TU 无自己的 admission 序列（FE-4 审计 F3=0：无第二语义权威；FD4=NONE）
- 代表性覆盖：Event 全量（admission/inline-no-publication/async publication/cancel/ordinary deadline）、Queue（资源/FIFO/close/cancel/deadline/reconcile/QueuePort lifetime）、RwLock（ActorId ownership）、Condition（admission/notify/deadline choreography；完整 AsyncMutex composition staged）、跨前端混合（一次 set()/unlock_write 同时解析 fiber+deferred winner 集）
- witness 敏感性：M1–M5 变异全 RED（M5 首试暴露覆盖缺口并补 case）；FE-CORRECTIVE-1 三 P1 修复后 C1–C4 纠正轮变异 RED/GREEN 记录于 tracked `docs/verification/fe3-multi-frontend-mutation-evidence.md`
- 布局代价探针实测：WaitNode 48→56 (+8B, DIV-15)、AsyncRwLock 120→128 (+8B, DIV-19)、WaitResume=16B
- 人审：review 5059658754 APPROVE；裁决 A：**ONE SEMANTIC CORE + REPRESENTATIVE SECOND FRONTEND PROVEN + PUBLIC COROUTINE FRONTEND/API DEFERRED**

**诚实边界（staged divergences，全部登记于 docs/architecture/divergence-registry.md）：** DIV-16（第二 frontend 仅 internal-testing，无生产 drain 接线）；DIV-17（deferred-discharge 资格：v1 = arming-thread discharge only；生产级并发 drain 需 symmetric transfer 或 suspend-ack gate，未证）；DIV-18（`mutex_handoff_one_locked` owner commit 仍 Fiber-typed；无 deferred Mutex/Semaphore admission 面）；未发布项：生产 coroutine drain、公共 coroutine API/Task、sender/receiver、Mutex/Semaphore stackless frontend。

**G1-Boundary 关联：** #227 comment 5466026609 已声明——wait/sync 半区有证据 YES；Completion/backend 与 I/O 半区**不声明**。

### Minimality（B3）

- "smallest" 是 design objective（thesis 原文），无全局最小性实验或证明。正确分类 = **HYPOTHESIS / DESIGN OBJECTIVE**。
- 可证的相邻命题是 **semantic compression occurred（局部）**：AC-2b（30 inline 位点→3 helpers + 192 变异门）、AC-2c（6 路径→1 closure）、FE（F3=0 重复权威清零、delivery 差异收敛到恰好 5 个 kind-switch 位点）、#237 two-tax（control-plane 机器只占 coordination 成本 ~5%）。
- 但 compression ≠ 全局最小。"最少 semantic owners" 的反例面（Mutex/Semaphore stackless 面缺失、R2 关闭时明言 "no further centralization EARNED" 是证据边界而非最小性证明）未系统枚举。

---

## CONTROL

lifecycle facts 盘点（每项：可见性/可检性/可控性/边界性质）：

| Fact | User-visible | Core-visible | Backend-visible | Inspectable | Controllable | 边界性质 |
| --- | --- | --- | --- | --- | --- | --- |
| operation identity（RequestKey/RequestHandle） | 部分（RequestHandle） | YES | YES | YES（death tests/negative compile） | 提交/取消经 handle | hard（generation 强制） |
| ownership（caller-owned Completion、borrow） | YES | YES | YES（borrow 契约） | YES | reset/reuse 由 caller | hard（lifetime 契约） |
| generation | NO（内部） | YES | YES | YES（测试） | NO（自动） | hard |
| completion | YES（Completion<T>） | YES | 仅 publish 权 | YES | 读取/reset 时机由 caller | hard |
| cancellation | YES（CancelToken/cancel API） | YES | intent 记录层 | YES | YES（typed outcome） | hard（layered，AC-9） |
| deadline | YES（typed timeout outcome） | YES | YES（arming） | YES | YES | hard（outcome）/mechanism（timer） |
| resource capacity（arena/queue/worker/ring/pipeline） | YES（配置） | YES | YES | YES（AC-1a accessors） | YES（拒绝 = `would_block`，实测） | hard |
| queue occupancy | AC-1a 观察 | YES | 部分 | YES（4 accessors） | NO（observation only） | observation only（RX-1 后冻结扩张） |
| ActorIdentity | NO（内部；ActorId 内部 token） | YES | NO | YES（测试） | NO | hard（G-serialized） |
| durability intent | YES（sync_data/sync_all 分立） | YES | n/a | YES | YES | hard（契约） |
| capability differences | 声明面存在（CopyDecision deferred 标签） | YES | YES | YES | 部分（unsupported_policy） | 声明式；实现面缺（sendfile/splice/registered 均未实现） |

分离结论：

- **A. explicit control EXISTS** — PROVEN — SCOPE-BOUNDED（上表 hard 行均有公共面 + 测试/应用执行证据）。
- **B. control has USER VALUE** — PARTIAL：容量配置→四 app 内存 flat（C 层）；deadline/cancel 有 typed value 但无与 conventional 的 controlled 对比。
- **C. control ENABLES SPECIALIZATION** — NOT YET TESTED（归 T-C2/T-SP1）。

A 成立不自动推出 B/C。

---

## SAFETY

### S0 Development assurance

实际在用且有过真实检出史的机制：

| 机制 | 检测什么 | 不检测什么 | 真实检出？ |
| --- | --- | --- | --- |
| TLA+（26 suites，`spec/tla/manifest.json`，`scripts/formal/verify.py`） | 抽象协议性质：admission、terminal winner、wake、generation、shutdown、公平 | 不证明 C++ 实现（#227 明示窄角色）；状态空间有界 | 是（历史，master 记录）：E9 retire/participant epilogue 缺口 → C++ 修复（PR #190/#194）；SplitWait vacuity（#195） |
| GenMC（`spec/weakmem/completion-publication/` 两 kernel） | 有界弱内存 publication/reset-reuse 序 | 非 whole-program 结论 | 是：#197 TSO 发现（PR #203） |
| ASan+UBSan（change-class gate） | 执行路径内存/UB | 未执行路径 | 是：app stack-use-after-scope（已修，file-tools findings）；FE 全套件 clean |
| TSan（change-class gate） | 真实线程数据竞争（被执行 schedule） | 未触达 race；不证 no-race | 是：#229 seam race（test-only）；FE P1-3 race witness；1 次一次性 hang |
| Death tests（21 个文件，Debug+Release 生效，exit-code pinned） | named fail-fast 边界触发 | 非 fail-fast 类缺陷 | 是（FE PUB1/QD1/RW 等） |
| Negative compile（8 个 verify 脚本） | 公共 API 误用类编译期拒绝 | 运行时语义 | 结构性（ witnessing via compile rejection） |
| Mutation（AC-2b 192 门 census；FE M1–M5 + C1–C4；D2/D4 uring real-mode） | 当前测试对具体 law 破坏的敏感性 | 只对被变异位点；不证全类 | 是（全 RED；M5 暴露过覆盖缺口后补） |
| DST（DST-PV-1：next-runnable choice seam + 脚本 replay，≤64 steps/≤8 fibers） | schedule 依赖的确定性缺陷 | 无 DPOR/随机/穷举；multi-worker 结构性不激活；弱内存 | **是：Q-LIV-1（生产 liveness 缺陷）** |
| Deterministic seams（31 phase seams + FakeAsyncBackend + logical clock） | race-window 因果测试 | 不替代真实并发证据 | 是（多个 causal tests） |
| Trace/refinement（e9-trace-conformance，pinned prefix replay） | 真实 C++ trace 与模型一致 | 覆盖 trace 前缀内性质 | 是（试点） |

**结论：T-S0 = PROVEN — SCOPE-BOUNDED。** 每层的"不检测什么"如实列出；TLA+ 成功 ≠ 实现证明 ≠ 产品安全证明。

### S1 Prevention / early detection（hazard → mechanism 配对）

| Hazard | Mechanism | 证据 |
| --- | --- | --- |
| ABA / stale identity | RequestKey generation（slot reuse 先 bump generation） | request-arena TLA+ suite；request-arena death test；negative-compile 脚本 |
| double terminal | `WaitNode::resolve_` 单点 CAS | e10-waitnode 模型；wait/resolve death tests；FE M-变异对 winner law 全 RED |
| completion-after-reuse | Completion reset/reuse generation | GenMC `kernel_reset_reuse`；completion_authority_death_test |
| lost wake | wake epoch / persistent predicate 协议（e9-park-wake、spawn-wake-epoch 模型） | 历史：模型暴露 C++ epilogue 缺口 → PR #190/#194 修复 |
| 分配进入 terminal/admission 关键路径 | prepare-before-register（强保证）+ noexcept publish tail + 失败 fail-fast | R2-ALLOC 4 witnesses；FE PUB1/QD1 death tests |
| wrong owner | ActorId（kind-tagged）+ Release 生效的 named fail-fast；owner 检查在 G 内 | FE P1-3 修复 + RW death child + turnover stress（TSan） |
| destruction while live | quiescent destruction 契约 + begin_teardown 门 + stranded-transit fail-fast（Debug+Release） | scheduler teardown 死亡测试组 |
| queue liveness drift | Q-LIV-1 opposite-role reconcile | DST replay witness + #242 regression witnesses |
| public misuse | negative-compile 脚本族 | 8 个 verify-*-negative-compile.sh |
| weak-memory publication | record.arm() acq_rel + GenMC kernels | kernel_publication 检查通过记录 |

**结论：T-S1 = PROVEN — PER-HAZARD SCOPE。** 不从 API 名字猜保护；每行有当前证据。不构成全 hazard 面（那是 S2/SE-1）。

### S2 Empirical net safety value

- conventional-vs-Sluice hazard corpus：**不存在**（SE-1 未执行）。
- 检测矩阵（SE-2）：**不存在**（现有 mutation/death/DST 证据是局部前身，但无统一矩阵）。
- Sluice-induced hazards 已单独计数（见下节），但没有与"被消除的 conventional hazards"的同口径统计。
- **结论：T-S2 = NOT YET TESTED。** 不能因为 S0 强就说 "Sluice is safer"。

---

## SLUICE-INDUCED HAZARDS

审计窗口内（#227 campaign 起，master 可追溯）Sluice 自身语义机器制造/暴露的 hazard 样本：

| # | Hazard | 层 | 归因 | 谁抓住 | 检出前是否 silent | 修复后迁移到 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Q-LIV-1 Queue liveness drift（blocking inline-success 不 reconcile 对侧 FIFO head → 停等者永久搁浅） | production | wait/queue 语义机器 induced | DST-PV-1 确定性 replay（dst_t5_v1 witness，#241） | 是（特定 interleave 下挂起，非数据竞争，G+S 全串行） | deterministically reproducible + fixed + 4 regression witnesses |
| 2 | R2-ALLOC：timed admission register-before-arm，`bad_alloc` 遗留 Registered node + live accounting | production | deadline/wait registration lifecycle induced | 人类对抗评审 round 2（#242） | 是（仅 OOM 时显形） | strong-guarantee prepare-before-register + witnesses |
| 3 | deadline heap O(N²) 增长（round-2 修复自身引入：`reserve(size+1)` 破坏 geometric growth） | production | 修复过程的次生 hazard | 人类对抗评审 round 3（capacity witness 在 round-2 代码上 RED） | 否（性能/扩展性类） | growth-correct reserve + witness od_alloc_a3 |
| 4 | 同类缺陷：`include/sluice/async/group.hpp` `reserve(size()+1)` idiom | production | 同 #3 类 | #242 round 3 顺带发现 | — | **未修（open，已登记 flag）** |
| 5 | FE P1-1：`defer_publication_locked` MAY-THROW（terminal 提交后插入分配可抛） | production | FE deferred-publication transit induced | FE-CORRECTIVE-1 对抗评审 | 是（分配失败才显形） | noexcept + named fail-fast exit 86（Debug+Release）+ PUB1/PUBCTL |
| 6 | FE P1-2：QueuePort lifetime hole（transit 不持 pin，begin_teardown 可穿过 in-flight 窗口） | production | FE deferred Queue ops induced | FE-CORRECTIVE-1 对抗评审 | 是 | pin transfer to awaiting frame + QPIN-1/2 + QD1 |
| 7 | FE P1-3：rwlock recursive-owner 检查在 G 外 → 数据竞争 | production | FE-3 重构 induced | FE 对抗评审 + TSan witness + M3 变异 | 是（竞争窗口） | 检查移入 ladder（G 内）+ death child + stress |
| 8 | P2：`WaitResume::fiber(nullptr)` 非法 token | production（seam 面） | FE token widening induced | FE 评审 | 是 | 归一化 Kind::none + static_assert（M4 编译拒绝） |
| 9 | test-seam race（#229） | test-only | 测试 seam induced | TSan | 是 | 修复 |
| 10 | select_event_registry_test TSan 一次性 hang（no-progress termination vs plain-wait residency 类） | test-only | 测试类 | TSan（20/20 复跑清白） | 是 | 记录于 gate 文档，待修 |
| 11 | `active_deadline_count_` ~30 处 inline 变异（authority duplication，drift-prone） | production（结构性） | 语义机器 induced | #234 六域审计 → AC-2b | 结构性风险非单点 bug | 3 helpers + 192 变异门（2 个 Queue-local 有意保留） |
| 12 | R2 authority duplication / AsyncQueue drift（AC-2 家族起源） | production（结构性） | 同上 | #234 六域审计 | 同上 | AC-2b/2c/FE 收敛；R2 关闭 |
| 13 | RX-1 实验 18 个 CONTROL run 物理无效（workers 饱和） | process | 实验设计 | 预注册有效性门自检 | 是（会被计为假样本） | 诚实排除，从不入分母 |

**COUNT：13 项检视；production 类 7 项（#1–3、#5–8，其中 6 项已修 + 1 项 open 兄弟缺陷 #4）；结构性重复 2 项（#11–12，已收敛）；test-only 2 项（#9–10）；process 1 项（#13）。**

**WHAT CAUGHT THEM：** 确定性 DST replay（1）；人类对抗评审（2,3,5,6,7,8）；TSan（7 佐证、9）；预注册有效性门（13）；六域审计（11,12）。**没有任何一项由外部用户发现（项目无外部用户，v0.0.1 tag-only）——这既证明 S0 层在工作，也如实说明检出全部发生在开发侧。**

**对 net-safety claim 的含义：** 显式边界在其自身构造期确实制造了可测的新协议 hazard 流（窗口内 ≥6 项 production 类）。canonical thesis 的反命题——"更多协议义务 → 更多 bug surface"——**是活的，未被反驳也未被证实**。这正是 SE-1/SE-2 必须把 Sluice-induced mutant 半区计入的原因；在 SE-1/SE-2 执行前，T-S2 必须保持 NOT YET TESTED。

（历史补充，早于 #227 基线但 master 记录：E9 formal→C++ epilogue 缺口修复 PR #190/#194、SplitWait vacuity #195、GenMC TSO #203 —— 形式层对 C++ 实现的真实检出史。）

---

## PERFORMANCE

### Existing ladder（P1：WSL2 / E1）

证据：`docs/results/performance-attribution/e1-round1-{read,write}-{smoke,representative}.json`（schema 2、kind e1tax、binary sha256 绑定、git.sha=9404df8、branch `research/issue-221-e1-abstraction-tax-baseline`、dirty:false）；正式表 `docs/verification/explicit-io-abstraction-tax.md:222-249`；校验门禁 `scripts/bench/perf-evidence-validate.py`（pre-push + CI）。

环境：WSL2（kernel 6.18.33.2）、Ryzen 7 5800H、8 逻辑 CPU、clang 21.1.8 Release、glibc 2.43、tmpfs `/tmp`、1 GiB/cell、warm cache、reps 21 median、workers=4。尺寸 4K/64K/1M，深度 representative 1/8/32。

Representative 中位值（µs/op）：

| op | size | depth | L0 | L1 | L2 | L1−L0 | L2−L1 | (L2−L1)/L1 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| read | 4K | 1 | 0.862 | 33.47 | 35.47 | 32.6 | 2.00 | 6.0% |
| read | 64K | 8 | 3.34 | 11.13 | 12.74 | 7.79 | 1.62 | 14.5% |
| read | 1M | 32 | 125.7 | 226.2 | 240.2 | 100.4 | 14.0 | 6.2% |
| write | 4K | 1 | 1.62 | 36.99 | 38.31 | 35.4 | 1.31 | 3.5% |
| write | 64K | 8 | 13.15 | 12.95 | 13.02 | −0.21 | +0.07 | 0.6% |
| write | 1M | 32 | 246.4 | 303.2 | 309.1 | 57.2 | 5.9 | 2.0% |

反例/异常（必须与好数字并排呈现）：

- **write 4K d32 w4：L2−L1 = +1.73 s（+266%）** —— control-plane tax 存在显著非小 cell；
- smoke 中存在负 tax cell（read 4K d64 w1：L2−L1 = −28.7 ms）——reap batching 等效应；
- write 64K d8：L1−L0 ≈ 0（负）——medium tax 不是普适常数。

诊断佐证（e1-diagnostics/，4K d1 read，262144 ops）：bpftrace sched_switch L1=1,076,083 vs L2=1,077,188（≈4.1 次/op，L2 仅 +0.4%），pread64 次数几乎相同 —— L1−L0 是 wake/handoff（medium）现象，非多余 syscall。

#237 two-tax 定理（基于同批证据）：4K d1 ~35 µs/op coordination 中，control plane（全部 Sluice 请求机器）≈1.3–2 µs（~5%），medium（线程 offload 往返，L1−L0）≈33–35 µs（~95%）。

**claim 形状（正确）：** "Sluice incremental control-plane tax 在这些 measured cells 内为 ~1.3–2 µs/op 量级（个别 cell 显著更大/更小）" **仅限该 host/环境/形状**。错误形状（禁止）："Sluice overhead is always ~5%"。E1 文档自设边界（:140 "tmpfs numbers must not be generalized to real SSD performance"；:193 "The uring ladder is deferred"；:198-207 cannot-claim 列表）。

注意：E1 artifacts 测于旧 commit（9404df8），但以 provenance 绑定形式 tracked 于 master —— 证据等级 E1，测点非当前 HEAD。

### Applications（P2）

`docs/applications/file-tools-findings.md`（Release、1 GiB、tmpfs、同 host）+ #127 归属：

| app | vs native | 归属 |
| --- | --- | --- |
| sluice-copy | 1.1–1.3× cp（含 atomic-output） | 达标；core coordination ≈30% wall（唯一真实 core 侧信号，PF-002，未分解，不授权改动） |
| sluice-hash | 1.5×（250 vs 378 MB/s） | APP（可移植 SHA-256 无 SHA-NI；70.3G vs 21.6G instr） |
| sluice-grep | V2 收复 sparse 2.4–3.1×；剩余差距算法类（kwset skip loop） | APP |
| sluice-tail | `-n 10` 0.86×（更快）；large-N 6.8×（app 保留算法） | APP |

**纪律：** grep matcher 改进不称为 Sluice runtime win；copy 的 30% coordination 是 open core 观察项，未分解、未授权优化。#127 结论"每个大 gap 都是 algorithm/APP 层"维持。

### Native Linux/NVMe（P3a）

**RE-0H：NOT DONE。RE-1（native L0/L1/L2）：NOT DONE。** docs/ 无任何 native/NVMe artifact；ARCH-FUNDAMENTAL-POSTRX1-REVIEW-AUTHOR-REPORT.md:438 明确 RE-1 要求 "≥1 native-Linux host + cold-cache HDD/NVMe cell"（尚未做）。因此 **near-native = NOT YET TESTED**（缺 native/raw-liburing matched evidence 时不得使用）。

### io_uring（P3b）

- UringAsyncBackend 为**双模真实实现**：`SLUICE_HAS_LIBURING` 定义时 real io_uring（~1300+ 行，liburing pinned 2.14），否则 UNSUPPORTED STUB（`uring_backend.cpp:12-17`）；`xmake/experimental.lua:13` `with-liburing` 选项默认 off，检测失败 fail loudly。
- real-mode 证据：D2/D4 mutation 证据（`docs/verification/phase-d2-uring-failure-noalloc-mutation-evidence.md`、`phase-d4-uring-wait-close-drain-mutation-evidence.md`，liburing 2.14 real mode，WSL2，本地执行）；stub/off 对照记录在案。
- CI 不跑 real liburing（workflow 无 liburing 字样；README:157 "Real liburing validation = Environment-dependent"）。
- **RE-1U（raw liburing L3 vs Sluice L4）：NOT DONE** —— runbook 要求的 `docs/results/liburing-validation-*.md` 不存在；无任何 raw-vs-Sluice 性能 ladder。
- **T-P3 = NOT YET TESTED。** stub/off 证据不得冒充 real-liburing 证据（AGENTS §12.2）；real-mode mutation 证据是 B 层（验证事实），不是 D 层（性能结论）。

### Envelope（P3c）

**RE-2（Z1–Z4 envelope）：NOT DONE。** Z1–Z4 仅是 #237/#227 中的预注册定义。无 multi-objective map。

---

## RESOURCE / BOUNDEDNESS

现有证据：

- 1 GiB app RSS（file-tools-findings.md:145-153）：copy 8.2 MB（1 MiB×depth 4）、hash 5.2 MB、grep 5.3 MB、tail 4.5 MB；"RSS is flat across 1 GiB inputs"（:154-155）；valgrind 0 leaks + ASan/UBSan clean。
- v6 overload/backpressure artifact（`docs/results/performance-attribution/v6-overload-backpressure.json`，schema 2，git ba01758）：capacities {16,64,256}×400 rounds —— refuse p50 40 ns vs accept p50 70 ns（1.75×），drain 1.7/9.9/101 µs；2000 轮 sustained RSS Δ = 0 kB。文档 `docs/verification/guarantee-cost.md:60-115`。

可支持的 claim：**"resource boundedness appears independent of input size, for these applications/configurations"（SUPPORTED — EXISTING APP CONFIGURATIONS）** + 容量拒绝路径有 measured cost vector。

不能自动支持：overload boundedness 全称、所有 hidden runtime 结构有界、所有 backend queue 有界、饱和行为稳定。

诚实增长面清单（当前树）：`deferred_publications_` transit（以并发 suspended deferred waiter 数为界，per-discharge 排空——现仅 test-only frontend）、`timer_pool_`/`deadline_heap_`（以并发 deadline 数为界；#242 后 growth-correct）、dispatch ring（以 request capacity 为界）、**`group.hpp` reserve idiom（open 缺陷，#4 号 hazard）**。

**E3（系统化 boundedness）/E4（formal overload 对比）：NOT EXECUTED**（#237 状态表 :371 明确 "E4 formal comparison absent"）。overall boundedness = **SUPPORTED / PARTIAL**。

---

## DETERMINISM / REPLAY

三问分离（§14）：

- **A. effects can be controlled?** YES —— FakeAsyncBackend（completion/fault 驱动）、logical clock `advance_clock`、31 个 phase seams、`cancel_wait`，全部既有。
- **B. schedules can be controlled?** PARTIAL —— DST-PV-1（PR #241 merged）补上唯一缺失维度：next-runnable choice（`Run(X)` + 既有 CompleteIo/AdvanceClock/Cancel；≤64 steps/≤8 fibers/≤8 actions；非法 decision fail-closed 无 fallback）。无随机 seed、无 DPOR、无穷举调度器、无通用框架（stop gate 有效）。multi-worker 结构性不激活（保持 FIFO，避免伪造）。
- **C. caught meaningful bugs?** YES —— **一个**：Q-LIV-1（生产确定性 liveness 缺陷；TSan/stress 原则上难命中，因无数据竞争；确定性 replay 一步复现并成为 #242 的 characterization witness）。

正确分类：**DETERMINISTIC VALUE — PROVEN IN ONE BUG-DISCOVERY CASE**。不升级为 "DST solves concurrency verification"。verdict 链：#239（Determinism Boundary Audit，OPEN，全矩阵未完成）→ DST-PV-1 窄 PoV → **MINIMAL DETERMINISTIC DRIVER JUSTIFIED** → PR #241。

---

## SPECIALIZATION

区分两类（G1-Control）：

- **A. FASTER SYSCALL：** sendfile/splice 存在 deferred 声明名（`sendfile_deferred`/`splice_deferred`，unsupported_policy 处理），**未实现**（#237 grep-verified：无 read_fixed/register_buffers/sendfile/splice/multishot）。无收益可声称。
- **B. THESIS-LINKED SPECIALIZATION：** 无 RE-3a A/B。候选合法性论证已成立：liburing 注册缓冲语义 = "All buffers must remain valid until unregistered / buffers must not be in use when unregistered"（context7 /axboe/liburing registration-api，本次审计核验）—— 即**显式 lifetime/资源信息是该类优化的合法性前提，不是附带元数据**。这正是 Sluice borrow/lifetime 契约与 RE-3a 的连接点，但连接点存在 ≠ 实验完成。
- 在树中真实存在的特化：sync 平面 `BufferedReadable` copy fast path（#237 税表中记为 negative-cost 行）—— 真实但未经 thesis-linked A/B（无"同一优化机会给 baseline"的对照），归 product capability 而非已证 specialization。
- #237 Q17D 裁决维持：capability-local specialization 是最高 evidence-per-effort 性能杠杆，**全部未实现**。

**G1-Control = NOT READY（NOT YET TESTED）。**

---

## PROGRAMMABILITY

### Human

- **可行性：** 四个 app（apps/sluice-{copy,hash,grep,tail}，均含 main.cpp，仅公共 header）可以构建并完成真实工作 —— PROVEN — SCOPE-BOUNDED（C 层）。
- **简单性：** E7 LOC/摩擦记账未执行 → PARTIAL。已记录的摩擦清单（#221）：RuntimeTaskContext 无原生 timer/readiness；tail follow 用 sliced sleep_for 占 worker；无 signal→cancellation 桥；typed task results 需 app 自备 slot+mutex/CV；fd stat/size 在 I/O 模型外；run-to-completion plumbing 跨 app 重复。摩擦观察 ≠ 简单性度量。
- 公共面事实：RuntimeBuilder/ApplicationRuntime/RuntimeTaskContext（application_runtime.hpp:56/163/202；submit_read/write、submit_*_request、await_completion）。

### AI

- controlled E8（多独立尝试、等价任务、POSIX/Asio/liburing/Sluice 基线、修复次数等指标）：**未执行**。#237 状态表原文 "AI-programmability advantage | HYPOTHESIS (never tested) | E8 unexecuted"。
- "AI agent 能维护本仓库/AGENTS.md 写得细" **不构成** E8 证据（prompt §15 明示）。**T-AI1 = NOT YET TESTED。**

---

## NEGATIVE / FALSIFIED EVIDENCE

| # | 项 | 分类 | 实验与结果 | 架构后果 |
| --- | --- | --- | --- | --- |
| 1 | **RX-1**（深度可观测性/归因扩张） | **FALSIFIED / NOT SUPPORTED**（唯一正式 fair falsification） | 预注册 H_RX1：288 正式运行、222 有效样本；C（纯外部）99.55% vs E（+AC-1a）98.20%，Δ=−1.35pp [−3.60,+0.45]；探索性天花板 +0.45pp（距 +15pp 门一个数量级） | **STOP EXPANSION**：无 L2 时戳/RequestKey 遥测/eBPF/RX-2/RX-3/autotuner，除非实质不同的预注册假设；AC-1a 保留为工程指标；教训：`frac_active` 是 duty-cycle 代理非饱和谓词（PR #236 merged e10e181；#227 存档） |
| 2 | AC-2d-b（更深 wait 集中化） | **NOT EARNED** | #242 三轮对抗评审后无 S3 authority duplication 残留；Stage 3 按入场条件跳过 | R2 COMPLETE；中央化停止，除非新 hazard/cost 证据 |
| 3 | 广义 DST 框架 | **NOT EARNED** | DST-PV-1 证明的只是最小 driver（单 seam 维度 + replay）；#239 全矩阵未完成 | 只保留 minimal seam；无 DPOR/随机/穷举 |
| 4 | Runtime policy / planner / autotuner | **UNAUTHORIZED** | #227 P3 禁止；#237：文献（NSDI'22 等）+RX-1 均不支持 per-op 自适应 | RE-5 才可能重开 |
| 5 | 生产 coroutine frontend / 公共 coroutine API | **DEFERRED（staged，非 falsified）** | FE 裁决 A：代表第二 frontend 已证；生产 drain 并发 discharge（DIV-17）、Mutex/Semaphore 面（DIV-18）未证 | 后续 slice 需各自证据 |
| 6 | mega BackendAdapter / 通用 capability framework | **UNAUTHORIZED** | #227 硬门 5；无多真实案例 | — |
| 7 | "uniformly small control-plane tax" | **反例在案** | E1 write 4K d32 cell L2−L1 = +266% | tax claim 必须逐 cell 限定 |
| 8 | 观测税"可忽略" | 未建立 | OBS-LOW +1.4–2.1%，n=8 下与噪声不可分、非单调 | 只能写"未建立可复现信号" |

区分纪律：FALSIFIED（实验反驳）仅 RX-1 一项；NOT EARNED（证据不足以授权）2/3；UNAUTHORIZED（路线图禁止）4/6；DEFERRED（staged 待证据）5。

---

## MASTER VS ACTIVE-BRANCH EVIDENCE

- PR #243 **MERGED**（merge commit 7437c8c = origin/master = HEAD；ancestry 机械验证）。**本次审计不存在 ACTIVE-BRANCH 层：所有 FE 证据为 MASTER-ESTABLISHED。**
- FE 证据链分层：tracked 代码/测试/文档（E0/E1：ladder 源码、fe3-multi-frontend-mutation-evidence.md、fe2 gate、FE-1A/1B/1C、divergence-registry DIV-15..19）+ merged PR/人审记录（E2：PR #243 body、review 5059658754、#227 comment 5466026609，在 GitHub 不在 git 树）+ **E4 untracked**（FE-FINAL 与 R2 两份 closeout 报告，人类持有；本审计引用其结论时已用 tracked 链交叉核验）。
- E1 ladder artifacts 测于旧 commit 9404df8（provenance 绑定、tracked）——E1 级，非当前 HEAD 重测。
- 仓库无 master-push CI（by design）；master 树的 CI 等价性来自 "merge 树 == 已过 CI 的 19c1bde 树（diff 为空）"。

---

## FULL CLAIM LEDGER

| ID | Canonical claim | Dimension | Exact scope | Status | MASTER/ACTIVE | Direct evidence | 证据类型 | Counterevidence | Limitation | 什么会证伪它 | Promotion gate | Next experiment | Core change authorized now? |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T-B0 | 存在明确可定位的 I/O lifecycle 语义边界 | Boundary | sluice_async request+wait lifecycle | PROVEN — SCOPE-BOUNDED | MASTER | RequestArena 五阶段/submit_transaction/reap-only publication/resolve_ CAS（文件:行号见 BOUNDARY 节） | A+E0 | 同步面刻意近裸（不同边界，不反证） | 只覆盖 async 生命周期；同步面是另一条更薄的路径 | 找到第二 terminal/publication 权威或 bypass 路径 | —（已达标） | — | NO |
| T-B1a | Request authority 跨 backend 共享 | Boundary | 4 backends 构造路径 | PROVEN — SCOPE-BOUNDED | MASTER | 四 backend 均经 submit_transaction；reap 唯一 publication | A+E0 | 无 | 代码级 reuse；未含真实 workload 切换 | 某 backend 建立私有 admission/publication 路径 | — | — | NO |
| T-B1b | Wait authority 跨 frontend 共享 | Boundary | Event/Queue/RwLock/Condition 全 winner/deadline/cancel/ownership 律 | PROVEN — WAIT/REPRESENTATIVE-FRONTEND SCOPE | MASTER | PR #243 merged；F3=0/FD4=NONE；M1–M5+C1–C4 全 RED；+8B×2 探针实测；人审 APPROVE | A+B+E0+E2 | DIV-16/17/18 staged 边界 | 第二 frontend 仅 internal-testing；Condition 裸队列呈现；无生产 drain | stackless 前端需要第二 admission 序列才能工作（即 F3>0 类发现） | —（已达标） | — | NO |
| T-B1c | 真实 workload 的 backend neutrality | Boundary | app 级 E2 | NOT YET TESTED | MASTER | 无 | — | 无（也无支持） | 完全未测 | E2 跑出语义改写/分支泄漏 | RE-1U 环境先建立 | E2 app×backend 交换 | NO |
| T-B2 | "smallest" 边界 | Boundary | 全局最小性 | HYPOTHESIS（design objective） | MASTER | 局部 compression（AC-2b/2c/FE/#237 two-tax） | A+E1 | 无最小性下界论证 | compression≠全局最小；不可达声明 | 证明任何更小边界仍满足同律集（理论/实验） | 需最小性论证框架（暂无计划） | — | NO |
| T-C1 | lifecycle state 可显式控制 | Control | 容量/deadline/cancel/durability/completion 等 hard 行 | PROVEN — SCOPE-BOUNDED | MASTER | 公共 API + 四 app 使用 + 容量拒绝实测（v6 refuse 40ns） | A+C+E0/E1 | observation-only 行（occupancy）单独标注 | 部分项仅观察；控制价值归 C2 | 公共面某 hard 项实际不可控/不可见 | — | — | NO |
| T-C2 | 控制启用有价值执行选择 | Control | thesis-linked specialization | NOT YET TESTED | MASTER | 无 | — | BufferedReadable 是未对照特化 | 无 A/B | RE-3a 显示收益不依赖显式信息 | RE-3a 协议 | RE-3a registered-buffer A/B | NO |
| T-S0 | 机械可检查（开发保证） | Safety | 当前 stack 的实际检出能力 | PROVEN — SCOPE-BOUNDED | MASTER | 26 TLA+ suites、GenMC×2、21 death 文件、8 negative-compile、192 门 mutation、M1–M5/C1–C4、D2/D4、DST | B+E0/E1/E2 | RX-1 18 个无效 run（过程性反例，被门拦下） | 每层仅对建模/执行/变异对象敏感 | 某核心 law 的变异全套件 GREEN | — | SE-2 矩阵把它系统化 | NO |
| T-S1 | hazard 已迁移 | Safety | 逐 hazard 配对（S1 表 10 行） | PROVEN — PER-HAZARD SCOPE | MASTER | 每行有模型/测试/死亡/变异证据；多个有真实检出史 | A+B+E2 | 覆盖面有限 | 非全 hazard 面 | 某"已迁移" hazard 找到 silent 复现路径 | — | SE-1 corpus 扩面 | NO |
| T-S2 | 净安全价值优于 conventional | Safety | 全仓 corpus 对比 | NOT YET TESTED | MASTER | 无（SE-1/SE-2 缺） | — | Sluice-induced 6 项 production hazard 在案 | 双向都未计量 | SE-1/SE-2 结果 | SE-1+SE-2 执行 | SE-1/SE-2 | NO |
| T-P1 | tax 可测且两税分离 | Performance | WSL2/tmpfs E1 cells | PROVEN — SPECIFIC WSL2 CELLS ONLY | MASTER | schema-2 artifacts + two-tax 分解 + bpftrace 归因 | D-partial+E1 | write 4K d32 +266%；负 tax cell；单机 | 测点为旧 commit；tmpfs/cache-hot；不得推广 | 原样重跑出现不同量级且 provenance 失效 | 同环境复现 + native 扩展 | RE-1 native | NO |
| T-P2 | near-native（一般化） | Performance | 多真实 regime | NOT YET TESTED | MASTER | 无 | — | 文档自设 "must not generalize" | 完全未测 | 任何 native 结果 | RE-0H→RE-1→RE-2 | RE-0H/RE-1 | NO |
| T-P3 | raw liburing vs Sluice uring 同机会 | Performance | L3/L4 matched | NOT YET TESTED | MASTER | 无 ladder；real-mode 实现存在 + 本地 mutation 证据 | — | uring ladder 明确 deferred | 性能面完全未测 | RE-1U 结果（任向） | RE-1U 执行 | RE-1U | NO |
| T-R1 | 资源由配置决定 | Resource | 1 GiB app configs + v6 artifact | SUPPORTED / PARTIAL | MASTER | RSS flat 表 + overload cost vector + RSS Δ 0 kB | C+B+E1 | group.hpp open 缺陷；E3/E4 缺 | 非全称；非饱和全貌 | 找到随 offered load 增长的隐藏结构 | E3/E4 执行 | E3（复用 harness） | NO |
| T-D1 | 显式边界助复现 timing bug | Determinism | DST-PV-1 单案例 | PROVEN — ONE BUG-DISCOVERY CASE | MASTER | dst_t5_v1 witness → Q-LIV-1 确定性复现 + 修复 | B+E2 | 单维度、无双案例 | 无 DPOR/穷举；multi-worker 不覆盖 | 第二个 DST 命中缺失（价值上限受限） | 更多案例需各自证明 | #239 全矩阵收尾 | NO |
| T-U1 | 人类可编程 | Programmability | 四 app 可行性 | PARTIAL（可行性 PROVEN；简单性未量） | MASTER | 四 app 公共 header 构建 | C+E1 | 摩擦清单 6 项 | E7 未执行 | — | E7 记账 | E7 随下一 workload | NO |
| T-AI1 | AI 编程更正确 | Programmability | controlled E8 | NOT YET TESTED | MASTER | 无 | — | — | 完全未测 | E8 结果（任向） | E8 协议冻结 | E8 | NO |
| T-SP1 | 显式信息启用安全优化 | Specialization | thesis-linked capability | NOT YET TESTED | MASTER | legality 论证候选（liburing 注册缓冲 = 显式 lifetime 前提；context7 核验） | — | 全部 capability 未实现 | 无 A/B | RE-3a 结果（任向） | RE-1U 先行 | RE-3a | NO |

---

## G1 READINESS

按 #227 四门（allowed：READY / PARTIAL / NOT READY / FAIL）：

| Gate | 问题 | Current verdict | Evidence | Missing evidence |
| --- | --- | --- | --- | --- |
| G1-Boundary | 多 frontend 表示能否共享一个 semantic lifecycle authority? | **PARTIAL** | wait/sync 半区：PR #243 merged 证据链（F3=0/FD4=NONE + mutation 敏感性 + 人审 APPROVE）| Completion/backend 半区与 I/O 半区未声明（#227 comment 5466026609 明示）；生产 drain 未接线 |
| G1-Safety | 显式边界是否在计入 Sluice-induced hazards 后实质迁移 hazards? | **NOT READY** | S0/S1 强（多机制真实检出史）；induced hazard 样本已计数（6 production） | SE-1 corpus + SE-2 检测矩阵不存在；净对比未计量 |
| G1-Performance | native Linux/NVMe/io_uring 上 near-native / Z2 envelope 内? | **NOT READY** | WSL2 E1 ladder + two-tax（单机） | RE-0H/RE-1/RE-1U/RE-2 全部未执行 |
| G1-Control | 至少一个由显式语义/资源/lifetime 真正启用的优化收益? | **NOT READY** | legality 论证候选存在（注册缓冲） | RE-3a A/B 未执行；capability 全部未实现 |

**FULL G1：NOT READY。** 不因任何单子实验好看而宣布 PASS。

---

## CLAIMS WE MUST NOT MAKE YET

| # | Claim | Why not | Missing experiment | Promotion gate |
| --- | --- | --- | --- | --- |
| 1 | "Sluice is near-native" | 无 native/NVMe/冷缓存任何测量；文档明令不得从 tmpfs 推广 | RE-0H→RE-1→RE-2 | native matched ladder 多 cell 稳定 |
| 2 | "Sluice is safer than conventional async I/O" | S2 未测；SE-1/SE-2 缺；induced hazards 未对比计量 | SE-1+SE-2 | corpus 级净迁移统计 |
| 3 | "zero-overhead / zero-cost abstractions" | E1 显示 control-plane tax 真实存在（个别 cell +266%） | — | —（该说法与现有证据直接冲突） |
| 4 | "Sluice supports a production coroutine frontend" | FE 裁决 A：仅 representative test-only 前端；DIV-16/17/18 staged | 生产 drain slice + 并发 discharge 证明 | 各自获批 slice |
| 5 | "backend-neutral in all practical workloads" | E2 未执行；仅代码级 reuse | E2 | app×backend 交换 + LOC/语义差为零 |
| 6 | "Sluice makes AI-generated systems code safer" | E8 unexecuted | E8 | controlled 多臂实验 |
| 7 | "explicit lifecycle already enables superior optimization" | RE-3a 未执行；无 capability 实现 | RE-3a | 公平 A/B + legality 论证成立 |
| 8 | "all Sluice resources are bounded under overload" | E3/E4 未执行；group.hpp open 缺陷 | E3+E4 | 系统化饱和实验 |
| 9 | "DST systematically covers concurrency bugs" | 单案例 + 单 seam 维度 | 更多 DST 案例/#239 全矩阵 | 逐案例证明 |
| 10 | "observability/attribution deepening is justified" | RX-1 FALSIFIED（STOP EXPANSION） | 仅限实质不同的新预注册假设 | 新假设 fair falsification |
| 11 | "TLA+/GenMC 证明实现正确" | 模型成功 ≠ 实现证明（#227 硬门 7） | refinement 持续 | 模型-代码对应证据 |

---

## TOP 5 MISSING EVIDENCE

按对 canonical thesis 的 information gain 排序：

1. **SE-1 hazard corpus + SE-2 detection matrix（含 Sluice-induced mutant 半区）** —— Claim：T-S2/G1-Safety。决策相关性：这是唯一能在**不购置任何硬件**前提下解锁的 G1 门；且 induced-hazard 流（6 项 production）已证明反命题是活的，必须正面计量。最小实验：复用现有 192 门 mutation census + M1–M5 + D2/D4 形态，建 13 族 hazard × 检测层矩阵。成本：中（无新基建）。可能负结果：存在 surviving silent mutants → S1 claim 收窄为逐例清单。架构决策变化：负结果不杀 thesis，但限定 safety claim 形状并指导 S1 机制投资。
2. **RE-0H + RE-1U：首个真实环境（native Linux + NVMe + real liburing）上的 raw liburing vs Sluice uring matched ladder** —— Claim：T-P3/T-P2/G1-Performance。#227 明文 "moves early because it is the direct test of the paper claim"。成本：高（硬件 + 环境）。可能负结果：L4−L3 tax 大 → 论文性能支柱受限，产品定位转向 #237 verdict 7 的 envelope/boundedness 卖点。
3. **RE-1 native L0/L1/L2 重复** —— Claim：T-P1 推广边界。与 2 同环境，边际成本低。负结果：两税比例在 native 存储上换手（device-bound 下 control-plane 占比应缩小——这本身是有价值的 envelope 数据）。
4. **E2 backend neutrality（真实 app × {ThreadPool, Uring} 交换）** —— Claim：T-B1c/T-B1。依赖 2 的环境；成本低。负结果：出现语义改写/backend 分支泄漏 → 边界 claim 收窄。
5. **RE-3a 注册缓冲/固定文件 capability A/B** —— Claim：T-SP1/G1-Control/T-C2。legality 论证已成立（显式 lifetime = 注册缓冲合法性前提，本次经 liburing 官方文档核验）。成本：中。负结果：收益不显著或 legality 不依赖 Sluice 契约 → G1-Control 需换候选。

不发明新大 campaign；以上全部是 #227 已定义的 SE-1/SE-2/RE-0H/RE-1/RE-1U/RE-2/RE-3a 序列。

---

## NEXT 3 EXPERIMENTS

**NEXT 1：SE-1 + SE-2（hazard corpus + 检测矩阵）**
WHY：information gain / 复杂度比最高——零硬件依赖、直接解锁 G1-Safety、把已存在的 6 项 induced hazards 与 mutation 基建（192 门、M1–M5、C1–C4、D2/D4）升级为统一证据；#227 的 current next step 在 AC-2a/FE 收口后自然轮到它。任何结论（含负结果）都直接进入论文的 safety 章节。

**NEXT 2：RE-0H + RE-1U（真实环境 + raw-liburing vs Sluice-uring ladder）**
WHY：性能是 hard validity requirement；RE-1U 是 paper claim 的直接检验，且环境筹备是长 lead time 项，必须早启动；SLUICE_HAS_LIBURING real 路径已实现并经 mutation 验证，缺的只是环境与 matched 配置测量。

**NEXT 3：RE-3a 注册缓冲/固定文件 capability A/B（在 RE-1U 之后）**
WHY：G1-Control 目前证据为零；这是唯一一个"合法性本身依赖显式 lifetime 语义"的候选优化（liburing 注册缓冲语义核验），能一实验同时回答 T-SP1 与 T-C2；排在 RE-1U 后因为公平 A/B 需要先有真实 io_uring 基线。

---

## CLAIM PROMOTION RULE

固定后续 promotion law：

```text
HYPOTHESIS
    ↓ direct experiment exists
SUPPORTED / PARTIAL
    ↓ replicated across required scope / fair baselines
PROVEN — SCOPE-BOUNDED

negative:
HYPOTHESIS
    ↓ fair falsification
FALSIFIED / NOT SUPPORTED
    ↓ STOP / narrow thesis / change architecture
```

不可跳级项（重申）：architecture intention、test count、formal model 存在性、one benchmark、one app、one host。本次审计执行情况：无任何 claim 跳级；RX-1 是仓库迄今唯一走完完整 fair falsification 链的 claim。

---

## FINAL VERDICT

**B. THESIS LEDGER ESTABLISHED — MULTIPLE THESIS DIMENSIONS SUPPORTED, G1 NOT YET READY**

理由：

- 不选 A：已支持的远不止 Boundary——S0/S1（逐 hazard 配对 + 真实检出史）、C1（控制面存在并被动用）、P1（两税分离的 scope-bounded 测量）、R1（partial）、D1（单案例）、U1（可行性）均有 scope-bounded 证据。
- 不选 C：四门中三门 NOT READY，一门 PARTIAL；SE-1/SE-2、RE-0H/RE-1U/RE-2、RE-3a 全部缺失。
- 不选 D：claim map 稳定——每条 claim 可追溯证据、可证伪、scope 明确；唯一的正式 falsification（RX-1）已被 roadmap 吸收为 stop signal。
- 不选 E：无未处理的实质性反证（E1 异常 cell 与 induced hazards 都已如实入账并界定 scope；RX-1 已收口）。

审计边界声明：本报告是 audit artifact，不授权任何 production change；未更新 #221/#225/#227；未 commit/push/开 PR。人类持有的两份 untracked closeout 报告未被移动或修改。
