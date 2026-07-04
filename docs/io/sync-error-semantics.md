# Sync I/O error & partial-I/O semantics

**Status:** Reference (sluice-CORE-024S §3). Every row documents behavior the
code actually implements; no row is aspirational. Cross-references
`docs/adr/ADR-024S-sync-runtime-contract.md` (G1–G10, N1–N10).

This document is the reviewer-facing answer to: *how are partial I/O and error
cases handled?* It exists so the merge cannot quietly change a behavior without
the change being visible here.

## Partial-I/O / error table

| Case | Expected behavior (as implemented) | Source |
|------|------------------------------------|--------|
| `read_some` returns 0 | **EOF.** `read_exact` maps this to `IoError::eof` when `dst` is not yet full. | `Reader::read_exact` |
| Short read (0 < n < requested) | `read_exact` advances `dst` by `n` and loops until `dst` is filled or EOF/error. Never returns a partial fill to the caller. | `Reader::read_exact` |
| Short write (0 < n < requested) | `write_all` advances `src` by `n` and loops until `src` is fully written. Never returns a partial write to the caller. | `Writer::write_all` |
| `write_some` returns 0 on non-empty `src` | `write_all` returns `IoError::invalid_state` (broken backend; not an infinite loop). | `Writer::write_all` |
| Reader/Writer returns n > requested | `invalid_state` (defensive; a reader/writer returning more than asked is broken). | `read_exact`, `write_all`, `read_vec`, `write_all_vec` |
| `EINTR` | **Retried**, never propagated. `detail::retry_on_eintr` loops only on `errno == EINTR`; any other return is returned immediately. Applies to read/write/readv/writev/pread/pwrite/preadv/pwritev/fdatasync/fsync. | `detail/posix_retry.hpp` |
| `EAGAIN` / `EWOULDBLOCK` | Propagated as `IoError::would_block`. (The sync backend opens fds blocking; this is reached only if a fd is set non-blocking externally.) Not retried. | `from_errno_value` |
| `ENOSPC` / out of space | `IoError::no_space`. | `from_errno_value` |
| `EACCES` / permission | `IoError::permission_denied`. | `from_errno_value` |
| Other `errno` values | `IoError::backend_error`; the raw `errno` is preserved verbatim in `IoError::errno_value`. `errno == 0` (never a real success) maps to `backend_error`. | `from_errno_value` |
| `fdatasync`/`fsync` failure | Propagated as `IoError` (typically `backend_error` / `no_space`). Not swallowed. `SyncableWriter::sync_data`/`sync_all` return `Result<void>`. | `FileWriter::sync_data`/`sync_all` |
| Positional I/O error | Propagated identically to cursor-based I/O (`Result<T>`). The fd's shared offset is NOT mutated even on error. | `FileReader/Writer::read_at`/`write_at`/`*_vec_at`/`read_at_exact`/`write_at_all` |
| Positional zero progress | `write_at_all`: zero progress on non-empty `src` → `invalid_state` (same rule as `write_all`). `read_at_exact`: EOF before fill → `eof`. | `FileWriter::write_at_all`, `FileReader::read_at_exact` |
| Close / destructor failure | `::close()` is called; its return value is **ignored** (no error surfaced). This is honest: the fd is released; a late close error cannot be reported through `Result` from a destructor. Documented limitation. | `FileReader::close`/`~FileReader`, `FileWriter::close`/`~FileWriter` |
| `flush()` failure | Propagated as `Result<void>`. (`FileWriter::flush` is a no-op currently; `SyncableWriter` subclasses may override.) | `Writer::flush`, `FaultWriter::flush` |
| submit after `BlockingIoPool::shutdown()` | **Silent no-op** (N10). The job is dropped; no error. Callers that need a guarantee must check before submitting or wrap their own error path. | `BlockingIoPool::submit` |
| Task exception inside pool | Captured; the **first** thrown exception is rethrown at the next `wait_all()` (then cleared). The pool stays usable afterward. Subsequent exceptions are dropped (first-wins). | `BlockingIoPool::wait_all` |
| Zero-length `read_exact`/`write_all`/`read_at_exact`/`write_at_all` | **Deterministic no-op**: the loop body never executes; returns success with no syscall. | loop predicates (`while (!dst.empty())` etc.) |

## How this is protected

The rows marked with a behavior the fault layer can reproduce are guarded by
`tests/fault_test.cpp` (short reads/writes, error injection, flush failure) and
the new `tests/sync_contract_negative_test.cpp` (sluice-CORE-024S §4), which
locks: EOF mapping, zero-progress `invalid_state`, zero-length no-op, positional
cursor-independence, and `BlockingIoPool` submit-after-shutdown no-op.

## Not covered (and why)

- **Physical persistence guarantee:** `fsync`/`fdatasync` reflect the
  OS/filesystem contract; they do NOT prove bytes hit non-volatile media. See
  `docs/sync-durability-model.md`. Not a sync-runtime bug — an OS-level limit.
- **Cancellation of in-flight syscalls:** out of scope (N4). A blocking call runs
  to kernel completion.
- **`submit`-after-`shutdown` returning an error:** the current no-op is recorded
  here as the contract (N10). Changing it to an error is a behavior change
  outside this closeout's scope.
