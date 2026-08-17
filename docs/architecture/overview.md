# Sluice Architecture Overview

Sluice is organized into two production libraries plus a test-only variant and
optional experimental code.

```text
Applications & Workloads          apps/ — public headers only, no test seams
        ↑ expresses I/O intent
Public API Surface                include/sluice/ + docs/reference/
        ↑ backend-neutral operations
Synchronous Core                  sluice_core (Reader/Writer, Result, copy, WAL)
Async Runtime                     sluice_async (Scheduler, Fiber, primitives,
                                  Completion, ApplicationRuntime)
        ↓ execution ownership
Backends & System Capabilities    ThreadPoolBackend (default real backend),
                                  UringAsyncBackend (experimental),
                                  FakeAsyncBackend/SyncBackend (testing)
```

A rendered version of this layered view (including the future workload
directions — networking and external-memory data structures, which are **not**
implemented capability) is the canonical asset
[`docs/assets/architecture/sluice-high-level-layered-view.png`](../assets/architecture/sluice-high-level-layered-view.png).
Application-driven development context: `docs/applications/README.md`.

## Build boundaries

```
sluice_core (synchronous, always builds)
├── Result<T> / IoError
├── Reader / Writer + buffered/fault/observed wrappers
├── copy_all / CopyStrategy
├── FileReader / FileWriter (POSIX)
├── BlockingIoContext (POSIX factory)
├── BlockingIoPool (bounded OS-thread pool)
├── SyncableWriter (sync_data / sync_all)
├── WAL (write-ahead log)
└── MemoryIoContext (deterministic in-memory)

sluice_async (opt-in, separate static library)
├── Scheduler (M:N fiber scheduler, multi-worker, work stealing)
├── Fiber / fiber_ctx (context-switch, x86_64 only)
├── WaitNode / WaitQueue (E10)
├── TimerRegistration / deadline (E11)
├── Event (E12-A)
├── Semaphore (E12-B)
├── AsyncMutex (E12-C)
├── AsyncCondition (E12-D)
├── AsyncQueue<T> (E12-E)
├── AsyncRwLock (E12-F)
├── Select (E13)
├── CancellationToken / CancelState (E27)
├── Future<T> (E28)
├── Group (E29)
├── Batch (E30)
├── Completion<T> / AsyncIoContext
├── ApplicationRuntime / RuntimeBuilder / RuntimeTaskContext
│   (lifecycle layer: build → start → submit → stop → drain → join; ADR-application-runtime)
├── AsyncBackend (internal boundary)
│   ├── FakeAsyncBackend (deterministic test vehicle)
│   ├── ThreadPoolBackend (portable, std::thread)
│   └── UringAsyncBackend (experimental, liburing-gated)
└── EventedWaitPolicy / ThreadedWaitPolicy

sluice_async_internal_testing (test-only variant)
├── Same authoritative sources as sluice_async
├── Guarded by SLUICE_ASYNC_INTERNAL_TESTING
├── Exposes deterministic causal phase seams
└── MUST NOT be linked alongside sluice_async; no executable links both

sluice_experimental_uring (optional, build-gated)
└── UringWriteBatch / UringIoContext (stub without liburing)
```

## Dependency graph

```
sluice_core           ← no dependency on sluice_async
sluice_async          ← depends on sluice_core (Result, IoError)
sluice_async_internal_testing ← test-only variant of sluice_async
sluice_experimental_uring ← depends on sluice_core, optional liburing
```

## Capability classification

| Category | Examples |
|----------|----------|
| **Public runtime capability** | Scheduler, Fiber, Event, Semaphore, AsyncMutex, AsyncCondition, AsyncQueue, AsyncRwLock, Select, Future, Group, Batch, CancellationToken, ApplicationRuntime |
| **Internal scheduler substrate** | WaitNode, WaitQueue, TimerRegistration, Mutex (TSA-annotated) |
| **Test-only seam** | SLUICE_ASYNC_INTERNAL_TESTING phase seams, FakeAsyncBackend held-pending mode |
| **Experimental backend** | UringAsyncBackend, UringWriteBatch |

## Key contracts

- Synchronous I/O is synchronous from the caller's perspective (G1).
- Positional I/O does not mutate the shared file offset (G6).
- `flush()` drains software buffering; `sync_data()` / `sync_all()` handle durability.
- `BlockingIoPool` is a bounded OS-thread execution helper, NOT an async runtime.
- Async primitives use `WaitNode` (one per wait epoch, caller-owned, address-stable).
- A wait epoch has exactly one terminal outcome and at most one runnable publication.
- Destructors must not invent unreportable I/O success.
- Destruction with live waiters or outstanding registrations is a contract violation.

## Platform restrictions

- POSIX (Linux, macOS, WSL).
- `Evented` execution strategy requires x86_64 Linux with `fiber_ctx::supported`.
- io_uring requires Linux + liburing (build-gated, off by default).

## Verification layers

1. **Acceptance tests** — `xmake test -v` (Clang Debug)
2. **Unit / component tests** — per-slice test binaries
3. **Sanitizer gates** — ASan, UBSan, TSan, Valgrind
4. **Code quality** — `clang-tidy`, `.clang-format`
5. **Formal models** — TLA+ specs under `spec/tla/`

See `docs/verification/README.md` for the full verification matrix.
