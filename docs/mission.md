# Sluice 宗旨

> **状态：FROZEN**
>
> 本文只冻结已经由 Sluice 研究结论支持的项目宗旨。它不承担代码风格、测试方法、注释规则、AI/LLM 上下文治理或一般工程规范。
>
> 研究证据以 `research/RESULTS.md` 与 Minimal Semantic Surface 研究结论为依据；没有研究支撑的工程偏好不进入本文。

## 一句话

> **Sluice 是一个显式 I/O 库：只暴露保持可观察 I/O 语义、正确性与真实资源边界所必需的信息；语义授权必须显式，执行机制与执行策略保持局部、可替换，并且只保留已经证明有价值的机制。**

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

Sluice 只应知道维持以下事实所必需的语义：

- 可观察 I/O effect；
- resource identity / lifetime；
- buffer participation 与 lifetime；
- completion / cancellation / deadline 等可观察异步语义；
- durability；
- 真实 resource bounds；
- 明确授予的 composition / transformation contract。

实现细节、后端机制、性能参数或 hint 不因“有用”而自动成为 public semantics。

### 2. 边界清晰

以下类别必须保持区分：

```text
SEMANTIC CONTRACT
CORRECTNESS AUTHORITY
RESOURCE BOUND
BACKEND CAPABILITY
EXECUTION POLICY
HINT / OBSERVATION
```

一个事实属于哪一层，必须由它定义的可观察行为、正确性责任、资源边界或合法 transformation 来证明，而不是由当前实现位置决定。

### 3. 权威显式

**信息不等于权限。**

只有额外 semantic contract 真正改变合法 transformation space 时，才承认新的 Semantic Authority。

长期保持：

```text
resource identity != fixed-resource optimization authority
operation grouping != fused / atomic admission authority
backend capability != semantic authority
hint / information != authority
```

### 4. 资源有界

当资源饱和会影响可观察行为或正确性时，边界必须被明确建模，例如 request capacity、in-flight/buffer budget 或 backend admission limit。

不要把不同资源压成一个模糊的 `concurrency=N`，也不要把纯性能参数误当成语义边界。

### 5. 执行可换

同一 semantic contract 不应绑定某一种 backend mechanism。

ThreadPool、io_uring 或其它执行机制可以不同，但 backend capability 不能反向定义公共语义；execution policy 默认留在 semantic core 之外。

### 6. 机制最小

研究已经多次得到同一个结果：**薄的局部机制可以解决问题时，不应升级成通用 framework。**

- Copy 的有效能力可由 thin local branch 表达，generic capability framework 未被证明有价值；
- Batch 没有获得新的 group-admission authority，新的 generalized Batch control layer 未被证明有价值；
- 性能研究要求先定位真实热点，再做局部优化，不能从架构故事出发扩张机制。

因此，新的 abstraction 或机制必须先证明真实语义、正确性、资源或执行价值，再获得长期存在资格。

## 固定研究护栏

### 显式信息不会自动产生 generic control

以下链条已经被研究否定为项目级假设：

```text
more explicit information
    -> more runtime control
    -> generic useful specialization / performance
```

Correctness、Semantic Authority 与 Performance 必须分别证明。

### Backend mechanism 不会自动变成 semantic contract

固定文件、registered resource、batch submit 等机制可以真实存在，但机制存在本身不证明新的高层 semantic authority。

### Execution policy 默认不属于 semantic core

例如：

```text
queue depth
worker count
chunk size
alignment
polling mode
registered resource use
preferred backend
```

它们可能显著影响性能，但除非改变调用者必须依赖的可观察 contract，否则不进入 semantic surface。

### Benchmark 结果不能直接扩张 API

研究已经看到：alignment 在 microbenchmark 中真实存在，但没有获得生产 copy workload 的控制面；chunk size 是更强的 workload lever，但也没有因此获得自动 public control surface。

性能结果必须先经过应用级证据，再讨论是否需要产品化；更不能由性能收益倒推语义授权。

## 非目标

基于现有研究，Sluice 不把以下方向作为项目宗旨：

- generic Control framework；
- 因为“信息更多”就构造全局 optimization planner；
- 因为 backend 有某机制就把机制暴露成 public semantics；
- 因为 operation 属于一个 group 就默认获得 fused / atomic admission；
- 为单个有效案例提前建设 generic capability framework；
- 把 host-local benchmark sweet spot 直接变成 semantic contract。

## 对后续架构审计的约束

任何 public/Core 概念都应首先归入一个主要类别：

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

1. 它定义了什么可观察行为？
2. 它拥有哪个 correctness invariant？
3. 它约束哪个真实资源？
4. 它授权了哪个原本不合法的 transformation？
5. 它是否只是 backend mechanism？
6. 它是否只是 execution policy / hint？
7. 删除或降级后，真实语义或正确性会失去什么？

如果这些问题无法给出研究与代码都能支持的答案，就不能仅凭“架构完整”保留该概念。
