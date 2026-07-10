----------------------- MODULE E11TimerWaitNeg5DeadlineLostParked -----------------------
(*
  E11 NEG-5 — Deadline Lost While Scheduler Is Parked (BROKEN liveness model).

  Defect: the Scheduler may Park indefinitely even when an active deadline is
  due, because there is no mechanism forcing the deadline-expiry resolution to
  execute. Parked becomes an absorbing state in which a due deadline is never
  observed. (In production this is the missing bounded-park-timeout / timer-pump
  in the worker loop: a parked Worker with an active deadline must not stay
  parked past that deadline.)

  Broken protocol:
      active deadline exists
      Worker observes no runnable work
      Worker parks indefinitely
      deadline becomes due
      no wake source executes timer expiry

  Required liveness counterexample:
      deadline due forever /\ wait unresolved forever /\ Scheduler parked forever

  The correct model (E11TimerWait) bounds park by the earliest active deadline
  and drives the timer pump from the worker loop, so DeadlineParkLiveness holds.
  This buggy model DROPS the Tick-from-parked and ResolveTimer-from-parked
  obligation: once parked, neither Tick nor ResolveTimer is enabled, so a due
  deadline can never resolve — a deadlock-free but liveness-violating trace.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, Regs, R0, R1

VARIABLES
    nodeState, linked, resolvedCount, wakeDispatched,
    regState, regEpoch, regDeadline, nodeAlive, now, parked

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
RegState  == {"Inert", "Active", "Retired", "Consumed"}
DeadlineVal == 0..3

ASSUME
    /\ Nodes # {} /\ N0 \in Nodes /\ N1 \in Nodes /\ N0 # N1
    /\ Regs # {} /\ R0 \in Regs /\ R1 \in Regs /\ R0 # R1

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ nodeAlive[n] = TRUE
    /\ \E r \in Regs :
        /\ regState[r] = "Inert"
        /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
        /\ linked' = [linked EXCEPT ![n] = TRUE]
        /\ regState' = [regState EXCEPT ![r] = "Active"]
        /\ regEpoch' = [regEpoch EXCEPT ![r] = n]
        /\ \E d \in DeadlineVal : regDeadline' = [regDeadline EXCEPT ![r] = d]
        /\ UNCHANGED <<nodeAlive, now, parked, resolvedCount, wakeDispatched>>

ResolveWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ regState' = [r \in Regs |->
                        IF /\ regEpoch[r] = n /\ regState[r] = "Active"
                        THEN "Retired" ELSE regState[r]]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ regState' = [r \in Regs |->
                        IF /\ regEpoch[r] = n /\ regState[r] = "Active"
                        THEN "Retired" ELSE regState[r]]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

ResolveTimer(r) ==
    /\ regState[r] = "Active"
    /\ now >= regDeadline[r]
    /\ nodeState[regEpoch[r]] = "Registered"
    /\ regState' = [regState EXCEPT ![r] = "Consumed"]
    /\ nodeState' = [nodeState EXCEPT ![regEpoch[r]] = "Expired"]
    /\ linked' = [linked EXCEPT ![regEpoch[r]] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![regEpoch[r]] =
                                    resolvedCount[regEpoch[r]] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

DestroyNode(n) ==
    /\ isTerminal(n)
    /\ nodeAlive[n] = TRUE
    /\ \A r \in Regs : regEpoch[r] = n => regState[r] \in {"Retired", "Consumed", "Inert"}
    /\ nodeAlive' = [nodeAlive EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, now, parked>>

(* Tick advances time. DEFECT: Tick is disabled while parked (a parked worker
   does not advance/drive the timer). Combined with the parked-absorbing Park
   action below, a due deadline can never resolve once parked. *)
Tick ==
    /\ ~parked
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, parked>>

(* Park: the Scheduler idles. DEFECT: there is NO Unpark action, and Tick /
   ResolveTimer are disabled while parked (ResolveTimer is still enabled by its
   own guard, but Tick — which makes the deadline due — is not). The trace:
   register a deadline in the future, advance time via Tick to just below the
   deadline, Park, then Tick-to-due is impossible (parked). The deadline stays
   due-forever-but-unresolved. Stutter keeps the state valid (no deadlock). *)
Park ==
    /\ ~parked
    /\ parked' = TRUE
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now>>

Stutter == UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                       regState, regEpoch, regDeadline, nodeAlive, now, parked>>

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : ResolveWake(n)
    \/ \E n \in Nodes : ResolveCancel(n)
    \/ \E r \in Regs : ResolveTimer(r)
    \/ \E n \in Nodes : DestroyNode(n)
    \/ Tick
    \/ Park

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0
    /\ regState = [r \in Regs |-> "Inert"]
    /\ regEpoch = [r \in Regs |-> N0]
    /\ regDeadline = [r \in Regs |-> 0]
    /\ nodeAlive = [n \in Nodes |-> FALSE]
    /\ now = 0
    /\ parked = FALSE

Vars == <<nodeState, linked, resolvedCount, wakeDispatched,
          regState, regEpoch, regDeadline, nodeAlive, now, parked>>
Spec == Init /\ [][Next]_Vars

(* I6 — Deadline Park Liveness: an Active due deadline is eventually resolved.
   The buggy model VIOLATES this: once parked with a due-but-unresolvable
   deadline, the wait stays unresolved forever. *)
DeadlineParkLiveness ==
    \A r \in Regs :
        (/\ regState[r] = "Active"
         /\ now >= regDeadline[r]
         /\ nodeState[regEpoch[r]] = "Registered")
        ~> nodeState[regEpoch[r]] = "Expired"

=============================================================================
