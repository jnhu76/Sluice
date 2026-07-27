# Sluice Documentation

Sluice is an experimental C++20 I/O control-flow library built around explicit capabilities, pluggable backends, and backend-neutral `Reader` / `Writer` semantics.

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

### Synchronous core

| Capability | Current contract | ADR | Verification |
|------------|-----------------|-----|--------------|
| Reader / Writer | [`api-reference.md`](api-reference.md) ([`sync-io-model.md`](sync-io-model.md)) | [ADR-024S](adr/ADR-024S-sync-runtime-contract.md) | [`sync_contract_negative_test.cpp`](../tests/sync_contract_negative_test.cpp) |
| File and positional I/O | [`sync-io-architecture.md`](sync-io-architecture.md) | ADR-024S | [`file_positional_test`](../tests/file_positional_test.cpp) |
| WAL and durability | [`sync-durability-model.md`](sync-durability-model.md) | ADR-024S | [`wal_test`](../tests/wal_test.cpp) |
| BlockingIoPool | [`api-reference.md`](api-reference.md) | ADR-024S | [`blocking_io_pool_test`](../tests/blocking_io_pool_test.cpp) |

### Async runtime

| Capability | Current contract | ADR | Verification |
|------------|-----------------|-----|--------------|
| Scheduler and fibers | [`async-runtime-plan.md`](async-runtime-plan.md) | [ADR-execution-model.md](adr/ADR-execution-model.md) | Deterministic causal tests |
| WaitQueue and primitives | [`e10-e12-api-semantic-closure.md`](e10-e12-api-semantic-closure.md) | ADR-execution-model | Formal TLA+ models in `docs/spec/` |
| Select / cancellation / timers | *(proposed)* | *(proposed)* | *(proposed)* |
| AsyncIoContext / Batch | *(proposed)* | [ADR-async-io-model.md](adr/ADR-async-io-model.md) | *(proposed)* |

### Experimental

| Capability | Status | Verification |
|------------|--------|-------------|
| io_uring (experimental) | Stub-only; build-gated behind `--with-liburing` | [`docs/io-uring-liburing-validation.md`](io-uring-liburing-validation.md) |

### Formal models

| Model | Guide |
|-------|-------|
| TLA+ specifications | [`docs/spec/`](spec/) |
| BlockingIoPool TLA+ | [`spec/tla/`](../spec/tla/) |

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

For historical context, see `docs/history/`. Historical documents are not current authority.

## Status metadata

Authoritative documents in this repository carry a status block near the top:

```
Status: Current | Accepted | Proposed | Superseded | Historical
Authority: Public Contract | ADR | Architecture | Design | Verification | History
```

Documents without this metadata are either historical records or non-authority references.