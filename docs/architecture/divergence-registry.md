# Divergence Registry

**Purpose:** Every intentional or pending divergence from the Zig source-derived
model is registered here. Unregistered divergence is architectural drift.

**Baseline:** `b20bcc7` (master, including PR #60 and PR #61). Entries are
derived from code audit and ADR review. Proposed target designs are labeled;
they are not implementation evidence.

Status values:
- **Approved** — governed by an ADR or explicit design decision
- **Accepted** — documented and acknowledged, no ADR yet but no action needed
- **Proposed transitional decision** — a Proposed ADR selects a bounded
  exception and revisit trigger, but the divergence remains non-binding until
  that ADR is accepted
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

## DIV-02: Backend-Owned RequestSlot Separated from Caller-Owned Completion

| Field | Value |
|-------|-------|
| ID | DIV-02 |
| Status | Proposed transitional decision |
| Introduced by | Existing Completion/backend-record separation; formalized as a transitional C++ adaptation by the Proposed explicit request contract |
| Governing ADR | ADR-explicit-io-request-contract (Proposed; Decision 2) |
| Reason | Zig places operation lifecycle and backend scratch in caller-owned `Operation.Storage`. The Proposed request contract selects preserving caller-owned `Completion<T>` while the context/backend owns a bounded arena of `RequestSlot` objects identified by context, slot, and generation. If accepted, this stages migration without making the current pointer/container implementation acceptable. |
| Benefit | Preserves public submit signatures and avoids one-step Runtime/Batch/copy-pipeline migration while still enabling stable identity, bounded admission, and an allocation-independent accepted terminal path. |
| Cost | Context/backend memory scales with configured capacity; caller cannot supply storage directly; current backends remain non-conforming until migrated. |
| Current evidence | Current code: `completion.hpp`, backend-specific containers, and no RequestSlot arena. Target decision: ADR-explicit-io-request-contract Decisions 1–5. No production implementation exists yet. |
| Revisit trigger | Re-evaluate when benchmarks or backend ABI evidence show caller-owned storage materially reduces per-request overhead and the public API migration cost for Runtime, Batch, and copy pipelines is controlled. |

---

## DIV-03: ThreadPoolBackend is Thread-Per-Op, Not Thread-Per-Task

| Field | Value |
|-------|-------|
| ID | DIV-03 |
| Status | Corrective planned |
| Introduced by | Implementation (no founding ADR for this specific model) |
| Governing ADR | ADR-execution-model §9.1 P2 (mentions blocking offload but does not approve per-op thread model) |
| Reason | Zig `Threaded` = thread-per-TASK (execution strategy). Sluice `ThreadPoolBackend` = thread-per-OP (blocking I/O offload for the Evented scheduler). These are different concepts at different layers. The naming is misleading and the per-op thread model has known resource issues (unbounded, expensive). Corrective action is Phase E of the current roadmap. |
| Benefit | Evented scheduler workers remain free during blocking I/O; simple functional prototype under normal resource availability. |
| Cost | Thread creation per op (expensive); unbounded thread count; misleading name suggests a bounded pool; violates AC-7 (bounded resources). |
| Current evidence | `threadpool_backend.hpp:8-9` ("one worker thread per outstanding op"); `group.hpp:49-51` (Threaded mode = thread-per-task). |
| Revisit trigger | Phase E roadmap (persistent blocking-I/O offload design with bounded capacity). Naming correction at that time. |

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
| Revisit trigger | Phase G roadmap (backend progress signal / unified wake); if latency-sensitive workloads require sub-ms backend wake. |

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
| Revisit trigger | If backend wake integration is designed (Phase G); if 2ms latency is unacceptable for a workload. |

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
| Governing ADR | ADR-explicit-io-request-contract (Proposed target); correction remains unimplemented |
| Reason | ThreadPoolBackend accepts unlimited concurrent ops with no capacity limit, no queue-full error, and no backpressure. Zig `Threaded` has `async_limit`/`concurrent_limit`. No Sluice ADR approves unbounded resource growth. |
| Benefit | (None — this is not a benefit, it is an absence of constraint.) |
| Cost | Unbounded thread creation; OOM under load; no graceful degradation; violates AC-7. |
| Current evidence | `threadpool_backend.hpp:51-57` (documented risk); no capacity parameter; no `would_block` error. |
| Revisit trigger | Phase E roadmap after bounded RequestSlot reference lifecycle and conformance framework exist. |

---

## DIV-13: AsyncBackend Is a Public Extension Point, Not Internal Seam

| Field | Value |
|-------|-------|
| ID | DIV-13 |
| Status | Accepted |
| Introduced by | Implementation (ADR claims L0 internal, but header is public and RuntimeBuilder accepts arbitrary subclass) |
| Governing ADR | ADR-explicit-io-completion-authority §3 (trusted backend-author model) |
| Reason | `AsyncBackend` is defined in a public installed header (`async_io_context.hpp`). `RuntimeBuilder::backend()` accepts `std::unique_ptr<AsyncBackend>`. Any user can subclass it. The ADR claim of "internal seam" is contradicted by the actual API surface; this is a deliberate public extension point. |
| Benefit | Extensibility for custom backends (e.g., network, GPU, test harness) without exposing Completion mutators publicly: derived backends inherit the protected `try_claim()` / `publish()` / `rollback_claim_before_accept()` helpers, the sanctioned backend-author capability. Ordinary non-backend callers still cannot publish (negative-compile gate). |
| Cost | Requires a backend author contract, conformance suite (follow-up), negative-compile authority (wired into CI). Deriving AsyncBackend IS the sanctioned path to publication capability; this is not a capability-isolation boundary against deliberately subclassing code. |
| Current evidence | `async_io_context.hpp` (public abstract class, protected helpers); tests subclass AsyncBackend (FakeBackend, ProbeBackend); `scripts/verify-completion-authority-negative-compile.sh` proves non-backend code cannot publish. |
| Revisit trigger | If publication authority is ever moved to an internal seam (AsyncBackend internalized), revisit. |

---

## Summary

| ID | Status | Area |
|----|--------|------|
| DIV-01 | Approved | Context shape |
| DIV-02 | Proposed transitional decision | Operation storage ownership |
| DIV-03 | Corrective planned | Backend execution model |
| DIV-04 | Approved | Wake integration |
| DIV-05 | Approved | Observation interval |
| DIV-06 | Approved | Durability ops |
| DIV-07 | Approved | Backend dispatch |
| DIV-08 | Approved | Scope |
| DIV-09 | Accepted | Registered buffers |
| DIV-10 | Accepted | Syscall cancellation |
| DIV-11 | Accepted | Cancel protection |
| DIV-12 | Corrective planned | Resource bounds |
| DIV-13 | Accepted | Backend extension point |
