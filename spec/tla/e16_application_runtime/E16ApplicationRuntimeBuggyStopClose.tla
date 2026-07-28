--------------------- MODULE E16ApplicationRuntimeBuggyStopClose ---------------------
(*
  NEG-E16-2: Stop vs close cancellation UAF.

  Defect: request_stop publishes state=Stopping, releases lifecycle_mutex,
  close owner destroys Group, THEN request_stop publishes root cancellation
  through destroyed Group.

  Expected violation: NoCancelAfterGroupDestroy
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Tasks, Callers, MaxIO, NONE, T0, T1, C0, C1

ASSUME
    /\ Tasks = {T0, T1}
    /\ Callers = {C0, C1}
    /\ MaxIO = 2
    /\ NONE \notin Tasks \cup Callers

VARIABLES
    runtime_state, stop_requested, root_cancel_published,
    group_alive, close_state, close_owner, driver_joined,
    resources_alive, scheduler_alive, io_context_alive,
    driver_state, driver_spawned, driver_exit_requested

vars == <<runtime_state, stop_requested, root_cancel_published,
          group_alive, close_state, close_owner, driver_joined,
          resources_alive, scheduler_alive, io_context_alive,
          driver_state, driver_spawned, driver_exit_requested>>

Init ==
    /\ runtime_state = "Running"
    /\ stop_requested = FALSE
    /\ root_cancel_published = FALSE
    /\ group_alive = TRUE
    /\ close_state = "Open"
    /\ close_owner = NONE
    /\ driver_joined = FALSE
    /\ resources_alive = TRUE
    /\ scheduler_alive = TRUE
    /\ io_context_alive = TRUE
    /\ driver_state = "drained_wait"
    /\ driver_spawned = TRUE
    /\ driver_exit_requested = FALSE

(* BUGGY: request_stop publishes Stopping FIRST, then cancels LATER
   (split across two transitions, allowing close to interleave) *)
RequestStopPhase1 ==
    /\ runtime_state = "Running"
    /\ ~stop_requested
    /\ stop_requested' = TRUE
    /\ runtime_state' = "Stopping"
    /\ UNCHANGED <<root_cancel_published, group_alive, close_state,
                   close_owner, driver_joined, resources_alive,
                   scheduler_alive, io_context_alive,
                   driver_state, driver_spawned, driver_exit_requested>>

(* BUGGY: cancellation published AFTER lock release, Group may be gone *)
RequestStopPhase2Cancel ==
    /\ runtime_state = "Stopping"
    /\ stop_requested
    /\ ~root_cancel_published
    /\ root_cancel_published' = TRUE
    /\ UNCHANGED <<runtime_state, stop_requested, group_alive,
                   close_state, close_owner, driver_joined,
                   resources_alive, scheduler_alive, io_context_alive,
                   driver_state, driver_spawned, driver_exit_requested>>

(* Close owner can interleave between Phase1 and Phase2 *)
CloseOwnerDestroy ==
    /\ runtime_state = "Stopping"
    /\ close_state = "Open"
    /\ close_state' = "InProgress"
    /\ close_owner' = C0
    /\ driver_exit_requested' = TRUE
    /\ UNCHANGED <<runtime_state, stop_requested, root_cancel_published,
                   group_alive, driver_joined, resources_alive,
                   scheduler_alive, io_context_alive,
                   driver_state, driver_spawned>>

DriverExitAction ==
    /\ driver_exit_requested
    /\ driver_state = "drained_wait"
    /\ driver_state' = "exited"
    /\ UNCHANGED <<runtime_state, stop_requested, root_cancel_published,
                   group_alive, close_state, close_owner, driver_joined,
                   resources_alive, scheduler_alive, io_context_alive,
                   driver_spawned, driver_exit_requested>>

JoinDriverAction ==
    /\ close_state = "InProgress"
    /\ driver_state = "exited"
    /\ driver_joined' = TRUE
    /\ UNCHANGED <<runtime_state, stop_requested, root_cancel_published,
                   group_alive, close_state, close_owner,
                   resources_alive, scheduler_alive, io_context_alive,
                   driver_state, driver_spawned, driver_exit_requested>>

DestroyGroupAction ==
    /\ close_state = "InProgress"
    /\ driver_joined
    /\ group_alive
    /\ group_alive' = FALSE
    /\ UNCHANGED <<runtime_state, stop_requested, root_cancel_published,
                   close_state, close_owner, driver_joined,
                   resources_alive, scheduler_alive, io_context_alive,
                   driver_state, driver_spawned, driver_exit_requested>>

Next ==
    \/ RequestStopPhase1
    \/ RequestStopPhase2Cancel
    \/ CloseOwnerDestroy
    \/ DriverExitAction
    \/ JoinDriverAction
    \/ DestroyGroupAction

Spec == Init /\ [][Next]_vars

(* This invariant SHOULD be violated: cancellation after Group destroy *)
NoCancelAfterGroupDestroy ==
    root_cancel_published => group_alive

=============================================================================
