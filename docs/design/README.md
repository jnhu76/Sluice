# Design Documents

This directory primarily indexes design documents that are **active** — under
review, approved but not started, or draft proposals for future work.

All four completed `phase-*` design-of-record documents were archived to
`docs/history/implementation-plans/` by issue #167 Step 5 (2026-08-25) — see
"Step 5 archive moves" below. None remain here; the directory now holds only
active/proposed designs.

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

**Exception:** a completed `phase-*` design-of-record file may stay in place
when a compliance/mechanical gate explicitly depends on its path. Such a file
is a completed record, not active future work, and should be paired with the
corresponding architecture compliance/closeout evidence. As of issue #167
Step 5 (2026-08-25) all four phase records are archived, so no completed phase
record remains in this directory.

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
| Phase E — Bounded Blocking-I/O Backend | `docs/history/implementation-plans/phase-e-bounded-threadpool-backend.md` (archived design record) | `docs/architecture/phase-e-compliance-gate.md` |

## Phase-record classification (issue #167 Step 5b, baseline `master@6d8ebf1`)

All four `phase-*` files were **completed** design-of-record documents — none
was an active proposal. All four are now archived (see "Step 5 archive moves"
below): phase-b (CLOSED-HISTORY / MOVE-NOW, Step 5c); phase-e, phase-f1,
phase-g (PINNED-EVIDENCE / MOVE-WITH-CONSUMERS, Steps 5e2 / 5e3 / 5e4). They
were classified per the same rule set as `docs/architecture/README.md`:
CURRENT = still consulted design authority; PINNED-EVIDENCE = completed record
whose path is a **mechanical pin** (a script, CI step, or operational
verification anchor that hard-codes it) or a **current code/verification
authority** that explicitly depends on the location; CLOSED-HISTORY = completed
record whose current facts have a CURRENT home and which is an archive-move
candidate — a relocatable prose or source/test comment link is **not** a pin
and does not by itself force PINNED-EVIDENCE. GitHub issue state is not the
decision. The directory currently holds no completed `phase-*` records.

Move policy: classify before move; a `phase-*` file moves only when its
exact-path consumers are updated atomically, a historical banner is added, and
no active verification anchor depends on the current path (mirrors
`docs/architecture/README.md`).

## Step 5 archive moves (executed 2026-08-25, issue #167 Step 5c / 5e2 / 5e3 / 5e4)

- [`phase-b-request-slot-reference.md`](../history/implementation-plans/phase-b-request-slot-reference.md)
  (CLOSED-HISTORY / MOVE-NOW) — relocatable consumers updated atomically:
  `docs/architecture/phase-b-compliance-gate.md:3` (link),
  `tests/request_arena_test.cpp:6` (test comment),
  `xmake/tests/async.lua:12` (lua comment — found on re-scan, not in the Step
  5b inventory). Historical banner added. Old path registered in `KNOWN_MOVED`
  (`scripts/check-doc-links.py`); any new reference fails the docs gate.
- [`phase-e-bounded-threadpool-backend.md`](../history/implementation-plans/phase-e-bounded-threadpool-backend.md)
  (PINNED-EVIDENCE / MOVE-WITH-CONSUMERS, executed 5e2) — production "frozen
  design" comments redirected to the historical record (the current invariants
  they cite have CURRENT homes: ADR-explicit-io-request-contract, AGENTS.md
  §12.1, async-request-lifecycle.md). Consumers updated atomically:
  `threadpool_backend.hpp:19`, `threadpool_backend.cpp:5`,
  `phase-e-compliance-gate.md:3`, `divergence-registry.md:102`,
  `as-built-async-architecture.md:104`. Banner added; old path in `KNOWN_MOVED`.
- [`phase-f1-scheduler-ready-sink.md`](../history/implementation-plans/phase-f1-scheduler-ready-sink.md)
  (PINNED-EVIDENCE / MOVE-WITH-CONSUMERS, executed 5e3) — F1 is implemented;
  the production header comment now points at the CURRENT ReadySink contract
  (`docs/architecture/async-request-lifecycle.md`, ADR §9.4) instead of the
  historical design; banner records the implemented status (in-body "Design"
  status preserved as historical text). Consumers updated atomically:
  `async_io_context.hpp:181`, `scheduler_identity_wake_test.cpp:16`,
  `uring_f1_scheduler_routing_test.cpp:20`, `phase-f1-compliance-gate.md:4`,
  `as-built-async-architecture.md:520,570`, `remediation-roadmap.md:532`.
  Old path in `KNOWN_MOVED`.
- [`phase-g-backend-progress-wake.md`](../history/implementation-plans/phase-g-backend-progress-wake.md)
  (PINNED-EVIDENCE / MOVE-WITH-CONSUMERS, executed 5e4; heaviest pin set) —
  IMPLEMENTED / COMPLETE (2026-08-15 closeout). The current park/wake
  invariants (R1–R4, split-wait bridge, MIXED-WAKE backstop, wake-bridge
  lost-wake closure) remain documented in CURRENT authority:
  ADR-execution-model §9.4/§9.4.7.2, foundation-freeze.md,
  phase-g-compliance-gate.md, `spec/tla/e9_park_wake/`. Consumers updated
  atomically: `scheduler.hpp:1702`, `scheduler_park_wake.cpp:166`,
  `foundation-freeze.md:87,103`, `phase-g-compliance-gate.md:5`,
  `remediation-roadmap.md:566`, `structural-audit.md:27`,
  `spec/tla/e9_park_wake/README.md:23`, three phase-g test suites, and
  `async_internal.lua:257,315`. Banner added; old path in `KNOWN_MOVED`.
- All four `phase-*` records are now archived; none remains in `docs/design/`.

## Navigation

- **Current architecture** — `docs/architecture/`
- **Public API contract** — `docs/reference/api.md`
- **Active ADRs** — `docs/adr/README.md`
- **Historical design docs** — `docs/history/implementation-plans/`, `docs/history/formal-design/`
- **Historical closeout records** — `docs/history/closeout/`
- **Investigation records** — `docs/investigations/README.md`
