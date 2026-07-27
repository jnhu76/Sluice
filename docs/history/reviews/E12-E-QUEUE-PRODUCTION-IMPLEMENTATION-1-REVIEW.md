# E12-E Queue Production Implementation — Independent Adversarial Review 1

> **Decision under review:** `E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1`
> **Reviewer:** independent (Phase I)
> **Date:** 2026-07-19
> **Branch:** `e12-e-queue-production-impl`
> **Commits reviewed:** `ffa515b..ae549f0`

## Verdict

**BLOCKED**

The author's Phase H self-assessment claims PASS. That claim is **rejected**. The
implementation contains a **BLOCKING counter-integrity defect** (F.1) that
makes `begin_teardown` fail-fast (`std::terminate`) after any `push_until` /
`pop_until` wait that resolves via timer expiry through the worker pump. This
defect is **reproduced deterministically** (5/5 runs, exit code 42 from a
`std::terminate` handler) on Clang Debug. The 20-test Phase G suite passes only
because no test calls `begin_teardown` after a timer-expiry path — the G1
cases stop one step short of the failing interaction.

Two additional MAJOR defects (F.2, F.3) show that two of the four binding
teardown-precondition counters (`active_queue_timers_`, `granted_not_resumed_`)
are **never incremented or decremented anywhere in the codebase** — they are
dead counters, so the corresponding `begin_teardown` preconditions are
unenforced. A fourth MAJOR (F.4) is a TOCTOU in the ordinary-op lifecycle gate
that breaks the authority's §6/§7 serialization claim.

The fast-path (P2/P3), blocking-admission (P4/P5), close-drain (CL1/CL2), and
teardown-positive (P7) substrates are well-formed and match the authority on
lock order, winner-before-publication, linear capability, and type-structure
concerns (see per-checklist evidence C/K/E/I below). The defects are confined
to the timer-expiry counter ledger, the dead counters, and the lifecycle-gate
locking. The implementation must not merge until F.1-F.4 are corrected and the
G1 tests are extended to call `begin_teardown` after the expiry.

## Findings

### F.1 [BLOCKING] Timer-pump expiry of a Queue wait leaks `active_wait_associations_`, causing `begin_teardown` to terminate

**Evidence.** The four Queue admit closures register the wait and increment
`port.active_wait_associations_` under G+S+role
(`src/async/scheduler.cpp:2197,2246,2294,2360`). On every **inline** resolution
path (admission recheck commit, admission-recheck closed, admission-recheck
already-due-expired), the same critical section decrements
`port.active_wait_associations_`
(`src/async/scheduler.cpp:2208,2215,2256,2262,2311,2320,2330,2377,2385,2394`).
On every **reconciler** resolution path (`queue_grant_consumer_locked`,
`queue_grant_producer_locked`, `queue_cancel`) the decrement is also performed
(`src/async/scheduler.cpp:2448,2480,2420`).

But when a Queue wait's deadline elapses **after** the admit closure has
suspended, the timer is driven by the worker pump `pump_deadlines_locked`
(`src/async/scheduler.cpp:2742-2807`). For each due ACTIVE registration the
pump inlines the resolve under G + `q->mtx()`:

```cpp
// src/async/scheduler.cpp:2786-2800
if (n != nullptr && q != nullptr) {
    LockGuard qlk(q->mtx());
    if (q->expire_locked(*n)) {
        Fiber* f = n->fiber();
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;   // <-- scheduler counter only
        if (f != nullptr && f->make_runnable()) {
            route_runnable_locked(f, g_worker);
            ++won;
        }
    }
}
```

Only `waiting_waitq_count_` (the Scheduler-wide counter) is touched. There is
**no path** from `pump_deadlines_locked` to `--port.active_wait_associations_`.
The standalone `expire_wait` (`src/async/scheduler.cpp:1276-1299`) has the
same omission — but it is in any case bypassed for in-pump expiry to avoid
self-deadlock on `global_mtx_`, so even fixing `expire_wait` would not help.

**Consequence for the binding authority.** Corrective-2 §7 makes
`active_wait_associations_ == 0` a hard precondition for `begin_teardown`
(`src/async/queue_port.cpp:431-434`):

```cpp
if (lifecycle_ != QueueLifecycle::operational ||
    active_port_calls_ != 0 || active_wait_associations_ != 0 ||
    active_queue_timers_ != 0 || granted_not_resumed_ != 0 ||
    !scheduler_.queue_role_waiters_empty_locked(*this)) {
    queue_lease_fail_fast();
}
```

After any `push_until` / `pop_until` that expires via the pump,
`active_wait_associations_ == 1` (per such wait). The next `begin_teardown`
calls `queue_lease_fail_fast` → `std::terminate`
(`src/async/queue_port.cpp:33-35`). The irreversible teardown path is therefore
unusable on any Queue that has ever had a timed wait expire — the central
lifecycle exit for the primitive is broken in production.

**Reproduction.** I wrote a throwaway probe (not committed; the rules forbid
touching `tests/` or `src/`) that mirrors `e12_queue_g1_push_until_expires_recovers_value`
exactly, then calls `port.begin_teardown()`. Built against the same
`libsluice_async_internal_testing.a` / `libsluice_core.a` and the same compile
flags as the test target (`-std=c++20 -DSLUICE_ASYNC_INTERNAL_TESTING -Iinclude
-Itests`):

```
probe: producer push_until returned status=2   (expired)
probe: producer recovered expired value=777
probe: run returned
about to call begin_teardown...
TERMINATE called
EXIT_CODE=42   (5/5 runs, Clang Debug)
```

The symmetric `pop_until` probe is also BLOCKING (exit 42). The probe source
lives at `/tmp/e12_queue_leak_probe.cpp` and `/tmp/e12_queue_leak_probe2.cpp`
and can be rebuilt with:

```
clang++ -g -O0 -std=c++20 -Wall -Wextra -Iinclude -Itests \
  -DSLUICE_ASYNC_INTERNAL_TESTING /tmp/e12_queue_leak_probe.cpp \
  build/linux/x86_64/debug/libsluice_async_internal_testing.a \
  build/linux/x86_64/debug/libsluice_core.a -pthread -o /tmp/e12_queue_leak_probe
```

**Why the test suite misses it.** `e12_queue_g1_push_until_expires_recovers_value`
and `e12_queue_g1_pop_until_expires` exercise the expiry path but stop after
recovering the value; they never call `begin_teardown`. The leak is invisible
to the existing matrix.

**Fix recommendation (reviewer-side; not applied).** The timer-expiry path for
a Queue-registered node must decrement `port.active_wait_associations_`. The
cleanest fix is to recognize Queue-bound expiries inside
`pump_deadlines_locked` (the registration already stores `&port.waiters_[r]`
as the queue pointer; the owning `QueuePort*` is recoverable). Alternatively,
route Queue expiries through a Queue-aware `expire_wait` variant that performs
the per-port decrement under G + role mtx. Either fix must be accompanied by a
test that calls `begin_teardown` after the expiry.

---

### F.2 [MAJOR] `active_queue_timers_` is a dead counter — the §7 / §8 ACTIVE-Queue-timer precondition is unenforced

**Evidence.** `grep -rn "active_queue_timers_"` over `include/` and `src/`
returns exactly three hits: the declaration
(`include/sluice/async/detail/queue_port.hpp:426`), the begin_teardown
precondition check (`src/async/queue_port.cpp:433`), and a comment. There is
no `++port.active_queue_timers_` and no `--port.active_queue_timers_` anywhere
in the codebase. The counter is initialized to 0 and never changes.

**Authority violation.** Corrective-2 §7 makes `active_queue_timers_` one of
the four binding counters, with the increment authority "PREPARED->ACTIVE
activation" and decrement authority "ACTIVE->RETIRED/CONSUMED winner". §8
further binds the increment to the moment a Queue timer transitions to ACTIVE.
Because the counter is dead, `begin_teardown`'s check
`active_queue_timers_ != 0` can never fire, so a Queue with an ACTIVE timer
(which exists — see F.3 for how `timer_pool_.emplace_back` creates ACTIVE
registrations directly) can still pass the teardown precondition. The §6
invariant "no ACTIVE Queue timer" at teardown is not enforced.

**Relation to F.3.** F.3 explains *why* the counter is dead: the
PREPARED-then-ACTIVE Queue timer model from §8 was never implemented. The
production code uses the generic ACTIVE-on-creation `TimerRegistration`
constructor (`timer_pool_.emplace_back(&node, &queue, deadline)` at
`src/async/scheduler.cpp:2296,2362`), so there is no distinct
PREPARED→ACTIVE transition at which §8 mandates the increment.

**Fix recommendation.** Either implement the §8 PREPARED-then-ACTIVE
`PreparedQueueTimer` / `prepare_queue_timer_locked` /
`activate_queue_timer_locked` / `discard_prepared_timer_locked` substrate
(and increment/decrement `active_queue_timers_` at the specified transitions),
or document an explicit deviation from §8 and replace the dead counter with
the Scheduler-wide `active_deadline_count_` filtered to Queue-bound
registrations. As written, the counter is misleading dead code that masks a
real precondition gap.

---

### F.3 [MAJOR] The §8 `PreparedQueueTimer` substrate is absent; Queue timed waits use the generic ACTIVE-on-creation timer

**Evidence.** Corrective-2 §8 binds a concrete Queue-specific timer
representation: `PreparedQueueTimer` (declaration in §8 lines 581-603),
`prepare_queue_timer_locked(deadline_t)`, `activate_queue_timer_locked(...)`,
`discard_prepared_timer_locked(...)`. None of these symbols exist in the
codebase:

```
$ grep -rn "PreparedQueueTimer\|prepare_queue_timer\|activate_queue_timer\|discard_prepared_timer" include/ src/
(no output)
```

The corresponding `Scheduler` private member declarations from §8 are also
absent (`include/sluice/async/scheduler.hpp:693-739` declares the admit and
grant seams only; there is no prepared-timer surface).

Instead, `queue_push_admit_until` and `queue_pop_admit_until` allocate a
generic ACTIVE-on-creation `TimerRegistration` directly under G+S+role
(`src/async/scheduler.cpp:2296-2300` and `2362-2366`):

```cpp
timer_pool_.emplace_back(&node, &port.waiters_[0], deadline);
reg = &timer_pool_.back();
++active_deadline_count_;
heap_push_locked(reg);
recompute_earliest_deadline_locked();
```

**Authority violations.**

1. **§8.1 lease-consumption ordering.** §8.1 mandates that all fallible
   preparation (allocation, timer-pool emplace, deadline-heap reserve) complete
   BEFORE the `QueueItemLease` is consumed by value, with the binding
   declaration order `GlobalLockGuard global_lock; PreparedQueueTimer prepared
   = prepare_queue_timer_locked(deadline);` followed by lease consumption. The
   production code instead performs the allocation AFTER the lease has been
   moved into the producer_operation state and UNDER G+S+P, so an allocation
   failure mid-admit would leave the lease stranded inside the operation with
   the timer half-registered. (In practice `std::list::emplace_back` and the
   heap reserve rarely throw, but the §8.1 ordering was made binding precisely
   to make that argument structural rather than probabilistic.)

2. **§11 allocation boundary.** The §11 table row "timer heap reserve/list
   block" lists Locks = "G" (global only, before registration). The production
   code holds G+S+role during the allocation.

3. **§2 / §13 counter mapping.** The T+1 / T-1 deltas in §13's transition
   table are predicated on the §8 PREPARED→ACTIVE / ACTIVE→RETIRED model.
   Without that model, the per-Queue `active_queue_timers_` counter (F.2)
   cannot be wired.

**Mitigating observation.** The generic ACTIVE-on-creation path inherits the
E11 timer-lifetime-closure guarantee (I4) and the existing
`retire_timer_for_node_locked` discipline, so the *safety* of timer expiry is
not compromised — expiry correctly observes the registration state before
dereferencing the node. The defect is one of *authority conformance* (the
binding §8 representation is absent) and *counter wiring* (F.2), not of
post-destruction UAF.

**Fix recommendation.** Implement the §8 substrate (a Queue-specific
prepared-then-active path) OR explicitly supersede §8 with a Corrective-3 that
re-binds the Queue timer model to the generic ACTIVE-on-creation registration
and re-derives the counter semantics. The current state — §8 binding, no
implementation, dead counter — is an authority/implementation drift that this
review must flag.

---

### F.4 [MAJOR] Ordinary-op lifecycle gate has a TOCTOU: `state_mtx_` is dropped between the `operational` check and `CallGuard` construction

**Evidence.** Every ordinary QueuePort entry (`try_push`, `try_pop`, `close`,
`push`, `push_until`, `pop`, `pop_until`) uses the same shape
(`src/async/queue_port.cpp:162-171,229-236,275-283,317-324,344-352,371-378,388-395`):

```cpp
{
    LockGuard lk(state_mtx_);                 // S only — G NOT held
    if (lifecycle_ != QueueLifecycle::operational) {
        queue_lease_fail_fast();
    }
}                                             // <-- S released here
CallGuard guard(*this);                       // <-- counter increment, NO LOCK
```

`CallGuard` increments `active_port_calls_` without holding any lock
(`src/async/queue_port.cpp:74-77`). Meanwhile `begin_teardown` acquires G+S,
reads `active_port_calls_` under G+S, and transitions
(`src/async/queue_port.cpp:427-443`).

**The race.** With two OS threads (the runtime supports multi-worker
`run_live(N)` — see `e12_queue_g2_multi_worker_producer_consumer`):

1. Thread A (`try_push`): acquires S, observes `lifecycle_ == operational`,
   releases S.
2. Thread B (`begin_teardown`): acquires G+S, observes `active_port_calls_ ==
   0` (A has not constructed its CallGuard yet), observes both role FIFOs
   empty, transitions `lifecycle_ = tearing_down`, returns the session.
3. Thread A: constructs `CallGuard`, increments `active_port_calls_` to 1.
   **An ordinary op is now in flight inside a port that has already begun
   teardown.**
4. Thread B (or any holder of the session): calls `take_next()` while A is
   mid-commit. `take_next` holds only S (`src/async/queue_port.cpp:451`); A's
   commit holds G+S. They serialize on S, so the ring is not torn — but the
   *semantic* contract is broken: the session can observe an empty ring, then
   A's commit refills it, then the session's ring-empty observation is stale.
   The authority §6 requires "no ordinary op in flight" at the teardown
   instant.

**Authority violation.** §6 states: "The lifecycle transition serializes
against ordinary call entry: an earlier ordinary call makes
`active_port_calls_ != 0`; an earlier teardown makes every later ordinary
entry fail-fast before CallGuard construction." The implementation does not
enforce this serialization: the lifecycle check and the CallGuard increment
are not in the same atomic critical section, and the lifecycle check does
not acquire G (the lock `begin_teardown` uses to serialize).

**Why this is not caught by tests.** Every existing test runs either
single-worker (`run(1)`) or two-worker with the second worker strictly
*consumer* (`run_live(2)` in G2). No test races `begin_teardown` against an
ordinary op on a second OS thread. The TSan matrix is clean only because the
racy interleaving is never scheduled.

**Fix recommendation.** Make the lifecycle check + CallGuard increment
atomic with respect to `begin_teardown`. The cheapest correct fix is to
acquire G before the lifecycle check, hold G through CallGuard construction
(so `active_port_calls_++` is observed under G), and release G before
proceeding to the body (the body re-acquires G+S as it does today).
Alternatively, hold S from the check through CallGuard construction. Either
way the four-counter precondition at `begin_teardown` then genuinely
serializes against ordinary entry as §6 promises.

---

### F.5 [MINOR] `is_closed()` reads the non-atomic `closed_` without any lock — data race

**Evidence.** `closed_` is a plain `bool`
(`include/sluice/async/detail/queue_port.hpp:412`), mutated under G+S only in
`close()` (`src/async/queue_port.cpp:288`). The public projection
`QueuePort::is_closed()` reads it with no lock
(`src/async/queue_port.cpp:130-132`):

```cpp
bool QueuePort::is_closed() const noexcept {
    return closed_;
}
```

`AsyncQueue<T>::is_closed()` is a public inline forward
(`include/sluice/async/async_queue.hpp:243`). A caller observing `is_closed()`
on one OS thread while another calls `close()` is a data race under the C++
memory model (non-atomic read vs non-atomic write, no synchronization edge).

**Authority context.** §11 fixes lock order G→S→role; the §7 CallGuard list
explicitly includes "snapshot (including is_closed/size projection)" as
CallGuard-covered. The `size()` projection does take `state_mtx_`
(`src/async/queue_port.cpp:138-144`); `is_closed()` does not, deviating from
the same §7 row.

**Fix recommendation.** Make `closed_` `std::atomic<bool>` (release-store in
`close()`, acquire-load in `is_closed()`), OR take `state_mtx_` in
`is_closed()` as `size()` does. The atomic form is preferable because the
projection is documented as "stable or monotonic" and a lock would be heavier
than necessary.

---

### F.6 [MINOR] Grant seams swap the commit / retire order vs §12

**Evidence.** §12 fixes the winner-publication order as:

```
winner CAS -> unlink -> timer retire/consume -> lease/control location
transfer or retention -> completion write -> counter update ->
make_runnable -> ticket append
```

i.e. **retire before commit**. The implementation does the opposite
(`src/async/scheduler.cpp:2438-2453` for `queue_grant_consumer_locked`,
`2469-2484` for `queue_grant_producer_locked`):

```cpp
if (!port.ring_empty_locked()) {
    *ctx->cons_out = std::move(port.ring_[head]);   // <-- commit FIRST
    ...
}
retire_timer_for_node_locked(*won);                  // <-- retire SECOND
```

**Is it exploitable?** No. Both the commit (a ring lease move — allocation-
free, no throw) and the retire (a CAS on the timer registration) happen before
`make_runnable`/`route_runnable_locked`, so the winner observes both completed
on resume. The swap is observationally equivalent for the Queue. It is,
however, a deviation from the binding §12 ordering, and a future change to
either step (e.g. making the commit fallible) would silently violate the
documented invariant.

**Fix recommendation.** Swap the two blocks to match §12 verbatim. Cost: zero;
review surface: smaller.

---

### F.7 [OBSERVATION] Test coverage gap: G1 stops short of the failing interaction

**Evidence.** The Phase G matrix is 20/20 green across Clang Debug, GCC Debug,
Clang ASan, Clang TSan (verified independently by this review). However the
two timed-expiry tests (`e12_queue_g1_push_until_expires_recovers_value`,
`e12_queue_g1_pop_until_expires`) recover the expired value and stop. Neither
calls `begin_teardown` afterwards. If either did, F.1 would have failed the
suite.

More broadly, the binding lifecycle-contract tests are absent:

- No test calls `begin_teardown` after ANY timed operation (the F.1 trigger).
- No test calls `begin_teardown` twice (the second-call fail-fast).
- No test issues an ordinary op after `begin_teardown` (the post-teardown
  fail-fast).
- No test destroys a `QueueTeardownSession` over a non-empty ring (the
  session-dtor fail-fast).

The author's Phase H "Honest exclusions" lists the latter three as
structurally enforced but not test-exercised — defensible for `[[noreturn]]`
fail-fast paths that require a death-test harness. The first gap (post-expiry
teardown) is NOT a fail-fast-on-misuse path: it is a perfectly legal caller
sequence (`push_until` expires, then the caller tears down), and the lack of
a test for it is the direct reason F.1 shipped.

**Fix recommendation.** Add a deterministic test that mirrors G1 and then
calls `begin_teardown` — it will fail until F.1 is fixed. Once a death-test
harness is available (the repo already has `e12_async_mutex_death_test`
infrastructure at `xmake.lua:1041+`), add the three structural fail-fast
cases.

---

### F.8 [OBSERVATION] `queue_cancel` is implemented and counter-correct, but unreachable from the public API

**Evidence.** `Scheduler::queue_cancel`
(`src/async/scheduler.cpp:2407-2426`) correctly decrements
`active_wait_associations_` under G+role, mirroring `mutex_cancel`. However
`AsyncQueue<T>` (`include/sluice/async/async_queue.hpp`) exposes no cancel
surface — a `grep` for "cancel" in that header finds only the destructor
comment. Corrective-2 §17 line "Queue v1 external cancellation deferred"
makes this consistent with authority. The implementation is therefore
correct dead code; this observation records it for future-v2 wiring and to
note that the counter-correctness of `queue_cancel` does NOT rescue F.1
(the pump-expiry path is a separate resolver that bypasses `queue_cancel`).

## Per-checklist evidence

**A. Lock order.** Every Queue critical section I inspected obeys G→S→exactly-
one-role; P and C are never held together. `try_push`/`try_pop`/`close`/
`push*`/`pop*` acquire G then S in declaration order
(`src/async/queue_port.cpp:189-190,239-240,285-286`; admit closures at
`src/async/scheduler.cpp:2191-2193,2240-2242,2288-2290,2353-2355`). The grant
seams are called with G+S caller-held and take the role mtx internally
(`src/async/scheduler.cpp:2434,2465`). `close()` drains consumers (C) then
producers (P) in sequence — never together (`src/async/queue_port.cpp:296-
302`). `queue_role_waiters_empty_locked` takes each role mtx sequentially
under G (`src/async/scheduler.cpp:2498-2505`). `queue_cancel` takes G then
exactly one role mtx (`src/async/scheduler.cpp:2413-2415`). **Category A is
clean** for the Queue surface, with the caveat that the lifecycle gate (F.4)
drops S between check and CallGuard — that is a serialization defect, not a
lock-order inversion.

**B. Winner-before-publication.** Both grant seams
(`queue_grant_consumer_locked`, `queue_grant_producer_locked`) perform in one
critical section: `wake_one_locked` (resolve CAS + unlink) → resource commit
(ring move into winner's out-lease, or winner's lease into freed slot) →
`retire_timer_for_node_locked` → counter decrement → `make_runnable` +
`route_runnable_locked` (`src/async/scheduler.cpp:2434-2454,2465-2486`).
Publication is unambiguously last. No path publishes before the commit. The
only deviation is the commit/retire micro-order (F.6), which is
observationally equivalent. **Category B is clean** modulo F.6.

**C. Lost-wake / lost-grant.** The admit closures follow the established E12
idiom (verified against the comments at `src/async/scheduler.cpp:972-1045`):
`register_wait_locked` + admission recheck + `make_waiting` are ONE atomic
critical section under G+S+role; only `context_switch` is outside. A
reconciler that runs between the lock release and the context switch observes
the Registered+Waiting node and resolves it through
`make_runnable`/`route_runnable_locked`; the scheduler's runnable-state
machine absorbs the "already-runnable at suspend" case. The admission recheck
(`node.prev_ == nullptr` + ring state) correctly tests "am I the FIFO head
with an admissible resource" under the same lock that the reconciler takes.
I found no window where a grant arrives between registration and recheck and
is missed. **Category C is clean.**

**D. Counter integrity.** **FAILED.** `active_port_calls_` is incremented by
`CallGuard` ctor and decremented by `CallGuard` dtor
(`src/async/queue_port.cpp:75-81`) on every ordinary-entry path — correct,
modulo the F.4 TOCTOU. `active_wait_associations_` is incremented on every
successful registration and decremented on every inline resolution, every
reconciler grant, and every `queue_cancel` — EXCEPT the pump-driven timer
expiry path (F.1). `active_queue_timers_` is never incremented or decremented
(F.2). `granted_not_resumed_` is never incremented or decremented (F.2). The
§13 counter-delta table is therefore not faithfully implemented: rows P5/C4
(`T+1 iff timed activation`), P6/P7/C5/C6/P9/C8 (`T-1 iff ACTIVE`), and the
publication/`R±1` deltas are only partially realized.

**E. Linear capability / custody.** `QueueItemLease` is move-only;
`std::exchange` empties source on every move
(`include/sluice/async/detail/queue_item.hpp:125-137`); empty-destination
move-assign and non-empty dtor are fail-fast
(`src/async/queue_port.cpp:40-55`). No code path copies a lease (verified by
`grep -rn "QueueItemLease[^*]"` over `include/`+`src/`). The ring is
`std::unique_ptr<QueueItemLease[]>` value-initialized empty at construction
(`src/async/queue_port.cpp:106-107`); every ring transfer is a move into an
empty slot with source-emptying. T ctor/move/dtor occurs only in
`QueueItemFactory::make` (outside locks; `include/sluice/async/detail/
queue_port.hpp:222-233`) and `release_typed_` (outside locks;
`include/sluice/async/detail/queue_port.hpp:290-316`). Typed reconstruction
from a control pointer validates `owner_port_` + `type_token_` + expected
`Location` (`include/sluice/async/detail/queue_port.hpp:299-305`) before the
`static_cast<Node<T>*>`. **Category E is clean.**

**F. begin_teardown preconditions.** All seven documented preconditions are
textually checked under G+S (`src/async/queue_port.cpp:431-434`):
lifecycle, four counters, role-FIFO emptiness. **However** two of the four
counter checks are no-ops because the counters are dead (F.2), one counter
is corrupted by the F.1 leak, and the entire precondition block is subject
to the F.4 TOCTOU against concurrent ordinary entry. The *shape* is correct;
the *enforcement* is not. **Category F is FAILED** (F.1+F.2+F.4).

**G. take_next / session dtor.** `take_next` checks `port_ != nullptr`,
acquires S, requires `lifecycle_ == tearing_down`, drains ring→teardown in
FIFO order, source slot emptied (`src/async/queue_port.cpp:447-469`). The
session dtor fail-fasts on a non-empty ring
(`src/async/queue_port.cpp:479-491`). Session move clears source
(`include/sluice/async/detail/queue_port.hpp:335-336`); copy and move-assign
are deleted; the constructor is private + friend-gated. Two-session creation
is blocked by the lifecycle transition (the second `begin_teardown` fail-
fasts). **Category G is clean** for the positive surface, with the caveat
that `take_next` holds only S — safe iff the F.4 TOCTOU is fixed.

**H. close drain.** `close()` sets `closed_ = true` under G+S, then loops
`queue_grant_consumer_locked` until null (each consumer gets the next FIFO
head ring item until the ring empties, then out-lease left empty → consumer
returns closed), then loops `queue_grant_producer_locked` until null (each
producer resolved Woken with lease retained → producer returns closed)
(`src/async/queue_port.cpp:288-302`). Each parked consumer gets exactly one
buffered item until the ring is empty, then closed; each parked producer gets
its lease retained. The "until the ring empties" semantics match §13 CL1.
**Category H is clean.**

**I. Type wrapper (P8).** `AsyncQueue<T>` static_asserts object +
nothrow-move-constructible + nothrow-destructible
(`include/sluice/async/async_queue.hpp:185-189`). Typed conversion
(`from_opaque_push_`, `from_opaque_pop_`) runs after QueuePort returns —
CallGuard has already decremented `active_port_calls_` — and the
`QueueItemFactory::release_*` calls (which move T and delete `Node<T>`) are
outside any Queue/Scheduler lock. `QueuePushResult<T>::take_value()` fail-
fasts on a missing payload (`include/sluice/async/async_queue.hpp:114-119`);
the `failed(s, T&&)` factory rejects `committed`
(`include/sluice/async/async_queue.hpp:96-101`). **Category I is clean.**

**J. queue_role_waiters_empty_locked.** Takes producer mtx under G, releases,
then takes consumer mtx under G, releases
(`src/async/scheduler.cpp:2498-2505`). The two role mtx are never held
together. Returns false on the first non-empty FIFO, true iff both empty.
**Category J is clean.**

**K. TLA+ model alignment.** The B4 model
(`docs/spec/e12_queue/E12Queue.tla`, `E12QueueClosed.tla`) explicitly excludes
teardown, timer expiry, and external cancellation (banner lines 43, 39). The
model validates the 19 canonical operation transitions and 6 publication
transitions for the closed-buffer / FIFO / capacity-bounded subset; the
implementation's P2/C1/P6/C5/CL1 transitions match the modeled actions on
the ring/lease/waiter state. The defects this review found (F.1 timer-expiry
counter leak, F.2 dead counters, F.4 lifecycle TOCTOU) are in substrates the
model does NOT cover — so the model's PASS does not bear on them. The model
also does NOT model the per-Queue four-counter ledger (it is a Corrective-2
§7 addition, post-dating the model). **Category K: no model/implementation
contradiction found in-scope; the model's scope excludes the defective
substrates, which is itself a residual gap.**

**L. Test coverage gaps.** See F.7. The 20-test matrix covers the positive
fast-path, blocking, close-drain, and teardown-drain surfaces well, and the
multi-worker G2 exercises cross-worker publication. Gaps: (a) no test calls
`begin_teardown` after a timed operation (would catch F.1); (b) no test
races `begin_teardown` against ordinary ops on a second OS thread (would
catch F.4); (c) the three `[[noreturn]]` fail-fast paths (second
`begin_teardown`, ordinary op after teardown, session dtor non-empty ring)
are structurally enforced but not test-exercised — defensible given the
existing death-test harness pattern is available but not yet applied to the
Queue. The author's Phase H "Honest exclusions" itemizes (c) but not (a) or
(b); (a) is the binding gap.

## Test verification

I re-ran the 4-cell sanitizer x toolchain matrix independently. All 20 cases
PASS in every cell; this is consistent with the author's Phase H claim. The
matrix does NOT exercise the failing interaction (F.1) or the racy
interaction (F.4), so green here is necessary but not sufficient.

```
# Clang Debug
xmake f -m debug --toolchain=clang -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
[run] e12_queue_g2_multi_worker_producer_consumer
ALL TESTS PASSED

# GCC Debug
xmake f -m debug --toolchain=gcc -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
ALL TESTS PASSED

# Clang ASan
xmake f -m asan --toolchain=clang -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
ALL TESTS PASSED

# Clang TSan
xmake f -m tsan --toolchain=clang -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
ALL TESTS PASSED
```

**Targeted F.1 reproduction** (probe source at `/tmp/e12_queue_leak_probe.cpp`,
buildable with the documented flags; not added to `tests/` per reviewer rules):

```
probe: producer push_until returned status=2   (QueueOpaquePushStatus::expired)
probe: producer recovered expired value=777
probe: run returned
about to call begin_teardown...
TERMINATE called
EXIT_CODE=42   (5/5 runs, Clang Debug; symmetric result for pop_until probe)
```

The exit code 42 comes from a `std::set_terminate` handler that calls
`std::_Exit(42)`, proving the `begin_teardown` precondition tripped
`queue_lease_fail_fast` → `std::terminate`.

## Conclusion

The implementation's fast path, blocking admission, close drain, linear
capability, and type-wrapper surfaces are well-formed and match the
Corrective-2 authority on lock order, winner-before-publication, and
type-structure concerns. The author's structural discipline on the P1-P3 and
P7 substrates is genuinely strong.

However, the timer-expiry counter ledger is broken (F.1), two of the four
binding teardown-precondition counters are dead code (F.2), the §8
PreparedQueueTimer substrate the authority makes binding is entirely absent
(F.3), and the ordinary-op lifecycle gate has a TOCTOU that breaks the §6/§7
serialization claim (F.4). F.1 alone makes `begin_teardown` terminate the
process after any `push_until`/`pop_until` wait that expires via the worker
pump — a deterministic, reproducible production crash on a legal caller
sequence, invisible to the test suite only because the G1 tests stop one
step short of the failing interaction (F.7).

**Verdict: BLOCKED.** The author's PASS claim is rejected. F.1, F.2, F.3,
and F.4 must be corrected, and the G1 tests extended to call `begin_teardown`
after the expiry, before this implementation can merge. A follow-up
Corrective-2 review should re-verify the timer-expiry counter flow end-to-end
and confirm the lifecycle gate is genuinely atomic with respect to
`begin_teardown`.

---

# Re-Review 1 — Corrective Loop Verification

> **Date:** 2026-07-19
> **Commit reviewed:** `1e8e747` (Phase I corrective loop)
> **Reviewer:** independent (Phase I re-review)

## Re-Verdict

**PASS WITH OBSERVATIONS**

The corrective loop (commit `1e8e747`) **genuinely closes every actionable
finding F.1-F.7**. The F.1 BLOCKING defect is reproduced-closed end-to-end
(two rebuilt probes, push_until + pop_until, 5/5 PASS each, exit 0 with
`begin_teardown` succeeding). F.2 wires both previously-dead counters via
distinct, single-fire paths. F.3 is superseded by a binding Corrective-3
document. F.4 makes the lifecycle gate + counter increment atomic w.r.t.
`begin_teardown`. F.5 makes `closed_` `std::atomic<bool>`. F.6 swaps commit /
retire to the §12 order. F.7 extends both G1 tests to call `begin_teardown`
after the pump-driven expiry. The 4-cell sanitizer×toolchain matrix is 20/20
green across Clang Debug, GCC Debug, Clang ASan, Clang TSan.

The "WITH OBSERVATIONS" qualifier covers four residual items uncovered by the
new-defect scan. None is BLOCKING; three are pre-existing latent concerns
correctly acknowledged in Corrective-3's "Honest residual scope" or in the
original F.3/F.8; one is a structural-fragility note on the on-resolve hook
that is safe under the current lock invariant but is not enforced structurally.
Each is documented below with file:line.

## Per-finding verification

### F.1 — CLOSED

**Fix site.** `src/async/scheduler.cpp:2879-2886` (`pump_deadlines_locked`):

```cpp
if (top->has_on_resolve()) {
    auto* port = static_cast<detail::QueuePort*>(top->owner_ctx_);
    if (port != nullptr && port->active_wait_associations_ > 0) {
        --port->active_wait_associations_;
    }
    top->fire_on_resolve_locked(/*timer_won=*/true);  // -> --active_queue_timers_
}
```

For a Queue-bound registration the pump now decrements BOTH per-port counters
under G + `q->mtx()`: `active_wait_associations_` directly via `owner_ctx_`,
and `active_queue_timers_` indirectly via the on-resolve thunk. Non-Queue
registrations have a null `on_resolve_` and skip the block unchanged.

**Resolution-path coverage.** I traced every ACTIVE->terminal transition for a
Queue-bound registration against the single `++active_wait_associations_`
admit-time increment (`scheduler.cpp:2233,2289,2343,2422`):

| Path | Site | `--active_wait_associations_` | `--active_queue_timers_` (via hook) |
| --- | --- | --- | --- |
| Inline commit (admit-time) | `scheduler.cpp:2244,2364` | manual | `fire_on_resolve_locked(false)` `2363` |
| Inline closed (admit-time) | `scheduler.cpp:2251,2374` | manual | `fire_on_resolve_locked(false)` `2373` |
| Inline already-due expired | `scheduler.cpp:2385,2462` | manual | `fire_on_resolve_locked(true)` `2384,2461` |
| Grant-seam retire (consumer) | `scheduler.cpp:2524` | manual | via `retire_timer_for_node_locked` `2514` -> `2929` |
| Grant-seam retire (producer) | `scheduler.cpp:2558` | manual | via `retire_timer_for_node_locked` `2547` -> `2929` |
| `queue_cancel` retire | `scheduler.cpp:2495` | manual | via `retire_timer_for_node_locked` `2493` -> `2929` |
| Pump-driven expiry (F.1) | `scheduler.cpp:2883` | via `owner_ctx_` | `fire_on_resolve_locked(true)` `2885` |

Every row has exactly one decrement of each counter. The C8 contract-violation
early-return at `scheduler.cpp:2340-2342,2419-2421` returns BEFORE the
increment, so no decrement is owed.

**Re-run.** The original `/tmp/e12_queue_leak_probe*.cpp` probes are gone (per
the task brief). I rebuilt two self-contained probes that mirror G1 exactly
and then call `begin_teardown`:

- `/tmp/e12_queue_reprobe.cpp` — `push_until` expiry + `begin_teardown`.
- `/tmp/e12_queue_reprobe2.cpp` — symmetric `pop_until` expiry + `begin_teardown`.

Built against `build/linux/x86_64/debug/libsluice_async_internal_testing.a` +
`libsluice_core.a` with the documented flags. Both exit 0 (PASS) 5/5 runs each;
`begin_teardown` returns a session; `session.empty()` is true. Pre-corrective
these probes would have printed `TERMINATE called` and exited 42 (the
`std::set_terminate` -> `std::_Exit(42)` handler).

```
probe: producer push_until got_expired=1 value=777
about to call begin_teardown...
begin_teardown returned; session.empty=1
EXIT_CODE=0 (PASS)   (5/5 runs, Clang Debug; symmetric for pop_until probe)
```

**F.1 is genuinely closed.** The fix is structural (an explicit hook fired on
the pump's consume transition), not a papered-over counter nudge.

### F.2 — CLOSED

**`active_queue_timers_`.** Incremented exactly once at admit-time registration
(`scheduler.cpp:2349,2428`) immediately after `timer_pool_.emplace_back` +
`on_resolve_` install, under G+S+role. Decremented exactly once via the
`queue_timer_on_resolve` static thunk (`scheduler.cpp:2196-2206`), fired by
`fire_on_resolve_locked` on every ACTIVE->terminal transition (the seven rows
of the F.1 table above). The thunk is guarded `if (> 0)` so a hypothetical
double-fire would be idempotent, but the single-fire analysis (F.1 table)
shows no double-fire path exists.

**`granted_not_resumed_`.** Incremented at grant-seam publication
(`scheduler.cpp:2528,2562`) inside the `if (f->make_runnable())` block under
G+S+role. Decremented at admit-seam resume (`scheduler.cpp:2266,2319,2401,2478`)
under G after `context_switch` returns. The four decrement sites each fire ONLY
when `context_switch` returned — i.e. when the fiber was suspended and then
resumed by a reconciler publication (which always incremented). The inline
admit-time paths return BEFORE `context_switch`, so they never reach the
decrement; symmetry holds. See "New-defect scan" for the one asymmetry
(pump-driven publication does not increment, relying on `active_port_calls_`).

**Counter ledger is consistent.** All four counters are wired, single-fire,
and exercised by the G1+G2 matrix.

### F.3 — CLOSED (via binding supersession)

The `docs/e12-queue-corrective-3.md` document is a properly-formed BINDING
supersession of Corrective-2 §8. It explicitly documents:

- The authority conflict (§8 binds `PreparedQueueTimer` / `prepare_*` /
  `activate_*` / `discard_prepared_*`; production implements none of them).
- The Corrective-3 replacement model (ACTIVE-on-creation `TimerRegistration`
  + the type-erased on-resolve hook).
- A row-by-row table mapping each §8 binding to its Corrective-3 equivalent
  (or its NON-binding-historical status).
- An "Honest residual scope" section that explicitly acknowledges the §8.1
  lease-consumption-ordering residual (allocation happens under G+S+role,
  AFTER the lease was moved in by the caller — a mid-admit allocation failure
  would strand the lease).

This is the correct way to resolve an authority/implementation drift: re-derive
the binding contract and document the residual. **F.3 is closed** in the form
the original review's fix recommendation explicitly permitted ("explicitly
supersede §8 with a Corrective-3 that re-binds the Queue timer model to the
generic ACTIVE-on-creation registration").

### F.4 — CLOSED

**Fix site.** Every ordinary entry (`try_push`, `try_pop`, `close`, `push`,
`push_until`, `pop`, `pop_until`) now performs the lifecycle check AND the
`++active_port_calls_` increment inside ONE G+S critical section, then
constructs `CallGuard` with `adopt_tag` (no second increment):

```cpp
// src/async/queue_port.cpp:182-190 (try_push; symmetric at 250-258, 299-307,
// 344-352, 374-383, 404-412, 423-432)
{
    LockGuard glk(scheduler_.global_mtx_);
    LockGuard lk(state_mtx_);
    if (lifecycle_ != QueueLifecycle::operational) {
        queue_lease_fail_fast();
    }
    ++active_port_calls_;  // observed under G+S by begin_teardown
}
CallGuard guard(*this, CallGuard::adopt_tag{});
```

The `adopt_tag` ctor (`queue_port.cpp:88`) stores the port pointer without
incrementing; the dtor (`queue_port.cpp:89-93`) owns the matching decrement.

**TOCTOU closure proof.** `begin_teardown` reads `active_port_calls_` under
G+S (`queue_port.cpp:465-473`). The increment at `queue_port.cpp:188` happens
under G+S and is published before G+S is released. Therefore: (a) if Thread A
has incremented, Thread B's `begin_teardown` observed under G+S will see the
non-zero counter and fail-fast; (b) if Thread A has NOT yet incremented, it is
still blocked on G+S (which B holds), so it cannot race past the lifecycle
check while B is mid-transition. The intermediate G+S release between the gate
and the body does NOT reopen the race, because `active_port_calls_` stays
non-zero across that window (the CallGuard has been constructed), so
`begin_teardown` cannot observe all-zero counters during it.

**Body lifecycle non-recheck is safe.** The body re-acquires G+S at
`queue_port.cpp:208-209` (and symmetric) WITHOUT re-checking `lifecycle_`. I
verified this is safe: `begin_teardown` can only transition
`operational -> tearing_down` when `active_port_calls_ == 0`
(`queue_port.cpp:468-470`), and the in-flight op holds the counter > 0 from
gate-increment through CallGuard-destruction, so no concurrent
`begin_teardown` can have flipped lifecycle during the body.

**No new lock-order issue.** Every ordinary entry's lock order is now G->S
(gate) then G->S (body); `close()` body additionally calls the grant seams
which take role mtx under G+S — same order as the rest of the Queue surface.
No re-entrant acquisition of G or S. No deadlock.

### F.5 — CLOSED

`closed_` is now `std::atomic<bool>`
(`include/sluice/async/detail/queue_port.hpp:416`). The two semantic ops are
explicit:

- `is_closed()` at `queue_port.cpp:144`: `closed_.load(std::memory_order::acquire)`.
- `close()` at `queue_port.cpp:313`: `closed_.store(true, std::memory_order::release)`.

The remaining reads at `queue_port.cpp:212,282` and `scheduler.cpp:2237,2249,
2303,2354,2369,2447,2549` use the implicit `std::atomic<bool>::operator
bool()` (C++11), which performs a `seq_cst` load — STRONGER than acquire, no
correctness loss; no data race under any memory model. I verified the project
compiles clean under `-Wall -Wextra` (Clang 20, GCC 15) with no conversion
warnings. The single write site is `queue_port.cpp:313` (release store under
G+S). **No caller depended on the old non-atomic semantics** — every prior
access was already under G+S or via `is_closed()`.

### F.6 — CLOSED

**Fix site.** Both grant seams now perform retire BEFORE commit, matching §12
verbatim:

```cpp
// src/async/scheduler.cpp:2513-2523 (queue_grant_consumer_locked; symmetric
// at 2546-2557 for queue_grant_producer_locked)
// ---- retire BEFORE commit (§12 verbatim order; F.6 corrective) ----
retire_timer_for_node_locked(*won);  // ACTIVE->RETIRED CAS (no throw)
// ---- resource commit BEFORE publication ----
if (!port.ring_empty_locked()) {
    *ctx->cons_out = std::move(port.ring_[head]);   // ring-lease move (no throw)
    ...
}
```

**Winner-before-publication preserved.** Both the retire (a CAS on the timer
registration's atomic state) and the commit (a `QueueItemLease` move —
allocation-free, no throw) happen BEFORE `make_runnable` / `route_runnable_locked`
at `scheduler.cpp:2527-2530,2561-2564`. The winner observes both completed on
resume. **No observable behavior change** vs the prior commit-then-retire
order: both steps are noexcept and were already both-before-publication; the
swap only aligns the micro-order with §12. The retire CAS firing the on-resolve
hook (F.2) is also before publication, so the `--active_queue_timers_` is
observed by the winner on resume — consistent.

### F.7 — CLOSED

Both G1 tests now call `begin_teardown` after the pump-driven expiry:

- `tests/e12_async_queue_test.cpp:705-710`
  (`e12_queue_g1_push_until_expires_recovers_value`):
  `QueueTeardownSession session = port.begin_teardown(); SLUICE_CHECK(session.empty());`
- `tests/e12_async_queue_test.cpp:750-754` (`e12_queue_g1_pop_until_expires`):
  same.

Both tests use `spin_wait(registered)` + `advance_clock(deadline)` so the
producer/consumer parks BEFORE the deadline elapses — i.e. the resolution
genuinely goes through `pump_deadlines_locked`, not the inline-already-due
admit path. The `begin_teardown` call therefore exercises the F.1 closure
end-to-end. Pre-corrective these calls would have terminated the test process;
post-corrective they PASS. **F.7 regression guard is genuine.**

## New-defect scan

**On-resolve hook fires exactly once per ACTIVE->terminal.** I traced every
`fire_on_resolve_locked` call site (`scheduler.cpp:2363,2373,2384,2442,2451,
2461,2885,2929`) against the matching ACTIVE->terminal CAS
(`try_claim_expiry` at `2381,2458,2845`; `retire()` at `2361,2371,2440,2449,
2922`). The pump path (`2845` -> `2885`) and the retire path (`2922` -> `2929`)
fire only on a successful CAS — strict single-fire. **The four admit-time
inline paths (`2361-2363, 2371-2373, 2381-2384, 2440-2442, 2449-2451,
2458-2461`) fire the hook UNCONDITIONALLY on the same line as a CAS whose
return value is ignored or whose `if` block does not enclose the fire.** Under
the G+S+role_mtx invariant held throughout the admit critical section, the CAS
cannot fail (the registration is freshly ACTIVE; no other resolver can run),
so the unconditional fire is safe in practice. This is a **structural
fragility (OBSERVATION O-1, non-blocking)**: the fire is not gated by the CAS
result, so a future restructuring that dropped a lock between registration and
inline expiry would silently introduce a double-fire. Cheap to harden: gate
the fire on the CAS return.

**A registration that was never ACTIVE does not fire the hook.** The only
registration construction sites for Queue waits are `timer_pool_.emplace_back`
at `scheduler.cpp:2345,2424`; both immediately install `on_resolve_` and the
registration's default state is ACTIVE (`timer_registration.hpp:178`). So a
Queue-bound registration is ALWAYS born ACTIVE; there is no path that installs
the hook on a non-ACTIVE registration. The hook therefore fires exactly once
per Queue registration lifetime. **Clean.**

**F.4 fix (G+S through the increment) introduces no new deadlock or lock-order
issue.** Verified above (F.4). The intermediate G+S release between the gate
and the body does not reopen the TOCTOU because `active_port_calls_` is
non-zero across the window. No caller can re-enter the gate while holding G or
S (the grant seams take role mtx only, never G or S re-entrantly). **Clean.**

**`closed_` atomic change is consistent across all G+S-held sites.** Verified
above (F.5). The implicit `operator bool()` calls at eight sites perform
`seq_cst` loads, stronger than the explicit `acquire` in `is_closed()`; no
site depended on the old non-atomic semantics. **Clean.**

**F.6 commit/retire order swap preserves winner-before-publication.** Verified
above (F.6). The commit is a `QueueItemLease` move (noexcept, allocation-free)
and the retire is an atomic CAS (noexcept); both are before `make_runnable`.
No observable behavior change. **Clean.**

**`granted_not_resumed_` decrement at resume — counter-asymmetry
(OBSERVATION O-2, non-blocking).** The grant seams increment
`granted_not_resumed_` at publication (`scheduler.cpp:2528,2562`). The pump
publication path (`scheduler.cpp:2888-2891`) does NOT increment it. The four
admit-seam resume sites decrement it under G after `context_switch`
(`scheduler.cpp:2266,2319,2401,2478`), guarded by `if (> 0)`. Net effect: a
pump-driven expiry never makes the counter positive, and the resume-time
decrement is a no-op. This is an asymmetry vs the documented Corrective-3
model ("incremented at grant-seam publication; decremented at admit-seam
resume") — the pump is also a publication path. **The asymmetry is harmless**:
the `active_port_calls_` counter (CallGuard, held from gate-increment through
CallGuard-destruction) closes the same "winner-resume in flight" window that
`granted_not_resumed_` would have closed, so `begin_teardown` still cannot
proceed during a pump-expiry resume. The guarded decrement prevents underflow.
Correct but asymmetric; documenting for future-Corrective-4 alignment.

**`queue_cancel` does not increment `granted_not_resumed_` on its
`make_runnable` (OBSERVATION O-3, non-blocking, pre-existing).**
`scheduler.cpp:2497-2499` publishes a cancelled winner via `make_runnable` +
`route_runnable_locked` without incrementing `granted_not_resumed_`. If the
admit seam's resume-time decrement fired on this path, it would underflow
(guarded, so no actual underflow). F.8 documents that `queue_cancel` is
unreachable from any caller (v1-deferred); grep confirms zero callers. So the
defect is currently unreachable. Latent only; the `> 0` guard rescues it even
if v2 wires it. **Pre-existing, deferred with F.8.**

**`timer_pool_.emplace_back` allocation-failure leak (OBSERVATION O-4,
non-blocking, pre-existing).** `scheduler.cpp:2345,2424` emplace a
`TimerRegistration` under G+S+role AFTER `++active_wait_associations_` /
`++waiting_waitq_count_` / `register_wait_locked`. If `emplace_back` throws
(`std::bad_alloc`), the three LockGuards unwind releasing the locks, but the
counter increments and the FIFO registration are NOT rolled back — the next
`begin_teardown` would fail-fast. This is the same residual the original
review's F.3 flagged ("an allocation failure mid-admit would leave the lease
stranded inside the operation with the timer half-registered") and that
Corrective-3 "Honest residual scope" explicitly acknowledges ("the risk is
probabilistic zero, not structural zero"). The corrective loop neither
introduced nor worsened this; the new counter increments at `2343,2344` add
two more unrolled-back side effects on the same path, but the path is the
same one already acknowledged. **Pre-existing, acknowledged; not blocking.**

## Test matrix

Re-ran the 4-cell matrix independently (Clang 20 / GCC 15, Debug + ASan + TSan,
WSL2 x86_64). All 20 cases PASS in every cell; the F.7-extended G1 cases
(`e12_queue_g1_push_until_expires_recovers_value`,
`e12_queue_g1_pop_until_expires`) exercise the F.1 closure end-to-end in every
cell. TSan is clean on `e12_queue_g2_multi_worker_producer_consumer` (the only
cross-OS-thread publication test).

```
# Clang Debug
xmake f -m debug --toolchain=clang -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
ALL TESTS PASSED

# GCC Debug
xmake f -m debug --toolchain=gcc -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
ALL TESTS PASSED

# Clang ASan
xmake f -m asan --toolchain=clang -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
ALL TESTS PASSED   (no ASan reports)

# Clang TSan
xmake f -m tsan --toolchain=clang -y && xmake build e12_async_queue_test && xmake run e12_async_queue_test
ALL TESTS PASSED   (no TSan reports)
```

**F.1 reproduction (rebuilt probes, Clang Debug, linking the just-built
libsluice_async_internal_testing.a + libsluice_core.a):**

```
# /tmp/e12_queue_reprobe.cpp   (push_until + begin_teardown)
# /tmp/e12_queue_reprobe2.cpp  (pop_until + begin_teardown)
probe: producer push_until got_expired=1 value=777
about to call begin_teardown...
begin_teardown returned; session.empty=1
EXIT_CODE=0 (PASS)   (5/5 runs each probe; pre-corrective would have exit=42)
```

## Conclusion

The Phase I corrective loop **genuinely closes every actionable finding**.
F.1 (the BLOCKING defect) is reproduced-closed end-to-end via two rebuilt
probes plus the F.7-extended G1 matrix in all four sanitizer cells; the fix is
structural (an explicit on-resolve hook fired exactly once per ACTIVE->terminal
transition), not a counter patch. F.2 wires both previously-dead counters via
single-fire paths traced across all seven resolution rows. F.3 is closed by a
properly-formed binding Corrective-3 supersession with an honest
residual-scope section. F.4 closes the TOCTOU by holding G+S through the
lifecycle check + counter increment; the body's non-recheck of lifecycle is
proven safe by the `active_port_calls_` invariant. F.5 is a clean
atomic<bool> release/acquire pair. F.6 swaps to the §12 retire-before-commit
order with no observable behavior change. F.7's regression guard is genuine
(park-then-advance_clock forces the pump path).

The new-defect scan produced four OBSERVATIONS (O-1 through O-4), all
non-blocking: one structural-fragility note on the unconditional on-resolve
fire in the admit-time inline paths (safe under the current lock invariant,
not structurally enforced); one counter-asymmetry where the pump publication
path skips the `granted_not_resumed_` increment (closed by
`active_port_calls_`); and two pre-existing latent concerns (the unreachable
`queue_cancel` missing-increment, and the `emplace_back` allocation-failure
leak) that were already acknowledged in the original review's F.3/F.8 and in
Corrective-3's "Honest residual scope". None of these is introduced or
worsened by the corrective loop.

**Re-Verdict: PASS WITH OBSERVATIONS.** The implementation is fit to merge.
The four observations should be tracked as follow-up items (a future
Corrective-4 could harden the admit-time fire-on-CAS-success gate, align the
pump-publication `granted_not_resumed_` increment, and add a `try/catch`
rollback around `timer_pool_.emplace_back`), but none blocks the Phase I
merge.
