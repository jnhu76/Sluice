# Known pre-existing gaps disclosed by the F2 review

These are not introduced by this slice: each is byte-identical at the F2
baseline `0ed16588b7101f108aa7282074ba3d568f9d34e9`. Originally recorded so
the F2 contraction is not credited with, or blamed for, behaviour it did not
change; §1 and §3 were subsequently fixed by the F2 final corrective round
(the owner prioritized correcting the real concurrency/lifetime defects over
carrying them), and §2 by the same round as a residual finding. Each fix has
its own receipt with deterministic witnesses:

1. **CORRECTED** — `../uring-poison-post-accept/README.md`.
2. **CORRECTED** — `../retire-delivery-moved-from/README.md`.
3. **CORRECTED** — `../e1-gate-lifetime/README.md`.

## 1. io_uring poison racing the post-accept ring push can strand a request

Found by the production-code review. Window: `src/async/uring_backend.cpp`
checks `fatal_error_` before `core_->reserve()`/`accept()` (around
`:425-427`), then pushes the accepted reservation through
`dispatch_after_accept(h)` (`:506`) with no backend lock held across the
accept-to-push interval. `src/async/uring_transport.cpp` pushes the entry
unconditionally (`:71`), reads `poisoned_before` at `:73`, returns early from
`dispatch_one_locked` when `fatal_error_` is set (`:98-99`), skips submission
(`:81`) and forces `unsubmitted_after_dispatch` false while poisoned (`:88-89`),
so no progress signal is raised for the stranded entry. Every rescue path is
closed afterwards: `poison_and_recover_locked` returns early once poisoned
(`:305-307`) and therefore never drains a late `dispatch_` entry, and
`service_pending_controls_locked_` does the same (`:634-635`). The settlement
driver then spins: `internal_work_retired()` stays false
(`src/async/uring_backend.cpp:344-362`), `settlement_converged_()` stays false
(`src/async/async_io_context.cpp:433-441`), and both escapes in
`drive_settlement_` are gated on `!pass.dispatch_retry_remains`, which stays
true (`src/async/uring_transport.cpp:178-183`).

Consequence: `shutdown()` and the `noexcept` destructor spin instead of
converging, unless the caller cancels the stranded identity, which does remove
it and win the pre-dispatch terminal
(`src/async/uring_transport.cpp:545-579`).

Baseline equivalence: `git show 0ed16588:src/async/uring_transport.cpp` carries
the identical `poisoned_before` / `newly_poisoned` / `unsubmitted_after_dispatch`
structure and the same absence of a post-poison drain. Not introduced by F2.
The trigger (a non-retryable `io_uring_submit` failure racing a submission) was
not reproduced with fault injection; the code path is confirmed, the field
trigger is suspected. The corrective round later reproduced it deterministically
with the existing seams and fixed it; see `../uring-poison-post-accept/README.md`.

## 2. `AsyncIoContext::retire_delivery` has no null-core guard

`src/async/async_io_context.cpp:817-820` dereferences `core_` while the two
sibling entry points this slice rewrote (`attach_observer` `:788-796`,
`cancel_observer` `:802-810`) both check it. Unchanged from baseline and
reachable only through a moved-from or destroyed context, which is a documented
caller misuse; recorded so the asymmetry is deliberate rather than accidental.
The corrective round reclassified it — no documented precondition exists, the
siblings define the moved-from contract as benign rejection, and a Scheduler
reference can reach the call — and added the guard with a deterministic
regression oracle; see `../retire-delivery-moved-from/README.md`.

## 3. E1 conformance `GateGuard` teardown race flakes under ASan

`tests/backend_conformance_e1_test.cpp`'s `GateGuard` destructor stores
`resume` and notifies, but never waits for the paused worker's `exited`
acknowledgement. A worker that loaded the gate pointer before the scenario's
`set_worker_unclaimed_retirement_pause_gate(nullptr)` can reach the gate
object after the scenario returned and the stack gate died — ASan reports a
`stack-use-after-scope` read inside `ThreadPoolBackend::wait_unclaimed_retirement_pause_`
(`src/async/threadpool_backend.cpp:652`). Observed intermittently in
`backend_conformance_e1_threadpool_test`'s
`public_cancel_removing_last_stale_entry_owes_a_wake` scenario:
2/12 failures at the corrected head `25734314` and 4/12 at the captured
pre-fix head `996f434f` (identical binary re-runs, load-conditional), so it
predates the W-03 closure and is not caused by it. The F2 slice's own
execution record already notes an ASan stack-use-after-scope defect this
gate family introduced and fixed once; this residual instance needs the
guard to wait for `exited` before the gate may die. Found during the W-03
closure matrix re-run (`w03-root-cause.md`). The corrective round fixed the
guard (exit acknowledgment wait) and the gate storage ordering (worker-side
gates outlive the context), with a deterministic witness and clean ASan
campaigns; see `../e1-gate-lifetime/README.md`.
