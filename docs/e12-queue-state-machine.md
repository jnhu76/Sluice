# E12-E Queue — Corrective-2 Authoritative State Machine

> **Current identity:** `E12-E-QUEUE-STATE-MACHINE-DESIGN-CORRECTIVE-2`
>
> **Status:** `PASS — INDEPENDENT ADVERSARIAL REVIEW PASS (B2)`
>
> **Integration authority:**
> [`E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2`](e12-queue-scheduler-integration.md)
>
> **Applied disposition:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1:
> SUPERSEDED — REQUEST-CHANGES`
>
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1-REVIEW:
> REQUEST-CHANGES`
>
> **Implementation status:** `AUTHORIZED — all four prerequisite gates PASS (see AUTHORIZATION-2)`

This document normalizes the abstract Queue machine to the Corrective-2
one-shot lease, unrestricted active-victim ticket stealing, and irreversible
teardown lifecycle. It does not change or claim verification of any TLA+
artifact.

```text
TARGET COVERAGE:
19 canonical transitions
6 publication transitions

VERIFIED COVERAGE — AUTHOR SELF-ASSESSMENT:
19/19 canonical transitions
6/6 publication transitions

Gate status (current):
  B1 Mutex no-throw substrate:               PASS  (independent review complete)
  B2 Corrective-2 independent review:        PASS  (independent adversarial review complete)
  B3 Condition T25 migration/reacquire:      PASS  (W1 corrective db656b5)
  B4 Queue formal model:                     PASS  (independent formal review complete)
```

## 1. Binding state domains

### 1.1 Object lifecycle and close state

Object lifecycle is distinct from Queue close:

```text
ObjectLifecycle = operational | tearing_down
QueueState      = Open | Closed
capacity >= 1
ring = FIFO sequence of unique ItemId
0 <= Len(ring) <= capacity
```

`Closed` is absorbing and prevents later producer commit, while buffered items
remain drainable by consumers. `tearing_down` is also absorbing and prevents
every ordinary QueuePort operation, including close and snapshot. Teardown is
not a canonical Queue operation.

### 1.2 Lease/control location and item owner

Every live ItemId has exactly one `QueueItemControl::Location`:

| Control location | Abstract ItemOwner | Live lease holder |
| --- | --- | --- |
| `detached` | ProducerCaller/public failed result | typed boundary or opaque failed result |
| `producer_operation` | ProducerOperation | producer operation |
| `ring` | Ring | exactly one fixed ring slot |
| `consumer_operation` | ConsumerOperation | consumer operation/opaque pop result |
| `teardown` | TeardownSession | `take_next()` result |
| `released` | ConsumerCaller or Destroyed | no QueueItemLease |

The successful history is:

```text
detached -> producer_operation -> ring -> consumer_operation -> released
```

The failed producer history is:

```text
detached -> producer_operation -> detached -> released
```

The teardown history is:

```text
ring -> teardown -> released
```

Only a non-empty move-only `QueueItemLease` authorizes a transition. Move
empties the source and requires an empty destination. Therefore ring ItemIds
are unique by the production type structure, not by a debug-only assertion.
The same detached control cannot be admitted twice: after first admission the
caller's lease is empty and location is no longer detached.

`owner_port_ == this` is necessary Queue identity validation but is never the
sole ownership proof.

### 1.3 Wait resolution and completion

Queue v1 projects the reusable WaitNode to:

```text
QueueV1WaitResolution = Detached | Registered | Woken | Expired
QueueCompletion = None | Pending | Committed | Closed | Expired
```

Coupling at stable boundaries:

```text
linked Queue waiter       => Registered + Pending
Woken producer/consumer   => Committed or Closed before publication
Expired producer/consumer => Expired before publication
terminal                  => unlinked
Published                 => payload location and completion final
```

External Queue cancellation is deferred. P8/C7 and cancellation publications
are reserved identifiers but are absent from Queue v1.

### 1.4 Publication, membership, timer, and execution

```text
Publication = NotApplicable | NotPublished | Published
Membership  = Detached | ProducerWaiters | ConsumerWaiters
Timer       = None | Prepared | Active | Retired | Consumed
Mode        = Idle | Reconciling | CommitGap
```

Prepared is pump-invisible, heap-unlinked, uncounted, and has no target.
Active implies a live linked registration and operation. Winner commit and its
matching publication are two conceptual sub-transitions in one lock-held
region; user code cannot observe CommitGap.

## 2. Admission rules

Producer fast commit requires:

```text
ObjectLifecycle == operational
QueueState == Open
Len(ring) < capacity
no older eligible producer
non-empty lease
control.owner_port == this
control.location == detached
```

Consumer fast commit requires:

```text
ObjectLifecycle == operational
Len(ring) > 0
no older eligible consumer
```

Otherwise `try_*` returns would-block, while blocking/timed operations append
to the appropriate FIFO. A closed producer receives its exact detached lease.
A consumer receives Closed only at Closed+empty. No direct producer-to-
consumer transfer exists.

Timed admission prepares all fallible timer resources before WaitNode
registration and retains G through immediate discard or activation. State/
resource outcomes precede an already-due deadline: push checks Closed then
immediate no-barging capacity; pop checks immediate item then Closed+empty.
Only an otherwise-blocking already-due operation expires inline.

Ordinary admission requires lifecycle operational and enters
`active_port_calls_`. Teardown has no ordinary CallGuard.

## 3. Canonical transition inventory

The binding target is:

```text
Producer: P1 P2 P3 P4 P5 P6 P7 P9 P10       (9)
Consumer: C1 C2 C3 C4 C5 C6 C8 C9           (8)
Close:    CL1 CL2                            (2)
                                               --
                                               19
```

### 3.1 Producer transitions

| ID | Name | Guard/authority | Lease/control transition | Result/publication |
| --- | --- | --- | --- | --- |
| P1 | PrepareProducerItem | typed QueuePort factory, outside locks | create typed node/control at detached; mint one lease | allocation may throw before port admission |
| P2 | FastPushCommit | Open+space+no older producer | detached->producer_operation->ring; source operation empty | immediate committed; reconcile |
| P3 | PushClosed | Closed | detached->producer_operation->detached | opaque failed result owns exact lease |
| P4 | TryPushWouldBlock | full or older producer | detached->producer_operation->detached | opaque failed result owns exact lease |
| P5 | ProducerWaitAdmission | blocking/timed | detached->producer_operation; link Registered+Pending; Prepared->Active iff timed | suspend, not published |
| P6 | ProducerGrantCommit | reconciler, Open+space+FIFO head | producer_operation->ring; operation emptied | PUB-P-COMM |
| P7 | ProducerClosedCommit | reconciler, Closed+FIFO head | producer_operation retained until detached return | PUB-P-CLOSED |
| P9 | ProducerExpire | already-due inline or ACTIVE timer winner | lease remains detached/producer_operation; never enters ring | inline result or PUB-P-EXPIRE |
| P10 | ProducerReturn | inline/resumed QueuePort then typed layer | failed producer_operation->detached->released; committed stays empty | exact public result; typed deletion outside locks |

### 3.2 Consumer transitions

| ID | Name | Guard/authority | Lease/control transition | Result/publication |
| --- | --- | --- | --- | --- |
| C1 | FastPopCommit | ring nonempty+no older consumer | ring->consumer_operation; source ring slot empty | immediate item; reconcile |
| C2 | PopClosedEmpty | Closed+empty | none | immediate closed |
| C3 | TryPopWouldBlock | empty or older consumer | ring unchanged | immediate would_block |
| C4 | ConsumerWaitAdmission | blocking/timed | consumer empty; link Registered+Pending; Prepared->Active iff timed | suspend, not published |
| C5 | ConsumerGrantCommit | ring nonempty+FIFO head | ring->consumer_operation; source slot empty | PUB-C-COMM |
| C6 | ConsumerClosedCommit | Closed+empty+FIFO head | none | PUB-C-CLOSED |
| C8 | ConsumerExpire | already-due inline or ACTIVE timer winner | ring unchanged; consumer empty | inline result or PUB-C-EXPIRE |
| C9 | ConsumerReturn | inline/resumed QueuePort then typed layer | consumer_operation->released | exact public result; typed deletion outside locks |

### 3.3 Close transitions

| ID | Name | Guard | State change | Return condition |
| --- | --- | --- | --- | --- |
| CL1 | CloseLinearize | lifecycle operational, Queue Open | Open->Closed | after closed reconciliation fixed point |
| CL2 | IdempotentClose | lifecycle operational, Queue Closed | Closed->Closed | after re-reconciliation fixed point |

Close rejects producers, drains buffered items to eligible consumers, and
completes consumers Closed only once the ring is empty. Close cannot run after
teardown begins.

## 4. Publication sub-transitions

| ID | Required predecessor | Binding publication state |
| --- | --- | --- |
| PUB-P-COMM | P6, Woken/unlinked, ring owns lease, committed | make runnable; append ticket; Published |
| PUB-P-CLOSED | P7, Woken/unlinked, operation retains lease, closed | make runnable; append ticket; Published |
| PUB-P-EXPIRE | registered P9, Expired/unlinked, operation retains lease | make runnable; append ticket; Published |
| PUB-C-COMM | C5, Woken/unlinked, consumer owns lease, committed | make runnable; append ticket; Published |
| PUB-C-CLOSED | C6, Woken/unlinked, no lease, closed | make runnable; append ticket; Published |
| PUB-C-EXPIRE | registered C8, Expired/unlinked, no lease | make runnable; append ticket; Published |

Publication uses the stable mapped-value address captured at admission and does
not choose a Worker or touch the owner map.

Worker consumption under G applies:

```text
if current Worker has Queue tickets:
    choose its oldest ticket
else:
    choose global oldest Queue ticket
    update that ticket's existing owner mapped value to current
```

Admission-owner activity never changes eligibility. An active victim's Queue
ticket can be claimed. Ticket claim/unlink and owner-slot update precede
`run_next_on`/`make_running`.

Reserved, absent, and uncounted:

```text
P8 ProducerCancel
C7 ConsumerCancel
PUB-P-CANCEL
PUB-C-CANCEL
```

## 5. Reconciliation state machine

With G+S held, acquire at most one role queue and perform the first applicable
transition:

```text
Closed + producer head:
    P7 then PUB-P-CLOSED

ring nonempty + consumer head:
    C5 then PUB-C-COMM

Closed + ring empty + consumer head:
    C6 then PUB-C-CLOSED

Open + ring has space + producer head:
    P6 then PUB-P-COMM

otherwise:
    fixed point
```

A linked terminal head is an invariant failure, not a skip/repair candidate.
The unique winner unlinks. G+S remain held through the fixed point, preventing
newcomer barging.

## 6. Termination and CommitGap

Let:

```text
R = 2 * linked_wait_epochs + unpublished_completed_epochs
```

Winner/unlink changes `(-1 linked,+1 unpublished)`, reducing R by one;
publication changes `(-1 unpublished)`, reducing it once more. Each lock-held
iteration performs both for one finite epoch. Ring transfers cannot duplicate
ItemIds because their source lease is emptied before publication.

CommitGap allows no external Queue mutation, user code, result revocation, or
normal successor other than the matching publication.

## 7. Binding invariants

### 7.1 Ring, lifecycle, and close

```text
0 <= Len(ring) <= capacity
ring ItemIds are unique
Closed is absorbing
Closed => no later producer commit
tearing_down is absorbing
tearing_down => no ordinary QueuePort operation
consumer Closed completion => ring empty at that completion
no direct producer-to-consumer transfer
```

### 7.2 Ownership

```text
one live ItemId => one non-released control location
one live control => exactly one non-empty lease
ring location <=> exactly one ring slot owns the lease
failed producer receives the original lease, never an alias
committed push result has no lease
pop empties its source ring slot
typed deletion occurs only after location becomes released and locks are absent
```

### 7.3 Wait, completion, publication

```text
linked <=> Registered+Pending at stable boundaries
terminal => unlinked
Woken => completion in {Committed, Closed}
Expired => completion == Expired
Published => completion and payload location final
one resolve winner => one unlink => at most one publication
```

### 7.4 Owner slot and stealing

```text
fiber_owner_ entry exists before Queue registration
mapped-value address stable through ticket removal, Fiber resume,
    and operation owner-slot release
no erase of that element during the interval
all mapped-value access under G
steal writes existing mapped value only
publication does not access owner map
active admission owner does not block steal
```

### 7.5 Timer and lifetime

```text
Prepared has no target, heap membership, pump visibility, or active count
Active => live core/operation/registration and linked waiter
Prepared guard is destroyed before G guard
ticket removed before operation resume/destruction
Scheduler outlives Queue and all epochs
```

### 7.6 FIFO and no barging

```text
only oldest eligible role waiter may grant
new producer cannot fast-commit over an older producer
new consumer cannot fast-pop over an older consumer
worker ticket selection is own-oldest, otherwise global-oldest
producer and consumer WaitQueue mutexes are never held together
```

## 8. Counter refinement

| State event | `active_port_calls_` | wait associations | active Queue timers | granted-not-resumed |
| --- | ---: | ---: | ---: | ---: |
| ordinary QueuePort call entered | +1 | 0 | 0 | 0 |
| P5/C4 registration | unchanged | +1 | +1 iff timed | 0 |
| P6/P7/P9/C5/C6/C8 winner unlink | unchanged | -1 | -1 iff Active | 0 |
| matching suspended publication | unchanged | 0 | 0 | +1 |
| suspended P10/C9 ticket removal/resume return | -1 | 0 | 0 | -1 |
| fast/inline P10/C9 return | -1 | 0 | 0 | 0 |
| preparation exception | -1 by CallGuard | 0 | 0 | 0 |

`active_port_calls_` counts only time inside the non-template QueuePort. The
four zeros do not prove typed conversion finished, arbitrary callers are gone,
or concurrent destruction is safe.

## 9. Teardown lifecycle

Teardown is outside §3:

```text
operational
+ zero active_port_calls_
+ zero wait associations
+ zero ACTIVE Queue timers
+ zero granted-not-resumed
+ both WaitQueues empty
-> begin_teardown under G+S
-> tearing_down + one move-only QueueTeardownSession
-> repeated ring -> teardown -> released
-> empty ring + completed session
-> QueueCore destruction
```

The session is unforgeable, non-copyable, and unique. `take_next()` does not
enter CallGuard. Typed destruction occurs after the port returns and no locks
are held. A second teardown, snapshot, close, admission, waiter, timer, or
ticket is impossible after the lifecycle transition.

## 10. Public outcome projection

```text
push:       committed | closed(T)
push_until: committed | closed(T) | expired(T)
try_push:   committed | closed(T) | would_block(T)

pop:        item(T) | closed
pop_until:  item(T) | closed | expired
try_pop:    item(T) | closed | would_block
```

Every failed push returns the exact typed payload by moving the complete
opaque lease through the typed boundary. There is no borrowed reference or raw
pointer recovery. The public failed factory takes `T&&`, so node-to-result
conversion performs one T move.

## 11. Formal-model status

The Queue TLA+ formal model is authored (B4 PASS): Model A (bounded MPMC
FIFO, 12 invariants) + Model B (Open/Closed, 7 invariants) + 7 negative
models under `docs/spec/e12_queue/`; gate `scripts/verify-e12-queue-formal.sh`
(exit 0); independent formal review PASS
(`docs/reviews/E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-2.md`). The model
normalizes the abstract machine to the one-shot lease/control location,
selected-waiter grant, no-barging, winner-before-publication, and close
monotonicity. The `tearing_down` lifecycle axis and E11 timer/expiry are
explicitly out of B4 scope (deferred to a future teardown / E11×E12 Model C).

```text
FORMAL/TLA STATUS: PASS — independent formal review complete (B4)
FORMAL PASS: CLAIMED (safety-only; Model A + Model B + 7 negatives)
```

Any model text that treats external cancellation, explicit Permit/reservation,
active-owner steal prohibition, or reusable item identity as Queue-v1
authority is historical only.

## 12. Superseded authority and current verdict

Superseded/non-binding claims include:

```text
Corrective-1 PASS
reusable QueueItemBase& admission
owner_port identity as complete ownership proof
active original owner blocks steal
ordinary take_quiescent_item mutation
active_calls_ as broad typed-call proof
21 canonical / 8 publication transitions
external Queue cancellation in v1
explicit slot/item Permit owner
direct handoff
terminal-head retry/skip
```

The integration row recount and scratch probes are recorded in the current
Corrective-2 authority. Formal status remains explicitly separate.

```text
E12-E-QUEUE-STATE-MACHINE-DESIGN-CORRECTIVE-2:
PASS — AUTHOR SELF-ASSESSMENT

TARGET COVERAGE:
19 canonical transitions
6 publication transitions

VERIFIED COVERAGE — AUTHOR SELF-ASSESSMENT:
19/19 canonical transitions
6/6 publication transitions

AUTHOR SELF-ASSESSMENT
INDEPENDENT ADVERSARIAL REVIEW PASS (B2)

E12-E IMPLEMENTATION AUTHORIZATION:
all four prerequisite gates PASS — see AUTHORIZATION-2
```
