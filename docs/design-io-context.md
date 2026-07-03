# IoContext — backend capability boundary

**Status: documented in SLUICE-CORE-009A; implemented in 009B–009F.**

```text
IoContext is not async.
IoContext is not io_uring.
IoContext is the backend capability boundary.
```

## 1. Scope

SLUICE-CORE-009 introduces an explicit I/O capability boundary inspired by Zig
`std.Io`'s context-owned backend model. `IoContext` is an abstract factory for
`Reader`/`Writer` handles, so future backends (async, io_uring, test fakes) can
plug in without callers changing how they obtain handles. This task does **not**
implement async, a thread pool, or io_uring. The current direct
`FileReader`/`FileWriter` constructors remain valid; `IoContext` is an additional
construction boundary.

## 2. Why an explicit I/O capability boundary exists

Today every caller constructs `FileReader`/`FileWriter` directly, coupling them
to the blocking POSIX backend. Before any async or io_uring work, the project
needs a single place where "which backend am I talking to" lives. `IoContext`
provides that: `open_reader`/`open_writer` return backend-owned handles behind
the abstract `Reader`/`Writer` interface, so the *use* of a handle is backend-
agnostic while the *choice* of backend is centralized.

## 3. Relationship to Zig `std.Io`

Zig `std.Io` carries the backend in the `Io` context object (e.g.
`Io.Threaded`), which owns the submission/completion machinery; `Reader`/
`Writer` are created from that context and the vtable negotiates buffer/stream
behavior against it. sluice cannot adopt that model wholesale (no async runtime
yet), but `IoContext` mirrors the *seam*: the context is where backend
capabilities are declared, and handles are obtained from it. Zig remains a design
reference, not a dependency.

## 4. Current backend: blocking POSIX

`BlockingIoContext` (009C) is the first concrete context. Its `open_reader`/
`open_writer` construct `FileReader`/`FileWriter`, propagate open errors at open
time (rather than deferring to first I/O like the direct constructors do), and
wire the optional stats pointers. No async, no thread pool.

## 5. Test/faking context possibility

Because `IoContext` is abstract, tests can supply a deterministic fake (e.g. a
`MemoryIoContext` with named in-memory files). 009D defers a full
`MemoryIoContext` — the existing in-memory readers/writers already cover test
needs, and a fake filesystem risks overbuilding. The seam exists for it.

## 6. What is not implemented

- No async/evented backend.
- No thread pool.
- No io_uring.
- No scheduler / cancellation token.
- No `MemoryIoContext` this stage — **deferred by SLUICE-CORE-009D**.

### 009D decision: defer MemoryIoContext

A `MemoryIoContext` (deterministic named in-memory files) is the natural test
fake, but it risks overbuilding a filesystem. The existing in-memory
`MemoryReader`/`MemoryWriter` plus the `FaultReader`/`FaultWriter` wrappers
already cover the deterministic test needs this project has, and
`BlockingIoContext` itself uses only temp files in tests. The abstract
`IoContext` seam remains, so a future `MemoryIoContext` can be added without
changing callers when a concrete need appears. Building it now would be
speculative.

## 7. Future backend candidates

- `MemoryIoContext` — deterministic named in-memory files (testing).
- An async context (post-012 research).
- An io_uring context (post-012, requires async preconditions).
- A fault-injecting context (wraps another context with `FaultReader`/
  `FaultWriter` semantics) for backend-level chaos testing.

Direct `FileReader`/`FileWriter` construction stays supported for code that does
not need backend indirection.
