------------------------------- MODULE E8OwnershipTransferBuggyOwner -------------------------------
(*
  Deliberately BROKEN variant of the E8 ownership-transfer protocol.

  Defect (Model A — ticket-only steal): StealRunnable moves the ticket
  W0Local -> W1Local but does NOT transfer owner. owner[F] stays W0.

  Causal chain the defect admits (the E8 spec §7 required counterexample):
    F owned by W0
    F runnable on W0 (ticket W0Local)
    W1 steals F : ticket -> W1Local, owner stays W0
    W1 pops F    : PopRunnable requires owner[f]=w; under the buggy model
                   we relax PopRunnable to NOT check owner (the bug
                   compounds: production would also need to pop without
                   owner check, or the steal is a no-op). See PopRunnableBuggy.
    F runs on W1, suspends : waitReg set; owner still W0.
    F becomes ready, WakeReady fires : publishes ticket to LocalOf(owner[f])
                   = LocalOf(W0) = W0Local.  <-- STALE OWNER ROUTE.
    F is now runnable on W0's local queue, but W1 is the worker that
    suspended it and is waiting to resume it. The ticket is on the wrong
    worker.

  TLC MUST find a counterexample to InvLocalMatchesOwner (and/or to a
  "wake routes to current execution owner" property) whose causal trace
  is semantically equivalent to:
    F owned W0 -> stolen to W1 without owner transfer -> runs W1 ->
    waits -> wakes -> stale owner says W0 -> routed back to W0.

  This is the load-bearing E8 proof: the correct model (E8OwnershipTransfer)
  PASSES InvLocalMatchesOwner; this buggy model FAILS it.
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, Workers, W0, W1, F0, F1

VARIABLES fiberState, ticketLocation, waitReg, owner

FiberState == {"Created", "Waiting", "Runnable", "Running", "Done"}
TicketLoc  == {"None", "PendingSpawn", "W0Local", "W1Local", "W0Inbox", "W1Inbox"}

LocalOf(w) == CASE w = W0 -> "W0Local"
                    [] w = W1 -> "W1Local"

InboxOf(w) == CASE w = W0 -> "W0Inbox"
                    [] w = W1 -> "W1Inbox"

Other(w) == IF w = W0 THEN W1 ELSE W0

SpawnPublish(f) ==
    /\ fiberState[f] = "Created"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "PendingSpawn"]
    /\ owner' = [owner EXCEPT ![f] = W0]
    /\ UNCHANGED waitReg

MovePendingToOwnerLocal(f) ==
    /\ ticketLocation[f] = "PendingSpawn"
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED <<fiberState, waitReg, owner>>

SuspendFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ waitReg[f] = "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "K"]
    /\ UNCHANGED <<ticketLocation, owner>>

WakeReady(f) ==
    /\ fiberState[f] = "Waiting"
    /\ waitReg[f] # "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "None"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED owner

MoveInboxToLocal(f) ==
    /\ ticketLocation[f] = InboxOf(owner[f])
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(owner[f])]
    /\ UNCHANGED <<fiberState, waitReg, owner>>

(* BUGGY Pop: does NOT require owner[f] = w. A thief must be able to pop
   the stolen ticket whose owner record still points at the victim; this
   is the necessary compounding defect for Model A to even run the stolen
   fiber. In the correct model PopRunnable requires owner[f]=w, which
   would make a non-transferring steal a dead no-op (the thief could
   never pop). The bug is precisely that production would relax this. *)
PopRunnableBuggy(w, f) ==
    /\ fiberState[f] = "Runnable"
    /\ ticketLocation[f] = LocalOf(w)
    \* NO owner check -- this is the bug.
    /\ fiberState' = [fiberState EXCEPT ![f] = "Running"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "None"]
    /\ UNCHANGED <<waitReg, owner>>

(* THE BROKEN ACTION: ticket moves, owner does NOT transfer. Model A. *)
StealRunnableBuggy(victim, thief, f) ==
    /\ victim # thief
    /\ fiberState[f] = "Runnable"
    /\ owner[f] = victim
    /\ ticketLocation[f] = LocalOf(victim)
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(thief)]
    /\ owner' = owner   \* <-- BUG: owner unchanged (Model A)
    /\ UNCHANGED <<fiberState, waitReg>>

FinishFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ ticketLocation[f] = "None"
    /\ waitReg[f] = "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Done"]
    /\ UNCHANGED <<ticketLocation, waitReg, owner>>

Stutter ==
    /\ UNCHANGED <<fiberState, ticketLocation, waitReg, owner>>

Next ==
    \/ Stutter
    \/ \E f \in Fibers : SpawnPublish(f)
    \/ \E f \in Fibers : MovePendingToOwnerLocal(f)
    \/ \E f \in Fibers : SuspendFiber(f)
    \/ \E f \in Fibers : WakeReady(f)
    \/ \E f \in Fibers : MoveInboxToLocal(f)
    \/ \E w \in Workers, f \in Fibers : PopRunnableBuggy(w, f)
    \/ \E victim \in Workers, thief \in Workers, f \in Fibers :
        StealRunnableBuggy(victim, thief, f)
    \/ \E f \in Fibers : FinishFiber(f)

Init ==
    /\ fiberState = [f \in Fibers |-> "Created"]
    /\ ticketLocation = [f \in Fibers |-> "None"]
    /\ waitReg = [f \in Fibers |-> "None"]
    /\ owner = [f \in Fibers |-> W0]

Spec == Init /\ [][Next]_<<fiberState, ticketLocation, waitReg, owner>>

(* The load-bearing invariant the buggy model must violate:
   after a steal->run->suspend->wake cycle, the new runnable ticket must
   be on the CURRENT execution owner's queue. Under Model A the wake
   routes to the stale owner (victim), so the ticket lands on the
   victim's local queue while the thief is the execution owner.

   We check BOTH:
     - InvLocalMatchesOwner (ticket on local queue => owner matches)
     - InvWakeRoutesToExecutionOwner (a Reachable-state property: there
       exists a state where a fiber is Runnable with its ticket on W1Local
       but owner=W0, OR a fiber was stolen to W1, ran on W1, suspended,
       and woke to W0Local).

   InvLocalMatchesOwner is the direct state invariant; the buggy model
   admits a state where ticketLocation[f]=W1Local and owner[f]=W0 (right
   after the buggy steal), which already violates it. The richer causal
   chain (steal->run->suspend->wake->stale-route) is the semantic proof
   that the violation is the stale-owner defect, not an unrelated one. *)

InvLocalMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Local" => owner[f] = W0)
        /\ (ticketLocation[f] = "W1Local" => owner[f] = W1)

InvInboxMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Inbox" => owner[f] = W0)
        /\ (ticketLocation[f] = "W1Inbox" => owner[f] = W1)

(* The full causal chain is reachable in the state graph (the buggy model
   has no dead-end before wake): from the InvLocalMatchesOwner violation at
   depth 4 (post-steal: ticket=W1Local, owner=W0), PopRunnableBuggy(W1) ->
   SuspendFiber -> WakeReady reaches a state with ticketLocation=W0Local,
   owner=W0, fiberState=Runnable, waitReg=None -- the stale-route. TLC finds
   the shallower violation first; both are the same defect class. *)

Inv ==
    /\ InvLocalMatchesOwner
    /\ InvInboxMatchesOwner
=====
