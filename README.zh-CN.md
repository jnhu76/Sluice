# Sluice

Sluice 是一个 C++20 显式 I/O runtime/library。当前仓库刻意保持精简：只保留生产代码、真实应用、当前架构/ADR 文档，以及一份过去研究结果的简要记录。

[English](README.md)

## 当前仓库

- `include/` — 公共 C++ 头文件
- `src/` — 生产实现
- `apps/` — 基于公共 API 的真实应用
- `docs/architecture/` — 当前架构说明
- `docs/adr/` — 保留的架构决策
- `research/RESULTS.md` — 过去研究阶段仍值得保留的结论
- `xmake.lua`、`xmake/` — 构建配置

旧 tests、benchmarks、examples、形式化模型、scripts、research campaign、生成型文档已经从当前树删除。Git 历史就是归档。

## 系统形态

Sluice 目前包含同步 I/O core 和可选的异步 runtime。

同步部分提供 `Result<T>` / `IoError`、Reader/Writer 风格 I/O、文件与 positional I/O、copy helper 和 durability 操作。

异步部分提供显式 operation、caller-owned completion、有界 request state、scheduler/fiber runtime、同步原语、取消以及 runtime 生命周期管理。`ThreadPoolBackend` 是普通 blocking-I/O backend；Linux io_uring 支持由 liburing 构建选项控制。

`include/` 和 `src/` 中的实现代码是权威来源。文档只描述当前代码，不再维护一套平行的历史解释体系。

## 应用

当前保留四个真实应用：

- [`sluice-copy`](apps/sluice-copy/README.md) — 有界文件复制
- [`sluice-hash`](apps/sluice-hash/README.md) — 流式 SHA-256
- [`sluice-grep`](apps/sluice-grep/README.md) — 流式文本匹配
- [`sluice-tail`](apps/sluice-tail/README.md) — last-N 与 follow 模式

## 构建

Sluice 使用 [Xmake](https://xmake.io)，需要 C++20 编译器。

仓库瘦身期间，构建配置也会同步收缩；当前可用 target 以 `xmake.lua` 和 `xmake/` 为准。

## 文档

- [Architecture](docs/architecture/README.md)
- [Architecture Decision Records](docs/adr/README.md)
- [历史研究结果](research/RESULTS.md)

## 当前方向

Sluice 不再由大规模 research campaign 驱动。接下来只做几件事：

1. 先保证保留下来的 C++ 实现可用；
2. 删除不必要模块和构建负担；
3. 根据当前代码重新建立 tests；
4. 根据当前 C++ 重新建立形式化验证和 TLA+；
5. 架构稳定后，只针对测量出的局部热点做细粒度优化。

旧 campaign 名称、issue 时间线、research 脚手架和旧形式化模型都不再作为当前 context 继承。

## License

Sluice 使用 [MIT License](LICENSE)。
