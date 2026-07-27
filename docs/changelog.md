# Changelog

## Unreleased — documentation corrective (post-v0.1.0)

Documentation and build metadata changes since the v0.1.0 tag. No production
library behavior or unit-test behavior changed.

### Changed

- **API reference accuracy** — `docs/api-reference.md` Async Runtime section
  rewritten to match installed public headers exactly. Test-only seams
  (`spawn_on`, `advance_clock`, `shutting_down_for_test`) marked as NOT user API.
- **Async quickstart** — new `examples/async_foundation_quickstart.cpp` (public
  headers only). README examples match.
- **Async runtime architecture** — `docs/architecture/async-runtime.md`
  separates Scheduler-integrated primitives from policy-based task waiting.
- **Link checker integrity** — `scripts/check-doc-links.py` now resolves
  markdown links strictly doc-relative (matches GitHub rendering). Self-test
  mode added. 69+ stale doc links repaired.
- **Changelog** — corrected version identity and E13/E14/E15 status.
- **Build metadata** — `xmake/examples.lua` adds `async_foundation_quickstart`
  target; `.github/workflows/ci.yml` adds acceptance and documentation gates.

## v0.1.0 — Runtime Foundation (E10–E15)

The first tagged release. Synchronous core (`sluice_core`) and asynchronous
runtime (`sluice_async`) both production-ready.

### Added

**Synchronous core (`sluice_core`):**

- `Result<T>`/`IoError` error model, `Reader`/`Writer` semantics, `copy_all`
  with `CopyStrategy` (Scratch/BufferedFirst/Auto), `FileReader`/`FileWriter`
  (POSIX, positional I/O, vector I/O), `BlockingIoContext`/`MemoryIoContext`,
  `SyncableWriter` (`sync_data`/`sync_all`), WAL record format, `BlockingIoPool`.

**Asynchronous runtime (`sluice_async`):**

- E10 `WaitNode`/`WaitQueue`, E11 `TimerRegistration`/deadline.
- E12-A `Event`, E12-B `Semaphore`, E12-C `AsyncMutex`, E12-D `AsyncCondition`,
  E12-E `AsyncQueue<T>`, E12-F `AsyncRwLock`.
- E13 `Select` (multi-arm Event/Timer select).
- E14 Threaded/Evented parity (`ThreadedWaitPolicy`/`EventedWaitPolicy`).
- `Scheduler`, `Fiber`, `Completion<T>`, `AsyncIoContext`, `Future<T>`,
  `Group`, `Batch`, `ThreadPoolBackend`.

**Experimental:**

- `UringAsyncBackend` — Linux io_uring (build-gated behind `--with-liburing`,
  stub without liburing). **Not the default backend.**

### Tests

109 tests, all green in debug and release. Coverage spans the synchronous
core, all async synchronization primitives, multi-worker scheduling, and both
execution strategies.

### Known limitations

```text
io_uring remains experimental unless real liburing validation supports promotion.
No production io_uring backend yet.
No cancellation model (public API); internal cancellation only.
No networking.
No default backend switch (BlockingIoContext stays the default).
No universal performance conclusion.
liburing/kernel support required for the uring path; without it the path is a clean stub.
FileWriter::flush() does not imply durability (by design).
Zig stdlib remains design reference only, not a dependency.
```

### Non-goals for this release

- io_uring as the default backend.
- Networking, mmap.
- Universal performance claims.
