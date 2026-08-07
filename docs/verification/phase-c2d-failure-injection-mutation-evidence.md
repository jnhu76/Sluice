# Phase C2d Failure-Injection / Accepted-Terminal Mutation Evidence

This document records the RED-validity evidence (Issue #68 §13) for the Phase
C2d test suite (rows 9–10: failure injection; accepted-terminal under
allocator failure). Every C2d detector case must be proven able to FAIL on
deliberately nonconforming code — green-only tests are not proof.

Method chosen: **local uncommitted single-point mutation** (the
C2b/C2c-precedented alternative; a test-only nonconforming fixture would
require duplicating `ThreadPoolBackend` internals). Each defect class was
proven by a temporary mutation — of the real production logic in
`src/async/threadpool_backend.cpp` (or `include/sluice/async/fake_backend.hpp`
for M8), and of the guarded seam's `enqueued`-outcome gate for M10
(test-variant logic) — a focused filtered test run, and an immediate restore,
applied by a local one-off harness (snapshot → apply one exact replacement →
build → run exactly the detector case → record RED → restore → re-run GREEN)
kept out of the tree, matching the C2b/C2c precedent (`/tmp/c2c_mutate_review.py`).

All commands ran on the test/phase-c2d-failure-injection-accepted-terminal
branch (master base `8b24ede5b54bc41fde67decb3b820c6385faf125`). Toolchain:
**Clang**, xmake, `xmake f -m debug --toolchain=clang -y`.

## Method

For each mutant M1–M10:

1. apply ONE single-point mutation to the production logic (exact string
   replacement via the harness);
2. rebuild the affected test target (`xmake build <target>`);
3. run exactly the detector case
   (`SLUICE_TEST_FILTER=<case> xmake run <target>`);
4. record the expected case, the actual failing case, the command, and the
   exit code;
5. restore the file from a pre-mutation snapshot of the CURRENT working tree
   (never from git, so uncommitted work is never lost);
6. re-run the case GREEN on the restored tree;
7. after all mutants, confirm no mutation marker remains
   (`grep -r MUTANT src/ include/` → no matches) and `git status --short`
   shows only the intended C2d changes.

Exit-code note: `xmake run` reports 255 when the child fails. Mutants M4/M6/M7/
M8 fail via `std::terminate`/`std::bad_alloc` (the case's own assertion
recorded the violation, then the process aborted through the standard
failed-case mechanism — the destructor fail-fast or the noexcept/allocator
boundary — so the harness reports the child signal). Mutants M2/M5/M9 fail on
the case's own assertion text. Mutants M1/M3/M10 fail on the case's own
message: the detectors collect the terminal facts and reset the ready
Completion BEFORE asserting (post-review-fix shape), so the backend destructor
stays quiescent and the harness prints the case's message — `wait_one lost the
dispatch-failure ready wake` (M1/M3) and `the injection must not fire on a
terminal_noop enqueue` (M10).

## Mutation matrix

| Mutant | Deliberate defect (§13 class) | Mutation applied | Expected failing case | Actual failing case / failure mode | Command | Exit |
| --- | --- | --- | --- | --- | --- | --- |
| M1 | dispatch failure handled by pushing the handle anyway (the terminal is recorded but the handle is ALSO pushed) | `threadpool_backend.cpp` `enqueue_after_commit` (guarded block): `injected_dispatch_failure = true` → `false` | `tp_c2d_dispatch_failure_injection_size_op` | same — the pushed handle makes the worker back off on the already-terminal slot (`mark_running` → false, no syscall), and because the post-lock notify choice took the worker-notify branch, the READY domain is never signalled: the parked waiter never wakes → the case's own `wait_one lost the dispatch-failure ready wake` message (bounded timeout; the failure branch reaps/resets, so the destructor stays quiescent) | `SLUICE_TEST_FILTER=tp_c2d_dispatch_failure_injection_size_op xmake run threadpool_backend_c2d_failure_test` | 255 |
| M2 | wrong terminal error code on the injected path | `threadpool_backend.cpp`: `IoError::Code::backend_error` → `IoError::Code::no_space` in the injection `record_terminal` | `tp_c2d_dispatch_failure_injection_size_op` | same — `c.result().error().code == IoError::Code::backend_error` fails | same command | 255 |
| M3 | ready-domain wake skipped on the injected path (lost wake) | `threadpool_backend.cpp`: the injected path's `signal_ready_progress();` → `(void)0;` | `tp_c2d_dispatch_failure_injection_size_op` | same — `wait_one lost the dispatch-failure ready wake` (bounded timeout; the parked waiter never woke; the failure branch reaps and resets the stored terminal so the case's own message is the observable) | same command | 255 |
| M4 | spawn-failure catch does not join the started workers | `threadpool_backend.cpp` ctor catch: `if (w.joinable()) w.join();` → `(void)w;` | `tp_c2d_partial_worker_startup_failure` | same — `std::terminate` on unwinding the joinable thread vector (process abort) | `SLUICE_TEST_FILTER=tp_c2d_partial_worker_startup_failure xmake run threadpool_backend_c2d_failure_test` | 255 |
| M5 | spawn-failure catch swallows the failure (partial pool accepted silently) | `threadpool_backend.cpp` ctor catch: `throw;` → `return;` | `tp_c2d_partial_worker_startup_failure` | same — `constructor must propagate the injected spawn failure` | same command | 255 |
| M6 | binding-CAS-loss rollback removed (candidate slot leaks) | `threadpool_backend.cpp` `submit_size`: `rollback_reserved_or_prepared(h)` removed from the `begin_binding` failure path | `tp_c2d_cas_loss_rejection_zero_side_effects` | same — `slot_in_use == 2` fails (leaked slot), then the non-quiescent destructor fail-fast aborts | `SLUICE_TEST_FILTER=tp_c2d_cas_loss_rejection_zero_side_effects xmake run threadpool_backend_c2d_failure_test` | 255 |
| M7 | post-commit heap allocation added to the enqueue path (Decision 14 / I9 violation) | `threadpool_backend.cpp` `enqueue_after_commit`: `(void)new char;` after `dispatch_.push_back(h)` | `tp_c2d_real_worker_post_commit_no_allocation` | same — bad_alloc thrown inside the noexcept post-commit path under always-throw aborts the process | `SLUICE_TEST_FILTER=tp_c2d_real_worker_post_commit_no_allocation xmake run threadpool_backend_c2d_failure_test` | 255 |
| M8 | Fake manual terminal path allocates | `fake_backend.hpp`: `(void)new int;` in `complete_oldest_with_error` | `fake_full_window_alloc_failure_defined_error_terminal` | same — bad_alloc under always-throw aborts the process | `SLUICE_TEST_FILTER=fake_full_window_alloc_failure_defined_error_terminal xmake run reference_backend_no_alloc_test` | 255 |
| M9 | worker-spawn injection seam removed (constructor can no longer inject) | `threadpool_backend.cpp` ctor: the guarded spawn-failure check block removed | `tp_c2d_partial_worker_startup_failure` | same — `constructor must propagate the injected spawn failure` | `SLUICE_TEST_FILTER=tp_c2d_partial_worker_startup_failure xmake run threadpool_backend_c2d_failure_test` | 255 |
| M10 | injection fires on a terminal_noop enqueue (the seam is not gated on the `enqueued` outcome) | `threadpool_backend.cpp` `enqueue_after_commit` (guarded block): `if (outcome == detail::EnqueueOutcome::enqueued) {` → `if (true) {` | `tp_c2d_cancel_wins_before_enqueue_injection_armed` | same — `the injection must not fire on a terminal_noop enqueue` (the cancel-won terminal stays canceled and is published; the detector's post-quiescence assertion fails with the case's own message) | `SLUICE_TEST_FILTER=tp_c2d_cancel_wins_before_enqueue_injection_armed xmake run threadpool_backend_c2d_failure_test` | 255 |

## Restore and hygiene

- Every mutant was restored from the pre-mutation snapshot and re-run GREEN
  (exit 0, `ALL TESTS PASSED`) before the next mutant was applied.
- Post-run `grep -r MUTANT src/ include/` → no matches (recursive scan; a
  plain `grep -c` on directory arguments is not a valid scan command).
- `git status --short` shows only the intended C2d files (guarded seams in
  `include/sluice/async/threadpool_backend.hpp` / `src/async/threadpool_backend.cpp`,
  the new test target, the Fake no-alloc case, the manifest/gate records, and
  docs).
