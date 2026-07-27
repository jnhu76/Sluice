# E16 Application Runtime Architecture

```text
STATUS: Proposed — design only. Implementation unauthorized.
BASELINE_SHA: ba7eb62563ca7c8af19e264ddb05a5a88a2fd7a7
BASELINE_DATE: 2026-07-27
```

## 1. Authority chain

```text
1. Task prompt E16-A0-APPLICATION-RUNTIME-ARCHITECTURE-1
2. AGENTS.md
3. Accepted ADRs (ADR-024S, ADR-async-io-model, ADR-execution-model)
4. Current architecture + API documentation (docs/api-reference.md)
5. Public production headers (include/sluice/**)
6. Production implementation (src/async/**)
7. Tests and acceptance consumers (tests/**)
8. Build targets and CI (xmake.lua, .github/workflows/ci.yml)
9. Historical plans, reviews, closeouts, comments, commits
```

Historical material is evidence, not current authority.

## 2. Executive summary

Sluice's async runtime foundation (E0–E15) is complete: `Scheduler`, `Fiber`,
`Group`, `Future`, `Completion<T>`, `AsyncIoContext`, `AsyncBackend`, cooperative
cancellation, and the E10–E13 synchronization primitives. What does not yet exist
is the **application layer** that ties these components into a coherent, startable,
stoppable, drainable, joinable unit with lifecycle authority.

This document proposes the architecture for that layer: an `ApplicationRuntime`
(working name — PROPOSED, not an existing API) that owns the `AsyncIoContext`,
`Scheduler`, root task domain (a root `Group`), root cancellation, and a single
dedicated driver thread, and exposes a unified
`start / submit / request_stop / drain / join / shutdown` contract.

The preferred architecture is **Alternative C: builder-constructed one-shot
Runtime with an injected backend**, driven by a **dedicated driver thread**. The
driver is the only thread the Runtime spawns; it is the only caller of
`Scheduler::run_live`. The Runtime owns its `AsyncIoContext` and `Scheduler`;
the backend is injected (enabling deterministic test injection). Construction is
separated from `start()`; `start()` is a transaction with rollback. The lifecycle
is one-shot (Constructed → Running → Stopped). The destructor requires explicit
`shutdown()` and fail-fast on misuse.

This is not a "zero new seam" layer. To close the lifecycle authority gaps
honestly, the design requires **four private PROPOSED seams** (none public, none
authorizing implementation):

- `sluice::app::detail::runtime_lifetime_fail_fast` — a private fail-fast entry
  for the Runtime destructor (production currently has NO generic fail-fast;
  all 7 existing entries are subsystem-bound — §18, §29).
- a **Fiber-local execution-identity seam** — an opaque tag stored *in Fiber
  state* (not `thread_local`, which is unsound under Fiber multiplexing) and read
  via a private Scheduler/detail accessor, so the Runtime can detect calls from
  its own tasks (§12, §21).
- a `RuntimeTaskTerminalGuard` — a `noexcept` RAII guard that publishes
  `terminal_count++` exactly once even if the user task throws (§16).
- a `recompute_task_set_terminal_locked()` helper — invoked under
  `lifecycle_mutex` on every mutation of admission/count state (§16).

None of these is a change to Scheduler *drive semantics* and none is a new public
API. They are private implementation seams, listed as ADR consequences.

**Implementation authorization: DENIED.** This task produces architecture
documentation and a Proposed ADR only.

## 3. Problem statement

The current Sluice async foundation provides powerful primitives but no unified
lifecycle authority. A consumer who wants to run evented tasks today must
manually:

1. Construct an `AsyncIoContext` with a backend (`async_io_context.hpp:121`).
2. Construct a `Scheduler` borrowing that context (`scheduler.hpp:213`).
3. Construct a `Group(Scheduler&)` for the root task set (`group.hpp:78`).
4. Call `Group::async()` to admit tasks (`group.hpp:94`).
5. Call `Group::await()` which drives `Scheduler::run_live(1, stop_predicate)`
   (`group.cpp:57-72`).
6. Rely on the caller to ensure correct destruction order.

This manual composition has gaps that the E16 Application Runtime must close.
The load-bearing production facts that constrain any solution:

- **`Scheduler::run()` / `run_live()` block the caller** (`scheduler.hpp:241,249`;
  `scheduler.cpp:495-584`). Workers are created AND joined *inside* the blocking
  call (`scheduler.cpp:565-579`); the worker `std::thread` objects are
  `run_impl` locals. **There is no separate worker-start / worker-join API.**
- **`Group::await()` drives `run_live` inline on the caller's OS thread**
  (`group.cpp:57-72`). `await` and "drive the scheduler" are the same blocking
  call; `run_live` joins its workers before returning.
- **`run_live` may return for reasons other than all work done**: on QUIESCENT,
  on MW-S3 without an effective wake source, or when the invocation stop
  predicate returns true (`scheduler.cpp:856-857, 891-897`). A single
  `run_live` invocation therefore cannot be assumed to remain resident for a
  Runtime's lifetime; a re-entry loop is required.
- **Backend `outstanding_` decrements at poll reap, not syscall completion**
  (`threadpool_backend.cpp:160-163`). So `outstanding()==0` requires that
  someone has polled/reaped every op to completion — which only a live
  Scheduler invocation does. Drain cannot mean merely "task bodies returned".

These gaps drive the architecture:

- **No unified start.** There is no non-blocking "launch workers and return"
  entry that an application can later stop. A dedicated driver thread is
  required because the only way to drive the Scheduler is a blocking call, and
  the caller of `start()` must not block.
- **No admission control.** `Group::async()` always succeeds (or throws); there
  is no gate that rejects submission after a stop request.
- **No runtime-level cancellation.** Cancellation is per-`Group`
  (`group.hpp:114`) or per-`Future` (`future.hpp:89`); there is no root cancel
  state that an application can publish to stop all work.
- **No startup rollback.** If the driver thread fails to spawn, there is no
  mechanism to leave the Runtime in a clean `StartFailed` state.
- **No drain/join separation.** `Group::await()` conflates "wait for tasks" with
  "drive the scheduler"; there is no independent drain-of-admitted-work vs.
  join-of-driver-thread distinction.
- **No destructor contract.** `Scheduler::~Scheduler()` asserts quiescence
  (`scheduler.cpp:114-196`); `Group::~Group()` fail-fast on pending evented
  tasks (`group.cpp:117-122`); `AsyncIoContext::~AsyncIoContext()` fail-fast on
  outstanding Completions (`async_io_context.cpp:30-41`). But there is no single
  owner that enforces the correct shutdown sequence before destruction.

The E16 Application Runtime exists to close these gaps.

## 4. Goals

1. Define a unified lifecycle: construct → start → run → stop → drain → join → destroyed.
2. Own and order the construction/destruction of `AsyncIoContext`, `Scheduler`,
   root task domain, and root cancellation.
3. Drive the Scheduler through a single dedicated driver thread with a
   `run_live` re-entry loop.
4. Provide an admission contract with a deterministic linearization point.
5. Provide separate, non-conflated `request_stop`, `drain`, `join`, `shutdown`.
6. Define startup as a transaction with rollback at every fallible step.
7. Define a destructor contract that prevents resource leaks and undefined behavior.
8. Support deterministic test injection of backends.
9. Drain outstanding backend I/O *before* the driver exits.
10. Detect lifecycle calls made from inside a Runtime task (Fiber-local
    execution identity).

## 5. Non-goals

| Non-goal | Decision | Rationale |
| --- | --- | --- |
| Restartable Runtime | **REJECTED** (E16) | One-shot lifecycle is the default. Restart requires backend/Scheduler reconstruction, worker generation, cancellation generations, stale references — all out of scope for E16. |
| Multiple independent root runtimes sharing one Scheduler | **REJECTED** (E16) | Scheduler is non-movable, non-copyable (`scheduler.hpp:216-219`). Sharing would require lifetime coordination that the Runtime does not provide. |
| Dynamic backend replacement | **REJECTED** (E16) | `AsyncIoContext` is move-only; backend is injected at construction (`async_io_context.hpp:121`). No hot-swap. |
| Hot worker-count resizing | **REJECTED** (E16) | Worker count is fixed at `start()` time. |
| Structured child submission | **REJECTED** (E16 v1) | `TaskFn = void(CancelToken&)` (matching `group.hpp:89`) gives no child-spawn capability. A restricted `TaskContext` with `spawn()` is a future extension. See §13.4. |
| Cross-process runtime management | **REJECTED** (E16) | Single-process only. |
| Durable work queue | **REJECTED** (E16) | Future consumer, not part of E16-A0. |
| Network server framework | **REJECTED** (E16) | Out of scope. |
| Structured logging framework | **REJECTED** (E16) | Out of scope. |
| Plugin system | **REJECTED** (E16) | Out of scope. |
| Global singleton Runtime | **REJECTED** (E16) | No globals (AGENTS.md §7: no new globals). |
| Caller-driven single-worker Runtime as the default | **REJECTED as default** (E16) | May be documented as a future deterministic/manual variant. It makes `start()` non-operational, removes parallelism, and collapses drain into execution. |
| Non-Linux backend completion | **REJECTED** (E16) | Linux-only for E16. |
| Stable ABI | **REJECTED** (E16) | Header-only C++20, no ABI stability. |
| Automatic recovery after fatal invariant violation | **REJECTED** (E16) | Fail-fast is the contract (`fail_fast.cpp:16-62`). |

## 6. Current production inventory

### 6.1 Component table

| Component | Header | Role |
| --- | --- | --- |
| `Scheduler` | `include/sluice/async/scheduler.hpp:211` | Multi-worker Evented scheduler. Owns WorkerState vector, global_mtx_, timer subsystem, wake source. Borrows AsyncIoContext. |
| `Fiber` | `include/sluice/async/fiber.hpp:60` | Task state machine (created→runnable→running→waiting→done). Caller-owned. No execution-identity field. |
| `Group` | `include/sluice/async/group.hpp:57` | Unordered task set. Threaded (std::thread/task) or Evented (Fiber/task). Cancel-propagation boundary. |
| `Future<T>` | `include/sluice/async/future.hpp:46` | Single-task awaitable. Producer: complete_with. Consumer: await/cancel. |
| `CancelToken` | `include/sluice/async/cancel.hpp:47` | Cooperative cancel-request state. Idempotent request(). |
| `CancelState` | `include/sluice/async/cancel.hpp:88` | Per-consumer protection + acknowledgement. |
| `AsyncBackend` | `include/sluice/async/async_io_context.hpp:55` | Internal backend boundary (abstract). submit/poll/wait_one/cancel/outstanding. |
| `AsyncIoContext` | `include/sluice/async/async_io_context.hpp:118` | Public L1. Owns backend (unique_ptr). Routes submit/poll/wait_one/cancel. Move-only. |
| `Completion<T>` | `include/sluice/async/completion.hpp:70` | Caller-owned op state. idle→outstanding→ready. Non-movable. |
| `Batch` | `include/sluice/async/batch.hpp:69` | Grouped completions driver over AsyncIoContext. |
| `ThreadPoolBackend` | `include/sluice/async/threadpool_backend.hpp:55` | Portable real backend. One thread per op; workers joined in destructor. |
| `SchedulerWakeHandle` | `include/sluice/async/scheduler.hpp:95` | External wake handle. notify() safe across Scheduler destruction. |
| `EventedWaitPolicy` | `include/sluice/async/evented_wait_policy.hpp:42` | Evented Future wait via Scheduler. Borrows Scheduler. |
| `BlockingIoPool` | `bench/support/blocking_io_pool.cpp` | Bounded OS-thread helper. Not the async runtime. |

### 6.2 Primitive ownership/lifecycle inventory

| Field | Scheduler | Fiber | Group | Future | CancelToken | AsyncIoContext | Completion | ThreadPoolBackend |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Existing role** | Multi-worker Evented scheduler | Task state machine | Unordered task set | Single-task awaitable | Cancel request state | L1 op routing | Op state machine | Portable backend |
| **Construction** | `explicit Scheduler(AsyncIoContext&)` — borrows ctx (`scheduler.hpp:213`) | `Fiber()` or `Fiber(Entry)` (`fiber.hpp:64-65`) | `Group()` or `Group(Scheduler&)` (`group.hpp:61,78`) | `Future()` or `Future(WaitPolicy&)` (`future.hpp:53-54`) | `CancelToken()` (`cancel.hpp:49`) | `explicit AsyncIoContext(unique_ptr<AsyncBackend>, AsyncStats*)` (`async_io_context.hpp:121`) | `Completion()` (`completion.hpp:79`) | `ThreadPoolBackend()` (`threadpool_backend.hpp:57`) |
| **Owner today** | Caller (stack/heap) | Caller/Group | Caller | Caller/Group (shared_ptr) | Caller/Future/Group | Caller | Caller | AsyncIoContext (unique_ptr) |
| **Threads** | Creates/joins workers INSIDE blocking run()/run_live() (`scheduler.cpp:565-579`); worker std::thread objects are run_impl locals, NOT separately ownable | None | Threaded: std::thread/task; Evented: none | None | None | None (backend may) | None | Spawns per-op threads; joined in DESTRUCTOR (`threadpool_backend.hpp:101`; `threadpool_backend.cpp:23-32`) |
| **Drive model** | Caller calls blocking `run()`/`run_live()` | Scheduler drives state machine | Caller calls `await()`/`cancel()` | WaitPolicy drives physical wait | Consumer observes | Caller/batch drives | Backend marks ready | AsyncIoContext drives |
| **Shutdown support** | `global_terminate_` atomic; workers joined in run (`scheduler.cpp:142,565-579`) | make_done (`fiber.hpp:101`) | `cancel()` requests+awaits (`group.hpp:125-128`); destructor drains (Threaded) / fail-fast (Evented) (`group.cpp:110-144`) | `cancel()` requests+awaits (`future.hpp:113-116`) | `request()` idempotent (`cancel.hpp:59`) | Destructor fail-fast if outstanding (`async_io_context.cpp:30-41`) | `reset()` (`completion.hpp:125-129`) | `destroying_` gate (`threadpool_backend.hpp:103`); destructor joins workers (`threadpool_backend.cpp:23-32`) |
| **Outstanding-work contract** | Destruction asserts quiescence: no ACTIVE Select, active_deadline_count_==0, waiting_select_count_==0 (`scheduler.cpp:165-196`) | done is absorbing (`fiber.hpp` state machine) | Evented destructor fail-fast if pending futures (`group.cpp:117-122`) | ready is absorbing (`future.hpp:119-121`) | None | Destruction fail-fast if outstanding Completions (`async_io_context.cpp:30-41`) | Must not destroy while outstanding (`completion.hpp:17-18`) | `outstanding_` decrements at POLL REAP, not syscall completion (`threadpool_backend.cpp:160-163`); destructor joins all workers |
| **Cancellation relationship** | Provides wait-cancellation (cancel_wait); doesn't own task cancellation | Owns per-fiber CancelToken + CancelState (`fiber.hpp` via Group wrapper) | Owns shared CancelToken; cancel-propagation boundary (`group.hpp:114,125-128`) | Owns CancelToken (`future.hpp:89`) | Is the request state | Routes cancel to backend | None | Best-effort cancel (`threadpool_backend.cpp:175-181`) |
| **Move/copy** | Non-copyable, non-movable (`scheduler.hpp:216-219`) | Non-copyable, non-movable (`fiber.hpp:67-70`) | Non-copyable, non-movable (`group.hpp:82-85`) | Non-copyable, non-movable (`future.hpp:56-59`) | Non-copyable, non-movable (`cancel.hpp:51-54`) | Non-copyable, move-only (`async_io_context.hpp:125-133`) | Non-copyable, non-movable (`completion.hpp:84-87`) | Non-copyable (`async_io_context.hpp:58-59`) |
| **Error channel** | Fail-fast (std::terminate) on invariant violation | State machine | Tasks swallow exceptions (`group.hpp:173-178, 251-256` — catch(...) in header, NOT group.cpp) | Result<T> channel | is_requested() | Result from submit; fail-fast on destruction | Result<T> channel | Result from submit |
| **Test-only seams** | `AsyncTestAccess` under `SLUICE_ASYNC_INTERNAL_TESTING` (`scheduler.hpp:1749-2165`) | None | `test_set_tasks_throw_on_nth` under `SLUICE_ASYNC_INTERNAL_TESTING` (`group.hpp:102-109`) | None | None | None | None | `shutting_down_for_test()` (`threadpool_backend.hpp:78`) |
| **Runtime suitability** | **OWN** — core of the Runtime | **WRAP** — owned by Group/Runtime | **OWN** (root Group) — the task admission domain; its group_token() IS the Runtime root cancel | **WRAP** — owned by tasks/Group | **OWN** (root = root Group's token) — runtime-level cancellation | **OWN** — owns backend | **EXCLUDE** — caller-owned | **INJECT** — backend implementation |

### 6.3 Current ownership graph

```text
Caller
├── owns → AsyncIoContext
│         └── owns → AsyncBackend (unique_ptr)
├── owns → Scheduler (borrows AsyncIoContext)
├── owns → Group(Scheduler&)  [Evented root]
│         ├── owns → CancelToken (shared across tasks)
│         ├── owns → EventedWaitPolicy (borrows Scheduler)
│         ├── owns → Future<void> per task (shared_ptr)
│         ├── owns → Fiber per task (unique_ptr)
│         └── owns → Stack per task (unique_ptr<byte[]>)
└── must manually:
      1. call Group::async() to admit
      2. call Group::await() → Scheduler::run_live(1, stop_predicate)
      3. ensure correct destruction order
```

### 6.4 Current lifecycle gaps

| Gap | Evidence | Impact |
| --- | --- | --- |
| No unified start | `Scheduler::run()` blocks caller (`scheduler.hpp:241`); workers joined inside (`scheduler.cpp:565-579`); no non-blocking launch | Cannot start runtime and later stop it from the same thread |
| No admission control | `Group::async()` always admits or throws (`group.hpp:94`); no gate | Cannot reject submission after stop |
| No runtime-level cancellation | Cancellation is per-Group (`group.hpp:114`) or per-Future (`future.hpp:89`) | No single "stop all" signal |
| No startup rollback | No multi-step startup sequence exists | Driver-spawn failure leaves partial state |
| No drain/join separation | `Group::await()` conflates task completion with scheduler driving (`group.cpp:41-91`) | Cannot independently drain work vs. join driver |
| No destructor contract | `Scheduler::~Scheduler()` asserts quiescence (`scheduler.cpp:114-196`); `Group::~Group()` fail-fast (`group.cpp:117-122`); `AsyncIoContext::~AsyncIoContext()` fail-fast (`async_io_context.cpp:30-41`); no single owner enforces sequence | Manual composition errors → std::terminate |

## 7. Architecture alternatives

### Alternative A — Runtime-owned backend

The Runtime creates the backend internally based on configuration.

```text
ApplicationRuntime
├── backend (created internally from config)
├── AsyncIoContext (owns backend)
├── Scheduler (borrows AsyncIoContext)
├── root Group(Scheduler&)
├── root CancelToken (= root Group's group_token())
├── dedicated driver thread
└── control state
```

| Dimension | Analysis |
| --- | --- |
| Construction order | 1. Create backend from config → 2. Create AsyncIoContext → 3. Create Scheduler → 4. Create root Group → 5. Acquire wake handle → 6. state=Constructed |
| Destruction order | Reverse after stop/drain/join/close |
| Backend testability | **POOR** — cannot inject FakeAsyncBackend; deterministic testing requires internal backend creation hooks |
| Partial-start rollback | Must track which steps completed; destroy in reverse |
| Cancellation owner | root Group's group_token() (`group.hpp:114`) |
| Admission owner | root Group, gated by Runtime admission flag |
| Drain authority | Runtime-owned admitted/terminal counts + driver |
| Public exposure | Could expose Scheduler& / Group& accessors |
| Misuse resistance | Backend choice fixed at construction |
| Complexity | Low (all internal) |
| Foundation changes | No scheduler drive-semantics change; one private Fiber-local identity seam + 3 private Runtime seams |
| Compatibility | No effect on existing users |
| Verification | Hard — no deterministic backend injection |

### Alternative B — Backend injected into Runtime

The consumer provides a backend (or a factory) at construction.

```text
RuntimeConfig / RuntimeBuilder
└── injected or factory-created AsyncBackend

ApplicationRuntime
├── AsyncIoContext (owns injected backend)
├── Scheduler (borrows AsyncIoContext)
├── root Group(Scheduler&)
├── root CancelToken (= root Group's group_token())
├── dedicated driver thread
└── control state
```

| Dimension | Analysis |
| --- | --- |
| Construction order | 1. Inject backend → 2. Create AsyncIoContext → 3. Create Scheduler → 4. Create root Group → 5. Acquire wake handle → 6. state=Constructed |
| Destruction order | Reverse of construction after stop/drain/join/close |
| Backend testability | **GOOD** — inject FakeAsyncBackend (deterministic) via existing `AsyncIoContext(unique_ptr<AsyncBackend>)` seam (`async_io_context.hpp:121`) |
| Partial-start rollback | Same as A; injected backend is the first step |
| Cancellation owner | root Group's group_token() |
| Admission owner | root Group, gated by Runtime admission flag |
| Drain authority | Runtime-owned admitted/terminal counts + driver |
| Public exposure | Same as A |
| Misuse resistance | Backend injected; config validated before start |
| Complexity | Medium (injection seam) |
| Foundation changes | No scheduler drive-semantics change; one private Fiber-local identity seam + 3 private Runtime seams |
| Compatibility | No effect on existing users |
| Verification | Good — deterministic backend injection via existing seam |

### Alternative C — Builder + one-shot Runtime with dedicated driver

A `RuntimeBuilder` collects configuration; `build()` validates and returns a
constructed (but not started) Runtime. `start()` is a separate, fallible
transaction that spawns the dedicated driver thread. The driver loops on
`run_live` re-entry. Backend is injected (from B).

```text
RuntimeBuilder
├── backend (injected)
├── worker_count
├── config validators
└── build() → ApplicationRuntime (Constructed, not started, no driver)

ApplicationRuntime (after build)
├── AsyncIoContext
├── Scheduler
├── root Group  (group_token() is root cancel)
├── SchedulerWakeHandle (Runtime-owned)
├── admitted_count / terminal_count (mutex-protected)
├── lifecycle_mutex / runtime_cv / control_epoch
├── 3 atomic stop-predicate snapshots
├── Fiber-local execution tag (per-Fiber, set/cleared by task wrapper)
└── start() → spawns driver → driver re-enters run_live
```

| Dimension | Analysis |
| --- | --- |
| Construction order | 1. Builder collects config → 2. `build()` validates + constructs owned objects + acquires wake handle → 3. Returns Runtime in `Constructed` state (no driver) |
| Destruction order | Per-state (§10): Constructed/StartFailed destroy components normally; Stopped already destroyed them via join owner |
| Backend testability | **GOOD** — same injection seam as B |
| Partial-start rollback | `start()` is a transaction; rollback at each step; driver-spawn failure leaves no surviving thread |
| Cancellation owner | root Group's group_token() |
| Admission owner | root Group, gated by Runtime admission flag; opens only after startup commit |
| Drain authority | Runtime-owned admitted/terminal counts + driver re-entry loop; outstanding()==0 verified between run_live invocations |
| Public exposure | Builder exposes config; Runtime exposes lifecycle ops |
| Misuse resistance | **BEST** — separation of construct/start prevents use-before-start; builder validates config; Fiber-local tag detects worker calls |
| Complexity | Medium-high (driver thread + re-entry loop + dual wake + Fiber-local seam) |
| Foundation changes | **No change to Scheduler drive semantics.** Four private PROPOSED seams: `runtime_lifetime_fail_fast`, Fiber-local execution-identity, `RuntimeTaskTerminalGuard`, `recompute_task_set_terminal_locked`. |
| Compatibility | No effect on existing users |
| Verification | **BEST** — builder enables config validation tests; start() transaction enables rollback tests; driver model enables deterministic drain/join tests |

### 7.4 Decision matrix

| Dimension | A: Runtime-owned | B: Injected | C: Builder + one-shot + driver |
| --- | --- | --- | --- |
| Backend testability | POOR | GOOD | GOOD |
| Misuse resistance | MEDIUM | MEDIUM | **BEST** |
| Startup rollback clarity | MEDIUM | MEDIUM | **BEST** |
| Config validation | MEDIUM | MEDIUM | **BEST** |
| Thread model clarity | (same driver model) | (same driver model) | **BEST** |
| Complexity | MEDIUM | MEDIUM | MEDIUM-HIGH |
| Foundation changes | 4 private seams | 4 private seams | 4 private seams |
| Compatibility | None | None | None |
| Verification | HARD | GOOD | **BEST** |
| API cleanliness | MEDIUM | MEDIUM | **BEST** |

### 7.5 Preferred architecture

**Alternative C (Builder + one-shot Runtime with dedicated driver thread and
injected backend) is preferred.**

Rationale:
- **Testability** is mandatory. The existing test infrastructure relies on
  `FakeAsyncBackend` injection (e.g. `examples/async_foundation_quickstart.cpp:17-28`).
  Alternative A cannot inject deterministic backends without internal hooks that
  would weaken production guarantees.
- **Misuse resistance** is a hard constraint. The construct/start separation
  prevents use-before-start; the Fiber-local execution tag lets the Runtime
  deterministically detect (and reject) lifecycle calls made from its own tasks.
- **Startup rollback** is a hard constraint. A transactional `start()` with
  rollback is the cleanest expression; the driver thread is the real background
  resource whose partial-spawn must roll back.
- **Verification** is a hard constraint. The builder pattern enables
  config-validation tests; the one-shot lifecycle enables deterministic
  state-transition tests; the driver model makes drain/join independently
  testable.

### 7.6 Alternative D — Caller-driven single-worker (rejected as default)

The caller's thread drives `run_live(1, ...)` inline (exactly like `Group::await`
today). No background driver thread. `worker_count` is fixed at 1.

**Rejected as the default Application Runtime** because it makes `start()`
non-operational (nothing runs until `drain()` is called), removes parallelism,
and collapses `drain()` into execution. It may be documented as a future
deterministic/manual variant, but it is not the E16 default.

## 8. Ownership graph (preferred)

```text
ApplicationRuntime
├── owns → AsyncIoContext (unique_ptr)
│         └── owns → AsyncBackend (unique_ptr, injected)
├── owns → Scheduler (unique_ptr, borrows *async_io_context)
├── owns → root Group (unique_ptr, Group(*scheduler))
│         ├── owns → CancelToken (group_token())  ← THE Runtime root cancel
│         ├── owns → EventedWaitPolicy (borrows *scheduler)
│         ├── owns → Future<void> per task (shared_ptr)
│         ├── owns → Fiber per task (unique_ptr; carries the execution tag)
│         └── owns → Stack per task (unique_ptr<byte[]>)
├── owns → SchedulerWakeHandle (Runtime-owned, via Scheduler::make_wake_handle, scheduler.hpp:909)
├── owns → dedicated driver thread (std::thread — the ONLY thread the Runtime spawns/joins)
├── owns → control state, all under lifecycle_mutex:
│         ├── state                (RuntimeState)
│         ├── admission_open       (bool)
│         ├── stop_requested       (bool, monotonic)
│         ├── admitted_count       (size)
│         ├── terminal_count       (size)
│         ├── control_epoch        (uint64)
│         └── join_state           ({NotStarted, InProgress, JoinedAndClosed})
├── owns → 3 lock-free atomic stop-predicate snapshots:
│         ├── driver_exit_requested
│         ├── task_set_terminal_snapshot
│         └── fatal_snapshot
├── owns → runtime_cv (signaled AFTER releasing lifecycle_mutex)
└── owns → RuntimeConfig (worker_count, backend config)
```

**Single cancel authority (P1-03 fix).** The Runtime does **not** create an
independent second root `CancelToken`. The authoritative cancellation state is
`root_group.group_token()` (`group.hpp:114`). `request_stop()` calls
`root_group.group_token().request()`. The root Group already owns its token
(`group.hpp:201`) and passes it to each task (`group.hpp:174,251`).

**Runtime owns its own counts; never calls Group::await() (P1-03 fix).** The
Runtime maintains `admitted_count` / `terminal_count` itself. Each submitted
task is wrapped so that its terminal exit publishes `terminal_count++` via the
`RuntimeTaskTerminalGuard`. The driver's stop predicate keys off the Runtime
counts, **not** `Group::size()` (which includes completed-but-unreaped tasks,
`group.hpp:132-135`, and so cannot express remaining non-terminal work). The
Runtime **never calls `Group::await()` while the driver exists** — `await()` would
call `run_live(1,...)` itself (`group.cpp:57-72`), violating "only the driver
drives the Scheduler" and forcing single-worker. After Runtime shutdown, `Group`
destruction reaps terminal Futures/Fibers/stacks non-blockingly
(`group.cpp:123-128`).

**Scheduler worker threads are NOT owned by the Runtime.** They are transient
inside each `run_live` invocation (`scheduler.cpp:565-579`). The Runtime joins
only the driver; that transitively proves the Scheduler worker threads were
joined inside the last `run_live`. **Backend worker threads** are joined by the
backend destructor (`threadpool_backend.cpp:23-32`), which the join owner runs
during resource close.

## 9. Construction order

```text
1. Consumer creates RuntimeBuilder
2. Consumer calls builder.backend(std::move(backend))
3. Consumer calls builder.workers(N)
4. Consumer calls builder.build() → ApplicationRuntime
   a. Validate config (worker_count >= 1, backend != nullptr)
   b. Construct AsyncIoContext (owns backend)
   c. Construct Scheduler (borrows AsyncIoContext)
   d. Construct root Group (borrows Scheduler)
   e. Acquire SchedulerWakeHandle (Scheduler::make_wake_handle)
   f. state = Constructed; admission = CLOSED; stop_requested = false
   g. No driver thread exists yet
5. Consumer calls runtime.start()
   a. LOCKED transition under lifecycle_mutex:
      - if stop_requested: return IoError::canceled (stay Constructed; remember stop)
      - if state != Constructed: return IoError::invalid_state
      - state = Starting
   b. Spawn driver thread (may throw std::system_error → rollback to StartFailed)
   c. Driver records driver_started, then waits on runtime_cv while state == Starting
      (startup barrier — driver may NOT enter run_live yet)
   d. LOCKED startup commit under lifecycle_mutex:
      - state = Running
      - admission_open = (stop_requested ? false : true)
      - recompute_task_set_terminal_locked()
      - control_epoch++
   e. Release lifecycle_mutex
   f. Wake driver (runtime_cv.notify_all + scheduler_wake_handle.notify)
   g. Driver observes state == Running → enters run_live re-entry loop
   h. Return Result success (or canceled/invalid_state per the locked checks)
```

All transitions are **locked transitions under `lifecycle_mutex`**, not lock-free
CAS. The linearization point of startup is the locked compound commit at step 5d.
Evidence for construction order:
- `AsyncIoContext` must be constructed before `Scheduler` (Scheduler borrows it: `scheduler.hpp:213`).
- `Scheduler` must be constructed before `Group(Scheduler&)` (Group borrows it: `group.hpp:78`).
- `Group` must be constructed before tasks are admitted (`group.hpp:94`).

## 10. Destruction order

Destruction safety is defined **per state** (resolves the two-meanings-of-Stopped
conflict):

| State | Components | Driver | Admitted work | Outstanding I/O | Destructor behavior |
| --- | --- | --- | --- | --- | --- |
| `Constructed` | may exist | none | none | none | Destroy components normally (AsyncIoContext→...); safe |
| `StartFailed` | may exist (rolled back) | none (joined on rollback) | none | none | Destroy components normally; safe |
| `Stopped` | **already destroyed** by join owner | joined | none | none | No-op teardown (components gone); safe |
| any other | — | — | — | — | `runtime_lifetime_fail_fast()` (PROPOSED) |

**Every transition INTO `Stopped` performs resource close first** (the join owner
does it — §17). Therefore `Stopped` means: driver joined AND all execution
resources destroyed (Group/Scheduler/AsyncIoContext/backend gone; backend
workers joined via backend destruction). Diagnostics MUST be snapshotted before
resource close, because components no longer exist at `Stopped`.

`request_stop()` in `Constructed` does **not** transition to `Stopped` — it
remembers `stop_requested` and remains `Constructed`. A later `start()` observes
`stop_requested`, returns `canceled`, and the object eventually destructs as
`Constructed`. If `start()` is never called, the object destructs safely as
`Constructed`.

Reverse destruction (for Constructed/StartFailed): root Group → Scheduler →
AsyncIoContext → (backend freed inside AsyncIoContext).

Evidence:
- `Group::~Group()` fail-fast if Evented futures pending (`group.cpp:117-122`).
- `Scheduler::~Scheduler()` asserts quiescence (`scheduler.cpp:165-196`).
- `AsyncIoContext::~AsyncIoContext()` fail-fast if outstanding (`async_io_context.cpp:30-41`).
- `~ThreadPoolBackend()` joins backend workers (`threadpool_backend.cpp:23-32`).

## 11. Lifecycle state machine

### 11.1 States

| State | Meaning |
| --- | --- |
| `Constructed` | Built but not started. Admission closed. No driver. `stop_requested` may be remembered. |
| `Starting` | `start()` in progress. Driver spawned but waiting at startup barrier. Admission not yet open. |
| `Running` | Started. Admission open (unless stop won the race at commit). Driver active in run_live re-entry loop. |
| `Stopping` | `request_stop()` committed. Admission closed. Root cancellation published. |
| `Draining` | `drain()` in progress. Waiting for drain_complete. |
| `Stopped` | Driver joined AND all execution resources destroyed. Safe to destroy (no-op). |
| `StartFailed` | `start()` failed. Rolled back. No surviving driver. Components may exist; safe to destroy. |
| `Fatal` | Invariant violation detected. Process should terminate. |

### 11.2 State diagram

```text
Constructed ──start()──→ Starting ──commit──→ Running
     │                      │
     │ remembers stop       ├──construct throw──→ StartFailed
     │ (stays Constructed)  │
     │                      └──stop wins pre-commit──→ Stopped (rollback + close)
     │
     │                 Running ──request_stop()──→ Stopping
     │                                              │
     │                                         drain()──→ Draining
     │                                                       │
     │                                                  join()──→ Stopped (join + close)
     │                                                            │
     │                                                      destructor (safe, no-op)
     │
     │   Any state ──invariant violation──→ Fatal
     │                                         │
     │                                    std::terminate
     │
     │   StartFailed ──destructor──→ safe (components destroyed normally)
     │   Constructed ──destructor──→ safe (components destroyed normally)
     │   Stopped     ──destructor──→ safe (no-op; components already gone)
     │   Any other state ──destructor──→ fail-fast
```

### 11.3 Legal transition table

| From → To | Trigger | Guard |
| --- | --- | --- |
| Constructed → Starting | `start()` | state == Constructed, !stop_requested (else return canceled, stay Constructed) |
| Starting → Running | startup commit (locked) | driver spawned; commit under lifecycle_mutex |
| Starting → StartFailed | construction throw | rollback completed |
| Starting → Stopped | stop wins pre-commit | rollback + resource close completed; start() returns canceled |
| Running → Stopping | `request_stop()` (locked compound commit) | state == Running |
| Stopping → Draining | `drain()` | state == Stopping |
| Draining → Stopped | `join()` (drain_complete reached) | join owner joins driver + closes resources |
| Stopping → Stopped | `shutdown()` (drain+join composition) | drain_complete reached; join owner closes |
| Any → Fatal | invariant violation | none (fail-fast) |

All transitions are **locked transitions under `lifecycle_mutex`**, not CAS.

### 11.4 Illegal-operation behavior

| Operation | State | Behavior |
| --- | --- | --- |
| `start()` | Starting, Running, Stopping, Draining, Stopped | **returns invalid_state** |
| `start()` | StartFailed, Fatal | **returns invalid_state** |
| `start()` | Constructed with stop_requested remembered | **returns canceled** (stays Constructed) |
| `submit()` | Constructed, Starting | **returns invalid_state** (not started) |
| `submit()` | Stopping, Draining, Stopped, StartFailed | **returns invalid_state** (admission closed) |
| `request_stop()` | Constructed | **remembers stop_requested, stays Constructed, returns success** |
| `request_stop()` | Starting | **remembers stop_requested; if commit not yet happened, start() rolls back to Stopped and returns canceled** |
| `request_stop()` | StartFailed, Stopped | **idempotent** (no-op, returns success) |
| `drain()` | Constructed, Starting, Running | **returns invalid_state** (must request_stop first) |
| `join()` | Constructed | **returns invalid_state** (no driver to join) |
| `join()` | before drain_complete | **returns invalid_state** |
| destructor | Running, Starting, Stopping, Draining | **fail-fast** (`runtime_lifetime_fail_fast`, PROPOSED) |
| destructor | Constructed, StartFailed, Stopped | **safe** (per §10) |

### 11.5 Idempotent-operation behavior

| Operation | Idempotent behavior |
| --- | --- |
| `request_stop()` | Second call returns success (no-op); monotonic stop_requested |
| `drain()` | Second call returns immediately if already drain_complete |
| `join()` | Second call returns immediately if join_state == JoinedAndClosed |
| `shutdown()` | Composition; idempotent because each component is idempotent |

### 11.6 Concurrency behavior when operations race

| Race | Resolution |
| --- | --- |
| `request_stop()` races with `submit()` | Locked compound commit: stop sets admission_open=false in the same critical section; submit's reservation checks admission_open under the same mutex. Linearization point = the locked commit / reservation. Loser rejects. |
| `request_stop()` races with `start()` pre-commit | request_stop records stop_requested; start observes it at the locked startup commit, rolls back, returns canceled. |
| `request_stop()` races with `drain()` | `request_stop()` closes admission first (locked); `drain()` only legal in Stopping/Draining. |
| `drain()` races with `join()` | `drain()` must complete (drain_complete) before `join()` is legal; `join()` returns invalid_state before drain_complete. |
| concurrent `join()` / `shutdown()` | One elected join owner (join_state NotStarted→InProgress→JoinedAndClosed); others wait on runtime_cv for JoinedAndClosed. |
| concurrent `request_stop()` | Locked compound commit; monotonic stop_requested; all callers return success. |

## 12. Operation/state matrix

| Operation | Constructed | Starting | Running | Stopping | Draining | Stopped | StartFailed | Fatal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `start()` | **allowed** (or canceled if stop_requested) | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state |
| `submit()` | invalid_state | invalid_state | **allowed** | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state |
| `request_stop()` | remember+stay (success) | record; rollback→Stopped | **allowed** (compound commit→Stopping) | idempotent | idempotent | idempotent | idempotent | undefined |
| `drain()` | invalid_state | invalid_state | invalid_state | **allowed** | idempotent | idempotent | idempotent | undefined |
| `join()` | invalid_state | invalid_state | invalid_state | invalid_state (before drain_complete) | invalid_state (before drain_complete) / **allowed** (drain_complete) | idempotent | idempotent | undefined |
| `shutdown()` | remember+stay; later start returns canceled | record; rollback+close→Stopped | **allowed** (stop+drain+join) | **allowed** (drain+join) | **allowed** (join) | idempotent | idempotent | undefined |
| destructor | **safe** (destroy components) | fail-fast | fail-fast | fail-fast | fail-fast | **safe** (no-op) | **safe** (destroy components) | undefined |

**Caller-identity footnote (Fiber-local execution identity, PROPOSED).**
`drain()`, `join()`, and `shutdown()` return `IoError::invalid_state` if invoked
from a task owned by the SAME Runtime (detected via the Fiber-local execution
tag — §21). `request_stop()` is worker-safe and may be called from a Runtime
task. Only the destructor (no Result channel) fail-fasts. This detection is
deterministic and enforceable; it is NOT an unchecked precondition.

## 13. Admission contract

### 13.1 Linearization rule

**A task is successfully admitted when its reservation succeeds under
`lifecycle_mutex`: admission_open is verified true AND admitted_count is
incremented.** The reservation is the linearization point of stop-vs-submit.

Concretely (`submit()`):

```text
LOCK lifecycle_mutex:
    if !admission_open:
        UNLOCK; return IoError::invalid_state
    admitted_count++
    recompute_task_set_terminal_locked()
UNLOCK
try:
    root_group.async(runtime_wrapper)
catch (exception from Future/stack/Fiber alloc, init_fiber, vector push_back):
    LOCK lifecycle_mutex:
        admitted_count--
        recompute_task_set_terminal_locked()
        control_epoch++
    UNLOCK
    dual-wake (runtime_cv.notify_all + scheduler_wake_handle.notify)
    return map_to_Result_error
```

All throwable steps in `Group::async_evented` (Future/stack/Fiber allocation,
`init_fiber`, vector `push_back`) occur **before** `Scheduler::spawn`
(`group.hpp:264-282`), so on exception the user task body has NOT executed. The
reservation+rollback protocol closes both failure modes:
- "admitted a task that never runs → drain waits forever" (rollback decrements
  admitted_count and recomputes the snapshot);
- "terminal_count > admitted_count" (the reservation is decremented before
  control returns to the caller).

### 13.2 Admission answers

| Question | Answer |
| --- | --- |
| When has a task been successfully admitted? | When the reservation succeeds (admission_open verified AND admitted_count incremented, under lifecycle_mutex). |
| When has an I/O operation been successfully admitted? | I/O admission is separate (Completion-based, `AsyncIoContext::submit_*`). Runtime admission gates task admission, not I/O ops. A task submits I/O after being admitted. |
| Is "placed in an intermediate queue" admission? | No. Admission is the reservation. The Fiber's placement on the scheduler's runnable queue (via `Group::async` → `Scheduler::spawn`) is a scheduler internal, not admission. |
| Is "worker started" admission? | No. The driver is started in `start()`. Task admission is per-submit reservation. |
| What happens when stopping races with submission? | Locked compound commit / reservation. Winner admitted, loser rejected with `IoError::invalid_state`. Deterministic. |
| Can a Running task submit child work after `request_stop()`? | E16 v1 has NO structured child submission. Any external capture of `ApplicationRuntime&` by a task body is treated as ordinary concurrent external `submit()`, subject to the same admission gate; after `request_stop()` closes admission, such a submit is rejected. See §13.4. |
| Can it submit during `drain()`? | `drain()` is only legal after `request_stop()` closed admission; any external submit observing closed admission is rejected. |
| Are child tasks part of the original drain set? | N/A — no structured child submission in E16 v1. |
| Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** — see §26 (Q1). |
| Does cancellation precede closing admission or follow it? | In the locked compound commit, `stop_requested=true` AND `admission_open=false` are set together; root cancellation is published via `group_token().request()` as part of the same logical stop. |
| How is rejected submission reported? | `submit()` returns `Result` with `IoError::invalid_state`. |
| Can rejection run user code? | No. Rejection returns before the reservation; no task body runs. |
| Can admission failure leave observable partial ownership? | No. Reservation rollback restores admitted_count and recomputes the snapshot under lifecycle_mutex. |

### 13.3 Post-commit open (T4 trace closure)

Admission opens **only after** the startup commit (§9 step 5d) AND only if
`!stop_requested` at commit. Therefore the trace "admission opens, then commit
fails / stop wins" is **impossible**: the commit and the admission-open are the
same locked compound transition. If stop wins pre-commit, admission never opens,
the driver is joined on rollback, and `start()` returns `canceled`.

### 13.4 Child submission — explicitly NOT supported in E16 v1

`TaskFn = void(CancelToken&)` (matching `group.hpp:89`) gives no child-spawn
capability. Tasks cannot spawn Runtime-owned child tasks. Any external capture of
`ApplicationRuntime&` by a task body is treated as ordinary concurrent external
`submit()`. A restricted `TaskContext` with `spawn()` is documented as a future
extension, out of scope for E16-A0. This removes the contradiction between
"child tasks terminal" guarantees and a `CancelToken&`-only signature.

## 14. Cancellation contract

- The authoritative Runtime root cancellation state is **`root_group.group_token()`**
  (`group.hpp:114`). The Runtime does NOT create an independent second token.
- `request_stop()` publishes root cancellation via `root_group.group_token().request()`.
- The root token is propagated to tasks by the root Group (it passes `&token_`
  to each task, `group.hpp:174,251`).
- Cancellation is **cooperative** (matching the existing model, `cancel.hpp:14`):
  tasks observe the token at cancel points (`check_cancel`, `cancel.hpp:147`).
- Cancellation is **not** an unconditional escape hatch: a task that does not
  observe cancellation can prevent `drain()` from returning (matching
  `group.hpp:69-76` semantics).
- The root Group is a cancel-propagation boundary: task wrappers swallow
  exceptions (`group.hpp:173-178, 251-256`).

## 15. Stop contract

`request_stop()` is `noexcept`, thread-safe, and legal + idempotent + non-lossy
in every non-Fatal state. It never fail-fast merely because `start()` is in
progress. `stop_requested` is monotonic, explicit, synchronized Runtime state.

**Four-case linearization** (locked compound commit, then release, then dual-wake):

1. **Constructed**: record `stop_requested=true`; stay `Constructed`; return
   success. A later `start()` observes it, returns `canceled`, and the object
   eventually destructs as `Constructed`.

2. **Starting, before startup commit**: record `stop_requested=true`. The
   startup commit observes it: `admission_open` stays false; `start()` rolls
   back (joins the driver if spawned, full resource close), transitions to
   `Stopped`, and returns `canceled`. The Runtime need not publish an externally
   observable `Running` state.

3. **Running** (post-commit): locked compound commit — `stop_requested=true`;
   `admission_open=false`; `state=Stopping`; `recompute_task_set_terminal_locked()`;
   `control_epoch++`. Release `lifecycle_mutex`. Then publish root cancellation
   via `root_group.group_token().request()`. Then dual-wake
   (`runtime_cv.notify_all` + `scheduler_wake_handle.notify`).

4. **Stopping, Draining, Stopped, StartFailed**: idempotent no-op; return success.

`request_stop()` does **not**:
- Block (it is non-blocking).
- Drain (that is `drain()`).
- Join threads (that is `join()`).

Evidence: `SchedulerWakeHandle::notify()` is safe across Scheduler destruction
and is a no-op if the Scheduler is dead (`scheduler.hpp:104-109`).

## 16. Drain contract

`drain()` is legal **only in `Stopping` or `Draining`**. In `Running` it returns
`IoError::invalid_state` (the caller must `request_stop()` first, which
atomically closes admission). This makes the drain set monotonically shrinking.

`drain()`:
1. Sets the drain intent, dual-wakes the driver (release-then-notify).
2. Blocks on `runtime_cv` until **`drain_complete`** is published.

### 16.1 task_set_terminal vs drain_complete (breaks the circular definition)

Two distinct concepts:

- **`task_set_terminal_snapshot`** (lock-free atomic, recomputed under
  `lifecycle_mutex` by `recompute_task_set_terminal_locked()`):
  `admission_open==false AND admitted_count==terminal_count`. This is **only a
  Scheduler invocation-exit hint**, not proof that Group Futures are terminal.
- **`drain_complete`** (derived Runtime state, published by the driver
  **between** `run_live` invocations): `task_set_terminal_snapshot==true AND
  AsyncIoContext::outstanding()==0`.

The Scheduler stop predicate (called under `global_mtx_` at the MW-S3 boundary,
`scheduler.cpp:856-857`) reads ONLY three lock-free atomics — `fatal_snapshot`,
`driver_exit_requested`, `task_set_terminal_snapshot` — and **never** reads
`drain_complete` (would be circular) and **never** calls `outstanding()` (lock
hazard: `AsyncIoContext::outstanding()` takes `access_mtx_`,
`async_io_context.cpp:150-153`; `ThreadPoolBackend::outstanding()` takes `mtx_`,
`threadpool_backend.cpp:183-186`).

The driver loop, **between** `run_live` invocations (outside any Scheduler lock),
checks `AsyncIoContext::outstanding()`. If
`task_set_terminal_snapshot && outstanding()==0`, it publishes `drain_complete`
and signals `runtime_cv`; otherwise it re-enters `run_live` to poll/reap
outstanding ops (`outstanding_` decrements at poll reap, not syscall completion,
`threadpool_backend.cpp:160-163`).

### 16.2 recompute_task_set_terminal_locked

```cpp
// PROPOSED — private Runtime implementation detail
void recompute_task_set_terminal_locked() noexcept {
    task_set_terminal_snapshot.store(
        !admission_open && admitted_count == terminal_count,
        std::memory_order_release);
}
```

Called under `lifecycle_mutex` on **every** mutation of `admission_open`,
`admitted_count`, or `terminal_count`:
- `request_stop()` closing admission;
- admission reservation;
- admission rollback (decrement on `Group::async` throw);
- terminal guard (`terminal_count++`);
- startup rollback;
- Fatal transition.

### 16.3 Throw-safe terminal accounting (RuntimeTaskTerminalGuard)

The Runtime task wrapper guarantees `terminal_count++` exactly once even if the
user `TaskFn` throws. PROPOSED shape:

```cpp
// PROPOSED — private Runtime implementation detail
{
    RuntimeTaskTerminalGuard guard{runtime};  // noexcept RAII; sets Fiber tag
    task(token);   // user TaskFn; may throw
}  // guard destructor: noexcept, exactly-once
```

The guard's destructor (under `lifecycle_mutex`): `terminal_count++`;
`recompute_task_set_terminal_locked()`; `control_epoch++`; release; dual-wake.
It runs on both normal-return and throw (RAII runs on stack unwind).

Three distinct terminal notions:
- `terminal_count` — user TaskFn has left (guard fires);
- Group Future terminal — Group wrapper `complete_with` (`group.hpp:251-258`)
  fires after the Runtime wrapper returns;
- `drain_complete` — driver confirms at invocation boundary (§16.1).

### 16.4 Bridge proof (Runtime terminal ⇒ Group Future terminal ⇒ drain_complete)

The Group wrapper completes its Future via `complete_with` (`group.hpp:251-258`)
**after** the Runtime task wrapper returns. `run_live` waits for its Scheduler
workers before returning (`scheduler.cpp:565-579`). Therefore when the driver
reaches an invocation boundary, Group Future publication for any returned task
has already completed. So `task_set_terminal_snapshot` (Runtime counts) being
true implies Group Futures are terminal; the driver's additional
`outstanding()==0` check then advances to `drain_complete`.

### 16.5 Drain guarantees

At `drain()` return (`drain_complete` reached):
- All admitted task bodies have exited (terminal_count == admitted_count).
- All admitted asynchronous I/O is no longer outstanding (`outstanding()==0`).
- The root Group's Futures for admitted tasks are terminal.

`drain()` does **not** guarantee:
- Worker threads joined (that is `join()`); the driver is still alive.
- `Completion<T>` objects destroyed (caller-owned; but no un-terminal backend op
  remains).

`drain()` drives the Scheduler **transitively** via the driver — the drain
calling thread does NOT call `run_live`.

**Queue emptiness alone is not proof of drain** (task prompt §12).

## 17. Join contract

`join()` is legal **only after `drain_complete`**. It returns `IoError::invalid_state`
if invoked from a task owned by the same Runtime (Fiber-local tag) or before
`drain_complete`.

`join()` is the **terminal close operation**. The elected join owner performs
the entire tail:

```text
join():
    LOCK lifecycle_mutex:
        require drain_complete (else invalid_state)
        require not from a Runtime task (Fiber-local tag, else invalid_state)
        if join_state == JoinedAndClosed: UNLOCK; return success
        if join_state == InProgress: wait on runtime_cv for JoinedAndClosed; return success
        join_state = InProgress (this caller is the owner)
    UNLOCK
    set driver_exit_requested snapshot
    dual-wake (release-then-notify)
    driver stop predicate observes driver_exit_requested → returns true
    run_live returns → Scheduler worker threads joined inside (scheduler.cpp:565-579)
    join driver thread
    snapshot diagnostics (components still exist here)
    resource close:
        destroy root Group
        destroy Scheduler
        destroy AsyncIoContext → destroys backend
            → ~ThreadPoolBackend joins backend workers (threadpool_backend.cpp:23-32)
    LOCK lifecycle_mutex:
        state = Stopped
        join_state = JoinedAndClosed
        control_epoch++
    UNLOCK
    notify_all (runtime_cv)
    return success
```

`join()` return implies: **driver joined AND Scheduler invocation workers joined
AND backend destroyed AND backend workers joined AND Runtime == Stopped.**

`shutdown()` composes `request_stop() + drain() + join()`; concurrent
`shutdown()`/`join()` callers all route through the same `join_state` — one
owner, others wait for `JoinedAndClosed`. No two callers ever touch the driver
or resources simultaneously.

This is cleaner than placing resource close in a hidden stage between `join()`
and `Stopped` with no public API: there is no "join returned but Stopped not
reached" window in which backend threads survive.

## 18. Destructor contract

**Chosen contract: B — explicit shutdown required; destructor validates and
fail-fast on misuse.** Per-state safety is defined in §10.

The destructor:
1. Checks `state`:
   - `Constructed` or `StartFailed`: destroy components normally (reverse order).
   - `Stopped`: no-op (components already destroyed by the join owner).
   - Any other state (`Running`, `Starting`, `Stopping`, `Draining`):
     **fail-fast** via `runtime_lifetime_fail_fast()` (PROPOSED).
2. The safe-state predicate is **not** state alone — it is the conjunction:
   `state ∈ {Constructed, StartFailed, Stopped}` AND (for Constructed/StartFailed)
   no joinable driver AND no active `run_live` AND `admitted==terminal` AND
   `outstanding()==0`. For Stopped the components are already gone.

Analysis of alternatives:

| Alternative | Hidden blocking | Error reporting | Deadlock risk | Outstanding I/O | Verdict |
| --- | --- | --- | --- | --- | --- |
| A: automatic stop+drain+join | **YES** — destructor blocks | None (no Result) | Risk if called from worker | Stranded | REJECTED |
| B: explicit shutdown + fail-fast | None (shutdown blocks) | shutdown returns Result | None | Handled by shutdown | **CHOSEN** |
| C: bounded best-effort | None | None | Risk | Stranded | REJECTED |

Rationale for B:
- Matches existing contracts: `AsyncIoContext::~AsyncIoContext()` fail-fast
  (`async_io_context.cpp:30-41`), `Group::~Group()` fail-fast
  (`group.cpp:117-122`), `Scheduler::~Scheduler()` asserts quiescence
  (`scheduler.cpp:165-196`).
- Hidden blocking in a destructor is a known anti-pattern (AGENTS.md §7:
  "Destructors must not invent unreportable I/O success").
- `shutdown()` returns `Result`, enabling error reporting.
- Fail-fast on misuse prevents silent resource leaks.

### 18.1 runtime_lifetime_fail_fast (PROPOSED private seam)

Production currently has **no generic, scope-agnostic fail-fast entry point**.
All 7 existing entries are subsystem-bound (`fail_fast.hpp:31-129`):
`async_mutex_lock_fail_fast`, `select_timer_pump_active_fail_fast`,
`select_multi_group_event_stage_fail_fast`, `select_invariant_fail_fast`,
`group_lifetime_fail_fast`, `evented_admission_fail_fast`,
`async_context_outstanding_fail_fast`.

The design proposes a new private Runtime-specific entry:

```cpp
// PROPOSED — NOT AN EXISTING API. Private implementation seam.
namespace sluice::app::detail {
[[noreturn]] void runtime_lifetime_fail_fast() noexcept;
}
```

Required contract:
- `[[noreturn]] noexcept`;
- no allocation, locking, I/O, dynamic formatting, or recovery;
- deterministic in Debug and Release;
- ultimately calls `std::terminate()` (matching `fail_fast.cpp:16-62`).

It follows the existing subsystem-specific pattern rather than reusing
`group_lifetime_fail_fast` or another semantically unrelated entry. It is NOT a
public API and does not authorize implementation; it is listed as an ADR
consequence.

## 19. Restartability

**Decision: One-shot lifecycle.**

```text
Constructed → Running → Stopped
```

A Stopped Runtime may **not** be restarted. Any restart capability requires:
backend reconstruction, Scheduler reconstruction, driver generation, old wake
handles, old task handles, cancellation generations, Completion reuse, statistics
reset, stale references, failed previous shutdown — all out of scope for E16.

Evidence: `Scheduler` is non-movable, non-copyable (`scheduler.hpp:216-219`).
Reconstruction would require destroying and recreating the Scheduler, which
requires the borrowed AsyncIoContext to remain valid and quiescent. Additionally,
under the chosen model, by `Stopped` the components have already been destroyed
(§10), so restart would require rebuilding them from scratch.

## 20. Failure and rollback model

### 20.1 Startup as a transaction

`start()` is a fallible transaction. Each step either succeeds (and is recorded
as completed) or fails (and triggers rollback of all previously completed steps).

### 20.2 Startup sequence and rollback points

| Step | Action | Failure mode | Rollback |
| --- | --- | --- | --- |
| 1 | Locked check: state==Constructed, !stop_requested | stop_requested remembered | Return `canceled`; stay Constructed; no objects created |
| 2 | state = Starting (locked) | — | — |
| 3 | Spawn driver thread | `std::system_error` (thread creation) | state = StartFailed; no driver to join (joinable==false verified); return error |
| 4 | Driver waits at startup barrier; stop may arrive | stop wins pre-commit | Record stop_requested; commit observes it; admission stays closed; join driver; full resource close; state = Stopped; start() returns `canceled` |
| 5 | Locked startup commit (state=Running, admission_open iff !stop_requested, recompute snapshot, control_epoch++) | Cannot fail (locked compound commit) | — |
| 6 | Release + wake driver | Cannot fail | — |

### 20.3 Failure-point analysis

| Failure point | Returned error | Resulting state | Threads joined | Objects destroyed | Retry permitted | Admission ever opened | Callbacks ran |
| --- | --- | --- | --- | --- | --- | --- | --- |
| stop_requested remembered in Constructed | `canceled` | Constructed | None | None | Yes (re-build) | No | No |
| Driver spawn failure | `backend_error` (or impl-defined) | StartFailed | None (joinable==false) | None | Yes (re-build) | No | No |
| Stop wins pre-commit | `canceled` | Stopped | Driver (if spawned) | Full close | No (one-shot) | No | No |

**No partially started background driver may survive a failed `start()`.** The
driver IS the background worker that `start()` creates; on spawn failure
`joinable()==false` is verified, and on stop-pre-commit rollback the driver is
joined. This guarantee is non-tautological (the F-2 finding in the prior design
was that "no worker may survive" was vacuous because construction spawned no
workers — under this design `start()` really does spawn the driver).

### 20.4 Admission reservation rollback

See §13.1. On `Group::async` throw, `admitted_count--` and
`recompute_task_set_terminal_locked()` run under `lifecycle_mutex` before
`control_epoch++` and dual-wake.

### 20.5 Throw-safe terminal guard

See §16.3. The `RuntimeTaskTerminalGuard` ensures `terminal_count++` happens
exactly once even if the user TaskFn throws (RAII runs on unwind), so a thrown
task can never leave `terminal_count` short and block `drain()` forever.

## 21. Proposed API alternatives

Every block in this section is **PROPOSED — NOT AN EXISTING API**.

### 21.1 API Sketch 1 — Factory + explicit start

```cpp
// PROPOSED — NOT AN EXISTING API
namespace sluice::async {

struct RuntimeConfig {
    std::unique_ptr<AsyncBackend> backend;  // injected
    unsigned worker_count = 1;
};

class ApplicationRuntime {
public:
    // Construction
    static Result<ApplicationRuntime> create(RuntimeConfig config);

    // Lifecycle
    Result<void> start();                    // may return canceled / invalid_state
    Result<void> submit(TaskFn task);        // admission-gated; invalid_state if closed
    void request_stop() noexcept;            // worker-safe; legal all non-Fatal states
    Result<void> drain();                    // invalid_state if from a Runtime task or in Running
    Result<void> join();                     // terminal close owner; invalid_state if from a Runtime task
    Result<void> shutdown();                 // composition: request_stop + drain + join

    // Non-copyable, non-movable
    ~ApplicationRuntime();                   // fail-fast if not in a safe state
};

}  // namespace sluice::async
```

### 21.2 API Sketch 2 — Builder + one-shot

```cpp
// PROPOSED — NOT AN EXISTING API
namespace sluice::async {

class RuntimeBuilder {
public:
    RuntimeBuilder& backend(std::unique_ptr<AsyncBackend> b);
    RuntimeBuilder& workers(unsigned n);
    Result<ApplicationRuntime> build();      // validates, constructs, returns Constructed
};

class ApplicationRuntime {
public:
    Result<void> start();
    Result<void> submit(TaskFn task);
    void request_stop() noexcept;
    Result<void> drain();
    Result<void> join();
    Result<void> shutdown();
    ~ApplicationRuntime();
};

}  // namespace sluice::async
```

### 21.3 Selected proposed API direction

**Sketch 2 (Builder + one-shot) is preferred.**

| Decision | Choice | Rationale |
| --- | --- | --- |
| Constructor vs factory | Builder (`build`) | Separates validation from construction; incremental config |
| Config validation point | In `build()` | Fail fast before any owned objects exist |
| Separate `start()` | Yes | Construct/start separation; spawns driver; transactional |
| Submission return | `Result<void>` | Admission rejection via `invalid_state`; reservation+rollback |
| `request_stop()` can fail | No (`noexcept`) | Idempotent; legal all non-Fatal; worker-safe |
| `drain()`/`join()` return | `Result<void>` | invalid_state for misuse (Running, from-worker, before drain_complete) |
| `start()` failure codes | `canceled` (stop interrupt) / `invalid_state` (wrong state) | Both EXIST in `IoError::Code` (`error.hpp:15,20`); no new code invented |
| Movable | No | Owns non-movable Scheduler, AsyncIoContext, driver thread |
| Copyable | No | Unique ownership |
| Exposes internals | No | Direct access weakens lifecycle authority |
| Diagnostics | Snapshot (not reference) | Components destroyed at Stopped; snapshot before close |
| Task function receives | `CancelToken&` | Matches `Group::async` signature (`group.hpp:89`); no child spawn |

### 21.4 Fiber-local execution-identity seam (PROPOSED)

Ordinary `thread_local` is **unsound** under Fiber multiplexing: Sluice uses
stackful Fibers, one OS worker runs many Fibers, and a TLS guard bound to the C++
call stack does not save/restore on Fiber context switch. Confirmed: `Fiber`
(`fiber.hpp:60-127`) has NO existing execution-identity field.

The design proposes a **Fiber-local** execution-identity seam:

```text
// PROPOSED — PRIVATE foundation seams (NOT public API)
- An opaque tag stored IN Fiber-local state (a private field on Fiber).
- A private Scheduler/detail accessor:
      Scheduler/detail::current_execution_tag() noexcept -> void*
- The Runtime task wrapper sets its Runtime identity on the current Fiber before
  invoking user code and clears it at terminal exit.
- Because the tag belongs to the Fiber, it remains correct across suspension,
  resumption, and migration among Scheduler execution turns.
```

Conceptual private shape:

```text
Fiber::execution_tag_                       // private field
Scheduler/detail::current_execution_tag()   // private accessor
Scheduler/detail::set_current_execution_tag()
```

All are PROPOSED private foundation seams, not public APIs. **The tag is NOT
mirrored or restored through ordinary `thread_local` storage.** The ADR states:
- no change to Scheduler drive semantics;
- one private Fiber-local execution-identity seam is required;
- deterministic detection remains supported: `request_stop()` is allowed from a
  Runtime task; `drain()`/`join()`/`shutdown()` return `invalid_state` when
  invoked from a task owned by the same Runtime.

## 22. Acceptance contracts

Each contract is executable in a later phase without timing sleeps, using the
deterministic `FakeAsyncBackend` injection seam (`async_io_context.hpp:121`).

### A1 — Normal lifecycle

```text
Given a valid deterministic backend (FakeAsyncBackend)
When Runtime starts, accepts two tasks, receives stop, drains, and joins
Then both admitted tasks complete exactly once
And no driver survives
And no Scheduler worker survives
And no backend worker survives
And Runtime reaches Stopped (components destroyed)
And destruction is safe (no-op)
```

### A2 — Submission after stop

```text
Given a Running Runtime
When request_stop wins before a concurrent submit
Then the submit is rejected deterministically (invalid_state)
And its body never runs
And it is not part of the drain set
```

### A3 — Submission wins before stop

```text
When submit wins the admission reservation race
Then the task belongs to the admitted set
And drain cannot return before it reaches terminal state
```

### A4 — External submit during drain (no child submission)

```text
Given a Stopping Runtime (admission closed)
When any external submit (incl. from a task capturing ApplicationRuntime&) observes closed admission
Then the submit is rejected (invalid_state)
And the body never runs
And only tasks admitted before the gate closed are in the drain set
```

### A5 — Stop wins pre-commit / driver spawn failure

```text
Given a Starting Runtime (driver spawned, barrier waiting)
When request_stop wins before the startup commit
Then admission never opens
And run_live is never entered (driver observed state != Running at barrier)
And the driver is joined
And full resource close runs (components destroyed)
And Runtime reaches Stopped
And start() returns canceled
---
Given start() whose driver-thread spawn throws std::system_error
Then no driver survives (joinable==false verified)
And Runtime reaches StartFailed
And start() returns an error
```

### A6 — Outstanding asynchronous I/O

```text
Given a Running Runtime with an admitted task that owns an outstanding Completion
When request_stop is called
Then the task is asked to cancel (root token published)
And drain() does NOT return until AsyncIoContext::outstanding()==0
And the driver keeps re-entering run_live to poll/reap the outstanding op
And the outstanding Completion is the task's responsibility (caller-owned)
But no un-terminal backend op remains at drain_complete
```

### A7 — Concurrent join/shutdown owner election

```text
Given a Draining Runtime with drain_complete reached
When two external threads concurrently call join() (or shutdown())
Then exactly one is elected join owner (join_state InProgress)
And the other waits on runtime_cv for JoinedAndClosed
And the owner joins the driver, snapshots diagnostics, destroys resources, publishes Stopped
And the other returns success after JoinedAndClosed
And no double-join / double-destroy occurs
```

### A8 — Destructor misuse

```text
Given a Runtime in Running state
When the destructor is called
Then the process terminates (fail-fast via runtime_lifetime_fail_fast, PROPOSED)
And this is deterministic in both Debug and Release
```

### A9 — Task body throws

```text
Given a Running Runtime
When a task body throws an exception
Then the RuntimeTaskTerminalGuard still publishes terminal_count++ (RAII on unwind)
And the exception is swallowed at the Group boundary
And the task reaches terminal state
And drain() can return
And Runtime health is unaffected
```

Evidence: Group wrapper swallows exceptions (`group.hpp:173-178, 251-256`).

### A10 — Shutdown initiated from a runtime worker

```text
Given a Running Runtime
When a task running on the driver/worker calls request_stop()
Then the call is allowed (request_stop is noexcept, thread-safe, worker-safe)
And the Runtime transitions to Stopping
And an EXTERNAL thread performs drain()/join() (the worker's drain/join would return invalid_state via the Fiber-local tag)
```

### A11 — Task throws ⇒ terminal_count still advances (terminal guard bridge)

```text
Given a Running Runtime with one admitted task that throws
When the task body throws
Then the RuntimeTaskTerminalGuard fires (terminal_count++ via noexcept destructor)
And task_set_terminal_snapshot becomes true
And the Group wrapper completes its Future (complete_with) after the Runtime wrapper returns
And at the next run_live invocation boundary the driver confirms drain_complete
And drain() returns
```

### A12 — Outstanding I/O at stop keeps driver alive

```text
Given a Running Runtime with an admitted task that submitted an outstanding op then returned
When request_stop is called
Then task_set_terminal_snapshot may be true but outstanding() != 0
And the driver does NOT publish drain_complete
And the driver keeps re-entering run_live until outstanding()==0 (poll reaps the op)
Then drain_complete is published and drain() returns
```

## 23. Unit-test plan

### 23.1 State-transition tests

| Test | Target |
| --- | --- |
| Constructed → Starting → Running | `start()` success path |
| Constructed (stop remembered) → start returns canceled | stop-wins-pre-start |
| Starting → StartFailed | driver spawn failure |
| Starting → Stopped | stop-wins-pre-commit (rollback + close) |
| Running → Stopping → Draining → Stopped | Full lifecycle |
| Running → Stopping (idempotent) | Double `request_stop()` |
| Draining (idempotent) | Double `drain()` |
| Join (idempotent) | Double `join()` |

### 23.2 Illegal-operation tests

| Test | Expected |
| --- | --- |
| `submit()` in Constructed | invalid_state |
| `submit()` in Stopped | invalid_state |
| `start()` in Running | invalid_state |
| `drain()` in Running | invalid_state (must stop first) |
| `join()` before drain_complete | invalid_state |
| `drain()`/`join()`/`shutdown()` from a Runtime task | invalid_state (Fiber-local tag) |
| destructor in Running | fail-fast (death test) |

### 23.3 Admission race ordering

| Test | Method |
| --- | --- |
| submit wins before stop | Deterministic phase seam |
| stop wins before submit | Deterministic phase seam |
| concurrent submit + stop | Barrier-synchronized threads |
| `Group::async` throws after reservation | admitted_count rolled back, snapshot recomputed |

### 23.4 Startup rollback at every fallible step

| Step | Injection | Verification |
| --- | --- | --- |
| stop remembered in Constructed | request_stop before start | start returns canceled; stays Constructed |
| Driver spawn failure | force std::system_error | start returns error; StartFailed; joinable==false |
| Stop wins pre-commit | request_stop during barrier | start returns canceled; Stopped; driver joined; resources closed |

### 23.5 Task exception containment + terminal guard

| Test | Expected |
| --- | --- |
| Task throws std::exception | Guard fires; terminal_count++; task terminal; Runtime healthy |
| Task throws non-standard | Guard fires; terminal_count++; task terminal; Runtime healthy |

### 23.6 Outstanding-I/O shutdown

| Test | Expected |
| --- | --- |
| Task with outstanding Completion at stop | Driver keeps re-entering run_live; drain waits until outstanding()==0 |

### 23.7 Driver re-entry loop

| Test | Expected |
| --- | --- |
| run_live returns MW-S3, no work pending | Driver parks on runtime_cv (between invocations) |
| New submit while driver parked | Dual-wake (CV + WakeHandle) re-enters run_live |

### 23.8 Dual-wake lost-wake race

| Test | Expected |
| --- | --- |
| Omit CV notify only | Driver misses between-invocation wake (mutation killer) |
| Omit WakeHandle notify only | Worker misses in-run_live wake (mutation killer) |

### 23.9 Concurrent join/shutdown owner election

| Test | Expected |
| --- | --- |
| Two concurrent join() callers | One owner; other waits for JoinedAndClosed; no double-join |
| Two concurrent shutdown() callers | One owner; other waits; no double-destroy |

### 23.10 Destructor misuse

| Test | Expected |
| --- | --- |
| Destructor in Running | Death test (runtime_lifetime_fail_fast) |
| Destructor in Stopped | Safe (no-op) |
| Destructor in StartFailed | Safe (destroy components) |
| Destructor in Constructed | Safe (destroy components) |

## 24. Mutation plan

| Mutation | Killing test |
| --- | --- |
| Allow submit after admission closes | A2: submit after stop must be rejected |
| Omit root cancellation publication | A1: tasks must observe cancel; drain must complete |
| Return from drain with one admitted task alive | A1: drain waits for all admitted tasks |
| Forget to join the driver | A1: no driver survives (thread count check) |
| Publish Running before startup is committed | A5: Runtime does not enter Running on failure |
| Omit rollback for stop-pre-commit | A5: no surviving driver; admission never opened |
| Allow destructor with live work | A8: destructor fail-fast (death test) |
| Misclassify a losing concurrent submit as admitted | A2/A3: deterministic admission reservation |
| Skip admission gate check | A2: submit after stop must be rejected |
| Double-join the driver | A7: join is idempotent via join_state (no crash) |
| Omit reservation rollback on `Group::async` throw | A1: drain waits forever for never-running task |
| Terminal guard not noexcept / not exactly-once | A11: thrown task leaves terminal_count short |
| Omit `recompute_task_set_terminal_locked()` on admission rollback | Driver waits forever (snapshot stale) |
| Call `outstanding()` in the stop predicate | Deadlock (access_mtx_/mtx_ under global_mtx_) |
| `drain()` legal in Running | Drain set grows unbounded |
| No join owner election | A7: double-join / double-destroy crash |
| Resource close omitted in join | Backend workers survive Stopped / destructor fail-fast |
| Omit CV notify only (between-invocation wake) | Driver misses wake while parked between run_live |
| Omit WakeHandle notify only (in-run_live wake) | Worker misses wake while parked in run_live |
| Admission open before commit | A5/T4: admission-opens-then-commit-fails |
| `join()` returns before driver joined | A1: driver survives |
| Re-entry loop omitted (single run_live) | A1/A3: hang on MW-S3 return |
| `stop_requested` not checked at commit | A5: stop-pre-commit not honored |
| `Group::size()` used instead of Runtime counts | A1: false drain (completed-but-unreaped counted) |
| Outstanding-I/O check omitted in shutdown | A6/A12: destructor fail-fast on outstanding |
| Fiber execution tag stored in `thread_local` | A10: detection unsound under Fiber multiplexing |

## 25. Fuzz/formal applicability

### 25.1 State variables

```text
runtime_state      ∈ {Constructed, Starting, Running, Stopping, Draining, Stopped, StartFailed, Fatal}
admission_open     ∈ {true, false}
stop_requested     ∈ {true, false}   (monotonic)
admitted_count     ≥ 0
terminal_count     ≥ 0
control_epoch      ≥ 0               (monotonic uint64)
driver_state       ∈ {idle, in_run_live, exiting}
join_state         ∈ {NotStarted, InProgress, JoinedAndClosed}
outstanding_io     ≥ 0
execution_tag_per_fiber  (per-Fiber Runtime identity)
```

### 25.2 Invariants

```text
task_set_terminal_snapshot == (!admission_open && admitted_count == terminal_count)
drain_complete => task_set_terminal_snapshot && outstanding_io == 0
Stopped => driver joined && backend destroyed && admitted == terminal
admission_open => startup commit happened && !stop_requested_at_commit
at most one driver thread ever live
at most one join owner (join_state transitions NotStarted->InProgress->JoinedAndClosed)
join() returns => driver joined && Scheduler workers joined && backend destroyed
Fiber execution tag invariant across suspend/resume/migration
a rejected task never executes
```

### 25.3 Formal impact

```text
MODEL_RECOMMENDED
```

**Reason:** The lifecycle state machine has 8 states, multiple concurrent
operations (submit, request_stop, drain, join, shutdown), a non-trivial admission
linearization rule, a driver re-entry loop, a dual-wake protocol, and a
join-owner election. A small TLA+ state model would catch counterexamples in:
- Admission race ordering (submit vs. request_stop, reservation+rollback).
- Drain completeness (task terminal AND outstanding==0).
- Driver re-entry lost-wake (invocation-boundary race).
- Destructor safety (only Constructed/StartFailed/Stopped safe; Stopped ⇒ resources destroyed).
- Startup rollback (no surviving driver on failure; stop-pre-commit).
- Join owner election (no double-join/double-destroy).

The repository has demonstrated capacity for focused state models with deliberate
counterexample discipline:
- `docs/spec/e7_publication/E7Buggy.tla` — a deliberately broken model whose
  counterexample (`InvDoneNoTicket`, `DefectDuplicatePublish`) is documented in
  its README.
- `docs/spec/e9_wake_handle_lifetime/` — the SchedulerWakeHandle callback-lifetime
  lease model (closed defect E9-LIFETIME-CORRECTIVE), including
  `E9WakeHandleLifetimeBuggySnapshot.tla`.
- `docs/spec/e13_select/E13SelectContract.tla` — layered formal core with TLC-checked
  refinement mappings and companion `NEGATIVE_MODELS.md`.

The repo uses TLC (`tlc2.TLC` via `tla2tools.jar`) with deliberate `Buggy`/`Neg`
counterexample discipline (e.g. `scripts/verify-e11-formal.sh`,
`scripts/verify-e12-async-mutex-formal.sh`, `scripts/run-e12-tlc-all.sh`). An E16
model should follow the same discipline: a correct model plus a negative/broken
model that reproduces a known defect (e.g. the invocation-boundary lost-wake).

The variables above are sufficient to express the core invariants. No fuzz
targets or TLA+ models are implemented in A0 (per task prompt §19).

## 26. Open questions

| ID | Question | Status | Impact |
| --- | --- | --- | --- |
| Q1 | Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** | If yes, a separate "internal admission" path is needed; if no, all work goes through the gate. |
| Q2 | Should `submit()` return a handle/Future for the admitted task? | **OPEN HUMAN DECISION** | Affects API shape; a handle enables per-task await/cancel. |
| Q3 | Should the Runtime expose a diagnostics snapshot (task count, worker count, state)? | **OPEN HUMAN DECISION** | Useful for monitoring; adds API surface; must snapshot before resource close. |
| Q4 | Should `drain()` have a deadline? | **OPEN HUMAN DECISION** | A deadline prevents indefinite blocking but adds timer dependency. |
| Q5 | Should the Runtime support Threaded mode (std::thread workers) in addition to Evented? | **OPEN HUMAN DECISION** | The existing Group supports both modes (`group.hpp:47-54`); the Runtime could expose both. |
| Q6 | What is the exact TaskFn signature beyond `void(CancelToken&)`? | **OPEN HUMAN DECISION** | Affects API compatibility with existing Group consumers. A restricted TaskContext (for future child spawn) is out of scope for E16-A0. |

Q7 (wake-epoch design) and Q8 (cancellation error code) are **resolved** in this
document (§15 control_epoch; §9/§11 `canceled` vs `invalid_state`) and removed
from open questions.

## 27. Implementation slices

If authorized, implementation would proceed in order:

1. **S1: Builder + config validation** — `RuntimeBuilder`, `RuntimeConfig`, validation.
2. **S2: Owned-object construction** — `ApplicationRuntime` owns AsyncIoContext, Scheduler, root Group; acquire wake handle.
3. **S3: Lifecycle mutex + control_epoch + atomic snapshots** — state/admission/stop_requested/counts under lifecycle_mutex; 3 atomic snapshots; recompute helper.
4. **S4: `start()` transaction** — locked transitions; driver spawn; startup barrier; startup commit; rollback paths.
5. **S5: Driver re-entry loop + dual wake** — release-then-notify; between-invocation `outstanding()` check; `drain_complete` publication.
6. **S6: Admission gate** — reservation + rollback; post-commit open.
7. **S7: `request_stop()`** — 4-case linearization; locked compound commit; root cancel via group_token().
8. **S8: `RuntimeTaskTerminalGuard` + Fiber-local execution tag** — throw-safe terminal; per-Fiber identity; worker-call detection.
9. **S9: `drain()`** — legal only Stopping/Draining; wait for drain_complete.
10. **S10: `join()` (terminal close owner)** — join_state election; join driver; resource close; publish Stopped.
11. **S11: `shutdown()`** — composition of request_stop + drain + join.
12. **S12: Destructor** — per-state safety; multi-clause predicate; `runtime_lifetime_fail_fast`.
13. **S13: Tests** — acceptance contracts A1-A12, unit tests, mutation tests.
14. **S14: Formal model** (if MODEL_RECOMMENDED accepted) — TLA+ correct + negative model.

## 28. Implementation authorization status

```text
E16 production implementation remains unauthorized.
Authorization requires an accepted ADR and an independent design review
with no open P0/P1 or mandatory-contract findings.
```

## 29. Evidence index

| Claim | Evidence |
| --- | --- |
| Scheduler borrows AsyncIoContext | `scheduler.hpp:213` |
| Scheduler non-movable | `scheduler.hpp:216-219` |
| Scheduler::run/run_live block caller | `scheduler.hpp:241,249`; `scheduler.cpp:495-584` |
| Workers created AND joined inside run_impl | `scheduler.cpp:565-579` |
| run_live stop predicate at MW-S3 boundary | `scheduler.cpp:856-857` |
| run_live returns on QUIESCENT / MW-S3-no-wake | `scheduler.cpp:891-897` |
| run_live(worker_count, stop_fn, stop_ctx) signature | `scheduler.hpp:264` |
| Scheduler destruction asserts quiescence | `scheduler.cpp:114-196` (asserts at 165-196) |
| SchedulerWakeHandle make_wake_handle | `scheduler.hpp:909` |
| SchedulerWakeHandle::notify safe across destruction | `scheduler.hpp:104-109`; `scheduler.cpp:201-237` |
| Group(Scheduler&) borrows scheduler | `group.hpp:78` |
| Group group_token() (root cancel authority) | `group.hpp:114` |
| Group owns its token, passes to tasks | `group.hpp:174,201,251` |
| Group::async_evented drives run_live | `group.cpp:57-72` |
| Group::size includes completed-but-unreaped | `group.hpp:132-135` |
| Group destructor fail-fast (Evented) | `group.cpp:117-122` |
| Group non-blocking cleanup when all terminal | `group.cpp:123-128` |
| Group tasks swallow exceptions (catch in header) | `group.hpp:173-178, 251-256` (NOT group.cpp) |
| Group::async_evented throwable steps precede spawn | `group.hpp:264-282` |
| TaskFn signature | `group.hpp:89` |
| AsyncIoContext owns backend | `async_io_context.hpp:121` |
| AsyncIoContext move-only | `async_io_context.hpp:125-133` |
| AsyncIoContext::outstanding() (public query) | `async_io_context.hpp:149`; impl `async_io_context.cpp:150-153` |
| AsyncIoContext destructor fail-fast | `async_io_context.cpp:30-41` |
| Completion caller-owned, non-movable | `completion.hpp:70-71,79,84-87` |
| Completion reset | `completion.hpp:125-129` |
| Completion outstanding contract (L11) | `completion.hpp:17-18` |
| CancelToken idempotent request | `cancel.hpp:49,59` |
| check_cancel is the cancel point | `cancel.hpp:147` |
| Future(WaitPolicy&) borrows policy | `future.hpp:53-54` |
| Future non-movable | `future.hpp:56-59` |
| Future cancel requests+awaits | `future.hpp:113-116` |
| Fiber class (no execution-tag field) | `fiber.hpp:60-127` |
| EventedWaitPolicy borrows Scheduler | `evented_wait_policy.hpp:42,48` |
| ThreadPoolBackend class/ctor | `threadpool_backend.hpp:55,57` |
| ThreadPoolBackend outstanding() takes mtx_ | `threadpool_backend.hpp:71`; `threadpool_backend.cpp:183-186` |
| ThreadPoolBackend destroying_ gate | `threadpool_backend.hpp:103` |
| ThreadPoolBackend workers joined in destructor | `threadpool_backend.hpp:101`; `threadpool_backend.cpp:23-32` |
| outstanding_ decrements at poll reap | `threadpool_backend.cpp:160-163` |
| ThreadPoolBackend shutting_down_for_test | `threadpool_backend.hpp:78` |
| fail_fast routes through std::terminate | `fail_fast.cpp:16-62` |
| All 7 fail-fast entries subsystem-bound (NO generic) | `fail_fast.hpp:31-129` |
| IoError::Code enum (8 enumerators; canceled, invalid_state) | `error.hpp:14-21` (canceled :15, invalid_state :20) |
| CI gate (Linux Clang Debug core) | `ci.yml:37-74` (full gate incl. neg-compile/acceptance/docs 76-101) |
| Production targets: sluice_core, sluice_async | `libraries.lua:7-33` |
| No public API for runtime lifecycle | `docs/api-reference.md` (no Runtime entry; "Async Runtime (E13+)" section at :807) |
| E16 design doc placeholder | `docs/design/README.md:28` |
| Existing formal models (E7/E9/E13) | `docs/spec/e7_publication/E7Buggy.tla`; `docs/spec/e9_wake_handle_lifetime/`; `docs/spec/e13_select/E13SelectContract.tla` |
| TLC checker discipline | `scripts/verify-e11-formal.sh` et al.; `scripts/run-e12-tlc-all.sh` |
