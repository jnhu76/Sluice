# FE-1b — Frontend-Neutral Wait Contract Freeze

Status: ARCHIVED DESIGN REPORT (contract freeze only; no production change in stage
FE-1b; archived with the FE campaign PR).
Stage: FE-1b of the FE Multi-Frontend Semantic Reuse Campaign.
Inputs: FE-1a audit (evidence, re-verified against current tree), current
sources under `include/sluice/async/` and `src/async/` at BASE_SHA.

---

## VERDICT

**FE-1b PASS — CONTRACT FROZEN**

The semantic Core contract below is expressed without any stackful type,
state name, worker concept, or suspension mechanism. Every clause is backed
by a current-tree source fact. The four adversarial corrections (A1–A4) are
incorporated as binding clauses, not commentary. The contract is implementable
by the stackful Fiber frontend unchanged (its current behavior satisfies every
clause) and by a stackless coroutine frontend through the two seams FE-1a
already identified (suspension-commit seam, publication seam).

---

## BASE

```text
HEAD == origin/master == 5706a6ddfa23d7057a29e60be414139e4698e3e5   (BASE_SHA)
PR #242 ancestry verified (merge commit 5706a6d is HEAD)
```

Working tree: only the two known human-owned untracked reports
(`FE-1A-FRONTEND-NEUTRAL-WAIT-CONTRACT-AUDIT.md`,
`R2-WAIT-LIFECYCLE-FINAL-CLOSEOUT-REPORT.md`) plus this report; preserved
untouched. No tracked dirty state.

Baseline (Clang Debug, run before this report): `xmake f -m debug
--toolchain=clang && xmake build sluice_core && xmake build sluice_async &&
xmake build -g test && xmake test -v` → **193/193 passed, exit 0**.

---

## FE-1a RE-VERIFICATION (§6 — all ten findings re-confirmed on current tree)

| # | FE-1a finding | Current-tree evidence | Confirmed |
|---|---|---|---|
| 1 | `WaitNode::resolve_` is token-independent | wait_node.hpp:240-250 — the CAS reads/writes `state_` only; `fiber_` never consulted | YES |
| 2 | Registration token is `Fiber*`-typed | wait_queue.hpp:185 `register_wait_locked(WaitNode&, Fiber*)`; wait_node.hpp:252 `Fiber* fiber_`; wait_node.hpp:218-227 CAS-then-bind | YES |
| 3 | Deadline authority is token-independent | scheduler_timer.cpp:78-168 — `prepare_/publish_ordinary_deadline_locked` etc. operate on {node, queue, deadline}; the only Fiber content of `await_wait_deadline` is the `me` token and the suspend/switch tail | YES |
| 4 | Cancellation closure is token-independent | scheduler_park_wake.cpp:1235-1249 — contains gate → CAS → retire; header comment (scheduler.hpp:1311-1327 region) explicitly documents the counter as stackful-frontend bookkeeping, not shared authority | YES |
| 5 | Publication is `Fiber*`-typed | scheduler.hpp:1309 `publish_waiting_fiber_runnable_locked(Fiber*)`; scheduler.cpp:1642-1652 owner lookup + `make_runnable` + `route_runnable_locked(Fiber*, WorkerState*)` | YES |
| 6 | `commit_suspend_locked` couples commitment with stackful mechanism | scheduler.cpp:1360-1382 — raises `suspend_switch_pending` (stack-safety for steal) + `make_waiting()` (Running→Waiting CAS) under G | YES |
| 7 | Mutex/RwLock owner identity is `Fiber*`-typed | scheduler.hpp:655-672 (`owner_` by reference), scheduler.hpp:986-1033 (`writer_owner` `Fiber*&`); recursive detection compares `owner == me` (scheduler_mutex.cpp:47) | YES |
| 8 | QueueWaitCtx binds caller-stack result storage | queue_detail.hpp:21; scheduler_queue.cpp:60/139/214/326 (`&lease`, `&out`), winner writes through `won->user()` at 453/488 | YES |
| 9 | `granted_not_resumed_` is resume-side EM bookkeeping | scheduler_queue.cpp:121-125/197-200/312-315/417-420 (resume-side decrement under G), queue_port.cpp:532/547 (teardown precondition) | YES |
| 10 | AC-2d: registration→commitment is one resolver-excluded CS | scheduler_park_wake.cpp:1145-1188 and scheduler_timer.cpp:104-159 — register + counters + inline rechecks + `commit_suspend_locked` under G(+W); only `context_switch` outside | YES |

FE-1a's classification stands: F3 = NONE; F1 = representation typing;
F2 = two narrow mechanism clusters. FE-1b entry condition satisfied.

---

## THE FROZEN CONTRACT — SEMANTIC VOCABULARY (no concrete classes)

These are contract roles, not C++ types. A frontend maps each role onto its
own representation; the Core states rules only over the roles.

### WaitEpoch

The unit of one wait. Established by ONE registration transition of a
caller-owned, address-stable epoch record from an initial (unlinked)
state into a registered (resolver-observable) state. The epoch carries:
the primitive's per-operation context (e.g. result storage the winner may
write through), the actor identity, the resume target, and side-state
bindings (deadline). The epoch is live from registration until its terminal
outcome is consumed by the caller. A live epoch is resolvable by exactly one
terminal outcome; a terminal or unregistered epoch rejects further
resolution without mutation.

### ActorIdentity

An opaque, equality-comparable identity of the logical execution actor that
performs admission and can own primitive resources (Mutex ownership, RwLock
writer ownership). Semantic requirements:

- equality-comparable, stable for one logical actor for the duration of any
  ownership or admission decision that consults it;
- no behavior; the Core never dereferences it, only compares it;
- does NOT own, address, or execute the continuation.

Current stackful representation: `Fiber*` (`ws->current`). Recursive-lock
detection (`owner == me`, scheduler_mutex.cpp:47) and non-owner unlock
asserts (scheduler_mutex.cpp:328) are ActorIdentity comparisons.

### ResumeTarget

An opaque datum bound to the epoch at admission, sufficient for the winner
side to create the publication obligation and for the frontend's delivery
topology to later resume the suspended continuation. Semantic requirements:

- bound at admission, before the epoch is resolver-observable (A4);
- never executed, dereferenced for semantic decisions, or required to be
  unique by the Core;
- delivery THROUGH it must not execute user code under authoritative locks (A3).

Current stackful representation: `Fiber*` (the same pointer as
ActorIdentity today — permitted coincidence, see A1).

### PublicationEligibility (SuspensionArmedCommit)

The epoch-side commit that authorizes at-most-once asynchronous publication.
Committed inside the admission critical section after the admission closure
authorizes suspension (registration committed, precedence negative, terminal
rechecks negative). Once committed, a terminal winner MAY legally create an
asynchronous publication obligation for this epoch. Before commitment (inline
resolution window), no asynchronous publication obligation may be created.

A2 rule: publication eligibility is a COMMIT, not physical suspension. The
stackful frontend represents it as `Running → Waiting`
(`commit_suspend_locked`, scheduler.cpp:1360-1382). A stackless frontend
represents it as its own armed state (the coroutine may already be physically
suspended when the commit executes). No frontend-neutral rule may be phrased
in terms of a physical suspend/resume event.

### TerminalOutcome

Exactly one of {woken, cancelled, expired}, won by exactly one atomic
terminal transition on the epoch (`resolve_` CAS, wait_node.hpp:240-250).
Timeout is not cancellation. Absorbing: losers observe, never overwrite.

### PublicationObligation

The exactly-once obligation a winner creates for an armed epoch: deliver the
resume target to the frontend delivery topology, after winner / side-state /
resource / accounting commits are all complete. An obligation is created at
most once per epoch, only for an armed epoch, and its DISCHARGE (continuation
execution) is decoupled from its COMMITMENT (A3).

---

## THE FROZEN CONTRACT — LAWS (corrected from source; supersede the candidate draft)

L1. Registration establishes exactly one live WaitEpoch: a single-shot
    transition; a registered or terminal epoch rejects re-registration
    without mutation. (wait_node.hpp:218-227, register_ returns false.)

L2. At admission — atomically w.r.t. every resolver — the epoch binds its
    actor identity, resume target, and per-epoch result storage FULLY, before
    the epoch becomes resolver-observable. "Resolver-observable" begins when
    the authoritative admission critical section releases; binding completed
    anywhere inside that section satisfies this law, whatever the textual
    order of the CAS and the binding writes. (A4. Current code binds
    `fiber_`/`home_` after the state CAS but under G+W —
    wait_node.hpp:218-227 + scheduler_park_wake.cpp:1161-1167 — which
    satisfies the semantic rule.)

L3. The primitive-defined outcome precedence (resource-available /
    already-due deadline / closed-state / suspension) is evaluated inside one
    serialized admission authority, atomic w.r.t. every resolver.

L4. Exactly one terminal outcome wins the epoch (one CAS). Losers perform no
    second unlink, no resource mutation, no side-state retirement, no
    accounting mutation, no publication.

L5. The winner, within one resolver-excluded critical section, completes in
    order: structural removal (exactly once) → side-state retirement (timer
    exactly once, state-gated before waiter dereference) → primitive resource
    commit/reconciliation → epoch accounting closure — all before any
    publication. Side state (timer/cancel/primitive) retires exactly once.

L6. An admission-time (inline) resolution of a caller that has NOT committed
    publication eligibility publishes nothing; the caller consumes the
    outcome inline. (scheduler_event.cpp:226-243 banner: "the fiber does NOT
    suspend... no publication".)

L7. Publication eligibility is committed atomically with admission w.r.t.
    the resolver set: commitment happens inside the same resolver-excluded
    critical section that closes admission. (scheduler_park_wake.cpp:1178;
    scheduler_timer.cpp:158.)

L8. An asynchronous publication obligation is created at most once, and only
    for an epoch whose publication eligibility was committed. (The
    `make_runnable` exactly-once guard is the current stackful HOME of this
    law — fiber.cpp:6-22 — not the law itself.)

L9. Publication commitment may occur under authoritative locks. User
    continuation execution MUST NOT: execution is deferred until after the
    authoritative locks are released. (A3. Today: publication =
    `make_runnable` + inbox push under G; the continuation executes later on
    the worker loop, outside G. No `publish(target){ target.resume(); }`
    exists anywhere in resolver critical sections.)

L10. Physical suspend/resume topology is frontend-specific and appears in no
     rule above. (A2.)

L11. ActorIdentity and ResumeTarget may map to one value in a given frontend
     but are NOT semantically identical. Ownership semantics (recursive
     detection, non-owner checks, handoff) consult ActorIdentity ONLY, never
     ResumeTarget addresses. (A1.)

L12. Timer authority is independent of waiter storage: expiry gates on timer
     state before any waiter dereference; waiter destruction after terminal
     resolution is always safe. (timer_registration.hpp:8-31.)

L13. Teardown of a primitive/Scheduler domain requires zero linked epochs,
     zero active timers, and zero published-but-unconsumed winners.
     (queue_port.cpp:532-547.)

§28-style check: no law names Fiber, FiberState, Worker, worker id,
context_switch, runnable deque, steal, sched_ctx, stack, or
Running/Waiting. The only admitted abstractions are the seven roles above.
§29 check: Event level/broadcast, Queue FIFO/lease/close, RwLock
fairness/batching, Mutex handoff, Semaphore permit transfer, Condition
release/reacquire choreography appear only as "primitive-defined" (L3) — the
contract does not absorb them.

---

## THE FOUR ADVERSARIAL CORRECTIONS (binding)

### A1 — ActorIdentity ≠ ResumeTarget

Today one `Fiber*` serves both roles: it is compared as ActorIdentity
(`owner == me` recursive detection, scheduler_mutex.cpp:47; `writer_owner`
comparisons, scheduler.hpp:986-1033) and consumed as ResumeTarget (winner
tails `f = won->fiber()` → `publish_waiting_fiber_runnable_locked(f)` →
worker resume). The frozen contract declares the roles DISTINCT (L11). The
stackful frontend MAY continue to map both to one `Fiber*`; the contract
forbids declaring them identical, so a second frontend may split them (a
coroutine's actor identity need not be its `coroutine_handle<>`). Consequence
already in force: ownership decisions must be phrased as ActorIdentity
comparisons, never as "same resume target address".

### A2 — PublicationEligibility commit ≠ physical suspension

`commit_suspend_locked` currently fuses two things: (i) the FN obligation
"commit eligibility inside the resolver-excluded CS" and (ii) the stackful
representation `suspend_switch_pending` + `make_waiting()` (scheduler.cpp:
1360-1382), where the pending flag exists for stack-safety of unsaved CPU
context during steal. The contract freezes (i) as
PublicationEligibilityCommit and classifies (ii) as EM. A stackless
frontend's commit is its own armed-state store; the coroutine may already be
physically suspended at that point (await_suspend runs after the coroutine
suspended). No FN rule may reference Running→Waiting.

### A3 — Publication commit ≠ continuation execution

The current publication seam commits an obligation (FiberState CAS + inbox
push) and never executes user code under G. The contract freezes the split:
commit-publication-under-lock is legal (L9 first half); discharge — any user
continuation execution, including a Condition waiter's reacquire epoch —
happens only after authoritative locks are released (L9 second half). A
frontend adapter that delivered by calling `resume()` inside a resolver CS
would violate the frozen contract even though it might "work" today.

### A4 — Continuation binding visibility is semantic, not textual

The candidate draft said "write token before CAS Registered". The current
code CASes `state_` first, then binds `fiber_`/`home_` (wait_node.hpp:
218-227), which is correct ONLY because the whole binding window sits under
G+W and resolvers require G+W. The frozen rule (L2) is semantic: full
binding before resolver-observability, where observability is gated by the
authoritative synchronization protocol. A future representation that CASes
first and binds later OUTSIDE the authoritative CS would violate L2 even if
it copied the textual order.

---

## FE-1b ENTRY/EXIT DECISIONS

- FE-1a verdict (PASS WITH REPRESENTATION COUPLING): re-confirmed; treated as
  evidence, re-verified mechanically (table above).
- Contract expressible without stackful concepts: YES (L1–L13).
- Stackful frontend conforms unchanged: YES — every law maps to current
  behavior; nothing in L1–L13 requires a production change to satisfy.
- No second timer/cancel/admission/resource authority is implied: the
  contract extends existing single authorities (resolve_ CAS, ordinary
  deadline lifecycle, cancellation closure, admission CS) to any frontend
  that binds the seven roles.

Therefore: **FE-1b PASS — CONTRACT FROZEN.** Campaign proceeds to FE-1c.

---

## WHAT FE-1b DOES NOT DECIDE

- The C++ representation of the seven roles (FE-1c).
- Whether any production signature changes (FE-1c; only if mechanically
  earned).
- The stackless frontend shape (FE-2).
- Public API (post-FE-2 decision; §44).
