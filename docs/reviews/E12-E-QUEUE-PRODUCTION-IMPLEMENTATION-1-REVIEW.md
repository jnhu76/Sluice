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
