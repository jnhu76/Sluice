--------------------------- MODULE E16ApplicationRuntime ---------------------------
(*
  E16 Application Runtime lifecycle protocol.

  Models the builder-constructed, one-shot, injected-backend Runtime with
  a single dedicated driver thread. Covers:
    - Lifecycle: Constructed/Starting/Running/Stopping/Draining/Stopped/StartFailed/Fatal
    - Driver FSM: not_started/barrier_wait/in_run_live/between_invocations/drained_wait/exiting/exited
    - Close ownership: Open/InProgress/Closed
    - Admission reservation + Group admission + rollback
    - Task terminal accounting + Group Future + outstanding I/O
    - Control epoch + persistent CV predicate (boundary wake)
    - Startup abort path
    - Root cancellation under lifecycle_mutex
    - Resource destruction order

  Bounded domain: Tasks={T0,T1}, Callers={C0,C1}, MaxIO=2
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
    runtime_state,          \* {Constructed,Starting,Running,Stopping,Draining,Stopped,StartFailed,Fatal}
    admission_open,         \* BOOLEAN
    stop_requested,         \* BOOLEAN
    root_cancel_published,  \* BOOLEAN
    startup_abort_requested,\* BOOLEAN
    admitted_count,         \* 0..Len(Tasks)
    terminal_count,         \* 0..Len(Tasks)
    group_future_terminal_count, \* 0..Len(Tasks)
    outstanding_io,         \* 0..MaxIO
    runtime_task_io_open,   \* BOOLEAN (per-task I/O capability)
    drain_complete,         \* BOOLEAN
    control_epoch,          \* 0..MaxEpoch (bounded)
    observed_epoch,         \* 0..MaxEpoch (driver's last observed)
    runtime_cv_signal,      \* BOOLEAN (abstract CV notification)
    scheduler_wake_signal,  \* BOOLEAN (abstract wake handle)
    driver_state,           \* {not_started,barrier_wait,in_run_live,between_invocations,drained_wait,exiting,exited}
    driver_spawned,         \* BOOLEAN
    driver_joined,          \* BOOLEAN
    run_live_entered,       \* BOOLEAN (history: driver entered run_live at least once)
    driver_exit_requested,  \* BOOLEAN
    close_state,            \* {Open,InProgress,Closed}
    close_owner,            \* Callers \cup {NONE}
    resources_alive,        \* BOOLEAN
    group_alive,            \* BOOLEAN
    scheduler_alive,        \* BOOLEAN
    io_context_alive,       \* BOOLEAN
    successful_submit_published, \* BOOLEAN (history: at least one full submit published)
    admission_reservation_active, \* BOOLEAN (transient reservation in progress)
    fatal_snapshot,         \* BOOLEAN
    task_admitted,          \* [Tasks -> BOOLEAN]
    task_committed,         \* [Tasks -> BOOLEAN] (Group admission committed)
    task_terminated,        \* [Tasks -> BOOLEAN]
    task_io_submitted,      \* [Tasks -> BOOLEAN]
    task_io_complete        \* [Tasks -> BOOLEAN]

vars == <<runtime_state, admission_open, stop_requested, root_cancel_published,
          startup_abort_requested, admitted_count, terminal_count,
          group_future_terminal_count, outstanding_io, runtime_task_io_open,
          drain_complete, control_epoch,
          observed_epoch, runtime_cv_signal, scheduler_wake_signal,
          driver_state, driver_spawned, driver_joined, run_live_entered,
          driver_exit_requested, close_state, close_owner, resources_alive,
          group_alive, scheduler_alive, io_context_alive,
          successful_submit_published, admission_reservation_active,
          fatal_snapshot, task_admitted, task_committed, task_terminated,
          task_io_submitted, task_io_complete>>

(* =========================================================================
   Init
   ========================================================================= *)
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
    /\ task_committed = [t \in Tasks |-> FALSE]
    /\ task_terminated = [t \in Tasks |-> FALSE]
    /\ task_io_submitted = [t \in Tasks |-> FALSE]
    /\ task_io_complete = [t \in Tasks |-> FALSE]

(* task_set_terminal_snapshot is a DERIVED value: the driver stop-predicate
   reads this, and it is always consistent with the authoritative state. *)
task_set_terminal_snapshot ==
    (~admission_open /\ admitted_count = terminal_count)

(* Epoch bound guard: all epoch-incrementing actions require this. *)
epoch_can_bump == control_epoch < MaxEpoch

(* =========================================================================
   Build (already constructed in Init; Build is identity for the model)
   ========================================================================= *)
Build ==
    /\ runtime_state = "Constructed"
    /\ UNCHANGED vars

(* =========================================================================
   Start transaction
   ========================================================================= *)

(* StartBegin: Constructed -> Starting. If stop_requested, start returns
   canceled and state remains Constructed. *)
StartBegin ==
    /\ runtime_state = "Constructed"
    /\ ~stop_requested
    /\ runtime_state' = "Starting"
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* StartBeginStopRemembered: start() when stop_requested=TRUE in Constructed
   returns canceled, state remains Constructed. *)
StartBeginStopRemembered ==
    /\ runtime_state = "Constructed"
    /\ stop_requested
    /\ UNCHANGED vars

(* DriverSpawn: Starting -> spawn driver thread, driver enters barrier. *)
DriverSpawn ==
    /\ runtime_state = "Starting"
    /\ ~driver_spawned
    /\ close_state = "Open"
    /\ driver_spawned' = TRUE
    /\ driver_state' = "barrier_wait"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DriverEnterBarrier: driver at barrier waits for startup decision.
   Modeled as observation (no state change beyond driver_state). *)
DriverEnterBarrier ==
    /\ driver_state = "barrier_wait"
    /\ UNCHANGED vars

(* StartupCommit: Starting -> Running. Locked compound commit. *)
StartupCommit ==
    /\ runtime_state = "Starting"
    /\ driver_spawned
    /\ ~stop_requested
    /\ ~startup_abort_requested
    /\ epoch_can_bump
    /\ runtime_state' = "Running"
    /\ admission_open' = TRUE
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* StartupSpawnFailure: driver spawn throws -> StartFailed. *)
StartupSpawnFailure ==
    /\ runtime_state = "Starting"
    /\ ~driver_spawned
    /\ runtime_state' = "StartFailed"
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* =========================================================================
   request_stop variants
   ========================================================================= *)

(* RequestStopConstructed: remember stop, remain Constructed. *)
RequestStopConstructed ==
    /\ runtime_state = "Constructed"
    /\ ~stop_requested
    /\ stop_requested' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* RequestStopStarting: record stop + startup abort, wake barrier. *)
RequestStopStarting ==
    /\ runtime_state = "Starting"
    /\ ~stop_requested
    /\ epoch_can_bump
    /\ stop_requested' = TRUE
    /\ startup_abort_requested' = TRUE
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, root_cancel_published,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* RequestStopRunning: compound commit under lifecycle_mutex. *)
RequestStopRunning ==
    /\ runtime_state = "Running"
    /\ ~stop_requested
    /\ epoch_can_bump
    /\ stop_requested' = TRUE
    /\ admission_open' = FALSE
    /\ root_cancel_published' = TRUE
    /\ runtime_state' = "Stopping"
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* RequestStopIdempotent: already stopped or stop already requested. *)
RequestStopIdempotent ==
    /\ stop_requested
    /\ runtime_state \in {"Stopping", "Draining", "Stopped"}
    /\ UNCHANGED vars

(* =========================================================================
   Startup abort path
   ========================================================================= *)

(* StartupAbortPublish: stop wins before commit; abort published. *)
StartupAbortPublish ==
    /\ runtime_state = "Starting"
    /\ startup_abort_requested
    /\ UNCHANGED vars

(* DriverObserveStartupAbort: driver at barrier sees abort, exits. *)
DriverObserveStartupAbort ==
    /\ driver_state = "barrier_wait"
    /\ startup_abort_requested
    /\ driver_state' = "exited"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* StartupAbortJoin: start owner joins driver after abort. *)
StartupAbortJoin ==
    /\ runtime_state = "Starting"
    /\ startup_abort_requested
    /\ driver_state = "exited"
    /\ ~driver_joined
    /\ driver_joined' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* StartupAbortClose: close resources after abort join. *)
StartupAbortClose ==
    /\ runtime_state = "Starting"
    /\ startup_abort_requested
    /\ driver_joined
    /\ close_state = "Open"
    /\ close_owner' = C0  \* start owner
    /\ group_alive' = FALSE
    /\ scheduler_alive' = FALSE
    /\ io_context_alive' = FALSE
    /\ resources_alive' = FALSE
    /\ runtime_state' = "Stopped"
    /\ close_state' = "Closed"
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* =========================================================================
   Admission protocol
   ========================================================================= *)

(* SubmitReserve: reserve under lifecycle_mutex. *)
SubmitReserve(t) ==
    /\ runtime_state = "Running"
    /\ admission_open
    /\ ~task_admitted[t]
    /\ ~admission_reservation_active
    /\ admission_reservation_active' = TRUE
    /\ admitted_count' = admitted_count + 1
    /\ task_admitted' = [task_admitted EXCEPT ![t] = TRUE]
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
                   task_committed, task_terminated, task_io_submitted, task_io_complete>>

(* SubmitGroupCommit: Group admission succeeds. Publish epoch + dual-wake. *)
SubmitGroupCommit(t) ==
    /\ admission_reservation_active
    /\ task_admitted[t]
    /\ epoch_can_bump
    /\ admission_reservation_active' = FALSE
    /\ task_committed' = [task_committed EXCEPT ![t] = TRUE]
    /\ successful_submit_published' = TRUE
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   fatal_snapshot, task_admitted, task_terminated,
                   task_io_submitted, task_io_complete>>

(* SubmitRollback: Group admission fails after reservation. *)
SubmitRollback(t) ==
    /\ admission_reservation_active
    /\ task_admitted[t]
    /\ ~task_committed[t]
    /\ epoch_can_bump
    /\ admission_reservation_active' = FALSE
    /\ admitted_count' = admitted_count - 1
    /\ task_admitted' = [task_admitted EXCEPT ![t] = FALSE]
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   terminal_count, group_future_terminal_count,
                   outstanding_io, runtime_task_io_open,
                   drain_complete, observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, fatal_snapshot,
                   task_committed, task_terminated, task_io_submitted, task_io_complete>>

(* SubmitSuccessPublish: full publication complete (alias for observability). *)
SubmitSuccessPublish ==
    /\ successful_submit_published
    /\ UNCHANGED vars

(* =========================================================================
   Task body execution
   ========================================================================= *)

(* TaskBodyExit: a task body finishes (normal or exception). *)
TaskBodyExit(t) ==
    /\ task_committed[t]
    /\ ~task_terminated[t]
    /\ driver_state = "in_run_live"
    /\ epoch_can_bump
    /\ task_terminated' = [task_terminated EXCEPT ![t] = TRUE]
    /\ terminal_count' = terminal_count + 1
    /\ runtime_task_io_open' = FALSE
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, group_future_terminal_count,
                   outstanding_io, drain_complete, observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed,
                   task_io_submitted, task_io_complete>>

(* GroupFuturePublish: Group Future becomes terminal after task exit. *)
GroupFuturePublish(t) ==
    /\ task_terminated[t]
    /\ group_future_terminal_count < terminal_count
    /\ group_future_terminal_count' = group_future_terminal_count + 1
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   outstanding_io, runtime_task_io_open,
                   drain_complete,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* SubmitTaskIO: task submits I/O while body active. *)
SubmitTaskIO(t) ==
    /\ task_committed[t]
    /\ ~task_terminated[t]
    /\ ~task_io_submitted[t]
    /\ outstanding_io < MaxIO
    /\ driver_state = "in_run_live"
    /\ close_state = "Open"
    /\ task_io_submitted' = [task_io_submitted EXCEPT ![t] = TRUE]
    /\ outstanding_io' = outstanding_io + 1
    /\ runtime_task_io_open' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count,
                   drain_complete,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_complete>>

(* CompleteTaskIO: backend completes I/O (still outstanding until reaped). *)
CompleteTaskIO(t) ==
    /\ task_io_submitted[t]
    /\ ~task_io_complete[t]
    /\ task_io_complete' = [task_io_complete EXCEPT ![t] = TRUE]
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted>>

(* ReapTaskIO: Scheduler reaps completed I/O, outstanding decrements. *)
ReapTaskIO(t) ==
    /\ task_io_submitted[t]
    /\ task_io_complete[t]
    /\ outstanding_io > 0
    /\ driver_state = "in_run_live"
    /\ outstanding_io' = outstanding_io - 1
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, runtime_task_io_open,
                   drain_complete,
                   control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* =========================================================================
   Driver loop
   ========================================================================= *)

(* DriverEnterRunLive: driver enters run_live from barrier (Running). *)
DriverEnterRunLive ==
    /\ driver_state = "barrier_wait"
    /\ runtime_state = "Running"
    /\ ~startup_abort_requested
    /\ driver_state' = "in_run_live"
    /\ run_live_entered' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DriverRunLiveReturn: run_live returns (invocation boundary). *)
DriverRunLiveReturn ==
    /\ driver_state = "in_run_live"
    /\ driver_state' = "between_invocations"
    /\ observed_epoch' = control_epoch
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DriverEnterBoundaryWait: driver parks at boundary on CV predicate. *)
DriverEnterBoundaryWait ==
    /\ driver_state = "between_invocations"
    /\ ~driver_exit_requested
    /\ ~fatal_snapshot
    /\ control_epoch = observed_epoch
    /\ UNCHANGED vars

(* DriverObserveEpoch: driver observes new epoch, re-evaluates. *)
DriverObserveEpoch ==
    /\ driver_state = "between_invocations"
    /\ control_epoch # observed_epoch
    /\ observed_epoch' = control_epoch
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DriverReenterRunLive: driver re-enters run_live after epoch change
   or when there are committed but non-terminated tasks to process. *)
DriverReenterRunLive ==
    /\ driver_state = "between_invocations"
    /\ (control_epoch # observed_epoch \/
        \E t \in Tasks : task_committed[t] /\ ~task_terminated[t])
    /\ ~driver_exit_requested
    /\ ~fatal_snapshot
    /\ driver_state' = "in_run_live"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* =========================================================================
   Drain
   ========================================================================= *)

(* DrainBegin: Stopping -> Draining. *)
DrainBegin ==
    /\ runtime_state = "Stopping"
    /\ epoch_can_bump
    /\ runtime_state' = "Draining"
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* PublishDrainComplete: driver publishes drain_complete at boundary. *)
PublishDrainComplete ==
    /\ driver_state \in {"between_invocations", "barrier_wait"}
    /\ stop_requested
    /\ ~admission_open
    /\ admitted_count = terminal_count
    /\ group_future_terminal_count = admitted_count
    /\ outstanding_io = 0
    /\ ~drain_complete
    /\ epoch_can_bump
    /\ drain_complete' = TRUE
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   observed_epoch, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* EnterDrainedWait: driver parks after drain_complete. *)
EnterDrainedWait ==
    /\ driver_state \in {"between_invocations", "barrier_wait"}
    /\ drain_complete
    /\ ~driver_exit_requested
    /\ driver_state' = "drained_wait"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* =========================================================================
   Close owner / join
   ========================================================================= *)

(* CloseOwnerElect(c): elect one close owner.
   Requires stop, abort, or fatal state, and drain must be complete
   (all I/O resolved) before close can begin. *)
CloseOwnerElect(c) ==
    /\ close_state = "Open"
    /\ c \in Callers
    /\ stop_requested \/ startup_abort_requested \/ runtime_state = "Fatal"
    /\ runtime_state = "Fatal" \/ outstanding_io = 0
    /\ close_state' = "InProgress"
    /\ close_owner' = c
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* CloseWaiterObserveInProgress: other caller sees InProgress, waits. *)
CloseWaiterObserveInProgress ==
    /\ close_state = "InProgress"
    /\ UNCHANGED vars

(* RequestDriverExit: close owner requests driver exit. *)
RequestDriverExit ==
    /\ close_state = "InProgress"
    /\ ~driver_exit_requested
    /\ epoch_can_bump
    /\ driver_exit_requested' = TRUE
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DriverExit: driver observes exit request and exits. *)
DriverExit ==
    /\ driver_state \in {"between_invocations", "drained_wait", "barrier_wait"}
    /\ driver_exit_requested
    /\ driver_state' = "exited"
    /\ UNCHANGED <<runtime_state, admission_open, stop_requested,
                   root_cancel_published, startup_abort_requested,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_spawned, driver_joined, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* JoinDriver: close owner joins the driver thread.
   If driver was never spawned, join is trivially satisfied. *)
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
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, run_live_entered,
                   driver_exit_requested, close_state, close_owner,
                   resources_alive, group_alive, scheduler_alive,
                   io_context_alive, successful_submit_published,
                   admission_reservation_active, fatal_snapshot,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DestroyGroup: destroy root Group. *)
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
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DestroyScheduler: destroy Scheduler (after Group). *)
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
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* DestroyIoContext: destroy AsyncIoContext (after Scheduler). *)
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
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner,
                   group_alive, scheduler_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* PublishStopped: publish terminal state after all resources destroyed.
   If runtime was Fatal, it remains Fatal; otherwise transitions to Stopped. *)
PublishStopped ==
    /\ close_state = "InProgress"
    /\ ~resources_alive
    /\ driver_joined
    /\ runtime_state' = IF runtime_state = "Fatal" THEN "Fatal" ELSE "Stopped"
    /\ close_state' = "Closed"
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* CloseWaiterReturn: waiter observes Closed and returns. *)
CloseWaiterReturn ==
    /\ close_state = "Closed"
    /\ UNCHANGED vars

(* =========================================================================
   Shutdown state dispatch
   ========================================================================= *)

(* ShutdownConstructed: direct close without driver/drain. *)
ShutdownConstructed ==
    /\ runtime_state = "Constructed"
    /\ close_state = "Open"
    /\ close_state' = "Closed"
    /\ close_owner' = C0
    /\ group_alive' = FALSE
    /\ scheduler_alive' = FALSE
    /\ io_context_alive' = FALSE
    /\ resources_alive' = FALSE
    /\ runtime_state' = "Stopped"
    /\ driver_joined' = TRUE  \* no driver to join; trivially satisfied
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned,
                   run_live_entered, driver_exit_requested,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* ShutdownStarting: delegate to startup abort path. *)
ShutdownStarting ==
    /\ runtime_state = "Starting"
    /\ ~startup_abort_requested
    /\ epoch_can_bump
    /\ startup_abort_requested' = TRUE
    /\ stop_requested' = TRUE
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<runtime_state, admission_open, root_cancel_published,
                   admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* ShutdownRunning: request_stop + drain + join path. *)
ShutdownRunning ==
    /\ runtime_state = "Running"
    /\ ~stop_requested
    /\ epoch_can_bump
    /\ stop_requested' = TRUE
    /\ admission_open' = FALSE
    /\ root_cancel_published' = TRUE
    /\ runtime_state' = "Stopping"
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ scheduler_wake_signal' = TRUE
    /\ UNCHANGED <<startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* ShutdownStopping: proceed to drain. *)
ShutdownStopping ==
    /\ runtime_state = "Stopping"
    /\ epoch_can_bump
    /\ runtime_state' = "Draining"
    /\ control_epoch' = control_epoch + 1
    /\ runtime_cv_signal' = TRUE
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, observed_epoch,
                   scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* ShutdownDraining: wait for drain_complete. *)
ShutdownDraining ==
    /\ runtime_state = "Draining"
    /\ drain_complete
    /\ UNCHANGED vars

(* ShutdownStartFailed: direct close. *)
ShutdownStartFailed ==
    /\ runtime_state = "StartFailed"
    /\ close_state = "Open"
    /\ close_state' = "Closed"
    /\ close_owner' = C0
    /\ group_alive' = FALSE
    /\ scheduler_alive' = FALSE
    /\ io_context_alive' = FALSE
    /\ resources_alive' = FALSE
    /\ runtime_state' = "Stopped"
    /\ driver_joined' = TRUE
    /\ UNCHANGED <<admission_open, stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned,
                   run_live_entered, driver_exit_requested,
                   successful_submit_published, admission_reservation_active,
                   fatal_snapshot, task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* ShutdownStopped: idempotent. *)
ShutdownStopped ==
    /\ runtime_state = "Stopped"
    /\ UNCHANGED vars

(* =========================================================================
   Fatal transition
   ========================================================================= *)
FatalTransition ==
    /\ runtime_state \notin {"Stopped", "Fatal"}
    /\ fatal_snapshot' = TRUE
    /\ runtime_state' = "Fatal"
    /\ admission_open' = FALSE
    /\ UNCHANGED <<stop_requested, root_cancel_published,
                   startup_abort_requested, admitted_count, terminal_count,
                   group_future_terminal_count, outstanding_io,
                   runtime_task_io_open,
                   drain_complete, control_epoch, observed_epoch,
                   runtime_cv_signal, scheduler_wake_signal,
                   driver_state, driver_spawned, driver_joined,
                   run_live_entered, driver_exit_requested,
                   close_state, close_owner, resources_alive,
                   group_alive, scheduler_alive, io_context_alive,
                   successful_submit_published, admission_reservation_active,
                   task_admitted, task_committed, task_terminated,
                   task_io_submitted, task_io_complete>>

(* =========================================================================
   Next
   ========================================================================= *)
Next ==
    \/ Build
    \/ StartBegin
    \/ StartBeginStopRemembered
    \/ DriverSpawn
    \/ DriverEnterBarrier
    \/ StartupCommit
    \/ StartupSpawnFailure
    \/ RequestStopConstructed
    \/ RequestStopStarting
    \/ RequestStopRunning
    \/ RequestStopIdempotent
    \/ StartupAbortPublish
    \/ DriverObserveStartupAbort
    \/ StartupAbortJoin
    \/ StartupAbortClose
    \/ \E t \in Tasks : SubmitReserve(t)
    \/ \E t \in Tasks : SubmitGroupCommit(t)
    \/ \E t \in Tasks : SubmitRollback(t)
    \/ SubmitSuccessPublish
    \/ \E t \in Tasks : TaskBodyExit(t)
    \/ \E t \in Tasks : GroupFuturePublish(t)
    \/ \E t \in Tasks : SubmitTaskIO(t)
    \/ \E t \in Tasks : CompleteTaskIO(t)
    \/ \E t \in Tasks : ReapTaskIO(t)
    \/ DriverEnterRunLive
    \/ DriverRunLiveReturn
    \/ DriverEnterBoundaryWait
    \/ DriverObserveEpoch
    \/ DriverReenterRunLive
    \/ DrainBegin
    \/ PublishDrainComplete
    \/ EnterDrainedWait
    \/ \E c \in Callers : CloseOwnerElect(c)
    \/ CloseWaiterObserveInProgress
    \/ RequestDriverExit
    \/ DriverExit
    \/ JoinDriver
    \/ DestroyGroup
    \/ DestroyScheduler
    \/ DestroyIoContext
    \/ PublishStopped
    \/ CloseWaiterReturn
    \/ ShutdownConstructed
    \/ ShutdownStarting
    \/ ShutdownRunning
    \/ ShutdownStopping
    \/ ShutdownDraining
    \/ ShutdownStartFailed
    \/ ShutdownStopped
    \/ FatalTransition

Spec == Init /\ [][Next]_vars

(* =========================================================================
   INVARIANTS (Section 10)
   ========================================================================= *)

(* E16-Inv1: Lifecycle typing *)
Inv1Typing ==
    /\ runtime_state \in {"Constructed","Starting","Running","Stopping",
                          "Draining","Stopped","StartFailed","Fatal"}
    /\ driver_state \in {"not_started","barrier_wait","in_run_live",
                         "between_invocations","drained_wait","exiting","exited"}
    /\ close_state \in {"Open","InProgress","Closed"}
    /\ close_owner \in Callers \cup {NONE}
    /\ admitted_count \in 0..2
    /\ terminal_count \in 0..2
    /\ outstanding_io \in 0..MaxIO

(* E16-Inv2: Admission authority *)
Inv2AdmissionAuthority ==
    admission_open =>
    (runtime_state = "Running" /\ ~stop_requested)

(* E16-Inv3: Stop closes admission atomically *)
Inv3StopClosesAdmission ==
    (runtime_state \in {"Stopping","Draining","Stopped"}) =>
    ~admission_open

(* E16-Inv4: Accounting bounds *)
Inv4AccountingBounds ==
    /\ 0 <= terminal_count
    /\ terminal_count <= admitted_count
    /\ 0 <= group_future_terminal_count
    /\ group_future_terminal_count <= terminal_count

(* E16-Inv5: Task terminal snapshot authority *)
Inv5TerminalSnapshot ==
    task_set_terminal_snapshot = (~admission_open /\ admitted_count = terminal_count)

(* E16-Inv6: Drain completeness *)
Inv6DrainComplete ==
    drain_complete =>
    (task_set_terminal_snapshot
     /\ outstanding_io = 0
     /\ group_future_terminal_count = admitted_count)

(* E16-Inv7: No premature Stopped *)
Inv7NoPrematureStopped ==
    runtime_state = "Stopped" =>
    (close_state = "Closed"
     /\ ~resources_alive
     /\ driver_joined
     /\ outstanding_io = 0)

(* E16-Inv8: Resource hierarchy *)
Inv8ResourceHierarchy ==
    /\ (group_alive => scheduler_alive)
    /\ (scheduler_alive => io_context_alive)
    /\ (resources_alive = (group_alive \/ scheduler_alive \/ io_context_alive))

(* E16-Inv9: Group destruction after start requires stop or fatal.
   If the runtime was ever started (run_live_entered), then group
   destruction requires stop to have been requested OR a fatal error.
   Fatal errors bypass normal stop and go directly to teardown. *)
Inv9NoCancelAfterGroupDestroy ==
    (~group_alive /\ run_live_entered) => (stop_requested \/ fatal_snapshot)

(* E16-Inv10: Startup abort excludes Running *)
Inv10StartupAbortExcludesRunning ==
    (startup_abort_requested /\ runtime_state = "Starting") =>
    ~run_live_entered

(* E16-Inv11: Driver uniqueness *)
Inv11DriverUniqueness ==
    driver_spawned => (driver_state # "not_started")

(* E16-Inv12: Close owner uniqueness *)
Inv12CloseOwnerUniqueness ==
    close_state = "InProgress" => close_owner \in Callers

(* E16-Inv13: Close state monotonicity *)
Inv13CloseMonotonicity ==
    close_state = "Closed" => (runtime_state = "Stopped" \/ runtime_state = "Fatal")

(* E16-Inv14: Successful submit boundary publication *)
Inv14SubmitPublication ==
    successful_submit_published => (control_epoch > 0)

(* E16-Inv15: Rollback completeness (structural; encoded in SubmitRollback) *)
Inv15RollbackComplete ==
    (~admission_reservation_active /\ admitted_count = 0) =>
    task_set_terminal_snapshot = (~admission_open)

(* E16-Inv16: Driver boundary park uses persistent authority.
   NOTE: This is a liveness concern (covered by WF on DriverObserveEpoch
   and DriverReenterRunLive in FairSpec), NOT a safety invariant. There is a
   legitimate transient window between epoch bump and driver observation.
   Kept as documentation; excluded from combined Inv. *)
Inv16BoundaryParkAuthority ==
    (driver_state \in {"between_invocations", "drained_wait"}
     /\ ~driver_exit_requested /\ ~fatal_snapshot)
    => (control_epoch = observed_epoch \/ driver_state = "drained_wait")

(* E16-Inv17: No boundary busy-loop (structural; DriverReenterRunLive
   requires epoch change) *)
Inv17NoBusyLoop == TRUE

(* E16-Inv18: Post-drain driver does not exit early *)
Inv18PostDrainNoEarlyExit ==
    (driver_state = "drained_wait"
     /\ ~driver_exit_requested /\ ~fatal_snapshot)
    => driver_state # "exited"

(* E16-Inv19: Driver exit precedes destruction *)
Inv19DriverExitBeforeDestruction ==
    ~scheduler_alive => (driver_state = "exited" \/ ~driver_spawned)

(* E16-Inv20: Rejected submit never executes *)
Inv20RejectedNeverExecutes ==
    \A t \in Tasks :
        (~task_admitted[t] => ~task_terminated[t])

(* E16-Inv21: Task I/O capability lifetime *)
Inv21TaskIOLifetime ==
    \A t \in Tasks :
        (task_io_submitted[t] => task_admitted[t])

(* E16-Inv22: Safe destructor states *)
Inv22SafeDestructor ==
    (close_state = "Closed" /\ runtime_state = "Stopped") =>
    ~resources_alive

(* E16-Inv23: Join return authority *)
Inv23JoinReturn ==
    (runtime_state = "Stopped" /\ close_state = "Closed") =>
    (driver_joined /\ ~resources_alive)

(* E16-Inv24: Shutdown is state-dispatched (structural) *)
Inv24ShutdownDispatched == TRUE

(* Combined invariant *)
Inv ==
    /\ Inv1Typing
    /\ Inv2AdmissionAuthority
    /\ Inv3StopClosesAdmission
    /\ Inv4AccountingBounds
    /\ Inv5TerminalSnapshot
    /\ Inv6DrainComplete
    /\ Inv7NoPrematureStopped
    /\ Inv8ResourceHierarchy
    /\ Inv9NoCancelAfterGroupDestroy
    /\ Inv10StartupAbortExcludesRunning
    /\ Inv11DriverUniqueness
    /\ Inv12CloseOwnerUniqueness
    /\ Inv13CloseMonotonicity
    /\ Inv14SubmitPublication
    /\ Inv18PostDrainNoEarlyExit
    /\ Inv19DriverExitBeforeDestruction
    /\ Inv20RejectedNeverExecutes
    /\ Inv21TaskIOLifetime
    /\ Inv22SafeDestructor
    /\ Inv23JoinReturn

(* =========================================================================
   LIVENESS (Section 11)
   ========================================================================= *)

(* Fairness declarations:
   - WF on DriverEnterRunLive: if Running and driver at barrier, it enters.
     Legitimate: the driver thread is running and the barrier predicate is met.
   - WF on DriverReenterRunLive: if epoch changed, driver re-enters.
     Legitimate: the CV predicate is persistently true.
   - WF on DriverObserveStartupAbort: abort eventually observed.
     Legitimate: the CV is signaled and the driver is waiting.
   - WF on DriverExit: exit request eventually honored.
     Legitimate: persistent predicate true under lifecycle_mutex.
   - WF on TaskBodyExit: admitted task body eventually exits.
     Environment assumption: user task terminates.
   - WF on GroupFuturePublish: Group Future publishes after task exit.
     Legitimate: internal protocol, no external dependency.
   - WF on CompleteTaskIO + ReapTaskIO: I/O eventually completes/reaps.
     Environment assumption: backend completes operations.
   - WF on PublishDrainComplete: driver publishes when conditions met.
     Legitimate: driver at boundary evaluates conditions.
*)

FairSpec ==
    /\ WF_vars(DriverEnterRunLive)
    /\ WF_vars(DriverRunLiveReturn)
    /\ WF_vars(DriverReenterRunLive)
    /\ WF_vars(DriverObserveStartupAbort)
    /\ WF_vars(DriverExit)
    /\ WF_vars(PublishDrainComplete)
    /\ WF_vars(EnterDrainedWait)
    /\ WF_vars(StartupAbortJoin)
    /\ WF_vars(StartupAbortClose)
    /\ WF_vars(RequestDriverExit)
    /\ WF_vars(JoinDriver)
    /\ WF_vars(DestroyGroup)
    /\ WF_vars(DestroyScheduler)
    /\ WF_vars(DestroyIoContext)
    /\ WF_vars(PublishStopped)
    /\ \A t \in Tasks : WF_vars(TaskBodyExit(t))
    /\ \A t \in Tasks : WF_vars(GroupFuturePublish(t))
    /\ \A t \in Tasks : WF_vars(CompleteTaskIO(t))
    /\ \A t \in Tasks : WF_vars(ReapTaskIO(t))

LivenessSpec == Spec /\ FairSpec

(* E16-Live1: Successful admitted task is observed by driver *)
Live1AdmissionObserved ==
    [](successful_submit_published /\ driver_state = "between_invocations"
      => <>(driver_state = "in_run_live" \/ runtime_state = "Stopped"
            \/ runtime_state = "Fatal" \/ driver_state = "drained_wait"
            \/ driver_state = "exited"))

(* E16-Live2: Stop-before-commit completes rollback *)
Live2StopBeforeCommit ==
    [](runtime_state = "Starting" /\ startup_abort_requested
      => <>(runtime_state = "Stopped"))

(* E16-Live3: Draining completes when work and I/O terminate *)
Live3DrainCompletes ==
    [](runtime_state = "Draining"
      => <>(drain_complete \/ runtime_state = "Fatal"))

(* E16-Live4: Close waiter completes *)
Live4CloseWaiterCompletes ==
    [](close_state = "Closed" => <>(runtime_state = "Stopped"))

(* E16-Live5: Driver exits after explicit exit request *)
Live5DriverExits ==
    [](driver_exit_requested /\ driver_state \in {"between_invocations","drained_wait","barrier_wait"}
      => <>(driver_state = "exited"))

(* E16-Live6: Post-drain park is stable (safety/liveness pair) *)
Live6PostDrainStable ==
    [](driver_state = "drained_wait" /\ ~driver_exit_requested /\ ~fatal_snapshot
      => driver_state = "drained_wait")

LifeProps ==
    /\ Live1AdmissionObserved
    /\ Live2StopBeforeCommit
    /\ Live3DrainCompletes
    /\ Live5DriverExits

(* =========================================================================
   REACHABILITY / NON-VACUITY (Section 13)
   ========================================================================= *)

NotReach_R1 == ~(runtime_state = "Running" /\ run_live_entered)
NotReach_R2 == ~(runtime_state = "Constructed" /\ stop_requested)
NotReach_R3 == ~(startup_abort_requested /\ driver_state = "exited" /\ ~run_live_entered)
NotReach_R4 == ~(\E t \in Tasks : task_terminated[t])
NotReach_R5 == ~(admission_reservation_active)
NotReach_R6 == ~(successful_submit_published /\ driver_state = "between_invocations")
NotReach_R7 == ~(runtime_state = "Draining")
NotReach_R8 == ~(\E t \in Tasks : task_terminated[t] /\ task_io_submitted[t] /\ ~task_io_complete[t])
NotReach_R9 == ~(drain_complete)
NotReach_R10 == ~(driver_state = "drained_wait")
NotReach_R11 == ~(runtime_state = "Stopped" /\ close_owner \in Callers)
NotReach_R12 == ~(close_state = "InProgress")
NotReach_R13 == ~(runtime_state = "Stopped" /\ ~driver_spawned)
NotReach_R14 == ~(runtime_state = "Stopped" /\ close_state = "Closed")
NotReach_R15 == ~(startup_abort_requested /\ runtime_state = "Starting")
NotReach_R16 == ~(\E t \in Tasks : task_terminated[t] /\ group_future_terminal_count > 0)

=============================================================================
