# E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1

## A. Independence

```text
TASK:
E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1

REPOSITORY:
jnhu76/Sluice

BASE_COMMIT:
be70fdec102e3a0d082330b9d8bba9f78ac3fcdb

MODE:
INDEPENDENT / READ-ONLY ADVERSARIAL DESIGN ANALYSIS
```

No AI-1 artifacts were read. All analysis derives from `master` source audit.

---

## B. Mandatory inherited constraints verification

Each constraint confirmed against the master source base:

### `primitive cancel != Select loser authority`

Confirmed. `Event::cancel`, `Semaphore::cancel`, `AsyncMutex::cancel` all operate on a still-Registered WaitNode via queue-membership-gated `cancel_locked`. They CANNOT undo a committed resource transfer (permit consumed, owner assigned, payload moved). This is explicitly documented in `e10-e12-api-semantic-closure.md` §3.14 E13 Readiness Mapping and D10.

**Evidence:** `wait_queue.hpp:222-233` — `cancel_locked` calls `resolve_(Cancelled)`; if the node is already terminal (Woken by resource), the CAS fails and returns false. No rollback.

### `group winner selection must be ordered before irreversible primitive commit`

Confirmed mandatory ordering. AsyncMutex `mutex_handoff_one_locked` (`scheduler.cpp:2079`) assigns `owner = f` BEFORE `make_runnable` + `route_runnable_locked` at line 2093. The owner commit IS the irreversible step. Semaphore release transfer resolves the node Woken (irreversible) in the same critical section. For Select, group winner selection must happen BEFORE any such irreversible commit, NOT as a compensation afterward.

### `first-scope cross-Scheduler Select is not authorized`

Confirmed. Every primitive (`Event`, `Semaphore`, `AsyncMutex`, `AsyncCondition`, `AsyncQueue<T>`) borrows a single `Scheduler&`. All coordination runs under that Scheduler's `global_mtx_`. Cross-Scheduler operations would require a second coordination domain. D10 explicitly states "First-scope Select arms MUST bind to the same Scheduler if atomic multi-arm coordination relies on one Scheduler::global_mtx_ domain."

### `WaitNode terminalization is absorbing`

Confirmed. `WaitNode::resolve_` transitions `Registered -> {Woken, Cancelled, Expired}` via CAS. No `reset()` or `rearm()` exists. Once terminal, the node stays terminal forever. `~WaitNode` asserts `!is_registered()`. `register_` checks `State::detached` — a terminal node fails registration.

### `exactly-once runnable publication is required`

Confirmed. `Fiber::make_runnable()` returns bool — it is the exactly-once guard. `route_runnable_locked` is called only after `make_runnable()` returns true. Every resolution path checks this. `scheduler.cpp:2093`: `if (f->make_runnable()) { route_runnable_locked(...); }`.

### `Queue payload and Mutex ownership are resources, not merely wake notifications`

Confirmed. Mutex `owner_` is assigned to the winner Fiber. Queue operations transfer typed lease ownership through `QueueItemLease`/`ItemNode<T>`. These are irreversible state mutations, not mere wake signals. The `E12-E` design explicitly says "A successful suspended operation resumes only after its concrete item or committed producer payload has been bound to that exact wait epoch" (§Success protocol).

### `AsyncCondition has a two-epoch protocol with mandatory Mutex reacquire`

Confirmed. `condition.hpp:17-33`, §3 Protocol overview. The Condition epoch resolves, then a mandatory untimed non-cancellable Mutex reacquire epoch runs. The caller-provided Condition node is terminal before the stack-local reacquire node is created. No single WaitNode can span both epochs.

---

## C. Required attack traces

### C1. Semaphore loser consumes permit

**Setup:** `Select(Event A, Semaphore S)` where `S` has exactly one permit. Both arms become ready concurrently: `S` release transfers the permit via `wake_one_locked` AND `Event A` is set, both before group winner is chosen.

**Timeline:**

```text
1. Select arms registered: node_E in Event queue, node_S in Semaphore queue
2. T1: external thread calls S.release()   // creates one pending permit
3. T1: global_mtx_ acquired
4. T1: wake_wait_one_locked(S.waiters_) sees node_S at FIFO head
5. T1: node_S.resolve_(Woken) succeeds     // <-- IRREVERSIBLE COMMIT
6. T1: unlink_locked(node_S)
7. T1: --waiting_waitq_count_
8. T1: make_runnable(node_S.fiber)         // loser fiber will resume
9. T1: route_runnable_locked(...)
10. T1: global_mtx_ released
11. T2: Event A set() wins race OR is already SET
12. T2: Event A admission path resolves node_E Woken
13. T3: Select group winner selection runs, picks Event A
```

**Analysis:**

The Semaphore permit IS consumed (step 5). The Semaphore loser's WaitNode is Woken — terminal, unlinked, runnable published (step 8-9). The loser Fiber will resume and read `WaitOutcome::woken` from its node. By the time group winner selection picks Event A, the Semaphore permit is gone and cannot be returned.

**Permit recovery analysis:**

- The consumed permit cannot be returned to `available_` — that would break the conservation law (`available + acquiredCount == initialPermits + acceptedReleaseCount`). If we increment `available_` after a resolved node, we create a permit from nothing.
- The permit WAS genuinely transferred. The loser has a legitimate `Woken` outcome — from the Semaphore's perspective, a successful acquire occurred.
- Re-injecting the permit into `available_` would create a state where the loser Fiber continues thinking it acquired, and the permit counter is wrong.
- A "compensation" that cancels the loser would set `Cancelled` on an already-`Woken` node — the CAS would fail (absorbing state).
- The only correct behavior: the loser IS genuinely woken. Select must NOT attempt to undo a primitive commit. The group winner selection must prevent this by ensuring loser arms are resolved as losers BEFORE any primitive-level commit.

**Conclusion:** Permit is lost if the Semaphore release resolves the node before group winner. This cannot be rolled back. **Therefore Select must ensure that for Semaphore (and all permit-bearing primitives), the group decision happens BEFORE permit transfer.** Any design where the primitive resolves before Select confirms group authority is broken.

---

### C2. Mutex loser becomes owner

**Setup:** `Select(Event A, AsyncMutex M)`. Fiber F1 holds M. F1's unlock hands ownership to the Select arm's Mutex node. Event A wins group afterward.

**Timeline:**

```text
1. Select registered: node_E in Event queue, node_M in Mutex queue
2. F1 (current owner) calls M.unlock()
3. global_mtx_ acquired
4. mutex_handoff_one_locked:
   a. wake_one_locked(M.waiters_) -> node_M
   b. node_M.resolve_(Woken) succeeds   // IRREVERSIBLE
   c. owner_ = node_M.fiber              // F_select now OWNS the mutex
   d. make_runnable(node_M.fiber) + route
5. global_mtx_ released
6. Group winner selection picks Event A (not Mutex)
```

**Analysis:**

The Select arm's Mutex WaitNode is terminal with `Woken` outcome. The Mutex `owner_` is set to `F_select`. The Mutex ownership has been IRREVERSIBLY committed to the Select fiber. This cannot be rolled back:

- Cannot undo `resolve_(Woken)` — it's absorbing.
- Cannot transfer ownership to another fiber without an explicit `unlock()` call, which only the current owner can do.
- Compensating by making the Select fiber call `M.unlock()` and somehow hand off to a third party is a protocol violation — the fiber never intended to acquire the Mutex in the first place.
- Setting `owner_` back to `NoOwner` would create a window where the Mutex is unlocked but the loser was already published as Woken — a fiber running with a `Woken` outcome yet not owning the Mutex. Any correct program would try to enter the critical section and trigger a debug assertion (non-owner unlock) or data race.

**Conclusion:** Mutex ownership transfer is irreversible once `owner_` is assigned and `make_runnable` is called. **Select must guarantee group winner is chosen BEFORE Mutex handoff proceeds.** A design where the Mutex unlock path does not check group authority before committing owner_ is unsound.

---

### C3. Queue pop loser moves payload

**Setup:** `Select(Event A, QueuePop Q)`. A producer has pushed an item. `Q.pop()` resolves the Select's pop arm: it removes the item from the ring and moves it into the operation's local storage. Event A wins group.

**Timeline:**

```text
1. Select registered: node_E in Event queue, node_pop in ConsumerWaiters queue
2. Item X is in the ring at slot i
3. Queue reconciler selects the waiting pop operation as the oldest eligible consumer
4. Under global_mtx_ + port mutex:
   a. ItemNode<T> at slot i is located
   b. Lease ownership transferred from ring to consumer operation
   c. node_pop resolved Woken
   d. Consumer operation result = item(X)
   e. slot i cleared
   f. make_runnable + route
5. Group winner selection picks Event A
```

**Analysis:**

Item X has been physically removed from the ring and moved into the consumer operation's typed result. The ring slot is cleared — the item is gone. Key problems:

- `T` may be move-only (`std::is_nothrow_move_constructible_v<T>`, no copy requirement). Item X cannot be duplicated.
- Re-inserting X into the ring would require pushing it back — but the Queue's owned lease system requires a fresh `detached` item to enter through the producer path. There is no "reinsert from consumer operation" path.
- Even if we could reinsert, X would go to the TAIL of the ring, changing FIFO order.
- The consumer's typed `QueuePopResult<T>` already holds item(X). The loser fiber will resume, see `Woken` on its node, and try to extract the item from the pop result — which is legitimate from the Queue's perspective.

**Conclusion:** Queue pop payload transfer is irreversible. The item is physically moved. **Select must prevent Queue pop from committing the item before group winner is determined.** A design that resolves the Queue wait before group selection WILL lose or duplicate payloads.

---

### C4. Queue push loser publishes value

**Setup:** `Select(Timer, QueuePush(value))`. A consumer is already waiting (Queue not full from producer side). `QueuePush(value)` direct-handoffs the value to the waiting consumer (via the ring — queue state machine P6 ProducerGrantCommit). Timer wins group.

**Timeline:**

```text
1. Select registered: timer_active, node_push in ProducerWaiters queue
2. Consumer C is waiting on ConsumerWaiters (queue not empty)
3. Timer fires while Queue reconciler is resolving push
4. Queue reconciler: ProducerGrantCommit
   a. detached -> producer_operation -> ring (item committed)
   b. Reconciler finds eligible consumer C
   c. ring[head] -> consumer_operation
   d. Consumer C resolved Woken
   e. Consumer C's QueuePopResult<T> now holds the item
   f. make_runnable(C)
5. Timer wins group: group authority picks Timer
```

**Analysis:**

The item is already in the consumer's typed result. A third Fiber (consumer C) can now observe the pushed value. This is an **externally visible side effect that cannot be rolled back**. Unlike C3 where the loser fiber itself holds the payload, here a completely unrelated third party (consumer C) has received the item. 

- The consumer C has no relation to Select — it's just a waiter on the Queue.
- The consumer C's program order proceeds with the item.
- Select telling the caller "Timer won, push didn't happen" would be a LIE — the push DID happen (item is in consumer C's hands).
- Even the compensation of "tell the caller the push committed" is wrong because the Timer already resolved.

**Conclusion:** Queue push that delivers to a waiting third-party consumer is an irreversibly visible side effect. **Select must not allow the Queue arm to commit (ring insertion + consumer grant) before group winner selection.** This is the strongest "winner before commit" argument for Queue.

---

### C5. Condition loser enters reacquire

**Setup:** `Select(Event A, Condition C)`. Condition C's condition epoch resolves (notify_one fires). The Condition begins mandatory AsyncMutex reacquire. Event A wins group.

**Timeline:**

```text
1. Select registered: node_E in Event queue, node_C in Condition queue
2. Mutex M is unlocked (not held by any fiber)
3. notify_one on C wakes node_C (Condition epoch resolves, Woken)
4. Condition wait resumes:
   a. node_C = Woken (terminal)
   b. Stack-local reacquire_node created
   c. M.lock(reacquire_node) begins
5. M is unlocked, so reacquire_node registers in M's queue
6. Another fiber locks M (barging into the FIFO)
7. Condition loser is now blocked on M forever (or until M is unlocked)
8. Meanwhile, Event A set() fires
9. Select group winner selection picks Event A
```

**Analysis:**

The Condition loser is in an irreversible two-phase protocol: the Condition epoch returned, and the mandatory reacquire epoch is in progress. Key problems:

- **The Condition node is already terminal.** The loser cannot "cancel" the Condition notification.
- **The reacquire cannot be cancelled** (C-H5: "mandatory reacquire is untimed and non-cancellable"). There is no `cancel` on the reacquire node.
- **The Fiber is suspended in `mutex_.lock()`**, which is inside the Select implementation, not at the caller level. The Select call has not returned.
- **If Select returns `Event A` while the reacquire is blocked**, the caller would get a result saying Event A won, but the Fiber would still be blocked in `Condition::wait`'s reacquire — the stack frame is not unwound.
- **If Select cancels the reacquire** (impossible — C-H5 explicitly forbids it), the Mutex ownership would be ambiguous.
- **The Mutex waiter queue now has the Condition's reacquire node.** If Select returns, who removes this node? The Mutex still routes to it.
- **The reacquire node is stack-local** in the Condition::wait frame. If Select somehow returns before the reacquire completes, the stack-local WaitNode is destroyed while Registered — UB (assertion in `~WaitNode`).

**Conclusion:** AsyncCondition as a direct Select arm is fundamentally incompatible with Select's single-return semantics. **The mandatory reacquire means the Condition wait call cannot be preempted mid-way.** AsyncCondition must be DEFERRED from first-scope Select, or a new combined seam must abort the Condition protocol at the Condition-epoch boundary before the reacquire begins.

---

### C6. Ready during partial registration

**Setup:** Select arms registered one by one. Arm 0 is Event — it becomes SET (ready) after registration but before arm 1 (Semaphore) is registered. The caller is not yet Waiting.

**Timeline:**

```text
1. Select begins: requires global_mtx_
2. Register arm 0 (Event): node_E registered in Event queue
3. Event is already SET OR set() is called on another thread
4. node_E resolves Woken (admission-time SET check)
5. --waiting_waitq_count_
6. make_runnable(current Fiber)    // but fiber is still running Select!
7. Register arm 1 (Semaphore): node_S registered
8. Fiber not yet Waiting — the make_runnable from step 6 is a no-op
   (fiber is Running, not Waiting — make_runnable returns false)
9. Select registers remaining arms
10. Select cannot proceed: one arm is already Woken, but no suspension happened
```

**Publication ordering analysis:**

The Event admission in step 4 resolves `Woken`. However, `make_runnable` returns false because the fiber is Running (not Waiting). The exactly-once publication guard prevents double-publication. This means:

- The `Woken` outcome is correct.
- But the fiber continues registering other arms, which is wasted work.
- After all arms are registered, Select must check for already-resolved arms before suspending. If one arm is already terminal, Select must NOT suspend — it must clean up remaining arms and return immediately.
- The cleanup of arm 1 (Semaphore) must cancel node_S (best-effort, may be already terminal too).

**Key observation:** Registration order matters for publication. A resolved arm during registration cannot publish the fiber (fiber is Running). Select must detect this post-registration and either skip suspension or handle the already-terminal case.

**Partial registration + ready case is not a correctness bug IF Select correctly detects terminal nodes before suspending.** It IS a performance concern (wasted registration of remaining arms) and a correctness concern if the cleanup of remaining arms fails (e.g., Queue arm already committed payload).

---

### C7. Two admission-ready arms

**Setup:** Event already SET (persistent readiness). Semaphore permit already available (permit > 0). Both arms are ready at Select start time.

**Timeline:**

```text
1. Select begins: global_mtx_ acquired
2. Register arm 0 (Event): node_E in Event queue
3. SET check: Event is SET -> wake_node_locked(node_E) -> resolve Woken -> unlink
4. Register arm 1 (Semaphore): node_S in Semaphore queue
5. Admission check: available > 0 AND node_S is FIFO head
   -> wake_node_locked(node_S) -> resolve Woken -> unlink
6. Both arms are terminal Woken
7. Select must pick exactly ONE winner
```

**Analysis:**

Both arms can legally be resolved Woken at admission time (they are registered and immediately resolved). This is fine — the primitive sees a legitimate acquire. Select must:

1. Detect that multiple arms are terminal after full registration.
2. Choose a winner deterministically (e.g., arm index order).
3. For the losing arm(s): the Woken outcome is already committed. The primitive resource has been consumed (permit decremented for Semaphore). **The loser arm's resource is gone, and the fiber's node is Woken.**

**Fairness concern:** If both arms are ready, the first-registered arm always wins if Select picks arm-index order. But arm-index order is arbitrary — the user may have listed Event first purely by code convention. A Semaphore permit is consumed even though Event was the group winner. From the user's perspective, the Semaphore permit was "lost" — consumed by Select.

**Is this a problem?** Yes, for stateful resources (Semaphore permits, Mutex ownership). The user expected to acquire one of the two, but both primitives report success. However, because the consuming primitive (Semaphore) committed first, the permit is legitimately consumed. The user would need to know that `Select` consumed a permit they didn't intend to use.

**Linearizability:** This IS linearizable — each primitive operation ran atomically and succeeded. The issue is resource semantics, not correctness per se.

---

### C8. Timer/resource simultaneous resolution

**Timeline patterns:**

**Case: Timer + Event**

```text
1. Select(Timer, Event) registered
2. Timer fires: try_claim_expiry succeeds (ACTIVE -> CONSUMED)
3. Concurrently, Event set() fires
4. Event set() drain resolves node_E Woken
5. Timer expiry resolves (second resolver) finds node_T already terminal
   but wait — two different nodes? No, each arm has its own WaitNode
6. If Timer arm's node is CONSUMED, it's the terminal authority
7. But the Event arm's node is also Woken
8. Select must pick which arm WON
```

**Key insight:** For Timer arms, `TimerRegistration::try_claim_expiry` is the timer's winning authority. It returns true if this expiry is the winner. The WaitNode is then resolved with `Expired` by the expiry path. But the resource arm (Event) resolves independently on its own node.

The resolution races are PER-ARM: each arm has its own `resolve_` CAS and its own `TimerRegistration` (if timed). The Select-level coordination must handle the case where multiple arms are independently terminal.

**Case: Timer + Queue push**

```text
1. Select(Timer, QueuePush(v)) registered
2. Queue reconciler finds eligible consumer, commits item to ring, grants to consumer
   -> QueuePush arm terminal with Committed result
3. Timer fires simultaneously
4. Timer claims expiry, resolves node_T with Expired
5. Both arms terminal — but QueuePush already affected a third party (consumer)
```

The Queue push committed to the ring and a consumer received the item. Even if the Timer is the group winner, the push happened. This is the C4 scenario replayed with a timer.

---

### C9. Destruction while callback armed

**Setup:** Select returns or throws. Timer registration still points at Select's group structure. External thread later wakes the primitive, which routes to Select's now-destroyed group.

**Timeline:**

```text
1. Select(Event A, Timer T) registered
2. Timer T is active (TimerRegistration state=ACTIVE)
3. Select returns (Event A wins, timer retired)
4. BUT — retirement fails (timer concurrently consumed?)
   OR Select arms were not fully cleaned up (exception path)
5. Select's group structure (stack-local or heap-allocated) is DESTROYED
6. Later: pump_deadlines_locked hits the stale TimerRegistration
7. Try_claim_expiry fails because it's already RETIRED/CONSUMED
   -> No UAF in the normal case (I4 closure)
```

**Analysis with TimerRegistration I4 protection:**

The `TimerRegistration`'s ACTIVE/RETIRED/CONSUMED state machine closes the UAF window. If Select properly retires all timer registrations before returning (which it MUST), the stale pump will see RETIRED and not dereference the node or group pointer.

**The dangerous case:** If Select fails to retire a timer (partial registration failure, exception during cleanup), and the TimerRegistration is still ACTIVE, the expiry path will:
1. `try_claim_expiry()` returns true (ACTIVE -> CONSUMED)
2. Dereferences `node_` — which points to a Select-internal WaitNode or group structure
3. The WaitNode is in a destroyed Select group -> **USE AFTER FREE**

**Mitigation:** Every Select arm's resources must be cleaned up BEFORE Select returns, even on failure paths. The TimerRegistration retirement MUST succeed. If it cannot (race with expiry), the expiry must NOT touch destroyed structures — which is safe IF the TimerRegistration is designed with the lifetime closure. But the group structure itself (not just the WaitNode) must outlive the potential expiry.

**Relevant existing mechanism:** `TimerRegistration` stores `WaitNode*` and `WaitQueue*`. After retirement, these are not dereferenced. But if the Select group introduced a new group-level structure (not a WaitQueue), the timer would need to reach it through a different path. **Any Select-internal structure referenced by the timer MUST outlive the timer's ACTIVE state.**

---

### C10. Partial registration failure

**Setup:** Select tries to register k arms. The k-th arm fails (bad allocation, invalid cross-Scheduler, reused node, or unsupported arm type). Arms 1..k-1 are already registered.

**Timeline:**

```text
Select(Event A, Semaphore S, AsyncMutex M, QueuePop Q)
Register A: OK (node_A in Event queue, waiting_waitq_count_++)
Register S: OK (node_S in Semaphore queue, waiting_waitq_count_++)
Register M: OK (node_M in Mutex queue, waiting_waitq_count_++)
Register Q: FAILS (e.g., queue in teardown, or node was already registered)
```

**Cleanup analysis:**

Each of A, S, M has a registered WaitNode in its respective primitive queue. Each has incremented `waiting_waitq_count_`. The Scheduler's worker loop will see MW-S3 (unresolved waits) and may stall.

Cleanup must:
1. Cancel/wake nodes A, S, M (best-effort, may fail if already terminal)
2. Decrement `waiting_waitq_count_` for each
3. Retire any timer registrations
4. NOT suspend (we never made the fiber Waiting)
5. Report the failure to the caller

Problems:
- **Queue arm (Q) may have already committed payload** before the registration failure is detected. See C3/C4.
- **Cancel on a just-registered arm may race with a concurrent primitive operation.** E.g., another thread calls `set()` on Event A between arm registration and the failure. Cancel returns false — node_A is already Woken. The permit or resource is consumed.
- **The cleanup is not atomic.** Between cancelling arm A and arm S, another resolver could wake arm S. The canceled arms that succeeded are fine (they report Cancelled). But arms that resolved Woken during cleanup are "consumed" resources for which the caller gets no corresponding success.

**The fundamental challenge:** There is no rollback mechanism for primitive operations. Once a WaitNode is registered and the primitive resolves it, the resource is gone. Partial failure cleanup is best-effort — it cannot guarantee that all resources are returned to their primitives.

---

## D. Candidate protocol comparison

### P1. Central group CAS under Scheduler global authority

A single group-level `resolve_`-like CAS, reachable only under `global_mtx_`. All arms register, then one atomic CAS selects the winner. The loser arms are cleaned up while still holding `global_mtx_`.

**Strengths:** Leverages existing `global_mtx_` serialization. Clean linearization point. No need for new atomic primitives — the group state is mutex-protected, not lock-free.

**Weaknesses:** All arms must be registered before the group CAS (sequential registration latency). Some arm registrations may be wasted (arm becomes ready before group CAS, as in C7). The group CAS itself must happen under `global_mtx_`, extending the critical section.

### P2. Reversible reservation + winner commit

Each arm performs a "reservation" (reversible claim on the resource). The group picks one winner and commits it; all other reservations are released.

**Strengths:** No wasted resource consumption (reservation is reversible). Arms can be registered independently.

**Weaknesses:** Requires new "reservation" seam on every primitive — a reversible permit hold, reversible Mutex ownership, reversible Queue payload slot reservation. This does not exist in the current design and would require significant refactoring. For Queue with typed payloads, reservation would need to hold a slot without moving the item — possible but adds complexity. **The existing primitives are designed around one-shot committed transitions, not two-phase reserve-then-commit.**

### P3. Primitive commit + loser compensation

Let all eligible arms resolve normally through their primitives. Then pick a group winner. For losers, compensate: return permits, transfer Mutex ownership, push Queue items back, etc.

**Strengths:** No changes to primitive registration paths. Arms proceed independently.

**Weaknesses:** **Compensation is impossible for most primitives.** Semaphore: cannot create a permit from nothing (conservation law). Mutex: cannot undo `owner_` assignment — the loser has a Woken outcome and believes it owns the Mutex. Queue: cannot reinsert a typed payload without changing FIFO order and duplicating moves. AsyncCondition: cannot interrupt the reacquire epoch. **This protocol is fundamentally unsound for resource-bearing primitives.**

### P4. Dedicated Select registration object inside each primitive

Each primitive gets a Select-aware registration path that creates a "Select arm" object within the primitive, linked to a group. The group coordinates winner selection before the primitive resource is committed.

**Strengths:** Clean separation of concerns. Each primitive can implement its own "Select arm" reservation without changing the WaitNode registration path.

**Weaknesses:** Requires new internal API on every primitive (6 primitives × multiple methods each). Increases primitive surface area. The registration path needs a new "deferred commit" state that the primitive does not currently have. **Large scope, high complexity.**

### P5. Poll-all then subscribe-all hybrid

Phase 1: poll all arms (try_lock, try_acquire, is_set, try_pop, try_push). If any succeeds, return immediately. Phase 2: register all arms and suspend.

**Strengths:** Fast path for immediately-ready resources (C7). No wasted suspension. Works with existing non-blocking APIs.

**Weaknesses:** **TOCTOU race between poll and subscribe.** A resource may be available at poll but consumed before subscribe (lost wake). Another resource may become ready between poll and subscribe (missed). The poll phase is advisory only — the subscribe phase must close the lost-wake window correctly, which requires the same registration+recheck pattern as the existing primitives. **The poll phase adds complexity without eliminating the fundamental challenge.** The subscribe phase still needs group winner selection.

### Scoring

| Criterion                  | P1 | P2 | P3 | P4 | P5 |
| -------------------------- | -: | -: | -: | -: | -: |
| Winner-before-commit       | 2  | 3  | 0  | 3  | 1  |
| Exactly-once publication   | 3  | 3  | 1  | 3  | 2  |
| Existing primitive changes | 3  | 0  | 2  | 1  | 2  |
| Queue compatibility        | 2  | 1  | 0  | 2  | 1  |
| Condition compatibility    | 2  | 0  | 0  | 1  | 1  |
| Formal-model tractability  | 3  | 1  | 0  | 2  | 1  |
| Failure rollback           | 2  | 3  | 0  | 2  | 2  |
| Same-Scheduler simplicity  | 3  | 2  | 2  | 2  | 2  |

**Score: 0-3 (3=best).**

### Recommendation

**P1 (Central group CAS under Scheduler global authority) is the recommended protocol.** It requires the fewest primitive changes (arms register through existing paths), provides a clean winner linearization point under `global_mtx_`, and the global_mtx_ serialization handles the winner-before-commit ordering for semaphore/mutex/event. 

For Queue, P1 requires a new internal seam: the reconciler must check group authority before committing items. This is a narrower change than P2's full reservation protocol.

For Condition, P1 cannot make it work — the two-epoch protocol is incompatible regardless of the coordination protocol chosen. Condition is deferred.

---

## E. First-scope recommendation

### S1: Event + Timer

**VERDICT: SAFE FOR FIRST SCOPE**

Both Event and Timer have no irreversible resource outside the WaitNode outcome. Event's `set_` is persistent and can be set again. Timer's expiry resolves the WaitNode but TimerRegistration has proper I4 lifetime closure. No payload ownership, no mandatory reacquire.

The Select group winner for Event+Timer is: either Event resolves Woken (admission or set()), or Timer resolves Expired. Both are clean terminal outcomes on distinct WaitNodes. The loser arm is simply cancelled (Event wait cancelled, timer retired). No external side effect.

### S2: Event + Timer + Semaphore

**VERDICT: SAFE ONLY WITH INTERNAL REFACTOR**

Semaphore adds irreversible permit consumption. Select must ensure the group winner is determined BEFORE the Semaphore's `resolve_(Woken)` on the arm's node. This requires a new internal seam: a "group check" before `wake_node_locked` or `wake_one_locked` in the Semaphore release path.

The seam could be: Semaphore_release checks a group pointer on the FIFO head. If the head is registered in a Select group, the release defers the permit transfer to the group coordinator rather than immediately resolving the node. The group coordinator then picks one winner and releases permits for losers.

**This is a non-trivial change to the Semaphore release path.** Alternative: the Select group CAS runs BEFORE any primitive's admission path finalizes. This requires Serialized Registration (register all arms atomically, then resolve with group CAS, all under a single global_mtx_ acquisition). The Semaphore acquires register the wait node but no permit is consumed until the group CAS authorizes it. This means Semaphore needs a new "pending" state between registration and group authorization.

### S3: Event + Timer + Semaphore + AsyncMutex

**VERDICT: SAFE ONLY WITH INTERNAL REFACTOR**

AsyncMutex adds irreversible `owner_` assignment. Same issue as Semaphore: direct handoff must check group authority before committing `owner_`. Additionally, Mutex unlock with handoff path (`mutex_handoff_one_locked`) must verify that the handoff target is the Select group winner, not just the FIFO head.

**New seam required:** `mutex_handoff_one_locked` must check if the head waiter is Select-grouped. If so, the handoff must be coordinated with the group. Same "pending" state concern as Semaphore.

### S4: Add Queue

**VERDICT: DEFER**

Queue payload transfer can affect third-party consumers (C4). Even with perfect group coordination, a Queue push that commits to the ring is visible to other consumers. The Select group must prevent ring commit before group winner, which requires a new "pending commit" state in the Queue reconciler.

Additionally:
- Queue has no cancel in v1 (D4). The loser arm cannot be cancelled if it committed.
- Typed payloads make compensation impossible (C3).
- Queue reconciler path is the most complex in the codebase.

Queue should be deferred until the simpler primitives (Event, Timer, Semaphore, AsyncMutex) are proven in Select.

### S5: Add AsyncCondition

**VERDICT: REJECT**

AsyncCondition's two-epoch protocol (Condition epoch + mandatory Mutex reacquire) is fundamentally incompatible with Select semantics. The reacquire epoch cannot be cancelled or timed out (C-H5). A fiber in the reacquire epoch is blocked on an ordinary Mutex lock, indistinguishable from any other Mutex waiter.

Even if we "wrap" Condition for Select by running the full Condition::wait inline and returning the outcome as an arm result, the reacquire would block the Select fiber before Select can return. This changes the semantics: Select would not return until the Condition wait fully completes (both epochs), which defeats the purpose of Select (await any one of multiple alternatives).

**AsyncCondition as a Select arm is rejected for first scope.** A possible future design could treat Condition as a special arm that only participates if the Mutex is already owned and the predicate is satisfied without blocking — but this is a different concept.

---

## F. Required source audit

### WaitNode::resolve_

**Location:** `wait_node.hpp:237-247`

**Classification:** **IMMUTABLE IRREVERSIBLE COMMIT**. The CAS transitions `Registered -> {Woken, Cancelled, Expired}`. Once resolved, no rollback. This is the sole winner authority.

### WaitQueue registration/unlink

**Location:** `wait_queue.hpp:174-186` (register_wait_locked), `wait_queue.hpp:302-317` (unlink_locked)

**Classification:**
- `register_wait_locked`: **REVERSIBLE RESERVATION** (can be undone by unlink before resolve CAS).
- `unlink_locked`: structural removal, must follow a winning resolve_.
- Intrusive FIFO list under `mtx_`. Registration is a structural mutation, not a terminal state.

### Scheduler global_mtx_ coordination

**Location:** `scheduler.cpp` throughout.

**Classification:** **OBSERVATION + SERIALIZATION**. All primitive operations acquire `global_mtx_`. This is the coordination domain for Select. A Select that holds `global_mtx_` while registering arms and picking a winner is serialized against all concurrent primitive operations.

### Event set/wait/cancel

**Location:** `event.hpp:103-172`, `scheduler.cpp:1319+` (event_set_broadcast)

**Classification:**
- `set()`: **IRREVERSIBLE COMMIT** (SET state + drain wake). Broadcast wakes are competing CAS on each node — each node's resolve_ is the true commit.
- `wait()` admission: **REVERSIBLE RESERVATION** (register before suspension) followed by either **IRREVERSIBLE COMMIT** (SET observed -> resolve Woken) or **OBSERVATION** (UNSET -> suspend).
- `cancel()`: best-effort for still-Registered nodes. **LOSER AUTHORITY** (cannot undo Woken).

### Semaphore release/acquire/cancel

**Location:** `semaphore.hpp:134-218`, `scheduler.cpp` (sem_release, sem_acquire, etc.)

**Classification:**
- `release()` transfer: **IRREVERSIBLE COMMIT** — `wake_one_locked` resolves the head immediately. `available_` unchanged.
- `release()` store: **IRREVERSIBLE COMMIT** — `available_++`.
- `acquire()` immediate: **IRREVERSIBLE COMMIT** — `available_--` + resolve Woken.
- `acquire()` suspend: **REVERSIBLE RESERVATION** (node registered but not yet resolved).
- `cancel()`: best-effort for still-Registered nodes.

### AsyncMutex handoff/lock/unlock/cancel

**Location:** `async_mutex.hpp:122-205`, `scheduler.cpp:2056-2097` (mutex_handoff_one_locked)

**Classification:**
- `unlock()` handoff: **IRREVERSIBLE COMMIT** — `owner_` assigned + make_runnable. Order: owner commit BEFORE publication (mandatory).
- `unlock()` no-waiter: owner_ = nullptr.
- `lock()` immediate: **IRREVERSIBLE COMMIT** — owner_ = current Fiber.
- `lock()` suspend: **REVERSIBLE RESERVATION** (node registered).
- `try_lock()`: **IRREVERSIBLE COMMIT** OR no-op.

### AsyncCondition prepare/reacquire

**Location:** `condition.hpp:223-274`

**Classification:**
- `condition_wait_prepare`: **IRREVERSIBLE COMMIT** — releases Mutex (handoff or NoOwner). The Mutex release is irreversible.
- Condition node resolve: **IRREVERSIBLE COMMIT** — terminal outcome.
- Reacquire epoch: **IRREVERSIBLE COMMIT** — standard Mutex lock, untimed, non-cancellable.

### AsyncQueue direct handoff and payload ownership

**Location:** `queue_port.hpp` (reconciler), `async_queue.hpp`

**Classification:**
- `P6 ProducerGrantCommit`: **IRREVERSIBLE COMMIT** — item enters ring, consumer operation gets payload. Visible to third party.
- `C6 ConsumerGrantCommit`: **IRREVERSIBLE COMMIT** — item exits ring, consumer's typed result holds payload.
- Suspended wait: **REVERSIBLE RESERVATION** (node in FIFO, no payload committed yet).
- Cancel: not available (D4 deferred).

### TimerRegistration retire/consume

**Location:** `timer_registration.hpp:112-130`

**Classification:**
- `retire()`: **REVERSIBLE RESERVATION** (CAS ACTIVE->RETIRED). Can lose to concurrent consume.
- `try_claim_expiry()`: **IRREVERSIBLE COMMIT** (CAS ACTIVE->CONSUMED). Winner claims timer authority.
- I4 closure: after RETIRED, node_/queue_ are never dereferenced.

### route_runnable_locked / make_waiting / make_runnable

**Location:** `scheduler.cpp:910-930` (route_runnable_locked), `scheduler.cpp:972-988` (await path)

**Classification:**
- `make_waiting()`: **TERMINAL STATE PUBLICATION** — Fiber state -> Waiting. Published to scheduler routing tables.
- `make_runnable()`: **EXACTLY-ONCE PUBLICATION GUARD** — returns false if already runnable or running.
- `route_runnable_locked()`: **CALLER RUNNABLE PUBLICATION** — pushes to local_runnable or pending_spawn_ + signal_wake. Only called after make_runnable returns true.

---

## G. Required conclusions

### 1. Most dangerous wrong designs

**Design A: "Register all arms, let primitives race normally, compensate losers" (P3).**
This is the most dangerous design because it appears workable at first glance (primitives already resolve independently) but is fundamentally unsound. Compensation is impossible for Mutex ownership, Queue payload transfer, and Semaphore permit consumption. The formal model would immediately produce counterexamples (NEG-M5 GrantWithoutOwnerCommit, NEG-SEM-2 ReleaseLoss). Any implementation based on this will have semantic bugs that cannot be fixed — the resources are gone, and there is no mechanism to recreate them.

**Design B: "Condition as a direct Select arm with special handling."**
The two-epoch protocol (C-H5 non-cancellable reacquire) makes this unsound even with special handling. The mandatory reacquire would either block Select's return (making it not a true select) or be preemptible (violating C-H5). There is no clean way to split a Condition wait across a Select boundary.

**Design C: "Cross-Scheduler Select using wake handles."**
Using `SchedulerWakeHandle` or similar cross-Scheduler wake to coordinate Select arms on different Schedulers would require distributed winner selection across lock domains. There is no ordering protocol between two `global_mtx_` instances. This would either require a third coordination lock (contention bottleneck) or admit races where both Schedulers think their arm won — violating exactly-once publication.

### 2. Recommended winner linearization point

**The group `resolve_` CAS under `global_mtx_`.**

This is a single atomic CAS (or equivalent mutex-protected flag) that transitions the Select group from `OPEN` to `WON_BY(arm_index)`. It runs after all arms are registered but BEFORE any arm's primitive resource can be committed. All arms are registered but not yet resolved (they remain in `Registered` state in their respective primitive queues). The linearization point is the instant the group CAS succeeds.

For arms that were already ready at registration time (Event SET, Semaphore permit available), the registration path must NOT immediately resolve Woken — it must check the group authority first. This is the key change required from the current primitive behavior.

### 3. Recommended first scope

**S1: Event + Timer.** These two primitives have no resource commitment beyond the WaitNode outcome. They can be implemented without any primitive internal refactoring. The Select implementation registers both arms, suspends, and whichever resolves first wins. Timer retirement handles cleanup of the loser.

### 4. Queue must be deferred

**YES.** Queue introduces:
- Typed payload ownership (C3, C4)
- Third-party visible side effects (C4)
- No cancel in v1 (D4)
- Complex reconciler path

Queue deferral is mandatory for first scope. Queue is authorized for S4 (post-foundation).

### 5. AsyncCondition must be deferred

**YES, unconditionally.** The two-epoch protocol with mandatory non-cancellable Mutex reacquire (C-H5) makes AsyncCondition fundamentally incompatible with Select semantics. There is no known design that makes Condition work as a Select arm without either:
- Blocking until the Condition wait fully completes (defeating Select)
- Breaking the C-H5 reacquire contract

### 6. Must restrict to one Scheduler

**YES.** Cross-Scheduler coordination is explicitly out of scope (D10, inherited constraint). All Select arms must bind to the same Scheduler instance. The Scheduler's `global_mtx_` is the coordination domain. Cross-Scheduler Select would require distributed consensus across lock domains, which is not supported by the existing infrastructure.

### 7. New primitive internal seams required

**For S2+ (Semaphore):** A "group check" seam in the Semaphore release path. When `wake_one_locked` finds the FIFO head is Select-grouped, the release must defer the permit transfer instead of immediately resolving. This requires reading a group pointer from the WaitNode (not currently present).

**For S3+ (AsyncMutex):** Same for Mutex handoff. `mutex_handoff_one_locked` must check if the head is Select-grouped before committing `owner_`.

**For S4 (Queue):** Queue reconciler must check group authority before executing `ProducerGrantCommit` or `ConsumerGrantCommit`. This is the most complex seam.

**For S5 (Condition):** Not applicable — Condition is rejected.

### 8. Cannot fully reuse WaitNode

**WaitNode alone is insufficient.** WaitNode provides single-epoch, single-queue lifecycle. Select needs:
- A group identity linking multiple WaitNodes to one Select invocation.
- A group state (OPEN / WON_BY(index) / CLOSED).
- A per-arm "participating in Select" flag that the primitive release/handoff path can check.

**Two approaches:**
- **Extend WaitNode with a group pointer** (`WaitNode::select_group_`). The primitive release path checks this pointer. If non-null, defers to group authority. This pollutes the clean E10 WaitNode abstraction with Select-specific state, which E10 explicitly forbids (no `select_group`, no `multi_wait_parent`).
- **Introduce a new `SelectArm` object** that wraps the WaitNode and is stored as `WaitNode::user_`. The primitive release path reads `user_` to check if it's a Select arm. This reuses the existing `void* user_` hook (currently used by E12-E Queue).

**Recommendation:** Use the `WaitNode::user_` hook with a new `SelectArm` structure. This does not require changing `WaitNode`'s state machine or adding new fields. The primitive release paths would check `node->user()` and, if it points to a `SelectArm` structure in the right state, defer to group coordination. This is the minimal change.

### 9. Formal model minimal state

The simplest formal model for S1 (Event + Timer) needs:

```text
State:
  groupState: {Idle, Open, WonBy(Event), WonBy(Timer), CleanedUp}
  eventState: Event model state (set_, queue, nodeState)
  timerState: {Active, Retired, Consumed}
  nodeState[2]: {Detached, Registered, Woken, Cancelled, Expired}
  publicationCount: Nat

Actions:
  RegisterEvent   -- register node_E in Event queue
  RegisterTimer   -- create TimerRegistration for node_T
  GroupCAS        -- linearization point: Open -> WonBy(E|T)
  EventSet        -- Event set() resolves node_E (only if group already picked E)
  TimerExpire     -- timer expiry resolves node_T (only if group already picked T)
  CleanupLoser    -- cancel/retire the losing arm
  Return          -- return to caller

Invariants:
  groupState = WonBy(E) => node_E = Woken /\ (node_T cancelled or retired)
  groupState = WonBy(T) => node_T = Expired /\ (node_E cancelled or retired)
  publicationCount <= 1   -- exactly-once runnable publication
  node_E and node_T never both terminal with different group outcomes
```

For S2 (Semaphore addition), add Semaphore state and the `available_` conservation law:

```text
Semaphore state:
  available: permit_count_t
  acquiredCount: permit_count_t (ghost)
  
Invariants:
  available + acquiredCount == initialPermits + acceptedReleaseCount
  Group won by Event => available_ unchanged from arm registration
  Group won by Semaphore => available_-- AND acquiredCount_++ (one permit consumed)
```

The critical negative model: Semaphore release resolves the arm BEFORE group CAS -> available unchanged while permit logically consumed. This should produce a counterexample on `InvPermitConservation`.

### 10. Production implementation blocking decisions

The following decisions must be resolved before production implementation:

| Decision | Question | Must close before |
| -------- | -------- | ----------------- |
| D-E13-1 | Group coordination protocol: central CAS under global_mtx_ | Any implementation |
| D-E13-2 | SelectArm representation: WaitNode::user_ hook or new field | Any implementation |
| D-E13-3 | Primitive seam for Semaphore: how to defer permit transfer | S2 scope |
| D-E13-4 | Primitive seam for AsyncMutex: how to defer owner handoff | S3 scope |
| D-E13-5 | Partial registration failure: atomic rollback or best-effort cancel | S1 implementation |
| D-E13-6 | Two-ready-arms policy: arm-index order or explicit priority | S1 implementation |
| D-E13-7 | TimerRegistration group lifetime: does group outlive timer? | S1 implementation |
| D-E13-8 | Select public API: function signature, WaitNode count, result type | Any implementation |
| D-E13-9 | Scope decision: S1 (Event+Timer) vs wait for S2 | First implementation |
| D-E13-10 | Queue integration authority: new seam design | S4 preparation |
| D-E13-11 | Condition: confirm reject or design exception case | Before formal model |
| D-E13-12 | Formal model: TLA+ scope matching production scope | Before production |

---

## H. Output

This report is written to:

```text
docs/reviews/E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1.md
```

No files outside this path were created or modified.

---

## I. Final status

```text
E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1:
COMPLETE

DESIGN AUTHORIZATION:
NONE

FORMAL MODEL AUTHORIZATION:
NONE

PRODUCTION IMPLEMENTATION:
DENIED

NEXT ACTION:
COMPARE THIS REPORT AGAINST THE PRIMARY PREPARATION DESIGN
IN AN INDEPENDENT ADVERSARIAL REVIEW
```
