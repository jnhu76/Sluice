# E13 Select — Layered Safety Invariants (PR #18)

This document enumerates the named layered safety invariants introduced in
PR #18 on top of the PR #17 layered formal core.  Each invariant is named,
scoped, and verified by TLC on a bounded domain; each is also accompanied by
a focused negative model that proves the invariant is *load-bearing*
(disabling it produces a TLC violation), and by a per-law non-vacuity
witness that proves the invariant's premise is reachable.

The three layers remain stable:

```text
E13SelectContract.tla       (Layer C) -- ContractSafetyInv (H1-H5 + I)
        ^ refinement
E13SelectCentralClaim.tla   (Layer S) -- CentralSafetyInv (J)
        ^ refinement
E13SelectEventTimer.tla     (Layer A) -- AdapterSafetyInv (K + L + M + N)
```

The PR #17 canonical aggregates (`ContractInv`, `CentralInv`,
`EventTimerInv`) are kept unchanged side-by-side with the new
`*SafetyInv` aggregates, so each PR #17 metric still reproduces exactly.
The new aggregates live in their own operator names so a regression in the
canonical model cannot be silently masked by an unrelated strengthening.

---

## Layer C — `ContractSafetyInv` (32 laws)

### H1 — winner uniqueness and linearization order

| Law | Statement |
|-----|-----------|
| `C_InvAtMostOneLinearizedWinner` | At most one arm is the linearized winner. |
| `C_InvAtMostOneCommittedWinner` | At most one arm has `arm_resolution = "WinnerCommitted"`. |
| `C_InvCommitRequiresWinnerLinearization` | `arm_resolution[i]="WinnerCommitted" => winner = i` (commit may only record the linearized winner, in any terminal phase). |

### H2 — losers never produce irreversible effects

| Law | Statement |
|-----|-----------|
| `C_InvLoserNeverPublishesResult` | A non-winner arm has `arm_publication_count = 0`. |
| `C_InvLoserNeverPublishesRunnable` | A non-winner arm contributes zero to runnable publication. |
| `C_InvLoserNeverCommitsIrreversibleEffect` | A non-winner never reaches `WinnerCommitted`. |

### H3 — publication bounds

| Law | Statement |
|-----|-----------|
| `C_InvAtMostOneResultPublication` | `result_publication_count <= 1`. |
| `C_InvAtMostOneRunnablePublication` | `runnable_publication_count <= 1`. |
| `C_InvRunnablePublicationRequiresPriorWaiting` | `runnable_publication_count = 1 => completion_mode = "Suspended"`. |
| `C_InvOnlyWinnerPublishes` | `arm_publication_count[i] > 0 => winner = i`. |

### H4 — completion and destruction

| Law | Statement |
|-----|-----------|
| `C_InvCompletionRequiresWinnerCommitted` | `Completed`/`Consumed` requires `winner \in Arms /\ arm_resolution[winner]="WinnerCommitted"`. |
| `C_InvCompletionRequiresAllLosersAborted` | Completion requires every non-winner to be `Aborted`/`Released`. |
| `C_InvCompletionRequiresAllAuthorityClosed` | Completion requires every registered arm's authority closed. |
| `C_InvDestroyRequiresConsumedOrValidAbort` | `Destroyed` requires `Consumed` or a valid `Aborted` terminal. |
| `C_InvDestroyedHasNoOpenAuthority` | `Destroyed` has no open authority. |

### H5 — reversible reservation

| Law | Statement |
|-----|-----------|
| `C_InvReservationCommitRequiresWinner` | Reservation `Committed` only on the winner. |
| `C_InvReservationClosesExactlyOnce` | `reservation_close_count[i] <= 1`. |
| `C_InvReservationIsNotIrreversibleCommit` | Reservation `Committed` is distinct from winner commit. |
| `C_InvLoserReservationReleased` | In terminal phase, a loser's reservation is `Released` (transient `Held` post-linearization is allowed). |

### I — registration rollback domain

| Law | Statement |
|-----|-----------|
| `C_InvRegistrationRollbackOnlyBeforeSuspension` | Rollback requires `contract_phase = "Building"`. |
| `C_InvRegistrationRollbackRequiresNoWinner` | Rollback requires `winner = NoArm`. |
| `C_InvRegistrationRollbackRequiresRunningCaller` | Rollback/Aborted require `caller_state = "Running"`. |
| `C_InvAbortedCallerNeverWaiting` | Aborted forbids `caller_state = "Waiting"`. |
| `C_InvDestroyedCallerNeverWaiting` | Destroyed forbids `caller_state = "Waiting"`. |
| `C_InvRollbackNeverPublishes` | Rollback terminal has `result_publication_count = 0 /\ runnable_publication_count = 0`. |
| `C_InvRollbackClosesEveryRegisteredAuthority` | Aborted has no open authority. |

Negative models covering these laws: `NEG-C1..C8` (see NEGATIVE_MODELS.md).

---

## Layer S — `CentralSafetyInv` (14 laws)

### J — Central Claim strategy safety

| Law | Statement |
|-----|-----------|
| `S_InvAtMostOneSuccessfulClaim` | `central_phase` reaches Claimed at most once. |
| `S_InvClaimRequiresOfferedArm` | A claim requires a candidate-ready offered arm. |
| `S_InvClaimSnapshotContainsWinner` | `winner \in claim_candidates`. |
| `S_InvClaimBeforeAdapterCommit` | `winner \in Arms /\ arm_resolution[winner]="WinnerCommitted" => arm_class[winner]="Winner"`. |
| `S_InvAdmissionTieUsesLowestIndex` | In Inline mode with `Card(claim_candidates) >= 2`, `winner` is the lowest index. |
| `S_InvRollbackDomainRefinesContract` | Central rollback obeys Contract rollback domain. |
| `S_InvRollbackDisabledAfterArmed` | `central_phase="Armed" => rollback disabled`. |
| `S_InvRollbackDisabledAfterClaim` | `central_phase` past Claimed forbids rollback. |

Negative models: `NEG-S1..S6`.

---

## Layer A — `AdapterSafetyInv` (K + L + M + N)

### K — Event broadcast laws

| Law | Statement |
|-----|-----------|
| `E_InvEventArmHasValidEventIdentity` | `arm_kind[i]="EventArm" => arm_event[i] \in Events`. |
| `E_InvBroadcastScansOnlyTargetEventArms` | During scan, every scanned arm belongs to the held Event. |
| `E_InvSameEventArmsShareIdentity` | Two Event arms sharing an identity have that identity in `Events`. |
| `E_InvPhaseOneOnlyOffersCandidates` | `scan_phase="Scanning" => central_phase="Armed"`. |
| `E_InvPhaseOneDoesNotClaim` | `scan_phase="Scanning" => central_phase # "Claimed"`. |
| `E_InvPhaseOneDoesNotCommit` | `scan_phase="Scanning" => winner=NoArm \/ arm_resolution[winner] # "WinnerCommitted"`. |
| `E_InvPhaseOneDoesNotPublish` | `scan_phase="Scanning" => result_publication_count = 0`. |
| `E_InvPhaseTwoStartsAfterScanAuthorityReleased` | `scan_phase="ProcessGroups" => held_event = NoEvent`. |
| `E_InvNoReadinessBypassesPermittedEventPath` | `candidate_ready[i] => arm_registered[i] /\ adapter_phase[i] \in {"Registered","TimerCancelled","Finalized","Retired"}`. |
| `E_InvNoRecursiveEventQueueAuthority` | At most one Event broadcast in flight. |
| `E_InvEventPersistentSetNotConsumed` | `e = last_broadcast_event => event_state[e]="Set"`. |
| `E_InvWinnerTerminalDetachedBeforeAuthorityClose` | Retired winner has terminal wait_outcome and unlinked node. |
| `E_InvLoserTerminalDetachedBeforeAuthorityClose` | Retired loser has terminal wait_outcome and unlinked node. |

### L — Timer registration laws

| Law | Statement |
|-----|-----------|
| `T_InvNoDereferenceWithoutActiveAuthority` | `timer_due /\ TimerArm /\ Registered => timer_state \in {"Active","Consumed","Retired"}`. |
| `T_InvRetiredRegistrationNeverDereferences` | `timer_state[i]="Retired" => timer_node_deref[i] \in {0,1}`. |
| `T_InvConsumedRegistrationNeverDereferences` | `timer_state[i]="Consumed" => timer_node_deref[i] \in {0,1}`. |
| `T_InvActiveStableDuringPumpAuthority` | `coordination_kind="Timer" /\ coordination_arm \in Arms => timer_state[coordination_arm] \in {"Active","Consumed"}`. |
| `T_InvDueObservationDoesNotConsume` | Admission observation of a due Timer does not consume it. |
| `T_InvConsumeRequiresWinner` | `timer_state[i]="Consumed" => arm_class[i]="Winner" /\ winner=i`. |
| `T_InvRetireRequiresLoserOrRollback` | `timer_state[i]="Retired" /\ arm_registered[i] => arm_class[i]="Loser" \/ finalization_step[i]="Rollback"`. |
| `T_InvTimerTransitionExactlyOnce` | Consumed/Retired records a unique `timer_transition_step`. |
| `T_InvStalePumpChecksState` | `timer_skip_observed[i] => timer_state[i] \in {"Retired","Consumed"}`. |
| `T_InvStalePumpSkipsWithoutDereference` | `timer_skip_observed[i] => timer_node_deref[i] \in {0,1}`. |
| `T_InvStalePumpNeverOffers` | `timer_skip_observed /\ Retired => Loser/Unclassified + TimerLoser/Rollback`. |
| `T_InvStalePumpNeverPublishes` | `timer_skip_observed /\ Retired => arm_publication_count = 0`. |

### M — per-arm exactly-once accounting

| Law | Statement |
|-----|-----------|
| `A_InvWaitAccountOpensAtMostOnce` | `wait_account_open_count[i] <= 1`. |
| `A_InvWaitAccountClosesAtMostOnce` | `wait_account_close_count[i] <= 1`. |
| `A_InvRegisteredArmClosesWaitAccount` | A registered arm's wait account is eventually closed. |
| `A_InvTimerAccountOpensAtMostOnce` | `timer_account_open_count[i] <= 1`. |
| `A_InvTimerAccountClosesAtMostOnce` | `timer_account_close_count[i] <= 1`. |
| `A_InvTimerConsumeOrRetireClosesExactlyOnce` | Finalized/Retired timer has `timer_account_close_count = 1`. |
| `A_InvNoAccountingUnderflow` | `close_count <= open_count` for every account kind. |
| `A_InvCompletedHasNoOpenAccounting` | Completed has no open wait/timer account. |
| `A_InvDestroyedHasNoOpenAccounting` | Destroyed has no open wait/timer account. |

### N — step-indexed operation/arm/epoch-aware history

| Law | Statement |
|-----|-----------|
| `N_InvCommitFollowsWinnerLinearization` | `commit_step[i] # NoStep => winner_linearization_step # NoStep /\ winner_linearization_step < commit_step[i]`. |
| `N_InvTimerTransitionFollowsLinearization` | Timer Consumed/TimerWinner/TimerLoser transitions follow winner linearization. |
| `N_InvAuthorityCloseFollowsTerminal` | `authority_close_step # NoStep => terminal_step # NoStep`. |
| `N_InvPublicationFollowsAuthorityClose` | `publication_step # NoStep => authority_close_step # NoStep`. |
| `N_InvPublicationFollowsAllAccountClose` | `publication_step # NoStep => every registered arm's account is closed`. |

Negative models: `NEG-E1..E6`, `NEG-T1..T5`, `NEG-A1..A4`.

---

## Verification

Every law above is verified by one of:

1. **Positive aggregate** (`*SafetyInv` cfg) — TLC must reach
   "Model checking completed. No error has been found".
2. **Focused negative model** — TLC must report
   "Invariant <LawName> is violated" under the FAULT selected for that law.
3. **FAULT="None" restoration** — TLC must reach "completed, no error"
   once the fault is disabled.

See NEGATIVE_MODELS.md for the focused negative matrix, NON_VACUITY.md for
the per-law reachability witness matrix, and run
`tools/formal/verify-e13-select-safety.sh` for the reproducible gate.
