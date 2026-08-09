------------------------------ MODULE D1UringPoison ------------------------------
(**
  Finite abstraction of Phase D1 io_uring permanent-submit recovery.

  The model deliberately separates:
    - transportLedger: actual monotonic logical SQ sequence;
    - identityLedger: Class-A proof identity recorded by the implementation;
    - physicalLedger: wrap-masked SQ storage position.

  It also models the load-bearing control ordering: an original operation CQE
  must remain deferred while its matching control is prepared/submitted.
  Permanent failure atomically proves the retained ledger Class-A; already
  confirmed entries remain Class-C and can retire only by CQE.
*)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Capacity, MaxSeq, OpSeq, ControlSeq, UseMaskedIdentity, DeferForControl,
          AllowPostPoisonSubmit

VARIABLES nextSeq,
          transportLedger,
          identityLedger,
          physicalLedger,
          sequenceHistory,
          identityHistory,
          confirmed,
          recovered,
          poisoned,
          opState,
          controlState,
          submitCount,
          submitCountAtPoison

vars == <<nextSeq, transportLedger, identityLedger, physicalLedger,
          sequenceHistory, identityHistory,
          confirmed, recovered, poisoned, opState, controlState,
          submitCount, submitCountAtPoison>>

OpStates == {"Outstanding", "Deferred", "ReadySuccess", "ReadyError"}
ControlStates == {"None", "Prepared", "Submitted", "Retired", "Recovered"}

Physical(seq) == (seq - 1) % Capacity

SeqSet(s) == {s[i] : i \in 1..Len(s)}

Prefix(s, count) == SubSeq(s, 1, count)
Suffix(s, count) == SubSeq(s, count + 1, Len(s))

Init ==
    /\ Capacity = 2
    /\ MaxSeq = 4
    /\ OpSeq = 1
    /\ ControlSeq = 2
    /\ nextSeq = 1
    /\ transportLedger = <<>>
    /\ identityLedger = <<>>
    /\ physicalLedger = <<>>
    /\ sequenceHistory = <<>>
    /\ identityHistory = <<>>
    /\ confirmed = {}
    /\ recovered = {}
    /\ poisoned = FALSE
    /\ opState = "Outstanding"
    /\ controlState = "None"
    /\ submitCount = 0
    /\ submitCountAtPoison = 0

Prepare ==
    /\ ~poisoned
    /\ nextSeq <= MaxSeq
    /\ Len(transportLedger) < Capacity
    /\ (nextSeq # ControlSeq \/ opState = "Outstanding")
    /\ transportLedger' = Append(transportLedger, nextSeq)
    /\ identityLedger' =
         Append(identityLedger,
                IF UseMaskedIdentity THEN Physical(nextSeq) ELSE nextSeq)
    /\ physicalLedger' = Append(physicalLedger, Physical(nextSeq))
    /\ sequenceHistory' = Append(sequenceHistory, nextSeq)
    /\ identityHistory' =
         Append(identityHistory,
                IF UseMaskedIdentity THEN Physical(nextSeq) ELSE nextSeq)
    /\ controlState' =
         IF nextSeq = ControlSeq THEN "Prepared" ELSE controlState
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<confirmed, recovered, poisoned, opState,
                   submitCount, submitCountAtPoison>>

PositiveSubmit(count) ==
    /\ ~poisoned
    /\ count \in 1..Len(transportLedger)
    /\ LET consumed == SeqSet(Prefix(transportLedger, count)) IN
         /\ confirmed' = confirmed \cup consumed
         /\ controlState' =
              IF ControlSeq \in consumed THEN "Submitted" ELSE controlState
    /\ transportLedger' = Suffix(transportLedger, count)
    /\ identityLedger' = Suffix(identityLedger, count)
    /\ physicalLedger' = Suffix(physicalLedger, count)
    /\ submitCount' = submitCount + 1
    /\ UNCHANGED <<nextSeq, sequenceHistory, identityHistory,
                   recovered, poisoned, opState,
                   submitCountAtPoison>>

PermanentFailure ==
    /\ ~poisoned
    /\ Len(transportLedger) > 0
    /\ poisoned' = TRUE
    /\ recovered' = recovered \cup SeqSet(transportLedger)
    /\ controlState' =
         IF ControlSeq \in SeqSet(transportLedger) THEN "Recovered"
         ELSE controlState
    /\ opState' =
         IF OpSeq \in SeqSet(transportLedger) THEN "ReadyError"
         ELSE IF opState = "Deferred" /\ ControlSeq \in SeqSet(transportLedger)
              THEN "ReadySuccess"
              ELSE opState
    /\ submitCount' = submitCount + 1
    /\ submitCountAtPoison' = submitCount + 1
    /\ UNCHANGED <<nextSeq, transportLedger, identityLedger, physicalLedger,
                   sequenceHistory, identityHistory,
                   confirmed>>

OperationCQE ==
    /\ OpSeq \in confirmed
    /\ opState = "Outstanding"
    /\ opState' =
         IF DeferForControl /\ controlState \in {"Prepared", "Submitted"}
         THEN "Deferred"
         ELSE "ReadySuccess"
    /\ UNCHANGED <<nextSeq, transportLedger, identityLedger, physicalLedger,
                   sequenceHistory, identityHistory,
                   confirmed, recovered, poisoned, controlState,
                   submitCount, submitCountAtPoison>>

ControlCQE ==
    /\ controlState = "Submitted"
    /\ controlState' = "Retired"
    /\ opState' = IF opState = "Deferred" THEN "ReadySuccess" ELSE opState
    /\ UNCHANGED <<nextSeq, transportLedger, identityLedger, physicalLedger,
                   sequenceHistory, identityHistory,
                   confirmed, recovered, poisoned,
                   submitCount, submitCountAtPoison>>

PostPoisonSubmit ==
    /\ AllowPostPoisonSubmit
    /\ poisoned
    /\ submitCount' = submitCount + 1
    /\ UNCHANGED <<nextSeq, transportLedger, identityLedger, physicalLedger,
                   sequenceHistory, identityHistory,
                   confirmed, recovered, poisoned, opState, controlState,
                   submitCountAtPoison>>

Quiescent ==
    /\ opState \in {"ReadySuccess", "ReadyError"}
    /\ controlState \notin {"Prepared", "Submitted"}
    /\ UNCHANGED vars

Next ==
    \/ Prepare
    \/ \E count \in 1..Capacity : PositiveSubmit(count)
    \/ PermanentFailure
    \/ OperationCQE
    \/ ControlCQE
    \/ PostPoisonSubmit
    \/ Quiescent

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ nextSeq \in 1..(MaxSeq + 1)
    /\ transportLedger \in Seq(0..MaxSeq)
    /\ identityLedger \in Seq(0..MaxSeq)
    /\ physicalLedger \in Seq(0..(Capacity - 1))
    /\ sequenceHistory \in Seq(0..MaxSeq)
    /\ identityHistory \in Seq(0..MaxSeq)
    /\ confirmed \subseteq 1..MaxSeq
    /\ recovered \subseteq 1..MaxSeq
    /\ poisoned \in BOOLEAN
    /\ opState \in OpStates
    /\ controlState \in ControlStates
    /\ submitCount \in Nat
    /\ submitCountAtPoison \in Nat

InvLedgerBound == Len(transportLedger) <= Capacity

InvLedgerShapes ==
    /\ Len(identityLedger) = Len(transportLedger)
    /\ Len(physicalLedger) = Len(transportLedger)
    /\ Len(sequenceHistory) = Len(identityHistory)

InvLogicalIdentity ==
    \A i, j \in 1..Len(sequenceHistory) :
        sequenceHistory[i] # sequenceHistory[j]
            => identityHistory[i] # identityHistory[j]

InvClassAUnconsumed == recovered \cap confirmed = {}

InvPoisonLedgerRecovered ==
    poisoned => SeqSet(transportLedger) \subseteq recovered

InvNoSubmitAfterPoison == poisoned => submitCount = submitCountAtPoison

InvReadyControlQuiescent ==
    opState \in {"ReadySuccess", "ReadyError"}
        => controlState \notin {"Prepared", "Submitted"}

InvSuccessWasSubmitted == opState = "ReadySuccess" => OpSeq \in confirmed

=============================================================================
