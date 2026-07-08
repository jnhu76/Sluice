------------------------------- MODULE E8OwnershipTransferBuggyOwner -------------------------------
(*
  Deliberately BROKEN variant of the E8 ownership-transfer protocol.

  SINGLE DEFECT (E8-FORMAL-CORRECTIVE): the buggy StealRunnable MOVES the
  runnable ticket W0Local -> W1Local but does NOT transfer the runnable owner
  record. ownerRecord[F] stays W0 while the ticket is on W1's local queue.

  This model is identical to E8OwnershipTransfer in every action EXCEPT
  StealRunnableBuggy's ownerRecord update. In particular:
    - PopRunnable is the SAME as the correct model (requires ownerRecord[f]=w
      AND ticketLocation[f]=LocalOf(w)). There is NO PopRunnableBuggy. The
      earlier compound-defect variant (which also relaxed PopRunnable) has been
      removed -- it was a second, unnecessary bug.

  Why a single defect suffices:
    The defect class is "runnable ticket / runnable owner-record split", NOT
    "stale WaitReg wake route". Production wake routing reads WaitReg.owner
    (waitOwner), NOT fiber_owner_ (ownerRecord); so a stale-route counterexample
    cannot be produced by this model at all (and would not match production).
    The load-bearing property the buggy model must violate is
    InvLocalMatchesOwner: after a steal, the ticket is on W1Local while
    ownerRecord is still W0. PopRunnable cannot fire for W1 (ownerRecord=W0 !=
    W1), so the stolen fiber is stranded -- exactly the E8-T4 "exactly one of
    steal/pop wins" defect. TLC finds this shallow counterexample at depth 4.

  Required counterexample (E8 spec §6):
    ownerRecord[f] = W0
    ticketLocation[f] = W0Local
    BrokenSteal W0 -> W1
    ticketLocation[f] = W1Local
    ownerRecord[f] = W0          \* remains victim
    InvLocalMatchesOwner violated

  TLC MUST find a counterexample to InvLocalMatchesOwner whose causal trace is
  exactly: steal moves the ticket without transferring ownerRecord. This is the
  load-bearing E8 proof: the correct model PASSES InvLocalMatchesOwner; this
  buggy model FAILS it via the single intended defect.
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, Workers, W0, W1, F0, F1, NA

VARIABLES fiberState, ticketLocation, waitReg, ownerRecord, execWorker, waitOwner

FiberState == {"Created", "Waiting", "Runnable", "Running", "Done"}
TicketLoc  == {"None", "PendingSpawn", "W0Local", "W1Local", "W0Inbox", "W1Inbox"}

ASSUME
    /\ Fibers # {}
    /\ Workers # {}
    /\ W0 \in Workers
    /\ W1 \in Workers
    /\ NA \notin Workers
    /\ W0 # W1
    /\ F0 # F1
    /\ F0 \in Fibers
    /\ F1 \in Fibers

LocalOf(w) == CASE w = W0 -> "W0Local"
                    [] w = W1 -> "W1Local"
                    [] OTHER -> "None"

InboxOf(w) == CASE w = W0 -> "W0Inbox"
                    [] w = W1 -> "W1Inbox"
                    [] OTHER -> "None"

TicketLive(f) == ticketLocation[f] # "None"
TicketFree(f) == ticketLocation[f] = "None"
RegFree(f)    == waitReg[f] = "None"

(* ---- All actions below are IDENTICAL to E8OwnershipTransfer except
   StealRunnableBuggy (the single defect) and the module name. ---- *)

SpawnPublish(f) ==
    /\ fiberState[f] = "Created"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "PendingSpawn"]
    /\ ownerRecord' = [ownerRecord EXCEPT ![f] = W0]
    /\ execWorker' = [execWorker EXCEPT ![f] = NA]
    /\ waitOwner' = [waitOwner EXCEPT ![f] = NA]
    /\ UNCHANGED <<waitReg>>

MovePendingToOwnerLocal(f) ==
    /\ ticketLocation[f] = "PendingSpawn"
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(ownerRecord[f])]
    /\ UNCHANGED <<fiberState, waitReg, ownerRecord, execWorker, waitOwner>>

SuspendFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ waitReg[f] = "None"
    /\ waitOwner[f] = NA
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "K"]
    /\ waitOwner' = [waitOwner EXCEPT ![f] = execWorker[f]]
    /\ execWorker' = [execWorker EXCEPT ![f] = NA]
    /\ UNCHANGED <<ticketLocation, ownerRecord>>

WakeReady(f) ==
    /\ fiberState[f] = "Waiting"
    /\ waitReg[f] # "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "None"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(waitOwner[f])]
    /\ waitOwner' = [waitOwner EXCEPT ![f] = NA]
    /\ UNCHANGED <<ownerRecord, execWorker>>

MoveInboxToLocal(f) ==
    /\ ownerRecord[f] \in Workers
    /\ ticketLocation[f] = InboxOf(ownerRecord[f])
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(ownerRecord[f])]
    /\ UNCHANGED <<fiberState, waitReg, ownerRecord, execWorker, waitOwner>>

(* SAME as the correct model. The buggy model does NOT relax PopRunnable: the
   load-bearing defect is solely in StealRunnableBuggy. Keeping PopRunnable
   intact is what makes the ticket/owner-record split observable -- a stolen
   fiber whose ownerRecord did not transfer CANNOT be popped by the thief
   (ownerRecord[f]=victim # thief), so the defect surfaces as a stuck runnable
   ticket on the wrong owner's queue (InvLocalMatchesOwner). *)
PopRunnable(w, f) ==
    /\ fiberState[f] = "Runnable"
    /\ ticketLocation[f] = LocalOf(w)
    /\ ownerRecord[f] = w
    /\ fiberState' = [fiberState EXCEPT ![f] = "Running"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "None"]
    /\ execWorker' = [execWorker EXCEPT ![f] = w]
    /\ UNCHANGED <<waitReg, ownerRecord, waitOwner>>

(* THE SINGLE DEFECT: ticket moves W0Local -> W1Local but ownerRecord does NOT
   transfer (Model A). Every other line is identical to StealRunnable in the
   correct model; only the ownerRecord update is broken. *)
StealRunnableBuggy(victim, thief, f) ==
    /\ victim # thief
    /\ fiberState[f] = "Runnable"
    /\ ownerRecord[f] = victim
    /\ ticketLocation[f] = LocalOf(victim)
    /\ waitOwner[f] = NA
    /\ execWorker[f] = NA
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(thief)]
    /\ ownerRecord' = ownerRecord   \* <-- BUG: ownerRecord unchanged (Model A). Correct model has [ownerRecord EXCEPT ![f] = thief].
    /\ UNCHANGED <<fiberState, waitReg, execWorker, waitOwner>>

FinishFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ ticketLocation[f] = "None"
    /\ waitReg[f] = "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Done"]
    /\ UNCHANGED <<ticketLocation, waitReg, ownerRecord, execWorker, waitOwner>>

Stutter ==
    /\ UNCHANGED <<fiberState, ticketLocation, waitReg, ownerRecord, execWorker, waitOwner>>

Next ==
    \/ Stutter
    \/ \E f \in Fibers : SpawnPublish(f)
    \/ \E f \in Fibers : MovePendingToOwnerLocal(f)
    \/ \E f \in Fibers : SuspendFiber(f)
    \/ \E f \in Fibers : WakeReady(f)
    \/ \E f \in Fibers : MoveInboxToLocal(f)
    \/ \E w \in Workers, f \in Fibers : PopRunnable(w, f)
    \/ \E victim \in Workers, thief \in Workers, f \in Fibers :
        StealRunnableBuggy(victim, thief, f)
    \/ \E f \in Fibers : FinishFiber(f)

Init ==
    /\ fiberState = [f \in Fibers |-> "Created"]
    /\ ticketLocation = [f \in Fibers |-> "None"]
    /\ waitReg = [f \in Fibers |-> "None"]
    /\ ownerRecord = [f \in Fibers |-> NA]
    /\ execWorker = [f \in Fibers |-> NA]
    /\ waitOwner = [f \in Fibers |-> NA]

Spec == Init /\ [][Next]_<<fiberState, ticketLocation, waitReg, ownerRecord, execWorker, waitOwner>>

(* The load-bearing invariant the buggy model must violate:
   after a steal, the ticket is on W1Local but ownerRecord is still W0.
   This is the runnable ticket / runnable owner-record split -- exactly the
   defect class production prevents by transferring ownerRecord in try_steal. *)
InvLocalMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Local" => ownerRecord[f] = W0)
        /\ (ticketLocation[f] = "W1Local" => ownerRecord[f] = W1)

InvInboxMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Inbox" => ownerRecord[f] = W0)
        /\ (ticketLocation[f] = "W1Inbox" => ownerRecord[f] = W1)

(* The full correct-model invariant conjunction (for completeness / to confirm
   that ONLY InvLocalMatchesOwner is the one that falls). *)
InvTicketImpliesRunnable ==
    \A f \in Fibers :
        ticketLocation[f] # "None" => fiberState[f] = "Runnable"

Inv ==
    /\ InvTicketImpliesRunnable
    /\ InvLocalMatchesOwner
    /\ InvInboxMatchesOwner
=====
