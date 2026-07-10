----------------------- MODULE E11TimerWaitNeg2TimerCancelDoublePublication -----------------------
(*
  E11 NEG-2 — Timer Expiry + Cancellation Double Publication (BROKEN model).

  Defect: the cancellation resolver has an INDEPENDENT completion authority — it
  does NOT guard on the resolve_ CAS winner. ResolveCancel fires on any non-
  Detached node (terminal OR Registered), unconditionally incrementing
  resolvedCount + wakeDispatched. This mirrors the timer-local winner authority
  defect but on the cancel cause.

  Broken protocol:
      timer-local completion authority
      + cancel-local completion authority
      -> both believe they won -> two publications

  Required counterexample:
      a timer-expired node is then "cancelled" -> resolvedCount[n] = 2,
      violating InvSingleResolutionWinner / InvSingleRunnablePublication.

  Identical to E11TimerWait EXCEPT ResolveCancel omits the Registered guard
  (fires on any non-Detached node). Isolates the defect (M5: one-rule diff).
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

(* DEFECT: ResolveCancel has NO winner-CAS guard. Fires on any non-Detached node
   (incl. Expired), unconditionally publishing. A timer-expired node can be
   "cancelled" -> double publication. (Correct: nodeState[n] = "Registered".) *)
ResolveCancel(n) ==
    /\ nodeState[n] # "Detached"   \* DEFECT: should be = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<linked, regState, regEpoch, regDeadline, nodeAlive, now, parked>>

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

Tick ==
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, parked>>

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

InvSingleResolutionWinner == \A n \in Nodes : resolvedCount[n] <= 1
SumResolvedCount == resolvedCount[N0] + resolvedCount[N1]
InvSingleRunnablePublication == wakeDispatched = SumResolvedCount

=============================================================================
