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
were the same two deliverables, and both are addressed by commits
`docs(formal): update RequestCore correspondence` and
`docs(evidence): record #524 verification and review`; the re-verification of
each blocked item is recorded below.

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
| `docs/roadmap/b1-1-request-core-evidence.md:566,568,678` list the retired mutant and action as live | **recorded, not edited**: outside this slice's allowed change set; supersession recorded here, in `correspondence-after.md`, and in the ledger's dated #524 section |
| No full-run receipt for the converged model | **fixed** — complete safety and liveness runs are recorded in `resource-and-execution-manifest.md` and committed under `raw/tlc/` |

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
record stays untouched as an out-of-scope dated record with its supersession
stated.

## Residuals carried forward (not defects introduced by this slice)

1. `docs/roadmap/b1-1-request-core-evidence.md` rows `:137`, `:566`, `:678`
   are superseded by this convergence. Editing it is outside this slice's
   allowed change set; the owner should either re-type those rows as dated or
   fold them into the #524 record. Flagged in the final receipt.
2. Pre-existing abstractions, unchanged and unchanged in scope: `Reclaim`
   carries no observer-phase pin (ObserverCore owns it); the model has no
   zero-op accept action; the pre-claim physical outcome path is C++-only.
3. `formal-assets.tsv` in the frozen F6 directory is historical and was not
   overwritten; the successor classification is
   `formal-assets.tsv` in this directory.
