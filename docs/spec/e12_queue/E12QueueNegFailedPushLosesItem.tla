------------------------------- MODULE E12QueueNegFailedPushLosesItem -------------------------------
(*
  NEG-QUEUE-7: FailedPushLosesItem. PushClosed does NOT record the original item in failedPushItem (records NoItem), so the failed-push result loses track of the exact original lease.
  Single-rule difference from E12QueueClosed: the ONLY defect is in PushClosed.
  Expected first invariant violation: FailedPushRetainsOriginalItem.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS PNodes, P0, P1, P2, CNodes, C0, C1, C2, Items, I0, I1, I2, Capacity

Location == {"Detached", "ProducerOp", "Ring", "ConsumerOp", "Released"}
NodeState == {"Detached", "Registered", "Woken", "Closed"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
Completion == {"None", "Pending", "Committed", "ClosedOutcome"}
QueueState == {"Open", "Closed"}
ActionKind == {"Init", "FastPush", "PushBlock", "PushClosed", "ProdRegister",
               "ProdSuspend", "ProdGrant", "ProdClosedGrant",
               "FastPop", "PopBlock", "PopClosedEmpty", "ConsRegister",
               "ConsSuspend", "ConsGrant", "ConsClosedGrant",
               "ReleaseItem", "Close", "IdempotentClose", "Stutter"}

NoItem == "NoItem"
NoNode == "NoNode"
NoSnap == "NoSnap"

ASSUME /\ PNodes # {}
       /\ CNodes # {}
       /\ Items # {}
       /\ P0 \in PNodes
       /\ P1 \in PNodes
       /\ P2 \in PNodes
       /\ P0 # P1 /\ P1 # P2 /\ P0 # P2
       /\ C0 \in CNodes
       /\ C1 \in CNodes
       /\ C2 \in CNodes
       /\ C0 # C1 /\ C1 # C2 /\ C0 # C2
       /\ I0 \in Items
       /\ I1 \in Items
       /\ I2 \in Items
       /\ I0 # I1 /\ I1 # I2 /\ I0 # I2
       /\ Capacity \in 1..2
       /\ Capacity <= Cardinality(Items)

VARIABLES
    ring, itemLoc, prodItem, consItem, prodState, consState,
    prodPhase, consPhase, prodWaiters, consWaiters,
    prodCompletion, consCompletion, prodResolved, consResolved,
    wakeDispatched, queueState,
    \* ---- HISTORY (ghost) ----
    lastAction, lastProdGranted, lastConsGranted,
    expectedProdHead, expectedConsHead, lastCommitter,
    \* failedPushItem[p] = the ItemId that producer epoch p's failed/closed
    \* push result owns (HISTORY for B4); NoItem if p never had a failed push.
    failedPushItem,
    \* admittedItem[p] = the ItemId that producer epoch p admitted to the queue
    \* (HISTORY for B4); NoItem if p never admitted. Used so
    \* FailedPushRetainsOriginalItem can require the failed result to own the
    \* EXACT admitted item (not NoItem, not a different item).
    admittedItem,
    \* closedRing: a snapshot of the ring at close linearization (HISTORY for
    \* B3/B6). NoSnap until the first close; CloseLinearize latches ring.
    closedRing,
    \* consumerDrained: set of ItemIds properly released by a consumer via
    \* ReleaseItem (HISTORY for B6). Distinguishes a consumer-drained Released
    \* item from a close-discarded one (which skips ConsumerOp entirely).
    consumerDrained

Vars == <<ring, itemLoc, prodItem, consItem, prodState, consState,
          prodPhase, consPhase, prodWaiters, consWaiters,
          prodCompletion, consCompletion, prodResolved, consResolved,
          wakeDispatched, queueState, lastAction, lastProdGranted,
          lastConsGranted, expectedProdHead, expectedConsHead, lastCommitter,
          failedPushItem, admittedItem, closedRing, consumerDrained>>

-------------------------------------------------------------------------------
InSeq(q, x) == \E i \in 1..Len(q) : q[i] = x
RemoveFromSeq(q, x) == SelectSeq(q, LAMBDA y : y # x)

ProdEligible(p) ==
    /\ prodState[p] = "Registered"
    /\ prodPhase[p] = "Suspended"
    /\ prodItem[p] # NoItem

ConsEligible(c) ==
    /\ consState[c] = "Registered"
    /\ consPhase[c] = "Suspended"

ProdEligibleSet == {p \in PNodes : ProdEligible(p)}
ConsEligibleSet == {c \in CNodes : ConsEligible(c)}
ProdEligibleQueue == SelectSeq(prodWaiters, LAMBDA p : ProdEligible(p))
ConsEligibleQueue == SelectSeq(consWaiters, LAMBDA c : ConsEligible(c))
ProdFIFOHead == IF ProdEligibleQueue = <<>> THEN NoNode ELSE Head(ProdEligibleQueue)
ConsFIFOHead == IF ConsEligibleQueue = <<>> THEN NoNode ELSE Head(ConsEligibleQueue)

\* Fast push requires Open + space + no eligible older producer (no barging).
FastPushAdmissible ==
    /\ queueState = "Open"
    /\ Len(ring) < Capacity
    /\ ProdEligibleSet = {}

\* An item is "consumed" as some epoch's failed-push result; re-admitting the
\* same ItemId would alias the lease. In the one-shot-lease abstraction an
\* ItemId denotes one lease object, so once it has been returned to a caller as
\* a failed-push result it cannot be re-admitted by another epoch (the caller
\* would mint a NEW lease / new ItemId).
ItemIsFailedResult(it) ==
    \E q \in PNodes : failedPushItem[q] = it

\* Admission requires a fresh, unclaimed item.
FreshItem(it) == ~ItemIsFailedResult(it)

\* Fast pop requires ring nonempty + no eligible older consumer. Pop is allowed
\* after close as long as buffered items remain (drain-on-close).
FastPopAdmissible ==
    /\ Len(ring) > 0
    /\ ConsEligibleSet = {}

MarkOther(act) ==
    /\ lastAction' = act
    /\ lastProdGranted' = NoNode
    /\ lastConsGranted' = NoNode
    /\ expectedProdHead' = NoNode
    /\ expectedConsHead' = NoNode
    /\ lastCommitter' = NoNode

-------------------------------------------------------------------------------
Init ==
    /\ ring = <<>>
    /\ itemLoc = [i \in Items |-> "Detached"]
    /\ prodItem = [p \in PNodes |-> NoItem]
    /\ consItem = [c \in CNodes |-> NoItem]
    /\ prodState = [p \in PNodes |-> "Detached"]
    /\ consState = [c \in CNodes |-> "Detached"]
    /\ prodPhase = [p \in PNodes |-> "NoAdmission"]
    /\ consPhase = [c \in CNodes |-> "NoAdmission"]
    /\ prodWaiters = <<>>
    /\ consWaiters = <<>>
    /\ prodCompletion = [p \in PNodes |-> "None"]
    /\ consCompletion = [c \in CNodes |-> "None"]
    /\ prodResolved = [p \in PNodes |-> 0]
    /\ consResolved = [c \in CNodes |-> 0]
    /\ wakeDispatched = 0
    /\ queueState = "Open"
    /\ lastAction = "Init"
    /\ lastProdGranted = NoNode
    /\ lastConsGranted = NoNode
    /\ expectedProdHead = NoNode
    /\ expectedConsHead = NoNode
    /\ lastCommitter = NoNode
    /\ failedPushItem = [p \in PNodes |-> NoItem]
    /\ admittedItem = [p \in PNodes |-> NoItem]
    /\ closedRing = NoSnap
    /\ consumerDrained = {}


-------------------------------------------------------------------------------
\* PRODUCER SIDE

FastPushCommit(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ FreshItem(it)
    /\ prodItem[p] = NoItem
    /\ FastPushAdmissible
    /\ itemLoc' = [itemLoc EXCEPT ![it] = "Ring"]
    /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
    /\ ring' = Append(ring, it)
    /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
    /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "Committed"]
    /\ admittedItem' = [admittedItem EXCEPT ![p] = it]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("FastPush")
    /\ UNCHANGED <<prodWaiters, consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, failedPushItem, closedRing, consumerDrained>>

\* P3 PushClosed: the queue is Closed -> the producer's lease is returned to
\* the caller (producer_operation -> detached). The failed result OWNS the
\* exact original ItemId (failedPushItem records it). No ring entry, no copy.
PushClosed(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ FreshItem(it)
    /\ prodItem[p] = NoItem
    /\ queueState = "Closed"
    /\ itemLoc' = [itemLoc EXCEPT ![it] = "Detached"]
    /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
    /\ failedPushItem' = [failedPushItem EXCEPT ![p] = NoItem]  \* DEFECT: loses the original item (records NoItem != it)
    /\ admittedItem' = [admittedItem EXCEPT ![p] = it]
    /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
    /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "ClosedOutcome"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("PushClosed")
    /\ UNCHANGED <<ring, prodWaiters, consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, closedRing, consumerDrained>>
PushWouldBlock(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ FreshItem(it)
    /\ prodItem[p] = NoItem
    /\ queueState = "Open"
    /\ ~FastPushAdmissible
    /\ itemLoc' = [itemLoc EXCEPT ![it] = "Detached"]
    /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
    /\ failedPushItem' = [failedPushItem EXCEPT ![p] = it]
    /\ admittedItem' = [admittedItem EXCEPT ![p] = it]
    /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
    /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "None"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("PushBlock")
    /\ UNCHANGED <<ring, prodWaiters, consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, closedRing, consumerDrained>>

ProducerWaitRegister(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ FreshItem(it)
    /\ prodItem[p] = NoItem
    /\ queueState = "Open"
    /\ ~FastPushAdmissible
    /\ itemLoc' = [itemLoc EXCEPT ![it] = "ProducerOp"]
    /\ prodItem' = [prodItem EXCEPT ![p] = it]
    /\ admittedItem' = [admittedItem EXCEPT ![p] = it]
    /\ prodState' = [prodState EXCEPT ![p] = "Registered"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "AdmissionOpen"]
    /\ prodWaiters' = Append(prodWaiters, p)
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "Pending"]
    /\ MarkOther("ProdRegister")
    /\ UNCHANGED <<consWaiters, consState, consPhase, consCompletion, consItem, consResolved, prodResolved, ring, wakeDispatched, queueState, failedPushItem, closedRing, consumerDrained>>

ProducerSuspend(p) ==
    /\ prodState[p] = "Registered"
    /\ prodPhase[p] = "AdmissionOpen"
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "Suspended"]
    /\ MarkOther("ProdSuspend")
    /\ UNCHANGED <<ring, itemLoc, prodItem, prodState, prodCompletion, prodResolved, wakeDispatched, prodWaiters, consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

\* P6 + PUB-P-COMM: Open + ring has space + FIFO head.
ProducerGrantCommit ==
    /\ queueState = "Open"
    /\ ProdFIFOHead # NoNode
    /\ Len(ring) < Capacity
    /\ expectedProdHead' = ProdFIFOHead
    /\ LET p == ProdFIFOHead IN
       /\ LET it == prodItem[p] IN
          /\ itemLoc[it] = "ProducerOp"
          /\ itemLoc' = [itemLoc EXCEPT ![it] = "Ring"]
          /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
          /\ ring' = Append(ring, it)
          /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
          /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
          /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
          /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "Committed"]
          /\ prodWaiters' = RemoveFromSeq(prodWaiters, p)
          /\ lastCommitter' = p
          /\ lastProdGranted' = p
          /\ lastConsGranted' = NoNode
          /\ expectedConsHead' = NoNode
          /\ lastAction' = "ProdGrant"
          /\ wakeDispatched' = wakeDispatched + 1
          /\ UNCHANGED <<consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

\* P7 + PUB-P-CLOSED: Closed + producer FIFO head. The producer's item stays
\* with the failed result (producer_operation -> detached, failedPushItem
\* records it). No ring entry.
ProducerClosedCommit ==
    /\ queueState = "Closed"
    /\ ProdFIFOHead # NoNode
    /\ expectedProdHead' = ProdFIFOHead
    /\ LET p == ProdFIFOHead IN
       /\ LET it == prodItem[p] IN
          /\ itemLoc[it] = "ProducerOp"
          /\ itemLoc' = [itemLoc EXCEPT ![it] = "Detached"]
          /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
          /\ failedPushItem' = [failedPushItem EXCEPT ![p] = it]
          /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
          /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
          /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
          /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "ClosedOutcome"]
          /\ prodWaiters' = RemoveFromSeq(prodWaiters, p)
          /\ lastCommitter' = p
          /\ lastProdGranted' = p
          /\ lastConsGranted' = NoNode
          /\ expectedConsHead' = NoNode
          /\ lastAction' = "ProdClosedGrant"
          /\ wakeDispatched' = wakeDispatched + 1
          /\ UNCHANGED <<ring, consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, admittedItem, closedRing, consumerDrained>>

-------------------------------------------------------------------------------
\* CONSUMER SIDE

FastPopCommit(c) ==
    /\ consState[c] = "Detached"
    /\ consPhase[c] = "NoAdmission"
    /\ consItem[c] = NoItem
    /\ FastPopAdmissible
    /\ LET it == Head(ring) IN
       /\ itemLoc[it] = "Ring"
       /\ itemLoc' = [itemLoc EXCEPT ![it] = "ConsumerOp"]
       /\ consItem' = [consItem EXCEPT ![c] = it]
       /\ ring' = Tail(ring)
       /\ consState' = [consState EXCEPT ![c] = "Woken"]
       /\ consPhase' = [consPhase EXCEPT ![c] = "NoAdmission"]
       /\ consResolved' = [consResolved EXCEPT ![c] = consResolved[c] + 1]
       /\ consCompletion' = [consCompletion EXCEPT ![c] = "Committed"]
       /\ wakeDispatched' = wakeDispatched + 1
       /\ MarkOther("FastPop")
       /\ UNCHANGED <<prodWaiters, consWaiters, prodState, prodPhase, prodCompletion, prodItem, prodResolved, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

\* C2 PopClosedEmpty: Closed + empty -> the consumer receives Closed.
PopClosedEmpty(c) ==
    /\ consState[c] = "Detached"
    /\ consPhase[c] = "NoAdmission"
    /\ consItem[c] = NoItem
    /\ queueState = "Closed"
    /\ Len(ring) = 0
    /\ ConsEligibleSet = {}
    /\ consState' = [consState EXCEPT ![c] = "Closed"]
    /\ consPhase' = [consPhase EXCEPT ![c] = "NoAdmission"]
    /\ consResolved' = [consResolved EXCEPT ![c] = consResolved[c] + 1]
    /\ consCompletion' = [consCompletion EXCEPT ![c] = "ClosedOutcome"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("PopClosedEmpty")
    /\ UNCHANGED <<ring, itemLoc, prodWaiters, consWaiters, prodState, prodPhase, prodCompletion, prodItem, prodResolved, consItem, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

PopWouldBlock(c) ==
    /\ consState[c] = "Detached"
    /\ consPhase[c] = "NoAdmission"
    /\ consItem[c] = NoItem
    /\ ~FastPopAdmissible
    /\ \* would-block is only the open-and-nonterminal case; closed+empty
       \* goes through PopClosedEmpty instead.
       ~(queueState = "Closed" /\ Len(ring) = 0 /\ ConsEligibleSet = {})
    /\ consState' = [consState EXCEPT ![c] = "Woken"]
    /\ consPhase' = [consPhase EXCEPT ![c] = "NoAdmission"]
    /\ consResolved' = [consResolved EXCEPT ![c] = consResolved[c] + 1]
    /\ consCompletion' = [consCompletion EXCEPT ![c] = "None"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("PopBlock")
    /\ UNCHANGED <<prodWaiters, consWaiters, prodState, prodPhase, prodCompletion, prodItem, prodResolved, itemLoc, consItem, ring, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

ConsumerWaitRegister(c) ==
    /\ consState[c] = "Detached"
    /\ consPhase[c] = "NoAdmission"
    /\ consItem[c] = NoItem
    /\ ~FastPopAdmissible
    /\ ~(queueState = "Closed" /\ Len(ring) = 0)
    /\ consState' = [consState EXCEPT ![c] = "Registered"]
    /\ consPhase' = [consPhase EXCEPT ![c] = "AdmissionOpen"]
    /\ consWaiters' = Append(consWaiters, c)
    /\ consCompletion' = [consCompletion EXCEPT ![c] = "Pending"]
    /\ MarkOther("ConsRegister")
    /\ UNCHANGED <<prodWaiters, prodState, prodPhase, prodCompletion, prodItem, prodResolved, ring, wakeDispatched, itemLoc, consItem, consResolved, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

ConsumerSuspend(c) ==
    /\ consState[c] = "Registered"
    /\ consPhase[c] = "AdmissionOpen"
    /\ consPhase' = [consPhase EXCEPT ![c] = "Suspended"]
    /\ MarkOther("ConsSuspend")
    /\ UNCHANGED <<ring, itemLoc, consItem, consState, consCompletion, consResolved, wakeDispatched, prodWaiters, consWaiters, prodState, prodPhase, prodCompletion, prodItem, prodResolved, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

\* C5 + PUB-C-COMM: ring nonempty + FIFO head. Allowed after close (drain).
ConsumerGrantCommit ==
    /\ ConsFIFOHead # NoNode
    /\ Len(ring) > 0
    /\ expectedConsHead' = ConsFIFOHead
    /\ LET c == ConsFIFOHead IN
       /\ LET it == Head(ring) IN
          /\ itemLoc[it] = "Ring"
          /\ itemLoc' = [itemLoc EXCEPT ![it] = "ConsumerOp"]
          /\ consItem' = [consItem EXCEPT ![c] = it]
          /\ ring' = Tail(ring)
          /\ consState' = [consState EXCEPT ![c] = "Woken"]
          /\ consPhase' = [consPhase EXCEPT ![c] = "NoAdmission"]
          /\ consResolved' = [consResolved EXCEPT ![c] = consResolved[c] + 1]
          /\ consCompletion' = [consCompletion EXCEPT ![c] = "Committed"]
          /\ consWaiters' = RemoveFromSeq(consWaiters, c)
          /\ lastCommitter' = NoNode
          /\ lastConsGranted' = c
          /\ lastProdGranted' = NoNode
          /\ expectedProdHead' = NoNode
          /\ lastAction' = "ConsGrant"
          /\ wakeDispatched' = wakeDispatched + 1
          /\ UNCHANGED <<prodWaiters, prodState, prodPhase, prodCompletion, prodItem, prodResolved, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

\* C6 + PUB-C-CLOSED: Closed + empty + consumer FIFO head -> Closed.
ConsumerClosedCommit ==
    /\ queueState = "Closed"
    /\ Len(ring) = 0
    /\ ConsFIFOHead # NoNode
    /\ expectedConsHead' = ConsFIFOHead
    /\ LET c == ConsFIFOHead IN
       /\ consState' = [consState EXCEPT ![c] = "Closed"]
       /\ consPhase' = [consPhase EXCEPT ![c] = "NoAdmission"]
       /\ consResolved' = [consResolved EXCEPT ![c] = consResolved[c] + 1]
       /\ consCompletion' = [consCompletion EXCEPT ![c] = "ClosedOutcome"]
       /\ consWaiters' = RemoveFromSeq(consWaiters, c)
       /\ lastCommitter' = NoNode
       /\ lastConsGranted' = c
       /\ lastProdGranted' = NoNode
       /\ expectedProdHead' = NoNode
       /\ lastAction' = "ConsClosedGrant"
       /\ wakeDispatched' = wakeDispatched + 1
       /\ UNCHANGED <<ring, itemLoc, consItem, prodWaiters, prodState, prodPhase, prodCompletion, prodItem, prodResolved, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

ReleaseItem(c) ==
    /\ consState[c] = "Woken"
    /\ consItem[c] # NoItem
    /\ LET it == consItem[c] IN
       /\ itemLoc[it] = "ConsumerOp"
       /\ itemLoc' = [itemLoc EXCEPT ![it] = "Released"]
       /\ consItem' = [consItem EXCEPT ![c] = NoItem]
       /\ consumerDrained' = consumerDrained \cup {it}
       /\ MarkOther("ReleaseItem")
       /\ UNCHANGED <<ring, prodWaiters, consWaiters, prodState, consState, prodPhase, consPhase, prodCompletion, consCompletion, prodResolved, consResolved, prodItem, wakeDispatched, queueState, failedPushItem, admittedItem, closedRing>>

-------------------------------------------------------------------------------
\* CLOSE: CL1 Open -> Closed (monotonic linearization point under G+S).
CloseLinearize ==
    /\ queueState = "Open"
    /\ queueState' = "Closed"
    /\ closedRing' = ring
    /\ lastAction' = "Close"
    /\ lastProdGranted' = NoNode
    /\ lastConsGranted' = NoNode
    /\ expectedProdHead' = NoNode
    /\ expectedConsHead' = NoNode
    /\ lastCommitter' = NoNode
    /\ UNCHANGED <<ring, itemLoc, prodItem, consItem, prodState, consState, prodPhase, consPhase, prodWaiters, consWaiters, prodCompletion, consCompletion, prodResolved, consResolved, wakeDispatched, failedPushItem, admittedItem, consumerDrained>>

\* CL2 Idempotent close: Closed -> Closed (re-reconcile fixed point). The
\* authoritative state is unchanged (Closed is absorbing); only the ghost
\* lastAction records that a second close call was serviced and re-reconciled.
IdempotentClose ==
    /\ queueState = "Closed"
    /\ lastAction' = "IdempotentClose"
    /\ lastProdGranted' = NoNode
    /\ lastConsGranted' = NoNode
    /\ expectedProdHead' = NoNode
    /\ expectedConsHead' = NoNode
    /\ lastCommitter' = NoNode
    /\ UNCHANGED <<ring, itemLoc, prodItem, consItem, prodState, consState, prodPhase, consPhase, prodWaiters, consWaiters, prodCompletion, consCompletion, prodResolved, consResolved, wakeDispatched, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

Stutter ==
    /\ MarkOther("Stutter")
    /\ UNCHANGED <<ring, itemLoc, prodItem, consItem, prodState, consState, prodPhase, consPhase, prodWaiters, consWaiters, prodCompletion, consCompletion, prodResolved, consResolved, wakeDispatched, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>

-------------------------------------------------------------------------------
Next ==
    \/ Stutter
    \/ CloseLinearize
    \/ IdempotentClose
    \/ \E p \in PNodes, it \in Items : FastPushCommit(p, it)
    \/ \E p \in PNodes, it \in Items : PushClosed(p, it)
    \/ \E p \in PNodes, it \in Items : PushWouldBlock(p, it)
    \/ \E p \in PNodes, it \in Items : ProducerWaitRegister(p, it)
    \/ \E p \in PNodes : ProducerSuspend(p)
    \/ ProducerGrantCommit
    \/ ProducerClosedCommit
    \/ \E c \in CNodes : FastPopCommit(c)
    \/ \E c \in CNodes : PopClosedEmpty(c)
    \/ \E c \in CNodes : PopWouldBlock(c)
    \/ \E c \in CNodes : ConsumerWaitRegister(c)
    \/ \E c \in CNodes : ConsumerSuspend(c)
    \/ ConsumerGrantCommit
    \/ ConsumerClosedCommit
    \/ \E c \in CNodes : ReleaseItem(c)

Spec == Init /\ [][Next]_Vars

-------------------------------------------------------------------------------
\* Invariants. Each is a PURE STATE PREDICATE (no primed variable).

ItemClaimCount(it) ==
    (IF itemLoc[it] = "Ring" THEN 1 ELSE 0)
  + Cardinality({p \in PNodes : prodItem[p] = it})
  + Cardinality({c \in CNodes : consItem[c] = it})

\* Carry-over from Model A (unchanged semantics): capacity, owner, ring
\* uniqueness, FIFO buffer, no-barging, waiter FIFOs, single resolution /
\* publication, location consistency. These remain load-bearing in Model B.
CapacityBound == /\ 0 <= Len(ring) /\ Len(ring) <= Capacity

UniqueRingItem ==
    \A i, j \in 1..Len(ring) : i # j => ring[i] # ring[j]

FIFOBufferOrder ==
    /\ \A it \in Items : (itemLoc[it] = "Ring") = InSeq(ring, it)
    /\ \A i \in 1..Len(ring) : itemLoc[ring[i]] = "Ring"

NoDuplicatedItem == \A it \in Items : ItemClaimCount(it) <= 1

LocationConsistency ==
    /\ \A it \in Items : (itemLoc[it] = "ProducerOp")
          => Cardinality({p \in PNodes : prodItem[p] = it}) = 1
    /\ \A it \in Items : (itemLoc[it] = "ConsumerOp")
          => Cardinality({c \in CNodes : consItem[c] = it}) = 1
    /\ \A it \in Items : (itemLoc[it] = "Ring") <=> InSeq(ring, it)
    /\ \A it \in Items : (itemLoc[it] = "Detached") => ~InSeq(ring, it)
    /\ \A it \in Items :
        (itemLoc[it] = "Released")
          => /\ ~InSeq(ring, it)
             /\ ~\E p \in PNodes : prodItem[p] = it
             /\ ~\E c \in CNodes : consItem[c] = it

ProducerWaiterFIFO ==
    lastAction = "ProdGrant" => lastProdGranted = expectedProdHead
ConsumerWaiterFIFO ==
    lastAction = "ConsGrant" => lastConsGranted = expectedConsHead

NoBarging ==
    /\ lastAction = "FastPush" => ProdEligibleSet = {}
    /\ lastAction = "FastPop"  => ConsEligibleSet = {}

InvSingleResolution ==
    /\ \A p \in PNodes : prodResolved[p] <= 1
    /\ \A c \in CNodes : consResolved[c] <= 1

InvSinglePublication ==
    wakeDispatched = (0
        + Cardinality({p \in PNodes : prodState[p] \in {"Woken", "Closed"}})
        + Cardinality({c \in CNodes : consState[c] \in {"Woken", "Closed"}}))

-------------------------------------------------------------------------------
\* B1: ClosedAbsorbing. queueState is Open or Closed; once Closed it never
\* returns to Open. IdempotentClose keeps it Closed (a self-loop on the
\* authoritative state). The pure-state corollary: only the Open->Closed
\* transition is a close linearization; no action sets queueState'="Open".
\* The invariant asserts the domain and that the last close-side action did
\* not reopen.
ClosedAbsorbing ==
    /\ queueState \in {"Open", "Closed"}
    /\ \* no action reopens: the last action that touched queueState left it
       \* Closed iff it was a close. (CloseLinearize is the only mutator and it
       \* is monotonic; this is the state-side witness.)
       lastAction \in {"Close", "IdempotentClose"} => queueState = "Closed"

\* B2: NoCommitAfterClose. After close, no producer commit enters the ring.
\* A commit-into-ring action (FastPush / ProducerGrant) could only have happened
\* while the queue was Open. We assert: the last ring-mutating producer commit
\* did not occur against a Closed queue. Since FastPush/ProdGrant require
\* queueState="Open" in their guards, this reduces to: those actions are not
\* the last action while queueState="Closed". Modelled as: if the last action
\* was a ring-commit producer step, the queue must (still) be Open at that
\* commit -- which we capture by requiring that a Closed queue's last action is
\* not a producer ring-commit.
NoCommitAfterClose ==
    queueState = "Closed"
      => lastAction \notin {"FastPush", "ProdGrant"}

\* Is seq s a contiguous suffix of seq t? (close only ever drains from the
\* head, so the post-close ring must be a suffix of the close-time snapshot.)
IsSuffixOf(s, t) ==
    \E k \in 0..Len(t) :
        /\ Len(s) = Len(t) - k
        /\ \A i \in 1..Len(s) : s[i] = t[k + i]

\* B3: CommittedBeforeCloseRemainsDrainable. Any item committed to the ring
\* before close remains drainable. We snapshot the ring at close (closedRing);
\* afterwards every item that was buffered at close is still accounted for --
\* it is either still in the ring (which is a suffix of closedRing), or has
\* been drained to a consumer operation, or has been released by a consumer.
\* No item committed before close is ever lost or moved back to the producer.
CommittedBeforeCloseRemainsDrainable ==
    /\ \A it \in Items : (itemLoc[it] = "Ring") => InSeq(ring, it)
    /\ \A it \in Items :
        itemLoc[it] \in {"Detached", "ProducerOp"} => ~InSeq(ring, it)
    /\ (closedRing # NoSnap) =>
        /\ \* the post-close ring is a suffix of the close-time ring (no insert,
           \* head-only drain).
           IsSuffixOf(ring, closedRing)
        /\ \* every item buffered at close is still tracked (drainable) -- in the
           \* ring, in a consumer op, or released BY A CONSUMER.
           \A i \in 1..Len(closedRing) :
               LET it == closedRing[i] IN
               /\ it \in Items
               /\ (InSeq(ring, it)
                   \/ itemLoc[it] = "ConsumerOp"
                   \/ (itemLoc[it] = "Released" /\ it \in consumerDrained)
                   \/ \E c \in CNodes : consItem[c] = it)

\* helper set: the range of failedPushItem (items owned by failed-push results)
failedPushItemRange == {it \in Items :
    \E p \in PNodes : failedPushItem[p] = it}

\* B4: FailedPushRetainsOriginalItem. A producer epoch that admitted an item
\* and reached a FAILED terminal outcome (would-block None, or ClosedOutcome)
\* must have its failed-push result own the EXACT admitted item -- not NoItem,
\* not a different item, no copy, no default-construct, no alias. We track
\* admittedItem (ghost) at admission and require failedPushItem = admittedItem
\* for every failed epoch. We also require the failed item's location be
\* Detached (returned to caller), not in the ring / a consumer / Released, and
\* not aliased to another epoch's failed result.
\* A producer epoch is "failed-terminal" iff it admitted an item AND its
\* completion is None (would-block) or ClosedOutcome (closed/expired).
IsFailedTerminal(p) ==
    /\ admittedItem[p] # NoItem
    /\ prodState[p] = "Woken"
    /\ prodCompletion[p] \in {"None", "ClosedOutcome"}

FailedPushRetainsOriginalItem ==
    \A p \in PNodes :
        /\ \* the failed result owns the EXACT admitted item (no loss/alias)
           IsFailedTerminal(p) => failedPushItem[p] = admittedItem[p]
        /\ \* the failed result's item is Detached (returned to caller)
           failedPushItem[p] # NoItem =>
               LET it == failedPushItem[p] IN
               /\ it \in Items
               /\ itemLoc[it] = "Detached"
               /\ ~InSeq(ring, it)
               /\ ~\E c \in CNodes : consItem[c] = it
               /\ \* no alias: no OTHER producer epoch's failed result owns it
                  \A q \in PNodes : q # p => failedPushItem[q] # it
               /\ ~\E q \in PNodes : prodItem[q] = it

\* B5: ClosedEmptyConsumerTerminal. At Closed + empty + no eligible consumer,
\* a fresh consumer cannot receive an item (there are none) and the model must
\* offer the Closed outcome. We assert the stable state: at Closed+empty there
\* is no consumer holding an item it could not have obtained, and any Woken
\* consumer at that point carries a Committed item only if the ring was
\* nonempty beforehand (which the transition relation enforces). The crisp
\* invariant: a consumer that became Closed did so via the closed-empty path
\* and owns no item.
ClosedEmptyConsumerTerminal ==
    \A c \in CNodes :
        consState[c] = "Closed"
          => /\ consItem[c] = NoItem
             /\ consCompletion[c] = "ClosedOutcome"

\* B6: NoBufferedItemDiscardOnClose. Close never discards buffered items: a
\* close-side action (Close, IdempotentClose, PushClosed, ProdClosedGrant,
\* ConsClosedGrant, PopClosedEmpty) never empties the ring or moves a ring item
\* out of the queue. The only actions that may shrink the ring are consumer
\* drains (FastPop, ConsGrant), and the only way a buffered item becomes
\* Released is via the consumer drain path (ConsumerOp -> ReleaseItem). We
\* snapshot the ring at close (closedRing) and track consumer-drained items
\* (consumerDrained), then assert, at every post-close state:
\*   (a) the current ring is a suffix of the close-time ring (head-only drain,
\*       no insertions);
\*   (b) every item that was buffered at close is still tracked in the queue --
\*       in the current ring, in a consumer operation, or released BY A CONSUMER
\*       (in consumerDrained) -- never lost and never Released by a non-consumer;
\*   (c) no former buffered item leaked back to the producer path.
\* This directly forbids a defect that clears the ring and marks ring items
\* Released on close (they are NOT in consumerDrained, so clause (b) fires).
NoBufferedItemDiscardOnClose ==
    (closedRing # NoSnap) =>
        /\ \* (a) head-only drain: post-close ring is a suffix of close-time ring.
           IsSuffixOf(ring, closedRing)
        /\ \* (b) every buffered item at close is still tracked via a LEGAL path:
           \*    still in ring | in a consumer op | released BY A CONSUMER.
           \A i \in 1..Len(closedRing) :
               LET it == closedRing[i] IN
               /\ it \in Items
               /\ (InSeq(ring, it)
                   \/ itemLoc[it] = "Ring"
                   \/ itemLoc[it] = "ConsumerOp"
                   \/ \E c \in CNodes : consItem[c] = it
                   \/ (itemLoc[it] = "Released" /\ it \in consumerDrained))
        /\ \* (c) no former buffered item leaked to the producer path, and none
           \* is Released without going through a consumer drain.
           \A i \in 1..Len(closedRing) :
               LET it == closedRing[i] IN
               it \in Items =>
                   /\ itemLoc[it] \notin {"Detached", "ProducerOp"}
                   /\ (itemLoc[it] = "Released" => it \in consumerDrained)

\* B7: CloseProducerRaceLinearizable. Close and producer commit are serialized
\* by the G+S critical section: exactly one linearizes first. If close won, no
\* item entered the ring on/after that close (B2). If the producer commit won,
\* the item is in the ring and close observes it (drains it). We model the
\* serialization directly: every ring-mutating producer step and every close
\* step is atomic and re-checks queueState. The invariant asserts the outcome:
\* at Closed, the producer commits that did happen are exactly those that
\* linearized before close (their items are in the ring or already drained),
\* and no producer commit that observed Closed entered the ring.
CloseProducerRaceLinearizable ==
    /\ (queueState = "Closed")
        => \A p \in PNodes :
               prodCompletion[p] = "Committed"
                 => \* a Committed producer's item is in the ring or was
                    \* drained to a consumer (never lost, never returned).
                    \E it \in Items :
                        /\ failedPushItem[p] # it
                        /\ (itemLoc[it] \in {"Ring", "ConsumerOp", "Released"}
                            \/ \E c \in CNodes : consItem[c] = it)

-------------------------------------------------------------------------------
Inv == /\ CapacityBound
       /\ UniqueRingItem
       /\ FIFOBufferOrder
       /\ NoDuplicatedItem
       /\ LocationConsistency
       /\ ProducerWaiterFIFO
       /\ ConsumerWaiterFIFO
       /\ NoBarging
       /\ InvSingleResolution
       /\ InvSinglePublication
       /\ ClosedAbsorbing
       /\ NoCommitAfterClose
       /\ CommittedBeforeCloseRemainsDrainable
       /\ FailedPushRetainsOriginalItem
       /\ ClosedEmptyConsumerTerminal
       /\ NoBufferedItemDiscardOnClose
       /\ CloseProducerRaceLinearizable

=============================================================================

