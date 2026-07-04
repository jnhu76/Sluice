# Sluice

Sluice 是一个实验性的 C++ I/O 控制流库，提供显式能力、可插拔后端和后端无关的 Reader/Writer 语义。

## 为什么选择 Sluice

大多数 C++ I/O 在你编写任何业务逻辑之前就把你绑定到了特定后端——POSIX 文件、socket、内存。Sluice 反其道而行：你面向抽象的 `Reader`/`Writer` 接口编程，后端是你在程序边缘选择的**可插拔能力**。

这意味着：

- **用确定性故障注入测试**（`FaultReader`/`FaultWriter`）——无需文件系统，无需 mock 框架。
- **用统计收集包装器做基准测试**（`ObservedReader`/`ObservedWriter`）——零拷贝透传，统计字节数和调用次数。
- **不改调用点就能切换后端**——今天用 POSIX 文件，明天用 io_uring，测试用内存，全部通过同一个 `copy_all` 原语。

库受 Zig `std.Io` 启发，但适配了 C++20 风格。它**不是**移植——而是对同一显式能力哲学的 C++ 实现。

## 5 分钟体验

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

真实后端版本见 `examples/mvp_copy_pipeline.cpp`。

## 核心概念

### Reader / Writer

两个基本抽象。其他一切——缓冲、可观测性、故障注入、后端——都是围绕其中之一的包装器。

| 概念 | 接口 | 关键方法 |
|---|---|---|
| Reader | `sluice::Reader` | `read_some()`, `read_exact()`, `skip_exact()` |
| Writer | `sluice::Writer` | `write_some()`, `write_all()`, `flush()` |
| 向量 I/O | (Reader/Writer 上) | `read_vec()`, `write_vec()`, `write_all_vec()` |

### 包装器组合

包装器持有内部 reader/writer 的引用并委托给它。最外层包装器由调用者驱动；每层添加一个能力：

```cpp
sluice::FileWriter      file("/tmp/out.bin");   // 原始 POSIX 写入
sluice::ObservedWriter  observed(file, stats);  // 统计字节和调用
sluice::BufferedWriter  buffered(observed, buf); // 添加缓冲

buffered.write_all(bytes);  // 流向: buffered → observed → file
buffered.flush();           // 统计反映内部调用
```

可用包装器：

| 包装器 | 添加的能力 |
|---|---|
| `BufferedReader` / `BufferedWriter` | 接口级缓冲 |
| `ObservedReader` / `ObservedWriter` | 透明的字节/调用计数 |
| `FaultReader` / `FaultWriter` | 确定性短 I/O 和错误注入 |

### Copy 原语

```cpp
sluice::copy_all(reader, writer);                      // 简单
sluice::copy_all(reader, writer, scratch);             // 带暂存缓冲
sluice::copy_all(reader, writer, scratch, options,     // 带策略和统计
                 &stats, &decision);
```

`copy_all` 有**缓冲快速路径**：当 reader 是 `BufferedReader`（实现了 `BufferedReadable` 能力）时，它先排空已缓冲的字节，再回退到暂存读取路径。这是可选的、可观测的，不是性能声明。

### Copy 策略

策略层（SLUICE-CORE-007）使 copy 路径选择变得显式：

| 策略 | 行为 |
|---|---|
| `CopyStrategy::Scratch` | 总是读入暂存缓冲再写入（安全回退） |
| `CopyStrategy::BufferedFirst` | 先尝试缓冲快速路径；失败则回退到暂存 |
| `CopyStrategy::Auto` | 让 `copy_all` 基于运行时能力探测决定 |

### 后端能力边界

`sluice::IoContext` 是打开 `Reader`/`Writer` 句柄的抽象工厂：

```cpp
sluice::BlockingIoContext ctx;           // POSIX 文件
sluice::MemoryIoContext  ctx;            // 内存（测试、示例）

auto reader = ctx.open_reader(path);
auto writer = ctx.open_writer(path);
sluice::copy_all(*reader, *writer);
```

直接 `FileReader`/`FileWriter` 构造函数在简单场景仍然有效。上下文抽象的存在是为了让未来后端（io_uring、网络 socket）可以插入而不改变调用点。

### 位置型 I/O

`FileReader`/`FileWriter` 支持位置型操作（`pread`/`pwrite`/`preadv`/`pwritev`），**不会**修改共享的文件偏移量——在同一 fd 上不同偏移量的两个位置操作是独立的：

```cpp
sluice::FileWriter writer(path);
sluice::FileReader reader(path);

writer.write_at(buf1, offset1);       // pwrite——游标不受影响
writer.write_at(buf2, offset2);       // 独立操作，无需 lseek
reader.read_at(dst, offset);          // pread
reader.read_at_exact(dst, offset);    // 循环直到填满 dst
writer.write_all_at(src, offset);     // 循环处理短写入
```

向量位置型形式（`read_vec_at`/`write_vec_at`/`write_all_vec_at`）也可用。

### BlockingIoPool

**有界 OS 线程执行助手**，用于卸载阻塞工作。它*不是*异步运行时——在固定数量的 `std::thread` 工作线程上运行可调用对象：

```cpp
#include <sluice/blocking_io_pool.hpp>

auto pool = sluice::BlockingIoPool::create(
    sluice::BlockingIoPoolOptions{.worker_count = 4, .max_queue_depth = 64});

auto task = pool->submit([] { return do_heavy_io(); });
auto result = task.get();  // 阻塞直到完成，返回值或重新抛出异常

pool->shutdown();  // 停止接受任务，排空已提交的工作，汇合工作线程
```

关键特性：有界队列（通过 `try_submit` 提供背压）、`shutdown()` 后拒绝提交、任务异常通过 `task.get()` 传播、实例拥有（无全局状态）、sanitizer 干净。TLA+ 形式化验证覆盖准入、生命周期和完成（`spec/tla/BlockingIoPool.tla`）。

### Flush / sync / 持久性

三个不同的操作，三个不同的契约：

| 操作 | 保证 |
|---|---|
| `flush()` | 将缓冲字节排空到内部 writer。**不保证持久性。** |
| `sync_data()` | `fdatasync`——数据完整性（通过 `SyncableWriter` 能力） |
| `sync_all()` | `fsync`——数据 + 元数据完整性（通过 `SyncableWriter` 能力） |

`BufferedWriter::flush()` 排空脏字节。`FileWriter::flush()` 是文档化的空操作（本阶段无 `fsync`）。析构函数**不会** flush——失败的 flush 无法从析构函数报告。

组合规则：在你使用的**最外层** writer 上调用 `flush()`：

```cpp
buffered.write_all(bytes);
buffered.flush();   // 排空 buffered → observed → file，然后 file.flush()
```

### WAL 记录格式

`sluice::wal` 提供最小的预写日志记录格式，用于练习 writer 语义：

```cpp
sluice::wal::write_record(writer, data);         // 写入一条记录
auto got = sluice::wal::read_record(reader);     // 读回
sluice::wal::write_record_vec(writer, iovecs);   // 向量变体
```

## 构建

项目使用 [xmake](https://xmake.io)。

```sh
xmake f -m debug                  # 配置（debug 模式）
xmake build sluice_core           # 构建静态库
xmake build -g test               # 构建所有测试
xmake test                        # 运行所有测试
xmake build -g examples           # 构建示例
```

启用实验性 io_uring（需要 liburing）：

```sh
xmake f --with-liburing=true
xmake build -g experimental
```

### Sanitizer 和内存检查

项目提供五种分析模式：

| 模式 | 标志 | 检测内容 |
|---|---|---|
| `asan` | `-fsanitize=address` | 越界、释放后使用、双重释放 |
| `tsan` | `-fsanitize=thread` | 数据竞争、死锁 |
| `ubsan` | `-fsanitize=undefined` | 有符号溢出、空指针解引用、对齐问题 |
| `asanubsan` | ASan + UBSan 组合 | 同时检测上述两类 |
| `valgrind` | 调试符号 + `-O3` | 内存泄漏、无效读写（运行时检查） |

**Sanitizer 测试**（编译时插桩）：

```sh
xmake f -m asan && xmake build -g test && xmake run -g test
xmake f -m tsan && xmake build -g test && xmake run -g test
xmake f -m ubsan && xmake build -g test && xmake run -g test
xmake f -m asanubsan && xmake build -g test && xmake run -g test
```

**Valgrind**（运行时包装器——用调试符号构建，然后在 valgrind 下运行）：

```sh
xmake f -m valgrind && xmake build -g test
valgrind --leak-check=full --error-exitcode=1 build/linux/x86_64/valgrind/<test_name>_test
```

**切换到 Clang**（如果 sanitizer 在 g++ 下有问题）：

```sh
xmake f --toolchain=clang -c && xmake build
xmake f --toolchain=clang -m asan -c && xmake build   # Clang + ASan
```

## 项目结构

```
include/sluice/          公开头文件
src/                     实现
tests/                   正确性测试（每个 slice 一个二进制）
examples/                可运行示例
bench/                   微基准测试（CSV 输出）
docs/                    设计文档、审计、决策记录
scripts/                 构建/分析辅助脚本
```

## 测试

测试位于 `tests/`，使用一个无依赖的轻量测试框架（`tests/harness.hpp`），仿照 Zig 的 `std.testing.io`——确定性的，无外部框架。每个 slice 有自己的二进制：

| 测试 | 覆盖内容 |
|---|---|
| `result_test` | `Result<T>` / `IoError` 语义 |
| `writer_test` | `write_all`：短写入、零进度拒绝、错误传播 |
| `reader_test` | `read_exact`、`skip_exact`：EOF、部分读取、错误传播 |
| `fault_test` | `FaultReader`/`FaultWriter` 确定性和部分写入保持 |
| `buffer_test` | `BufferedReader`/`BufferedWriter` 顺序、EOF、脏 flush、flush 错误 |
| `observed_test` | 统计记账 + 数据透明性 |
| `copy_test` | `copy_all` 精确字节、总计数、双向错误传播 |
| `wal_test` | WAL 往返、截断、校验和不匹配、故障传播 |
| `file_test` | POSIX 文件往返、EOF、缺失文件、仅移动 |
| `io_context_api_test` | `IoContext` 接口契约 |
| `blocking_io_context_test` | `BlockingIoContext` 打开/错误路径 |
| `memory_io_context_test` | `MemoryIoContext` 往返 |
| `file_sync_test` | `SyncableWriter` 持久性契约 |
| `wal_writer_test` | WAL writer + sync 集成 |
| `uring_*_test` | 实验性 io_uring（无 liburing 时为 stub） |
| `file_positional_test` | 位置型读写（`pread`/`pwrite`/`preadv`/`pwritev`）——游标隔离、向量分块 |
| `blocking_io_pool_test` | `BlockingIoPool` 单元/属性测试——生命周期、拒绝、异常传播 |
| `blocking_io_pool_invariants_test` | `BlockingIoPool` B 类不变量强制 |
| `blocking_io_pool_prod_test` | `BlockingIoPool` 生产池并发 |
| `blocking_io_pool_stress_test` | `BlockingIoPool` 压力测试 |
| `sync_contract_negative_test` | 同步契约否定测试——G4–G6、G9、N4、N10 强制 |
| `sync_matrix_test` | 同步矩阵正确性 |

## 示例

| 示例 | 展示内容 |
|---|---|
| `mvp_memory_io_context` | **5 分钟体验**——内存往返，无文件系统 |
| `mvp_copy_pipeline` | 带缓冲快速路径的典型 MVP 组合 |
| `cat` | 流式读取文件到 stdout，带统计观测 |
| `copy_file` | `copy_all` 在两个文件间复制，显式 flush |
| `small_writes` | 通过 `BufferedWriter` + `ObservedWriter` 的多次小写入 |
| `fault_write` | 确定性 `FaultWriter` 失败 |
| `wal_records` | 在磁盘上写入和读回 WAL 记录 |
| `mvp_limited_copy` | `CopyLimit::bytes(N)` 限制复制 |
| `mvp_wal_vector` | 通过 `write_record_vec` 的 WAL 记录 |
| `mvp_copy_strategy` | Scratch / BufferedFirst / Auto 策略 |
| `mvp_wal_durable` | WAL 持久性：write/flush/sync + LSN 追踪 |
| `mvp_io_context_copy` | 通过 `BlockingIoContext` 打开 reader/writer |
| `blocking_io_pool` | 有界池：提交任务、收集结果、观测统计 |
| `sync_random_read` | 跨文件位置型随机读取 |

## 设计参考

Zig `std.Io` 是参考模型。`./zig` 下的 Zig 源码树**不**编译或链接——仅用于设计灵感。实现不依赖 Zig。

| Zig 概念 | Sluice 等价物 |
|---|---|
| `std.Io.Reader` | `sluice::Reader` |
| `std.Io.Writer` | `sluice::Writer` |
| 接口拥有缓冲 | `BufferedReader`/`BufferedWriter` 包装器 |
| `std.testing.io` | `FaultReader`/`FaultWriter` |
| `streamTo` | `copy_all(Reader&, Writer&)` |
| `readVec`/`writeVec` | `read_vec`/`write_vec`/`write_all_vec` |
| flush / sync 分离 | `flush()` + `SyncableWriter` |
| `Io` 能力上下文 | `sluice::IoContext` |

### 刻意的差异

这些是刻意的设计选择，不是缺陷：

1. **缓冲所有权在接口之外** —— Sluice 保持基础 `Reader`/`Writer` 无缓冲，通过包装器类型提供缓冲。
2. **`copy_all` 有缓冲快速路径** —— 通过可选的 `BufferedReadable` 能力，但不是完整的 Zig 零拷贝。
3. **`flush()` 是分离的，不是统一的** —— `FileWriter::flush()` 是空操作；`BufferedWriter::flush()` 做真正的排空工作。
4. **错误模型更丰富** —— `sluice::IoError::Code` 包含八个类别加上原始 `os_errno`。
5. **故障注入是原创设计** —— `FaultReader`/`FaultWriter` + `FaultPlan` 专为确定性测试构建。
6. **向量 I/O 语义保守** —— 在 EOF、错误或第一个正短结果时停止。错误立即传播。

## API 参考

完整 API 参考见 [`docs/api-reference-zh.md`](docs/api-reference-zh.md)。

## 状态

同步优先阶段已**完成**（任务 017S–023S）。阻塞基线现在是位置型的、定义了耐久性的、并发的（`BlockingIoPool`），并跨 W1–W4 进行了基准测试。同步优先就绪门控已转为**绿色**——异步实现已解除阻塞。

同步运行时契约（`docs/adr/ADR-024S-sync-runtime-contract.md`）已固定：所有调用都是同步的，位置型 I/O 不修改文件偏移量，EINTR 被重试，`BlockingIoPool` 是有界 OS 线程执行助手（不是异步运行时）。契约的明确非目标（无 `async`/`await`、无协程抽象、无 P2300、无飞行中系统调用取消）保护了同步与未来异步工作之间的边界。

异步**设计**（`docs/adr/ADR-async-io-model.md`）保持接受状态。异步**实现**将针对工程化的阻塞基线进行——而非仅顺序阻塞。

实验性 io_uring 写入 spike（SLUICE-CORE-013）位于 `sluice::experimental`，通过可选的 `--with-liburing` 构建门控。它**不是**默认后端。详见 `docs/io-uring-spike.md`。

完整变更日志见 `docs/changelog.md`。
