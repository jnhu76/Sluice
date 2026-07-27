# E12-F AsyncRwLock — Design / Spec / Implementation Authorization

> Status:
> ```text
> E12-F-DESIGN: COMPLETE
> E12-F-IMPLEMENTATION-AUTHORIZATION: AUTHORIZED FOR IMPLEMENTATION
> ```
>
> Authority baseline: E10 CLOSED (WaitNode/WaitQueue); E11 CLOSED
> (deadline/timer); E12-A Event CLOSED; E12-B Semaphore CLOSED;
> E12-C AsyncMutex CLOSED; E12-D AsyncCondition CLOSED;
> E12-E AsyncQueue CLOSED; E13 Select CLOSED.
> This document does NOT reopen any closed subsystem.
>
> Cross-primitive preparation:
> [`docs/history/implementation-plans/e12-sync-primitives-plan.md`](../history/implementation-plans/e12-sync-primitives-plan.md) §9
> (RwLock semantic authority, Task H) — this document resolves the
> HUMAN-DECISION-REQUIRED fairness policy classification.

---

## Status

```text
E12-F-DESIGN: COMPLETE
E12-F-IMPLEMENTATION-AUTHORIZATION: AUTHORIZED FOR IMPLEMENTATION
```

---

## Scope

AsyncRwLock v1 provides:

```text
read lock (shared acquisition)
write lock (exclusive acquisition)
read unlock (release one reader share)
write unlock (release exclusive ownership)
deadline-aware acquisition (read_lock_until / write_lock_until)
cancellation (per-wait-epoch cancel)
Scheduler integration (global_mtx_ coordination domain)
destruction invariant (caller contract)
multi-worker correctness
```

Explicitly excluded from v1:

```text
upgrade read → write
downgrade write → read
recursive locking
reentrant locking
optimistic reads
try-upgrade / atomic upgrade
lock promotion queues
priority inheritance
cross-process locking
Select integration
```

---

## Existing authority chain

### Authority map

| Behavior / State | Authoritative component |
| --- | --- |
| waiter registration | Scheduler::await_wait (global_mtx_ + q.mtx()) |
| queue structural mutation | WaitQueue (private, Scheduler-friend-only) |
| deadline registration | Scheduler timer subsystem (timer_pool_ + deadline_heap_) |
| cancellation resolution | WaitNode::resolve_(Cancelled) CAS under q.mtx() |
| resource grant | Scheduler seam (primitive-specific: handoff/transfer/batch) |
| winner selection | WaitNode::resolve_ CAS (one winner authority) |
| unlink/removal | WaitQueue::unlink_locked (same CS as winning CAS) |
| runnable publication | Scheduler::route_runnable_locked (AFTER resource commit) |
| terminal accounting | Scheduler::waiting_waitq_count_ (increment/decrement under global_mtx_) |
| destruction validation | ~WaitQueue asserts empty; primitive asserts resource-free |

### Lock ordering (established, invariant)

```text
Scheduler::global_mtx_  (G)
    → WaitQueue::mtx_   (W)
```

All existing E12 primitives follow this order. The Condition takes two queue
mutexes SEQUENTIALLY (never simultaneously) under G. The Queue takes
G → QueuePort::state_mtx_ (S) → exactly one role mtx.

AsyncRwLock MUST follow:

```text
G → AsyncRwLock waiters_.mtx()
```

No primitive-local lock is introduced. All authoritative RwLock state
mutations occur under G (same as Mutex/Semaphore/Event).

---

## Goals

1. Writer-fair admission preventing writer starvation under continuous readers.
2. Reader batch grant for throughput (consecutive reader prefix at queue head).
3. Exactly-once terminal resolution per wait epoch (E10 law preserved).
4. Winner-before-publication commit for writer grant (E12-C pattern).
5. Batch-commit-before-publication for reader grant (new seam, same principle).
6. Unified cancellation/deadline/resource precedence (resource-first, E11/E12 pattern).
7. Destruction contract consistent with existing primitives.
8. No public WaitQueue structural exposure.

---

## Non-goals

1. Select integration (v1 does NOT participate in Select).
2. Upgrade/downgrade.
3. Per-fiber reader ownership tracking.
4. Recursive/reentrant acquisition.
5. Priority inheritance.
6. Configurable fairness policy.

---

## API decision

### Recommendation: explicit lock/unlock (Method A)

```cpp
class AsyncRwLock {
public:
    explicit AsyncRwLock(Scheduler& scheduler) noexcept;
    ~AsyncRwLock();

    // Non-copyable, non-movable (stable address; WaitQueue identity).
    AsyncRwLock(const AsyncRwLock&) = delete;
    AsyncRwLock& operator=(const AsyncRwLock&) = delete;
    AsyncRwLock(AsyncRwLock&&) = delete;
    AsyncRwLock& operator=(AsyncRwLock&&) = delete;

    // --- Read acquisition ---
    void read_lock(WaitNode& node);
    void read_lock_until(WaitNode& node, Scheduler::deadline_t deadline);
    [[nodiscard]] bool try_read_lock();

    // --- Write acquisition ---
    void write_lock(WaitNode& node);
    void write_lock_until(WaitNode& node, Scheduler::deadline_t deadline);
    [[nodiscard]] bool try_write_lock();

    // --- Release ---
    void unlock_read() noexcept;
    void unlock_write() noexcept;

    // --- Cancellation ---
    [[nodiscard]] bool cancel(WaitNode& node);
};
```

### Rationale

1. **Consistency with AsyncMutex**: AsyncMutex uses explicit `lock(node)` /
   `unlock()` with caller-owned WaitNode. AsyncRwLock follows the same pattern.
2. **No guard type in existing primitives**: AsyncMutex, Semaphore, Event all
   use explicit release. Introducing a guard ONLY for RwLock creates API
   divergence without justification.
3. **Fiber migration**: A guard's destructor must call unlock from whatever
   worker the Fiber resumes on. This is safe (unlock is thread-safe under G),
   but adds complexity for no semantic gain in v1.
4. **Cancellation interaction**: If read_lock resolves Cancelled/Expired, no
   resource was acquired; a guard would need conditional construction. The
   WaitNode outcome pattern handles this cleanly.
5. **ABI stability**: No additional installed header types.

### Rejected: guard-returning API (Method B)

A `Result<ReadGuard>` / `Result<WriteGuard>` API would:
- Diverge from AsyncMutex/Semaphore/Event API style.
- Require move-only guard types with async suspension lifetime concerns.
- Add installed header surface for marginal safety gain.
- Complicate the cancellation path (guard must not unlock on Cancelled/Expired).

If a guard is desired in future, it can be a non-intrusive RAII wrapper added
without breaking the explicit API.

---

## Fairness policy

### Decision: Phase-fair FIFO prefix batching

This is the **FIFO-compatible writer-fair admission** policy. It is equivalent
to the policy used by Tokio's `sync::RwLock` and is the canonical anti-starvation
choice identified in `e12-sync-primitives-plan.md` §9.1.

### Rules

1. **No active writer AND waiters_ is empty**: a new reader acquires inline
   (fast path). If the FIFO queue is non-empty, the new reader MUST queue
   regardless of the queued waiters' modes.
2. **Once ANY waiter is queued**: all subsequently arriving readers MUST queue
   behind it. No reader barging past ANY queued waiter (writer OR reader).
3. **Writer release / last-reader release**: call `grant_from_head_locked()`:
   - Head is a writer → grant ONE writer (exclusive).
   - Head is a reader → grant the maximal consecutive reader prefix (batch).
4. **Reader batch**: only the contiguous read-waiter prefix starting at the
   queue head. The batch STOPS at the first writer waiter.
5. **No strict global FIFO completion order** is promised (readers within a
   batch resume concurrently), but **admission order** prevents starvation
   of BOTH writers AND queued readers.

### Deterministic counterexample (reader-bypass-reader)

```text
R1 active (holds read lock)
W1 queues (write_lock suspends)
R2 queues behind W1 (read_lock suspends)
W1 canceled/expired (removed from queue; R2 is now head)
R3 arrives (read_lock fast path)
```

Under the OLD rule ("no queued writer exists"), R3 would acquire inline,
bypassing R2. Under continuous reader traffic, R2 starves indefinitely.

Under the CORRECT rule ("waiters_ is empty"), R3 observes waiters_ non-empty
(R2 is still queued) and MUST queue behind R2. R2 is granted first.

### Admission recheck after registration

After a node is registered at the FIFO tail, the admission recheck MUST NOT
inspect only "no writer ahead of this node." It MUST call the unified
head-reconcile logic:

```text
grant_from_head_locked()
```

This function grants ONLY from the queue head:

- Head is a writer AND resource free → grant one writer.
- Head is a reader AND no writer active → grant maximal reader prefix.
- Otherwise → no grant (caller suspends).

A newly registered tail node can ONLY be granted if ALL preceding waiters
were already resolved and unlinked, making this node the new head. There is
no path that grants a tail node while earlier readers remain queued.

### Comparison matrix

| Policy | Advantage | Risk | Verdict |
| --- | --- | --- | --- |
| Reader preference | Reader throughput | Writer starvation under continuous readers | **REJECTED** |
| Writer preference | Prevents writer starvation | Reader convoy; excessive writer bias | **REJECTED** |
| Strict FIFO | Simple to explain | No reader batching; readers serialize unnecessarily | **REJECTED** |
| Phase-fair / FIFO prefix batching | Fair + reader concurrency balance | Slightly more complex state machine | **SELECTED** |

### Starvation guarantee

```text
An eligible queued waiter (writer OR reader) cannot be overtaken by
later-arriving readers, assuming current resource owners eventually release
and the Scheduler eventually runs runnable Fibers.
```

This is STRONGER than AsyncMutex's no-barging (M-H3): it prohibits both
reader-past-writer AND new-reader-past-queued-reader.

---

## State model

### RwLock state (primitive-local, mutated ONLY under global_mtx_)

```cpp
// Conceptual (not a public struct; fields live directly on AsyncRwLock).
std::size_t active_readers_;   // count of currently held read shares
bool writer_active_;           // true iff a writer holds exclusive access
Fiber* writer_owner_;          // the Fiber holding write lock (nullptr if none)
WaitQueue waiters_;            // single unified FIFO queue (readers + writers)
```

### Waiter mode

Each waiter carries a mode (read or write). Since WaitNode is mode-agnostic
(E10 design), the mode is stored via the existing `WaitNode::user_` pointer
(E12-E Queue precedent) pointing to a primitive-internal context struct:

```cpp
struct RwWaitCtx {
    enum class Mode : std::uint8_t { read, write };
    Mode mode;
};
```

### RwWaitCtx ownership and lifecycle

**The public caller does NOT construct or set `RwWaitCtx`.**

`read_lock` / `read_lock_until` / `write_lock` / `write_lock_until` internally
create a stack-local `RwWaitCtx` and manage the `user_` pointer:

```text
1. RwWaitCtx ctx{mode};             // stack-local, inside the lock function
2. node.set_user(&ctx);             // BEFORE registration
3. ... register / suspend / resume ...
4. node.set_user(nullptr);          // AFTER terminal resume, BEFORE return
```

Lifecycle proof obligations:

1. **Fiber stack alive during suspension**: the lock function's stack frame
   remains live across the suspension point (the Fiber's stack is preserved
   until the Fiber resumes — this is the fundamental Fiber/coroutine contract).
   Therefore `ctx` is address-stable and alive for the entire outstanding epoch.
2. **cancel/expire/grant only reads context while node is registered/linked**:
   the Scheduler seam reads `node.user()` ONLY under G + W while the node is
   still linked. After resolve_ + unlink, the winner does NOT re-read user().
3. **Address stability**: `ctx` lives on the Fiber's stack; the Fiber stack does
   not move. The address is stable from set_user to clear.
4. **Node initial state**: `node.user()` MUST be nullptr on entry to any
   lock function. A non-null user on entry is a caller contract violation
   (debug assert + fail-fast; same category as reusing an outstanding node).
5. **Clear before return**: user_ is set to nullptr BEFORE the lock function
   returns to the caller. No dangling pointer survives the call.

### `WaitNode::user_` contract

`user_` is a controlled primitive-per-operation context hook. Authorized
production users: AsyncQueue (E12-E), AsyncRwLock (E12-F). Each primitive sets
`user_` before registration and clears it after terminal resolution. `user_` is
read ONLY by the owning Scheduler seam under G + W while the node is linked. It
is NOT a general-purpose user payload. The header comment on `user()` in
`wait_node.hpp` names both primitives and retains the linked-node lifetime
restriction.

### Legal states

| active_readers_ | writer_active_ | Meaning |
| --- | --- | --- |
| 0 | false | Unlocked (free) |
| > 0 | false | Read-locked (N readers) |
| 0 | true | Write-locked (one writer) |

### Illegal states (invariant violations)

| Condition | Enforcement |
| --- | --- |
| active_readers_ > 0 AND writer_active_ == true | Cannot occur (admission prevents); debug assert in unlock paths |
| active_readers_ underflow (unlock_read when 0) | Caller contract violation; debug assert |
| unlock_write without writer ownership | Caller contract violation; debug assert |
| unlock_write when writer_active_ == false | Caller contract violation; debug assert |
| waiter registered in more than one queue | E10 C8 (register_ CAS rejects); structurally impossible |
| waiter mode changes while queued | Mode is immutable after registration (caller-owned const context) |
| cross-Scheduler registration | cancel() membership scan prevents; wrong-RwLock returns false |
| destruction with active owner or queued waiter | Caller contract violation; debug assert + ~WaitQueue assert |

Internal authority violations (forgeable bypass) are NOT converted to
recoverable errors. This document distinguishes TWO categories:

### Category A: Caller misuse / lifetime contract violation

```text
Debug: assert with specific diagnostic message.
Release: no recovery semantics. The behavior is undefined in the same
         sense as AsyncMutex non-owner unlock or Semaphore double-release.
         This is NOT a safety-detection boundary; it is a caller contract.
```

Applies to:

- unlock_read when active_readers_ == 0 (underflow)
- unlock_write without writer ownership
- unlock_write when writer_active_ == false
- destruction with active owner or queued waiter
- `node.user() != nullptr` on entry to a public lock function (caller
  provided a WaitNode that is already outstanding or has stale user_ —
  this is caller-provided WaitNode misuse, same category as reusing an
  outstanding node)
- recursive write_lock by current owner (debug assert; try_write_lock
  returns false without assertion)

### Category B: Internal linked-queue / authority corruption

```text
Debug: assert with specific diagnostic (node address, observed state,
       expected state, queue identity).
Release: deterministic fail-fast (std::abort or equivalent). NOT
         skip-and-continue, NOT return error, NOT undefined behavior.
```

Applies to:

- linked node is already terminal (Unlink Law violation)
- node.user() == nullptr for a LINKED node during grant traversal
- mode is neither read nor write for a LINKED node
- resolve_(Woken) returns false for a valid linked eligible node
- home/queue identity mismatch detected internally

### Classification rationale

`node.user() != nullptr` on PUBLIC LOCK ENTRY is Category A (caller misuse):
the caller provided a WaitNode with stale state. The Scheduler has not yet
touched the queue; no internal corruption exists. Release trusts the caller.

`node.user() == nullptr` for a LINKED node DURING GRANT is Category B
(internal corruption): the Scheduler itself set user_ before registration
and no external actor should clear it while linked. Observing null means
internal state was corrupted. Release aborts.

This matches the existing AsyncMutex/Semaphore/Event pattern: debug assert
is the diagnostic for caller misuse; Release trusts the caller. Internal
corruption (e.g., WaitQueue head_ pointing to a terminal node) is a
separate, stronger category that does not exist in normal operation.

---

## Waiter model

Each wait epoch carries (via WaitNode + RwWaitCtx):

```text
mode            = read | write (immutable after registration)
epoch identity  = WaitNode address (E10 identity model)
Scheduler identity = the Scheduler& the AsyncRwLock borrows
owner/primitive identity = the AsyncRwLock's waiters_ queue
terminal state  = WaitNode::state_ (Detached/Registered/Woken/Cancelled/Expired)
deadline/cancellation registration = E11 TimerRegistration (if timed)
```

---

## Lock ordering

### Lock order table

| Lock / Authority | Order | Notes |
| --- | --- | --- |
| Scheduler::global_mtx_ (G) | 1 (outermost) | All authoritative decisions |
| AsyncRwLock::waiters_.mtx() (W) | 2 (innermost) | Structural queue operations |
| Timer registration | Under G | Timer pool/heap mutations under G |
| Runnable publication | Under G | route_runnable_locked after resource commit |

### Per-operation lock holdings

| Operation | Holds |
| --- | --- |
| try_read_lock / try_write_lock (fast path) | G + W |
| read_lock / write_lock (queued registration) | G + W (register + admission recheck) |
| cancel | G + W (resolve + unlink + head reconcile + publication) |
| deadline expiry | G + W (resolve + unlink + head reconcile + publication) |
| unlock_read / unlock_write (grant) | G + W (resource commit + publication) |
| deadline admission | G + W (timer registration under G) |

### Publication timing

Publication (route_runnable_locked) occurs UNDER G, AFTER the resource commit
(active_readers_ increment or writer_active_/writer_owner_ set). This matches
the Mutex owner-before-publication pattern. The queue mtx W is held during
the resolve + unlink; publication follows in the same G critical section.

---

## Read acquisition

### Fast path (try_read_lock / read_lock inline admission)

A reader acquires inline IFF ALL hold:

```text
writer_active_ == false
AND waiters_ is empty (queue head is nullptr)
```

The "waiters_ is empty" check prevents ALL forms of barging: no reader may
bypass ANY queued waiter, whether writer or reader. This is strictly stronger
than "no queued writer" and eliminates the reader-bypass-reader starvation
counterexample.

Implementation: under G + W, check `waiters_.head_ == nullptr` AND
`writer_active_ == false`. If both hold: active_readers_++, return true.

For `try_read_lock()`: same check, return false if not admissible.

### Queued path (read_lock)

1. Internally create stack-local `RwWaitCtx{mode=read}`; set node.set_user(&ctx).
   (See RwWaitCtx ownership section — caller does NOT construct this.)
2. Under G + W: register node at FIFO tail (register_wait_locked).
3. Admission recheck via unified head reconcile (closes lost-wake window):
   - Call `grant_from_head_locked()`.
   - If this node became the queue head (all prior waiters already resolved)
     AND writer_active_ == false: the grant logic resolves this node Woken,
     active_readers_++, unlink, return.
   - Otherwise: commit suspension (make_waiting + context_switch).
   - **There is NO path that checks "no writer ahead of this specific node".**
     Grant is exclusively head-driven.
4. On resume: read outcome from node (Woken / Cancelled / Expired).
   - Woken: the caller now holds a read share (active_readers_ was incremented
     by the grantor BEFORE publication).
   - Cancelled/Expired: no resource acquired; no unlock_read needed.
5. Before function return: node.set_user(nullptr) (clear context pointer).

### Linearization point (read acquisition)

The instant `active_readers_` is incremented under G (either inline admission
or batch grant commit). This is the single atomic point at which the read share
is owned.

---

## Write acquisition

### Fast path (try_write_lock / write_lock inline admission)

A writer acquires inline IFF ALL hold:

```text
active_readers_ == 0
writer_active_ == false
waiters_ is empty (no queued waiter has FIFO priority)
```

The "waiters_ empty" check enforces no-barging (M-H3 precedent): a queued
waiter (reader or writer) with FIFO priority cannot be bypassed.

### Queued path (write_lock)

1. Internally create stack-local `RwWaitCtx{mode=write}`; set node.set_user(&ctx).
   (Caller does NOT construct this.)
2. Under G + W: register node at FIFO tail.
3. Admission recheck via unified head reconcile:
   - Call `grant_from_head_locked()`.
   - If this node is the FIFO head AND active_readers_ == 0 AND
     writer_active_ == false: resolve Woken inline,
     writer_active_ = true, writer_owner_ = current Fiber, unlink, return.
   - Otherwise: commit suspension.
4. On resume:
   - Woken: writer_active_ == true, writer_owner_ == this Fiber (committed
     by the grantor BEFORE publication).
   - Cancelled/Expired: no resource acquired.
5. Before function return: node.set_user(nullptr) (clear context pointer).

### Linearization point (write acquisition)

The instant `writer_active_` is set true and `writer_owner_` is committed
under G. For handoff: this is in the grantor's critical section (winner-
before-publication, matching mutex_handoff_one_locked).

---

## Read unlock (unlock_read)

### Preconditions

```text
active_readers_ > 0
writer_active_ == false  (implied by legal state invariant)
```

Caller contract: the calling context holds a read share. v1 does NOT track
per-fiber reader identity (see Ownership section).

### Behavior

Under G + W:

1. `active_readers_--`.
2. If `active_readers_ > 0` after decrement: return (other readers still hold;
   no grant needed).
3. If `active_readers_ == 0` (this was the last reader): attempt grant to
   queue head.

### Last-reader grant logic

When the last reader releases:

1. If queue is empty: return (unlocked, no waiters).
2. Call `grant_from_head_locked()` (the same unified reconcile used everywhere):
   - Head mode == write: grant ONE writer (writer grant seam).
   - Head mode == read: grant the reader prefix batch (batch grant seam).

Note: a reader at the head when the last reader releases CAN occur if all
writers that were ahead were canceled/expired (removing them from the queue).
The unified grant logic handles both cases uniformly.

**There is NO "skip stale/canceled head nodes" step.** Under the E10 Unlink
Law, a terminal node is unlinked in the SAME critical section as its winning
CAS. Therefore a linked node is NEVER already-terminal. Encountering one is an
internal invariant violation (see fail-fast rules in batch grant section).

---

## Write unlock (unlock_write)

### Preconditions

```text
writer_active_ == true
writer_owner_ == current Fiber (g_worker->current)
```

Non-owner unlock_write or unlock_write while unlocked: caller precondition
violation (debug assert, no mutation).

### Behavior

Under G + W:

1. `writer_active_ = false; writer_owner_ = nullptr`.
2. Attempt grant to queue head.

### Post-write grant logic

1. If queue is empty: return (unlocked, no waiters).
2. If queue head mode == write: grant ONE writer (writer grant seam).
3. If queue head mode == read: grant the reader prefix batch (batch grant seam).

### Grant-during-new-waiter

New waiters cannot insert into the batch during grant: the entire grant
sequence runs under G + W. Registration also requires G + W. Therefore no
new waiter can appear between batch identification and batch commit.

---

## Reader batch grant

This is the core new mechanism for E12-F.

### Batch member selection

Starting from the queue head, collect the maximal prefix of consecutive
read-mode waiters. STOP at:

- A write-mode waiter (MUST NOT skip a valid queued writer).
- The queue tail.

### Fail-fast on invalid linked nodes (E10 Unlink Law consequence)

Under the E10 Unlink Law, terminal transition + unlink occur in the SAME
critical section. Therefore, while holding G + W and traversing linked nodes,
the following conditions are INTERNAL INVARIANT VIOLATIONS, not recoverable
cases:

```text
- linked node is already terminal (Cancelled/Expired/Woken)
- node.user() == nullptr
- mode is neither read nor write
- valid linked reader's resolve_(Woken) returns false
- home/queue identity mismatch
```

Required enforcement:

```text
Debug: assert with specific diagnostic (node address, observed state, expected
       state, queue identity).
Release: deterministic fail-fast (std::abort or equivalent; NOT skip-and-
         continue, NOT return error, NOT undefined behavior).
```

**Do NOT skip and continue scanning.** A linked-but-terminal node means the
Unlink Law was violated somewhere upstream. Continuing masks corruption.

The batch is identified under G + W. No new node can be appended during
identification (registration requires G + W). No concurrent cancel/expire can
insert into the batch traversal: cancel/expire also require G + W, and the
batch holds G + W continuously. Therefore per-node CAS failure (resolve_
returning false) CANNOT occur for a valid linked reader. If it does, this is
an invariant violation (fail-fast).

### Race matrix correction

Cancel/deadline/grant races are determined by who first acquires G + W:

- If grant holds G + W first: all linked nodes in the prefix are still
  registered (not terminal). Every resolve_(Woken) succeeds. No CAS failure.
- If cancel/expire holds G + W first: it resolves + unlinks the node. The
  node is NO LONGER linked. The grant traversal never sees it.

There is NO scenario where the batch holds continuous G + W and encounters
a per-node concurrent cancel insertion. The old "If CAS fails: skip" design
is REMOVED.

### Batch linearization: Method B (commit-all-then-publish)

**Selected approach**: commit the entire batch atomically, then publish each
winner individually.

Under ONE continuous G + W critical section:

```text
1. Identify the reader prefix (batch members).
2. For each batch member (in FIFO order):
   a. Call claim_waiter_woken_no_publish_locked(node) — the canonical helper.
   b. Collect the returned publication record into a local intrusive list.
3. active_readers_ += (number of claimed winners).
4. Release W (queue structural operations complete).
5. For each collected publication record (under G, W released):
   a. route_runnable_locked(record) — publication.
```

### Canonical no-publish grant helper

A new Scheduler-private helper (name adjustable):

```cpp
// Scheduler-private. Called under G + W.
// Does NOT call route_runnable_locked.
// Returns the claimed WaitNode* (same as input; for chaining convenience).
WaitNode* claim_waiter_woken_no_publish_locked(WaitNode& node) noexcept;
```

Responsibilities (all under G + W):

```text
1. resolve_(Woken) — winner CAS. MUST succeed for a valid linked node
   (fail-fast if not; see invariant violations above).
2. unlink_locked — remove from WaitQueue.
3. Retire bound TimerRegistration (if timed wait; under G).
4. Update active_deadline_count_ (decrement if timer was active).
5. Recompute earliest deadline when needed (if retired timer was heap min).
6. Decrement waiting_waitq_count_ (terminal accounting).
7. Clear node.next_ / node.prev_ (node is now unlinked from main queue).
```

Does NOT:

```text
- Allocate (no dynamic memory).
- Throw exceptions (noexcept; all operations are no-throw).
- Duplicate timer lifecycle logic (calls existing retire helper).
- Allow double accounting (one call per node; node is unlinked after).
- Call route_runnable_locked (publication is caller's responsibility).
- Return a separate ClaimRecord struct (removed; see below).
```

Compatibility:

- Works for both plain waiters (no timer) and timed waiters (timer retired).
- Works for reader batch grant AND writer single grant.
- Reusable by any future primitive that needs resolve + unlink + accounting
  without immediate publication.

**Publication data storage**: the caller (batch loop) caches `Fiber*` and
`WorkerState*` owner (looked up from `Scheduler::fiber_owner_` under G) BEFORE
clearing temporary links. No separate ClaimRecord type is needed. The intrusive
WaitNode list IS the sole publication storage mechanism.

### Why Method B

- **Clear linearization point**: the instant active_readers_ is incremented
  by batch_size is the single point at which ALL batch members simultaneously
  own their read shares.
- **No mid-batch writer insertion**: G + W held throughout identification +
  commit; no registration can interleave.
- **No CAS failure path**: under continuous G + W, all linked prefix nodes
  are guaranteed registered. resolve_(Woken) always succeeds. No rollback.
- **Publication order does not affect ownership**: all batch members own
  their shares after step 3; publication is a notification, not a grant.

### Proof obligations

1. **No allocation failure**: the batch collector uses NO dynamic allocation.
   The intrusive WaitNode list is the sole storage.
2. **No unstable waiter addresses**: batch members are identified by WaitNode*
   (stable; caller-owned, address-stable while outstanding — E10 §3).
3. **Publication failure**: route_runnable_locked cannot fail (it enqueues
   on a deque; if allocation fails, this is a system-level OOM — same as
   all existing publication paths).
4. **One waiter's publication anomaly does not revoke others**: ownership
   was committed at step 3. Publication is best-effort notification.
5. **Exactly-once completion**: each node's resolve_ CAS is the single
   authority. claim_waiter_woken_no_publish_locked is called exactly once
   per node per epoch.
6. **Cancellation cannot win after commit**: once resolve_(Woken) succeeds,
   the node is terminal + unlinked. A concurrent cancel cannot find it.

### Intrusive publication list (corrected lifecycle)

The batch builds a temporary intrusive list of claimed winners using the
WaitNode's own next_/prev_ pointers AFTER unlink from the main queue.

**Ownership**: this is Scheduler-private temporary ownership. The nodes are
already unlinked from the WaitQueue; no other resolver can reach them.

**Publication loop (CORRECTED — no post-publication node access)**:

```cpp
// Pseudocode (inside Scheduler::rwlock_grant_readers_locked)
WaitNode* pub_head = nullptr;
WaitNode* pub_tail = nullptr;
std::size_t granted = 0;

// Pass 1: identify + claim + collect (under G + W)
for (WaitNode* n = waiters.head_; n != nullptr; ) {
    WaitNode* next = n->next_;  // cache BEFORE claim (claim clears next_/prev_)
    auto* ctx = static_cast<RwWaitCtx*>(n->user());
    // INVARIANT: ctx != nullptr, ctx->mode == read (fail-fast otherwise)
    if (ctx == nullptr || ctx->mode != RwWaitCtx::Mode::read) {
        // invariant violation — fail-fast (debug assert + abort)
    }
    claim_waiter_woken_no_publish_locked(*n);
    // claim has cleared n->next_, n->prev_; n is unlinked from main queue.
    // Thread onto local publication list (reusing now-free next_/prev_):
    n->next_ = nullptr;
    n->prev_ = pub_tail;
    if (pub_tail) pub_tail->next_ = n; else pub_head = n;
    pub_tail = n;
    ++granted;
    n = next;  // advance using CACHED next (not n->next_ which was cleared)
}

// Commit
active_readers += granted;

// Release W (queue structural operations complete)
// Publication (still under G; W released)
WaitNode* w = pub_head;
while (w != nullptr) {
    // Cache ALL needed data BEFORE route:
    WaitNode* pub_next = w->next_;      // next in temporary list
    Fiber* fib = w->fiber();            // the winner's Fiber
    WorkerState* owner = fiber_owner_[fib]; // canonical routing owner
    w->next_ = nullptr;                 // clear temporary linkage
    w->prev_ = nullptr;                 // clear temporary linkage
    // After this point, DO NOT dereference w.
    // The Fiber may resume and destroy/reuse the WaitNode immediately.
    route_runnable_locked(fib, owner);  // uses cached data only
    w = pub_next;                       // advance to cached next
}
```

`fiber_owner_` is the Scheduler's `std::unordered_map<Fiber*, WorkerState*>`
(under `global_mtx_`). This is the canonical routing data source used by all
existing publication paths (wake_wait_one_locked, mutex_handoff_one_locked,
sem_release, etc.). The lookup is safe under G.

**Critical lifecycle rules**:

1. `next` is cached BEFORE claim (claim clears next_/prev_).
2. In the publication loop, `pub_next` is cached BEFORE route_runnable_locked.
3. After route_runnable_locked, the current WaitNode MUST NOT be dereferenced.
   The woken Fiber may resume on another worker and immediately destroy or
   reuse the WaitNode (caller-owned; lifecycle ends on terminal resolution).
4. Temporary next_/prev_ linkage is cleared BEFORE publication.
5. No other worker can destroy a yet-to-be-published node: the node is
   terminal (Woken) and unlinked. The caller's Fiber is suspended until
   publication routes it. The WaitNode's E10 contract guarantees the caller
   does not destroy/reuse the node until the lock function returns, which
   requires publication to complete.

**Dedicated lifecycle test required**:
`batch_publication_does_not_access_published_node` — verifies that the
publication loop advances via cached pointers and never touches a node after
its route_runnable_locked call.

No dynamic allocation. No exception risk. O(batch_size) under G + W.

---

## Cancellation and deadline races

### Precedence (resource-first, consistent with E12-A/B/C/D/E)

At admission (under G + W):

```text
1. Resource admissible (read: no writer active AND waiters_ empty;
   write: free + head) → resolve Woken inline. Resource wins over a due
   deadline.
2. Not admissible + deadline already due → resolve Expired inline (E11 I5).
3. Not admissible + future deadline → register + timer; normal race.
```

For a registered timed wait, RESOURCE_WAKE / TIMER_EXPIRE / CANCEL compete
through the single WaitNode::resolve_ CAS (exactly-once authority).

### Competition matrix

| Race | Winner | Resource state | Queue state | Publication |
| --- | --- | --- | --- | --- |
| read grant vs cancel | Who first holds G + W | If grant first: active_readers_++ | Grant: node unlinked by claim helper | Grant: published; cancel cannot find node |
| read grant vs deadline | Who first holds G + W | Same as above | Same as above | Same as above |
| write grant vs cancel | Who first holds G + W | If grant first: writer_active_=true | Grant: node unlinked by claim helper | Grant: owner commit THEN publish |
| write grant vs deadline | Who first holds G + W | Same as above | Same as above | Same as above |
| reader batch vs one reader cancel | Who first holds G + W | If batch first: all prefix readers granted (no CAS failure possible) | If cancel first: node already unlinked; batch never sees it | Batch publishes only nodes it claimed; cancel publishes its own winner |
| last reader unlock vs writer timeout | Who first holds G + W | If timeout first: writer node terminal + unlinked; last reader sees shorter queue | Writer unlinked by expire | No publication for expired writer |
| writer unlock vs reader deadline pump | Who first holds G + W | If pump first: expired readers unlinked; batch sees shorter prefix | Expired readers removed by expire | Batch publishes only linked readers it finds |
| destruction vs queued waiter | N/A (contract violation) | Debug assert fires | ~WaitQueue asserts empty | No publication; caller must drain first |

**Key correction**: there is no "per-node CAS race" within a batch traversal.
The batch holds G + W continuously. Cancel/expire require G + W. They cannot
interleave. A node is either already unlinked (cancel won G + W earlier) or
still linked and registered (batch wins G + W now). No intermediate state.

### Invariants preserved

```text
one wait epoch → one terminal winner (resolve_ CAS)
at most one runnable publication per winner
winner owns unlink/removal obligation
loser does not unlink or publish
```

### Reader batch extension to winner authority

The batch grant resolves MULTIPLE nodes in ONE critical section. Each node's
resolve_ CAS is still the individual winner authority. The batch does NOT
create a "group winner" — it is a sequence of individual claim operations
(claim_waiter_woken_no_publish_locked) under one lock hold. Under continuous
G + W, every linked prefix node is guaranteed registered; therefore every
claim succeeds. The extension is structurally identical to Event::set()'s
drain loop (event_set_broadcast resolves ALL registered waiters in one G
hold), with the addition that RwLock's batch is prefix-bounded (stops at
first writer) rather than draining the entire queue.

---

## Cancel and expiry queue advancement (head reconcile)

### Problem

When a cancel or deadline expiry removes the queue HEAD, the newly exposed
head may be immediately resource-admissible. Without reconcile, the exposed
head would wait until the next unlock — violating liveness.

```text
R1 active
W1 queued (head)
R2 queued behind W1
W1 canceled or expired
```

After W1 removal, R2 is the new head. Since `writer_active_ == false` and
`active_readers_ > 0` (R1 still holds), R2 CAN join the existing reader phase
immediately. It MUST NOT wait for R1's next unlock_read.

### Normative rule

**Any terminal resolution (cancel or expiry) that unlinks a node MUST call
`rwlock_grant_from_head_locked(...)` in the SAME Scheduler authority flow
(same G + W hold) after the unlink, if the queue is non-empty.**

This is mandatory for BOTH cancel and expiry, regardless of whether the
removed node was the head. (If a non-head node is removed, the grant call is
a no-op because the head is unchanged and was already inadmissible.)

### rwlock_cancel seam (full-state)

The cancel seam MUST have access to complete RwLock state to perform reconcile:

```text
rwlock_cancel(
    WaitQueue& waiters,
    std::size_t& active_readers,
    bool& writer_active,
    Fiber*& writer_owner,
    WaitNode& node
) -> bool
```

Cancel winner normative flow (under G + W):

```text
1. Membership check: node belongs to this WaitQueue (contains_locked).
   If not: return false (wrong-queue / detached / already-terminal).
2. resolve_(Cancelled) — winner CAS.
   If CAS fails (grant/expire already won): return false.
3. unlink_locked — remove node from queue.
4. Retire bound TimerRegistration (if timed wait).
5. Update active_deadline_count_ (decrement if timer was active).
6. Decrement waiting_waitq_count_ (terminal accounting).
7. Capture publication data: Fiber* + WorkerState* owner
   (from fiber_owner_ map, under G).
8. Head reconcile: if queue is non-empty after unlink:
   rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                 writer_owner)
   — this may grant the newly exposed head (writer or reader prefix).
9. Release W.
10. Publish cancel winner: route_runnable_locked(cancel_fiber, cancel_owner).
11. Publish any newly granted winners (from step 8): each exactly once.
```

Publication order: the cancel winner and any newly granted winners are ALL
published in the same G hold. Each is published exactly once. The cancel
winner's publication is independent of the grant publications.

### Deadline expiry: RwLock-specific expiry seam

The generic `expire_wait(WaitQueue&, WaitNode&)` performs resolve + unlink +
accounting + publication for ONE node. It does NOT perform head reconcile.
RwLock requires reconcile after expiry. Therefore:

**Selected approach: RwLock-specific expiry seam.**

```text
rwlock_expire_wait(
    WaitQueue& waiters,
    std::size_t& active_readers,
    bool& writer_active,
    Fiber*& writer_owner,
    WaitNode& node
) -> bool
```

Expiry winner normative flow (under G + W):

```text
1. resolve_(Expired) — winner CAS.
   If CAS fails (grant/cancel already won): return false.
2. unlink_locked — remove node from queue.
3. TimerRegistration state: CONSUMED (pump claimed it).
4. Update active_deadline_count_ (decrement).
5. Decrement waiting_waitq_count_ (terminal accounting).
6. Capture publication data: Fiber* + WorkerState* owner.
7. Head reconcile: if queue is non-empty after unlink:
   rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                 writer_owner)
8. Release W.
9. Publish expired waiter: route_runnable_locked(expired_fiber, expired_owner).
10. Publish any newly granted winners (from step 7): each exactly once.
```

### Timer routing: how pump_deadlines_locked reaches rwlock_expire_wait

The existing `pump_deadlines_locked` calls generic `expire_wait` for each due
timer. RwLock timers must route to `rwlock_expire_wait` instead.

**Mechanism**: TimerRegistration carries a controlled owner-context hook
(precedent: E12-E Queue's `on_resolve_` + `owner_ctx_` pattern).

For RwLock timed waits, the TimerRegistration is configured at registration:

```text
on_expire_reconcile_ = &Scheduler::rwlock_timer_expire_reconcile
owner_ctx_           = pointer to a stable RwLockExpireCtx
```

Where:

```cpp
// Stable context owned by the AsyncRwLock (or embedded in its state).
// Address-stable for the lifetime of the AsyncRwLock.
struct RwLockExpireCtx {
    WaitQueue* waiters;
    std::size_t* active_readers;
    bool* writer_active;
    Fiber** writer_owner;
};
```

The pump flow for an RwLock timer:

```text
pump_deadlines_locked:
  for each due ACTIVE timer:
    try_claim_expiry (ACTIVE -> CONSUMED)
    if on_expire_reconcile_ != nullptr:
        call on_expire_reconcile_(owner_ctx_, node)
        // This calls rwlock_expire_wait with full state
    else:
        call generic expire_wait(q, node)
```

Hook contract:

```text
Owner: the AsyncRwLock primitive (ctx is embedded in or owned by the lock).
Lifetime: ctx outlives all TimerRegistrations bound to this lock's waiters
          (caller must drain all waits before destruction; ~AsyncRwLock asserts
          no queued waiters, which implies no outstanding timers).
noexcept: the hook MUST NOT throw.
Lock order: called under G; the hook acquires W internally (G -> W preserved).
Destruction: ~AsyncRwLock asserts no outstanding waits; therefore no timer
             can fire after destruction begins.
```

### Rejected: generic post-terminal hook on WaitNode

A generic hook on WaitNode would expose reconcile capability to all primitives
and blur the authority boundary. The RwLock-specific TimerRegistration hook
follows the Queue's `on_resolve_` precedent and keeps the reconcile logic
contained within the RwLock's Scheduler seams.

### Tests for head reconcile

```text
head_writer_cancel_grants_reader_prefix_immediately
head_writer_expiry_grants_reader_prefix_immediately
head_reader_cancel_grants_remaining_reader_prefix_immediately
head_reader_expiry_grants_remaining_reader_prefix_immediately
cancel_reconcile_publishes_each_winner_once
expiry_reconcile_retires_timers_once
```

---

## Linearization points

| Operation | Linearization point |
| --- | --- |
| read_lock (fast path) | active_readers_++ under G |
| read_lock (batch grant) | active_readers_ += batch_size under G (all batch members simultaneous) |
| write_lock (fast path) | writer_active_ = true under G |
| write_lock (handoff grant) | writer_active_ = true + writer_owner_ = winner under G |
| unlock_read (last reader) | active_readers_ reaches 0; grant begins |
| unlock_write | writer_active_ = false; grant begins |
| cancel | resolve_(Cancelled) CAS success |
| deadline expire | resolve_(Expired) CAS success |

---

## Publication and exactly-once rules

1. A winner is published (route_runnable_locked) EXACTLY ONCE.
2. Publication occurs AFTER resource commit (owner-before-publication for
   writer; active_readers_ commit for reader batch).
3. A loser (cancel/expire won the CAS) is published by the cancel/expire
   path, NOT by the grant path.
4. While continuous G + W are held, resolve_(Woken) failure for a valid
   linked eligible node is an internal invariant violation and
   deterministically fail-fasts. No skip-and-continue path exists.
5. No second wake: once a node is Woken, no other resolver can re-resolve it.

---

## Destruction contract

| State at ~AsyncRwLock | Behavior |
| --- | --- |
| Unlocked, no waiters | Normal destruction |
| active_readers_ > 0 | Caller contract violation; debug assert |
| writer_active_ == true | Caller contract violation; debug assert |
| Queued readers | ~WaitQueue asserts head_ == nullptr (debug) |
| Queued writers | ~WaitQueue asserts head_ == nullptr (debug) |
| Outstanding deadline callbacks | Caller must resolve all waits before destruction |
| Committed winner not yet published | Cannot occur: commit + publication are in the same G CS |

The destructor:

```cpp
~AsyncRwLock() {
    assert(active_readers_ == 0 && "AsyncRwLock destroyed with active readers");
    assert(!writer_active_ && "AsyncRwLock destroyed with active writer");
    // ~WaitQueue asserts head_ == nullptr (existing E10 invariant)
}
```

No cancel-all. No wake-all. No silent detach/leak. Consistent with
AsyncMutex, Semaphore, Event, Condition, Queue.

### Release behavior for destruction/misuse (normative)

```text
Debug: assert fires with specific diagnostic.
Release: no recovery semantics. The caller lifetime contract is violated.
         The behavior is undefined in the same sense as destroying an
         AsyncMutex with a held lock or a Semaphore with queued waiters.
```

This is a **caller lifetime contract**, NOT a safety-detection boundary.
RwLock follows the EXACT same strategy as AsyncMutex/Semaphore/Event:

- Debug assert is the diagnostic tool.
- Release trusts the caller.
- No std::terminate, no abort, no no-op fallback, no recoverable error.
- The implementation does NOT introduce a new Release fail-fast mechanism
  that differs from existing primitives.

Rationale for NOT diverging from existing primitives: introducing a Release
abort for RwLock alone would create an inconsistent contract surface across
E12 primitives. If Release fail-fast is desired, it must be a cross-primitive
design decision (separate ADR), not an RwLock-local choice.

---

## Select relationship

```text
AsyncRwLock v1 does NOT participate in Select.
```

### Rationale

1. Select (E13) currently supports Event arms and Timer arms. Adding RwLock
   as a Select arm would require:
   - A new SelectPort on AsyncRwLock.
   - Rollback semantics if a different arm wins (a "tentative read acquisition"
     that must be released if another arm is selected).
   - New ownership/publication interactions between Select's winner-finalization
     and RwLock's batch grant.
2. The existing Select architecture is optimized for level-triggered readiness
   (Event SET). RwLock's admission is NOT level-triggered: a read lock is
   admissible only when no writer is active/queued, which is an edge-triggered
   condition that changes on every unlock.
3. No existing authority model evidence suggests Select integration can be
   added without new rollback or publication problems.

Select integration is recorded as a **future independent phase** (post-v1).

---

## Rejected designs

### 1. Only reader_count + writer bool, no unified wait queue

**Rejected.** Without a unified FIFO queue, there is no structural authority
to enforce "no reader barges past a queued writer." Two separate queues
(readers/writers) require a cross-queue admission oracle that duplicates
Scheduler authority and creates ordering ambiguity.

### 2. Reader always fast-path, only queue when writer active

**Rejected.** This is reader-preference. A continuous stream of readers
starves writers indefinitely. Violates the anti-starvation goal.

### 3. Writer queued but new readers still allowed to barge

**Rejected.** Same as #2 with extra steps. The queued writer is overtaken
by later readers. Violates writer-fair admission.

### 4. unlock with notify_all(), let waiters race

**Rejected.** Violates exactly-once publication (multiple waiters woken,
only one can acquire). Creates thundering herd. Violates the FIFO no-barging
rule (a later reader could win the race against an earlier queued writer).
The existing E10 model requires the grantor to select the winner, not the
waiters.

### 5. Reader batch: publish one, then decide next

**Rejected.** Creates a mid-batch seam where a writer could be admitted
between individual reader grants. The "batch" degenerates into sequential
single-reader grants with no concurrency benefit. Violates the phase-fair
admission promise.

### 6. Cancellation loser unlinks itself

**Rejected.** Violates the E10 Unlink Law (§7): only the winner CAS owner
unlinks. A loser (e.g., grant lost to cancel) does NOT unlink; the cancel
winner already unlinked the node.

### 7. sleep_for to test writer starvation

**Rejected.** AGENTS.md §6.3: "sleep_for MAY be used during diagnosis but
MUST NOT be the proof of ordering, liveness, absence of a lost wake, or
exactly-once publication." Use deterministic phase seams and barriers.

### 8. Two readers simultaneously upgrade

**Rejected.** Upgrade is explicitly out of v1 scope. Even in a future with
upgrade, simultaneous upgrade is a deadlock or a lost-update hazard.

### 9. Destructor silently cancels all waiters

**Rejected.** Violates the destruction contract established by all existing
primitives. The caller MUST ensure all waits are terminal before destruction.
Silent cancel-all would mask caller bugs and create publication during
destruction (dangling Fiber pointers if the Scheduler is also dying).

### 10. Read/write lock as Select arms for Select reuse

**Rejected.** See Select relationship section. Creates new rollback semantics,
tentative acquisition, and cross-arm publication interactions that the current
Select architecture does not support.

---

## Ownership and unlock API

### Writer ownership

Writer ownership is tracked by Fiber* identity (writer_owner_), matching
AsyncMutex's owner_ model. Only the Fiber that acquired the write lock may
call unlock_write. Non-owner unlock_write is a caller precondition violation
(debug assert).

### Reader ownership

v1 does NOT track per-fiber reader identity. `active_readers_` is a global
count. A read share is a **caller-managed transferable capability**: whoever
holds a share may release it, regardless of Fiber identity or thread context.

### Calling context table (normative)

| Operation | Any running Fiber | Current writer owner (recursive) | External OS thread | Notes |
| --- | --- | --- | --- | --- |
| `try_read_lock()` | Allowed | Allowed | Allowed | No Fiber identity needed; pure state check + increment |
| `try_write_lock()` | Allowed | Returns false / no mutation | **FORBIDDEN** | On success records `writer_owner_ = g_worker->current`; recursive call by current owner returns false (matching AsyncMutex try_lock) |
| `read_lock(node)` | Allowed | N/A | N/A | Suspending; requires current Fiber context |
| `read_lock_until(node, dl)` | Allowed | N/A | N/A | Suspending; requires current Fiber context |
| `write_lock(node)` | Allowed | N/A | N/A | Suspending; requires current Fiber context |
| `write_lock_until(node, dl)` | Allowed | N/A | N/A | Suspending; requires current Fiber context |
| `unlock_read()` | Allowed | Allowed | Allowed | v1 does not track reader identity; any holder may release |
| `unlock_write()` | Allowed (owner only) | Allowed (IS the owner) | **FORBIDDEN** | Checks `writer_owner_ == g_worker->current`; non-owner is debug assert |
| `cancel(node)` | Allowed | Allowed | Allowed | Serialized by G + W; queue-identity-safe |

### try_write_lock semantics

`try_write_lock()` on success MUST set `writer_owner_` to the current Fiber.

- **Any currently running Fiber**: allowed. If the lock is free and the queue
  is empty, the calling Fiber becomes the owner.
- **Current writer owner recursively calling**: returns false, no mutation.
  This matches AsyncMutex `try_lock()` behavior for recursive calls by the
  current owner (debug assert in the suspending `write_lock`; `try_write_lock`
  simply returns false without assertion).
- **External OS thread**: FORBIDDEN. There is no `g_worker->current` to record
  as owner. The operation cannot succeed.

Note: "non-owner Fiber" is NOT forbidden for `try_write_lock` — a Fiber that
does not currently own the lock is exactly the expected successful caller.
The distinction is: recursive call by the CURRENT owner returns false.

### unlock_read allows external threads

Consistent with Semaphore::release() being callable from external threads.
The caller contract is "holds a read share" — not "is a specific Fiber."
This enables patterns where a read share is transferred to a completion
callback on a different thread.

### Consequences of untracked reader identity

1. **Wrong-context unlock_read is NOT detectable** in v1. Any Fiber (or
   external thread) can call unlock_read and decrement the count.
2. **Double unlock_read** decrements twice; if active_readers_ reaches 0
   prematurely, a writer may be granted while readers still believe they
   hold shares. This is a caller bug, detectable only by debug assert
   (active_readers_ underflow check).
3. **This is an accepted v1 limitation.** The existing AsyncMutex also
   trusts the caller to unlock from the correct context (non-owner unlock
   is a debug assert, not a runtime check in Release).
4. **RAII guard** (future): a ReadGuard/WriteGuard wrapper can enforce
   correct release without changing the primitive's internal model.
5. **Cancellation after grant**: once resolve_(Woken) succeeds, the read
   share is owned. Cancellation cannot "un-own" it. The caller MUST
   eventually call unlock_read. If the Fiber's task is canceled after
   acquiring a read share, the task's cleanup path must release it.

---

## Test plan

### Suggested target name

```text
async_rwlock_test (in tests/async_rwlock_test.cpp)
```

### 1. Basic semantics

```text
multiple concurrent readers (N readers acquire, all hold simultaneously)
single writer exclusivity (writer blocks second writer)
writer blocks readers (readers queue behind active writer)
readers block writer (writer queues behind active readers)
last reader grants queued writer
writer unlock grants reader prefix batch
writer does not starve under continuous readers (admission control)
reader after queued writer cannot barge
reader batch excludes following writer
reader batch stops at first queued writer
try_read_lock fails when writer queued
try_read_lock fails when ANY waiter queued (including reader)
try_write_lock fails when readers active
try_write_lock fails when queue non-empty (no barging)
new_reader_queues_when_any_waiter_exists
```

### 2. Cancellation / deadline

```text
queued reader canceled (cancel returns true, node Cancelled)
queued writer canceled
head writer times out, following readers proceed (batch grant)
reader within head batch times out (excluded from batch; already unlinked)
cancel races with grant (exactly-once: one winner)
deadline races with last-reader unlock
writer cancel exposes following reader batch
stale callback after grant (timer retired; no double-resolve)
cancel wrong-RwLock returns false (queue-identity-safe)
cancel detached/terminal node returns false
queued_reader_not_bypassed_after_writer_cancel
queued_reader_not_bypassed_after_writer_expiry
timed_reader_batch_retires_each_timer_exactly_once
head_writer_cancel_grants_reader_prefix_immediately
head_writer_expiry_grants_reader_prefix_immediately
head_reader_cancel_grants_remaining_reader_prefix_immediately
head_reader_expiry_grants_remaining_reader_prefix_immediately
cancel_reconcile_publishes_each_winner_once
expiry_reconcile_retires_timers_once
```

### 3. Exactly-once / authority / fail-fast

```text
one terminal outcome per waiter (no double resolve)
one publication per winner (route called exactly once)
loser cannot unlink (cancel loser observes terminal, no mutation)
wrong queue identity (cancel on different RwLock returns false)
cross-Scheduler misuse (cancel on different Scheduler's RwLock returns false)
double unlock_read (debug assert fires)
unlock_write without writer (debug assert fires)
destroy with active reader (debug assert fires)
destroy with active writer (debug assert fires)
destroy with queued waiter (~WaitQueue assert fires)
invalid_mode_linked_node_fail_fast
terminal_linked_node_fail_fast
batch_publication_does_not_access_published_node
registration_reconcile_grants_only_head_prefix
```

### 4. Multi-worker

```text
reader acquisition on one worker, release on another (allowed)
writer handoff across workers (unlock on owner worker, winner resumes on thief)
concurrent reader unlocks racing to become last reader (exactly one triggers grant)
cancel and unlock on different workers (serialized by G)
deadline pump and unlock on different workers (serialized by G)
reader batch publication across workers (winners routed to respective owners)
```

Use deterministic seams (SLUICE_ASYNC_INTERNAL_TESTING), barriers, and
explicit state observation. No sleep_for as proof.

### 5. Fairness tests

**Test A: writer does not starve**

```text
R1 active (holds read lock)
W1 queues (write_lock suspends)
R2 arrives (read_lock MUST queue behind W1)
R1 releases (unlock_read)
→ W1 MUST acquire before R2
```

Deterministic: use phase seams to control ordering. Assert W1's node is
Woken before R2's node.

**Test B: reader phase batching**

```text
W1 active (holds write lock)
R1, R2, R3 queue (read_lock suspends × 3)
W2 queues (write_lock suspends)
W1 releases (unlock_write)
→ R1/R2/R3 form one reader phase (all three Woken in one batch)
→ W2 waits until ALL of R1/R2/R3 release
```

Assert: after W1 unlock, active_readers_ == 3, W2 still queued.
After R1+R2+R3 all unlock_read, W2 acquires.

**Test C: queued reader not bypassed after writer cancel**

```text
R1 active (holds read lock)
W1 queues (write_lock suspends)
R2 queues behind W1 (read_lock suspends)
W1 canceled (cancel returns true; W1 unlinked)
R3 arrives (read_lock)
→ R3 MUST queue (waiters_ non-empty: R2 is head)
R1 releases (unlock_read)
→ R2 MUST acquire before R3
```

This is the deterministic counterexample from the Fairness policy section.
Assert: R2's node is Woken before R3's node.

**Test D: queued reader not bypassed after writer expiry**

```text
Same as Test C, but W1 expires (deadline) instead of being canceled.
→ Same assertions.
```

### 6. Sanitizer and build gates

```text
Clang Debug full
GCC Debug full (if CI adds it)
Clang Release full (public header change)
TSan full (concurrency)
ASan+UBSan (ownership/layout)
multi-worker stability loop (scripts/verify-*.sh pattern)
```

---

## Formal model plan

### Recommendation: new TLA+ model required

AsyncRwLock introduces batch grant and writer-fair admission, which are not
modeled by existing E12-B/C/D/E specs.

### Model state

```tla
VARIABLES
    activeReaders,    \* Nat (0..MaxReaders)
    writerActive,     \* Bool
    writerOwner,      \* ProcSet union {NoOwner}
    queue,            \* Seq of [mode: {"R","W"}, proc: ProcSet]
    terminal,         \* [ProcSet -> {"pending","woken","cancelled","expired"}]
    published         \* [ProcSet -> Bool] (at most one publication)
```

### Safety properties

```text
InvMutualExclusion:   NOT (writerActive AND activeReaders > 0)
InvAtMostOneWriter:   writerActive => writerOwner != NoOwner
InvNoDoubleTerminal:  \A p \in ProcSet: terminal[p] changes at most once
InvWriterEligible:    granted writer was queue-head eligible
InvReaderPrefix:      granted readers belong to eligible head reader prefix
InvNoBarge:           no reader admitted (inline or batch) while ANY waiter
                      is queued ahead — prohibits BOTH reader-past-writer
                      AND new-reader-past-queued-reader
InvNoUnderflow:       activeReaders >= 0
InvExactlyOncePub:    \A p: published[p] set at most once
InvLinkedNotTerminal: \A n in queue: n is registered (not terminal)
```

### Liveness (under fairness assumptions)

```text
LivenessWriterAcquires:
    WF(GrantWriter): queued head writer eventually acquires when owners release
LivenessReaderPrefix:
    WF(GrantReaders): head reader prefix eventually acquires
LivenessCanceledHeadDoesNotBlock:
    canceled/timed-out head does not permanently block queue progress
```

### Broken-model variant (negative check)

Demonstrate the checker CAN find violations:

```text
BrokenBargingWriter:   allow reader to bypass queued writer → InvNoBarge fails
BrokenBargingReader:   allow new reader to bypass queued reader → InvNoBarge fails
BrokenStarvation:      reader-preference admission → LivenessWriterAcquires fails
BrokenDoubleTerminal:  allow two resolves → InvNoDoubleTerminal fails
BrokenSimultaneous:    skip mutual exclusion check → InvMutualExclusion fails
BrokenLinkedTerminal:  leave terminal node linked → InvLinkedNotTerminal fails
```

### Verification script

```text
scripts/verify-e12-rwlock-formal.sh
```

Pattern: safety-only TLC run + broken-model counterexample check (mirrors
existing verify-e12-*-formal.sh scripts).

---

## Compatibility impact

### Public headers

New file: `include/sluice/async/async_rwlock.hpp` (installed header).
No existing public API behavior or object layout changes.
`WaitNode::user_` documentation in the existing public header is updated
(comment-only; no semantic or layout change).
A new installed `async_rwlock.hpp` header is added.

### Build targets

- `sluice_async`: add `src/async/async_rwlock.cpp` (or inline in header if
  the implementation is thin-forwarding like AsyncMutex).
- `sluice_async_internal_testing`: same source + test seams.
- No change to `sluice_core`.

### Scheduler seams

New private Scheduler methods (mirroring existing pattern):

```text
rwlock_try_read_lock(WaitQueue&, std::size_t& active_readers, bool& writer_active)
rwlock_read_lock(WaitQueue&, std::size_t& active_readers, bool& writer_active, WaitNode&)
rwlock_read_lock_until(WaitQueue&, ..., deadline_t)
rwlock_try_write_lock(WaitQueue&, std::size_t& active_readers, bool& writer_active, Fiber*& writer_owner)
rwlock_write_lock(WaitQueue&, ..., WaitNode&)
rwlock_write_lock_until(WaitQueue&, ..., deadline_t)
rwlock_cancel(WaitQueue&, std::size_t& active_readers, bool& writer_active, Fiber*& writer_owner, WaitNode&)
rwlock_expire_wait(WaitQueue&, std::size_t& active_readers, bool& writer_active, Fiber*& writer_owner, WaitNode&)
rwlock_unlock_read(WaitQueue&, std::size_t& active_readers, bool& writer_active, Fiber*& writer_owner)
rwlock_unlock_write(WaitQueue&, std::size_t& active_readers, bool& writer_active, Fiber*& writer_owner)
```

Note: `rwlock_cancel` and `rwlock_expire_wait` take FULL RwLock state
(not just WaitQueue&) because they perform head reconcile after unlink.

The unified grant helper (called from unlock seams AND admission recheck):

```text
rwlock_grant_from_head_locked(WaitQueue&, std::size_t& active_readers, bool& writer_active, Fiber*& writer_owner)
```

This dispatches to:

```text
rwlock_grant_readers_locked(WaitQueue&, std::size_t& active_readers)
rwlock_grant_one_writer_locked(WaitQueue&, bool& writer_active, Fiber*& writer_owner)
```

The canonical no-publish claim helper (shared by reader batch + writer grant):

```text
claim_waiter_woken_no_publish_locked(WaitNode&) -> WaitNode*
```

This is a general Scheduler-private utility (not RwLock-specific) that
performs resolve + unlink + timer retirement + accounting WITHOUT publication.
It is reusable by any future primitive needing deferred publication.

The RwLock-specific timer expiry hook (for pump_deadlines_locked routing):

```text
rwlock_timer_expire_reconcile(void* owner_ctx, WaitNode& node)
```

This is installed on TimerRegistration at timed-wait registration. It casts
`owner_ctx` to `RwLockExpireCtx*` and calls `rwlock_expire_wait` with full
state. Follows the E12-E Queue `on_resolve_` + `owner_ctx_` precedent.

### WaitQueue changes

**None required.** The existing WaitQueue supports:

- FIFO registration (register_wait_locked) ✓
- Head observation (head_ accessible under mtx_) ✓
- Traversal under mtx_ (contains_locked demonstrates the pattern) ✓
- Per-node resolve + unlink (wake_node_locked, cancel_locked, expire_locked) ✓
- Loop resolution (event_set_broadcast drain pattern) ✓

The batch grant traverses the queue under mtx_ (same as contains_locked's
scan pattern) and calls resolve_ + unlink_locked per node. No new WaitQueue
method is needed; the Scheduler (as sole friend) can traverse head_ → next_
under mtx_.

### Mode storage

Uses existing `WaitNode::user_` (E12-E Queue precedent). No WaitNode
structural modification required. The `user_` comment will be updated during
implementation to reflect its role as a controlled primitive-per-operation
context hook (authorized users: Queue, RwLock). See RwWaitCtx ownership
section for the full specification.

---

## Implementation phases

### Phase 1: Core RwLock (no deadline)

1. Add `include/sluice/async/async_rwlock.hpp` (public header).
2. Add canonical helper: `claim_waiter_woken_no_publish_locked` in Scheduler.
3. Add Scheduler seams: rwlock_try_read_lock, rwlock_read_lock,
   rwlock_try_write_lock, rwlock_write_lock, rwlock_cancel,
   rwlock_unlock_read, rwlock_unlock_write.
4. Add unified grant helper: rwlock_grant_from_head_locked,
   rwlock_grant_readers_locked, rwlock_grant_one_writer_locked.
5. Basic tests (no deadline): acquisition, exclusivity, batching, fairness,
   reader-bypass-reader prevention, fail-fast invariants.

### Phase 2: Deadline integration

5. Add rwlock_read_lock_until, rwlock_write_lock_until (E11 timer pattern).
6. Deadline tests: timeout races, already-due admission, timer retirement.

### Phase 3: Multi-worker + stability

7. Multi-worker tests with deterministic seams.
8. TSan full suite.
9. Stability loop script.

### Phase 4: Formal model

10. TLA+ spec + safety invariants + liveness (under fairness).
11. Broken-model negative check.
12. Verification script.

---

## Open questions

1. **Reader fast-path is now O(1)**: the corrected admission rule
   ("waiters_ is empty") requires only a head_ == nullptr check, not a
   queue scan. The previous "any queued writer" scan is eliminated.
   No optimization counter is needed. **CLOSED.**

2. **External-thread unlock_read**: AsyncMutex requires a running Fiber for
   unlock (owner check uses g_worker->current). For RwLock, unlock_read does
   NOT require Fiber identity (v1 accepts any caller). Should unlock_read be
   callable from an external OS thread? **Decision**: YES, consistent with
   Semaphore::release() being callable from external threads. The caller
   contract is "holds a read share" — not "is a specific Fiber."
   See Calling context table. **CLOSED.**

3. **Recursive read lock detection**: v1 does NOT detect it (no per-fiber
   reader tracking). A Fiber that calls read_lock while holding a read share
   will either acquire again (if waiters_ empty) or deadlock (if any waiter
   is queued between the two calls). This is documented as a caller contract
   violation. **No runtime check in v1.** **ACCEPTED.**

4. **ClaimRecord storage for large batches**: the intrusive publication list
   reuses unlinked WaitNode next_/prev_ pointers. No fixed-size array limit.
   No dynamic allocation. **CLOSED** (see Intrusive publication list section).

5. **try_write_lock from external thread**: FORBIDDEN. writer_owner_ requires
   a valid Fiber*. Recursive call by current owner returns false (matching
   AsyncMutex try_lock). See Calling context table. **CLOSED.**

6. **Cancel/expiry head reconcile**: cancel and expiry MUST call
   `rwlock_grant_from_head_locked` after unlink in the same G + W hold.
   Timer routing uses RwLock-specific `on_expire_reconcile_` hook on
   TimerRegistration (Queue `on_resolve_` precedent). **CLOSED.**

7. **ClaimRecord removed**: publication storage uses intrusive WaitNode list
   exclusively. No separate ClaimRecord struct. **CLOSED.**

---

## Implementation authorization

```text
AUTHORIZED FOR IMPLEMENTATION
```

### Authorization checklist

| Criterion | Status |
| --- | --- |
| API determined | ✓ (explicit lock/unlock, WaitNode pattern) |
| Fairness determined | ✓ (phase-fair FIFO prefix batching; waiters_-empty fast path) |
| Reader admission corrected | ✓ (no reader bypasses ANY queued waiter; counterexample documented) |
| Admission recheck unified | ✓ (grant_from_head_locked; no per-node "writer ahead" check) |
| Cancel head reconcile defined | ✓ (rwlock_cancel takes full state; calls grant_from_head_locked after unlink) |
| Deadline expiry reconcile defined | ✓ (rwlock_expire_wait + TimerRegistration on_expire_reconcile_ hook) |
| RwWaitCtx ownership determined | ✓ (internal stack-local; caller does NOT construct; lifecycle proved) |
| user_ documentation updated | ✓ (both AsyncQueue and AsyncRwLock named; header reflects both primitives) |
| Fail-fast on invalid linked nodes | ✓ (Category B: debug assert + Release abort; E10 Unlink Law consequence) |
| Race matrix corrected | ✓ (G + W serialization; no per-node CAS failure in batch) |
| Canonical no-publish helper designed | ✓ (claim_waiter_woken_no_publish_locked; no alloc; no throw; returns WaitNode*) |
| Publication storage unified | ✓ (intrusive WaitNode list only; no ClaimRecord; fiber_owner_ for routing) |
| Publication list lifecycle corrected | ✓ (cached next/fiber/owner before publish; no post-publication dereference) |
| Calling context table determined | ✓ (try_write_lock: any Fiber allowed, recursive returns false, external forbidden) |
| Destruction/Release contract determined | ✓ (Category A: debug assert; Release trusts caller; same as existing primitives) |
| Two violation categories defined | ✓ (Category A caller misuse vs Category B internal corruption; user_ entry is A) |
| Batch grant determined | ✓ (Method B: claim-all-then-publish, intrusive local list) |
| Lock ordering determined | ✓ (G → W, no new lock levels) |
| Cancellation/deadline matrix determined | ✓ (resource-first, G + W serialization, head reconcile) |
| WaitQueue capability sufficient | ✓ (no new WaitQueue methods needed) |
| Test plan captures incorrect implementations | ✓ (fairness + fail-fast + lifecycle + reconcile tests) |
| TLA+ InvNoBarge covers reader-past-reader | ✓ (prohibits both reader-past-writer AND reader-past-reader) |
| No blocking semantic questions | ✓ (all open questions CLOSED or ACCEPTED) |

### Infrastructure changes required

- New Scheduler private seams (rwlock_* methods in scheduler.hpp/scheduler.cpp).
- New canonical helper (claim_waiter_woken_no_publish_locked) in Scheduler.
- New RwLock-specific timer expiry hook (rwlock_timer_expire_reconcile) +
  RwLockExpireCtx stable context.
- TimerRegistration: add on_expire_reconcile_ function pointer (or reuse
  existing on_resolve_ slot with extended semantics; implementation detail).
- New public header (include/sluice/async/async_rwlock.hpp).
- New source file (src/async/async_rwlock.cpp) if not header-only.
- New test file (tests/async_rwlock_test.cpp).
- xmake.lua: add source to sluice_async + sluice_async_internal_testing targets.
- New formal model (spec/tla/AsyncRwLock.tla or docs/spec/e12_rwlock/).
- New verification script (scripts/verify-e12-rwlock-formal.sh).
- WaitNode::user_ comment update (implementation phase; design documented here).

---

## Repository evidence reviewed

```text
include/sluice/async/wait_node.hpp        — E10 WaitNode lifecycle, resolve_ CAS
include/sluice/async/wait_queue.hpp       — E10 WaitQueue protocol, sealed authority
include/sluice/async/scheduler.hpp        — E7-E13 Scheduler seams, lock order
include/sluice/async/async_mutex.hpp      — E12-C ownership model, handoff pattern
include/sluice/async/semaphore.hpp        — E12-B permit model, no-barging
include/sluice/async/event.hpp            — E12-A broadcast drain pattern
include/sluice/async/condition.hpp        — E12-D two-epoch protocol
include/sluice/async/async_queue.hpp      — E12-E QueuePort, user_ pointer
include/sluice/async/select.hpp           — E13 Select (Event/Timer arms only)
include/sluice/async/group.hpp            — Group (not relevant to RwLock)
include/sluice/async/lock_guard.hpp       — LockGuard (TSA annotation, not RwLock guard)
docs/history/implementation-plans/e12-sync-primitives-plan.md §9       — RwLock semantic authority (Task H)
docs/history/closeout/e12-async-mutex.md                   — M-H1..M-H4 policy register
docs/history/closeout/e12-semaphore.md                     — A1..A5 policy register
docs/api-reference.md                     — Result<T>/IoError model
Tokio sync::RwLock (context7)             — writer-fair FIFO policy reference
```
