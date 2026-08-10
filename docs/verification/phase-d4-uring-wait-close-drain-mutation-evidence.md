# Phase D4 — Uring Wait / Close / Drain Mutation Evidence

Date: 2026-08-10. Branch: feat/phase-d4-uring-wait-close-drain. D3 is MERGED
into master; the D4 branch base is current master `259f0bd2dc5d027fd463132b65db1bef9c33f08f`
(the D3 merge commit) — no D3-branch stacking remains. Kernel:
6.18.33.2-microsoft-standard-WSL2. liburing: 2.14 (pinned). Build: Clang Debug
with `--with-liburing` (real mode).

Every mutant below was applied as a TEMPORARY production edit, the focused
evidence case was rebuilt and run (expect RED), the edit was reverted
byte-for-byte, and the same case was rebuilt and run again (expect GREEN). All
thirteen D4 implementation mutants (D4-M1..M13) plus the D4-L1 lift mutant
were executed on the final D4 implementation; all nine repair mutants
(D4-RM1..RM9) were executed on the final PR #84 repair head, and the G2 drift
closure finding (2026-08-10) was fixed with its own regression self-test. All
RED→GREEN transitions were confirmed by the actual binary exit codes (the
evidence targets fail non-zero on any `SLUICE_CHECK` violation or fail-fast; a
RED that manifests as a hang is bounded by a watchdog deadline — hang
watchdogs only, never ordering proof).

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
  INSIDE the transaction between arena commit and `binding -> outstanding`).
  The detector is the PR #84 lock-domain proof: the closer reads
  `c.outstanding()` AT its own return. Under the fix the dispatch_mtx_
  handoff makes the submit's LP release-store visible to that read
  (outstanding == true). Under the mutation close returns while the submitter
  is still paused and the read sees `binding` (outstanding == false) -> RED.
  The bounded probe window is failure protection only — the pass/fail
  decision is the outstanding() state assertion, never a timing claim.
- RED: the closer's post-return `c.outstanding()` is false — close returned
  before the LP release-store (rc=255).
- GREEN after revert: close blocks until the LP release-store, then returns;
  the closer observes outstanding == true; the LP-winning request still
  completes; a later submit is rejected.

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

## D4-RM1 — unlocked plain-bool fast-path reads re-introduced (P0-B)

- Mutated: `src/async/uring_backend.cpp`, `submit_size`/`submit_void` — the
  in-lock Stage-0 admission/poison authority replaced by the pre-repair
  unlocked fast-path reads of `fatal_error_`/`admission_closed_` (plain bools
  read outside `dispatch_mtx_`).
- Detector: TSan, focused set (`uring_c2e_close_waits_for_inflight_
  acceptance_lp`, `uring_c2e_close_wins_submit_started_before_close_rejected`,
  `uring_c2e_multiple_parked_waiters_all_wake`, `uring_c2e_close_wakes_parked_
  waiter_one_shot_no_busy_spin`, `uring_c2e_submit_races_close_linearization`).
  The pause-gate cases cannot expose the race (the gate handshake orders the
  read before the close write), so `uring_c2e_submit_races_close_linearization`
  runs 256 relaxed-atomic submit-vs-close attempts with NO happens-before edge:
  TSan observes the unsynchronized access directly.
- RED (tsan-fixed2.txt): `ThreadSanitizer: data race` — `Write of size 1` at
  `close_admission()` (`uring_backend.cpp:1669`) vs the unlocked read in
  `submit_size` (both one-byte stack locations).
- GREEN after revert (tsan-green.txt): the same focused set passes under TSan
  with zero race reports; the only readers of the admission/poison flags now
  run inside `dispatch_mtx_` (in-lock Stage-0 authority, D2 poison precedence
  preserved).

## D4-RM2 — death evidence case-set registration removed (P0-C, source side)

- Mutated: `tests/uring_backend_c2e_death_test.cpp` — one pinned death case
  registration removed from the real branch, so the driven run prints 8
  `[run]` lines instead of the pinned 9.
- Detector (aggregate-level): `python3 scripts/verify-backend-conformance.py`
  (real mode).
- RED (rm2-red3.txt): `uring_c2e_quiescent_destruction=INCOMPLETE`, lifecycle
  `INCOMPLETE`, `overall INCOMPLETE`, `RESULT: FAIL (1 mandatory issue)` — the
  death matrix no longer proves the full pinned corpus.
- GREEN after revert (rm2-green3.txt): `uring_c2e_quiescent_destruction=PASS`,
  `overall ELIGIBLE`, `RESULT: PASS`.

## D4-RM3 — backend-contract pin registration removed (P1-B)

- Mutated: `tests/uring_backend_test.cpp` — one pinned
  `uring_backend_contract` case registration removed from the real branch.
- Detector (aggregate-level, real mode).
- RED (rm3-red.txt): `uring_backend_contract=INCOMPLETE`, backend_specific
  `INCOMPLETE`, `overall INCOMPLETE`, `RESULT: FAIL` — the backend-specific
  record can no longer prove the stub+real contract corpus.
- GREEN after revert (rm3-green.txt): `uring_backend_contract=PASS`,
  `overall ELIGIBLE`.

## D4-RM4 — death evidence-mode case moved back inside the liburing guard (P1-A)

- Mutated: `tests/uring_backend_c2e_death_test.cpp` —
  `SLUICE_TEST_CASE(uring_d4_c2e_death_evidence_mode)` wrapped in
  `#if defined(SLUICE_HAS_LIBURING)` — the D3 R1/R3 defect shape re-applied to
  the D4 death target: the stub build registers 8 cases and emits no
  `[evidence-meta]`.
- Detector: `D4EvidenceModeDriveTest.test_c2e_death_evidence_mode_registered_
  in_both_builds` (mechanical source check).
- RED (rm4-red.txt): `AssertionError: 'mode=stub' not found in ...` — the
  both-builds registration is gone (1 failed, 9 passed).
- GREEN after revert: all 10 D4 self-tests pass; the death target registers
  the evidence-mode case in BOTH builds, emitting `mode=real` / `mode=stub`
  via the internal `#if/#else` (a stub run is INCOMPLETE by
  `required_modes=("real",)`, never by a missing case).

## D4-RM5 — c2e evidence-mode case registration removed (P1-A)

- Mutated: `tests/uring_backend_c2e_close_drain_test.cpp` — the
  `SLUICE_TEST_CASE(uring_d4_c2e_evidence_mode)` registration removed from
  both branches (the run prints the semantic set and zero meta lines).
- Detector (aggregate-level, real mode).
- RED (rm5-red.txt): `uring_c2e_close_drain=INCOMPLETE`, lifecycle
  `INCOMPLETE`, `overall INCOMPLETE`, `RESULT: FAIL`.
- GREEN after revert (rm5-green.txt): `uring_c2e_close_drain=PASS`,
  `overall ELIGIBLE`. The re-compile-out shape is additionally pinned at the
  gate level by `D4EvidenceModeDriveTest.test_c2e_stub_missing_evidence_mode_
  case_is_case_set_incomplete` (classified INCOMPLETE as a case-set mismatch,
  never PASS).

## D4-RM6 — wait-source include moved out of the liburing guard (P1-D)

- Mutated: `include/sluice/async/uring_backend.hpp` — the
  `#include <sluice/async/detail/uring_wait_source.hpp>` placed outside
  `#if defined(SLUICE_HAS_LIBURING)` (the stub/OFF public header would pull
  the Linux/POSIX-only `<poll.h>` / `<sys/eventfd.h>` / `<unistd.h>` chain).
- Detector: `D4DriftDetectorTest.test_wait_source_include_guarded_for_stub_
  public_header` (mechanical source check).
- RED (rm6-red.txt): `AssertionError: unexpectedly None: wait-source include
  has no enclosing #if`.
- GREEN after revert: the include sits inside the guard; the stub/OFF public
  header stays portable and the mechanical check passes.

## D4-RM7 — pre-poll park counter removed (P1-C)

- Mutated: `include/sluice/async/detail/uring_wait_source.hpp` — the guarded
  `prepark_counter_` increment removed from the observe-phase park.
- Detector: `uring_c2e_multiple_parked_waiters_all_wake` — a deterministic
  per-participant pre-poll park count; the bounded deadline is a hang watchdog
  ONLY, the pass/fail decision is the state assertion (all participants
  reached the park point), never a timing claim.
- RED (rm7-red.txt): `uring_c2e: only 1 of 3 participants reached the pre-poll
  park point (deadline)` — rc=255.
- GREEN after revert: exactly 3 participants are observed at the park point;
  the single control wake releases all three and each returns 0 (nothing
  fabricated).

## D4-RM8 — close-admission lock serialization removed (P1-C)

- Mutated: `src/async/uring_backend.cpp`, `close_admission()` — the
  `dispatch_mtx_` guard removed (close no longer serializes against the submit
  admission transaction; the D4-M2 defect shape, re-proven without any sleep).
- Detector: `uring_c2e_close_waits_for_inflight_acceptance_lp` — the PR #84
  lock-domain proof: the closer reads `c.outstanding()` AT its own return. The
  bounded probe window is failure protection only; the pass/fail decision is
  the `outstanding()` state assertion, never a timing claim.
- RED (rm8-red.txt): `uring_c2e: close_admission returned before the in-flight
  acceptance LP (admission transaction violated)` — rc=255.
- GREEN after revert: close blocks until the LP release-store; the closer
  observes `outstanding() == true`; the LP-winning request completes; a later
  submit rejects `invalid_state`.

## D4-RM9 — destruction record pin drifted from source (P0-C, manifest side)

- Mutated: `scripts/backend_conformance_manifest.py` —
  `uring_d4_c2e_death_evidence_mode` removed from the
  `uring_c2e_quiescent_destruction` cases pin (manifest 8 vs source 9).
- Detectors: (a) `D4DriftDetectorTest.test_uring_quiescent_destruction_pin_
  matches_source` — RED (set mismatch: the source still registers the case the
  pin lost); (b) aggregate gate (real mode) — RED (rm9-red.txt /
  rm9-agg-red.txt): `uring_c2e_quiescent_destruction=INCOMPLETE`, `overall
  INCOMPLETE`, `RESULT: FAIL (1 mandatory issue)`.
- GREEN after revert: the drift self-test passes; the aggregate reports
  `uring_c2e_quiescent_destruction=PASS`, `overall ELIGIBLE`, `RESULT: PASS`
  (rm9-agg-green.txt).

## G2 drift closure (found during the final gate cycle, 2026-08-10)

- Finding: `uring_c2e_close_drain` pinned 16 cases while the current source
  registers 17 — `uring_c2e_submit_races_close_linearization` (added for the
  P0-B TSan linearization evidence) was never added to the pin. The strict
  set-equivalence gate (Issue #81 P1 G2) classifies a current-build real run
  as `INCOMPLETE` (`unexpected=[uring_c2e_submit_races_close_linearization]`)
  even though the binary itself passes — the earlier aggregate PASS files had
  been produced by pre-case binaries and were stale.
- Fix: the pin now lists all 17 cases; the record notes document the
  linearization case; `D4DriftDetectorTest.test_uring_close_drain_pin_matches_
  source` turns this drift class into a RED self-test for every pinned D4
  multi-case Uring record (close/drain, backend contract, quiescent
  destruction — D3 c2b/c2c pins verified matching as well). 375 self-tests
  pass; the fresh real-mode aggregate reports `uring_c2e_close_drain=PASS`,
  `overall ELIGIBLE`.

---

## Confirmation commands (final head, mutations reverted)

```sh
xmake build -r uring_backend_c2e_close_drain_test uring_backend_c2e_death_test
xmake run uring_backend_c2e_close_drain_test
# 17/17 PASS (close while pending/enqueued/running/backend_ready, post-close
#            size+void+malformed rejection, submit-vs-close LP both orderings,
#            concurrent submit-vs-close linearization (256 relaxed-atomic
#            attempts), close-then-pending/running cancel, one-shot parked-
#            waiter wake, multi-waiter interrupt, interrupt-vs-final-ready
#            race, drained != releasable, poison + close)

xmake run uring_backend_c2e_death_test
# 9/9 PASS (7 non-quiescent states fail-fast exit 86; quiescent control exit
#           0; evidence-mode case emits mode=real in both-build registration)

SLUICE_TEST_FILTER=conformance_close_drain_uring xmake run backend_conformance_test
# shared C2e suite for Uring: PASS, [conformance-meta] backend=Uring
# profile=KernelIoProfile mode=real (close_rejects_future_submit,
# close_preserves_accepted_terminal, drain_then_reset_releases_slot,
# slot_released_but_admission_stays_closed)
```

Aggregate gate (real liburing, final head, 2026-08-10): shared /
shared_capacity / c2e_shared_close_drain suites PASS, lifecycle PASS (incl.
`uring_c2e_close_drain` with the full 17-case pin and
`uring_c2e_quiescent_destruction` with the 9-case pin), backend_specific PASS
(`uring_backend_contract` 10-case pin), `overall ELIGIBLE` (KernelIo), RESULT:
PASS. Stub build (same final head): 156/156 tests PASS, Uring
`mode=stub` — shared/lifecycle/backend_specific INCOMPLETE, `overall
INCOMPLETE` (aggregate PASS only for the stub-mode run; a stub build can never
make KernelIo ELIGIBLE). Python manifest self-test suite: 375 tests PASS
(`python3 -m unittest discover -v scripts/tests`).
