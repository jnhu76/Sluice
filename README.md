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

## Status / scope

This phase is correctness-only. See the final report for known limitations.
