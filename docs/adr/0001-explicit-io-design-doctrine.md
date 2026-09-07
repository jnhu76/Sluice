# ADR-0001：Sluice 的显式 I/O 宗旨与设计边界

- **状态**：Accepted / Frozen
- **范围**：项目级设计原则
- **替代方式**：若未来需要改变，只能通过新的 ADR 显式替代；不在普通实现重构中直接修改本决策

## Context

Sluice 长期面临一个核心风险：随着功能、后端、测试、研究与运行时机制增加，项目容易把“实现中存在的东西”误当成“项目必须长期拥有的语义”。

这会产生几类典型问题：

- backend capability 反向污染 public API；
- execution policy 被包装成 semantic contract；
- 为未来可能的扩展保留没有真实 caller 的 abstraction；
- correctness authority 分散在多个对象中，形成隐含协议；
- 测试 seam、mock、simulation backend、观测框架等为了过去的需求永久留在核心路径；
- 自然语言文档、注释和旧研究叙事逐渐成为第二套事实来源；
- 显式语义被错误地理解为必然带来 generic control、自动优化或性能收益。

Sluice 的价值不在于拥有最多的 async/runtime abstraction，而在于建立一个**小、清楚、可验证的显式 I/O boundary**。

因此需要冻结一组长期原则，用来审判当前与未来所有 public surface、runtime mechanism、backend 和验证设施。

## Decision

Sluice 被定义为：

> **一个显式 I/O 库：只暴露调用者真正必须依赖的 I/O 语义和资源边界，用最小的正确性机制忠实执行这些语义，并让执行机制可替换但不能反过来污染公共语义。**

项目采用六条长期原则：

```text
Minimal semantics.
Clear boundaries.
Explicit authority.
Named bounds.
Replaceable execution.
Minimum mechanism.
```

中文固定表述为：

> **语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

规范性简版见 [`../mission.md`](../mission.md)。本 ADR 解释这些原则为什么存在以及它们约束什么。

## 1. 为什么是 Minimal semantics

“显式 I/O”不等于“把所有内部状态都公开”。

公共语义只应覆盖调用者为了正确表达 I/O 行为而必须依赖的事实，例如：

- operation intent；
- resource identity；
- buffer / lifetime boundary；
- request lifecycle；
- completion；
- cancellation；
- durability；
- 真实 resource bounds。

判断一个概念是否属于 public surface 的默认问题是：

> 如果调用者不知道它，是否仍能准确、安全地表达自己的 I/O 意图？

如果可以，它更可能属于 implementation、policy、helper 或根本不应存在。

这条规则防止内部 machinery 因“已经实现了”而自动获得 API 永久居留权。

## 2. 为什么 Boundary 必须清晰

Sluice 把以下职责明确分开：

```text
SEMANTIC SURFACE
CORRECTNESS KERNEL
RESOURCE BOUNDS
BACKEND CAPABILITY
EXECUTION POLICY
OBSERVATION / HINT
```

它们之间不是同义词。

### Application boundary

Application 应依赖 public semantic boundary，而不是 `detail/`、scheduler bookkeeping、backend scratch state 等内部实现。

如果真实应用必须穿透 public API 才能完成普通任务，这首先是 boundary finding，而不是自动把内部 helper 公开化的理由。

### Backend boundary

后端支持某种机制，只说明 mechanism available，不说明上层 contract 应暴露对应语义。

例如，某个 backend 能够 fixed-file、batch submit、register buffer 或提供额外 hint，并不自动授予 Sluice 新的 semantic authority。

### Policy boundary

chunk size、queue discipline、worker topology、batching strategy 等通常属于 execution policy。

只有当它们成为调用者必须依赖的可观察 contract 时，才有资格进入 semantic surface。

## 3. 为什么需要 Explicit authority

并发 I/O 中最危险的问题之一，是一个 invariant 没有单一清楚的 owner。

例如：

- 谁决定 request 被接受；
- 谁决定 terminal winner；
- 谁结束 buffer borrow；
- 谁发布 Completion；
- 谁关闭 waiter registration；
- cancel 与 completion 谁赢；
- 谁释放 slot 和 accounting。

如果这些事实分散在多个类中，并依赖“调用顺序应该如此”才能保持正确，那么实现会变成 distributed protocol。

因此默认原则是：

> **一个关键 invariant 对应一个尽量清楚的 authority。**

这不要求所有状态都塞进一个大对象，而是要求决策权和最终事实来源可定位、可测试、可形式化描述。

## 4. 为什么强调 Named bounds

异步系统很容易通过隐式 unbounded queue、outstanding requests、tasks、waiters 或 buffer growth 把资源风险藏起来。

重要资源必须有明确上界。

但“有界”不意味着每个上界都要成为动态配置。配置项也是 public complexity 和 reasoning tax。

如果一个简单固定值已经满足真实调用者，固定值优于为了假想通用性新增一套配置 framework。

## 5. 为什么 Replaceable execution 不等于 Multi-backend framework

Sluice 希望 public I/O contract 不绑定某一种 execution mechanism。

这意味着 ThreadPool、io_uring 或未来其它机制原则上可以执行同一 semantic contract。

但这不构成“必须永远维护多个 backend”的义务。

一个 backend 长期存在至少要证明一种价值：

- 有真实 caller；
- 提供真实需要的能力；
- 是跨 mechanism semantic conformance 的必要 witness；
- 是不可替代的 correctness/testing mechanism。

仅仅“理论上是第二个实现”不够。

因此 replaceability 是**边界要求**，不是 backend 数量 KPI。

## 6. 为什么加入 Minimum mechanism

这是本决策最重要的补充原则。

传统架构评审容易问：

> 这个 abstraction 是否合理？

Sluice 还必须先问：

> **这个 abstraction 是否需要存在？**

任何 class、interface、backend、wrapper、queue、helper、state、macro、configuration、test seam、scheduler layer 都需要支付：

- runtime cost；
- compile/header cost；
- ownership/lifetime cost；
- test cost；
- documentation cost；
- human reasoning cost；
- LLM context cost。

因此新增一层机制时，必须说明它当前买来了什么。

足够强的理由通常只有：

1. 表达必要语义；
2. 维护关键 invariant；
3. 表达真实 resource bound；
4. 隔离真实 execution difference；
5. 服务真实 caller 且比直接实现更简单；
6. 提供不可替代的验证能力。

“future flexibility”“framework completeness”“过去测试用过”“论文这样设计”默认都不足。

这是一种 subtraction-first discipline：

> **不为没有产生价值的层付税。**

## 7. 信息不等于 Semantic Authority

Sluice 拒绝以下推理：

```text
系统知道更多信息
    -> runtime 获得更多控制权
    -> 可以做新的 transformation
```

这个链条并不天然成立。

任何新增 semantic-authority claim 必须使用下面的证明形状：

```text
C = baseline contract
T = candidate transformation
G = additional semantic grant

T 在 C 下并不天然合法；
T 对所有满足 C + G 的程序才合法。
```

因此长期保持：

```text
resource identity != fixed-resource optimization authority
operation grouping != fused / atomic admission authority
backend capability != semantic authority
hint / information != authority
```

如果 T 在原 contract 下本来就合法，那么它可以是 implementation optimization，但不是新增 semantic grant 的价值证明。

## 8. Correctness、Performance、Semantic Authority 分开

Sluice 不把下面的链条作为项目假设：

```text
explicit semantics
    -> generic control
    -> safety
    -> performance / specialization
```

三条证据线独立：

### Correctness

目标是让危险从 silent/distributed failure 迁移为更机械的结果：无法表达、静态拒绝、fail-fast、可确定复现，或至少被明确检测。

同时必须记录 Sluice 自己新增了哪些 protocol hazard；不能只统计消除的风险。

### Performance

Correctness machinery 和 resource bounds 不是免费的。

它们必须面对公平 baseline 和真实环境测量。语义正确不能成为性能免检理由。

### Semantic Authority

只有额外 contract 真正改变合法 transformation space 时，才承认新增 authority。

性能收益不能反过来证明某个 public semantic concept 必须存在。

## 9. Verification 的位置

测试和 formal verification 是保护 boundary 的手段，不是新的架构身份。

允许并鼓励：

- deterministic tests；
- property-based tests；
- stateful fuzzing；
- ASan / TSan / UBSan；
- race / death / negative tests；
- TLA+ 或其它适合关键协议的形式化模型。

但数量不是目标。

TLA+ 证明模型，不直接证明 C++。重要模型必须建立 C++ state / transition / authority 的 correspondence / refinement evidence。

如果一个形式化状态无法指出代码中的对应事实，或者代码的重要 transition 在模型里没有 counterpart，应视为验证缺口。

## 10. Context hygiene

AI 参与开发后，自然语言 context 也成为真实工程成本。

旧注释、旧 ADR、旧 roadmap、旧测试名词和历史 research narrative 即使曾经正确，也可能让模型围绕过期世界继续扩展代码。

因此 Sluice 固定各信息源职责：

```text
代码 / 构建定义        当前实现事实
architecture.md        代码推导出的当前架构快照
mission.md             冻结的项目宗旨
ADR                     重大设计决定及理由
测试 / fuzz / formal   correctness evidence
research/RESULTS.md     值得长期保留的研究结论
```

普通代码注释只承担局部、非显然的 WHY / invariant；不承担历史、roadmap 或重复架构说明。

## 11. Consequences

采用本决策后：

- public API 默认不扩张；
- `detail/` 穿透视为 boundary finding；
- 没有真实 caller 的 abstraction 必须重新证明生存资格；
- backend 数量本身没有价值；
- internal mechanism 可以被移动、合并、internalize 或删除，只要必要语义与 correctness 不丢失；
- architecture review 必须同时审查 semantic surface 和 mechanism count；
- header/cpp 减肥以减少概念、依赖、authority 和实现负担为目标，而不是追求 LOC KPI；
- architecture snapshot 可以随代码变化，项目宗旨不能为现有实现找理由而漂移；
- correctness、performance、semantic authority 分别建立证据；
- verification infrastructure 也接受 minimum-mechanism 审查。

架构审计的标准处置只有：

```text
KEEP
SIMPLIFY
MERGE
INTERNALIZE
DELETE
```

## 12. Rejected alternatives

### “先做完整 framework，再慢慢优化”

拒绝。它会提前支付 abstraction、API、测试和 context 成本，而真实 caller 尚未证明这些层有价值。

### “为了 replaceability 必须保留多个 backend”

拒绝。Replaceability 是 semantic boundary 不绑死 execution mechanism，不是实现数量要求。

### “显式语义天然带来控制和性能优势”

拒绝。Correctness、performance 和 semantic authority 必须独立证明。

### “更多 formal model 就意味着更安全”

拒绝。模型若没有 C++ correspondence，只证明模型；验证数量也不能替代 boundary correctness。

### “保留旧设计信息有助于 AI 理解项目”

部分拒绝。只有长期有效、不会与当前事实竞争的信息值得留在活跃 context；其余历史应留在 Git 归档，而不是成为日常 authority。

## 13. Stability

本 ADR 与 `docs/mission.md` 共同定义 Sluice 的长期设计基线。

普通 feature、refactor、backend replacement、performance optimization、测试或形式化工作都不得通过修改这两份文档来为自身获得合法性。

如果未来出现足够强的新证据，必须：

1. 新建 ADR；
2. 明确指出要替代的原则；
3. 给出新证据和 tradeoff；
4. 由人类显式批准 supersede。

在此之前，默认方向保持：

> **在不损失必要 I/O 语义、正确性和真实执行能力的前提下，删除更多，而不是解释更多。**
