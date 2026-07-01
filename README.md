# io-core

A small C++ I/O core inspired by Zig `std.Io`.

This C++ core is inspired by Zig std.Io.
Zig source is used as a design reference only.
The core does not depend on Zig.
The first phase focuses on correctness, observability, and deterministic fault injection.
Optimization, benchmarking, io_uring, async backends, and policy selection are intentionally deferred.

## What's here

A composable, blocking I/O core:

- `cppio::Reader` / `cppio::Writer` — the two abstractions everything else is built on.
- `cppio::Result<T>` / `cppio::IoError` — a small expected-like error channel (C++20, no `std::expected` dependency).
- `cppio::BufferedReader` / `cppio::BufferedWriter` — interface-level buffering wrappers.
- `cppio::FileReader` / `cppio::FileWriter` — blocking POSIX file I/O, RAII, move-only.
- `cppio::ObservedReader` / `cppio::ObservedWriter` — transparent stats-collecting wrappers.
- `cppio::FaultReader` / `cppio::FaultWriter` — deterministic short-I/O and failure injection driven by a `FaultPlan`.
- `cppio::copy_all(reader, writer, scratch)` — the copy primitive.
- `cppio::wal::write_record` / `read_record` — a minimal WAL record format for exercising writer semantics.

## Design reference

Zig `std.Io` is the reference model. The Zig source tree under `./zig` is **not**
compiled or linked; it is read only for design inspiration (interface-owned
buffers, explicit capability objects, `std.testing.io`-style fault injection).
None of the implementation depends on Zig.

| Zig idea                    | C++ implementation                                             |
| --------------------------- | -------------------------------------------------------------- |
| `std.Io.Reader`             | `cppio::Reader`                                                |
| `std.Io.Writer`             | `cppio::Writer`                                                |
| explicit I/O capability     | minimal blocking file support now; `IoContext` deferred        |
| interface-owned buffer      | `BufferedReader` / `BufferedWriter` wrappers                   |
| `std.testing.io`            | `FaultReader` / `FaultWriter`                                  |
| failing I/O                 | `FaultReader` / `FaultWriter`                                  |
| `streamTo` / copy primitive | `copy_all(Reader&, Writer&)`                                   |

## Wrapper composition

Wrappers compose by reference. The outermost wrapper is what the caller drives;
each delegates to the next layer in the chain:

```cpp
FileWriter file("/tmp/out.bin");
ObservedWriter observed(file, stats);
BufferedWriter buffered(observed, buffer);

buffered.write_all(bytes);   // flows: buffered -> observed -> file
buffered.flush();            // stats reflect inner calls
```

and:

```cpp
FileReader input("/tmp/in.bin");
FileWriter output("/tmp/out.bin");
auto result = copy_all(input, output);
```

`BufferedWriter` does **not** flush in its destructor — a flush that fails cannot
be reported from a destructor, so callers must call `flush()` explicitly before
the wrapper goes out of scope.

## Flush contract

The `flush()` operation means different things at different layers:

| Layer | `flush()` does | Errors? |
|---|---|---|
| `Writer::flush()` | Generic operation exposed by the abstraction. | yes (virtual) |
| `BufferedWriter::flush()` | Drains **all dirty buffered bytes** into the inner writer, then calls `inner.flush()`. The destructor does **not** flush, because a flush that fails cannot be reported safely from a destructor. | yes |
| `FileWriter::flush()` | **Documented no-op in this phase.** It does *not* imply `fsync` / `fdatasync`, does *not* imply durability. Durability semantics are deferred. | n/a |

**Composition rule:** call `flush()` on the **outermost** writer you used. It
propagates inward:

```cpp
FileWriter file(...);
ObservedWriter observed(file, stats);
BufferedWriter buffered(observed, buffer);
buffered.write_all(...);
buffered.flush();   // correct: drains buffered -> observed -> file, then file.flush()
```

**Flushing buffered bytes is not the same as a durable commit.** Even after
`BufferedWriter::flush()` returns success, the bytes may live only in the OS page
cache; a crash can still lose them until an explicit durability barrier (not
provided this phase) runs.

## Building

The project uses [xmake](https://xmake.io). Build the static library, run the
tests, and build the examples:

```sh
xmake f -m debug            # configure
xmake build cppio_core      # build the static library
xmake build -g test         # build all correctness tests
xmake test                  # run them
xmake build -g examples     # build the examples
```

There is no pre-existing `cppio-bench` integration; benchmark logic is untouched.

## Tests

Tests live under `tests/` and use a tiny dependency-free harness
(`tests/harness.hpp`) modeled after Zig's `std.testing.io` — deterministic, no
external test framework. Each slice has its own binary:

- `result_test` — `Result<T>` / `IoError` semantics
- `writer_test` — `write_all`: short writes, zero-progress rejection, error propagation
- `reader_test` — `read_exact` / `stream_to`: EOF, partial reads, error propagation
- `fault_test` — `FaultReader` / `FaultWriter` determinism and partial-write preservation
- `buffer_test` — `BufferedReader` / `BufferedWriter` order, EOF, dirty flush, flush-error
- `observed_test` — stats accounting + data transparency
- `copy_test` — `copy_all` exact bytes, totals, both-side error propagation
- `wal_test` — WAL round-trip, truncation, checksum mismatch, fault propagation
- `file_test` — POSIX file round-trip, EOF, missing-file, move-only, on-disk WAL

## Examples

Under `examples/`:

- `cat` — stream a file to stdout, observed for stats
- `copy_file` — `copy_all` between two files, explicit flush
- `small_writes` — many tiny writes through `BufferedWriter` + `ObservedWriter`
- `fault_write` — a deterministic `FaultWriter` failure
- `wal_records` — write and read back WAL records on disk

## Intentional deviations from the Zig model

These divergences from `std.Io` are deliberate and approved by the task scope
(correctness / observability / composability phase; optimization, io_uring,
and async are deferred). Documented here so they read as design, not oversight.

- **Buffer ownership is *outside* the interface (wrapper model), not inside it.**
  In Zig, the buffer lives *inside* each `Reader`/`Writer` (`r.buffer/seek/end`,
  `w.buffer/end`); every reader is inherently buffered and VTable operations
  (`drain`, `stream`, `rebase`) negotiate around it. This core deliberately keeps
  `Reader`/`Writer` **unbuffered** and puts buffering in **wrapper types**
  (`BufferedReader`/`BufferedWriter`), following the task's design table.
  Consequence: some Zig operations have no direct analog here — `rebase`,
  `preserve_len`, the `Discarding`/`Allocating` writers. Those are out of scope
  for this phase.

- **`copy_all` uses an intermediate scratch buffer, not a zero-copy fast path.**
  Zig's `Reader.stream` (Reader.zig:168) routes the first bytes through the
  reader's *own* buffer so the writer reads directly from buffered data — no
  extra copy, and no scratch allocation when data is already buffered.
  `cppio::copy_all(reader, writer, scratch)` always copies through the
  caller-provided scratch, and wrapping it around a `BufferedReader` buys
  nothing today. This is the largest fidelity gap and the prime candidate for
  the zero-copy/io_uring phase.

- **`flush()` is split, not uniform.**
  In Zig one `flush` contract does the right thing regardless of whether the
  writer is buffered (it drains until the buffer is empty, Writer.zig:312).
  Here `FileWriter::flush()` is a documented no-op for user-space state (no
  `fsync` this phase), and `BufferedWriter::flush()` does the real work.
  Callers must therefore know which layer in a chain is buffered to flush
  meaningfully; the unified Zig model avoids that. Deferred.

- **Error model is flattened.**
  Zig keeps it minimal: `Reader.Error = {ReadFailed, EndOfStream}`,
  `Writer.Error = {WriteFailed}`; backend detail lives in the implementation.
  `cppio::IoError::Code` carries eight categories (`eof`, `canceled`,
  `interrupted`, `would_block`, `no_space`, `permission_denied`,
  `invalid_state`, `backend_error`) plus a raw `os_errno`. The `backend_error`
  code plays the role of Zig's `ReadFailed`/`WriteFailed`; the extra categories
  let callers branch without digging into a backend. Considered an improvement
  for C++ usage, not a regression.

- **`std.testing.io` has no direct analog; the fault wrappers are original.**
  Zig's current `std.testing.io` is built on an `Io.Threaded` context
  (testing.zig:36), not a simple fault-injecting recorder. `FaultReader`/
  `FaultWriter` + `FaultPlan` are this project's design to satisfy the task's
  "deterministic fault injection" requirement. They are specified by the task,
  so they are authoritative rather than a Zig-port fidelity issue.

## Status / scope

This phase is correctness-only. See the final report for known limitations.
