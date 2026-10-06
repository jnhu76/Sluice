---------------- MODULE ShutdownCore ----------------
(***************************************************************************)
(* E2 (#452): the shutdown / reference-accounting spine.                   *)
(*                                                                         *)
(* Source correspondence: the merged E2 implementation --                  *)
(*   include/sluice/async/async_io_context.hpp (ShutdownPolicy,            *)
(*     ShutdownOutcome, the settlement seam, stop_policy_ /                *)
(*     execution_closed_ / settlement_outcome_),                           *)
(*   src/async/async_io_context.cpp (drive_settlement_: close admission,   *)
(*     collect_outstanding + cancel_identity under cancel_then_drain, the  *)
(*     pass loop, settlement_converged_, stop_execution,                   *)
(*     retire_execution_resources, retire_notification, the                *)
(*     execution-closed write), the destructor (fail-fast on a live        *)
(*     progress binding, on public bindings, and on a settlement that did  *)
(*     not complete),                                                      *)
(*   include/sluice/async/detail/request_core.hpp (CoreOccupancy:          *)
(*     outstanding / execution_refs / control_refs / publication_inflight  *)
(*     / observer_registrations; collect_outstanding;                      *)
(*     retire_delivered_episodes; reclaimable_),                           *)
(*   src/async/detail/request_core.cpp (accept claims the execution        *)
(*     borrow; publish completes the public result; consume/discard        *)
(*     release the binding; the pin outlives the binding),                 *)
(*   src/async/threadpool_backend.cpp + uring_backend.cpp                  *)
(*     (internal_work_retired / stop_execution / retire_execution_resources;*)
(*     quiescence witnesses expressed as settlement facts, retained        *)
(*     published results with live bindings legal at execution close).     *)
(*                                                                         *)
(* One model action per implementation transition; every guard is the      *)
(* guard of the C++ operation it names.  The model is deliberately free    *)
(* of backend internals (ring, workers, queues), mutex discipline, the     *)
(* poison/Class-A recovery machinery and the progress-wait protocol: the   *)
(* backend enters only through the retirement facts the settlement reads.  *)
(* Unresolved backend health (the health_unresolved / health_failed        *)
(* outcomes) is out of scope here: those outcomes keep the context         *)
(* unsettled forever, which the model represents by the absence of any     *)
(* CloseExecution edge, not by extra state.                                *)
(*                                                                         *)
(* Actors: Owner (Accept / Consume / Discard / RegisterHost /              *)
(*   UnregisterHost / Destroy -- the external caller), Driver              *)
(*   (RecordCancelPolicy / CloseAdmission / CancelOutstandingWin /         *)
(*   CancelOutstandingControl / KernelComplete / BeginPublish /            *)
(*   ClaimEpisode / RetireEpisode / CloseExecution / RetireNotification    *)
(*   / Reclaim -- the settling progress owner), RequestCore (the           *)
(*   reference facts every action reads and writes).                       *)
(* Atomicity: every action is one critical section at the granularity      *)
(*   the C++ serialization provides (the core mutex, or the settling       *)
(*   owner's exclusive drive).                                             *)
(*                                                                         *)
(* Per-operation facts (OpId = {"o1","o2"}):                               *)
(*   accepted        the operation is in the finite accepted set           *)
(*   terminal        none / ok / err / canceled -- the chosen terminal     *)
(*   published       the public result is published                        *)
(*   borrow_refs     execution references that may touch the caller's      *)
(*                   buffer or File (CoreOccupancy.execution_refs)         *)
(*   control_refs    backend control pins (CoreOccupancy.control_refs)     *)
(*   episode_refs    delivery episodes claimed but not retired             *)
(*                   (CoreOccupancy.observer_registrations)                *)
(*   episode_claimed_once  a delivery episode already happened for this    *)
(*                   request incarnation; registration is one-shot         *)
(*   bindings        live public Request bindings                          *)
(*   reclaimed       the slot returned to the free set (release_slot_)     *)
(* Context facts:                                                          *)
(*   admission_open  the core admission authority                          *)
(*   close_snapshot  the accepted set captured at the admission close      *)
(*                   (the finite set the shutdown drives), as the          *)
(*                   record twin of accepted                               *)
(*   policy          drain / cancel_then_drain (stop_policy_, upgrade-only)*)
(*   stop_recorded   request_stop/close recorded something                 *)
(*   execution_closed  the SHUT execution-closed record                    *)
(*   notification_open the progress notification fd (retire_notification)  *)
(*   host_interest   external progress/notification registrations          *)
(*   destroyed       the context storage is freed                          *)
(*                                                                         *)
(* Fairness: the safety invariants and reachability certificates hold on    *)
(* Spec alone. The conditional liveness pair (LiveOpsSettle,                *)
(* LiveShutdownCloses) holds on LSpec, whose WF conjuncts are the explicit  *)
(* backend/progress assumptions; each property is separately violated on    *)
(* the fairness-free Spec by its own negative-control configuration.        *)
(***************************************************************************)
EXTENDS Naturals, Sequences, TLC

CONSTANT OpIds,
         MutM1DestroyIgnoresClosure,
         MutM4RequireConsumption,
         MutM6SnapshotBeforeClose,
         MutM7CloseWithoutConvergence,
         MutM8RetireFdEarly

OpSet == OpIds

VARIABLES
    \* per-operation facts
    accepted,       \* [OpId -> BOOLEAN]
    terminal,       \* [OpId -> {"none","ok","err","canceled"}]
    published,      \* [OpId -> BOOLEAN]
    borrow_refs,    \* [OpId -> Nat]   execution_refs
    control_refs,   \* [OpId -> Nat]   control_refs
    cancel_targeted,  \* [OpId -> BOOLEAN]: a cancel action targeted the op
    episode_refs,   \* [OpId -> Nat]   observer_registrations
    episode_claimed_once,  \* [OpId -> BOOLEAN]: one registration per request
    bindings,       \* [OpId -> Nat]   public_bindings
    reclaimed,      \* [OpId -> BOOLEAN]
    \* context facts
    admission_open,
    close_snapshot,     \* SUBSET OpIds: the frozen finite accepted set
    policy,             \* "drain" / "cancel_then_drain"
    stop_recorded,
    execution_closed,
    notification_open,
    host_interest,      \* Nat: external registration count
    destroyed

TypeOK ==
    /\ accepted \in [OpSet -> BOOLEAN]
    /\ terminal \in [OpSet -> {"none","ok","err","canceled"}]
    /\ published \in [OpSet -> BOOLEAN]
    /\ borrow_refs \in [OpSet -> Nat]
    /\ control_refs \in [OpSet -> Nat]
    /\ cancel_targeted \in [OpSet -> BOOLEAN]
    /\ episode_refs \in [OpSet -> Nat]
    /\ episode_claimed_once \in [OpSet -> BOOLEAN]
    /\ bindings \in [OpSet -> Nat]
    /\ reclaimed \in [OpSet -> BOOLEAN]
    /\ admission_open \in BOOLEAN
    /\ close_snapshot \in [OpSet -> BOOLEAN]
    /\ policy \in {"drain", "cancel_then_drain"}
    /\ stop_recorded \in BOOLEAN
    /\ execution_closed \in BOOLEAN
    /\ notification_open \in BOOLEAN
    /\ host_interest \in Nat
    /\ destroyed \in BOOLEAN

-------------------------------------------------------------------------------
(* External caller surface *)

Accept(o) ==
    /\ admission_open \/ MutM6SnapshotBeforeClose
    /\ ~accepted[o]
    /\ ~reclaimed[o]
    /\ accepted' = [accepted EXCEPT ![o] = TRUE]
    /\ terminal' = [terminal EXCEPT ![o] = "none"]
    /\ published' = [published EXCEPT ![o] = FALSE]
    /\ borrow_refs' = [borrow_refs EXCEPT ![o] = 1]
    /\ control_refs' = [control_refs EXCEPT ![o] = 0]
    /\ episode_refs' = [episode_refs EXCEPT ![o] = 0]
    /\ episode_claimed_once' = [episode_claimed_once EXCEPT ![o] = FALSE]
    /\ bindings' = [bindings EXCEPT ![o] = 1]
    /\ UNCHANGED <<reclaimed, admission_open, close_snapshot, policy,
                   stop_recorded, execution_closed, notification_open,
                   host_interest, destroyed, cancel_targeted>>

ConsumeOrDiscard(o) ==
    /\ published[o]
    /\ bindings[o] = 1
    /\ bindings' = [bindings EXCEPT ![o] = 0]
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, reclaimed, admission_open, close_snapshot,
                   policy, stop_recorded, execution_closed,
                   notification_open, host_interest, destroyed, cancel_targeted, episode_claimed_once>>

RegisterHost ==
    /\ ~destroyed
    /\ host_interest < 2
    /\ host_interest' = host_interest + 1
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, admission_open,
                   close_snapshot, policy, stop_recorded, execution_closed,
                   notification_open, destroyed, cancel_targeted, episode_claimed_once>>

UnregisterHost ==
    /\ host_interest > 0
    /\ host_interest' = host_interest - 1
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, admission_open,
                   close_snapshot, policy, stop_recorded, execution_closed,
                   notification_open, destroyed, cancel_targeted, episode_claimed_once>>

(* The destructor's always-on preconditions: an external caller violation
   has no successor state -- the process terminates instead.  MutM1 is the
   pre-E2 classification (bare occupancy treated as destroyable): the
   settlement record is not required and an internal pin would pass. *)
Destroy ==
    /\ ~destroyed
    /\ IF MutM1DestroyIgnoresClosure
        THEN /\ \A o \in OpSet: borrow_refs[o] = 0
             /\ \A o \in OpSet: control_refs[o] = 0
             /\ host_interest = 0
             /\ \A o \in OpSet: bindings[o] = 0
        ELSE /\ execution_closed
             /\ host_interest = 0
             /\ \A o \in OpSet: bindings[o] = 0
    /\ destroyed' = TRUE
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, admission_open,
                   close_snapshot, policy, stop_recorded, execution_closed,
                   notification_open, host_interest, cancel_targeted, episode_claimed_once>>

-------------------------------------------------------------------------------
(* Driver surface: the settlement path *)

RecordCancelPolicy ==
    /\ ~execution_closed
    /\ policy = "drain"
    /\ policy' = "cancel_then_drain"
    /\ stop_recorded' = TRUE
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, admission_open,
                   close_snapshot, execution_closed, notification_open,
                   host_interest, destroyed, cancel_targeted, episode_claimed_once>>

CloseAdmission ==
    /\ admission_open
    /\ admission_open' = FALSE
    /\ close_snapshot' = accepted
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, policy, stop_recorded,
                   execution_closed, notification_open, host_interest,
                   destroyed, cancel_targeted, episode_claimed_once>>

(* cancel_then_drain against an operation the kernel has not entered yet:
   the cancel wins before execution, the borrow retires and the canceled
   terminal publishes (PublicCancel::won_before_execution). *)
CancelOutstandingWin(o) ==
    /\ ~execution_closed
    /\ stop_recorded
    /\ policy = "cancel_then_drain"
    /\ accepted[o]
    /\ ~published[o]
    /\ terminal[o] = "none"
    /\ borrow_refs[o] = 1
    /\ terminal' = [terminal EXCEPT ![o] = "canceled"]
    /\ borrow_refs' = [borrow_refs EXCEPT ![o] = 0]
    /\ published' = [published EXCEPT ![o] = TRUE]
    /\ cancel_targeted' = [cancel_targeted EXCEPT ![o] = TRUE]
    /\ UNCHANGED <<accepted, control_refs, episode_refs, bindings, reclaimed,
                   admission_open, close_snapshot, policy, stop_recorded,
                   execution_closed, notification_open, host_interest,
                   destroyed, episode_claimed_once>>

(* cancel_then_drain against a kernel-visible operation: the control pins
   the slot (CoreOccupancy.control_refs) until the control retires; the
   borrow stays until the kernel outcome arrives. *)
CancelOutstandingControl(o) ==
    /\ ~execution_closed
    /\ stop_recorded
    /\ policy = "cancel_then_drain"
    /\ accepted[o]
    /\ ~published[o]
    /\ terminal[o] = "none"
    /\ borrow_refs[o] = 1
    /\ control_refs[o] = 0
    /\ control_refs' = [control_refs EXCEPT ![o] = 1]
    /\ cancel_targeted' = [cancel_targeted EXCEPT ![o] = TRUE]
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, episode_refs,
                   bindings, reclaimed, admission_open, close_snapshot,
                   policy, stop_recorded, execution_closed,
                   notification_open, host_interest, destroyed, episode_claimed_once>>

(* The kernel completes a pinned operation: the borrow retires and the
   physical terminal is chosen; publication follows in a later pass.  A
   kernel only reports ECANCELED to operations a cancel control actually
   targeted (the C++ terminal mint at uring_completion), so the canceled
   terminal requires cancel_targeted[o]. *)
KernelComplete(o) ==
    /\ accepted[o]
    /\ ~published[o]
    /\ terminal[o] = "none"
    /\ borrow_refs[o] = 1
    /\ terminal' = [terminal EXCEPT ![o] =
                        IF cancel_targeted[o] THEN "canceled" ELSE "ok"]
    /\ borrow_refs' = [borrow_refs EXCEPT ![o] = 0]
    /\ UNCHANGED <<accepted, published, control_refs, episode_refs, bindings,
                   reclaimed, admission_open, close_snapshot, policy,
                   stop_recorded, execution_closed, notification_open,
                   host_interest, destroyed, cancel_targeted, episode_claimed_once>>

(* The control retires once the terminal is chosen (delivered CQE or Class-A
   recovery); this is the internal retirement obligation of D4. *)
RetireControl(o) ==
    /\ control_refs[o] = 1
    /\ terminal[o] # "none"
    /\ control_refs' = [control_refs EXCEPT ![o] = 0]
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, episode_refs,
                   bindings, reclaimed, admission_open, close_snapshot,
                   policy, stop_recorded, execution_closed,
                   notification_open, host_interest, destroyed, cancel_targeted, episode_claimed_once>>

BeginPublish(o) ==
    /\ terminal[o] # "none"
    /\ ~published[o]
    /\ published' = [published EXCEPT ![o] = TRUE]
    /\ UNCHANGED <<accepted, terminal, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, admission_open,
                   close_snapshot, policy, stop_recorded, execution_closed,
                   notification_open, host_interest, destroyed, cancel_targeted, episode_claimed_once>>

(* OBS-01: at most one active terminal-notification registration per
   request, and SHUT-02 closes new attachment during shutdown while the
   existing registrations retire. The claimed-once bit is that finiteness:
   without it ClaimEpisode/RetireEpisode could alternate forever and the
   settlement could fairly evade execution close. *)
ClaimEpisode(o) ==
    /\ (notification_open \/ MutM8RetireFdEarly)
    /\ ~execution_closed
    /\ published[o]
    /\ ~reclaimed[o]
    /\ episode_refs[o] = 0
    /\ ~episode_claimed_once[o]
    /\ episode_refs' = [episode_refs EXCEPT ![o] = 1]
    /\ episode_claimed_once' = [episode_claimed_once EXCEPT ![o] = TRUE]
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   bindings, reclaimed, admission_open, close_snapshot,
                   policy, stop_recorded, execution_closed,
                   notification_open, host_interest, destroyed, cancel_targeted>>

RetireEpisode(o) ==
    /\ episode_refs[o] = 1
    /\ episode_refs' = [episode_refs EXCEPT ![o] = 0]
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   bindings, reclaimed, admission_open, close_snapshot,
                   policy, stop_recorded, execution_closed,
                   notification_open, host_interest, destroyed, cancel_targeted, episode_claimed_once>>

(* The convergence predicate: settlement_converged_ -- the backend internal
   retirement witness (all controls retired; the dispatch/publication facts
   are below the model's abstraction line) plus the occupancy facts. *)
Converged ==
    /\ \A o \in OpSet:
            /\ borrow_refs[o] = 0
            /\ control_refs[o] = 0
            /\ episode_refs[o] = 0
            /\ (accepted[o] => published[o])

CloseExecution ==
    /\ ~execution_closed
    /\ ~admission_open
    /\ ~notification_open
    /\ (Converged \/ MutM7CloseWithoutConvergence)
    /\ (~MutM4RequireConsumption \/ (\A o \in OpSet: bindings[o] = 0))
    /\ execution_closed' = TRUE
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, admission_open,
                   close_snapshot, policy, stop_recorded, notification_open,
                   host_interest, destroyed, cancel_targeted, episode_claimed_once>>

(* SHUT ordering: the notification retires only after the delivery episodes
   are retired -- a wake can never be lost or land on a reused descriptor
   for an episode the contract still owes. *)
RetireNotification ==
    /\ notification_open
    /\ Converged
    /\ notification_open' = FALSE
    /\ UNCHANGED <<accepted, terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, reclaimed, admission_open,
                   close_snapshot, policy, stop_recorded, execution_closed,
                   host_interest, destroyed, cancel_targeted, episode_claimed_once>>

(* try_reclaim_: the slot frees only when every reference is gone.  A
   retained published result keeps the slot (V20). *)
Reclaim(o) ==
    /\ ~reclaimed[o]
    /\ accepted[o]
    /\ published[o]
    /\ bindings[o] = 0
    /\ borrow_refs[o] = 0
    /\ control_refs[o] = 0
    /\ episode_refs[o] = 0
    /\ reclaimed' = [reclaimed EXCEPT ![o] = TRUE]
    /\ accepted' = [accepted EXCEPT ![o] = FALSE]
    /\ UNCHANGED <<terminal, published, borrow_refs, control_refs,
                   episode_refs, bindings, admission_open, close_snapshot,
                   policy, stop_recorded, execution_closed,
                   notification_open, host_interest, destroyed, cancel_targeted, episode_claimed_once>>

Next ==
    \/ \E o \in OpSet: Accept(o)
    \/ \E o \in OpSet: ConsumeOrDiscard(o)
    \/ RegisterHost
    \/ UnregisterHost
    \/ Destroy
    \/ RecordCancelPolicy
    \/ CloseAdmission
    \/ \E o \in OpSet: CancelOutstandingWin(o)
    \/ \E o \in OpSet: CancelOutstandingControl(o)
    \/ \E o \in OpSet: KernelComplete(o)
    \/ \E o \in OpSet: RetireControl(o)
    \/ \E o \in OpSet: BeginPublish(o)
    \/ \E o \in OpSet: ClaimEpisode(o)
    \/ \E o \in OpSet: RetireEpisode(o)
    \/ CloseExecution
    \/ RetireNotification
    \/ \E o \in OpSet: Reclaim(o)

-------------------------------------------------------------------------------
(* Safety: the settlement contract *)

(* The accepted set never grows after the admission close: every accepted
   operation was in the snapshot frozen at the close. *)
(* Safety: the settlement contract *)

(* The accepted set never grows after the admission close: every accepted
   operation was in the snapshot frozen at the close.  A mutant that
   snapshots the accepted set before closing admission admits operations
   outside the snapshot and is killed here. *)
InvDestroyRequiresExecutionClosed ==
    destroyed => execution_closed

InvNoAcceptAfterAdmissionClose ==
    admission_open \/ (\A o \in OpSet: accepted[o] => close_snapshot[o])

InvDestroyRequiresNoExternalBindings ==
    destroyed => /\ host_interest = 0
                 /\ \A o \in OpSet: bindings[o] = 0

InvDestroyRequiresNoBorrowTouchingRefs ==
    destroyed => \A o \in OpSet: borrow_refs[o] = 0

InvExecutionClosedNoBorrowTouchingExecution ==
    execution_closed => /\ \A o \in OpSet: borrow_refs[o] = 0
                        /\ \A o \in OpSet: control_refs[o] = 0

(* The kernel never reports ECANCELED for an operation no cancel control
   targeted: a canceled terminal is reachable only through a cancel action. *)
InvCanceledRequiresTargeting ==
    \A o \in OpSet: terminal[o] = "canceled" => cancel_targeted[o]

(* The slot frees only when every reference the contract requires has
   retired; a retained published result keeps its slot legally. *)
InvNoReclaimBeforeAllRequiredRefsRetire ==
    \A o \in OpSet:
        reclaimed[o] =>
            /\ ~accepted[o]
            /\ bindings[o] = 0
            /\ borrow_refs[o] = 0
            /\ control_refs[o] = 0
            /\ episode_refs[o] = 0

(* A bare internal pin with all external bindings gone is never by itself a
   caller violation: the Destroy guard names only external facts, so a
   context holding just an internal control pin stays destroyable once the
   settlement closes execution.  A mutant that turns the internal pin into
   a caller violation (the pre-E2 destructor) can never reach destroyed
   from such a state and is killed by the D4 coverage certificate. *)
InvInternalPinIsNotCallerViolation ==
    \A o \in OpSet:
        (control_refs[o] > 0 /\ \A p \in OpSet: bindings[p] = 0) =>
            execution_closed \/ ~destroyed

(* The notification never retires while a delivery episode is owed: a
   mutant that detaches the fd without the delivery retirement
   acknowledgement is killed here. *)
InvNotificationRetireRequiresNoDeliveryRefs ==
    ~notification_open => \A o \in OpSet: episode_refs[o] = 0

(* ---------------------------------------------------------------------- *)
(* Reachability certificates: each is expected to be VIOLATED in its own
   configuration (the negated conjunction), which certifies the state the
   contract requires to be reachable is reachable. *)

InvCovPublishedSurvivesExecutionClose ==
    ~(execution_closed /\ \E o \in OpSet: published[o] /\ bindings[o] = 1)

InvCovInternalPinThenCleanClose ==
    ~\E o \in OpSet:
        control_refs[o] = 1 /\ \A p \in OpSet: bindings[p] = 0
        /\ ~destroyed

InvCovCancelPolicyCompletes ==
    ~(execution_closed /\ \E o \in OpSet: terminal[o] = "canceled")

InvCovUnconsumedResultConverges ==
    ~(execution_closed /\ \E o \in OpSet: published[o] /\ bindings[o] = 1
                                                /\ terminal[o] = "ok")

(* Holding forms for the MutM4 run: with the close requiring every binding
   consumed, the certified retained-result states are unreachable, so these
   invariants hold and the clean run is the kill. *)
CertM4CloseRequiresZeroBindings ==
    execution_closed => \A o \in OpSet: bindings[o] = 0

CertM4NoRetainedResultAtClose ==
    execution_closed => ~\E o \in OpSet: published[o] /\ bindings[o] = 1

Init ==
    /\ accepted = [o \in OpSet |-> FALSE]
    /\ terminal = [o \in OpSet |-> "none"]
    /\ published = [o \in OpSet |-> FALSE]
    /\ borrow_refs = [o \in OpSet |-> 0]
    /\ control_refs = [o \in OpSet |-> 0]
    /\ cancel_targeted = [o \in OpSet |-> FALSE]
    /\ episode_refs = [o \in OpSet |-> 0]
    /\ episode_claimed_once = [o \in OpSet |-> FALSE]
    /\ bindings = [o \in OpSet |-> 0]
    /\ reclaimed = [o \in OpSet |-> FALSE]
    /\ admission_open = TRUE
    /\ close_snapshot = [o \in OpSet |-> FALSE]
    /\ policy = "drain"
    /\ stop_recorded = FALSE
    /\ execution_closed = FALSE
    /\ notification_open = TRUE
    /\ host_interest = 0
    /\ destroyed = FALSE

vars == <<accepted, terminal, published, borrow_refs, control_refs,
          episode_refs, episode_claimed_once, bindings, reclaimed,
          admission_open, close_snapshot,
          policy, stop_recorded, execution_closed, notification_open,
          host_interest, destroyed, cancel_targeted>>

Spec ==
    Init /\ [][Next]_vars

(* Conditional liveness for the SHUT-03 convergence fragment: the owner
   keeps driving settlement steps and the kernel eventually reports each
   outstanding physical outcome. The C++ poison-stuck-borrow branch (V22:
   health_unresolved, execution stays open) is excluded by the kernel
   fairness assumption, not modeled as a stuck state. *)
Fairness ==
    /\ WF_vars(CloseAdmission)
    /\ WF_vars(RetireNotification)
    /\ WF_vars(CloseExecution)
    /\ \A o \in OpSet:
        /\ WF_vars(KernelComplete(o))
        /\ WF_vars(CancelOutstandingWin(o))
        /\ WF_vars(CancelOutstandingControl(o))
        /\ WF_vars(RetireControl(o))
        /\ WF_vars(BeginPublish(o))
        /\ WF_vars(ClaimEpisode(o))
        /\ WF_vars(RetireEpisode(o))

LSpec == Spec /\ Fairness

LiveOpsSettle ==
    \A o \in OpSet: accepted[o] ~> terminal[o] # "none"

(* SHUT-03 conditional convergence over the whole settlement, not a
   post-retirement fragment: once admission has closed and the context is
   not destroyed, fair kernel completion, fair physical/control retirement
   and fair progress drive the state to the execution-closed record. The
   one-shot episode abstraction keeps the registration region finite, so no
   fair behavior can loop forever short of the close. *)
LiveShutdownCloses ==
    (~admission_open /\ ~destroyed) ~> execution_closed

=============================================================================
