# E12-D AsyncCondition — C++ Implementation Plan 1 (PLAN-ONLY)

**Task:** `E12-D-CONDITION-CPP-IMPLEMENTATION-PLAN-ONLY-1`
**Mode:** PLAN-ONLY (audit / read / map / analyze / plan). No production code, test code,
existing doc, file create/delete/format, worktree-mutating command, or commit performed.
**Author:** independent adversarial planner
**Date:** 2026-07-15
**Branch:** `feat/e12-D-condition-formal`
**HEAD:** `45015855f6c7283ce1b576d1d65ebe6a97315078`

---

## A. Verdict

```text
E12-D-CONDITION-CPP-IMPLEMENTATION-PLAN-ONLY-1:
READY-FOR-APPROVAL
```

Rationale: the closed TLA+ formal gate, the frozen preparation spec (`docs/e12-condition.md`,
policy register C-H1..C-H10 CLOSED, frozen public API, T0–T32 test list), and the existing
E12-C substrate together provide a complete, code-evidenced implementation frontier. The one
load-bearing missing internal seam — a caller-holds-`global_mtx_` variant of the Mutex
handoff — is an IMPLEMENTATION BOUNDARY already identified by the preparation audit (§6.2
"audit interaction with `mutex_handoff_one_locked`") and is mechanically small (the existing
`mutex_handoff_one_locked` already requires `global_mtx_` held and only takes `waiters_.mtx()`
internally). No P0/P1 finding blocks the start of construction. The plan is frozen below and
awaits external approval; no implementation is performed in this task.

---

## B. Repository state

| Item | Value |
|------|-------|
| Branch | `feat/e12-D-condition-formal` |
| HEAD | `45015855f6c7283ce1b576d1d65ebe6a97315078` |
| Worktree | `/home/hoo/Projects/io-core`, clean (`git status --short` empty) |
| Formal commits present | `8202776` (fix C6; positive 24 invariants pass; NegC6 pierces `InvFIFOGrant`) and `4501585` (extend `OrdinaryEpochs`; reach1/reach2 CEX; NegC9/NegC10 Mark guards; C9/C10 named-property CEX) |
| Formal gate script | `scripts/verify-e12-async-condition-formal.sh` exists, executable, safety-only (no liveness run) |
| Formal gate status (declared) | gate 0-ed: positive PASS; reach1/reach2 expected CEX; NEG-C1..C10 each violate its one named property; WRONG-PROPERTY gate PASS |
| AsyncCondition C++ code | **none** — `grep -rn 'AsyncCondition|class Condition' include/ src/ tests/` returns zero matches (clean slate) |
| `e12_async_condition_test.cpp` | does not exist (to be created) |
| `e12_async_condition_authority_probe.cpp` | does not exist (to be created) |
| Existing related tests (status: pass on `8202776`/`4501585` per prep audit) | `tests/e12_async_mutex_test.cpp`, `tests/e12_event_test.cpp`, `tests/e12_semaphore_test.cpp` |

**Note on formal gate reproduction:** the verify script defaults `TLA2TOOLS_JAR` to
`/tmp/tla2tools.jar` (not vendored; fetch URL in the script header) and requires `java` on
PATH. This PLAN-ONLY task did **not** re-execute the formal gate (doing so would fetch a
remote jar — an outward network action not authorized for a read-only plan task). The
"gate 0-ed" status is taken from the commit messages of `8202776` and `4501585`, which are
the declared final state of the formal frontier. Re-running
`bash scripts/verify-e12-async-condition-formal.sh` is the first step of the subsequent
construction task, not of this plan.

---

## C. Authority map

All evidence is by direct file read. `scheduler.cpp` line numbers refer to
`/home/hoo/Projects/io-core/src/async/scheduler.cpp`; header line numbers to the
`include/sluice/async/*.hpp` files.

| Concern | Authoritative file/type/function | Evidence | Planned reuse |
|---------|----------------------------------|----------|---------------|
| WaitNode (one-shot lifecycle, resolve CAS, single winner) | `include/sluice/async/wait_node.hpp` `class WaitNode` (`register_`, `resolve_`, `is_registered`, `outcome`, `fiber()`); states Detached→Registered→{Woken,Cancelled,Expired} | wait_node.hpp:81-239 | Condition node + reacquire node are BOTH ordinary `WaitNode`s. Two distinct instances per `wait()` call (C-H7): caller-supplied condition node + stack-local reacquire node. No node type change. |
| WaitQueue (intrusive FIFO, sealed, Scheduler-only friend) | `include/sluice/async/wait_queue.hpp` `class WaitQueue` (`register_wait_locked`, `wake_one_locked`, `wake_node_locked`, `cancel_locked`, `expire_locked`, `contains_locked`, `unlink_locked`, `mtx()`) | wait_queue.hpp:119-322 | AsyncCondition owns a **private `WaitQueue waiters_`** (the Condition queue), exactly as Event/Semaphore/AsyncMutex do. Reuse `register_wait_locked`/`wake_one_locked`/`contains_locked`/`cancel_locked` unchanged. |
| Scheduler coordination domain (lock order) | `Scheduler::global_mtx_` (`Mutex`, GUARDED_BY authority); queue mtx taken UNDER it | scheduler.hpp:700; every mutating seam takes `global_mtx_` then `q.mtx()` | The CONDITION-WAIT-PREPARE combined seam holds `global_mtx_` throughout; Condition queue mtx and Mutex queue mtx are taken **sequentially**, never simultaneously (prep §6.3). |
| Current-fiber acquisition | `g_worker` (TLS, `thread_local WorkerState*`, scheduler.cpp:36) → `ws->current` (`WorkerState::current`, scheduler.hpp:143) | scheduler.cpp:36, 1042-1043, 1884-1886 | Condition `wait()` captures `me = ws->current` exactly as `await_wait`/`mutex_lock` do. The condition node is registered with `me` as its fiber handle. |
| Suspension (make_waiting + context_switch) | `Fiber::make_waiting()` (fiber.hpp:97, lawful running→waiting); `fiber_ctx::Switch` + `context_switch` (scheduler.cpp:1070-1073) | fiber.hpp:96-97; scheduler.cpp:1068-1073 | Condition `wait()` suspends via the SAME mechanism. The switch-back target is the calling fiber's `me->ctx`; it resumes after `context_switch` returns, identical to `await_wait`/`mutex_lock`. |
| Runnable publication (exactly-once) | `Fiber::make_runnable()` (fiber.cpp:6-22, CAS created→runnable OR waiting→runnable, returns true only on real transition); `route_runnable_locked` (scheduler.cpp:906-932, push to `local_runnable`/`pending_spawn_` + `signal_wake_locked`) | fiber.cpp:6-22; scheduler.cpp:906-932, 1093-1101 | Condition-epoch publication (notify resolves the condition node → `make_runnable`+`route`) and reacquire-epoch publication (mutex handoff → `make_runnable`+`route`) BOTH reuse this unchanged. Two distinct publications per `wait()` call are lawful (prep §13.2: "Fiber may be published once for the Condition resolution and once for the Mutex reacquire"). |
| Resume entry (no synchronous re-entry) | `run_next_on` (scheduler.cpp:826-837) is the SOLE resume site; `route_runnable_locked` only enqueues + notifies, never switches context | scheduler.cpp:826-837, 906-932 | Confirms the woken fiber NEVER runs inline within the notifier's/unlocker's critical section. This is what makes "register condition node + handoff mutex + make_waiting" safe in one CS: the handoff winner is merely enqueued, not executed. |
| Mutex direct handoff (MUTEX-HANDOFF-ONE, owner-before-publication) | `Scheduler::mutex_handoff_one_locked(WaitQueue& waiters, Fiber*& owner)` (scheduler.cpp:2056-2097). Source order: `wake_one_locked` (resolve+unlink) → `owner = f` (commit, line 2079) → [test seam 2080-2088] → `retire_timer_for_node_locked` (2089) → `--waiting_waitq_count_` (2090) → `make_runnable`+`route_runnable_locked` (2093-2094). `SLUICE_REQUIRES(global_mtx_)`; takes `waiters.mtx()` internally at 2072. | scheduler.hpp:558-559; scheduler.cpp:2056-2097 | This is the seam the CONDITION-WAIT-PREPARE combined step reuses to release the bound Mutex. Owner commit BEFORE publication is already load-bearing here. |
| Mutex unlock (public wrapper) | `Scheduler::mutex_unlock(WaitQueue& waiters, Fiber*& owner)` (scheduler.cpp:2099-2127): takes `global_mtx_` (2115), asserts `owner==me` (2116-2117), calls `mutex_handoff_one_locked` (2122) or sets `owner=nullptr` (2126) | scheduler.cpp:2099-2127 | AsyncCondition's wait-prepare does NOT call `mutex_unlock` (it must register the condition node first, under the same `global_mtx_`). It calls a **caller-holds-`global_mtx_` handoff seam** (see Gap G-1). |
| Mutex admission / reacquire path | `Scheduler::mutex_lock(WaitQueue& waiters, Fiber*& owner, WaitNode& node)` (scheduler.cpp:1863-1937). Admission recheck at 1911-1921: `node.prev_==nullptr && owner==nullptr` → `wake_node_locked` (inline Woken) → `owner=me` → `--waiting_waitq_count_` → `make_runnable` (discarded, fiber running) → return (no suspend). Else `make_waiting` + `context_switch`. | scheduler.cpp:1863-1937 | **The reacquire epoch reuses `mutex_lock` verbatim** with a fresh stack-local `WaitNode` (C-H3, C-H7). No new reacquire code path; ordinary FIFO-tail admission (C-H8) is already this. |
| Mutex try_lock (no-barging) | `Scheduler::mutex_try_lock` (scheduler.cpp:1835-1861): `owner==nullptr && waiters.empty_locked()` | scheduler.cpp:1835-1861 | Unchanged. No-barging for ordinary contenders vs reacquire waiters is already enforced by the empty-queue check. |
| Mutex cancel (queue-identity gate) | `Scheduler::mutex_cancel` (scheduler.cpp:2026-2054): `contains_locked` membership scan before `cancel_locked` | scheduler.cpp:2026-2054 | Reacquire epoch is non-cancellable (C-H5): no `cancel` is offered on the reacquire node. The condition-node cancel is a NEW condition-specific seam (see G-2). |
| Event notify_all precedent (snapshot/drain) | `Scheduler::event_set_broadcast(WaitQueue& waiters, std::atomic<bool>& set_flag)` (scheduler.cpp:1299-1337): store SET (1320) then `while (wake_wait_one_locked(waiters) != nullptr) ++woken;` (1333-1335), all under `global_mtx_` held continuously | scheduler.cpp:1299-1337 | AsyncCondition `notify_all` follows this exact pattern: under `global_mtx_`, loop `wake_wait_one_locked(condition_queue)` until empty. No separate snapshot structure is needed — the continuous `global_mtx_` hold IS the atomic snapshot (late waiters cannot register because admission also needs `global_mtx_`). |
| Scheduler timer / deadline (E11) | `Scheduler::await_wait_deadline`, `expire_wait`, `TimerRegistration`, `retire_timer_for_node_locked`, `register_test_deadline_locked` | scheduler.hpp:264-314, 917-944; scheduler.cpp:1939-2024 | `wait_until` registers a timer for the **Condition epoch only** (C-H4). Reuses E11 registration/retirement unchanged. A non-timer winner (notify/cancel) retires the timer in the same CS as the resolve CAS (E11 I4). |
| Test controller (deterministic seam) | `tests/async_test_control.hpp` facades (`E7AdmissionSeam`, `E9ParkSeam`, `E11TimerControl`, `E12EventSeam`, `E12MutexSeam`); `tests/async_test_control_internal.hpp` (`PhaseTag`, `PhaseState`, `SchedulerController`); `tests/async_test_control.cpp` (`test_phase` blocks worker while production lock held) | async_test_control_internal.hpp:38-106; async_test_control.cpp:49-73 | Add new `PhaseTag` values + `E12ConditionSeam` facade + `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` call sites in scheduler.cpp (mirrors `e12_mutex_handoff_before_publication` at 2080-2087 and `e12_set_store_before_drain` at 1329-1332). |
| Cancellation/deadline frontier (E12-G) | E12-G not started; no cross-primitive cancellation policy exists | (absence) | E12-D implements ONLY: condition-node cancel (queue-identity gate, mirroring `event_cancel_wait`), condition-epoch deadline (E11 reuse). Reacquire is untimed + non-cancellable (C-H4/C-H5). Cross-primitive cancel propagation is E12-G, out of scope. |

---

## D. TLA+ → C++ mapping

Formal model: `docs/spec/e12_async_condition/E12AsyncCondition.tla`. Nomenclature: the brief's
names map to actual model actions as shown. Items marked **FORMAL-ONLY** must NOT be copied
to C++ (production supports dynamic waiter counts; the fixed epoch slots exist only to bound
TLC's state space).

| Formal concept/action | Required runtime meaning | Existing C++ seam | Gap |
|-----------------------|--------------------------|--------------------|-----|
| `mutexOwner: Fiber ∪ {NoOwner}` | AsyncMutex `owner_` (`Fiber*`, nullptr=NoOwner) | `AsyncMutex::owner_` (async_mutex.hpp:209) | none |
| `conditionQueue: Seq(ConditionEpoch)` | AsyncCondition's private `WaitQueue waiters_` (intrusive FIFO) | `WaitQueue` (wait_queue.hpp:119) | none |
| `mutexQueue: Seq(MutexEpochs)` — **unified** (Reacquire ∪ Ordinary, one queue) | AsyncMutex's private `WaitQueue waiters_` — reacquire nodes and ordinary lock nodes share it | `AsyncMutex::waiters_` (async_mutex.hpp:210); `mutex_lock` appends via `register_wait_locked` | none — the reacquire node is an ordinary `WaitNode` registered into the SAME Mutex queue via the SAME `mutex_lock` path |
| `conditionNodeState` / `mutexNodeState` | `WaitNode::state_` atomic (Detached/Registered/Woken/Cancelled/Expired) | wait_node.hpp:190-238 | none |
| `waitPhase` (Idle/ConditionWaiting/ConditionResolved/ReacquirePending/MutexWaiting/Returned) | implicit in control flow + `WaitNode` terminal state; no public phase field | (control flow) | none — NOT a runtime enum to copy; it is a proof abstraction |
| `WaitAdmissionSuspend` (= CONDITION-WAIT-PREPARE) | one atomic `global_mtx_` CS: register condition node (+ optional timer) → release/handoff Mutex → `make_waiting` → release `global_mtx_` → `context_switch` | `mutex_handoff_one_locked` (handoff half); `register_wait_locked` (register half); `make_waiting`+`context_switch` (suspend half) | **G-1**: need a combined Scheduler seam that holds `global_mtx_` across register+handoff+suspend (no existing single method does all three). The handoff helper must NOT re-take a Mutex queue lock already held by the caller (prep §6.2). |
| `WaitDueInline` (deadline already due at admission → Expired inline, keep Mutex, skip reacquire) | `wait_until` admission: if deadline already due, resolve condition node `Expired` inline WITHOUT releasing Mutex or suspending; return Expired | E11 I5 admission-closure pattern (`mutex_lock_until` 1979-2006) | **G-3**: condition-specific admission ordering — resource/notify not applicable at admission (the resource IS releasing the mutex); the only admission predicates are "already-due deadline" vs "register+release+suspend". Straightforward. |
| `NotifyOne` (resolve eligible FIFO condition head Woken, dequeue, no Mutex token) | `notify_one()`: under `global_mtx_`+cond q.mtx(), `wake_one_locked` the condition queue → `retire_timer_for_node_locked` → `--waiting_waitq_count_` → `make_runnable`+`route`. No Mutex mutation. | `wake_wait_one_locked` (scheduler.cpp:1076-1118) — used by Event/Semaphore/Mutex | **G-2 (minor)**: notify_one resolves the condition node but must NOT route it as "mutex granted". The existing `wake_wait_one_locked` resolves Woken + routes runnable — which is exactly right: the fiber wakes, runs its reacquire body, calls `mutex_.lock(reacquire_node)`. No Mutex grant happens at notify time. ✓ |
| `NotifyAll` (snapshot+drain, each once, order preserved, late excluded) | `notify_all()`: under `global_mtx_`, loop `wake_wait_one_locked(condition_queue)` until nullptr | `event_set_broadcast` drain loop (scheduler.cpp:1333-1335) | none — identical pattern. The continuous `global_mtx_` hold IS the atomic snapshot; late waiters excluded because admission needs `global_mtx_`. |
| `CancelCondition` (resolve one Registered+queued condition epoch Cancelled) | `AsyncCondition::cancel(node)`: queue-identity gate (`contains_locked`) + `cancel_locked` + retire timer + route | `event_cancel_wait` / `sem_cancel` / `mutex_cancel` (all identical shape) | **G-2**: new `Scheduler::condition_cancel_wait` (or equivalent private seam) mirroring `event_cancel_wait`. Mechanical. |
| `ExpireCondition` (timer fires on condition node → Expired) | E11 timer expiry on the condition node → `expire_wait` | `expire_wait` (scheduler.hpp:314) | none — reuses E11 expiry unchanged. The condition node is registered into the condition `WaitQueue`, and the `TimerRegistration` binds {node, condition_queue}; `expire_wait` resolves it Expired. |
| `BeginReacquire` + `ReacquireImmediate` + `ReacquireSuspend` | after condition node resolves, the fiber (now NOT owning Mutex) creates a stack-local `WaitNode reacquire_node` and calls `mutex_.lock(reacquire_node)` | `mutex_lock` (scheduler.cpp:1863-1937) — admission recheck (ReacquireImmediate) or register+suspend (ReacquireSuspend) | none — the reacquire epoch is LITERALLY `mutex_.lock(reacquire_node)`. No new code. |
| `OrdinaryLockImmediate` / `OrdinaryLockSuspend` | an ordinary `mutex_.lock()` caller (not a condition waiter) | `mutex_lock` / `mutex_try_lock` | none — unchanged. Mixed ordering (C9/C10) falls out for free because reacquire nodes and ordinary nodes enter the SAME `mutexQueue` via the SAME `register_wait_locked` tail-append. |
| `MutexUnlockHandoff` (direct handoff, owner-before-publication, kind-agnostic FIFO) | `mutex_handoff_one_locked` — grants `MutexFIFOHead` regardless of epoch kind | scheduler.cpp:2056-2097 | none — `wake_one_locked` resolves the FIFO head by structural position, NOT by node kind. Unified FIFO (C-H8) is already the behavior. |
| `ReturnOwned` (wait returns only when `mutexOwner == current Fiber`) | `wait()` returns after `mutex_.lock(reacquire_node)` returns; the latched condition reason is returned | control flow | none |
| `Destroy` (gated by `destructionSafe`: empty condition queue, no active wait) | `~AsyncCondition()`: debug-assert no registered condition waiters + no in-flight `wait()` call | `~WaitQueue` assert (wait_queue.hpp:135); `active_waits_` counter (prep §14) | **G-4**: add `active_waits_` counter (or equivalent) to catch the reacquire-phase destruction case (a reacquire node lives in the Mutex queue, not the Condition queue, so the Condition queue being empty is insufficient). IMPLEMENTATION BOUNDARY per prep §14. |
| **FORMAL-ONLY** `OrdinaryEpochs={O1,O2,O3}`, `ReacquireEpochs={R1,R2,R3}`, `ConditionEpochs={C1,C2,C3}` | DO NOT copy — fixed slots bound TLC's state space | — | production uses dynamic per-call `WaitNode`s |
| **FORMAL-ONLY** `Mark` macro, `lastAction`, `lastActor`, `lastTargetEpoch`, `lastGrantedEpoch`, `pre*` snapshots, `expectedFIFOHead`, `notifyAllSnapshot`, `registrationCommitted`, `conditionReason` (as a map), resolution/publication counters | DO NOT copy — proof instrumentation to express safety as state invariants | — | production has no such fields; safety is enforced by the resolve CAS + control flow |
| **FORMAL-ONLY** NEG gate fault-injection fields (e.g. NegC6 `expectedFIFOHead':=R2`, NegC9 `w:=R1`, NegC10 separate reacquire-only queue) | DO NOT copy — these are the *defects* the neg gates prove are caught | — | — |

---

## E. Findings

All findings cite file + line. Severity: P0 (architecture cannot safely implement), P1 (must
fix before implementation), P2 (handle during implementation), P3 (record for later frontier).

### F-E1 — P2: CONDITION-WAIT-PREPARE needs a caller-holds-`global_mtx_` handoff seam (Gap G-1)

The preparation spec (§6.2) already identifies this: `mutex_handoff_one_locked` acquires
`waiters_.mtx()` internally (scheduler.cpp:2072), so the combined seam must not already hold
the Mutex queue lock. Two viable shapes (compared in §F):
- (a) call the existing `mutex_handoff_one_locked` directly from the combined seam (it takes
  `waiters_.mtx()` itself; the caller holds only `global_mtx_` + the Condition queue mtx,
  which is a DIFFERENT mutex — no self-deadlock), OR
- (b) add a `_locked` variant that assumes the caller already holds `waiters_.mtx()`.

Evidence the existing helper is already safe to call from the combined seam:
`mutex_handoff_one_locked` is `SLUICE_REQUIRES(global_mtx_)` (scheduler.hpp:558-559), takes
ONLY `waiters_.mtx()` internally (scheduler.cpp:2072), and does NOT re-acquire `global_mtx_`.
The combined seam holds `global_mtx_` + (transiently) the **Condition** queue mtx — a
different `Mutex` object from the Mutex's `waiters_.mtx()`. Acquiring the Mutex's
`waiters_.mtx()` under `global_mtx_` is the canonical lock order (global_mtx_ → q.mtx()),
already used everywhere. **Conclusion: option (a) — call `mutex_handoff_one_locked` directly
— needs NO new Mutex-side seam at all.** The prep audit's "narrow caller-lock-held variant
may be required" is a conservative note; the code evidence shows the existing helper is
already caller-`global_mtx_`-held and internally takes only the Mutex queue mtx, which the
combined seam does NOT hold. No Mutex modification is required for correctness. (A `_locked`
variant is a minor stylistic option, not a necessity — see §F.)

### F-E2 — P2: Condition-node cancel needs a new queue-identity-gated Scheduler seam (Gap G-2)

`event_cancel_wait` / `sem_cancel` / `mutex_cancel` are per-primitive seams (scheduler.cpp
1370-1390, 1752-1779, 2026-2054) that all share the identical shape: `global_mtx_`+`q.mtx()`
→ `contains_locked` → `cancel_locked` → `retire_timer_for_node_locked` →
`--waiting_waitq_count_` → `make_runnable`+`route`. AsyncCondition needs an equivalent
`condition_cancel_wait(WaitQueue& waiters, WaitNode& node)`. This is mechanical duplication of
a 4-line body. Alternatively a single private helper could be factored, but that would touch
all three existing primitives — out of scope (§H: do not refactor existing primitives). New
dedicated seam is the safe choice.

### F-E3 — P2: `wait_until` admission ordering (Gap G-3)

At `wait_until` admission, the resource-first precedence from E11/E12-A/E12-B/E12-C does NOT
apply in the same form: the "resource" for a condition wait is a notification, which cannot be
observed at admission (notify_one/notify_all need `global_mtx_`, which the admission holds).
The only admission predicate is the already-due deadline (C-H4): if `now >= deadline` at
admission, resolve the condition node `Expired` inline, keep the Mutex, return `Expired`
without suspending or reacquiring (mirrors `WaitDueInline`). This matches `mutex_lock_until`'s
I5 path (scheduler.cpp:1996-2006) and `await_event_wait_deadline`. Straightforward; the only
subtlety is that Expired-at-admission must NOT release the Mutex (the caller already owns it
and `wait()` returns ownership per C-H1 only on the suspend path — but an Expired-at-admission
return is an Expired outcome WITH the Mutex still owned, so the caller retains it; this is
`WaitDueInline`'s `InvDueInlineRetainsOwnership`).

### F-E4 — P2: `active_waits_` lifetime counter for reacquire-phase destruction (Gap G-4)

`~AsyncCondition()`'s `~WaitQueue` asserts the Condition queue is empty (wait_queue.hpp:135),
but a fiber in the reacquire epoch has its reacquire node in the **Mutex's** queue, not the
Condition queue. So a Condition-queue-empty assert would NOT catch destruction-during-
reacquire. Prep §14 classifies the counter as an IMPLEMENTATION BOUNDARY. Recommended: an
`active_waits_` (or `in_flight_wait_calls_`) counter incremented at `wait()`/`wait_until()`
entry and decremented at return, debug-asserted zero in `~AsyncCondition()`. This also covers
the Condition-epoch destruction case redundantly.

### F-E5 — P3: formal verify script has no compile-probe (authority-seal) gate

`scripts/verify-e12-async-condition-formal.sh` is pure TLA+/TLC (no C++ compile probe),
unlike `scripts/verify-e12-async-mutex-formal.sh` which has a `compile_probe_gate`
(210-230) asserting `tests/e12_async_mutex_authority_probe.cpp` fails to compile on sealed
names. To seal the AsyncCondition public API (no `wait_queue()`/`mutex()`/`waiting_count()`/
`notify_n()`/`reacquire_node()` accessor — prep §5 lines 211-219), the construction task
should add `tests/e12_async_condition_authority_probe.cpp` AND extend the verify script with
a compile-probe gate mirroring the mutex one. This is a construction-task gate concern, not a
plan blocker; recorded here so it is not forgotten.

### F-E6 — P3: no automated CI; gate is manual

No `.github/workflows/`, no `.gitlab-ci.yml`, no root `forgejo/workflows` exist for io-core
(only `zig/.forgejo/workflows/ci.yaml`, which is the Zig reference library's CI and does not
build the C++ core). The authoritative gate is the manual command sequence in §K. This does
not block the plan but means gate enforcement is a human discipline.

### F-E7 — P2: TSan high-worker-count sharp edge

Existing E12 tests note (e12_event_test.cpp T23, lines 1328-1332) a known raw-fiber-asm +
TSan DEADLYSIGNAL at high concurrent-worker counts. Condition tests should follow the same
discipline: causal two/three-way proofs run under TSan at low worker counts; high-worker-
count stress avoids TSan or documents the classification. This shapes the test plan (§I): the
deterministic causal proofs (T0–T29) run under ASan+UBSan+TSan; the coordination stress
(T30–T32) runs under ASan+UBSan and TSan only at low concurrency.

**No P0 or P1 findings.** The architecture can safely implement E12-D Condition on the
existing substrate without modifying Mutex semantics, Scheduler coordination, or the Fiber
state machine. The only Scheduler-side additions are the combined wait-prepare seam and the
condition-cancel seam (both private, both mechanical), plus `active_waits_` and test-seam
phases.

---

## F. Candidate comparison

Three candidates are compared against the code evidence. The recommended candidate is marked.

### Candidate A — Condition reuses Mutex's unified waiter queue directly + internal enqueue-reacquire seam

Condition does NOT own a separate `WaitQueue`; instead the condition waiters live in the
Mutex's `waiters_`, tagged as "condition" vs "ordinary/reacquire". notify_one/notify_all
operate on the Mutex queue filtering for condition-tagged heads.

**Evaluation:**
- Fits current architecture? **NO** — violates the frozen public API (prep §5: AsyncCondition
  owns `WaitQueue waiters_`). Violates C-H2/C-H3 separation (the Condition epoch and the
  Mutex epoch are distinct queues in the formal model: `conditionQueue` ≠ `mutexQueue`).
- Modify Mutex? **YES, deeply** — Mutex would need to host two kinds of waiters and expose a
  filter, breaking the sealed authority (async_mutex.hpp:39-50: "private WaitQueue is NOT
  publicly reachable").
- Modify Scheduler? **YES** — handoff would need to skip condition-tagged nodes, contradicting
  the unified FIFO (C-H8) and the formal `MutexFIFOHead` (kind-agnostic).
- Waiter lifetime: a node would live in two roles in one queue — contradicts the formal
  `InvNoDualQueueMembership` and `InvConditionQueueWellFormed`.
- FIFO / no-barging: broken — tagging introduces a priority/class distinction the formal model
  explicitly forbids (C10 negates exactly "separate queues / reacquire-only head").
- Cancellation boundary: condition cancel would have to operate inside the Mutex queue,
  conflating the two epochs.
- Test complexity: high — the seam between condition-notify and mutex-handoff on one queue is
  novel.
- Hot-path cost: filter scan on every handoff.
- E12-A/B/C consistency: **NO** — all three own a private `WaitQueue`; this breaks the pattern.
- Parallel-architecture risk: **HIGH** — this is precisely the "second weaker mutex handoff"
  the brief forbids.
- **Verdict: REJECTED.** It violates the frozen API, the formal model's two-queue structure,
  the sealed Mutex authority, and the C10 negative-gate semantics.

### Candidate B — Condition waiter, on notify, is CONVERTED into a fresh Mutex waiter node, then enters the unified Mutex queue

AsyncCondition owns its private `WaitQueue` (condition queue). On `notify_one`/`notify_all`,
the resolved condition node is unlinked and a NEW `WaitNode` is created and registered into
the Mutex's `waiters_` (the reacquire node), then the fiber is woken to... wait, but the
fiber is suspended; it cannot create the reacquire node until it resumes.

**Evaluation:**
- Fits current architecture? Partially — Condition owns a queue (matches frozen API), but the
  "convert to a Mutex node at notify time" step must happen on the notifier's thread, for a
  fiber that is suspended. The reacquire node would have to be allocated/stored by the
  notifier or pre-allocated by the waiter.
- Modify Mutex? No (good).
- Modify Scheduler? Needs a "register a node on behalf of a suspended fiber" path — but the
  existing `register_wait_locked` records `fiber` from the node; a notifier-registered node
  would need the suspended fiber's handle, which IS recoverable (`won->fiber()`). But then the
  suspended fiber, on resume, would find a reacquire node it did not create — lifetime
  confusion (who owns/destroys it?).
- Waiter lifetime: **fragile** — the reacquire node's lifetime straddles the notify (notifier
  creates/registers) and the resume (waiter consumes). The prep spec (C-H7) explicitly states
  the reacquire node is **stack-local to the active Fiber wait call** — created BY the waiter
  on resume, NOT by the notifier. Candidate B contradicts C-H7.
- FIFO: the reacquire node enters the Mutex queue at notify time, NOT at resume time — so its
  FIFO position reflects the notify instant, not the "fiber actually reached reacquire"
  instant. This subtly differs from C-H8 ("ordinary FIFO-tail Mutex admission" by the waiter's
  own `mutex_.lock()` call). It also means a notified-but-not-yet-resumed fiber occupies a
  Mutex queue slot while suspended on the Condition — violating `InvNoDualQueueMembership`
  (a fiber simultaneously in condition queue and mutex queue — well, it's been removed from
  the condition queue, but it now holds a mutex slot while not yet running reacquire).
- Cancellation: the reacquire node would need to be cancellable-by-Mutex-destroy, but C-H5
  says reacquire is non-cancellable — if the notifier pre-registered it, a Mutex cancel could
  target it.
- Test complexity: high — lifetime ownership of the converted node.
- Hot-path cost: one extra node + register per notify.
- E12-A/B/C consistency: weak — no precedent for notifier-registered-on-behalf-of-waiter.
- Parallel-architecture risk: medium.
- **Verdict: REJECTED.** It contradicts C-H7 (stack-local reacquire node created by the
  waiter) and muddies reacquire FIFO timing and lifetime ownership.

### Candidate C (RECOMMENDED) — Condition owns a private WaitQueue; reacquire is an ordinary stack-local `mutex_.lock()` call by the resumed waiter

AsyncCondition owns its private `WaitQueue waiters_` (the condition queue), exactly matching
the frozen API (prep §5) and the formal `conditionQueue`. `wait()`:
1. (under `global_mtx_`) register the caller-supplied condition node into `waiters_` (+ optional
   timer for `wait_until`);
2. (still under `global_mtx_`) release the bound Mutex via `mutex_handoff_one_locked`
   (direct handoff to the Mutex FIFO head, or `owner_=nullptr` if empty) — owner-before-
   publication already load-bearing in the helper;
3. `make_waiting` + release `global_mtx_` + `context_switch`.
On resume (condition node terminal with reason r ∈ {Woken, Expired, Cancelled}), the fiber
creates a **stack-local** `WaitNode reacquire_node` and calls `mutex_.lock(reacquire_node)`
(C-H3, C-H7, C-H8). `wait()` returns the latched reason r only after `mutex_.lock` returns
(when `mutex_owner == current Fiber`, C-H1). `notify_one`/`notify_all` resolve condition nodes
Woken via the existing `wake_one_locked`/drain pattern; the resolved fiber wakes, runs its
reacquire body, and enters the Mutex queue via the ordinary admission path — so its Mutex FIFO
position is determined by when it actually calls `mutex_.lock`, exactly C-H8.

**Evaluation:**
- Fits current architecture? **YES** — Condition owns a private `WaitQueue`, mirroring
  Event/Semaphore/AsyncMutex. The reacquire is a plain `mutex_.lock()` call, reusing the
  closed E12-C admission path verbatim.
- Modify Mutex? **NO** — Mutex is untouched. The handoff is reused via the existing
  `mutex_handoff_one_locked` (which already requires `global_mtx_` held and takes only the
  Mutex queue mtx internally).
- Modify Scheduler? **Minimal** — add (i) the combined `condition_wait_prepare`-style seam
  (register condition node + handoff mutex + suspend, under `global_mtx_`), (ii)
  `condition_cancel_wait` (queue-identity cancel, mirroring `event_cancel_wait`), (iii)
  `condition_notify_one`/`condition_notify_all` (thin wrappers over `wake_wait_one_locked`/
  drain), (iv) `active_waits_`-style accounting is on AsyncCondition, not Scheduler. All
  private seams.
- Waiter lifetime: **clean** — the condition node is caller-supplied (lives in the caller's
  `wait()` frame); the reacquire node is stack-local to the same frame, created after resume.
  They are NEVER registered simultaneously (the condition node is terminal+unlinked before the
  reacquire node is created). Satisfies `InvNoDualQueueMembership` and C-H7.
- FIFO / no-barging: **correct** — reacquire nodes and ordinary lock nodes enter the SAME
  Mutex `waiters_` via the SAME `register_wait_locked` tail-append; `mutex_handoff_one_locked`
  grants the structural FIFO head regardless of kind. C9/C10 mixed ordering is satisfied by
  construction (no class priority).
- Cancellation/deadline boundary: **clean** — condition-node cancel is queue-identity-gated
  (mirrors Event/Sem/Mutex); condition-epoch deadline reuses E11; reacquire is untimed +
  non-cancellable (C-H4/C-H5) because no cancel/expire is offered on the reacquire node.
- Test complexity: **lowest** — reacquire is just `mutex_.lock()`, already tested by
  `e12_async_mutex_test.cpp`. New tests focus on the condition epoch + the combined seam.
- Hot-path cost: **minimal** — one `WaitNode` register + one handoff call (existing) per
  `wait()`; no extra allocation (reacquire node is stack-local). `route_runnable_locked` and
  `make_runnable` allocate nothing per-call.
- E12-A/B/C consistency: **strongest** — identical ownership/queue/seam pattern.
- Parallel-architecture risk: **NONE** — no second mutex handoff; the Condition uses the ONE
  accepted `mutex_handoff_one_locked`.
- **Verdict: RECOMMENDED.** It is the only candidate that satisfies the frozen API, C-H7
  (stack-local reacquire node), C-H8 (ordinary FIFO-tail admission), the formal two-queue
  model, the sealed Mutex authority, and the "no parallel architecture" constraint, while
  reusing the closed E12-C handoff and admission paths unchanged.

**Recommended: Candidate C.**

---

## G. Frozen implementation proposal

This is a proposal awaiting approval. No code is modified in this task.

### G.1 New / modified internal types

**New header `include/sluice/async/condition.hpp`** — `class AsyncCondition` (frozen public
API, prep §5 lines 177-198):
```cpp
class AsyncCondition {
public:
    explicit AsyncCondition(AsyncMutex& mutex) noexcept;
    ~AsyncCondition();
    AsyncCondition(const AsyncCondition&) = delete;
    AsyncCondition& operator=(const AsyncCondition&) = delete;
    AsyncCondition(AsyncCondition&&) = delete;
    AsyncCondition& operator=(AsyncCondition&&) = delete;

    [[nodiscard]] WaitOutcome wait(WaitNode& condition_node);
    [[nodiscard]] WaitOutcome wait_until(WaitNode& condition_node,
                                         Scheduler::deadline_t deadline);
    [[nodiscard]] bool cancel(WaitNode& condition_node);
    void notify_one();
    void notify_all();

private:
    AsyncMutex& mutex_;
    Scheduler& scheduler_;
    WaitQueue waiters_;
    std::size_t active_waits_{0};  // debug lifetime guard (G-4); debug-asserted 0 in dtor
};
```
Private conceptual state matches prep §5 (lines 201-207) plus `active_waits_` (prep §14).
`scheduler_` is obtained as `mutex_`'s borrowed Scheduler (AsyncMutex already borrows
`Scheduler&`, async_mutex.hpp:208) — AsyncCondition reaches it via the bound Mutex; no second
Scheduler reference is stored independently (it is derived from `mutex_` at construction to
avoid drift). Exact storage of `scheduler_` is an implementation detail; the constraint is
that AsyncCondition uses the SAME Scheduler as its bound AsyncMutex (C-H2).

**No change to `AsyncMutex`** (sealed authority preserved). **No change to `WaitNode`/
`WaitQueue`/`Fiber`** (sealed). **Scheduler additions are private seams only** (below).

### G.2 New Scheduler private seams (in `scheduler.hpp`/`scheduler.cpp`, under the E12-D section)

These mirror the E12-A/E12-B/E12-C private-seam pattern (scheduler.hpp:317-559). All are
`private`, friend-gated by `AsyncCondition` (exactly as Event/Semaphore/AsyncMutex friend the
Scheduler or vice-versa per the existing sealed pattern — the established idiom is that the
primitive's methods call `scheduler_.<primitive>_*(...)` private seams, so `AsyncCondition`
is a friend of `Scheduler` or the seams are accessed via the same mechanism the other
primitives use).

```cpp
// CONDITION-WAIT-PREPARE combined seam (prep §7). Caller: AsyncCondition::wait/wait_until.
// Under global_mtx_: register condition node into `cond_waiters` (+ optional timer for
// wait_until), release/handoff the bound Mutex via mutex_handoff_one_locked (or owner_=nullptr
// if the Mutex queue is empty), make_waiting, release global_mtx_, context_switch.
// Returns the latched condition-node outcome (Woken/Expired/Cancelled).
WaitOutcome condition_wait_prepare(WaitQueue& cond_waiters, WaitNode& cond_node,
                                   AsyncMutex::handoff_target mutex_target);
//   where mutex_target is a thin handle carrying (WaitQueue& mutex_waiters, Fiber*& owner)
//   so the seam can call mutex_handoff_one_locked(mutex_waiters, owner) without exposing
//   AsyncMutex internals publicly. Exact parameter shape is an IMPLEMENTATION BOUNDARY.

// Deadline-aware variant: composes condition_wait_prepare with E11 TimerRegistration on the
// condition node (C-H4). Admission: if deadline already due -> resolve Expired inline WITHOUT
// releasing the Mutex or suspending (WaitDueInline); else register timer + prepare + suspend.
WaitOutcome condition_wait_prepare_until(WaitQueue& cond_waiters, WaitNode& cond_node,
                                         AsyncMutex::handoff_target mutex_target,
                                         deadline_t deadline);

// notify_one: under global_mtx_+cond_waiters.mtx(), wake_one_locked the condition queue ->
// retire timer -> --waiting_waitq_count_ -> make_runnable+route. No Mutex mutation.
void condition_notify_one(WaitQueue& cond_waiters);

// notify_all: under global_mtx_, loop wake_wait_one_locked(cond_waiters) until nullptr
// (mirrors event_set_broadcast drain, scheduler.cpp:1333-1335).
void condition_notify_all(WaitQueue& cond_waiters);

// Queue-identity-gated cancel (mirrors event_cancel_wait, scheduler.cpp:1370-1390).
[[nodiscard]] bool condition_cancel_wait(WaitQueue& cond_waiters, WaitNode& cond_node);
```

**The handoff-target parameter shape** is the one IMPLEMENTATION BOUNDARY worth calling out:
the combined seam must release the bound AsyncMutex's `owner_` via `mutex_handoff_one_locked`,
which needs `(WaitQueue& mutex_waiters, Fiber*& owner)`. AsyncMutex's `waiters_` and `owner_`
are private. Two options:
- (i) `AsyncCondition` is a `friend class AsyncMutex` (AsyncMutex already friend-gates
  Scheduler; adding AsyncCondition as a friend is the minimal exposure), and the seam takes
  `AsyncMutex&` directly, calling `mutex_handoff_one_locked(mutex.waiters_, mutex.owner_)`.
- (ii) AsyncMutex exposes a private `handoff_target()` returning `{WaitQueue&, Fiber*&}`,
  friend-gated to AsyncCondition.
Option (i) is simpler and matches the existing friend density (Scheduler is already a friend
of WaitQueue; AsyncMutex's privates are reachable by friend). The choice is deferred to the
construction task as an implementation detail; both preserve the sealed PUBLIC authority (no
public accessor is added).

### G.3 Queue ownership / state

- Condition queue: `AsyncCondition::waiters_` (private `WaitQueue`). Holds condition nodes
  (caller-supplied `WaitNode`, one per `wait()` call, registered Detached→Registered).
- Mutex queue: `AsyncMutex::waiters_` (untouched). Holds ordinary lock nodes AND reacquire
  nodes (both are ordinary `WaitNode`s; the Mutex cannot distinguish them and must not).
- A fiber is NEVER in both queues simultaneously: the condition node is terminal+unlinked
  (by `wake_one_locked`/`cancel_locked`/`expire_locked`, same CS as the resolve CAS) before
  the fiber resumes and creates its stack-local reacquire node. This is `InvNoDualQueueMembership`.
- `active_waits_` counts in-flight `wait()`/`wait_until()` calls (incremented at entry,
  decremented at return, including the Expired-at-admission inline path). Debug-asserted 0 in
  `~AsyncCondition()` (G-4).

### G.4 Lock ordering

Single canonical order, unchanged: `global_mtx_` → (Condition queue mtx OR Mutex queue mtx,
taken **sequentially** per prep §6.3, never simultaneously). Within the CONDITION-WAIT-PREPARE
CS:
```
hold global_mtx_
  lock cond_waiters.mtx(); register condition node (+ timer); unlock cond_waiters.mtx();
  // mutex_handoff_one_locked takes mutex_waiters.mtx() internally under global_mtx_:
  mutex_handoff_one_locked(mutex_waiters, owner);   // or owner = nullptr if empty
  me->make_waiting();
release global_mtx_
context_switch
```
The Condition queue mtx and the Mutex queue mtx are NEVER held at the same instant (prep §6.3).
`mutex_handoff_one_locked` acquires the Mutex queue mtx itself (scheduler.cpp:2072); the
combined seam does NOT pre-hold it, so no self-deadlock. This is the key evidence that no
Mutex-side `_locked` variant is strictly required (F-E1).

### G.5 Transfer protocol (notify)

- `notify_one`: resolves the eligible FIFO condition head Woken (`wake_one_locked`), unlinks
  it (same CS), retires any bound timer (`retire_timer_for_node_locked`), decrements
  `waiting_waitq_count_`, publishes the winner runnable (`make_runnable`+`route_runnable_locked`).
  **No Mutex state is touched.** The winner fiber resumes, runs its reacquire body
  (`mutex_.lock(reacquire_node)`), and enters the Mutex queue at that instant (C-H8 FIFO-tail).
- `notify_all`: under `global_mtx_`, loop `wake_wait_one_locked(cond_waiters)` until nullptr
  (mirrors `event_set_broadcast` scheduler.cpp:1333-1335). Each iteration resolves+unlinks+
  retires+routes one winner. The continuous `global_mtx_` hold is the atomic snapshot: late
  waiters cannot register (admission needs `global_mtx_`), so they are excluded (C-H10). Each
  waiter is resolved exactly once (the resolve CAS is the single winner authority).

### G.6 Grant-before-runnable protocol (Mutex handoff reuse)

The Mutex reacquire grant follows `mutex_handoff_one_locked` (scheduler.cpp:2056-2097)
unchanged: `wake_one_locked` (resolve+unlink) → `owner = f` (commit, line 2079, BEFORE
publication) → retire timer → `--waiting_waitq_count_` → `make_runnable`+`route` (2093-2094).
Owner-before-publication is already load-bearing (M-H1). The reacquire winner is the Mutex
FIFO head, kind-agnostic (C-H8, C9/C10). No new grant protocol is introduced.

### G.7 Error behavior

- Non-owner `wait()` (calling fiber does not own the bound Mutex): debug assert (caller
  precondition violation), no mutation — mirrors AsyncMutex's non-owner unlock contract
  (async_mutex.hpp:63-68).
- `cancel` on a detached/terminal/wrong-Condition node: returns `false` without mutation
  (mirrors `event_cancel_wait` truth table, scheduler.cpp:1370-1390). Wrong-Condition cancel
  is a safe false, not UB.
- `~AsyncCondition()` with active waits: debug assert (`active_waits_ == 0` and Condition
  queue empty); release is a no-op (no cancel-all, no wake-all) — mirrors Event/Semaphore/
  AsyncMutex destruction contracts.
- Recursive `wait()` by the owner (owner calls `wait()` while already waiting): impossible to
  express correctly; the precondition is that the caller owns the Mutex and `wait()` releases
  it. A second `wait()` on the same fiber would find it not owning — debug assert.

### G.8 Cancellation / deadline scope

- **In scope (E12-D):** condition-node cancel (`AsyncCondition::cancel`, queue-identity-gated,
  mirrors Event/Sem/Mutex); condition-epoch deadline (`wait_until`, E11 timer reuse, C-H4).
- **Out of scope (E12-G):** cross-primitive cancellation policy; reacquire-epoch cancel/expire
  (explicitly excluded by C-H5: reacquire is untimed + non-cancellable); task cancellation
  propagation. The reacquire node is never passed to any `cancel`/`expire_wait`/`wait_until`;
  it is created fresh and passed only to `mutex_.lock(reacquire_node)`.

### G.9 Prohibited (per brief §"Formal-only boundary" and §"范围约束")

- Do NOT copy formal-only instrumentation (`OrdinaryEpochs`, `Mark`, `lastAction`,
  `expectedFIFOHead`, NEG fault-injection fields, fixed slots) into production.
- Do NOT create a second/parallel mutex handoff for Condition (Candidate A rejected).
- Do NOT have the notifier create/register the reacquire node (Candidate B rejected; violates
  C-H7).
- Do NOT add Reacquire-fixed-priority or Ordinary-fixed-priority (violates C9/C10).
- Do NOT split condition/mutex waiters into two Mutex queues.
- Do NOT add a public `wait_queue()`/`mutex()`/`waiting_count()`/`notify_n()`/
  `reacquire_node()` accessor (frozen API, prep §5).
- Do NOT refactor Event/Semaphore/Mutex to share a factored cancel helper (out of scope).
- Do NOT implement E12-E/E12-F/E12-G, scheduler rewrite, public API overhaul, performance
  optimization, sleep-based tests, or fixed waiter slots.
- Production must support dynamic waiter counts (no fixed epoch slots).

---

## H. File-by-file plan

| File | Reason | Planned change | NOT allowed to change | Corresponding test |
|------|--------|-----------------|------------------------|--------------------|
| `include/sluice/async/condition.hpp` (NEW) | Add the AsyncCondition primitive | New header: `class AsyncCondition` per G.1, frozen public API (prep §5). Header banner mirroring event.hpp/semaphore.hpp/async_mutex.hpp (sealed authority, lock order, destruction contract). | — (new file) | compiled by `e12_async_condition_test` + authority probe |
| `include/sluice/async/async_mutex.hpp` | Allow AsyncCondition to reach `waiters_`/`owner_` for the handoff seam | Minimal: add `friend class AsyncCondition;` (option (i) in G.2). NO public accessor, NO semantic change, NO new public method. | Mutex semantics, FIFO, handoff order, public API, destruction contract, try_lock/lock/unlock/cancel/lock_until behavior | authority probe asserts no PUBLIC accessor exists |
| `include/sluice/async/scheduler.hpp` | Declare the new private E12-D seams | Add private seam declarations (G.2) under a new `// ---- E12-D AsyncCondition ----` section, mirroring the E12-A/B/C sections (scheduler.hpp:317-559). All `private`; friend-gate as the existing primitives are. | Existing seams, public API, fields, lock order, Scheduler state machine | — |
| `src/async/scheduler.cpp` | Implement the new seams + test-seam call sites | (1) Implement `condition_wait_prepare`/`_until` (combined seam: register+handoff+make_waiting+context_switch, per G.4); (2) `condition_notify_one`/`_all` (mirror `wake_wait_one_locked`/`event_set_broadcast` drain); (3) `condition_cancel_wait` (mirror `event_cancel_wait`); (4) add `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` phase call sites (e.g. `e12_condition_wait_prepare_after_register_before_handoff`, `e12_condition_notify_before_drain`) mirroring scheduler.cpp:2080-2087 and 1329-1332. | Existing methods, Fiber state machine, lock order, route_runnable_locked, make_runnable, timer subsystem, all other E12 seams | `e12_async_condition_test` (phase seams observed via controller) |
| `tests/e12_async_condition_test.cpp` (NEW) | Runtime tests T0–T32 (prep §18) | New test file mirroring `e12_async_mutex_test.cpp` scaffolding (FiberStack, IdleBackend, spin barriers, `run(1)`/`run(N)`/`run_live`). Implements the deterministic test plan in §I. | — (new file) | self |
| `tests/e12_async_condition_authority_probe.cpp` (NEW) | Seal the public API (F-E5) | New negative compile probe mirroring `e12_async_mutex_authority_probe.cpp`: attempts `cond.wait_queue()`, `cond.mutex()`, `cond.waiting_count()`, `cond.notify_n()`, `cond.reacquire_node()`, and `scheduler.wake_wait_one(cond.wait_queue())` — all must NOT compile. | — (new file) | compiled (expecting FAILURE) by the extended verify script |
| `xmake.lua` | Register the new test target | Add `target("e12_async_condition_test")` block mirroring `e12_async_mutex_test` (xmake.lua:972-984): `add_deps("sluice_core","sluice_async_internal_testing")`, `add_includedirs("include","tests")`, `set_group("test")`, `add_tests(...)`. The authority probe is NOT a target (compiled by the verify script, mirroring the mutex pattern). | Existing targets, build modes, sanitizer config, TSA flags | `xmake run e12_async_condition_test` |
| `scripts/verify-e12-async-condition-formal.sh` | Add the compile-probe authority gate (F-E5) | Add a `compile_probe_gate` mirroring `scripts/verify-e12-async-mutex-formal.sh:210-230`, compiling `tests/e12_async_condition_authority_probe.cpp` with `$CXX -std=c++20 -fsyntax-only -I include` and asserting FAILURE on the sealed names. Conditionally enforced only if the probe file exists. | The TLA+/TLC gate list (already 0-ed), the verdict logic, the jar handling | the script itself |
| `docs/e12-condition.md` | (NO CHANGE) | The preparation spec is CLOSED; this plan does not modify it. | — | — |

**Files explicitly NOT touched:** `include/sluice/async/event.hpp`, `semaphore.hpp`,
`wait_node.hpp`, `wait_queue.hpp`, `fiber.hpp`, `mutex.hpp`, `thread_annotations.hpp`,
`lock_guard.hpp`; `src/async/fiber.cpp`; all existing tests; all existing TLA+ models; all
other `src/*.cpp`. The only existing files modified are `async_mutex.hpp` (one friend line),
`scheduler.hpp`/`scheduler.cpp` (new private seams + test call sites), `xmake.lua` (one
target block), and the verify script (one gate). This is the minimal surface.

---

## I. Deterministic test plan

Each test: name, initial state, phase-control steps, key assertions, bug prevented, formal
counterpart. All tests use single-worker cooperative `run(1)` unless noted; determinism via
spawn order + `await_ready_flag`/`ReadyFlag` barriers + registered-flag happens-before
barriers (the established E12 idiom, NO `sleep_for` as causal proof; bounded waits only as
failure guards). Phase seams use the `E12ConditionSeam` controller facade (new, mirroring
`E12MutexSeam`/`E12EventSeam`).

### Basic semantics

**T0 — construction and bound-Mutex identity.** Construct `AsyncCondition cond(mtx)`. Assert
`&cond` is bound to `mtx` (observable only via behavior: a `wait()` on `mtx`'s owner releases
`mtx`). Bug prevented: wrong-Mutex binding. Formal: `InvReacquireSameFiber`/C-H2.

**T6 — notify_one empty.** `cond.notify_one()` with empty condition queue. Assert no crash, no
Mutation, `sched.waiting_count()==0`. Bug: lost-no-op / spurious wake. Formal: `NotifyOne`
empty-queue no-op.

**T10-Empty — notify_all empty.** Same for `notify_all()`. Formal: `NotifyAll` empty snapshot.

**T2 — notify before wait is lost (non-persistent).** Owner calls `cond.notify_one()` THEN
`cond.wait(cn)`. The notify must NOT be consumed by the later wait (non-persistent; no token
stored — prep §8). Assert the waiter suspends (is not immediately woken by the prior notify)
and is only released by a subsequent notify. Bug: notification-before-wait accumulated.
Formal: `NotifyOne` non-persistence.

**T7 — notify_one single waiter.** One owner parks at `cond.wait(cn)`; a coordinator (or the
owner after releasing) calls `notify_one`. Assert the waiter resumes, reacquires the Mutex
(`mutex_owner == waiter`), and `wait()` returns `Woken`. Bug: lost wakeup / no reacquire.
Formal: `NotifyOne` + `ReturnOwned`.

**T16 — wait returns owning (reacquire after Woken).** Assert `wait()` returns only after the
calling fiber owns the Mutex (C-H1). Use a second owner to hold the Mutex after notify so the
waiter must queue for reacquire; assert the waiter does NOT return until the second owner
unlocks. Bug: return-without-ownership. Formal: `InvReturnedOwnsMutex` / NEG-C3.

**Non-owner wait fails.** A fiber that does NOT own `mtx` calls `cond.wait(cn)`. Assert debug
assertion fires (caller precondition). Bug: non-owner wait corrupts ownership. Formal: NEG-C1
(`InvConditionWaiterDoesNotOwnMutex`).

### Lost-wakeup boundary (T3, T4, T5)

**T3 — wait register-before-release closure.** Arm the
`e12_condition_wait_prepare_after_register_before_handoff` seam. Owner enters `wait(cn)`;
the seam pauses AFTER the condition node is registered but BEFORE the Mutex is released. From
a coordinator thread, assert: condition node is Registered & linked in `cond.waiters_`
(observable via `sched.waiting_count()` ≥ 1), AND `mtx` is still owned by the owner. Release
the seam; the handoff+release+suspend proceeds. Bug: release-before-register lost-notify.
Formal: `InvNoLostNotifyWindow` / NEG-C8.

**T4 — notify in release/register boundary does not strand.** Two-phase: arm the pre-handoff
seam AND have a notifier thread blocked on `global_mtx_` (the seam holds it). Release the
seam; the notifier acquires `global_mtx_` and calls `notify_one`. Assert the condition waiter
is resolved Woken (not stranded) and eventually reacquires+returns. Bug: notify lost in the
register-release window. Formal: `WaitAdmissionSuspend` atomicity.

**T5 — Mutex direct handoff during Condition admission.** Pre-queue an ordinary Mutex waiter
(W1) on `mtx.waiters_` (W1 called `mtx.lock(w1)` and suspended). Owner enters `cond.wait(cn)`;
the CONDITION-WAIT-PREPARE handoff step must hand the Mutex to W1 (the FIFO head), NOT to the
condition waiter. Arm `e12_mutex_handoff_before_publication`; assert W1 is the handoff winner
(owner committed to W1 before publication) and the condition waiter remains suspended on the
condition queue. Bug: handoff diverted to the condition waiter (Candidate-A-like defect).
Formal: `MutexUnlockHandoff` kind-agnostic FIFO / NEG-C9, NEG-C10.

### Reacquire-before-return (T14, T16, T17, T18)

**T14 — notified waiter does not return while notifier owns Mutex.** Notifier holds `mtx`,
calls `notify_one`, then continues holding `mtx`. The notified waiter must NOT return from
`wait()` until the notifier unlocks. Assert via a `notifier_released` flag that the waiter's
return happens-after the unlock. Bug: return-without-reacquire. Formal: `InvReturnedOwnsMutex`.

**T17 — mandatory reacquire after Expired.** `wait_until` with a deadline; advance the test
clock past the deadline while the waiter is suspended (E11TimerControl). Assert the condition
node resolves `Expired`, the waiter reacquires the Mutex, and `wait_until` returns `Expired`
with ownership. Bug: Expired path skips reacquire. Formal: `WaitDueInline` (admission-due
keeps Mutex) vs suspended-Expired (reacquires).

**T18 — mandatory reacquire after Cancelled.** Cancel the condition node; assert the waiter
resumes, reacquires the Mutex, and returns `Cancelled` with ownership. Bug: Cancelled path
skips reacquire. Formal: `CancelCondition` + `ReturnOwned`.

### Condition FIFO (T8)

**T8 — notify_one FIFO multiple waiters.** Three owners W1,W2,W3 register on the condition
queue in deterministic order (sequenced by registered-flag barriers). Call `notify_one` three
times (interleaved with reacquire completion). Assert the notify ORDER is W1→W2→W3 (Condition
FIFO) via sequence numbers. NOTE (C-H8): the Mutex RETURN order need NOT match the notify
order — assert only the condition-notify order. Bug: condition-notify non-FIFO. Formal:
`InvNotifyOneFIFO`.

### Mixed ordering C9/C10 implementation counterparts (T15)

**T15a — Ordinary→Reacquire.** Construct: an ordinary locker O calls `mtx.lock()` and queues
(WaitQueue position 1). Then a notified condition waiter R wakes and calls
`mtx.lock(reacquire_node)` (position 2). The owner unlocks; assert O is granted first (FIFO
head), then R. Bug: reacquire fixed-priority (Candidate-A/C10 defect). Formal: reach1
(`<<Ordinary,Reacquire>>` reachable + `InvOrdinaryAndReacquireFIFO` / NEG-C10).

**T15b — Reacquire→Ordinary.** Mirror: R (notified) calls `mtx.lock(reacquire_node)` first
(position 1), then O calls `mtx.lock()` (position 2). Assert R granted first, then O. Bug:
ordinary fixed-priority / reacquire starvation (NEG-C9 "always grant R1" defect). Formal:
reach2 (`<<Reacquire,Ordinary>>` reachable + `InvEligiblePreMutexQueue` / NEG-C9).

In both, assert the grant order equals the real Mutex-queue enqueue order, NOT any class
priority.

### notify_all (T10, T11, T12, T13)

**T10 — notify_all snapshot.** Three waiters W1,W2,W3 registered. `notify_all`. Assert all
three resolve Woken exactly once, each reacquires, all return; condition queue empty at end.
Bug: incomplete drain / double-resolve. Formal: `InvNotifyAllSnapshotComplete`.

**T11 — notify_all excludes late waiter.** Arm `e12_condition_notify_before_drain`; while
paused mid-drain, a late waiter L attempts `cond.wait(cn_L)` (blocks on `global_mtx_`). Assert
L is NOT resolved by this `notify_all` (it registered after the snapshot). Bug: late waiter
consumes old notify_all. Formal: `InvNotifyAllExcludesLateWaiter`.

**T12 — notify_all cancel race.** During `notify_all` drain (paused at seam), cancel one
snapshot member. Assert exactly-one resolution (cancel loses to the Woken resolve, or vice
versa, but never both). Bug: double-resolve on cancel/notify race. Formal:
`InvSingleConditionResolution`.

**T13 — notify_all expire race.** During drain, expire one snapshot member (advance clock).
Assert exactly-one resolution. Bug: double-resolve on expire/notify race. Formal: same.

### No barging (T15-variants + no-barging)

**No-barging reacquire.** While a reacquire waiter R is queued on `mtx` (FIFO head), a
newcomer calls `mtx.try_lock()`. Assert `try_lock` returns false (R has FIFO priority). Bug:
barging past a reacquire waiter. Formal: `InvNoOwnerlessMutexDemand` / no-barging.

**Grant-before-runnable.** Arm `e12_mutex_handoff_before_publication` during a reacquire
grant. Assert `owner == winner` is committed BEFORE the winner is published runnable (the
existing mutex seam already proves this for ordinary handoff; the test asserts it holds for a
reacquire-node winner too). Bug: runnable-before-grant. Formal: `InvGrantOwnerCommit`.

### Lifecycle (T27, T28, T29)

**T27 — safe destruction empty.** Destroy `AsyncCondition` with no active waits. Assert no
assertion, no leak (ASan clean). Formal: `Destroy` / `destructionSafe`.

**T28 — destruction with Condition waiter (contract).** Destroy `AsyncCondition` while a
condition node is Registered. Assert debug assertion fires (`active_waits_`/queue-empty).
Bug: silent destruction stranding a waiter. Formal: NEG-C7 (`InvDestructionPrecondition`).

**T29 — destruction during reacquire (contract).** Destroy `AsyncCondition` while a fiber is
in the reacquire epoch (condition node terminal, reacquire node in the Mutex queue). Assert
`active_waits_` debug assertion fires (the Condition-queue-empty check alone is insufficient —
G-4). Bug: reacquire-phase destruction not caught. Formal: `Destroy` reacquire case (prep §14).

### Coordination stress (T30, T31, T32) — ASan+UBSan; TSan at low concurrency (F-E7)

**T30 — lost-notify coordination 1000/1000.** 1000 iterations: owner notifies at random
boundary points relative to waiter registration; assert no lost wakeup, no permanent sleep,
no double wake, no lock-free return. Exactly-once via XOR-of-outcomes idiom.

**T31 — notify/cancel/expire coordination 500/500.** 500 iterations: three-way race among
notify, cancel, and timer-expire on a condition node; assert exactly-one terminal outcome per
waiter.

**T32 — notify_all snapshot coordination 500/500.** 500 iterations: `notify_all` racing with
registration; assert snapshot completeness + late-exclusion.

### Migration / external-thread (T24, T25, T26)

**T24 — external-thread notify/cancel.** `notify_one`/`notify_all`/`cancel` called from a
non-worker OS thread (mirrors Event/Semaphore external-thread tests). Assert correct routing
(`pending_spawn_` path when `g_worker` null).

**T25/T26 — real Worker migration during Condition/reacquire phase.** Multi-worker `run(N)`;
assert a Condition waiter can suspend on W0 and resume (reacquire) on a different worker
after a steal (E8 identity model: ownership is `Fiber*`, not Worker). Bug: worker-pinned
ownership assumption.

### Authority probe (compile, must FAIL)

**Authority probe.** `tests/e12_async_condition_authority_probe.cpp` attempts the frozen-
excluded accessors (prep §5 lines 211-219) + direct `scheduler.wake_wait_one(cond.waiters_)`.
Must NOT compile. Compiled by the extended verify script. Bug: sealed public authority
bypassed. (Mirrors `e12_async_mutex_authority_probe.cpp`.)

---

## J. Commit plan

Subsequent construction (NOT this task) should use:

```text
feat(async): implement E12-D condition wait and mutex reacquire
```

If review finds a defect, the corrective commit:

```text
fix(async): correct E12-D condition handoff and lifecycle
```

This PLAN-ONLY task creates NO commit. The worktree remains clean and at `4501585`.

---

## K. Gate plan

All commands are quoted from the repo's authoritative sources (`xmake.lua:714-736`,
`README.md:171-225`, the verify scripts). No command is fabricated. There is NO automated CI
for io-core (F-E6); the gate is this manual sequence, run by the construction task after
implementation.

### K.1 Build (Debug + Release)
```sh
xmake f -m debug && xmake build
xmake f -m release && xmake build
```

### K.2 Target tests (the new + regression)
```sh
xmake build e12_async_condition_test && xmake run e12_async_condition_test
# primitive regression (unchanged primitives must still pass):
xmake build e12_async_mutex_test && xmake run e12_async_mutex_test
xmake build e12_event_test && xmake run e12_event_test
xmake build e12_semaphore_test && xmake run e12_semaphore_test
```

### K.3 io-core suite (full test group)
```sh
xmake build -g test && xmake run -g test
# (equivalently: xmake test)
# filter a single case: SLUICE_TEST_CASE=e12_t15a xmake run e12_async_condition_test
```

### K.4 Sanitizers
```sh
xmake f -m asan     && xmake build -g test && xmake run -g test
xmake f -m ubsan    && xmake build -g test && xmake run -g test
xmake f -m tsan     && xmake build -g test && xmake run -g test   # low-concurrency cases; F-E7
xmake f -m asanubsan&& xmake build -g test && xmake run -g test
xmake f -m valgrind && xmake build -g test
    valgrind --leak-check=full build/linux/x86_64/valgrind/e12_async_condition_test
```
Note (F-E7): TSan at high worker counts hits a known raw-fiber-asm DEADLYSIGNAL; the
deterministic causal tests (T0–T29) run under TSan; high-concurrency stress (T30–T32) runs
under ASan+UBSan and TSan only at low concurrency.

### K.5 Format / static gates
- Clang TSA gate (the ONLY enforced static gate; xmake.lua:43-44, 65-68):
  ```sh
  xmake f --toolchain=clang -c && xmake build sluice_async sluice_async_internal_testing
  ```
  (`-Wthread-safety` + `-Werror=thread-safety`; the new `condition.hpp` and scheduler seams
  are under `src/async/*.cpp` + `include/sluice/async/*.hpp`, so they are TSA-checked.)
- `clang-format` (manual, `.clang-format` exists; no xmake gate): `clang-format -i
  include/sluice/async/condition.hpp tests/e12_async_condition_test.cpp` and verify no diff.
- `clang-tidy` (`.clang-tidy` exists, `WarningsAsErrors: ''` — not a hard gate; run for
  information only).

### K.6 Formal verification
```sh
bash scripts/verify-e12-async-condition-formal.sh
```
Requires `java` on PATH and `TLA2TOOLS_JAR` (defaults `/tmp/tla2tools.jar`; the script prints
the fetch URL if missing). Exit 0 = all 14 sub-gates produced expected verdicts (positive
PASS; reach1/reach2 CEX; NEG-C1..C10 each violate its named property; WRONG-PROPERTY PASS).
The construction task should ALSO run the sibling formal gates to confirm no regression:
```sh
bash scripts/verify-e12-async-mutex-formal.sh
bash scripts/verify-e12-event-formal.sh
bash scripts/verify-e12-semaphore-formal.sh
```

### K.7 Authority-seal compile probe (added to the verify script per F-E5)
The extended `verify-e12-async-condition-formal.sh` compiles
`tests/e12_async_condition_authority_probe.cpp` with `$CXX -std=c++20 -fsyntax-only -I include`
and asserts FAILURE (mirroring `verify-e12-async-mutex-formal.sh:210-230`). Run as part of K.6.

### K.8 Repository authoritative gate
There is no umbrella `xmake regression`/`make check`. The authoritative gate is the union of
K.1–K.7: Debug+Release build, full test group pass, sanitizer matrix clean (with the TSan
caveat), Clang TSA gate clean, formal gate 0-ed (including the new compile probe). A
construction task is "green" only when ALL of these pass.

---

## L. Stop statement

```text
PLAN COMPLETE — NO IMPLEMENTATION PERFORMED
AWAITING EXTERNAL PLAN APPROVAL
```

No production code, test code, existing documentation, or worktree state was modified in this
task. The single artifact produced is this plan file
(`docs/reviews/e12-d-condition-cpp-implementation-plan-1.md`), which is the explicitly
permitted output. The worktree remains clean at `4501585` on `feat/e12-D-condition-formal`.
Implementation awaits external approval of this plan.
