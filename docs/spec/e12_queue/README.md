# E12-E Queue — TLA+ Formal Model (safety)

> `sluice::async::Queue` (bounded MPMC FIFO), task
> **E12-E-QUEUE-FORMAL-MODEL-1** (B4).
>
> Status: **B4: COMPLETE — INDEPENDENT FORMAL REVIEW REQUIRED.** This model
> obeys the **Corrective-2** design authority
> (`E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2: PASS`, B2). The
> semantic authority document is [`docs/e12-queue.md`](../../e12-queue.md).

This directory contains the **safety-only** TLA+ / TLC formal model for the
E12-E Queue. It proves the one-shot-lease / bounded-MPMC-FIFO safety properties
of the Corrective-2 design (no barging, no lost or duplicated item, FIFO buffer
order, selected-waiter grant, winner-before-publication, no direct handoff) and
the Open/Closed monotonicity properties (close is absorbing, drain-on-close,
producer rejection after close, closed+empty consumer terminal). It provides
seven negative models that each fail a single named invariant for the intended
reason, plus a wrong-property gate that proves the defects are specific.

## The load-bearing E12-E question

> Can the Corrective-2 Queue's bounded MPMC FIFO and Open/Closed monotonicity
> be modelled so that every safety property is a TLC-checkable state invariant
> (no primed variables in invariants), with the one-shot lease abstraction as
> the sole ownership authority and no spurious liveness?

**Answer: YES** — by (a) modelling every live `ItemId` as having exactly one
control location (`detached | producer_operation | ring | consumer_operation |
released`) and making every move empty its source and require an empty
destination (the one-shot lease abstraction), (b) carrying minimal ghost/history
evidence (`lastAction`, `lastCommitter`, `expectedProdHead`, `expectedConsHead`,
`lastProdGranted`, `lastConsGranted`) so transition properties (FIFO grant,
barging, commit-before-publish) become state predicates, (c) snapshotting the
ring at close (`closedRing`) and tracking consumer-drained items
(`consumerDrained`) so drain-on-close is a prime-free state predicate, (d)
carrying an explicit `ring : Seq(ItemId)` and explicit producer/consumer waiter
sequences so FIFO ordering has a real authority, and (e) making the model
safety-only (no delivery liveness).

## Files

| File | Purpose |
|------|---------|
| `E12Queue.tla` | **Model A** — correct bounded MPMC FIFO safety model (12 named invariants + 4 structural) |
| `E12Queue.cfg` | Model A safety config (SPECIFICATION Spec, INVARIANT Inv) |
| `E12QueueClosed.tla` | **Model B** — correct Open/Closed monotonicity safety model (7 named invariants); extends Model A's one-shot-lease substrate with close/drain semantics |
| `E12QueueClosed.cfg` | Model B safety config |
| `E12QueueNegDuplicateLease.tla/.cfg` | NEG-QUEUE-1: duplicate lease (double-append) → `UniqueRingItem` |
| `E12QueueNegMoveNotEmptied.tla/.cfg` | NEG-QUEUE-2: move leaves source non-empty → `UniqueItemOwner` |
| `E12QueueNegBarging.tla/.cfg` | NEG-QUEUE-3: dropped eligibility guard → `NoBarging` |
| `E12QueueNegPublishBeforeCommit.tla/.cfg` | NEG-QUEUE-4: publish before commit → `NoPublishedPendingCompletion` |
| `E12QueueNegCommitAfterClose.tla/.cfg` | NEG-QUEUE-5: commit after close → `NoCommitAfterClose` |
| `E12QueueNegCloseDiscardsBuffer.tla/.cfg` | NEG-QUEUE-6: close discards buffer → `NoBufferedItemDiscardOnClose` |
| `E12QueueNegFailedPushLosesItem.tla/.cfg` | NEG-QUEUE-7: failed push loses item → `FailedPushRetainsOriginalItem` |
| `_gen_neg.py` | Build aid that generates all 7 negatives from their parent models by a single-rule substitution. Not part of the formal gate. Re-run after editing a parent model, then re-run `scripts/verify-e12-queue-formal.sh`. |

## Model domain

`PNodes = {P0, P1, P2}` (producer epochs), `CNodes = {C0, C1, C2}` (consumer
epochs), `Items = {I0, I1, I2}`, `Capacity = 1`. Three producer and three
consumer epochs with capacity 1 suffice to express: (a) the barging topology
(two producers, one eligible waiter + one newcomer), (b) the FIFO grant
topology (multiple suspended waiters), (c) the close/drain interleaving
(producer commit racing close), while keeping the state space checkable.
Capacity 1 (not 2) is load-bearing: with capacity 2 and two producers both
fast-push commit and never park, so the selected-waiter grant path is
unreachable. Capacity 1 forces at least one producer to park, making grants and
their FIFO/barging/no-handoff invariants reachable and checkable.

## State dimensions

| Variable | Type | Meaning |
|----------|------|---------|
| `ring` | `Seq(ItemId)` | the bounded FIFO buffer (the single handoff authority: producer→ring→consumer only) |
| `itemLoc` | `[ItemId -> Loc]` | per-item control location (`Detached/ProducerOp/Ring/ConsumerOp/Released`) — the one-shot lease authority |
| `prodWaiters` | `Seq(PNode)` | producer FIFO wait queue (explicit ordering authority for grants) |
| `consWaiters` | `Seq(CNode)` | consumer FIFO wait queue |
| `prodState` / `consState` | `[PNode -> State]` | per-epoch lifecycle (Detached/Registered/Woken) |
| `prodPhase` / `consPhase` | `[PNode -> Phase]` | NoAdmission/AdmissionOpen/Suspended |
| `prodItem` / `consItem` | `[PNode -> ItemId∪{NoItem}]` | the item currently held by an operation (empty iff the operation owns nothing) |
| `prodCompletion` / `consCompletion` | `[PNode -> Outcome]` | None/Pending/Committed/None(would-block)/ClosedOutcome |
| `prodResolved` / `consResolved` | `[PNode -> Nat]` | terminal resolution count (single-resolution) |
| `wakeDispatched` | `Int` | total runnable publications (single-publication) |
| `queueState` | `{"Open","Closed"}` | Model B only: monotonic close state |
| `failedPushItem` | `[PNode -> ItemId∪{NoItem}]` | Model B: the item a failed-push result owns |
| `admittedItem` | `[PNode -> ItemId∪{NoItem}]` | Model B GHOST: the item a producer epoch admitted (for `FailedPushRetainsOriginalItem`) |
| `closedRing` | `Seq(ItemId)∪{NoSnap}` | Model B GHOST: ring snapshot latched at close (for drain-on-close) |
| `consumerDrained` | `Set(ItemId)` | Model B GHOST: items released via a consumer drain (for `NoBufferedItemDiscardOnClose`) |
| `lastAction` | `ActionKind` | HISTORY: the action that produced the current state |
| `lastCommitter` | `PNode∪{NoNode}` | HISTORY: the producer epoch that committed on the last ring-mutation |
| `expectedProdHead` / `expectedConsHead` | `Node∪{NoNode}` | HISTORY: FIFO head latched BEFORE queue mutation (for no-barging / FIFO grant) |
| `lastProdGranted` / `lastConsGranted` | `Node∪{NoNode}` | HISTORY: the epoch actually granted (winner-before-publication) |

The `lastAction` / `last*` / `expected*` / `admittedItem` / `closedRing` /
`consumerDrained` variables are **ghost/history evidence**: they exist solely
so that transition properties (FIFO grant, no-barging, commit-before-publish,
drain-on-close, failed-push item retention) are expressible as pure state
invariants. They are NOT production fields.

## Correct properties — Model A (bounded MPMC FIFO safety)

| Property | Formal name | Meaning |
|----------|-------------|---------|
| A1 | `CapacityBound` | `Len(ring) ≤ Capacity` always |
| A2 | `UniqueItemOwner` | every live item has `ItemClaimCount = 1` (one-shot lease; no double-claim) |
| A3 | `UniqueRingItem` | no item appears twice in the ring (`∀ i≠j : ring[i]≠ring[j]`) |
| A4 | `NoLostItem` | every `Detached` item is fresh (no operation/ring/consumer holds it) |
| A5 | `NoDuplicatedItem` | `itemLoc` is a function over live items (no two locations claim one item) |
| A6 | `FIFOBufferOrder` | the ring reflects FIFO admission order among buffered items |
| A7 | `ProducerWaiterFIFO` | `prodWaiters` is duplicate-free and only admissible epochs are queued |
| A8 | `ConsumerWaiterFIFO` | `consWaiters` is duplicate-free and only admissible epochs are queued |
| A9 | `NoBarging` | a fast push/pop required no eligible waiter of its role (history-backed) |
| A10 | `CommittedBeforePublished` | the last published committer was in a terminal Committed state (history-backed) |
| A11 | `NoPublishedPendingCompletion` | publication implies the winner's completion was finalized first (history-backed) |
| A12 | `LocationConsistency` | `itemLoc` is consistent with the ring and the per-operation item fields |

(`ItemClaimCount(it)` = (1 if in ring else 0) + |producers holding| + |consumers
holding|; the one-shot lease requires this to be exactly 1 for every live item.)

## Correct properties — Model B (Open/Closed monotonicity)

| Property | Formal name | Meaning |
|----------|-------------|---------|
| B1 | `ClosedAbsorbing` | `queueState` is monotonic Open→Closed (close is absorbing) |
| B2 | `NoCommitAfterClose` | no item enters the ring after close linearizes |
| B3 | `CommittedBeforeCloseRemainsDrainable` | every item buffered at close is still tracked (ring / consumer op / consumer-released); the post-close ring is a suffix of the close-time snapshot (`closedRing`) |
| B4 | `FailedPushRetainsOriginalItem` | a failed-push terminal epoch owns the EXACT admitted item (`failedPushItem = admittedItem`; ghost-backed) |
| B5 | `ClosedEmptyConsumerTerminal` | a consumer that became Closed at closed+empty owns no item |
| B6 | `NoBufferedItemDiscardOnClose` | close never discards buffered items: post-close ring is a suffix of `closedRing`, and every former buffered item is still tracked via a LEGAL path (ring / consumer op / consumer-drained Released), never Released by a non-consumer (`consumerDrained` ghost) |
| B7 | `CloseProducerRaceLinearizable` | close and producer commit are serialized by the G+S critical section; exactly one linearizes first |

## Negative models (in scope — 7)

| NEG | Parent | Single broken rule | Counterexample | Expected property | TLC result |
|-----|--------|-------------------|----------------|-------------------|------------|
| NEG-QUEUE-1 | A | `FastPushCommit` appends the item TWICE (`ring'=Append(Append(ring,it),it)`) | `ring = <<I0,I0>>` | `UniqueRingItem` | CEX (violated) |
| NEG-QUEUE-2 | A | `ProducerGrantCommit` leaves `prodItem[p]=it` (source not emptied) | item claimed by both ring and producer op | `UniqueItemOwner` | CEX (violated) |
| NEG-QUEUE-3 | A | `FastPushCommit` drops `ProdEligibleSet={}` guard (barging) | fast-push over an eligible producer waiter | `NoBarging` | CEX (violated) |
| NEG-QUEUE-4 | A | `ProducerGrantCommit` leaves winner's completion `Pending` then publishes | `wakeDispatched++` with `prodCompletion=Pending` | `NoPublishedPendingCompletion` | CEX (violated) |
| NEG-QUEUE-5 | B | `FastPushCommit` drops `queueState="Open"` (commit after close) | `FastPushCommit` succeeds at `queueState="Closed"` | `NoCommitAfterClose` | CEX (violated) |
| NEG-QUEUE-6 | B | `CloseLinearize` clears the ring + releases ring items (discard) | `ring=<<>>`, former ring items `Released` but NOT in `consumerDrained` | `NoBufferedItemDiscardOnClose` | CEX (violated) |
| NEG-QUEUE-7 | B | `PushClosed` records `failedPushItem=NoItem` (loses the item) | `admittedItem=it` but `failedPushItem=NoItem` | `FailedPushRetainsOriginalItem` | CEX (violated) |

Each negative is a self-contained module identical to its parent EXCEPT for ONE
single-rule defect in ONE named action. The wrong-property gate (NEG-QUEUE-1
checked against `NoBarging`) confirms the defects are property-specific.

## Out-of-scope negatives (4) — justification and coverage

The Corrective-2 audit identified four additional defect classes that B4 does
NOT model as negatives. Each is out of scope for a precise, non-vacuous reason,
and each has named coverage elsewhere (production test, Model A/B invariant, or
deferred Model C). No B4 false-PASS arises from their omission because each is
either (i) already pinned by an existing invariant that a modelled negative
exercises, or (ii) a property of machinery the model deliberately does not own.

| Topic | Why out of B4 scope | Coverage |
|-------|---------------------|----------|
| **ActiveVictimUnstealable** (a suspended waiter whose grant is stolen by a latecomer cannot be left "active-victim") | The selected-waiter grant (only the FIFO head of the eligible role is granted) is fully pinned by `NoBarging` (A9) + `ProducerWaiterFIFO`/`ConsumerWaiterFIFO` (A7/A8) + the `expectedProdHead`/`lastProdGranted` history. There is no separate "steal" action in the model: a grant always targets the latched FIFO head. A "steal" negative would just be another `NoBarging`/FIFO mutation, duplicating NEG-QUEUE-3. | `NoBarging` (A9), `ProducerWaiterFIFO` (A7), `ConsumerWaiterFIFO` (A8); production test coverage in the Queue scheduler-integration suite (`docs/e12-queue-scheduler-integration.md`). |
| **OwnerSlotEarlyErase** (a producer/consumer operation's item slot is erased before the operation reaches a terminal outcome) | The one-shot lease makes `prodItem`/`consItem` erase and terminal-outcome finalization the SAME atomic step (every move empties its source AND finalizes). There is no window in which an operation is non-terminal yet holds no item. An "early-erase" negative would mutate a rule to clear the slot without finalizing, which is exactly the `UniqueItemOwner` (A2) / `LocationConsistency` (A12) defect class already covered by NEG-QUEUE-2. | `UniqueItemOwner` (A2), `LocationConsistency` (A12); NEG-QUEUE-2 exercises the same defect class. |
| **PreparedTimerVisibleToPump** (a timer prepared by an E11 expiry must be visible to the scheduler pump before it fires) | The E12-E model does not own E11 timer/expiry state machinery — expiry is the E11 authority (mirroring the E12-B Semaphore decision). Model B reaches `ClosedOutcome` via `PushClosed`/`PopClosedEmpty` directly to close the close-race proof; it does not model the timer-pump interleaving. Modelling it here would require importing E11's timer state, which is out of B4's refinement boundary. | Deferred to a cross-cutting E11×E12 Model C; the close-race half that B4 owns is pinned by `CloseProducerRaceLinearizable` (B7) and exercised by NEG-QUEUE-5. |
| **TeardownReusable** (a torn-down Queue must not be reusable: no operation on a Closed+drained Queue can resurrect it) | `ClosedAbsorbing` (B1) pins that `queueState` is monotonic Open→Closed and close is absorbing (every post-close state is Closed; `IdempotentClose` only re-reconciles). There is no "reopen" action in the model. A "reuse" negative would need to add a reopen rule that does not exist in the Corrective-2 design; that is a liveness/progress question, not a safety defect, and B4 is safety-only. | `ClosedAbsorbing` (B1); reuse-freedom is a corollary of monotonicity. Production teardown tests in the Queue suite. |

## Design notes (refinement / scope)

- **One-shot lease as the sole ownership authority.** Every live `ItemId` has
  exactly one control location. Every move (Detached→ProducerOp, ProducerOp→Ring,
  Ring→ConsumerOp, ConsumerOp→Released) empties its source and requires an empty
  destination. This is the Corrective-2 abstraction; `UniqueItemOwner` (A2) and
  `LocationConsistency` (A12) are its state-predicate expression.
- **No direct handoff.** The ring is the ONLY producer→consumer channel: a
  producer commits to the ring, a consumer drains from the ring. There is no
  producer→consumer transfer that bypasses the ring. This is encoded by the
  `itemLoc` move rules (ProducerOp→Ring→ConsumerOp only).
- **Selected-waiter grant (no barging).** When the ring has space and a
  producer is parked, only the FIFO head of the eligible producer set may be
  granted. `NoBarging` (A9) pins this via the `expectedProdHead`/`lastProdGranted`
  history: a fast push that committed required `ProdEligibleSet = {}`.
- **Winner-before-publication.** A grant finalizes the winner's completion
  BEFORE incrementing `wakeDispatched`. `CommittedBeforePublished` (A10) and
  `NoPublishedPendingCompletion` (A11) pin this via history.
- **Capacity 1 is load-bearing.** With capacity 2 and two producers, both
  fast-push commit and never park; the grant/FIFO/barging path is unreachable
  and its invariants become vacuous. Capacity 1 forces parking.
- **Transition properties as state invariants (ghost evidence).** `NoBarging`,
  `CommittedBeforePublished`, `NoPublishedPendingCompletion`,
  `CommittedBeforeCloseRemainsDrainable`, and `NoBufferedItemDiscardOnClose` are
  properties of an action's before/after state. They are expressed as state
  predicates over history/ghost variables (`lastAction`, `expected*`,
  `last*Granted`, `admittedItem`, `closedRing`, `consumerDrained`), written by
  every action. No invariant uses a primed variable (actions do; invariants do
  not).
- **Safety-only.** There is no liveness `.cfg`. Delivery liveness (every
  admitted item is eventually delivered or returned; every waiter is eventually
  granted or released) is out of scope for B4 and deferred to a later model-C
  task. The model does not assume scheduler fairness.
- **Ghosts are refinement-consistent.** `admittedItem`, `closedRing`, and
  `consumerDrained` are pure refinement ghosts: they are written by exactly the
  actions the design authorizes (the four admission actions; `CloseLinearize`;
  `ReleaseItem`) and read only by the invariants that need them. Removing them
  (and the invariants that depend on them) yields a sound, weaker model; they
  never exclude reachable states (no `ASSUME` narrows the state space).

## Results (actual TLC execution)

TLC runtime version (exact runtime line, reported by TLC itself):
`TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)` (java OpenJDK 25.0.3).

| Model | Result | States generated | Distinct states | Depth |
|-------|--------|-----------------|-----------------|-------|
| E12Queue [Model A, 12 invariants] | PASS | 1614934 | 352131 | 15 |
| E12QueueClosed [Model B, 7 invariants] | PASS | 5820858 | 1168618 | 15 |
| NEG-QUEUE-1 DuplicateLease | CEX (`UniqueRingItem`) | 6 | 4 | 3 |
| NEG-QUEUE-2 MoveNotEmptied | CEX (`UniqueItemOwner`) | 14035 | 9089 | 7 |
| NEG-QUEUE-3 Barging | CEX (`NoBarging`) | 14064 | 9109 | 7 |
| NEG-QUEUE-4 PublishBeforeCommit | CEX (`NoPublishedPendingCompletion`) | 14068 | 9111 | 6 |
| NEG-QUEUE-5 CommitAfterClose | CEX (`NoCommitAfterClose`) | 64 | 47 | 3 |
| NEG-QUEUE-6 CloseDiscardsBuffer | CEX (`NoBufferedItemDiscardOnClose`) | 53 | 36 | 3 |
| NEG-QUEUE-7 FailedPushLosesItem | CEX (`FailedPushRetainsOriginalItem`) | 64 | 47 | 4 |
| WRONG-PROPERTY gate (NEG-1 vs `NoBarging`) | OK (passes; defect specific) | — | — | — |

State counts vary slightly run-to-run (TLC's state enumeration is
worker/scheduling-dependent); the verdicts are deterministic. The above are
representative single-run figures from the verify gate.

Reproduce: `scripts/verify-e12-queue-formal.sh`
(default JAR: `$repo/tla2tools.jar`; override with `TLA2TOOLS_JAR=...`).
The full AsyncCondition + Queue suite:
`bash scripts/run-e12-tlc-all.sh` (default `all`; or `async` / `queue`).

## What this model does NOT cover

- **Liveness / progress** (every admitted item eventually delivered or
  returned; every waiter eventually granted or released). Safety-only.
- **E11 timer/expiry state machinery.** Model B reaches `ClosedOutcome`
  directly via `PushClosed`/`PopClosedEmpty` to close the close-race proof; the
  timer-pump interleaving is the E11 authority (deferred cross-cutting Model C).
- **MW admission/steal (E7–E9), Fiber lifecycle/asm (E2/E4).**
- **Backends / io_uring / timerfd / networking.**
- **`push(N)` / `pop(N)` batch API** (multi-item; deferred).
- **Memory/resource reclamation beyond the lease abstraction** (Released items
  are modelled as terminal; production deallocation is out of scope).
- **E12-B Semaphore / E12-C Mutex / E12-D Condition / E12-F RwLock / Select**
  (separate phases).
