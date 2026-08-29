# Sluice 项目代理协作契约

本文件规定整个仓库内编码代理必须遵守的工作方式、架构边界和文档路由。它不是第二份 API 参考、架构说明、迁移状态表、测试手册或 Issue 记录。更具体的目录级 `AGENTS.md` 可以增加局部约束，但不得静默削弱 Accepted ADR、架构宪法、公共契约或本文件的安全边界。

规范词“必须”“禁止”“应当”“可以”具有约束含义。

## 1. 项目定位与边界

Sluice 是实验性的 C++20 显式 I/O 与控制流库，核心目标是让 I/O 能力、请求身份、资源所有权、执行策略和后端机制可见、可控制、可验证。

必须保持以下层次独立：

- `sluice_core`：同步 `Reader` / `Writer`、`Result<T>` / `IoError`、文件与持久化语义以及 `BlockingIoPool`；
- `sluice_async`：可选异步运行时、`AsyncIoContext`、`Completion<T>`、Scheduler 和后端集成；
- `sluice_async_internal_testing`：使用生产源文件和编译期保护测试 seam 的测试专用目标；生产目标不得依赖它，任何 executable 不得同时链接两个 async variant；
- `sluice_experimental_uring`：默认关闭的实验性 io_uring 后端；io_uring 是机制，不是整体架构；
- `zig/`：源代码派生的设计参考，不是生产依赖，也不能机械照搬。

以下概念不得混淆：

- 同步 I/O 与可选异步运行时；
- Threaded/Evented 任务执行策略与阻塞 I/O offload；
- Scheduler worker、阻塞 I/O worker、内核 queue depth、request capacity 和应用 pipeline depth；
- caller-owned `Completion<T>` 与被接受请求的逻辑身份；
- 测试 seam 与生产语义权威。

公共 `Reader` / `Writer` 语义保持同步；异步实现便利不能静默改变同步公共契约。

不得在无关任务中顺手引入全局 Runtime、隐式默认 executor、P2300、协程层、actor runtime、通用 Awaitable、新取消模型、网络扩展或统一同步原语基类。

## 2. 当前架构状态

本文件不记录会随实现变化的 backend 迁移比例、测试数量、文件行数、Phase 完成状态或性能数字。当前事实按以下入口读取：

| 问题 | 权威入口 |
| --- | --- |
| 项目与子系统导航 | `docs/README.md` |
| Build target、实现边界与代码入口 | `docs/architecture/overview.md`、`xmake.lua` 与 `xmake/*.lua` |
| 当前架构文档分类 | `docs/architecture/README.md` |
| 异步 I/O 最高层工程原则 | `docs/architecture/architecture-constitution.md` |
| Accepted/Proposed/Superseded 决策 | `docs/adr/README.md` 与对应 ADR |
| 公共 API 行为 | `include/sluice/`、`docs/reference/api.md` |
| 当前实现 | `src/` 与实际构建目标 |
| 当前验证方法 | `docs/verification/README.md` |
| 未来范围与非目标 | `docs/roadmap/README.md` |
| Zig 差异 | `docs/architecture/divergence-registry.md` |
| 历史与阶段证据 | `docs/history/`、Git、Issue、PR |

任何“已完成”“已迁移”“已验证”或性能结论都必须从当前权威与可重复证据派生，禁止从本文件、旧 closeout 或历史 Phase 名称推断。

## 3. 权威来源与冲突处理

先按事实类型找到权威，不使用一个全局顺序比较不同种类的信息：

- 当前任务、已批准 Issue/计划决定授权范围和交付目标，不自动改写架构事实；
- Accepted ADR 决定具体架构选择；`architecture-constitution.md` 规定跨子系统工程原则；
- 公共 header 与 API reference 共同承担对外契约；不一致时属于缺陷；
- CURRENT 架构文档描述设计和 as-built 结构，生产代码描述实际执行；
- 测试、脚本、形式化模型和结果文件提供各自证据类别，不自行创造产品或架构语义；
- `xmake.lua` 与其包含的 `xmake/*.lua` 是目标、依赖、feature gate 和测试接线权威；CI workflow 是远端门禁接线权威；
- Roadmap、Proposed 设计和 Issue 只授权其明确范围内的未来工作；
- scanner、review summary、注释、commit message、EVIDENCE/HISTORICAL 文档只提供证据或历史。

同一事实发生冲突时必须：

1. 用聚焦测试、实际构建或精确静态论证刻画 as-built；
2. 确认是实现违反契约、权威过期，还是新语义尚未获批；
3. 通过明确的 ADR/设计决策收敛语义；
4. 在获批范围内同步接口、实现、测试和权威文档；
5. 将有意差异登记到 `divergence-registry.md`，不隐藏分叉。

现有代码只是证据，不自动构成先例。Accepted ADR 不得被改写成“历史上从未做过该决定”；需要改变时新增 superseding decision。

## 4. 显式权威分离

修改异步 I/O 时必须先回答每个状态和动作由谁拥有：

### 4.1 RequestArena / RequestSlot

RequestArena/RequestSlot 生命周期域拥有请求来源、generation、状态、admission/release、terminal winner、结果存储、borrow、waiter registration 和计数。

### 4.2 Backend progress

backend 拥有有界 progress 机制、dispatch/kernel ownership、backend cancellation attempt 和 `RequestKey` 到执行所有权的映射。

### 4.3 Completion publication

只有指定 reap 路径可以把 caller-owned Completion 发布为 ready；worker、CQE、cancel 或 Scheduler 不得绕过它。公共调用者拥有 Completion 的合法读取、reset/reuse 生命周期，不拥有内部 claim/publication 能力。

### 4.4 Scheduler routing

Scheduler 只拥有 runnable/Fiber routing、wait epoch、routing lease 和自身 shutdown/drain，不拥有 I/O terminal result 或 request generation。

禁止用 parallel map、ready deque、closure、`Completion*`、Scheduler record 或 worker-local object 建立第二套逻辑身份权威。

## 5. 工作树保护与授权

开始任务必须检查：

```sh
git status --short
git diff --stat
git diff
```

先判断工作模式：

- 调查/审计只读取、复现、测量和报告；
- 设计提出边界、方案、风险和验证，不自动进入生产实现；
- 施工需要明确修改授权；已批准的具体 Issue/计划或 `/tdd` 可以作为相应范围的授权；
- 评审只检查现有 diff、证据和风险，不擅自扩展作者工作。

必须保留无关 tracked、untracked 和 ignored 文件。未经明确授权，禁止 commit、push、merge、rebase、force-push、切换他人分支或执行破坏性 Git 操作。不得使用 `git clean`、`git reset --hard`、整仓 checkout/restore 或隐式 stash 处理他人改动，也不得删除 `.c-review-results/` 来让 finding 消失。

## 6. 基线与验证策略

验证强度必须与风险匹配。纯文档或单一机械规则修改先运行对应文档/机械门禁，不需要在施工前机械运行完整 C++ 测试。生产代码修改的最低 Linux Clang Debug 基线以 `docs/verification/README.md` 和当前 CI 为准：

```sh
xmake f -m debug --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

开发循环先运行最小相关测试，再按影响扩大到 package/target、完整 Debug gate 和 change-class gate。公共契约、共享基础设施、发布候选或 CI 要求时运行完整门禁。

基线失败时先隔离、确认是否既存并记录准确命令与输出，禁止用弱化断言、重试、增加 sleep/yield/timeout、跳过 target、warning-only 或改变测试分组制造绿灯。

## 6.1 本地 pre-push gate

本地机械门禁的唯一入口是：

```sh
bash scripts/gates/pre-push.sh
```

CI 在干净 checkout 中必须传入明确范围：

```sh
bash scripts/gates/pre-push.sh --range <base>..<head>
```

`lefthook.yml` 只是 dispatcher，`scripts/gates/pre-push.sh` 才是规则入口。本地 hook 可以被绕过，因此不能替代 GitHub CI。禁止用 `|| true`、环境变量污染或空 diff 让必需 gate 静默跳过。

## 7. 开发与 TDD

默认使用 TDD，但先确保测试针对的是正确问题：

- 行为已知的新功能和 Bug：先建立能在修改前失败的 regression/minimal reproducer，再实现和重构；
- 根因未知、性能、并发、时序或工具链问题：先调查、测量和建立 characterization，再进入修复；
- 测试应证明公开语义和承重不变量，不复刻私有实现；优先在真实下一层运行，仅在架构外部边界隔离；
- race/liveness 测试使用 barrier、受控时钟、phase seam、明确状态观察和因果调度；sleep/stress 只能补充，不能充当顺序或活性证明；
- 修复 flake 必须定位非确定性来源和生命周期所有者，禁止靠扩大等待或重跑掩盖。

实现应选择满足需求的最小长期正确结构。结构性缺陷通过重构消除；替换完成后删除旧路径。不得为了固定比例“升级基础设施”，也不得在局部修复中顺手扩大为 generic Executor、网络栈或新运行时。

## 8. 架构合规门禁

涉及以下任一语义的生产修改，在实现前必须完成架构合规：异步 I/O 所有权、RequestKey/RequestSlot、Completion publication、submit/dispatch/reap、取消、资源容量、Scheduler wake/progress、Runtime、公共异步 API、shutdown/drain、同步原语、io_uring 或 executor/thread pool。

必须：

1. 阅读 `docs/architecture/architecture-constitution.md` 并列出适用 AC-N；
2. 阅读 governing Accepted ADR 和 CURRENT 架构文档；
3. 使用 `docs/architecture/design-compliance-gate.md`，或证明 phase-specific gate 覆盖同等字段；
4. 明确状态机、锁/原子权威、资源容量、分配边界、wake/progress、取消和 shutdown；
5. 在实现前把证据标记为 `PENDING`，只能在实际执行后填写 `PASS`；
6. 对 Zig 差异分类并按需更新 `docs/architecture/divergence-registry.md`。

权威、失败、wake、容量或生命周期仍为 Unknown/TBD 时阻断生产实现。测试通过是必要条件，不是架构合规的充分条件。

## 9. C++、I/O 与失败响应

- 使用 C++20，warnings as errors；尽量通过类型、不可伪造 capability 和状态机缩小非法状态空间。
- 普通 I/O 失败使用 `Result<T>` / `IoError`，不引入 exception-based 公共控制流；保留必要的原始 OS error。
- `read_some` / `write_some` 允许 short I/O；exact/all helper 必须正确循环；非空 write 的零进展是 backend failure，不能无限重试。
- 阻塞 syscall 的 `EINTR` 通过仓库统一权威处理，不复制不同 retry loop。
- positional I/O 不改变共享 file offset；`flush()` 不等于 durability；`sync_data()` 与 `sync_all()` 语义不同。
- destructor 不得发明无法报告的成功，也不得隐藏 flush、cancel、drain 或异步 wait。
- borrowed buffer 与 caller-owned Completion 必须在契约规定的请求生命周期内存活并保持地址稳定。
- attacker-controlled size、整数转换、算术溢出和分配/I/O 边界必须在执行前验证；影响 correctness/liveness/data integrity 的返回值不能忽略。

### 9.1 Descriptor validation

真实 syscall backend 在 commit 前验证表示层 malformed descriptor，但不得用 `fcntl(F_GETFD)` 预检替代真实操作并引入 TOCTOU。错误必须保持 Completion idle，不产生 accepted slot、borrow 或后台执行；非负但已关闭的 fd 可以被接受，并由真实 syscall 返回结果。

### 9.2 Failure response 与 assert authority

失败分类和 `assert()` 规则的唯一完整来源是 `docs/architecture/failure-model.md`。`NDEBUG` 和 debug assert 不是语义权威：任何可由输入、线程交错或环境触达的失败，必须使用 typed failure、Debug/Release 都生效的 named fail-fast，或编译期结构保证。`assert()` 只能用于该文档允许并由 `scripts/gates/assert-hygiene.allowlist` 精确登记的站点；既有 assert 只是 evidence，不是新增先例。

## 10. 显式请求生命周期

请求协议的唯一完整来源是 `docs/adr/ADR-explicit-io-request-contract.md` 及对应 CURRENT 架构文档。所有 backend 必须保持以下承重不变量：

### 10.1 Stable identity

每个 accepted request 具有包含 context provenance、slot 和 generation 的稳定 `RequestKey`；slot reuse 在新 occupant 可见前增加 generation。

### 10.2 Transactional submission

successful submit 表示所有必要有界资源、Completion binding、borrow、accounting 和可靠 terminal path 已提交；failed submit 不留下 request、borrow、queue/kernel work 或 accounting residue。Completion 变为 outstanding 后不得再返回 rejection；post-accept enqueue 不得依赖 allocation/throw 或动态扩容。

### 10.3 Enqueue/cancel arbitration

enqueue/cancel 必须由同一个 request-state 权威仲裁；不能同时留下独立 dispatch linkage 与 terminal-ready linkage。

### 10.4 Dispatch ownership

blocking dequeue 与 `running` ownership transfer 之间不得暴露可被 cancel/reap 穿过的空窗；kernel ownership 未结束时不得释放 slot。

### 10.5 Terminal winner

ordinary result、有效 cancel 和获批 shutdown terminal 由单一 terminal-winner 仲裁；loser 不覆盖结果、不重复 unlink/account/release。

### 10.6 Reap authority

只有 reap 关闭 registration、取得 delivery、结束 borrow、写入 terminal result、更新 accounting 并 release-publish Completion-ready。

### 10.7 Slot release

release 只能发生在 reap 离开 lifecycle domain、enqueue pin 已确认、registration 已关闭且无 backend/kernel ownership 后；release 不等待异步进展，不调用 Scheduler、user code 或 backend progress。

禁止把 `Completion*`、closure、queue index 或 worker index当作 generation-safe logical identity。

## 11. 分层取消

以下概念必须独立：task cancellation、wait cancellation、I/O operation cancellation、syscall interruption、admission close、graceful drain 和 abort shutdown。

每个 cancel API 必须说明目标身份、winner authority、可能 disposition/result、是否中断 syscall、exactly-once 和 best-effort 边界。pending/enqueued cancel 可以按获批协议赢得 canceled terminal；running blocking syscall 在未确认有效中断时只记录 intent，真实 syscall success/error 仍可原样获胜。io_uring cancel CQE 只是取消尝试证据，不能独立覆盖原请求结果。取消路径不得直接发布 Completion-ready，也不得作用于已复用 generation。

## 12. 资源边界

每个重要资源必须显式有界，或由调用者拥有并负责设限。至少区分：

```text
request capacity
blocking-I/O worker count
scheduler worker count
io_uring queue depth
application pipeline depth
caller-owned Completion count
```

新增或修改资源前必须说明 capacity、allocation time、hot-path allocation、full behavior、reclamation、high-water evidence 和 shutdown owner。容量压力应在 acceptance 前返回可报告结果，例如 `would_block`。

禁止 unbounded per-op map/ready queue、per-op detached thread、以 `std::function` 作为核心 accepted record、按历史提交数增长的容器，以及把 post-accept 动态分配当作活性前提。

### 12.1 ThreadPoolBackend

ThreadPoolBackend 必须使用固定持久 worker、有界 dispatch storage、RequestKey/RequestSlot 身份和固定 operation payload；worker 只记录 backend-ready，reap 才发布 Completion-ready。不得按 operation 创建线程或让 storage 随历史提交量增长。

### 12.2 UringAsyncBackend

修改真实 io_uring 路径时必须保持 RequestKey、未提交 suffix、SQE/kernel/CQE ownership 和 stale-generation validation；request capacity 与 ring depth 分离。stub/off 证据不能冒充 real-liburing 证据。

## 13. 并发、锁与唤醒

### 13.1 Lock order

并发设计必须给出 lock/atomic authority table 和全局锁序。RequestArena slot-lifecycle 是 leaf domain；持有它时不得调用 Scheduler、ReadySink、user code，不得 syscall、join 或等待 backend/kernel progress，也不得形成 backend-lock ↔ arena-lock 双向顺序。

### 13.2 Wake obligation

每个 progress-enabling state change 必须说明 persistent predicate/state、producer、sleeping consumer、signal、commit-to-sleep race closure、最坏观察延迟和 shutdown 行为。优先使用 predicate、epoch、sequence 或等价协议；禁止用 yield loop、sleep、无状态 notify 或未声明的 periodic poll 证明 wake 正确。

### 13.3 Deterministic concurrency tests

并发正确性应使用 barrier、condition variable、受控时钟、deterministic phase seam 和明确状态观察；TSan 与 stress 只能补充，不能替代因果证据。

Threaded 与 Evented 必须保持不同物理等待机制：Evented 由 Scheduler/Fiber/global coordination 管理 runnable 与 wake；Threaded 默认阻塞调用线程。它们可以共享 Future/Group/CancelToken 和 L1 Completion/AsyncIoContext 语义，但不得为追求代码统一而伪造共同 substrate。

## 14. Shutdown 与销毁

异步 backend/context 的默认销毁前提是 quiescent。除非新的 Accepted ADR 明确改变：

- destructor 不隐式 close admission、cancel、drain、等待 I/O 或发布 terminal；
- 存在 accepted/bound request 时销毁属于 Debug/Release 都生效的 contract violation；
- 正常顺序是显式 close admission → 持续 progress/reap → caller reset/destroy ready Completion → 所有 request/slot/backend ownership 归零 → destroy；
- 已 idle 的持久 worker 可以在销毁时被通知并 join，但这不是隐式 I/O drain。

任何 abort/cancel-on-shutdown 模式都需要单独获批语义和验证。

## 15. 测试专用控制面

测试 seam 必须由 `SLUICE_ASYNC_INTERNAL_TESTING` 编译期保护，并且：

- 不改变公共 API、exported production behavior 或生产 correctness dependency；
- 不泄露到 production target、installed header 的非保护路径或正常 include path；
- 优先放入 `src/async/*_test_seams.hpp` / `*_test_access.hpp` 等非安装 header；
- 只提供 deterministic phase control、只读观察或有界证据，不暴露任意 queue/RequestSlot mutation；
- 若影响 object layout，必须明确设计和成本，不能静默发生。

生产排除规则由 `scripts/gates/mechanical-facts.py` 等当前 gate 机械验证；不要在本文件复制具体 target 接线。

## 16. 按变更类型选择门禁

完整命令与当前可用性以 `docs/verification/README.md`、`xmake.lua` 和 CI 为准：

### 16.1 公共契约

公共 header、template、`noexcept`、fail-fast 或 API 修改需要 Clang Release、公共契约和相应 negative-compile 证据。

### 16.2 Ownership 与内存

ownership、allocation、buffer lifetime、parsing 或 filesystem 修改需要 ASan + UBSan，必要时运行 Valgrind。

### 16.3 并发语义

Scheduler、同步、取消、queue、wake、multi-worker 或 backend migration 需要 deterministic causal tests + TSan。

### 16.4 Build 与 CI

证明 core/async/test 独立构建、失败正确传播、可选 feature 默认关闭、测试 seam 不泄漏生产目标。

### 16.5 io_uring

始终验证 stub/off；环境具备时单独报告 real liburing。

### 16.6 文档与机械规则

运行 doc links、architecture docs、mechanical facts、claim/assert hygiene 和 `git diff --check` 等适用 gate。

### 16.7 性能

遵循 `docs/verification/performance-engineering.md`，提供同 session、Release、machine-readable evidence。

不得把 unavailable/unexecuted gate 报告为 PASS。性能变更不能只凭 microbenchmark 或端到端总差值授权；必须定义 workload、competent baseline、归属层、同 session A/B 和 regression matrix。性能收益不得以削弱 ownership、identity、cancel、wake 或 shutdown 语义换取。

## 17. 形式化与弱内存证据

TLA+ 模型、GenMC kernel 和 C++ 测试是不同证据层：

- TLA+ inventory 位于 `spec/tla/manifest.json`，统一入口是 `python3 scripts/formal/verify.py`；不得直接在源 suite 目录运行 TLC；
- 修改被建模的状态转移、admission、queue bound、terminal winner、wake、generation 或 shutdown 时，必须更新对应模型，或明确记录覆盖缺口和触发条件；
- 必须保留 broken/negative model，证明模型能命中目标错误；
- 必须有 C++ regression 把抽象性质连接到实际行为；
- 形式化模型检查不等于证明 C++ 程序无缺陷；bounded weak-memory kernel 也不是 whole-program 结论。

只建模能捕获承重竞态的最小协议，禁止为“有形式化覆盖”制造大而空的模型。

## 18. Backend conformance 与测试哲学

正确性测试证明语义，不证明实现偏好。thread count、容器内部、时间、syscall 名称可以验证资源机制，但不能单独证明 no-lost-wake、exactly-once、accepted terminality、取消、shutdown convergence 或 backend conformance。

语义测试应覆盖 capacity refusal、accepted request 单一 terminal、enqueued cancel 不执行、running cancel 保留真实结果、wait 不丢 wake、stale generation 不 dispatch、quiescent destruction 等。共同契约必须对所有适用 backend 运行；backend-specific mechanism test 不能代表全仓 conformance。

## 19. Finding 与安全评审

自动 scanner 和 review finding 都是待验证假设。处理时必须检查实际代码与调用者、确认 trust boundary 和可达 failure mode，并用测试、聚焦 probe 或精确静态论证分类为 true positive、false positive、accepted risk 或 duplicate。

修复必须定位根因并保持范围外行为。禁止批量混合无关 finding、为消除告警改变公共语义、因 scanner 不再报告就声称已修复、弱化 fail-fast/error vocabulary，或把 stale identity、double terminal、lost wake 描述成无害时序噪声。

## 20. 代码结构、格式与依赖

使用仓库 `.clang-format` 和 `.clang-tidy`，只格式化实际修改范围。禁止为局部修复执行整仓格式化；`clang-tidy --fix` 属于代码修改，必须正常评审和验证。

先阅读和复用现有结构。成熟第三方库能显著降低长期复杂度且依赖边界合理时可以引入，但必须评估供应、构建、平台和 ABI 成本；简单稳定能力不为“禁止造轮子”强行加依赖。

密钥、令牌和发布凭证不得进入版本控制，即使仓库是私有的；使用环境变量、密钥管理或被忽略的本地配置。

文件大小只是结构审查信号，不是机械拆分阈值。按独立变化原因、生命周期、并发权威、领域边界和可测试性决定拆分；生成代码和高内聚状态机单独判断。单一调用不是禁止 helper 的理由，仅在它形成清晰语义/生命周期/错误边界或显著降低认知负担时抽取。

## 21. 注释与文档

代理必须阅读修改区域附近的现有注释，但应结合类型、代码、测试和权威文档验证，不能无条件相信。

禁止：

- 复述显然控制流、类型或变量名的注释；
- Issue、PR、Job、Phase、修复记录和迁移过程等历史型生产注释；
- 用长注释弥补职责混乱、命名不清或无法测试的结构。

应当保留无法从代码可靠推导的 `WHY`、`INVARIANT`、`OWNERSHIP`、`LOCK ORDER`、`PROTOCOL`、`INTENTIONAL` 信息，尤其是容易被“简化”后破坏的约束。可以机器验证的约束优先编码为类型、capability、assertion、测试、形式化模型或静态 gate；注释保留不可执行的原因和边界。修改相关代码时必须同步审查附近注释，失效注释属于缺陷。

文档负责概念、架构、契约、导航和代码无法自然表达的唯一信息，不复述实现流程。当前文档描述当前完整状态，不包含施工报告；历史与过程属于 Git、Issue、PR、EVIDENCE 或 `docs/history/`。修改 authority、lifecycle、ownership、failure、resource、lock、wake、cancel、shutdown、API、target、feature gate、formal binding 或 Zig divergence 时，必须更新相应唯一权威。

实现导航只指向稳定的 target、目录、公共入口、构建清单和验证接线，不手工复制完整 source/test inventory、数量、Phase 或迁移比例。精确 target/source membership 以 `xmake.lua` 和 `xmake/*.lua` 为准；移动或重命名导航目标时必须在同一变更中更新引用，并运行 `python3 scripts/check-doc-links.py` 与 `python3 scripts/verify-architecture-docs.py`。

禁止无作用域地声称性能提升、sanitizer/real-liburing 覆盖、形式化实现证明或完整迁移；所有 claim 必须命名证据类别与适用边界。

## 22. Commit 与 PR

每个 commit 应语义内聚、可独立审查和回滚，并保持代码可构建、可运行。正确性任务优先分离 regression evidence、生产修复、formal/negative control、文档/合规证据和性能证据；不得把无关清理混入协议迁移。

提交前必须复核最终 diff，确认无架构分叉、重复真相源、新旧双轨、无效代码、无价值抽象、过度兜底或范围膨胀。只有在用户明确授权并完成 review 后才能 commit/push；未经要求不得 merge。

PR 使用 `.github/pull_request_template.md`，如实填写 baseline、authority、语义变化、资源/锁/wake/cancel/shutdown 影响、实际命令结果、跳过项和剩余 divergence/risk。不得预填 PASS，也不得在生产 backend、wake bridge、Scheduler routing 或公共 API 仍在范围外时声称完整完成。

## 23. 完成报告

非平凡任务结束时简洁报告：

- 授权范围与 baseline branch/SHA；
- governing ADR/架构和适用 AC-N；
- 根因或设计判断；
- 修改文件及语义影响；
- 状态/竞态、资源、锁序和 wake 证明中实际适用的部分；
- 实际执行的 focused/full/change-class 命令及结果；
- `SKIPPED` 项及原因、真实剩余风险；
- commit、published branch 和工作树状态（仅在已授权时）。

“看起来正确”“应该通过”“CI 会检查”以及未执行的门禁都不是完成证据。
