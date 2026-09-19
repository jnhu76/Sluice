/-
Stage 7 V2.3 (FCB1-POST-V23-STACK-379-385): the select core's
transition system, mapping `src/async/select.cpp`,

The two-arm untimed select over one event arm + one timer arm, modeled
from the current C++ (`include/sluice/async/select.hpp`,
`src/async/select.cpp`, `src/async/select_event.cpp`,
`src/async/select_timer.cpp`) on the frozen Stage-0 V2.3 calculus
(`CalcV2.lean`), with the frozen judge (`JudgeV2.lean`).

Re-adjudication, not inheritance.  The pre-V2.3 campaign verdict
("select core is THEOREM B", branch `formal/fcb1-stage-7` at
`7d03e1f6`) rested on the completion-shadow projection: a keeper
erasing the `flagSet` observation transferred
`tracesEnc_shadow enc shadowKeepFlagSet` into a universal mismatch.
Under V2.3 that lemma is false — `tracesEnc_shadow_false` (Stage 0)
exhibits an encoding possessing a completion-shadowed trace — so the
old universal proof has no valid reconstruction, exactly as for the
AsyncQueue (Stage 6).  The old verdict is retired, not appealed; the
old branch and its SHA remain fetchable as historical evidence.  This
file rebuilds the model from production and re-derives what the frozen
method supports.

`BASE(select) = {}` — `BaseOpsSig.none`, the bare substrate, with the
Event arm semantics carried by the modeled state (the adjudicated
Stage-1 Event core's own flag discipline; the reduction verdict for
select is relative to that stage's verdict).  Frozen at Stage 0
(stage-0 card §8); no enlargement, no shrinking.

Production anchors (select.cpp unless stated):

  * `select(Scheduler&, Cases&&...)` returns `SelectResult` by value
    and is fiber-bound only — `select_admit` throws off a worker
    (:538-547); the model's `sel`/`selT` carry `extCap = false`.
  * arms register in index order (:623-645) and the admission scan
    reads readiness in index order (:691-714), keeping the FIRST ready
    arm — event ready = the latch (:699), timer ready = deadline passed
    (:702).  The model fixes the production-meaningful two-arm shape:
    `sel` = event arm first, `selT` = timer arm first.
  * resolution is one atomic critical section
    (`select_process_group_locked`, :149-180): already-won rejection
    (:153-155), the winner claim CAS (`claim_winner_locked`, :164-168),
    winner commit (event: unlink+retire, `select_event.cpp:11-31` --
    the latch is NOT consumed: level semantics; timer:
    `select_timer.cpp:143` active→consumed), loser finalization
    (:192-200), and publication in the same lock hold (:238-388:
    `--waiting_select_count_` + `make_runnable`, :322-332).
  * no arm ready → suspend (:758-770): the group arms
    (`phase = armed`), the caller parks, and only a source resolution
    wakes it (event edge `event_set_broadcast` →
    `select_resolve_event_locked`, :390-466; timer edge
    `pump_deadlines_locked` → `select_resolve_timer_locked`, :468-521).
  * each async source claims its own arm: the event edge resolves with
    the event arm (:443-454 -- the lowest index *matching the event*),
    the pump with the timer arm.  The global lowest-ready-index scan is
    the admission scan only.
  * `event_reset` is blind to armed selects (`scheduler_event.cpp:39-42`)
    and an event win leaves the latch set.
  * a fiber can also run `evSet` (the Event surface is callable on a
    fiber); its section resolves an armed group exactly like the
    external one.
  * no cancel or abandon exists for a parked select (`cancel.cpp` has
    no select references); a parked select is woken only by a source.

Modeling disclosures (bounded instance, the charter's "two-arm untimed
select"):

  * `kSelectMaxArms = 8` general arms are narrowed to the two
    production arm kinds in both index orders (`sel`, `selT`); the
    lowest-index scan is real (the two orders pick different winners
    when both arms are ready).
  * one group slot: a `sel` issued while a group is armed or resolved
    parks and is never woken.  Production admits distinct groups, and
    a second group on the SAME event fails fast at resolution
    (:433-435) — a contract-excluded scenario; the model conservatively
    adds silence, never a wrong completion.
  * timer due-ness is an environment given: `onTick` raises `timDue`
    (time passes; `envTime` steps the clock at idle points), and
    `selExpire` is the pump (:38-55 of `select_timer.cpp`).  Due-ness
    persisting across selects models the legal fresh-select-with-past-
    deadline input.
  * the entry facet is total (always `some`): the arm-shape validation
    is the template constraint, not a runtime rejection.
  * event waiters (`EventV2`'s waitq and its drained-count result) are
    out of the modeled surface: they are the Stage-1 core's discipline,
    not the select's.
-/

import Sluice.Formal.QueueV2

namespace Sluice.Formal

/-! ## API and state -/

inductive SelCall : Type where
  | evSet
  | evReset
  | sel
  | selT
deriving instance DecidableEq for SelCall

inductive SelRes : Type where
  | rEvSet
  | rEvReset
  | wonEv
  | wonTim
deriving instance DecidableEq for SelRes

abbrev SelSig : ApiSig := ⟨SelCall, SelRes⟩

/-- The group machine: `idle` (no select in flight), `armed` (a select
suspended with no ready arm), `done` (a source committed the winner;
delivery pending). -/
inductive SelPhase : Type where
  | idle
  | armed
  | done
deriving instance DecidableEq, Inhabited for SelPhase

/-- Select private state: the event latch (level semantics — an event
win does not consume it), the timer-arm due-ness bit (environment
raised), the group phase, the committed winner, the armed order (which
arm has the lower index), the tracked caller, the resolution ghosts
(source readiness at the resolution instant), and the completion
window (resolved-but-unconsumed outcomes). -/
structure SelState where
  flag : Bool
  timDue : Bool
  phase : SelPhase
  winner : Option SelRes
  order : Bool
  caller : Option FiberId
  winEvOK : Bool
  winTimOK : Bool
  resolved : List (FiberId × SelRes)

def selInit : SelState :=
  ⟨false, false, SelPhase.idle, none, true, none, false, false, []⟩

/-- Remove the first record of `f`, yielding its outcome (the one-shot
consume of the committed result at the resumed caller's finish). -/
def selConsume : List (FiberId × SelRes) → FiberId → Option (SelRes × List (FiberId × SelRes))
  | [], _ => none
  | (g, o) :: rest, f => if g = f then some (o, rest)
      else (fun p => (p.1, (g, o) :: p.2)) <$> selConsume rest f

/-! ## The resolution sections -/

/-- The event source's claim on an armed group
(`select_resolve_event_locked` → `select_process_group_locked`,
:390-466 + :149-180): the latch is set (level semantics — not
consumed), the event arm wins, the ghost records the source read, the
caller is published runnable.  No armed group = no resolution. -/
def selResolveEv (s : SelState) : Option (SelState × FiberId) :=
  match s.phase, s.caller with
  | SelPhase.armed, some f =>
      some ({ s with flag := true, phase := SelPhase.done, winner := some SelRes.wonEv, winEvOK := true, resolved := s.resolved ++ [(f, SelRes.wonEv)] }, f)
  | _, _ => none

/-- The shared `Event::set` body (`event_set_broadcast`,
`scheduler_event.cpp:21-37`, plus the select resolution it drives):
set the latch, then resolve an armed group in the same critical
section. -/
def selSetSection (s : SelState) : SelRes × SelState × List FiberId :=
  match selResolveEv s with
  | some (s', f) => (SelRes.rEvSet, s', [f])
  | none => (SelRes.rEvSet, { s with flag := true }, [])

/-- The timer pump's claim (`select_resolve_timer_locked`, :468-521,
driven by `select_timer_pump_entry_locked`, `select_timer.cpp:38-55`):
the timer arm wins, the ghost records the due read, the registration
is consumed (`select_timer.cpp:143`), the caller is published
runnable. -/
def selExpire (s : SelState) (_t : Tick) (f : FiberId) : Option SelState :=
  match s.phase, s.caller with
  | SelPhase.armed, some g =>
      if g = f ∧ s.timDue then
        some { s with phase := SelPhase.done, winner := some SelRes.wonTim, winTimOK := true, resolved := s.resolved ++ [(f, SelRes.wonTim)] }
      else none
  | _, _ => none

/-! ## The primitive -/

/-- Entry gate: no caller-precondition aborts exist on this surface
(arm-shape validation is the template constraint; `select_admit`
asserts only the fiber/worker pairing, which is the calculus's own
fiber-bound discipline). -/
def selAdmit : SelState → FiberId → SelCall → Option SelState
  | s, _, _ => some s

/-- The inline paths of a dispatched call.

  `evSet` / `evReset` -- the Event surface sections (fiber-callable);
  `evSet` resolves an armed group in the same critical section.

  `sel` (event arm at the lower index) -- the admission scan
  (:691-714): the first ready arm wins inline; no arm ready →
  suspend.  `selT` is the timer-first order.  A call issued while the
  single group slot is busy suspends into the never-woken untracked
  park (the concurrent-group boundary, disclosed above). -/
def selRun : SelState → Tick → FiberId → SelCall →
    Option (SelRes × SelState × List FiberId)
  | s, _, _, SelCall.evSet => some (selSetSection s)
  | s, _, _, SelCall.evReset => some (SelRes.rEvReset, { s with flag := false }, [])
  | s, _, _, SelCall.sel =>
      if s.phase = SelPhase.idle then
        if s.flag then some (SelRes.wonEv, s, [])
        else if s.timDue then some (SelRes.wonTim, s, [])
        else none
      else none
  | s, _, _, SelCall.selT =>
      if s.phase = SelPhase.idle then
        if s.timDue then some (SelRes.wonTim, s, [])
        else if s.flag then some (SelRes.wonEv, s, [])
        else none
      else none

/-- Suspension: the group arms (`phase = armed`, :758-759) with the
caller recorded and the scan order fixed; a call arriving while the
slot is busy parks untracked (state unchanged — it is never woken). -/
def selPark : SelState → FiberId → SelCall →
    Option (SelState × List FiberId)
  | s, f, SelCall.sel =>
      if s.phase = SelPhase.idle
        then some ({ s with phase := SelPhase.armed, order := true, caller := some f }, [])
        else some (s, [])
  | s, f, SelCall.selT =>
      if s.phase = SelPhase.idle
        then some ({ s with phase := SelPhase.armed, order := false, caller := some f }, [])
        else some (s, [])
  | _, _, _ => none

/-- The resumed select caller consumes the committed winner
(:775-808): the result is exactly the resolution's record, and the
group slot is released (the group ends consumed).  The Event calls
never suspend. -/
def selFinish : SelState → FiberId → SelCall →
    Option (SelRes × SelState × List FiberId)
  | s, f, SelCall.sel =>
      match selConsume s.resolved f with
      | some (r, rest) =>
          some (r, { s with resolved := rest, phase := SelPhase.idle, winner := none, winEvOK := false, winTimOK := false, caller := none }, [])
      | none => none
  | s, f, SelCall.selT =>
      match selConsume s.resolved f with
      | some (r, rest) =>
          some (r, { s with resolved := rest, phase := SelPhase.idle, winner := none, winEvOK := false, winTimOK := false, caller := none }, [])
      | none => none
  | _, _, _ => none

/-- The Event surface is external-capable (`event_set_broadcast` /
`event_reset` take only `global_mtx_`); the select call is fiber-bound
(:538-547). -/
def selExtCap : SelCall → Bool
  | SelCall.evSet => true
  | SelCall.evReset => true
  | _ => false

/-- The external critical sections: identical bodies to the fiber
paths (both read no worker state). -/
def selExtRun : SelCall → SelState → Tick →
    Option (SelRes × SelState × List FiberId)
  | SelCall.evSet, s, _ => some (selSetSection s)
  | SelCall.evReset, s, _ => some (SelRes.rEvReset, { s with flag := false }, [])
  | _, _, _ => none

/-- The clock mirror: time passing can due the timer arm (a finite
deadline eventually passes; due-ness persists, modeling the legal
past-deadline fresh select). -/
def selOnTick : SelState → Tick → SelState :=
  fun s _ => { s with timDue := true }

/-- The primitive, reducible so its facet equations reduce in proofs. -/
@[reducible] def selPrim : PrimLTS2 SelSig :=
  { State := SelState
    init := selInit
    admit := selAdmit
    run := selRun
    park := selPark
    finish := selFinish
    extCap := selExtCap
    extRun := selExtRun
    onTick := selOnTick
    expire := selExpire }

/-! ## The result-semantics authority

The select contract's own outcome function, written independently of
the primitive's sections: no wake lists, no completion window.  Wherever
a public result exists, the primitive's result must be exactly this one
(the Semaphore `release(bool)` lesson: a wrong result can preserve
every state invariant, so the agreement is its own check). -/

def selSpecRun : SelState → SelCall → Option (SelRes × SelState)
  | s, SelCall.evSet =>
      match selResolveEv s with
      | some (s', _) => some (SelRes.rEvSet, s')
      | none => some (SelRes.rEvSet, { s with flag := true })
  | s, SelCall.evReset => some (SelRes.rEvReset, { s with flag := false })
  | s, SelCall.sel =>
      if s.phase = SelPhase.idle then
        (if s.flag then some (SelRes.wonEv, s)
         else if s.timDue then some (SelRes.wonTim, s) else none)
      else none
  | s, SelCall.selT =>
      if s.phase = SelPhase.idle then
        (if s.timDue then some (SelRes.wonTim, s)
         else if s.flag then some (SelRes.wonEv, s) else none)
      else none

/-- The resumed caller's delivered result is the committed winner. -/
def selSpecFin : SelRes → SelRes := fun r => r

theorem selRun_result_agrees : ∀ (s : SelState) (t : Tick) (f : FiberId) (c : SelCall)
    (r : SelRes) (s' : SelState) (ws : List FiberId),
    selRun s t f c = some (r, s', ws) → (selSpecRun s c).map Prod.fst = some r := by
  intro s t f c r s' ws h
  cases c with
  | evSet =>
      simp only [selRun, selSetSection] at h
      simp only [selSpecRun]
      cases hr : selResolveEv s with
      | none =>
          simp only [hr] at h ⊢
          simp at h
          obtain ⟨h1, -, -⟩ := h
          simp [h1]
      | some p =>
          obtain ⟨s2, g⟩ := p
          simp only [hr] at h ⊢
          simp at h
          obtain ⟨h1, -, -⟩ := h
          simp [h1]
  | evReset =>
      simp only [selRun, selSpecRun] at h ⊢
      simp at h
      obtain ⟨h1, -, -⟩ := h
      simp [h1]
  | sel =>
      simp only [selRun] at h
      simp only [selSpecRun]
      by_cases hph : s.phase = SelPhase.idle
      · simp only [hph, if_true] at h ⊢
        by_cases hf : s.flag = true
        · simp only [hf, if_true] at h ⊢
          simp at h
          obtain ⟨h1, -, -⟩ := h
          simp [h1]
        · simp only [hf, Bool.false_eq_true, if_false] at h ⊢
          by_cases ht : s.timDue = true
          · simp only [ht, if_true] at h ⊢
            simp at h
            obtain ⟨h1, -, -⟩ := h
            simp [h1]
          · simp only [ht, Bool.false_eq_true, if_false] at h
            simp at h
      · simp only [hph, if_false] at h
        simp at h
  | selT =>
      simp only [selRun] at h
      simp only [selSpecRun]
      by_cases hph : s.phase = SelPhase.idle
      · simp only [hph, if_true] at h ⊢
        by_cases ht : s.timDue = true
        · simp only [ht, if_true] at h ⊢
          simp at h
          obtain ⟨h1, -, -⟩ := h
          simp [h1]
        · simp only [ht, Bool.false_eq_true, if_false] at h ⊢
          by_cases hf : s.flag = true
          · simp only [hf, if_true] at h ⊢
            simp at h
            obtain ⟨h1, -, -⟩ := h
            simp [h1]
          · simp only [hf, Bool.false_eq_true, if_false] at h
            simp at h
      · simp only [hph, if_false] at h
        simp at h

theorem selExtRun_result_agrees : ∀ (c : SelCall) (s : SelState) (t : Tick)
    (r : SelRes) (s' : SelState) (ws : List FiberId),
    selExtRun c s t = some (r, s', ws) → (selSpecRun s c).map Prod.fst = some r := by
  intro c s t r s' ws h
  cases c with
  | evSet =>
      simp only [selExtRun, selSetSection] at h
      simp only [selSpecRun]
      cases hr : selResolveEv s with
      | none =>
          simp only [hr] at h ⊢
          simp at h
          obtain ⟨h1, -, -⟩ := h
          simp [h1]
      | some p =>
          obtain ⟨s2, g⟩ := p
          simp only [hr] at h ⊢
          simp at h
          obtain ⟨h1, -, -⟩ := h
          simp [h1]
  | evReset =>
      simp only [selExtRun, selSpecRun] at h ⊢
      simp at h
      obtain ⟨h1, -, -⟩ := h
      simp [h1]
  | sel => simp [selExtRun] at h
  | selT => simp [selExtRun] at h

theorem selFinish_result_agrees : ∀ (s : SelState) (f : FiberId) (c : SelCall)
    (r : SelRes) (s' : SelState) (ws : List FiberId),
    selFinish s f c = some (r, s', ws) →
      ∃ o rest, selConsume s.resolved f = some (o, rest) ∧ selSpecFin o = r := by
  intro s f c r s' ws h
  cases c with
  | sel =>
      simp only [selFinish] at h
      cases hc : selConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          simp at h
          obtain ⟨h1, h2, -⟩ := h
          exact ⟨o, rest, rfl, h1⟩
  | selT =>
      simp only [selFinish] at h
      cases hc : selConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          simp at h
          obtain ⟨h1, h2, -⟩ := h
          exact ⟨o, rest, rfl, h1⟩
  | evSet => simp [selFinish] at h
  | evReset => simp [selFinish] at h

/-! ## Safety: the group discipline

The state machine the sections maintain, as one conjunction:

  * phase/winner agreement -- the committed winner exists exactly while
    the group is `done` (already-won rejection, :153-155);
  * source readiness -- the winner carries the ghost of the source read
    that committed it: an event win saw the latch, a timer win saw the
    due timer (the resolution instant, frozen against later `reset`);
  * armed quiet -- a suspended group never coexists with a set latch:
    every latch-raising section resolves an armed group in the same
    critical section (the atomic-resolution discipline, :21-37 +
    :390-466);
  * the tracked caller is recorded while the group stands;
  * the completion window holds at most one record, and only a `done`
    group holds one -- exactly the committed winner for the tracked
    caller (the one-shot claim CAS, `claim_winner_locked` :164-168). -/
@[reducible] def selSafe (s : SelState) : Prop :=
  (s.phase = SelPhase.done ↔ ∃ r, s.winner = some r) ∧
  (∀ r, s.winner = some r →
    (r = SelRes.wonEv ∧ s.winEvOK = true) ∨ (r = SelRes.wonTim ∧ s.winTimOK = true)) ∧
  (s.phase = SelPhase.armed → s.flag = false ∧ ∃ g, s.caller = some g) ∧
  (s.phase = SelPhase.done → ∃ g, s.caller = some g) ∧
  (s.resolved.length ≤ 1 ∧
    (s.resolved = [] ∨ (s.phase = SelPhase.done ∧ (∃ r, s.winner = some r) ∧
      ∀ p ∈ s.resolved, some p.1 = s.caller ∧ some p.2 = s.winner)))

theorem selAdmit_unchanged {s : SelState} {f : FiberId} {c : SelCall} {s' : SelState}
    (h : selAdmit s f c = some s') : s' = s := by
  simp only [selAdmit, Option.some.injEq] at h
  exact h.symm

/-- On an armed group the winner is still unset. -/
theorem selArmed_winner_none {s : SelState}
    (h1 : s.phase = SelPhase.done ↔ ∃ r, s.winner = some r)
    (harm : s.phase = SelPhase.armed) : s.winner = none := by
  cases hw : s.winner with
  | none => rfl
  | some r =>
      exfalso
      have hdone : s.phase = SelPhase.done := h1.2 ⟨r, hw⟩
      rw [hdone] at harm
      exact absurd harm (by simp [SelPhase.done])

/-- The event-edge resolution preserves the discipline. -/
theorem selResolveEv_preserves {s : SelState} {s2 : SelState} {g : FiberId}
    (h : selResolveEv s = some (s2, g)) (hx : selSafe s) : selSafe s2 := by
  obtain ⟨h1, h2, h3, h4, h5⟩ := hx
  obtain ⟨h5len, h5win⟩ := h5
  obtain ⟨harm, hcs⟩ : s.phase = SelPhase.armed ∧ (∃ f, s.caller = some f) := by
    cases hp : s.phase with
    | idle => simp [selResolveEv, hp] at h
    | done => simp [selResolveEv, hp] at h
    | armed =>
        cases hc : s.caller with
        | none => simp [selResolveEv, hp, hc] at h
        | some f => exact ⟨rfl, f, rfl⟩
  obtain ⟨f, hcf⟩ := hcs
  have hwnone : s.winner = none := selArmed_winner_none h1 harm
  have hre0 : s.resolved = [] := by
    cases hwin : s.resolved with
    | nil => rfl
    | cons p t =>
        exfalso
        rw [hwin] at h5win
        obtain hcons | ⟨hdoneC, _, _⟩ := h5win
        · exact absurd hcons (by simp)
        · exact absurd harm (by rw [hdoneC]; simp)
  simp only [selResolveEv, harm, hcf, hre0, Option.some.injEq, Prod.mk.injEq] at h
  obtain ⟨hseq, -⟩ := h
  subst hseq
  refine ⟨?_, ?_, ?_, ?_, ⟨by simp, Or.inr ⟨rfl, ⟨SelRes.wonEv, rfl⟩, ?_⟩⟩⟩
  · constructor
    · intro _; exact ⟨SelRes.wonEv, rfl⟩
    · intro _; rfl
  · intro r hr
    have hw : Option.some SelRes.wonEv = some r := hr
    simp only [Option.some.injEq] at hw
    subst hw
    exact Or.inl ⟨rfl, rfl⟩
  · intro hcon; cases hcon
  · intro _; exact ⟨f, rfl⟩
  · intro p hp
    simp only [List.nil_append, List.mem_cons, List.not_mem_nil] at hp
    rcases hp with hp | hp
    · rw [hp]; simp
    · exact absurd hp (by simp)

/-- The shared `Event::set` section preserves the discipline. -/
theorem selSetSection_preserves {s : SelState} {r : SelRes} {s2 : SelState}
    {ws : List FiberId}
    (h : selSetSection s = (r, s2, ws)) (hx : selSafe s) : selSafe s2 := by
  simp only [selSetSection] at h
  cases hr : selResolveEv s with
  | none =>
      simp only [hr] at h
      simp only [Prod.mk.injEq, Option.some.injEq] at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      obtain ⟨h1, h2, h3, h4, h5⟩ := hx
      refine ⟨h1, h2, ?_, h4, h5⟩
      intro hcon
      have hparm : s.phase = SelPhase.armed := hcon
      obtain ⟨-, hcb⟩ := h3 hparm
      obtain ⟨f2, hcf2⟩ := hcb
      simp [selResolveEv, hparm, hcf2] at hr
  | some p =>
      obtain ⟨s2', g⟩ := p
      simp only [hr] at h
      simp only [Prod.mk.injEq, Option.some.injEq] at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      exact selResolveEv_preserves hr hx

theorem selRun_preserves {s : SelState} {t : Tick} {f : FiberId} {c : SelCall}
    {r : SelRes} {s' : SelState} {ws : List FiberId}
    (h : selRun s t f c = some (r, s', ws)) (hx : selSafe s) : selSafe s' := by
  cases c with
  | evSet =>
      simp only [selRun, Option.some.injEq] at h
      exact selSetSection_preserves h hx
  | evReset =>
      simp only [selRun, Prod.mk.injEq, Option.some.injEq] at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      obtain ⟨h1, h2, h3, h4, h5⟩ := hx
      refine ⟨h1, h2, ?_, h4, h5⟩
      intro hcon
      exact ⟨rfl, (h3 hcon).2⟩
  | sel =>
      simp only [selRun, Prod.mk.injEq, Option.some.injEq] at h
      by_cases hph : s.phase = SelPhase.idle
      · simp only [hph, if_true] at h
        by_cases hf : s.flag = true
        · simp only [hf, if_true, Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨-, hst, -⟩ := h
          subst hst; exact hx
        · simp only [hf, Bool.false_eq_true, if_false] at h
          by_cases ht : s.timDue = true
          · simp only [ht, if_true, Option.some.injEq, Prod.mk.injEq] at h
            obtain ⟨-, hst, -⟩ := h
            subst hst; exact hx
          · simp only [ht, Bool.false_eq_true, if_false] at h
            simp at h
      · simp only [hph, if_false] at h
        simp at h
  | selT =>
      simp only [selRun, Prod.mk.injEq, Option.some.injEq] at h
      by_cases hph : s.phase = SelPhase.idle
      · simp only [hph, if_true] at h
        by_cases ht : s.timDue = true
        · simp only [ht, if_true, Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨-, hst, -⟩ := h
          subst hst; exact hx
        · simp only [ht, Bool.false_eq_true, if_false] at h
          by_cases hf : s.flag = true
          · simp only [hf, if_true, Option.some.injEq, Prod.mk.injEq] at h
            obtain ⟨-, hst, -⟩ := h
            subst hst; exact hx
          · simp only [hf, Bool.false_eq_true, if_false] at h
            simp at h
      · simp only [hph, if_false] at h
        simp at h

theorem selExtRun_preserves {c : SelCall} {s : SelState} {t : Tick}
    {r : SelRes} {s' : SelState} {ws : List FiberId}
    (h : selExtRun c s t = some (r, s', ws)) (hx : selSafe s) : selSafe s' := by
  cases c with
  | evSet =>
      simp only [selExtRun, Option.some.injEq] at h
      exact selSetSection_preserves h hx
  | evReset =>
      simp only [selExtRun, Prod.mk.injEq, Option.some.injEq] at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      obtain ⟨h1, h2, h3, h4, h5⟩ := hx
      refine ⟨h1, h2, ?_, h4, h5⟩
      intro hcon
      exact ⟨rfl, (h3 hcon).2⟩
  | sel => simp [selExtRun] at h
  | selT => simp [selExtRun] at h

theorem selPark_preserves {s : SelState} {t : Tick} {f : FiberId} {c : SelCall}
    {s' : SelState} {ws : List FiberId}
    (hrun : selRun s t f c = none) (hpark : selPark s f c = some (s', ws))
    (hx : selSafe s) : selSafe s' := by
  obtain ⟨h1, h2, h3, h4, h5⟩ := hx
  obtain ⟨h5len, h5win⟩ := h5
  cases c with
  | evSet => simp [selPark] at hpark
  | evReset => simp [selPark] at hpark
  | sel =>
      simp only [selPark] at hpark
      by_cases hph : s.phase = SelPhase.idle
      · simp only [hph, if_true, Option.some.injEq, Prod.mk.injEq] at hpark
        obtain ⟨hst, -⟩ := hpark
        subst hst
        have hf : s.flag = false := by
          cases hf : s.flag with
          | false => rfl
          | true => simp [selRun, hph, hf] at hrun
        have ht : s.timDue = false := by
          cases ht : s.timDue with
          | false => rfl
          | true => simp [selRun, hph, hf, ht] at hrun
        have hwnone : s.winner = none := by
          cases hw : s.winner with
          | none => rfl
          | some r =>
              exfalso
              have hdone : s.phase = SelPhase.done := h1.2 ⟨r, hw⟩
              rw [hdone] at hph
              exact absurd hph (by simp [SelPhase.idle])
        refine ⟨?_, ?_, ?_, ?_, ⟨h5len, ?_⟩⟩
        · constructor
          · intro hcon; cases hcon
          · intro hcon
            rw [hwnone] at hcon
            exact absurd hcon (by simp)
        · intro r hr
          rw [hwnone] at hr
          exact absurd hr (by simp)
        · intro _; exact ⟨hf, ⟨f, rfl⟩⟩
        · intro hcon; exact absurd hcon (by simp [SelPhase.done])
        · obtain hcons | ⟨hdoneC, _, _⟩ := h5win
          · exact Or.inl hcons
          · rw [hdoneC] at hph
            exact absurd hph (by decide)
      · simp only [hph, if_false, Option.some.injEq, Prod.mk.injEq] at hpark
        obtain ⟨hst, -⟩ := hpark
        subst hst; exact ⟨h1, h2, h3, h4, ⟨h5len, h5win⟩⟩
  | selT =>
      simp only [selPark] at hpark
      by_cases hph : s.phase = SelPhase.idle
      · simp only [hph, if_true, Option.some.injEq, Prod.mk.injEq] at hpark
        obtain ⟨hst, -⟩ := hpark
        subst hst
        have ht : s.timDue = false := by
          cases ht : s.timDue with
          | false => rfl
          | true => simp [selRun, hph, ht] at hrun
        have hf : s.flag = false := by
          cases hf : s.flag with
          | false => rfl
          | true => simp [selRun, hph, ht, hf] at hrun
        have hwnone : s.winner = none := by
          cases hw : s.winner with
          | none => rfl
          | some r =>
              exfalso
              have hdone : s.phase = SelPhase.done := h1.2 ⟨r, hw⟩
              rw [hdone] at hph
              exact absurd hph (by simp [SelPhase.idle])
        refine ⟨?_, ?_, ?_, ?_, ⟨h5len, ?_⟩⟩
        · constructor
          · intro hcon; cases hcon
          · intro hcon
            rw [hwnone] at hcon
            exact absurd hcon (by simp)
        · intro r hr
          rw [hwnone] at hr
          exact absurd hr (by simp)
        · intro _; exact ⟨hf, ⟨f, rfl⟩⟩
        · intro hcon; exact absurd hcon (by simp [SelPhase.done])
        · obtain hcons | ⟨hdoneC, _, _⟩ := h5win
          · exact Or.inl hcons
          · rw [hdoneC] at hph
            exact absurd hph (by decide)
      · simp only [hph, if_false, Option.some.injEq, Prod.mk.injEq] at hpark
        obtain ⟨hst, -⟩ := hpark
        subst hst; exact ⟨h1, h2, h3, h4, ⟨h5len, h5win⟩⟩

theorem selFinish_preserves {s : SelState} {f : FiberId} {c : SelCall}
    {r : SelRes} {s' : SelState} {ws : List FiberId}
    (h : selFinish s f c = some (r, s', ws)) (hx : selSafe s) : selSafe s' := by
  obtain ⟨h1, h2, h3, h4, h5⟩ := hx
  obtain ⟨h5len, h5win⟩ := h5
  cases c with
  | sel =>
      simp only [selFinish, Option.some.injEq] at h
      cases hc : selConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc, Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨-, hst, -⟩ := h
          subst hst
          have hrest0 : rest = [] := by
            cases hp : s.resolved with
            | nil => simp [selConsume, hp] at hc
            | cons q t =>
                cases t with
                | nil =>
                    rw [hp] at hc
                    simp only [selConsume] at hc
                    by_cases hqf : q.1 = f
                    · simp only [hqf, if_true, Option.some.injEq, Prod.mk.injEq] at hc
                      obtain ⟨-, hrest⟩ := hc
                      exact hrest.symm
                    · simp only [hqf, Bool.false_eq_true, if_false] at hc
                      simp at hc
                | cons u t2 =>
                    exfalso
                    rw [hp] at h5len
                    simp at h5len
          refine ⟨?_, ?_, ?_, ?_, ⟨by rw [hrest0]; simp, Or.inl hrest0⟩⟩
          · constructor
            · intro hcon; simp at hcon
            · intro hcon; simp at hcon
          · intro r0 hr0; exact absurd hr0 (by simp)
          · intro hcon; exact absurd hcon (by simp [SelPhase.done])
          · intro hcon; exact absurd hcon (by simp [SelPhase.done])
  | selT =>
      simp only [selFinish, Option.some.injEq] at h
      cases hc : selConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc, Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨-, hst, -⟩ := h
          subst hst
          have hrest0 : rest = [] := by
            cases hp : s.resolved with
            | nil => simp [selConsume, hp] at hc
            | cons q t =>
                cases t with
                | nil =>
                    rw [hp] at hc
                    simp only [selConsume] at hc
                    by_cases hqf : q.1 = f
                    · simp only [hqf, if_true, Option.some.injEq, Prod.mk.injEq] at hc
                      obtain ⟨-, hrest⟩ := hc
                      exact hrest.symm
                    · simp only [hqf, Bool.false_eq_true, if_false] at hc
                      simp at hc
                | cons u t2 =>
                    exfalso
                    rw [hp] at h5len
                    simp at h5len
          refine ⟨?_, ?_, ?_, ?_, ⟨by rw [hrest0]; simp, Or.inl hrest0⟩⟩
          · constructor
            · intro hcon; simp at hcon
            · intro hcon; simp at hcon
          · intro r0 hr0; exact absurd hr0 (by simp)
          · intro hcon; exact absurd hcon (by simp [SelPhase.done])
          · intro hcon; exact absurd hcon (by simp [SelPhase.done])
  | evSet => simp [selFinish] at h
  | evReset => simp [selFinish] at h

theorem selOnTick_preserves {s : SelState} {t : Tick} (hx : selSafe s) :
    selSafe (selOnTick s t) := hx

theorem selExpire_preserves {s : SelState} {t : Tick} {f : FiberId} {s' : SelState}
    (h : selExpire s t f = some s') (hx : selSafe s) : selSafe s' := by
  obtain ⟨h1, h2, h3, h4, h5⟩ := hx
  obtain ⟨h5len, h5win⟩ := h5
  obtain ⟨harm, hcs⟩ : s.phase = SelPhase.armed ∧ (∃ g, s.caller = some g) := by
    cases hp : s.phase with
    | idle => simp [selExpire, hp] at h
    | done => simp [selExpire, hp] at h
    | armed =>
        cases hc : s.caller with
        | none => simp [selExpire, hp, hc] at h
        | some g => exact ⟨rfl, g, rfl⟩
  obtain ⟨g, hcg⟩ := hcs
  have hwnone : s.winner = none := selArmed_winner_none h1 harm
  have hre0 : s.resolved = [] := by
    cases hwin : s.resolved with
    | nil => rfl
    | cons p t2 =>
        exfalso
        rw [hwin] at h5win
        obtain hcons | ⟨hdoneC, _, _⟩ := h5win
        · exact absurd hcons (by simp)
        · exact absurd harm (by rw [hdoneC]; simp)
  simp only [selExpire, harm, hcg, hre0] at h
  by_cases hcond : g = f ∧ s.timDue = true
  · rw [if_pos hcond] at h
    obtain ⟨hgf, hdue⟩ := hcond
    subst hgf
    simp only [Option.some.injEq] at h
    subst h
    refine ⟨?_, ?_, ?_, ?_, ⟨by simp, Or.inr ⟨rfl, ⟨SelRes.wonTim, rfl⟩, ?_⟩⟩⟩
    · constructor
      · intro _; exact ⟨SelRes.wonTim, rfl⟩
      · intro _; rfl
    · intro r hr
      have hw : Option.some SelRes.wonTim = some r := hr
      simp only [Option.some.injEq] at hw
      subst hw
      exact Or.inr ⟨rfl, rfl⟩
    · intro hcon; cases hcon
    · intro _; exact ⟨g, rfl⟩
    · intro p hp
      simp only [List.nil_append, List.mem_cons, List.not_mem_nil] at hp
      rcases hp with hp | hp
      · rw [hp]; simp
      · exact absurd hp (by simp)
  · rw [if_neg hcond] at h
    simp at h

/-- The primitive never breaks the discipline: at every reachable
configuration the group machine is coherent -- the winner exists exactly
while the group is resolved, every win carries its source-read ghost, a
suspended group never coexists with a set latch, and the completion
window holds at most the one committed record. -/
theorem sel_safe {cfg m : PrimCfg SelSig selPrim} {t : Trace SelSig}
    (hrun : PrimRuns2 SelSig selPrim cfg t m) :
    selSafe cfg.prim → selSafe m.prim := by
  induction hrun with
  | stop cfg => exact fun h => h
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro h
      refine ih ?_
      cases hstep with
      | submit f c hsub => exact h
      | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
          have hss : s' = cfg.prim :=
            selAdmit_unchanged (s := cfg.prim) (f := rp.fiber) (c := rp.call) hadmit
          rw [hss]
          exact h
      | dispatchResumed rp rest hcurE hrunqE hfreshE => exact h
      | fiberEffect d b ps rest r s' wk hcurE hb hrunE hmap hwake =>
          exact selRun_preserves hrunE h
      | fiberDone d r hcurE => exact h
      | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
          exact selPark_preserves hrunE hparkE h
      | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
          exact selFinish_preserves hfinE h
      | extApply x c preE postE hcap hnovel => exact h
      | extEffect preE postE e ps rest r s' wk hextsE hresE hrunE hmap hwake =>
          exact selExtRun_preserves hrunE h
      | extDone preE postE e r hextsE hresE => exact h
      | envTime t0 hcurE hle => exact selOnTick_preserves (t := t0) h
      | envExpire f s' preP postP p hcurE hexp hparE hf =>
          exact selExpire_preserves (s := cfg.prim) (t := cfg.now) hexp h

/-- `primInit`'s state satisfies the discipline, so every run endpoint
does. -/
theorem sel_safe_holds {fin : PrimCfg SelSig selPrim} {t : Trace SelSig}
    (hrun : PrimRuns2 SelSig selPrim (primInit SelSig selPrim) t fin) :
    selSafe fin.prim := by
  have hi : selSafe selInit := by
    refine ⟨?_, ?_, ?_, ?_, ?_⟩
    · constructor
      · intro h; cases h
      · intro h
        obtain ⟨r, hr⟩ := h
        cases hr
    · intro r hr; cases hr
    · intro h; cases h
    · intro h; cases h
    · exact ⟨by decide, Or.inl rfl⟩
  exact sel_safe hrun hi



/-! ## Scenario batteries (reachability witnesses)

Ten possession chains from `primInit`, one per externally meaningful
behavior: inline event win, inline timer win, event-edge hand-off,
timer-pump hand-off, both-ready priority (both orders), reset
blindness, the fiber-path `evSet` resolution, re-arm with the latch
still set (level semantics), and the no-spurious-resolution silence.
-/

abbrev ss0 : SelState := selInit

/-- Battery A — the inline event cycle: a fiber sets the latch, then a
`sel` issued with the latch set wins inline. -/
abbrev sa0 : PrimCfg SelSig selPrim := primInit SelSig selPrim
abbrev sa1 : PrimCfg SelSig selPrim :=
  { prim := ss0, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.evSet, fresh := true }],
    retired := ([] : List FiberId), nextFiber := 1, exts := [] }
abbrev sa2 : PrimCfg SelSig selPrim :=
  { prim := ss0, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SelCall.evSet } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev ssa3 : SelState := { ss0 with flag := true }
abbrev sa3 : PrimCfg SelSig selPrim :=
  { prim := ssa3, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SelCall.evSet } SelRes.rEvSet),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sa4 : PrimCfg SelSig selPrim :=
  { prim := ssa3, now := 0, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }
abbrev sa5 : PrimCfg SelSig selPrim :=
  { prim := ssa3, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }
abbrev sa6 : PrimCfg SelSig selPrim :=
  { prim := ssa3, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SelCall.sel } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sa7 : PrimCfg SelSig selPrim :=
  { prim := ssa3, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SelCall.sel } SelRes.wonEv),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sa8 : PrimCfg SelSig selPrim :=
  { prim := ssa3, now := 0, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }

theorem sad1 : PrimStep2 SelSig selPrim sa0 none sa1 :=
  PrimStep2.submit sa0 0 SelCall.evSet (Or.inl rfl)
theorem sad2 : PrimStep2 SelSig selPrim sa1
    (some (issueObs SelSig (Caller.fiber 0) SelCall.evSet)) sa2 := by
  refine PrimStep2.dispatchFresh sa1
    { fiber := 0, call := SelCall.evSet, fresh := true } [] ss0 ?_ ?_ ?_ ?_
  all_goals rfl
theorem sad3 : PrimStep2 SelSig selPrim sa2 none sa3 :=
  PrimStep2.fiberEffect sa2 { fiber := 0, call := SelCall.evSet } false [] []
    SelRes.rEvSet ssa3 [] rfl rfl rfl rfl Wakes.nil
theorem sad4 : PrimStep2 SelSig selPrim sa3
    (some (compObs SelSig (Caller.fiber 0) SelCall.evSet SelRes.rEvSet)) sa4 :=
  PrimStep2.fiberDone sa3 { fiber := 0, call := SelCall.evSet } SelRes.rEvSet rfl
theorem sad5 : PrimStep2 SelSig selPrim sa4 none sa5 :=
  PrimStep2.submit sa4 0 SelCall.sel (Or.inr (by simp))
theorem sad6 : PrimStep2 SelSig selPrim sa5
    (some (issueObs SelSig (Caller.fiber 0) SelCall.sel)) sa6 := by
  refine PrimStep2.dispatchFresh sa5
    { fiber := 0, call := SelCall.sel, fresh := true } [] ssa3 ?_ ?_ ?_ ?_
  all_goals rfl
theorem sad7 : PrimStep2 SelSig selPrim sa6 none sa7 :=
  PrimStep2.fiberEffect sa6 { fiber := 0, call := SelCall.sel } false [] []
    SelRes.wonEv ssa3 [] rfl rfl rfl rfl Wakes.nil
theorem sad8 : PrimStep2 SelSig selPrim sa7
    (some (compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv)) sa8 :=
  PrimStep2.fiberDone sa7 { fiber := 0, call := SelCall.sel } SelRes.wonEv rfl

theorem sel_battery_inlineEv :
    ∃ fin : PrimCfg SelSig selPrim,
      PrimRuns2 SelSig selPrim sa0
        [issueObs SelSig (Caller.fiber 0) SelCall.evSet,
         compObs SelSig (Caller.fiber 0) SelCall.evSet SelRes.rEvSet,
         issueObs SelSig (Caller.fiber 0) SelCall.sel,
         compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv]
        fin ∧
      fin.prim.flag = true ∧ fin.prim.phase = SelPhase.idle ∧
        fin.prim.winner = none := by
  refine ⟨sa8, ?_, rfl, rfl, rfl⟩
  exact PrimRuns2.step sa0 sa1 none _ sa8 sad1
    (PrimRuns2.step sa1 sa2 _ _ sa8 sad2
      (PrimRuns2.step sa2 sa3 _ _ sa8 sad3
        (PrimRuns2.step sa3 sa4 _ _ sa8 sad4
          (PrimRuns2.step sa4 sa5 _ _ sa8 sad5
            (PrimRuns2.step sa5 sa6 _ _ sa8 sad6
              (PrimRuns2.step sa6 sa7 _ _ sa8 sad7
                (PrimRuns2.step sa7 sa8 _ _ sa8 sad8 (PrimRuns2.stop sa8))))))))

/-- Battery B — the inline timer win: time passes, then `selT` issued
with the timer due wins inline. -/
abbrev sb1 : PrimCfg SelSig selPrim :=
  { prim := { ss0 with timDue := true }, now := 1, cur := none, parked := [],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 0, exts := [] }
abbrev sb2 : PrimCfg SelSig selPrim :=
  { prim := { ss0 with timDue := true }, now := 1, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.selT, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }
abbrev sb3 : PrimCfg SelSig selPrim :=
  { prim := { ss0 with timDue := true }, now := 1,
    cur := some (FSlot.running { fiber := 0, call := SelCall.selT } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev ssb4 : SelState := { ss0 with timDue := true }
abbrev sb4 : PrimCfg SelSig selPrim :=
  { prim := ssb4, now := 1,
    cur := some (FSlot.returning { fiber := 0, call := SelCall.selT } SelRes.wonTim),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sb5 : PrimCfg SelSig selPrim :=
  { prim := ssb4, now := 1, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }

theorem sbd1 : PrimStep2 SelSig selPrim sa0 none sb1 :=
  PrimStep2.envTime sa0 1 rfl (by decide)
theorem sbd2 : PrimStep2 SelSig selPrim sb1 none sb2 :=
  PrimStep2.submit sb1 0 SelCall.selT (Or.inl rfl)
theorem sbd3 : PrimStep2 SelSig selPrim sb2
    (some (issueObs SelSig (Caller.fiber 0) SelCall.selT)) sb3 := by
  refine PrimStep2.dispatchFresh sb2
    { fiber := 0, call := SelCall.selT, fresh := true } []
    { ss0 with timDue := true } ?_ ?_ ?_ ?_
  all_goals rfl
theorem sbd4 : PrimStep2 SelSig selPrim sb3 none sb4 :=
  PrimStep2.fiberEffect sb3 { fiber := 0, call := SelCall.selT } false [] []
    SelRes.wonTim ssb4 [] rfl rfl rfl rfl Wakes.nil
theorem sbd5 : PrimStep2 SelSig selPrim sb4
    (some (compObs SelSig (Caller.fiber 0) SelCall.selT SelRes.wonTim)) sb5 :=
  PrimStep2.fiberDone sb4 { fiber := 0, call := SelCall.selT } SelRes.wonTim rfl

theorem sel_battery_inlineTim :
    ∃ fin : PrimCfg SelSig selPrim,
      PrimRuns2 SelSig selPrim sa0
        [issueObs SelSig (Caller.fiber 0) SelCall.selT,
         compObs SelSig (Caller.fiber 0) SelCall.selT SelRes.wonTim]
        fin ∧
      fin.prim.timDue = true ∧ fin.prim.phase = SelPhase.idle := by
  refine ⟨sb5, ?_, rfl, rfl⟩
  exact PrimRuns2.step sa0 sb1 none _ sb5 sbd1
    (PrimRuns2.step sb1 sb2 _ _ sb5 sbd2
      (PrimRuns2.step sb2 sb3 _ _ sb5 sbd3
        (PrimRuns2.step sb3 sb4 _ _ sb5 sbd4
          (PrimRuns2.step sb4 sb5 _ _ sb5 sbd5 (PrimRuns2.stop sb5)))))

/-- Battery C — the event-edge hand-off: `sel` suspends on a quiet
group, an external `set` resolves it in the same critical section, and
the resumed caller reports `wonEv`. -/
abbrev ssC3 : SelState := { ss0 with phase := SelPhase.armed, order := true, caller := some 0 }
abbrev sc3 : PrimCfg SelSig selPrim :=
  { prim := ssC3, now := 0, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 1, exts := [] }
abbrev sc4 : PrimCfg SelSig selPrim :=
  { prim := ssC3, now := 0, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 1, exts := [{ x := 0, call := SelCall.evSet, result := none }] }
abbrev ssC5 : SelState :=
  { flag := true, timDue := false, phase := SelPhase.done,
    winner := some SelRes.wonEv, order := true, caller := some 0,
    winEvOK := true, winTimOK := false, resolved := [(0, SelRes.wonEv)] }
abbrev sc5 : PrimCfg SelSig selPrim :=
  { prim := ssC5, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := false }],
    retired := ([] : List FiberId), nextFiber := 1,
    exts := [{ x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }] }
abbrev sc6 : PrimCfg SelSig selPrim :=
  { prim := ssC5, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := false }],
    retired := ([] : List FiberId), nextFiber := 1, exts := [] }
abbrev sc7 : PrimCfg SelSig selPrim :=
  { prim := ssC5, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SelCall.sel } true),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev ssC8 : SelState :=
  { flag := true, timDue := false, phase := SelPhase.idle, winner := none,
    order := true, caller := none, winEvOK := false, winTimOK := false,
    resolved := [] }
abbrev sc8 : PrimCfg SelSig selPrim :=
  { prim := ssC8, now := 0, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }

abbrev sc1 : PrimCfg SelSig selPrim :=
  { prim := ss0, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := true }],
    retired := ([] : List FiberId), nextFiber := 1, exts := [] }
abbrev sc2 : PrimCfg SelSig selPrim :=
  { prim := ss0, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SelCall.sel } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

theorem scd1 : PrimStep2 SelSig selPrim sa0 none sc1 :=
  PrimStep2.submit sa0 0 SelCall.sel (Or.inl rfl)
theorem scd2 : PrimStep2 SelSig selPrim sc1
    (some (issueObs SelSig (Caller.fiber 0) SelCall.sel)) sc2 :=
  PrimStep2.dispatchFresh sc1
    { fiber := 0, call := SelCall.sel, fresh := true } [] ss0 rfl rfl rfl rfl
theorem scd3 : PrimStep2 SelSig selPrim sc2 none sc3 :=
  PrimStep2.runPark sc2 { fiber := 0, call := SelCall.sel } false [] []
    ssC3 [] rfl rfl rfl rfl Wakes.nil
theorem scd4 : PrimStep2 SelSig selPrim sc3
    (some (issueObs SelSig (Caller.ext 0) SelCall.evSet)) sc4 :=
  PrimStep2.extApply sc3 0 SelCall.evSet [] [] rfl (by simp)
theorem scd5 : PrimStep2 SelSig selPrim sc4 none sc5 := by
  refine PrimStep2.extEffect sc4 [] []
    { x := 0, call := SelCall.evSet, result := none }
    [{ fiber := 0, call := SelCall.sel }] [] SelRes.rEvSet ssC5 [0] ?_ ?_ ?_ ?_ ?_
  · rfl
  · rfl
  · rfl
  · rfl
  · exact Wakes.drop (p := ({ fiber := 0, call := SelCall.sel } : Pnd SelSig)) Wakes.nil
theorem scd6 : PrimStep2 SelSig selPrim sc5
    (some (compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet)) sc6 :=
  PrimStep2.extDone sc5 [] []
    { x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }
    SelRes.rEvSet rfl rfl
theorem scd7 : PrimStep2 SelSig selPrim sc6 none sc7 :=
  PrimStep2.dispatchResumed sc6 { fiber := 0, call := SelCall.sel, fresh := false } [] rfl rfl rfl
theorem scd8 : PrimStep2 SelSig selPrim sc7
    (some (compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv)) sc8 :=
  PrimStep2.finishDone sc7 { fiber := 0, call := SelCall.sel } true [] []
    SelRes.wonEv ssC8 [] rfl rfl rfl rfl Wakes.nil

theorem seqOK_selHandoffEv : SeqOK SelSig
    [issueObs SelSig (Caller.fiber 0) SelCall.sel,
     issueObs SelSig (Caller.ext 0) SelCall.evSet,
     compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
     compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem sel_battery_handoffEv :
    TracesPrimS SelSig selPrim
      [issueObs SelSig (Caller.fiber 0) SelCall.sel,
       issueObs SelSig (Caller.ext 0) SelCall.evSet,
       compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
       compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine ⟨⟨sc8, PrimRuns2.step sa0 sc1 none _ sc8 scd1
    (PrimRuns2.step sc1 sc2 _ _ sc8 scd2
      (PrimRuns2.step sc2 sc3 _ _ sc8 scd3
        (PrimRuns2.step sc3 sc4 _ _ sc8 scd4
          (PrimRuns2.step sc4 sc5 _ _ sc8 scd5
            (PrimRuns2.step sc5 sc6 _ _ sc8 scd6
              (PrimRuns2.step sc6 sc7 _ _ sc8 scd7
                (PrimRuns2.step sc7 sc8 _ _ sc8 scd8 (PrimRuns2.stop sc8))))))))⟩,
    seqOK_selHandoffEv⟩

/-- Battery D — the timer-pump hand-off: `sel` suspends, time passes,
the pump resolves the armed group, the resumed caller reports `wonTim`. -/
abbrev ssD5 : SelState :=
  { flag := false, timDue := true, phase := SelPhase.done,
    winner := some SelRes.wonTim, order := true, caller := some 0,
    winEvOK := false, winTimOK := true, resolved := [(0, SelRes.wonTim)] }
abbrev sd4 : PrimCfg SelSig selPrim :=
  { prim := { ssC3 with timDue := true }, now := 1, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 1, exts := [] }
abbrev sd5 : PrimCfg SelSig selPrim :=
  { prim := ssD5, now := 1, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := false }],
    retired := ([] : List FiberId), nextFiber := 1, exts := [] }
abbrev sd6 : PrimCfg SelSig selPrim :=
  { prim := ssD5, now := 1,
    cur := some (FSlot.running { fiber := 0, call := SelCall.sel } true),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev ssD7 : SelState :=
  { flag := false, timDue := true, phase := SelPhase.idle, winner := none,
    order := true, caller := none, winEvOK := false, winTimOK := false,
    resolved := [] }
abbrev sd7 : PrimCfg SelSig selPrim :=
  { prim := ssD7, now := 1, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }

theorem sdd1 : PrimStep2 SelSig selPrim sc3 none sd4 :=
  PrimStep2.envTime sc3 1 rfl (by decide)
theorem sdd2 : PrimStep2 SelSig selPrim sd4 none sd5 := by
  refine PrimStep2.envExpire sd4 0 ssD5 [] [] { fiber := 0, call := SelCall.sel }
    ?_ ?_ ?_ ?_
  all_goals rfl
theorem sdd3 : PrimStep2 SelSig selPrim sd5 none sd6 :=
  PrimStep2.dispatchResumed sd5 { fiber := 0, call := SelCall.sel, fresh := false } [] rfl rfl rfl
theorem sdd4 : PrimStep2 SelSig selPrim sd6
    (some (compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonTim)) sd7 :=
  PrimStep2.finishDone sd6 { fiber := 0, call := SelCall.sel } true [] []
    SelRes.wonTim ssD7 [] rfl rfl rfl rfl Wakes.nil

theorem seqOK_selHandoffTim : SeqOK SelSig
    [issueObs SelSig (Caller.fiber 0) SelCall.sel,
     compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonTim] := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem sel_battery_handoffTim :
    TracesPrimS SelSig selPrim
      [issueObs SelSig (Caller.fiber 0) SelCall.sel,
       compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonTim] := by
  refine ⟨⟨sd7, PrimRuns2.step sa0 sc1 none _ sd7 scd1
    (PrimRuns2.step sc1 sc2 _ _ sd7 scd2
      (PrimRuns2.step sc2 sc3 _ _ sd7 scd3
        (PrimRuns2.step sc3 sd4 _ _ sd7 sdd1
          (PrimRuns2.step sd4 sd5 _ _ sd7 sdd2
            (PrimRuns2.step sd5 sd6 _ _ sd7 sdd3
              (PrimRuns2.step sd6 sd7 _ _ sd7 sdd4 (PrimRuns2.stop sd7)))))))⟩,
    seqOK_selHandoffTim⟩

/-- Battery E — both sources ready, event arm first: the admission scan
picks the event arm (`wonEv`). -/
abbrev ssE1 : SelState := { ss0 with flag := true }
abbrev seA1 : PrimCfg SelSig selPrim :=
  { prim := ss0, now := 0, cur := none, parked := [],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 0, exts := [{ x := 0, call := SelCall.evSet, result := none }] }
abbrev seP1 : PrimCfg SelSig selPrim :=
  { prim := ssE1, now := 0, cur := none, parked := [],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 0, exts := [{ x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }] }
abbrev seP2 : PrimCfg SelSig selPrim :=
  { prim := ssE1, now := 0, cur := none, parked := [],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 0, exts := [] }
abbrev ssE0 : SelState := selOnTick ssE1 1
abbrev se0 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1, cur := none, parked := [],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 0, exts := [] }
abbrev se1 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }
abbrev se2 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1,
    cur := some (FSlot.running { fiber := 0, call := SelCall.sel } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev se3 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1,
    cur := some (FSlot.returning { fiber := 0, call := SelCall.sel } SelRes.wonEv),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev se4 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }

theorem seP0s : PrimStep2 SelSig selPrim sa0
    (some (issueObs SelSig (Caller.ext 0) SelCall.evSet)) seA1 := by
  refine PrimStep2.extApply sa0 0 SelCall.evSet [] [] rfl ?_
  intro h
  cases h
theorem seP1s : PrimStep2 SelSig selPrim seA1 none seP1 := by
  refine PrimStep2.extEffect seA1 [] []
    { x := 0, call := SelCall.evSet, result := none }
    [] [] SelRes.rEvSet ssE1 [] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.nil
theorem seP2s : PrimStep2 SelSig selPrim seP1
    (some (compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet)) seP2 :=
  PrimStep2.extDone seP1 [] []
    { x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }
    SelRes.rEvSet rfl rfl
theorem seP3s : PrimStep2 SelSig selPrim seP2 none se0 := by
  refine PrimStep2.envTime seP2 1 ?_ (by decide)
  · rfl
theorem sed1 : PrimStep2 SelSig selPrim se0 none se1 :=
  PrimStep2.submit se0 0 SelCall.sel (Or.inl rfl)
theorem sed3 : PrimStep2 SelSig selPrim se1
    (some (issueObs SelSig (Caller.fiber 0) SelCall.sel)) se2 := by
  refine PrimStep2.dispatchFresh se1
    { fiber := 0, call := SelCall.sel, fresh := true } [] ssE0 ?_ ?_ ?_ ?_
  all_goals rfl
theorem sed4 : PrimStep2 SelSig selPrim se2 none se3 :=
  PrimStep2.fiberEffect se2 { fiber := 0, call := SelCall.sel } false [] []
    SelRes.wonEv ssE0 [] rfl rfl rfl rfl Wakes.nil
theorem sed5 : PrimStep2 SelSig selPrim se3
    (some (compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv)) se4 :=
  PrimStep2.fiberDone se3 { fiber := 0, call := SelCall.sel } SelRes.wonEv rfl

theorem seqOK_selPrioEv : SeqOK SelSig
    [issueObs SelSig (Caller.ext 0) SelCall.evSet,
     compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
     issueObs SelSig (Caller.fiber 0) SelCall.sel,
     compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem sel_battery_prioEv :
    TracesPrimS SelSig selPrim
      [issueObs SelSig (Caller.ext 0) SelCall.evSet,
       compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
       issueObs SelSig (Caller.fiber 0) SelCall.sel,
       compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine ⟨⟨se4, PrimRuns2.step sa0 seA1 _ _ se4 seP0s
    (PrimRuns2.step seA1 seP1 _ _ se4 seP1s
      (PrimRuns2.step seP1 seP2 _ _ se4 seP2s
        (PrimRuns2.step seP2 se0 _ _ se4 seP3s
          (PrimRuns2.step se0 se1 _ _ se4 sed1
            (PrimRuns2.step se1 se2 _ _ se4 sed3
              (PrimRuns2.step se2 se3 _ _ se4 sed4
                (PrimRuns2.step se3 se4 _ _ se4 sed5 (PrimRuns2.stop se4))))))))⟩,
    seqOK_selPrioEv⟩

/-- Battery F — both sources ready, timer arm first: the scan picks the
timer arm (`wonTim`). -/
abbrev se5 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1,
    cur := some (FSlot.running { fiber := 0, call := SelCall.selT } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev se6 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1,
    cur := some (FSlot.returning { fiber := 0, call := SelCall.selT } SelRes.wonTim),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev se7 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }

abbrev sf1 : PrimCfg SelSig selPrim :=
  { prim := ssE0, now := 1, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.selT, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }
theorem sfd1 : PrimStep2 SelSig selPrim se0 none sf1 :=
  PrimStep2.submit se0 0 SelCall.selT (Or.inl rfl)
theorem sfd2 : PrimStep2 SelSig selPrim sf1
    (some (issueObs SelSig (Caller.fiber 0) SelCall.selT)) se5 :=
  PrimStep2.dispatchFresh sf1
    { fiber := 0, call := SelCall.selT, fresh := true } [] ssE0 rfl rfl rfl rfl
theorem sfd3 : PrimStep2 SelSig selPrim se5 none se6 :=
  PrimStep2.fiberEffect se5 { fiber := 0, call := SelCall.selT } false [] []
    SelRes.wonTim ssE0 [] rfl rfl rfl rfl Wakes.nil
theorem sfd4 : PrimStep2 SelSig selPrim se6
    (some (compObs SelSig (Caller.fiber 0) SelCall.selT SelRes.wonTim)) se7 :=
  PrimStep2.fiberDone se6 { fiber := 0, call := SelCall.selT } SelRes.wonTim rfl

theorem seqOK_selPrioTim : SeqOK SelSig
    [issueObs SelSig (Caller.ext 0) SelCall.evSet,
     compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
     issueObs SelSig (Caller.fiber 0) SelCall.selT,
     compObs SelSig (Caller.fiber 0) SelCall.selT SelRes.wonTim] := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem sel_battery_prioTim :
    TracesPrimS SelSig selPrim
      [issueObs SelSig (Caller.ext 0) SelCall.evSet,
       compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
       issueObs SelSig (Caller.fiber 0) SelCall.selT,
       compObs SelSig (Caller.fiber 0) SelCall.selT SelRes.wonTim] := by
  refine ⟨⟨se7, PrimRuns2.step sa0 seA1 _ _ se7 seP0s
    (PrimRuns2.step seA1 seP1 _ _ se7 seP1s
      (PrimRuns2.step seP1 seP2 _ _ se7 seP2s
        (PrimRuns2.step seP2 se0 _ _ se7 seP3s
          (PrimRuns2.step se0 sf1 _ _ se7 sfd1
            (PrimRuns2.step sf1 se5 _ _ se7 sfd2
              (PrimRuns2.step se5 se6 _ _ se7 sfd3
                (PrimRuns2.step se6 se7 _ _ se7 sfd4 (PrimRuns2.stop se7))))))))⟩,
    seqOK_selPrioTim⟩

/-- Battery G — reset blindness: `evReset` while a group is armed does
not resolve it; the later `evSet` does. -/
abbrev ssG4 : SelState := { ssC3 with flag := false }
abbrev sg3 : PrimCfg SelSig selPrim :=
  { prim := ssC3, now := 0, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 1, exts := [{ x := 0, call := SelCall.evReset, result := none }] }
abbrev sg4 : PrimCfg SelSig selPrim :=
  { prim := ssG4, now := 0, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 1, exts := [{ x := 0, call := SelCall.evReset, result := some SelRes.rEvReset }] }
abbrev sg5 : PrimCfg SelSig selPrim :=
  { prim := ssG4, now := 0, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 1, exts := [] }
abbrev sg6 : PrimCfg SelSig selPrim :=
  { prim := ssG4, now := 0, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId),
    nextFiber := 1, exts := [{ x := 0, call := SelCall.evSet, result := none }] }
abbrev ssG7 : SelState :=
  { flag := true, timDue := false, phase := SelPhase.done,
    winner := some SelRes.wonEv, order := true, caller := some 0,
    winEvOK := true, winTimOK := false, resolved := [(0, SelRes.wonEv)] }
abbrev sg7 : PrimCfg SelSig selPrim :=
  { prim := ssG7, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := false }],
    retired := ([] : List FiberId), nextFiber := 1,
    exts := [{ x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }] }
abbrev sg8 : PrimCfg SelSig selPrim :=
  { prim := ssG7, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := false }],
    retired := ([] : List FiberId), nextFiber := 1, exts := [] }
abbrev sg9 : PrimCfg SelSig selPrim :=
  { prim := ssG7, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SelCall.sel } true),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sg10 : PrimCfg SelSig selPrim :=
  { prim := ssC8, now := 0, cur := none, parked := [], runq := [],
    retired := [0], nextFiber := 1, exts := [] }

theorem sgd1 : PrimStep2 SelSig selPrim sc3
    (some (issueObs SelSig (Caller.ext 0) SelCall.evReset)) sg3 := by
  refine PrimStep2.extApply sc3 0 SelCall.evReset [] [] rfl ?_
  simp
theorem sgd2 : PrimStep2 SelSig selPrim sg3 none sg4 :=
  PrimStep2.extEffect sg3 [] []
    { x := 0, call := SelCall.evReset, result := none }
    [] [{ fiber := 0, call := SelCall.sel }] SelRes.rEvReset ssG4 [] rfl rfl rfl rfl
    (Wakes.keep (p := ({ fiber := 0, call := SelCall.sel } : Pnd SelSig)) Wakes.nil)
theorem sgd3 : PrimStep2 SelSig selPrim sg4
    (some (compObs SelSig (Caller.ext 0) SelCall.evReset SelRes.rEvReset)) sg5 :=
  PrimStep2.extDone sg4 [] []
    { x := 0, call := SelCall.evReset, result := some SelRes.rEvReset }
    SelRes.rEvReset rfl rfl
theorem sgd4 : PrimStep2 SelSig selPrim sg5
    (some (issueObs SelSig (Caller.ext 0) SelCall.evSet)) sg6 := by
  refine PrimStep2.extApply sg5 0 SelCall.evSet [] [] rfl ?_
  simp
theorem sgd5 : PrimStep2 SelSig selPrim sg6 none sg7 := by
  refine PrimStep2.extEffect sg6 [] []
    { x := 0, call := SelCall.evSet, result := none }
    [{ fiber := 0, call := SelCall.sel }] [] SelRes.rEvSet ssG7 [0] ?_ ?_ ?_ ?_ ?_
  · rfl
  · rfl
  · rfl
  · rfl
  · exact Wakes.drop (p := ({ fiber := 0, call := SelCall.sel } : Pnd SelSig)) Wakes.nil
theorem sgd6 : PrimStep2 SelSig selPrim sg7
    (some (compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet)) sg8 :=
  PrimStep2.extDone sg7 [] []
    { x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }
    SelRes.rEvSet rfl rfl
theorem sgd7 : PrimStep2 SelSig selPrim sg8 none sg9 :=
  PrimStep2.dispatchResumed sg8 { fiber := 0, call := SelCall.sel, fresh := false } [] rfl rfl rfl
theorem sgd8 : PrimStep2 SelSig selPrim sg9
    (some (compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv)) sg10 :=
  PrimStep2.finishDone sg9 { fiber := 0, call := SelCall.sel } true [] []
    SelRes.wonEv ssC8 [] rfl rfl rfl rfl Wakes.nil

theorem seqOK_selResetBlind : SeqOK SelSig
    [issueObs SelSig (Caller.fiber 0) SelCall.sel,
     issueObs SelSig (Caller.ext 0) SelCall.evReset,
     compObs SelSig (Caller.ext 0) SelCall.evReset SelRes.rEvReset,
     issueObs SelSig (Caller.ext 0) SelCall.evSet,
     compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
     compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem sel_battery_resetBlind :
    TracesPrimS SelSig selPrim
      [issueObs SelSig (Caller.fiber 0) SelCall.sel,
       issueObs SelSig (Caller.ext 0) SelCall.evReset,
       compObs SelSig (Caller.ext 0) SelCall.evReset SelRes.rEvReset,
       issueObs SelSig (Caller.ext 0) SelCall.evSet,
       compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
       compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine ⟨⟨sg10, PrimRuns2.step sa0 sc1 none _ sg10 scd1
    (PrimRuns2.step sc1 sc2 _ _ sg10 scd2
      (PrimRuns2.step sc2 sc3 _ _ sg10 scd3
        (PrimRuns2.step sc3 sg3 _ _ sg10 sgd1
            (PrimRuns2.step sg3 sg4 _ _ sg10 sgd2
              (PrimRuns2.step sg4 sg5 _ _ sg10 sgd3
                (PrimRuns2.step sg5 sg6 _ _ sg10 sgd4
                  (PrimRuns2.step sg6 sg7 _ _ sg10 sgd5
                    (PrimRuns2.step sg7 sg8 _ _ sg10 sgd6
                      (PrimRuns2.step sg8 sg9 _ _ sg10 sgd7
                        (PrimRuns2.step sg9 sg10 _ _ sg10 sgd8 (PrimRuns2.stop sg10)))))))))))⟩,
    seqOK_selResetBlind⟩

/-- Battery H — the fiber-path resolution: a second fiber's `evSet`
section resolves the armed group exactly like the external one. -/
abbrev sh5 : PrimCfg SelSig selPrim :=
  { prim := ssC3, now := 0, cur := none,
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := [{ fiber := 1, call := SelCall.evSet, fresh := true }],
    retired := [], nextFiber := 2, exts := [] }
abbrev sh6 : PrimCfg SelSig selPrim :=
  { prim := ssC3, now := 0,
    cur := some (FSlot.running { fiber := 1, call := SelCall.evSet } false),
    parked := [{ fiber := 0, call := SelCall.sel }],
    runq := ([] : List (PReady SelSig)), retired := [], nextFiber := 2, exts := [] }
abbrev sh7 : PrimCfg SelSig selPrim :=
  { prim := ssC5, now := 0,
    cur := some (FSlot.returning { fiber := 1, call := SelCall.evSet } SelRes.rEvSet),
    parked := [], runq := [{ fiber := 0, call := SelCall.sel, fresh := false }],
    retired := [], nextFiber := 2, exts := [] }
abbrev sh8 : PrimCfg SelSig selPrim :=
  { prim := ssC5, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := SelCall.sel, fresh := false }],
    retired := [1], nextFiber := 2, exts := [] }
abbrev sh9 : PrimCfg SelSig selPrim :=
  { prim := ssC5, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SelCall.sel } true),
    parked := [], runq := [], retired := [1], nextFiber := 2, exts := [] }
abbrev sh10 : PrimCfg SelSig selPrim :=
  { prim := ssC8, now := 0, cur := none, parked := [], runq := [],
    retired := [0, 1], nextFiber := 2, exts := [] }

theorem shd1 : PrimStep2 SelSig selPrim sc3 none sh5 :=
  PrimStep2.submit sc3 1 SelCall.evSet (Or.inl rfl)
theorem shd2 : PrimStep2 SelSig selPrim sh5
    (some (issueObs SelSig (Caller.fiber 1) SelCall.evSet)) sh6 := by
  refine PrimStep2.dispatchFresh sh5
    { fiber := 1, call := SelCall.evSet, fresh := true } [] ssC3 ?_ ?_ ?_ ?_
  all_goals rfl
theorem shd3 : PrimStep2 SelSig selPrim sh6 none sh7 := by
  refine PrimStep2.fiberEffect sh6 { fiber := 1, call := SelCall.evSet } false
    [{ fiber := 0, call := SelCall.sel }] [] SelRes.rEvSet ssC5 [0] ?_ ?_ ?_ ?_ ?_
  · rfl
  · rfl
  · rfl
  · rfl
  · exact Wakes.drop (p := ({ fiber := 0, call := SelCall.sel } : Pnd SelSig)) Wakes.nil
theorem shd4 : PrimStep2 SelSig selPrim sh7
    (some (compObs SelSig (Caller.fiber 1) SelCall.evSet SelRes.rEvSet)) sh8 :=
  PrimStep2.fiberDone sh7 { fiber := 1, call := SelCall.evSet } SelRes.rEvSet rfl
theorem shd5 : PrimStep2 SelSig selPrim sh8 none sh9 :=
  PrimStep2.dispatchResumed sh8 { fiber := 0, call := SelCall.sel, fresh := false } [] rfl rfl rfl
theorem shd6 : PrimStep2 SelSig selPrim sh9
    (some (compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv)) sh10 :=
  PrimStep2.finishDone sh9 { fiber := 0, call := SelCall.sel } true [] []
    SelRes.wonEv ssC8 [] rfl rfl rfl rfl Wakes.nil

theorem seqOK_selFiberSet : SeqOK SelSig
    [issueObs SelSig (Caller.fiber 0) SelCall.sel,
     issueObs SelSig (Caller.fiber 1) SelCall.evSet,
     compObs SelSig (Caller.fiber 1) SelCall.evSet SelRes.rEvSet,
     compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem sel_battery_fiberSet :
    TracesPrimS SelSig selPrim
      [issueObs SelSig (Caller.fiber 0) SelCall.sel,
       issueObs SelSig (Caller.fiber 1) SelCall.evSet,
       compObs SelSig (Caller.fiber 1) SelCall.evSet SelRes.rEvSet,
       compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv] := by
  refine ⟨⟨sh10, PrimRuns2.step sa0 sc1 _ _ sh10 scd1
    (PrimRuns2.step sc1 sc2 _ _ sh10 scd2
      (PrimRuns2.step sc2 sc3 none _ sh10 scd3
        (PrimRuns2.step sc3 sh5 _ _ sh10 shd1
          (PrimRuns2.step sh5 sh6 _ _ sh10 shd2
            (PrimRuns2.step sh6 sh7 none _ sh10 shd3
              (PrimRuns2.step sh7 sh8 _ _ sh10 shd4
                (PrimRuns2.step sh8 sh9 none _ sh10 shd5
                  (PrimRuns2.step sh9 sh10 _ _ sh10 shd6 (PrimRuns2.stop sh10)))))))))⟩,
    seqOK_selFiberSet⟩

/-- Battery I — re-arm with the latch still set: after the hand-off
completes, a second `sel` wins inline (level semantics — the event win
never consumed the flag). -/
theorem sel_battery_rearm :
    ∃ fin : PrimCfg SelSig selPrim,
      PrimRuns2 SelSig selPrim sa0
        [issueObs SelSig (Caller.fiber 0) SelCall.sel,
         issueObs SelSig (Caller.ext 0) SelCall.evSet,
         compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet,
         compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv,
         issueObs SelSig (Caller.fiber 0) SelCall.sel,
         compObs SelSig (Caller.fiber 0) SelCall.sel SelRes.wonEv]
        fin ∧
      fin.prim.flag = true ∧ fin.prim.phase = SelPhase.idle ∧
        fin.prim.resolved = [] := by
  refine ⟨sa8, ?_, rfl, rfl, rfl⟩
  refine PrimRuns2.step sa0 sc1 _ _ sa8 scd1 ?_
  refine PrimRuns2.step sc1 sc2 _ _ sa8 scd2 ?_
  refine PrimRuns2.step sc2 sc3 none _ sa8 scd3 ?_
  refine PrimRuns2.step sc3 sc4 _ _ sa8 scd4 ?_
  refine PrimRuns2.step sc4 sc5 none _ sa8 scd5 ?_
  refine PrimRuns2.step sc5 sc6 _ _ sa8 scd6 ?_
  refine PrimRuns2.step sc6 sc7 none _ sa8 scd7 ?_
  refine PrimRuns2.step sc7 sc8 _ _ sa8 scd8 ?_
  refine PrimRuns2.step sc8 sa5 _ _ sa8 (PrimStep2.submit sc8 0 SelCall.sel (Or.inr (by simp))) ?_
  refine PrimRuns2.step sa5 sa6 _ _ sa8 sad6 ?_
  refine PrimRuns2.step sa6 sa7 none _ sa8 sad7 ?_
  exact PrimRuns2.step sa7 sa8 _ _ sa8 sad8 (PrimRuns2.stop sa8)

/-- Battery J — no spurious resolution: `evSet` with nothing armed just
raises the latch; no group machine state moves. -/
abbrev sj2 : PrimCfg SelSig selPrim :=
  { prim := ss0, now := 0, cur := none, parked := [], runq := [],
    retired := ([] : List FiberId), nextFiber := 0,
    exts := [{ x := 0, call := SelCall.evSet, result := none }] }
abbrev sj3 : PrimCfg SelSig selPrim :=
  { prim := ssE1, now := 0, cur := none, parked := [], runq := [],
    retired := ([] : List FiberId), nextFiber := 0,
    exts := [{ x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }] }
abbrev sj4 : PrimCfg SelSig selPrim :=
  { prim := ssE1, now := 0, cur := none, parked := [], runq := [],
    retired := ([] : List FiberId), nextFiber := 0, exts := [] }

theorem sjd1 : PrimStep2 SelSig selPrim sa0
    (some (issueObs SelSig (Caller.ext 0) SelCall.evSet)) sj2 := by
  refine PrimStep2.extApply sa0 0 SelCall.evSet [] [] rfl ?_
  intro h
  cases h
theorem sjd2 : PrimStep2 SelSig selPrim sj2 none sj3 := by
  refine PrimStep2.extEffect sj2 [] []
    { x := 0, call := SelCall.evSet, result := none }
    [] [] SelRes.rEvSet ssE1 [] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.nil
theorem sjd3 : PrimStep2 SelSig selPrim sj3
    (some (compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet)) sj4 :=
  PrimStep2.extDone sj3 [] []
    { x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }
    SelRes.rEvSet rfl rfl

theorem sel_battery_noSpurious :
    ∃ fin : PrimCfg SelSig selPrim,
      PrimRuns2 SelSig selPrim sa0
        [issueObs SelSig (Caller.ext 0) SelCall.evSet,
         compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet]
        fin ∧
      fin.prim.flag = true ∧ fin.prim.phase = SelPhase.idle ∧
        fin.prim.winner = none ∧ fin.prim.resolved = [] := by
  refine ⟨sj4, ?_, rfl, rfl, rfl, rfl⟩
  exact PrimRuns2.step sa0 sj2 _ _ sj4 sjd1
    (PrimRuns2.step sj2 sj3 none _ sj4 sjd2
      (PrimRuns2.step sj3 sj4 _ _ sj4 sjd3 (PrimRuns2.stop sj4)))


/-! ## The capability record

Per-encoding-class conditionals over the bare substrate
(`BaseOpsSig.none` — `BASE(select) = {}`, Stage-0 §8).  These are NOT
the stage verdict and must not be read as THEOREM B: each conditional
is stated for one encoding class only, no universal separation is
claimed, and none is refuted.  The open boundary — whether any
encoding's disciplined trace language is observationally equivalent to
`selPrim` under Stage-0 V2.3 — is recorded on the stage card. -/

/-- The natural select-shaped encoding over the bare substrate:
call domains declared faithfully (`evSet`/`evReset` external, the
select fiber-bound), programs that bottom out with a value no decoding
accepts. -/
def encSel : Encoding BaseOpsSig.none SelSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := selExtCap

theorem sel_encoding_class_inhabited :
    ∃ _enc : Encoding BaseOpsSig.none SelSig, True := ⟨encSel, trivial⟩

/-- The over-production separator: a bare external `sel` issue. -/
def extSelTrace : Trace SelSig :=
  [issueObs SelSig (Caller.ext 0) SelCall.sel]

theorem seqOK_extSel : SeqOK SelSig extSelTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- The primitive never produces the external `sel` issue: the only
step that emits an external issue is `extApply`, and it requires
`selExtCap (sel _) = true`. -/
theorem sel_not_extSel : ¬ TracesPrimS SelSig selPrim extSelTrace := by
  rintro ⟨⟨fin, hrun⟩, -⟩
  obtain ⟨ob, t', heq⟩ : ∃ ob t', extSelTrace = ob :: t' := ⟨_, _, rfl⟩
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := primRuns_cons hrun ob t' heq
  cases hstep with
  | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
      injection heq with h3 h4
      simp [issueObs] at h3
  | extApply x c preE postE hcap hnovel =>
      injection heq with h3 h4
      simp only [issueObs, Obs.mk.injEq] at h3
      rw [← h3.2.1] at hcap
      simp [selExtCap] at hcap
  | fiberDone d r hcurE =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3
  | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3
  | extDone preE postE e r hextsE hresE =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3

/-- Every encoding declaring the fiber-bound `sel` external emits the
bare issue at entry (`SysStep.extStart`). -/
theorem enc_extSel (enc : Encoding BaseOpsSig.none SelSig)
    (hcap : enc.extCap SelCall.sel = true) :
    TracesEnc BaseOpsSig.none SelSig enc extSelTrace :=
  ⟨{ st := subInit, bst := (), cur := none, parked := [], runq := ([] : List (Ready BaseOpsSig.none SelSig)), retired := ([] : List FiberId), nextFiber := 0, exts := [{ x := 0, call := SelCall.sel, prog := enc.prog (SelCall.sel) }] },
    SysRuns.step (encInit BaseOpsSig.none SelSig) _ _ [] _
      (SysStep.extStart (encInit BaseOpsSig.none SelSig) 0 (SelCall.sel) hcap
        (by show (0 : ExternalId) ∉ ([] : List (ExtBusy BaseOpsSig.none SelSig)).map
              (fun e : ExtBusy BaseOpsSig.none SelSig => e.x)
            simp))
      (SysRuns.stop _)⟩

/-- Auxiliary, per-encoding-class result — NOT the stage verdict: an
encoding that declares the fiber-bound `sel` external over-produces. -/
theorem sel_over_produces_of_extCap (enc : Encoding BaseOpsSig.none SelSig)
    (hcap : enc.extCap SelCall.sel = true) :
    OverProduces selPrim BaseOpsSig.none enc :=
  ⟨extSelTrace, ⟨enc_extSel enc hcap, seqOK_extSel⟩,
    fun hP => sel_not_extSel hP⟩

/-- The under-production separator: the external `evSet` issue plus its
completion, produced from the initial configuration. -/
def extSetTrace : Trace SelSig :=
  [issueObs SelSig (Caller.ext 0) SelCall.evSet,
   compObs SelSig (Caller.ext 0) SelCall.evSet SelRes.rEvSet]

abbrev spc0 : PrimCfg SelSig selPrim := primInit SelSig selPrim

abbrev spc1 : PrimCfg SelSig selPrim :=
  { prim := selInit, now := 0, cur := none, parked := [], runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId), nextFiber := 0, exts := [{ x := 0, call := SelCall.evSet, result := none }] }

abbrev spc2 : PrimCfg SelSig selPrim :=
  { prim := { selInit with flag := true }, now := 0, cur := none, parked := [], runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId), nextFiber := 0, exts := [{ x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }] }

abbrev spc3 : PrimCfg SelSig selPrim :=
  { prim := { selInit with flag := true }, now := 0, cur := none, parked := [], runq := ([] : List (PReady SelSig)), retired := ([] : List FiberId), nextFiber := 0, exts := [] }

theorem spd1 : PrimStep2 SelSig selPrim spc0
    (some (issueObs SelSig (Caller.ext 0) SelCall.evSet)) spc1 :=
  PrimStep2.extApply spc0 0 SelCall.evSet [] [] rfl
    (by show (0 : ExternalId) ∉ ([] : List (ExtPend SelSig)).map (fun e : ExtPend SelSig => e.x)
        simp)

theorem spd2 : PrimStep2 SelSig selPrim spc1 none spc2 := by
  refine PrimStep2.extEffect spc1 [] []
    { x := 0, call := SelCall.evSet, result := none } [] []
    SelRes.rEvSet
    { selInit with flag := true } [] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.nil

theorem spd3 : PrimStep2 SelSig selPrim spc2
    (some (compObs SelSig (Caller.ext 0) SelCall.evSet
      SelRes.rEvSet)) spc3 :=
  PrimStep2.extDone spc2 [] []
    { x := 0, call := SelCall.evSet, result := some SelRes.rEvSet }
    SelRes.rEvSet rfl rfl

theorem seqOK_extSet : SeqOK SelSig extSetTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem sel_possesses_extSet : TracesPrimS SelSig selPrim extSetTrace :=
  ⟨⟨spc3, PrimRuns2.step spc0 spc1 _ _ spc3 spd1
      (PrimRuns2.step spc1 spc2 none _ spc3 spd2
        (PrimRuns2.step spc2 spc3 _ _ spc3 spd3 (PrimRuns2.stop spc3)))⟩,
    seqOK_extSet⟩

/-- Inverting the step that emits the external `evSet` issue. -/
theorem extCap_true_of_extSetIssue {enc : Encoding BaseOpsSig.none SelSig}
    {cfg m2 : SysCfg BaseOpsSig.none SelSig} {ob : Obs SelSig}
    (hstep : SysStep BaseOpsSig.none SelSig enc SubStep cfg (some ob) m2)
    (hx : ob.caller = Caller.ext 0) (hc : ob.call = SelCall.evSet)
    (hr : ob.result = Option.none) :
    enc.extCap SelCall.evSet = true := by
  cases hstep with
  | dispatchFresh r rest hcur hrunq hfresh =>
      have h1 : Caller.fiber r.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | complete t v r hcur hpure hdec =>
      have h1 : Caller.fiber t.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | extComplete preE postE e _v r hsplit hpure hdec =>
      have h1 : Option.some r = Option.none := hr
      exact absurd h1 (by simp)
  | extStart x c hc' _ =>
      have hc1 : c = SelCall.evSet := hc
      rw [hc1] at hc'
      exact hc'

/-- Auxiliary, per-encoding-class result — NOT the stage verdict: an
encoding that declares the external `evSet` internal under-produces (it
can never emit the external `evSet` issue the primitive produces). -/
theorem sel_under_produces_of_no_extCap (enc : Encoding BaseOpsSig.none SelSig)
    (hcap : enc.extCap SelCall.evSet = false) :
    UnderProduces selPrim BaseOpsSig.none enc := by
  refine ⟨extSetTrace, sel_possesses_extSet, ?_⟩
  rintro ⟨hrun, -⟩
  obtain ⟨fin, hrun⟩ :
      ∃ fin, EncRuns BaseOpsSig.none SelSig enc
        (encInit BaseOpsSig.none SelSig) extSetTrace fin := hrun
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := sysRuns_cons hrun _ _ rfl
  have hcap' := extCap_true_of_extSetIssue hstep rfl rfl rfl
  simp [hcap] at hcap'

/-- The Stage-7 capability record: the two class conditionals the frozen
method supports.  This is RESEARCH/DEFER territory, not THEOREM B: no
universal separation is claimed, and none is refuted. -/
theorem sel_capability_defer :
    (∀ enc : Encoding BaseOpsSig.none SelSig,
        enc.extCap SelCall.sel = true →
          OverProduces selPrim BaseOpsSig.none enc) ∧
    (∀ enc : Encoding BaseOpsSig.none SelSig,
        enc.extCap SelCall.evSet = false →
          UnderProduces selPrim BaseOpsSig.none enc) :=
  ⟨sel_over_produces_of_extCap, sel_under_produces_of_no_extCap⟩

/-- Non-vacuity instance for the under-production conditional. -/
def encNoSel : Encoding BaseOpsSig.none SelSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := fun _ => false

theorem encNoSel_under : UnderProduces selPrim BaseOpsSig.none encNoSel :=
  sel_under_produces_of_no_extCap encNoSel rfl

/-- Non-vacuity instance for the over-production conditional. -/
def encExtSel : Encoding BaseOpsSig.none SelSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := fun c =>
    match c with
    | SelCall.sel => true
    | _ => false

theorem encExtSel_over : OverProduces selPrim BaseOpsSig.none encExtSel :=
  sel_over_produces_of_extCap encExtSel rfl

/-! ## The mutant battery

Five hand-written faults, one per independent discipline the model
carries.  M1 and M3 are SAFETY kills (a state invariant breaks); M2 and
M5 are RESULT-SEMANTICS kills (the state stays safe; the public result
contradicts the outcome authority); M4 is a TRACE-REMOVAL kill (every
invariant still holds — every other facet is the model's — but the
timer hand-off witness becomes unreachable; the TLA separation
configuration carries that formal certificate). -/

/-- M1 (SAFETY — at-most-once): the done-group gate dropped from the
event-edge resolution — a `done` group resolves again, appending a
second completion-window record (the one-shot claim CAS,
`claim_winner_locked` :164-168, broken). -/
def selResolveEvM1 (s : SelState) : Option (SelState × FiberId) :=
  match s.phase, s.caller with
  | SelPhase.armed, some f =>
      some ({ s with flag := true, phase := SelPhase.done, winner := some SelRes.wonEv, winEvOK := true, resolved := s.resolved ++ [(f, SelRes.wonEv)] }, f)
  | SelPhase.done, some f =>
      some ({ s with flag := true, winner := some SelRes.wonEv, winEvOK := true, resolved := s.resolved ++ [(f, SelRes.wonEv)] }, f)
  | _, _ => none

/-- A `done` group holding its one committed record. -/
abbrev ssM1 : SelState :=
  { ss0 with phase := SelPhase.done, caller := some 0, winner := some SelRes.wonEv, winEvOK := true, resolved := [(0, SelRes.wonEv)] }

/-- The discipline of a done group with its single committed record. -/
theorem selSafe_done {s : SelState} {f : FiberId} {r : SelRes}
    (hghost : (r = SelRes.wonEv ∧ s.winEvOK = true) ∨ (r = SelRes.wonTim ∧ s.winTimOK = true))
    (hs : s.phase = SelPhase.done ∧ s.caller = some f ∧ s.winner = some r ∧
      s.resolved = [(f, r)]) :
    selSafe s := by
  refine ⟨?_, ?_, ?_, ?_, ⟨by simp [hs.2.2.2], Or.inr ⟨hs.1, ⟨r, hs.2.2.1⟩, ?_⟩⟩⟩
  · constructor
    · intro _; exact ⟨r, hs.2.2.1⟩
    · intro h
      obtain ⟨r', hr'⟩ := h
      have hw : s.winner = some r' := hr'
      rw [hs.2.2.1] at hw
      simp only [Option.some.injEq] at hw
      subst hw
      exact hs.1
  · intro r' hr'
    have hw : s.winner = some r' := hr'
    rw [hs.2.2.1] at hw
    simp only [Option.some.injEq] at hw
    subst hw
    exact hghost
  · intro hcon
    rw [hs.1] at hcon
    exact absurd hcon (by simp [SelPhase.done])
  · intro _; exact ⟨f, hs.2.1⟩
  · intro p hp
    rw [hs.2.2.2] at hp
    simp only [List.mem_cons, List.not_mem_nil] at hp
    rcases hp with hp | hp
    · rw [hp]
      exact ⟨hs.2.1.symm, hs.2.2.1.symm⟩
    · exact absurd hp (by simp)

theorem selSafe_ssM1 : selSafe ssM1 :=
  selSafe_done (Or.inl ⟨rfl, rfl⟩) ⟨rfl, rfl, rfl, rfl⟩

/-- The mutant endpoint: a second window record. -/
abbrev ssM1end : SelState :=
  { ssM1 with flag := true, winner := some SelRes.wonEv, winEvOK := true, resolved := ssM1.resolved ++ [(0, SelRes.wonEv)] }

theorem selM1_breaks :
    selResolveEvM1 ssM1 = some (ssM1end, 0) ∧
    selSafe ssM1 ∧
    ¬ selSafe ssM1end := by
  refine ⟨rfl, selSafe_ssM1, ?_⟩
  intro h
  obtain ⟨-, -, -, -, h5⟩ := h
  obtain ⟨hlen, -⟩ := h5
  simp [ssM1, ssM1end] at hlen

/-- M2 (RESULT-SEMANTICS): the resumed select's finish delivers `wonEv`
regardless of the consumed record.  The released state is still safe;
the public result contradicts the completion authority (the consumed
record is a timer win). -/
def selFinishM2 : SelState → FiberId → SelCall →
    Option (SelRes × SelState × List FiberId)
  | s, f, SelCall.sel =>
      match selConsume s.resolved f with
      | some (_, rest) =>
          some (SelRes.wonEv, { s with resolved := rest, phase := SelPhase.idle, winner := none, winEvOK := false, winTimOK := false, caller := none }, [])
      | none => none
  | s, f, c => selFinish s f c

/-- Release of the group slot: the idle machine a finish leaves. -/
def selReleased (s : SelState) : SelState :=
  { s with resolved := [], phase := SelPhase.idle, winner := none, winEvOK := false, winTimOK := false, caller := none }

theorem selReleased_safe (s : SelState) : selSafe (selReleased s) := by
  refine ⟨?_, ?_, ?_, ?_, ⟨by simp [selReleased], Or.inl rfl⟩⟩
  · constructor
    · intro h; cases h
    · intro h
      obtain ⟨r, hr⟩ := h
      cases hr
  · intro r hr; cases hr
  · intro hcon; cases hcon
  · intro hcon; cases hcon

/-- A timer-won done group (the state battery D's pump leaves). -/
abbrev ssM2 : SelState :=
  { ss0 with phase := SelPhase.done, caller := some 0, timDue := true, winner := some SelRes.wonTim, winTimOK := true, resolved := [(0, SelRes.wonTim)] }

theorem selSafe_ssM2 : selSafe ssM2 :=
  selSafe_done (Or.inr ⟨rfl, rfl⟩) ⟨rfl, rfl, rfl, rfl⟩

theorem selM2_wrong_result :
    selFinishM2 ssM2 0 SelCall.sel = some (SelRes.wonEv, selReleased ssM2, []) ∧
    selFinish ssM2 0 SelCall.sel = some (SelRes.wonTim, selReleased ssM2, []) ∧
    selSafe ssM2 ∧
    selSafe (selReleased ssM2) ∧
    (selConsume ssM2.resolved 0 = some (SelRes.wonTim, []) ∧
      selSpecFin SelRes.wonTim = SelRes.wonTim) :=
  ⟨rfl, rfl, selSafe_ssM2, selReleased_safe ssM2, ⟨rfl, rfl⟩⟩

/-- M3 (SAFETY — armed quiet): the scan's ready-source gate dropped from
the park — a select suspends into `armed` with the latch already set.
Every armed-quiet break is a spurious-resolution enabler: a later
`evReset` would erase the readiness the resumed caller's inline scan
would have seen. -/
def selParkM3 : SelState → FiberId → SelCall →
    Option (SelState × List FiberId)
  | s, f, SelCall.sel =>
      some ({ s with phase := SelPhase.armed, order := true, caller := some f }, [])
  | s, f, SelCall.selT =>
      some ({ s with phase := SelPhase.armed, order := false, caller := some f }, [])
  | _, _, _ => none

theorem selM3_breaks :
    selRun ssa3 0 0 SelCall.sel = some (SelRes.wonEv, ssa3, []) ∧
    selParkM3 ssa3 0 SelCall.sel
      = some ({ ssa3 with phase := SelPhase.armed, order := true, caller := some 0 }, []) ∧
    ¬ selSafe { ssa3 with phase := SelPhase.armed, order := true, caller := some 0 } := by
  refine ⟨rfl, rfl, ?_⟩
  intro h
  obtain ⟨-, -, h3, -, -⟩ := h
  obtain ⟨hflag, -⟩ := h3 rfl
  exact absurd hflag (by simp [ssa3])

/-- M4 (TRACE-REMOVAL): the timer pump dropped — `expire` never fires.
Every state invariant still holds (every other facet is the model's),
but the timer hand-off is unreachable: the pump step the model's
timer-hand-off battery rides on vanishes.  The Lean side carries the
definitional removal; the TLA separation configuration carries the
formal trace-removal certificate. -/
def selPrimM4 : PrimLTS2 SelSig := { selPrim with expire := fun _ _ _ => none }

abbrev ssM4 : SelState :=
  { ss0 with phase := SelPhase.armed, caller := some 0, timDue := true }

theorem selM4_removes :
    selPrim.expire ssM4 0 0
      = some { ssM4 with phase := SelPhase.done, winner := some SelRes.wonTim, winTimOK := true, resolved := ssM4.resolved ++ [(0, SelRes.wonTim)] } ∧
    selPrimM4.expire ssM4 0 0 = none :=
  ⟨rfl, rfl⟩

/-- M5 (RESULT-SEMANTICS — the admission scan): the scan order flipped —
`sel` checks the timer arm before the event arm.  With both arms ready
the wrong arm wins: the result contradicts the outcome authority's
`sel` order (the lowest-index arm, :691-714). -/
def selRunM5 : SelState → Tick → FiberId → SelCall →
    Option (SelRes × SelState × List FiberId)
  | s, _, _, SelCall.sel =>
      if s.phase = SelPhase.idle then
        (if s.timDue then some (SelRes.wonTim, s, [])
         else if s.flag then some (SelRes.wonEv, s, [])
         else none)
      else none
  | s, t, f, c => selRun s t f c

theorem selSafe_ssE0 : selSafe ssE0 := by
  refine ⟨?_, ?_, ?_, ?_, ⟨by decide, Or.inl rfl⟩⟩
  · constructor
    · intro h; cases h
    · intro h
      obtain ⟨r, hr⟩ := h
      cases hr
  · intro r hr; cases hr
  · intro hcon; cases hcon
  · intro hcon; cases hcon

theorem selM5_wrong_arm :
    selRun ssE0 0 0 SelCall.sel = some (SelRes.wonEv, ssE0, []) ∧
    selRunM5 ssE0 0 0 SelCall.sel = some (SelRes.wonTim, ssE0, []) ∧
    selSafe ssE0 ∧
    (selSpecRun ssE0 SelCall.sel).map Prod.fst = some SelRes.wonEv :=
  ⟨rfl, rfl, selSafe_ssE0, rfl⟩

end Sluice.Formal
