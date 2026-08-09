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
| [ADR-explicit-io-completion-authority](ADR-explicit-io-completion-authority.md) | Completion Publication Authority | **Accepted** | Async I/O foundation | selected portions of ADR-async-io-model | ADR-explicit-io-request-contract (claim/rollback details, direct claim transition) |
| [ADR-explicit-io-request-contract](ADR-explicit-io-request-contract.md) | Unified Explicit I/O Request Contract | **Accepted** | Async I/O request lifecycle | selected portions of ADR-async-io-model and Completion Authority claim/rollback details | — |
| [ADR-execution-model](ADR-execution-model.md) | Dual Threaded/Evented Execution Model | **Accepted** | Async runtime | — | — |
| [ADR-application-runtime](ADR-application-runtime.md) | Application Runtime Architecture | **Accepted** | Async runtime (E16) | — | — |

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
- **Superseded by:** ADR-explicit-io-completion-authority for publication
  authority; ADR-explicit-io-request-contract identifies a limited
  supersession scope. ADR-execution-model extends it.
- **Current authority?** Yes — defines the three-layer L0/L1/L2 async model.
- **Implementation:** Deferred behind sync-first readiness gate. L1 (Completion<T>, AsyncIoContext) not yet implemented.

### ADR-explicit-io-completion-authority: Completion Publication Authority

- **Status:** Accepted
- **Subsystem:** Async I/O foundation
- **Supersedes:** conflicting publication-authority and caller-lifecycle
  portions of ADR-async-io-model
- **Superseded by:** ADR-explicit-io-request-contract for the direct
  claim transition and pre-accept rollback details only
- **Current authority?** Yes — private publication mutators, backend claim,
  reap-only publication, and fail-fast invalid transitions landed in PR #61.

### ADR-explicit-io-request-contract: Unified Explicit I/O Request Contract

- **Status:** Accepted
- **Subsystem:** Async I/O request lifecycle
- **Supersedes:** selected request identity, admission, reap,
  cancellation-target, and borrow-release portions of ADR-async-io-model, plus
  the direct claim transition and pre-accept rollback details of
  ADR-explicit-io-completion-authority
- **Current authority?** Yes — the unified RequestKey / RequestSlot / RequestArena
  lifecycle, the five-stage admission transaction, terminal-winner arbitration,
  reap-only publication, and the borrow/waiter/release rules are the binding
  async request contract. Fake, Sync/Synthetic, and ThreadPool backends conform;
  Uring is mid-migration (see Phase D). Amended by the Decision 18 execution-ownership
  clarification (Uring private-ring execution ownership).
- **Implementation:** RequestArena / RequestSlot / RequestKey landed; Fake, Sync,
  and ThreadPool backends migrated; conformance manifest tracks the remaining
  Uring gap.

### ADR-execution-model (E0): Dual Threaded/Evented Execution Model

- **Status:** Accepted
- **Subsystem:** Async runtime
- **Supersedes:** none
- **Superseded by:** none
- **Current authority?** Yes — defines the Threaded/Evented execution strategy contract.
- **Implementation:** E10–E13 (scheduler, timer, primitives) complete. Evented (E14+) deferred.

### ADR-application-runtime (E16): Application Runtime Architecture

- **Status:** Accepted
- **Subsystem:** Async runtime (E16)
- **Supersedes:** none
- **Superseded by:** none
- **Current authority?** Yes — accepted and implemented.
- **Decides:** the architecture of the E16 Application Runtime layer — ownership, lifecycle, admission, cancellation, drain/join, destructor, restartability, error model, and public-surface direction.
- **Design document:** `docs/design/e16-application-runtime.md`
- **Implementation:** Landed; see `include/sluice/async/application_runtime.hpp`,
  `src/async/application_runtime.cpp`, and the `application_runtime_*` tests.

## Historical notes

- ADR-024S supplemented by `docs/io/sync-error-semantics.md`, `docs/history/closeout/sync-runtime-bench-notes.md`.
- ADR-async-io-model rests on `docs/history/implementation-plans/async-problem-statement.md`, `docs/history/implementation-plans/async-source-inventory.md`, `docs/history/implementation-plans/async-design-alternatives.md` (all historical).
- ADR-execution-model governs the PHASE E job sequence in `docs/history/implementation-plans/zig-stdio-migration-jobs.md` (historical).
