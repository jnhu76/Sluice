------------------------------- MODULE E8OwnershipTransfer -------------------------------
(*
  E8 runnable ownership-transfer / work-stealing protocol (sluice-CORE-E8).

  Narrow TLA+ model of the abstract scheduling protocol whose correctness is
  load-bearing for E8: stealing a Runnable Fiber TRANSFERS its execution
  ownership to the thief, so a subsequent wake routes to the thief, not the
  original owner.

  Extends the E7 runnable-publication vocabulary (docs/spec/e7_publication/
  E7Publication.tla). The E7 model is the CLOSED baseline: one successful
  created|waiting -> runnable transition grants one runnable publication
  capability; transport/consumption move/consume the ticket without
  publishing. E8 adds ONE new action, StealRunnable, which is MOVE + OWNER
  TRANSFER (never PUBLISH).

  Domain (finite, exhaustive TLC): Workers = {W0, W1}, Fibers = {F0, F1}.

  Models ONLY the state relevant to ownership transfer / wake routing:
    fiberState, ticketLocation, waitReg, owner.
  Does NOT model AsyncBackend, context-switch asm, stacks, ThreadPool, io_uring,
  performance, MW-S2 admission (closed by E7MultiWorkerProgress.tla), or
  Chase-Lev deques (E16).
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, Workers, W0, W1, F0, F1

VARIABLES
    fiberState,        \* [Fibers -> FiberState]
    ticketLocation,    \* [Fibers -> TicketLoc]  -- the ONE abstract runnable token
    waitReg,           \* [Fibers -> WaitKey | None]  -- active wait registration
    owner              \* [Fibers -> Workers]    -- CURRENT execution owner (mutable by steal)

FiberState == {"Created", "Waiting", "Runnable", "Running", "Done"}
TicketLoc  == {"None", "PendingSpawn", "W0Local", "W1Local", "W0Inbox", "W1Inbox"}
WaitKeyVal == {"K"} \cup {"None"}

ASSUME
    /\ Fibers # {}
    /\ Workers # {}

(* =========================================================================
   Helpers
   ========================================================================= *)

LocalOf(w) == CASE w = W0 -> "W0Local"
                    [] w = W1 -> "W1Local"

InboxOf(w) == CASE w = W0 -> "W0Inbox"
                    [] w = W1 -> "W1Inbox"

Other(w) == IF w = W0 THEN W1 ELSE W0

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
    /\ owner' = [owner EXCEPT ![f] = W0]   \* initial owner established at spawn
    /\ UNCHANGED waitReg

(* MOVE: pending_spawn -> owner local (run() distribute). *)
MovePendingToOwnerLocal(f) ==
    /\ ticketLocation[f] = "PendingSpawn"
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED <<fiberState, waitReg, owner>>

(* STATE_ONLY (atomic): a Running fiber registers a wait AND suspends.
   Abstracts the production await_* sequence (register-under-lock, double-
   check, make_waiting, switch away) as one linearized transition. *)
SuspendFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ waitReg[f] = "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "K"]
    /\ UNCHANGED <<ticketLocation, owner>>

(* PUBLISH: wake a waiting fiber. Waiting -> Runnable; exactly ONE ticket
   published to the CURRENT owner's local queue. The owner read here is
   the load-bearing one: it MUST reflect any StealRunnable that happened
   since the original spawn. *)
WakeReady(f) ==
    /\ fiberState[f] = "Waiting"
    /\ waitReg[f] # "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "None"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED owner

(* MOVE: owner inbox -> owner local. Cardinality unchanged. Kept for
   vocabulary completeness with E7; production inbox is dead storage
   (E8-0 audit O5/O6) but the model permits the transition. *)
MoveInboxToLocal(f) ==
    /\ ticketLocation[f] = InboxOf(owner[f])
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED <<fiberState, waitReg, owner>>

(* CONSUME: pop a runnable ticket; Runnable -> Running; ticket consumed.
   Requires the ticket be on THIS worker's local queue == the current
   owner's queue (E8-Inv10). *)
PopRunnable(w, f) ==
    /\ fiberState[f] = "Runnable"
    /\ ticketLocation[f] = LocalOf(w)
    /\ owner[f] = w
    /\ fiberState' = [fiberState EXCEPT ![f] = "Running"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "None"]
    /\ UNCHANGED <<waitReg, owner>>

(* =========================================================================
   THE E8 ACTION: StealRunnable.

   MOVE existing ticket + TRANSFER owner. Does NOT call make_runnable
   (no created|waiting -> runnable transition). Does NOT create a second
   ticket. The ticket simply changes local queue and the owner record
   changes to the thief. One abstract transition.

   Preconditions:  Runnable, owner = victim, ticket on victim's local.
   Postconditions: Runnable, owner = thief,  ticket on thief's local.
   ========================================================================= *)
StealRunnable(victim, thief, f) ==
    /\ victim # thief
    /\ fiberState[f] = "Runnable"
    /\ owner[f] = victim
    /\ ticketLocation[f] = LocalOf(victim)
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(thief)]
    /\ owner' = [owner EXCEPT ![f] = thief]
    /\ UNCHANGED <<fiberState, waitReg>>

(* STATE_ONLY: finish. Running -> Done. Precondition: no ticket, no reg. *)
FinishFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ ticketLocation[f] = "None"
    /\ waitReg[f] = "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Done"]
    /\ UNCHANGED <<ticketLocation, waitReg, owner>>

(* Stutter: no spurious deadlock; safety invariants checked across all
   reachable states including terminal ones. *)
Stutter ==
    /\ UNCHANGED <<fiberState, ticketLocation, waitReg, owner>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

Next ==
    \/ Stutter
    \/ \E f \in Fibers : SpawnPublish(f)
    \/ \E f \in Fibers : MovePendingToOwnerLocal(f)
    \/ \E f \in Fibers : SuspendFiber(f)
    \/ \E f \in Fibers : WakeReady(f)
    \/ \E f \in Fibers : MoveInboxToLocal(f)
    \/ \E w \in Workers, f \in Fibers : PopRunnable(w, f)
    \/ \E victim \in Workers, thief \in Workers, f \in Fibers :
        StealRunnable(victim, thief, f)
    \/ \E f \in Fibers : FinishFiber(f)

Init ==
    /\ fiberState = [f \in Fibers |-> "Created"]
    /\ ticketLocation = [f \in Fibers |-> "None"]
    /\ waitReg = [f \in Fibers |-> "None"]
    /\ owner = [f \in Fibers |-> W0]   \* placeholder until SpawnPublish sets it

Spec == Init /\ [][Next]_<<fiberState, ticketLocation, waitReg, owner>>

(* =========================================================================
   E8 safety invariants (E8-Inv1 .. E8-Inv10)
   ========================================================================= *)

(* E8-Inv1: Ticket implies Runnable. *)
InvTicketImpliesRunnable ==
    \A f \in Fibers :
        ticketLocation[f] # "None" => fiberState[f] = "Runnable"

(* E8-Inv2: Runnable owns one ticket (E7 linear-ticket rule). *)
InvRunnableHasTicket ==
    \A f \in Fibers :
        fiberState[f] = "Runnable" => ticketLocation[f] # "None"

(* E8-Inv3: Local ticket matches CURRENT owner (load-bearing after steal). *)
InvLocalMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Local" => owner[f] = W0)
        /\ (ticketLocation[f] = "W1Local" => owner[f] = W1)

(* E8-Inv4: Inbox destination matches CURRENT owner. *)
InvInboxMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Inbox" => owner[f] = W0)
        /\ (ticketLocation[f] = "W1Inbox" => owner[f] = W1)

(* E8-Inv5: Waiting Fibers are not stealable (no ticket; steal disabled). *)
InvWaitingTicketFree ==
    \A f \in Fibers :
        fiberState[f] = "Waiting" => ticketLocation[f] = "None"

(* E8-Inv6: Running Fibers are not stealable (no ticket). *)
InvRunningTicketFree ==
    \A f \in Fibers :
        fiberState[f] = "Running" => ticketLocation[f] = "None"

(* E8-Inv7: Done Fibers are detached. *)
InvDoneDetached ==
    \A f \in Fibers :
        fiberState[f] = "Done"
            => /\ ticketLocation[f] = "None"
               /\ waitReg[f] = "None"

(* E8-Inv8: Steal preserves ticket cardinality. Encoded structurally:
   StealRunnable has NO make_runnable and moves one ticket; it cannot
   produce ticketLocation = None -> NonNone. The companion state invariant
   is that the total number of live tickets is unchanged by StealRunnable
   (it moves a NonNone loc to another NonNone loc). Captured by Inv1+Inv2
   together with StealRunnable's pre/post. No additional formula needed;
   this is documentation. *)

(* E8-Inv9: Wake routes to CURRENT owner. Encoded structurally in WakeReady:
   ticketLocation' = LocalOf(owner[f]). The state-level companion is
   InvLocalMatchesOwner: after WakeReady the new ticket is on the current
   owner's local queue. No separate formula. *)

(* E8-Inv10: Pop executes only current-owner work. Encoded structurally in
   PopRunnable: requires owner[f] = w AND ticketLocation[f] = LocalOf(w).
   State-level companion: InvLocalMatchesOwner. *)

(* Waiting iff registered (E7 baseline, preserved). *)
InvWaitingRegistered ==
    \A f \in Fibers :
        fiberState[f] = "Waiting" => waitReg[f] # "None"
InvRegisteredWaiting ==
    \A f \in Fibers :
        waitReg[f] # "None" => fiberState[f] = "Waiting"

(* Runnable => unregistered (E7 baseline; makes steal safe — a stealable
   runnable fiber has no active wait reg whose owner could be stale). *)
InvRunnableUnregistered ==
    \A f \in Fibers :
        fiberState[f] = "Runnable" => waitReg[f] = "None"

(* E8-Inv-extra: a stolen fiber that suspends on the thief and wakes must
   route to the thief. This is the E8-T3 causal chain. It is a property of
   the *sequence* StealRunnable -> PopRunnable(thief) -> SuspendFiber ->
   WakeReady. The state invariant that closes it is InvLocalMatchesOwner
   evaluated after WakeReady: the new ticket is on LocalOf(owner[f]),
   and owner[f] = thief (set by StealRunnable, unchanged by Pop/Suspend/
   Wake). So InvLocalMatchesOwner is the load-bearing check. *)

Inv ==
    /\ InvTicketImpliesRunnable
    /\ InvRunnableHasTicket
    /\ InvLocalMatchesOwner
    /\ InvInboxMatchesOwner
    /\ InvWaitingTicketFree
    /\ InvRunningTicketFree
    /\ InvDoneDetached
    /\ InvWaitingRegistered
    /\ InvRegisteredWaiting
    /\ InvRunnableUnregistered
=====
