# M0 Hot-Path Call DAG — Sluice v2 (master `8fdcd99c`)

Scope: the real call DAG of every supported invocation form (W-01 direct,
W-02 bounded scope pipeline, W-03 external loop), covering the 16 lifecycle
points required by the M0 study charter. All paths are traced from source at
BASE `8fdcd99c5f9a42dadfe5aa0c095bf96c756aeb01`; line numbers refer to that
tree. Complexity claims used in the ledger live in
[`m0-complexity-ledger.md`](m0-complexity-ledger.md); this file establishes
*who calls whom, when, and how often*.

Notation: `[N]` marks a step whose cost is a function of a named cardinality
(defined in the ledger). Steps without `[N]` are O(1) scalar work. `(lock)`
marks a mutex acquisition; `(cv)` a condition-variable park; `(sys)` a
syscall; `(scan N)` a full/pass-bounded linear scan.

## 1. Lifecycle coverage matrix

| # | Lifecycle point | W-01 direct | W-02 ThreadPool | W-02 io_uring | W-03 external loop |
|---|---|---|---|---|---|
| 1 | File / Context establishment | `File::open` (`src/file_resource.cpp:68`, `src/blocking_file.cpp`) | `make_threadpool_context` → workers spawn (`threadpool_backend.cpp:93-128`) | `make_uring_context` → ring setup + capability probes (`uring_backend.cpp:147-230`) | same as W-02 |
| 2 | Request admission | n/a | `RequestScope::submit_read` → `AsyncIoContext::submit_read` (`async_io_context.cpp:249`) → `backend_->submit_read` (`threadpool_backend.cpp:211`) | same entry (`uring_backend.cpp:422`) | same as W-02 |
| 3 | Slot allocation | n/a | `RequestCore::reserve` (`request_core.cpp:158`), LIFO `free_slots_` pop, O(1) | same | same |
| 4 | Identity / generation | n/a | `RequestKey{context, slot, generation}` assigned at `accept` (`request_core.cpp:218`); generation bumped at release (`request_core.cpp:619-624`) | same; plus monotonic cookie at dispatch (`uring_transport.cpp:526`) | same |
| 5 | Request lookup | n/a | `Request::ready/take_result` → `RequestCore::lookup/consume_public_result` (`request_core.cpp:257/330`) — O(1) slot+generation resolve | same | same |
| 6 | Dispatch queue | n/a | `dispatch_.push_back` + `work_cv_.notify_one` (`threadpool_backend.cpp:299-332`) | `dispatch_->push_back` then immediate drain loop (`uring_transport.cpp:43-102`) | same as W-02 |
| 7 | Backend submission | n/a | worker dequeue (`threadpool_backend.cpp:385-412`) then blocking syscall (`run_syscall`, 480-543) | `dispatch_one_locked` (SQE prep, `uring_transport.cpp:130-209`) → `io_uring_submit` (218-235) | same as W-02 |
| 8 | Completion routing | n/a | worker: `offer_terminal`+`release_execution` (`threadpool_backend.cpp:443-459`), push `publication_pending_` | CQE: `handle_one_cqe` → `find_live_router_cookie_` `[scan R]` (`uring_completion.cpp:148`, `uring_transport.cpp:551-557`) → `finalize_operation_terminal_` | same as W-02 |
| 9 | Terminal publication | n/a | progress owner: drain calls `publish_one` (`threadpool_backend.cpp:354-373`) → `begin/complete_publication` (`threadpool_backend.cpp:545-585`) | `poll()` publication drain (`uring_backend.cpp:624-635`) | same as W-02 |
| 10 | Observer delivery | n/a | `deliver_event` → `claim_observer_delivery`/sink/`retire_observer_delivery` (`threadpool_backend.cpp:375-383`) | same (`uring_backend.cpp:588-592`) | same; or via `on_ready` routing sink |
| 11 | Result take / discard | n/a | `RequestScope::take` → `resolve_` `[scan S]` → `consume_public_result` (`request_scope.hpp:132-143`) | same | host drains via Request handles |
| 12 | Cancellation | n/a | `cancel_identity` → `dispatch_.remove_exact` `[scan D]` + `core_->cancel` (`threadpool_backend.cpp:761-793`) | `cancel_key`: dispatch `[scan D]`, running-cancel SQE, sticky intent (`uring_transport.cpp:578-664`) | same |
| 13 | Progress polling | n/a | `wait_one` pass loop (`async_io_context.cpp:561-684`) → `poll_progress` (`threadpool_backend.cpp:549-603`, two `delivery_` `[scan C]` sweeps) | `poll_progress` (`uring_backend.cpp:304-327`) → `poll()` (594-657): dispatch drain, submit, controls `[scan R]`, reap (batch 32), publication drain, owed sweep `[scan C with per-slot lock]` | host `poll(2)` on eventfd, then `poll_progress` + `acknowledge_progress_notification` |
| 14 | Retry / recovery | `compose` retry loops (`blocking_file.cpp:286-299`); EINTR (`posix_retry.hpp`) | n/a (blocking syscalls never retryable-fail) | `account_transport_result_locked` EAGAIN/EBUSY reconcile (`uring_transport.cpp:237-279`); poison/Class-A recovery `poison_and_recover_locked` (337-524) `[scan L, scan R]` | same as W-02 uring |
| 15 | Reclamation | n/a | `try_reclaim_`/`release_slot_` under core lock on consume/retire edges (`request_core.cpp:588-626`) | same + `retire_router_entry_` + cookie free-list return (`uring_transport.cpp:559-576`) | same |
| 16 | Shutdown / teardown | `File` close/dtor | `AsyncIoContext::~` → `drive_settlement_` (`async_io_context.cpp:467-551`): `occupancy()` `[scan N]` per convergence check, `collect_outstanding` `[scan N]`, backend stop/join | same + `internal_work_retired` ledger/owed scans, `~UringAsyncBackend` quiescence (`uring_backend.cpp:337-372`) | same |

The Direct form has no core, no slots, no queues: everything is
precheck → syscall → `IoOutcome` under shared File semantics
(`src/blocking_file.cpp`, `include/sluice/detail/file_semantics.hpp`). The
remaining sections trace the async forms.

## 2. W-02 ThreadPool DAG (per request, steady state)

```
submit_read (caller thread)
  RequestScope::reserve_slot_            [scan S: first-free linear scan, scope slots]
  AsyncIoContext::submit_read            (lock access_mtx_)
    ThreadPoolBackend::submit_request
      core_->reserve                     (lock core mutex) O(1) LIFO free-slot pop
      prepared_ops_[slot] = ...          O(1) vector store
      delivery_[slot] re-init            (lock work_mtx_) O(1)
      core_->accept                      (lock core mutex) O(1)
    [Request created; access_mtx_ released]
  dispatch_after_accept                  (lock work_mtx_)
    dispatch_.push_back                 O(1) ring append
    work_cv_.notify_one                 (cv)

worker thread (per completion)
  work_cv_.wait pop dispatch_.pop_front  (lock work_mtx_) O(1)
  core_->claim_execution                 (lock core mutex) O(1)
  run_syscall (pread/pwrite/...)         (sys) — dominant physical cost
  core_->offer_terminal                  (lock core mutex) O(1)
  core_->release_execution               (lock core mutex) O(1)
  publication_pending_.push_back         (lock work_mtx_) O(1)
  signal_ready_progress → eventfd write  (sys) + ProgressSource signal (lock)

driver thread (per wait_one pass)
  run_progress_pass_ → poll_progress
    snapshot pass_bound                  (lock work_mtx_)
    drain publication_pending_ → publish_batch_   [pass-bounded ≤ C]
    owed sweep over delivery_            [scan C] (one lock hold)
    publish_one per entry: begin/complete_publication (lock core mutex ×2) + deliver_event
    deliver owed events                  (lock core mutex per event)
    any_event_owed_locked_               [scan C] (second lock hold)
    core_->outstanding_summary           (lock core mutex)
  progress_->wait_if_unchanged           (lock) + poll(2) on eventfd (sys) when idle
```

Per-op lock acquisitions on the steady path (ThreadPool, one completion per
pass): `work_mtx_` ×~5, core mutex ×~7, `access_mtx_` ×1, progress mutex ×~2,
plus two syscalls for signal/drain and the physical pread. Every collection
step is O(1) except the two per-pass `delivery_` sweeps `[scan C]` (C =
`request_capacity`) and the scope-side scans.

## 3. W-02 io_uring DAG (per request, steady state)

```
submit_read (caller thread)
  RequestScope::reserve_slot_            [scan S]
  AsyncIoContext::submit_read            (lock access_mtx_)
    UringAsyncBackend::submit_request
      core_->reserve / accept            (lock core mutex) O(1) ×2
      prepared_ops_[slot] = ...          O(1)
      delivery_[slot] re-init            (lock dispatch_mtx_) O(1)
    dispatch_after_accept                (lock dispatch_mtx_)
      dispatch_->push_back              O(1)
      drain loop: dispatch_one_locked per entry
        io_uring_get_sqe, prep_read, set_data64(cookie)   O(1)
        router_[router_slot] = {cookie, handle}           O(1)
        cookie_free_list_.pop_back                        O(1)
        transport_ledger_->append                         O(1)
        dispatch_->remove_exact(h)                        [scan D: linear + shift]
      submit_transport_locked → io_uring_submit            (sys)
      service_pending_controls_locked_                     [scan R: full router scan]
  [Request created]

driver thread (per wait_one pass)
  run_progress_pass_ → poll_progress → poll()
    dispatch drain (usually empty)       (lock dispatch_mtx_)
    submit_transport_locked (only if ledger non-empty)      (sys)
    service_pending_controls_locked_     [scan R]
    reap_cqes: peek_batch_cqe ≤32 → handle_one_cqe per CQE
        (lock dispatch_mtx_ per CQE)
        find_live_router_cookie_         [scan R: reverse linear scan per CQE]
        finalize_operation_terminal_: offer_terminal + release_execution
          (core mutex ×2), publication_pending_->push_back
    publication drain                     [pass-bounded] lock per entry
    owed sweep over delivery_             [scan C with one lock/unlock pair per slot index]
  any_event_owed_locked_                  [scan C] (poll_progress)
  dispatch_retry_remains_locked_          O(1) + [scan L] only when ledger non-empty
  progress_->wait_if_unchanged           poll(2)/eventfd
```

Per-CQE routing cost is `[scan R]` under `dispatch_mtx_`; per-pass fixed cost
adds `[scan R]` (controls) + two `[scan C]` sweeps, and the owed sweep takes
the mutex once per slot index (capacity lock/unlock pairs) even when nothing
is owed. All `[scan …]` steps here are bounded by configured capacity
(request_capacity / ring_entries), not by live work.

## 4. Shutdown DAG (both backends)

`~AsyncIoContext` / `shutdown()` → `drive_settlement_`
(`async_io_context.cpp:467-551`):

1. `close_admission` (core lock).
2. On `cancel_then_drain`: `collect_outstanding` `[scan N]` + one
   `cancel_identity` per outstanding request (each enters the backend cancel
   path with its own `[scan D]`/`[scan R]` work).
3. Loop: `snapshot` token → `run_progress_pass_` → `settlement_converged_` =
   `internal_work_retired()` (backend: ring/queue/owed checks; uring includes
   `[scan L]` `all_class_a_recovery_retired` when the ledger is non-empty)
   + `core_->occupancy()` `[scan N]`.
4. `stop_execution` (cv broadcast) → `retire_execution_resources` (join
   workers / ring exit) → `progress_->retire_notification`.

All O(capacity) shutdown scans are in this convergence loop, **not** on the
per-request path. This is the audit's guard against the classic confusion of
recovery/teardown scans with per-completion cost.

## 5. Where O(N) actually appears, summarized

| Mechanism | Cardinality | Frequency | Phase |
|---|---|---|---|
| `RequestScope::reserve_slot_` (`request_scope.cpp:46`) | S = scope capacity | every submit | hot (production) |
| `RequestScope::resolve_` (`request_scope.hpp:114`) | S | every `wait_for` + `take` | hot (production) |
| `BoundedHandleRing::remove_exact` (`threadpool_backend.cpp:64`) | D = live dispatch depth | cancel only | warm |
| `BoundedDispatchQueue::remove_exact` (`uring_internal.hpp:118`) | D | every dispatched op + cancel | hot (uring submit) |
| `find_live_router_cookie_` (`uring_transport.cpp:551`) | R = router capacity | every CQE + every consumed control | hot (uring completion) |
| `service_pending_controls_locked_` (`uring_transport.cpp:666`) | R | every pass (uring) | warm-hot |
| `delivery_` owed sweeps (both backends) | C = request capacity | every pass | warm-hot |
| uring owed sweep per-slot locking (`uring_backend.cpp:636-654`) | C lock pairs | every pass | warm-hot |
| `occupancy()` / `collect_outstanding` / `snapshot()` (`request_core.cpp:59/91/657`) | N = request capacity | settlement/diagnostics only | cold |
| `poison_and_recover_locked` ledger×router scans (`uring_transport.cpp:377-443`) | L×R | submit-failure recovery only | cold |
| Router `for` sweeps in poison (446-520) | R (+CQ visibility L) | test-mutant-gated only | test-only |

W-01 direct execution touches none of these: no container, no lock, no
allocation beyond the caller's own buffer.

Two cold edges complete the lifecycle coverage: W-03 notification
registration/acknowledgement/detach (`async_io_context.cpp:708-736`,
PROG-03) are O(1) scalar+eventfd operations, and observer
cancellation/retirement (`AsyncIoContext::cancel_observer`,
`async_io_context.cpp:774-804` → `RequestCore::cancel_observer`,
`request_core.cpp:541-566`) is an O(1) phase transition.
