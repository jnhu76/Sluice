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

A Fiber is a cancellation-propagation boundary: tasks swallow
`IoError::canceled` at their cancel points.

## Threaded vs Evented

Two execution strategies share the same public primitive surface:

| Strategy | Wait mechanism | Fiber required | Platform |
|----------|---------------|----------------|----------|
| **Threaded** (`ThreadedWaitPolicy`) | `std::condition_variable` | No | Any |
| **Evented** (`EventedWaitPolicy`) | Fiber suspend/resume | Yes | x86_64 Linux |

The strategy is injected as a `WaitPolicy&` reference at construction or await
time. All primitives (`Event::wait`, `Semaphore::acquire`, `AsyncMutex::lock`,
`AsyncCondition::wait`, `AsyncQueue::push/pop`, `AsyncRwLock::read_lock/write_lock`,
`Select`) suspend via the policy, so the same primitive code path works in both
modes.

**Threaded** is the portable default. Each blocking wait consumes an OS thread.

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
- Cross-primitive parity tests enforce semantic equivalence between Threaded
  and Evented strategies.

## References

- ADR-execution-model.md — the accepted execution-strategy contract.
- `docs/architecture/async-synchronization.md` — the primitive layer.
- `docs/architecture/async-io-foundation.md` — Completion / AsyncIoContext / backends.
