----------------------- MODULE E12EventNeg1LostSet -----------------------
(*
  NEG-EVENT-1 -- Lost Set During Admission (E12-A-EVENT-CORRECTIVE-1).

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

  Built on the E12-A-EVENT-CORRECTIVE-1 multi-step model state (same variables
  as E12Event). The set protocol here is the correct multi-step StartSet ->
  DrainOne -> FinishSet; the ONLY defect is CommitSuspendBuggy.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, MaxGen

VARIABLES nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetDrain,
          protoPhase, resetGeneration, registrationGeneration,
          activeSetGen, wakeEpochGen, resolutionCause

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
ProtoPhase == {"Idle", "SetDrain"}
NoGen == 1000000

ASSUME /\ Nodes # {}
       /\ N0 \in Nodes
       /\ N1 \in Nodes
       /\ N0 # N1
       /\ MaxGen \in 1..10

Vars == <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetDrain,
          protoPhase, resetGeneration, registrationGeneration,
          activeSetGen, wakeEpochGen, resolutionCause>>

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
    /\ wokenBySetDrain = [n \in Nodes |-> 0]
    /\ protoPhase = "Idle"
    /\ resetGeneration = 0
    /\ registrationGeneration = [n \in Nodes |-> 0]
    /\ activeSetGen = NoGen
    /\ wakeEpochGen = [n \in Nodes |-> NoGen]
    /\ resolutionCause = [n \in Nodes |-> "None"]

Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ admissionPhase[n] = "NoAdmission"
    /\ protoPhase = "Idle"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
    /\ linked' = [linked EXCEPT ![n] = TRUE]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
    /\ registrationGeneration' = [registrationGeneration EXCEPT ![n] = resetGeneration]
    /\ UNCHANGED <<resolvedCount, wakeDispatched, eventSet,
                   admissionSawSet, wokenBySetDrain,
                   protoPhase, resetGeneration, activeSetGen,
                   wakeEpochGen, resolutionCause>>

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
    /\ resolutionCause' = [resolutionCause EXCEPT ![n] = "AdmissionSet"]
    /\ UNCHANGED <<eventSet, wokenBySetDrain,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen, wakeEpochGen>>

\* DEFECT: CommitSuspendBuggy does NOT check eventSet. It allows committing
\* suspension even when SET. The correct model requires eventSet = "UNSET".
CommitSuspendBuggy(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    \* DEFECT: no eventSet guard -- allows suspend while SET.
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  wokenBySetDrain,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen, wakeEpochGen, resolutionCause>>

StartSet ==
    /\ protoPhase = "Idle"
    /\ eventSet = "UNSET"
    /\ protoPhase' = "SetDrain"
    /\ eventSet' = "SET"
    /\ activeSetGen' = resetGeneration
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  admissionPhase, admissionSawSet, wokenBySetDrain,
                  resetGeneration, registrationGeneration, wakeEpochGen,
                  resolutionCause>>

DrainOne(n) ==
    /\ protoPhase = "SetDrain"
    /\ activeSetGen # NoGen
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "Suspended"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ wokenBySetDrain' = [wokenBySetDrain EXCEPT ![n] = wokenBySetDrain[n] + 1]
    /\ wakeEpochGen' = [wakeEpochGen EXCEPT ![n] = activeSetGen]
    /\ resolutionCause' = [resolutionCause EXCEPT ![n] = "SetBroadcast"]
    /\ UNCHANGED <<eventSet, admissionPhase, admissionSawSet,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen>>

FinishSet ==
    /\ protoPhase = "SetDrain"
    /\ protoPhase' = "Idle"
    /\ activeSetGen' = NoGen
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  admissionPhase, admissionSawSet, wokenBySetDrain,
                  resetGeneration, registrationGeneration,
                  wakeEpochGen, resolutionCause>>

ResetEvent ==
    /\ protoPhase = "Idle"
    /\ eventSet = "SET"
    /\ resetGeneration < MaxGen
    /\ eventSet' = "UNSET"
    /\ resetGeneration' = resetGeneration + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  admissionPhase, admissionSawSet, wokenBySetDrain,
                  protoPhase, registrationGeneration, activeSetGen,
                  wakeEpochGen, resolutionCause>>

ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ protoPhase = "Idle"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ resolutionCause' = [resolutionCause EXCEPT ![n] = "Cancel"]
    /\ UNCHANGED <<eventSet, admissionPhase, admissionSawSet, wokenBySetDrain,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen, wakeEpochGen>>

Stutter == UNCHANGED Vars

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionWake(n)
    \/ \E n \in Nodes : CommitSuspendBuggy(n)
    \/ StartSet
    \/ \E n \in Nodes : DrainOne(n)
    \/ FinishSet
    \/ ResetEvent
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

\* The expected violated property: InvEventAdmissionClosure (E3).
\* E3 consequence: a wait that committed suspension (admissionPhase=Suspended)
\* cannot have done so while SET was visible. The broken CommitSuspendBuggy
\* allows Suspended while eventSet="SET", so this invariant is violated when:
\*   nodeState[n]="Registered" /\ admissionPhase[n]="Suspended" /\ eventSet="SET"
\* This is the "Event SET + WaitNode Registered + waiter suspended" counterexample.
\* (Note: with the multi-step drain, a CORRECT model can transiently have a
\*  Suspended+Registered node while a drain is mid-flight (protoPhase=SetDrain).
\*  The lost-set counterexample is distinguished by protoPhase=Idle: a Suspended
\*  node with NO active drain to resolve it, while SET.)
InvEventAdmissionClosure ==
    \A n \in Nodes :
        /\ nodeState[n] = "Registered"
        /\ admissionPhase[n] = "Suspended"
        /\ protoPhase = "Idle"
        => eventSet = "UNSET"

\* Also asserted (must still hold): these are not the target of the defect.
InvSingleResolutionWinner ==
    \A n \in Nodes : resolvedCount[n] <= 1
InvSingleRunnablePublication ==
    wakeDispatched = SumResolvedCount

=============================================================================
