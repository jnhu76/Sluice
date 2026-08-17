# Design Documents

This directory primarily indexes design documents that are **active** — under
review, approved but not started, or draft proposals for future work.

A small set of completed `phase-*` design-of-record documents intentionally
remain here because repository compliance gates still anchor those exact paths.
They are retained for gate stability and traceability; their presence here does
**not** make them active proposals.

## Status rules

An active design document is one of:

- **Proposed** — under review, not yet binding.
- **Accepted and awaiting implementation** — approved but not started.
- **Draft** — an early proposal not yet ready for review.
- **Decision — intentional defer** — a scope decision retained until a
  documented revisit trigger fires; it does not authorize implementation.

Completed implementation designs are normally moved to historical records under
`docs/history/implementation-plans/` (implementation designs),
`docs/history/formal-design/` (formal models that guided implementation), or
`docs/history/closeout/` (subsystem closeout reports).

**Exception:** completed `docs/design/phase-*` design-of-record files may stay in
place when a compliance/mechanical gate explicitly depends on their path. Such
files are completed records, not active future work, and should be paired with
the corresponding architecture compliance/closeout evidence.

## Current active designs

After E15 (Runtime Foundation), no E12/E13/E14 design documents remain here —
they are implemented and have been moved to `docs/history/`.

The following future-phase proposals may exist:

| Design | Status | Related ADR |
|--------|--------|-------------|
| [Selectable / awaitable composability](selectable-async-composability-decision.md) | Decision — intentional defer (issue #99); no production work authorized | [ADR-async-io-model](../adr/ADR-async-io-model.md), [ADR-execution-model](../adr/ADR-execution-model.md) |
| Fuzz infrastructure | Not yet proposed | — |

If no actual design document exists for a row above, it is a placeholder for
future discussion and **must not** be presented as accepted or planned.

## Completed designs

Most completed designs live under `docs/history/`; gate-pinned phase records are
listed at their retained path.

| Subsystem | Design location | Closeout |
|-----------|----------------|----------|
| E10 WaitNode / WaitQueue | `docs/history/implementation-plans/` | `docs/history/closeout/e10-waitnode-wait-queue.md` |
| E11 Deadline / Timer | `docs/history/implementation-plans/` | `docs/history/closeout/e11-deadline-timer-wait.md` |
| E12-A Event | `docs/history/implementation-plans/` | `docs/history/closeout/e12-event.md` |
| E12-B Semaphore | `docs/history/implementation-plans/` | `docs/history/closeout/e12-semaphore.md` |
| E12-C AsyncMutex | `docs/history/implementation-plans/` | `docs/history/closeout/e12-async-mutex.md` |
| E12-D AsyncCondition | `docs/history/implementation-plans/` | `docs/history/closeout/e12-condition.md` |
| E12-E AsyncQueue | `docs/history/implementation-plans/` | `docs/history/closeout/e12-queue.md` |
| E12-F AsyncRwLock | `docs/history/implementation-plans/e12-rwlock.md` | `docs/history/closeout/e10-e12-api-semantic-closure.md` |
| E13 Select | `docs/history/implementation-plans/e13-select-*.md` | `docs/history/closeout/e13-select-p7-rollback-closeout.md` |
| E14 Threaded/Evented Parity | `docs/history/implementation-plans/e14-threaded-evented-parity-preparation.md` | — |
| Phase E — Bounded Blocking-I/O Backend | `docs/design/phase-e-bounded-threadpool-backend.md` (gate-pinned completed record) | `docs/architecture/phase-e-compliance-gate.md` |

## Navigation

- **Current architecture** — `docs/architecture/`
- **Public API contract** — `docs/reference/api.md`
- **Active ADRs** — `docs/adr/README.md`
- **Historical design docs** — `docs/history/implementation-plans/`, `docs/history/formal-design/`
- **Historical closeout records** — `docs/history/closeout/`
