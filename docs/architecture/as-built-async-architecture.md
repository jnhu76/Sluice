# As-Built Async Architecture

**Authority:** This document describes what the code *actually does* at commit
`d299fc0` (master, post-PR #59). Where ADRs and implementation disagree, this
document records the implementation truth and flags the discrepancy.

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
│       └── SyncBackend         (completes at poll time)
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
        → backend checks Completion is idle
        → backend calls Completion::mark_outstanding()  ← BACKEND is marking authority
        → backend records op (returns Result<void>)
    → tally_submit(stats_, result)
    → update_max_outstanding(stats_)
    → unlock access_mtx_
→ return Result<void>
```

**Authority note:** `AsyncIoContext` does NOT check idle state and does NOT
call `mark_outstanding()`. It only serializes backend access (access_mtx_),
forwards the call, and tallies statistics. ALL backends (ThreadPool, Uring,
Fake, Sync) individually check `c.idle()` and call `c.mark_outstanding()`
inside their own `submit_*` implementations.

The header comment on `AsyncBackend::submit_read` (`async_io_context.hpp:68-70`)
incorrectly states "marking the Completion via mark_outstanding() is the
context's job, not the backend's." This is a **P1 stale comment** — see
findings document P1-01. The implementation truth is: backend marks.

### 2.2 ThreadPoolBackend

```text
submit_*(op, c)
→ accepting_new_work()? (mtx_ guarded destroying_ flag)
→ enqueue_size/void(c, work_lambda)
    → c.mark_outstanding()           ← BACKEND marks (correct authority)
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
→ drain ready_size_ + ready_void_
    → for each: c->complete_with(result); --outstanding_
→ collect worker indices to join
→ unlock mtx_
→ join each collected worker thread (OUTSIDE lock)
→ return count

wait_one()
→ lock mtx_
→ cv_.wait(lk, [&]{ return !ready_*.empty() || destroying_; })
→ (then same drain as poll)
```

**Key properties:**
- One `std::thread` per submitted operation (thread-per-op, NOT thread-per-task)
- `workers_` vector grows with cumulative ops (reaped slots become non-joinable
  placeholders, never reclaimed) — **bounded memory growth, P2**
- `std::function` type erasure per op (heap allocation for the work lambda)
- `std::deque` for ready queues (dynamic allocation on push)
- No capacity limit on outstanding ops (unbounded)
- No queue-full error (never returns `would_block`)

### 2.3 UringAsyncBackend (gated, SLUICE_HAS_LIBURING)

```text
submit_*(op, c)
→ c.mark_outstanding() (backend-side)
→ register_op: comp_to_op.emplace, ops.emplace, pending_sqes.push_back
→ io_uring_get_sqe() → prepare pread/pwrite/fsync
→ io_uring_submit()

poll()
→ io_uring_peek_batch_cqe()
→ for each CQE: lookup Completion*, complete_with(result)
→ return count

wait_one()
→ io_uring_submit_and_wait(1)
→ (then drain CQEs as poll)
```

### 2.4 FakeAsyncBackend

```text
submit_*(op, c)
→ c.mark_outstanding() (backend-side)
→ ready_size_/pending_size_ push (stage for test-controlled completion)

poll()
→ complete staged ops on demand (test-controlled)

wait_one()
→ complete next staged op (does not truly block)
```

### 2.5 SyncBackend

```text
submit_*(op, c)
→ c.mark_outstanding() (backend-side)
→ entries_.push_back(Entry{op, &c})   ← synthetic entry buffered
→ (NO real syscall; NO inline completion)

poll() / wait_one()
→ for each buffered entry: c.complete_with(synthetic_result)
→ entries_.clear()
→ return count

cancel(c)
→ find entry in entries_
→ c.complete_with(cancelled) DIRECTLY  ← bypasses reap authority (see findings P1-03)
→ entries_.erase(it)
```

**Key properties:**
- Synthetic backend for early async foundation testing (job 017)
- Does NOT execute real syscalls; ReadOps complete with their full `len`
- Completion happens at poll()/wait_one() time, NOT at submit time
- `cancel()` calls `complete_with()` directly, bypassing the poll/wait_one
  reap authority — this is an **authority conflict** (see findings P1-03)

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
| Completion mark_outstanding | Each backend (inside submit_*) | `threadpool_backend.cpp:71`, `uring_backend.cpp:484`, `fake_backend.hpp:221,228`, `sync_backend.hpp:40,46,52,58`. Context does NOT call it (grep `async_io_context.cpp` = 0 matches). Header comment at `async_io_context.hpp:68-70` is STALE (P1-01). |
| Completion complete_with | Backend (via poll/wait_one reap) | `threadpool_backend.cpp:160-163` |
| Completion publication to ready | poll()/wait_one() ONLY (A3/O1) | ADR-async-io-model §6 |
| Backend admission gate | ThreadPoolBackend::accepting_new_work() | `threadpool_backend.hpp:159` |
| Scheduler wake (external) | SchedulerWakeHandle::notify() | `scheduler.hpp:107` |
| Scheduler wake (internal) | route_runnable_locked → wake epoch | E9 ADR §9.4.4 |
| Runtime admission | ApplicationRuntime::submit() admission gate | `application_runtime.hpp:177` |
| Root cancellation | Group::group_token() (Runtime-owned Group) | ADR-application-runtime §3 |
| Fiber execution identity | Fiber::execution_tag_ (Fiber-local, not TLS) | `application_runtime.hpp:326-331` |
