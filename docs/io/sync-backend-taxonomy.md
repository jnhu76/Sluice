# Sync backend taxonomy

**Status:** Authoritative planning reference (sluice-CORE-024S §2).
**Scope:** This document is the long-term boundary for sync backend decisions.
It defines which sync backends are allowed, which are deferred, and which belong
to `async-runtime` instead. It exists so future work does not create a "backend
zoo" — every new backend must justify itself against this taxonomy.

Companion to `docs/adr/ADR-024S-sync-runtime-contract.md` (the contract) and
`docs/io/sync-error-semantics.md` (the partial-I/O table).

## Decision statement

> 024S prepares the backend taxonomy but does not create a backend zoo. The only
> new production implementation in 024S is a bounded OS-thread `BlockingIoPool`.
> Everything else is documented as future/deferred taxonomy only.

## 2.1 I/O primitives (what operation is performed)

| Primitive | Examples | Current status |
|-----------|----------|----------------|
| Sequential file I/O | `read`, `write` | **current / default** |
| Positional file I/O | `pread`, `pwrite` | **current** (018S/019S) |
| Durability operations | `fsync`, `fdatasync`, flush-equivalent | **current** (SyncableWriter, 020S) |
| Vectored I/O | `readv`, `writev`, `preadv`, `pwritev` | future, **not 024S** (already exists as non-positional `read_vec`/`write_vec`; positional `*_vec_at` is current — both are stable, no new vectored surface in 024S) |
| Memory mapping | `mmap`, `msync`, `munmap` | future / deferred |
| Direct I/O | `O_DIRECT`, aligned buffers | future / deferred |
| Platform durability variants | Linux/macOS/Windows-specific persistence APIs | future / deferred |

## 2.2 Sync execution models (who waits, where blocking happens)

| Execution model | Description | Current status |
|-----------------|-------------|----------------|
| Direct blocking | caller thread performs the blocking syscall | **current / default** |
| Bounded blocking pool | fixed OS-thread pool executes blocking I/O tasks | **024S production implementation target** (`sluice::BlockingIoPool`) |
| Thread-per-operation | creates one OS thread per operation | **explicitly rejected** for production (per-op thread spawn has unsustainable creation cost; bench `async_writes_bench` measured this as slower than direct blocking) |
| Green threads / fibers | user-space scheduling over carrier OS threads | **out of scope** (requires a runtime) |
| Scheduler / runtime model | generalized task scheduling abstraction | **deferred** until sync and async backends stabilize |

## 2.3 Async / completion backends (NOT part of sync-runtime 024S)

These are documented as belonging elsewhere. **None is implemented in 024S.**

| Backend | Where it belongs |
|---------|------------------|
| io_uring | `async-runtime` / experimental completion backend (`UringAsyncBackend`, gated behind liburing) |
| IOCP | future Windows async backend |
| Async thread-pool backend | `async-runtime` portable fallback (`ThreadPoolBackend`) |
| Sync façade over async backend | only **after** async backends are stable — not now |

## 2.4 Reactor / network backends (out of scope for file-I/O sync-runtime)

| Backend | Reason |
|---------|--------|
| epoll | network/reactor layer, not an ordinary file-I/O completion model |
| kqueue | network/reactor layer, not a current sync file backend |
| select / poll | not a production file-I/O backend target |

## 2.5 Backend decision rules

1. **024S implements only the bounded OS-thread blocking pool.** No other backend is added in this task.
2. **Future sync backends must not be added just because they are possible.** A new sync backend requires: (a) a workload justification, (b) a contract document, (c) negative tests, and (d) a benchmark comparison against direct blocking AND the blocking pool.
3. **io_uring must be implemented and validated in `async-runtime` before any sync façade over it is considered.** A sync façade over an unproven async backend would couple sync correctness to async maturity.
4. **Runtime scheduling is deferred** until sync and async backend semantics are stable. A scheduler built on unstable primitives would ossify their bugs.
5. **Per-operation thread spawning is rejected as a production model.** It is permitted only in benches/tests as a comparison upper bound.

## 2.6 Where the production pool lives

```text
include/sluice/blocking_io_pool.hpp   (namespace sluice)
src/blocking_io_pool.cpp
```

The pre-existing `bench/support/blocking_io_pool.hpp` (namespace `sluice::bench`)
becomes a thin adapter over the production pool — benches do not duplicate the
implementation (merge-readiness rule).
