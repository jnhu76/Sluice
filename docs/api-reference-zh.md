# API 参考

v0.1-mvp 以来的公开、准稳定 API。"准稳定"意味着删除或静默重新语义化会破坏使用者；视为在小版本间冻结，仅在有计划的弃用时更改。

内部细节和实验性 API 见 `docs/api-audit.md`。

## 错误模型

### `sluice::IoError`

```cpp
struct IoError {
    enum class Code : std::uint8_t {
        eof,              // 流结束
        canceled,         // 操作取消 (ECANCELED)
        interrupted,      // 系统调用被中断 (EINTR)
        would_block,      // 非阻塞模式会阻塞 (EAGAIN/EWOULDBLOCK)
        no_space,         // 空间不足 (ENOSPC, EDQUOT)
        permission_denied,// 访问被拒绝 (EACCES, EPERM, ENOENT, ENOTDIR)
        invalid_state,    // 前置条件违反（如出错后仍尝试 flush 脏字节）
        backend_error,    // 未分类 / 原始 errno
    };
    Code code;
    int os_errno = 0;     // 保留的 POSIX errno（不适用时为 0）
};
```

**辅助函数：**

| 函数 | 描述 |
|---|---|
| `to_string(IoError::Code)` | 返回 `"eof"`、`"interrupted"` 等 |
| `from_errno_value(int)` | 将 POSIX errno 映射为 `IoError` |
| `operator==(IoError, IoError)` | 比较 `code` 和 `os_errno` |

---

## Result 类型

### `sluice::Result<T>`

最小化的 `[[nodiscard]]` expected 类型。持有 `T` 或 `IoError`。

```cpp
Result<std::size_t> n = reader.read_some(buf);
if (n.has_value()) {
    // n.value() == 读取的字节数
} else {
    // n.error() == IoError
}

// Result<void> 用于无返回值的操作
Result<void> ok = writer.write_all(data);
if (!ok) return ok.error();
```

| 方法 | 描述 |
|---|---|
| `has_value()` / `operator bool()` | 持有值时返回 true |
| `value()` / `value_or(fallback)` | 访问值 |
| `error()` | 访问错误 |

**构造：**

| 工厂方法 | 描述 |
|---|---|
| `make_unexpected<T>(IoError)` | 创建错误结果 |

---

## 核心抽象

### `sluice::Reader`

抽象字节源。具体 reader 实现 `read_some`；`read_exact` 和 `stream_to` 是派生操作。

| 方法 | 签名 | 描述 |
|---|---|---|
| `read_some` | `Result<size_t> read_some(span<byte> dst)` | **原语。** 读取最多 `dst.size()` 字节。EOF 返回 0。 |
| `read_exact` | `Result<void> read_exact(span<byte> dst)` | **派生。** 精确读取 `dst.size()` 字节或失败。 |
| `read_vec` | `Result<size_t> read_vec(span<IoSlice> dsts)` | **虚函数。** 分散读取到多个 slice。首个短读时停止。 |
| `read_vec_all` | `Result<void> read_vec_all(span<IoSlice> dsts)` | **派生。** 填满每个 slice 的每个字节。 |
| `stream_to` | `Result<size_t> stream_to(Writer&)` | **派生。** 复制直到 EOF（无界）。 |
| `stream_to` | `Result<uint64_t> stream_to(Writer&, span<byte> scratch, CopyLimit, CopyStats*)` | **派生。** 有界复制，带暂存缓冲。 |
| `stream_to` | `Result<uint64_t> stream_to(Writer&, CopyLimit)` | **派生。** 有界复制，内部暂存。 |

### `sluice::Writer`

抽象字节汇。具体 writer 实现 `write_some` 和 `flush`；`write_all` 是派生操作。

| 方法 | 签名 | 描述 |
|---|---|---|
| `write_some` | `Result<size_t> write_some(span<const byte> src)` | **原语。** 写入最多 `src.size()` 字节。 |
| `flush` | `Result<void> flush()` | **原语。** 将缓冲字节排空到内部 writer。不保证持久性。 |
| `write_all` | `Result<void> write_all(span<const byte> src)` | **派生。** 重试直到所有字节写入或出错。 |
| `write_vec` | `Result<size_t> write_vec(span<const ConstIoSlice> srcs)` | **虚函数。** 从多个 slice 聚集写入。首个短写时停止。 |
| `write_all_vec` | `Result<void> write_all_vec(span<const ConstIoSlice> srcs)` | **派生。** 写入每个 slice 的每个字节。 |

### `sluice::IoSlice` / `sluice::ConstIoSlice`

```cpp
struct IoSlice       { span<byte> bytes; };       // 可变目标
struct ConstIoSlice  { span<const byte> bytes; };  // 不可变源
```

---

## Copy

### `sluice::copy_all`

```cpp
// 策略感知主重载
Result<uint64_t> copy_all(Reader&, Writer&, span<byte> scratch,
                          CopyOptions, CopyStats* = nullptr,
                          CopyDecision* = nullptr);

// 有界复制，调用者提供暂存
Result<uint64_t> copy_all(Reader&, Writer&, span<byte> scratch,
                          CopyLimit, CopyStats* = nullptr);

// 无界复制，调用者提供暂存
Result<uint64_t> copy_all(Reader&, Writer&, span<byte> scratch);

// 有界复制，内部暂存
Result<uint64_t> copy_all(Reader&, Writer&, CopyLimit);

// 无界复制，内部暂存
Result<uint64_t> copy_all(Reader&, Writer&);
```

### `sluice::CopyStrategy`

```cpp
enum class CopyStrategy {
    Auto,            // 默认；当前行为同 BufferedFirst
    Scratch,         // 强制使用暂存读写循环
    BufferedFirst,   // 先排空缓冲字节，再使用暂存
    VectorDeferred,  // 保留槽位（未实现）
    FileRangeDeferred,
    SendfileDeferred,
    SpliceDeferred,
};
```

### `sluice::CopyOptions`

```cpp
struct CopyOptions {
    CopyLimit limit = CopyLimit::unlimited();
    CopyStrategy strategy = CopyStrategy::Auto;
    UnsupportedStrategyPolicy unsupported_policy =
        UnsupportedStrategyPolicy::ReturnInvalidState;
};
```

### `sluice::CopyDecision`

```cpp
struct CopyDecision {
    CopyStrategy requested = CopyStrategy::Auto;
    CopyStrategy selected = CopyStrategy::Auto;
    string_view reason = "auto";
    bool used_buffered_fast_path = false;
    bool used_scratch_path = false;
    bool unsupported_requested = false;
};
```

### `sluice::CopyLimit`

```cpp
CopyLimit::unlimited()   // 复制直到 EOF 或错误
CopyLimit::bytes(n)      // 最多复制 n 字节
CopyLimit::nothing()     // 等同于 bytes(0)
```

---

## 包装器

### `sluice::BufferedReader` / `sluice::BufferedWriter`

接口级缓冲包装器。调用者提供并拥有后备存储。

```cpp
BufferedReader(Reader& inner, span<byte> buffer, BufferStats* = nullptr);
BufferedWriter(Writer& inner, span<byte> buffer, BufferStats* = nullptr);
```

- 不可复制、不可移动。
- `BufferedReader` 同时实现 `BufferedReadable`（见下文）。
- `BufferedWriter` 析构时**不** flush（debug assert 捕获误用）。

### `sluice::BufferedReadable`

`copy_all` 缓冲快速路径的可选能力接口。

```cpp
class BufferedReadable {
    virtual span<const byte> peek_buffered() const = 0;
    virtual Result<void> consume_buffered(size_t n) = 0;
};
```

### `sluice::ObservedReader` / `sluice::ObservedWriter`

透明的统计收集包装器。零拷贝透传。

```cpp
ObservedReader(Reader& inner, ReaderStats& stats, VectorStats* = nullptr);
ObservedWriter(Writer& inner, WriterStats& stats, VectorStats* = nullptr);
```

**统计结构体：**

```cpp
struct ReaderStats { uint64_t read_calls, read_bytes, eof_count, read_errors; };
struct WriterStats { uint64_t write_calls, write_bytes, short_writes,
                     write_errors, flush_calls, flush_errors; };
```

### `sluice::MemoryReader` / `sluice::MemoryWriter`

内存源/汇。用于测试、示例和故障包装器。

```cpp
// 构造
MemoryReader from_string(string_view s);
MemoryReader from_bytes(span<const byte> bytes);
MemoryWriter from_string(string_view s);

// 访问
const vector<byte>& bytes() const;  // MemoryWriter
vector<byte> take();                // MemoryWriter（移出）
size_t remaining() const;           // MemoryReader
```

### `sluice::FaultReader` / `sluice::FaultWriter`

确定性故障注入包装器。

```cpp
FaultReader(Reader& inner, const FaultPlan& plan);
FaultWriter(Writer& inner, const FaultPlan& plan);
```

**`FaultPlan`：**

```cpp
struct FaultPlan {
    optional<uint64_t> fail_after_read_calls;    // 读调用次数后失败
    optional<uint64_t> fail_after_write_calls;   // 写调用次数后失败
    optional<uint64_t> fail_after_bytes;         // 字节数后失败
    optional<size_t> max_read_size;      // 将读取限制为此大小
    optional<size_t> max_write_size;     // 将写入限制为此大小
    bool fail_flush = false;             // flush 时是否失败
    IoError error = {IoError::Code::backend_error, 0};
};
```

---

## 后端

### `sluice::FileReader` / `sluice::FileWriter`

阻塞 POSIX 文件 I/O。RAII，仅移动。

```cpp
// 打开，可选测量钩子
FileReader(const string& path, SyscallStats* = nullptr, VectorStats* = nullptr);
FileWriter(const string& path, SyscallStats* = nullptr,
           VectorStats* = nullptr, SyncStats* = nullptr);

// 接管已打开的 fd（-1 表示空）
FileReader(int fd);
FileWriter(int fd);

// 查询
bool opened() const;
const optional<IoError>& open_error() const;
```

- `FileWriter::flush()` 是**空操作**（无 fsync）。使用 `sync_data()` / `sync_all()`。
- `FileWriter` 实现 `SyncableWriter`。

### `sluice::IoContext` / `sluice::BlockingIoContext`

```cpp
class IoContext {
    virtual Result<unique_ptr<Reader>> open_reader(string_view path, OpenReaderOptions = {}) = 0;
    virtual Result<unique_ptr<Writer>> open_writer(string_view path, OpenWriterOptions = {}) = 0;
};

// 选项
struct OpenReaderOptions { SyscallStats* = nullptr; VectorStats* = nullptr; };
struct OpenWriterOptions { SyscallStats* = nullptr; VectorStats* = nullptr; SyncStats* = nullptr; };
```

`BlockingIoContext` 是具体的 POSIX 实现。打开错误在打开时返回。

### `sluice::MemoryIoContext`

用于测试和示例的确定性内存上下文。

```cpp
MemoryIoContext ctx;
ctx.seed("path", bytes);                // 种子可读数据
auto r = ctx.open_reader("path");       // 返回 MemoryReader（独立副本）
auto w = ctx.open_writer("path");       // 返回全新的 MemoryWriter
```

---

## Sync / 持久性

### `sluice::SyncableWriter`

可选能力接口。与 `Writer::flush()` 分离。

```cpp
class SyncableWriter {
    virtual Result<void> sync_data() = 0;  // fdatasync
    virtual Result<void> sync_all() = 0;   // fsync
};
```

`FileWriter` 实现此接口。检测通过 `dynamic_cast`。

---

## WAL

### `sluice::wal` 自由函数

```cpp
Result<void> write_record(Writer&, span<const byte> payload);
Result<void> write_record_vec(Writer&, span<const byte> payload);
Result<vector<byte>> read_record(Reader&);
```

记录格式（小端）：`magic(u32) | length(u32) | payload | checksum(u32)`。

### `sluice::wal::WalWriter`

最小 WAL 持久性包装器。追踪三个 LSN，不变量为 `durable_lsn <= flushed_lsn <= written_lsn`。

```cpp
WalWriter(Writer& writer);
WalWriter(Writer& writer, SyncableWriter* syncable);

Result<void> write_record(span<const byte> payload);
Result<void> write_record_vec(span<const byte> payload);
Result<void> flush();    // flushed_lsn 推进
Result<void> sync();     // durable_lsn 推进（需要 SyncableWriter）

uint64_t written_lsn() const;
uint64_t flushed_lsn() const;
uint64_t durable_lsn() const;
```

---

## 测量

所有统计结构体由调用者拥有，默认初始化为零，通过可空指针附加（null = 不计数）。

| 结构体 | 追踪内容 |
|---|---|
| `SyscallStats` | POSIX read/write 系统调用次数和字节数 |
| `BufferStats` | 缓冲命中/未命中/重填活动 |
| `CopyStats` | Copy 循环迭代次数、字节数、停止原因、策略选择 |
| `VectorStats` | read_vec/write_vec 调用次数、字节数、回退次数 |
| `SyncStats` | sync_data/sync_all 调用次数和错误 |
| `UringStats` | 实验性 io_uring 队列/提交/完成计数 |
