---------------------- MODULE E12EventNeg2WakeOne ----------------------
(*
  NEG-EVENT-2 -- Wake-One Manual-Reset Strands Waiter (E12-A-EVENT-CORRECTIVE-1).

  One-rule difference from the E12 set semantics: SetEventBuggy resolves ONLY
  ONE registered Suspended waiter (the FIFO head / a victim), instead of
  draining all registered Suspended waiters. The broken protocol:

    W1 registered, W2 registered
    SetEventBuggy -> SET, resolves only W1
    set-on-SET idempotent (cannot re-enter a drain on SET); the failing trace
    takes neither reset nor cancellation, and deadline resolution is absent
    W2 remains Registered+Suspended forever while Event remains SET

  Required counterexample: W2 remains Registered+Suspended forever while
  Event remains SET. Violated expected property: EventSetDrainLivenessNonVacuous
  (E4 liveness).

  Defect: SetEventBuggy resolves only ONE Suspended node, leaving the others
  stranded. The correct set protocol (StartSet -> DrainOne* -> FinishSet) drains
  every Suspended node under fairness; this buggy atomic set picks one victim.

  NOTE: this negative model uses the ORIGINAL atomic SetEventBuggy (not the
  multi-step StartSet/DrainOne/FinishSet) because the defect it targets is the
  set-releases-ALL cardinality, which is most directly expressed by an atomic
  set that resolves exactly one. The multi-step model is the refinement that
  proves set/reset epoch isolation (E6); NEG-2 targets the orthogonal E4
  liveness/cardinality property. State variables are shared for consistency.
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

CommitSuspend(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ eventSet = "UNSET"
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  wokenBySetDrain,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen, wakeEpochGen, resolutionCause>>

\* DEFECT: SetEventBuggy transitions to SET and resolves ONLY ONE Suspended node
\* (picks a victim via \E), instead of draining all. A non-victim Suspended node
\* remains Registered forever while eventSet stays SET. Since eventSet is now
\* SET, SetEventBuggy cannot re-fire (it requires UNSET), and reset is absent, so
\* the stranded node is never resolved.
SetEventBuggy ==
    /\ eventSet = "UNSET"
    /\ Cardinality(Drainable) >= 1
    /\ \E victim \in Drainable :
          /\ eventSet' = "SET"
          /\ activeSetGen' = resetGeneration
          /\ nodeState' =
                 [n \in Nodes |->
                    IF n = victim THEN "Woken" ELSE nodeState[n]]
          /\ linked' =
                 [n \in Nodes |->
                    IF n = victim THEN FALSE ELSE linked[n]]
          /\ resolvedCount' =
                 [n \in Nodes |->
                    IF n = victim THEN resolvedCount[n] + 1 ELSE resolvedCount[n]]
          /\ wokenBySetDrain' =
                 [n \in Nodes |->
                    IF n = victim THEN wokenBySetDrain[n] + 1 ELSE wokenBySetDrain[n]]
          /\ wakeEpochGen' =
                 [n \in Nodes |->
                    IF n = victim THEN activeSetGen' ELSE wakeEpochGen[n]]
          /\ wakeDispatched' = wakeDispatched + 1
          /\ resolutionCause' =
                 [n \in Nodes |->
                    IF n = victim THEN "SetBroadcast" ELSE resolutionCause[n]]

    /\ UNCHANGED <<admissionPhase, admissionSawSet,
                  protoPhase, resetGeneration, registrationGeneration>>

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
    \/ \E n \in Nodes : CommitSuspend(n)
    \/ SetEventBuggy
    \/ ResetEvent
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

\* Fairness on the buggy set (so it must eventually run when drainable + UNSET).
FairSetEventBuggy == WF_Vars(SetEventBuggy)
LivenessSpecFair == Spec /\ FairSetEventBuggy

\* The expected violated property: EventSetDrainLivenessNonVacuous (E4).
\* A Suspended node that is NOT the victim remains Registered forever.
EventSetDrainLivenessNonVacuous ==
    \A n \in Nodes :
        [] ( (/\ nodeState[n] = "Registered"
              /\ admissionPhase[n] = "Suspended")
             => <> (isTerminal(n)) )

=============================================================================
