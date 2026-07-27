# Async I/O Foundation Architecture

**Status:** Current
**Authority:** Architecture
**Scope:** `sluice_async` — Completion, AsyncBackend, AsyncIoContext, Batch, Future, Group, CancellationToken.

The async I/O foundation is the L1 op-execution layer and the task abstractions
built on top of it. It decouples op submission and completion from the
synchronous `Reader` / `Writer` contract.

## Completion\<T\>

`Completion<T>` is the state for one outstanding operation. It is
**caller-owned** so allocation is decoupled from submit (mirrors Zig
`std.Io Completion`). The runtime never allocates a `Completion`.

**Lifecycle (ADR §5 L7–L11):**

- **L7** — address-stable while outstanding; MUST NOT be moved, destroyed, or
  reused until ready.
- **L8** — submitting into a not-ready `Completion` returns
  `IoError::invalid_state` synchronously.
- **L9** — `result()` before ready is a contract violation: debug assertion;
  release returns `IoError::invalid_state`.
- **L11** — destroying an `AsyncIoContext` with outstanding Completions is a
  contract violation (fail-fast in both Debug and Release).

State machine: `idle → outstanding → ready → (reset) → idle`.

Every successful `complete_with()` stamps the `Completion` with a monotonic
reap sequence number (`next_reap_seq()`). `Batch::next()` consumes it to surface
completions in true reap order.

## AsyncBackend

`AsyncBackend` is the internal backend boundary (ADR §4). Not public-facing;
`AsyncIoContext` delegates to it. Concrete backends implement:

| Method | Description |
|--------|-------------|
| `submit_read(ReadOp, Completion<size_t>&)` | Submit a positional read. |
| `submit_write(WriteOp, Completion<size_t>&)` | Submit a positional write. |
| `submit_sync_data(SyncDataOp, Completion<void>&)` | Submit `fdatasync`. |
| `submit_sync_all(SyncAllOp, Completion<void>&)` | Submit `fsync`. |
| `poll()` | Non-blocking reap; returns count made ready. |
| `wait_one()` | Blocking reap; waits until >=1 ready. |

### Backends

| Backend | Build gate | Description |
|---------|-----------|-------------|
| `FakeAsyncBackend` | Always | Deterministic test vehicle; held-pending mode, error/short injection. |
| `ThreadPoolBackend` | Always | Portable real backend; one `std::thread` per outstanding op. |
| `UringAsyncBackend` | `--with-liburing` | Linux io_uring; high-concurrency, one syscall batches many ops. Stub without liburing. |

`AsyncIoContext` owns its backend (`unique_ptr`). State is instance-owned; no
globals.

## AsyncIoContext

`AsyncIoContext` is the public L1 API surface. It holds an `AsyncBackend`,
exposes `submit_*` / `cancel` / `poll` / `wait_one`, and manages the
caller-owned `AsyncStats` sink.

**Op descriptors** — all read/write ops are positional (carry an explicit
offset). Sync ops carry no buffer/offset.

**Move semantics (E15-P1-03 / E15-P2-06):**

- Safe move paths: idle-to-idle, source-with-outstanding transfer, self move,
  chained moves, moved-from destruction.
- Fail-fast paths: destination-outstanding move-assign, destroy-with-outstanding
  (both Debug and Release).

## Batch

`Batch` is the lowest-level awaitable group (sluice-CORE-030, T4): N operations
submitted together, awaited as a whole, iterated in completion order via
`next()`. It is a driver over `AsyncIoContext` — it submits N ops via the
existing per-op `submit_*`, drives `poll`/`wait_one`, and surfaces completions
in reap order.

`BatchOp` is a discriminated variant of `ReadOp` / `WriteOp` / `SyncDataOp` /
`SyncAllOp`. `BatchResult` carries the per-op result.

## Future\<T\>

`Future<T>` is a single-task awaitable (sluice-CORE-028, T2). Producer side:
`complete_with()`. Consumer side: `await()` / `cancel()`.

- Idempotent: once the result is materialized, both `await()` and `cancel()` are
  no-ops returning the cached result.
- Composes `Completion<T>` and `CancelToken`.
- Two wait policies: `ThreadedWaitPolicy` (blocks calling thread) and
  `EventedWaitPolicy` (suspends Fiber on a Scheduler).

## Group

`Group` is an unordered task set (sluice-CORE-029, T3). Tasks are added via
`async(fn)`; the group is awaited as a whole (`await` waits for ALL tasks) or
canceled as a whole (`cancel` requests cancel on every task then awaits).

- **Threaded mode** — each `async(fn)` spawns a `std::thread`; `await` blocks
  the calling thread on each task Future.
- **Evented mode** — each `async(fn)` spawns a Fiber on the Scheduler; the task
  Future uses `EventedWaitPolicy` so a `Future::await` inside `fn` suspends the
  Fiber.

Group is a cancel-propagation boundary: tasks swallow `IoError::canceled`.

## CancellationToken / CancelState

Cooperative cancellation primitives (sluice-CORE-027, T1):

- `CancelToken` — the cancel-request state, shareable between cancellers and
  consumers. Thread-safe; `request()` is idempotent.
- `CancelState` — per-consumer protection/acknowledge state.
- `CancelProtection` — `unblocked` / `blocked`; blocks DELIVERY (not the
  request).
- `check_cancel(token, state)` — the cancel point; returns
  `IoError::canceled` if a cancel was requested and not protected.

This layer is deliberately free of any scheduler / fiber / thread-pool
dependency.

## Lifecycle and fail-fast contracts

| Contract | Enforcement |
|----------|-------------|
| `Completion` address-stable while outstanding | Non-copyable, non-movable. |
| Submit into not-ready `Completion` | Returns `IoError::invalid_state`. |
| `result()` before ready | Debug assert; release returns `invalid_state`. |
| Destroy `AsyncIoContext` with outstanding Completions | Fail-fast (Debug + Release). |
| Move-assign `AsyncIoContext` over destination with outstanding | Fail-fast (Debug + Release). |
| Destroy primitive with live waiters | Debug assert; documented as contract violation. |
| `Mutex` acquisition failure | Fail-fast (`std::terminate`). |

## References

- ADR-async-io-model.md — the accepted L1 async I/O model.
- `docs/architecture/async-runtime.md` — the Scheduler and Fiber layer.
- `docs/architecture/async-synchronization.md` — the primitive layer.
