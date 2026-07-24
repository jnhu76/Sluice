# E10–E12 Async Synchronization API & Semantic Closure

**Task**: `E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1`

**Status**: `CORRECTIVE-2 READY — AUTHOR SELF-ASSESSMENT; INDEPENDENT RE-REVIEW REQUIRED`

**Current Status**: `CLOSED — E12-G FINAL INDEPENDENT REVIEW PASS`

> **Final authorization — 2026-07-19.** The Corrective-2 author self-assessment
> recorded in the `**Status**` line above and in §14 below remains preserved
> verbatim as the historical stage verdict at PR #13 merge time. Subsequent
> independent reviews for E12-B Semaphore, E12-C AsyncMutex (migration/data-race
> micro-review), E12-D AsyncCondition, and the E12-G cross-primitive semantic
> closure all completed successfully. The final authorization and governance
> effect are recorded in §15. The `**Status**` line is no longer the controlling
> current-status authority; `**Current Status**` and §15 are.

**Branch**: `audit/e10-e12-api-semantic-closure`

> **Corrective-2 (current revision).** An independent adversarial review found
> that Corrective-1 still overstated T23 registration, parity coverage,
> negative-compile automation, E12-C review status, Queue cancellation
> vocabulary, and parts of the public API inventory. Corrective-2 replaces the
> test-only registration protocol, adds an automated negative-compile gate,
> and reconciles the affected documentation. It does not independently approve
> its own corrective; semantic closure remains denied pending re-review.
>
> **Corrective-1 (superseded claims).** Applied C1–C7 from
> `E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-CORRECTIVE-1`: closes the Event
> T23/CANCEL-2a ASan flake deterministically (no Event production-code
> change), corrects the E13 cancellation contract, repairs stale residual
> risks R2/R5 and the E12-B/C review-status contradiction, corrects the
> WaitNode "type system" wording, removes `close()`/deadline-as-cancellation
> vocabulary for Queue, fixes the `WaitOutcome` classification and the
> `WaitQueue`/`TimerRegistration` substrate wording, and strengthens the D1
> rationale. The full Clang ASan matrix was re-run ×3 with **no exclusions**
> and was green each time. See §11.3, §11.4, §12, §14.
>
> **Fresh Corrective-2 verification.** The reproducible matrix is now recorded
> in §11.3. A sandbox-external Clang ASan+LSan run completed all 15 configured
> E10/E11/E12 binaries three consecutive times (45/45, no filters or target
> exclusions). Earlier diagnostic attempts, including one baseline-matched T16
> `DEADLYSIGNAL`, remain recorded rather than being overwritten by the green
> rerun. Four formal gates exit 0; E11 and Event exit 1 because this TLC build
> emits an unnamed temporal counterexample for the two liveness negative
> models. Matched baseline runs have the same result and the counterexamples
> are non-vacuous; see §11.3 and the Corrective-2 review artifact.

---

## 1. Baseline

```text
BASE_BRANCH:         master
BASE_COMMIT:         d0cd915159a49ee30e88b0fdaec04a7b78260af1
INITIAL_REVIEW_HEAD: a97aca1bb9248a2c5bc05d914ba8670de590ed34
HEAD_BRANCH:         audit/e10-e12-api-semantic-closure
PREEXISTING_UNTRACKED: tests/test_t3_simple.cpp, tla2tools.jar
```

---

## 2. Public API Inventory Matrix

All signatures extracted from installed headers (`include/sluice/async/`). Evidence is `file:line`.

### 2.1 E10: WaitNode (wait_node.hpp)

| Method | Signature | noexcept | nodiscard | Fiber | Ext Thread | Suspend | Notes |
|--------|-----------|----------|-----------|-------|------------|---------|-------|
| ctor | `WaitNode() noexcept` (L125) | yes | — | — | yes | no | default |
| ctor | `explicit WaitNode(Fiber*) noexcept` (L130) | yes | — | — | yes | no | |
| dtor | `~WaitNode()` (L136) | no | — | — | — | no | `assert(!is_registered())` |
| is_registered | `bool is_registered() const noexcept` (L160) | yes | — | — | yes | no | acquire load |
| is_terminal | `bool is_terminal() const noexcept` (L163) | yes | — | — | yes | no | acquire load |
| outcome | `WaitOutcome outcome() const noexcept` (L169) | yes | — | — | yes | no | acquire load |
| was_woken | `bool was_woken() const noexcept` (L176) | yes | — | — | yes | no | acquire load |
| was_cancelled | `bool was_cancelled() const noexcept` (L179) | yes | — | — | yes | no | acquire load |
| was_expired | `bool was_expired() const noexcept` (L184) | yes | — | — | yes | no | E11; acquire load |
| fiber | `Fiber* fiber() const noexcept` (L189) | yes | — | — | yes | no | immutable |
| user | `void* user() const noexcept` (L150) | yes | — | — | yes | no | E12-E Queue ctx |
| set_user | `void set_user(void*) noexcept` (L151) | yes | — | — | yes | no | |

`next_`, `prev_`, and `home_` are also public data members in the installed
header. They exist for intrusive implementation access and are owned by
`WaitQueue` under its mutex; public visibility does not make user mutation a
supported operation. Copy and move construction/assignment are all deleted.

**WaitOutcome** (L81-99):
```cpp
enum class WaitOutcome : std::uint8_t {
    unresolved = 0,  // Not yet terminal
    woken = 1,       // Resolved by wake
    cancelled = 2,   // Resolved by cancellation
    expired = 3,     // Resolved by deadline expiry (E11)
};
```

**State machine** (L107-117): `Detached → Registered → {Woken, Cancelled, Expired} [T]`

**Lifecycle**: WaitNode is CALLER-OWNED, address-stable, one per wait epoch. Non-copyable, non-movable (identity = address). Fresh-per-epoch is enforced by the absorbing WaitNode state machine and the registration precondition that registration succeeds only from `Detached`; the deleted copy/move operations preserve object identity and address stability but do not by themselves prevent terminal-node reuse. Destroyed only when terminal (or never registered). Reuse is forbidden — terminal-state WaitNodes must not be re-registered (C1 finding in §6).

### 2.2 E10: WaitQueue (wait_queue.hpp)

WaitQueue is a publicly nameable installed-header type whose structural
authority is PRIVATE and SEALED. Its structural operations are reachable ONLY
through Scheduler (the sole friend). Its public surface is construction,
destruction, and deleted copy/move special members.

| Method | Signature | Notes |
|--------|-----------|-------|
| ctor | `WaitQueue() noexcept` (L121) | |
| dtor | `~WaitQueue()` (L132) | `assert(head_ == nullptr)` |
| (all structural ops) | PRIVATE | friend Scheduler only |

### 2.3 E11: TimerRegistration (timer_registration.hpp)

| Method | Signature | noexcept | Notes |
|--------|-----------|----------|-------|
| ctor | `TimerRegistration() = default` | — | |
| ctor | `TimerRegistration(WaitNode*, WaitQueue*, deadline_tick_t) noexcept` (L98) | yes | |
| try_claim_expiry | `bool try_claim_expiry() noexcept` (L112) | yes | CAS ACTIVE→CONSUMED |
| retire | `bool retire() noexcept` (L125) | yes | CAS ACTIVE→RETIRED |
| is_active | `bool is_active() const noexcept` (L134) | yes | |
| is_retired | `bool is_retired() const noexcept` (L137) | yes | |
| is_consumed | `bool is_consumed() const noexcept` (L140) | yes | |
| state | `State state() const noexcept` (L143) | yes | |
| node | `WaitNode* node() const noexcept` (L150) | yes | |
| queue | `WaitQueue* queue() const noexcept` (L151) | yes | |
| deadline | `deadline_tick_t deadline() const noexcept` (L152) | yes | |
| has_on_resolve | `bool has_on_resolve() const noexcept` (L158) | yes | E12-E Queue |
| fire_on_resolve_locked | `void fire_on_resolve_locked(bool) noexcept` (L159) | yes | |

The public type surface also includes `OnResolveFn`, public nested enum
`State { active, retired, consumed }`, deleted copy/move special members, and
public data member `std::size_t heap_index`. This visibility is documented even
though `TimerRegistration` is Scheduler-integrated substrate rather than a
standalone synchronization primitive.

**E11 Scheduler deadline API** (scheduler.hpp):
- `deadline_t monotonic_now() const noexcept` (L275)
- `void await_wait_deadline(WaitQueue&, WaitNode&, deadline_t)` (L305) — requires Fiber
- `bool expire_wait(WaitQueue&, WaitNode&)` (L315)
- `void advance_clock(deadline_t)` (L282) — TEST-ONLY

### 2.4 E12-A: Event (event.hpp)

| Method | Signature | Ret | noexcept | nodiscard | Fiber | Ext Thread | Suspend | Notes |
|--------|-----------|-----|----------|-----------|-------|------------|---------|-------|
| ctor | `explicit Event(Scheduler&, bool initially_set = false) noexcept` (L78) | — | yes | — | — | yes | no | |
| dtor | `~Event() = default` (L86) | — | — | — | — | — | no | waiters must be empty |
| is_set | `bool is_set() const noexcept` (L95) | bool | yes | yes | — | yes | no | lock-free acquire |
| set | `void set()` (L103) | void | no | — | — | yes | no | broadcast to all |
| reset | `void reset()` (L111) | void | no | — | — | yes | no | does NOT cancel |
| wait | `void wait(WaitNode&)` (L122) | void | no | — | yes | no | yes | result via node.outcome() |
| wait_until | `void wait_until(WaitNode&, deadline_t)` (L142) | void | no | — | yes | no | yes | result via node.outcome() |
| cancel | `bool cancel(WaitNode&)` (L170) | bool | no | yes | — | yes | no | queue-identity-gated |

**Result form**: `void` + `node.outcome()` returns `WaitOutcome` (woken/cancelled/expired).

**Deadline precedence** (L134-141): SET checked BEFORE already-due deadline:
- SET + already due → Woken
- UNSET + already due → Expired

### 2.5 E12-B: Semaphore (semaphore.hpp)

`using permit_count_t = std::uint32_t;` (L82)

| Method | Signature | Ret | noexcept | nodiscard | Fiber | Ext Thread | Suspend | Notes |
|--------|-----------|-----|----------|-----------|-------|------------|---------|-------|
| ctor | `Semaphore(Scheduler&, permit_count_t initial, permit_count_t max) noexcept` (L92) | — | yes | — | — | yes | no | `assert(max > 0)` |
| dtor | `~Semaphore() = default` (L108) | — | — | — | — | — | no | waiters must be empty |
| available | `permit_count_t available() const noexcept` (L120) | count | yes | yes | — | yes | no | lock-free obs only |
| try_acquire | `bool try_acquire()` (L134) | bool | no | yes | — | yes | no | no barging |
| acquire | `void acquire(WaitNode&)` (L150) | void | no | — | yes | no | yes | result via node.outcome() |
| acquire_until | `void acquire_until(WaitNode&, deadline_t)` (L173) | void | no | — | yes | no | yes | result via node.outcome() |
| cancel | `bool cancel(WaitNode&)` (L198) | bool | no | yes | — | yes | no | queue-identity-gated |
| release | `bool release()` (L217) | bool | no | yes | — | yes | no | transfer/store/overflow |

**Result form**: `void` + `node.outcome()`.

**Deadline precedence** (L160-168): permit admission BEFORE already-due deadline.

### 2.6 E12-C: AsyncMutex (async_mutex.hpp)

| Method | Signature | Ret | noexcept | nodiscard | Fiber | Ext Thread | Suspend | Notes |
|--------|-----------|-----|----------|-----------|-------|------------|---------|-------|
| ctor | `explicit AsyncMutex(Scheduler&) noexcept` (L91) | — | yes | — | — | yes | no | initially unlocked |
| dtor | `~AsyncMutex()` (L100) | — | no | — | — | — | no | `assert(owner_ == nullptr)` |
| try_lock | `bool try_lock()` (L122) | bool | no | yes | yes | no | no | recursive→false |
| lock | `void lock(WaitNode&)` (L139) | void | no | — | yes | no | yes | result via node.outcome() |
| lock_until | `void lock_until(WaitNode&, deadline_t)` (L162) | void | no | — | yes | no | yes | result via node.outcome() |
| cancel | `bool cancel(WaitNode&)` (L188) | bool | no | yes | — | yes | no | any OS thread |
| unlock | `void unlock()` (L203) | void | no | — | yes | no | no | must be owner |

**Result form**: `void` + `node.outcome()`.

**Direct handoff**: `owner_ = winner` BEFORE `make_runnable` + `route_runnable_locked` (L548-560).

**Friends**: `AsyncCondition` (solely to read `scheduler_` and pass `waiters_`/`owner_` by reference).

### 2.7 E12-D: AsyncCondition (condition.hpp)

| Method | Signature | Ret | noexcept | nodiscard | Fiber | Ext Thread | Suspend | Notes |
|--------|-----------|-----|----------|-----------|-------|------------|---------|-------|
| ctor | `explicit AsyncCondition(AsyncMutex&) noexcept` (L111) | — | yes | — | — | yes | no | derives Scheduler from mutex |
| dtor | `~AsyncCondition()` (L120) | — | no | — | — | — | no | `assert(active_waits_ == 0)` |
| wait | `WaitOutcome wait(WaitNode&)` (L223) | WaitOutcome | no | **yes** | yes | no | yes | must own bound Mutex |
| wait_until | `WaitOutcome wait_until(WaitNode&, deadline_t)` (L256) | WaitOutcome | no | **yes** | yes | no | yes | must own bound Mutex |
| cancel | `bool cancel(WaitNode&)` (L276) | bool | no | yes | — | yes | no | any OS thread |
| notify_one | `void notify_one()` (L283) | void | no | — | — | yes | no | |
| notify_all | `void notify_all()` (L289) | void | no | — | — | yes | no | atomic snapshot-drain |

**Result form**: `WaitOutcome` returned directly. Distinct from Event/Semaphore/Mutex `void` + `node.outcome()`.

**Two-epoch protocol** (L17-33): Condition epoch (Condition node registered on Condition queue) → Reacquire epoch (stack-local reacquire node, calls `mutex_.lock(reacquire_node)`). The reacquire is untimed and non-cancellable (C-H4/C-H5).

**Already-due inline** (L629): deadline already due at admission → Expired INLINE, Mutex NOT released, caller RETAINS ownership.

### 2.8 E12-E: AsyncQueue<T> (async_queue.hpp)

Template constraints (L218-222):
```cpp
static_assert(std::is_object_v<T>);
static_assert(std::is_nothrow_move_constructible_v<T>);
static_assert(std::is_nothrow_destructible_v<T>);
```

| Method | Signature | Ret | noexcept | nodiscard | Fiber | Ext Thread | Suspend | Notes |
|--------|-----------|-----|----------|-----------|-------|------------|---------|-------|
| ctor | `explicit AsyncQueue(Scheduler&, std::size_t capacity)` (L229) | — | no | — | — | yes | no | throws `std::invalid_argument` if cap==0 |
| dtor | `~AsyncQueue() = default` (L232) | — | — | — | — | — | no | ring must be empty |
| try_push | `QueuePushResult<T> try_push(T)` (L252) | typed | no | yes | — | yes | no | |
| try_pop | `QueuePopResult<T> try_pop()` (L263) | typed | no | yes | — | yes | no | |
| close | `void close() noexcept` (L272) | void | yes | — | — | yes | no | idempotent |
| is_closed | `bool is_closed() const noexcept` (L276) | bool | yes | yes | — | yes | no | lock-free |
| capacity | `std::size_t capacity() const noexcept` (L277) | size | yes | yes | — | yes | no | |
| size | `std::size_t size() const noexcept` (L280) | size | yes | yes | — | yes | no | |
| push | `QueuePushResult<T> push(T)` (L288) | typed | no | yes | yes | no | yes | |
| push_until | `QueuePushResult<T> push_until(T, deadline_t)` (L299) | typed | no | yes | yes | no | yes | |
| pop | `QueuePopResult<T> pop()` (L311) | typed | no | yes | yes | no | yes | |
| pop_until | `QueuePopResult<T> pop_until(deadline_t)` (L320) | typed | no | yes | yes | no | yes | |
| begin_teardown | `detail::QueueTeardownSession begin_teardown() noexcept` (L333) | session | yes | — | — | yes | no | irreversible |
| release_teardown | `T release_teardown(detail::QueueTeardownSession&) noexcept` (L342) | T | yes | — | — | yes | no | |

**Result types**:
- `QueuePushResult<T>`: status → `committed | closed | expired | would_block`; `take_value()` recovers T
- `QueuePopResult<T>`: status → `item | closed | expired | would_block`; `take_value()` recovers T
- Both hand-written `operator=` with destroy-and-rebuild (PR #12 corrective) — T need NOT be move-assignable
- Both expose a defaulted noexcept move constructor, noexcept move assignment,
  deleted copy construction/assignment, defaulted destructor, `status() const
  noexcept`, and `take_value() && noexcept`. `QueuePushResult` exposes
  `committed()`/`failed(status,T&&)`; `QueuePopResult` exposes
  `item(T&&)`/`closed()`/`expired()`/`would_block()`, all `noexcept`.
- `QueuePushResult<T>` and `QueuePopResult<T>` themselves declare no explicit
  `requires` clause or `static_assert`; the three `T` constraints above belong
  to `AsyncQueue<T>`. Their public operations are nevertheless declared
  `noexcept` exactly as shown.

**WaitNode visibility**: WaitNode is HIDDEN from the user. Internal to QueuePort (role queues).

**Cancellation**: `AsyncQueue<T>` v1 has **no public wait-epoch cancellation API and no `Cancelled` result**. `close()` and deadline expiry are distinct Queue state-machine causes (`closed` / `expired` statuses), not cancellation. There is no `cancel(WaitNode&)` on `AsyncQueue<T>`; per-wait-epoch cancellation is deferred by D4 to a future authority.

**Teardown** (L327-346): `begin_teardown()` →
`detail::QueueTeardownSession` (move constructor `noexcept`; move assignment
and both copy operations deleted; destructor `noexcept`; public
`take_next() noexcept` and `empty() const noexcept`). Although it lives in a
`detail` namespace, it is part of `AsyncQueue<T>`'s public signature.
`release_teardown(session)` recovers typed T from the drained ring.

---

## 3. Cross-Primitive Semantic Contract Matrix

Each cell marked with status and evidence.

### 3.1 Resource State

| Primitive | State | Status | Evidence |
|-----------|-------|--------|----------|
| Event | `std::atomic<bool> set_` (SET/UNSET) | PROVEN | `event.hpp:176` |
| Semaphore | `std::atomic<permit_count_t> available_` in [0, max] | PROVEN | `semaphore.hpp:223` |
| AsyncMutex | `Fiber* owner_` (nullptr = unlocked) | PROVEN | `async_mutex.hpp:222` |
| AsyncCondition | no persistent state (notify before wait is lost) | PROVEN | `condition.hpp:176` header |
| AsyncQueue<T> | ring of `capacity_` slots + `closed_` flag | PROVEN | `queue_port.hpp:403-416` |

### 3.2 Readiness Persistence

| Primitive | Persistence | Status | Evidence |
|-----------|-------------|--------|----------|
| Event | persistent (manual-reset); SET stays until reset() | PROVEN | `event.hpp:17-23` |
| Semaphore | permits stored; not consumed until acquisition | PROVEN | `semaphore.hpp:15-21` |
| AsyncMutex | ownership is persistent until unlock | PROVEN | `async_mutex.hpp:16-19` |
| AsyncCondition | NON-persistent; notify before wait is lost | PROVEN | `condition.hpp:173` |
| AsyncQueue<T> | buffered items persist until popped | PROVEN | `queue_port.hpp:403` ring |

### 3.3 Wake Cardinality

| Primitive | Cardinality | Status | Evidence |
|-----------|-------------|--------|----------|
| Event | set() broadcasts to ALL registered waiters | PROVEN | `event.hpp:103` |
| Semaphore | release() wakes exactly ONE FIFO head (or stores) | PROVEN | `semaphore.hpp:202-216` |
| AsyncMutex | unlock() hands off to exactly ONE FIFO head (or frees) | PROVEN | `async_mutex.hpp:192-205` |
| AsyncCondition | notify_one() wakes exactly ONE; notify_all() drains all | PROVEN | `condition.hpp:283-293` |
| AsyncQueue<T> | each push/pop wakes exactly ONE counter-role waiter | PROVEN | `queue_port.hpp:728-739` |

### 3.4 FIFO / No-Barging

| Primitive | FIFO Policy | Status | Evidence |
|-----------|-------------|--------|----------|
| Event | FIFO waiter selection (WaitQueue); set() broadcast all | PROVEN | `wait_queue.hpp:14` |
| Semaphore | FIFO waiter selection; try_acquire fails if queued waiter | PROVEN | `semaphore.hpp:134 header` |
| AsyncMutex | FIFO waiter selection; try_lock fails if queued waiter | PROVEN | `async_mutex.hpp:122 header` |
| AsyncCondition | FIFO Condition queue; reacquire FIFO-tail (no priority) | PROVEN | `condition.hpp:247` |
| AsyncQueue<T> | FIFO producer role + FIFO consumer role; FIFO ring | PROVEN | `queue_port.hpp:424` |

### 3.5 Fast Path

| Primitive | Fast Path | Status | Evidence |
|-----------|-----------|--------|----------|
| Event | SET at admission → Woken inline, no suspend | PROVEN | `event.hpp:119-121` |
| Semaphore | available > 0 + no queued waiter → Woken inline | PROVEN | `semaphore.hpp:140-148` |
| AsyncMutex | owner == nullptr + no queued waiter → Woken inline | PROVEN | `async_mutex.hpp:126-137` |
| AsyncCondition | already-due deadline → Expired inline, retain ownership | PROVEN | `condition.hpp:148-151` |
| AsyncQueue<T> | try_push/try_pop: ring slot available → immediate commit | PROVEN | `queue_port.hpp:378-379` |

### 3.6 Blocking Admission

| Primitive | Admission Closure | Status | Evidence |
|-----------|-------------------|--------|----------|
| Event | register + recheck SET + (if SET) resolve inline; else suspend | PROVEN | `event.hpp:122-124` |
| Semaphore | register + recheck permit + (if admissible) consume inline; else suspend | PROVEN | `semaphore.hpp:150-151` |
| AsyncMutex | register + recheck owner free + (if admissible) Woken inline; else suspend | PROVEN | `async_mutex.hpp:139-141` |
| AsyncCondition | register Condition node + release Mutex + make_waiting; then suspend | PROVEN | `condition.hpp:233-241` |
| AsyncQueue<T> | register role node + recheck ring + (if admissible) reconcile inline; else suspend | PROVEN | `queue_port.hpp:693-710` |

### 3.7 Already-Due Precedence

| Primitive | Precedence | Status | Evidence |
|-----------|------------|--------|----------|
| Event | SET readiness checked BEFORE already-due deadline | PROVEN | `event.hpp:134-141` |
| Semaphore | permit admission BEFORE already-due deadline | PROVEN | `semaphore.hpp:160-168` |
| AsyncMutex | ownership admission BEFORE already-due deadline | PROVEN | `async_mutex.hpp:148-157` |
| AsyncCondition | already-due → Expired inline, Mutex NOT released | PROVEN | `condition.hpp:148-151` |
| AsyncQueue<T> | resource-first admission wins over due deadline | PROVEN | `queue_port.hpp:706-710` |

### 3.8 Registered Race Causes

| Primitive | Competing Causes | Status | Evidence |
|-----------|-----------------|--------|----------|
| Event | RESOURCE_WAKE (set) / CANCEL (cancel) / TIMER_EXPIRE (deadline) | PROVEN | `event.hpp:116-118` |
| Semaphore | RESOURCE_WAKE (release) / CANCEL (cancel) / TIMER_EXPIRE (deadline) | PROVEN | `semaphore.hpp:155-158` |
| AsyncMutex | RESOURCE_WAKE (unlock handoff) / CANCEL (cancel) / TIMER_EXPIRE (deadline) | PROVEN | `async_mutex.hpp:143-147` |
| AsyncCondition | notify_one/notify_all / CANCEL (cancel) / TIMER_EXPIRE (deadline) | PROVEN | `condition.hpp:146-159` |
| AsyncQueue<T> | reconciler grant / deadline expiry / **CLOSE** (close() drains role FIFOs to `closed`); **no CANCEL cause in v1** — `close()` and deadline expiry are distinct Queue state-machine causes, not cancellation | PROVEN | `queue_port.hpp:680-683` |

### 3.9 Cancellation Scope

| Primitive | Scope | Status | Evidence |
|-----------|-------|--------|----------|
| Event | per-wait-epoch cancel; queue-identity-gated | PROVEN | `event.hpp:146-172` |
| Semaphore | per-wait-epoch cancel; queue-identity-gated | PROVEN | `semaphore.hpp:177-200` |
| AsyncMutex | per-wait-epoch cancel; queue-identity-gated | PROVEN | `async_mutex.hpp:166-190` |
| AsyncCondition | per-Condition-epoch cancel; queue-identity-gated; reacquire NOT cancellable | PROVEN | `condition.hpp:161-169` |
| AsyncQueue<T> | **No public wait-epoch cancellation API and no Cancelled result in v1.** `close()` and deadline expiry are distinct Queue state-machine causes (`closed` / `expired`), not cancellation surfaces. Wait-epoch cancel is deferred by D4. | INTENTIONALLY-DIFFERENT | `queue_port.hpp:683` |

### 3.10 External-Thread-Safe Operations

| Primitive | External-Thread-Safe Methods | Status | Evidence |
|-----------|------------------------------|--------|----------|
| Event | set(), reset(), cancel(), is_set() | PROVEN | `event.hpp:103,111,170,95` |
| Semaphore | release(), cancel(), try_acquire(), available() | PROVEN | `semaphore.hpp:217,198,134,120` |
| AsyncMutex | cancel() | PROVEN | `async_mutex.hpp:188` |
| AsyncCondition | notify_one(), notify_all(), cancel() | PROVEN | `condition.hpp:283,289,276` |
| AsyncQueue<T> | try_push, try_pop, close, is_closed, capacity, size, begin_teardown | PROVEN | `queue_port.hpp:378-395` |

### 3.11 Destruction/Teardown Contract

| Primitive | Destruction Contract | Status | Evidence |
|-----------|---------------------|--------|----------|
| Event | waiters must be empty (debug assert); no auto-cancel | PROVEN | `event.hpp:53-56` |
| Semaphore | waiters must be empty (debug assert); no auto-cancel | PROVEN | `semaphore.hpp:51-56` |
| AsyncMutex | unlocked + waiters empty (debug asserts); no auto-cancel | PROVEN | `async_mutex.hpp:56-61` |
| AsyncCondition | active_waits_ == 0 (debug assert); no auto-cancel | PROVEN | `condition.hpp:66-73` |
| AsyncQueue<T> | ring empty + teardown session complete; no auto-drain | PROVEN | `async_queue.hpp:35-42` |

### 3.12 Payload/Ownership Transfer

| Primitive | Payload Transfer | Status | Evidence |
|-----------|-----------------|--------|----------|
| Event | no payload | NOT-APPLICABLE | |
| Semaphore | permit transfer (release → FIFO head) or store | PROVEN | `semaphore.hpp:202-216` |
| AsyncMutex | ownership transferred via direct handoff (owner_ := winner) | PROVEN | `async_mutex.hpp:192-205` |
| AsyncCondition | Mutex ownership transferred via CONDITION-WAIT-PREPARE | PROVEN | `condition.hpp:136-145` |
| AsyncQueue<T> | T moved via QueueItemLease → Node<T> → typed result | PROVEN | `async_queue.hpp:356-387` |

### 3.13 Misuse Handling

| Primitive | Misuse Cases | Status | Evidence |
|-----------|-------------|--------|----------|
| Event | wrong-Event cancel → false; detached → false; terminal → false | PROVEN | `event.hpp:150-165` |
| Semaphore | wrong-Semaphore cancel → false; overflow release → false; bad ctor → debug assert | PROVEN | `semaphore.hpp:180-196` |
| AsyncMutex | recursive lock → debug assert; non-owner unlock → debug assert; wrong-Mutex cancel → false | PROVEN | `async_mutex.hpp:63-68` |
| AsyncCondition | non-owner wait → debug assert; wrong-Condition cancel → false; active_waits_ > 0 at dtor → debug assert | PROVEN | `condition.hpp:75-81` |
| AsyncQueue<T> | capacity 0 → throw; second begin_teardown → fail-fast; op after teardown → fail-fast; result misuse → fail-fast | PROVEN | `async_queue.hpp:53-58` |

### 3.14 E13 Select Readiness Mapping

| Primitive | E13 Select Assessment | Status | Evidence |
|-----------|----------------------|--------|----------|
| Event | WaitNode is single-epoch; can register in one queue at a time | PROVEN | `wait_node.hpp:53-54` |
| Semaphore | same WaitNode contract; primitive cancel is best-effort cleanup for a still-Registered arm only — NOT a Select-level loser authority | PROVEN | `semaphore.hpp:198` |
| AsyncMutex | same WaitNode contract; primitive cancel is best-effort cleanup for a still-Registered arm only — NOT a Select-level loser authority (cannot undo a committed owner handoff) | PROVEN | `async_mutex.hpp:188`, `async_mutex.hpp:548-560` |
| AsyncCondition | mandatory reacquire makes it unsuitable as direct Select arm | PROVEN | `condition.hpp:17-33` |
| AsyncQueue<T> | payload-bearing; no cancel in v1; Select integration needs new authority (including group-winner ordering relative to item commit) | DEFERRED | `queue_port.hpp:683`, `queue_port.hpp:728-739` |

---

## 4. Decision Register (D1–D10)

### D1: Public Result Model Consistency

```text
DECISION D1:
STATUS: ACCEPTED
CURRENT FACT:
  Event/Semaphore/AsyncMutex:   void wait(WaitNode&) + node.outcome()
  AsyncCondition:               WaitOutcome wait(WaitNode&) — returns directly
  AsyncQueue<T>:                typed QueuePushResult<T> / QueuePopResult<T>
CONFLICT:
  Three different result forms exist across the five primitives.
  Superficially inconsistent.
DECISION:
  This is intentional LAYERING, not drift. The differences are justified
  by each primitive's semantic model:
  - Event/Semaphore/Mutex offer a low-level E10 epoch API where the caller
    owns the WaitNode and queries outcome. This is the EL1 (epoch-level) API.
  - AsyncCondition returns WaitOutcome directly. The PRIMARY reason is its
    two-epoch protocol: the Condition outcome is latched in the Condition
    epoch BEFORE the mandatory Mutex reacquire epoch begins, and is returned
    ONLY after Mutex ownership is restored. Surfacing the latched outcome as
    a return value (rather than via node.outcome()) is the natural ergonomic
    form for this two-epoch shape, because the caller needs the Condition-
    epoch reason to decide whether to re-check the predicate inside the
    critical section it has just re-entered. This is EL2 (ergonomic epoch-
    level) API. The "user needs to inspect the outcome" framing is a
    SECONDARY ergonomic observation, not the load-bearing justification.
  - AsyncQueue<T> hides WaitNode and returns typed result objects because
    Queue operations are typed payload operations, not raw epoch operations.
    The WaitNode is internal to QueuePort. This is EL3 (fully abstract) API.
  No unification is required. The layering is consistent with the dependency
  trunk: E10 epoch → E12-A/B/C low-level → E12-D ergonomic → E12-E abstract.
RATIONALE:
  Condition outcome is latched before mandatory Mutex reacquire and returned
  only after ownership is restored. That latched-then-reacquired shape is
  what makes a direct WaitOutcome return the natural form for AsyncCondition;
  Event/Semaphore/Mutex have no reacquire epoch, so node.outcome() suffices.
  The three layers serve different user needs. Forcing a single result form
  would either expose WaitNode to Queue users (breaking abstraction) or
  hide WaitNode from Event/Semaphore/Mutex users (removing the cancellation
  handle). The current design preserves both use cases.
COMPATIBILITY:
  No change. All existing callers are unaffected.
IMPLEMENTATION EFFECT:
  None. Document the layering in the authority doc.
TEST EFFECT:
  None. Existing tests already cover each form.
DOCUMENTS SUPERSEDED:
  None.
```

### D2: WaitNode Visibility in User-Facing API

```text
DECISION D2:
STATUS: ACCEPTED
CURRENT FACT:
  Event/Semaphore/AsyncMutex/AsyncCondition require caller to provide WaitNode&.
  AsyncQueue<T> hides WaitNode (internal to QueuePort).
  WaitNode is non-copyable, non-movable, stack-allocatable, fresh-per-epoch.
CONFLICT:
  Is WaitNode& an E10 internal mechanism leaked to E12, or a necessary
  explicit cancellation handle?
DECISION:
  WaitNode& is a NECESSARY explicit cancellation handle at the low-level
  API layer (EL1/EL2). It is NOT a leak. Reasons:
  1. Stack lifetime: WaitNode is caller-owned, stack-allocated, zero heap
     allocation. Hiding it would require internal allocation or per-op handle.
  2. Cancel from another thread: external cancel(WaitNode&) requires the
     caller to hold the node reference. This is the intended cancellation
     mechanism.
  3. Fresh-per-epoch: the contract that each wait epoch uses a fresh WaitNode
     is enforced by the absorbing WaitNode state machine and the registration
     precondition that registration succeeds only from Detached. Deleted
     copy/move operations preserve object identity and address stability; they
     do not by themselves prevent terminal-node reuse.
  4. Future RAII wrapper: a convenience overload that hides WaitNode can be
     added later without breaking the existing low-level API.
  5. Queue is different: Queue operations are typed payload operations bound
     to a lease control block. The WaitNode is an implementation detail of
     the role FIFO; exposing it would break the typed abstraction.
RATIONALE:
  The current design is a deliberate two-tier API: low-level (WaitNode&
  visible) for Event/Semaphore/Mutex/Condition, and high-level (WaitNode
  hidden) for Queue. This is consistent with the dependency trunk.
COMPATIBILITY:
  No change. Future RAII wrappers would be additive.
IMPLEMENTATION EFFECT:
  None. Document the intentional two-tier design.
TEST EFFECT:
  None.
DOCUMENTS SUPERSEDED:
  None.
```

### D3: Deadline Precedence

```text
DECISION D3:
STATUS: ACCEPTED
CURRENT FACT:
  All primitives except AsyncCondition use resource-first admission precedence:
  - Event: SET checked BEFORE already-due deadline (event.hpp:134-141)
  - Semaphore: permit admission BEFORE already-due (semaphore.hpp:160-168)
  - AsyncMutex: ownership admission BEFORE already-due (async_mutex.hpp:148-157)
  - AsyncQueue: resource-first wins over due deadline (queue_port.hpp:706-710)
  EXCEPTION — AsyncCondition: already-due → Expired inline, retain ownership
  (condition.hpp:148-151). AsyncCondition is deadline-first: when the deadline
  is already due at admission, the wait returns Expired immediately without
  checking the condition predicate or acquiring the mutex. This is the only
  primitive where deadline precedence overrides resource availability.
CONFLICT:
  None. AsyncCondition's deadline-first behavior is an intentional design
  choice (condition variables must recheck the predicate after acquiring the
  mutex, which is only meaningful when the waiter actually blocks).
DECISION:
  Resource-first is the authoritative admission precedence for Event, Semaphore,
  AsyncMutex, and AsyncQueue. AsyncCondition is the explicit deadline-first
  exception. This is PROVEN across all five primitives with file:line evidence.
  Distinction between "admission precedence" (which check runs first at
  admission time) and "registered-race winner" (who wins the resolve_ CAS
  after registration) is maintained: admission precedence is resource-first
  (or deadline-first for AsyncCondition); the registered race is resolved by
  the single WaitNode::resolve_ CAS.
RATIONALE:
  Resource-first prevents a pathological case where a resource becomes
  available but the waiter is told "expired" because the deadline was
  already due. The resource being ready makes the deadline moot.
COMPATIBILITY:
  No change. This is the existing, tested behavior.
IMPLEMENTATION EFFECT:
  None.
TEST EFFECT:
  Covered. Queue already-due deadline tests H1–H4 exist in
  tests/async_queue_primitive_test.cpp (see §11.2–11.3).
DOCUMENTS SUPERSEDED:
  None.
```

### D4: Cancellation Semantics

```text
DECISION D4:
STATUS: ACCEPTED
CURRENT FACT:
  cancel(wait_node) on Event/Semaphore/AsyncMutex/AsyncCondition:
  - Resolves exactly one registered wait epoch with Cancelled
  - Queue-identity-gated: wrong-object cancel returns false, no mutation, no UB
  - Cross-Scheduler safe: membership check scans THIS object's queue only
  - NOT task cancellation, NOT Fiber cancellation, NOT cancel-all, NOT object close
  - AsyncQueue<T> has NO public cancel() — INTENTIONALLY DEFERRED (D4)
CONFLICT:
  None. All existing cancel implementations are consistent.
DECISION:
  "cancel" in the E10-E12 context means ONLY "per-wait-epoch cancellation".
  All other meanings (task cancel, Fiber cancel, cancel-all, I/O cancel) are
  explicitly out of scope.
  Queue v1 lacking public cancel() is intentional and must be documented as
  DEFERRED, not as an omission.
RATIONALE:
  Narrow cancellation scope is a deliberate E10 design choice. The single
  WaitNode::resolve_ CAS is the authority; cancel is just one of the three
  terminal causes (alongside wake and expiry). Expanding cancel to mean
  task/Fiber/object cancellation would require a separate authority.
COMPATIBILITY:
  No change. Queue cancel remains deferred.
IMPLEMENTATION EFFECT:
  None. Document Queue cancel as DEFERRED.
TEST EFFECT:
  Existing wrong-object cancel tests are comprehensive. Queue cancel tests
  are not applicable (v1 has no cancel).
DOCUMENTS SUPERSEDED:
  None.
```

### D5: Lifecycle and Destruction

```text
DECISION D5:
STATUS: ACCEPTED
CURRENT FACT:
  All primitives: destruction with active waits is a CALLER CONTRACT VIOLATION.
  Destructors do NOT cancel, wake, expire, or synthesize outcomes.
  - Event/Semaphore: ~WaitQueue asserts head_ == nullptr (debug)
  - AsyncMutex: asserts owner_ == nullptr (debug) + ~WaitQueue
  - AsyncCondition: asserts active_waits_ == 0 (debug) + ~WaitQueue
  - AsyncQueue: requires ring empty + teardown complete; fail-fast otherwise
  Release builds: no recovery protocol. Caller must drain first.
CONFLICT:
  None. All primitives are consistent in treating destruction-with-waiters
  as a caller contract violation.
DECISION:
  No unified teardown is needed. The existing contracts are consistent:
  - Event/Semaphore: simple waiters-empty (single WaitQueue)
  - AsyncMutex: unlocked + waiters-empty (owner_ + WaitQueue)
  - AsyncCondition: active_waits_ == 0 (covers reacquire epoch)
  - AsyncQueue: teardown session (complex four-counter ledger)
  Adding Queue-style teardown to Event/Semaphore/Mutex would be over-engineering.
RATIONALE:
  The destruction contract complexity scales with the primitive's internal
  state. A simple Event needs only waiters-empty; a Queue with buffered
  payloads needs a full teardown session. Forcing uniformity would add
  unnecessary complexity to simple primitives.
COMPATIBILITY:
  No change.
IMPLEMENTATION EFFECT:
  None.
TEST EFFECT:
  Existing destruction tests are adequate.
DOCUMENTS SUPERSEDED:
  None.
```

### D6: Thread Calling Boundaries

```text
DECISION D6:
STATUS: ACCEPTED
CURRENT FACT (from header evidence):
  Fiber-only (blocking/timed ops):
    Event::wait, Event::wait_until
    Semaphore::acquire, Semaphore::acquire_until
    AsyncMutex::lock, AsyncMutex::lock_until, AsyncMutex::try_lock, AsyncMutex::unlock
    AsyncCondition::wait, AsyncCondition::wait_until
    AsyncQueue::push, push_until, pop, pop_until
  Any OS thread (non-suspending ops):
    Event::set, reset, cancel, is_set
    Semaphore::release, cancel, try_acquire, available
    AsyncMutex::cancel
    AsyncCondition::notify_one, notify_all, cancel
    AsyncQueue::try_push, try_pop, close, is_closed, capacity, size, begin_teardown
  Construction/destruction: thread-neutral (no running Fiber required)
CONFLICT:
  None found. All methods are correctly documented.
DECISION:
  The thread boundary is proven by implementation lock domain analysis:
  - Fiber-only methods require g_worker->current (the calling Fiber)
  - External-thread methods do not access g_worker and route through
    global_mtx_ (taken by the Scheduler which is concurrency-safe)
RATIONALE:
  The distinction is load-bearing: blocking ops suspend the calling Fiber,
  which requires a Fiber context. External-thread ops are producers that
  resolve waiters without suspending themselves.
COMPATIBILITY:
  No change.
IMPLEMENTATION EFFECT:
  None.
TEST EFFECT:
  External-thread tests exist for all primitives.
DOCUMENTS SUPERSEDED:
  None.
```

### D7: Fairness and Barging

```text
DECISION D7:
STATUS: ACCEPTED
CURRENT FACT (from header evidence):
  - FIFO waiter selection: all primitives (WaitQueue is FIFO)
  - No barging: try_acquire/try_lock fail if a queued waiter has FIFO priority
  - Direct handoff: AsyncMutex::unlock commits owner_ BEFORE publication
  - Event set() broadcasts to ALL waiters (no completion order guarantee)
  - Condition notify_all() drains all (no reacquire order guarantee)
  - Queue: FIFO per role, but thread scheduling may alter resume order
CONFLICT:
  None.
DECISION:
  Only claim what is proven:
  - "FIFO waiter selection" — PROVEN (WaitQueue is FIFO intrusive list)
  - "no barging at authoritative admission" — PROVEN (try_* checks queue)
  Must NOT claim:
  - "strict fairness" — thread scheduling and work stealing affect resume order
  - "strict completion order" — not guaranteed by the implementation
  - "starvation freedom" — not formally proven
RATIONALE:
  FIFO at the waiter-selection level is the only guarantee the implementation
  makes. Resume order is affected by Worker scheduling, E8 stealing, and OS
  thread scheduling, which are outside the primitive's control.
COMPATIBILITY:
  No change in behavior. Documentation must be precise.
IMPLEMENTATION EFFECT:
  Review and correct any documentation that claims more than FIFO selection.
TEST EFFECT:
  Existing FIFO tests are sufficient.
DOCUMENTS SUPERSEDED:
  Any documentation that claims "strict fairness" without evidence.
```

### D8: Error and Failure Classification

```text
DECISION D8:
STATUS: ACCEPTED
CURRENT FACT:
  Error categories across all primitives:
  NORMAL OUTCOME:
    - try_acquire/try_lock/try_push/try_pop returning false/would_block
    - release() returning false (overflow)
    - cancel() returning false (wrong-object, detached, terminal)
    - push_until/pop_until returning expired
  RECOVERABLE ERROR:
    - None. All recoverable conditions are modeled as normal outcomes.
  CALLER CONTRACT VIOLATION (debug assert; release = UB):
    - invalid ctor args (Semaphore max_permits == 0)
    - recursive Mutex lock
    - non-owner Mutex unlock
    - destruction with active waits
    - WaitNode destroyed while Registered
  INTERNAL INVARIANT VIOLATION / FAIL-FAST (std::terminate):
    - Mutex lock/try_lock underlying std::mutex failure
    - Queue lease misuse (wrong port, type-token, location)
    - second begin_teardown
    - QueuePort destruction with non-empty ring
  RESOURCE EXHAUSTION POLICY:
    - Queue capacity 0 → std::invalid_argument (exception)
    - timer allocation failure (not handled in current code; O-4 observation)
CONFLICT:
  None.
DECISION:
  The classification is consistent. Debug asserts are NOT a release-build
  error handling API. The fail-fast policy for internal invariant violations
  is appropriate for a systems library where recovery from internal corruption
  is not meaningful.
RATIONALE:
  This classification follows the existing repository conventions:
  - Recoverable conditions are normal outcomes (try_* returning false)
  - Caller bugs are debug asserts (no runtime overhead in release)
  - Internal corruption is fail-fast (no recovery semantic)
COMPATIBILITY:
  No change.
IMPLEMENTATION EFFECT:
  None. Document the classification.
TEST EFFECT:
  Death tests exist for Mutex lock failures. Queue misuse fail-fast is
  tested via authority probes.
DOCUMENTS SUPERSEDED:
  None.
```

### D9: Type Constraints

```text
DECISION D9:
STATUS: ACCEPTED
CURRENT FACT:
  AsyncQueue<T> static_assert (async_queue.hpp:218-222):
    std::is_object_v<T>
    std::is_nothrow_move_constructible_v<T>
    std::is_nothrow_destructible_v<T>
  QueuePushResult<T> and QueuePopResult<T>:
    Move-constructible: yes (defaulted)
    Move-assignable: yes (hand-written destroy-and-rebuild, PR #12 corrective)
    Copy: deleted
    T need NOT be default-constructible (std::optional<T> storage)
    T need NOT be move-assignable (hand-written operator=)
  WaitNode: non-copyable, non-movable (identity = address)
  All primitives: non-copyable, non-movable (WaitQueue is non-movable)
  No incomplete type or reference type constraints (not tested)
CONFLICT:
  None. The PR #12 corrective fixed the move-assignable-T contract gap.
DECISION:
  The existing type constraints are correct and proven. The hand-written
  move-assign operators on QueuePushResult<T> and QueuePopResult<T> satisfy
  the documented contract (T need not be move-assignable).
  Existing tests for MoveConstructOnly types must be preserved and extended.
RATIONALE:
  The PR #12 review discovered that `= default` move-assign delegates to
  std::optional<T>::operator=, which requires T to be move-assignable.
  The hand-written destroy-and-rebuild fixes this without changing the API.
COMPATIBILITY:
  No change. The PR #12 corrective is already merged.
IMPLEMENTATION EFFECT:
  Preserve existing MoveConstructOnly tests. Add static_assert probes for
  all primitives (non-copyable, non-movable).
TEST EFFECT:
  Existing MoveConstructOnly tests in async_queue_primitive_test.cpp are adequate.
  Add static_assert probes for Event/Semaphore/AsyncMutex/AsyncCondition
  non-copyable/non-movable constraints.
DOCUMENTS SUPERSEDED:
  None.
```

### D10: E13 Select Dependency Contract

```text
DECISION D10:
STATUS: ACCEPTED
CURRENT FACT:
  E13 Select does not exist yet. The following claims are based on the
  current E10-E12 implementation facts.
DECISION:
  E13 MAY RELY ON:
    1. WaitNode is strictly single-epoch (one registration, one terminal outcome).
       wait_node.hpp:53-54, L107-117.
    2. A WaitNode cannot be simultaneously in multiple WaitQueues.
       wait_queue.hpp:44-46 (linked-at-most-once invariant).
    3. Existing primitive cancel is a best-effort cleanup mechanism ONLY for
       an arm that remains Registered in that primitive queue. It is NOT a
       Select-level loser authority and CANNOT undo an arm that has already
       resolved or committed its resource. event.hpp:170, semaphore.hpp:198,
       async_mutex.hpp:188.
    4. TimerRegistration binds to one WaitNode and one WaitQueue.
       timer_registration.hpp:98-99.
    5. The Scheduler is the sole resolution authority (sealed WaitQueue).
       wait_queue.hpp:62-95.
    6. First-scope Select arms MUST bind to the same Scheduler if atomic
       multi-arm coordination relies on one Scheduler::global_mtx_ domain.

  E13 MUST NOT RELY ON:
    1. A WaitNode being in multiple queues simultaneously.
       Violates linked-at-most-once.
    2. Queue cancel (v1 has no public cancel).
       Must treat Queue as non-cancellable in Select.
    3. AsyncCondition as a direct Select arm without handling the mandatory
       reacquire epoch. condition.hpp:17-33.
    4. Public WaitQueue access (sealed; no wait_queue() accessor).
    5. Any multi-wait registration primitive (does not exist).

  E13 NEEDS NEW AUTHORITY FOR:
    1. Parent/group winner authority: when multiple waits are registered,
       who decides which one wins? The current single-wait resolve_ CAS
       has no concept of a group. E13 requires a parent/group claim that
       ORDERS group-winner selection relative to irreversible primitive
       commit (e.g., AsyncMutex owner commit, Queue item commit). Without
       such an ordering authority, a loser arm may already have committed
       its resource before the group winner is chosen, and primitive-level
       cancel cannot undo that commit.
    2. Multi-wait registration: a mechanism to register one Fiber on
       multiple WaitQueues and resolve when any one is ready.
    3. Select-specific loser cleanup: when one arm wins, the others must
       be cleaned up. Existing primitive cancel() is a best-effort
       per-queue mechanism, NOT a Select-level loser authority; a
       Select-level coordinator is required to track registered arms and
       drive cleanup before the losing arm commits an irreversible
       resource.
    4. Payload-bearing Queue operations in Select: Queue push/pop carry
       typed payloads; Select needs to integrate these.
    5. Whether Select is built on public API or Scheduler private seams.
       The current sealed authority suggests private seams are needed.

RATIONALE:
  The current E10 WaitNode is deliberately single-epoch and single-queue.
  E13 Select requires multi-wait coordination, which is a new authority
  that must be designed without polluting the E10 WaitNode with Select-
  specific state (group_id, parent, etc.).
COMPATIBILITY:
  No change. E13 is not implemented.
IMPLEMENTATION EFFECT:
  None. Document the E13 contract.
TEST EFFECT:
  None.
DOCUMENTS SUPERSEDED:
  None.
```

---

## 5. Intentional Differences

| Difference | Primitive(s) | Reason | Evidence |
|------------|-------------|--------|----------|
| Result form: void + node.outcome() vs WaitOutcome vs typed | Event/Semaphore/Mutex vs Condition vs Queue | Layered API design (EL1/EL2/EL3) | D1 above |
| WaitNode visibility: caller-provided vs hidden | Event/Semaphore/Mutex/Condition vs Queue | Queue is typed payload abstraction | D2 above |
| No public cancel | AsyncQueue | Queue v1 DEFERRED; cancel requires state machine extension | D4 above |
| Teardown exists only on Queue | AsyncQueue | Queue has buffered payloads requiring ordered drain | D5 above |
| active_waits_ diagnostic counter | AsyncCondition | Two-epoch protocol needs reacquire-epoch coverage | condition.hpp:198 |
| QueueItemLease vs WaitNode& | Queue vs others | Queue is a typed payload channel; others are synchronization primitives | D2 above |
| `notify_one`/`notify_all` vs `set`/`release` | Condition vs Event/Semaphore | Condition is non-persistent; Event/Semaphore are persistent | §3.2 |
| Direct handoff (owner commit before publication) | AsyncMutex | Mutex ownership must be visible before winner resumes | async_mutex.hpp:548-560 |

---

## 6. Confirmed Contradictions

### C1: E12-E Queue Timer Substrate Supersession Chain

```text
FINDING F1:
SEVERITY: P2 (documentation clarity)
CONTRADICTION:
  e12-queue-scheduler-integration.md §8 declares PreparedQueueTimer as BINDING,
  but Corrective-3 (e12-queue-corrective-3.md) supersedes it because the
  production implementation never implemented PreparedQueueTimer.
  The production code uses generic ACTIVE-on-creation TimerRegistration
  with on_resolve_ hook.
  e12-queue-scheduler-integration.md's status banner does not clearly
  indicate this supersession.
RESOLUTION:
  Add a SUPERSESSION NOTICE at the top of e12-queue-scheduler-integration.md §8
  referencing Corrective-3 as the binding authority for the timer model.
  No code change.
```

### C2: api-reference.md Missing E10-E12 Async Primitives

```text
FINDING F2:
SEVERITY: P1 (user-facing documentation gap)
CONTRADICTION:
  api-reference.md and api-reference-zh.md document the synchronous
  sluice::async::Mutex (OS thread mutex wrapper) but do NOT document
  any E10-E12 async primitives: WaitOutcome, WaitNode, Event, Semaphore,
  AsyncMutex, AsyncCondition, AsyncQueue<T>.
  The async primitives are documented only in the e12-*.md design docs,
  which are not user-facing API references.
RESOLUTION:
  Add E10-E12 async synchronization section to api-reference.md and
  api-reference-zh.md. This is a documentation-only change.
```

### C3: changelog.md Not Updated for E10-E12

```text
FINDING F3:
SEVERITY: P1 (user-facing documentation gap)
CONTRADICTION:
  changelog.md has no entries for any E10-E12 async primitives.
  The only async entry is for sluice::async::Mutex (synchronous wrapper).
RESOLUTION:
  Add E10-E12 entries to changelog.md under the unreleased section.
```

### C4: WaitOutcome Not Formally Documented in User Docs

```text
FINDING F4:
SEVERITY: P2 (documentation clarity)
CONTRADICTION:
  WaitOutcome (woken/cancelled/expired) is referenced throughout E12
  documents but the C++ enum definition is never written out in any
  user-facing documentation.
RESOLUTION:
  Add WaitOutcome enum reference to api-reference.md.
```

### C5: E12-D AsyncCondition Status: IMPLEMENTATION BLOCKED but Code Exists

```text
FINDING F5:
SEVERITY: P1 (documentation vs implementation drift)
CONTRADICTION:
  docs/e12-condition.md status banner says IMPLEMENTATION BLOCKED,
  but the production code in condition.hpp is fully implemented
  and passes all 34 test cases.
  WaitOutcome returns directly from wait()/wait_until() (L223, L256).
  The implementation is complete and functional.
RESOLUTION:
  Update e12-condition.md status banner to reflect IMPLEMENTATION COMPLETE
  with REVIEW-REQUIRED (matching the actual state).
  The implementation was unblocked as part of the E12 dependency trunk
  but the document was not updated.
```

### C6: async-runtime-plan.md May Have Old E12 Ordering

```text
FINDING F6:
SEVERITY: P2 (documentation clarity)
CONTRADICTION:
  async-runtime-plan.md may still carry the old decomposition-list ordering
  for E12. The e12-sync-primitives-plan.md resolved this with the dependency
  trunk ordering (Event → Semaphore → Mutex → Condition → Queue → RwLock).
RESOLUTION:
  Verify and update async-runtime-plan.md to reference the canonical
  E12 dependency trunk ordering from e12-sync-primitives-plan.md.
```

---

## 7. Compatibility Policy

### Source Compatibility
- All existing public method signatures are PRESERVED.
- No method is renamed, removed, or reordered.
- No parameter type is changed.
- `[[nodiscard]]` additions are source-compatible (may produce new warnings, not errors).

### ABI Compatibility
- No existing type layout is changed.
- No virtual method is added or reordered.
- `WaitOutcome` enum values are unchanged.
- `QueuePushResult<T>`/`QueuePopResult<T>` layouts are unchanged.

### Behavioral Compatibility
- No deadline precedence is changed.
- No FIFO/barging behavior is changed.
- No cancellation semantic is changed.
- No destruction contract is changed.

### Documentation Compatibility
- Documentation-only changes are additive (new sections, status updates).
- No existing documentation claims are contradicted.

---

## 8. Required Non-Breaking Corrections

### 8.1 `[[nodiscard]]` Additions

| File | Method | Current | Required | Reason |
|------|--------|---------|----------|--------|
| `condition.hpp` | `wait()` | has `[[nodiscard]]` | keep | already correct |
| `condition.hpp` | `wait_until()` | has `[[nodiscard]]` | keep | already correct |

Status: The audited value-returning methods (condition wait/wait_until) already
have `[[nodiscard]]`. Other value-returning accessors (e.g. WaitOutcome return
values, QueuePushResult/QueuePopResult) were not in scope for this audit. No
additions needed for the audited set.

### 8.2 Static Assert Probes

Add compile-time API contract probes for primitives that lack them:

| Probe | Primitive(s) Missing | File to Create | Status |
|-------|---------------------|----------------|--------|
| non-copyable, non-movable | Event, Semaphore, AsyncCondition | tests/async_sync_api_contract_probe.cpp | COMPLETED |
| QueueResult move-only, non-copyable | (already exists) | — | COMPLETED |
| QueueResult move-assignable with MoveConstructOnly | (already exists) | — | COMPLETED |

### 8.3 Documentation Corrections

| File | Correction | Priority | Status |
|------|-----------|----------|--------|
| `e12-condition.md` | Update status banner: IMPLEMENTATION BLOCKED → IMPLEMENTATION COMPLETE, REVIEW-REQUIRED | P1 | COMPLETED (C5) |
| `e12-queue-scheduler-integration.md` | Add supersession notice for §8 timer model | P2 | COMPLETED (C1) |
| `api-reference.md` | Add E10-E12 async synchronization section | P1 | COMPLETED (C2) |
| `api-reference-zh.md` | Add E10-E12 async synchronization section | P1 | COMPLETED (C2) |
| `changelog.md` | Add E10-E12 entries | P1 | COMPLETED (C3) |
| `async-runtime-plan.md` | Verify E12 ordering matches dependency trunk | P2 | COMPLETED (C6) |

---

## 9. Deferred Breaking Proposals

```text
BREAKING-PROPOSAL-1:
NOT IMPLEMENTED
REQUIRES EXPLICIT AUTHORIZATION
Description: Add RAII operation handle (e.g., WaitOperation) that wraps
  WaitNode allocation and provides scoped cancellation. This would add
  a convenience overload without removing the existing WaitNode& API.
Impact: Additive only. No existing callers affected.

BREAKING-PROPOSAL-2:
NOT IMPLEMENTED
REQUIRES EXPLICIT AUTHORIZATION
Description: Add public cancel() to AsyncQueue<T> for producer/consumer
  wait epochs. Requires Queue state machine extension (P8/C7 transitions).
Impact: Additive. Requires new QueueV1WaitResolution canonical value.

BREAKING-PROPOSAL-3:
NOT IMPLEMENTED
REQUIRES EXPLICIT AUTHORIZATION
Description: Add sleep API / duration overload for deadline operations.
Impact: Additive convenience. No existing API affected.

BREAKING-PROPOSAL-4:
NOT IMPLEMENTED
REQUIRES EXPLICIT AUTHORIZATION
Description: Add close() or teardown() to Event/Semaphore/AsyncMutex/
  AsyncCondition for explicit irreversible shutdown with in-flight waiter
  resolution.
Impact: Breaking if it changes destruction contract. Requires separate
  design authority.
```

---

## 10. E13 Select Dependency Contract

### 10.1 E13 MAY RELY ON

```text
1. WaitNode is strictly single-epoch.
   Evidence: wait_node.hpp:53-54, L107-117.
   One WaitNode → one registration → one terminal outcome.

2. A WaitNode cannot be simultaneously in multiple WaitQueues.
   Evidence: wait_queue.hpp:44-46 (linked-at-most-once).
   register_() only succeeds from Detached; home_ is set once.

3. Existing primitive cancel is a best-effort cleanup mechanism ONLY for
   an arm that remains Registered in that primitive queue. It is NOT a
   Select-level loser authority and CANNOT undo an arm that has already
   resolved or committed its resource.
   Evidence: event.hpp:170, semaphore.hpp:198, async_mutex.hpp:188.
   cancel(wait_node) resolves the node with Cancelled if it is still
   Registered in the target queue; it returns false if already terminal
   (i.e., it cannot reverse a wake/expiry that already committed).

4. TimerRegistration binds to one WaitNode and one WaitQueue.
   Evidence: timer_registration.hpp:98-99.
   Constructor takes (WaitNode*, WaitQueue*, deadline). One registration
   per wait epoch.

5. The Scheduler is the sole resolution authority.
   Evidence: wait_queue.hpp:62-95 (sealed authority).
   All structural WaitQueue operations are private, Scheduler is the
   only friend. No public wait_queue() accessor on any primitive.

6. All primitives serialize under global_mtx_ (per-Scheduler).
   Evidence: scheduler.hpp (all seams require global_mtx_).
   First-scope Select arms MUST bind to the same Scheduler if atomic
   multi-arm coordination relies on one Scheduler::global_mtx_ domain.
   Cross-Scheduler Select needs a higher-level coordinator because
   global_mtx_ is per-Scheduler.
```

### 10.2 E13 MUST NOT RELY ON

```text
1. A WaitNode being in multiple queues simultaneously.
   Violates linked-at-most-once.

2. Queue cancel (v1 has no public cancel).
   Queue operations in Select must handle the lack of cancel differently
   (e.g., deadline-only, or new Select-specific cancel authority).

3. AsyncCondition as a direct Select arm without handling reacquire.
   The mandatory reacquire epoch (condition.hpp:17-33) means the Condition
   wait is not a simple "ready/not-ready" predicate — it has a post-
   resolution side effect (reacquiring the Mutex).

4. Public WaitQueue access.
   No primitive exposes its private WaitQueue. Select cannot bypass
   the Scheduler to register/resolve on queues directly.

5. Any multi-wait registration primitive.
   Does not exist. Select must build its own.

6. Primitive cancel as a Select-level loser authority.
   Existing cancel() is best-effort per-queue cleanup; it cannot undo an
   arm that has already resolved or committed its resource. Select loser
   cleanup needs a higher-level authority that orders group-winner
   selection relative to irreversible primitive commit.
```

### 10.3 E13 NEEDS NEW AUTHORITY FOR

```text
1. Parent/group winner authority.
   When multiple waits are registered for Select, a single "group winner"
   must be chosen. The current single-wait resolve_ CAS has no concept
   of a group. E13 requires a parent/group claim that ORDERS group-winner
   selection relative to irreversible primitive commit (e.g., AsyncMutex
   owner commit at async_mutex.hpp:548-560, Queue item commit at
   queue_port.hpp:728-739). Without such an ordering authority, a loser
   arm may already have committed its resource before the group winner is
   chosen, and primitive-level cancel cannot undo that commit.

2. Multi-wait registration.
   A mechanism to register one Fiber on multiple WaitQueues and resolve
   when any one is ready. This is a new primitive, not an extension of
   the existing single-wait await_wait.

3. Select-specific loser cleanup.
   When one arm wins, the others must be cleaned up. The existing
   primitive cancel() is best-effort per-queue cleanup and is NOT a
   Select-level loser authority; a Select-level coordinator is required
   to track registered arms and drive cleanup BEFORE a losing arm
   commits an irreversible resource (see #1).

4. Payload-bearing Queue operations in Select.
   Queue push/pop carry typed payloads (QueueItemLease). Select needs to
   integrate these without breaking the type abstraction.

5. Select architecture: public API vs private seams.
   Whether Select is built on the public primitive API (Event::wait,
   Semaphore::acquire, etc.) or on Scheduler private seams. The sealed
   authority suggests private seams are needed for atomic multi-registration.

6. Condition reacquire handling.
   If AsyncCondition participates in Select, the reacquire epoch must be
   handled. Options: (a) exclude Condition from Select, (b) make Select
   aware of the reacquire, (c) add a non-reacquire Condition variant.
```

---

## 11. Test Obligations

### 11.1 Existing Test Coverage (from codebase audit)

> **Status (2026-07-19):** all cells that were `MISSING` / `—` in the
> original audit are now covered by this closure (see §11.2 for the new TUs
> and cases). The table below records the post-closure coverage.
>
> Legend: **Yes** = tested; **n/a** = not applicable (no such API surface for
> this primitive); **—** = not tested (intentional gap, recorded as residual
> risk in §12 if non-trivial).

| Primitive | Fast Path | Blocking | Timed | Already-Due | Cancel | Wrong-Obj Cancel | Ext Thread | Destruction | Stress |
|-----------|-----------|----------|-------|-------------|--------|-----------------|------------|-------------|--------|
| E10 WaitNode | Yes | Yes | — | — | Yes | — | Yes | Yes | Yes |
| E11 Timer | Yes | Yes | Yes | Yes | Yes | — | Yes | Yes | Yes |
| E12-A Event | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| E12-B Semaphore | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| E12-C AsyncMutex | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| E12-D AsyncCondition | Yes | Yes | Yes | Yes | Yes | Yes (T30/T31) | Yes | Yes | Yes |
| E12-E Queue | Yes | Yes | Yes | Yes (H1–H4) | n/a (no cancel API) | n/a | Yes | Yes | Yes (G2) |

> `AsyncQueue<T>` v1 has no public wait-epoch cancellation API. `close()` is an
> independent monotonic Queue state transition; deadline expiry is an
> independent terminal operation result. Neither is a cancellation surface,
> and Queue v1 has no `Cancelled` public result. The Cancel / Wrong-Obj Cancel
> columns are therefore `n/a` by D4, not a gap.

### 11.2 Required Test Additions

> **Status (2026-07-19):** T1–T4 are **IMPLEMENTED and PASSING** as part of
> this closure. The new TUs are `tests/async_sync_api_contract_probe.cpp` (T3),
> `tests/async_sync_cross_primitive_parity_test.cpp` (T2), and additions to
> `tests/async_queue_primitive_test.cpp` (T1) and `tests/async_condition_primitive_test.cpp`
> (T4). All are wired into `xmake.lua` under the `test` group. Corrective-2
> requires a fresh toolchain matrix; §11.3 intentionally does not reuse the
> earlier author summary.

#### T1: Queue Already-Due Deadline Tests — IMPLEMENTED (H1–H4)
- push_until with already-due deadline: resource-first (slot available → committed) — `e12_queue_h1_push_until_already_due_free_slot_committed`
- push_until with already-due deadline + full ring: Expired — `e12_queue_h2_push_until_already_due_full_ring_expired`
- pop_until with already-due deadline + empty ring: Expired — `e12_queue_h4_pop_until_already_due_empty_ring_expired`
- pop_until with already-due deadline + item available: item — `e12_queue_h3_pop_until_already_due_item_available`

#### T2: Cross-Primitive Parity Tests — IMPLEMENTED (scoped evidence)
- `PROVEN_BY_THIS_NEW_TU`: pairwise distinct `WaitOutcome` enum values + fresh
  node unresolved — `e12_parity_waitoutcome_values_are_distinct_and_fresh_unresolved`.
  This is not a dynamic absorbing-state or concurrent-publication proof.
- resource-first admission consistency (Event/Semaphore/AsyncMutex) — `e12_parity_d3_event_set_plus_already_due_woken`, `e12_parity_d3_semaphore_permit_plus_already_due_woken`, `e12_parity_d3_mutex_free_plus_already_due_woken`
- D4 wrong-object cancel no mutation (Event/Semaphore/AsyncMutex) —
  `e12_parity_d4_event_cancel_wrong_event_returns_false`,
  `e12_parity_d4_semaphore_cancel_wrong_semaphore_returns_false`,
  `e12_parity_d4_mutex_cancel_wrong_mutex_returns_false`.
- `PROVEN_BY_EXISTING_PER_PRIMITIVE_TEST`: AsyncCondition already-due/ownership
  (`e12_cond_t1_already_due_inline_expired_retains_ownership`) and D4
  wrong-Condition (`e12_cond_t31_cancel_wrong_condition_returns_false`); Queue
  D3 through H1–H4; absorbing terminals and concurrent exactly-once publication
  through each primitive's race/terminal suite.
- `NOT_APPLICABLE`: Queue D4 cancellation (no public wait-epoch cancel API).

#### T3: Compile-Time API Contract Probes — IMPLEMENTED
- All primitives non-copyable, non-movable (static_assert) — `tests/async_sync_api_contract_probe.cpp` D5 lifecycle block
- QueueResult move-only, non-copyable (static_assert) — same file D1/D9 block
- QueueResult move-assignable with MoveConstructOnly (static_assert) — same file PR #12 block, using a hand-written `NonMoveAssignable` test type
- WaitOutcome four-value enum distinctness (static_assert) — same file vocabulary block
- Nine negative-compile blocks (including `NEG_WAITNODE_MOVE`) are executed by
  `scripts/verify-e12-api-contract-negative-compile.sh`. The gate defines each
  macro separately, requires compilation to fail, verifies the deleted-member
  diagnostic, and fails if any negative compile succeeds.
- Authority negative probes (already exist for all primitives): verified still green via the formal verify scripts (§11.3).

#### T4: Missing Wrong-Object Cancel Test for AsyncCondition — IMPLEMENTED (T30/T31)
- Condition cancel on wrong Condition → false — `e12_cond_t31_cancel_wrong_condition_returns_false`
- Condition cancel on a Detached (unregistered) node → false — `e12_cond_t30_cancel_detached_node_returns_false`
- (Condition cross-Scheduler wrong-object cancel is structurally covered by the Event CANCEL-2b case at the WaitQueue substrate level; AsyncCondition reuses the same `Scheduler::cancel_wait` path. Not duplicated here to avoid redundant substrate-level coverage.)

### 11.3 Verification Matrix

Corrective-2 invalidated the earlier reproduced-result summary because it
changed test sources. The following results are from fresh isolated build
directories at the corrected source. The complete commands, per-binary exits,
failure accounting, formal classifications, and baseline traces are preserved
in the Corrective-2 review artifact.

| Gate | Actual result | Classification |
|------|---------------|----------------|
| Clang Debug | 15/15 configured E10/E11/E12 binaries, exit 0 | PASS |
| GCC Debug | 15/15 configured E10/E11/E12 binaries, exit 0 | PASS |
| Clang ASan+LSan, no filter/exclusion | 15/15 × 3, 45/45 exits 0, sandbox-external | PASS |
| Clang TSan | 15/15, no race/assertion/deadly-signal/timeout | PASS |
| Directed Clang Debug | CANCEL-2a 50/50; T23 50/50 | PASS |
| Directed Clang ASan | CANCEL-2a, T23, parity TU, Condition T30+T31: each 50/50 (200 launches total) | PASS |
| Negative compile | nine macros independently rejected by Clang and GCC | PASS |
| Release production build | `sluice_core`, `sluice_async`, `sluice_async_internal_testing` | PASS |
| Formal | Semaphore, AsyncMutex, AsyncCondition, Queue exit 0; E11 and Event exit 1 | 4 PASS; 2 PREEXISTING TOOLCHAIN-SENSITIVE LIMITATIONS WITH BASELINE PARITY |

The acceptance and diagnostic environments are intentionally distinct:

```text
AUTHOR ACCEPTANCE MATRIX:
unsandboxed ASan+LSan 15/15 ×3 (45/45, failures 0)

ADDITIONAL SANDBOX OBSERVATION:
address-only 44/45, with one T16 DEADLYSIGNAL;
baseline parity independently reproduced under the same sandbox setup.

FORMAL EXPECTATION MATRIX:
4/6 EXPECTED GATES PASS
2/6 BASELINE-PARITY-PROVEN TOOLCHAIN-SENSITIVE LIMITATIONS
0 HEAD-ONLY REGRESSIONS
```

The repository currently configures **15**, not 14, E10/E11/E12 binaries; the
matrix ran all 15. Before the successful sandbox-external ASan+LSan matrix, a
sandboxed leak-enabled attempt ran all 15 test bodies but every process exited
nonzero when LeakSanitizer could not use `ptrace`/`/proc`; this is retained as
15 failed diagnostic launches. A subsequent sandboxed address-only diagnostic
(`detect_leaks=0`) produced 44/45: round 3's unfiltered Event binary hit a T16
wild-jump `AddressSanitizer:DEADLYSIGNAL`. At the baseline commit, the identical
Event command produced the same T16 failure in one round, the original
CANCEL-2a registration assertion failure in another, and one pass. The
corrected HEAD therefore removes the CANCEL-2a failure and does not enlarge the
baseline T16 failure class, but the earlier HEAD failure is not erased by the
later 45/45 result.

For formal verification, both E11 NEG-5 and Event NEG-EVENT-2 reach the
property antecedent and then stutter with a waiter still registered, so the
negative models are non-vacuous and do kill their intended mutations. This TLC
runtime prints only `Temporal properties were violated`, not the checked
property name, while both scripts intentionally require a named-property
diagnostic and therefore exit 1. The spec/config and scripts have zero diff
from baseline, and identical baseline commands reproduce both exit-1 results.
They are not ordinary formal PASS results and are not invalid negative models.

### 11.4 Event T23 / CANCEL-2a Deterministic Closure (Corrective-2 C1)

Corrective-1 claimed T23 had replaced its pre-wait `suspended` flag and
`yield()` with `waiting_count() >= 4`, but its actual source still published
`suspended` before `wait()`, yielded, and used fixed retry counts. It also used
the absolute deadline `100` in every iteration while repeatedly calling the
absolute-time `advance_clock(100)`, so from the second iteration W2 expired
inline instead of completing a registered timed wait.

The corrective changes are confined to `tests/event_primitive_test.cpp` only.
**No Event production semantics were changed** (`include/sluice/async/event.hpp`,
`include/sluice/async/wait_node.hpp`, `include/sluice/async/wait_queue.hpp`,
`src/async/scheduler.cpp`, `src/async/event.cpp` are unmodified — verified
by `git diff`). The fixes:

- **T23**: all four waiters now call the Event operation directly. The driver
  waits for `Scheduler::waiting_count() >= 4`, whose count is incremented under
  `global_mtx_` only after each node is linked, Registered, and accounted.
  Each iteration uses a monotonically increasing absolute W2 deadline and
  advances the test clock to that exact absolute value. Expiry and cancel are
  single operations after the registration gate; all fixed retry loops and the
  causal `yield()` assumption are removed.
- **CANCEL-2a**: `fcancel` now waits for `waiting_count() >= 1` before asserting
  registration and attempting wrong-Event cancel. Its `cancel_done` release is
  the sole publisher and occurs after both wrong-cancel and still-registered
  assertions; `fset` observes it with acquire before `ev_a.set()`. Thus both
  registration-before-cancel and cancel/assertions-before-set are mechanical.

No production Event or Scheduler implementation was changed. Stress counts and
sanitizer results are recorded only after the fresh Corrective-2 matrix runs.

---

## 12. Residual Risks

> Corrective-2 reconstruction does not accept author status claims as review
> evidence. R2 is classified from the fresh formal command;
> R5 is closed by the actual plan text. E12-B, E12-C, and E12-D each remain
> implementation-review required because no final independent artifact closes
> the corresponding review.

| Risk | Severity | Description |
|------|----------|-------------|
| ~~R1~~ | CLOSED | E12-B independent implementation review completed with `ACCEPT (WITH OBSERVATIONS)` (`docs/reviews/E12-B-SEMAPHORE-IMPLEMENTATION-INDEPENDENT-REVIEW-1.md`, 2026-07-19); its closure condition (E12-G cross-primitive audit PASS) was satisfied by `docs/reviews/E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1.md`. E12-C migration/data-race micro-review completed with PASS (`docs/reviews/E12-C-MIGRATION-DATA-RACE-MICRO-REVIEW-1.md`, 2026-07-19). Both E12-B and E12-C are now CLOSED. |
| ~~R2~~ | CLOSED | E12-D AsyncCondition formal gate was freshly run with the recorded jar and exits 0: correct model, reachability checks, all ten negative models, wrong-property gate, and seven independent compile probes produced their expected results. This closes only the stale formal-evidence risk; E12-D independent review was closed by R7. |
| R3 | Low | Queue timer allocation failure (O-4 observation) is not handled. On allocation failure, the wait proceeds without a timer, potentially stranding the waiter. |
| R4 | Low | O-1 inline-path CAS/hook ordering and O-2 pump-publication counter asymmetry are documented as non-blocking observations from Queue review. |
| ~~R5~~ | CLOSED | async-runtime-plan.md was updated by this closure (the F1 cross-primitive audit line and E12 primitive statuses now reflect the dependency trunk ordering). |
| R6 | Medium | E13 Select dependency contract is based on current implementation facts. The actual Select design may discover new requirements that change the contract. Corrective-1 (C2) strengthened the contract to record that primitive cancel is NOT a Select-level loser authority and that E13 requires a parent/group claim ordering group-winner selection relative to irreversible primitive commit. E13 Select preparation may begin, but Select design and production implementation are not yet authorized. |
| ~~R7~~ | CLOSED | E12-D AsyncCondition preparation and implementation received an independent adversarial review PASS (`docs/reviews/E12-D-ASYNC-CONDITION-INDEPENDENT-REVIEW.md`, 2026-07-19). E12-D is CLOSED. |

---

## 13. Supersession Map

| Superseded Document | Superseding Authority | Scope |
|--------------------|-----------------------|-------|
| e12-queue.md (historical analysis) | e12-queue-state-machine.md + e12-queue-scheduler-integration.md | Queue semantic model |
| e12-queue-scheduler-integration.md §8 (PreparedQueueTimer) | e12-queue-corrective-3.md | Queue timer model |
| e12-sync-primitives-plan.md (old decomposition list) | e12-sync-primitives-plan.md §dependency trunk | E12 ordering |
| This document | None (new authority) | E10-E12 cross-primitive API and semantic closure |

---

## Appendix A: file:line Evidence Index

### E10
- WaitNode: `include/sluice/async/wait_node.hpp`
- WaitQueue: `include/sluice/async/wait_queue.hpp`

### E11
- TimerRegistration: `include/sluice/async/timer_registration.hpp`
- Scheduler deadline API: `include/sluice/async/scheduler.hpp:265-316`

### E12-A
- Event: `include/sluice/async/event.hpp`

### E12-B
- Semaphore: `include/sluice/async/semaphore.hpp`

### E12-C
- AsyncMutex: `include/sluice/async/async_mutex.hpp`

### E12-D
- AsyncCondition: `include/sluice/async/condition.hpp`

### E12-E
- AsyncQueue<T>: `include/sluice/async/async_queue.hpp`
- QueuePort: `include/sluice/async/detail/queue_port.hpp`
- QueueItemFactory: `include/sluice/async/detail/queue_port.hpp:221-322`
- QueueItemControl/Lease: `include/sluice/async/detail/queue_item.hpp`

### OS Thread Mutex (NOT AsyncMutex)
- Mutex: `include/sluice/async/mutex.hpp`

### Scheduler
- Scheduler: `include/sluice/async/scheduler.hpp`

---

## Appendix B: Document Status After This Closure

> Reconstructed from actual review artifacts by Corrective-2 (C3). "Review
> status" below is the independent adversarial review verdict where one
> exists; "—" means no independent review artifact exists for that scope.

| Document | Preparation | Implementation | Independent impl review | After Closure |
|----------|-------------|----------------|-------------------------|---------------|
| e10-waitnode-wait-queue.md | CLOSED | COMPLETE | PASS (history) | CLOSED (unchanged) |
| e11-deadline-timer-wait.md | CLOSED | COMPLETE | PASS (history) | CLOSED (unchanged) |
| e12-event.md | CLOSED | COMPLETE | PASS (history) | CLOSED (unchanged) |
| e12-semaphore.md | CLOSED | COMPLETE | `ACCEPT (WITH OBSERVATIONS)` (`E12-B-SEMAPHORE-IMPLEMENTATION-INDEPENDENT-REVIEW-1`, 2026-07-19); closure condition satisfied by E12-G PASS | CLOSED |
| e12-async-mutex.md | CLOSED | COMPLETE | PASS (`E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1`, 2026-07-19) | CLOSED |
| e12-condition.md | PASS (`E12-D-ASYNC-CONDITION-INDEPENDENT-REVIEW` §K, 2026-07-19) | COMPLETE | PASS (same artifact, 2026-07-19) | CLOSED |
| e12-queue.md | Superseded | — | — | Superseded (unchanged) |
| e12-queue-state-machine.md | PASS | — | PASS | PASS (unchanged) |
| e12-queue-scheduler-integration.md | PASS | — | PASS | PASS + supersession notice |
| e12-queue-corrective-3.md | BINDING | — | — | BINDING (unchanged) |
| e12-queue-production-implementation.md | PASS | COMPLETE | PASS (PR #12 + corrective) | PASS (unchanged) |
| e12-sync-primitives-plan.md | READY | — | — | READY (unchanged) |
| api-reference.md | v0.1-mvp | — | — | Updated with E10-E12 section |
| api-reference-zh.md | v0.1-mvp | — | — | Updated with E10-E12 section |
| changelog.md | unreleased | — | — | Updated with E10-E12 entries + closure |
| e10-e12-api-semantic-closure.md | NEW | — | PASS (`E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1`, 2026-07-19) | CLOSED — see §15 |

C3 reconciliation notes:
- E12-B independent implementation review was completed after PR #13 merge by
  `docs/reviews/E12-B-SEMAPHORE-IMPLEMENTATION-INDEPENDENT-REVIEW-1.md`
  (2026-07-19), verdict `ACCEPT (WITH OBSERVATIONS)`. Its closure condition
  (E12-G cross-primitive audit PASS) is satisfied.
- `reviews/E12-C-REVIEW.md` retains its historical `PASS (corrective applied)`
  verdict and the historical "await the final migration data-race micro-review"
  text; that micro-review was subsequently completed by
  `docs/reviews/E12-C-MIGRATION-DATA-RACE-MICRO-REVIEW-1.md` (2026-07-19),
  verdict PASS. The `E12-C-REVIEW.md` FINAL STATUS block records the resulting
  CLOSED governance effect without rewriting the historical reviewer text.
- E12-D preparation and implementation were reviewed after PR #13 merge by
  `docs/reviews/E12-D-ASYNC-CONDITION-INDEPENDENT-REVIEW.md` (2026-07-19),
  verdict PASS.

---

## 14. Final Report

```text
E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-CORRECTIVE-2:
    PASS — AUTHOR SELF-ASSESSMENT;
    INDEPENDENT RE-REVIEW REQUIRED
```

### 14.1 Verdict

Corrective-2 is a test/documentation correction submitted for independent
re-review. It does not authorize semantic closure. Scope must be verified from
the baseline-to-final-HEAD diff, including `include`, `src`, and `docs/spec`.

**Corrective-2 (this revision)** corrects the independently confirmed blockers
while retaining the valid C1–C7 contract work, without
touching production code:

- **C1**: Event T23 and CANCEL-2a now use Scheduler-authoritative
  `waiting_count()` registration gates. T23 also uses a monotonically
  increasing absolute deadline and has no fixed retry loop (§11.4).
- **C2**: E13 cancellation contract corrected — primitive cancel is best-
  effort per-queue cleanup, NOT a Select-level loser authority; E13 needs
  a parent/group claim ordering group-winner selection relative to
  irreversible primitive commit; first-scope Select arms must bind to one
  Scheduler if relying on one `global_mtx_` domain.
- **C3**: E12-B, E12-C, and E12-D are all implementation-review required.
  The E12-C artifact's explicit pending micro-review controls over its earlier
  intermediate PASS-looking line.
- **C4**: Every "fresh-per-epoch enforced by the type system" claim
  replaced with the absorbing state machine + Detached-only registration
  precondition wording.
- **C5**: `close()` and deadline expiry are no longer described as Queue
  cancellation surfaces — they are distinct Queue state-machine causes.
- **C6**: `WaitOutcome` classified as `unresolved` + three terminal
  outcomes (`woken`/`cancelled`/`expired`); `WaitQueue` and
  `TimerRegistration` described as Scheduler-integrated runtime substrate,
  not standalone user synchronization primitives.
- **C7**: D1 rationale strengthened — AsyncCondition's direct
  `WaitOutcome` return is justified primarily by its two-epoch protocol
  (outcome latched before mandatory Mutex reacquire, returned only after
  ownership is restored), not merely by "users need to inspect the outcome".

The closure establishes:

- A **Public API Inventory Matrix** (§2) sourced from real headers with
  `file:line` evidence — not from docs.
- A **Cross-Primitive Semantic Contract Matrix** (§3) of 14 dimensions × 5
  primitives, each cell labeled PROVEN / DOCUMENTED-BUT-UNPROVEN /
  DOCUMENTED / NOT-APPLICABLE with `file:line` evidence.
- **Decisions D1–D10** (§4) recording the cross-primitive contract on result
  model, WaitNode visibility, deadline precedence, cancellation semantics,
  lifecycle, thread boundaries, fairness, error classification, type
  constraints, and the E13 Select dependency.
- **Contradictions C1–C6** (§6) severity-rated, each with a RESOLUTION that
  was applied as part of this closure.
- The **E13 Select dependency contract** (§10) — what E13 may assume and
  what it must not break.
- A **supersession map** (§13) recording which older docs are superseded by
  which authority.

### 14.2 Baseline

| Item | Value |
|---|---|
| Branch | `audit/e10-e12-api-semantic-closure` |
| Baseline (master) | `d0cd915` (Merge PR #12 — E12-E Queue production impl) |
| Toolchains | Recorded from the fresh Corrective-2 run |
| Platform | Recorded from the fresh Corrective-2 run |

### 14.3 API Inventory summary

The public inventory includes `WaitNode`, the publicly nameable but sealed
`WaitQueue`, `TimerRegistration`, `Event`, `Semaphore`, `AsyncMutex`,
`AsyncCondition`, `AsyncQueue<T>`, both typed Queue result templates, and the
`detail::QueueTeardownSession` type exposed by Queue signatures. Plus the load-bearing distinction:
`include/sluice/async/mutex.hpp` is the OS-thread `Mutex` (TSA-annotated
`std::mutex` wrapper), **not** the Fiber-suspending `AsyncMutex`. The two
must never be conflated in docs, naming, or audit (recorded in §2 and in the
new api-reference sections).

### 14.4 Matrix summary

14 dimensions × 5 primitives = 70 cells. Distribution (post-closure):

- PROVEN (runtime test or formal model): majority — Event/Semaphore/Mutex/
  Condition/Queue each have per-primitive test suites + formal models where
  applicable.
- DOCUMENTED-BUT-UNPROVEN: a small set (e.g. Queue O-4 allocation-failure
  path), recorded as residual risks (§12).
- INTENTIONALLY-DIFFERENT: cells where a primitive's behavior is deliberately
  distinct from the cross-primitive norm (e.g. AsyncCondition deadline-first
  admission in D3), with documented rationale.
- DEFERRED: cells where the behavior is correct but formal evidence is deferred
  pending future review or implementation maturity.
- DOCUMENTED: invariants recorded in design docs and reproduced in the
  matrix with citations.
- NOT-APPLICABLE: structural (e.g. Queue has no `cancel(WaitNode&)`; Event
  has no payload transfer).

### 14.5 Decisions

D1–D10 are recorded in §4 with rationale, alternatives considered, and
`file:line` evidence. All ten are non-breaking: they describe the production
code as it exists, they do not prescribe any API rename, signature change,
or semantic change. Where a future breaking improvement is conceivable, it
is recorded in §9 (Deferred Breaking Proposals) and explicitly excluded from
this closure.

### 14.6 Findings (C1–C6)

Six contradictions were identified and resolved:

- C1 (P2): `e12-queue-scheduler-integration.md` §8 PreparedQueueTimer
  supersession notice — RESOLVED (supersession notice added).
- C2 (P1): `api-reference.md` / `api-reference-zh.md` missing E10–E12
  async primitives — RESOLVED (sections added in both languages).
- C3 (P1): `changelog.md` missing E10–E12 entries — RESOLVED.
- C4 (P2): WaitOutcome not formally documented in user docs — RESOLVED
  (enum reference added to api-reference.md).
- C5 (P1): `e12-condition.md` status banner stale (IMPLEMENTATION BLOCKED
  while code is implemented) — RESOLVED (banner updated).
- C6 (P2): `async-runtime-plan.md` E12 ordering status stale — RESOLVED.
- New (post-closure): cross-primitive test gaps (Queue already-due,
  AsyncCondition wrong-object cancel, cross-primitive parity, compile-time
  contract probes) — RESOLVED (new TUs and cases added; all green).

### 14.7 Files

**New (original closure):**
- `docs/e10-e12-api-semantic-closure.md` (this document)
- `tests/async_sync_api_contract_probe.cpp`
- `tests/async_sync_cross_primitive_parity_test.cpp`
- `docs/reviews/E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1-REVIEW-REQUEST.md`

**Modified (original closure, non-breaking):**
- `docs/api-reference.md`, `docs/api-reference-zh.md`
- `docs/changelog.md`, `docs/async-runtime-plan.md`, `docs/e12-condition.md`
- `docs/e12-queue-scheduler-integration.md` (§8 supersession notice)
- `tests/async_condition_primitive_test.cpp` (T30/T31)
- `tests/async_queue_primitive_test.cpp` (H1–H4)
- `xmake.lua` (two new test targets)

**Modified/added by Corrective-2 (this revision, non-breaking):**
- `tests/event_primitive_test.cpp` (C1: T23 + CANCEL-2a deterministic closure —
  no Event production semantics changed)
- `tests/async_sync_cross_primitive_parity_test.cpp` and `xmake.lua` (truthful D3/D4
  scope and no dynamic-terminal overclaim)
- `tests/async_sync_api_contract_probe.cpp` plus
  `scripts/verify-e12-api-contract-negative-compile.sh` (nine automated
  expected-failure compiles)
- `docs/e10-e12-api-semantic-closure.md` (C2/C3/C4/C5/C6/C7 wording and
  status corrections, §11.3/§11.4/§12/§14 reconciliation)
- `docs/api-reference.md`, `docs/api-reference-zh.md` (exact public inventory)
- `docs/e12-condition.md`, `docs/e12-async-mutex.md`,
  `docs/reviews/E12-C-REVIEW.md`, and the historical-status notice in
  `docs/reviews/E12-D-CONDITION-PREPARATION-AUDIT-1.md` (C3 authority status)
- `docs/reviews/E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1-REVIEW-REQUEST.md`
  (actual Corrective-2 code/evidence request)

**Unchanged (verified):** every file under `include/sluice/async/` and
`src/async/`; every per-primitive authority probe; the protected untracked
files `tests/test_t3_simple.cpp` and `tla2tools.jar`.

### 14.8 API changes

```text
BREAKING PUBLIC API CHANGES: 0
NON-BREAKING DOC/TEST/VERIFY CHANGES: see §14.7 and baseline-to-final-HEAD diff
```

No public symbol was renamed, no signature was changed, no semantic was
altered. The audit is doc + test only.

### 14.9 Tests

- New compile-time probe: `tests/async_sync_api_contract_probe.cpp` (lifecycle,
  WaitOutcome vocabulary, Queue result constraints + 9 negative-compile
  blocks); the negative branches are automated by
  `scripts/verify-e12-api-contract-negative-compile.sh`.
- New cross-primitive parity test: `tests/async_sync_cross_primitive_parity_test.cpp`
  (D3 and D4 for Event/Semaphore/AsyncMutex; enum distinctness + fresh
  unresolved only; 7 cases). AsyncCondition/Queue and dynamic terminal
  publication remain existing per-primitive evidence.
- New Queue already-due cases: H1–H4 in `tests/async_queue_primitive_test.cpp`.
- New AsyncCondition wrong-object cancel cases: T30/T31 in
  `tests/async_condition_primitive_test.cpp`.
- **Corrective-2 (C1)**: Event T23 + CANCEL-2a deterministic closure in
  `tests/event_primitive_test.cpp` (replaces `yield()`-based causal sync with
  mechanically-gated atomics + `waiting_count()` admission proof).

### 14.10 Verification

Fresh Corrective-2 evidence is recorded in §11.3 and the review artifact. The
full sandbox-external Clang ASan+LSan matrix is 45/45 with no exclusions;
Clang/GCC Debug, TSan, directed stress, negative compile, and production
Release gates are green. Earlier failed diagnostic launches and the
baseline-matched T16 ASan observation remain explicitly counted.

### 14.11 Formal model

All six formal scripts are reported individually in the review artifact.
Semaphore, AsyncMutex, AsyncCondition, and Queue exit 0. E11 and Event exit 1:
their liveness negatives produce non-vacuous stuttering counterexamples, but
the runtime omits the named property string required by the scripts. Identical
baseline commands reproduce both results with zero spec/config/script diff;
they are classified as pre-existing toolchain-sensitive limitations with
baseline parity, never as ordinary PASS.

### 14.12 Compatibility

The closure preserves the documented compatibility surface: the four special
members of every primitive remain deleted (D5/D9), `WaitOutcome` remains the
four-value vocabulary, the typed Queue result types remain move-only and
move-assignable for non-move-assignable `T` (D1/D9 / PR #12 corrective), and
resource-first deadline precedence (D3) plus queue-identity cancellation (D4)
are backed by the precisely scoped evidence in §11.2.

### 14.13 E13 Select dependency

Recorded in §10. E13 may assume the E10–E12 surface as documented in this
closure. E13 must not break: the single-resolver CAS protocol, the
wait-epoch identity model, the resource-first precedence, the
queue-identity cancellation gate, or the typed-result move semantics.
Corrective-1 (C2) additionally records that primitive `cancel` is NOT a
Select-level loser authority and that E13 needs a parent/group claim
ordering group-winner selection relative to irreversible primitive commit.

### 14.14 Residual risks

Recorded in §12. E12-B, E12-C, and E12-D all remain implementation-review
required; E12-C specifically awaits the migration data-race micro-review named
by its artifact. E13 contract discovery remains non-blocking for this audit.

### 14.15 Review

Independent adversarial review is **REQUESTED**. The review request is at
`docs/reviews/E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1-REVIEW-REQUEST.md`.
The reviewer should reproduce the verification matrix (§11.3), confirm the
non-breaking scope (§14.7), and independently verify a sample of matrix
cells (§3) and decisions (§4). The verdict block at the top of this section
remains `PASS — AUTHOR SELF-ASSESSMENT; INDEPENDENT REVIEW REQUIRED` until
the reviewer signs it.

---

## 15. E12-G Final Closure Authorization

> This section was appended after PR #13 merge (2026-07-19) to record the
> final governance effect of the four independent reviews that closed the
> review-required statuses disclosed by §12 and Appendix B at PR #13 merge
> time. The historical Corrective-2 author self-assessment in §14 above is
> preserved verbatim.

### 15.1 Review baseline

```text
PR13_MERGE_COMMIT (reviewed by all four reviews): e75c0fed3b4866efed02ab74ee8505ee7ddc79de
PR13_BASE:                                    d0cd915159a49ee30e88b0fdaec04a7b78260af1
REVIEW_DATE:                                  2026-07-19
```

### 15.2 Independent review artifacts

| Scope | Artifact | Authority ID | Verdict |
|---|---|---|---|
| E12-B Semaphore production implementation | `docs/reviews/E12-B-SEMAPHORE-IMPLEMENTATION-INDEPENDENT-REVIEW-1.md` | `E12-B-SEMAPHORE-IMPLEMENTATION-INDEPENDENT-REVIEW-1` | `ACCEPT (WITH OBSERVATIONS)`; closure condition (E12-G PASS) satisfied |
| E12-C AsyncMutex migration / data-race micro-review (Corrective-4 closure) | `docs/reviews/E12-C-MIGRATION-DATA-RACE-MICRO-REVIEW-1.md` | `E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1` | `PASS` |
| E12-D AsyncCondition preparation + implementation | `docs/reviews/E12-D-ASYNC-CONDITION-INDEPENDENT-REVIEW.md` | `E12-D-ASYNC-CONDITION-INDEPENDENT-REVIEW` | `PASS` |
| E10-E12 cross-primitive semantic closure (this PR #13) | `docs/reviews/E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1.md` | `E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1` | `PASS` |

### 15.3 Independence statement

Each of the four reviews above was produced by a separate reviewer (AI-1,
AI-2, AI-3, and the E12-G reviewer) acting independently. The E12-G
cross-primitive reviewer reached its PASS verdict on the cross-primitive
closure scope from PR #13 evidence alone, without reading the three sibling
per-primitive review artifacts during its own review. The four-way
governance reconciliation recorded in this section was applied only after
E12-G reached its verdict.

### 15.4 Final authorization

```text
E12-B-SEMAPHORE-IMPLEMENTATION-INDEPENDENT-REVIEW-1:
ACCEPT (WITH OBSERVATIONS); closure condition satisfied by E12-G PASS

E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1:
PASS

E12-D-ASYNC-CONDITION-INDEPENDENT-REVIEW:
PASS

E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1:
PASS

E12-B IMPLEMENTATION:
CLOSED

E12-C IMPLEMENTATION:
CLOSED

E12-D IMPLEMENTATION:
CLOSED

E10-E12 CROSS-PRIMITIVE SEMANTIC CLOSURE:
AUTHORIZED

E12-G:
CLOSED
```

### 15.5 Residual risk status after closure

```text
R1 (E12-B + E12-C review-required): CLOSED
R2 (stale AsyncCondition formal evidence): CLOSED
R3 (Queue timer allocation failure O-4): OPEN-NON-BLOCKING
R4 (Queue O-1/O-2 observations): OPEN-NON-BLOCKING
R5 (async-runtime-plan.md ordering/status): CLOSED
R6 (E13 Select dependency contract): OPEN-NON-BLOCKING — preparation may begin, implementation not authorized
R7 (E12-D preparation + implementation review): CLOSED
```

### 15.6 Scope of this authorization

This authorization closes the **cross-primitive semantic closure scope** of
PR #13: the API inventory matrix (§2), the 14-dimension × 5-primitive semantic
matrix (§3), decisions D1–D10 (§4), the E13 Select dependency contract (§10),
the contradictions C1–C6 resolutions, and the Corrective-2 finding
dispositions.

It also records the per-primitive implementation closure of E12-B, E12-C,
and E12-D by reference to the corresponding sibling review artifacts.

It does **not**:
- close E12-F RwLock (still `DEFERRED`);
- change E12-E AsyncQueue\<T\> status (still `Phase I review PASS WITH
  OBSERVATIONS`);
- authorize E13 Select production implementation (E13 preparation may begin;
  Select design and production implementation require their own separate
  authority);
- close the E11/Event named-liveness formal gate (still
  `PREEXISTING-BASELINE-PARITY-PROVEN`, awaiting a future formal/tooling
  corrective that is out of scope for the async substrate closure).

### 15.7 Compatibility

This authorization is documentation/governance only. No production code, no
formal spec, no build policy, and no public API was modified by any of the
four reviews or by the status synchronization that recorded this section.
