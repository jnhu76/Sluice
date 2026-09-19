# Stage 7V2.3 — Select core, replayed and re-adjudicated on the amended calculus

Campaign: FCB1-POST-V23-STACK-379-385 (Issue #375, §34). Calculus:
Stage-0-V2.3 as amended through Stage 5 (`CalcV2.lean`, unchanged by this
stage). Judgment layer: `JudgeV2.lean`. Chain: Stage-0 V2.3 → Event →
Semaphore (#378) → Mutex (#379) → Condition (#380) → RwLock (#381) →
Queue (#382) → this select core (#383). **RE-ADJUDICATED ON POST-#378
V2.3 AUTHORITY**: the pre-V2.3 stage-7 verdict is retired (§6); this
stage's capability verdict is RESEARCH/DEFER.

`BASE(select) = {}` — the Stage-0 §8 default (the select's sections read
no substrate beyond the bare execution frame; the event latch is the
select's own private state). No base, no enlargement, no alternative.

## 1. Scope and subject

The select core: `include/sluice/async/select.hpp`,
`src/async/select.cpp`, `src/async/select_event.cpp`,
`src/async/select_timer.cpp`, `src/async/scheduler_event.cpp`. Core
surface modeled: the two Event-surface sections (`evSet`, `evReset`)
and the two select calls (`sel` — the event arm at the lower index —
and `selT` — the timer arm at the lower index). Outside the core
surface, exactly as at Stage 1: the event waiters (`EventV2`'s waitq
and its drained-count result — the Stage-1 core's discipline, not the
select's), and the concurrent group list beyond its head (the
production surface supports multiple simultaneous groups with a
contract-level fail-fast exclusion; this stage models the single
in-flight group, §9).

Four structural facts shape the model:

* **One group slot, two arms, fixed orders.** A select suspends as a
  group (`phase = armed`, select.cpp:758-759) with its scan order
  fixed at entry: `sel` scans the event arm first, `selT` the timer
  arm (the admission scan :691-714 takes the first ready arm in index
  order). The model carries exactly two orders rather than a general
  arm vector (§9).
* **Atomic resolution.** The source sections resolve an armed group in
  the same critical section that raises the source: `Event::set` sets
  the latch and runs `select_resolve_event_locked` →
  `select_process_group_locked` (:390-466) with the one-shot claim CAS
  (:164-168) and the result write (:149-180); the timer pump
  (`select_timer_pump_entry_locked`, select_timer.cpp:38-55, the
  registration consumed at :143) is the timer-side counterpart. There
  is no window in which the latch stands and a group stays armed —
  the armed-quiet invariant below.
* **Level semantics and reset blindness.** An event win does not
  consume the latch (select_event.cpp:26-28), so a fresh select sees
  it again (the re-arm); `event_reset` cannot see armed groups
  (scheduler_event.cpp:39-42) — a reset while a group stands armed
  leaves the group alone.
* **Timer due-ness is an environment given.** The model carries a
  `timDue` bit raised by `onTick` (the clock passing at idle points)
  and consumed by the pump (`expire`); due-ness persists, modeling
  the legal fresh-select-with-past-deadline input. The deadline
  registration and its internals are outside the modeled surface.

## 2. Call-domain census (`extCap`/`extRun`)

| entry     | code                                   | worker read | domain          | model                          |
|-----------|----------------------------------------|-------------|-----------------|--------------------------------|
| `evSet`   | scheduler_event.cpp:21-37 (the one set body; select_event.cpp:11-53 is the winner/loser finalize it drives) | none (`global_mtx_` only) | fiber-callable **and** external-capable | `extCap = true`, `extRun = selExtRun` |
| `evReset` | scheduler_event.cpp:39-42              | none        | fiber-callable **and** external-capable | `extCap = true`, `extRun = selExtRun` |
| `sel`     | select.hpp:127-134 + select.cpp:538-547, :691-714, :758-759, :775-808 | the worker pair (`select_admit` asserts a running Fiber, :538-547) | fiber-bound | `extCap = false` |
| `selT`    | the symmetric order                    | the worker pair | fiber-bound  | `extCap = false` |
| event waiters | EventV2 surface                    | —           | outside the core surface | not modeled |
| group list beyond the head | select.cpp group iteration | —      | outside the core surface (single slot modeled) | not modeled |

Disclosures carried into the model: the entry facet is total (`selAdmit
≡ some`) — arm-shape validation is the template constraint, not a
runtime rejection; a select issued while the group slot is busy parks
untracked and is never woken (the concurrent-group boundary, §9); the
external event sections are body-identical to the fiber paths (both
read no worker state).

## 3. The group discipline

The whole select discipline is one conjunction (the Lean `selSafe`,
the TLA invariants):

* **Phase/winner agreement**: the committed winner exists exactly while
  the group is `done` (already-won rejection, :153-155).
* **Source-read ghosts**: the winner carries the source read that
  committed it — an event win saw the latch (`winEvOK`), a timer win
  saw the due timer (`winTimOK`); frozen at the resolution instant
  against later resets.
* **Armed quiet**: a suspended group never coexists with a set latch —
  every latch-raising section resolves an armed group in the same
  critical section (the atomic-resolution discipline).
* **The tracked caller** is recorded while the group stands.
* **The one-record completion window**: at most one resolved-but-
  unconsumed outcome, only a `done` group holds one, and it is exactly
  the committed winner for the tracked caller (the one-shot claim CAS,
  :164-168).

Alongside stands **result agreement** (the Semaphore lesson: a wrong
public result preserves every state invariant, so the agreement with
the outcome authority is its own check): `selSpecRun` (the frozen scan
orders, written resolution-free) and `selSpecFin` (the finish delivers
the consumed record), proved per facet (`selRun_result_agrees`,
`selExtRun_result_agrees`, `selFinish_result_agrees`).

Proved in Lean by runs-induction over all twelve `PrimStep2`
constructors with per-transition preservation lemmas
(`selRun_preserves`, `selPark_preserves`, `selFinish_preserves`,
`selExtRun_preserves`, and the section lemmas `selResolveEv_preserves`,
`selSetSection_preserves`, `selExpire_preserves`, `selOnTick_preserves`).

## 4. Possession batteries (ten, one per behavior class)

Explicit `PrimRuns2` step chains (the Stage-5 idiom; batteries C–H
start from instrumented intermediate configurations — each a
constructed state proved safe by `selSafe`, whose shape the TLA mirror
explores from `primInit` via the `armed`/`due` boots and the park/
handoff coverage witnesses):

* `sel_battery_inlineEv` — from `primInit`: a fiber sets the latch,
  then a `sel` issued with the latch set wins the event arm inline (4
  observations).
* `sel_battery_inlineTim` — time passes, then a `selT` issued with the
  timer due wins the timer arm inline.
* `sel_battery_handoffEv` — a `sel` arms; an external `evSet` resolves
  the armed group in the same section; the resumed select consumes and
  reports `wonEv` (the set-section hand-off).
* `sel_battery_handoffTim` — a `sel` arms; the pump fires and resolves;
  the resumed select reports `wonTim` (the timer-pump hand-off).
* `sel_battery_prioEv` / `sel_battery_prioTim` — both arms ready (latch
  set, timer due): the scan order decides — `sel` wins `wonEv`, `selT`
  wins `wonTim`.
* `sel_battery_resetBlind` — an external `evReset` over an armed group
  leaves it armed; the later `evSet` still resolves it (the blindness).
* `sel_battery_fiberSet` — a second fiber's `evSet` resolves the armed
  group from the fiber path (the sections are caller-agnostic).
* `sel_battery_rearm` — after a full consume the latch still stands
  (level semantics); a fresh `sel` wins inline again.
* `sel_battery_noSpurious` — an external `evSet` with nothing armed
  only raises the latch: no resolution, no window record.

## 5. Mutant battery (five independent fault classes)

* M1 `selResolveEvM1` — the done-group gate dropped from the event
  resolution (SAFETY, at-most-once): a `done` group resolves again,
  appending a second completion-window record; `selM1_breaks` exhibits
  the mutant endpoint against the safe start (`selSafe` holds at the
  start, `¬ selSafe ssM1end` at the endpoint). TLA `MutReResolve`
  killed by `InvWindow`.
* M2 `selFinishM2` — the resumed select delivers `wonEv` regardless of
  the consumed record (RESULT-SEMANTICS): the released state stays
  safe; `selM2_wrong_result` exhibits the contradiction with the
  completion authority (the consumed record is a timer win). TLA
  `MutFinishLie` killed by `InvResultAgree`.
* M3 `selParkM3` — the scan's ready-source gate dropped from the park
  (SAFETY, armed quiet): a select arms with the latch already set;
  `selM3_breaks` exhibits the model refusing the park (the scan wins
  inline) while the mutant arms into `¬ selSafe`. TLA `MutParkReady`
  killed by `InvQuietArmed`.
* M4 `selPrimM4` — the timer pump omitted, `expire` never fires
  (TRACE-REMOVAL): every state invariant still holds (every other
  facet is the model's); `selM4_removes` exhibits the definitional
  removal (the model's pump fires on the armed-due state, the
  mutant's does not). TLA `MutNoTimer` completes cleanly with
  `NotHandoffTim` — the timer-handoff witness is unreachable there,
  while `SelectCoreCovHandoffTim` certifies it reachable in the
  correct model.
* M5 `selRunM5` — the scan order flipped, `sel` checking the timer arm
  first (RESULT-SEMANTICS): with both arms ready the wrong arm wins;
  `selM5_wrong_arm` exhibits the divergent results against the frozen
  authority. TLA `MutM5` killed by `InvResultAgree`.

## 6. Capability adjudication

* **OLD VERDICT — THEOREM B** (pre-V2.3 stage 7, branch
  `formal/fcb1-stage-7` at `7d03e1f6`): the select core is
  irreducible to the bare substrate; every encoding mismatches.
* **THE OLD PROOF IS REFUTED.** The proof rested on the
  completion-shadow projection (`tracesEnc_shadow enc
  shadowKeepFlagSet` — a keeper erasing the flag/window state). Under
  V2.3 that lemma is false — `tracesEnc_shadow_false` (CalcV2.lean)
  exhibits an encoding possessing a completion-shadowed trace — so no
  THEOREM-B-shaped conclusion can rest on it. The same refutation that
  retired the Stage-6 queue verdict retires this one.
* **INVALID UNDER STAGE-0 V2.3.** With the shadow gone, the old
  universal mismatch proof has no premises to stand on; the verdict it
  carried is retired, not appealed.
* **NEW SEARCH RESULTS.** What the frozen method supports are two
  conditional, per-encoding-class statements (`sel_capability_defer`):
  - Over-production for the class that declares the fiber-bound `sel`
    externally callable: the primitive never emits an external `sel`
    issue (`selExtCap sel = false`), while every such encoding does —
    at the encoding machine's entry step, before any program runs
    (`sel_over_produces_of_extCap`, separator `extSelTrace`,
    non-vacuity instance `encExtSel_over`).
  - Under-production for the class that declares `evSet` not
    externally callable: the primitive emits the external `evSet`
    issue plus its completed resolution (`sel_possesses_extSet`),
    while no such encoding can emit that issue at all
    (`sel_under_produces_of_no_extCap`, non-vacuity instance
    `encNoSel_under`).
  Neither statement is named THEOREM B and neither closes the universal
  question. The encoding class is inhabited (`encSel`,
  `sel_encoding_class_inhabited`).
* **VERDICT — RESEARCH/DEFER.** RESEARCH/DEFER does NOT mean
  reducible. RESEARCH/DEFER does NOT mean probably irreducible. The
  exact open boundary:

  > Does there exist an Encoding BaseOpsSig.none SelSig whose
  > disciplined trace language is observationally equivalent to
  > `selPrim` under Stage-0 V2.3? No such reduction is presently
  > constructed. No universal mismatch proof is presently known. The
  > previous universal mismatch proof is invalid because its
  > completion-shadow lemma is false under V2.3.

## 7. Model decisions recorded by this build

No production-vs-model fact conflict surfaced: the model was built
directly from the current sections. The decisions below are disclosed
modeling choices, not corrections.

* **Two fixed arm orders, not a general arm vector.** The production
  scan is index-ordered over an arm vector; the modeled surface is the
  two-arm select with both orders (the `sel`/`selT` pair). The scan
  discipline (first ready arm in index order) is what the model
  carries.
* **Due-ness persists.** `onTick` only raises; nothing lowers. This
  models the legal fresh select whose deadline is already past.
* **Total entry facet.** Registrations cannot reject; bad arm shapes
  are the template's constraint, not a runtime path.
* **Single group slot with an untracked overflow park.** The head
  group is the modeled surface; a select arriving while the slot is
  busy parks untracked and is never woken (§9).

## 8. The TLA mirror (`formal/tla/SelectCore.tla`)

One action per `PrimStep2` constructor, the census of §2 as comments,
`Boot` selecting three initial states (`"prim"` — the primInit-
faithful start; `"armed"` — f0 parked on `sel`; `"due"` — f0 armed
with the timer already due; the boots are relation-reachable but not
primInit-reachable in-window, the documented battery disclosure).
Safety matrix: `TypeOK`, `InvQuietArmed`, `InvWinnerDone`, `InvWindow`,
`InvGroupOwnership`, `InvResultAgree`, `InvCompDiscipline`.

Configurations and outcomes: safety `SelectCore` (prim),
`SelectCoreArmed`, `SelectCoreDue` — all clean; coverage
`SelectCoreCov{InlineEv,InlineTim,HandoffEv,HandoffTim,PrioEv,PrioTim,
ResetBlind,Rearm}` — each violated on its negated witness (the witness
is reachable); kills `SelectCoreMutReResolve` → `InvWindow`,
`SelectCoreMutFinishLie` → `InvResultAgree`,
`SelectCoreMutParkReady` → `InvQuietArmed`,
`SelectCoreMutScanFlip` → `InvResultAgree`; separation
`SelectCoreMutNoTimer` — clean with `NotHandoffTim` asserted (full
safety intact).

TLC lessons recorded by this mirror (mirror-implementation facts, not
model-vs-C++ disputes; the first two are the Stage-5/6 lessons biting
again):

* **Every primed variable must be constrained per action.** `EnvTime`
  constrained twenty-six of twenty-seven variables and TLC rejected
  exactly that action — `cur'` missing. The count now stands at
  twenty-seven for every action.
* **Guard complements must be written against the action set, not the
  English.** The plain set-section guard needed `~(resolve shape)`
  *under the mutant switch too* — an overlap with the resolve action
  under `MutReResolve` would have given the mutant a phantom
  plain-path broadcast.
* **The delivery result is the consumed record's outcome.** The
  resumed select's finish carries `c.out` into the completion
  observation — the same V2.3 split as the queue's waiters (the
  resolver fixed the outcome, the finish reports it).

## 9. Modeling disclosures

* **Single event alphabet.** The model's `flag` and the `evSet`/`evReset`
  calls denote the select arm's own event. In production a `set`/`reset`
  on any other `Event` instance leaves an armed group alone — resolution
  runs only through the armed arm's select port (select.cpp:399-411).
  The discipline facts (armed-quiet, no-spurious) therefore read
  per-event, and hold per-event in production.
* **Single group slot.** The production surface holds a list of
  simultaneous groups with a contract-level fail-fast exclusion for
  multi-arm overlap; the model carries one in-flight group and parks
  further selects untracked (never woken). The list is the Stage-1
  core's discipline plus the scheduler's accounting, not new select
  semantics.
* **No cancel for parked selects.** The production surface has no
  cancel path that aborts an armed group (unlike the queue's
  test-access `queue_cancel`); the model reflects that — an armed
  group leaves only by resolution.
* **Event waiters out of the surface.** `EventV2`'s waitq and
  drained-count results are the Stage-1 core's discipline; the select
  mirror reads only the latch.
* **The timer model is due-ness, not clocks.** Registrations,
  deadlines, and the timer wheel are outside the surface; `EnvTime`/
  `EnvExpire` carry the only observable consequence (the pump resolving
  an armed due group).
* **The fiber call domain carries all four calls.** `evSet`/`evReset`
  are fiber-callable (the fiber event sections) and the select calls
  are fiber-bound — no fiber-domain narrowing is applied (the
  QueueCore close precedent does not arise).
* **Boot groups are pre-Init** — the same prefix-scoped
  `InvCompDiscipline` exception the Stage-4/5/6 cards document.
* **`order` is mirrored but unread.** The group records its arm order
  (`order` field) exactly as production stores it, but no guard or
  invariant reads it — the scan authority is carried structurally by
  the sel/selT split (§3). Write-only in both the Lean model and the
  TLA mirror.

## 10. Gates and verdict

* `scripts/verify_formal.sh` — **PASS** (lake build; no sorry/admit;
  every exported select theorem depends only on
  `[propext, Quot.sound]` or fewer).
* `scripts/verify_tla.sh` — **PASS** (stages 1–6 re-run green; Stage 7:
  3 safety boots clean, 8 coverage witnesses violated, 4 mutants
  killed on their exact intended invariants, 1 trace-removal
  separation clean).
* `CalcV2.lean` / `JudgeV2.lean` — unchanged.
* Fresh-context adversarial review: **READY** (no BLOCKING, no MAJOR;
  three MINOR findings, all fixed in this card: the single-event
  alphabet disclosure, the `evSet` census citation phrasing, and the
  write-only `order` disclosure). The reviewer independently re-ran
  `verify_formal.sh` (PASS; all 40 select theorems within
  `[propext, Quot.sound]`), verified the full-gate
  `VERIFY_TLA: PASS` log (102 runs), reproduced
  `SelectCoreCovPrioEv` byte-identical (5929 generated / 2854
  distinct), spot-checked three transition families against
  select.cpp / scheduler_event.cpp / event.cpp, and confirmed the
  open-boundary wording matches the PR body verbatim.
* Verdict: **STAGE7_SEMANTICS_PASS / CAPABILITY_RESEARCH_DEFER /
  READY_FOR_STACK_CONTINUATION.**
