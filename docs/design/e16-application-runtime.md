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
`Scheduler`, root task domain, root cancellation, and worker-thread lifecycle, and
exposes a unified `start / submit / request_stop / drain / join / shutdown` contract.

The preferred architecture is **Alternative B/C: injected backend + builder-constructed
one-shot Runtime**. The Runtime owns its `AsyncIoContext` and `Scheduler`; the backend
is injected (enabling deterministic test injection). Construction is separated from
`start()`; `start()` is a transaction with rollback. The lifecycle is one-shot
(Constructed → Running → Stopped). The destructor requires explicit `shutdown()` and
fail-fast on misuse.

**Implementation authorization: DENIED.** This task produces architecture
documentation and a Proposed ADR only.

## 3. Problem statement

The current Sluice async foundation provides powerful primitives but no unified
lifecycle authority. A consumer who wants to run evented tasks today must manually:

1. Construct an `AsyncIoContext` with a backend (`async_io_context.hpp:121`).
2. Construct a `Scheduler` borrowing that context (`scheduler.hpp:213`).
3. Construct a `Group(Scheduler&)` for the root task set (`group.hpp:78`).
4. Call `Group::async()` to admit tasks (`group.hpp:94`).
5. Call `Group::await()` which drives `Scheduler::run_live(1, stop_predicate)`
   (`group.cpp:69`).
6. Rely on the caller to ensure correct destruction order.

This manual composition has gaps that the E16 Application Runtime must close:

- **No unified start.** `Scheduler::run()` / `run_live()` blocks the caller
  (`scheduler.hpp:241,249`). There is no non-blocking "launch workers and return"
  entry that an application can later stop.
- **No admission control.** `Group::async()` always succeeds (or throws); there is
  no gate that rejects submission after a stop request.
- **No runtime-level cancellation.** Cancellation is per-`Group` (`group.hpp:114`)
  or per-`Future` (`future.hpp:89`); there is no root cancel state that an
  application can publish to stop all work.
- **No startup rollback.** If the Nth worker fails to start, there is no mechanism
  to join already-started workers and leave the Runtime in a clean `StartFailed`
  state.
- **No drain/join separation.** `Group::await()` conflates "wait for tasks" with
  "drive the scheduler"; there is no independent drain-of-admitted-work vs.
  join-of-worker-threads distinction.
- **No destructor contract.** `Scheduler::~Scheduler()` asserts quiescence
  (`scheduler.cpp:169-196`); `Group::~Group()` fail-fast on pending evented tasks
  (`group.cpp:117-122`). But there is no single owner that enforces the correct
  shutdown sequence before destruction.

The E16 Application Runtime exists to close these gaps.

## 4. Goals

1. Define a unified lifecycle: construct → start → run → stop → drain → join → destroyed.
2. Own and order the construction/destruction of `AsyncIoContext`, `Scheduler`,
   root task domain, and root cancellation.
3. Provide an admission contract with a deterministic linearization point.
4. Provide separate, non-conflated `request_stop`, `drain`, `join`, `shutdown`.
5. Define startup as a transaction with rollback at every fallible step.
6. Define a destructor contract that prevents resource leaks and undefined behavior.
7. Support deterministic test injection of backends.
8. Remain a thin layer above the existing foundation — no new primitives, no
   scheduler changes, no public API re-semanticization.

## 5. Non-goals

| Non-goal | Decision | Rationale |
| --- | --- | --- |
| Restartable Runtime | **REJECTED** (E16) | One-shot lifecycle is the default. Restart requires backend/Scheduler reconstruction, worker generation, cancellation generations, stale references — all out of scope for E16. |
| Multiple independent root runtimes sharing one Scheduler | **REJECTED** (E16) | Scheduler is non-movable, non-copyable (`scheduler.hpp:218-219`). Sharing would require lifetime coordination that the Runtime does not provide. |
| Dynamic backend replacement | **REJECTED** (E16) | `AsyncIoContext` is move-only; backend is injected at construction (`async_io_context.hpp:121`). No hot-swap. |
| Hot worker-count resizing | **REJECTED** (E16) | Worker count is fixed at `start()` time. |
| Cross-process runtime management | **REJECTED** (E16) | Single-process only. |
| Durable work queue | **REJECTED** (E16) | Future consumer, not part of E16-A0. |
| Network server framework | **REJECTED** (E16) | Out of scope. |
| Structured logging framework | **REJECTED** (E16) | Out of scope. |
| Plugin system | **REJECTED** (E16) | Out of scope. |
| Global singleton Runtime | **REJECTED** (E16) | No globals (AGENTS.md §7: no new globals). |
| Non-Linux backend completion | **REJECTED** (E16) | Linux-only for E16. |
| Stable ABI | **REJECTED** (E16) | Header-only C++20, no ABI stability. |
| Automatic recovery after fatal invariant violation | **REJECTED** (E16) | Fail-fast is the contract (`fail_fast.cpp`). |

## 6. Current production inventory

### 6.1 Component table

| Component | Header | Role |
| --- | --- | --- |
| `Scheduler` | `include/sluice/async/scheduler.hpp:211` | Multi-worker Evented scheduler. Owns WorkerState vector, global_mtx_, timer subsystem, wake source. Borrows AsyncIoContext. |
| `Fiber` | `include/sluice/async/fiber.hpp:60` | Task state machine (created→runnable→running→waiting→done). Caller-owned. |
| `Group` | `include/sluice/async/group.hpp:57` | Unordered task set. Threaded (std::thread/task) or Evented (Fiber/task). Cancel-propagation boundary. |
| `Future<T>` | `include/sluice/async/fiber.hpp:46` | Single-task awaitable. Producer: complete_with. Consumer: await/cancel. |
| `CancelToken` | `include/sluice/async/cancel.hpp:47` | Cooperative cancel-request state. Idempotent request(). |
| `CancelState` | `include/sluice/async/cancel.hpp:88` | Per-consumer protection + acknowledgement. |
| `AsyncBackend` | `include/sluice/async/async_io_context.hpp:55` | Internal backend boundary (abstract). submit/poll/wait_one/cancel/outstanding. |
| `AsyncIoContext` | `include/sluice/async/async_io_context.hpp:118` | Public L1. Owns backend (unique_ptr). Routes submit/poll/wait_one/cancel. Move-only. |
| `Completion<T>` | `include/sluice/async/completion.hpp:70` | Caller-owned op state. idle→outstanding→ready. Non-movable. |
| `Batch` | `include/sluice/async/batch.hpp:69` | Grouped completions driver over AsyncIoContext. |
| `ThreadPoolBackend` | `include/sluice/async/threadpool_backend.hpp:55` | Portable real backend. One thread per op. |
| `SchedulerWakeHandle` | `include/sluice/async/scheduler.hpp:95` | External wake handle. Outlives Scheduler. |
| `EventedWaitPolicy` | `include/sluice/async/evented_wait_policy.hpp:42` | Evented Future wait via Scheduler. Borrows Scheduler. |
| `BlockingIoPool` | `bench/support/blocking_io_pool.cpp` | Bounded OS-thread helper. Not the async runtime. |

### 6.2 Primitive ownership/lifecycle inventory

| Field | Scheduler | Fiber | Group | Future | CancelToken | AsyncIoContext | Completion | ThreadPoolBackend |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Existing role** | Multi-worker Evented scheduler | Task state machine | Unordered task set | Single-task awaitable | Cancel request state | L1 op routing | Op state machine | Portable backend |
| **Construction** | `explicit Scheduler(AsyncIoContext&)` — borrows ctx (`scheduler.hpp:213`) | `Fiber()` or `Fiber(Entry)` (`fiber.hpp:64-65`) | `Group()` or `Group(Scheduler&)` (`group.hpp:61,78`) | `Future()` or `Future(WaitPolicy&)` (`future.hpp:53-54`) | `CancelToken()` (`cancel.hpp:49`) | `explicit AsyncIoContext(unique_ptr<AsyncBackend>, AsyncStats*)` (`async_io_context.hpp:121`) | `Completion()` (`completion.hpp:79`) | `ThreadPoolBackend()` (`threadpool_backend.hpp:57`) |
| **Owner today** | Caller (stack/heap) | Caller/Group | Caller | Caller/Group (shared_ptr) | Caller/Future/Group | Caller | Caller | AsyncIoContext (unique_ptr) |
| **Threads** | Creates/joins workers in `run()`/`run_live()` (`scheduler.hpp:241,249`) | None | Threaded: std::thread/task; Evented: none | None | None | None (backend may) | None | Spawns/joins per-op threads (`threadpool_backend.hpp:101`) |
| **Drive model** | Caller calls `run()`/`run_live()` | Scheduler drives state machine | Caller calls `await()`/`cancel()` | WaitPolicy drives physical wait | Consumer observes | Caller/batch drives | Backend marks ready | AsyncIoContext drives |
| **Shutdown support** | `global_terminate_` atomic; workers joined in run (`scheduler.hpp:1175`) | make_done (`fiber.hpp:101`) | `cancel()` requests+awaits (`group.hpp:125-128`); destructor drains (Threaded) / fail-fast (Evented) (`group.cpp:110-144`) | `cancel()` requests+awaits (`future.hpp:113-116`) | `request()` idempotent (`cancel.hpp:59`) | Destructor fail-fast if outstanding (`async_io_context.cpp:30-41`) | `reset()` (`completion.hpp:125-129`) | `destroying_` gate; destructor joins (`threadpool_backend.hpp:101-103`) |
| **Outstanding-work contract** | Destruction asserts quiescence: no ACTIVE Select, active_deadline_count_==0, waiting_select_count_==0 (`scheduler.cpp:169-196`) | done is absorbing (`fiber.hpp:46`) | Evented destructor fail-fast if pending futures (`group.cpp:117-122`) | ready is absorbing (`future.hpp:70`) | None | Destruction fail-fast if outstanding Completions (`async_io_context.cpp:30-41`) | Must not destroy while outstanding (`completion.hpp:11-12`) | Destruction joins workers |
| **Cancellation relationship** | Provides wait-cancellation (cancel_wait); doesn't own task cancellation | Owns per-fiber CancelToken + CancelState (`fiber.hpp:111-112`) | Owns shared CancelToken; cancel-propagation boundary (`group.hpp:114,125-128`) | Owns CancelToken (`future.hpp:89`) | Is the request state | Routes cancel to backend | None | Best-effort cancel |
| **Move/copy** | Non-copyable, non-movable (`scheduler.hpp:216-219`) | Non-copyable, non-movable (`fiber.hpp:67-70`) | Non-copyable, non-movable (`group.hpp:82-85`) | Non-copyable, non-movable (`future.hpp:56-59`) | Non-copyable, non-movable (`cancel.hpp:51-54`) | Non-copyable, move-only (`async_io_context.hpp:125-133`) | Non-copyable, non-movable (`completion.hpp:84-87`) | Non-copyable (`async_io_context.hpp:58-59`) |
| **Error channel** | Fail-fast (std::terminate) on invariant violation | State machine | Tasks swallow exceptions (`group.cpp:177,254`) | Result<T> channel | is_requested() | Result from submit; fail-fast on destruction | Result<T> channel | Result from submit |
| **Test-only seams** | `AsyncTestAccess` under `SLUICE_ASYNC_INTERNAL_TESTING` (`scheduler.hpp:1749-2165`) | None | `test_set_tasks_throw_on_nth` under `SLUICE_ASYNC_INTERNAL_TESTING` (`group.hpp:102-109`) | None | None | None | None | `shutting_down_for_test()` (`threadpool_backend.hpp:78`) |
| **Runtime suitability** | **OWN** — core of the Runtime | **WRAP** — owned by Group/Runtime | **OWN** (root Group) — the task admission domain | **WRAP** — owned by tasks/Group | **OWN** (root token) — root cancellation | **OWN** — owns backend | **EXCLUDE** — caller-owned | **INJECT** — backend implementation |

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
| No unified start | `Scheduler::run()` blocks caller (`scheduler.hpp:241`); no non-blocking launch | Cannot start runtime and later stop it from the same thread |
| No admission control | `Group::async()` always admits or throws (`group.hpp:94`); no gate | Cannot reject submission after stop |
| No runtime-level cancellation | Cancellation is per-Group (`group.hpp:114`) or per-Future (`future.hpp:89`) | No single "stop all" signal |
| No startup rollback | No multi-step startup sequence exists | Nth worker failure leaves partial state |
| No drain/join separation | `Group::await()` conflates task completion with scheduler driving (`group.cpp:41-91`) | Cannot independently drain work vs. join threads |
| No destructor contract | `Scheduler::~Scheduler()` asserts quiescence (`scheduler.cpp:169-196`); `Group::~Group()` fail-fast (`group.cpp:117-122`); no single owner enforces sequence | Manual composition errors → std::terminate |

## 7. Architecture alternatives

### Alternative A — Runtime-owned backend

The Runtime creates the backend internally based on configuration.

```text
ApplicationRuntime
├── backend (created internally from config)
├── AsyncIoContext (owns backend)
├── Scheduler (borrows AsyncIoContext)
├── root Group(Scheduler&)
├── root CancelToken
└── worker/control state
```

| Dimension | Analysis |
| --- | --- |
| Construction order | 1. Create backend from config → 2. Create AsyncIoContext → 3. Create Scheduler → 4. Create root Group → 5. Create root CancelToken |
| Destruction order | Reverse: 1. Stop/drain/join → 2. Destroy Group → 3. Destroy Scheduler → 4. Destroy AsyncIoContext → 5. Destroy backend |
| Backend testability | **POOR** — cannot inject FakeAsyncBackend; deterministic testing requires internal backend creation hooks |
| Partial-start rollback | Must track which steps completed; destroy in reverse |
| Cancellation owner | root CancelToken owned by Runtime |
| Admission owner | root Group |
| Drain authority | root Group::await() |
| Public exposure | Could expose Scheduler& / Group& accessors |
| Misuse resistance | Backend choice fixed at construction |
| Complexity | Low (all internal) |
| Foundation changes | None |
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
├── root CancelToken
└── worker/control state
```

| Dimension | Analysis |
| --- | --- |
| Construction order | 1. Inject backend → 2. Create AsyncIoContext → 3. Create Scheduler → 4. Create root Group → 5. Create root CancelToken |
| Destruction order | Reverse of construction after stop/drain/join |
| Backend testability | **GOOD** — inject FakeAsyncBackend (deterministic) via existing `AsyncIoContext(unique_ptr<AsyncBackend>)` seam (`async_io_context.hpp:121`) |
| Partial-start rollback | Same as A; injected backend is the first step |
| Cancellation owner | root CancelToken owned by Runtime |
| Admission owner | root Group |
| Drain authority | root Group::await() |
| Public exposure | Same as A |
| Misuse resistance | Backend injected; config validated before start |
| Complexity | Medium (injection seam) |
| Foundation changes | None |
| Compatibility | No effect on existing users |
| Verification | Good — deterministic backend injection via existing seam |

### Alternative C — Builder + one-shot Runtime

A `RuntimeBuilder` collects configuration; `build()` validates and returns a
constructed (but not started) Runtime. `start()` is a separate, fallible transaction.

```text
RuntimeBuilder
├── backend (injected)
├── worker_count
├── config validators
└── build() → ApplicationRuntime (Constructed, not started)

ApplicationRuntime (after build)
├── AsyncIoContext
├── Scheduler
├── root Group
├── root CancelToken
└── start() → Result<void> (transaction with rollback)
```

| Dimension | Analysis |
| --- | --- |
| Construction order | 1. Builder collects config → 2. `build()` validates + constructs owned objects → 3. Returns Runtime in `Constructed` state |
| Destruction order | `shutdown()` then destructor (fail-fast if not shutdown) |
| Backend testability | **GOOD** — same injection seam as B |
| Partial-start rollback | `start()` is a transaction; rollback at each step |
| Cancellation owner | root CancelToken owned by Runtime |
| Admission owner | root Group; admission opens only after `start()` commits |
| Drain authority | Runtime::drain() |
| Public exposure | Builder exposes config; Runtime exposes lifecycle ops |
| Misuse resistance | **BEST** — separation of construct/start prevents use-before-start; builder validates config |
| Complexity | Medium-high (builder + lifecycle state machine) |
| Foundation changes | None |
| Compatibility | No effect on existing users |
| Verification | **BEST** — builder enables config validation tests; start() transaction enables rollback tests |

### 7.4 Decision matrix

| Dimension | A: Runtime-owned | B: Injected | C: Builder + one-shot |
| --- | --- | --- | --- |
| Backend testability | POOR | GOOD | GOOD |
| Misuse resistance | MEDIUM | MEDIUM | **BEST** |
| Startup rollback clarity | MEDIUM | MEDIUM | **BEST** |
| Config validation | MEDIUM | MEDIUM | **BEST** |
| Complexity | LOW | MEDIUM | MEDIUM-HIGH |
| Foundation changes | None | None | None |
| Verification | HARD | GOOD | **BEST** |
| API cleanliness | MEDIUM | MEDIUM | **BEST** |

### 7.5 Preferred architecture

**Alternative C (Builder + one-shot Runtime) is preferred**, with backend injection
from Alternative B.

Rationale:
- **Testability** is mandatory. The existing test infrastructure relies on
  `FakeAsyncBackend` injection (`examples/async_foundation_quickstart.cpp:17-28`).
  Alternative A cannot inject deterministic backends without internal hooks that
  would weaken production guarantees.
- **Misuse resistance** is a hard constraint (task prompt §2.4: "A Runtime must
  establish lifecycle authority and invariants"). The construct/start separation
  prevents the common error of using a Runtime before it is started.
- **Startup rollback** is a hard constraint (task prompt §15: "No partially started
  background worker may survive a failed start()"). A transactional `start()` with
  rollback is the cleanest expression.
- **Verification** is a hard constraint (task prompt §17-19). The builder pattern
  enables config-validation tests; the one-shot lifecycle enables deterministic
  state-transition tests.

## 8. Ownership graph (preferred)

```text
ApplicationRuntime
├── owns → AsyncIoContext (unique_ptr)
│         └── owns → AsyncBackend (unique_ptr, injected)
├── owns → Scheduler (unique_ptr, borrows *async_io_context)
├── owns → root Group (unique_ptr, Group(scheduler))
│         ├── owns → root CancelToken (shared across root tasks)
│         ├── owns → EventedWaitPolicy (borrows *scheduler)
│         ├── owns → Future<void> per task (shared_ptr)
│         ├── owns → Fiber per task (unique_ptr)
│         └── owns → Stack per task (unique_ptr<byte[]>)
├── owns → root CancelToken (runtime-level cancellation)
├── owns → worker/control state
│         ├── admission_open atomic<bool>
│         ├── state atomic<RuntimeState>
│         └── worker threads (created in start(), joined in join())
└── owns → RuntimeConfig (worker_count, backend config)
```

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
   e. Construct root CancelToken
   f. State = Constructed
   g. Admission = CLOSED
5. Consumer calls runtime.start()
   a. Transition Constructed → Starting
   b. Validate preconditions (admission == CLOSED, no workers)
   c. Open admission gate
   d. Transition Starting → Running
   e. (Workers are driven by root Group's await → Scheduler::run_live)
```

Evidence for construction order:
- `AsyncIoContext` must be constructed before `Scheduler` (Scheduler borrows it: `scheduler.hpp:213`).
- `Scheduler` must be constructed before `Group(Scheduler&)` (Group borrows it: `group.hpp:78`).
- `Group` must be constructed before tasks are admitted (`group.hpp:94`).

## 10. Destruction order

```text
1. Consumer calls runtime.shutdown()  (or relies on destructor fail-fast)
   a. request_stop() — close admission, publish root cancellation
   b. drain() — wait for all admitted tasks terminal
   c. join() — join worker threads
   d. Transition → Stopped
2. Destructor (state must be Stopped)
   a. Validate state == Stopped (else fail-fast)
   b. Destroy root Group (must be empty/drained)
   c. Destroy Scheduler (must be quiescent)
   d. Destroy AsyncIoContext (must have zero outstanding)
   e. Destroy backend
```

Evidence for destruction order:
- `Group::~Group()` fail-fast if Evented futures pending (`group.cpp:117-122`).
- `Scheduler::~Scheduler()` asserts quiescence (`scheduler.cpp:169-196`).
- `AsyncIoContext::~AsyncIoContext()` fail-fast if outstanding (`async_io_context.cpp:30-41`).

## 11. Lifecycle state machine

### 11.1 States

| State | Meaning |
| --- | --- |
| `Constructed` | Built but not started. Admission closed. No workers. |
| `Starting` | `start()` in progress. Admission not yet open. |
| `Running` | Started. Admission open. Workers active. |
| `Stopping` | `request_stop()` called. Admission closed. Root cancellation published. |
| `Draining` | `drain()` in progress. Waiting for admitted tasks to reach terminal state. |
| `Stopped` | Drained + joined. Safe to destroy. |
| `StartFailed` | `start()` failed. Rolled back. No surviving workers. Safe to destroy. |
| `Fatal` | Invariant violation detected. Process should terminate. |

### 11.2 State diagram

```text
Constructed ──start()──→ Starting ──success──→ Running
     │                      │
     │                      └──fail──→ StartFailed
     │                                    │
     │                              destructor (safe)
     │
     │                 Running ──request_stop()──→ Stopping
     │                                              │
     │                                         drain()──→ Draining
     │                                                       │
     │                                                  join()──→ Stopped
     │                                                            │
     │                                                      destructor (safe)
     │
     │   Any state ──invariant violation──→ Fatal
     │                                         │
     │                                    std::terminate
     │
     │   StartFailed ──destructor──→ safe (no workers, admission never opened)
     │   Stopped ──destructor──→ safe
     │   Any other state ──destructor──→ fail-fast
```

### 11.3 Legal transition table

| From → To | Trigger | Guard |
| --- | --- | --- |
| Constructed → Starting | `start()` | state == Constructed |
| Starting → Running | start() success | all steps succeeded |
| Starting → StartFailed | start() failure | rollback completed |
| Running → Stopping | `request_stop()` | state == Running |
| Stopping → Draining | `drain()` | state == Stopping |
| Draining → Stopped | drain() complete + join() | all tasks terminal, workers joined |
| Running → Draining | `drain()` (idempotent path) | state == Running (implicit stop) |
| Any → Fatal | invariant violation | none (fail-fast) |

### 11.4 Illegal-operation behavior

| Operation | State | Behavior |
| --- | --- | --- |
| `start()` | Starting, Running, Stopping, Draining, Stopped | **returns error** (invalid_state) |
| `start()` | StartFailed, Fatal | **returns error** |
| `submit()` | Constructed, Starting | **returns error** (not started) |
| `submit()` | Stopping, Draining, Stopped, StartFailed | **returns error** (admission closed) |
| `request_stop()` | Constructed | **returns error** (not started) |
| `request_stop()` | StartFailed, Stopped | **idempotent** (no-op, returns success) |
| `drain()` | Constructed, Starting | **returns error** |
| `join()` | Constructed | **returns error** (no workers to join) |
| destructor | Running, Starting, Stopping, Draining | **fail-fast** (std::terminate) |
| destructor | Constructed, StartFailed, Stopped | **safe** |

### 11.5 Idempotent-operation behavior

| Operation | Idempotent behavior |
| --- | --- |
| `request_stop()` | Second call returns success (no-op) |
| `drain()` | Second call returns immediately if already drained |
| `join()` | Second call returns immediately if already joined |
| `shutdown()` | Composition; idempotent because each component is idempotent |

### 11.6 Concurrency behavior when operations race

| Race | Resolution |
| --- | --- |
| `request_stop()` races with `submit()` | Admission gate is atomic; linearization point is the gate check. Loser rejects. |
| `request_stop()` races with `drain()` | `request_stop()` closes admission first; `drain()` observes closed admission. |
| `drain()` races with `join()` | `drain()` must complete before `join()` (drain proves tasks terminal; join proves threads joined). |
| concurrent `request_stop()` | Atomic state transition; exactly one wins; loser is idempotent no-op. |

## 12. Operation/state matrix

| Operation | Constructed | Starting | Running | Stopping | Draining | Stopped | StartFailed | Fatal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `start()` | **allowed** | returns error | returns error | returns error | returns error | returns error | returns error | returns error |
| `submit()` | returns error | returns error | **allowed** | returns error | returns error | returns error | returns error | returns error |
| `request_stop()` | returns error | fail-fast | **allowed** | idempotent | idempotent | idempotent | idempotent | undefined |
| `drain()` | returns error | returns error | **allowed** (implicit stop) | **allowed** | idempotent | idempotent | idempotent | undefined |
| `join()` | returns error | returns error | **allowed** | **allowed** | **allowed** | idempotent | idempotent | undefined |
| `shutdown()` | returns error | fail-fast | **allowed** | **allowed** | **allowed** | idempotent | idempotent | undefined |
| destructor | **safe** | fail-fast | fail-fast | fail-fast | fail-fast | **safe** | **safe** | undefined |

## 13. Admission contract

### 13.1 Linearization rule

**A task is successfully admitted when the admission gate is open at the instant
the task is enqueued into the root Group via `Group::async()`.**

Concretely:
- The admission gate is an `std::atomic<bool> admission_open_` owned by the Runtime.
- `submit(task)` checks `admission_open_` **before** calling `Group::async()`.
- If `admission_open_ == true`: the task is admitted. `Group::async()` proceeds.
  The task body is guaranteed to execute (barring exception).
- If `admission_open_ == false`: the task is **rejected**. `submit()` returns
  `Result` with `IoError::invalid_state`. The task body **never executes**.
- `request_stop()` atomically sets `admission_open_ = false`. This is the
  linearization point for stop-vs-submit: any `submit()` that observes the gate
  as open before the store is admitted; any `submit()` that observes it closed is
  rejected.

### 13.2 Admission answers

| Question | Answer |
| --- | --- |
| When has a task been successfully admitted? | When `admission_open_` is observed true AND `Group::async()` is invoked. |
| When has an I/O operation been successfully admitted? | I/O admission is separate (Completion-based, `AsyncIoContext::submit_*`). Runtime admission gates task admission, not I/O ops. A task submits I/O after being admitted. |
| Is "placed in an intermediate queue" admission? | No. Admission is the `Group::async()` call (which spawns a Fiber). The Fiber's placement on the scheduler's runnable queue is a scheduler internal, not admission. |
| Is "worker started" admission? | No. Workers are started in `start()`. Task admission is per-submit. |
| What happens when stopping races with submission? | Atomic gate. Winner admitted, loser rejected. Deterministic. |
| Can a Running task submit child work after `request_stop()`? | Only if the child `submit()` observes the gate as open. After `request_stop()` closes the gate, child submits are rejected. |
| Can it submit child work during `drain()`? | No — `drain()` is only entered after `request_stop()` closes the gate. |
| Are child tasks part of the original drain set? | Yes — any task admitted before the gate closed is part of the drain set. |
| Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** — see §26. |
| Does cancellation precede closing admission or follow it? | `request_stop()` closes admission AND publishes root cancellation. The order within `request_stop()` is: close admission first, then publish cancellation. |
| How is rejected submission reported? | `submit()` returns `Result` with `IoError::Code::invalid_state`. |
| Can rejection run user code? | No. Rejection returns before `Group::async()` is called. |
| Can admission failure leave observable partial ownership? | No. If `Group::async()` throws (e.g., `init_fiber` failure), the exception propagates and the task is not admitted. The Runtime remains in a valid state. |

## 14. Cancellation contract

- The Runtime owns a **root CancelToken** (`cancel.hpp:47`).
- `request_stop()` publishes root cancellation via `root_token_.request()`.
- The root token is distributed to the root Group and propagated to tasks.
- Cancellation is **cooperative** (matching the existing model, `cancel.hpp:14`):
  tasks observe the token at cancel points (`check_cancel`, `cancel.hpp:147`).
- Cancellation is **not** an unconditional escape hatch: a task that does not
  observe cancellation can prevent `drain()` from returning (matching
  `group.hpp:69-76` semantics).
- The root Group is a cancel-propagation boundary: tasks swallow
  `IoError::canceled` (`group.cpp:177,254`).

## 15. Stop contract

`request_stop()`:

1. Atomically transitions Running → Stopping (CAS on `state_`). Loser (concurrent
   caller) returns success (idempotent).
2. Closes the admission gate: `admission_open_.store(false)`.
3. Publishes root cancellation: `root_token_.request()`.
4. Wakes the scheduler (via `SchedulerWakeHandle::notify()`) so the parked worker
   observes the stop.
5. Returns `Result<void>` success.

`request_stop()` does **not**:
- Block (it is non-blocking).
- Drain (that is `drain()`).
- Join threads (that is `join()`).

Evidence: `SchedulerWakeHandle::notify()` is safe across Scheduler destruction
and is a no-op if the Scheduler is dead (`scheduler.hpp:106-109`).

## 16. Drain contract

`drain()`:

1. Waits until **all admitted tasks are terminal** (their `Future<void>` is ready).
2. Guarantees at return:
   - All admitted tasks are terminal.
   - All child tasks (admitted before gate closed) are terminal.
   - The root Group's task set is fully reaped.
   - No user callback is executing (for tasks that observed cancellation or
     completed naturally).
3. Does **not** guarantee:
   - That all `Completion<T>` objects are ready (I/O ops owned by tasks may still
     be outstanding — this is a task-contract violation, not a Runtime issue).
   - That the root Group is empty (the Group may still hold completed futures
     until reaped).
   - That no worker may publish more application work (workers are parked, not
     joined — `join()` proves that).

`drain()` is implemented by driving the scheduler (via the existing
`Group::await()` → `Scheduler::run_live(1, stop_predicate)` path,
`group.cpp:41-91`) until the root Group's stop predicate fires (all futures
ready, `group.cpp:19-26`).

**Queue emptiness alone is not proof of drain** (task prompt §12). Drain requires
all task Futures ready, verified by the stop predicate (`group.cpp:19-26`).

## 17. Join contract

`join()`:

1. Joins all worker threads created in `start()`.
2. **Precondition**: `drain()` must have completed (all admitted tasks terminal).
   If `join()` is called before `drain()`, it **returns an error** (invalid_state).
3. After `join()` returns: no worker threads survive.
4. `join()` is idempotent: repeated calls return immediately.
5. `join()` does **not** imply drain (they are separate operations).
6. `join()` is **not** legal before `drain()` (returns error).
7. `join()` blocks the calling thread until all workers are joined.

Evidence: `Scheduler::run()`/`run_live()` create worker threads that are joined
at the end of the run (`scheduler.cpp:142` comment: "Workers are joined in run()").
The Runtime's `join()` joins the workers that `start()` launched.

## 18. Destructor contract

**Chosen contract: B — explicit shutdown required; destructor validates and
fail-fast on misuse.**

The destructor:
1. Checks `state_ == Stopped` (or `StartFailed`, or `Constructed`).
2. If state is Running, Starting, Stopping, or Draining: **fail-fast**
   (`std::terminate` via `detail::runtime_lifetime_fail_fast`).
3. If state is Stopped/StartFailed/Constructed: proceed with destruction in
   reverse order (Group → Scheduler → AsyncIoContext → backend).

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
  (`scheduler.cpp:169-196`).
- Hidden blocking in a destructor is a known anti-pattern (AGENTS.md §7:
  "Destructors must not invent unreportable I/O success").
- `shutdown()` returns `Result`, enabling error reporting.
- Fail-fast on misuse prevents silent resource leaks.

## 19. Restartability

**Decision: One-shot lifecycle.**

```text
Constructed → Running → Stopped
```

A Stopped Runtime may **not** be restarted. Any restart capability requires:
backend reconstruction, Scheduler reconstruction, worker generation, old wake
handles, old task handles, cancellation generations, Completion reuse, statistics
reset, stale references, failed previous shutdown — all out of scope for E16.

Evidence: `Scheduler` is non-movable, non-copyable (`scheduler.hpp:216-219`).
Reconstruction would require destroying and recreating the Scheduler, which
requires the borrowed AsyncIoContext to remain valid and quiescent.

## 20. Failure and rollback model

### 20.1 Startup as a transaction

`start()` is a fallible transaction. Each step either succeeds (and is recorded
as completed) or fails (and triggers rollback of all previously completed steps).

### 20.2 Startup sequence and rollback points

| Step | Action | Failure mode | Rollback |
| --- | --- | --- | --- |
| 1 | Validate config (worker_count >= 1, backend != nullptr) | Invalid config | Return error; state → StartFailed; no objects created |
| 2 | Construct AsyncIoContext (owns backend) | Constructor exception | Return error; state → StartFailed; backend destroyed |
| 3 | Construct Scheduler (borrows AsyncIoContext) | Constructor exception (e.g., `evented_admission_fail_fast` on unsupported target, `scheduler.cpp:98-104`) | Destroy Scheduler; destroy AsyncIoContext; return error; state → StartFailed |
| 4 | Construct root Group (borrows Scheduler) | Constructor exception | Destroy Group; destroy Scheduler; destroy AsyncIoContext; return error; state → StartFailed |
| 5 | Open admission gate | Cannot fail | — |
| 6 | Transition → Running | Cannot fail | — |

### 20.3 Failure-point analysis

| Failure point | Returned error | Resulting state | Threads to join | Objects to destroy | Retry permitted | Admission ever opened | Callbacks ran |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Invalid config | `invalid_state` | StartFailed | None | None | Yes (re-call start) | No | No |
| AsyncIoContext construction | exception | StartFailed | None | backend | Yes | No | No |
| Scheduler construction | exception | StartFailed | None | AsyncIoContext | Yes | No | No |
| Group construction | exception | StartFailed | None | Scheduler, AsyncIoContext | Yes | No | No |
| Stop races with start | `invalid_state` | StartFailed or Running | Per rollback | Per rollback | Yes | No | No |

**No partially started background worker may survive a failed `start()`.**
Workers are only created after all construction steps succeed (in the
`start()` → Running transition). If any construction step fails, no workers
exist to join.

### 20.4 Exception during startup

If any constructor throws:
1. Catch the exception.
2. Destroy all already-constructed objects in reverse order.
3. Transition → StartFailed.
4. Return `Result` with the error.

Evidence: `Group::async_evented` demonstrates the pattern — `init_fiber` failure
throws before any vector mutation or spawn (`group.cpp:264-269`); strong
exception safety with correct admission semantics.

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
    Result<void> start();
    Result<void> submit(TaskFn task);  // admission-gated
    void request_stop() noexcept;
    Result<void> drain();
    Result<void> join();
    Result<void> shutdown();  // composition: request_stop + drain + join
    
    // Non-copyable, non-movable
    ~ApplicationRuntime();  // fail-fast if not Stopped
};

}  // namespace sluice::async
```

| Decision | Choice | Rationale |
| --- | --- | --- |
| Constructor vs factory | Factory (`create`) | Separates validation from construction; enables `Result` return |
| Config validation point | In `create()` | Fail fast before any owned objects exist |
| Separate `start()` | Yes | Enables construct/start separation (misuse resistance) |
| Submission return | `Result<void>` | Admission rejection reported via Result |
| `request_stop()` can fail | No (`noexcept`) | Idempotent; always succeeds |
| `drain()`/`join()` return | `Result<void>` | Error reporting for misuse |
| Movable | No | Owns non-movable Scheduler, AsyncIoContext |
| Copyable | No | Unique ownership |
| Exposes internals | No | Default assumption: direct access weakens lifecycle authority |
| Diagnostics | Snapshot (not reference) | Avoids lifetime coupling |
| Task function receives | `CancelToken&` | Matches existing Group::async signature (`group.hpp:89`) |

### 21.2 API Sketch 2 — Builder + one-shot

```cpp
// PROPOSED — NOT AN EXISTING API
namespace sluice::async {

class RuntimeBuilder {
public:
    RuntimeBuilder& backend(std::unique_ptr<AsyncBackend> b);
    RuntimeBuilder& workers(unsigned n);
    Result<ApplicationRuntime> build();  // validates, constructs, returns Constructed
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

Same decisions as Sketch 1, plus:
- Builder collects config incrementally.
- `build()` validates and constructs owned objects.
- `start()` is a separate transaction.

### 21.3 Selected proposed API direction

**Sketch 2 (Builder + one-shot) is preferred.**

Rationale:
- Builder enables incremental config collection with validation.
- `build()` / `start()` separation prevents use-before-start.
- Matches the preferred architecture (Alternative C, §7.5).

## 22. Acceptance contracts

Each contract is executable in a later phase without timing sleeps, using the
deterministic `FakeAsyncBackend` injection seam (`async_io_context.hpp:121`).

### A1 — Normal lifecycle

```text
Given a valid deterministic backend (FakeAsyncBackend)
When Runtime starts, accepts two tasks, receives stop, drains, and joins
Then both admitted tasks complete exactly once
And no worker survives
And Runtime reaches Stopped
And destruction is safe
```

### A2 — Submission after stop

```text
Given a Running Runtime
When request_stop wins before a concurrent submit
Then the submit is rejected deterministically
And its body never runs
And it is not part of the drain set
```

### A3 — Submission wins before stop

```text
When submit wins the admission linearization race
Then the task belongs to the admitted set
And drain cannot return before it reaches terminal state
```

### A4 — Child admission during shutdown

```text
Given a Running Runtime
When request_stop closes the admission gate
Then any child submit() observing the closed gate is rejected
And the child task body never runs
And only tasks admitted before the gate closed are in the drain set
```

### A5 — Nth worker startup failure

```text
IMPLEMENTATION BLOCKED — E16 workers are driven by Scheduler::run_live, not
independently spawned. The equivalent failure is: if start() fails at any
construction step, then:
  All earlier-constructed objects are destroyed in reverse order
  Admission never opens
  Runtime does not enter Running
  No background thread survives
```

### A6 — Outstanding asynchronous I/O

```text
Given a Running Runtime with an admitted task that owns an outstanding Completion
When request_stop is called
Then the task is asked to cancel (root token published)
And drain() waits for the task to reach terminal state
And the outstanding Completion is the task's responsibility (caller-owned)
And AsyncIoContext destruction fail-fast if Completions remain outstanding
```

### A7 — Repeated stop/drain/join

```text
Given a Running Runtime
When request_stop is called twice
Then the second call is a no-op (idempotent)
When drain is called twice
Then the second call returns immediately
When join is called twice
Then the second call returns immediately
```

### A8 — Destructor misuse

```text
Given a Runtime in Running state
When the destructor is called
Then the process terminates (fail-fast via std::terminate)
And this is deterministic in both Debug and Release
```

### A9 — Task body throws

```text
Given a Running Runtime
When a task body throws an exception
Then the exception is swallowed at the Group boundary (cancel-propagation boundary)
And the task reaches terminal state (Future completed)
And drain() can return
And Runtime health is unaffected
```

Evidence: `Group::async_evented` swallows exceptions (`group.cpp:253-256`);
`Group::async_threaded` swallows exceptions (`group.cpp:176-179`).

### A10 — Shutdown initiated from a runtime worker

```text
Given a Running Runtime
When a task running on a worker thread calls request_stop()
Then the call is allowed (request_stop is noexcept and thread-safe)
And the Runtime transitions to Stopping
And drain() / join() complete normally
```

Rationale: `request_stop()` is `noexcept` and thread-safe. Restricting it to
non-worker threads would require TLS checks that add complexity without
preventing the more important case of shutdown-from-external-thread.

## 23. Unit-test plan

### 23.1 State-transition tests

| Test | Target |
| --- | --- |
| Constructed → Starting → Running | `start()` success path |
| Constructed → Starting → StartFailed | `start()` failure at each step |
| Running → Stopping → Draining → Stopped | Full lifecycle |
| Running → Stopping (idempotent) | Double `request_stop()` |
| Draining (idempotent) | Double `drain()` |
| Join (idempotent) | Double `join()` |

### 23.2 Illegal-operation tests

| Test | Expected |
| --- | --- |
| `submit()` in Constructed | Returns error |
| `submit()` in Stopped | Returns error |
| `start()` in Running | Returns error |
| `join()` before `drain()` | Returns error |
| destructor in Running | Fail-fast (death test) |

### 23.3 Admission race ordering

| Test | Method |
| --- | --- |
| submit wins before stop | Deterministic phase seam (E7 admission discipline) |
| stop wins before submit | Deterministic phase seam |
| concurrent submit + stop | Barrier-synchronized threads |

### 23.4 Startup rollback at every fallible step

| Step | Injection | Verification |
| --- | --- | --- |
| Invalid config | worker_count=0 | Returns error; no objects created |
| Scheduler construction failure | `force_next_init_fiber_fail` analog | Returns error; AsyncIoContext destroyed |
| Group construction failure | Exception injection | Returns error; Scheduler + AsyncIoContext destroyed |

### 23.5 Task exception containment

| Test | Expected |
| --- | --- |
| Task throws std::exception | Swallowed; task terminal; Runtime healthy |
| Task throws non-standard | Swallowed; task terminal; Runtime healthy |

### 23.6 Outstanding-I/O shutdown

| Test | Expected |
| --- | --- |
| Task with outstanding Completion at stop | Task cancel observed; drain waits; AsyncIoContext fail-fast if leaked |

### 23.7 Destructor misuse

| Test | Expected |
| --- | --- |
| Destructor in Running | Death test (std::terminate) |
| Destructor in Stopped | Safe |
| Destructor in StartFailed | Safe |

## 24. Mutation plan

| Mutation | Killing test |
| --- | --- |
| Allow submit after admission closes | A2: submit after stop must be rejected |
| Omit root cancellation publication | A1: tasks must observe cancel; drain must complete |
| Return from drain with one admitted task alive | A1: drain waits for all admitted tasks |
| Forget to join one worker | A1: no worker survives (thread count check) |
| Publish Running before startup is committed | A5: Runtime does not enter Running on failure |
| Omit rollback for Nth failure | A5: no surviving worker; admission never opened |
| Allow destructor with live work | A8: destructor fail-fast (death test) |
| Misclassify a losing concurrent submit as admitted | A2/A3: deterministic admission linearization |
| Skip admission gate check | A2: submit after stop must be rejected |
| Double-join a worker thread | A7: join is idempotent (no crash) |

## 25. Fuzz/formal applicability

### 25.1 State variables

```text
runtime_state      ∈ {Constructed, Starting, Running, Stopping, Draining, Stopped, StartFailed, Fatal}
admission_open     ∈ {true, false}
stop_requested     ∈ {true, false}
admitted_count     ≥ 0
terminal_count     ≥ 0
workers_started    ≥ 0
workers_joined     ≥ 0
outstanding_io     ≥ 0
```

### 25.2 Potential invariants

```text
terminal_count ≤ admitted_count
Stopped implies workers_started == workers_joined
Stopped implies outstanding_io == 0
admission_open implies state == Running
a rejected task never executes
drain-complete implies terminal_count == admitted_count
StartFailed implies no surviving worker
```

### 25.3 Formal impact

```text
MODEL_RECOMMENDED
```

**Reason:** The lifecycle state machine has 8 states, multiple concurrent
operations (submit, request_stop, drain, join), and a non-trivial admission
linearization rule. A small TLA+ or similar state model would catch
counterexamples in:
- Admission race ordering (submit vs. request_stop).
- Drain completeness (all admitted tasks terminal).
- Destructor safety (only Stopped/StartFailed/Constructed are safe).
- Startup rollback (no surviving worker on failure).

The model need not be large — the variables above are sufficient to express
the core invariants. The existing E7/E9/E13 formal models
(`docs/spec/e9_wake_handle_lifetime/`, E13SelectContract.tla) demonstrate
the repository's capacity for focused state models.

## 26. Open questions

| ID | Question | Status | Impact |
| --- | --- | --- | --- |
| Q1 | Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** | If yes, a separate "internal admission" path is needed; if no, all work goes through the gate. |
| Q2 | Should `submit()` return a handle/Future for the admitted task? | **OPEN HUMAN DECISION** | Affects API shape; a handle enables per-task await/cancel. |
| Q3 | Should the Runtime expose a diagnostics snapshot (task count, worker count, state)? | **OPEN HUMAN DECISION** | Useful for monitoring; adds API surface. |
| Q4 | Should `drain()` have a deadline? | **OPEN HUMAN DECISION** | A deadline prevents indefinite blocking but adds timer dependency. |
| Q5 | Should the Runtime support Threaded mode (std::thread workers) in addition to Evented? | **OPEN HUMAN DECISION** | The existing Group supports both modes (`group.hpp:47-54`); the Runtime could expose both. |
| Q6 | What is the exact TaskFn signature? `void(CancelToken&)` (matching Group::async) or richer? | **OPEN HUMAN DECISION** | Affects API compatibility with existing Group consumers. |

## 27. Implementation slices

If authorized, implementation would proceed in order:

1. **S1: Builder + config validation** — `RuntimeBuilder`, `RuntimeConfig`, validation.
2. **S2: Owned-object construction** — `ApplicationRuntime` owns AsyncIoContext, Scheduler, root Group.
3. **S3: Lifecycle state machine** — states, transitions, atomic state_.
4. **S4: `start()` transaction** — construction order, rollback, Running transition.
5. **S5: Admission gate** — atomic admission_open_, submit() gating, rejection.
6. **S6: `request_stop()`** — state transition, gate close, root cancellation, wake.
7. **S7: `drain()`** — drive scheduler until all admitted tasks terminal.
8. **S8: `join()`** — join worker threads, precondition drain().
9. **S9: `shutdown()`** — composition of request_stop + drain + join.
10. **S10: Destructor** — state validation, fail-fast, reverse destruction.
11. **S11: Diagnostics** — snapshot API (if Q3 resolved yes).
12. **S12: Tests** — acceptance contracts A1-A10, unit tests, mutation tests.

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
| Scheduler::run blocks caller | `scheduler.hpp:241` |
| Scheduler destruction asserts quiescence | `scheduler.cpp:169-196` |
| Group(Scheduler&) borrows scheduler | `group.hpp:78` |
| Group::async_evented drives run_live | `group.cpp:69` |
| Group::await stop predicate | `group.cpp:19-26` |
| Group destructor fail-fast (Evented) | `group.cpp:117-122` |
| Group tasks swallow exceptions | `group.cpp:177,254` |
| AsyncIoContext owns backend | `async_io_context.hpp:121` |
| AsyncIoContext move-only | `async_io_context.hpp:125-133` |
| AsyncIoContext destructor fail-fast | `async_io_context.cpp:30-41` |
| Completion caller-owned, non-movable | `completion.hpp:84-87` |
| CancelToken idempotent request | `cancel.hpp:59` |
| check_cancel is the cancel point | `cancel.hpp:147` |
| Fiber state machine | `fiber.hpp:40-46` |
| Future idempotent await/cancel | `future.hpp:101-116` |
| EventedWaitPolicy borrows Scheduler | `evented_wait_policy.hpp:48` |
| ThreadPoolBackend spawns/joins threads | `threadpool_backend.hpp:101` |
| SchedulerWakeHandle outlives Scheduler | `scheduler.hpp:106-109` |
| fail_fast routes through std::terminate | `fail_fast.cpp:16-62` |
| Test seams under SLUICE_ASYNC_INTERNAL_TESTING | `scheduler.hpp:1749`, `group.hpp:102` |
| CI gate is Linux Clang Debug | `.github/workflows/ci.yml:37-74` |
| Production targets: sluice_core, sluice_async | `xmake/libraries.lua:7-32` |
| No public API for runtime lifecycle | `docs/api-reference.md` (no Runtime entry) |
| E16 design doc placeholder | `docs/design/README.md:28` |
