# Sluice

Sluice 是一个实验性的 C++20 explicit-I/O 运行时：应用代码通过后端无关的
公开 API 表达**想要什么** I/O，由可插拔的后端决定每个操作**如何**执行。

[English](README.md)

![Sluice 高层架构](docs/assets/architecture/sluice-high-level-layered-view.png)

*图中 `network` 与 `external-memory data structures` 属于未来的工作负载
方向，不是已实现的能力——见[项目状态](#项目状态)。*

## Sluice 是什么？

围绕一个想法构建的两个 C++20 库：应用 I/O 意图不应与后端执行方式耦合。

- **`sluice_core`** —— 同步核心。`Result<T>` / `IoError` 错误模型，
  `Reader` / `Writer` 接口及缓冲、故障注入、观测包装器，`copy_all`，
  POSIX 文件与位置型 I/O，持久化（`sync_data` / `sync_all`），WAL 记录
  格式，以及有界的 `BlockingIoPool` 辅助器。
- **`sluice_async`** —— 可选的异步运行时。M:N fiber `Scheduler`，协作式
  同步原语（`Event`、`Semaphore`、`AsyncMutex`、`AsyncCondition`、
  `AsyncQueue`、`AsyncRwLock`、`Select`），取消原语，
  `Future` / `Group` / `Batch`，以及 explicit-I/O 层：调用者持有的
  `Completion<T>` 操作、`ThreadPoolBackend` 和 `ApplicationRuntime`
  生命周期层。

错误是值（`Result<T>`），绝不是异常。设计受 Zig `std.Io` 启发并适配
C++20 惯用法——它不是移植。

## 为什么选择 explicit I/O？

```text
应用代码表达 I/O 意图
        ↓
Sluice 公开 API 拥有操作语义
        ↓
后端决定操作如何执行
```

- **后端无关接口** —— 用其他后端替换 `ThreadPoolBackend` 不需要改写
  应用代码。
- **显式操作** —— 读、写、同步都是以位置型操作描述符形式经公开 API
  提交的。
- **显式结果** —— 每个操作都解析为调用者持有的
  `Completion<T>` / `Result<T>`；失败是需要处理的值。
- **调用者持有缓冲区** —— 缓冲区与 Completion 在文档声明的请求生命周期
  内保持存活且地址稳定。
- **协作式取消** —— 取消是显式且协作式的；真实的 syscall 结果绝不会被
  改写成"已取消"。
- **有界资源** —— 请求容量、worker 数量、队列深度都是显式上界，而非
  意外增长。

同样的"表达 vs 执行"分离也用于测试：`MemoryIoContext` 与
`FakeAsyncBackend` 提供确定性的内存与故障注入测试，无需文件系统，也
无需 mock 框架。

## 快速开始

需要 Linux（同步核心也可在 macOS/WSL 使用）、[xmake](https://xmake.io)
和 C++20 编译器（推荐 Clang）。

```bash
git clone https://github.com/jnhu76/Sluice.git
cd Sluice
xmake f -m release -y
xmake build sluice-copy        # 或 sluice-hash、sluice-grep、sluice-tail
```

### 同步核心

```cpp
// 内存往返：无需文件系统，无需配置。
#include <sluice/memory_io_context.hpp>
#include <sluice/copy.hpp>

#include <cstddef>
#include <cstdio>
#include <string_view>
#include <vector>

std::vector<std::byte> to_bytes(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

int main() {
    sluice::MemoryIoContext ctx;
    ctx.seed("input.txt", to_bytes("hello world"));

    auto r = ctx.open_reader("input.txt");
    auto w = ctx.open_writer("output.txt");
    if (!r.has_value() || !w.has_value()) return 1;

    auto copied = sluice::copy_all(*r.value(), *w.value());
    if (!copied.has_value()) return 2;

    auto bytes = static_cast<sluice::MemoryWriter&>(*w.value()).take();
    std::printf("copied %llu bytes: %.*s\n",
                static_cast<unsigned long long>(copied.value()),
                int(bytes.size()),
                reinterpret_cast<const char*>(bytes.data()));
}
```

### 异步运行时

```cpp
// 一个运行时任务通过真实后端写文件，随后干净关闭。
#include <sluice/async/application_runtime.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <memory>

int main() {
    using namespace sluice::async;

    int fd = ::open("/tmp/sluice-quickstart.txt",
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;

    RuntimeBuilder builder;
    builder.backend(std::make_unique<ThreadPoolBackend>()).workers(1);
    auto built = builder.build();
    if (!built.has_value()) return 2;
    ApplicationRuntime& rt = *built.value();

    if (!rt.start().has_value()) return 3;

    auto task = rt.submit([&fd](RuntimeTaskContext& ctx) {
        static constexpr char msg[] = "hello explicit I/O\n";
        Completion<std::size_t> done;
        if (!ctx.submit_write(WriteOp{fd,
                                      reinterpret_cast<const std::byte*>(msg),
                                      sizeof msg - 1, /*offset=*/0},
                              done)
                 .has_value())
            return;
        (void)ctx.await_completion(done);  // 任务挂起，直到操作就绪
        auto n = done.result();            // Result<std::size_t>：错误是值
        if (n.has_value()) std::printf("wrote %zu bytes\n", n.value());
        ::close(fd);
    });
    if (!task.has_value()) return 4;

    rt.request_stop();               // 关闭准入，发布停止
    if (!rt.drain().has_value()) return 5;  // 等待任务与未完成 I/O
    if (!rt.join().has_value()) return 6;   // join 驱动线程，释放资源
    return 0;
}
```

更多可运行程序见 [examples/](examples/)——例如
`examples/runtime_acceptance.cpp` 仅依赖公开头文件即可走完整个运行时
生命周期。

## 今天能构建什么

完全构建在 Sluice 公开头文件之上的真实应用（无测试接缝、不包含私有
源码）：

- [`sluice-copy`](apps/sluice-copy/README.md) —— 有界异步安全文件复制
  （临时文件 + 原子 rename，可选持久化）
- [`sluice-hash`](apps/sluice-hash/README.md) —— 有界流式 SHA-256
- [`sluice-grep`](apps/sluice-grep/README.md) —— 有界流式字面量搜索
- [`sluice-tail`](apps/sluice-tail/README.md) —— 向后 last-N 扫描 +
  follow 模式，Ctrl-C 干净取消

应用轨道包含实测性能、内存上界、sanitizer 证据以及与系统工具的对比
——见
[docs/applications/file-tools-findings.md](docs/applications/file-tools-findings.md)。

## 后端

- `ThreadPoolBackend` —— 可移植的默认真实后端：基于 `std::thread` 的
  固定持久阻塞 I/O worker 池。
- `UringAsyncBackend` —— 实验性 Linux io_uring；由 `--with-liburing`
  构建门控，默认关闭。
- `FakeAsyncBackend` —— 确定性测试后端，提供精确、可编排的完成行为。

后端内部机制、一致性证据与 io_uring runbook 见
[docs/architecture/](docs/architecture/) 与
[docs/verification/](docs/verification/)。

## 项目状态

- **最新 tag 发布：** `v0.1.0` —— 运行时基础：同步核心、异步调度器与
  fiber 运行时、同步原语。
- **当前开发（master，超出 tag）：** 跨后端的 explicit-I/O 请求生命周期、
  `ApplicationRuntime` 层，以及第一批真实应用（copy / hash / grep /
  tail，2026-08 合并）。
- **实验性：** `UringAsyncBackend` —— real-liburing 验证证据仍依赖环境。
- **未实现：** 网络与外存数据结构（KV / B+ tree / LSM）。它们是未来的
  工作负载方向——用于产生 API 压力的证据生成器，不是当前能力——见
  [docs/applications/README.md](docs/applications/README.md)。

Sluice 是研究质量的实验性软件：平台支持以 Linux 为中心（fiber 调度器
需要 x86_64），除实测应用证据外不做任何性能声明。

## 文档

- [开发者文档枢纽](docs/README.md) —— Sluice 如何工作、如何修改
- [架构概览](docs/architecture/overview.md)
- [API 参考（中文）](docs/reference/api.zh-CN.md) ·
  [API Reference (English)](docs/reference/api.md)
- [应用轨道：真实工作负载带来的发现](docs/applications/README.md)
- [验证矩阵](docs/verification/README.md) —— sanitizer、确定性因果
  测试、形式化模型
- [路线图](docs/roadmap/README.md) · [变更日志](docs/changelog.md)

## 许可证

尚未声明许可证；在添加之前，仓库所有者保留所有权利。
