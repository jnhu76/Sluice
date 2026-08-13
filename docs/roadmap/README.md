# Roadmap

This directory lists active future work. Completed phases are documented in
`docs/history/closeout/`.

## Completed phases

The following phases are **complete** and no longer active:

- **v0.1-mvp** — blocking, measurable, Zig-inspired I/O core (tagged `v0.1.0`)
- **E10–E11 async substrate** — WaitNode, WaitQueue, Timer, deadline integration
- **E12 synchronization primitives** — Event, Semaphore, AsyncMutex, AsyncCondition, AsyncQueue, AsyncRwLock
- **E13 Select** — multi-arm Event/Timer select with deterministic causal tests
- **E14 Threaded/Evented parity** — semantic parity between Threaded and Evented execution strategies
- **E15 Runtime Foundation** — Mutex noexcept/fail-fast, Queue production implementation, cross-primitive closure, Future/Group/Batch
- **E16 Application Runtime** — `ApplicationRuntime` / `RuntimeTaskContext` lifecycle layer ([ADR-application-runtime](../adr/ADR-application-runtime.md), Accepted; `api-reference.md` §`sluice::async::ApplicationRuntime`)
- **Sync runtime** — positional I/O, BlockingIoPool, W1–W4 benchmarks (ADR-024S)

## Future work

### v0.1.x hardening / bug hunt

- **Status:** Ongoing maintenance.
- Regression coverage gaps, edge-case repair, documentation polish.

### Fuzz infrastructure

- **Status:** Not yet proposed.
- Coverage-guided or structure-aware fuzzing for async primitives.

### Real liburing validation

- **Status:** Environment-dependent.
- Requires liburing-equipped host. Stub path is the default build.

### Non-Linux portability evidence

- **Status:** Not yet started.
- macOS and Windows evidence for the synchronous core and ThreadPoolBackend.

### Documentation / API polish

- **Status:** Ongoing.
- Public API contract completeness, architecture documentation, examples.

## Non-goals

- No `async`/`await` or coroutine abstraction in the current phase.
- No P2300 sender/receiver model.
- No networking, timers (OS-level), mmap, or group commit.
- No universal performance claims.
- io_uring stays experimental unless real liburing validation supports promotion.

## Navigation

- **Proposed designs** — `docs/design/README.md`
- **Historical plans** — `docs/history/implementation-plans/`
- **Active ADRs** — `docs/adr/README.md`
