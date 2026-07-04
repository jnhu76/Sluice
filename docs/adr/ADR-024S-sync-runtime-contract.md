# ADR-024S: Sync Runtime Contract (merge-readiness closeout)

**Status:** Accepted (sluice-CORE-024S, sync-runtime merge-readiness).
**Supersedes:** none. **Supplemented by:** `docs/io/sync-error-semantics.md`,
`docs/bench/sync-runtime-bench-notes.md`, `docs/reviews/024S-sync-runtime-merge-readiness.md`.

## Context

The sync-runtime branch carries the stable synchronous I/O foundation:
positional blocking I/O (018S/019S), `BlockingIoPool` (021S), the W1–W4 blocking
benchmark matrix (022S), and durability baselines (020S/023S). It is
sanitizer-clean (TSan/ASan/UBSan/Valgrind). Before merging to `master` as the
default I/O layer, the runtime's guarantees and — crucially — its **non-goals**
must be written down so a future async/io_uring/P2300 abstraction does not leak
back into the sync contract.

This is a **closeout** task, not an expansion: it documents what exists and adds
negative tests that protect the contract. No runtime growth.

## Decision

The sync runtime's contract is fixed as follows.

### 1. Guarantees

| # | Guarantee |
|---|-----------|
| G1 | All calls are **synchronous from the caller's perspective**. A `read_some`/`write_some`/positional/durability call returns only after the underlying operation has completed or failed. |
| G2 | **Direct blocking I/O blocks the calling thread** until completion or error. `FileReader`/`FileWriter` call `::read`/`::write`/`::pread`/`::pwrite`/`::readv`/`::writev`/`::preadv`/`::pwritev`/`::fdatasync`/`::fsync` directly. |
| G3 | `Reader`/`Writer` keep ordinary sync semantics. `read_some`/`write_some` may return short (fewer bytes than requested); the derived `read_exact`/`write_all` are responsible for looping across shorts. |
| G4 | `read_exact(dst)` loops across short `read_some` until `dst` is filled; **EOF before fill → `IoError::eof`**; a reader returning more than asked → `invalid_state`. Empty `dst` is an immediate success (no syscall needed). |
| G5 | `write_all(src)` loops across short `write_some` until `src` is fully written; **zero progress on non-empty `src` → `IoError::invalid_state`** (treated as a broken backend, not an infinite loop); a writer returning more than asked → `invalid_state`. Empty `src` is an immediate success. |
| G6 | **Positional I/O (`read_at`/`write_at`/`*_vec_at`/`read_at_exact`/`write_at_all`) does not mutate the shared file offset.** The underlying `pread`/`pwrite` advance their own per-call offset argument; the fd's cursor is untouched. |
| G7 | **EINTR is retried**, never propagated. All blocking syscalls pass through `detail::retry_on_eintr`, which loops only on `errno == EINTR`. |
| G8 | Durability ops (`SyncableWriter::sync_data`/`sync_all` → `fdatasync`/`fsync`) surface OS/filesystem success or failure via `Result<void>`. They do **not** overclaim physical-media persistence beyond the OS/filesystem contract (see `docs/sync-durability-model.md`). |
| G9 | `BlockingIoPool` (production: `sluice::BlockingIoPool`, `include/sluice/blocking_io_pool.hpp`) is a **bounded OS-thread execution helper for blocking work, NOT an async runtime.** It runs callables on a fixed number of `std::thread` workers (NOT one thread per operation). It exposes task return values via a returned future-like handle, surfaces task exceptions, rejects submissions after `shutdown()`, and is bounded (backpressure / `try_submit` rejection). It is not selectable as an `IoContext`, implements no completion model, and has no dependency relationship with the I/O types. See `docs/io/sync-backend-taxonomy.md` for the full backend boundary. |
| G10 | Errors are `Result<T>` / `IoError` (codes: `eof`, `canceled`, `interrupted`, `would_block`, `no_space`, `permission_denied`, `invalid_state`, `backend_error`). `errno` is mapped verbatim where the syscall returns `-1`; `errno == 0` maps to `backend_error` (never a real success). |
| G11 | `BlockingIoPool::submit` / `try_submit` after `shutdown()` is **rejected** (`IoError::invalid_state`), NOT a silent no-op. Production pool callers must handle the rejection; silent drops hid bugs (the pre-024S bench pool was a no-op, recorded as a known behavior change). |

### 2. Explicit non-goals (NOT guaranteed; do not depend on these)

| # | Non-goal |
|---|----------|
| N1 | No `async`/`await`, coroutines, or `co_await`. |
| N2 | No coroutine abstraction. |
| N3 | No `io_uring` completion-queue semantics in the default sync path. (`src/experimental/uring_*` is gated, off by default, not part of this contract.) |
| N4 | No cancellation of in-flight blocking syscalls. A blocking `read`/`write`/`fsync` runs to completion or kernel error. |
| N5 | No actor mailbox semantics. |
| N6 | No P2300 sender/receiver model. |
| N7 | No scheduler abstraction / task graph. |
| N8 | No attempt to solve function-coloring at this layer. |
| N9 | No implicit buffer lifetime extension: the caller owns buffers; positional/buffered wrappers borrow them. |

### 3. Layering

```text
File / Reader / Writer              (sync I/O types, G1-G8)
    ↓
Direct blocking backend             (::read/::write/::pread/::pwrite/...)
    ↓
Optional BlockingIoPool             (production bounded OS-thread pool, isolates
  (sluice::BlockingIoPool)            blocking work from caller threads; G9.
                                      bench/support/ is now a thin adapter)
    ↓
Higher-level services may build their own concurrency model ABOVE this layer
```

Actor / P2300 / async / `io_uring` belong **above or beside** this layer, never
inside the sync runtime contract. A future async runtime consumes the sync types
as primitives; it does not modify them.

### 4. `BlockingIoPool` lifecycle contract (G9 detail)

The **production** pool (`sluice::BlockingIoPool`, `include/sluice/blocking_io_pool.hpp`):

- Construct via `BlockingIoPoolOptions{worker_count, max_queue_depth}`. `worker_count == 0` and `max_queue_depth == 0` are **rejected** (factory returns `IoError::invalid_state`). Worker threads are fixed in number; thread-creation failure does not leave a half-valid pool (RAII via `unique_ptr<Impl>`).
- `try_submit(callable) -> Result<Task<T>>`: **non-blocking**. Returns a rejection (`IoError::would_block` when full, `invalid_state` after `shutdown()` — G11). The returned `Task<T>` carries the callable's return value and any thrown exception; `task.get()` blocks until complete and surfaces the value or rethrows.
- `submit(callable) -> Result<Task<T>>`: **blocking with backpressure** — waits for queue space, then enqueues. Rejects only after `shutdown()` (G11).
- `shutdown()`: stops accepting, **drains already-submitted work** (running blocking syscalls are NOT cancelled — no async cancellation claim), joins workers. **Idempotent.** Destructor calls it if not already called (drain-and-join).
- `stats() -> PoolStats`: observability — `submitted`, `started`, `completed`, `failed`, `rejected`, `queue_depth`, `worker_count`. Caller-owned (nullable), never global.
- State is **instance-owned only** (no globals); two pools do not interfere (sanitizer-verified).
- **Progress boundary:** tasks are arbitrary user callables, and the pool does
  not guarantee progress for same-pool dependency graphs. A task must not
  synchronously require queued work from the same saturated pool to make forward
  progress. Same-pool recursive blocking `submit()` and capacity-exhausting
  same-pool `Task::get()` waits are outside the progress guarantee. This is not
  a restriction on normal external producers, and ordinary callable capture /
  reference lifetime remains the caller's C++ responsibility.
- NOT an `IoContext`, NOT async, NOT a fiber/green-thread scheduler, NOT a P2300 executor.

### 5. Formal verification scope

The TLA+ model in `spec/tla/BlockingIoPool.tla` verifies the **internal
admission / bounded-queue / dequeue / completion / shutdown-drain protocol**.
It checks:

- `NoInternalProtocolStuck`: every reachable modeled state is either legitimate
  quiescence after admission is closed and accepted work has drained, or has a
  real modeled protocol transition enabled.
- queue bound, lifecycle consistency, and get-after-done linearizability.
- modeled-task progress under the model's weak-fairness assumptions.

The model does **not** prove deadlock freedom for arbitrary user callables or
arbitrary task dependency graphs. It assumes modeled accepted task execution is
finite and excludes same-pool recursive blocking submission and same-pool cyclic
or capacity-exhausting `Task::get()` dependencies.

The **bench adapter** (`sluice::bench::BlockingIoPool`, `bench/support/`) wraps the production pool so benchmarks do not duplicate the implementation.

## Consequences

- The sync layer is **boring, stable, and correct by design**. Reviewers merge it knowing what it is and is not.
- Negative tests (`tests/sync_contract_negative_test.cpp` + `tests/blocking_io_pool_test.cpp`) lock G4-G6, G9, G11, and N4 against future drift.
- Any future async backend is built **on top of** these primitives; the contract forbids rewriting `Reader`/`Writer`/positional semantics to serve async.
- The production pool's `submit`-after-`shutdown` is now a **rejection** (G11 changed from the pre-024S no-op). This is a deliberate behavior change: silent drops hid bugs. The bench adapter preserves the old fire-and-forget `void submit` shape for existing bench call sites, mapping the rejection internally.
