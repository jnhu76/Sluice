# Public API surface audit

**Status: SLUICE-CORE-015D.** Audit of the public header surface as of the
v0.1-mvp tag + 015A–015C additions. Goal: before future async work begins,
record what is stable, what is experimental, what is internal, and what is
intentionally left unchanged — so the surface does not grow by accident.

This audit is descriptive. It does **not** remove or move any API; the headers
were already well-structured (`detail/` for impl details, `experimental/` for
the uring spike). Nothing in the public headers was found to be clearly internal
*and* unused, so no relocation was warranted.

## Public, stable-ish APIs

These are the intended user-facing surface. "Stable-ish" means: removing or
silently re-semanticizing these would break consumers; treat as frozen across
minor work, change only with a deliberate deprecation.

### Core abstractions (`reader.hpp`, `writer.hpp`, `result.hpp`, `error.hpp`, `iovec.hpp`)
- `Reader` (abstract): `read_some`, `read_exact`, `read_vec`, `read_vec_all`
  (015A), `stream_to` overloads.
- `Writer` (abstract): `write_some`, `flush`, `write_all`, `write_vec`,
  `write_all_vec`.
- `Result<T>`, `IoError` (+ `Code` enum), `make_unexpected`, `from_errno_value`,
  `to_string(IoError)`.
- `IoSlice`, `ConstIoSlice`.

### Wrappers (`buffer.hpp`, `observed.hpp`, `fault.hpp`, `buffered_readable.hpp`, `sync.hpp`)
- `BufferedReader` / `BufferedWriter` (and their `BufferStats`).
- `BufferedReadable` capability interface (`peek_buffered` / `consume_buffered`).
- `ObservedReader` / `ObservedWriter` (+ `ReaderStats` / `WriterStats`).
- `MemoryReader` / `MemoryWriter` (+ `from_string`, `from_bytes` (015B), `bytes`).
- `FaultReader` / `FaultWriter` (+ `FaultPlan`).
- `SyncableWriter` capability interface (`sync_data` / `sync_all`).

### Backends + copy (`file.hpp`, `io_context.hpp`, `copy.hpp`, `copy_strategy.hpp`, `limit.hpp`, `measurement.hpp`, `wal.hpp`)
- `FileReader` / `FileWriter` (POSIX, RAII, move-only; `open_error()` accessor).
- `IoContext` (abstract) + `BlockingIoContext`.
- `MemoryIoContext` (015C) — deterministic test/demo context.
- `copy_all` overloads (incl. strategy-aware), `CopyStrategy`, `CopyOptions`,
  `CopyDecision`, `CopyLimit`, `to_string(CopyStrategy)`,
  `to_string(UnsupportedStrategyPolicy)`.
- Stats structs: `SyscallStats`, `BufferStats`, `CopyStats`, `VectorStats`,
  `SyncStats`, `UringStats`.
- `wal::write_record` / `wal::read_record` / `wal::write_record_vec` /
  `wal::WalWriter` (+ LSN accessors).

## Experimental APIs

Build-gated and explicitly not the default backend. Use at your own risk; the
surface may change without deprecation.

- `sluice::experimental::UringWriteBatch` + `UringWriteResult` (013).
- `sluice::experimental::UringIoContext` (013) — standalone, NOT a subclass of
  `sluice::IoContext`.
- `UringStats` lives in `sluice` (not experimental) only so bench/CSV helpers can
  treat it uniformly; it is still associated with the experimental uring path.

## Internal details (do not depend on these)

- **`sluice::detail::retry_on_eintr`** and everything under `include/sluice/detail/`
  (`posix_retry.hpp`). Impl-only; may change or move without notice.
- **`wal::detail::checked_u32_len`** and other `detail::` symbols inside `wal`.
- The `void* ring_` member and the liburing-specific branch in
  `experimental/uring_write_batch.cpp` — implementation detail of the spike.
- `bench/bench_common.*` — bench-only helper, not a library API.

## APIs intentionally left unchanged (this task)

- `read_vec` — the conservative stop-on-short primitive (005B). Unchanged;
  `read_vec_all` (015A) is the new sibling, `read_vec` keeps its semantics.
- `BlockingIoContext` — the default backend. Untouched by 015C
  (`MemoryIoContext` is a sibling, not a replacement).
- `flush()` ≠ durability — FileWriter::flush remains a non-durable no-op;
  durability stays split into `SyncableWriter`. (See parity-audit `flush` row.)
- Default backend selection — blocking POSIX stays the default; io_uring stays
  experimental.
- The error model — no new `IoError::Code` was added in 015 (MemoryIoContext
  reuses the existing `permission_denied` for "path not found", matching
  BlockingIoContext's ENOENT mapping).

## What this audit did NOT do

- No public API was removed or renamed.
- No header was relocated (the `detail/` and `experimental/` split is already
  correct).
- No experimental API was promoted to stable.
- No stable API was demoted to experimental.
