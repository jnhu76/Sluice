---------------- MODULE ObserverCore ----------------
(***************************************************************************)
(* C1 (#396/#433): the host-neutral Observer delivery protocol.            *)
(*                                                                         *)
(* Source correspondence: the merged #396 implementation --               *)
(*   include/sluice/async/detail/observer_protocol.hpp (ObserverPhase and  *)
(*     the four disposition vocabularies),                                 *)
(*   src/async/detail/request_core.cpp (register_observer,                 *)
(*     complete_publication's queueing edge, claim_observer_delivery,      *)
(*     cancel_observer, retire_observer_delivery, reclaimable_,            *)
(*     release_slot_),                                                     *)
(*   src/async/uring_backend.cpp / threadpool_backend.cpp (deliver_event:  *)
(*     the driver's claim before the ready event),                         *)
(*   src/async/scheduler_park_wake.cpp + scheduler.cpp (the host adapter:  *)
(*     await_completion reserve-before-arm, cancel_waiter, on_ready        *)
(*     routing, drain retirement and wake).                                *)
(* One model action per protocol transition; every guard is the guard of   *)
(* the C++ operation it names.  The model is deliberately free of          *)
(* io_uring/threadpool internals, mutexes, Scheduler/Fiber structure and   *)
(* ProgressSource vocabulary: the publication event enters as the          *)
(* CompletePublish action and nothing else (the S5 backend-neutrality      *)
(* boundary -- no backend -> scheduler/waiter/progress edge exists here).  *)
(*                                                                         *)
(* Actors: Operation (Accept/SettleTerminal/CancelRequest/                 *)
(*   ReleaseBinding -- the request owner), Driver (BeginPublish/           *)
(*   CompletePublish/ClaimDelivery -- the fixed progress owner), Host      *)
(*   waiter (WaiterArrive/WaiterRefused/CancelRegistration/                *)
(*   RouteDelivery/ProcessDelivery -- the RuntimeTaskContext adapter),     *)
(*   RequestCore (the phase facts every action reads and writes).         *)
(* Atomicity: every action is one core-mutex critical section (or one     *)
(*   adapter step outside the core, as marked); two concurrent actors     *)
(*   interleave only at action boundaries, which is exactly the C++       *)
(*   serialization the mutex provides.                                     *)
(*                                                                         *)
(* Abstraction decisions, each mapping to the C++ fact it preserves:      *)
(*   - operation_state collapses the slot lifecycle to the observer-      *)
(*     relevant spine accepted/terminal/publishing/published; the exec/   *)
(*     ctl/publication-pin half of reclaimable_ is B1-1's obligation      *)
(*     (#394, RequestCore.tla) and is abstracted as "settled by the      *)
(*     time publication completes" here.                                  *)
(*   - The independent CancelRequest models the public Request cancel     *)
(*     (won_before_execution -> terminal "canceled"); the post-claim      *)
(*     cancel-intent arbitration also belongs to B1-1.  Observer cancel   *)
(*     never touches these facts (S1).                                    *)
(*   - already_terminal is a DISPOSITION WITHOUT A PHASE TRANSITION:      *)
(*     the C++ register_observer returns ObserverRegistration::           *)
(*     already_terminal and leaves observer_phase = unattached (no        *)
(*     Unattached->Retired edge is taken).  The model keeps the phase     *)
(*     unattached and records the host-visible disposition; the host's    *)
(*     fast path (Completion::ready() -> proceed without waiting) is      *)
(*     modeled as the waiter never entering the waiting state.            *)
(*   - Re-arm: C++ register_observer accepts unattached AND retired as    *)
(*     armable while unpublished, so cancel-then-re-arm is a NEW          *)
(*     registration generation.  reg_episode numbers them; at-most-once  *)
(*     delivery is per episode (OBS-02 "per registration generation").   *)
(*   - try_reclaim_ is factored into the Reclaim action with the same     *)
(*     predicate (published, binding released, observer unattached/       *)
(*     retired); the C++ inline calls and the factored action admit      *)
(*     the same reachable observer facts (the factoring only adds        *)
(*     interleaving).                                                     *)
(*   - Single host consumer: every arm is issued by the waiter path      *)
(*     (the merged RuntimeTaskContext adapter, C1-D).  The duplicate      *)
(*     disposition covers any second attach; an attach without a waiter  *)
(*     differs only by lacking wake obligations, which the core-side     *)
(*     invariants do not distinguish.                                     *)
(*                                                                         *)
(* Per-slot protocol facts:                                                *)
(*   operation_state  free / accepted / terminal / publishing / published  *)
(*   terminal_state   none / ok / err / canceled (chosen terminal)         *)
(*   binding_live     public binding (register resolves publicly, the     *)
(*                    claim/cancel/retire operations resolve internally   *)
(*                    -- the pin outlives the binding, OBS-03)            *)
(*   observer_phase   unattached / armed / queued / delivering / retired   *)
(*                    (ObserverPhase, one field, one mutex)               *)
(*   delivery_state   idle / owed / claimed / routed / retired -- the     *)
(*                    delivery obligation plus the host routing split     *)
(*                    (claimed = driver claimed, routed = sink marked     *)
(*                    the wait record delivered)                          *)
(*   waiter_state     absent / registered / delivered (host WaitRecord)   *)
(*   observer_registration  the host-visible disposition of the most      *)
(*                    recent protocol call (the C++ return values)       *)
(*   host_record_free the host wait-record pool, capacity one; the        *)
(*                    adapter reserves it BEFORE attach (C1-G), so       *)
(*                    exhaustion (no_space) precedes observer acceptance  *)
(*                                                                         *)
(* Ghost monitoring by full identity and registration episode:            *)
(*   stage_reg[r]     0 none / 1 armed / 2 queued / 3 claimed / 4 retired  *)
(*   delivery_claims[r], delivery_retires[r]  successful claim/hook       *)
(*                    returns per episode (S3 at-most-once evidence)      *)
(*   stage_op[i]      0 / 1 accepted / 2 terminal / 3 published           *)
(*   pub_done_ids     identities whose publication completed              *)
(*   obs_cancelled[i], op_cancel_requested[i]  S1 discrimination         *)
(*   refused_attach[i]  a non-arming disposition occurred while accepted  *)
(*   attach_in_window[r]  armed while terminal chosen but unpublished     *)
(*   suppressed_queued   episodes retired out of queued by cancellation   *)
(*   saw_in_progress     a cancel hit the delivering window and lost      *)
(*   arms_per_id[i], rearmed  coverage ghosts (observer-free settlement, *)
(*                    registration renewal)                               *)
(*   waiter_left[r]   none / completed / canceled -- how the episode's    *)
(*                    waiter left the waiting state                       *)
(*                                                                         *)
(* Safety floor (task S1-S5, #433's list, OBS-01..03):                     *)
(*   InvCancelIsolation                 S1 observer cancel never cancels  *)
(*   InvArmedRequiresUnpublished        S1/S4 attach resolves with        *)
(*   InvAlreadyTerminalRequiresPublication  publication atomically        *)
(*   InvPhaseDeliveryAgreement          S2 retired owes no delivery       *)
(*   InvRetiredStaysRetired             S2/#433 no return to active       *)
(*   InvAtMostOnceDelivery              S3 single claim per generation    *)
(*   InvSingleOwedEpisode               S3 one delivery window per id     *)
(*   InvDeliveryAfterPublication        S3 claim gated behind publication *)
(*   InvRetireOwnership                 S2/OBS-02 cancel ack != retire-  *)
(*                                       ment of a claimed delivery      *)
(*   InvObserverPin                     #433 reclaim needs retirement    *)
(*   InvWaiterBacking                   host retention until wake        *)
(*   InvUnoccupiedClean                 reset discipline                 *)
(*   S4's reachability half is the InvCovNoObserverSettlement witness;    *)
(*   its temporal half is L0 (settlement with no observer participation). *)
(*   S5 is structural: no variable or action names a backend, scheduler   *)
(*   or progress fact; publication enters only as CompletePublish.        *)
(*                                                                         *)
(* Conditional liveness under the declared assumptions (the C1-F C++      *)
(* mapping of OBS-03's progress bullets -- no instant scheduling, no      *)
(* bounded execution time, no infinite-CPU assumption; only weak fairness *)
(* of the services below, i.e. "the driver continues making progress"):   *)
(*   L0  accepted ~> published          (operation independence, S4)      *)
(*   L1  armed + publication ~> retired (delivery progress)              *)
(*   L2  waiting + publication ~> woken (waiter wake progress)           *)
(*   L4  refused attach ~> published    (registration failure preserves   *)
(*                                      the request, #433)                *)
(*   Fairness: WF on SettleTerminal, BeginPublish, CompletePublish,       *)
(*   ClaimDelivery, RouteDelivery, ProcessDelivery, Reclaim (the driver   *)
(*   service chain and the core reclaim service).  Accept, WaiterArrive,  *)
(*   WaiterRefused, CancelRegistration, CancelRequest, ReleaseBinding     *)
(*   are environment actions and carry NO fairness: attachment,           *)
(*   cancellation and binding release are host/owner choices.  Liveness   *)
(*   is conditioned on publication (L1/L2) rather than assuming it.  The  *)
(*   hook-termination assumption is ProcessDelivery's own atomicity; the  *)
(*   progress-owner service assumption is the WF conjuncts; no #397       *)
(*   ProgressSource concept appears (the model needs only "publication    *)
(*   completed" and "a driver eventually runs").                          *)
(*                                                                         *)
(* Mutant switches (all FALSE in the reference cfg).  The first five      *)
(* mirror the merged C++ mutation seams one-for-one (xmake/tests.lua,     *)
(* the SLUICE_C1_MUTANT_ defines); each must die on its named invariant   *)
(*   MutAttachIgnoresPublication     arms on a published slot; dies on    *)
(*                                   InvArmedRequiresUnpublished (and L2)*)
(*   MutAttachTreatsTerminalAsPublished  reports already-terminal while   *)
(*                                   only terminal-chosen; dies on        *)
(*                                   InvAlreadyTerminalRequiresPublication*)
(*   MutDeliveryClaimUnbounded       claims from armed/queued/delivering; *)
(*                                   dies on InvAtMostOnceDelivery       *)
(*   MutCancelDuringDeliveryReturns  cancel retires inside the delivery   *)
(*                                   window; dies on InvRetireOwnership   *)
(*   MutPublicationSkipsQueue        publication does not queue the armed *)
(*                                   registration; dies on L1/L2          *)
(*   Model-only switches proving the remaining floor is load-bearing:     *)
(*   MutObsCancelCancelsOp           observer cancel also cancels the     *)
(*                                   operation; dies on InvCancelIsolation*)
(*   MutReclaimIgnoresObserver       reclaim drops the observer pin;      *)
(*                                   dies on InvObserverPin               *)
(*   MutNoDeliveryWake               the drain never processes a routed   *)
(*                                   delivery; dies on L1/L2              *)
(***************************************************************************)
EXTENDS Naturals

CONSTANT MutAttachIgnoresPublication,
          MutAttachTreatsTerminalAsPublished,
          MutDeliveryClaimUnbounded,
          MutCancelDuringDeliveryReturns,
          MutPublicationSkipsQueue,
          MutObsCancelCancelsOp,
          MutReclaimIgnoresObserver,
          MutNoDeliveryWake

Slots == {"s0"}
GenMax == 1
MaxEp == 2

Ids == [slot: Slots, gen: 0..GenMax]
Eps == 1..MaxEp
RegKeys == [slot: Slots, gen: 0..GenMax, ep: Eps]

Ops == {"free", "accepted", "terminal", "publishing", "published"}
Terms == {"none", "ok", "err", "canceled"}
Phases == {"unattached", "armed", "queued", "delivering", "retired"}
Deliveries == {"idle", "owed", "claimed", "routed", "retired"}
Waiters == {"absent", "registered", "delivered"}
Dispositions == {"none", "armed", "duplicate", "already_terminal",
                 "not_found", "no_space", "in_progress", "retired",
                 "not_registered"}
Lefts == {"none", "completed", "canceled"}

VARIABLES operation_state, terminal_state, binding_live, gen,
          observer_phase, delivery_state, waiter_state,
          observer_registration, host_record_free, reg_episode,
          stage_reg, delivery_claims, delivery_retires,
          stage_op, pub_done_ids, obs_cancelled, op_cancel_requested,
          refused_attach, attach_in_window, suppressed_queued,
          saw_in_progress, arms_per_id, rearmed, waiter_left

vars == <<operation_state, terminal_state, binding_live, gen,
          observer_phase, delivery_state, waiter_state,
          observer_registration, host_record_free, reg_episode,
          stage_reg, delivery_claims, delivery_retires,
          stage_op, pub_done_ids, obs_cancelled, op_cancel_requested,
          refused_attach, attach_in_window, suppressed_queued,
          saw_in_progress, arms_per_id, rearmed, waiter_left>>

Id(s) == [slot |-> s, gen |-> gen[s]]
CurReg(s) == [slot |-> s, gen |-> gen[s], ep |-> reg_episode[s]]
NewReg(s) == [slot |-> s, gen |-> gen[s], ep |-> reg_episode[s] + 1]
PubDoneFor(r) == [slot |-> r.slot, gen |-> r.gen] \in pub_done_ids
Occupied(s) == operation_state[s] # "free"
RegHeld(s) == Occupied(s) /\ reg_episode[s] >= 1
SameId(r1, r2) == r1.slot = r2.slot /\ r1.gen = r2.gen

PublicationVisible(s) ==
  IF MutAttachIgnoresPublication
    THEN FALSE
    ELSE IF MutAttachTreatsTerminalAsPublished
      THEN terminal_state[s] # "none"
      ELSE operation_state[s] = "published"

Init ==
  /\ operation_state = [s \in Slots |-> "free"]
  /\ terminal_state = [s \in Slots |-> "none"]
  /\ binding_live = [s \in Slots |-> FALSE]
  /\ gen = [s \in Slots |-> 0]
  /\ observer_phase = [s \in Slots |-> "unattached"]
  /\ delivery_state = [s \in Slots |-> "idle"]
  /\ waiter_state = [s \in Slots |-> "absent"]
  /\ observer_registration = [s \in Slots |-> "none"]
  /\ host_record_free = TRUE
  /\ reg_episode = [s \in Slots |-> 0]
  /\ stage_reg = [r \in RegKeys |-> 0]
  /\ delivery_claims = [r \in RegKeys |-> 0]
  /\ delivery_retires = [r \in RegKeys |-> 0]
  /\ stage_op = [i \in Ids |-> 0]
  /\ pub_done_ids = {}
  /\ obs_cancelled = [i \in Ids |-> FALSE]
  /\ op_cancel_requested = [i \in Ids |-> FALSE]
  /\ refused_attach = [i \in Ids |-> FALSE]
  /\ attach_in_window = [r \in RegKeys |-> FALSE]
  /\ suppressed_queued = {}
  /\ saw_in_progress = FALSE
  /\ arms_per_id = [i \in Ids |-> 0]
  /\ rearmed = FALSE
  /\ waiter_left = [r \in RegKeys |-> "none"]

(*******************************************************************)
(* Operation side: the request owner                               *)
(*******************************************************************)

Accept(s) ==
  /\ operation_state[s] = "free"
  /\ gen[s] < GenMax
  /\ operation_state' = [operation_state EXCEPT ![s] = "accepted"]
  /\ binding_live' = [binding_live EXCEPT ![s] = TRUE]
  /\ terminal_state' = [terminal_state EXCEPT ![s] = "none"]
  /\ observer_phase' = [observer_phase EXCEPT ![s] = "unattached"]
  /\ delivery_state' = [delivery_state EXCEPT ![s] = "idle"]
  /\ observer_registration' = [observer_registration EXCEPT ![s] = "none"]
  /\ reg_episode' = [reg_episode EXCEPT ![s] = 0]
  /\ stage_op' = [stage_op EXCEPT ![Id(s)] = 1]
  /\ UNCHANGED <<gen, waiter_state, host_record_free, stage_reg,
                 delivery_claims, delivery_retires, pub_done_ids,
                 obs_cancelled, op_cancel_requested, refused_attach,
                 attach_in_window, suppressed_queued, saw_in_progress,
                 arms_per_id, rearmed, waiter_left>>

SettleTerminal(s) ==
  /\ operation_state[s] = "accepted"
  /\ terminal_state[s] = "none"
  /\ \E v \in {"ok", "err"} :
       terminal_state' = [terminal_state EXCEPT ![s] = v]
  /\ operation_state' = [operation_state EXCEPT ![s] = "terminal"]
  /\ stage_op' = [stage_op EXCEPT ![Id(s)] = 2]
  /\ UNCHANGED <<binding_live, gen, observer_phase, delivery_state,
                 waiter_state, observer_registration, host_record_free,
                 reg_episode, stage_reg, delivery_claims,
                 delivery_retires, pub_done_ids, obs_cancelled,
                 op_cancel_requested, refused_attach, attach_in_window,
                 suppressed_queued, saw_in_progress, arms_per_id,
                 rearmed, waiter_left>>

CancelRequest(s) ==
  /\ operation_state[s] = "accepted"
  /\ terminal_state[s] = "none"
  /\ operation_state' = [operation_state EXCEPT ![s] = "terminal"]
  /\ terminal_state' = [terminal_state EXCEPT ![s] = "canceled"]
  /\ op_cancel_requested' = [op_cancel_requested EXCEPT ![Id(s)] = TRUE]
  /\ stage_op' = [stage_op EXCEPT ![Id(s)] = 2]
  /\ UNCHANGED <<binding_live, gen, observer_phase, delivery_state,
                 waiter_state, observer_registration, host_record_free,
                 reg_episode, stage_reg, delivery_claims,
                 delivery_retires, pub_done_ids, obs_cancelled,
                 refused_attach, attach_in_window, suppressed_queued,
                 saw_in_progress, arms_per_id, rearmed, waiter_left>>

ReleaseBinding(s) ==
  /\ binding_live[s]
  /\ operation_state[s] \in {"publishing", "published"}
  /\ binding_live' = [binding_live EXCEPT ![s] = FALSE]
  /\ UNCHANGED <<operation_state, terminal_state, gen, observer_phase,
                 delivery_state, waiter_state, observer_registration,
                 host_record_free, reg_episode, stage_reg,
                 delivery_claims, delivery_retires, stage_op,
                 pub_done_ids, obs_cancelled, op_cancel_requested,
                 refused_attach, attach_in_window, suppressed_queued,
                 saw_in_progress, arms_per_id, rearmed, waiter_left>>

(*******************************************************************)
(* Driver side: the fixed progress owner                           *)
(*******************************************************************)

BeginPublish(s) ==
  /\ operation_state[s] = "terminal"
  /\ binding_live[s]
  /\ operation_state' = [operation_state EXCEPT ![s] = "publishing"]
  /\ UNCHANGED <<terminal_state, binding_live, gen, observer_phase,
                 delivery_state, waiter_state, observer_registration,
                 host_record_free, reg_episode, stage_reg,
                 delivery_claims, delivery_retires, stage_op,
                 pub_done_ids, obs_cancelled, op_cancel_requested,
                 refused_attach, attach_in_window, suppressed_queued,
                 saw_in_progress, arms_per_id, rearmed, waiter_left>>

CompletePublish(s) ==
  /\ operation_state[s] = "publishing"
  /\ operation_state' = [operation_state EXCEPT ![s] = "published"]
  /\ pub_done_ids' = pub_done_ids \cup {Id(s)}
  /\ stage_op' = [stage_op EXCEPT ![Id(s)] =
                    IF stage_op[Id(s)] < 3 THEN 3 ELSE stage_op[Id(s)]]
  /\ IF observer_phase[s] = "armed" /\ ~MutPublicationSkipsQueue
       THEN /\ observer_phase' = [observer_phase EXCEPT ![s] = "queued"]
            /\ delivery_state' = [delivery_state EXCEPT ![s] = "owed"]
            /\ stage_reg' = [stage_reg EXCEPT ![CurReg(s)] = 2]
       ELSE UNCHANGED <<observer_phase, delivery_state, stage_reg>>
  /\ UNCHANGED <<terminal_state, binding_live, gen, waiter_state,
                 observer_registration, host_record_free, reg_episode,
                 delivery_claims, delivery_retires, obs_cancelled,
                 op_cancel_requested, refused_attach, attach_in_window,
                 suppressed_queued, saw_in_progress, arms_per_id,
                 rearmed, waiter_left>>

ClaimDelivery(s) ==
  /\ Occupied(s)
  /\ IF MutDeliveryClaimUnbounded
       THEN observer_phase[s] \in {"armed", "queued", "delivering"}
       ELSE observer_phase[s] = "queued"
  /\ observer_phase' = [observer_phase EXCEPT ![s] = "delivering"]
  /\ delivery_state' = [delivery_state EXCEPT ![s] = "claimed"]
  /\ delivery_claims' = [delivery_claims EXCEPT ![CurReg(s)] =
                           delivery_claims[CurReg(s)] + 1]
  /\ stage_reg' = [stage_reg EXCEPT ![CurReg(s)] = 3]
  /\ UNCHANGED <<operation_state, terminal_state, binding_live, gen,
                 waiter_state, observer_registration, host_record_free,
                 reg_episode, delivery_retires, stage_op, pub_done_ids,
                 obs_cancelled, op_cancel_requested, refused_attach,
                 attach_in_window, suppressed_queued, saw_in_progress,
                 arms_per_id, rearmed, waiter_left>>

(*******************************************************************)
(* Host adapter: the RuntimeTaskContext waiter path                *)
(*******************************************************************)

WaiterArrive(s) ==
  /\ host_record_free
  /\ waiter_state[s] = "absent"
  /\ IF ~Occupied(s) \/ ~binding_live[s]
       THEN /\ observer_registration' =
                [observer_registration EXCEPT ![s] = "not_found"]
            /\ refused_attach' =
                 IF Occupied(s)
                   THEN [refused_attach EXCEPT ![Id(s)] = TRUE]
                   ELSE refused_attach
            /\ UNCHANGED <<operation_state, terminal_state,
                           binding_live, gen, observer_phase,
                           delivery_state, waiter_state,
                           host_record_free, reg_episode, stage_reg,
                           delivery_claims, delivery_retires, stage_op,
                           pub_done_ids, obs_cancelled,
                           op_cancel_requested, attach_in_window,
                           suppressed_queued, saw_in_progress,
                           arms_per_id, rearmed, waiter_left>>
       ELSE IF observer_phase[s] \notin {"unattached", "retired"}
       THEN /\ observer_registration' =
                [observer_registration EXCEPT ![s] = "duplicate"]
            /\ refused_attach' = [refused_attach EXCEPT ![Id(s)] = TRUE]
            /\ UNCHANGED <<operation_state, terminal_state,
                           binding_live, gen, observer_phase,
                           delivery_state, waiter_state,
                           host_record_free, reg_episode, stage_reg,
                           delivery_claims, delivery_retires, stage_op,
                           pub_done_ids, obs_cancelled,
                           op_cancel_requested, attach_in_window,
                           suppressed_queued, saw_in_progress,
                           arms_per_id, rearmed, waiter_left>>
       ELSE IF PublicationVisible(s)
       THEN /\ observer_registration' =
                [observer_registration EXCEPT ![s] = "already_terminal"]
            /\ refused_attach' = [refused_attach EXCEPT ![Id(s)] = TRUE]
            /\ UNCHANGED <<operation_state, terminal_state,
                           binding_live, gen, observer_phase,
                           delivery_state, waiter_state,
                           host_record_free, reg_episode, stage_reg,
                           delivery_claims, delivery_retires, stage_op,
                           pub_done_ids, obs_cancelled,
                           op_cancel_requested, attach_in_window,
                           suppressed_queued, saw_in_progress,
                           arms_per_id, rearmed, waiter_left>>
       ELSE /\ reg_episode[s] < MaxEp
            /\ observer_phase' = [observer_phase EXCEPT ![s] = "armed"]
            /\ delivery_state' = [delivery_state EXCEPT ![s] = "idle"]
            /\ waiter_state' = [waiter_state EXCEPT ![s] = "registered"]
            /\ observer_registration' =
                 [observer_registration EXCEPT ![s] = "armed"]
            /\ host_record_free' = FALSE
            /\ reg_episode' =
                 [reg_episode EXCEPT ![s] = reg_episode[s] + 1]
            /\ stage_reg' = [stage_reg EXCEPT ![NewReg(s)] = 1]
            /\ attach_in_window' =
                 [attach_in_window EXCEPT ![NewReg(s)] =
                    (terminal_state[s] # "none" /\
                     operation_state[s] # "published")]
            /\ arms_per_id' =
                 [arms_per_id EXCEPT ![Id(s)] = arms_per_id[Id(s)] + 1]
            /\ rearmed' = (rearmed \/ reg_episode[s] >= 1)
            /\ UNCHANGED <<operation_state, terminal_state,
                           binding_live, gen, delivery_claims,
                           delivery_retires, stage_op, pub_done_ids,
                           obs_cancelled, op_cancel_requested,
                           refused_attach, suppressed_queued,
                           saw_in_progress, waiter_left>>

WaiterRefused(s) ==
  /\ Occupied(s)
  /\ ~host_record_free
  /\ observer_registration' =
       [observer_registration EXCEPT ![s] = "no_space"]
  /\ refused_attach' = [refused_attach EXCEPT ![Id(s)] = TRUE]
  /\ UNCHANGED <<operation_state, terminal_state, binding_live, gen,
                 observer_phase, delivery_state, waiter_state,
                 host_record_free, reg_episode, stage_reg,
                 delivery_claims, delivery_retires, stage_op,
                 pub_done_ids, obs_cancelled, op_cancel_requested,
                 attach_in_window, suppressed_queued, saw_in_progress,
                 arms_per_id, rearmed, waiter_left>>

CancelRegistration(s) ==
  /\ Occupied(s)
  /\ IF observer_phase[s] \in {"armed", "queued"}
       THEN /\ observer_phase' =
                [observer_phase EXCEPT ![s] = "retired"]
            /\ delivery_state' = [delivery_state EXCEPT ![s] = "retired"]
            /\ stage_reg' = [stage_reg EXCEPT ![CurReg(s)] = 4]
            /\ suppressed_queued' =
                 IF observer_phase[s] = "queued"
                   THEN suppressed_queued \cup {CurReg(s)}
                   ELSE suppressed_queued
            /\ obs_cancelled' =
                 [obs_cancelled EXCEPT ![Id(s)] = TRUE]
            /\ IF MutObsCancelCancelsOp /\ terminal_state[s] = "none"
                 THEN terminal_state' =
                        [terminal_state EXCEPT ![s] = "canceled"]
                 ELSE UNCHANGED terminal_state
            /\ IF waiter_state[s] = "registered"
                 THEN /\ waiter_state' =
                        [waiter_state EXCEPT ![s] = "absent"]
                      /\ host_record_free' = TRUE
                      /\ waiter_left' =
                           [waiter_left EXCEPT ![CurReg(s)] = "canceled"]
                 ELSE UNCHANGED <<waiter_state, host_record_free,
                                  waiter_left>>
            /\ UNCHANGED <<operation_state, binding_live, gen,
                           observer_registration, reg_episode,
                           delivery_claims, delivery_retires, stage_op,
                           pub_done_ids, op_cancel_requested,
                           refused_attach, attach_in_window,
                           saw_in_progress, arms_per_id, rearmed>>
       ELSE IF observer_phase[s] = "delivering" /\ ~MutCancelDuringDeliveryReturns
       THEN /\ observer_registration' =
                [observer_registration EXCEPT ![s] = "in_progress"]
            /\ saw_in_progress' = TRUE
            /\ UNCHANGED <<operation_state, terminal_state,
                           binding_live, gen, observer_phase,
                           delivery_state, waiter_state,
                           host_record_free, reg_episode, stage_reg,
                           delivery_claims, delivery_retires, stage_op,
                           pub_done_ids, obs_cancelled,
                           op_cancel_requested, refused_attach,
                           attach_in_window, suppressed_queued,
                           arms_per_id, rearmed, waiter_left>>
       ELSE IF observer_phase[s] = "delivering"
       THEN /\ observer_phase' =
                [observer_phase EXCEPT ![s] = "retired"]
            /\ delivery_state' = [delivery_state EXCEPT ![s] = "retired"]
            /\ stage_reg' = [stage_reg EXCEPT ![CurReg(s)] = 4]
            /\ obs_cancelled' =
                 [obs_cancelled EXCEPT ![Id(s)] = TRUE]
            /\ IF waiter_state[s] = "registered"
                 THEN /\ waiter_state' =
                        [waiter_state EXCEPT ![s] = "absent"]
                      /\ host_record_free' = TRUE
                      /\ waiter_left' =
                           [waiter_left EXCEPT ![CurReg(s)] = "canceled"]
                 ELSE UNCHANGED <<waiter_state, host_record_free,
                                  waiter_left>>
            /\ UNCHANGED <<operation_state, terminal_state,
                           binding_live, gen, observer_registration,
                           reg_episode, delivery_claims,
                           delivery_retires, stage_op, pub_done_ids,
                           op_cancel_requested, refused_attach,
                           attach_in_window, suppressed_queued,
                           saw_in_progress, arms_per_id, rearmed>>
       ELSE /\ observer_registration' =
                [observer_registration EXCEPT ![s] = "not_registered"]
            /\ UNCHANGED <<operation_state, terminal_state,
                           binding_live, gen, observer_phase,
                           delivery_state, waiter_state,
                           host_record_free, reg_episode, stage_reg,
                           delivery_claims, delivery_retires, stage_op,
                           pub_done_ids, obs_cancelled,
                           op_cancel_requested, refused_attach,
                           attach_in_window, suppressed_queued,
                           saw_in_progress, arms_per_id, rearmed,
                           waiter_left>>

RouteDelivery(s) ==
  /\ observer_phase[s] = "delivering"
  /\ delivery_state[s] = "claimed"
  /\ waiter_state[s] = "registered"
  /\ delivery_state' = [delivery_state EXCEPT ![s] = "routed"]
  /\ waiter_state' = [waiter_state EXCEPT ![s] = "delivered"]
  /\ UNCHANGED <<operation_state, terminal_state, binding_live, gen,
                 observer_phase, observer_registration, host_record_free,
                 reg_episode, stage_reg, delivery_claims,
                 delivery_retires, stage_op, pub_done_ids, obs_cancelled,
                 op_cancel_requested, refused_attach, attach_in_window,
                 suppressed_queued, saw_in_progress, arms_per_id,
                 rearmed, waiter_left>>

ProcessDelivery(s) ==
  /\ delivery_state[s] = "routed"
  /\ ~MutNoDeliveryWake
  /\ delivery_state' = [delivery_state EXCEPT ![s] = "retired"]
  /\ IF observer_phase[s] = "delivering"
       THEN /\ observer_phase' =
                [observer_phase EXCEPT ![s] = "retired"]
            /\ delivery_retires' =
                 [delivery_retires EXCEPT ![CurReg(s)] =
                    delivery_retires[CurReg(s)] + 1]
            /\ stage_reg' = [stage_reg EXCEPT ![CurReg(s)] = 4]
       ELSE UNCHANGED <<observer_phase, delivery_retires, stage_reg>>
  /\ waiter_state' = [waiter_state EXCEPT ![s] = "absent"]
  /\ host_record_free' = TRUE
  /\ waiter_left' = [waiter_left EXCEPT ![CurReg(s)] = "completed"]
  /\ UNCHANGED <<operation_state, terminal_state, binding_live, gen,
                 observer_registration, reg_episode, delivery_claims,
                 stage_op, pub_done_ids, obs_cancelled,
                 op_cancel_requested, refused_attach, attach_in_window,
                 suppressed_queued, saw_in_progress, arms_per_id,
                 rearmed>>

(*******************************************************************)
(* Core reclaim service                                            *)
(*******************************************************************)

Reclaim(s) ==
  /\ operation_state[s] = "published"
  /\ ~binding_live[s]
  /\ gen[s] < GenMax
  /\ MutReclaimIgnoresObserver \/
     observer_phase[s] \in {"unattached", "retired"}
  /\ operation_state' = [operation_state EXCEPT ![s] = "free"]
  /\ binding_live' = [binding_live EXCEPT ![s] = FALSE]
  /\ terminal_state' = [terminal_state EXCEPT ![s] = "none"]
  /\ observer_phase' = [observer_phase EXCEPT ![s] = "unattached"]
  /\ delivery_state' = [delivery_state EXCEPT ![s] = "idle"]
  /\ observer_registration' = [observer_registration EXCEPT ![s] = "none"]
  /\ reg_episode' = [reg_episode EXCEPT ![s] = 0]
  /\ gen' = [gen EXCEPT ![s] = gen[s] + 1]
  /\ UNCHANGED <<waiter_state, host_record_free, stage_reg,
                 delivery_claims, delivery_retires, stage_op,
                 pub_done_ids, obs_cancelled, op_cancel_requested,
                 refused_attach, attach_in_window, suppressed_queued,
                 saw_in_progress, arms_per_id, rearmed, waiter_left>>

SlotAction(s) ==
  \/ Accept(s)
  \/ SettleTerminal(s)
  \/ CancelRequest(s)
  \/ ReleaseBinding(s)
  \/ BeginPublish(s)
  \/ CompletePublish(s)
  \/ ClaimDelivery(s)
  \/ WaiterArrive(s)
  \/ WaiterRefused(s)
  \/ CancelRegistration(s)
  \/ RouteDelivery(s)
  \/ ProcessDelivery(s)
  \/ Reclaim(s)

Next == \E s \in Slots : SlotAction(s)

Spec == Init /\ [][Next]_vars

SettleTerminalAny == \E s \in Slots : SettleTerminal(s)
BeginPublishAny == \E s \in Slots : BeginPublish(s)
CompletePublishAny == \E s \in Slots : CompletePublish(s)
ClaimDeliveryAny == \E s \in Slots : ClaimDelivery(s)
RouteDeliveryAny == \E s \in Slots : RouteDelivery(s)
ProcessDeliveryAny == \E s \in Slots : ProcessDelivery(s)
ReclaimAny == \E s \in Slots : Reclaim(s)

Fair ==
  /\ WF_vars(SettleTerminalAny)
  /\ WF_vars(BeginPublishAny)
  /\ WF_vars(CompletePublishAny)
  /\ WF_vars(ClaimDeliveryAny)
  /\ WF_vars(RouteDeliveryAny)
  /\ WF_vars(ProcessDeliveryAny)
  /\ WF_vars(ReclaimAny)

FairSpec == Spec /\ Fair

(*******************************************************************)
(* Safety                                                          *)
(*******************************************************************)

TypeOK ==
  /\ operation_state \in [Slots -> Ops]
  /\ terminal_state \in [Slots -> Terms]
  /\ binding_live \in [Slots -> BOOLEAN]
  /\ gen \in [Slots -> 0..GenMax]
  /\ observer_phase \in [Slots -> Phases]
  /\ delivery_state \in [Slots -> Deliveries]
  /\ waiter_state \in [Slots -> Waiters]
  /\ observer_registration \in [Slots -> Dispositions]
  /\ host_record_free \in BOOLEAN
  /\ reg_episode \in [Slots -> 0..MaxEp]
  /\ stage_reg \in [RegKeys -> 0..4]
  /\ delivery_claims \in [RegKeys -> 0..3]
  /\ delivery_retires \in [RegKeys -> 0..1]
  /\ stage_op \in [Ids -> 0..3]
  /\ pub_done_ids \subseteq Ids
  /\ obs_cancelled \in [Ids -> BOOLEAN]
  /\ op_cancel_requested \in [Ids -> BOOLEAN]
  /\ refused_attach \in [Ids -> BOOLEAN]
  /\ attach_in_window \in [RegKeys -> BOOLEAN]
  /\ suppressed_queued \subseteq RegKeys
  /\ saw_in_progress \in BOOLEAN
  /\ arms_per_id \in [Ids -> 0..MaxEp]
  /\ rearmed \in BOOLEAN
  /\ waiter_left \in [RegKeys -> Lefts]

InvUnoccupiedClean ==
  \A s \in Slots :
    (operation_state[s] = "free") =>
      /\ observer_phase[s] = "unattached"
      /\ delivery_state[s] = "idle"
      /\ waiter_state[s] = "absent"
      /\ terminal_state[s] = "none"
      /\ ~binding_live[s]
      /\ reg_episode[s] = 0

(* S1: observer cancellation never cancels the operation. *)
InvCancelIsolation ==
  \A i \in Ids :
    (obs_cancelled[i] /\ ~op_cancel_requested[i] /\
     gen[i.slot] = i.gen /\ Occupied(i.slot)) =>
       terminal_state[i.slot] # "canceled"

(* OBS-01: attachment resolves atomically with publication. *)
InvArmedRequiresUnpublished ==
  \A s \in Slots :
    (observer_phase[s] = "armed") => operation_state[s] # "published"

InvAlreadyTerminalRequiresPublication ==
  \A s \in Slots :
    (observer_registration[s] = "already_terminal") =>
      operation_state[s] = "published"

(* S2: a retired registration owes and holds no delivery. *)
InvPhaseDeliveryAgreement ==
  \A s \in Slots :
    /\ (observer_phase[s] \in {"unattached", "armed"}) =>
         delivery_state[s] = "idle"
    /\ (observer_phase[s] = "queued") => delivery_state[s] = "owed"
    /\ (observer_phase[s] = "delivering") =>
         delivery_state[s] \in {"claimed", "routed"}
    /\ (observer_phase[s] = "retired") => delivery_state[s] = "retired"

(* #433: retired observers cannot return to active states. *)
InvRetiredStaysRetired ==
  \A r \in RegKeys :
    (stage_reg[r] = 4 /\ gen[r.slot] = r.gen /\
     reg_episode[r.slot] = r.ep) =>
       observer_phase[r.slot] = "retired"

(* S3: single delivery ownership, at most once per registration
   generation; two drivers, two callbacks, two retirements. *)
InvAtMostOnceDelivery ==
  \A r \in RegKeys : delivery_claims[r] =< 1

InvSingleRetirement ==
  \A r \in RegKeys : delivery_retires[r] =< 1

InvSingleOwedEpisode ==
  ~\E r1, r2 \in RegKeys :
    (r1 # r2 /\ SameId(r1, r2) /\
     stage_reg[r1] \in {2, 3} /\ stage_reg[r2] \in {2, 3})

InvDeliveryAfterPublication ==
  \A r \in RegKeys :
    (delivery_claims[r] >= 1) => PubDoneFor(r)

(* OBS-02: a claimed delivery retires only through the delivery
   hook; a cancel acknowledgment is not that retirement. *)
InvRetireOwnership ==
  \A s \in Slots :
    (RegHeld(s) /\ observer_phase[s] = "retired" /\
     delivery_claims[CurReg(s)] = 1) =>
       delivery_retires[CurReg(s)] = 1

(* #433/OBS-03: the live registration pins the slot by full
   identity; reclaim cannot occur before observer retirement.  The
   C++ release_slot_ resets the phase and advances the generation,
   so the pin is only expressible on the episode ledger: a live
   episode (armed/queued/claimed) implies its identity is still the
   occupied one. *)
InvObserverPin ==
  \A r \in RegKeys :
    (stage_reg[r] \in {1, 2, 3}) =>
      (operation_state[r.slot] # "free" /\ gen[r.slot] = r.gen)

(* Host retention: the wait record is held until the wake. *)
InvWaiterBacking ==
  \A s \in Slots :
    (waiter_state[s] # "absent") =>
      /\ ~host_record_free
      /\ observer_phase[s] \in {"armed", "queued", "delivering"}

(*******************************************************************)
(* Coverage witnesses: each InvCov* is the NEGATION of a required  *)
(* race or scenario outcome; its cfg must be VIOLATED, certifying  *)
(* the scenario is reachable and actually explored.                 *)
(*   InvCovAttachRodePublication   attach vs publication, attach won *)
(*   InvCovAlreadyTerminalFastPath attach vs publication, pub won   *)
(*   InvCovWindowAttach            terminal completion vs attach    *)
(*   InvCovCancelLostToClaim       cancel vs delivery claim, claim  *)
(*                                  won (delivery_in_progress seen) *)
(*   InvCovQueuedSuppression       cancel vs delivery claim, cancel *)
(*                                  won out of queued               *)
(*   InvCovDeliveryRetired         observer retirement vs delivery  *)
(*                                  (hook return after claim)       *)
(*   InvCovNoObserverSettlement    S4: settlement with no observer  *)
(*   InvCovPinnedPastRelease       OBS-03: delivery pinned past     *)
(*                                  binding release                 *)
(*   InvCovReArm                   registration renewal after cancel *)
(*******************************************************************)

InvCovAttachRodePublication ==
  ~\E r \in RegKeys : stage_reg[r] = 2

InvCovAlreadyTerminalFastPath ==
  ~\E s \in Slots : observer_registration[s] = "already_terminal"

InvCovWindowAttach ==
  ~\E r \in RegKeys : attach_in_window[r]

InvCovCancelLostToClaim ==
  ~saw_in_progress

InvCovQueuedSuppression ==
  suppressed_queued = {}

InvCovDeliveryRetired ==
  \A r \in RegKeys : delivery_retires[r] = 0

InvCovNoObserverSettlement ==
  ~\E i \in Ids :
    (i \in pub_done_ids /\ arms_per_id[i] = 0)

InvCovPinnedPastRelease ==
  ~\E s \in Slots :
    (observer_phase[s] \in {"queued", "delivering"} /\ ~binding_live[s])

InvCovReArm ==
  ~rearmed

(*******************************************************************)
(* Conditional liveness, by full identity and registration episode *)
(*******************************************************************)

WaiterOn(r) ==
  /\ waiter_state[r.slot] # "absent"
  /\ gen[r.slot] = r.gen
  /\ reg_episode[r.slot] = r.ep
  /\ waiter_left[r] = "none"

L0 ==
  \A i \in Ids :
    [](stage_op[i] = 1 => <>(stage_op[i] >= 3))

L1 ==
  \A r \in RegKeys :
    []( (stage_reg[r] = 1 /\ PubDoneFor(r)) => <>(stage_reg[r] = 4) )

L2 ==
  \A r \in RegKeys :
    []( (WaiterOn(r) /\ PubDoneFor(r)) => <>(waiter_left[r] # "none") )

L4 ==
  \A i \in Ids :
    []( (stage_op[i] = 1 /\ refused_attach[i]) => <>(stage_op[i] >= 3) )

=============================================================================
