# Sluice 宗旨

> **状态：FROZEN**
>
> 本文定义 Sluice 的长期项目宗旨。它不是当前实现说明，不随普通重构、后端变化或阶段性需求修改。
>
> 如未来确有证据需要改变宗旨，应通过新的、显式的人类架构决策替代，而不是直接改写本文。

## 一句话

> **Sluice 是一个显式 I/O 库：只暴露调用者真正必须依赖的 I/O 语义和资源边界，用最小的正确性机制忠实执行这些语义，并让执行机制可替换但不能反过来污染公共语义。**

## 六条长期原则

> **语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

```text
Minimal semantics.
Clear boundaries.
Explicit authority.
Named bounds.
Replaceable execution.
Minimum mechanism.
```

### 1. 语义最少

公共 API 只暴露调用者为了正确表达 I/O intent 而必须依赖的事实，例如 operation、resource identity、buffer/lifetime boundary、request lifecycle、completion、cancellation、durability 和必要 resource bounds。

如果调用者不知道某个概念，仍能准确、安全地表达 I/O 意图，则该概念通常不应进入公共语义面。

### 2. 边界清晰

长期职责必须保持区分：

```text
SEMANTIC SURFACE
CORRECTNESS KERNEL
RESOURCE BOUNDS
BACKEND CAPABILITY
EXECUTION POLICY
OBSERVATION / HINT
```

Application 不应依赖实现内部件。Backend capability、execution policy、observation 或 hint 不会因为存在就自动升级为 public semantic authority。

### 3. 权威显式

关键 correctness invariant 应有清楚、尽量唯一的 authority。

Request acceptance、terminal winner、buffer borrow retirement、Completion publication、cancel/complete precedence、resource accounting 等不能依赖多个对象之间隐含而脆弱的约定共同维持。

### 4. 资源有界

影响正确性、内存占用或调度压力的重要资源必须有明确上界。

Named bound 不等于所有东西都要配置化。配置本身也是复杂度；简单固定上界足够时，不为理论通用性增加 policy。

### 5. 执行可换

同一套 I/O contract 可以由不同 execution mechanism 实现；更换 mechanism 不应要求应用理解另一套 request/completion/cancellation 生命周期。

但“执行可换”不意味着必须维护多个 backend，也不意味着建设大而全的 backend framework。

### 6. 机制最小

任何 class、interface、backend、wrapper、queue、helper、state、macro、configuration、test seam 或 scheduler layer，都必须回答：

> **它现在买来了什么？**

只有以下价值通常足以支持长期存在：

- 必要 I/O 语义；
- 关键 correctness invariant；
- 真实 resource bound；
- 真实 execution difference；
- 真实 caller；
- 不可替代的验证价值。

“以后可能有用”“方便未来扩展”“架构更完整”“过去存在过”都不是默认保留理由。

## 固定边界规则

1. Application 不应依赖 `detail/` 或其它实现内部件；出现时视为 boundary leak。
2. Backend capability != semantic authority。
3. Execution policy != semantic contract。
4. Observation / information != authority。
5. Internal state != public semantics。
6. Explicit semantics != automatic optimization entitlement。

## 三条证据线彼此独立

```text
Correctness
Performance
Semantic Authority
```

显式语义不会自动证明 safety、performance 或 specialization；三者必须分别建立证据。

必要 correctness/resource machinery 必须接受性能审查；性能结果也不能反向扩张公共语义。

## 验证原则

测试、property test、fuzz、sanitizer 和形式化验证都只服务于真实 correctness boundary。

TLA+ 证明模型，不会单独证明 C++。重要模型必须建立 C++ state / transition / authority 的对应证据。

## 固定审查结果

面对任何现存或新增模块，只允许以下架构处置：

```text
KEEP
SIMPLIFY
MERGE
INTERNALIZE
DELETE
```

默认方向是：

> **在不损失必要 I/O 语义、正确性和真实执行能力的前提下，删除更多，而不是解释更多。**

## 非目标

Sluice 不以以下目标为项目身份：

- 大而全的 async framework；
- 通用 task / actor / future 生态；
- 为 backend 数量而建设 multi-backend framework；
- 自动 batching / autotuning / specialization 平台；
- 通用 observability / diagnosis framework；
- 通过增加 abstraction、文档、注释或形式化模型数量证明成熟度。

## 文档职责

```text
代码 / 构建定义        当前实现事实
docs/architecture.md  当前代码推导出的架构快照
docs/mission.md       本文：冻结的项目宗旨
docs/adr/             宗旨与重大架构决策的理由
测试 / fuzz / formal   correctness evidence
research/RESULTS.md    值得长期保留的研究结论
```
