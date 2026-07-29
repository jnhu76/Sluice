--------------------- MODULE E16ApplicationRuntimeBuggyBoundaryWake ---------------------
(*
  NEG-E16-1: Invocation-boundary lost wake.

  Defect: successful submit publishes a runnable task but OMITS
  control_epoch++ and Runtime CV notification while driver is parked
  between run_live invocations.

  Expected violation: NoStrandedSuccessfulAdmission
  (an admitted task exists, driver remains between_invocations,
   observed_epoch equals control_epoch, no future wake obligation)
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
    runtime_state, admission_open, stop_requested, root_cancel_published,
    startup_abort_requested, admitted_count, terminal_count,
    group_future_terminal_count, outstanding_io, runtime_task_io_open,
    task_set_terminal_snapshot, drain_complete, control_epoch,
    observed_epoch, runtime_cv_signal, scheduler_wake_signal,
    driver_state, driver_spawned, driver_joined, run_live_entered,
    driver_exit_requested, close_state, close_owner, resources_alive,
    group_alive, scheduler_alive, io_context_alive,
    successful_submit_published, admission_reservation_active,
    fatal_snapshot, task_admitted, task_terminated,
    task_io_submitted, task_io_complete

vars == <<runtime_state, admission_open, stop_requested, root_cancel_published,
          startup_abort_requested, admitted_count, terminal_count,
          group_future_terminal_count, outstanding_io, runtime_task_io_open,
          task_set_terminal_snapshot, drain_complete, control_epoch,
          observed_epoch, runtime_cv_signal, scheduler_wake_signal,
          driver_state, driver_spawned, driver_joined, run_live_entered,
          driver_exit_requested, close_state, close_owner, resources_alive,
          group_alive, scheduler_alive, io_context_alive,
          successful_submit_published, admission_reservation_active,
          fatal_snapshot, task_admitted, task_terminated,
          task_io_submitted, task_io_complete>>

epoch_can_bump == control_epoch < MaxEpoch

Init ==
    /\ runtime_state = "Constructed"
    /\ admission_open = FALSE
    /\ stop_requested = FALSE
    /\ root_cancel_published = FALSE
    /\ startup_abort_requested = FALSE
    /\ admitted_count = 0
    /\ terminal_count = 0
    /\ group_future_terminal_count = 0
    /\ outstanding_io = 0
    /\ runtime_task_io_open = FALSE
    /\ task_set_terminal_snapshot = TRUE
    /\ drain_complete = FALSE
    /\ control_epoch = 0
    /\ observed_epoch = 0
    /\ runtime_cv_signal = FALSE
    /\ scheduler_wake_signal = FALSE
    /\ driver_state = "not_started"
    /\ driver_spawned = FALSE
    /\ driver_joined = FALSE
    /\ run_live_entered = FALSE
    /\ driver_exit_requested = FALSE
    /\ close_state = "Open"
    /\ close_owner = NONE
    /\ resources_alive = TRUE
    /\ group_alive = TRUE
    /\ scheduler_alive = TRUE
    /\ io_context_alive = TRUE
    /\ successful_submit_published = FALSE
    /\ admission_reservation_active = FALSE
    /\ fatal_snapshot = FALSE
    /\ task_admitted = [t \in Tasks |-> FALSE]
    /\ task_terminated = [t \in Tasks |-> FALSE]
    /\ task_io_submitted = [t \in Tasks |-> FALSE]
    /\ task_io_complete = [t \in Tasks |-> FALSE]

(* Simplified lifecycle for negative model focus *)
StartBegin ==
    /\ runtime_state = "Constructed"
    /\ runtime_state' = "Starting"
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open, task_set_terminal_snapshot,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

DriverSpawn ==
    /\ runtime_state = "Starting"
    /\ ~driver_spawned
    /\ driver_spawned' = TRUE
    /\ driver_state' = "barrier_wait"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open, task_set_terminal_snapshot,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

StartupCommit ==
    /\ runtime_state = "Starting"
    /\ driver_spawned
    /\ ~stop_requested
    /\ runtime_state' = "Running"
    /\ admission_open' = TRUE
    /\ task_set_terminal_snapshot' = FALSE
    /\ epoch_can_bump
    /\ control_epoch' = control_epoch + 1
    /\ UNCHANGED <<stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open, drain_complete, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

DriverEnterRunLive ==
    /\ driver_state = "barrier_wait"
    /\ runtime_state = "Running"
    /\ driver_state' = "in_run_live"
    /\ run_live_entered' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open, task_set_terminal_snapshot,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

DriverRunLiveReturn ==
    /\ driver_state = "in_run_live"
    /\ driver_state' = "between_invocations"
    /\ observed_epoch' = control_epoch
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open, task_set_terminal_snapshot,
                   drain_complete, control_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

(* BUGGY: SubmitGroupCommit OMITS control_epoch++ and CV notification *)
SubmitReserve(t) ==
    /\ runtime_state = "Running"
    /\ admission_open
    /\ ~task_admitted[t]
    /\ ~admission_reservation_active
    /\ admission_reservation_active' = TRUE
    /\ admitted_count' = admitted_count + 1
    /\ task_admitted' = [task_admitted EXCEPT ![t] = TRUE]
    /\ task_set_terminal_snapshot' = FALSE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   terminal_count, group_future_terminal_count,
                   outstanding_io, runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, fatal_snapshot,
                   task_terminated, task_io_submitted, task_io_complete>>

(* BUG: no control_epoch++, no runtime_cv_signal *)
SubmitGroupCommitBuggy(t) ==
    /\ admission_reservation_active
    /\ task_admitted[t]
    /\ admission_reservation_active' = FALSE
    /\ successful_submit_published' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open, task_set_terminal_snapshot,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   fatal_snapshot, task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

(* Driver re-entry requires epoch change (correct), but buggy submit
   never advances epoch, so driver stays parked *)
DriverReenterRunLive ==
    /\ driver_state = "between_invocations"
    /\ control_epoch # observed_epoch
    /\ driver_state' = "in_run_live"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open, task_set_terminal_snapshot,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

(* Driver waits at boundary (models time passing with no wake) *)
DriverWaitAtBoundary ==
    /\ driver_state = "between_invocations"
    /\ control_epoch = observed_epoch
    /\ UNCHANGED vars

Next ==
    \/ StartBegin
    \/ DriverSpawn
    \/ StartupCommit
    \/ DriverEnterRunLive
    \/ DriverRunLiveReturn
    \/ \E t \in Tasks : SubmitReserve(t)
    \/ \E t \in Tasks : SubmitGroupCommitBuggy(t)
    \/ DriverReenterRunLive
    \/ DriverWaitAtBoundary

Spec == Init /\ [][Next]_vars

(* This property SHOULD be violated by the buggy model *)
NoStrandedSuccessfulAdmission ==
    [](successful_submit_published
       /\ driver_state = "between_invocations"
       /\ \E t \in Tasks : task_admitted[t] /\ ~task_terminated[t]
       => <>(driver_state = "in_run_live" \/ control_epoch # observed_epoch))

=============================================================================
