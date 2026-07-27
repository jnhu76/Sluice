# API Reference

Public, stable-ish APIs as of v0.1-mvp. "Stable-ish" means removing or silently re-semanticizing these would break consumers; treat as frozen across minor work, change only with deliberate deprecation.

For internal details and experimental APIs, see `docs/api-audit.md`.

## Error model

### `sluice::IoError`

```cpp
struct IoError {
    enum class Code : std::uint8_t {
        eof,              // end of stream
        canceled,         // operation canceled (ECANCELED)
        interrupted,      // system call interrupted (EINTR)
        would_block,      // non-blocking would block (EAGAIN/EWOULDBLOCK)
        no_space,         // no space left (ENOSPC, EDQUOT)
        permission_denied,// access denied (EACCES, EPERM, ENOENT, ENOTDIR)
        invalid_state,    // precondition violated (e.g. flush dirty bytes after error)
        backend_error,    // unclassified / raw errno
    };
    Code code;
    int os_errno = 0;     // preserved POSIX errno (0 if not applicable)
};
```

**Helpers:**

| Function | Description |
|---|---|
| `to_string(IoError::Code)` | Returns `"eof"`, `"interrupted"`, etc. |
| `from_errno_value(int)` | Maps POSIX errno to `IoError` |
| `operator==(IoError, IoError)` | Compares both `code` and `os_errno` |

---

## Result type

### `sluice::Result<T>`

A minimal `[[nodiscard]]` expected-like type. Carries either `T` or `IoError`.

```cpp
Result<std::size_t> n = reader.read_some(buf);
if (n.has_value()) {
    // n.value() == bytes read
} else {
    // n.error() == IoError
}

// Result<void> for operations with no return value
Result<void> ok = writer.write_all(data);
if (!ok) return ok.error();
```

| Method | Description |
|---|---|
| `has_value()` / `operator bool()` | True if holding a value |
| `value()` / `value_or(fallback)` | Access the value |
| `error()` | Access the error |

**Constructors:**

| Factory | Description |
|---|---|
| `make_unexpected<T>(IoError)` | Create an error-result |

**Copy / move / assignment semantics (E15-P1-01/02):**

- `Result<T>` is copyable and movable; `Result<void>` is trivially copyable.
- Copy/move **construction** and copy/move **assignment** all perform
  placement-new construction of `T` into the storage (assignment never calls
  `T::operator=`). The advertised `noexcept` therefore tracks
  `std::is_nothrow_move_constructible_v<T>`, not move-assignability — a type
  with a non-throwing move-assign but throwing move-ctor yields a
  `noexcept(false)` move-assignment.
- **Exception guarantee on assignment:** the old value (if any) is destroyed
  and a deterministic `IoError::Code::invalid_state` sentinel is written
  *before* the replacement is constructed. If the replacement construction
  throws, `*this` is left in a valid error-state `Result`:
  `has_value() == false` AND `error().code == IoError::Code::invalid_state`;
  the object remains destroy-safe and reassignable. The exception propagates
  and the eventual destructor does not run `~T()` a second time.

---

## Core abstractions

### `sluice::Reader`

Abstract byte source. Concrete readers implement `read_some`; `read_exact` and `stream_to` are derived.

| Method | Signature | Description |
|---|---|---|
| `read_some` | `Result<size_t> read_some(span<byte> dst)` | **Primitive.** Read up to `dst.size()` bytes. Returns 0 on EOF. |
| `read_exact` | `Result<void> read_exact(span<byte> dst)` | **Derived.** Read exactly `dst.size()` bytes or fail. |
| `read_vec` | `Result<size_t> read_vec(span<IoSlice> dsts)` | **Virtual.** Scatter-read into slices. Stops on first short read. |
| `read_vec_all` | `Result<void> read_vec_all(span<IoSlice> dsts)` | **Derived.** Fill every byte of every slice. |
| `stream_to` | `Result<size_t> stream_to(Writer&)` | **Derived.** Copy until EOF (unbounded). |
| `stream_to` | `Result<uint64_t> stream_to(Writer&, span<byte> scratch, CopyLimit, CopyStats*)` | **Derived.** Bounded copy with scratch. |
| `stream_to` | `Result<uint64_t> stream_to(Writer&, CopyLimit)` | **Derived.** Bounded copy, internal scratch. |

### `sluice::Writer`

Abstract byte sink. Concrete writers implement `write_some` and `flush`; `write_all` is derived.

| Method | Signature | Description |
|---|---|---|
| `write_some` | `Result<size_t> write_some(span<const byte> src)` | **Primitive.** Write up to `src.size()` bytes. |
| `flush` | `Result<void> flush()` | **Primitive.** Drain buffered bytes to inner writer. Not durable. |
| `write_all` | `Result<void> write_all(span<const byte> src)` | **Derived.** Retry until all bytes written or error. |
| `write_vec` | `Result<size_t> write_vec(span<const ConstIoSlice> srcs)` | **Virtual.** Scatter-write from slices. Stops on first short write. |
| `write_all_vec` | `Result<void> write_all_vec(span<const ConstIoSlice> srcs)` | **Derived.** Write every byte of every slice. |

### `sluice::IoSlice` / `sluice::ConstIoSlice`

```cpp
struct IoSlice       { span<byte> bytes; };       // mutable destination
struct ConstIoSlice  { span<const byte> bytes; };  // immutable source
```

---

## Copy

### `sluice::copy_all`

```cpp
// Strategy-aware primary overload
Result<uint64_t> copy_all(Reader&, Writer&, span<byte> scratch,
                          CopyOptions, CopyStats* = nullptr,
                          CopyDecision* = nullptr);

// Bounded copy with caller scratch
Result<uint64_t> copy_all(Reader&, Writer&, span<byte> scratch,
                          CopyLimit, CopyStats* = nullptr);

// Unbounded copy with caller scratch
Result<uint64_t> copy_all(Reader&, Writer&, span<byte> scratch);

// Bounded copy, internal scratch
Result<uint64_t> copy_all(Reader&, Writer&, CopyLimit);

// Unbounded copy, internal scratch
Result<uint64_t> copy_all(Reader&, Writer&);
```

### `sluice::CopyStrategy`

```cpp
enum class CopyStrategy {
    Auto,            // default; currently behaves as BufferedFirst
    Scratch,         // force scratch read/write loop
    BufferedFirst,   // drain buffered bytes first, then scratch
    VectorDeferred,  // reserved (not implemented)
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
CopyLimit::unlimited()   // copy until EOF or error
CopyLimit::bytes(n)      // copy at most n bytes
CopyLimit::nothing()     // == bytes(0)
```

---

## Wrappers

### `sluice::BufferedReader` / `sluice::BufferedWriter`

Interface-level buffering wrappers. Caller provides and owns the backing storage.

```cpp
BufferedReader(Reader& inner, span<byte> buffer, BufferStats* = nullptr);
BufferedWriter(Writer& inner, span<byte> buffer, BufferStats* = nullptr);
```

- Non-copyable, non-movable.
- `BufferedReader` also implements `BufferedReadable` (see below).
- `BufferedWriter` does **not** flush in destructor (debug assert catches misuse).

### `sluice::BufferedReadable`

Opt-in capability interface for `copy_all`'s buffered fast path.

```cpp
class BufferedReadable {
    virtual span<const byte> peek_buffered() const = 0;
    virtual Result<void> consume_buffered(size_t n) = 0;
};
```

### `sluice::ObservedReader` / `sluice::ObservedWriter`

Transparent stats-collecting wrappers. Zero-copy pass-through.

```cpp
ObservedReader(Reader& inner, ReaderStats& stats, VectorStats* = nullptr);
ObservedWriter(Writer& inner, WriterStats& stats, VectorStats* = nullptr);
```

**Stats structs:**

```cpp
struct ReaderStats { uint64_t read_calls, read_bytes, eof_count, read_errors; };
struct WriterStats { uint64_t write_calls, write_bytes, short_writes,
                     write_errors, flush_calls, flush_errors; };
```

### `sluice::MemoryReader` / `sluice::MemoryWriter`

In-memory sources/sinks. Used by tests, examples, and fault wrappers.

```cpp
// Construction
MemoryReader from_string(string_view s);
MemoryReader from_bytes(span<const byte> bytes);
MemoryWriter from_string(string_view s);

// Access
const vector<byte>& bytes() const;  // MemoryWriter
vector<byte> take();                // MemoryWriter (move out)
size_t remaining() const;           // MemoryReader
```

### `sluice::FaultReader` / `sluice::FaultWriter`

Deterministic fault injection wrappers.

```cpp
FaultReader(Reader& inner, const FaultPlan& plan);
FaultWriter(Writer& inner, const FaultPlan& plan);
```

**`FaultPlan`:**

```cpp
struct FaultPlan {
    optional<uint64_t> fail_after_read_calls;
    optional<uint64_t> fail_after_write_calls;
    optional<uint64_t> fail_after_bytes;
    optional<size_t> max_read_size;      // clamp reads to this size
    optional<size_t> max_write_size;     // clamp writes to this size
    bool fail_flush = false;
    IoError error = {IoError::Code::backend_error, 0};
};
```

---

## Backends

### `sluice::FileReader` / `sluice::FileWriter`

Blocking POSIX file I/O. RAII, move-only.

```cpp
// Open with optional measurement hooks
FileReader(const string& path, SyscallStats* = nullptr, VectorStats* = nullptr);
FileWriter(const string& path, SyscallStats* = nullptr,
           VectorStats* = nullptr, SyncStats* = nullptr);

// Adopt an already-open fd (-1 for empty)
FileReader(int fd);
FileWriter(int fd);

// Query
bool opened() const;
const optional<IoError>& open_error() const;
```

- `FileWriter::flush()` is a **no-op** (no fsync). Use `sync_data()` / `sync_all()`.
- `FileWriter` implements `SyncableWriter`.

### `sluice::IoContext` / `sluice::BlockingIoContext`

```cpp
class IoContext {
    virtual Result<unique_ptr<Reader>> open_reader(string_view path, OpenReaderOptions = {}) = 0;
    virtual Result<unique_ptr<Writer>> open_writer(string_view path, OpenWriterOptions = {}) = 0;
};

// Options
struct OpenReaderOptions { SyscallStats* = nullptr; VectorStats* = nullptr; };
struct OpenWriterOptions { SyscallStats* = nullptr; VectorStats* = nullptr; SyncStats* = nullptr; };
```

`BlockingIoContext` is the concrete POSIX implementation. Open errors are returned at open time.

### `sluice::MemoryIoContext`

Deterministic in-memory context for tests and examples.

```cpp
MemoryIoContext ctx;
ctx.seed("path", bytes);                // seed readable data
auto r = ctx.open_reader("path");       // returns MemoryReader (independent copy)
auto w = ctx.open_writer("path");       // returns fresh MemoryWriter
```

---

## Sync / Durability

### `sluice::SyncableWriter`

Opt-in capability interface. Separate from `Writer::flush()`.

```cpp
class SyncableWriter {
    virtual Result<void> sync_data() = 0;  // fdatasync
    virtual Result<void> sync_all() = 0;   // fsync
};
```

`FileWriter` implements this. Detection is via `dynamic_cast`.

---

## WAL

### `sluice::wal` free functions

```cpp
Result<void> write_record(Writer&, span<const byte> payload);
Result<void> write_record_vec(Writer&, span<const byte> payload);
Result<vector<byte>> read_record(Reader&);
```

Record format (little-endian): `magic(u32) | length(u32) | payload | checksum(u32)`.

### `sluice::wal::WalWriter`

Minimal WAL durability wrapper. Tracks three LSNs with invariant `durable_lsn <= flushed_lsn <= written_lsn`.

```cpp
WalWriter(Writer& writer);
WalWriter(Writer& writer, SyncableWriter* syncable);

Result<void> write_record(span<const byte> payload);
Result<void> write_record_vec(span<const byte> payload);
Result<void> flush();    // flushed_lsn advances
Result<void> sync();     // durable_lsn advances (needs SyncableWriter)

uint64_t written_lsn() const;
uint64_t flushed_lsn() const;
uint64_t durable_lsn() const;
```

---

## Async synchronization

`sluice::async::Mutex` (header `#include <sluice/async/mutex.hpp>`) is the
Clang-TSA-annotated exclusive lock used internally by the async Scheduler. It
is a thin `std::mutex` shim that satisfies `BasicLockable` and `Lockable`, so
`std::lock_guard<Mutex>`, `std::unique_lock<Mutex>`, `std::condition_variable_any`,
and `sluice::async::LockGuard` all work against it.

```cpp
class Mutex {
public:
    void lock() noexcept;       // acquires; never throws
    bool try_lock() noexcept;   // acquires without blocking; never throws
    void unlock() noexcept;     // releases; never throws
};
```

**Failure contract (fail-fast).** `lock()`, `try_lock()`, and `unlock()` are
`noexcept`. An underlying acquisition failure — the `std::system_error` that
`std::mutex::lock()`/`try_lock()` may throw on resource exhaustion or other
platform errors — is **not** propagated as a recoverable exception. Instead
the `Mutex` boundary converts it to process termination (fail-fast via
`std::terminate`). The runtime cannot resume user execution after such a
failure while preserving ownership, queue-membership, and publication
invariants inside an authoritative Scheduler transition, so a recoverable
exception edge would be unsound. This contract is recorded in
`docs/history/implementation-plans/async-mutex-nothrow-authority.md`.

A violated `unlock()` ownership precondition (unlocking a `Mutex` you do not
own) is a program invariant violation (undefined behavior), not a recoverable
error; `noexcept` here documents that no recovery path exists.

**Source/ABI note.** `noexcept` is part of the function type. Downstream code
that takes the address of a member (e.g. `&sluice::async::Mutex::lock`) must
be recompiled against this header so the function-pointer type matches. **No
in-repo translation unit takes such an address** (verified: zero occurrences),
and the `Mutex` surface is entirely inline in the header, so every TU already
recompiles. Under the Itanium ABI verified for the current toolchains,
`noexcept` is not part of symbol mangling, so the symbol names are unchanged.
This is **not** claimed as an absolute ABI guarantee across all toolchains or
platforms; it is limited to the platforms and compilers actually verified
(see `docs/history/closeout/async-mutex-nothrow-implementation.md` for the verified set).

---

## Async Synchronization (E10–E12)

The async synchronization primitives are built on the E10 `WaitNode`/`WaitQueue` substrate
and the E11 deadline/timer integration. See `docs/history/closeout/e10-e12-api-semantic-closure.md` for the
cross-primitive authority.

### `sluice::async::WaitOutcome`

```cpp
enum class WaitOutcome : std::uint8_t {
    unresolved = 0,  // Not yet terminal (the only non-terminal value)
    woken = 1,       // Resolved by wake (RESOURCE_WAKE)
    cancelled = 2,   // Resolved by wait-epoch cancellation (CANCEL)
    expired = 3,     // Resolved by deadline expiry (TIMER_EXPIRE, E11)
};
```

`WaitOutcome` is a four-value enum: `unresolved` is the only non-terminal
value, and `woken` / `cancelled` / `expired` are the three terminal outcomes
(absorbing — once terminal, the value does not change). `AsyncQueue<T>` does
NOT use `WaitOutcome`; it returns the typed `QueuePushResult<T>` /
`QueuePopResult<T>` whose `status()` carries `committed`/`item`/`closed`/
`expired`/`would_block`.

### `sluice::async::WaitNode`

One canonical wait lifecycle. Caller-owned, address-stable, non-copyable, non-movable.
One fresh `WaitNode` per wait epoch. The caller provides it to blocking operations
and queries `node.outcome()` after resume.

Fresh-per-epoch is enforced by the absorbing `WaitNode` state machine and the
registration precondition that registration succeeds only from `Detached`.
Deleted copy/move operations preserve object identity and address stability;
they do not by themselves prevent terminal-node reuse.

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

    // Public in the installed header for intrusive implementation access;
    // not a supported user-mutation surface. WaitQueue owns these under mtx_.
    WaitNode* next_{nullptr};
    WaitNode* prev_{nullptr};
    WaitQueue* home_{nullptr};
};
```

### `sluice::async::WaitQueue` and `sluice::async::TimerRegistration`

These are publicly nameable types in installed headers, but they are
Scheduler-integrated runtime substrate, not standalone user synchronization
primitives. `WaitQueue` exposes no public registration or resolution method;
those structural methods are private and `Scheduler` is the sole friend.

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

Persistent manual-reset async Event. Non-copyable, non-movable.

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
    void set();                          // broadcast to all registered waiters; ext-thread safe
    void reset();                        // does NOT cancel waiters
    void wait(WaitNode& node);           // Fiber-only; suspend until SET or cancel
    void wait_until(WaitNode& node, Scheduler::deadline_t deadline);  // Fiber-only
    [[nodiscard]] bool cancel(WaitNode& node);  // per-wait-epoch cancel; any thread
};
```

### `sluice::async::Semaphore`

Async counting Semaphore. Non-copyable, non-movable.

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

    [[nodiscard]] permit_count_t available() const noexcept;  // lock-free snapshot
    [[nodiscard]] bool try_acquire();          // no barging; any thread
    void acquire(WaitNode& node);              // Fiber-only
    void acquire_until(WaitNode& node, Scheduler::deadline_t deadline);  // Fiber-only
    [[nodiscard]] bool cancel(WaitNode& node); // per-wait-epoch cancel; any thread
    [[nodiscard]] bool release();              // transfer/store/overflow; ext-thread safe
};
```

### `sluice::async::AsyncMutex`

Fiber-suspending async Mutex. Non-copyable, non-movable. Ownership is `Fiber*` identity
(survives E8 work stealing).

```cpp
class AsyncMutex {
public:
    explicit AsyncMutex(Scheduler& scheduler) noexcept;
    ~AsyncMutex();  // assert(owner_ == nullptr)
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;

    [[nodiscard]] bool try_lock();              // Fiber-only; recursive→false
    void lock(WaitNode& node);                  // Fiber-only
    void lock_until(WaitNode& node, Scheduler::deadline_t deadline);  // Fiber-only
    [[nodiscard]] bool cancel(WaitNode& node);  // per-wait-epoch cancel; any thread
    void unlock();                              // Fiber-only; must be owner
};
```

### `sluice::async::AsyncCondition`

Fiber-suspending async condition variable. Bound to one `AsyncMutex` at construction.
Non-copyable, non-movable. Two-epoch protocol: Condition epoch + mandatory Mutex reacquire.

```cpp
class AsyncCondition {
public:
    explicit AsyncCondition(AsyncMutex& mutex) noexcept;
    ~AsyncCondition();  // assert(active_waits_ == 0)
    AsyncCondition(const AsyncCondition&) = delete;
    AsyncCondition& operator=(const AsyncCondition&) = delete;
    AsyncCondition(AsyncCondition&&) = delete;
    AsyncCondition& operator=(AsyncCondition&&) = delete;

    [[nodiscard]] WaitOutcome wait(WaitNode& condition_node);           // Fiber-only; must own Mutex
    [[nodiscard]] WaitOutcome wait_until(WaitNode& condition_node,      // Fiber-only; must own Mutex
                                          Scheduler::deadline_t deadline);
    [[nodiscard]] bool cancel(WaitNode& condition_node);  // per-Condition-epoch cancel; any thread
    void notify_one();                                    // any thread; non-persistent
    void notify_all();                                    // any thread; atomic snapshot-drain
};
```

### `sluice::async::AsyncQueue<T>`

Bounded MPMC FIFO channel. Non-copyable, non-movable. `T` must be an object type,
nothrow-move-constructible, and nothrow-destructible. `T` need NOT be default-constructible
or move-assignable.

`AsyncQueue<T>` v1 has **no public wait-epoch cancellation API** and **no
`Cancelled` result**. `close()` and deadline expiry are distinct Queue
state-machine causes (`closed` / `expired` statuses), not cancellation. There
is no `cancel(WaitNode&)` on `AsyncQueue<T>`; per-wait-epoch cancellation is
deferred to a future authority (see
`docs/history/closeout/e10-e12-api-semantic-closure.md` D4).

```cpp
template <class T>
class AsyncQueue final {
    static_assert(std::is_object_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);

public:
    explicit AsyncQueue(Scheduler& scheduler, std::size_t capacity);  // throws if capacity == 0
    ~AsyncQueue() = default;

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    // Fast paths (no suspend)
    [[nodiscard]] QueuePushResult<T> try_push(T value);
    [[nodiscard]] QueuePopResult<T> try_pop();
    void close() noexcept;  // idempotent, monotonic
    [[nodiscard]] bool is_closed() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // Blocking (Fiber-only)
    [[nodiscard]] QueuePushResult<T> push(T value);
    [[nodiscard]] QueuePushResult<T> push_until(T value, Scheduler::deadline_t deadline);
    [[nodiscard]] QueuePopResult<T> pop();
    [[nodiscard]] QueuePopResult<T> pop_until(Scheduler::deadline_t deadline);

    // Teardown (irreversible)
    detail::QueueTeardownSession begin_teardown() noexcept;
    T release_teardown(detail::QueueTeardownSession& session) noexcept;
};
```

The teardown type lives in `sluice::async::detail`, but it is part of the
publicly observable signature of `AsyncQueue<T>`:

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

**Result types (exact public members):**

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

Neither result template declares an explicit `requires` clause or
`static_assert`; the object/nothrow-move/nothrow-destruction constraints above
are constraints of `AsyncQueue<T>` itself. The result members remain declared
`noexcept` exactly as shown.

Both result types are move-only. Move-assignment uses destroy-and-rebuild so
`T` need not be move-assignable (PR #12 corrective).

### `sluice::async::AsyncRwLock`

Fiber-suspending async Read-Write Lock with writer-fair phase-batched scheduling.
Non-copyable, non-movable. Multiple concurrent readers OR one exclusive writer.

Writer-fair policy: new readers cannot barge past queued writers. When the queue
head is a reader, the maximal consecutive reader prefix is granted as one batch.

```cpp
class AsyncRwLock {
public:
    explicit AsyncRwLock(Scheduler& scheduler) noexcept;
    ~AsyncRwLock();  // assert(active_readers_==0, !writer_active_, waiters empty)
    AsyncRwLock(const AsyncRwLock&) = delete;
    AsyncRwLock& operator=(const AsyncRwLock&) = delete;
    AsyncRwLock(AsyncRwLock&&) = delete;
    AsyncRwLock& operator=(AsyncRwLock&&) = delete;

    [[nodiscard]] bool try_read_lock();              // any thread; fails if writer active or queue non-empty
    void read_lock(WaitNode& node);                  // Fiber-only
    void read_lock_until(WaitNode& node, Scheduler::deadline_t deadline);  // Fiber-only

    [[nodiscard]] bool try_write_lock();             // Fiber-only; recursive→false
    void write_lock(WaitNode& node);                 // Fiber-only
    void write_lock_until(WaitNode& node, Scheduler::deadline_t deadline); // Fiber-only

    void unlock_read() noexcept;                     // any thread; caller must hold read share
    void unlock_write() noexcept;                    // Fiber-only; must be writer owner

    [[nodiscard]] bool cancel(WaitNode& node);       // per-wait-epoch cancel; any thread
};
```

### Common Vocabulary

| Term | Meaning |
|------|---------|
| `wait-epoch` | One fresh `WaitNode` registration → one terminal outcome. Identified by `WaitNode` object identity. |
| `wait-epoch cancellation` | `cancel(WaitNode&)` resolves exactly one registered wait epoch. NOT task/Fiber/I/O cancellation. |
| `absolute monotonic deadline` | `Scheduler::deadline_t` = `uint64_t` monotonic ticks. `expired iff now >= deadline`. |
| `already-due deadline` | A deadline ≤ `monotonic_now()` at admission time. All primitives resolve inline without suspending. |
| `admission precedence` | Resource readiness checked BEFORE already-due deadline (resource-first), except AsyncCondition which uses deadline-first (already-due → Expired inline). |
| `registered race` | After registration, RESOURCE_WAKE / TIMER_EXPIRE / CANCEL compete through the single `WaitNode::resolve_` CAS. |
| `FIFO waiter selection` | Waiters are selected in FIFO registration order. Does NOT guarantee strict completion order. |
| `no barging` | `try_*` operations fail if a queued waiter has FIFO priority. |
| `destroy` | Destructor. Requires waiters empty (debug assert). Does NOT cancel/wake/clean up. |
| `close` | Monotonic Open→Closed (Queue only). Drains role FIFOs. |
| `begin_teardown` | Irreversible operational→tearing_down (Queue only). Returns `QueueTeardownSession`. |

### Thread Calling Boundaries

| Operation Class | Examples | Requires Fiber | Safe from Ext Thread |
|----------------|----------|---------------|---------------------|
| Blocking/timed wait | `wait`, `acquire`, `lock`, `push`, `pop` | Yes | No |
| Non-blocking try | `try_acquire`, `try_push`, `try_pop` | No | Yes (except `try_write_lock` — Fiber-only; requires current Fiber to record writer ownership) |
| Wake/notify | `set`, `release`, `notify_one`, `notify_all` | No | Yes |
| Cancel | `cancel` (all primitives with cancel) | No | Yes |
| Observation | `is_set`, `available`, `is_closed`, `capacity`, `size` | No | Yes |
| Construction/destruction | ctors, dtors | No | Yes (constructors); destruction requires quiescence (empty WaitQueue, no active condition waits, no mutex owner) |

---

## Async Runtime (E13+)

The async runtime types build on the E10–E12 synchronization substrate. See
`docs/architecture/async-runtime.md` and `docs/architecture/async-io-foundation.md`
for the architectural context.

### `sluice::async::Scheduler`

M:N fiber scheduler. Non-copyable, non-movable. Constructs against an
`AsyncIoContext&`. Owns Workers, the deadline heap, and the external-wake
control block.

```cpp
class Scheduler {
public:
    using deadline_t = std::uint64_t;  // monotonic ticks

    explicit Scheduler(AsyncIoContext& ctx) noexcept;
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Fiber lifecycle
    bool init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size);
    void spawn(Fiber& fiber) noexcept;
    void spawn_on(Fiber& fiber, unsigned worker_id) noexcept;

    // Run entry points
    void run(unsigned worker_count);
    void run_live(unsigned worker_count);
    void run_live(unsigned worker_count, bool (*stop_fn)(void*), void* stop_ctx);
    void run_until_idle();

    // E5/E6 completion awaits
    void await_completion_size(Completion<std::size_t>& c);
    void await_completion_void(Completion<void>& c);
    void await_ready_flag(const std::atomic<bool>& ready);

    // E10 WaitQueue suspension
    void await_wait(WaitQueue& q, WaitNode& node);
    bool wake_wait_one(WaitQueue& q);
    bool cancel_wait(WaitQueue& q, WaitNode& node);

    // E11 deadline / timer wait
    deadline_t monotonic_now() const noexcept;
    void advance_clock(deadline_t t);
    void await_wait_deadline(WaitQueue& q, WaitNode& node, deadline_t deadline);
    bool expire_wait(WaitQueue& q, WaitNode& node);

    // E12-A Event
    std::size_t event_set_broadcast(Event& event);
    void event_reset(std::atomic<bool>& set_flag);
    void await_event_wait(WaitQueue& q, const std::atomic<bool>& set_flag, WaitNode& node);
    void await_event_wait_deadline(WaitQueue& q, const std::atomic<bool>& set_flag,
                                   WaitNode& node, deadline_t deadline);
    bool event_cancel_wait(WaitQueue& q, WaitNode& node);

    // E12-B Semaphore
    [[nodiscard]] bool sem_try_acquire(WaitQueue& waiters, std::atomic<std::uint32_t>& available);
    void sem_acquire(WaitQueue& waiters, std::atomic<std::uint32_t>& available, WaitNode& node);
    void sem_acquire_until(WaitQueue& waiters, std::atomic<std::uint32_t>& available,
                           WaitNode& node, deadline_t deadline);
    [[nodiscard]] bool sem_cancel(WaitQueue& waiters, WaitNode& node);
    [[nodiscard]] bool sem_release(WaitQueue& waiters, std::atomic<std::uint32_t>& available,
                                   std::uint32_t max_permits);

    // E12-C AsyncMutex
    [[nodiscard]] bool mutex_try_lock(WaitQueue& waiters, Fiber*& owner);
    void mutex_lock(WaitQueue& waiters, Fiber*& owner, WaitNode& node);
    void mutex_lock_until(WaitQueue& waiters, Fiber*& owner, WaitNode& node, deadline_t deadline);
    [[nodiscard]] bool mutex_cancel(WaitQueue& waiters, WaitNode& node);
    void mutex_unlock(WaitQueue& waiters, Fiber*& owner);

    // E12-D AsyncCondition
    WaitOutcome condition_wait_prepare(WaitQueue& cond_waiters, WaitNode& cond_node,
                                       WaitQueue& mutex_waiters, Fiber*& owner, bool& released_mutex);
    WaitOutcome condition_wait_prepare_until(WaitQueue& cond_waiters, WaitNode& cond_node,
                                             WaitQueue& mutex_waiters, Fiber*& owner,
                                             deadline_t deadline, bool& released_mutex);
    void condition_notify_one(WaitQueue& cond_waiters);
    std::size_t condition_notify_all(WaitQueue& cond_waiters);
    [[nodiscard]] bool condition_cancel_wait(WaitQueue& cond_waiters, WaitNode& cond_node);

    // E12-E Queue
    void queue_push_admit(detail::QueuePort& port, WaitNode& node, detail::QueueItemLease& lease);
    void queue_pop_admit(detail::QueuePort& port, WaitNode& node, detail::QueueItemLease& out);
    void queue_push_admit_until(detail::QueuePort& port, WaitNode& node,
                                detail::QueueItemLease& lease, deadline_t deadline);
    void queue_pop_admit_until(detail::QueuePort& port, WaitNode& node,
                               detail::QueueItemLease& out, deadline_t deadline);
    [[nodiscard]] bool queue_cancel(detail::QueuePort& port, detail::QueueRole role, WaitNode& node);

    // E12-F AsyncRwLock
    [[nodiscard]] bool rwlock_try_read_lock(WaitQueue& waiters, std::size_t& active_readers, bool& writer_active);
    void rwlock_read_lock(WaitQueue& waiters, std::size_t& active_readers, bool& writer_active, WaitNode& node);
    void rwlock_read_lock_until(WaitQueue& waiters, std::size_t& active_readers,
                                bool& writer_active, WaitNode& node, deadline_t deadline, void* expire_ctx);
    [[nodiscard]] bool rwlock_try_write_lock(WaitQueue& waiters, std::size_t& active_readers,
                                             bool& writer_active, Fiber*& writer_owner);
    void rwlock_write_lock(WaitQueue& waiters, std::size_t& active_readers,
                           bool& writer_active, Fiber*& writer_owner, WaitNode& node);
    void rwlock_write_lock_until(WaitQueue& waiters, std::size_t& active_readers,
                                 bool& writer_active, Fiber*& writer_owner,
                                 WaitNode& node, deadline_t deadline, void* expire_ctx);
    void rwlock_unlock_read(WaitQueue& waiters, std::size_t& active_readers,
                            bool& writer_active, Fiber*& writer_owner);
    void rwlock_unlock_write(WaitQueue& waiters, std::size_t& active_readers,
                             bool& writer_active, Fiber*& writer_owner);
    [[nodiscard]] bool rwlock_cancel(WaitQueue& waiters, std::size_t& active_readers,
                                     bool& writer_active, Fiber*& writer_owner, WaitNode& node);

    // E9 external wake
    SchedulerWakeHandle make_wake_handle() noexcept;
    void attach_ready_wake(const std::atomic<bool>& ready, SchedulerWakeHandle& wh);

    // Diagnostics
    std::size_t runnable_count() const;
    std::size_t waiting_count() const;
    std::size_t waiting_ready_count() const;
    static unsigned current_worker_id();
    WorkerState* owner_of(const Fiber& f) const;
    unsigned owner_id_of(const Fiber& f) const;
};
```

`select()` is a **free function** (not a `Scheduler` member):

```cpp
template <class... Cases>
    requires (
        sizeof...(Cases) >= 1 &&
        sizeof...(Cases) <= kSelectMaxArms &&
        (SelectCaseType<Cases> && ...)
    )
SelectResult select(Scheduler& scheduler, Cases&&... cases);
```

### `sluice::async::SchedulerWakeHandle`

External-wake handle. Move-only. Lets an external producer thread wake a
parked Scheduler Worker without holding a raw `Scheduler*`.

```cpp
class SchedulerWakeHandle {
public:
    SchedulerWakeHandle() = default;
    SchedulerWakeHandle(SchedulerWakeHandle&&) noexcept = default;
    SchedulerWakeHandle& operator=(SchedulerWakeHandle&&) noexcept = default;
    SchedulerWakeHandle(const SchedulerWakeHandle&) = delete;
    SchedulerWakeHandle& operator=(const SchedulerWakeHandle&) = delete;

    bool notify() noexcept;
    bool bound() const noexcept;
};
```

### `sluice::async::Fiber`

Logical task. Non-copyable, non-movable. The caller provides an entry
function and (for Evented mode) a stack. `Fiber` is **not** itself a
cancel-propagation boundary — `Group` is (see below).

```cpp
class Fiber {
public:
    using Entry = std::function<void(Fiber&)>;

    Fiber() = default;
    explicit Fiber(Entry entry);
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    FiberState state() const noexcept;
    bool make_runnable() noexcept;
    void make_running() noexcept;
    void make_waiting() noexcept;
    void make_done() noexcept;

    Entry& entry() noexcept;
    const Entry& entry() const noexcept;
    void set_entry(Entry e);
    CancelToken& cancel_token() noexcept;
    CancelState& cancel_state() noexcept;

    fiber_ctx::Context ctx{};  // public data member
};
```

### `sluice::async::CancelToken` / `sluice::async::CancelState` / `sluice::async::CancelGuard`

Cooperative cancellation primitives. `CancelToken` is the cancel-request
state; `CancelState` is per-consumer protection/acknowledge state.

```cpp
class CancelToken {
public:
    CancelToken() = default;
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) = delete;
    CancelToken& operator=(CancelToken&&) = delete;

    void request() noexcept;       // idempotent cancel request
    bool is_requested() const noexcept;
    void rearm() noexcept;
    void clear() noexcept;
};

enum class CancelProtection : std::uint8_t { unblocked, blocked };

class CancelState {
public:
    CancelProtection protection() const noexcept;
    CancelProtection swap_protection(CancelProtection next) noexcept;
    bool acknowledged() const noexcept;
    void acknowledge() noexcept;
    void reset_acknowledgement() noexcept;
};

class CancelGuard {
public:
    CancelGuard(CancelState& state, CancelProtection next) noexcept;
    ~CancelGuard();
    CancelGuard(CancelGuard&&) noexcept = default;
    CancelGuard(const CancelGuard&) = delete;
    CancelGuard& operator=(const CancelGuard&) = delete;
    CancelGuard& operator=(CancelGuard&&) = delete;
};
```

The cancellation point is the free function:

```cpp
Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;
```

### `sluice::async::Future<T>`

Single-task awaitable (E28). Producer side: `complete_with()`. Consumer side:
`await()` / `cancel()`. The physical wait delegates to a `WaitPolicy&`
(injected at construction). `Future()` uses the `default_wait_policy()`
(Threaded). `explicit Future(WaitPolicy& policy)` injects an Evented policy.

```cpp
template <class T>
class Future {
public:
    Future();
    explicit Future(WaitPolicy& policy);
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) = delete;
    Future& operator=(Future&&) = delete;

    void complete_with(Result<T> r);
    CancelToken& cancel_token() noexcept;
    Result<T> await();
    Result<T> cancel();
    bool ready() const noexcept;
};
```

### `sluice::async::Group`

Unordered task set (E29). Tasks are added via `async(fn)`; the group is awaited
as a whole or canceled as a whole. **Cancel-propagation boundary**: tasks swallow
`IoError::canceled`. The physical wait delegates to a `WaitPolicy&` (Scheduler
for Evented, default Threaded otherwise).

```cpp
class Group {
public:
    Group() = default;
    explicit Group(Scheduler& sched);
    ~Group();
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&) = delete;
    Group& operator=(Group&&) = delete;

    template <class Fn>
    void async(Fn fn);

    CancelToken& group_token() noexcept;
    void await();
    void cancel();

    std::size_t size() const noexcept;
};
```

### `sluice::async::WaitPolicy`

Abstract policy that decides how `Future<T>` and `Group` physically wait.
The async primitives (`Event`, `Semaphore`, `AsyncMutex`, `AsyncCondition`,
`AsyncRwLock`, `AsyncQueue`) do **not** use `WaitPolicy` — they suspend fibers
directly via `Scheduler` members.

```cpp
class WaitPolicy {
public:
    virtual ~WaitPolicy() = default;
    WaitPolicy(const WaitPolicy&) = delete;
    WaitPolicy& operator=(const WaitPolicy&) = delete;

    virtual void wait_until_ready(const std::atomic<bool>& ready,
                                  std::mutex& mtx,
                                  std::condition_variable& cv) = 0;
    virtual void notify_ready() noexcept {}
};

class ThreadedWaitPolicy : public WaitPolicy {
public:
    void wait_until_ready(const std::atomic<bool>& ready, std::mutex& mtx,
                          std::condition_variable& cv) override;
};

class EventedWaitPolicy final : public WaitPolicy {
public:
    explicit EventedWaitPolicy(Scheduler& scheduler) noexcept;
    void wait_until_ready(const std::atomic<bool>& ready, std::mutex&,
                          std::condition_variable&) override;
    void notify_ready() noexcept override;
};

WaitPolicy& default_wait_policy() noexcept;
```

### `sluice::async::SelectResult`

E13 Select winner descriptor. `SelectKind` is `event` or `timer`.
`SelectTimerOutcome` is `fired` (timer winner only).

```cpp
class SelectResult {
public:
    constexpr SelectResult() noexcept = default;

    [[nodiscard]] constexpr bool has_winner() const noexcept;
    [[nodiscard]] constexpr std::size_t index() const noexcept;
    [[nodiscard]] constexpr SelectKind kind() const noexcept;
    [[nodiscard]] constexpr SelectTimerOutcome timer_outcome() const noexcept;
};
```

Select arms:

```cpp
class EventSelectCase {
public:
    explicit EventSelectCase(Event& event) noexcept;
};

class TimerSelectCase {
public:
    explicit TimerSelectCase(Scheduler& scheduler, select_deadline_t deadline) noexcept;
};

inline constexpr std::size_t kSelectMaxArms = 8;
```

### `sluice::async::Completion<T>`

Single outstanding operation's state (E17). Caller-owned, address-stable,
non-copyable, non-movable. `result()` / `reset()` / `complete_with()` /
`mark_outstanding()` are **not** `noexcept`.

```cpp
template <class T>
class Completion {
public:
    Completion() = default;
    ~Completion() = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    bool ready() const noexcept;
    bool outstanding() const noexcept;
    bool idle() const noexcept;
    Result<T> result() const;           // valid only when ready
    void mark_outstanding();
    void complete_with(Result<T> res);
    void reset();
};
```

`detail::next_reap_seq()` is a free function in `sluice::async::detail`, not
part of the public `Completion` surface.

### `sluice::async::AsyncBackend`

Internal backend boundary (ADR §4). `AsyncIoContext` delegates to it. Concrete
backends implement `submit_*` / `poll` / `wait_one`. `wait_one()` returns
`Result<std::size_t>`; `cancel()` has two overloads.

```cpp
class AsyncBackend {
public:
    virtual ~AsyncBackend() = default;
    AsyncBackend(const AsyncBackend&) = delete;
    AsyncBackend& operator=(const AsyncBackend&) = delete;

    void attach_stats(AsyncStats* s);

    virtual Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) = 0;
    virtual Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) = 0;

    virtual std::size_t poll() = 0;               // non-blocking reap
    virtual Result<std::size_t> wait_one() = 0;   // blocking reap

    virtual void cancel(Completion<std::size_t>& c);
    virtual void cancel(Completion<void>& c);

    virtual std::size_t outstanding() const noexcept = 0;
};
```

Op descriptors (public structs in the same header):

```cpp
struct ReadOp    { int fd = -1; std::byte* dst = nullptr; std::size_t len = 0; std::uint64_t offset = 0; };
struct WriteOp   { int fd = -1; const std::byte* src = nullptr; std::size_t len = 0; std::uint64_t offset = 0; };
struct SyncDataOp{ int fd = -1; };
struct SyncAllOp { int fd = -1; };
```

### `sluice::async::AsyncIoContext`

Public L1 async API surface (E17). Owns an `AsyncBackend` via
`std::unique_ptr`. Non-copyable; move-semantic. `wait_one()` returns the
reap count for that wait, **not** the op's byte result — read the op result
from the `Completion` after it is ready.

```cpp
class AsyncIoContext {
public:
    explicit AsyncIoContext(std::unique_ptr<AsyncBackend> backend,
                            AsyncStats* stats = nullptr);
    ~AsyncIoContext();
    AsyncIoContext(const AsyncIoContext&) = delete;
    AsyncIoContext& operator=(const AsyncIoContext&) = delete;
    AsyncIoContext(AsyncIoContext&&) noexcept;
    AsyncIoContext& operator=(AsyncIoContext&&) noexcept;

    // Op submission
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    // Reap
    std::size_t poll();              // non-blocking
    Result<std::size_t> wait_one();  // blocking; returns count reaped

    // Cancel
    void cancel(Completion<std::size_t>& c);
    void cancel(Completion<void>& c);

    // Stats / observation
    std::size_t outstanding() const noexcept;
    const AsyncStats* stats() const noexcept;
};
```

### `sluice::async::BatchOp` / `sluice::async::BatchResult` / `sluice::async::Batch`

Grouped completions (E30). `Batch` has a **default** constructor (no
`AsyncIoContext&` parameter). `add()` takes a `BatchOp`. `await_one()` takes
the `AsyncIoContext&` to reap through and returns `Result<std::size_t>` (the
reap count). `next()` iterates in reap order. **No `cancel()` method.**

```cpp
struct BatchOp {
    ReadOp read{};
    WriteOp write{};
    SyncDataOp sync_data{};
    SyncAllOp sync_all{};
    enum class Kind : std::uint8_t { read, write, sync_data, sync_all } kind = Kind::read;
};

struct BatchResult {
    std::size_t index = 0;
    bool is_void = false;
    std::optional<Result<std::size_t>> size_res;
    std::optional<Result<void>> void_res;
};

class Batch {
public:
    Batch() = default;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    Batch(Batch&&) = delete;
    Batch& operator=(Batch&&) = delete;

    std::size_t add(BatchOp op);
    Result<std::size_t> await_one(AsyncIoContext& ctx);
    std::optional<BatchResult> next() noexcept;
    std::size_t pending_count() const noexcept;
};
```

### `sluice::async::ThreadPoolBackend`

Portable real backend. One `std::thread` per outstanding op. Always buildable;
no external dependency. Construct directly; there is no factory function.

```cpp
class ThreadPoolBackend : public AsyncBackend {
public:
    ThreadPoolBackend();
    ~ThreadPoolBackend() override;
    // ... AsyncBackend implementation
    void shutting_down_for_test();
};
```

### `sluice::async::UringAsyncBackend`

Linux io_uring backend. Build-gated behind `SLUICE_HAS_LIBURING`. Without
liburing, it is an unsupported stub: `submit_*` returns
`IoError::backend_error` synchronously; `poll()` / `wait_one()` reap nothing.
Construct directly; there is no factory function.

```cpp
class UringAsyncBackend : public AsyncBackend {
public:
    explicit UringAsyncBackend(unsigned queue_depth = 64);
    ~UringAsyncBackend() override;
    // ... AsyncBackend implementation (real path gated by SLUICE_HAS_LIBURING)
    bool available() const noexcept;
};
```

### `sluice::async::FakeAsyncBackend`

Deterministic test backend. Construct directly and configure `auto_bytes()`,
`auto_error()`, `auto_eof()`, `auto_short_then_full()`; or drive
`complete_oldest_with_bytes()` / `complete_oldest_with_error()` manually.

```cpp
class FakeAsyncBackend : public AsyncBackend {
public:
    FakeAsyncBackend() = default;
    ~FakeAsyncBackend() override = default;

    void auto_bytes(std::size_t n);
    void auto_error(IoError e);
    void auto_eof();
    void auto_disable();
    void auto_short_then_full(std::size_t first_short);

    void complete_oldest_with_bytes(std::size_t n);
    void complete_oldest_with_error(IoError e);
    void complete_oldest_sync_ok();
    void complete_oldest_sync_error(IoError e);

    // ... AsyncBackend implementation
};
```

### Free functions (I/O coordinators)

```cpp
Result<std::size_t> read_all(AsyncIoContext& ctx, int fd, std::span<std::byte> dst,
                             std::uint64_t offset);
Result<std::size_t> write_all(AsyncIoContext& ctx, int fd, std::span<const std::byte> src,
                              std::uint64_t offset);
Result<void> sync_data_all(AsyncIoContext& ctx, int fd);
Result<void> sync_all_all(AsyncIoContext& ctx, int fd);
```

## Measurement

All stats structs are caller-owned, default-initialized to zero, and attached via nullable pointer (null = no counting).

| Struct | Tracks |
|---|---|
| `SyscallStats` | POSIX read/write syscall counts and bytes |
| `BufferStats` | Buffer hit/miss/refill activity |
| `CopyStats` | Copy loop iterations, byte counts, stop reasons, strategy selection |
| `VectorStats` | read_vec/write_vec calls, bytes, fallback counts |
| `SyncStats` | sync_data/sync_all calls and errors |
| `UringStats` | Experimental io_uring queue/submit/completion counts |
