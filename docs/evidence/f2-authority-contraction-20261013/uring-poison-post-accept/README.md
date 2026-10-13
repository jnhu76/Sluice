# F2 io_uring poison × post-accept push — correction receipt

Status: **CORRECTED**. Closes `../known-preexisting-gaps.md` §1 (recorded there
as a pre-existing, code-confirmed gap with a suspected field trigger). The
deterministic RED witness reproduced the strand at the branch head
`9f919ae20b864c152386b2839ecc62b68536acc4` before the production change; the
GREEN witness passes after it. Raw logs: `raw/red-unfixed-head.log` (test-only
diff applied, production code at the head) and `raw/green-fixed-worktree.log`.

## Verified execution DAG (before)

Confirmed by inspection of `src/async/uring_backend.cpp`,
`src/async/uring_transport.cpp`, `src/async/detail/request_core.cpp` at the
head, not assumed from the prior review note.

```text
Thread A (submit_request)                     Thread B (any transport user)

fatal_error_ check passes (uring_backend.cpp:425, unlocked)
core_->reserve (:436) -> core_->accept (:493)
  [accepted: execution_refs=1, binding live, unclaimed]
      |                                       non-retryable submit failure
      |                                       account_transport_result_locked
      |                                       -> poison_and_recover_locked
      |                                          (drains the then-empty dispatch_,
      |                                           Class-A retires the ledger,
      |                                           fatal_error_ set, wake signalled)
      |
dispatch_after_accept (transport:43):
  lock dispatch_mtx_
  dispatch_->push_back(h) (:71)
  poisoned_before = true (:73)
  dispatch_one_locked refuses on fatal_error_ (:98)
  loop breaks; submit/service skipped (:81)
  newly_poisoned = false (:85)
  unsubmitted_after_dispatch = false (:88, forced by !fatal_error_)
  no signal_ready_progress (:92)
  unlock

h holds a dispatch-queue seat forever:
  poison_and_recover_locked early-returns (:305)
  service_pending_controls_locked_ early-returns (:634)
  poll()'s dispatch loop is fatal-gated (uring_backend.cpp:585)
internal_work_retired() = false (dispatch_ non-empty, :344-362)
settlement_converged_() = false (async_io_context.cpp:433)
drive_settlement_: both escapes (:495/:499) need !dispatch_retry_remains,
  which stays true (transport:178-183) -> settlement spins
escape: the caller's public cancel wins the pre-dispatch terminal
  (transport:545-579) and removes the queued reservation
```

The strand window is real: the health check at `:425` is unlocked, the
accept-to-push interval holds no backend lock, and every poison write happens
under `dispatch_mtx_` after the pusher's check.

## Kernel-visibility classification

An entry sitting in `dispatch_` while `fatal_error_` is set is **provably
never kernel-visible**: `dispatch_one_locked` refuses before taking any SQE,
and no submit runs after poison. It therefore owes the certain
`failure(fatal_error_)` terminal — the same certainty class as the pre-poison
dispatch drain — never an `uncertain` outcome and never a synthesized success.
Work already staged in the transport ledger keeps its existing reconcile-then-
Class-A proof; work already in the kernel keeps its real CQE path.

## Deterministic witness

`post_poison_accept_push_retires_through_failure_terminal` in
`tests/uring_progress_race_test.cpp` (release, real-liburing profile):

1. An in-kernel blocked pipe read is submitted and really submitted to the
   kernel (hook passthrough).
2. A submitter thread passes the health check, is accepted, and parks at the
   existing `AcceptedPreDispatchPauseGate` seam — after `core_->accept`,
   before the dispatch push. It holds the context admission mutex there, so
   the peer poison is driven through the backend's own transport.
3. The main thread arms the existing submit hook for one non-retryable
   failure (`-EINVAL`), takes a running cancel on the in-kernel read (stages
   a control ledger entry), and runs one backend `poll()`: the real submit
   fails, reconciliation proves the staged entry kernel-invisible, and
   `poison_and_recover_locked` sets `fatal_error_` and retires visible work.
4. The poison transition's wake is drained from the notification fd.
5. The parked submitter is resumed; its `dispatch_after_accept` now pushes
   after poison. Distinguished observations: accepted identity (stranded
   slot/generation), dispatch queue occupancy, terminal publication, the
   retirement wake, the in-kernel read's real outcome, and
   `internal_work_retired()`.
6. A bounded drive loop replaces any hang; the pre-fix escape is the public
   cancel documented in the gap record, used only after the strand is
   captured so the process reports instead of spinning.

No new production seam was added; the scenario composes the existing
pre-dispatch pause gate, the existing submit hook, and the existing cancel
test seam.

## Witnesses

- RED (production at `9f919ae2`, test applied): deterministic FAIL, exit 1 —
  `poisoned=1 drained=0 wake=0 late_terminal=0 inkernel_real=1` — the
  accepted reservation strands (`drained=0`), raises no wake (`wake=0`), and
  cannot reach a terminal through bounded drives (`late_terminal=0`); the
  in-kernel read still reports its real 4-byte outcome (`inkernel_real=1`).
- GREEN (corrected worktree, this slice): deterministic PASS — queue drained
  at the push, retirement wake published, failure terminal
  (`IoError::Code::backend_error`) published exactly once, in-kernel read
  keeps its real outcome, `internal_work_retired()` true, context destructor
  converges. 5/5 stable reruns; full suite 22/22.

## Corrective diff

`src/async/uring_transport.cpp` / `include/sluice/async/uring_backend.hpp`:

- `dispatch_after_accept` now splits on `poisoned_before`: the healthy path is
  unchanged; a push observed under poison goes to the new
  `retire_undispatched_after_poison_locked_()` and raises
  `signal_ready_progress()` through the existing single call site.
- `retire_undispatched_after_poison_locked_()` drains the queue under the same
  lock hold: `claim_execution` arbitrates a racing pre-dispatch public cancel
  (not `claimed` -> the cancel path already owns publication; skip), then
  `offer_terminal(failure(fatal_error_))` -> `chosen`,
  `release_execution` -> `borrow_touch_fully_retired`,
  `publication_pending_` enqueue, `completion_errors` bump — the same
  retirement sequence the pre-poison drain uses.
- `poison_and_recover_locked` is untouched: its drain invariant (an entry in
  `dispatch_` at poison time cannot have been cancelled, because the cancel
  path removes it under the same mutex) still holds and its fail-fasts stay.

Edge preservation: exactly-once terminal selection (claim arbitration; the
core fail-fasts otherwise), effect certainty (provably unsubmitted -> certain
failure; in-kernel -> real CQE), physical/control reference retirement
(`borrow_touch_fully_retired`), no double publication (one queue seat, one
publication enqueue), no slot reuse while referenced (binding live through
publication), no accepted request stranded, no new lock (everything under the
existing `dispatch_mtx_`; RequestCore's own mutex order is unchanged), bounded
queue capacity, shutdown convergence.

## DAG after (the added edge)

```text
dispatch_after_accept (poisoned_before):
  push h -> dispatch_
  retire_undispatched_after_poison_locked_():
    pop h; claim_execution arbitrates cancel race
    offer_terminal(failure) -> chosen
    release_execution -> borrow_touch_fully_retired
    publication_pending_ <- h
  signal_ready_progress()            <-- the previously missing edge
next progress pass: publish_one -> complete_publication -> published
internal_work_retired() -> true; settlement_converged_() -> true;
drive_settlement_ exits through convergence, not through an escape
```

The missing edge was *queue-seat-to-terminal under poison*; every later
mechanism (publication drain, occupancy retirement, settlement convergence)
already existed downstream of it.

## Gates

Re-collected at the final implementation head in the closeout matrix
(`../gates.json`); see the PR closeout receipt. Immediate post-fix checks at
the corrective worktree: `uring_progress_race_test` 22/22 (5/5 reruns),
`uring_external_loop_test`, `uring_registration_lifecycle_test`,
`shutdown_lifecycle_test`, `uring_core_cutover_test`,
`backend_conformance_e1_uring_test` all PASS (release + real liburing).
