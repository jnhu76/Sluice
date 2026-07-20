-------------------------- MODULE E13SelectContract --------------------------
(*
  Stable, strategy-neutral Select contract for E13 PR #17.

  This module deliberately contains NO CandidateReady, Event, Timer,
  WaitQueue, timer heap, or global-mutex concepts.  It describes only the
  externally observable Select lifecycle and the authority/effect laws that
  every future strategy and adapter must refine.

  A pre-claim reservation is optional.  When present it is reversible:
  Held must close exactly once as either Committed by the linearized winner or
  Released by a loser/rollback path.  Irreversible external commit is forbidden
  before winner linearization.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Arms, MaxArms

NoArm == 1000000

VARIABLES
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
    reservation_close_count

ContractVars ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      arm_resolution, authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count>>

ContractPhaseT ==
    {"Building", "Selecting", "WinnerLinearized", "Closing", "Completed",
     "Rollback", "Aborted", "Consumed", "Destroyed"}
EvidenceT == {"None", "Offered", "Reserved"}
ReservationT == {"None", "Held", "Committed", "Released"}
ResolutionT == {"None", "Open", "WinnerCommitted", "Released"}
CallerStateT == {"Running", "Waiting", "Runnable", "Resumed"}
CompletionModeT == {"None", "Inline", "Suspended"}

ContractTypeOK ==
    /\ contract_phase \in ContractPhaseT
    /\ arm_registered \in [Arms -> BOOLEAN]
    /\ readiness_evidence \in [Arms -> EvidenceT]
    /\ reservation_state \in [Arms -> ReservationT]
    /\ arm_resolution \in [Arms -> ResolutionT]
    /\ authority_open \in [Arms -> BOOLEAN]
    /\ winner \in Arms \cup {NoArm}
    /\ caller_state \in CallerStateT
    /\ completion_mode \in CompletionModeT
    /\ result_publication_count \in 0..1
    /\ runnable_publication_count \in 0..1
    /\ arm_publication_count \in [Arms -> 0..1]
    /\ reservation_close_count \in [Arms -> 0..1]

ContractDomainWellFormed ==
    /\ Cardinality(Arms) = MaxArms
    /\ NoArm \notin Arms
    /\ \A i \in Arms :
        /\ (~arm_registered[i] <=> arm_resolution[i] = "None")
        /\ ~arm_registered[i] =>
              /\ readiness_evidence[i] = "None"
              /\ reservation_state[i] = "None"
              /\ ~authority_open[i]
        /\ readiness_evidence[i] # "Reserved" =>
              reservation_state[i] = "None"
        /\ reservation_state[i] # "None" =>
              readiness_evidence[i] = "Reserved"
        /\ (reservation_state[i] = "Held" =>
              /\ reservation_close_count[i] = 0
              /\ arm_resolution[i] = "Open")
        /\ (reservation_state[i] \in {"Committed", "Released"} =>
              reservation_close_count[i] = 1)
        /\ (reservation_state[i] = "None" =>
              reservation_close_count[i] = 0)

ExactlyOneLinearizedWinner ==
    /\ (contract_phase \in
          {"WinnerLinearized", "Closing", "Completed", "Consumed"}
        => winner \in Arms)
    /\ (contract_phase \in {"Building", "Selecting", "Rollback", "Aborted"}
        => winner = NoArm)

NoIrreversibleEffectBeforeLinearization ==
    \A i \in Arms :
        arm_resolution[i] = "WinnerCommitted" => winner = i

ReservationCommitOrReleaseClosesOnce ==
    \A i \in Arms :
        /\ (reservation_state[i] = "Committed" =>
              /\ winner = i
              /\ arm_resolution[i] = "WinnerCommitted")
        /\ (reservation_state[i] = "Released" =>
              arm_resolution[i] = "Released")
        /\ reservation_close_count[i] <= 1

LosersNeverPublish ==
    \A i \in Arms : arm_publication_count[i] > 0 => winner = i

AllAuthorityClosedBeforeCompletion ==
    contract_phase \in {"Completed", "Consumed", "Aborted", "Destroyed"}
        => \A i \in Arms : ~authority_open[i]

PublicationExactlyBounded ==
    /\ result_publication_count <= 1
    /\ runnable_publication_count <= 1
    /\ (contract_phase \in {"Completed", "Consumed"} =>
          /\ result_publication_count = 1
          /\ winner \in Arms
          /\ arm_publication_count[winner] = 1
          /\ IF completion_mode = "Inline"
                THEN runnable_publication_count = 0
                ELSE /\ completion_mode = "Suspended"
                     /\ runnable_publication_count = 1)
    /\ (contract_phase \in {"Aborted"} =>
          /\ result_publication_count = 0
          /\ runnable_publication_count = 0
          /\ \A i \in Arms : arm_publication_count[i] = 0)

WinnerAndLosersFinalBeforeCompletion ==
    contract_phase \in {"Completed", "Consumed"} =>
        /\ arm_resolution[winner] = "WinnerCommitted"
        /\ \A i \in Arms \ {winner} : arm_resolution[i] = "Released"

BadTerminalWaiting ==
    /\ contract_phase \in {"Aborted", "Destroyed"}
    /\ caller_state = "Waiting"

NoBadTerminalWaiting == ~BadTerminalWaiting

TerminalCallerStateWellFormed ==
    /\ (contract_phase \in {"Rollback", "Aborted"} =>
          caller_state = "Running")
    /\ (contract_phase = "Destroyed" =>
          \/ /\ completion_mode = "None"
                /\ caller_state = "Running"
             \/ /\ completion_mode = "Inline"
                /\ caller_state = "Running"
             \/ /\ completion_mode = "Suspended"
                /\ caller_state = "Resumed")

ContractRollbackEnabledDomain ==
    /\ contract_phase = "Building"
    /\ winner = NoArm
    /\ caller_state = "Running"

ContractRegistrationRollbackDisabledAfterSuspension ==
    caller_state = "Waiting" => ~ContractRollbackEnabledDomain

ContractInv ==
    /\ ContractTypeOK
    /\ ContractDomainWellFormed
    /\ ExactlyOneLinearizedWinner
    /\ NoIrreversibleEffectBeforeLinearization
    /\ ReservationCommitOrReleaseClosesOnce
    /\ LosersNeverPublish
    /\ AllAuthorityClosedBeforeCompletion
    /\ PublicationExactlyBounded
    /\ WinnerAndLosersFinalBeforeCompletion
    /\ NoBadTerminalWaiting
    /\ TerminalCallerStateWellFormed
    /\ ContractRegistrationRollbackDisabledAfterSuspension

ContractInit ==
    /\ contract_phase = "Building"
    /\ arm_registered = [i \in Arms |-> FALSE]
    /\ readiness_evidence = [i \in Arms |-> "None"]
    /\ reservation_state = [i \in Arms |-> "None"]
    /\ arm_resolution = [i \in Arms |-> "None"]
    /\ authority_open = [i \in Arms |-> FALSE]
    /\ winner = NoArm
    /\ caller_state = "Running"
    /\ completion_mode = "None"
    /\ result_publication_count = 0
    /\ runnable_publication_count = 0
    /\ arm_publication_count = [i \in Arms |-> 0]
    /\ reservation_close_count = [i \in Arms |-> 0]

ContractRegisterArm(i) ==
    /\ contract_phase = "Building"
    /\ ~arm_registered[i]
    /\ arm_registered' = [arm_registered EXCEPT ![i] = TRUE]
    /\ arm_resolution' = [arm_resolution EXCEPT ![i] = "Open"]
    /\ authority_open' = [authority_open EXCEPT ![i] = TRUE]
    /\ UNCHANGED <<contract_phase, readiness_evidence, reservation_state,
                    winner, caller_state, completion_mode,
                    result_publication_count, runnable_publication_count,
                    arm_publication_count, reservation_close_count>>

ContractFinishRegistration ==
    /\ contract_phase = "Building"
    /\ Arms # {}
    /\ \A i \in Arms : arm_registered[i]
    /\ contract_phase' = "Selecting"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractOfferReadiness(i) ==
    /\ contract_phase = "Selecting"
    /\ arm_registered[i]
    /\ readiness_evidence[i] = "None"
    /\ readiness_evidence' = [readiness_evidence EXCEPT ![i] = "Offered"]
    /\ UNCHANGED <<contract_phase, arm_registered, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractReserveReadiness(i) ==
    /\ contract_phase = "Selecting"
    /\ arm_registered[i]
    /\ readiness_evidence[i] = "None"
    /\ readiness_evidence' = [readiness_evidence EXCEPT ![i] = "Reserved"]
    /\ reservation_state' = [reservation_state EXCEPT ![i] = "Held"]
    /\ UNCHANGED <<contract_phase, arm_registered, arm_resolution,
                    authority_open, winner, caller_state, completion_mode,
                    result_publication_count, runnable_publication_count,
                    arm_publication_count, reservation_close_count>>

ContractSuspendCaller ==
    /\ contract_phase = "Selecting"
    /\ winner = NoArm
    /\ caller_state = "Running"
    /\ caller_state' = "Waiting"
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open, winner,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractLinearizeWinner(i) ==
    /\ contract_phase = "Selecting"
    /\ winner = NoArm
    /\ readiness_evidence[i] \in {"Offered", "Reserved"}
    /\ winner' = i
    /\ contract_phase' = "WinnerLinearized"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractCommitWinner(i) ==
    /\ contract_phase \in {"WinnerLinearized", "Closing"}
    /\ winner = i
    /\ arm_resolution[i] = "Open"
    /\ arm_resolution' = [arm_resolution EXCEPT ![i] = "WinnerCommitted"]
    /\ reservation_state' =
          [reservation_state EXCEPT
             ![i] = IF @ = "Held" THEN "Committed" ELSE @]
    /\ reservation_close_count' =
          [reservation_close_count EXCEPT
             ![i] = IF reservation_state[i] = "Held" THEN @ + 1 ELSE @]
    /\ contract_phase' = "Closing"
    /\ UNCHANGED <<arm_registered, readiness_evidence, authority_open,
                    winner, caller_state, completion_mode,
                    result_publication_count, runnable_publication_count,
                    arm_publication_count>>

ContractReleaseLoser(i) ==
    /\ contract_phase \in {"WinnerLinearized", "Closing"}
    /\ winner \in Arms
    /\ winner # i
    /\ arm_resolution[i] = "Open"
    /\ arm_resolution' = [arm_resolution EXCEPT ![i] = "Released"]
    /\ reservation_state' =
          [reservation_state EXCEPT
             ![i] = IF @ = "Held" THEN "Released" ELSE @]
    /\ reservation_close_count' =
          [reservation_close_count EXCEPT
             ![i] = IF reservation_state[i] = "Held" THEN @ + 1 ELSE @]
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    authority_open, winner, caller_state, completion_mode,
                    result_publication_count, runnable_publication_count,
                    arm_publication_count>>

ContractCloseAuthority(i) ==
    /\ contract_phase \in {"WinnerLinearized", "Closing", "Rollback"}
    /\ authority_open[i]
    /\ arm_resolution[i] \in {"WinnerCommitted", "Released"}
    /\ authority_open' = [authority_open EXCEPT ![i] = FALSE]
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

SuccessfulClosureReady ==
    /\ contract_phase = "Closing"
    /\ winner \in Arms
    /\ arm_resolution[winner] = "WinnerCommitted"
    /\ \A i \in Arms \ {winner} : arm_resolution[i] = "Released"
    /\ \A i \in Arms : ~authority_open[i]
    /\ result_publication_count = 0
    /\ \A i \in Arms : arm_publication_count[i] = 0

ContractPublishInline ==
    /\ SuccessfulClosureReady
    /\ caller_state = "Running"
    /\ result_publication_count' = 1
    /\ arm_publication_count' = [arm_publication_count EXCEPT ![winner] = 1]
    /\ completion_mode' = "Inline"
    /\ contract_phase' = "Completed"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    runnable_publication_count, reservation_close_count>>

ContractPublishSuspended ==
    /\ SuccessfulClosureReady
    /\ caller_state = "Waiting"
    /\ result_publication_count' = 1
    /\ runnable_publication_count' = 1
    /\ arm_publication_count' = [arm_publication_count EXCEPT ![winner] = 1]
    /\ caller_state' = "Runnable"
    /\ completion_mode' = "Suspended"
    /\ contract_phase' = "Completed"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner,
                    reservation_close_count>>

ContractResumeCaller ==
    /\ contract_phase = "Completed"
    /\ completion_mode = "Suspended"
    /\ caller_state = "Runnable"
    /\ caller_state' = "Resumed"
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open, winner,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractConsumeResult ==
    /\ contract_phase = "Completed"
    /\ \/ /\ completion_mode = "Inline"
           /\ caller_state = "Running"
       \/ /\ completion_mode = "Suspended"
           /\ caller_state = "Resumed"
    /\ contract_phase' = "Consumed"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractDestroyOperation ==
    /\ contract_phase \in {"Consumed", "Aborted"}
    /\ contract_phase' = "Destroyed"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractBeginRollback ==
    /\ ContractRollbackEnabledDomain
    /\ contract_phase' = "Rollback"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractRollbackRelease(i) ==
    /\ contract_phase = "Rollback"
    /\ arm_registered[i]
    /\ arm_resolution[i] = "Open"
    /\ arm_resolution' = [arm_resolution EXCEPT ![i] = "Released"]
    /\ reservation_state' =
          [reservation_state EXCEPT
             ![i] = IF @ = "Held" THEN "Released" ELSE @]
    /\ reservation_close_count' =
          [reservation_close_count EXCEPT
             ![i] = IF reservation_state[i] = "Held" THEN @ + 1 ELSE @]
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    authority_open, winner, caller_state, completion_mode,
                    result_publication_count, runnable_publication_count,
                    arm_publication_count>>

ContractFinishRollback ==
    /\ contract_phase = "Rollback"
    /\ \A i \in Arms :
          \/ /\ ~arm_registered[i]
             /\ arm_resolution[i] = "None"
             /\ ~authority_open[i]
          \/ /\ arm_registered[i]
             /\ arm_resolution[i] = "Released"
             /\ ~authority_open[i]
    /\ contract_phase' = "Aborted"
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count>>

ContractStutter == UNCHANGED ContractVars

ContractNext ==
    \/ ContractStutter
    \/ \E i \in Arms : ContractRegisterArm(i)
    \/ ContractFinishRegistration
    \/ \E i \in Arms : ContractOfferReadiness(i)
    \/ \E i \in Arms : ContractReserveReadiness(i)
    \/ ContractSuspendCaller
    \/ \E i \in Arms : ContractLinearizeWinner(i)
    \/ \E i \in Arms : ContractCommitWinner(i)
    \/ \E i \in Arms : ContractReleaseLoser(i)
    \/ \E i \in Arms : ContractCloseAuthority(i)
    \/ ContractPublishInline
    \/ ContractPublishSuspended
    \/ ContractResumeCaller
    \/ ContractConsumeResult
    \/ ContractDestroyOperation
    \/ ContractBeginRollback
    \/ \E i \in Arms : ContractRollbackRelease(i)
    \/ ContractFinishRollback

ContractSpec == ContractInit /\ [][ContractNext]_ContractVars

ReachContractInline ==
    /\ contract_phase = "Completed"
    /\ completion_mode = "Inline"
    /\ winner \in Arms
    /\ result_publication_count = 1
    /\ runnable_publication_count = 0
    /\ \A i \in Arms : ~authority_open[i]

ReachContractReservationSuspended ==
    /\ contract_phase = "Completed"
    /\ completion_mode = "Suspended"
    /\ result_publication_count = 1
    /\ runnable_publication_count = 1
    /\ \E w \in Arms :
          /\ winner = w
          /\ reservation_state[w] = "Committed"
          /\ reservation_close_count[w] = 1
    /\ \E l \in Arms :
          /\ l # winner
          /\ reservation_state[l] = "Released"
          /\ reservation_close_count[l] = 1

NotReachContractInline == ~ReachContractInline
NotReachContractReservationSuspended == ~ReachContractReservationSuspended

=============================================================================
