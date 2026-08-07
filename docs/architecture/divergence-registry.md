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
| Status | Active transitional decision (Phase B) |
| Introduced by | Existing Completion/backend-record separation; formalized as a transitional C++ adaptation by the explicit request contract |
| Governing ADR | ADR-explicit-io-request-contract (Accepted; Decision 2) |
| Reason | Zig places operation lifecycle and backend scratch in caller-owned `Operation.Storage`. The accepted request contract selects preserving caller-owned `Completion<T>` while the context/backend owns a bounded arena of `RequestSlot` objects identified by context, slot, and generation. This stages migration without making the prior pointer/container implementation acceptable. |
| Benefit | Preserves public submit signatures and avoids one-step Runtime/Batch/copy-pipeline migration while still enabling stable identity, bounded admission, and an allocation-independent accepted terminal path. |
| Cost | Context/backend memory scales with configured capacity; caller cannot supply storage directly; the non-reference backend `UringAsyncBackend` remains non-conforming until Phase D (`ThreadPoolBackend` migrated in Phase E). |
| Current evidence | Phase B activates the transitional backend-owned `RequestSlot` arena for the reference backends (FakeAsyncBackend, SyncBackend) only; **Phase E (PR #64) migrated `ThreadPoolBackend` onto the same unified `RequestArena` lifecycle** (see DIV-03/DIV-12 resolutions and `docs/architecture/phase-e-compliance-gate.md`). `UringAsyncBackend` retains its pointer/container tracking until Phase D. The shared `sluice::async::detail::RequestArena` provides one logical capacity per context/backend pair (ADR Decision 2; no two independently oversubscribable stores). PR #63's review closeout moved queue linkage and submission order INTO the slot (`ready_next_`, `submit_seq_`) and removed FakeAsyncBackend's side-band `HandleRing` FIFO and per-kind staging deques; the arena owns a construction-time bounded ready-ring so reap preserves backend-known (terminal-winner) order (ADR Decision 9) for all backends. **C2a (Phase C capacity conformance):** the shared capacity suite (`tests/backend_conformance_test.cpp` `run_capacity_cases`) proves bounded capacity / `would_block` rejection / exact accounting / recycle for the migrated backends (Fake, ThreadPool) through the `make_backend_with_capacity` test seam. Uring has no RequestArena capacity before Phase D, so its capacity coverage is recorded as a `not_implemented` manifest record (`uring_capacity_not_implemented`, `scripts/backend_conformance_manifest.py`), which enters Uring's verdict via `applicable_evidence_for_backend()` — Uring stays NOT CONFORMING and is never skip-as-pass for capacity. See [`docs/architecture/phase-c2a-compliance-gate.md`](phase-c2a-compliance-gate.md). **C2b (Phase C identity / generation / cancel-winner conformance):** the arena-level state-transition matrix, generation/stale-event/provenance contract, and identity-bearing reap order are pinned in `request_lifecycle_scheme_b_test` / `request_arena_test` / `request_arena_death_test`; Fake and ThreadPool integration evidence for cancel-winner and publication-boundary semantics (rows 5–8) is pinned in `backend_scheme_b_race_test` / `threadpool_backend_scheme_b_race_test`. Eight single-point production mutations (A, B1, B2, C, D, E, F, G) prove the cases fail on deliberately nonconforming identity behavior (`docs/verification/phase-c2b-identity-mutation-evidence.md`). Uring's C2b coverage is recorded as a `not_implemented` manifest record (`uring_c2b_identity_not_implemented`), which enters Uring's verdict — Uring stays NOT CONFORMING and is never skip-as-pass for identity. See [`docs/architecture/phase-c2b-compliance-gate.md`](phase-c2b-compliance-gate.md). **C2c (Phase C waiter / borrow / delivery-lease conformance):** the arena-level borrow-lifetime matrix (commit owns → reap releases; survives pending/enqueued/running/backend_ready and every cancel/wait-cancel path), single-waiter registration matrix + no-overwrite cardinality (the matrix is pinned from ADR Decision 10 — registration is orthogonal to execution state and only reap closes it, so running/backend_ready registration is legal and terminal-won-but-unreaped waiters are delivered by reap), waiter-cancel vs I/O-cancel independence, and the move-only lease transfer chains + register-vs-reap / cancel_waiter-vs-reap races are pinned in `request_waiter_borrow_lease_test`; Fake and ThreadPool integration evidence (rows 11–14) is pinned in `backend_c2c_waiter_borrow_test` / `threadpool_backend_c2c_waiter_borrow_test`, including the ThreadPool running and backend_ready-before-reap borrow windows (a worker finishing its syscall is NOT the borrow lifetime end) and waiter registration inside the running/backend_ready windows. Nine single-point production mutations (A–I) prove the cases fail on deliberately nonconforming borrow/waiter/lease behavior (`docs/verification/phase-c2c-waiter-borrow-mutation-evidence.md`). Uring's C2c coverage is recorded as a `not_implemented` manifest record (`uring_c2c_borrow_waiter_not_implemented`), which enters Uring's verdict — Uring stays NOT CONFORMING and is never skip-as-pass for borrow/waiter/lease. The row 12b/14b boundary (real public waiter / RequestHandle / Scheduler registration consumer and real Scheduler routing-record lifetime) is the Accepted ADR's own Decision 10 deferral to Phase F, not a new divergence. See [`docs/architecture/phase-c2c-compliance-gate.md`](phase-c2c-compliance-gate.md). **C2d (Phase C failure-injection / accepted-terminal conformance):** rows 9–10 are pinned on the REAL `ThreadPoolBackend` (`threadpool_backend_c2d_failure_test`, 12 cases) via `SLUICE_ASYNC_INTERNAL_TESTING`-guarded deterministic seams — ADR Gate-4 per-stage pre-commit injection at reserve / prepare / commit-boundary: the injected reserve failure (would_block) leaves the Completion idle with zero residue, the injected prepare failure rolls back the candidate slot (capacity immediately recyclable), and the COMMIT-BOUNDARY arm (the binding CAS wins, then commit is injected to fail) executes the REAL `rollback_binding_before_accept` + slot rollback — the only executable instance of that branch in the corpus — returning the Completion to fully reusable idle; transactional pre-commit rejection (binding-CAS loss → `invalid_state`, zero residue, capacity recyclable), partial worker-startup failure (stop + join + synchronous rethrow — the finding P1-04 regression test), post-commit permanent dispatch failure (injected between enqueue and dispatch push, inside `work_mtx_`, handle never visible to a worker — the ADR Decision-12 winner candidate): submit success, exactly one defined `backend_error` terminal, once-only reap publication, borrow active until reap, no worker/syscall execution (size + void paths), post-commit zero-allocation under always-throw operator new (real worker path and injected path), and the dispatch-failure vs cancel exactly-one-winner invariant. Thirteen single-point production mutations (M1–M13) prove the cases fail on deliberately nonconforming behavior (`docs/verification/phase-c2d-failure-injection-mutation-evidence.md`); the ring-full invariant fail-fast path is untouched. The Fake reference path adds a full-window defined-error no-allocation case (`reference_backend_no_alloc_test`). Uring's C2d coverage is recorded as a `not_implemented` manifest record (`uring_c2d_failure_injection_not_implemented`), which enters Uring's verdict — Uring stays NOT CONFORMING and is never skip-as-pass for failure injection; its own `uring_submit_failure_test` drives the pre-RequestArena SQE model and does not satisfy the C2d contract. See [`docs/architecture/phase-c2d-compliance-gate.md`](phase-c2d-compliance-gate.md). |
| Revisit trigger | Re-evaluate when benchmarks or backend ABI evidence show caller-owned storage materially reduces per-request overhead and the public API migration cost for Runtime, Batch, and copy pipelines is controlled. |

---

## DIV-03: ThreadPoolBackend is Thread-Per-Op, Not Thread-Per-Task

| Field | Value |
|-------|-------|
| ID | DIV-03 |
| Status | Resolved (Phase E) |
| Introduced by | Implementation (no founding ADR for this specific model) |
| Governing ADR | ADR-execution-model §9.1 P2; resolved by ADR-explicit-io-request-contract (Accepted) Phase E |
| Reason | Zig `Threaded` = thread-per-TASK (execution strategy). The legacy Sluice `ThreadPoolBackend` was thread-per-OP (blocking I/O offload for the Evented scheduler) — a different concept at a different layer, with a misleading name and known unbounded resource issues. Phase E replaced it. |
| Benefit (historical) | Evented scheduler workers remained free during blocking I/O; a simple functional prototype under normal resource availability. |
| Cost (historical) | Thread creation per op (expensive); unbounded thread count; misleading name suggested a bounded pool; violated AC-7. |
| Resolution | Phase E (`feat/phase-e-bounded-threadpool-explicit-io`) replaced the per-op-thread model with a fixed pool of persistent blocking-I/O workers + a construction-time bounded dispatch ring + `RequestArena` / `RequestSlot` as the single request-lifecycle authority. Workers are created only at construction and worker storage never grows; the thread-per-OP model is gone. The name is retained for API continuity but the backend is now a bounded blocking-I/O offload mechanism, not a Zig `Threaded` translation. |
| Current evidence | `include/sluice/async/threadpool_backend.hpp` (ThreadPoolConfig, persistent workers, bounded dispatch ring, RequestArena); `tests/threadpool_backend_reap_test.cpp` (workers_spawned_for_test == worker_count for the backend's whole life); `docs/design/phase-e-bounded-threadpool-backend.md`. |
| Revisit trigger | None for the per-op model. The naming (ThreadPoolBackend) is retained for continuity; a future rename would be a separate API ADR. |

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
| Current evidence | `threadpool_backend.hpp:29-33`; `cancel()` returns but op continues. The shared `RequestArena` now records `cancel_intent_` on a `running` slot (ADR-explicit-io-request-contract Decision 11) so the arena layer is correct for the ThreadPool/Uring migration: `cancel()` returns `intent_recorded` without storing a terminal, and `record_terminal` records the REAL result VERBATIM (an ordinary success is NOT rewritten to canceled — cancel is best-effort). Only a backend that CONFIRMS the interruption took effect records `TerminalResult::err(canceled)` explicitly via `record_canceled`. The reference backends never enter `running`, so this is dormant at the reference layer. |
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
| Status | Resolved (Phase E) |
| Introduced by | Implementation drift (no ADR approves unbounded thread creation) |
| Governing ADR | ADR-explicit-io-request-contract (Accepted); corrected in Phase E |
| Reason | The legacy ThreadPoolBackend accepted unlimited concurrent ops with no capacity limit, no queue-full error, and no backpressure (Zig `Threaded` has `async_limit`/`concurrent_limit`). No Sluice ADR approved that unbounded growth. |
| Benefit (historical) | (None — this was an absence of constraint.) |
| Cost (historical) | Unbounded thread creation; OOM under load; no graceful degradation; violated AC-7. |
| Resolution | Phase E gave `ThreadPoolBackend` an explicit `ThreadPoolConfig{request_capacity, worker_count}`, a construction-time bounded dispatch ring (capacity == request_capacity), and `RequestArena` admission that returns synchronous `would_block` at capacity (Completion idle, no borrow). Worker threads are created only at construction and never grow. The unbounded path is gone. |
| Current evidence | `include/sluice/async/threadpool_backend.hpp` (ThreadPoolConfig, BoundedDispatchQueue); `tests/threadpool_backend_phase_e_test.cpp :: phase_e_capacity_full_returns_would_block`. |
| Revisit trigger | None. The bounds are now explicit and configurable; tuning the defaults is a benchmark decision, not a divergence. |

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

## DIV-14: prepare() Descriptor Validation Deferred for Reference Backends

| Field | Value |
|-------|-------|
| ID | DIV-14 |
| Status | Accepted |
| Introduced by | ADR-explicit-io-request-contract (Accepted) Decision 5/6 + Phase B closeout |
| Governing ADR | ADR-explicit-io-request-contract (Decision 6 `invalid_argument` vocabulary) |
| Reason | Decision 6 declares `invalid_argument` for "malformed operation: invalid length/buffer contract, impossible offset conversion, or invalid fd parameter form." The Phase B reference backends (FakeAsyncBackend, SyncBackend) perform NO real I/O — `fd` is a metadata carrier, not a syscall target (the test corpus deliberately uses `ReadOp{-1, ...}` as an "unused by fake" sentinel), and `BorrowMetadata` carries no offset. Enforcing the representable causes (negative fd, null buffer with nonzero length) at the reference `prepare()` would reject reference-backend test traffic without backing a real safety property. |
| Benefit | The reference layer stays focused on the arena/slot/terminal lifecycle contract (its actual scope); the test corpus is not churned to placate a check that guards no real syscall at this layer. |
| Cost | A malformed descriptor is NOT rejected at the reference `prepare()` — it is accepted and surfaces only at the full-backend prepare paths (Phase D/E), where a real syscall would actually dereference the fd/buffer. Callers cannot rely on `invalid_argument` from the reference backends for fd/buffer-form errors until those phases. |
| Current evidence | `include/sluice/async/detail/request_arena.hpp` `prepare()` (no fd/buffer-form check; the deferral is documented at the call site); ADR Phase B closeout "Round-4 review closeout" item 3; reference-backend tests use `ReadOp{-1, ...}` as a documented sentinel. **Phase E closeout (ThreadPool):** the ThreadPoolBackend now enforces the Decision 6 `invalid_argument` causes (negative fd, null buffer with nonzero length, offset beyond `off_t`, length beyond `SSIZE_MAX`) at its own descriptor-validation step before commit (`ThreadPoolBackend::validate_read/write/sync`); a non-negative but closed fd is accepted and later completes with the real `EBADF` terminal (no `fcntl(F_GETFD)` preflight). This divergence is therefore resolved for ThreadPool only; it remains open for Fake/Sync (reference, no real syscall) and Uring (Phase D). |
| Revisit trigger | Phase D (Uring) full-backend prepare path MUST enforce the Decision 6 `invalid_argument` causes before issuing a real syscall; the divergence is then fully resolved. (ThreadPool is resolved as of Phase E.) |

---

## Summary

| ID | Status | Area |
|----|--------|------|
| DIV-01 | Approved | Context shape |
| DIV-02 | Active transitional decision (Phase B) | Operation storage ownership |
| DIV-03 | Resolved (Phase E) | Backend execution model |
| DIV-04 | Approved | Wake integration |
| DIV-05 | Approved | Observation interval |
| DIV-06 | Approved | Durability ops |
| DIV-07 | Approved | Backend dispatch |
| DIV-08 | Approved | Scope |
| DIV-09 | Accepted | Registered buffers |
| DIV-10 | Accepted | Syscall cancellation |
| DIV-11 | Accepted | Cancel protection |
| DIV-12 | Resolved (Phase E) | Resource bounds |
| DIV-13 | Accepted | Backend extension point |
| DIV-14 | Accepted (partially resolved for ThreadPool in Phase E) | prepare() descriptor validation deferred for reference backends |
