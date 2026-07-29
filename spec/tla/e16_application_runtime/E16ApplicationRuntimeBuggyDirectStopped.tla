--------------------- MODULE E16ApplicationRuntimeBuggyDirectStopped ---------------------
(*
  NEG-E16-6 (E16-POST-MERGE-CORRECTIVE-1): DirectStoppedWithResources.

  This negative model reproduces the EXACT production defect that the C1
  corrective repairs: a direct-close path (Constructed shutdown OR startup
  abort) publishes

      runtime_state' = "Stopped"
      close_state'   = "Closed"

  WITHOUT destroying the Runtime-owned execution resources — i.e. it leaves
      group_alive'        = TRUE
      scheduler_alive'    = TRUE
      io_context_alive'   = TRUE
      resources_alive'    = TRUE

  The correct model funnels every Stopped publication through a unified close
  that destroys Group, Scheduler, and AsyncIoContext (sets all *_alive to
  FALSE) BEFORE publishing Stopped. This buggy model differs ONLY in that the
  direct-close steps omit the resource-destruction updates; it adds no other
  defect.

  Expected violation: Inv7NoPrematureStopped
      runtime_state = "Stopped" =>
          (close_state = "Closed" /\ ~resources_alive
           /\ driver_joined /\ outstanding_io = 0)

  because after the buggy direct close, runtime_state = "Stopped" while
  resources_alive = TRUE. This is the same property the correct model's
  Inv7NoPrematureStopped enforces, demonstrating that the model CAN produce a
  counterexample for this defect class (the negative model is not vacuous).
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Tasks, Callers, MaxIO, NONE, T0, T1, C0, C1

ASSUME
    /\ Tasks = {T0, T1}
    /\ Callers = {C0, C1}
    /\ MaxIO = 2
    /\ NONE \notin Tasks \cup Callers

VARIABLES
    runtime_state, stop_requested, startup_abort_requested,
    driver_spawned, driver_joined,
    group_alive, scheduler_alive, io_context_alive, resources_alive,
    close_state, outstanding_io

vars == <<runtime_state, stop_requested, startup_abort_requested,
          driver_spawned, driver_joined,
          group_alive, scheduler_alive, io_context_alive, resources_alive,
          close_state, outstanding_io>>

Init ==
    /\ runtime_state = "Constructed"
    /\ stop_requested = FALSE
    /\ startup_abort_requested = FALSE
    /\ driver_spawned = FALSE
    /\ driver_joined = FALSE
    /\ group_alive = TRUE
    /\ scheduler_alive = TRUE
    /\ io_context_alive = TRUE
    /\ resources_alive = TRUE
    /\ close_state = "Open"
    /\ outstanding_io = 0

(* RequestStopConstructed: remember stop before start (no state change). *)
RequestStopConstructed ==
    /\ runtime_state = "Constructed"
    /\ ~stop_requested
    /\ stop_requested' = TRUE
    /\ UNCHANGED <<runtime_state, startup_abort_requested,
                  driver_spawned, driver_joined,
                  group_alive, scheduler_alive, io_context_alive, resources_alive,
                  close_state, outstanding_io>>

(* BUGGY: shutdown from Constructed publishes Stopped/Closed WITHOUT destroying
   resources. This is the C1 defect on the merged master. The correct model
   would set group_alive'=FALSE, scheduler_alive'=FALSE, io_context_alive'=FALSE,
   resources_alive'=FALSE here before publishing Stopped. *)
BuggyShutdownConstructed ==
    /\ runtime_state = "Constructed"
    /\ close_state = "Open"
    /\ close_state' = "Closed"
    /\ runtime_state' = "Stopped"
    /\ driver_joined' = TRUE   \* no driver to join
    /\ UNCHANGED <<stop_requested, startup_abort_requested,
                  driver_spawned,
                  group_alive, scheduler_alive, io_context_alive, resources_alive,
                  outstanding_io>>

(* BUGGY: startup-abort close publishes Stopped/Closed WITHOUT destroying
   resources. stop_requested before commit drove the abort; the (already-spawned)
   driver is joined, then Stopped is published while resources stay alive. *)
BuggyStartupAbortClose ==
    /\ runtime_state = "Starting"
    /\ startup_abort_requested
    /\ driver_spawned
    /\ driver_joined
    /\ close_state = "Open"
    /\ close_state' = "Closed"
    /\ runtime_state' = "Stopped"
    /\ UNCHANGED <<stop_requested, startup_abort_requested,
                  driver_spawned,
                  group_alive, scheduler_alive, io_context_alive, resources_alive,
                  outstanding_io>>

(* Driver spawn (for the startup-abort path): spawns a driver then joins it. *)
SpawnAndJoin ==
    /\ runtime_state = "Starting"
    /\ ~driver_spawned
    /\ ~startup_abort_requested
    /\ driver_spawned' = TRUE
    /\ driver_joined' = TRUE
    /\ UNCHANGED <<runtime_state, stop_requested, startup_abort_requested,
                  group_alive, scheduler_alive, io_context_alive, resources_alive,
                  close_state, outstanding_io>>

(* Transition to Starting (begin) so the startup-abort path is reachable. *)
StartBegin ==
    /\ runtime_state = "Constructed"
    /\ ~stop_requested
    /\ runtime_state' = "Starting"
    /\ UNCHANGED <<stop_requested, startup_abort_requested,
                  driver_spawned, driver_joined,
                  group_alive, scheduler_alive, io_context_alive, resources_alive,
                  close_state, outstanding_io>>

(* Request stop while Starting (drives startup abort). *)
RequestStopStarting ==
    /\ runtime_state = "Starting"
    /\ ~startup_abort_requested
    /\ startup_abort_requested' = TRUE
    /\ stop_requested' = TRUE
    /\ UNCHANGED <<runtime_state,
                  driver_spawned, driver_joined,
                  group_alive, scheduler_alive, io_context_alive, resources_alive,
                  close_state, outstanding_io>>

Next ==
    \/ StartBegin
    \/ RequestStopConstructed
    \/ RequestStopStarting
    \/ SpawnAndJoin
    \/ BuggyShutdownConstructed
    \/ BuggyStartupAbortClose

Spec == Init /\ [][Next]_vars

(* Inv7NoPrematureStopped (mirrors the correct model's invariant). *)
Inv7NoPrematureStopped ==
    runtime_state = "Stopped" =>
    (close_state = "Closed"
     /\ ~resources_alive
     /\ driver_joined
     /\ outstanding_io = 0)

=============================================================================
