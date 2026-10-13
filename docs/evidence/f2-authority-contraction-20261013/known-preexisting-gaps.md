# Known pre-existing gaps disclosed by the F2 review

These are not introduced by this slice and are not fixed here: each is
byte-identical at the F2 baseline `0ed16588b7101f108aa7282074ba3d568f9d34e9`,
and fixing one would be a separate protocol change with its own oracle. They are
recorded so the F2 contraction is not credited with, or blamed for, behaviour it
did not change, and so the owner sees them before the installed break merges.

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
trigger is suspected.

## 2. `AsyncIoContext::retire_delivery` has no null-core guard

`src/async/async_io_context.cpp:817-820` dereferences `core_` while the two
sibling entry points this slice rewrote (`attach_observer` `:788-796`,
`cancel_observer` `:802-810`) both check it. Unchanged from baseline and
reachable only through a moved-from or destroyed context, which is a documented
caller misuse; recorded so the asymmetry is deliberate rather than accidental.
