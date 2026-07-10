# E11 — Deadline / Timer Wait Integration

Normative specification for the E11 deadline / timer wait integration
(sluice-CORE-E11). **Status: [CLOSED]** — implemented (see as-built notes at the
end of this document and `docs/spec/e11_timer_wait/`).

This document is the authoritative E11 spec. It is written to extend — not
repeat — the as-built E10 wait protocol documented in
[`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md) and audited for
insertion in [`docs/e11-arch-recon-audit.md`](e11-arch-recon-audit.md) (verdict:
`E11-READY-WITH-CONSTRAINTS`, implementation **GO**).

E11 is bound by the project construction method
[`docs/async-runtime-construction-method.md`](async-runtime-construction-method.md)
(M1–M9).

---

## Why timers come before the synchronization API explosion

Without deadlines, every future asynchronous synchronization primitive is
forced to choose between:

```text
await forever
```

and a primitive-specific timeout protocol.

Timeout is not merely an API parameter.

It introduces a competing wait-resolution cause:

```text
RESOURCE_WAKE
    vs
TIMER_EXPIRE
    vs
CANCEL
```

For one wait epoch, these causes must compete for the existing E10
`WaitNode` resolution authority.

However, timers introduce two additional protocol dimensions that do not exist
in ordinary resource wake:

```text
timer registration lifetime
Scheduler time-progress / park liveness
```

A timer integration is therefore correct only if it preserves both:

```text
resolution safety
+
deadline liveness
```

Timers belong at the wait protocol layer before Mutex, Event, Condition,
Semaphore, Queue, or Select are introduced.

---

## As-built insertion boundary

E10 is authoritative. Production evidence:
`include/sluice/async/wait_node.hpp`, `include/sluice/async/wait_queue.hpp`,
`src/async/scheduler.cpp`. Formal model: `docs/spec/e10_waitnode/`.

The existing production protocol provides:

```text
fresh single-use WaitNode per wait epoch

WaitNode::state_
    ->
one canonical resolve_ CAS authority

RESOURCE_WAKE
    ->
Scheduler::wake_wait_one
    ->
WaitNode::resolve_(Woken)

CANCEL
    ->
Scheduler::cancel_wait
    ->
WaitNode::resolve_(Cancelled)

CAS winner
    ->
winner-only unlink
    ->
Scheduler wait accounting (--waiting_waitq_count_)
    ->
Fiber::make_runnable
    ->
route_runnable_locked
```

`wake_epoch_` is the Scheduler park/wake epoch (E9).

It is NOT a Fiber wait epoch.

A wait epoch is structurally identified by its fresh, non-reusable
`WaitNode` OBJECT identity — the live `WaitNode` instance for one object
lifetime. This is the E10 wait-epoch isolation mechanism — there is no epoch
counter, and none is required (audit §E finding 8, §F).

Note (F4-corrected): the live `WaitNode` object identity is NOT the same as its
numeric storage ADDRESS. A later wait epoch E+1 is a distinct object whose
storage MAY reuse E's numeric address after E's node is destroyed. The epoch
isolation mechanism is the fresh live-object instance + its absorbing terminal
state, NOT a permanently-unique address token. E11's `TimerRegistration` binds
to the live `WaitNode&` (object identity) and gates expiry on its own
independently-stable retirement state, so a stale expiry cannot reach E+1 even
when E+1 reuses E's address (I3/I4; formal NEG-3).

E11 MUST extend this protocol.

E11 MUST NOT introduce a parallel timer publication path.

---

## Goal

Implement one Scheduler-integrated deadline service for E10 wait epochs:

```text
monotonic absolute deadline
Scheduler-owned timer registration
timer expiry
deadline-aware Scheduler parking
timer registration retirement
distinct expired wait outcome
```

A Fiber must be able to wait on a Scheduler source with a deadline and resume
exactly once with one observable wait outcome:

```text
woken
cancelled
expired
```

---

## Deadline semantics

Deadline is a monotonic absolute time point.

Conceptually:

```text
Deadline = monotonic_clock::time_point
```

Wall-clock time MUST NOT participate in wait correctness.

The normative expiry predicate is:

```text
now >= deadline
```

A deadline that is already expired when wait admission is attempted MUST NOT
cause the Fiber to suspend and wait for a future timer scan.

It must participate in the same wait-resolution protocol immediately.

Relative durations may be added later as convenience constructors:

```text
duration
    ->
monotonic now + duration
    ->
Deadline
```

The protocol authority remains the absolute monotonic deadline.

---

## Required wait-resolution protocol

For one wait epoch represented by `WaitNode N`:

```text
RESOURCE_WAKE(N)
TIMER_EXPIRE(N)
CANCEL(N)
```

compete for exactly one existing E10 resolution authority:

```text
WaitNode::resolve_
```

Conceptually:

```text
REGISTERED
    |
    +-- RESOURCE_WAKE ----> WOKEN
    |
    +-- TIMER_EXPIRE -----> EXPIRED
    |
    +-- CANCEL -----------> CANCELLED
```

The first successful terminal CAS wins.

Terminal states are absorbing.

A losing cause:

```text
does not unlink the WaitNode
does not decrement Scheduler wait accounting
does not make the Fiber runnable
does not publish a runnable ticket
```

Timer expiry MUST reach runnable publication through the same Scheduler
resolution seam as resource wake and cancellation:

```text
Scheduler resolution serialization
    ->
WaitNode::resolve_(<expired>)
    ->
winner-only unlink
    ->
wait accounting closure
    ->
Fiber::make_runnable
    ->
route_runnable_locked
```

Timer expiry MUST NEVER directly enqueue or resume a Fiber.

> **Authority note (audit §L).** This is the supported insertion boundary: the
> E10 authority (`WaitNode::resolve_` CAS) *is* the arbitration E11 extends.
> Timer expiry is a THIRD resolution cause that enters the same
> `global_mtx_` + `q.mtx()` critical section, calls `resolve_(<timer-outcome>)`,
> and on CAS win only performs `unlink_locked` + `--waiting_waitq_count_` +
> `make_runnable` + `route_runnable_locked`. E10 already proves the form works
> (wake and cancel are two causes competing this way). The implementation MUST
> respect the E10-CORRECTIVE-2 seal: all resolution stays through a Scheduler
> seam (`Scheduler::expire_wait`, mirroring `wake_wait_one` / `cancel_wait`);
> `WaitQueue` structural operations remain private (no re-opened public resolver
> for the timer).

---

## Wait-epoch identity rule

A timer registration for wait epoch E MUST bind to the `WaitNode` identity of E.

It MUST NOT identify the wait using only:

```text
Fiber*
Worker*
WaitQueue*
Scheduler*
```

A later wait epoch uses a distinct fresh `WaitNode`.

Therefore:

```text
timer registered for node N_E
```

must never be redirected to:

```text
node N_E+1
```

A stale timer registration may only refer to its originally bound wait epoch.

This preserves the structural epoch isolation established by E10 (absorbing
terminal state + C8 reuse rejection; no epoch counter required).

> **Audit conclusion (§F, NEG-3 trace).** A stale timer expiry bound to
> `&node_N` targets `node_N`'s `state_`, which is terminal — the CAS fails,
> no publication, no re-suspension. It CANNOT reach `node_{N+1}`, which is a
> distinct object. This holds **only if** the timer registration captures
> `WaitNode&` (or an equivalent stable handle) and NEVER captures only
> `Fiber*`.

---

## Timer-registration lifetime law

`WaitNode` identity is sufficient for wait-epoch identity.

It is NOT, by itself, sufficient for timer callback lifetime safety.

A `WaitNode` is caller/Fiber-frame owned and may be destroyed after its wait
returns.

Therefore E11 MUST establish an explicit timer-registration lifetime protocol.

Normative law:

```text
A timer expiry path may dereference a WaitNode
IFF
the timer registration still owns active resolution authority for that node.
```

Before a Fiber may resume and eventually destroy its `WaitNode`, every timer
registration bound to that node must be logically retired under synchronization
that excludes a concurrent expiry dereference.

Required ordering for a non-timer winner:

```text
RESOURCE_WAKE or CANCEL wins resolve_ CAS
    ->
timer registration becomes RETIRED / INERT
    ->
no future expiry path may dereference WaitNode
    ->
runnable publication
    ->
Fiber may resume
    ->
WaitNode may be destroyed
```

For a timer winner:

```text
TIMER_EXPIRE wins resolve_ CAS
    ->
timer registration becomes CONSUMED / RETIRED
    ->
winner-only unlink and publication
```

Physical removal from a timer heap or timer wheel MAY be lazy.

Lifetime safety MAY NOT be lazy.

A physically retained stale timer entry must contain enough independently
stable state to observe that it is retired without dereferencing the retired
`WaitNode`.

The absorbing terminal state of `WaitNode` is NOT sufficient protection after
the `WaitNode` object has been destroyed.

> **Audit guidance (§H, lifetime).** E10 gives logical invalidation for free
> via the CAS rejection while the node is still alive. E11's additional
> obligation is the **post-destruction** window: once the fiber resumes and its
> frame returns, the `WaitNode` storage is gone. The timer registration MUST
> carry independently stable retirement state (generation / token / inert flag
> on a stable control block) so a straggling expiry observes "retired" WITHOUT
> touching the destroyed node. This is the load-bearing difference between E10
> loser cleanup (immediate, node still alive) and E11 timer-lifetime closure.

---

## Deadline cancellation semantics

"Deadline cancellation" means:

```text
revoke the timer registration's right to attempt resolution
+
close expiry callback reachability to the bound WaitNode
```

It does NOT necessarily mean:

```text
immediately erase the timer entry from the physical timer data structure
```

Logical retirement and physical removal are distinct operations.

The implementation may use:

```text
eager removal
lazy retired entries
stable timer control blocks
generation/token validation
another proven mechanism
```

but it MUST prove:

```text
retired timer
    ->
cannot resolve
    ->
cannot publish
    ->
cannot dereference a destroyed WaitNode
```

---

## Scheduler time-progress and park protocol

A deadline does not expire merely because monotonic time passes.

The runtime must define how an expiry action becomes executable while Scheduler
Workers are idle or parked.

E11 MUST integrate active deadlines with the E9 Scheduler park/wake protocol.

At least one proven topology must exist:

```text
Scheduler timed park until earliest active deadline
```

or:

```text
timer producer/service reaches the E9 Scheduler wake substrate
```

or another equivalent mechanism.

The implementation mechanism is secondary.

The required protocol is:

```text
active deadline exists
    ->
Scheduler cannot park indefinitely past the earliest deadline
```

The Scheduler's final park-admission decision must account for timer state.

The following races must preserve liveness:

```text
deadline registered before park admission

deadline registered during park admission

new earlier deadline replaces the previous earliest deadline

earliest deadline is retired while a Worker is preparing to park

deadline becomes due while all Workers are parked
```

No primitive-specific condition variable or private timer wake side channel may
bypass the E9 Scheduler wake protocol.

> **Construction-method hook (M2 four-dimensional topology).** For E11 the four
> state dimensions are:
>
> ```text
> resource:        runnable, backend outstanding/ready, external wait/ready,
>                  + deadline value + deadline-due predicate
> execution:       Worker/Fiber execution + E8 runnable ownership
> coordination:    MW classifier, backend admission, park phase, wake epoch,
>                  + timer wake / timed-park admission state
> invocation:      RunMode (Drain | Live); run return classification
> ```
>
> Dimension 4 (run invocation lifetime) MUST remain represented — E11 may not
> re-open the E9-CORRECTIVE run-lifetime / park-admission closure. A deadline
> in `Drain` mode must not revive the E9 hang (cf. E10-CORRECTIVE C1
> external-wake-domain classification).

---

## Required invariants

### I1 — Single Resolution Winner

```text
For one wait epoch, at most one of RESOURCE_WAKE,
TIMER_EXPIRE, and CANCEL wins WaitNode::resolve_.
```

This is inherited from E10 and MUST remain preserved.

### I2 — Single Runnable Publication

```text
For one wait epoch, at most one runnable ticket is published
as the result of wait resolution.
```

This is inherited from E10 and MUST remain preserved.

### I3 — Wait-Epoch Isolation

```text
A timer registration bound to WaitNode N_E may only attempt
resolution of N_E and cannot resolve a later wait epoch N_E+1.
```

### I4 — Timer Lifetime Closure

```text
After a timer registration is retired or consumed, no expiry path
may dereference its bound WaitNode.
```

### I5 — Deadline Admission Closure

```text
A deadline that is already due during wait admission cannot be lost
between registration, the final readiness/deadline recheck, and Fiber suspension.
```

### I6 — Deadline Park Liveness

```text
If an active wait deadline becomes due and the runtime has not terminated,
the Scheduler eventually executes or observes the corresponding timer-expiry
resolution attempt.
```

A Worker may not remain indefinitely parked solely because no non-timer wake
source fires.

### I7 — Cleanup Closure

```text
After one cause wins a wait epoch, every losing registration becomes
immediately unable to resolve or publish.

Any registration that can outlive the WaitNode storage must additionally
be unable to dereference that storage after retirement.
```

---

## Required formal state dimensions

The E11 formal model MUST explicitly represent:

```text
WaitNode identity
WaitNode resolution state
resolution cause
runnable publication count

timer registration identity
timer -> WaitNode binding
timer ACTIVE / RETIRED / CONSUMED state

monotonic logical time
deadline value
deadline due predicate

Scheduler parked / executable state
timer wake or timed-park admission state
```

Do not collapse:

```text
WaitNode terminal state
```

and:

```text
timer registration lifetime
```

into one state dimension.

Do not collapse:

```text
single resolution winner
```

and:

```text
Scheduler deadline liveness
```

into one invariant.

---

## Formal gate

The correct E11 protocol must preserve I1–I7.

The formal model extends `docs/spec/e10_waitnode/E10WaitNode.tla` with a third
resolver `ResolveTimer(n)` (same shape and CAS authority as `ResolveWake` /
`ResolveCancel`) plus the timer-registration lifetime and deadline-liveness
state dimensions above.

Negative models must demonstrate the following broken protocols.

### NEG-1 — Resource Wake + Timer Double Publication

Broken protocol:

```text
RESOURCE_WAKE publishes independently
TIMER_EXPIRE publishes independently
```

Counterexample:

```text
same wait epoch
    ->
two runnable publications
```

> The buggy variant omits the CAS guard on `ResolveTimer` (unconditional
> transition + re-route); the existing `E10WaitNodeBuggyNoWinner` already
> produces the `InvNoDoubleCompletion` (`resolvedCount[n] <= 1`)
> counterexample for wake+cancel — adding `ResolveTimer` yields the same shape
> for wake+timer.

### NEG-2 — Timer Expiry + Cancellation Double Publication

Broken protocol:

```text
timer-local completion authority
+
cancellation-local completion authority
```

Counterexample:

```text
both believe they won
    ->
two runnable publications
```

### NEG-3 — Stale Timer Cross-Epoch Resolution

Broken protocol:

```text
timer binds only to Fiber identity
```

Counterexample:

```text
timer registered for epoch E
resource resolves E
Fiber starts E+1
old timer expires
old timer resolves E+1
```

> **Negative model requirement (audit §J).** The E10 model has multiple nodes,
> so cross-node staleness is expressible, but it lacks the mapping of *external
> registration captured handle* vs *node identity*. E11 MUST add a negative
> model where a stale expiry (bound to node N0) attempts to resolve node N1,
> and prove `resolve_` rejects it because N0 is terminal (absorbing) and N1 is
> a different object. The correct variant binds `WaitNode&` and the CAS fails.

### NEG-4 — Timer Callback After WaitNode Retirement

Broken protocol:

```text
timer entry retains WaitNode*
resource/cancel resolves wait
Fiber resumes
WaitNode is destroyed
stale timer expiry dereferences WaitNode*
```

The formal model should represent the protocol defect as:

```text
WaitNode retired
AND
timer retains callback/dereference authority
```

and violate `TimerLifetimeClosure`.

The production gate MUST additionally include a deterministic lifetime race
test under ASan/UBSan.

### NEG-5 — Deadline Lost While Scheduler Is Parked

Broken protocol:

```text
active deadline exists
Worker observes no runnable work
Worker parks indefinitely
deadline becomes due
no wake source executes timer expiry
```

Counterexample:

```text
deadline due
AND
wait remains unresolved forever
AND
Scheduler remains parked
```

The corrected protocol must demonstrate deadline park liveness.

---

## Required deterministic tests

Tests MUST NOT use `sleep_for` timing as causal proof.

Use a controllable monotonic clock, deterministic timer driver, explicit
barriers, or another test-controlled causal mechanism.

At minimum test:

```text
already-expired deadline at wait admission

resource wake wins before timer

timer expiry wins before resource wake

timer expiry vs cancellation

resource wake vs timer expiry at the arbitration boundary

losing timer cannot publish

timer bound to old wait epoch cannot resolve a later wait epoch

timer registration retired before WaitNode destruction

stale physical timer entry after retirement cannot dereference WaitNode

Scheduler parked with one active deadline wakes/advances at expiry

installation of a new earlier deadline changes park admission

retirement of the earliest deadline preserves the next deadline

repeated race stress in debug and release
```

Run applicable sanitizer verification:

```text
ASan
UBSan
TSan
```

Known Fiber/TSan tooling limitations must be classified separately from protocol
failures and may not hide a timer-lifetime defect.

> **Construction-method hook (M7).** Every race proof above uses a phase seam /
> barrier / latch / explicit test hook — NOT `sleep_for` and NOT
> "run 1000 times and hope". Stress evidence is gathered AFTER the causal
> boundary is proven.

---

## Explicitly deferred

E11 does NOT implement:

```text
Mutex
Event
Condition
Semaphore
Queue
RwLock

Select / multi-wait

high-level sleep API
high-level AsyncReader / AsyncWriter deadline API

networking
timerfd-specific public API
io_uring timeout operations
timer-wheel performance optimization
hierarchical timing wheel
lock-free timer queues
NUMA timer sharding
```

The first implementation should choose the simplest timer registration and
deadline-driving mechanism that preserves the protocol invariants.

Data-structure optimization belongs to the later runtime-hardening phase (E15).

---

## Exit condition

E11 is CLOSED only when one Fiber can wait on one Scheduler wait source with a
monotonic deadline and all of the following hold:

```text
RESOURCE_WAKE
TIMER_EXPIRE
CANCEL
```

compete through the existing E10 `WaitNode::resolve_` authority.

Exactly one cause wins.

Exactly one runnable ticket may be published.

Timeout is observably distinct from cancellation.

A timer registered for one wait epoch cannot resolve a later wait epoch.

A retired timer registration cannot dereference a destroyed `WaitNode`.

An already-due deadline cannot be lost during wait admission.

A Scheduler with an active deadline cannot park indefinitely past that
deadline.

The required TLA+ negative models produce counterexamples for broken protocols.

The corrected formal model preserves I1–I7.

Deterministic production tests and applicable sanitizer gates are green.

---

## As-built topology (CLOSED record)

The chosen implementation, satisfying the exit condition above:

```text
Deadline representation      deadline_t = deadline_tick_t (uint64 monotonic ticks)
monotonic clock              Scheduler::clock_ (atomic; steady_clock in prod,
                             controllable logical clock in tests via advance_clock)
timer container              binary min-heap of TimerRegistration* over a
                             pointer-stable std::list pool. Reclamation
                             contract (proven by e11_t18): logical retirement
                             (ACTIVE->RETIRED) is IMMEDIATE; lifetime safety is
                             IMMEDIATE (atomic state gate); PHYSICAL reclamation
                             is LAZY-AT-DEADLINE (pump pops+erases an entry only
                             when now >= its deadline, regardless of state). A
                             far-future RETIRED entry remains physically in the
                             heap+pool until its original deadline is reached.
                             Pool size is bounded by concurrent ACTIVE waits +
                             retired/consumed entries whose deadlines have not
                             yet been reached — NOT solely by concurrent waits;
                             "unbounded growth fixed" is not claimed absolutely.
                             Wheel-compaction / eager removal is deferred to E15.
timer registration ownership Scheduler owns timer_pool_ (std::list); each block
                             lives exactly one wait epoch
retirement state             TimerRegistration::state_ atomic
                             (ACTIVE / RETIRED / CONSUMED) — the independently-
                             stable callback-lifetime authority (I4)
WaitNode binding             reg.node_ = &WaitNode (captures WaitNode&, never
                             only Fiber*); immutable after registration
address/storage reuse protect  expiry gates on reg.state_ (try_claim_expiry)
                               BEFORE reading node_; non-timer winner retires
                               reg in the same CS as resolve_ (before the fiber
                               can resume and destroy its node)
Scheduler expiry seam        Scheduler::expire_wait (mirrors wake_wait_one /
                             cancel_wait); driven by pump_deadlines_locked in the
                             worker loop + advance_clock (test driver)
timer expiry winner path     pump -> try_claim_expiry(ACTIVE->CONSUMED) ->
                             global_mtx_ + q.mtx() -> resolve_(expired) ->
                             unlink_locked -> --waiting_waitq_count_ ->
                             make_runnable + route_runnable_locked
non-timer retirement path    wake_wait_one/cancel_wait winner ->
                             retire_timer_for_node_locked (ACTIVE->RETIRED) in
                             the SAME CS as resolve_, BEFORE runnable publication
deadline park topology       park_on_wake_source bounded by an atomic cache of
                             the earliest ACTIVE deadline (no global_mtx_ taken
                             under wake_mtx_); worker-loop pump re-establishes
                             the authoritative deadline set each iteration (I6)
RunMode integration          external_wake_possible_locked includes
                             any_active_deadline_locked; Drain leaves a deadline
                             wait stranded (STALLED) exactly like E9/E10 (no
                             Drain-hang regression — E11-T14)
```

Formal model: `docs/spec/e11_timer_wait/` (correct model + NEG-1..NEG-5).
Deterministic tests: `tests/e11_timer_wait_test.cpp` (E11-T0..T18).

---

## Cross-links

- E10 as-built protocol (authoritative): [`docs/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)
- E10 formal model: `docs/spec/e10_waitnode/`
- E11 insertion audit (GO/NO-GO, authority map): [`docs/e11-arch-recon-audit.md`](e11-arch-recon-audit.md)
- E9 park/wake + external-wake protocol (deadline-liveness substrate): `docs/e9-0-wake-source-topology-audit.md`, `docs/spec/e9_park_wake/`
- Construction method (M1–M9, binding): [`docs/async-runtime-construction-method.md`](async-runtime-construction-method.md)
- Roadmap placement (E11 in the dependency trunk): [`docs/async-runtime-plan.md`](async-runtime-plan.md)
