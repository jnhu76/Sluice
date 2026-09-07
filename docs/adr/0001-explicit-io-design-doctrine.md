# ADR-0001：显式 I/O 的最小语义与设计边界

- **状态**：Accepted / Frozen
- **范围**：Sluice 的显式 I/O 研究结论
- **证据来源**：`research/RESULTS.md` 与 Minimal Semantic Surface 研究结论

## Context

Sluice 早期研究曾尝试一条更强的推理链：

```text
more explicit I/O information
    -> more runtime control
    -> safety / specialization / performance
```

连续实验没有支持这条 generic control thesis。

Fixed-file、Copy 与 Batch 三组实验给出了三个关键负面/限定性结果：

1. **Fixed-file**：resource identity 与 backend mechanism 都是真实能力，但没有建立新的 Sluice-specific semantic authority，也没有建立通用性能 premium。
2. **Copy**：显式 composed operation 可以成为合法 transformation boundary，但有效实现只需要 thin local branch；generic capability framework 没有被证明有价值。
3. **Batch**：知道若干操作属于同一 Batch，不等于拥有 fused / atomic group-admission authority；group information 不产生 group authority。

性能与 buffer 实验进一步说明：

- small-I/O 下固定 per-operation machinery 成本真实存在；
- 较大 I/O 可以摊薄这些成本；
- alignment 在 microbenchmark 中存在，但没有获得生产 copy workload 的控制面；
- chunk size 是更强的 workload lever，但也没有因此获得自动 public semantic status；
- host-local 测量结果不能直接升级成通用架构结论。

因此，项目需要从“暴露更多信息、获得更多 control”转向另一个问题：

> **一个显式 I/O 库真正必须知道多少语义，哪些事实必须保持在 semantic boundary 之外？**

## Decision

Sluice 的设计宗旨冻结为：

> **只暴露保持可观察 I/O 语义、正确性与真实资源边界所必需的信息；语义授权必须显式，执行机制与执行策略保持局部、可替换，并且只保留已经证明有价值的机制。**

项目采用六条长期原则：

```text
Minimal semantics.
Clear boundaries.
Explicit authority.
Named bounds.
Replaceable execution.
Minimum mechanism.
```

中文固定表述：

> **语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

规范性简版见 [`../mission.md`](../mission.md)。

## 1. Minimal semantics —— 从“更多 explicit”转向“最小充分语义”

Minimal Semantic Surface 研究把问题从：

```text
How much intent can Sluice expose?
How much control can Sluice obtain?
```

改成：

```text
What must Sluice know?
What must Sluice NOT know?
Which facts are semantic contracts?
Which facts are mechanisms or policy?
Which contracts actually enlarge the legal transformation space?
```

显式 I/O 仍然必须表达调用者真正依赖的事实，例如：

- resource identity / lifetime；
- buffer participation / ownership lifetime；
- Read / Write / durability 等 I/O effect；
- accepted / terminal publication / cancellation / deadline / reuse 等可观察 async semantics；
- 会影响正确性或可观察饱和行为的 resource bounds；
- 真正授予额外 transformation freedom 的 composition contract。

但“实现中存在”不等于“应进入 public semantic surface”。

## 2. Clear boundaries —— 六类事实不能互相冒充

研究要求长期区分：

```text
SEMANTIC_CONTRACT
CORRECTNESS_AUTHORITY
RESOURCE_BOUND
BACKEND_CAPABILITY
EXECUTION_POLICY
HINT / OBSERVATION
```

它们分别回答不同问题：

- **Semantic Contract**：调用者能够观察或依赖什么？
- **Correctness Authority**：哪个事实必须由 runtime 维护才能保证语义正确？
- **Resource Bound**：哪个独立资源有可观察或 correctness-relevant 的上界？
- **Backend Capability**：底层到底能做什么？
- **Execution Policy**：在合法语义范围内，runtime 选择怎么做？
- **Hint / Observation**：系统知道什么，但并未因此获得额外权限？

架构不能因为当前 backend、benchmark 或实现布局方便，就把这些类别合并。

## 3. Explicit authority —— 信息不等于合法 transformation

这是研究中最重要的纠正。

长期保持：

```text
Information
    != Semantic Contract
    != Semantic Authority
    != Backend Mechanism
    != Unique Incremental Value
    != Material Performance
```

任何新的 semantic-authority claim 必须满足：

```text
C = baseline contract
T = candidate transformation
G = additional semantic grant

T 在 C 下并不天然合法或成立；
T 对所有满足 C + G 的程序才合法。
```

因此：

```text
resource identity != fixed-resource optimization authority
operation grouping != fused / atomic admission authority
backend capability != semantic authority
hint / information != authority
```

Copy 是正向但有限的例子：composed operation 可以成为合法 transformation boundary；但 authority、unique value 与 performance materiality 仍然是三件不同的事。

Batch 是负向控制：group membership 本身没有授予 group-admission authority。

## 4. Named bounds —— 资源边界是语义的一部分，但不能被泛化

显式 I/O 必须能够命名那些饱和后会改变可观察行为或 correctness 的真实资源边界，例如：

```text
request capacity
buffer / in-flight budget
backend admission limit
```

但研究同时要求不要把不同资源折叠成模糊的 `concurrency=N`，也不要把纯性能参数误升格成 semantic bound。

资源边界的价值来自它约束了真实资源与可观察行为，而不是“可配置”本身。

## 5. Replaceable execution —— contract 与 mechanism 分离

研究支持的目标不是“拥有很多 backend”，而是：

> **public semantic contract 不应由某个 backend 的实现细节定义。**

同一语义可以由 threaded blocking、io_uring 或其它机制实现；执行 mechanism 可以变化，但其能力不能反向污染 semantic surface。

Fixed-file 实验给出了明确例子：backend 固定文件机制是真实 capability，但它不会自动变成高层 semantic authority。

因此 replaceability 是 boundary discipline，不是 backend 数量 KPI。

## 6. Minimum mechanism —— 只有被证据赚到的层才保留

这是从多个实验共同得到的工程结论。

### Copy

研究确认 explicit Copy 可以提供合法 transformation boundary，但具体有效机制可以是一个 **thin local branch**。

结论：

> **generic capability framework 未被证明有价值。**

### Batch

研究确认 primitive consecutive submits 已能从 backend 获得 transport batching，而当前 Batch contract 没有 group admission authority。

结论：

> **新的 generalized Batch control layer 未被证明有价值。**

### Performance / optimization

保留下来的优化规则是：

1. 先 profile 当前实现；
2. 优化测量到的局部热点，而不是架构故事；
3. microbenchmark effect 不能直接进入 public control surface；
4. 一个 thin local mechanism 能解决观察到的问题时，不在没有第二个真实用例前泛化。

因此，Sluice 的 subtraction 原则不是审美偏好，而是研究结果的直接后果：

> **局部机制足够时，不建设通用层。**

## 7. Execution policy 默认留在 semantic core 之外

Minimal Semantic Surface 研究明确把以下内容视为默认的 execution policy / mechanism，而不是 application semantics：

```text
queue depth
worker count
chunk size
alignment preference
polling mode
registered-file use
registered-buffer use
preferred backend
cache strategy
measured sweet spot
```

这些因素可以非常重要。

例如研究显示 chunk size 对应用吞吐量的影响远大于 alignment treatment；但这并没有自动授权把 chunk size 变成新的 semantic contract。

同理，alignment 的 microbenchmark 信号没有获得生产 copy workload 的 control knob。

结论：

> **性能重要，不等于性能参数属于 public semantics。**

## 8. Correctness、Semantic Authority、Performance 独立

Sluice 不再接受以下项目级假设：

```text
explicit semantics
    -> generic control
    -> safety
    -> performance / specialization
```

三条证据线必须独立：

### Correctness

语义与 resource/lifetime boundary 是否足以保持正确行为？

### Semantic Authority

额外 contract 是否真的让一个原本不合法的 transformation 变得合法？

### Performance

在真实 workload 与公平 baseline 下，这种机制是否值得？

一个维度的成功不能替另一个维度作证。

## 9. 对架构审计的直接含义

后续 header / Core / API 审计不应先问“这个 abstraction 设计得漂不漂亮”，而应先分类：

```text
SEMANTIC_CONTRACT
CORRECTNESS_AUTHORITY
RESOURCE_BOUND
BACKEND_CAPABILITY
EXECUTION_POLICY
HINT / OBSERVATION
LEGACY / UNJUSTIFIED
```

然后回答：

```text
A. 它定义了什么可观察行为？
B. 它拥有哪个 correctness invariant？
C. 它约束哪个独立资源？
D. 它授权哪个原本不合法的 transformation？
E. 它是否只是 backend capability？
F. 它是否只是 execution policy / hint？
G. 同一事实能否安全地从更低层获得？
H. 删除、降级或 internalize 后会失去什么？
```

允许的方向包括：

```text
KEEP_CORE
KEEP_INTERNAL
DEMOTE_TO_CAPABILITY
DEMOTE_TO_POLICY
DEMOTE_TO_HINT
REWRITE_CONTRACT
REMOVE
RESEARCH_REQUIRED
```

这里不预判任何当前模块必须删除；研究只提供审判标准。

## 10. Rejected alternatives

### “更多 explicit information 自然带来更多 control”

拒绝。G1-Control 没有被建立。

### “Backend 有这个机制，所以 public API 应该表达它”

拒绝。Backend capability != semantic authority。

### “Batch 知道 operations 属于一组，所以 runtime 可以 fused admission”

拒绝。当前 Batch contract 没有授予 group-admission semantics。

### “一个案例有效，就应该抽象成 generic framework”

拒绝。Copy 只赚到了 thin local mechanism，没有赚到 generic capability framework。

### “某个 benchmark 参数更快，所以应该成为 semantic field”

拒绝。Alignment 与 chunk-size 研究都表明 performance lever 与 semantic contract 必须分开。

### “一次 host 上的结果可以直接成为通用性能结论”

拒绝。当前研究明确保留了跨 host 外部有效性限制。

## Consequences

采用本 ADR 后：

- public semantic surface 默认保持最小；
- backend mechanism、execution policy 与 hint 默认不能升级为 semantic contract；
- composition / intent 字段必须说明它真正授予的 observable semantic grant；
- resource bound 必须对应真实独立资源；
- generalized abstraction 必须由多个真实证据点赚到，而不是预付未来扩展成本；
- 性能优化优先做局部、可测量机制；
- architecture subtraction 以删除错误分类和未被证明的层为目标，而不是追求 LOC 数字。
