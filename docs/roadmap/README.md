# Roadmap

This directory lists only active future work. Completed phases are documented in `docs/history/closeout/`.

## Completed phases

The following phases are **complete** and no longer active:

- **v0.1-mvp** — blocking, measurable, Zig-inspired I/O core (tagged `v0.1.0`)
- **E10–E13 async substrate** — WaitNode, Timer, synchronization primitives, formal models
- **Sync runtime** — positional I/O, BlockingIoPool, W1–W4 benchmarks (ADR-024S)
- **E15 Runtime Foundation** — Mutex noexcept/fail-fast, Queue production implementation, cross-primitive closure

## Future work

### E13 Select — Production Implementation

- **Status:** Proposed (design complete, implementation not authorized)
- **Blocking:** Independent design review pass
- **Related ADR:** ADR-execution-model
- **Related docs:** `docs/design/e13-select-production-architecture.md`, `docs/design/e13-select-public-api.md`

### E14 Evented — Threaded vs Evented Semantic Parity

- **Status:** Proposed (preparation complete)
- **Blocking:** E13 select implementation, Evented design authorization
- **Related ADR:** ADR-execution-model

### E12-F AsyncRwLock — Production Implementation

- **Status:** Proposed (design exists)
- **Blocking:** E12-F design review and authorization
- **Related ADR:** ADR-execution-model

### E16 Application Runtime

- **Status:** Not yet proposed or discussed
- **Do not present as accepted or planned.**

## Non-goals

- No `async`/`await` or coroutine abstraction
- No P2300 sender/receiver model
- No networking, timers, mmap, or group commit
- No universal performance claims
- io_uring stays experimental unless real liburing validation supports promotion

## Navigation

- **Proposed designs** — `docs/design/README.md`
- **Historical plans** — `docs/history/implementation-plans/`