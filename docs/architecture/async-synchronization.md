# Async Synchronization Architecture

**Status:** Current
**Authority:** Architecture
**Scope:** `sluice_async` — WaitNode / WaitQueue, deadline/timer, Event, Semaphore, AsyncMutex, AsyncCondition, AsyncQueue, AsyncRwLock, Select.

The async synchronization primitives share a common substrate: the
`WaitNode` / `WaitQueue` pair (E10) with deadline/timer integration (E11). Each
primitive maps its blocking wait onto one or more wait epochs, and all share the
same terminal-outcome and publication laws.

## WaitNode / WaitQueue (E10)

The canonical wait lifecycle:

- **`WaitNode`** — caller-owned, address-stable, non-copyable, non-movable. One
  fresh `WaitNode` per wait epoch. The caller provides it to blocking operations
  and queries `node.outcome()` after resume.
- **`WaitQueue`** — Scheduler-integrated runtime substrate. Exposes no public
  registration or resolution method; those structural methods are private and
  `Scheduler` is the sole friend.

`WaitOutcome` is a four-value enum:

| Value | Meaning |
|-------|---------|
| `unresolved` | Not yet terminal (the only non-terminal value) |
| `woken` | Resolved by wake (RESOURCE_WAKE) |
| `cancelled` | Resolved by wait-epoch cancellation (CANCEL) |
| `expired` | Resolved by deadline expiry (TIMER_EXPIRE, E11) |

Terminal outcomes are absorbing — once terminal, the value does not change.

**Laws:**

- A wait epoch has exactly one terminal outcome and at most one runnable
  publication.
- Fresh-per-epoch is enforced by the absorbing `WaitNode` state machine and the
  registration precondition that registration succeeds only from `Detached`.
- Queue-identity-safe cancellation: `cancel(WaitNode&)` resolves exactly one
  registered wait epoch. NOT task/Fiber/I/O cancellation.

## Deadline / Timer (E11)

- `Scheduler::deadline_t` = `uint64_t` monotonic ticks. `expired iff now >= deadline`.
- `TimerRegistration` state machine: `active` / `retired` / `consumed`.
- Already-due deadline at admission time: all primitives resolve inline without
  suspending.
- **Admission precedence** — resource readiness checked BEFORE already-due
  deadline (resource-first), except `AsyncCondition` which uses deadline-first
  (already-due → Expired inline).

## Event (E12-A)

Persistent manual-reset async Event. Non-copyable, non-movable.

- `set()` — broadcast to all registered waiters; ext-thread safe.
- `reset()` — does NOT cancel waiters.
- `wait(WaitNode&)` — Fiber-only; suspend until SET or cancel.
- `wait_until(WaitNode&, deadline)` — Fiber-only; deadline variant.
- `cancel(WaitNode&)` — per-wait-epoch cancel; any thread.

## Semaphore (E12-B)

Async counting Semaphore. Non-copyable, non-movable.

- `try_acquire()` — no barging; any thread.
- `acquire(WaitNode&)` / `acquire_until(WaitNode&, deadline)` — Fiber-only.
- `release()` — transfer/store/overflow; ext-thread safe.
- `cancel(WaitNode&)` — per-wait-epoch cancel; any thread.

## AsyncMutex (E12-C)

Fiber-suspending async Mutex. Ownership is `Fiber*` identity (survives E8 work
stealing). Non-copyable, non-movable.

- `try_lock()` — Fiber-only; recursive → false.
- `lock(WaitNode&)` / `lock_until(WaitNode&, deadline)` — Fiber-only.
- `unlock()` — Fiber-only; must be owner.
- `cancel(WaitNode&)` — per-wait-epoch cancel; any thread.

The internal `sluice::async::Mutex` (Clang-TSA-annotated) is a separate type
used by the Scheduler itself. Its acquisition failure contract is fail-fast
(`std::terminate`) — documented in `docs/history/implementation-plans/async-mutex-nothrow-authority.md`
(now at `docs/history/implementation-plans/async-mutex-nothrow-authority.md`).

## AsyncCondition (E12-D)

Fiber-suspending async condition variable. Bound to one `AsyncMutex` at
construction. Non-copyable, non-movable.

Two-epoch protocol: Condition epoch + mandatory Mutex reacquire.

- `wait(WaitNode&)` / `wait_until(WaitNode&, deadline)` — Fiber-only; must own Mutex.
- `notify_one()` — any thread; non-persistent.
- `notify_all()` — any thread; atomic snapshot-drain.
- `cancel(WaitNode&)` — per-Condition-epoch cancel; any thread.

## AsyncQueue\<T\> (E12-E)

Bounded MPMC FIFO channel. Non-copyable, non-movable. `T` must be an object
type, nothrow-move-constructible, and nothrow-destructible.

- `try_push(T)` / `try_pop()` — fast paths (no suspend).
- `push(T)` / `pop()` / `push_until(T, deadline)` / `pop_until(deadline)` — Fiber-only.
- `close()` — idempotent, monotonic Open→Closed.
- `begin_teardown()` / `release_teardown()` — irreversible teardown.

`AsyncQueue<T>` v1 has **no public wait-epoch cancellation API**. `close()` and
deadline expiry are distinct Queue state-machine causes (`closed` / `expired`
statuses), not cancellation.

Result types: `QueuePushResult<T>` (`committed` / `failed`) and
`QueuePopResult<T>` (`item` / `closed` / `expired` / `would_block`).

## AsyncRwLock (E12-F)

Fiber-suspending async Read-Write Lock with writer-fair phase-batched
scheduling. Non-copyable, non-movable. Multiple concurrent readers OR one
exclusive writer.

Writer-fair policy: new readers cannot barge past queued writers. When the queue
head is a reader, the maximal consecutive reader prefix is granted as one batch.

- `try_read_lock()` — any thread; fails if writer active or queue non-empty.
- `read_lock(WaitNode&)` / `read_lock_until(WaitNode&, deadline)` — Fiber-only.
- `try_write_lock()` — Fiber-only; recursive → false.
- `write_lock(WaitNode&)` / `write_lock_until(WaitNode&, deadline)` — Fiber-only.
- `unlock_read()` — any thread; caller must hold read share.
- `unlock_write()` — Fiber-only; must be writer owner.
- `cancel(WaitNode&)` — per-wait-epoch cancel; any thread.

## Select (E13)

Multi-arm Event/Timer select. The public entry is the variadic
`select(Arm&&...)` template, which forwards to `Scheduler::select_admit`.

- **Arms** — Event arms (level-triggered, persistent) and Timer arms
  (one-shot deadlines).
- **Admission** — centralized in `Scheduler::select_admit`. If any arm is
  already ready, the inline-ready path resolves immediately. Otherwise the
  Fiber suspends and is resumed when any arm resolves.
- **Winner/loser finalization** — first-claim-wins; losers are finalized
  without publication.
- **Rollback** — registration-failure rolls back all prior registrations in the
  same group.
- **Multi-worker** — owner routing + external-thread Event set + exactly-one
  runnable publication.

`SelectResult` carries the winning arm index, kind (`event` / `timer`), and
timer outcome (`fired`).

## Shared terminal / publication laws

All primitives share these invariants:

| Law | Description |
|-----|-------------|
| Exactly-once terminal outcome | A wait epoch resolves to exactly one of `woken` / `cancelled` / `expired`. |
| At-most-one runnable publication | A terminal winner publishes at most one runnable; losers do not publish. |
| Absorbing terminal state | Once terminal, `WaitOutcome` does not change. |
| FIFO waiter selection | Waiters are selected in FIFO registration order (does not guarantee strict completion order). |
| No barging | `try_*` operations fail if a queued waiter has FIFO priority. |
| Resource-first admission | Resource readiness checked before already-due deadline (except AsyncCondition: deadline-first). |
| Destroy requires quiescence | Destructors assert waiters empty (debug); destruction with live waiters is a contract violation. |

## Thread calling boundaries

| Operation Class | Examples | Requires Fiber | Safe from Ext Thread |
|----------------|----------|---------------|---------------------|
| Blocking/timed wait | `wait`, `acquire`, `lock`, `push`, `pop` | Yes | No |
| Non-blocking try | `try_acquire`, `try_push`, `try_pop` | No | Yes (except `try_write_lock` — Fiber-only) |
| Wake/notify | `set`, `release`, `notify_one`, `notify_all` | No | Yes |
| Cancel | `cancel` (all primitives with cancel) | No | Yes |
| Observation | `is_set`, `available`, `is_closed`, `capacity`, `size` | No | Yes |
| Construction/destruction | ctors, dtors | No | Yes (constructors); destruction requires quiescence |

## References

- ADR-execution-model.md — the accepted execution-strategy contract.
- `docs/architecture/async-runtime.md` — the Scheduler and Fiber layer.
- `docs/architecture/async-io-foundation.md` — Completion / AsyncIoContext / backends.
- `spec/tla/` — per-subsystem TLA+ models (E10 WaitNode, E11 Timer, E12 primitives, E13 Select).
