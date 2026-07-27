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

**复制 / 移动 / 赋值语义 (E15-P1-01/02)：**

- `Result<T>` 可复制、可移动；`Result<void>` 可平凡复制。
- 复制/移动**构造**和复制/移动**赋值**都在存储区中对 `T` 执行 placement-new
  构造（赋值永不调用 `T::operator=`）。因此 `noexcept` 跟的是
  `std::is_nothrow_move_constructible_v<T>`，而非 move 可赋值性——一个
  move 赋值不抛但 move 构造抛的类型，其 move 赋值是 `noexcept(false)`。
- **赋值的异常保证：** 旧值（如有）被销毁，并在替换构造*之前*写入一个
  确定性的 `IoError::Code::invalid_state` 哨兵。如果替换构造抛出异常，
  `*this` 处于合法的错误态 `Result`：`has_value() == false` 且
  `error().code == IoError::Code::invalid_state`；对象保持可销毁、可重新赋值。
  异常向外传播，最终析构函数不会第二次运行 `~T()`。

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

## 异步同步原语

`sluice::async::Mutex`（头文件 `#include <sluice/async/mutex.hpp>`）是异步
Scheduler 内部使用的、带 Clang TSA 注解的独占锁。它是 `std::mutex` 的薄封装，
满足 `BasicLockable` 与 `Lockable`，因此 `std::lock_guard<Mutex>`、
`std::unique_lock<Mutex>`、`std::condition_variable_any` 以及
`sluice::async::LockGuard` 均可基于它使用。

```cpp
class Mutex {
public:
    void lock() noexcept;       // 获取锁；不会抛出
    bool try_lock() noexcept;   // 非阻塞获取；不会抛出
    void unlock() noexcept;     // 释放锁；不会抛出
};
```

**失败契约（fail-fast）。** `lock()`、`try_lock()` 与 `unlock()` 均为
`noexcept`。底层获取失败——即 `std::mutex::lock()`/`try_lock()` 在资源耗尽或
其他平台错误时可能抛出的 `std::system_error`——**不会**作为可恢复异常向外传播。
`Mutex` 边界会将其转换为进程终止（fail-fast，经 `std::terminate`）。在权威
Scheduler 转移中，运行时无法在一次锁失败之后既能恢复用户执行又能保持所有权、
队列成员、发布等不变量，因此可恢复异常边界的存在是不正确的。该契约记录于
`docs/history/implementation-plans/async-mutex-nothrow-authority.md`。

违反 `unlock()` 所有权前置条件（解锁一个你不拥有的 `Mutex`）属于程序不变量违反
（未定义行为），而非可恢复错误；此处的 `noexcept` 仅用于说明不存在恢复路径。

**源码/ABI 说明。** `noexcept` 是函数类型的一部分。取成员地址的下游代码
（例如 `&sluice::async::Mutex::lock`）必须重新编译此头文件，使函数指针类型匹配。
**仓库内没有任何翻译单元取该类地址**（已验证：零命中），且 `Mutex` 接口完全
内联在头文件中，因此每个 TU 都会重新编译。在当前已验证工具链对应的 Itanium ABI
下，`noexcept` 不参与符号修饰，因此符号名不变。此处**不**声称在所有工具链/平台上
对 ABI 绝对不受影响；仅限于实际验证过的平台与编译器（已验证集合见
`docs/history/closeout/async-mutex-nothrow-implementation.md`）。

---

## 异步同步原语 (E10–E12)

异步同步原语建立在 E10 `WaitNode`/`WaitQueue` 基底与 E11 截止期/计时器集成之上。
跨原语权威文档见 `docs/history/closeout/e10-e12-api-semantic-closure.md`。

> **注意：** `include/sluice/async/mutex.hpp` 中的 `Mutex` 是 Scheduler 内部线程阻塞锁
> （TSA 注解的 `std::mutex` 包装），**不是** Fiber 挂起式 `AsyncMutex`。二者不可在
> 文档、命名或审计中混淆。

### `sluice::async::WaitOutcome`

```cpp
enum class WaitOutcome : std::uint8_t {
    unresolved = 0,  // 尚未终态（唯一的非终态值）
    woken = 1,       // 由 wake 解析 (RESOURCE_WAKE)
    cancelled = 2,   // 由 wait-epoch 取消解析 (CANCEL)
    expired = 3,     // 由截止期到期解析 (TIMER_EXPIRE, E11)
};
```

`WaitOutcome` 是一个四值枚举：`unresolved` 是唯一的非终态值，
`woken` / `cancelled` / `expired` 是三个终态结果（吸收式 —— 一旦终态便不再改变）。
`AsyncQueue<T>` **不**使用 `WaitOutcome`；它返回带类型的 `QueuePushResult<T>` /
`QueuePopResult<T>`，其 `status()` 携带 `committed`/`item`/`closed`/`expired`/
`would_block`。

### `sluice::async::WaitNode`

一次规范等待生命周期。调用者拥有、地址稳定、不可拷贝、不可移动。
每个 wait-epoch 使用一个全新 `WaitNode`。调用者将其传给阻塞操作，并在恢复后查询
`node.outcome()`。

“每纪元全新”是由吸收式 `WaitNode` 状态机以及“只有从 `Detached` 出发注册才能成功”的
注册前置条件所强制；被删除的拷贝/移动操作只是为了保持对象身份与地址稳定，它们本身
并不能阻止终态节点被复用。

```cpp
class WaitNode {
public:
    WaitNode() noexcept = default;
    explicit WaitNode(Fiber* fiber) noexcept;
    ~WaitNode();  // assert(!is_registered())

    void* user() const noexcept;
    void set_user(void* p) noexcept;

    WaitNode(const WaitNode&) = delete;
    WaitNode& operator=(const WaitNode&) = delete;
    WaitNode(WaitNode&&) = delete;
    WaitNode& operator=(WaitNode&&) = delete;

    bool is_registered() const noexcept;
    bool is_terminal() const noexcept;
    WaitOutcome outcome() const noexcept;
    bool was_woken() const noexcept;
    bool was_cancelled() const noexcept;
    bool was_expired() const noexcept;  // E11
    Fiber* fiber() const noexcept;

    // 安装头文件中公开以供侵入式实现访问；不是受支持的用户修改面。
    // WaitQueue 在 mtx_ 保护下拥有这些字段。
    WaitNode* next_{nullptr};
    WaitNode* prev_{nullptr};
    WaitQueue* home_{nullptr};
};
```

### `sluice::async::WaitQueue` 与 `sluice::async::TimerRegistration`

二者在安装头文件中是可命名的公开类型，但属于 Scheduler 集成的运行时基底，并非独立的
用户同步原语。`WaitQueue` 没有公开注册/解析方法；结构操作均为 private，唯一 friend 是
`Scheduler`。

```cpp
class WaitQueue {
public:
    WaitQueue() noexcept = default;
    ~WaitQueue();  // assert(empty)
    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator=(const WaitQueue&) = delete;
    WaitQueue(WaitQueue&&) = delete;
    WaitQueue& operator=(WaitQueue&&) = delete;
};

using deadline_tick_t = std::uint64_t;

class TimerRegistration {
public:
    using OnResolveFn = void (*)(void* owner_ctx, bool timer_won) noexcept;
    enum class State : std::uint8_t { active, retired, consumed };
    TimerRegistration() = default;
    TimerRegistration(WaitNode*, WaitQueue*, deadline_tick_t) noexcept;
    TimerRegistration(const TimerRegistration&) = delete;
    TimerRegistration& operator=(const TimerRegistration&) = delete;
    TimerRegistration(TimerRegistration&&) = delete;
    TimerRegistration& operator=(TimerRegistration&&) = delete;
    bool try_claim_expiry() noexcept;
    bool retire() noexcept;
    bool is_active() const noexcept;
    bool is_retired() const noexcept;
    bool is_consumed() const noexcept;
    State state() const noexcept;
    WaitNode* node() const noexcept;
    WaitQueue* queue() const noexcept;
    deadline_tick_t deadline() const noexcept;
    bool has_on_resolve() const noexcept;
    void fire_on_resolve_locked(bool timer_won) noexcept;
    std::size_t heap_index = static_cast<std::size_t>(-1);
};
```

### `sluice::async::Event`

持久手动复位异步 Event。不可拷贝、不可移动。

```cpp
class Event {
public:
    explicit Event(Scheduler& scheduler, bool initially_set = false) noexcept;
    ~Event() = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    [[nodiscard]] bool is_set() const noexcept;
    void set();                          // 广播给所有已注册等待者；外部线程安全
    void reset();                        // 不取消等待者
    void wait(WaitNode& node);           // 仅 Fiber；挂起直到 SET 或取消
    void wait_until(WaitNode& node, Scheduler::deadline_t deadline);  // 仅 Fiber
    [[nodiscard]] bool cancel(WaitNode& node);  // 按 wait-epoch 取消；任意线程
};
```

### `sluice::async::Semaphore`

异步计数 Semaphore。不可拷贝、不可移动。

```cpp
class Semaphore {
public:
    using permit_count_t = std::uint32_t;
    Semaphore(Scheduler& scheduler, permit_count_t initial_permits,
              permit_count_t max_permits) noexcept;
    ~Semaphore() = default;
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;

    [[nodiscard]] permit_count_t available() const noexcept;  // 无锁快照
    [[nodiscard]] bool try_acquire();          // 无 barging；任意线程
    void acquire(WaitNode& node);              // 仅 Fiber
    void acquire_until(WaitNode& node, Scheduler::deadline_t deadline);  // 仅 Fiber
    [[nodiscard]] bool cancel(WaitNode& node); // 按 wait-epoch 取消；任意线程
    [[nodiscard]] bool release();              // 转移/存储/溢出；外部线程安全
};
```

### `sluice::async::AsyncMutex`

Fiber 挂起式异步互斥锁。不可拷贝、不可移动。所有权以 `Fiber*` 身份标识（在 E8 work
stealing 下仍存活）。

```cpp
class AsyncMutex {
public:
    explicit AsyncMutex(Scheduler& scheduler) noexcept;
    ~AsyncMutex();  // assert(owner_ == nullptr)
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;

    [[nodiscard]] bool try_lock();              // 仅 Fiber；递归→false
    void lock(WaitNode& node);                  // 仅 Fiber
    void lock_until(WaitNode& node, Scheduler::deadline_t deadline);  // 仅 Fiber
    [[nodiscard]] bool cancel(WaitNode& node);  // 按 wait-epoch 取消；任意线程
    void unlock();                              // 仅 Fiber；必须是持有者
};
```

### `sluice::async::AsyncCondition`

Fiber 挂起式异步条件变量。构造时绑定一个 `AsyncMutex`。不可拷贝、不可移动。
双 epoch 协议：Condition epoch + 强制 Mutex 重新获取。

```cpp
class AsyncCondition {
public:
    explicit AsyncCondition(AsyncMutex& mutex) noexcept;
    ~AsyncCondition();  // assert(active_waits_ == 0)
    AsyncCondition(const AsyncCondition&) = delete;
    AsyncCondition& operator=(const AsyncCondition&) = delete;
    AsyncCondition(AsyncCondition&&) = delete;
    AsyncCondition& operator=(AsyncCondition&&) = delete;

    [[nodiscard]] WaitOutcome wait(WaitNode& condition_node);           // 仅 Fiber；必须持有 Mutex
    [[nodiscard]] WaitOutcome wait_until(WaitNode& condition_node,      // 仅 Fiber；必须持有 Mutex
                                          Scheduler::deadline_t deadline);
    [[nodiscard]] bool cancel(WaitNode& condition_node);  // 按 Condition-epoch 取消；任意线程
    void notify_one();                                    // 任意线程；非持久
    void notify_all();                                    // 任意线程；原子快照排空
};
```

### `sluice::async::AsyncQueue<T>`

有界 MPMC FIFO 通道。不可拷贝、不可移动。`T` 必须是对象类型、nothrow 可移动构造、
nothrow 可析构。`T` **不必**可默认构造或可移动赋值。

`AsyncQueue<T>` v1 **没有**公开的 wait-epoch 取消 API，也**没有** `Cancelled`
结果。`close()` 与截止期到期是 Queue 状态机的独立原因（`closed` / `expired`
状态），**不是**取消。`AsyncQueue<T>` 上没有 `cancel(WaitNode&)`；按 wait-epoch
的取消推迟到未来的权威机构（见 `docs/history/closeout/e10-e12-api-semantic-closure.md` D4）。

```cpp
template <class T>
class AsyncQueue final {
    static_assert(std::is_object_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);

public:
    explicit AsyncQueue(Scheduler& scheduler, std::size_t capacity);  // capacity == 0 抛异常
    ~AsyncQueue() = default;

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    // 快速路径（不挂起）
    [[nodiscard]] QueuePushResult<T> try_push(T value);
    [[nodiscard]] QueuePopResult<T> try_pop();
    void close() noexcept;  // 幂等、单调
    [[nodiscard]] bool is_closed() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // 阻塞（仅 Fiber）
    [[nodiscard]] QueuePushResult<T> push(T value);
    [[nodiscard]] QueuePushResult<T> push_until(T value, Scheduler::deadline_t deadline);
    [[nodiscard]] QueuePopResult<T> pop();
    [[nodiscard]] QueuePopResult<T> pop_until(Scheduler::deadline_t deadline);

    // 拆除（不可逆）
    detail::QueueTeardownSession begin_teardown() noexcept;
    T release_teardown(detail::QueueTeardownSession& session) noexcept;
};
```

拆除类型位于 `sluice::async::detail`，但它出现在 `AsyncQueue<T>` 的公开签名中：

```cpp
namespace detail {
class QueueTeardownSession final {
public:
    QueueTeardownSession(QueueTeardownSession&&) noexcept;
    QueueTeardownSession& operator=(QueueTeardownSession&&) = delete;
    QueueTeardownSession(const QueueTeardownSession&) = delete;
    QueueTeardownSession& operator=(const QueueTeardownSession&) = delete;
    ~QueueTeardownSession() noexcept;
    detail::QueueItemLease take_next() noexcept;
    bool empty() const noexcept;
};
}  // namespace detail
```

**结果类型（精确公开成员）：**

```cpp
template <class T>
class QueuePushResult final {
public:
    static QueuePushResult committed() noexcept;
    static QueuePushResult failed(QueuePushStatus, T&&) noexcept;
    QueuePushResult(QueuePushResult&&) noexcept = default;
    QueuePushResult& operator=(QueuePushResult&&) noexcept;
    QueuePushResult(const QueuePushResult&) = delete;
    QueuePushResult& operator=(const QueuePushResult&) = delete;
    ~QueuePushResult() = default;
    QueuePushStatus status() const noexcept;
    T take_value() && noexcept;
};

template <class T>
class QueuePopResult final {
public:
    static QueuePopResult item(T&&) noexcept;
    static QueuePopResult closed() noexcept;
    static QueuePopResult expired() noexcept;
    static QueuePopResult would_block() noexcept;
    QueuePopResult(QueuePopResult&&) noexcept = default;
    QueuePopResult& operator=(QueuePopResult&&) noexcept;
    QueuePopResult(const QueuePopResult&) = delete;
    QueuePopResult& operator=(const QueuePopResult&) = delete;
    ~QueuePopResult() = default;
    QueuePopStatus status() const noexcept;
    T take_value() && noexcept;
};
```

这两个结果模板自身都没有显式 `requires` 子句或 `static_assert`；上面的对象类型、
nothrow 移动构造和 nothrow 析构约束属于 `AsyncQueue<T>` 本身。结果类型成员仍严格按
以上签名声明为 `noexcept`。

二者均为仅移动类型；移动赋值采用 destroy-and-rebuild，因此 `T` 不必可移动赋值
（PR #12 纠正）。

### 通用词汇

| 术语 | 含义 |
|------|------|
| `wait-epoch` | 一次全新 `WaitNode` 注册 → 一次终态结果。由 `WaitNode` 对象身份标识。 |
| `wait-epoch 取消` | `cancel(WaitNode&)` 解析恰好一个已注册的 wait-epoch。**不是** task/Fiber/IO 取消。 |
| `绝对单调截止期` | `Scheduler::deadline_t` = `uint64_t` 单调滴答。`当且仅当 now >= deadline 时到期`。 |
| `已到截止期` | 入场时截止期 ≤ `monotonic_now()`。所有原语内联解析，不挂起。 |
| `入场优先级` | 资源就绪**先于**已到截止期检查（资源优先），AsyncCondition 除外（已到截止期内联返回 Expired，deadline 优先）。 |
| `注册后竞争` | 注册后 RESOURCE_WAKE / TIMER_EXPIRE / CANCEL 通过单一 `WaitNode::resolve_` CAS 竞争。 |
| `FIFO 等待者选择` | 等待者按 FIFO 注册顺序选择。**不**保证严格完成顺序。 |
| `无 barging` | 若有 FIFO 优先级更高的已排队等待者，`try_*` 操作失败。 |
| `destroy` | 析构函数。要求等待者为空（debug assert）。**不**取消/唤醒/清理。 |
| `close` | 单调 Open→Closed（仅 Queue）。排空角色 FIFO。 |
| `begin_teardown` | 不可逆 operational→tearing_down（仅 Queue）。返回 `QueueTeardownSession`。 |

### 线程调用边界

| 操作类别 | 示例 | 需要 Fiber | 外部线程安全 |
|---------|------|-----------|-------------|
| 阻塞/限时等待 | `wait`、`acquire`、`lock`、`push`、`pop` | 是 | 否 |
| 非阻塞 try | `try_acquire`、`try_push`、`try_pop` | 否 | 是 |
| 唤醒/通知 | `set`、`release`、`notify_one`、`notify_all` | 否 | 是 |
| 取消 | `cancel`（所有含 cancel 的原语） | 否 | 是 |
| 观察 | `is_set`、`available`、`is_closed`、`capacity`、`size` | 否 | 是 |
| 构造/析构 | 构造函数、析构函数 | 否 | 构造函数：是；析构函数：需要静默状态（WaitQueue 为空、无活跃 condition 等待、无 mutex 持有者） |

---

## 异步运行时 (E13+)

异步运行时类型构建在 E10–E12 同步基板之上。架构背景见
`docs/architecture/async-runtime.md` 和 `docs/architecture/async-io-foundation.md`。

### `sluice::async::Scheduler`

M:N fiber 调度器。不可复制、不可移动。拥有 Worker、deadline 堆和外部唤醒控制块。

```cpp
class Scheduler {
public:
    using deadline_t = std::uint64_t;  // 单调 tick

    template <class... Args>
    explicit Scheduler(ExecutionStrategy strategy, Args&& ...args);
    ~Scheduler();  // 断言：无活跃等待者/未完成注册

    template <class Fn>
    Fiber* spawn(Fn&& fn);  // 生成新 Fiber

    void run();  // 运行调度器（单线程调用者）

    template <class... Arms>
    SelectResult select(Arm&&... arms);  // E13 入口

    void shutdown();
};
```

**执行策略：**

| 策略 | 描述 | 平台 |
|------|------|------|
| `threaded` | 每个阻塞等待消耗一个 OS 线程（`std::condition_variable`）。 | 任意 |
| `evented` | Fiber 在单线程上挂起/恢复；需要 `fiber_ctx::supported`。 | x86_64 Linux |

### `sluice::async::CancellationToken` / `sluice::async::CancelState`

协作式取消原语（E27）。`CancelToken` 是取消请求状态；`CancelState`
是每消费者的保护/确认状态。

```cpp
class CancelToken {
public:
    void request() noexcept;          // 幂等的取消请求
    bool is_requested() const noexcept;
};

enum class CancelProtection : std::uint8_t { unblocked, blocked };

class CancelState {
public:
    void protect() noexcept;          // 进入保护区域（阻止传递）
    void unprotect() noexcept;        // 退出保护区域
    Result<void> check_cancel(const CancelToken&) noexcept;
};
```

### `sluice::async::Future<T>`

单任务可等待对象（E28）。生产者侧：`complete_with()`。消费者侧：
`await()` / `cancel()`。幂等：结果确定后两者都是返回缓存结果的空操作。

```cpp
template <class T>
class Future {
public:
    Future();                          // Threaded 等待策略（默认）
    explicit Future(WaitPolicy& policy);  // 注入 Evented 策略

    Result<T> await();                 // Threaded：阻塞；Evented：挂起 Fiber
    Result<T> cancel();                // 请求取消；幂等
    void complete_with(Result<T> r);   // 生产者发布结果
};
```

### `sluice::async::Group`

无序任务集（E29）。通过 `async(fn)` 添加任务；整体等待或整体取消。
取消传播边界：任务吞掉 `IoError::canceled`。

```cpp
class Group {
public:
    Group();                           // Threaded 模式
    explicit Group(Scheduler& scheduler);  // Evented 模式

    template <class Fn>
    void async(Fn&& fn);               // 生成一个任务

    void await();                      // 等待所有任务
    void cancel();                     // 请求取消所有任务，然后等待
};
```

### `sluice::async::SelectResult`

E13 Select 获胜者描述符。

```cpp
class SelectResult {
public:
    bool has_winner() const noexcept;           // 全过期/取消时返回 false
    std::size_t index() const noexcept;         // 获胜臂索引
    SelectKind kind() const noexcept;           // event 或 timer
    SelectTimerOutcome timer_outcome() const noexcept;  // fired（仅 timer 获胜）
};
```

### `sluice::async::Completion<T>`

单个未完成操作的状态（E17）。调用者拥有，地址稳定，不可复制、不可移动。

```cpp
template <class T>
class Completion {
public:
    Completion() noexcept = default;

    bool ready() const noexcept;
    Result<T> result() const noexcept;   // 仅 ready 时有效
    void reset() noexcept;               // 回到 idle 可复用

    std::size_t next_reap_seq() const noexcept;  // reap 顺序（E15-P1-04）
};
```

### `sluice::async::AsyncBackend`

内部后端边界（ADR §4）。不对外暴露；`AsyncIoContext` 委托给它。

```cpp
class AsyncBackend {
public:
    virtual ~AsyncBackend() = default;
    virtual Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) = 0;
    virtual Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) = 0;
    virtual std::size_t poll() = 0;      // 非阻塞 reap
    virtual std::size_t wait_one() = 0;  // 阻塞 reap
};
```

### `sluice::async::AsyncIoContext`

公开 L1 异步 API 表面（E17）。拥有 `AsyncBackend`。不可复制；移动语义
遵循 E15-P1-03 / E15-P2-06。

```cpp
class AsyncIoContext {
public:
    template <class Backend, class... Args>
    explicit AsyncIoContext(in_place_type_t<Backend>, Args&&... args);

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    std::size_t poll();              // 非阻塞
    std::size_t wait_one();          // 阻塞

    std::size_t cancel();            // 取消所有未完成操作

    void attach_stats(AsyncStats* s);  // 调用者拥有，可空
};
```

### `sluice::async::Batch`

分组完成（E30）。一起提交 N 个操作，整体等待，通过 `next()` 按完成顺序迭代。

```cpp
class Batch {
public:
    explicit Batch(AsyncIoContext& io);
    ~Batch();

    void add(BatchOp op);            // 添加一个操作
    void await_one();                // 等待 >=1 完成
    std::optional<BatchResult> next();  // 按 reap 顺序迭代
    std::size_t cancel();            // 取消所有未完成操作
};
```

### `sluice::async::ThreadPoolBackend`

可移植的真实后端（sluice-CORE-020A）。每个未完成操作一个 `std::thread`。
始终可构建；无外部依赖。

```cpp
class ThreadPoolBackend : public AsyncBackend {
public:
    ThreadPoolBackend();
};
```

### `sluice::async::UringAsyncBackend`

Linux io_uring 后端（sluice-CORE-020B）。构建门控在 liburing 之后。没有
`SLUICE_HAS_LIBURING` 时是不支持的 stub：`submit_*` 同步返回
`IoError::backend_error`；`poll()` / `wait_one()` 不 reap 任何内容。

```cpp
class UringAsyncBackend : public AsyncBackend {
public:
    UringAsyncBackend();
};
```

工厂函数：

```cpp
AsyncIoContext threadpool_backend();
AsyncIoContext uring_backend();
```

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
