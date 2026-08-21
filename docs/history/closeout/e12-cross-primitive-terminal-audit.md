# E12-G — Cross-Primitive Cancellation / Deadline Terminal-Resolution Audit

> **DESIGN / AS-BUILT AUDIT ONLY.** This document is the E12-G normative
> cross-primitive audit. It does NOT modify production implementation, tests,
> the public API, the Scheduler, or any formal model. No commit, push, or PR is
> authorized by this document. Its only authorized deliverables are (a) this
> file and (b) a minimal status/link update to
> [`docs/history/implementation-plans/e12-sync-primitives-plan.md`](docs/history/implementation-plans/e12-sync-primitives-plan.md) §10.
>
> **Authority chain (per `AGENTS.md` §2):**
> ```text
> 1. this E12-G task scope
> 2. accepted as-built per-primitive E12 docs (e12-event / e12-semaphore /
>    e12-async-mutex / e12-condition / e12-queue* / e12-rwlock) + e10/e11
> 3. public headers under include/sluice/async/ + docs/api-reference.md
> 4. production implementation under src/async/ (scheduler.cpp, queue_port.cpp,
>    queue_detail.hpp) + include/sluice/async/ headers
> 5. tests/ + scripts/verify-e12-*-formal.sh
> 6. xmake.lua target membership
> 7. .github/workflows/ci.yml (Clang Debug gate)
> ```
>
> `docs/history/implementation-plans/e12-sync-primitives-plan.md` §10 is the **preparation baseline** only.
> It contains OBSOLETE preparation-era candidates (Condition Model A/B,
> RwLock "1–2 reader/writer queues", RwLock "read-permit pre-increment/refund",
> RwLock "upgrade state", "Queue external cancellation candidate design"). The
> per-primitive as-built docs and code are the authority; the preparation
> matrix is corrected here, NOT re-imposed on closed primitives.

---

## 1. Status

```text
E12-G-DESIGN: COMPLETE
E12-G-AS-BUILT-AUDIT: PASS
E12-G-PRODUCTION-CORRECTIVE: NOT REQUIRED
E12-G-TEST-CORRECTIVE: NOT REQUIRED  (optional non-blocking parity additions identified)
E12-G-FORMAL-CORRECTIVE: NOT REQUIRED (F1 chosen; RwLock negative-model parity gap is non-blocking)
E12-G-CLOSEOUT: READY FOR INDEPENDENT RE-REVIEW
```

**Verdict in one line:** every E12 primitive satisfies the G-TERM laws as
built. No production defect, no public-API conflict, no Scheduler authority
corruption was found. Two DOC-DRIFT findings (F-G-1 RwLock preparation matrix drift, F-G-2 Condition admission precedence drift)
and several non-blocking parity / formal-coverage observations are recorded.
No corrective production implementation is authorized or required by this
audit.

---

## 2. Scope and non-goals

### 2.1 In scope

```text
E12-A Event
E12-B Semaphore
E12-C AsyncMutex
E12-D AsyncCondition
E12-E AsyncQueue
E12-F AsyncRwLock

E10 WaitNode / WaitQueue          (the terminal-winner authority substrate)
E11 TimerRegistration / deadline pump  (the third resolver + timer lifetime)
Scheduler terminal accounting / publication substrate
```

### 2.2 Out of scope (NOT in E12-G)

```text
a new CancelToken model
Queue v1 external cancellation            (DEFERRED-BY-DESIGN; not a parity failure)
E13 Select integration                    (shared-substrate regression boundary only)
a new generic sync base class / Grant framework
a unified Awaitable / Lockable interface
coroutine API
RAII guard
RwLock upgrade / downgrade
wait morph
priority inheritance
lock-free optimization
```

The hard rule from the task: E12-G MUST NOT become a pretext to create
`SynchronizationPrimitiveBase`, `GenericGrantFramework`,
`GenericCancellationCallback`, `UniversalWaitContext`, or a unified public
cancel token. This audit proposes a shared private helper ONLY if evidence
proves a real defect shared by multiple primitives that a narrow fix cannot
close. **No such defect was found**, so no shared helper is proposed.

### 2.3 E13 Select boundary

E13 Select is OUT of the primitive parity matrix. It is treated here only as a
**shared-substrate regression boundary**: the `waiting_select_count_` counter,
the unified deadline heap (Ordinary | Select tagged entries), and the Scheduler
publication seam are shared with E12. Changes implied by this audit (none)
MUST NOT regress E13. The audit does NOT re-verify Select semantics.

---

## 3. Authority and as-built source map

The load-bearing claim of this audit is that every G-TERM law is backed by an
**as-built code site** (file:line), not by preparation prose. The source map
below is the evidence spine.

### 3.1 Generic terminal-resolution substrate (E10 + E11 + Scheduler)

| Concern | File:line / symbol | Authority / invariant | Lock order |
| --- | --- | --- | --- |
| Single terminal-winner CAS | `include/sluice/async/wait_node.hpp:241` `WaitNode::resolve_` | `state_` `registered -> {woken,cancelled,expired}` CAS, acq_rel/acquire. The unique winner authority. | lock-free; CAS is the winner point |
| Structural unlink (one path) | `include/sluice/async/wait_queue.hpp:302` `WaitQueue::unlink_locked` | private; Scheduler-only friend; called only by a winning resolver in the SAME critical section as its CAS | caller holds `mtx_` |
| Wake FIFO head (Woken) | `wait_queue.hpp:199` `wake_one_locked` | private; Scheduler-only | caller holds `mtx_` |
| Cancel specific node (Cancelled) | `wait_queue.hpp:222` `cancel_locked` | private; Scheduler-only | caller holds `mtx_` |
| Wake specific node (Woken, admission) | `wait_queue.hpp:245` `wake_node_locked` | private; Scheduler-only; E12-A admission closure | caller holds `mtx_` |
| Expire specific node (Expired) | `wait_queue.hpp:266` `expire_locked` | private; Scheduler-only; E11 third resolver | caller holds `mtx_` |
| Membership gate (queue-identity cancel) | `wait_queue.hpp:289` `contains_locked` | private; Scheduler-only; scans THIS queue only, never reads foreign `home_` | caller holds `mtx_` |
| Generic wake entry | `scheduler.cpp:1168` `Scheduler::wake_wait_one` | takes G → q.mtx() | G → q.mtx() |
| Generic cancel entry | `scheduler.cpp:1182` `Scheduler::cancel_wait` | takes G → q.mtx() | G → q.mtx() |
| Generic expire entry | `scheduler.cpp:1334` `Scheduler::expire_wait` | takes G → q.mtx() | G → q.mtx() |
| Generic timed wait | `scheduler.cpp:1251` `Scheduler::await_wait_deadline` | G → q.mtx(); creates `TimerRegistration` in `timer_pool_`; I5 already-due inline Expired | G → q.mtx() |
| Deadline pump | `scheduler.cpp:3792` `Scheduler::pump_deadlines_locked` | caller holds G; I4 state-before-node gate via `try_claim_expiry` BEFORE dereferencing node/queue; RwLock branch routes to `rwlock_expire_wait`; generic/Queue branch inlines `expire_locked` (avoids G self-deadlock) | G held |
| Timer retire (non-timer winner) | `scheduler.cpp:3911` `Scheduler::retire_timer_for_node_locked` | ACTIVE→RETIRED in the SAME G CS as the resolve CAS; I4 closure | caller holds G |
| Timer expiry claim (timer winner) | `include/sluice/async/timer_registration.hpp:112` `TimerRegistration::try_claim_expiry` | ACTIVE→CONSUMED CAS; the I4 winner gate; called BEFORE any node dereference | atomic |
| Runnable publication guard | `Fiber::make_runnable` (Fiber method) | exactly-once ticket guard; returns false if already Runnable | n/a |
| Runnable routing | `scheduler.cpp:968` `Scheduler::route_runnable_locked` | pushes to owner inbox under `inbox_mtx_`; `signal_wake_locked` | caller holds G; acquires wake_mtx_ |
| Wake signal | `scheduler.cpp:290` `Scheduler::signal_wake_locked` | ++wake_epoch_; `wake_cv_.notify_all()` | acquires wake_mtx_ (safe under G) |
| Wait-queue wait count | `scheduler.hpp:1116` `waiting_waitq_count_` | ++ on register, -- on every resolution; guarded by `if (>0)` defense-in-depth | G |
| Active deadline count | `scheduler.hpp:1267` `active_deadline_count_` | ++ on register, -- on ACTIVE→{RETIRED,CONSUMED} exactly once | G |
| External-wake classification | `scheduler.hpp:1698` `external_wake_possible_locked` | `!waiting_ready_.empty() || waiting_waitq_count_ > 0 || any_active_deadline_locked() || waiting_select_count_ > 0` | G |
| Timer pool | `scheduler.hpp:1244` `timer_pool_` (std::list) | pointer-stable; lazy-at-deadline erasure | G |
| Deadline heap | `scheduler.hpp:1252` `deadline_heap_` | unified Ordinary\|Select tagged min-heap (E13 P3) | G |

### 3.2 Per-primitive source map

Each row points at the file / function / state committed before publication /
lock order. The "state committed before publication" column is the load-bearing
G-TERM-3 evidence.

| Primitive | Entry | Authority function | Locks | State committed before publication |
| --- | --- | --- | --- | --- |
| **Event** | `Event::set` | `Scheduler::event_set_broadcast` (`scheduler.cpp:1361`) — loop `wake_wait_one_locked` until drained under one continuous G hold | G → q.mtx() | per-waiter `resolve_(Woken)` (no resource commit; Event has no consumable resource — G-TERM-3 N/A for Event) |
| **Event** | `Event::wait` admission | `Scheduler::await_event_wait` (`scheduler.cpp:1741`) | G → q.mtx() | register + `++waiting_waitq_count_`; if SET observed → `wake_node_locked` resolves THIS node Woken inline (no suspension) |
| **Event** | `Event::wait_until` admission | `Scheduler::await_event_wait_deadline` (`scheduler.cpp:1818`) | G → q.mtx() | SET precedence over already-due deadline → Woken inline |
| **Event** | `Event::cancel` | `Scheduler::event_cancel_wait` (`scheduler.cpp:1697`) | G → q.mtx() | membership gate (`contains_locked`) BEFORE resolve CAS |
| **Semaphore** | `Semaphore::try_acquire` | `Scheduler::sem_try_acquire` (`scheduler.cpp:1906`) | G → q.mtx() | succeed iff `empty_locked() && available>0`; no barging |
| **Semaphore** | `Semaphore::acquire` admission | `Scheduler::sem_acquire` (`scheduler.cpp:1932`) | G → q.mtx() | register; FIFO-head (`prev_==nullptr`) + `available>0` → resolve Woken inline + `available--`; else suspend |
| **Semaphore** | `Semaphore::acquire_until` admission | `Scheduler::sem_acquire_until` (`scheduler.cpp:2012`) | G → q.mtx() | permit-first over already-due deadline → Woken inline (A4) |
| **Semaphore** | `Semaphore::release` | `Scheduler::sem_release` (`scheduler.cpp:2129`) | G → q.mtx() (inside `wake_wait_one_locked`) | pending-permit transfer to FIFO head (available UNCHANGED) OR store++ OR overflow-false; **no pre-decrement, no refund** |
| **Semaphore** | `Semaphore::cancel` | `Scheduler::sem_cancel` (`scheduler.cpp:2100`) | G → q.mtx() | membership gate; serialized before release observes queue (Conclusion A) |
| **AsyncMutex** | `AsyncMutex::lock` admission | `Scheduler::mutex_lock` (`scheduler.cpp:2211`) | G → q.mtx() | register; FIFO head + owner==nullptr → `wake_node_locked` + `owner = me`; else suspend |
| **AsyncMutex** | `AsyncMutex::lock_until` admission | `Scheduler::mutex_lock_until` (`scheduler.cpp:2294`) | G → q.mtx() | resource-first over already-due deadline → Woken + `owner = me` |
| **AsyncMutex** | `AsyncMutex::unlock` handoff | `Scheduler::mutex_handoff_one_locked` (`scheduler.cpp:2411`) | caller holds G; takes q.mtx() internally | `wake_one_locked` → `owner = winner Fiber` (`scheduler.cpp:2434`) BEFORE `make_runnable`+`route_runnable_locked`. **MUTEX-HANDOFF-ONE seam.** |
| **AsyncMutex** | `AsyncMutex::cancel` | `Scheduler::mutex_cancel` (`scheduler.cpp:2381`) | G → q.mtx() | membership gate; cancel of queued waiter just unlinks (no owner change) |
| **AsyncCondition** | `AsyncCondition::wait` prepare | `Scheduler::condition_wait_prepare` (`scheduler.cpp:3569`) | G; cond_waiters.mtx() then released; mutex_waiters.mtx() via handoff (NEVER simultaneous) | CONDITION-WAIT-PREPARE: register cond_node under cond.mtx() → `mutex_handoff_one_locked(mutex_waiters)` → `make_waiting` → release G → switch |
| **AsyncCondition** | `AsyncCondition::wait_until` prepare | `Scheduler::condition_wait_prepare_until` (`scheduler.cpp:3650`) | G; cond.mtx() then mutex.mtx() (sequential) | **deadline-FIRST** (C-H4): already-due deadline at admission → Expired inline, Mutex NOT released, no reacquire epoch. Else register+timer+handoff |
| **AsyncCondition** | `AsyncCondition::notify_one` | `Scheduler::condition_notify_one` (`scheduler.cpp:3734`) | G → cond.mtx() | `wake_wait_one_locked`; no Mutex mutation |
| **AsyncCondition** | `AsyncCondition::notify_all` | `Scheduler::condition_notify_all` (`scheduler.cpp:3745`) | G → cond.mtx() (one continuous hold) | drain `wake_wait_one_locked` to nullptr (atomic snapshot); no Mutex mutation |
| **AsyncCondition** | `AsyncCondition::cancel` | `Scheduler::condition_cancel_wait` (`scheduler.cpp:3769`) | G → cond.mtx() | membership gate; cancel affects only Condition epoch (reacquire is non-cancellable) |
| **AsyncQueue** | `AsyncQueue<T>::push` admit | `Scheduler::queue_push_admit` (`scheduler.cpp:3180`) | G → S (state_mtx_) → producer.mtx() | register; `++active_wait_associations_`, `++waiting_waitq_count_`; Open+space+FIFO head → commit lease into ring inline (P2); Closed → resolve Woken with lease retained (P3); else suspend |
| **AsyncQueue** | `AsyncQueue<T>::pop` admit | `Scheduler::queue_pop_admit` (`scheduler.cpp:3243`) | G → S → consumer.mtx() | symmetric on consumer role |
| **AsyncQueue** | `queue_grant_consumer_locked` | `Scheduler::queue_grant_consumer_locked` (`scheduler.cpp:3474`) | caller holds G+S; takes consumer.mtx() | resolve head → retire timer (F.6 retire-before-commit) → move ring HEAD into winner's `cons_out` → `--active_wait_associations_`, `--waiting_waitq_count_`, `++granted_not_resumed_` → publish |
| **AsyncQueue** | `queue_grant_producer_locked` | `Scheduler::queue_grant_producer_locked` (`scheduler.cpp:3505`) | caller holds G+S; takes producer.mtx() | resolve head → retire → move winner's `prod_lease` into freed ring slot (or retain if Closed) → accounting → publish |
| **AsyncQueue** | queue timer hook | `Scheduler::queue_timer_on_resolve` (`scheduler.cpp:3167`) | G | `static_cast<QueuePort*>(owner_ctx)`; `--active_queue_timers_` if >0; invoked exactly once per ACTIVE→terminal |
| **AsyncQueue** | timer expiry in pump | `pump_deadlines_locked` inline path (`scheduler.cpp:3879–3901`) | G → q.mtx() | expire_locked → if has_on_resolve: `--port->active_wait_associations_`, fire on_resolve (`--active_queue_timers_`) → `--waiting_waitq_count_` → make_runnable+route |
| **AsyncQueue** | close drain | `QueuePort::close` (`queue_port.cpp:297`; drain loops at 321 and 325) | G+S held | `while (queue_grant_consumer_locked)` + `while (queue_grant_producer_locked)`; close-vs-grant serialized by G+S |
| **AsyncQueue** | teardown | `QueuePort::begin_teardown` (`queue_port.cpp:464`) | G+S | irreversible operational→tearing_down; preconditions: zero active_port_calls_/wait_associations/queue_timers/granted_not_resumed, both role queues empty |
| **AsyncRwLock** | `AsyncRwLock::read_lock` admission | `Scheduler::rwlock_read_lock` (`scheduler.cpp:2682`) | G → W (waiters_.mtx()) | install `RwWaitCtx{read}` on node.user(); admission via `rwlock_claim_node_woken_locked` (NO `grant_from_head` — see REGISTRATION-ADMISSION-DRIFT note) |
| **AsyncRwLock** | `AsyncRwLock::write_lock` admission | `Scheduler::rwlock_write_lock` (`scheduler.cpp:2776`) | G → W | install `RwWaitCtx{write}`; admission claim + commit `writer_active=true, writer_owner=me` |
| **AsyncRwLock** | `AsyncRwLock::unlock_read` reconcile | `Scheduler::rwlock_unlock_read` (`scheduler.cpp:2831`) | G | `--active_readers`; if reaches 0 → `rwlock_grant_from_head_locked` |
| **AsyncRwLock** | `AsyncRwLock::unlock_write` reconcile | `Scheduler::rwlock_unlock_write` (`scheduler.cpp:2845`) | G | clear writer state → `rwlock_grant_from_head_locked` |
| **AsyncRwLock** | cancel reconcile | `Scheduler::rwlock_cancel` (`scheduler.cpp:2865`) | G → W | membership gate → cancel_locked → retire timer → `--active_deadline_count_`, `--waiting_waitq_count_` → **`rwlock_grant_from_head_locked`** (head reconcile) → publish cancel winner AFTER grants |
| **AsyncRwLock** | expire reconcile | `Scheduler::rwlock_expire_wait` (`scheduler.cpp:2898`) | caller holds G (pump); acquires W internally | resolve Expired → unlink → timer CONSUMED → `--active_deadline_count_`, `--waiting_waitq_count_` → **`rwlock_grant_from_head_locked`** → publish expired waiter AFTER grants |
| **AsyncRwLock** | prefix batch grant | `Scheduler::rwlock_grant_from_head_locked` (`scheduler.cpp:2536`) | caller holds G; acquires W internally; releases W before publication | dispatch on head.mode: writer → `rwlock_grant_one_writer_locked`; reader → `rwlock_grant_readers_locked` (maximal consecutive reader prefix). **Batch publication caches next/fiber/owner BEFORE route; does NOT dereference resumed nodes** |
| **AsyncRwLock** | timer reconcile routing | `pump_deadlines_locked` RwLock branch (`scheduler.cpp:3854–3872`) | G (pump); `rwlock_expire_wait` acquires W | identifies RwLock timers by `top->on_resolve_ == &rwlock_timer_expire_reconcile`; null ExpireCtx = Category B fail-fast (`assert+abort`) |

---

## 4. As-built primitive inventory

This section records the **as-built** shape of each primitive and explicitly
marks any field that contradicts the preparation matrix as OBSOLETE.

### 4.1 Event (E12-A)

```text
state            : std::atomic<bool> set_  (lock-free acquire/release)
queues           : 1 (waiters_; intrusive FIFO; private structural ops)
per-op context   : none (Event has no per-waiter resource)
ownership ident. : none
close/reset state: set/reset only; reset does NOT cancel
destruction      : caller-contract; ~WaitQueue asserts empty in debug; no cancel/wake
timer registration: per wait_until via E11 TimerRegistration
terminal causes  : Woken (set broadcast / admission SET) | Cancelled | Expired
admission precedence: SET checked BEFORE already-due deadline (resource-first)
```

### 4.2 Semaphore (E12-B)

```text
state            : std::atomic<permit_count_t> available_ (observational load;
                   authoritative under G); const permit_count_t max_permits_
queues           : 1 (waiters_; FIFO)
per-op context   : none
ownership ident. : none (permits are anonymous)
destruction      : caller-contract; debug assert empty; no cancel-all/wake-all
timer registration: per acquire_until
terminal causes  : Woken (release transfer / immediate acquire) | Cancelled | Expired
release disposition: transfer (available UNCHANGED) | store++ | overflow-false; NO pre-decrement, NO refund
admission precedence: permit admissible BEFORE already-due deadline (resource-first, A4)
```

### 4.3 AsyncMutex (E12-C)

```text
state            : Fiber* owner_  (SOLE ownership authority; nullptr == NoOwner)
                   (NO redundant bool locked_)
queues           : 1 (waiters_; FIFO; queue contains only Suspended epochs)
per-op context   : none
ownership ident. : Fiber* identity (survives E8 work stealing)
destruction      : caller-contract; ~AsyncMutex asserts owner_==nullptr AND queue empty
handoff seam     : mutex_handoff_one_locked (owner = winner BEFORE publication)
terminal causes  : Woken (handoff / admission acquire) | Cancelled | Expired
                   immediate-path outcomes resolve terminal but do NOT publish runnable
admission precedence: free + FIFO-head BEFORE already-due deadline (resource-first)
```

### 4.4 AsyncCondition (E12-D)

```text
state            : AsyncMutex& mutex_; Scheduler& scheduler_; WaitQueue waiters_;
                   (+ an IMPLEMENTATION-BOUNDARY active_waits_ debug/lifetime counter)
queues           : 1 own (waiters_) + the bound Mutex's waiters_ (two epochs, sequential)
per-op context   : caller-provided WaitNode for the Condition epoch;
                   STACK-LOCAL reacquire WaitNode for the Mutex reacquire epoch
ownership ident. : via the bound Mutex (Fiber*)
destruction      : caller-contract; asserts active_waits_==0; does NOT notify/cancel/force
model            : **Model A (mandatory reacquisition) — CLOSED**
                   reacquire is untimed (C-H4) and non-cancellable (C-H5)
                   the Condition outcome (Woken/Expired/Cancelled) is latched and returned
                   the original Condition cancel/expiry does NOT re-win reacquire
terminal causes  : Woken (notify_one/all) | Cancelled (Condition epoch only) | Expired (Condition epoch only)
                   then mandatory Mutex reacquire (a separate ordinary-tail Mutex admission)
admission precedence: **deadline-FIRST for the Condition epoch** (C-H4 —
                   already-due deadline at admission -> Expired inline, Mutex NOT released,
                   NO reacquire epoch). This is the documented divergence from the
                   resource-first rule used by Event/Semaphore/Mutex/Queue/RwLock.
notify_all mechanism: loop wake_wait_one_locked under one continuous G hold (atomic snapshot)
```

### 4.5 AsyncQueue (E12-E)

```text
state            : std::unique_ptr<QueueItemLease[]> ring_; QueueState (Open|Closed);
                   QueueLifecycle (operational|tearing_down); capacity >= 1
queues           : 2 (producer role + consumer role); NEVER held together
per-op context   : QueueWaitCtx (port, role, prod_control/prod_lease OR cons_out)
                   stashed on node.user() before registration
ownership ident. : operation epoch (the suspended push/pop); no persistent owner
destruction      : quiet-state caller-contract for ~AsyncQueue<T>();
                   IRREVERSIBLE begin_teardown -> QueueTeardownSession (drains ring)
result type      : QueuePushResult<T> / QueuePopResult<T> with status()
                   committed | closed | expired | would_block (+ item for pop)
                   WaitOutcome is the resolution cause; QueueCompletion is the
                   operation completion (None|Pending|Committed|Closed|Expired)
terminal causes  : Woken => Completion in {Committed, Closed};
                   Expired => Completion = Expired;
                   WouldBlock => inline try_* only (NEVER a resolution)
external cancel  : **DEFERRED in Queue v1** (P8/C7 + PUB-P-*-CANCEL reserved but absent)
                   — NOT a parity failure (see §11 G-TERM-7)
timer registration: per push_until/pop_until; on_resolve_=&queue_timer_on_resolve;
                   owner_ctx_=&port; ++active_queue_timers_
admission precedence: resource (free slot / available item) BEFORE already-due deadline (resource-first)
structural lock  : synchronous Mutex state_mtx_ (NOT E12-C AsyncMutex); no-throw/fail-fast
                   (ASYNC-MUTEX-NOTHROW-AUTHORITY-1, B1 PASS)
```

### 4.6 AsyncRwLock (E12-F)

```text
state            : std::size_t active_readers_; bool writer_active_; Fiber* writer_owner_;
                   WaitQueue waiters_  (**ONE unified FIFO queue** — readers + writers)
queues           : 1 (unified) — NOT "1-2 reader/writer queues" (preparation obsolete)
per-op context   : RwWaitCtx { Mode mode } stashed on node.user() (controlled hook)
                   stack-local; set before registration, cleared after terminal resume
ownership ident. : writer_owner_ : Fiber* identity; readers are anonymous (counted)
fairness policy  : **phase-fair FIFO prefix batching** — a new reader CANNOT bypass
                   ANY queued waiter (reader OR writer); when queue head is a reader,
                   the maximal consecutive reader prefix is granted as one batch;
                   writer boundary stops the prefix at the first writer (single grant)
destruction      : caller-contract; ~AsyncRwLock asserts active_readers_==0,
                   !writer_active_, AND ~WaitQueue asserts head_==nullptr
handoff seam     : writer: writer_active_=true + writer_owner_=winner BEFORE publication
                   (winner-before-publication, mirrors mutex_handoff_one_locked)
                   reader: active_readers_ += batch_size BEFORE batch publication
                   (committed after all members claimed, BEFORE publication)
terminal causes  : Woken (read/write admission or prefix-batch/handoff grant) |
                   Cancelled | Expired
reconcile        : **EXPLICIT** — rwlock_cancel and rwlock_expire_wait both call
                   rwlock_grant_from_head_locked under a continuous G hold across two distinct W critical sections (unlink in the first W CS, head reconcile in the second);
                   reader prefix can join an existing reader phase; writer boundary
                   grants exactly one writer
admission precedence: resource admissible (no writer + empty queue for reader;
                   no writer + 0 readers + empty queue for writer) BEFORE already-due
                   deadline (resource-first)
upgrade/downgrade: DEFER (not in v1); recursive read/write: FORBID
```

---

## 5. Corrected cross-primitive matrices

The preparation matrix in `e12-sync-primitives-plan.md` §10.1/§10.2 is the
CHECKLIST. The matrices below are the **as-built authority**; rows that
contradict the preparation are flagged OBSOLETE.

### 5.1 Primitive state matrix

| Primitive | Primitive-local resource state | WaitQueues (count + role) | Per-operation context | Ownership identity | Close/reset state | Destruction state |
| --- | --- | --- | --- | --- | --- | --- |
| Event | `atomic<bool> set_` | 1 (waiters) | none | none | set/reset only; reset non-resolving | caller-contract; assert empty; no cancel |
| Semaphore | `atomic<uint32> available_`; `const max_permits_` | 1 (demand) | none | none | n/a | caller-contract; assert empty; no cancel |
| AsyncMutex | `Fiber* owner_` (no redundant bool) | 1 (waiters; only Suspended epochs) | none | `Fiber*` identity | n/a | caller-contract; assert `owner==nullptr` AND empty |
| AsyncCondition | (delegates to Mutex) | 1 own + Mutex's | caller WaitNode (cond) + stack-local WaitNode (reacquire) | via Mutex (`Fiber*`) | n/a | caller-contract; assert `active_waits_==0` |
| AsyncQueue | `unique_ptr<QueueItemLease[]> ring_`; `Open\|Closed`; `operational\|tearing_down`; `capacity>=1` | 2 (producer + consumer; never together) | `QueueWaitCtx` on `node.user_` | operation epoch (no persistent owner) | `close` monotonic+idempotent+drain; `begin_teardown` irreversible | quiet-state caller-contract for dtor; teardown session drains ring |
| AsyncRwLock | `size_t active_readers_`; `bool writer_active_`; `Fiber* writer_owner_` | **1 unified FIFO** (readers+writers; NOT 1–2) | `RwWaitCtx{Mode}` on `node.user_` | writer: `Fiber*`; readers: anonymous count | n/a | caller-contract; assert `active_readers_==0`, `!writer_active_`, empty |

**OBSOLETE preparation claims corrected above:**
- RwLock "1–2 (readers, writers)" → as-built is ONE unified FIFO queue.
- RwLock "reader-count + writer bool (+ policy)" + "read permit pre-increment/refund" → as-built commits `active_readers_` BEFORE batch publication under continuous G+W; **there is no per-node loser refund path** (no per-node CAS race inside a batch; the batch holds G+W continuously, cancel/expire require G+W, no interleaving).
- RwLock "upgrade state" → DEFER (not in v1).

### 5.2 Terminal cause matrix

Each cell records: terminal WaitNode outcome → resource state mutation → payload/owner transfer → timer action → queue unlink → publication → public return/result.

| Primitive | `RESOURCE_WAKE` | `TIMER_EXPIRE` | `CANCEL` | `CLOSE` / `RESET` / `NOTIFY` |
| --- | --- | --- | --- | --- |
| **Event** | `Woken`; Event state unchanged; no payload; retire timer in same CS; unlink; publish. Admission SET → `Woken` inline. | `Expired`; Event unchanged; timer CONSUMED; unlink; publish. | `Cancelled`; Event unchanged; retire timer; unlink; publish. | `set` triggers RESOURCE_WAKE drain (per-waiter `resolve_(Woken)`); `reset` flips bool only — does NOT resolve/cancel/unlink. |
| **Semaphore** | `Woken`; release transfers pending permit to FIFO head (`available_` UNCHANGED, `accepted_release_count++` conceptually, `acquiredCount++`); retire timer; unlink; publish. Immediate acquire: `available_--`. | `Expired`; no permit removed (no refund path — Conclusion A); timer CONSUMED; unlink; publish. | `Cancelled`; no permit removed; serialized before release observes queue; retire timer; unlink; publish. | n/a |
| **AsyncMutex** | `Woken` via handoff: `owner = winner Fiber` BEFORE publication; retire timer; unlink; publish. Admission acquire: `owner = me`. | `Expired`; no ownership; timer CONSUMED; unlink; publish. | `Cancelled` (queued only): unlink only; no owner change; retire timer; publish. Cancel after handoff loses. | n/a |
| **AsyncCondition** | `Woken` (Condition epoch): notify_one/all resolves FIFO head; retire timer; unlink; publish Condition winner. **Then mandatory Mutex reacquire** (separate ordinary-tail Mutex admission; non-cancellable; the latched Condition outcome is the return value). | `Expired` (Condition epoch): timer CONSUMED; unlink; publish. Then reacquire. **Admission already-due → Expired inline, Mutex NOT released, NO reacquire.** | `Cancelled` (Condition epoch only): unlink; retire timer; publish. Then reacquire (masked — the Condition cancel does NOT re-win reacquire). | `notify_one`/`notify_all` are the RESOURCE_WAKE sources; atomic snapshot-drain. |
| **AsyncQueue (producer)** | `Woken` ⇒ Completion in {Committed, Closed}. Committed: lease moved into ring slot; `--active_wait_associations_`, `--waiting_waitq_count_`, `++granted_not_resumed_`; publish. Closed: lease retained, returned to caller as `closed(T)`. | `Expired`; lease NEVER entered ring (retained by caller as `expired(T)`); timer CONSUMED; `--active_wait_associations_`, `--active_queue_timers_`, `--waiting_waitq_count_`; publish. | **DEFERRED in v1** (no public cancel API; P8 reserved). | `close`: monotonic Open→Closed; drains role FIFOs; blocked producers complete `closed(T)`; blocked consumers drain then `closed`. |
| **AsyncQueue (consumer)** | `Woken` ⇒ Completion in {Committed, Closed}. Committed: ring HEAD moved into `cons_out`; accounting + publish. Closed: `closed` (only at Closed+empty). | `Expired`; ring UNCHANGED (no item reserved); timer CONSUMED; accounting + publish. Returns `expired`. | **DEFERRED in v1** (C7 reserved). | (as above) |
| **AsyncRwLock (reader)** | `Woken` (admission or prefix-batch): `active_readers_` incremented BEFORE publication; retire timer; unlink; publish (batch caches next/fiber/owner before route). | `Expired`; no reader count change (increment had not happened for a queued reader; reader grant commits only at claim under G+W); timer CONSUMED; `--active_deadline_count_`, `--waiting_waitq_count_`; **head reconcile** via `rwlock_grant_from_head_locked`; publish expired waiter AFTER grants. | `Cancelled`; no reader count change; retire timer; accounting; **head reconcile**; publish cancel winner AFTER grants. | n/a |
| **AsyncRwLock (writer)** | `Woken` (admission or single-grant handoff): `writer_active_=true`, `writer_owner_=winner` BEFORE publication; retire timer; unlink; publish. | `Expired`; no writer state change; timer CONSUMED; accounting; **head reconcile**; publish. | `Cancelled`; no writer state change; retire timer; accounting; **head reconcile**; publish. | n/a |

**Important accuracy note on Queue terminal outcomes:** the public result type
is `QueuePushResult<T>`/`QueuePopResult<T>` with a distinct `status()` enum
(`committed`/`item`/`closed`/`expired`/`would_block`), NOT `WaitOutcome`. The
resolution cause (`WaitOutcome`) and the Queue operation completion
(`QueueCompletion`) are two separate dimensions coupled only at stable
boundaries (`Woken ⇒ Completion in {Committed, Closed}`; `Expired ⇒ Expired`).
This is NOT forced into the `WaitOutcome` shape (G-TERM-1 caveat satisfied).

### 5.3 Calling-context matrix

Five distinct calling contexts (per the task's hard rule against conflating
them):

| Operation | running Fiber | Scheduler worker, no current Fiber | external OS thread | owner Fiber only | capability holder |
| --- | --- | --- | --- | --- | --- |
| Event `is_set` / Semaphore `available` / Queue `is_closed`/`capacity`/`size` | ✓ | ✓ | ✓ | n/a | n/a |
| Event `set` / Semaphore `release` / Condition `notify_one`/`notify_all` | ✓ | ✓ | ✓ (ext-thread safe; `g_worker==nullptr` → route via `pending_spawn_`+`signal_wake_locked`) | n/a | n/a |
| Event `reset` | ✓ | ✓ | ✓ | n/a | n/a |
| Event `wait`/`wait_until`, Semaphore `acquire`/`acquire_until`, AsyncMutex `lock`/`lock_until`, AsyncRwLock `read_lock`/`write_lock`(+`_until`), Queue `push`/`pop`(+`_until`) | ✓ (Fiber-only; suspends) | ✗ | ✗ | n/a | n/a |
| `try_*` cross-thread-safe (Semaphore `try_acquire`; Queue `try_push`/`try_pop`; AsyncRwLock `try_read_lock`) | ✓ | ✓ | ✓ | n/a | n/a |
| AsyncMutex `try_lock` | ✓ (Fiber-only — successful acquisition commits `owner_` to the current Fiber) | ✗ | ✗ | n/a | n/a |
| AsyncRwLock `try_write_lock` | ✓ (Fiber-only — must record `writer_owner_`) | ✗ | ✗ | n/a | n/a |
| AsyncMutex `unlock` | ✓ | ✗ | ✗ | **✓ (owner Fiber only)** | n/a |
| AsyncRwLock `unlock_read` | ✓ | ✓ | ✓ (ext-thread safe) | n/a | ✓ (must hold a read share — not runtime-enforced in v1) |
| AsyncRwLock `unlock_write` | ✓ | ✗ | ✗ | **✓ (writer owner only)** | n/a |
| `cancel(WaitNode&)` (Event/Semaphore/AsyncMutex/AsyncCondition/AsyncRwLock) | ✓ | ✓ | ✓ (any thread) | n/a | n/a |
| Queue `cancel` | **N/A — no public cancel API in v1** | | | | |
| Queue `close` / `begin_teardown` / `take_next` | ✓ | ✓ | ✓ (close); teardown requires quiet-state | n/a | n/a |
| Construction | ✓ | ✓ | ✓ | n/a | n/a |
| Destruction | ✓ | ✓ | ✓ (requires quiescence: empty WaitQueue, no active condition waits, no mutex owner) | n/a | n/a |

**Notes on accuracy:**
- `AsyncMutex::try_lock` is Fiber-context-only because a successful acquisition commits `owner_` to the current Fiber identity. The internal critical section is thread-safe under `global_mtx_` + `waiters_.mtx()`, but that does NOT make the public call legal without a running Fiber (the implementation asserts `g_worker != nullptr` and reads `ws->current`).
- `unlock_read()` from an external OS thread is an **accepted v1 limitation**: a
  wrong-context `unlock_read` is NOT runtime-detected. This is documented in
  `e12-rwlock.md` and is not a defect.
- `unlock_write()` is Fiber-only because `writer_owner_` requires a valid
  `Fiber*` (E12-F Open Q #5 CLOSED).
- The capability-holder column is shown for `unlock_read` only; none of the
  other primitives track a runtime capability that a non-owner release would
  violate (Semaphore/Event releases are anonymous by design).

### 5.4 Destruction / fail-fast matrix

Category A = caller misuse / lifetime contract. Category B = internal authority
corruption.

| Primitive | Misuse class | Debug behavior | Release behavior | Death test | Auto cancel/wake/recovery |
| --- | --- | --- | --- | --- | --- |
| Event | destroy with registered waiters (A) | `~WaitQueue` asserts `head_==nullptr` | no-op (caller-owned nodes remain; UB if frames exit) | via `~WaitQueue` assertion path | **NONE** |
| Semaphore | destroy with registered waiters (A) | `~WaitQueue` asserts empty | no-op | via `~WaitQueue` | **NONE** |
| AsyncMutex | destroy while `owner_!=nullptr` (A); destroy with queued Registered (A); non-owner unlock (A); recursive lock (A) | `~AsyncMutex` asserts `owner_==nullptr`; `~WaitQueue` asserts empty; unlock/lock assert owner/recursion | no-op | `async_mutex_death_test` | **NONE** |
| AsyncCondition | destroy with Registered waiter (A); destroy with in-flight reacquire (A); wait by non-owner (A) | `~AsyncCondition` asserts `active_waits_==0` | no-op | (per-primitive) | **NONE** |
| AsyncQueue | destroy with active operation (A); second teardown session (A); `take_next` after session release (A) | quiet-state debug asserts in dtor / `begin_teardown` preconditions | no-op | (per-primitive) | **NONE** (teardown is an explicit, irreversible protocol, NOT destructor recovery) |
| AsyncRwLock | destroy with active readers (A); destroy with active writer (A); destroy with queued waiters (A); non-owner `unlock_write` (A); recursive write (A) | `~AsyncRwLock` asserts `active_readers_==0`, `!writer_active_`; `~WaitQueue` asserts empty | no-op | `async_rwlock_death_test` | **NONE** |
| RwLock null ExpireCtx in pump | internal: timer registered without a valid ExpireCtx (B) | `assert(false)+abort` at `scheduler.cpp:3862` | `abort` | (Category B invariant) | n/a — fail-fast |

**Destruction is not a resolver** (G-TERM-14): the only destructor-coordinated
drain in the system is the Queue's explicit `QueueTeardownSession` protocol
(NOT a destructor). No primitive destructor invents cancel-all / wake-all /
force-unlock / detach / synthesize.

---

## 6. G-TERM normative laws

These are the E12-G cross-primitive laws. Each is marked with its evidence
class:

```text
[AS-BUILT]    — proven by existing production code (file:line cited)
[NORMATIVE]   — new E12-G cross-law distilled from as-built parity
[INFERENCE]   — derived; flagged for independent review
```

### G-TERM-1 — One terminal winner [AS-BUILT + NORMATIVE]

For every wait epoch (one fresh `WaitNode`), at most one of `RESOURCE_WAKE`,
`TIMER_EXPIRE`, `CANCEL`, or a primitive-specific terminal (Queue `Closed`)
wins.

- The single winner authority is `WaitNode::resolve_`
  (`wait_node.hpp:241`), a CAS `registered -> {woken, cancelled, expired}`.
- Every loser CAS fails and performs no unlink, no publication, no resource
  mutation.
- **Queue accuracy caveat (satisfied):** Queue `Closed` is NOT represented as a
  `WaitOutcome` value. The resolution cause (`Woken`) and the operation
  completion (`QueueCompletion ∈ {Committed, Closed}`) are separate dimensions.
  A Queue producer/consumer that wins the `resolve_(Woken)` CAS returns either
  `committed` or `closed` depending on the `QueueCompletion` written by the
  reconciler before publication. The single-resolution-winner law still holds
  (one `resolve_` winner per epoch); only the result vocabulary is richer. The
  matrix is NOT forced into a 4-value `WaitOutcome` shape.

### G-TERM-2 — Winner owns unlink [AS-BUILT]

The terminal transition and the unlink/removal obligation are ONE coordinated
critical section. The winner CAS succeeds → the winner unlinks in the SAME
`q.mtx()` (and `global_mtx_`) critical section
(`wait_queue.hpp:199/222/245/266` + `unlink_locked:302`).

A loser:
```text
does NOT unlink
does NOT refund / does NOT decrement primitive resource counters
does NOT publish a runnable ticket
does NOT change owner / writer_owner / active_readers
```

### G-TERM-3 — Resource commit before publication [AS-BUILT]

When `RESOURCE_WAKE` represents a resource transfer, the primitive commits the
resource for the exact CAS winner BEFORE `make_runnable` + `route_runnable_locked`.

| Primitive | Commit before publication | Evidence |
| --- | --- | --- |
| Semaphore (release transfer) | pending permit transferred (conceptually `accepted_release_count++`, `acquiredCount++`); `available_` UNCHANGED | `sem_release` (`scheduler.cpp:2129`) |
| AsyncMutex (handoff) | `owner = winner Fiber` (`scheduler.cpp:2434`) BEFORE `make_runnable` | `mutex_handoff_one_locked` (`scheduler.cpp:2411`); `e12-async-mutex.md` §10.5 |
| AsyncCondition (notify + reacquire) | notify resolves Condition epoch; the latched Condition outcome is returned; **mandatory Mutex reacquire is a separate epoch** (return holds Mutex in all Condition-outcome cases — Model A) | `condition_wait_prepare*` (`scheduler.cpp:3569/3650`) |
| AsyncQueue (payload/ring ownership) | lease moved into ring (producer) or ring HEAD moved into `cons_out` (consumer) BEFORE publication; `QueueCompletion` written BEFORE publication | `queue_grant_producer_locked` (`scheduler.cpp:3505`); `queue_grant_consumer_locked` (`scheduler.cpp:3474`); F.6 retire-before-commit |
| AsyncRwLock (reader count) | `active_readers_ += batch_size` committed AFTER all members claimed, BEFORE batch publication | `rwlock_grant_from_head_locked` (`scheduler.cpp:2536`); `e12-rwlock.md` reader-batch section |
| AsyncRwLock (writer owner) | `writer_active_=true`, `writer_owner_=winner` BEFORE publication (mirrors `mutex_handoff_one_locked`) | `rwlock_write_lock` (`scheduler.cpp:2776`); `e12-rwlock.md` linearization table |
| **Event** | **N/A** — Event has no consumable resource. `set_` is published (flipped) before the per-waiter `resolve_(Woken)` drain, but no per-winner primitive state must be committed. This is correctly marked N/A; no虚假 commit is invented. | `event_set_broadcast` (`scheduler.cpp:1361`) |

### G-TERM-4 — Timer lifecycle exactly once [AS-BUILT]

Every timed epoch satisfies exactly one of:
```text
ACTIVE -> RETIRED    (non-timer winner closed the registration)
ACTIVE -> CONSUMED   (timer expiry won)
```

Audited counters and their decrement paths:

| Counter | Increment | Decrement paths (all legal) |
| --- | --- | --- |
| `active_deadline_count_` (Scheduler) | register in `await_wait_deadline` / `*_until` admission | (a) non-timer winner: `retire_timer_for_node_locked` ACTIVE→RETIRED; (b) timer winner: `pump_deadlines_locked` `try_claim_expiry` ACTIVE→CONSUMED; (c) admission already-due inline Expired (`try_claim_expiry`) |
| `waiting_waitq_count_` (Scheduler) | `register_wait_locked` success | every resolution (wake/cancel/expire/inline-admit/grant); guarded by `if (>0)` defense-in-depth |
| `active_queue_timers_` (QueuePort) | timer registration in `queue_*_admit_until` | `queue_timer_on_resolve` thunk fires exactly once per ACTIVE→terminal (retire OR consume) |
| `active_wait_associations_` (QueuePort) | successful P5/C4 link | (a) the unique winner immediately after unlink (grant paths); (b) `queue_cancel` (deferred — no public cancel in v1, so this path is reserved); (c) pump inline path for Queue-bound expired registrations (`scheduler.cpp:3888–3894`) |
| `granted_not_resumed_` (QueuePort) | suspended-winner ticket publication in grant seams | admit seams when the winner fiber resumes and the operation releases the owner slot |
| `active_port_calls_` (QueuePort) | ordinary QueuePort CallGuard after lifecycle check | same CallGuard at QueuePort return/unwind |
| `waiting_select_count_` (Scheduler) | E13 P6 Armed suspended SelectGroup | out of E12-G scope (E13 regression boundary) |

TimerRegistration pool/heap reclamation is **lazy-at-deadline** (a far-future
RETIRED entry remains physically in the heap+pool until `now >= its deadline`).
Lifetime safety is IMMEDIATE via the atomic `state_` gate (I4). This is the
closed E11 contract; E12 primitives inherit it unchanged.

### G-TERM-5 — State-before-node stale-timer law [AS-BUILT]

`pump_deadlines_locked` reads the stable `TimerRegistration::state_` (via
`try_claim_expiry`) BEFORE dereferencing `node()` / `queue()`
(`scheduler.cpp:3830`). Only an ACTIVE winner (the unique `try_claim_expiry`
CAS winner) dereferences the node.

Every primitive-specific expiry hook preserves this law:
- Generic path: inline `expire_locked` under fresh `q.mtx()` AFTER the claim.
- Queue path: same inline path, additionally invokes the per-port `on_resolve_`
  thunk and `--active_wait_associations_` (the QueuePort address is stable for
  the AsyncQueue lifetime).
- RwLock path: routes to `rwlock_expire_wait` (which acquires W internally);
  identifies the timer by `on_resolve_ == &rwlock_timer_expire_reconcile`; the
  `owner_ctx_` is a stable `AsyncRwLock::ExpireCtx` (address-stable for the
  AsyncRwLock lifetime).

A null `ExpireCtx` for an RwLock timer is a Category B internal invariant
violation → `assert(false) + std::abort` (`scheduler.cpp:3862`). Silently
erasing would leave the bound node unresolved.

### G-TERM-6 — Admission precedence [AS-BUILT + NORMATIVE]

For "resource available + deadline already due" at admission:

| Primitive | Precedence | Authority |
| --- | --- | --- |
| Event | **resource-first** (SET precedence over already-due deadline → Woken) | `e12-event.md` §3.7 F-EVENT-DEADLINE; `await_event_wait_deadline` |
| Semaphore | **resource-first** (permit admissible + due → Woken) | A4; `sem_acquire_until` |
| AsyncMutex | **resource-first** (free + FIFO-head + due → Woken) | `mutex_lock_until` |
| AsyncQueue | **resource-first** (free slot / available item + due → Committed) | `queue_*_admit_until` |
| AsyncRwLock | **resource-first** (admissible reader/writer + due → Woken) | `rwlock_*_lock_until` |
| **AsyncCondition** | **deadline-FIRST** (already-due → Expired inline, Mutex NOT released, NO reacquire epoch) | C-H4; `condition_wait_prepare_until` (`scheduler.cpp:3695`) |

**The Condition divergence is deliberate and documented**, not a parity
failure. The Condition epoch owns its own deadline; releasing the Mutex and
then immediately re-acquiring it (because the deadline already elapsed) would
be wasted work and would violate the C-H4 untimed-reacquire contract. The
divergence has explicit authority (`e12-condition.md` C-H4;
`docs/api-reference.md` line 769) and a parity test
(`cond_t1_already_due_inline_expired_retains_ownership`).

### G-TERM-7 — Queue-identity-safe cancellation [AS-BUILT]

Public `cancel(WaitNode&)` cancels ONLY a node currently Registered AND linked
in THIS primitive's current WaitQueue. Foreign primitive / foreign queue /
detached / terminal node → `false` WITHOUT mutation.

- The membership gate is `WaitQueue::contains_locked`
  (`wait_queue.hpp:289`), private, Scheduler-friend only. It scans THIS
  queue's intrusive list while holding THIS Scheduler's `global_mtx_` + THIS
  queue's `mtx()`. It NEVER reads a foreign node's `home_` and NEVER locks a
  foreign primitive/Scheduler.
- Per-primitive cancel seams all call `contains_locked` BEFORE the resolve CAS:
  `event_cancel_wait`, `sem_cancel`, `mutex_cancel`,
  `condition_cancel_wait`, `rwlock_cancel`.
- Cross-Scheduler wrong-object cancel is synchronized and structurally safe
  (proven by `event_wrong_event_*_scheduler_loses_safely`,
  `sem_t24/t25_wrong_semaphore_*`, `mtx_t8/t9_wrong_mutex_*`,
  `cond_t31_cancel_wrong_condition_returns_false`, `rwlock_t13_cancel_foreign_*`).
- **Queue v1 has NO public cancel API** → classified `DEFERRED — NOT A PARITY
  FAILURE`. P8/C7 and PUB-P-*-CANCEL are reserved identifiers, absent from v1.
  This audit MUST NOT add a Queue cancel API.

### G-TERM-8 — Terminal-removal progress law [NORMATIVE + AS-BUILT per-primitive]

> Removing a terminal winner MUST NOT strand a successor that is already
> admissible under the current resource state.

Per-primitive classification:

| Primitive | Class | Proof / evidence |
| --- | --- | --- |
| **Event** | NONE | Event has no consumable resource. `set()` drain already resolved every registered waiter; removal of a terminal waiter (cancel/expire winner) does not change Event state and does not strand any successor (a successor will be woken by a future `set()`, or by its own cancel/expire). |
| **AsyncCondition (Condition epoch)** | NONE | The Condition queue is broadcast-only; `notify_one/all` resolve waiters; cancel/expire of one waiter does not change Condition state. (The Mutex reacquire epoch is governed by Mutex's IMPLICIT class below.) |
| **Semaphore** | IMPLICIT / STRUCTURALLY IMPOSSIBLE | Stable-state invariant `EligibleQueuedWaiterExists ⇒ available_ == 0` (Conclusion A). Cancel/expire of the FIFO head happens under `global_mtx_`, unlinking the head BEFORE a later `release` observes the queue. There is no state where `available_ > 0` AND an eligible queued waiter exists. So head removal cannot strand a permit. Proof: `e12-semaphore.md` §5.4; `sem_cancel`/`sem_acquire_until` admission recheck + `sem_release` serialization. |
| **AsyncMutex** | IMPLICIT / STRUCTURALLY IMPOSSIBLE | The handoff is ONE atomic transition: `wake_one_locked` → `owner := winner` → unlink, all in the same `global_mtx_`+`q.mtx()` CS. Cancel/expire of a Suspended queued node unlinks it; the successor becomes the new FIFO head and is handed off at the NEXT `unlock`. A stable observable state `owner == nullptr ∧ non-empty wait queue ∧ no in-flight handoff` does not exist: either the queue is empty (no successor to strand) OR a handoff is committed atomically with the unlink. Cancel/expire of an already-Woken (handed-off) node loses (Category B fail-fast); there is no window where the head is Woken but `owner` is not yet committed. Proof: `mutex_handoff_one_locked` (`scheduler.cpp:2411`); `e12-async-mutex.md` §10.5; test `mtx_t22_cancelled_head_then_handoff`. |
| **AsyncQueue** | IMPLICIT / STRUCTURALLY IMPOSSIBLE | Producer expiry (P9): lease NEVER entered ring (no slot consumed) → nothing to reconcile. Consumer expiry (C8): ring UNCHANGED (no item reserved) → nothing to reconcile. The single `resolve_` CAS plus G+S serialization is the sole arbiter; expiry and grant commit are mutually exclusive via the CAS (only one performs a ring mutation, and expiry never does). Close-vs-expiry is serialized by G+S. Counter deltas for expiry are only `W-1, T-1` (no ring delta). Proof: `e12-queue-state-machine.md` §3.1/§3.2/§8; `e12-queue.md` §D.4 Case A/B; tests `queue_g1_push_until_expires_recovers_value`, `queue_g1_pop_until_expires`, `queue_h1..h4`. |
| **AsyncRwLock** | **EXPLICIT RECONCILE** | `rwlock_cancel` and `rwlock_expire_wait` BOTH call `rwlock_grant_from_head_locked` under a continuous G hold across two distinct W critical sections: the first W CS performs unlink + accounting, then W is released while G remains held, and the second W CS performs head reconciliation. This may grant the newly-exposed head: reader prefix can join an existing reader phase (`writer_active_==false ∧ active_readers_>0` → join); writer boundary grants exactly one writer. The reconcile is MANDATORY regardless of whether the removed node was the head (a non-head removal is a no-op reconcile because the head is unchanged). The dedicated `rwlock_expire_wait` seam exists precisely because the generic `expire_wait` does NOT reconcile. Proof: `e12-rwlock.md` §"Cancel and expiry queue advancement"; `rwlock_cancel` (`scheduler.cpp:2865`); `rwlock_expire_wait` (`scheduler.cpp:2898`); tests `rwlock_head_writer_cancel_grants_reader_prefix_immediately`, `rwlock_head_writer_expiry_grants_reader_prefix_immediately`, `rwlock_cancel_reconcile_preserves_fifo`, `rwlock_expiry_reconcile_preserves_fifo`. |

The "global_mtx_ serializes so safe" hand-wave is REJECTED for every primitive
above; each row gives the full state argument.

### G-TERM-9 — FIFO / no-barging parity [AS-BUILT + NORMATIVE]

Each primitive's actual fairness is recorded; different semantics are NOT
conflated.

| Primitive | Fairness | Evidence |
| --- | --- | --- |
| Event broadcast | set releases ALL registered waiters satisfied by SET (one `resolve_(Woken)` per epoch); no FIFO ordering claim between independent waiters | `event_set_broadcast` drain loop |
| Semaphore | FIFO selection among already-queued waiters; **barging FORBIDDEN** (`try_acquire` fails iff eligible queued waiter has FIFO priority) | A2; `sem_try_acquire` empty-queue gate |
| AsyncMutex | FIFO + direct handoff + **no-barging** (M-H2/H3/H4); `try_lock` fails while an eligible waiter has FIFO priority | `mutex_try_lock` empty-queue gate; `mutex_handoff_one_locked` |
| AsyncCondition | Condition epoch: FIFO notify-one (head) + atomic snapshot-drain notify-all. **Reacquire: ordinary AsyncMutex FIFO-TAIL admission** (C-H8 — no notified priority). Notified order does NOT guarantee Mutex return order. | `condition_notify_one/all`; `e12-condition.md` §8, C-H8 |
| AsyncQueue | producer/consumer FIFO + own-oldest/global-oldest ticket selection; active-admission owner does NOT block stealing | `pop_queue_runnable_locked`; `e12-queue-scheduler-integration.md` §10 |
| AsyncRwLock | **phase-fair FIFO prefix batching**: new reader CANNOT bypass ANY queued waiter (reader OR writer); queue-head reader → maximal consecutive reader prefix granted as one batch; queue-head writer → single grant. Stronger than AsyncMutex no-barging (prohibits both reader-past-writer AND new-reader-past-queued-reader). | `rwlock_grant_from_head_locked`; `e12-rwlock.md` fairness policy |

### G-TERM-10 — External-thread publication [AS-BUILT]

Every external-thread-callable resolver routes through the Scheduler canonical
path: `make_runnable` → `route_runnable_locked` → `signal_wake_locked`.

- Canonical routing is determined by the **explicit `WorkerState* owner`**
  supplied to `route_runnable_locked` (`scheduler.cpp:968`), not directly by
  `g_worker` inside the routing helper: `owner != nullptr` → enqueue
  `owner->local_runnable`; `owner == nullptr` → enqueue `pending_spawn_`;
  either way `signal_wake_locked` fires.
- Resolver families differ in how they obtain the owner:
  - Generic Event / Semaphore / Condition / AsyncMutex wake/cancel paths
    commonly pass `g_worker`; an external caller therefore supplies `nullptr`
    and routes through `pending_spawn_`.
  - Queue / AsyncRwLock specialized resolver paths capture the canonical
    `WorkerState*` from `fiber_owner_` before publication; an external caller
    can therefore still route directly to an owner inbox when the Fiber has a
    recorded home worker.
- All resolver families share the terminal-winner → `make_runnable`
  publication guard → `route_runnable_locked(explicit owner)` →
  `signal_wake_locked` shape.
- Evidence: `event_external_thread_set_wakes_live`, `sem_t26_external_thread_release_wakes_live`, `cond_t24_external_thread_notify`, `mtx_t10_external_thread_cancel_succeeds`.

### G-TERM-11 — Context lifetime [AS-BUILT]

| Context | Stable address | Valid during ACTIVE registration | Not read after unlink/publication | No stale-callback UAF |
| --- | --- | --- | --- | --- |
| `QueueWaitCtx` (per Queue wait-op) | stack-local in the admit frame; alive for the wait-epoch | ✓ | ✓ (reconciler reads `won->user()` only inside the G+S+W CS before publication) | ✓ |
| `RwWaitCtx` (per RwLock wait-op) | stack-local in the lock frame; alive for the wait-epoch | ✓ | ✓ (cleared after terminal resume) | ✓ |
| `TimerRegistration::owner_ctx_` (Queue: `&port`; RwLock: `&ExpireCtx`) | address-stable for the primitive lifetime | ✓ | ✓ (fired exactly once per ACTIVE→terminal) | ✓ |
| `TimerRegistration::on_resolve_` hook context | (same as owner_ctx_) | ✓ | ✓ | ✓ |
| Queue teardown/session context | `QueueTeardownSession` is move-only, non-copyable; port outlives session | ✓ | ✓ | ✓ |

### G-TERM-12 — No post-publication object access [AS-BUILT]

After `route_runnable_locked`, a resolver MUST NOT dereference:
```text
the winning WaitNode
the Fiber stack context
the operation lease holder
```

The load-bearing concern is the RwLock reader batch publication. Evidence
(`e12-rwlock.md` §"Intrusive publication list"):
- `next` is cached BEFORE claim (claim clears `next_/prev_`).
- In the publication loop, `pub_next`, `fib`, `owner` are cached BEFORE
  `route_runnable_locked`.
- After `route_runnable_locked`, the current WaitNode is NOT dereferenced. The
  woken Fiber may resume on another worker and immediately destroy/reuse the
  WaitNode.
- Temporary `next_/prev_` linkage is cleared BEFORE publication.
- Dedicated test: `rwlock_batch_publication_does_not_access_published_node`.

The Scheduler holds `global_mtx_` across the batch publication, which prevents
the resumed Fiber from destroying its node mid-publication (the Fiber cannot
re-enter a Scheduler seam that takes `global_mtx_` until the holder releases).
This synchronization argument is explicit, NOT merely a reliance on cached
`next`.

For Queue and Mutex handoff, the commit-then-publish ordering under continuous
G (+S/+W) ensures the resolver never touches the resumed node after
publication; the `user_` context is read only inside the pre-publication CS.

### G-TERM-13 — Accounting ledger [AS-BUILT]

See G-TERM-4 table. For each increment, all legal decrement paths are listed.
The guards prevent numeric underflow in the current implementation, but are
not themselves proof of accounting correctness: an unexpected double-decrement
would be masked by leaving the counter at zero. The PASS conclusion comes from
the path-by-path increment/decrement ledger, terminal-winner ownership, and
exactly-once timer-state transition analysis in G-TERM-4. WaitQueue structural
registration authority is sealed to Scheduler-owned seams: `Scheduler::await_wait`
is one generic seam; E12 primitives register through additional primitive-specific
Scheduler admission seams (`await_wait_deadline`, `await_event_wait*`, `sem_acquire*`,
`mutex_lock*`, `condition_wait_prepare*`, `queue_push_admit*`, `queue_pop_admit*`,
`rwlock_read_lock*`, `rwlock_write_lock*`), all funneling through `WaitQueue::register_wait_locked`.
The Queue `active_wait_associations_` / `active_queue_timers_` / `granted_not_resumed_`
live on QueuePort (queue_port.hpp:429–431) and are reached by Scheduler via
the `friend class ::sluice::async::detail::QueuePort` grant.

### G-TERM-14 — Destruction is not a resolver [AS-BUILT]

Except for the explicit `QueueTeardownSession` protocol (an irreversible
user-driven drain, NOT a destructor), no primitive destructor invents:
```text
cancel all
wake all
force unlock
detach waiters
synthesize Closed / Woken / Cancelled
```

Per-primitive destructor contracts are in §5.4. Every destructor is at most a
debug assertion + no-op release; recovery is the caller's responsibility.

---

## 7. Primitive-by-primitive audit

For each primitive, the §10 audit questions are answered against as-built
evidence.

### 7.1 Event

| Question | Answer | Evidence |
| --- | --- | --- |
| `set()` atomic snapshot/drain | One continuous G CS: `set_.exchange(true)` then loop `wake_wait_one_locked` until empty. Atomic w.r.t. `reset()` and admission. | `event_set_broadcast` (`scheduler.cpp:1361`); `e12-event.md` §6 |
| `set` vs cancel/expiry per-node independence | Each waiter resolves through its OWN `resolve_`; one timer winning one waiter does not defeat `set()` on the others. | `e12-event.md` §6.4; `event_one_cancels_during_set_broadcast`, `event_one_expires_during_set_broadcast` |
| Late waiter while SET | Admission observes SET → returns Woken inline (no suspend). | `await_event_wait`; `event_late_waiter_after_set_returns_immediately` |
| `reset` does not resolve waiters | `reset` flips bool only; does NOT cancel/wake/unlink. | `event_reset`; `e12-event.md` §3.5; `event_reset_does_not_cancel_registered_waiter` |
| Broadcast timer retirement exactly once | Per-winner retire in the drain loop; each winner retired exactly once. | `event_set_broadcast` per-iteration `retire_timer_for_node_locked` |
| External-thread `set`/`cancel` | Safe; `g_worker==nullptr` → route via `pending_spawn_`+`signal_wake_locked`. | `event_external_thread_set_wakes_live`, `event_parked_live_worker_awakened_by_external_set` |
| Destruction with registered waiters | caller-contract violation; `~WaitQueue` asserts empty; no cancel/wake. | `e12-event.md` §3.9 |
| Set/reset epoch isolation | `global_mtx_` serialization makes OLD_SET_WAKES_POST_RESET_WAITER mechanically impossible (no generation counter needed). | `e12-event.md` §7; `event_old_set_does_not_wake_post_reset_waiter`, causal T27/T30/T31 |

**Verdict: PASS.** No defect. No DOC-DRIFT.

### 7.2 Semaphore

| Question | Answer | Evidence |
| --- | --- | --- |
| Release-created permit transfer/store/reject | Atomic disposition under one G CS: transfer to FIFO head (`available_` UNCHANGED), store++ if `available_<max`, overflow-false if `available_==max`. | `sem_release` (`scheduler.cpp:2129`); A1 |
| Cancel/expiry never needs permit refund | No permit was ever removed; cancel/expire serialize before release observes queue (Conclusion A). | `e12-semaphore.md` §5; `sem_t13_w1_cancelled_before_release_grants_w2` |
| `available > 0` + queued waiter stable state | NOT reachable: stable-state invariant `EligibleQueuedWaiterExists ⇒ available_ == 0`. | `e12-semaphore.md` §5.4 |
| Head cancel stranding a permit | Impossible (Conclusion A): cancel unlinks head under G before release observes queue. | `sem_t13_*` |
| Release vs cancel/expiry | Serialized by G; the winner of the resolve CAS performs its mutation; the loser is no-op. | `e12-semaphore.md` §5.2 |
| Overflow path accounting | `available_==max ∧ no eligible waiter` → return false, NO mutation (not even history fields in production). | A1; `sem_t7_release_at_capacity_false_no_mutation` |

**Verdict: PASS.** No defect. No DOC-DRIFT (the preparation §5 obsolete
"pre-decrement/refund" / "granted_in_flight" model is already explicitly
marked OBSOLETE in `e12-semaphore.md` §3.1 and in the plan §5.1 historical
passage).

### 7.3 AsyncMutex

| Question | Answer | Evidence |
| --- | --- | --- |
| Direct owner handoff | `mutex_handoff_one_locked`: `wake_one_locked` → `owner = winner` → retire timer → `--waiting_waitq_count_` → `make_runnable`+`route`. | `scheduler.cpp:2411`; M-H1 |
| Owner commit before publication | `owner = f` BEFORE `make_runnable`/`route_runnable_locked`. | `e12-async-mutex.md` §10.5; `mtx_t5_owner_before_publication_phase` |
| Cancel/expiry after handoff loses | Late terminal attempts against Woken epochs are no-ops (`CancelAttemptTerminal`/`ExpireAttemptTerminal`). | `e12-async-mutex.md` §14.6; `mtx_t16_handoff_wins_before_cancel` |
| `owner == nullptr` + non-empty queue externally observable | NOT observable: handoff is atomic with unlink; queue non-empty implies either in-flight handoff (under G) or a future unlock will hand off. | `mutex_handoff_one_locked` atomicity |
| Head cancel stranding a free mutex | Impossible: cancel of a queued waiter just unlinks; the Mutex is still owned (or was just atomically handed off). | `mtx_t22_cancelled_head_then_handoff` |
| Non-owner unlock Category A | Debug assert; release no-op; not a supported no-op. | `e12-async-mutex.md` §7.3 |
| Immediate-path outcomes do NOT publish runnable | `LockImmediate` / `LockUntilImmediate` etc. resolve terminal but carry "no runnable publication". | `e12-async-mutex.md` §14.6; `InvPublicationRequiresSuspensionOrHandoff`; NEG-M10 |

**Verdict: PASS.** No defect. No DOC-DRIFT.

### 7.4 AsyncCondition

Model A is authoritative (C-H1 CLOSED). The Model A/B preparation debate is
NOT reopened.

| Question | Answer | Evidence |
| --- | --- | --- |
| Condition epoch can be Woken/Cancelled/Expired | Yes — through the single `resolve_` authority. | `e12-condition.md` §10.1 |
| Return-before mandatory Mutex reacquire | NO — Model A: return only after reacquire. | C-H1 |
| Reacquire untimed | YES (C-H4) — the Condition deadline does NOT govern reacquire. | `condition_wait_prepare_until` (deadline governs only Condition epoch) |
| Reacquire non-cancellable | YES (C-H5) — reacquire node is stack-local, NOT exposed to `cancel()`. | `e12-condition.md` §4.2 |
| Return always holds Mutex | YES (Model A, all three Condition outcomes). | `cond_t16_wait_returns_owning_reacquire`, `cond_t17_reacquire_after_expired`, `cond_t18_reacquire_after_cancelled` |
| Caller node + stack-local reacquire node | YES — caller provides Condition node; reacquire uses a stack-local `WaitNode reacquire_node` inside `wait`. | `e12-condition.md` §4.2, §12 |
| Condition cancel/expiry cannot leak into reacquire | YES — reacquire is a separate epoch; the Condition cancel/expiry is latched and does not re-win. | `e12-condition.md` §11.2 |
| Latched Condition outcome survives reacquire | YES — the return value is the latched Condition reason. | `cond_t17/t18` |
| `notify_all` snapshot/drain | Atomic snapshot-drain under one continuous G hold (loop `wake_wait_one_locked`). | `condition_notify_all` (`scheduler.cpp:3745`); `cond_t10_notify_all_snapshot`, `cond_t11_notify_all_excludes_late_waiter` |
| Destruction with waiter | caller-contract; `active_waits_==0` assert; no notify/cancel/force. | `e12-condition.md` §14 |
| Lost-notify window closed | CONDITION-WAIT-PREPARE: register cond_node BEFORE releasing Mutex, all under continuous G. | `e12-condition.md` §6/§7; `cond_t3_register_before_release_closure`, `cond_t4_notify_in_release_register_boundary`, `cond_t30_lost_notify_window_50` |

**Verdict: PASS.** No defect. No DOC-DRIFT.

### 7.5 AsyncQueue

Strictly Queue v1 as-built.

| Question | Answer | Evidence |
| --- | --- | --- |
| External cancellation deferred | YES — no public cancel API; P8/C7 reserved. **DEFERRED — NOT A PARITY FAILURE.** | `e12-queue.md` §1.3; `api-reference.md` AsyncQueue section |
| Two-role WaitQueues | producer (not-full) + consumer (not-empty); NEVER held together. | `e12-queue-state-machine.md` §1.4, §7.6 |
| Result-bearing selected-waiter grant | `QueuePushResult<T>`/`QueuePopResult<T>` with `status()`; `WaitOutcome` is the cause, `QueueCompletion` is the completion. | `e12-queue.md` §"Separate scheduler outcome from Queue completion" |
| Payload/ring transfer before publication | grant seams move lease into ring (producer) or ring HEAD into `cons_out` (consumer) BEFORE publication; F.6 retire-before-commit. | `queue_grant_producer/consumer_locked` |
| Close monotonic + drain | `close` Open→Closed monotonic idempotent; drains role FIFOs. | `QueuePort::close`; `queue_close_idempotent_and_closed_empty_terminal`, `queue_closed_drains_buffered_then_closed` |
| Producer/consumer timer counters | `active_queue_timers_`, `active_wait_associations_` on QueuePort. | queue_port.hpp:429–430; `queue_timer_on_resolve` |
| `owner_ctx` / `on_resolve` hooks | `owner_ctx_ = &port`, `on_resolve_ = &queue_timer_on_resolve`; invoked exactly once per ACTIVE→terminal. | `e12-queue-corrective-3.md` (referenced); `scheduler.cpp:3167` |
| Teardown session | irreversible `operational→tearing_down`; preconditions zero active counters + empty role queues. | `QueuePort::begin_teardown`; `queue_p7_*` |
| Producer expiry removing head when a free slot exists | **Structurally impossible**: producer expiry (P9) NEVER enters the ring (no slot consumed). The single `resolve_` CAS plus G+S serialization means expiry and grant commit are mutually exclusive. | `e12-queue-state-machine.md` §3.1 P9 row; `queue_g1_push_until_expires_recovers_value` |
| Consumer expiry removing head when a ring item exists | **Structurally impossible**: consumer expiry (C8) NEVER mutates the ring (no item reserved). | `e12-queue-state-machine.md` §3.2 C8 row; `queue_g1_pop_until_expires` |
| Resource reconciler atomic commit invariant | Holds: the loser semantic prevents any partial ring mutation; only the CAS winner performs a ring mutation, and expiry never does. | `e12-queue.md` §D.4 Case A/B |
| Close vs expiry | Serialized by G+S; no interleaving window. | `e12-queue.md` §"Close-vs-producer commit race" |
| Grant vs expiry | Mutually exclusive via the `resolve_` CAS; the loser is no-op. | G-TERM-8 Queue row |
| Stale TimerRegistration context | I4 state-before-node gate; on_resolve fires only for an ACTIVE winner; QueuePort address is stable for the AsyncQueue lifetime. | `pump_deadlines_locked` Queue branch |

**Verdict: PASS.** No defect. No DOC-DRIFT.

### 7.6 AsyncRwLock

| Question | Answer | Evidence |
| --- | --- | --- |
| One unified FIFO WaitQueue | YES — single `waiters_` for readers + writers. | `e12-rwlock.md` §"RwLock state" |
| Read/write mode context | `RwWaitCtx { Mode }` via `node.user_` (controlled hook). | `e12-rwlock.md` lines 286–336 |
| New reader cannot bypass any queued waiter | Fast path requires `!writer_active_ && waiters_.empty()`. | `rwlock_try_read_lock`; `rwlock_read_lock` admission |
| Head reader prefix batch | `rwlock_grant_from_head_locked` dispatches on head.mode; reader → maximal consecutive prefix. | `rwlock_grant_readers_locked`; `rwlock_t11_reader_batch_stops_at_writer` |
| Head writer single grant | prefix stops at first writer; exactly one writer granted. | `rwlock_grant_one_writer_locked`; `rwlock_t4_writer_blocks_readers_batch_grant` |
| Cancel/expiry explicit head reconcile | MANDATORY: both `rwlock_cancel` and `rwlock_expire_wait` call `rwlock_grant_from_head_locked` under a continuous G hold across two distinct W critical sections (unlink in the first, head reconcile in the second). | `e12-rwlock.md` §"Cancel and expiry queue advancement"; `rwlock_head_writer_cancel_grants_reader_prefix_immediately`, `rwlock_head_writer_expiry_grants_reader_prefix_immediately`, `rwlock_cancel_reconcile_preserves_fifo`, `rwlock_expiry_reconcile_preserves_fifo` |
| Reader prefix can join existing reader phase | YES — after W1 cancel/expiry, R2 (new head) joins R1's reader phase immediately (`writer_active_==false ∧ active_readers_>0`). | `e12-rwlock.md` §9.1 reader-prefix-joining scenario |
| Writer owner commit before publication | `writer_active_=true, writer_owner_=winner` BEFORE publication. | `rwlock_write_lock`/`rwlock_grant_one_writer_locked` |
| Batch publication does NOT access resumed WaitNode | YES — next/fiber/owner cached before route; no post-route dereference. | `e12-rwlock.md` §"Intrusive publication list"; `rwlock_batch_publication_does_not_access_published_node` |

**OBSOLETE preparation claims NOT in the as-built matrix:**
- read permit pre-increment/refund — the as-built commits `active_readers_` only at claim under continuous G+W; no per-node refund path exists.
- 1–2 queues — as-built is ONE unified queue.
- upgrade state — DEFER (not in v1).

**Verdict: PASS.** No defect. **One DOC-DRIFT** (the preparation matrix row;
corrected in §5.1). The RwLock formal model is the newest and thinnest (see
§11, formal strategy F1 + non-blocking observation).

---

## 8. Shared TimerRegistration / accounting audit

The shared substrate (E11 TimerRegistration + the Scheduler counters) is
audited once here; every primitive inherits it.

### 8.1 TimerRegistration lifetime (E11 I4)

```text
state_            : atomic {active, retired, consumed}
node_ / queue_    : immutable after registration; captures WaitNode& (NOT only Fiber*)
on_resolve_       : function ptr; null for non-Queue/non-RwLock waits
owner_ctx_        : void*; null for ordinary waits; &port for Queue; &ExpireCtx for RwLock
```

Lifetime law: a non-timer winner retires the registration (ACTIVE→RETIRED) in
the SAME G CS as its resolve CAS, BEFORE runnable publication. A timer winner
claims it (ACTIVE→CONSUMED) in `pump_deadlines_locked` BEFORE dereferencing
node/queue. Physical reclamation is lazy-at-deadline; lifetime safety is
immediate via the atomic gate. This is the closed E11 contract; E12 primitives
do NOT alter it.

### 8.2 `pump_deadlines_locked` routing (G-TERM-5 + per-primitive reconcile)

```text
for each due heap-min:
    heap_pop_min_locked
    if Select kind: select branch (out of E12-G scope)
    else ordinary:
        if NOT try_claim_expiry: erase inert entry; continue
        --active_deadline_count_
        n = top->node(); q = top->queue()
        if top->on_resolve_ == &rwlock_timer_expire_reconcile:
            rwlock_expire_wait(...)         # acquires W internally; head reconcile
        else if has_on_resolve (Queue):
            inline expire_locked under q.mtx()
            --port->active_wait_associations_
            fire_on_resolve_locked(true)   # --active_queue_timers_
            --waiting_waitq_count_; make_runnable+route
        else (generic):
            inline expire_locked under q.mtx()
            --waiting_waitq_count_; make_runnable+route
        erase_popped_registration_locked(top)
```

This routing satisfies G-TERM-5 (state-before-node) uniformly: the
`try_claim_expiry` gate is the single ACTIVE→CONSUMED winner authority; every
downstream dereference is gated on it.

### 8.3 Counter ledger (G-TERM-13)

See G-TERM-4 table. The cross-cutting audit conclusion: every increment has at
least one named legal decrement path. The `if (>0)` guards exist and prevent
numeric underflow, but are not themselves correctness proof; the PASS conclusion
rests on the path-by-path increment/decrement ledger and terminal-winner ownership.

---

## 9. Adversarial traces

For each applicable race, the linearization topology is recorded. The proof
form required by the task (initial state, queue topology, competing
authorities, lock order, winner linearization point, post-winner state,
publication count, loser behavior) is given for the load-bearing races.

### 9.1 RESOURCE_WAKE vs CANCEL (Semaphore, AsyncMutex)

```text
initial state       : Semaphore available_=0 (or Mutex owner==other); W1 queued (FIFO head)
queue topology      : [W1]
competing authorities: release (RESOURCE_WAKE) vs cancel_wait(W1) (CANCEL)
lock acquisition     : both take global_mtx_ first
winner linearization : the first to acquire global_mtx_ performs its resolve_ CAS
                       (the second sees W1 already unlinked)
resource state after : (Semaphore) release either transferred to W1 (available_ unchanged)
                       or, if cancel won, stored available_++ at the next release
                       (Mutex) if release won: owner=W1.fiber; if cancel won: owner unchanged
timer state after    : non-timer winner retires W1's timer in the same CS
queue membership after: W1 unlinked by the winner
publication count    : exactly one (the winner)
loser behavior       : release/cancel loser returns nullptr/false; no unlink, no publish
```
Evidence: `sem_t13_w1_cancelled_before_release_grants_w2`, `mtx_t15/t16`.

### 9.2 RESOURCE_WAKE vs EXPIRE

Same shape as 9.1 with TIMER_EXPIRE replacing CANCEL. The timer winner path
goes through `pump_deadlines_locked` (which holds G), so it serializes with
the release/handoff path identically.

Evidence: `sem_t17/t18`, `mtx_t13/t14`, `rwlock_t8_write_lock_until_expiry`,
`rwlock_t9_read_lock_until_resource_first`, `queue_g1_*`.

### 9.3 CANCEL vs EXPIRE

Both take G; the first to win the `resolve_` CAS is the unique winner. The
non-timer winner (cancel) retires the timer; the timer winner (expire) consumes
it. Exactly one publication.

Evidence: `timer_cancel_wins_timer_loses`, `event_timer_expire_wins_cancel`,
`event_resource_wake_wins_cancel`.

### 9.4 CLOSE/SET/NOTIFY vs EXPIRE

- Event `set` vs per-waiter timer: each waiter resolves independently through
  its own `resolve_`; one timer winning one waiter does not defeat `set()` on
  the others (G-TERM-1 + Event §6.4).
- Queue `close` vs producer/consumer expiry: serialized by G+S; close either
  observes pre-expiry state (expiry completes under G+S) or post-expiry state;
  no half-state. A blocked producer woken by close returns `closed(T)` (lease
  retained); a blocked consumer drains then `closed`.
- Condition `notify_*` vs Condition-epoch expiry: serialized by G.

Evidence: `event_one_expires_during_set_broadcast`, `queue_p5_close_*`,
`cond_t13a/t13b`.

### 9.5 External-thread resolver vs worker resolver

Both take `global_mtx_`; serialization is identical to the worker-vs-worker
case. The external thread sees `g_worker==nullptr`, so `route_runnable_locked`
routes via `pending_spawn_` + `signal_wake_locked`. No path uses a stale
current worker.

Evidence: `event_external_thread_set_wakes_live`, `sem_t26_*`,
`cond_t24_external_thread_notify`, `mtx_t10_external_thread_cancel_succeeds`.

### 9.6 Last owner release vs head cancellation (AsyncMutex, AsyncRwLock)

- AsyncMutex: `mutex_unlock` calls `mutex_handoff_one_locked` under G. If a
  concurrent `mutex_cancel(head)` runs first, it unlinks the head under G; the
  handoff then finds the new head (or empty) — no stranding (G-TERM-8 IMPLICIT).
- AsyncRwLock: `rwlock_unlock_write` clears writer state under G, then calls
  `rwlock_grant_from_head_locked`. A concurrent `rwlock_cancel(head)` runs
  under G+W; whichever wins the head's resolve CAS performs its mutation, and
  BOTH paths call `rwlock_grant_from_head_locked` to reconcile the new head
  (G-TERM-8 EXPLICIT).

Evidence: `mtx_t22_cancelled_head_then_handoff`, `rwlock_cancel_reconcile_preserves_fifo`.

### 9.7 Stale timer after resource grant

The non-timer winner retires the timer in the same G CS as its resolve CAS,
BEFORE publication. A later stale expiry observes RETIRED in
`pump_deadlines_locked` and is inertly erased (I4 gate). The retired
registration's node may already be destroyed; `erase_popped_registration_locked`
matches by address without reading node/queue.

Evidence: `timer_t10_forced_stale_pump_after_destruction_is_inert`,
`timer_storage_reuse_epoch_isolation`, `timer_old_timer_cannot_resolve_later_epoch`.

### 9.8 Foreign primitive cancellation

`cancel(WaitNode&)` membership gate scans THIS primitive's queue only; a node
in a foreign primitive (same or different Scheduler) returns false WITHOUT
mutation. No foreign `home_` read; no foreign lock.

Evidence: `event_wrong_event_*_loses_safely`, `sem_t24/t25_*`,
`mtx_t8/t9_wrong_mutex_*`, `cond_t31_cancel_wrong_condition_returns_false`,
`rwlock_t13_cancel_foreign_rwlock_false`.

### 9.9 Destruction with active epoch

Every primitive destructor is at most a debug assertion + no-op release. No
destructor resolves, cancels, wakes, or detaches (G-TERM-14). The only
destructor-coordinated drain is the Queue's explicit `QueueTeardownSession`
(not a destructor).

### 9.10 Publication vs WaitNode/context destruction

G-TERM-12. The Scheduler holds `global_mtx_` across publication, which prevents
the resumed Fiber from re-entering a Scheduler seam to destroy its node
mid-publication. The RwLock batch publication caches next/fiber/owner before
route and does NOT dereference the resumed node.

Evidence: `rwlock_batch_publication_does_not_access_published_node`.

---

## 10. Test evidence map

The A/B/hybrid decision: **HYBRID A+B** is the as-built architecture and is
the correct choice.

### 10.1 As-built test architecture

```text
Per-primitive TUs (Method A — primitive-specific resource semantics):
  event_primitive_test              (42 cases)
  semaphore_primitive_test          (31 cases)
  async_mutex_primitive_test        (23 cases)
  async_condition_primitive_test    (32 cases)
  async_queue_primitive_test        (26 cases)
  async_rwlock_test                 (24 cases)
  async_rwlock_death_test           (Category A/B fail-fast)
  async_mutex_death_test            (Category A fail-fast)
  timer_wait_test                   (16 cases — E11 substrate)

Cross-primitive parity TU (Method B — genuinely shared substrate laws):
  async_sync_cross_primitive_parity_test  (7 cases — D3 resource-first + D4 cancel-membership)
```

The cross-primitive parity TU deliberately covers ONLY the surfaces that are
truly shared and that would be erased by per-primitive differences:
- D3 resource-first admission precedence (Event/Semaphore/AsyncMutex; Condition
  covered per-primitive because of its deliberate deadline-first divergence).
- D4 queue-identity cancellation (Event/Semaphore/AsyncMutex; Condition
  per-primitive `cond_t31`; Queue N/A — no public cancel).
- `WaitOutcome` vocabulary distinctness + fresh-`WaitNode` unresolved.

### 10.2 G-TERM law → evidence class map

| Law | Class | Existing evidence |
| --- | --- | --- |
| G-TERM-1 (one terminal winner) | PROVEN BY EXISTING TEST | per-primitive three-way race tests; `parity_waitoutcome_*` |
| G-TERM-2 (winner owns unlink) | PROVEN BY EXISTING TEST | `wait_queue_test`, `wait_queue_resolution_authority_test`, `wait_queue_unlink_topology_test` |
| G-TERM-3 (resource commit before publication) | PROVEN BY EXISTING TEST | `mtx_t5_owner_before_publication_phase`; `rwlock_batch_publication_does_not_access_published_node`; `queue_p4_*` |
| G-TERM-4 (timer lifecycle exactly once) | PROVEN BY EXISTING TEST | `timer_*` suite (T8–T18); `sem_t28_terminal_timed_wait_no_timer_leak` |
| G-TERM-5 (state-before-node) | PROVEN BY EXISTING TEST | `timer_t10_forced_stale_pump_after_destruction_is_inert` |
| G-TERM-6 (admission precedence) | PROVEN BY EXISTING TEST | `parity_d3_*`; `event_set_plus_already_due_deadline_is_woken`; `sem_t14/t15`; `mtx_t11/t12`; `cond_t1_already_due_inline_expired_retains_ownership`; `rwlock_t7/t8/t9`; `queue_h1..h4` |
| G-TERM-7 (queue-identity cancel) | PROVEN BY EXISTING TEST | `parity_d4_*`; `event_wrong_event_*`; `sem_t23/t24/t25`; `mtx_t8/t9`; `cond_t31`; `rwlock_t13` |
| G-TERM-8 (terminal-removal progress) | PARTIALLY COVERED | Semaphore/Mutex/Queue: IMPLICIT, structurally proven by code + `sem_t13`/`mtx_t22`/`queue_g1_*`. RwLock: EXPLICIT, proven by `rwlock_head_writer_cancel_grants_reader_prefix_immediately` + 3 sibling reconcile tests. **No production bug.** |
| G-TERM-9 (FIFO / no-barging) | PROVEN BY EXISTING TEST | `sem_t10/t11/t12`; `mtx_t3/t4/t21`; `cond_t8`; `queue_capacity_and_fifo`; `rwlock_t4/t5/t10/t11` |
| G-TERM-10 (external-thread publication) | PARTIALLY COVERED | Generic resolver families (`event_external_thread_set_wakes_live`, `sem_t26`, `cond_t24`, `mtx_t10`) and RwLock specialized cancellation (`rwlock_head_writer_cancel_grants_reader_prefix_immediately` — calls `AsyncRwLock::cancel` from the main external OS thread while queued Fibers are owned by the Scheduler, then verifies canonical publication and head reconciliation) are covered causally. Queue specialized routing is established by source/authority audit (no dedicated external-thread `close()` runtime test exists). |
| G-TERM-11 (context lifetime) | PROVEN BY EXISTING TEST (ASan/UBSan) | per-primitive ASan/UBSan clean runs; `rwlock_batch_publication_does_not_access_published_node` |
| G-TERM-12 (no post-publication access) | PROVEN BY EXISTING TEST | `rwlock_batch_publication_does_not_access_published_node` |
| G-TERM-13 (accounting ledger) | PARTIALLY COVERED | Guarded decrements prevent numeric underdepth but are not themselves correctness proof. `sem_t27/t28` and `timer_*` exercise key decrement paths; the complete result relies on the path-by-path increment/decrement ledger, terminal-winner ownership, and source audit. No dedicated cross-counter ledger-invariant runtime test exists. |
| G-TERM-14 (destruction is not a resolver) | PROVEN BY EXISTING TEST | `event_destruction_after_terminal_waits_safe`; `mtx_t18_destruction_safe_unlocked_empty`; `cond_t27_safe_destruction_empty`; `rwlock_t0_construction_and_destruction`; `async_rwlock_death_test`; `async_mutex_death_test` |

### 10.3 Optional non-blocking parity additions (NOT required by this audit)

These are NOT findings of a defect. They are optional future additions should
an independent reviewer want a denser cross-primitive net. They are explicitly
out of E12-G's no-new-test scope.

```text
parity_d3_rwlock_admissible_plus_already_due_woken      (AsyncRwLock resource-first)
parity_d3_queue_free_slot_plus_already_due_committed    (AsyncQueue resource-first)
parity_d5_terminal_waiter_leaves_queue_empty            (cross-primitive queue-empty-after-terminal)
parity_d6_terminal_timed_wait_no_timer_leak             (cross-primitive active_deadline_count_==0 after terminal)
```

Rationale for HYBRID A+B: the existing split correctly keeps
primitive-specific resource semantics (which DIFFER — Mutex handoff vs Queue
lease transfer vs RwLock batch grant) in per-primitive TUs, while lifting only
the genuinely uniform substrate laws (resource-first precedence for the
non-Condition primitives; queue-identity cancel membership) into the parity
TU. Adding a single "universal" terminal-parity file would either erase real
differences (Queue's two-completion-dimension result, Condition's
deadline-first divergence, RwLock's explicit reconcile) or duplicate the
per-primitive coverage. The existing architecture is correct.

---

## 11. Formal evidence decision

**Decision: F1 — No new model.** The existing E10/E11 + per-primitive E12
formal models are sufficient for the G-TERM laws. The cross-law → existing
model mapping is below.

### 11.1 Why F1 (and why NOT F2/F3)

- **F2 (one generic terminal-protocol model) is REJECTED.** It would abstract
  wait state / queue membership / timer state / resource commit / publication /
  reconcile into one generic protocol. That abstraction would erase the real
  differences the audit must preserve: Event's broadcast-vs-resource
  distinction, Queue's two-dimension (resolution cause + completion) result,
  AsyncCondition's two-epoch mandatory reacquire, AsyncRwLock's EXPLICIT
  reconcile class. A generic model would be either wrong or vacuous on these.
- **F3 (three archetype models) is PARTIALLY already the as-built.** The
  archetype split the task suggests is essentially what the per-primitive
  models already realize:
  - broadcast/no consumable resource → `e12_event` (4 negatives), `e12_async_condition` (10 negatives).
  - atomic transfer → `e12_semaphore` (7 negatives), `e12_async_mutex` (11 negatives), `e12_queue` (7 negatives).
  - terminal-removal explicit reconcile → `e12_rwlock` (1 negative today).
  Building a NEW F3 layer on top of these would duplicate, not strengthen.
  The right move is to close the per-primitive coverage gaps (notably RwLock's
  thin negative set), not to add a cross-cutting model.

### 11.2 Cross-law → existing formal model mapping

| G-TERM law | Existing formal evidence |
| --- | --- |
| G-TERM-1 (one terminal winner) | `E10WaitNode` (`InvNoDoubleCompletion`); inherited by every E12 model. |
| G-TERM-2 (winner owns unlink) | `E10WaitNode` formalization of the CAS+unlink critical section. |
| G-TERM-3 (resource commit before publication) | `e12_async_mutex` `InvGrantOwnerCommit`/`InvGrantPublicationCoupling`; `e12_queue`: invariant `NoPublishedPendingCompletion`, negative model `E12QueueNegPublishBeforeCommit`; `e12_semaphore` `InvGrantCommitCoupling`. |
| G-TERM-4 (timer lifecycle exactly once) | `e11_timer_wait` `TimerLifetimeClosure`; NEG-4. |
| G-TERM-5 (state-before-node) | `e11_timer_wait` NEG-3 (stale cross-epoch) + NEG-4 (post-destruction). |
| G-TERM-6 (admission precedence) | `e12_semaphore` `InvPermitFirstDeadline` + NEG-7; `e12_async_mutex` admission closure invariants; `e12_async_condition` `InvDueInlineRetainsOwnership`. |
| G-TERM-7 (queue-identity cancel) | DOCUMENT-ONLY (membership gate is structural; per-primitive TUs prove it causally). Not separately modelled — the resolve_ CAS authority already covers single-winner; membership is a structural property. |
| G-TERM-8 (terminal-removal progress) | No dedicated RwLock formal invariant or negative model specifically encodes cancel/expiry head reconciliation. Evidence: causal C++ tests `rwlock_head_writer_cancel_grants_reader_prefix_immediately`, `rwlock_head_writer_expiry_grants_reader_prefix_immediately`, `rwlock_cancel_reconcile_preserves_fifo`, `rwlock_expiry_reconcile_preserves_fifo`; as-built source audit of `rwlock_cancel` / `rwlock_expire_wait` / `rwlock_grant_from_head_locked`. Formal classification: FORMAL-GAP F-G-3, non-blocking. `e12_semaphore` stable-state invariant `InvNoIdlePermitWithEligibleWaiter` (NEG-6). |
| G-TERM-9 (FIFO / no-barging) | `e12_semaphore` `InvFIFOGrant` (NEG-4); `e12_async_mutex` FIFO invariants; `e12_async_condition` `InvNotifyOneFIFO`/`InvNotifyAllSnapshotComplete`; `e12_queue`: invariant `NoBarging`, negative model `E12QueueNegBarging`; `e12_rwlock`: fairness invariant relevant to G-TERM-9 `NoReaderBarging`, negative model `E12RwLockNegReaderBypass`. |
| G-TERM-10 (external-thread publication) | DOCUMENT-ONLY — production-mechanism proof based on the explicit `WorkerState* owner` argument supplied to `route_runnable_locked`. Not separately modelled — the Scheduler publication model (E7/E8/E9) covers exactly-once publication; external-thread destination selection itself is not separately modelled. |
| G-TERM-11/12 (context/post-publication) | `e12_rwlock` `batch_publication_does_not_access_published_node` (runtime); the formal model represents publication as one step. |
| G-TERM-13 (accounting ledger) | `e12_semaphore` `InvPermitConservation`; `e12_queue` lease-conservation invariants; `e11_timer_wait` active-count invariants. |
| G-TERM-14 (destruction not a resolver) | `e12_async_condition` NEG-C7 `DestroyWithActiveWaiters` → `InvDestructionPrecondition`; per-primitive destruction is documented caller-contract. |

### 11.3 Non-blocking formal observation (NOT a blocker)

The RwLock formal model (`docs/spec/e12_rwlock/`) is the newest (landed 2026-07-25)
and thinnest among the E12 set:

```text
e12_rwlock/  : E12RwLock.tla + E12RwLock.cfg + 1 negative (E12RwLockNegReaderBypass)
               no README.md; verify-e12-rwlock-formal.sh is the smallest (3685 bytes)
compared to:
e12_semaphore/   : 7 negatives + README + _gen_neg.py
e12_async_mutex/ : 11 negatives + README + _gen_neg.py
e12_async_condition/: 10 negatives
e12_queue/       : 7 negatives + README + _gen_neg.py
e12_event/       : 4 negatives + README
```

The single RwLock negative (`E12RwLockNegReaderBypass`) covers only the
no-barging/reader-prefix property. The EXPLICIT reconcile class (G-TERM-8
RwLock row) — the property that distinguishes RwLock from every other
primitive — is currently covered only by deterministic C++ tests
(`rwlock_head_writer_cancel_grants_reader_prefix_immediately` and siblings),
not by a formal negative. This is a **FORMAL-GAP, non-blocking**: the C++
evidence is causal and direct, the production behavior is correct, and the
runtime reconcile tests protect the property (no dedicated formal invariant or
negative model encodes the reconcile path). A future RwLock negative
model for the reconcile path (e.g. `E12RwLockNegNoReconcileAfterCancel`) would
bring parity with the sibling models; it is NOT required to close E12-G and is
NOT authorized by this audit. (2026-08-21: this gap is closed by the issue
audit #162 Phase 4 repair — see §11.4; the reconcile path is now encoded by a
dedicated formal negative and five reachability witnesses.)

---

### 11.4 Audit #162 Phase 4 — RwLock model-drift repair and coverage closure (2026-08-21)

The adversarial C++↔TLA+ audit (issue #162) found two high-severity MODEL
defects in `spec/tla/e12_rwlock/E12RwLock.tla`, both confirmed by TLC and now
repaired as a unit (MODEL-001 and MODEL-002 are the same code region; the
repair is single-assignment + guards + anti-revocation + negative controls):

```text
MODEL-001  CancelQueued/ExpireQueued reconcile branches were DEAD CODE: each
           branch primed nodeState'/resolutionCount'/publicationCount' twice
           (top-level and inside the branch), making the conjunction
           unsatisfiable — RW4 was vacuously true for cancel/expire and the
           C++ rwlock_cancel -> rwlock_grant_from_head_locked behavior was
           entirely unmodeled.
MODEL-002  The dead writer-grant branch lacked the C++ admission guard
           `if (active_readers > 0 || writer_active) return;`
           (scheduler_rwlock.cpp:119). Repairing MODEL-001 without adding the
           guard would have modeled a reader-revocation transition the C++ can
           never take.
```

**Repair** (`spec/tla/e12_rwlock/E12RwLock.tla`, `E12RwLock.cfg`):

1. **Single assignment** — every reconcile branch assigns every state variable
   exactly once (no duplicate primed assignments; the CHOOSE/LET structure is
   shared, branches assign disjoint final states).
2. **Writer-grant guard** — both reconcile actions encode `activeReaders = 0`
   (plus `writerOwner = NoWriter`) before granting a head writer; reader-prefix
   grants MERGE into the live reader set (`activeReaders += prefix`), matching
   the C++ batch grant.
3. **Anti-revocation invariant** — new HISTORY ghost `revocationOccurred` and
   invariant `ReaderRevocationFree` (RW10): a granted reader may disappear only
   through its own `UnlockRead`. The ghost is set only by a writer-grant that
   clears a live reader set; the guard makes that unreachable in the positive
   model.
4. **Negative control NEG-RW3** — `E12RwLockNegWriterRevoke.tla` drops the
   `activeReaders = 0` guard in BOTH reconcile actions; `ReaderRevocationFree`
   must FAIL there and only there (all other laws stay intact).
5. **Reachability witnesses (non-vacuity)** — five `E12RwLock.reach-*.cfg`
   reverse invariants prove the repaired branches genuinely fire:
   cancel/expire reader-prefix merge into a live reader set,
   cancel/expire writer-refused head stays blocked (R1/R2 topologies), and the
   writer-blocked contrast that even the dead-reconcile mutant satisfies.

**TLA+ operator-precedence pitfall identified during regeneration** (NOT a
TLC implementation limitation; wording corrected by the PR #168 review): an
unparenthesized `x' = y \/ (S # {})` does NOT express "assign the
disjunction" — `=` binds tighter than `\/`, so the line parses as
`(x' = y) \/ (S # {})`: the second disjunct is a separate successor conjunct
that assigns nothing, and TLC then reports "Successor state is not completely
specified ... variable is not assigned" on exactly the successors that
disjunct enables. The intended boolean disjunction would need
`x' = (y \/ (S # {}))`; the model instead uses the equivalent IF-form
`x' = IF S = {} THEN y ELSE TRUE` — semantically identical, unambiguous
under precedence, and the assignment idiom already used everywhere in this
model (e.g. nodeState'). The tracked generator
`scripts/formal/gen-rwlock-neg-writer-revoke.py` encodes this law and FAILS
CLOSED if the source ever regresses to the unsafe form; exact full-line
substitutions (never a regex over `*` or `-`) keep the `\*` comment lines and
the arithmetic `-` operators untouched, and a round-trip check proves the
derived file is the exact inverse of the source.

**Evidence (TLC 2.19 / tla2tools v1.7.4, Java 25, 4 workers, isolated
mktemp workspace; `scripts/formal/verify-async-rwlock.sh` exit 0):**

```text
PASS  E12RwLock [12 invariants incl. ReaderRevocationFree]
      1615 states generated, 1030 distinct, depth 7
CEX   NEG ReaderBypass  (NoReaderBarging violated, as expected)
CEX   NEG NoReconcile   (InvNoStrandedGrantableHead violated, as expected)
CEX   NEG WriterRevoke  (ReaderRevocationFree violated, as expected)   [NEG-RW3]
CEX   REACH cancel reader-prefix merge     (NoReachCancelReaderPrefixMerge violated)
CEX   REACH expire reader-prefix merge     (NoReachExpireReaderPrefixMerge violated)
CEX   REACH cancel writer-refused (R1)     (NoReachCancelWriterRefused violated)
CEX   REACH expire writer-refused (R2)     (NoReachExpireWriterRefused violated)
CEX   REACH writer-blocked contrast        (NoReachWriterBlockedByReaders violated)
```

**Deterministic C++ counter-evidence** (issue #162 Phase 2, R1–R5 race
classes; `tests/async_rwlock_test.cpp`, exact `SLUICE_TEST_FILTER` runs,
all PASS): `rwlock_audit_r1_cancel_head_writer_wall`,
`rwlock_audit_r2_expire_head_writer_wall`,
`rwlock_audit_r3_writer_owner_cancel_reader_head`,
`rwlock_audit_r4_cancel_exposes_writer_at_zero`,
`rwlock_audit_r5_cancel_wins_over_late_expiry`,
`rwlock_audit_r5_expiry_wins_cancel_returns_false`,
`rwlock_audit_r5_grant_wins_cancel_returns_false`,
`rwlock_audit_r5_cancel_wins_grant_is_noop`,
`rwlock_audit_m3_write_lock_until_resource_first`,
`rwlock_audit_m3_write_lock_until_due_blocked_expires`.
Full Clang Debug gate: 189/189 tests pass.

**Manifest / gate wiring**: `spec/tla/manifest.json` e12-rwlock suite:
negative_gate_count 2 → 3, reachability_gate_count 0 → 5;
`scripts/formal/verify-async-rwlock.sh` runs all 9 gates with named-verdict
checks; `python3 scripts/formal/verify.py check` PASS.

### 11.5 Audit #162 Phase 5 — timed-admission modeling parity, MODEL-003 closure (2026-08-21)

Finding MODEL-003 (issue #162): `E12RwLock` was the only E12 sync-primitive
suite without timed-admission modeling — the `*_lock_until`
resource-first-vs-already-due precedence
(`src/async/scheduler_rwlock.cpp` `rwlock_read_lock_until` /
`rwlock_write_lock_until`, precedence 1 claim at :514/:602 before precedence 2
due-expiry at :531/:615) was entirely absent, while `E12Semaphore`
(`deadlineDue` + `InvPermitFirstDeadline`, P7) and `E12AsyncMutex`
(`LockUntil*` family + evidence latches, M7) both model it.

**Repair** (`spec/tla/e12_rwlock/E12RwLock.tla`, `E12RwLock.cfg`), at sibling
parity and in the suite's one-atomic-admission-step idiom:

1. **Ghosts** — `deadlineDue[e]` is the environment-chosen admission input
   (`\E due \in BOOLEAN` in the two `*UntilAdmit` actions, so BOTH precedence
   halves are reachable); `admissionSawResource[e]` / `admissionSawDue[e]`
   are evidence latches set atomically with the resolution, making the
   precedence invariant a prime-free state predicate (P7/M7 pattern).
2. **Six timed actions** — `ReadUntilAdmit` / `WriteUntilAdmit` (resource
   admissible: fresh node at queue head — model `Len(queue)=0` pre-state —
   plus the read/write admissibility guards; WINS over a due deadline),
   `ReadUntilExpired` / `WriteUntilExpired` (not admissible + already due →
   Expired at admission, no runnable publication, never queue-visible), and
   `ReadUntilSuspend` / `WriteUntilSuspend` (not admissible + not due →
   park). The transient timer registration is invisible at this abstraction
   (created and retired inside the same atomic step).
3. **RW11** — `InvResourceFirstDeadline`: an admission that saw an
   admissible resource AND an already-due deadline must resolve `Woken`. The
   Expired-at-admission actions latch `admissionSawResource = FALSE`, so a
   precedence inversion violates it.
4. **Two reachability witnesses** — `E12RwLock.reach-until-resource-beat-due.cfg`
   (precedence 1 fires with `due = TRUE`) and
   `E12RwLock.reach-until-expired.cfg` (precedence 2 fires inline).

The four plain admission actions latch evidence too (`sawResource = TRUE` for
immediate admits), so RW11 constrains the whole admission family uniformly.

**Evidence (TLC 2.19 / tla2tools v1.7.4, isolated mktemp workspace;
`scripts/formal/verify-async-rwlock.sh` exit 0):**

```text
PASS  E12RwLock [13 invariants incl. InvResourceFirstDeadline]
      7321 states generated, 3781 distinct, 0 left on queue
CEX   NEG ReaderBypass  (NoReaderBarging violated, as expected)
CEX   NEG NoReconcile   (InvNoStrandedGrantableHead violated, as expected)
CEX   NEG WriterRevoke  (ReaderRevocationFree violated, as expected)   [NEG-RW3]
CEX   REACH cancel reader-prefix merge     (NoReachCancelReaderPrefixMerge violated)
CEX   REACH expire reader-prefix merge     (NoReachExpireReaderPrefixMerge violated)
CEX   REACH cancel writer-refused (R1)     (NoReachCancelWriterRefused violated)
CEX   REACH expire writer-refused (R2)     (NoReachExpireWriterRefused violated)
CEX   REACH writer-blocked contrast        (NoReachWriterBlockedByReaders violated)
CEX   REACH until resource-beat-due        (NoReachUntilResourceBeatDue violated)
CEX   REACH until admission-expire         (NoReachUntilExpired violated)
```

**Deterministic C++ counter-evidence** (issue #162 Phase 2, exact
`SLUICE_TEST_FILTER` runs, both PASS, added in Phase 2):
`rwlock_audit_m3_write_lock_until_resource_first`,
`rwlock_audit_m3_write_lock_until_due_blocked_expires` — the C++ precedence
was already as-built-correct; MODEL-003 was pure model drift, so this phase
is model+gates only (no production change, per the C++-FIRST protocol).

**Generator compatibility**: the MODEL-002 negative control
`E12RwLockNegWriterRevoke.tla` is regenerated from the extended source by
`scripts/formal/gen-rwlock-neg-writer-revoke.py`; all fail-closed anchors
held (exact guard lines ×2, IF-form ghost ×2, module header), the round-trip
inverse proof passed, and the regenerated negative still violates exactly
`ReaderRevocationFree`.

**Manifest / gate wiring**: `spec/tla/manifest.json` e12-rwlock suite:
reachability_gate_count 5 → 7; `scripts/formal/verify-async-rwlock.sh` runs
all 11 gates (positive 13-invariant + 3 negatives + 7 witnesses);
`python3 scripts/formal/verify.py check` PASS.

### 11.6 Audit #162 Phase 6 — remaining model findings: explicit verdicts (2026-08-21)

Per-item dispositions for MODEL-004..MODEL-009 (issue #162 §4). Audit scope
decision: **no auto model building** — every item receives an explicit
verdict (repaired claim, documented divergence, justified gap, or INFO-keep)
instead. Gates re-run green after the edits:
`scripts/formal/verify-e10-waitnode.sh`, `verify-e7-publication.sh`,
`verify-e8-ownership-transfer.sh` — all PASS with their named negatives.

| ID | Verdict | Disposition |
| --- | --- | --- |
| MODEL-004 (MEDIUM) | STALE CLAIM REPAIRED (doc-in-model) | `E10WaitNode.tla` header now DECLARES the E10/E11 resolver split: production has THREE CAS-competing terminal resolvers (woken / cancelled / expired, `wait_node.hpp:81-99` + `resolve_` at `:241-251`) plus the node-targeted `wake_node_locked`; E10 deliberately instantiates the single-winner law for the two-resolver core, and the `expired` resolver + its timer races are owned by the E11 suite. The winner-CAS law is resolver-count-agnostic (Registered → terminal under one CAS), so a third E10 resolver would duplicate E11 coverage without a new race class. |
| MODEL-005 (LOW) | DIVERGENCE DOCUMENTED; C++ deadness CONFIRMED with per-member precision, removal re-tracked (issue #170) | Verified this round (rg survey re-verified by the PR #168 review, @ `15a80ba`): `WorkerState::inbox` (deque) has ZERO push/pop repo-wide — **unused storage** carrying no ticket; `inbox_cv` has ZERO production waiters (no `wait/wait_for/wait_until`) — its notify sites (`scheduler.cpp:288…1959`) are **notify-only / semantically inert transport**, NOT a zero-ref dead declaration; `inbox_mtx` is **LIVE** (34 refs — it serializes cross-worker publication into the single `local_runnable` queue, the actual production runnable queue, 42 refs). The E7 artifact states the model-side fact precisely: `W*Inbox`/`MoveInboxToLocal` are **UNREACHABLE compatibility states** — Init and every producer action assign only `PendingSpawn`/`W*Local`, so the checked graph never enters the Inbox tier and the E7 gate does NOT exercise that hop (`E7Publication.tla` header + the README refinement-map row). C++ dead-field removal (deque + CV + their no-op notifies; the live mutex stays) is re-tracked as issue #170: it is a wake-path-wide mechanical change requiring its own review and §16.3 TSan, out of this audit's minimal boundary. |
| MODEL-006 (LOW/INFO) | KEEP (self-documented) | E9's 1-bit wakeEpoch ABA limitation is documented inside the model; persistent state is the return authority. No action. |
| MODEL-007 (MEDIUM, aggregate) | JUSTIFIED COVERAGE GAP + TRIGGERS RECORDED | The five mechanisms (I47-F2 suspend-switch/steal window; Phase-F1 WaitRecord registry races; CancelToken epoch protocol; #115 spawn-to-busy-worker wake obligation; G1 retire-ring ticket rescue) are coverage gaps, not defects. Recorded in `docs/verification/formal/cpp-model-coverage.md` debt register with per-mechanism triggers. Issue #162 §7.3's two suggested focused models are deliberately NOT auto-built (scope decision); taking them up is the revisit trigger. |
| MODEL-008 (INFO) | KEEP (documented) | `RecordCanceledConfirmed` has no production caller on master (ThreadPool EINTR retry keeps running requests uninterruptible); already documented in the model. No action. |
| MODEL-009 (LOW) | REFINEMENT/DOCUMENTATION DRIFT REPAIRED by NARROWING the model claim (PR #168 review); NOT tracked as formal debt | `E8OwnershipTransfer.tla` covers the registration-time-owner family ONLY: Completion waits route by `WaitReg.owner` — exactly what `WakeReady` models. The WaitQueue-class wake discipline (the E12 primitives resolve the target via `fiber_owner_`, the CURRENT owner updated by steal) is **not instantiated by any E8 action**, and worker liveness / G1 retire-ring rescue are outside this model's state machine — so this suite proves NOTHING about that family (the earlier "no E8 invariant distinguishes them" wording followed from omitting the second behavior, not from comparing both). The finding was drift of the model's own documentation, repaired by the narrowed claim itself: the current-owner family is recorded as a documentation-level coverage boundary (implementation-level safety argument, separate from the gate), deliberately NOT re-tracked as formal debt — the issue #171 umbrella owns the MODEL-007 unmodeled-mechanism list, which does not include this routing family. |

### 11.7 Audit #162 Phase 7 — C++ observations CPP-001/CPP-002: verdicts and repair (2026-08-21)

Issue #162 §6 recorded two LOW C++ observations "not confirmed bugs, review
only". This phase investigated both to explicit verdicts and repaired them
(the issue's §7.5 recommendation: incidental cleanup through the normal
review gates). **No reachable behavior changes** — both repairs are
no-op-removal plus violation-state fail-fast, proven below.

**CPP-001 (owner-lookup discipline unification) — CONFIRMED, REPAIRED.**

The rwlock cancel/expire/grant publication sites resolved the target worker
with `fiber_owner_.find` + a `g_worker` tolerant fallback, while every other
primitive (semaphore, condition, timer, event, queue, mutex, park-wake)
routes through the I47-F1 authoritative `publish_waiting_fiber_runnable_locked`
/ `owner_for_fiber_locked` helper, which FAIL-FASTS
(`scheduler_missing_fiber_owner_fail_fast`) on a missing record.

Proof the fallback was not load-bearing: `fiber_owner_` is written at six
sites (`scheduler.cpp:271, 322, 391, 537, 1564, 1954` — spawn, distribute,
retire-ring rescue, re-route, steal) and **never erased** (zero erase sites
repo-wide), so a Fiber that has run and entered Waiting ALWAYS has a record.
A miss is exactly the Scheduler invariant violation the authoritative path
fail-fasts on; the rwlock fallback silently routed around it. Repair: all
four lookup sites across the three rwlock publication regions
(`rwlock_grant_from_head_locked` writer + reader-prefix loop,
`rwlock_cancel` capture, `rwlock_expire_wait` capture) now call
`owner_for_fiber_locked` under the already-held `global_mtx_`. The null-fiber
tolerance order is preserved exactly (the lookup happens only inside the
`fib != nullptr` / `cancel_fiber != nullptr` guards the old code already
used to skip the route).

**CPP-002 (dead `make_runnable` on the current Running fiber) — CONFIRMED,
REPAIRED, class widened.**

`Fiber::make_runnable` is a documented no-op from `running`
(`fiber.hpp:89-90` "No-op from runnable/running"), and every inline
admission-resolution path runs BEFORE `commit_suspend_locked` / `make_waiting`,
so `me` is always the current RUNNING fiber there: the call could only return
false, and discarding it achieved nothing. Issue #162 listed five sites; the
full class is **thirteen** plus one doubly-dead conditional: mutex ×3,
rwlock ×2, semaphore ×3, event ×3, condition ×1, and
`scheduler_timer.cpp`'s `if (me->make_runnable()) route_runnable_locked(...)`
whose condition is always false, so its route branch NEVER executed. All
removed; the touched comments now state the actual invariant (the Fiber is
RUNNING, continues inline, and needs no runnable publication — publishing
one would violate E7 exactly-once). Two pre-existing comments that
rationalized the call ("make_runnable may return false. That is not a
reason to publish it"; "Call it for state consistency") are corrected —
there was no state effect to preserve.

**Focused architecture-compliance note** (AGENTS.md §8; this change touches
scheduler wake/publication code paths, so the phase-specific gate is recorded
here against the generic gate): state machines — unchanged (no transition
added/removed); wake/publication protocol — unchanged in every reachable
state (removed calls were provable no-ops; the owner value is identical in
every reachable state, so routing targets are unchanged); lock/atomic
domains — unchanged (`owner_for_fiber_locked` requires `global_mtx_`, held
at every converted site; no new lock order); capacities/resources —
unchanged; shutdown/drain — unchanged; formal models — unaffected (E7
publication exactly-once and E8 current-owner routing are precisely what
the code still does; no modeled state transition changed, so no model edit
is required under the §17 binding).

**Evidence**: baseline Clang Debug 189/189 PASS before the edits; Clang
Debug 189/189 PASS after; Clang TSan full suite ALL TESTS PASSED after
(§16.3 change class). Inline-admission semantics are covered by the
existing deterministic suite (including `rwlock_audit_m3_*`,
semaphore/mutex/event `*_until` admission cases and the wait-queue race
matrices).

### 11.8 PR #168 review closeout — verification-strength and scope-narrowing repairs (2026-08-21)

The PR #168 adversarial review (our own + CodeRabbit) found that the
Phase 4/5/6 evidence, while green, overstated its strength in three places
and left two generated-negative discipline gaps. All repairs are
formal/docs/test-only: **no production C++ semantics changed** in this
round.

1. **MODEL-003 sensitivity negative control (NEG-RW4)** — the two timed
   admission reachability witnesses (§11.5) proved both correct paths
   REACHABLE, but nothing proved `InvResourceFirstDeadline` would FAIL if
   the precedence were wrong. New generated negative
   `E12RwLockNegDeadlinePrecedence.tla`
   (`scripts/formal/gen-rwlock-neg-deadline-precedence.py`): in BOTH
   `ReadUntilAdmit` and `WriteUntilAdmit` the disposition is split on the
   environment's due bit — the `due = FALSE` successor is EXACTLY the
   positive behavior, while `due = TRUE` (resource admissible AND deadline
   already due) wrongly resolves `Expired` and commits NO ownership (no
   reader grant, no `activeReaders` increment, no `writerOwner` install).
   Narrowed after the PR #168 adversarial review: the first draft flipped
   the outcome unconditionally and kept the ownership commits, which would
   have made the mutant also violate `WriterOwnerConsistency` (an Expired
   node still owning the writer lock) — i.e. a BROADER broken model, not an
   isolated precedence mutation. The evidence latches are untouched
   (`admissionSawResource' = TRUE`; the `\E due` latch), so the mutant
   cannot make the invariant self-proving by erasing evidence. The
   derivation hoists the common assignments before the split and makes the
   IF the action's last conjunct, so both successors assign every variable
   exactly once under any reading of TLA+ quantifier/IF scope. Parity:
   `E12SemNeg7DeadlinePrecedence` (NEG-SEM-7). Observed verdicts:
   `InvResourceFirstDeadline` VIOLATED (named CEX) and, on the SAME mutant,
   the remaining 12 positive invariants PASS — the new
   `E12RwLockNegDeadlinePrecedence.specificity.cfg` gate proves the
   negative is EXACT (fails for the deadline-precedence defect and nothing
   else). Adversarially probed in an isolated workspace: re-broadening the
   write-path defect branch to install `writerOwner' = e` makes the
   specificity gate FAIL on `WriterOwnerConsistency`, so the specificity
   check genuinely detects collateral damage.
2. **Generated-negative freshness gates (fail-closed)** —
   `verify-async-rwlock.sh` previously ran TLC on the committed
   `E12RwLockNegWriterRevoke.tla` without proving it equals what the
   CURRENT positive model would generate (CodeRabbit finding; accepted).
   Both generators now have a `--check` mode (byte-compare against the
   committed negative, repository untouched, stale -> non-zero with
   "generated negative is stale; regenerate it"), and the verifier runs
   both checks BEFORE any TLC execution. Adversarially probed: a positive
   model edit that flows into a negative (anchor drift or carried-content
   drift) turns the gate RED before TLC launches.
3. **TLA+ precedence wording (§11.4 above)** — the "TLC 2.19 constraint"
   mislabel is corrected everywhere it appeared (generator docstring,
   closeout §11.4, manifest notes): the `x' = y \/ c` failure is TLA+
   operator-precedence parsing (`=` binds tighter than `\/`), not a TLC
   implementation limitation.
4. **E7 Inbox-tier claim narrowed (CodeRabbit finding; verified true)** —
   no `Init` value and no producer action ever assigns `W0Inbox`/`W1Inbox`,
   so `MoveInboxToLocal` is UNREACHABLE in the checked graph: the E7 gate
   does not exercise that hop and never proved anything about it. The
   model header, the action comment, and the README refinement-map row now
   say exactly that (unreachable compatibility states of the never-built
   staged design); no producer transition was added (that would widen
   model scope to defend old prose). Production publication uses the
   direct `local_runnable` path.
5. **MODEL-005 wording precision (CodeRabbit finding; corrected, not
   blindly adopted)** — the four members are now described individually
   (§11.6 row): `inbox` = unused storage; `inbox_cv` = notify-only /
   no production waiter (NOT a zero-ref dead declaration — it HAS notify
   call sites); `inbox_mtx` = LIVE lock; `local_runnable` = the production
   queue. Removal re-tracked as issue #170 (needs wake-path review + TSan;
   NOT done in this PR).
6. **E8 wake-routing claim narrowed (CodeRabbit finding; accepted)** —
   E8 models only the registration-time-owner family; the
   current-owner (`fiber_owner_`) WaitQueue routing, worker liveness, and
   the G1 retire-ring rescue are not in its state machine, so the model
   proves nothing about them. The "no E8 invariant distinguishes them"
   sentence is gone (it followed from omitting the second behavior). The
   finding was refinement/documentation drift repaired by the narrowed
   claim itself; the current-owner family stays a documentation-level
   coverage boundary and is deliberately NOT re-tracked as formal debt
   (the #171 umbrella owns the MODEL-007 mechanism list only — a different
   list, so #171's scope is not widened to absorb MODEL-009).
7. **Markdown lint** — two line-leading `#162` lines (this file, and
   `docs/verification/formal/migration-report.md`) that a Markdown parser
   would read as headings were rewritten as prose.
8. **Deterministic-test hygiene** — the six `std::this_thread::yield()`
   calls inside the `rwlock_audit_r2_*`/`rwlock_audit_r5_*` driver-fiber
   loops were removed: under `sched.run(1)` they are OS-thread scheduling
   hints, not Fiber interleaving authority — the causal seams are
   `await_ready_flag`, `advance_clock` (whose pump resolves the node
   inline under `global_mtx_`), run phases, and terminal-state assertions.
   The review's second pass also collapsed the arbitrary retry loops
   (`200 && !terminal` / `5×`) to a SINGLE `advance_clock(100)` per driver:
   if the inline-pump argument is right, one advance from clock 0 past the
   deadline is the complete causal evidence — a loop would only mask a
   falsified assumption. All ten audit cases re-ran green after both
   removals.
9. **#162 residual re-tracking** — E12AsyncMutexNegM4 generator staleness
   (pre-existing, NOT a #162 rwlock defect) -> issue #169; E7 C++
   dead/inert field cleanup -> issue #170; MODEL-007's five unmodeled
   mechanisms -> umbrella issue #171 (details stay in the
   `cpp-model-coverage.md` debt register with triggers).
10. **CodeRabbit "docstring coverage 42.31%" advisory — NOT ACTIONED**:
    not a repository required gate; bulk docstrings would add comment
    noise against the repository's comment-simplification direction, and
    the finding is outside #162's correctness scope.

**Evidence (`scripts/formal/verify-async-rwlock.sh`, TLC 2.19 / tla2tools
v1.7.4, isolated mktemp workspace, exit 0):**

```text
fresh  gen-rwlock-neg-writer-revoke.py --check       (byte-identical)
fresh  gen-rwlock-neg-deadline-precedence.py --check (byte-identical)
PASS   E12RwLock [13 invariants]  7321 states, 3781 distinct
CEX    NEG ReaderBypass        (NoReaderBarging violated)
CEX    NEG NoReconcile         (InvNoStrandedGrantableHead violated)
CEX    NEG WriterRevoke        (ReaderRevocationFree violated)        [NEG-RW3]
CEX    NEG DeadlinePrecedence  (InvResourceFirstDeadline violated)    [NEG-RW4]
PASS   NEG DeadlinePrecedence specificity — the 12 remaining positive
       invariants hold on the SAME mutant (3748 states, full exploration)
CEX    REACH × 7               (all seven NoReach* witnesses)
```

`verify-e7-publication.sh` / `verify-e8-ownership-transfer.sh` /
`verify-e10-waitnode.sh` re-ran PASS after the comment edits;
`python3 scripts/formal/verify.py check` PASS (95 .tla / 241 .cfg all
manifest-covered). Manifest: e12-rwlock `negative_gate_count` 3 -> 5
(4 violation gates + 1 specificity pass gate across 4 negative models);
gate structure is now positive(1) + negative(5) + reachability(7).

---

## 12. Findings ledger

| ID | Primitive / shared | Class | Severity | Evidence | Required action |
| --- | --- | --- | --- | --- | --- |
| F-G-1 | RwLock vs preparation matrix | DOC-DRIFT | LOW | `e12-sync-primitives-plan.md` §10.1 row still lists RwLock "1–2 (readers, writers)" queues, "read permit pre-increment/refund", and "upgrade state" as live candidates. As-built (E12-F) is ONE unified FIFO queue, commits `active_readers_` only at claim (no per-node refund), and DEFERs upgrade. | Minimal status/link update to `e12-sync-primitives-plan.md` §10 (authorized by this task): declare §10 a preparation baseline, point at this audit, mark the RwLock row obsolete. Do NOT rewrite §10. |
| F-G-2 | Condition admission precedence | DOC-DRIFT (documented divergence) | LOW | Condition uses deadline-FIRST admission (C-H4); all other primitives use resource-first. Already documented in `api-reference.md` line 769 and `e12-condition.md` C-H4, but the `e12-sync-primitives-plan.md` §10.2 matrix and §7 do not surface the divergence as a parity property. | None beyond recording it in this audit (done in §5.2, G-TERM-6). It is a deliberate, documented, tested divergence, NOT a parity failure. |
| F-G-3 | RwLock formal model coverage | FORMAL-GAP | LOW | `docs/spec/e12_rwlock/` has only 1 negative (reader-bypass); the EXPLICIT reconcile class is formal-implicit (covered by liveness + runtime tests only). | Non-blocking. Future F1+ work may add `E12RwLockNegNoReconcileAfterCancel` for parity. NOT required to close E12-G; NOT authorized by this audit. |
| F-G-4 | Cross-primitive parity test coverage | TEST-GAP (non-blocking) | LOW | `async_sync_cross_primitive_parity_test` covers D3 (Event/Sem/mtx) and D4 (Event/Sem/mtx). RwLock and Queue resource-first admission precedence and a few cross-law ledger assertions are covered only per-primitive. | Non-blocking. Optional future Method-B additions listed in §10.3. NOT required to close E12-G; NOT authorized by this audit. |
| F-G-5 | Queue external cancellation | DEFERRED-BY-DESIGN | N/A | Queue v1 has no public cancel API; P8/C7 reserved. | None. NOT a parity failure (G-TERM-7). Do NOT add a Queue cancel API under E12-G. |
| F-G-6 | Event / Semaphore / AsyncMutex / AsyncCondition / AsyncQueue / AsyncRwLock (production behavior) | PASS | N/A | Every G-TERM law is mapped to an as-built code site and an explicit evidence class (runtime test, death test, formal invariant/negative, or contract audit). No authority corruption, no public-API conflict, no Scheduler defect. | None. |

**No PRODUCTION-BUG finding.** No API-CONTRACT-CONFLICT finding. The only
non-PASS findings are DOC-DRIFT (F-G-1, F-G-2), FORMAL-GAP (F-G-3, non-blocking),
TEST-GAP (F-G-4, non-blocking), and DEFERRED-BY-DESIGN (F-G-5).

Per the task's finding rules:
- preparation-vs-as-built drift → DOC-DRIFT (F-G-1), not a production bug.
- Queue no external cancel → DEFERRED-BY-DESIGN (F-G-5), not a parity failure.
- thin RwLock formal model → FORMAL-GAP (F-G-3), non-blocking.
- no test for a code-correct property → TEST-GAP (F-G-4), non-blocking.
- no reachable-state/race evidence of a production defect → NO PRODUCTION-BUG.

---

## 13. Corrective implementation boundaries

**None authorized.** E12-G-PRODUCTION-CORRECTIVE: NOT REQUIRED.

If an independent reviewer disputes any G-TERM-8 IMPLICIT claim (Semaphore /
AsyncMutex / Queue), the smallest possible corrective boundary would be:
- a deterministic regression test (Method A, per-primitive TU) that forces the
  specific head-cancel-then-successor-admissible topology and asserts the
  successor is not stranded;
- NOT a production change (the production behavior is already correct by the
  state arguments in §6 G-TERM-8 and §7);
- NOT a shared private helper (no defect is shared across primitives).

If an independent reviewer disputes the RwLock EXPLICIT reconcile, the
corrective boundary would be:
- additional deterministic reconcile tests (already 4 exist) OR a formal
  negative model (F-G-3);
- NOT a production change (`rwlock_cancel` / `rwlock_expire_wait` already call
  `rwlock_grant_from_head_locked` mandatorily).

No public API change is proposed or contemplated.

---

## 14. Authorization checklist

This audit asserts ONLY:

```text
[x] design + as-built documentation (this file)
[x] minimal status/link update to docs/e12-sync-primitives-plan.md §10
```

This audit does NOT authorize, propose, or require:

```text
[ ] any production implementation change
[ ] any test addition or modification
[ ] any TLA+ model addition or modification
[ ] any public API change
[ ] any Scheduler refactor
[ ] any shared private helper (no shared defect found)
[ ] any commit, push, or PR
[ ] any reopening of E10/E11/E12-A..F closed semantics
[ ] any reopening of the Condition Model A/B decision (Model A is CLOSED)
[ ] any Queue external-cancellation API
[ ] any unified CancelToken / Waitable / Lockable / Grant framework
```

---

## 15. Final verdict

```text
E12-G-DESIGN: COMPLETE
E12-G-AS-BUILT-AUDIT: PASS
E12-G-PRODUCTION-CORRECTIVE: NOT REQUIRED
E12-G-TEST-CORRECTIVE: NOT REQUIRED  (optional non-blocking Method-B additions identified in §10.3)
E12-G-FORMAL-CORRECTIVE: NOT REQUIRED (F1 chosen; RwLock negative-model parity gap F-G-3 is non-blocking)
E12-G-CLOSEOUT: READY FOR INDEPENDENT RE-REVIEW
```

The E12 primitive set (Event / Semaphore / AsyncMutex / AsyncCondition /
AsyncQueue / AsyncRwLock), built on the E10 WaitNode/WaitQueue and E11
TimerRegistration substrate, satisfies the G-TERM-1..14 cross-primitive
terminal-resolution laws **as built**. Every law has a code site
(`file:line`) and an explicit evidence class (runtime test, death test, formal
invariant/negative, or contract audit). The single deliberate, documented
divergence (Condition deadline-first admission, C-H4) has explicit authority
and a parity test. The single EXPLICIT reconcile class (RwLock cancel/expiry
head-reconcile) is implemented mandatorily and tested causally.

The only non-PASS findings are documentation drift (F-G-1, F-G-2), a
non-blocking formal-coverage gap on the newest primitive (F-G-3), a
non-blocking cross-primitive test-coverage gap (F-G-4), and a deferred-by-
design feature (F-G-5, Queue external cancel). None of these authorizes
production change under E12-G.

---

## 16. Cross-links

- Preparation baseline (corrected by §5): [`docs/history/implementation-plans/e12-sync-primitives-plan.md`](docs/history/implementation-plans/e12-sync-primitives-plan.md) §10
- E10 as-built: [`docs/history/closeout/e10-waitnode-wait-queue.md`](e10-waitnode-wait-queue.md)
- E11 as-built: [`docs/history/closeout/e11-deadline-timer-wait.md`](e11-deadline-timer-wait.md)
- E12-A Event: [`docs/history/closeout/e12-event.md`](e12-event.md)
- E12-B Semaphore: [`docs/history/closeout/e12-semaphore.md`](e12-semaphore.md)
- E12-C AsyncMutex: [`docs/history/closeout/e12-async-mutex.md`](e12-async-mutex.md)
- E12-D AsyncCondition: [`docs/history/closeout/e12-condition.md`](e12-condition.md)
- E12-E AsyncQueue: [`docs/history/closeout/e12-queue.md`](e12-queue.md), [`docs/history/implementation-plans/e12-queue-state-machine.md`](docs/history/implementation-plans/e12-queue-state-machine.md), [`docs/history/implementation-plans/e12-queue-scheduler-integration.md`](docs/history/implementation-plans/e12-queue-scheduler-integration.md)
- E12-F AsyncRwLock: [`docs/history/implementation-plans/e12-rwlock.md`](docs/history/implementation-plans/e12-rwlock.md)
- Public API reference: [`docs/api-reference.md`](api-reference.md)
- Construction method (M1–M9): [`docs/history/implementation-plans/async-runtime-construction-method.md`](docs/history/implementation-plans/async-runtime-construction-method.md)
- Prior cross-primitive semantic-closure review: [`docs/history/reviews/E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1.md`](docs/history/reviews/E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1.md)
- Formal models: `docs/spec/e10_waitnode/`, `docs/spec/e11_timer_wait/`, `docs/spec/e12_event/`, `docs/spec/e12_semaphore/`, `docs/spec/e12_async_mutex/`, `docs/spec/e12_async_condition/`, `docs/spec/e12_queue/`, `docs/spec/e12_rwlock/`
- Formal gates: `scripts/formal/verify-*.sh`
- Cross-primitive parity tests: `tests/async_sync_cross_primitive_parity_test.cpp`
