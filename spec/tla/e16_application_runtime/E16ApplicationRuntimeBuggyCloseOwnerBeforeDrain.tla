--------------------- MODULE E16ApplicationRuntimeBuggyCloseOwnerBeforeDrain ---------------------
(*
  NEG-E16-5: Close owner elected before drain complete.

  Defect: CloseOwnerElect omits the (drain_required => drain_complete)
  guard, allowing the close owner to be elected while the Runtime is in
  Draining state with drain_required=TRUE and drain_complete=FALSE.

  Expected violation: Inv25StoppedAfterDrain
*)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Tasks, Callers, MaxIO, E0, E1, NONE, T0, T1, C0, C1

ASSUME
    /\ Tasks = {T0, T1}
    /\ Callers = {C0, C1}
    /\ MaxIO \in 2..3
    /\ NONE \notin Tasks \cup Callers
    /\ E0 # E1

VARIABLES
    runtime_state, admission_open, stop_requested,
    root_cancel_published, startup_abort_requested,
    admitted_count, terminal_count,
    group_future_terminal_count, outstanding_io,
    runtime_task_io_open,
    drain_complete, drain_required, drain_completed_once,
    control_epoch, observed_epoch,
    runtime_cv_signal, scheduler_wake_signal,
    driver_state, driver_spawned, driver_joined,
    run_live_entered, driver_exit_requested,
    close_state, close_owner,
    resources_alive, group_alive, scheduler_alive,
    io_context_alive,
    successful_submit_published, admission_reservation_active,
    fatal_snapshot,
    task_admitted, task_committed, task_terminated,
    task_io_submitted, task_io_complete

Epochs == {E0, E1}
NextEpoch(e) == IF e = E0 THEN E1 ELSE E0

PublishedEpoch ==
    IF control_epoch = observed_epoch
    THEN NextEpoch(control_epoch)
    ELSE control_epoch

vars == <<runtime_state, admission_open, stop_requested,
          root_cancel_published, startup_abort_requested,
          admitted_count, terminal_count,
          group_future_terminal_count, outstanding_io,
          runtime_task_io_open,
          drain_complete, drain_required, drain_completed_once,
          control_epoch, observed_epoch,
          runtime_cv_signal, scheduler_wake_signal,
          driver_state, driver_spawned, driver_joined,
          run_live_entered, driver_exit_requested,
          close_state, close_owner,
          resources_alive, group_alive, scheduler_alive,
          io_context_alive,
          successful_submit_published, admission_reservation_active,
          fatal_snapshot,
          task_admitted, task_committed, task_terminated,
          task_io_submitted, task_io_complete>>

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
    /\ drain_complete = FALSE
    /\ drain_required = FALSE
    /\ drain_completed_once = FALSE
    /\ control_epoch = E0
    /\ observed_epoch = E0
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
    /\ task_committed = [t \in Tasks |-> FALSE]
    /\ task_terminated = [t \in Tasks |-> FALSE]
    /\ task_io_submitted = [t \in Tasks |-> FALSE]
    /\ task_io_complete = [t \in Tasks |-> FALSE]

(* Minimal lifecycle to reach Draining with drain_required and ~drain_complete *)
StartBegin ==
    /\ runtime_state = "Constructed"
    /\ ~stop_requested
    /\ runtime_state' = "Starting"
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

DriverSpawn ==
    /\ runtime_state = "Starting"
    /\ ~driver_spawned
    /\ ~startup_abort_requested
    /\ close_state = "Open"
    /\ driver_spawned' = TRUE
    /\ driver_state' = "barrier_wait"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

StartupCommit ==
    /\ runtime_state = "Starting"
    /\ driver_spawned
    /\ ~stop_requested
    /\ ~startup_abort_requested
    /\ runtime_state' = "Running"
    /\ admission_open' = TRUE
    /\ drain_required' = TRUE
    /\ control_epoch' = PublishedEpoch
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_completed_once,
                   observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

RequestStopRunning ==
    /\ runtime_state = "Running"
    /\ ~stop_requested
    /\ stop_requested' = TRUE
    /\ admission_open' = FALSE
    /\ root_cancel_published' = TRUE
    /\ runtime_state' = "Stopping"
    /\ control_epoch' = PublishedEpoch
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

DrainBegin ==
    /\ runtime_state = "Stopping"
    /\ control_epoch' = PublishedEpoch
    /\ runtime_state' = "Draining"
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   observed_epoch,
                   scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* BUGGY: CloseOwnerElect omits (drain_required => drain_complete) guard *)
CloseOwnerElectBuggy(c) ==
    /\ c \in Callers
    /\ runtime_state = "Draining"
    /\ close_state = "Open"
    /\ stop_requested
    /\ outstanding_io = 0
    /\ close_state' = "InProgress"
    /\ close_owner' = c
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* Minimal close path: exit, join, destroy, Stopped *)
RequestDriverExit ==
    /\ close_state = "InProgress"
    /\ ~driver_exit_requested
    /\ driver_exit_requested' = TRUE
    /\ control_epoch' = PublishedEpoch
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   observed_epoch,
                   scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

DriverExit ==
    /\ driver_state \in {"between_invocations", "drained_wait", "barrier_wait"}
    /\ driver_exit_requested
    /\ driver_state' = "exited"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

JoinDriver ==
    /\ close_state = "InProgress"
    /\ (driver_state = "exited" \/ ~driver_spawned)
    /\ ~driver_joined
    /\ driver_joined' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

DestroyGroup ==
    /\ close_state = "InProgress"
    /\ driver_joined
    /\ group_alive
    /\ group_alive' = FALSE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

DestroyScheduler ==
    /\ close_state = "InProgress"
    /\ ~group_alive
    /\ scheduler_alive
    /\ scheduler_alive' = FALSE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

DestroyIoContext ==
    /\ close_state = "InProgress"
    /\ ~group_alive
    /\ ~scheduler_alive
    /\ io_context_alive
    /\ io_context_alive' = FALSE
    /\ resources_alive' = FALSE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, 
                   group_alive, scheduler_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

PublishStopped ==
    /\ close_state = "InProgress"
    /\ ~group_alive
    /\ ~scheduler_alive
    /\ ~io_context_alive
    /\ ~resources_alive
    /\ driver_joined
    /\ runtime_state' = "Stopped"
    /\ close_state' = "Closed"
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, drain_required, drain_completed_once,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

Next ==
    \/ StartBegin
    \/ DriverSpawn
    \/ StartupCommit
    \/ RequestStopRunning
    \/ DrainBegin
    \/ \E c \in Callers : CloseOwnerElectBuggy(c)
    \/ RequestDriverExit
    \/ DriverExit
    \/ JoinDriver
    \/ DestroyGroup
    \/ DestroyScheduler
    \/ DestroyIoContext
    \/ PublishStopped

Spec == Init /\ [][Next]_vars

(* This invariant SHOULD be violated by the buggy model *)
Inv25StoppedAfterDrain ==
    (runtime_state = "Stopped" /\ drain_required) =>
    drain_completed_once

=============================================================================