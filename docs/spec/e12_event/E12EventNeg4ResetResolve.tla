--------------------- MODULE E12EventNeg4ResetResolve ---------------------
(*
  NEG-EVENT-4 -- Reset Resolves Waiter (E12-A-EVENT-CORRECTIVE-1, Corrective J).

  Single broken rule: ResetBuggy changes one Registered waiter to terminal
  (Woken) and records the "Reset" resolution cause. The correct ResetEvent is a
  pure state flip that NEVER touches a node.

  Counterexample: a node that was Registered becomes terminal with
  resolutionCause = "Reset". Violated expected property: InvResetNonResolution
  (E5).

  The resolutionCause variable is written by EVERY modeled terminal-resolution
  action (AdmissionWake -> AdmissionSet, DrainOne -> SetBroadcast, ResolveCancel
  -> Cancel) AND by the buggy ResetBuggy -> Reset. It is not a constant forced
  to pass: the bug writes the "Reset" value, which the property forbids.
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
\* ResolutionCause is extended with "Reset" so the buggy ResetBuggy can write it
\* (the correct model never produces "Reset").
ResolutionCause == {"None", "AdmissionSet", "SetBroadcast", "Cancel", "Reset"}
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

\* DEFECT: ResetBuggy changes ONE Registered waiter to terminal (Woken) and
\* records the "Reset" resolution cause. The correct ResetEvent never touches a
\* node. Here ResetBuggy both flips eventSet AND resolves a victim (if any
\* Registered node exists).
ResetBuggy(n) ==
    /\ protoPhase = "Idle"
    /\ eventSet = "SET"
    /\ resetGeneration < MaxGen
    /\ nodeState[n] = "Registered"
    /\ eventSet' = "UNSET"
    /\ resetGeneration' = resetGeneration + 1
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ resolutionCause' = [resolutionCause EXCEPT ![n] = "Reset"]
    /\ UNCHANGED <<admissionPhase, admissionSawSet, wokenBySetDrain,
                  protoPhase, registrationGeneration, activeSetGen, wakeEpochGen>>

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
    \/ StartSet
    \/ \E n \in Nodes : DrainOne(n)
    \/ FinishSet
    \/ \E n \in Nodes : ResetBuggy(n)
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

\* The expected violated property: InvResetNonResolution (E5). A terminal node
\* must NEVER have the "Reset" resolution cause. The buggy ResetBuggy writes it.
InvResetNonResolution ==
    \A n \in Nodes :
        isTerminal(n) => resolutionCause[n] # "Reset"

=============================================================================
