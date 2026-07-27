# Async Runtime Architecture

**Status:** Current
**Authority:** Architecture
**Scope:** `sluice_async` production library — Scheduler, Fiber, execution strategies.

The async runtime is the M:N fiber scheduler at the heart of `sluice_async`. It
provides the execution substrate on which all async synchronization primitives
(Event, Semaphore, AsyncMutex, AsyncCondition, AsyncQueue, AsyncRwLock, Select)
are built.

## Scheduler

`sluice::async::Scheduler` is the central authority. It owns:

- **Worker registration** — each Worker owns a scheduler context and a current
  Fiber (E7-C1). Fibers are pinned to their first-execution Worker (E7-C2).
- **Runnable routing** — the canonical wake seam. A terminal winner transition
  and its runnable-publication obligation are one coordinated authority; losers
  do not unlink, publish, or wake.
- **WaitQueue integration** — `WaitQueue` structural operations are private;
  `Scheduler` is the sole friend. Public registration or resolution methods do
  not exist on `WaitQueue`.
- **Timer deadline heap** — monotonic `deadline_t` ticks, with `TimerRegistration`
  state machine (`active` / `retired` / `consumed`).
- **External wake** — `SchedulerWakeHandle` lets an external producer thread wake
  a parked Scheduler Worker without holding a raw `Scheduler*`.

The Scheduler is non-copyable, non-movable. Destruction with live waiters,
registrations, or outstanding callbacks is a contract violation (fail-fast in
Debug).

## Fiber

`sluice::async::Fiber` represents a logical task. The Fiber context-switch
implementation (`fiber_ctx::context_switch`) is architecture-specific:

- **x86_64 Linux** — stack-switching assembly (`fiber_ctx::supported == true`).
- **Other platforms** — `fiber_ctx::supported == false`; Evented tests skip
  cleanly.

A Fiber carries a `CancelToken` and `CancelState` but is **not** itself the
cancel-propagation boundary. The documented cancel-propagation boundary is
`Group`: tasks swallow `IoError::canceled` at their cancel points.

## Two distinct waiting layers

The runtime has two distinct waiting layers that are easy to confuse:

**Scheduler-integrated primitives** (Fiber-only blocking path):

| Primitive | Suspension mechanism |
|-----------|---------------------|
| `Event` | `Scheduler::await_event_wait` |
| `Semaphore` | `Scheduler::sem_acquire` |
| `AsyncMutex` | `Scheduler::mutex_lock` |
| `AsyncCondition` | `Scheduler::condition_wait_prepare` |
| `AsyncQueue<T>` | `Scheduler::queue_push_admit` / `queue_pop_admit` |
| `AsyncRwLock` | `Scheduler::rwlock_read_lock` / `rwlock_write_lock` |
| `Select` | `Scheduler::select_admit` (via friended free `select()`) |

These primitives suspend fibers directly through `Scheduler` members. They do
**not** use `WaitPolicy`, and there is no "Threaded primitive" implementation —
they are fiber-scheduler-native.

**Policy-based task waiting** (Threaded/Evented parity):

| Type | Wait mechanism | Fiber required | Platform |
|------|---------------|----------------|----------|
| `Future<T>` | `WaitPolicy&` (injected) | Evented: Yes / Threaded: No | Any |
| `Group` | `WaitPolicy&` (Scheduler or default) | Evented: Yes / Threaded: No | Any |

`WaitPolicy` is the abstract seam that decides *how* a task waits physically:

- `ThreadedWaitPolicy` — `std::condition_variable` (portable, any platform).
- `EventedWaitPolicy` — Fiber suspend/resume via a `Scheduler&` (x86_64 Linux).

`Future<T>` and `Group` are the only types that delegate the physical wait to a
`WaitPolicy`. The async primitives do not.

**Threaded** is the portable default for policy-based waiting. Each blocking
wait consumes an OS thread.

**Evented** requires a running `Scheduler` and x86_64 Linux. A Fiber awaiting a
pending operation suspends (does not block a worker thread); the worker runs
other Fibers. Completion wakes the suspended Fiber through the canonical wake
seam.

## Multi-worker

E7 introduced multi-worker scheduling:

- **Worker-local execution state** — each Worker has its own runnable queue and
  current Fiber.
- **Pinned routing** — a Fiber runs on the Worker where it first executed (E7-C2).
- **Work stealing** — steal = MOVE + OWNER TRANSFER (never PUBLISH); a stolen
  Fiber wake-routes to the thief Worker (E8).
- **Serialized backend access** — `AsyncIoContext::access_mtx_` serializes
  `poll()`/`wait_one()` across Workers (E7-C).

## External wake

E9 added the external-wake subsystem:

- `SchedulerWakeHandle` — a control-block-backed handle that an external
  producer thread holds to wake a parked Scheduler without holding a raw
  `Scheduler*` (which would be use-after-free across Scheduler destruction).
- **Wake-handle lifetime** — `notify()` holds `Control::mtx` (the callback
  lease) through the Scheduler wake callback, so destruction cannot interleave
  with an in-flight callback.

## Ownership and shutdown

- The Scheduler **owns** its Workers, the deadline heap, and the WakeHandle
  control block.
- Primitives (Event, Semaphore, AsyncMutex, etc.) **borrow** a `Scheduler&`;
  they must not outlive the Scheduler.
- `WaitNode` is **caller-owned**, address-stable, non-copyable, non-movable.
  One fresh `WaitNode` per wait epoch.
- Destruction with live waiters or outstanding registrations is a contract
  violation (fail-fast in Debug, documented as undefined in Release).

## Platform restrictions

- **POSIX** (Linux, macOS, WSL) for the synchronous core and ThreadPoolBackend.
- **Evented** requires x86_64 Linux with `fiber_ctx::supported == true`.
- **io_uring** (`UringAsyncBackend`) requires Linux + liburing (build-gated,
  off by default). Without liburing, the backend is an unsupported stub.

## Verification

- Deterministic causal tests (no `sleep_for` proof) via
  `SLUICE_ASYNC_INTERNAL_TESTING` phase seams.
- Authority probes (negative-compile) enforce queue-identity safety and
  resolution-authority boundaries.
- Death tests (POSIX fork/exec) enforce fail-fast boundaries.
- Cross-primitive parity tests enforce semantic equivalence across the
  Scheduler-integrated primitives (Event / Semaphore / AsyncMutex / AsyncCondition
  / AsyncQueue / AsyncRwLock).
- Policy-based parity tests enforce semantic equivalence between Threaded and
  Evented strategies for `Future<T>` and `Group`.

## References

- ADR-execution-model.md — the accepted execution-strategy contract.
- `docs/architecture/async-synchronization.md` — the primitive layer.
- `docs/architecture/async-io-foundation.md` — Completion / AsyncIoContext / backends.
