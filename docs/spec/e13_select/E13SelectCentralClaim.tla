----------------------- MODULE E13SelectCentralClaim -------------------------
(*
  Candidate A / Central Claim is one concrete Select strategy.  It is not the
  abstract Select contract and is not presented as the only possible strategy.

  CandidateReady, the claim-time candidate snapshot, lowest-index selection,
  and Winner/Loser classification live only in this module.  The top-level
  INSTANCE below is the explicit refinement mapping to E13SelectContract.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Arms, MaxArms

NoArm == 1000000

VARIABLES
    \* Contract projection variables.
    contract_phase,
    arm_registered,
    readiness_evidence,
    reservation_state,
    arm_resolution,
    authority_open,
    winner,
    caller_state,
    completion_mode,
    result_publication_count,
    runnable_publication_count,
    arm_publication_count,
    reservation_close_count,
    linearized_winner,
    linearized_winner_valid,

    \* Central Claim strategy variables.
    central_phase,
    candidate_ready,
    claim_candidates,
    arm_class,
    claim_mode,
    claim_snapshot_frozen,
    claim_snapshot_frozen_valid

ContractProjectionVars ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      arm_resolution, authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count,
      linearized_winner, linearized_winner_valid>>

CentralOnlyVars ==
    <<central_phase, candidate_ready, claim_candidates, arm_class, claim_mode,
      claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralVars == <<ContractProjectionVars, CentralOnlyVars>>

ContractRefinement == INSTANCE E13SelectContract WITH
    Arms <- Arms,
    MaxArms <- MaxArms,
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
    linearized_winner_valid <- linearized_winner_valid

CentralPhaseT ==
    {"Registering", "Admission", "Armed", "Claimed", "Closing",
     "Completed", "Rollback", "Aborted", "Consumed", "Destroyed"}
ArmClassT == {"Unclassified", "Winner", "Loser"}
ClaimModeT == {"None", "Inline", "Suspended"}

ReadySet == {i \in Arms : candidate_ready[i]}

LowestReady ==
    IF ReadySet = {} THEN NoArm
    ELSE CHOOSE i \in ReadySet : \A j \in ReadySet : i <= j

CentralTypeOK ==
    /\ central_phase \in CentralPhaseT
    /\ candidate_ready \in [Arms -> BOOLEAN]
    /\ claim_candidates \subseteq Arms
    /\ arm_class \in [Arms -> ArmClassT]
    /\ claim_mode \in ClaimModeT
    /\ linearized_winner \in Arms \cup {NoArm}
    /\ linearized_winner_valid \in BOOLEAN
    /\ claim_snapshot_frozen \subseteq Arms
    /\ claim_snapshot_frozen_valid \in BOOLEAN

CentralStateWellFormed ==
    /\ \A i \in Arms :
        /\ (candidate_ready[i] =>
              /\ arm_registered[i]
              /\ readiness_evidence[i] = "Offered")
        /\ reservation_state[i] = "None"
        /\ reservation_close_count[i] = 0
    /\ (winner = NoArm =>
          /\ claim_candidates = {}
          /\ claim_mode = "None"
          /\ \A i \in Arms : arm_class[i] = "Unclassified")
    /\ (winner \in Arms =>
          /\ winner \in claim_candidates
          /\ arm_class[winner] = "Winner"
          /\ \A i \in Arms \ {winner} :
                arm_registered[i] => arm_class[i] = "Loser")
    /\ (central_phase = "Registering" => contract_phase = "Building")
    /\ (central_phase \in {"Admission", "Armed"} =>
          contract_phase = "Selecting")
    /\ (central_phase = "Claimed" =>
          contract_phase = "WinnerLinearized")
    /\ (central_phase = "Closing" => contract_phase = "Closing")
    /\ (central_phase = "Completed" => contract_phase = "Completed")
    /\ (central_phase = "Rollback" => contract_phase = "Rollback")
    /\ (central_phase = "Aborted" => contract_phase = "Aborted")
    /\ (central_phase = "Consumed" => contract_phase = "Consumed")
    /\ (central_phase = "Destroyed" => contract_phase = "Destroyed")
    /\ (claim_mode = "Inline" => caller_state = "Running")
    /\ (claim_mode = "Suspended" => caller_state \in {"Waiting", "Runnable", "Resumed"})

LowestIndexClaimFromSnapshot ==
    winner \in Arms => \A i \in claim_candidates : winner <= i

CentralRollbackEnabledDomain ==
    /\ central_phase = "Registering"
    /\ contract_phase = "Building"
    /\ winner = NoArm
    /\ caller_state = "Running"

CentralRegistrationRollbackDisabledAfterSuspension ==
    caller_state = "Waiting" => ~CentralRollbackEnabledDomain

(* ========================================================================= *)
(* PR #18 Central Claim safety invariants (Layer S).                          *)
(*                                                                           *)
(* These named laws are consequences of the PR #17 Central Claim transition   *)
(* relation.  They are aggregated in `CentralSafetyInv`, kept separate from   *)
(* the canonical PR #17 `CentralInv` so PR #17 metrics reproduce.             *)
(*                                                                           *)
(* `S_InvAdmissionTieUsesLowestIndex` applies ONLY to a same claim epoch      *)
(* admission claim with multiple offered arms in the snapshot.  It does NOT   *)
(* claim that every post-suspension race uses the lowest index; once the      *)
(* caller is suspended, the resolver that first obtains legal claim           *)
(* authority wins that race.                                                 *)
(* ========================================================================= *)

CentralClaimedPhase == central_phase \in {"Claimed", "Closing", "Completed", "Consumed"}

\* -- J: claim correctness --------------------------------------------------

S_InvAtMostOneSuccessfulClaim ==
    \* At most one arm may be classified Winner.  An unregistered arm must
    \* remain Unclassified, so only genuine registered non-winner arms can be
    \* classified Loser.
    Cardinality({i \in Arms : arm_class[i] = "Winner"}) <= 1

S_InvClaimRequiresOfferedArm ==
    winner \in Arms => candidate_ready[winner]

S_InvClaimSnapshotContainsWinner ==
    \* The live claim snapshot always contains the winner once one exists.
    \* This is the precondition for the frozen-snapshot identity laws below.
    winner \in Arms => winner \in claim_candidates

S_InvClaimSnapshotImmutableAfterClaim ==
    \* Once the frozen snapshot has been stamped by a claim, the live
    \* `claim_candidates` must remain bit-for-bit equal to that frozen value.
    \* This is the strict immutability law: any post-claim mutation of
    \* claim_candidates (addition OR removal of a member) violates it.  The
    \* frozen value is written exactly once, by CentralClaimWinner, and never
    \* again; this law asserts that the live variable tracks it forever after.
    \* Single-claim / single-operation model: the frozen snapshot captures the
    \* one claim epoch; multi-epoch claim identity is deferred.
    claim_snapshot_frozen_valid => claim_candidates = claim_snapshot_frozen

S_InvWinnerChosenFromSnapshot ==
    \* The winner remains drawn from the frozen snapshot stamped at claim time.
    \* Strengthens the live-snapshot membership law by pinning identity to the
    \* frozen snapshot, which itself is immutable per the law above.  Together
    \* these prove the winner cannot be retroactively swapped out for an arm
    \* that was not in the snapshot at claim time.
    winner \in Arms /\ claim_snapshot_frozen_valid
        => winner \in claim_snapshot_frozen

S_InvClaimBeforeAdapterCommit ==
    \* A Central winner commit (arm_resolution[winner] = "WinnerCommitted")
    \* requires that the winner was first classified by a claim
    \* (arm_class[winner] = "Winner").  CentralClaimWinner is the only action
    \* that assigns arm_class = "Winner", and CentralCommitWinner is gated on
    \* it, so commit cannot precede claim.
    winner \in Arms /\ arm_resolution[winner] = "WinnerCommitted"
        => arm_class[winner] = "Winner"

S_InvExactlyOneWinnerClassification ==
    winner \in Arms =>
        /\ arm_class[winner] = "Winner"
        /\ Cardinality({i \in Arms : arm_class[i] = "Winner"}) = 1

S_InvAllOtherRegisteredArmsClassifiedLoser ==
    winner \in Arms =>
        \A i \in Arms \ {winner} :
            arm_registered[i] => arm_class[i] = "Loser"

S_InvClassificationsStableAfterClaim ==
    \* arm_class is reassigned only by CentralClaimWinner (gated on
    \* winner = NoArm) and never by commit/release/close/publish.  The state
    \* consequence asserted here: once a winner exists, the winner's class is
    \* fixed to Winner and persists through terminal phases.
    winner \in Arms => arm_class[winner] = "Winner"

S_InvNoPrimitiveMutationInsideClaim ==
    \* The Central Claim layer owns no primitive state (no Event, Timer,
    \* WaitNode).  Candidate readiness is Central evidence, not a primitive.
    \* This law asserts the typing invariant: the layer only carries abstract
    \* classification + claim snapshot state, no reservation Held/Committed.
    \A i \in Arms : reservation_state[i] = "None"

\* -- Admission tie (narrow domain only) -----------------------------------

S_InvAdmissionTieUsesLowestIndex ==
    \* Applies ONLY to an admission claim (claim_mode = "Inline") in a single
    \* claim epoch, when multiple offered arms exist in the snapshot.  The
    \* winner must be the lowest-indexed candidate.  This is NOT asserted for
    \* post-suspension races.
    /\ claim_mode = "Inline"
    /\ winner \in Arms
    /\ Cardinality(claim_candidates) >= 2
    => \A i \in claim_candidates : winner <= i

\* -- Rollback alignment ----------------------------------------------------

S_InvRollbackDomainRefinesContract ==
    \* When Central rollback is enabled, the Contract rollback domain must
    \* also be enabled (the concrete action refines the abstract one).
    CentralRollbackEnabledDomain
        => ContractRefinement!ContractRollbackEnabledDomain

S_InvRollbackDisabledAfterArmed ==
    \* Once the caller is Armed (suspended), rollback must be disabled at
    \* this layer too.
    central_phase = "Armed" => ~CentralRollbackEnabledDomain

S_InvRollbackDisabledAfterClaim ==
    central_phase \in {"Claimed", "Closing", "Completed", "Consumed"}
        => ~CentralRollbackEnabledDomain

CentralSafetyInv ==
    /\ S_InvAtMostOneSuccessfulClaim
    /\ S_InvClaimRequiresOfferedArm
    /\ S_InvClaimSnapshotContainsWinner
    /\ S_InvClaimSnapshotImmutableAfterClaim
    /\ S_InvWinnerChosenFromSnapshot
    /\ S_InvClaimBeforeAdapterCommit
    /\ S_InvExactlyOneWinnerClassification
    /\ S_InvAllOtherRegisteredArmsClassifiedLoser
    /\ S_InvClassificationsStableAfterClaim
    /\ S_InvNoPrimitiveMutationInsideClaim
    /\ S_InvAdmissionTieUsesLowestIndex
    /\ S_InvRollbackDomainRefinesContract
    /\ S_InvRollbackDisabledAfterArmed
    /\ S_InvRollbackDisabledAfterClaim

CentralInv ==
    /\ ContractRefinement!ContractInv
    /\ CentralTypeOK
    /\ CentralStateWellFormed
    /\ LowestIndexClaimFromSnapshot
    /\ CentralRegistrationRollbackDisabledAfterSuspension

CentralInit ==
    /\ ContractRefinement!ContractInit
    /\ central_phase = "Registering"
    /\ candidate_ready = [i \in Arms |-> FALSE]
    /\ claim_candidates = {}
    /\ arm_class = [i \in Arms |-> "Unclassified"]
    /\ claim_mode = "None"
    /\ claim_snapshot_frozen = {}
    /\ claim_snapshot_frozen_valid = FALSE

CentralRegisterArm(i) ==
    /\ central_phase = "Registering"
    /\ ContractRefinement!ContractRegisterArm(i)
    /\ UNCHANGED CentralOnlyVars

CentralFinishRegistration ==
    /\ central_phase = "Registering"
    /\ ContractRefinement!ContractFinishRegistration
    /\ central_phase' = "Admission"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralObserveCandidate(i) ==
    /\ central_phase \in {"Admission", "Armed"}
    /\ ~candidate_ready[i]
    /\ ContractRefinement!ContractOfferReadiness(i)
    /\ candidate_ready' = [candidate_ready EXCEPT ![i] = TRUE]
    /\ UNCHANGED <<central_phase, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralSuspendCaller ==
    /\ central_phase = "Admission"
    /\ ReadySet = {}
    /\ ContractRefinement!ContractSuspendCaller
    /\ central_phase' = "Armed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralClaimWinner(i) ==
    /\ central_phase \in {"Admission", "Armed"}
    /\ i = LowestReady
    /\ i # NoArm
    /\ ContractRefinement!ContractLinearizeWinner(i)
    /\ claim_candidates' = ReadySet
    /\ arm_class' =
          [j \in Arms |->
             IF ~arm_registered[j] THEN "Unclassified"
             ELSE IF j = i THEN "Winner" ELSE "Loser"]
    /\ claim_mode' = IF central_phase = "Admission" THEN "Inline"
                    ELSE "Suspended"
    /\ central_phase' = "Claimed"
    /\ claim_snapshot_frozen' = ReadySet
    /\ claim_snapshot_frozen_valid' = TRUE
    /\ UNCHANGED candidate_ready

CentralCommitWinner(i) ==
    /\ central_phase = "Claimed"
    /\ arm_class[i] = "Winner"
    /\ ContractRefinement!ContractCommitWinner(i)
    /\ central_phase' = "Closing"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralReleaseLoser(i) ==
    /\ central_phase \in {"Claimed", "Closing"}
    /\ arm_class[i] = "Loser"
    /\ ContractRefinement!ContractReleaseLoser(i)
    /\ UNCHANGED CentralOnlyVars

CentralCloseAuthority(i) ==
    /\ central_phase \in {"Claimed", "Closing", "Rollback"}
    /\ ContractRefinement!ContractCloseAuthority(i)
    /\ UNCHANGED CentralOnlyVars

CentralPublishInline ==
    /\ central_phase = "Closing"
    /\ claim_mode = "Inline"
    /\ ContractRefinement!ContractPublishInline
    /\ central_phase' = "Completed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralPublishSuspended ==
    /\ central_phase = "Closing"
    /\ claim_mode = "Suspended"
    /\ ContractRefinement!ContractPublishSuspended
    /\ central_phase' = "Completed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralResumeCaller ==
    /\ central_phase = "Completed"
    /\ ContractRefinement!ContractResumeCaller
    /\ UNCHANGED CentralOnlyVars

CentralConsumeResult ==
    /\ central_phase = "Completed"
    /\ ContractRefinement!ContractConsumeResult
    /\ central_phase' = "Consumed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralDestroyOperation ==
    /\ central_phase \in {"Consumed", "Aborted"}
    /\ ContractRefinement!ContractDestroyOperation
    /\ central_phase' = "Destroyed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralBeginRollback ==
    /\ CentralRollbackEnabledDomain
    /\ ContractRefinement!ContractBeginRollback
    /\ central_phase' = "Rollback"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralRollbackRelease(i) ==
    /\ central_phase = "Rollback"
    /\ ContractRefinement!ContractRollbackRelease(i)
    /\ UNCHANGED CentralOnlyVars

CentralFinishRollback ==
    /\ central_phase = "Rollback"
    /\ ContractRefinement!ContractFinishRollback
    /\ central_phase' = "Aborted"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode,
                  claim_snapshot_frozen, claim_snapshot_frozen_valid>>

CentralStutter == UNCHANGED CentralVars

CentralNext ==
    \/ CentralStutter
    \/ \E i \in Arms : CentralRegisterArm(i)
    \/ CentralFinishRegistration
    \/ \E i \in Arms : CentralObserveCandidate(i)
    \/ CentralSuspendCaller
    \/ \E i \in Arms : CentralClaimWinner(i)
    \/ \E i \in Arms : CentralCommitWinner(i)
    \/ \E i \in Arms : CentralReleaseLoser(i)
    \/ \E i \in Arms : CentralCloseAuthority(i)
    \/ CentralPublishInline
    \/ CentralPublishSuspended
    \/ CentralResumeCaller
    \/ CentralConsumeResult
    \/ CentralDestroyOperation
    \/ CentralBeginRollback
    \/ \E i \in Arms : CentralRollbackRelease(i)
    \/ CentralFinishRollback

CentralSpec == CentralInit /\ [][CentralNext]_CentralVars

\* TLC checks this instantiated abstract behavior as a temporal PROPERTY.
RefinesContract == ContractRefinement!ContractSpec

ReachCentralTieBreak ==
    /\ winner \in Arms
    /\ Cardinality(claim_candidates) >= 2
    /\ winner \in claim_candidates
    /\ \A i \in claim_candidates : winner <= i
    /\ arm_class[winner] = "Winner"
    /\ \A i \in claim_candidates \ {winner} : arm_class[i] = "Loser"

NotReachCentralTieBreak == ~ReachCentralTieBreak

\* =========================================================================
\* PR #18 non-vacuity witnesses (W): reachability of each Central safety law's
\* trigger condition, so the law is provably non-vacuous in the model.
\* =========================================================================

\* S_InvClaimRequiresOfferedArm premise: an offer was claimed.
ReachCentralClaimed ==
    central_phase \in {"Claimed", "Closing", "Completed", "Consumed"}
    /\ winner \in Arms
NotReachCentralClaimed == ~ReachCentralClaimed

\* S_InvClaimSnapshotContainsWinner premise: claim_candidates latched.
ReachCentralClaimCandidatesLatched ==
    central_phase \in {"Claimed", "Closing", "Completed", "Consumed"}
    /\ Cardinality(claim_candidates) >= 1
    /\ winner \in claim_candidates
NotReachCentralClaimCandidatesLatched == ~ReachCentralClaimCandidatesLatched

\* S_InvAdmissionTieUsesLowestIndex premise: multi-candidate admission tie.
ReachCentralAdmissionTie ==
    /\ claim_mode = "Inline"
    /\ Cardinality(claim_candidates) >= 2
    /\ winner \in claim_candidates
    /\ \A i \in claim_candidates : winner <= i
NotReachCentralAdmissionTie == ~ReachCentralAdmissionTie

\* S_InvAtMostOneSuccessfulClaim premise: a successful claim happened.
ReachCentralWinnerClassified ==
    central_phase \in {"Claimed", "Closing", "Completed", "Consumed"}
    /\ \E w \in Arms : arm_class[w] = "Winner"
    /\ \E l \in Arms : arm_class[l] = "Loser"
NotReachCentralWinnerClassified == ~ReachCentralWinnerClassified

\* PR #18 corrective-1 (P1-3) non-vacuity witnesses for the frozen claim
\* snapshot laws.  These prove the laws' premises are reachable so the laws
\* are not vacuously TRUE.

\* S_InvClaimSnapshotImmutableAfterClaim premise: the frozen snapshot has
\* been stamped (a claim happened and froze the live snapshot).
ReachCentralFrozenSnapshotValid ==
    claim_snapshot_frozen_valid
NotReachCentralFrozenSnapshotValid == ~ReachCentralFrozenSnapshotValid

\* S_InvWinnerChosenFromSnapshot non-trivial premise: the frozen snapshot
\* contained more than one candidate, so the winner-chosen-from-snapshot law
\* exercises a real selection (not a single-element trivial case).
ReachCentralMultiCandidateSnapshot ==
    /\ claim_snapshot_frozen_valid
    /\ Cardinality(claim_snapshot_frozen) >= 2
NotReachCentralMultiCandidateSnapshot == ~ReachCentralMultiCandidateSnapshot

=============================================================================
