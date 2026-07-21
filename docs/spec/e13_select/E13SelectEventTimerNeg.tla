------------------------ MODULE E13SelectEventTimerNeg ------------------------
(*
  PR #18 focused Event/Timer/Accounting negative models (T/U/V).

  Wraps the canonical E13SelectEventTimer by INSTANCE-WITH and adds ONE
  focused fault per FAULT selector.  Same architecture as the Contract/Central
  neg modules.  The adapter has many variables; each fault explicitly mutates
  only the few it targets and freezes everything else via the canonical
  EventTimerVars / AccountingVars / HistoryVars tuples plus the remaining
  adapter scalars.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Arms, MaxArms, Events, FAULT

NoArm == 1000000
NoEvent == 2000000
NoStep == 0

VARIABLES
    \* Contract projection
    contract_phase, arm_registered, readiness_evidence, reservation_state,
    arm_resolution, authority_open, winner, caller_state, completion_mode,
    result_publication_count, runnable_publication_count,
    arm_publication_count, reservation_close_count,
    linearized_winner, linearized_winner_valid,
    \* Central projection
    central_phase, candidate_ready, claim_candidates, arm_class, claim_mode,
    claim_snapshot_frozen, claim_snapshot_frozen_valid,
    \* Adapter
    arm_kind, arm_index, adapter_phase, arm_event, wait_outcome, wait_linked,
    context_state, timer_state, timer_due, timer_node_deref,
    timer_skip_observed, waiting_account_open, timer_account_open, event_state,
    admission_checked, scan_phase, held_event, scan_remaining,
    coordination_kind, coordination_arm, last_broadcast_event,
    finalization_step, rollback_started, select_result_published,
    registration_count, registered_arm_count, retired_arm_count, now,
    \* Accounting + History
    wait_account_open_count, wait_account_close_count,
    timer_account_open_count, timer_account_close_count,
    group_id, claim_epoch, broadcast_epoch, timer_pump_epoch,
    winner_linearization_step, commit_step, terminal_step, unlink_step,
    timer_transition_step, account_close_step, authority_close_step,
    publication_step, global_step,
    fault_used

Base == INSTANCE E13SelectEventTimer
        WITH Arms <- Arms, MaxArms <- MaxArms, Events <- Events,
             contract_phase <- contract_phase,
             arm_registered <- arm_registered,
             readiness_evidence <- readiness_evidence,
             reservation_state <- reservation_state,
             arm_resolution <- arm_resolution,
             authority_open <- authority_open,
             winner <- winner, caller_state <- caller_state,
             completion_mode <- completion_mode,
             result_publication_count <- result_publication_count,
             runnable_publication_count <- runnable_publication_count,
             arm_publication_count <- arm_publication_count,
             reservation_close_count <- reservation_close_count,
             linearized_winner <- linearized_winner,
             linearized_winner_valid <- linearized_winner_valid,
             claim_snapshot_frozen <- claim_snapshot_frozen,
             claim_snapshot_frozen_valid <- claim_snapshot_frozen_valid,
             central_phase <- central_phase,
             candidate_ready <- candidate_ready,
             claim_candidates <- claim_candidates,
             arm_class <- arm_class, claim_mode <- claim_mode,
             arm_kind <- arm_kind, arm_index <- arm_index,
             adapter_phase <- adapter_phase, arm_event <- arm_event,
             wait_outcome <- wait_outcome, wait_linked <- wait_linked,
             context_state <- context_state, timer_state <- timer_state,
             timer_due <- timer_due, timer_node_deref <- timer_node_deref,
             timer_skip_observed <- timer_skip_observed,
             waiting_account_open <- waiting_account_open,
             timer_account_open <- timer_account_open,
             event_state <- event_state, admission_checked <- admission_checked,
             scan_phase <- scan_phase, held_event <- held_event,
             scan_remaining <- scan_remaining,
             coordination_kind <- coordination_kind,
             coordination_arm <- coordination_arm,
             last_broadcast_event <- last_broadcast_event,
             finalization_step <- finalization_step,
             rollback_started <- rollback_started,
             select_result_published <- select_result_published,
             registration_count <- registration_count,
             registered_arm_count <- registered_arm_count,
             retired_arm_count <- retired_arm_count, now <- now,
             wait_account_open_count <- wait_account_open_count,
             wait_account_close_count <- wait_account_close_count,
             timer_account_open_count <- timer_account_open_count,
             timer_account_close_count <- timer_account_close_count,
             group_id <- group_id, claim_epoch <- claim_epoch,
             broadcast_epoch <- broadcast_epoch,
             timer_pump_epoch <- timer_pump_epoch,
             winner_linearization_step <- winner_linearization_step,
             commit_step <- commit_step, terminal_step <- terminal_step,
             unlink_step <- unlink_step,
             timer_transition_step <- timer_transition_step,
             account_close_step <- account_close_step,
             authority_close_step <- authority_close_step,
             publication_step <- publication_step,
             global_step <- global_step

\* All canonical EventTimer state (everything except fault_used), listed flat
\* so UNCHANGED ETAllState / UNCHANGED ETNegVars do not depend on evaluating
\* the Base instance's tuple operators (which TLC refuses inside UNCHANGED
\* because they indirect through the WITH binding).  Keep this list in 1:1
\* correspondence with EventTimerVars ++ AccountingVars ++ HistoryVars.
ETContractProj ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      arm_resolution, authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count,
      linearized_winner, linearized_winner_valid>>
ETCentralProj ==
    <<central_phase, candidate_ready, claim_candidates, arm_class, claim_mode,
      claim_snapshot_frozen, claim_snapshot_frozen_valid>>
ETAdapterVars ==
    <<arm_kind, arm_index, adapter_phase, arm_event, wait_outcome, wait_linked,
      context_state, timer_state, timer_due, timer_node_deref,
      timer_skip_observed, waiting_account_open, timer_account_open, event_state,
      admission_checked, scan_phase, held_event, scan_remaining,
      coordination_kind, coordination_arm, last_broadcast_event,
      finalization_step, rollback_started, select_result_published,
      registration_count, registered_arm_count, retired_arm_count, now>>
ETAccountingVars ==
    <<wait_account_open_count, wait_account_close_count,
      timer_account_open_count, timer_account_close_count>>
ETHistoryVars ==
    <<group_id, claim_epoch, broadcast_epoch, timer_pump_epoch,
      winner_linearization_step, commit_step, terminal_step, unlink_step,
      timer_transition_step, account_close_step, authority_close_step,
      publication_step, global_step>>

ETAllState == <<ETContractProj, ETCentralProj, ETAdapterVars,
                ETAccountingVars, ETHistoryVars>>

ETNegVars == <<ETAllState, fault_used>>

ETNegInit ==
    /\ Base!EventTimerInit
    /\ fault_used = FALSE

FaultActive(name) == FAULT = name

\* Helper: freeze every variable EXCEPT those explicitly primed by the fault.
FreezeETAll == UNCHANGED ETAllState

\* ---- NEG-E1: post-arming readiness bypasses broadcast protocol ----------
\* During the Registering phase an arm is falsely promoted to candidate_ready
\* before FinishRegistration has run.  In the canonical model candidate_ready
\* can only become true via AdmissionObserveReady or ScanEventArm, both of
\* which require adapter_phase[i] = "Registered".  The fault promotes an arm
\* whose adapter_phase[i] = "Registering" -- not in the permitted
\* {"Registered","TimerCancelled","Finalized","Retired"} set -- and so violates
\* E_InvNoReadinessBypassesPermittedEventPath.
Fault_E1 ==
    /\ FaultActive("E1")
    /\ ~fault_used
    /\ central_phase = "Registering"
    /\ coordination_kind = "None"
    /\ \E i \in Arms :
          /\ adapter_phase[i] = "Registering"
          /\ arm_registered[i]
          /\ ~candidate_ready[i]
          /\ candidate_ready' = [candidate_ready EXCEPT ![i] = TRUE]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETAdapterVars, ETAccountingVars,
                    ETHistoryVars, central_phase, claim_candidates,
                    arm_class, claim_mode, claim_snapshot_frozen,
                    claim_snapshot_frozen_valid>>

\* ---- NEG-E2: broadcast scans arm belonging to another Event -------------
\* During a scan for held_event e, an arm mapped to a DIFFERENT event is
\* added to scan_remaining.  This violates
\* E_InvBroadcastScansOnlyTargetEventArms.
\* (Note: scan_remaining is primed, so ETAdapterVars cannot be in UNCHANGED.)
Fault_E2 ==
    /\ FaultActive("E2")
    /\ ~fault_used
    /\ coordination_kind = "Event"
    /\ scan_phase = "Scanning"
    /\ held_event \in Events
    /\ \E i \in Arms :
          /\ arm_kind[i] = "EventArm"
          /\ arm_event[i] # held_event
          /\ arm_event[i] \in Events
          /\ i \notin scan_remaining
          /\ scan_remaining' = scan_remaining \cup {i}
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETAccountingVars, ETHistoryVars,
                    ETCentralProj, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, retired_arm_count,
                    now>>

\* ---- NEG-E3: Phase 1 commits --------------------------------------------
\* During scan phase (Scanning), a winner is forged and immediately committed.
\* The canonical model only ever linearizes a winner through ClaimEventWinner /
\* ClaimTimerWinner, which advance central_phase out of "Armed".  The fault
\* mutates winner + arm_resolution in place to fake a winner commit while the
\* broadcast scan is still in phase 1.  This violates
\* E_InvPhaseOneDoesNotCommit
\* (scan_phase = "Scanning" => winner = NoArm \/ arm_resolution[winner] # "WinnerCommitted").
Fault_E3 ==
    /\ FaultActive("E3")
    /\ ~fault_used
    /\ scan_phase = "Scanning"
    /\ winner = NoArm
    /\ \E i \in Arms :
          /\ arm_kind[i] = "EventArm"
          /\ arm_event[i] = held_event
          /\ winner' = i
          /\ arm_resolution' = [arm_resolution EXCEPT ![i] = "WinnerCommitted"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETCentralProj, contract_phase, arm_registered,
                    readiness_evidence, reservation_state, authority_open,
                    caller_state, completion_mode, result_publication_count,
                    runnable_publication_count, arm_publication_count,
                    reservation_close_count, linearized_winner,
                    linearized_winner_valid, ETAdapterVars, ETAccountingVars,
                    ETHistoryVars>>

\* ---- NEG-E4: Select consumes persistent Event SET -----------------------
\* Select sets event_state back to Unset for an Event that has already been
\* broadcast (last_broadcast_event = e).  This violates
\* E_InvEventPersistentSetNotConsumed.
\* (event_state is primed; ETAdapterVars cannot be in UNCHANGED.)
Fault_E4 ==
    /\ FaultActive("E4")
    /\ ~fault_used
    /\ \E e \in Events :
          /\ e = last_broadcast_event
          /\ event_state[e] = "Set"
          /\ event_state' = [event_state EXCEPT ![e] = "Unset"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, arm_kind, arm_index,
                    adapter_phase, arm_event, wait_outcome, wait_linked,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, admission_checked, scan_phase,
                    held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, retired_arm_count,
                    now, ETAccountingVars, ETHistoryVars>>

\* ---- NEG-E5: Phase 1 publishes -----------------------------------------
\* During scan phase (Scanning), a result is published (result_publication_count
\* becomes 1).  The canonical model only publishes from ProcessGroups / inline
\* / suspended complete actions, never from Scanning.  This violates
\* E_InvPhaseOneDoesNotPublish (scan_phase = "Scanning" => result_publication_count = 0).
\* (result_publication_count, arm_publication_count are primed, so ETContractProj
\* cannot be in UNCHANGED; we list its other members individually.)
Fault_E5 ==
    /\ FaultActive("E5")
    /\ ~fault_used
    /\ scan_phase = "Scanning"
    /\ result_publication_count = 0
    /\ \E i \in Arms :
          /\ arm_kind[i] = "EventArm"
          /\ arm_event[i] = held_event
          /\ result_publication_count' = 1
          /\ arm_publication_count' = [arm_publication_count EXCEPT ![i] = 1]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETCentralProj, contract_phase, arm_registered,
                    readiness_evidence, reservation_state, arm_resolution,
                    authority_open, winner, caller_state, completion_mode,
                    runnable_publication_count, reservation_close_count,
                    linearized_winner, linearized_winner_valid, ETAdapterVars,
                    ETAccountingVars, ETHistoryVars>>

\* ---- NEG-E6: shared Event cross-group processing publishes wrong group ---
\* Single-group reduction: an Event broadcast publishes a result for an arm
\* that is NOT an Event arm of the held event (cross-group contamination).
Fault_E6 ==
    /\ FaultActive("E6")
    /\ ~fault_used
    /\ coordination_kind = "Event"
    /\ scan_phase = "ProcessGroups"
    /\ held_event = NoEvent
    /\ \E i \in Arms :
          /\ arm_kind[i] = "TimerArm"
          /\ arm_publication_count' =
                [arm_publication_count EXCEPT ![i] = 1]
          /\ result_publication_count' = 1
    /\ fault_used' = TRUE
    /\ UNCHANGED <<contract_phase, arm_registered, readiness_evidence,
                    reservation_state, arm_resolution, authority_open, winner,
                    caller_state, completion_mode, runnable_publication_count,
                    reservation_close_count, linearized_winner,
                    linearized_winner_valid, ETCentralProj, arm_kind, arm_index,
                    adapter_phase, arm_event, wait_outcome, wait_linked,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, retired_arm_count,
                    now, ETAccountingVars, ETHistoryVars>>

\* ---- NEG-T1: Retired registration dereferences WaitNode -----------------
\* A Timer arm whose registration is Retired gets timer_node_deref forced to 2
\* (out of the legal {0,1} window).  The canonical TimerPumpEntry requires
\* Active and never derefs a Retired registration; the invariant allows
\* timer_node_deref in {0,1}.  The fault forces a deref past the window,
\* violating T_InvRetiredRegistrationNeverDereferences regardless of the
\* starting value.
\* (timer_node_deref is primed; ETAdapterVars cannot be in UNCHANGED.)
Fault_T1 ==
    /\ FaultActive("T1")
    /\ ~fault_used
    /\ \E i \in Arms :
          /\ arm_kind[i] = "TimerArm"
          /\ timer_state[i] = "Retired"
          /\ timer_node_deref[i] < 2
          /\ timer_node_deref' = [timer_node_deref EXCEPT ![i] = 2]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, arm_kind, arm_index,
                    adapter_phase, arm_event, wait_outcome, wait_linked,
                    context_state, timer_state, timer_due, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm, last_broadcast_event,
                    finalization_step, rollback_started, select_result_published,
                    registration_count, registered_arm_count, retired_arm_count,
                    now, ETAccountingVars, ETHistoryVars>>

\* ---- NEG-T2: due observation consumes before claim ----------------------
\* A Timer arm whose registration is Active gets timer_state Consumed without
\* a prior claim (winner linearization).
Fault_T2 ==
    /\ FaultActive("T2")
    /\ ~fault_used
    /\ winner = NoArm
    /\ \E i \in Arms :
          /\ arm_kind[i] = "TimerArm"
          /\ timer_state[i] = "Active"
          /\ timer_state' = [timer_state EXCEPT ![i] = "Consumed"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, arm_kind, arm_index,
                    adapter_phase, arm_event, wait_outcome, wait_linked,
                    context_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, retired_arm_count,
                    now, ETAccountingVars, ETHistoryVars>>

\* ---- NEG-T3: Timer arm retired without being a loser or rollback --------
\* A Timer arm that is NOT classified as a Loser (it is Unclassified or
\* Winner) has its registration forced to Retired without going through
\* CancelTimerLoser / RollbackRetireTimer.  This violates
\* T_InvRetireRequiresLoserOrRollback
\* (Retired + arm_registered => Loser or finalization_step="Rollback").
\* (timer_state is primed; ETAdapterVars cannot be in UNCHANGED.)
Fault_T3 ==
    /\ FaultActive("T3")
    /\ ~fault_used
    /\ \E i \in Arms :
          /\ arm_kind[i] = "TimerArm"
          /\ arm_registered[i]
          /\ timer_state[i] = "Active"
          /\ arm_class[i] # "Loser"
          /\ timer_state' = [timer_state EXCEPT ![i] = "Retired"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, arm_kind, arm_index,
                    adapter_phase, arm_event, wait_outcome, wait_linked,
                    context_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, retired_arm_count,
                    now, ETAccountingVars, ETHistoryVars>>

\* ---- NEG-T4: pump authority Retired mid-coordination -------------------
\* While a Timer pump coordination is in flight (coordination_kind = "Timer"),
\* the pump arm's registration is forced to Retired.  The canonical
\* ConsumeTimerWinner only flips Active -> Consumed for the linearized winner;
\* retiring mid-pump violates T_InvActiveStableDuringPumpAuthority
\* (coordination_kind = "Timer" /\ coordination_arm \in Arms
\*     => timer_state[coordination_arm] \in {"Active", "Consumed"}).
\* (timer_state is primed; ETAdapterVars cannot be in UNCHANGED.)
Fault_T4 ==
    /\ FaultActive("T4")
    /\ ~fault_used
    /\ coordination_kind = "Timer"
    /\ coordination_arm \in Arms
    /\ timer_state[coordination_arm] \in {"Active", "Consumed"}
    /\ timer_state' =
          [timer_state EXCEPT ![coordination_arm] = "Retired"]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, arm_kind, arm_index,
                    adapter_phase, arm_event, wait_outcome, wait_linked,
                    context_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, retired_arm_count,
                    now, ETAccountingVars, ETHistoryVars>>

\* ---- NEG-T5: stale-pump Retired loser publishes ------------------------
\* A Timer arm reached Retired via a stale-pump skip (timer_skip_observed) is
\* forced to publish (arm_publication_count = 1).  The canonical TimerPumpSkip
\* never publishes a result; a Retired loser registration has zero publication.
\* This violates T_InvStalePumpNeverPublishes.
\* (arm_publication_count is primed; ETContractProj cannot be in UNCHANGED.)
Fault_T5 ==
    /\ FaultActive("T5")
    /\ ~fault_used
    /\ \E i \in Arms :
          /\ arm_kind[i] = "TimerArm"
          /\ timer_skip_observed[i]
          /\ timer_state[i] = "Retired"
          /\ arm_publication_count' = [arm_publication_count EXCEPT ![i] = 1]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETCentralProj, contract_phase, arm_registered,
                    readiness_evidence, reservation_state, arm_resolution,
                    authority_open, winner, caller_state, completion_mode,
                    result_publication_count, runnable_publication_count,
                    reservation_close_count, linearized_winner,
                    linearized_winner_valid, ETAdapterVars, ETAccountingVars,
                    ETHistoryVars>>

\* ---- NEG-A1: waiting accounting closes twice ----------------------------
\* An arm with wait_account_close_count = 1 gets a second close.
Fault_A1 ==
    /\ FaultActive("A1")
    /\ ~fault_used
    /\ \E i \in Arms :
          /\ wait_account_close_count[i] = 1
          /\ wait_account_close_count' =
                [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, ETAdapterVars,
                    wait_account_open_count, timer_account_open_count,
                    timer_account_close_count, ETHistoryVars>>

\* ---- NEG-A2: timer accounting closes twice ------------------------------
Fault_A2 ==
    /\ FaultActive("A2")
    /\ ~fault_used
    /\ \E i \in Arms :
          /\ timer_account_close_count[i] = 1
          /\ timer_account_close_count' =
                [timer_account_close_count EXCEPT ![i] = @ + 1]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, ETAdapterVars,
                    wait_account_open_count, wait_account_close_count,
                    timer_account_open_count, ETHistoryVars>>

\* ---- NEG-A3: operation completes with accounting open ------------------
\* The operation reaches Completed while an arm has an open wait account.
Fault_A3 ==
    /\ FaultActive("A3")
    /\ ~fault_used
    /\ contract_phase = "Closing"
    /\ winner \in Arms
    /\ \E i \in Arms : waiting_account_open[i]
    /\ contract_phase' = "Completed"
    /\ result_publication_count' = 1
    /\ arm_publication_count' = [arm_publication_count EXCEPT ![winner] = 1]
    /\ completion_mode' = "Inline"
    /\ fault_used' = TRUE
    /\ UNCHANGED <<arm_registered, readiness_evidence, reservation_state,
                    arm_resolution, authority_open, winner, caller_state,
                    runnable_publication_count, reservation_close_count,
                    linearized_winner, linearized_winner_valid, ETCentralProj,
                    ETAdapterVars, ETAccountingVars, ETHistoryVars>>

\* ---- NEG-A4: accounting counter underflow -------------------------------
\* An arm with close_count = 0, open_count = 0 gets close_count = 1 (close
\* without open => underflow: close > open).
Fault_A4 ==
    /\ FaultActive("A4")
    /\ ~fault_used
    /\ \E i \in Arms :
          /\ wait_account_open_count[i] = 0
          /\ wait_account_close_count[i] = 0
          /\ wait_account_close_count' =
                [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<ETContractProj, ETCentralProj, ETAdapterVars,
                    wait_account_open_count, timer_account_open_count,
                    timer_account_close_count, ETHistoryVars>>

FaultNext ==
    \/ Fault_E1 \/ Fault_E2 \/ Fault_E3 \/ Fault_E4 \/ Fault_E5 \/ Fault_E6
    \/ Fault_T1 \/ Fault_T2 \/ Fault_T3 \/ Fault_T4 \/ Fault_T5
    \/ Fault_A1 \/ Fault_A2 \/ Fault_A3 \/ Fault_A4

ETNegStutter == UNCHANGED ETNegVars

BaseNextFrozen == Base!EventTimerNext /\ UNCHANGED fault_used

ETNegNext ==
    \/ ETNegStutter
    \/ BaseNextFrozen
    \/ FaultNext

ETNegSpec == ETNegInit /\ [][ETNegNext]_ETNegVars

\* Re-export adapter safety invariants by name for each NEG cfg.
E_InvNoReadinessBypassesPermittedEventPath == Base!E_InvNoReadinessBypassesPermittedEventPath
E_InvBroadcastScansOnlyTargetEventArms == Base!E_InvBroadcastScansOnlyTargetEventArms
E_InvPhaseOneDoesNotCommit == Base!E_InvPhaseOneDoesNotCommit
E_InvPhaseOneDoesNotPublish == Base!E_InvPhaseOneDoesNotPublish
E_InvEventPersistentSetNotConsumed == Base!E_InvEventPersistentSetNotConsumed
\* NEG-E6 targets the Contract-layer winner-only publication law, reached via
\* the two-step refinement chain adapter -> central -> contract.  The adapter
\* module's AdapterSafetyInv does not re-assert this law directly because it is
\* already part of ContractSafetyInv; the NEG model needs the named re-export
\* below so the E6 cfg can name a single invariant.
E_InvOnlyWinnerPublishes ==
    Base!CentralRefinement!ContractRefinement!C_InvOnlyWinnerPublishes
T_InvRetiredRegistrationNeverDereferences == Base!T_InvRetiredRegistrationNeverDereferences
T_InvConsumeRequiresWinner == Base!T_InvConsumeRequiresWinner
T_InvRetireRequiresLoserOrRollback == Base!T_InvRetireRequiresLoserOrRollback
T_InvActiveStableDuringPumpAuthority == Base!T_InvActiveStableDuringPumpAuthority
T_InvConsumedRegistrationNeverDereferences == Base!T_InvConsumedRegistrationNeverDereferences
T_InvStalePumpNeverOffers == Base!T_InvStalePumpNeverOffers
T_InvStalePumpNeverPublishes == Base!T_InvStalePumpNeverPublishes
A_InvWaitAccountClosesAtMostOnce == Base!A_InvWaitAccountClosesAtMostOnce
A_InvTimerAccountClosesAtMostOnce == Base!A_InvTimerAccountClosesAtMostOnce
A_InvCompletedHasNoOpenAccounting == Base!A_InvCompletedHasNoOpenAccounting
A_InvNoAccountingUnderflow == Base!A_InvNoAccountingUnderflow
AdapterSafetyInv == Base!AdapterSafetyInv

=============================================================================
