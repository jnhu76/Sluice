# Sluice

Sluice 是一个正在收缩为小型、稳定工程基线的 C++20 I/O runtime/library。

[English](README.md)

## 当前仓库

```text
include/              公共 C++ 头文件
src/                  生产实现
apps/                 基于公共 API 的真实应用
research/RESULTS.md    保留的研究结论
xmake.lua、xmake/      构建配置
```

当前 C++ 实现是主要事实来源。`apps/` 被保留，因为它们是真实的库使用者。

旧 tests、benchmarks、examples、scripts、CI、docs、形式化模型、TLA+ 以及 research campaign 脚手架都已经从当前树删除。Git 历史就是归档。

## 系统形态

当前代码包含同步 I/O core 和可选异步 runtime。

同步部分提供 `Result<T>` / `IoError`、Reader/Writer 风格 I/O、文件与 positional I/O、copy helper 和 durability 操作。

异步部分包含显式 operation、caller-owned completion、有界 request state、scheduler/runtime、同步原语、取消和 backend execution。Linux io_uring 支持由当前构建配置决定。

真正保留下来的能力以当前 headers 和 build files 为准，不再由旧文档定义。

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

## Research

过去研究只保留 [research/RESULTS.md](research/RESULTS.md) 中的最终结论，不保留研究过程。

## 当前方向

```text
保证保留的 C++ 可用
    -> 删除不必要模块和抽象
    -> 固定精简后的架构
    -> 根据当前行为重建 tests
    -> 根据当前代码重写 docs
    -> 根据当前 C++ 重建形式化验证和 TLA+
    -> 建立 C++ <-> TLA+ 显式对应
    -> 只优化测量出的局部热点
```

正确性仍然是硬要求。性能优化放在架构稳定之后，并且只做细粒度、可测量的局部优化。

## License

Sluice 使用 [MIT License](LICENSE)。
