# MW-HW-1 source-precise execution DAG (master `8e2aa5ab`)

Reconstructed from source after the H2b merge (PR #565). Line numbers refer to
that tree. Notation: `[scan N]` full/pass-bounded linear scan over cardinality
N; `(lock)` mutex acquisition; `(core lock)` the RequestCore mutex;
`(sys)` syscall; `(enter)` `io_uring_enter`; C = `request_capacity`
(request slots, router entries, delivery records), D = live dispatch depth,
R = router capacity (= C), L = transport-ledger length, S = scope capacity,
K = owed events, P = pending cancel controls.

Cardinality note verified this round: `router_.size() == capacity_` (both sized
`request_capacity`), so every router scan is `[scan C]`.

## 1. W-02 io_uring steady state (primary object)

Per request at D=1 (sliding window), userspace path in execution order:

```
RequestScope::submit_read                       (request_scope.cpp:158)
  reserve_slot_          [scan S] (S=1 at D=1; first-free scan)
  AsyncIoContext::submit_read                   (async_io_context.cpp:249)
    initiation_rejection precheck               O(1)
    (lock access_mtx_)                          1 acquisition
    tally/stats + outstanding probe             (core lock ×1 via outstanding_summary)
    UringAsyncBackend::submit_request           (uring_backend.cpp:424)
      validate_op (precheck_data_op/state_op)   O(1)
      core_->reserve            (core lock)     O(1) LIFO free-slot pop
      prepared_ops_[slot] store                 O(1)
      delivery_[slot] re-init   (lock dispatch_mtx_)  #1
      core_->accept             (core lock)     O(1)
      dispatch_after_accept     (lock dispatch_mtx_)  #2, whole section:
        dispatch_->push_back                    O(1) ring append
        drain loop (front + dispatch_one_locked per entry)
          io_uring_get_sqe                      O(1) mmap ring write (NOT a syscall)
          core_->claim_execution (core lock)
          allocate_cookie_                      O(1) monotonic
          io_uring_prep_read/write/...          ~10 stores into SQE
          router_[router_slot] = {...}          O(1); LIFO cookie free list
          transport_ledger_->append             O(1) + 4 invariant checks
          dispatch_->remove_exact [scan D + shift]  (D=1: ~O(1))
        submit_transport_locked → io_uring_submit   (enter) #1/op
        service_pending_controls_locked_        [scan C] FULL router scan   ← H-K1
        dispatch_retry_remains_locked_          O(1) (ledger empty short-circuit)
  [submit path locks: access ×1, dispatch ×2, core ×3; syscalls: 1 enter]

RequestScope::wait_for → wait_for_slot_          (request_scope.hpp:146)
  request.ready() → core_->lookup (lock access_mtx_ + core lock ×1)   [miss]
  AsyncIoContext::wait_one                      (async_io_context.cpp:561)
    (lock access_mtx_) ×3 (closed check, stats, drive_entry_admitted_)
    progress_->snapshot()       (progress mtx)
    run_progress_pass_ → poll_progress          (uring_backend.cpp:304)
      poll()                                    (uring_backend.cpp:606)
        (lock dispatch_mtx_) drain dispatch_ (empty in steady state)
        submit_transport_locked (ledger empty → no enter)
        service_pending_controls_locked_        [scan C] FULL router scan   ← H-K1
        reap_cqes()                             (uring_completion.cpp:200)
          io_uring_peek_batch_cqe ≤32           mmap ring read (NOT a syscall)
          per CQE: (lock dispatch_mtx_)
            find_live_router_cookie_            [scan C worst case, reverse;
                                                  ~1 probe at D=1 (LIFO reuse
                                                  clusters live entries at top)]
            finalize_operation_terminal_
              core_->offer_terminal (core lock)
              core_->release_execution (core lock)
              publication_pending_->push_back
              retire_router_entry_              O(1) + free-list push
        (lock) pass_bound = publication_pending_->size()
        per entry: (lock) pop_front → publish_one
          core_->begin_publication (core lock)
          core_->complete_publication (core lock)
          deliver_event: claim_observer_delivery (core lock)
                         sink on_ready callback
                         retire_observer_delivery (core lock)
        (lock) owed_present = owed_count_ != 0   O(1) since H2b; sweep only when K>0
      (lock dispatch_mtx_) immediate_work_remains (owed_visible_locked_ = O(1) count)
      has_immediate_physical_work()             O(1) CQ ring reads
      core_->outstanding_summary (core lock); core_->health_failed (core lock)
      progress_->exhausted (progress mtx)
  request.ready() (core lookup)                 [hit]
RequestScope::take → resolve_ [scan S] → consume_public_result (core lock)
  [pass path locks/op (1.49 passes avg): dispatch ×~7, core ×~9, progress ×~2,
   access ×~4; syscalls: 0 in the inline-completion regime]
```

Measured per-op counts (timed phase, C=1024, D=1, warm tmpfs):
progress passes/op ≈ 1.49 (harness stats `poll_calls/ops`), submissions/op = 1,
`io_uring_enter`/op = 1.00 (census), core-mutex acquisitions/op ≈ 12,
dispatch-mutex acquisitions/op ≈ 9, access-mutex ≈ 5, progress-mutex ≈ 2-3.
All mutex pairs uncontended in this shape (whole-census futex count = 2).

Key structural facts established this round:

1. **The router scans dominate the C-dependent userspace cost at low depth**
   (`service_pending_controls_locked_` at submit AND at each pass; the
   measured 2.48 scans/op at 4k/D1/C1024 is 1 + poll_calls/op — work-ledger
   W1). RouterEntry ≈ 24 B, so the code inspects ≈ 2.48·C·24 B ≈ 59.5 KiB of
   router entries per op at C=1024 — a logical traversal range, not an
   equal amount of DRAM traffic (at D=1 live entries cluster near the top).
2. `io_uring_get_sqe`/`peek_cqe` are mmap-ring userspace helpers, not
   syscalls; the ONLY steady-state syscall is one `io_uring_submit`
   (`io_uring_enter`) per op.
3. The park path (`poll(2)` on the eventfd, `drain_notification_nolock_`
   reads) is cold in the warm-tmpfs W-02 shape: completions land before or
   during the first pass; the probe (`has_immediate_physical_work`) catches
   them without parking. Census: 6 `read` and 0 `poll` total for 80k ops.
4. H2b's `owed_count_` gate is present at both `poll()` and
   `poll_progress()` (`owed_visible_locked_`, uring_backend.hpp:251).

## 2. W-02 ThreadPool steady state (contrast backend)

```
submit: scope reserve [scan S] → access lock → core reserve/accept (core ×2)
  dispatch_after_accept (work_mtx_): dispatch_.push_back + work_cv_.notify_one (cv)
worker thread: work_cv_.wait → pop (work_mtx_) → claim_execution (core)
  → run_syscall → pread64/pwrite64/... (sys) ← the physical I/O
  → offer_terminal + release_execution (core ×2)
  → publication_pending_.push_back (work_mtx_)
  → signal_ready_progress → eventfd write (sys, deduped by notification_pending_)
driver pass (poll_progress, threadpool_backend.cpp:549):
  (work_mtx_) ONE hold: drain publication_pending_ into publish_batch_
    + owed sweep [scan C] into owed_batch_ (single-lock snapshot shape)
  publish_one per entry (core ×2) + deliver_event (core ×2)
  owed deliver + release_control (core) per event
  (work_mtx_) any_event_owed_locked_ [scan C]   ← ThreadPool did NOT get H2b
  outstanding_summary (core)
```

Census (4 KiB, D=64, 4 workers, 20k ops): `pread64` 20,004 (1.00/op, the
physical read), **futex 46,246 (2.31/op, 79% of traced time)** — the
worker-handoff condvar ping-pong — plus eventfd writes and 2,116 `poll`.
ThreadPool pays ~3.4 syscalls/op and a condvar wake chain per op; io_uring
pays 1 enter/op. ThreadPool's per-pass owed scan is still `[scan C]` ×2 with
no derived count (H2b was uring-only productization).

## 3. W-01 direct/blocking (contrast form)

`File::open` → per call: precheck (`file_semantics.hpp`) → single
`pread`/`pwrite` (EINTR-retried, posix_retry.hpp) → `IoOutcome`. No core, no
slots, no queues, no locks, no allocation. The semantic floor reference: 1
syscall per operation plus the caller's buffer.

## 4. Cancel, retry, recovery, teardown (cold/warm edges)

- Cancel pre-dispatch: `cancel_key` (uring_transport.cpp:578): dispatch
  `remove_exact [scan D]` + core cancel + publication push + signal. O(D).
- Cancel running: `issue_running_cancel_locked_` (614): router find
  `[scan C worst]`, cancel64 SQE prep, control ref acquire, ledger append.
  SQE exhaustion ⇒ `control_pending` sticky intent — the ONLY producer of a
  nonzero pending-control count. Serviced by `service_pending_controls_locked_`
  scans (the H-K1 mechanism's reason to exist).
- Submit retry: EAGAIN/EBUSY ⇒ ledger reconcile `[scan L]` + 10 ms retry
  cadence (kTransportRetryInterval). Cold in practice.
- Poison/Class-A recovery: `poison_and_recover_locked` `[scan L × C]`. Cold.
- Overflow flush: `io_uring_get_events` (enter) only when
  `io_uring_cq_has_overflow`. Not observed in this round's census (0 extra
  enters beyond 1.0/op).
- Shutdown/settlement: occupancy `[scan N]` per convergence check +
  `internal_work_retired` authoritative scans. Cold, per-context.

## 5. Zero-op (len=0) path

`submit_request` with zero_op bypasses `dispatch_after_accept` entirely:
`publish_zero_op_inline` (acquire_control + begin/complete_publication, core
×3, dispatch lock ×1) sets `event_owed` + `owed_count_`, signals. The next
pass sweeps and delivers. Source-derived: this path never touches ring
state, so it performs no `io_uring_enter`; the eventfd write is deduped by
`notification_pending_` until a drain runs. **No syscall census backs this**
— the shared harness rejects `--block=0`, so the attempted zero-op trace
captured only process startup (per review B F5); coverage is by the owed
suites (SMOKE/parity/residue/external-loop), not by a census.
