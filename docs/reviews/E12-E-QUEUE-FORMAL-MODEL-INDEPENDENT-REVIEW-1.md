# E12-E Queue Formal Model — Independent Review (1)

```text
E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-1
```

> **Scope:** formal-model review of the B4 Queue TLA+ artefacts authored in
> commits `9572985` + `43d47a0` (branch `e12-e-queue-production-impl`). The
> reviewer did NOT author the model and does NOT trust the author's
> self-assessment, the README's claimed TLC numbers, the commit messages, or
> any summary. Every blocking claim below was reproduced independently by
> running TLC (v2.19 of 08 August 2024, rev `5a47802`) and reading the actual
> `.tla` source. Scratch probes were kept under `/tmp/e12vac/`; the only
> durable file added is this document. No file under `docs/spec/e12_queue/`,
> any Queue design doc, or any production path was modified. Nothing was
> committed.
>
> Authority obeyed: `docs/e12-queue-state-machine.md` (Corrective-2 binding
> semantics), `docs/e12-queue.md` (binding decisions above the historical
> marker), `docs/e12-queue-scheduler-integration.md`, and the B2 review
> (`docs/reviews/E12-E-QUEUE-CORRECTIVE-2-INDEPENDENT-ADVERSARIAL-REVIEW-1.md`).

---

## A. Verdict

```text
E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-1: REQUEST-CHANGES
```

The model is largely sound and its design intent is faithful to the B2
Corrective-2 authority, but **one BLOCKING defect** in Model B makes a
load-bearing ghost variable dead and renders the antecedent of specific
invariant clauses unreachable — the precise "B4 false-PASS via vacuity"
pattern this review was tasked to hunt. The defect is a one-line fix (verified
by the reviewer: after the fix the model still PASSES `Inv` and the negative
gate still fires). Two MAJOR findings and three MINOR findings are also
recorded.

```text
BLOCKING findings: 1   (F.1.1 — Model B ReleaseItem dead action; consumerDrained
                              ghost never written; B3/B6 Released-antecedent
                              clauses vacuous)
MAJOR findings:    2   (F.2.1 — README overstates teardown coverage as a
                              corollary of ClosedAbsorbing; F.2.2 — README cites
                              a non-existent "production test suite" as coverage
                              for two scope-outs)
MINOR findings:    3   (F.3.1/F.3.2/F.3.3 — README state-count/depth drift,
                              wording)
```

Independent reproduction summary (all run by the reviewer, not the author):

| Model | Result | States generated | Distinct states | Depth |
|-------|--------|-----------------:|----------------:|------:|
| `E12Queue` (Model A) | PASS | 1,614,934 | 352,131 | 13 |
| `E12QueueClosed` (Model B) | PASS | 5,820,858 | 1,168,618 | 14 |
| NEG-QUEUE-1 DuplicateLease | CEX on `UniqueRingItem` | 3 | 3 | 2 |
| NEG-QUEUE-2 MoveNotEmptied | CEX on `UniqueItemOwner` | 13,652 | 8,804 | 6 |
| NEG-QUEUE-3 Barging | CEX on `NoBarging` | 13,650 | 8,803 | 6 |
| NEG-QUEUE-4 PublishBeforeCommit | CEX on `NoPublishedPendingCompletion` | 13,652 | 8,804 | 6 |
| NEG-QUEUE-5 CommitAfterClose | CEX on `NoCommitAfterClose` | 38 | 21 | 3 |
| NEG-QUEUE-6 CloseDiscardsBuffer | CEX on `NoBufferedItemDiscardOnClose` | 51 | 34 | 3 |
| NEG-QUEUE-7 FailedPushLosesItem | CEX on `FailedPushRetainsOriginalItem` | 38 | 21 | 3 |
| WRONG-PROPERTY gate (NEG-1 vs `NoBarging`) | OK (defect specific) | — | — | — |

State counts vary slightly run-to-run (TLC worker scheduling); the verdicts
are deterministic and were reproduced. The full gate
(`scripts/verify-e12-queue-formal.sh`) PASSES end-to-end on the unmodified
tree (it must continue to PASS after the fix).

---

## B. Independence and substrate verification

### B.1 TLC runtime and tooling (independently confirmed)

- `java -cp tla2tools.jar tlc2.TLC` reports `TLC2 Version 2.19 of 08 August
  2024 (rev: 5a47802)` — matches the README's claimed runtime exactly.
- `tla2tools.jar` is the pre-existing untracked jar at the repo root (not
  touched).
- All runs used `java -XX:+UseParallelGC -cp $repo/tla2tools.jar tlc2.TLC
  -nowarning -config <cfg> <module>` from `docs/spec/e12_queue/`.
- Scratch probes live under `/tmp/e12vac/` (copies of the models with added
  reach invariants; never written into the repo).

### B.2 Model files under review (verified present, unmodified by this review)

- `docs/spec/e12_queue/E12Queue.tla` (Model A, 692 lines) + `.cfg`
- `docs/spec/e12_queue/E12QueueClosed.tla` (Model B, 772 lines) + `.cfg`
- 7 negatives: `E12QueueNeg{DuplicateLease,MoveNotEmptied,Barging,
  PublishBeforeCommit,CommitAfterClose,CloseDiscardsBuffer,FailedPushLosesItem}`
- `docs/spec/e12_queue/README.md`, `_gen_neg.py`
- `scripts/verify-e12-queue-formal.sh`, `scripts/run-e12-tlc-all.sh`

---

## C. BLOCKING findings (top of report)

### C.1 / F.1.1 — BLOCKING: Model B `ReleaseItem` is a dead action; `consumerDrained` ghost is never written; `Released` location is never reached; B3/B6 `Released`-antecedent clauses are vacuous

**Location:** `docs/spec/e12_queue/E12QueueClosed.tla`, `ReleaseItem(c)`,
lines **477** and **479**.

**The defect.** `ReleaseItem` is the ONLY action that moves an item to the
`Released` location and the ONLY action that writes the `consumerDrained`
ghost. Its body simultaneously:

- line 477:  `consumerDrained' = consumerDrained \cup {it}`  (a primed assignment), AND
- line 479:  `UNCHANGED <<..., closedRing, consumerDrained>>`  (which conjoins
  `consumerDrained' = consumerDrained`).

In TLA+, an `UNCHANGED <<..., x, ...>>` conjoins `x' = x` to the action. The
two conjuncts `consumerDrained' = consumerDrained \cup {it}` AND
`consumerDrained' = consumerDrained` are jointly satisfiable ONLY when
`it \in consumerDrained` already. Since `consumerDrained` starts as `{}` and
`it` is the item being released (not already in the set), the conjunction is
**FALSE** in every state where `ReleaseItem` would otherwise fire. The action
is therefore unsatisfiable — it is a dead transition that never executes.

**Independent evidence (all reproduced by the reviewer, on the unmodified
shipped model).**

1. **`consumerDrained` is always empty.** Checking the invariant
   `consumerDrained = {}` against the full Model B state space holds with no
   counterexample: 5,820,858 states generated, 1,168,618 distinct, depth 14
   (identical to the `Inv` PASS run). I.e. in EVERY reachable state
   `consumerDrained = {}`.

2. **No item is ever in the `Released` location.** The invariant
   `\A it \in Items : itemLoc[it] # "Released"` holds across the full state
   space. `ReleaseItem` is the only action that writes `itemLoc' = ... "Released"`
   in Model B (CloseLinearize in the correct model does not; only the NEG-6
   mutation does). Since `ReleaseItem` never fires, `Released` is never
   reached.

3. **The `ReleaseItem` precondition IS reachable.** The invariant
   `\A c \in CNodes : ~(consState[c]="Woken" /\ consItem[c]#NoItem /\
   itemLoc[consItem[c]]="ConsumerOp")` is VIOLATED — a counterexample reaches
   the state (Init -> FastPushCommit(I0) -> FastPopCommit(C0)) where
   `consState[C0]="Woken"`, `consItem[C0]=I0`, `itemLoc[I0]="ConsumerOp"`.
   This is exactly `ReleaseItem(C0)`'s guard. From this state `ReleaseItem`
   should fire; it does not, because of the conjunct conflict above.

4. **Isolation check.** An automated scan of every action in both models for
   "variable appears both primed and in UNCHANGED" returns exactly ONE hit:
   `ReleaseItem` / `consumerDrained` in `E12QueueClosed.tla`. Model A is
   entirely clean (it has no `consumerDrained` variable). No other action has
   this defect.

5. **Fix verified by the reviewer.** Removing `consumerDrained` from the
   `UNCHANGED` list on line 479 (leaving the line 477 primed assignment as
   the sole writer) and re-running:
   - `consumerDrained = {}` is now VIOLATED (counterexample found) — the
     consumer-drain path is live.
     The `Released` location is now reachable, and `Released /\ it \in
     consumerDrained` is reachable.
   - The full `Inv` still PASSES on the fixed model: **10,007,847 states /
     1,962,157 distinct / depth 14** — i.e. the fix is sound AND it exposes
     ~40% more distinct states that the dead action had hidden (1.17M vs
     1.96M). The shipped model was exploring a strictly smaller state space
     than its own variable declarations imply.
   - NEG-QUEUE-6 (`CloseDiscardsBuffer`) still violates
     `NoBufferedItemDiscardOnClose` under the fix (51 states, depth 3) — the
     fix does not weaken the negative gate; B6's remaining live clauses
     (the `IsSuffixOf` clause and the `itemLoc="Released" => it \in
     consumerDrained` implication) still catch the close-discard defect.

**Why this is BLOCKING (vacuity).** The task's Check 3 states verbatim:
"An invariant that holds only because the reachable state space never touches
its antecedent is VACUOUS. ... If an invariant's antecedent is never
reachable, that is a BLOCKING finding." Two Model B invariant clauses have
antecedents that are never reachable in the shipped model:

- **B3 `CommittedBeforeCloseRemainsDrainable`** (line 643): the disjunct
  `(itemLoc[it] = "Released" /\ it \in consumerDrained)` has antecedent
  `itemLoc[it]="Released"`, which is never true. That disjunct is vacuous.
- **B6 `NoBufferedItemDiscardOnClose`** (line 723, disjunct; and line 730,
  implication):
  - line 723 disjunct `(itemLoc[it] = "Released" /\ it \in consumerDrained)`
    — vacuous (same antecedent).
  - line 730 implication `(itemLoc[it] = "Released" => it \in consumerDrained)`
    — vacuously TRUE because `itemLoc[it]="Released"` is never satisfied. This
    clause asserts nothing in the shipped model.

The ghost variable `consumerDrained` exists ONLY to power these clauses; with
`ReleaseItem` dead, it is written by no action and read only by vacuous
clauses. It provides **zero verification power** as shipped. The README's
claim (line 88, 198-202) that `consumerDrained` "distinguishes a consumer-
drained Released item from a close-discarded one" and is "written by exactly
the actions the design authorizes (...; `ReleaseItem`)" is **false** for the
shipped model.

**Severity note (why BLOCKING, not MAJOR).** B3 and B6 as whole invariants
are NOT entirely vacuous — their other clauses (the `IsSuffixOf(ring,
closedRing)` clause; the `in ring | ConsumerOp | \E c: consItem[c]=it`
disjuncts; the `itemLoc[it] \notin {Detached,ProducerOp}` clause) ARE live
and exercised, and NEG-6's CEX still fires. So this is not a "the entire B4
gate is void" situation. But the task explicitly designates an unreachable
invariant antecedent as BLOCKING, the dead action silently halves the explored
state space, and the defect is in a ghost variable the README presents as
load-bearing. A one-line fix resolves it cleanly and the reviewer has verified
the fix preserves all PASS verdicts and the negative CEX. This must be fixed
before B4 can be declared PASS.

**Required corrective.** In `docs/spec/e12_queue/E12QueueClosed.tla`,
`ReleaseItem(c)`, remove `consumerDrained` from the `UNCHANGED <<...>>` list
on line 479 (it is already correctly primed on line 477). Concretely, change
line 479 from:

```
/\ UNCHANGED <<ring, prodWaiters, consWaiters, prodState, consState, prodPhase, consPhase, prodCompletion, consCompletion, prodResolved, consResolved, prodItem, wakeDispatched, queueState, failedPushItem, admittedItem, closedRing, consumerDrained>>
```

to:

```
/\ UNCHANGED <<ring, prodWaiters, consWaiters, prodState, consState, prodPhase, consPhase, prodCompletion, consCompletion, prodResolved, consResolved, prodItem, wakeDispatched, queueState, failedPushItem, admittedItem, closedRing>>
```

After the fix, regenerate the Model B negatives with `_gen_neg.py` (NEG-5/6/7
inherit `ReleaseItem` from Model B; the fix applies to them automatically) and
re-run `scripts/verify-e12-queue-formal.sh`. The reviewer confirmed the full
`Inv` PASS and all 7 negative CEX survive the fix.

---

## D. Per-invariant vacuity analysis (19 invariants)

Method: for each invariant, the reviewer wrote a NEGATED reachability probe
`NeverX == ~(antecedent)` and ran TLC. A CEX means the antecedent IS reached
(non-vacuous); a PASS means the antecedent is NEVER reached (vacuity). All
probes ran against the FULL reachable state space (not a sampled prefix),
using copies of the models under `/tmp/e12vac/`.

### D.1 Model A — 12 named + 4 structural invariants

| # | Invariant | Pure-state? | Antecedent reached? | Verdict |
|---|-----------|:-----------:|:-------------------:|:-------:|
| A1 | `CapacityBound` | yes | `Len(ring)=Capacity` reached (`NeverRingFull` CEX) | NON-VACUOUS |
| A2 | `UniqueItemOwner` | yes | all 5 `itemLoc` branches reached (Ring/ProducerOp/ConsumerOp/Detached/Released all observed) | NON-VACUOUS |
| A3 | `UniqueRingItem` | yes | ring with >=2 elements reached; NEG-1 confirms it can fail | NON-VACUOUS |
| A4 | `NoLostItem` | yes | `itemLoc \in {Ring,ProducerOp,ConsumerOp}` reached | NON-VACUOUS |
| A5 | `NoDuplicatedItem` | yes | `ItemClaimCount` exercised; NEG-2 drives it to 2 | NON-VACUOUS |
| A6 | `FIFOBufferOrder` | yes | non-empty ring reached (`NeverRingNonempty` CEX) | NON-VACUOUS |
| A7 | `ProducerWaiterFIFO` | yes | `lastAction="ProdGrant"` reached (`NeverReachActionProdGrant` CEX) | NON-VACUOUS |
| A8 | `ConsumerWaiterFIFO` | yes | `lastAction="ConsGrant"` reached | NON-VACUOUS |
| A9 | `NoBarging` | yes | `lastAction \in {FastPush,FastPop}` reached AND `ProdEligibleSet`/`ConsEligibleSet` non-empty reached (`NeverProdEligible`/`NeverConsEligible` CEX) | NON-VACUOUS |
| A10 | `CommittedBeforePublished` | yes | all 4 antecedents (`FastPush`/`FastPop`/`ProdGrant`/`ConsGrant`) reached | NON-VACUOUS |
| A11 | `NoPublishedPendingCompletion` | yes | `lastAction="ProdGrant"` reached; NEG-4 confirms the `prodCompletion[lastCommitter]="Committed"` clause can fail | NON-VACUOUS |
| A12 | `LocationConsistency` | yes | every `itemLoc` value reached | NON-VACUOUS |
| S1 | `InvSingleResolution` | yes | `prodResolved`/`consResolved` reach 1 (grants fire) | NON-VACUOUS |
| S2 | `InvSinglePublication` | yes | `wakeDispatched` and Woken counts both move; equality is exercised | NON-VACUOUS |
| S3 | `InvProdWaitersWellFormed` | yes | `prodWaiters` non-empty reached (`NeverTwoWaiters` CEX shows 2+ queued) | NON-VACUOUS |
| S4 | `InvConsWaitersWellFormed` | yes | `consWaiters` non-empty reached (`NeverTwoConsWaiters` CEX) | NON-VACUOUS |

**Model A: 16/16 invariants non-vacuous and exercised.** No primed variables
in any invariant. No tautology. No unreachable antecedent. The bounds
(Capacity=1, 3 producers, 3 consumers, 3 items) genuinely exercise the
non-trivial topology: 2+ suspended waiters of each role are reachable, the
ring is reached at capacity, and all four grant/fast-commit actions fire.

### D.2 Model B — 7 named B-invariants (+ 10 carried-over Model A invariants)

| # | Invariant | Pure-state? | Antecedent reached? | Verdict |
|---|-----------|:-----------:|:-------------------:|:-------:|
| B1 | `ClosedAbsorbing` | yes | `queueState="Closed"` reached (`NeverClosed` CEX); `lastAction \in {Close,IdempotentClose}` reached | NON-VACUOUS |
| B2 | `NoCommitAfterClose` | yes | `queueState="Closed"` reached; NEG-5 confirms `lastAction \in {FastPush,ProdGrant}` at Closed can be forced | NON-VACUOUS |
| B3 | `CommittedBeforeCloseRemainsDrainable` | yes | `closedRing # NoSnap` reached (`NeverClosedRingSet` CEX); **BUT the `(itemLoc="Released" /\ it \in consumerDrained)` disjunct at line 643 is VACUOUS** — see F.1.1 | **PARTIALLY VACUOUS (BLOCKING)** |
| B4 | `FailedPushRetainsOriginalItem` | yes | failed-terminal producers reached (`NeverFailedTerminalProducer` and `NeverFailedClosedProducer` CEX; `failedPushItem` non-NoItem reached); NEG-7 confirms the `failedPushItem=admittedItem` clause can fail | NON-VACUOUS |
| B5 | `ClosedEmptyConsumerTerminal` | yes | `consState="Closed"` reached (`NeverConsClosed` CEX) | NON-VACUOUS |
| B6 | `NoBufferedItemDiscardOnClose` | yes | `closedRing # NoSnap` reached; NEG-6 confirms suffix + Released-clauses can fail; **BUT the `(itemLoc="Released" /\ it \in consumerDrained)` disjunct (line 723) and the `(itemLoc="Released" => it \in consumerDrained)` implication (line 730) are VACUOUS** — see F.1.1 | **PARTIALLY VACUOUS (BLOCKING)** |
| B7 | `CloseProducerRaceLinearizable` | yes | `queueState="Closed"` with Committed producers reached (`NeverCommittedProducer` CEX) | NON-VACUOUS |

The 10 carried-over Model A invariants (`CapacityBound`, `UniqueRingItem`,
`FIFOBufferOrder`, `NoDuplicatedItem`, `LocationConsistency`,
`ProducerWaiterFIFO`, `ConsumerWaiterFIFO`, `NoBarging`,
`InvSingleResolution`, `InvSinglePublication`) are non-vacuous in Model B for
the same reasons as Model A (all relevant antecedents reachable).

**Model B summary: 16/17 non-vacuous; B3 and B6 each have one vacuous clause
(traceable to the single root cause in F.1.1).** After the one-line fix in
F.1.1, the reviewer confirmed the `Released` location and
`consumerDrained`-nonempty states become reachable, so B3/B6's currently-
vacuous clauses become live and the model remains sound (`Inv` PASS at 1.96M
distinct states).

---

## E. Per-negative CEX reproduction (7)

Each negative was run by the reviewer directly (not via the README). For each:
the cfg checks ONLY the named invariant, so a CEX proves that specific
property is genuine. Each mutation is a SINGLE rule (one `\* DEFECT:` line in
`_gen_neg.py`), confirmed by reading the generator and diffing the generated
module against its parent.

| NEG | Expected invariant | TLC-reported violation | CEX reachability | Single defect? | Verdict |
|-----|--------------------|------------------------|------------------|:--------------:|:-------:|
| NEG-1 DuplicateLease | `UniqueRingItem` | `Error: Invariant UniqueRingItem is violated.` — `ring=<<I0,I0>>` after one `FastPushCommit` (double-append) | reachable, depth 2, not deadlock | yes — `ring'=Append(Append(ring,it),it)` | PASS |
| NEG-2 MoveNotEmptied | `UniqueItemOwner` | `Error: Invariant UniqueItemOwner is violated.` — after `ProducerGrantCommit`, `prodItem[P1]=I1` AND `itemLoc[I1]="Ring"` (item double-claimed) | reachable, depth 6, not deadlock | yes — `prodItem' = prodItem` (source not cleared) | PASS |
| NEG-3 Barging | `NoBarging` | `Error: Invariant NoBarging is violated.` — `FastPushCommit` fires while `prodWaiters=<<P1>>` with `prodPhase[P1]="Suspended"` (eligible waiter barged over) | reachable, depth 6, not deadlock | yes — guard reduced to `Len(ring)<Capacity` (dropped `ProdEligibleSet={}`) | PASS |
| NEG-4 PublishBeforeCommit | `NoPublishedPendingCompletion` | `Error: Invariant NoPublishedPendingCompletion is violated.` — after `ProducerGrantCommit`, `prodCompletion[P1]="Pending"` while `wakeDispatched` was incremented (published before finalize) | reachable, depth 6, not deadlock | yes — `prodCompletion' = ... "Pending"` (not Committed) | PASS |
| NEG-5 CommitAfterClose | `NoCommitAfterClose` | `Error: Invariant NoCommitAfterClose is violated.` — trace Init(Open) -> `CloseLinearize`(Closed) -> `FastPushCommit` at `queueState="Closed"`, `ring=<<I0>>` (commit after close) | reachable, depth 3, not deadlock | yes — `FastPushAdmissible` replaced by `Len(ring)<Capacity /\ ProdEligibleSet={}` (dropped `queueState="Open"`) | PASS |
| NEG-6 CloseDiscardsBuffer | `NoBufferedItemDiscardOnClose` | `Error: Invariant NoBufferedItemDiscardOnClose is violated.` — Init -> `FastPushCommit`(I0) -> `CloseLinearize` clears ring (`ring'=<<>>`) and sets `itemLoc[I0]="Released"` while `closedRing=<<I0>>` and `I0 \notin consumerDrained` | reachable, depth 3, not deadlock | yes — `CloseLinearize` adds `ring'=<<>>` and `itemLoc'=[it \| itemLoc[it]="Ring" @ "Released"]` | PASS |
| NEG-7 FailedPushLosesItem | `FailedPushRetainsOriginalItem` | `Error: Invariant FailedPushRetainsOriginalItem is violated.` — Init -> `CloseLinearize` -> `PushClosed`: `admittedItem[P0]=I0` but `failedPushItem[P0]=NoItem` (lost the item) | reachable, depth 3, not deadlock | yes — `failedPushItem' = ... NoItem` (records NoItem instead of `it`) | PASS |

**Wrong-property gate.** The verify script additionally checks NEG-1's
duplicate-lease defect against `NoBarging` (a property the defect does NOT
violate) and requires it to PASS — confirming the defects are property-
specific, not generic. Reviewer-confirmed: the gate reports `OK (NoBarging
passes under NEG-1; expected property not mis-flagged)`.

**Pairwise vacuity confirmation.** For each NEG-* the corresponding POSITIVE
invariant is non-vacuous (D.1/D.2) AND the negative produces a real CEX on
that exact invariant. Together this proves each of the 7 properties is
meaningful (the positive shows it can hold; the negative shows it can fail).
The only blemish is the F.1.1 defect, which leaves a *disjunct* of B6 (not
the whole invariant) under-exercised; NEG-6 still fires on B6's live clauses.

---

## F. Findings (severity-tagged)

### F.1 BLOCKING

**F.1.1** — See §C.1 above. Model B `ReleaseItem` dead action;
`consumerDrained` ghost never written; B3/B6 `Released`-antecedent clauses
vacuous. One-line fix verified.

### F.2 MAJOR

**F.2.1 — Teardown scope-out mis-attributes coverage to `ClosedAbsorbing`.**
- Location: `docs/spec/e12_queue/README.md` lines 162 (TeardownReusable row)
  and the §"What this model does NOT cover" entry.
- The README claims teardown reuse-freedom is covered by `ClosedAbsorbing`
  (B1) and that "reuse-freedom is a corollary of monotonicity."
- The authority (`docs/e12-queue-state-machine.md` §1.1, lines 42-57) is
  explicit that **`ObjectLifecycle = operational | tearing_down` is a
  SEPARATE axis from `QueueState = Open | Closed`**: "`Closed` is absorbing
  ... `tearing_down` is also absorbing and prevents every ordinary QueuePort
  operation, including close and snapshot." The model has NO
  `ObjectLifecycle`/`tearing_down` variable. `ClosedAbsorbing` pins the
  close axis only; it says nothing about the teardown axis.
- This is not a false-PASS (the model does not claim to *verify* teardown
  irreversibility; it simply does not model `tearing_down` at all, which is a
  legitimate scope choice for a bounded-MPMC-FIFO + Open/Closed safety
  model). But the README's framing ("covered by ClosedAbsorbing"; "corollary
  of monotonicity") conflates two distinct absorbing axes and overstates what
  B1 proves. A reader could incorrectly conclude teardown irreversibility has
  formal coverage.
- Required corrective: re-frame the TeardownReusable scope-out as "teardown
  lifecycle (`operational`/`tearing_down`) is not modelled in B4; it is a
  distinct axis from `queueState` and is deferred to a future model. B4
  verifies close-monotonicity only." Do not claim `ClosedAbsorbing` covers
  teardown.

**F.2.2 — Two scope-outs cite a non-existent "production test suite".**
- Location: `docs/spec/e12_queue/README.md` line 159 (ActiveVictim coverage
  column: "production test coverage in the Queue scheduler-integration
  suite") and line 162 (Teardown coverage column: "Production teardown tests
  in the Queue suite").
- The B2 review (§B.0) and this review independently confirm there is **no
  production Queue code and no Queue test suite** anywhere in `include/`,
  `src/`, or `tests/` (`grep -rln "AsyncQueue|QueueItemLease|QueuePort|
  queue_runnable" include/ src/ tests/` returns zero hits). The named
  "scheduler-integration suite" is a *design document*
  (`docs/e12-queue-scheduler-integration.md`), not executable tests.
- Citing a non-existent test suite as the named coverage for a scope-out
  violates Check 7's requirement that the named coverage "is REAL and
  actually covers the topic." For ActiveVictim and Teardown, the real
  coverage is: ActiveVictim is pinned in-scope by A7/A8/A9 + the
  `expectedProdHead`/`lastProdGranted` history (this part IS real and the
  README states it correctly in the "Why out of B4 scope" column); Teardown
  is simply not modelled (see F.2.1). The "production test" claim adds a
  false coverage assertion on top of the real one.
- Required corrective: drop the "production test coverage in the Queue
  scheduler-integration suite" / "Production teardown tests in the Queue
  suite" wording, or restate it as "future production tests (none exist yet;
  Queue implementation is unauthorized pending B4)."

### F.3 MINOR

**F.3.1 — README state-count/depth table slightly drifted from independent
runs (cosmetic; non-blocking).**
- README line 211 reports Model A "Depth 15"; the reviewer's independent run
  reports depth 13 (1,614,934 states / 352,131 distinct, matching the README
  counts). The README's own footnote (lines 222-224) notes state counts vary
  run-to-run and "verdicts are deterministic," which is accurate. Depth is
  also BFS-scheduling-sensitive. Non-blocking; recorded for completeness.
- NEG-5/6/7 state counts in the README (64/47, 53/36, 64/47) differ from the
  reviewer's runs (38/21, 51/34, 38/21) for the same reason. Verdicts
  identical.

**F.3.2 — README B6 description references `consumerDrained` as load-bearing
(tied to F.1.1).**
- README line 88 and lines 197-202 describe `consumerDrained` as a real
  refinement ghost "written by exactly the actions the design authorizes
  (...; `ReleaseItem`)." Per F.1.1, `ReleaseItem` does not write it as
  shipped. This wording becomes accurate after the F.1.1 fix; until then it
  is misleading. Grouped under the BLOCKING fix.

**F.3.3 — `_gen_neg.py` NEG-5 mutation comment says "dropped queueState =
  Open" but the generated `FastPushCommit` also re-spells the guard.**
- `_gen_neg.py` lines 197-216: the NEG-5 mutation replaces the whole
  `FastPushCommit` body, substituting `FastPushAdmissible` with
  `Len(ring) < Capacity /\ ProdEligibleSet = {}`. The header comment
  summarizes this as "drops queueState = Open," which is accurate (that is
  the one semantic guard removed) but a reader of the generated `.tla` sees
  a full body replacement and must diff to confirm only one guard changed.
  The diff confirms it IS a single-guard change. Non-blocking; the
  wrong-property gate and the single-DEFECT-line convention keep this honest.

---

## G. Conformance to B2 authority (Check 1)

Independently checked against the FINAL Corrective-2 authority (not historical
analysis). Conformance is strong; no superseded semantics leaked into the
model code.

| Authority property (Corrective-2) | Model realization | Conformance |
|---|---|:---:|
| One-shot move-only `QueueItemLease` (no Permit, no reservation) | every move empties source, requires empty destination; `itemLoc` is a per-item function over `Detached/ProducerOp/Ring/ConsumerOp/Released`; `UniqueItemOwner`/`LocationConsistency` express it | MATCH |
| No direct producer->consumer handoff | payload path is ProducerOp -> Ring -> ConsumerOp -> Released only; no action moves ProducerOp -> ConsumerOp | MATCH |
| Selected-waiter grant, FIFO head only | `ProducerGrantCommit`/`ConsumerGrantCommit` grant `ProdFIFOHead`/`ConsFIFOHead`; `expectedProdHead`/`lastProdGranted` history latches the head BEFORE mutation | MATCH |
| No barging (newcomer cannot fast-commit over older eligible waiter) | `FastPushAdmissible` requires `ProdEligibleSet={}`; `NoBarging` (A9) pins it | MATCH |
| Winner-before-publication | completion finalized Committed BEFORE `wakeDispatched'` increment; `CommittedBeforePublished`/`NoPublishedPendingCompletion` pin it | MATCH |
| Close monotonic & idempotent; drain-on-close; producers rejected after close; closed+empty consumer terminal | CL1 `CloseLinearize` (Open->Closed), CL2 `IdempotentClose`, P3 `PushClosed`, P7 `ProducerClosedCommit`, C2 `PopClosedEmpty`, C6 `ConsumerClosedCommit`; consumers drain via `FastPop`/`ConsGrant` post-close | MATCH |
| Close vs producer commit serialized by G+S | both `FastPushCommit` and `CloseLinearize` re-check `queueState` atomically; `CloseProducerRaceLinearizable` (B7) + NEG-5 pin it | MATCH |
| Failed push returns the EXACT original lease (no copy/alias/default-construct) | `admittedItem` ghost captured at admission; `failedPushItem` must equal `admittedItem` for failed-terminal epochs (`FailedPushRetainsOriginalItem`, B4); NEG-7 confirms | MATCH |
| 19 canonical + 6 publication transitions | the model abstracts P2/P3/P4/P5/P6/P7/P9(skip)/P10, C1/C2/C3/C4/C5/C6/C8(skip)/C9, CL1/CL2 and PUB-P-COMM/PUB-C-COMM/PUB-P-CLOSED/PUB-C-CLOSED. P1 (typed factory, outside locks) and P9/C8 (timer expiry) are intentionally out of scope (no timer state in B4). See scope note below. | MATCH (with documented scope) |
| Active-victim stealing unrestricted | not modelled (E7-E9 machinery out of B4 scope); the model has no "steal" action and grants only the FIFO head. Documented as out-of-scope. | MATCH (scope-out) |

**Superseded-semantics scan.** `grep` of both model files for
`permit|reusable|active.owner|veto|cancel|direct.handoff|reservation`
(in non-comment lines) returns zero hits. No reusable-item identity, no
active-owner veto, no quiet drain, no cancellation outcome, no Permit, no
direct handoff appears in the model code. The model is clean Corrective-2.

**Scope note (P1/P9/C8 and the timer).** P1 (typed `QueueItemLease` factory
outside locks) is structural C++, not a Queue-state transition; omitting it
from the TLA+ model is correct. P9/C8 (timer-expiry pre-grant winner) require
E11 timer state, which B4 deliberately does not own (mirrors the E12-B
Semaphore precedent); the README documents this as deferred to a cross-
cutting E11 x E12 Model C. This is a legitimate refinement boundary, not a
conformance gap.

---

## H. Abstraction mapping completeness (Check 2)

The README's refinement map (Model A lines 47-63; Model B lines 41-55;
README §"State dimensions") covers every load-bearing TLA+ concept:

- `Capacity` <-> `AsyncQueue capacity_` (runtime fixed >= 1) — present
- `ring` <-> FIFO lease ring — present, the single handoff authority
- `itemLoc` <-> `QueueItemControl::Location` — present (5 locations)
- `prodWaiters`/`consWaiters` <-> producer/consumer `WaitQueue` intrusive
  FIFOs — present
- `FastPushCommit`<->P2, `PushWouldBlock`<->P4, `ProducerWaitRegister`<->P5,
  `ProducerGrantCommit`<->P6+PUB-P-COMM, `FastPopCommit`<->C1,
  `PopWouldBlock`<->C3, `ConsumerWaitRegister`<->C4,
  `ConsumerGrantCommit`<->C5+PUB-C-COMM, `ReleaseItem`<->C9 — all present
- `PushClosed`<->P3, `ProducerClosedCommit`<->P7+PUB-P-CLOSED,
  `PopClosedEmpty`<->C2, `ConsumerClosedCommit`<->C6+PUB-C-CLOSED,
  `CloseLinearize`<->CL1, `IdempotentClose`<->CL2 — all present (Model B)
- `wakeDispatched` <-> runnable publication count — present
- `queueState` <-> `closed_` — present (Model B)
- History ghosts (`lastAction`, `lastCommitter`, `expectedProdHead/Head`,
  `lastProdGranted`/`lastConsGranted`, `admittedItem`, `closedRing`,
  `consumerDrained`) — all explicitly marked HISTORY/ghost, not production
  fields.

**No modeled variable lacks a production meaning.** Every variable maps to a
production seam or is an explicitly-labelled ghost that exists only to make a
transition property a state predicate.

**Production concepts with no model analog (deliberately):**
- `ObjectLifecycle` (`operational`/`tearing_down`) — NOT modeled. See F.2.1.
  This is a documented scope-out but the README mis-attributes its coverage.
- Owner-slot mapped-value address / Worker stealing (E7-E9) — NOT modeled
  (different refinement layer; correct scope-out).
- Timer/`PreparedTimer`/`TimerRegistration` (E11) — NOT modeled (correct
  scope-out; Model C territory).
- `active_port_calls_` CallGuard ledger — NOT modeled (it is a broad typed-
  call proof outside the Queue-state machine; correct scope-out).

The one abstraction gap worth flagging is `ObjectLifecycle`/teardown (F.2.1):
it is a load-bearing production axis (Corrective-2 §1.1, §9) with NO model
analog. The scope-out is defensible, but the README must not claim
`ClosedAbsorbing` covers it.

---

## I. Fairness / liveness (Check 4)

- **Safety-only claim: VERIFIED.** `grep` of both models for
  `WF_|SF_|<><|[]<>|<>[]|PROPERTY|leadsTo` (in non-comment lines) returns
  zero hits. No `.cfg` contains a `PROPERTY` directive. The README's
  "Safety-only" statement is accurate.
- No fairness is assumed, so no fairness is "unjustifiably strong." The model
  permits infinite stuttering (the `Stutter` action and the `_{Vars}`
  stuttering form), which is the correct safety-only discipline (liveness
  would require scheduler fairness the model cannot justify).
- The README correctly states delivery/progress liveness is deferred to a
  later Model C. No vacuous liveness property is present.

---

## J. State-space adequacy (Check 5)

The bounds `PNodes={P0,P1,P2}`, `CNodes={C0,C1,C2}`, `Items={I0,I1,I2}`,
`Capacity=1` are ADEQUATE for the non-trivial topology. Independently
confirmed via reachability probes:

- `NeverRingFull` -> CEX: ring reaches `Len(ring)=Capacity` (the bound is
  exercised; `CapacityBound` is stressed at its limit, not just `Len=0`).
- `NeverProdEligible` / `NeverConsEligible` -> CEX: non-empty eligibility sets
  are reached (the no-barging machinery is meaningfully exercised).
- `NeverTwoWaiters` / `NeverTwoConsWaiters` -> CEX: 2+ suspended waiters of
  EACH role are reachable (multi-waiter FIFO topology is exercised; the FIFO
  grant head is a real choice, not a singleton).
- All 4 grant/fast-commit actions (`FastPush`/`FastPop`/`ProdGrant`/
  `ConsGrant`) are reachable; the would-block actions (`PushBlock`/`PopBlock`)
  are reachable; register/suspend/release actions are reachable.
- `Capacity=1` is load-bearing as the README claims: with `Capacity=2` and two
  producers, both can fast-push and never park, leaving the grant/FIFO/
  no-barging path (and thus A7/A8/A9/A10/A11) unreachable. The model
  correctly chooses `Capacity=1` to force parking.

**Caveat (F.1.1 again).** In Model B, the dead `ReleaseItem` artificially
shrinks the reachable state space to 1.17M distinct states; after the
one-line fix it is 1.96M. So the *as-shipped* Model B state space is smaller
than intended — but this is a symptom of the BLOCKING defect, not a separate
bound problem. The bounds themselves are adequate; the fix restores the full
intended exploration.

---

## K. Out-of-scope adjudication (Check 7 — 4 topics)

| Topic | README justification precise? | Named coverage real? | False-PASS risk? | Verdict |
|---|---|---|---|---|
| **ActiveVictimUnstealable** | Precise in the "why" column: the model has no steal action; a grant always targets the latched FIFO head; A7/A8/A9 + `expectedProdHead`/`lastProdGranted` history cover it; a steal negative would duplicate NEG-3. Sound. | PARTLY: the in-scope invariant coverage (A7/A8/A9) IS real and exercised. BUT the "production test coverage in the Queue scheduler-integration suite" claim is FALSE — no production tests exist (F.2.2). | No false-PASS for the modeled property (A7/A8/A9 are non-vacuous and NEG-3 exercises the defect class). | MAJOR (F.2.2 — drop the false test-suite claim) |
| **OwnerSlotEarlyErase** | Precise: the one-shot lease makes slot-erase and terminal-outcome-finalization the SAME atomic step; an early-erase negative is the `UniqueItemOwner`/`LocationConsistency` defect class already covered by NEG-2. | YES: A2 + A12 are non-vacuous (D.1); NEG-2 produces a real CEX on `UniqueItemOwner`. | No false-PASS. | PASS |
| **PreparedTimerVisibleToPump** | Precise: the model does not own E11 timer/expiry state; Model B reaches `ClosedOutcome` directly via `PushClosed`/`PopClosedEmpty`; the timer-pump interleaving is E11 authority, deferred to Model C. The close-race half B4 owns is pinned by B7 + NEG-5. | YES: B7 is non-vacuous (D.2); NEG-5 produces a real CEX on `NoCommitAfterClose`. | No false-PASS for the close-race property; the timer-pump property is honestly deferred, not silently dropped. | PASS |
| **TeardownReusable** | IMPRECISE: claims `ClosedAbsorbing` (B1) covers reuse-freedom as "a corollary of monotonicity." But `ObjectLifecycle` (`operational`/`tearing_down`) is a SEPARATE axis from `QueueState` (authority §1.1). B1 pins close-monotonicity, NOT teardown-irreversibility. | PARTLY: `ClosedAbsorbing` is real and non-vacuous for the CLOSE axis; but it does NOT cover the TEARDOWN axis, which is simply unmodelled. The "Production teardown tests in the Queue suite" claim is FALSE (F.2.2). | No false-PASS (the model does not claim to verify teardown; it just mis-labels coverage). The property is genuinely uncovered, but teardown is outside the B4 §C2/§C3 minimum (bounded MPMC FIFO + Open/Closed), so this is a documentation defect, not a gate void. | MAJOR (F.2.1 + F.2.2) |

---

## L. A/B coverage of the B4 gate (Check 8) and key state (Check 9)

**B4 minimum (task §C2/§C3):** bounded MPMC FIFO safety + Open/Closed
monotonicity, with the named invariants.

- **Model A** satisfies the bounded-MPMC-FIFO half: capacity bound, one-shot
  lease uniqueness, no lost/duplicated item, FIFO buffer order, FIFO waiter
  grant, no barging, winner-before-publication, location consistency. 12 named
  + 4 structural invariants, all non-vacuous (D.1). PASS for this half.
- **Model B** satisfies the Open/Closed-monotonicity half: close absorbing,
  no-commit-after-close, committed-before-close-remains-drainable, failed-
  push-retains-original-item, closed-empty-consumer-terminal, no-buffered-
  item-discard-on-close, close-producer-race-linearizable. 7 named B-
  invariants. PASS for this half, **subject to the F.1.1 fix** (two clauses of
  B3/B6 are vacuous as shipped; the rest of B3/B6 and all of B1/B2/B4/B5/B7
  are non-vacuous).

Together A+B cover the B4 minimum after the one-line fix.

**Key state completeness (Check 9):**
- **One-shot lease:** completely modelled via `itemLoc` (5 locations) and
  move-empties-source. No defect.
- **Close:** completely modelled (Open/Closed, CL1/CL2, drain, producer
  rejection, closed+empty terminal). No defect (B3/B6 partial-vacuity aside).
- **Publication state:** modelled via `wakeDispatched`, `lastCommitter`,
  completion finalization before publication. No defect.
- **Teardown (`tearing_down`):** NOT modelled (separate axis; see F.2.1).
  Documented scope-out, mis-attributed coverage.

No one-shot-lease, close, or publication state is incorrectly or
incompletely modelled EXCEPT the `Released` location / `consumerDrained` ghost
in Model B (F.1.1), which is a dead action rather than a missing state per
se.

---

## M. Summary

- **Reproduced independently:** Model A PASS (1.61M/352k/depth 13); Model B
  PASS (5.82M/1.17M/depth 14); all 7 negatives CEX on their EXPECTED named
  invariants; wrong-property gate OK; full `verify-e12-queue-formal.sh` green.
- **BLOCKING (1):** F.1.1 — Model B `ReleaseItem` lists `consumerDrained` in
  BOTH a primed assignment (line 477) and the `UNCHANGED` list (line 479),
  making the action unsatisfiable. The `consumerDrained` ghost is never
  written, the `Released` location is never reached, and the
  `Released`-antecedent clauses of B3 (line 643) and B6 (lines 723, 730) are
  vacuous. One-line fix verified: after removing `consumerDrained` from the
  UNCHANGED list, the model still PASSES `Inv` (now exploring 1.96M distinct
  states, ~40% more), all 7 negatives still CEX, and the previously-vacuous
  clauses become live.
- **MAJOR (2):** F.2.1 (teardown coverage mis-attributed to `ClosedAbsorbing`
  — a distinct axis); F.2.2 (two scope-outs cite a non-existent "production
  test suite").
- **MINOR (3):** F.3.1 (state-count/depth drift in README table); F.3.2
  (`consumerDrained` README wording, tied to F.1.1); F.3.3 (NEG-5 body-
  replacement comment).
- **Non-vacuous:** 16/16 Model A invariants; 16/17 Model B invariants (B3 and
  B6 each have one vacuous clause, single root cause F.1.1).
- **Conformance:** clean Corrective-2; no superseded semantics in model code.
- **Safety-only:** confirmed (no liveness/fairness/PROPERTY).
- **Bounds:** adequate for the non-trivial topology.

The model's design intent and faithfulness to the B2 authority are sound.
The BLOCKING defect is a mechanical TLA+ authoring slip (primed variable also
listed in UNCHANGED) with a trivial, verified fix; it does not indicate a
deeper design flaw. Once F.1.1 is fixed and F.2.1/F.2.2 are re-worded, the B4
Queue formal model should be re-run through `scripts/verify-e12-queue-formal.sh`
and re-reviewed.

```text
E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-1: REQUEST-CHANGES

  BLOCKING: 1  (F.1.1 — Model B ReleaseItem dead action; consumerDrained ghost
                  never written; B3/B6 Released-antecedent clauses vacuous.
                  One-line fix verified by reviewer: Inv still PASS at 1.96M
                  distinct states; all 7 negatives still CEX.)
  MAJOR:    2  (F.2.1 teardown coverage mis-attributed to ClosedAbsorbing;
                  F.2.2 non-existent "production test suite" cited as coverage)
  MINOR:    3  (F.3.1 README count/depth drift; F.3.2 consumerDrained wording,
                  grouped under F.1.1; F.3.3 NEG-5 body-replacement comment)

  Model A:     16/16 invariants non-vacuous and exercised.
  Model B:     16/17 invariants non-vacuous; B3 and B6 each have one vacuous
               clause (single root cause F.1.1).
  Negatives:   7/7 produce real CEX on their EXPECTED named invariant (not
               deadlock, not unreachable); each is a single-rule mutation;
               wrong-property gate confirms defects are property-specific.
  Conformance: clean Corrective-2 (no Permit, no reusable item, no active-owner
               veto, no cancellation outcome, no direct handoff in model code).
  Safety-only: confirmed (no liveness/fairness/PROPERTY).

  B4 (Queue formal model): REQUEST-CHANGES (one BLOCKING, fixable in one line)
  E12-E IMPLEMENTATION AUTHORIZATION: remains DENIED — B4 not yet PASS
```
