# #524 correspondence-before — reconstructing the current protocol

Baseline (BASE_SHA): `9a501d1c8839c2f1375d48937c9bf4df18b93b26`.
Model under reconstruction: `formal/tla/RequestCore.tla` at BASE.
C++ authority: `include/sluice/async/detail/request_core.hpp`,
`src/async/detail/request_core.cpp`, `include/sluice/async/request.hpp`.

This record is the R1 gate: no model edge is removed until the correspondence
is established. It answers, in order, the six questions the issue poses.

## 1. The four candidate rows

| TLA+ action | Current C++ owner | Linearization point | Decision |
|---|---|---|---|
| `Consume` | `RequestCore::consume_public_result` | `src/async/detail/request_core.cpp:315-336` under the core mutex | KEEP |
| `DiscardPublic` | `RequestCore::discard_public_result` | `src/async/detail/request_core.cpp:269-286` under the core mutex | KEEP |
| `ReleaseBind` | **none** — the pre-F2 caller-owned `Completion` release edge | — | REMOVE |
| `Reclaim` | `RequestCore::reclaimable_` / `try_reclaim_` / `release_slot_` | `src/async/detail/request_core.cpp:566-604` | KEEP |

Reachability of each public API into those owners:

- `Request::take_result()` → `consume_public_result` (`request.hpp:119-120`).
- `Request::discard()`, the destructor and move-assign-over →
  `release_responsibility_` → `discard_public_result` (`request.hpp:72-82`,
  `133`, `148-172`).
- `Reclaim` is not an API: it is the synchronous zero-gap reclaim inside the
  releasing calls, guarded by `reclaimable_`.

## 2. Every write of the public binding, and what guards it

`slot.binding_live` is written in exactly six places:

| Site | Function | Reached from | Guard |
|---|---|---|---|
| `request_core.cpp:160` | `reserve` | admission of a free slot | slot was already fully released (`release_slot_` cleared the binding) |
| `request_core.cpp:191` | `accept` | submit commit | installs the binding |
| `request_core.cpp:283` | `discard_public_result` | `Request::discard` / destructor / move-assign-over | **requires `published`** (`:278-282`) |
| `request_core.cpp:310` | `observe_public_result` | `try_result` | only under `SLUICE_B2_MUTANT_OBSERVE_CONSUMES`; still behind the `published` gate |
| `request_core.cpp:332` | `consume_public_result` | `Request::take_result` | **requires `published`** (`:322-324`) |
| `request_core.cpp:583` | `release_slot_` | `rollback` (reserved slot) and `try_reclaim_` via `reclaimable_` | reclaim already requires `published && !binding_live`; rollback is on a `reserved` slot |

So the only two release edges that clear a *live* public binding both require
`slot->published == true`. There is no third one.

## 3. Is a release before publication completion allowed? No.

The question the issue raises is whether the current protocol permits releasing
the public binding while publication is still in flight (the window
`publication_inflight && !published`).

- `discard_public_result` reads `published` only (`:278`) and returns
  `BindingRelease::not_visible_yet` for an inflight publication (`:280-281`).
  Its only production caller, `Request::release_responsibility_`, turns that
  arm into a violation: `detail::request_nonterminal_release_fail_fast()`
  (`request.hpp:162-165`), i.e. the always-on nonterminal-release diagnostic.
- `consume_public_result` returns `PublicConsumption::pending` (`:322-324`).
- `begin_publication` cannot even start without `binding_live` (`:440`), so
  `publication_inflight` implies the binding is live; nothing releases it
  while inflight, and `reclaimable_` additionally refuses `publication_inflight`.

The window is therefore *diagnosed*, not legal. It is exercised by
`tests/public_request_release_violation_test.cpp`
(`discard_request_during_publication_inflight`) and by the
`SLUICE_B2_MUTANT_DISCARD_ACCEPTS_INFLIGHT` mutant, and the model already
gives it the negative oracle `MutDiscardInflight` (killed by
`InvDiscardAfterPublish`).

This is the decisive finding for `ReleaseBind`: its distinguishing guard is
`pub[s] # "none"`, i.e. it is legal *from `inflight`*, which the current C++
refuses. Its `pub = "done"` branch does nothing `DiscardPublic` does not
already do (see §5).

History agrees: the pre-F2 `Completion` release edge called
`release_public_binding`, whose guard was `terminal_chosen &&
execution_refs == 0 && (publication_inflight || published)` — the model's
`ReleaseBind` guard verbatim. F2 deleted `Completion`
(`docs/evidence/f2-authority-contraction-20261013/execution-record.md:59`:
"accepted releases during `publication_inflight` … Retired"), and the
remaining `release_public_binding` was deleted by `3bdc61d7`
("refactor(core)!: leave one release edge for the public binding").

## 4. Do `Consume`/`DiscardPublic` cover every legal release? Yes.

Model guards: both require `phase = "accepted"`, `bind[s]`, `term[s] # "none"`
and `pub[s] = "done"`. The C++ release edges require `phase accepted`,
`binding_live`, and `published`; and `published` implies `terminal_chosen`
(`begin_publication` requires it at `:440`). The guards are the same predicate
on the reachable state set, so the two actions are complete for the current
protocol.

The deliberate over-approximations live only in mutants
(`MutConsumeUnpublished`, `MutDiscardInflight`), which is where they belong.

No counterexample was found, so the issue's stop condition ("if a legal public
release behavior cannot be represented by Consume/DiscardPublic, stop and give
the counterexample") does not trigger.

## 5. Behaviour inclusion of the deleted edge

`ReleaseBind` produced two classes of released state:

- `pub = "done"`: `DiscardPublic` produces the same state. The only delta is
  the ghost (`discarded[i]` set instead of both ghosts clear), which is *added*
  information — it records which public call released. No current behavior is
  lost.
- `pub = "inflight"`: not reproducible by any surviving action. Independently
  confirmed by reviewer A with a witness invariant: the state
  `stage = 3 ∧ pub = "inflight" ∧ consumed = discarded = FALSE ∧ pubDoneIds = {}`
  is reachable at BASE (`Invariant NotWInflightThenReleased is violated`,
  1659 states, trace `Accept → CancelRequest → RetireExec → BeginPublish →
  ReleaseBind`) and unreachable in the converged model.

That is the intended contraction: the removed states are exactly the pre-F2
caller-owned release states, which have no current owner.

## 6. Fairness, and the L1/L5 premises

`Fair` (BASE `RequestCore.tla:496-502`) is
`WF_vars` over `ClaimExecAny`, `PhysicalOutcomeAny`, `RetireExecAny`,
`BeginPublishAny`, `CompletePublishAny`, `ReclaimAny`. `ReleaseBind` carries
**no fairness conjunct** — the fact the F6 closeout already relied on when it
classified the model as a safety-and-liveness-conservative superset. Removing
a disjunct of `Next` cannot change any fair action's guard, and for every state
where `ReleaseBind` was enabled some fair action remains enabled
(`pub = "inflight"` → `CompletePublish`; `pub = "done"` → `Consume`), so no
state loses its last successor and no obligation becomes unreachable.

L1 (`accepted ~> published`) and L5 (`reclaimable ~> reclaimed`) are quantified
by full request identity; their antecedents stay reachable after the removal
(both are exercised in the converged model — see `transition-diff.md`). The
properties themselves are unchanged, and are re-checked in full (not inferred
from the smaller state space) — see `safety-preflight.md` and the complete
`RequestCoreLive.cfg` run recorded in `resource-and-execution-manifest.md`.

## 7. Implicit dependencies of the deleted edge (the sweep)

Deleting an action is only safe if nothing else leans on it. Every
RequestCore cfg at BASE was run, and each violating trace was searched for
`ReleaseBind`:

| cfg | verdict on the BASE model | `ReleaseBind` in the violating trace |
|---|---|---|
| `RequestCoreMutReleaseResolvable` | killed on `InvReleasedNotLive` | **yes** (the mutated edge) |
| `RequestCoreMutReclaimIgnoresPub` | killed on `InvReclaimConditions` | **yes** (the enabling edge) |
| the other 19 mutant cfgs | their named kills | no |
| `RequestCore`, `RequestCoreLive`, `MutReadyBeforePayload` | clean | n/a |

The `MutReclaimIgnoresPub` trace is
`Reserve → Accept → CancelRequest → RetireExec → BeginPublish → ReleaseBind → Reclaim`,
and the violating reclaim record carries `pub = "inflight"`. Removing
`ReleaseBind` removes the only state in which a released binding has
`pub ≠ "done"`, so the guard that mutant drops becomes implied by the
surviving guard `~bind[s]`, and the mutant can no longer be killed.

Consequence for the oracle migration (executed in
`formal(test): migrate retired binding-release mutant`): `MutReleaseResolvable`
retires with the edge it mutated, and `MutReclaimIgnoresPub` is reclassified
from a killed mutant to a **subsumption witness** whose cfg must complete
cleanly, with the subsuming fact named by the new invariant
`InvBindImpliesPublished` (`phase[s] = "accepted" ∧ ~bind[s] ⇒ pub[s] = "done"`).
That witness keeps its teeth: reviewer B showed that under
`MutDiscardInflight = TRUE` (an inflight-tolerant release edge present again)
the same cfg reports `Invariant InvBindImpliesPublished is violated`, and with
the new invariant removed from the list it reports `InvReclaimConditions` again.

## 8. Verdict

Correspondence holds: `ReleaseBind` is exclusively the retired pre-F2
`Completion` release flavor, both surviving release edges are modeled exactly,
and no legal current release behavior is unrepresented. The edge may be
removed. Reviewer A independently reached the same conclusion (its
`REQUEST_CHANGES` verdict concerned the still-pending documentation and run
receipts, not the correspondence).
