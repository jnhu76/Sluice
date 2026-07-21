# E13 Select — Focused Negative Models (PR #18)

This document describes the focused negative model matrix added in PR #18.
Each negative model is a single-fault mutation of the canonical spec that
proves a named layered safety law is *load-bearing*: disabling the law
produces a TLC "Invariant <LawName> is violated" report on a reachable
counterexample.

The matrix covers 33 faults across the four layers:

```text
Contract (R)  -- NEG-C1..C9   -- 9 faults, E13SelectContractNeg.tla
Central  (S)  -- NEG-S1..S7   -- 7 faults, E13SelectCentralClaimNeg.tla
Adapter  (E)  -- NEG-E1..E6   -- 6 Event-broadcast faults
           (T) -- NEG-T1..T5  -- 5 Timer-registration faults
           (A) -- NEG-A1..A4  -- 4 accounting faults
                              -- 15 faults, E13SelectEventTimerNeg.tla
Multi-group (MG) -- NEG-MG1   -- 1 cross-group rollback fault,
                                 E13SelectMultiGroupNeg.tla
```

Plus four FAULT="None" restoration configs proving the canonical model
still passes once each fault family is disabled.

## PR #18 corrective-1 additions

Three new faults were added to cover laws that PR #18's initial submission
asserted but did not isolate in a focused negative model:

- **`NEG-C9`** isolates `C_InvWinnerIdentityStableAfterLinearization` (the
  frozen-winner identity law added in corrective-1 P1-4).  A legal
  linearization has stamped `linearized_winner = A`; the fault flips the
  live `winner` to a different registered arm B ≠ A while no arm is
  `WinnerCommitted` and the frozen history is left unchanged.
- **`NEG-S7`** isolates `S_InvClaimSnapshotImmutableAfterClaim` in its
  hardest case (addition, not removal).  Requires a 3-arm cfg so a winner,
  a snapshot-internal candidate, and a snapshot-external registered arm can
  coexist.  The fault adds the external arm to `claim_candidates` while
  leaving the frozen snapshot and the winner untouched.
- **`NEG-MG1`** isolates the transition-level cross-group non-interference
  that the per-action UNCHANGED audit establishes structurally.  Group g0's
  `RollbackArm` spuriously mutates group g1's `authority_open` while group
  g1 is at a terminal phase; the post-fault state is caught by
  `MG_InvAuthorityClosureDoesNotCrossGroups`.

In addition, **`NEG-S2`** was retargeted to `S_InvClaimSnapshotImmutableAfterClaim`
(the strict frozen-equality law added in corrective-1 P1-3).  The original
`S_InvWinnerChosenFromSnapshot` target was strengthened to depend on
`claim_snapshot_frozen_valid`, which S2 does not set; the live-snapshot
mutation now isolates the strict immutability law instead.

The adapter accounting faults `NEG-A1`/`NEG-A2` are accompanied by a
double-check in `verify-e13-select-safety.sh` (`expect_negative_no_typeok`)
that asserts the focused fault isolates the at-most-once law WITHOUT
tripping `EventTimerTypeOK`.  This is enabled by the corrective-1 widening
of the accounting counter TypeOK domain from `0..1` to `0..2`: a fault can
now push a counter to 2 (the value the law forbids) without first breaking
the typing precondition.

---

## Architecture (INSTANCE-WITH wrapper)

Every negative module wraps the canonical spec by INSTANCE-WITH, binding
each canonical variable to a same-named wrapper variable.  The canonical
`*Next` is reused unchanged via a frozen `BaseNextFrozen`:

```tla
Base == INSTANCE E13SelectContract WITH contract_phase <- contract_phase, ...

FaultActive(name) == FAULT = name

CNegNext ==
    \/ CNegStutter                       \* UNCHANGED CNegVars
    \/ BaseNextFrozen                    \* Base!ContractNext /\ UNCHANGED fault_used
    \/ FaultNext                         \* \/ Fault_C1 \/ Fault_C2 \/ ...

CNegSpec == CNegInit /\ [][CNegNext]_CNegVars
```

Rules:

1. **One focused fault per negative model.**  Each cfg selects a single
   `FAULT` constant and asserts the matching named invariant.
2. **The canonical `*Spec` / `*Inv` are NOT modified.**  All faults live in
   the wrapper module only.
3. **Every fault action is reachable from a legal state.**  A fault whose
   guard is unreachable would be a vacuous test; reachability is verified
   by `expect_negative` (TLC must find a counterexample, not deadlock or
   pass vacuously).
4. **The expected target invariant fails when the fault is enabled.**  TLC
   must report `Invariant <LawName> is violated`, not a different invariant
   nor a temporal property violation.
5. **Fault disabled (`FAULT = "None"`) restores PASS.**  Each restore cfg
   asserts the canonical aggregate and must reach "completed, no error".
6. **No ASSUME FALSE, no Init crippling, no state constraint hiding.**

---

## Contract negative models — `E13SelectContractNeg.tla`

| Cfg | Target invariant | Fault (what the mutation does) |
|-----|------------------|--------------------------------|
| `NEG-C1` | `C_InvCommitRequiresWinnerLinearization` | Commits an arm (`WinnerCommitted`) while `winner = NoArm` (no linearization yet). |
| `NEG-C2` | `C_InvAtMostOneCommittedWinner` | Commits a second arm `WinnerCommitted` alongside an existing committed winner. |
| `NEG-C3` | `C_InvLoserNeverPublishesResult` | A Released (loser) arm publishes a result. |
| `NEG-C4` | `C_InvCompletionRequiresAllAuthorityClosed` | Reaches Completed while an arm's authority is still open. |
| `NEG-C5` | `C_InvLoserReservationReleased` | A non-winner arm at terminal success keeps `reservation_state = Held`. |
| `NEG-C6` | `C_InvRegistrationRollbackRequiresRunningCaller` | Begins rollback from a Waiting caller (PR #17 P0-1 regression). |
| `NEG-C7` | `C_InvAbortedCallerNeverWaiting` | Forces the caller to Waiting from a legal Aborted terminal (PR #17 P0-1). |
| `NEG-C8` | `C_InvDestroyRequiresConsumedOrValidAbort` | Destroys from Completed+Inline while resetting `completion_mode` to None. |
| `NEG-C9` | `C_InvWinnerIdentityStableAfterLinearization` | Corrective-1 P1-4: flips live `winner` to B ≠ A while `linearized_winner = A` and no arm is `WinnerCommitted`. |

Restore cfg: `E13SelectContractNeg.restore.cfg` (FAULT="None") asserts
`ContractSafetyInv`.

---

## Central Claim negative models — `E13SelectCentralClaimNeg.tla`

| Cfg | Target invariant | Fault |
|-----|------------------|-------|
| `NEG-S1` | `S_InvClaimRequiresOfferedArm` | Claims a non-offered (not `candidate_ready`) arm. |
| `NEG-S2` | `S_InvClaimSnapshotImmutableAfterClaim` | Corrective-1 retarget: removes the winner from `claim_candidates` post-claim while leaving `claim_snapshot_frozen` untouched. |
| `NEG-S3` | `S_InvAtMostOneSuccessfulClaim` | A second arm is classified `Winner` alongside an existing winner. |
| `NEG-S4` | `S_InvClaimBeforeAdapterCommit` | Commits the winner before its `arm_class` is set to "Winner". |
| `NEG-S5` | `S_InvAdmissionTieUsesLowestIndex` | In an admission tie, claims a non-lowest-index arm. |
| `NEG-S6` | `S_InvClaimSnapshotContainsWinner` | Linearizes a winner that is not in `claim_candidates` (single-group reduction of cross-group contamination). |
| `NEG-S7` | `S_InvClaimSnapshotImmutableAfterClaim` | Corrective-1 P1-3 (3-arm cfg): adds a snapshot-external registered arm to `claim_candidates` while leaving the frozen snapshot and the winner untouched. |

Restore cfg: `E13SelectCentralClaimNeg.restore.cfg` (FAULT="None") asserts
`CentralSafetyInv`.

---

## Event/Timer/Accounting negative models — `E13SelectEventTimerNeg.tla`

### E — Event broadcast faults

| Cfg | Target invariant | Fault |
|-----|------------------|-------|
| `NEG-E1` | `E_InvNoReadinessBypassesPermittedEventPath` | Promotes a Registering arm to candidate_ready, bypassing admission/scan. |
| `NEG-E2` | `E_InvBroadcastScansOnlyTargetEventArms` | Adds an arm of another Event to the scan set. |
| `NEG-E3` | `E_InvPhaseOneDoesNotCommit` | Forges + commits a winner during Scanning. |
| `NEG-E4` | `E_InvEventPersistentSetNotConsumed` | Resets event_state to Unset for an already-broadcast Event. |
| `NEG-E5` | `E_InvPhaseOneDoesNotPublish` | Publishes a result during Scanning. |
| `NEG-E6` | `E_InvOnlyWinnerPublishes` | A Timer arm publishes via an Event broadcast (cross-coordination contamination); targets the Contract-layer law via the two-step refinement chain `Base!CentralRefinement!ContractRefinement!C_InvOnlyWinnerPublishes`. |

### T — Timer registration faults

| Cfg | Target invariant | Fault |
|-----|------------------|-------|
| `NEG-T1` | `T_InvRetiredRegistrationNeverDereferences` | Forces `timer_node_deref = 2` on a Retired registration. |
| `NEG-T2` | `T_InvConsumeRequiresWinner` | Consumes a Timer registration while `winner = NoArm`. |
| `NEG-T3` | `T_InvRetireRequiresLoserOrRollback` | Retires a Timer that is neither Loser nor Rollback terminal. |
| `NEG-T4` | `T_InvActiveStableDuringPumpAuthority` | Retires the live pump-authority arm mid-coordination. |
| `NEG-T5` | `T_InvStalePumpNeverPublishes` | Publishes on a Retired arm reached via stale-pump skip. |

### A — Accounting faults

| Cfg | Target invariant | Fault |
|-----|------------------|-------|
| `NEG-A1` | `A_InvWaitAccountClosesAtMostOnce` | Pushes `wait_account_close_count[i]` from 1 to 2.  Corrective-1: counter TypeOK domain widened to 0..2 so this isolates the law without tripping `EventTimerTypeOK`. |
| `NEG-A2` | `A_InvTimerAccountClosesAtMostOnce` | Pushes `timer_account_close_count[i]` from 1 to 2.  Same widened-TypeOK isolation as NEG-A1. |
| `NEG-A3` | `A_InvCompletedHasNoOpenAccounting` | Reaches Completed with an open account. |
| `NEG-A4` | `A_InvNoAccountingUnderflow` | Decrements a close count past its open count. |

Restore cfg: `E13SelectEventTimerNeg.restore.cfg` (FAULT="None") asserts
`AdapterSafetyInv` (full 33-law aggregate).

The verifier additionally runs `NEG-A1`/`NEG-A2` through
`expect_negative_no_typeok`, which requires the TLC output to contain NO
`EventTimerTypeOK is violated` line.  This double-check confirms the fault
isolates exactly the at-most-once law rather than piggy-backing on a typing
precondition.

---

## Multi-group negative models — `E13SelectMultiGroupNeg.tla`

| Cfg | Target invariant | Fault |
|-----|------------------|-------|
| `NEG-MG1` | `MG_InvAuthorityClosureDoesNotCrossGroups` | Corrective-1: group g0's `RollbackArm` spuriously mutates group g1's `authority_open` while group g1 has independently reached a terminal phase with correctly closed authority. |

Restore cfg: `E13SelectMultiGroupNeg.restore.cfg` (FAULT="None") asserts
`MGSafetyInv`.

---

## Implementation notes

- **UNCHANGED conflicts.** A fault that primes variable `X` cannot list
  `ETAdapterVars` (or any tuple containing `X`) in its UNCHANGED clause;
  TLC silently picks one.  Each fault lists its frozen variables
  individually in that case.
- **`E_InvOnlyWinnerPublishes` reachability.** The adapter module does not
  re-assert the Contract's winner-only publication law (it is part of
  `ContractSafetyInv`).  NEG-E6 reaches it via
  `Base!CentralRefinement!ContractRefinement!C_InvOnlyWinnerPublishes` so a
  single cfg can name a single invariant.
- **`E_InvEventPersistentSetNotConsumed`** was a placeholder `TRUE` in
  PR #17.  PR #18 strengthens it to a real state law
  (`e = last_broadcast_event => event_state[e]="Set"`) so NEG-E4 can target
  it without vacuity.  The canonical safety + safety3mix configs still PASS
  unchanged on the strengthened law.
- **Reachability of Retired+ Consumed Timer arms.**  In the 2-arm bounded
  model the canonical Retired+Timer deref window stays in {0, 1}; NEG-T1
  forces `timer_node_deref = 2` directly so the violation is deterministic
  on every Retired reachable state.

---

## Verification

Run `tools/formal/verify-e13-select-safety.sh`.  Every `expect_negative`
line must print `NEG <label> (<LawName> violated; ...)`, and every
`expect_restored` line must print `RESTORE <label> (FAULT="None" PASS; ...)`.
