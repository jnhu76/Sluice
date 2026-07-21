------------------------ MODULE E13SelectContractNeg ------------------------
(*
  PR #18 focused Contract negative models (R: NEG-C1..NEG-C8).

  This module wraps the canonical E13SelectContract by INSTANCE-WITH, where
  every Contract variable is bound to the same-named wrapper variable.  The
  canonical ContractNext is reused unchanged via the INSTANCE; the wrapper's
  Next disjoins it with ONE focused fault action selected by the constant
  FAULT.  Each fault action is REACHABLE from a legal Contract state and
  mutates state so that exactly its target named invariant fails.

  Rules (Q):
  - one focused fault per negative model;
  - canonical ContractSpec / ContractInv are NOT modified;
  - every fault action is reachable from a legal state;
  - the expected target invariant fails when the fault is enabled;
  - fault disabled (FAULT = "None") restores PASS;
  - no ASSUME FALSE, no Init crippling, no state constraint hiding behavior.

  Each cfg selects a FAULT and asserts the matching target invariant by name.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Arms, MaxArms, FAULT

NoArm == 1000000

VARIABLES
    contract_phase, arm_registered, readiness_evidence, reservation_state,
    arm_resolution, authority_open, winner, caller_state, completion_mode,
    result_publication_count, runnable_publication_count,
    arm_publication_count, reservation_close_count,
    linearized_winner, linearized_winner_valid, fault_used

Base == INSTANCE E13SelectContract
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
             linearized_winner_valid <- linearized_winner_valid

\* All canonical Contract state plus the fault marker.
CNegVars ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      arm_resolution, authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count,
      linearized_winner, linearized_winner_valid, fault_used>>

CNegInit ==
    /\ Base!ContractInit
    /\ fault_used = FALSE

FaultActive(name) == FAULT = name

\* Common helper: "no Contract arm is mid-commit/winner yet" pre-state for
\* the irreversible-commit-before-linearization fault.
PreLinearization ==
    /\ contract_phase = "Selecting"
    /\ winner = NoArm

CNegStateExceptResolution ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count,
      linearized_winner, linearized_winner_valid>>

\* ----- NEG-C1: irreversible commit before winner linearization -----------
Fault_C1 ==
    /\ FaultActive("C1")
    /\ ~fault_used
    /\ PreLinearization
    /\ \E i \in Arms :
          /\ arm_registered[i]
          /\ arm_resolution[i] = "Open"
          /\ arm_resolution' =
                [arm_resolution EXCEPT ![i] = "WinnerCommitted"]
    /\ fault_used' = TRUE
    /\ UNCHANGED CNegStateExceptResolution

\* ----- NEG-C2: two committed winners -------------------------------------
Fault_C2 ==
    /\ FaultActive("C2")
    /\ ~fault_used
    /\ \E i, j \in Arms :
          /\ i # j
          /\ arm_resolution[i] = "WinnerCommitted"
          /\ winner = i
          /\ arm_registered[j]
          /\ arm_resolution[j] = "Open"
          /\ arm_resolution' =
                [arm_resolution EXCEPT ![j] = "WinnerCommitted"]
    /\ fault_used' = TRUE
    /\ UNCHANGED CNegStateExceptResolution

\* ----- NEG-C3: loser publishes result ------------------------------------
Fault_C3 ==
    /\ FaultActive("C3")
    /\ ~fault_used
    /\ \E i \in Arms :
          /\ arm_registered[i]
          /\ arm_resolution[i] = "Released"
          /\ arm_publication_count' =
                [arm_publication_count EXCEPT ![i] = 1]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open, winner,
                    caller_state, completion_mode, result_publication_count,
                    runnable_publication_count, reservation_close_count,
                    linearized_winner, linearized_winner_valid>>

\* ----- NEG-C4: complete with open arm authority --------------------------
Fault_C4 ==
    /\ FaultActive("C4")
    /\ ~fault_used
    /\ winner \in Arms
    /\ contract_phase = "Closing"
    /\ \E i \in Arms : authority_open[i]
    /\ contract_phase' = "Completed"
    /\ result_publication_count' = 1
    /\ arm_publication_count' =
          [arm_publication_count EXCEPT ![winner] = 1]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, runnable_publication_count,
                    reservation_close_count,
                    linearized_winner, linearized_winner_valid>>

\* ----- NEG-C5: loser reservation remains Held ----------------------------
\* A loser arm at terminal success keeps reservation_state = Held.
Fault_C5 ==
    /\ FaultActive("C5")
    /\ ~fault_used
    /\ winner \in Arms
    /\ contract_phase = "Completed"
    /\ \E i \in Arms :
          /\ i # winner
          /\ arm_registered[i]
          /\ reservation_state[i] = "None"
          /\ reservation_state' =
                [reservation_state EXCEPT ![i] = "Held"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count,
                    linearized_winner, linearized_winner_valid>>

\* ----- NEG-C6: registration rollback begins after caller Waiting ----------
\* Simulate the PR #17 P0-1 historical fault: BeginRollback from a Waiting
\* caller.  The canonical Contract forbids this (domain requires Running);
\* this fault violates C_InvRegistrationRollbackRequiresRunningCaller and
\* C_InvAbortedCallerNeverWaiting if allowed to finish.
Fault_C6 ==
    /\ FaultActive("C6")
    /\ ~fault_used
    /\ contract_phase = "Selecting"
    /\ caller_state = "Waiting"
    /\ winner = NoArm
    /\ contract_phase' = "Rollback"
    /\ fault_used' = TRUE
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count,
                    linearized_winner, linearized_winner_valid>>

\* ----- NEG-C7: Aborted operation retains Waiting caller ------------------
\* PR #17 P0-1 regression target.  From a legally-reached Aborted terminal
\* (caller Running), the fault spuriously sets caller_state = "Waiting".
\* This violates C_InvAbortedCallerNeverWaiting.
Fault_C7 ==
    /\ FaultActive("C7")
    /\ ~fault_used
    /\ contract_phase = "Aborted"
    /\ caller_state = "Running"
    /\ caller_state' = "Waiting"
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open, winner,
                    completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count,
                    linearized_winner, linearized_winner_valid>>

\* ----- NEG-C8: destroy before consumed or valid abort --------------------
\* Destroy the operation directly from Completed (a non-terminal-for-destroy
\* phase).  The canonical ContractDestroyOperation requires Consumed/Aborted.
\* This violates C_InvDestroyRequiresConsumedOrValidAbort because Completed
\* with completion_mode # None would satisfy the law; the fault uses
\* completion_mode = None to make Destroy illegal.
Fault_C8 ==
    /\ FaultActive("C8")
    /\ ~fault_used
    /\ contract_phase = "Completed"
    /\ completion_mode = "Inline"
    /\ completion_mode' = "None"
    /\ contract_phase' = "Destroyed"
    /\ fault_used' = TRUE
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    result_publication_count, runnable_publication_count,
                    arm_publication_count, reservation_close_count,
                    linearized_winner, linearized_winner_valid>>

\* ----- NEG-C9: winner identity flip after linearization -------------------
\* A legal ContractLinearizeWinner has stamped linearized_winner = A and
\* linearized_winner_valid = TRUE.  The fault flips the live `winner` to a
\* different registered arm B (B # A) while leaving the frozen linearization
\* history unchanged.  This violates C_InvWinnerIdentityStableAfterLinearization
\* (linearized_winner_valid => winner = linearized_winner) WITHOUT tripping the
\* commit or no-irreversible-effect laws (no arm is WinnerCommitted).
Fault_C9 ==
    /\ FaultActive("C9")
    /\ ~fault_used
    /\ linearized_winner_valid
    /\ linearized_winner \in Arms
    /\ \E b \in Arms :
          /\ b # linearized_winner
          /\ arm_registered[b]
          /\ \A i \in Arms : arm_resolution[i] # "WinnerCommitted"
          /\ winner' = b
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open,
                    caller_state, completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count,
                    linearized_winner, linearized_winner_valid>>

FaultNext ==
    \/ Fault_C1
    \/ Fault_C2
    \/ Fault_C3
    \/ Fault_C4
    \/ Fault_C5
    \/ Fault_C6
    \/ Fault_C7
    \/ Fault_C8
    \/ Fault_C9

CNegStutter == UNCHANGED CNegVars

\* The canonical ContractNext does not touch fault_used; freeze it here.
BaseNextFrozen == Base!ContractNext /\ UNCHANGED fault_used

CNegNext ==
    \/ CNegStutter
    \/ BaseNextFrozen
    \/ FaultNext

CNegSpec == CNegInit /\ [][CNegNext]_CNegVars

\* Re-export the canonical safety invariants by name so each NEG cfg can
\* assert its target invariant.
C_InvAtMostOneLinearizedWinner == Base!C_InvAtMostOneLinearizedWinner
C_InvAtMostOneCommittedWinner == Base!C_InvAtMostOneCommittedWinner
C_InvCommitRequiresWinnerLinearization == Base!C_InvCommitRequiresWinnerLinearization
C_InvNoIrreversibleEffectBeforeLinearization == Base!C_InvNoIrreversibleEffectBeforeLinearization
C_InvWinnerIdentityStableAfterLinearization == Base!C_InvWinnerIdentityStableAfterLinearization
C_InvLoserNeverPublishesResult == Base!C_InvLoserNeverPublishesResult
C_InvCompletionRequiresAllAuthorityClosed == Base!C_InvCompletionRequiresAllAuthorityClosed
C_InvLoserReservationReleased == Base!C_InvLoserReservationReleased
C_InvRegistrationRollbackRequiresRunningCaller == Base!C_InvRegistrationRollbackRequiresRunningCaller
C_InvAbortedCallerNeverWaiting == Base!C_InvAbortedCallerNeverWaiting
C_InvDestroyRequiresConsumedOrValidAbort == Base!C_InvDestroyRequiresConsumedOrValidAbort

\* Positive restoration: with FAULT = "None", the full ContractSafetyInv
\* (and canonical ContractInv) must PASS over the same state space.
ContractSafetyInv == Base!ContractSafetyInv
ContractInv == Base!ContractInv

\* Reachability witness: the fault was actually used (non-deadlock evidence).
FaultUsedWitness == fault_used

=============================================================================
