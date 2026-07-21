# E13 Select — Non-Vacuity Witness Matrix (PR #18)

A safety invariant is *vacuous* if its premise is unreachable in the model:
the law holds trivially because the state it constrains never arises.  PR #18
ships a per-law non-vacuity witness matrix that proves each named layered
safety law has a reachable premise, so every PASS on the positive aggregate
is meaningful.

## Method

For each named law we define an inverse-reachability predicate
`NotReach_<X> == ~Reach_<X>` where `Reach_<X>` is a non-trivial state
configuration the law actually constrains.  A TLC run with
`INVARIANT NotReach_<X>` reports:

- `Invariant NotReach_<X> is violated` — the law's premise is reachable
  (the law is not vacuous).  This is the desired witness.
- `Model checking completed. No error has been found` — the premise is
  unreachable (the law is vacuous; needs redesign).

Each witness is a single named cfg so the result is unambiguous (a single
`INVARIANT` line per cfg means TLC stops at the first violation).

## Matrix

### Layer C — Contract

| Witness cfg | Predicate | Premise proved reachable |
|-------------|-----------|--------------------------|
| `nv_inline` | `NotReachContractInline` | Completed inline (R11 anchor). |
| `nv_reservation` | `NotReachContractReservationSuspended` | Suspended reservation completion. |
| `nv_commitwinner` | `NotReachContractCommittedWinner` | A committed winner exists (H2 premise). |
| `nv_loser` | `NotReachContractLoserExists` | A classified loser exists (H2 premise). |
| `nv_armpub` | `NotReachContractArmPublished` | At least one arm published (H3 premise). |
| `nv_completed` | `NotReachContractCompleted` | Completed phase reached (H4 premise). |
| `nv_destroyed` | `NotReachContractDestroyed` | Destroyed phase reached (H4 premise). |
| `nv_aborted` | `NotReachContractAborted` | Aborted phase reached (R9 premise). |
| `nv_rollback` | `NotReachContractRollback` | Rollback phase reached (R9 premise). |
| `nv_lin_not_committed` | `NotReachContractWinnerLinearizedNotCommitted` | Corrective-1 P1-4: a winner is linearized but not yet committed (frozen-winner identity window). |

### Layer S — Central Claim

| Witness cfg | Predicate | Premise proved reachable |
|-------------|-----------|--------------------------|
| `central_tiebreak` | `NotReachCentralTieBreak` | Multi-candidate admission tie-break. |
| `central_claimed` | `NotReachCentralClaimed` | An offer was claimed (J premise). |
| `central_candidates` | `NotReachCentralClaimCandidatesLatched` | `claim_candidates` latched at claim time. |
| `central_admission_tie` | `NotReachCentralAdmissionTie` | Multi-candidate Inline admission tie. |
| `central_winner_classified` | `NotReachCentralWinnerClassified` | Winner and Loser classified. |
| `nv_frozen_snapshot` | `NotReachCentralFrozenSnapshotValid` | Corrective-1 P1-3: the frozen snapshot was stamped (a claim happened). |
| `nv_multi_candidate_snapshot` | `NotReachCentralMultiCandidateSnapshot` | Corrective-1 P1-3: the frozen snapshot contained >= 2 candidates (non-trivial selection). |

### Layer A — Adapter

PR #17's `scene_*.cfg` files (R1-R12) already cover the adapter's
causal reachability.  PR #18 adds witnesses for the new accounting and
history laws:

| Witness cfg | Predicate | Premise proved reachable |
|-------------|-----------|--------------------------|
| `adapter_wait_closed` | `NotReachAdapterWaitAccountClosed` | A wait account was closed (M premise). |
| `adapter_timer_closed` | `NotReachAdapterTimerAccountClosed` | A timer account was closed (M premise). |
| `adapter_retired` | `NotReachAdapterRetiredTimerExists` | A Retired timer exists (L premise). |
| `adapter_consumed` | `NotReachAdapterConsumedTimerExists` | A Consumed timer exists (L premise). |
| `adapter_commit_step` | `NotReachAdapterCommitStepRecorded` | A `commit_step` was recorded (N premise). |
| `adapter_publication_step` | `NotReachAdapterPublicationStepRecorded` | A `publication_step` was recorded (N premise). |
| `adapter_account_count_one` | `NotReachAdapterAccountCountOne` | Corrective-1: at least one arm reached the legal counter value 1 (the value the at-most-once laws constrain, post TypeOK widening to 0..2). |

### Layer MG — Multi-group

| Witness cfg | Predicate | Premise proved reachable |
|-------------|-----------|--------------------------|
| `reach_shared_event` | `NotMG_ReachSharedEventBothGroupsComplete` | Corrective-1: two groups complete via a single shared Event identity. |
| `reach_mixed_event_timer` | `NotMG_ReachMixedEventTimer` | Corrective-1: one group completes via an Event arm, another via a Timer arm. |
| `reach_rollback_vs_complete` | `NotMG_ReachOneRollbackOtherComplete` | Corrective-1: one group reaches Aborted terminal via rollback, the other completes successfully. |
| `nv_installed_not_finished` | `NotMG_ReachArmInstalledNotFinished` | Corrective-1 P1-1: an arm is installed (kind/identity/wait/authority/account) but `g_phase` is still `Registering` (pre-`FinishRegistration`). |
| `nv_pre_finish_rollback` | `NotMG_ReachPreFinishRollback` | Corrective-1 P1-1: a registration rollback is reachable from the pre-`FinishRegistration` state. |

## Verification

All 28 witnesses report `Invariant NotReach_<X> is violated`.  Run
`tools/formal/verify-e13-select-safety.sh` and look for the `REACH <label>`
lines under the "Per-law non-vacuity witnesses (W)" section (Layer C/S/A)
and the "Multi-group bounded non-interference (O)" section (Layer MG).

## Relationship to focused negative models

The non-vacuity witnesses and the focused negative models (see
NEGATIVE_MODELS.md) are complementary:

- A non-vacuity witness proves the law's premise is reachable in the
  *canonical* model.
- A focused negative model proves that if the law's conclusion is
  deliberately broken, TLC catches it.

Together they show each law is both *reachable* and *load-bearing*.
