# Design Documents

This directory indexes design documents that are **active** — under review,
approved but not started, or draft proposals for future work.

## Status rules

A design document is active only when:

- **Proposed** — under review, not yet binding.
- **Accepted and awaiting implementation** — approved but not started.
- **Draft** — an early proposal not yet ready for review.

Completed implementation designs are **not** kept here. They are historical
records under `docs/history/implementation-plans/` (implementation designs),
`docs/history/formal-design/` (formal models that guided implementation), or
`docs/history/closeout/` (subsystem closeout reports).

## Current active designs

After E15 (Runtime Foundation), no E12/E13/E14 design documents remain here —
they are implemented and have been moved to `docs/history/`.

The following future-phase proposals may exist:

| Design | Status | Related ADR |
|--------|--------|-------------|
| [Phase E — Bounded Blocking-I/O Backend](phase-e-bounded-threadpool-backend.md) | Accepted (governing `feat/phase-e-bounded-threadpool-explicit-io`) | [ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md) |
| [E16 Application Runtime](e16-application-runtime.md) | Proposed | [ADR-application-runtime](../adr/ADR-application-runtime.md) |
| Fuzz infrastructure | Not yet proposed | — |

If no actual design document exists for a row above, it is a placeholder for
future discussion and **must not** be presented as accepted or planned.

## Completed designs (moved to history)

These subsystems were designed and implemented in E10–E15. Their design
documents are historical:

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

## Navigation

- **Current architecture** — `docs/architecture/`
- **Public API contract** — `docs/api-reference.md`
- **Active ADRs** — `docs/adr/README.md`
- **Historical design docs** — `docs/history/implementation-plans/`, `docs/history/formal-design/`
- **Historical closeout records** — `docs/history/closeout/`
