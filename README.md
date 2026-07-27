# Sluice

An experimental C++20 I/O control-flow library built around explicit
capabilities, pluggable backends, and backend-neutral `Reader` / `Writer`
semantics.

**Current status:** v0.1.0 — Runtime Foundation (E10–E15) complete.
Synchronous core, async runtime, and synchronization primitives are
production-ready. E16 Application Runtime is the next proposed phase.

## Why Sluice

Most C++ I/O ties you to a specific backend — POSIX files, sockets, memory —
before you've written a line of business logic. Sluice flips that: you code
against abstract `Reader`/`Writer` interfaces, and the backend is a **pluggable
capability** you choose at the edges of your program.

This means:

- **Test with deterministic fault injection** (`FaultReader`/`FaultWriter`) — no filesystem, no mocking framework.
- **Benchmark with stats-collecting wrappers** (`ObservedReader`/`ObservedWriter`) — zero-copy pass-through that counts bytes and calls.
- **Swap backends without changing call sites** — POSIX files today, io_uring tomorrow, in-memory for tests, all through the same `copy_all` primitive.

The library is inspired by Zig's `std.Io` but adapted for C++20 idioms. It is
**not** a port — it's a C++ take on the same explicit-capability philosophy.

## Build boundaries

| Library | Description | Default |
|---------|-------------|---------|
| `sluice_core` | Synchronous core: Result, Reader/Writer, copy, file I/O, WAL, BlockingIoPool | Always builds |
| `sluice_async` | Async runtime: Scheduler, Fiber, synchronization primitives, Completion, Future/Group/Batch | Opt-in; built explicitly or as a dependency of async tests/examples |
| `sluice_async_internal_testing` | Test-only variant with deterministic causal seams | Test-only |
| `sluice_experimental_uring` | Optional io_uring code (stub without liburing) | Off by default |

## 5-minute synchronous example

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

## 5-minute asynchronous example

```cpp
// Async runtime: submit an op, poll for completion, read the result.
// (examples/async_foundation_quickstart.cpp — builds against public headers)
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <cstddef>
#include <cstdio>
#include <memory>

int main() {
    // FakeAsyncBackend is a deterministic test backend: auto_bytes(n) makes
    // the next poll() complete each outstanding op with n bytes.
    auto backend = std::make_unique<sluice::async::FakeAsyncBackend>();
    sluice::async::FakeAsyncBackend* raw = backend.get();
    raw->auto_bytes(8);

    sluice::async::AsyncIoContext ctx(std::move(backend));

    // submit_read against a caller-owned Completion.
    sluice::async::Completion<std::size_t> c;
    std::byte buf[8]{};
    if (!ctx.submit_read(sluice::async::ReadOp{0, buf, 8, 0}, c).has_value())
        return 1;

    // poll() reaps completions non-blockingly; returns count reaped.
    if (ctx.poll() != 1) return 2;

    // The op result is read from the Completion after it is ready — NOT from
    // wait_one()/poll() return values.
    if (!c.ready()) return 3;
    auto r = c.result();
    if (!r.has_value() || r.value() != 8) return 4;

    std::printf("async quickstart: read %zu bytes\n", r.value());
    return 0;
}
```

## Implemented capabilities

### Synchronous core (`sluice_core`)

- `Result<T>` / `IoError` error model
- `Reader` / `Writer` + `BufferedReader` / `BufferedWriter` / `ObservedReader` / `ObservedWriter` / `FaultReader` / `FaultWriter`
- `copy_all` with `CopyStrategy` (Scratch / BufferedFirst / Auto)
- `FileReader` / `FileWriter` (POSIX, positional I/O, vector I/O)
- `BlockingIoContext` / `MemoryIoContext` factory abstraction
- `BlockingIoPool` (bounded OS-thread execution helper)
- `SyncableWriter` (`sync_data` / `sync_all`)
- WAL record format

### Async runtime (`sluice_async`)

- `Scheduler` (M:N fiber scheduler, multi-worker, work stealing)
- `Fiber` (context-switch, x86_64 Linux)
- `WaitNode` / `WaitQueue` (E10), `TimerRegistration` / deadline (E11)
- `Event` (E12-A), `Semaphore` (E12-B), `AsyncMutex` (E12-C)
- `AsyncCondition` (E12-D), `AsyncQueue<T>` (E12-E), `AsyncRwLock` (E12-F)
- `Select` (E13) — multi-arm Event/Timer select
- `CancelToken` / `CancelState` / `CancelGuard` (cancellation primitives)
- `Future<T>` (E28), `Group` (E29), `Batch` (E30)
- `Completion<T>` / `AsyncIoContext` / `AsyncBackend`
- `FakeAsyncBackend` (deterministic test vehicle)
- `ThreadPoolBackend` (portable, std::thread)

### Experimental

- `UringAsyncBackend` — Linux io_uring (build-gated behind `--with-liburing`; stub without liburing). Remains experimental; real-liburing and non-Linux evidence are limited.

## Build and test

```bash
xmake f -m debug                  # configure (debug mode)
xmake build sluice_core           # build synchronous core
xmake build sluice_async          # build async runtime
xmake build -g test               # build all tests
xmake test                        # run all tests
```

Enable experimental io_uring (requires liburing):

```bash
xmake f --with-liburing=true
xmake build -g experimental
```

### Sanitizers

```bash
xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test
xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test
```

## Verification model

- **Acceptance** — `public_api_acceptance` (public-only compile+run probe); `async_foundation_quickstart` (async foundation consumer); future E16 runtime acceptance consumer
- **Unit / component** — `xmake test -v` (per-slice test binaries)
- **Deterministic causal tests** — `SLUICE_ASYNC_INTERNAL_TESTING` phase seams
- **Sanitizer gates** — ASan, UBSan, TSan
- **Formal models** — TLA+ specs under `docs/spec/` and `spec/tla/`

For the full verification matrix, see [`docs/verification/README.md`](docs/verification/README.md).

## Project layout

```
include/sluice/          Public headers (core + async)
src/                     Implementation (core + async)
tests/                   Correctness tests (one binary per slice)
examples/                Runnable examples
bench/                   Microbenchmarks (CSV output)
docs/                    Architecture, design, history, roadmap, verification
  architecture/          Current architecture documents
  design/                Active proposed designs
  adr/                   Accepted Architecture Decisions
  history/               Closeout records, implementation plans, formal designs, reviews
  verification/          Verification matrix and scripts
  roadmap/               Active future work
scripts/                 Build/analysis helpers
xmake/                   Build configuration
```

## Known limitations

- `Evented` execution strategy requires x86_64 Linux with `fiber_ctx::supported`.
- `io_uring` requires Linux + liburing (build-gated, off by default).
- Real-liburing validation and non-Linux portability evidence remain limited.
- `AsyncQueue<T>` v1 has no public wait-epoch cancellation API.
- Vector I/O semantics are conservative (stop on EOF, error, or first positive short result).

## Documentation links

- [Architecture overview](docs/architecture/overview.md)
- [Async runtime](docs/architecture/async-runtime.md)
- [Async synchronization](docs/architecture/async-synchronization.md)
- [Async I/O foundation](docs/architecture/async-io-foundation.md)
- [Public API reference](docs/api-reference.md)
- [Chinese API reference](docs/api-reference-zh.md)
- [Verification matrix](docs/verification/README.md)
- [Roadmap](docs/roadmap/README.md)
- [Changelog](docs/changelog.md)

## License

See `LICENSE` for details.
