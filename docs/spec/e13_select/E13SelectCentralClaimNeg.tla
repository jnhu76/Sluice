----------------------- MODULE E13SelectCentralClaimNeg -----------------------
(*
  PR #18 focused Central Claim negative models (S: NEG-S1..NEG-S6).

  Wraps the canonical E13SelectCentralClaim by INSTANCE-WITH and adds ONE
  focused fault per FAULT selector.  Same architecture as E13SelectContractNeg.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Arms, MaxArms, FAULT

NoArm == 1000000

VARIABLES
    contract_phase, arm_registered, readiness_evidence, reservation_state,
    arm_resolution, authority_open, winner, caller_state, completion_mode,
    result_publication_count, runnable_publication_count,
    arm_publication_count, reservation_close_count,
    linearized_winner, linearized_winner_valid,
    central_phase, candidate_ready, claim_candidates, arm_class, claim_mode,
    claim_snapshot_frozen, claim_snapshot_frozen_valid,
    fault_used

Base == INSTANCE E13SelectCentralClaim
        WITH Arms <- Arms, MaxArms <- MaxArms,
             contract_phase <- contract_phase,
             arm_registered <- arm_registered,
             readiness_evidence <- readiness_evidence,
             reservation_state <- reservation_state,
             arm_resolution <- arm_resolution,
             authority_open <- authority_open,
             winner <- winner,
             caller_state <- caller_state,
             completion_mode <- completion_mode,
             result_publication_count <- result_publication_count,
             runnable_publication_count <- runnable_publication_count,
             arm_publication_count <- arm_publication_count,
             reservation_close_count <- reservation_close_count,
             linearized_winner <- linearized_winner,
             linearized_winner_valid <- linearized_winner_valid,
             central_phase <- central_phase,
             candidate_ready <- candidate_ready,
             claim_candidates <- claim_candidates,
             arm_class <- arm_class,
             claim_mode <- claim_mode,
             claim_snapshot_frozen <- claim_snapshot_frozen,
             claim_snapshot_frozen_valid <- claim_snapshot_frozen_valid

SNegVars ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      arm_resolution, authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count,
      linearized_winner, linearized_winner_valid, central_phase,
      candidate_ready, claim_candidates, arm_class, claim_mode,
      claim_snapshot_frozen, claim_snapshot_frozen_valid, fault_used>>

SNegInit ==
    /\ Base!CentralInit
    /\ fault_used = FALSE

FaultActive(name) == FAULT = name

\* ----- NEG-S1: claim a non-offered arm -----------------------------------
\* A Central claim assigns Winner to an arm that is not candidate_ready.
Fault_S1 ==
    /\ FaultActive("S1")
    /\ ~fault_used
    /\ central_phase \in {"Admission", "Armed"}
    /\ \E i \in Arms :
          /\ arm_registered[i]
          /\ ~candidate_ready[i]
          /\ winner' = i
          /\ claim_candidates' = claim_candidates
          /\ arm_class' = [j \in Arms |->
               IF j = i THEN "Winner"
               ELSE IF arm_registered[j] THEN "Loser"
               ELSE "Unclassified"]
          /\ claim_mode' =
               IF central_phase = "Admission" THEN "Inline" ELSE "Suspended"
          /\ central_phase' = "Claimed"
          /\ contract_phase' = "WinnerLinearized"
    /\ fault_used' = TRUE
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count, candidate_ready,
                    linearized_winner, linearized_winner_valid,
                    claim_snapshot_frozen, claim_snapshot_frozen_valid>>

\* ----- NEG-S2: mutate claim snapshot after claim -------------------------
\* After a claim, the snapshot is mutated to REMOVE the winner from
\* claim_candidates (claim snapshot no longer contains the winner).
Fault_S2 ==
    /\ FaultActive("S2")
    /\ ~fault_used
    /\ winner \in Arms
    /\ claim_candidates' = claim_candidates \ {winner}
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open, winner,
                    caller_state, completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count, central_phase, candidate_ready,
                    arm_class, claim_mode,
                    linearized_winner, linearized_winner_valid,
                    claim_snapshot_frozen, claim_snapshot_frozen_valid>>

\* ----- NEG-S3: two successful group claims -------------------------------
\* Two arms are classified Winner.
Fault_S3 ==
    /\ FaultActive("S3")
    /\ ~fault_used
    /\ winner \in Arms
    /\ \E i \in Arms :
          /\ i # winner
          /\ arm_registered[i]
          /\ arm_class' = [arm_class EXCEPT ![i] = "Winner"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open, winner,
                    caller_state, completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count, central_phase, candidate_ready,
                    claim_candidates, claim_mode,
                    linearized_winner, linearized_winner_valid,
                    claim_snapshot_frozen, claim_snapshot_frozen_valid>>

\* ----- NEG-S4: adapter commit before group claim -------------------------
\* An arm is linearized AND committed while central_phase is still Admission/
\* Armed (no claim yet, so arm_class is Unclassified).  The target invariant
\* S_InvClaimBeforeAdapterCommit requires arm_class[winner] = Winner for a
\* committed winner; here it is Unclassified.
Fault_S4 ==
    /\ FaultActive("S4")
    /\ ~fault_used
    /\ central_phase \in {"Admission", "Armed"}
    /\ contract_phase = "Selecting"
    /\ \E i \in Arms :
          /\ arm_registered[i]
          /\ arm_resolution[i] = "Open"
          /\ arm_class[i] = "Unclassified"
          /\ winner' = i
          /\ arm_resolution' = [arm_resolution EXCEPT ![i] = "WinnerCommitted"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, authority_open, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count, central_phase, candidate_ready,
                    claim_candidates, arm_class, claim_mode,
                    linearized_winner, linearized_winner_valid,
                    claim_snapshot_frozen, claim_snapshot_frozen_valid>>

\* ----- NEG-S5: wrong lowest-index admission winner -----------------------
\* Admission tie with >=2 candidates; fault picks a non-lowest winner.
Fault_S5 ==
    /\ FaultActive("S5")
    /\ ~fault_used
    /\ central_phase = "Admission"
    /\ Cardinality({i \in Arms : candidate_ready[i]}) >= 2
    /\ \E i \in Arms :
          /\ candidate_ready[i]
          /\ \E j \in Arms :
                /\ candidate_ready[j]
                /\ j < i
          /\ winner' = i
          /\ claim_candidates' = {k \in Arms : candidate_ready[k]}
          /\ arm_class' = [k \in Arms |->
               IF k = i THEN "Winner"
               ELSE IF arm_registered[k] THEN "Loser"
               ELSE "Unclassified"]
          /\ claim_mode' = "Inline"
          /\ central_phase' = "Claimed"
          /\ contract_phase' = "WinnerLinearized"
    /\ fault_used' = TRUE
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count, candidate_ready,
                    linearized_winner, linearized_winner_valid,
                    claim_snapshot_frozen, claim_snapshot_frozen_valid>>

\* ----- NEG-S6: winner taken from another group ---------------------------
\* In this single-group Central model "another group" is modelled as a winner
\* whose arm is NOT in the claim snapshot (a candidate that did not belong to
\* this group's observed ready set).  The fault linearizes a winner that is
\* not in claim_candidates, violating S_InvClaimSnapshotContainsWinner and
\* S_InvWinnerChosenFromSnapshot.  This is the single-group reduction of the
\* cross-group contamination property (the multi-group module O proves the
\* stronger form directly).
Fault_S6 ==
    /\ FaultActive("S6")
    /\ ~fault_used
    /\ central_phase \in {"Admission", "Armed"}
    /\ \E i \in Arms :
          /\ arm_registered[i]
          /\ i \notin claim_candidates
          /\ winner' = i
          /\ arm_class' = [j \in Arms |->
               IF j = i THEN "Winner"
               ELSE IF arm_registered[j] THEN "Loser"
               ELSE "Unclassified"]
          /\ claim_mode' =
               IF central_phase = "Admission" THEN "Inline" ELSE "Suspended"
          /\ central_phase' = "Claimed"
          /\ contract_phase' = "WinnerLinearized"
    /\ fault_used' = TRUE
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count, candidate_ready, claim_candidates,
                    linearized_winner, linearized_winner_valid,
                    claim_snapshot_frozen, claim_snapshot_frozen_valid>>

FaultNext ==
    \/ Fault_S1
    \/ Fault_S2
    \/ Fault_S3
    \/ Fault_S4
    \/ Fault_S5
    \/ Fault_S6

SNegStutter == UNCHANGED SNegVars

BaseNextFrozen == Base!CentralNext /\ UNCHANGED fault_used

SNegNext ==
    \/ SNegStutter
    \/ BaseNextFrozen
    \/ FaultNext

SNegSpec == SNegInit /\ [][SNegNext]_SNegVars

\* Re-export Central safety invariants by name.
S_InvClaimRequiresOfferedArm == Base!S_InvClaimRequiresOfferedArm
S_InvClaimSnapshotContainsWinner == Base!S_InvClaimSnapshotContainsWinner
S_InvWinnerChosenFromSnapshot == Base!S_InvWinnerChosenFromSnapshot
S_InvAtMostOneSuccessfulClaim == Base!S_InvAtMostOneSuccessfulClaim
S_InvClaimBeforeAdapterCommit == Base!S_InvClaimBeforeAdapterCommit
S_InvAdmissionTieUsesLowestIndex == Base!S_InvAdmissionTieUsesLowestIndex
S_InvClaimSnapshotImmutableAfterClaim == Base!S_InvClaimSnapshotImmutableAfterClaim
S_InvExactlyOneWinnerClassification == Base!S_InvExactlyOneWinnerClassification
CentralSafetyInv == Base!CentralSafetyInv

=============================================================================
