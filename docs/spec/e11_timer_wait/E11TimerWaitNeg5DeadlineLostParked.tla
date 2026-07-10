----------------------- MODULE E11TimerWaitNeg5DeadlineLostParked -----------------------
(*
  E11 NEG-5 — Deadline Lost While Scheduler Is Parked (BROKEN liveness model).

  Defect: once the Scheduler Parks, the clock-driving Tick and the timer
  resolver are NOT re-enabled (there is no Unpark/wake-back path and Tick is
  disabled while parked). So if a deadline becomes due while parked, it can
  never resolve: the wait strands forever even though the deadline is due. In
  production this is the missing bounded-park-timeout / timer pump from the
  worker loop — a parked Worker with an active deadline must not stay parked
  past that deadline.

  Broken protocol:
      active deadline exists
      Worker observes no runnable work
      Worker parks
      (clock already at/advanced past the deadline)
      no Tick / ResolveTimer re-enabled from the parked state
      -> deadline due forever, wait unresolved forever, parked forever

  Required liveness counterexample (under fairness):
      a state where an Active deadline is due, the node is Registered, and the
      Scheduler is parked, repeats forever (the deadline is never resolved).

  MODEL DESIGN (M5 one-rule difference). Identical to E11TimerWait's liveness
  structure EXCEPT Tick is disabled while parked (/\ ~parked) and there is no
  Unpark action. Under fairness on ResolveTimer and Tick, the correct model
  resolves a due Active deadline; this buggy model cannot, because once parked
  Tick cannot advance to make a future deadline due AND ResolveTimer — though
  enabled by its own guard — is never forced if the deadline was made due
  before parking and the parked state stutters. The result: a fairness-respecting
  infinite trace with a due Active deadline never resolved.
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

(* DEFECT: the timer pump does NOT run while the Scheduler is parked (no
   bounded park-timeout re-loops the worker to pump deadlines). Combined with
   Tick being disabled while parked, a due deadline can never resolve once the
   Scheduler parks — exactly the lost-deadline-while-parked bug. (Correct model:
   a parked worker is woken/re-loops at the earliest deadline and pumps.) *)
ResolveTimer(r) ==
    /\ ~parked
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

(* Tick advances time. DEFECT: Tick is disabled while parked — a parked worker
   does not advance/drive the timer. A deadline that becomes due while parked
   (or was already due) cannot be driven to resolution from the parked state. *)
Tick ==
    /\ ~parked
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, parked>>

(* Park: the Scheduler idles. DEFECT: there is NO Unpark action — once parked,
   the Scheduler stays parked forever (only Stutter is enabled, since Tick is
   disabled while parked and Register/Resolve require non-parked-relevant state
   but the due deadline is stranded). The correct model bounds park by the
   earliest deadline and re-enables the timer pump; this model omits both. *)
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

(* Fairness on the timer resolver and Tick (the actions that, in the correct
   model, drive a due deadline to resolution). The buggy model VIOLATES
   liveness DESPITE this fairness, because Tick is disabled while parked and
   there is no Unpark — the defect, not the absence of fairness, loses the
   deadline. Bare WF conjuncts (NOT wrapped in []); mirrors E9 LivenessSpec. *)
FairResolveTimer == WF_Vars(\E r \in Regs : ResolveTimer(r))
FairTick == WF_Vars(Tick)
FairPark == WF_Vars(Park)
LivenessSpecFair ==
    Spec
    /\ FairResolveTimer
    /\ FairTick
    /\ FairPark

(* I6 — Deadline Park Liveness: a Registered wait with a due Active deadline is
   eventually terminal. The buggy model VIOLATES this: register a deadline that
   is already due, then Park — Tick is disabled while parked (cannot change
   anything) and with WF on Park the parked state repeats, stranding the due
   deadline forever. Form [](P => <>Q), matching the correct model + E9 Life2. *)
DeadlineParkLiveness ==
    \A r \in Regs :
        [] ( (/\ regState[r] = "Active"
              /\ now >= regDeadline[r]
              /\ nodeState[regEpoch[r]] = "Registered")
             => <> (isTerminal(regEpoch[r])) )

=============================================================================
