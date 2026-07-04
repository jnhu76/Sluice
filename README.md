Sluice is an experimental C++ I/O control-flow library for explicit capabilities, pluggable backends, and backend-neutral Reader/Writer semantics.

## Why Sluice

Most C++ I/O ties you to a specific backend — POSIX files, sockets, memory — before you've written a line of business logic. Sluice flips that: you code against abstract `Reader`/`Writer` interfaces, and the backend is a **pluggable capability** you choose at the edges of your program.

This means:

- **Test with deterministic fault injection** (`FaultReader`/`FaultWriter`) — no filesystem, no mocking framework.
- **Benchmark with stats-collecting wrappers** (`ObservedReader`/`ObservedWriter`) — zero-copy pass-through that counts bytes and calls.
- **Swap backends without changing call sites** — POSIX files today, io_uring tomorrow, in-memory for tests, all through the same `copy_all` primitive.

The library is inspired by Zig's `std.Io` but adapted for C++20 idioms. It is **not** a port — it's a C++ take on the same explicit-capability philosophy.

## 5-minute tour

```cpp
// In-memory round-trip: no filesystem, no setup.
#include <sluice/memory_io_context.hpp>
#include <sluice/copy.hpp>
#include <cstdio>

int main() {
    sluice::MemoryIoContext ctx;

    auto r = ctx.open_reader("hello world");
    auto w = ctx.open_writer();

    sluice::copy_all(*r, *w);

    auto bytes = w->take_bytes();
    std::printf("%s\n", bytes.data());  // prints: hello world
}
```

For a real-backend version, see `examples/mvp_copy_pipeline.cpp`.

## Core concepts

### Reader / Writer

The two fundamental abstractions. Everything else — buffering, observability, fault injection, backends — is a wrapper around one of these.

| Concept | Interface | Key methods |
|---|---|---|
| Reader | `sluice::Reader` | `read_some()`, `read_exact()`, `skip_exact()` |
| Writer | `sluice::Writer` | `write_some()`, `write_all()`, `flush()` |
| Vector I/O | (on Reader/Writer) | `read_vec()`, `write_vec()`, `write_all_vec()` |

### Wrapper composition

Wrappers hold a reference to an inner reader/writer and delegate to it. The outermost wrapper is what the caller drives; each layer adds a capability:

```cpp
sluice::FileWriter      file("/tmp/out.bin");   // raw POSIX writes
sluice::ObservedWriter  observed(file, stats);  // count bytes & calls
sluice::BufferedWriter  buffered(observed, buf); // add buffering

buffered.write_all(bytes);  // flows: buffered → observed → file
buffered.flush();           // stats reflect inner calls
```

Available wrappers:

| Wrapper | What it adds |
|---|---|
| `BufferedReader` / `BufferedWriter` | Interface-level buffering |
| `ObservedReader` / `ObservedWriter` | Transparent byte/call counting |
| `FaultReader` / `FaultWriter` | Deterministic short-I/O and error injection |

### Copy primitive

```cpp
sluice::copy_all(reader, writer);                      // simple
sluice::copy_all(reader, writer, scratch);             // with scratch buffer
sluice::copy_all(reader, writer, scratch, options,     // with strategy & stats
                 &stats, &decision);
```

`copy_all` has a **buffered fast path**: when the reader is a `BufferedReader` (which implements the `BufferedReadable` capability), it drains already-buffered bytes before falling back to scratch reads. This is opt-in, observable, and not a performance claim.

### Copy strategies

The strategy layer (SLUICE-CORE-007) makes copy-path selection explicit:

| Strategy | Behaviour |
|---|---|
| `CopyStrategy::Scratch` | Always read into scratch, then write (safe fallback) |
| `CopyStrategy::BufferedFirst` | Try the buffered fast path first; fall back to scratch |
| `CopyStrategy::Auto` | Let `copy_all` decide based on runtime capability probes |

### Backend capability boundary

`sluice::IoContext` is an abstract factory that opens `Reader`/`Writer` handles:

```cpp
sluice::BlockingIoContext ctx;           // POSIX files
sluice::MemoryIoContext  ctx;            // in-memory (tests, examples)

auto reader = ctx.open_reader(path);
auto writer = ctx.open_writer(path);
sluice::copy_all(*reader, *writer);
```

Direct `FileReader`/`FileWriter` constructors remain valid for simple cases. The context abstraction exists so future backends (io_uring, network sockets) can plug in without changing call sites.

### Positional I/O

`FileReader`/`FileWriter` support positional operations (`pread`/`pwrite`/`preadv`/`pwritev`) that do **not** mutate the shared file offset — two positional ops on the same fd at different offsets are independent:

```cpp
sluice::FileWriter writer(path);
sluice::FileReader reader(path);

writer.write_at(buf1, offset1);       // pwrite — cursor untouched
writer.write_at(buf2, offset2);       // independent, no lseek needed
reader.read_at(dst, offset);          // pread
reader.read_at_exact(dst, offset);    // loops until dst is filled
writer.write_all_at(src, offset);     // loops across shorts
```

Vectored positional forms (`read_vec_at`/`write_vec_at`/`write_all_vec_at`) are also available.

### BlockingIoPool

A **bounded OS-thread execution helper** for offloading blocking work. It is *not* an async runtime — it runs callables on a fixed number of `std::thread` workers:

```cpp
#include <sluice/blocking_io_pool.hpp>

auto pool = sluice::BlockingIoPool::create(
    sluice::BlockingIoPoolOptions{.worker_count = 4, .max_queue_depth = 64});

auto task = pool->submit([] { return do_heavy_io(); });
auto result = task.get();  // blocks until complete, surfaces value or rethrows

pool->shutdown();  // stops accepting, drains submitted work, joins workers
```

Key properties: bounded queue (backpressure via `try_submit`), submission rejected after `shutdown()`, task exceptions surface via `task.get()`, instance-owned (no globals), sanitizer-clean. Formal TLA+ verification covers admission, lifecycle, and completion (`spec/tla/BlockingIoPool.tla`).

### Flush / sync / durability

Three distinct operations with three distinct contracts:

| Operation | What it guarantees |
|---|---|
| `flush()` | Drains buffered bytes to the inner writer. **No durability.** |
| `sync_data()` | `fdatasync` — data integrity (via `SyncableWriter` capability) |
| `sync_all()` | `fsync` — data + metadata integrity (via `SyncableWriter` capability) |

`BufferedWriter::flush()` drains dirty bytes. `FileWriter::flush()` is a documented no-op (no `fsync` this phase). The destructor does **not** flush — a flush that fails cannot be reported from a destructor.

Composition rule: call `flush()` on the **outermost** writer you used:

```cpp
buffered.write_all(bytes);
buffered.flush();   // drains buffered → observed → file, then file.flush()
```

### WAL record format

`sluice::wal` provides a minimal write-ahead log record format for exercising writer semantics:

```cpp
sluice::wal::write_record(writer, data);         // write a record
auto got = sluice::wal::read_record(reader);     // read it back
sluice::wal::write_record_vec(writer, iovecs);   // vector variant
```

## Building

The project uses [xmake](https://xmake.io).

```sh
xmake f -m debug                  # configure (debug mode)
xmake build sluice_core           # build the static library
xmake build -g test               # build all tests
xmake test                        # run all tests
xmake build -g examples           # build examples
```

To enable the experimental io_uring spike (requires liburing):

```sh
xmake f --with-liburing=true
xmake build -g experimental
```

### Sanitizers and memory checking

The project ships with five analysis modes. Each configures the compiler/runtime appropriately and produces binaries under a dedicated output directory.

| Mode | Flag | What it catches |
|---|---|---|
| `asan` | `-fsanitize=address` | Out-of-bounds, use-after-free, double-free |
| `tsan` | `-fsanitize=thread` | Data races, deadlocks |
| `ubsan` | `-fsanitize=undefined` | Signed overflow, null dereference, alignment issues |
| `asanubsan` | ASan + UBSan combined | Both of the above simultaneously |
| `valgrind` | debug symbols + `-O3` | Memory leaks, invalid reads/writes (runtime check) |

**Sanitizer tests** (compile-time instrumentation):

```sh
xmake f -m asan && xmake build -g test && xmake run -g test
xmake f -m tsan && xmake build -g test && xmake run -g test
xmake f -m ubsan && xmake build -g test && xmake run -g test
xmake f -m asanubsan && xmake build -g test && xmake run -g test
```

**Valgrind** (runtime wrapper — build with debug symbols, then run under valgrind):

```sh
xmake f -m valgrind && xmake build -g test
valgrind --leak-check=full --error-exitcode=1 build/linux/x86_64/valgrind/<test_name>_test
```

**Switching to Clang** (if sanitizers misbehave with g++):

```sh
xmake f --toolchain=clang -c && xmake build
xmake f --toolchain=clang -m asan -c && xmake build   # Clang + ASan
```

## Project layout

```
include/sluice/          Public headers
src/                     Implementation
tests/                   Correctness tests (one binary per slice)
examples/                Runnable examples
bench/                   Microbenchmarks (CSV output)
docs/                    Design notes, audits, decision records
scripts/                 Build/analysis helpers
```

## Tests

Tests live under `tests/` and use a tiny dependency-free harness (`tests/harness.hpp`) modeled after Zig's `std.testing.io` — deterministic, no external framework. Each slice has its own binary:

| Test | What it covers |
|---|---|
| `result_test` | `Result<T>` / `IoError` semantics |
| `writer_test` | `write_all`: short writes, zero-progress rejection, error propagation |
| `reader_test` | `read_exact`, `skip_exact`: EOF, partial reads, error propagation |
| `fault_test` | `FaultReader`/`FaultWriter` determinism and partial-write preservation |
| `buffer_test` | `BufferedReader`/`BufferedWriter` order, EOF, dirty flush, flush-error |
| `observed_test` | Stats accounting + data transparency |
| `copy_test` | `copy_all` exact bytes, totals, both-side error propagation |
| `buffered_readable_test` | `peek_buffered`/`consume_buffered` fast-path capability |
| `copy_fast_path_test` / `copy_stats_fast_path_test` | Buffered fast path + fast/scratch stats |
| `copy_strategy_test` (+ variants) | `CopyStrategy` API and routing |
| `wal_test` | WAL round-trip, truncation, checksum mismatch, fault propagation |
| `file_test` | POSIX file round-trip, EOF, missing-file, move-only |
| `writer_vec_test` / `reader_vec_test` | Default vector fallback |
| `file_vec_test` | POSIX `readv`/`writev` round-trip, `IOV_MAX` chunking |
| `wal_vec_test` | `write_record_vec` round-trip |
| `vector_stats_test` | `VectorStats` fallback counting |
| `io_context_api_test` | `IoContext` interface contract |
| `blocking_io_context_test` | `BlockingIoContext` open/error paths |
| `memory_io_context_test` | `MemoryIoContext` round-trip |
| `memory_reader_convenience_test` | `MemoryReader::from_bytes` convenience |
| `file_sync_test` | `SyncableWriter` durability contract |
| `syncable_writer_test` | `SyncableWriter` interface + capability detection |
| `wal_writer_test` | WAL writer + sync integration |
| `read_vec_all_test` | `read_vec_all` helper |
| `file_positional_test` | Positional read/write (`pread`/`pwrite`/`preadv`/`pwritev`) — cursor isolation, vector chunking |
| `blocking_io_pool_test` | `BlockingIoPool` unit/property tests — lifecycle, rejection, exception propagation |
| `blocking_io_pool_invariants_test` | `BlockingIoPool` B-class invariant enforcement |
| `blocking_io_pool_prod_test` | `BlockingIoPool` production pool concurrency |
| `blocking_io_pool_stress_test` | `BlockingIoPool` stress under contention |
| `sync_contract_negative_test` | Sync contract negative tests — G4–G6, G9, N4, N10 enforcement |
| `sync_matrix_test` | Sync matrix correctness |
| `uring_*_test` | Experimental io_uring spike (stub without liburing) |

## Examples

| Example | What it shows |
|---|---|
| `mvp_memory_io_context` | **5-minute tour** — in-memory round-trip, no filesystem |
| `mvp_copy_pipeline` | Canonical MVP composition with buffered fast path |
| `cat` | Stream a file to stdout, observed for stats |
| `copy_file` | `copy_all` between two files, explicit flush |
| `small_writes` | Many tiny writes through `BufferedWriter` + `ObservedWriter` |
| `fault_write` | Deterministic `FaultWriter` failure |
| `wal_records` | Write and read back WAL records on disk |
| `mvp_limited_copy` | `CopyLimit::bytes(N)` copy with stop-reason stats |
| `mvp_wal_vector` | WAL records via `write_record_vec` |
| `mvp_copy_strategy` | Scratch / BufferedFirst / Auto / deferred-rejected strategies |
| `mvp_wal_durable` | WAL durability: write/flush/sync with LSN tracking |
| `mvp_io_context_copy` | Open reader/writer through `BlockingIoContext`, copy, flush, sync |
| `blocking_io_pool` | Bounded pool: submit tasks, collect results, observe stats |
| `sync_random_read` | Positional random read across a file |
| `experimental_uring_write` | Experimental io_uring write path (skips cleanly without liburing) |

## Design reference

Zig `std.Io` is the reference model. The Zig source tree under `./zig` is **not** compiled or linked — it is read for design inspiration only. None of the implementation depends on Zig.

| Zig idea | Sluice equivalent |
|---|---|
| `std.Io.Reader` | `sluice::Reader` |
| `std.Io.Writer` | `sluice::Writer` |
| interface-owned buffer | `BufferedReader`/`BufferedWriter` wrappers |
| `std.testing.io` | `FaultReader`/`FaultWriter` |
| `streamTo` | `copy_all(Reader&, Writer&)` |
| `readVec`/`writeVec` | `read_vec`/`write_vec`/`write_all_vec` |
| flush / sync split | `flush()` + `SyncableWriter` |
| `Io` capability context | `sluice::IoContext` |

### Intentional divergences

These are deliberate design choices, not gaps:

1. **Buffer ownership is outside the interface** — Sluice keeps base `Reader`/`Writer` unbuffered and provides buffering through wrapper types. This means operations like Zig's `rebase`/`preserve_len` have no direct analog.

2. **`copy_all` has a buffered fast path** via the opt-in `BufferedReadable` capability, but is not full Zig zero-copy. Unbuffered readers go through `read_some(scratch) → write_all(scratch)`.

3. **`flush()` is split, not uniform** — `FileWriter::flush()` is a documented no-op; `BufferedWriter::flush()` does the real drain work. Callers must know which layer is buffered.

4. **Error model is richer** — `sluice::IoError::Code` carries eight categories (`eof`, `canceled`, `interrupted`, `would_block`, `no_space`, `permission_denied`, `invalid_state`, `backend_error`) plus a raw `os_errno`, giving callers more to branch on than Zig's `ReadFailed`/`WriteFailed`.

5. **Fault injection is original design** — `FaultReader`/`FaultWriter` + `FaultPlan` are purpose-built for deterministic testing, not a port of Zig's `std.testing.io`.

6. **Vector I/O semantics are conservative** — stop on EOF, on error, or on the first positive short result. Errors propagate immediately even after partial progress. This is a prototype, not a performance claim.

## API reference

For the full API reference, see [`docs/api-reference.md`](docs/api-reference.md).

## Status

The sync-first phase is **complete** (jobs 017S–023S). The blocking baseline is now positional, durability-defined, concurrent (`BlockingIoPool`), and benchmarked across W1–W4. The sync-first readiness gate is **GREEN** — async implementation is unblocked.

The sync runtime contract (`docs/adr/ADR-024S-sync-runtime-contract.md`) is fixed: all calls are synchronous, positional I/O does not mutate the file offset, EINTR is retried, and `BlockingIoPool` is a bounded OS-thread execution helper (not an async runtime). The contract's explicit non-goals (no `async`/`await`, no coroutine abstraction, no P2300, no cancellation of in-flight syscalls) protect the boundary between sync and future async work.

The async **design** (`docs/adr/ADR-async-io-model.md`) remains accepted. Async **implementation** proceeds against the engineered blocking baselines — not sequential-only blocking.

An experimental io_uring write spike (SLUICE-CORE-013) lives under `sluice::experimental` behind an optional `--with-liburing` build gate. It is **not** the default backend. See `docs/io-uring-spike.md`.

For the full changelog, see `docs/changelog.md`.
