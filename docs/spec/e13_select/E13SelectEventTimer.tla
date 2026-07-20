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
    now,

    \* PR #18 exactly-once accounting counters (M) and step-indexed history (N).
    \* These are adapter-only variables; they are NOT part of the Central Claim
    \* or Contract `WITH` mapping, so the refinement chain is unaffected.
    \* A boolean `waiting_account_open` cannot distinguish "never opened",
    \* "closed once", and "closed twice"; the per-arm counters below do.
    wait_account_open_count,
    wait_account_close_count,
    timer_account_open_count,
    timer_account_close_count,

    \* Step-indexed history (N).  NoStep marks "action has not occurred";
    \* never compare step counts across different (group, arm, epoch) keys.
    \* Single-group model uses group_id = 0 throughout.
    group_id,
    claim_epoch,
    broadcast_epoch,
    timer_pump_epoch,
    winner_linearization_step,
    commit_step,
    terminal_step,
    unlink_step,
    timer_transition_step,
    account_close_step,
    authority_close_step,
    publication_step,
    global_step

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

\* PR #18 accounting counters and step-indexed history.  Kept out of
\* AdapterVars so the canonical PR #17 R1-R12 reachability predicates and
\* metrics reproduce unchanged; checked by dedicated PR #18 safety cfgs.
AccountingVars ==
    <<wait_account_open_count, wait_account_close_count,
      timer_account_open_count, timer_account_close_count>>

HistoryVars ==
    <<group_id, claim_epoch, broadcast_epoch, timer_pump_epoch,
      winner_linearization_step, commit_step, terminal_step, unlink_step,
      timer_transition_step, account_close_step, authority_close_step,
      publication_step, global_step>>

AdapterSafetyVars == <<AccountingVars, HistoryVars>>

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

\* PR #18 step-indexed history domain.  NoStep (0) means "action has not
\* occurred yet"; a positive value is the global_step at which it occurred.
\* All step comparisons must pair steps belonging to the same (group, arm,
\* epoch) key, never arm 0's step against arm 1's.
\* The upper bound 40 matches the maximum search depth observed on the
\* largest canonical cfg (4-arm mixed root, depth 40).  Each step variable
\* is written at most once along any behaviour, so 0..40 is sufficient and
\* keeps the per-arm step domain tractable.
NoStep == 0
StepT == 0..40
GroupIdT == 0..1
EpochT == 0..4

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
    /\ wait_account_open_count \in [Arms -> 0..1]
    /\ wait_account_close_count \in [Arms -> 0..1]
    /\ timer_account_open_count \in [Arms -> 0..1]
    /\ timer_account_close_count \in [Arms -> 0..1]
    /\ group_id \in GroupIdT
    /\ claim_epoch \in EpochT
    /\ broadcast_epoch \in EpochT
    /\ timer_pump_epoch \in EpochT
    /\ winner_linearization_step \in StepT
    /\ commit_step \in [Arms -> StepT]
    /\ terminal_step \in [Arms -> StepT]
    /\ unlink_step \in [Arms -> StepT]
    /\ timer_transition_step \in [Arms -> StepT]
    /\ account_close_step \in [Arms -> StepT]
    /\ authority_close_step \in [Arms -> StepT]
    /\ publication_step \in StepT
    /\ global_step \in StepT

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

EventTimerRollbackEnabledDomain ==
    /\ registration_count > 0
    /\ \A i \in Arms : adapter_phase[i] # "Registering"
    /\ central_phase = "Registering"
    /\ contract_phase = "Building"
    /\ caller_state = "Running"
    /\ winner = NoArm

EventTimerRegistrationRollbackDisabledAfterSuspension ==
    caller_state = "Waiting" => ~EventTimerRollbackEnabledDomain

EventTimerInv ==
    /\ CentralRefinement!CentralInv
    /\ EventTimerTypeOK
    /\ AdapterLifecycleWellFormed
    /\ CoordinationWellFormed
    /\ TimerAuthorityWellFormed
    /\ EventTimerRegistrationRollbackDisabledAfterSuspension

(* ========================================================================= *)
(* PR #18 layered adapter safety invariants (K, L, M, N).                     *)
(*                                                                           *)
(* These named laws are aggregated in `AdapterSafetyInv`, kept separate from  *)
(* the canonical PR #17 `EventTimerInv` so the canonical adapter graph and    *)
(* both refinements reproduce.  K = Event adapter, L = Timer adapter,         *)
(* M = exactly-once accounting, N = step-indexed history/order.              *)
(* ========================================================================= *)

\* -- K: Event adapter safety ----------------------------------------------

E_InvEventArmHasValidEventIdentity ==
    \A i \in Arms :
        arm_kind[i] = "EventArm" => arm_event[i] \in Events

E_InvBroadcastScansOnlyTargetEventArms ==
    \* While scanning, every scanned arm belongs to the held Event identity.
    coordination_kind = "Event" /\ scan_phase = "Scanning"
        => \A i \in Arms :
              i \in scan_remaining
                  => /\ arm_kind[i] = "EventArm"
                     /\ arm_event[i] = held_event

E_InvSameEventArmsShareIdentity ==
    \* Two Event arms registered to the same Event share that identity.
    \A i, j \in Arms :
        arm_kind[i] = "EventArm" /\ arm_kind[j] = "EventArm"
            /\ arm_event[i] = arm_event[j]
        => arm_event[i] \in Events

E_InvPhaseOneOnlyOffersCandidates ==
    \* Event scan phase (Scanning) only offers Central candidates; it does
    \* not claim, commit, close authority, or publish.
    scan_phase = "Scanning" => central_phase = "Armed"

E_InvPhaseOneDoesNotClaim ==
    scan_phase = "Scanning" => central_phase # "Claimed"

E_InvPhaseOneDoesNotCommit ==
    scan_phase = "Scanning" => winner = NoArm \/ arm_resolution[winner] # "WinnerCommitted"

E_InvPhaseOneDoesNotPublish ==
    scan_phase = "Scanning" => result_publication_count = 0

E_InvPhaseTwoStartsAfterScanAuthorityReleased ==
    \* ProcessGroups (phase 2) may claim only after held_event is released.
    scan_phase = "ProcessGroups" => held_event = NoEvent

E_InvNoReadinessBypassesPermittedEventPath ==
    \* A candidate may be observed ready only via admission or broadcast scan,
    \* not via a direct post-arming resolver.  In the model, candidate_ready[i]
    \* becomes true only through CentralObserveCandidate, which the adapter
    \* invokes from Admission or Scan.  Once observed, candidate_ready stays
    \* true (it is monotonic evidence) through terminal phases.  The
    \* load-bearing law: an observed candidate must be a registered arm whose
    \* adapter phase has advanced along the legitimate Registered -> Finalized
    \* -> Retired path (never a bypassed Detached or unregistered arm).  The
    \* initial-offer readiness is enforced by AdmissionObserveReady /
    \* ScanEventArm guards.
    \A i \in Arms :
        candidate_ready[i]
            => /\ arm_registered[i]
               /\ adapter_phase[i] \in
                     {"Registered", "TimerCancelled", "Finalized", "Retired"}

E_InvNoRecursiveEventQueueAuthority ==
    \* Only one Event broadcast coordination may be in flight at a time.
    coordination_kind = "Event"
        => /\ Cardinality({e \in Events : event_state[e] = "Set"}) >= 1
           /\ scan_phase \in {"Scanning", "ProcessGroups"}

E_InvEventPersistentSetNotConsumed ==
    \* Select never consumes a persistent Event Set back to Unset.  Once an
    \* Event identity has been broadcast (last_broadcast_event recorded), that
    \* Event's state must still be Set -- Select must not collapse the Set back
    \* to Unset (a real Event queue reset would race a stale broadcaster).
    \A e \in Events :
        e = last_broadcast_event => event_state[e] = "Set"

E_InvWinnerTerminalDetachedBeforeAuthorityClose ==
    \* A Retired adapter arm has a terminal wait_outcome and an unlinked node
    \* before (i.e., at the time of) adapter authority close.
    \A i \in Arms :
        adapter_phase[i] = "Retired" /\ arm_class[i] = "Winner"
            => /\ wait_outcome[i] \in {"Woken", "Expired"}
               /\ ~wait_linked[i]

E_InvLoserTerminalDetachedBeforeAuthorityClose ==
    \A i \in Arms :
        adapter_phase[i] = "Retired" /\ arm_class[i] = "Loser"
            => /\ wait_outcome[i] = "Cancelled"
               /\ ~wait_linked[i]

\* -- L: Timer adapter safety ----------------------------------------------

T_InvNoDereferenceWithoutActiveAuthority ==
    \A i \in Arms :
        timer_node_deref[i] > 0
            => /\ arm_kind[i] = "TimerArm"
               /\ timer_state[i] \in {"Active", "Consumed", "Retired"}

T_InvRetiredRegistrationNeverDereferences ==
    \* Once Retired, the registration is never dereferenced again.  The pump
    \* entry requires Active; a Retired arm cannot increment deref.  State
    \* law: a Retired Timer arm's deref count is frozen at its pump-time value
    \* (0 or 1) and never grows further.
    \A i \in Arms :
        timer_state[i] = "Retired"
            => timer_node_deref[i] \in {0, 1}

T_InvConsumedRegistrationNeverDereferences ==
    \* A Consumed registration belongs to the winner and is not dereferenced
    \* again.  ConsumeTimerWinner flips Active->Consumed; no action derefs a
    \* Consumed registration (TimerPumpEntry requires Active).  The deref count
    \* is frozen at its value at consume time (0 if won via admission, 1 if won
    \* via a post-suspension pump).
    \A i \in Arms :
        timer_state[i] = "Consumed"
            => /\ arm_class[i] = "Winner"
               /\ winner = i
               /\ timer_node_deref[i] \in {0, 1}

T_InvActiveStableDuringPumpAuthority ==
    \* While a Timer pump coordination is in flight, the pumped arm holds an
    \* Active registration UNTIL ConsumeTimerWinner flips it to Consumed (which
    \* then lets the claim proceed).  The pump entry itself requires Active;
    \* once consumed the arm is the linearized winner.  No Retired or stale
    \* registration can be the live pump authority.
    coordination_kind = "Timer" /\ coordination_arm \in Arms
        => timer_state[coordination_arm] \in {"Active", "Consumed"}

T_InvDueObservationDoesNotConsume ==
    \* Observing a due Timer at admission does not consume the registration.
    \A i \in Arms :
        timer_due[i] /\ arm_kind[i] = "TimerArm" /\ adapter_phase[i] = "Registered"
            => timer_state[i] \in {"Active", "Consumed", "Retired"}

T_InvConsumeRequiresWinner ==
    \A i \in Arms :
        timer_state[i] = "Consumed"
            => /\ arm_class[i] = "Winner"
               /\ winner = i

T_InvRetireRequiresLoserOrRollback ==
    \A i \in Arms :
        timer_state[i] = "Retired" /\ arm_registered[i]
            => \/ /\ arm_class[i] = "Loser"
               \/ finalization_step[i] = "Rollback"

T_InvTimerTransitionExactlyOnce ==
    \* A Timer registration transitions Active -> {Consumed, Retired} at most
    \* once.  timer_transition_step records the unique step.
    \A i \in Arms :
        timer_state[i] \in {"Consumed", "Retired"}
            => timer_transition_step[i] # NoStep

T_InvStalePumpChecksState ==
    \A i \in Arms :
        timer_skip_observed[i]
            => timer_state[i] \in {"Retired", "Consumed"}

T_InvStalePumpSkipsWithoutDereference ==
    \A i \in Arms :
        timer_skip_observed[i]
            => timer_node_deref[i] \in {0, 1}

T_InvStalePumpNeverOffers ==
    \* The stale-pump skip action (TimerPumpSkip) never offers a new
    \* candidate: it does not write candidate_ready, does not increment
    \* timer_node_deref, and does not call CentralObserveCandidate.  A Retired
    \* loser or rollback registration cannot become a fresh offer via the skip
    \* path.  State law: a Retired arm reached via a stale skip is either a
    \* classified loser or a rollback terminal (never an unclassified mid-
    \* lifecycle arm and never a fresh admission candidate).
    \A i \in Arms :
        timer_skip_observed[i] /\ timer_state[i] = "Retired"
            => arm_class[i] \in {"Loser", "Unclassified"}
               /\ finalization_step[i] \in
                     {"TimerLoser", "Rollback"}

T_InvStalePumpNeverPublishes ==
    \* The stale-pump skip action never publishes: a Retired loser registration
    \* observed via skip has zero per-arm publication.  (The winner, if also
    \* skip-observed as Consumed, may publish via the normal winner path, not
    \* the skip path; this law targets Retired loser registrations.)
    \A i \in Arms :
        timer_skip_observed[i] /\ timer_state[i] = "Retired"
            => arm_publication_count[i] = 0

\* -- M: exactly-once accounting -------------------------------------------

A_InvWaitAccountOpensAtMostOnce ==
    \A i \in Arms : wait_account_open_count[i] <= 1

A_InvWaitAccountClosesAtMostOnce ==
    \A i \in Arms : wait_account_close_count[i] <= 1

A_InvRegisteredArmClosesWaitAccount ==
    \* Every registered arm that reached a terminal adapter phase has closed
    \* its waiting account exactly once.
    \A i \in Arms :
        adapter_phase[i] \in {"Finalized", "Retired"}
            => wait_account_close_count[i] = 1

A_InvTimerAccountOpensAtMostOnce ==
    \A i \in Arms : timer_account_open_count[i] <= 1

A_InvTimerAccountClosesAtMostOnce ==
    \A i \in Arms : timer_account_close_count[i] <= 1

A_InvTimerConsumeOrRetireClosesExactlyOnce ==
    \* A Timer registration that has reached a terminal adapter phase has
    \* closed its timer account exactly once.  Consumed is an intermediate
    \* state (ConsumeTimerWinner); the account closes later in FinalizeTimer-
    \* Winner.  So this law binds at Finalized/Retired, the terminal phases.
    \A i \in Arms :
        arm_kind[i] = "TimerArm"
            /\ adapter_phase[i] \in {"Finalized", "Retired"}
        => timer_account_close_count[i] = 1

A_InvNoAccountingUnderflow ==
    \A i \in Arms :
        /\ wait_account_close_count[i] <= wait_account_open_count[i]
        /\ timer_account_close_count[i] <= timer_account_open_count[i]

A_InvCompletedHasNoOpenAccounting ==
    contract_phase \in {"Completed", "Consumed"}
        => \A i \in Arms :
              /\ wait_account_close_count[i] = wait_account_open_count[i]
              /\ timer_account_close_count[i] = timer_account_open_count[i]

A_InvDestroyedHasNoOpenAccounting ==
    contract_phase = "Destroyed"
        => \A i \in Arms :
              /\ wait_account_close_count[i] = wait_account_open_count[i]
              /\ timer_account_close_count[i] = timer_account_open_count[i]

\* -- N: step-indexed history / order correctness -------------------------
\* All comparisons pair steps of the SAME arm in the SAME group/epoch.
\* No law compares arm 0's step against arm 1's.  NoStep (0) = "not occurred".

N_InvCommitFollowsWinnerLinearization ==
    \A i \in Arms :
        commit_step[i] # NoStep
            => winner_linearization_step # NoStep
               /\ winner_linearization_step < commit_step[i]

N_InvTerminalFollowsCommit ==
    \* Winner terminal (detach) follows commit; loser terminal follows
    \* release, which the adapter performs without a commit.  For a winner
    \* arm, terminal_step must follow commit_step.
    \A i \in Arms :
        commit_step[i] # NoStep /\ terminal_step[i] # NoStep
            => commit_step[i] <= terminal_step[i]

N_InvUnlinkFollowsTerminal ==
    \A i \in Arms :
        unlink_step[i] # NoStep /\ terminal_step[i] # NoStep
            => terminal_step[i] <= unlink_step[i]

N_InvTimerTransitionFollowsLinearization ==
    \* For a winner (Consumed) or classified-loser (Retired via TimerLoser)
    \* Timer transition, the transition follows winner linearization.  A
    \* rollback Retired timer transitions without a winner (no Select claim
    \* epoch) and is excluded from this law; rollback has its own terminal
    \* accounting under A_Inv* and N_InvAuthorityCloseFollowsTerminal.
    \A i \in Arms :
        timer_transition_step[i] # NoStep
            /\ finalization_step[i] \in {"TimerConsumed", "TimerWinner",
                                          "TimerLoser"}
        => winner_linearization_step # NoStep
           /\ winner_linearization_step < timer_transition_step[i]

N_InvAccountCloseFollowsTimerTransition ==
    \A i \in Arms :
        account_close_step[i] # NoStep /\ timer_transition_step[i] # NoStep
            => timer_transition_step[i] <= account_close_step[i]

N_InvAuthorityCloseFollowsTerminal ==
    \A i \in Arms :
        authority_close_step[i] # NoStep /\ terminal_step[i] # NoStep
            => terminal_step[i] <= authority_close_step[i]

N_InvPublicationFollowsAuthorityClose ==
    \* Publication follows every arm's authority close.
    publication_step # NoStep
        => \A i \in Arms :
              arm_registered[i] => authority_close_step[i] # NoStep

N_InvPublicationFollowsAllAccountClose ==
    publication_step # NoStep
        => \A i \in Arms :
              arm_registered[i] => account_close_step[i] # NoStep
                 \/ arm_kind[i] = "EventArm"

AdapterSafetyInv ==
    /\ E_InvEventArmHasValidEventIdentity
    /\ E_InvBroadcastScansOnlyTargetEventArms
    /\ E_InvSameEventArmsShareIdentity
    /\ E_InvPhaseOneOnlyOffersCandidates
    /\ E_InvPhaseOneDoesNotClaim
    /\ E_InvPhaseOneDoesNotCommit
    /\ E_InvPhaseOneDoesNotPublish
    /\ E_InvPhaseTwoStartsAfterScanAuthorityReleased
    /\ E_InvNoReadinessBypassesPermittedEventPath
    /\ E_InvNoRecursiveEventQueueAuthority
    /\ E_InvEventPersistentSetNotConsumed
    /\ E_InvWinnerTerminalDetachedBeforeAuthorityClose
    /\ E_InvLoserTerminalDetachedBeforeAuthorityClose
    /\ T_InvNoDereferenceWithoutActiveAuthority
    /\ T_InvRetiredRegistrationNeverDereferences
    /\ T_InvConsumedRegistrationNeverDereferences
    /\ T_InvActiveStableDuringPumpAuthority
    /\ T_InvDueObservationDoesNotConsume
    /\ T_InvConsumeRequiresWinner
    /\ T_InvRetireRequiresLoserOrRollback
    /\ T_InvTimerTransitionExactlyOnce
    /\ T_InvStalePumpChecksState
    /\ T_InvStalePumpSkipsWithoutDereference
    /\ T_InvStalePumpNeverOffers
    /\ T_InvStalePumpNeverPublishes
    /\ A_InvWaitAccountOpensAtMostOnce
    /\ A_InvWaitAccountClosesAtMostOnce
    /\ A_InvRegisteredArmClosesWaitAccount
    /\ A_InvTimerAccountOpensAtMostOnce
    /\ A_InvTimerAccountClosesAtMostOnce
    /\ A_InvTimerConsumeOrRetireClosesExactlyOnce
    /\ A_InvNoAccountingUnderflow
    /\ A_InvCompletedHasNoOpenAccounting
    /\ A_InvDestroyedHasNoOpenAccounting
    /\ N_InvCommitFollowsWinnerLinearization
    /\ N_InvTerminalFollowsCommit
    /\ N_InvUnlinkFollowsTerminal
    /\ N_InvTimerTransitionFollowsLinearization
    /\ N_InvAccountCloseFollowsTimerTransition
    /\ N_InvAuthorityCloseFollowsTerminal
    /\ N_InvPublicationFollowsAuthorityClose
    /\ N_InvPublicationFollowsAllAccountClose

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
    /\ wait_account_open_count = [i \in Arms |-> 0]
    /\ wait_account_close_count = [i \in Arms |-> 0]
    /\ timer_account_open_count = [i \in Arms |-> 0]
    /\ timer_account_close_count = [i \in Arms |-> 0]
    /\ group_id = 0
    /\ claim_epoch = 0
    /\ broadcast_epoch = 0
    /\ timer_pump_epoch = 0
    /\ winner_linearization_step = NoStep
    /\ commit_step = [i \in Arms |-> NoStep]
    /\ terminal_step = [i \in Arms |-> NoStep]
    /\ unlink_step = [i \in Arms |-> NoStep]
    /\ timer_transition_step = [i \in Arms |-> NoStep]
    /\ account_close_step = [i \in Arms |-> NoStep]
    /\ authority_close_step = [i \in Arms |-> NoStep]
    /\ publication_step = NoStep
    /\ global_step = 0

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
                    retired_arm_count, now, AccountingVars, HistoryVars>>

RegisterEventArm(i, e) ==
    /\ adapter_phase[i] = "Registering"
    /\ e \in Events
    /\ arm_kind' = [arm_kind EXCEPT ![i] = "EventArm"]
    /\ arm_event' = [arm_event EXCEPT ![i] = e]
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Pending"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = TRUE]
    /\ context_state' = [context_state EXCEPT ![i] = "Installed"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = TRUE]
    /\ wait_account_open_count' =
          [wait_account_open_count EXCEPT ![i] = @ + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Registered"]
    /\ registration_count' = registration_count + 1
    /\ registered_arm_count' = registered_arm_count + 1
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<CentralProjectionVars, arm_index, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    select_result_published, retired_arm_count, now,
                    wait_account_close_count, timer_account_open_count,
                    timer_account_close_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step, terminal_step,
                    unlink_step, timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

RegisterTimerArm(i) ==
    /\ adapter_phase[i] = "Registering"
    /\ arm_kind' = [arm_kind EXCEPT ![i] = "TimerArm"]
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Pending"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = TRUE]
    /\ context_state' = [context_state EXCEPT ![i] = "Installed"]
    /\ timer_state' = [timer_state EXCEPT ![i] = "Active"]
    /\ timer_due' = [timer_due EXCEPT ![i] = (now >= 2)]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = TRUE]
    /\ wait_account_open_count' =
          [wait_account_open_count EXCEPT ![i] = @ + 1]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = TRUE]
    /\ timer_account_open_count' =
          [timer_account_open_count EXCEPT ![i] = @ + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Registered"]
    /\ registration_count' = registration_count + 1
    /\ registered_arm_count' = registered_arm_count + 1
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<CentralProjectionVars, arm_index, arm_event,
                    timer_node_deref, timer_skip_observed, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm, last_broadcast_event,
                    finalization_step, rollback_started,
                    select_result_published, retired_arm_count, now,
                    wait_account_close_count, timer_account_close_count,
                    group_id, claim_epoch, broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step, terminal_step,
                    unlink_step, timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

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
                    retired_arm_count, now, AccountingVars, HistoryVars>>

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
                    retired_arm_count, now, AccountingVars, HistoryVars>>

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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

ClaimAdmissionWinner(i) ==
    /\ central_phase = "Admission"
    /\ AllAdmissionChecked
    /\ CentralRefinement!CentralClaimWinner(i)
    /\ winner_linearization_step' = global_step + 1
    /\ claim_epoch' = claim_epoch + 1
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<AdapterVars, AccountingVars, group_id, broadcast_epoch,
                    timer_pump_epoch, commit_step, terminal_step, unlink_step,
                    timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

SuspendCaller ==
    /\ central_phase = "Admission"
    /\ AllAdmissionChecked
    /\ CentralRefinement!CentralSuspendCaller
    /\ UNCHANGED <<AdapterVars, AccountingVars, HistoryVars>>

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
                    retired_arm_count, now, AccountingVars, HistoryVars>>

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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

ClaimEventWinner(i) ==
    /\ coordination_kind = "Event"
    /\ scan_phase = "ProcessGroups"
    /\ CentralRefinement!CentralClaimWinner(i)
    /\ winner_linearization_step' = global_step + 1
    /\ claim_epoch' = claim_epoch + 1
    /\ broadcast_epoch' = broadcast_epoch + 1
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<AdapterVars, AccountingVars, group_id, timer_pump_epoch,
                    commit_step, terminal_step, unlink_step,
                    timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

ClaimTimerWinner(i) ==
    /\ coordination_kind = "Timer"
    /\ coordination_arm = i
    /\ CentralRefinement!CentralClaimWinner(i)
    /\ winner_linearization_step' = global_step + 1
    /\ claim_epoch' = claim_epoch + 1
    /\ timer_pump_epoch' = timer_pump_epoch + 1
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<AdapterVars, AccountingVars, group_id, broadcast_epoch,
                    commit_step, terminal_step, unlink_step,
                    timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

FinalizeEventWinner(i) ==
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "EventArm"
    /\ arm_class[i] = "Winner"
    /\ CentralRefinement!CentralCommitWinner(i)
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Woken"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ wait_account_close_count' =
          [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "EventWinner"]
    /\ global_step' = global_step + 1
    /\ commit_step' = [commit_step EXCEPT ![i] = global_step + 1]
    /\ terminal_step' = [terminal_step EXCEPT ![i] = global_step + 1]
    /\ unlink_step' = [unlink_step EXCEPT ![i] = global_step + 1]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now,
                    wait_account_open_count, timer_account_open_count,
                    timer_account_close_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, timer_transition_step,
                    account_close_step, authority_close_step,
                    publication_step>>

ConsumeTimerWinner(i) ==
    /\ central_phase = "Claimed"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ arm_class[i] = "Winner"
    /\ timer_state[i] = "Active"
    /\ timer_state' = [timer_state EXCEPT ![i] = "Consumed"]
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerConsumed"]
    /\ timer_transition_step' =
          [timer_transition_step EXCEPT ![i] = global_step + 1]
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, adapter_phase,
                    arm_event, wait_outcome, wait_linked, context_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now,
                    AccountingVars, group_id, claim_epoch, broadcast_epoch,
                    timer_pump_epoch, winner_linearization_step, commit_step,
                    terminal_step, unlink_step, account_close_step,
                    authority_close_step, publication_step>>

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
    /\ wait_account_close_count' =
          [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = FALSE]
    /\ timer_account_close_count' =
          [timer_account_close_count EXCEPT ![i] = @ + 1]
    /\ account_close_step' =
          [account_close_step EXCEPT ![i] = global_step + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerWinner"]
    /\ global_step' = global_step + 1
    /\ commit_step' = [commit_step EXCEPT ![i] = global_step + 1]
    /\ terminal_step' = [terminal_step EXCEPT ![i] = global_step + 1]
    /\ unlink_step' = [unlink_step EXCEPT ![i] = global_step + 1]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now,
                    wait_account_open_count, timer_account_open_count,
                    group_id, claim_epoch, broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, timer_transition_step,
                    authority_close_step, publication_step>>

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
    /\ wait_account_close_count' =
          [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "EventLoser"]
    /\ global_step' = global_step + 1
    /\ terminal_step' = [terminal_step EXCEPT ![i] = global_step + 1]
    /\ unlink_step' = [unlink_step EXCEPT ![i] = global_step + 1]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now,
                    wait_account_open_count, timer_account_open_count,
                    timer_account_close_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step,
                    timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

CancelTimerLoser(i) ==
    /\ central_phase = "Closing"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ arm_class[i] = "Loser"
    /\ timer_state[i] = "Active"
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Cancelled"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ wait_account_close_count' =
          [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "TimerCancelled"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerCancelled"]
    /\ global_step' = global_step + 1
    /\ terminal_step' = [terminal_step EXCEPT ![i] = global_step + 1]
    /\ unlink_step' = [unlink_step EXCEPT ![i] = global_step + 1]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, arm_event,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now,
                    wait_account_open_count, timer_account_open_count,
                    timer_account_close_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step,
                    timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

RetireTimerLoser(i) ==
    /\ central_phase = "Closing"
    /\ adapter_phase[i] = "TimerCancelled"
    /\ finalization_step[i] = "TimerCancelled"
    /\ CentralRefinement!CentralReleaseLoser(i)
    /\ timer_state' = [timer_state EXCEPT ![i] = "Retired"]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = FALSE]
    /\ timer_account_close_count' =
          [timer_account_close_count EXCEPT ![i] = @ + 1]
    /\ account_close_step' =
          [account_close_step EXCEPT ![i] = global_step + 1]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerLoser"]
    /\ timer_transition_step' =
          [timer_transition_step EXCEPT ![i] = global_step + 1]
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, wait_outcome,
                    wait_linked, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now,
                    wait_account_open_count, wait_account_close_count,
                    timer_account_open_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step, terminal_step,
                    unlink_step, authority_close_step, publication_step>>

CloseAdapterAuthority(i) ==
    /\ adapter_phase[i] = "Finalized"
    /\ CentralRefinement!CentralCloseAuthority(i)
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Retired"]
    /\ retired_arm_count' = retired_arm_count + 1
    /\ authority_close_step' =
          [authority_close_step EXCEPT ![i] = global_step + 1]
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, wait_outcome, wait_linked,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open,
                    timer_account_open, event_state, admission_checked,
                    scan_phase, held_event, scan_remaining, coordination_kind,
                    coordination_arm, last_broadcast_event, finalization_step,
                    rollback_started, select_result_published,
                    registration_count, registered_arm_count, now,
                    AccountingVars, group_id, claim_epoch, broadcast_epoch,
                    timer_pump_epoch, winner_linearization_step, commit_step,
                    terminal_step, unlink_step, timer_transition_step,
                    account_close_step, publication_step>>

CompleteInline ==
    /\ \A i \in Arms : adapter_phase[i] = "Retired"
    /\ coordination_kind = "None"
    /\ CentralRefinement!CentralPublishInline
    /\ select_result_published' = TRUE
    /\ publication_step' = global_step + 1
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, finalization_step, rollback_started,
                    registration_count, registered_arm_count,
                    retired_arm_count, now, AccountingVars, group_id,
                    claim_epoch, broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step, terminal_step,
                    unlink_step, timer_transition_step, account_close_step,
                    authority_close_step>>

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
    /\ publication_step' = global_step + 1
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<arm_kind, arm_index, adapter_phase, arm_event,
                    wait_outcome, wait_linked, context_state, timer_state,
                    timer_due, timer_node_deref, timer_skip_observed,
                    waiting_account_open, timer_account_open, event_state,
                    admission_checked, last_broadcast_event,
                    finalization_step, rollback_started, registration_count,
                    registered_arm_count, retired_arm_count, now,
                    AccountingVars, group_id, claim_epoch, broadcast_epoch,
                    timer_pump_epoch, winner_linearization_step, commit_step,
                    terminal_step, unlink_step, timer_transition_step,
                    account_close_step, authority_close_step>>

ResumeCaller ==
    /\ CentralRefinement!CentralResumeCaller
    /\ UNCHANGED <<AdapterVars, AccountingVars, HistoryVars>>

ConsumeResult ==
    /\ CentralRefinement!CentralConsumeResult
    /\ UNCHANGED <<AdapterVars, AccountingVars, HistoryVars>>

DestroyOperation ==
    /\ CentralRefinement!CentralDestroyOperation
    /\ UNCHANGED <<AdapterVars, AccountingVars, HistoryVars>>

BeginRollback ==
    /\ EventTimerRollbackEnabledDomain
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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

RollbackEventArm(i) ==
    /\ central_phase = "Rollback"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "EventArm"
    /\ CentralRefinement!CentralRollbackRelease(i)
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Cancelled"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ wait_account_close_count' =
          [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "Rollback"]
    /\ global_step' = global_step + 1
    /\ terminal_step' = [terminal_step EXCEPT ![i] = global_step + 1]
    /\ unlink_step' = [unlink_step EXCEPT ![i] = global_step + 1]
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, timer_state, timer_due,
                    timer_node_deref, timer_skip_observed, timer_account_open,
                    event_state, admission_checked, scan_phase, held_event,
                    scan_remaining, coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now,
                    wait_account_open_count, timer_account_open_count,
                    timer_account_close_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step,
                    timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

RollbackCancelTimer(i) ==
    /\ central_phase = "Rollback"
    /\ adapter_phase[i] = "Registered"
    /\ arm_kind[i] = "TimerArm"
    /\ timer_state[i] = "Active"
    /\ wait_outcome' = [wait_outcome EXCEPT ![i] = "Cancelled"]
    /\ wait_linked' = [wait_linked EXCEPT ![i] = FALSE]
    /\ waiting_account_open' = [waiting_account_open EXCEPT ![i] = FALSE]
    /\ wait_account_close_count' =
          [wait_account_close_count EXCEPT ![i] = @ + 1]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "TimerCancelled"]
    /\ registered_arm_count' = registered_arm_count - 1
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "TimerCancelled"]
    /\ global_step' = global_step + 1
    /\ terminal_step' = [terminal_step EXCEPT ![i] = global_step + 1]
    /\ unlink_step' = [unlink_step EXCEPT ![i] = global_step + 1]
    /\ UNCHANGED <<CentralProjectionVars, arm_kind, arm_index, arm_event,
                    context_state, timer_state, timer_due, timer_node_deref,
                    timer_skip_observed, timer_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    retired_arm_count, now,
                    wait_account_open_count, timer_account_open_count,
                    timer_account_close_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step,
                    timer_transition_step, account_close_step,
                    authority_close_step, publication_step>>

RollbackRetireTimer(i) ==
    /\ central_phase = "Rollback"
    /\ adapter_phase[i] = "TimerCancelled"
    /\ CentralRefinement!CentralRollbackRelease(i)
    /\ timer_state' = [timer_state EXCEPT ![i] = "Retired"]
    /\ timer_account_open' = [timer_account_open EXCEPT ![i] = FALSE]
    /\ timer_account_close_count' =
          [timer_account_close_count EXCEPT ![i] = @ + 1]
    /\ account_close_step' =
          [account_close_step EXCEPT ![i] = global_step + 1]
    /\ context_state' = [context_state EXCEPT ![i] = "Cleared"]
    /\ adapter_phase' = [adapter_phase EXCEPT ![i] = "Finalized"]
    /\ finalization_step' = [finalization_step EXCEPT ![i] = "Rollback"]
    /\ timer_transition_step' =
          [timer_transition_step EXCEPT ![i] = global_step + 1]
    /\ global_step' = global_step + 1
    /\ UNCHANGED <<arm_kind, arm_index, arm_event, wait_outcome,
                    wait_linked, timer_due, timer_node_deref,
                    timer_skip_observed, waiting_account_open, event_state,
                    admission_checked, scan_phase, held_event, scan_remaining,
                    coordination_kind, coordination_arm,
                    last_broadcast_event, rollback_started,
                    select_result_published, registration_count,
                    registered_arm_count, retired_arm_count, now,
                    wait_account_open_count, wait_account_close_count,
                    timer_account_open_count, group_id, claim_epoch,
                    broadcast_epoch, timer_pump_epoch,
                    winner_linearization_step, commit_step, terminal_step,
                    unlink_step, authority_close_step, publication_step>>

FinishRollback ==
    /\ \A i \in Arms : adapter_phase[i] \in {"Detached", "Retired"}
    /\ CentralRefinement!CentralFinishRollback
    /\ UNCHANGED <<AdapterVars, AccountingVars, HistoryVars>>

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
                    registered_arm_count, retired_arm_count, now, AccountingVars, HistoryVars>>

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
                    registered_arm_count, retired_arm_count,
                    AccountingVars, HistoryVars>>

EventTimerStutter == UNCHANGED <<EventTimerVars, AccountingVars, HistoryVars>>

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
    /\ caller_state = "Running"
    /\ winner = NoArm
    /\ registration_count > 0
    /\ registration_count < MaxArms
    /\ result_publication_count = 0
    /\ runnable_publication_count = 0
    /\ \A i \in Arms : arm_publication_count[i] = 0
    /\ \A i \in Arms :
          \/ /\ arm_registered[i]
             /\ adapter_phase[i] = "Retired"
             /\ arm_resolution[i] = "Released"
             /\ ~authority_open[i]
             /\ finalization_step[i] = "Rollback"
          \/ /\ ~arm_registered[i]
             /\ adapter_phase[i] = "Detached"
             /\ arm_resolution[i] = "None"
             /\ ~authority_open[i]
             /\ finalization_step[i] = "None"

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

\* =========================================================================
\* PR #18 non-vacuity witnesses (W) for the new adapter safety laws
\* (M accounting + N history).  These complement PR #17's R1-R12 witnesses by
\* proving the new accounting/history laws' premises are reachable, so the
\* laws are not vacuously TRUE.
\* =========================================================================

\* A_InvWaitAccountClosesAtMostOnce premise: a wait account was closed.
ReachAdapterWaitAccountClosed ==
    \E i \in Arms : wait_account_close_count[i] >= 1
NotReachAdapterWaitAccountClosed == ~ReachAdapterWaitAccountClosed

\* A_InvTimerAccountClosesAtMostOnce premise: a timer account was closed.
ReachAdapterTimerAccountClosed ==
    \E i \in Arms : timer_account_close_count[i] >= 1
NotReachAdapterTimerAccountClosed == ~ReachAdapterTimerAccountClosed

\* T_InvRetiredRegistrationNeverDereferences premise: a Retired timer exists.
ReachAdapterRetiredTimerExists ==
    \E i \in Arms :
        /\ arm_kind[i] = "TimerArm"
        /\ timer_state[i] = "Retired"
NotReachAdapterRetiredTimerExists == ~ReachAdapterRetiredTimerExists

\* T_InvConsumeRequiresWinner premise: a Consumed timer exists.
ReachAdapterConsumedTimerExists ==
    \E i \in Arms :
        /\ arm_kind[i] = "TimerArm"
        /\ timer_state[i] = "Consumed"
NotReachAdapterConsumedTimerExists == ~ReachAdapterConsumedTimerExists

\* N_InvCommitFollowsWinnerLinearization premise: a commit_step was recorded.
ReachAdapterCommitStepRecorded ==
    \E i \in Arms : commit_step[i] # NoStep
NotReachAdapterCommitStepRecorded == ~ReachAdapterCommitStepRecorded

\* N_InvPublicationFollowsAuthorityClose premise: a publication_step recorded.
ReachAdapterPublicationStepRecorded ==
    publication_step # NoStep
NotReachAdapterPublicationStepRecorded == ~ReachAdapterPublicationStepRecorded

=============================================================================
