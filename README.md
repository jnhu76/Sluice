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
- Buffered fast path (CPPIO-CORE-006): `cppio::BufferedReadable`, an opt-in capability a `BufferedReader` implements so `copy_all` drains already-buffered bytes before falling back to the scratch path. See `docs/buffered-fast-path.md`.
- Copy strategy layer (CPPIO-CORE-007): `cppio::CopyStrategy` / `CopyOptions` / `CopyDecision` make copy path selection explicit and observable. See `docs/copy-strategy.md`.
- Flush/sync/durability separation (CPPIO-CORE-008): `cppio::SyncableWriter` (`sync_data`/`sync_all`), `SyncStats`, and `wal::WalWriter` (written/flushed/durable LSN invariant). `flush()` drains bytes and never implies durability. See `docs/flush-sync-durability.md`.
- Backend capability boundary (CPPIO-CORE-009): `cppio::IoContext` / `BlockingIoContext` open `Reader`/`Writer` handles through an abstract factory so future backends can plug in. Direct `FileReader`/`FileWriter` constructors remain valid. See `docs/io-context.md`.
- Core microbench harness + optimization matrix (CPPIO-CORE-010/011): `bench/*_bench` emit CSV; `scripts/run_core_microbenches.sh` + `scripts/summarize_core_microbench.py` run and summarize. Scoped, evidence-linked decisions live in `docs/optimization-decision-matrix.md`. No universal performance claims.
  ```cpp
  cppio::CopyOptions options;
  options.strategy = cppio::CopyStrategy::Scratch;
  cppio::CopyDecision decision;
  copy_all(reader, writer, scratch, options, &stats, &decision);
  ```
- `cppio::wal::write_record` / `read_record` — a minimal WAL record format for exercising writer semantics.
- Vector I/O (CPPIO-CORE-005): `cppio::IoSlice` / `cppio::ConstIoSlice`, `Reader::read_vec`, `Writer::write_vec` / `write_all_vec`, POSIX `readv`/`writev` overrides on the file backends, and `cppio::wal::write_record_vec`. See `docs/readv-writev-design-note.md`.

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
| `readVec`/`writeVec`        | `read_vec`/`write_vec`/`write_all_vec` (+ POSIX `readv`/`writev`) |

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
- `buffered_readable_test` — `BufferedReadable` `peek_buffered`/`consume_buffered`
- `copy_fast_path_test` / `copy_stats_fast_path_test` — `copy_all` buffered fast path + fast/scratch stats
- `copy_strategy_test` / `copy_scratch_strategy_test` / `copy_buffered_first_strategy_test` / `copy_deferred_strategy_test` / `copy_strategy_stats_test` — `CopyStrategy` API + Scratch/BufferedFirst/Auto routing + deferred handling + strategy counters
- `wal_test` — WAL round-trip, truncation, checksum mismatch, fault propagation
- `file_test` — POSIX file round-trip, EOF, missing-file, move-only, on-disk WAL
- `writer_vec_test` / `reader_vec_test` — default vector fallback: in-order, empty-skip, short I/O, error propagation
- `file_vec_test` — POSIX `readv`/`writev` round-trip, `IOV_MAX` chunking, open-error, `VectorStats`
- `wal_vec_test` — `write_record_vec` round-trips, byte-equivalence with `write_record`, overflow guard
- `vector_stats_test` — `VectorStats` fallback counting through the observed wrappers

## Examples

Under `examples/`:

- `cat` — stream a file to stdout, observed for stats
- `copy_file` — `copy_all` between two files, explicit flush
- `small_writes` — many tiny writes through `BufferedWriter` + `ObservedWriter`
- `fault_write` — a deterministic `FaultWriter` failure
- `wal_records` — write and read back WAL records on disk
- `mvp_copy_pipeline` — the canonical MVP composition; demonstrates the buffered fast path
- `mvp_limited_copy` — `CopyLimit::bytes(N)` copy with stop-reason stats
- `mvp_wal_vector` — WAL records via `write_record_vec`, read back with `read_record`
- `mvp_copy_strategy` — demonstrates Scratch / BufferedFirst / Auto / deferred-rejected / deferred-fallback with decision output
- `mvp_wal_durable` — WAL durability boundary: write/flush/sync with written/flushed/durable LSN output
- `mvp_io_context_copy` — opens reader/writer through `BlockingIoContext`, copies, flushes, syncs

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

- **`copy_all` has a buffered fast path, but is not full Zig zero-copy.**
  Zig's `Reader.stream` (Reader.zig:168) writes `r.buffer[r.seek..r.end]`
  directly to the writer because the buffer lives inside the reader. cppio keeps
  the base `Reader` unbuffered, so CPPIO-CORE-006 added an opt-in
  `BufferedReadable` capability: when `copy_all`'s reader also implements it
  (i.e. a `BufferedReader`), already-buffered unread bytes are drained to the
  writer via `peek_buffered()`/`consume_buffered()` before falling back to the
  scratch read path. Unbuffered readers still go through `read_some(scratch) →
  write_all(scratch)`. The wrapper-vs-interface-owned-buffer divergence remains
  (see `docs/buffered-fast-path.md`); this is not a performance claim —
  measurement lands in CPPIO-CORE-010.

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

- **Vector I/O (`read_vec`/`write_vec`) is partial fidelity, landed in CPPIO-CORE-005.**
  The *shape* matches Zig's `readVec`/`writeVec`. The default fallback and the
  POSIX `readv`/`writev` overrides share conservative vector-primitive semantics:
  stop on EOF, on error, or on the first positive short result (they are **not**
  `read_exact`/`write_all` over slices — `write_all_vec` is the separate all-or-
  error helper). Two intentional divergences from Zig remain: (1) the default
  fallback loops over `read_some`/`write_some` rather than negotiating around an
  interface-owned buffer (this core has no internal buffer — see above), and
  (2) errors are propagated immediately even after partial progress, where
  Zig's `readVec` swallows `EndOfStream` when bytes were already read. The POSIX
  overrides on the file backends collapse slices into one syscall. This is a
  prototype, **not** a performance claim — no benchmark integration yet. See
  `docs/readv-writev-design-note.md`.

## Status / scope

This phase is correctness-only. See the final report for known limitations.
