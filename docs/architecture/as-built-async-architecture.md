# As-Built Async Architecture

**Authority:** This document describes what the code *actually does* at commit
`b20bcc7` (master, including PR #60 and PR #61). Where ADRs and implementation
disagree, this document records the implementation truth and flags the
discrepancy.

**Target-design boundary:**
[ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md)
is Proposed. Its `RequestKey`, bounded `RequestSlot`, identity-bearing reap,
capacity, cancel disposition, and close-admission lifecycle are **not** present
in this as-built baseline.

---

## 1. Component Topology

```text
ApplicationRuntime (E16)
├── AsyncIoContext (L1, move-only, owns backend)
│   ├── access_mtx_ (serialized backend access domain)
│   └── AsyncBackend (L0, virtual, injected)
│       ├── ThreadPoolBackend   (per-op thread, portable fallback)
│       ├── UringAsyncBackend   (gated, liburing)
│       ├── FakeAsyncBackend    (deterministic test vehicle)
│       └── SyncBackend         (synthetic/reference; completes at poll time)
├── Scheduler (E7–E13, borrows AsyncIoContext)
│   ├── global_mtx_ (coordination domain)
│   ├── WorkerState[] (per-worker execution state)
│   ├── wake_cv_ / wake_mtx_ / wake_epoch_ (E9 park/wake)
│   ├── waiting_completion_ map (Completion-backed waits)
│   ├── waiting_ready_ map (persistent-readiness waits)
│   ├── WaitQueue / WaitNode (E10 cancellation-safe waits)
│   ├── TimerRegistration heap (E11 deadlines)
│   └── E12 primitives (Event, Semaphore, Mutex, Condition, Queue, RwLock)
├── Group (root task domain, borrows Scheduler)
│   ├── CancelToken (root cancel authority)
│   └── Fiber[] + stacks (Evented mode)
├── SchedulerWakeHandle (external wake capability)
├── driver_thread_ (single dedicated thread)
└── lifecycle_mtx_ / runtime_cv_ / state machine
```

---

## 2. L1 I/O Submission Path (per backend)

### 2.1 Common entry

```text
caller
→ AsyncIoContext::submit_*(op, Completion&)
    → lock access_mtx_
    → backend_->submit_*(op, c)
        → backend calls AsyncBackend::try_claim(c)  ← BACKEND is claim authority (CAS idle → outstanding)
        → on claim failure: return synchronous invalid_state (no tracking mutation)
        → backend records op (returns Result<void>)
    → tally_submit(stats_, result)
    → update_max_outstanding(stats_)
    → unlock access_mtx_
→ return Result<void>
```

**Authority note (ADR-explicit-io-completion-authority):** `AsyncIoContext` does
NOT claim the Completion and does NOT check idle state itself. It only serializes
backend access (access_mtx_), forwards the call, and tallies statistics. The
backend is the explicit claim authority via the protected `try_claim()` helper
(atomic `idle → outstanding` CAS); claim failure returns synchronous
`invalid_state` without touching tracking or the outstanding counter. The
previously-stale header comment on `AsyncBackend::submit_read` (P1-01) was
corrected by PR #61 to name the backend as the claim authority.

### 2.2 ThreadPoolBackend

```text
submit_*(op, c)
→ accepting_new_work()? (mtx_ guarded destroying_ flag)
→ validate/normalize operation parameters
→ try_claim(c)                        ← BACKEND claim authority (CAS; failure is invalid_state)
→ enqueue_size/void(c, work_lambda)   ← claim already won
    → lock mtx_
    → ++outstanding_
    → workers_.emplace_back(worker_lambda)
        worker_lambda:
            → execute blocking pread/pwrite/fdatasync/fsync
            → lock mtx_
            → ready_size_/ready_void_.push_back({&c, result, worker_idx})
            → unlock mtx_
            → cv_.notify_one()
    → on std::thread ctor exception:
        → fail_spawn_size/void(&c, error)
            → lock mtx_
            → ready_*.push_back({&c, error, kNoWorker})
            → cv_.notify_one()
    → unlock mtx_

poll()
→ lock mtx_
→ swap ready_size_ + ready_void_ into local deques
→ unlock mtx_
→ for each local entry:
    → publish(c, result)               ← reap publication (single-winner CAS)
    → lock mtx_; --outstanding_; move matching worker out; unlock
    → join that worker (OUTSIDE lock)
→ return count

wait_one()
→ lock mtx_
→ cv_.wait(lk, [&]{ return !ready_*.empty(); })
→ unlock; call poll() for the same per-entry publish/decrement/take/join path
```

**Key properties:**
- One `std::thread` per submitted operation (thread-per-op, NOT thread-per-task)
- `workers_` vector grows with cumulative ops (reaped slots become non-joinable
  placeholders, never reclaimed) — **unbounded retained container growth, P2**
- `std::function` type erasure per op (potential heap allocation depending on
  small-object optimization)
- `std::deque` for ready queues (dynamic allocation on push)
- No capacity limit on outstanding ops (unbounded)
- No queue-full error (never returns `would_block`)

### 2.3 UringAsyncBackend (gated, SLUICE_HAS_LIBURING)

```text
submit_*(op, c)
→ check impl_, fatal_error
→ checked_uring_length(op.len)
→ try_claim(c)                     ← BACKEND is claim authority (CAS; BEFORE SQE acquisition)
→ get_sqe_with_pressure()          ← acquire SQE slot (may fail: ring full)
    → on failure: rollback_claim_before_accept(c); return backend_error   (ADR §10 bridge)
→ io_uring_prep_read/write/fsync   ← fill SQE (unsubmitted)
→ io_uring_sqe_set_data(sqe, id)   ← set user_data = monotonic op id
→ register_op(impl, id, c, OpRec)
    → comp_to_op.emplace(comp_key, id)
    → ops.emplace(id, rec)
    → pending_sqes.push_back({operation, id})
    → next_id = id + 1
→ return {}

NOTE: No io_uring_submit() here. SQEs accumulate in pending_sqes.
Submission to kernel occurs later in poll()/wait_one() via submit_pending().

poll()
→ submit_pending() (flush pending_sqes to kernel)
→ io_uring_peek_batch_cqe()
→ for each CQE: lookup OpRec by id, publish(c, result)    ← reap publication (single-winner CAS)
→ return count

wait_one()
→ submit_pending() + io_uring_submit_and_wait(1)
→ (then drain CQEs as poll)
```

**Transactional admission note (ADR §10):** the backend claims BEFORE
acquiring the SQE and rolls the claim back if SQE acquisition fails (null-SQE
branch only — no untracked SQE on that path). P0-02 REMAINS: `register_op`
container allocations (comp_to_op, ops, pending_sqes) happen AFTER the SQE is
prepared and are still non-transactional — deferred to the RequestSlot PR.

### 2.4 FakeAsyncBackend

```text
submit_*(op, c)
→ try_claim(c) (backend-side; CAS; on failure return invalid_state)
→ ready_size_/pending_size_ push (stage for test-controlled completion)

poll()
→ publish staged ops on demand (test-controlled)

wait_one()
→ publish next staged op (does not truly block)
```

### 2.5 SyncBackend

```text
submit_*(op, c)
→ try_claim(c) (backend-side; CAS; on failure return invalid_state)
→ entries_.push_back(Entry{op, &c})   ← synthetic entry buffered
→ (NO real syscall; NO inline completion)

poll() / wait_one()
→ for each buffered entry: publish(c, synthetic_result)   ← reap publication
→ entries_.clear()
→ return count

cancel(c)
→ find entry in entries_
→ mark entry cancelled (records cancel intent; does NOT publish)
→ entry remains buffered; poll()/wait_one() reap path publishes the canceled result
```

**Cancel authority (P1-03 resolved by PR #61):** cancel no longer publishes a
terminal result directly; it records cancel intent and lets the reap path
(`poll()`/`wait_one()`) publish the terminal canceled result via `publish()`,
matching the unified reap/publication authority.

**Key properties:**
- Synthetic backend for early async foundation testing (job 017)
- Does NOT execute real syscalls; ReadOps complete with their full `len`
- Completion happens at poll()/wait_one() time, NOT at submit time
- (P1-03 resolved by PR #61) `cancel()` previously called the legacy
  `complete_with()` directly, bypassing the poll/wait_one reap authority; it now
  records cancel intent and the reap path publishes the canceled result.

---

## 3. Runtime I/O Path (E16)

```text
RuntimeTaskFn(RuntimeTaskContext& ctx)
→ ctx.submit_read(op, c)
    → AsyncIoContext::submit_read (delegated)
→ ctx.await_completion(c)
    → Scheduler::await_completion_size(c)
        → lock global_mtx_
        → if c.ready(): return inline (no suspend)
        → register in waiting_completion_ map {&c → WaitReg{fiber, owner_worker}}
        → make_waiting(fiber)
        → context_switch(fiber → scheduler continuation)
        → (Fiber suspended)

... later, on backend progress ...

Scheduler worker loop:
→ ctx_.poll() (under access_mtx_)
→ wake_ready_completions_locked()
    → scan waiting_completion_ map
    → for each: if c.ready(): erase registration, make_runnable(fiber), route
→ (routed Fiber resumes on owning worker)
→ Fiber returns from context_switch
→ await_completion returns
→ task reads c.result()
```

---

## 4. Scheduler Progress Model

### 4.1 Who calls poll/wait_one

| Caller | Context | Mode |
|--------|---------|------|
| Scheduler worker loop | MW-S1 (work exists) | `ctx_.poll()` non-blocking |
| Scheduler worker loop | MW-S2 (globally idle, backend outstanding) | `ctx_.wait_one()` blocking (one elected participant) |
| Batch::await_one | Caller-driven | `ctx.wait_one()` |
| op_helpers (read_all etc.) | Caller-driven poll-loop | `ctx.poll()` / `ctx.wait_one()` |

### 4.2 Multi-worker classification (E7/E9)

```text
MW-S1: any worker running/runnable → poll only, no blocking
MW-S2: globally idle + backend outstanding → one elected participant wait_one
MW-S3: globally idle + no backend outstanding + unresolved waits → mode-dependent
QUIESCENT: no work, no waits → return
```

### 4.3 Park domains (E9 P3)

```text
BACKEND domain:   ctx_.wait_one() — at most one worker (E7 rule)
SCHEDULER domain: wake_cv_.wait_for(2ms) — any number of workers

MIXED-WAKE (backend outstanding + external-wake-capable wait registered):
    → MW-S2 participant parks on SCHEDULER domain (NOT backend)
    → backend progress observed by 2ms bounded observation interval
    → external wake observed immediately via wake_cv_
```

### 4.4 The 2ms backstop

The 2ms bounded observation interval in Scheduler-domain park is:
- **Protocol authority** for backend progress observation in MIXED-WAKE mode
  (per ADR §9.4.7.1 E9-CORRECTIVE)
- **Defense-in-depth** for lost wakes in non-MIXED Scheduler-domain park
- NOT the primary wake mechanism for external producers (wake epoch is)
- NOT a busy poll (it is a bounded timed wait on a condition variable)

### 4.5 Backend completion → Scheduler wake

In the current implementation:
- Backend completion does **NOT** directly signal the Scheduler wake source
- Backend readiness is observed by:
  - `poll()` during MW-S1 (non-blocking, every worker loop iteration)
  - `wait_one()` return during MW-S2 backend-domain park
  - 2ms observation interval expiry during MIXED-WAKE Scheduler-domain park
- External producers signal via `SchedulerWakeHandle::notify()` → wake epoch

---

## 5. Shutdown Paths

### 5.1 ApplicationRuntime

```text
request_stop()
→ publish root CancelToken
→ close admission
→ transition to Stopping

drain()
→ wait for admitted_count == terminal_count
→ wait for io_ctx_->outstanding() == 0
→ set drain_complete_

join()
→ signal driver_exit_requested_
→ wake driver (notify_all)
→ driver_thread_.join()
→ close_resources()
    → destroy Group, Scheduler, AsyncIoContext (in order)

shutdown()
→ state-dispatched: request_stop + drain + join in sequence
→ one close owner elected (CloseState CAS)
```

### 5.2 ThreadPoolBackend destruction

```text
~ThreadPoolBackend()
→ lock mtx_; destroying_ = true; unlock
→ for each worker in workers_: if joinable, join
→ (all in-flight syscalls complete before destructor returns)
```

### 5.3 AsyncIoContext destruction

```text
~AsyncIoContext()
→ if backend_ && backend_->outstanding() > 0:
    → detail::async_context_outstanding_fail_fast() → std::terminate
→ (fail-fast in BOTH Debug and Release)
```

### 5.4 Scheduler destruction

```text
~Scheduler()
→ assert quiescence (no outstanding backend ops, no waiting fibers)
→ destroy WorkerState vector
```

---

## 6. Resource Inventory

| Resource | Owner | Lifetime | Capacity | Bounded? | Dynamic growth? | Hot-path alloc? | Failure | Shutdown |
|----------|-------|----------|----------|----------|-----------------|-----------------|---------|----------|
| Driver thread | ApplicationRuntime | start→join | 1 | Yes | No | No | start() error | join() |
| Scheduler workers | Scheduler::run/run_live | invocation | worker_count param | Yes (per invocation) | Grows monotonically across invocations | No (setup) | N/A | joined at run end |
| ThreadPoolBackend threads | ThreadPoolBackend | per-op spawn→reap join | UNBOUNDED | **No** | Yes (per op) | **Yes** (std::thread ctor) | bad_alloc → op error | destructor joins |
| BlockingIoPool workers | BlockingIoPool | pool lifetime | configured | Yes | No | No | N/A | shutdown/join |
| Completion objects | Caller | caller-managed | caller decides | caller | No (by contract) | No | N/A | caller |
| workers_ vector | ThreadPoolBackend | backend lifetime | UNBOUNDED | **No** | Yes (cumulative ops) | No (reap-time) | N/A | destructor |
| ready_size_/ready_void_ deques | ThreadPoolBackend | backend lifetime | outstanding ops | Bounded by outstanding | Yes | **Yes** (push_back) | bad_alloc (unhandled) | drained at reap |
| waiting_completion_ map | Scheduler | Scheduler lifetime | outstanding waits | Bounded by fibers | Yes | Yes (registration) | N/A | assert empty |
| waiting_ready_ map | Scheduler | Scheduler lifetime | registered waits | Bounded by fibers | Yes | Yes (registration) | N/A | assert empty |
| wake_cv_ / wake_mtx_ | Scheduler | Scheduler lifetime | 1 | Yes | No | No | N/A | destructor |
| Fiber stacks | Group / caller | per-task | 64 KiB each | Bounded by tasks | Yes (per task) | Yes (new) | bad_alloc | Group destructor |
| std::function per op | ThreadPoolBackend | per-op | N/A | UNBOUNDED | Yes | **Yes** (heap) | bad_alloc → op error | N/A |

---

## 7. Lock Ordering

```text
ApplicationRuntime::lifecycle_mtx_
    (standalone; never held while calling into Scheduler/AsyncIoContext)

Scheduler::global_mtx_
    → WorkerState::inbox_mtx
    → WaitQueue::mtx_ (E10)

AsyncIoContext::access_mtx_
    → AsyncBackend::mtx_ (ThreadPoolBackend internal)

Group::mtx_
    (acquired under global_mtx_ ONLY via group_stop_predicate callback)
    (never held while calling Scheduler::spawn)

wake_mtx_ (Scheduler park/wake)
    (standalone; notified under global_mtx_ but not acquired under it)
```

---

## 8. Key Authority Assignments (as-built)

| Authority | As-built owner | Evidence |
|-----------|---------------|----------|
| Completion claim (idle → outstanding) | Each backend, via protected `AsyncBackend::try_claim()` (CAS) | ADR-explicit-io-completion-authority §6; `async_io_context.hpp` (`try_claim` helper); all backends check the claim return value. Context does NOT claim (it routes the call). Header comment at `async_io_context.hpp` names the backend as the claim authority (P1-01 resolved by PR #61). |
| Completion publish (outstanding → ready) | Backend reap path, via protected `AsyncBackend::publish()` (single-winner CAS through `publishing` transient) | ADR-explicit-io-completion-authority §7; publish confined to `poll()`/`wait_one()` drain. |
| Completion rollback (outstanding → idle) | Backend, via `AsyncBackend::rollback_claim_before_accept()` (pre-acceptance only) | ADR-explicit-io-completion-authority §10; io_uring SQE-acquisition-after-claim gap. |
| Completion publication to ready | poll()/wait_one() ONLY (A3/O1) | ADR-async-io-model §6 |
| Request identity | Completion pointer and backend-specific records; no common context/slot/generation key | `completion.hpp`; backend sources; P1-06 remains open. The Proposed request-contract ADR is target design only. |
| Request capacity | No common bounded RequestSlot arena | ThreadPool is unbounded; ring depth is not request capacity; Fake/Sync have no configured capacity. |
| Identity-bearing reap | Not implemented; poll/wait_one return a count | Scheduler and Batch recover identity by scanning/reap sequence; P1-07 remains open. |
| Backend admission gate | ThreadPoolBackend::accepting_new_work() | `threadpool_backend.hpp:159` |
| Scheduler wake (external) | SchedulerWakeHandle::notify() | `scheduler.hpp:107` |
| Scheduler wake (internal) | route_runnable_locked → wake epoch | E9 ADR §9.4.4 |
| Runtime admission | ApplicationRuntime::submit() admission gate | `application_runtime.hpp:177` |
| Root cancellation | Group::group_token() (Runtime-owned Group) | ADR-application-runtime §3 |
| Fiber execution identity | Fiber::execution_tag_ (Fiber-local, not TLS) | `application_runtime.hpp:326-331` |

---

## 9. Phase B reference-layer delta (post-baseline)

> This section records a DELTA over the `b20bcc7` baseline above. It is added by
> the Phase B working-tree change (branch `feat/bounded-request-slot-reference`,
> ADR-explicit-io-request-contract **Accepted**) and reflects what the reference
> backends now actually do. The baseline sections (1–8) are left unchanged so
> the pre-Phase-B state remains readable; this section is the authoritative
> description of the reference-layer lifecycle as implemented.

The Phase B change adds a bounded `detail::RequestArena` shared by
`FakeAsyncBackend` and `SyncBackend` and migrates them onto the five-stage
admission (reserve → prepare → commit → enqueue → dispatch/reap) defined by
ADR-explicit-io-request-contract. The public submit/cancel/complete surface is
unchanged (ADR Decision 7); the `RequestKey` is bound privately during commit.

As-built authority changes at the reference layer (production backends
Uring/ThreadPool remain at the baseline until Phase D/E):

| Authority | Phase B reference-layer owner | Evidence |
|-----------|-------------------------------|----------|
| Request identity | `detail::RequestKey{ContextIdentity, SlotIndex, Generation}` in the bounded arena | `request_key.hpp`, `request_slot.hpp` (under `include/sluice/async/detail/`); cancel/reap/register re-validate generation under the leaf mutex (I6) |
| Request capacity | `detail::RequestArena` with construction-time `request_capacity`; full → `would_block`; `capacity_rejections` counter | `request_arena.hpp` (under `include/sluice/async/detail/`); `SyncBackend(request_capacity)`, `FakeAsyncBackend(request_capacity)` |
| Completion claim (reference backends) | Two-stage binding: `begin_binding` (idle → binding CAS) + `commit_binding` (binding → outstanding release-store = submit-success LP) | `completion.hpp` (private binding mutators); `async_io_context.hpp` (protected helpers). Legacy `try_claim` retained for Uring/ThreadPool. |
| Terminal winner | `RequestArena::record_terminal` / `cancel` — exactly-once; losers no-op (I10) | `request_arena.hpp`; proven by `request_lifecycle_scheme_b_test.cpp` |
| Enqueue-in-flight pin | Set at commit; cleared by enqueue/acknowledge as the submit path's final slot access; reap acquire-checks it (reap-ineligible while live) (I17/I19) | `request_slot.hpp`, `request_arena.hpp` |
| Identity-bearing reap | `RequestArena::reap(sink, publish)` delivers a by-value `ReadyEvent{RequestKey, OperationKind, OptionalWaiterDelivery}` to a `SynchronousReadySink` after releasing the leaf mutex (I11/I16) | `ready_sink.hpp`, `reference_ready_sink.hpp` (under `include/sluice/async/detail/`) |
| Slot release / generation++ | `RequestArena::release` — fail-fast (Debug AND Release) on live pin or open registration; generation increments before the slot is visible to a new release | `request_arena.hpp`; `request_arena_death_test.cpp` |

Findings closed at the reference layer: P0-02, P1-02, P1-05, P1-06, P1-07,
P1-09 (arena), P1-10, P2-03. See `current-architecture-findings.md` summary
table and `phase-b-compliance-gate.md` for the evidence ledger. Production-
backend migration (Uring/ThreadPool) remains open for Phase D/E; Scheduler/
Batch/Runtime integration for Phase F/G.
