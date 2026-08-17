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

## Authority and conflict resolution

[`AGENTS.md` §3](../AGENTS.md) is the canonical authority chain. This document
must not invent a second hierarchy. In abbreviated form, resolve conflicts in
this order:

```text
1. Explicit current task / approved issue scope / accepted review finding
2. Accepted ADRs and active subsystem design or closeout documents
3. architecture/architecture-constitution.md
4. AGENTS.md
5. Public headers under include/sluice/ + reference/api.md
6. Production implementation under src/
7. Contract / regression / causal / formal tests
8. xmake target and feature-gate definitions
9. GitHub workflow merge gates
10. Root README for orientation and common commands
```

Scanner reports, investigations, roadmap notes, comments, commit messages, and
historical documents are **evidence**, not automatic authority. Documents under
[`history/`](history/README.md) are never current authority.

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

| Capability | Current documentation | ADR | Verification |
|------------|-----------------------|-----|--------------|
| Reader / Writer | [`reference/api.md`](reference/api.md), [`reference/sync-io-model.md`](reference/sync-io-model.md) | [ADR-024S](adr/ADR-024S-sync-runtime-contract.md) | `tests/sync_contract_negative_test.cpp` |
| Partial I/O / error semantics | [`reference/sync-error-semantics.md`](reference/sync-error-semantics.md) | ADR-024S | same |
| File and positional I/O | [`architecture/sync-io-architecture.md`](architecture/sync-io-architecture.md) | ADR-024S | `tests/file_positional_test.cpp` |
| Sync backend boundary | [`architecture/sync-backend-taxonomy.md`](architecture/sync-backend-taxonomy.md) | ADR-024S | — |
| WAL and durability | [`architecture/sync-durability-model.md`](architecture/sync-durability-model.md) | ADR-024S | `tests/wal_test.cpp` |
| BlockingIoPool | [`reference/api.md`](reference/api.md) | ADR-024S | `tests/blocking_io_pool_test.cpp` |

### Async runtime (`sluice_async`)

| Capability | Current documentation | ADR | Verification | Status |
|------------|-----------------------|-----|--------------|--------|
| Scheduler / Fiber | [`architecture/async-runtime.md`](architecture/async-runtime.md) | [ADR-execution-model](adr/ADR-execution-model.md) | deterministic causal tests, multi-worker tests | Implemented |
| WaitNode / WaitQueue | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | `tests/wait_queue_test.cpp` + formal TLA+ | Implemented |
| Event / Semaphore / AsyncMutex / AsyncCondition / AsyncQueue / AsyncRwLock | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | per-primitive `tests/*_test.cpp` | Implemented |
| Select | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model | `tests/select_*_test.cpp` | Implemented |
| CancellationToken | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-cancel-request-epoch](adr/ADR-cancel-request-epoch.md) | `tests/cancel_token_test.cpp` | Implemented |
| Future / Group / Batch | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-async-io-model](adr/ADR-async-io-model.md) | `tests/future_test.cpp` etc. | Implemented |
| Completion / AsyncIoContext | [`reference/api.md`](reference/api.md), [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-explicit-io-request-contract](adr/ADR-explicit-io-request-contract.md) | `tests/async_io_context_test.cpp` | Implemented |
| ApplicationRuntime / RuntimeTaskContext | [`reference/api.md`](reference/api.md) | [ADR-application-runtime](adr/ADR-application-runtime.md) | `tests/application_runtime_test.cpp` | Implemented |
| AsyncBackend / ThreadPoolBackend | [`reference/api.md`](reference/api.md), [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-explicit-io-request-contract | `tests/threadpool_backend_test.cpp` | Implemented |
| UringAsyncBackend | [`reference/api.md`](reference/api.md), [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-explicit-io-request-contract | `tests/uring_backend_test.cpp` | Experimental |

## Reading order before changing a subsystem

1. [`AGENTS.md`](../AGENTS.md) — repository operating contract and canonical
   conflict-resolution rules.
2. This document — orientation only.
3. The governing **ADR** and any explicitly active design/closeout document.
4. The **current architecture document** for the subsystem under
   [`architecture/`](architecture/).
5. The **public API contract** in [`reference/api.md`](reference/api.md).
6. The **verification guide** under [`verification/`](verification/README.md).
7. The **production implementation** under `src/`.
8. The **tests** under `tests/`.

For historical context, see [`history/`](history/README.md). Historical
documents are not current authority.

## Status metadata

Many authority-bearing documents carry a status block such as:

```text
Status: Current | Accepted | Proposed | Superseded | Historical
Authority: Public Contract | ADR | Architecture | Design | Verification | History
```

Treat this metadata as a classification aid, not as a replacement for the
repository-wide authority chain in `AGENTS.md` §3. Some current references do
not yet carry a status block; absence of metadata does not make a document
historical, and a stale status label never outranks a higher authority.
