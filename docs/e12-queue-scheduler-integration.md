# E12-E Queue Scheduler Integration — Corrective-2

> **Task identity:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2`
>
> **Status:** `PASS — INDEPENDENT ADVERSARIAL REVIEW PASS (B2)`
>
> **Applied review disposition:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1:
> SUPERSEDED — REQUEST-CHANGES`
>
> **Review disposition:**
> `E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-1-REVIEW:
> REQUEST-CHANGES`
>
> **Dependent substrate:**
> `ASYNC-MUTEX-NOTHROW-AUTHORITY-1: DESIGN PASS — PRODUCTION IMPLEMENTED —
> INDEPENDENT REVIEW PASS (B1)`
>
> **Review gate:** `INDEPENDENT ADVERSARIAL REVIEW PASS (B2)`

This is document design only. It does not authorize or modify Queue,
Scheduler, Mutex, WaitQueue, TimerRegistration, tests, build rules, production
PhaseTags, or a formal model.

```text
TARGET COVERAGE:
19 canonical transitions
6 publication transitions

VERIFIED COVERAGE — AUTHOR SELF-ASSESSMENT:
19/19 canonical transitions
6/6 publication transitions

E12-E IMPLEMENTATION AUTHORIZATION: all four prerequisite gates PASS.
See docs/e12-queue-implementation-authorization.md (AUTHORIZATION-2).

Gate status (current):
  B1 Mutex no-throw substrate:               PASS  (independent review complete)
  B2 Corrective-2 independent review:        PASS  (independent adversarial review complete)
  B3 Condition T25 migration/reacquire:      PASS  (W1 corrective db656b5)
  B4 Queue formal model:                     PASS  (independent formal review complete)
```

## 1. Disposition and root-cause record

Corrective-1 is not current authority. Its author-declared PASS and coverage
claims are withdrawn. The accepted review defects share four root causes:

1. a reusable `QueueItemBase&` plus `owner_ == this` established identity but
   did not establish detached, unique ownership;
2. admission-owner activity was incorrectly treated as execution
   availability, although the Scheduler may steal runnable work from an active
   victim;
3. quiet drain was modeled as an ordinary mutation and `active_calls_` was
   described more broadly than the non-template QueuePort authority it counted;
4. PREPARED timer rollback depended on an abstract guard without fixing the
   list iterator representation or C++ reverse-destruction ordering.

The authoritative corrections are therefore structural: a one-shot linear
lease, unrestricted active-victim ticket stealing after own-ticket preference,
an irreversible teardown session, a narrowly defined call counter, and a
concrete PREPARED guard.

## 2. Locked architecture decisions

The following remain binding:

```text
fixed-capacity ring
Scheduler-owned reconciliation
global -> Queue state -> exactly one role WaitQueue lock
no direct producer-to-consumer handoff
Queue v1 external cancellation deferred
19 canonical transition target
6 publication transition target
PREPARED -> ACTIVE -> RETIRED/CONSUMED timer model
operation-embedded intrusive runnable ticket
admission-captured stable fiber owner slot
no-throw/fail-fast Mutex substrate dependency
exact public push/pop result vocabulary
AsyncQueue copy/move deleted
```

Corrective-2 replaces only the rejected realization details:

```text
reusable item reference     -> one-shot QueueItemLease
active owner blocks steal   -> own-oldest, otherwise global-oldest
public quiet drain          -> QueueTeardownSession
active_calls_               -> active_port_calls_
author coverage assertion   -> row-by-row evidence before self-assessment
```

Context7 was consulted for the C++20 standard-library surface. Its selected
`/microsoft/stl` index did not return the requested container-invalidation or
automatic-lifetime clauses, so it is not treated as proof. The representation
claims below are instead bound to explicit C++ declarations and the scratch
language/representation probes in §18.

## 3. File, include, and authority boundary

The future production layout remains a fixed non-template boundary:

```text
include/sluice/async/async_queue.hpp
    AsyncQueue<T>, public results, QueueItemFactory typed conversion

include/sluice/async/detail/queue_item.hpp
    QueueItemControl, QueueItemLease, opaque result declarations

include/sluice/async/detail/queue_port.hpp
    fixed QueuePort and QueueTeardownSession declarations

include/sluice/async/scheduler.hpp
    private Queue seams; QueuePort is the only Queue friend

src/async/queue_detail.hpp
    QueueCore, operation types, wait registration, runnable ticket,
    PreparedQueueTimer

src/async/queue_port.cpp
    non-template Queue authority and lifecycle transitions

src/async/scheduler.cpp
    Queue reconciliation, timer activation/retirement, ticket publication
    and worker-side ticket selection
```

Installed detail types expose no raw-pointer lease constructor, adopt/reset,
location mutation, owner-map mutation, reconciliation entry, ticket
constructor, or teardown-session constructor. A caller-created QueuePort can
mint leases only for that port's own typed factory and core; it cannot access
the private port embedded in another `AsyncQueue<T>`.

## 4. Exact lease and control type graph

The following is the binding declaration shape. Equivalent implementation
spelling may change only if it preserves every access and state invariant.

```cpp
namespace sluice::async::detail {

class QueuePort;
class QueueOpaquePushResult;
class QueueOpaquePopResult;
class QueueTeardownSession;
class QueueItemFactory;

class QueueItemControl final {
private:
    enum class Location : std::uint8_t {
        detached,
        producer_operation,
        ring,
        consumer_operation,
        teardown,
        released,
    };

    QueuePort* const owner_port_;
    void* const typed_node_;
    const void* const type_token_;
    Location location_{Location::detached};

    QueueItemControl(QueuePort&, void* typed_node,
                     const void* type_token) noexcept;

    QueueItemControl(const QueueItemControl&) = delete;
    QueueItemControl& operator=(const QueueItemControl&) = delete;
    QueueItemControl(QueueItemControl&&) = delete;
    QueueItemControl& operator=(QueueItemControl&&) = delete;

    friend class QueuePort;
    friend class QueueItemLease;
    friend class QueueTeardownSession;
    friend class QueueItemFactory;
};

class QueueItemLease final {
public:
    QueueItemLease(QueueItemLease&& other) noexcept
        : control_(std::exchange(other.control_, nullptr)) {}

    QueueItemLease& operator=(QueueItemLease&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        // Binding precondition: the destination is empty. Violation is
        // fail-fast; a live lease is never silently overwritten.
        require_empty_or_terminate();
        control_ = std::exchange(other.control_, nullptr);
        return *this;
    }

    QueueItemLease(const QueueItemLease&) = delete;
    QueueItemLease& operator=(const QueueItemLease&) = delete;

    ~QueueItemLease() noexcept;

    explicit operator bool() const noexcept { return control_ != nullptr; }

private:
    QueueItemControl* control_{nullptr};

    QueueItemLease() noexcept = default;
    explicit QueueItemLease(QueueItemControl& control) noexcept
        : control_(&control) {}

    QueueItemControl* release_control() noexcept {
        return std::exchange(control_, nullptr);
    }

    void adopt_control(QueueItemControl& control) noexcept;
    void require_empty_or_terminate() const noexcept;

    friend class QueuePort;
    friend class QueueOpaquePushResult;
    friend class QueueOpaquePopResult;
    friend class QueueTeardownSession;
    friend class QueueItemFactory;
};

} // namespace sluice::async::detail
```

`QueueItemLease` is an internal linear capability, not a user-extensible smart
pointer. Its non-empty destructor is a fail-fast invariant check; it never
deletes a type-erased node and never invokes user code. Every well-formed path
explicitly transfers or releases the control before destruction. Typed
deletion occurs through the exact hidden `QueueItemFactory::Node<T>` static type outside all
Queue/Scheduler locks.

The fixed, non-template factory hides the typed node and embeds the control so
their addresses and lifetime are related without a second allocation:

```cpp
class QueueItemFactory final {
public:
    template<class T, class U>
    static QueueItemLease make(QueuePort&, U&& value);

    template<class T>
    static T release_failed(QueuePort&, QueueItemLease&&) noexcept;

    template<class T>
    static T release_popped(QueuePort&, QueueItemLease&&) noexcept;

    template<class T>
    static void release_teardown(QueuePort&, QueueItemLease&&) noexcept;

private:
    static QueueItemControl make_control(
        QueuePort&, void* typed_node, const void* type_token) noexcept;

    template<class T>
    class Node final {
    private:
        template<class U>
        Node(QueuePort& port, U&& value)
            : control_(QueueItemFactory::make_control(
                  port, this, queue_type_token<T>())),
              value_(std::forward<U>(value)) {}

        QueueItemControl control_;
        T value_;

        friend class QueueItemFactory;
    };
};
```

`make<T>` allocates the hidden exact node outside internal locks and returns
the only lease to its embedded control. The three release functions validate
the owning port, exact type token, and expected control location; they change
the location to released, empty the lease, and use the hidden `Node<T>` static
type for move/destruction. A downstream caller cannot name or construct the
node, and cannot reach the private QueuePort inside `AsyncQueue<T>`. There is no
public constructor from `QueueItemControl*`, no public adopt/reset, and no way
to modify `location_` or `type_token_`. A raw control address therefore cannot
be upgraded into authority.

### 4.1 One-shot admission proof

The binding location history is:

```text
typed QueuePort factory:
    create control at detached
    create exactly one non-empty QueueItemLease

QueuePort push entry:
    require lease non-empty
    require control.owner_port_ == this
    require control.location_ == detached
    consume by-value lease; caller source is empty
    detached -> producer_operation

failed push:
    producer_operation -> detached
    return exactly the original control in one lease

committed push:
    producer_operation -> ring
    move lease into an empty ring slot
    operation/result contain no lease

pop commit:
    ring -> consumer_operation
    move lease out; source ring slot becomes empty

typed extraction:
    validate owner_port_ and type_token_
    consumer_operation/detached -> released
    release control, move T once, delete exact factory Node<T> outside locks

teardown:
    ring -> teardown -> released
    typed layer deletes the exact node outside locks
```

Every move construction and every allowed move assignment uses
`std::exchange`, so its source is empty. A second `try_push(std::move(lease))`
receives an empty lease and fails the entry contract before any state mutation.
`owner_port_ == this` is only the identity check; the non-empty lease and
`location_ == detached` jointly prove admission authority.

### 4.2 Ring ownership representation

The fixed ring is allocated once at Queue construction:

```cpp
std::unique_ptr<QueueItemLease[]> ring_;
```

Each slot starts empty. Every transfer requires an empty destination and uses
move construction/empty-destination move assignment. Consequently:

```text
source becomes empty
destination was empty
one control cannot occupy two slots
all non-empty ring leases have location == ring
all ring ItemIds are unique by construction
ring transfer allocates nothing and deletes nothing
```

The control is embedded in the typed node; the ring stores only the exclusive
non-deleting lease. QueueCore never destroys a payload. Typed result conversion
or teardown changes the location to `released`, empties the lease, and destroys
the exact typed node after all locks are released. Debug assertions may mirror
these rules but are not their enforcement mechanism.

## 5. Opaque and public result contracts

The non-template push seam consumes a lease by value:

```cpp
enum class QueueOpaquePushStatus : std::uint8_t {
    committed,
    closed,
    expired,
    would_block,
};

class QueueOpaquePushResult final {
public:
    static QueueOpaquePushResult committed() noexcept;
    static QueueOpaquePushResult failed(
        QueueOpaquePushStatus,
        QueueItemLease&&) noexcept;

    QueueOpaquePushResult(QueueOpaquePushResult&&) noexcept;
    QueueOpaquePushResult& operator=(QueueOpaquePushResult&&) noexcept;

    QueueOpaquePushResult(const QueueOpaquePushResult&) = delete;
    QueueOpaquePushResult& operator=(const QueueOpaquePushResult&) = delete;

    QueueOpaquePushStatus status() const noexcept;
    QueueItemLease take_failed_lease() && noexcept;

private:
    QueueOpaquePushStatus status_;
    QueueItemLease lease_;

    QueueOpaquePushResult(QueueOpaquePushStatus s, QueueItemLease&& l) noexcept
        : status_(s), lease_(std::move(l)) {}
};

class QueuePort final {
public:
    QueueOpaquePushResult try_push(QueueItemLease);
    QueueOpaquePushResult push(QueueItemLease);
    QueueOpaquePushResult push_until(
        QueueItemLease,
        Scheduler::deadline_t);
};
```

The result invariant is checked at every factory and move boundary:

```text
status == committed     <=> lease is empty
status != committed     <=> lease is non-empty
```

Move assignment swaps complete valid result states; it never overwrites a live
lease. `take_failed_lease()` changes the result to a valid moved-from
no-payload state. There is no pointer copy, raw-pointer reconstruction,
borrowed item reference, or failed-payload destruction by QueuePort.

The typed failure path is fixed:

```text
QueueOpaquePushResult
-> move out the unique QueueItemLease
-> validate owner_port_ and queue_type_token<T>()
-> recover exact hidden factory Node<T>
-> change detached -> released
-> move T exactly once into QueuePushResult<T>
-> delete factory Node<T> outside all locks
```

The public failed-result factory is:

```cpp
static QueuePushResult failed(Status, T&& value) noexcept;
```

It is not a by-value factory. The result uses destroy-and-move-construct for
move assignment, so `T` need not be move-assignable. It preserves:

```text
T is nothrow move-constructible
T is nothrow destructible
self-move leaves a valid state
moved-from result has a valid no-payload status
typed node -> public failed result performs one necessary T move
```

The opaque pop result is the symmetric move-only pair: it owns a non-empty
lease iff its status is `item`; closed/expired/would-block carry no lease.

## 6. Queue lifecycle and teardown authority

Close state and object lifecycle are separate:

```cpp
enum class QueueLifecycle : std::uint8_t {
    operational,
    tearing_down,
};
```

Ordinary `take_quiescent_item()` is removed. QueuePort instead exposes one
irreversible transition:

```cpp
class QueueTeardownSession final {
public:
    QueueTeardownSession(QueueTeardownSession&& other) noexcept
        : port_(std::exchange(other.port_, nullptr)) {}

    QueueTeardownSession& operator=(QueueTeardownSession&&) = delete;
    QueueTeardownSession(const QueueTeardownSession&) = delete;
    QueueTeardownSession& operator=(const QueueTeardownSession&) = delete;

    ~QueueTeardownSession() noexcept;

    QueueItemLease take_next() noexcept;
    bool empty() const noexcept;

private:
    QueuePort* port_{nullptr};

    explicit QueueTeardownSession(QueuePort& port) noexcept
        : port_(&port) {}

    friend class QueuePort;
};

class QueuePort final {
public:
    QueueTeardownSession begin_teardown() noexcept;
};
```

`begin_teardown()` does not enter ordinary CallGuard. Under G+S it requires:

```text
lifecycle == operational
active_port_calls_ == 0
active_wait_associations_ == 0
active_queue_timers_ == 0
granted_not_resumed_ == 0
producer WaitQueue empty
consumer WaitQueue empty
no live teardown authority
```

It then performs `operational -> tearing_down` and returns the only session.
The lifecycle transition serializes against ordinary call entry: an earlier
ordinary call makes `active_port_calls_ != 0`; an earlier teardown makes every
later ordinary entry fail-fast before CallGuard construction.

Once tearing down, the port admits no push/pop/try/timed call, close, snapshot,
second teardown session, waiter, timer, or ticket. `take_next()` requires the
session still owns the port and lifecycle is `tearing_down`; under G+S it moves
one non-empty ring slot into the result and changes `ring -> teardown`. The
source slot becomes empty. The typed layer changes `teardown -> released` and
destroys the exact typed node outside locks.

Session move clears its source. Copy, public construction, and second session
creation are impossible. Session destruction is no-throw and fail-fast unless
the ring is empty; it then marks the unique authority complete. QueueCore
destruction is permitted only after the ring is empty and the teardown session
has completed. Concurrent `AsyncQueue<T>` destruction remains a caller contract
violation; the counters do not make arbitrary caller lifetime safe.

## 7. Ordinary call guard and counter ledger

`active_calls_` is removed. The exact counter is:

```text
active_port_calls_ =
    Number of ordinary QueuePort operations that have entered the
    non-template Queue authority and have not yet returned from QueuePort.
```

It does not mean that the complete public `AsyncQueue<T>` method is still
running. Typed result conversion and typed node destruction occur after
QueuePort return and are not counted.

CallGuard covers:

```text
push, push_until, try_push
pop, pop_until, try_pop
close
snapshot (including is_closed/size projection)
```

CallGuard decrements on normal inline return, failed-result return, suspended
resume return, timer-preparation exception, validation exception if such
exceptions are admitted, and every early branch. It does not cover
`begin_teardown`, `QueueTeardownSession::take_next`, typed conversion, or typed
destruction.

| Counter | Increment authority | Decrement authority | Exact zero meaning |
| --- | --- | --- | --- |
| `active_port_calls_` | ordinary QueuePort CallGuard after lifecycle check | same guard at QueuePort return/unwind | no ordinary operation is inside QueuePort |
| `active_wait_associations_` | successful P5/C4 link | the unique winner immediately after unlink | no linked Queue wait epoch |
| `active_queue_timers_` | PREPARED->ACTIVE activation | ACTIVE->RETIRED/CONSUMED winner | no ACTIVE Queue timer |
| `granted_not_resumed_` | one suspended-winner ticket publication | worker removes ticket and operation resumes/releases owner slot | no published suspended Queue operation awaiting resume |

The four zeros do not prove all typed public methods returned, all arbitrary
callers disappeared, or concurrent destruction is safe.

Transition deltas use `(A,W,T,R)` for these four counters:

| Event | Delta |
| --- | --- |
| ordinary QueuePort entry/return | `A +1/-1` |
| timer preparation exception | `A -1` by CallGuard; others unchanged |
| P5/C4 link | `W +1`, `T +1` iff timed activation |
| P6/P7/C5/C6 winner | `W -1`, `T -1` iff ACTIVE |
| registered P9/C8 timer winner | `W -1`, `T -1` |
| matching suspended publication | `R +1` |
| ticket removal + resume owner-slot release | `R -1` |
| immediate result | only eventual `A -1` |

## 8. PREPARED timer physical representation

> **SUPERSESSION NOTICE (E10-E12-API-SEMANTIC-CLOSURE-1, finding F1).** The
> `PreparedQueueTimer` design described in this section is **SUPERSEDED** for the
> production implementation. The authority for the queue timer model is
> `docs/e12-queue-corrective-3.md`, which records that production never
> implemented `PreparedQueueTimer`: timers are constructed directly ACTIVE via
> the generic `TimerRegistration` path with an `on_resolve_` hook, and the
> PREPARED/ACTIVE split below is preserved here only as the historical design
> record that Corrective-3 supersedes. No code change resulted from this notice;
> it clarifies document authority only. See
> `docs/e10-e12-api-semantic-closure.md` finding F1 for the cross-document
> trace.

The concrete pool remains `std::list<TimerRegistration>`. PREPARED and ACTIVE
registrations occupy the same list element. List insertion does not relocate
existing elements and does not invalidate their iterators/references; erasing
an element invalidates only that element. This exact representation, not a
generic “stable container” assumption, supports the guard:

```cpp
class PreparedQueueTimer final {
public:
    PreparedQueueTimer(PreparedQueueTimer&& other) noexcept
        : scheduler_(std::exchange(other.scheduler_, nullptr)),
          iterator_(other.iterator_),
          armed_(std::exchange(other.armed_, false)) {}

    PreparedQueueTimer& operator=(PreparedQueueTimer&&) = delete;
    PreparedQueueTimer(const PreparedQueueTimer&) = delete;
    PreparedQueueTimer& operator=(const PreparedQueueTimer&) = delete;

    ~PreparedQueueTimer() noexcept;

private:
    Scheduler* scheduler_{nullptr};
    TimerPool::iterator iterator_{};
    bool armed_{false};

    PreparedQueueTimer(Scheduler& sched, TimerPool::iterator it, bool armed) noexcept
        : scheduler_(&sched), iterator_(it), armed_(armed) {}

    friend class Scheduler;
};
```

Only one guard is armed. Move clears the source. Discard erases the PREPARED
list element then disarms the guard. Activation mutates that same element to
ACTIVE, inserts its deadline-heap pointer using reserved capacity, increments
`active_queue_timers_`, and disarms the guard. Neither path can allocate or
throw. The destructor is no-throw; if armed it rolls back only while G remains
held.

The binding local declaration order is:

```cpp
GlobalLockGuard global_lock(global_mtx_);
PreparedQueueTimer prepared = prepare_queue_timer_locked(deadline);

// admission, then explicit discard or activation
```

C++ destroys automatic objects in reverse construction order. Therefore
`prepared` is destroyed before `global_lock` on exception and every early
return. The reverse declaration order is forbidden.

### 8.1 Lease consumption after fallible preparation

All fallible preparation (allocation, timer-pool emplace, deadline-heap
reserve) must complete BEFORE consuming the `QueueItemLease` by value. The
binding push_until sequence is:

```text
1. prepare_queue_timer_locked() — may throw allocation exceptions
2. consume the lease by value into QueuePort::push_until parameter
3. perform admission under G+S
4. explicit discard or activation
```

If `prepare_queue_timer_locked()` throws, the lease remains with the caller
and is not consumed. The `QueueItemLease` destructor is fail-fast and never
deletes a typed node; therefore the lease MUST NOT be consumed before
preparation completes. This ordering is enforced by the declaration sequence:
the lease is passed by value only after the prepared timer is constructed.

Generic and Queue constructors remain distinct:

```cpp
static TimerRegistration make_active_generic(/* existing context */);

static TimerRegistration make_prepared_queue(
    Scheduler::deadline_t) noexcept;
```

Queue PREPARED starts as:

```text
state = PREPARED
target_kind = none
node = nullptr
queue = nullptr
queue_wait = nullptr
not deadline-heap linked
pump invisible
not counted by active_queue_timers_
```

The existing generic constructor still creates ACTIVE with its original bound
target and heap semantics; no generic path passes through PREPARED.

Scheduler operations are:

```cpp
PreparedQueueTimer
prepare_queue_timer_locked(deadline_t);

void activate_queue_timer_locked(
    PreparedQueueTimer&,
    QueueWaitRegistration&) noexcept;

void discard_prepared_timer_locked(
    PreparedQueueTimer&) noexcept;
```

Preparation may reserve deadline-heap capacity, emplace the list element, and
throw allocation exceptions before registration. Activation requires a linked
registration and complete context, changes PREPARED->ACTIVE at the same
address, increments the active counter, performs allocation-free heap
insertion, then disarms once. Discard requires PREPARED and no heap link,
erases the list element, leaves the active counter unchanged, then disarms
once.

## 9. Stable owner-slot invariant

Before Queue wait registration, Scheduler finds the existing `fiber_owner_`
entry under G, validates the captured owner, and stores the address of its
mapped value in the operation-embedded ticket:

```text
Before Queue registration:
    fiber_owner_ contains the Fiber entry.

Mapped-value address remains valid until all three have happened:
    1. Queue ticket removed;
    2. Fiber resumed;
    3. Queue operation no longer stores/references the slot.

No Scheduler path erases that element during this interval.
All mapped-value reads and writes occur under global_mtx_.
```

`std::unordered_map` rehash invalidates iterators but does not invalidate
references/pointers to elements. Erasing the referenced element would
invalidate the slot and is explicitly forbidden. Fiber completion cleanup must
therefore occur after the Queue operation releases the slot. Stealing writes
only the existing mapped value; it performs no lookup, `operator[]`, insert,
erase, rehash, or allocation. Winner publication neither reads nor updates the
owner map.

## 10. Queue runnable publication and consumption

Winner publication under G performs only:

```text
use the stable owner slot captured at admission
Fiber::make_runnable
append the operation-embedded ticket to the global Queue ticket list
increment ticket/runnable accounting
clear global_terminate_
transition idle/wake epoch as required
```

The winner does not decide the eventual execution Worker.

Worker consumption under G is exactly:

```cpp
QueueRunnableTicket*
pop_queue_runnable_locked(WorkerState& current) noexcept {
    if (auto* own = find_oldest_owned_ticket_locked(current)) {
        unlink_queue_ticket_locked(*own);
        return own;
    }

    auto* stolen = queue_runnable_head_;
    if (stolen == nullptr) {
        return nullptr;
    }

    unlink_queue_ticket_locked(*stolen);
    *stolen->fiber_owner_slot_ = &current;
    return stolen;
}
```

Binding selection semantics:

1. choose the current Worker's oldest Queue ticket if one exists;
2. otherwise choose the global oldest Queue ticket;
3. the original admission owner's active/inactive state never affects
   eligibility;
4. claim, unlink, owner-slot update, ticket/runnable accounting, and worker
   selection state all occur under G;
5. unlink and any owner-slot update precede `run_next_on`/`make_running`.

The own-ticket preference intentionally outranks an older foreign ticket.
Without an own ticket, global FIFO chooses the head even when its admission
owner is active.

| Scheduler dimension | Publication mapping | Consumption mapping |
| --- | --- | --- |
| `global_terminate_` | cleared because runnable work exists | cannot become true while any Queue ticket remains |
| idle state | published work makes an idle Worker wake-eligible | selected Worker leaves idle classification |
| admission state | cancel/resolve candidate admission as existing runnable route requires | no stale admission remains when running |
| runnable classification | Queue ticket is runnable work | removed ticket becomes running work |
| `runnable_count` | increment exactly once with ticket append | decrement/transfer exactly once with unlink |
| ticket count | increment on append | decrement on unlink |
| wake epoch | signal after append/state update | consumption observes the published epoch |
| old owner metadata | immutable admission owner retained for diagnostics/preference | never erased or used as an eligibility veto |
| new owner metadata | unchanged at publication | stolen ticket writes existing mapped owner slot to current |
| worker selection | winner chooses none | own-oldest else global-oldest |

Deterministic required trace:

```text
F waits on Queue while owned by W0
W0 runs a blocker until F resumes
an external thread publishes F
W1 is idle/available
W0 remains active

W1 may claim the global-oldest Queue ticket, update F's stable owner slot
under G, unlink it, and resume F.
```

Any rule that waits for W0 to become inactive is a design defect.

## 11. Locking, lifetime, allocation, and exception boundaries

Lock order is fixed:

```text
G = Scheduler::global_mtx_
S = QueueCore::state_mtx_
P = producer WaitQueue mutex
C = consumer WaitQueue mutex

G -> S -> exactly one of P/C
G -> wake mutex
P and C are never held together
```

The authoritative synchronous `Mutex` dependency is no-throw/fail-fast; the
current production substrate is not yet authorized to satisfy that dependency.
No payload destructor, move, allocator, deleter callback, virtual dispatch, or
type-erased user-code call occurs under G/S/P/C.

| Resource | Allocation point | Locks | Winner relation | Failure policy |
| --- | --- | --- | --- | --- |
| fixed lease ring | Queue construction | none | before calls | construction throws |
| typed factory Node<T> | typed push factory | none | before port entry | allocation/construction throws |
| operation/wait/ticket | caller stack/embedded | none | before registration | no allocation |
| timer heap reserve/list block | PREPARED preparation | G | before registration | may throw; guard rolls back |
| timer activation | after registration | G+S+role | before suspension | no allocation/no throw |
| runnable ticket append | embedded intrusive link | G | after winner commit | no allocation/no throw |
| opaque/public result | return storage | outside locks for T | after port return | no heap; T move/dtor no-throw |

The empty sets are binding:

```text
allocation after winner CAS: NONE
recoverable exception after winner CAS: NONE
payload destruction under Queue/Scheduler locks: NONE
owner-map insertion/erase/lookup during publication or steal: NONE
```

Scheduler outlives Queue, all operations, tickets, timers, and teardown.
Operations outlive their WaitNode, owner-slot reference, and ticket. A ticket
is unlinked before operation resume/destruction. ACTIVE timers retire or are
consumed before the operation frame disappears.

## 12. Reconciliation and publication ordering

With G+S held, reconciliation takes at most one role lock and repeatedly
applies the first eligible action:

```text
Closed + producer head:
    P7 -> PUB-P-CLOSED

ring nonempty + consumer head:
    C5 -> PUB-C-COMM

Closed + ring empty + consumer head:
    C6 -> PUB-C-CLOSED

Open + ring space + producer head:
    P6 -> PUB-P-COMM

otherwise:
    fixed point
```

For every suspended winner:

```text
winner CAS
-> unlink
-> timer retire/consume
-> lease/control location transfer or retention
-> completion write
-> counter update
-> make_runnable
-> ticket append
-> follow-up reconciliation
```

No terminal linked head is skipped or repaired. Observation is an invariant
failure. New admission cannot barge because G+S remain held to the fixed point.

## 13. Canonical transition coverage — author-verified 19/19

Legend: `A/W/T/R` are the four counters from §7. `slot` is the admission-
captured mapped-value address. Every row assumes destination lease storage is
empty and preserves unique ring ItemIds.

| ID | Exact authority | Control location before -> after / uniqueness | Locks | WaitNode / timer | Completion and counters | Owner slot, publication, steal | Follow-up | Allocation / exception / lifetime |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| P1 | typed QueuePort factory | none -> detached; one lease minted | none | none / none | none; counters 0 | no slot/publication | enter QueuePort | node allocation may throw before admission; typed node live |
| P2 | push admission | detached -> producer_operation -> ring; source op empty, unique slot | G+S | detached / PREPARED discarded or none | committed; A remains | no slot; inline, no steal | reconcile | no allocation/throw after entry; node/ring live |
| P3 | push admission | detached -> producer_operation -> detached; exact lease returned | G+S | detached / PREPARED discarded or none | closed; A remains | no slot/publication | return | no allocation/throw; caller result owns node |
| P4 | try-push admission | detached -> producer_operation -> detached; exact lease returned | G+S | detached / none | would_block; A remains | no slot/publication | return | no allocation/throw; caller result owns node |
| P5 | timed/blocking admission | detached -> producer_operation; no ring duplicate | G+S+P | Detached->Registered / PREPARED->ACTIVE iff timed | pending; W+1,T+1? | slot captured before link; not published | suspend | preparation alone may throw before link; operation/Fiber/core live |
| P6 | reconciler | producer_operation -> ring; operation emptied, unique ring ItemId | G+S+P | Registered->Woken->unlinked / ACTIVE->RETIRED | committed; W-1,T-1?,R+1 at publication | existing slot; PUB-P-COMM; active-owner steal eligible | reconcile | no allocation/throw; op lives through resume |
| P7 | reconciler | producer_operation -> detached lease retained by op for result | G+S+P | Registered->Woken->unlinked / ACTIVE->RETIRED | closed; W-1,T-1?,R+1 | existing slot; PUB-P-CLOSED; active-owner steal eligible | reconcile | no allocation/throw; op/result lifetime |
| P9 | admission or timer pump | detached/producer_operation retained; never enters ring | G+S inline or G+S+P | Detached or Registered->Expired->unlinked / PREPARED discard or ACTIVE->CONSUMED | expired; inline A only, registered W-1,T-1,R+1 | registered uses existing slot and PUB-P-EXPIRE; steal eligible | return/reconcile | prep may throw only before link; registered path no throw |
| P10 | QueuePort then typed layer | committed empty; failed producer_operation->detached->released | brief G+S on resume, then none | terminal/unlinked / closed | observed; A-1,R-1 iff resumed | ticket already unlinked; slot released after resume | typed conversion | one T move/delete outside locks; exact node lifetime ends |
| C1 | pop admission | ring -> consumer_operation; source slot empty, uniqueness preserved | G+S | detached / PREPARED discarded or none | committed; A remains | no slot/publication | reconcile | no allocation/throw; node held by operation |
| C2 | pop admission | no lease -> no lease | G+S | detached / PREPARED discarded or none | closed; A remains | no slot/publication | return | no allocation/throw; core/call live |
| C3 | try-pop admission | ring unchanged | G+S | detached / none | would_block; A remains | no slot/publication | return | no allocation/throw |
| C4 | timed/blocking admission | empty consumer operation; ring unchanged | G+S+C | Detached->Registered / PREPARED->ACTIVE iff timed | pending; W+1,T+1? | slot captured before link; not published | suspend | preparation may throw before link; operation/Fiber/core live |
| C5 | reconciler | ring -> consumer_operation; source slot empty, unique ItemId | G+S+C | Registered->Woken->unlinked / ACTIVE->RETIRED | committed; W-1,T-1?,R+1 | existing slot; PUB-C-COMM; active-owner steal eligible | reconcile | no allocation/throw; op owns node through resume |
| C6 | reconciler | no lease -> no lease | G+S+C | Registered->Woken->unlinked / ACTIVE->RETIRED | closed; W-1,T-1?,R+1 | existing slot; PUB-C-CLOSED; active-owner steal eligible | reconcile | no allocation/throw; op/core live |
| C8 | admission or timer pump | ring unchanged; consumer remains empty | G+S inline or G+S+C | Detached or Registered->Expired->unlinked / PREPARED discard or ACTIVE->CONSUMED | expired; inline A only, registered W-1,T-1,R+1 | registered uses existing slot and PUB-C-EXPIRE; steal eligible | return/reconcile | prep may throw only before link; registered no throw |
| C9 | QueuePort then typed layer | consumer_operation -> released; ring already empty at source | brief G+S on resume, then none | terminal/unlinked / closed | observed; A-1,R-1 iff resumed | ticket unlinked; slot released after resume | typed result | one T move/delete outside locks; exact node ends |
| CL1 | close caller | operational Queue remains operational; ring leases retained/drained only by C5 | G+S then one role | eligible waiters use P7/C5/C6 / timers retire | Open->Closed; A+1/-1 plus winner deltas | each winner publishes stealable ticket through existing slot | closed fixed point | no allocation/throw; core lives through call |
| CL2 | close caller | Closed->Closed; ring uniqueness unchanged | G+S then one role | same winner rules / timers retire | idempotent; A+1/-1 plus winner deltas | each winner stealable, owner activity irrelevant | re-reconcile | no allocation/throw; core lives through call |

Reserved and excluded from the v1 count:

```text
P8 ProducerCancel — RESERVED / DEFERRED / NOT IN QUEUE V1
C7 ConsumerCancel — RESERVED / DEFERRED / NOT IN QUEUE V1
```

## 14. Publication transition coverage — author-verified 6/6

| ID | Exact authority and predecessor | Location / uniqueness | Locks, Wait, timer | Completion / counters | Owner slot, publication, steal | Follow-up / allocation / lifetime |
| --- | --- | --- | --- | --- | --- | --- |
| PUB-P-COMM | P6 winner after unlink | ring owns unique lease; op empty | G+S+P; Woken/off; retired-or-none | committed; R+1 | captured slot; ticket appended; active owner never vetoes steal | release P, reconcile; no allocation/throw; op live |
| PUB-P-CLOSED | P7 winner after unlink | op retains unique lease | G+S+P; Woken/off; retired-or-none | closed; R+1 | captured slot; active-victim steal allowed | release P, reconcile; no allocation/throw; op live |
| PUB-P-EXPIRE | registered P9 winner | op retains unique lease | G+S+P; Expired/off; consumed | expired; R+1 | captured slot; active-victim steal allowed | release P, reconcile; no allocation/throw; op live |
| PUB-C-COMM | C5 winner after unlink | consumer op owns unique lease, ring source empty | G+S+C; Woken/off; retired-or-none | committed; R+1 | captured slot; active-victim steal allowed | release C, reconcile; no allocation/throw; op live |
| PUB-C-CLOSED | C6 winner after unlink | no lease | G+S+C; Woken/off; retired-or-none | closed; R+1 | captured slot; active-victim steal allowed | release C, reconcile; no allocation/throw; op live |
| PUB-C-EXPIRE | registered C8 winner | ring unchanged, op empty | G+S+C; Expired/off; consumed | expired; R+1 | captured slot; active-victim steal allowed | release C, reconcile; no allocation/throw; op live |

No publication row performs an owner-map lookup/update, chooses an execution
Worker, destroys payload, allocates, or throws.

```text
AUTHOR SELF-ASSESSMENT
canonical target coverage: 19/19
publication target coverage: 6/6
INDEPENDENT REVIEW REQUIRED
```

## 15. Teardown is outside the canonical operation machine

Teardown is a separate irreversible lifecycle protocol, not a twentieth
canonical Queue operation and not a close transition:

```text
operational + quiet counters/queues
-> begin_teardown
-> tearing_down + unique session
-> repeated ring -> teardown -> released
-> empty ring + completed session
-> QueueCore destruction
```

No ordinary CallGuard or publication transition participates.

## 16. Adversarial trace disposition

Each disposition is a design claim, not production evidence.

| # | Scenario | Disposition |
| ---: | --- | --- |
| 1 | same lease used for two `try_push` calls | `COUNTEREXAMPLE BLOCKED BY:` first by-value move empties source; second entry rejects empty lease |
| 2 | source lease used after move | `COUNTEREXAMPLE BLOCKED BY:` `std::exchange` leaves source empty and entry requires non-empty |
| 3 | another QueuePort receives the lease | `COUNTEREXAMPLE BLOCKED BY:` immutable `owner_port_` mismatch before location mutation |
| 4 | raw control pointer forges a lease | `COUNTEREXAMPLE BLOCKED BY:` constructor/adopt are private; QueuePort is the only mint authority |
| 5 | committed item also appears in failed result | `COUNTEREXAMPLE BLOCKED BY:` committed iff result lease empty; ring owns the moved lease |
| 6 | one control occupies two ring slots | `COUNTEREXAMPLE BLOCKED BY:` move-only lease plus empty-destination transfer empties source |
| 7 | failed push does not return lease | `COUNTEREXAMPLE BLOCKED BY:` every non-committed opaque status requires one non-empty lease |
| 8 | failed push returns an alias | `COUNTEREXAMPLE BLOCKED BY:` result receives the original moved lease; no raw reconstruction API exists |
| 9 | pop leaves source ring slot occupied | `COUNTEREXAMPLE BLOCKED BY:` ring-to-operation move empties the source before commit |
| 10 | teardown races ordinary admission | `COUNTEREXAMPLE BLOCKED BY:` G+S serializes lifecycle check/CallGuard entry against irreversible transition |
| 11 | active owner W0 blocks W1 steal | `COUNTEREXAMPLE BLOCKED BY:` owner activity is not an eligibility predicate; W1 may take global oldest |
| 12 | current Worker has own ticket and older foreign ticket | `COUNTEREXAMPLE BLOCKED BY:` own-oldest rule intentionally wins before global-oldest |
| 13 | no own ticket, foreign global oldest exists | `COUNTEREXAMPLE BLOCKED BY:` global head is selected regardless of admission-owner activity |
| 14 | stealing erases owner entry | `COUNTEREXAMPLE BLOCKED BY:` no-erase interval is a binding lifetime invariant |
| 15 | stealing inserts into owner map | `COUNTEREXAMPLE BLOCKED BY:` ticket stores mapped-value address; steal writes it directly under G |
| 16 | `run_next_on` before ticket unlink | `COUNTEREXAMPLE BLOCKED BY:` claim/unlink/accounting/owner update all precede run under G |
| 17 | active old owner rejects every foreign claim | `COUNTEREXAMPLE BLOCKED BY:` activity check is deleted from eligibility |
| 18 | termination true while Queue ticket exists | `COUNTEREXAMPLE BLOCKED BY:` ticket count participates in classification and publication clears termination |
| 19 | PREPARED guard outlives G guard | `COUNTEREXAMPLE BLOCKED BY:` mandatory declaration order destroys prepared first |
| 20 | two armed guards after move | `COUNTEREXAMPLE BLOCKED BY:` move exchanges `armed_` to false in source |
| 21 | immediate commit leaves PREPARED | `COUNTEREXAMPLE BLOCKED BY:` explicit discard or armed destructor erases same list element |
| 22 | immediate Closed leaves PREPARED | `COUNTEREXAMPLE BLOCKED BY:` same discard/RAII path before return |
| 23 | activation allocates | `COUNTEREXAMPLE BLOCKED BY:` heap capacity reserved during preparation; activation is noexcept |
| 24 | pool growth invalidates address/iterator | `COUNTEREXAMPLE BLOCKED BY:` concrete pool is `std::list`; growth probe checks address and iterator |
| 25 | generic timer starts PREPARED | `COUNTEREXAMPLE BLOCKED BY:` distinct active-generic and prepared-Queue factories |
| 26 | discard changes active timer counter | `COUNTEREXAMPLE BLOCKED BY:` PREPARED is uncounted; discard delta is zero |
| 27 | armed guard erases twice | `COUNTEREXAMPLE BLOCKED BY:` discard/activation disarm exactly once; moved source is disarmed |
| 28 | begin teardown with active port call | `COUNTEREXAMPLE BLOCKED BY:` precondition requires `active_port_calls_ == 0` under G+S |
| 29 | snapshot after teardown | `COUNTEREXAMPLE BLOCKED BY:` ordinary entry rejects `tearing_down` before CallGuard |
| 30 | teardown session copied | `COUNTEREXAMPLE BLOCKED BY:` copy construction/assignment are deleted |
| 31 | two teardown sessions live | `COUNTEREXAMPLE BLOCKED BY:` lifecycle is absorbing and constructor private |
| 32 | teardown drain enters CallGuard | `COUNTEREXAMPLE BLOCKED BY:` begin/take_next are explicitly outside ordinary guard |
| 33 | four zeros prove all typed methods returned | `COUNTEREXAMPLE BLOCKED BY:` counter definitions explicitly exclude typed conversion/destruction and arbitrary callers |

At the document-design level there is no unresolved Critical/Required trace.
Production proof still requires implementation, tests, and independent review;
none is authorized here.

## 17. Cross-document normalization obligations

The current authority set must agree on all of the following:

```text
Corrective-1 = SUPERSEDED — REQUEST-CHANGES
Corrective-2 = current document authority
Queue v1 = no Permit/reservation owner
push failure = exact typed payload recovered through one complete opaque lease
active-victim ticket stealing = allowed
teardown = irreversible session, not ordinary drain
active_port_calls_ = non-template QueuePort interval only
formal/TLA status = not updated, not PASS evidence
Condition T25 = separate unresolved baseline task
```

## 18. Language and representation probes

All probes were scratch files under `/tmp` and were not added to the
repository. Fresh syntax-only results:

```text
clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only \
  /tmp/e12_queue_lease_positive.cpp
EXIT: 0
ASSERTS: move clears source; failed opaque result returns the identical
         control; foreign-port identity is rejected; moved-from reuse is
         rejected; release empties the final lease.

clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only \
  /tmp/e12_queue_lease_negative.cpp
EXIT: 1 (EXPECTED)
RESULT: 12 compile errors covering deleted lease copy, private raw-control
        constructor, private location, by-value lvalue resubmission, private
        Queue port, private teardown constructor, deleted teardown copy,
        private reconciliation, private ticket, and private owner-map mutation.

clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only \
  /tmp/e12_queue_teardown_probe.cpp
EXIT: 0
ASSERTS: session is move-only; move empties source; lifecycle is irreversible;
         snapshot is rejected; one session drains to empty.

clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only \
  /tmp/e12_queue_timer_prepared_probe.cpp
EXIT: 0
ASSERTS: exact Prepared guard declaration compiles; move disarms source;
         move assignment/copy are deleted.

clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only \
  /tmp/e12_queue_result_probe.cpp
EXIT: 0
ASSERTS: non-move-assignable T is supported; result remains move-assignable;
         failed(Status,T&&) performs one move; self-move is valid; moved-from
         result has no payload.
```

Fresh warning-enabled executable builds for the four positive probes each
returned exit 0. Running all four executables also returned exit 0. The timer
runtime assertions established:

```text
existing ACTIVE and PREPARED list addresses/iterators survive pool growth
immediate committed/Closed/expired and pre-registration exception leave:
    no PREPARED registration
    no new heap entry
    active_queue_timers_ unchanged
Prepared address == activated ACTIVE address
activation performs zero allocator calls after reserve
activation/discard disarm exactly once
Prepared destructor observes G still held under required declaration order
generic constructor still starts ACTIVE and heap-linked
```

## 19. Current verdict and authorization

The declarations, row recount, cross-document normalization, scratch probes,
whitespace check, and scope check support the author self-assessment below.

```text
E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2:
PASS

TARGET COVERAGE:
19 canonical transitions
6 publication transitions

VERIFIED COVERAGE — AUTHOR SELF-ASSESSMENT:
19/19 canonical transitions
6/6 publication transitions

AUTHOR SELF-ASSESSMENT
INDEPENDENT ADVERSARIAL REVIEW REQUIRED

E12-E IMPLEMENTATION AUTHORIZATION: DENIED
```
