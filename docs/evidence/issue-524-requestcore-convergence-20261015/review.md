# #524 independent model-preservation review (R4)

The freeze gate required by the issue: a review by reviewers other than the
implementer, on six axes, with the verdict vocabulary
`APPROVE_MODEL_FREEZE` / `REQUEST_CHANGES` / `STOP_CORRESPONDENCE_FAILURE`.

Three reviewers were dispatched independently, each with the artifact, the
claims to attack, and explicit instructions to falsify rather than endorse.
Each worked read-only on the worktree (scratch under `/tmp`), with the pinned
TLC jar, and each produced its own TLC runs.

## Verdicts

| Reviewer | Scope | Verdict |
|---|---|---|
| A | semantic correspondence, behaviour inclusion (§A/§B) | `REQUEST_CHANGES` — **semantics not falsified**; the changes requested were the pending documentation and run receipts, "not model-semantics defects … the model semantics themselves are approved for freeze" |
| B | safety, liveness, mutation adequacy (§C/§D/§E) | `APPROVE_MODEL_FREEZE` |
| C | history integrity, config/gate hygiene (§F) | `REQUEST_CHANGES` — mechanics all PASS (C1/C2/C4/C5); only C3 failed, because the living roadmap docs still assert the removed flavor in the present tense and no #524 receipt existed yet |

No reviewer returned `STOP_CORRESPONDENCE_FAILURE`. The two `REQUEST_CHANGES`
were the same two deliverables — the pending documentation and the pending run
receipts. The documentation deliverable is addressed by commits
`docs(formal): update RequestCore correspondence` and
`docs(evidence): record #524 verification and review`; of the receipt
deliverable the **safety** half is closed with a committed receipt, while the
**liveness** half remains open (the gate was started on the frozen snapshot and
stopped on owner instruction). That open half then drew the round-2 review
recorded at the end of this file. The per-item disposition is below.

## Reviewer A — semantic correspondence and behaviour inclusion

**Confirmed** (its own enumeration of every `binding_live` write, and its own
git archaeology):

- Every current write of the public binding is `reserve` (`:160`), `accept`
  (`:191`), `discard_public_result` (`:283`), `consume_public_result` (`:332`),
  the named `SLUICE_B2_MUTANT_OBSERVE_CONSUMES` seam (`:310`), and
  `release_slot_` (`:583`, reached only for a non-live binding). Both live
  release edges require `published`.
- The pre-F2 `Completion` release edge called `release_public_binding`, whose
  guard was `terminal_chosen && execution_refs == 0 && (publication_inflight ||
  published)` — the old `ReleaseBind` guard verbatim; F2 deleted `Completion`
  and `3bdc61d7` deleted the remaining `release_public_binding`.
- The removed edge really did create states the survivors cannot:
  `stage = 3 ∧ pub = "inflight" ∧ consumed = discarded = FALSE ∧ pubDoneIds = {}`
  is reachable at BASE (1659 states) and unreachable after convergence.
- Guards of `Consume`/`DiscardPublic` are the same predicate as the C++
  release edges on the reachable set, so they are complete; the deliberate
  over-approximations live only in mutants.
- No legal current release path is unmodelled. It also noted the two
  *pre-existing* abstractions (no observer pin in `Reclaim`; no zero-op accept)
  and correctly classified them as out of this slice's scope.

**Requested changes and disposition**

| Finding | Disposition |
|---|---|
| `docs/roadmap/v1-formal-evidence-map.md:244,268` still assert `ReleaseBind` as the live compatibility release flavor | **fixed** in `docs(formal): update RequestCore correspondence` — both rows now state the #524 removal and name `InvBindImpliesPublished` |
| `docs/roadmap/b1-1-request-core-evidence.md:566,568,678` (and the cfg count at `:137`) list the retired mutant and action as live | **fixed** in `docs(formal): mark the superseded B1-1 rows` (`eb0a7350`) — a dated supersession banner plus inline `[superseded by #524]` marks on the three rows and a post-#524 inventory; the file's historical numbers are not rewritten. Reviewer A's objection was recorded before that commit and is kept here verbatim as the review-time finding |
| No full-run receipt for the converged model | **safety: fixed** — the complete safety run is a committed receipt (`raw/tlc/safety.log`, exit 0, 31,289,609 / 5,359,732 / depth 53). **liveness: still open** — the formal liveness receipt is PENDING: the gate was started on the frozen snapshot and stopped on owner instruction, so `raw/tlc/liveness.log` is partial and `exit-codes.tsv` records `liveness STOPPED_ON_OWNER_REQUEST`. Reviewer B's complete-space liveness run is corroborating scratch evidence, not a substituted receipt (see `resource-and-execution-manifest.md` and the round-2 section below) |

## Reviewer B — safety, liveness, mutation adequacy

`APPROVE_MODEL_FREEZE`. Its independent evidence, all reproduced without
trusting the implementer's summary:

- **Transition diff.** Comment-stripped diff is exactly the five code changes
  claimed; `Next_new = Next_old` minus the `ReleaseBind` pairs; no transition
  added; `Fair` byte-identical.
- **Isolation control.** BASE model with *only* `\/ ReleaseBind(s)` removed
  yields `31,289,609 / 5,359,732 / depth 53`, byte-identical to the converged
  model — so the whole behavioural delta is that one disjunct and the new
  flag-conditional is inert at the reference configuration.
- **Complete runs.** Reference safety clean (31,289,609 / 5,359,732, 50 s);
  reference liveness clean with `PROPERTY L1` and `PROPERTY L5` over the
  complete space (5,359,732 distinct, 28 m 49 s wall, 10 incremental temporal
  passes, no `Temporal properties were violated`).
- **No stuck states.** For every state where `ReleaseBind` was enabled,
  `CompletePublish` or `Consume` remains enabled, so no obligation is stranded;
  it also verified empirically that the gate's `-deadlock` flag shape still
  reports a temporal violation for a stranded obligation (so the flag cannot
  mask one).
- **`InvBindImpliesPublished`.** Independently proved inductive via its
  contrapositive, confirmed satisfied on the full reference run, and confirmed
  falsifiable/non-vacuous by a tripwire run.
- **`MutReclaimIgnoresPub`.** Clean pass verified, and the reclassification
  justified: equal distinct counts force equal reachable sets, and its extra
  precondition is unreachable; with `MutDiscardInflight = TRUE` the invariant
  bites again, so the witness has teeth rather than being a silenced gate.
- **`MutDiscardKeepsBind`.** Reachable; killed only by `InvReleasedNotLive`;
  hand-checked that every other invariant in its cfg holds in the violating
  state; distinct from both siblings on action, mutated guard and witness
  state.
- **No silent kill drift.** It ran the complete RequestCore battery and every
  gate reported its named kill, including the two whose expected invariant sits
  late in the list.
- One unexplained cosmetic statistic (TLC maximum outdegree 10 vs 11 with
  identical generated/distinct/depth) is recorded in `transition-diff.md`
  rather than explained away; it feeds no verdict.

## Reviewer C — history integrity and hygiene

Mechanics all PASS; only the stale-current-claims item failed.

| Check | Result |
|---|---|
| C1 frozen evidence byte-integrity (`git diff 9a501d1c -- docs/evidence/f2-… f6-… docs/review`) | EMPTY — nothing frozen was touched; `formal-assets.tsv` intact; the reuse premise `git diff eb7bf055 821c489f -- formal/tla/ scripts/verify_tla.sh` still empty |
| C2 changed-file set | inside the allowed set; `src/`, `include/`, `apps/`, root spec diff EMPTY |
| C3 stale references | **FAIL at review time** — 7 present-tense claims in the two living roadmap files, plus the out-of-scope B1-1 record; every `docs/review/` and frozen-evidence hit is a correctly-labelled dated record; `tests/`, `xmake/`, CI clean |
| C4 constant-set integrity | 23/23 cfgs declare exactly the module's 21 constants; correct booleans; verified by its own script and three TLC runs |
| C5 gate referential integrity | 171/171 `run_*` references resolve; gate inventory delta is exactly the four expected lines; the one `violate`→`clean` downgrade is recorded, not silent |

**Disposition of C3:** the seven present-tense claims are in dated phase records
(B1-B, B1-C, B1-D, B2) whose ReleaseBind clauses were true at their baselines,
so they are not rewritten; the #524 convergence is recorded as a new dated
section in `docs/roadmap/v1-conformance.md` that names every superseded row and
carries the new evidence. The two living-map rows that cannot be read as dated
history (`v1-formal-evidence-map.md:244,268`) were edited in place. The B1-1
record, flagged at review time as an out-of-scope dated record, was
subsequently marked in place (`eb0a7350`, after this review round): a dated
supersession banner states which rows the convergence falsifies, and those rows
carry inline `[superseded by #524]` marks without their historical numbers being
rewritten. This is the disposition the round-2 review confirmed as required.

## Residuals carried forward (not defects introduced by this slice)

1. `docs/roadmap/b1-1-request-core-evidence.md` rows `:137`, `:566`, `:678`
   were superseded by this convergence. **Closed:** marked in place by
   `eb0a7350` (dated banner + inline supersession marks, historical numbers
   retained), so no owner action remains on this item.
2. Pre-existing abstractions, unchanged and unchanged in scope: `Reclaim`
   carries no observer-phase pin (ObserverCore owns it); the model has no
   zero-op accept action; the pre-claim physical outcome path is C++-only.
3. `formal-assets.tsv` in the frozen F6 directory is historical and was not
   overwritten; the successor classification is
   `formal-assets.tsv` in this directory.
4. The formal liveness receipt is outstanding. This is the only item of the
   round-1 `REQUEST_CHANGES` that is neither fixed nor dismissed; see below.

## Round-2 independent review (2026-10-15) — `REQUEST_CHANGES`

A further review, by a reviewer other than the implementer, of the pushed head
`07ca71e8`, returned `REQUEST_CHANGES`: the `ReleaseBind` removal and the
mutation migration were accepted, but the formal evidence set was found
internally inconsistent. It read the model, the 23 cfgs, the gate diff, the C++
correspondence and the evidence files; it ran no TLC and no C++ test, and it
published no comment and merged nothing.

Accepted as correct without change: `ReleaseBind` removal; retirement of the old
mutant plus the new discard mutant; the 23-cfg constant migration; the
`MutReclaimIgnoresPub` → `run_clean` reclassification; no BASE drift (both
commits have the same tree SHA).

| # | Finding | Disposition |
|---|---|---|
| 1 | The classification still asserted liveness PASS: `formal-assets.tsv` said "liveness PASS on the complete space" and the round-1 disposition above marked the missing-receipt finding **fixed**, while the manifest itself scopes reviewer B's result to scratch evidence and the committed liveness log is partial (`STOPPED_ON_OWNER_REQUEST`) | **fixed here** — the model row now reads: formal safety PASS (receipt in `raw/tlc/`); formal liveness **PENDING**, no L1/L5 claim, reviewer B's run corroborating only. The round-1 disposition was split into its closed safety half and its open liveness half, and the missing-receipt finding is no longer marked fully resolved |
| 2 | The successor `formal-assets.tsv` had a 7-field row under an 8-column header (`RequestCoreLive.cfg`), and lumped 21 `Mut*` cfgs as negative mutants although two of them are the separately-listed subsumption witnesses | **fixed here** — every row is 8 fields; the mutant row is the 19 kill gates (16 invariant-killed + 3 temporal-killed) and says so, with both witnesses excluded from that count and cross-referenced to their own rows |
| 3 | "Which states were lost" claimed the lost state set was exactly the inflight family, which is false for the ghost-inclusive state: old `… → BeginPublish → CompletePublish → ReleaseBind` reached `pub = "done" ∧ bind = FALSE` with `consumed = discarded = FALSE`, which the converged model cannot (its releases always record the edge) | **fixed here** — `transition-diff.md` now separates the protocol-state projection (where exactly the inflight family is lost) from the full model state (which additionally loses the unrecorded-release ghost configuration), and states that the isolation control's counters establish the relation delta, not this state classification. The same assertion in the ledger received the same qualification |
| 4 | The final disposition still described the B1-1 record as unedited and "outside this slice's allowed change set", although `eb0a7350` had already marked it | **fixed here** — `review.md` (both places), `correspondence-after.md` and the ledger's #524 section now record it as fixed by `eb0a7350`, with the review-time objection kept as history |

The reviewer's own recommendation on the three open items is adopted:
keep `MutReclaimIgnoresPub` as a subsumption witness; complete the formal
liveness run before closing #524; add no C++ baseline build for this
model-only change.

`MODEL_STATUS` stays `MODEL_CONVERGED_PENDING_FULL_TLC`, and this file gains no
freeze clearance from round 2: the round-2 verdict was `REQUEST_CHANGES`, and
the outstanding liveness receipt is the item that keeps #524 open.
