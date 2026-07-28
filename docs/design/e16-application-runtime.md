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
honestly, the design requires **five private PROPOSED foundation seams** (none
public, none authorizing implementation):

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
- a **Group transactional admission seam** — `Group::async_evented` must gain a
  strongly exception-safe / aggregate task-record insertion before E16 admission
  rollback is correct (§13.5, P2-01). This is a foundation **prerequisite**, not
  a Runtime seam, so E16 implementation is blocked until it lands.

None of these is a change to Scheduler *drive semantics* and none is a new public
API. They are private implementation seams / prerequisites, listed as ADR
consequences.

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
5. Provide separate, non-conflated `request_stop`, `drain`, `join`, and a
   `shutdown` that is correct in **every** safe state (not a simple
   `request_stop+drain+join` composition, which is invalid in pre-Running
   states — §17.1).
6. Define startup as a transaction with rollback at every fallible step, **with
   a startup barrier that a concurrent `request_stop()` can release**
   (stop-before-commit, §9 step 5 / P1-03).
7. Define a destructor contract that prevents resource leaks and undefined behavior.
8. Support deterministic test injection of backends.
9. Drain outstanding backend I/O *before* the driver exits.
10. Detect lifecycle calls made from inside a Runtime task (Fiber-local
    execution identity).
11. Give Runtime tasks a **restricted I/O capability** (`RuntimeTaskContext`,
    §21.5) so an admitted task can submit the asynchronous I/O the Runtime
    drives, without exposing raw internals (P1-04).
12. Return Runtime ownership via **owned indirection**
    (`Result<std::unique_ptr<ApplicationRuntime>>`, P1-02) because the Runtime is
    non-movable; the stable heap address anchors driver-thread captures, the
    Fiber-local identity tag, and private component pointers.
13. Publish root cancellation **under `lifecycle_mutex`** so it cannot race with
    the resource close that destroys the root Group (P1-01).
14. Block E16 implementation on a **Group transactional admission seam** (§13.5)
    so admission failure cannot leave malformed partial task records (P2-01).

## 5. Non-goals

| Non-goal | Decision | Rationale |
| --- | --- | --- |
| Restartable Runtime | **REJECTED** (E16) | One-shot lifecycle is the default. Restart requires backend/Scheduler reconstruction, worker generation, cancellation generations, stale references — all out of scope for E16. |
| Multiple independent root runtimes sharing one Scheduler | **REJECTED** (E16) | Scheduler is non-movable, non-copyable (`scheduler.hpp:216-219`). Sharing would require lifetime coordination that the Runtime does not provide. |
| Dynamic backend replacement | **REJECTED** (E16) | `AsyncIoContext` is move-only; backend is injected at construction (`async_io_context.hpp:121`). No hot-swap. |
| Hot worker-count resizing | **REJECTED** (E16) | Worker count is fixed at `start()` time. |
| Structured child submission | **REJECTED** (E16 v1) | `RuntimeTaskContext` (§21.5) exposes cancellation + I/O submission but **no `spawn`**, so a task cannot spawn Runtime-owned child tasks. A restricted `RuntimeTaskContext::spawn()` is a future extension. See §13.4. |
| Unrestricted backend/AsyncIoContext access from tasks | **REJECTED** (E16 v1) | A task reaches I/O only through the restricted `RuntimeTaskContext` (§21.5); it never receives a raw `Scheduler&`, `Group&`, backend, or `AsyncIoContext&`. |
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
| Foundation changes | No scheduler drive-semantics change; one private Fiber-local identity seam + 3 private Runtime seams + 1 Group transactional admission prerequisite (5 total, §21.6) |
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
| Foundation changes | No scheduler drive-semantics change; one private Fiber-local identity seam + 3 private Runtime seams + 1 Group transactional admission prerequisite (5 total, §21.6) |
| Compatibility | No effect on existing users |
| Verification | Good — deterministic backend injection via existing seam |

### Alternative C — Builder + one-shot Runtime with dedicated driver

A `RuntimeBuilder` collects configuration; `build()` validates and returns
**owned indirection** to a constructed (but not started) Runtime. `start()` is a
separate, fallible transaction that spawns the dedicated driver thread. The
driver loops on `run_live` re-entry. Backend is injected (from B).

```text
RuntimeBuilder
├── backend (injected)
├── worker_count
├── config validators
└── build() → Result<std::unique_ptr<ApplicationRuntime>>
              (Constructed, not started, no driver, stable heap address)

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
| Construction order | 1. Builder collects config → 2. `build()` validates + constructs `ApplicationRuntime` on the heap + constructs owned objects + acquires wake handle → 3. Returns `Result<std::unique_ptr<ApplicationRuntime>>` in `Constructed` state (no driver) |
| Destruction order | Per-state (§10): Constructed/StartFailed destroy components normally; Stopped already destroyed them via join owner |
| Backend testability | **GOOD** — same injection seam as B |
| Partial-start rollback | `start()` is a transaction; rollback at each step; driver-spawn failure leaves no surviving thread |
| Cancellation owner | root Group's group_token() |
| Admission owner | root Group, gated by Runtime admission flag; opens only after startup commit |
| Drain authority | Runtime-owned admitted/terminal counts + driver re-entry loop; outstanding()==0 verified between run_live invocations |
| Public exposure | Builder exposes config; Runtime exposes lifecycle ops |
| Misuse resistance | **BEST** — separation of construct/start prevents use-before-start; builder validates config; Fiber-local tag detects worker calls |
| Complexity | Medium-high (driver thread + re-entry loop + dual wake + Fiber-local seam) |
| Foundation changes | **No change to Scheduler drive semantics.** Five private PROPOSED seams/prereqs: `runtime_lifetime_fail_fast`, Fiber-local execution-identity, `RuntimeTaskTerminalGuard`, `recompute_task_set_terminal_locked`, and the Group transactional admission prerequisite (§13.5/§21.6). |
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
| Foundation changes | 5 seams/prereqs | 5 seams/prereqs | 5 seams/prereqs |
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
│         ├── startup_abort_requested (bool; set by request_stop during Starting, §9)
│         ├── root_cancel_published  (bool, monotonic; set when group_token().request() runs, P1-01)
│         ├── admitted_count       (size)
│         ├── terminal_count       (size)
│         ├── control_epoch        (uint64)
│         └── close_state          ({Open, InProgress, Closed})  ← unified
│                                  terminal-close ownership (§17.0)
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
4. Consumer calls builder.build() → Result<std::unique_ptr<ApplicationRuntime>>
   a. Validate config (worker_count >= 1, backend != nullptr)
   b. Construct ApplicationRuntime on the heap (stable address)
   c. Construct AsyncIoContext (owns backend)
   d. Construct Scheduler (borrows AsyncIoContext)
   e. Construct root Group (borrows Scheduler)
   f. Acquire SchedulerWakeHandle (Scheduler::make_wake_handle)
   g. state = Constructed; admission = CLOSED; stop_requested = false
   h. No driver thread exists yet
5. Consumer calls runtime.start()
   a. LOCK lifecycle_mutex
      - require state == Constructed (else return IoError::invalid_state)
      - if stop_requested already remembered:
            UNLOCK; return IoError::canceled (stay Constructed)
      - state = Starting
      - startup_abort_requested = false
      UNLOCK
   b. Spawn driver thread (may throw std::system_error → rollback to StartFailed).
      The driver, once spawned, publishes driver_started and then waits at the
      startup barrier (step c).
   c. Driver startup barrier predicate (wait on runtime_cv under lifecycle_mutex):
        state != Starting
        OR startup_abort_requested
        OR fatal_snapshot
      The driver holds lifecycle_mutex only while reading the predicate; it
      releases it to wait. The driver does NOT enter run_live until it observes
      state == Running at the barrier.
   d. start() owner re-locks lifecycle_mutex and decides:
      - if stop_requested (a concurrent request_stop arrived during step b/c):
            startup_abort_requested = true
            control_epoch++
            UNLOCK
            notify Runtime CV + SchedulerWakeHandle (releases the barrier)
            join driver
            close all Runtime execution resources
            LOCK lifecycle_mutex: state = Stopped; close_state = Closed; control_epoch++
            UNLOCK; notify runtime_cv
            return IoError::canceled
      - otherwise (no stop requested):
            state = Running
            admission_open = true
            recompute_task_set_terminal_locked()
            control_epoch++
            UNLOCK
            notify Runtime CV + SchedulerWakeHandle (releases the barrier)
            return success
   e. Driver behavior after barrier wake:
        if state == Running: enter the run_live re-entry loop
        otherwise (startup aborted): exit WITHOUT ever calling run_live
```

The linearization point of a **successful** startup is the locked compound commit
at step d (Running branch). The linearization point of a **stop-before-commit
abort** is the locked `startup_abort_requested = true; control_epoch++` at step d
(stop branch); from that point `start()` owns the driver join and the resource
close. The driver observes the decision through the barrier predicate and never
enters `run_live` on the abort branch.

**P1-03 invariants:**
```text
stop-before-commit → run_live is NEVER entered
stop-before-commit → admission NEVER opens
stop-before-commit → driver is joined by the start owner exactly once
                     (becomes joinable-by-owner, or was never spawned if b threw)
stop-before-commit → resources are closed exactly once
stop-before-commit → final Stopped only after resource close
```

There is **no unconditional `Starting → Running` commit**. The commit is gated on
`!stop_requested` read under `lifecycle_mutex` at step d.

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
| Starting → Running | startup commit (locked) | driver spawned; commit under lifecycle_mutex; !stop_requested at commit |
| Starting → StartFailed | construction throw (driver spawn) | rollback completed; joinable==false verified |
| Starting → Stopped | stop wins pre-commit (abort path) | startup_abort_requested set under lifecycle_mutex; barrier released; driver joined; rollback + resource close completed; start() returns canceled |
| Running → Stopping | `request_stop()` (locked compound commit; root cancel published under lifecycle_mutex) | state == Running |
| Stopping → Draining | `drain()` | state == Stopping |
| Draining → Stopped | `join()` (drain_complete reached) | close owner joins driver + closes resources |
| Stopping → Stopped | `shutdown()` (state-dispatched close, §17.1) | close owner: drain_complete reached; join + close |
| Constructed → Stopped | `shutdown()` (direct close) | close owner destroys components; no driver |
| StartFailed → Stopped | `shutdown()` (direct close) | close owner destroys remaining components |
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
| `join()` | Second call returns immediately if close_state == Closed |
| `shutdown()` | State-dispatched (§17.1); idempotent via close_state == Closed; correct in every safe state, not only Running |

### 11.6 Concurrency behavior when operations race

| Race | Resolution |
| --- | --- |
| `request_stop()` races with `submit()` | Locked compound commit: stop sets admission_open=false in the same critical section; submit's reservation checks admission_open under the same mutex. Linearization point = the locked commit / reservation. Loser rejects. |
| `request_stop()` races with `start()` pre-commit | request_stop records stop_requested; start observes it at the locked startup commit, rolls back, returns canceled. |
| `request_stop()` races with `drain()` | `request_stop()` closes admission first (locked); `drain()` only legal in Stopping/Draining. |
| `drain()` races with `join()` | `drain()` must complete (drain_complete) before `join()` is legal; `join()` returns invalid_state before drain_complete. |
| concurrent `join()` / `shutdown()` | One elected close owner (close_state Open→InProgress→Closed, §17.0); others wait on runtime_cv for Closed. The single mechanism covers join, shutdown, startup-abort close, StartFailed close, and Constructed close. |
| concurrent `request_stop()` | Locked compound commit; monotonic stop_requested; all callers return success. |

## 12. Operation/state matrix

| Operation | Constructed | Starting | Running | Stopping | Draining | Stopped | StartFailed | Fatal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `start()` | **allowed** (or canceled if stop_requested) | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state |
| `submit()` | invalid_state | invalid_state | **allowed** | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state |
| `request_stop()` | remember+stay (success) | record; rollback→Stopped | **allowed** (compound commit→Stopping) | idempotent | idempotent | idempotent | idempotent | undefined |
| `drain()` | invalid_state | invalid_state | invalid_state | **allowed** | idempotent | idempotent | idempotent | undefined |
| `join()` | invalid_state | invalid_state | invalid_state | invalid_state (before drain_complete) | invalid_state (before drain_complete) / **allowed** (drain_complete) | idempotent | idempotent | undefined |
| `shutdown()` | **allowed** (direct close: destroy components→Stopped) | **allowed** (record startup_abort; start owner rollback+close→Stopped; shutdown waits for Closed) | **allowed** (request_stop+drain+join) | **allowed** (drain+join) | **allowed** (wait drain_complete; join+close) | idempotent (Closed) | **allowed** (direct close: destroy remaining components→Stopped) | undefined |
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
    observed_pending_work = true       # an admission always creates pending work
UNLOCK
try:
    root_group.async(runtime_wrapper)  # may spawn the Fiber (Scheduler::spawn)
catch (exception from Future/stack/Fiber alloc, init_fiber, vector push_back):
    LOCK lifecycle_mutex:
        admitted_count--
        recompute_task_set_terminal_locked()
        control_epoch++                # publish the rollback (P1-07)
    UNLOCK
    dual-wake (runtime_cv.notify_all + scheduler_wake_handle.notify)
    # error mapping is fixed (§20.6): rethrow std::bad_alloc unchanged; map
    # init_fiber-failure / Group-converted errors to Result with backend_error.
    return map_submit_failure(error)
# SUCCESS path: a runnable Fiber now exists but the driver may be parked at an
# invocation boundary. Publish the epoch and dual-wake so the driver observes it
# (P1-07). Scheduler::spawn only notifies the worker inbox_cv (scheduler.cpp:419-441);
# it does NOT touch the Runtime CV or control_epoch.
LOCK lifecycle_mutex:
    control_epoch++                    # ALWAYS on successful admission (P1-07)
UNLOCK
dual-wake (runtime_cv.notify_all + scheduler_wake_handle.notify)
return success
```

**Why the success-path epoch++ and dual-wake are mandatory (P1-07).** The driver
parks on `runtime_cv` between `run_live` invocations and only re-enters `run_live`
when its persistent predicate fires: `driver_exit_requested OR fatal OR
control_epoch != observed_epoch` (§16.1b). `Scheduler::spawn()` publishes the
Fiber to a worker inbox and notifies that worker's `inbox_cv`
(`scheduler.cpp:419-441`); it does **not** signal the Runtime CV and does **not**
change `control_epoch`. So if `submit()` returned without epoch++ and dual-wake,
a driver that had just returned from `run_live` and was about to wait on
`runtime_cv` would never observe the new work — the admitted task could remain
stranded until an unrelated notification. The epoch++ + dual-wake on every
successful admission closes this boundary liveness gap. The epoch++ also makes
the rollback path symmetric (both paths publish the epoch and wake).

All throwable steps in `Group::async_evented` (Future/stack/Fiber allocation,
`init_fiber`, vector `push_back`) occur **before** `Scheduler::spawn`
(`group.hpp:264-282`), so on exception the user task body has NOT executed and no
Fiber was published. The Runtime reservation+rollback protocol closes the
**Runtime accounting** failure modes:
- "admitted a task that never runs → drain waits forever" (rollback decrements
  admitted_count and recomputes the snapshot);
- "terminal_count > admitted_count" (the reservation is decremented before
  control returns to the caller).

**It does NOT, by itself, close the Group internal ownership failure mode**
(P2-01): `Group::async_evented` performs three independent vector `push_back`s
(`futures_`, `evented_fibers_`, `evented_stacks_`) under `mtx_` with no capacity
reservation (`group.hpp:278-280`). A later `push_back` may throw after an earlier
`push_back` succeeded, leaving the Group with a malformed partial task record (e.g. a Fiber
without its matching stack, or a Fiber+stack without a Future). `Scheduler::spawn`
has not occurred and user code has not run, so the partial record is not
*observed* at runtime, but the Group may retain malformed ownership. Closing this
requires the Group transactional admission seam (§13.5), listed as a foundation
prerequisite.

### 13.2 Admission answers

| Question | Answer |
| --- | --- |
| When has a task been successfully admitted? | When the reservation succeeds (admission_open verified AND admitted_count incremented, under lifecycle_mutex) **and** the success path publishes `control_epoch++` + dual-wake so a driver parked at an invocation boundary observes the new work (§13.1, P1-07). |
| When has an I/O operation been successfully admitted? | I/O admission is separate (Completion-based, `RuntimeTaskContext::submit_*` → `AsyncIoContext::submit_*`, §21.5). Runtime admission gates task admission, not I/O ops. A task submits I/O through its `RuntimeTaskContext` after being admitted. |
| Is "placed in an intermediate queue" admission? | No. Admission is the reservation. The Fiber's placement on the scheduler's runnable queue (via `Group::async` → `Scheduler::spawn`) is a scheduler internal, not admission. |
| Is "worker started" admission? | No. The driver is started in `start()`. Task admission is per-submit reservation. |
| What happens when stopping races with submission? | Locked compound commit / reservation. Winner admitted, loser rejected with `IoError::invalid_state`. Deterministic. |
| Can a Running task submit child work after `request_stop()`? | E16 v1 has NO structured child submission (`RuntimeTaskContext` has no `spawn`). Any external capture of `ApplicationRuntime&` by a task body is treated as ordinary concurrent external `submit()`, subject to the same admission gate; after `request_stop()` closes admission, such a submit is rejected. See §13.4. |
| Can an admitted task submit I/O after `request_stop()`? | **Yes.** Per-task I/O progress must be drainable; an admitted task may continue submitting I/O through `RuntimeTaskContext` after stop, and `drain()` cannot return until that I/O is reaped (`outstanding()==0`). No new I/O may be submitted after the task body exits (§21.5). |
| Can it submit during `drain()`? | `drain()` is only legal after `request_stop()` closed admission; any external submit observing closed admission is rejected. |
| Are child tasks part of the original drain set? | N/A — no structured child submission in E16 v1. |
| Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** — see §26 (Q1). |
| Does cancellation precede closing admission or follow it? | In the locked compound commit `stop_requested=true`, `admission_open=false`, AND root cancellation (`root_group.group_token().request()`) are published together **while `lifecycle_mutex` is held** (§15 case 3). Publishing cancellation under the mutex is what prevents the request_stop-vs-close use-after-free (P1-01). |
| How is rejected submission reported? | `submit()` returns `Result` with `IoError::invalid_state`. |
| Can rejection run user code? | No. Rejection returns before the reservation; no task body runs. |
| Can admission failure leave observable partial ownership? | **Runtime admission accounting** (`admitted_count`, task-set snapshot) rolls back fully under `lifecycle_mutex`. **Group internal ownership** (Future/stack/Fiber/`futures_`/`evented_*_` vectors) does NOT roll back today — `Group::async_evented` performs three independent vector `push_back`s with no reservation (`group.hpp:278-280`), so a later insertion may throw and leave the Group with a partial task record. This is why E16 is blocked on the Group transactional admission seam (§13.5, P2-01). |

### 13.3 Post-commit open (T4 trace closure)

Admission opens **only after** the startup commit (§9 step 5d) AND only if
`!stop_requested` at commit. Therefore the trace "admission opens, then commit
fails / stop wins" is **impossible**: the commit and the admission-open are the
same locked compound transition. If stop wins pre-commit, admission never opens,
the driver is joined on rollback, and `start()` returns `canceled`.

### 13.4 Child submission — explicitly NOT supported in E16 v1

`RuntimeTaskFn = void(RuntimeTaskContext&)` (§21.5) gives no child-spawn
capability — `RuntimeTaskContext` exposes cancellation and I/O submission only,
not `spawn`. Tasks cannot spawn Runtime-owned child tasks. Any external capture
of `ApplicationRuntime&` by a task body is treated as ordinary concurrent
external `submit()`. A restricted `RuntimeTaskContext::spawn()` is documented as
a future extension, out of scope for E16-A0. This removes the contradiction
between "child tasks terminal" guarantees and an unrestricted child-admission
path. (The root `CancelToken&` is still directly reachable inside the task via
`RuntimeTaskContext::cancel_token()`, §21.5.)

### 13.5 Group transactional admission seam — E16 implementation prerequisite (P2-01)

**E16 implementation is blocked until `Group::async_evented` provides a
transactional admission seam.** Today it performs three independent vector
`push_back`s under `mtx_` with no capacity reservation
(`group.hpp:278-280`):

```text
evented_fibers_.push_back(std::move(fiber_up));   // may throw (realloc)
evented_stacks_.push_back(std::move(stack_up));   // may throw (realloc)
futures_.push_back(std::move(fut));               // may throw (realloc)
```

A later insertion may throw after an earlier insertion succeeded, leaving the
Group with a partial task record (e.g. Fiber without stack, or Fiber+stack
without Future). `Scheduler::spawn` has not occurred and user code has not run,
so the partial record is not *executed*, but the Group may retain malformed
ownership that complicates destruction and later submission.

The future implementation must provide one of:

```text
A. an explicit Group reservation/commit API;
B. a strongly exception-safe async_evented insertion (reserve capacity before
   the first push_back, as the Threaded path already does at group.hpp:170); or
C. a single aggregate task-record container inserted atomically.
```

A preferred aggregate shape (conceptual only — PROPOSED):

```cpp
// Conceptual only — PROPOSED
struct EventedTaskRecord {
    std::unique_ptr<Fiber> fiber;
    std::unique_ptr<std::byte[]> stack;
    std::shared_ptr<Future<void>> future;
};
// Group inserts ONE record (strongly exception-safe) before Scheduler::spawn.
```

Then `Group::async_evented` inserts one record before `Scheduler::spawn`.

**E16-A0 does NOT implement this seam.** It is listed as a required private
foundation change / implementation prerequisite (ADR §9), and the foundation seam
count is updated to **five** (§21.6).

**Runtime reservation behavior with the seam.** The Runtime may still roll back
`admitted_count` because user code has not run. But the design now states
accurately:

```text
Runtime admission accounting rollback is possible.
Group internal ownership rollback REQUIRES the new transactional Group seam.
```

**Acceptance/mutation obligation (A18):** inject failure at each
allocation/insertion point and prove:
- the user task never executes;
- Runtime `admitted_count` rolls back;
- the Group retains no malformed partial task record;
- destruction is safe;
- later submission remains valid.

## 14. Cancellation contract

- The authoritative Runtime root cancellation state is **`root_group.group_token()`**
  (`group.hpp:114`). The Runtime does NOT create an independent second token.
- `request_stop()` publishes root cancellation via
  `root_group.group_token().request()`. **In the Running state this publication
  occurs while `lifecycle_mutex` is held** (§15 case 3), so that root cancellation
  cannot race with the resource close that destroys `root_group` (P1-01).
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

2. **Starting, before startup commit**: record `stop_requested=true` and set
   `startup_abort_requested=true` (P1-03). The start owner, at the locked startup
   decision (§9 step 5d), observes `stop_requested`: it sets
   `startup_abort_requested`, increments `control_epoch`, releases `lifecycle_mutex`,
   and notifies the Runtime CV + SchedulerWakeHandle to release the barrier. The
   driver wakes at the barrier, observes `state != Running`, and exits **without
   ever entering `run_live`**. The start owner joins the driver, closes resources,
   transitions to `Stopped`, and returns `canceled`. The Runtime never publishes an
   externally observable `Running` state, and `run_live` is never entered.

3. **Running** (post-commit) — **root cancellation is published while
   `lifecycle_mutex` is still held**, so root cancellation cannot race with the
   resource close that destroys `root_group`. The linearization is:

   ```text
   LOCK lifecycle_mutex:
       stop_requested = true
       admission_open  = false
       root_group.group_token().request()      ← published UNDER the mutex
       state           = Stopping
       recompute_task_set_terminal_locked()
       control_epoch   = control_epoch + 1
   UNLOCK lifecycle_mutex
   runtime_cv.notify_all()
   scheduler_wake_handle.notify()
   ```

   Calling `CancelToken::request()` under `lifecycle_mutex` is safe:
   - it is `noexcept` (`cancel.hpp:59`) — it cannot throw out of the critical
     section;
   - it is idempotent (`cancel.hpp:59`) — a second stop is a benign no-op;
   - it performs a single `release` store on an atomic flag (`cancel.hpp:78-80`)
     — an atomic state publication, not a call into user code;
   - it does **not** acquire Scheduler locks and does not block.

   The two notifications remain **outside** `lifecycle_mutex` (release-then-notify,
   §16), exactly as before. Only the ordering of `group_token().request()` moved:
   it now precedes the unlock instead of following it.

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

The Scheduler invocation stop predicate (called under `global_mtx_` at the MW-S3
boundary, `scheduler.cpp:856-857`) reads ONLY three lock-free atomics —
`fatal_snapshot`, `driver_exit_requested`, `task_set_terminal_snapshot` — and
**never** reads `drain_complete` (which would be circular: `drain_complete` is
derived Runtime state published by THIS driver). It does not call
`outstanding()` either, but **not because of a lock-order hazard**: production's
`Scheduler::classify_locked()` already calls `ctx_.outstanding()` while holding
`global_mtx_` (`scheduler.cpp:1068-1071`), because `global_mtx_ → access_mtx_` is
the **accepted lock order** (documented at that site). The real architectural
reason the stop predicate omits `outstanding()` is **separation of authority**:
the Runtime driver — not the Scheduler invocation — is the sole publisher of
`drain_complete`, and it confirms `outstanding()==0` **between** invocations
(outside any Scheduler lock) so that the publication is a single, owned
transition rather than a side effect of the Scheduler's MW classification.

**Drain classification reasoning (verified against production control flow).**
`Scheduler::classify_locked()` (`scheduler.cpp:1052-1080`) orders the worker
states as:

```text
runnable/running            => MW-S1
outstanding I/O (ctx_.outstanding() > 0)  => MW-S2
wait-only                   => MW-S3_unresolved
otherwise                   => quiescent
```

The invocation stop predicate is consulted in the **MW-S3** path, not in MW-S2.
Therefore:

```text
While AsyncIoContext::outstanding() > 0, Scheduler classification is MW-S2,
so the invocation stop predicate is NOT the authority that terminates the run;
run_live keeps driving poll/reap of the outstanding op.

After outstanding reaches zero, classification may advance to MW-S3 or
quiescent; the task-terminal snapshot can then cause the invocation to return,
and the Runtime driver confirms drain_complete between invocations.
```

This is why the driver's between-invocation `outstanding()==0` check is the
drain authority, not a workaround for a forbidden lock order. (Equivalently: a
defect would be to publish `drain_complete` directly from the invocation stop
predicate without the driver's between-invocation check — that is an
authority/lifecycle proof violation, not a deadlock.)

The driver loop, **between** `run_live` invocations (outside any Scheduler lock),
performs the unified boundary protocol of §16.1b. It does **not** re-enter
`run_live` blindly: it first parks on a persistent CV predicate so that an
admitted task (whose `submit()` published `control_epoch++` + dual-wake, §13.1)
is never stranded, and so that a post-drain driver does not spin or exit
prematurely.

### 16.1b Unified driver state machine (P1-07)

The dedicated driver thread is the only caller of `Scheduler::run_live` and runs
one coherent state machine. `run_live` may return at QUIESCENT / MW-S3-no-wake /
stop-predicate-true boundaries (`scheduler.cpp:823-918`) without all work being
done, so the driver **cannot** assume a single `run_live` stays resident; it must
park at each boundary under a persistent predicate and re-enter `run_live` only
when new or remaining work warrants it.

```text
driver_state ∈ {not_started, barrier_wait, in_run_live, between_invocations,
                drained_wait, exiting, exited}
```

```text
not_started
    │ start() spawns the driver; driver records driver_started
    ▼
barrier_wait                                  (P1-03)
    │ wait on runtime_cv (under lifecycle_mutex) until:
    │     state != Starting
    │     OR startup_abort_requested
    │     OR fatal_snapshot
    │ if state == Running:        → in_run_live
    │ else (startup aborted):     → exiting (run_live NEVER entered)
    ▼
in_run_live
    │ call Scheduler::run_live(worker_count, stop_predicate, this)
    │   stop_predicate (under global_mtx_, scheduler.cpp:856-857) reads ONLY the
    │   three lock-free snapshots (fatal_snapshot, driver_exit_requested,
    │   task_set_terminal_snapshot) — never drain_complete, never outstanding()
    │ run_live returns at an invocation boundary
    ▼
between_invocations                            (the load-bearing boundary state)
    │ snapshot observed_epoch = control_epoch
    │ wait on runtime_cv (under lifecycle_mutex) until the PERSISTENT predicate:
    │     driver_exit_requested
    │     OR fatal_snapshot
    │     OR control_epoch != observed_epoch
    │ on wake, re-evaluate (still under the mutex) and dispatch:
    │     if driver_exit_requested:            → exiting
    │     if fatal_snapshot:                   → exiting (then Fatal)
    │     if task_set_terminal_snapshot && outstanding()==0:
    │         publish drain_complete
    │         signal runtime_cv                (wake drain() callers)
    │         → drained_wait
    │     else (new/remaining work):           → in_run_live   (re-enter run_live)
    ▼
drained_wait                                   (post-drain park; P2-03)
    │ snapshot observed_epoch = control_epoch
    │ wait on runtime_cv (under lifecycle_mutex) until:
    │     driver_exit_requested
    │     OR fatal_snapshot
    │     OR control_epoch != observed_epoch
    │ on wake:
    │     if driver_exit_requested:            → exiting
    │     if fatal_snapshot:                   → exiting (then Fatal)
    │     otherwise (epoch changed, not exit): re-evaluate; admission is closed
    │                                          so this is not a busy loop — it
    │                                          fires only on a real control change
    ▼
exiting
    │ run_live not re-entered
    ▼
exited                                         (driver joinable by close owner, §17)
```

**The persistent CV predicate is the liveness authority, not the notification.**
Correctness of `between_invocations` and `drained_wait` rests on the conjunction
`driver_exit_requested OR fatal_snapshot OR control_epoch != observed_epoch`,
checked **under `lifecycle_mutex`** in the standard wait-while pattern
(check predicate under mutex → `wait` releases mutex → re-check on wake). Because
every state-changing operation (`request_stop()`, successful `submit()`,
admission rollback, terminal-guard `terminal_count++`, startup abort/commit,
close-owner publication of `driver_exit_requested`) performs `control_epoch++`
and `runtime_cv.notify_all()` **after** releasing `lifecycle_mutex`, a lost
notification cannot strand the driver: the next predicate check observes the
bumped epoch. This is why the success path of `submit()` MUST publish the epoch
and dual-wake (§13.1) — `Scheduler::spawn()` only notifies a worker `inbox_cv`
(`scheduler.cpp:419-441`) and does not touch the Runtime CV or epoch.

**Boundary-liveness trace (the defect P1-07 closes).** Without the success-path
epoch++ + dual-wake and the `between_invocations` park, the following was
possible:
```text
driver returns from run_live, about to wait on runtime_cv
submit reserves, calls Group::async → Scheduler::spawn publishes the Fiber
(no Runtime CV notify, no control_epoch change)
driver begins waiting  → task stranded until an unrelated notification
```
With §13.1 (epoch++ + dual-wake on success) and §16.1b (between_invocations
park on the epoch predicate), the driver's predicate fires on the bumped epoch
and re-enters `run_live`, which polls the newly-published runnable Fiber.

**Post-drain stability.** In `drained_wait`, admission is closed and I/O is zero
(or being reaped), so ordinary new work cannot appear; the only normal exit is
the close owner publishing `driver_exit_requested` (§17). The
`control_epoch != observed_epoch` term is a defensive re-evaluation path that
fires only on a genuine control change, never as a busy loop.

The driver FSM is part of the formal model (`driver_state`, §25.1) and is covered
by acceptance A19 (post-drain park) and A20 (boundary liveness), and by the
mutation plan (§24).

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
user `RuntimeTaskFn` throws. PROPOSED shape:

```cpp
// PROPOSED — private Runtime implementation detail
{
    RuntimeTaskTerminalGuard guard{runtime};   // noexcept RAII; sets Fiber tag
    RuntimeTaskContext ctx{runtime, ...};      // §21.5; valid only inside this block
    task(ctx);                                 // user RuntimeTaskFn; may throw
}  // ctx destroyed (no further I/O submission possible); guard destructor:
   // noexcept, exactly-once terminal_count++
```

The guard's destructor (under `lifecycle_mutex`): `terminal_count++`;
`recompute_task_set_terminal_locked()`; `control_epoch++`; release; dual-wake.
It runs on both normal-return and throw (RAII runs on stack unwind). The
`RuntimeTaskContext` is destroyed at the end of the block, so no I/O may be
submitted after the task body exits (P1-04).

Three distinct terminal notions:
- `terminal_count` — user `RuntimeTaskFn` has left (guard fires);
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

## 17. Join and terminal-close contract

### 17.0 Unified terminal-close ownership (P1-05)

There is **one** lifecycle close/join ownership mechanism for **all** paths that
destroy Runtime execution resources:

```text
join()                 (Running → ... → Stopped, post-drain)
shutdown()             (every safe state → Stopped)
startup-abort close    (Starting → Stopped, §9 step 5d stop branch)
StartFailed close      (StartFailed → Stopped)
Constructed close      (Constructed → Stopped)
```

The previous design conflated this into a single `join_state` named for the
driver join. P1-05 makes the ownership mechanism **truthfully cover all
terminal-close paths, not only the driver join**, by renaming it to a unified
`close_state`:

```text
close_state ∈ {Open, InProgress, Closed}
```

- **Open** — no caller has begun terminal close; resources are alive.
- **InProgress** — exactly one caller (the **close owner**) is performing
  resource close. Every other concurrent caller of `join()`/`shutdown()`/etc.
  observes `InProgress`, waits on `runtime_cv` for `Closed`, and returns success.
- **Closed** — terminal close finished; resources destroyed; `state == Stopped`.
  Idempotent: any further close-path call returns success immediately.

The owner election is a single locked transition `Open → InProgress` under
`lifecycle_mutex`. Only the owner ever touches the driver, the resources, or the
`Stopped` publication. No two callers ever destroy resources simultaneously.
This replaces the prior `join_state` (which implied only the driver join);
`close_state` is the same mechanism, renamed to cover every close path.

`root_cancel_published` (formal model, §25.1) tracks whether root cancellation has
been published. The close owner asserts it never publishes root cancellation on a
Group it is about to destroy: in the Running path `request_stop()` already
published it under `lifecycle_mutex` (P1-01), and in pre-Running paths no tasks
were ever admitted so there is nothing to cancel.

### 17.1 shutdown() — state-dispatched lifecycle operation (P1-05)

`shutdown()` is **not** a simple `request_stop() + drain() + join()` composition.
That composition is invalid in pre-Running states, because `drain()` and
`join()` return `invalid_state` in `Constructed`/`Starting` (§11.4). `shutdown()`
is instead a **state-dispatched** lifecycle operation that elects the single
close owner (§17.0) and runs the state-appropriate close path:

```text
shutdown():
    LOCK lifecycle_mutex:
        if close_state == Closed: UNLOCK; return success            // idempotent
        if close_state == InProgress: wait on runtime_cv for Closed; return success
        // else elect this caller as close owner:
        close_state = InProgress
    UNLOCK
    dispatch on the state observed at election:
```

| State at election | shutdown() close path |
| --- | --- |
| **Constructed** | No driver exists. Destroy constructed components in reverse order; publish `Stopped`; `close_state = Closed`. Return success. |
| **Starting** | Record `stop_requested` / `startup_abort_requested`; wake the startup barrier (§9 step 5c). The `start()` owner — which already holds the startup transaction — performs rollback + resource close and publishes `Stopped`. `shutdown()` waits on `runtime_cv` for `Closed` (it does **not** destroy resources itself; only the start owner may). Return success when `Stopped`. |
| **Running** | `request_stop()` (publishes root cancellation under `lifecycle_mutex`, P1-01) → `drain()` (wait for `drain_complete`) → driver join + resource close → `Stopped`. |
| **Stopping** | `drain()` → driver join + resource close → `Stopped`. |
| **Draining** | Wait for `drain_complete` (the active drain owner publishes it), then driver join + resource close → `Stopped`. |
| **StartFailed** | Synchronously close remaining constructed components in reverse order; publish `Stopped`. Return success. |
| **Stopped** | Idempotent success (`close_state == Closed`). |

**Required invariants (P1-05):**
```text
exactly one close owner across all of {join, shutdown, startup-abort, StartFailed, Constructed}
no two threads ever destroy components
final Stopped only after resource close
close_state transitions Open → InProgress → Closed (monotonic)
```

In `Constructed`/`StartFailed`, where there is no driver and no admitted work,
the close path is **not** `drain()+join()` — it is direct component destruction
under the close-owner election. This is why `shutdown()` cannot be defined as the
simple three-way composition.

### 17.2 join() — the post-drain terminal close

`join()` is legal **only after `drain_complete`** (i.e. in `Draining` with
`drain_complete` reached, or `Stopping` after a `drain()`). It returns
`IoError::invalid_state` if invoked from a task owned by the same Runtime
(Fiber-local tag) or before `drain_complete`. `join()` elects the close owner
exactly like `shutdown()` and runs the Running/Stopping tail (driver join +
resource close → `Stopped`):

```text
join():
    LOCK lifecycle_mutex:
        require drain_complete (else invalid_state)
        require not from a Runtime task (Fiber-local tag, else invalid_state)
        if close_state == Closed: UNLOCK; return success
        if close_state == InProgress: wait on runtime_cv for Closed; return success
        close_state = InProgress (this caller is the owner)
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
        close_state = Closed
        control_epoch++
    UNLOCK
    notify_all (runtime_cv)
    return success
```

`join()` return implies: **driver joined AND Scheduler invocation workers joined
AND backend destroyed AND backend workers joined AND Runtime == Stopped.**

Concurrent `join()`/`shutdown()` callers all route through the same `close_state`
— one owner, others wait for `Closed`. No two callers ever touch the driver or
resources simultaneously.

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
| 2 | state = Starting; startup_abort_requested = false (locked) | — | — |
| 3 | Spawn driver thread | `std::system_error` (thread creation) | state = StartFailed; no driver to join (joinable==false verified); return `backend_error` |
| 4 | Driver waits at startup barrier (predicate: state != Starting OR startup_abort_requested OR fatal); stop may arrive | stop wins pre-commit | Set `startup_abort_requested=true`; control_epoch++; release+notify (releases the barrier); join driver; full resource close; state = Stopped; start() returns `canceled` |
| 5 | Locked startup commit (state=Running, admission_open=true, recompute snapshot, control_epoch++) — ONLY if !stop_requested | Cannot fail (locked compound commit) | — |
| 6 | Release + wake driver (barrier release) | Cannot fail | — |

### 20.3 Failure-point analysis

| Failure point | Returned error | Resulting state | Threads joined | Objects destroyed | Retry permitted | Admission ever opened | Callbacks ran |
| --- | --- | --- | --- | --- | --- | --- | --- |
| stop_requested remembered in Constructed | `canceled` | Constructed | None | None | Yes (re-build) | No | No |
| Driver spawn failure | `backend_error` (preserves native code in `os_errno`) | StartFailed | None (joinable==false) | None | Yes (re-build) | No | No |
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
`control_epoch++` and dual-wake. This rolls back **Runtime admission accounting**;
**Group internal ownership** rollback requires the Group transactional admission
seam (§13.5, P2-01).

### 20.5 Throw-safe terminal guard

See §16.3. The `RuntimeTaskTerminalGuard` ensures `terminal_count++` happens
exactly once even if the user `RuntimeTaskFn` throws (RAII runs on unwind), so a
thrown task can never leave `terminal_count` short and block `drain()` forever.

### 20.6 Exception-to-IoError mapping (P2-02)

The Runtime uses a **mixed model**: public methods return `Result<void>` for
ordinary/expected failures, while invariant violations fail-fast
(`std::terminate`). The mapping is **deterministic and fixed** (no
"implementation-defined"):

| Failure | Result / behavior |
| --- | --- |
| Invalid configuration passed to `build()` | `Result` with `IoError::invalid_state` (`error.hpp:20`) |
| Wrong Runtime lifecycle state for the operation | `Result` with `IoError::invalid_state` |
| Same-Runtime task calls `drain()`/`join()`/`shutdown()` (detected via Fiber-local tag) | `Result` with `IoError::invalid_state` |
| `request_stop()` interrupts `start()` during Starting | `start()` returns `Result` with `IoError::canceled` (`error.hpp:15`) |
| Driver `std::thread` construction throws `std::system_error` (driver spawn) | `start()` returns `Result` with `IoError::backend_error` (`error.hpp:21`), preserving the native error code in `os_errno` when representable |
| `Scheduler::init_fiber()` returns false (Group converts it to a runtime error after transactional rollback) | `submit()` returns `Result` with `IoError::backend_error` |
| Group task-record allocation throws `std::bad_alloc` | **propagated as `std::bad_alloc`** (NOT mapped to `invalid_state`); any other documented internal allocation failure follows the same explicit propagation policy (never "implementation-defined") |
| User `RuntimeTaskFn` throws | Swallowed by the Group boundary (`group.hpp:251-256`) after the `RuntimeTaskTerminalGuard` fires; `submit()` already returned success — the exception is not returned by `submit()`. Runtime health is unaffected. |
| Async backend submit error (read/write/sync via `RuntimeTaskContext`) | Existing `Result<void>` propagated by `RuntimeTaskContext::submit_*` (delegating to the Runtime-owned `AsyncIoContext`) |
| Async completion error | Stored in the caller-owned `Completion<T>` under the existing foundation contract; not surfaced through `submit()` |
| Invariant violation (destructor misuse, quiescence failure, double-close) | Runtime-specific fail-fast (`runtime_lifetime_fail_fast`, PROPOSED) → `std::terminate` — NOT a `Result` |

**`std::bad_alloc` is NOT silently mapped to `invalid_state`.** Doing so would
conflate an out-of-memory condition with a misuse condition and lose information.
If the project later adopts a global no-throw allocation policy, this row would
change — but that would be an **OPEN HUMAN DECISION that blocks ADR acceptance**
until resolved, not a silent implementation choice.

**Exception specifications are stated accurately.** `request_stop()` is `noexcept`
(§15; it cannot propagate any exception). `build()`, `start()`, and `submit()` are
**NOT `noexcept`** because `std::bad_alloc` may propagate from them (per the row
above). `drain()`, `join()`, and `shutdown()` return `Result<void>` and do not
allocate in a way that throws `bad_alloc`.

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
    // Construction — owned indirection. ApplicationRuntime is non-copyable AND
    // non-movable, so it cannot be stored directly in Result<T> (which moves its
    // value into storage, result.hpp:155-189, and has no in-place immovable-value
    // facility). The factory returns unique ownership. The Runtime's stable heap
    // address also anchors driver-thread captures, the Fiber-local Runtime
    // identity tag, the lifecycle mutex/CV, and private component pointers.
    static Result<std::unique_ptr<ApplicationRuntime>>
    create(RuntimeConfig config);

    // Lifecycle
    Result<void> start();                    // may return canceled / invalid_state
    Result<void> submit(RuntimeTaskFn task); // admission-gated; invalid_state if closed
    void request_stop() noexcept;            // worker-safe; legal all non-Fatal states
    Result<void> drain();                    // invalid_state if from a Runtime task or in Running
    Result<void> join();                     // terminal close owner; invalid_state if from a Runtime task
    Result<void> shutdown();                 // state-dispatched lifecycle op (§17.1)

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
    // Owned indirection (P1-02): ApplicationRuntime is non-movable, so it cannot
    // be returned by value through Result<T> (result.hpp:155-189). build()
    // validates config, constructs ApplicationRuntime on the heap, and returns
    // unique ownership.
    Result<std::unique_ptr<ApplicationRuntime>> build();
};

class ApplicationRuntime {
public:
    Result<void> start();
    Result<void> submit(RuntimeTaskFn task);   // §21.5 RuntimeTaskFn/RuntimeTaskContext
    void request_stop() noexcept;
    Result<void> drain();
    Result<void> join();
    Result<void> shutdown();                    // §17.1 state-dispatched
    ~ApplicationRuntime();
};

}  // namespace sluice::async
```

### 21.3 Selected proposed API direction

**Sketch 2 (Builder + one-shot) is preferred.**

| Decision | Choice | Rationale |
| --- | --- | --- |
| Constructor vs factory | Builder (`build`) | Separates validation from construction; incremental config |
| **Ownership return type (P1-02)** | `Result<std::unique_ptr<ApplicationRuntime>>` | ApplicationRuntime is non-movable; `Result<T>` has no in-place immovable-value facility (`result.hpp:155-189`). Owned indirection returns unique ownership without moving the Runtime. The stable heap address anchors driver-thread captures, the Fiber-local Runtime identity tag, the lifecycle mutex/CV, and private component pointers. |
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
| Task function receives (P1-04) | `RuntimeTaskContext&` (§21.5) | The task must be able to submit the asynchronous I/O that the Runtime drives. `RuntimeTaskContext` exposes `cancel_token()` plus four `submit_*` methods (`submit_read`, `submit_write`, `submit_sync_data`, `submit_sync_all`) that delegate to the Runtime-owned `AsyncIoContext`; it is non-owning and valid only during the task body invocation. |

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

### 21.5 RuntimeTaskContext and RuntimeTaskFn (PROPOSED — P1-04)

A Runtime task must be able to submit the asynchronous I/O that the Runtime
drives. `void(CancelToken&)` alone cannot (Q6). The task capability contract is
therefore fixed as a restricted, non-owning execution context that exposes the
root `CancelToken` plus I/O submission that delegates to the Runtime-owned
`AsyncIoContext`.

```cpp
// PROPOSED — NOT AN EXISTING API
namespace sluice::async {

// Restricted, non-owning task execution context. Valid ONLY for the duration of
// one RuntimeTaskFn invocation. Does not expose a raw Scheduler, Group, backend,
// or unrestricted AsyncIoContext&.
class RuntimeTaskContext {
public:
    // The authoritative root Group token (§14). Cooperative cancel observation.
    CancelToken& cancel_token() noexcept;

    // I/O submission delegates to the Runtime-owned AsyncIoContext. These mirror
    // the existing AsyncIoContext submit surface; the op state (Completion<T>)
    // remains caller-owned and must stay alive and address-stable until reaped.
    Result<void> submit_read     (ReadOp op, Completion<std::size_t>& completion);
    Result<void> submit_write    (WriteOp op, Completion<std::size_t>& completion);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& completion);
    Result<void> submit_sync_all (SyncAllOp op, Completion<void>& completion);

    RuntimeTaskContext(const RuntimeTaskContext&) = delete;
    RuntimeTaskContext& operator=(const RuntimeTaskContext&) = delete;
};

using RuntimeTaskFn = std::function<void(RuntimeTaskContext&)>;

}  // namespace sluice::async
```

The exact callable wrapper type (`std::function` vs. a templated `submit<Fn>`)
is an implementation choice; the **capability contract** above is fixed.

**Constraints (P1-04):**

- `RuntimeTaskContext` is **non-owning** and valid **only during** the task body
  invocation. Storing, copying, or escaping it is a caller contract violation
  (and is prevented by type design: deleted copy, no public ctor).
- It exposes **no** raw `Scheduler&`, `Group&`, backend, or unrestricted
  `AsyncIoContext&`. I/O submission is funneled through the four `submit_*`
  methods, which delegate to the Runtime-owned `AsyncIoContext`.
- Cancellation is exposed through the authoritative root Group token
  (`cancel_token()`), preserving the single cancel authority (§14).
- It does **not** expose structured child submission in E16 v1
  (no `spawn`/`submit_child`).
- It cannot outlive the Fiber/task invocation; the Runtime task wrapper
  constructs it on entry and destroys it on exit (before the terminal guard
  fires).
- **Direct I/O submission after Runtime admission closes remains legal** for
  tasks already admitted, because per-task I/O progress must be drainable: a
  task admitted before `request_stop()` may continue submitting I/O after stop,
  and `drain()` cannot return until that I/O is reaped (`outstanding()==0`).
- **No new I/O operation may be submitted after the task body has exited.** The
  context is destroyed at task-body exit; any Completion left outstanding
  remains caller-owned and drain-relevant but no new submission is possible.

This resolves **Q6** (task I/O capability) and removes it from open decisions
(§26). `TaskFn = void(CancelToken&)` references throughout the design are
updated to `RuntimeTaskFn = void(RuntimeTaskContext&)`.

### 21.6 Foundation seam count (P2-01 correction)

E16 requires **five** private PROPOSED foundation seams (not four), because
`Group::async_evented` must gain a transactional admission seam before E16
admission rollback is correct (§13.5):

1. `runtime_lifetime_fail_fast` (§18.1)
2. Fiber-local execution-identity seam (§21.4)
3. `RuntimeTaskTerminalGuard` (§16.3)
4. `recompute_task_set_terminal_locked()` (§16.2)
5. **Group transactional admission seam** (§13.5) — new prerequisite

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
Then exactly one is elected close owner (close_state Open→InProgress)
And the other waits on runtime_cv for Closed
And the owner joins the driver, snapshots diagnostics, destroys resources, publishes Stopped
And the other returns success after Closed
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

### A13 — Stop publication versus resource close (P1-01)

```text
Given an empty Running Runtime
When request_stop() acquires lifecycle_mutex (pauses inside the critical section)
And a concurrent shutdown()/join() attempts terminal close
Then the join owner CANNOT destroy root_group before request_stop() releases lifecycle_mutex
And root cancellation (root_group.group_token().request()) is published while root_group is still alive
And no use-after-free is possible on the root CancelToken
And the join owner observes state==Stopping (not Stopped) and participates in the drain→close sequence
```

Deterministic trace (no wall-clock sleeps): the lifecycle authority is held by
`lifecycle_mutex`. `request_stop()` runs `group_token().request()` **inside** the
mutex; the join owner's resource close runs only after it observes a state that
permits close, which requires acquiring the same mutex. The close owner therefore
cannot overtake the cancellation publication.

### A14 — Non-movable Runtime ownership (P1-02)

```text
Given RuntimeBuilder::build()
Then the returned type is Result<std::unique_ptr<ApplicationRuntime>>
And the Runtime is constructed on the heap by the builder
And no move/copy construction of ApplicationRuntime is required to return it
And the resulting ApplicationRuntime has a stable address for the lifetime of the unique_ptr
```

### A15 — Stop-before-commit startup barrier (P1-03)

```text
Given the driver has been spawned and is waiting at the Starting barrier
When request_stop() wins before the startup commit
Then the barrier predicate (state != Starting OR startup_abort_requested OR fatal) is satisfied
And startup_abort_requested is set under lifecycle_mutex
And the Runtime CV and SchedulerWakeHandle are notified
And the driver is joined
And run_live is NEVER entered (driver observed state != Running at the barrier)
And admission never opens
And full resource close runs exactly once
And start() returns canceled
```

### A16 — Runtime task performs asynchronous I/O (P1-04)

```text
Given an admitted RuntimeTaskFn(RuntimeTaskContext&)
When the task submits an asynchronous read through RuntimeTaskContext::submit_read
Then the operation is owned and driven by the Runtime-owned AsyncIoContext
And drain() cannot return before that operation is reaped (outstanding()==0)
And no raw AsyncIoContext& / Scheduler& / Group& / backend reference is exposed to the task
And the RuntimeTaskContext is valid only for the duration of the task body invocation
And no I/O may be submitted after the task body has exited
```

### A17 — shutdown() in every safe state (P1-05)

```text
For each safe state in {Constructed, Starting, Running, Stopping, Draining, StartFailed, Stopped}:
When shutdown() is called from an external thread
Then exactly one close owner is elected (close_state Open → InProgress → Closed)
And every other concurrent caller waits on runtime_cv for Closed
And resources are destroyed exactly once
And the Runtime reaches Stopped
And shutdown() returns success
Specifically:
- Constructed: no driver exists; destroy constructed components; publish Stopped
- Starting: record startup_abort; wake the barrier; wait for the start owner's rollback+close
- Running: request_stop() + drain() + join()
- Stopping: drain() + join()
- Draining: join() after drain_complete (or wait for the drain owner)
- StartFailed: synchronously close remaining components; publish Stopped
- Stopped: idempotent success
```

### A18 — Group task-record insertion failure (P2-01)

```text
Given Group::async_evented with the future transactional admission seam
When failure is injected at each insertion/allocation point
  (Future alloc, stack alloc, Fiber alloc, init_fiber, vector push_back Nth)
Then the user task body NEVER executes
And Runtime admitted_count rolls back under lifecycle_mutex
And the Group retains no malformed partial task record (transactional seam: all-or-nothing)
And the Group remains safe to destroy
And a later async_evented submission remains valid
```

### A19 — Post-drain driver park (P2-03)

```text
Given a Runtime whose driver has published drain_complete
When no join/close owner has requested driver exit
Then the driver does NOT re-enter run_live in a busy loop
And the driver does NOT exit before the terminal owner requests it
And the driver parks on runtime_cv under lifecycle_mutex, waking only on:
    driver_exit_requested
    OR fatal_snapshot
    OR control_epoch != observed_epoch
When the join owner sets driver_exit_requested
Then the driver wakes, exits the loop, and becomes joinable
```

No wall-clock sleeps; correctness is established by the persistent
state/control-epoch predicate, not by notification timing.

### A20 — Invocation-boundary liveness on successful admission (P1-07)

```text
Given a Running Runtime whose driver has returned from run_live and is parked
  in the between_invocations state on the persistent CV predicate
When an external thread calls submit() with a valid RuntimeTaskFn
  and submit() succeeds (admission_open, admitted_count++)
Then the success path of submit() publishes control_epoch++ AND dual-wakes
  (runtime_cv.notify_all + scheduler_wake_handle.notify)
And the driver's between_invocations predicate fires on the bumped control_epoch
And the driver re-enters run_live, which polls the newly-published runnable Fiber
And the admitted task body executes (no stranded task)
And this holds even if the wake notification races the driver's wait
  (the persistent predicate, not the notification, is the liveness authority)
---
Given the same setup but submit() omits control_epoch++ on the success path
Then the driver's predicate never fires for the new work
And the task is stranded until an unrelated notification   (FORBIDDEN — mutation)
```

Deterministic, no wall-clock sleeps: the epoch is bumped under `lifecycle_mutex`
and the driver re-checks the predicate under the same mutex, so the wake cannot
be lost regardless of notification timing.

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
| submit wins before stop | `EventedGroupAdmissionPhase.AfterAggregateInsert` before `request_stop` |
| stop wins before submit | `request_stop` before `EventedGroupAdmissionPhase.BeforeAggregateInsert` |
| concurrent submit + stop | Barrier-synchronized threads at `AfterAggregateInsert` |
| `Group::async` throws after reservation | `EventedGroupAdmissionPhase.BeforeSpawn` inject throw; `admitted_count` rolled back, snapshot recomputed |

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

### 23.7 Driver re-entry loop and boundary liveness

| Test | Expected |
| --- | --- |
| run_live returns MW-S3, no work pending | Driver enters `between_invocations`, parks on the persistent CV predicate (§16.1b) |
| New successful `submit()` while driver parked in `between_invocations` | `submit()` success path publishes `control_epoch++` + dual-wake; the driver's predicate fires on the bumped epoch and re-enters `run_live`, polling the newly-published Fiber — no stranded task (A20, P1-07) |
| run_live returns with outstanding I/O | Driver re-enters `run_live` to poll/reap (outstanding decrements at poll reap) |
| run_live returns with task_set_terminal && outstanding()==0 | Driver publishes drain_complete, signals runtime_cv, enters `drained_wait` |

### 23.8 Dual-wake lost-wake race

| Test | Expected |
| --- | --- |
| Omit Runtime-CV notify only (between-invocation wake) | Driver misses the boundary wake — A20: admitted task stranded / drain blocked (mutation killer) |
| Omit `control_epoch++` on successful submit only | Driver's predicate never fires on new work — A20: stranded task (mutation killer, P1-07) |
| Omit SchedulerWakeHandle notify only (in-run_live wake) | Worker misses the in-`run_live` wake (mutation killer) |

### 23.9 Concurrent join/shutdown owner election

| Test | Expected |
| --- | --- |
| Two concurrent join() callers | One close owner (close_state InProgress); other waits for Closed; no double-join |
| Two concurrent shutdown() callers | One close owner; other waits; no double-destroy |
| shutdown() in Constructed | No driver; direct component destruction; publish Stopped; not drain()+join() (which would be invalid_state) |
| shutdown() in Starting | Records startup_abort; start owner rolls back+close; shutdown waits for Closed |
| shutdown() in StartFailed | Direct component close; publish Stopped |

### 23.10 Destructor misuse

| Test | Expected |
| --- | --- |
| Destructor in Running | Death test (runtime_lifetime_fail_fast) |
| Destructor in Stopped | Safe (no-op) |
| Destructor in StartFailed | Safe (destroy components) |
| Destructor in Constructed | Safe (destroy components) |

### 23.11 Deterministic test seams (P2-02)

Naming a test is insufficient: the acceptance/mutation plan needs **deterministic
private test seams** that pause, inject failure, or expose phase observation
without altering production state-machine semantics. These are PROPOSED private
test seams (guarded, non-installed, like the existing `SLUICE_ASYNC_INTERNAL_TESTING`
seams in `group.hpp:102-109`, `scheduler.hpp:1749-2165`,
`threadpool_backend.hpp:78`). They MUST NOT change production object layout or
exported behavior.

```text
EventedGroupAdmissionPhase:        # §13.5 Group transactional seam
    RecordConstructed
    BeforeAggregateInsert
    AfterAggregateInsert
    BeforeSpawn

RuntimeStartupPhase:               # §9 / P1-03
    StartingPublished
    BeforeDriverSpawn
    DriverBarrierEntered
    BeforeStartupCommit
    StartupAbortPublished
    BeforeAbortJoin

RuntimeDriverPhase:                # §16.1b / P1-07 / P2-03
    BeforeRunLive
    AfterRunLiveReturn
    BeforeRuntimeCvWait
    SuccessfulSubmitPublished
    DrainCompletePublished
    DrainedWaitEntered
    ExitRequestedObserved

RuntimeClosePhase:                 # §17 / P1-05
    CloseOwnerElected
    BeforeDriverJoin
    BeforeGroupDestroy
    AfterGroupDestroy
    BeforeStoppedPublication
```

A seam may: pause the production thread at a phase; inject a deterministic
failure (e.g. `bad_alloc` at a chosen allocation, `init_fiber` returning false,
`std::system_error` at driver spawn); or expose phase observation to the test.
No wall-clock sleeps: ordering is established by phase barriers, not timing.

**Every mutation has an exact killing test (named below), driven by these seams.**

| Mutation | Killing test (acceptance/unit) + seam used |
| --- | --- |
| non-movable Runtime returned by value (P1-02) | A14 — compile-time; no seam needed |
| root cancellation published after close becomes possible (P1-01) | A13 — `RuntimeClosePhase.BeforeGroupDestroy` races `request_stop`; assert cancel published before close |
| startup-abort wake omitted (P1-03) | A15 — `RuntimeStartupPhase.StartupAbortPublished` + `request_stop` at `BeforeStartupCommit`; assert driver joined, run_live never entered |
| driver enters run_live after abort (P1-03) | A15 — assert `RuntimeDriverPhase.BeforeRunLive` never reached on abort |
| successful submit omits Runtime epoch/wake (P1-07) | A20 — `RuntimeDriverPhase.BeforeRuntimeCvWait` + `SuccessfulSubmitPublished`; assert driver re-enters `run_live` |
| `RuntimeTaskContext` loses I/O capability (P1-04) | A16 — admitted task submits I/O; assert drain waits for `outstanding()==0` |
| Group partial task record survives failure (P2-01) | A18 — `EventedGroupAdmissionPhase.BeforeAggregateInsert`/`AfterAggregateInsert` inject `bad_alloc`; assert no malformed record |
| Runtime `admitted_count` rollback omitted | A1/A2 — `EventedGroupAdmissionPhase.BeforeSpawn` inject throw; assert `admitted_count` decremented, drain returns |
| terminal guard omitted | A9/A11 — task throws at body; assert `terminal_count++` still fires |
| `terminal_count` increments twice | A9 — task returns normally; assert exactly-once |
| `drain_complete` published before Group Future publication | A11 — assert `RuntimeDriverPhase.DrainCompletePublished` only after Group Future terminal |
| driver spins after `drain_complete` (P2-03) | A19 — observe `DrainedWaitEntered`, no `BeforeRunLive` until `ExitRequestedObserved` |
| driver exits before close requests exit (P2-03) | A19 — `DrainedWaitEntered` without `driver_exit_requested`; assert driver still joinable |
| two close owners (P1-05) | A17/A7 — two `shutdown()` at `CloseOwnerElected`; assert one owner |
| backend destroyed with outstanding I/O | A6/A12 — `RuntimeClosePhase.BeforeGroupDestroy` with `outstanding()>0`; assert fail-fast or drain first |
| ordinary `thread_local` used for Fiber identity | A10 — Fiber suspends/resumes; assert detection still correct |
| `bad_alloc` mapped to `invalid_state` (P2-01) | forbidden — verified by code review of §20.6; runtime probe: inject `bad_alloc`, assert propagation not `invalid_state` |
| `drain_complete` published from the invocation stop predicate | A6/A12 — assert `drain_complete` only published at `RuntimeDriverPhase` between-invocation authority, never inside `run_live` |

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
| Double-join the driver | A7: join is idempotent via close_state (no crash) |
| Omit reservation rollback on `Group::async` throw | A1: drain waits forever for never-running task |
| Terminal guard not noexcept / not exactly-once | A11: thrown task leaves terminal_count short |
| Omit `recompute_task_set_terminal_locked()` on admission rollback | Driver waits forever (snapshot stale) |
| Publish `drain_complete` directly from the invocation stop predicate, bypassing the driver's between-invocation authority check | Authority/lifecycle-proof violation (NOT a deadlock): the Runtime driver is the sole `drain_complete` publisher; the stop predicate does not consult `outstanding()` by design, and production's `global_mtx_ → access_mtx_` lock order permits `outstanding()` under `global_mtx_` anyway (`scheduler.cpp:1068-1071`). Killed by A6/A12 + the drain-authority invariant (§16.1). |
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
| Publish state=Stopping BEFORE root cancellation, then allow close (P1-01) | A13: use-after-free on root CancelToken during concurrent close |
| Return `ApplicationRuntime` by value despite non-movable type (P1-02) | A14: build() fails to compile / requires illegal move |
| Omit startup-barrier abort wake (P1-03) | A15: driver hangs at Starting barrier when stop wins pre-commit |
| Driver enters run_live after stop won pre-commit (P1-03) | A15: run_live entered on abort branch; admission opens |
| `RuntimeTaskContext` lacks I/O submission (P1-04) | A16: admitted task cannot submit the I/O the Runtime drives |
| shutdown() in Constructed calls drain()+join() and returns invalid_state (P1-05) | A17: shutdown in pre-Running states fails instead of closing |
| Two shutdown() callers both destroy components (P1-05) | A17/A7: double-destroy crash |
| Group second/third task-record insertion throws after earlier insertion (P2-01) | A18: Group retains malformed partial task record |
| Map `std::bad_alloc` to `invalid_state` (P2-02) | Mutation: OOM misclassified as misuse; no killing test (config/contract gap) — documented as forbidden |
| Driver busy-loops after drain_complete (P2-03) | A19: driver re-enters run_live in a spin with no new work |
| Driver exits immediately at drain_complete before join request (P2-03) | A19: driver gone before close owner can join it |
| Successful `submit()` omits `control_epoch++` (P1-07) | A20: admitted task stranded at the invocation boundary |
| Successful `submit()` omits the Runtime-CV notify (P1-07) | A20: driver parked in between_invocations misses the new work |
| Driver has no `between_invocations` park / re-enters run_live unconditionally (P1-07) | A20/A19: busy-loop, or lost wake at the boundary |
| Driver wait uses notification timing instead of the persistent epoch predicate (P1-07) | A20: lost wake strands an admitted task |

Every mutation has an identified acceptance or unit test that kills it (the
`std::bad_alloc`-to-`invalid_state` row is a forbidden mutation verified by code
review of the mapping table §20.6, not by a runtime test).

## 25. Fuzz/formal applicability

### 25.1 State variables

```text
runtime_state      ∈ {Constructed, Starting, Running, Stopping, Draining, Stopped, StartFailed, Fatal}
admission_open     ∈ {true, false}
stop_requested     ∈ {true, false}   (monotonic)
root_cancel_published ∈ {true, false} (monotonic; set when root_group.group_token().request() runs)
startup_abort_requested ∈ {true, false} (set by request_stop during Starting; releases the barrier)
admitted_count     ≥ 0
terminal_count     ≥ 0
control_epoch      ≥ 0               (monotonic uint64)
driver_state       ∈ {not_started, barrier_wait, in_run_live, between_invocations,
                      drained_wait, exiting, exited}
close_state        ∈ {Open, InProgress, Closed}   (unified terminal-close ownership, §17.0)
resources_alive    ∈ {true, false}   (true while Group/Scheduler/AsyncIoContext/backend exist)
runtime_task_io_outstanding ≥ 0      (Runtime-task-submitted I/O counted in outstanding_io)
outstanding_io     ≥ 0
execution_tag_per_fiber  (per-Fiber Runtime identity)
```

### 25.2 Invariants

```text
task_set_terminal_snapshot == (!admission_open && admitted_count == terminal_count)
drain_complete => task_set_terminal_snapshot && outstanding_io == 0
resources_alive == false => root_cancel publication cannot access Group
                            (P1-01: cancel published under lifecycle_mutex before close)
Stopped => resources_alive == false
Stopped => close_state == Closed
admission_open => startup commit happened && !stop_requested_at_commit
at most one driver thread ever live
at most one close owner (close_state transitions Open -> InProgress -> Closed)
startup_abort_requested => driver never enters run_live   (P1-03)
build success does not require moving ApplicationRuntime  (P1-04: unique_ptr ownership)
drain_complete => admitted == terminal && outstanding_io == 0
Runtime task I/O is submitted only through RuntimeTaskContext (P1-04)
no Runtime task I/O submitted after task body exit (P1-04)
join() returns => driver joined && Scheduler workers joined && backend destroyed
Fiber execution tag invariant across suspend/resume/migration
a rejected task never executes

# Driver FSM liveness (P1-07)
successful submit() => control_epoch++ && dual-wake   (success path, not only rollback)
every control-changing op (stop, submit success/rollback, terminal-guard,
    startup abort/commit, close-owner driver_exit) => control_epoch++ && CV notify
driver_state ∈ {between_invocations, drained_wait} =>
    driver waits on the persistent predicate:
        driver_exit_requested OR fatal_snapshot OR control_epoch != observed_epoch
    (checked under lifecycle_mutex; the epoch term, not the notify, is the authority)
driver_state ∈ {between_invocations, drained_wait} =>
    no busy re-entry without a control_epoch change
admitted task with runnable Fiber eventually observed by run_live
    (no stranded task at the invocation boundary)
driver_state == drained_wait => principal wake is driver_exit_requested (close owner)
```

### 25.3 Formal impact

```text
MODEL_REQUIRED
```

**Reason:** The lifecycle state machine has 8 states, multiple concurrent
operations (submit, request_stop, drain, join, shutdown), a non-trivial admission
linearization rule, a driver re-entry loop, a dual-wake protocol, and a unified
terminal-close owner election. A TLA+ state model is **required** (not merely
recommended) to catch counterexamples in:
- Admission race ordering (submit vs. request_stop, reservation+rollback).
- Drain completeness (task terminal AND outstanding==0).
- Driver re-entry lost-wake (invocation-boundary race, P1-07).
- Destructor safety (only Constructed/StartFailed/Stopped safe; Stopped ⇒ resources destroyed).
- Startup rollback / stop-pre-commit (no surviving driver on failure; run_live never entered on abort).
- Unified terminal-close owner election (no double-join/double-destroy across all close paths).
- Successful-admission boundary wake (P1-07: epoch++ + dual-wake ⇒ no stranded task).

**Required before ADR acceptance (P2-03):**

```text
1. A passing E16 Runtime lifecycle TLA+ model.
2. At least one deliberate buggy/negative model.
3. A TLC counterexample for a known broken transition
   (e.g. invocation-boundary lost-wake, or stop-vs-close use-after-free).
4. Final model transitions match the design and ADR.
```

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
counterexample discipline (e.g. `scripts/verify-timer-wait-formal.sh`,
`scripts/verify-async-mutex-formal.sh`, `scripts/run-async-tlc-all.sh`). The E16
model MUST follow the same discipline: a correct model plus a negative/broken
model that reproduces a known defect.

**ADR acceptance remains blocked until the required model exists and passes.**
No TLA+ model files are authored in this documentation pass (scope is
documentation-only); the model is an explicit, required prerequisite, not a
recommendation. The minimum variables (§25.1) and invariants (§25.2) are
sufficient to express the core contract.

## 26. Open questions

| ID | Question | Status | Impact |
| --- | --- | --- | --- |
| Q1 | Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** | If yes, a separate "internal admission" path is needed; if no, all work goes through the gate. |
| Q2 | Should `submit()` return a handle/Future for the admitted task? | **OPEN HUMAN DECISION** | Affects API shape; a handle enables per-task await/cancel. |
| Q3 | Should the Runtime expose a diagnostics snapshot (task count, worker count, state)? | **OPEN HUMAN DECISION** | Useful for monitoring; adds API surface; must snapshot before resource close. |
| Q4 | Should `drain()` have a deadline? | **OPEN HUMAN DECISION** | A deadline prevents indefinite blocking but adds timer dependency. |
| Q5 | Should the Runtime support Threaded mode (std::thread workers) in addition to Evented? | **OPEN HUMAN DECISION** | The existing Group supports both modes (`group.hpp:47-54`); the Runtime could expose both. |

Q6 (task I/O capability) is **resolved** in this document (§21.5
`RuntimeTaskContext`: `RuntimeTaskFn = void(RuntimeTaskContext&)`) and removed
from open questions. Q7 (wake-epoch design) and Q8 (cancellation error code) are
**resolved** in this document (§15 control_epoch; §9/§11 `canceled` vs
`invalid_state`) and removed from open questions.

**No implementation-blocking contract gap may remain open** (P1-04 / final
open-decision policy, §26.1). The remaining Q1–Q5 are optional product decisions
that do not block the architecture.

### 26.1 Final open-decision policy

A Proposed ADR may retain optional product decisions, but **not**
implementation-blocking contract gaps.

**Allowed to remain open** (optional product decisions):

```text
whether submit returns a task handle (Q2)
whether diagnostics snapshots are public (Q3)
whether drain later gains a deadline (Q4)
whether Threaded mode is supported later (Q5)
whether internal cleanup bypasses admission, provided E16 v1 does not need it (Q1)
```

**NOT allowed to remain open** (implementation-blocking contract gaps, all
resolved in this revision):

```text
Runtime ownership return type      → Result<unique_ptr<ApplicationRuntime>> (P1-02)
Task I/O capability                → RuntimeTaskContext (P1-04)
stop/start barrier behavior        → startup barrier + abort path (P1-03)
shutdown behavior by state         → state-dispatched (§17.1) (P1-05)
terminal-close ownership           → unified close_state (§17.0) (P1-05)
error mapping for documented failures → fixed table (§20.6) (P2-02)
Group transactional admission prerequisite → listed (§13.5) (P2-01)
```

## 27. Implementation slices

If authorized, implementation would proceed in order:

1. **S0 (PREREQUISITE): Group transactional admission seam** — `Group::async_evented`
   must gain a strongly exception-safe / aggregate task-record insertion before
   E16 admission rollback is correct (§13.5, P2-01). **Blocks S6.**
2. **S1: Builder + config validation** — `RuntimeBuilder`, `RuntimeConfig`,
   validation; `build()` returns `Result<std::unique_ptr<ApplicationRuntime>>` (P1-02).
3. **S2: Owned-object construction** — `ApplicationRuntime` (heap, stable address)
   owns AsyncIoContext, Scheduler, root Group; acquire wake handle.
4. **S3: Lifecycle mutex + control_epoch + atomic snapshots + close_state** —
   state/admission/stop_requested/counts under lifecycle_mutex; 3 atomic
   snapshots; recompute helper; `close_state {Open,InProgress,Closed}`;
   `startup_abort_requested`; `root_cancel_published`.
5. **S4: `start()` transaction** — locked transitions; driver spawn; startup
   barrier predicate (`state != Starting || startup_abort_requested || fatal`);
   startup commit OR abort path; driver behavior after barrier (P1-03).
6. **S5: Driver re-entry loop + dual wake + post-drain park** — release-then-notify;
   between-invocation `outstanding()` check; `drain_complete` publication; post-drain
   park on `runtime_cv` until `driver_exit_requested`/`fatal`/epoch change (P2-03).
7. **S6: Admission gate** — reservation + rollback (Runtime accounting); relies on S0
   for Group ownership rollback (§13.5).
8. **S7: `request_stop()`** — 4-case linearization; locked compound commit; root
   cancel via `group_token().request()` **published under `lifecycle_mutex`** (P1-01).
9. **S8: `RuntimeTaskTerminalGuard` + Fiber-local execution tag +
   `RuntimeTaskContext`** — throw-safe terminal; per-Fiber identity; worker-call
   detection; restricted task I/O capability delegating to AsyncIoContext (P1-04).
10. **S9: `drain()`** — legal only Stopping/Draining; wait for drain_complete.
11. **S10: `join()` (post-drain close path)** — close_state owner election; join
    driver; resource close; publish Stopped (§17.2).
12. **S11: `shutdown()` (state-dispatched close)** — one close owner across all
    paths; per-state dispatch including Constructed/Starting/StartFailed
    direct close (§17.1, P1-05).
13. **S12: Destructor** — per-state safety; multi-clause predicate;
    `runtime_lifetime_fail_fast`.
14. **S13: Tests** — acceptance contracts A1-A20, unit tests, mutation tests,
    deterministic phase seams (§23.11).
15. **S14: Formal model** (MODEL_REQUIRED, P2-03) — TLA+ correct + negative
    model + TLC counterexample; variables per §25.1, invariants per §25.2. A
    required ADR-acceptance prerequisite.

## 28. Implementation authorization status

```text
E16 production implementation remains unauthorized.
Authorization requires:
  - an accepted ADR (currently Proposed);
  - the required TLA+ lifecycle model existing and passing (MODEL_REQUIRED, P2-03);
  - an independent design review with no open P0/P1 or mandatory-contract findings.
```

## 29. Evidence index

| Claim | Evidence |
| --- | --- |
| Scheduler borrows AsyncIoContext | `scheduler.hpp:213` |
| Scheduler non-movable | `scheduler.hpp:216-219` |
| Scheduler::run/run_live block caller | `scheduler.hpp:241,249`; `scheduler.cpp:495-584` |
| Workers created AND joined inside run_impl | `scheduler.cpp:565-579` |
| run_live stop predicate at MW-S3 boundary | `scheduler.cpp:856-857` |
| run_live returns on QUIESCENT / MW-S3-no-wake / stop-predicate-true (invocation boundaries) | `scheduler.cpp:823-918` (key termination at 891-897) |
| Scheduler::spawn publishes to worker inbox, notifies inbox_cv only (NOT Runtime CV, NOT control_epoch) | `scheduler.cpp:419-441` |
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
| TLC checker discipline | `scripts/verify-timer-wait-formal.sh` et al.; `scripts/run-async-tlc-all.sh` |
