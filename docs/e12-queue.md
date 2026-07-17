# E12-E Queue — Corrective-2 Semantic Authority and Historical Audit

> **Current authority:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2: PASS`
>
> **Status:** `PASS — AUTHOR SELF-ASSESSMENT — INDEPENDENT REVIEW REQUIRED`
>
> **Applied disposition:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1:
> SUPERSEDED — REQUEST-CHANGES`
>
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1-REVIEW:
> REQUEST-CHANGES`
>
> **State-machine authority:**
> `E12-E-QUEUE-STATE-MACHINE-DESIGN-CORRECTIVE-2:
> PASS — AUTHOR SELF-ASSESSMENT`
>
> **Dependent substrate:**
> `ASYNC-MUTEX-NOTHROW-AUTHORITY-1:
> DESIGN PASS — PRODUCTION IMPLEMENTED — INDEPENDENT REVIEW PASS (B1)`
>
> **Implementation authorization:** `DENIED — B2/B4 OPEN`

Corrective-2 retains the Queue-v1 semantics but replaces the rejected reusable
item reference, active-owner steal veto, ordinary quiet drain, broad call
counter, and abstract PREPARED timer guard.

## Current binding semantic decisions

### Queue v1 scope

| Dimension | Binding value |
| --- | --- |
| public type | `AsyncQueue<T>`; copy/move deleted |
| topology | bounded MPMC, runtime fixed capacity >= 1 |
| ordering | FIFO lease ring; FIFO producer/consumer waiters; no barging |
| transfer | detached -> producer operation -> ring -> consumer operation -> released; no direct handoff |
| admission authority | one-shot move-only unforgeable `QueueItemLease`, consumed by value |
| failed push | complete original opaque lease returned, then exact typed payload recovered |
| close | monotonic/idempotent; buffered items drain; later producers fail Closed |
| expiry | distinct pre-grant winner; external cancellation remains deferred |
| timed admission | concrete list-iterator PREPARED guard; ACTIVE/RETIRED/CONSUMED afterward |
| runnable selection | current Worker own-oldest, otherwise global-oldest; active victims are stealable |
| owner slot | mapped-value address captured before registration; no erase until ticket removal, resume, and operation release |
| teardown | irreversible unique `QueueTeardownSession`; ordinary drain API removed |
| call counter | `active_port_calls_` covers only ordinary non-template QueuePort interval |
| Scheduler | borrowed `Scheduler&`; Scheduler outlives Queue and all epochs |

### Public outcomes and exact failed ownership

```text
push:       committed | closed(T)
push_until: committed | closed(T) | expired(T)
try_push:   committed | closed(T) | would_block(T)

pop:        item(T) | closed
pop_until:  item(T) | closed | expired
try_pop:    item(T) | closed | would_block
```

Queue v1 has no cancelled outcome. The non-template push seam obeys:

```text
committed => opaque result lease empty
closed/expired/would_block => opaque result owns exactly one original lease
```

Typed failure conversion moves out that complete lease, validates port and
type, changes the control to released, moves `T` once through a `T&&` failed
factory, and destroys the exact typed node outside locks. It never reconstructs
authority from a raw pointer or borrowed reference.

### Element and ring constraints

```cpp
std::is_object_v<T>
std::is_nothrow_move_constructible_v<T>
std::is_nothrow_destructible_v<T>
```

Every control has exactly one location: detached, producer operation, ring,
consumer operation, teardown, or released. Every non-empty ring slot owns a
move-only lease; transfer empties its source and requires an empty destination.
This makes ring ItemIds unique in production structure. QueueCore never moves,
destroys, or invokes `T` under Queue/Scheduler locks.

### Architecture and lifetime boundary

`detail::QueuePort` is the fixed non-template Scheduler friend. Downstream code
cannot construct a lease from a control pointer, mutate location, construct a
teardown session, call reconciliation, forge a ticket, or mutate the owner map.
The complete type graph, timer guard, call ledger, 19/6 rows, and 33
counterexamples are binding in
[`docs/e12-queue-scheduler-integration.md`](e12-queue-scheduler-integration.md).

`begin_teardown()` requires zero ordinary port calls, linked waits, ACTIVE
Queue timers, and granted-not-resumed operations, plus empty WaitQueues. It
irreversibly changes lifecycle to tearing-down. The unique session drains ring
leases and the typed layer destroys nodes outside locks. These counters do not
prove arbitrary callers have disappeared; concurrent destruction is still a
caller contract violation.

### Transition target and evidence status

```text
TARGET COVERAGE:
19 canonical transitions
6 publication transitions

VERIFIED COVERAGE — AUTHOR SELF-ASSESSMENT:
19/19 canonical transitions
6/6 publication transitions

P8/C7: RESERVED — DEFERRED — NOT IN QUEUE V1
PUB-P-CANCEL/PUB-C-CANCEL: RESERVED — DEFERRED
```

Expiry remains P9/C8 and return remains P10/C9; numbering is unchanged.

### Deferred alternatives

```text
capacity zero/rendezvous
unbounded Queue
direct handoff
overflow drop/conflation
Select/multi-wait
close with cause
external cancellation registration
explicit slot reservation / Permit
Kotlin CQS/segmented channel algorithm
```

For Queue v1, explicit item/slot reservation is superseded. The fixed ring uses
atomic move-only lease ownership transfer. Permit remains a Queue-v2 candidate
only.

### Formal and Condition baseline

Corrective-2 modifies no TLA+ artifact. Formal status is not updated and no
formal PASS is claimed.

```text
E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1:
PASS — closed by W1 corrective (db656b5)

Condition build: PASS
Condition runtime suite: PASS (Clang Debug/ASan/TSan full suite green)
T25 migration/reacquire: PASS (deterministic rewrite)
```

The T25 hang was root-caused to a test-harness defect (unbounded coordinator
waits, missing `f_idle`/`bounded_wait`/suspension handshake) and closed by
mirroring the Mutex T19 determinism discipline; no production code was
touched. Evidence: `docs/async-runtime-hang-and-gcc-corrective.md` §B.1/§C/§E.1.

### Authorization gates

Before production implementation:

1. ~~the accepted Mutex substrate design still needs separate implementation
   authorization and realization~~ — **B1 PASS**: production fail-fast Mutex
   landed (`be07564`) with death tests (`e2cfe61`), independent production
   implementation review PASS
   (`docs/reviews/ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-REVIEW.md`,
   commit `15dc9b4`);
2. Corrective-2 needs a fresh independent adversarial review — **B2 OPEN**;
3. ~~Condition T25 needs its separate hang audit~~ — **B3 PASS** (W1
   corrective `db656b5`);
4. later formal normalization must preserve the one-shot lease and corrected
   steal/teardown semantics — **B4 OPEN** (no Queue TLA+ model yet).

```text
E12-E IMPLEMENTATION AUTHORIZATION: DENIED — B2/B4 OPEN
```

---

# NON-BINDING HISTORICAL ANALYSIS

Everything below this marker is retained as the original preparation and
semantic-decision record. It is non-binding even where an old heading says
`PASS`, `authoritative`, `CLOSED`, `HUMAN-DECISION-REQUIRED`, or
`implementation ready`. In particular, old reusable-item, active-owner veto,
quiet-drain, broad-counter, cancellation, 21/8 transition, template-friend,
explicit-reservation, terminal-skip, direct-handoff, and unspecified-result
statements are superseded by the current authority above.

## Historical E12-E-QUEUE-SEMANTIC-DECISION-1

> **Decision identity:** `E12-E-QUEUE-SEMANTIC-DECISION-1`
> **Status:** `PASS`
> **Implementation remains unauthorized.** See §M.

### Decision sources

The following hierarchy was used (highest authority first):

1. **Repository evidence** — Sluice E10/E11/E12 scheduler, waiter, timer,
   cancellation, locking, testing-seam, and lifecycle authorities.
2. **Preparation audit** — `docs/e12-queue.md` §A–§O (the discovery record).
3. **Cross-primitive plan** — `docs/e12-sync-primitives-plan.md` §8.
4. **External research evidence** — CQS, Kotlin Channels, Kotlin Channel
   close/buffering/receive/send/cancellation semantics. Adapted to Sluice
   authorities; not copied as lock-free implementation.

### Binding decisions

#### Queue shape

| Decision | Value | Basis |
| --- | --- | --- |
| Type name | `AsyncQueue<T>` | Consistent with `AsyncMutex`/`AsyncCondition` naming; avoids collision with `WaitQueue` |
| Concurrency topology | MPMC | The existing `WaitQueue` substrate supports multiple producers and consumers; no restriction is necessary |
| Capacity | runtime fixed, `capacity >= 1` | Matches Semaphore `max_permits_` runtime pattern; compile-time capacity is a future option |
| Buffer order | FIFO | Standard queue semantics; matches `WaitQueue` FIFO discipline |
| Producer waiter order | FIFO eligible-waiter grant | `WaitQueue` FIFO + no-barging (Semaphore A2 precedent) |
| Consumer waiter order | FIFO eligible-waiter grant | Same |
| Backpressure | producer suspends when full | Bounded queue requirement; plan §8.1 |
| Deferred | rendezvous (capacity-0), unbounded, direct handoff, overflow drop, conflation, Select, close-with-cause | See deferred list below |

#### Success protocol

```text
RESULT-BEARING SELECTED-WAITER GRANT
```

A successful suspended operation resumes only after its concrete item or
committed producer payload has been bound to that exact wait epoch. A `woken`
scheduler outcome alone is not sufficient to mean Queue success. Queue
operation completion is recorded in Queue-specific state (separate from
`WaitOutcome`).

#### Internal pre-grant retry

```text
INTERNAL PRE-GRANT RETRY / SKIP
```

Cancelled, expired, terminal, or otherwise ineligible candidate waiters are
skipped internally before grant. Internal retry does not publish Queue
success, does not consume/duplicate an item, does not consume/leak capacity,
and does not permit a newly arriving operation to barge over an older eligible
queued waiter. The resource remains in primitive ownership throughout.

#### Payload path

```text
producer → authoritative bounded buffer → consumer
```

Direct producer-to-consumer payload bypass is not part of the first version.
Even when a consumer is already waiting, the payload is semantically committed
to the authoritative FIFO buffer before the oldest eligible consumer receives
the buffer head. An implementation may later optimize this sequence only if it
proves observational equivalence.

#### Stable payload ownership

```text
ItemNode<T> owns one constructed T
Queue buffer = fixed-capacity ring of owning ItemNode handles
```

`T` construction occurs before acquiring Queue or Scheduler locks. Queue
capacity storage is allocated during Queue construction. Grant/commit critical
sections perform only non-throwing handle, index, waiter-state, and ownership
operations. No user-defined `T` operation runs under `Scheduler::global_mtx_`
or under any Queue internal structural lock. After a consumer is granted an
ItemNode, `T` is moved out and the node is destroyed after all Queue and
Scheduler locks have been released.

#### Type requirements

```cpp
std::is_object_v<T>
std::is_nothrow_move_constructible_v<T>
std::is_nothrow_destructible_v<T>
```

`emplace` is deferred to a separate exception/constructor protocol.

#### Separate scheduler outcome from Queue completion

`WaitOutcome` decides which scheduler wait cause won. Queue operation
completion decides whether a payload/slot commit occurred and what the
operation returns. These are separate. The primitive must inspect Queue
completion state; it must never infer success from generic `woken` alone.

#### Cancellation and deadline contract

Sluice's existing final-winner semantics apply:

- **Case A** (cancel/expire wins `resolve_` before grant): the waiter becomes
  terminal; the grant path must not commit an item or producer payload to it;
  the resource remains with its prior owner; a grant operation skips/retries
  another candidate.
- **Case B** (grant `resolve_(woken)` wins): the Queue commit is completed
  before runnable publication; later cancel/expire attempts are losers; wait
  cancellation cannot revoke the committed grant.
- **Post-resume abandonment**: once grant commits, the operation owns its
  result. Abandonment after resume is outside E12-E v1.
- Prompt post-commit cancellation (Kotlin Channel style) is **explicitly
  rejected** for E12-E v1: it conflicts with Sluice's single-CAS final-winner
  authority and would require a separate undelivered-element protocol.

#### Push failure retains payload

A push that does not commit must return ownership of the original payload to
the caller. No silent destruction of a producer payload merely because a
blocked push was closed, cancelled, or expired.

#### Close contract

```text
idempotent, monotonic, drain-on-close, no reopen, no close cause in v1
```

Close linearization point: `closed: false → true` under the Queue structural
state authority. After this point: no new producer may commit; new push
operations return `closed` with the undelivered payload; blocked producers
complete with a `closed` outcome and their undelivered payload; already-committed
buffered items remain FIFO-consumable; waiting consumers receive available
buffered items in eligible FIFO order; once the buffer is empty, remaining
waiting consumers complete with a `closed` outcome; new pop operations continue
to receive buffered items; closed-and-empty pop returns `closed`; close never
discards buffered items; close never reopens.

#### Close-vs-producer commit race

Close and producer commit are serialized by G+S. The authoritative lock order
is: global_mtx_ (G) then state_mtx_ (S). Close acquires G+S and sets
`closed = true` before releasing. Producer commit (P6) acquires G+S and
transfers the ItemNode into the ring before releasing. Since both operations
hold G+S for their entire critical section, exactly one of the following holds:

```text
close linearizes before producer commit:
    producer's G+S acquisition observes closed == true
    producer returns closed with undelivered payload
    no ItemNode enters the ring

producer commit linearizes before close:
    producer's G+S acquisition observes closed == false
    ItemNode enters the ring
    close observes the ring entry and drains it
```

There is no interleaving window: the G+S critical section is atomic with
respect to both the closed-state check and the ring transfer. A producer
selected before close but not yet in G+S will observe closed on entry and
fail. A producer already in G+S will complete its commit before close can
acquire G. This is the same serialization that prevents concurrent admits
from violating ring uniqueness.

#### Fairness and no-barging

Guaranteed:
- FIFO order of buffered elements
- FIFO grant order among eligible queued producers
- FIFO grant order among eligible queued consumers
- no barging over an older eligible waiter

Not guaranteed:
- FIFO scheduler execution, FIFO user-code resumption, FIFO operation return,
  global starvation freedom

Fast paths must not bypass an eligible queued waiter.

#### Destructor contract

Quiet-state caller contract: no concurrent Queue operation; no registered
producer waiter; no registered consumer waiter; no granted-but-not-resumed
operation that references Queue state. The destructor does not implicitly call
close, does not wake or cancel waiters, and debug-asserts the quiet-state
preconditions. A non-empty but quiescent buffer is permitted at destruction.

#### Rebalancing/drain obligation

A conceptual non-throwing Queue reconciliation operation, while holding the
authoritative locks, repeatedly performs all immediately possible commits:
grant buffered FIFO items to eligible consumer waiters; after item removal
creates capacity, commit oldest eligible producer payloads into the buffer;
newly committed producer payloads may then satisfy eligible consumers; skip
terminal/CAS-losing candidates internally; stop when no further immediate
commit is possible.

#### Producer commit protocol

```text
1. Under authoritative lock order, inspect FIFO producer wait queue.
2. Skip/unlink any terminal or CAS-losing candidate.
3. Select the oldest eligible producer.
4. Win that wait epoch via resolve_(woken).
5. Move its ItemNode handle into the authoritative buffer.
6. Mark the producer Queue completion as committed.
7. Publish the waiter runnable.
```

Steps 4–7 are one winner-before-publication commit region. No user-defined
`T` operation and no potentially-throwing allocation occurs in this region.

Push linearization point: the ItemNode ownership transfer into the
authoritative buffer (before runnable publication for blocked pushes).

#### Consumer commit protocol

```text
1. Under authoritative lock order, inspect FIFO consumer wait queue.
2. Skip/unlink any terminal or CAS-losing candidate.
3. Select the oldest eligible consumer.
4. Win that wait epoch via resolve_(woken).
5. Remove the FIFO head ItemNode from the authoritative buffer.
6. Bind that ItemNode to the selected consumer operation state.
7. Mark the consumer Queue completion as committed.
8. Publish the waiter runnable.
```

Steps 4–8 are one winner-before-publication commit region. After resume, the
consumer moves `T` out of the node outside all internal locks.

Pop linearization point: the FIFO ItemNode ownership transfer from the buffer
to the selected consumer operation (before runnable publication for blocked
pops).

#### Candidate linearization points (closed)

| Operation | Linearization point |
| --- | --- |
| Fast successful push | ItemNode enters authoritative buffer |
| Blocked successful push | ItemNode enters buffer before runnable publication |
| Fast successful pop | FIFO ItemNode leaves buffer, becomes owned by consumer operation |
| Blocked successful pop | ItemNode becomes owned by consumer operation before runnable publication |
| Failed push due to close | Observes already-linearized closed state; retains payload |
| Failed pop due to closed-and-empty | Observes `closed && buffer.empty` under structural state authority |
| Cancelled/expired wait | Corresponding `resolve_` CAS wins before any Queue grant commit |
| Close | `closed` changes `false → true` |

#### Formal-model stages

| Model | Scope | Status |
| --- | --- | --- |
| A | Bounded MPMC FIFO buffer + producer/consumer selected-waiter grant; no close, timeout, or cancellation | Required for v1 |
| B | Add close, drain-on-close, producer rejection, closed-and-empty consumers | Required for v1 |
| C | Add timeout and cancellation as pre-grant competing winners; internal retry over terminal candidates | Required for v1 |
| D | Direct-handoff optimization | Deferred |

#### Deferred from E12-E v1

```text
Direct payload handoff (producer→consumer bypass)
Rendezvous (capacity-0)
Unbounded queue
Overflow drop policies
Conflation
Select integration (deferred to E13)
Close with cause
emplace (deferred to separate exception/constructor protocol)
Post-resume abandonment protocol
```

### Evidence-to-decision matrix

| External source | Evidence adopted | Evidence adapted | Evidence explicitly not copied |
| --- | --- | --- | --- |
| CQS | Single-winner grant authority; internal retry over failed candidates; separation of scheduler outcome from primitive completion | Adapted to Sluice's serialized `global_mtx_` + `resolve_` CAS model; no `REFUSE` state unless implementation evidence requires it | Lock-free infinite-array/FAA implementation; CQS `REFUSE` as a literal state |
| Kotlin Channels | Close semantics (drain-on-close, idempotent, monotonic); FIFO buffer + grant; payload retention on close/cancel | Adapted to Sluice's existing deadline/cancellation final-winner semantics | Prompt post-commit cancellation (rejected v1); Kotlin's undelivered-element callback protocol |
| Kotlin Channel close | `close` as monotonic lifecycle terminal; buffered items drain after close; producers rejected after close | Adapted to the `closed` Queue completion state (separate from `WaitOutcome`) | Close-with-cause (deferred) |

### Remaining unresolved questions

Only API spelling and concrete private type/function names may remain open.
No payload-ownership, close, cancellation, fairness, transfer, or
linearization question remains unresolved.

---

## A. Verdict

(Historical audit follows.)

```text
E12-E-QUEUE-PREPARATION-AUDIT-1: READY-FOR-SPEC
```

> **Corrective note (E12-E-QUEUE-PREPARATION-AUDIT-CORRECTIVE-1).** The
> original draft returned `PARTIAL` on the grounds that several semantic
> decisions were unresolved. That conflated two distinct gates. The original
> preparation-audit task defines the verdicts strictly in terms of
> **repository discovery completeness**:
>
> * `READY-FOR-SPEC` — "repository discovery is complete enough to begin
>   writing an authoritative semantic/API specification" (explicitly *not*
>   implementation authorization);
> * `BLOCKED` — "critical repository evidence is missing";
> * `PARTIAL` — (implied) discovery incomplete.
>
> No authoritative repository rule states that a primitive with unresolved
> `HUMAN DECISION REQUIRED` items must receive `PARTIAL` rather than
> `READY-FOR-SPEC`. The plan's per-primitive `HUMAN DECISION REQUIRED` label
> (`docs/e12-sync-primitives-plan.md` §8.2, §12) is the *primitive's* cross-
> primitive status, not an audit-verdict authority, and it does not override
> the task's explicit `READY-FOR-SPEC` definition. Since this audit found all
> reusable authorities PROVEN and no critical evidence missing (see §B, §D),
> the verdict consistent with the task contract is `READY-FOR-SPEC`. The
> unresolved decisions block specification **finalization** and
> **implementation**, not the **start** of specification work.

**Three distinct gates (do not conflate):**

```text
SPECIFICATION START      — READY (this verdict). Discovery is complete enough
                           to begin drafting an authoritative semantic/API
                           specification. The decisions to be settled BY that
                           specification are now identified (§E, §N).

SPECIFICATION FINALIZATION — NOT READY. The specification cannot be declared
                           authoritative until the open decision cluster
                           (bounded/unbounded; close-lifecycle; transfer
                           model; destruction contract; element-type/exception
                           model; topology) is resolved within it. These are
                           human-authority acts the specification exists to
                           settle, not gaps in this audit.

IMPLEMENTATION           — NOT AUTHORIZED (see §M). Distinct from both gates
                           above; requires the finalized specification, the
                           Scheduler handoff-seam design, and a separate
                           authorization act.
```

`READY-FOR-SPEC` does **not** mean implementation readiness. See §M.

---

## B. Executive conclusion

* **Does any queue implementation already exist?** **No.** Exhaustive search of
  `include/`, `src/`, `tests/`, `examples/`, `bench/`, `docs/`, and the build
  manifest (`xmake.lua`) confirms there is **no** `queue.hpp`, `channel.hpp`,
  `pipe.hpp`, `mailbox.hpp`, or `async_queue.hpp` anywhere. The only `*Queue*`
  type is `sluice::async::WaitQueue` — a scheduler-internal suspended-fiber
  wait-list that carries **no payloads** and is sealed behind
  `friend class Scheduler` (`include/sluice/async/wait_queue.hpp:119,145`). The
  `Queue`/`E12-E` primitive is documented only as future work
  (`docs/e12-sync-primitives-plan.md` §8; `docs/async-runtime-plan.md:433,505`),
  with its core shape still `HUMAN DECISION REQUIRED`. **PROVEN — NOT FOUND.**

* **Is the repository ready to begin an E12-E specification?** **Yes.**
  All *reusable authorities* (scheduler substrate, deadline/timer, cancellation,
  locking, the existing E12 primitive patterns, testing-seam discipline,
  formal-model conventions) are PROVEN. This is the SPECIFICATION-START gate
  and it is satisfied. **Specification finalization is a separate gate**: the
  specification cannot be declared authoritative until the open decision
  cluster (§E) is resolved *within* it. Those are human-authority acts the
  specification exists to settle — not gaps in this audit. See §A's three-gate
  model.

* **Largest unresolved semantic decision:** the **close-lifecycle cluster** —
  specifically whether `close()` permits buffered items to drain to existing
  consumers while rejecting producers, versus immediate-discard, and how that
  couples to blocked-consumer wake behavior. This cluster settles:
  push-after-close, pop-after-close-but-data-remains, blocked-consumer wake
  semantics, and the linearization points of close-vs-push and close-vs-pop.
  (`docs/e12-sync-primitives-plan.md` §8.2 marks this `HUMAN DECISION
  REQUIRED`.) Note: the destruction contract is a **separate** decision that is
  *not* automatically fixed by a close policy (§D.7, Check 7 of the
  corrective).

* **Largest scheduler/runtime integration risk:** the **winner-before-
  publication commit seam for item/slot reservation** — **conditional on the
  chosen transfer semantics (§E.5 D15, §D.4, §H).** This seam is required only
  under **grant semantics** (a woken consumer is granted a specific item / a
  woken producer is granted a specific slot, committed before runnable
  publication). Under a **condition-style wake-and-retry** design the existing
  `Scheduler::wake_wait_one` (returns `bool`, does not hand back the winner —
  `include/sluice/async/scheduler.hpp:255`, `src/async/scheduler.cpp:1108`) may
  suffice. The plan §8.3 (`docs/e12-sync-primitives-plan.md:1348-1353`) states
  the commit-seam requirement **conditionally**: "If exact winner identity must
  be known to bind item X before publication, then REQUIRES WINNER-BEFORE-
  PUBLICATION COMMIT SEAM"; §14 is explicitly framed "For a grant-bearing
  primitive" (`:1815`). A precedent exists —
  `Scheduler::mutex_handoff_one_locked` (`scheduler.hpp:558`,
  `scheduler.cpp:2065-2106`, commit `owner = f` at `:2088` between the resolve
  CAS and `make_runnable`) — but a Queue grant commit (buffer/reservation +
  payload) is more complex than a single-pointer assignment. Whether E12-E is
  grant-bearing at all is itself an open design decision (§E.5).

* **Is implementation authorized?** **No.** See §M.

---

## C. Repository inventory (queue-relevant symbols)

Every queue-like symbol found in the repository. "Layer" distinguishes the
async-fiber runtime from sync I/O and the OS-thread blocking pool. **No
user-facing data-queue abstraction exists** — confirmed by exhaustive
case-insensitive grep for `queue|channel|mailbox|pipe|buffer|bounded|capacity|
producer|consumer|push|pop|enqueue|dequeue|send|recv` across `include/`,
`src/`, `tests/`, and `docs/`, filtered to production code.

| Location | Symbol | Layer | Current role | Queue relevance | Public/internal/test-only | Reusable? | Evidence status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `include/sluice/async/wait_queue.hpp:119` | `class WaitQueue` | async runtime substrate | Intrusive doubly-linked FIFO of suspended-fiber `WaitNode`s; resolved by `Scheduler` only | **Wait-list, NOT a data queue.** Carries waiters, never payloads. FIFO discipline only. | Internal — header is installed but class is fully private, `friend class Scheduler` sole authority (`:145`) | **Yes — as a per-primitive private member.** A bounded Queue holds two (not-full + not-empty); an unbounded Queue holds one (not-empty only); rendezvous is DEFERRED with a distinct pairing protocol (Check 8). | PROVEN — internal wait-substrate |
| `include/sluice/async/wait_node.hpp:123` | `class WaitNode` | async runtime substrate | Caller-owned, address-stable wait epoch; `resolve_(outcome)` CAS is the sole winner authority (`:225-235`) | Wait-epoch identity, not payload | Internal (installed but no public mutators) | **Yes** — Queue waiters are `WaitNode`s, identical to other primitives | PROVEN |
| `include/sluice/async/scheduler.hpp:255` | `Scheduler::wake_wait_one` → `bool` | async runtime | Public single-winner wake; **winner identity consumed inside Scheduler** | The seam a Queue `notify` would call — but it does NOT expose the winner | Public | **Yes for wake-count; NO for winner-aware commit** (see §D, §H) | PROVEN — winner identity not handed back |
| `include/sluice/async/scheduler.hpp:558` | `Scheduler::mutex_handoff_one_locked` → `WaitNode*` | async runtime (E12-C private seam) | Mutex direct-handoff: resolve FIFO head, commit `owner=f`, publish — winner returned to caller | **The precedent for the Queue item/slot reservation seam.** Returns `WaitNode*`, commits primitive state between CAS and `make_runnable` | Private (Scheduler-internal; Mutex-specific) | **Yes — architectural template only;** the Queue commit body is different (buffer/reservation, not a pointer) | PROVEN — reusable with adaptation |
| `include/sluice/async/scheduler.hpp:144` | `WorkerState::local_runnable` (`std::deque<Fiber*>`) | scheduler internal | Per-worker runnable-fiber run queue | Scheduler run-queue; not a messaging abstraction | Internal (private member) | No — scheduler plumbing | PROVEN not a data queue |
| `include/sluice/async/scheduler.hpp:150` | `WorkerState::inbox` (`std::deque<Fiber*>` + cv) | scheduler internal | Cross-worker Fiber transport inbox | Scheduler run-queue; "inbox" is Fiber routing, not a data mailbox | Internal (private member) | No | PROVEN not a data queue |
| `include/sluice/async/scheduler.hpp:835` | `Scheduler::pending_spawn_` (`std::deque<Fiber*>`) | scheduler internal | Pre-start Fiber assignment queue | Scheduler run-queue | Internal (`SLUICE_GUARDED_BY(global_mtx_)`) | No | PROVEN not a data queue |
| `include/sluice/async/async_mutex.hpp:223` | `AsyncMutex::waiters_` (`WaitQueue`) | primitive internal | FIFO of mutex waiters | Wait-list, no payload | Internal private; **no `wait_queue()` accessor** (`:40-47`) | (Pattern reference) | PROVEN internal |
| `include/sluice/async/semaphore.hpp:225` | `Semaphore::waiters_` (`WaitQueue`) | primitive internal | FIFO of semaphore waiters | Wait-list, no payload; `max_permits_` is a permit bound, not a queue capacity | Internal private; no accessor (`:34-41`) | (Pattern reference) | PROVEN internal |
| `include/sluice/async/condition.hpp` | `AsyncCondition::waiters_` (`WaitQueue`) | primitive internal | FIFO of condition waiters | Wait-list, no payload | Internal private | (Pattern reference) | PROVEN internal |
| `include/sluice/async/threadpool_backend.hpp:86-89` | `enqueue_size`/`enqueue_void` | backend internal | OS-thread job submission for blocking-I/O offload | Producer/consumer at the OS-thread level (Completion-based), not a Fiber channel | Internal backend API | No — different runtime layer | PROVEN distinct |
| `include/sluice/async/batch.hpp:89-107` | `Batch::pop`/`next` | async API | Reap I/O completions in completion order; each dequeued once | "dequeue" of IO completions, not a messaging queue | Public but IO-completion reap API (mirrors Zig) | No — different abstraction | PROVEN not a generic queue |
| `include/sluice/buffer.hpp` | `BufferedReader`/`BufferedWriter` | sync I/O | Caller-owned byte buffer for Reader/Writer | Byte buffer, not a channel | Public | No | PROVEN not queue-like |
| `include/sluice/buffered_readable.hpp:21` | `class BufferedReadable` | sync I/O capability | Expose buffered unread bytes (`peek_buffered`/`consume_buffered`) | Byte buffer capability interface | Public | No | PROVEN not queue-like |
| `include/sluice/blocking_io_pool.hpp` | `BlockingIoPoolOptions{.max_queue_depth=64}`, `shutdown()` | sync OS-thread pool | Bounded OS-thread task queue with backpressure + `shutdown()` lifecycle | **Closest analog in the repo** (bounded depth + shutdown), but lives in the **blocking-IO threadpool, NOT the async/fiber runtime** | Public (sync pool) | Naming precedent only (`shutdown`/bounded depth); **not an async primitive** | PROVEN — distinct runtime layer |
| `tests/e10_wait_queue_test.cpp` | — | test | Tests the `WaitNode`/`WaitQueue` protocol via public `Scheduler` seams (no test friend) | Test fixture for the internal wait-substrate | Test-only | (Reference for test conventions) | PROVEN test-only |
| `tests/test_t3_simple.cpp` (untracked) | `t3_simple_live` | test | `AsyncCondition` cond.wait + notify_one smoke test | **No queue content.** Uses `AsyncMutex`, `AsyncCondition`, `WaitNode` | Test-only (uncommitted) | None — unrelated to queue | PROVEN unrelated |
| `scripts/run-e12-tlc-all.sh` (untracked) | — | tooling | Runs 13 TLA+ TLC checks for E12-D `AsyncCondition` | **No queue content.** | Tooling (uncommitted) | None | PROVEN unrelated |
| `docs/e12-sync-primitives-plan.md` §8 (lines 1281-1413) | "E12-E Queue" | doc/planning | Future primitive design: bounded/unbounded buffer, `send`/`push`, `recv`/`pop`, `close`, not-full/not-empty waiters, item/slot reservation | **Promises a future Queue API but NOT implemented.** Bounded-vs-unbounded is `HUMAN DECISION REQUIRED` (§8.2); capacity-zero (rendezvous) `DEFERRED` | Doc only — no code, no header | (The authority this audit deepens) | PROVEN future-design doc, not a shipped API |
| `docs/async-runtime-plan.md:433,505-515` | "E12-E Queue" / "Queue" | doc/planning | Lists Queue as future E12-E: "Introduces producer/consumer backpressure"; requires not-empty/not-full waiters + close | Same future-design reference | Doc only | (Roadmap authority) | PROVEN future-design, no code |
| `docs/api-reference.md` | — | doc | Public v0.1-mvp API manifest | **No queue/channel primitive listed.** Only "queue" hits are `UringStats` io_uring submit/completion counts | Public-API manifest | — | PROVEN no public queue API promised |

### C.1 Explicit determinations (per Phase 1 checklist)

1. **User-facing queue abstraction?** **NOT FOUND.** No `Queue`, `Channel`,
   `Mailbox`, `Pipe`, or `async_queue` type in `include/` or `src/`. Verified
   by grep for `class Channel|class Mailbox|async_queue|class Queue|struct
   Queue|blocking_queue|std::queue` returning zero production hits.
2. **Scheduler-internal queue only?** **YES** — `WaitQueue` (wait-list
   substrate), `local_runnable`/`inbox`/`pending_spawn_` (`std::deque<Fiber*>`,
   `scheduler.hpp:144,150,835`). All private; no public accessors.
3. **Producer/consumer waiter queues?** **Partially** — Mutex/Semaphore/
   Condition/Event each hold a private `WaitQueue waiters_`. These are
   **single-sided** waiter FIFOs (one side suspends, the other resolves);
   there is **no two-sided buffered producer/consumer transfer** anywhere.
4. **Channel/mailbox equivalent?** **NONE.** `inbox` (`scheduler.hpp:150`) is
   cross-worker Fiber routing plumbing, not a data mailbox.
5. **Queue-like test fixtures?** **YES — one:** `tests/e10_wait_queue_test.cpp`
   exercises the internal `WaitQueue` substrate via public Scheduler seams. No
   `tests/test_*queue*` or `tests/test_*channel*` for any data queue exists.
6. **Abandoned/stale queue designs in docs?** **YES** — `docs/e12-sync-
   primitives-plan.md` §8 and `docs/async-runtime-plan.md` describe an E12-E
   Queue that is **not implemented**. This is forward-looking, NOT frozen.
7. **Documentation implicitly promising queue behavior?** **No implicit
   promise of a *shipped* queue.** Docs express *intent to one day add a
   Queue*; the frozen public-API manifest (`docs/api-reference.md`) contains
   no queue/channel primitive.
8. **Naming collisions?** `include/sluice/async/` has **no** `queue.hpp`/
   `channel.hpp`/`pipe.hpp`/`mailbox.hpp` — those names are free. `WaitQueue`
   already occupies the `*Queue` suffix in `sluice::async`, so a bare `Queue`
   name would coexist with (and risk confusion with) `WaitQueue`. `close`/
   `shutdown`/`drain` are not methods on any async primitive today;
   `shutdown()` is used only on `BlockingIoPool` (sync). `send`/`recv` appear
   only as doc terms. **CONCLUSION:** mild naming pressure; a disambiguating
   name (e.g. `AsyncQueue`, consistent with `AsyncMutex`/`AsyncCondition`) is
   advisable but not forced. This is a **UNDECIDED** API-style detail.

---

## D. Existing authority map

The authoritative mechanisms E12-E would use or extend. Each row is backed by
exact file:line evidence. This section also folds in the exception/type-system
and lifetime/destruction authority audits (§D.6, §D.7), since both are
"existing authority" determinations about what the repository does and does
not already constrain.

### D.1 Suspension and resumption (Phase 2.1)

* **How a fiber registers a wait:** `Scheduler::await_wait(WaitQueue& q,
  WaitNode& node)` (`include/sluice/async/scheduler.hpp:249`, impl
  `src/async/scheduler.cpp:1037`). Under `global_mtx_` + `q.mtx()`
  (`scheduler.cpp:1053-1054`) it calls `q.register_wait_locked(node, me)`
  (`:1055`) which tail-links the node and CAS-moves it `Detached→Registered`
  (`wait_queue.hpp:174-186`, `wait_node.hpp:203-213`).
* **When it becomes suspended:** `me->make_waiting()` (`scheduler.cpp:1070`)
  is called **inside** the same critical section; the actual `context_switch`
  (`:1075`) is the **only** step outside the lock.
* **How it is published as runnable:** the winner path
  (`wake_wait_one_locked`, `scheduler.cpp:1078-1106`) does
  `make_runnable` (`:1102`) then `route_runnable_locked` (`:1103`), both still
  under `global_mtx_`.
* **Are registration and scheduler handoff atomic?** **PROVEN YES** — register,
  recheck, and `make_waiting` all occur inside one `global_mtx_`+`q.mtx()`
  critical section (`scheduler.cpp:1048-1071`). Only `context_switch` is
  outside.
* **May wakeups occur before suspension completes?** **No** — a concurrent
  resolver takes `global_mtx_`, which the admission critical section holds
  continuously through `make_waiting`. The defense-in-depth `is_terminal()`
  recheck at `:1065` catches any speculative registration that a racing
  resolver already terminalized.
* **Lost-wakeup prevention:** the register+recheck+suspend idiom (E10 §4)
  closes the lost-wake window. For Queue, the "condition predicate" (buffer
  non-empty / not-full) is rechecked under the same lock — the Event
  `await_event_wait` recheck (`scheduler.cpp:1440-1455`) is the canonical
  template.
* **Resumptions under or outside internal locks?** **Outside.** The resumed
  fiber reads `node.outcome()` after `context_switch` returns, holding no lock
  (`scheduler.cpp:1075`). The **publication** (`make_runnable`/
  `route_runnable_locked`) happens under `global_mtx_` *before* the fiber
  actually resumes; the fiber's own post-resume code runs lock-free.

**Classification: DIRECTLY REUSABLE** for the admission-closure *mechanism*
(register+recheck+make_waiting shape). The Queue-specific *predicate* (buffer
non-empty / not-full) and any payload/reservation bookkeeping are additions,
but the suspension/resumption idiom itself transfers verbatim.

### D.2 Deadlines and timers (Phase 2.2) — PROVEN

* **Authoritative deadline representation:** `deadline_t` =
  `deadline_tick_t` = `std::uint64_t` — a **monotonic absolute tick**
  (`include/sluice/async/timer_registration.hpp:65`,
  `include/sluice/async/scheduler.hpp:269`). **NOT** a duration or wall-clock
  (`docs/e11-deadline-timer-wait.md:162,166-168`). The caller converts a
  relative duration via `monotonic_now() + duration` (`scheduler.hpp:266-274`).
* **Timer registration/cancellation:** `class TimerRegistration`
  (`timer_registration.hpp:71-151`) — a Scheduler-owned, pointer-stable
  control block bound to a live `WaitNode&`. Independent atomic state
  `active/retired/consumed` (`:75-79`). Cancellation/retirement via two CAS:
  `try_claim_expiry()` (`:96-101`, `active→consumed`) and `retire()`
  (`:109-114`, `active→retired`).
* **Composed wait+deadline:** `Scheduler::await_wait_deadline`
  (`scheduler.hpp:304`, impl `scheduler.cpp:1191-1272`). Registers the node,
  registers the timer (`timer_pool_.emplace_back` `:1223`), and performs the
  already-due (I5) inline-Expired recheck (`:1235-1252`) — all atomically
  under one `global_mtx_`+`q.mtx()` critical section.
* **Timeout-race linearization:** the **loser semantic** — three causes
  (`RESOURCE_WAKE`/`TIMER_EXPIRE`/`CANCEL`) compete for one `resolve_` CAS
  (`wait_node.hpp:225-235`). A grant whose CAS won is **final**; a late
  expire/cancel is a no-op loser (`docs/e11-deadline-timer-wait.md:193-220`).
  The non-timer winner retires the registration in the **same** `global_mtx_`
  critical section as its CAS (`retire_timer_for_node_locked`,
  `scheduler.cpp:2440-2467`, called at `:1097` and `:1134`).
* **Timeout ownership:** **scheduler-owned**, not primitive-specific. All
  three causes enter the same `global_mtx_`+`q.mtx()` critical section
  (`docs/e11-deadline-timer-wait.md:249-260`). There is **no** primitive-local
  timeout path. `expire_wait` is the single driver
  (`scheduler.cpp:1274-1297`) plus the worker-loop `pump_deadlines_locked`
  (`:2373`).
* **How timeout vs success is linearized:** by the single `resolve_` CAS plus
  `global_mtx_` serialization (Conclusion A, see §D.5).

**Classification: REUSABLE WITH ADAPTATION.** A Queue `recv_until`/`send_until`
composes `await_wait_deadline` as Semaphore/Mutex/Event do — but Queue adds
**new semantic state** on top of the deadline substrate: payload ownership, a
possible reservation-rollback obligation when a timer wins before a
reservation commit (Case A of the four post-selection cases — §D.4), and a
`closed` outcome distinct from `expired`. The timer-registration/retirement
mechanism itself is directly reusable; the Queue-specific result/rollback
layer is new. Note: under Case B (grant CAS wins first, late expire is the
loser — §D.2), the loser semantic already handles the race without Queue-
specific rollback.

### D.3 Cancellation (Phase 2.3) — PROVEN (production-supported)

* **Does cancellation exist?** **Yes** — `Scheduler::cancel_wait`
  (`scheduler.hpp:261`, impl `scheduler.cpp:1122-1142`) resolves a node with
  `Cancelled` via `q.cancel_locked(node)` → `node.resolve_(cancelled)` CAS
  (`wait_queue.hpp:222-233`). Primitive-specific queue-identity-gated wrappers
  exist (Event `event_cancel_wait` `scheduler.hpp:379`; Semaphore `sem_cancel`
  `:447`; AsyncMutex `mutex_cancel` `:532`; AsyncCondition
  `condition_cancel_wait` `:659`) — each validates membership via
  `WaitQueue::contains_locked` (`wait_queue.hpp:289-294`) before attempting
  the CAS, so a foreign/detached/terminal node fails the gate and returns
  false with no mutation.
* **Races with wakeup/close/timeout/destruction:** resolved by the single
  `resolve_` CAS + `global_mtx_` serialization (truth table at
  `docs/e10-waitnode-wait-queue.md:227-238`). Cancel vs wake: one CAS wins,
  the other is a no-op loser. Cancel vs timeout: same. Cancel vs close/
  destruction: there is **no close authority yet** (E12-E introduces it);
  destruction is a caller-contract violation (see §D.7).
* **How waiters are removed/tombstoned:** unlinked **immediately in the same
  critical section** as the winning CAS — `cancel_locked` does
  `resolve_(Cancelled)` then `unlink_locked` (`wait_queue.hpp:222-233`,
  `302-317`) under `q.mtx()`. The terminal state itself is the absorbing
  "tombstone"; there is no separate tombstone structure (the Unlink Law,
  `wait_queue.hpp:18-44`).
* **Production-supported or test-only?** **Production-supported.** `cancel_wait`
  and the primitive cancels are compiled unconditionally; they are **not**
  inside any `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` block. The
  test/production separation is sharp (see §J).

> **Note (distinct layer):** `include/sluice/async/cancel.hpp` defines a
> *cooperative task cancellation* layer (`CancelToken`/`CancelState`) that is
> explicitly "free of any scheduler/fiber dependency" (`cancel.hpp:13-16`) and
> lives ABOVE `AsyncBackend`. This is **task-level** cancellation, disjoint
> from the wait-cancellation path E12-E would use. Do not conflate them.

**Classification: REUSABLE WITH ADAPTATION.** The queue-identity-gated cancel
mechanism (mirroring `sem_cancel`/`mutex_cancel`) is directly reusable. Queue
adds **new post-cancel distinctions** that the existing primitives do not have
(§D.4 four-case separation):
* **Case A** (cancel CAS wins before the grant path's resolve_ attempt): the
  cancel path unlinks the node; the grant path's `resolve_(Woken)` loses; no
  reservation is established. Under grant semantics, the slot/item stays
  available — no explicit refund logic is needed because the loser semantic
  prevents the grant from occurring at all.
* **Case B** (grant resolve_(Woken) wins before cancel): cancel is the loser
  (no-op). The reservation is already committed under the correct commit-
  before-publication order. Ordinary WaitNode cancellation does NOT require
  "refund after a completed reservation commit."
* **Cases C and D** (abandonment after resume / payload failure after grant):
  **not wait-resolution problems** — they are operation-level or exception-
  level issues (§D.6) not solved by the cancel/expire loser semantic.
Also: a `closed` outcome distinct from `cancelled` must be represented (§E.3).

### D.4 Existing E12 primitive patterns (Phase 2.4)

See §C inventory. Each implemented primitive (Event/Semaphore/AsyncMutex/
AsyncCondition) is **REUSABLE WITH ADAPTATION** as a structural template; the
AsyncMutex `mutex_handoff_one_locked` seam is the **most important** precedent
for Queue.

| Primitive | Pattern source | Classification | Note |
| --- | --- | --- | --- |
| E12-A Event | `event.hpp`; `Scheduler::event_*` (`scheduler.hpp:317-379`) | REUSABLE WITH ADAPTATION | The lost-set admission closure (`await_event_wait`, `scheduler.cpp:1395-1470`) is the template for Queue recv/send admission; the broadcast-drain (`event_set_broadcast`, `:1301-1339`) is the template for close-and-wake-all. Event's "ready predicate" is a sticky bool; Queue's is buffer-state (may clear on consume), so the drain is **not** directly applicable but the admission shape is. |
| E12-B Semaphore | `semaphore.hpp`; `Scheduler::sem_*` (`scheduler.hpp:382-464`) | REUSABLE WITH ADAPTATION | FIFO + no-barging try (`sem_try_acquire`, `:1560-1584`, `empty_locked()`-gated) and admission head-test (`node.prev_ == nullptr`, `:1636`) are the exact template for Queue `try_recv`/`try_send`. The release transfer-vs-store disposition (`sem_release`, `:1783-1835`) is **conceptually** the rendezvous/direct-handoff path but Queue's reservation-state requirement makes it harder. |
| E12-C AsyncMutex | `async_mutex.hpp`; `Scheduler::mutex_*` (`scheduler.hpp:467-559`) | **REUSABLE WITH ADAPTATION — load-bearing** | `mutex_handoff_one_locked` (`scheduler.hpp:558`, `scheduler.cpp:2065-2106`) is the **architectural template** for the Queue item/slot reservation seam. Returns `WaitNode*`, commits `owner = f` (`:2088`) between resolve CAS and `make_runnable`, holds both locks throughout. The Queue commit body is different (buffer/reservation + payload, not a single pointer). |
| E12-D AsyncCondition | `condition.hpp`; `Scheduler::condition_*` (`scheduler.hpp:562-660`) | CONCEPTUALLY RELEVANT ONLY | Value: (a) precedent for a primitive composing another primitive's seam (Condition reuses `mutex_handoff_one_locked`, no second handoff); (b) **sequential two-queue-mtx discipline** under `global_mtx_` (`scheduler.cpp:2196-2200`) — relevant if Queue holds a not-empty AND not-full queue. The two-epoch reacquire dance and `AsyncMutex&` binding are **NOT** relevant (Queue is standalone; §8.4 forbids Queue using async Mutex internally). The `notify_all` drain (`:2326-2348`) is DIRECTLY REUSABLE for close-and-wake-all. |

**Special attention to previously discovered defect classes:**

* **Ownerless queued demand** — Semaphore's supply/demand separation (§5.1.2)
  and Mutex's `InvNoOwnerlessQueuedDemand` (`E12AsyncMutex.tla`) are the
  precedent. Queue faces an analog **only under grant semantics** (a reserved
  slot/item with no owning producer/consumer). Under a wake-and-retry design
  there is no per-waiter reservation to be orphaned. The plan §8.3 states
  "EXPLICIT RESERVATION STATE REQUIRED" within the grant-bearing framing it
  assumes (`docs/e12-sync-primitives-plan.md:1344-1346`); whether E12-E adopts
  grant semantics is an open design decision (§E.5 D15, §D.4 transfer-semantics
  alternatives).
* **Waiter registered but not yet suspended / premature runnable publication** —
  the register+recheck+`make_waiting` atomic admission window (§D.1) closes
  this. Queue inherits it verbatim.
* **Non-atomic admission** — closed by the single-critical-section admission
  (E10 §4).
* **Notify/wakeup before handoff** — Mutex's owner-before-publication order
  (`mutex_handoff_one_locked`, commit at `:2088` before `make_runnable` at
  `:1102`) is the precedent **for grant semantics**. Whether Queue needs a
  handoff at all depends on the chosen transfer model (§E.5 D15: wake-and-retry
  / grant-with-reservation / direct handoff).
* **Cancellation and expiry after selection — four distinct cases (Check 2):**
  The loser semantic (§D.2) resolves the **resolution race** (a grant whose CAS
  won is final; a late cancel/expire is a no-op). The resolution race maps to
  cases A and B of the corrective:
  * **Case A** (cancel/expire wins the resolve_ CAS before the grant path
    attempts it): the grant path's `resolve_(Woken)` loses, and no reservation
    is established. The slot/item stays available for another waiter. **Under
    grant semantics**, this is the "resource-rollback" case (item stays / slot
    refunded). **Under wake-and-retry** there is no reservation to roll back.
  * **Case B** (grant `resolve_(Woken)` wins first): a late cancel/expire CAS
    is the loser and does nothing. No "refund after completed reservation"
    arises — the existing loser semantic is sufficient.
  * **Case C** (operation abandoned after the waiter resumes): **not a
    wait-resolution problem** — the waiter has already resolved Woken, resumed,
    and the caller chooses not to complete the operation. Not solved by
    cancel/expire.
  * **Case D** (payload construction/move/publication fails after grant):
    **not a wait-resolution problem** — an exception-level issue (§D.6). Not
    solved by the cancel/expire loser semantic.
* **Queue mutation under the wrong lock** — Queue internal state (buffer,
  closed, bookkeeping) is protected by a **synchronous structural lock** (the
  `Mutex` wrapper at `mutex.hpp`), NOT the E12-C async Mutex (plan §8.4,
  `docs/e12-sync-primitives-plan.md:1377-1402`). The structural lock must NOT
  be held across `context_switch` or async Mutex acquire.
* **Test seam altering production semantics** — the `ASYNC-TEST-SEAM-
  AUTHORITY-CORRECTIVE-1` banner (`scheduler.hpp:38-57`) forbids test hooks in
  installed headers; all deterministic seams live in the non-installed
  `sluice_async_internal_testing` variant (§J).

**Transfer-semantics alternatives (Checks 1, 2, 3 of the corrective — NOT
selected here):** E12-E's wake mechanism has at least three viable shapes,
which the specification must choose among:

```text
A. condition-style wake-and-retry
    A woken consumer is told "the queue may be non-empty; re-check and retry."
    No item is bound to the waking epoch; the winner identity need not be known
    before publication. The existing Scheduler::wake_wait_one (returns bool) may
    suffice. No item/slot reservation state; no commit seam.

    Semantic costs of wake-and-retry (Check 1):
    - the woken waiter is NOT granted a specific item or slot; it receives only
      permission to re-check;
    - the condition (buffer non-empty / not-full) may be false again by the time
      the waiter resumes — another thread may have consumed the item / filled
      the slot;
    - the waiter may need to create a new wait epoch and suspend again;
    - a newly arriving operation may acquire the resource before the woken
      retryer;
    - strict FIFO resource grant and strict no-barging do NOT follow merely
      from FIFO waiter selection — the FIFO order of wake does not guarantee
      FIFO order of eventual acquisition;
    - wake order, successful resource-acquisition order, and completion order
      are distinct;
    - if an anti-barging credit, pending grant, or reserved-availability count
      is introduced to prevent this race, that state is itself a form of
      reservation-like bookkeeping and must be explicitly classified as such.

B. selected-waiter grant with slot/item reservation
    A woken consumer is granted a specific item (committed to its epoch before
    runnable publication); a woken producer is granted a specific slot. This is
    the grant-bearing design the plan §8.3/§14 assumes. REQUIRES EXPLICIT
    RESERVATION STATE and (per plan §8.3:1348-1353, §14.3.4:1930-1931)
    REQUIRES A WINNER-BEFORE-PUBLICATION COMMIT SEAM if the exact winner must
    be known to bind the item/slot.

C. direct producer-to-consumer handoff
    Producer and consumer are paired at wake time and the payload transfers
    directly, bypassing the buffer (or with the buffer as fallback). This is a
    refinement of B (it still needs reservation + commit) and is closest to
    rendezvous; rendezvous proper (capacity-0) is DEFERRED (D3).
```

This audit does **not** select among A/B/C. Mechanism classification by
transfer model:

```
Required under ALL viable designs (A, B, C):
    atomic admission closure (§D.1);
    one terminal wait-resolution winner (resolve_ CAS);
    close linearization (§H);
    single payload ownership (no payload has two owners concurrently);
    no successful push after close linearizes;
    publication must not expose incomplete observable state.

Required only under selected-waiter GRANT semantics (B, C):
    per-waiter item reservation;
    per-waiter slot reservation;
    exact-winner identity before publication;
    winner-before-publication resource commit seam;
    no-ownerless-reservation invariants;
    grant-specific phase tags and deterministic tests.

Required only under DIRECT HANDOFF (C):
    producer-to-consumer pairing state;
    direct-transfer payload ownership;
    buffer-versus-handoff mutual exclusion;
    direct-handoff phase tags and negative models.

Wake-and-retry-specific obligations (A, if chosen):
    retry/re-wait semantics (the waiter may need to re-register);
    possible loss of FIFO acquisition order after wake (the FIFO waiter
    selection order does not guarantee FIFO acquisition order);
    anti-barging policy is an explicit design choice; if chosen it requires
    reservation-like bookkeeping, which must be named;
    no claim that `woken` means item/slot ownership.
```

**Four post-selection cases (Check 2 of the corrective).** The following four
cases are distinct and must not be conflated:

```text
A. wait cancellation or expiry wins BEFORE the grant CAS
    → the grant path must not establish a reservation. The loser semantic
    (§D.2) already prevents this: the cancel/expire CAS wins, the grant
    path's resolve_(Woken) CAS loses, and no reservation is committed.

B. grant resolve_(Woken) CAS wins BEFORE cancellation or expiry
    → a late cancel/expire CAS loses (the loser semantic). The reservation
    is already committed under the correct commit-before-publication order
    (§14.2). The loser cannot revoke it. Therefore "refund after completed
    reservation" is NOT a normal case under grant semantics — ordinary
    WaitNode cancellation does not require refund.

C. the granted operation is abandoned AFTER the waiter resumes
    → the waiter resumes, reads the result, and the caller decides not to
    complete the operation (e.g. discards the item). This is an operation-
    level problem, not a wait-resolution problem. It is not solved by the
    existing cancel_wait loser semantic.

D. payload construction, move, or publication fails AFTER grant
    → an exception or error occurs after the grant CAS won but before the
    result is fully observable. This is an exception-level problem (§D.6).
    It is not solved by the existing cancel_wait loser semantic.
```

Cases A and B are resolved by the existing E10/E11 loser semantic (the single
`resolve_` CAS winner: `resolve_(Woken)` vs `resolve_(Cancelled)` vs
`resolve_(Expired)`, exactly one wins). Cases C and D are **not covered** by
the wait-resolution substrate; they are decisions for the semantic
specification. Anywhere this report previously described "refund" or "rollback"
as a general cancel-after-reservation problem, it is corrected to case A only
(cancel/expire wins before reservation commit).

### D.5 Locking and memory authority (Phase 2.5) — PROVEN

| Concern | Current authority | Exact evidence | Reusable for E12-E? | Missing seam or rule | Risk |
| --- | --- | --- | --- | --- | --- |
| Global scheduler lock | `Scheduler::global_mtx_` (`scheduler.hpp:801`, `mutable`) protects all coordination state (waiting maps, `waiting_waitq_count_`, timer pool/heap, admission) | Decl `:801`; acquired at `scheduler.cpp:1053,1118,1131,1213,1287,1321` and throughout worker loop | **Yes** — Queue wake/cancel/expire serialize on it identically | None | Low |
| Per-queue structural lock | `WaitQueue::mtx_` + `mtx()` (`wait_queue.hpp:319,152`); private, `friend Scheduler` | `:152`, `:319` | **Yes** — Queue's not-empty/not-full `WaitQueue`s each have one | None | Low |
| Documented lock ordering | `global_mtx_` → `q.mtx()` → (optionally `wake_mtx_`) | `docs/e10-waitnode-wait-queue.md:321`; enforced at every site; reverse `wake_mtx_→global_mtx_` forbidden (`scheduler.hpp:872-874`) | **Yes** — Queue must follow the same order | **Queue-specific:** if a structural buffer lock is added *outside* `WaitQueue`, its position in the order must be defined (likely: `global_mtx_` → buffer-lock → `q.mtx()`, or buffer-state folded into the existing `q.mtx()` discipline) | Medium — a new lock is a new deadlock surface |
| Structural `Mutex` (sync) | `include/sluice/async/mutex.hpp:17-33` — TSA-annotated `std::mutex` wrapper, synchronous/thread-blocking, NOT async | Full class quoted at `:17-33` (`SLUICE_CAPABILITY`, delegates to `std::mutex impl_`) | **Yes — this is the lock Queue uses internally** (plan §8.4) | Must NOT be held across `context_switch`/async acquire | Low if rule obeyed |
| TSA annotations | Pervasive vocabulary in `thread_annotations.hpp`; used across all primitives | e.g. `SLUICE_GUARDED_BY`, `SLUICE_REQUIRES`, `SLUICE_RETURN_CAPABILITY` | **Yes** — Queue fields/methods should be annotated identically | None | Low |
| Callbacks/allocs/moves/dtors under runtime locks | **No user code under any runtime lock today.** `context_switch` always outside the lock. Only Scheduler-owned control-block allocations under `global_mtx_` (`timer_pool_.emplace_back`) | `scheduler.cpp:1271` (block ends `:1267`); comments `:1208,1400,1592`; `timer_pool_` bounded by live+pending deadline waits (`docs/e11-deadline-timer-wait.md:915-920`) | **Partial.** The "no user code under `global_mtx_`" rule is PROVEN and Queue must obey it. Whether user `T` ops may run under a *Queue internal* lock is UNRESOLVED (§D.6.1). | **Queue-specific rule required, not yet settled:** whether user `T` construct/move/destroy may run under the structural buffer lock, or must be moved outside all internal locks (§D.6 Q7.3 / §D.6.1). Do NOT assume "structural lock is fine." | **High** |
| Allocator/lifetime | `WaitNode` caller-owned, address-stable, non-copy/move; `~WaitNode` debug-asserts `!is_registered()` (`wait_node.hpp:136-139`); `WaitQueue` debug-asserts empty (`wait_queue.hpp:132-137`) | as cited | **Yes** for wait-node lifetime | **Queue-specific (grant semantics only):** the item/slot reservation representation must survive E8 Worker migration (keyed by wait epoch/`WaitNode`, not Worker) — plan §14.3.4 | Medium |
| Payload movement invoking user code under a runtime lock | **Currently impossible** — no primitive carries user payload `T`; a `WaitNode` holds only an opaque `Fiber*` | `wait_node.hpp:237`; no `T` in any primitive | **N/A today — Queue is the first payload-bearing primitive** | **A new rule is required** (see §D.6 / §D.6.1) | **High** |

### D.6 Exception and type-system authority (Phase 7)

**No existing async primitive carries user payload `T`.** A `WaitNode` holds
only an opaque `Fiber*` (`wait_node.hpp:237`). The sync I/O layer
(`include/sluice/buffer.hpp`, `buffered_readable.hpp`) carries bytes
(`std::byte`/char) but is not a messaging queue. So E12-E is the **first
payload-bearing async primitive** — there is no direct precedent for `T`
constraints in the async layer.

Implications to resolve (each currently **UNDECIDED** — no repository evidence
fixes it):

| `T` property | Implication | Likely requirement |
| --- | --- | --- |
| move-only `T` | queue must move, never copy | **required to support** (modern C++ default; `std::move`-only types like `unique_ptr` are common) |
| non-default-constructible `T` | cannot default-construct a return slot | **required to support** → emplace directly into queue storage / `optional<T>`-style result |
| throwing move ctor | payload move under lock could throw | **see below Q7.3** — must decide nothrow-move requirement vs exception handling |
| throwing copy ctor | copy into buffer could throw | likely **forbid copy** (move-only queue) or require nothrow copy |
| throwing allocator | buffer allocation under lock could throw | likely use fixed-capacity storage (bounded) avoiding dynamic alloc under lock |
| throwing emplace ctor | in-place construction could throw | decide whether emplace runs under structural lock |
| destructor of `T` | runs when item removed/discarded | must NOT run under `global_mtx_` |
| alignment/storage | `T` may need alignment | queue storage must respect alignof(T) |
| reference/pointer/incomplete/const-qualified `T` | various | likely **restrict to object types** (no references; pointers allowed but questionable value; incomplete types likely forbidden; const-T likely forbidden for pop) |

**Mandatory questions (Phase 7):**

1. **Must E12-E require nothrow move construction?** **UNDECIDED.** Repository
   evidence: `WaitNode`/`Fiber` operations are `noexcept` where they touch
   runtime state; no payload precedent. The Semaphore/Mutex primitives are
   entirely `noexcept`-shaped (no `T`). A nothrow-move requirement is the
   **safest** choice (enables the commit-before-publication seam to be
   exception-free) but may be too restrictive. **HUMAN DECISION.**
2. **Could emplacement construct directly into queue storage?** **UNDECIDED —
   feasible and attractive.** A bounded queue with `std::aligned_storage`-style
   slots (or `std::optional<T>` slots) could emplace directly, avoiding a move.
   This composes with the reservation model (reserve a slot, then emplace).
   **CONSISTENT BUT UNPROVEN.**
3. **May user-defined `T` operations (construct/move/emplace/destroy) run under
   internal locks?** **UNRESOLVED.** This decomposes into two distinct
   sub-questions the corrective (Check 5) separates:
   * **(3a) Under `Scheduler::global_mtx_`?** **STRONGLY NO** — the repo-wide
     rule is "no user code under `global_mtx_`" (§D.5; only Scheduler-owned
     control-block allocations occur there). A user `T` op under `global_mtx_`
     would violate the established discipline. PROVEN rule.
   * **(3b) Under any Queue internal lock (the structural buffer lock)?**
     **UNRESOLVED — do not treat as settled.** A Queue structural lock is still
     an internal runtime/primitive lock. User-defined `T` operations may throw,
     block, allocate, re-enter the Queue, acquire another lock, or invoke
     arbitrary user code; running them under the structural lock is therefore
     **not** automatically acceptable merely because it is not `global_mtx_`.
     No authoritative repository rule settles whether user `T` ops may run under
     *any* Queue internal lock. The earlier draft's "structural buffer lock:
     probably yes" is **withdrawn** as unsupported. Candidate approaches the
     specification may choose among (NOT prescribed here): nothrow-move
     restriction; preconstruction outside the lock; fixed storage vs allocating
     containers. These five are separated below (§D.6.1).
4. **If payload transfer throws after waiter selection, how is the waiter
   restored or failed?** **UNDECIDED — this is Case D of the four post-selection
   cases (§D.4), distinct from cases A–C.** The grant CAS has already won; the
   payload move or construction fails after the winner is known. Options:
   (a) the waiter is failed with an exception outcome; (b) the slot/item is
   rolled back and the waiter re-queued (hard); (c) nothrow-move required so
   this cannot happen. **Coupled to Q7.1 and (only under grant semantics) to
   the reservation-rollback rules of §F.2.** This is NOT solved by the
   existing cancel/expire loser semantic: the loser CAS handles resolution
   races (cases A and B), not post-grant payload failures.
5. **What exception guarantee is realistic?** **UNDECIDED.** Basic guarantee
   (queue remains valid, no leak) is the floor; strong guarantee (operation
   rolled back) is hard given any commit-before-publication seam (grant
   semantics only).
6. **Are existing async primitives exception-neutral?** **Yes — they are
   `noexcept`-shaped** (no throwing operations in the wait/wake/cancel/expire
   paths). E12-E would be the first primitive that *could* throw (via `T`).
7. **Does the project avoid exceptions entirely?** **No** — the project uses
   C++ exceptions (sanitizers and standard library; no `-fno-exceptions`
   observed). But the async runtime paths are designed to be non-throwing.

#### D.6.1 Five separated sub-questions (Check 5 — not prescribed)

```text
(a) no user-defined T operation under Scheduler::global_mtx_
        — PROVEN rule (§D.5). Not optional.

(b) no user-defined T operation under any Queue internal lock
        — UNRESOLVED. No authority settles it. A user T op under the
        structural lock can throw/block/alloc/re-enter/acquire — not
        automatically safe.

(c) nothrow-move restrictions on T
        — UNDECIDED (Q7.1). Would make (b) and Q7.4 moot if adopted, but may
        be too restrictive.

(d) preconstruction of T outside the lock (construct, then move/link under lock)
        — UNDECIDED candidate; trades a move under lock for construction
        outside.

(e) fixed storage (bounded, aligned slots) vs allocating containers
        — UNDECIDED; fixed storage avoids dynamic allocation under lock (relevant
        to throwing-allocator), allocating containers add an allocation under
        some lock.
```

**Status:** the exception/type/payload-lock model is a cluster of decisions
the specification must settle. It blocks **specification finalization** (the
`T`-under-lock rule and the exception guarantee are part of the authoritative
contract) and, under grant semantics, interacts with the reservation-state
design; it does **not** block the **start** of specification work (§A
SPECIFICATION-START gate).

### D.7 Lifetime and destruction authority (Phase 8)

**Plausible contracts** against repo precedent:

| Contract | Repo precedent | Fit for Queue |
| --- | --- | --- |
| destruction requires no waiters | `~WaitQueue` debug-asserts empty (`wait_queue.hpp:132-137`) | **strong precedent** |
| destruction asserts no waiters | `~AsyncMutex` asserts `owner_==nullptr` (`async_mutex.hpp:100-103`); `~AsyncCondition` asserts `active_waits_==0` (`condition.hpp:120-124`) | **strong precedent** |
| destruction cancels all waiters | **NONE** — no primitive cancels on dtor; plan §4.4 Event explicitly forbids "primitive-local cancellation winner" | **forbidden by precedent** |
| destruction behaves like close | none | possible but **diverges from precedent** |
| destruction is UB with concurrent ops | implicit in caller-contract model | possible |
| shared-state ownership permits ops to outlive wrapper | none (no `shared_ptr`-based primitive) | **no precedent** — likely not first-scope |
| coroutine frames hold queue state alive | none | **no precedent** |

**Risks:**

* **use-after-free:** a fiber resuming after the Queue is destroyed. Mitigated
  by the caller-contract (waiters must be drained before dtor) + the
  `~WaitQueue` debug assert.
* **coroutine resumption after destruction:** the resume path reads
  `node.outcome()` then returns to primitive code that dereferences the Queue.
  The debug-assert-empty contract makes this a caller violation.
* **timer callback after destruction:** E11 `TimerRegistration` is
  Scheduler-owned (in `timer_pool_`), not Queue-owned, and is retired in the
  same CS as the resolve CAS (`retire_timer_for_node_locked`). So a timer
  callback cannot reach a destroyed Queue *through the registration* — but a
  fiber that owns a live `WaitNode` bound to a destroyed Queue would
  use-after-free on resume. The destruction contract must prevent this.
* **cancellation callback after destruction:** symmetric.
* **payload destruction under runtime locks:** `T`'s destructor must NOT run
  under `global_mtx_` (§D.6.1(a) — PROVEN). Whether it may run under a Queue
  internal lock is UNRESOLVED (§D.6.1(b)). Discarded buffered items (under an
  immediate-discard close policy) must be destroyed somewhere whose lock
  discipline is settled by the §D.6 contract — do not presume "the structural
  lock" here.
* **close/destruction callback reentrancy:** if close wakes waiters, and a
  woken fiber resumes inline (cooperative scheduler), it could re-enter the
  Queue. The existing primitives handle this via the publication-under-lock /
  resume-outside-lock discipline (§D.1).

**Close and destruction are independent decisions (Check 7).** Selecting a
close policy (drain-on-close vs immediate-discard, §E.4) does **not** settle
the destruction contract. The destruction-relevant decisions are separately:

```text
close semantics               — §E.4 D5–D14 (a lifecycle-operation contract)
post-close drain semantics    — part of D5/D7
destructor preconditions      — what ~Queue asserts (empty waiters? empty buffer?)
concurrent destruction behavior — UB-with-concurrent-ops vs caller-contract
whether destruction implicitly closes — does ~Queue run close()? (precedent: NO)
whether destruction wakes/cancels waiters — precedent: NO (forbidden; §4.4)
```

Repository precedent **strongly recommends** a quiet-state destructor contract
(debug-assert empty; no cancel/wake/force-close), but that recommendation does
not become proven merely because a close policy is selected — it stands on its
own precedent evidence (the four existing-primitive destructors).

**Recommended destruction contract (non-authoritative, following precedent):**
caller-contract violation — `~Queue` debug-asserts both wait queues empty,
buffer empty (or drained). **No cancel-on-destruct, no wake-on-destruct, no
implicit-close-on-destruct.** This matches all four existing primitives and
the Event destruction resolution (`docs/e12-sync-primitives-plan.md` §4.4). It
is a recommendation for the specification to confirm, not a closed decision.

**Status: UNDECIDED** — the plan does not explicitly close the Queue
destruction contract. Recommend closing as caller-contract-violation for
consistency, independent of the close-policy choice. Classification:
**blocks specification finalization** (the destructor precondition is part of
the authoritative contract), not specification start.

---

## E. Semantic decision register (Phase 3)

Every material decision, with repository constraints and current status. Status
legend: `RESOLVED` (repository authority fixes it), `HUMAN-DECISION-REQUIRED`
(materially different valid choices exist), `DEFERRED` (out of first scope),
`UNDECIDED` (not yet analyzed to a status).

> The plan document (`docs/e12-sync-primitives-plan.md` §8) is the cross-
> primitive authority; this register reproduces its Queue decisions and adds
> the decisions the plan leaves implicit.

### E.1 Queue topology

| # | Decision | Available options | Repository constraints | Risk of choosing incorrectly | Evidence needed | Status |
| --- | --- | --- | --- | --- | --- | --- |
| D1 | bounded vs unbounded | (a) bounded (capacity ≥ 1, backpressure); (b) unbounded (no not-full waiters, no backpressure); (c) both (template/config) | Plan §8.2: `HUMAN DECISION REQUIRED`. Repo has both precedents: bounded (`BlockingIoPool max_queue_depth=64`, sync) and unbounded (`std::deque` run-queues). Semaphore's `max_permits_` bounded-counter pattern is reusable for bounded. | Materially different APIs, state space, and liveness. Wrong choice forces a re-spec. | A product-level decision: does the first Queue need backpressure? | **HUMAN-DECISION-REQUIRED** |
| D2 | capacity: compile-time vs runtime | (a) template `<std::size_t Cap>`; (b) runtime constructor arg | Semaphore uses runtime `max_permits_` (`semaphore.hpp:225`) — precedent for runtime. No compile-time-capacity primitive exists. | Compile-time enables static storage optimization but reduces flexibility; runtime matches Semaphore. | Consistency with Semaphore precedent favors runtime. | **UNDECIDED** (depends on D1) |
| D3 | capacity-zero (rendezvous) | include or defer | **Plan §8.5/§8.2: DEFERRED.** Rendezvous is a separate protocol (producer/consumer pairing + direct transfer + no buffer), NOT a derived case of bounded Queue. | High — modeling capacity-0 as a bounded queue with Cap=0 misrepresents the state space (no buffer, pairing relation instead). | None for first scope. | **DEFERRED** |
| D4 | concurrency topology | (a) MPSC; (b) MPMC; (c) SPMC; (d) SPSC | All existing primitives are multi-waiter (MPMC-equivalent at the wait layer). `WaitQueue` is an intrusive FIFO supporting arbitrary producers of wakeups. | Restricting topology reduces state space but may be too narrow for a general primitive. | Product decision: is MPMC required? (The wait substrate supports it.) | **HUMAN-DECISION-REQUIRED** (default leaning MPMC given substrate, but not fixed) |

### E.2 Operation model

Candidate operations and classification (Phase 3.2 vocabulary). **No operation
is `REQUIRED BY EXISTING PROJECT DIRECTION`** — the roadmap
(`docs/async-runtime-plan.md:505-515`) requires only "not-empty waiters,
not-full waiters, close semantics"; it does not name operations.

| Operation | Candidate name(s) | Classification | Note |
| --- | --- | --- | --- |
| blocking push | `push`/`send`/`enqueue`/`async_push` | CONSISTENT BUT UNPROVEN | Plan §8.1 uses `send`/`push`. Semaphore/Mutex use `acquire`/`lock`; no `send`/`push` precedent in async primitives. |
| non-blocking push | `try_push`/`try_send` | CONSISTENT BUT UNPROVEN | Mirrors `try_acquire`/`try_lock` (`semaphore.hpp:134`, `async_mutex.hpp:122`). |
| timed push | `push_until`/`send_until`/`push_for` | CONSISTENT BUT UNPROVEN | Mirrors `acquire_until`/`lock_until`/`wait_until` (`:173`,`:162`,`event.hpp:142`). The repo uses `_until(deadline_t)` (absolute), not `_for(duration)`. |
| blocking pop | `pop`/`recv`/`dequeue`/`async_pop` | CONSISTENT BUT UNPROVEN | Plan §8.1 uses `recv`/`pop`. |
| non-blocking pop | `try_pop`/`try_recv` | CONSISTENT BUT UNPROVEN | |
| timed pop | `pop_until`/`recv_until` | CONSISTENT BUT UNPROVEN | |
| close | `close` | CONSISTENT BUT UNPROVEN | Not a method on any async primitive today; `shutdown()` is `BlockingIoPool`-only (sync). |
| closed query | `is_closed` | OPTIONAL | Mirrors Event `is_set()` (`event.hpp:95`). |
| size query | `size` | OPTIONAL | Mirrors Semaphore `available()` (A5: observational snapshot, may stale; `docs/e12-semaphore.md`). |
| empty/full | `empty`/`full` | OPTIONAL | SEMANTICALLY DANGEROUS if used for TOCTOU decisions (plan §3.7); diagnostics-only is safe. |
| waiting counts | `waiting_producers`/`waiting_consumers` | OPTIONAL | Test/diagnostic only; not in any primitive's public surface today. |

**Naming-style decision:** The repo's async-fiber primitives use the `Async`
prefix for the two that collide with sync/std names (`AsyncMutex`,
`AsyncCondition`) and no prefix for the two that don't (`Event`, `Semaphore`).
There is no sync `Queue` in the repo, so the bare name `Queue` is *available*,
but `WaitQueue` already lives in `sluice::async`. A disambiguating name
(`AsyncQueue`) is **advisable but not forced** — **UNDECIDED API-style.**

### E.3 Blocking conditions (Phase 3.3) — must be distinguished, not collapsed

These are **distinct** failure/block causes the API result model must represent
separately (do not collapse into one generic "failure"):

1. consumer blocked because queue empty;
2. producer blocked because bounded queue full (only if bounded chosen);
3. operation rejected because queue closed;
4. waiter timed out;
5. waiter cancelled;
6. object being destroyed (caller-contract violation — see §D.7);
7. payload construction or movement failed (exception — see §D.6).

**Status:** the E10/E11 resolution causes (`woken`/`expired`/`cancelled`) cover
1,2,4,5 via `WaitOutcome`. **(3) closed** is a **new** outcome cause E12-E
introduces — the plan §8.2 resolves blocked-producers-on-close to "wake with a
`closed` outcome" but the consumer side and the result-model representation are
**HUMAN-DECISION-REQUIRED**. **(7) exception** is entirely new (no existing
primitive carries payload). **UNDECIDED** how `closed` is represented (a fourth
outcome enum? a separate result type?).

### E.4 Close semantics (Phase 3.4) — mandatory decision area

This is the **largest open cluster**. Per plan §8.2:

| # | Decision | Options | Plan status |
| --- | --- | --- | --- |
| D5 | close with buffered items | (a) drain-on-close (consumers may recv remaining; producers reject); (b) immediate-discard | **HUMAN-DECISION-REQUIRED** |
| D6 | close with blocked producers | wake with `closed` outcome; `send` returns error | **RESOLVED** (plan §8.2) |
| D7 | close with blocked consumers | (a) wake with `closed` (recv returns "no more data"); (b) let them drain buffered items first, then `closed` | **HUMAN-DECISION-REQUIRED** — coupled to D5 |
| D8 | push after close | error (`closed`), no enqueue | **RESOLVED** (plan §8.2) |
| D9 | pop after close but data remains | coupled to D5 | **HUMAN-DECISION-REQUIRED** |
| D10 | close idempotence | idempotent vs non-idempotent | **UNDECIDED** |
| D11 | `try_pop` after close | may continue draining vs rejected | **UNDECIDED** (coupled to D5/D9) |
| D12 | `pop` distinguishes closed-and-empty from timeout | yes (separate outcome) vs no | **UNDECIDED** (coupled to D7 and the result model) |
| D13 | size/empty validity after close | valid snapshot vs invalid | **UNDECIDED** |
| D14 | reopening | forbidden (close monotonic) | **UNDECIDED** (plan §11.5 asserts "close is monotonic" as a target invariant, implying forbidden) |

> D5/D7/D9 form one coupled cluster: choosing drain-on-close transitively fixes
> D9 (pop-after-close-but-data-remains = allowed) and biases D7 (let consumers
> drain then close). Choosing immediate-discard transitively fixes them the
> other way. **This cluster must be settled as a unit.**

**Compatibility with RAII destruction:** whichever close policy is chosen must
compose with the destruction contract (§D.7). If destruction is a caller-
contract-violation debug-assert (the repo-wide precedent), then close is the
*only* graceful-shutdown mechanism and must be specifiable independently of
destruction.

### E.5 Transfer semantics (Phase 3.5)

| # | Decision | Options | Status |
| --- | --- | --- | --- |
| D15 | transfer model | (a) buffer-first (always through buffer); (b) direct producer→consumer handoff (rendezvous-style optimization when consumer waiting); (c) hybrid | **UNDECIDED** — D3 (rendezvous) is DEFERRED, but a *bounded* queue with waiting consumer may still optimize via direct handoff. Plan §14.3.4 implies Queue needs the winner-aware seam for *both* consumer item reservation and producer slot reservation, suggesting hybrid is anticipated. |
| D16 | linearization point for push | buffer insertion (buffer-first) / slot reservation (handoff) / payload commit | **UNDECIDED** — see §H linearization matrix |
| D17 | linearization point for pop | item reservation (handoff) / buffer removal (buffer-first) | **UNDECIDED** — see §H |
| D18 | slot reserved before payload movement? | yes (handoff) / no (buffer-first) | **UNDECIDED** — coupled to D15 |
| D19 | selected receiver cancelled | item stays for another consumer (reservation refunded) — **RESOLVED by composition** (plan §8.2 + §5.2 analogue) | RESOLVED (once reservation state exists) |
| D20 | payload transfer throws | waiter restored or failed? | **UNDECIDED** — see §D.6 |

**The ordered moments that must NOT be collapsed (plan §3.5):**
`waiter selected → capacity reserved → payload committed → result published →
waiter runnable → operation returned`. These are distinct; the audit forbids
treating them as one atomic step (see §F).

### E.6 Fairness (Phase 3.6)

| # | Decision | Options | Status |
| --- | --- | --- | --- |
| D21 | FIFO order of buffered elements | yes (buffer is FIFO) | **RESOLVED** — buffer is a FIFO queue by definition (a `std::deque`-equivalent). |
| D22 | FIFO order of blocked producers | yes (not-full `WaitQueue` is FIFO) | **RESOLVED** — `WaitQueue` is an intrusive FIFO (`wait_queue.hpp:319-321`); selection via `wake_one_locked` resolves head (`:199-211`). |
| D23 | FIFO order of blocked consumers | yes (not-empty `WaitQueue` is FIFO) | **RESOLVED** — same. |
| D24 | barging forbidden? | yes (no bypass of eligible queued waiter) | **STRONGLY IMPLIED RESOLVED** — Semaphore A2 and Mutex M-H3 both forbid barging (`docs/e12-sync-primitives-plan.md` §5.4, §6.3). Queue is expected to inherit this, but it is not explicitly closed for Queue. **UNDECIDED-to-RESOLVED** — recommend closing as no-barging for consistency. |
| D25 | scheduler run order / completion order | may differ from wake order | **RESOLVED** — scheduler run order is a Worker/E8 concern, not a primitive guarantee. FIFO wake ≠ FIFO completion (plan §3.6). |
| D26 | starvation freedom | not guaranteed by safety-only models | **RESOLVED** (negative) — Semaphore/Mutex formal models are safety-only; liveness is out of scope for the same reason (external producer fairness unjustified). |

> **Do not claim FIFO fairness merely because `std::deque`/`WaitQueue` is
> FIFO.** The FIFO here is *waiter-selection* FIFO and *buffer-order* FIFO,
> which ARE proven by the `WaitQueue` discipline. Completion order is NOT FIFO.

### E.7 Introspection (Phase 3.7)

| Op | Snapshot semantics | Locking cost | TOCTOU risk | Deadlock-in-callback risk | Recommendation |
| --- | --- | --- | --- | --- | --- |
| `size()` | observational snapshot, may stale (Semaphore A5) | one `q.mtx()` acquire | high if used to decide push/pop | low (no callback) | diagnostics/test-only or clearly-documented "may stale" |
| `empty()` | derived from size==0 | as above | high | low | as above |
| `full()` | bounded only; size==capacity | as above | high | low | as above |
| `waiting_producers()`/`waiting_consumers()` | queue length snapshot | one `q.mtx()` | medium | low | test/diagnostic only |
| `is_closed()` | atomic load | none | low (monotonic) | none | safe in production |

**No primitive today exposes waiter counts in its public surface.** Following
that precedent, these should be **test/diagnostic-only** (via
`AsyncTestAccess`-style accessors in the testing variant), not public API.

---

## F. Preliminary state-machine map (Phase 4)

### F.1 Necessary state dimensions

| Dimension | Necessary? | Derivable? | Source |
| --- | --- | --- | --- |
| `closed` (bool) | **Yes** | no | plan §8.1, §11.5 |
| buffer contents (FIFO seq of T) | **Yes** | no | plan §8.1 |
| capacity (bound) | **Yes if bounded** (D1) | no | plan §8.1 |
| producer waiters (not-full `WaitQueue`) | **Yes if bounded** (absent for unbounded — no backpressure) | no | plan §8.1; Check 8 |
| consumer waiters (not-empty `WaitQueue`) | **Yes** | no | plan §8.1 |
| waiter admission phase | **Yes** (or collapsed — see below) | no | Semaphore models it explicitly (`admissionPhase`); Mutex collapses it (Registered=Suspended). Either convention is viable. |
| waiter completion state (`nodeState`) | **Yes** | no | E10/E11/E12 precedent |
| deadline registration | **Yes** (via E11 `TimerRegistration`) | composed, not new | E11 |
| cancellation state | **Yes** (via `resolve_(cancelled)`) | composed | E10 |
| **reserved slots** (producer side) | **Yes — EXPLICIT, under grant semantics only** (B/C) | no | plan §8.3, §14.3.4 (conditional — §D.4) |
| **reserved items** (consumer side) | **Yes — EXPLICIT, under grant semantics only** (B/C) | no | plan §8.3, §14.3.4 (conditional — §D.4) |
| selected producer / selected consumer | **Yes** (transient, before commit) | no | plan §3.5, §4 |
| payload ownership | **Yes** | no | plan §3.5 |
| runnable publication state | **Yes** (history/ghost) | ghost | Mutex/Semaphore precedent |

**Derivable/optional:** `size` is derivable from buffer length; `empty`/`full`
from size and capacity; `is_closed` is the `closed` bool.

### F.2 Dangerous transient states (Phase 4 risk inventory)

For each: triggering race → potentially invalid state → required linearization/
ownership rule → existing-infrastructure coverage → new seam needed?

| # | Dangerous transient state | Triggering race | Invalid state if unresolved | Required rule | Existing infra solves? | New seam? |
| --- | --- | --- | --- | --- | --- | --- |
| R1 | consumer registered while an item is available | consumer admission races a producer push | lost item / stranded consumer | admission recheck under lock (consume-or-register atomically) | **Yes** — Event/Semaphore admission idiom (§D.1) | No |
| R2 | producer registered while capacity is available | producer admission races a consumer pop | lost slot / stranded producer | symmetric admission recheck | **Yes** | No |
| R3 | closed queue with admitted producer | close races push commit | item enqueued after close linearized | close monotonic; push-after-close rejected at linearization | **Partial** — close linearization is new | **Yes — close seam + linearization rule** |
| R4 | closed-and-empty queue with stranded consumer | close races a waiting consumer | consumer waits forever after close | close wakes blocked consumers (D7) | **No** — close is new | **Yes — close-wake** |
| R5 | buffered item + selected direct-handoff receiver | direct handoff races buffer state | item delivered twice (buffer + handoff) | handoff and buffer are mutually exclusive for a given item | **No** — direct handoff is new (D15) | **Yes if D15=handoff** |
| R6 | reserved slot without owning producer | producer cancel CAS wins before the grant path's resolve_(Woken) CAS (Case A) | The grant path's resolve_ loses; no reservation is established. The slot stays available. | The existing loser semantic prevents the grant from occurring at all — no explicit "refund" logic is needed. | **Yes** — loser semantic (§D.2) covers this. | No |
| R7 | reserved item without owning consumer | consumer cancel CAS wins before the grant path's resolve_(Woken) CAS (Case A) | The item stays in the buffer for another consumer. | Symmetric to R6. | **Yes** — loser semantic. | No |
| R8 | waiter selected (grant CAS won) but late timeout tries to expire it | expire CAS arrives after the grant's resolve_(Woken) already won (Case B) | The expire CAS is the loser (no-op). The selected waiter keeps its grant. | Grant final (CAS winner final); late timeout cannot revoke. | **Yes** — loser semantic (§D.2). | No |
| R9 | waiter timed out (expire CAS won) but grant tries to resolve Woken later | grant resolve_(Woken) CAS arrives after expire already won (Case A) | The grant CAS loses; the item/slot stays in the buffer. No double-delivery. | The loser semantic prevents the grant from establishing a reservation. | **Yes** — loser semantic. | No |
| R10 | cancelled producer whose slot might appear reserved | cancel CAS wins before the grant path's resolve_(Woken) (Case A) | The node is unlinked; the grant path never sees an eligible waiter. The slot stays available. | The loser semantic prevents the grant. | **Yes** — loser semantic. | No |
| R11 | cancelled consumer whose item might appear reserved | symmetric to R10 (Case A) | The item stays in the buffer. | Symmetric. | **Yes** — loser semantic. | No |
| R12 | payload moved from producer but not owned by queue or consumer | move-throw or crash mid-transfer (Case D — payload failure after grant) | ownerless payload | exception guarantee (§D.6) — **not a wait-resolution problem** | **No** — no payload primitive exists; the cancel/expire loser semantic does not address this. | **Yes — exception rule** |
| R13 | waiter published runnable before result storage complete | publication-before-commit | consumer resumes, reads empty result | commit-before-publication (Mutex precedent) | **Pattern exists** (`mutex_handoff_one_locked`) | **Yes — Queue-specific commit seam** |
| R14 | queue destruction with registered waiters | dtor races in-flight ops | use-after-free | destruction contract (§D.7) | **No** — contract is new | **Yes — destruction contract decision** |
| R15 | close racing with timeout | close races expire | double-resolution | single `resolve_` CAS + `global_mtx_` serialization | **Yes** | No |
| R16 | close racing with successful push | close races push commit | item enqueued after close | close linearization vs push commit | **No** | **Yes — linearization rule** |
| R17 | close racing with successful pop | close races pop | item consumed after close-and-empty | close linearization vs pop | **No** | **Yes** |

**Summary (corrected for Check 2 — four-case separation):** R1, R2, R6, R7,
R8, R9, R10, R11, R15 are covered by existing infrastructure (admission idiom
+ loser semantic; the loser semantic handles both Case A — cancel/expire wins
before grant — and Case B — grant wins before cancel/expire). R3, R4, R5, R12,
R13, R14, R16, R17 require **new Queue-specific seams and rules** — primarily
the **winner-before-publication commit seam** (R5, R13 — grant semantics only),
the **close lifecycle** (R3, R4, R16, R17), **exception rule** (R12 — Case D,
payload failure after grant), and **destruction contract** (R14).

---

## G. Candidate invariant catalogue (Phase 5)

**Non-authoritative** — these are candidate proof targets for the eventual
formal model, drawn from plan §11.5 and the patterns in
`docs/spec/e12_semaphore/` and `docs/spec/e12_async_mutex/`.

### G.1 Candidate invariants (safety/lifecycle/API-contract)

Invariants are classified by transfer model: **ALL** = applies under all viable
designs (A, B, C); **GRANT** = applies only under grant semantics (B, C);
**HANDOFF** = applies only under direct handoff (C).

| Candidate invariant | Plain-language meaning | Necessary state variables | Likely violating transition | Class | Model scope |
| --- | --- | --- | --- | --- | --- |
| `0 ≤ buffer_size ≤ capacity` (bounded form) | buffer never over/underflows | buffer, capacity | push to full / pop from empty | safety | **ALL** |
| `closed ⇒ no new producer may commit` | close is monotonic; post-close push rejected | closed, push action | push commit after close linearizes | safety/lifecycle | **ALL** |
| `closed ∧ buffer_empty ⇒ no consumer may successfully acquire an item` | closed-and-empty means no more data | closed, buffer | pop succeeding after close+empty | safety | **ALL** |
| every reserved slot has exactly one owning producer | no orphan slots | reserved slots, producer epochs | (Case A: cancel wins before grant — loser semantic prevents it; Case B: grant wins first — no refund needed) | safety | **GRANT** |
| every reserved item has exactly one owning consumer | no orphan items | reserved items, consumer epochs | symmetric | safety | **GRANT** |
| no payload has two owners | single ownership | payload ownership | handoff + buffer both hold item | safety | **ALL** |
| no payload is ownerless after source ownership relinquished | no leak | payload ownership | move-throw mid-transfer (Case D) | safety/exception | **ALL** |
| a waiter completes at most once | exactly-once resolution | nodeState | double resolve_ | safety | **ALL** (covered by `InvSingleResolution`) |
| a waiter cannot both time out and succeed | loser semantic | nodeState | expire after grant (Case B: loser is no-op) | safety | **ALL** (loser semantic) |
| a waiter cannot both cancel and succeed | loser semantic | nodeState | cancel after grant (Case B: loser is no-op) | safety | **ALL** |
| a runnable waiter has a complete published result | commit-before-publication | runnablePublished, result | publish before result write | safety | **GRANT** (`InvGrantPublicationCoupling` analog) |
| a removed waiter cannot later be selected | unlink finality | queue, nodeState | select unlinked node | safety | **ALL** |
| successful pop preserves queue element order | FIFO buffer | buffer | out-of-order pop | safety | **ALL** |
| successful push appears exactly once | no duplication | buffer, handoff | item in both buffer and handoff | safety | **HANDOFF** |
| close is monotonic | once closed, stays closed | closed | reopen / un-close | lifecycle | **ALL** |
| destruction leaves no externally resumable waiter referencing destroyed state | no use-after-free | waiters, destroyed | dtor with in-flight ops | lifecycle | **ALL** (covered by `InvDestructionPrecondition` analog) |

### G.2 Tempting-but-INVALID invariants (fail during transient admission)

| Invalid invariant | Why it fails |
| --- | --- |
| `waiting consumer ⇒ queue empty` | A consumer may be registered-and-not-yet-suspended, or registered while a producer is mid-push-commit; transiently both a waiter and a non-empty buffer can coexist. |
| `waiting producer ⇒ queue full` | Symmetric transient during admission or during a consumer's pop-commit. |
| `buffer non-empty ⇒ no waiting consumer` | A consumer may be in the admission window (registered, not yet resolved) while the buffer is non-empty. |
| `available capacity ⇒ no waiting producer` | Symmetric transient. |
| `wake order == completion order` | Scheduler run order differs from wake order (E8/Worker scheduling). |

These must NOT be asserted; the valid forms are the *stable-state* implications
(e.g. `EligibleQueuedConsumerExists ⇒ buffer empty` holds only at stable
states, mirroring Semaphore's `InvNoIdlePermitWithEligibleWaiter`).

---

## H. Linearization-point matrix (Phase 6)

For every prospective externally-visible outcome. "Candidate LP" = the moment
the operation becomes externally effective. **Multi-stage transitions are NOT
hidden behind "during wakeup."**

| Operation outcome | Candidate linearization point | Payload ownership before | Payload ownership after | Competing races | Unresolved issue |
| --- | --- | --- | --- | --- | --- |
| successful non-blocking push (buffer has space, no waiting consumer) | buffer insertion (emplace/move into buffer) under structural lock | producer | queue (buffer) | racing pop, racing close | if D15=handoff and a waiting consumer exists, the LP is slot-reservation+commit, not buffer insertion — **depends on D15** |
| successful non-blocking push to a waiting consumer (direct handoff) | consumer-epoch selection + item commit (before consumer published runnable) | producer | consumer (via reservation) | racing cancel/expire of the consumer, racing close | **winner-before-publication commit seam required**; item bound to epoch, not buffer |
| successful blocked push (producer was queued, then slot granted) | slot reservation for the producer epoch (before producer published runnable) | (producer was suspended) | queue/consumer | racing timeout/cancel of the producer, racing close | **producer-side commit seam**; D18 (slot reserved before payload move?) |
| failed push due to close | close-linearization observes the push as post-close | producer (retained) | producer (retained) | racing push commit | **close vs push LP order** (R16) — UNDECIDED |
| timed-out push | expire `resolve_` CAS wins before the grant path's attempt (Case A) | producer | producer | racing grant | The loser semantic prevents the grant from establishing a reservation. No slot-refund is needed: the slot was never reserved. Under grant semantics, the slot stays available for another producer. |
| cancelled push | cancel `resolve_` CAS wins before the grant path's attempt (Case A) | producer | producer | racing grant | Symmetric to timed-out push. The loser semantic prevents the grant. |
| successful buffered pop (buffer non-empty) | item removal from buffer under structural lock | queue (buffer) | consumer | racing push, racing close, racing other consumer | if a waiting producer exists, popping makes space → producer handoff; **coupled LP** |
| successful direct-handoff pop (consumer was queued, item handed off) | item reservation to consumer epoch (before consumer published runnable) | queue/producer | consumer (reservation) | racing cancel/expire, racing close | **consumer-side commit seam** (R13) |
| failed pop due to closed-and-empty | close-linearization observes empty buffer + closed | n/a | n/a | racing push | depends on D5/D7 (drain vs discard) — UNDECIDED |
| timed-out pop | expire `resolve_` CAS wins before the grant path's attempt (Case A) | n/a (no item reserved) | n/a | racing grant | The loser semantic prevents the grant. No item-return needed: the item was never reserved. It stays in the buffer for another consumer. |
| cancelled pop | cancel `resolve_` CAS wins before the grant path's attempt (Case A) | n/a | n/a | racing grant | Symmetric. The item stays in the buffer. |
| close | `closed` bool store under structural lock (monotonic) | n/a | n/a | racing push/pop, racing waiter admission | **LP definition + wake-all rule** (D5/D7) — UNDECIDED |
| destruction/shutdown | debug-assert empty (caller contract) — no graceful shutdown via dtor | n/a | n/a | in-flight ops | **destruction contract** (§D.7) — UNDECIDED |

**Key unresolved theme:** every grant-bearing outcome (handoff push, blocked
push, handoff pop — transfer models B/C of §D.4) requires the
**winner-before-publication commit seam**, and every close-racing outcome
requires a **close linearization rule** that does not yet exist. Under
**wake-and-retry (model A)** the handoff/reservation rows do not arise — a
woken waiter rechecks the buffer and retries, so the LP is the buffer mutation
itself and no winner identity is needed before publication. Whether E12-E is
A, B, or C is itself open (§E.5 D15). These are the two largest specification
tasks *conditional on the transfer-semantics choice*.

---

## I. Risk register (Phase 12, classified)

> **Classification legend (Check 3 of the corrective).** The original draft
> labelled many items `P0 — blocks specification`. That conflated the gates of
> §A. An issue is `P0 — blocks specification` **only if it blocks the *start*
> of specification work** (i.e. missing repository evidence). Issues that the
> specification *exists to settle* are reclassified as
> `P0 — must be resolved by the semantic specification before that
> specification can be declared authoritative` (blocks **finalization**, not
> start) or `P0 — blocks implementation, not specification work`. No item
> below blocks the SPECIFICATION-START gate; that gate is satisfied (§A).

| ID | Risk | Classification | Exact evidence | Falsifiable corrective condition |
| --- | --- | --- | --- | --- |
| Q-F-1 | Close-lifecycle cluster (D5/D7/D9/D14) unsettled | **P0 — must be resolved by the semantic specification before it can be declared authoritative** (blocks finalization; does NOT block specification start) | plan §8.2 marks `HUMAN DECISION REQUIRED` | The specification records a decision selecting drain-on-close or immediate-discard, with the coupled D7/D9/D14 consequences traced. |
| Q-F-2 | Exception/element-type model (§D.6 Q7.1–Q7.4) and the `T`-under-internal-lock rule (§D.6.1) unsettled | **P0 — must be resolved by the semantic specification before it can be declared authoritative** (blocks finalization; under grant semantics also interacts with reservation design) | no payload-bearing primitive exists (`wait_node.hpp:237` holds only `Fiber*`); no rule settles `T`-ops under a Queue internal lock | The specification records decisions on nothrow-move, the `T`-under-lock rule (§D.6.1(b)), and throw-after-selection handling. |
| Q-F-3 | Bounded vs unbounded (D1) unsettled | **P0 — must be resolved by the semantic specification before it can be declared authoritative** (blocks finalization; changes the state space) | plan §8.2 `HUMAN DECISION REQUIRED` | The specification records a decision. |
| Q-F-4 | Concurrency topology (D4) and transfer model (D15: wake-and-retry A / grant B / handoff C) unsettled | **P0 — must be resolved by the semantic specification before it can be declared authoritative** (blocks finalization; determines whether reservation/commit-seam apply at all) | no authority fixes topology; plan §8.3/§14.3.4 state reservation/commit-seam **conditionally** on grant semantics | The specification records decisions on topology and transfer model, and derives whether reservation state / commit seam are needed. |
| Q-I-1 | Winner-before-publication commit seam for item/slot reservation not designed (grant semantics B/C only) | **P0 — blocks implementation, not specification work** (and only if Q-F-4 selects grant/handoff) | `wake_wait_one` returns `bool` (`scheduler.hpp:255`); Mutex precedent `mutex_handoff_one_locked` (`scheduler.hpp:558`) returns `WaitNode*`; plan §8.3:1348-1353 and §14.3.4:1930-1931 state the requirement **conditionally** | A `queue_recv_handoff_one_locked`/`queue_send_handoff_one_locked` design exists (only if grant/handoff chosen), with the commit body specified, holding both locks, committing before `make_runnable`. |
| Q-I-2 | User-defined `T` operations under any Queue internal lock (not just `global_mtx_`) | **P0 — blocks implementation, not specification work** (the rule is part of the spec, but the *mechanism* blocks impl) | repo rule "no user code under `global_mtx_`" is PROVEN (§D.5); whether `T`-ops may run under a Queue internal lock is UNRESOLVED (§D.6.1(b)) | The specification records the `T`-under-lock rule; the implementation honors it. |
| Q-I-3 | Destruction contract not closed | **P0 — must be resolved by the semantic specification before it can be declared authoritative** (destructor precondition is part of the authoritative contract) — independent of the close-policy choice (Check 7) | plan does not explicitly close Queue destruction (§D.7) | The specification records a destruction contract (recommended: caller-contract-violation debug assert, matching precedent), independent of close policy. |
| Q-P2-1 | Naming collision (`Queue` vs `WaitQueue`) | **P2 — required before closure** | `WaitQueue` in `sluice::async` (`wait_queue.hpp:119`) | A name chosen (recommended `AsyncQueue` for consistency). |
| Q-P2-2 | Introspection API surface (E.7) not decided | **P2 — required before closure** | no precedent for public waiter-count accessors | A decision recorded (recommended test/diagnostic-only). |
| Q-P2-3 | `closed` outcome representation (new 4th outcome vs separate result type) | **P2 — required before closure** | `WaitOutcome` has only `woken/cancelled/expired` (`wait_node.hpp:88-98`) | A result-model decision recorded. |
| Q-P3-1 | Formal model scope (which of Models A–D in first deliverable) | **P3 — optional improvement** | §K.2 | A model plan recorded. |

---

## J. Testing and seam plan (Phase 9)

### J.1 Required deterministic race coverage (tests NOT added in this audit)

> **Conditionality note:** races involving handoff/reservation commit seams
> (marked with **control**) are required only under grant/handoff semantics
> (B/C). Under wake-and-retry (A) they do not arise. The `close_before_drain`
> control is generally relevant to any Queue with close.

| Race to cover | Seam type needed | Can it use existing seams? |
| --- | --- | --- |
| producer admission before queue lock | observation/control | existing E7 admission seam pattern |
| producer registered before scheduler handoff | observation/control | existing admission-window seam pattern |
| producer selected before slot reservation | **control** | **new — `queue_send_handoff_before_publication`** (mirrors `e12_mutex_handoff_before_publication`) |
| producer payload commit before runnable publication | control | new — same seam |
| consumer admission before queue lock | observation | existing pattern |
| consumer registered before scheduler handoff | observation | existing pattern |
| consumer selected before item reservation | **control** | **new — `queue_recv_handoff_before_publication`** |
| consumer result write before runnable publication | control | new — same seam |
| close before waiter drain | control | **new — `queue_close_before_drain`** (mirrors `e12_set_store_before_drain`) |
| close after waiter selection | control | new |
| timeout before waiter removal | control | existing E11 clock seam (`advance_clock`) |
| timeout after waiter selection | control | existing + new commit-window seam |
| cancel before waiter removal | control | existing cancel path + retry-loop pattern |
| cancel after waiter selection | control | existing + new commit-window seam |
| direct handoff before payload move | control | new (only if D15=handoff) |
| buffer insertion before consumer drain | observation | existing |
| buffer removal before producer drain | observation | existing |

### J.2 Seam classification (Phase 9 vocabulary)

| Seam | Classification | Rationale |
| --- | --- | --- |
| `queue_recv_handoff_before_publication` | **PRODUCTION SEMANTIC SEAM** (test-gated) — required only under grant/handoff semantics (B/C) | The commit-before-publication ordering is load-bearing production semantics; the seam observes/controls it. Mirrors `e12_mutex_handoff_before_publication` (`scheduler.cpp:2089`). |
| `queue_send_handoff_before_publication` | **PRODUCTION SEMANTIC SEAM** (test-gated) — required only under grant/handoff semantics (B/C) | Symmetric. |
| `queue_close_before_drain` | **PRODUCTION SEMANTIC SEAM** (test-gated) | close-store-before-drain ordering is load-bearing under any Queue with close (mirrors `e12_set_store_before_drain` `:1331`). |
| admission-window markers | **TEST-ONLY OBSERVATION SEAM** | non-controlling markers |
| E11 clock-driven timeout races | **TEST-ONLY CONTROL SEAM** | existing `advance_clock` |

**Constraint application (per Phase 9 rules):** test-only controllers must NOT
appear in installed public headers, grant forgeable authority, allocate/invoke
callbacks on the hot path, change lock ordering, or alter semantics when
disabled. The `ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1` banner
(`scheduler.hpp:38-57`) enforces this: seams live in the non-installed
`sluice_async_internal_testing` variant (`xmake.lua:70-86`), compiled with
`SLUICE_ASYNC_INTERNAL_TESTING`, gated by `#if defined(...)` at each call site.
The new Queue seams must follow this **exactly**.

### J.3 Existing reusable seams

* **E11 logical clock** (`AsyncTestAccess::enable_test_clock`/`set_clock`/
  `advance_clock`, `scheduler.hpp:1091-1126`) — for timeout races.
* **Retry-loop + barrier pattern** (e.g. `e12_event_test.cpp:713-752`) — for
  CAS races (cancel-vs-wake).
* **External-thread + `run_live(1)` pattern** (e.g. `e12_t27`,
  `e12_event_test.cpp:1858-1930`) — for causal proofs that pause a worker
  holding `global_mtx_`.
* **Authority NEG compile probes** (one per primitive, `tests/e12_*_authority_
  probe.cpp`) — to prove the Queue's `wait_queue()` / reservation internals are
  inaccessible from production code.

### J.4 Tests writable without new seams

* basic FIFO ordering (single producer/consumer);
* bounded capacity enforcement;
* close-then-push-rejected (D8 resolved);
* try_push/try_pop no-barging (mirrors Semaphore tests);
* timeout/cancel basics (via E11 clock + existing cancel path).

---

## K. Formal-model recommendation (Phase 10)

### K.1 Feasibility determinations

* **Separate producers/consumers?** **Yes** — the contention topologies
  (producer vs consumer, close vs both) require distinct roles. Mirror the
  Mutex "epoch" model where each Node/Epoch is one wait epoch.
* **Minimum counts:** **2 producers, 2 consumers, capacity 2, 2 values** —
  sufficient to express: FIFO buffer order, not-full + not-empty queues both
  non-empty, close racing push and pop, direct handoff vs buffer. (Semaphore
  uses 3 nodes/MaxPermits 2-3; Mutex 3 epochs; Queue needs *both* sides.)
* **Buffer order modeled?** **Yes** — `buffer : Seq(Value)` is a necessary
  dimension (FIFO pop order is a target invariant).
* **Direct handoff modeled?** **Only if D15=handoff.** If first-scope is
  buffer-first only, defer handoff to a later model (Model D below).
* **Admission phases explicit?** **Either convention viable** — Semaphore
  models `admissionPhase` explicitly; Mutex collapses it. Recommend Mutex-style
  collapse (Registered=Suspended) for simplicity unless admission-closure
  invariants prove tautological without it.
* **Deadlines/cancellation in first model?** **No** — defer to Model C. The
  loser semantic is already proven by Semaphore/Mutex/Event/E11; re-proving it
  for Queue adds state-space cost without new insight. First model should be
  safety-only without timeout/cancel.
* **Close in first model?** **Yes** — close is load-bearing and novel; it must
  be in the *first* model (Model B at latest, arguably Model A).
* **Negative models necessary?** **Yes** — see K.3.

### K.2 Staged formal-model plan (FEASIBILITY RECOMMENDATION ONLY — not
authorization to implement)

```text
Model A — bounded queue safety without timeout/cancel/close
    State: buffer (Seq Value), capacity, not-full queue (if bounded),
           not-empty queue, nodeState per epoch, runnablePublished,
           history ghosts. ADD reserved-slots / reserved-items ONLY under
           grant semantics (B/C); under wake-and-retry (A) these dimensions
           and their invariants do not arise.
    Target invariants: buffer bounds, single-resolution, single-publication,
        FIFO buffer order. UNDER GRANT SEMANTICS ALSO: grant-commit-coupling
        (item bound before publication), no-double-item (buffer ∩ reserved =
        ∅), no-ownerless-reservation.
    Negative controls: UNDER GRANT SEMANTICS: publish-before-result-write;
        item-in-both-buffer-and-reserved; reserved-slot-without-owner.

Model B — close and drain semantics (adds closed bool + close action)
    Adds: closed, close-vs-push LP, close-vs-pop LP, close-wake rule (D5/D7).
    Target invariants: close monotonic; no-commit-after-close;
        closed-and-empty ⇒ no successful pop (per D5/D7).
    Negative controls: push-commit-after-close; pop-after-close-and-empty;
        wake-only-one-side-on-close.

Model C — timeout/cancellation races (adds E11 deadline + cancel causes)
    Re-uses the proven loser semantic. The four-case separation (§D.4) applies:
    Case A (cancel/expire wins the resolve_ CAS before the grant path attempts
    it): the grant path's resolve_(Woken) loses, and no reservation is
    established. Case B (grant resolve_(Woken) wins first): the late
    cancel/expire CAS is the loser and does nothing. The target is to verify
    that the loser semantic is sufficient for both cases — no additional
    "refund after completed reservation" logic is needed.
    Negative controls: grant-path commits reservation after cancel/expire
        already won (Case A violation — the loser must prevent the commit);
        cancel/expire revokes a reservation after the grant won (Case B
        violation — the loser must be a no-op).

Model D — direct producer-to-consumer handoff (only if D15=handoff/C)
    Adds: direct transfer path, handoff-buffer mutual exclusion (R5).
    Negative controls: item-in-both-buffer-and-handoff; handoff-after-close.
```

### K.3 Likely negative controls

```text
publish runnable before writing the consumer result      → InvGrantPublicationCoupling (grant only)
grant path commits reservation after cancel/expire       → InvAdmissionClosure (Case A: the loser
  already won                                                    semantic must prevent the commit)
cancel/expire revokes a reservation after the grant      → InvGrantFinality (Case B: late loser
  already won                                                     must be a no-op)
allow push commit after close linearizes                  → InvCloseMonotonic
duplicate an item across direct handoff and buffer        → InvNoDoubleItem (handoff only)
wake only one side during close                           → InvCloseDrainComplete
omit the admission phase                                   → InvAdmissionClosure
treat selection as completion                              → InvGrantCommitCoupling (grant only)
```

### K.4 Safety-only stance (recommended)

Following Semaphore/Mutex/Condition, the Queue model should be **safety-only**.
Consumer wake depends on an external producer enqueue, so producer fairness
would be unjustified (same reasoning as Semaphore's removed liveness property,
`docs/spec/e12_semaphore/README.md:153-159`). Liveness would only be justified
if Queue owned an internal multi-step protocol that must drain (like Event's
broadcast) — close-and-wake-all is a candidate, but it can be specified as a
finite atomic drain rather than a fairness property.

---

## L. Required document sequence (Phase 11)

### L.1 Required documents (gate mapping)

The documents below are what the specification phase must produce. The
"Blocks" column uses the three gates of §A: **START** (specification drafting
may begin without this), **FINAL** (specification cannot be declared
authoritative without this), **IMPL** (implementation cannot begin without
this). Drafting (START) can begin immediately — the documents are the vehicle
by which the open decisions get settled, not a prerequisite to drafting them.

| # | Document | Purpose | Decisions it must settle | Blocks |
| --- | --- | --- | --- | --- |
| 1 | E12-E semantic contract | the authoritative semantic authority (like `e12-semaphore.md` §3) | bounded/unbounded, topology, transfer model (A/B/C), close cluster, destruction | **FINAL** |
| 2 | public API specification | operation names, signatures, result model, naming | operation set (E.2), naming, result/outcome representation (closed vs expired vs cancelled) | **FINAL** |
| 3 | state-machine design | the state dimensions (F.1), transitions, reservation representation (grant-only) | admission-phase convention, reservation-state representation (if grant), commit-seam design (if grant) | **FINAL / IMPL** |
| 4 | scheduler integration design | the `Scheduler::queue_*` seams | the exact `queue_*` admission/cancel/close seams; the `queue_*_handoff_one_locked` signatures **only if grant/handoff (B/C) chosen** | **IMPL** |
| 5 | timeout/cancellation contract | refund/return rules (R6/R7/R10/R11) — grant-only | slot-refund, item-return semantics (or none, under wake-and-retry) | **FINAL (grant) / IMPL** |
| 6 | close contract + destruction contract (separate — Check 7) | D5–D14 + destruction (§D.7) | close cluster; destruction contract (independent) | **FINAL** |
| 7 | exception and element-type requirements + T-under-lock rule | §D.6 / §D.6.1 | nothrow-move?, the `T`-under-internal-lock rule (§D.6.1(b)), throw-after-selection handling | **FINAL / IMPL** |
| 8 | deterministic test plan | J.1 race coverage, seam list | the new seams (J.2) | **IMPL** |
| 9 | TLA+ model plan | K.2 staged models | model scope, negative list | **IMPL** |
| 10 | negative-model plan | K.3 | per-model expected invariant | **IMPL** |
| 11 | implementation sequencing document | build order | order of: seams → fast paths → slow paths → close → deadline/cancel → (handoff if chosen) | **IMPL** |
| 12 | closure checklist | sign-off | review gates, formal gate, authority probe, sanitizer runs | **IMPL** |

### L.2 Recommended creation order

```text
Specification work (START) may begin immediately — the open decisions are
settled BY the documents below, not before drafting them.

1. Begin drafting the E12-E semantic contract (doc 1) — record the open
   decisions as decisions-to-be-taken (§E), and resolve them within the draft.
2. Exception & element-type requirements + T-under-lock rule (doc 7) — feed
   doc 3.
3. State-machine design (doc 3) — depends on 1 and 7; reservation/commit-seam
   sections are written only if grant/handoff is selected in doc 1.
4. Scheduler integration design (doc 4) — the admission/cancel/close seams
   always; the handoff seams only if grant/handoff chosen.
5. Public API specification (doc 2).
6. Close contract (doc 6a) and destruction contract (doc 6b) — separate.
7. Timeout/cancellation contract (doc 5) — grant-only rollback rules.
8. TLA+ model plan (doc 9) + negative-model plan (doc 10).
9. Deterministic test plan (doc 8).
10. Implementation sequencing (doc 11).
11. Closure checklist (doc 12).

Specification FINALIZATION is reached when docs 1,2,3,6,7 have resolved every
open decision. IMPLEMENTATION (a separate authorization act, §M) additionally
requires docs 4,5,8–12 and the closed decision cluster.
```

---


## N. Mandatory audit questions (answered)

1. **Does any production queue abstraction already exist?** **No.** (§C)
2. **Is E12-E intended to be bounded, unbounded, or undecided?** **Undecided
   (HUMAN-DECISION-REQUIRED).** Plan §8.2. (§E.1 D1)
3. **Is the intended concurrency topology known?** **Undecided.** (§E.1 D4)
4. **Are both producers and consumers allowed to suspend?** **Consumers: yes,
   by design** (the plan requires "not-empty waiters", plan §8.1, a `WaitQueue`
   whose waiters suspend). **Producers: only if bounded** — an unbounded queue
   has no not-full condition, so producers never block; a bounded queue has
   "not-full waiters" (plan §8.1) whose producers suspend. (§D.1, Check 8)
5. **Is direct producer-to-consumer handoff required?** **Undecided.** Rendezvous
   (capacity-0) is DEFERRED (D3); bounded-with-handoff (D15) is not closed.
   (§E.5)
6. **Probable payload ownership model?** **Undecided in detail, and conditional
   on the transfer model (§D.4 A/B/C).** Under grant semantics (B/C):
   producer owns until commit → queue/consumer owns after, with explicit
   reservation state. Under wake-and-retry (A) there is no per-waiter
   reservation: ownership transfers at the buffer mutation. (§E.5, §F.1)
7. **Likely linearization point for push?** **Undecided between**
   buffer-insertion (buffer-first) **and** slot-reservation+commit (handoff).
   (§H)
8. **Likely linearization point for pop?** **Undecided between**
   buffer-removal (buffer-first) **and** item-reservation+commit (handoff).
   (§H)
9. **What does close mean?** **Undecided** — the cluster D5/D7/D9 is open.
   Close monotonicity is a target invariant (plan §11.5). (§E.4)
10. **Are buffered items drained after close?** **Undecided (HUMAN-DECISION).**
    (§E.4 D5)
11. **What happens to blocked producers on close?** **RESOLVED: wake with
    `closed` outcome; `send` returns error.** (plan §8.2) (§E.4 D6)
12. **What happens to blocked consumers on close?** **Undecided
    (HUMAN-DECISION),** coupled to D5. (§E.4 D7)
13. **Can timeout, cancellation, close, and success race?** **Yes.** Timeout/
    cancel/success races are resolved by the loser semantic (§D.2) with the
    four-case separation of §D.4: (Case A) cancel/expire wins the resolve_ CAS
    before the grant path attempts it — no reservation is established; (Case B)
    grant CAS wins first — late cancel/expire is the loser (no-op). **Close
    races are new** and require linearization rules (R3, R4, R16, R17). (§F.2)
14. **Which authority resolves those races?** Timeout/cancel/success: the single
    `resolve_` CAS + `global_mtx_` serialization (§D.2). Close races: **no
    authority yet — must be specified.** (§H)
15. **Can a selected waiter still be cancelled or timed out?** **No, not once
    the grant's `resolve_` CAS won** — this is Case B of the four-case
    separation (§D.4): the loser semantic makes a late cancel/expire a no-op
    (§D.2). Under grant semantics (B/C) the reservation is committed *before*
    publication so a "selected" waiter is already a CAS winner. Under
    wake-and-retry (A) there is no selection-with-reservation, so the question
    of revoking a reservation does not arise. (§F.2 R8)
16. **Are producer and consumer waiters FIFO?** **Yes** — both are `WaitQueue`s
    (intrusive FIFO; `wait_queue.hpp:319-321`). (§E.6 D22, D23)
17. **Does FIFO waiter order imply FIFO completion?** **No** — scheduler run
    order differs from wake order. (§E.6 D25)
18. **May payload movement execute user code under a runtime lock?** **Under
    `global_mtx_`: NO (PROVEN repo rule). Under any Queue internal
    (structural) lock: UNRESOLVED — do not presume it is acceptable.** A Queue
    internal lock is still a runtime lock; user `T` ops may throw/block/alloc/
    re-enter/acquire. No authority settles this. (§D.6, §D.6.1, §D.5)
19. **What happens if payload movement throws?** **Undecided — this is Case D
    of the four-case separation (§D.4): payload failure after grant.** The
    cancel/expire loser semantic (which handles Cases A and B) does NOT address
    this. It is coupled to the nothrow-move requirement (§D.6 Q7.1) and the
    `T`-under-internal-lock rule (§D.6.1(b)). (§D.6 Q7.4)
20. **What queue element-type restrictions are likely necessary?** **Likely
    move-only, nothrow-move-or-throw-handled, object types only** — but all
    UNDECIDED. (§D.6)
21. **What is the destruction contract?** **Undecided;** recommended
    caller-contract-violation debug assert (precedent). (§D.7)
22. **Can a timer or callback access queue state after destruction?** **Not
    through the registration** (E11 `TimerRegistration` is Scheduler-owned and
    retired in the winner CS); **but a fiber resuming after dtor would
    use-after-free** — the destruction contract must prevent this. (§D.7)
23. **Which existing E12 primitive is most reusable?** **AsyncMutex** — for the
    `mutex_handoff_one_locked` winner-before-publication commit-seam template
    (relevant under grant semantics B/C; under wake-and-retry A the Event/
    Semaphore admission idiom is the primary reuse). (§D.4)
24. **Which existing E12 primitive would be dangerous to copy directly?**
    **None is dangerous per se, but** copying Semaphore's release-transfer
    (anonymous, no winner-aware commit) as Queue's handoff would be dangerous
    **under grant semantics** — a grant-bearing Queue needs the winner-aware
    commit that Semaphore deliberately avoided. Also: do NOT generalize any
    seam into a generic callback framework (plan §14.4 forbids
    `UniversalGrant`/`ResourceGrantFramework`/`Waitable`). (§D.4)
25. **Are new scheduler APIs likely required?** **Yes** — `queue_*`
    admission/cancel/close methods mirroring the
    `event_*`/`sem_*`/`mutex_*`/`condition_*` pattern, always. The two private
    handoff seams (`queue_recv_handoff_one_locked`,
    `queue_send_handoff_one_locked`) are required **only under grant/handoff
    semantics (B/C)**; under wake-and-retry (A) they are not. (§D.4, §J.2)
26. **Are new phase tags likely required?** **Yes for close; conditionally for
    handoff.** `queue_close_before_drain` (test-gated production semantic seam)
    is generally relevant (any Queue with close). The handoff tags
    (`queue_recv_handoff_before_publication`,
    `queue_send_handoff_before_publication`) are required **only under
    grant/handoff semantics (B/C)**; under wake-and-retry (A) they are not.
    (§J.2)
27. **Can the first implementation avoid cancellation?** **Yes — cancellation
    can be excluded from the first E12-E scope, provided the semantic/API
    specification says so explicitly and no cancellation-facing API is shipped.
    No authoritative roadmap or cross-primitive contract requires E12-E's first
    implementation to expose cancellation.** The roadmap
    (`docs/async-runtime-plan.md:505-515`) requires only "not-empty waiters,
    not-full waiters, close semantics"; the plan §8.2 resolves the cancel-race
    *semantic* (what happens if cancel and reservation race) but does not
    mandate that the first implementation expose a cancellation API. The
    existing cancellation infrastructure (E10 `cancel_wait`, E11 timer expiry)
    remains reusable for a later extension. (§D.4, §F.2)
28. **Can the first implementation avoid direct handoff?** **Yes** — a
    buffer-first queue (no direct handoff) is a viable first scope; handoff is
    an optimization (Model D). Moreover a **wake-and-retry** design (A) avoids
    reservation/commit-seam entirely. The four-case separation (§D.4) confirms
    that Cases A and B (cancel/expire races) are already handled by the loser
    semantic without any Queue-specific refund logic. (§E.5 D15, §D.4, §K.2)
29. **What must be formally modeled before implementation?** At minimum:
    **buffer-bounds safety + close monotonicity** (Models A+B). The
    reservation/grant-commit-coupling invariants are required to be modeled
    **only under grant semantics (B/C)**; under wake-and-retry (A) they do not
    exist. Timeout/cancel (Model C) tests the loser semantic without refund
    logic (Cases A and B are sufficient with the existing resolution
    authority) and handoff (Model D) can stage later. (§K.2)
30. **What exact artifacts must exist before implementation may start?** (1)
    closed decision cluster (D1, D4, D5/D7/D9, D14, D15, §D.6 Q7.1 +
    §D.6.1(b), destruction — all settled *within* the specification); (2) E12-E
    semantic contract; (3) state-machine design (with reservation
    representation **only if grant**); (4) the `queue_*` seam designs
    (handoff seams **only if grant/handoff**); (5) exception/element-type
    contract incl. the `T`-under-lock rule; (6) close contract + destruction
    contract (separate); (7) deterministic test plan; (8) TLA+ model plan
    (Models A+B minimum); (9) implementation sequencing; (10) closure
    checklist. (§L) Note: these gate **implementation**, not the **start** of
    specification work (§A).

---

## O. Evidence index (key file:line references)

**Closed substrate (E10/E11):**
- `include/sluice/async/wait_node.hpp:123` — `WaitNode`; `:190-196` State enum;
  `:225-235` `resolve_` CAS (sole winner authority); `:136-139` dtor assert.
- `include/sluice/async/wait_queue.hpp:119` — `WaitQueue`; `:145`
  `friend class Scheduler`; `:152` `mtx()`; `:174-186` `register_wait_locked`;
  `:199-211` `wake_one_locked`; `:222-233` `cancel_locked`; `:266-272`
  `expire_locked`; `:289-294` `contains_locked`; `:302-317` `unlink_locked`;
  `:319-321` intrusive FIFO `head_`/`tail_`.
- `include/sluice/async/scheduler.hpp:249,255,261,304,314` — public wait API;
  `:801` `global_mtx_`; `:558-559` `mutex_handoff_one_locked` (the precedent
  seam); `:1045` `retire_timer_for_node_locked`.
- `src/async/scheduler.cpp:1037` `await_wait`; `:1078-1106`
  `wake_wait_one_locked`; `:1108` `wake_wait_one` (returns bool);
  `:1122-1142` `cancel_wait`; `:1191-1272` `await_wait_deadline`;
  `:1274-1297` `expire_wait`; `:2065-2106` `mutex_handoff_one_locked` body
  (`owner = f` commit at `:2088`); `:2440-2467` `retire_timer_for_node_locked`.
- `include/sluice/async/timer_registration.hpp:65-151` — `deadline_t`,
  `TimerRegistration`.
- `include/sluice/async/mutex.hpp:17-33` — synchronous structural `Mutex`
  (TSA-annotated `std::mutex` wrapper, NOT async).

**Existing E12 primitives:**
- `include/sluice/async/event.hpp` — `~Event()=default` (`:86`); `waiters_`
  (`:177`).
- `include/sluice/async/semaphore.hpp` — `~Semaphore()=default` (`:108`);
  `max_permits_`/`available_`/`waiters_` (`:222-225`).
- `include/sluice/async/async_mutex.hpp` — `~AsyncMutex` asserts `owner_==nullptr`
  (`:100-103`); `owner_`/`waiters_` (`:221-223`); no `wait_queue()` accessor.
- `include/sluice/async/condition.hpp` — `~AsyncCondition` asserts
  `active_waits_==0` (`:120-124`); two-epoch wait (`:223-274`).

**Testing/seam authority:**
- `include/sluice/async/scheduler.hpp:38-57` —
  `ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1`.
- `xmake.lua:46-86` — production vs `sluice_async_internal_testing` targets.
- `tests/e12_*_authority_probe.cpp` — NEG compile probes (not xmake targets;
  driven by verify scripts).
- `tests/e12_event_test.cpp:713-752,611-651,1858-1930` — race test patterns.

**Formal models:**
- `docs/spec/e12_semaphore/E12Semaphore.tla` (12 invariants, 7 negatives);
  `docs/spec/e12_async_mutex/E12AsyncMutex.tla` (19 invariants, 11 negatives);
  `docs/spec/e12_event/`, `docs/spec/e12_async_condition/`,
  `docs/spec/e11_timer_wait/`.
- `scripts/verify-e12-*-formal.sh` — TLC gate pattern (expect_pass /
  expect_fail + named invariant / wrong_property_gate / compile_probe_gate).
- `tla2tools.jar` (repo root) — TLC runtime.

**Cross-primitive authority (the doc this audit deepens, does NOT supersede):**
- `docs/e12-sync-primitives-plan.md` §8 (Queue), §14 (grant-commit boundary).
- `docs/async-runtime-plan.md:433,505-515` — Queue roadmap entry.

---

## M. Implementation authorization

```text
E12-E IMPLEMENTATION AUTHORIZATION: DENIED
```

This audit is a **discovery and preparation audit only**. It cannot authorize
implementation. Implementation requires, at minimum, closure of the P0 decision
cluster (§I), production of the documents in §L in the order of §L.2, and a
separate independent authorization act. The cross-primitive authority
(`docs/e12-sync-primitives-plan.md` §8) remains `HUMAN-DECISION-REQUIRED` for
the core Queue shape.

> **SUPERSEDED HISTORICAL END NOTE:** The preceding historical denial reason is
> no longer current. The binding denial and remaining gates are stated in this
> file's opening Current Authority section: independent approval/production
> realization of `ASYNC-MUTEX-NOTHROW-AUTHORITY-1`, independent adversarial
> review of Corrective-2, and the separate Condition T25 hang audit.
> Implementation remains denied.
