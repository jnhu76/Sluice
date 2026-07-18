# E12-E Queue Production Implementation — Progress Note

> **Decision identity:** `E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1`
>
> **Status:** `AUTHOR SELF-ASSESSMENT PASS — AWAITING PHASE I INDEPENDENT REVIEW`
>
> This document records the as-built state of the E12-E Queue production
> implementation. The implementation is FUNCTIONALLY COMPLETE through P8 +
> Phase G, and the Phase H author self-assessment (below) records a PASS.
> The claim is NOT final: Phase I (independent adversarial implementation
> review) must still run. The implementation PASSes only when an independent
> reviewer confirms there is no exploitable defect against the Corrective-2
> authority, the formal model, and the documented lock order.

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

### P8 — public AsyncQueue<T> — LANDED (this commit)

`include/sluice/async/async_queue.hpp`: the public typed template wrapper.

- `QueuePushStatus` / `QueuePopStatus` aliases to the opaque enums (status
  values are type-erased; only the payload is typed).
- `QueuePushResult<T>` / `QueuePopResult<T>` move-only typed results
  holding `std::optional<T>` storage. The `committed`/`closed`/`expired`/
  `would_block`/`item` factory methods construct the appropriate state;
  `take_value()` recovers the exact T (one move). Storage is `optional<T>`
  so T need NOT be default-constructible or move-assignable — the runtime
  constraint is nothrow-move-constructible + nothrow-destructible
  (mirrors `QueueItemFactory::make<T>`).
- `AsyncQueue<T>`: thin template over an embedded non-template `QueuePort`.
  `try_push` / `push` / `push_until` mint the typed `Node<T>` OUTSIDE
  locks via `QueueItemFactory::make<T>`, drive the QueuePort seam, and
  convert the opaque result back to typed (releasing the `Node<T>` via
  `release_failed<T>` / `release_popped<T>` OUTSIDE locks — typed
  conversion is explicitly NOT counted in `active_port_calls_` per §7).
  `try_pop` / `pop` / `pop_until` symmetric. `close`, snapshots,
  `begin_teardown` are thin forwards. `release_teardown(session)` is the
  typed helper to drain via the unique session.
- Copy/move deleted (the embedded QueuePort is non-movable).
- `static_assert` enforces T is object + nothrow-move-constructible +
  nothrow-destructible.

Verified: `e12_async_queue_test` — 3 P8 cases PASS (typed FIFO + failure
recovery with exact-T `would_block` and `closed`, typed teardown via
`release_teardown`, capacity-0 ctor rejection). Clang Debug + GCC Debug
+ ASan + TSan clean. Production `sluice_async` target compiles clean.

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
enforced by the P1 linear capability, the P2+P3 ring/failed-payload
structure, and the P8 typed wrapper's `static_assert` + `optional<T>`
storage + factory `release_*` identity validation (NEG-1/2/3/4/7 of the
formal model map directly). The lock-order / state-machine /
runtime-structure subset is enforced by the G -> S -> exactly-one-role
critical sections in the P4-P6 grant seams and the P7 lifecycle
transition.

## Phase G — extended test matrix — LANDED (this commit)

Phase G extends coverage beyond the single-worker deterministic subset:

- **G1 — timed variants (P9 ProducerExpire / C8 ConsumerExpire):**
  - `e12_queue_g1_push_until_expires_recovers_value`: a producer parks on
    a full ring with a future deadline; no consumer frees a slot; no
    close occurs; the deadline elapses (driven by `advance_clock`); the
    producer resumes with `expired` and the EXACT original T (777) is
    recovered. Timer retired with no leak.
  - `e12_queue_g1_pop_until_expires`: a consumer parks on an empty open
    ring with a future deadline; no producer arrives; the deadline
    elapses; the consumer resumes with `expired` (empty out-lease).

- **G2 — multi-worker producer-consumer migration:** a producer Fiber
  parks on a full ring on W0; a consumer Fiber (spawned on W1) frees a
  slot; the reconcile-grant publishes the producer runnable across the
  worker boundary; the producer resumes and commits. `run_live(2)` keeps
  the run resident while the producer is parked (mirrors e12_mtx T19).
  Asserts: the consumer popped the pre-fill, the producer committed
  after the slot freed, the committed value is exact, ring size is 1.

- **G3 — sanitizer matrix (4 cells):** Clang Debug, GCC Debug, Clang
  ASan, Clang TSan. All 20 cases PASS in every cell. TSan stress x5
  (rare-race amplification) clean.

- **Transition coverage:** the suite as a whole exercises 18/19 canonical
  transitions (every state-machine path except P8 ProducerTimedReturn,
  a typed-wrapper-only concern that adds no QueuePort state) and 6/6
  publication paths.

Verified: 20/20 `e12_async_queue_test` cases PASS on Clang Debug, GCC
Debug, ASan, TSan. E11/E12 sync primitive tests regress-clean. Production
`sluice_async` target compiles clean.

## Phase H — author self-assessment — PASS

The author has reviewed the as-built implementation against the
Corrective-2 authority, the B4 formal model, the documented lock order,
and the test evidence. The assessment records a PASS, with the explicit
caveat that an independent Phase I reviewer must still confirm.

### Authority surface (Corrective-2 §3/§5/§6/§7/§9)

| Requirement | As-built |
| --- | --- |
| Non-template QueuePort is the ONLY Queue friend of Scheduler | `friend class ::sluice::async::detail::QueuePort;` in scheduler.hpp |
| `begin_teardown` does NOT enter ordinary CallGuard | `begin_teardown` body has no `CallGuard guard(*this);` |
| `begin_teardown` requires all four counters == 0 | checked under G+S before transition |
| `begin_teardown` requires both role FIFOs empty | `scheduler_.queue_role_waiters_empty_locked(*this)` |
| `begin_teardown` performs operational -> tearing_down under G+S | `LockGuard glk(global_mtx_); LockGuard lk(state_mtx_); lifecycle_ = tearing_down;` |
| `take_next` requires lifecycle == tearing_down | checked at entry |
| `take_next` moves ring -> teardown | `out.control_->location_ = Location::teardown;` |
| Typed layer releases Node<T> outside locks | `release_teardown` / `release_popped` / `release_failed` delete Node outside any QueuePort/Scheduler critical section |
| No Permit, no reusable item, no active-owner veto, no cancellation outcome, no direct handoff | no Permit field, no reusable item slot, no active-owner concept, no `cancelled` pop/push status, no direct handoff seam |
| No T ctor/move/dtor under G/S/P/C | T moves occur in `QueueItemFactory::make/release_*` (outside locks) and `AsyncQueue<T>::from_opaque_*` (outside locks) |
| Scheduler is the ONLY friend of WaitQueue | `friend class Scheduler;` is the sole friend in wait_queue.hpp; QueuePort reaches role FIFOs via Scheduler seams only |

### Lock order (Corrective-2 §3.5)

`global_mtx_ (G) -> QueuePort::state_mtx_ (S) -> exactly one role WaitQueue mtx (P or C)`.

| Site | G | S | role | Notes |
| --- | --- | --- | --- | --- |
| try_push commit + grant | ✓ | ✓ | C (internally by grant seam) | G+S caller-held; grant takes C under G |
| try_pop commit + grant | ✓ | ✓ | P (internally by grant seam) | symmetric |
| close drain loop | ✓ | ✓ | C then P (sequentially, never together) | grant seams each take one role under G |
| push/pop admit | ✓ | ✓ | own role (admission closure) | single suspend; reconciler commits |
| queue_cancel | ✓ | — | target role | identity-safe; no ring mutation |
| begin_teardown | ✓ | ✓ | both (sequentially via helper) | role mtx taken under G only |
| take_next | — | ✓ | — | lifecycle == tearing_down; no waiters can exist |

P and C are NEVER held together (the two grant seams each take exactly one
role mtx under G; `begin_teardown` queries each role mtx sequentially).

### Linear capability (P1)

| Property | As-built |
| --- | --- |
| QueueItemControl non-copyable/non-movable; private ctor | `queue_item.hpp` |
| QueueItemLease move-only; `std::exchange` empties source | `queue_item.hpp` |
| Non-empty lease dtor fail-fast | `~QueueItemLease` |
| Empty-destination move-assign fail-fast | enforced in `QueueOpaquePushResult/PopResult::operator=` |
| `queue_type_token<T>` stable per-type sentinel | inline header-only |
| `queue_lease_fail_fast` `[[noreturn]] noexcept` | declared + defined |

The P1 7-negative access-control probe still rejects every probe
(NEG_LEASE_COPY / NEG_LEASE_COPY_ASSIGN / NEG_LEASE_PUBLIC_DEFAULT_CTOR /
NEG_LEASE_PUBLIC_CONTROL_CTOR / NEG_CONTROL_PUBLIC_CTOR / NEG_CONTROL_COPY /
NEG_CONTROL_MOVE).

### Winner-before-publication (Corrective-2 §8)

Each grant seam (`queue_grant_consumer_locked` /
`queue_grant_producer_locked`) performs in one critical section:

1. `wake_one_locked` — resolve FIFO head `Woken` (the resolve CAS is the
   winner authority)
2. resource commit — read `won->user()` for per-op context; move the ring
   item into the winner's out-lease OR move the winner's lease into the
   freed slot
3. `retire_timer_for_node_locked` — disarm any bound timer (E11 I4)
4. counter decrement (`active_wait_associations_`)
5. `make_runnable` + `route_runnable_locked` — publication LAST

This mirrors `mutex_handoff_one_locked`'s ordering exactly.

### TLA+ formal model (B4)

The B4 model (`docs/spec/e12_queue/`) verified the state machine: the
closed/empty terminal, FIFO order, capacity bound, the 19 canonical
transitions, the 6 publication paths, and 33 counterexamples. The model
passed TLC2 v2.19 on the live spec + closed-drain variant + all 7
negatives. The as-built implementation matches the model's transitions
(18/19 are exercised by tests; the remaining P8 ProducerTimedReturn adds
no QueuePort state).

### Test evidence

| Cell | Result |
| --- | --- |
| Clang Debug | 20/20 PASS |
| GCC Debug | 20/20 PASS |
| Clang ASan | 20/20 PASS (no leak, no UAF) |
| Clang TSan | 20/20 PASS (no data race) |
| Clang TSan ×5 stress | 5/5 PASS (rare-race amplification) |
| E11/E12 sync primitive regressions | clean |

### Honest exclusions

- The fail-fast paths (second `begin_teardown`, ordinary op after
  teardown, session dtor with non-empty ring, non-empty lease dtor) call
  `std::terminate` and are NOT exercised by unit tests — they are
  structural invariants serialized by the lifecycle transition and the P1
  linear capability. The P1 access-control probe covers the
  type-structure subset (7 negatives); the runtime fail-fast subset
  remains structurally enforced, not test-exercised.
- The multi-worker migration test (G2) demonstrates the publication path
  crosses worker boundaries; it does NOT deterministically prove which
  worker resumes the producer (W0 or W1) — both are correct, and TSan
  confirms no race either way. A T19-style deterministic steal proof
  (proving a specific worker steals) is out of scope for Phase G; the
  E8 steal substrate has its own T19 proof.
- Phase I (independent adversarial implementation review) has NOT run.
  This self-assessment PASS is the AUTHOR's claim; the implementation
  PASSes only when Phase I confirms.

## Repository state

```text
branch:           e12-e-queue-production-impl
HEAD:             2b22793 (Phase G extended test matrix)
working tree:     clean between commits
untracked files:  tests/test_t3_simple.cpp  (pre-existing, unrelated; untouched)
                  tla2tools.jar             (pre-existing, unrelated; untouched)
pushed:           no   (no upstream; no push / merge / PR)
```
