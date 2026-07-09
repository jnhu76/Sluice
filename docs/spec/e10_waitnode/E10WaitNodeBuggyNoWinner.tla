------------------------------- MODULE E10WaitNodeBuggyNoWinner -------------------------------
(*
  NEGATIVE model (E10 §12): the protocol defect where wake and cancellation do
  NOT share one winner authority — each resolver independently transitions to
  its terminal state WITHOUT a winner CAS, so BOTH can "win" for the same node
  (double completion). This is the §2/§7 violation the correct model forbids.

  Production analogue: imagine wake_one() did `state_ = Woken; unlink;` and
  cancel() did `state_ = Cancelled; unlink;` with no CAS — both could fire and
  the second overwrites the first's outcome, unlinking twice (corrupting links)
  and double-dispatching a scheduler wake. The correct model's resolve_ CAS
  prevents exactly this.

  This buggy variant produces an InvNoDoubleCompletion counterexample:
  resolvedCount[n] reaches 2 (both ResolveWakeBuggy and ResolveCancelBuggy fire
  for the same node). It documents WHY the single winner CAS is load-bearing.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1

VARIABLES nodeState, linked, resolvedCount, wakeDispatched

ASSUME /\ Nodes # {} /\ N0 \in Nodes /\ N1 \in Nodes /\ N0 # N1

Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
    /\ linked' = [linked EXCEPT ![n] = TRUE]
    /\ UNCHANGED <<resolvedCount, wakeDispatched>>

(* BUG: no winner CAS. Enabled whenever the node is Registered OR already
   terminal — it unconditionally (re)writes the state and re-unlinks, double-
   counting. This is the defect. *)
ResolveWakeBuggy(n) ==
    /\ nodeState[n] # "Detached"            \* no CAS: fires even if already terminal
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1

ResolveCancelBuggy(n) ==
    /\ nodeState[n] # "Detached"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1

Stutter == UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched>>

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : ResolveWakeBuggy(n)
    \/ \E n \in Nodes : ResolveCancelBuggy(n)

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0

Spec == Init /\ [][Next]_<<nodeState, linked, resolvedCount, wakeDispatched>>

(* The property the buggy model VIOLATES: at most one winning resolution. *)
InvNoDoubleCompletion ==
    \A n \in Nodes : resolvedCount[n] <= 1

=============================================================================
