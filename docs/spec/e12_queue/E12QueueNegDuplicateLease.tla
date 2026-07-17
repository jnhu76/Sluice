------------------------------- MODULE E12QueueNegDuplicateLease -------------------------------
(*
  NEG-QUEUE-1: DuplicateLease. FastPushCommit appends the item to the ring TWICE for one admission (two ring slots own the same ItemId -- a duplicate lease).
  Single-rule difference from E12Queue: the ONLY defect is in FastPushCommit.
  Expected first invariant violation: UniqueRingItem.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS PNodes, P0, P1, P2, CNodes, C0, C1, C2, Items, I0, I1, I2, Capacity

(* PNodes is the (small, exhaustive) producer-epoch set; CNodes the consumer-
   epoch set; Items the small ItemId set; Capacity bounds the ring. Three
   epochs per role + Capacity 1 is the smallest bound in which a producer is
   forced to WAIT (ring full with one item while a second producer wants to
   push) and a consumer is forced to WAIT (ring empty while a consumer wants
   to pop), so the selected-waiter grant paths (P6/C5) and the no-barging /
   FIFO-waiter invariants are non-vacuously exercised. *)

VARIABLES
    \* @type: Seq(ItemId)        the FIFO lease ring (ordering authority)
    ring,
    \* @type: [ItemId -> Location]  one-shot control location table
    itemLoc,
    \* @type: [PNode -> ItemId \cup {NoItem}]  item a producer epoch operates on
    prodItem,
    \* @type: [CNode -> ItemId \cup {NoItem}]  item a consumer epoch operates on
    consItem,
    \* @type: [PNode -> NodeState]   producer-epoch lifecycle
    prodState,
    \* @type: [CNode -> NodeState]   consumer-epoch lifecycle
    consState,
    \* @type: [PNode -> AdmissionPhase]
    prodPhase,
    \* @type: [CNode -> AdmissionPhase]
    consPhase,
    \* @type: Seq(PNode)          producer waiter FIFO
    prodWaiters,
    \* @type: Seq(CNode)          consumer waiter FIFO
    consWaiters,
    \* @type: [PNode -> Completion] producer-epoch Queue completion
    prodCompletion,
    \* @type: [CNode -> Completion] consumer-epoch Queue completion
    consCompletion,
    \* @type: [PNode -> 0..1]     terminal resolution count (single resolution)
    prodResolved,
    \* @type: [CNode -> 0..1]
    consResolved,
    \* @type: Int                 total runnable publications (single publication)
    wakeDispatched,
    \* ---- HISTORY: ghost evidence so transition properties are state invariants ----
    \* @type: ActionKind
    lastAction,
    \* @type: PNode \cup {NoNode}
    lastProdGranted,
    \* @type: CNode \cup {NoNode}
    lastConsGranted,
    \* @type: PNode \cup {NoNode}
    expectedProdHead,
    \* @type: CNode \cup {NoNode}
    expectedConsHead,
    \* @type: PNode \cup {NoNode}   epoch whose completion was finalized in the
    \*                              last commit step (A10/A11 winner-before-pub)
    lastCommitter

Location == {"Detached", "ProducerOp", "Ring", "ConsumerOp", "Released"}
NodeState == {"Detached", "Registered", "Woken"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
Completion == {"None", "Pending", "Committed"}
ActionKind == {"Init", "FastPush", "PushBlock", "ProdRegister", "ProdGrant",
               "FastPop", "PopBlock", "ConsRegister", "ConsGrant",
               "ReleaseItem", "Stutter"}

\* Sentinels. Items I0/I1/I2 are model values; a distinct sentinel is needed
\* for "no item bound yet".
NoItem == "NoItem"
NoNode == "NoNode"

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
       \* Capacity must be <= |Items| so the ring can be full at least once,
       \* which is required for the PushBlock / no-barging coverage to be
       \* non-vacuous.
       /\ Capacity <= Cardinality(Items)

Vars == <<ring, itemLoc, prodItem, consItem, prodState, consState,
          prodPhase, consPhase, prodWaiters, consWaiters,
          prodCompletion, consCompletion, prodResolved, consResolved,
          wakeDispatched, lastAction, lastProdGranted, lastConsGranted,
          expectedProdHead, expectedConsHead, lastCommitter>>

-------------------------------------------------------------------------------
\* Membership in a sequence (value-membership; x \in seq tests domain only).
InSeq(q, x) == \E i \in 1..Len(q) : q[i] = x

\* Order-preserving removal of all occurrences of x from q (there is at most
\* one, by the waiter-FIFO well-formedness invariant). SelectSeq preserves the
\* relative order of the remaining elements.
RemoveFromSeq(q, x) ==
    SelectSeq(q, LAMBDA y : y # x)

\* Producer epoch eligibility for a reconciliation grant: Registered, Suspended
\* (admission window committed), and still holding an admitted item.
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

\* Fast Push is admissible: ring has space AND no eligible producer waiter has
\* FIFO priority (no barging, A9). Idempotent with Semaphore ImmediateAdmissible.
FastPushAdmissible ==
    /\ Len(ring) < Capacity
    /\ ProdEligibleSet = {}

\* Fast Pop is admissible: ring nonempty AND no eligible consumer waiter.
FastPopAdmissible ==
    /\ Len(ring) > 0
    /\ ConsEligibleSet = {}

\* HISTORY helper: mark a non-grant action.
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
    /\ lastAction = "Init"
    /\ lastProdGranted = NoNode
    /\ lastConsGranted = NoNode
    /\ expectedProdHead = NoNode
    /\ expectedConsHead = NoNode
    /\ lastCommitter = NoNode

-------------------------------------------------------------------------------
\* PRODUCER SIDE

\* P1+P2 Fast Push commit: an external producer arrives with a fresh item,
\* ring has space and no eligible older producer is linked (no barging).
\* detatched -> producer_operation -> ring in one atomic step; the source
\* operation is emptied (prodItem -> NoItem). Completion is finalized Committed
\* BEFORE publication (A10/A11).
FastPushCommit(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ prodItem[p] = NoItem
    /\ FastPushAdmissible
    /\ itemLoc' = [itemLoc EXCEPT ![it] = "Ring"]
    /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
    /\ ring' = Append(Append(ring, it), it)  \* DEFECT: double-append (duplicate lease)
    /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
    /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "Committed"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("FastPush")
    /\ UNCHANGED <<prodWaiters, consWaiters, consState, consPhase,
                   consCompletion, consItem, consResolved>>
PushWouldBlock(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ prodItem[p] = NoItem
    /\ ~FastPushAdmissible
    /\ \* try-push would-block: the item does NOT enter the ring; the caller's
       \* lease is returned (Detached). The producer epoch completes Woken with
       \* no item held and no Queue commit.
       /\ itemLoc' = [itemLoc EXCEPT ![it] = "Detached"]
       /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
    /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
    /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "None"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("PushBlock")
    /\ UNCHANGED <<ring, prodWaiters, consWaiters, consState, consPhase,
                   consCompletion, consItem, consResolved>>

\* P5 Producer wait admission: the producer parks in the producer FIFO. The
\* item moves detached -> producer_operation and stays there until grant/close.
ProducerWaitRegister(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ prodItem[p] = NoItem
    /\ ~FastPushAdmissible
    /\ itemLoc' = [itemLoc EXCEPT ![it] = "ProducerOp"]
    /\ prodItem' = [prodItem EXCEPT ![p] = it]
    /\ prodState' = [prodState EXCEPT ![p] = "Registered"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "AdmissionOpen"]
    /\ prodWaiters' = Append(prodWaiters, p)
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "Pending"]
    /\ MarkOther("ProdRegister")
    /\ UNCHANGED <<consWaiters, consState, consPhase, consCompletion,
                   consItem, consResolved, prodResolved, ring, wakeDispatched>>

\* P5/P6 Admission suspend commit: closes the register/suspend window. Once
\* Suspended the producer is eligible for a reconciler grant.
ProducerSuspend(p) ==
    /\ prodState[p] = "Registered"
    /\ prodPhase[p] = "AdmissionOpen"
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "Suspended"]
    /\ MarkOther("ProdRegister")
    /\ UNCHANGED <<ring, itemLoc, prodItem, prodState, prodCompletion,
                   prodResolved, wakeDispatched, prodWaiters, consWaiters,
                   consState, consPhase, consCompletion, consItem, consResolved>>

\* P6 + PUB-P-COMM Reconciler producer grant: Open + ring has space + FIFO head.
\* producer_operation -> ring (source emptied), winner BEFORE publication.
\* HISTORY: latch expectedProdHead BEFORE queue mutation.
ProducerGrantCommit ==
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
          /\ UNCHANGED <<consWaiters, consState, consPhase, consCompletion,
                         consItem, consResolved>>

-------------------------------------------------------------------------------
\* CONSUMER SIDE

\* C1 Fast Pop commit: ring nonempty + no older eligible consumer. ring ->
\* consumer_operation (source ring slot emptied). Completion finalized Committed
\* BEFORE publication.
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
       /\ UNCHANGED <<prodWaiters, consWaiters, prodState, prodPhase,
                      prodCompletion, prodItem, prodResolved>>

\* C3 Try-pop would-block (ring empty OR an older eligible consumer is linked).
\* The consumer receives would_block; no item moves.
PopWouldBlock(c) ==
    /\ consState[c] = "Detached"
    /\ consPhase[c] = "NoAdmission"
    /\ consItem[c] = NoItem
    /\ ~FastPopAdmissible
    /\ consState' = [consState EXCEPT ![c] = "Woken"]
    /\ consPhase' = [consPhase EXCEPT ![c] = "NoAdmission"]
    /\ consResolved' = [consResolved EXCEPT ![c] = consResolved[c] + 1]
    /\ consCompletion' = [consCompletion EXCEPT ![c] = "None"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("PopBlock")
    /\ UNCHANGED <<prodWaiters, consWaiters, prodState, prodPhase,
                   prodCompletion, prodItem, prodResolved, itemLoc, consItem,
                   ring>>

\* C4 Consumer wait admission.
ConsumerWaitRegister(c) ==
    /\ consState[c] = "Detached"
    /\ consPhase[c] = "NoAdmission"
    /\ consItem[c] = NoItem
    /\ ~FastPopAdmissible
    /\ consState' = [consState EXCEPT ![c] = "Registered"]
    /\ consPhase' = [consPhase EXCEPT ![c] = "AdmissionOpen"]
    /\ consWaiters' = Append(consWaiters, c)
    /\ consCompletion' = [consCompletion EXCEPT ![c] = "Pending"]
    /\ MarkOther("ConsRegister")
    /\ UNCHANGED <<prodWaiters, prodState, prodPhase, prodCompletion,
                   prodItem, prodResolved, ring, wakeDispatched, itemLoc,
                   consItem, consResolved>>

\* C4 admission suspend commit.
ConsumerSuspend(c) ==
    /\ consState[c] = "Registered"
    /\ consPhase[c] = "AdmissionOpen"
    /\ consPhase' = [consPhase EXCEPT ![c] = "Suspended"]
    /\ MarkOther("ConsRegister")
    /\ UNCHANGED <<ring, itemLoc, consItem, consState, consCompletion,
                   consResolved, wakeDispatched, prodWaiters, consWaiters,
                   prodState, prodPhase, prodCompletion, prodItem, prodResolved>>

\* C5 + PUB-C-COMM Reconciler consumer grant: ring nonempty + FIFO head. ring ->
\* consumer_operation (source slot emptied), winner BEFORE publication.
\* HISTORY: latch expectedConsHead BEFORE queue mutation.
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
          /\ UNCHANGED <<prodWaiters, prodState, prodPhase, prodCompletion,
                         prodItem, prodResolved>>

\* C9 Release item: the consumer_operation completes and the item is released
\* (typed destruction outside locks). consumer_operation -> released.
ReleaseItem(c) ==
    /\ consState[c] = "Woken"
    /\ consItem[c] # NoItem
    /\ LET it == consItem[c] IN
       /\ itemLoc[it] = "ConsumerOp"
       /\ itemLoc' = [itemLoc EXCEPT ![it] = "Released"]
       /\ consItem' = [consItem EXCEPT ![c] = NoItem]
       /\ MarkOther("ReleaseItem")
       /\ UNCHANGED <<ring, prodWaiters, consWaiters, prodState, consState,
                     prodPhase, consPhase, prodCompletion, consCompletion,
                     prodResolved, consResolved, prodItem, wakeDispatched>>

-------------------------------------------------------------------------------
\* Stutter: no-op (quiescence). Ensures Next is always enabled and the model
\* cannot deadlock merely for lack of a next step.
Stutter ==
    /\ MarkOther("Stutter")
    /\ UNCHANGED <<ring, itemLoc, prodItem, consItem, prodState, consState,
                  prodPhase, consPhase, prodWaiters, consWaiters,
                  prodCompletion, consCompletion, prodResolved, consResolved,
                  wakeDispatched>>

-------------------------------------------------------------------------------
Next ==
    \/ Stutter
    \/ \E p \in PNodes, it \in Items : FastPushCommit(p, it)
    \/ \E p \in PNodes, it \in Items : PushWouldBlock(p, it)
    \/ \E p \in PNodes, it \in Items : ProducerWaitRegister(p, it)
    \/ \E p \in PNodes : ProducerSuspend(p)
    \/ ProducerGrantCommit
    \/ \E c \in CNodes : FastPopCommit(c)
    \/ \E c \in CNodes : PopWouldBlock(c)
    \/ \E c \in CNodes : ConsumerWaitRegister(c)
    \/ \E c \in CNodes : ConsumerSuspend(c)
    \/ ConsumerGrantCommit
    \/ \E c \in CNodes : ReleaseItem(c)

Spec == Init /\ [][Next]_Vars

-------------------------------------------------------------------------------
\* Invariants. Each is a PURE STATE PREDICATE (no primed variable). Actions
\* freely use primes; invariants do not.

\* A1: CapacityBound.
CapacityBound ==
    /\ 0 <= Len(ring)
    /\ Len(ring) <= Capacity

\* A2: UniqueItemOwner. Every live ItemId has exactly one non-released control
\* location AND that location is the single owner: the operation handles and
\* ring membership are consistent with it. An item is "live" (has an owner
\* inside the queue) iff its location is Ring / ProducerOp / ConsumerOp; for
\* those, exactly one claim exists and it matches the location. Detached items
\* are at the typed boundary (owned by the caller, not by the queue) and have
\* zero queue claims; Released items likewise have zero claims.
ItemClaimCount(it) ==
    (IF itemLoc[it] = "Ring" THEN 1 ELSE 0)
  + Cardinality({p \in PNodes : prodItem[p] = it})
  + Cardinality({c \in CNodes : consItem[c] = it})

UniqueItemOwner ==
    \A it \in Items :
        \* live (queue-owned) items have exactly one claim matching the location
        /\ (itemLoc[it] = "Ring"
              => /\ ItemClaimCount(it) = 1
                 /\ ~\E p \in PNodes : prodItem[p] = it
                 /\ ~\E c \in CNodes : consItem[c] = it)
        /\ (itemLoc[it] = "ProducerOp"
              => /\ ItemClaimCount(it) = 1
                 /\ Cardinality({p \in PNodes : prodItem[p] = it}) = 1
                 /\ ~\E c \in CNodes : consItem[c] = it)
        /\ (itemLoc[it] = "ConsumerOp"
              => /\ ItemClaimCount(it) = 1
                 /\ ~\E p \in PNodes : prodItem[p] = it
                 /\ Cardinality({c \in CNodes : consItem[c] = it}) = 1)
        /\ (itemLoc[it] = "Detached"
              => /\ ~\E p \in PNodes : prodItem[p] = it
                 /\ ~\E c \in CNodes : consItem[c] = it)
        /\ (itemLoc[it] = "Released"
              => /\ ~\E p \in PNodes : prodItem[p] = it
                 /\ ~\E c \in CNodes : consItem[c] = it)

\* A3: UniqueRingItem. ring ItemIds are duplicate-free (one lease per slot).
UniqueRingItem ==
    \A i, j \in 1..Len(ring) :
        i # j => ring[i] # ring[j]

\* A4: NoLostItem. Every ADMITTED item (location in Ring/ProducerOp/
\* ConsumerOp) is still tracked by exactly one claim -- it has not vanished
\* into a state the model cannot account for. (Detached items are at the typed
\* boundary awaiting admission and are tracked by the caller; Released items
\* are deliberately unclaimed.)
NoLostItem ==
    \A it \in Items :
        itemLoc[it] \in {"Ring", "ProducerOp", "ConsumerOp"}
        => ItemClaimCount(it) = 1

\* A5: NoDuplicatedItem. No item is claimed by two places at once (no item
\* appears in the ring twice, nor in the ring and an operation, nor in two
\* operations). Combined with NoLostItem this is the one-lease-per-item law.
NoDuplicatedItem ==
    \A it \in Items :
        ItemClaimCount(it) <= 1

\* A6: FIFOBufferOrder. ring preserves insertion order -- modelled directly by
\* keeping ring as a Seq and only ever Append-ing to its tail and Tail-ing its
\* head. The invariant asserts ring membership equals the set of items whose
\* location is Ring (no extra/missing) and there are no duplicates (already
\* UniqueRingItem). This pins that ring IS the authoritative ordering.
FIFOBufferOrder ==
    /\ \A it \in Items :
        (itemLoc[it] = "Ring") = InSeq(ring, it)
    /\ \A i \in 1..Len(ring) : itemLoc[ring[i]] = "Ring"

\* A7: ProducerWaiterFIFO (history-backed). The last producer grant granted
\* exactly the eligible FIFO head latched before queue mutation.
ProducerWaiterFIFO ==
    lastAction = "ProdGrant" => lastProdGranted = expectedProdHead

\* A8: ConsumerWaiterFIFO (history-backed).
ConsumerWaiterFIFO ==
    lastAction = "ConsGrant" => lastConsGranted = expectedConsHead

\* A9: NoBarging. A fast push cannot commit while an eligible producer waiter
\* is linked; a fast pop cannot commit while an eligible consumer waiter is
\* linked. Equivalent to the stable-state form: if there is an eligible
\* producer waiter, the ring is either full OR a producer grant is the only
\* path that can add to it. We assert the stable admission state: an eligible
\* producer waiter excludes a fast-push pre-state, and vice versa.
NoBarging ==
    /\ \* no fast push could have just committed over an eligible producer
       \* waiter: if the last action was FastPush, there must currently be no
       \* eligible producer waiter (eligible waiters only arise from park-then-
       \* suspend; a fast push that committed required ProdEligibleSet={}).
       lastAction = "FastPush" => ProdEligibleSet = {}
    /\ lastAction = "FastPop"  => ConsEligibleSet = {}

\* A10: CommittedBeforePublished (history-backed). The last publication step
\* finalized the winner's completion BEFORE incrementing wakeDispatched. We
\* capture this by requiring that the last-published committer (a producer
\* epoch for ProdGrant, the consumer epoch for ConsGrant, or the fast-path
\* epoch itself) is in a terminal Committed state. FastPush/FastPop always
\* commit the acting epoch; grants commit the FIFO head.
CommittedBeforePublished ==
    /\ (lastAction = "FastPush")
        => \E p \in PNodes :
               prodState[p] = "Woken" /\ prodCompletion[p] = "Committed"
    /\ (lastAction = "FastPop")
        => \E c \in CNodes :
               consState[c] = "Woken" /\ consCompletion[c] = "Committed"
    /\ (lastAction = "ProdGrant")
        => lastCommitter # NoNode
           /\ prodState[lastCommitter] = "Woken"
           /\ prodCompletion[lastCommitter] = "Committed"
    /\ (lastAction = "ConsGrant")
        => \E c \in CNodes :
               consState[c] = "Woken" /\ consCompletion[c] = "Committed"

\* A11: NoPublishedPendingCompletion. No publication without a finalized
\* completion. The would-block paths (PushBlock/PopBlock) set completion=None
\* but they DO increment wakeDispatched -- so we explicitly exclude them: a
\* publication that represents a Queue commit (FastPush/FastPop/ProdGrant/
\* ConsGrant) must carry a Committed completion for its winner. The would-block
\* publications are not Queue commits and carry no completion obligation.
NoPublishedPendingCompletion ==
    \* Every wakeDispatched-incrementing action is either a would-block
    \* (explicitly completion=None, by construction) or a commit whose winner
    \* is Committed. This is what CommittedBeforePublished already pins for the
    \* commit actions; we additionally forbid a commit action whose winner is
    \* Pending/None.
    /\ (lastAction = "ProdGrant")
        => prodCompletion[lastCommitter] = "Committed"
    /\ (lastAction \in {"FastPush", "FastPop", "ConsGrant"})
        => TRUE

\* A12: LocationConsistency. The control-location table (itemLoc) is
\* consistent with the operation handles (prodItem / consItem) and the ring.
\* An item in ProducerOp must be held by a producer epoch; an item in
\* ConsumerOp by a consumer epoch; an item in Ring must be in the ring; an
\* item Detached or Released must be held by no operation and (if Released)
\* not in the ring.
LocationConsistency ==
    /\ \A it \in Items :
        (itemLoc[it] = "ProducerOp")
          => Cardinality({p \in PNodes : prodItem[p] = it}) = 1
    /\ \A it \in Items :
        (itemLoc[it] = "ConsumerOp")
          => Cardinality({c \in CNodes : consItem[c] = it}) = 1
    /\ \A it \in Items :
        (itemLoc[it] = "Ring") <=> InSeq(ring, it)
    /\ \A it \in Items :
        (itemLoc[it] = "Detached")
          => ~InSeq(ring, it)
    /\ \A it \in Items :
        (itemLoc[it] = "Released")
          => /\ ~InSeq(ring, it)
             /\ ~\E p \in PNodes : prodItem[p] = it
             /\ ~\E c \in CNodes : consItem[c] = it

\* Supporting structural invariants (not named in the task but required for a
\* sound model): single resolution per epoch, single publication count, waiter
\* FIFO well-formedness. These mirror the Semaphore structural invariants.
InvSingleResolution ==
    /\ \A p \in PNodes : prodResolved[p] <= 1
    /\ \A c \in CNodes : consResolved[c] <= 1

InvSinglePublication ==
    wakeDispatched = (0
        + Cardinality({p \in PNodes : prodState[p] = "Woken"})
        + Cardinality({c \in CNodes : consState[c] = "Woken"}))

\* A waiter FIFO contains only Registered nodes in an admissible phase, and no
\* node appears twice.
InvProdWaitersWellFormed ==
    /\ \A i, j \in 1..Len(prodWaiters) :
        i # j => prodWaiters[i] # prodWaiters[j]
    /\ \A p \in PNodes :
        InSeq(prodWaiters, p) => prodState[p] = "Registered"
    /\ \A p \in PNodes :
        InSeq(prodWaiters, p) => prodPhase[p] \in {"AdmissionOpen", "Suspended"}

InvConsWaitersWellFormed ==
    /\ \A i, j \in 1..Len(consWaiters) :
        i # j => consWaiters[i] # consWaiters[j]
    /\ \A c \in CNodes :
        InSeq(consWaiters, c) => consState[c] = "Registered"
    /\ \A c \in CNodes :
        InSeq(consWaiters, c) => consPhase[c] \in {"AdmissionOpen", "Suspended"}

Inv == /\ CapacityBound
       /\ UniqueItemOwner
       /\ UniqueRingItem
       /\ NoLostItem
       /\ NoDuplicatedItem
       /\ FIFOBufferOrder
       /\ ProducerWaiterFIFO
       /\ ConsumerWaiterFIFO
       /\ NoBarging
       /\ CommittedBeforePublished
       /\ NoPublishedPendingCompletion
       /\ LocationConsistency
       /\ InvSingleResolution
       /\ InvSinglePublication
       /\ InvProdWaitersWellFormed
       /\ InvConsWaitersWellFormed

=============================================================================

