# Changelog

## v0.1-mvp — blocking measurable Zig-inspired I/O core

The first tagged release. A blocking, measurable, Zig-`std.Io`-inspired C++ I/O
core. Explicitly **not** async, **not** io_uring-as-default, and makes **no
universal performance claim**.

### Added

- **Core abstractions** (001): `Reader`/`Writer`/`Result<T>`/`IoError`,
  `read_some`/`write_some`/`read_exact`/`write_all`.
- **POSIX file backend** (002): `FileReader`/`FileWriter` with EINTR retry and
  errno preservation on open failure.
- **Copy/stream limits** (003): `CopyLimit` (unlimited/bytes/nothing) and the
  flush-contract documentation.
- **Measurement hooks** (004): `SyscallStats`/`BufferStats`/`CopyStats` —
  optional, caller-owned, never global.
- **Vector I/O** (005): `IoSlice`/`ConstIoSlice`, `read_vec`/`write_vec`/
  `write_all_vec`, POSIX `readv`/`writev` overrides with `IOV_MAX` chunking,
  `VectorStats`, `wal::write_record_vec`.
- **Buffered fast path** (006): `BufferedReadable` capability interface +
  `copy_all` fast path + MVP examples.
- **Copy strategy layer** (007): `CopyStrategy`/`CopyOptions`/`CopyDecision`
  (Auto/Scratch/BufferedFirst + deferred slots).
- **Flush/sync/durability separation** (008): `SyncableWriter`
  (`sync_data`/`sync_all`), `SyncStats`, `wal::WalWriter` with the
  `written ≤ flushed ≤ durable` LSN invariant.
- **Backend boundary** (009): `IoContext` (abstract) + `BlockingIoContext`
  (POSIX); open errors surfaced at open time.
- **Microbench harness** (010): `bench/*_bench` (small_writes/copy_strategy/
  wal_write/sync_smoke) + run script + summarizer + methodology doc.
- **Optimization decision matrix** (011): runbook + summarizer +
  evidence-linked, scoped decisions (no universal claims).
- **Experimental io_uring spike** (013): `cppio::experimental::UringWriteBatch`/
  `UringIoContext`/`UringStats`, build-gated behind `--with-liburing`,
  skip-clean without liburing. **Not the default backend.**

### Documentation

- MVP closeout, Zig `std.Io` source inventory + parity audit, io_uring readiness
  gate, io_uring spike design, flush/sync/durability contract, WAL durability
  model, copy-strategy contract, buffered-fast-path note, core-microbench
  methodology, optimization runbook, optimization decision matrix,
  release checklist, liburing validation runbook, changelog.

### Tests

35 tests, all green in debug and release. Coverage spans result/error
semantics, every wrapper, vector I/O (default + POSIX), the copy strategy layer
(all strategies + deferred handling + counters), sync/durability (LSN invariant
on all paths), the IoContext boundary, and the uring stub path.

### Known limitations

```text
io_uring remains experimental unless real liburing validation supports promotion.
No production io_uring backend yet.
No async runtime.
No cancellation model.
No networking.
No timers.
No default backend switch (BlockingIoContext stays the default).
No universal performance conclusion.
liburing/kernel support required for the uring path; without it the path is a clean stub.
FileWriter::flush() does not imply durability (by design).
Zig stdlib remains design reference only, not a dependency.
```

### Non-goals for this release

- Async/evented backend.
- io_uring as the default backend.
- Networking, timers, mmap.
- Universal performance claims.

See `docs/release-v0.1-mvp-checklist.md` for the tagging checklist.
