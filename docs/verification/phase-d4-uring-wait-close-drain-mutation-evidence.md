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
were executed on the final D4 implementation; the round-2 repair mutants
(D4-RM1..RM9, round-1; D4-RM10..RM12 plus the pre-poll-barrier-uniqueness and
close-LP shared-lock mutants, round-2) were executed on the final PR #84
repair head, the round-3 repair mutant D4-RM13 (inter-iteration control wake)
was executed on the round-3 head, and the G2 drift closure finding
(2026-08-10) was fixed with its own regression self-test. All RED→GREEN
transitions were confirmed by the actual binary exit codes (the evidence
targets fail non-zero on any `SLUICE_CHECK` violation or fail-fast; a RED that
manifests as a hang is bounded by a watchdog deadline — hang watchdogs only,
never ordering proof).

Round-2 (2026-08-10) added: D4-RM10 (post-poll ring-ready bypasses control
reclassification), D4-RM11 (destructor preflight bypassed before queue_exit),
D4-RM12 (non-EINTR poll failure treated as retryable), the pre-poll-barrier-
uniqueness mutant (one waiter double-counts participant arrivals), and the
close-LP shared-lock mutant revisited (D4-RM8 mutant now caught by a
source-drift self-test). The round-2 head SHA and case counts are recorded
in the Confirmation commands block.

Round-3 (2026-08-11) added: D4-RM13 (inter-iteration control wake
rebaselined per internal progress iteration) — the P0 control-wake-theorem
gap found by review. The CONTROL baseline belongs to the whole external
`wait_one()` invocation, not one internal `wait_for_change` loop; the
context-level `AsyncIoContext::WaitSourceProgressPauseGate` seam pins the
exact inter-iteration window. The round-3 head SHA and case counts are
recorded in the Confirmation commands block.

Round-4 (2026-08-11, PR #84 round-4 review) added the two remaining P0
mutants and the P1 lock-order repair, each with its own RED→GREEN detector:

- D4-RM14 (P0-1): the commit-to-park handshake — the Scheduler's MW-S2
  participant registers its mandatory control-observation baseline with the
  backend wait source under global_mtx_ (arm_committed_wait) BEFORE the
  backend-park commitment is exposed, and wait_one() consumes that baseline
  (consume_committed_wait) instead of a bare entry snapshot. Mutant = the arm
  removed from the Phase-B commit; detector =
  `stop_between_mw_s2_commit_and_backend_wait_registration`
  (application_runtime_drain_starvation_test), which injects
  request_stop() in the commit-to-wait_one window via the
  `mw_s2_committed_before_wait_one` phase seam and proves the run terminates
  and RE-ENTERS (the monotonic per-entry wait counter reaches 2). RED: the
  first wait parks through the interrupt, the run never re-enters, and the
  detector fails with the re-entry message. GREEN after revert: the run
  re-enters and drain()/join() converge.
- D4-RM15 (P0-2): the durable-broadcast gate — a FUTURE-generation waiter's
  eventfd drain is gated on the parked-at-publish waiters' acknowledgement
  (a single consumable eventfd token cannot implement notify_all once a
  future waiter drains it; a woken poller whose readiness recheck finds an
  empty counter re-sleeps). Mutant = the gate removed from wait_for_change;
  detector = `uring_c2e_future_waiter_cannot_steal_old_wake` (new pinned C2e
  case, the 21st): an old waiter held at the pre-poll barrier, an interrupt,
  and a future waiter that must NOT drain the token; the old waiter returns
  interrupted (0) and the future waiter wakes only on real progress. RED:
  the future waiter drains the token and reaches the barrier (fast-fail
  diagnostic), and the old waiter's poll re-sleeps forever. GREEN after
  revert: old waiter interrupted, future waiter reaps real progress.
- D4-RM16 (P1-3): the poison wake deferred past dispatch_mtx_ — the
  wait-source mutex is a LEAF domain and must never be acquired while
  holding dispatch_mtx_ (frozen lock order: access_mtx_ -> dispatch_mtx_ ->
  arena leaf -> wait-source mtx_). The in-lock signal in
  poison_and_recover_locked was removed; the two paths with NO following
  reap (enqueue_after_commit, issue_running_cancel) now defer the wake past
  their own dispatch_mtx_ scope (state first, then wake). No new mutant
  detector is needed: the repair is covered by the restored lock-order
  contract comment, the full race-class TSan evidence, and the poison
  semantics tests (uring_c2e_poison_close_keeps_class_c and the D2 poison
  matrix) that prove the terminal/ready state is unchanged.

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
  The case makes NO deterministic mutex-blocking claim (a runtime "closer
  blocked on the admission mutex" observation cannot be made without scheduler
  timing). The DETERMINISTIC authority that the submit Stage 0..commit_binding
  path and close_admission's admission-close write share the same
  `dispatch_mtx_` lives in the source-drift self-test
  `D4DriftDetectorTest.test_close_admission_uses_dispatch_mtx` (round-2, the
  D4-RM8 mutant revisited): it parses `uring_backend.cpp`, locates the real
  `close_admission()` body, and asserts both `arena_.close_admission()` and
  `admission_closed_ = true` sit INSIDE a `lock_guard(dispatch_mtx_)` critical
  section. Focused TSan on submit||close and the concurrent linearization
  case `uring_c2e_submit_races_close_linearization` complete the evidence.
- RED: the source-drift self-test reports the missing critical section
  (rc=255); the runtime case observes the in-flight LP state and the
  post-close reject at the contract level.
- GREEN after revert: the self-test passes; the runtime case proves the
  in-flight LP-winning request is accepted, driven to exactly one terminal,
  and that post-close submission rejects — the LP-window semantics are
  covered by the source-drift authority (the shared dispatch_mtx_ critical
  section) plus TSan on submit||close. NOTE (D4-RM16 / round-4): the runtime
  case deliberately does NOT claim a concurrent closer blocked on the
  admission mutex — the current case resumes + joins the submitter before
  the main thread calls close, so no "closer observes outstanding == true"
  statement is provable from it; the deterministic lock-sharing authority is
  the source-drift self-test.

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

## D4-RM10 — post-poll ring-ready branch bypasses control epoch reclassification (P0, round-2)

- Mutated: `include/sluice/async/detail/uring_wait_source.hpp` —
  `wait_for_change()`: restored the old `if (pfds[0].revents & POLLIN) return
  BackendWakeReason::progress;` BEFORE the post-poll control-epoch recheck, so a
  co-ready ring + control wake returns progress and swallows the control wake.
- Detectors: `uring_c2e_control_wins_over_co_ready_ring` (single-waiter co-ready)
  and `uring_c2e_two_waiter_consumer_strand` (production-bug detector: A+B
  outstanding, A reaped by a concurrent consumer, T1 reparks on B forever).
- RED (rm10-red-a.txt): `uring_c2e_control_wins: waiter never returned (strand
  on stale token after ring POLLIN)` — the single waiter re-loops on a stale
  token after the override "ring" is drained and the interrupt was swallowed.
- RED (rm10-red-b.txt): `uring_c2e_two_waiter_strand: T1 never returned (reparked
  on B after swallowing the control wake)` — the exact production interleaving.
- GREEN after revert: both cases PASS (control recheck wins, the waiter observes
  the interrupt, the final poll reaps nothing; returns 0).

## D4-RM11 — destructor preflight bypassed before io_uring_queue_exit (P0, round-2)

- Mutated: `src/async/uring_backend.cpp` — `~UringAsyncBackend()`: commented out
  the `detail::uring_non_quiescent_destruction_fail_fast();` call so a non-
  quiescent destroy proceeds past the preflight to the `io_uring_queue_exit`
  teardown boundary.
- Detector: `uring_c2e_death_preflight_before_queue_exit_order` (new pinned
  case). The death child installs a `BeforeQueueExit` hook (raw `void(*)(void*)`
  + ctx) that `_Exit(90)`. Under the fix the preflight fires first (exit 86) and
  the hook is NEVER reached; under the mutant the hook IS reached (exit 90).
  `expect_terminated_via_fail_fast` recognizes 86 only, so exit 90 is RED and is
  named exactly (distinct from 86 fail-fast / 87 unexpected-return / 88 child-
  setup-fail).
- RED (rm11-red.txt): `[death] preflight-order: FAIL (exit=90; expected terminate
  exit 86. exit=87 means the call returned instead of terminating)` — the
  teardown boundary was reached before the preflight would have run.
- GREEN after revert: `[death] preflight-order: PASS (terminated via Mutex
  fail-fast boundary, exit=86)`; full death suite 10/10 PASS.

## D4-RM12 — non-EINTR poll(2) failure treated as retryable (P1, round-2)

- Mutated: `include/sluice/async/detail/uring_wait_source.hpp` —
  `wait_for_change()`: added `continue;` for ALL `rc < 0` (including non-EINTR),
  so a physical poll failure busy-spins instead of fail-fasting.
- Detector: `uring_c2e_non_eintr_poll_failure_failfast` (new pinned case). A
  fork/exec child creates a fresh backend, installs a test-only `PollFn` seam
  that returns -1 with `errno=EIO`, submits a blocked read, and calls wait_one.
  Under the fix the wait source terminates (child `_Exit(86)` via a deterministic
  terminate handler); under the mutant the child busy-spins and the parent's
  8-second watchdog SIGKILLs it (RED). The PollFn seam avoids relying on an
  invalid fd (poll reports POLLNVAL via revents, not `rc<0`).
- RED (rm12-red.txt): `uring_c2e: non-EINTR poll watchdog fired (child busy-
  spinning, D4-RM12 mutant)` then `!watchdog` check fails.
- GREEN after revert: child `_Exit(86)`; case PASSes.

## Pre-poll barrier uniqueness mutant (P1, round-2)

- Mutated: `include/sluice/async/detail/uring_wait_source.hpp` — restored the
  loop-reentered `prepark_counter_.fetch_add(1)` as the SOLE park-arrival signal
  and removed the `BeforePhysicalPollPauseGate` barrier, so one waiter retrying
  on EINTR can contribute multiple arrivals and `arrivals == N` no longer proves
  N distinct participants parked.
- Detector: the multi-waiter cases rely on the barrier (`arrivals == N` is the
  deterministic pre-condition for driving the control wake); a mutant that lets
  one waiter double-count can report `arrivals == N` with fewer than N distinct
  parked participants, degrading the deterministic proof into a timing one. The
  barrier is the structural fix (it blocks a participant at the poll boundary
  until released, so the same thread cannot arrive twice before release).
- RED: a barrier-less build lets `arrivals` advance past the distinct-participant
  bound (the proof no longer pins N distinct parked waiters).
- GREEN after revert: the barrier guarantees one arrival per distinct
  participant; multi-waiter cases PASS.

## Close-LP shared-lock mutant revisited (D4-RM8, round-2 source-drift detector)

- Mutated: `src/async/uring_backend.cpp` — `close_admission()`: removed the
  `std::lock_guard<std::mutex> lk(dispatch_mtx_)` critical section so
  `arena_.close_admission()` and `admission_closed_ = true` are written
  unlocked (the submit-vs-close shared-lock arbitration is gone).
- Detector: `D4DriftDetectorTest.test_close_admission_uses_dispatch_mtx` (new
  source-drift self-test). It parses `uring_backend.cpp`, locates the real
  `close_admission()` body (the one that calls `arena_.close_admission()`), and
  asserts both writes sit INSIDE a `lock_guard(dispatch_mtx_)` critical section.
  This is the DETERMINISTIC authority for the submit-vs-close linearization
  (a runtime "closer blocked on mutex" observation cannot be made without
  scheduler timing — the rewritten `uring_c2e_close_waits_for_inflight_
  acceptance_lp` case makes no mutex-blocking claim). Focused TSan on
  submit||close and `uring_c2e_submit_races_close_linearization` complete the
  evidence.
- RED (rm8-red.txt): `AssertionError: close_admission() has no lock_guard
  (dispatch_mtx_) critical section (D4-RM8 mutant: shared lock removed)`.
- GREEN after revert: self-test PASSes; close-drain suite PASSes.

## D4-RM13 — inter-iteration control wake rebaselined per internal progress iteration (P0, round-3)

- Mutated: `src/async/async_io_context.cpp` — `wait_one()`: the
  `token.control_generation = control_baseline` pin removed (the CONTROL
  baseline is rebaselined to the fresh snapshot on every internal progress
  iteration, restoring the pre-fix behavior; `(void)control_baseline;` keeps
  the unused-variable warning quiet).
- Detector: `ctx_wait_one_inter_iteration_control_wake_not_lost`
  (`async_io_context_split_wait_c2e_test`, new pinned case). The round-3 P0
  found by review: the control wake lands in the inter-iteration window
  BETWEEN `wait_for_change()` returning `progress` and the next internal
  `snapshot()`. A test-only context-level pause seam
  (`AsyncIoContext::WaitSourceProgressPauseGate`, compiled out of production)
  parks T1 at the exact window; the test drives `signal_progress` (first
  wake), parks T1, then fires `interrupt_all` (control wake) and resumes T1.
  Under the fix the invocation-level control baseline (C0) is preserved, so
  the second `wait_for_change({P_now, C0})` sees the control delta and
  returns interrupted -> final poll -> 0. Under the mutant the fresh snapshot
  absorbs C1 into the observed token, the stale control event is drained, and
  T1 reparks on the blocked op forever (bounded watchdog -> RED).
- RED (rm13-red.txt): `T1 never returned after resume — the inter-iteration
  control wake was rebaselined away and T1 reparked (D4-RM13 mutant:
  control_generation pinned per loop)` — rc=255.
- GREEN after revert: `ctx_wait_one_inter_iteration_control_wake_not_lost`
  PASSes; the full split-wait target (2 cases) PASSes. This is the last real
  protocol blocker on the D4 wait path: the control-wake theorem's scope is
  one EXTERNAL wait_one invocation, not one internal wait_for_change loop.

## D4-RM14 — commit-to-park handshake removed (P0-1, round-4)

- Mutated: `src/async/scheduler.cpp` — the MW-S2 Phase-B commit block: the
  `ctx_.arm_backend_wait_commit()` call removed (the participant no longer
  registers its mandatory control-observation baseline under global_mtx_
  before the backend-park commitment is exposed; wait_one() falls back to a
  bare entry snapshot, so a request_stop() landing between the commit and
  the wait_one() entry is rebaselined as a past event and the participant
  parks through it).
- Detector: `stop_between_mw_s2_commit_and_backend_wait_registration`
  (application_runtime_drain_starvation_test, new case). The
  `mw_s2_committed_before_wait_one` phase seam (compiled out of production)
  pauses the committed participant between the registration and
  `ctx_.wait_one()`; the test fires `request_stop()` in that exact window,
  releases the seam, and requires the run to terminate and RE-ENTER (the
  monotonic per-entry wait counter of the ThreadPool ready wait reaches 2).
  The wait-source arm/consume (`BackendWaitSource::arm_committed_wait` /
  `consume_committed_wait`, implemented by both ReadyWaitSource and
  UringWaitSource) is the fix under test.
- RED (mutant applied): `FAILED 1 check(s): the run never re-entered the
  backend wait after the stop (commit-to-park registration lost)` — the
  first wait parks through the interrupt (baseline = post-stop snapshot).
- GREEN after revert: the case PASSes; drain()/join() converge; the full
  runtime drain-starvation target (2 cases) PASSes.

## D4-RM15 — durable-broadcast gate removed (P0-2, round-4)

- Mutated: `include/sluice/async/detail/uring_wait_source.hpp` —
  `wait_for_change()`: the `cv_.wait(lk, pending_wake_count_ == 0)` gate
  removed (the pre-park drain runs unconditionally, so a FUTURE-generation
  waiter can consume the single eventfd token that an OLD-generation waiter
  was woken by but has not yet rechecked — the woken poller's readiness
  recheck then finds an empty counter and re-sleeps; interrupt_all() loses
  the wake).
- Detector: `uring_c2e_future_waiter_cannot_steal_old_wake` (new pinned C2e
  case, the 21st in `uring_c2e_close_drain`): an old waiter (T1) parks and
  is held at the pre-poll barrier; the interrupt publishes C0 -> C1 and the
  single token; a future waiter (T2, baseline C1) must NOT drain the token —
  under the fix it blocks on the gate, T1's poll returns on the still-present
  token, T1 rechecks C1 != C0 and returns interrupted (0) (its single
  acknowledgement releases the gate), and T2 then drains the stale token,
  parks, and wakes on REAL progress (1).
- RED (mutant applied): `uring_c2e_future_waiter_steal: future waiter
  reached the pre-poll barrier (token drained)` — the future waiter drained
  the token; the old waiter's poll re-slept forever (rc=255).
- GREEN after revert: the case PASSes; the full C2e real target (21/21)
  PASSes.

## D4-RM16 — poison wake inside dispatch_mtx_ (P1-3, round-4)

- Repaired: `src/async/uring_backend.cpp` —
  `poison_and_recover_locked()` no longer calls `signal_ready_progress()`
  while holding dispatch_mtx_ (the wait-source mutex is a LEAF domain; the
  frozen lock order is access_mtx_ -> dispatch_mtx_ -> arena leaf ->
  wait-source mtx_). The two paths with NO following reap —
  `enqueue_after_commit()` and `issue_running_cancel()` — defer the wake
  past their own dispatch_mtx_ scope (state first, then wake, D4-RM16). The
  poll()/wait_one() paths already signal outside the lock via their n>0
  reap path (poison always retires >= 1 ledger/queue entry).
- Evidence: the pre-fix code nested wait-source mtx_ under dispatch_mtx_
  (the header's lock-order comment and the gate doc now agree with the
  code); the poison semantics are unchanged (proven by
  `uring_c2e_poison_close_keeps_class_c` and the D2 poison matrix cases);
  the race classes are covered by the round-4 TSan evidence.

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

## Confirmation commands (round-2 head, mutations reverted)

Round-2 head: `471e4fb` (feat/phase-d4-uring-wait-close-drain); the round-2
slices added the post-poll control-priority reclassification (D4-RM10), the
non-EINTR poll fail-fast (D4-RM12), the pre-poll participant-uniqueness
barrier, the honest close-LP evidence + source-drift detector, the
genuine-state destruction + BeforeQueueExit order probe (D4-RM11), and the
SQE-pressure case rename. Case counts below are the round-2 numbers.

```sh
xmake build -r uring_backend_c2e_close_drain_test uring_backend_c2e_death_test
xmake run uring_backend_c2e_close_drain_test
# 21/21 PASS (round-2 adds: control-wins-over-co-ready-ring,
#            two-waiter-consumer-strand, non-eintr-poll-failure-failfast;
#            the rewritten close-waits-for-inflight-acceptance-lp makes no
#            mutex-blocking claim)

xmake run uring_backend_c2e_death_test
# 10/10 PASS (round-2 adds: preflight-before-queue-exit-order; the pending/
#             enqueued children now destroy in the GENUINE state via a leaked
#             thread; preflight fires exit 86 before io_uring_queue_exit)

SLUICE_TEST_FILTER=conformance_close_drain_uring xmake run backend_conformance_test
# shared C2e suite for Uring: PASS, [conformance-meta] backend=Uring
# profile=KernelIoProfile mode=real (close_rejects_future_submit,
# close_preserves_accepted_terminal, drain_then_reset_releases_slot,
# slot_released_but_admission_stays_closed)
```

Aggregate gate (real liburing, round-2 head, 2026-08-10): shared /
shared_capacity / c2e_shared_close_drain suites PASS, lifecycle PASS (incl.
`uring_c2e_close_drain` with the full 21-case pin and
`uring_c2e_quiescent_destruction` with the 10-case pin), backend_specific PASS
(`uring_backend_contract` 10-case pin, SQE-pressure case renamed to
`uring_sqe_pressure_retains_accepted_work`), `overall ELIGIBLE` (KernelIo),
RESULT: PASS. Stub build (same round-2 head): full test group builds and runs
clean, Uring `mode=stub` — shared/lifecycle/backend_specific INCOMPLETE,
`overall INCOMPLETE` (aggregate PASS only for the stub-mode run; a stub build
can never make KernelIo ELIGIBLE). Python manifest self-test suite: 376 tests
PASS (`python3 -m unittest discover -v scripts/tests`).

Round-4 confirmation (2026-08-11, PR #84 round-4 head, mutations reverted):

```sh
# P0-1 detector (GREEN):
SLUICE_TEST_FILTER=stop_between_mw_s2_commit_and_backend_wait_registration \
  xmake run application_runtime_drain_starvation_test   # ALL TESTS PASSED
# P0-2 detector (GREEN):
SLUICE_TEST_FILTER=uring_c2e_future_waiter_cannot_steal_old_wake \
  xmake run uring_backend_c2e_close_drain_test          # ALL TESTS PASSED
# P1-1 fixed strand case (GREEN):
SLUICE_TEST_FILTER=uring_c2e_two_waiter_consumer_strand \
  xmake run uring_backend_c2e_close_drain_test          # ALL TESTS PASSED
# Full real Debug suite: 158/158 PASS (xmake test -v, --with-liburing=true)
# Stub suite: 156/156 PASS; stub aggregate PASS with the P1-2 strict rule
#   (expected stub INCOMPLETE only; malformed stub case-set/meta -> FAIL)
# Python manifest self-tests: 379 PASS
#   (incl. new GATE-L8/L9/L10 stub expected-vs-unexpected cases)
```
