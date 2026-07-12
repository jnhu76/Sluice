# Sluice Async Runtime Construction Roadmap

## Current Baseline

Sluice has completed the execution substrate through E9 (Scheduler park/wake
and external-wake protocol), and the wait-queue protocol through E10
(WaitNode + cancellation-safe WaitQueue core). E8 (runnable ownership transfer
/ work stealing) superseded E7's conservative pinned-Fiber contract.

Current accepted capabilities:

```text
Completion state machine
AsyncBackend seam

Fake backend
ThreadPool backend
io_uring backend

stackful Fiber
x86_64 context switching
Fiber lifecycle state machine

single-worker Evented Scheduler

Completion await / suspend / resume
persistent ready-flag waiting

Evented Future
Evented Group

backend progress policy
poll / wait_one distinction

multi-worker Scheduler
Worker-local execution state (sched_ctx is reached only via the local
    worker's WorkerState at every context-switch site — ADR §9.2.2)

runnable ownership transfer / work stealing (E8 — supersedes E7 pinning;
    a stolen Fiber may resume on the thief; wake routes by WaitReg.owner)

owner-preserving wake routing (route by the wait-epoch resume owner
    captured at suspend; the executing Worker's own sched_ctx is used)

serialized backend access

MW-S1 / MW-S2 / MW-S3 / QUIESCENT protocol

two-phase blocking admission

exactly-once runnable publication

Scheduler park/wake + external-wake protocol (E9)

fresh single-use WaitNode per wait epoch (E10)
one canonical resolve_ CAS resolution authority (E10)
cancellation-safe WaitQueue (wake vs cancel, one winner) (E10)
sealed Scheduler-integrated registration/resolution authority (E10-CORRECTIVE-2)
WaitNode wait-epoch isolation (structural: node identity + absorbing terminal)
external wake-domain classification closure (E10-CORRECTIVE C1)

TLA+:
    runnable-publication protocol
    multi-worker progress/admission protocol
    runnable ownership-transfer protocol (E8)
    park/wake liveness + external-wake protocol (E9)
    WaitNode single-winner + no-double-completion (E10)
```

The runtime currently has a mature execution substrate.

It does not yet have a complete asynchronous wait/runtime service layer.

The next construction stages should be ordered by protocol dependency.

---

# E8 — Runnable Ownership Transfer / Work Stealing

## Goal

Allow an idle Worker to execute Runnable Fibers currently owned by another Worker.

## Load-bearing protocol

```text
Runnable ticket
        +
Fiber execution owner
        ↓
atomic abstract ownership transfer
```

Steal is:

```text
MOVE runnable ticket
+
TRANSFER execution ownership
```

Steal is not:

```text
new runnable publication
```

## Scope

```text
Runnable-only stealing
owner transfer
new-owner wake routing
steal-vs-pop exclusivity
MW-S1 integration
```

## Explicitly deferred

```text
lock-free deque optimization
NUMA policy
priority
affinity
```

## Formal gate

TLA+ ownership-transfer model:

```text
correct ownership transfer PASS

ticket-only steal with stale owner
    =>
counterexample
```

## Exit condition

A stolen Fiber can:

```text
W0 runnable
-> steal W1
-> run W1
-> suspend
-> wake
-> resume W1
```

with exactly one runnable ticket throughout.

---

# E9 — Scheduler Park/Wake and External-Wake Protocol

## Why E9 changes from the old roadmap

The old roadmap called this:

```text
Io-aware wait queues / futex layer
```

That name is too narrow.

The real missing runtime abstraction is:

```text
How does an idle Scheduler Worker park,
and what sources are allowed to wake it?
```

E6 only solved:

```text
backend outstanding
    ->
ctx.wait_one()
```

E5 persistent ready flags can be completed by an external OS thread, but the Scheduler currently has no general external wake primitive.

Persistent readiness prevents lost wake semantically.

It does not provide runtime liveness while Workers are parked or after `run()` has returned.

## Goal

Create one explicit Scheduler wake/parking substrate.

Conceptual model:

```text
Worker sees no executable local/global work

final global recheck

park generation / wake epoch established

Worker parks

producer publishes wake-relevant state

producer signals Scheduler wake source

Worker wakes

re-drain
reclassify
```

## Required wake sources

At minimum:

```text
runnable publication
external Future ready producer
backend progress
shutdown/termination coordination
```

Do not make every primitive invent its own condition variable.

## Key race

```text
check no work
        ↓
producer publishes work
        ↓
Worker parks forever
```

E9 must establish:

```text
publish-before-park
publish-during-park
publish-after-park
```

all preserve liveness.

This is another admission protocol and should receive a narrow TLA+ model.

## Possible implementation mechanisms

Implementation mechanism is secondary to protocol:

```text
condition_variable
futex
eventfd
platform wake primitive
```

The protocol should not be named after one mechanism.

## Exit condition

An external OS thread may complete an Evented Future and wake a parked Scheduler without polling or caller-driven scheduler re-entry.

---

# E10 — WaitNode and Cancellation-Safe Wait Queue Core

## Goal

Create one reusable representation of a suspended asynchronous waiter.

Today Scheduler wait state is distributed across:

```text
waiting_size_
waiting_void_
waiting_ready_
```

This was acceptable while proving Completion and persistent-ready semantics.

It will become expensive when adding:

```text
Mutex
Condition
Event
Semaphore
Queue
Timer
Select
```

E10 should introduce the common wait-queue substrate before adding all of those APIs.

## Conceptual WaitNode

A WaitNode may need to represent:

```text
waiting Fiber
current execution owner
wait source identity
registration state
wake state
cancellation state
optional deadline linkage
```

Do not create a universal abstraction by elegance.

Build it from the proof obligations of the first primitives.

## Required state protocol

Conceptually:

```text
UNREGISTERED
    ↓
REGISTERED
    ↓
CLAIMED_BY_WAKE
    ↓
RUNNABLE_PUBLISHED
```

Cancellation introduces a competing claimant:

```text
REGISTERED
    ↓
CLAIMED_BY_CANCEL
```

Exactly one claimant may win.

This is the asynchronous equivalent of runnable publication capability.

## Main invariant

```text
one suspended wait epoch
    ->
at most one wake/cancel winner
    ->
at most one runnable publication
```

## Exit condition

The Scheduler has a reusable cancellation-safe single-wait queue substrate that does not depend on a particular synchronization primitive. **[CLOSED — `0debd21` / `dbabd21` / `3cd17c6`; as-built: `docs/e10-waitnode-wait-queue.md`]**

---

# E11 — Deadline / Timer Wait Integration

**Status: [CLOSED].** Authoritative spec:
[`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md).
Insertion audit: [`docs/e11-arch-recon-audit.md`](e11-arch-recon-audit.md)
(verdict `E11-READY-WITH-CONSTRAINTS`, implementation GO).
Formal model: [`docs/spec/e11_timer_wait/`](spec/e11_timer_wait/).
As-built: `Scheduler::await_wait_deadline` / `expire_wait` /
`advance_clock`; `TimerRegistration` (independently-stable ACTIVE/RETIRED/CONSUMED
callback-lifetime control block); `WaitNode::expired` third terminal outcome
through the same `resolve_` CAS.

## Why timers come before the synchronization API explosion

Without deadlines, every future API is forced to choose between:

```text
await forever
```

and an ad hoc timeout implementation.

Timeout introduces a competing wait-resolution cause:

```text
RESOURCE_WAKE
    vs
TIMER_EXPIRE
    vs
CANCEL
```

For one wait epoch these must compete for the existing E10 `WaitNode::resolve_`
authority. Timers therefore belong at the wait protocol layer before Mutex,
Event, Condition, Semaphore, Queue, or Select are introduced.

## Scope (summary)

```text
monotonic absolute deadline
Scheduler-owned timer registration
timer expiry as a THIRD resolve_ cause (distinct expired outcome)
deadline-aware Scheduler parking (E9 park-liveness integration)
timer registration retirement / lifetime closure
```

Deferred: high-level sleep API, timerfd/io_uring-timeout public APIs,
timer-wheel performance optimization, Select/multi-wait, all sync primitives.

## Exit condition (summary)

`RESOURCE_WAKE`, `TIMER_EXPIRE`, and `CANCEL` compete through the existing E10
`resolve_` authority; exactly one cause wins; exactly one runnable ticket may be
published; timeout is observably distinct from cancellation; a timer bound to
one wait epoch cannot resolve a later epoch; a retired timer cannot dereference
a destroyed `WaitNode`; an already-due deadline cannot be lost during wait
admission; a Scheduler with an active deadline cannot park indefinitely past it;
the NEG-1…NEG-5 TLA+ models produce counterexamples for broken protocols; I1–I7
hold; deterministic + sanitizer gates green. Full normative text:
[`docs/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md).

---

# E12 — Async Synchronization Primitives

Only after E9-E11 should Sluice add user-facing asynchronous synchronization primitives.

Canonical decomposition (protocol-dependency order, resolved by the E12
preparation audit — see [`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md)):

```text
E12-A Event              [CLOSED — E12-A-EVENT-CORRECTIVE-2 + ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1; both independent reviews PASS; fresh formal gate PASS (0-ed) — docs/e12-event.md; formal: docs/spec/e12_event/]
E12-B Semaphore          [NEXT — may begin]
E12-C Mutex
E12-D Condition
E12-E Queue
E12-F RwLock
E12-G Cross-Primitive Cancellation / Deadline Audit
```

This order matches the SYNCHRONIZATION dependency trunk below (§ Summary) and
supersedes the earlier decomposition-list order. It was chosen by protocol
dependency (Event → Semaphore → Mutex → Condition → Queue → RwLock → Audit),
not by API popularity, and is authoritative now that the E12 preparation
audit is complete. The per-primitive semantic authority and the outstanding
human decisions are recorded in the preparation document.

## Event

Simplest persistent readiness primitive.

Useful to validate WaitNode integration.

## Semaphore

Introduces permit accounting and waiter queue ordering.

Key invariant (supply accounting — corrected in E12-PREP-CORRECTIVE-2; see
[`docs/e12-sync-primitives-plan.md`](e12-sync-primitives-plan.md) §5.1):

```text
initial_supply + successful_release_count
    ==
free_permits + granted_not_yet_committed + successful_acquire_count
```

Queued demand is a *separate* dimension (count of live wait epochs still
demanding a permit) and MUST NOT be counted on the supply side.

## Mutex

Introduces exclusive ownership.

Must decide:

```text
fairness
handoff
barging
```

Do not copy `std::mutex` semantics without defining Evented ownership.

## Condition

Depends on Mutex.

The critical operation is:

```text
release mutex
+
register condition wait
+
suspend
```

and later:

```text
wake
+
reacquire mutex
```

Condition should not be implemented before the wait protocol is stable.

## Queue

Introduces producer/consumer backpressure.

Requires:

```text
not-empty waiters
not-full waiters
close semantics
```

## RwLock

Highest state complexity among the basic primitives.

Do it last.

## General primitive rule

Every primitive must map onto:

```text
WaitNode registration
wake/cancel/deadline claim
runnable publication
Scheduler wake
```

A primitive is not allowed to create a private scheduler side channel.

---

# E13 — Select / Multi-Wait Winner Protocol

## Goal

Allow one Fiber to await multiple asynchronous alternatives.

Conceptually:

```text
select {
    Completion A
    Future B
    Timer C
    Event D
}
```

## Why Select is late

Single-source waiting currently has:

```text
one wait epoch
one registration
one wake capability
```

Select changes this to:

```text
one logical wait epoch
N physical registrations
one winner
N-1 losers requiring deregistration
```

This is a significant state-space increase.

## Core protocol

```text
SelectState = OPEN

source A claims
    OPEN -> WON_A

source B attempts claim
    loses

timer attempts claim
    loses

winner publishes Fiber once

all losing registrations are removed
```

The winner CAS or equivalent claim is another capability protocol.

## Required formal verification

TLA+ should cover:

```text
two sources become ready concurrently
source ready vs timeout
source ready vs cancel
winner selected exactly once
Fiber published exactly once
loser registrations eventually removed
```

## Exit condition

One logical Fiber wait can safely span heterogeneous readiness sources.

---

# Deferred — High-Level Async I/O Bridge

**Not in current frontier (E9–E14).** Preserved as a future obligation.

At this point Sluice has:

```text
backend Completion
Fiber suspension
Scheduler progress
Worker migration
Scheduler wake
WaitNode
deadlines
synchronization
Select
```

Only now should the runtime expose a higher-level asynchronous I/O experience.

## Goal

Build the application-facing bridge over the existing explicit I/O core.

Potential first surfaces:

```text
AsyncReader
AsyncWriter

read
write
read_at
write_at

read_exact
write_all

accept
connect
recv
send
```

Do not move workload-specific policy into the low-level core.

## Key design question

The bridge should answer:

```text
What does a Fiber await?
What owns the Completion?
What owns the buffer?
How does cancellation propagate?
How does deadline race completion?
```

The low-level explicit I/O semantics remain authoritative.

This layer composes them into Evented execution.

## Exit condition

Normal application code can perform asynchronous I/O without manually wiring `Completion<T>` and Scheduler await calls.

---

# E14 — Threaded vs Evented Semantic Parity and Runtime Closure

The original roadmap called this E12.

It should remain, but much later.

## Goal

Compare observable semantics between:

```text
Threaded policy
Evented policy
```

for:

```text
Future
Group
I/O
Event
Semaphore
Mutex
Condition
Queue
deadlines
cancellation
Select
```

## Audit dimensions

```text
result propagation
error propagation
cancellation
deadline
exactly-once completion
wake cardinality
close semantics
repeated await
ownership
exception handling
```

Performance is not semantic parity.

## Exit condition

The same public operation has explicitly documented semantic differences or proven parity across policies.

No accidental divergence.

---

# E15 — Runtime Hardening and Performance Architecture (deferred — not in current frontier)

Only after semantic closure should Sluice optimize the runtime architecture.

Potential work:

```text
Chase-Lev work-stealing deque
lock contention reduction
per-Worker progress structures
batched inbox drain
NUMA topology
affinity
backend sharding
io_uring ring topology
cache-line placement
false-sharing audit
parking strategy optimization
```

Every optimization must preserve the existing protocol models.

For example:

```text
global-lock StealRunnable
```

may later become:

```text
Chase-Lev steal CAS
+
ownership transfer protocol
```

The data structure changes.

The semantic action does not.

## Required discipline

For every optimization:

```text
old abstract action
        ↓
new implementation refinement
```

Do not reopen the runtime protocol merely because a faster data structure has different mechanics.

---

# Summary

The recommended construction order is:

```text
E8
Runnable ownership transfer / work stealing

E9
Scheduler park/wake and external-wake protocol [CLOSED]

E10
WaitNode and cancellation-safe wait queue core [CLOSED]

E11
Deadline / timer wait integration   [CLOSED — spec: docs/e11-deadline-timer-wait.md; formal: docs/spec/e11_timer_wait/]

E12
Async synchronization primitives
  E12-A Event
  E12-B Semaphore
  E12-C Mutex
  E12-D Condition
  E12-E Queue
  E12-F RwLock
  E12-G Cross-Primitive Cancellation / Deadline Audit

E13
Select / multi-wait winner protocol

E14
Threaded vs Evented semantic parity and runtime closure
```

The dependency trunk is:

```text
EXECUTION
    Fiber
    Scheduler
    backend progress
    multi-worker
    ownership transfer

        ↓

LIVENESS
    park
    wake
    external producer notification

        ↓

WAIT PROTOCOL
    WaitNode
    one wake winner
    cancellation

        ↓

TIME
    deadline
    timeout race

        ↓

SYNCHRONIZATION
    Event
    Semaphore
    Mutex
    Condition
    Queue
    RwLock

        ↓

COMPOSITION
    Select / multi-wait

        ↓

SEMANTIC CLOSURE
    Threaded vs Evented parity
```

The runtime should continue to be built by protocol dependency, not by API popularity.

## Construction method lock (E9-CORRECTIVE)

All async-runtime phases from E9-CORRECTIVE onward MUST follow the normative
construction method in [`docs/async-runtime-construction-method.md`](async-runtime-construction-method.md)
(M1–M9). It was written in response to the E9 defect where the formal model
omitted the run-lifetime dimension and green-lit a deterministic hang. An
async-runtime phase that violates M1–M9 (e.g. omits a load-bearing state
dimension, ships a hidden semantic switch, or relies on `sleep_for`-based
causal proof) is NOT accepted as CLOSED.

