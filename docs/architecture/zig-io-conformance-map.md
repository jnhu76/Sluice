# Zig std.Io → Sluice Conformance Map

**Baseline:** `d299fc0` (master). Zig source: `zig/lib/std/Io.zig` (local reference).

Classification key:
- **F** — Faithful: core semantic preserved, C++ expression differs
- **I** — Intentional Divergence: approved by ADR/design/test
- **A** — Accidental Drift: no explicit decision, emerged during implementation
- **M** — Missing: Zig capability absent in Sluice
- **O** — Obsolete: doc/implementation no longer represents architecture
- **U** — Unresolved: evidence insufficient or contradictory

---

## Conformance Matrix

| Zig concept | Zig semantic purpose | Sluice equivalent | Class | Evidence | Consequence |
|---|---|---|---|---|---|
| `Io` (userdata + vtable) | Lightweight copyable capability; any holder can submit ops | `AsyncIoContext` (move-only, owning, mutex-serialized) | **I** | ADR-async-io-model §3 A6; `async_io_context.hpp:118-158` | Sluice context is an owner, not a borrowed capability. Runtime injects it. Acceptable for the current single-runtime model; a lightweight façade is deferred. |
| `Operation` (tagged union) | Explicit op descriptor with typed result | `ReadOp/WriteOp/SyncDataOp/SyncAllOp` structs | **F** | `async_io_context.hpp:32-49`; ADR §3 | Same semantic: explicit positional ops, typed results. C++ uses separate structs instead of a tagged union. |
| `Operation.Storage` (caller-owned reusable slot with 4-state intrusive list) | Caller allocates storage; backend uses it for submission→pending→completion lifecycle; no backend allocation per-op | `Completion<T>` (caller-owned, 3-state: idle→outstanding→ready) + backend-internal records (deque entries, thread handles, map entries) | **U** | `completion.hpp:1-238`; `batch.hpp:9-22` documents a Batch-scope decision to defer native Operation.Storage | Sluice separates the caller-visible Completion from backend-internal per-op state. The Batch header documents a local scope decision (minimize changes for that batch of work), NOT a permanent architecture-wide approval. Phase 1 roadmap will re-evaluate operation storage. Cannot classify as Intentional without a governing ADR. |
| `Pending.Userdata` (7×usize backend scratch per op) | Backend-private per-op scratch in caller-provided storage | ThreadPoolBackend: `std::function` + `std::thread` + deque entry; Uring: SQE userdata + internal map | **A** | `threadpool_backend.hpp:127-136`; no ADR approves per-op heap allocation as permanent model | Sluice backends use heap-allocated containers rather than fixed inline scratch. No explicit design decision approved this; it emerged during implementation for portability. Cost is per-op allocation on the hot path. Phase 1 roadmap will evaluate alternatives. |
| `operate` (blocking-shaped I/O on current task) | Submit + await on the current concurrency unit; returns result inline | `op_helpers::read_all/write_all` (poll-loop) or `RuntimeTaskContext::submit_* + await_completion` (Fiber suspend) | **F** | `op_helpers.hpp:1-64`; `application_runtime.hpp:60-86` | Both paths preserve blocking-shaped semantics. The Runtime path is the Evented equivalent; op_helpers is the Threaded equivalent. |
| `Batch` (caller storage + concurrent await + intrusive lists) | N ops submitted together; await ≥1; iterate in completion order; cancel as a whole | `Batch` class (driver over AsyncIoContext; `vector<unique_ptr<Slot>>`) | **I** | `batch.hpp:1-137` | Semantic contract preserved (submit N, await ≥1, iterate reap order, cancel). Implementation is a driver over per-op submit, NOT a native backend batchAwait vtable. Documented as deliberate narrowing in Batch header. The mechanism diverges from Zig (no native batch vtable entry) but the caller-visible semantics are equivalent. |
| `Threaded` (thread-per-task execution strategy) | Each async task gets a dedicated OS thread; blocking waits are natural | `Group()` default mode (thread-per-task via `std::thread`) + `ThreadPoolBackend` (thread-per-op for blocking-I/O offload) | **I** | `group.hpp:49-51`; ADR-execution-model §2 | Zig Threaded = thread-per-TASK. Sluice Group Threaded = thread-per-task (faithful). ThreadPoolBackend = thread-per-OP (blocking-I/O offload), which is a DIFFERENT concept at a different layer. ThreadPoolBackend is NOT an implementation of Zig Threaded; it is a blocking-I/O offload mechanism for the Evented scheduler. The naming conflates these. See divergence registry DIV-03. |
| `Evented` / `Uring` (scheduler + fiber + ring) | Suspend task/fiber on wait; scheduler worker remains free; kernel ring for I/O; backend owns completion wake; per-thread backend state | `Scheduler` + `Fiber` + `UringAsyncBackend` (gated) + `AsyncIoContext` + `ApplicationRuntime` | **I** | ADR-execution-model §2; `scheduler.hpp:212-265` | Execution semantics (Fiber suspends, worker free, task resumes) are faithful. But backend architecture diverges: Zig integrates scheduler + backend + wake in one Io vtable; Sluice splits into 4 independent components connected by a polling bridge. Uring integration topology also diverges (standalone backend vs. per-thread ring). Classified **I** overall: execution semantic F, backend boundary I, wake integration I. |
| Completion wake (backend-owned wake integration) | Backend completion directly makes the waiting task runnable via the Io vtable | Scheduler polling bridge: `poll()`/`wait_one()` → `wake_ready_completions_locked()` → route Fiber | **I** | ADR §9.4.1 P3 (decoupled wake domains); ADR §9.4.12 (BACKEND-WAKE-SEAM-GAP) | Sluice backend does NOT directly wake the Scheduler. The Scheduler observes backend readiness via polling. Explicitly accepted as P3; P5 (interruptible backend wait) deferred. |
| Cancellation region (`CancelProtection`) | Structured cancel protection: protected/unprotected regions; `recancel`; `swapCancelProtection` | `CancelToken` (cooperative, single-shot) + `check_cancel`; no protection regions | **M** | ADR-async-io-model §7 X6 (deferred to job 021); `cancel.hpp` | Structured cancellation (protection regions, recancel) not implemented. Current model is minimal best-effort. |
| Futex / sync capabilities (Io-aware waits) | `futexWait`, `futexWake`, Mutex, Condition, Event, Semaphore, RwLock — all Io-aware (suspend fiber, not thread) | E10–E12 primitives: `AsyncMutex`, `Event`, `AsyncCondition`, `AsyncQueue`, `Semaphore`, `AsyncRwLock` via `WaitQueue`/`WaitNode` + Scheduler | **F** | `scheduler.hpp:276-500`; ADR-execution-model §9 frontier E10-E12 | Full set implemented. All suspend the Fiber (not the OS thread) under Evented. Threaded fallback uses Future/WaitPolicy. |
| Resource bounds (`Io.Limit`, `async_limit`, `concurrent_limit`) | Explicit concurrency limits; `Limit.nothing`/`.unlimited`/`.limited(n)`; backpressure via `ConcurrentError` | ThreadPoolBackend: UNBOUNDED; Scheduler workers: per-invocation count; BlockingIoPool: configured; no unified resource model | **A** | `threadpool_backend.hpp:51-57` (documented risk); Zig `Threaded.zig:39-40` (`async_limit`, `concurrent_limit`) | Sluice has no equivalent of `async_limit`. ThreadPoolBackend accepts unlimited ops. No `ConcurrentError` / `would_block` on submit. This is accidental drift — no ADR approves unbounded thread creation. |
| `Group` (cancel-propagation boundary; swallows Cancel) | Unordered task set; await/cancel as a whole; tasks swallow `error.Canceled` | `Group` class (Threaded + Evented modes) | **F** | `group.hpp:1-258` | Semantic preserved: cancel-propagation boundary, tasks swallow exceptions, await waits for all. |
| `Select` (multi-wait winner protocol) | Wait on multiple sources; exactly-once winner | E13 `select()` template + `SelectGroup`/`SelectPort`/`SelectArmSlot` | **F** | `scheduler.hpp:16`; `select_fwd.hpp`; E13 spec | Implemented with exactly-once winner CAS. |
| Registered buffers / files | Kernel-pinned buffers for zero-copy io_uring | Not implemented | **M** | ADR-async-io-model §5 (deferred); §14 | Explicitly deferred pending lifetime contract. |
| Signal-based blocking syscall cancellation | `pthread_kill`/`tgkill` to interrupt blocking I/O | Not implemented | **M** | `threadpool_backend.hpp:29-33` | Portable cancel of in-flight blocking syscall deferred. Cancel is best-effort (op completes with real result). |
| `AsyncBackend` (L0 internal seam) | Backend implementations are library-internal; caller never subclasses or sees backend internals | `AsyncBackend` abstract class in PUBLIC installed header; `RuntimeBuilder::backend()` accepts `unique_ptr<AsyncBackend>`; any user can subclass | **U** | `async_io_context.hpp:52-115`; `application_runtime.hpp` builder API | ADR claims L0 is "never public-facing" but the type is a public extension point. Users CAN and MUST subclass it to provide custom backends. This forces Completion mutators public (backend subclasses need them). Must decide: truly internal (selector/config API) or formally public (backend author contract). See DIV-13. |

---

## Summary by Classification

| Class | Count | Key areas |
|-------|-------|-----------|
| F (Faithful) | 6 | Operation, operate, futex/sync, Group, Select, Evented execution semantics (partial) |
| I (Intentional) | 6 | Io capability shape, Batch (driver adaptation), Threaded naming, completion wake bridge, Evented backend architecture, SyncDataOp/SyncAllOp |
| A (Accidental) | 2 | Resource bounds (unbounded ThreadPoolBackend), Pending.Userdata heap model |
| M (Missing) | 3 | CancelProtection, registered buffers, signal-based syscall cancel |
| O (Obsolete) | 0 | — |
| U (Unresolved) | 2 | Operation.Storage separation, AsyncBackend public-vs-internal |

---

## Notes

1. The largest semantic gap is **resource bounds**. Zig's `Threaded` has explicit
   `async_limit` and `concurrent_limit` with backpressure (`ConcurrentError`).
   Sluice's ThreadPoolBackend has no capacity limit and no queue-full error.
   This is not approved by any ADR — it is implementation drift.

2. The **completion wake bridge** (polling instead of direct backend→Scheduler
   wake) is an explicit, documented, accepted decision (E9 P3). It is classified
   **I** despite being a significant structural difference from Zig.

3. **Operation.Storage** separation is classified **U** (Unresolved), not
   Intentional. The Batch header documents a local scope decision to defer
   native operation storage for that batch of work. This does NOT constitute
   architecture-wide approval of permanent Completion/storage separation.
   Phase 1 roadmap will re-evaluate. Do not treat the current model as
   approved.

4. **Pending.Userdata** heap model is classified **A** (Accidental Drift).
   No ADR or design document explicitly approved per-op heap allocation as
   the permanent backend scratch model. It emerged for portability during
   implementation. Phase 1 will evaluate alternatives (caller-owned storage,
   bounded arena, intrusive structures).

5. **ThreadPoolBackend** is a blocking-I/O offload mechanism (thread-per-op),
   NOT an implementation of Zig's `Threaded` execution strategy (thread-per-
   task). Group Threaded mode is the faithful Zig Threaded equivalent.
   Conflating these leads to incorrect capacity reasoning.

6. **Evented** is no longer classified as a single **F**. The execution
   semantic (Fiber suspends, worker free) is faithful, but the backend
   architecture (4 independent components + polling bridge vs. Zig's
   integrated Io vtable) is an intentional structural divergence. The
   coarse single-F classification masked these differences.

7. **AsyncBackend** is classified **U** (Unresolved). The ADR claims it is
   an internal seam, but it is a public installed header that users subclass.
   This contradiction forces Completion mutators public and prevents
   structural authority enforcement. Must be resolved by explicit decision:
   truly internal (Choice A) or formally public extension point (Choice B).
