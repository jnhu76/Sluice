# Sluice Developer Documentation

This is the entry point for people **working on** Sluice — contributors,
maintainers, and coding agents. If you want to *use* Sluice as a library,
start at the root [README](../README.md) instead.

## Start here

| You are... | Start with |
|------------|------------|
| Any contributor or coding agent | [`AGENTS.md`](../AGENTS.md) — the repository operating contract |
| Understanding the architecture | [`architecture/overview.md`](architecture/overview.md) |
| Changing the public API | [`reference/api.md`](reference/api.md) + the relevant [ADR](adr/README.md) |
| Changing the async runtime | [`architecture/async-runtime.md`](architecture/async-runtime.md) → ADR → [verification](verification/README.md) |
| Working on applications | [`applications/README.md`](applications/README.md) |
| Investigating a live problem | [`investigations/`](investigations/) |
| Looking for historical context | [`history/`](history/README.md) |

## Documentation authority hierarchy

When two documents disagree, the higher authority wins. `AGENTS.md` §3 defines
the full conflict-resolution chain; the documentation portion is:

```text
Public API contract (include/sluice/ headers + reference/)
        >
Accepted ADR (adr/)
        >
Current architecture (architecture/)
        >
Verification evidence (verification/)
        >
Active design (design/)
        >
Investigation (investigations/)
        >
Roadmap (roadmap/)
        >
Historical documents (history/)   — never current authority
```

## Directory map

| Directory | Question it answers | Audience |
|-----------|--------------------|----------|
| [`reference/`](reference/) | What exactly is the public contract? | All |
| [`architecture/`](architecture/) | How does it work? | Contributor |
| [`adr/`](adr/) | Why was it designed this way? | Contributor |
| [`verification/`](verification/) | How do we prove it works? | Contributor |
| [`applications/`](applications/) | What have real workloads taught us? | Contributor |
| [`design/`](design/) | What are we considering next? | Contributor |
| [`investigations/`](investigations/) | What is being diagnosed right now? | Contributor |
| [`known-issues/`](known-issues/) | What is deliberately deferred? | Contributor |
| [`roadmap/`](roadmap/) | What is active future work? | Contributor |
| [`history/`](history/README.md) | How did we get here? | Maintainer |
| [`post-freeze/`](post-freeze/structural-audit.md) | Post-freeze structural-hygiene records (pinned by the mechanical-facts gate) | Maintainer |
| [`results/`](results/) | Local validation result data (untracked artifacts land here) | Maintainer |
| [`templates/`](templates/) | Document templates used by the architecture gates | Contributor |

## Subsystem map

Statuses: **Implemented** (production headers + sources + tests),
**Experimental** (build-gated, off by default).

### Synchronous core (`sluice_core`)

| Capability | Contract | ADR | Verification |
|------------|----------|-----|--------------|
| Reader / Writer | [`reference/api.md`](reference/api.md), [`reference/sync-io-model.md`](reference/sync-io-model.md) | [ADR-024S](adr/ADR-024S-sync-runtime-contract.md) | `tests/sync_contract_negative_test.cpp` |
| Partial I/O / error semantics | [`reference/sync-error-semantics.md`](reference/sync-error-semantics.md) | ADR-024S | same |
| File and positional I/O | [`architecture/sync-io-architecture.md`](architecture/sync-io-architecture.md) | ADR-024S | `tests/file_positional_test.cpp` |
| Sync backend boundary | [`architecture/sync-backend-taxonomy.md`](architecture/sync-backend-taxonomy.md) | ADR-024S | — |
| WAL and durability | [`architecture/sync-durability-model.md`](architecture/sync-durability-model.md) | ADR-024S | `tests/wal_test.cpp` |
| BlockingIoPool | [`reference/api.md`](reference/api.md) | ADR-024S | `tests/blocking_io_pool_test.cpp` |

### Async runtime (`sluice_async`)

| Capability | Contract | ADR | Verification | Status |
|------------|----------|-----|--------------|--------|
| Scheduler / Fiber | [`architecture/async-runtime.md`](architecture/async-runtime.md) | [ADR-execution-model](adr/ADR-execution-model.md) | deterministic causal tests, multi-worker tests | Implemented |
| WaitNode / WaitQueue | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | `tests/wait_queue_test.cpp` + formal TLA+ | Implemented |
| Event / Semaphore / AsyncMutex / AsyncCondition / AsyncQueue / AsyncRwLock | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | per-primitive `tests/*_test.cpp` | Implemented |
| Select | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | `tests/select_*_test.cpp` | Implemented |
| CancellationToken | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-cancel-request-epoch](adr/ADR-cancel-request-epoch.md) | `tests/cancel_token_test.cpp` | Implemented |
| Future / Group / Batch | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-async-io-model](adr/ADR-async-io-model.md) | `tests/future_test.cpp` etc. | Implemented |
| Completion / AsyncIoContext | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-explicit-io-request-contract](adr/ADR-explicit-io-request-contract.md) | `tests/async_io_context_test.cpp` | Implemented |
| ApplicationRuntime / RuntimeTaskContext | [`reference/api.md`](reference/api.md) | [ADR-application-runtime](adr/ADR-application-runtime.md) | `tests/application_runtime_test.cpp` | Implemented |
| ThreadPoolBackend | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-explicit-io-request-contract | `tests/threadpool_backend_test.cpp` | Implemented |
| UringAsyncBackend | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-explicit-io-request-contract | `tests/uring_backend_test.cpp` | Experimental |

## Reading order before changing a subsystem

1. [`AGENTS.md`](../AGENTS.md) — repository operating contract.
2. This document — orientation.
3. The governing **ADR** under [`adr/`](adr/README.md).
4. The **current architecture document** for the subsystem under
   [`architecture/`](architecture/).
5. The **public API contract** in [`reference/api.md`](reference/api.md).
6. The **verification guide** under [`verification/`](verification/README.md).
7. The **production implementation** under `src/`.
8. The **tests** under `tests/`.

For historical context, see [`history/`](history/README.md). Historical
documents are not current authority.

## Status metadata

Authoritative documents carry a status block near the top:

```text
Status: Current | Accepted | Proposed | Superseded | Historical
Authority: Public Contract | ADR | Architecture | Design | Verification | History
```

Documents without this metadata are either historical records or non-authority
references.
