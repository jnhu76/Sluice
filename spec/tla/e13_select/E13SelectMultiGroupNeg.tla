---------------------- MODULE E13SelectMultiGroupNeg ----------------------
(*
  PR #18 corrective-1 multi-group negative models (MG: NEG-MG1..).

  Wraps the canonical E13SelectMultiGroup by INSTANCE-WITH, where every
  multi-group variable is bound to the same-named wrapper variable.  The
  canonical MGNext is reused unchanged via the INSTANCE; the wrapper's Next
  disjoins it with ONE focused fault action selected by the constant FAULT.
  Each fault action is REACHABLE from a legal multi-group state and mutates
  state so that exactly its target named invariant fails.

  Rules (Q):
  - one focused fault per negative model;
  - canonical MGSpec / MGSafetyInv are NOT modified;
  - every fault action is reachable from a legal state;
  - the expected target invariant fails when the fault is enabled;
  - fault disabled (FAULT = "None") restores PASS;
  - no ASSUME FALSE, no Init crippling, no state constraint hiding behavior.

  Each cfg selects a FAULT and asserts the matching target invariant by name.
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Groups, Events, FAULT

NoEvent == 2000000
NoGroup == 3000000

VARIABLES
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
    event_state,
    broadcast_event,
    broadcast_phase,
    broadcast_scanned_groups,
    broadcast_published_groups,
    g_timer_pump_pending,
    now,
    fault_used

Base == INSTANCE E13SelectMultiGroup
        WITH Groups <- Groups, Events <- Events,
             g_phase <- g_phase,
             g_winner_present <- g_winner_present,
             g_caller <- g_caller,
             g_completion_mode <- g_completion_mode,
             g_result_published <- g_result_published,
             g_runnable_published <- g_runnable_published,
             g_arms_retired <- g_arms_retired,
             g_arm_class <- g_arm_class,
             g_arm_kind <- g_arm_kind,
             g_arm_event <- g_arm_event,
             g_arm_timer_active <- g_arm_timer_active,
             g_arm_timer_consumed <- g_arm_timer_consumed,
             g_arm_timer_retired <- g_arm_timer_retired,
             g_arm_wait_linked <- g_arm_wait_linked,
             g_arm_authority_open <- g_arm_authority_open,
             g_arm_account_open <- g_arm_account_open,
             g_arm_account_close_count <- g_arm_account_close_count,
             g_arm_publication_count <- g_arm_publication_count,
             g_rollback_started <- g_rollback_started,
             event_state <- event_state,
             broadcast_event <- broadcast_event,
             broadcast_phase <- broadcast_phase,
             broadcast_scanned_groups <- broadcast_scanned_groups,
             broadcast_published_groups <- broadcast_published_groups,
             g_timer_pump_pending <- g_timer_pump_pending,
             now <- now

\* All canonical MG state plus the fault marker.
MGNegVars ==
    <<g_phase, g_winner_present, g_caller, g_completion_mode,
      g_result_published, g_runnable_published, g_arms_retired,
      g_arm_class, g_arm_kind, g_arm_event, g_arm_timer_active,
      g_arm_timer_consumed, g_arm_timer_retired, g_arm_wait_linked,
      g_arm_authority_open, g_arm_account_open, g_arm_account_close_count,
      g_arm_publication_count, g_rollback_started, event_state,
      broadcast_event, broadcast_phase, broadcast_scanned_groups,
      broadcast_published_groups, g_timer_pump_pending, now, fault_used>>

MGNegInit ==
    /\ Base!MGInit
    /\ fault_used = FALSE

FaultActive(name) == FAULT = name

\* ----- NEG-MG1: cross-group authority mutation during rollback -------------
\* Group g0 is mid-rollback (g_phase[g0]="Rollback", authority open).  Group
\* g1 has independently progressed to a terminal phase (Completed/Consumed/
\* Aborted/Destroyed) where its authority is correctly CLOSED.  The fault
\* makes group g0's RollbackArm spuriously RE-OPEN group g1's authority as a
\* side effect, even though group g1 is independent of group g0's rollback.
\* This violates the transition-level non-interference guarantee that the
\* per-action UNCHANGED audit establishes structurally (RollbackArm's
\* UNCHANGED list freezes every group-h field).  At the state level the
\* violation surfaces immediately as a broken MG_InvAuthorityClosureDoesNot
\* CrossGroups: a group at a terminal phase must have its authority closed,
\* and the fault has just opened group g1's.  The fault does NOT touch any
\* other field of group g1, so only this law trips.
Fault_MG1 ==
    /\ FaultActive("MG1")
    /\ ~fault_used
    /\ \E g0, g1 \in Groups :
          /\ g0 # g1
          /\ g_phase[g0] = "Rollback"
          /\ g_arm_authority_open[g0]
          /\ g_phase[g1] \in {"Completed", "Consumed", "Aborted", "Destroyed"}
          /\ ~g_arm_authority_open[g1]
          /\ g_arm_authority_open' =
                [g_arm_authority_open EXCEPT ![g0] = FALSE, ![g1] = TRUE]
          /\ g_arm_wait_linked' =
                [g_arm_wait_linked EXCEPT ![g0] = FALSE]
          /\ g_arm_account_open' =
                [g_arm_account_open EXCEPT ![g0] = FALSE]
          /\ g_arm_account_close_count' =
                [g_arm_account_close_count EXCEPT ![g0] = @ + 1]
          /\ g_arms_retired' =
                [g_arms_retired EXCEPT ![g0] = TRUE]
    /\ fault_used' = TRUE
    /\ UNCHANGED <<g_phase, g_winner_present, g_caller, g_completion_mode,
                    g_result_published, g_runnable_published, g_arm_class,
                    g_arm_kind, g_arm_event, g_arm_timer_active,
                    g_arm_timer_consumed, g_arm_timer_retired,
                    g_arm_publication_count, g_rollback_started, event_state,
                    broadcast_event, broadcast_phase, broadcast_scanned_groups,
                    broadcast_published_groups, g_timer_pump_pending, now>>

FaultNext ==
    \/ Fault_MG1

MGNegStutter == UNCHANGED MGNegVars

\* The canonical MGNext does not touch fault_used; freeze it here.
BaseNextFrozen == Base!MGNext /\ UNCHANGED fault_used

MGNegNext ==
    \/ MGNegStutter
    \/ BaseNextFrozen
    \/ FaultNext

MGNegSpec == MGNegInit /\ [][MGNegNext]_MGNegVars

\* Re-export the canonical safety invariants by name so each NEG cfg can
\* assert its target invariant.
MG_InvAuthorityClosureDoesNotCrossGroups ==
    Base!MG_InvAuthorityClosureDoesNotCrossGroups
MG_InvRollbackDoesNotAffectOtherGroup ==
    Base!MG_InvRollbackDoesNotAffectOtherGroup
MG_InvAbortedGroupHasNoOpenAccounting ==
    Base!MG_InvAbortedGroupHasNoOpenAccounting

\* Positive restoration: with FAULT = "None", the full MGSafetyInv must PASS
\* over the same state space.
MGSafetyInv == Base!MGSafetyInv

\* Reachability witness: the fault was actually used (non-deadlock evidence).
FaultUsedWitness == fault_used

=============================================================================
