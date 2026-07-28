------------------------------- MODULE E11TimerWaitNeg3StaleCrossEpoch -------------------------------
(*
  E11 NEG-3 — Stale Timer Cross-Epoch Resolution (BROKEN model).

  This is the address-reuse boundary between I3 and I4. The defect: a timer
  registration captures only a REUSABLE STORAGE SLOT (the address / Fiber
  identity), not the logical wait epoch. When epoch E+1 reuses the same slot A,
  a stale timer registered for E resolves E+1.

  Storage-reuse trace (the load-bearing counterexample):
      epoch E uses storage slot A
      timer R_E captures only reusable storage A (NOT the epoch identity)
      E resolves (e.g. RESOURCE_WAKE); node E dies (slot A freed)
      epoch E+1 reuses storage slot A
      R_E expires
      R_E resolves E+1 (because it keys on slot A, which E+1 now occupies)

  Required counterexample:
      a timer registered for epoch E causes resolution of E+1 -> a second
      resolution for the slot, violating wait-epoch isolation (I3).

  MODEL DESIGN. Two slots (A0, A1) and two epochs (N0, N1). slotOf[n] maps each
  epoch to the slot its node occupies; slots are REUSABLE (DestroyNode frees the
  slot, and a later Register may claim it). The buggy ResolveTimer targets the
  slot the registration captured (regSlot[r]) rather than the epoch it was bound
  to (regEpoch[r]). When E+1 reuses E's slot, the stale timer for E resolves E+1.

  The CORRECT model keys the expiry on regEpoch[r] (the logical epoch), which is
  immutable and distinct per Register — so a stale timer for E can never reach
  E+1. Production realizes this by capturing a WaitNode reference (never only a
  Fiber pointer).

  REACHABILITY. Register(N0) claims slot A0 + Active R0 (regSlot[R0]=A0,
  regEpoch[R0]=N0). ResolveWake(N0) retires R0. DestroyNode(N0) frees slot A0.
  Register(N1) REUSES slot A0 (slotOf[N1]=A0). Now a stale ResolveTimer(R0)
  fires (if R0 were still Active) and, keying on regSlot[R0]=A0 = slotOf[N1],
  resolves N1. To make R0 still Active at expiry time, the buggy model does NOT
  retire R0 on ResolveWake (the defect is the slot-keyed expiry; the stale timer
  must remain armed). The counterexample witnesses resolvedCount[N1] >= 1 caused
  by R0 (bound to N0), violating InvSingleResolutionWinner.

  M5 one-rule difference: the defect is ResolveTimer keying on regSlot[r] (the
  reusable address) instead of regEpoch[r] (the immutable epoch identity).
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, Regs, R0, R1, Slots, A0, A1

VARIABLES
    nodeState, slotOf, slotFree, resolvedCount,
    regState, regEpoch, regSlot, regDeadline, now

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
RegState  == {"Inert", "Active", "Retired", "Consumed"}
DeadlineVal == 0..3

ASSUME
    /\ Nodes # {} /\ N0 \in Nodes /\ N1 \in Nodes /\ N0 # N1
    /\ Regs # {} /\ R0 \in Regs /\ R1 \in Regs /\ R0 # R1
    /\ Slots # {} /\ A0 \in Slots /\ A1 \in Slots /\ A0 # A1

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

(* Register: claim a free slot for this epoch, bind an Active registration that
   captures BOTH the epoch (regEpoch) AND the slot (regSlot). A fresh node
   occupies its slot (slotFree := FALSE). *)
Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ \E s \in Slots :
        /\ slotFree[s] = TRUE
        /\ \E r \in Regs :
            /\ regState[r] = "Inert"
            /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
            /\ slotOf' = [slotOf EXCEPT ![n] = s]
            /\ slotFree' = [slotFree EXCEPT ![s] = FALSE]
            /\ regState' = [regState EXCEPT ![r] = "Active"]
            /\ regEpoch' = [regEpoch EXCEPT ![r] = n]
            /\ regSlot' = [regSlot EXCEPT ![r] = s]
            /\ \E d \in DeadlineVal : regDeadline' = [regDeadline EXCEPT ![r] = d]
            /\ UNCHANGED <<now, resolvedCount>>

(* ResolveWake resolves the node. To isolate the cross-epoch defect (M5:
   one-rule difference = the slot-keyed expiry), the registration bound to n is
   NOT retired here — the buggy design's stale timer must stay armed so it can
   later fire on the reused slot. (The correct production design retires the
   reg in the same step; NEG-4 isolates that lifetime defect separately. Here
   the load-bearing bug is the slot-vs-epoch keying, so the reg stays Active to
   make the stale expiry reachable.) *)
ResolveWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ UNCHANGED <<slotOf, slotFree, regState, regEpoch, regSlot, regDeadline, now>>

(* DEFECT: ResolveTimer targets the SLOT the registration captured (regSlot[r]),
   resolving whichever epoch CURRENTLY occupies that slot — NOT the epoch the
   registration was bound to (regEpoch[r]). When E+1 reuses E's slot, a stale
   Active timer for E resolves E+1. (Correct model keys on regEpoch[r]: resolves
   exactly m = regEpoch[r], which is immutable and distinct per epoch.) *)
ResolveTimer(r) ==
    /\ regState[r] = "Active"
    /\ now >= regDeadline[r]
    /\ \E m \in Nodes :
        /\ slotOf[m] = regSlot[r]                 \* DEFECT: keys on slot, not epoch
        /\ nodeState[m] = "Registered"
        /\ regState' = [regState EXCEPT ![r] = "Consumed"]
        /\ nodeState' = [nodeState EXCEPT ![m] = "Expired"]
        /\ resolvedCount' = [resolvedCount EXCEPT ![m] = resolvedCount[m] + 1]
        /\ UNCHANGED <<slotOf, slotFree, regEpoch, regSlot, regDeadline, now>>

(* DestroyNode: free the slot so a later epoch may reuse it. Models the
   post-resolution lifetime window + storage reuse. The buggy design does not
   require the registration to be retired first (the stale timer stays armed),
   so the slot is freed while R0 is still Active — allowing N1 to reuse it and
   the stale slot-keyed R0 expiry to resolve N1. *)
DestroyNode(n) ==
    /\ isTerminal(n)
    /\ slotFree' = [slotFree EXCEPT ![slotOf[n]] = TRUE]
    /\ UNCHANGED <<nodeState, slotOf, resolvedCount,
                  regState, regEpoch, regSlot, regDeadline, now>>

Tick ==
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, slotOf, slotFree, resolvedCount,
                  regState, regEpoch, regSlot, regDeadline>>

Stutter == UNCHANGED <<nodeState, slotOf, slotFree, resolvedCount,
                       regState, regEpoch, regSlot, regDeadline, now>>

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : ResolveWake(n)
    \/ \E r \in Regs : ResolveTimer(r)
    \/ \E n \in Nodes : DestroyNode(n)
    \/ Tick

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ slotOf = [n \in Nodes |-> A0]
    /\ slotFree = [s \in Slots |-> TRUE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ regState = [r \in Regs |-> "Inert"]
    /\ regEpoch = [r \in Regs |-> N0]
    /\ regSlot = [r \in Regs |-> A0]
    /\ regDeadline = [r \in Regs |-> 0]
    /\ now = 0

Vars == <<nodeState, slotOf, slotFree, resolvedCount,
          regState, regEpoch, regSlot, regDeadline, now>>
Spec == Init /\ [][Next]_Vars

(* I3 — Wait-Epoch Isolation: a timer registration that fired (Consumed) must
   have resolved its OWN bound epoch (the node at regEpoch[r] is Expired), NOT a
   different epoch that merely reused the registration's captured slot. The
   buggy slot-keyed ResolveTimer consumes R (bound to N0) while expiring N1,
   leaving nodeState[regEpoch[R]=N0] = "Woken" (resolved by the wake, not by the
   timer) — so a Consumed registration whose bound epoch is NOT Expired is the
   cross-epoch-resolution witness. (The correct model keys ResolveTimer on
   regEpoch[r], so a Consumed R always coincides with its bound node Expired.) *)
InvWaitEpochIsolation ==
    \A r \in Regs :
        regState[r] = "Consumed" => nodeState[regEpoch[r]] = "Expired"

(* Also keep the single-resolution guard: a slot reused across two epochs must
   not accumulate two timer resolutions. *)
InvSingleResolutionWinner == \A n \in Nodes : resolvedCount[n] <= 1

=============================================================================
