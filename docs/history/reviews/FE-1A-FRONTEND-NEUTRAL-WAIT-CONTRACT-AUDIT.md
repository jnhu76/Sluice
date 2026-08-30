# FE-1a — Frontend-Neutral Wait Contract Audit

Status: ARCHIVED AUDIT REPORT (read-only phase; no production change in stage FE-1a;
archived with the FE campaign PR).
Audit class: OBSERVE → CLASSIFY → PROVE.
Non-goals honored: no coroutine design, no seam implementation, no API change,
no #227 edit, no WaitNode/WaitQueue/Scheduler modification.

---

## VERDICT

**FE-1a PASS WITH REPRESENTATION COUPLING — SEMANTICS REUSABLE, TYPE ADAPTER REQUIRED**

- No F3 (semantic-authority coupling) was found. Every R2 semantic authority —
  terminal winner, unlink law, deadline lifecycle, cancellation closure,
  admission precedence, suspension-permission atomicity,
  winner-before-publication, exactly-once retirement, primitive
  reconciliation — is implemented and provable without any Fiber semantic.
- F1 (representation coupling) exists and is enumerable: `WaitNode::fiber_`,
  `AsyncMutex::owner_` / `AsyncRwLock` `writer_owner` typed `Fiber*`,
  `WaitRecord{Fiber*, WorkerState*}`, publication seam signature `Fiber*`.
- F2 (mechanism coupling) exists and is narrow: the physical suspension
  cluster (`commit_suspend_locked` + `suspend_switch_pending` +
  `context_switch` + `g_worker`) and the worker-routing cluster
  (`route_runnable_locked`, inbox, steal, idle-dance, `granted_not_resumed_`
  resume-side decrement). Both are already centralized at ONE seam function
  each; a second frontend adapts at exactly these two points without
  duplicating R2 authority.
- FE-1b entry condition (§35): SATISFIED — no F3.

---

## BASE

```text
HEAD == origin/master == 5706a6ddfa23d7057a29e60be414139e4698e3e5   (BASE_SHA)
PR #242 verified MERGED (merge commit 5706a6d, mergedAt 2026-08-28T14:16:17Z)
PR #238 MERGED 2026-08-27  centralize ordinary deadline registration lifecycle
PR #240 MERGED 2026-08-27  centralize narrow wait cancellation authority
PR #241 MERGED 2026-08-28  minimal deterministic runnable-choice seam (DST-PV-1)
PR #242 MERGED 2026-08-28  close R2 wait lifecycle after queue liveness repair
```

Working tree: only the known human-owned untracked
`docs/history/reviews/R2-WAIT-LIFECYCLE-FINAL-CLOSEOUT-REPORT.md`
(preserved untouched). No tracked dirty state. No production code, no public
API, no test, and no roadmap document was modified by this audit.

Inputs inspected: AGENTS.md; issues #221/#225/#226-referenced #227 ledger/#237
(GitHub); the current tree under `include/sluice/async/` and `src/async/`
(wait_node.hpp, wait_queue.hpp, scheduler.hpp, scheduler.cpp,
scheduler_park_wake.cpp, scheduler_timer.cpp, scheduler_event.cpp,
scheduler_condition.cpp, scheduler_queue.cpp, scheduler_rwlock.cpp,
scheduler_mutex.cpp excerpts, fiber.hpp, fiber.cpp, timer_registration.hpp,
queue_port.hpp excerpts). Historical reports used as evidence only; the
current tree is authority.

---

## WHY FE-1a EXISTS

R2 closed with "NO FURTHER CENTRALIZATION EARNED" and single-owner
authorities for registration, terminal resolution, ordinary deadline
lifecycle, cancellation closure, suspend commitment, primitive
reconciliation, and runnable publication. The open question is whether those
authorities are frontend-neutral or accidentally stackful-coupled. FE-1a
answers it on current master by classifying every R2 fact as FN / PP / EM and
by attempting to falsify the neutrality claim.

---

## CURRENT R2 AUTHORITY MAP

| Authority | Owner (file) | Mechanism today |
|---|---|---|
| Wait-epoch registration | `WaitQueue::register_wait_locked` (wait_queue.hpp:185) under `Scheduler::await_wait*` (scheduler_park_wake.cpp:1145, scheduler_timer.cpp:78, scheduler_event.cpp:226/307, scheduler_queue.cpp:45/131/204/319, scheduler_rwlock.cpp, scheduler_condition.cpp:30/116) | Detached→Registered CAS + FIFO tail-link + count, one G+W CS |
| Terminal resolution | `WaitNode::resolve_` (wait_node.hpp:240) — ONE CAS | Registered→{woken,cancelled,expired}, acq_rel |
| Unlink | `WaitQueue::unlink_locked` (wait_queue.hpp:319), winners only, same CS as CAS | Single structural-removal seam |
| Ordinary deadline lifecycle | `prepare_/publish_/consume_/retire_ordinary_deadline_locked` (scheduler.hpp:1994-2065, scheduler_timer.cpp:341-415) + `pump_deadlines_locked` | May-throw prepare → noexcept publish; CAS ACTIVE→{CONSUMED,RETIRED}; exactly-once `active_deadline_count_` |
| Cancellation closure | `Scheduler::cancel_primitive_wait_locked` (scheduler_park_wake.cpp:1235) | `contains_locked` membership gate → `cancel_locked` CAS+unlink → `retire_timer_for_node_locked` |
| Suspend commitment | `Scheduler::commit_suspend_locked` (scheduler.cpp:1360) | raise `suspend_switch_pending` + `make_waiting()` under G |
| Reconciliation / grant | `mutex_handoff_one_locked`, `queue_grant_{consumer,producer}_locked`, `rwlock_grant_from_head_locked`/`rwlock_claim_node_woken_locked` | resolve → owner/resource commit → timer retire → accounting → publication LAST |
| Runnable publication | `publish_waiting_fiber_runnable_locked` (scheduler.cpp:1642) → `route_runnable_locked` (scheduler.cpp:1559) | `make_runnable()` exactly-once guard → owner routing → wake epoch |
| Timer storage | `timer_pool_` / `deadline_heap_` / `TimerRegistration` (timer_registration.hpp) | Scheduler-owned, address-stable, lazy-at-deadline reclamation, `TimerLifetimeClosure` |

---

## COMPLETE FN / PP / EM MATRIX

Legend: FN = frontend-neutral semantic fact; PP = primitive-specific policy;
EM = execution mechanism / frontend detail. "Fiber dep?" asks whether the
*semantic* fact requires a Fiber (no = the Fiber appearing at the site is
representation only).

| R2 fact | Current authority | file/function | Current representation | Current consumer | Class | Fiber dep? | Worker dep? | Scheduler dep? | Semantic or representation-only? | Stackless reuses authority unchanged? | Blocker if not | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Wait registration (epoch establishment) | WaitNode CAS + WaitQueue link | wait_node.hpp:218, wait_queue.hpp:185 | Detached→Registered acq_rel CAS; intrusive FIFO tail | All admit seams | FN (single-shot epoch) + PP (FIFO discipline) | NO | NO | YES (seam under G) | Semantic: exactly one live epoch, reuse rejected | YES | — | wait_queue.hpp:186-197 |
| Waiter identity (continuation token) | `WaitNode::fiber_` | wait_node.hpp:129/192/252 | `Fiber*` recorded immutable at register | Winner publication tail; Mutex/RwLock owner commit | EM (representation) | Representation only | NO | NO | The *need* for one opaque identity bound at admission is semantic; Fiber* is the current token | YES with type adapter | token type | wait_node.hpp:53-63 |
| Queue membership | WaitQueue intrusive links | wait_queue.hpp:196-198/319-334 | next_/prev_/home_ under queue mtx_ | Resolver unlink; contains gate | FN (membership serialized w.r.t. resolvers) + EM (list) | NO | NO | YES | Semantic: linked-at-most-once, terminal nodes unreachable | YES | — | wait_queue.hpp:44-51 |
| Terminal winner | `resolve_` CAS | wait_node.hpp:240-250 | atomic State CAS | wake/cancel/expire/inline resolvers | FN | NO (outcome enum has no Fiber semantics) | NO | NO (CAS is lock-free authority) | Semantic: exactly-one winner | YES unchanged | — | wait_node.hpp:24-57 |
| Terminal outcome | `WaitOutcome` {woken,cancelled,expired} | wait_node.hpp:80-98 | enum on node | Caller resume-side read | FN | NO | NO | NO | Semantic | YES | — | wait_node.hpp:75-98 |
| Unlink | winners-only, same CS | wait_queue.hpp:319 | pointer splice | wake_one/cancel/expire/wake_node | FN (Unlink Law) | NO | NO | YES (CS) | Semantic: terminal ⇒ unlinked exactly once | YES | — | wait_queue.hpp:18-50 |
| Deadline prepare | `prepare_ordinary_deadline_locked` | scheduler_timer.cpp:341 | heap reserve + pool node, MAY THROW, mutates nothing | Timed admissions | FN (strong admission-atomicity rule) + EM (containers) | NO | NO | YES (G) | Semantic: no admission mutation before potentially-throwing prepare | YES | — | scheduler.hpp:1958-1996 |
| Deadline publish | `publish_ordinary_deadline_locked` | scheduler_timer.cpp:379 | hook install + count + heap push + cache refresh, noexcept | Timed admissions | FN (publish is exception-free after commit) + EM (heap/cache) | NO | NO | YES | Semantic + storage | YES | — | scheduler.hpp:1998-2012 |
| Deadline consume (timer wins) | `consume_ordinary_deadline_locked` | scheduler_timer.cpp:405 | CAS ACTIVE→CONSUMED, --count | pump/inline already-due | FN (exactly-once timer authority) | NO | NO | YES | Semantic | YES | — | scheduler.hpp:2049-2056 |
| Deadline retire (non-timer winner) | `retire_ordinary_deadline_locked` / `retire_timer_for_node_locked` | scheduler_timer.cpp:411/417 | CAS ACTIVE→RETIRED in winner CS before publication | All winner paths | FN (timer-lifetime closure: retire-before-publish, state-gate before node deref) | NO | NO | YES | Semantic (prevents stale-expiry UAF) | YES | — | scheduler_timer.cpp:206-210/417-452 |
| Cancellation closure | `cancel_primitive_wait_locked` | scheduler_park_wake.cpp:1235 | contains gate → CAS → retire | Event/Sem/Mutex/Cond/Queue/RwLock cancel seams | FN (target membership, one Cancelled winner, timer retire) | NO | NO | YES | Semantic; repo already documents it "frontend-neutral ONLY through those steps" (scheduler.hpp:1311-1327) | YES | — | scheduler_park_wake.cpp:1237-1244 |
| Already-due precedence | admission closure | scheduler_timer.cpp:131-146, scheduler_event.cpp:362-373, scheduler_queue.cpp:281-290, scheduler_condition.cpp:166-181, scheduler_rwlock.cpp:535-552 | inline `expire_locked` before suspend | All timed admits | FN (rule) with PP ordering (resource-first vs deadline-first is per-primitive) | NO | NO | YES | Semantic: a due deadline must not strand the suspender | YES | — | scheduler.hpp:475-481 |
| Resource precedence | per-primitive admission predicate | event SET; sem permit+head; mutex owner-free+head; queue open+space+head; rwlock head-mode | under G(+S)+W | admit seams | PP (primitive-defined) — the *requirement* that precedence be primitive-defined under one serialized authority is FN | resource identity only (Mutex/RwLock owner is Fiber*-typed: F1) | NO | YES | Policy | YES | — | scheduler.hpp:590-634/674-710/966-1005 |
| Closed-state precedence | Queue close disposition | scheduler_queue.cpp:91-95/167-171/269-277/374-382 | wake with lease retained / out empty | Queue admits + close drain | PP (Queue policy) | NO | NO | YES | Policy | YES | — | scheduler_queue.cpp:535-556 |
| Suspend permission | admission tail rechecks | every admit seam: register → counters → inline-resolution rechecks → terminal recheck | under G(+role) | before commit_suspend | FN (permission = "registered, not inline-resolved, not terminal, all under resolver-excluded CS") | NO | NO | YES | Semantic (AC-2d core) | YES if commitment stays in the CS | see AC-2d section | scheduler_park_wake.cpp:1145-1179 |
| Suspend commitment | `commit_suspend_locked` | scheduler.cpp:1360 | `suspend_switch_pending`=true + `make_waiting()` CAS | all suspend paths | FN obligation (commit inside resolver-excluded CS) + EM representation (FiberState; the pending flag is stack-safety for steal) | Representation | Representation (steal safety of unsaved stack) | YES | Obligation semantic; flag is stackful-specific | YES (frontend supplies its own commitment mutation) | commitment seam | scheduler.cpp:1366-1381 |
| Physical suspension | `fiber_ctx::context_switch` | scheduler_park_wake.cpp:1139-1143 etc. | stackful switch to sched_ctx | end of every admit seam | EM | YES — entirely | YES (worker sched_ctx) | YES | Mechanism only | NO — replaced by await_suspend | replaced mechanism | all admit seams |
| Resource commit | primitive-defined, between resolve and publication | mutex owner=f; queue ring move; rwlock readers/writer state | under G(+S)+W | grant seams + inline admits | FN ordering law ("winner-before-publication") + PP content | owner identity token (F1) | NO | YES | Ordering semantic; commit content is PP | YES | — | mutex_handoff (scheduler_mutex.cpp), scheduler_queue.cpp:444-511 |
| Primitive reconciliation | Queue Q-LIV-1 (opposite-role grant after inline success), RwLock head reconcile after cancel/expire/unlock, Condition reacquire-by-caller | scheduler_queue.cpp:107-110/183-186/297-300/402-405, scheduler_rwlock.cpp:432-440/468-475, condition.cpp caller | grant seams | resource-changing winners | FN obligation ("a resource-changing winner performs required reconciliation") + PP specifics | NO | NO | YES | Obligation semantic | YES | — | scheduler_queue.cpp:100-110 |
| Runnable publication | `publish_waiting_fiber_runnable_locked` | scheduler.cpp:1642 | owner lookup + `make_runnable()` CAS + route | every winner tail | FN fact (one publication obligation, at most once, only for a truly-suspended continuation) + EM (FiberState guard, owner map) | Representation + guard home | YES (routing target) | YES | Fact semantic; mechanism EM | YES via adapter | publication seam | scheduler.cpp:1427-1437 |
| Continuation routing | `route_runnable_locked` | scheduler.cpp:1559 | owner Worker inbox / pending_spawn_ / steal / wake epoch / idle-dance | publication | EM | YES | YES | YES | Mechanism only | NO — frontend schedules its own continuation | replaced mechanism | scheduler.cpp:1559-1629 |
| Wait accounting | `waiting_waitq_count_` (+select) | scheduler.hpp:1500/1543 | G-guarded count | MW-S3 classification; every admit/winner path | FN obligation (exactly-once epoch retirement; unresolved-wait liveness is Scheduler-Core) with current counter = frontend bookkeeping — the repo states this split itself (scheduler.hpp:1317-1320) | NO | NO | YES (Scheduler-Core per §17) | Obligation semantic; counter representation | YES (Scheduler keeps the count) | — | scheduler.hpp:1311-1327, 1491-1500 |
| Timer accounting | `active_deadline_count_`, per-port `active_queue_timers_`/`active_wait_associations_` | scheduler.hpp:1848, scheduler_queue.cpp:569-586 | counts under G / S | pump, retire/consume | FN (exactly-once ACTIVE→terminal) + PP (per-port) | NO | NO | YES | Semantic balance | YES | — | scheduler.hpp:1930-1956 |

Additional mechanism inventory (all EM): `waiting_ready_`/`waiting_size_`/
`waiting_void_` maps (legacy/flag registries, scheduler.hpp:1487-1489);
`WaitRecord` registry (identity completion route, scheduler.hpp:1203-1212);
`fiber_owner_` map; `suspend_switch_pending`; `granted_not_resumed_`
(resume-side decrement + teardown precondition, queue_port.hpp:447,
queue_port.cpp:532/547); wake epoch/idle-dance; `g_worker` TLS.

---

## WAITNODE AUDIT

What WaitNode owns: the lifecycle state (`state_`: detached/registered/
woken/cancelled/expired), the terminal-outcome projection (`WaitOutcome`),
the immutable continuation token (`fiber_`), the per-op context slot
(`user_`), and (via WaitQueue under its mtx_) the intrusive links.

What WaitNode does NOT own: scheduler state, timers, primitive resource
state, queue structure (delegated to WaitQueue under mtx_), any wake/route
mechanism, any reference to the Scheduler.

Who transitions terminal state: exactly the `resolve_(outcome)` CAS
(wait_node.hpp:240). Absorbing terminals; losers get `false` and must do
nothing (§2/§7 Design Law).

Is terminal outcome independent of Fiber? YES — `WaitOutcome` and `State`
encode no fiber concept; the CAS tests only Registered→terminal.

Does `resolve_` require Fiber semantics? NO. It stores a terminal value and
returns the win bit. "Continuation target" is *data on the node* (`fiber_`),
never consulted by the resolver's decision.

Does queue membership depend semantically on Fiber? NO — membership is the
intrusive link set under the queue mtx; the fiber pointer is payload.

Does node lifetime assume stack/Fiber lifetime? The semantic rule is:
caller-owned, address-stable while registered, and MUST be terminal (or
never registered) before its owning frame is destroyed (wait_node.hpp:52-57,
131-138). Today the frame is a blocked fiber's stack frame. Nothing in the
authority requires *stack* storage — a coroutine frame satisfies the same
rule naturally; a callback frontend must provide equivalent address
stability (heap or scope discipline).

Can a future stackless waiter use the same terminal-state machine without
becoming a Fiber? YES — the machine is {Detached, Registered, T∈{woken,
cancelled, expired}} with a single CAS; it never mentions Fiber.

Any stackful invariant forcing duplication of terminal authority? NONE
found (attempted falsification §25 below).

Authority reuse vs object-layout reuse: the authority (one CAS, absorbing
terminals, winner-owns-unlink) is reusable verbatim. The object layout
(`Fiber* fiber_` member) would need parameterization (token type) in a
second frontend — F1, not failure. FE-1 does not require the current layout
to serve every frontend.

## REGISTRATION AUDIT (`register_wait_locked`)

`WaitQueue::register_wait_locked(node, fiber)` (wait_queue.hpp:185)
establishes, under the queue mtx (taken under G by every Scheduler admit
seam):

1. waiter membership — CAS Detached→Registered + FIFO tail-link (structural);
2. terminal eligibility — Registered is the only resolvable state;
3. ordering — FIFO tail position (WaitQueue's own discipline; PP at this
   layer; primitives may impose additional head/eligibility rules on top,
   e.g. RwLock head-mode dispatch, Queue FIFO role);
4. continuation identity — records `fiber` (opaque, immutable) on the node;
5. stackful Fiber binding — none beyond (4): no scheduler map, no stack
   address, no worker affinity is taken at registration.

Separation verdict: (1)(2)(3) are FN semantic facts (plus the queue's FIFO
policy as PP); (4) is the semantic requirement "bind one continuation
identity at admission" with an EM representation; (5) does not exist.

Could a stackless frontend reuse the registration semantic contract while
binding a different continuation token? YES — the contract is
single-shot-CAS + membership + one opaque token recorded before the epoch
becomes resolver-visible. The correctness fact that makes the token
necessary is semantic, not stackful: resolution and suspension happen on
different threads (the resolver is the waker/canceller/timer thread), so the
epoch must carry the resume target recorded at admission. That is option-B
(neutral token), not option-C, and it is NOT Fiber-specific.

## SUSPEND-COMMIT AUDIT (`commit_suspend_locked`)

A. Semantic permission to suspend: established by the CALLER before the
call — registration committed, counters incremented, inline-resolution and
terminal rechecks negative, all inside the resolver-excluded G(+role) CS
(scheduler.cpp:1387-1389 precondition). `commit_suspend_locked` itself makes
NO permission decision.

B. Concrete mutation that parks a Fiber: `suspend_switch_pending.store(true,
release)` then `fiber->make_waiting()` (Running→Waiting CAS) under G
(scheduler.cpp:1378-1381). The Waiting state is (i) the visibility act that
makes the continuation "suspended" and (ii) the anchor for the
publication-side exactly-once guard (`make_runnable` returns true only from
created/waiting, fiber.cpp:6-22).

C. Physical context switch: strictly OUTSIDE the lock, in each admit seam's
tail (e.g. scheduler_park_wake.cpp:1184-1187).

Does current code conflate them? Partially but benignly: A is fully
separated (precondition, not performed here); B and the *stack-safety* role
of `suspend_switch_pending` are fused — the flag exists so `try_steal`
refuses a ticket whose victim's CPU context is not yet saved
(scheduler.hpp:181-196), which is a stackful-only concern riding on the
commitment seam. The commitment obligation itself ("commit suspension inside
the resolver-excluded CS, after authorization") is FN.

What fact authorizes suspension? "The epoch is Registered, un-resolved,
not inline-resolvable, and every resolver is excluded from the critical
section until commitment is visible." That is the AC-2d atomicity contract.

Does commit_suspend_locked decide a semantic winner? NO — winners are
decided exclusively by the resolve CAS on the node. The seam commits a
previously-authorized execution mechanism.

Under co_await: A survives verbatim (the admission closure is unchanged).
B's obligation survives; its representation becomes the coroutine frame's
suspended-state + the awaiter's await_suspend returning the handle inside
the same CS. The stack-safety half of `suspend_switch_pending` disappears
(no unsaved stack). C is replaced by await_suspend (returning to the caller
or suspending the coroutine).

## DEADLINE AUDIT

R2-ALLOC merged shape (scheduler_timer.cpp:78-168): prepare (may throw;
nothing mutated — heap reserve + pool node) → register (noexcept) →
++counters (noexcept) → publish (noexcept: hook + ACTIVE count + heap push +
cache refresh) → already-due closure → terminal recheck → commit_suspend →
switch.

Does any deadline semantic fact require Fiber / FiberState / worker / stack
context? NO. The deadline contract is expressed entirely over: wait epoch
identity ({node, queue} binding), queue membership, the terminal authority
(the same resolve CAS — expired is a third outcome, no second winner
protocol), the timer's own ACTIVE/RETIRED/CONSUMED identity, and
Scheduler-local bookkeeping. `await_wait_deadline`'s only Fiber content is
`me` as the registration token and the suspend/switch tail (EM).

Timer storage mechanism vs deadline semantic contract: storage (pointer-
stable pool, min-heap, lazy-at-deadline reclamation, earliest-deadline
atomic cache for park timeout) is EM/infrastructure. The semantic contract
is FN: absolute monotonic deadline (`expired iff now >= deadline`),
already-due admission closure, non-timer winner retires the timer in its CS
before publication, expiry gates on ACTIVE before dereferencing the node
(TimerLifetimeClosure, timer_registration.hpp:8-31), exactly-once
ACTIVE→terminal with exactly-once count decrement
(scheduler.hpp:1930-1956). The storage could be re-plumbed without touching
one semantic rule. Queue-local arming divergence
(queue_push/pop_admit_until keeps its local publish order,
scheduler_queue.cpp:234-247) is an accepted PP/mechanism divergence, not a
Fiber dependency.

## CANCELLATION AUDIT

`cancel_primitive_wait_locked` (scheduler_park_wake.cpp:1235-1249): exact
target membership (`contains_locked` scan under G+W) → `cancel_locked` (the
Cancelled CAS + unlink in the same CS) → `retire_ordinary_deadline` path
(`retire_timer_for_node_locked`). No Fiber inspection, no publication, no
counter, no primitive-resource mutation, no allocation.

FN: exact target membership; Cancelled as one competing terminal outcome;
unlink-by-winner; timer retirement; no-synthesis-of-wake. EM: the winning
CALLER's `--waiting_waitq_count_` and the `Fiber* f = node.fiber()` +
`publish_waiting_fiber_runnable_locked(f)` tail — both sit at the six
primitive cancel seams (event_cancel_wait, sem_cancel, mutex_cancel,
condition_cancel_wait, queue_cancel, rwlock_cancel), not in the closure.
PP: Queue/RwLock post-cancel reconcile (head grant), Mutex "does not change
owner", Condition "does not change owner".

Would stackless cancellation need a second terminal/cancel authority? NO.
The closure is already the single authority and its own header comment
records the frontend-neutrality split (scheduler.hpp:1311-1327): the
exactly-once retirement obligation is semantic; the concrete counter is
current stackful-frontend bookkeeping. A stackless frontend reuses the same
closure and supplies its own publication tail.

## PUBLICATION AUDIT

The frontend-neutral fact underneath `make_runnable` /
`publish_waiting_fiber_runnable_locked` / `route_runnable_locked` / worker
inboxes:

> A terminal winner whose continuation is asynchronously resumable creates
> EXACTLY ONE continuation-publication obligation, discharged only after
> winner state, side state (timer), primitive resource commit/reconcile, and
> epoch accounting are fully committed, and only if the continuation is
> actually suspended (inline-admission resolutions publish nothing).

Evidence that winner-before-publication is FN while the mechanism is EM:
- Ordering is uniform and load-bearing everywhere:
  resolve CAS → retire timer → resource commit (mutex owner = winner,
  scheduler_mutex.cpp handoff; queue ring move, scheduler_queue.cpp:457-464)
  → counters → make_runnable/route LAST (scheduler.hpp:736-746, 907-919).
- The exactly-once guard (`make_runnable` false ⇒ no second ticket,
  fiber.cpp:6-22; drain comment scheduler.cpp:1427-1434) and the
  "inline Woken of a RUNNING caller publishes nothing" rule
  (scheduler_event.cpp:237-240) are the semantic law; the FiberState CAS is
  its current home (EM).
- `route_runnable_locked`'s contents — owner Worker inbox, steal-eligibility,
  idle-dance epoch invalidation, MW-S2 admission demotion, terminate-clear,
  wake epoch — are pure execution topology (EM), provably not load-bearing
  for primitive correctness (the correctness ordering is complete before
  routing begins).

So: winner-before-publication is FN even though runnable-publication
mechanism is EM. Explicit.

## ACCOUNTING AUDIT

| Counter | Class | Reasoning (not by name) |
|---|---|---|
| `waiting_waitq_count_` | FN obligation / Scheduler-Core representation | Proves "unresolved wait epochs exist" for MW-S3 run liveness, and each epoch's exactly-once retirement. MW classification is Scheduler semantic authority (§17) that survives multi-frontend. The repo itself classifies the concrete counter as "current stackful-frontend bookkeeping... not shared authority" for the *primitive closure* (scheduler.hpp:1317-1320) — i.e., the obligation is semantic, the counter is Scheduler-owned, not primitive-owned. |
| `waiting_select_count_` | FN obligation (Scheduler-Core) | Same MW-liveness role for Armed SelectGroups. |
| `waiting_ready_` map | EM | Legacy flag-wait registry; representation of one wait kind; identity registry already superseded it for arena waits. |
| `waiting_size_` / `waiting_void_` maps | EM | Non-arena fallback registries (documented non-dual). |
| `wait_record_live_count_` + WaitRecord pool | FN bound (bounded resource) + EM representation | wait_capacity-bounded registry; generation-safe reuse mirrors R1 law. |
| `active_deadline_count_` | FN (timer accounting invariant) | Every ACTIVE registration reaches exactly one terminal with exactly-one decrement — semantic balance. |
| `active_queue_timers_`, `active_wait_associations_` | PP | Per-port primitive accounting. |
| `granted_not_resumed_` | EM bookkeeping proving a semantic invariant | Exists only because publication (winner resolver's thread) and consumption (fiber resume) are decoupled by worker routing; guards teardown against a published-but-unconsumed winner. A frontend that resumes inline still needs the invariant but not this counter. |
| `suspend_switch_pending` | EM (stackful-specific) | Steal-safety for an unsaved fiber stack; disappears with stackless suspension. |
| `idle_workers_`, `dance_epoch_`, `wake_epoch_`, `observed_epoch` | EM | Worker-pool park/termination convergence machinery. |

## LIFETIME AUDIT

| Object | Lifetime rule today | Relies on blocked-fiber stack? | Coroutine frame OK? | Raw-callback frontend OK? | Class |
|---|---|---|---|---|---|
| WaitNode | caller-owned; address-stable while Registered; terminal before frame exit | YES (await frame) | YES (frame lives while suspended) | Only with explicit stable storage (heap/scope) | Semantic rule FN; storage EM |
| QueueWaitCtx (node.user()) | stack-local in admit fn; points at caller's stack `QueueItemLease` (`&lease`, scheduler_queue.cpp:60/214) | YES — the winner reconciler writes through `ctx->prod_lease`/`ctx->cons_out` into the suspended caller's frame | YES — lease/ctx move into the coroutine frame | NO without heap | Semantic rule (winner writes the result into the waiter's epoch storage before publication) FN; location EM |
| RwWaitCtx | stack-local {mode} trivial struct | YES | YES | YES (trivially copyable, re-homable) | EM payload, FN role (queue-head mode policy input = PP) |
| TimerRegistration | Scheduler-owned pool; independently-stable state; MUST outlive bound node; state-gate before node deref | NO — explicitly decoupled (timer_registration.hpp:8-31, scheduler.hpp:1800-1818) | YES | YES | FN design already |
| QueueItemControl/lease | caller-owned; moves to ring on commit (location enum) | lease lives in producer frame until commit | YES | needs stable storage | PP + storage EM |
| Condition reacquire epoch | caller runs mutex_.lock() after resume; notifier never touches Mutex | resumes on the fiber | coroutine resumes and reacquires | callback must re-enter reacquire | FN obligation (acquire-after-wait choreography is Condition PP; "caller runs it" is FN) |
| WaitRecord (completion waits) | Scheduler-owned, bounded pool, generation-safe | NO | YES | YES | FN bound + EM representation |

Semantic lifetime rules extracted (frontend-free): (1) epoch state must be
address-stable from registration until terminal + resume-side consumption;
(2) the winner may write through epoch-recorded result storage before
publication; (3) timer authority must be independent of waiter storage;
(4) teardown requires zero linked epochs and zero published-unconsumed
winners. No authority requires stack allocation specifically; the blocked-
fiber stack is today's storage mechanism, and a coroutine frame satisfies
rules (1)(2) structurally.

## LOCK-TO-SEMANTIC MAPPING

| Lock | Correctness relationship enforced (semantic) | Current mechanism (not semantic) |
|---|---|---|
| `global_mtx_` (G) | (a) registration→suspension-commitment is atomic w.r.t. ALL resolvers (AC-2d); (b) admission precedence serializes with set/unlock/release/close linearization; (c) winner-before-publication ordering is one visible step; (d) timer heap/pool mutations are exclusive | ONE Scheduler-global mutex |
| WaitQueue `mtx_` | membership transitions serialize with resolution+unlink (Unlink Law same-CS) | per-queue mutex taken under G |
| QueuePort `state_mtx_` (S) + role mutexes | ring occupancy transitions serialize with role FIFO resolution; the two role mutexes never held together (no cycle) | port mutex + one-role-at-a-time under G+S |
| primitive-passed-by-ref state (`owner`, `available`, counts) | primitive resource policy mutates only under the authoritative locks — no lock-free admission | G(+S)+W discipline |

None of the semantic relationships names a Fiber. `G → wake_mtx_` one-way,
`G → S → one role`, `G → q.mtx()`, `G → inbox_mtx`, registry-leaf rules are
mechanism. A second frontend sharing the Scheduler semantic critical section
inherits the proofs unchanged (see AC-2d section).

## REPRESENTATIVE PRIMITIVE WALKTHROUGHS

### Event (`await_event_wait[_deadline]`, scheduler_event.cpp:226-393)

1. caller enters; `me` captured from `ws->current` — **EM** (token source)
2. register node on `waiters_` (CAS+link), `++waiting_waitq_count_` — **FN**(epoch)+**EM**(count site)
3. resource/precedence: SET observed after registration → `wake_node_locked` inline Woken, timer retire, counters, NO publication (caller RUNNING) — precedence rule **PP** (Event level-state), inline-no-publication law **FN**
4. deadline possibility (timed overload): resource-first, then already-due inline Expired (prepare/publish R2-ALLOC order) — **FN**(closure/order)+**PP**(precedence)
5. suspend authorization: terminal recheck negative under G+W — **FN**
6. physical suspend: `commit_suspend_locked` (**FN** obligation + **EM** FiberState) then `context_switch` (**EM**)
7. terminal winner: set() broadcast `event_set_broadcast` (SET exchange + drain loop, one G CS) / cancel / timer — resolve CAS — **FN**; set/reset epoch isolation under G — **FN**(atomicity) achieved by lock (**EM**)
8. primitive reconcile: none (broadcast drains all; reset touches no node) — **PP** (level semantics)
9. continuation publication: retire→count→`publish_waiting_fiber_runnable_locked` — **FN** fact, **EM** mechanism
10. resume-side cleanup: caller reads `node.outcome()` — **FN**

### AsyncQueue (`queue_push/pop_admit[_until]`, scheduler_queue.cpp)

1. caller enters; detach control → producer_operation; `QueueWaitCtx{port, role, c, &lease, &out}` onto `node.set_user` — **PP**(lease semantics)+**EM**(stack ctx)
2. register on role FIFO under G+S+role; `++active_wait_associations_`, `++waiting_waitq_count_` — **FN**(epoch)+**PP**(port counts)
3. resource/precedence: Open+space+FIFO-head → commit lease into ring inline + inline Woken + Q-LIV-1 opposite-role grant after role-mutex release — **PP**(capacity/FIFO/close)+**FN**(reconcile obligation, winner-before-publication)
4. deadline (timed): resource-first; already-due inline Expired with lease retained — **FN**(closure)+**PP**(expired disposition)
5. suspend authorization: rechecks negative under G+S+role — **FN**
6. physical suspend: commit (**FN**+**EM**) + switch (**EM**)
7. terminal winner: reconciler grant (`queue_grant_{producer,consumer}_locked`) / close drain / cancel / pump-expiry — resolve CAS **FN**; ring read/write in winner CS **PP**
8. primitive reconcile: grant performs resource commit (ring move / lease→slot) BEFORE publication; retire BEFORE commit — **FN**(order)+**PP**(content)
9. publication: counters, `granted_not_resumed_++`, make_runnable, route — **FN** fact + **EM**
10. resume-side: post-resume `--granted_not_resumed_` under G (**EM**); read lease (`null` = committed) — **PP** contract
Teardown: `begin_teardown` requires zero epochs/timers/published-winners/linked waiters — **FN** (resource balance).

### AsyncRwLock (`rwlock_*`, scheduler_rwlock.cpp)

1. caller enters; `RwWaitCtx{mode}` on node — **PP**(mode)+**EM**(stack)
2. register on the single unified FIFO — **FN**(epoch)+**PP**(unified FIFO choice)
3. resource/precedence: head-mode dispatch — writer-fair (head writer blocks reader batch at FIFO boundary) / maximal consecutive reader prefix — **PP**(fairness policy); claim via the ONE `rwlock_claim_node_woken_locked` (resolve+unlink+retire+count, no publication) shared by grant and inline admission (anti-drift) — **FN**(single claim authority)
4. deadline: resource-first, already-due Expired, timer via ordinary authority + `rwlock_timer_expire_reconcile` binding — **FN**+**PP** binding
5. suspend authorization — **FN**
6. physical suspend — **FN** obligation + **EM**
7. terminal winner: unlock_read/unlock_write/cancel/expire → `rwlock_grant_from_head_locked` — resolve CAS **FN**
8. primitive reconcile: head reconcile after ANY unlink (cancel/expire); commit (`active_readers += batch` / `writer_active+owner=f`) BEFORE publication — **FN**(order)+**PP**(batching/fairness)
9. publication: after W release, under G, per-fiber make_runnable + route — **FN** fact + **EM**
10. resume-side: `node.set_user(nullptr)`; readers hold shared count — **PP**

### AsyncCondition (`condition_wait_prepare[_until]`, scheduler_condition.cpp)

1. caller must own the bound Mutex (precondition) — **PP**(mutex choreography)
2. register cond_node on cond_waiters under G (cond queue mtx released BEFORE mutex queue work — sequential topology, never simultaneous) — **FN**(epoch)+**EM**(lock shape)
3. resource/precedence: untimed — none (always releases Mutex); timed — already-due inline Expired RETAINS ownership, no reacquire epoch (`released_mutex=false`) — **FN**(closure + obligation latching)+**PP**(deadline governs Condition epoch only)
4. Mutex release via the ONE `mutex_handoff_one_locked` (owner→winner BEFORE publication) or `owner=nullptr` — **PP**(handoff policy)+**FN**(owner-before-publication order)
5. suspend authorization: register-before-handoff is one CS w.r.t. notify/cancel/expire (lost-notify closure) — **FN** (this is the strongest admission-atomicity exhibit: a notify cannot interleave between Condition registration and Mutex release)
6. physical suspend — **FN** obligation + **EM**
7. terminal winner: notify_one/notify_all drain / cancel / Condition-epoch timer — resolve CAS **FN**
8. primitive reconcile: none by notifier; the winner runs its OWN reacquire epoch after resume — **PP**(choreography)+**FN**(obligation sits with the waiter epoch, latched via `released_mutex`)
9. publication — **FN** fact + **EM**
10. resume-side: latch outcome, reacquire — **PP**

## STACKLESS THOUGHT EXPERIMENT

Pattern for every primitive (event shown; Queue/RwLock/Condition identical
in shape):

```text
CURRENT STACKFUL (await_event_wait):
  admission closure (register + precedence + rechecks)   under G
      -> SAME
  commit_suspend_locked (authority + Waiting)            under G
      -> SAME OBLIGATION / representation replaced
  context_switch (park stack; resume = someone routes)   REPLACED MECHANISM
  winner: resolve_ CAS + retire + counters               -> SAME
  publication: make_runnable + route to owner Worker     SAME FACT / NEW MECHANISM
      (one obligation, at most once, suspended-only)
  resume: worker pops ticket, make_running, switch back  REPLACED MECHANISM
      (co_await: await_suspend returns handle; resumer
       calls handle.resume() wherever the frontend wants)

HYPOTHETICAL STACKLESS (co_await event.async_wait(node, token)):
  admission closure                    SAME (CS unchanged)
  suspend commitment                   SAME CS; frontend stores
                                       "suspended" state for the
                                       exactly-once publication guard
  resolve_/retire/counters             SAME
  publication obligation               SAME; delivered as
                                       adapter.publish(token) at the
                                       SAME source point
  physical suspension/resumption       REPLACED (await_suspend / resume)
```

What survives unchanged: registration contract, terminal-winner CAS and
Unlink Law, deadline prepare/publish/consume/retire + already-due closure +
timer-lifetime closure, cancellation closure with membership gate, admission
precedence and inline-resolution laws, winner-before-publication order,
exactly-once publication/retirement/reconciliation obligations, teardown
balance invariants, the lock-domain semantic relationships.

What is replaced: the physical switch; the FiberState machine as the guard's
home; Worker routing/inbox/steal/idle-dance as the delivery topology;
`g_worker` capture (frontend supplies its continuation identity);
`suspend_switch_pending`'s stack-safety half; `granted_not_resumed_`'s
resume-side decrement site (the invariant stays; the site moves into the
adapter's consume step).

The desired result holds: identical semantic authority; mechanisms localized
to two seams.

## CONTINUATION TOKEN REQUIREMENT

Question: does the semantic Core require `Fiber*`, a generic continuation
token, or only a publication obligation?

Findings from source:

- The terminal/deadline/cancel/admission authorities dereference the token
  NOWHERE: `resolve_`, `expire_locked`, `cancel_locked`, `contains_locked`,
  `prepare_/publish_/consume_/retire_ordinary_deadline_locked`,
  `cancel_primitive_wait_locked` are token-free (only
  `cancel_primitive_wait_locked`'s CALLERS read `node.fiber()` for their
  publication tail).
- The winner's publication tail needs an identity: resolution runs on the
  resolver's thread; the suspender is not discoverable otherwise. The
  binding must be recorded at admission (before the epoch is resolver-
  visible). Mutex/RwLock additionally need a comparable owner identity for
  recursive-detection and handoff — any opaque equality-comparable identity
  suffices; nothing uses fiber-specific behavior.
- Therefore: the minimum semantic requirement is "one opaque continuation
  identity bound at admission + an exactly-once publication guard owned by
  the (epoch, continuation) pair". This is option **B in its minimal form**
  (a generic token IS semantically required, because resolver ≠ suspender
  thread), but the token carries NO behavior and NO fiber semantics — it is
  not `Fiber*`-specific (option A is true for everything the token is
  *asked to do*; option B describes the datum itself; option C alone would
  strand the resolver). Current correctness law does NOT assume
  stackfulness anywhere in the token's use (option C is false).

Proof sketch: compile-level — every Fiber-typed use is at publication tails
or primitive owner fields; every authority function's correctness argument
in this report never invokes fiber state. Runtime-level —
`register_test_deadline_locked` (scheduler_timer.cpp:534-558) already
registers epochs with `fiber = nullptr` and resolves them through the full
pump path: the terminal/deadline machinery operates on null tokens today.

## SCHEDULER DEPENDENCY QUESTION

Frontend-neutral ≠ Scheduler-independent. Legitimately Scheduler-Core
(survives multiple frontends sharing one Scheduler): the G semantic critical
section; deadline authority (heap/pool/pump/already-due closure);
cancellation serialization; admission atomicity; MW classification and
wait-epoch liveness accounting; teardown preconditions. Scheduler stackful
machinery (separable): Worker pool, Fiber routing, steal, idle-dance, park
domains, `suspend_switch_pending`, context-switch bridges. A stackless
frontend does NOT imply a second scheduler — it implies a second suspension
mechanism behind `commit_suspend_locked` and a second delivery mechanism
behind `publish_waiting_fiber_runnable_locked`/`route_runnable_locked`.

## AC-2d CONNECTION

R2's finding: final terminal rechecks are mostly non-load-bearing because
the lock topology closes the resolver window (every admit seam's comment:
"it cannot be... every resolver takes global_mtx_" — e.g.
scheduler_park_wake.cpp:1169-1172; the rechecks are defense-in-depth).

Would the proof survive a future frontend? The exact required atomicity
contract, stated frontend-free:

> From the instant the epoch becomes resolver-visible (registration) to the
> instant suspension commitment is visible, the suspender must exclude every
> resolver from deciding the epoch. Equivalently: {register, precedence
> evaluation, rechecks, commitment} is ONE transition in the resolver
> partial order.

Today this is achieved by holding G (plus role locks) across the whole span
and committing inside it. If a future frontend shares the SAME Scheduler
semantic critical section (acquires G for its await_suspend equivalent), the
proof survives verbatim. If frontend adaptation inserts a gap — semantic
admission under G, then suspension commitment later without re-entering the
resolver-excluded domain (e.g., an await_suspend that returns to machinery
which suspends asynchronously) — the closed window reopens: a resolver could
win between admission and commitment, publish to a continuation that never
suspended, and the inline-resolution law (no publication for a never-
suspended caller) becomes unenforceable. FE-1a records the contract only;
no fix is proposed and none is needed for a shared-critical-section
frontend.

## DST CONNECTION

DST-PV-1 (PR #241) selects, among already-legal continuations, which
runnable ticket a worker executes next. Its decision vocabulary today is
Fiber-typed (`Run(F1)` — the seam sits at the worker-loop next-runnable
choice, inside the EM routing cluster). The deeper semantic decision —
"execute runnable continuation X next" — is continuation-neutral. Because
the seam hooks the EM layer only, a second frontend publishing through the
same Scheduler would need the test seam re-typed/extended to observe its
tickets (test-infrastructure F2), but no correctness law of DST's semantics
depends on Fiber identity. Recorded migration pressure: low, test-only, no
production coupling created by the current design. DST not modified.

## FRONTEND LEAKAGE FINDINGS (§24/§25/§26)

Falsification attempts (§25) and their outcomes:
1. "WaitNode resolution requires Fiber state transition" — REFUTED: resolve_
   CAS never touches Fiber; `register_test_deadline_locked` runs the whole
   terminal/deadline path with null tokens.
2. "Suspend permission only makes sense because the Fiber is bound to
   g_worker" — REFUTED: g_worker is only the token/admission-context source;
   the permission fact is the resolver-excluded CS.
3. "Primitive correctness depends on Worker ownership" — REFUTED: all
   correctness ordering completes before `route_runnable_locked`; routing
   targets are locality/topology (owner preferred, round-robin fallback,
   pending_spawn_ preservation all preserve correctness).
4. "TimerRegistration lifetime assumes stack frame" — REFUTED:
   independently-stable by explicit design (TimerLifetimeClosure).
5. "Cancellation needs a second authority for a new frontend" — REFUTED:
   the closure is token-free and explicitly documented frontend-neutral.

True findings:

**F0 — naming/comment-only leakage (no action required; on-touch renames):**
- `waiting_waitq_count_` comment "Count of fibers suspended on a WaitQueue"
  (scheduler.hpp:1491-1494) — counts wait epochs, not fibers.
- `WorkerState::inbox_mtx` "historical name" note (scheduler.hpp:173-177).
- WaitNode banner "constructed in the await frame of the waiting
  fiber/task" (wait_node.hpp:53) — already half-neutralized.
- Various "Fiber-suspending substrate" phrasings in primitive seam banners
  (async_mutex.hpp ~25 Fiber mentions, condition.hpp 12, async_rwlock.hpp 14
  — descriptive, not contractual).

**F1 — representation coupling (authority reusable; type adapter required):**
- `WaitNode::fiber_` + `WaitQueue::register_wait_locked(node, Fiber*)`
  signature hard-types the admission token (wait_node.hpp:252,
  wait_queue.hpp:185).
- `AsyncMutex::owner_` / RwLock `writer_owner` are `Fiber*&` — the ownership
  identity type (scheduler.hpp:680-732, 988-1024).
- `WaitReg`/`WaitRecord` store `Fiber* fiber; WorkerState* owner`
  (scheduler.hpp:1177-1180, 1203-1212).
- Publication seam `publish_waiting_fiber_runnable_locked(Fiber*)` and every
  winner tail's `Fiber* f = won->fiber()` (six primitive seams + wake/
  cancel/expire/pump paths).
- `QueueWaitCtx`/grants bind stack result storage by raw pointer — the
  mechanism is frame-typed even though the rule is not.

**F2 — mechanism coupling (narrow seam required; no semantic redesign):**
- Physical suspension cluster: `commit_suspend_locked` +
  `suspend_switch_pending` + `context_switch` + `g_worker` capture —
  centralized in ONE function; the stack-safety half of the pending flag is
  stackful-only.
- Delivery cluster: `route_runnable_locked` + inboxes + steal + idle-dance +
  wake epoch — centralized in ONE function.
- Exactly-once publication guard homed in `FiberState` (fiber.cpp:6-22) —
  a second frontend must home an equivalent guard in its own suspension
  state (or the epoch).
- `granted_not_resumed_` resume-side decrement sites assume the winner
  returns through `context_switch` (queue admit tails).
- DST seam typed at the Fiber-level EM layer (test-only).

**F3 — semantic authority coupling: NONE FOUND.** No correctness law
required duplicating or re-deriving terminal/deadline/cancel/admission
authority for a second frontend.

MOST IMPORTANT LEAKAGE: the F1 admission-token typing
(`register_wait_locked(node, Fiber*)` + `node.fiber()` publication tails),
because it is the single point through which all six primitive seams and
both centralization points touch the frontend type; parameterizing that one
edge (plus the Mutex/RwLock owner identity) collapses most of the F1 list.

## FRONTEND-NEUTRAL CONTRACT (§27, corrected from source)

A wait epoch must satisfy:

1. Registration establishes exactly one live epoch: a single-shot transition
   from the initial state; a registered or terminal epoch rejects
   re-registration without mutation.
2. At admission — atomically w.r.t. every resolver — the epoch binds one
   opaque continuation identity (resolver-discoverable) and one unresolved-
   epoch accounting unit, and evaluates the primitive-defined outcome
   precedence (resource-available / already-due deadline / closed-state /
   suspension) under one serialized admission authority.
3. Exactly one terminal outcome wins the epoch: one atomic terminal
   transition; every loser performs no second wake, unlink, publication, or
   accounting mutation.
4. The winner, within one resolver-excluded critical section, completes in
   order: structural removal (exactly once), side-state retirement (timer
   exactly once, state-gated before any waiter dereference), primitive-
   defined resource commit/reconciliation, and epoch-accounting closure —
   all BEFORE publication.
5. An admission-time (inline) resolution of a caller that never suspended
   publishes nothing; the caller consumes the outcome inline.
6. Suspension may be committed only while the epoch remains eligible, and
   commitment must be atomic with registration w.r.t. the resolver set.
7. A resource-changing winner performs the primitive's required
   reconciliation before returning (opposite-role grant, head advance, or
   the waiter-owned reacquire obligation as the primitive defines).
8. Publication of the continuation occurs only after winner, resource, and
   accounting state are fully committed; occurs at most once; and only when
   the continuation is actually suspended.
9. Timer authority is independent of waiter storage: expiry gates on timer
   state before dereferencing waiter state; waiter destruction after
   resolution is always safe.
10. Teardown of a primitive/Scheduler domain requires zero linked epochs,
    zero ACTIVE timers, and zero published-but-unconsumed winners.
11. The physical suspension/resumption mechanism is frontend-specific and
    appears in no rule above.

(§28 check: the contract names no Fiber, FiberState, worker id, owner
Worker, context_switch, runnable deque, steal, sched_ctx, or stack. The only
admitted frontend-ish term is "continuation identity", which §16 proved is
the semantic minimum — an opaque datum, not a stackful mechanism. §29 check:
Queue FIFO/lease/close dispositions, RwLock fairness/batching, Semaphore
permit transfer, Mutex ownership handoff, Condition mutex choreography, and
Event level/broadcast behavior appear only as "primitive-defined" — the
contract does not absorb them.)

## PRIMITIVE-POLICY BOUNDARY (PP — stays with primitives)

Queue FIFO role policy and ring/lease/close dispositions; RwLock writer-
fairness and reader-batch boundary; Semaphore permit transfer/store/overflow;
Mutex FIFO ownership handoff (owner-before-publication is the FN order, the
ownership policy is PP); Condition release/reacquire choreography and
deadline-governs-Condition-epoch-only; Event set/reset epoch isolation and
readiness-over-deadline precedence. The FN contract states only
"outcome precedence is primitive-defined under one serialized admission
authority".

## EXECUTION-MECHANISM BOUNDARY (EM — replaceable per frontend)

context_switch + stack management; FiberState machine and its home for the
exactly-once guards; g_worker TLS and admission-context capture;
WorkerState/local_runnable/inbox/steal/round-robin targeting; idle-dance,
wake epoch, park domains, MW-S2 backend bridge; suspend_switch_pending's
stack-safety role; granted_not_resumed_'s resume-side site; DST seam typing
(test layer).

## MINIMUM FUTURE FRONTEND RESPONSIBILITIES (§30)

A second frontend's adapter MUST own:
1. Providing the continuation identity bound at admission (the token the
   winner's publication tail delivers);
2. Committing suspension inside the admission critical section — after the
   closure authorizes it — including whatever "suspended" state the
   publication guard consults (the at-most-once anchor);
3. Receiving exactly-once publication at the existing publication source
   point (post commit/reconcile/accounting);
4. Scheduling/resuming the continuation (the delivery topology).

The adapter MUST NOT own: the terminal winner; the deadline winner or timer
retirement; the cancellation winner or membership validation; primitive
resource policy or reconciliation; admission precedence; wait-epoch
accounting; teardown balance. If an adapter needed any of these, frontend
neutrality would have failed — none is required by the current seam shape.

Performance note (§32, inspection only): the semantic information the
adapter must carry is {token, suspended-guard} — two data items an awaiter
frame already has. Nothing in the audit found a mechanically necessary
allocation, virtual dispatch, extra atomic, or extra mutex in the adapter
path; the current seams are plain functions under an existing lock, and a
frontend can bind the same values without new indirection. No cost should be
baked into FE-1b by assumption.

## SEMANTIC REUSE VERDICT (§31A)

YES — a second frontend can reuse every R2 semantic authority unchanged:
terminal winner (resolve CAS), registration contract, deadline lifecycle
(AC-2b four-phase + closures), cancellation closure (membership-gated),
admission precedence + inline-resolution law, suspension-permission
atomicity (shared G critical section), winner-before-publication,
exactly-once retirement/publication obligations, reconciliation obligations,
teardown invariants. The reuse requires no re-derivation, no second
Scheduler, and no duplication of any authority.

## TYPE REUSE VERDICT (§31B)

- WaitNode: PARTIAL — state machine, user_ slot, and lifecycle reusable
  verbatim; the `Fiber* fiber_` member wants token parameterization (F1).
- WaitQueue: NEAR-FULL — structure, resolvers, Unlink Law reusable; only the
  `register_wait_locked` token parameter is Fiber-typed.
- TimerRegistration: FULL — already decoupled from waiter storage; reusable
  unchanged.
- Primitive state: PARTIAL — counts/queues/ring reusable; Mutex/RwLock
  owner-identity fields are `Fiber*`-typed (F1).
- Not required for success: type reuse is not a precondition of semantic
  reuse (§31); the gaps are mechanical parameterizations.

## FE-1b RECOMMENDATION

ENTRY CONDITION (§35): SATISFIED — no F3; F1 accepted; F2 seams are narrow
(one suspension-commit function, one publication function) and duplicate no
R2 authority.

RECOMMENDED FE-1b SCOPE (design statement only, no implementation):
1. Define the continuation-identity edge: what type/parameter satisfies
   `register_wait_locked`'s token, the publication tails, and the
   Mutex/RwLock owner identity for both current and future frontends
   (smallest viable statement; likely "opaque handle + suspended-guard").
2. Write the suspend-commitment contract as a portability rule: commitment
   inside the resolver-excluded CS (the AC-2d clause above) as the
   requirement any frontend awaiter must meet.
3. Specify the publication-delivery seam at the existing two centralization
   points (`commit_suspend_locked`, `publish_waiting_fiber_runnable_locked`/
   `route_runnable_locked`) without changing their signatures yet.
4. Inventory the F1 typing as the mechanical-change checklist for any future
   frontend PR.

DO NOT: implement co_await/awaiter/promise, sender/receiver, a generic
continuation framework, a second Scheduler, or modify WaitNode/WaitQueue/
Scheduler production semantics.

## §37 — IF FIBER DISAPPEARED TOMORROW

Correctness laws that survive unchanged (mechanically supported above):
the ONE-WINNER resolve-CAS terminal law; the Unlink Law (CAS+unlink same
CS); the deadline lifecycle (prepare/publish/consume/retire, already-due
closure, timer-lifetime closure); the cancellation closure (membership gate
→ CAS → retire); admission precedence and the inline no-publication law;
registration-to-commitment atomicity (provided the replacement commits
inside the resolver-excluded CS); winner-before-publication; exactly-once
publication/retirement/reconciliation; teardown balance. Laws that die with
Fiber are exactly the EM cluster: stack switching, FiberState guards' home,
worker routing, steal, idle-dance, stack-safety flag, resume-side decrement
sites. None of the dying items is a semantic authority. That is the FE-1a
success criterion, met with representation coupling.
