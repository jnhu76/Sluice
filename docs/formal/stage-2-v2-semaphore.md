# Stage 2V2.3 — Semaphore core, replayed on the split return window

Campaign: FCB1-METHOD-CORRECTIVE-1 (Issue #375). Calculus: Stage-0-V2.3
(`formal/Sluice/Formal/CalcV2.lean`, `Caller := fiber | ext`, three-phase
external calls, per-stage `extCap`/`extRun` census, and the V2.3 split of a
fiber call's critical section (`fiberEffect`) from its physical return
(`fiberDone`)). Judgment layer: `JudgeV2.lean`. Chain: Stage-0 V2.3 →
Event replay (#377 line) → this semaphore replay → method review.

## 1. Scope and subject

Counting semaphore: `include/sluice/async/semaphore.hpp`,
`src/async/scheduler_semaphore.cpp`. Core surface modeled: `acquire`
(void) and `release` (bool: whether a permit was granted). `try_acquire`
is a non-suspending probe; the deadline entry point (`acquire_until`) and
`cancel` are extensions outside the core model, exactly as Event's
`initially_set` constructor was out of scope at Stage 1
(`event.hpp:16-17`).

## 2. Call-domain census (`extCap`/`extRun`)

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

## 3. The two interleaving windows and the parameterized claim

Window A (external effect/return): an external `release` applies its grant
at `extEffect` but its completion observation lands only at `extDone`, and
other steps legitimately interleave in between (`global_mtx_`
serialization is not physical-return serialization; the Stage-1 `event_set`
ordering applies unchanged). An acquire may complete while the release
that minted its permit is still in flight.

Window B (fiber effect/return, the V2.3 seam): the C++ `LockGuard`
destructor releases `global_mtx_` at the end of a fiber call's critical
section (`scheduler_semaphore.cpp:147-161`) *before* the fiber physically
returns, so an external caller's whole `release` may serialize inside a
fiber's return window. `fiberEffect` fixes the result and applies the
state effect while the fiber keeps the worker baton; `extApply`,
`extEffect`, `extDone`, and the fiber's own `fiberDone` are exactly the
steps legal in that window. `fiberWindowTrace` is the witness trace
`i(F release), i(E release), c(E release = false), c(F release = true)`:
the fiber's section stores the last permit, the external release is
refused at the ceiling and returns first, and only then does the fiber
physically return. A step relation that fuses a fiber's critical section
with its return cannot emit that trace; `semPrim_possesses_fiberWindow`
proves the split calculus possesses it.

The guarantee, restated over both windows and parameterized over the
constructor:

* **`semPermitsHonoredGen initial`**: at every completed acquire, the
  number of completed takes is at most the number of release *issues*
  observed so far plus the initial permits — every consumed permit was
  minted by a release whose call had already been issued, or drawn from
  the constructor's stock.
* **`semPrimOf_guarantees`**: `Guarantees (semPrimOf initial max)
  (semPermitsHonoredGen initial)` for every `initial max : Nat`. The
  ceiling `max` is immaterial to the claim, exactly as predicted; the
  modeled default instance is `semPrim = semPrimOf 0 1`.
* The paused strict guarantee "completed takes < completed grants" is
  false for the primitive itself; `windowTrace` refutes it
  (`semPrim_possesses_window`, `window_not_strict`).

The mirror behind it: each release issue is credited at its observation —
at the fiber dispatch (`semPendRel`) or at the external entry
(`semPendExt`, records with `result = none`) — and the credit is
discharged when the release's section takes effect, whether the outcome
is a stored permit, a handoff to a parked acquirer, or a refusal that
discards it. In the V2.3 window the credit lives on the fiber's
`returning` slot until `fiberDone` retires it together with the take or
grant it enabled; a resumed acquire may re-suspend (`runPark` is
unguarded on the resumed bit), so the mirror is an inequality in the safe
direction.

## 4. What is proved (`formal/Sluice/Formal/SemV2.lean`)

* `semBalance_mirror` — the permit-accounting mirror, by induction over
  the `PrimStep2` constructors (now including `fiberEffect`/`fiberDone`),
  carrying `hstale` (published runq entries are acquires), `hparked`
  (parked calls are acquires), and `hexts` (in-flight external records
  are releases), parameterized over `semPrimOf initial max`.
* `semStep_take_credit` — an acquire-completing step leaves at least one
  permit's worth of accounting behind (inline: the consumed permit rides
  the `returning` slot; resumed: a handoff-delivered permit is held).
* `semRuns_split` — every run splits at an arbitrary observation cut.
* `semPrimOf_guarantees` — the parameterized guarantee, by splitting the
  run at the completing acquire and applying the mirror from `primInit`
  (whose `available = initial` by construction).
* Possession batteries (non-vacuity and tightness):
  `semPrim_possesses_extRel` (three-phase external release),
  `semPrim_possesses_handoff` (park, handoff to the queue head, resumed
  completion — one issue, one take: the bound is achieved; handoff runs
  through `fiberEffect` + `fiberDone` with the permit published between
  them), `semPrim_possesses_window` (external window, strict claim
  refuted), `semPrim_possesses_fiberWindow` (fiber return window, the
  V2.3 seam), `semPrimOf_possesses_initialTake` (`initial = 1, max = 2`:
  a take drawn purely from constructor stock, zero release issues),
  `semPrimOf_possesses_ceiling` (`initial = 0, max = 2`: two stored
  grants, third release refused — three issues, two grants).
* `semMutant_not_guarantees` — a mutant whose `acquire` succeeds without
  a permit violates `semPermitsHonored`; the guarantee has teeth.

Axiom audit (`scripts/verify_formal.sh`): all of the above —
`semPrimOf_guarantees`, `semBalance_mirror`, `semRuns_split`,
`semStep_take_credit`, the six possessions, the tightness witnesses, and
`semMutant_not_guarantees` — depend on exactly `[propext, Quot.sound]`.

## 5. Disposition of the THEOREM-B question

The semaphore core's reducibility to the bare substrate is **not
adjudicated** here, in either direction. What is recorded:

* `release` is one atomic check-then-act section in the C++; a fiber-wise
  encoding must serialize it with wait-queue cells, and the discipline
  lets a second release's issue interleave between the first release's
  issue and its completion — the counter-evidence shape is the same as
  Event's Stage-1 blockade, so no "reducible" verdict may be claimed
  without an encoding analysis.
* No universal non-reducibility is proved either; per the campaign's
  wording discipline the THEOREM-B question is **RESEARCH/DEFER**,
  recorded on the charter issue (#375). The campaign stays OPEN.

## 6. Redelivered from the paused branch

From `formal/fcb1-stage-2-paused` (`23e380e2`), kept: the two-call core
surface, `SemState`, `semRun`'s section shapes, `semRun_none`,
`semRun_pndMap_nil`, `semRuns_split`. Rebuilt for V2.3: `semPrimOf`
parameterizes the constructor (`initial`, `max`); the mirror, credit, and
guarantee are restated over the split steps; the fiber-window,
initial-stock, and ceiling batteries are new. No Stage-3 material (async
mutex/lock-guard surfaces) is present on this branch.

## 7. Gates

| gate | result |
|------|--------|
| `lake build` (Lean 4.33.1, formal/Sluice.lean incl. SemV2) | PASS |
| `./scripts/verify_formal.sh` (build + sorry/admit scan + axiom audit ⊆ {propext, Quot.sound}) | PASS |
| `./scripts/verify_tla.sh` (TLC EventCore + mutant, unchanged from Stage 1) | PASS |

## 8. Downstream obligations

* **`SemCore.tla` is a merge gate for this PR's final merge** (human
  review decision, #378 verdict): a TLA+ mirror of the semaphore core on
  the V2.3 action set, with TLC coverage by action class — two fiber
  callers and two external callers, `initial ∈ {0, 1}`, `max ∈ {1, 2}`,
  and paths for stored permit, FIFO handoff, full/refused release,
  external effect-before-return, fiber effect-before-return, and
  cross-window interleavings — plus mutants for permit creation/loss,
  double consumption, FIFO bypass, wrong results, and fused
  effect/return granularity. It is deliberately *after* the V2.3 replay:
  mirroring the wrong action granularity would have copied the seam into
  the model.
* The THEOREM-B question (semaphore reducibility, either direction) is
  recorded RESEARCH/DEFER on #375; a per-encoding counter-evidence
  battery (the semaphore analogue of Stage 1's `encChained` blockade) is
  the natural next step and is not attempted here.
* `try_acquire`/`cancel`/`acquire_until` enter the model only if a stage
  needs them; their census rows above are the authority until then.
