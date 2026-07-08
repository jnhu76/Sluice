------------------------------- MODULE E8OwnershipTransfer -------------------------------
(*
  E8 runnable ownership-transfer / work-stealing protocol (sluice-CORE-E8).

  Narrow TLA+ model of the abstract scheduling protocol whose correctness is
  load-bearing for E8: stealing a Runnable Fiber TRANSFERS its runnable
  ownership to the thief, preserving the invariant that a stolen Fiber's
  runnable ticket and runnable owner-record always agree (the steal-
  consistency record), so that the thief — which becomes the current executor
  on Pop — captures the correct Worker as the wait-epoch resume owner when it
  later suspends.

  STATE-INDEXED AUTHORITY MODEL (E8-FORMAL-CORRECTIVE).

  The as-built production authority is state-indexed, NOT global. Three
  distinct representations carry ownership/authority, one per Fiber phase:

    ownerRecord[f]  -- RUNNABLE ownership + steal-consistency record.
                       Production: Scheduler::fiber_owner_[F].
                       Mutated by StealRunnable (victim -> thief). Read by the
                       steal eligibility check. NOT read by any wake path.

    execWorker[f]   -- RUNNING execution authority.
                       Production: g_worker (TLS) / WorkerState::current.
                       Set by PopRunnable (the consumer of the owner-local
                       ticket becomes the executor). Cleared at Suspend.

    waitOwner[f]    -- WAITING wait-epoch resume owner.
                       Production: WaitReg.owner captured as g_worker at
                       suspend time (Scheduler::await_* stores WaitReg{me, ws}
                       with ws = g_worker). Read by WakeReady to route the
                       woken Fiber. NOT ownerRecord.

  These three agree at lifecycle transition boundaries (the invariants below),
  but they are DIFFERENT production fields. In particular:

    WakeReady routes by waitOwner[f], NOT by ownerRecord[f].

  This matches production wake routing (wake_ready_*_locked read
  it->second.owner = WaitReg.owner, then route_runnable_locked(f, owner)).
  The earlier single-variable model read owner[f] in WakeReady, which silently
  claimed wake routing reads fiber_owner_ — a refinement ambiguity that is
  corrected here.

  Extends the E7 runnable-publication vocabulary (docs/spec/e7_publication/
  E7Publication.tla): one successful created|waiting -> runnable transition
  grants one runnable publication capability; transport/consumption move/
  consume the ticket without publishing. E8 adds ONE new action, StealRunnable,
  which is MOVE + OWNER-RECORD TRANSFER (never PUBLISH).

  Domain (finite, exhaustive TLC): Workers = {W0, W1}, Fibers = {F0, F1}.

  Models ONLY the state relevant to ownership transfer / wake routing:
    fiberState, ticketLocation, waitReg, ownerRecord, execWorker, waitOwner.
  Does NOT model AsyncBackend, context-switch asm, stacks, ThreadPool, io_uring,
  performance, MW-S2 admission (closed by E7MultiWorkerProgress.tla), or
  Chase-Lev deques (E16).
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, Workers, W0, W1, F0, F1, NA

VARIABLES
    fiberState,        \* [Fibers -> FiberState]
    ticketLocation,    \* [Fibers -> TicketLoc]  -- the ONE abstract runnable token
    waitReg,           \* [Fibers -> WaitKey | None]  -- active wait registration
    ownerRecord,       \* [Fibers -> Workers \cup {NA}]  -- RUNNABLE owner (runnable ticket owner); mutable by steal
    execWorker,        \* [Fibers -> Workers \cup {NA}]  -- RUNNING executor (current Worker); set by Pop, cleared by Suspend
    waitOwner          \* [Fibers -> Workers \cup {NA}]  -- WAITING wait-epoch resume owner; set by Suspend, read by Wake

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

(* =========================================================================
   Helpers
   ========================================================================= *)

(* Total over Workers \cup {NA}: return "None" for NA so the helpers never
   throw a no-true-CASE branch. Callers that read the result also guard on
   ownerRecord/waitOwner \in Workers where the value must be a real local. *)
LocalOf(w) == CASE w = W0 -> "W0Local"
                    [] w = W1 -> "W1Local"
                    [] OTHER -> "None"

InboxOf(w) == CASE w = W0 -> "W0Inbox"
                    [] w = W1 -> "W1Inbox"
                    [] OTHER -> "None"

TicketLive(f) == ticketLocation[f] # "None"
TicketFree(f) == ticketLocation[f] = "None"
RegFree(f)    == waitReg[f] = "None"

(* =========================================================================
   Actions
   ========================================================================= *)

(* PUBLISH: spawn. created -> runnable, one ticket to pending_spawn_.
   Establishes the runnable owner record (initial owner). *)
SpawnPublish(f) ==
    /\ fiberState[f] = "Created"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "PendingSpawn"]
    /\ ownerRecord' = [ownerRecord EXCEPT ![f] = W0]   \* initial runnable owner established at spawn
    /\ execWorker' = [execWorker EXCEPT ![f] = NA]     \* no executor while merely Runnable
    /\ waitOwner' = [waitOwner EXCEPT ![f] = NA]       \* no wait-epoch resume owner while Runnable
    /\ UNCHANGED <<waitReg>>

(* MOVE: pending_spawn -> owner local (run() distribute). *)
MovePendingToOwnerLocal(f) ==
    /\ ticketLocation[f] = "PendingSpawn"
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(ownerRecord[f])]
    /\ UNCHANGED <<fiberState, waitReg, ownerRecord, execWorker, waitOwner>>

(* STATE_ONLY (atomic): a Running fiber registers a wait AND suspends.
   Abstracts the production await_* sequence (register-under-lock, double-
   check, make_waiting, switch away) as one linearized transition.

   THE WAIT-RESUME-AUTHORITY CAPTURE (E8-AUTH-Inv10): waitOwner' := execWorker.
   Production equivalent: WaitReg.owner = g_worker at suspend time
   (Scheduler::await_* stores WaitReg{me, ws} with ws = g_worker, the Worker
   currently executing the Fiber). execWorker is cleared (the Fiber is no
   longer running); waitReg is set. ownerRecord is left UNCHANGED -- production
   never writes fiber_owner_ from await_*; it retains its last runnable-owner
   value, which equals the suspend-time executor by the Running invariant. *)
SuspendFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ waitReg[f] = "None"
    /\ waitOwner[f] = NA   \* precondition hygiene: waitOwner must be NA before capture
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "K"]
    /\ waitOwner' = [waitOwner EXCEPT ![f] = execWorker[f]]   \* capture current executor (WaitReg.owner = g_worker)
    /\ execWorker' = [execWorker EXCEPT ![f] = NA]            \* no executor while Waiting
    /\ UNCHANGED <<ticketLocation, ownerRecord>>

(* PUBLISH: wake a waiting fiber. Waiting -> Runnable; exactly ONE ticket
   published to the queue named by waitOwner[f] -- the captured wait-epoch
   resume owner.

   ROUTING AUTHORITY = waitOwner[f] (production WaitReg.owner). NOT ownerRecord.
   This is the load-bearing correction: production wake_ready_*_locked reads
   it->second.owner (the registration's captured owner) and calls
   route_runnable_locked(f, owner). ownerRecord[f] is invariant-equal to
   waitOwner[f] in valid Waiting states (Inv5), but it is NOT the routing
   source. Modeling wake as reading waitOwner preserves the distinction
   between the routing authority and the consistency relation. *)
WakeReady(f) ==
    /\ fiberState[f] = "Waiting"
    /\ waitReg[f] # "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ waitReg' = [waitReg EXCEPT ![f] = "None"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(waitOwner[f])]
    /\ waitOwner' = [waitOwner EXCEPT ![f] = NA]   \* resume owner consumed
    /\ UNCHANGED <<ownerRecord, execWorker>>

(* MOVE: owner inbox -> owner local. Cardinality unchanged. Kept for
   vocabulary completeness with E7; production inbox is dead storage
   (E8-0 audit O5/O6) but the model permits the transition. Guarded on
   ownerRecord \in Workers so the LocalOf/InboxOf CASE helpers (which are
   total only over {W0,W1}) are never evaluated at NA. *)
MoveInboxToLocal(f) ==
    /\ ownerRecord[f] \in Workers
    /\ ticketLocation[f] = InboxOf(ownerRecord[f])
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(ownerRecord[f])]
    /\ UNCHANGED <<fiberState, waitReg, ownerRecord, execWorker, waitOwner>>

(* CONSUME: pop a runnable ticket; Runnable -> Running; ticket consumed.
   Requires the ticket be on THIS worker's local queue == the current
   ownerRecord's queue (E8-AUTH-Inv3 / former Inv10). The consumer becomes
   the executor: execWorker' := w (production: run_next_on sets
   WorkerState::current = fiber on ws; g_worker = ws on the worker thread). *)
PopRunnable(w, f) ==
    /\ fiberState[f] = "Runnable"
    /\ ticketLocation[f] = LocalOf(w)
    /\ ownerRecord[f] = w
    /\ fiberState' = [fiberState EXCEPT ![f] = "Running"]
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = "None"]
    /\ execWorker' = [execWorker EXCEPT ![f] = w]
    /\ UNCHANGED <<waitReg, ownerRecord, waitOwner>>

(* =========================================================================
   THE E8 ACTION: StealRunnable.

   MOVE existing ticket + TRANSFER runnable ownerRecord. Does NOT call
   make_runnable (no created|waiting -> runnable transition). Does NOT create
   a second ticket. The ticket changes local queue and the runnable owner
   record changes to the thief. One abstract transition.

   Preconditions:  Runnable, ownerRecord = victim, ticket on victim's local,
                   waitOwner = None (a valid Runnable Fiber has no active wait
                   registration -- E7 InvRunnableUnregistered; steal cannot
                   race with a live wait epoch).
   Postconditions: Runnable, ownerRecord = thief, ticket on thief's local.
   Unchanged:      execWorker, waitOwner (both None for a valid Runnable Fiber).
   ========================================================================= *)
StealRunnable(victim, thief, f) ==
    /\ victim # thief
    /\ fiberState[f] = "Runnable"
    /\ ownerRecord[f] = victim
    /\ ticketLocation[f] = LocalOf(victim)
    /\ waitOwner[f] = NA   \* steal cannot race a live wait epoch (Inv9)
    /\ execWorker[f] = NA  \* a valid Runnable Fiber has no executor
    /\ ticketLocation' = [ticketLocation EXCEPT ![f] = LocalOf(thief)]
    /\ ownerRecord' = [ownerRecord EXCEPT ![f] = thief]
    /\ UNCHANGED <<fiberState, waitReg, execWorker, waitOwner>>

(* STATE_ONLY: finish. Running -> Done. Precondition: no ticket, no reg. *)
FinishFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ ticketLocation[f] = "None"
    /\ waitReg[f] = "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Done"]
    /\ UNCHANGED <<ticketLocation, waitReg, ownerRecord, execWorker, waitOwner>>

(* Stutter: no spurious deadlock; safety invariants checked across all
   reachable states including terminal ones. *)
Stutter ==
    /\ UNCHANGED <<fiberState, ticketLocation, waitReg, ownerRecord, execWorker, waitOwner>>

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
    /\ ownerRecord = [f \in Fibers |-> NA]   \* no runnable owner until SpawnPublish sets it
    /\ execWorker = [f \in Fibers |-> NA]    \* no fiber is Running at Init
    /\ waitOwner = [f \in Fibers |-> NA]     \* no fiber is Waiting at Init

Spec == Init /\ [][Next]_<<fiberState, ticketLocation, waitReg, ownerRecord, execWorker, waitOwner>>

(* =========================================================================
   E8-AUTH safety invariants (state-indexed authority)

   "NA" (not-a-worker) is the explicit empty marker for the authority fields:
   a Fiber that is not Running has execWorker = NA; a Fiber that is not
   Waiting has waitOwner = NA. This keeps the routing-authority field
   (waitOwner) and the consistency field (ownerRecord) distinct from the
   real-Worker domain, so phase guards bind them unambiguously.
   ========================================================================= *)

(* E8-AUTH-Inv1: Ticket implies Runnable. *)
InvTicketImpliesRunnable ==
    \A f \in Fibers :
        ticketLocation[f] # "None" => fiberState[f] = "Runnable"

(* E8-AUTH-Inv2: Runnable has one ticket and owner record; no exec/wait auth. *)
InvRunnableAuthority ==
    \A f \in Fibers :
        fiberState[f] = "Runnable"
            => /\ ticketLocation[f] # "None"
               /\ ownerRecord[f] \in Workers
               /\ execWorker[f] = NA            \* no executor while merely Runnable
               /\ waitOwner[f] = NA             \* no wait-epoch resume owner while Runnable

(* E8-AUTH-Inv3: Local ticket matches runnable owner record (load-bearing
   after steal; this is what the negative model violates). *)
InvLocalMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Local" => ownerRecord[f] = W0)
        /\ (ticketLocation[f] = "W1Local" => ownerRecord[f] = W1)

(* E8-AUTH-Inv3b: Inbox destination matches runnable owner record. *)
InvInboxMatchesOwner ==
    \A f \in Fibers :
        /\ (ticketLocation[f] = "W0Inbox" => ownerRecord[f] = W0)
        /\ (ticketLocation[f] = "W1Inbox" => ownerRecord[f] = W1)

(* E8-AUTH-Inv4: Running authority consistency.
   fiberState = Running => execWorker is a real Worker, no ticket, no wait
   auth, and ownerRecord agrees with execWorker (the consumer of the owner-
   local ticket becomes the executor). *)
InvRunningAuthority ==
    \A f \in Fibers :
        fiberState[f] = "Running"
            => /\ execWorker[f] \in Workers
               /\ ticketLocation[f] = "None"
               /\ waitOwner[f] = NA            \* no live wait resume owner while Running
               /\ waitReg[f] = "None"
               /\ ownerRecord[f] = execWorker[f]

(* E8-AUTH-Inv5: Waiting authority consistency.
   fiberState = Waiting => waitOwner is a real Worker (the resume authority),
   no ticket, no executor, and ownerRecord is invariant-equal to waitOwner
   (the runnable owner record retains the suspend-time executor because the
   Running invariant bound ownerRecord=execWorker, and Suspend sets
   waitOwner=execWorker while leaving ownerRecord unchanged). *)
InvWaitingAuthority ==
    \A f \in Fibers :
        fiberState[f] = "Waiting"
            => /\ waitReg[f] # "None"
               /\ ticketLocation[f] = "None"
               /\ execWorker[f] = NA          \* no executor while Waiting
               /\ waitOwner[f] \in Workers
               /\ ownerRecord[f] = waitOwner[f]

(* E8-AUTH-Inv6: Wake routes by wait owner.
   Encoded structurally in WakeReady: ticketLocation' = LocalOf(waitOwner[f]).
   There is NO abstract rule by which WakeReady routes via ownerRecord, even
   though ownerRecord = waitOwner in all valid Waiting states (Inv5). The
   model preserves the routing-authority vs consistency-relation distinction.
   (Structural; no separate state formula.) *)

(* E8-AUTH-Inv7: Done detached. *)
InvDoneDetached ==
    \A f \in Fibers :
        fiberState[f] = "Done"
            => /\ ticketLocation[f] = "None"
               /\ waitReg[f] = "None"

(* E8-AUTH-Inv8: Steal preserves one ticket. Encoded structurally:
   StealRunnable has no make_runnable and moves one ticket; it cannot
   produce ticketLocation None -> NonNone. Captured by Inv1 + Inv2 together
   with StealRunnable's pre/post. (Structural; no separate formula.) *)

(* E8-AUTH-Inv9: Steal cannot race with a valid Waiting authority.
   StealRunnable is enabled only for fiberState = Runnable and waitOwner = NA
   (precondition). A valid Fiber cannot simultaneously be stealable and
   registered for wait resume (E7 InvRunnableUnregistered). (Structural +
   InvRunnableUnregistered.) *)

(* E8-AUTH-Inv10: Suspend captures current executor.
   Encoded structurally in SuspendFiber: waitOwner' = execWorker. (Structural;
   the state-level consequence is InvWaitingAuthority: waitOwner is set and
   equals ownerRecord which equaled execWorker under InvRunningAuthority.) *)

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

Inv ==
    /\ InvTicketImpliesRunnable
    /\ InvRunnableAuthority
    /\ InvLocalMatchesOwner
    /\ InvInboxMatchesOwner
    /\ InvRunningAuthority
    /\ InvWaitingAuthority
    /\ InvDoneDetached
    /\ InvWaitingRegistered
    /\ InvRegisteredWaiting
    /\ InvRunnableUnregistered
=====
