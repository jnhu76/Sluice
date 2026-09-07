# Sluice 设计宗旨与边界

本文定义 Sluice 的长期设计宗旨、边界和取舍原则。

它是**规范性设计准则**，不是当前实现快照、roadmap、实验记录或历史说明。当前代码“现在是什么”由代码与构建定义决定；`docs/architecture.md` 只负责从当前代码描述现状。本文回答的是另一个问题：**Sluice 应该成为什么，以及什么东西不应进入 Sluice。**

除非出现明确的新证据并经过人工架构决策，本文不应因为实现漂移、临时需求或某个后端的能力而被反向改写。

---

## 1. 一句话宗旨

> **Sluice 是一个显式 I/O 库：只暴露调用者真正必须依赖的 I/O 语义和资源边界，用最小的正确性机制忠实执行这些语义，并让执行机制可替换但不反过来污染语义。**

项目的长期原则固定为：

> **语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

对应英文短句：

```text
Minimal semantics.
Clear boundaries.
Explicit authority.
Named bounds.
Replaceable execution.
Minimum mechanism.
```

---

## 2. Minimal semantics —— 语义最少

公共 API 只应该暴露调用者为了正确表达 I/O intent 而必须知道的事实。

典型的必要语义包括：

- operation：read / write / durability 等明确操作；
- resource identity：实际参与操作的资源身份；
- buffer 与 lifetime boundary：谁拥有、借用何时结束；
- request lifecycle：提交、接受、终结、发布、释放之间的区别；
- completion：结果由谁持有、何时可观察、何时可复用；
- cancellation：取消意图、终结结果和胜负关系；
- durability：数据写入与持久化不是同一个语义；
- resource bound：会影响正确性或资源占用的重要上界。

公共 surface 的默认判断是**不增加**。

新增一个 public type、method、state、configuration 或 abstraction 前，必须回答：

> 如果调用者不知道这个概念，是否仍能准确、安全地表达它的 I/O 意图？

如果答案是 YES，则该概念通常不应进入公共语义面。

显式不等于“参数越多越好”。重要的是不要把调用者真正依赖的语义藏在 runtime 的默认猜测和隐含 policy 中。

---

## 3. Clear boundaries —— 边界清晰

Sluice 的职责必须分层，且不得因为当前实现方便而随意穿透。

```text
Application
    |
    | public I/O semantic boundary
    v
+------------------------------+
|      Sluice Public API       |
| operation / completion /     |
| cancel / durability / bounds |
+------------------------------+
               |
               v
+------------------------------+
|      Correctness Kernel      |
| ownership / lifecycle /      |
| terminal / publication       |
+------------------------------+
               |
       +-------+-------+
       |               |
       v               v
 Resource Bounds   Execution Mechanism
                       |
               ThreadPool / io_uring / ...
                       |
                       v
                      OS
```

边界规则：

1. **Application 不应依赖 Sluice 的 `detail/` 或实现内部件。** 出现这种依赖应视为 boundary leak，需要证明或消除，而不是自然扩大 public surface。
2. **Correctness Kernel 不应暴露为应用层 policy。** 内部状态存在不等于调用者必须知道它。
3. **Backend capability 不等于 public semantic authority。** 某个后端能够做某件事，不代表 Sluice 应该向上暴露一个新语义。
4. **Execution policy 不得伪装成 semantic contract。** chunk size、queue strategy、batching、worker topology 等只有在真正成为调用者可观察契约时才进入语义面。
5. **Observation / hint 可以跨层存在，但观察本身不授予控制权。** 看见更多信息不等于合法获得新的变换权限。

这些责任必须保持区分：

```text
SEMANTIC SURFACE
CORRECTNESS KERNEL
RESOURCE BOUNDS
BACKEND CAPABILITY
EXECUTION POLICY
OBSERVATION / HINT
```

---

## 4. Explicit authority —— 权威显式

关键 correctness decision 必须有明确 authority。

典型问题包括：

- 谁决定 request 被接受；
- 谁持有 request identity；
- 谁决定 terminal winner；
- 谁结束 buffer borrow；
- 谁发布 Completion；
- 谁关闭 waiter registration；
- cancel 与 completion 冲突时谁裁决；
- 谁负责 slot / resource accounting 的最终释放。

理想结构是：

> **一个 invariant 对应一个清楚的 authority。**

如果同一个 invariant 需要多个对象分别维护一部分状态、靠调用顺序和约定共同成立，这是优先审计对象。

“更多状态机”不是目标。目标是用更少、更集中的 authority 消除 silent / distributed protocol bugs。

---

## 5. Named bounds —— 资源有界

影响正确性、内存占用或调度压力的重要资源应当有明确上界，而不是默认为无限增长。

可能包括：

- request capacity；
- outstanding operation count；
- queue capacity；
- worker count；
- buffer / chunk size；
- task 或 waiter 的有界资源。

但 named bound 不等于“所有东西都要配置化”。

配置项本身也是复杂度。如果一个简单固定上界已经满足真实需求，就不应为了理论上的通用性增加 builder、option 或动态 policy。

---

## 6. Replaceable execution —— 执行可换

Sluice 的语义不应绑死在某一种执行机制上。

同一套 I/O contract 可以由不同 mechanism 执行，例如线程池或 io_uring；更换 mechanism 不应要求应用理解另一套 request / completion / cancellation 生命周期。

但“执行可换”**不等于必须维护多个 backend**，也不等于要建设一个大而全的 backend framework。

一个 backend implementation 只有在至少满足一项时才有长期生存资格：

- 服务真实 caller；
- 提供真实且需要的执行能力；
- 验证同一语义可以跨 mechanism 保持一致；
- 作为不可替代的 correctness / conformance witness。

没有真实价值的第二、第三、第四种实现不因为“架构完整”而自动获得保留资格。

---

## 7. Minimum mechanism —— 机制最小

Sluice 不为抽象本身付税。

任何新增或现存的：

```text
class
interface
backend
wrapper
queue
helper
state
macro
configuration
test seam
scheduler layer
```

都必须回答：

> **它现在买来了什么？**

只有以下理由通常足够：

1. 表达必要 I/O 语义；
2. 维护关键 correctness invariant；
3. 表达真实 resource bound；
4. 隔离真实存在的 execution difference；
5. 服务真实 caller，并且比直接代码更简单；
6. 为关键语义提供不可替代的验证能力。

以下理由默认不足：

- “以后可能有用”；
- “方便未来扩展”；
- “理论上可以有第二个实现”；
- “以前测试用过”；
- “框架看起来更完整”；
- “抽象层次更漂亮”；
- “某篇论文或旧设计这样做”。

默认处置只有：

```text
KEEP
SIMPLIFY
MERGE
INTERNALIZE
DELETE
```

不存在“先永久留着以后也许会用”的特殊类别。

---

## 8. Semantic authority —— 信息不等于权限

显式语义的第一价值是 correctness、control clarity、observability 和 composition；它**不会自动产生性能收益或优化权限**。

任何“因为知道了 G，所以允许 transformation T”的主张，都必须证明：

```text
C = baseline contract
T = candidate transformation
G = additional semantic grant

T 在 C 下并不天然合法或成立；
T 对所有满足 C + G 的程序才合法。
```

必须长期保持以下区分：

```text
resource identity != fixed-resource optimization authority
operation grouping != fused / atomic admission authority
backend capability != semantic authority
hint / information != authority
```

如果 transformation 在原 contract 下本来就合法，那么它可以是实现优化，但不能被包装成“新增显式语义带来的权限”。

---

## 9. Correctness、Performance、Semantic Authority 相互独立

Sluice 不再假设以下链条必然成立：

```text
explicit semantics
    -> generic control
    -> safety
    -> performance / specialization
```

这几条必须分别证明：

### Correctness

关注是否把危险从 silent / distributed failure 迁移到更机械的失败方式：

```text
UNREPRESENTABLE
STATICALLY_REJECTED
DYNAMICALLY_DETECTED / FAIL_FAST
DETERMINISTICALLY_REPRODUCIBLE
SILENT / UNDETECTED
```

同时必须统计 Sluice 自己引入的新 protocol bug class，而不是只统计它消除了什么。

### Performance

必要的 correctness / resource machinery 必须有可接受成本，但成本必须通过公平 baseline 和真实测量判断。

不能因为某层“语义上正确”就免除性能审查，也不能因为某个 microbenchmark 更快就反向扩张公共语义。

### Semantic Authority

只有真实改变合法 transformation space 的 contract 才授予额外 authority；不能用性能结果倒推语义必须存在。

---

## 10. Verification 是边界的一部分，不是另一套事实来源

关键 invariant 应尽可能由机械证据保护：

- deterministic unit / conformance tests；
- property-based tests；
- stateful fuzzing；
- ASan / TSan / UBSan；
- race / death / negative tests；
- TLA+ 或其它形式化模型；
- C++ 与模型的 correspondence / refinement witness。

TLA+ 只证明模型中的性质，**不会单独证明 C++ 实现正确**。关键模型必须能够指出对应的 C++ state / transition / authority，并通过实现侧证据验证这种对应关系不是空的。

测试和 formal 的目标不是增加数量，而是保护真正的 correctness boundary。

---

## 11. Context hygiene —— 不建立第二套现实

自然语言 context 也有成本。

Sluice 不应重新积累大量重复、历史化、互相竞争的事实来源。

长期职责固定为：

```text
代码 / 构建定义        当前实现事实
architecture.md        从当前代码推导出的现状快照
本文                    稳定的设计宗旨与边界
测试 / property / fuzz  可执行行为与 invariant 证据
形式化模型             抽象协议性质及其对应证据
research/RESULTS.md     值得保留的研究结论
```

普通代码注释只保留无法从代码直接看出的局部 WHY / invariant，不承担历史叙事、roadmap 或架构说明。

一个事实如果会频繁变化，不应复制到多个地方。

---

## 12. 非目标

Sluice 不以以下目标为项目身份：

- 大而全的 async framework；
- 通用 task / actor / future 生态；
- 为了 backend 数量而建设 multi-backend framework；
- 自动 batching / autotuning / specialization 平台；
- 通用 observability / diagnosis framework；
- 通过增加 abstraction 来追求“架构完整”；
- 通过增加文档、注释或形式化模型数量证明成熟度。

上层 convenience 可以存在，但只有在证明属于 Sluice 边界时才进入 core；否则应该位于应用层、独立 helper，或直接不存在。

---

## 13. 以后审代码的固定顺序

面对任何现存或新增模块，按以下顺序判断：

1. **Boundary**：调用者真的需要知道它吗？
2. **Semantics**：它表达了必要的 I/O contract 吗？
3. **Authority**：它维护了一个独立且必要的 correctness authority 吗？
4. **Bounds**：它表达或执行真实的 resource bound 吗？
5. **Execution**：它隔离了真实 execution difference 吗？
6. **Caller**：有真实 caller 吗？
7. **Verification**：它是否为关键 correctness 提供不可替代的证明能力？
8. **Cost**：能否用更少的状态、层、header、配置或 indirection 达成同一目标？

最后只允许得到：

```text
KEEP
SIMPLIFY
MERGE
INTERNALIZE
DELETE
```

这套判断优先于“现在代码已经这么写了”。

---

## 14. 文档稳定性

本文不是随实现同步变化的说明书。

- 实现变化应更新 `architecture.md`，而不是自动修改本文来为变化寻找理由；
- 新 backend、新 abstraction、新优化机制不能通过修改本文来获得合法性；
- 若未来确有证据表明这些设计宗旨本身需要变化，必须作为显式的人类架构决策处理，而不是普通重构、agent 自动修订或阶段性 roadmap 更新。

Sluice 的默认方向始终是：

> **在不损失必要 I/O 语义、正确性和真实执行能力的前提下，删除更多，而不是解释更多。**
