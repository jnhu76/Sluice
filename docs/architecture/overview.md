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
Backends & System Capabilities    AsyncBackend public extension point;
                                  ThreadPoolBackend (default real backend),
                                  UringAsyncBackend (experimental),
                                  FakeAsyncBackend/SyncBackend (testing)
```

A rendered version of this layered view (including the future workload
directions — networking and external-memory data structures, which are **not**
implemented capability) is the canonical asset
[`docs/assets/architecture/sluice-high-level-layered-view.png`](../assets/architecture/sluice-high-level-layered-view.png).
Application-driven development context: `docs/applications/README.md`.

## Authoritative implementation map

This is the single current routing surface for build boundaries. It answers
where a change starts without copying the exact source manifest. The linked
Xmake files remain executable authority for target membership, dependencies,
feature gates, and test wiring.

| Boundary | Stable responsibility and dependency rule | Public or shared surface | Implementation | Build and verification authority |
|----------|-------------------------------------------|--------------------------|----------------|----------------------------------|
| `sluice_core` | Synchronous `Result<T>` / `IoError`, Reader/Writer, buffering, copy, WAL, positional and durable file I/O, and `BlockingIoPool`; no dependency on async | [`include/sluice/`](../../include/sluice/) excluding opt-in async and experimental subdirectories | Synchronous translation units directly under [`src/`](../../src/) | [`xmake/libraries.lua`](../../xmake/libraries.lua), [`xmake/tests/core.lua`](../../xmake/tests/core.lua) |
| `sluice_async` | Opt-in production async runtime, Completion/AsyncIoContext, backends, Runtime/Scheduler integration, and explicit request lifecycle; depends on `sluice_core` | [`include/sluice/async/`](../../include/sluice/async/) | [`src/async/`](../../src/async/) | [`xmake/libraries.lua`](../../xmake/libraries.lua), [`xmake/experimental.lua`](../../xmake/experimental.lua), [`xmake/tests/async.lua`](../../xmake/tests/async.lua), and [`verification/`](../verification/) |
| `sluice_async_internal_testing` | Test-only build of the authoritative async production sources plus guarded deterministic controls; production targets must not depend on it, and no executable may link both async variants | Production async headers plus guarded non-installed seam headers in [`src/async/`](../../src/async/) | The same async production sources plus the controller under [`tests/`](../../tests/) | [`xmake/libraries.lua`](../../xmake/libraries.lua), [`xmake/tests/async_internal.lua`](../../xmake/tests/async_internal.lua) |
| `sluice_experimental_uring` | Optional standalone io_uring helpers; depends on `sluice_core`, is off by default, and builds stubs without liburing | [`include/sluice/experimental/`](../../include/sluice/experimental/) | [`src/experimental/`](../../src/experimental/) | [`xmake/experimental.lua`](../../xmake/experimental.lua); real and stub evidence are reported separately |
| Applications | Real workloads using installed public headers only, with no test seams or private include path | Installed public Sluice headers | [`apps/`](../../apps/) | [`xmake/apps.lua`](../../xmake/apps.lua) and [`docs/applications/`](../applications/) |
| Zig design reference | Source-derived design reference only; no production target may build, link, vendor, or mechanically copy it | None | [`zig/`](../../zig/) | Differences are classified in [`divergence-registry.md`](divergence-registry.md) |

The liburing option also selects the real `UringAsyncBackend` path inside
`sluice_async`; that backend and the standalone `sluice_experimental_uring`
target are distinct boundaries. io_uring remains one mechanism, not the
architecture.

### Map stability rules

- Keep exact source membership in Xmake target declarations, not in this table.
- Route to stable directories or named authorities; do not copy every header,
  source, test, count, or current migration percentage.
- When a boundary moves, update the build authority and this map together.
  `check-doc-links.py` detects stale paths; `verify-architecture-docs.py`
  verifies every backticked target row against Xmake declarations.

## Capability classification

| Category | Examples |
|----------|----------|
| **Public runtime capability** | Scheduler, Fiber, Event, Semaphore, AsyncMutex, AsyncCondition, AsyncQueue, AsyncRwLock, Select, Future, Group, Batch, CancellationToken, ApplicationRuntime, AsyncIoContext |
| **Public backend extension point** | AsyncBackend and its backend wait/capability hooks |
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

## Platform status

Current validation is Linux/WSL-centric.

- The synchronous core is POSIX-oriented and contains Linux/macOS-compatible
  code paths, but broader non-Linux portability evidence is still incomplete.
- `Evented` execution requires x86_64 Linux with `fiber_ctx::supported`.
- io_uring requires Linux + liburing (build-gated, off by default).

See the roadmap and verification documents for the current portability evidence;
do not infer support solely from a code path compiling conditionally.

## Verification layers

1. **Acceptance tests** — `xmake test -v` (Clang Debug)
2. **Unit / component tests** — per-slice test binaries
3. **Sanitizer gates** — ASan, UBSan, TSan, Valgrind
4. **Code quality** — `clang-tidy`, `.clang-format`
5. **Formal models** — TLA+ specs under `spec/tla/`

See `docs/verification/README.md` for the full verification matrix.
