------------------------------- MODULE E7Publication -------------------------------
(*
  E7 pinned-Fiber runnable-publication protocol (sluice-CORE-E7).

  Narrow TLA+ model of the abstract scheduling protocol whose correctness is
  load-bearing for the proven E7-T2 duplicate-publication defect. Models
  ONLY the state relevant to runnable publication / transport / consumption.
  Does NOT model AsyncBackend internals, context-switch asm, stack layout,
  ThreadPool syscalls, or performance. Fiber migration is E8 (out of scope).

  Domain (finite, exhaustive TLC):
    Fibers    = {F0, F1, F2}
    Workers   = {W0, W1}
    WaitKeys  = {K0, K1}

  ticketLocation[f] is the SINGLE abstract runnable publication token for a
  Fiber. The intended protocol is linear:
      no ticket -> publication creates one -> transport moves it -> pop consumes
  A transport action must NOT create another publication.

  AUDIT #162 MODEL-005 resolution (2026-08-22, issue #170): the former
  W*Inbox tier and MoveInboxToLocal — compatibility states of the staged
  E7-A/E7-B routed-inbox design that production never populated (Init
  assigned no W*Inbox location, no producer ever created one, and the
  transport hop never fired in the checked graph) — are REMOVED. As-built
  is a SINGLE owner-local queue: WorkerState::local_runnable, whose
  cross-worker publication is serialized by the target worker's inbox_mtx
  (kept; the name is historical), and the dead WorkerState::inbox storage
  + notify-only inbox_cv were deleted from production in #170. The model
  now matches the as-built ticket locations exactly (the C++ inbox deque
  and CV no longer exist to diverge from). E7-T2 is unaffected: the
  duplicate-publication defect lives in WakeReady, not in transport.
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, Workers, WaitKeys, W0, W1, F0, F1, F2, K0, K1

VARIABLES
    fiberState,        \* [Fibers -> FiberState]
    ticketLocation,    \* [Fibers -> TicketLoc]  -- the ONE abstract runnable token
    waitReg,           \* [Fibers -> WaitKey | None]  -- active wait registration
    owner              \* [Fibers -> Workers]    -- pinned owning worker (immutable per fiber)

FiberState == {"Created", "Waiting", "Runnable", "Running", "Done"}
TicketLoc  == {"None", "PendingSpawn", "W0Local", "W1Local"}
WaitKeyVal == WaitKeys \cup {"None"}

ASSUME
    /\ Fibers # {}
    /\ Workers # {}
    /\ WaitKeys # {}

(* =========================================================================
   Helper operators
   ========================================================================= *)

LocalOf(w) == CASE w = W0 -> "W0Local"
                    [] w = W1 -> "W1Local"

TicketLive(f) == ticketLocation[f] # "None"
TicketFree(f) == ticketLocation[f] = "None"
RegFree(f)    == waitReg[f] = "None"

(* =========================================================================
   Actions
   ========================================================================= *)

(* PUBLISH: spawn. created -> runnable, one ticket to pending_spawn_. *)
SpawnPublish(f) ==
    /\ fiberState[f] = "Created"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "PendingSpawn"]
    /\ UNCHANGED waitReg
    /\ UNCHANGED owner

(* STATE_ONLY (atomic): a Running fiber registers a wait AND suspends.
   Abstracts the production await_* sequence (register-under-lock, double-
   check, make_waiting, switch away) as one linearized transition: Running
   + unregistered -> Waiting + registered. The transient "Running +
   registered" window is collapsed at this abstraction boundary; it is
   documented in the refinement map. *)
SuspendFiber(f, key) ==
    /\ fiberState[f] = "Running"
    /\ waitReg[f] = "None"
    /\ key \in WaitKeys
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ waitReg' = [waitReg EXCEPT ![f] = key]
    /\ UNCHANGED <<ticketLocation, owner>>

(* PUBLISH: wake a waiting fiber. Waiting -> Runnable; exactly ONE ticket
   published to the owner's local queue. Publication is CONDITIONAL on
   owning the Waiting -> Runnable transition. (E7-T2 fix refines this.) *)
WakeReady(f) ==
    /\ fiberState[f] = "Waiting"
    /\ waitReg[f] # "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "None"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED owner

(* MOVE: pending_spawn -> owner local (run() distribute). Cardinality unchanged. *)
MovePendingToOwnerLocal(f) ==
    /\ ticketLocation[f] = "PendingSpawn"
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED <<fiberState, waitReg, owner>>

(* CONSUME: pop a runnable ticket; Runnable -> Running; ticket consumed. *)
PopRunnable(f) ==
    /\ fiberState[f] = "Runnable"
    /\ ticketLocation[f] = LocalOf(owner[f])
    /\ fiberState' = [fiberState EXCEPT ![f] = "Running"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "None"]
    /\ UNCHANGED <<waitReg, owner>>

(* STATE_ONLY: finish. Running -> Done. Precondition: no ticket, no reg.
   This makes Inv4 hold WITHOUT a cleanup transition. *)
FinishFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ ticketLocation[f] = "None"
    /\ waitReg[f] = "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Done"]
    /\ UNCHANGED <<ticketLocation, waitReg, owner>>

(* Stutter action: always enabled. Models "the coordinated run has ended or
   is idle" so the state graph has no spurious deadlock; safety invariants
   are checked across ALL reachable states (including terminal ones). *)
Stutter ==
    /\ UNCHANGED <<fiberState, ticketLocation, waitReg, owner>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

Next ==
    \/ Stutter
    \/ \E f \in Fibers :
        \/ SpawnPublish(f)
        \/ \E key \in WaitKeys : SuspendFiber(f, key)
        \/ WakeReady(f)
        \/ MovePendingToOwnerLocal(f)
        \/ PopRunnable(f)
        \/ FinishFiber(f)

InitOwner ==
    [ f \in Fibers |->
        IF f = F0 THEN W0
        ELSE IF f = F1 THEN W1
        ELSE W0 ]

Init ==
    /\ fiberState = [f \in Fibers |-> "Created"]
    /\ ticketLocation = [f \in Fibers |-> "None"]
    /\ waitReg = [f \in Fibers |-> "None"]
    /\ owner = InitOwner

Spec == Init /\ [][Next]_<<fiberState, ticketLocation, waitReg, owner>>

(* =========================================================================
   Safety invariants Inv1 - Inv8
   ========================================================================= *)

InvTicketImpliesRunnable ==
    \A f \in Fibers :
        ticketLocation[f] # "None" => fiberState[f] = "Runnable"

InvRunnableHasTicket ==
    \A f \in Fibers :
        fiberState[f] = "Runnable" => ticketLocation[f] # "None"

InvRunningTicketFree ==
    \A f \in Fibers :
        fiberState[f] = "Running" => ticketLocation[f] = "None"

InvDoneDetached ==
    \A f \in Fibers :
        fiberState[f] = "Done"
            => /\ ticketLocation[f] = "None"
               /\ waitReg[f] = "None"

InvWaitingRegistered ==
    \A f \in Fibers :
        fiberState[f] = "Waiting" => waitReg[f] # "None"

InvRegisteredWaiting ==
    \A f \in Fibers :
        waitReg[f] # "None" => fiberState[f] = "Waiting"

InvRunnableUnregistered ==
    \A f \in Fibers :
        fiberState[f] = "Runnable" => waitReg[f] = "None"

(* Inv7: encoded structurally -- no action has the shape
   "Runnable -> Runnable plus publish". WakeReady requires Waiting. *)

InvPinnedLocal ==
    \A f \in Fibers :
        /\ ticketLocation[f] = "W0Local" => owner[f] = W0
        /\ ticketLocation[f] = "W1Local" => owner[f] = W1

Inv ==
    /\ InvTicketImpliesRunnable
    /\ InvRunnableHasTicket
    /\ InvRunningTicketFree
    /\ InvDoneDetached
    /\ InvWaitingRegistered
    /\ InvRegisteredWaiting
    /\ InvRunnableUnregistered
    /\ InvPinnedLocal
=====
