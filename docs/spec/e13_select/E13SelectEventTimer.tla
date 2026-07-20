------------------------- MODULE E13SelectEventTimer -------------------------
(*
  First-scope adapters for the Central Claim strategy:
    - persistent Event arms with real Event identity and two-phase broadcast
    - independent Timer arms with TimerRegistration authority

  This module owns adapter detail only.  Its top-level INSTANCE is the explicit
  refinement mapping to E13SelectCentralClaim.  R1-R12 are causal reachability
  witnesses over concrete action history, not terminal-state lookalikes.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Arms, MaxArms, Events

NoArm == 1000000
NoEvent == 2000000

VARIABLES
    \* Contract projection.
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

    \* Central Claim projection.
    central_phase,
    candidate_ready,
    claim_candidates,
    arm_class,
    claim_mode,

    \* Event/Timer adapter state.
    arm_kind,
    arm_index,
    adapter_phase,
    arm_event,
    wait_outcome,
    wait_linked,
    context_state,
    timer_state,
    timer_due,
    timer_node_deref,
    timer_skip_observed,
    waiting_account_open,
    timer_account_open,
    event_state,
    admission_checked,
    scan_phase,
    held_event,
    scan_remaining,
    coordination_kind,
    coordination_arm,
    last_broadcast_event,
    finalization_step,
    rollback_started,
    select_result_published,
    registration_count,
    registered_arm_count,
    retired_arm_count,
    now

ContractProjectionVars ==
    <<contract_phase, arm_registered, readiness_evidence, reservation_state,
      arm_resolution, authority_open, winner, caller_state, completion_mode,
      result_publication_count, runnable_publication_count,
      arm_publication_count, reservation_close_count>>

CentralOnlyProjectionVars ==
    <<central_phase, candidate_ready, claim_candidates, arm_class, claim_mode>>

CentralProjectionVars ==
    <<ContractProjectionVars, CentralOnlyProjectionVars>>

AdapterVars ==
    <<arm_kind, arm_index, adapter_phase, arm_event, wait_outcome, wait_linked,
      context_state, timer_state, timer_due, timer_node_deref,
      timer_skip_observed, waiting_account_open, timer_account_open,
      event_state, admission_checked, scan_phase, held_event, scan_remaining,
      coordination_kind, coordination_arm, last_broadcast_event,
      finalization_step, rollback_started, select_result_published,
      registration_count, registered_arm_count, retired_arm_count, now>>

EventTimerVars == <<CentralProjectionVars, AdapterVars>>

CentralRefinement == INSTANCE E13SelectCentralClaim WITH
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
    central_phase <- central_phase,
    candidate_ready <- candidate_ready,
    claim_candidates <- claim_candidates,
    arm_class <- arm_class,
    claim_mode <- claim_mode

ArmKindT == {"None", "EventArm", "TimerArm"}
AdapterPhaseT ==
    {"Detached", "Registering", "Registered", "TimerCancelled",
     "Finalized", "Retired"}
WaitOutcomeT == {"None", "Pending", "Woken", "Expired", "Cancelled"}
ContextStateT == {"None", "Installed", "Cleared"}
TimerStateT == {"None", "Active", "Consumed", "Retired"}
EventStateT == {"Unset", "Set"}
ScanPhaseT == {"NoScan", "Scanning", "ProcessGroups"}
CoordinationKindT == {"None", "Event", "Timer"}
FinalizationStepT ==
    {"None", "EventWinner", "TimerConsumed", "TimerWinner",
     "EventLoser", "TimerCancelled", "TimerLoser", "Rollback"}

ReadySource(i) ==
    \/ /\ arm_kind[i] = "EventArm"
       /\ arm_event[i] \in Events
       /\ event_state[arm_event[i]] = "Set"
    \/ /\ arm_kind[i] = "TimerArm"
       /\ timer_due[i]

AllAdmissionChecked == \A i \in Arms : admission_checked[i]

EventTimerTypeOK ==
    /\ arm_kind \in [Arms -> ArmKindT]
    /\ arm_index \in [Arms -> Arms]
    /\ adapter_phase \in [Arms -> AdapterPhaseT]
    /\ arm_event \in [Arms -> Events \cup {NoEvent}]
    /\ wait_outcome \in [Arms -> WaitOutcomeT]
    /\ wait_linked \in [Arms -> BOOLEAN]
    /\ context_state \in [Arms -> ContextStateT]
    /\ timer_state \in [Arms -> TimerStateT]
    /\ timer_due \in [Arms -> BOOLEAN]
    /\ timer_node_deref \in [Arms -> 0..1]
    /\ timer_skip_observed \in [Arms -> BOOLEAN]
    /\ waiting_account_open \in [Arms -> BOOLEAN]
    /\ timer_account_open \in [Arms -> BOOLEAN]
    /\ event_state \in [Events -> EventStateT]
    /\ admission_checked \in [Arms -> BOOLEAN]
    /\ scan_phase \in ScanPhaseT
    /\ held_event \in Events \cup {NoEvent}
    /\ scan_remaining \subseteq Arms
    /\ coordination_kind \in CoordinationKindT
    /\ coordination_arm \in Arms \cup {NoArm}
    /\ last_broadcast_event \in Events \cup {NoEvent}
    /\ finalization_step \in [Arms -> FinalizationStepT]
    /\ rollback_started \in BOOLEAN
    /\ select_result_published \in BOOLEAN
    /\ registration_count \in 0..MaxArms
    /\ registered_arm_count \in 0..MaxArms
    /\ retired_arm_count \in 0..MaxArms
    /\ now \in 0..3

AdapterLifecycleWellFormed ==
    /\ \A i \in Arms : arm_index[i] = i
    /\ \A i \in Arms :
        /\ (adapter_phase[i] = "Detached" =>
              /\ arm_kind[i] = "None"
              /\ wait_outcome[i] = "None"
              /\ ~wait_linked[i]
              /\ context_state[i] = "None"
              /\ timer_state[i] = "None")
        /\ (adapter_phase[i] = "Registering" =>
              /\ arm_kind[i] = "None"
              /\ wait_outcome[i] = "None"
              /\ ~wait_linked[i]
              /\ context_state[i] = "None"
              /\ timer_state[i] = "None")
        /\ (adapter_phase[i] = "Registered" =>
              /\ arm_kind[i] \in {"EventArm", "TimerArm"}
              /\ wait_outcome[i] = "Pending"
              /\ wait_linked[i]
              /\ context_state[i] = "Installed"
              /\ (arm_kind[i] = "TimerArm" =>
                    /\ timer_state[i] \in {"Active", "Consumed"}
                    /\ timer_account_open[i]))
        /\ (adapter_phase[i] = "TimerCancelled" =>
              /\ arm_kind[i] = "TimerArm"
              /\ wait_outcome[i] = "Cancelled"
              /\ ~wait_linked[i]
              /\ context_state[i] = "Installed"
              /\ timer_state[i] = "Active")
        /\ (adapter_phase[i] \in {"Finalized", "Retired"} =>
              /\ wait_outcome[i] \in {"Woken", "Expired", "Cancelled"}
              /\ ~wait_linked[i]
              /\ context_state[i] = "Cleared"
              /\ ~waiting_account_open[i]
              /\ ~timer_account_open[i])
        /\ (arm_kind[i] = "EventArm" =>
              /\ arm_event[i] \in Events
              /\ timer_state[i] = "None"
              /\ ~timer_account_open[i])
        /\ (arm_kind[i] = "TimerArm" => arm_event[i] = NoEvent)
        /\ (wait_linked[i] <=> waiting_account_open[i])
        /\ (timer_account_open[i] =>
              /\ arm_kind[i] = "TimerArm"
              /\ timer_state[i] \in {"Active", "Consumed"})
        /\ (timer_node_deref[i] > 0 => arm_kind[i] = "TimerArm")
    /\ registered_arm_count = Cardinality({i \in Arms : wait_linked[i]})
    /\ retired_arm_count =
          Cardinality({i \in Arms : adapter_phase[i] = "Retired"})
    /\ registration_count =
          Cardinality({i \in Arms :
              adapter_phase[i] \in
                  {"Registered", "TimerCancelled", "Finalized", "Retired"}})
    /\ (select_result_published <=> result_publication_count = 1)

CoordinationWellFormed ==
    /\ (coordination_kind = "None" =>
          /\ coordination_arm = NoArm
          /\ held_event = NoEvent
          /\ scan_phase = "NoScan")
    /\ (coordination_kind = "Timer" =>
          /\ coordination_arm \in Arms
          /\ held_event = NoEvent
          /\ scan_phase = "NoScan")
    /\ (coordination_kind = "Event" =>
          /\ coordination_arm = NoArm
          /\ scan_phase \in {"Scanning", "ProcessGroups"}
          /\ (scan_phase = "Scanning" => held_event \in Events)
          /\ (scan_phase = "ProcessGroups" => held_event = NoEvent))

TimerAuthorityWellFormed ==
    \A i \in Arms :
        /\ (timer_skip_observed[i] =>
              /\ timer_state[i] \in {"Retired", "Consumed"}
              /\ timer_due[i])
        /\ (timer_state[i] = "Consumed" =>
              /\ arm_class[i] = "Winner"
              /\ winner = i)
        /\ (finalization_step[i] = "TimerConsumed" =>
              /\ timer_state[i] = "Consumed"
              /\ wait_outcome[i] = "Pending"
              /\ wait_linked[i])

EventTimerInv ==
    /\ CentralRefinement!CentralInv
    /\ EventTimerTypeOK
    /\ AdapterLifecycleWellFormed
    /\ CoordinationWellFormed
    /\ TimerAuthorityWellFormed

EventTimerInit ==
    /\ CentralRefinement!CentralInit
    /\ arm_kind = [i \in Arms |-> "None"]
    /\ arm_index = [i \in Arms |-> i]
    /\ adapter_phase = [i \in Arms |-> "Detached"]
    /\ arm_event = [i \in Arms |-> NoEvent]
    /\ wait_outcome = [i \in Arms |-> "None"]
    /\ wait_linked = [i \in Arms |-> FALSE]
    /\ context_state = [i \in Arms |-> "None"]
    /\ timer_state = [i \in Arms |-> "None"]
    /\ timer_due = [i \in Arms |-> FALSE]
    /\ timer_node_deref = [i \in Arms |-> 0]
    /\ timer_skip_observed = [i \in Arms |-> FALSE]
    /\ waiting_account_open = [i \in Arms |-> FALSE]
    /\ timer_account_open = [i \in Arms |-> FALSE]
    /\ event_state = [e \in Events |-> "Unset"]
    /\ admission_checked = [i \in Arms |-> FALSE]
    /\ scan_phase = "NoScan"
    /\ held_event = NoEvent
    /\ scan_remaining = {}
    /\ coordination_kind = "None"
    /\ coordination_arm = NoArm
    /\ last_broadcast_event = NoEvent
    /\ finalization_step = [i \in Arms |-> "None"]
    /\ rollback_started = FALSE
    /\ select_result_published = FALSE
    /\ registration_count = 0
    /\ registered_arm_count = 0
    /\ retired_arm_count = 0
    /\ now = 0

BeginRegistration(i) ==
    /\ adapter_phase[i] = "Detached"
    /\ CentralRefinement!CentralRegisterArm(i)
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Registering"]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, wait_outcome, wait_linked,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count,
                    retired_arm_count, now>>

RegisterEventArm(i, e) ==
    /\ adapter_phase[i] = "Registering"
    /\ e \in Events
    /\ arm_kind' = [arm_kind EXCEPT ![i] = "EventArm"]
    /\ arm_event' = [arm_event EXCEPT ![i] = e]
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Pending"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = TRUE]
    /\ context_state' = [context_state EXCEPT ![i] = "Installed"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = TRUE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Registered"]
    /\ registration_count' = registration_count + 1
    /\ registered_arm_count' = registered_arm_count + 1
    /\ UNCHANGED <<CentralProjectionVars, arm_index, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, retired_arm_count, now>>

RegisterTimerArm(i) ==
    /\ adapter_phase[i] = "Registering"
    /\ arm_kind' = [arm_kind EXCEPT ![i] = "TimerArm"]
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Pending"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = TRUE]
    /\ context_state' = [context_state EXCEPT ![i] = "Installed"]
    /\ timer_state' = [timer_state EXCEPT ![i] = "Active"]
    /\ timer_due' = [timer_due EXCEPT ![i] = (now >= 2)]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = TRUE]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = TRUE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Registered"]
    /\ registration_count' = registration_count + 1
    /\ registered_arm_count' = registered_arm_count + 1
    /\ UNCHANGED <<CentralProjectionVars, arm_index, arm_event,
                    timer_node_deref, timer_skip_observed, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm, last_broadcast_event,
                    finalization_step, rollback_started,
                    select_result_published, retired_arm_count, now>>

FinishRegistration ==
    /\ \A i \in Arms : adapter_phase[i] = "Registered"
    /\ CentralRefinement!CentralFinishRegistration
    /\ admission_checked' = [i \in Arms |-> FALSE]
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count,
                    retired_arm_count, now>>

AdmissionObserveReady(i) ==
    /\ central_phase = "Admission"
    /\ adapter_phase[i] = "Registered"
    /\ ~admission_checked[i]
    /\ ReadySource(i)
    /\ CentralRefinement!CentralObserveCandidate(i)
    /\ admission_checked' = [admission_checked EXCEPT ![i] = TRUE]
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count,
                    retired_arm_count, now>>

AdmissionObserveNotReady(i) ==
    /\ central_phase = "Admission"
    /\ adapter_phase[i] = "Registered"
    /\ ~admission_checked[i]
    /\ ~ReadySource(i)
    /\ admission_checked' = [admission_checked EXCEPT ![i] = TRUE]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

ClaimAdmissionWinner(i) ==
    /\ central_phase = "Admission"
    /\ AllAdmissionChecked
    /\ CentralRefinement!CentralClaimWinner(i)
    /\ UNCHANGED AdapterVars

SuspendCaller ==
    /\ central_phase = "Admission"
    /\ AllAdmissionChecked
    /\ CentralRefinement!CentralSuspendCaller
    /\ UNCHANGED AdapterVars

SetEventBeforeRegistration(e) ==
    /\ central_phase = "Registering"
    /\ \A i \in Arms : ~arm_registered[i]
    /\ event_state[e] = "Unset"
    /\ event_state' = [event_state EXCEPT ![e] = "Set"]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, admission_checked, scan_phase,
                    held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count,
                    retired_arm_count, now>>

StartEventBroadcast(e) ==
    /\ central_phase = "Armed"
    /\ coordination_kind = "None"
    /\ event_state[e] = "Unset"
    /\ event_state' = [event_state EXCEPT ![e] = "Set"]
    /\ coordination_kind' = "Event"
    /\ held_event' = e
    /\ scan_phase' = "Scanning"
    /\ scan_remaining' =
          {i \in Arms : /\ adapter_phase[i] = "Registered"
                        /\ arm_kind[i] = "EventArm"
                        /\ arm_event[i] = e}
    /\ last_broadcast_event' = e
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, admission_checked, coordination_arm,
                    finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

ScanEventArm(i) ==
    /\ coordination_kind = "Event"
    /\ scan_phase = "Scanning"
    /\ i \in scan_remaining
    /\ held_event = arm_event[i]
    /\ event_state[held_event] = "Set"
    /\ CentralRefinement!CentralObserveCandidate(i)
    /\ scan_remaining' = scan_remaining \ {i}
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

FinishEventScan ==
    /\ coordination_kind = "Event"
    /\ scan_phase = "Scanning"
    /\ scan_remaining = {}
    /\ scan_phase' = "ProcessGroups"
    /\ held_event' = NoEvent
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

ClaimEventWinner(i) ==
    /\ coordination_kind = "Event"
    /\ scan_phase = "ProcessGroups"
    /\ CentralRefinement!CentralClaimWinner(i)
    /\ UNCHANGED AdapterVars

FinishEmptyEventBroadcast ==
    /\ coordination_kind = "Event"
    /\ scan_phase = "ProcessGroups"
    /\ CentralRefinement!ReadySet = {}
    /\ coordination_kind' = "None"
    /\ scan_phase' = "NoScan"
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    held_event, scan_remaining, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

TimerPumpEntry(i) ==
    /\ central_phase = "Armed"
    /\ coordination_kind = "None"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ timer_state[i] = "Active"
    /\ timer_due[i]
    /\ timer_node_deref[i] = 0
    /\ CentralRefinement!CentralObserveCandidate(i)
    /\ timer_node_deref' = [timer_node_deref EXCEPT ![i] = @ + 1]
    /\ coordination_kind' = "Timer"
    /\ coordination_arm' = i
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

ClaimTimerWinner(i) ==
    /\ coordination_kind = "Timer"
    /\ coordination_arm = i
    /\ CentralRefinement!CentralClaimWinner(i)
    /\ UNCHANGED AdapterVars

FinalizeEventWinner(i) ==
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "EventArm"
    /\ arm_class[i] = "Winner"
    /\ CentralRefinement!CentralCommitWinner(i)
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Woken"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "EventWinner"]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now>>

ConsumeTimerWinner(i) ==
    /\ central_phase = "Claimed"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ arm_class[i] = "Winner"
    /\ timer_state[i] = "Active"
    /\ timer_state' = [timer_state EXCEPT ![i] = "Consumed"]
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerConsumed"]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

FinalizeTimerWinner(i) ==
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ arm_class[i] = "Winner"
    /\ timer_state[i] = "Consumed"
    /\ finalization_step[i] = "TimerConsumed"
    /\ CentralRefinement!CentralCommitWinner(i)
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Expired"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = FALSE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerWinner"]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now>>

FinalizeEventLoser(i) ==
    /\ central_phase = "Closing"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "EventArm"
    /\ arm_class[i] = "Loser"
    /\ CentralRefinement!CentralReleaseLoser(i)
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Cancelled"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "EventLoser"]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now>>

CancelTimerLoser(i) ==
    /\ central_phase = "Closing"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ arm_class[i] = "Loser"
    /\ timer_state[i] = "Active"
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Cancelled"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "TimerCancelled"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerCancelled"]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, arm_event,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now>>

RetireTimerLoser(i) ==
    /\ central_phase = "Closing"
    /\ adapter_phase[i] = "TimerCancelled"
    /\ finalization_step[i] = "TimerCancelled"
    /\ CentralRefinement!CentralReleaseLoser(i)
    /\ timer_state' = [timer_state EXCEPT ![i] = "Retired"]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerLoser"]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, wait_outcome,
                    wait_linked, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

CloseAdapterAuthority(i) ==
    /\ adapter_phase[i] = "Finalized"
    /\ CentralRefinement!CentralCloseAuthority(i)
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Retired"]
    /\ retired_arm_count' = retired_arm_count + 1
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, wait_outcome, wait_linked,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, now>>

CompleteInline ==
    /\ \A i \in Arms : adapter_phase[i] = "Retired"
    /\ coordination_kind = "None"
    /\ CentralRefinement!CentralPublishInline
    /\ select_result_published' = TRUE
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    registration_count, registered_arm_count,
                    retired_arm_count, now>>

CompleteSuspended ==
    /\ \A i \in Arms : adapter_phase[i] = "Retired"
    /\ coordination_kind \in {"Event", "Timer"}
    /\ CentralRefinement!CentralPublishSuspended
    /\ select_result_published' = TRUE
    /\ coordination_kind' = "None"
    /\ coordination_arm' = NoArm
    /\ scan_phase' = "NoScan"
    /\ held_event' = NoEvent
    /\ scan_remaining' = {}
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, last_broadcast_event,
                    finalization_step, rollback_started, registration_count,
                    registered_arm_count, retired_arm_count, now>>

ResumeCaller ==
    /\ CentralRefinement!CentralResumeCaller
    /\ UNCHANGED AdapterVars

ConsumeResult ==
    /\ CentralRefinement!CentralConsumeResult
    /\ UNCHANGED AdapterVars

DestroyOperation ==
    /\ CentralRefinement!CentralDestroyOperation
    /\ UNCHANGED AdapterVars

BeginRollback ==
    /\ registration_count > 0
    /\ \A i \in Arms : adapter_phase[i] # "Registering"
    /\ CentralRefinement!CentralBeginRollback
    /\ rollback_started' = TRUE
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

RollbackEventArm(i) ==
    /\ central_phase = "Rollback"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "EventArm"
    /\ CentralRefinement!CentralRollbackRelease(i)
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Cancelled"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "Rollback"]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now>>

RollbackCancelTimer(i) ==
    /\ central_phase = "Rollback"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ timer_state[i] = "Active"
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Cancelled"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "TimerCancelled"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerCancelled"]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, arm_event,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now>>

RollbackRetireTimer(i) ==
    /\ central_phase = "Rollback"
    /\ adapter_phase[i] = "TimerCancelled"
    /\ CentralRefinement!CentralRollbackRelease(i)
    /\ timer_state' = [timer_state EXCEPT ![i] = "Retired"]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "Rollback"]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, wait_outcome,
                    wait_linked, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

FinishRollback ==
    /\ \A i \in Arms : adapter_phase[i] \in {"Detached", "Retired"}
    /\ CentralRefinement!CentralFinishRollback
    /\ UNCHANGED AdapterVars

TimerPumpSkip(i) ==
    /\ coordination_kind = "None"
    /\ adapter_phase[i] = "Retired"
    /\ arm_kind[i] = "TimerArm"
    /\ timer_state[i] \in {"Retired", "Consumed"}
    /\ timer_due[i]
    /\ ~timer_skip_observed[i]
    /\ timer_skip_observed' = [timer_skip_observed EXCEPT ![i] = TRUE]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_due, timer_node_deref,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now>>

Tick ==
    /\ coordination_kind = "None"
    /\ now < 3
    /\ now' = now + 1
    /\ timer_due' =
          [i \in Arms |->
             IF /\ arm_kind[i] = "TimerArm"
                /\ now + 1 >= 2
             THEN TRUE ELSE timer_due[i]]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_state, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count>>

EventTimerStutter == UNCHANGED EventTimerVars

EventTimerNext ==
    \/ EventTimerStutter
    \/ \E i \in Arms : BeginRegistration(i)
    \/ \E i \in Arms, e \in Events : RegisterEventArm(i, e)
    \/ \E i \in Arms : RegisterTimerArm(i)
    \/ FinishRegistration
    \/ \E i \in Arms : AdmissionObserveReady(i)
    \/ \E i \in Arms : AdmissionObserveNotReady(i)
    \/ \E i \in Arms : ClaimAdmissionWinner(i)
    \/ SuspendCaller
    \/ \E e \in Events : SetEventBeforeRegistration(e)
    \/ \E e \in Events : StartEventBroadcast(e)
    \/ \E i \in Arms : ScanEventArm(i)
    \/ FinishEventScan
    \/ \E i \in Arms : ClaimEventWinner(i)
    \/ FinishEmptyEventBroadcast
    \/ \E i \in Arms : TimerPumpEntry(i)
    \/ \E i \in Arms : ClaimTimerWinner(i)
    \/ \E i \in Arms : FinalizeEventWinner(i)
    \/ \E i \in Arms : ConsumeTimerWinner(i)
    \/ \E i \in Arms : FinalizeTimerWinner(i)
    \/ \E i \in Arms : FinalizeEventLoser(i)
    \/ \E i \in Arms : CancelTimerLoser(i)
    \/ \E i \in Arms : RetireTimerLoser(i)
    \/ \E i \in Arms : CloseAdapterAuthority(i)
    \/ CompleteInline
    \/ CompleteSuspended
    \/ ResumeCaller
    \/ ConsumeResult
    \/ DestroyOperation
    \/ BeginRollback
    \/ \E i \in Arms : RollbackEventArm(i)
    \/ \E i \in Arms : RollbackCancelTimer(i)
    \/ \E i \in Arms : RollbackRetireTimer(i)
    \/ FinishRollback
    \/ \E i \in Arms : TimerPumpSkip(i)
    \/ Tick

EventTimerSpec == EventTimerInit /\ [][EventTimerNext]_EventTimerVars

RefinesCentralClaim == CentralRefinement!CentralSpec

\* State constraint used only by E13Select.cfg's explicit four-arm bounded
\* scene: arms 0/1 are Event arms on the same real Event 0; arms 2/3 are
\* independent Timer arms.  None is allowed while an arm is Detached or
\* Registering.  No lifecycle/action is disabled after a valid registration.
FourArmMixedConstraint ==
    IF /\ Arms = {0, 1, 2, 3}
       /\ Events = {0, 1}
    THEN /\ arm_kind[0] \in {"None", "EventArm"}
         /\ arm_kind[1] \in {"None", "EventArm"}
         /\ arm_kind[2] \in {"None", "TimerArm"}
         /\ arm_kind[3] \in {"None", "TimerArm"}
         /\ arm_event[0] \in {NoEvent, 0}
         /\ arm_event[1] \in {NoEvent, 0}
         /\ arm_event[2] = NoEvent
         /\ arm_event[3] = NoEvent
    ELSE TRUE

\* Concrete causal reachability obligations R1-R12.
Reach_R1 ==
    /\ completion_mode = "Inline"
    /\ winner \in Arms
    /\ arm_kind[winner] = "EventArm"
    /\ finalization_step[winner] = "EventWinner"
    /\ result_publication_count = 1
    /\ runnable_publication_count = 0

Reach_R2 ==
    /\ completion_mode = "Inline"
    /\ winner \in Arms
    /\ arm_kind[winner] = "TimerArm"
    /\ finalization_step[winner] = "TimerWinner"
    /\ timer_state[winner] = "Consumed"
    /\ result_publication_count = 1
    /\ runnable_publication_count = 0

Reach_R3 ==
    /\ completion_mode = "Suspended"
    /\ winner \in Arms
    /\ arm_kind[winner] = "EventArm"
    /\ finalization_step[winner] = "EventWinner"
    /\ claim_mode = "Suspended"
    /\ runnable_publication_count = 1

Reach_R4 ==
    /\ completion_mode = "Suspended"
    /\ winner \in Arms
    /\ arm_kind[winner] = "TimerArm"
    /\ finalization_step[winner] = "TimerWinner"
    /\ timer_node_deref[winner] = 1
    /\ claim_mode = "Suspended"
    /\ runnable_publication_count = 1

Reach_R5 ==
    /\ winner \in Arms
    /\ arm_kind[winner] = "EventArm"
    /\ finalization_step[winner] = "EventWinner"
    /\ \E t \in Arms :
          /\ arm_kind[t] = "TimerArm"
          /\ finalization_step[t] = "TimerLoser"
          /\ timer_state[t] = "Retired"

Reach_R6 ==
    /\ winner \in Arms
    /\ arm_kind[winner] = "TimerArm"
    /\ finalization_step[winner] = "TimerWinner"
    /\ timer_state[winner] = "Consumed"
    /\ \E e \in Arms :
          /\ arm_kind[e] = "EventArm"
          /\ finalization_step[e] = "EventLoser"

Reach_R7 ==
    /\ winner \in Arms
    /\ Cardinality(claim_candidates) >= 2
    /\ \A i \in claim_candidates : winner <= i
    /\ \A i \in claim_candidates \ {winner} : arm_class[i] = "Loser"

Reach_R8 ==
    \E e \in Events :
        /\ last_broadcast_event = e
        /\ Cardinality({i \in claim_candidates :
              /\ arm_kind[i] = "EventArm"
              /\ arm_event[i] = e}) >= 2
        /\ winner \in claim_candidates

Reach_R9 ==
    /\ rollback_started
    /\ central_phase \in {"Aborted", "Destroyed"}
    /\ winner = NoArm
    /\ retired_arm_count >= 1
    /\ result_publication_count = 0
    /\ runnable_publication_count = 0
    /\ \A i \in Arms : adapter_phase[i] \in {"Detached", "Retired"}

Reach_R10 ==
    /\ winner \in Arms
    /\ arm_kind[winner] = "EventArm"
    /\ completion_mode \in {"Inline", "Suspended"}
    /\ \E t \in Arms :
          /\ arm_kind[t] = "TimerArm"
          /\ timer_state[t] = "Retired"
          /\ finalization_step[t] = "TimerLoser"
          /\ timer_skip_observed[t]
          /\ timer_node_deref[t] = 0

Reach_R11 ==
    /\ central_phase = "Consumed"
    /\ completion_mode = "Inline"
    /\ result_publication_count = 1
    /\ runnable_publication_count = 0

Reach_R12 ==
    /\ central_phase = "Consumed"
    /\ completion_mode = "Suspended"
    /\ result_publication_count = 1
    /\ runnable_publication_count = 1
    /\ caller_state = "Resumed"

NotReach_R1 == ~Reach_R1
NotReach_R2 == ~Reach_R2
NotReach_R3 == ~Reach_R3
NotReach_R4 == ~Reach_R4
NotReach_R5 == ~Reach_R5
NotReach_R6 == ~Reach_R6
NotReach_R7 == ~Reach_R7
NotReach_R8 == ~Reach_R8
NotReach_R9 == ~Reach_R9
NotReach_R10 == ~Reach_R10
NotReach_R11 == ~Reach_R11
NotReach_R12 == ~Reach_R12

=============================================================================
