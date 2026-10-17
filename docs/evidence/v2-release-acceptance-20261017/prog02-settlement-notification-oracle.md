# PROG-02 io_uring settlement-notification coverage (release gate RG5)

Requirement: root §11 PROG-02 (no-lost-wake), for the io_uring backend.
Ledger gap closed: G6 row "PROG-02 io_uring settlement notification"
(formerly: structural argument only). Recorded at PR head; oracles live in
`tests/uring_progress_race_test.cpp` (23 registered named oracles), with
companion evidence in `tests/uring_owed_residue_test.cpp` and the shutdown
control-pin suite.

## Two notions the table keeps apart

- **Kernel completion readiness** — a CQE is available to userspace (SQ/CQ
  state), so a `io_uring_cq_ready`-class probe can observe it.
- **Discharged settlement obligations** — every borrow/control/publication
  reference the terminal created has been retired and the notification fd
  has been drained back to quiescence by the documented acknowledge
  operation. "Readiness" alone does not imply discharge; oracles that pin
  discharge assert the fd and the backend residue, not only the wake.

## Coverage table — notification timing class → named oracle

| # | Timing class (CQE/notification relative to the wait handshake) | Named oracle(s) | Discipline |
|---|---|---|---|
| 1 | CQE lands before the final drain/probe of the pass | `k1_k2_cqe_before_drain_recovered_by_final_probe`; mutant `progress_source_uring_mut_final_probe_dropped` (kill below) | real ring, deterministic pause-gate staging |
| 2 | Notification written without an epoch change (suppressed/spurious wake) | `k3_notification_without_epoch_mutation_wakes_and_reparks` | real ring, synthetic notification injection |
| 3 | CQE lands after the final actionable-work recheck | `k5_k6_cqe_after_final_recheck_wakes_poll` | real ring, deterministic pause-gate staging |
| 4 | CQE lands while the owner is blocked in `poll(2)` on the eventfd | `k7_cqe_while_blocked_in_poll_wakes_owner` | real ring, deterministic pause-gate staging + real eventfd poll |
| 5 | Several completions land inside one wake window | `k8_multiple_cqes_coalesce_into_one_wake` | real ring, deterministic pause-gate staging |
| 6 | Notification with zero readable CQEs | `k9_spurious_notification_zero_cqes_is_harmless` | real ring, synthetic spurious notification |
| 7 | Control transition racing a kernel CQE, before revalidation | `k10a_control_signal_racing_kernel_cq_before_revalidation` | real ring, userspace control interrupt racing a real kernel CQE, deterministic staging |
| 8 | Control transition racing a kernel CQE, before the poll arm | `k10b_control_signal_racing_kernel_cq_before_poll` | real ring, userspace control interrupt racing a real kernel CQE, deterministic staging |
| 9 | Saturated eventfd with a real CQE pending before drain | `k11a_saturated_eventfd_with_real_cqe_before_drain` | real ring, real saturation injection |
| 10 | Saturation drained, then park, then a fresh CQE | `k11b_drained_saturation_then_park_still_wakes_on_cqe` | real ring, real saturation injection |
| 11 | CQ overflow list holds completions when the wake fires | `k13_cq_overflow_completions_are_not_stranded` | real ring, CQ overflow-list staging, deterministic |
| 12 | Peer-driver admission while parked rejected; owner still recovers the CQ | `k14_peer_drive_while_owner_parked_is_rejected_and_owner_recovers_cq` | real ring, deterministic pause-gate staging |
| 13 | Backend poison transition while owner parked | `poison_transition_wakes_parked_owner`; health propagation `f2_backend_poison_reaches_owner_health_verdict` | real ring, backend-internal poison transition |
| 14 | Retryable submit failure / accepted dispatch lands while parked | `u2_k15_retryable_submit_failure_does_not_strand_owner`; `u2b_accepted_dispatch_wakes_parked_owner` | real ring, deterministic submit-failure injection (fiction hooks) |
| 15 | Publication pass bounded by entry budget, obligations survive | `f1_publication_pass_is_entry_bounded`; residue delivery pinned by `tests/uring_owed_residue_test.cpp` A/B/C | real kernel |
| 16 | Cancellation racing a parked owner, end to end on the real kernel | `cancel_race_with_parked_owner_settles_and_leaves_the_fd_quiescent` (this campaign) | real kernel + prepark freeze |
| 17 | Poisoned-shutdown wake and borrow retention | `shutdown_lifecycle_uring_test` poison scenarios + mutants m5/m9/m11/m12 | real kernel |

Across these, PROG-02's five pre-park conditions are exercised as: (1)
armed mechanism — every oracle; (2) stale-notification acknowledgement
before the final check — k3/k9/k11b; (3) no actionable work after a bounded
pass — k1_k2/k5_k6/f1/owed-residue; (4) no token/control transition between
record and final check — k10a/k10b/cancel-race; (5) work arriving after the
check makes the armed wait return — k5_k6/k7/k8/k13/cancel-race.

## Discrimination (mutant kill)

`正确实现 GREEN → 目标 mutant RED`:

- Correct implementation: `uring_progress_race_test` exits 0 — all 23
  registered oracles pass (post-fix head; runs recorded in the matrix cells
  debug-y/release-y/asanubsan-y/tsan-y and the 25× stress passes in
  `matrix-round0-cancel-race-defect/defect-note.md`).
- Target mutant: the new compile-time mutation
  `SLUICE_PROGRESS_SOURCE_MUTANT_FINAL_PROBE_DROPPED` (registered target
  `progress_source_uring_mut_final_probe_dropped`) removes the final
  physical CQ probe from the pre-park recheck in
  `include/sluice/async/detail/progress_source.hpp`. It is killed by oracle
  #1: the mutant build fails `k1_k2_cqe_before_drain_recovered_by_final_probe`
  (exit 1; the CQE-before-drain case strands the bounded wait to its
  deadline instead of being recovered by the probe). Kill log:
  `matrix/mut-progress_source_uring_mut_final_probe_dropped-run.log` at the
  PR head.

The gate previously existed only as a comment naming M-C1 as killed by
k1_k2; it is now a registered mutation target so the kill is executable
evidence, matching the repo's other named mutation builds (never run as
regular tests).

## What the structural argument alone did not prove, now proved

- A real-kernel cancellation completing against a parked owner settles
  with exactly one physical canceled terminal (never a fabricated
  zero-effect success), retires its control reference, and leaves the
  notification fd genuinely quiescent after the documented drain — the
  full "readiness → discharge" chain (#16).
- The pre-park probe is load-bearing: dropping it is an executable RED,
  not an argument (#1).

No production code changed for RG5: the gate addition in
`progress_source.hpp` is inert unless the mutant macro is defined, and the
macro is defined only by the registered mutant target.
