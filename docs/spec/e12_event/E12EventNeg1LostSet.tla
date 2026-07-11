----------------------- MODULE E12EventNeg1LostSet -----------------------
(*
  NEG-EVENT-1 -- Lost Set During Admission.

  One-rule difference from E12Event. Identical EXCEPT CommitSuspendBuggy does
  NOT recheck set_ before committing suspension. The broken protocol:

    waiter checks UNSET (outside the admission CS)
    set -> SET (sees no registered waiters, empty drain)
    waiter registers + commits suspension
    no final readiness closure

  Required counterexample: Event SET + WaitNode Registered + waiter Suspended
  forever. Violated expected property: InvEventAdmissionClosure (E3).

  The defect: CommitSuspendBuggy allows committing suspension even when
  eventSet = "SET" (the guard does NOT check eventSet). The correct model's
  CommitSuspend requires eventSet = "UNSET".
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1

VARIABLES nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetEpoch

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}

ASSUME /\ Nodes # {}
       /\ N0 \in Nodes
       /\ N1 \in Nodes
       /\ N0 # N1

Vars == <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetEpoch>>

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}
SumResolvedCount == resolvedCount[N0] + resolvedCount[N1]
Drainable ==
    {n \in Nodes : nodeState[n] = "Registered" /\ admissionPhase[n] = "Suspended"}

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0
    /\ eventSet = "UNSET"
    /\ admissionPhase = [n \in Nodes |-> "NoAdmission"]
    /\ admissionSawSet = [n \in Nodes |-> FALSE]
    /\ wokenBySetEpoch = [n \in Nodes |-> 0]

Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ admissionPhase[n] = "NoAdmission"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
    /\ linked' = [linked EXCEPT ![n] = TRUE]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
    /\ UNCHANGED <<resolvedCount, wakeDispatched, eventSet,
                   admissionSawSet, wokenBySetEpoch>>

AdmissionWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ eventSet = "SET"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = TRUE]
    /\ UNCHANGED <<eventSet, wokenBySetEpoch>>

\* DEFECT: CommitSuspendBuggy does NOT check eventSet. It allows committing
\* suspension even when SET. The correct model requires eventSet = "UNSET".
CommitSuspendBuggy(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    \* DEFECT: no eventSet guard -- allows suspend while SET.
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  wokenBySetEpoch>>

SetEvent ==
    /\ eventSet = "UNSET"
    /\ eventSet' = "SET"
    /\ nodeState' =
           [n \in Nodes |->
              IF n \in Drainable THEN "Woken" ELSE nodeState[n]]
    /\ linked' =
           [n \in Nodes |->
              IF n \in Drainable THEN FALSE ELSE linked[n]]
    /\ resolvedCount' =
           [n \in Nodes |->
              IF n \in Drainable THEN resolvedCount[n] + 1 ELSE resolvedCount[n]]
    /\ wokenBySetEpoch' =
           [n \in Nodes |->
              IF n \in Drainable THEN wokenBySetEpoch[n] + 1 ELSE wokenBySetEpoch[n]]
    /\ wakeDispatched' = wakeDispatched + Cardinality(Drainable)
    /\ UNCHANGED <<admissionPhase, admissionSawSet>>

ResetEvent ==
    /\ eventSet = "SET"
    /\ eventSet' = "UNSET"
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  admissionPhase, admissionSawSet, wokenBySetEpoch>>

ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<eventSet, admissionPhase, admissionSawSet, wokenBySetEpoch>>

Stutter == UNCHANGED Vars

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionWake(n)
    \/ \E n \in Nodes : CommitSuspendBuggy(n)
    \/ SetEvent
    \/ ResetEvent
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

\* The expected violated property: InvEventAdmissionClosure (E3).
\* E3 consequence: a node that committed suspension (admissionPhase=Suspended)
\* cannot have done so while SET was visible. The broken CommitSuspendBuggy
\* allows Suspended while eventSet="SET", so this invariant is violated when:
\*   nodeState[n]="Registered" /\ admissionPhase[n]="Suspended" /\ eventSet="SET"
\* This is the "Event SET + WaitNode Registered + waiter suspended" counterexample.
InvEventAdmissionClosure ==
    \A n \in Nodes :
        /\ nodeState[n] = "Registered"
        /\ admissionPhase[n] = "Suspended"
        => eventSet = "UNSET"

\* Also asserted (must still hold): these are not the target of the defect.
InvSingleResolutionWinner ==
    \A n \in Nodes : resolvedCount[n] <= 1
InvSingleRunnablePublication ==
    wakeDispatched = SumResolvedCount

=============================================================================
