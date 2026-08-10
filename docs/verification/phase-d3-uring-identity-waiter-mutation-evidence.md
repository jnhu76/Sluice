# Phase D3 — Uring C2b/C2c Integration Mutation Evidence

Date: 2026-08-09 (initial); updated 2026-08-10 for the PR #83 review repair
(R1/R3 evidence-metadata honesty, R2 cancel-control-authority kernel
portability — D3-M6 re-confirmed). Branch:
test/phase-d3-uring-identity-waiter-conformance.
Baseline SHA: `126612a` (master, PR #80 merge). Kernel: 6.18.33.2-microsoft-standard-WSL2.
liburing: 2.14 (pinned). Build: Clang Debug with `--with-liburing` (real mode).

Every mutant below was applied as a TEMPORARY production/arena edit, the
focused evidence case was rebuilt and run (expect RED), the edit was reverted
byte-for-byte, and the same case was rebuilt and run again (expect GREEN). All
eleven mutants were executed on the final D3 head; all RED→GREEN transitions
were confirmed by the actual binary exit codes (the evidence targets fail
non-zero on any `SLUICE_CHECK` violation or fail-fast). The R2 repair renamed
the control-before-original detector to
`uring_c2b_cancel_control_never_authors_terminal` (kernel-portable); D3-M6 was
re-confirmed RED→GREEN against the renamed corpus on the repaired head.

Command shapes:

```sh
# RED run (mutant applied):
xmake build <target> && SLUICE_TEST_FILTER=<case> xmake run <target>
#   observed: non-zero exit (FAILED checks or fail-fast terminate)

# GREEN run (mutant reverted):
xmake build <target> && SLUICE_TEST_FILTER=<case> xmake run <target>
#   observed: exit 0, "ALL TESTS PASSED"
```

## D3-M1 — route CQE by raw SlotIndex instead of full cookie/handle

- Mutated: `src/async/uring_backend.cpp`, operation-CQE branch of
  `handle_one_cqe` — cookie-keyed `find_live_router_cookie_(user_data)` replaced
  by `router_[user_data - 1]` (the pre-P0-B ABA encoding).
- Detector: `uring_c2b_stale_cookie_cqe_dropped`
  (`uring_backend_c2b_identity_test`).
- RED: stale cookie 1 resolves to a recycled/free router array slot; the arena
  rejects the fabricated handle and the identity invariant fail-fasts
  (`terminate called without an active exception`, rc=255).
- GREEN after revert: stale cookie dropped, B untouched, B terminal intact.

## D3-M2 — allow stale SlotHandle cancel to hit the new occupant

- Mutated: `include/sluice/async/detail/request_arena.hpp`, `RequestArena::cancel`
  — generation validation replaced by a slot-index-only lookup.
- Detector: `uring_c2b_stale_slothandle_cancel_harmless`.
- RED: the captured generation-N handle resolves to the N+1 occupant; the
  cancel mutates B (terminal stored) and the generation-reuse detector fails.
- GREEN after revert: `not_found`, zero side effect, B completes normally.

## D3-M3 — pending cancel does not disarm enqueue

- Mutated: `src/async/uring_backend.cpp`, `enqueue_after_commit` — the
  `enqueued` vs `terminal_noop` outcome discriminator removed (unconditional
  dispatch push + drain).
- Detector: `uring_c2b_pending_cancel_wins_no_sqe`.
- RED: the canceled request is pushed onto the dispatch ring after the cancel
  won; the drain's `mark_running` sees the already-terminal slot and fail-fasts
  (rc=255). No SQE was ever legitimately installable.
- GREEN after revert: `terminal_noop`, no linkage, canceled terminal reaped once.

## D3-M4 — enqueued cancel does not remove the dispatch linkage

- Mutated: `src/async/uring_backend.cpp`, production cancel core `cancel_handle_`
  — `dispatch_->remove_exact(handle)` removed.
- Detector: `uring_c2b_enqueued_cancel_wins_no_sqe`.
- RED: the cancel-won request stays on the dispatch ring; the resumed drain
  attempts SQE installation on the terminal slot and fail-fasts (rc=255).
- GREEN after revert: linkage removed FIRST, no future SQE, canceled terminal once.

## D3-M5 — running cancel locally terminalizes / releases the slot

- Mutated: `src/async/uring_backend.cpp`, `cancel_handle_` intent branch —
  a local `record_terminal(canceled)` added before the best-effort AsyncCancel.
- Detector: `uring_c2b_running_cancel_intent_real_result`.
- RED: the slot is terminalized while the original operation CQE is still
  kernel-owned; the later original CQE hits the double-terminal invariant and
  fail-fasts (rc=255).
- GREEN after revert: intent only; the original CQE decides (0-byte EOF), no
  canceled tally.

## D3-M6 — cancel CQE chooses / overwrites the terminal

- Mutated: `src/async/uring_backend.cpp`, `handle_one_cqe` control branch — the
  deferred ORIGINAL terminal replaced by a fabricated `canceled` terminal when
  the control CQE retires.
- Detector: `uring_c2b_original_cqe_before_control_cqe` (order B: original CQE
  first, control second). Also covered by the kernel-portable
  `uring_c2b_cancel_control_never_authors_terminal` detector's Path B (control
  retires, original verbatim) on a kernel where the cancel is ineffective.
- RED: the control CQE overwrites the deferred 0-byte original result with a
  fabricated canceled terminal; the verbatim-result detector fails
  (`FAILED 1 check(s): res.has_value()`, rc=1).
- GREEN after revert: original result verbatim (0 bytes), control CQE
  informational only. Re-confirmed on the R2 (kernel-portable) D3 head: mutant
  RED (`res.has_value()` fail at `uring_backend_c2b_identity_test.cpp`), revert
  GREEN.

## D3-M7 — borrow ends at original CQE / record_terminal instead of reap

- Mutated: `include/sluice/async/detail/request_arena.hpp`,
  `RequestArena::record_terminal` — `borrow_.active = false` added at
  backend_ready.
- Detector: `uring_c2c_borrow_active_through_lifecycle` (backend_ready window).
- RED: the backend_ready-before-reap borrow observation reports inactive; the
  I18 borrow-lifetime detector fails (rc=255).
- GREEN after revert: borrow active through backend_ready, ends only at reap.

## D3-M8 — wait-cancel cancels the I/O

- Mutated: `include/sluice/async/uring_backend.hpp`,
  `cancel_waiter_for_test` — after removing the waiter, an
  `issue_running_cancel` + transport flush is added (the blocked read gets
  effectively cancelled).
- Detector: `uring_c2c_wait_cancel_keeps_io`.
- RED: the I/O no longer continues; the original CQE reports the effective
  cancellation instead of the 0-byte EOF, and the independence detector fails
  (rc=255).
- GREEN after revert: wait-cancel removes only the waiter, the lease moves to
  the caller, the real result wins, no waiter delivered.

## D3-M9 — I/O cancel removes the waiter

- Mutated: `src/async/uring_backend.cpp`, `cancel_handle_` terminal_won branch —
  `arena_.cancel_waiter(handle)` added.
- Detector: `uring_c2c_io_cancel_keeps_waiter`.
- RED: after the enqueued cancel win the registration is gone; the
  waiter-survival detector fails (rc=255).
- GREEN after revert: the waiter survives the I/O cancel and is delivered with
  the canceled terminal at reap.

## D3-M10 — reap drops the waiter RoutingLease

- Mutated: `include/sluice/async/detail/request_arena.hpp`, `RequestArena::reap`
  — the waiter-delivery branch disabled.
- Detector: `uring_c2c_waiter_delivery_exactly_once`.
- RED: the sink receives a ReadyEvent without the registered token/lease; the
  exactly-once delivery detector fails (rc=255).
- GREEN after revert: token + lease delivered exactly once with the first (and
  only) publication.

## D3-M11 — stale waiter handle mutates the N+1 occupant

- Mutated: `include/sluice/async/detail/request_arena.hpp`,
  `register_waiter` / `cancel_waiter` — generation validation replaced by
  slot-index-only lookups.
- Detector: `uring_c2c_stale_waiter_authority_harmless`.
- RED: the captured generation-N handle registers/cancels on the live N+1
  occupant; the stale-waiter detector fails (rc=255).
- GREEN after revert: `not_found`, B's registration/borrow/terminal untouched.

---

## Confirmation commands (final D3 head, mutations reverted)

```sh
xmake build uring_backend_c2b_identity_test && xmake run uring_backend_c2b_identity_test
# 11/11 PASS (identity chain, generation reuse, stale cookie, stale handle,
#            pending/enqueued/running cancel, original-vs-control CQE order
#            (B) + kernel-portable cancel-control-authority detector, reap
#            boundary)

xmake build uring_backend_c2c_waiter_borrow_test && xmake run uring_backend_c2c_waiter_borrow_test
# 14/14 PASS (borrow lifecycle/exact metadata/no-buffer shape, registration
#            enqueued/running/backend_ready, cardinality, wait-cancel vs
#            I/O-cancel independence, exactly-once delivery, stale waiter,
#            register-after-record_terminal-before-reap)
```

Aggregate gate (real liburing): `uring_c2b_identity_integration=PASS`,
`uring_c2c_borrow_waiter_integration=PASS` (exact pinned case-sets, exactly one
`[evidence-meta] evidence=... mode=real` per run); `uring_c2e_close_drain_
not_implemented` still INCOMPLETE; KernelIo overall still NOT CONFORMING
(hard-code retained — D4).
