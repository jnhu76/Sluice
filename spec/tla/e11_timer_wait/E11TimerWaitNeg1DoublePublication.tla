----------------------- MODULE E11TimerWaitNeg1DoublePublication -----------------------
(*
  E11 NEG-1 — Resource Wake + Timer Double Publication (BROKEN model).

  Defect: the timer expiry resolver publishes INDEPENDENTLY — it increments
  resolvedCount + wakeDispatched and consumes the registration WITHOUT
  performing the resolve_ CAS winner transition on the node. Specifically the
  buggy ResolveTimer does NOT move nodeState out of "Registered", so a CONCURRENT
  ResolveWake (which guards on nodeState = "Registered") can subsequently fire
  on the SAME node and publish a SECOND time. This is the "timer-local
  completion authority" defect: the timer believes it won and dispatched a
  runnable ticket, but the node is still Registered and another cause can still
  resolve it.

  Broken protocol:
      RESOURCE_WAKE publishes (CAS Registered -> Woken)
      TIMER_EXPIRE published earlier without the CAS (node still Registered)
      -> two runnable publications for one wait epoch

  Required counterexample:
      same wait epoch N -> resolvedCount[N] = 2 and wakeDispatched = 2,
      violating InvSingleResolutionWinner / InvSingleRunnablePublication.

  MODEL DESIGN (M5 one-rule difference). Identical to E11TimerWait EXCEPT
  ResolveTimer publishes + consumes the registration but leaves nodeState at
  "Registered" (omits the resolve_ CAS that would move it to "Expired"). Every
  other rule is the correct protocol. This is the smallest semantic defect that
  represents "timer expiry without the shared winner CAS" and makes the double
  publication reachable: after the buggy timer fires, ResolveWake is STILL
  enabled on the Registered node.

  REACHABILITY. The defect is reachable because ResolveTimerBuggy and
  ResolveWake are BOTH enabled in the state (regState=Active, due,
  nodeState=Registered): TLC schedules ResolveTimerBuggy (publishes once, node
  stays Registered, reg -> Consumed), then ResolveWake (node Registered ->
  Woken, publishes again). resolvedCount reaches 2. This is the same shape as
  the E10 E10WaitNodeBuggyNoWinner counterexample (two resolvers firing on one
  Registered node).
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

ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ regState' = [r \in Regs |->
                        IF /\ regEpoch[r] = n
                           /\ regState[r] = "Active"
                        THEN "Retired"
                        ELSE regState[r]]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

(* DEFECT: ResolveTimer publishes (resolvedCount + wakeDispatched) and consumes
   the registration, but it does NOT perform the resolve_ CAS — nodeState stays
   "Registered". A subsequent ResolveWake is still enabled on the same node and
   publishes AGAIN -> resolvedCount = 2. (Correct model transitions
   nodeState[regEpoch[r]] to "Expired", closing the Registered window.) *)
ResolveTimerBuggy(r) ==
    /\ regState[r] = "Active"
    /\ now >= regDeadline[r]
    /\ nodeState[regEpoch[r]] = "Registered"
    /\ regState' = [regState EXCEPT ![r] = "Consumed"]
    \* DEFECT: nodeState is NOT moved to "Expired" (no resolve_ CAS).
    /\ resolvedCount' = [resolvedCount EXCEPT ![regEpoch[r]] =
                                    resolvedCount[regEpoch[r]] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<nodeState, linked, regEpoch, regDeadline, nodeAlive, now, parked>>

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
    \/ \E n \in Nodes : ResolveCancel(n)
    \/ \E r \in Regs : ResolveTimerBuggy(r)
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
