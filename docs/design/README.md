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

Most completed designs live under `docs/history/`; gate-/consumer-pinned phase
records are classified in "Phase-record classification" below.

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

## Phase-record classification (issue #167 Step 5b, baseline `master@6d8ebf1`)

All four `phase-*` files below are **completed** design-of-record documents —
none is an active proposal (the only active design row is the table above).
They are classified per the same rule set as `docs/architecture/README.md`:
CURRENT = still consulted design authority; PINNED-EVIDENCE = completed record
whose path is a **mechanical pin** (a script, CI step, or operational
verification anchor that hard-codes it) or a **current code/verification
authority** that explicitly depends on the location; CLOSED-HISTORY = completed
record whose current facts have a CURRENT home and which is an archive-move
candidate — a relocatable prose or source/test comment link is **not** a pin
and does not by itself force PINNED-EVIDENCE. GitHub issue state is not the
decision.

| File | Self-declared status | Class | Pins / consumers | Move candidate | Rationale |
|------|----------------------|-------|------------------|----------------|-----------|
| [`phase-b-request-slot-reference.md`](phase-b-request-slot-reference.md) | Accepted (governs the Phase B implementation) | **CLOSED-HISTORY** | `docs/architecture/phase-b-compliance-gate.md:3` (link), `tests/request_arena_test.cpp:6` (test comment) — both **relocatable path consumers**, atomically updated with the move; no script/spec/mechanical gate hard-codes the path | `docs/history/implementation-plans/` (MOVE-NOW; atomic move set: `git mv` + update both consumers + historical banner) | Design implemented (all four backends on the RequestArena lifecycle); current facts carried by ADR-explicit-io-request-contract, AGENTS.md §4.1/§10–§12, `docs/architecture/async-request-lifecycle.md`, and the compliance gate (EVIDENCE) |
| [`phase-e-bounded-threadpool-backend.md`](phase-e-bounded-threadpool-backend.md) | Design frozen (governs the Phase E implementation) | **PINNED-EVIDENCE** | `include/sluice/async/threadpool_backend.hpp:19`, `src/async/threadpool_backend.cpp:5` (production "frozen design" comments); `docs/architecture/divergence-registry.md:102`; `docs/architecture/phase-e-compliance-gate.md:3`; `docs/architecture/as-built-async-architecture.md:104`; this README's completed-designs row ("gate-pinned completed record") | `docs/history/implementation-plans/` (MOVE-WITH-CONSUMERS, deferred) | Explicitly retained as the gate-pinned completed record; production backend code points at it as the frozen design. Atomic move set: the six consumers listed |
| [`phase-f1-scheduler-ready-sink.md`](phase-f1-scheduler-ready-sink.md) | Design (Issue #98 F1) — implemented, banner stale | **PINNED-EVIDENCE** | `include/sluice/async/async_io_context.hpp:181` (production comment); `tests/scheduler_identity_wake_test.cpp:16`; `tests/uring_f1_scheduler_routing_test.cpp:20`; `docs/architecture/phase-f1-compliance-gate.md:4`; `docs/architecture/as-built-async-architecture.md:519,569`; `docs/architecture/remediation-roadmap.md:532` | `docs/history/implementation-plans/` (MOVE-WITH-CONSUMERS, deferred) | F1 delivered (Issue #98 closed); the production Scheduler ReadySink is live and the header documents its lock protocol by pointing at this design. Atomic move set: the six consumers listed; also consider a banner-only factual update ("Design" → implemented) in the same change |
| [`phase-g-backend-progress-wake.md`](phase-g-backend-progress-wake.md) | IMPLEMENTED / COMPLETE | **PINNED-EVIDENCE** | `docs/architecture/foundation-freeze.md:87,103`; `docs/architecture/phase-g-compliance-gate.md:5`; `docs/architecture/remediation-roadmap.md:566`; `docs/post-freeze/structural-audit.md:27`; `include/sluice/async/scheduler.hpp:1702`; `src/async/scheduler_park_wake.cpp:166`; `spec/tla/e9_park_wake/README.md:23`; `tests/phase_g_backend_progress_wake_test.cpp:2`; `tests/phase_g_closeout_test.cpp:3`; `tests/phase_g_closeout_uring_test.cpp:2`; `xmake/tests/async_internal.lua:257,315` | `docs/history/implementation-plans/` (MOVE-WITH-CONSUMERS, deferred; heaviest pin set) | Design-of-record for the current park/wake bridge (R1–R4, interrupt bridge, MIXED-WAKE verdict), referenced by production code, three test suites, the `e9_park_wake` TLA model README, and four architecture records |

Move policy: classify before move; a `phase-*` file moves only when its
exact-path consumers are updated atomically, a historical banner is added, and
no active verification anchor depends on the current path (mirrors
`docs/architecture/README.md`). No move is performed by this classification
pass.

## Navigation

- **Current architecture** — `docs/architecture/`
- **Public API contract** — `docs/reference/api.md`
- **Active ADRs** — `docs/adr/README.md`
- **Historical design docs** — `docs/history/implementation-plans/`, `docs/history/formal-design/`
- **Historical closeout records** — `docs/history/closeout/`
- **Investigation records** — `docs/investigations/README.md`
