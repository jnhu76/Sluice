# Sluice

Sluice 是一个实验性的 C++20 explicit-I/O 运行时：应用代码通过后端无关的
公开 API 表达**想要什么** I/O，由可插拔的后端决定每个操作**如何**执行。

[English](README.md)

![Sluice 高层架构](docs/assets/architecture/sluice-high-level-layered-view.png)

*图中 `network` 与 `external-memory data structures` 属于未来的工作负载
方向，不是已实现的能力。`ThreadPoolBackend (portable)` 表示它采用
`std::thread`、不依赖 liburing 的实现策略，并不表示跨平台验证已经完成——
见[项目状态](#项目状态)。*

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

I/O 失败通过值（`Result<T>` / `IoError`）表达，而不是由 I/O API 通过异常
报告。值构造、内存分配或用户代码自身产生的普通 C++ 异常仍遵循普通 C++
语义。设计受 Zig `std.Io` 启发并适配 C++20 惯用法——它不是移植。

## 为什么选择 explicit I/O？

```text
应用代码表达 I/O 意图
        ↓
Sluice 公开 API 拥有操作语义
        ↓
后端决定操作如何执行
```

- **后端无关接口** —— 用另一个兼容后端替换 `ThreadPoolBackend` 时，不需要
  改写操作层的应用逻辑。
- **显式操作** —— 读、写、同步都是以位置型操作描述符形式经公开 API
  提交的。
- **显式结果** —— 每个操作都解析为调用者持有的
  `Completion<T>` / `Result<T>`；I/O 失败是需要处理的值。
- **调用者持有缓冲区** —— 缓冲区与 Completion 在文档声明的请求生命周期
  内保持存活且地址稳定。
- **协作式取消** —— 取消是显式且协作式的；真实的 syscall 结果绝不会被
  改写成“已取消”。
- **有界资源** —— 请求容量、worker 数量、队列深度都是显式上界，而非
  意外增长。

同样的“表达 vs 执行”分离也用于测试：`MemoryIoContext` 与
`FakeAsyncBackend` 提供确定性的内存与故障注入测试，无需文件系统，也
无需 mock 框架。

## 快速开始

当前验证以 Linux/WSL 为主。你需要 [xmake](https://xmake.io) 和 C++20
编译器（推荐 Clang）。同步核心面向 POSIX，也包含兼容 macOS 的代码路径，
但更广泛的非 Linux 可移植性证据仍不完整。

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

#include <cstddef>
#include <cstdio>
#include <memory>

int main() {
    using namespace sluice::async;

    RuntimeBuilder builder;
    builder.backend(std::make_unique<ThreadPoolBackend>()).workers(1);
    auto built = builder.build();
    if (!built.has_value()) return 1;
    ApplicationRuntime& rt = *built.value();

    if (!rt.start().has_value()) return 2;

    int fd = ::open("/tmp/sluice-quickstart.txt",
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        (void)rt.shutdown();
        return 3;
    }

    auto task = rt.submit([fd](RuntimeTaskContext& ctx) {
        static constexpr char msg[] = "hello explicit I/O\n";
        Completion<std::size_t> done;
        if (!ctx.submit_write(WriteOp{fd,
                                      reinterpret_cast<const std::byte*>(msg),
                                      sizeof msg - 1, /*offset=*/0},
                              done)
                 .has_value())
            return;
        (void)ctx.await_completion(done);  // 任务挂起，直到操作就绪
        auto n = done.result();            // Result<std::size_t>：I/O 错误是值
        if (n.has_value()) std::printf("wrote %zu bytes\n", n.value());
    });
    if (!task.has_value()) {
        ::close(fd);
        (void)rt.shutdown();
        return 4;
    }

    rt.request_stop();
    auto drained = rt.drain();
    if (!drained.has_value()) {
        (void)rt.shutdown();
        ::close(fd);
        return 5;
    }

    auto joined = rt.join();
    ::close(fd);
    if (!joined.has_value()) return 6;
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

- `ThreadPoolBackend` —— 默认真实后端：使用 `std::thread` 实现固定数量、
  持久存在的阻塞 I/O worker，不依赖 liburing。
- `UringAsyncBackend` —— 实验性 Linux io_uring；由 `--with-liburing`
  构建门控，默认关闭。
- `FakeAsyncBackend` —— 确定性测试后端，提供精确、可编排的完成行为。

后端内部机制、一致性证据与 io_uring runbook 见
[docs/architecture/](docs/architecture/) 与
[docs/verification/](docs/verification/)。

## 项目状态

- **参考基线：** `v0.0.1` —— 为六域审计战役冻结的 explicit-I/O 产品面：
  同步核心、异步调度器与 fiber 运行时、同步原语、后端集合，以及第一批
  真实应用（copy / hash / grep / tail）。见 #227。
- **开发在 master 上继续**，超出 tag——见
  [路线图](docs/roadmap/README.md)。
- **实验性：** `UringAsyncBackend` —— real-liburing 验证证据仍依赖环境。
- **未实现：** 网络与外存数据结构（KV / B+ tree / LSM）。它们是未来的
  工作负载方向——用于产生 API 压力的证据生成器，不是当前能力——见
  [docs/applications/README.md](docs/applications/README.md)。

Sluice 是研究质量的实验性软件：平台支持以 Linux 为中心（fiber 调度器
需要 x86_64），除实测应用证据外不做任何性能声明。

## 文档

- [开发者文档枢纽](docs/README.md) —— Sluice 如何工作、如何修改
- [架构概览](docs/architecture/overview.md)
- [API 参考（英文 canonical）](docs/reference/api.md)
- [Reference 索引](docs/reference/README.md) —— 包含中文伴随版以及同步维护规则
- [应用轨道：真实工作负载带来的发现](docs/applications/README.md)
- [验证矩阵](docs/verification/README.md) —— sanitizer、确定性因果
  测试、形式化模型
- [路线图](docs/roadmap/README.md) · [变更日志](docs/changelog.md)

## 许可证

Sluice 采用 [MIT License](LICENSE)。
