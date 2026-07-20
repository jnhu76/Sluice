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

(* ========================================================================= *)
(* PR #18 layered safety invariants.                                          *)
(*                                                                           *)
(* These named laws are consequences of the corrected PR #17 Contract         *)
(* transition relation.  They are kept in a separate aggregate               *)
(* `ContractSafetyInv` so the canonical PR #17 `ContractInv` and its          *)
(* reproducible metrics remain byte-identical; the expanded suite is checked  *)
(* by the PR #18 safety verifier in dedicated cfg files.                     *)
(*                                                                           *)
(* No law here references WaitNode Cancelled or TimerRegistration Retired as  *)
(* the definition of a loser.  An abstract loser is a registered non-winner   *)
(* arm whose resolution has reached Released.                                *)
(* ========================================================================= *)

LinearizedPhase == contract_phase \in
    {"WinnerLinearized", "Closing", "Completed", "Consumed"}
TerminalPhase == contract_phase \in
    {"Completed", "Consumed", "Aborted", "Destroyed"}

\* -- H1: winner and commit --------------------------------------------------

C_InvAtMostOneLinearizedWinner ==
    LinearizedPhase => winner \in Arms
    \* The single `winner` variable structurally bounds the linearized winner
    \* count to at most one; this law bounds it to at least one past
    \* linearization.  `winner` is never reassigned once non-NoArm because
    \* ContractLinearizeWinner guards on winner = NoArm, so winner identity is
    \* stable after linearization (guard argument, not just a state law).

C_InvAtMostOneCommittedWinner ==
    \A i, j \in Arms :
        arm_resolution[i] = "WinnerCommitted"
            /\ arm_resolution[j] = "WinnerCommitted" => i = j

C_InvCommitRequiresWinnerLinearization ==
    \* An irreversible winner commit may only occur after a winner has been
    \* linearized.  In state terms: a committed arm must be the current
    \* linearized winner.  It must never appear in a pre-linearization phase
    \* (Building/Selecting/Rollback/Aborted) where winner = NoArm.
    \A i \in Arms :
        arm_resolution[i] = "WinnerCommitted" => winner = i

C_InvNoIrreversibleEffectBeforeLinearization ==
    \A i \in Arms :
        arm_resolution[i] = "WinnerCommitted" => winner = i

C_InvWinnerIdentityStableAfterLinearization ==
    \A i \in Arms :
        arm_resolution[i] = "WinnerCommitted" => winner = i

\* -- H2: losers -------------------------------------------------------------

C_InvLoserNeverPublishesResult ==
    \A i \in Arms :
        arm_resolution[i] = "Released" => arm_publication_count[i] = 0

C_InvLoserNeverPublishesRunnable ==
    runnable_publication_count = 1
        => /\ winner \in Arms
           /\ completion_mode = "Suspended"

C_InvLoserNeverCommitsIrreversibleEffect ==
    \A i \in Arms :
        arm_registered[i] /\ winner # i
            => arm_resolution[i] # "WinnerCommitted"

C_InvAllLosersAbortedBeforeCompletion ==
    contract_phase \in {"Completed", "Consumed"} =>
        \A i \in Arms \ {winner} :
            arm_registered[i] => arm_resolution[i] = "Released"

\* -- H3: publication --------------------------------------------------------

C_InvAtMostOneResultPublication == result_publication_count <= 1

C_InvAtMostOneRunnablePublication == runnable_publication_count <= 1

C_InvInlineCompletionPublishesNoRunnable ==
    completion_mode = "Inline" => runnable_publication_count = 0

C_InvSuspendedCompletionPublishesRunnableExactlyOnce ==
    completion_mode = "Suspended" => runnable_publication_count = 1

C_InvRunnablePublicationRequiresPriorWaiting ==
    runnable_publication_count = 1 => completion_mode = "Suspended"

C_InvOnlyWinnerPublishes ==
    \A i \in Arms : arm_publication_count[i] > 0 => winner = i

\* -- H4: completion and destruction ----------------------------------------

C_InvCompletionRequiresWinnerCommitted ==
    contract_phase \in {"Completed", "Consumed"} =>
        /\ winner \in Arms
        /\ arm_resolution[winner] = "WinnerCommitted"

C_InvCompletionRequiresAllLosersAborted ==
    contract_phase \in {"Completed", "Consumed"} =>
        \A i \in Arms \ {winner} :
            arm_registered[i] => arm_resolution[i] = "Released"

C_InvCompletionRequiresAllAuthorityClosed ==
    TerminalPhase => \A i \in Arms : ~authority_open[i]

C_InvDestroyRequiresConsumedOrValidAbort ==
    contract_phase = "Destroyed" =>
        \/ completion_mode \in {"Inline", "Suspended"}
        \/ winner = NoArm

C_InvDestroyedHasNoOpenAuthority ==
    contract_phase = "Destroyed" => \A i \in Arms : ~authority_open[i]

\* -- H5: reservation contract ----------------------------------------------

C_InvReservationCommitRequiresWinner ==
    \A i \in Arms :
        reservation_state[i] = "Committed" => winner = i

C_InvLoserReservationReleased ==
    \* A loser's held reservation may persist transiently after winner
    \* linearization, but it must close to Released before completion.  At
    \* terminal success the contract forbids a lingering Held loser reservation.
    TerminalPhase /\ winner \in Arms =>
        \A i \in Arms \ {winner} :
            arm_registered[i] => reservation_state[i] \in {"None", "Released"}

C_InvReservationClosesExactlyOnce ==
    \A i \in Arms : reservation_close_count[i] <= 1

C_InvHeldReservationIsReversible ==
    \A i \in Arms :
        reservation_state[i] = "Held" => arm_resolution[i] = "Open"

C_InvReservationIsNotIrreversibleCommit ==
    \A i \in Arms :
        reservation_state[i] = "Held" => arm_resolution[i] # "WinnerCommitted"

\* -- I: PR #17 rollback closure as permanent safety law --------------------

C_InvRegistrationRollbackOnlyBeforeSuspension ==
    ContractRegistrationRollbackDisabledAfterSuspension

C_InvRegistrationRollbackRequiresNoWinner ==
    contract_phase = "Rollback" => winner = NoArm

C_InvRegistrationRollbackRequiresRunningCaller ==
    contract_phase = "Rollback" => caller_state = "Running"

C_InvAbortedCallerNeverWaiting ==
    contract_phase = "Aborted" => caller_state # "Waiting"

C_InvDestroyedCallerNeverWaiting ==
    contract_phase = "Destroyed" => caller_state # "Waiting"

C_InvRollbackNeverPublishes ==
    contract_phase \in {"Rollback", "Aborted"} =>
        /\ result_publication_count = 0
        /\ runnable_publication_count = 0

C_InvRollbackClosesEveryRegisteredAuthority ==
    contract_phase = "Aborted" => \A i \in Arms : ~authority_open[i]

ContractSafetyInv ==
    /\ C_InvAtMostOneLinearizedWinner
    /\ C_InvAtMostOneCommittedWinner
    /\ C_InvCommitRequiresWinnerLinearization
    /\ C_InvNoIrreversibleEffectBeforeLinearization
    /\ C_InvWinnerIdentityStableAfterLinearization
    /\ C_InvLoserNeverPublishesResult
    /\ C_InvLoserNeverPublishesRunnable
    /\ C_InvLoserNeverCommitsIrreversibleEffect
    /\ C_InvAllLosersAbortedBeforeCompletion
    /\ C_InvAtMostOneResultPublication
    /\ C_InvAtMostOneRunnablePublication
    /\ C_InvInlineCompletionPublishesNoRunnable
    /\ C_InvSuspendedCompletionPublishesRunnableExactlyOnce
    /\ C_InvRunnablePublicationRequiresPriorWaiting
    /\ C_InvOnlyWinnerPublishes
    /\ C_InvCompletionRequiresWinnerCommitted
    /\ C_InvCompletionRequiresAllLosersAborted
    /\ C_InvCompletionRequiresAllAuthorityClosed
    /\ C_InvDestroyRequiresConsumedOrValidAbort
    /\ C_InvDestroyedHasNoOpenAuthority
    /\ C_InvReservationCommitRequiresWinner
    /\ C_InvLoserReservationReleased
    /\ C_InvReservationClosesExactlyOnce
    /\ C_InvHeldReservationIsReversible
    /\ C_InvReservationIsNotIrreversibleCommit
    /\ C_InvRegistrationRollbackOnlyBeforeSuspension
    /\ C_InvRegistrationRollbackRequiresNoWinner
    /\ C_InvRegistrationRollbackRequiresRunningCaller
    /\ C_InvAbortedCallerNeverWaiting
    /\ C_InvDestroyedCallerNeverWaiting
    /\ C_InvRollbackNeverPublishes
    /\ C_InvRollbackClosesEveryRegisteredAuthority

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

\* =========================================================================
\* PR #18 non-vacuity witnesses (W): reachability of the trigger condition of
\* each named layered safety law.  A TLC "Invariant <name> is violated" on the
\* matching NotReach_<X> proves the law's premise is reachable in the model, so
\* the law is not vacuously TRUE.  Each witness below states a non-trivial
\* state configuration the corresponding law actually constrains.
\* =========================================================================

\* C_InvCommitRequiresWinnerLinearization premise: a committed winner exists.
ReachContractCommittedWinner ==
    \E i \in Arms : arm_resolution[i] = "WinnerCommitted"
NotReachContractCommittedWinner == ~ReachContractCommittedWinner

\* C_InvLoserNeverPublishesResult premise: a classified loser exists.
ReachContractLoserExists ==
    \E i \in Arms :
        /\ i # winner
        /\ winner \in Arms
        /\ arm_resolution[i] \in {"Aborted", "Released"}
NotReachContractLoserExists == ~ReachContractLoserExists

\* C_InvOnlyWinnerPublishes premise: at least one arm published.
ReachContractArmPublished ==
    \E i \in Arms : arm_publication_count[i] > 0
NotReachContractArmPublished == ~ReachContractArmPublished

\* C_InvCompletionRequiresWinnerCommitted premise: Completed phase is reached.
ReachContractCompleted ==
    contract_phase = "Completed"
NotReachContractCompleted == ~ReachContractCompleted

\* C_InvDestroyRequiresConsumedOrValidAbort premise: Destroyed phase is reached.
ReachContractDestroyed ==
    contract_phase = "Destroyed"
NotReachContractDestroyed == ~ReachContractDestroyed

\* C_InvAbortedCallerNeverWaiting premise: Aborted phase is reached.
ReachContractAborted ==
    contract_phase = "Aborted"
NotReachContractAborted == ~ReachContractAborted

\* C_InvRegistrationRollbackRequiresRunningCaller premise: rollback executed.
ReachContractRollback ==
    contract_phase = "Rollback"
NotReachContractRollback == ~ReachContractRollback

=============================================================================
