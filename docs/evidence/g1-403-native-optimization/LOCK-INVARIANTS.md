# G1 C9 — Synchronization lock/invariant inventory (r2, post-review)

Status: C9 investigation input, not implementation authorization.
Incorporates the independent reviewer's corrections (verdict
APPROVE_WITH_CORRECTIONS, findings 1–10 and 19 applied). Baseline commit:
`d0673a8b` (BASE_SHA `10aaf58b`). Measured costs come from the corrected #535
evidence (`docs/evidence/403-flamegraph-profiling-20261015` v2) and the
unprivileged PMU campaign (`PMU.md`), both on the same X99 host.

Scope: the lock acquisitions executed for **one 4 KiB read** through the
steady-state pipeline workload (W-02, depth 16, `request_capacity = depth =
16`, bench/bench_w02_pipeline.cpp:108-109,115-117,133), for both production
backends. The W-01 direct path takes no backend locks and is out of scope.

## 1. Mutex inventory (whole program, async paths)

| Lock | Owner of the fact | Protects | Type |
|---|---|---|---|
| `RequestCore::mutex_` | RequestCore | slot phases, free list, terminal ledger, `execution_refs`, `control_refs`, `publication_inflight`, `observer_registrations`, `public_bindings`, `outstanding`, `execution_claimed`, admission, health | authority (REQ-02/04/05) |
| `ThreadPoolBackend::work_mtx_` | ThreadPoolBackend | `dispatch_`, `publication_pending_`, `delivery_[i]`, `active_workers_`, `stopping_` | backend transport + delivery authority |
| `UringAsyncBackend::dispatch_mtx_` | UringAsyncBackend | `dispatch_`, `publication_pending_`, `delivery_[i]`, `router_`, `fatal_error_`, ledger, and the ring SQ (submission runs under it) | backend transport + delivery authority |
| `ProgressSource::mtx_` | ProgressSource | `progress_epoch_`/`progress_exhaustion_`, `control_epoch_`/`control_exhaustion_`, observed/acknowledged control, `health_failed_`, eventfd drain discipline | PROG-02 wake authority |
| `AsyncIoContext::access_mtx_` | AsyncIoContext | `execution_closed_`, stats counters, drive-owner flag | context lifetime authority |
| `work_cv_` (ThreadPoolBackend) | — | worker park on `dispatch_` emptiness | transport wake |

RequestScope deliberately holds **no mutex and no condition variable**: a
scope `Slot` is `{State, std::variant}` (request_scope.hpp:62-66); readiness is
observed by polling `Request::ready()`/`take_result()`, which perform core
`lookup`/`observe`/`consume` under `RequestCore::mutex_`
(request.hpp:86-135); the W-02 bench's ready sink is a no-op
(reference_ready_sink.hpp:10-14). Scope waiting is
`wait_for_slot_` polling readiness and parking via `ctx_.wait_one()`
(request_scope.hpp:146-186).

Most RequestCore public methods take `mutex_` exactly once
(request_core.cpp:35,62,74,145,181,208,218,227,232,238,243,252,270,290,317,
340,361,374,387,405,418,432,456,479,506,527,553); the lock-free exceptions are
`capacity()` (:30) and `context()` (:32).

## 2. Per-request lock steps — ThreadPoolBackend (steady state, ~1 op per driver pass)

MEASURED_COST references are the w4 case `W02-threadpool-4k-d16-w4`
(CPU/op 13.66 µs; mutex machinery 55.2% of all cycles; 6.78 futex syscalls/op;
221,398 futex events/repetition over ~10 hot futex words).

### 2.1 Submit side (driver thread)

| LOCK_STEP | Where | READ_SET | WRITE_SET | PROTECTED_INVARIANT |
|---|---|---|---|---|
| TP-S1 `reserve` | request_core.cpp:145 | free_slots_, slots_, admission_open_ | slot phase free→reserved, `execution_refs`=1 at accept-time layout | REQ-02 admission opens; slot exclusivity |
| TP-S2 record init | threadpool_backend.cpp:257 | delivery_[slot] | kind, event_owed=false | delivery record initialized before any publication |
| TP-S3 `accept` | request_core.cpp:181 | slot=reserved, borrow facts | phase reserved→accepted, outstanding++, `execution_refs`:=1, `RequestKey` | REQ-02/REQ-04 identity publication — THE acceptance linearization point |
| TP-S4 dispatch push | threadpool_backend.cpp:297,319 | dispatch_, stopping_ | dispatch_.push_back | worker sees entry only after accept returned |
| TP-S5 `notify_one` | threadpool_backend.cpp:325 | cv state | wake | PROG-02 analog for the worker pool (outside lock, standard) |

### 2.2 Worker side (one worker thread, per completed op)

| LOCK_STEP | Where | READ_SET | WRITE_SET | PROTECTED_INVARIANT |
|---|---|---|---|---|
| TP-W1 pop+`claim_execution` | threadpool_backend.cpp:387–405; request_core.cpp:361 | dispatch_, stopping_, slot, `execution_claimed` | dispatch_.pop, `execution_claimed`:=true, active_workers_++ | an entry is executed by exactly one worker; claim arbitrates the cancel/pre-dispatch race (`execution_refs` is an accept-time fact, not written here) |
| TP-W2 active dec | threadpool_backend.cpp:429 | active_workers_ | active_workers_-- | quiescence summary for shutdown |
| TP-W3 `offer_terminal` | threadpool_backend.cpp:447 | terminal ledger, slot | terminal chosen, outcome stored | REQ-03 terminal choice is single-winner (physical vs cancel vs error) — independent linearization point |
| TP-W4 `release_execution` | threadpool_backend.cpp:450; request_core.cpp:387 | `execution_refs`, slot | `execution_refs`--, `try_reclaim_` (no reclaim effect while the slot is still unpublished — phase remains `accepted`; `SlotPhase::retired` is generation exhaustion, request_core.cpp:597-600) | REQ-04 borrow retirement — reclaim-predicate input |
| TP-W5 publish push | threadpool_backend.cpp:455–457 | publication_pending_ | push_back | publication ordered after terminal+retirement |
| TP-W6 `signal()` | progress_source.hpp:248–260 | progress_epoch_ | epoch++ (saturating), eventfd write **under mtx_** (progress_source.hpp:418-439) | PROG-02: epoch bump and kernel notification are one atomic step w.r.t. waiter revalidation |

### 2.3 Driver side (per pass; the driver is `wait_one` → `run_progress_pass_` → `poll_progress`)

| LOCK_STEP | Where | READ_SET | WRITE_SET | PROTECTED_INVARIANT |
|---|---|---|---|---|
| TP-D0 `snapshot` | progress_source.hpp:91 | epoch pair | (token) | waiter observes a consistent epoch |
| TP-D1 pass_bound | threadpool_backend.cpp:543 | publication_pending_ | — | pass boundary snapshot |
| TP-D2 pop ×k | threadpool_backend.cpp:553–555 | publication_pending_ | pop_front | publication order preserved; `publish_one` (TP-D3..D5) runs **outside** `work_mtx_`, after the pop's scope closes |
| TP-D3 `begin_publication` | request_core.cpp:456 | publication_inflight, phase | publication_inflight++ | REQ-04: slot pinned while payload read |
| TP-D4 `complete_publication` | request_core.cpp:479 | publication_inflight, public_bindings, phase | inflight--, binding++/reclaim check | REQ-04 publication completion; REQ-05 reclaim becomes possible only here |
| TP-D5 `claim_observer_delivery` | request_core.cpp:527 | observer ledger | registration consumed | exactly-once on_ready |
| TP-D5b sink `on_ready` | reference_ready_sink.hpp:10-14 (bench: no-op) | — | — | no synchronization of its own in this workload |
| TP-D6 delivery scan | threadpool_backend.cpp:561–579 | delivery_[0..15] | event_owed clear | owed-event ownership transfer; in THIS workload (4 KiB reads, not zero-ops) **0 owed events** — the scan is pure overhead here (`acquire_control`/`event_owed` are zero-op-only, threadpool_backend.cpp:330,343) |
| TP-D7 `release_control` | request_core.cpp:506 | control_refs | refs--, try_reclaim_ | zero-op-only step (not exercised by W-02 4 KiB reads) |
| TP-D8 imm-remains | threadpool_backend.cpp:584 | publication_pending_ | — | honest pass report; ordered **after** processing so a push during TP-D3..D5 still reports |
| TP-D9 `occupancy()` | request_core.cpp:34–59 | **all 16 slots**, counters | — | outstanding summary for `accepted_work_remains` |
| TP-D10 `core_->health_failed()` | async_io_context.cpp:399; request_core.cpp:237 | health flag | — | sticky core health composition |
| TP-D11 `observe_pending_control` | progress_source.hpp:42 | control generations | — | PROG-04 control visibility |
| TP-D12–D15 `wait_if_unchanged` ×4 acquisitions | progress_source.hpp:123,154,180,201 | health, control, epoch, fd | eventfd drain; post-poll control check | PROG-02 park protocol: health → revalidate+drain → probe → revalidate → **poll → post-poll revalidate** (:201) |

Per-op acquisition counts at steady state (1 op/pass, non-parking):
`work_mtx_` = 2 (submit) + 3 (worker) + 3 (driver: D1, D2, D8) + 16 scan
iterations (TP-D6) = **24**; `RequestCore::mutex_` = 10 (S1, S3, W1, W3, W4,
D3, D4, D5, D9, D10); `ProgressSource::mtx_` = 5 (D0, D10, D11, TP-W6, plus
`exhausted()` at async_io_context.cpp:416). A **parking** pass replaces D0/D11
with one `wait_if_unchanged` invocation costing 4 acquisitions and adds
`wait_health_failed()` (progress_source.hpp:63) — 9 for the source.

### 2.4 MEASURED_COST per step class (w4, sampled shares of all cycles)

| Step class | Evidence | Cost |
|---|---|---|
| mutex fast+slow path total (all `work_mtx_`+core) | REPORT.md §2.1 | **55.2%** of cycles |
| `occupancy()` slot scan (TP-D9) | perf sample, direct | **1.13%** + its mutex share |
| eventfd notification write (TP-W6) | stack-attributed `write` | **5.06%** |
| futex syscalls | strace census | 6.78/op; 221,398/rep; ~10 hot words |
| pread64 (the payload) | perf sample | 11.79% |
| futex_wait/futex_wake classified as condvar by wrapper | REPORT.md §2.1 | 7.97% + 4.78% (wrapper-based label; with no scope cv and no timed scope park — request_scope.hpp:146-186 — these samples belong to the TP `work_cv_`/mutex family, not a scope mechanism) |

## 3. Per-request lock steps — UringAsyncBackend (same workload)

MEASURED_COST: CPU/op 7.63 µs; 1.00 syscall/op; **60,004 `io_uring_enter` per
repetition, all `to_submit=1`**; enter kernel path 36.14% of cycles.

| LOCK_STEP | Where | READ_SET | WRITE_SET | PROTECTED_INVARIANT |
|---|---|---|---|---|
| UR-S1 `reserve` | uring_backend.cpp:436 | as TP | as TP | REQ-02 |
| UR-S2 record init | uring_backend.cpp:477–483 | delivery_[slot] | kind/event_owed | as TP-S2 |
| UR-S3 `accept` | uring_backend.cpp:497 | as TP | as TP | REQ-02/04 |
| UR-S4 dispatch+prep+**enter** | uring_transport.cpp:43–97 (lock :49; submit :218–235) | dispatch_, ring SQ, fatal_error_, ledger | SQE prep, `io_uring_submit` (to_submit observed =1), controls | SQ contents vs router/ledger vs poison state are one consistent snapshot; `io_uring_enter` runs **under dispatch_mtx_** |
| UR-P1 poll: drain+submit+controls | uring_backend.cpp:588–601 | dispatch_, fatal_error_ | SQE prep, enter | same |
| UR-P2 reap: `handle_one_cqe` **per CQE** | uring_completion.cpp:99–100 | router_, prepared_ops_, ledger | terminal offer, `release_execution`, publication push, per-CQE `release_control` (control_sqes path :120) | router entry vs slot lifetime vs terminal single-winner; lock taken once **per CQE** (batch peek=32) |
| UR-P3 publish pop ×k | uring_backend.cpp:616–619 | publication_pending_ | pop | as TP-D2; `publish_one` outside the lock (:622) |
| UR-P4 begin/complete/claim | uring_backend.cpp:557–581 | as TP-D3..D5 | as TP | as TP |
| UR-P5 delivery scan | uring_backend.cpp:625–643 | delivery_[0..15] | event_owed | as TP-D6; uring **does** carry control refs in this workload (dispatch-time acquire uring_transport.cpp:651, per-CQE release uring_completion.cpp:120), so owed events occur |
| UR-P6 pass tail locks | uring_backend.cpp:289–298 | publication_pending_, fatal_error_ | — | pass report; `outstanding()` for accepted_work_remains |
| UR-W signal | progress_source.hpp:248 | epoch | epoch++, eventfd | PROG-02; rare in uring steady state (kernel CQ → ring eventfd is the wake path) |

Kernel-side: completions are consumed from shared CQ memory; `dispatch_mtx_`
serializes reap against submit/poison, not against the kernel.

## 4. Redundancy findings (C9 Q1/Q2/Q4 evidence)

F1. **Delivery scan = 16 acquisitions of the same mutex per pass for 0 owed
events in this workload** (TP-D6; UR-P5 has real owed events). All iterations
protect one invariant (owed-event ownership) and run sequentially on the
driver thread. Mergeable into one acquisition + collect + process-after-
unlock. No new state, no protocol change; the pass-boundary comment
(threadpool_backend.cpp:546) is preserved because an event set after the scan
instant is deferred by the same epoch-bump signal in both designs, and
`internal_work_retired()` (threadpool_backend.cpp:809-813;
uring_backend.cpp:344-362) still gates settlement on empty owed flags while
`drive_settlement_` loops on it (async_io_context.cpp:435-443,467-519).

F2. **`handle_one_cqe` takes `dispatch_mtx_` per CQE** (uring). Nothing inside
re-acquires the backend lock (retire/find helpers are lock-free,
uring_transport.cpp:540-576); lock order dispatch_mtx_→core mutex_ is uniform;
a cancel cannot win mid-batch against a dispatched op (`execution_claimed` is
set at dispatch, uring_transport.cpp:152, so cancel returns `requested`,
request_core.cpp:260-263). Hoisting to one acquisition per batch preserves the
linearization order on the same thread. Callers: the reap loop
(uring_completion.cpp:222 via :229/:276) and the self-locking test seam
(uring_test_seams.hpp:77-79) — the seam keeps its own locking entry point; the
poison/overflow flush paths (uring_completion.cpp:233,254,269) re-lock
separately and stay outside the hoisted scope.

F3. **`occupancy()` full 16-slot scan per pass** (TP-D9; also
`settlement_converged_`, UR-P6) to read one field (`outstanding`). Directly
sampled at 1.13% before counting its mutex share. C2 experiment target, not a
C9 granularity merge — a counter introduces derived state and needs the C2
mechanism gate.

F4. **`offer_terminal` + `release_execution` are adjacent same-thread
acquisitions** (TP-W3/W4; fused in uring_completion.cpp:70/:89 — the uring
completion path already calls them back-to-back inside one `handle_one_cqe`
critical section, which TP could mirror only via a new combined RequestCore
operation). Protocol-surface change; out of C9's no-protocol-change boundary.
CONDITIONAL; only measured A/B evidence may reopen it.

F5. **`wait_if_unchanged` takes `mtx_` four times per park attempt**
(progress_source.hpp:123,154,180,201). The first two are one critical section
separated only by deadline arithmetic; the post-probe (:180) and post-poll
(:201) revalidations are load-bearing PROG-02 steps. Constraint: the
prerevalidate pause gate (:149-151) sits between acquisitions 1 and 2 and must
stay outside `mtx_` after any merge (test resumers that take `mtx_`, e.g.
`set_progress_epoch_for_test` :310-313, would otherwise deadlock the gate).
Win bounded (one parking pass per idle wait) — LOW priority.

F6. **Publication pop loop takes the backend mutex once per popped entry**
(threadpool_backend.cpp:549-560; uring_backend.cpp:613-624). Same mutex, same
pass-boundary invariant (bound fixed at :543/:609), same thread, sequential.
By the reviewer-confirmed criteria this is the same merge class as F1; TP-D1
(pass_bound) folds into the same acquisition. TP-D8 (immediate_work_remains)
**cannot** fold in: it is ordered after the processing (TP-D3..D5) so a push
during processing still reports — it keeps its own acquisition.

## 5. Verdict

C9 candidate set (all protocol-preserving, no new runtime state):
- C9-G1 delivery-scan single acquisition (both backends) — F1.
- C9-G2 per-batch CQE lock (uring) — F2, with the seam and flush constraints above.
- C9-G3 publication-pop single acquisition within the fixed pass bound, absorbing TP-D1 — F6.
- C9-G4 health+deadline merge in `wait_if_unchanged` (acquisitions 1+2 only; gate stays outside `mtx_`) — F5, low priority.

Out of C9 scope, deferred with reasons:
- occupancy counter → C2 experiment (derived state, needs mechanism gate).
- offer_terminal+release_execution fusion → new core API (protocol surface).
- signal/eventfd batching, submit batching → C1 territory (PROG-02/PROG-03
  protocol analysis and negative oracles required).

No synchronization change is implemented until the reviewer corrections are
applied (done in this revision) and each candidate passes its negative
oracles plus A/B; C9 ends either in measured, gated implementations or in
`C9_INVESTIGATED_NO_SAFE_CHANGE` with the evidence retained.
