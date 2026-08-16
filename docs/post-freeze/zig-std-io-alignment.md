# Zig `std.Io` ↔ Sluice Structural Alignment (Post-Freeze R0)

**Status:** COMPLETE (2026-08-16)
**Baseline SHA:** `d9184de`
**Companion:** `docs/architecture/zig-io-conformance-map.md` — the pre-freeze
**semantic** conformance map (reconciled 2026-08-13 against Codeberg master
`89e0881f`). This document adds the **structural / conceptual-ownership**
axis requested by the post-freeze hygiene task. Where a row's semantics are
already classified there, this document cites it and classifies only the
structure.

---

## 1. Zig sources actually read (no memory)

- `lib/std/Io/Threaded.zig` — `Threaded.io()` full vtable wiring; `Threaded.init`
  (pool = `cpu_count - 1`, `async_limit`, allocator used *only* by
  async/concurrent/group entry points; SIGIO/SIGPIPE handlers for interrupt).
- `lib/std/Io/IoUring.zig` — event-loop `idle()`: per-thread io_uring,
  `submit_and_wait(1)`, CQE dispatch by `Completion.UserData` enum in
  `cqe.user_data`, ready-fiber scheduling, `ASYNC_CANCEL` on `EINTR`.
- `lib/std/Io.zig` — `Io.Evented` compile-time platform dispatch (IoUring on
  Linux x86_64/aarch64, Kqueue on BSD/macOS, `void` elsewhere); `Group.cancel`
  / `Select.cancel` scoping.
- The "lib/std/Io" directory layout (Dir, File, IoUring, Kqueue, Reader,
  Writer, Threaded, net/, tty — sibling modules behind the interface).
- Sources: context7 index of `ziglang/zig` **master** (snippets above cite
  upstream paths at the master branch) and the repo-structure listing of the
  GitHub mirror.
  The pre-freeze semantic map additionally pinned Codeberg master `89e0881f`
  (2026-08-11); no contradicting drift was observed between that anchor and
  the master snippets read today.

### 1.1 The Zig structural model (as-read, condensed)

```text
Io.zig                 interface ONLY: userdata + vtable (~106 slots), zero
                       backend code, owns no state; high-level combinators
                       (Future/Group/Select/CancelProtection) are generic over
                       the interface
Io/Threaded.zig        ONE whole backend: pool + async_limit + signal handling;
                       implements every vtable slot (including mutexLock /
                       conditionWait / clock) for the thread strategy
Io/IoUring.zig         evented backend: fibers + per-thread rings (Kqueue.zig
                       same shape); selected by Io.Evented per platform
Io/Reader.zig,
Io/Writer.zig          streaming interfaces (the sync-core vocabulary)
Io/File.zig, Dir.zig,
Io/net.zig             I/O domains as sibling modules behind the interface
```

Load-bearing structural facts:

- **The interface file owns no mechanism and no state.** Sync primitives,
  clock, deadlines are *vtable parameters/slots*, not subsystems.
- **There is no central timer file and no central primitives file.** Deadlines
  ride on operations/waits; each backend implements its own time source.
- **One backend = one file.** Backend mechanism never leaks into the
  interface or into other domains.
- **Blocking work** (Threaded strategy) is thread-pool offload bounded by
  `async_limit`; the allocator is required only for the async execution
  entries, everything else runs inline on the caller.
- **Completion routing** is typed userdata in the kernel completion
  (cookie/enum in `user_data`), not pointer identity.

---

## 2. Concept map (structural axis)

`Alignment` here grades **module-boundary/ownership shape**, independent of
the semantic F/I/M/P/X classes in the conformance map.

| Concept | Zig std.Io | Sluice current | Alignment | Action |
|---|---|---|---|---|
| Io context | `Io` = copyable interface value, no state | `AsyncIoContext` = move-only owner (DIV-01, semantic I) | divergent-but-valid (see SD-2) | none |
| Interface vs mechanism | `Io.zig` has zero implementation | `Scheduler` class = interface + mechanism + all primitive state in one header | **divergent** | partially improved by R1 split (file level); interface split = PF-1, deferred |
| backend | one backend = one file (`Io/Threaded.zig`, `Io/IoUring.zig`, `Io/Kqueue.zig`) | `threadpool_backend.cpp`, `uring_backend.cpp`, `sync_backend.hpp`, `fake_backend.hpp` — one file each | **aligned** | none |
| operation/request | `Operation` tagged union + `Operation.Storage` + `Pending.Userdata` | `ReadOp`/`WriteOp`/… + backend-owned `RequestSlot` arena (DIV-02) | divergent-but-valid | none (frozen ADR) |
| completion | backend makes the waiting task runnable directly | Scheduler polling bridge + 2 ms backstop (DIV-04/05, Phase G bridge) | divergent-but-frozen | none (Phase G owns; policy-gated) |
| wake | `submit_and_wait` return / fiber ready-queue | `SchedulerWakeHandle` + park/wake protocol (R1–R4) | aligned concept, different mechanism | none |
| interrupt | signal handlers installed by backend (SIGIO/SIGPIPE) | interrupt bridge gated by `backend_wait_active_` | aligned (backend-owned interruption) | none |
| timer | no central timer module; deadlines as wait/op parameters | centralized Scheduler deadline heap + `advance_clock` + `*_until` | **divergent shape, aligned semantics** (clock row F in semantic map) | none (centralized heap is the C++-natural single-owner design; split isolates it in `scheduler_timer.cpp`) |
| deadline | vtable clock entries (`now`/`sleep`/`Timeout`) | `deadline_t`, `await_*_deadline`, timer select case | aligned | PF-3 convenience gap only |
| cancellation | `cancel`/`cancelRequested` + CancelProtection regions | layered model (task/wait/op/interrupt) + CancelToken epochs | aligned-at-concept (semantic map: F) | none |
| blocking work | `Threaded` pool, `async_limit` | `ThreadPoolBackend` bounded offload + core `BlockingIoPool` | aligned shape (DIV-03 keeps concepts distinct) | none |
| thread pool | `Io/Threaded.zig` single file | `threadpool_backend.cpp` single file | **aligned** | none |
| file I/O | `Io/File.zig`, `Io/Dir.zig` | core `file.cpp` + positional/stream async ops | aligned (domain module per side) | none |
| socket I/O | `Io/net.zig` + `net/` | absent (X, DIV-08) | out of scope | workload-gated |
| process I/O | `Io` process vtable ops | absent (X) | out of scope | PF-2 |
| error model | Zig error sets | `Result<T>`/`IoError` | valid difference (SD-4) | none |
| resource ownership | explicit allocators everywhere | RAII + construction-time bounds | valid difference (SD-1) | none |
| **implementation TU organization** | mechanism spread by concept across files | `scheduler.cpp` concentrated **10 concepts** | **divergent — the one real finding** | **R1 SPLIT (this round)** |

---

## 3. Structural divergences, classified

### ALIGN_NOW (done in R1)

**SD-A1 — Scheduler implementation TU concentration.** Zig spreads mechanism
by concept (no file owns more than one); Sluice's `scheduler.cpp` holds ten
concepts (audit §4). Fixed by pure code motion into concept TUs — the repo's
own established pattern (`select_event.cpp`, `select_timer.cpp`,
`queue_port.cpp`). No semantic change; no new abstraction.

### VALID_CPP_DIFFERENCE (keep, with rationale)

**SD-1 — RAII + construction-time bounds vs explicit allocators.** Sluice
passes no allocator; capacities are constructor parameters and violation is
fail-fast. This is the repository's C++ ownership identity (AGENTS.md §12);
Zig's allocator parameterization has no C++20 equivalent worth importing
(would be a new public API — Stop 5).

**SD-2 — Concrete `Scheduler` class vs stateless `Io` interface + vtable.**
Zig needs a vtable because any-holder-must-submit is a runtime-open set of
backends; Sluice has exactly one scheduler mechanism and a *compile-time*
backend seam (`AsyncBackend`, DIV-13). Introducing an Io-like vtable split
inside the Scheduler would be a scheduler redesign (Stop 2) with zero
polymorphism payoff (AGENTS.md §13 / task §13: no virtual interface without
true polymorphism). Revisit only via PF-1 with application evidence.

**SD-3 — Backend-owned request storage vs caller-owned `Operation.Storage`.**
Frozen ADR (DIV-02, implemented on all backends, revisit trigger recorded).

**SD-4 — `Result<T>`/`IoError` vs error sets.** Frozen core contract.

**SD-5 — Centralized deadline heap.** Zig has no central timer module;
Sluice's heap is the single-owner C++ design for `advance_clock` +
multi-worker earliest-deadline recompute. Same semantics (semantic map clock
row F); mechanism stays, R1 merely isolates it in `scheduler_timer.cpp`.

### DEFER_TO_APPLICATION_EVIDENCE (recorded, not implemented)

**PF-1** `scheduler.hpp` interface/mechanism/state split (public-header
restructure; blocked on real seam pain).
**PF-2** process I/O gap (first subprocess workload).
**PF-3** public `sleep_for`/`Timeout` convenience type (ergonomic evidence).

---

## 4. Canonical vocabulary check

| Core concept | Canonical Sluice name | Strays? |
|---|---|---|
| request | `RequestKey` / `RequestSlot` / `ReadOp`… | no (op = descriptor, request = accepted identity — distinct on purpose) |
| completion | `Completion<T>` | no |
| wait/park/wake | `park_on_wake_source` / `wake_wait_one` / `signal_wake_locked` | no (no task/job/work/event-for-request strays; `runnable` is the scheduler's queue element, distinct concept) |
| deadline/timeout | `deadline_t`, `*_until`, `*_deadline` | no stray `timeout` type names |
| cancel | `cancel_waiter`, `CancelToken`, `CancelProtection` | layered names per §11, intentional |
| executor/backend | `Scheduler` (execution) vs `AsyncBackend` (I/O mechanism) | kept distinct; matches AC-8; **do not** rename either to Zig's `Threaded` (DIV-03) |
| blocking offload | `ThreadPoolBackend`, `BlockingIoPool` | distinct from execution strategy (DIV-03) |

One core concept = one canonical name holds across the codebase today. No
renames performed or proposed (public-API freeze).

---

## 5. Application/foundation seam (post-R1 shape)

```text
Application
    │  application_runtime.hpp, group.hpp, future.hpp, select.hpp,
    │  async_mutex/condition/semaphore/rwlock/event/queue.hpp,
    │  timer_registration.hpp, cancel.hpp, async_io_context.hpp
    ▼
Runtime coordination        ApplicationRuntime / Scheduler (scheduler.cpp, scheduler_park_wake.cpp,
    │                       scheduler_timer.cpp + primitive TUs — one concept per TU)
    ├── request lifecycle   detail/request_arena.hpp / request_slot.hpp
    ▼
Backend seam                AsyncBackend (threadpool_backend.cpp, uring_backend.cpp,
    │                       sync/fake) — one backend per file
    ▼
Synchronous core            Reader/Writer/Result/file/WAL (unchanged since freeze)
```

Dependency direction is strictly downward; no reverse edge exists today
(grep-verified: no `include/sluice/async` header includes a backend header;
backends include only `detail/` + context headers). Adding a backend-agnostic
application capability must not require editing backend files, and adding a
backend must not require editing runtime files — both hold at `d9184de` and
after R1.
