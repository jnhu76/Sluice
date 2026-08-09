# Phase D4 — Uring Wait / Close / Drain Mutation Evidence

Date: 2026-08-10. Branch: test/phase-d3-uring-identity-waiter-conformance (D4
work-in-progress; final branch feat/phase-d4-uring-wait-close-drain stacked on
the D3 head). Baseline SHA: `4cc4789` (D3 head). Kernel:
6.18.33.2-microsoft-standard-WSL2. liburing: 2.14 (pinned). Build: Clang Debug
with `--with-liburing` (real mode).

Every mutant below was applied as a TEMPORARY production edit, the focused
evidence case was rebuilt and run (expect RED), the edit was reverted
byte-for-byte, and the same case was rebuilt and run again (expect GREEN). All
thirteen mutants were executed on the final D4 implementation; all RED→GREEN
transitions were confirmed by the actual binary exit codes (the evidence
targets fail non-zero on any `SLUICE_CHECK` violation or fail-fast; a RED that
manifests as a hang is bounded by a watchdog deadline — hang watchdogs only,
never ordering proof).

Command shapes:

```sh
# RED run (mutant applied):
xmake build -r <target> && timeout 60 SLUICE_TEST_FILTER=<case> xmake run <target>
#   observed: non-zero exit (rc=255 fail-fast / check failure; rc=124 hang-watchdog)

# GREEN run (mutant reverted):
xmake build -r <target> && timeout 60 SLUICE_TEST_FILTER=<case> xmake run <target>
#   observed: exit 0, "ALL TESTS PASSED"
```

## D4-M1 — post-close submit accepted (admission-closed rejection removed at every layer)

- Mutated: `src/async/uring_backend.cpp` — the fast-path AND in-lock
  `admission_closed_` Stage-0 checks in `submit_size` AND `submit_void`, plus
  the `RequestArena::reserve()` closed-rejection in
  `include/sluice/async/detail/request_arena.hpp`. (Single-layer removals are
  masked by the remaining layers: the backend checks and the arena reserve are
  mutually reinforcing backstops for the same invariant.)
- Detectors: `uring_c2e_close_wins_submit_started_before_close_rejected`,
  `uring_c2e_void_submit_after_close_rejected`,
  `uring_c2e_malformed_submit_after_close_rejected`.
- RED: rc=255 each — the resumed submit is accepted after close (a check
  `r.error().code == IoError::Code::invalid_state` fails at
  `uring_backend_c2e_close_drain_test.cpp:354`) or a fail-fast fires when the
  drained-into-invalid state is observed.
- GREEN after revert: all three cases pass; post-close submit rejects
  synchronously with `invalid_state`, Completion idle, zero residue.

## D4-M2 — close returns before the in-flight acceptance LP completes

- Mutated: `src/async/uring_backend.cpp`, `close_admission()` — the
  `dispatch_mtx_` guard removed (the arena close + `admission_closed_ = true`
  run unlocked, so close no longer serializes against the submit admission
  transaction). This is the migration plan §13 priority mutant "close returns
  before acceptance LP arbitration".
- Detector: `uring_c2e_close_waits_for_inflight_acceptance_lp` (submit paused
  INSIDE the transaction between arena commit and `binding -> outstanding`;
  the closer must block until the LP completes).
- RED: the negative probe fails — `close_returned` is true while the submit
  still holds the transaction (rc=255 fail-fast from the racing state).
- GREEN after revert: close blocks until the LP release-store, then returns;
  the LP-winning request still completes; a later submit is rejected.

## D4-M3 — close cancels the enqueued request

- Mutated: `src/async/uring_backend.cpp`, `close_admission()` — after closing,
  the dispatch-ring front is terminalized with `canceled` (close becomes an
  I/O cancellation primitive).
- Detector: `uring_c2e_close_while_enqueued_preserves_dispatch`.
- RED: the resumed drain attempts SQE installation on the terminalized front —
  `mark_running false after get_sqe (invariant violation — cancel cannot have
  won under the dispatch_mtx_ discipline)` fail-fast (rc=255).
- GREEN after revert: the dispatch linkage is preserved; the request executes
  and reaches its real 8-byte terminal.

## D4-M4 — close retroactively cancels an accepted pending request

- Mutated: `src/async/uring_backend.cpp`, `enqueue_after_commit()` — when
  `admission_closed_`, the accepted request is terminalized with `canceled`
  instead of being enqueued (close retroactively rejects accepted work).
- Detector: `uring_c2e_close_while_pending_preserves_accepted`.
- RED: the resumed submit's request is reaped as canceled — the
  `res.value() == 8` real-terminal check fails (rc=255).
- GREEN after revert: the accepted request (submit won the LP) reaches its real
  terminal even though close completed while it was `pending`.

## D4-M5 — poison error loses to admission-closed error

- Mutated: `src/async/uring_backend.cpp`, `submit_size` — the `fatal_error_`
  check moved AFTER the `admission_closed_` check (a poisoned backend that is
  then closed reports `invalid_state` instead of the poison `backend_error`).
- Detector: `uring_c2e_poison_close_keeps_class_c`.
- RED: `r.error().code == IoError::Code::backend_error` fails at
  `uring_backend_c2e_close_drain_test.cpp:777` (the close flag shadows the
  poison error; rc=255).
- GREEN after revert: the permanent-transport poison error keeps precedence;
  the quarantined Class-A ledger entry stays as teardown evidence.

## D4-M6 — close does not wake parked waiters (interrupt_all skipped)

- Mutated: `src/async/uring_backend.cpp`, `close_admission()` — the
  `wait_source_->interrupt_all()` call removed.
- Detector: `uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin`.
- RED: the parked `wait_one` participant never returns — the join blocks until
  the watchdog kills the run (rc=124; hang watchdog, no fabricated
  completion).
- GREEN after revert: close wakes the parked participant with a one-shot
  control wake (returns 0, nothing fabricated), and a future wait parks
  normally again and wakes on real progress.

## D4-M7 — wait_for_change ignores the control epoch

- Mutated: `include/sluice/async/detail/uring_wait_source.hpp`,
  `wait_for_change()` — the `control_epoch_ != observed.control_generation`
  branch (control wake reporting) removed.
- Detector: `uring_c2e_multiple_parked_waiters_all_wake` (and
  `uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin`).
- RED: `interrupt_backend_waiters()` bumps the epoch and writes the eventfd,
  but the woken poller re-checks nothing and re-parks forever — all three
  participants strand (rc=124 hang watchdog).
- GREEN after revert: one interrupt wakes ALL parked participants; each
  re-evaluates and returns 0 (interrupted, nothing fabricated).

## D4-M8 — wait_for_change polls only the ring fd (control fd dropped)

- Mutated: `include/sluice/async/detail/uring_wait_source.hpp`,
  `wait_for_change()` — the poll set reduced to `pfds[1]` with `nfds=1` (the
  control eventfd is never polled).
- Detector: `uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin` (and
  `uring_c2e_multiple_parked_waiters_all_wake`).
- RED: control-plane writes never wake the parked poll — the close-wake phase
  hangs (rc=124).
- GREEN after revert: ring progress (kernel) and control wakes (eventfd) both
  unblock the park.

## D4-M9 — ring fd never installed into the wait source

- Mutated: `src/async/uring_backend.cpp`, ring setup — `set_ring_fd(...)`
  removed (the wait source parks on `ring_fd_ == -1`).
- Detector: `uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin`.
- RED: `poll(2)` ignores `fd < 0`, so the kernel-progress wake path is dead;
  phase-2 real-progress wake (pipe close -> CQE) never reaches the parked
  waiter — the run hangs (rc=124; the POLLNVAL fail-fast is reserved for a
  CLOSED fd, not `-1`, per poll(2) semantics).
- GREEN after revert: the parked waiter wakes with POLLIN exactly when the
  kernel delivers the CQE.

## D4-M10 — split-wait capability disabled (wait_source() == nullptr)

- Mutated: `include/sluice/async/uring_backend.hpp`, `wait_source()` — always
  returns `nullptr`, forcing `AsyncIoContext::wait_one()` onto the legacy
  serialized path (blocks under `access_mtx_` in `io_uring_submit_and_wait`).
- Detector: `uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin`.
- RED: the observe-phase park never arms the wait-phase flag — the
  deterministic `wait_until(wait_phase)` times out and the case bails
  (rc=255; multi-participant parking is structurally impossible on the legacy
  path).
- GREEN after revert: the split-phase `snapshot -> poll -> park` protocol runs
  with the park OUTSIDE `access_mtx_`.

## D4-M11 — poll()/wait_one() refuse to reap after close

- Mutated: `src/async/uring_backend.cpp`, `poll()` — an early
  `if (admission_closed_) return 0;` added (close blocks the reap path).
- Detectors: `uring_c2e_close_while_backend_ready`,
  `uring_c2e_close_then_pending_cancel_wins`.
- RED: the post-close `poll() == 1` expectations see 0 and fail-fast (rc=255)
  — the backend-ready terminal is never reaped and the canceled terminal never
  publishes.
- GREEN after revert: reap remains legal after close; the terminal publishes
  exactly once.

## D4-M12 — cancel refused after close

- Mutated: `src/async/uring_backend.cpp`, `cancel_handle_()` — an early
  `if (admission_closed_) return not_found;` added.
- Detectors: `uring_c2e_close_then_pending_cancel_wins`,
  `uring_c2e_close_then_running_cancel_intent_only`.
- RED: the close-then-pending cancel no longer wins the canceled terminal
  (rc=255) and the close-then-running cancel no longer records intent (rc=255)
  — the requests are stranded (drain timeout fail-fast).
- GREEN after revert: cancel remains legal after close (ADR Decision 15):
  pending cancel wins with no SQE ever installed; running cancel records
  intent only and the original CQE decides verbatim.

## D4-M13 — non-quiescent destruction waits instead of fail-fast

- Mutated: `src/async/uring_backend.cpp`, `~UringAsyncBackend()` — the
  quiescence preflight's fail-fast replaced by an unbounded wait loop (the
  migration plan §13 priority mutant "destructor drains/cancels implicitly" —
  AGENTS.md §14 prohibits destructors that wait for async progress).
- Detector: `uring_c2e_death_destroy_with_pending` (the 8-case death matrix,
  `uring_backend_c2e_death_test`; a representative child case was used for the
  RED run — the 60s child watchdog makes a full-matrix hang run impractical).
- RED: the child exceeds the death-runner watchdog — "child exceeded watchdog
  timeout and was killed; the destructor likely hung instead of fail-fasting"
  (case fails, rc=255). Note: a pure preflight-REMOVAL mutant is masked by the
  `Completion` destructor contract (destruction of a non-idle Completion also
  fail-fasts), so the D4 mutation targets the wait/progress prohibition, which
  is the behavior the preflight exists to prevent.
- GREEN after revert: the full 8-case matrix passes — 7 non-quiescent states
  fail-fast with exit 86, the quiescent control case exits 0.

---

## D4-L1 — KernelIo gate lift (the final mutation)

- Mutated: `scripts/verify-backend-conformance.py` — the pre-D4 KernelIo
  fail-closed hard-code re-introduced (`_backend_verdict` returns
  NOT CONFORMING for KernelIoProfile regardless of evidence).
- Detector (aggregate-level): the real-mode aggregate gate
  (`python3 scripts/verify-backend-conformance.py --no-build`).
- RED (pre-lift state, recorded before the removal): with the complete D4 real
  evidence set in place, KernelIo still reports `overall NOT CONFORMING`
  (hard-code ignores the evidence) — the lift condition "only if all mandatory
  real evidence complete" was not yet satisfied by the machinery.
- GREEN (post-lift, executed 2026-08-10): with the hard-code removed and the
  per-suite real-mode downgrades in place, the real-mode gate reports
  `shared=PASS`, `lifecycle=PASS` (`uring_c2b_identity_integration`,
  `uring_c2c_borrow_waiter_integration`, `uring_c2d_failure_injection`,
  `uring_c2e_close_drain` all PASS with mode=real), `backend_specific=PASS`
  (`uring_backend_contract` mode=real), `overall ELIGIBLE`.
- Stub-mode control (spec §41): the same evidence corpus in a stub build
  reports `shared=INCOMPLETE`, `lifecycle=INCOMPLETE`, `backend_specific=
  INCOMPLETE`, `overall INCOMPLETE` — the real-mode obligations can never be
  satisfied by stub/subset evidence (verified 2026-08-10).

---

## Confirmation commands (final head, mutations reverted)

```sh
xmake build -r uring_backend_c2e_close_drain_test uring_backend_c2e_death_test
xmake run uring_backend_c2e_close_drain_test
# 16/16 PASS (close while pending/enqueued/running/backend_ready, post-close
#            size+void+malformed rejection, submit-vs-close LP both orderings,
#            close-then-pending/running cancel, one-shot parked-waiter wake,
#            multi-waiter interrupt, interrupt-vs-final-ready race, drained !=
#            releasable, poison + close)

xmake run uring_backend_c2e_death_test
# 8/8 PASS (7 non-quiescent states fail-fast exit 86; quiescent control exit 0)

SLUICE_TEST_FILTER=conformance_close_drain_uring xmake run backend_conformance_test
# shared C2e suite for Uring: PASS, [conformance-meta] backend=Uring
# profile=KernelIoProfile mode=real (close_rejects_future_submit,
# close_preserves_accepted_terminal, drain_then_reset_releases_slot,
# slot_released_but_admission_stays_closed)
```

Aggregate gate (real liburing): shared / shared_capacity / c2e_shared_close_drain
suites PASS, lifecycle PASS, backend_specific PASS, `overall ELIGIBLE`
(KernelIo). Stub build: all three suites INCOMPLETE, `overall INCOMPLETE` —
stub evidence never satisfies the real-mode obligations (spec §41).
