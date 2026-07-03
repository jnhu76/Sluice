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
