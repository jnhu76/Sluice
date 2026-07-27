# Changelog

## v0.1.0 — E15 Runtime Foundation MVP

The E15 Runtime Foundation completes the async synchronization substrate, closing
E10–E12 and the E15 Runtime Foundation. The runtime foundation is now production-ready
for fiber-suspending synchronization primitives.

### Added

- **E12-E AsyncQueue\<T\>** (`sluice-CORE-E12-E`). Bounded MPMC FIFO
  channel: `push(T)`/`push_until(T, deadline_t)`/`try_push(T)`/
  `pop()`/`pop_until(deadline_t)`/`try_pop()`/`close()`/
  `begin_teardown()`. Typed result objects (`QueuePushResult<T>`,
  `QueuePopResult<T>`). Non-template `QueuePort` core + thin template
  wrapper. Queue production implementation with Corrective-2 state machine,
  Corrective-3 timer substrate supersession, and B1–B4 authorization gates.
  Formal model (Models A/B) + independent review PASS.
  See `docs/history/closeout/e12-queue.md`.

- **E12-F AsyncRwLock** (`sluice-CORE-E12-F`). Fiber-suspending async
  Read-Write Lock with writer-fair phase-batched scheduling. Design and
  proposed implementation authorized. See `docs/design/e12-rwlock.md`.

- **E10–E12 implemented async synchronization substrate closure.**
  Independent reviews for E12-B Semaphore, E12-C AsyncMutex (including
  the migration/data-race micro-review), E12-D AsyncCondition, and the
  E12-G cross-primitive semantic closure are complete. E12-B, E12-C,
  E12-D, and E12-G are closed under their recorded authorities. E12-E
  production implementation is authorized and complete. See
  `docs/e10-e12-api-semantic-closure.md` §15.

### Changed

- **`sluice::async::Mutex` acquisition is now `noexcept` / fail-fast.**
  `lock()`, `try_lock()`, and `unlock()` are declared `noexcept`. An
  underlying acquisition failure is converted to process termination via
  `std::terminate` (fail-fast entry
  `sluice::async::detail::async_mutex_lock_fail_fast`). Rationale in
  `docs/async-mutex-nothrow-authority.md`; evidence in
  `docs/history/closeout/async-mutex-nothrow-implementation.md`.

- **E15 corrective: T25 migration/reacquire hang fixed.** Condition
  variable migration-reacquire hang resolved by committed `db656b5`.

- **E12-E Queue Corrective-3: timer substrate supersession.** The E12-E
  Queue now uses the E13 `TimerRegistration` heap (independent sleep
  node heap) instead of the earlier E11 linked-list timer substrate.
  See `docs/history/closeout/e12-queue-corrective-3.md`.

### Verification

- **Sanitizer-clean:** ASan, UBSan, TSan, Valgrind all pass.
- **Formal models:** E12-E Queue Models A/B (12 invariants, 7 negatives)
  TLC PASS. Independent formal review PASS.
- **Negative-compile verification:** added for E12-E Queue invariants.
- **Deterministic causal tests:** T25 migration/reacquire 500/500 PASS.

### Known limitations

- E13 Select production implementation is not yet authorized.
- E14 Evented parity is deferred.
- io_uring remains experimental (`sluice::experimental`, build-gated).
- E16 Application Runtime is not yet proposed.

## unreleased — ongoing async substrate work

### Added

- **E10 WaitNode and WaitQueue** (`sluice-CORE-E10`). One canonical wait
  lifecycle primitive: `WaitNode` (caller-owned, non-copyable, non-movable,
  one-per-epoch) with `WaitOutcome` enum (`unresolved`, with terminal outcomes
  `woken` / `cancelled` / `expired`). `WaitQueue` is a Scheduler-integrated
  runtime substrate (intrusive FIFO, **sealed authority** — Scheduler is the
  sole registration and resolution friend; not a standalone user
  synchronization primitive). One-winner `resolve_` CAS protocol.
  See `docs/history/closeout/e10-waitnode-wait-queue.md`.

- **E11 Deadline / Timer Wait** (`sluice-CORE-E11`). `TimerRegistration`
  control block (Scheduler-integrated runtime substrate, not a standalone
  user synchronization primitive) with independently-stable retirement state
  (`ACTIVE`/`RETIRED`/`CONSUMED`). `Scheduler::deadline_t` (monotonic
  absolute deadline). `await_wait_deadline`, `expire_wait`, `monotonic_now`.
  See `docs/history/closeout/e11-deadline-timer-wait.md`.

- **E12-A Event** (`sluice-CORE-E12-A`). Persistent manual-reset async
  `Event`: `set()`/`reset()`/`wait(WaitNode&)`/`wait_until(WaitNode&,
  deadline_t)`/`cancel(WaitNode&)`. `set()` broadcasts to all registered
  waiters. Queue-identity-gated cancellation. See `docs/history/closeout/e12-event.md`.

- **E12-B Semaphore** (`sluice-CORE-E12-B`). Async counting `Semaphore`:
  `acquire(WaitNode&)`/`acquire_until(WaitNode&, deadline_t)`/
  `try_acquire()`/`release()`/`cancel(WaitNode&)`. Permit transfer or store.
  No barging. See `docs/history/closeout/e12-semaphore.md`.

- **E12-C AsyncMutex** (`sluice-CORE-E12-C`). Fiber-suspending async
  `AsyncMutex`: `lock(WaitNode&)`/`lock_until(WaitNode&, deadline_t)`/
  `try_lock()`/`unlock()`/`cancel(WaitNode&)`. Direct ownership handoff
  (owner commit BEFORE publication). See `docs/history/closeout/e12-async-mutex.md`.

- **E12-D AsyncCondition** (`sluice-CORE-E12-D`). Fiber-suspending async
  condition variable: `wait(WaitNode&)`/`wait_until(WaitNode&, deadline_t)`/
  `notify_one()`/`notify_all()`/`cancel(WaitNode&)`. Two-epoch protocol
  (Condition epoch + mandatory Mutex reacquire). Returns `WaitOutcome`
  directly. See `docs/history/closeout/e12-condition.md`.

- **E12-E AsyncQueue\<T\>** (`sluice-CORE-E12-E`). Bounded MPMC FIFO
  channel: `push(T)`/`push_until(T, deadline_t)`/`try_push(T)`/
  `pop()`/`pop_until(deadline_t)`/`try_pop()`/`close()`/
  `begin_teardown()`. Typed result objects (`QueuePushResult<T>`,
  `QueuePopResult<T>`). Non-template `QueuePort` core + thin template
  wrapper. See `docs/history/closeout/e12-queue.md`.

- **E10-E12 API & Semantic Closure** (`E10-E12-ASYNC-SYNC-API-SEMANTIC-
  CLOSURE-1`). Cross-primitive API inventory, semantic contract matrix,
  D1-D10 decision register, E13 Select dependency contract.
  See `docs/e10-e12-api-semantic-closure.md`.

- **E10–E12 implemented async synchronization substrate closure.**
  Independent reviews for E12-B Semaphore, E12-C AsyncMutex (including
  the migration/data-race micro-review), E12-D AsyncCondition, and the
  E12-G cross-primitive semantic closure are complete. E12-B, E12-C,
  E12-D, and E12-G are closed under their recorded authorities. E12-E
  remains at Phase I review PASS (WITH OBSERVATIONS). E12-F RwLock
  remains deferred. E13 Select preparation may begin, but Select design
  and production implementation are not yet authorized. See
  `docs/e10-e12-api-semantic-closure.md` §15.

### Changed

- **`sluice::async::Mutex` acquisition is now `noexcept` / fail-fast.**
  `lock()`, `try_lock()`, and `unlock()` are declared `noexcept`. An
  underlying acquisition failure (the `std::system_error` that
  `std::mutex::lock()`/`try_lock()` may throw) is no longer propagated as a
  recoverable exception; the `Mutex` boundary converts it to process
  termination via `std::terminate` (a single named fail-fast entry,
  `sluice::async::detail::async_mutex_lock_fail_fast`). Rationale and the
  full contract live in `docs/async-mutex-nothrow-authority.md`; production
  realization evidence in `docs/history/closeout/async-mutex-nothrow-implementation.md`.
  `noexcept` is part of the function type: downstream code taking
  `&sluice::async::Mutex::lock` must be recompiled. No in-repo TU does so,
  and the `Mutex` surface is inline-only, so this is transparent for
  in-tree consumers.

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
