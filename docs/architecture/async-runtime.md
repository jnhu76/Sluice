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

## Lock protocol and wake law

The current durable lock/wake law below is the authority for lock ordering and
wake obligations in the Scheduler/backend domain. It is stated as the current
as-built protocol; the campaign that produced it (Phase G P5-CORRECTIVE, the
2 ms MIXED-WAKE verdict, the interrupt bridge) is historical record in
`docs/history/implementation-plans/phase-g-backend-progress-wake.md` §4 and is
not re-stated here.

### Lock inventory (guarded state)

| Lock / domain | Guards |
|---|---|
| `G` — Scheduler global coordination lock (`global_mtx_`) | runnable routing, admission, classification, worker population bookkeeping |
| `W` — wake epoch/predicate lock (`wake_mtx_`) | `wake_epoch_` + `wake_cv_`; per-worker park baselines (`observed_epoch`, park domain) |
| `B` — backend wait-source domain | backend ready/control epochs (`ReadyWaitSource`) or progress/control epochs + eventfd counters (`UringWaitSource`); the parked `wait_one()` observer |
| `A` — `AsyncIoContext` backend-serialization lock (`access_mtx_`) | serializes `poll()` / `wait_one()` across Workers |
| `L` — `RequestArena` lifecycle leaf domain | slot lifecycle: generation, pin/reap, borrow, exactly-once terminal |
| `I` — per-worker inbox lock/domain (`WorkerState::inbox_mtx`) | the worker's single `local_runnable` queue |

### Allowed edges (current as-built)

```text
G -> A        classify_locked / drain poll (unchanged)
G -> B        arm_backend_wait_commit at the MW-S2 commit (backend or MIXED domain)
G -> W        signal_wake_locked under G (wake publication)
G -> W -> B   signal_wake_locked -> interrupt_backend_waiters (W and B are leaves;
              no reverse edge exists — no wait-source path acquires wake_mtx_)
G -> I        routing: route_runnable_locked / spawn / steal push under inbox_mtx
A -> B        context wait_one snapshot/poll/wait_for_change
A -> L        poll/reap
L -> (release) -> sink.on_ready
lifecycle -> B  request_stop -> interrupt_backend_waiters
W -> I        park predicate: local_runnable read acquires inbox_mtx NESTED
              under wake_mtx_ (the wake->inbox edge; never the reverse order)
```

`G -> W -> B` is one-directional: `signal_wake_locked` advances the wake epoch
under `W`, releases it, then interrupts backend waiters. W and B are leaves
with inbound edges only from G/A; no path acquires G, A, or W while holding B.

### Forbidden reverse edges

```text
I -> W   forbidden — taking wake_mtx_ under inbox_mtx would invert the park
         predicate's wake->inbox edge (routing always signals AFTER releasing
         the inbox lock)
I -> G   forbidden — global_mtx_ is never acquired while holding inbox_mtx
B -> G   forbidden — a backend wait source never calls the Scheduler
B -> A   forbidden — the wait source never re-enters AsyncIoContext serialization
```

### Cycle / safety reasoning

- **Leaf-domain reasoning:** `L` is a leaf domain — while holding it the code
  never calls the Scheduler, `ReadySink`, or user code, never syscalls, joins,
  or waits for backend/kernel progress (constitution AC-6; `AGENTS.md` §3.6).
- **No B -> G:** a backend wait source never calls the Scheduler; the interrupt
  bridge is a bounded mutex + epoch bump + non-blocking notify/eventfd write
  (no Scheduler call, no user code, no join, no blocking syscall).
- **Bridge is notification, not completion:** the notification bridge never
  publishes a `Completion`, never mutates a `RequestSlot`, never routes a
  Fiber, and never allocates (the epoch/eventfd protocol has no queue node).
- **Wake obligation:** every producer publishes persistent state BEFORE the
  wake (state first, then notify, in all sites). The commit-to-sleep race is
  closed by the persistent epoch/predicate under `W` (`park_on_wake_source`
  re-checks `wake_epoch_`, `global_terminate_`, and the inbox under the same
  locks the producer uses), not by timeout.

### Formal / safety pointers

The lock/wake law is pinned by the repository's formal evidence, which is not
modified by this document:

- constitution **AC-6** (explicit wake obligation; the 2 ms MIXED-WAKE
  backstop is protocol authority per usage site, not defense-in-depth);
- anchor **F08** in `spec/formal/anchors.json` and its `wake-signal` /
  `park-boundary` anchor states (`wake-epoch-state`);
- the **e9-park-wake** TLA suite (`spec/tla/e9_park_wake/`) — R1–R4, the
  split-wait bridge, non-vacuity witnesses and fail-closed negatives.

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
- `docs/architecture/architecture-constitution.md` AC-6 — explicit wake obligation.
- `spec/tla/e9_park_wake/` — R1–R4 park/wake TLA model and witnesses.
