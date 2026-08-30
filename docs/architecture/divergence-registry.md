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
| Status | Implemented (Phase B reference — PR #63; Phase E ThreadPool — PR #64; Phase D Uring — PR #78/#80/#83/#84); revisit trigger preserved |
| Introduced by | Existing Completion/backend-record separation; formalized as a transitional C++ adaptation by the explicit request contract |
| Governing ADR | ADR-explicit-io-request-contract (Accepted; Decision 2) |
| Reason | Zig places operation lifecycle and backend scratch in caller-owned `Operation.Storage`. The accepted request contract selects preserving caller-owned `Completion<T>` while the context/backend owns a bounded arena of `RequestSlot` objects identified by context, slot, and generation. This stages migration without making the prior pointer/container implementation acceptable. |
| Benefit | Preserves public submit signatures and avoids one-step Runtime/Batch/copy-pipeline migration while still enabling stable identity, bounded admission, and an allocation-independent accepted terminal path. |
| Cost | Context/backend memory scales with configured capacity; caller cannot supply storage directly. (The pre-Phase-D "Uring remains non-conforming until Phase D" tail is historical — Phase D closed it; the D1/D2 and D3/D4 reconciliation blocks below record the evidence.) |
| Current evidence | Phase B reference backends (PR #63) and Phase E ThreadPool (PR #64) migrated to the unified `sluice::async::detail::RequestArena` / `RequestSlot` lifecycle; Phase D migrated `UringAsyncBackend` onto the same arena with private-ring identity, reap publication, and close/drain/destruction conformance (D1–D4: PR #78/#80/#83/#84). **All four backends — FakeAsyncBackend, SyncBackend, ThreadPoolBackend, UringAsyncBackend — now use the RequestArena lifecycle**; the pre-Phase-D pointer/container tracking and the `uring_*_not_implemented` manifest records are gone (replaced by implemented real-mode evidence; see the migration record below). The arena owns a construction-time bounded ready-ring so reap preserves backend-known (terminal-winner) order (ADR Decision 9) for every RequestArena-backed backend. The shared capacity suite (`tests/backend_conformance_test.cpp` `run_capacity_cases`) proves bounded capacity / `would_block` rejection / exact accounting / recycle on all four backends. KernelIo verdict is decided by the ordinary machinery with per-suite real-mode attribution: ELIGIBLE on a complete real-mode evidence set, INCOMPLETE on a stub/subset build. The row 12b/14b boundary's Scheduler half (real Scheduler registration consumer and routing-record lifetime) is IMPLEMENTED by Phase F1 (issue #98: `Scheduler::await_completion_*` registers a real arena waiter; the Scheduler-owned `ReadyRoutingSink` consumes the identity-bearing reap; `Scheduler::cancel_waiter` / `RuntimeTaskContext::cancel_waiter` remove only the waiter). The residual 12b/14b deferral is CLOSED: Phase F3 (ADR `docs/adr/ADR-public-request-handle.md`) added the public `RequestHandle` identity surface — additive `submit_*_request -> Result<RequestHandle>` plus the read-only `request_state` consumer, with non-forgeable construction (negative-compile gate); `tests/request_handle_test.cpp` proves cross-context C2b-row-4b rejection and stale-generation C2c-row-14b safety. Still not a new divergence. `FakeAsyncBackend::close_admission()` mirrors `ThreadPoolBackend::close_admission()` (ADR Decision 15 reference semantics); the B3 context-level detector (`ctx_wait_one_interrupt_final_poll_closes_ready_race`) pins `AsyncIoContext::wait_one`'s interrupted-branch final poll. Phase-gate evidence details: [phase-b-compliance-gate](phase-b-compliance-gate.md), [phase-e-compliance-gate](phase-e-compliance-gate.md), [phase-d-uring-migration-plan](phase-d-uring-migration-plan.md). |
| Revisit trigger | Re-evaluate when benchmarks or backend ABI evidence show caller-owned storage materially reduces per-request overhead and the public API migration cost for Runtime, Batch, and copy pipelines is controlled. |


### Historical evidence / migration record

**Pre-Phase-D (superseded by D1–D4, 2026-08-08):** the following was this
entry's `Current evidence` field before the Phase D reconciliation. Its
"Uring retains pointer/container tracking until Phase D", "Uring stays NOT
CONFORMING", and "`uring_*_not_implemented`" statements describe the pre-Phase-D
state and are retained as the migration record; they are NOT current
architecture state (D1–D4 closed them):

Phase B activates the transitional backend-owned `RequestSlot` arena for the reference backends (FakeAsyncBackend, SyncBackend) only; **Phase E (PR #64) migrated `ThreadPoolBackend` onto the same unified `RequestArena` lifecycle** (see DIV-03/DIV-12 resolutions and `docs/architecture/phase-e-compliance-gate.md`). `UringAsyncBackend` retains its pointer/container tracking until Phase D. The shared `sluice::async::detail::RequestArena` provides one logical capacity per context/backend pair (ADR Decision 2; no two independently oversubscribable stores). PR #63's review closeout moved queue linkage and submission order INTO the slot (`ready_next_`, `submit_seq_`) and removed FakeAsyncBackend's side-band `HandleRing` FIFO and per-kind staging deques; the arena owns a construction-time bounded ready-ring so reap preserves backend-known (terminal-winner) order (ADR Decision 9) for all RequestArena-backed backends — FakeAsyncBackend, SyncBackend, and ThreadPoolBackend. `UringAsyncBackend` is explicitly excluded until Phase D: it retains its pointer/container tracking and stays NOT CONFORMING, so its reap order is not covered by this statement. **C2a (Phase C capacity conformance):** the shared capacity suite (`tests/backend_conformance_test.cpp` `run_capacity_cases`) proves bounded capacity / `would_block` rejection / exact accounting / recycle for the migrated backends (Fake, ThreadPool) through the `make_backend_with_capacity` test seam. Uring has no RequestArena capacity before Phase D, so its capacity coverage is recorded as a `not_implemented` manifest record (`uring_capacity_not_implemented`, `scripts/backend_conformance_manifest.py`), which enters Uring's verdict via `applicable_evidence_for_backend()` — Uring stays NOT CONFORMING and is never skip-as-pass for capacity. See [`docs/architecture/phase-c2a-compliance-gate.md`](phase-c2a-compliance-gate.md). **C2b (Phase C identity / generation / cancel-winner conformance):** the arena-level state-transition matrix, generation/stale-event/provenance contract, and identity-bearing reap order are pinned in `request_lifecycle_scheme_b_test` / `request_arena_test` / `request_arena_death_test`; Fake and ThreadPool integration evidence for cancel-winner and publication-boundary semantics (rows 5–8) is pinned in `backend_scheme_b_race_test` / `threadpool_backend_scheme_b_race_test`. Eight single-point production mutations (A, B1, B2, C, D, E, F, G) prove the cases fail on deliberately nonconforming identity behavior (`docs/verification/phase-c2b-identity-mutation-evidence.md`). Uring's C2b coverage is recorded as a `not_implemented` manifest record (`uring_c2b_identity_not_implemented`), which enters Uring's verdict — Uring stays NOT CONFORMING and is never skip-as-pass for identity. See [`docs/architecture/phase-c2b-compliance-gate.md`](phase-c2b-compliance-gate.md). **C2c (Phase C waiter / borrow / delivery-lease conformance):** the arena-level borrow-lifetime matrix (commit owns → reap releases; survives pending/enqueued/running/backend_ready and every cancel/wait-cancel path), single-waiter registration matrix + no-overwrite cardinality (the matrix is pinned from ADR Decision 10 — registration is orthogonal to execution state and only reap closes it, so running/backend_ready registration is legal and terminal-won-but-unreaped waiters are delivered by reap), waiter-cancel vs I/O-cancel independence, and the move-only lease transfer chains + register-vs-reap / cancel_waiter-vs-reap races are pinned in `request_waiter_borrow_lease_test`; Fake and ThreadPool integration evidence (rows 11–14) is pinned in `backend_c2c_waiter_borrow_test` / `threadpool_backend_c2c_waiter_borrow_test`, including the ThreadPool running and backend_ready-before-reap borrow windows (a worker finishing its syscall is NOT the borrow lifetime end) and waiter registration inside the running/backend_ready windows. Nine single-point production mutations (A–I) prove the cases fail on deliberately nonconforming borrow/waiter/lease behavior (`docs/verification/phase-c2c-waiter-borrow-mutation-evidence.md`). Uring's C2c coverage is recorded as a `not_implemented` manifest record (`uring_c2c_borrow_waiter_not_implemented`), which enters Uring's verdict — Uring stays NOT CONFORMING and is never skip-as-pass for borrow/waiter/lease. See [`docs/architecture/phase-c2c-compliance-gate.md`](phase-c2c-compliance-gate.md). **C2d (Phase C failure-injection / accepted-terminal conformance):** rows 9–10 are pinned on the REAL `ThreadPoolBackend` (`threadpool_backend_c2d_failure_test`, 12 cases) via `SLUICE_ASYNC_INTERNAL_TESTING`-guarded deterministic seams — ADR Gate-4 per-stage pre-commit injection at reserve / prepare / commit-boundary: the injected reserve failure (would_block) leaves the Completion idle with zero residue, the injected prepare failure rolls back the candidate slot (capacity immediately recyclable), and the COMMIT-BOUNDARY arm (the binding CAS wins, then commit is injected to fail) executes the REAL `rollback_binding_before_accept` + slot rollback — the only executable instance of that branch in the corpus — returning the Completion to fully reusable idle; transactional pre-commit rejection (binding-CAS loss → `invalid_state`, zero residue, capacity recyclable), partial worker-startup failure (stop + join + synchronous rethrow — the finding P1-04 regression test), post-commit permanent dispatch failure (injected between enqueue and dispatch push, inside `work_mtx_`, handle never visible to a worker — the ADR Decision-12 winner candidate): submit success, exactly one defined `backend_error` terminal, once-only reap publication, borrow active until reap, no worker/syscall execution (size + void paths), post-commit zero-allocation under always-throw operator new (real worker path and injected path), and the dispatch-failure vs cancel exactly-one-winner invariant. Thirteen single-point production mutations (M1–M13) prove the cases fail on deliberately nonconforming behavior (`docs/verification/phase-c2d-failure-injection-mutation-evidence.md`); the ring-full invariant fail-fast path is untouched. The Fake reference path adds a full-window defined-error no-allocation case (`reference_backend_no_alloc_test`). Uring's C2d coverage is recorded as a `not_implemented` manifest record (`uring_c2d_failure_injection_not_implemented`), which enters Uring's verdict — Uring stays NOT CONFORMING and is never skip-as-pass for failure injection; its own `uring_submit_failure_test` drives the pre-RequestArena SQE model and does not satisfy the C2d contract. See [`docs/architecture/phase-c2d-compliance-gate.md`](phase-c2d-compliance-gate.md). **C2e (Phase C close / drain / destruction conformance):** rows 15–16 are pinned by the shared close/drain suite (`run_close_drain_cases`, driven per backend by `conformance_close_drain_fake` / `conformance_close_drain_threadpool` — close rejects future submit with `invalid_state` leaving the Completion idle and zero residue; accepted-before-close reaches exactly ONE defined terminal with cancel/poll/reap legal after close; drained != releasable (`accepted_outstanding == 0` and Completion ready but `slot_in_use == 1` until the caller resets the ready Completion); slot-release vs admission-close orthogonality) plus the deterministic ThreadPool windows (`threadpool_backend_c2e_close_drain_test`, 18 cases — close while pending/enqueued/running with the real result verbatim, close then pending cancel winner, close then running cancel intent only, one-shot parked-waiter wake with no busy-spin, close ‖ final `record_terminal` in both orderings, the interrupt-vs-final-ready window closed by `wait_one`'s final reap, invariant race drain, submit ‖ close linearization, and the admission-transaction arbitration: close must block on an in-flight acceptance protocol paused between the slot commit (Step 4) and the Step 5 `binding -> outstanding` release-store (the acceptance LP), and a submit paused before the admission lock must reject at reserve after close; plus the review-P1 descriptor-validation precedence cases: post-close malformed submit rejects `invalid_state` (not `invalid_argument`) and capacity-full beats a malformed descriptor), the extended ThreadPool death matrix (incl. the `pending` state) and the new Fake-type death target (`fake_backend_death_test` — the reference path fail-fasts through the arena destructor in Debug AND Release). `FakeAsyncBackend::close_admission()` was added as the reference mirror of `ThreadPoolBackend::close_admission()` (ADR Decision 15 reference semantics). Both backends now hold a backend admission transaction lock across the whole submit acceptance protocol (ADR §"Commit / accept" :453-462 — the winning submit retains its context/admission lock through Step 5, the acceptance LP), so `close_admission()` serializes against an in-flight acceptance protocol and no new LP can occur after it returns (`tp_c2e_close_waits_for_inflight_acceptance_lp` / `tp_c2e_close_wins_submit_started_before_close_rejected` on ThreadPool, `fake_c2e_close_waits_for_inflight_acceptance_lp` on Fake; `close_admission_gates_reserve_not_inflight_prepared_slot` pins the arena boundary — the arena's `commit()` is only the LP's slot half and carries no admission check). The B3 context-level detector (`ctx_wait_one_interrupt_final_poll_closes_ready_race`, test-only split-wait backend) pins `AsyncIoContext::wait_one`'s interrupted-branch final poll. Row 16's pre-existing FULL verdict was re-audited and confirmed, then strengthened. Thirteen single-point production mutation executions across twelve defect classes (M1–M12 incl. the M11-fake driver; 11 RED executions = 10 RED defect classes, M6/M7 documented behavior-neutral defense-in-depth redundancy) prove the cases fail on deliberately nonconforming behavior (`docs/verification/phase-c2e-close-drain-destruction-mutation-evidence.md`). Uring's C2e coverage is recorded as a `not_implemented` manifest record (`uring_c2e_close_drain_not_implemented`), which enters Uring's verdict — Uring stays NOT CONFORMING and is never skip-as-pass for close/drain/destruction. See [`docs/architecture/phase-c2e-compliance-gate.md`](phase-c2e-compliance-gate.md). |

> **Phase D1/D2 reconciliation (2026-08-09):** the pre-Phase-D Uring statements
> in the migration record below are historical. PR #78 completed
> Uring's bounded `RequestArena`, private-ring identity, reap-publication, and
> P0-D recovery migration. Phase D2 adds real-liburing C2d failure/no-allocation
> evidence and reconciles the already-satisfied shared C2a capacity suite.
> Uring remains NOT CONFORMING only because D3's complete C2b/C2c integration
> evidence and D4's C2e wait/close/drain evidence are pending; stub mode is
> still build/API-only. No new Zig divergence or lifecycle authority is added.

> **Phase D3/D4 reconciliation (2026-08-10):** D3 (PR #83, branch
> test/phase-d3-uring-identity-waiter-conformance) closed the C2b/C2c
> integration records with real-liburing evidence and D4 (PR #84, branch
> `feat/phase-d4-uring-wait-close-drain`) implemented the wait source
> (ring-fd poll + control eventfd), `close_admission()` with accept-LP
> serialization, the drained-vs-releasable destruction proof, the shared C2e
> suite for Uring, and the death matrix, then lifted the KernelIo fail-closed
> hard-code only after the complete mandatory real-mode evidence set passed.
> The KernelIo profile is now decided by the ordinary machinery: ELIGIBLE on a
> complete real-mode evidence set, INCOMPLETE on a stub/subset build (per-suite
> KernelIo real-mode attribution; spec §41). The Uring entries that formerly
> entered the verdict as `not_implemented` records (`uring_c2b_identity_*`,
> `uring_c2c_borrow_waiter_*`, `uring_c2e_close_drain_*`) are replaced by
> implemented real-mode evidence. No new Zig divergence or lifecycle authority
> is added.

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
| Current evidence | `include/sluice/async/threadpool_backend.hpp` (ThreadPoolConfig, persistent workers, bounded dispatch ring, RequestArena); `tests/threadpool_backend_reap_test.cpp` (workers_spawned_for_test == worker_count for the backend's whole life); `docs/history/implementation-plans/phase-e-bounded-threadpool-backend.md`. |
| Revisit trigger | None for the per-op model. The naming (ThreadPoolBackend) is retained for continuity; a future rename would be a separate API ADR. |

---

## DIV-04: Decoupled Wake Domains (Backend Does Not Directly Wake Scheduler)

| Field | Value |
|-------|-------|
| ID | DIV-04 |
| Status | Amended (Phase G, 2026-08-15) |
| Introduced by | ADR-execution-model §9.4.1 P3; E9-CORRECTIVE |
| Governing ADR | ADR-execution-model (§9.4.7.2 Phase G amendment) |
| Reason | Zig backend completion directly makes the waiting task runnable via the Io vtable. Sluice decouples: backend publishes to ready queue; Scheduler observes via poll/wait_one/2ms interval. This avoids a lock-ordering hazard (backend mtx → Scheduler global_mtx) and keeps the backend interface minimal. |
| Amendment | The decoupling is PRESERVED, but Phase G narrowed its scope for split-wait production backends (ThreadPool, real io_uring): a Scheduler wake publication (`signal_wake_locked`) now reaches a participant parked in `ctx_.wait_one()` through `backend_wait_active_` -> `interrupt_backend_waiters()` — a control-epoch interrupt on the wait source, invoked after the wake-epoch publication and never under a backend lock. The backend still never calls into the Scheduler and never acquires `global_mtx_`; the upward direction is Scheduler→backend-park interrupt, not backend→Scheduler. Reference poll-driven backends (Fake, Sync/Synthetic) keep the original decoupled shape with the bounded observation interval. |
| Benefit | No upward lock coupling (unchanged, including for the bridge); backend remains a simple leaf; Scheduler retains routing authority; MIXED-WAKE backend latency on production backends is now prompt (no observation interval as authority). |
| Cost | Reference backends retain up to 2ms observation latency in MIXED-WAKE (poll-driven readiness cannot self-notify — intentional). The bridge adds one acquire-load to every Scheduler wake publication when no participant is parked, and a control-epoch bump + notify when one is. |
| Current evidence | ADR-execution-model §9.4.7.2; `scheduler.cpp` `signal_wake_locked` bridge; `docs/architecture/phase-g-compliance-gate.md`; `tests/phase_g_closeout_test.cpp` (Cases A–D); `tests/phase_g_closeout_uring_test.cpp` (UR-G1..G7); `spec/tla/e9_park_wake/` (bridge model). |
| Revisit trigger | A future backend that is neither split-wait nor poll-driven (P5 remains reserved for that case). |

---

## DIV-05: 2ms Bounded Observation Interval as Protocol Authority

| Field | Value |
|-------|-------|
| ID | DIV-05 |
| Status | Amended (Phase G, 2026-08-15) |
| Introduced by | ADR-execution-model §9.4.7.1 E9-CORRECTIVE |
| Governing ADR | ADR-execution-model (§9.4.7.2 Phase G amendment) |
| Reason | In MIXED-WAKE mode (backend outstanding + external-wake-capable wait), the MW-S2 participant parks on the Scheduler domain (wake_cv_) with a 2ms timeout. Backend progress is observed when the timeout expires. This is the protocol authority for backend progress in this mode — not merely "defensive." |
| Amendment | On split-wait production backends (ThreadPool, real io_uring), MIXED-WAKE now parks the participant in the BACKEND domain (`ctx_.wait_one()`) for both wake kinds, with external wakes delivered by the Scheduler interrupt bridge. The interval there is a CONDITION-DRIVEN park cap only — applied when an active deadline (E11 timer pump) or a registered level-triggered ready-flag wait (E5-A2 poll resolution) demands a bounded re-drain; with neither, the park is unbounded and event-driven (no fixed-interval latency or periodic CPU wake). The interval remains PROTOCOL AUTHORITY for reference poll-driven backends (Fake, Sync/Synthetic), which cannot self-notify readiness. |
| Benefit | Single park point for mixed waits is preserved; production path gains prompt backend progress and prompt external wake; no busy-spin (the interrupt is one-shot per invocation). |
| Cost | Reference backends keep the 2ms worst-case latency and periodic wake in MIXED-WAKE (documented reference exemption). |
| Current evidence | ADR-execution-model §9.4.7.2; `scheduler.cpp` MW-S2 park-domain decision + `max_park` derivation (unbounded sentinel by default); `ready_wait_source.hpp` bounded-wait capability; `docs/architecture/phase-g-compliance-gate.md`; closeout causal tests. |
| Revisit trigger | If a reference backend gains a self-notifying wait source; if a workload on a reference backend requires sub-2ms MIXED-WAKE latency (that would be an application-triggered foundation change per the freeze policy). |

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
| Status | Resolved (implemented) |
| Introduced by | ADR-async-io-model §7 X6 (deferred to job 021) |
| Governing ADR | ADR-async-io-model §7 X6 (deferral discharged by implementation) |
| Reason | Zig has `CancelProtection` with protected/unprotected regions and `recancel`. The original divergence claimed Sluice had only a cooperative single-shot `CancelToken` + E10 wait cancellation and no protection regions. |
| Benefit (historical) | (Recorded at acceptance: simpler model, no nested protection complexity.) |
| Cost (historical) | (Recorded at acceptance: could not express "cancel here but not there"; no recancel.) |
| Resolution | Job 021 landed: `include/sluice/async/cancel.hpp:37-147` implements the `CancelProtection` enum, `CancelState::swap_protection` (Zig `swapCancelProtection`), `CancelGuard` RAII (restores prior protection), `CancelToken::rearm` (Zig `recancel`), and `check_cancel`. The public API reference documents the same surface (`docs/reference/api.md:992-1005`), so the registry's old "no protection regions" claim contradicted both code and API. Residual (P-level, not a divergence): no `*Uncancelable` sync-wait twins — see the conformance map `P` row. |
| Current evidence | `include/sluice/async/cancel.hpp` (CancelProtection / CancelState / CancelGuard / CancelToken::rearm / check_cancel); `src/async/cancel.cpp`; `docs/reference/api.md` (CancelToken / CancelState / CancelGuard section); `docs/adr/ADR-cancel-request-epoch.md` (request-epoch representation, 2026-08-13 corrective pass); conformance map (F/F row, audit issue #94). The rearm/recancel semantics were corrected in the 2026-08-13 pass: `rearm()` now re-arms delivery for the shared token's consumers via the request epoch (previously a no-op behind a per-consumer bool); regression tests `cancel_rearm_re_enables_delivery`, `cancel_clear_then_request_is_a_fresh_request`, `cancel_shared_token_two_consumers_deliver_and_rearm`, `cancel_protection_rearm_blocks_until_unprotected_point`, `cancel_future_producer_cancel_points_and_rearm` (tests/cancel_token_test.cpp) fail on the pre-fix code and pass after. |
| Revisit trigger | If `*Uncancelable` wait twins are required by a consumer; that would be a new capability, not a re-open of this divergence. |

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
| Status | Resolved for real syscall backends (ThreadPool Phase E; Uring Phase D); reference-only exemption remains (Fake/Sync) |
| Introduced by | ADR-explicit-io-request-contract (Accepted) Decision 5/6 + Phase B closeout |
| Governing ADR | ADR-explicit-io-request-contract (Decision 6 `invalid_argument` vocabulary) |
| Reason | Decision 6 declares `invalid_argument` for "malformed operation: invalid length/buffer contract, impossible offset conversion, or invalid fd parameter form." The Phase B reference backends (FakeAsyncBackend, SyncBackend) perform NO real I/O — `fd` is a metadata carrier, not a syscall target (the test corpus deliberately uses `ReadOp{-1, ...}` as an "unused by fake" sentinel), and `BorrowMetadata` carries no offset. Enforcing the representable causes (negative fd, null buffer with nonzero length) at the reference `prepare()` would reject reference-backend test traffic without backing a real safety property. |
| Benefit | The reference layer stays focused on the arena/slot/terminal lifecycle contract (its actual scope); the test corpus is not churned to placate a check that guards no real syscall at this layer. |
| Cost | A malformed descriptor is NOT rejected at the reference `prepare()` — it is accepted and surfaces only at the full-backend prepare paths (Phase D/E), where a real syscall would actually dereference the fd/buffer. Callers cannot rely on `invalid_argument` from the reference backends for fd/buffer-form errors until those phases. |
| Current evidence | `include/sluice/async/detail/request_arena.hpp` `prepare()` (no fd/buffer-form check; the deferral is documented at the call site); ADR Phase B closeout "Round-4 review closeout" item 3; reference-backend tests use `ReadOp{-1, ...}` as a documented sentinel. **Phase E closeout (ThreadPool):** the ThreadPoolBackend now enforces the Decision 6 `invalid_argument` causes (negative fd, null buffer with nonzero length, offset beyond `off_t`, length beyond `SSIZE_MAX`) at its own descriptor-validation step before commit (`ThreadPoolBackend::validate_read/write/sync`); a non-negative but closed fd is accepted and later completes with the real `EBADF` terminal (no `fcntl(F_GETFD)` preflight). **Phase D closeout (Uring):** `UringAsyncBackend` enforces the same Decision-6 causes at its Stage 1.5 validation before commit (`uring_backend.cpp:378-433` `validate_*` helpers), and the C2e review cases pin the precedence rules (post-close malformed submit rejects `invalid_state`, not `invalid_argument`; capacity-full beats malformed descriptor). |
| Revisit trigger | **Trigger FIRED (Phase D):** the revisit trigger required Phase D to enforce the Decision-6 `invalid_argument` causes before issuing a real syscall; Uring does so at Stage 1.5. The divergence is fully resolved for every real-syscall backend; the Fake/Sync reference exemption is retained by design (they perform no real syscall and their corpus deliberately uses `ReadOp{-1, ...}` as an unused-fd sentinel). |

---

## DIV-15: FE-2 WaitNode Resume-Target Token Widening (+8 bytes)

| Field | Value |
|-------|-------|
| ID | DIV-15 |
| Status | Accepted |
| Introduced by | FE campaign (FE-2; compliance gate `docs/architecture/fe2-frontend-seam-compliance-gate.md`) |
| Governing ADR | FE-1b frozen contract (`docs/history/reviews/FE-1B-FRONTEND-NEUTRAL-CONTRACT-FREEZE.md`, design authority) |
| Reason | The stackless second frontend cannot pass a continuation through the SAME registration/winner/publication authorities while the epoch token is typed `Fiber*` (FE-1a F1). `WaitResume {void*, Kind}` is the minimal earned representation (FE-1c strategy A). |
| Benefit | One semantic Core serves both frontends; no duplicated admission/terminal/deadline/cancel authority. |
| Cost | +8 bytes per live WaitNode (token field widened from bare `Fiber*` to pointer + kind tag); one kind compare at winner publication tails. Stackful behavior is bit-identical (fiber branch unchanged; `none` preserves the old null-token semantics). |
| Current evidence | `include/sluice/async/wait_node.hpp` (WaitResume), `src/async/scheduler.cpp` (`publish_wait_winner_locked` / defer / take), tests/fe2_stackless_event_pov_test.cpp (Clang Debug 194/194). |
| Revisit trigger | If ActorIdentity separation (FE-3 Mutex/RwLock) lands, re-audit whether the token should carry both roles; if a third frontend emerges, re-rank the type strategy. |

---

## DIV-16: Test-Only Stackless Frontend (No Public Coroutine API)

| Field | Value |
|-------|-------|
| ID | DIV-16 |
| Status | Accepted |
| Introduced by | FE campaign (FE-2) |
| Governing ADR | FE-1b frozen contract; campaign §44 (public API decision deferred) |
| Reason | The second frontend exists as proof-of-value: tiny test-local coroutine task + awaiter + `FeDeferredRecord`, reached only through `Scheduler::AsyncTestAccess` seams compiled into `sluice_async_internal_testing` (AGENTS.md §15 C4 mechanism). |
| Benefit | Proves cross-frontend semantic reuse without spending public-API stability or ABI surface. |
| Cost | The deferred delivery branch (`publish_wait_winner_locked` deferred kind + transit list) is exercised only by internal-testing builds until a production consumer exists; drain-point placement at public resolver seams is deliberately NOT production-wired yet (FE-3+ scope). |
| Current evidence | `src/async/scheduler_fe2_test_seam.cpp` (empty TU in production builds), `src/async/scheduler_test_access.hpp`, xmake/tests/async_internal.lua. |
| Revisit trigger | FE-3 representative slices or any public coroutine-API ADR. |

---

## DIV-17: Deferred-Discharge Eligibility Rule (Resume-Before-Suspend Window)

| Field | Value |
|-------|-------|
| ID | DIV-17 |
| Status | Accepted (contract rule for the staged frontend; v1 discipline stated) |
| Introduced by | FE-4 adversarial review A (finding 2) |
| Governing ADR | FE-1b frozen contract (L2/L7/L9); FE-1c seam design |
| Reason | A deferred delivery obligation becomes visible to any thread the moment the admission CS releases G. A drain driver on another thread could take the record and call `handle.resume()` while the admitting thread is still inside its `bool await_suspend` tail (the coroutine has not finished suspending) — the P2426/Gamarjoba concurrent-resume hazard. |
| Rule (v1) | Discharge is only lawful from the thread that armed the record, AFTER its `await_suspend` tail completed (the test drain helpers discharge on the admitting thread, after `start()`/`join()`). `FeTask` uses `final_suspend{suspend_always}`, so an early resume cannot destroy the frame — the current shape is safe under the stated discipline, not under an arbitrary concurrent drainer. |
| Benefit | Names the hazard now, so the production-frontend slice cannot inherit it silently. |
| Cost | The v1 PoV cannot discharge from a foreign thread; a production frontend must adopt symmetric transfer or a suspend-completed acknowledgment gate before draining concurrently. |
| Current evidence | Adversarial review A (this branch); single-threaded discharge in every FE-2/FE-3 test. |
| Revisit trigger | The production stackless-frontend slice (first concurrent drain driver), or any public coroutine-API ADR. |

---

## DIV-18: Mutex/Semaphore Cancel Tails Migrated; Handoff Owner-Commit Staged

| Field | Value |
|-------|-------|
| ID | DIV-18 |
| Status | Partially resolved (cancel tails migrated to the winner-kind tail; `mutex_handoff_one_locked` owner-commit staged) |
| Introduced by | FE-4 adversarial reviews A+B (converging finding: fiber-only publication tails) |
| Governing ADR | FE-1b frozen contract L8; FE-3 equivalence audit |
| Reason | `mutex_cancel` and `sem_cancel` published via `node.fiber()` + the direct fiber route — for a deferred token that reinterprets the delivery record as a `Fiber*` and strands the continuation. Both are migrated onto `publish_wait_winner_locked` (behavior-equal for the fiber branch). `mutex_handoff_one_locked` still commits `owner = won->fiber()` BEFORE publication: that owner commit is inherently Fiber-typed until the Mutex owner field is re-typed to `ActorId` (the declared Mutex-identity slice), so migrating its publication call alone would NOT close the hazard. |
| Benefit | Cancel/expire delivery of a deferred waiter can no longer corrupt memory on the Mutex/Semaphore paths; the remaining staged hazard is registered instead of latent. |
| Cost | Until the Mutex slice, a deferred epoch MUST NOT reach a Mutex/Semaphore queue (the deferred Condition PoV presents bare queues with an empty bound mutex — enforced by test construction, documented in the seam header). |
| Current evidence | `scheduler_mutex.cpp` / `scheduler_semaphore.cpp` cancel tails (this branch); `mutex_handoff_one_locked` owner commit; audit row #7 (F1-open). |
| Revisit trigger | The Mutex owner-identity slice (re-types `owner`, migrates the handoff commit + publication, and admits deferred Mutex waiters). |

---

## DIV-19: FE-3 AsyncRwLock Writer-Owner ActorId Widening (+8 bytes)

| Field | Value |
|-------|-------|
| ID | DIV-19 |
| Status | Accepted |
| Introduced by | FE campaign (FE-3 RwLock slice; recorded by FE-CORRECTIVE-1 P2 ABI hygiene — the layout delta predates the corrective and was previously unrecorded) |
| Governing ADR | FE-1b A1 (ActorIdentity != ResumeTarget); FE-3 equivalence audit |
| Reason | Writer ownership semantics must compare the holding ACTOR's identity, not the ResumeTarget delivery token (FE-1b A1): `writer_owner_` re-typed `Fiber*` -> `ActorId` so the fiber and stackless frontends share ONE ownership rule (`rwlock_write_admit_locked` recursive check, `rwlock_try_write_admission_locked`, `rwlock_unlock_write_core_locked`, grant-time commit). |
| Benefit | Ownership semantics are frontend-neutral; with FE-CORRECTIVE-1 P1-3 every `writer_owner` read/write — including the recursive-owner decision — is serialized under `global_mtx_` (the pre-corrective fiber entries read it before G: a data race). |
| Cost | `sizeof(AsyncRwLock)` 120 -> 128 bytes, alignment unchanged (8): the owner field widens `Fiber*` (8B) -> `ActorId` (16B: pointer + kind), absorbed with existing padding. Valid-call stackful semantics are preserved; invalid-call behavior changed EARLIER in the FE campaign (recursive blocking write: debug assert -> named fail-fast active in Debug AND Release) — that change is not attributable to the representation. FE-CORRECTIVE-1 itself adds ZERO layout delta (header diffs are a `noexcept` qualifier and a factory-body ternary only). |
| Current evidence | Mechanically measured: BASE `origin/master` 4bee61f probe `sizeof(AsyncRwLock)=120 alignof=8`; FE branch + corrective `sizeof(AsyncRwLock)=128 alignof=8` (same probe, same compiler); `include/sluice/async/async_rwlock.hpp` (`ActorId writer_owner_`), `src/async/scheduler_rwlock.cpp`. |
| Revisit trigger | The Mutex owner-identity slice (the same widening for `AsyncMutex::owner_`); any third frontend that needs a distinct actor representation. |

---

## Summary

| ID | Status | Area |
|----|--------|------|
| DIV-01 | Approved | Context shape |
| DIV-02 | Implemented (Phase B reference; Phase E ThreadPool; Phase D Uring) | Operation storage ownership |
| DIV-03 | Resolved (Phase E) | Backend execution model |
| DIV-04 | Amended (Phase G) | Wake integration |
| DIV-05 | Amended (Phase G) | Observation interval |
| DIV-06 | Approved | Durability ops |
| DIV-07 | Approved | Backend dispatch |
| DIV-08 | Approved | Scope |
| DIV-09 | Accepted | Registered buffers |
| DIV-10 | Accepted | Syscall cancellation |
| DIV-11 | Resolved (implemented) | Cancel protection |
| DIV-12 | Resolved (Phase E) | Resource bounds |
| DIV-13 | Accepted | Backend extension point |
| DIV-14 | Resolved for real syscall backends (ThreadPool + Uring); reference-only exemption remains | prepare() descriptor validation deferred for reference backends |
| DIV-15 | Accepted | FE-2 WaitNode token widening |
| DIV-16 | Accepted | FE-2 test-only stackless frontend |
| DIV-17 | Accepted | FE deferred-discharge eligibility rule (resume-before-suspend window) |
| DIV-18 | Partially resolved | FE Mutex/Semaphore cancel tails migrated; handoff owner-commit staged |
| DIV-19 | Accepted | FE-3 AsyncRwLock writer-owner ActorId widening (+8 bytes) |
