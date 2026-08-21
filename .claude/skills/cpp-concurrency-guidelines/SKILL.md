---
name: cpp-concurrency-guidelines
description: C++ shared-state correctness workflow for Sluice. Use when changing threads/fibers, locks, atomics, concurrent queues, publication, wake/park, cancellation, shutdown, or any mutable state observed by multiple execution contexts.
---

# C++ concurrency correctness

Use this skill to make the actual C++ protocol explicit before changing synchronization. It complements `cpp-coding-standards`; repository contracts and `AGENTS.md` remain authoritative.

If the task also asks to measure or optimize performance, load `cpp-concurrency-performance` after the correctness model is explicit.

If correctness depends on CAS loops, cross-operation memory ordering, linearization, ABA, reclamation, or a non-blocking progress property, also load `cpp-lock-free`.

## Step 1 — build the shared-state map

For every mutable object touched by more than one execution context, record:

| State | Owner | Readers | Writers | Protection / publication |
|---|---|---|---|---|
| ... | ... | ... | ... | ... |

Include state indirectly shared through callbacks, fibers/tasks, backend completions, queues, registries, timers, or shutdown paths.

Prefer removing sharing, transferring ownership, partitioning state, or publishing immutable state before adding synchronization.

**Completion criterion:** every mutable shared-state item in the changed protocol has one named ownership/protection story.

## Step 2 — recover the as-built C++ protocol

Recover current C++ behavior before using a model, comment, or desired design as the protocol description. Write the smallest state machine that explains the changed behavior. For each transition, identify:

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

Pay special attention to transitions involving enqueue/dequeue, waiter registration/removal, grant/ownership transfer, terminal resolution, cancellation/expiration, wake-before-suspend / park commit, shutdown/drain, worker exit, or ownership migration.

Keep separate every C++ step whose intermediate state another thread/fiber can observe.

**Completion criterion:** every concurrency-relevant transition maps to concrete C++ operations and no observable intermediate state is hidden behind “eventually”, “atomically” without proof, or “somewhere under a lock”.

## Step 3 — prove synchronization edges

For every read that relies on another context's write, name the edge that makes it valid:

- same mutex / critical-section order;
- release → acquire relation on the same synchronization object;
- successful CAS/RMW ordering;
- immutable publication before handoff;
- another explicit C++ memory-model relation.

Distinct atomic objects have distinct modification orders unless an actual happens-before argument connects the observations. Observing one atomic does not automatically reveal a later operation on another atomic.

For condition-variable or park/wake protocols, identify:

- the persistent predicate/state;
- who mutates it;
- the wake publication;
- the baseline/arming point;
- the recheck that closes the lost-wake window.

A notification is not persistent state. Correctness must survive a signal that occurs immediately before the waiter sleeps.

**Completion criterion:** every correctness-relevant read/write or wait/wake dependency has a named synchronization argument.

## Step 4 — prove lifetime and authority

For each thread/fiber/task/waiter/request involved, state:

- who creates it;
- who owns it while queued/running/waiting;
- what references/borrows it retains;
- when cancellation may race it;
- who performs terminal resolution;
- who may publish it runnable/complete;
- when its storage may be reused or destroyed.

A task capture, waiter node, callback context, queue entry, or routing token without a complete lifetime story blocks a correctness claim.

**Completion criterion:** no concurrent observer can access state after its lifetime ends, and each terminal/publication transition has one authority.

## Step 5 — close liveness and shutdown

For each accepted or waiting state, identify:

```text
progress obligation:
state/event that can enable progress:
who can produce it:
how the waiter observes it:
fairness / scheduler / backend / external-event assumptions:
shutdown interaction:
```

Then look for a reachable closed state or cycle with unfinished work in which no participant allowed by those assumptions can create the required progress or future wake.

The existence of one successful path is not a liveness proof. Conversely, a protocol that intentionally waits for a documented external event need not terminate unconditionally; the external-event assumption must be explicit.

For Sluice accepted work, preserve the repository's accepted-work progress contract. A watchdog may rescue or diagnose a stuck test, but it must not create the only correctness path.

**Completion criterion:** under the stated assumptions, no reachable closed stuck state/cycle strands work whose contract requires progress; any remaining fairness or external-event assumption is explicit rather than hidden.

## Step 6 — implement narrowly

Prefer synchronization that makes the recovered protocol obvious:

- scoped locks;
- one authority for one state transition;
- state and its protecting lock colocated when practical;
- predicates derived from persistent state;
- explicit ownership transfer rather than duplicated mutable truth;
- atomics only where independent observation is intended and proved.

A hot mutex is a performance observation, not permission to replace the protocol with lock-free code.

**Completion criterion:** synchronization structure mirrors the stated protocol instead of compensating for undocumented state.

## Step 7 — produce causal evidence

For a race/liveness/cancellation repair, evidence should normally progress through:

1. deterministic pre-fix C++ schedule or precise invariant violation;
2. post-fix run of the same causal schedule;
3. focused regression;
4. TSan where supported;
5. applicable repository gates from `AGENTS.md`;
6. formal/model evidence only for the protocol it actually represents.

Prefer stable role/state barriers over arbitrary sleeps or yield counts. Stress tests are secondary evidence, not the strongest proof of a concurrency repair.

## Completion record

Before declaring concurrent C++ complete, be able to state:

```text
shared-state map:
as-built protocol/state machine:
synchronization edges:
lifetime/terminal/publication authority:
progress assumptions and stuck-cycle analysis:
deterministic evidence:
TSan / applicable gates:
remaining assumptions:
```

## Detailed reference

`REFERENCE.md` preserves the previous long-form concurrency handbook. Search for the exact topic first and read only the smallest relevant section; do not load the whole handbook by default. Current routing and completion criteria live here.
