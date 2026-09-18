/-
Sluice Event core, Stage 1V2.3 (FCB1-METHOD-CORRECTIVE-1), re-adjudicated
on the Stage-0-V2.3 calculus (V2.2 execution domains plus the v2.3 split of
the fiber call's critical-section effect from its physical return).

The Event primitive (`include/sluice/async/event.hpp`,
`src/async/scheduler_event.cpp`).  Core surface: `wait` (void, one-shot
value), `set` (returns the number of waiters drained:
`event_set_broadcast : std::size_t`), `reset` (void).  The deadline entry
point and `cancel` are extensions outside the core model, as in V2.1.

Call-domain census (from the code, verified in the Stage-0-V2.2 review):

  wait  = fiber-only   (`await_event_wait` dereferences `g_worker`)
  set   = external-capable (`event_set_broadcast`: `global_mtx_` only)
  reset = external-capable (`event_reset`: `global_mtx_` only)

The V2.1 countermodel relied on set/reset entering the scheduler FIFO;
V2.2 removes that assumption (BRAKE-1 v2.2 invalidated the V2.1 verdict),
and the trace it certificated is now *possessed* by the primitive.  The
re-adjudication separates on a different axis: an external-capable call's
implementation program runs under `extOpAllowed`, which forbids `attach` --
so the natural chained encoding (whose `set` needs the flag marker write)
cannot execute its external `set` past the first substrate step.  The
primitive's external set is atomic; the encoding's is stuck
(`encChained_underProduces`).  The THEOREM-A existential is *not* refuted
by one encoding (the wording discipline of the freeze, §6/§15): encodings
declaring `extCap set = false` are all refuted universally
(`enc_no_ext_cap_under`); a pure-program encoding produces the external
witness, and the remaining `extCap set = true` class is deferred, so the
Event capability verdict is RESEARCH / DEFER.
-/

import Sluice.Formal.CalcV2
import Sluice.Formal.JudgeV2

namespace Sluice.Formal

/-! ## API and state -/

inductive EventCall : Type where
  | wait
  | set
  | reset
deriving instance DecidableEq for EventCall

inductive EventResult : Type where
  | done
  | setWoke (n : Nat)
deriving instance DecidableEq for EventResult

abbrev EventSig : ApiSig := ⟨EventCall, EventResult⟩

/-- The Event state: the latch (`Event::set_`) and the parked waiters in
FIFO order (`Event::waiters_`). -/
structure EventState where
  flag : Bool
  waitq : List FiberId

/-! ## The primitive and its facet facts -/

/-- `event_set_broadcast`'s fused critical section (shared by the fiber and
the external path): latch, and if it was clear, drain the whole waiter
queue, publishing every waiter in FIFO order; the result is the number of
drained waiters (0 when the latch was already set).  `event_reset`'s
section unlatches.  `wait`'s inline path completes only on a set latch. -/
def eventRun (s : EventState) (c : EventCall) : Option (EventResult × EventState × List FiberId) :=
  match c with
  | EventCall.wait => if s.flag = true then some (EventResult.done, s, []) else none
  | EventCall.set =>
      if s.flag = true then some (EventResult.setWoke 0, s, [])
      else some (EventResult.setWoke s.waitq.length, { flag := true, waitq := [] }, s.waitq)
  | EventCall.reset => some (EventResult.done, { flag := false, waitq := s.waitq }, [])

def eventPrim : PrimLTS2 EventSig :=
  { State := EventState
    init := { flag := false, waitq := [] }
    admit := fun s _ _ => some s
    run := fun s _ _ c => eventRun s c
    park := fun s f c =>
      match c with
      | EventCall.wait => some ({ s with waitq := s.waitq ++ [f] }, [])
      | _ => none
    finish := fun s f c =>
      match c with
      | EventCall.wait => some (EventResult.done, s, [])
      | _ => none
    extCap := fun c =>
      match c with
      | EventCall.wait => false
      | _ => true
    extRun := fun c s _ =>
      match c with
      | EventCall.wait => none
      | _ => eventRun s c
    onTick := fun s _ => s
    expire := fun _ _ _ => none }

@[simp] theorem eventPrim_admit (s : EventState) (f : FiberId) (c : EventCall) :
    eventPrim.admit s f c = some s := rfl

@[simp] theorem eventPrim_run (s : EventState) (t : Tick) (f : FiberId) (c : EventCall) :
    eventPrim.run s t f c = eventRun s c := rfl

@[simp] theorem eventPrim_park_wait (s : EventState) (f : FiberId) :
    eventPrim.park s f EventCall.wait
      = some ({ s with waitq := s.waitq ++ [f] }, []) := rfl

@[simp] theorem eventPrim_park_set (s : EventState) (f : FiberId) :
    eventPrim.park s f EventCall.set = none := rfl

@[simp] theorem eventPrim_park_reset (s : EventState) (f : FiberId) :
    eventPrim.park s f EventCall.reset = none := rfl

@[simp] theorem eventPrim_finish_wait (s : EventState) (f : FiberId) :
    eventPrim.finish s f EventCall.wait = some (EventResult.done, s, []) := rfl

@[simp] theorem eventPrim_finish_set (s : EventState) (f : FiberId) :
    eventPrim.finish s f EventCall.set = none := rfl

@[simp] theorem eventPrim_finish_reset (s : EventState) (f : FiberId) :
    eventPrim.finish s f EventCall.reset = none := rfl

@[simp] theorem eventPrim_extCap_wait : eventPrim.extCap EventCall.wait = false := rfl

@[simp] theorem eventPrim_extCap_set : eventPrim.extCap EventCall.set = true := rfl

@[simp] theorem eventPrim_extCap_reset : eventPrim.extCap EventCall.reset = true := rfl

@[simp] theorem eventPrim_extRun_wait (s : EventState) (t : Tick) :
    eventPrim.extRun EventCall.wait s t = none := rfl

@[simp] theorem eventPrim_extRun_set (s : EventState) (t : Tick) :
    eventPrim.extRun EventCall.set s t = eventRun s EventCall.set := rfl

@[simp] theorem eventPrim_extRun_reset (s : EventState) (t : Tick) :
    eventPrim.extRun EventCall.reset s t = eventRun s EventCall.reset := rfl

@[simp] theorem eventPrim_onTick (s : EventState) (t : Tick) :
    eventPrim.onTick s t = s := rfl

@[simp] theorem eventPrim_expire (s : EventState) (t : Tick) (f : FiberId) :
    eventPrim.expire s t f = none := rfl

/-! ## The safety guarantee -/

/-- A `set` issue observation (fiber dispatch or external entry). -/
def evIsSetIssue (o : Obs EventSig) : Bool :=
  decide (o.call = EventCall.set ∧ o.result = none)

/-- The Event primitive's safety contract, restated for V2.2: every `wait`
completion is preceded by a `set` *issue*.  (V2.1 anchored on the set
*completion*; under V2.2 a drained waiter may complete before the draining
external `set` physically returns -- its critical section has run, so the
set's issue is the observable anchor.) -/
def eventNoWaitBeforeSet (t : Trace EventSig) : Prop :=
  ∀ (pre : Trace EventSig) (cl : Caller) (suf : Trace EventSig),
    t = pre ++ compObs EventSig cl EventCall.wait EventResult.done :: suf →
    ∃ o ∈ pre, evIsSetIssue o = true

/-- The carried invariant, fused: if any state shape that lets a `wait`
complete is present -- set latch, in-flight external `set`, dispatched
`set`, published (stale) runnable entry, resumed `wait` in `cur`, or a
`returning` inline-completed `wait` (V2.3: its completion is pending while
an external `reset` may already have cleared the latch) -- then a `set`
issue is already present in the prefix `pin` that led to `cfg`; and no
external `wait` record exists (the entry gate `extCap` forbids it). -/
def EventInv (cfg : PrimCfg EventSig eventPrim) (pin : Trace EventSig) : Prop :=
  (((cfg.prim.flag = true) ∨
      (∃ e ∈ cfg.exts, e.call = EventCall.set) ∨
      (∃ d : Pnd EventSig, cfg.cur = some (FSlot.running d false) ∧ d.call = EventCall.set) ∨
      (∃ w ∈ cfg.runq, w.fresh = false) ∨
      (∃ d : Pnd EventSig, cfg.cur = some (FSlot.running d true) ∧ d.call = EventCall.wait) ∨
      (∃ d : Pnd EventSig,
        cfg.cur = some (FSlot.returning d EventResult.done) ∧ d.call = EventCall.wait)) →
    ∃ o ∈ pin, evIsSetIssue o = true) ∧
  (∀ e ∈ cfg.exts, e.call ≠ EventCall.wait)

theorem eventInv_mono {cfg : PrimCfg EventSig eventPrim} {pin pin' : Trace EventSig}
    (hsub : pin ⊆ pin') (hinv : EventInv cfg pin) : EventInv cfg pin' :=
  ⟨fun h => match hinv.1 h with
    | ⟨o, hm, h'⟩ => ⟨o, hsub hm, h'⟩, hinv.2⟩

/-- Membership travels into the step's own observation suffix. -/
theorem mem_pin_snoc {pin : Trace EventSig} {o : Obs EventSig} (ob : Option (Obs EventSig))
    (hm : o ∈ pin) : o ∈ pin ++ ob.toList := by
  cases ob with
  | none => simpa using hm
  | some _ => exact List.mem_append.mpr (Or.inl hm)

/-- A witness in the old prefix is a witness in the new prefix. -/
theorem EventInv_pin_lift {pin : Trace EventSig} (obs : Option (Obs EventSig))
    (h : ∃ o ∈ pin, evIsSetIssue o = true) : ∃ o ∈ pin ++ obs.toList, evIsSetIssue o = true := by
  obtain ⟨o, hm, h'⟩ := h
  exact ⟨o, mem_pin_snoc obs hm, h'⟩

/-- Every step preserves the invariant, retargeting the prefix by the
step's own observations. -/
theorem eventStep_preserved {cfg cfg' : PrimCfg EventSig eventPrim} {ob : Option (Obs EventSig)}
    (hstep : PrimStep2 EventSig eventPrim cfg ob cfg') :
    ∀ pin, EventInv cfg pin → EventInv cfg' (pin ++ ob.toList) := by
  intro pin hinv
  obtain ⟨hmain, hnowait⟩ := hinv
  cases hstep with
  | submit f c hsub =>
      refine ⟨fun hanton => ?_, hnowait⟩
      rcases hanton with ha | ha | ha | ha | ha | ha
      · exact EventInv_pin_lift _ (hmain (Or.inl ha))
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ha)))
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inl ha))))
      · obtain ⟨w, hw, hwf⟩ := ha
        rw [List.mem_append] at hw
        rcases hw with hw | hw
        · exact EventInv_pin_lift none
            (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hwf⟩)))))
        · simp only [List.mem_singleton] at hw
          cases hw
          simp at hwf
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inl ha))))))
      · exact EventInv_pin_lift _
          (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inr ha))))))
  | dispatchFresh r rest s' hcur hrunq hfresh hadmit =>
      have hs' : s' = cfg.prim := by
        have h1 : some cfg.prim = some s' := hadmit
        injection h1 with h2
        exact h2.symm
      refine ⟨fun hanton => ?_, hnowait⟩
      rcases hanton with ha | ha | ha | ha | ha | ha
      · rw [hs'] at ha
        exact EventInv_pin_lift _ (hmain (Or.inl ha))
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ha)))
      · -- the step's own issue observation witnesses the dispatch
        obtain ⟨d, hd, hdcall⟩ := ha
        have h1 := Option.some.inj hd
        injection h1 with h2 _
        subst h2
        have hrc : r.call = EventCall.set := hdcall
        refine ⟨issueObs EventSig (Caller.fiber r.fiber) r.call,
          List.mem_append.mpr (Or.inr (List.Mem.head _)), ?_⟩
        simp only [evIsSetIssue, issueObs, hrc]
        rfl
      · obtain ⟨w, hw, hwf⟩ := ha
        have hold : w ∈ cfg.runq := by
          rw [hrunq]
          exact List.mem_cons_of_mem _ hw
        exact EventInv_pin_lift _
          (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hold, hwf⟩)))))
      · obtain ⟨d', hd', _⟩ := ha
        have h1 := Option.some.inj hd'
        simp at h1
      · obtain ⟨d', hd', _⟩ := ha
        have h1 := Option.some.inj hd'
        simp at h1
  | dispatchResumed r rest hcur hrunq hfresh =>
      refine ⟨fun hanton => ?_, hnowait⟩
      rcases hanton with ha | ha | ha | ha | ha | ha
      · exact EventInv_pin_lift _ (hmain (Or.inl ha))
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ha)))
      · obtain ⟨d', hd', _⟩ := ha
        have h1 := Option.some.inj hd'
        simp at h1
      · obtain ⟨w, hw, hwf⟩ := ha
        have hold : w ∈ cfg.runq := by
          rw [hrunq]
          exact List.mem_cons_of_mem _ hw
        exact EventInv_pin_lift none
          (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hold, hwf⟩)))))
      · -- the dispatched resumed waiter came from a stale runq entry
        have hold : r ∈ cfg.runq := by
          rw [hrunq]
          exact List.Mem.head _
        exact EventInv_pin_lift none
          (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨r, hold, hfresh⟩)))))
      · obtain ⟨d', hd', _⟩ := ha
        have h1 := Option.some.inj hd'
        simp at h1
  | fiberEffect d b ps rest r s' wk hcur hb hrunP hmap hwake =>
      rw [hb] at hcur
      cases hcall : d.call with
      | wait =>
          rw [hcall] at hrunP
          have h1 : (if cfg.prim.flag = true then some (EventResult.done, cfg.prim, [])
              else none) = some (r, s', wk) := hrunP
          by_cases hcf : cfg.prim.flag
          · rw [if_pos hcf] at h1
            injection h1 with h0
            injection h0 with _ hrest
            injection hrest with hs'eq hwkeq
            cases hwkeq
            have hps : ps = [] := by
              cases ps with
              | nil => rfl
              | cons p ps' => simp at hmap
            subst hps
            refine ⟨fun hanton => ?_, hnowait⟩
            obtain ⟨o, hm, h'⟩ := hmain (Or.inl hcf)
            rcases hanton with ha | ha | ha | ha | ha | ha
            · rw [← hs'eq] at ha
              exact ⟨o, mem_pin_snoc none hm, h'⟩
            · exact ⟨o, mem_pin_snoc none hm, h'⟩
            · obtain ⟨d', hd', _⟩ := ha
              have h1 := Option.some.inj hd'
              simp at h1
            · obtain ⟨w, hw, hwf⟩ := ha
              rw [List.mem_append] at hw
              rcases hw with hw | hw
              · exact EventInv_pin_lift _
                  (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hwf⟩)))))
              · simp at hw
            · obtain ⟨d', hd', _⟩ := ha
              have h1 := Option.some.inj hd'
              simp at h1
            · -- the wait's own returning slot satisfies this disjunct
              exact ⟨o, mem_pin_snoc none hm, h'⟩
          · rw [if_neg hcf] at h1
            simp at h1
      | set =>
          rw [hcall] at hrunP
          have h1 : (if cfg.prim.flag = true then some (EventResult.setWoke 0, cfg.prim, [])
              else some (EventResult.setWoke cfg.prim.waitq.length,
                { flag := true, waitq := [] }, cfg.prim.waitq)) = some (r, s', wk) := hrunP
          by_cases hcf : cfg.prim.flag
          · rw [if_pos hcf] at h1
            injection h1 with h0
            injection h0 with _ hrest
            injection hrest with hs'eq hwkeq
            cases hwkeq
            have hps : ps = [] := by
              cases ps with
              | nil => rfl
              | cons p ps' => simp at hmap
            subst hps
            refine ⟨fun hanton => ?_, hnowait⟩
            obtain ⟨o, hm, h'⟩ := hmain (Or.inl hcf)
            rcases hanton with ha | ha | ha | ha | ha | ha
            · rw [← hs'eq] at ha
              exact ⟨o, mem_pin_snoc none hm, h'⟩
            · exact ⟨o, mem_pin_snoc none hm, h'⟩
            · obtain ⟨d', hd', _⟩ := ha
              have h1 := Option.some.inj hd'
              simp at h1
            · obtain ⟨w, hw, hwf⟩ := ha
              rw [List.mem_append] at hw
              rcases hw with hw | hw
              · exact EventInv_pin_lift _
                  (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hwf⟩)))))
              · simp at hw
            · obtain ⟨d', hd', _⟩ := ha
              have h1 := Option.some.inj hd'
              simp at h1
            · obtain ⟨d', hd', hdcall'⟩ := ha
              have h1 := Option.some.inj hd'
              injection h1 with h2 _
              subst h2
              rw [hcall] at hdcall'
              simp at hdcall'
          · rw [if_neg hcf] at h1
            injection h1 with h0
            injection h0 with _ hrest
            injection hrest with _ hwkeq
            cases hwkeq
            -- the latch is raised by this section; the set issue came from
            -- this call's dispatch (`hcur` with the fresh bit `false`)
            have hsrc : ∃ o ∈ pin, evIsSetIssue o = true :=
              hmain (Or.inr (Or.inr (Or.inl ⟨d, hcur, hcall⟩)))
            refine ⟨fun hanton => ?_, hnowait⟩
            rcases hanton with ha | ha | ha | ha | ha | ha
            · exact EventInv_pin_lift _ hsrc
            · exact EventInv_pin_lift _ hsrc
            · obtain ⟨d', hd', _⟩ := ha
              have h1 := Option.some.inj hd'
              simp at h1
            · exact EventInv_pin_lift _ hsrc
            · obtain ⟨d', hd', _⟩ := ha
              have h1 := Option.some.inj hd'
              simp at h1
            · obtain ⟨d', hd', hdcall'⟩ := ha
              have h1 := Option.some.inj hd'
              injection h1 with h2 _
              subst h2
              rw [hcall] at hdcall'
              simp at hdcall'
      | reset =>
          rw [hcall] at hrunP
          have h1 : some (EventResult.done, { flag := false, waitq := cfg.prim.waitq }, [])
              = some (r, s', wk) := hrunP
          injection h1 with h0
          injection h0 with _ hrest
          injection hrest with hs'eq hwkeq
          cases hwkeq
          have hps : ps = [] := by
            cases ps with
            | nil => rfl
            | cons p ps' => simp at hmap
          subst hps
          refine ⟨fun hanton => ?_, hnowait⟩
          rcases hanton with ha | ha | ha | ha | ha | ha
          · rw [← hs'eq] at ha
            simp at ha
          · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ha)))
          · obtain ⟨d', hd', _⟩ := ha
            have h1 := Option.some.inj hd'
            simp at h1
          · obtain ⟨w, hw, hwf⟩ := ha
            rw [List.mem_append] at hw
            rcases hw with hw | hw
            · exact EventInv_pin_lift _
                (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hwf⟩)))))
            · simp at hw
          · obtain ⟨d', hd', _⟩ := ha
            have h1 := Option.some.inj hd'
            simp at h1
          · obtain ⟨d', hd', hdcall'⟩ := ha
            have h1 := Option.some.inj hd'
            injection h1 with h2 _
            subst h2
            rw [hcall] at hdcall'
            simp at hdcall'
  | fiberDone d r hcur =>
      refine ⟨fun hanton => ?_, hnowait⟩
      rcases hanton with ha | ha | ha | ha | ha | ha
      · exact EventInv_pin_lift _ (hmain (Or.inl ha))
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ha)))
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
      · obtain ⟨w, hw, hwf⟩ := ha
        exact EventInv_pin_lift _
          (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hwf⟩)))))
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
  | runPark d b ps rest s' wk hcur hrunE hparkE hmap hwake =>
      have hcw : d.call = EventCall.wait := by
        cases hcall : d.call with
        | set =>
            rw [hcall] at hparkE
            have h2 : Option.none = some (s', wk) := hparkE
            simp at h2
        | reset =>
            rw [hcall] at hparkE
            have h2 : Option.none = some (s', wk) := hparkE
            simp at h2
        | wait => rfl
      rw [hcw] at hparkE
      have h1 : some ({ cfg.prim with waitq := cfg.prim.waitq ++ [d.fiber] }, []) = some (s', wk) :=
        hparkE
      injection h1 with h3
      obtain ⟨h4, hwk⟩ := Prod.mk.inj h3
      have hsub : s' = { cfg.prim with waitq := cfg.prim.waitq ++ [d.fiber] } := h4.symm
      subst hwk
      have hps : ps = [] := by
        cases ps with
        | nil => rfl
        | cons p ps' => simp at hmap
      subst hps
      refine ⟨fun hanton => ?_, hnowait⟩
      rcases hanton with ha | ha | ha | ha | ha | ha
      · rw [hsub] at ha
        exact EventInv_pin_lift _ (hmain (Or.inl ha))
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ha)))
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
      · obtain ⟨w, hw, hf⟩ := ha
        rw [List.mem_append] at hw
        rcases hw with hw | hw
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hf⟩)))))
        · simp at hw
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
  | finishDone d b ps rest r s' wk hcur hb hfinD hmap hwake =>
      have hcw : d.call = EventCall.wait := by
        cases hcall : d.call with
        | set =>
            rw [hcall] at hfinD
            have h2 : Option.none = some (r, s', wk) := hfinD
            simp at h2
        | reset =>
            rw [hcall] at hfinD
            have h2 : Option.none = some (r, s', wk) := hfinD
            simp at h2
        | wait => rfl
      rw [hcw] at hfinD
      have h1 : some (EventResult.done, cfg.prim, []) = some (r, s', wk) := hfinD
      injection h1 with h0
      injection h0 with _ hrest
      injection hrest with hs'eq hwkeq
      cases hwkeq
      have hps : ps = [] := by
        cases ps with
        | nil => rfl
        | cons p ps' => simp at hmap
      subst hps
      refine ⟨fun hanton => ?_, hnowait⟩
      rcases hanton with ha | ha | ha | ha | ha | ha
      · rw [← hs'eq] at ha
        exact EventInv_pin_lift _ (hmain (Or.inl ha))
      · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ha)))
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
      · obtain ⟨w, hw, hwf⟩ := ha
        rw [List.mem_append] at hw
        rcases hw with hw | hw
        · exact EventInv_pin_lift _
            (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hwf⟩)))))
        · simp at hw
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
      · obtain ⟨d', hd', _⟩ := ha
        simp at hd'
  | extApply x c preE postE hcap hxfresh =>
      refine ⟨fun hanton => ?_, ?_⟩
      · rcases hanton with ha | ha | ha | ha | ha | ha
        · exact EventInv_pin_lift _ (hmain (Or.inl ha))
        · obtain ⟨e, he, hset⟩ := ha
          rw [List.mem_append] at he
          rcases he with he | he
          · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ⟨e, he, hset⟩)))
          · simp only [List.mem_singleton] at he
            cases he
            have hc' : c = EventCall.set := hset
            refine ⟨issueObs EventSig (Caller.ext x) c,
              List.mem_append.mpr (Or.inr (List.Mem.head _)), ?_⟩
            simp only [evIsSetIssue, issueObs, hc']
            rfl
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inl ha))))
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inr (Or.inl ha)))))
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inl ha))))))
        · exact EventInv_pin_lift _
            (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inr ha))))))
      · intro e' he'
        rw [List.mem_append] at he'
        rcases he' with he' | he'
        · exact hnowait e' he'
        · simp only [List.mem_singleton] at he'
          cases he'
          intro hwait
          have hw' : c = EventCall.wait := hwait
          rw [hw', eventPrim_extCap_wait] at hcap
          exact absurd hcap (by simp)
  | extEffect preE postE e ps rest r s' wk hsplitE hnone hrunE hmap hwake =>
      have hemem : e ∈ cfg.exts := by
        rw [hsplitE]
        exact List.mem_append.mpr (Or.inr (List.Mem.head _))
      have hc : e.call ≠ EventCall.wait := by
        intro hwait
        rw [hwait] at hrunE
        have h2 : Option.none = some (r, s', wk) := hrunE
        simp at h2
      refine ⟨fun hanton => ?_, ?_⟩
      · rcases hanton with ha | ha | ha | ha | ha | ha
        · -- only the `set` section can raise the latch (`wait` has no
            -- external section; `reset` lowers it)
          have hsetcall : e.call = EventCall.set := by
            cases hcall : e.call with
            | wait => exact absurd hcall hc
            | reset =>
                rw [hcall] at hrunE
                have h2 : some (EventResult.done,
                    { flag := false, waitq := cfg.prim.waitq }, [])
                    = some (r, s', wk) := hrunE
                injection h2 with h0
                injection h0 with _ hrest
                injection hrest with hs'eq _
                rw [← hs'eq] at ha
                simp at ha
            | set => rfl
          obtain ⟨o, hm, h'⟩ := hmain (Or.inr (Or.inl ⟨e, hemem, hsetcall⟩))
          exact ⟨o, mem_pin_snoc none hm, h'⟩
        · obtain ⟨e', he', hset⟩ := ha
          rw [List.mem_append, List.mem_cons] at he'
          rcases he' with he' | he' | he'
          · exact EventInv_pin_lift _
              (hmain (Or.inr (Or.inl ⟨e',
                by rw [hsplitE]; exact List.mem_append.mpr (Or.inl he'), hset⟩)))
          · cases he'
            exact EventInv_pin_lift _ (hmain (Or.inr (Or.inl ⟨e, hemem, hset⟩)))
          · exact EventInv_pin_lift _
              (hmain (Or.inr (Or.inl ⟨e',
                by rw [hsplitE]; exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ he')),
                hset⟩)))
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inl ha))))
        · obtain ⟨w, hw, hwf⟩ := ha
          rw [List.mem_append] at hw
          rcases hw with hw | hw
          · exact EventInv_pin_lift none
              (hmain (Or.inr (Or.inr (Or.inr (Or.inl ⟨w, hw, hwf⟩)))))
          · -- a publication means the section drained waiters: the call is
              -- a `set`
            cases ps with
            | nil => simp at hw
            | cons p ps' =>
                have hsetcall : e.call = EventCall.set := by
                  cases hcall : e.call with
                  | wait => exact absurd hcall hc
                  | reset =>
                      rw [hcall] at hrunE
                      have h2 : some (EventResult.done,
                          { flag := false, waitq := cfg.prim.waitq }, [])
                          = some (r, s', wk) := hrunE
                      injection h2 with h0
                      injection h0 with _ hrest
                      injection hrest with _ hwkeq
                      cases hwkeq
                      simp at hmap
                  | set => rfl
                obtain ⟨o, hm, h'⟩ := hmain (Or.inr (Or.inl ⟨e, hemem, hsetcall⟩))
                exact ⟨o, mem_pin_snoc none hm, h'⟩
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inl ha))))))
        · exact EventInv_pin_lift _
            (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inr ha))))))
      · intro e' he'
        rw [List.mem_append, List.mem_cons] at he'
        rcases he' with he' | he' | he'
        · exact hnowait e' (by rw [hsplitE]; exact List.mem_append.mpr (Or.inl he'))
        · cases he'
          exact hc
        · exact hnowait e'
            (by rw [hsplitE]; exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ he')))
  | extDone preE postE e r hsplitE hsome =>
      refine ⟨fun hanton => ?_, ?_⟩
      · rcases hanton with ha | ha | ha | ha | ha | ha
        · exact EventInv_pin_lift _ (hmain (Or.inl ha))
        · obtain ⟨e', he', hset⟩ := ha
          refine EventInv_pin_lift (some (compObs EventSig (Caller.ext e.x) e.call r))
            (hmain (Or.inr (Or.inl ⟨e', ?_, hset⟩)))
          rw [hsplitE]
          rcases List.mem_append.mp he' with h | h
          · exact List.mem_append.mpr (Or.inl h)
          · exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ h))
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inl ha))))
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inr (Or.inl ha)))))
        · exact EventInv_pin_lift _ (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inl ha))))))
        · exact EventInv_pin_lift _
            (hmain (Or.inr (Or.inr (Or.inr (Or.inr (Or.inr ha))))))
      · intro e' he'
        rcases List.mem_append.mp he' with h | h
        · exact hnowait e' (by rw [hsplitE]; exact List.mem_append.mpr (Or.inl h))
        · exact hnowait e'
            (by rw [hsplitE]; exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ h)))
  | envTime t hcur hle =>
      exact ⟨fun ha => EventInv_pin_lift _ (hmain ha), hnowait⟩
  | envExpire f s' preP postP p hcur hexp hparkedE hpf =>
      have h2 : Option.none = some s' := hexp
      simp at h2

/-- Inverting a step that completes a `wait`: the source configuration has
a set latch, a `returning` inline-completed `wait` (V2.3: its return is
pending, and the latch may since have been cleared by an external `reset`),
or a resumed `wait` in `cur`. -/
theorem eventStep_waitComp_inv {cfg cfg' : PrimCfg EventSig eventPrim} {ob : Obs EventSig}
    (hstep : PrimStep2 EventSig eventPrim cfg (some ob) cfg')
    (hc : ob.call = EventCall.wait) (hr : ob.result = some EventResult.done)
    (hext : ∀ e ∈ cfg.exts, e.call ≠ EventCall.wait) :
    cfg.prim.flag = true ∨
      (∃ d : Pnd EventSig,
        cfg.cur = some (FSlot.returning d EventResult.done) ∧ d.call = EventCall.wait) ∨
      (∃ d : Pnd EventSig,
        cfg.cur = some (FSlot.running d true) ∧ d.call = EventCall.wait) := by
  cases hstep with
  | dispatchFresh r rest s' hcur hrunq hfresh hadmit =>
      have h1 : Option.none = some EventResult.done := hr
      exact absurd h1 (by simp)
  | fiberDone d r hcur =>
      have hr2 : r = EventResult.done := Option.some.inj hr
      subst hr2
      have hc1 : d.call = EventCall.wait := hc
      exact Or.inr (Or.inl ⟨d, hcur, hc1⟩)
  | finishDone d b ps rest r s' wk hcur hb hfinD hmap hwake =>
      rw [hb] at hcur
      have hc1 : d.call = EventCall.wait := hc
      exact Or.inr (Or.inr ⟨d, hcur, hc1⟩)
  | extApply x c preE postE hcap hxfresh =>
      have h1 : Option.none = some EventResult.done := hr
      exact absurd h1 (by simp)
  | extDone preE postE e r hsplitE hsome =>
      have hc1 : e.call = EventCall.wait := hc
      exact absurd hc1
        (hext e (by rw [hsplitE]; exact List.mem_append.mpr (Or.inr (List.Mem.head _))))

/-- The guarantee, by induction over the run with the carried invariant.
The prefix `pin` grows by each step's own observations; a `wait` completion
inverts to a set latch or a resumed waiter, both covered by the invariant
with witnesses already in the prefix. -/
theorem eventPrim_guarantees : Guarantees eventPrim eventNoWaitBeforeSet := by
  intro t ht pre cl suf hsplit
  obtain ⟨⟨fin, hrun⟩, _⟩ := ht
  have main : ∀ (cfg0 : PrimCfg EventSig eventPrim) (t : Trace EventSig)
      (fin : PrimCfg EventSig eventPrim),
      PrimRuns2 EventSig eventPrim cfg0 t fin →
      ∀ (pin : Trace EventSig) (pre : Trace EventSig) (cl : Caller) (suf : Trace EventSig),
      EventInv cfg0 pin →
      t = pre ++ compObs EventSig cl EventCall.wait EventResult.done :: suf →
      ∃ o ∈ pin ++ pre, evIsSetIssue o = true := by
    intro cfg0 t fin hrun
    induction hrun with
    | stop cfg => intro pin pre cl suf _ hnil; simp at hnil
    | step cfg cfg' o t2 fin2 hstep hrest ih =>
        intro pin pre cl suf hinv hsp
        cases o with
        | none =>
            obtain ⟨o, hm, h'⟩ := ih (pin ++ []) pre cl suf
              (eventStep_preserved hstep pin hinv) (by simpa using hsp)
            exact ⟨o, by simpa using hm, h'⟩
        | some ob =>
            cases pre with
            | nil =>
                have hsp' : ob :: t2 = compObs EventSig cl EventCall.wait EventResult.done :: suf := by
                  simpa [Option.toList] using hsp
                injection hsp' with h1 h2
                cases h1
                have hcmp := eventStep_waitComp_inv hstep rfl rfl hinv.2
                rcases hcmp with hflag | hret | hresumed
                · obtain ⟨o, hm, h'⟩ := hinv.1 (Or.inl hflag)
                  exact ⟨o, by simpa using hm, h'⟩
                · obtain ⟨d, hd, hdc⟩ := hret
                  obtain ⟨o, hm, h'⟩ :=
                    hinv.1 (Or.inr (Or.inr (Or.inr (Or.inr (Or.inr ⟨d, hd, hdc⟩)))))
                  exact ⟨o, by simpa using hm, h'⟩
                · obtain ⟨d, hd, hdc⟩ := hresumed
                  obtain ⟨o, hm, h'⟩ :=
                    hinv.1 (Or.inr (Or.inr (Or.inr (Or.inr (Or.inl ⟨d, hd, hdc⟩)))))
                  exact ⟨o, by simpa using hm, h'⟩
            | cons a tl =>
                have hsp' : ob :: t2 = a :: (tl ++ compObs EventSig cl EventCall.wait
                    EventResult.done :: suf) := by
                  simpa [Option.toList, List.cons_append] using hsp
                injection hsp' with hab hsplit2
                subst hab
                obtain ⟨o, hm, h'⟩ :=
                  ih (pin ++ [ob]) tl cl suf (eventStep_preserved hstep pin hinv) hsplit2
                refine ⟨o, ?_, h'⟩
                simpa [List.cons_append] using hm
  have hinv0 : EventInv (primInit EventSig eventPrim) [] := by
    refine ⟨fun hanton => ?_, ?_⟩
    · rcases hanton with ha | ha | ha | ha | ha | ha
      · have h1 : (false : Bool) = true := ha
        simp at h1
      · obtain ⟨e, he, _⟩ := ha
        have h1 : (e : ExtPend EventSig) ∈ ([] : List (ExtPend EventSig)) := he
        simp at h1
      · obtain ⟨d', hd', _⟩ := ha
        have h1 : Option.none = some (FSlot.running d' false) := hd'
        simp at h1
      · obtain ⟨w, hw, _⟩ := ha
        have h1 : (w : PReady EventSig) ∈ ([] : List (PReady EventSig)) := hw
        simp at h1
      · obtain ⟨d', hd', _⟩ := ha
        have h1 : Option.none = some (FSlot.running d' true) := hd'
        simp at h1
      · obtain ⟨d', hd', _⟩ := ha
        have h1 : Option.none = some (FSlot.returning d' EventResult.done) := hd'
        simp at h1
    · intro e he
      have h1 : (e : ExtPend EventSig) ∈ ([] : List (ExtPend EventSig)) := he
      simp at h1
  obtain ⟨o, hm, h'⟩ :=
    main (primInit EventSig eventPrim) t fin hrun [] pre cl suf hinv0 hsplit
  exact ⟨o, by simpa using hm, h'⟩

/-! ## Possession batteries

The primitive's trace language is not vacuous: it produces the full
external drain of a parked waiter, and the two-observation external
sequence (issue then completion) that the V2.1 countermodel needed. -/

/-- Drain of one parked waiter by an external set, in the rule-10 order:
the waiter's completion precedes the external set's own physical return
(`global_mtx_` serializes state effects, not returns), so the guarantee is
anchored on the set *issue*. -/
def drainTrace : Trace EventSig :=
  [issueObs EventSig (Caller.fiber 0) EventCall.wait,
   issueObs EventSig (Caller.ext 0) EventCall.set,
   compObs EventSig (Caller.fiber 0) EventCall.wait EventResult.done,
   compObs EventSig (Caller.ext 0) EventCall.set (EventResult.setWoke 1)]

def eq0 : PrimCfg EventSig eventPrim := primInit EventSig eventPrim

def eq1 : PrimCfg EventSig eventPrim :=
  { prim := { flag := false, waitq := [] }, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := EventCall.wait, fresh := true }], retired := [],
    nextFiber := 1, exts := [] }

def eq2 : PrimCfg EventSig eventPrim :=
  { prim := { flag := false, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := EventCall.wait } false), parked := [],
    runq := [], retired := [], nextFiber := 1, exts := [] }

def eq3 : PrimCfg EventSig eventPrim :=
  { prim := { flag := false, waitq := [0] }, now := 0, cur := none,
    parked := [{ fiber := 0, call := EventCall.wait }], runq := [], retired := [],
    nextFiber := 1, exts := [] }

def eq4 : PrimCfg EventSig eventPrim :=
  { prim := { flag := false, waitq := [0] }, now := 0, cur := none,
    parked := [{ fiber := 0, call := EventCall.wait }], runq := [], retired := [],
    nextFiber := 1, exts := [{ x := 0, call := EventCall.set, result := none }] }

def eq5 : PrimCfg EventSig eventPrim :=
  { prim := { flag := true, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 0, call := EventCall.wait, fresh := false }],
    retired := [], nextFiber := 1,
    exts := [{ x := 0, call := EventCall.set, result := some (EventResult.setWoke 1) }] }

def eq6 : PrimCfg EventSig eventPrim :=
  { prim := { flag := true, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := EventCall.wait } true), parked := [],
    runq := [], retired := [], nextFiber := 1,
    exts := [{ x := 0, call := EventCall.set, result := some (EventResult.setWoke 1) }] }

def eq7 : PrimCfg EventSig eventPrim :=
  { prim := { flag := true, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1,
    exts := [{ x := 0, call := EventCall.set, result := some (EventResult.setWoke 1) }] }

def eq8 : PrimCfg EventSig eventPrim :=
  { prim := { flag := true, waitq := [] }, now := 0, cur := none, parked := [],
    runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem ed1 : PrimStep2 EventSig eventPrim eq0 none eq1 :=
  PrimStep2.submit eq0 0 EventCall.wait (Or.inl rfl)

theorem ed2 : PrimStep2 EventSig eventPrim eq1
    (some (issueObs EventSig (Caller.fiber 0) EventCall.wait)) eq2 := by
  refine PrimStep2.dispatchFresh eq1 { fiber := 0, call := EventCall.wait, fresh := true } []
    { flag := false, waitq := [] } ?_ ?_ ?_ ?_
  all_goals rfl

theorem ed3 : PrimStep2 EventSig eventPrim eq2 none eq3 :=
  PrimStep2.runPark eq2 { fiber := 0, call := EventCall.wait } false [] []
    { flag := false, waitq := [0] } [] rfl rfl rfl rfl Wakes.nil

theorem ed4 : PrimStep2 EventSig eventPrim eq3
    (some (issueObs EventSig (Caller.ext 0) EventCall.set)) eq4 :=
  PrimStep2.extApply eq3 0 EventCall.set [] [] rfl
    (by show (0 : ExternalId) ∉ ([] : List (ExtPend EventSig)).map (fun e : ExtPend EventSig => e.x)
        simp)

theorem ed5 : PrimStep2 EventSig eventPrim eq4 none eq5 := by
  refine PrimStep2.extEffect eq4 [] [] { x := 0, call := EventCall.set, result := none }
    [{ fiber := 0, call := EventCall.wait }] []
    (EventResult.setWoke 1) { flag := true, waitq := [] } [0] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.drop _ Wakes.nil

theorem ed6 : PrimStep2 EventSig eventPrim eq5 none eq6 := by
  refine PrimStep2.dispatchResumed eq5 { fiber := 0, call := EventCall.wait, fresh := false } []
    ?_ ?_ ?_
  all_goals rfl

theorem ed7 : PrimStep2 EventSig eventPrim eq6
    (some (compObs EventSig (Caller.fiber 0) EventCall.wait EventResult.done)) eq7 :=
  PrimStep2.finishDone eq6 { fiber := 0, call := EventCall.wait } true [] []
    EventResult.done { flag := true, waitq := [] } [] rfl rfl rfl rfl Wakes.nil

theorem ed8 : PrimStep2 EventSig eventPrim eq7
    (some (compObs EventSig (Caller.ext 0) EventCall.set (EventResult.setWoke 1))) eq8 :=
  PrimStep2.extDone eq7 [] []
    { x := 0, call := EventCall.set, result := some (EventResult.setWoke 1) }
    (EventResult.setWoke 1) rfl rfl

theorem seqOK_drainTrace : SeqOK EventSig drainTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem eventPrim_possesses_drain : TracesPrim EventSig eventPrim drainTrace :=
  ⟨eq8, PrimRuns2.step eq0 eq1 none _ eq8 ed1
    (PrimRuns2.step eq1 eq2 _ _ eq8 ed2
      (PrimRuns2.step eq2 eq3 none _ eq8 ed3
        (PrimRuns2.step eq3 eq4 _ _ eq8 ed4
          (PrimRuns2.step eq4 eq5 none _ eq8 ed5
            (PrimRuns2.step eq5 eq6 none _ eq8 ed6
              (PrimRuns2.step eq6 eq7 _ _ eq8 ed7
                (PrimRuns2.step eq7 eq8 _ [] eq8 ed8 (PrimRuns2.stop eq8))))))))⟩

/-- The two-observation external sequence: the V2.1 countermodel's trace,
now possessed by the primitive itself (set/reset do not enter the FIFO). -/
def extWitnessTrace : Trace EventSig :=
  [issueObs EventSig (Caller.ext 0) EventCall.set,
   compObs EventSig (Caller.ext 0) EventCall.set (EventResult.setWoke 0)]

def ex0 : PrimCfg EventSig eventPrim := primInit EventSig eventPrim

def ex1 : PrimCfg EventSig eventPrim :=
  { prim := { flag := false, waitq := [] }, now := 0, cur := none, parked := [],
    runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := EventCall.set, result := none }] }

def ex2 : PrimCfg EventSig eventPrim :=
  { prim := { flag := true, waitq := [] }, now := 0, cur := none, parked := [],
    runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := EventCall.set, result := some (EventResult.setWoke 0) }] }

def ex3 : PrimCfg EventSig eventPrim :=
  { prim := { flag := true, waitq := [] }, now := 0, cur := none, parked := [],
    runq := [], retired := [], nextFiber := 0, exts := [] }

theorem exs1 : PrimStep2 EventSig eventPrim ex0
    (some (issueObs EventSig (Caller.ext 0) EventCall.set)) ex1 :=
  PrimStep2.extApply ex0 0 EventCall.set [] [] rfl
    (by show (0 : ExternalId) ∉ ([] : List (ExtPend EventSig)).map (fun e : ExtPend EventSig => e.x)
        simp)

theorem exs2 : PrimStep2 EventSig eventPrim ex1 none ex2 :=
  PrimStep2.extEffect ex1 [] [] { x := 0, call := EventCall.set, result := none } [] []
    (EventResult.setWoke 0) { flag := true, waitq := [] } [] rfl rfl rfl rfl Wakes.nil

theorem exs3 : PrimStep2 EventSig eventPrim ex2
    (some (compObs EventSig (Caller.ext 0) EventCall.set (EventResult.setWoke 0))) ex3 :=
  PrimStep2.extDone ex2 [] []
    { x := 0, call := EventCall.set, result := some (EventResult.setWoke 0) }
    (EventResult.setWoke 0) rfl rfl

theorem seqOK_extWitness : SeqOK EventSig extWitnessTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem eventPrim_possesses_ext : TracesPrim EventSig eventPrim extWitnessTrace :=
  ⟨ex3, PrimRuns2.step ex0 ex1 _ _ ex3 exs1
    (PrimRuns2.step ex1 ex2 none _ ex3 exs2
      (PrimRuns2.step ex2 ex3 _ [] ex3 exs3 (PrimRuns2.stop ex3)))⟩

/-! ## The natural chained encoding and its external blockade

`set` needs the flag marker write, so its program continues after its first
substrate step with `attach` -- which `extOpAllowed` forbids off a fiber
(`await_wait*` reads `g_worker`).  The external `set` enters, takes one
step, and is stuck; it never reaches `pure`, so it never completes.  (The
Stage-0 v2.2.1 repair ensures this blockade is the *only* reason: the
machine does advance external programs.) -/

def evFlagQ : QueueId := 0
def evWaitQ : QueueId := 1

def evSetProg : ExtProg BaseOpsSig.none EventSig :=
  ExtProg.eff (SubOp.wakeOne evFlagQ) (fun _ =>
    ExtProg.eff (SubOp.attach evFlagQ none) (fun _ =>
      ExtProg.eff (SubOp.wakeOne evWaitQ) (fun _ => ExtProg.pure (SubVal.bool true))))

def encChained : Encoding BaseOpsSig.none EventSig :=
  { prog := fun c =>
      match c with
      | EventCall.set => evSetProg
      | _ => ExtProg.pure (SubVal.bool false)
    decode := fun c v =>
      match c, v with
      | EventCall.set, SubVal.bool b => some (EventResult.setWoke (if b then 1 else 0))
      | _, _ => none
    extCap := fun c =>
      match c with
      | EventCall.wait => false
      | _ => true }

/-- Programs reachable by an external `set` against `encChained`: the entry
program, or a program stuck on the `attach` its first step produced. -/
inductive evSetPhase : ExtProg BaseOpsSig.none EventSig → Prop where
  | entry : evSetPhase evSetProg
  | stuck : ∀ k : SubVal → ExtProg BaseOpsSig.none EventSig,
      evSetPhase (ExtProg.eff (SubOp.attach evFlagQ none) k)

theorem evSetPhase_step {o : SubOp} {k : SubVal → ExtProg BaseOpsSig.none EventSig}
    {v : SubVal}
    (hp : evSetPhase (ExtProg.eff o k)) (ho : extOpAllowed o = true) :
    evSetPhase (k v) := by
  cases hp with
  | entry => exact evSetPhase.stuck _
  | stuck => simp [extOpAllowed] at ho

def evEncStuck (cfg : SysCfg BaseOpsSig.none EventSig) : Prop :=
  ∀ e ∈ cfg.exts, e.call = EventCall.set → evSetPhase e.prog

theorem evEncStuck_step {cfg cfg' : SysCfg BaseOpsSig.none EventSig}
    {ob : Option (Obs EventSig)}
    (hstep : SysStep BaseOpsSig.none EventSig encChained SubStep cfg ob cfg') :
    evEncStuck cfg → evEncStuck cfg' := by
  cases hstep
  case extSubOpStep preE postE e o k v st' hsplit hprog hallowed _ _ =>
      intro hst e' he' hset'
      rw [List.mem_append, List.mem_cons] at he'
      rcases he' with he' | he' | he'
      · exact hst e' (by rw [hsplit]; exact List.mem_append.mpr (Or.inl he')) hset'
      · cases he'
        have hmem : e ∈ cfg.exts := by
          rw [hsplit]
          exact List.mem_append.mpr (Or.inr (List.Mem.head _))
        have hcall : e.call = EventCall.set := hset'
        have hph := hst e hmem hcall
        rw [hprog] at hph
        exact evSetPhase_step hph hallowed
      · refine hst e' ?_ hset'
        rw [hsplit]
        exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ he'))
  case extSubOpWake preE postE e o k v st' w ores preP postP p kw hsplit hprog hallowed
      _ _ _ _ _ =>
      intro hst e' he' hset'
      rw [List.mem_append, List.mem_cons] at he'
      rcases he' with he' | he' | he'
      · exact hst e' (by rw [hsplit]; exact List.mem_append.mpr (Or.inl he')) hset'
      · cases he'
        have hmem : e ∈ cfg.exts := by
          rw [hsplit]
          exact List.mem_append.mpr (Or.inr (List.Mem.head _))
        have hcall : e.call = EventCall.set := hset'
        have hph := hst e hmem hcall
        rw [hprog] at hph
        exact evSetPhase_step hph hallowed
      · refine hst e' ?_ hset'
        rw [hsplit]
        exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ he'))
  case extComplete preE postE e v r hsplit hpure hdec =>
      intro hst e' he' hset'
      refine hst e' ?_ hset'
      rw [hsplit]
      rcases List.mem_append.mp he' with h | h
      · exact List.mem_append.mpr (Or.inl h)
      · exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ h))
  case extStart x c hcap hxfresh =>
      intro hst e he hset
      rw [List.mem_append] at he
      rcases he with he | he
      · exact hst e he hset
      · simp only [List.mem_singleton] at he
        cases he
        have hc' : c = EventCall.set := hset
        rw [hc']
        exact evSetPhase.entry
  all_goals
      intro hst e' he' hset'
      exact hst e' he' hset'

theorem evEncStuck_runs {cfg fin : SysCfg BaseOpsSig.none EventSig} {t : Trace EventSig}
    (hrun : EncRuns BaseOpsSig.none EventSig encChained cfg t fin) :
    evEncStuck cfg → evEncStuck fin := by
  induction hrun with
  | stop cfg => intro hst; exact hst
  | step cfg cfg' o t2 fin2 hstep hrest ih =>
      intro hst
      exact ih (evEncStuck_step hstep hst)

/-- A silent prefix from the empty encoding keeps `exts` empty: no rule
extends `exts` without emitting the entry observation. -/
theorem encRuns_silent_exts_nil {cfg fin : SysCfg BaseOpsSig.none EventSig}
    {t : Trace EventSig}
    (hrun : EncRuns BaseOpsSig.none EventSig encChained cfg t fin) :
    t = [] → cfg.exts = [] → fin.exts = [] := by
  induction hrun with
  | stop cfg => intro _ h; exact h
  | step cfg cfg' o t2 fin2 hstep hrest ih =>
      intro htrace hexts
      have ho : o = none := by
        cases o with
        | none => rfl
        | some ob => simp [Option.toList] at htrace
      subst ho
      simp only [Option.toList, List.nil_append] at htrace
      cases hstep with
      | extSubOpStep preE postE e o k v st' hsplit _ _ _ _ =>
          rw [hexts] at hsplit
          cases preE with
          | nil => simp at hsplit
          | cons a l => simp at hsplit
      | extSubOpWake preE postE e o k v st' w ores preP postP p kw hsplit _ _ _ _ _ _ _ =>
          rw [hexts] at hsplit
          cases preE with
          | nil => simp at hsplit
          | cons a l => simp at hsplit
      | submit f c _ => exact ih htrace hexts
      | dispatchResumed r rest _ _ _ => exact ih htrace hexts
      | subOpStep t o k v st' _ _ _ _ => exact ih htrace hexts
      | subOpWake t o k v st' w ores preP postP p kw _ _ _ _ _ _ _ =>
          exact ih htrace hexts
      | suspendBlock t w k _ _ _ => exact ih htrace hexts
      | suspendConsume t w k o _ _ _ => exact ih htrace hexts
      | baseRunNone t o k r bst' _ _ _ => exact ih htrace hexts
      | baseRunWake1 t o k r bst' preP postP p o' k' r' bst'' _ _ _ _ _ _ =>
          exact ih htrace hexts
      | basePark t o k bst' _ _ _ _ => exact ih htrace hexts
      | envTime t' _ _ => exact ih htrace hexts
      | envExpire q w st' preP postP p kw _ _ _ _ _ => exact ih htrace hexts

/-- The in-flight record `extStart` registers for the external `set`. -/
def evExtSetRecord : ExtBusy BaseOpsSig.none EventSig :=
  { x := 0, call := EventCall.set, prog := encChained.prog EventCall.set }

/-- Inverting the step that emits an external `set` issue. -/
theorem sysStep_extIssue_inv {cfg m2 : SysCfg BaseOpsSig.none EventSig} {ob : Obs EventSig}
    (hstep : SysStep BaseOpsSig.none EventSig encChained SubStep cfg (some ob) m2)
    (hx : ob.caller = Caller.ext 0) (hc : ob.call = EventCall.set)
    (hr : ob.result = Option.none) :
    ∃ (preE postE : List (ExtBusy BaseOpsSig.none EventSig))
      (e : ExtBusy BaseOpsSig.none EventSig),
      m2.exts = preE ++ e :: postE ∧ cfg.exts = preE ++ postE ∧
      e.x = (0 : ExternalId) ∧ e.call = EventCall.set ∧
      e.prog = encChained.prog EventCall.set := by
  cases hstep with
  | dispatchFresh r rest hcur hrunq hfresh =>
      have h1 : Caller.fiber r.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | complete t v r hcur hpure hdec =>
      have h1 : Caller.fiber t.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | extComplete preE postE e v r hsplit hpure hdec =>
      have h1 : Option.some r = Option.none := hr
      exact absurd h1 (by simp)
  | extStart x c hcap hxfresh =>
      have hx1 : Caller.ext x = Caller.ext 0 := hx
      injection hx1 with hx2
      cases hx2
      have hc1 : c = EventCall.set := hc
      cases hc1
      exact ⟨cfg.exts, [], evExtSetRecord, rfl, (List.append_nil _).symm, rfl, rfl, rfl⟩

/-- Any step emitting an external `set` issue forces `extCap set = true`. -/
theorem extCap_true_of_extIssue {enc : Encoding BaseOpsSig.none EventSig}
    {cfg m2 : SysCfg BaseOpsSig.none EventSig} {ob : Obs EventSig}
    (hstep : SysStep BaseOpsSig.none EventSig enc SubStep cfg (some ob) m2)
    (hx : ob.caller = Caller.ext 0) (hc : ob.call = EventCall.set)
    (hr : ob.result = Option.none) :
    enc.extCap EventCall.set = true := by
  cases hstep with
  | dispatchFresh r rest hcur hrunq hfresh =>
      have h1 : Caller.fiber r.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | complete t v r hcur hpure hdec =>
      have h1 : Caller.fiber t.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | extComplete preE postE e v r hsplit hpure hdec =>
      have h1 : Option.some r = Option.none := hr
      exact absurd h1 (by simp)
  | extStart x c hc' _ =>
      have hx1 : Caller.ext x = Caller.ext 0 := hx
      injection hx1 with _
      have hc1 : c = EventCall.set := hc
      rw [hc1] at hc'
      exact hc'

/-- Inverting the step that emits the external `set` completion. -/
theorem sysStep_extComp_inv {cfg m2 : SysCfg BaseOpsSig.none EventSig} {ob : Obs EventSig}
    (hstep : SysStep BaseOpsSig.none EventSig encChained SubStep cfg (some ob) m2)
    (hx : ob.caller = Caller.ext 0) (hc : ob.call = EventCall.set)
    (hr : ob.result = some (EventResult.setWoke 0)) :
    ∃ (preE postE : List (ExtBusy BaseOpsSig.none EventSig)) (v : SubVal)
      (e : ExtBusy BaseOpsSig.none EventSig),
      cfg.exts = preE ++ e :: postE ∧ e.x = 0 ∧ e.call = EventCall.set ∧
      e.prog = ExtProg.pure v := by
  cases hstep with
  | dispatchFresh r rest hcur hrunq hfresh =>
      have h1 : Caller.fiber r.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | complete t v r hcur hpure hdec =>
      have h1 : Caller.fiber t.fiber = Caller.ext 0 := hx
      exact absurd h1 (by simp)
  | extStart x c hcap hxfresh =>
      have h1 : Option.none = some (EventResult.setWoke 0) := hr
      exact absurd h1 (by simp)
  | extComplete preE postE e v r hsplit hpure hdec =>
      have hx1 : Caller.ext e.x = Caller.ext 0 := hx
      injection hx1 with hx2
      have hc1 : e.call = EventCall.set := hc
      exact ⟨preE, postE, v, e, hsplit, hx2, hc1, hpure⟩

theorem evSetPhase_not_pure {v : SubVal} (h : evSetPhase (ExtProg.pure v)) : False := by
  revert h
  generalize hp : ExtProg.pure v = p
  intro h
  cases h with
  | entry => exact absurd hp (by simp [evSetProg])
  | stuck k => exact absurd hp (by simp)

/-- The natural chained encoding cannot produce the external witness: its
`set` enters, but its program's continuation is blocked on `attach` forever,
so no external `set` completion exists. -/
theorem encChained_not_extWitness :
    ¬ TracesEncS BaseOpsSig.none EventSig encChained extWitnessTrace := by
  rintro ⟨hrun, _⟩
  obtain ⟨fin, hrun⟩ :
      ∃ fin, EncRuns BaseOpsSig.none EventSig encChained
        (encInit BaseOpsSig.none EventSig) extWitnessTrace fin := hrun
  obtain ⟨m1, m2, h1, hstep1, hrest1⟩ := sysRuns_cons hrun _ _ rfl
  have hnil1 : m1.exts = [] := encRuns_silent_exts_nil h1 rfl rfl
  obtain ⟨preE, postE, e1, hsplitE, hpre, hex0, hc1, hprog1⟩ :=
    sysStep_extIssue_inv hstep1 rfl rfl rfl
  have hpp : preE ++ postE = [] := by rw [← hpre]; exact hnil1
  have hpre0 : preE = [] := by
    cases preE with
    | nil => rfl
    | cons a l => simp at hpp
  subst hpre0
  have hpost0 : postE = [] := hpp
  subst hpost0
  have hst2 : evEncStuck m2 := by
    intro e' he' _
    rw [hsplitE] at he'
    simp only [List.mem_append, List.nil_append, List.mem_singleton] at he'
    cases he'
    rw [hprog1]
    exact evSetPhase.entry
  obtain ⟨m3, m4, h2, hstep2, _⟩ := sysRuns_cons hrest1 _ _ rfl
  have hst3 : evEncStuck m3 := evEncStuck_runs h2 hst2
  obtain ⟨preE', postE', v, e0, hsplit2, _, hc2, hpure2⟩ :=
    sysStep_extComp_inv hstep2 rfl rfl rfl
  have hemem : e0 ∈ m3.exts := by
    rw [hsplit2]
    exact List.mem_append.mpr (Or.inr (List.Mem.head _))
  have hph := hst3 e0 hemem hc2
  rw [hpure2] at hph
  exact evSetPhase_not_pure hph

/-- Every encoding that declares `set` fiber-only under-produces: no step
can emit the external `set` issue at all. -/
theorem enc_no_ext_cap_under (enc : Encoding BaseOpsSig.none EventSig)
    (hcap : enc.extCap EventCall.set = false) :
    UnderProduces eventPrim BaseOpsSig.none enc := by
  refine ⟨extWitnessTrace, ⟨eventPrim_possesses_ext, seqOK_extWitness⟩, ?_⟩
  rintro ⟨hrun, _⟩
  obtain ⟨fin, hrun⟩ :
      ∃ fin, EncRuns BaseOpsSig.none EventSig enc
        (encInit BaseOpsSig.none EventSig) extWitnessTrace fin := hrun
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := sysRuns_cons hrun _ _ rfl
  have hcap' := extCap_true_of_extIssue hstep rfl rfl rfl
  simp [hcap] at hcap'

theorem encChained_underProduces : UnderProduces eventPrim BaseOpsSig.none encChained :=
  ⟨extWitnessTrace, ⟨eventPrim_possesses_ext, seqOK_extWitness⟩, encChained_not_extWitness⟩

theorem encChained_not_reduction : ¬ Reduction eventPrim encChained := by
  intro red
  exact encChained_not_extWitness
    (red.fwd extWitnessTrace ⟨eventPrim_possesses_ext, seqOK_extWitness⟩)

/-! ## The verdict: RESEARCH / DEFER

No universal separation is claimed for the external witness: an encoding
with a `pure` external `set` program produces it.  So THEOREM A is neither
proved nor refuted for the `extCap set = true` class; the stage records the
universal failure of the fiber-only class and the chained encoding's
under-production, and defers. -/

/-- A degenerate encoding whose external `set` program is already `pure`:
it produces the external witness, so the witness is not a universal
separator. -/
def encPure : Encoding BaseOpsSig.none EventSig :=
  { prog := fun _ => ExtProg.pure (SubVal.bool true)
    decode := fun c v =>
      match c, v with
      | EventCall.set, SubVal.bool _ => some (EventResult.setWoke 0)
      | _, _ => none
    extCap := fun c =>
      match c with
      | EventCall.wait => false
      | _ => true }

def px0 : SysCfg BaseOpsSig.none EventSig := encInit BaseOpsSig.none EventSig

def px1 : SysCfg BaseOpsSig.none EventSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [],
    nextFiber := 0,
    exts := [{ x := 0, call := EventCall.set, prog := ExtProg.pure (SubVal.bool true) }] }

def px2 : SysCfg BaseOpsSig.none EventSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [],
    nextFiber := 0, exts := [] }

theorem pxs1 : SysStep BaseOpsSig.none EventSig encPure SubStep px0
    (some (issueObs EventSig (Caller.ext 0) EventCall.set)) px1 :=
  SysStep.extStart px0 0 EventCall.set rfl
    (by show (0 : ExternalId) ∉ ([] : List (ExtBusy BaseOpsSig.none EventSig)).map
          (fun e : ExtBusy BaseOpsSig.none EventSig => e.x)
        simp)

theorem pxs2 : SysStep BaseOpsSig.none EventSig encPure SubStep px1
    (some (compObs EventSig (Caller.ext 0) EventCall.set (EventResult.setWoke 0))) px2 := by
  refine SysStep.extComplete px1 [] []
    { x := 0, call := EventCall.set, prog := ExtProg.pure (SubVal.bool true) }
    (SubVal.bool true) (EventResult.setWoke 0) ?_ ?_ ?_
  all_goals rfl

theorem encPure_possesses_ext : TracesEnc BaseOpsSig.none EventSig encPure extWitnessTrace :=
  ⟨px2, SysRuns.step px0 px1 _ _ px2 pxs1
    (SysRuns.step px1 px2 _ [] px2 pxs2 (SysRuns.stop px2))⟩

theorem event_capability_defer :
    (∀ enc : Encoding BaseOpsSig.none EventSig,
        enc.extCap EventCall.set = false → UnderProduces eventPrim BaseOpsSig.none enc) ∧
    ¬ SeparatedBy eventPrim BaseOpsSig.none extWitnessTrace := by
  refine ⟨enc_no_ext_cap_under, ?_⟩
  intro hsep
  exact hsep.2 encPure ⟨encPure_possesses_ext, seqOK_extWitness⟩

/-! ## The negative mutant

`wait` completing on a clear latch breaks the guarantee outright: the
two-observation inline wait has no set issue at all. -/

def eventMutantRun (s : EventState) (t : Tick) (f : FiberId) (c : EventCall) :
    Option (EventResult × EventState × List FiberId) :=
  match c with
  | EventCall.wait => some (EventResult.done, s, [])
  | _ => eventRun s c

def eventMutant : PrimLTS2 EventSig :=
  { eventPrim with run := eventMutantRun }

def mutantTrace : Trace EventSig :=
  [issueObs EventSig (Caller.fiber 0) EventCall.wait,
   compObs EventSig (Caller.fiber 0) EventCall.wait EventResult.done]

def mu0 : PrimCfg EventSig eventMutant := primInit EventSig eventMutant

def mu1 : PrimCfg EventSig eventMutant :=
  { prim := { flag := false, waitq := [] }, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := EventCall.wait, fresh := true }], retired := [],
    nextFiber := 1, exts := [] }

def mu2 : PrimCfg EventSig eventMutant :=
  { prim := { flag := false, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := EventCall.wait } false), parked := [],
    runq := [], retired := [], nextFiber := 1, exts := [] }

def mu2b : PrimCfg EventSig eventMutant :=
  { prim := { flag := false, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := EventCall.wait } EventResult.done),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def mu3 : PrimCfg EventSig eventMutant :=
  { prim := { flag := false, waitq := [] }, now := 0, cur := none, parked := [],
    runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem mud1 : PrimStep2 EventSig eventMutant mu0 none mu1 :=
  PrimStep2.submit mu0 0 EventCall.wait (Or.inl rfl)

theorem mud2 : PrimStep2 EventSig eventMutant mu1
    (some (issueObs EventSig (Caller.fiber 0) EventCall.wait)) mu2 := by
  refine PrimStep2.dispatchFresh mu1 { fiber := 0, call := EventCall.wait, fresh := true } []
    { flag := false, waitq := [] } ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | simp [eventMutant, eventMutantRun]

theorem mud3 : PrimStep2 EventSig eventMutant mu2 none mu2b := by
  refine PrimStep2.fiberEffect mu2 { fiber := 0, call := EventCall.wait } false [] []
    EventResult.done { flag := false, waitq := [] } [] ?_ ?_ ?_ ?_ Wakes.nil
  all_goals first
    | rfl
    | simp [eventMutant, eventMutantRun]

theorem mud4 : PrimStep2 EventSig eventMutant mu2b
    (some (compObs EventSig (Caller.fiber 0) EventCall.wait EventResult.done)) mu3 :=
  PrimStep2.fiberDone mu2b { fiber := 0, call := EventCall.wait } EventResult.done rfl

theorem seqOK_mutantTrace : SeqOK EventSig mutantTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem eventMutant_possesses : TracesPrim EventSig eventMutant mutantTrace :=
  ⟨mu3, PrimRuns2.step mu0 mu1 none _ mu3 mud1
    (PrimRuns2.step mu1 mu2 _ _ mu3 mud2
      (PrimRuns2.step mu2 mu2b none _ mu3 mud3
        (PrimRuns2.step mu2b mu3 _ [] mu3 mud4 (PrimRuns2.stop mu3))))⟩

theorem eventMutant_not_guarantees : ¬ Guarantees eventMutant eventNoWaitBeforeSet := by
  intro h
  obtain ⟨o, ho, h'⟩ :=
    h mutantTrace ⟨eventMutant_possesses, seqOK_mutantTrace⟩
      [issueObs EventSig (Caller.fiber 0) EventCall.wait] (Caller.fiber 0) [] rfl
  simp only [List.mem_singleton] at ho
  cases ho
  simp [evIsSetIssue, issueObs] at h'

end Sluice.Formal
