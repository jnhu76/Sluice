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

    \* Central Claim strategy variables.
    central_phase,
    candidate_ready,
    claim_candidates,
    arm_class,
    claim_mode

ContractProjectionVars ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      arm_resolution, authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count>>

CentralOnlyVars ==
    <<central_phase, candidate_ready, claim_candidates, arm_class, claim_mode>>

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
    reservation_close_count <- reservation_close_count

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

CentralRegisterArm(i) ==
    /\ central_phase = "Registering"
    /\ ContractRefinement!ContractRegisterArm(i)
    /\ UNCHANGED CentralOnlyVars

CentralFinishRegistration ==
    /\ central_phase = "Registering"
    /\ ContractRefinement!ContractFinishRegistration
    /\ central_phase' = "Admission"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

CentralObserveCandidate(i) ==
    /\ central_phase \in {"Admission", "Armed"}
    /\ ~candidate_ready[i]
    /\ ContractRefinement!ContractOfferReadiness(i)
    /\ candidate_ready' = [candidate_ready EXCEPT ![i] = TRUE]
    /\ UNCHANGED <<central_phase, claim_candidates, arm_class, claim_mode>>

CentralSuspendCaller ==
    /\ central_phase = "Admission"
    /\ ReadySet = {}
    /\ ContractRefinement!ContractSuspendCaller
    /\ central_phase' = "Armed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

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
    /\ UNCHANGED candidate_ready

CentralCommitWinner(i) ==
    /\ central_phase = "Claimed"
    /\ arm_class[i] = "Winner"
    /\ ContractRefinement!ContractCommitWinner(i)
    /\ central_phase' = "Closing"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

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
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

CentralPublishSuspended ==
    /\ central_phase = "Closing"
    /\ claim_mode = "Suspended"
    /\ ContractRefinement!ContractPublishSuspended
    /\ central_phase' = "Completed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

CentralResumeCaller ==
    /\ central_phase = "Completed"
    /\ ContractRefinement!ContractResumeCaller
    /\ UNCHANGED CentralOnlyVars

CentralConsumeResult ==
    /\ central_phase = "Completed"
    /\ ContractRefinement!ContractConsumeResult
    /\ central_phase' = "Consumed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

CentralDestroyOperation ==
    /\ central_phase \in {"Consumed", "Aborted"}
    /\ ContractRefinement!ContractDestroyOperation
    /\ central_phase' = "Destroyed"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

CentralBeginRollback ==
    /\ CentralRollbackEnabledDomain
    /\ ContractRefinement!ContractBeginRollback
    /\ central_phase' = "Rollback"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

CentralRollbackRelease(i) ==
    /\ central_phase = "Rollback"
    /\ ContractRefinement!ContractRollbackRelease(i)
    /\ UNCHANGED CentralOnlyVars

CentralFinishRollback ==
    /\ central_phase = "Rollback"
    /\ ContractRefinement!ContractFinishRollback
    /\ central_phase' = "Aborted"
    /\ UNCHANGED <<candidate_ready, claim_candidates, arm_class, claim_mode>>

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

=============================================================================
