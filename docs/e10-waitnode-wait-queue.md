# E10 — WaitNode and Cancellation-Safe Wait Queue

As-built authoritative documentation for the E10 wait primitive
(sluice-CORE-E10). Production evidence: `include/sluice/async/wait_node.hpp`,
`include/sluice/async/wait_queue.hpp`, `src/async/scheduler.cpp`. Formal model:
`docs/spec/e10_waitnode/`.

> **E10 wait cancellation is not task cancellation and is not I/O operation
> cancellation.** Cancel here means only: resolve THIS registered wait with the
> Cancelled terminal outcome. It does not cancel the task, the fiber, the
> runtime, the underlying I/O operation, or propagate through a task tree.

---

## 1. Scope

E10 establishes the minimal cancellation-safe waiting primitive required by
later deadline/timer integration, async synchronization primitives, and
multi-wait/select.

### Implemented (IN scope)

```text
WaitNode             — one canonical wait lifecycle
WaitQueue            — one cancellation-safe wait queue protocol
single-wait registration
wake-vs-cancel winner protocol (one authority)
safe unlink/removal (bound to the winner)
single canonical terminal transition seam
runtime/fiber wake integration (Scheduler)
tests + formal model + this document
```

### Explicitly excluded (NOT implemented)

```text
timers, deadline scheduling, sleep APIs
mutex, semaphore, condition variable, event primitive, channel
select, multi-wait, wait-any, wait-all
I/O backend cancellation, io_uring cancellation
task cancellation propagation, structured concurrency
priority waiting, configurable fairness policy
```

WaitNode contains NO future-specific state (no `timer_id`, `deadline`,
`select_group`, `multi_wait_parent`, `mutex_owner`, `semaphore_count`,
`condition_variable pointer`, I/O opcode, io_uring user_data, or backend
completion object). Future extensibility is not a valid reason to add any.

---

## 2. WaitNode lifecycle

```
Detached ──register──> Registered ─┬─resolve(Woken)─────> Woken      [terminal]
                                   └─resolve(Cancelled)─> Cancelled [terminal]
```

- `Detached`: initial; not linked in any queue. `register_()` moves to
  `Registered`.
- `Registered`: linked in exactly one WaitQueue; resolvable.
- `Woken` / `Cancelled`: absorbing terminal states. The winner unlinks the
  node, but the terminal state is kept forever so `outcome()` is queryable.

The winner transition is `WaitNode::resolve_(outcome)` =
`compare_exchange_strong(state_, Registered, outcome, acq_rel, acquire)`. It
returns true **only** for the unique winner. Every loser returns false.

### Memory ordering (§9)

- `register_`: `acq_rel` (publishes Registered + membership).
- `resolve_`: `acq_rel` on success (release publishes the terminal outcome to
  every later `outcome()`/`is_terminal()` acquire); `acquire` on failure (the
  loser reloads the winner's outcome).
- No blanket `seq_cst` — acq_rel is the simplest proven ordering, matching the
  repository's established style (`Fiber::state_`, `Completion` state).

---

## 3. Ownership table

| field/state | lifetime owner | mutation authority | sync mechanism | destruction precondition |
|---|---|---|---|---|
| WaitNode lifetime | the calling fiber/task (caller-owned) | caller | n/a | terminal (winner unlinked it), or never registered |
| `WaitNode::state_` | the node | `resolve_` CAS (winner); `register_` (queue op) | `std::atomic` acq_rel | terminal before destroy |
| link fields (`next_/prev_/home_`) | the node | `WaitQueue` under `mtx_` | `WaitQueue::mtx_` | unlinked (winner did it) |
| queue membership | `WaitQueue` | `WaitQueue` ops under `mtx_` | `mtx_` | empty or cancel-all on destroy |
| Fiber handle (`fiber_`) | the node; set at register | immutable after register | none | n/a |

**§3 questions, answered:**

1. Can a WaitNode outlive its waiting fiber/task? **No** — it is a local in the
   await frame; outliving the fiber is a caller contract violation.
2. Can a queue retain a pointer after wait completion? **No** — the winner
   unlinks before/atomically with the resolve CAS, in the same critical section.
3. Who removes the node from the queue? **The winner transition** (wake_one /
   cancel winner).
4. Does the winner remove it? **Yes**, in the same critical section as its CAS.
5. Can a loser observe a still-linked terminal node? **No** — the loser's CAS
   fails; it never reaches the unlink. By the time a loser observes the node,
   the winner has already unlinked it (same critical section, same lock).
6. Can cancellation race queue traversal? **No** — `cancel` takes `q.mtx()`;
   the winner's unlink and the loser's failed CAS both occur under it.
7. Can wake race node destruction? **No** — the fiber must not destroy a
   Registered node (`~WaitNode` asserts `!is_registered()`); the winner
   resolves+unlinks it first.
8. Can queue destruction race registered waiters? **The queue destructor
   asserts empty in debug** (§10); the caller must drain (cancel-all) first.

---

## 4. Registration protocol (lost-wake analysis)

`Scheduler::await_wait(q, node)` mirrors the proven `await_ready_flag` idiom
(commit `422036c` "close await lost-wake window"):

```text
under global_mtx_ + q.mtx():
    register node into q              (Detached -> Registered)
    if already terminal: undo, return (defense-in-depth recheck)
    make_waiting()                    (atomic w.r.t. the wake path)
release locks
context_switch out
```

The register + recheck + make_waiting is **one atomic transition with respect
to the wake path** (`wake_wait_one` / `cancel_wait` run under `global_mtx_`).
Only `context_switch` is outside the lock.

**Why the queue protocol does not create a wake-before-suspend loss (§4):**
E10 does not implement arbitrary condition predicates. The queue itself is
resolvable only by an explicit `wake_wait_one(q)` call, and that call takes
`global_mtx_` + `q.mtx()` — the same locks the suspending fiber holds during
register+make_waiting. So a wake that "happened" before the fiber suspended
either (a) found the queue empty (no wake delivered) and the fiber registers
normally, or (b) ran under the lock the fiber now holds — impossible
concurrently. The condition→wait race is the **caller's** responsibility
(check the condition before `await_wait`); the queue protocol itself is closed.

---

## 5. WaitQueue protocol

- **Intrusive** doubly-linked list (node identity = address; non-movable, like
  `Completion<T>` L7).
- **FIFO** (matches existing `local_runnable` / `pending_spawn_` discipline; no
  configurable policy).

Operations: `register_wait[_locked]`, `wake_one[_locked]`, `cancel[_locked]`,
`cancel_all`, `unlink_locked`, `empty_locked`.

**Intrusive-link invariants (proven, §5):**
- linked-at-most-once: `register_` only succeeds from `Detached`.
- double-unlink impossible: `unlink_locked` runs only on a winning CAS, under
  `mtx_`, exactly once.
- terminal node not indefinitely reachable: the winner unlinks it immediately.
- destroyed node never linked: `~WaitNode` asserts `!is_registered()`.

---

## 6. Winner and linearization protocol

**One canonical terminal resolver seam:** `resolve_(outcome)`.

The winner is determined by the CAS; the linearization point (§7) is the
instant the CAS stores the terminal value. At that instant the node is
(a) terminally resolved, (b) the unique winner, (c) the unique unlink owner.

**Wake vs cancellation truth table (§6):**

| Wake | Cancel | Required outcome | E10 behavior |
|---|---|---|---|
| no | no | remains waiting | Registered; fiber parked |
| wins | loses | one wake/resume, Woken | `wake_one` CAS succeeds, routes; `cancel` CAS fails |
| loses | wins | one resume, Cancelled | `cancel` CAS succeeds, routes; `wake` returns null |
| concurrent | concurrent | one winner, valid state | `mtx_` serializes; one CAS wins |
| repeated wake | none | no duplicate enqueue | 2nd `wake_one` finds empty queue / CAS fails |
| none | repeated cancel | no duplicate completion | 2nd `cancel` CAS fails |
| wake after cancel | cancel already won | wake is loser/no-op | `wake_one` returns null |
| cancel after wake | wake already won | cancel is loser/no-op | `cancel` returns false |

Cancellation **resumes the waiting execution** (explicit, tested choice — C10b):
a cancelled wait is routed through the canonical wake seam, so it is never
permanently suspended.

---

## 7. Unlink and destruction semantics

**Unlink Law (§7):** queue removal is NOT an independent protocol. There is
ONE unlink path — `WaitQueue::unlink_locked`, called only by a winning
resolver (wake_one / cancel / cancel_all), in the same `q.mtx()` critical
section as its winning CAS. There is no separate wake-unlink, cancel-unlink, or
destructor-unlink.

**Exactly-once unlink:** the winner CAS succeeds for exactly one caller; that
caller unlinks exactly once. A loser returns before `unlink_locked`.

**Destruction preconditions (§10):**
- `~WaitNode`: asserts `!is_registered()` (debug). A terminal or never-
  registered node destroys cleanly; a Registered node asserts.
- `~WaitQueue`: asserts empty (debug). The caller must drain (cancel-all)
  first. The Scheduler does NOT auto-cancel-all on run termination (matching
  E9's treatment of `waiting_ready_`: a stranded MW-S3 wait is left for the
  caller to resolve or re-enter `run()`).

---

## 8. Scheduler / E7-E9 compatibility

E10 reuses the existing scheduler wake/runnable ownership and does NOT
redefine fiber runnable ownership, worker ownership, steal, RunMode, or Drain.

- Winner routes through `route_runnable_locked` (the canonical seam), guarded
  by E7-T2 `make_runnable()` (exactly-once publication).
- `waiting_waitq_count_` is counted by `classify_locked` exactly like the
  other wait maps, so MW-S3 (unresolved waits) is correct.
- No direct `local_runnable` push from `WaitQueue`; all routing goes through
  the scheduler seam.
- Lock order: `global_mtx_` → `q.mtx()` → `wake_mtx_` (via
  `route_runnable_locked` → `signal_wake_locked`). Consistent with the existing
  `global_mtx_` → `inbox_mtx_` → `wake_mtx_` order.

**Drain interaction (§10, C11):** a registered wait and its resolution do NOT
revive the E9 Drain hang. In Drain mode, MW-S3 with an unresolved E10 wait
returns STALLED exactly as E9 does (the run terminates; the wait is left for
the caller). E10 does NOT redefine wake capability.

---

## 9. Cancellation boundary

E10 cancellation = `node.resolve_(Cancelled)` only. It is:

- NOT task cancellation
- NOT fiber cancellation
- NOT runtime cancellation
- NOT I/O operation cancellation
- NOT task-tree cancellation propagation

The APIs are named `cancel_wait` / `cancel_locked` / `was_cancelled` to make
this boundary explicit.

---

## 10. Future boundaries (E11/E12/E13)

E10 is the foundation. It does NOT pre-implement:

- **E11** — Deadline / Timer wait integration (will compose a deadline with a
  WaitNode; the timer expiry is a third resolver that uses the SAME
  `resolve_(outcome)` authority — no new winner protocol).
- **E12+** — Async synchronization primitives (mutex/semaphore/condvar) built
  atop WaitQueue.
- **E13+** — Multi-wait / select (a node may participate in multiple queues;
  this is deferred and requires E10's single-winner authority to remain the
  resolution mechanism).

No speculative fields for these phases exist in WaitNode.

---

## 11. Test counterexamples

See `tests/e10_wait_queue_test.cpp` (C1-C9, C12 — pure protocol) and
`tests/e10_scheduler_wait_test.cpp` (C10, C11 — scheduler integration). Each
test documents the production property it protects.
