# ADR Index

Architecture Decision Records for Sluice.

## Status key

| Status | Meaning |
|--------|---------|
| **Accepted** | Decision approved; current authority unless superseded |
| **Proposed** | Under review; not yet binding |
| **Superseded** | Replaced by a later decision |
| **Rejected** | Considered and declined |
| **Deprecated** | Still standing but not recommended for new work |

## Active ADRs

| ADR | Title | Status | Subsystem | Supersedes | Superseded by |
|-----|-------|--------|-----------|------------|---------------|
| [ADR-024S](ADR-024S-sync-runtime-contract.md) | Sync Runtime Contract | **Accepted** | Synchronous core | — | — |
| [ADR-async-io-model](ADR-async-io-model.md) | Async I/O model | **Accepted** | Async runtime | — | — |
| [ADR-execution-model](ADR-execution-model.md) | Dual Threaded/Evented Execution Model | **Accepted** | Async runtime | — | — |

## ADR details

### ADR-024S: Sync Runtime Contract

- **Status:** Accepted
- **Subsystem:** Synchronous core
- **Supersedes:** none
- **Superseded by:** none
- **Current authority?** Yes — defines G1-G11 guarantees and N1-N9 non-goals for the synchronous I/O layer.
- **Implementation:** Jobs 017S–023S complete. See `docs/sync-io-architecture.md` and `docs/sync-io-model.md`.
- **Verification:** `tests/sync_contract_negative_test.cpp`

### ADR-async-io-model (016D): Async I/O Model

- **Status:** Accepted
- **Subsystem:** Async runtime
- **Supersedes:** none
- **Superseded by:** none (ADR-execution-model extends it)
- **Current authority?** Yes — defines the three-layer L0/L1/L2 async model.
- **Implementation:** Deferred behind sync-first readiness gate. L1 (Completion<T>, AsyncIoContext) not yet implemented.

### ADR-execution-model (E0): Dual Threaded/Evented Execution Model

- **Status:** Accepted
- **Subsystem:** Async runtime
- **Supersedes:** none
- **Superseded by:** none
- **Current authority?** Yes — defines the Threaded/Evented execution strategy contract.
- **Implementation:** E10–E13 (scheduler, timer, primitives) complete. Evented (E14+) deferred.

## Historical notes

- ADR-024S supplemented by `docs/io/sync-error-semantics.md`, `docs/history/closeout/sync-runtime-bench-notes.md`.
- ADR-async-io-model rests on `docs/history/implementation-plans/async-problem-statement.md`, `docs/history/implementation-plans/async-source-inventory.md`, `docs/history/implementation-plans/async-design-alternatives.md` (all historical).
- ADR-execution-model governs the PHASE E job sequence in `docs/history/implementation-plans/zig-stdio-migration-jobs.md` (historical).