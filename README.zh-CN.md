# Sluice

Sluice 是一个 C++20 显式 I/O library/runtime。

它的目标不是做大而全的异步框架，而是：**只暴露调用者真正必须依赖的 I/O 语义和资源边界，用尽可能小的正确性机制忠实执行这些语义，并让执行机制可以替换但不能反过来污染公共语义。**

[English](README.md)

## 设计宗旨

Sluice 的长期原则固定为：

> **语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

对应：

```text
Minimal semantics.
Clear boundaries.
Explicit authority.
Named bounds.
Replaceable execution.
Minimum mechanism.
```

规范性设计准则见 [`docs/design-doctrine.md`](docs/design-doctrine.md)。

这份 doctrine 定义 **Sluice 应该成为什么，以及什么东西不应进入 Sluice**。当前 C++ 与构建定义负责回答“仓库今天实际上做什么”；架构文档只从当前代码描述现状，不能反过来覆盖代码事实，也不能扩大 doctrine。

## 当前仓库

```text
include/              C++ 头文件
src/                  生产实现
apps/                 真实应用
xmake.lua、xmake/      构建配置
docs/                 稳定设计宗旨与代码推导出的架构文档
research/RESULTS.md    保留的研究结论
```

旧 campaign 脚手架、过时 tests/benchmarks/examples、旧 CI、旧文档和旧形式化模型都不是当前设计 authority。Git 历史只作为归档存在。

## 当前实现

现存代码包含同步 I/O core 和可选异步 runtime。

同步部分提供 `Result<T>` / `IoError`、Reader/Writer 风格 I/O、文件与 positional I/O、copy helper、durability 操作及相关工具。

异步部分包含显式 operation、caller-owned completion、有界 request state、scheduler/runtime、取消、同步设施和 backend execution。

这些实现细节仍然可以继续减肥。任何 abstraction、backend、helper、state 或 public type，只有在它确实买来了以下价值时才有长期生存资格：

- 必要 I/O 语义；
- 关键 correctness invariant；
- 真实 resource bound；
- 真实 execution difference；
- 真实 caller；
- 不可替代的验证价值。

“以后可能有用”“架构更完整”“理论上可以有第二个实现”都不是默认保留理由。

## 边界规则

Application 应依赖公共 semantic boundary，而不是实现内部件。Backend capability、observation、hint 或 execution policy 不会因为存在就自动升级为 public semantic authority。

显式语义也不自动意味着 generic optimization、specialization 或更高性能。Correctness、performance、semantic authority 必须分别证明，不能互相代替。

## 应用

- [`sluice-copy`](apps/sluice-copy/README.md)
- [`sluice-hash`](apps/sluice-hash/README.md)
- [`sluice-grep`](apps/sluice-grep/README.md)
- [`sluice-tail`](apps/sluice-tail/README.md)

## 构建

Sluice 使用 [Xmake](https://xmake.io)，需要 C++20 编译器。

```bash
git clone https://github.com/jnhu76/Sluice.git
cd Sluice
xmake f -m release -y
xmake
```

当前可用 target 以 `xmake.lua` 和 `xmake/` 为准。

## Correctness 与验证

正确性是硬要求，但验证机制也必须服从最小原则。Deterministic tests、property tests、fuzzing、sanitizers 和 formal model 应围绕真实 invariant 生长，而不是再形成一套平行的大框架。

形式化模型证明的是模型本身，不会单独证明 C++ 实现正确。重要模型必须建立明确的 C++ state / transition / authority 对应关系，并用实现侧证据证明这种对应不是空的。

## Research

过去研究只保留 [research/RESULTS.md](research/RESULTS.md) 中值得长期保留的结论，不保留 campaign 过程。

## License

Sluice 使用 [MIT License](LICENSE)。
