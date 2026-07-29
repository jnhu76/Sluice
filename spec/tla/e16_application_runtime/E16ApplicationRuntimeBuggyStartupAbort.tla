--------------------- MODULE E16ApplicationRuntimeBuggyStartupAbort ---------------------
(*
  NEG-E16-3: Startup abort still enters run_live.

  Defect: stop wins before startup commit, startup barrier is released,
  but driver enters run_live anyway (missing abort check at barrier exit).

  Expected violation: StartupAbortNeverRuns
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
    runtime_state, stop_requested, startup_abort_requested,
    driver_state, driver_spawned, run_live_entered,
    admission_open, control_epoch

vars == <<runtime_state, stop_requested, startup_abort_requested,
          driver_state, driver_spawned, run_live_entered,
          admission_open, control_epoch>>

epoch_can_bump == control_epoch < MaxEpoch

Init ==
    /\ runtime_state = "Constructed"
    /\ stop_requested = FALSE
    /\ startup_abort_requested = FALSE
    /\ driver_state = "not_started"
    /\ driver_spawned = FALSE
    /\ run_live_entered = FALSE
    /\ admission_open = FALSE
    /\ control_epoch = 0

StartBegin ==
    /\ runtime_state = "Constructed"
    /\ ~stop_requested
    /\ runtime_state' = "Starting"
    /\ UNCHANGED <<stop_requested, startup_abort_requested,
                   driver_state, driver_spawned, run_live_entered,
                   admission_open, control_epoch>>

DriverSpawn ==
    /\ runtime_state = "Starting"
    /\ ~driver_spawned
    /\ driver_spawned' = TRUE
    /\ driver_state' = "barrier_wait"
    /\ UNCHANGED <<runtime_state, stop_requested, startup_abort_requested,
                   run_live_entered, admission_open, control_epoch>>

RequestStopStarting ==
    /\ runtime_state = "Starting"
    /\ ~stop_requested
    /\ stop_requested' = TRUE
    /\ startup_abort_requested' = TRUE
    /\ epoch_can_bump
    /\ control_epoch' = control_epoch + 1
    /\ UNCHANGED <<runtime_state, driver_state, driver_spawned,
                   run_live_entered, admission_open>>

(* BUGGY: driver enters run_live WITHOUT checking startup_abort_requested *)
DriverEnterRunLiveBuggy ==
    /\ driver_state = "barrier_wait"
    /\ driver_spawned
    /\ driver_state' = "in_run_live"
    /\ run_live_entered' = TRUE
    /\ UNCHANGED <<runtime_state, stop_requested, startup_abort_requested,
                   driver_spawned, admission_open, control_epoch>>

Next ==
    \/ StartBegin
    \/ DriverSpawn
    \/ RequestStopStarting
    \/ DriverEnterRunLiveBuggy

Spec == Init /\ [][Next]_vars

(* This invariant SHOULD be violated *)
StartupAbortNeverRuns ==
    (startup_abort_requested /\ runtime_state = "Starting") => ~run_live_entered

=============================================================================
