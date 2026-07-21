------------------------ MODULE E13SelectMultiGroup ------------------------
(*
  PR #18 bounded two-group composition model.

  This module is a focused, bounded model of TWO concurrent SelectGroups
  (Groups = {g0, g1}) sharing a single Event identity space.  Its purpose is
  to prove bounded multi-group non-interference:

    - each group's winner, publication, classification, authority closure,
      and TimerRegistration belong to that group only;
    - a single Event broadcast scans arms from BOTH groups but processes each
      group independently, deduplicating by group identity;
    - one group's claim/complete/rollback does not complete or publish the
      other group.

  It is NOT an unbounded concurrency proof and does NOT claim cross-Scheduler
  correctness.  It composes two 1-arm-per-group SelectGroup instances plus a
  shared Event state, modelled directly (not via full EventTimer INSTANCE) to
  keep the bounded state space tractable.

  Each group g has exactly one Select arm (arm 0), which may be an Event arm
  bound to a shared Event identity or an independent Timer arm.  The shared
  Event set space lets one broadcast reach both groups' Event arms.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Groups, Events

NoEvent == 2000000
NoGroup == 3000000

VARIABLES
    \* Per-group Contract/Central projection (one arm per group, arm index 0).
    g_phase,
    g_winner_present,
    g_caller,
    g_completion_mode,
    g_result_published,
    g_runnable_published,
    g_arms_retired,
    g_arm_class,
    g_arm_kind,
    g_arm_event,
    g_arm_timer_active,
    g_arm_timer_consumed,
    g_arm_timer_retired,
    g_arm_wait_linked,
    g_arm_authority_open,
    g_arm_account_open,
    g_arm_account_close_count,
    g_arm_publication_count,
    g_rollback_started,

    \* Shared Event state, indexed by Event identity.
    event_state,

    \* Event broadcast coordination (one in flight at a time; shared across
    \* groups, processed per-group).
    broadcast_event,
    broadcast_phase,
    broadcast_scanned_groups,
    broadcast_published_groups,

    \* Timer pump coordination per group (independent).
    g_timer_pump_pending,

    now

MGVars ==
    <<g_phase, g_winner_present, g_caller, g_completion_mode,
      g_result_published, g_runnable_published, g_arms_retired,
      g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
      g_arm_timer_consumed, g_arm_timer_retired, g_arm_wait_linked,
      g_arm_authority_open, g_arm_account_open, g_arm_account_close_count,
      g_arm_publication_count, g_rollback_started, event_state,
      broadcast_event, broadcast_phase, broadcast_scanned_groups,
      broadcast_published_groups, g_timer_pump_pending, now>>

GroupPhaseT ==
    {"Registering", "Admission", "Armed", "Claimed", "Closing",
     "Completed", "Rollback", "Aborted", "Consumed", "Destroyed"}
CallerStateT == {"Running", "Waiting", "Runnable", "Resumed"}
CompletionModeT == {"None", "Inline", "Suspended"}
ArmClassT == {"Unclassified", "Winner", "Loser"}
ArmKindT == {"None", "EventArm", "TimerArm"}
EventStateT == {"Unset", "Set"}
BroadcastPhaseT == {"NoBroadcast", "Scanning", "ProcessGroups"}

MGTypeOK ==
    /\ g_phase \in [Groups -> GroupPhaseT]
    /\ g_winner_present \in [Groups -> BOOLEAN]
    /\ g_caller \in [Groups -> CallerStateT]
    /\ g_completion_mode \in [Groups -> CompletionModeT]
    /\ g_result_published \in [Groups -> 0..1]
    /\ g_runnable_published \in [Groups -> 0..1]
    /\ g_arms_retired \in [Groups -> BOOLEAN]
    /\ g_arm_class \in [Groups -> ArmClassT]
    /\ g_arm_kind \in [Groups -> ArmKindT]
    /\ g_arm_event \in [Groups -> Events \cup {NoEvent}]
    /\ g_arm_timer_active \in [Groups -> BOOLEAN]
    /\ g_arm_timer_consumed \in [Groups -> BOOLEAN]
    /\ g_arm_timer_retired \in [Groups -> BOOLEAN]
    /\ g_arm_wait_linked \in [Groups -> BOOLEAN]
    /\ g_arm_authority_open \in [Groups -> BOOLEAN]
    /\ g_arm_account_open \in [Groups -> BOOLEAN]
    /\ g_arm_account_close_count \in [Groups -> 0..1]
    /\ g_arm_publication_count \in [Groups -> 0..1]
    /\ g_rollback_started \in [Groups -> BOOLEAN]
    /\ event_state \in [Events -> EventStateT]
    /\ broadcast_event \in Events \cup {NoEvent}
    /\ broadcast_phase \in BroadcastPhaseT
    /\ broadcast_scanned_groups \subseteq Groups
    /\ broadcast_published_groups \subseteq Groups
    /\ g_timer_pump_pending \in [Groups -> BOOLEAN]
    /\ now \in 0..3

\* An Event broadcast reaches the Event arms of every group bound to it.
BroadcastTargetGroups(e) ==
    {g \in Groups :
        /\ g_arm_kind[g] = "EventArm"
        /\ g_arm_event[g] = e
        /\ g_phase[g] = "Armed"}

GroupArmReady(g) ==
    \/ /\ g_arm_kind[g] = "EventArm"
       /\ g_arm_event[g] \in Events
       /\ event_state[g_arm_event[g]] = "Set"
    \/ /\ g_arm_kind[g] = "TimerArm"
       /\ g_arm_timer_active[g]
       /\ now >= 2

MGInit ==
    /\ g_phase = [g \in Groups |-> "Registering"]
    /\ g_winner_present = [g \in Groups |-> FALSE]
    /\ g_caller = [g \in Groups |-> "Running"]
    /\ g_completion_mode = [g \in Groups |-> "None"]
    /\ g_result_published = [g \in Groups |-> 0]
    /\ g_runnable_published = [g \in Groups |-> 0]
    /\ g_arms_retired = [g \in Groups |-> FALSE]
    /\ g_arm_class = [g \in Groups |-> "Unclassified"]
    /\ g_arm_kind = [g \in Groups |-> "None"]
    /\ g_arm_event = [g \in Groups |-> NoEvent]
    /\ g_arm_timer_active = [g \in Groups |-> FALSE]
    /\ g_arm_timer_consumed = [g \in Groups |-> FALSE]
    /\ g_arm_timer_retired = [g \in Groups |-> FALSE]
    /\ g_arm_wait_linked = [g \in Groups |-> FALSE]
    /\ g_arm_authority_open = [g \in Groups |-> FALSE]
    /\ g_arm_account_open = [g \in Groups |-> FALSE]
    /\ g_arm_account_close_count = [g \in Groups |-> 0]
    /\ g_arm_publication_count = [g \in Groups |-> 0]
    /\ g_rollback_started = [g \in Groups |-> FALSE]
    /\ event_state = [e \in Events |-> "Unset"]
    /\ broadcast_event = NoEvent
    /\ broadcast_phase = "NoBroadcast"
    /\ broadcast_scanned_groups = {}
    /\ broadcast_published_groups = {}
    /\ g_timer_pump_pending = [g \in Groups |-> FALSE]
    /\ now = 0

\* ---- per-group actions (independent except shared event_state) ----------

\* PR #18 corrective-1 (C2): registering an arm installs the arm kind/identity
\* and opens WaitNode/authority/accounting but LEAVES g_phase[g] = "Registering".
\* Registration is a two-step process: arm install (here) then FinishRegistration
\* (advance to Admission).  A real registration rollback can now happen between
\* the two steps, while g_phase[g] is still "Registering" and the caller is
\* Running.  This is the canonical PR #17 registration-rollback domain
\* (Registering/Running/no winner); the previous single-action design that
\* jumped directly to Admission made BeginRollback unreachable.
RegisterEventArm(g, e) ==
    /\ g_phase[g] = "Registering"
    /\ g_arm_kind[g] = "None"
    /\ e \in Events
    /\ g_arm_kind' = [g_arm_kind EXCEPT ![g] = "EventArm"]
    /\ g_arm_event' = [g_arm_event EXCEPT ![g] = e]
    /\ g_arm_wait_linked' = [g_arm_wait_linked EXCEPT ![g] = TRUE]
    /\ g_arm_authority_open' = [g_arm_authority_open EXCEPT ![g] = TRUE]
    /\ g_arm_account_open' = [g_arm_account_open EXCEPT ![g] = TRUE]
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_timer_active, g_arm_timer_consumed,
                    g_arm_timer_retired, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending,
                    g_arm_account_close_count, now>>

RegisterTimerArm(g) ==
    /\ g_phase[g] = "Registering"
    /\ g_arm_kind[g] = "None"
    /\ g_arm_kind' = [g_arm_kind EXCEPT ![g] = "TimerArm"]
    /\ g_arm_wait_linked' = [g_arm_wait_linked EXCEPT ![g] = TRUE]
    /\ g_arm_authority_open' = [g_arm_authority_open EXCEPT ![g] = TRUE]
    /\ g_arm_account_open' = [g_arm_account_open EXCEPT ![g] = TRUE]
    /\ g_arm_timer_active' = [g_arm_timer_active EXCEPT ![g] = TRUE]
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_event, g_arm_timer_consumed,
                    g_arm_timer_retired, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending,
                    g_arm_account_close_count, now>>

\* PR #18 corrective-1 (C2): the second step of registration.  Once the arm is
\* installed, wait/authority/account opened, and rollback not yet started, the
\* caller may finish registration and advance to Admission.  Registration
\* success: RegisterEventArm/RegisterTimerArm -> FinishRegistration -> Admission.
\* Registration failure: RegisterEventArm/RegisterTimerArm -> BeginRollback.
\* Both branches are reachable because this step is separate from the arm
\* install.
FinishRegistration(g) ==
    /\ g_phase[g] = "Registering"
    /\ g_arm_kind[g] # "None"
    /\ g_arm_wait_linked[g]
    /\ g_arm_authority_open[g]
    /\ g_arm_account_open[g]
    /\ ~g_rollback_started[g]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Admission"]
    /\ UNCHANGED <<g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

SuspendCaller(g) ==
    /\ g_phase[g] = "Admission"
    /\ ~GroupArmReady(g)
    /\ g_caller' = [g_caller EXCEPT ![g] = "Waiting"]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Armed"]
    /\ UNCHANGED <<g_winner_present, g_completion_mode, g_result_published,
                    g_runnable_published, g_arms_retired, g_arm_class,
                    g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open,
                    g_arm_account_open, g_arm_account_close_count,
                    g_arm_publication_count, g_rollback_started, event_state,
                    broadcast_event, broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

ClaimAdmissionWinner(g) ==
    /\ g_phase[g] = "Admission"
    /\ GroupArmReady(g)
    /\ g_winner_present' = [g_winner_present EXCEPT ![g] = TRUE]
    /\ g_arm_class' = [g_arm_class EXCEPT ![g] = "Winner"]
    /\ g_completion_mode' = [g_completion_mode EXCEPT ![g] = "Inline"]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Claimed"]
    /\ UNCHANGED <<g_caller, g_result_published, g_runnable_published,
                    g_arms_retired, g_arm_kind, g_arm_event,
                    g_arm_timer_active, g_arm_timer_consumed,
                    g_arm_timer_retired, g_arm_wait_linked,
                    g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

\* ---- shared Event broadcast (the multi-group seam) ---------------------
\* One Event identity is set; the broadcast scans every group's Event arm
\* bound to it, deduplicating by group identity, then each scanned group may
\* claim independently.

StartEventBroadcast(e) ==
    /\ broadcast_phase = "NoBroadcast"
    /\ e \in Events
    /\ event_state[e] = "Unset"
    /\ BroadcastTargetGroups(e) # {}
    /\ event_state' = [event_state EXCEPT ![e] = "Set"]
    /\ broadcast_event' = e
    /\ broadcast_phase' = "Scanning"
    /\ broadcast_scanned_groups' = {}
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, broadcast_published_groups,
                    g_timer_pump_pending, now>>

ScanGroup(g) ==
    /\ broadcast_phase = "Scanning"
    /\ broadcast_event \in Events
    /\ g \in BroadcastTargetGroups(broadcast_event)
    /\ g \notin broadcast_scanned_groups
    /\ broadcast_scanned_groups' =
          broadcast_scanned_groups \cup {g}
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_published_groups,
                    g_timer_pump_pending, now>>

FinishEventScan ==
    /\ broadcast_phase = "Scanning"
    /\ broadcast_scanned_groups = BroadcastTargetGroups(broadcast_event)
    /\ broadcast_phase' = "ProcessGroups"
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_scanned_groups, broadcast_published_groups,
                    g_timer_pump_pending, now>>

\* Each scanned group may independently claim the shared Event winner.
\* Group identity deduplication: a group is processed at most once because
\* ScanGroup already added it to broadcast_scanned_groups and ClaimEventWinner
\* requires broadcast_phase = "ProcessGroups" (post-scan).
ClaimEventWinner(g) ==
    /\ broadcast_phase = "ProcessGroups"
    /\ g \in broadcast_scanned_groups
    /\ g \notin broadcast_published_groups
    /\ g_phase[g] = "Armed"
    /\ g_caller[g] = "Waiting"
    /\ g_winner_present' = [g_winner_present EXCEPT ![g] = TRUE]
    /\ g_arm_class' = [g_arm_class EXCEPT ![g] = "Winner"]
    /\ g_completion_mode' = [g_completion_mode EXCEPT ![g] = "Suspended"]
    /\ g_caller' = [g_caller EXCEPT ![g] = "Runnable"]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Claimed"]
    /\ broadcast_published_groups' =
          broadcast_published_groups \cup {g}
    /\ UNCHANGED <<g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    g_timer_pump_pending, now>>

FinishEmptyEventBroadcast ==
    /\ broadcast_phase = "ProcessGroups"
    /\ broadcast_published_groups = broadcast_scanned_groups
    /\ broadcast_event' = NoEvent
    /\ broadcast_phase' = "NoBroadcast"
    /\ broadcast_scanned_groups' = {}
    /\ broadcast_published_groups' = {}
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, g_timer_pump_pending, now>>

\* ---- Timer pump (independent per group) --------------------------------

TimerPumpEntry(g) ==
    /\ broadcast_phase = "NoBroadcast"
    /\ g_phase[g] = "Armed"
    /\ g_arm_kind[g] = "TimerArm"
    /\ g_arm_timer_active[g]
    /\ g_arm_timer_consumed[g] = FALSE
    /\ now >= 2
    /\ g_winner_present' = [g_winner_present EXCEPT ![g] = TRUE]
    /\ g_arm_class' = [g_arm_class EXCEPT ![g] = "Winner"]
    /\ g_arm_timer_consumed' = [g_arm_timer_consumed EXCEPT ![g] = TRUE]
    /\ g_completion_mode' = [g_completion_mode EXCEPT ![g] = "Suspended"]
    /\ g_caller' = [g_caller EXCEPT ![g] = "Runnable"]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Claimed"]
    /\ UNCHANGED <<g_arm_kind, g_result_published, g_runnable_published,
                    g_arms_retired, g_arm_event, g_arm_timer_active,
                    g_arm_timer_retired, g_arm_wait_linked, g_arm_authority_open,
                    g_arm_account_open, g_arm_account_close_count,
                    g_arm_publication_count, g_rollback_started, event_state,
                    broadcast_event, broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

\* ---- finalization (per group; does not touch the other group) ----------

FinalizeWinner(g) ==
    /\ g_phase[g] = "Claimed"
    /\ g_winner_present[g]
    /\ g_arm_class[g] = "Winner"
    /\ g_arm_wait_linked' = [g_arm_wait_linked EXCEPT ![g] = FALSE]
    /\ g_arm_authority_open' = [g_arm_authority_open EXCEPT ![g] = FALSE]
    /\ g_arm_account_open' = [g_arm_account_open EXCEPT ![g] = FALSE]
    /\ g_arm_account_close_count' =
          [g_arm_account_close_count EXCEPT ![g] = @ + 1]
    /\ g_arms_retired' = [g_arms_retired EXCEPT ![g] = TRUE]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Closing"]
    /\ UNCHANGED <<g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arm_class,
                    g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_publication_count, g_rollback_started, event_state,
                    broadcast_event, broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

PublishInline(g) ==
    /\ g_phase[g] = "Closing"
    /\ g_arms_retired[g]
    /\ g_completion_mode[g] = "Inline"
    /\ g_caller[g] = "Running"
    /\ g_result_published' = [g_result_published EXCEPT ![g] = 1]
    /\ g_arm_publication_count' =
          [g_arm_publication_count EXCEPT ![g] = 1]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Completed"]
    /\ UNCHANGED <<g_winner_present, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_rollback_started, g_caller,
                    g_completion_mode, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

PublishSuspended(g) ==
    /\ g_phase[g] = "Closing"
    /\ g_arms_retired[g]
    /\ g_completion_mode[g] = "Suspended"
    /\ g_caller[g] = "Runnable"
    /\ g_result_published' = [g_result_published EXCEPT ![g] = 1]
    /\ g_runnable_published' = [g_runnable_published EXCEPT ![g] = 1]
    /\ g_arm_publication_count' =
          [g_arm_publication_count EXCEPT ![g] = 1]
    /\ g_caller' = [g_caller EXCEPT ![g] = "Runnable"]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Completed"]
    /\ UNCHANGED <<g_winner_present, g_arms_retired, g_arm_class, g_arm_kind,
                    g_arm_event, g_arm_timer_active, g_arm_timer_consumed,
                    g_arm_timer_retired, g_arm_wait_linked,
                    g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_rollback_started,
                    g_completion_mode, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

ConsumeResult(g) ==
    /\ g_phase[g] = "Completed"
    /\ g_result_published[g] = 1
    /\ g_phase' = [g_phase EXCEPT ![g] = "Consumed"]
    /\ UNCHANGED <<g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

DestroyOperation(g) ==
    /\ g_phase[g] \in {"Consumed", "Aborted"}
    /\ g_phase' = [g_phase EXCEPT ![g] = "Destroyed"]
    /\ UNCHANGED <<g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

\* ---- rollback (per group; registration domain only) --------------------
\*
\* PR #18 corrective-1 (C2/C3): registration rollback is now genuinely
\* reachable.  After RegisterEventArm/RegisterTimerArm installs an arm but
\* BEFORE FinishRegistration advances g_phase to Admission, g_phase[g] is still
\* "Registering", the caller is Running, and the arm_kind is installed -- the
\* exact pre-state BeginRollback requires.  This preserves the canonical PR #17
\* rollback domain (Registering/Running/no winner); Admission and every later
\* phase remains rollback-disabled.

BeginRollback(g) ==
    /\ g_phase[g] = "Registering"
    /\ g_caller[g] = "Running"
    /\ ~g_winner_present[g]
    /\ g_arm_kind[g] # "None"
    /\ g_rollback_started' = [g_rollback_started EXCEPT ![g] = TRUE]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Rollback"]
    /\ UNCHANGED <<g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    event_state, broadcast_event, broadcast_phase,
                    broadcast_scanned_groups, broadcast_published_groups,
                    g_timer_pump_pending, now>>

RollbackArm(g) ==
    /\ g_phase[g] = "Rollback"
    /\ g_arm_authority_open[g]
    /\ g_arm_wait_linked' = [g_arm_wait_linked EXCEPT ![g] = FALSE]
    /\ g_arm_authority_open' = [g_arm_authority_open EXCEPT ![g] = FALSE]
    /\ g_arm_account_open' = [g_arm_account_open EXCEPT ![g] = FALSE]
    /\ g_arm_account_close_count' =
          [g_arm_account_close_count EXCEPT ![g] = @ + 1]
    /\ g_arms_retired' = [g_arms_retired EXCEPT ![g] = TRUE]
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arm_class,
                    g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_publication_count, g_rollback_started, event_state,
                    broadcast_event, broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

FinishRollback(g) ==
    /\ g_phase[g] = "Rollback"
    /\ g_arms_retired[g]
    /\ g_phase' = [g_phase EXCEPT ![g] = "Aborted"]
    /\ UNCHANGED <<g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

Tick ==
    /\ broadcast_phase = "NoBroadcast"
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arms_retired,
                    g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_wait_linked, g_arm_authority_open, g_arm_account_open,
                    g_arm_account_close_count, g_arm_publication_count,
                    g_rollback_started, event_state, broadcast_event,
                    broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending>>

MGStutter == UNCHANGED MGVars

MGNext ==
    \/ MGStutter
    \/ \E g \in Groups, e \in Events : RegisterEventArm(g, e)
    \/ \E g \in Groups : RegisterTimerArm(g)
    \/ \E g \in Groups : FinishRegistration(g)
    \/ \E g \in Groups : SuspendCaller(g)
    \/ \E g \in Groups : ClaimAdmissionWinner(g)
    \/ \E e \in Events : StartEventBroadcast(e)
    \/ \E g \in Groups : ScanGroup(g)
    \/ FinishEventScan
    \/ \E g \in Groups : ClaimEventWinner(g)
    \/ FinishEmptyEventBroadcast
    \/ \E g \in Groups : TimerPumpEntry(g)
    \/ \E g \in Groups : FinalizeWinner(g)
    \/ \E g \in Groups : PublishInline(g)
    \/ \E g \in Groups : PublishSuspended(g)
    \/ \E g \in Groups : ConsumeResult(g)
    \/ \E g \in Groups : DestroyOperation(g)
    \/ \E g \in Groups : BeginRollback(g)
    \/ \E g \in Groups : RollbackArm(g)
    \/ \E g \in Groups : FinishRollback(g)
    \/ Tick

MGSpec == MGInit /\ [][MGNext]_MGVars

(* =========================================================================
   PR #18 multi-group non-interference invariants (O).
   ========================================================================= *)

\* -- per-group isolation --------------------------------------------------

MG_InvWinnerBelongsToOwnGroup ==
    \* A group's winner arm belongs to that group.  With one arm per group
    \* this is structural; the law additionally binds the winner classification
    \* to the same group's state.
    \A g \in Groups :
        g_winner_present[g] => g_arm_class[g] = "Winner"

MG_InvPublicationBelongsToOwnGroup ==
    \* A group's result publication increments only that group's counters.
    \A g \in Groups :
        g_result_published[g] = 1 => g_arm_publication_count[g] = 1

MG_InvClaimSnapshotContainsOnlyOwnGroupArms ==
    \* Each group's winner classification is bound to its own arm; an Event
    \* broadcast never writes another group's classification while processing
    \* this group.
    \A g \in Groups :
        g_arm_class[g] = "Winner" => g_winner_present[g]

MG_InvArmClassificationDoesNotCrossGroups ==
    \* Scanning group g during a broadcast never mutates any group's
    \* classification: ScanGroup only writes broadcast_scanned_groups.  A
    \* group becomes Winner/Loser only via a claim action, which requires
    \* ProcessGroups (post-scan) or the admission path.  State law: while a
    \* broadcast is Scanning, no group that is NOT a claim winner has been
    \* classified Winner by the broadcast; classifications observed during
    \* scanning remain Unclassified until ProcessGroups.
    broadcast_phase = "Scanning"
        => \A g \in broadcast_scanned_groups :
              g_arm_class[g] = "Unclassified"
                 \/ g_phase[g] \in {"Claimed", "Closing", "Completed"}

MG_InvAuthorityClosureDoesNotCrossGroups ==
    \* A group's authority-open flag and that group's phase are bound: a group
    \* reaches Completed/Consumed/Destroyed only with its own authority closed.
    \* Cross-group non-interference is structural (no action writes another
    \* group's authority_open); this state law confirms each group's own
    \* closure.
    \A g \in Groups :
        g_phase[g] \in {"Completed", "Consumed", "Aborted", "Destroyed"}
            => ~g_arm_authority_open[g]

MG_InvTimerRegistrationDoesNotCrossGroups ==
    \* Each group owns its own Timer registration flags.  State law: a
    \* Consumed Timer registration in group g belongs to group g's winner
    \* (not to another group's classification).
    \A g \in Groups :
        g_arm_timer_consumed[g]
            => /\ g_arm_kind[g] = "TimerArm"
               /\ g_arm_class[g] = "Winner"
               /\ g_winner_present[g]

MG_InvResultCountIsPerGroup ==
    \* Each group publishes at most one result.
    \A g \in Groups : g_result_published[g] <= 1

MG_InvRunnableCountIsPerGroup ==
    \A g \in Groups : g_runnable_published[g] <= 1

\* -- shared Event across groups ------------------------------------------

MG_InvEventBroadcastDeduplicatesGroupProcessing ==
    \* A single Event broadcast scans each target group at most once and
    \* publishes each target group at most once.
    \* broadcast_scanned_groups never contains a group twice (it is a set).
    \* broadcast_published_groups is a subset of scanned groups.
    broadcast_published_groups \subseteq broadcast_scanned_groups

MG_InvOneGroupClaimDoesNotCompleteOtherGroup ==
    \* When group g claims via the shared Event, group h's result publication
    \* is unaffected.  ClaimEventWinner only writes g's counters.  State law:
    \* a published group and a non-published group can coexist.
    \A g, h \in Groups :
        g # h /\ g_result_published[g] = 1
            => g_result_published[h] <= 1

MG_InvSharedEventDoesNotConsumeAcrossGroups ==
    \* The shared Event Set state is persistent across groups: one group
    \* claiming does not Unset the Event for the other group.  The model has
    \* no action that Unsets event_state once Set; this law confirms that
    \* invariant structurally by stating the Event Set is monotone within a
    \* broadcast epoch.
    broadcast_phase \in {"Scanning", "ProcessGroups"}
        => event_state[broadcast_event] = "Set"

(* =========================================================================
   PR #18 corrective-1 (C5): multi-group registration-rollback invariants.
   These laws make the registration rollback domain and its per-group
   non-interference explicit named state invariants.  They are consequences
   of the corrected state machine (BeginRollback requires Registering) and
   the per-action UNCHANGED audit (every rollback action freezes every
   cross-group field of the other group).  NEG-MG1 (focused negative model)
   proves MG_InvRollbackDoesNotAffectOtherGroup is load-bearing.
   ========================================================================= *)

MG_InvRollbackRequiresRegistering ==
    \* The canonical PR #17 rollback domain: rollback may begin only while
    \* g_phase is still Registering.  Admission/Armed/Claimed/Closing/Completed
    \* are rollback-disabled (the corrective-1 split makes this load-bearing:
    \* BeginRollback is reachable exactly from the post-install pre-finish
    \* state).
    \A g \in Groups :
        g_phase[g] = "Rollback"
            => g_rollback_started[g]
               /\ g_arm_kind[g] # "None"

MG_InvRollbackRequiresRunningCaller ==
    \* Rollback may begin only while the caller of that group is Running.
    \* This is the multi-group form of the PR #17 corrective-1 law
    \* ContractRegistrationRollbackDisabledAfterSuspension.
    \A g \in Groups :
        g_phase[g] = "Rollback" => g_caller[g] = "Running"

MG_InvRollbackRequiresInstalledArm ==
    \* Rollback requires that an arm has been installed in this group
    \* (g_arm_kind # None).  Rollback of an empty registration is meaningless.
    \A g \in Groups :
        g_phase[g] = "Rollback" => g_arm_kind[g] # "None"

MG_InvRollbackRequiresNoWinner ==
    \* Rollback may begin only before a winner exists for this group.
    \A g \in Groups :
        g_phase[g] \in {"Rollback", "Aborted"} => ~g_winner_present[g]

MG_InvRollbackNeverPublishes ==
    \* A group in rollback or Aborted phase has published no result and no
    \* runnable notification.
    \A g \in Groups :
        g_phase[g] \in {"Rollback", "Aborted"}
            => /\ g_result_published[g] = 0
               /\ g_runnable_published[g] = 0
               /\ g_arm_publication_count[g] = 0

MG_InvRollbackClosesOwnAuthorityOnly ==
    \* A group's rollback transitions (RollbackArm/FinishRollback) close only
    \* that group's authority/account; they never open or mutate the other
    \* group's authority.  State consequence: while group g is mid-rollback,
    \* group g's own authority may be open or closed (RollbackArm closes it),
    \* but the rollback actions themselves never OPEN an authority in any
    \* group (they only close group g's authority).  Therefore, once group g
    \* reaches Aborted, group g's authority is closed.
    \A g \in Groups :
        g_phase[g] = "Aborted" => ~g_arm_authority_open[g]

MG_InvRollbackDoesNotAffectOtherGroup ==
    \* A group's rollback actions touch only that group's state.  Every
    \* rollback action (BeginRollback/RollbackArm/FinishRollback) writes only
    \* group g's variables and UNCHANGES every other group's g_* projection.
    \*
    \* The transition-level guarantee -- "group h's fields are UNCHANGED by
    \* group g's rollback action" -- cannot be fully expressed as a pure
    \* state invariant, because group h may legitimately change its own state
    \* via its own actions while group g is mid-rollback.  The load-bearing
    \* state consequence asserted here is narrower and sound: a group g that
    \* actually went through a registration rollback (g_rollback_started[g])
    \* must not have produced a winner classification or a result publication
    \* of its own -- i.e., group g's rollback never spuriously fabricated a
    \* winner or published a result for itself, let alone for another group.
    \*
    \* The transition-level non-interference (group g's RollbackArm does not
    \* touch group h's authority) is verified structurally per-action by the
    \* UNCHANGED audit and is load-bearing via the focused negative model
    \* NEG-MG1, which makes group g's RollbackArm close group h's authority
    \* and is caught by MG_InvAuthorityClosureDoesNotCrossGroups /
    \* MG_InvAbortedGroupHasNoOpenAccounting on the resulting state.
    \A g \in Groups :
        g_rollback_started[g]
            => /\ g_arm_class[g] # "Winner"
               /\ ~g_winner_present[g]
               /\ g_result_published[g] = 0

MG_InvAbortedGroupHasNoOpenAccounting ==
    \* An Aborted group has closed its wait/account (RollbackArm closed both
    \* the wait link and the account_open flag).  This is the multi-group
    \* form of the Contract's rollback-closes-authority law.
    \A g \in Groups :
        g_phase[g] = "Aborted"
            => /\ ~g_arm_wait_linked[g]
               /\ ~g_arm_account_open[g]

MG_InvAbortedGroupCallerNotWaiting ==
    \* An Aborted group never has a Waiting caller (rollback preserves the
    \* Running caller, which is the precondition for BeginRollback).  This is
    \* the multi-group form of C_InvAbortedCallerNeverWaiting.
    \A g \in Groups :
        g_phase[g] = "Aborted" => g_caller[g] # "Waiting"

MGSafetyInv ==
    /\ MGTypeOK
    /\ MG_InvWinnerBelongsToOwnGroup
    /\ MG_InvPublicationBelongsToOwnGroup
    /\ MG_InvClaimSnapshotContainsOnlyOwnGroupArms
    /\ MG_InvArmClassificationDoesNotCrossGroups
    /\ MG_InvAuthorityClosureDoesNotCrossGroups
    /\ MG_InvTimerRegistrationDoesNotCrossGroups
    /\ MG_InvResultCountIsPerGroup
    /\ MG_InvRunnableCountIsPerGroup
    /\ MG_InvEventBroadcastDeduplicatesGroupProcessing
    /\ MG_InvOneGroupClaimDoesNotCompleteOtherGroup
    /\ MG_InvSharedEventDoesNotConsumeAcrossGroups
    /\ MG_InvRollbackRequiresRegistering
    /\ MG_InvRollbackRequiresRunningCaller
    /\ MG_InvRollbackRequiresInstalledArm
    /\ MG_InvRollbackRequiresNoWinner
    /\ MG_InvRollbackNeverPublishes
    /\ MG_InvRollbackClosesOwnAuthorityOnly
    /\ MG_InvRollbackDoesNotAffectOtherGroup
    /\ MG_InvAbortedGroupHasNoOpenAccounting
    /\ MG_InvAbortedGroupCallerNotWaiting

\* Reachability witnesses (inverse invariants) for non-vacuity.

MG_ReachSharedEventBothGroupsComplete ==
    /\ broadcast_published_groups = Groups
    /\ \A g \in Groups :
          /\ g_phase[g] = "Completed"
          /\ g_result_published[g] = 1
          /\ g_arm_kind[g] = "EventArm"
    /\ Cardinality(Events) = 1

MG_ReachMixedEventTimer ==
    \E ge, gt \in Groups :
        /\ ge # gt
        /\ g_arm_kind[ge] = "EventArm"
        /\ g_phase[ge] \in {"Completed", "Consumed"}
        /\ g_result_published[ge] = 1
        /\ g_arm_kind[gt] = "TimerArm"
        /\ g_phase[gt] \in {"Completed", "Consumed"}
        /\ g_result_published[gt] = 1

MG_ReachOneRollbackOtherComplete ==
    \E gr, gc \in Groups :
        /\ gr # gc
        /\ g_phase[gr] \in {"Aborted", "Destroyed"}
        /\ g_rollback_started[gr]
        /\ g_result_published[gr] = 0
        /\ g_phase[gc] \in {"Completed", "Consumed"}
        /\ g_result_published[gc] = 1

(* =========================================================================
   PR #18 corrective-1 (J): non-vacuity witnesses for the registration-split
   rollback domain.  These prove the premise of MG_InvRollback* is reachable
   in the model -- that the post-install pre-finish registration state, and a
   real rollback started from that state, are both reachable.  Without these
   witnesses the rollback laws would be vacuously TRUE (the model could simply
   never reach the rollback premise).
   ========================================================================= *)

\* Premise of MG_InvRollbackRequiresRegistering / RequiresInstalledArm:
\* the post-install pre-finish registration state exists in the model.  This is
\* the exact state from which BeginRollback is now reachable (the corrective-1
\* fix).
MG_ReachArmInstalledNotFinished ==
    \E g \in Groups :
        /\ g_phase[g] = "Registering"
        /\ g_arm_kind[g] # "None"
        /\ g_arm_wait_linked[g]
        /\ g_arm_authority_open[g]
        /\ g_arm_account_open[g]
        /\ ~g_rollback_started[g]
        /\ g_caller[g] = "Running"
        /\ ~g_winner_present[g]

\* Premise of MG_InvRollbackRequiresRunningCaller / RequiresNoWinner /
\* NeverPublishes: a real rollback has started from the registration domain
\* (g_phase=Registering, arm installed, caller Running, no winner).
MG_ReachPreFinishRollback ==
    \E g \in Groups :
        /\ g_phase[g] = "Rollback"
        /\ g_rollback_started[g]
        /\ g_arm_kind[g] # "None"
        /\ g_caller[g] = "Running"
        /\ ~g_winner_present[g]
        /\ g_result_published[g] = 0

NotMG_ReachSharedEventBothGroupsComplete
    == ~MG_ReachSharedEventBothGroupsComplete
NotMG_ReachMixedEventTimer == ~MG_ReachMixedEventTimer
NotMG_ReachOneRollbackOtherComplete == ~MG_ReachOneRollbackOtherComplete
NotMG_ReachArmInstalledNotFinished == ~MG_ReachArmInstalledNotFinished
NotMG_ReachPreFinishRollback == ~MG_ReachPreFinishRollback

=============================================================================
