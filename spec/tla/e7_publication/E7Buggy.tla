------------------------------- MODULE E7Buggy -----------------------------------
(*
  Deliberately BROKEN variant of the E7 runnable-publication protocol,
  matching the proven old production defect (Phase-5 flight recorder).

  Defect: make_runnable() returned void (silent no-op on already-Runnable),
  and wake producers enqueued UNCONDITIONALLY. The production await_*
  sequence has a transient window: register-wait under lock, THEN
  make_waiting. A concurrent wake_ready_*_locked can fire in that window,
  while the fiber is Running + registered, and re-publish a runnable ticket
  for a fiber whose original ticket was already consumed. Result: two live
  tickets; the first runs the fiber to Done; the second pops a Done fiber.

  Uses ticketCount[f] \in 0..N to allow duplicates. TLC MUST find a
  counterexample to InvDoneNoTicket matching the observed causal chain.
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, Workers, WaitKeys, W0, W1, F0, F1, F2, K0, K1

VARIABLES fiberState, ticketCount, waitReg

FiberState == {"Created", "Waiting", "Runnable", "Running", "Done"}

SpawnPublish(f) ==
    /\ fiberState[f] = "Created"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Runnable"]
    /\ ticketCount' = [ticketCount EXCEPT ![f] = @ + 1]
    /\ UNCHANGED waitReg

PopRunnable(f) ==
    /\ fiberState[f] = "Runnable"
    /\ ticketCount[f] > 0
    /\ fiberState' = [fiberState EXCEPT ![f] = "Running"]
    /\ ticketCount' = [ticketCount EXCEPT ![f] = @ - 1]
    /\ UNCHANGED waitReg

\* Production transient step 1: a Running fiber registers a wait (under lock).
RegisterWait(f, key) ==
    /\ fiberState[f] = "Running"
    /\ waitReg[f] = "None"
    /\ key \in WaitKeys
    /\ waitReg' = [waitReg EXCEPT ![f] = key]
    /\ UNCHANGED <<fiberState, ticketCount>>

\* Production transient step 2: the registered Running fiber now suspends.
SuspendFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ waitReg[f] # "None"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Waiting"]
    /\ UNCHANGED <<ticketCount, waitReg>>

\* THE BROKEN ACTION: wake publishes UNCONDITIONALLY. Admits Running +
\* registered (the transient) OR Waiting + registered. Increments
\* ticketCount regardless of whether a state transition was owned. This is
\* the proven old-producer defect (void make_runnable + unconditional route).
\* THE DEFECT (proven producer): publish an additional runnable ticket for a
\* fiber that is ALREADY Runnable, without owning any state transition. This
\* abstracts the production "make_runnable() no-op + unconditional
\* route_runnable_locked" on a fiber whose spawn ticket is still unconsumed.
\* The result is two live tickets for one runnable epoch.
DefectDuplicatePublish(f) ==
    /\ fiberState[f] = "Runnable"
    /\ ticketCount[f] >= 1
    /\ ticketCount' = [ticketCount EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<fiberState, waitReg>>

FinishFiber(f) ==
    /\ fiberState[f] = "Running"
    /\ fiberState' = [fiberState EXCEPT ![f] = "Done"]
    /\ UNCHANGED <<ticketCount, waitReg>>

Stutter == UNCHANGED <<fiberState, ticketCount, waitReg>>

Next ==
    \/ Stutter
    \/ \E f \in Fibers :
        \/ SpawnPublish(f)
        \/ PopRunnable(f)
        \/ \E key \in WaitKeys : RegisterWait(f, key)
        \/ SuspendFiber(f)
        \/ DefectDuplicatePublish(f)
        \/ FinishFiber(f)

Init ==
    /\ fiberState = [f \in Fibers |-> "Created"]
    /\ ticketCount = [f \in Fibers |-> 0]
    /\ waitReg = [f \in Fibers |-> "None"]

Spec == Init /\ [][Next]_<<fiberState, ticketCount, waitReg>>

\* The invariant the broken model is expected to VIOLATE.
InvDoneNoTicket ==
    \A f \in Fibers :
        fiberState[f] = "Done" => ticketCount[f] = 0
=====
