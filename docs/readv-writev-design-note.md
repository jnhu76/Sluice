# readv/writev design note

**Status: design only.** No implementation in CPPIO-CORE-004. Scheduled for
CPPIO-CORE-005.

## Why

After CPPIO-CORE-004 added measurement hooks, the next primitive before zero-copy
or io_uring is **vector I/O** (`readv`/`writev`). It narrows the gap with Zig's
internal vector read/write paths and prepares concrete optimizations:

- WAL record framing (`header | payload | checksum`) is naturally three iovs —
  a single `writev` syscall instead of three writes or a manual gather.
- A `write_all_vec` over a record's slices avoids copying them into one
  contiguous buffer.

## Proposed API (CPPIO-CORE-005)

```cpp
// include/cppio/iovec.hpp
namespace cppio {
struct IoSlice { std::span<std::byte> bytes; };        // mutable, for read
struct ConstIoSlice { std::span<const std::byte> bytes; };  // for write
}

// Reader
virtual Result<std::size_t> read_vec(std::span<IoSlice> dsts);

// Writer
virtual Result<std::size_t> write_vec(std::span<const ConstIoSlice> srcs);
Result<void> write_all_vec(std::span<const ConstIoSlice> srcs);
```

## Default fallback

The base `Reader::read_vec` / `Writer::write_vec` provide a **default fallback**
that loops over the slices calling `read_some`/`write_some`. This means every
existing reader/writer works with vector I/O out of the box, *without* a POSIX
backend — important for the in-memory test doubles and fault wrappers. The
fallback preserves order, stops on EOF/error, and returns total bytes.

## POSIX override

`FileReader`/`FileWriter` override `read_vec`/`write_vec` to call `::readv`/
`::writev` directly, collapsing N slices into one syscall. Only the POSIX file
classes do this — in-memory and fault wrappers keep the fallback.

## VectorStats (CPPIO-CORE-005, optional)

```cpp
struct VectorStats {
    std::uint64_t read_vec_calls = 0;
    std::uint64_t read_vec_bytes = 0;
    std::uint64_t read_vec_iovecs = 0;
    std::uint64_t read_vec_fallback_calls = 0;
    std::uint64_t write_vec_calls = 0;
    std::uint64_t write_vec_bytes = 0;
    std::uint64_t write_vec_iovecs = 0;
    std::uint64_t write_vec_fallback_calls = 0;
};
```

`*_fallback_calls` distinguishes "used the default loop" from "used the real
POSIX readv/writev" — important for the CPPIO-CORE-011 decision matrix.

## WAL use

A new `wal::write_record_vec` writes `header | payload | checksum` via
`write_all_vec`. The old `wal::write_record` stays intact (no removal). Both
must round-trip with the existing `wal::read_record`.

## Non-goals for CPPIO-CORE-005

- No io_uring.
- No async.
- No benchmark integration (that's CPPIO-CORE-010).
- No performance claim.
- No removal of the old scalar `read_some`/`write_some` APIs.

## Future relation

- **io_uring**: vector I/O maps naturally to fixed-band submission queue
  entries; this API is the shape io_uring would consume later (CPPIO-CORE-012).
- **WAL batching**: once `write_all_vec` exists, multi-record WAL batching
  becomes a single vector write — deferred until measured as worthwhile.
