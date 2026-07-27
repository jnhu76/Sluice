# E11-ARCH-RECON — Deadline / Timer Wait Integration Protocol Audit

> Read-only architecture and protocol audit of the current C++ async runtime,
> performed before implementing E11 (Deadline / Timer Wait Integration).
>
> **Status:** AUDIT COMPLETE. Verdict: `E11-READY-WITH-CONSTRAINTS`.
> Implementation: **GO**.
>
> **Scope note (as-built frontier).** This audit was re-run after rebasing
> `feat/e11-arch-recon` onto `master` at `7e149a9` (PR #4), which merged the
> full E10 chain: `0debd21` (E10 WaitNode + cancellation-safe WaitQueue),
> `dbabd21` / `3cd17c6` (E10-CORRECTIVE wake-domain closure + resolution
> authority + sealed registration), and `a1b4714` (statically-exposed global
> lock authority corrective). An earlier draft of this audit ran against a
> branch state without E10 and reached a NO-GO conclusion; that conclusion is
> superseded by this document. The audit premise ("E10 has already introduced
> the cancellation-safe wait queue core") is now satisfied by production.

---

## Mission

Determine where deadline/timer expiry must enter the existing E10 wait
protocol so that `RESOURCE_WAKE`, `TIMER_EXPIRE`, and `CANCEL` compete within
the same wait epoch and exactly one resolution may publish a runnable Fiber.

This is an architecture reconstruction and protocol-boundary audit. It does
not implement E11.

---

## A. Verdict

```
E11-READY-WITH-CONSTRAINTS
```

**Explanation.** E10 introduces exactly the substrate the audit assumes: a
per-wait `WaitNode` with ONE canonical terminal resolver authority —
`resolve_(outcome)` CAS, `registered -> {woken, cancelled}` — plus ONE
Scheduler resolution integration (`wake_wait_one` / `cancel_wait`) that
reaches ONE runnable-publication path (`make_runnable` +
`route_runnable_locked`). Resource wake and cancellation ALREADY compete on
that CAS. E11 adds `TIMER_EXPIRE` as a *third cause* using the *same
authority* — and the design doc (`docs/e10-waitnode-wait-queue.md` §10) plus
the TLA+ refinement map both state this exact composition.

The constraint (not a blocker) is that the E10 WaitNode carries NO
epoch/generation token: its wait-epoch isolation is *structural* (each
`await_wait` constructs a fresh single-use node; terminal states are
absorbing; reuse is rejected, C8). This isolation is preserved provided E11's
timer registration binds to a **`WaitNode&` (identity), not a `Fiber*`**, and
expiry resolves only while the node is still `registered`. The E11 mandate
(Section L) is: **timer registration captures `WaitNode` identity and
resolves ONLY through the existing `resolve_` CAS — never a parallel
timer-wake publication protocol.**

---

## B. Current E10 Wait Protocol (as-built)

Files: `include/sluice/async/wait_node.hpp`,
`include/sluice/async/wait_queue.hpp`, `src/async/scheduler.cpp`.

### Lifecycle of one Fiber through one WaitNode wait

```
RUNNING  (FiberState::running; ws->current == f; g_worker == ws)
   |
   v  fiber body: WaitNode node; sched.await_wait(q, node);
REGISTER  (scheduler.cpp:983 Scheduler::await_wait)
   |  under global_mtx_ + q.mtx():
   |     q.register_wait_locked(node, me)   // node.register_(this, fiber): CAS detached->registered (wait_node.hpp:180)
   |     ++waiting_waitq_count_             // scheduler MW-S3 accounting
   |     recheck: if (node.is_terminal()) undo, return   // defense-in-depth (lost-wake closure)
   |     me->make_waiting()
   |  release locks; context_switch out
   v
WAITING  (FiberState::waiting; node.state_ == registered; linked in q's FIFO list)
   |
   |  ......a resolver (waker or canceller) from any thread......
   v
RESOLVE  (winner CAS) — the two competing causes converge HERE:
   |  Scheduler::wake_wait_one(q)  [scheduler.cpp:1024]  OR
   |  Scheduler::cancel_wait(q, node) [scheduler.cpp:1049]
   |     under global_mtx_ + q.mtx():
   |        winner = q.wake_one_locked()  /  q.cancel_locked(node)
   |           -> node.resolve_(Woken|Cancelled)   // *** THE ONE ARBITRATION CAS (wait_node.hpp:196) ***
   |              compare_exchange_strong(state_, registered, outcome)
   |           -> if winner: unlink_locked(node)      // SAME q.mtx() critical section
   |        --waiting_waitq_count_
   |        if (f->make_runnable()) route_runnable_locked(f, g_worker)   // E7-T2 + canonical publish
   v
RUNNABLE  (FiberState::runnable; on owner->local_runnable; signal_wake_locked)
   v
RUNNING -> ... done
```

**Post-resolution node state:** terminal (`woken` or `cancelled`), absorbing,
no longer linked. The node identity (its address) is retained and `outcome()`
remains queryable; the node **cannot** be reused (C8).

---

## C. Authority Map

| Authority | Production owner | State/field | Writer | Consumer |
|---|---|---|---|---|
| wait membership | `WaitQueue` | `head_/tail_` + node `next_/prev_/home_` | `register_wait_locked`, `unlink_locked` (under `mtx_`) | `wake_one_locked`, `cancel_locked`, `empty_locked` |
| **wait resolution (winner authority)** | **`WaitNode`** | **`WaitNode::state_` (`std::atomic<State>`)** | **`resolve_` CAS** (sole terminal writer) | `is_terminal/outcome/was_woken/was_cancelled` (lock-free acquire), all resolvers |
| wake capability (whose CAS wins) | `WaitNode::state_` CAS linearization | — | the winning `resolve_` | losing `resolve_` (returns false, no-op) |
| Scheduler wait accounting | `Scheduler` | `waiting_waitq_count_` | `await_wait` (++), `wake_wait_one`/`cancel_wait` winner (--) | `classify_locked`, `external_wake_possible_locked` |
| runnable publication | `Fiber` | `Fiber::state_` | `make_runnable` CAS | `wake_wait_one`/`cancel_wait` (publish only after node resolved) |
| Scheduler wake notification | `Scheduler` | `wake_epoch_` + `wake_cv_` | `signal_wake_locked` (under `wake_mtx_`) | `park_on_wake_source` |
| Fiber execution (RUNNING) | `WorkerState` | `ws->current` + TLS `g_worker` | `run_next_on` | `await_wait` (reads `g_worker`) |
| **WaitNode lifetime** | **the calling fiber (caller-owned, address-stable, non-movable)** | object identity | `await_wait` creates; fiber-frame destroys | `~WaitNode` asserts `!is_registered()` |
| wait-epoch identity | **structural: node identity (no counter)** | node address | a fresh `WaitNode` per wait | `register_` rejects non-Detached (C8) |

The decisive observation (vs E9, and central to the E11 question): **the
resolution authority is now a real, named object** — `WaitNode::state_` CAS.
This is the closest thing to `try_resolve(epoch, cause)`, and the "epoch" is
the node identity.

---

## D. Resource Wake vs Cancellation Call Graph

Both paths **converge** on one common arbitration point: `WaitNode::resolve_`.

```
RESOURCE_WAKE:
  (any thread) Scheduler::wake_wait_one(q)
    -> LockGuard(global_mtx_) + LockGuard(q.mtx())
    -> q.wake_one_locked()
         -> head->resolve_(Woken)        // *** arbitration CAS ***
         -> if winner: unlink_locked(head)
    -> if won: --waiting_waitq_count_; make_runnable + route_runnable_locked
    -> signal_wake_locked  (via route_runnable_locked)

CANCEL:
  (any thread) Scheduler::cancel_wait(q, node)
    -> LockGuard(global_mtx_) + LockGuard(q.mtx())
    -> q.cancel_locked(node)
         -> node.resolve_(Cancelled)     // *** SAME arbitration CAS ***
         -> if winner: unlink_locked(node)
    -> if won: --waiting_waitq_count_; make_runnable + route_runnable_locked
    -> signal_wake_locked  (via route_runnable_locked)
```

**Convergence confirmed.** Both resolvers serialize on `global_mtx_` +
`q.mtx()`, both call `resolve_` on the same `WaitNode::state_`, and only the
CAS winner proceeds to unlink + count-decrement + route. The loser's CAS
fails and it performs no cleanup, no publication. This is the documented
Design Law (§2, `wait_node.hpp`; §7, `wait_queue.hpp`; §6 truth table,
`docs/e10-waitnode-wait-queue.md`).

**E11 extension point:** `TIMER_EXPIRE` becomes a third resolver that calls
`resolve_(<timer-outcome>)` — through the same `global_mtx_` + `q.mtx()`
critical section, routing to the same `make_runnable` +
`route_runnable_locked`. E10 already proves the form works (wake and cancel
are already two causes competing this way).

---

## E. Wait Epoch Findings (Task C)

### 1–6. wake_epoch_ meaning

`wake_epoch_` is the **Scheduler wake epoch** (the park-admission
lost-wake-window closer, unchanged from E9). It is NOT a wait epoch. It lives
in `Scheduler` (not in `WaitNode`/`WaitQueue`); written by
`signal_wake_locked`, read by `park_on_wake_source`. It is advanced by
route/notify/terminate, NOT by wait register/resolve.

### 7. Scheduler epoch, Fiber epoch, or another concept?

Scheduler park-admission epoch. Not a wait epoch.

### 8. Can a WaitNode distinguish wait N from wait N+1?

**YES — by identity, not by counter.** Each `await_wait` constructs a **fresh
`WaitNode`** in the caller's frame. The node's address *is* its epoch
identity. Because:

- `register_` only succeeds from `Detached` (C8, wait_node.hpp:180-190),
- terminal states (`woken`/`cancelled`) are absorbing (no transition out),
- and `~WaitNode` asserts `!is_registered()` (a node cannot be destroyed
  while Registered; the fiber cannot return from `await_wait` until it is
  resolved),

wait N and wait N+1 are necessarily **two different node objects**. A stale
resolution from N acts on N's `state_`, which is terminal (absorbing); it
cannot reach N+1's node. **Identity-stability + absorbing-terminal = epoch
isolation, with no counter.**

### 9–10. Is a WaitNode reusable? When?

**Not reusable.** There is no reset API; terminal is absorbing; `register_`
rejects a non-Detached node (proven in `e10_c8_node_reuse_rejected`). This is
the *load-bearing ABA-prevention mechanism*.

### 11. Can an external callback retain a pointer/handle to a WaitNode after resume?

An external resolver (`wake_wait_one`/`cancel_wait`) holds a `WaitQueue&` +
`WaitNode&`. After the fiber resumes, the node is terminal+unlinked but the
caller still owns the object (it is the fiber's frame-local). Touching the
node after it is terminal is safe BY DESIGN — `outcome()` is lock-free and
well-defined; no resolver can transition it again. A stale resolution under
identity is a no-op (CAS fails).

### 12. Protection against a stale wake from epoch N affecting epoch N+1?

**YES — absorbing terminal + identity separation.** A stale resolution from N
targets N's node (now terminal/absorbing); N+1 is a different object. The CAS
rejects. No epoch counter is required.

### Conclusion

```
EPOCH_ISOLATION:
PRESENT   (structural: node identity + absorbing terminal + C8 reuse rejection)
```

---

## F. Stale Timer / ABA Assessment (Task E)

### NEG-3 trace against current production semantics

```
Wait epoch N:
    Fiber F await_wait(q, node_N)   // node_N a fresh object in F's frame
    node_N.state_ == registered; node_N address = &node_N

RESOURCE_WAKE wins N (or cancel, or — under E11 — a timer):
    resolve_(&node_N) CAS: registered -> terminal; unlink; route
    F resumes; node_N is now terminal (absorbing), unlinked

Fiber F re-enters wait epoch N+1:
    F (or another fiber) await_wait(q', node_{N+1})   // a DIFFERENT object
    node_{N+1}.state_ == registered; node_{N+1} address != &node_N

Old timer expiry from N fires:
    if it holds &node_N (the correct binding):
        resolve_(&node_N, <timer>) -> CAS FAILS (node_N is terminal/absorbing) -> no-op   [SAFE]
    if (a DEFECTIVE one) it held only F*:
        it cannot "resolve F" — no resolution API takes a Fiber*; resolution operates on WaitNode&   [NOT EXPRESSIBLE]
```

### Conclusion

```
STALE_TIMER_CROSS_EPOCH:
PREVENTED   (provided E11's timer registration binds WaitNode& and not Fiber*)
```

**Why.** The E10 substrate exposes **`WaitNode&` identity** as the resolution
target, and terminal states are absorbing. A stale timer expiry bound to
`&node_N` targets `node_N`'s `state_`, which is terminal — the CAS fails, no
publication, no re-suspension. It **cannot** reach `node_{N+1}`, which is a
distinct object with its own `state_`.

**The one E11 requirement that keeps this true:** the timer registration
**MUST** capture `{WaitNode& or equivalent stable handle, deadline}` and
**MUST NOT** capture only `Fiber*`. The `WaitNode` is the epoch token — its
identity *is* the wait-epoch identity. A timer bound to `Fiber*` would
re-introduce NEG-3 (no per-fiber wait-epoch). The E10 design prevents this by
making `resolve_` operate on `WaitNode&`, not `Fiber*`; E11 must respect that
boundary.

---

## G. Runnable Publication Audit (Task F)

All production paths that can reach `make_runnable` + `route_runnable_locked`
(the E10 resolution paths are the relevant ones):

| Publication path | Caller | Required authority | Can wait resolution reach it? | Epoch-aware? |
|---|---|---|---|---|
| `spawn`/`spawn_on`/run-distribute -> `local_runnable` | scheduler spawn path | `make_runnable` true (created->runnable) | No (initial) | n/a |
| `route_runnable_locked` <- `wake_ready_completions_locked` | backend drain | `make_runnable` true (waiting->runnable) | Yes (legacy path) | No (legacy path) |
| `route_runnable_locked` <- `wake_ready_flags_locked` | flag drain | `make_runnable` true | Yes (legacy path) | No (legacy path) |
| `route_runnable_locked` <- **`wake_wait_one` winner** | **E10 resource wake** | **`resolve_(Woken)` winner THEN `make_runnable` true** | **Yes** | **Yes (via node identity)** |
| `route_runnable_locked` <- **`cancel_wait` winner** | **E10 cancel** | **`resolve_(Cancelled)` winner THEN `make_runnable` true** | **Yes** | **Yes (via node identity)** |
| `try_steal` -> thief `local_runnable` | steal | none (MOVE, no make_runnable) | No (transport) | No |

### Answer

> Does unique wait-resolution winner imply unique runnable publication?

```
YES — mechanically enforced
```

Dual enforcement: **(1)** `resolve_` CAS returns exactly one winner (TLA+
`InvNoDoubleCompletion`, `resolvedCount[n] <= 1`); **(2)** even if two winners
were hypothetically possible, `make_runnable()`'s CAS (E7-T2) blocks a second
enqueue. Each resolver calls `route_runnable_locked` only *after* the node is
resolved (in the same locked CS) and only if `make_runnable` returns true. The
TLA+ `InvNoDuplicateSchedulerWake` (`wakeDispatched == Σ resolvedCount`)
captures this. The E10 refinement map models the whole `q.mtx()` CS (resolve +
unlink + route) as one atomic step, so the formal single-winner property
carries publication uniqueness directly.

This is now a **documented** invariant (design doc §6, TLA+ README
linearization proof step 3).

---

## H. Cleanup and Lifetime Findings (Task G)

### Winner cleanup (E10)
`wake_one_locked`/`cancel_locked` winner: `resolve_` CAS wins ->
`unlink_locked` (same CS) -> return node -> Scheduler decrements
`waiting_waitq_count_`, calls `make_runnable`+`route_runnable_locked`.
One-shot, atomic w.r.t. other resolvers.

### Loser cleanup
The loser's `resolve_` CAS returns false -> the resolver returns before
`unlink_locked`. **No cleanup hook, no callback, no registration
invalidation** — the CAS failure *is* the loser's cleanup. Proven (design doc
§6 truth table: "loser/no-op"). It is cleanly lock-free observable: the loser
reads the winner's terminal outcome via acquire.

### WaitQueue unlink
ONE path — `unlink_locked`, called only on a winning CAS, under `mtx_`. (No
wake-unlink/cancel-unlink/destructor-unlink — §7 Unlink Law.)

### Registration invalidation
Only `unlink_locked` (logical removal = structural removal; one operation).
No separate "invalidate" state.

### WaitNode lifetime / external registration
Caller-owned, address-stable, non-movable. The fiber cannot destroy its node
until `await_wait` returns (it suspends inside), and `await_wait` returns only
once a resolver wins (which resolves+unlinks the node). So a Registered node
is never destroyed while Registered (`~WaitNode` assert, C9).

### What timer integration requires
Timer expiry is a *resolution cause*, not a signal. It must:

- enter the same `global_mtx_` + `q.mtx()` CS,
- call `resolve_(<timer-outcome>)`,
- on CAS win only: `unlink_locked` + `--waiting_waitq_count_` +
  `make_runnable`+`route_runnable_locked`.

Cleanup is **logical invalidation by binding**: a timer bound to `&node_N`
whose expiry fires after the node is terminal observes an absorbing state ->
CAS fails -> no-op. This is the *same* loser semantic as the existing
loser-path. **No immediate physical timer-wheel removal is required for
correctness** (it may be done lazily); E10 gives logical invalidation for free
via the CAS rejection.

> **E11 IMPLEMENTATION CORRECTION (as-built).** The above statement is accurate
> ONLY while the `WaitNode` object is still alive (its absorbing terminal state
> rejects a straggling `resolve_` CAS). E11's additional obligation is the
> **post-destruction** window: once the fiber resumes and its caller-owned
> `WaitNode` goes out of scope, the node storage is gone, and a physically-
> retained/lazy timer entry holding a raw `WaitNode*` would dereference freed
> memory. The absorbing terminal state is **NOT** sufficient protection after
> the `WaitNode` object has been destroyed. The as-built E11 therefore carries
> an independently-stable `TimerRegistration` control block whose atomic
> `state` (ACTIVE/RETIRED/CONSUMED) is the callback-lifetime authority: a
> non-timer winner retires the registration in the SAME `global_mtx_` CS that
> resolves the node (before runnable publication), so a straggling expiry
> observes RETIRED via the registration's OWN state and MUST NOT dereference the
> destroyed node. This is the load-bearing difference between E10 loser cleanup
> (immediate, node still alive) and E11 timer-lifetime closure (post-
> destruction). See `include/sluice/async/timer_registration.hpp` and
> `docs/spec/e11_timer_wait/` (NEG-4) for the formal proof.

### Protocol meaning of "deadline cancellation" in this codebase

```
invalidate the epoch authority (logical) — and specifically:
when the wait resolves by ANY cause (resource wake, another timer,
or cancel), the node becomes terminal (absorbing); any later timer
expiry bound to it observes the terminal state and its resolve_ CAS
fails. "Deadline cancellation" = resolving the node to a terminal
outcome via the normal resolution path, which lazily invalidates all
competing timer registrations for free. It needs no separate
timer-deletion API.
```

> **E11 IMPLEMENTATION CORRECTION (as-built).** The "lazily invalidates ...
> for free" claim holds ONLY while the `WaitNode` is alive. After the fiber
> resumes and the node storage is destroyed, the CAS-rejection argument no
> longer applies (the `state_` is freed). The as-built E11 closes this gap with
> an independently-stable `TimerRegistration` retirement state
> (ACTIVE/RETIRED/CONSUMED): a non-timer winner retires the bound registration
> before runnable publication, and a straggling expiry observes RETIRED on the
> registration's OWN state (not the node) and MUST NOT dereference it. So
> "deadline cancellation" = (1) retire the timer registration's callback
> authority under `global_mtx_` + (2) lazily drop the physical timer entry,
> where (1) is the load-bearing post-destruction safety and (2) is optional.
> Distinct from E10: this is E11 callback-lifetime closure, not merely E10
> logical-loser safety.

---

## I. Formal Model Gap Matrix

| Required E11 concept | Current model status | Production mapping | Gap |
|---|---|---|---|
| RESOURCE_WAKE | **Modeled** (`ResolveWake`, E10WaitNode.tla:91) | `wake_wait_one` -> `resolve_(Woken)` | None |
| TIMER_EXPIRE | **Not modeled** | — | **Add a third resolver (`ResolveTimer`) + outcome. Mechanical extension: same `Registered` guard, same CAS authority, same route.** |
| CANCEL | **Modeled** (`ResolveCancel`, E10WaitNode.tla:103) | `cancel_wait` -> `resolve_(Cancelled)` | None |
| wake capability (single winner) | **Modeled** (`resolve_` CAS, `resolvedCount<=1`) | `WaitNode::state_` CAS | None — the authority E11 extends |
| wait epoch | **Modeled structurally** (node identity; `Register` single-shot; terminal absorbing) | per-wait WaitNode identity | **Model lacks a time dimension; add a wait-epoch-identity annotation.** Wait-epoch isolation is a consequence of `InvNoTerminalResurrection` + single-shot `Register` |
| runnable publication | **Modeled** (`wakeDispatched == Σ resolvedCount`, `InvNoDuplicateSchedulerWake`) | `make_runnable`+`route_runnable_locked` | None |
| registration lifetime | **Modeled** (`Register` from Detached; `InvLinkedImpliesRegistered`) | `register_wait_locked` | None |
| WaitNode reuse | **Modeled** (rejected; C8, `Register` requires Detached) | `register_` CAS rejects non-Detached | None — the ABA-prevention mechanism |
| stale external callback | **Partially modeled** (absorbing terminal blocks stale resolve; E9 handle-lifetime models Scheduler-destruction staleness) | terminal CAS failure; `SchedulerWakeHandle` Control block | **Model does not show an execution where a stale callback tries to resolve; add a NEG-3 negative model (required for E11).** |

### Semantic drift between formal artifacts and production

Minimal and documented (E10-CORRECTIVE C4): the formal
`ResolveWake`/`ResolveCancel` abstracts the entire `q.mtx()` CS (CAS + unlink
+ route) as one atomic step, whereas the production CAS is the winner
linearization point and the unlink runs later in the same CS. This refinement
boundary is explicit and does not weaken the single-winner property. E11's
`ResolveTimer` must follow the same abstraction.

---

## J. Negative Model Requirements

### NEG-1 — Resource Wake + Timer Double Publication
- **Counterexample target:** same node, `RESOURCE_WAKE` and `TIMER_EXPIRE`
  each independently publish.
- **Minimal formal state:** the existing E10 state plus a `ResolveTimer(n)`
  action (same shape as `ResolveWake`/`ResolveCancel`). The *buggy* variant
  omits the CAS (unconditional transition + re-route).
- **Required invariant violation:** `InvNoDoubleCompletion`
  (`resolvedCount[n] <= 1`).
- **Can the current model express it?** **YES, with a trivial extension.**
  `E10WaitNodeBuggyNoWinner` already demonstrates the wake+cancel
  counterexample; adding `ResolveTimer` and removing its CAS guard yields the
  same `resolvedCount=2` counterexample for wake+timer. The state dimension
  (CAS authority on the resolve cause) is already present.

### NEG-2 — Timer Expiry + Cancellation Double Publication
- **Counterexample target:** same node, timer-local and cancel-local
  completion state both believe they won.
- **Minimal formal state:** as NEG-1, plus the existing `ResolveCancel`.
- **Required invariant violation:** SingleWinner (I1).
- **Can the current model express it?** **YES** — as above. Two causes
  sharing one CAS authority is exactly E10's Design Law; a third cause (timer)
  is a direct addition.

### NEG-3 — Stale Timer Cross-Epoch Wake
- **Counterexample target:** a timer registered for epoch N resolves epoch N+1.
- **Minimal formal state:** two nodes `N0` (epoch N), `N1` (epoch N+1); a
  timer registration capturing `N0`; the *buggy* variant lets the timer
  resolve `N1` (binds the wrong identity / no identity check). The *correct*
  variant: the timer resolves its bound node, which is terminal -> CAS fails.
- **Required invariant violation:** epoch isolation (I3).
- **Can the current model express it?** **PARTIALLY — needs a new dimension.**
  The E10 model has multiple nodes, so cross-node staleness is expressible,
  but it lacks the mapping of *external registration captured handle* vs
  *node identity*. **E11 must add a negative model where a stale expiry
  (bound to node N0) attempts to resolve node N1, and prove `resolve_`
  rejects it because N0 is terminal (absorbing) and N1 is a different object.**
  The missing state dimension is small: it is the *captured identity* of an
  external callback, relative to node identity.

---

## K. E11 Invariant Review

### I1 — Single Winner
> For one wait epoch, at most one of RESOURCE_WAKE, TIMER_EXPIRE, and CANCEL
> owns the wake capability.

**REDUNDANT (already enforced).** This is exactly E10's
`InvNoDoubleCompletion` / `resolve_` CAS Design Law. E11 inherits it; no new
work. Normative wording OK, extended to three causes:

```
For one wait epoch (node identity), at most one of RESOURCE_WAKE,
TIMER_EXPIRE, and CANCEL owns the resolve_ CAS win.
```

### I2 — Single Publication
> For one wait epoch, at most one runnable ticket is published as the result
> of wait resolution.

**REDUNDANT (already enforced).** `InvNoDuplicateSchedulerWake`
(`wakeDispatched == Σ resolvedCount`) + `make_runnable` E7-T2 CAS. E11
inherits it. Wording OK.

### I3 — Epoch Isolation
> A wake source registered for wait epoch E cannot resolve any later wait
> epoch.

**NEEDS REWORDING (minor).** The invariant holds, but the *mechanism* is node
identity + absorbing terminal, not an epoch counter. Reword to match the
codebase:

```
A wake source registered for wait epoch E (node N_E) cannot resolve
any later wait epoch. A later epoch's node is a distinct object
(N_{E+1}); a stale resolution bound to N_E acts on N_E's absorbing
terminal state and its resolve_ CAS fails.
```

This wording makes the E11 implementer explicit: **bind to `WaitNode&`, not
`Fiber*`.**

### I4 — Cleanup Closure
> After a wait epoch resolves, all losing registrations eventually become
> inert and cannot retain wake authority for that epoch or a future epoch.

**NEEDS REWORDING.** "Eventually" is too weak; E10 loser cleanup is
*immediate and synchronous* (CAS failure -> instant no-op). Reword:

```
After a wait epoch resolves, all competing (losing) registrations
observe the absorbing terminal state and cannot publish — their
resolve_ CAS fails immediately. A losing timer expiry needs no
separate deletion API; lazy invalidation via binding to the
resolved node is a consequence of the CAS rejection.
```

---

## L. Recommended E11 Insertion Boundary

```
A. Extend the existing WaitNode arbitration
```

**This is the supported boundary.** The E10 authority (`WaitNode::resolve_`
CAS) *is* the arbitration E11 must extend. Not B (`SchedulerWakeHandle` is a
Scheduler-lifetime signal handle, not a per-wait resolution object), and not C
(no new resolution object is needed — `WaitNode` already is one).

### Existing symbols to extend
- `WaitOutcome`: add `expired = 3` (or keep semantics: timer expiry may route
  as `woken`, or get its own outcome — this is a **design decision**: see
  below). The route path is identical either way.
- `WaitNode::State`: add a matching terminal state if `expired` is a distinct
  outcome.
- `WaitNode::resolve_`: already accepts any terminal outcome — it is
  outcome-agnostic as long as the outcome is terminal. Extend the `target`
  selection for the new outcome.
- A new Scheduler resolution seam `expire_wait(q, node)` mirroring
  `wake_wait_one`/`cancel_wait`, OR — simpler — a timer expiry calls the
  existing `cancel_wait` (semantic: deadline elapsed = cancel), OR a new
  `expire_wait` returns a distinct `expired` outcome. **DESIGN DECISION**
  (see below).

### Authority that must remain unique
- `WaitNode::state_` resolve CAS. Timer expiry **MUST** go through it — never
  a parallel enqueue.

### State a timer registration must capture
```
{ WaitNode& node (or a stable handle), deadline, Scheduler& (or a resolver-seam handle) }
```
**`WaitNode&` is the epoch token.** Do not capture `Fiber*` alone (NEG-3).

### State timer expiry must validate
```
node.resolve_(<timer-outcome>) operates as:
  - the node is still `registered` (CAS wins), OR
  - the node is already terminal (CAS fails -> no-op; lazy invalidation).
```
No separate epoch check is needed — the CAS *is* the check, because terminal
is absorbing.

### Publication path timer expiry must use
The SAME path as `wake_wait_one`/`cancel_wait`: `global_mtx_` + `q.mtx()` ->
`resolve_` -> unlink -> `--waiting_waitq_count_` -> `make_runnable` +
`route_runnable_locked`. No parallel timer-wake protocol.

### Cleanup ownership
The Scheduler resolution seam (as `wake_wait_one`/`cancel_wait` already do).
Timer-wheel removal (physical) is a lazy subsystem concern decoupled from
correctness — the CAS rejection makes a straggling expiry inert.

---

## M. Prerequisite Corrective Task

```
NONE
```

The E10 wait-epoch resolution authority exists, is sealed
(E10-CORRECTIVE-2 R1+R2), and is backed by the TLA+ refinement in production.
E11 may begin directly; no prerequisite is required. The formal negative
models (NEG-1/2/3) should be *added as part of E11* alongside the
implementation (they are the E11 formal gate, not a blocking prerequisite).

---

## N. Final E11 Go / No-Go

```
E11 IMPLEMENTATION: GO
```

**Protocol boundary the implementation MUST preserve:**

```
1. Timer expiry is a THIRD resolution cause that goes through the
   EXISTING WaitNode::resolve_(outcome) CAS authority — NOT a
   parallel timer-wake protocol.

2. The timer registration binds to a WaitNode& (node identity IS the
   wait-epoch identity), and NEVER captures only a Fiber*. This
   preserves EPOCH_ISOLATION (absorbing terminal + C8 reuse rejection).

3. Timer expiry routes through the SAME Scheduler resolution seam as
   wake_wait_one / cancel_wait:
   global_mtx_ + q.mtx() -> resolve_ -> unlink_locked ->
   --waiting_waitq_count_ -> make_runnable + route_runnable_locked.
   NEVER a parallel enqueue.

4. A losing timer expiry reuses the E10 loser semantic (CAS fail ->
   immediate no-op); no separate timer-deletion API is needed for
   correctness.

5. E11 formal gate: extend E10WaitNode.tla with ResolveTimer +
   NEG-1/NEG-2/NEG-3 negative models (stale expiry bound to N0
   cannot resolve N1).

6. Respect the seal: all resolution stays through a Scheduler seam;
   WaitQueue operations remain private (do NOT re-open a public
   resolver for the timer).
```

Stop.
