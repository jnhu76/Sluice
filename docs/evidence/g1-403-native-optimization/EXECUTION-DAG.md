# G1 C9 — Execution DAG for one request, r2 (post-review)

Status: C9 investigation input, not implementation authorization.
Incorporates the independent reviewer's corrections (APPROVE_WITH_CORRECTIONS;
findings 1–10 and 19 applied). Baseline commit: `d0673a8b`. Companion to
LOCK-INVARIANTS.md r2. Threads: **D** = driver/owner thread (`wait_one` loop),
**W** = one ThreadPoolBackend worker, **K** = kernel (syscall context).

Notation: `hb(A,B)` = A happens-before B. Edges are the synchronization-
relevant subgraph; pure computation edges (buffer prep, outcome decode) are
elided.

## 1. TP backend edges

| EDGE_ID | CALLER | CALLEE | THREAD_OWNER | LOCK_OWNER | STATE_READ | STATE_WRITE | LINEARIZATION_POINT | HAPPENS_BEFORE | WAKE_SOURCE | WAIT_PREDICATE | FAULT_PATH | REQUIREMENT_ID | MODEL_CORRESPONDENCE |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| E01 | `submit_read` | `RequestCore::reserve` | D | core `mutex_` (request_core.cpp:145) | free_slots_, admission_open_ | slot:=reserved | slot allocation | hb(E01,E03) | — | — | admission closed → error | REQ-02 | reserve/accept actions |
| E02 | `submit_read` | delivery-record init | D | `work_mtx_` (:257) | delivery_[s] | kind,owed:=false | record init | hb(E02,E09) | — | — | — | REQ-04 | — |
| E03 | `submit_read` | `RequestCore::accept` | D | core `mutex_` (:181) | slot=reserved, borrow facts | phase:=accepted, outstanding++, `execution_refs`:=1, key | **acceptance tx commit** | hb(E03,E04); hb(E03,W1) | — | — | invalid state → rollback | REQ-02, REQ-04 | accept action |
| E04 | `dispatch_after_accept` | push+notify | D | `work_mtx_` (:297; push :319), cv outside | dispatch_ | dispatch_.push | entry visible to workers | hb(E04, W-pop) | `work_cv_.notify_one` | workers: `!dispatch_.empty() ∨ stopping_` | injected dispatch failure → offer_terminal(backend_error)+retire | REQ-03 | dispatch |
| E05 | `worker_loop` | pop+`claim_execution` | W | `work_mtx_` (:387) ∋ core `mutex_` (:361) | dispatch_, stopping_, `execution_claimed` | dispatch_.pop, **`execution_claimed`:=true** (`execution_refs` untouched — accept-time fact), active_workers_++ | **execution claim** (cancel race arbiter) | hb(E05, syscall); hb(E05,E07) | cv wake | as E04 | claim lost (retired) → drop+signal | REQ-04 | claim_execution |
| E06 | `worker_loop` | `run_syscall` (pread64) | W→K | none | prepared op | page cache | physical read | hb(E06,E07) | — | — | errno → failure outcome | REQ-01 (W-01 semantics) | physical io (unmodeled env) |
| E07 | `worker_loop` | `offer_terminal` | W | core `mutex_` (:447) | ledger, terminal state | terminal chosen, outcome stored | **terminal single-winner** | hb(E07,E08) | — | — | not chosen → fail-fast | REQ-03, PROG-04 | offer_terminal |
| E08 | `worker_loop` | `release_execution` | W | core `mutex_` (:450; core :387) | `execution_refs`, slot | `execution_refs`--, `try_reclaim_` (no effect while unpublished — phase stays `accepted`; `SlotPhase::retired` is generation exhaustion, request_core.cpp:597-600) | **borrow retirement** | hb(E08,E10) | — | — | verdict≠retired → fail-fast | REQ-04, REQ-05 | release_execution |
| E09 | `worker_loop` | publish push | W | `work_mtx_` (:455–457) | publication_pending_ | push | publication queued | hb(E09, D-pop) | — | — | — | REQ-04 | — |
| E10 | `worker_loop` | `signal()` | W | `ProgressSource::mtx_` (:248) | epoch | epoch++, eventfd write under mtx_ (:418-439) | **notification ordered with epoch** | hb(E10, D-revalidate); eventfd hb(D-poll return) | eventfd POLLIN | waiter: epoch/token unchanged → poll | write failure → `wait_domain_fail_fast_` (process abort, not sticky health); retired fd → write skipped (:418-420) | PROG-02, PROG-01 | signal + wake |
| E11 | `wait_one` | `snapshot` | D | `ProgressSource::mtx_` (:91) | epoch pair | token | token read | hb(E11,E17) | — | — | — | PROG-02 | observe |
| E12 | `wait_one`→`poll_progress` | pass_bound read | D | `work_mtx_` (:543) | publication_pending_ | — | pass boundary | hb(E12,E13) | — | — | — | REQ-04 | pass bound |
| E13 | `poll_progress` | pop, then `publish_one` **outside** `work_mtx_` | D | `work_mtx_` for the pop only (:553–555); core `mutex_` ×3 for begin/complete/claim (:456,:479,:527) | publication_pending_, slot, bindings | pop; inflight++/--; bindings++; observer claim | **publication completion** (payload read pinned) | hb(E13,E16-scan-order); hb(E13, sink) | — | — | grant≠granted → fail-fast | REQ-04 | begin/complete_publication |
| E14 | `deliver_event` | sink `on_ready` + readiness observation | D | none of its own in this workload: the bench sink is a no-op (reference_ready_sink.hpp:10-14); readiness is observed by `Request::ready()`/`take_result()` → core lookup/observe/consume under core `mutex_` (request.hpp:86-135); no scope mutex/cv exists (request_scope.hpp:62-66,146-186) | core slot state | observer registration consumed | readiness publication via core lookups | hb(E14, bench's `wait_for` polling `ready()`) | none (polled) | bench: `wait_for_slot_` loop on `ready()` + `ctx_.wait_one()` | — | REQ-03 | ready event |
| E15 | `poll_progress` | delivery scan | D | `work_mtx_` ×capacity (:561–579) | delivery_[0..15] | owed cleared (0 iterations effective in this workload) | owed-event transfer | hb(E15, later passes) | — | — | — | REQ-04 | — |
| E16 | `poll_progress` | `release_control` | D | core `mutex_` (:575; core :506) | control_refs | refs--, try_reclaim_ | **control retirement** (zero-op-only; not exercised by W-02 4 KiB reads) | hb(E16, reclaim) | — | — | verdict≠released → fail-fast | REQ-04, REQ-05 | release_control |
| E17 | `wait_one` | `wait_if_unchanged` | D | `ProgressSource::mtx_` ×4 (:123,:154,:180,:201) + poll | health, control, epoch, fd | drained bytes; parked; post-poll control check | **park decision** (health → revalidate∧drain → probe → revalidate → poll → post-poll revalidate) | drain hb(poll); signal hb(poll return) | eventfd POLLIN / deadline | epoch==token ∧ ¬control_pending ∧ probe()==false | poll err≠EINTR → sticky `health_failed_` (:202-209); POLLNVAL → terminate (:212-218) | PROG-02, PROG-03, PROG-04 | wait/park/wake |
| E18 | `run_progress_pass_` | `occupancy()` + `core_->health_failed()` | D | core `mutex_` (:587→:34-59; :399→:237) | all slots, health flag | — | outstanding summary + health composition | hb(E18, wait decision) | — | — | — | REQ-05 summary | — |
| E19 | shutdown | `close_admission`+`drive_settlement_` | D | core `mutex_` | admission, occupancy | admission closed | settlement convergence | hb(E19, destroy) | — | `internal_work_retired` ∧ occupancy all-zero | poison paths | REQ-06, PROG-04 | settle loop |

Reclaim (REQ-05) is attempted from E08 (no effect while unpublished), E13's
`complete_publication`, and E16's `release_control` inside the core lock; the
predicate (`settled ∧ unbound ∧ no refs ∧ not inflight ∧ no observer`) is
evaluated in that same critical section. Model correspondence: the bounded
TLC model of the publication/reclaim protocol (#524 evidence) — actions map
1:1 by name; the probe/POLL paths are environment actions.

## 2. Uring deltas (same numbering scheme)

| EDGE_ID | Delta vs TP | LINEARIZATION_POINT | Notes |
|---|---|---|---|
| U-E04 | push+`dispatch_one_locked`+`io_uring_submit` all under `dispatch_mtx_` (uring_transport.cpp:43–97; submit :218–235) | SQE visible to kernel | observed `to_submit=1`; enter holds the backend lock |
| U-E06 | completion delivered by kernel CQE | kernel completion order | reap via `io_uring_peek_batch_cqe(32)` |
| U-E05+07+08+09 | fused in `handle_one_cqe` under one **per-CQE** `dispatch_mtx_` acquisition (uring_completion.cpp:99–100) | terminal/retire/queue per CQE | batch peek, lock per cqe — C9-G2 target; callers are the reap loop plus the self-locking test seam (uring_test_seams.hpp:77-79); `release_execution` then publication push inside the same section (:89–:92) |
| U-E10 | no worker signal; the driver's eventfd is the ring eventfd (registered); wake = kernel CQ → eventfd | kernel-ordered | PROG-03 external loop: W-03 driver epoll_waits this fd |
| U-E13..E16 | identical core-lock sequence on the driver thread; uring carries real control refs in this workload (dispatch acquire uring_transport.cpp:651, per-CQE release uring_completion.cpp:120) | as TP | `poll()` structure uring_backend.cpp:583–646 |

## 3. The five C9 questions (r2)

**Q1. Which independent lock acquisitions protect the same invariant?**
The 16 delivery-scan iterations per pass (E15/U-E13 tail) protect one
invariant and are same-thread sequential → C9-G1. The per-entry publication
pops within one fixed pass bound (E13 pop, both backends) protect the same
queue/order invariant → C9-G3. The per-CQE acquisitions inside one reap batch
(uring) protect the same router/slot snapshot → C9-G2. The first two
`wait_if_unchanged` acquisitions protect one consistent snapshot → C9-G4.
Conversely, `claim_execution` / `offer_terminal` / `release_execution` are
*independent* linearization points on different invariants and are **not**
"the same lock acquired twice".

**Q2. Which steps already happen sequentially on the same thread?**
The entire delivery scan (E15), the whole reap batch (uring), the publication
pops within one pass bound, E07+E08, and `wait_if_unchanged`'s acquisitions
1+2. Sequentiality alone is necessary but not sufficient — the merge must not
move a linearization point across an interleaving another thread may legally
observe; F1/F2/F5/F6 in LOCK-INVARIANTS.md r2 argue each candidate.

**Q3. Which transitions must keep an independent linearization point?**
acceptance (E03), execution claim (E05), terminal choice (E07), borrow
retirement (E08), publication begin/complete (E13), observer claim (E13′),
control retirement (E16), admission close (E19), epoch bump (E10), park
decision (E17). Each is externally or concurrently observable (RequestKey
publication, cancel races, exactly-once delivery, reclaim eligibility,
PROG-02 losslessness). The DAG's edges are exactly these points.

**Q4. Duplicate queries or duplicate lock acquisitions?**
(a) `occupancy()` full-slot scan per pass for one field — duplicate *query*,
1.13% sampled + mutex share → C2 target. (b) 16× scan acquisitions/pass →
C9-G1. (c) per-entry publication pops → C9-G3 (TP-D1 folds in; TP-D8 keeps
its own acquisition because it is ordered after processing). (d) per-CQE lock
→ C9-G2. (e) E17's acquisitions 1+2 → C9-G4 (gate stays outside `mtx_`).
The pass report's second read of `publication_pending_` (TP-D8) cannot be
absorbed into a processing-time section — a push during processing must still
report `immediate_work_remains`.

**Q5. Can synchronization overhead shrink without new state or protocol
change?**
Yes, four granularity merges (C9-G1..G4) qualify strictly: same lock, same
critical-section order, same linearization points, no new runtime state, no
observable-interleaving removal beyond the pass-boundary window that the
existing epoch-bump signal already makes unobservable. Everything else
promising (occupancy counter, submit batching, signal coalescing, terminal
fusion) requires either derived state (C2 gate) or protocol analysis with
negative oracles (C1 gate) and is therefore out of C9.

## 4. Review and authorization state

- Independent review verdict: **APPROVE_WITH_CORRECTIONS**; corrections 1–10
  and 19 are applied in this r2 revision (verified claims 11–18 retained).
- Required before implementing C9-G1..G4: negative oracles (no-lost-wake,
  owed-event deferred-delivery, settlement-during-pass, C9-G2 batch vs cancel
  seam behavior) identified; existing concurrency and mutation suites as
  regression floor; A/B against the frozen B0 baseline with per-backend
  adjudication.
- Decision rule: measured win ≥5% on the preregistered cases with no tail
  regression >10% and full gates green ⇒ implement as an independent commit;
  otherwise `C9_INVESTIGATED_NO_SAFE_CHANGE` with the A/B evidence retained.
