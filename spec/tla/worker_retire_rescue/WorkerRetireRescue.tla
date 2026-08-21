------------------------------- MODULE WorkerRetireRescue -------------------------------
(* WorkerRetireRescue - worker-retirement runnable-ticket rescue (MODEL-007e).

Focused safety model of the G1 retirement epilogue: a worker leaving the
active run participant set must not strand tickets on its private
local_runnable queue - it moves them to the pre-run domain
(pending_spawn_), where a LIVE worker's loop top (or the next
invocation's setup) redispatches them AND re-records the owner (a
retire-seeded ticket carries its DEAD original owner; the retire moves
the ticket, not the ownership record).

C++ fact source (baseline c1e93f9):
  - src/async/scheduler.cpp:1230-1270  the fused retire+rescue epilogue
    (under one global_mtx_ scope: --live_loop_workers_, active := false,
    move local_runnable -> pending_spawn_ under nested inbox_mtx,
    signal_wake_locked unconditionally)
  - src/async/scheduler.cpp:485-545    loop-top pending_spawn_ pop with
    the fiber_owner_ re-record
  - src/async/scheduler.cpp:383-393    run() setup redistribution
  - src/async/scheduler.cpp:1902-1975  try_steal (transport + owner
    transfer; NEVER calls make_runnable)
  - include/sluice/async/scheduler.hpp WorkerState::active / loop_exited

Boundary: Workers {W0 (retires with the unconsumed ticket), W1 (the
survivor)}; one Fiber F. ticketAt is a SET over the locations - singleton
as-built; the set shape makes NEG-RT2's duplication (rescue copies
instead of moves) expressible without a second ticket object. Safety
only.

Non-goals (explicit): the #161 idle dance / live_loop_workers_ /
idle_count terms (their own suite, spec/tla/e12_rwlock_scheduler_
liveness/ - composition boundary only; if they were needed to prove
ticket topology the boundary would be too wide); the post-retire route
onto a dead inbox and its steal backstop (E8/#115 transport domain);
the next-invocation D2 redistribution (isomorphic to the loop-top D1);
timers, backend, suspend-switch window (MODEL-007(a)'s domain).

Memory-model boundary: `active` is a release/acquire atomic, but every
ticket consumer (rescue, dispatch, run) needs global_mtx_ or the owning
inbox_mtx_ - both SC mutex domains. The fused retire+rescue action is a
faithful SC abstraction. No C++ weak-memory claim.

Init uses equality form (TLC 2.19; issue #172 lesson). *)
EXTENDS Naturals, FiniteSets

CONSTANTS
    W0, W1,                    \* model values: the retiring worker / the survivor
    W0Local, W1Local, Pending, \* ticket locations
    RescueOnRetire,            \* NEG-RT1: FALSE = retire strands the queue (pre-G1 defect)
    RescueMovesNotCopies,      \* NEG-RT2: FALSE = rescue copies without clearing
    RerecordOwnerOnDispatch    \* NEG-RT3: FALSE = dispatch drops the owner re-record

Workers == {W0, W1}
Locs == {W0Local, W1Local, Pending}

VARIABLES
    wActive,       \* [Workers -> BOOLEAN] the participant-active flags
    ticketAt,      \* SUBSET Locs - the ONE runnable ticket's live locations
    owner,         \* the fiber_owner_ record: W0 / W1 / Unassigned
    fiberState,    \* Runnable / Running / Done
    sawRescue      \* history witness: a rescue move happened

vars == <<wActive, ticketAt, owner, fiberState, sawRescue>>

Init ==
    /\ wActive = [w \in Workers |-> TRUE]
    /\ ticketAt = {W0Local}
    /\ owner = W0
    /\ fiberState = "Runnable"
    /\ sawRescue = FALSE

(* R1 - the fused G-scope retirement epilogue: active-flag clear + rescue
   + unconditional signal effect. The owner record is NOT updated (the
   C++ fact: the dead owner rides with the ticket into pending_spawn_).
   NEG-RT1: no rescue (pre-G1 "terminate path strands queued runnables").
   NEG-RT2: the rescue copies instead of moves (queue not cleared). *)
RetireW0 ==
    /\ wActive[W0] = TRUE
    /\ wActive' = [wActive EXCEPT ![W0] = FALSE]
    /\ IF (W0Local \in ticketAt) /\ RescueOnRetire
         THEN /\ ticketAt' =
                  IF RescueMovesNotCopies
                     THEN (ticketAt \ {W0Local}) \cup {Pending}
                     ELSE ticketAt \cup {Pending}
              /\ sawRescue' = TRUE
         ELSE /\ UNCHANGED <<ticketAt, sawRescue>>
    /\ UNCHANGED <<owner, fiberState>>

(* D1 - the loop-top redispatch: a LIVE worker pops pending_spawn_ and
   RE-RECORDS the owner (the adversarial-review route-to-dead-worker
   repair). NEG-RT3 drops the re-record: the stale dead owner survives. *)
RedispatchW1 ==
    /\ wActive[W1] = TRUE
    /\ Pending \in ticketAt
    /\ ticketAt' = (ticketAt \ {Pending}) \cup {W1Local}
    /\ owner' = IF RerecordOwnerOnDispatch THEN W1 ELSE owner
    /\ UNCHANGED <<wActive, fiberState, sawRescue>>

(* X1 - pop + make_running + switch, fused (the suspend-switch window is
   MODEL-007(a)'s modeled domain). The ticket leaves the transport
   domain; the fiber begins executing on the live owner. *)
RunFiber(w) ==
    /\ wActive[w] = TRUE
    /\ (IF w = W0 THEN W0Local \in ticketAt ELSE W1Local \in ticketAt)
    /\ ticketAt' = ticketAt \ {IF w = W0 THEN W0Local ELSE W1Local}
    /\ fiberState' = "Running"
    /\ UNCHANGED <<wActive, owner, sawRescue>>

FinishFiber ==
    /\ fiberState = "Running"
    /\ fiberState' = "Done"
    /\ UNCHANGED <<wActive, ticketAt, owner, sawRescue>>

Stutter == UNCHANGED vars

Next ==
    \/ RetireW0
    \/ RedispatchW1
    \/ \E w \in Workers : RunFiber(w)
    \/ FinishFiber
    \/ Stutter

Spec == Init /\ [][Next]_vars

(* ---- As-built safety laws ---- *)

(* A retired worker's queue never holds the ticket (the rescue emptied it
   in the same critical section). Violated by NEG-RT1 (the pre-G1
   strand). *)
InvNoTicketOnRetiredWorker ==
    (wActive[W0] = FALSE) => (W0Local \notin ticketAt)

(* One live ticket per Fiber: rescue/dispatch/steal are TRANSPORT, never
   publication. Violated by NEG-RT2 (copy without clear). *)
InvSingleTicket ==
    Cardinality(ticketAt) \leq 1

(* A runnable Fiber's ticket is always recoverable: in the pre-run domain
   or on an ACTIVE worker's queue. Entailed co-victim of NEG-RT1. *)
InvRunnableHasRecoverableTicket ==
    (fiberState = "Runnable") =>
        (   (Pending \in ticketAt)
         \/ ((W0Local \in ticketAt) /\ (wActive[W0] = TRUE))
         \/ ((W1Local \in ticketAt) /\ (wActive[W1] = TRUE)) )

(* Owner/location agreement for worker queues, per the REAL owner
   semantics - NO assumption that Pending implies any owner (the pending
   ticket carries its dead original owner until the dispatch re-record).
   Violated by NEG-RT3. *)
InvOwnerLocationConsistency ==
    /\ ((W0Local \in ticketAt) => (owner = W0))
    /\ ((W1Local \in ticketAt) => (owner = W1))

(* ---- Reachability witnesses (NoReach* deliberately false at the
   target; TLC's CEX is the witness). ---- *)

NoReachInitialTicket ==
    ~ (wActive[W0] = TRUE /\ W0Local \in ticketAt /\ fiberState = "Runnable")
NoReachRetiredTicketUnconsumed ==
    ~ (wActive[W0] = FALSE /\ fiberState = "Runnable" /\ ticketAt # {})
NoReachPending == ~ (Pending \in ticketAt)
NoReachRedispatched ==
    ~ (W1Local \in ticketAt /\ owner = W1 /\ wActive[W0] = FALSE)
NoReachResumed == ~ (fiberState \in {"Running", "Done"})
NoReachRescueMove == ~ sawRescue
====
