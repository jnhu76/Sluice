# Sluice 项目代理协作契约

本文件是仓库级 durable governance + task routing：只保留几乎所有代理在任何任务前都必须知道的硬规则，并把领域细节路由到唯一权威。它不是第二份架构手册、验证手册、失败模型、请求协议、formal-method 手册或 PR 手册；当前事实（target 接线、门禁命令、测试规模、性能数字、阶段状态）一律从对应权威读取，不从本文件推断。更具体的目录级 `AGENTS.md` 可以增加局部约束，但不得静默削弱 Accepted ADR、架构宪法、公共契约或本文件的安全边界。

规范词“必须”“禁止”“应当”“可以”具有约束含义。

## 1. Mission 与项目原则

Sluice 是实验性的 C++20 显式 I/O 与控制流库，核心目标是让 I/O 能力、请求身份、资源所有权、执行策略和后端机制可见、可控制、可验证。当前 north star 与语义权威见 GitHub `#225`（architecture constitution）；执行顺序见 `#227`（唯一 roadmap）。

必须保持以下层次独立：

- `sluice_core`：同步 `Reader` / `Writer`、`Result<T>` / `IoError`、文件与持久化语义以及 `BlockingIoPool`；
- `sluice_async`：可选异步运行时、`AsyncIoContext`、`Completion<T>`、Scheduler 和后端集成；
- `sluice_async_internal_testing`：使用生产源文件和编译期保护测试 seam 的测试专用目标；生产目标不得依赖它，任何 executable 不得同时链接两个 async variant；
- `sluice_experimental_uring`：默认关闭的实验性 io_uring 后端；io_uring 是机制，不是整体架构；
- `zig/`：源代码派生的设计参考，不是生产依赖，也不能机械照搬。

以下概念不得混淆：同步 I/O 与可选异步运行时；Threaded/Evented 任务执行策略与阻塞 I/O offload；Scheduler worker、阻塞 I/O worker、内核 queue depth、request capacity 和应用 pipeline depth；caller-owned `Completion<T>` 与被接受请求的逻辑身份；测试 seam 与生产语义权威。公共 `Reader` / `Writer` 语义保持同步；异步实现便利不能静默改变同步公共契约。

不得在无关任务中顺手引入全局 Runtime、隐式默认 executor、P2300、协程层、actor runtime、通用 Awaitable、新取消模型、网络扩展或统一同步原语基类。

## 2. 权威与冲突处理

| 事实类型 | 权威 |
| --- | --- |
| 任务范围与交付目标 | 已批准 Issue / 计划 |
| 架构选择 / 跨子系统原则 | Accepted ADR；`docs/architecture/architecture-constitution.md`（AC-N） |
| 公共契约 | `include/sluice/` 与 `docs/reference/api.md`（不一致属于缺陷） |
| target / 依赖 / feature gate | `xmake.lua` 与 `xmake/*.lua`；CI workflow 是远端门禁接线权威 |
| 验证方法 | `docs/verification/README.md` |
| 失败分类与 `assert()` | `docs/architecture/failure-model.md` |
| 请求协议 | `docs/adr/ADR-explicit-io-request-contract.md` |
| 执行顺序 | GitHub `#227`（唯一） |
| 历史与过程证据 | Git、Issue、PR、`docs/history/` |

同一事实冲突时：先以聚焦测试、实际构建或精确静态论证刻画 as-built → 判断是实现违约、权威过期，还是新语义尚未获批 → 通过明确的 ADR/设计决策收敛语义 → 在获批范围内同步接口、实现、测试和权威文档 → 将有意差异登记到 `docs/architecture/divergence-registry.md`，不隐藏分叉。现有代码只是证据，不自动构成先例；改写 Accepted ADR 必须新增 superseding decision。开发者文档入口与完整导航见 `docs/README.md`。

## 3. 硬不变量

以下每条只保留跨任务必须知道的压缩形式；完整协议由指针权威承载（路由见 §9）。

### 3.1 职责分离

公共/可观察语义面 ≠ 内部正确性内核 ≠ 资源边界 ≠ 后端能力 ≠ 执行策略 ≠ 观测/提示；信息、capability、观测本身都不是权威（`#225`）。

### 3.2 请求生命周期

accepted request 具有稳定 `RequestKey`（context provenance / slot / generation；slot 复用在新 occupant 可见前先增 generation）。submit 事务化：成功即全部有界资源、Completion binding、borrow、accounting 与可靠 terminal path 已提交，失败零残留；Completion outstanding 后不得再返回 rejection；post-accept enqueue 不得依赖 allocation/throw 或动态扩容。enqueue/cancel 由同一 request-state 权威仲裁；blocking dequeue 与 running ownership transfer 之间不得暴露可被 cancel/reap 穿过的空窗。ordinary result、有效 cancel 与获批 shutdown terminal 由单一 terminal-winner 仲裁，loser 不覆盖结果、不重复 unlink/account/release。只有 reap 关闭 registration、结束 borrow、写入 terminal result、更新 accounting 并 release-publish ready；release 只发生在 reap 离开 lifecycle domain、enqueue pin 已确认、registration 已关闭且无 backend/kernel ownership 之后，release 不等待异步进展、不调用 Scheduler/user code。完整协议：`docs/adr/ADR-explicit-io-request-contract.md`、`docs/architecture/async-request-lifecycle.md`。

### 3.3 Completion publication

只有指定 reap 路径可以把 caller-owned `Completion<T>` 发布为 ready；worker、CQE、cancel 或 Scheduler 不得绕过。禁止把 `Completion*`、closure、queue index、worker index 或 Scheduler record 当作 generation-safe 逻辑身份或第二套逻辑身份权威。

### 3.4 分层取消

task cancellation、wait cancellation、I/O operation cancellation、syscall interruption、admission close、graceful drain、abort shutdown 必须保持独立。每个 cancel API 必须说明目标身份、winner authority、可能 disposition、是否中断 syscall、exactly-once 与 best-effort 边界。pending/enqueued cancel 可以按获批协议赢得 canceled terminal；running blocking syscall 在未确认有效中断时只记录 intent，真实 syscall success/error 仍可原样获胜；io_uring cancel CQE 只是取消尝试证据。取消路径不得直接发布 Completion-ready，也不得作用于已复用 generation。权威：ADR-cancel-request-epoch。

### 3.5 资源边界

request capacity、blocking-I/O worker count、scheduler worker count、io_uring queue depth、application pipeline depth、caller-owned Completion count 必须显式有界，或由调用者拥有并负责设限。新增或修改资源前必须说明 capacity、分配时机、hot-path allocation、full behavior、reclamation 与 shutdown owner；容量压力应在 acceptance 前返回可报告结果（如 `would_block`）。禁止 unbounded per-op map/ready queue、per-op detached thread、以 `std::function` 作为核心 accepted record、按历史提交量增长的容器，以及把 post-accept 动态分配当作活性前提。backend 专属规则由 `docs/architecture/async-io-foundation.md` 与 constitution 承载。

### 3.6 锁序与唤醒义务

RequestArena slot-lifecycle 是 leaf domain：持有它时不得调用 Scheduler、ReadySink、user code，不得 syscall、join 或等待 backend/kernel progress，也不得形成 backend-lock ↔ arena-lock 双向顺序。每个 progress-enabling state change 必须说明 persistent predicate/state、producer、sleeping consumer、signal、commit-to-sleep race closure、最坏观察延迟和 shutdown 行为；禁止用 yield loop、sleep、无状态 notify 或未声明的 periodic poll 证明 wake 正确。Threaded 与 Evented 保持不同物理等待机制，不为代码统一伪造共同 substrate。权威：constitution AC-6 与 `docs/architecture/async-*.md`。

### 3.7 Shutdown

异步 backend/context 的默认销毁前提是 quiescent：destructor 不隐式 close admission、cancel、drain、等待 I/O 或发布 terminal；存在 accepted/bound request 时销毁属于 Debug/Release 都生效的 contract violation；正常顺序是显式 close admission → 持续 progress/reap → caller reset/destroy ready Completion → 所有 request/slot/backend ownership 归零 → destroy。已 idle 的持久 worker 可以在销毁时被通知并 join，但这不是隐式 I/O drain。任何 abort/cancel-on-shutdown 模式都需要单独获批语义和验证。

### 3.8 失败与 assert

普通 I/O 失败使用 `Result<T>` / `IoError`，不引入 exception-based 公共控制流，保留必要的原始 OS error。`read_some` / `write_some` 允许 short I/O；exact/all helper 必须正确循环；非空 write 的零进展是 backend failure，不能无限重试；EINTR 通过仓库统一权威处理。positional I/O 不改变共享 file offset；`flush()` 不等于 durability；`sync_data()` 与 `sync_all()` 语义不同。任何可由输入、线程交错或环境触达的失败，必须使用 typed failure、Debug/Release 都生效的 named fail-fast，或编译期结构保证；`assert()` 只能用于 `docs/architecture/failure-model.md` 允许并由 `scripts/gates/assert-hygiene.allowlist` 精确登记的站点，既有 assert 只是 evidence，不是新增先例。descriptor validation 在 commit 前拒绝表示层 malformed，但不得用 `fcntl(F_GETFD)` 预检引入 TOCTOU。borrowed buffer 与 caller-owned `Completion<T>` 必须在契约规定的请求生命周期内存活并保持地址稳定；destructor 不得发明无法报告的成功，也不得隐藏 flush、cancel、drain 或异步 wait。

### 3.9 测试 seam

测试 seam 必须由 `SLUICE_ASYNC_INTERNAL_TESTING`（或对应 family macro）编译期保护：不改变公共 API、exported production behavior 或生产 correctness dependency；不泄漏到 production target、installed header 的非保护路径或正常 include path；优先放入非安装 header；只提供 deterministic phase control、只读观察或有界证据，不暴露任意 queue/RequestSlot mutation；若影响 object layout，必须明确设计和成本。机械权威：`scripts/gates/mechanical-facts.py`（seam/production exclusion）。

## 4. 授权 / STOP / MERGE

工作模式：调查/审计只读取、复现、测量和报告；设计提出边界、方案、风险和验证，不自动进入生产实现；施工需要明确修改授权（已批准的具体 Issue/计划或 `/tdd`）；评审只检查现有 diff、证据和风险，不擅自扩展作者工作。工作树与 Git 保护见 §5。

涉及异步所有权、RequestKey/RequestSlot、Completion publication、submit/dispatch/reap、取消、资源容量、Scheduler wake/progress、Runtime、公共异步 API、shutdown/drain、同步原语、io_uring 或 executor/thread pool 的生产修改，实现前必须完成架构合规：阅读 `docs/architecture/architecture-constitution.md` 并列出适用 AC-N → 阅读 governing Accepted ADR 与 CURRENT 架构文档 → 使用 `docs/architecture/design-compliance-gate.md`，或证明 phase-specific gate 覆盖同等字段 → 明确状态机、锁/原子权威、资源容量、分配边界、wake/progress、取消和 shutdown → 合规证据标记为 `PENDING`，只能在实际执行后填写 `PASS` → 对 Zig 差异分类并按需更新 divergence-registry。权威、失败、wake、容量或生命周期仍为 Unknown/TBD 时阻断生产实现；测试通过是必要条件，不是架构合规的充分条件。

未经明确授权，禁止 commit、push、merge、rebase、force-push、切换他人分支或执行破坏性 Git 操作；未经要求不得 merge。PR 使用 `.github/pull_request_template.md`，如实填写 baseline、语义变化、实际命令结果、跳过项和剩余风险，不得预填 PASS。

STOP 条件（停止扩大施工、记录 residual 并报告，不绕过）：移动被机械 pin 的文档会改变可执行语义；两个 CURRENT 权威对产品语义真实冲突；公共 header/docs 契约问题无法 docs-only 解决；一次移动需要生产 C++ 行为变化；历史证据移动会丢失 verifier provenance；对本文件的 slim 会删除唯一硬不变量。Residual 留给 S0B 或后续 bounded corrective，不隐瞒。

## 5. 工作树与 Git 安全

开始任务必须检查 `git status --short`、`git diff --stat`、`git diff`。必须保留无关 tracked、untracked 和 ignored 文件；不得使用 `git clean`、`git reset --hard`、整仓 checkout/restore 或隐式 stash 处理他人改动；不得删除 `.c-review-results/` 来让 finding 消失。

本地机械门禁的唯一入口是 `bash scripts/gates/pre-push.sh`；CI 在干净 checkout 中用 `--range <base>..<head>` 传入明确范围；`lefthook.yml` 只是 dispatcher。本地 hook 可以被绕过，不能替代 GitHub CI。禁止用 `|| true`、环境变量污染或空 diff 让必需 gate 静默跳过。

密钥、令牌和发布凭证不得进入版本控制，即使仓库是私有的。

## 6. 证据、TDD 与最小充分验证

验证强度必须与风险匹配；基线命令、change-class 门禁与当前可用性以 `docs/verification/README.md` 和当前 CI 为准，本文件不复制命令矩阵。纯文档或单一机械规则修改先运行对应文档/机械门禁，不需要仪式性运行完整 C++ 测试；生产代码修改至少覆盖 Linux Clang Debug 基线的受影响部分；修改 xmake manifest、test wiring、formal manifest 或 CI execution wiring 时，必须运行对应 focused validation。

TDD：行为已知的新功能和 bug，先建立修改前可失败的 regression/reproducer；根因未知、性能、并发、时序或工具链问题，先调查、测量和建立 characterization 再修复。测试证明公开语义和承重不变量，不复刻私有实现；race/liveness 测试使用 barrier、受控时钟、phase seam、明确状态观察和因果调度，sleep/stress 只能补充。语义测试覆盖 capacity refusal、单一 terminal、enqueued cancel 不执行、running cancel 保留真实结果、wait 不丢 wake、stale generation 不 dispatch、quiescent destruction；共同契约对所有适用 backend 运行，backend-specific mechanism test 不能代表全仓 conformance。修复 flake 必须定位非确定性来源和生命周期所有者，禁止靠扩大等待或重跑掩盖。

基线失败时先隔离、确认是否既存并记录准确命令与输出；禁止用弱化断言、重试、增加 sleep/yield/timeout、跳过 target、warning-only 或改变测试分组制造绿灯。不得把未运行的 gate 报告为 PASS；“看起来正确”“应该通过”“CI 会检查”都不是完成证据。

## 7. 基准、研究与声明纪律

所有 claim 必须命名证据类别与适用边界；禁止无作用域地声称性能提升、sanitizer/real-liburing 覆盖、形式化实现证明或完整迁移。性能变更遵循 `docs/verification/performance-engineering.md`：定义 workload、competent baseline、归属层、同 session A/B 和 regression matrix；不得只凭 microbenchmark 或端到端总差值授权；性能收益不得以削弱 ownership、identity、cancel、wake 或 shutdown 语义换取。

TLA+ 模型、GenMC kernel 和 C++ 测试是不同证据层；统一入口是 `python3 scripts/formal/verify.py`，inventory 位于 `spec/tla/manifest.json`，不得直接在源 suite 目录运行 TLC。修改被建模的状态转移、admission、terminal winner、wake、generation 或 shutdown 时，必须更新对应模型或明确记录覆盖缺口；必须保留 broken/negative model；形式化模型检查不等于证明 C++ 程序无缺陷。负结果是 stop 信号：在付出复杂度成本前先充分测量以证伪方向。

Scanner 和 review finding 都是待验证假设：必须检查实际代码与调用者、确认 trust boundary 和可达 failure mode，并分类为 true positive、false positive、accepted risk 或 duplicate。修复必须定位根因并保持范围外行为；禁止批量混合无关 finding、为消除告警改变公共语义、因 scanner 不再报告就声称已修复，或把 stale identity、double terminal、lost wake 描述成无害时序噪声。

## 8. 文档与注释

文档承载概念、架构、契约、导航和代码无法自然表达的唯一信息；当前文档描述当前完整状态，不包含施工报告；历史与过程属于 Git、Issue、PR、EVIDENCE 与 `docs/history/`。修改 authority、lifecycle、ownership、failure、resource、lock、wake、cancel、shutdown、API、target、feature gate、formal binding 或 Zig divergence 时，必须更新相应唯一权威。

移动或重命名文档/target 必须在同一变更中更新全部 consumer，并运行 `python3 scripts/check-doc-links.py` 与 `python3 scripts/verify-architecture-docs.py`。历史文件若内容容易被误读为 current authority，加明显 banner（HISTORICAL / NOT CURRENT AUTHORITY），但不篡改原结论；禁止 copy-new/leave-old 双真相源。

注释：禁止复述显然控制流、类型或变量名；禁止 Issue/PR/Job/Phase/修复记录等历史型生产注释；保留无法从代码可靠推导的 WHY、INVARIANT、OWNERSHIP、LOCK ORDER、PROTOCOL、INTENTIONAL 约束；可机器验证的约束优先编码为类型、capability、assertion、测试、形式化模型或静态 gate。修改相关代码时必须同步审查附近注释，失效注释属于缺陷。

代码：C++20 + warnings-as-errors，尽量通过类型、不可伪造 capability 和状态机缩小非法状态空间；attacker-controlled size、整数转换、算术溢出和分配/I/O 边界必须在执行前验证。使用仓库 `.clang-format` / `.clang-tidy`，只格式化实际修改范围，禁止整仓格式化；`clang-tidy --fix` 属于代码修改，正常评审。引入成熟第三方库需评估供应、构建、平台和 ABI 成本；简单稳定能力不为“禁止造轮子”强行加依赖。

## 9. 任务路由

**只加载任务相关的最小权威；不要递归加载全部历史/证据文档。**

| 任务 | 先读 |
| --- | --- |
| 使用 Sluice | 根 README → `docs/reference/` |
| 改公共语义 | 公共 header + `docs/reference/api.md` + governing ADR |
| 请求生命周期 | `docs/architecture/async-request-lifecycle.md` + ADR-explicit-io-request-contract + 验证 |
| 等待 / 同步 | `docs/architecture/async-synchronization.md` + ADR-execution-model + 验证 |
| 后端 / io_uring | `docs/architecture/async-io-foundation.md` + ADR-explicit-io-request-contract + 验证 |
| Scheduler / 并发 | `docs/architecture/async-runtime.md` + constitution AC-6 + 验证 |
| 失败处理 | `docs/architecture/failure-model.md` + 相关公共契约 |
| 构建 / CI | `docs/architecture/overview.md`（authoritative implementation map）+ `docs/verification/README.md` |
| 形式化 | `docs/verification/formal-models.md` + `spec/tla/manifest.json` |
| 历史缘由 | `docs/history/` |
| 当前要做什么 | `#227`（唯一执行顺序）；Safety `#289`；Performance `#259` |

## 10. 完成报告与提交

非平凡任务结束时简洁报告：授权范围与 baseline branch/SHA；governing ADR/适用 AC-N；根因或设计判断；修改文件及语义影响；实际适用的状态/竞态、资源、锁序、wake 证明；实际执行的 focused/full/change-class 命令及结果；`SKIPPED` 项及原因、真实剩余风险；commit、published branch 和工作树状态（仅在已授权时）。

每个 commit 应语义内聚、可独立审查和回滚，并保持代码可构建；正确性任务优先分离 regression evidence、生产修复、formal/negative control、文档/合规证据和性能证据。提交前必须复核最终 diff：无架构分叉、重复真相源、新旧双轨、无效代码、过度兜底或范围膨胀。
