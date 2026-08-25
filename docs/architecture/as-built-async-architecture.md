# As-Built Async Architecture

**Authority:** This document describes what the code *actually does* at commit
`b20bcc7` (master, including PR #60 and PR #61). Where ADRs and implementation
disagree, this document records the implementation truth and flags the
discrepancy.

**Target-design boundary:**
[ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md)
is Accepted (2026-08-02). Its `RequestKey`, bounded `RequestSlot`, identity-bearing
reap, capacity, cancel disposition, and close-admission lifecycle were **not**
present in the `b20bcc7` baseline; they are implemented today at the reference
layer (Phase B, PR #63 — see §9), for `ThreadPoolBackend` (Phase E, PR #64 —
see §2.2), and for `UringAsyncBackend` (Phase D, PR #78/#80/#83/#84 — see §2.3).
Sections 1–8 below remain the `b20bcc7` baseline record; the current state of
each subsystem is carried by the update blocks in §2.2/§2.3 and the delta table
in §9. Re-baselined by audit issue #94 (2026-08-13).

**Request lifecycle navigation (issue #139):** the end-to-end story of ONE
explicit-I/O request — annotated state diagram, authority table, and a worked
ThreadPoolBackend walkthrough from `submit_write` to `Completion::reset` —
lives in [async-request-lifecycle.md](async-request-lifecycle.md). This file
keeps the component/progress/shutdown view; that file owns the per-request
narrative.

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
│   ├── WaitRecord pool (preallocated, identity-bearing waits, Phase F1)
│   ├── waiting_completion_ map (legacy fallback, non-arena backends only)
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

> Per-request narrative (state diagram, authority table, full walkthrough):
> [async-request-lifecycle.md](async-request-lifecycle.md) — this section keeps
> the per-backend submission-shape comparison.

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

> **Phase E update (merged to master as PR #64, `a8178d8`):**
> The per-op-thread model, `std::function` payload, `try_claim` admission, and
> `ready_size_`/`ready_void_` deques described below are the **pre-Phase-E
> legacy** record. Phase E replaced them with: `ThreadPoolConfig{request_
> capacity, worker_count}`; a fixed pool of persistent blocking-I/O workers
> (created only at construction); a construction-time bounded `BoundedDispatch
> Queue`; the five-stage `RequestArena` admission (reserve → prepare → install
> publication binding → begin_binding → commit → install_binding →
> commit_binding → enqueue) mirroring the migrated reference backends; a fixed
> `PreparedBlockingOp` per slot (no `std::function`/`Completion*`); workers that
> record `backend-ready` ONLY via `record_terminal`; reap-only Completion-ready
> publication; real-syscall descriptor validation before commit; and a
> persistent ready-epoch wake. See `docs/design/phase-e-bounded-threadpool-
> backend.md` and `docs/architecture/phase-e-compliance-gate.md`. DIV-03 and
> DIV-12 are Resolved; DIV-14 is partially resolved for ThreadPool.

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

> **Phase D update (merged to master: D1 PR #78, D2 PR #80, D3 PR #83,
> D4 PR #84):** the `pending_sqes`/`comp_to_op`/`ops` model, the `id`-based
> `user_data`, and the legacy `try_claim` admission described below are the
> **pre-Phase-D legacy** record. Phase D replaced them with: the five-stage
> `RequestArena` admission shared with the other backends (reserve → prepare →
> Stage 1.5 descriptor validation → begin_binding/commit_binding → commit →
> enqueue → dispatch); a **single private ring** whose `SQE.user_data` carries
> a 64-bit op cookie (generation-safe, `uring_backend.cpp:101-119`), not a
> `Completion*`; `UringConfig{request_capacity=64, queue_depth=64}` with
> `would_block` admission at capacity (capacity independent of ring depth, ADR
> Decision 13/18); `TransportLedger` physical-SQ tracking preserving unsubmitted
> suffixes after partial submit (P0-D recovery); descriptor validation of the
> Decision-6 `invalid_argument` causes before commit (`uring_backend.cpp:378-433`);
> workers/CQE handlers that record `backend-ready` ONLY via `record_terminal`;
> reap-only Completion-ready publication; a wait source (ring-fd poll + control
> eventfd, D4) with split-phase `wait_one`; `close_admission()` with accept-LP
> serialization; and quiescent destruction fail-fast. The C2b/C2c/C2d/C2e
> conformance records are closed with real-liburing evidence; KernelIo is
> ELIGIBLE in real mode and honestly INCOMPLETE in stub builds. See
> `docs/architecture/phase-d-uring-migration-plan.md`,
> `phase-d2-uring-failure-noalloc-gate.md`,
> `docs/history/closeout/phase-d3-uring-identity-waiter-gate.md`, and
> `docs/history/closeout/phase-d4-uring-wait-close-drain-gate.md`. P0-02, P1-06, and DIV-14 are
> resolved for Uring (see §9, `current-architecture-findings.md`, and
> `divergence-registry.md`).

The pseudocode and transactional note below describe the **pre-Phase-D legacy
record** and are retained for history:

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

**Transactional admission note (ADR §10) — pre-Phase-D legacy:** the backend
claims BEFORE acquiring the SQE and rolls the claim back if SQE acquisition
fails (null-SQE branch only — no untracked SQE on that path). P0-02 REMAINED
at the baseline: `register_op` container allocations (comp_to_op, ops,
pending_sqes) happened AFTER the SQE was prepared and were non-transactional —
deferred to the RequestSlot PR, and closed by the Phase D RequestArena
migration (see the update block above).

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
        → acquire WaitRecord from preallocated pool (free-list pop)
           if nullptr: return IoError::no_space (pool exhausted)
        → register arena waiter (WaiterToken + RoutingLease)
           legacy fallback: waiting_size_ map if backend returns not_supported
        → commit_suspend_locked(fiber)
        → context_switch(fiber → scheduler continuation)
        → (Fiber suspended)

... later, on backend progress ...

Scheduler worker loop:
→ drain_routed_completion_waits_locked() (under global_mtx_)
    → ctx_.poll() (under access_mtx_)
       → arena.reap(sink) invokes ReadyRoutingSink.on_ready()
       → sink marks record delivered under wait_registry_mtx_
    → pop delivered list under wait_registry_mtx_
    → per record: make_runnable(fiber), route_runnable_locked(fiber, owner)
→ (routed Fiber resumes on owning worker)
→ Fiber returns from context_switch
→ await_completion returns (reads frozen CompletionWaitOutcome)
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

### 4.3 Park domains (E9 P3; Phase G split-wait amendment)

```text
BACKEND domain:   ctx_.wait_one() — at most one worker (E7 rule)
SCHEDULER domain: wake_cv_ park (bounded by the 2ms interval) — any number of workers

Production split-wait backends (ThreadPool ReadyWaitSource, real-liburing
UringWaitSource — supports_bounded_wait()):
    MIXED-WAKE parks the BACKEND domain for BOTH wake kinds. The MW-S2
    commit arms the backend wait (arm_committed_wait), publishes
    backend_wait_active_, and parks in ctx_.wait_one(max_park). External
    Scheduler wakes reach the parked participant through the bridge:
    signal_wake_locked -> backend_wait_active_ -> interrupt_backend_waiters
    (control-epoch bump + notify). Backend progress arrives through the
    wait source's own epoch. The 2ms interval applies ONLY when an active
    deadline demands a bounded park cap; it is defense-in-depth, not the
    authority, on this path.

Reference poll-driven backends (Fake, Sync/Synthetic):
    MIXED-WAKE retains the E9 P3 shape — the participant parks on the
    SCHEDULER domain and observes backend progress by the bounded
    observation interval. Their readiness cannot self-notify (poll-driven),
    so the interval remains protocol authority there (DIV-05 reference
    exemption, Phase G amended).
```

### 4.4 The 2ms backstop (Phase G closeout verdict)

The 2ms bounded observation interval in Scheduler-domain park is:
- **Protocol authority** for backend progress observation in MIXED-WAKE on
  the reference poll-driven backends only (per ADR §9.4.7.1 E9-CORRECTIVE,
  Phase G amendment)
- **Defense-in-depth** for lost wakes in non-MIXED Scheduler-domain park
- **Defense-in-depth, condition-driven only** for MIXED-WAKE on split-wait
  production backends: the park cap applies only when a condition genuinely
  demands a bounded re-drain — an active deadline (E11 timer pump) or a
  registered level-triggered ready-flag wait (E5-A2 poll resolution, 2ms).
  With neither present the park is the unbounded sentinel and progress /
  bridge wakes it through the epoch / control-interrupt transport — there is
  no fixed-interval polling tax on this path
- NOT the primary wake mechanism for external producers (wake epoch is)
- NOT a busy poll (it is a bounded timed wait on a condition variable)

### 4.5 Backend completion → Scheduler wake (Phase G as-built)

- On split-wait production backends, backend readiness signals the READY
  domain (`ReadyWaitSource::signal_progress` / uring re-poll signal), and a
  parked MW-S2 participant observes it through its `ctx_.wait_one()` park —
  prompt, no observation interval.
- External Scheduler wakes reach a parked backend participant through the
  bridge: `signal_wake_locked` publishes the wake epoch, then (acquire-load
  of `backend_wait_active_`) calls `interrupt_backend_waiters()` — a
  control-epoch bump plus the wait source's interrupt transport (cv notify /
  control-fd poll wake). The interrupted `wait_one` returns; the
  participant re-drains both domains and reclassifies. One-shot by
  construction: a later invocation snapshots the advanced control epoch and
  parks normally (no busy-spin).
- The MW-S2 commit-to-park window is closed by the D4-RM13 invocation-level
  control baseline plus the armed floor (`arm_committed_wait` /
  `consume_committed_wait`): a stop or external wake landing between the
  commit and the park is observed by that invocation, not rebaselined away.
- `poll()` during MW-S1 remains non-blocking; `Batch::await_one` and
  op_helpers park through the same `ctx_.wait_one()` authority.
- External producers signal via `SchedulerWakeHandle::notify()` → wake epoch
  (and, if a participant is parked in the backend domain, the bridge above).
- **Phase F1 (issue #98) identity route:** an arena-backed reap with a
  registered Scheduler waiter calls the Scheduler-owned `ReadyRoutingSink`
  with the by-value `ReadyEvent`; the sink only marks the record delivered
  (leaf `wait_registry_mtx_`, allocation-free). The worker-loop drain then
  pops the delivered records under `global_mtx_` and routes each fiber via
  `route_runnable_locked` (which signals the wake epoch) — the reap itself
  never wakes the Scheduler directly, and a delivery is never lost to the
  drain: delivered-list publication happens under `wait_registry_mtx_`, and
  both the MW-S1 loop-top drain and the MW-S2/MW-S3 park-return drains
  observe it before parking/terminating.

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
| WaitRecord pool (`wait_records_` + free list, R) | Scheduler | Scheduler lifetime | `wait_capacity` (default 256) | Yes (fixed) | No (preallocated) | No (free-list pop) | `no_space` on exhaustion | all records freed at `~Scheduler` (assert) |
| waiting_completion_ map (legacy fallback) | Scheduler | Scheduler lifetime | outstanding waits | Bounded by fibers | Yes (legacy only) | Yes (registration, legacy path only) | N/A | assert empty |
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

> The rows below describe the `b20bcc7` baseline. Current owners after Phase B
> (reference layer), Phase E (ThreadPool), and Phase D (Uring) are listed in §9
> and the §2.2/§2.3 update blocks: request identity, capacity, identity-bearing
> reap, and publication binding are now the shared `detail::RequestArena` /
> `RequestSlot` authorities on all four backends.

| Authority | As-built owner | Evidence |
|-----------|---------------|----------|
| Completion claim (idle → outstanding) | Each backend, via protected `AsyncBackend::try_claim()` (CAS) | ADR-explicit-io-completion-authority §6; `async_io_context.hpp` (`try_claim` helper); all backends check the claim return value. Context does NOT claim (it routes the call). Header comment at `async_io_context.hpp` names the backend as the claim authority (P1-01 resolved by PR #61). |
| Completion publish (outstanding → ready) | Backend reap path, via protected `AsyncBackend::publish()` (single-winner CAS through `publishing` transient) | ADR-explicit-io-completion-authority §7; publish confined to `poll()`/`wait_one()` drain. |
| Completion rollback (outstanding → idle) | Backend, via `AsyncBackend::rollback_claim_before_accept()` (pre-acceptance only) | ADR-explicit-io-completion-authority §10; io_uring SQE-acquisition-after-claim gap. |
| Completion publication to ready | poll()/wait_one() ONLY (A3/O1) | ADR-async-io-model §6 |
| Request identity | Completion pointer and backend-specific records; no common context/slot/generation key | `completion.hpp`; backend sources; **`b20bcc7` baseline row** — P1-06 is since CLOSED (Phase B reference, Phase E ThreadPool, Phase D Uring: per-slot Generation + stale-key rejection) and the request-contract ADR is ACCEPTED (`ADR-explicit-io-request-contract`); the current identity owner is the shared `detail::RequestArena` / `RequestSlot` on all four backends (§9, §2.2/§2.3). |
| Request capacity | No common bounded RequestSlot arena | ThreadPool is unbounded; ring depth is not request capacity; Fake/Sync have no configured capacity. |
| Identity-bearing reap | Not implemented at the `b20bcc7` baseline; poll/wait_one return a count | Scheduler and Batch recover identity by scanning/reap sequence; P1-07 was open. **Since closed**: the arena owns identity-bearing reap on all four backends (§9) and Phase F1 makes the PRODUCTION Scheduler its consumer (§9 F1 delta, `docs/design/phase-f1-scheduler-ready-sink.md`). |
| Backend admission gate | ThreadPoolBackend::accepting_new_work() | `threadpool_backend.hpp:159` |
| Scheduler wake (external) | SchedulerWakeHandle::notify() | `scheduler.hpp:107` |
| Scheduler wake (internal) | route_runnable_locked → wake epoch | E9 ADR §9.4.4 |
| Runtime admission | ApplicationRuntime::submit() admission gate | `application_runtime.hpp:177` |
| Root cancellation | Group::group_token() (Runtime-owned Group) | ADR-application-runtime §3 |
| Fiber execution identity | Fiber::execution_tag_ (Fiber-local, not TLS) | `application_runtime.hpp:326-331` |

---

## 9. Phase B reference-layer delta (post-baseline)

> This section records a DELTA over the `b20bcc7` baseline above. It is added by
> the Phase B change (merged to master as PR #63, `7f434f0`;
> ADR-explicit-io-request-contract **Accepted**) and reflects what the reference
> backends now actually do. The baseline sections (1–8) are left unchanged so
> the pre-Phase-B state remains readable; this section is the authoritative
> description of the reference-layer lifecycle as implemented.

The Phase B change adds a bounded `detail::RequestArena` shared by
`FakeAsyncBackend` and `SyncBackend` and migrates them onto the five-stage
admission (reserve → prepare → commit → enqueue → dispatch/reap) defined by
ADR-explicit-io-request-contract. The public submit/cancel/complete surface is
unchanged (ADR Decision 7); the `RequestKey` is bound privately during commit.

As-built authority changes at the reference layer (the production backend
Uring remains at the baseline until Phase D; ThreadPool migrated in Phase E —
see §2.2):

| Authority | Phase B reference-layer owner | Evidence |
|-----------|-------------------------------|----------|
| Request identity | `detail::RequestKey{ContextIdentity, SlotIndex, Generation}` in the bounded arena | `request_key.hpp`, `request_slot.hpp` (under `include/sluice/async/detail/`); cancel/reap/register re-validate generation under the leaf mutex (I6) |
| Request capacity | `detail::RequestArena` with construction-time `request_capacity`; full → `would_block`; `capacity_rejections` counter | `request_arena.hpp` (under `include/sluice/async/detail/`); `SyncBackend(request_capacity)`, `FakeAsyncBackend(request_capacity)` |
| Completion claim (reference backends) | Two-stage binding: `begin_binding` (idle → binding CAS) + `commit_binding` (binding → outstanding release-store = submit-success LP) | `completion.hpp` (private binding mutators); `async_io_context.hpp` (protected helpers). Legacy `try_claim` retained for Uring/ThreadPool. |
| Terminal winner | `RequestArena::record_terminal` / `cancel` — exactly-once; the state is validated BEFORE the terminal is written (a non-accepted slot fails fast — review I2); losers no-op (I10) | `request_arena.hpp`; proven by `request_lifecycle_scheme_b_test.cpp` + `request_arena_death_test.cpp :: record-terminal-on-prepared` |
| Enqueue-in-flight pin | Set at commit; cleared by enqueue as the submit path's FINAL slot access (the `acknowledge_enqueue_pin` escape hatch was removed in round 2 — the concurrent test asserts the pin is already cleared after join instead of masking a pin bug); reap acquire-checks it (reap-ineligible while live) (I17/I19) | `request_slot.hpp`, `request_arena.hpp` |
| Completion publication binding | The type-erased binding (opaque Completion*, requested_bytes, publish thunk) lives IN the RequestSlot record; installed before commit; reap validates it BEFORE any accounting change and publishes Completion-ready THROUGH it inside the leaf domain (review C2/C3 — no parallel identity map; a missing binding fails fast, I4/I5/I11) | `request_slot.hpp`, `request_arena.hpp`; cancel resolves via `RequestArena::resolve_completion` (bounded O(capacity) slot scan) |
| Identity-bearing reap | `RequestArena::reap(sink)` — allocation-free SINGLE-DOMAIN protocol (review C3): per eligible slot under one lock acquisition it validates the binding, closes registration, takes any waiter delivery, ends the borrow, transitions to completion_ready, decrements the counters, and publishes Completion-ready THROUGH the slot binding (the ready release-store is the leaf domain's own linearization point — I18), then leaves the lock and delivers a by-value `ReadyEvent{RequestKey, OperationKind, OptionalWaiterDelivery}` to a `SynchronousReadySink` (I9/I11/I16) | `request_arena.hpp`, `ready_sink.hpp`, `reference_ready_sink.hpp` (under `include/sluice/async/detail/`); `acquire_observer_of_ready_sees_all_effects` acquire-loads a real `Completion::ready()` |
| Slot release / generation++ | TWO authorities (review I1): `rollback_reserved_or_prepared` (pre-commit rollback, recoverable errors) and `release_completed_binding` (the caller's reset/ready-destruction handshake — ANY failure fails fast in Debug AND Release: stale handle, live pin, open registration, wrong state). Generation increments before the slot is visible to a new reserve. Arena destruction with `slot_in_use != 0` also fails fast (no dangling capability). | `completion.hpp` (private binding payload), `request_arena.hpp`; `completion_binding_test.cpp`, `request_arena_death_test.cpp` |
| fd/buffer borrow (I7/I8) | Borrow metadata (fd, address, length) written at prepare; borrow begins at commit and ends at completion-ready publication (observable via `borrow_active`) | `request_slot.hpp`; `request_arena_test.cpp :: arena_borrow_lifecycle` |

Findings closed at the reference layer: P0-02, P1-02, P1-05, P1-06, P1-07,
P1-09 (arena), P1-10, P2-03. See `current-architecture-findings.md` summary
table and `phase-b-compliance-gate.md` for the evidence ledger. Production
backend migration is complete: ThreadPool in Phase E (PR #64 — see
`phase-e-compliance-gate.md` and §2.2) and Uring in Phase D (PR #78/#80/#83/#84
— see `docs/history/closeout/phase-d4-uring-wait-close-drain-gate.md` and §2.3).

### 9.1 Phase F1 delta — production Scheduler consumes identity-bearing reap

> Added by Phase F1 (issue #98; `docs/design/phase-f1-scheduler-ready-sink.md`,
> `docs/architecture/phase-f1-compliance-gate.md`). This is the first
> PRODUCTION consumer of the arena's identity-bearing reap: the Scheduler no
> longer recovers identity by scanning `Completion*`-keyed maps and re-checking
> `c->ready()`.

- **Registration.** `Scheduler::await_completion_size/void` now creates a
  Scheduler wait record (`WaitRecord` in a bounded intrusive free-list
  registry) and registers a REAL arena waiter on the accepted request:
  `WaiterToken{scheduler_identity, record_index, record_generation}` plus a
  `RoutingLease::pinning` that pins the record. The arena leaf (under
  `global_mtx_` → `access_mtx_`) arbitrates registration against reap
  extraction exactly once. Non-arena backends (`register_waiter` →
  `not_supported`) keep the legacy `Completion*`-keyed maps as a DISJOINT
  fallback — never a second authority on the identity path.
- **Delivery.** The backend reap (Fake/Sync/ThreadPool/Uring) calls the
  Scheduler-owned `ReadyRoutingSink` (installed via
  `AsyncIoContext::set_ready_sink`) with the by-value `ReadyEvent{key, kind,
  OptionalWaiterDelivery{token, lease}}`; the sink validates scheduler
  identity + record generation, then links the record into the delivered list
  (allocation-free; no user code under the sink lock).
- **Routing.** The worker-loop drain (`drain_routed_completion_waits_locked`,
  under `global_mtx_`) pops delivered records and routes the fiber exactly
  once via the canonical `route_runnable_locked` — the Scheduler remains the
  sole fiber-routing authority. Reap-won and cancel-won races resolve through
  the same record terminality (I16/I4).
- **Waiter cancellation.** `Scheduler::cancel_waiter` (and the production
  caller `RuntimeTaskContext::cancel_waiter`, ADR Decision 10) removes ONLY
  the waiter: the arena leaf races cancel against reap; on cancel-win the
  pinned record is retired and the fiber is woken with the wait-cancelled
  outcome while the I/O, borrow, and terminal result stay untouched (I5).
- **Lock order.** The registry adds the LEAF `wait_registry_mtx_` (R):
  G→A (existing), G→R (registration/drain/cancel); the sink takes only R;
  R never precedes G/A. No join, allocation, syscall, or user code under R.
- **Shutdown.** `~Scheduler` fail-fasts when `wait_record_live_count_ != 0`
  (a registered waiter neither delivered nor cancelled — abandoned wake
  obligation; Debug and Release).
- **Tests.** `tests/scheduler_identity_wake_test.cpp` (T1–T10: routing,
  completion-before-registration inline return, exactly-once wake,
  waiter-cancel keeps I/O, cancel-vs-reap race, stale-generation drop,
  duplicate-waiter invalid_state, shutdown convergence, SyncBackend and
  ThreadPoolBackend routing), `tests/scheduler_identity_wake_death_test.cpp`
  (registry-nonempty destruction fail-fast), and the real-liburing
  `uring_f1_scheduler_routing` case.

Remaining after F1+F2+F3 (Phase F COMPLETE): only the backend-ready wake
bridge (Phase G). F2 added `BatchResultOrigin` (rejected vs
accepted_and_completed) to `BatchResult` (ADR Decision 9 — Batch consumes
outcome origin explicitly; `tests/batch_result_origin_test.cpp`). F3 added the
public `RequestHandle` identity surface (ADR-public-request-handle): additive
`submit_*_request -> Result<RequestHandle>`, the read-only `request_state`
identity consumer, non-forgeable construction (negative-compile gate), and the
`not_supported` policy for external/non-arena backends
(`tests/request_handle_test.cpp` — cross-context C2b-4b + stale-generation
C2c-14b provenance). Phase G (wake bridge, 2ms backstop decision, DIV-04/05
reclassification) remains separate and untouched.
