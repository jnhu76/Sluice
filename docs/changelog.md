# Changelog

## Unreleased — Group Evented admission exception safety (P2-01)

### Changed

- **`sluice::async::Group::async_evented` admission is now transactional.** All
  three bookkeeping vectors (`evented_fibers_`, `evented_stacks_`, `futures_`) are
  reserved to `size() + 1` BEFORE the first `push_back`, inside one `mtx_`
  critical section (`include/sluice/async/group.hpp`). Previously the three
  `push_back`s could fail independently, leaving a partial Fiber/stack/Future
  record on allocation failure. Now a reserve failure propagates
  `std::bad_alloc` with no partial task record, no `Scheduler::spawn`, and the
  user task never runs. The three moved types are noexcept-movable (pinned by
  `static_assert`), so the post-reserve commit block is non-throwing. This
  closes the E16 foundation prerequisite P2-01 (Group transactional admission
  seam); it does NOT implement E16 Application Runtime. The Threaded
  (`async_threaded`) public behavior is unchanged.

### Tests

- `tests/group_evented_admission_exception_safety_test.cpp` — deterministic
  failure injection at each of the three reserve boundaries, proving no partial
  task record, no Scheduler publication, safe destruction without `await()`, and
  Group reusability. Links `sluice_async_internal_testing`; the test seam is
  absent from production.

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
  target; `.github/workflows/ci.yml` restores the documentation verification
  gate (self-test + full scan). The link checker now scans only git-tracked
  Markdown via `git ls-files`, so generated/gitignored files under `docs/`
  cannot leak into the CI scan set.

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
Public cooperative cancellation primitives exist (CancelToken, CancelState,
CancelGuard, check_cancel()), but E16 has not yet introduced
application-runtime-wide cancellation ownership and shutdown policy.
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
