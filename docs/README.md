# Sluice Developer Documentation

This is the entry point for people **working on** Sluice — contributors,
maintainers, and coding agents. If you want to *use* Sluice as a library,
start at the root [README](../README.md) instead.

**Load the minimum relevant authority for the task. Do not recursively load
every historical or evidence document.**

## Task routing

| Task | Load first |
|------|------------|
| Use the library | root [README](../README.md) → [`reference/`](reference/README.md) |
| Change public semantics | relevant public header + [`reference/api.md`](reference/api.md) + governing [ADR](adr/README.md) |
| Change request lifecycle | [`architecture/async-request-lifecycle.md`](architecture/async-request-lifecycle.md) + [ADR-explicit-io-request-contract](adr/ADR-explicit-io-request-contract.md) + [verification](verification/README.md) |
| Change wait / synchronization | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) + [ADR-execution-model](adr/ADR-execution-model.md) + [verification](verification/README.md) |
| Change backend / io_uring | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) + [ADR](adr/README.md) + [verification](verification/README.md) |
| Change Scheduler / concurrency | [`architecture/async-runtime.md`](architecture/async-runtime.md) + [constitution AC-6](architecture/architecture-constitution.md) + [verification](verification/README.md) |
| Change failure behavior | [`architecture/failure-model.md`](architecture/failure-model.md) + public contract if observable |
| Change build / CI | [`architecture/overview.md`](architecture/overview.md) (authoritative implementation map) + [verification](verification/README.md) |
| Formal methods work | [`verification/formal-models.md`](verification/formal-models.md) + `spec/tla/manifest.json` |
| Run Safety (S0) work | [#289](https://github.com/jnhu76/Sluice/issues/289) + current contract/architecture only |
| Application / workload work | [`applications/README.md`](applications/README.md) |
| Historical rationale | [`history/`](history/README.md) |

Governing discipline: [`AGENTS.md`](../AGENTS.md) is the durable repository
operating contract (hard invariants, authorization, verification posture).
This page is navigation, not an independent authority — it does not duplicate
the AGENTS authority chain.

## What is current vs historical

- [`architecture/README.md`](architecture/README.md) classifies every
  architecture document: CURRENT authority vs point-in-time evidence/history.
- Documents under [`history/`](history/README.md) (superseded plans, closeouts,
  point-in-time audit evidence) are **never current authority**.
- Scanner reports, investigations, ledgers, comments, commit messages, and
  closeout documents are **evidence**, not automatic authority. When sources
  disagree, resolve through the AGENTS authority chain, fix the stale
  artifact, and record intentional divergence rather than silently picking a
  winner.

## Directory map

| Directory | Question it answers | Audience |
|-----------|--------------------|----------|
| [`reference/`](reference/README.md) | What exactly is the public contract? | All |
| [`architecture/`](architecture/README.md) | How does it work? Current authority + classification index. | Contributor |
| [`adr/`](adr/README.md) | Why was it designed this way? | Contributor |
| [`verification/`](verification/README.md) | How do we prove it works? | Contributor |
| [`applications/`](applications/README.md) | What have real workloads taught us? | Contributor |
| [`design/`](design/README.md) | What designs are proposed or intentionally deferred? | Contributor |
| [`investigations/`](investigations/README.md) | Where do open investigations live? (empty between investigations) | Contributor |
| [`known-issues/`](known-issues/security-review-followups.md) | What is deliberately deferred, and why? | Contributor |
| [`roadmap/`](roadmap/README.md) | Where is execution ordering tracked? (thin pointer to GitHub Issues) | Contributor |
| [`history/`](history/README.md) | How did we get here? Superseded plans, closeouts, audits. | Maintainer |
| [`post-freeze/`](post-freeze/post-freeze-final-report.md) | Post-freeze structural audit evidence; live verification anchor scanned by `scripts/gates/mechanical-facts.py` | Maintainer |
| [`results/`](results/README.md) | Machine-produced validation / benchmark evidence artifacts | Maintainer |
| [`templates/`](templates/) | Document templates used by the architecture gates | Contributor |

## Subsystem map

The capability rows below route to semantic authorities. For production
directories, build/source ownership, and executable verification wiring, use the
[`authoritative implementation map`](architecture/overview.md#authoritative-implementation-map).
Exact target membership remains executable in `xmake.lua` and `xmake/*.lua`.

### Synchronous core (`sluice_core`)

| Capability | Current documentation | ADR |
|------------|-----------------------|-----|
| Reader / Writer | [`reference/api.md`](reference/api.md), [`reference/sync-io-model.md`](reference/sync-io-model.md) | [ADR-024S](adr/ADR-024S-sync-runtime-contract.md) |
| Partial I/O / error semantics | [`reference/sync-error-semantics.md`](reference/sync-error-semantics.md) | ADR-024S |
| File and positional I/O | [`architecture/sync-io-architecture.md`](architecture/sync-io-architecture.md) | ADR-024S |
| Sync backend boundary | [`architecture/sync-backend-taxonomy.md`](architecture/sync-backend-taxonomy.md) | ADR-024S |
| WAL and durability | [`architecture/sync-durability-model.md`](architecture/sync-durability-model.md) | ADR-024S |
| BlockingIoPool | [`reference/api.md`](reference/api.md) | ADR-024S |

### Async runtime (`sluice_async`)

| Capability | Current documentation | ADR |
|------------|-----------------------|-----|
| Scheduler / Fiber | [`architecture/async-runtime.md`](architecture/async-runtime.md) | [ADR-execution-model](adr/ADR-execution-model.md) |
| WaitNode / WaitQueue | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model |
| Event / Semaphore / AsyncMutex / AsyncCondition / AsyncQueue / AsyncRwLock / Select | [`architecture/async-synchronization.md`](architecture/async-synchronization.md) | ADR-execution-model |
| CancellationToken | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-cancel-request-epoch](adr/ADR-cancel-request-epoch.md) |
| Future / Group / Batch | [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-async-io-model](adr/ADR-async-io-model.md) |
| Completion / AsyncIoContext | [`reference/api.md`](reference/api.md), [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | [ADR-explicit-io-request-contract](adr/ADR-explicit-io-request-contract.md) |
| ApplicationRuntime / RuntimeTaskContext | [`reference/api.md`](reference/api.md) | [ADR-application-runtime](adr/ADR-application-runtime.md) |
| AsyncBackend / ThreadPoolBackend / UringAsyncBackend | [`reference/api.md`](reference/api.md), [`architecture/async-io-foundation.md`](architecture/async-io-foundation.md) | ADR-explicit-io-request-contract |

## Reading order before changing a subsystem

1. [`AGENTS.md`](../AGENTS.md) — repository operating contract and canonical
   conflict-resolution rules.
2. This document — orientation only.
3. The governing **ADR** and any explicitly active design/closeout document.
4. The **current architecture document** for the subsystem under
   [`architecture/`](architecture/README.md).
5. The **public API contract** in [`reference/api.md`](reference/api.md).
6. The **verification guide** under [`verification/`](verification/README.md).
7. The **production implementation** under `src/`.
8. The **tests** under `tests/`.

## Navigation stability

- Prefer target, package, and directory boundaries over exhaustive source-file
  inventories.
- Name an exact file only when it is a durable public entry, registry, build
  manifest, schema, or verification driver.
- Do not store test counts, line counts, migration percentages, benchmark
  values, or Phase completion in navigation tables.
- Update moved or renamed targets in the same change. The existing
  `python3 scripts/check-doc-links.py` gate verifies local Markdown paths;
  `python3 scripts/verify-architecture-docs.py` verifies architecture
  classification and implementation-map target names against Xmake.

## Status metadata

Many authority-bearing documents carry a status block such as:

```text
Status: Current | Accepted | Proposed | Superseded | Historical
Authority: Public Contract | ADR | Architecture | Design | Verification | History
```

Treat this metadata as a classification aid, not as a replacement for the
repository-wide authority chain in `AGENTS.md` §2. Some current references do
not yet carry a status block; absence of metadata does not make a document
historical, and a stale status label never outranks a higher authority.
