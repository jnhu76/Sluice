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

### Layer S — Central Claim

| Witness cfg | Predicate | Premise proved reachable |
|-------------|-----------|--------------------------|
| `central_tiebreak` | `NotReachCentralTieBreak` | Multi-candidate admission tie-break. |
| `central_claimed` | `NotReachCentralClaimed` | An offer was claimed (J premise). |
| `central_candidates` | `NotReachCentralClaimCandidatesLatched` | `claim_candidates` latched at claim time. |
| `central_admission_tie` | `NotReachCentralAdmissionTie` | Multi-candidate Inline admission tie. |
| `central_winner_classified` | `NotReachCentralWinnerClassified` | Winner and Loser classified. |

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

## Verification

All 20 witnesses report `Invariant NotReach_<X> is violated`.  Run
`tools/formal/verify-e13-select-safety.sh` and look for the `REACH <label>`
lines under the "Per-law non-vacuity witnesses (W)" section.

## Relationship to focused negative models

The non-vacuity witnesses and the focused negative models (see
NEGATIVE_MODELS.md) are complementary:

- A non-vacuity witness proves the law's premise is reachable in the
  *canonical* model.
- A focused negative model proves that if the law's conclusion is
  deliberately broken, TLC catches it.

Together they show each law is both *reachable* and *load-bearing*.
