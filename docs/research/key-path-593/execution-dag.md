# End-to-end execution DAG

Base: `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`. Line numbers are from that
tree; the anchors used in the tables were re-read in the PR worktree before
publication, the rest are as read during source mapping. This document records
*structure and synchronization*, not cost — cost is in
[mechanism-ledger.md](mechanism-ledger.md).

The rule used throughout: an edge exists only where the code establishes it by
program order, a lock release/acquire pair, a documented kernel primitive, or a
queue handoff whose producer and consumer share a lock. Adjacent timestamps are
never used as evidence of ordering.

## 1. Threads, authorities, lifetimes

| Symbol | Meaning |
|---|---|
| **S** | submitting thread (`RequestScope::submit_read` or `ctx.submit_read`) |
| **P** | progress-owner thread (`wait_one` / `poll_progress`); `P == S` in W-02, `P != S` in W-03 |
| **K** | kernel: the `io_uring_enter` syscall context, the io-wq worker, and the CQ ring producer |

| Authority | Owner | Storage | Non-authoritative mirrors |
|---|---|---|---|
| Terminal choice (exactly once) | `RequestCore` | `slots_[i]` under `RequestCore::mutex_` | `RouterEntry::terminal_delivered` (routing retirement only) |
| Borrow / execution lifetime | `RequestCore` | `execution_refs`, `binding_live` | — |
| Publication boundary | `RequestCore` | `publication_inflight`, `published` | `publication_pending_` queue entry |
| Observer delivery state | `RequestCore` | `observer_phase` (registered/armed/queued/delivering/retired) | `DeliveryRecord::observer_armed`, `event_owed`, `owed_key` |
| Dispatch/admission authority | `RequestCore` | `admission_open_`, `free_slots_`, generation | `AsyncIoContext` `closed_`, backend queue emptiness |
| Backend SQ/CQ mechanism | `UringAsyncBackend` | `dispatch_mtx_` + ring SQ/CQ shared memory | — |
| Kernel transport ledger | `UringAsyncBackend` | `TransportLedger` under `dispatch_mtx_` | `submit_flushes_` (test-only counter) |
| Progress notification epoch | `ProgressSource` | `progress mtx_` + eventfd | `notification_pending_`, `epoch` |
| Acceptance serialization | `AsyncIoContext` | `access_mtx_`, `owner_thread_`, `drive_active_` | — |

The split matters for every elision claim: a mechanism may only be removed if
it performs no transition of an authority it does not own.

## 2. Submit path (thread S)

```
RequestScope::submit_read                              src/async/request_scope.cpp:152
  S1  reserve scope slot (linear scan)                 request_scope.cpp:46-55
  S2  ctx_.submit_read                                 request_scope.cpp:163-167
        AsyncIoContext::submit_read                    src/async/async_io_context.cpp:249
        S3  public precondition (closed + access)      async_io_context.cpp:250-253
        S4  [access_mtx_]                              async_io_context.cpp:255
        S5  backend_->submit_read -> submit_request    async_io_context.cpp:259 -> src/async/uring_backend.cpp:423
              S6  backend validate (zero/range/dst)    uring_backend.cpp:430-437
              S7  [core mutex] RequestCore::reserve     uring_backend.cpp:447 -> src/async/detail/request_core.cpp:165
              S8  prepared_ops_[slot] write             uring_backend.cpp:479-486
              S9  [dispatch_mtx_] delivery_ init        uring_backend.cpp:488-500
              S10 [core mutex] RequestCore::accept      uring_backend.cpp:512-518 -> request_core.cpp:184
              S11 dispatch_after_accept                 src/async/uring_transport.cpp:43
                    S12 dispatch_->push_back            uring_transport.cpp:72
                    S13 dispatch_one_locked loop        uring_transport.cpp:79-85, 130
                          S14 io_uring_get_sqe           uring_transport.cpp:141
                          S15 [core mutex] claim_execution  uring_transport.cpp:152 -> request_core.cpp:371
                          S16 SQE prep + data64          uring_transport.cpp:166-189
                          S17 router install + live_cookies_++ + ledger append  uring_transport.cpp:190-199
                          S18 dispatch_->remove_exact   uring_transport.cpp:201-206
                    S19 submit_transport_locked -> io_uring_submit  uring_transport.cpp:87 -> 218-235
                          S20 account_transport_result_locked           uring_transport.cpp:233 -> 237-279
                    S21 service_pending_controls_locked_            uring_transport.cpp:89 -> 695
                    S22 residue signal (conditional)                uring_transport.cpp:93-101
        S23 Request key -> RequestScope::commit_      async_io_context.cpp:262-266 -> request_scope.cpp:172-175
```

Structural facts established by this DAG:

- **Exactly one kernel submission per accept.** `dispatch_after_accept` drains
  the dispatch queue and flushes inline (`uring_transport.cpp:86-88`) while
  both `access_mtx_` and `dispatch_mtx_` are held. In W-02 steady state one
  accept pushes one entry, so `io_uring_submit` carries exactly one SQE. The
  pass-start flush (`uring_backend.cpp:631`) encounters an empty ledger and
  returns without a syscall. Measured: 1.00005–1.00007 enters/op and
  `sq_ready = 1.00` at every depth.
- **`delivery_` initialization precedes `accept`** (`S9` before `S10`), and
  cannot move: between `accept` returning and `delivery_` being initialized, a
  concurrent `cancel_key` can win `won_before_execution` and push a publication
  (`uring_transport.cpp:589-596`); `publish_one` would then read the *previous*
  generation's `delivery_[slot].kind`. This ordering is a semantic obligation,
  not redundancy.
- **Three distinct core transitions per submit** (`reserve`, `accept`,
  `claim_execution`) exist for three different races: admission, linearization,
  and cancellation arbitration. `claim_execution` losing to a pre-dispatch
  cancel rolls back the SQE tail (`uring_transport.cpp:151-159`).

## 3. Progress pass (thread P)

```
AsyncIoContext::wait_one (W-02) / poll_progress (W-03)     src/async/async_io_context.cpp:561 / 429
  P1  [access_mtx_] drive_entry_admitted_ / drive_exit_    async_io_context.cpp:179-196
  P2  ProgressSource::snapshot [progress mtx_]             include/sluice/async/detail/progress_source.hpp:91
  P3  run_progress_pass_                                   async_io_context.cpp:417
        P4  UringAsyncBackend::poll_progress               src/async/uring_backend.cpp:304
              P5  poll()                                   uring_backend.cpp:616
                    P6  [dispatch_mtx_] drain dispatch_ + pass-start flush + controls   uring_backend.cpp:620-634
                    P7  reap_cqes (no lock at entry)       src/async/uring_completion.cpp:200
                          P8  io_uring_peek_batch_cqe (BATCH=32, shared memory)        uring_completion.cpp:210
                          P9  per CQE: cqe_seen (khead store) + handle_one_cqe          uring_completion.cpp:218-222
                          P10 overflow: io_uring_get_events + reconcile + re-reap      uring_completion.cpp:236-277
                    P11 publication drain (see §4)          uring_backend.cpp:638-657
                    P12 owed gate [dispatch_mtx_]           uring_backend.cpp:658-662
                    P13 owed sweep (only if owed_count_ != 0)  uring_backend.cpp:663-686
              P14 [dispatch_mtx_] ProgressPass facts        uring_backend.cpp:310-322
              P15 has_immediate_physical_work (unlocked ring read)  uring_backend.cpp:285-293
              P16 outstanding() -> [core mutex] outstanding_summary  uring_backend.cpp:706 -> request_core.cpp:93
        P17 [core mutex] health_failed                      async_io_context.cpp:421
        P18 [progress mtx_] wait health + exhaustion        async_io_context.cpp:437-441
  P19 classify -> return, or park via wait_if_unchanged     async_io_context.cpp:596-683
        P20 [progress mtx_] health + epoch revalidation + eventfd drain read
        P21 probe (unlocked ring read)
        P22 [progress mtx_] re-check
        P23 poll(eventfd, deadline)
```

`run_progress_pass_` composes post-pass facts; `poll_progress` reads state in
three lock domains at three instants (backend under `dispatch_mtx_`, CQ ring
unlocked, core under `mutex_`). The offsets are legal because PROG-01 requires
only post-pass facts and PROG-02 (epoch token + eventfd + physical probe)
covers anything that arrives after the summary.

## 4. Completion → publication → observer → take

```
[K] CQE posted to CQ ring shared memory (kernel release; eventfd written when registered)
  |  shared-memory visibility; overflow list when full
  v
P  reap_cqes -> handle_one_cqe                         uring_completion.cpp:200 -> 99
     C1 [dispatch_mtx_]                                 uring_completion.cpp:100
     C2 control-cookie branch (cancel CQE)              uring_completion.cpp:101-144
     C3 find_live_router_cookie_ (reverse scan)         uring_transport.cpp:551
     C4 finalize_operation_terminal_                    uring_completion.cpp:23
          C5 [core mutex] offer_terminal (exactly once) request_core.cpp:349
          C6 [core mutex] release_execution (borrow retire)  request_core.cpp:397
          C7 publication_pending_->push_back (under C1) uring_completion.cpp:92
          C8 retire_router_entry_ (if no control)       uring_completion.cpp:94-96
     C9 [dispatch_mtx_] pass_bound = publication_pending_->size()   uring_backend.cpp:640-643
     C10 per entry [dispatch_mtx_] pop_front             uring_backend.cpp:649-652
     C11 publish_one (outside lock)                      uring_backend.cpp:578
          C12 [core mutex] begin_publication             request_core.cpp:442
          C13 unlock read of delivery_[slot].kind (pinned by publication_inflight)  uring_backend.cpp:589-593
          C14 [core mutex] complete_publication          request_core.cpp:466
          C15 [core mutex] claim_observer_delivery       request_core.cpp:517
          C16 sink.on_ready (outside all locks)          uring_backend.cpp:612
          C17 [core mutex] retire_observer_delivery      request_core.cpp:564
[T] Request::ready / RequestScope::take                  include/sluice/async/request.hpp:130; request_scope.cpp:252
     T1 resolve_ linear scan (scope-private, no lock)    include/sluice/async/request_scope.hpp:114-129
     T2 [core mutex] consume_public_result               request_core.cpp:326
     T3 try_reclaim_ -> release_slot_ (generation++)     request_core.cpp:603-636
```

Why the publication queue cannot be collapsed into the completion phase:

1. `handle_one_cqe` holds `dispatch_mtx_` (`C1`); the publication endpoint
   calls a host hook (`C16`). THREAD-02 forbids host hooks under core/backend
   locks, and a hook may re-enter submit/cancel, which take `dispatch_mtx_`.
2. PROG-01 requires a bounded pass and explicitly permits an *entry snapshot*
   of queued publications; `pass_bound` (`C9`) is that boundary.
3. There are multiple producers (`finalize`, poison, cancel, undispatched
   retirement) and exactly one consumer.
4. The unlocked `delivery_[slot].kind` read (`C13`) is safe because
   `begin_publication` pins the slot (`publication_inflight`); this pinning
   argument is what makes the begin/complete split load-bearing.

What is *not* mandatory in this shape is the per-entry pop lock (`C10`) and the
no-observer claim/retire pair (`C15`, `C17`) — that is exactly the K1 candidate,
and its measured ceiling is the subject of
[k1-completion-publication.md](k1-completion-publication.md).

## 5. Cross-thread edges (W-03 only)

| Edge | From → To | Primitive | Location |
|---|---|---|---|
| X1 | S's `io_uring_submit` (holding `access_mtx_` + `dispatch_mtx_`) ↔ P's pass entry | mutex | `uring_transport.cpp:218-235` vs `async_io_context.cpp:179-191` |
| X2 | S's `dispatch_->push_back` → P's pass-start drain | queue ownership under `dispatch_mtx_` | `uring_transport.cpp:72` vs `uring_backend.cpp:620-634` |
| X3 | K CQE post → P's epoll wake | kernel eventfd registered at `uring_backend.cpp:259-283` | `bench/bench_w03_external_loop.cpp:199-207` |
| X4 | S's `delivery_[slot]` init → P's `publish_one` read | `dispatch_mtx_` release/acquire via the publication queue | `uring_backend.cpp:488-500` → `:589-593` |
| X5 | P's `complete_publication` → S's next `reserve` reusing the slot | `core mutex_` + generation | `request_core.cpp:466-488` → `:165-182` |
| X6 | P's publication → S's `consume_public_result` | `core mutex_` | `request_core.cpp:466` ↔ `:326` |

`X1` is bidirectional in effect: the submitter's syscall blocks the driver's
pass entry, and a long pass drains `dispatch_` under the same lock the
submitter needs. Measured contention is small under normal speed: undistorted
`perf sched timehist` shows ≈0.004–0.005 context switches/op, and futex
tracepoints ≈0.27/op in both arms.

## 6. Cancellation and control

- **Pre-dispatch cancel** (`uring_transport.cpp:584`): `[dispatch_mtx_]`
  `remove_exact` → `[core mutex] cancel` → `release_execution` →
  `publication_pending_->push_back` (under the same lock) → unlock → stats +
  conditional signal. The subsequent publication path is identical to a normal
  completion; this is the second producer of the queue.
- **In-flight cancel** (`issue_running_cancel_locked_`, `uring_transport.cpp:620`):
  `[core mutex] acquire_control` pins the slot (REQ-05), an
  `IORING_OP_ASYNC_CANCEL` SQE is prepared and ledgered. If the SQ is full the
  intent is sticky (`pending_control_count_`) and serviced by
  `service_pending_controls_locked_`.
- **Control CQE** (`uring_completion.cpp:101-144`): `[core mutex]
  release_control`; the router entry must survive the original CQE
  (`retire_router_entry_` only when `terminal_delivered && control_state ==
  none`). Cancellation therefore *adds* core transitions and extends router
  lifetime; it never reduces K1 work.

In every census arm of this campaign the cancellation/control path was idle
(`control_cqe = 0`, control scans 0), so the cancel analysis is structural
(source plus mutation receipts), not load-measured.

## 7. Shutdown, poison and recovery

| Obligation | Mechanism | Location |
|---|---|---|
| Destructor quiescence | `publication_pending_` empty, no owed, no live cookie/control, ledger quiescent, core occupancy zero | `uring_backend.cpp:338-359` |
| Poison recovery | per-entry Class-A retirement, failed dispatch termination, stranded-completion settlement only after CQE visibility proof, borrow retained and reported while overflow cannot be flushed | `uring_transport.cpp:337-524`; `uring_completion.cpp:240-250` |
| Ledger reconciliation | align ledger prefix with `io_uring_sq_ready` on partial submit / retryable error | `uring_transport.cpp:298-335` |
| Settlement drive | `drive_settlement_` pass+park loop with `internal_work_retired()` | `async_io_context.cpp:467-551` |
| No lost wake | epoch token + eventfd + physical probe before/after park | `progress_source.hpp:108-232` |

These are the witnesses every elision arm had to preserve. The K1 arm keeps
`published == reaped` on every W-02 traced pass (70,061 passes; W-03 has
aggregate push/pop parity only), the K4 arms keep the residue advertisement and
10 ms bounded retry, and both keep the destructor quiescence checks green in
the deterministic suite.

## 8. Edge ownership summary

| Edge class | Count per logical op (W-02 4 KiB) | Authority moved | Can it be removed? |
|---|---|---|---|
| Same-thread calls | full fixed chain, every op | none (call structure) | only by deleting work (§ mechanism ledger) |
| `core mutex_` critical sections | 10 fixed + ~2/pass (measurement: 12.4/op at d16) | terminal, lifetime, publication, observer | no; individual *transitions* may be skipped when they change nothing (K1-b) |
| `dispatch_mtx_` critical sections | 2 submit-side + ~2.5 completion/pass (measurement) | backend queues, router, ledger | lock *acquisitions* may be merged (K1-a, K3-a) |
| `access_mtx_` | 1 submit + 2 per public drive call | acceptance / owner exclusivity | no |
| Queue ownership transfers | dispatch 1+1, publication 1+1, ledger 1+1 | accepted-not-dispatched, publication, accounting | no; shape may be batched |
| Kernel submit | exactly 1 `io_uring_enter`, 1 SQE | SQ ownership → kernel | the *frequency* can be amortized (K2-a/K4-b) |
| Kernel completion | 1 CQE, 1 `cqe_seen`, batch peek 1/(CQEs/peek) | CQ head advance | no |
| Progress notification | 0 user signals/op, kernel eventfd aggregated ≈1 per 12 ops (W-03 d16), 0 parks (W-02) | wake obligation | no; cadence is already aggregation |
| Observer publication/take | 1 publication, 1 take; claim/retire only meaningful when armed | OBS-02, REQ-04, HANDLE-01 | claim/retire skipped only when no observer is armed |

The one edge whose *frequency* is avoidable without touching an authority is
the kernel submit edge. That is the campaign's single material finding.
