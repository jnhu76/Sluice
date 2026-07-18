# E12-E Queue Production Implementation — Progress Note

> **Decision identity:** `E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1`
>
> **Status:** `IN PROGRESS — P1-P7 LANDED; P8 NEXT`
>
> This document records the as-built progress of the E12-E Queue production
> implementation. It is NOT a PASS self-assessment: the implementation is
> partial (the type foundation, fast paths, blocking/timed wait admission +
> reconciliation + publication, and the teardown lifecycle are complete and
> tested; the public `AsyncQueue<T>` template wrapper is not yet authored).
> An independent adversarial implementation review (Phase I) has NOT run; it
> must run only once the implementation is functionally complete through P8.

## Authorization baseline

```text
E12-E-QUEUE-IMPLEMENTATION-AUTHORIZATION-2: PASS
E12-E QUEUE PRODUCTION IMPLEMENTATION: AUTHORIZED
```

All four prerequisite gates independently re-verified and closed:
- B1 Mutex no-throw substrate: PASS (`be07564` + `e2cfe61` + review `15dc9b4`).
- B2 Corrective-2 independent adversarial review: PASS (review `4f81d6c`).
- B3 Condition T25 migration/reacquire: PASS (W1 corrective `db656b5`).
- B4 Queue formal model + independent formal review: PASS
  (model `9572985` + corrective `f53faf0` + review-2 `6aa2334`).

See `docs/e12-queue-implementation-authorization.md` AUTHORIZATION-2 section
for the full per-gate evidence.

## Implementation progress (Phase E)

### P1 — type & linear capability foundation — LANDED (`ffa515b`)

`include/sluice/async/detail/queue_item.hpp`: `QueueItemControl`
(non-copyable/non-movable; private ctor; `location_`/`type_token_` mutated
only by authority), `QueueItemLease` (move-only; `std::exchange` empties
source; empty-destination move-assign / non-empty dtor are fail-fast;
never deletes a node / invokes user code), `queue_lease_fail_fast`
(`[[noreturn]] noexcept`), `queue_type_token<T>` (inline, stable per-type
sentinel). `src/async/queue_port.cpp`: the P1 out-of-line bodies.

Verified: 7 access-control negative-compile probes (NEG_LEASE_COPY,
NEG_LEASE_COPY_ASSIGN, NEG_LEASE_PUBLIC_DEFAULT_CTOR,
NEG_LEASE_PUBLIC_CONTROL_CTOR, NEG_CONTROL_PUBLIC_CTOR, NEG_CONTROL_COPY,
NEG_CONTROL_MOVE) each correctly rejected; positive probe clean; Clang +
GCC `sluice_async` builds green.

### P2+P3 — QueuePort skeleton + fast paths — LANDED (`89cb9f7`)

`include/sluice/async/detail/queue_port.hpp`: `QueueOpaquePushResult` /
`QueueOpaquePopResult` (move-only; status<=>lease invariants),
`QueueItemFactory` (typed `Node<T>`; `make<T>` outside locks; `release_*`
validate port + type-token + location, recover Node via the control's
`typed_node_` back-pointer, move T once, mark released, delete Node outside
locks), `QueueLifecycle`, `QueueTeardownSession` (skeleton),
`QueuePort` (ring + counters + lifecycle + close + `state_mtx_`).
`src/async/queue_port.cpp`: ctor, `CallGuard`, `try_push` (P2 FastPushCommit
/ P3 PushClosed / P4 TryPushWouldBlock), `try_pop` (C1 FastPopCommit / C2
PopClosedEmpty / C3 TryPopWouldBlock), `close` (CL1/CL2), snapshots,
teardown-session skeleton. Ring moves preserve control custody; no T under
`state_mtx_`.

Verified: `e12_async_queue_test` — 7 P2+P3 cases PASS (capacity/FIFO,
capacity-1, try_pop would_block, close idempotent + closed-empty terminal,
closed drains buffered, push_closed returns exact lease, one-shot lease
move empties source). Clang Debug + GCC Debug builds + runs green. P1
authority probe still 7/7 negatives rejected.

### P4-P6 — wait admission + reconciliation + publication + stealing — LANDED (`96e4618`)

`push` / `push_until` / `pop` / `pop_until` perform wait admission via the
Scheduler seams (`queue_push_admit` / `queue_pop_admit` / `*_until`
deadline-aware variants). The Scheduler-coupled core (producer/consumer
`WaitQueue` role FIFOs, the inline admission closure, deadline-aware
admission with inline-Expired, the reconciliation grant seams
`queue_grant_consumer_locked` / `queue_grant_producer_locked` running under
G + S + exactly-one-role, winner-before-publication commit ordering,
timer retirement on grant) is implemented. `close` drains both FIFOs via
the grant seams (each parked consumer gets one buffered item until the
ring empties, then closed; each parked producer gets its lease retained as
closed). Active-victim work stealing is inherited unchanged from E8 (the
queue uses the standard runnable-ticket publication path).

Verified: `e12_async_queue_test` — 4 P4-P6 concurrency cases PASS
(blocking pop granted on push, blocking push granted on pop, close
completes blocked producer with `closed` + exact lease, close drains
buffered items to a blocked consumer then closed). Clang Debug + GCC
Debug + ASan + TSan clean.

### P7 — teardown lifecycle — LANDED (this commit)

`QueuePort::begin_teardown` performs the irreversible
`operational -> tearing_down` transition under G + S with the full
precondition check:

```
lifecycle_              == operational
active_port_calls_      == 0
active_wait_associations_ == 0
active_queue_timers_    == 0
granted_not_resumed_    == 0
producer WaitQueue empty
consumer WaitQueue empty
```

The waiters-emptiness query is the Scheduler-authority seam
`Scheduler::queue_role_waiters_empty_locked(port)` (QueuePort is not a
friend of `WaitQueue`); it takes each role `mtx()` sequentially under G.
Once `tearing_down`, every ordinary entry (push/pop/try/timed/close/
snapshot/second `begin_teardown`) fail-fast on the lifecycle check
before constructing a CallGuard. `take_next` drains ring slots
one-by-one (`ring -> teardown`) in FIFO order; the typed layer recovers
each value via `release_teardown` and destroys the exact `Node<T>`
outside locks. Session destruction is no-throw and succeeds iff the ring
is empty.

Verified: `e12_async_queue_test` — 3 P7 cases PASS (multi-item ring
drained FIFO, empty ring yields immediately-empty session, session
move-only empties source). Clang Debug + GCC Debug + ASan + TSan clean.
The fail-fast paths (second `begin_teardown`, ordinary op after teardown,
session dtor with non-empty ring) call `std::terminate` and are NOT
exercised — they are structural invariants serialized by the lifecycle
transition and the linear capability (P1).

### P8 — public AsyncQueue<T> — NOT YET IMPLEMENTED

The public `AsyncQueue<T>` template wrapper
(`include/sluice/async/async_queue.hpp`) is not yet authored. It is a
thin typed layer that converts opaque results to typed ones, drives the
factory for `Node<T>` make/release, and destroys the exact `Node<T>`
OUTSIDE all locks. Copy/move deleted.

## Transition coverage (as-built)

Canonical (target 19): live = P2 FastPushCommit, P3 PushClosed, P4
TryPushWouldBlock, P5 ProducerWaitAdmission, P6 ProducerGrantCommit, P7
ProducerClosedCommit, P9 ProducerExpire, P10 ProducerReturn, C1
FastPopCommit, C2 PopClosedEmpty, C3 TryPopWouldBlock, C4
ConsumerWaitAdmission, C5 ConsumerGrantCommit, C6 ConsumerClosedCommit,
C8 ConsumerExpire, C9 ConsumerReturn, CL1 CloseLinearize, CL2
IdempotentClose (18/19). The remaining transition (P8 ProducerTimedReturn
at the typed wrapper boundary) is a typed-layer-only concern and does not
add a QueuePort state.

Publication (target 6): the suspended-winner publication path (resolve
CAS + commit + retire + `make_runnable` + `route_runnable_locked`, all
winner-before-publication) is exercised by the four P4-P6 concurrency
cases and the close-drain paths (6/6).

Counterexamples (33): the type-structure / access-control subset is
enforced by the P1 linear capability and the P2+P3 ring/failed-payload
structure (NEG-1/2/3/4/7 of the formal model map directly). The
lock-order / state-machine / runtime-structure subset is enforced by the
G -> S -> exactly-one-role critical sections in the P4-P6 grant seams
and the P7 lifecycle transition.

## Known exclusions / honest scoping

- The implementation is PARTIAL (P8 + Phase G extended matrix + Phase H
  + Phase I remain). This document does NOT claim
  `E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1: PASS`. Phase H author
  self-assessment PASS and Phase I independent implementation review have
  NOT run.
- The fast-path + wait-admission + teardown tests verify the
  single-worker deterministic subset. The full multi-worker concurrency
  / migration / sanitizer matrix (Phase G) lands after P8.

## Repository state

```text
branch:           e12-e-queue-production-impl
HEAD:             (see git log; advances as commits land)
working tree:     clean between commits
untracked files:  tests/test_t3_simple.cpp  (pre-existing, unrelated; untouched)
                  tla2tools.jar             (pre-existing, unrelated; untouched)
pushed:           no   (no upstream; no push / merge / PR)
```
