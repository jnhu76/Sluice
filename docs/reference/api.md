# API Reference

Public, stable-ish APIs as of v0.1-mvp. "Stable-ish" means removing or silently re-semanticizing these would break consumers; treat as frozen across minor work, change only with deliberate deprecation.

For internal details and experimental APIs, see `docs/history/archive/api-audit.md`.

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
        invalid_argument, // malformed operation descriptor (ADR-explicit-io-request-contract Decision 6)
        not_found,        // stale/unknown RequestKey (cancel/reap lookup)
        not_supported,    // backend/platform does not provide the op or cancel capability
    };
    Code code;
    int os_errno = 0;     // preserved POSIX errno (0 if not applicable)
};
```

`invalid_argument`, `not_found`, and `not_supported` are introduced by the explicit I/O
request contract (ADR-explicit-io-request-contract, Decision 6). They keep admission
rejection, stale-key lookup, and capability refusal distinct from one another and from
configured-capacity `would_block`, lifecycle `invalid_state`, and genuine-init `no_space`.
Phase B reference backends (FakeAsyncBackend, SyncBackend) emit them; the cancel disposition
lookup returns `not_found` for an absent or stale generation rather than overloading
`invalid_state`.

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

    // ================================================================
    // Supported user API
    // ================================================================
    explicit Scheduler(AsyncIoContext& ctx,
                       std::size_t wait_capacity = 256);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Fiber lifecycle
    bool init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size);
    void spawn(Fiber& fiber) noexcept;

    // Run entry points
    void run(unsigned worker_count);
    void run_live(unsigned worker_count);
    void run_until_idle();

    // E5/E6 completion awaits
    void await_completion_size(Completion<std::size_t>& c);
    void await_completion_void(Completion<void>& c);

    // E9 external wake
    SchedulerWakeHandle make_wake_handle() noexcept;

    // Diagnostics
    std::size_t runnable_count() const;
    std::size_t waiting_count() const;
    std::size_t waiting_ready_count() const;
    static unsigned current_worker_id();

    // ================================================================
    // Test-only seams — NOT supported user API
    // ================================================================
    void spawn_on(Fiber& fiber, unsigned worker_id) noexcept;  // deterministic-test hook
    void advance_clock(deadline_t t);  // TEST-ONLY causal seam (M7)
};
```

Worker topology is established at each run entry under the Scheduler
coordination lock. WorkerState storage grows monotonically and remains
address-stable; an invocation uses an immutable snapshot of its first
`worker_count` workers. `spawn()` before or between run invocations is deferred
to `pending_spawn_`, then assigned among the next invocation's participating
workers. During an active invocation, `spawn()` routes only among that
invocation's participants; after termination is published, new work is
deferred to the next invocation rather than routed to an exiting worker.
When a suspended Fiber survives a Drain STALLED boundary, its recorded owner
may belong to a retained WorkerState outside a later smaller invocation.
Runnable publication then rebinds the Fiber to one of the later invocation's
participants under the Scheduler coordination lock, or defers the ticket until
the next invocation if no worker can accept it.

**Installed runtime substrate (not direct user API).** The following are
public in the installed header due to encapsulation boundaries but are NOT
part of the supported user contract. They are used internally by the async
primitives (`Event`, `Semaphore`, `AsyncMutex`, `AsyncCondition`,
`AsyncQueue<T>`, `AsyncRwLock`) to drive the Scheduler's internal state
machine. Production code MUST NOT call them directly:

- `await_ready_flag`, `await_wait`, `wake_wait_one`, `cancel_wait` (WaitQueue seams)
- `monotonic_now`, `await_wait_deadline`, `expire_wait` (E11 timer seams)
- `event_set_broadcast`, `event_reset`, `await_event_wait`, `await_event_wait_deadline`, `event_cancel_wait` (E12-A)
- `sem_try_acquire`, `sem_acquire`, `sem_acquire_until`, `sem_cancel`, `sem_release` (E12-B)
- `mutex_try_lock`, `mutex_lock`, `mutex_lock_until`, `mutex_cancel`, `mutex_unlock` (E12-C)
- `condition_wait_prepare`, `condition_wait_prepare_until`, `condition_notify_one`, `condition_notify_all`, `condition_cancel_wait` (E12-D)
- `queue_push_admit`, `queue_pop_admit`, `queue_push_admit_until`, `queue_pop_admit_until`, `queue_cancel` (E12-E, accept `detail::QueuePort&` / `detail::QueueItemLease&`)
- `rwlock_try_read_lock`, `rwlock_read_lock`, `rwlock_read_lock_until`, `rwlock_try_write_lock`, `rwlock_write_lock`, `rwlock_write_lock_until`, `rwlock_unlock_read`, `rwlock_unlock_write`, `rwlock_cancel` (E12-F)
- `attach_ready_wake`, `owner_of`, `owner_id_of` (internal diagnostics)
- `run_live(unsigned, bool(*)(void*), void*)` — Group-scoped live invocation with raw predicate lifetime constraints

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
state; `CancelState` is per-consumer protection/acknowledge state. The token
carries a request **epoch** (generation) alongside the pending bit
(ADR-cancel-request-epoch): acknowledgement is relative to a specific request,
so `rearm()` re-arms delivery for every consumer of a shared token, while
`CancelState::reset_acknowledgement()` re-arms a single consumer.

```cpp
class CancelToken {
public:
    CancelToken() = default;
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) = delete;
    CancelToken& operator=(CancelToken&&) = delete;

    void request() noexcept;       // idempotent cancel request (no re-arm when already pending)
    bool is_requested() const noexcept;
    std::uint64_t epoch() const noexcept;  // identity of the current request
    void rearm() noexcept;         // re-arm delivery of the pending request (Zig recancel)
    void clear() noexcept;         // remove the pending request (token reuse)
};

enum class CancelProtection : std::uint8_t { unblocked, blocked };

class CancelState {
public:
    CancelProtection protection() const noexcept;
    CancelProtection swap_protection(CancelProtection next) noexcept;
    bool acknowledged(const CancelToken& token) const noexcept;  // delivered the token's current request?
    void acknowledge(const CancelToken& token) noexcept;         // record delivery of the token's current request
    void reset_acknowledgement() noexcept;                       // per-consumer re-arm
};

class CancelGuard {
public:
    CancelGuard(CancelState& state, CancelProtection next) noexcept;
    ~CancelGuard();
    CancelGuard(CancelGuard&& other) noexcept;  // NOT defaulted — nulls source state
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

Accessors follow the Completion L9 pattern (AGENTS.md §9.2): calling them on
a no-winner (or, for `timer_outcome()`, non-timer) result is a caller
contract violation — Debug asserts; Release returns the deterministic
fallback (`index() == 0`, `kind() == SelectKind::event`,
`timer_outcome() == SelectTimerOutcome::fired`) instead of stale data.
Check `has_winner()` first.

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
non-copyable, non-movable (ADR-explicit-io-completion-authority). Publication
mutators (`try_claim`, `publish`, `rollback_claim_before_accept`) are PRIVATE
— ordinary non-backend callers cannot forge state transitions. `result()` is
not `noexcept`; `reset()` is `noexcept`.

`Completion<T>` is an asynchronous terminal-publication cell; its value type `T`
must be nothrow default-constructible, copy-constructible, nothrow
move-assignable, and nothrow destructible (compile-enforced by `static_assert`
on the template). `result()` returns the stored result by value — it copies it
out, it does not move it out; the Completion keeps its copy until `reset()`.
`Completion<void>` carries no value, so these traits do not apply.

```cpp
template <class T>
class Completion {
public:
    Completion() = default;
    ~Completion();                      // fail-fast if outstanding/publishing/resetting
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    bool ready() const noexcept;
    bool outstanding() const noexcept;  // true for outstanding or transient publishing
    bool idle() const noexcept;
    Result<T> result() const;           // valid only when ready
    void reset() noexcept;              // ready → resetting → idle; idle → no-op;
                                        // outstanding/publishing/resetting → fail-fast
};
```

Phase B (ADR-explicit-io-request-contract, Accepted, Decision 15): `reset()` from
`ready` is also the slot-release handshake for a request accepted through the
reference backends (FakeAsyncBackend, SyncBackend). The slot bound at commit
remains in use (`slot_in_use` accounting) until `reset()` — or ready-Completion
destruction — returns it to the bounded arena with `generation++`. The context/
backend must therefore outlive every bound slot: destroying a context (or arena)
while any slot is still bound fails fast in Debug and Release. The release uses
the completed-binding authority (`release_completed_binding`): ANY release
failure (stale handle, live enqueue pin, open waiter registration, wrong slot
state) is an internal protocol violation and fails fast — a silently-failed
release would let the `Completion` become reusable while its old slot stays
permanently in use.

`detail::next_reap_seq()` is a free function in `sluice::async::detail`, not
part of the public `Completion` surface.

### `sluice::async::AsyncBackend`

Public backend extension point (ADR §4). `AsyncIoContext` delegates to it.
Concrete backends implement `submit_*` / `poll` / `wait_one`. `wait_one()`
returns `Result<std::size_t>`; `cancel()` has two overloads. Any class that
derives `AsyncBackend` is a trusted backend-author: it inherits the protected
`try_claim()` / `publish()` / `rollback_claim_before_accept()` helpers, the
only sanctioned way to claim a `Completion` and publish a terminal result.

Issue #67 adds the OPTIONAL split-phase wait capability. A backend may
override `wait_source()` to expose a `BackendWaitSource` (snapshot /
`wait_for_change` / `interrupt_all`) — an observe-only readiness wait that
never reaps or publishes, may run concurrently with serialized consuming
operations, and can be interrupted by the control plane. The default is
`nullptr` (source-compatible for existing external backends): such backends
keep the legacy serialized `wait_one` contract, where the whole blocking call
runs under the context's serialized access domain. `wait_one_is_nonblocking()`
(default false) declares that a backend's `wait_one` never blocks (the
reference backends, whose readiness is produced synchronously inside poll).
`ApplicationRuntime` accepts a backend only if it provides the split wait
capability OR the non-blocking contract, and rejects anything else with
`invalid_state` — the multi-participant runtime path must never take a
BLOCKING serialized wait (a participant parked while holding the context lock
starves every other poll/reap path and deadlocks drain).

Phase G review P1b (PR #108) makes BOUNDED parking a separate, truthful
capability on `BackendWaitSource`: `supports_bounded_wait()` (default false)
reports whether the bounded `wait_for_change(observed, max_park)` overload
actually bounds the physical park — the base implementation of that overload
does NOT honor the bound. `AsyncIoContext::wait_one(max_park)` therefore
REJECTS a finite cap with a synchronous `not_supported` (no park, no
accounting side effect) when the wait source lacks the capability instead of
silently parking unbounded past the caller's deadline; the capability query
`AsyncIoContext::has_bounded_split_wait_capability()` composes split wait AND
bounded transport for callers with a deadline obligation (the Scheduler's
MW-S2 park-domain routing). The in-tree sources (ReadyWaitSource:
`cv.wait_for`; UringWaitSource: poll timeout) report true. Note for
out-of-tree `BackendWaitSource` subclasses: `supports_bounded_wait()` is a
NEW virtual inserted among the existing ones — source-compatible (the
default returns false, preserving the pre-Phase-G behavior) but not
layout/ABI-compatible; Sluice is an experimental library and does not
promise vtable stability across releases.

Phase B (ADR-explicit-io-request-contract, Accepted, Decision 5) adds the
protected two-stage **binding** helpers (`begin_binding` / `commit_binding` /
`rollback_binding_before_accept`) used by the migrated backends (Fake/Sync, and
— as of Phase E — ThreadPool) to install the RequestKey/context/release-
capability payload before the `Completion` becomes observable as outstanding.
The legacy single-step `try_claim` remains for the not-yet-migrated backends
(Uring); the two paths do not race because a given `Completion` is driven by
exactly one backend.

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

    // Issue #67 split-phase wait capability (optional, default absent). A
    // backend that provides it lets AsyncIoContext::wait_one park for readiness
    // WITHOUT holding access_mtx_ (pure observation, interruptible by the
    // control plane). Backends that return nullptr keep the legacy serialized
    // wait_one contract: the whole blocking wait runs under access_mtx_ — safe
    // when the backend's wait_one never blocks (see wait_one_is_nonblocking)
    // or when the caller is the single documented driver. Source-compatible:
    // existing external backends do not override it.
    virtual BackendWaitSource* wait_source() noexcept { return nullptr; }

    // Issue #67 (D3): whether wait_one() is guaranteed NON-BLOCKING (returns
    // immediately with whatever is currently reaped — e.g. the reference
    // backends, whose readiness is produced synchronously inside poll).
    // ApplicationRuntime requires EITHER a split wait capability OR this
    // non-blocking contract: the multi-participant runtime path must never
    // take a BLOCKING legacy wait_one (a participant parked while holding
    // access_mtx_ starves every other poll/reap path and deadlocks drain).
    // The default (false) is the conservative choice for external backends.
    virtual bool wait_one_is_nonblocking() const noexcept { return false; }

    virtual std::size_t outstanding() const noexcept = 0;

protected:
    // Legacy single-step claim (Uring/ThreadPool).
    template <class T> static bool try_claim(Completion<T>& c) noexcept;
    // Phase B two-stage binding (Fake/Sync — migrated reference backends).
    template <class T> static bool begin_binding(Completion<T>& c) noexcept;             // idle -> binding
    template <class T> static void commit_binding(Completion<T>& c) noexcept;            // binding -> outstanding (submit-success LP)
    template <class T> static void rollback_binding_before_accept(Completion<T>& c) noexcept; // binding -> idle
    template <class T> static void publish(Completion<T>& c, Result<T>&& result) noexcept;
    template <class T> static void rollback_claim_before_accept(Completion<T>& c) noexcept;
};
```

Op descriptors (public structs in the same header):

```cpp
struct ReadOp    { int fd = -1; std::byte* dst = nullptr; std::size_t len = 0; std::uint64_t offset = 0; };
struct WriteOp   { int fd = -1; const std::byte* src = nullptr; std::size_t len = 0; std::uint64_t offset = 0; };
struct SyncDataOp{ int fd = -1; };
struct SyncAllOp { int fd = -1; };
```

### `sluice::async::RequestHandle` / `sluice::async::RequestHandleState`

Opaque public identity for one accepted I/O request (Phase F3 /
ADR-public-request-handle). **Identity, not ownership**: copying a handle
allocates nothing and does not extend the Completion, fd/buffer borrow, slot,
or routing-lease lifetime. Construction-controlled (non-forgeable): the only
producer of a valid handle is a successful `submit_*_request`; the identity
components are private (enforced by a negative-compile gate). A handle stays
valid while its generation is the current occupant of its slot in its context;
after the Completion is reset/destroyed or the slot is reused, the handle is
inert (`request_state() == not_found`).

```cpp
enum class RequestHandleState : std::uint8_t {
    outstanding,        // accepted, not yet terminal
    backend_ready,      // terminal won, not yet reaped to Completion-ready
    completion_ready,   // reaped; Completion::ready()
    not_found,          // stale / wrong context / released / invalid
};

class RequestHandle {
public:
    constexpr RequestHandle() noexcept = default;          // invalid handle
    constexpr bool valid() const noexcept;
    friend bool operator==(const RequestHandle&, const RequestHandle&) noexcept = default;
};
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

    // Identity-returning submission (Phase F3 / ADR-public-request-handle).
    // Additive: on success returns the accepted request's RequestHandle
    // (Decision 4: success => exactly one valid handle); on synchronous
    // rejection returns the error and NO handle; not_supported (with no side
    // effect) if the backend lacks the identity contract
    // (supports_request_identity() == false).
    Result<RequestHandle> submit_read_request(ReadOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_write_request(WriteOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_sync_data_request(SyncDataOp op, Completion<void>& c);
    Result<RequestHandle> submit_sync_all_request(SyncAllOp op, Completion<void>& c);

    // Read-only identity consumer: the request's lifecycle state, or not_found
    // for a stale / cross-context / released / invalid handle. not_supported
    // for a backend without the identity contract. Never mutates the request,
    // registers a waiter, or cancels I/O.
    Result<RequestHandleState> request_state(const RequestHandle& h) const;

    // Reap
    std::size_t poll();              // non-blocking
    // Blocking; returns the count of Completions reaped. With a
    // split-wait-capable backend (wait_source() != nullptr) the wait NEVER
    // holds the serialized access domain: the call loops
    //   snapshot -> poll (serialized) -> park in the observe-only ready wait
    // so other participants' poll/reap paths stay reachable (issue #67).
    // A successful return of 0 means the wait was interrupted by the control
    // plane (close_admission / interrupt_backend_waiters) with nothing
    // reaped, or that no work was outstanding — 0 is NOT an error and never
    // fabricates a completion. Without the capability the legacy serialized
    // contract applies (the whole call, including a backend-side block, runs
    // under the serialized access domain).
    Result<std::size_t> wait_one();
    // Phase G bounded-park variant: identical semantics, with each physical
    // park capped at `max_park` so a deadline-driven caller re-drains in
    // time. A wait source WITHOUT the bounded transport
    // (supports_bounded_wait() == false) gets a synchronous not_supported —
    // never a silently discarded bound (PR #108 review P1b). Check
    // has_bounded_split_wait_capability() first when a deadline obligation
    // exists.
    Result<std::size_t> wait_one(std::chrono::nanoseconds max_park);
    // Capability queries: split wait (wait_source() != nullptr), and split
    // wait AND bounded physical parking. The latter gates deadline-bound
    // backend-domain parks (the Scheduler's MW-S2 routing).
    bool has_split_wait_capability() const noexcept;
    bool has_bounded_split_wait_capability() const noexcept;

    // Control-plane wake (issue #67): unblocks every participant parked in
    // wait_one()'s observe phase so shutdown / admission close can
    // re-evaluate. No-op for backends without the split wait capability.
    void interrupt_backend_waiters() noexcept;

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

// Admission origin (Phase F2 / ADR Decision 9): orthogonal to success/error.
//   rejected               — submit failed before commit/accept (no accepted
//                            request existed). The Result carries the rejection.
//   accepted_and_completed — submit crossed commit and later reached a terminal
//                            result via reap (success, error, or canceled).
enum class BatchResultOrigin : std::uint8_t {
    rejected,
    accepted_and_completed,
};

struct BatchResult {
    std::size_t index = 0;
    BatchResultOrigin origin = BatchResultOrigin::accepted_and_completed;
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

Portable real blocking-I/O backend. Always buildable; no external dependency.
Construct directly; there is no factory function.

Phase E (ADR-explicit-io-request-contract, Accepted): `ThreadPoolBackend` is now
a **bounded explicit-I/O backend** — the first production backend to drive real
POSIX syscalls through the `detail::RequestArena` / `RequestSlot` lifecycle. It
uses a fixed pool of persistent blocking-I/O workers and a construction-time
bounded dispatch ring, with `RequestArena` as the single request-lifecycle
authority. The legacy "one `std::thread` per op + `std::function` + `Completion*`
ready deque" model (DIV-03 / DIV-12) is gone. Workers record `backend-ready`
ONLY; reap (`poll` / `wait_one`) is the sole `Completion`-ready publication
authority. `cancel` drives the shared state machine (pending/enqueued cancel may
win the canceled terminal under Scheme B; running cancel records intent only and
the real result wins verbatim — DIV-10). The public submit signatures are
unchanged (ADR Decision 7).

```cpp
struct ThreadPoolConfig {
    std::size_t request_capacity = 64;  // arena capacity == dispatch ring capacity
    std::size_t worker_count = 4;       // persistent blocking-I/O workers
};

class ThreadPoolBackend : public AsyncBackend {
public:
    ThreadPoolBackend();                           // ThreadPoolConfig{} defaults
    explicit ThreadPoolBackend(ThreadPoolConfig config);
    ~ThreadPoolBackend() override;                 // quiescent; joins workers

    // ... AsyncBackend implementation (submit_read/write/sync_data/sync_all,
    //     poll, wait_one, cancel, outstanding)

    // Production admission close (ADR Decision 15). New submit_* returns
    // invalid_state (Completion idle, no borrow); existing accepted requests
    // continue; cancel/poll/wait_one/reap remain legal. Idempotent.
    // Issue #67: also wakes any participant parked in the ready wait as a
    // one-shot re-evaluation signal (never fabricates readiness).
    void close_admission();

    // Phase E resource introspection (method-only; no member data exposed).
    std::size_t arena_capacity() const noexcept;
    std::size_t arena_slot_in_use() const noexcept;
    std::size_t arena_capacity_rejections() const noexcept;
    std::size_t configured_worker_count() const noexcept;
};
```

`request_capacity` and `worker_count` MUST be `> 0`. These are DISTINCT
resources (ADR Decision 13, AC-7): request capacity, blocking-I/O worker count,
Scheduler worker count, io_uring queue depth, and application pipeline depth are
all separate. No worker thread is created after construction and worker storage
never grows. Destruction is quiescent and fail-fast: it does NOT implicitly
cancel/drain/publish; the caller must `close_admission` + drain to
`outstanding() == 0` first. The real-syscall descriptor validation (ADR Decision
6; `invalid_argument` for negative fd, null buffer with nonzero length, offset
beyond `off_t`, length beyond `SSIZE_MAX`) runs before commit; a non-negative
but closed fd is accepted and later completes with the real `EBADF` terminal
(AGENTS.md §9.1 — no `fcntl(F_GETFD)` preflight).

### `sluice::async::UringAsyncBackend`

Linux io_uring backend. Build-gated behind `SLUICE_HAS_LIBURING`. Without
liburing, it is an unsupported stub: `submit_*` returns
`IoError::backend_error` synchronously; `poll()` / `wait_one()` reap nothing.
Construct directly; there is no factory function.

```cpp
class UringAsyncBackend : public AsyncBackend {
public:
    explicit UringAsyncBackend(unsigned queue_depth = 64);
    explicit UringAsyncBackend(UringConfig config);
    ~UringAsyncBackend() override;
    // ... AsyncBackend implementation (real path gated by SLUICE_HAS_LIBURING)
    bool available() const noexcept;
};
```

`UringConfig::request_capacity` must be in `[1, UINT32_MAX]`, matching the
internal `SlotIndex` domain; `queue_depth` must be nonzero. Invalid explicit
configuration is rejected with `std::invalid_argument` before backend-state
allocation. The legacy constructor preserves its `0 -> 64` mapping.

### `sluice::async::FakeAsyncBackend`

Deterministic test backend. Construct directly and configure `auto_bytes()`,
`auto_error()`, `auto_eof()`, `auto_short_then_full()`; or drive
`complete_oldest_with_bytes()` / `complete_oldest_with_error()` manually.

Phase B (ADR-explicit-io-request-contract, Accepted): `FakeAsyncBackend` now
drives the bounded `detail::RequestArena` five-stage admission and the unified
reap path with a `detail::SynchronousReadySink`. The public submit/cancel/
complete surface is unchanged (ADR Decision 7); the optional `request_capacity`
constructor argument bounds the arena (default 64). Test-only introspection
(`arena_slot_in_use()`, `arena_capacity_rejections()`, `sink_deliveries()`)
exposes the arena lifecycle for regression tests.

Phase C2e (ADR Decision 15 reference semantics): `FakeAsyncBackend` exposes the
same production admission close as `ThreadPoolBackend` — `close_admission()`
rejects new `submit_*` with `invalid_state` (Completion idle, no borrow) while
existing accepted requests continue; cancel/poll/wait_one/reap remain legal.
Fake has no split wait capability (its `wait_one` is non-blocking by contract),
so there is no parked participant to wake — the arena admission flag alone is
the full reference semantics.

```cpp
class FakeAsyncBackend : public AsyncBackend {
public:
    explicit FakeAsyncBackend(std::size_t request_capacity = 64);
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

    // Production admission close (ADR Decision 15; reference semantics).
    void close_admission() noexcept;

    // Phase B test-only introspection (the arena is a private detail).
    std::size_t arena_capacity() const noexcept;
    std::size_t arena_slot_in_use() const noexcept;
    std::size_t arena_capacity_rejections() const noexcept;
    std::size_t sink_deliveries() const noexcept;

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

---

## Application Runtime (E16)

Builder-constructed, one-shot, injected-backend application lifecycle layer.
Owns `AsyncIoContext`, `Scheduler`, root `Group`, root cancellation, and a
single dedicated driver thread. ADR: `docs/adr/ADR-application-runtime.md`.

### `sluice::async::RuntimeBuilder`

```cpp
class RuntimeBuilder {
public:
    RuntimeBuilder& backend(std::unique_ptr<AsyncBackend> b);
    RuntimeBuilder& workers(unsigned n);
    Result<std::unique_ptr<ApplicationRuntime>> build();
};
```

### `sluice::async::ApplicationRuntime`

Non-copyable, non-movable. Constructed on the heap via `RuntimeBuilder::build()`.

```cpp
class ApplicationRuntime {
public:
    Result<void> start();
    Result<void> submit(RuntimeTaskFn task);
    void request_stop() noexcept;
    Result<void> drain();
    Result<void> join();
    Result<void> shutdown();
    ~ApplicationRuntime();
};
```

| Method | Legal states | Returns on illegal state |
|--------|-------------|-------------------------|
| `start()` | Constructed | `invalid_state` (or `canceled` if stop remembered) |
| `submit()` | Running (admission open) | `invalid_state` |
| `request_stop()` | Any (idempotent, noexcept) | — |
| `drain()` | Stopping, Draining | `invalid_state` (also from a Runtime task) |
| `join()` | After drain_complete | `invalid_state` (also from a Runtime task) |
| `shutdown()` | Any (state-dispatched) | `invalid_state` from a Runtime task |

### `sluice::async::RuntimeTaskContext`

Restricted, non-owning task execution context. Valid only during one
`RuntimeTaskFn` invocation.

```cpp
class RuntimeTaskContext {
public:
    CancelToken& cancel_token() noexcept;
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    // M1-A: cooperative Completion wait.
    // Phase F1 (issue #98): now returns Result<void> — success means the
    // Completion reached a terminal result (read it via c.result());
    // canceled means the WAIT was cancelled (waiter cancellation, not I/O
    // cancellation: the I/O continues and c remains outstanding).
    Result<void> await_completion(Completion<std::size_t>& c);
    Result<void> await_completion(Completion<void>& c);

    // Phase F1 (ADR Decision 10): waiter cancellation. Removes ONLY the
    // Scheduler wait registration for `c`; the I/O, the borrow, and the
    // terminal result are untouched. Returns true when this call won the
    // waiter (the task's await will resume with canceled); false when the
    // reap already delivered (the await resumes with the real result).
    Result<bool> cancel_waiter(Completion<std::size_t>& c);
    Result<bool> cancel_waiter(Completion<void>& c);
};
using RuntimeTaskFn = std::function<void(RuntimeTaskContext&)>;
```

#### `await_completion`

Cooperatively await a submitted, outstanding `Completion`. Returns inline (no
suspend) if the `Completion` is already ready; otherwise suspends the calling
Fiber exactly once and resumes exactly once when the `Completion` reaches a
terminal result. The result stays in the `Completion` — read it via
`c.result()` after this returns, then `c.reset()` before reuse (`Completion`
L7/L9 lifecycle).

Phase F1 (issue #98): the wait is now a REAL arena waiter registration plus a
Scheduler routing record, resumed by the identity-bearing reap through the
Scheduler-owned `ReadyRoutingSink` (ADR Decisions 9/10) — the legacy
`Completion*`-keyed re-scan is gone from the production path. The return value
is `Result<void>`:

- `has_value()` — the `Completion` reached its terminal result (`c.ready()`
  is true; read via `c.result()`);
- `canceled` — the WAIT was cancelled by `cancel_waiter` (the I/O continues:
  `c` remains outstanding and later reaches a terminal result normally).

Backed by the already-audited `Scheduler::await_completion_*` primitive; the
`Scheduler*` is private and never escapes to task code.

Preconditions:
- `c` is outstanding against THIS Runtime's `AsyncIoContext` (a prior
  `submit_*` on this context marked it outstanding). Awaiting an idle
  `Completion` is a caller contract violation (Debug asserts; Release
  documents).
- called only from within a Runtime task (the `RuntimeTaskContext&` lifetime is
  the task invocation).
- at most one waiter per `Completion` (a second concurrent `await_completion`
  on the same `Completion` is a synchronous `invalid_state`, not a suspend).

#### `cancel_waiter`

Phase F1 (ADR Decision 10) waiter cancellation: removes only the Scheduler
wait registration — never the I/O operation, never the borrow, never a
terminal result. Callable from any thread while the task's await is suspended
(the Scheduler wakes the task with the `canceled` outcome; the I/O still
completes normally and `c` publishes its real result).

- returns `true` — this call won the waiter: the awaited task resumes with
  `canceled`;
- returns `false` — the reap already delivered: the task resumes with the real
  result (the call was a no-op);
- `not_found` — no registration exists (the Completion was never awaited, or
  already reset);
- `not_supported` — the backend has no waiter machinery (custom non-arena
  `AsyncBackend`; there is nothing to cancel).

Authority: submit-time errors stay synchronous (from `submit_*`); completion
errors stay terminal results in the `Completion`. Zero per-op allocation, zero
extra copies, one suspend + one resume per unresolved await, supports multiple
simultaneously outstanding `Completion`s. See
`docs/history/implementation-plans/m1-runtime-io-await-race.md`.
