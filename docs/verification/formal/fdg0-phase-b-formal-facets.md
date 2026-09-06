# FDG-0 Phase B — Formal Facets

> Owner doc for the FDG-0 Phase B facet layer (issue #298; governing issue,
> "Phase B only — Formal Facets"). Established 2026-09-06 at master
> `a708884d` (PR #302 merge), branch `safety/fdg0-phase-b-formal-facets`.
>
> **Claim (exact wording, never stronger):** facets are EXPLICIT routing
> metadata that partition an existing formal claim's reopened-evidence
> ORDER. A PRECISE facet result means only "these are the formal artifacts
> to reopen FIRST". It does NOT mean the model is correct, stale, or must
> change, and it never changes a semantic disposition (always
> `UNDETERMINED`).

## Why facets exist

After #300 (SCIP structural substrate) and #302 (Xmake Build Truth), the
resolver routes at CLAIM level: any hit on claim F08 reopens all six F08
suites. Phase B tests one bounded question:

> Can broad claims be partitioned into evidence-backed internal facets so a
> trustworthy anchor/path hit reopens only the relevant formal evidence,
> without weakening conservative claim-level impact detection?

Permanent rule: **facet precision is OPTIONAL; claim-level conservatism is
REQUIRED.** A facet must never create a new false negative, never narrow a
COARSE/UNKNOWN/stale/unresolved answer, and never answer NO.

## What facets are NOT

- not new top-level claims (F01/F03/F04/F06/F08 remain the only claim ids;
  `union(facet.formal_suites) == claim.formal_suites` is mechanically
  validated — overlap allowed, loss and expansion forbidden);
- not TLA+ theorems or model-change evidence;
- not proof that C++ implements a model action (semantic labels are
  EXPLICIT registry metadata sourced from existing authorities, never a
  parser-verified claim);
- not a semantic disposition and not a permission to suppress
  UNKNOWN/fail-closed evidence.

## Registry schema (schema_version 2)

`spec/formal/anchors.json` remains the SOLE hand-maintained cross-domain
authority. Schema 2 adds:

```text
claim.cpp_anchors[i].id     optional stable anchor identity (unique within
                            the claim); present on all anchors of a
                            faceted claim
claim.facets[]              optional; each facet:
  id                          unique within the claim
  anchor_refs                 anchor ids contained in the facet
  formal_suites               subset of the parent claim's suites
  semantic_labels             EXPLICIT routing metadata: source authority,
                              trace_events, model_actions
  evidence / note             optional provenance
```

Schema 1 registries stay loadable; declaring facets under schema 1 is
invalid. Faceted claims: F04 (2 facets), F06 (2 facets), F08 (3 facets).
F01 and F03 are deliberately unfaceted (see the audit matrix).

Mechanically validated (`formal_impact.py validate_registry`): duplicate
anchor/facet ids, unknown anchor_ref, facet suite not in parent AND not in
the manifest, facet-suite union == parent (loss/expansion forbidden),
zero-anchor/zero-target facets, faceted anchor referenced by no facet,
facet evidence paths, and declared `trace_events` against the existing
e9 vocabulary authority (`e9_trace_validate.KNOWN_EVENTS`). Model-action
names are declared metadata only — no TLA parsing exists to make them
compiler-authoritative, and none was built.

## Reality audit matrix (preregistered; full citations in the prereg)

| claim | anchors | suites | decision | rationale |
|---|---|---|---|---|
| F01 | validate_ (gate, SHARED w/ F06), free_slot_locked_ | request-arena | NOT FACETED | single suite — nothing to partition |
| F03 | record_terminal, run_syscall, finalize_operation_terminal_ (gated) | request-arena, e10-waitnode, e13-select-core, e13-select-safety | NOT FACETED | all three anchors own the request-arena terminal face (S1A B03: arbitration + Layer-B producer obligations); the e10/e13 consumer faces have no registered anchor — a consumer facet would be zero-anchor (invalid) and a new anchor would expand authority without an evidence-backed mapping |
| F04 | reap, publish_from_reap | request-arena, f1-wait-record | FACETED ×2 | arena-reap-publication [reap] → request-arena + f1 (Race B overlap kept conservatively); completion-ready-release [publish_from_reap] → f1-wait-record (E7-T2; GenMC K1/K2 as evidence metadata — the SC request-arena model does not own the LP memory order) |
| F06 | cancel, validate_ (SHARED w/ F01), CancelToken::request, check_cancel | request-arena, cancel-token-epoch, e12-rwlock | FACETED ×2 | arena-cancel-arbitration [cancel, validate_] → request-arena + e12-rwlock; request-epoch-delivery [request, check_cancel] → cancel-token-epoch + e12-rwlock; e12-rwlock is the primitive-domain reconcile face of the same layered-cancellation claim with no dedicated anchor — the overlap is the conservative choice |
| F08 | signal_wake_locked, park_on_wake_source, run_impl, wake_epoch_ | e9-park-wake, e9-trace-conformance, spawn-wake-epoch, e8-suspend-switch, e12-async-condition, e12-rwlock-scheduler-liveness | FACETED ×3 | wake-publication [wake-signal, wake-epoch-state]; park-commit-return [park-boundary]; startup-population [startup-settlement]. Seeded by the e9 trace authority table, the R-F1 startup model boundary (#223), the #115 publication obligation, and the manifest notes of the two e12 downstream faces |

F08 facet target sets: wake {e9-park-wake, e9-trace-conformance,
spawn-wake-epoch, e12-async-condition, e12-rwlock-scheduler-liveness};
park {e9-park-wake, e9-trace-conformance, e8-suspend-switch}; startup
{e9-park-wake, e9-trace-conformance}. F08 is the PRIMARY pilot; F04 is
the independent second claim (F06 is a third, bonus).

## Routing semantics (task book §11–§16)

```text
PRECISE            build world verified; graph fresh; no unresolved /
                   traversal / global fail-closed risk; claim impacted via
                   symbol-level anchor reach (never coarse-only)
                     revalidation_targets = union of the affected facets'
                     suites (validated ⊆ parent)
                   reached anchors (task book §13/§14):
                     DIRECT     the triggering anchor id (changed symbol
                                IS a registered anchor)
                     STRUCTURAL the TERMINAL anchor(s) of the structural
                                path(s) — never intermediate helpers
                     facet set  = union of the facets containing them
CONSERVATIVE_ALL   COARSE file fallback; UNKNOWN; stale graph; stale build
                   world; unresolved anchor; frontier overflow; facet
                   mapping ambiguity; claims WITHOUT facets
                     revalidation_targets = full parent suite set;
                     candidate_facets retained as DIAGNOSTICS ONLY when a
                     demoted reach suggests them (never authoritative)
NONE               claim not impacted (no entry)
```

Exit contract, claim classification, and the provenance vocabulary
(BUILD/COMPILER/HEURISTIC/EXPLICIT/FALLBACK) are unchanged; the facet
mapping itself is EXPLICIT. `impact` text/JSON exposes per claim:
`facet_scope`, `reached_anchor_ids`, `affected_facets`,
`revalidation_targets`, optional `candidate_facets`. `explain` prints
anchor ids, containing facets, facet targets, semantic labels and the
EXPLICIT provenance.

## Adversarial corpus and measurement

Preregistration: `docs/results/formal/fdg0-phase-b-prereg.json` — frozen
and committed (9c291614) BEFORE implementation and before any routing
result existed. The final corpus run compared against it; the prereg file
was not rewritten. Machine-readable results:
`docs/results/formal/fdg0-phase-b.json` (driver:
`scripts/formal/fdg0_phase_b_eval.py`; B10/B12 hermetic in
`scripts/tests/test_formal_impact_facets.py`).

### Verdict metrics (issue #298 §8 / task book §22)

| metric | value |
|---|---|
| claim-level recall (T1–T10 depth 2, see regression) | unchanged vs #300 reference |
| silent NO | 0 |
| incorrectly narrowed COARSE/UNKNOWN | 0 (B8 6 targets, B9 6 targets + fail-closed, all conservative rows full parent sets) |
| F08 useful facets | YES (park 6→3, startup 6→2) |
| independent second useful claim | YES (F04 release face 2→1; F06 both faces 3→2) |
| **mean PRECISE B1–B7 fan-out reduction** | **33.33%** (threshold 30% — MET) |
| supplementary F04/F06 rows (raw, outside the frozen denominator) | 29.17% mean (S-04a 0% is the honest cost of the f1 Race-B overlap) |

Per-row PRECISE results: B1a 5/6 (wake), B1b 3/6 (park), B2 3/6
(helper→park only), B3 5/6 (rogue state writer → wake), B4 2/6 (startup),
B5-F06 2/3 (arena facet survives the shared-anchor row), B7 6/6 (park+we
union — the full claim; union is the honest answer for a two-anchor path).

### Preregistration comparison (10/16 rows match; deviations disclosed)

1. **B1a/B1b** — prereg predicted a both-direction anchor union (park+wake
   for both wake- and park-side edits); the implementation routes DIRECT
   hits by the TRIGGERING anchor only (task book §13 "record which exact
   anchor ID triggered the claim"), a semantics decision fixed during
   implementation after smoke-testing, before the corpus ran. Direction is
   toward MORE precision, never less; targets for B1a match the prereg
   number anyway (wake facet = 5 suites).
2. **B3** — class DIRECT vs prereg STRUCTURAL: the T9-shaped edit also
   inserts the hpp declaration, whose hunk attributes directly to a wake
   anchor. Facet outcome identical (wake-publication, 5 targets).
3. **B6b** — prereg predicted F03 only; the real graph carries
   finalize→record_terminal→validate_, so F01 (conservative) and F06
   (PRECISE arena facet via the identity-gate terminal) legitimately
   surface. The prereg under-predicted real reach; nothing was narrowed
   that was not anchor-backed.
4. **B7** — facet targets 6 vs prereg 5: prereg arithmetic error (park ∪
   wake facets = all six suites). The 0% reduction is the honest cost of
   the union rule for a two-anchor path.

Fixture repairs made DURING development, before the final corpus run (both
are #300-precedented fixture-bug classes, not result tuning): two-phase
specimens must query the post-phase-B world (querying a restored HEAD-world
graph against phase-A-numbered hunks fabricates DIRECT hits); helper
fixtures must be legal C++ (free function, or a member declaration placed
after `external_wake_possible_locked`'s inline body — a declaration
inserted elsewhere in scheduler.hpp folds into the preceding inline
member's refs under the documented nearest-preceding heuristic and
fabricates structural paths).

## Maintenance cost (issue #298 §23)

| item | before | after |
|---|---|---|
| anchors.json LOC | 231 | 341 |
| anchors with ids | 0 | 10 (all anchors of faceted claims) |
| facets | 0 | 7 |
| facet→anchor refs | 0 | 10 |
| facet→suite mappings | 0 | 17 |
| semantic_labels blocks | 0 | 7 |
| resolver LOC (formal_impact.py) | 1812 | 2153 (+341, incl. docs/validation) |
| new test LOC | — | 589 (26 hermetic tests) |
| new corpus driver LOC | — | 622 |

The review-noise reduction costs ~110 registry lines and 7 hand-maintained
facets, each carrying an explicit authority citation. The union invariant
makes drift LOUD: adding a suite to a claim without placing it in a facet
(or dropping one from a facet) fails `check`/`index` structurally.

## Regression evidence (actual runs, this branch)

- `python3 -m unittest scripts.tests.test_formal_impact
  scripts.tests.test_formal_impact_facets scripts.tests.test_build_truth`
  → 101 tests OK (26 new).
- `formal_impact.py index` + `check` at HEAD → all 16 anchors resolve,
  build world verified.
- `ftlr0_eval.py run` (T1–T10) and `fdg0_phase_a_eval.py run` (A-specimens)
  → see docs/results/formal/*.json (regenerated on this branch).
- `verify.py check` → PASS. `bash scripts/gates/pre-push.sh` → see PR body.

## Residuals

1. B7/S-04a show 0% reduction: two-anchor unions and the deliberately
   conservative f1 overlap bound the achievable precision. Facet precision
   is bounded by real call-graph connectivity — the wake source is consumed
   by everything, so wake-side edits stay broad. That is a property of the
   authority, not a defect of the mechanism.
2. e12-rwlock sits in BOTH F06 facets (no dedicated anchor for the
   primitive reconcile face). A future bounded change could register that
   anchor and split it out; Phase B did not expand authority.
3. The nearest-preceding attribution heuristic makes header-inserted
   declarations fold into preceding inline members' refs (documented above);
   future fixture/registry work near scheduler.hpp must account for it.
4. Semantic disposition remains UNDETERMINED everywhere; facet ordering
   says nothing about whether a model must change (Phase C / later work).

STOP: Phase C (historical gold corpus, AST semantic fingerprint, CodeQL,
semantic refresh workflow, pre-push/CI enforcement) is NOT started and is
not authorized by this document.
