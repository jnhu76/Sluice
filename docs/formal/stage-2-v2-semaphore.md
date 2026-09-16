# Stage 2V2 — Semaphore core, re-adjudicated on the execution domains

Campaign: FCB1-METHOD-CORRECTIVE-1 (Issue #375). Calculus: Stage-0-V2.2
(`formal/Sluice/Formal/CalcV2.lean`, `Caller := fiber | ext`, three-phase
external calls, per-stage `extCap`/`extRun` census). Judgment layer:
`JudgeV2.lean`. This stage restores and completes the paused Stage-2 work
(`formal/fcb1-stage-2-paused` at `23e380e2`) on the V2.2 foundations, with
the paused claims restated to survive the external-call window.

## 1. Scope and subject

Counting semaphore: `include/sluice/async/semaphore.hpp`,
`src/async/scheduler_semaphore.cpp`. Core surface modeled: `acquire`
(void) and `release` (bool: whether a permit was granted). `try_acquire`
is a non-suspending probe; the deadline entry point (`acquire_until`) and
`cancel` are extensions outside the core model, exactly as Event's
`initially_set` constructor was out of scope at Stage 1
(`event.hpp:16-17`).

## 2. Call-domain census (v2.2 `extCap`/`extRun`)

| entry           | code                          | worker read            | domain           | model                              |
|-----------------|-------------------------------|------------------------|------------------|------------------------------------|
| `acquire`       | scheduler_semaphore.cpp:34-71 | `g_worker` :36-37, `commit_suspend_locked` :61, `context_switch` :67-71 | fiber-bound | `extCap = false`, `extRun = none` |
| `release`       | scheduler_semaphore.cpp:147-161 | none (`global_mtx_` :149 only) | external-capable | `extCap = true`, `extRun = semRun` |
| `try_acquire`   | scheduler_semaphore.cpp:21-32 | none                   | external-capable | extension, outside the core surface |
| `cancel`        | scheduler_semaphore.cpp:134-145 | none                   | external-capable | extension, outside the core surface |
| `acquire_until` | scheduler_semaphore.cpp:73-132 | `g_worker` :75-76      | fiber-bound      | extension, outside the core surface |

The modeled `semRun` mirrors the C++ section shapes:
`acquire`'s inline fast path is `node.prev_ == nullptr ∧ available > 0`
(:46-54); `release` is wake-the-head (:151-153), else store a permit below
the ceiling (:155-159), else refuse (:156-158).

## 3. The v2.2 window and the restated claims

An external `release` applies its grant at `extEffect` but its completion
observation lands only at `extDone`, and other steps legitimately
interleave in between (`global_mtx_` serialization is not physical-return
serialization; the rule-10 ordering proven at Stage 1 for `event_set`
applies here unchanged). An acquire may therefore complete while the
release that minted its permit is still in flight.

Consequences, both handled head-on in `SemV2.lean`:

* The paused permit ledger (`available + grants = takes + available' +
  resumed`) is not step-wise conserved in that window. The restored
  `semBalance_mirror` credits each release *issue* at its observation —
  at the fiber dispatch (`semPendRel`) or at the external entry
  (`semPendExt`, records with `result = none`) — and discharges the
  credit when the release's section takes effect. Refused releases
  discard their credit, and a resumed acquire may re-suspend
  (`runPark` is unguarded on the resumed bit), so the mirror is an
  inequality in the safe direction.
* The paused strict guarantee "completed takes < completed grants" is
  false for the primitive itself. `windowTrace` possesses the violating
  trace (external release grants, an inline acquire completes, then the
  external release returns), and `semPrim_possesses_window` proves the
  primitive possesses it with the strict claim refuted
  (`window_not_strict`). The honest guarantee is
  `semPermitsHonored`: at every completed acquire, the number of
  completed takes is at most the number of release *issues* observed so
  far — every consumed permit was minted by a release whose call had
  already been issued.

## 4. What is proved (`formal/Sluice/Formal/SemV2.lean`)

* `semBalance_mirror` — the permit-accounting mirror, by induction over
  all 11 `PrimStep2` constructors, carrying `hstale` (published runq
  entries are acquires), `hparked` (parked calls are acquires), and
  `hexts` (in-flight external records are releases).
* `semStep_take_credit` — an acquire-completing step leaves at least one
  permit's worth of accounting behind (inline: a permit was available;
  resumed: a handoff-delivered permit is held).
* `semPrim_guarantees` — `Guarantees semPrim semPermitsHonored`, by
  splitting the run at the cut (`semRuns_split`) and applying the mirror
  from `primInit`.
* Possession batteries (non-vacuity and tightness):
  `semPrim_possesses_extRel` (three-phase external release), 
  `semPrim_possesses_handoff` (park, handoff to the queue head, resumed
  completion — one issue, one take: the bound is achieved),
  `semPrim_possesses_window` (the v2.2 window, refuting the strict
  claim).
* `semMutant_not_guarantees` — a mutant whose `acquire` succeeds without
  a permit violates `semPermitsHonored`; the guarantee has teeth.

Axiom audit (scripts/verify_formal.sh): `semPrim_guarantees`,
`semBalance_mirror`, `semStep_take_credit`, the three possessions, and
`semMutant_not_guarantees` depend on exactly `[propext, Quot.sound]`.

## 5. Disposition of the THEOREM-B question

The semaphore core's reducibility to the bare substrate is **not
adjudicated** here, in either direction. What is recorded:

* `release` is one atomic check-then-act section in the C++; a fiber-wise
  encoding must serialize it with wait-queue cells, and the v2.2
  discipline lets a second release's issue interleave between the first
  release's issue and its completion — the counter-evidence shape is the
  same as Event's Stage-1 blockade, so no "reducible" verdict may be
  claimed without an encoding analysis.
* No universal non-reducibility is proved either; per the campaign's
  wording discipline the THEOREM-B question is **RESEARCH/DEFER**,
  recorded on the charter issue (#375). The campaign stays OPEN.

## 6. Redelivered from the paused branch

From `formal/fcb1-stage-2-paused` (`23e380e2`), kept: the two-call core
surface, `SemState`, `semRun`'s section shapes, `semRun_none`,
`semRun_pndMap_nil`, `semRuns_split`. Rebuilt for V2.2: `semPrim` gains
the census fields; the ledger and guarantee are restated as above; the
window refutation, the tightness witnesses, and the mutant are new.
No Stage-3 material (async mutex/lock-guard surfaces) is present on this
branch.

## 7. Gates

| gate | result |
|------|--------|
| `lake build` (Lean 4.33.1, formal/Sluice.lean incl. SemV2) | PASS |
| `./scripts/verify_formal.sh` (build + sorry/admit scan + axiom audit ⊆ {propext, Quot.sound}) | PASS |
| `./scripts/verify_tla.sh` (TLC EventCore + mutant, unchanged from Stage 1V2.2) | PASS |

No TLA+ mirror was built for the semaphore in this stage; the balance is
inductively proved in Lean over the calculus that the Stage-1 TLC model
already mirrors. A `SemCore.tla` mirror with the ledger as an invariant
is a follow-up obligation if the human review asks for model-checked
depth.

## 8. Downstream obligations

* The THEOREM-B question (semaphore reducibility, either direction) is
  recorded RESEARCH/DEFER on #375; a per-encoding counter-evidence
  battery (the semaphore analogue of Stage 1's `encChained` blockade) is
  the natural next step and is not attempted here.
* `try_acquire`/`cancel`/`acquire_until` enter the model only if a stage
  needs them; their census rows above are the authority until then.
