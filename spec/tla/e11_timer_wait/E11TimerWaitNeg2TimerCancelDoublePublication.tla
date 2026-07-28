----------------------- MODULE E11TimerWaitNeg2TimerCancelDoublePublication -----------------------
(*
  E11 NEG-2 — Timer Expiry + Cancellation Double Publication (BROKEN model).

  Defect: the cancellation resolver has an INDEPENDENT completion authority —
  it publishes (resolvedCount + wakeDispatched) WITHOUT performing the resolve_
  CAS winner transition on the node. The buggy ResolveCancel does NOT move
  nodeState out of "Registered", so a CONCURRENT ResolveTimer (which guards on
  nodeState = "Registered") can subsequently fire on the SAME node and publish a
  SECOND time. This mirrors NEG-1 but with the cancel cause as the buggy
  independent publisher.

  Broken protocol:
      timer-local completion authority
      + cancel-local completion authority
      -> both believe they won -> two publications

  Required counterexample:
      same wait epoch N -> resolvedCount[N] = 2 and wakeDispatched = 2,
      violating InvSingleResolutionWinner / InvSingleRunnablePublication.

  MODEL DESIGN (M5 one-rule difference). Identical to E11TimerWait EXCEPT
  ResolveCancel publishes but leaves nodeState at "Registered" (omits the
  resolve_ CAS that would move it to "Cancelled"). Every other rule is the
  correct protocol. REACHABILITY: ResolveCancelBuggy and ResolveTimer are BOTH
  enabled on (regState=Active, due, nodeState=Registered); TLC schedules the
  buggy cancel (publishes once, node stays Registered) then the timer (publishes
  again), reaching resolvedCount = 2. Same shape as the E10 NoWinner CE.
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
    /\ nodeAlive[n] = FALSE
    /\ \E r \in Regs :
        /\ regState[r] = "Inert"
        /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
        /\ linked' = [linked EXCEPT ![n] = TRUE]
        /\ regState' = [regState EXCEPT ![r] = "Active"]
        /\ regEpoch' = [regEpoch EXCEPT ![r] = n]
        /\ \E d \in DeadlineVal : regDeadline' = [regDeadline EXCEPT ![r] = d]
        /\ nodeAlive' = [nodeAlive EXCEPT ![n] = TRUE]
        /\ UNCHANGED <<now, parked, resolvedCount, wakeDispatched>>

AdmissionExpire(n) ==
    /\ nodeState[n] = "Registered"
    /\ \E r \in Regs :
        /\ regEpoch[r] = n
        /\ regState[r] = "Active"
        /\ now >= regDeadline[r]
    /\ nodeState' = [nodeState EXCEPT ![n] = "Expired"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ regState' = [r \in Regs |->
                          IF /\ regEpoch[r] = n
                             /\ regState[r] = "Active"
                             /\ now >= regDeadline[r]
                          THEN "Consumed"
                          ELSE regState[r]]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

ResolveWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ regState' = [r \in Regs |->
                        IF /\ regEpoch[r] = n
                           /\ regState[r] = "Active"
                        THEN "Retired"
                        ELSE regState[r]]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

(* DEFECT: ResolveCancel publishes (resolvedCount + wakeDispatched) but does NOT
   perform the resolve_ CAS — nodeState stays "Registered". A subsequent
   ResolveTimer is still enabled on the same node and publishes AGAIN ->
   resolvedCount = 2. (Correct model transitions nodeState to "Cancelled".) *)
ResolveCancelBuggy(n) ==
    /\ nodeState[n] = "Registered"
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    \* DEFECT: nodeState is NOT moved to "Cancelled" (no resolve_ CAS); the
    \* registration is also NOT retired (cancel-local authority ignores it).
    /\ UNCHANGED <<nodeState, linked, regState, regEpoch, regDeadline, nodeAlive, now, parked>>

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
    \/ \E n \in Nodes : AdmissionExpire(n)
    \/ \E n \in Nodes : ResolveWake(n)
    \/ \E n \in Nodes : ResolveCancelBuggy(n)
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

(* The invariants the buggy model must VIOLATE. *)
InvSingleResolutionWinner == \A n \in Nodes : resolvedCount[n] <= 1
SumResolvedCount == resolvedCount[N0] + resolvedCount[N1]
InvSingleRunnablePublication == wakeDispatched = SumResolvedCount

=============================================================================
