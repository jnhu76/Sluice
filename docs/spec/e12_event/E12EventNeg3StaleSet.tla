---------------------- MODULE E12EventNeg3StaleSet ----------------------
(*
  NEG-EVENT-3 -- Old Set Wakes Post-Reset Waiter (E12-A-EVENT-CORRECTIVE-1,
  Corrective I).

  Single broken rule: the global serialization between old set drain, reset,
  and admission is LOST. In the correct model, ResetEvent and Register require
  protoPhase = "Idle" (they cannot complete while a SetDrain is active). In this
  negative model, ResetBuggy and RegisterBuggy may run while protoPhase =
  "SetDrain", allowing:

    S1 StartSet
        -> active set epoch, activeSetGen = G0
    ResetBuggy
        -> eventSet UNSET, resetGeneration = G1   (while S1 drain still active)
    Wnew RegisterBuggy
        -> Registered, registrationGeneration = G1 (admitted AFTER reset)
    stale DrainOne(Wnew)
        -> Wnew Woken by the S1 epoch, wakeEpochGen[Wnew] = G0

  Counterexample: Wnew has registrationGeneration = G1 but wakeEpochGen = G0,
  with G1 > G0. Violated expected property: InvSetEpochIsolation (E6).

  The model does NOT:
    - initialize a stale wake directly (the wake comes from a real DrainOne)
    - disable all progress (Reset/Register/Drain all still run)
    - reuse an already terminal node (Wnew is Registered)
    - change WaitNode single-resolution semantics (resolve_ CAS is intact)
    - remove deadline/cancel generally (ResolveCancel remains)

  The single broken rule is the lost protoPhase guard on Reset + Register.
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

\* DEFECT: RegisterBuggy drops the protoPhase = "Idle" guard. A waiter may be
\* admitted WHILE a set drain is active.
RegisterBuggy(n) ==
    /\ nodeState[n] = "Detached"
    /\ admissionPhase[n] = "NoAdmission"
    \* DEFECT: no protoPhase guard.
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

\* DrainOne: may resolve a Registered+Suspended node via the active set epoch.
\* In the buggy model this can reach a node admitted (Registered+Suspended) by
\* RegisterBuggy+CommitSuspend while the drain was active — the stale wake.
\* Note: the node must be Suspended (CommitSuspend ran), so the test reaches
\* the stale state via RegisterBuggy -> CommitSuspend -> DrainOne all while
\* protoPhase="SetDrain".
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
    /\ Drainable = {}
    /\ protoPhase' = "Idle"
    /\ activeSetGen' = NoGen
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  admissionPhase, admissionSawSet, wokenBySetDrain,
                  resetGeneration, registrationGeneration,
                  wakeEpochGen, resolutionCause>>

\* DEFECT: ResetBuggy drops the protoPhase = "Idle" guard. Reset may run WHILE a
\* set drain is active, advancing resetGeneration under the stale epoch.
ResetBuggy ==
    /\ eventSet = "SET"
    /\ resetGeneration < MaxGen
    \* DEFECT: no protoPhase guard.
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
    \/ \E n \in Nodes : RegisterBuggy(n)
    \/ \E n \in Nodes : AdmissionWake(n)
    \/ \E n \in Nodes : CommitSuspend(n)
    \/ StartSet
    \/ \E n \in Nodes : DrainOne(n)
    \/ FinishSet
    \/ ResetBuggy
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

\* The expected violated property: InvSetEpochIsolation (E6).
\* A waiter admitted after a reset (registrationGeneration = G1) that is woken
\* by an older set epoch (wakeEpochGen = G0, with G0 < G1) violates the law.
InvSetEpochIsolation ==
    \A n \in Nodes :
        wakeEpochGen[n] # NoGen
        => registrationGeneration[n] <= wakeEpochGen[n]

\* Also asserted (must still hold): not the target of the defect.
InvSingleResolutionWinner ==
    \A n \in Nodes : resolvedCount[n] <= 1
InvSingleRunnablePublication ==
    wakeDispatched = resolvedCount[N0] + resolvedCount[N1]

=============================================================================
