--------------------- MODULE E16ApplicationRuntimeBuggyCloseOwner ---------------------
(*
  NEG-E16-4: Two close owners.

  Defect: there is no Open->InProgress owner election, so each caller can
  independently drive the full close sequence (exit request -> driver exit ->
  join -> destroy). We track per-caller close progress in `closed[c]`, which
  becomes TRUE exactly when caller c completes its own destroy. The correct
  model has exactly one elected owner; here BOTH callers can become owners.

  Expected violation: AtMostOneCloseOwner (a real "two owners" violation,
  recorded only when two distinct callers each complete a destroy -- not the
  vacuous "system already closed" shape the previous single-close_owner
  invariant produced).
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Tasks, Callers, MaxIO, MaxEpoch, NONE, T0, T1, C0, C1

ASSUME
    /\ Tasks = {T0, T1}
    /\ Callers = {C0, C1}
    /\ MaxIO \in 2..3
    /\ MaxEpoch \in Nat \ {0}
    /\ NONE \notin Tasks \cup Callers

VARIABLES
    runtime_state, close_state,
    driver_joined, group_alive, resources_alive,
    driver_state, driver_exit_requested, drain_complete,
    exit_requested_by, joined, closed

vars == <<runtime_state, close_state,
          driver_joined, group_alive, resources_alive,
          driver_state, driver_exit_requested, drain_complete,
          exit_requested_by, joined, closed>>

Init ==
    /\ runtime_state = "Draining"
    /\ close_state = "Open"
    /\ driver_joined = FALSE
    /\ group_alive = TRUE
    /\ resources_alive = TRUE
    /\ driver_state = "drained_wait"
    /\ driver_exit_requested = FALSE
    /\ drain_complete = TRUE
    /\ exit_requested_by = NONE
    /\ joined = [c \in Callers |-> FALSE]
    /\ closed = [c \in Callers |-> FALSE]

(* BUGGY: no owner election; any caller may request the driver exit. *)
CloseWithoutElection(c) ==
    /\ c \in Callers
    /\ runtime_state = "Draining"
    /\ drain_complete
    /\ ~driver_exit_requested
    /\ driver_exit_requested' = TRUE
    /\ exit_requested_by' = c
    /\ UNCHANGED <<runtime_state, close_state,
                   driver_joined, group_alive, resources_alive,
                   driver_state, drain_complete, joined, closed>>

DriverExitAction ==
    /\ driver_exit_requested
    /\ driver_state = "drained_wait"
    /\ driver_state' = "exited"
    /\ UNCHANGED <<runtime_state, close_state,
                   driver_joined, group_alive, resources_alive,
                   driver_exit_requested, drain_complete,
                   exit_requested_by, joined, closed>>

(* BUGGY: no owner gate; each caller independently joins the exited driver
   (per-caller join flag), so more than one caller can pass this step. *)
JoinByCaller(c) ==
    /\ c \in Callers
    /\ driver_state = "exited"
    /\ ~joined[c]
    /\ joined' = [joined EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<runtime_state, close_state,
                   driver_joined, group_alive, resources_alive,
                   driver_state, driver_exit_requested, drain_complete,
                   exit_requested_by, closed>>

(* BUGGY: no owner gate; any caller that has joined may complete its OWN
   destroy (per-caller closed[c]' = TRUE). The correct model would let
   exactly one elected owner reach here; this buggy model lets every joined
   caller reach it, so two callers can each become a close owner. *)
DestroyByCaller(c) ==
    /\ c \in Callers
    /\ joined[c]
    /\ ~closed[c]
    /\ group_alive' = FALSE
    /\ resources_alive' = FALSE
    /\ runtime_state' = "Stopped"
    /\ close_state' = "Closed"
    /\ closed' = [closed EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<driver_joined, driver_state, driver_exit_requested,
                   drain_complete, exit_requested_by, joined>>

Next ==
    \/ \E c \in Callers : CloseWithoutElection(c)
    \/ DriverExitAction
    \/ \E c \in Callers : JoinByCaller(c)
    \/ \E c \in Callers : DestroyByCaller(c)

Spec == Init /\ [][Next]_vars

(* This invariant SHOULD be violated by the buggy model: it forbids two
   distinct callers from each completing a close destroy. It is satisfied by
   any single-owner close and by states where no caller has closed yet, so a
   violation proves genuinely that BOTH callers became owners -- exactly the
   "two close owners" defect -- rather than merely that the system reached a
   closed state. *)
AtMostOneCloseOwner ==
    Cardinality({c \in Callers : closed[c]}) <= 1

=============================================================================
