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
discards it. In the V2.3 window that discharge happens at
`fiberEffect` itself — the section has run — and what rides the
`returning` slot is a completed inline acquire's held permit, which
`fiberDone` retires together with its take observation. A resumed
acquire may re-suspend (`runPark` is unguarded on the resumed bit),
evaporating its handed-off permit from the accounted pool — an
over-approximation in the C++'s favor that keeps the mirror an
inequality in the safe direction.

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

## 7. The TLA mirror (`formal/tla/SemCore.tla`)

A TLC-executable mirror of the same action set, written from this card's
census and the V2.3 Lean machine (not copied from any pre-reset model).
Constants: `Initial`, `Max` under `ASSUME Max > 0 /\ 0 <= Initial <= Max`
(the constructor domain); fixed caller sets `Fibers = {"f0","f1"}`,
`Exts = {"e0","e1"}` (Lean's `nextFiber` minting is dropped: a caller
re-enters once its previous call is no longer in flight — not queued,
parked, or holding the slot); observation fuel `MaxHistory`. `Spec == Init /\ [][Next]_vars` — **no fairness is assumed
anywhere**; every claim below is safety.

### 7.1 Action crosswalk (Lean `PrimStep2` ↔ TLA)

| Lean constructor (`CalcV2.lean`) | SemCore.tla action(s) | observable |
|----------------------------------|------------------------|------------|
| `submit` | `FiberSubmit(f,c)` | silent |
| `dispatchFresh` | `FiberDispatch` | fiber issue; requires the baton free |
| `fiberEffect` (`P.run` fast-path take) | `AcqTakeFast` | silent; `available -= 1`, slot → returning **with the baton** |
| `fiberEffect` (release: wake head) | `FibRelHandoff` | silent; publishes the queue head to `runq`, slot → returning |
| `fiberEffect` (release: store) | `FibRelStore` | silent; `available += 1`, slot → returning |
| `fiberEffect` (release: refuse) | `FibRelRefuse` | silent; result `f`, no grant |
| `fiberDone` | `FiberDone` | fiber completion; baton freed |
| `runPark` (`P.run` = none) | `AcqPark` | silent; appends to `waitq`, slot freed with the suspension |
| `dispatchResumed` | `FiberResume` | silent dispatch of a published acquire |
| `finishDone` | `FinishResumed` | resumed acquire's completion (effect and return do not come apart) |
| `extApply` | `ExtIssue(x)` | external issue; legal while a fiber holds the baton |
| `extEffect` (`semRun` branches) | `ExtRelHandoff(x)` / `ExtRelStore(x)` / `ExtRelRefuse(x)` | silent state effects |
| `extDone` | `ExtDone(x)` | external completion |
| `envTime` / `expire` | omitted | the semaphore's `onTick` is the identity and it has no timers |

The worker-slot encoding: Lean `FSlot.running d b` ↔ `cur` with
`phase = "run"` (`resumed` ↔ `b`); Lean `FSlot.returning d r` ↔ `cur`
with `phase = "ret"` (`result` ↔ `r`). The critical V2.3 property — a
returning fiber still holds the baton — is `FiberDispatch` /
`FiberResume` requiring `cur = NoCur`: **no second fiber dispatches while
any fiber sits in `phase = "ret"`**, while `ExtIssue`/`ExtEffect`/
`ExtDone` and `FiberDone` itself are legal in that window. `AcqTakeFast`
takes only with `waitq` empty (`sem_acquire` :46 checks
`node.prev_ == nullptr` first); `FibRelHandoff` wakes exactly
`Head(waitq)` (:151-153); `FibRelStore` stores only under the ceiling
(:155-159); `FibRelRefuse` is the `cur >= max_permits` branch (:156-158).

### 7.2 Safety invariants

| TLA invariant | content | Lean analogue |
|---------------|---------|---------------|
| `TypeOK` | every variable in its state domain | type discipline |
| `InvCapacity` | `0 <= available <= Max` | `SemState` well-formedness |
| `InvPermitPool` | `available + Holders + TakesCount(history) = Initial + granted` (exact conservation) | `semBalance_mirror`, strengthened — see below |
| `InvTakeBound` | completed takes `<=` release issues `+ Initial` | `semPermitsHonoredGen` |
| `InvQueueNoDup` | no fiber parked twice; no fiber queued twice in `runq` | mirror hygiene |
| `InvQueueOwnership` | a queued/parked fiber is never the dispatched one; published entries are acquires | `hstale` + slot disjointness |
| `InvFifo` | ghost pair `lastHead = lastChosen` (updated only by handoff actions) | the :151 wake-the-head shape |
| `InvCompDiscipline` | every completion has a same-caller same-call prior issue with no intervening completion | no completion before issue, no double completion |
| `InvReleaseResult` | the public release bool agrees with the effect branch that ran: handoff/store ⇒ `true`, refusal ⇒ `false` — checked on completed observations **and** on effected-but-not-returned calls (fiber return window and external "eff" records) | `semRun`'s structural coupling — see below |

**`InvPermitPool` is an equality by disclosure, not by drift.** The Lean
calculus over-approximates: `runPark` is unguarded on the resumed bit, so
a resumed acquire may re-suspend and evaporate its handed-off permit from
the accounted pool — that is why `semBalance_mirror` is an inequality in
the safe direction (§3). The TLA machine omits that over-approximation
because the C++ has it nowhere to occur: after `context_switch`
(scheduler_semaphore.cpp:67-71) the resumed `sem_acquire` returns
directly — there is no re-check loop — and `FinishResumed` never parks.
The equality is what gives the mutant battery its teeth: permit creation
and permit loss die on exact conservation, not merely on the loose bound.

**`InvReleaseResult` is the release result-semantics corrective.** The
public bool a `release()` returns (`relRet`, scheduler_semaphore.cpp:147-161)
must agree with the effect branch that actually ran: a handoff or a store
returns `true`, a ceiling refusal returns `false`. The Lean side fixes this
structurally — `semRun` returns `relRet true` exactly on the handoff and
store branches and `relRet false` on the refusal branch, so branch and
result cannot come apart by construction. The TLA side must not take that
coupling on faith: it now carries an independent effect authority in the
state and checks the agreement with `InvReleaseResult`. The authority is
written by each effect action **from its own branch, never from the result
bool**:

* fiber effects stamp the ghost `fibKind` (`"handoff"`/`"stored"`/
  `"refused"`) on the fiber's return-window slot;
* external effects stamp `kind` on the effected external record;
* completions carry the effect's `kind` on their history comp record (so
  the fused model, whose release completions are the only release
  observations, is covered too).

`InvReleaseResult` then requires, in all three places and for every
release completion in `history`, that `kind = handoff/store` implies
`result = "t"` and `kind = refused` implies `result = "f"`. Because the
kind is state-carried, the model now distinguishes executions the old
state merged (an external "eff" record or a release comp with result `t`
no longer collapses "stored" and "handoff" origins once the queue has
moved on) — the reachable-state counts in §7.6 grow by a few percent
versus the pre-corrective runs, which is this refinement, not a
behavioral change: every old reachable state is the projection of a new
one, so nothing previously verified is lost. The new property does not
reach into any coverage ghost — it depends on `cur`/`fibKind`, the ext
records, and `history` only — so coverage certificates cannot
accidentally buy a clean result contract.

### 7.3 The seam witness and the fusion separation

`InvWitness == wseq # 5` is the TLA form of `semPrim_possesses_fiberWindow`
(window B, §3), carried by a four-variable ghost machine (`wseq`, `wf`,
`wx`, `wlive`) over one call instance:

1. a fiber release's section **stores** the last permit (`FibRelStore`,
   `wseq 0 → 1`, records `wf`, sets `wlive`);
2. an external release **enters** after it (`ExtIssue`, `1 → 2`);
3. the external is **refused** at the ceiling the fiber had just filled
   (`ExtRelRefuse`, `2 → 3`);
4. the external **physically returns** (`ExtDone`, `3 → 4`);
5. only then the fiber itself **physically returns** (`FiberDone` with
   `WitnessStep5`, `4 → 5`).

`wlive` anchors steps 1 and 5 to the same call instance: `wf`
re-submitting or physically returning early kills the recording, so a
later call of the same fiber cannot fake the window across calls. Under
`MutFusedReturn` the fused store completes the call in one transition
(`wlive` goes FALSE immediately) and no `FiberDone` of a returning
release exists, so `wseq = 5` is unreachable. **TLC independently
rediscovered the V2.3 seam**: the correct model violates `InvWitness`
(the counterexample is exactly the window-B trace), and the fused model
satisfies it over its whole reachable state space — the hard criterion
"the model must distinguish `FiberEffect` from `FiberDone`" is met by
the checker, not by construction.

### 7.4 Mutant battery

| switch | injected defect | killed by |
|--------|-----------------|-----------|
| `MutCreatePermit` | parking mints a permit | `InvPermitPool` |
| `MutLosePermit` | a granted release stores nothing | `InvPermitPool` |
| `MutDoubleConsume` | the fast path skips the decrement | `InvPermitPool` |
| `MutFifoBypass` | handoff wakes the queue tail | `InvFifo` |
| `MutOverflowFull` | ceiling refusal overflows: `available++` with `granted++` (accounting stays balanced), result stays `false` | `TypeOK` / `InvCapacity` |
| `MutWrongFullResult` | ceiling refusal returns `true` — state and accounting unchanged, `effect_kind = refused` | `InvReleaseResult` |
| `MutWrongGrantResult` | a stored or handoff grant returns `false` — the state effect (and `effect_kind`) run normally | `InvReleaseResult` |
| `MutFusedReturn` | effect + return fused (the V2.2 shape) | **not a safety violation** — killed by the witness check (§7.3) |

The three result mutants split the old combined `MutWrongFull` so each
fault class is tested independently: `MutOverflowFull` proves the ceiling
sensitivity (capacity alone catches overflow even when the returned pair
is internally consistent), `MutWrongFullResult` proves a refused
`release()` cannot report `true`, and `MutWrongGrantResult` proves a
granted `release()` cannot report `false`. Each dies on exactly its
intended invariant — a `release(true)` at the ceiling does not touch
capacity, and a grant returning `false` does not touch accounting — so
the result contract is gated on its own invariant, not borrowed from the
capacity checks.

The fused mutant is the interesting one: fusion only *skips* states, so
every safety invariant still holds under it. Its verdict is the
separation certificate — the gate runs it and requires a clean
completion *with `InvWitness` among its invariants*, i.e. the witness
must be unreachable exactly where the split does not exist.

### 7.5 Coverage

Coverage is by violation: each cfg checks one negated conjunction, and
the violation is the reachability certificate. The conjunctions are
monotone over `history` plus set-once flags, sized to a single
execution's `MaxHistory` fuel.

| cfg family | conjunction | certifies |
|------------|-------------|-----------|
| `CovA1`/`B1`/`D1` | `CovW1` = stored release + refusal + release return-window + external return-window | the two interleaving windows on a stored release |
| `CovA2`/`C2` | `CovW2` = acquire completion inside an external call's window | window A against an acquire |
| `CovA3`/`B3` | `CovQ` = FIFO handoff chain (handoff section ran, handoff-published acquire completed) + a take after a release issue + external grant in reverse entry order | the full handoff-and-reorder path |
| `CovC3` | `CovExtReorder` = external grant in reverse entry order (first entrant refused, later entrant granted) | the reorder alone at `(0,2)`, where the full `CovQ` conjunction needs a longer single execution than that domain's state space can afford to search |
| `CovD4` | `CovQ1` = the FIFO handoff chain alone | the handoff path at `(1,2)` on the same fuel grounds as `CovC3` |
| `CovB2`/`D2` | `CovW2 /\ CovInitialTake` | a take drawn purely from constructor stock |
| `CovC1` | `CovW1 /\ available = 2` | both windows with the ceiling reached |
| `CovD3` | `available = 2` | the ceiling state at all |
| `CovWitness` | `wseq = 5` | **the V2.3 seam witness itself** |

Configurations: the four safety cfgs are the full constructor matrix;
every scenario class is certified in at least two domains — `CovW1` at
`(0,1)`/`(1,1)`/`(1,2)`, `CovW2` in all four (directly, and inside the
initial-stock conjunction), the FIFO handoff chain at
`(0,1)`/`(1,1)`/`(1,2)`, the reverse-entry-order external grant at
`(0,1)`/`(1,1)`/`(0,2)`, the initial-stock take at `(1,1)`/`(1,2)`, and
the ceiling at `(0,2)`/`(1,2)` — so the coverage claim holds per
constructor domain, not just at one point. The witness and coverage
cfgs also carry the nine safety invariants (the eight plus
`InvReleaseResult`), so the deeper certificates cannot be earned by a
run that breaks safety — or the release result contract — on the way.

### 7.6 Model-checking results

TLC 2026.09.12.025210, `-deadlock` (terminal parked states exist by the
fuel bounds), one run per cfg, single worker. Full gate ≈ 20 min wall
clock — the full-domain counts below are a few percent higher than the
pre-corrective runs (§7.2: the effect-authority `kind` field refines the
state space; the old model merged states whose `stored` vs `handoff`
origins had diverged once the queue moved on). "violated" is the required
outcome for witness/coverage/mutant cfgs (reachability or kill
certificate); "clean" for safety and the fused mutant.

Safety matrix — all clean, `InvReleaseResult` included:

| cfg | (initial, max) | fuel | generated | distinct |
|-----|----------------|------|-----------|----------|
| `SemCore` | (0, 1) | 6 | 750,463 | 510,689 |
| `SemCoreI1` | (1, 1) | 6 | 546,239 | 362,739 |
| `SemCoreM2` | (0, 2) | 6 | 653,299 | 435,503 |
| `SemCoreI1M2` | (1, 2) | 6 | 619,007 | 403,653 |

Witness and coverage — all violated, each on its own invariant:

| cfg | (initial, max) | fuel | violated invariant | generated | distinct |
|-----|----------------|------|--------------------|-----------|----------|
| `CovWitness` | (0, 1) | 6 | `InvWitness` | 5,112 | 3,224 |
| `CovA1` | (0, 1) | 9 | `InvCovW1` | 83,232 | 49,493 |
| `CovA2` | (0, 1) | 7 | `InvCovW2` | 3,261 | 2,030 |
| `CovA3` | (0, 1) | 9 | `InvCovQ` | 772,238 | 448,355 |
| `CovB1` | (1, 1) | 11 | `InvCovW1` | 1,370,852 | 776,735 |
| `CovB2` | (1, 1) | 7 | `InvCovInitial` | 90,887 | 50,906 |
| `CovB3` | (1, 1) | 11 | `InvCovQ` | 3,057,902 | 1,726,714 |
| `CovC1` | (0, 2) | 11 | `InvCovMax2` | 71,620 | 42,681 |
| `CovC2` | (0, 2) | 7 | `InvCovW2` | 3,128 | 1,864 |
| `CovC3` | (0, 2) | 9 | `InvCovReorder` | 16,163 | 9,501 |
| `CovD1` | (1, 2) | 11 | `InvCovW1` | 81,784 | 46,925 |
| `CovD2` | (1, 2) | 7 | `InvCovInitial` | 112,866 | 63,424 |
| `CovD3` | (1, 2) | 5 | `InvCovMax2State` | 33 | 29 |
| `CovD4` | (1, 2) | 6 | `InvCovQ1` | 105,411 | 61,681 |

Mutants — the seven safety mutants violated, each on its intended
invariant, the fused mutant clean:

| cfg | violated invariant | generated | distinct |
|-----|--------------------|-----------|----------|
| `MutCreatePermit` | `InvPermitPool` | 48 | 38 |
| `MutLosePermit` | `InvPermitPool` | 33 | 29 |
| `MutDoubleConsume` | `InvPermitPool` | 503 | 316 |
| `MutFifoBypass` | `InvFifo` | 7,321 | 4,399 |
| `MutOverflowFull` | `TypeOK` (ceiling overflow; `InvCapacity` equally bites) | 402 | 256 |
| `MutWrongFullResult` | `InvReleaseResult` | 402 | 256 |
| `MutWrongGrantResult` | `InvReleaseResult` | 33 | 29 |
| `MutFusedReturn` | none — clean, `InvWitness` unreachable | 818,595 | 594,981 |

The `MutWrongFullResult` counterexample is an external release effected
at the ceiling with `result = "t"`, `effect_kind = "refused"`; the
`MutWrongGrantResult` counterexample is an external store
(`effect_kind = "stored"`) returning `"f"` — both killed at the effected
record, before any completion, by `InvReleaseResult` (§7.2).

### 7.7 Gate wiring

`scripts/verify_tla.sh` runs both stages automatically: the four safety
cfgs must complete with no error; the witness cfg must fail with
`Invariant InvWitness is violated`; each coverage cfg must fail with its
own invariant; each safety mutant must fail with the invariant named in
§7.4 (`MutOverflowFull` accepts either `TypeOK` or `InvCapacity`, the two
forms of the ceiling check; the two result mutants must fail with
`InvReleaseResult` and nothing else); the fused-return cfg must complete
with no error while checking `InvWitness`. Expected failure modes are
matched textually — a bare non-zero exit or a wrong-invariant failure
aborts the gate.

## 8. Gates

| gate | result |
|------|--------|
| `lake build` (Lean 4.33.1, formal/Sluice.lean incl. SemV2) | PASS |
| `./scripts/verify_formal.sh` (build + sorry/admit scan + axiom audit ⊆ {propext, Quot.sound}) | PASS |
| `./scripts/verify_tla.sh` (Stage 1V2.2 EventCore + mutant; Stage 2V2.3 SemCore: safety matrix, witness, 13 coverage certs, 7 safety mutants incl. the release result-semantics battery, fused-return separation) | PASS |

## 9. Downstream obligations

* **`SemCore.tla` is delivered on this branch (§7) and remains a merge
  gate for PR #378's final merge** (human review decision, #378
  verdict): the V2.3 action set, the constructor matrix, the 13 coverage
  certificates, the mutant battery, and the fusion-separation witness
  are all wired into `scripts/verify_tla.sh`.
* The THEOREM-B question (semaphore reducibility, either direction) is
  recorded RESEARCH/DEFER on #375; a per-encoding counter-evidence
  battery (the semaphore analogue of Stage 1's `encChained` blockade) is
  the natural next step and is not attempted here.
* `try_acquire`/`cancel`/`acquire_until` enter the model only if a stage
  needs them; their census rows above are the authority until then.
