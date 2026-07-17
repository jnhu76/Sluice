#!/usr/bin/env python3
"""Generate the E12-E Queue negative models from the correct models.

Each NEG is a self-contained TLA+ module identical to its parent model EXCEPT
for ONE single-rule defect in ONE named action. The generator does a precise
block replacement of the action body; the rest is copied verbatim with the
module renamed. A header comment records the defect + expected invariant.

This is a build aid (not part of the formal gate). It mirrors
docs/spec/e12_semaphore/_gen_neg.py. Re-run after editing a parent model to
regenerate the negatives, then re-run scripts/verify-e12-queue-formal.sh.

Mapping (parent -> NEG -> expected invariant):
  E12Queue.tla (Model A):
    NegDuplicateLease       FastPushCommit     UniqueRingItem
    NegMoveNotEmptied       ProducerGrantCommit UniqueItemOwner
    NegBarging              FastPushCommit     NoBarging
    NegPublishBeforeCommit  ProducerGrantCommit NoPublishedPendingCompletion
  E12QueueClosed.tla (Model B):
    NegCommitAfterClose     FastPushCommit     NoCommitAfterClose
    NegCloseDiscardsBuffer  CloseLinearize     NoBufferedItemDiscardOnClose
    NegFailedPushLosesItem  PushClosed         FailedPushRetainsOriginalItem
"""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODEL_A = (HERE / "E12Queue.tla").read_text()
MODEL_B = (HERE / "E12QueueClosed.tla").read_text()

CFG_A = """\
SPECIFICATION Spec
INVARIANT {inv}

CONSTANTS
P0 = P0
P1 = P1
P2 = P2
C0 = C0
C1 = C1
C2 = C2
I0 = I0
I1 = I1
I2 = I2
PNodes = {{P0, P1, P2}}
CNodes = {{C0, C1, C2}}
Items = {{I0, I1, I2}}
Capacity = 1
"""


def replace_action(text, name, new_body):
    """Replace the action definition `name == ...` with new_body (which must
    include the `name == ...` opening line). The action block runs until the
    next top-level definition or a `----` separator."""
    lines = text.split("\n")
    start = next(i for i, ln in enumerate(lines)
                 if ln.startswith(name + " ==") or ln.startswith(name + "("))
    end = len(lines)
    for j in range(start + 1, len(lines)):
        ln = lines[j]
        if ln.startswith("----"):
            end = j
            break
        if ln and not ln[0].isspace() and " ==" in ln and not ln.startswith("\\*"):
            end = j
            break
    return "\n".join(lines[:start] + new_body.split("\n") + lines[end:])


def gen(parent_text, parent_mod, modname, header_desc, action_name, defect_body,
        expected_inv):
    text = parent_text.replace(f"MODULE {parent_mod}", f"MODULE {modname}", 1)
    text = replace_action(text, action_name, defect_body)
    body = text[text.index("EXTENDS"):]
    header = (
        f"------------------------------- MODULE {modname} "
        f"-------------------------------\n(*\n  {header_desc}\n"
        f"  Single-rule difference from {parent_mod}: the ONLY defect is in "
        f"{action_name}.\n  Expected first invariant violation: "
        f"{expected_inv}.\n*)\n"
    )
    (HERE / f"{modname}.tla").write_text(header + body + "\n")
    (HERE / f"{modname}.cfg").write_text(CFG_A.format(inv=expected_inv))
    print(f"{modname}: {action_name} -> {expected_inv}")


# ===========================================================================
# Model A negatives
# ===========================================================================

# NEG DuplicateLease: FastPushCommit appends the item to the ring TWICE for one
# admission (a duplicate lease / two ring slots own the same ItemId).
NEG_DUP = r"""FastPushCommit(p, it) ==
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
                   consCompletion, consItem, consResolved>>"""

# NEG MoveNotEmptied: ProducerGrantCommit moves producerOp->ring but leaves the
# source producer operation holding the item (prodItem[p] not cleared). The item
# is now claimed by both the ring and the producer operation.
NEG_MOVE = r"""ProducerGrantCommit ==
    /\ ProdFIFOHead # NoNode
    /\ Len(ring) < Capacity
    /\ expectedProdHead' = ProdFIFOHead
    /\ LET p == ProdFIFOHead IN
       /\ LET it == prodItem[p] IN
          /\ itemLoc[it] = "ProducerOp"
          /\ itemLoc' = [itemLoc EXCEPT ![it] = "Ring"]
          /\ prodItem' = prodItem  \* DEFECT: source not emptied (prodItem[p] still = it)
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
                         consItem, consResolved>>"""

# NEG Barging: FastPushCommit drops the no-eligible-producer guard, allowing a
# newcomer to fast-commit over an older eligible producer waiter. We mutate the
# FastPushAdmissible predicate (referenced by the guard) by overriding the whole
# FastPushCommit to not require FastPushAdmissible's producer-set clause: we
# replace the guard with the weaker "ring has space" only.
NEG_BARGE = r"""FastPushCommit(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ prodItem[p] = NoItem
    /\ Len(ring) < Capacity  \* DEFECT: dropped ProdEligibleSet = {} (barging)
    /\ itemLoc' = [itemLoc EXCEPT ![it] = "Ring"]
    /\ prodItem' = [prodItem EXCEPT ![p] = NoItem]
    /\ ring' = Append(ring, it)
    /\ prodState' = [prodState EXCEPT ![p] = "Woken"]
    /\ prodPhase' = [prodPhase EXCEPT ![p] = "NoAdmission"]
    /\ prodResolved' = [prodResolved EXCEPT ![p] = prodResolved[p] + 1]
    /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "Committed"]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ MarkOther("FastPush")
    /\ UNCHANGED <<prodWaiters, consWaiters, consState, consPhase,
                   consCompletion, consItem, consResolved>>"""

# NEG PublishBeforeCommit: ProducerGrantCommit increments wakeDispatched
# (publishes) but leaves the winner's completion Pending (not finalized before
# publication).
NEG_PUB = r"""ProducerGrantCommit ==
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
          /\ prodCompletion' = [prodCompletion EXCEPT ![p] = "Pending"]  \* DEFECT: not finalized before publication
          /\ prodWaiters' = RemoveFromSeq(prodWaiters, p)
          /\ lastCommitter' = p
          /\ lastProdGranted' = p
          /\ lastConsGranted' = NoNode
          /\ expectedConsHead' = NoNode
          /\ lastAction' = "ProdGrant"
          /\ wakeDispatched' = wakeDispatched + 1
          /\ UNCHANGED <<consWaiters, consState, consPhase, consCompletion,
                         consItem, consResolved>>"""

# ===========================================================================
# Model B negatives
# ===========================================================================

# NEG CommitAfterClose: FastPushCommit drops the queueState="Open" requirement,
# allowing a producer commit into the ring after close.
NEG_COMMITCLOSE = r"""FastPushCommit(p, it) ==
    /\ prodState[p] = "Detached"
    /\ prodPhase[p] = "NoAdmission"
    /\ it \in Items
    /\ itemLoc[it] = "Detached"
    /\ FreshItem(it)
    /\ prodItem[p] = NoItem
    /\ Len(ring) < Capacity
    /\ ProdEligibleSet = {}  \* DEFECT: dropped queueState = "Open" (commit after close)
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
    /\ UNCHANGED <<prodWaiters, consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, failedPushItem, closedRing, consumerDrained>>"""

# NEG CloseDiscardsBuffer: CloseLinearize discards buffered items (clears the
# ring and releases the items) instead of leaving them drainable. The ghost
# closedRing STILL snapshots the pre-discard ring (closedRing' = ring), so the
# B6 invariant -- which compares the post-close (emptied) ring against the
# snapshot and checks each snapshot item is still tracked -- fires on the
# discarded items (they are now Released by a non-consumer path AND the ring is
# no longer a suffix of the snapshot).
NEG_DISCARDBUFFER = r"""CloseLinearize ==
    /\ queueState = "Open"
    /\ queueState' = "Closed"
    /\ closedRing' = ring
    /\ lastAction' = "Close"
    /\ lastProdGranted' = NoNode
    /\ lastConsGranted' = NoNode
    /\ expectedProdHead' = NoNode
    /\ expectedConsHead' = NoNode
    /\ lastCommitter' = NoNode
    /\ \* DEFECT: discard buffered items on close (clear the ring + release them)
       /\ ring' = <<>>
       /\ itemLoc' = [it \in Items |->
            IF itemLoc[it] = "Ring" THEN "Released" ELSE itemLoc[it]]
    /\ UNCHANGED <<prodItem, consItem, prodState, consState, prodPhase, consPhase, prodWaiters, consWaiters, prodCompletion, consCompletion, prodResolved, consResolved, wakeDispatched, failedPushItem, admittedItem, consumerDrained>>"""

# NEG FailedPushLosesItem: PushClosed records NoItem in failedPushItem (instead
# of the original it), so the failed-push result loses track of the exact
# original lease -- violates FailedPushRetainsOriginalItem (the IsFailedTerminal
# clause requires failedPushItem[p] = admittedItem[p] = it, but the defect sets
# failedPushItem[p] = NoItem).
NEG_LOSEITEM = r"""PushClosed(p, it) ==
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
    /\ UNCHANGED <<ring, prodWaiters, consWaiters, consState, consPhase, consCompletion, consItem, consResolved, queueState, closedRing, consumerDrained>>"""


# ===========================================================================
# generate
# ===========================================================================
gen(MODEL_A, "E12Queue", "E12QueueNegDuplicateLease",
    "NEG-QUEUE-1: DuplicateLease. FastPushCommit appends the item to the ring "
    "TWICE for one admission (two ring slots own the same ItemId -- a "
    "duplicate lease).",
    "FastPushCommit", NEG_DUP, "UniqueRingItem")

gen(MODEL_A, "E12Queue", "E12QueueNegMoveNotEmptied",
    "NEG-QUEUE-2: MoveNotEmptied. ProducerGrantCommit moves producerOp->ring "
    "but leaves prodItem[p] = it (source operation not emptied); the item is "
    "claimed by both the ring and the producer operation.",
    "ProducerGrantCommit", NEG_MOVE, "UniqueItemOwner")

gen(MODEL_A, "E12Queue", "E12QueueNegBarging",
    "NEG-QUEUE-3: Barging. FastPushCommit drops the ProdEligibleSet = {} guard, "
    "allowing a newcomer producer to fast-commit over an older eligible "
    "producer waiter.",
    "FastPushCommit", NEG_BARGE, "NoBarging")

gen(MODEL_A, "E12Queue", "E12QueueNegPublishBeforeCommit",
    "NEG-QUEUE-4: PublishBeforeCommit. ProducerGrantCommit increments "
    "wakeDispatched (publishes) but leaves the winner's completion Pending "
    "(not finalized before publication).",
    "ProducerGrantCommit", NEG_PUB, "NoPublishedPendingCompletion")

gen(MODEL_B, "E12QueueClosed", "E12QueueNegCommitAfterClose",
    "NEG-QUEUE-5: CommitAfterClose. FastPushCommit drops the queueState = Open "
    "guard, allowing a producer commit into the ring after close linearizes.",
    "FastPushCommit", NEG_COMMITCLOSE, "NoCommitAfterClose")

gen(MODEL_B, "E12QueueClosed", "E12QueueNegCloseDiscardsBuffer",
    "NEG-QUEUE-6: CloseDiscardsBuffer. CloseLinearize clears the ring and "
    "releases all buffered items (discards them) instead of leaving them "
    "drainable.",
    "CloseLinearize", NEG_DISCARDBUFFER, "NoBufferedItemDiscardOnClose")

gen(MODEL_B, "E12QueueClosed", "E12QueueNegFailedPushLosesItem",
    "NEG-QUEUE-7: FailedPushLosesItem. PushClosed does NOT record the original "
    "item in failedPushItem (records NoItem), so the failed-push result loses "
    "track of the exact original lease.",
    "PushClosed", NEG_LOSEITEM, "FailedPushRetainsOriginalItem")
