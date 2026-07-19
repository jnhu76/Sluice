# E12-E Queue — Corrective-3: §8 timer substrate supersession

> **Status:** BINDING (supersedes Corrective-2 §8 for the Queue timer model)
> **Date:** 2026-07-19
> **Origin:** Phase I independent adversarial review
> (`docs/reviews/E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1-REVIEW.md`),
> findings F.1, F.2, F.3.

## Authority conflict that this Corrective-3 resolves

Corrective-2 §8 binds a Queue-specific timer substrate:

- `PreparedQueueTimer` (a distinct Queue-owned prepared-then-active timer
  representation)
- `prepare_queue_timer_locked(deadline_t)` (allocate + register PREPARED
  under G, BEFORE the lease is consumed)
- `activate_queue_timer_locked(...)` (PREPARED -> ACTIVE under G+S+role,
  increments `active_queue_timers_`)
- `discard_prepared_timer_locked(...)` (PREPARED -> discarded on an
  admission-recheck commit, no counter delta)

The Phase I review found that the production implementation does NOT
implement any of these symbols (`grep` over `include/` + `src/` is empty).
Instead the four Queue admit seams (`queue_push_admit_until`,
`queue_pop_admit_until`, and the untimed variants) allocate a generic
ACTIVE-on-creation `TimerRegistration` directly under G+S+role
(`scheduler.cpp:2346`-`2351`, `2425`-`2430`). Two binding consequences:

- **F.2**: `active_queue_timers_` is a dead counter (never incremented or
  decremented anywhere), so the `begin_teardown` precondition
  `active_queue_timers_ == 0` is unenforced.
- **F.1**: the pump-driven timer-expiry path
  (`pump_deadlines_locked`) decrements only `waiting_waitq_count_`
  (Scheduler-wide); it does NOT decrement `port.active_wait_associations_`.
  After any `push_until` / `pop_until` wait that resolves via the pump, the
  per-port counter leaks by 1 per such wait, and the next `begin_teardown`
  fail-fasts (`std::terminate`). Reproduced 5/5 on Clang Debug.

This Corrective-3 supersedes §8's prepared-then-active substrate with a
minimal model that (a) closes F.1, (b) wires F.2, and (c) keeps the
existing ACTIVE-on-creation `TimerRegistration` allocation path (the E11
timer-lifetime-closure I4 guarantee is inherited unchanged).

## The Corrective-3 model

### TimerRegistration extension (type-erased on-resolve hook)

`TimerRegistration` gains an optional `{on_resolve_, owner_ctx_}` pair
(timer_registration.hpp):

```cpp
using OnResolveFn = void (*)(void* owner_ctx, bool timer_won) noexcept;
OnResolveFn on_resolve_{nullptr};
void* owner_ctx_{nullptr};
bool has_on_resolve() const noexcept;
void fire_on_resolve_locked(bool timer_won) noexcept;
```

The hook is invoked by the Scheduler exactly once per ACTIVE->terminal
transition (consume or retire) under `global_mtx_`. Non-Queue waits leave
both null; the Scheduler's default `--waiting_waitq_count_` accounting
applies unchanged.

### Queue-bound registration install (the four admit seams)

At admit time, `queue_push_admit_until` / `queue_pop_admit_until`:

1. Register the wait; `++port.active_wait_associations_`;
   `++waiting_waitq_count_`.
2. Allocate the `TimerRegistration` (ACTIVE-on-creation, unchanged).
3. Install the hook: `reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;
   reg->owner_ctx_ = &port;`
4. `++port.active_queue_timers_;`  (F.2 wiring)
5. `++active_deadline_count_;` (Scheduler-wide, unchanged)
6. Admission recheck / closed / already-due-expired inline paths: each
   performs the existing `--port.active_wait_associations_` decrement
   manually AND fires the hook (which decrements `active_queue_timers_`).

The untimed admit seams (`queue_push_admit`, `queue_pop_admit`) install NO
hook; their manual `--port.active_wait_associations_` decrement is
unchanged.

### Scheduler::queue_timer_on_resolve (the hook body)

A `static` member of Scheduler (so it can reach `QueuePort`'s private
counter via the friend grant). Decrements `active_queue_timers_` exactly
once:

```cpp
void Scheduler::queue_timer_on_resolve(void* owner_ctx, bool) noexcept {
    auto* port = static_cast<detail::QueuePort*>(owner_ctx);
    if (port == nullptr) return;
    if (port->active_queue_timers_ > 0) --port->active_queue_timers_;
}
```

### Pump-driven expiry (F.1 closure)

`pump_deadlines_locked` (scheduler.cpp ~2800) for a Queue-bound
registration:

1. `try_claim_expiry()` wins (ACTIVE -> CONSUMED).
2. `--active_deadline_count_`.
3. Under `q->mtx()`, `q->expire_locked(*n)` wins the resolve CAS.
4. **F.1 fix**: if `top->has_on_resolve()`:
   - decrement `port->active_wait_associations_` via `owner_ctx_`
   - `top->fire_on_resolve_locked(/*timer_won=*/true)` (decrements
     `active_queue_timers_`)
5. `--waiting_waitq_count_` (unchanged).
6. `make_runnable` + `route_runnable_locked` (publication LAST).

Non-Queue registrations skip step 4 entirely.

### Retire (close-of-callback-authority)

`retire_timer_for_node_locked` (scheduler.cpp ~2826) when the matched
registration retires (ACTIVE -> RETIRED):

1. `r.retire()` returns true.
2. `--active_deadline_count_`.
3. **F.2 fix**: `r.fire_on_resolve_locked(/*timer_won=*/false)` (decrements
   `active_queue_timers_`).

### granted_not_resumed_ (F.2 second counter)

The grant seams (`queue_grant_consumer_locked`,
`queue_grant_producer_locked`) `++port.granted_not_resumed_` when their
`make_runnable` succeeds (a suspended-winner ticket is published). The
four admit seams decrement `--port.granted_not_resumed_` under G
immediately after `context_switch` returns (the fiber has resumed; the
ticket is consumed).

## What this Corrective-3 changes vs §8

| §8 binding | Corrective-3 |
| --- | --- |
| `PreparedQueueTimer` distinct representation | NOT implemented; the generic ACTIVE-on-creation `TimerRegistration` is used. |
| `prepare_queue_timer_locked` (PREPARED under G, before lease consumption) | NOT implemented; allocation happens under G+S+role. |
| `activate_queue_timer_locked` (PREPARED -> ACTIVE, ++counter) | The `++active_queue_timers_` happens at admit-time registration (immediately after `timer_pool_.emplace_back`), under G+S+role. |
| `discard_prepared_timer_locked` (PREPARED -> discarded, no delta) | NOT needed: the ACTIVE-on-creation model has no PREPARED state to discard. An admission-recheck commit simply retires the just-created ACTIVE registration (existing `retire_timer_for_node_locked` path). |
| Counter authority "PREPARED->ACTIVE activation / ACTIVE->RETIRED or CONSUMED winner" | Counter authority "registration creation / registration retire-or-consume". Same effective lifetime; the counter is non-zero iff an ACTIVE Queue timer exists. |

## What this Corrective-3 preserves from §8

- The four-counter ledger (`active_port_calls_`,
  `active_wait_associations_`, `active_queue_timers_`,
  `granted_not_resumed_`) and the `begin_teardown` precondition that all
  four are zero. Both counters that §8 made per-port
  (`active_wait_associations_`, `active_queue_timers_`) are now wired.
- The E11 timer-lifetime-closure I4 guarantee (a retired registration is
  observed via its atomic state before its node pointer is touched). The
  Corrective-3 hook fires BEFORE the registration is erased from the pool,
  in the same critical section that performed the ACTIVE->terminal CAS.
- The allocation-outside-locks discipline for user T (unchanged).

## Honest residual scope

- The §8.1 lease-consumption ordering ("prepare timer BEFORE consuming the
  lease") is NOT preserved by Corrective-3: the lease is consumed by value
  at admit entry (the caller's `push_until(value)` moves `value` into the
  lease before the admit seam runs), and timer allocation happens AFTER
  that, under G+S+role. A mid-admit allocation failure would strand the
  lease inside the operation. In practice `std::list::emplace_back` and
  the heap reserve do not throw (the heap is a pre-sized vector); the
  risk is probabilistic zero, not structural zero. A future Corrective-4
  may re-introduce a prepare-step if a throwing allocation path is
  identified.
- This Corrective-3 is binding for the production implementation; §8's
  prepared-then-active substrate is NON-BINDING historical from this
  decision forward.

## Test verification

- The two G1 timed tests (`e12_queue_g1_push_until_expires_recovers_value`,
  `e12_queue_g1_pop_until_expires`) now call `begin_teardown` after the
  expiry. Pre-Corrective-3 these would `std::terminate` 5/5; post-
  Corrective-3 they PASS (the pump-driven expiry decrements both
  per-port counters, and `begin_teardown` succeeds).
- All 20 `e12_async_queue_test` cases PASS on Clang Debug, GCC Debug,
  Clang ASan, Clang TSan. E11/E12 sync primitive tests regress-clean.
- Production `sluice_async` target compiles clean.
