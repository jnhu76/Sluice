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
| G9 | `BlockingIoPool` is a **thread-pool execution helper for blocking work, NOT an async runtime.** It runs `std::function<void()>` jobs on fixed `std::thread` workers. It is not selectable as an `IoContext`, implements no completion model, and has no dependency relationship with the I/O types. |
| G10 | Errors are `Result<T>` / `IoError` (codes: `eof`, `canceled`, `interrupted`, `would_block`, `no_space`, `permission_denied`, `invalid_state`, `backend_error`). `errno` is mapped verbatim where the syscall returns `-1`; `errno == 0` maps to `backend_error` (never a real success). |

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
| N10 | `BlockingIoPool::submit` after `shutdown()` is a **silent no-op**, not an error (see G9 + `docs/io/sync-error-semantics.md`). |

### 3. Layering

```text
File / Reader / Writer              (sync I/O types, G1-G8)
    ↓
Direct blocking backend             (::read/::write/::pread/::pwrite/...)
    ↓
Optional BlockingIoPool             (execution helper, isolates blocking work
  (bench/support/, NOT an IoContext)  from caller threads; G9)
    ↓
Higher-level services may build their own concurrency model ABOVE this layer
```

Actor / P2300 / async / `io_uring` belong **above or beside** this layer, never
inside the sync runtime contract. A future async runtime consumes the sync types
as primitives; it does not modify them.

### 4. `BlockingIoPool` lifecycle contract (G9 detail)

- Construct with `threads >= 1` (0 is coerced to 1) and `max_queued >= 1`.
- `submit(job)`: blocks if the queue is at capacity; **no-op after `shutdown()`** (N10); a null job is ignored.
- `wait_all()`: blocks until every submitted job has completed. If a job threw, the first captured exception is rethrown here (then cleared); the pool stays usable afterward.
- `shutdown()`: stops accepting, drains the queue, joins workers. **Idempotent.** The destructor calls it if not already called.
- `thread_count()`: fixed worker count.
- State is **instance-owned only** (no globals); two pools do not interfere (sanitizer-verified).

## Consequences

- The sync layer is **boring, stable, and correct by design**. Reviewers merge it knowing what it is and is not.
- Negative tests (§4 of the task, see `tests/sync_contract_negative_test.cpp`) lock G4–G6, G9, N4, N10 against future drift.
- Any future async backend is built **on top of** these primitives; the contract forbids rewriting `Reader`/`Writer`/positional semantics to serve async.
- No close-out code change is needed for the contract itself; only the `submit`-after-`shutdown` no-op (N10) is recorded honestly rather than retrofitted to an error (changing it would be a behavior change outside this closeout's scope).
