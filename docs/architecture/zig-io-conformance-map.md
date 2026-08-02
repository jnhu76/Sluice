# Zig std.Io → Sluice Conformance Map

**Baseline:** `b20bcc7` (master, including PR #60 and PR #61). Zig sources:
`zig/lib/std/Io.zig` and the local `Io/{Threaded,Uring,Kqueue,Dispatch,fiber}.zig`
reference files.

The classification records the selected architecture target. A row explicitly
states when current production code has not yet migrated; a target decision is
not implementation evidence.

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
| `Operation.Storage` (caller-owned reusable slot with intrusive lifecycle lists) | Caller allocates stable bounded storage for submission→pending→completion and identity-preserving reuse | Target: caller-owned Completion bound to context/backend-owned bounded `RequestSlot(context, slot, generation)`; current code still uses backend-specific dynamic records | **I** | ADR-explicit-io-request-contract Decisions 1–5; DIV-02 | Intentional **transitional C++ adaptation**: core stable/bounded storage semantics are restored with different ownership to preserve API. Current backends are not conforming until Phases B–E migrate them. Caller-owned storage retains an explicit revisit trigger. |
| `Pending.Userdata` (7×usize backend scratch per op) | Backend-private bounded scratch in stable operation storage | Target RequestSlot has fixed/pre-reserved backend scratch; current ThreadPool uses `std::function`/thread/deque and Uring uses maps/deques | **A** | Current code: threadpool/uring sources; target: ADR-explicit-io-request-contract Decision 3 | Exact Zig scratch ABI is not required, but current hot-path heap mechanics remain accidental drift. The target is decided; implementation and per-backend layout evidence are pending. |
| `operate` (blocking-shaped I/O on current task) | Submit + await on the current concurrency unit; returns result inline | `op_helpers::read_all/write_all` (poll-loop) or `RuntimeTaskContext::submit_* + await_completion` (Fiber suspend) | **F** | `op_helpers.hpp:1-64`; `application_runtime.hpp:60-86` | Both paths preserve blocking-shaped semantics. The Runtime path is the Evented equivalent; op_helpers is the Threaded equivalent. |
| `Batch` (caller storage + concurrent await + intrusive lists) | N ops submitted together; await ≥1; iterate in completion order; cancel as a whole | `Batch` class (driver over AsyncIoContext; `vector<unique_ptr<Slot>>`) | **I** | `batch.hpp:1-137` | Semantic contract preserved (submit N, await ≥1, iterate reap order, cancel). Implementation is a driver over per-op submit, NOT a native backend batchAwait vtable. Documented as deliberate narrowing in Batch header. The mechanism diverges from Zig (no native batch vtable entry) but the caller-visible semantics are equivalent. |
| `Threaded` (thread-per-task execution strategy) | Each async task gets a dedicated OS thread; blocking waits are natural | `Group()` default mode (thread-per-task via `std::thread`) + `ThreadPoolBackend` (thread-per-op for blocking-I/O offload) | **I** | `group.hpp:49-51`; ADR-execution-model §2 | Zig Threaded = thread-per-TASK. Sluice Group Threaded = thread-per-task (faithful). ThreadPoolBackend = thread-per-OP (blocking-I/O offload), which is a DIFFERENT concept at a different layer. ThreadPoolBackend is NOT an implementation of Zig Threaded; it is a blocking-I/O offload mechanism for the Evented scheduler. The naming conflates these. See divergence registry DIV-03. |
| `Evented` / `Uring` (scheduler + fiber + ring) | Suspend task/fiber on wait; scheduler worker remains free; kernel ring for I/O; backend owns completion wake; per-thread backend state | `Scheduler` + `Fiber` + `UringAsyncBackend` (gated) + `AsyncIoContext` + `ApplicationRuntime` | **I** | ADR-execution-model §2; `scheduler.hpp:212-265` | Execution semantics (Fiber suspends, worker free, task resumes) are faithful. But backend architecture diverges: Zig integrates scheduler + backend + wake in one Io vtable; Sluice splits into 4 independent components connected by a polling bridge. Uring integration topology also diverges (standalone backend vs. per-thread ring). Classified **I** overall: execution semantic F, backend boundary I, wake integration I. |
| Completion wake (backend-owned wake integration) | Backend completion directly makes the waiting task runnable via the Io vtable | Scheduler polling bridge: `poll()`/`wait_one()` → `wake_ready_completions_locked()` → route Fiber | **I** | ADR §9.4.1 P3 (decoupled wake domains); ADR §9.4.12 (BACKEND-WAKE-SEAM-GAP) | Sluice backend does NOT directly wake the Scheduler. The Scheduler observes backend readiness via polling. Explicitly accepted as P3; P5 (interruptible backend wait) deferred. |
| Durability ops (`fileSync` / direct vtable method) | Durability as a property of write ops or explicit Io call | `SyncDataOp`/`SyncAllOp` as first-class async operations with own Completion | **I** | ADR-async-io-model §3; `async_io_context.hpp:32-49` | Zig models durability inline; Sluice makes it a schedulable, cancellable, observable operation. Semantic enrichment, not loss. See DIV-06. |
| Cancellation region (`CancelProtection`) | Structured cancel protection: protected/unprotected regions; `recancel`; `swapCancelProtection` | `CancelToken` (cooperative, single-shot) + `check_cancel`; no protection regions | **M** | ADR-async-io-model §7 X6 (deferred to job 021); `cancel.hpp` | Structured cancellation (protection regions, recancel) not implemented. Current model is minimal best-effort. |
| Futex / sync capabilities (Io-aware waits) | `futexWait`, `futexWake`, Mutex, Condition, Event, Semaphore, RwLock — all Io-aware (suspend fiber, not thread) | E10–E12 primitives: `AsyncMutex`, `Event`, `AsyncCondition`, `AsyncQueue`, `Semaphore`, `AsyncRwLock` via `WaitQueue`/`WaitNode` + Scheduler | **F** | `scheduler.hpp:276-500`; ADR-execution-model §9 frontier E10-E12 | Full set implemented. All suspend the Fiber (not the OS thread) under Evented. Threaded fallback uses Future/WaitPolicy. |
| Resource bounds (`Io.Limit`, `async_limit`, `concurrent_limit`) | Explicit concurrency limits and observable admission pressure | Target: bounded context/backend `request_capacity` with `would_block`; current ThreadPool is unbounded and other backends have no common request capacity | **A** | Current code: backend headers; target: ADR-explicit-io-request-contract Decision 13; DIV-12 | The correction is specified but unimplemented. `request_capacity`, pipeline depth, Runtime workers, blocking workers, and uring queue depth are explicitly distinct. |
| `Group` (cancel-propagation boundary; swallows Cancel) | Unordered task set; await/cancel as a whole; tasks swallow `error.Canceled` | `Group` class (Threaded + Evented modes) | **F** | `group.hpp:1-258` | Semantic preserved: cancel-propagation boundary, tasks swallow exceptions, await waits for all. |
| `Select` (multi-wait winner protocol) | Wait on multiple sources; exactly-once winner | E13 `select()` template + `SelectGroup`/`SelectPort`/`SelectArmSlot` | **F** | `scheduler.hpp:16`; `select_fwd.hpp`; E13 spec | Implemented with exactly-once winner CAS. |
| Registered buffers / files | Kernel-pinned buffers for zero-copy io_uring | Not implemented | **M** | ADR-async-io-model §5 (deferred); §14 | Explicitly deferred pending lifetime contract. |
| Signal-based blocking syscall cancellation | `pthread_kill`/`tgkill` to interrupt blocking I/O | Not implemented | **M** | `threadpool_backend.hpp:29-33` | Portable cancel of in-flight blocking syscall deferred. Cancel is best-effort (op completes with real result). |
| `AsyncBackend` (L0 internal seam) | Backend implementations are library-internal; caller never subclasses or sees backend internals | Public `AsyncBackend` extension point with trusted backend-author contract. `Completion` mutation stays private; derived backends receive protected `try_claim` / `publish` / `rollback_claim_before_accept` capabilities | **I** | ADR-explicit-io-completion-authority §3; DIV-13 (Accepted); `completion.hpp` (private mutators + friend AsyncBackend); `scripts/verify-completion-authority-negative-compile.sh` | Custom/test backends remain injectable without exposing mutation APIs to ordinary callers. Backend subclasses enter the trusted computing base and must pass a future backend conformance suite. |

---

## Summary by Classification

| Class | Count | Key areas |
|-------|-------|-----------|
| F (Faithful) | 5 | Operation, operate, futex/sync, Group, Select |
| I (Intentional) | 8 | Io capability shape, transitional Operation.Storage ownership, Batch driver, Threaded naming, Evented topology, completion wake bridge, SyncDataOp/SyncAllOp, AsyncBackend extension point |
| A (Accidental) | 2 | Resource bounds (unbounded ThreadPoolBackend), Pending.Userdata heap model |
| M (Missing) | 3 | CancelProtection, registered buffers, signal-based syscall cancel |
| O (Obsolete) | 0 | — |
| U (Unresolved) | 0 | — |

---

## Notes

1. The largest current implementation gap is **resource bounds**. The Proposed
   request contract selects bounded `request_capacity` and synchronous
   `would_block`, but ThreadPoolBackend remains unbounded and no backend yet has
   the common arena. The row stays **A** until implementation evidence exists.

2. The **completion wake bridge** (polling instead of direct backend→Scheduler
   wake) is an explicit, documented, accepted decision (E9 P3). It is classified
   **I** despite being a significant structural difference from Zig.

3. **Operation.Storage** ownership is now classified **I** as an accepted
   transitional adaptation under DIV-02: caller-owned Completion plus a
   backend/context-owned bounded RequestSlot arena. This classification approves
   the target ownership decision, not today's pointer/container mechanics.

4. **Pending.Userdata** heap mechanics remain **A** (Accidental Drift). The
   target bounded RequestSlot scratch and intrusive/pre-reserved linkage are
   specified, but no backend has implemented them yet.

5. **ThreadPoolBackend** is a blocking-I/O offload mechanism (thread-per-op),
   NOT an implementation of Zig's `Threaded` execution strategy (thread-per-
   task). Group Threaded mode is the faithful Zig Threaded equivalent.
   Conflating these leads to incorrect capacity reasoning.

6. **Evented** is no longer classified as a single **F**. The execution
   semantic (Fiber suspends, worker free) is faithful, but the backend
   architecture (4 independent components + polling bridge vs. Zig's
   integrated Io vtable) is an intentional structural divergence. The
   coarse single-F classification masked these differences.

7. **AsyncBackend** is classified **I** (Intentional Divergence), not **U**.
   DIV-13 is Accepted: the ADR claim that L0 is an internal seam is superseded
   by the public extension point decision (ADR-explicit-io-completion-authority
   §3). Publication mutators stay private on `Completion<T>`; derived backends
   receive the protected `try_claim` / `publish` / `rollback_claim_before_accept`
   helpers as the sanctioned backend-author capability. Ordinary non-backend
   callers still cannot forge publication (negative-compile gate). A backend
   conformance suite is a follow-up requirement.
