# Sluice Documentation

Sluice is an experimental C++20 I/O control-flow library built around explicit
capabilities, pluggable backends, and backend-neutral `Reader` / `Writer`
semantics.

## Quick navigation

| What you need | Where to find it |
|---------------|------------------|
| **Public API contract** | [`docs/api-reference.md`](api-reference.md) |
| **Accepted Architecture Decisions** | [`docs/adr/README.md`](adr/README.md) |
| **Current architecture** | [`docs/architecture/overview.md`](architecture/overview.md) |
| **Proposed/unapproved designs** | [`docs/design/README.md`](design/README.md) |
| **Testing and verification** | [`docs/verification/README.md`](verification/README.md) |
| **Active roadmap** | [`docs/roadmap/README.md`](roadmap/README.md) |
| **Historical closeout records** | [`docs/history/closeout/`](history/closeout/) |
| **Changelog** | [`docs/changelog.md`](changelog.md) |

## Subsystem map

Statuses: **Implemented** (production headers + sources + tests), **Experimental**
(build-gated, off by default), **Proposed** (design not yet authorized),
**Historical** (superseded or closeout evidence).

### Synchronous core (`sluice_core`)

| Capability | Current contract | ADR | Verification |
|------------|-----------------|-----|--------------|
| Reader / Writer | [`api-reference.md`](api-reference.md) ([`sync-io-model.md`](sync-io-model.md)) | [ADR-024S](adr/ADR-024S-sync-runtime-contract.md) | [`sync_contract_negative_test.cpp`](../tests/sync_contract_negative_test.cpp) |
| File and positional I/O | [`sync-io-architecture.md`](sync-io-architecture.md) | ADR-024S | [`file_positional_test`](../tests/file_positional_test.cpp) |
| WAL and durability | [`sync-durability-model.md`](sync-durability-model.md) | ADR-024S | [`wal_test`](../tests/wal_test.cpp) |
| BlockingIoPool | [`api-reference.md`](api-reference.md) | ADR-024S | [`blocking_io_pool_test`](../tests/blocking_io_pool_test.cpp) |

### Async runtime (`sluice_async`)

| Capability | Current contract | ADR | Verification | Status |
|------------|-----------------|-----|--------------|--------|
| Scheduler / Fiber | [`async-runtime.md`](architecture/async-runtime.md) | [ADR-execution-model.md](adr/ADR-execution-model.md) | Deterministic causal tests, multi-worker tests | Implemented |
| WaitNode / WaitQueue | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`wait_queue_test`](../tests/wait_queue_test.cpp), formal TLA+ | Implemented |
| Event | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`event_primitive_test`](../tests/event_primitive_test.cpp) | Implemented |
| Semaphore | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`semaphore_primitive_test`](../tests/semaphore_primitive_test.cpp) | Implemented |
| AsyncMutex | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`async_mutex_primitive_test`](../tests/async_mutex_primitive_test.cpp) | Implemented |
| AsyncCondition | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`async_condition_primitive_test`](../tests/async_condition_primitive_test.cpp) | Implemented |
| AsyncQueue | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`async_queue_primitive_test`](../tests/async_queue_primitive_test.cpp) | Implemented |
| AsyncRwLock | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`async_rwlock_test`](../tests/async_rwlock_test.cpp) | Implemented |
| Select | [`async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | [`select_*_test`](../tests/select_inline_test.cpp) | Implemented |
| CancellationToken | [`async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-async-io-model.md](adr/ADR-async-io-model.md) | [`cancel_token_test`](../tests/cancel_token_test.cpp) | Implemented |
| Future / Group | [`async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-async-io-model | [`future_test`](../tests/future_test.cpp), [`group_test`](../tests/group_test.cpp) | Implemented |
| Completion / AsyncIoContext | [`async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-async-io-model | [`async_io_context_test`](../tests/async_io_context_test.cpp) | Implemented |
| Batch | [`async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-async-io-model | [`batch_test`](../tests/batch_test.cpp) | Implemented |
| ThreadPoolBackend | [`async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-async-io-model | [`threadpool_backend_test`](../tests/threadpool_backend_test.cpp) | Implemented |
| UringAsyncBackend | [`async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-async-io-model | [`uring_backend_test`](../tests/uring_backend_test.cpp) | Experimental |

### Experimental

| Capability | Status | Verification |
|------------|--------|-------------|
| io_uring (`UringAsyncBackend`) | Experimental; stub-only by default, real path gated behind `--with-liburing` | [`uring_backend_test`](../tests/uring_backend_test.cpp) (stub-mode) |

### Formal models

| Model | Guide |
|-------|-------|
| TLA+ specifications | [`spec/tla/`](../spec/tla/) |

## Reading order for agents

Before changing a subsystem, read the following in order:

1. **AGENTS.md** (repository operating contract)
2. This document (docs/README.md) — for orientation
3. The relevant **ADR** under `docs/adr/`
4. The **current architecture document** for the subsystem
5. The **public API contract** in `docs/api-reference.md`
6. The **verification guide** under `docs/verification/`
7. The **production implementation** under `src/`
8. The **tests** under `tests/`

For historical context, see `docs/history/`. Historical documents are not
current authority.

## Status metadata

Authoritative documents in this repository carry a status block near the top:

```
Status: Current | Accepted | Proposed | Superseded | Historical
Authority: Public Contract | ADR | Architecture | Design | Verification | History
```

Documents without this metadata are either historical records or non-authority
references.
