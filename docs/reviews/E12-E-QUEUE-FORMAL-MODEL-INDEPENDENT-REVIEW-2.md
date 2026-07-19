# E12-E Queue Formal Model — Independent Review (2)

```text
E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-2
```

> **Scope:** formal-model review of the B4 Queue TLA+ artefacts at HEAD
> `f53faf0` on branch `e12-e-queue-production-impl`, i.e. AFTER the corrective
> that addressed REVIEW-1 (`E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-1`:
> `REQUEST-CHANGES`). This reviewer did NOT author the model, did NOT perform
> REVIEW-1, and does NOT trust the author's self-assessment, REVIEW-1, the
> README, commit `f53faf0`'s message, or any summary. Every blocking claim
> below was reproduced independently by running TLC (v2.19 of 08 August 2024,
> rev `5a47802`) and reading the actual `.tla` source. Scratch probes were
> kept under `/tmp/e12rev2/`; the only durable file added is this document. No
> file under `docs/spec/e12_queue/`, any Queue design doc, or any production
> path was modified. Nothing was committed.
>
> Authority obeyed: `docs/e12-queue-state-machine.md` (Corrective-2 binding
> semantics), `docs/e12-queue.md` (binding decisions above the historical
> marker), `docs/e12-queue-scheduler-integration.md`, and the B2 review
> (`docs/reviews/E12-E-QUEUE-CORRECTIVE-2-INDEPENDENT-ADVERSARIAL-REVIEW-1.md`).

---

## A. Verdict

```text
E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-2: PASS
```

The BLOCKING defect F.1.1 from REVIEW-1 is **fixed, real, and complete**.
The `ReleaseItem` dead action now fires; the `consumerDrained` ghost is
genuinely written; the `Released` location is reachable; the previously-
vacuous `Released`-antecedent clauses of B3 and B6 are now LIVE. An
exhaustive scan of all 159 actions across all 9 model files finds **ZERO**
remaining "variable in both a primed assignment and UNCHANGED" conflicts. The
full B4 gate (`scripts/verify-e12-queue-formal.sh`) is green end-to-end
(exit 0): Model A PASS (12 invariants), Model B PASS (7 invariants), all 7
negatives CEX on their EXPECTED named invariant, wrong-property gate OK.
Both MAJOR findings (F.2.1 teardown scope, F.2.2 non-existent test suite)
and the MINOR findings (F.3.1/F.3.2 state-count/wording) are resolved. An
adversarial hunt for NEW vacuity or defects introduced by the larger state
space found NONE: every load-bearing action fires, `consumerDrained`
precisely tracks every `Released` item, and NEG-6's CEX still hinges on the
`consumerDrained` ghost exactly as designed.

```text
BLOCKING findings: 0
MAJOR findings:     0
MINOR findings:     0

B4 (Queue formal model): PASS
E12-E IMPLEMENTATION AUTHORIZATION: B4 gate satisfied (subject to the broader
    authorization process; this review confirms only the B4 formal-model gate)
```

Independent reproduction summary (all run by THIS reviewer, not the author):

| Model | Result | States generated | Distinct states | Depth |
|-------|--------|-----------------:|----------------:|------:|
| `E12Queue` (Model A) | PASS | 1,614,934 | 352,131 | 13 |
| `E12QueueClosed` (Model B) | PASS | 10,007,847 | 1,962,157 | 14 |
| NEG-1 DuplicateLease | CEX on `UniqueRingItem` | 3 | 3 | 2 |
| NEG-2 MoveNotEmptied | CEX on `UniqueItemOwner` | 13,652 | 8,804 | 6 |
| NEG-3 Barging | CEX on `NoBarging` | 13,650 | 8,803 | 6 |
| NEG-4 PublishBeforeCommit | CEX on `NoPublishedPendingCompletion` | 13,652 | 8,804 | 6 |
| NEG-5 CommitAfterClose | CEX on `NoCommitAfterClose` | 38 | 21 | 3 |
| NEG-6 CloseDiscardsBuffer | CEX on `NoBufferedItemDiscardOnClose` | 51 | 34 | 3 |
| NEG-7 FailedPushLosesItem | CEX on `FailedPushRetainsOriginalItem` | 38 | 21 | 3 |
| WRONG-PROPERTY gate (NEG-1 vs `NoBarging`) | OK (defect specific) | — | — | — |

State counts vary slightly run-to-run (TLC worker scheduling); the verdicts
are deterministic and were all reproduced. The official gate script
(`scripts/verify-e12-queue-formal.sh`) reports exit 0 with all expected
verdicts.

---

## B. Independence and substrate verification

### B.1 TLC runtime and tooling (independently confirmed)

- `java -cp tla2tools.jar tlc2.TLC` reports `TLC2 Version 2.19 of 08 August
  2024 (rev: 5a47802)` — matches the README's claimed runtime exactly.
- Java runtime: OpenJDK 25.0.3 (Ubuntu 25.0.3+9-2-26.04.2-Ubuntu).
- `tla2tools.jar` is the pre-existing untracked jar at the repo root (not
  touched; `git status` confirms it remains untracked).
- All runs used `java -XX:+UseParallelGC -cp $repo/tla2tools.jar tlc2.TLC
  -nowarning [-metadir <isolated-dir>] -config <cfg> <module>`. Isolated
  `-metadir` per run was used after an initial TLC state-directory collision
  (two probes launched in the same second share TLC's timestamp-named meta
  dir); this is a harness artefact, not a model property.
- Scratch probes live under `/tmp/e12rev2/` (copies of the models with
  appended probe invariants; never written into the repo).

### B.2 Model files under review (HEAD `f53faf0`)

- `docs/spec/e12_queue/E12Queue.tla` (Model A) + `.cfg`
- `docs/spec/e12_queue/E12QueueClosed.tla` (Model B — fixed) + `.cfg`
- 7 negatives: `E12QueueNeg{DuplicateLease,MoveNotEmptied,Barging,
  PublishBeforeCommit,CommitAfterClose,CloseDiscardsBuffer,FailedPushLosesItem}`
- `docs/spec/e12_queue/README.md`, `_gen_neg.py`
- `scripts/verify-e12-queue-formal.sh`, `scripts/run-e12-tlc-all.sh`

The corrective commit `f53faf0` touched 5 files: 4 `.tla` (Model B +
NEG-5/6/7, each a one-token deletion of `consumerDrained` from the
`ReleaseItem` UNCHANGED list) and the README. The diff was read in full and
is exactly the F.1.1 corrective plus the README re-wording.

---

## C. BLOCKING fix verification (F.1.1) — independently reproduced

REVIEW-1's BLOCKING F.1.1 was: Model B `ReleaseItem` listed `consumerDrained`
in BOTH the primed assignment `consumerDrained' = consumerDrained \cup {it}`
AND the `UNCHANGED <<..., consumerDrained>>` list, making the conjunction
unsatisfiable, so the action never fired, the ghost was never written, and
the `Released`-antecedent clauses of B3/B6 were vacuous. The mandate required
independent confirmation that the fix is real and complete.

### C.1 The fix is mechanically present and correct

`E12QueueClosed.tla`, `ReleaseItem(c)`, now reads (lines 470-479):

```
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
```

`consumerDrained` is primed on line 477 and is ABSENT from the UNCHANGED
list on line 479. The same one-token fix is present in the three Model-B-
derived negatives (NEG-5/6/7), each at the corresponding `ReleaseItem`
(line 426 / 423 / 422 respectively). Confirmed by reading all four sites.

### C.2 Exhaustive primed/UNCHANGED conflict scan — ZERO conflicts

A Python scanner (`/tmp/scan_conflicts.py`) parsed every top-level action in
all 9 model files, extracted each action's primed-assigned variables and
its UNCHANGED-list variables, and flagged any intersection (a variable in
BOTH). Result:

```
Scanned 9 files, 159 top-level actions total.
CONFLICTS (var in BOTH primed assignment AND UNCHANGED): 0

  E12Queue.tla:                         15 actions, CLEAN
  E12QueueClosed.tla:                   21 actions, CLEAN
  E12QueueNegDuplicateLease.tla:        15 actions, CLEAN
  E12QueueNegMoveNotEmptied.tla:        15 actions, CLEAN
  E12QueueNegBarging.tla:               15 actions, CLEAN
  E12QueueNegPublishBeforeCommit.tla:   15 actions, CLEAN
  E12QueueNegCommitAfterClose.tla:      21 actions, CLEAN
  E12QueueNegCloseDiscardsBuffer.tla:   21 actions, CLEAN
  E12QueueNegFailedPushLosesItem.tla:   21 actions, CLEAN
```

Scan method: regex extraction of `var ' =` (primed assignments, including
the EXCEPT form) and `UNCHANGED <<...>>` / `UNCHANGED var` lists, per
action body (action body = consecutive lines following a column-0
`Name(args) ==` header up to the next column-0 definition or `----`
separator). The scanner prints each action's primed and unchanged sets for
audit; the Model B detail confirms `ReleaseItem` primed = {`consItem`,
`consumerDrained`, `itemLoc`} and unchanged no longer contains
`consumerDrained`. The class of defect that was F.1.1 is absent everywhere.

### C.3 State-count evidence — the dead action now fires

Model B `Inv` PASS, run independently by this reviewer:

```
10,007,847 states generated, 1,962,157 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 14.
```

This is **materially larger** than the buggy REVIEW-1 figure (5,820,858
generated / 1,168,618 distinct, depth 14): ~68% more distinct states
(1.96M vs 1.17M), i.e. ~40% of the intended state space that the dead
`ReleaseItem` had hidden is now exposed. This is the expected signature of
the fix (REVIEW-1 predicted ~1.96M; README claims ~1.9M-2.0M). The match is
exact.

### C.4 The four probe invariants — all reproduce the fix

Probe invariants were built as renamed copies of `E12QueueClosed.tla` with
one appended invariant, run against the FULL state space. A probe that
HOLDS means the antecedent is never reached (vacuity); a probe that is
VIOLATED means the antecedent IS reached (the path is live).

| # | Probe invariant | Expected after fix | Result | CEX witness |
|---|-----------------|--------------------|:------:|-------------|
| P1 | `consumerDrained = {}` | VIOLATED (ghost grows) | **VIOLATED** | `consumerDrained = {I0}`, `itemLoc[I0]="Released"`, `lastAction="ReleaseItem"` |
| P2 | `\A it: itemLoc[it] # "Released"` | VIOLATED (Released reachable) | **VIOLATED** | `itemLoc[I0]="Released"`, `consumerDrained={I0}`, `lastAction="ReleaseItem"` |
| P3 | `~(\E it: itemLoc[it]="Released" /\ it \in consumerDrained)` | VIOLATED (B3/B6 clause live) | **VIOLATED** | `itemLoc[I0]="Released" /\ I0 \in consumerDrained`, `lastAction="ReleaseItem"` |
| P4 (control) | `\A c: ~(consState[c]="Woken" /\ consItem[c]#NoItem /\ itemLoc[consItem[c]]="ConsumerOp")` | VIOLATED (precondition reachable) | **VIOLATED** | `consState[C0]="Woken"`, `consItem[C0]=I0`, `itemLoc[I0]="ConsumerOp"` |

In REVIEW-1 (buggy model), P1 and P2 HELD (vacuous — `consumerDrained` was
always `{}`, `Released` was never reached). They are now VIOLATED, proving
the dead action fires and the previously-vacuous antecedents are reachable.
P3 directly demonstrates the B3 line-643 clause
`(itemLoc[it]="Released" /\ it \in consumerDrained)` and the B6 line-723
disjunct / line-730 implication antecedents are now live.

### C.5 The 3 regenerated negatives — same fix, expected CEX preserved

All three Model-B-derived negatives (NEG-5, NEG-6, NEG-7) carry the same
`consumerDrained`-from-UNCHANGED fix and still produce their expected named-
invariant CEX (see §E). Confirmed by reading each `ReleaseItem` site and by
running each negative.

---

## D. Full B4 gate reproduction (independent)

### D.1 Model A and Model B baseline (independent)

| Model | Verdict | States | Distinct | Depth |
|-------|---------|-------:|---------:|------:|
| Model A (`E12Queue`, 12 invariants) | **PASS** | 1,614,934 | 352,131 | 13 |
| Model B (`E12QueueClosed`, 7 invariants) | **PASS** | 10,007,847 | 1,962,157 | 14 |

Both `Inv` conjunctions PASS. No deadlock, no invariant violation, no
fingerprint collision above TLC's optimistic bound (Model B 8.6E-7
optimistic / 4.0E-7 actual; Model A 2.4E-8 / 3.8E-9 — well within safe
range).

### D.2 All 7 negatives — real CEX on EXPECTED named invariant

Each negative cfg was read and confirmed to check ONLY its expected named
invariant. Each was run directly by this reviewer (not via the README).
Each produces a real, reachable CEX on its expected property — not a
deadlock, not a parse error, not a different property.

| NEG | cfg INVARIANT | TLC violation | States | Distinct | Depth | Single-defect? |
|-----|---------------|---------------|-------:|---------:|------:|:---------------:|
| NEG-1 DuplicateLease | `UniqueRingItem` | `Invariant UniqueRingItem is violated.` (`ring=<<I0,I0>>`) | 3 | 3 | 2 | yes — `ring'=Append(Append(ring,it),it)` |
| NEG-2 MoveNotEmptied | `UniqueItemOwner` | `Invariant UniqueItemOwner is violated.` | 13,652 | 8,804 | 6 | yes — `prodItem' = prodItem` |
| NEG-3 Barging | `NoBarging` | `Invariant NoBarging is violated.` | 13,650 | 8,803 | 6 | yes — guard drops `ProdEligibleSet={}` |
| NEG-4 PublishBeforeCommit | `NoPublishedPendingCompletion` | `Invariant NoPublishedPendingCompletion is violated.` | 13,652 | 8,804 | 6 | yes — `prodCompletion' = ... "Pending"` |
| NEG-5 CommitAfterClose | `NoCommitAfterClose` | `Invariant NoCommitAfterClose is violated.` | 38 | 21 | 3 | yes — `FastPushAdmissible` -> `Len(ring)<Capacity /\ ProdEligibleSet={}` (drops `queueState="Open"`) |
| NEG-6 CloseDiscardsBuffer | `NoBufferedItemDiscardOnClose` | `Invariant NoBufferedItemDiscardOnClose is violated.` | 51 | 34 | 3 | yes — `CloseLinearize` adds `ring'=<<>>` + `itemLoc'[Ring]="Released"` |
| NEG-7 FailedPushLosesItem | `FailedPushRetainsOriginalItem` | `Invariant FailedPushRetainsOriginalItem is violated.` | 38 | 21 | 3 | yes — `failedPushItem'[p]=NoItem` (instead of `it`) |

The 3 regenerated negatives (NEG-5/6/7) all carry the F.1.1 fix in their
`ReleaseItem` and still CEX on their named properties.

### D.3 Wrong-property gate — OK

The verify script additionally checks NEG-1's duplicate-lease defect against
`NoBarging` (a property the defect does NOT violate) and requires it to
PASS — confirming defects are property-specific. Independent confirmation:
the official gate run reports `OK WRONG-PROPERTY gate (NoBarging passes
under NEG-1; expected property not mis-flagged)`.

### D.4 Official gate script — exit 0

`scripts/verify-e12-queue-formal.sh` run end-to-end:

```
PASS  E12Queue [Model A, 12 invariants]
PASS  E12QueueClosed [Model B, 7 invariants]
CEX   NEG-QUEUE-1 DuplicateLease (UniqueRingItem violated, as expected)
CEX   NEG-QUEUE-2 MoveNotEmptied (UniqueItemOwner violated, as expected)
CEX   NEG-QUEUE-3 Barging (NoBarging violated, as expected)
CEX   NEG-QUEUE-4 PublishBeforeCommit (NoPublishedPendingCompletion violated, as expected)
CEX   NEG-QUEUE-5 CommitAfterClose (NoCommitAfterClose violated, as expected)
CEX   NEG-QUEUE-6 CloseDiscardsBuffer (NoBufferedItemDiscardOnClose violated, as expected)
CEX   NEG-QUEUE-7 FailedPushLosesItem (FailedPushRetainsOriginalItem violated, as expected)
OK    WRONG-PROPERTY gate (NoBarging passes under NEG-1; expected property not mis-flagged)
  TLC runtime version: 2.19 of 08 August 2024 (rev: 5a47802)
=== gate 0-ed ===
```

Exit code 0. The script's `named_violation` grep requires the EXACT
expected property name in TLC's output, so a CEX on the wrong property
would FAIL the gate. It does not.

---

## E. Per-negative CEX confirmation (with the F.1.1-related focus on NEG-6)

### E.1 NEG-6 CEX hinges on `consumerDrained` exactly as designed

The mandate specifically required confirming that NEG-6's CEX still hinges
on the `consumerDrained` ghost: a close-discarded item is `Released` but
NOT in `consumerDrained`, while a consumer-drained item IS in
`consumerDrained`. The full NEG-6 CEX trace (independently reproduced):

```
State 1: <Initial predicate>
  itemLoc = (I0 :> "Detached" @@ ...),  ring = <<>>,  consumerDrained = {},  queueState = "Open"

State 2: <FastPushCommit>
  itemLoc = (I0 :> "Ring" @@ ...),  ring = <<I0>>,  consumerDrained = {},  queueState = "Open"

State 3: <CloseLinearize>   <-- the DEFECT mutation fires
  itemLoc = (I0 :> "Released" @@ ...),  ring = <<>>,  closedRing = <<I0>>,
  consumerDrained = {},  queueState = "Closed",  lastAction = "Close"
```

State 3 is the violating state. `I0` was buffered at close (`closedRing=<<I0>>`),
was released directly by `CloseLinearize` (`itemLoc[I0]="Released"`), and
is NOT in `consumerDrained` (`consumerDrained={}`) because the consumer
drain path (`ReleaseItem`) was never traversed. This is exactly the shape
B6's line-730 implication `(itemLoc[it]="Released" => it \in consumerDrained)`
forbids for `closedRing` items — and exactly what the `consumerDrained`
ghost exists to distinguish. The B6 line-723 disjunct
`(itemLoc[it]="Released" /\ it \in consumerDrained)` and the line-643 B3
clause are the consumer-drained LEGAL path; NEG-6's CEX takes the ILLEGAL
close-discard path and is caught because `consumerDrained` does not contain
the close-discarded item.

Contrast with the CORRECT model: in the fixed Model B, the ONLY way an item
becomes `Released` is via `ReleaseItem`, which always adds it to
`consumerDrained`. The probe `ReleasedImpliesDrained ==
\A it: itemLoc[it]="Released" => it \in consumerDrained` PASSES across the
full 1.96M-state space of the correct Model B (§F.2). So the ghost
precisely partitions consumer-drained `Released` items (legal) from any
other `Released` item (only reachable via the NEG-6 defect). The ghost
genuinely does the job the README claims for it.

### E.2 NEG-5/NEG-7 regenerated negatives — single-defect confirmed

Direct diff of each regenerated negative against Model B (excluding the
replaced header comment block) shows exactly one DEFECT line:

- **NEG-5** (`E12QueueNegCommitAfterClose.tla` line 167): `FastPushAdmissible`
  replaced inline by `Len(ring) < Capacity /\ ProdEligibleSet = {}` with
  comment `\* DEFECT: dropped queueState = "Open" (commit after close)`.
  Exactly one guard removed.
- **NEG-6** (`E12QueueNegCloseDiscardsBuffer.tla`, `CloseLinearize`): adds
  `ring' = <<>>` and `itemLoc' = [it \in Items |-> IF itemLoc[it]="Ring"
  THEN "Released" ELSE itemLoc[it]]` with comment
  `\* DEFECT: discard buffered items on close`. `consumerDrained` is left
  UNCHANGED (correctly — the defect must NOT add to it, or the CEX would
  not fire). Single-rule defect.
- **NEG-7** (`E12QueueNegFailedPushLosesItem.tla` line 192):
  `failedPushItem' = [failedPushItem EXCEPT ![p] = NoItem]` with comment
  `\* DEFECT: loses the original item (records NoItem != it)`. Single-rule
  defect.

The `_gen_neg.py` generator's single-DEFECT-line convention holds.

---

## F. New-vacuity / new-defect hunt (adversarial)

The fix makes `ReleaseItem` fire and exposes ~68% more distinct state. The
mandate required scrutinizing whether any invariant now passes only weakly,
whether any new deadlock appeared, and whether the `consumerDrained` ghost
now genuinely distinguishes the two cases. Findings: **no new vacuity, no
new defect, no new deadlock.**

### F.1 Every load-bearing action fires (dead-action hunt)

The REVIEW-1 dead action was the canonical hidden-vacuity source. A dead
action (guard never satisfiable) would make any clause keyed on its
`lastAction` vacuous. This reviewer built per-action probes
`lastAction # "<kind>"` for the close-axis and grant actions not already
covered by §C.4, and ran each against the full state space. All are
VIOLATED (the action fires):

| Action | Probe verdict | CEX states | Meaning |
|--------|:-------------:|-----------:|---------|
| `FastPush` | VIOLATED (§C.4 / A9 probe) | — | fast push fires |
| `FastPop` | VIOLATED (A9 probe) | — | fast pop fires |
| `ReleaseItem` | VIOLATED (P1/P2/P3) | 500 | consumer drain fires |
| `Close` | VIOLATED (B3 probe) | 3 | close fires |
| `IdempotentClose` | VIOLATED | 37 | CL2 fires |
| `PushClosed` | VIOLATED | 38 | P3 fires (B4 antecedent live) |
| `PopClosedEmpty` | VIOLATED | 47 | C2 fires (B5 antecedent live) |
| `ProdGrant` | VIOLATED | 23,343 | P6 fires (A10/A11 antecedent live) |
| `ConsGrant` | VIOLATED | 15,015 | C5 fires (A8 antecedent live) |
| `ProdClosedGrant` | VIOLATED | 23,236 | P7 fires (B4 closed path live) |
| `ConsClosedGrant` | VIOLATED | 14,898 | C6 fires (B5 closed path live) |

Plus `ProdRegister`, `ConsRegister`, `ProdSuspend`, `ConsSuspend`,
`PushBlock`, `PopBlock`, `Stutter` are trivially reachable (the wait/suspend
path is exercised whenever the ring is full/empty, which `CapacityBound`'s
non-vacuity proves). **No dead action exists in Model B.** The close+drain
machinery that the F.1.1 fix exposed is fully live.

### F.2 `consumerDrained` precisely tracks every `Released` item

Probe `ReleasedImpliesDrained == \A it \in Items : (itemLoc[it]="Released"
=> it \in consumerDrained)` PASSES across the full 10,007,847-state /
1,962,157-distinct Model B state space (depth 14). This means: in the
CORRECT model, `ReleaseItem` is the sole writer of the `Released` location
AND it always adds to `consumerDrained`, so the ghost and the location are
in perfect 1:1 correspondence. Consequently:

- B6's line-730 implication `(itemLoc[it]="Released" => it \in
  consumerDrained)` is **non-vacuous AND holds because the model is
  correct**, not because the antecedent is unreachable. Its antecedent IS
  reachable (P2/P3), and the only way to violate it is the NEG-6 defect.
- The ghost genuinely distinguishes consumer-drained `Released` items
  (`consumerDrained` contains them) from close-discarded ones (only
  reachable via the NEG-6 mutation, which does NOT add to `consumerDrained`).
  The README's description of `consumerDrained` (lines 88, 197-207) is now
  accurate.

### F.3 No new deadlock

No probe and no gate run reported a deadlock. The model retains the
`Stutter` action and the `_{Vars}` stuttering form, so every state has a
successor; safety-only BFS reaches the full finite state space (0 states
left on queue in every run). The ~68% state-space expansion did not
introduce a terminal state.

### F.4 Non-vacuity spot-checks (B3, B6, B7, A9, A10, A11) on the LARGER space

REVIEW-1's per-invariant vacuity analysis was against the buggy 1.17M-state
space. The mandate required re-confirming non-vacuity on the corrected 1.96M
space. Negated-antecedent probes, all VIOLATED (antecedent reached):

| Inv | Probe (negated antecedent) | Result | Conclusion |
|-----|----------------------------|:------:|------------|
| B3/B6 antecedent | `closedRing = NoSnap` | **VIOLATED** (3 states) | close fires; `closedRing # NoSnap` reached |
| B7 antecedent | `~(queueState="Closed" /\ \E p: prodCompletion[p]="Committed")` | **VIOLATED** (51 states) | Closed+Committed-producer reached |
| B6 `Released` clause | P2 `\A it: itemLoc[it]# Released` | **VIOLATED** | Released reachable |
| B6 `Released /\ drained` clause | P3 | **VIOLATED** | B3-643 / B6-723 antecedents live |
| A9 antecedent | `lastAction # "FastPush" /\ # "FastPop"` | **VIOLATED** (3 states) | fast push/pop reached |
| A10 antecedent | `lastCommitter = NoNode` | **VIOLATED** (13,652 states) | a ring commit happened |
| A11 antecedent | `lastAction # "ProdGrant"` | **VIOLATED** (13,652 states) | ProdGrant reached |

All spot-checked invariants are non-vacuous on the corrected, larger state
space. No invariant that REVIEW-1 found non-vacuous has become vacuous
after the fix, and the two previously-vacuous clauses (B3-643, B6-723/730)
are now live.

---

## G. MAJOR / MINOR adjudication (REVIEW-1 F.2.x, F.3.x)

### G.1 F.2.1 (MAJOR) — Teardown scope-out: RESOLVED

REVIEW-1 required re-framing `TeardownReusable` as honestly UNcovered (a
separate `ObjectLifecycle = operational | tearing_down` axis not modelled
in B4), NOT claimed as a `ClosedAbsorbing` corollary.

Current README (`docs/spec/e12_queue/README.md` line 167, TeardownReusable
row "Why out of B4 scope" column) reads:

> The teardown lifecycle `ObjectLifecycle = operational | tearing_down`
> (Corrective-2 state-machine §1.1, §9) is a **separate axis** from
> `QueueState = Open | Closed` and is **not modelled** in B4 at all — there
> is no `tearing_down` variable. B4 verifies close-monotonicity only;
> `ClosedAbsorbing` (B1) pins the close axis, **NOT** teardown irreversibility.

And the Coverage column:

> Not covered by any B4 invariant (the `tearing_down` axis is absent).
> Deferred to a future teardown Model C that adds the `ObjectLifecycle` axis.

This is honest and correct. `grep` for the old mis-attribution
("corollary of monotonicity", "covered by ClosedAbsorbing" for teardown)
returns zero hits. The B1 description (line 124) is correctly scoped to
"`queueState` is monotonic Open->Closed (close is absorbing)". The
authority (`docs/e12-queue-state-machine.md` §1.1 lines 47-57) explicitly
separates the two axes; the README now matches it. **RESOLVED.**

### G.2 F.2.2 (MAJOR) — Non-existent "production test suite": RESOLVED

REVIEW-1 required removing the false "production test coverage in the Queue
scheduler-integration suite" / "Production teardown tests in the Queue
suite" claims. Current README:

- ActiveVictim coverage column (line 164): "future production tests, **none
  existing yet** because Queue implementation is unauthorized pending B4,
  will cover the worker-selection half."
- Teardown coverage column (line 167): "No production teardown tests exist
  yet; Queue implementation is unauthorized pending B4."

`grep -ni "production test" README.md` returns zero hits. The B2 review
(§B.0) and this review independently confirm no Queue production code/tests
exist (`grep -rln "AsyncQueue\|QueueItemLease\|QueuePort\|queue_runnable"
include/ src/ tests/` returns zero hits; the only untracked test file
`tests/test_t3_simple.cpp` is an unrelated AsyncCondition smoke test).
**RESOLVED.**

### G.3 F.3.1 / F.3.2 (MINOR) — README state counts and consumerDrained wording: RESOLVED

- The state-count table (README lines 214-225) now reports RANGES spanning
  author + independent-reviewer reproductions, with a footnote explaining
  run-to-run variance and a note that "Model B's distinct-state count rose
  from ~1.17M to ~1.9M-2.0M after the F.1.1 corrective." This reviewer's
  independent Model A (1,614,934 / 352,131 / depth 13) and Model B
  (10,007,847 / 1,962,157 / depth 14) runs fall within the stated ranges
  (~1.6M / ~352k / 13-15 and ~8M-10M / ~1.9M-2.0M / 14-15). Consistent.
- The `consumerDrained` wording (README lines 88, 197-207) describing it
  as "written by exactly the actions the design authorizes (...;
  `ReleaseItem`)" is now accurate, because `ReleaseItem` does in fact write
  it after the F.1.1 fix (§C.4 P1, §F.2). **RESOLVED.**

### G.4 F.3.3 (MINOR) — `_gen_neg.py` NEG-5 comment: not re-litigated

REVIEW-1 F.3.3 was a non-blocking comment-clarity note about NEG-5's
body-replacement. The wrong-property gate and single-DEFECT-line convention
keep this honest (§E.2 confirms NEG-5 is a single-guard change). Not
blocking; no change required.

---

## H. Conformance to B2 / Corrective-2 authority (re-confirmed)

Independently checked against the FINAL Corrective-2 authority (state-
machine §1.1, §3, §7; e12-queue.md binding decisions; scheduler-integration
doc; B2 review). Conformance is unchanged from REVIEW-1 (which found it
strong) and is not affected by the F.1.1 fix (a mechanical authoring slip,
not a design change).

| Authority property | Model realization | Conformance |
|---|---|:---:|
| One-shot move-only `QueueItemLease` (no Permit, no reservation) | every move empties source, requires empty destination; `itemLoc` over `Detached/ProducerOp/Ring/ConsumerOp/Released`; `UniqueItemOwner`/`LocationConsistency` | MATCH |
| No direct producer->consumer handoff | payload path ProducerOp -> Ring -> ConsumerOp -> Released only | MATCH |
| Selected-waiter grant, FIFO head only | `ProducerGrantCommit`/`ConsumerGrantCommit` grant the FIFO head; `expected*Head`/`last*Granted` history latches before mutation | MATCH |
| No barging | `FastPushAdmissible` requires `ProdEligibleSet={}`; `NoBarging` (A9) | MATCH |
| Winner-before-publication | completion finalized Committed BEFORE `wakeDispatched'++`; A10/A11 | MATCH |
| Close monotonic & idempotent; drain-on-close; producers rejected after close; closed+empty consumer terminal | CL1 `CloseLinearize`, CL2 `IdempotentClose`, P3 `PushClosed`, P7 `ProducerClosedCommit`, C2 `PopClosedEmpty`, C6 `ConsumerClosedCommit`; B1/B2/B3/B5/B6/B7 | MATCH |
| Close vs producer commit serialized by G+S | both re-check `queueState` atomically; B7 + NEG-5 | MATCH |
| Failed push returns EXACT original lease | `admittedItem` captured at admission; `FailedPushRetainsOriginalItem` (B4); NEG-7 | MATCH |
| Teardown (`operational`/`tearing_down`) is a SEPARATE axis | NOT modelled (correctly); README now says so honestly (F.2.1 resolved) | MATCH (scope-out) |
| 19 canonical + 6 publication transitions abstracted | P2-P7,P9(skip),P10; C1-C6,C8(skip),C9; CL1/CL2; PUB-* . P1 (typed factory, outside locks), P9/C8 (timer expiry, E11 state) deliberately out of scope | MATCH (documented scope) |

**Superseded-semantics scan.** A non-comment-line scan of all 9 model files
for `permit|reusable|active.owner|veto|cancel|direct.handoff|reservation|
quiet.drain` returns only 3 hits, all in comment lines that document these
concepts as OUT OF SCOPE (e.g. "Teardown, timer expiry, and external
cancellation remain OUT OF SCOPE"). No superseded semantics leak into model
code. The model is clean Corrective-2.

**Safety-only.** A scan of all 9 model files and cfgs for
`WF_|SF_|<><|[]<>|PROPERTY|leadsTo` returns ZERO hits in non-comment lines.
No `.cfg` contains a `PROPERTY` directive. The model is safety-only; no
vacuous liveness.

**ASSUME.** The only `ASSUME` (Model B line 76) is constant well-formedness
(`PNodes#{}`, distinctness of nodes/items, `Capacity \in 1..2`, `Capacity <=
Cardinality(Items)`). It does not narrow the reachable state space; ghosts
never exclude reachable states.

---

## I. Summary

- **F.1.1 fix verified real and complete (independent):** the one-token
  deletion of `consumerDrained` from the `ReleaseItem` UNCHANGED list is
  present in Model B and all 3 Model-B-derived negatives. Model B `Inv`
  PASSES at 10,007,847 states / 1,962,157 distinct / depth 14 (materially
  larger than the buggy 1.17M; matches the predicted ~1.96M). The four
  probe invariants confirm the ghost grows (P1), the `Released` location is
  reachable (P2), the B3-643/B6-723/730 antecedents are live (P3), and the
  `ReleaseItem` precondition is reachable (P4 control).
- **Zero primed/UNCHANGED conflicts anywhere:** an exhaustive scan of all
  159 actions across all 9 model files finds ZERO conflicts. The class of
  defect that was F.1.1 is absent everywhere.
- **Full B4 gate green (independent):** Model A PASS (12 inv); Model B PASS
  (7 inv); all 7 negatives CEX on their EXPECTED named invariant (not
  deadlock); wrong-property gate OK; `scripts/verify-e12-queue-formal.sh`
  exit 0.
- **MAJOR/MINOR resolved:** F.2.1 (teardown honestly scoped out, not a
  ClosedAbsorbing corollary), F.2.2 (no false "production test suite"),
  F.3.1/F.3.2 (state counts consistent, `consumerDrained` wording accurate).
- **No new vacuity / defect introduced:** every load-bearing action fires
  (§F.1); `consumerDrained` precisely tracks every `Released` item
  (`ReleasedImpliesDrained` PASSES over the full 1.96M space, §F.2); NEG-6's
  CEX still hinges on `consumerDrained` exactly as designed (§E.1); no new
  deadlock; B3/B6/B7/A9/A10/A11 all non-vacuous on the larger space (§F.4).
- **Conformance:** clean Corrective-2; no superseded semantics in model
  code; safety-only; bounds adequate.

The F.1.1 fix is a clean, minimal corrective that resolves the only
BLOCKING defect, exposes the previously-hidden state space, makes the
load-bearing ghost and `Released`-antecedent clauses live, and preserves
every PASS verdict and every negative CEX. No compensating defect, new
vacuity, or regression was introduced. The model conforms to the B2
Corrective-2 authority throughout.

```text
E12-E-QUEUE-FORMAL-MODEL-INDEPENDENT-REVIEW-2: PASS

  BLOCKING: 0
  MAJOR:    0
  MINOR:    0

  F.1.1 fix:      VERIFIED REAL AND COMPLETE (independent TLC reproduction).
                  Model B Inv PASS at 10,007,847 / 1,962,157 / depth 14
                  (up from buggy 5,820,858 / 1,168,618; ~40% more state exposed).
                  consumerDrained ghost now grows (P1 VIOLATED);
                  Released location now reachable (P2 VIOLATED);
                  B3-643 / B6-723 / B6-730 antecedents now LIVE (P3 VIOLATED).
  Conflict scan:  0 primed/UNCHANGED conflicts across 159 actions / 9 files.
  Full gate:      Model A PASS (12 inv); Model B PASS (7 inv);
                  7/7 negatives CEX on EXPECTED named invariant;
                  wrong-property gate OK; verify-e12-queue-formal.sh exit 0.
  New vacuity:    NONE. All load-bearing actions fire; consumerDrained
                  precisely tracks every Released item (ReleasedImpliesDrained
                  PASS over full 1.96M space); NEG-6 CEX hinges on
                  consumerDrained as designed; no new deadlock.
  MAJOR/MINOR:    F.2.1 / F.2.2 / F.3.1 / F.3.2 RESOLVED.
  Conformance:    clean Corrective-2 (no Permit, no reusable item, no
                  active-owner veto, no cancellation outcome, no direct
                  handoff in model code). Safety-only. Bounds adequate.

  B4 (Queue formal model): PASS
```
