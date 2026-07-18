# E12-E Queue Production Implementation — Progress Note

> **Decision identity:** `E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1`
>
> **Status:** `IN PROGRESS — P1-P3 LANDED; P4-P6 NEXT`
>
> This document records the as-built progress of the E12-E Queue production
> implementation. It is NOT a PASS self-assessment: the implementation is
> partial (the fast paths and type foundation are complete and tested; the
> Scheduler-coupled blocking/timed/reconciliation/publication/stealing core
> is not yet implemented). An independent adversarial implementation review
> (Phase I) has NOT run; it must run only once the implementation is
> functionally complete through P8.

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

### P4-P6 — wait admission + reconciliation + publication + stealing — NOT YET IMPLEMENTED

`push` / `push_until` / `pop` / `pop_until` currently throw
`std::logic_error` ("wait admission not yet implemented"). The
Scheduler-coupled core (producer/consumer `WaitQueue`, the inline admission
closure, the PREPARED timer model + the three Scheduler timer ops,
reconciliation state machine, runnable ticket publication, worker-side
own-oldest/global-oldest selection with active-victim stealing) is scoped
and designed (see the Scheduler-integration recon and
`docs/e12-queue-scheduler-integration.md`) but not yet coded. This is the
correctness-critical core; it is the largest remaining work item.

### P7-P8 — teardown + public AsyncQueue<T> — NOT YET IMPLEMENTED

`QueueTeardownSession::begin_teardown` returns a session bound to the port
but does NOT yet perform the irreversible `operational -> tearing_down`
transition under G+S with the full precondition check; `take_next` / `empty`
/ dtor are wired structurally but the lifecycle transition is P7. The public
`AsyncQueue<T>` template wrapper (`include/sluice/async/async_queue.hpp`)
is not yet authored (P8).

## Transition coverage (as-built)

Canonical (target 19): live = P2 FastPushCommit, P3 PushClosed, P4
TryPushWouldBlock, C1 FastPopCommit, C2 PopClosedEmpty, C3 TryPopWouldBlock,
CL1 CloseLinearize, CL2 IdempotentClose (8/19). Pending P4-P6: P5
ProducerWaitAdmission, P6 ProducerGrantCommit, P7 ProducerClosedCommit, P9
ProducerExpire, P10 ProducerReturn, C4 ConsumerWaitAdmission, C5
ConsumerGrantCommit, C6 ConsumerClosedCommit, C8 ConsumerExpire, C9
ConsumerReturn (11/19).

Publication (target 6): 0/6 (all depend on P5-P6 reconciliation).

Counterexamples (33): the type-structure / access-control subset is enforced
by the P1 linear capability and the P2+P3 ring/failed-payload structure
(NEG-1/2/3/4/7 of the formal model map directly). The lock-order /
state-machine / runtime-structure subset is pending P4-P6.

## Known exclusions / honest scoping

- The implementation is PARTIAL. This document does NOT claim
  `E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1: PASS`. Phase H author
  self-assessment PASS and Phase I independent implementation review have
  NOT run.
- The fast-path tests verify only the no-Scheduler subset. The full
  concurrency / migration / sanitizer matrix (Phase G) requires P4-P6.
- `push`/`push_until`/`pop`/`pop_until` throw on call — any caller invoking
  them today gets `std::logic_error`. This is an honest deferred-path
  marker, not a silent stub.

## Repository state

```text
branch:           e12-e-queue-production-impl
HEAD:             (see git log; advances as commits land)
working tree:     clean between commits
untracked files:  tests/test_t3_simple.cpp  (pre-existing, unrelated; untouched)
                  tla2tools.jar             (pre-existing, unrelated; untouched)
pushed:           no   (no upstream; no push / merge / PR)
```
