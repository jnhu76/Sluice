--------------------- MODULE E16ApplicationRuntimeBuggyCloseOwner ---------------------
(*
  NEG-E16-4: Two close owners.

  Defect: join and shutdown both proceed without Open->InProgress owner
  election. Both callers can destroy resources.

  Expected violation: AtMostOneCloseOwner
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Tasks, Callers, MaxIO, NONE, T0, T1, C0, C1

ASSUME
    /\ Tasks = {T0, T1}
    /\ Callers = {C0, C1}
    /\ MaxIO = 2
    /\ NONE \notin Tasks \cup Callers

VARIABLES
    runtime_state, close_state, close_owner,
    driver_joined, group_alive, resources_alive,
    driver_state, driver_exit_requested, drain_complete

vars == <<runtime_state, close_state, close_owner,
          driver_joined, group_alive, resources_alive,
          driver_state, driver_exit_requested, drain_complete>>

Init ==
    /\ runtime_state = "Draining"
    /\ close_state = "Open"
    /\ close_owner = NONE
    /\ driver_joined = FALSE
    /\ group_alive = TRUE
    /\ resources_alive = TRUE
    /\ driver_state = "drained_wait"
    /\ driver_exit_requested = FALSE
    /\ drain_complete = TRUE

(* BUGGY: no owner election, any caller can proceed directly *)
CloseWithoutElection(c) ==
    /\ c \in Callers
    /\ runtime_state = "Draining"
    /\ drain_complete
    /\ driver_exit_requested' = TRUE
    /\ UNCHANGED <<runtime_state, close_state, close_owner,
                   driver_joined, group_alive, resources_alive,
                   driver_state, drain_complete>>

DriverExitAction ==
    /\ driver_exit_requested
    /\ driver_state = "drained_wait"
    /\ driver_state' = "exited"
    /\ UNCHANGED <<runtime_state, close_state, close_owner,
                   driver_joined, group_alive, resources_alive,
                   driver_exit_requested, drain_complete>>

(* BUGGY: both callers can join (no owner gate) *)
JoinByCaller(c) ==
    /\ c \in Callers
    /\ driver_state = "exited"
    /\ ~driver_joined
    /\ driver_joined' = TRUE
    /\ close_owner' = c
    /\ UNCHANGED <<runtime_state, close_state,
                   group_alive, resources_alive,
                   driver_state, driver_exit_requested, drain_complete>>

(* BUGGY: both callers can destroy (no owner gate) *)
DestroyByCaller(c) ==
    /\ c \in Callers
    /\ driver_joined
    /\ group_alive
    /\ group_alive' = FALSE
    /\ resources_alive' = FALSE
    /\ runtime_state' = "Stopped"
    /\ close_state' = "Closed"
    /\ UNCHANGED <<close_owner, driver_joined,
                   driver_state, driver_exit_requested, drain_complete>>

Next ==
    \/ \E c \in Callers : CloseWithoutElection(c)
    \/ DriverExitAction
    \/ \E c \in Callers : JoinByCaller(c)
    \/ \E c \in Callers : DestroyByCaller(c)

Spec == Init /\ [][Next]_vars

(* This invariant SHOULD be violated: two distinct callers both close *)
AtMostOneCloseOwner ==
    close_state = "Closed" =>
    (close_owner \in Callers /\
     \A c \in Callers : (c # close_owner) =>
        ~(driver_joined /\ ~group_alive))

=============================================================================
