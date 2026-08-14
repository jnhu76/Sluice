# Selectable / awaitable async-result composability — scope decision

| | |
|---|---|
| Status | **Decision — intentional defer** (closes issue #99; audit umbrella #94) |
| Date | 2026-08-14 |
| Related ADRs | [ADR-async-io-model](../adr/ADR-async-io-model.md), [ADR-execution-model](../adr/ADR-execution-model.md) (E13 select winner protocol, E11 timer registrations) |
| Authority | AGENTS.md §8 (architecture gate), §13.2 (wake obligation for any new wait surface) |

## Question

What is the correct selectable/awaitable abstraction for heterogeneous
asynchronous result sources in Sluice — and should one be built now?

This is a **design / scope decision**, not an implementation mandate. No
production work is authorized by closing this issue; implementation is a
follow-up only when a revisit trigger (below) fires.

## Current state (verified against the tree)

- `select()` admits **Event + Timer cases only** — no I/O-Completion case.
  `SelectCaseType` constrains to `EventSelectCase` / `TimerSelectCase`
  (`include/sluice/async/select_fwd.hpp`); kinds are `SelectKind{event, timer}`
  (`include/sluice/async/select.hpp`). The exactly-one-winner CAS is proven
  (E13 formal safety).
- A **public `Future<T>` exists** (`include/sluice/async/future.hpp`,
  sluice-CORE-028 T2): caller-owned value channel, producer `complete_with`,
  idempotent `await()`/`cancel()`, `cancel_token()`, with the physical wait
  delegated to a `WaitPolicy&` (Threaded = block thread, Evented = Fiber
  suspend). Closing this issue does **not** imply Sluice has no Future.
- I/O-Completion awaiting is single-op (`Scheduler::await_completion_*`,
  `RuntimeTaskContext::await_completion`). No first-of-N heterogeneous select
  over Completions/Futures exists.
- Scheduler-integrated timer machinery exists internally (`deadline_t`,
  `monotonic_now()`, `await_wait_deadline`, `TimerRegistration`,
  `TimerSelectCase`, `*_until` waits). There is **no standalone public
  `sleep_for`/`Timeout` convenience type**.

## Decision

**Option 4 — intentional defer.** The Event + Timer select domain remains the
supported composability surface. No heterogeneous I/O/Completion/Future select
and no public `sleep_for`/`Timeout` convenience are added now.

Rationale:

- the current Event/Timer select is complete and conformance-mapped for its
  documented domain;
- a public `Future<T>` already exists, so single-value await is covered;
- selecting directly over `Completion<T>` may prematurely freeze the wrong
  abstraction before the wake topology is settled;
- Phase F only just established the public `RequestHandle` identity surface;
- Phase G owns the backend-ready → Scheduler wake bridge, and a speculative
  general-select API should not be entangled with that topology change;
- implement only after a real caller demonstrates the composition shape needed.

This is **not** "ignore forever." It is a recorded defer with explicit revisit
triggers.

The conformance map describes the Event/Timer-only `select()` and the absent
`sleep_for`/`Timeout` as **intentional domain narrowing** (`P` row), not an
accidental missing feature (see
[`docs/architecture/zig-io-conformance-map.md`](../architecture/zig-io-conformance-map.md),
note 6).

## Revisit triggers

Re-open this decision when **one** of the following becomes concrete:

1. a production caller needs I/O-vs-timeout or first-of-N heterogeneous
   composition that the current Event/Timer select cannot express;
2. `Future<T>` needs first-of-N heterogeneous composition;
3. a generic `Selectable` / `WaitSource` abstraction is designed;
4. Phase G changes the wake topology in a way that materially affects the
   design.

`sleep_for` / `Timeout` convenience is **deferred with the same trigger set**;
do not add a convenience API merely to close tracking.

## Constraints any future design must preserve

- exactly-one winner;
- loser waiter cancellation ≠ I/O cancellation (cancel only the wait
  registration, never the underlying operation — the result stays owned by the
  submitter);
- no leaked waiter registration; no double wake;
- shutdown convergence for the new wait surface.

## Non-goals

- No networking (DIV-08).
- No change to the synchronous `Reader`/`Writer` contract.
- No change to existing Event/Timer select semantics.
