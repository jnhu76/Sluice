---
name: cpp-concurrency-guidelines
description: Correctness workflow for C++ shared-state concurrency in Sluice. Use when code introduces or changes threads/fibers, mutexes, condition variables, atomics, concurrent queues, publication, wake/park, cancellation, shutdown, or other shared mutable state.
origin: custom
---

# C++ concurrency correctness

Use this skill to make the concurrent protocol explicit before changing synchronization. It complements `cpp-coding-standards`; it does not replace `AGENTS.md` architecture rules.

## Authority and composition

Repository contracts and accepted ADRs define allowed semantics. This skill supplies the C++ concurrency proof/review process.

If the task is also about measured performance, load `cpp-concurrency-performance` after correctness is established.

If correctness depends on CAS loops, memory ordering across atomic operations, linearization, ABA, reclamation, or a lock-free/wait-free progress property, also load `cpp-lock-free`.

## Step 1 — build the shared-state map

For every mutable object touched by more than one execution context, record:

| State | Owner | Readers | Writers | Protection / publication |
|---|---|---|---|---|
| ... | ... | ... | ... | ... |

Include state that is indirectly shared through callbacks, fibers/tasks, backend completions, queues, registries, timers, or shutdown paths.

Prefer removing sharing, transferring ownership, partitioning state, or publishing immutable state before adding synchronization.

**Completion criterion:** every mutable shared state item in the changed protocol has one named ownership/protection story.

## Step 2 — recover the protocol

Write the smallest state machine that explains the changed behavior. For each transition, identify:

```text
trigger
preconditions
locks held
atomic/state writes in program order
queue/ownership mutation
publication or terminal claim
wake/routing action
locks released
observable result
```

Pay special attention to transitions involving:

- enqueue/dequeue;
- waiter registration/removal;
- grant/ownership transfer;
- terminal resolution;
- cancellation/expiration;
- wake-before-suspend / park commit;
- shutdown/drain;
- worker exit or ownership migration.

Do not fuse multiple C++ steps into one conceptual step when another thread/fiber can observe an intermediate state.

**Completion criterion:** the protocol has no concurrency-relevant transition described only as “eventually” or “somewhere under a lock.”

## Step 3 — prove synchronization edges

For every read that relies on another context's write, name the edge that makes it valid:

- same mutex / critical section order;
- release → acquire relation on the same synchronization object;
- successful CAS/RMW ordering;
- immutable publication before handoff;
- another explicit C++ memory-model relation.

A write to one atomic does not automatically publish a later write to another atomic. Treat distinct atomic objects as distinct modification orders unless an actual happens-before argument connects them.

For condition-variable or park/wake protocols, identify:

- the persistent predicate/state;
- who mutates it;
- the wake publication;
- the baseline/arming point;
- the recheck that closes the lost-wake window.

A wake event is not persistent state. Correctness must survive a notification that happens just before the waiter actually sleeps.

**Completion criterion:** every correctness-relevant read/write or wait/wake dependency has a named synchronization argument.

## Step 4 — prove lifetime and ownership

For each thread/fiber/task/waiter/request involved, state:

- who creates it;
- who owns it while queued/running/waiting;
- what references/borrows it retains;
- when cancellation may race it;
- who performs terminal resolution;
- who may publish it runnable/complete;
- when its storage may be reused or destroyed.

A task capture, waiter node, callback context, or queue entry without a complete lifetime story blocks implementation.

**Completion criterion:** no concurrent observer can access state after its ownership/lifetime has ended, and terminal/publication authority is unique.

## Step 5 — prove liveness and shutdown

For every accepted or registered unit of work, identify all legal states until terminal retirement.

Ask:

- Can accepted work disappear between queues/owners?
- Can every waiter be woken by either persistent state or a future signal?
- Can cancellation and completion both publish terminal state?
- Can shutdown stop progress before queued/active work reaches its documented outcome?
- Can two participants each wait for progress only the other can create?
- Can a counter/generation/local flag become stale relative to the global state it summarizes?

For Sluice, preserve the core law:

```text
accepted -> cannot disappear
```

A watchdog may rescue or diagnose a stuck test; it must not create the correctness path.

**Completion criterion:** every accepted/waiting state has a finite protocol path to its documented terminal/drained state under the stated assumptions.

## Step 6 — implement narrowly

Prefer synchronization that makes the invariant obvious:

- scoped locks;
- one authority for one state transition;
- state and its protecting lock colocated when practical;
- predicates derived from persistent state;
- explicit ownership transfer rather than duplicated mutable truth;
- atomics only where their independent observation is intended and proved.

Do not choose lock-free code merely because a mutex is present or contended.

**Completion criterion:** synchronization structure mirrors the protocol instead of compensating for an undocumented protocol.

## Step 7 — produce causal evidence

For a race/liveness/cancellation repair, evidence should normally progress through:

1. deterministic pre-fix C++ schedule or precise invariant violation;
2. post-fix run of the same causal schedule;
3. focused regression;
4. TSan where supported;
5. applicable repository gates from `AGENTS.md`;
6. formal/model evidence only for the protocol it actually represents.

Prefer stable role/state barriers over arbitrary sleeps or yield counts. Stress tests are useful secondary evidence, not the strongest proof of a concurrency repair.

## Completion record

Before declaring concurrent C++ complete, be able to state:

```text
shared-state map: complete / gaps
protocol/state machine:
synchronization edges:
lifetime/ownership:
wake/progress/shutdown:
deterministic evidence:
TSan / applicable gates:
remaining assumptions:
```

## Detailed reference

`REFERENCE.md` contains the previous comprehensive concurrency handbook, source links, examples, and detailed heuristics. Read it only when a specific design or review question needs deeper reference material.