# Sluice

一个实验性的 C++20 I/O 控制流库，围绕显式能力、可插拔后端和后端无关的
`Reader` / `Writer` 语义构建。

**当前状态：** v0.1.0 — Runtime Foundation (E10–E15) 已完成。
同步核心、异步运行时和同步原语已可生产。E16 Application Runtime 是下一个提议阶段。

## 为什么选择 Sluice

大多数 C++ I/O 在你编写任何业务逻辑之前就把你绑定到了特定后端——POSIX
文件、socket、内存。Sluice 反其道而行：你面向抽象的 `Reader`/`Writer`
接口编程，后端是你在程序边缘选择的**可插拔能力**。

这意味着：

- **用确定性故障注入测试**（`FaultReader`/`FaultWriter`）——无需文件系统，无需 mock 框架。
- **用统计收集包装器做基准测试**（`ObservedReader`/`ObservedWriter`）——零拷贝透传，统计字节数和调用次数。
- **不改调用点就能切换后端**——今天用 POSIX 文件，明天用 io_uring，测试用内存，全部通过同一个 `copy_all` 原语。

库受 Zig `std.Io` 启发，但适配了 C++20 风格。它**不是**移植——而是对同一显式能力哲学的 C++ 实现。

## 构建边界

| 库 | 描述 | 默认 |
|---------|-------------|---------|
| `sluice_core` | 同步核心：Result、Reader/Writer、copy、文件 I/O、WAL、BlockingIoPool | 始终构建 |
| `sluice_async` | 异步运行时：Scheduler、Fiber、同步原语、Completion、Future/Group/Batch | 可选；显式构建或作为 async tests/examples 的依赖 |
| `sluice_async_internal_testing` | 测试专用变体，包含确定性因果接缝 | 仅测试 |
| `sluice_experimental_uring` | 可选 io_uring 代码（无 liburing 时为 stub） | 默认关闭 |

## 5 分钟同步示例

```cpp
// 内存往返：无需文件系统，无需配置。
#include <sluice/memory_io_context.hpp>
#include <sluice/copy.hpp>
#include <cstdio>

int main() {
    sluice::MemoryIoContext ctx;

    auto r = ctx.open_reader("hello world");
    auto w = ctx.open_writer();

    sluice::copy_all(*r, *w);

    auto bytes = w->take_bytes();
    std::printf("%s\n", bytes.data());  // 输出: hello world
}
```

## 5 分钟异步示例

```cpp
// 异步运行时：提交一个操作，轮询完成，读取结果。
// (examples/async_foundation_quickstart.cpp — 针对公共头文件构建)
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <cstddef>
#include <cstdio>
#include <memory>

int main() {
    // FakeAsyncBackend 是确定性测试后端：auto_bytes(n) 使下一次 poll()
    // 以 n 字节完成每个未完成的操作。
    auto backend = std::make_unique<sluice::async::FakeAsyncBackend>();
    sluice::async::FakeAsyncBackend* raw = backend.get();
    raw->auto_bytes(8);

    sluice::async::AsyncIoContext ctx(std::move(backend));

    // 针对调用者拥有的 Completion 提交读取。
    sluice::async::Completion<std::size_t> c;
    std::byte buf[8]{};
    if (!ctx.submit_read(sluice::async::ReadOp{0, buf, 8, 0}, c).has_value())
        return 1;

    // poll() 非阻塞地收割完成；返回收割数量。
    if (ctx.poll() != 1) return 2;

    // 操作结果在 Completion 就绪后从中读取——而不是从 wait_one()/poll() 返回值读取。
    if (!c.ready()) return 3;
    auto r = c.result();
    if (!r.has_value() || r.value() != 8) return 4;

    std::printf("async quickstart: 读取 %zu 字节\n", r.value());
    return 0;
}
```

## 已实现能力

### 同步核心（`sluice_core`）

- `Result<T>` / `IoError` 错误模型
- `Reader` / `Writer` + `BufferedReader` / `BufferedWriter` / `ObservedReader` / `ObservedWriter` / `FaultReader` / `FaultWriter`
- `copy_all` 及 `CopyStrategy`（Scratch / BufferedFirst / Auto）
- `FileReader` / `FileWriter`（POSIX、位置型 I/O、向量 I/O）
- `BlockingIoContext` / `MemoryIoContext` 工厂抽象
- `BlockingIoPool`（有界 OS 线程执行助手）
- `SyncableWriter`（`sync_data` / `sync_all`）
- WAL 记录格式

### 异步运行时（`sluice_async`）

- `Scheduler`（M:N fiber 调度器、多工作线程、工作窃取）
- `Fiber`（上下文切换，x86_64 Linux）
- `WaitNode` / `WaitQueue`（E10）、`TimerRegistration` / deadline（E11）
- `Event`（E12-A）、`Semaphore`（E12-B）、`AsyncMutex`（E12-C）
- `AsyncCondition`（E12-D）、`AsyncQueue<T>`（E12-E）、`AsyncRwLock`（E12-F）
- `Select`（E13）— 多臂 Event/Timer select
- `CancelToken` / `CancelState` / `CancelGuard`（取消原语）
- `Future<T>`（E28）、`Group`（E29）、`Batch`（E30）
- `Completion<T>` / `AsyncIoContext` / `AsyncBackend`
- `FakeAsyncBackend`（确定性测试工具）
- `ThreadPoolBackend`（可移植，std::thread）

### 实验性

- `UringAsyncBackend` — Linux io_uring（通过 `--with-liburing` 构建门控；无 liburing 时为 stub）。仍为实验性；real-liburing 和非 Linux 验证证据有限。

## 构建与测试

```bash
xmake f -m debug                  # 配置（debug 模式）
xmake build sluice_core           # 构建同步核心
xmake build sluice_async          # 构建异步运行时
xmake build -g test               # 构建所有测试
xmake test                        # 运行所有测试
```

启用实验性 io_uring（需要 liburing）：

```bash
xmake f --with-liburing=true
xmake build -g experimental
```

### Sanitizer

```bash
xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test
xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test
```

## 验证模型

- **验收** — `public_api_acceptance`（公共头文件编译+运行探测）；`async_foundation_quickstart`（异步基础消费者）；未来的 E16 运行时验收消费者
- **单元/组件** — `xmake test -v`（每个 slice 的测试二进制）
- **确定性因果测试** — `SLUICE_ASYNC_INTERNAL_TESTING` 阶段接缝
- **Sanitizer 门控** — ASan、UBSan、TSan
- **形式化模型** — TLA+ 规范，位于 `spec/tla/`（按套件目录组织）

完整验证矩阵见 [`docs/verification/README.md`](docs/verification/README.md)。

## 项目结构

```
include/sluice/          公开头文件（core + async）
src/                     实现（core + async）
apps/                    真实面向用户的程序（见下方“应用程序”）
tests/                   正确性测试（每个 slice 一个二进制）
examples/                能力演示示例
bench/                   微基准测试（CSV 输出）
docs/                    架构、设计、历史、路线图、验证
  architecture/          当前架构文档
  design/                活跃提议设计
  adr/                   已接受的架构决策
  applications/          应用轨道计划与实践报告
  history/               收尾记录、实现计划、形式化设计、审查
  verification/          验证矩阵和脚本
  roadmap/               活跃未来工作
scripts/                 构建/分析辅助工具
xmake/                   构建配置
```

## 应用程序

`apps/` 下的程序完全构建在公开头文件之上（无测试缝隙、不包含
`src/`），与 `examples/`（能力演示）相区分：

- `sluice-copy` —— 有界异步安全文件复制（临时文件 + 原子 rename，可选持久化）
- `sluice-hash` —— 有界流式 SHA-256 文件哈希
- `sluice-grep` —— 有界流式字面量搜索
- `sluice-tail` —— 有界 last-N + follow 模式尾部跟踪（Ctrl-C 干净取消）

使用 `xmake build sluice-copy` 等命令构建；每个应用有自己的 README
（CLI、语义、资源上界与实测行为）。应用轨道的实践证据见
[docs/applications/file-tools-findings.md](docs/applications/file-tools-findings.md)。

## 已知限制

- `Evented` 执行策略需要 x86_64 Linux 且 `fiber_ctx::supported`。
- `io_uring` 需要 Linux + liburing（构建门控，默认关闭）。
- real-liburing 验证和非 Linux 可移植性证据仍然有限。
- `AsyncQueue<T>` v1 没有公开的 wait-epoch 取消 API。
- 向量 I/O 语义保守（在 EOF、错误或第一个正短结果时停止）。

## 文档链接

- [架构概览](docs/architecture/overview.md)
- [异步运行时](docs/architecture/async-runtime.md)
- [异步同步](docs/architecture/async-synchronization.md)
- [异步 I/O 基础](docs/architecture/async-io-foundation.md)
- [公开 API 参考](docs/api-reference-zh.md)
- [英文 API 参考](docs/api-reference.md)
- [验证矩阵](docs/verification/README.md)
- [路线图](docs/roadmap/README.md)
- [变更日志](docs/changelog.md)

## 许可证

详见 `LICENSE`。
