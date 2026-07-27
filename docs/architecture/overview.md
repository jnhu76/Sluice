# Sluice Architecture Overview

Sluice is organized into two production libraries (`sluice_core` and `sluice_async`) plus optional experimental code.

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
├── Scheduler (M:N fiber scheduler)
├── WaitNode / WaitQueue (E10)
├── TimerRegistration / deadline (E11)
├── Event (E12-A)
├── Semaphore (E12-B)
├── AsyncMutex (E12-C)
├── AsyncCondition (E12-D)
├── AsyncQueue<T> (E12-E)
├── AsyncRwLock (E12-F, proposed)
├── Select (E13, proposed)
└── Evented (E14, proposed)

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

## Key contracts

- Synchronous I/O is synchronous from the caller's perspective (G1).
- Positional I/O does not mutate the shared file offset (G6).
- `flush()` drains software buffering; `sync_data()` / `sync_all()` handle durability.
- `BlockingIoPool` is a bounded OS-thread execution helper, NOT an async runtime.
- Async primitives use `WaitNode` (one per wait epoch, caller-owned, address-stable).
- A wait epoch has exactly one terminal outcome and at most one runnable publication.
- Destructors must not invent unreportable I/O success.

## Platform restrictions

- POSIX (Linux, macOS, WSL).
- `Evented` execution strategy requires x86_64 Linux with `fiber_ctx::supported`.
- io_uring requires Linux + liburing (build-gated, off by default).

## Verification layers

1. **Acceptance tests** — `xmake test -v` (Clang Debug)
2. **Unit/component tests** — per-slice test binaries
3. **Mutation testing** — `scripts/run-mutation-test.sh`
4. **Code quality** — `clang-tidy`, `.clang-format`
5. **Formal models** — TLA+ specs under `docs/spec/` and `spec/tla/`

See `docs/verification/README.md` for the full verification matrix.