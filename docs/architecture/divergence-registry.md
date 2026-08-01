# Divergence Registry

**Purpose:** Every intentional or pending divergence from the Zig source-derived
model is registered here. Unregistered divergence is architectural drift.

**Baseline:** `d299fc0` (master). Entries derived from code audit and ADR review.

Status values:
- **Approved** — governed by an ADR or explicit design decision
- **Accepted** — documented and acknowledged, no ADR yet but no action needed
- **Pending decision** — evidence insufficient or contradictory; needs resolution
- **Corrective planned** — accidental drift with a remediation plan

---

## DIV-01: Owning Move-Only Context vs. Lightweight Copyable Capability

| Field | Value |
|-------|-------|
| ID | DIV-01 |
| Status | Approved |
| Introduced by | ADR-async-io-model §3 A6 |
| Governing ADR | ADR-async-io-model |
| Reason | C++ move-only ownership provides compile-time exclusivity; Zig's copyable capability relies on convention. Single-runtime model makes lightweight façade unnecessary at L1. |
| Benefit | Compile-time prevention of accidental context sharing; clear destruction order. |
| Cost | Cannot pass context by value; every holder needs a reference/pointer; no zero-cost capability delegation. |
| Current evidence | `async_io_context.hpp:118-158` (move-only, owns backend); Runtime injects by reference. |
| Revisit trigger | If multi-context or capability-delegation patterns emerge (e.g., sub-runtimes, per-subsystem contexts). |

---

## DIV-02: Completion Separated from Operation Storage

| Field | Value |
|-------|-------|
| ID | DIV-02 |
| Status | Approved |
| Introduced by | Batch header design; ADR-async-io-model §4 |
| Governing ADR | ADR-async-io-model |
| Reason | Zig unifies caller storage (Operation.Storage: unused→submission→pending→completion) with backend scratch (Pending.Userdata). Sluice separates caller-visible Completion from backend-internal per-op state. This simplifies the public API at the cost of backend allocation. |
| Benefit | Simpler caller contract (only Completion to manage); backend freedom in internal tracking. |
| Cost | Backend per-op heap allocation (std::function, deque entry, thread); no zero-allocation accepted-op path in ThreadPoolBackend. |
| Current evidence | `completion.hpp:1-238`; `batch.hpp:9-22` (documents deliberate narrowing); `threadpool_backend.cpp:85` (per-op thread + function). |
| Revisit trigger | If per-op allocation becomes a measured bottleneck; if caller-owned operation storage is designed (Phase 1 roadmap). |

---

## DIV-03: ThreadPoolBackend is Thread-Per-Op, Not Thread-Per-Task

| Field | Value |
|-------|-------|
| ID | DIV-03 |
| Status | Accepted |
| Introduced by | Implementation (no founding ADR for this specific model) |
| Governing ADR | ADR-execution-model §9.1 P2 (mentions blocking offload) |
| Reason | Zig `Threaded` = thread-per-TASK (execution strategy). Sluice `ThreadPoolBackend` = thread-per-OP (blocking I/O offload for the Evented scheduler). These are different concepts at different layers. The naming is misleading but the architecture is sound: Group provides thread-per-task; ThreadPoolBackend provides I/O offload. |
| Benefit | Evented scheduler workers remain free during blocking I/O; simple correct implementation. |
| Cost | Thread creation per op (expensive); unbounded thread count; misleading name suggests a bounded pool. |
| Current evidence | `threadpool_backend.hpp:8-9` ("one worker thread per outstanding op"); `group.hpp:49-51` (Threaded mode = thread-per-task). |
| Revisit trigger | Phase 3 roadmap (portable blocking-I/O offload design); naming correction in Phase 0. |

---

## DIV-04: Decoupled Wake Domains (Backend Does Not Directly Wake Scheduler)

| Field | Value |
|-------|-------|
| ID | DIV-04 |
| Status | Approved |
| Introduced by | ADR-execution-model §9.4.1 P3; E9-CORRECTIVE |
| Governing ADR | ADR-execution-model |
| Reason | Zig backend completion directly makes the waiting task runnable via the Io vtable. Sluice decouples: backend publishes to ready queue; Scheduler observes via poll/wait_one/2ms interval. This avoids a lock-ordering hazard (backend mtx → Scheduler global_mtx) and keeps the backend interface minimal. |
| Benefit | No upward lock coupling; backend remains a simple leaf; Scheduler retains routing authority. |
| Cost | Up to 2ms observation latency in MIXED-WAKE mode; no instant backend→Fiber resume. |
| Current evidence | ADR §9.4.7.1 (2ms is protocol authority for MIXED-WAKE); `scheduler.hpp` worker loop (poll → wake_ready_completions_locked). |
| Revisit trigger | Phase 2 roadmap (unified progress/wake); if latency-sensitive workloads require sub-ms backend wake. |

---

## DIV-05: 2ms Bounded Observation Interval as Protocol Authority

| Field | Value |
|-------|-------|
| ID | DIV-05 |
| Status | Approved |
| Introduced by | ADR-execution-model §9.4.7.1 E9-CORRECTIVE |
| Governing ADR | ADR-execution-model |
| Reason | In MIXED-WAKE mode (backend outstanding + external-wake-capable wait), the MW-S2 participant parks on the Scheduler domain (wake_cv_) with a 2ms timeout. Backend progress is observed when the timeout expires. This is the protocol authority for backend progress in this mode — not merely "defensive." |
| Benefit | Avoids split-brain between backend cv and scheduler cv; single park point for mixed waits. |
| Cost | 2ms worst-case latency for backend completion in MIXED-WAKE; periodic CPU wake even if no progress. |
| Current evidence | ADR §9.4.7.1; scheduler worker loop implementation. |
| Revisit trigger | If backend wake integration is designed (Phase 2); if 2ms latency is unacceptable for a workload. |

---

## DIV-06: SyncDataOp/SyncAllOp as Async Operations

| Field | Value |
|-------|-------|
| ID | DIV-06 |
| Status | Approved |
| Introduced by | ADR-async-io-model §3 |
| Governing ADR | ADR-async-io-model |
| Reason | Zig models durability as a property of write operations or explicit `Io` calls. Sluice models `sync_data`/`sync_all` as first-class async operations with their own Completion. This makes durability explicit in the operation stream. |
| Benefit | Durability is schedulable, cancellable, and observable like any other op. |
| Cost | Additional op types in the vtable; slightly more complex backend interface. |
| Current evidence | `async_io_context.hpp:32-49` (SyncDataOp, SyncAllOp structs). |
| Revisit trigger | None planned. |

---

## DIV-07: Virtual Backend Interface (C++ vtable)

| Field | Value |
|-------|-------|
| ID | DIV-07 |
| Status | Approved |
| Introduced by | ADR-async-io-model §4 |
| Governing ADR | ADR-async-io-model |
| Reason | Zig uses a `*const VTable` pointer in the Io capability (manual vtable). Sluice uses C++ virtual dispatch (`AsyncBackend` abstract class). Semantically equivalent; idiomatic C++. |
| Benefit | Type safety, override checking, familiar C++ pattern. |
| Cost | Virtual call overhead (negligible vs. syscall); cannot inline backend calls. |
| Current evidence | `async_io_context.hpp:52-115` (AsyncBackend abstract class). |
| Revisit trigger | None. This is a pure C++ idiom choice with no semantic impact. |

---

## DIV-08: File-Only Scope

| Field | Value |
|-------|-------|
| ID | DIV-08 |
| Status | Approved |
| Introduced by | ADR-async-io-model §1; project scope |
| Governing ADR | ADR-async-io-model |
| Reason | Sluice targets file I/O (positional read/write/sync). Networking, timers, and signals are out of scope. Zig `std.Io` is broader (sockets, futex, etc.). |
| Benefit | Focused scope; simpler backend contract; no socket lifetime complexity. |
| Cost | Cannot directly compare with Zig's full capability set; some Zig patterns (e.g., Io-aware futex) have no Sluice equivalent. |
| Current evidence | `async_io_context.hpp` vtable: submit_read, submit_write, submit_sync_data, submit_sync_all only. |
| Revisit trigger | If networking or timer backends are added (would require vtable extension and new ADR). |

---

## DIV-09: Registered Buffers/Files Deferred

| Field | Value |
|-------|-------|
| ID | DIV-09 |
| Status | Accepted |
| Introduced by | ADR-async-io-model §5 (explicit deferral) |
| Governing ADR | ADR-async-io-model |
| Reason | Kernel-pinned buffers (io_uring registered buffers/files) require a lifetime contract that Sluice has not yet designed. Explicitly deferred. |
| Benefit | Avoids premature lifetime complexity; current path works without registration. |
| Cost | No zero-copy registered-buffer path; each op passes user pointers. |
| Current evidence | ADR §5, §14 (deferred); Uring backend uses normal pread/pwrite SQEs. |
| Revisit trigger | If io_uring performance requires registered buffers; when lifetime contract is designed. |

---

## DIV-10: No Signal-Based Blocking Syscall Cancellation

| Field | Value |
|-------|-------|
| ID | DIV-10 |
| Status | Accepted |
| Introduced by | Implementation; `threadpool_backend.hpp:29-33` |
| Governing ADR | None (documented limitation) |
| Reason | Portable cancellation of in-flight blocking syscalls (via `pthread_kill`/`tgkill`) is complex and platform-specific. Current cancel is best-effort: the op completes with its real result. |
| Benefit | Simplicity; no signal-safety hazards; no UB from interrupted syscalls. |
| Cost | Cannot interrupt a long-running fsync/pread/pwrite; cancel only affects waiting, not the syscall. |
| Current evidence | `threadpool_backend.hpp:29-33`; `cancel()` returns but op continues. |
| Revisit trigger | If workload requires interruptible long fsync; if io_uring cancel (IORING_OP_ASYNC_CANCEL) is integrated. |

---

## DIV-11: No Structured Cancellation Protection Regions

| Field | Value |
|-------|-------|
| ID | DIV-11 |
| Status | Accepted |
| Introduced by | ADR-async-io-model §7 X6 (deferred to job 021) |
| Governing ADR | ADR-async-io-model |
| Reason | Zig has `CancelProtection` with protected/unprotected regions and `recancel`. Sluice has cooperative single-shot `CancelToken` + E10 wait cancellation. Structured regions are deferred. |
| Benefit | Simpler current model; no nested protection complexity. |
| Cost | Cannot express "cancel here but not there" within a task; no recancel. |
| Current evidence | `cancel.hpp` (CancelToken); ADR §7 X6. |
| Revisit trigger | If task cancellation needs finer granularity; if Zig-style structured cancel is required by a consumer. |

---

## DIV-12: Unbounded ThreadPoolBackend (Accidental Drift)

| Field | Value |
|-------|-------|
| ID | DIV-12 |
| Status | Corrective planned |
| Introduced by | Implementation drift (no ADR approves unbounded thread creation) |
| Governing ADR | None — this is accidental |
| Reason | ThreadPoolBackend accepts unlimited concurrent ops with no capacity limit, no queue-full error, and no backpressure. Zig `Threaded` has `async_limit`/`concurrent_limit`. No Sluice ADR approves unbounded resource growth. |
| Benefit | (None — this is not a benefit, it is an absence of constraint.) |
| Cost | Unbounded thread creation; OOM under load; no graceful degradation; violates AC-7. |
| Current evidence | `threadpool_backend.hpp:51-57` (documented risk); no capacity parameter; no `would_block` error. |
| Revisit trigger | Phase 1 roadmap (bounded capacity design). This is the highest-priority corrective. |

---

## Summary

| ID | Status | Area |
|----|--------|------|
| DIV-01 | Approved | Context shape |
| DIV-02 | Approved | Operation storage |
| DIV-03 | Accepted | Backend execution model |
| DIV-04 | Approved | Wake integration |
| DIV-05 | Approved | Observation interval |
| DIV-06 | Approved | Durability ops |
| DIV-07 | Approved | Backend dispatch |
| DIV-08 | Approved | Scope |
| DIV-09 | Accepted | Registered buffers |
| DIV-10 | Accepted | Syscall cancellation |
| DIV-11 | Accepted | Cancel protection |
| DIV-12 | Corrective planned | Resource bounds |
