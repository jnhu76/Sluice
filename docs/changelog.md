# Changelog

## v0.2.0 — Runtime Foundation (E10–E15)

The Runtime Foundation completes the async synchronization substrate and
runtime. E10 WaitNode/WaitQueue, E11 deadline/timer, E12-A through E12-F
primitives, E13 Select, and E14 Threaded/Evented parity are all implemented
and merged to production. The runtime is now production-ready for
fiber-suspending synchronization primitives, multi-arm select, and both
Threaded and Evented execution strategies.

### Added

- **E10 WaitNode / WaitQueue** (`sluice-CORE-E10`). Canonical wait lifecycle
  primitive with one-winner `resolve_` CAS protocol.
  See `docs/history/closeout/e10-waitnode-wait-queue.md`.

- **E11 Deadline / Timer Wait** (`sluice-CORE-E11`). `TimerRegistration`
  control block with monotonic `deadline_t`, `await_wait_deadline`,
  `expire_wait`. See `docs/history/closeout/e11-deadline-timer-wait.md`.

- **E12-A Event** through **E12-F AsyncRwLock** (`sluice-CORE-E12-A..F`).
  Event, Semaphore, AsyncMutex, AsyncCondition, AsyncQueue\<T\>, AsyncRwLock.
  All closed under recorded authorities with independent review PASS.
  See `docs/history/closeout/e10-e12-api-semantic-closure.md`.

- **E13 Select** (`sluice-CORE-E13`). Multi-arm Event/Timer select with
  registration-rollback exception topology and destruction-contract closure.
  Formal model (TLA+) PASS. See `docs/history/closeout/e13-select-p7-rollback-closeout.md`.

- **E14 Threaded / Evented Parity** (`sluice-CORE-E14`). Both execution
  strategies implemented and merged (PR #29). `ThreadedWaitPolicy` and
  `EventedWaitPolicy` provide the physical-wait seam for `Future<T>` and
  `Group`. See `docs/history/implementation-plans/e14-threaded-evented-parity-preparation.md`.

- **E15 Runtime Foundation closure.** Substrate complete and production-ready.

### Changed

- **`sluice::async::Mutex` acquisition is now `noexcept` / fail-fast.**
  `lock()`, `try_lock()`, and `unlock()` are declared `noexcept`. An
  underlying acquisition failure is converted to process termination via
  `std::terminate` (fail-fast entry
  `sluice::async::detail::async_mutex_lock_fail_fast`). Rationale in
  `docs/history/implementation-plans/async-mutex-nothrow-authority.md`; evidence in
  `docs/history/closeout/async-mutex-nothrow-implementation.md`.

### Verification

- **Sanitizer-clean:** ASan, UBSan, TSan, Valgrind all pass.
- **Formal models:** E12-E Queue Models A/B, E13 Select TLA+ — all TLC PASS.
- **Negative-compile verification:** for E12-E Queue and E13 Select invariants.
- **Deterministic causal tests:** all phase-seam tests PASS.

### Known limitations

- io_uring remains experimental (`sluice::experimental`, build-gated).
- E16 Application Runtime is the next proposed phase.

## v0.1.0 — blocking measurable Zig-inspired I/O core

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
- **Microbench harness** (010): `bench/*_bench.cpp` (small_writes/copy_strategy/
  wal_write/sync_smoke) + run script + summarizer + methodology doc.
- **Optimization decision matrix** (011): runbook + summarizer +
  evidence-linked, scoped decisions (no universal claims).
- **Experimental io_uring spike** (013): `sluice::experimental::UringWriteBatch`/
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

See `docs/history/archive/release-v0.1-mvp-checklist.md` for the tagging checklist.
