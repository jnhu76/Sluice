/-
Sluice Stage-0-V2.3 vacuity certificate.

The stateless echo primitive (`echoPrim`) is observationally equivalent,
over the disciplined trace languages (`TracesPrimS` / `TracesEncS`), to its
encoding over the bare substrate (`echoEnc`, `BASE(echo) = {}`).  Both
sides run under the same call-execution discipline; the equivalence is a
pair of step-by-step simulations between the configurations reachable in
echo runs, composed with the initial-configuration agreement of the two
configuration maps.  Under the v2.3 split the primitive's silent
`fiberEffect` maps to zero encoding steps (the configuration maps erase
the `running`/`returning` distinction, and echo's section is stateless),
and the encoding's `complete` maps to the primitive's `fiberEffect` ++
`fiberDone` pair.

The V2.2 execution-domain tests certify that the external-call machinery
is semantic, not notational:

  * `domPrim_possesses` — a primitive run where two fibers are queued ahead
    of an external caller, whose call (issue *and* completion) serializes
    between the first fiber's return and the second fiber's issue — the
    runnable FIFO is bypassed, as the `global_mtx_`-only code paths allow;
  * `domEnc_possesses` — the encoding side produces the same schedule, so
    the execution domains are symmetric by construction;
  * `encLie_overProduces` — an encoding that declares a fiber-only call
    externally capable over-produces: the domain declaration is part of
    the adjudicated surface and the gate bites.
-/

import Sluice.Formal.JudgeV2

namespace Sluice.Formal

/-! ## Echo: the reducible-wrapper control -/

/-- The echo API call shape. -/
inductive EchoCall : Type where
  | ping
deriving instance DecidableEq for EchoCall

/-- The echo API result shape. -/
inductive EchoResult : Type where
  | pong
deriving instance DecidableEq for EchoResult

/-- The echo API signature. -/
abbrev EchoSig : ApiSig := ⟨EchoCall, EchoResult⟩

/-- The stateless echo primitive: every admitted call returns `pong` at its
dispatch, nothing parks, the clock mirror is the identity, and no call has
an external domain. -/
def echoPrim : PrimLTS2 EchoSig :=
  { State := Unit
    init := ()
    admit := fun s _ _ => some s
    run := fun s _ _ _ => some (EchoResult.pong, s, [])
    park := fun _ _ _ => none
    finish := fun _ _ _ => none
    extCap := fun _ => false
    extRun := fun _ _ _ => none
    onTick := fun s _ => s
    expire := fun _ _ _ => none }

/-- The echo encoding over the bare substrate: the trivial immediately-
returning program, decoded to `pong`; no call is declared external-capable. -/
def echoEnc : Encoding BaseOpsSig.none EchoSig :=
  { prog := fun _ => ExtProg.pure SubVal.unit
    decode := fun _ _ => some EchoResult.pong
    extCap := fun _ => false }

/-! ## Reachable configuration shapes

In an echo run nothing ever parks and no external call exists, so no
runnable entry is ever resumed and the only reachable steps are `submit`,
`dispatchFresh`, the inline completion (`fiberEffect` + `fiberDone` on
the primitive side, `complete` on the encoding side) and the idle
`envTime`. -/

/-- Shape of a primitive echo configuration: nothing parked, no external
call in flight, every runnable entry is a fresh submission, and every
returning slot carries the echo result (V2.3: the completion's result is
fixed by the section, so the shape must carry it across the split). -/
def PrimShapeEcho (cfg : PrimCfg EchoSig echoPrim) : Prop :=
  cfg.parked = [] ∧ cfg.exts = [] ∧ (∀ r ∈ cfg.runq, r.fresh = true) ∧
  (∀ d rr, cfg.cur = some (FSlot.returning d rr) → rr = EchoResult.pong)

/-- Shape of an encoding echo configuration: nothing parked, no external
call in flight, and every program (queued or running) is the trivial
immediately-returning one. -/
def EncShapeEcho (cfg : SysCfg BaseOpsSig.none EchoSig) : Prop :=
  cfg.parked = [] ∧ cfg.exts = [] ∧
  (∀ r ∈ cfg.runq, r.fresh = true ∧ r.prog = ExtProg.pure SubVal.unit) ∧
  (∀ t : Running BaseOpsSig.none EchoSig, cfg.cur = some t → t.prog = ExtProg.pure SubVal.unit)

/-- A queued primitive call, as an encoding-side runnable entry. -/
def echoReady (r : PReady EchoSig) : Ready BaseOpsSig.none EchoSig :=
  { fiber := r.fiber, call := r.call, prog := ExtProg.pure SubVal.unit, fresh := r.fresh }

/-- The in-flight primitive call slot, as the encoding-side running fiber;
the maps erase the `running`/`returning` distinction (echo's section is
stateless, so the distinction carries no encoding-side content). -/
def echoRun (sl : FSlot EchoSig) : Running BaseOpsSig.none EchoSig :=
  { fiber := sl.fiber, call := sl.call, prog := ExtProg.pure SubVal.unit }

/-- An encoding-side runnable entry, as a queued primitive call. -/
def echoRunqP (r : Ready BaseOpsSig.none EchoSig) : PReady EchoSig :=
  { fiber := r.fiber, call := r.call, fresh := r.fresh }

/-- The encoding-side running fiber, as the in-flight primitive slot; the
call is always a freshly dispatched one (echo never resumes). -/
def echoCur (t : Running BaseOpsSig.none EchoSig) : FSlot EchoSig :=
  FSlot.running { fiber := t.fiber, call := t.call } false

/-- Map a primitive echo configuration to the encoding side. -/
def echoToE (cfg : PrimCfg EchoSig echoPrim) : SysCfg BaseOpsSig.none EchoSig :=
  { st := { subInit with now := cfg.now }
    bst := ()
    cur := cfg.cur.map echoRun
    parked := []
    runq := cfg.runq.map echoReady
    retired := cfg.retired
    nextFiber := cfg.nextFiber
    exts := [] }

/-- Map an encoding echo configuration back to the primitive side. -/
def echoToP (cfg : SysCfg BaseOpsSig.none EchoSig) : PrimCfg EchoSig echoPrim :=
  { prim := ()
    now := cfg.st.now
    cur := cfg.cur.map echoCur
    parked := []
    runq := cfg.runq.map echoRunqP
    retired := cfg.retired
    nextFiber := cfg.nextFiber
    exts := [] }

/-- The primitive configuration after echo's `fiberEffect`: its fields are
written in exactly the shape the `fiberEffect` constructor produces. -/
def echoMid (cfg : SysCfg BaseOpsSig.none EchoSig) (tR : Running BaseOpsSig.none EchoSig) :
    PrimCfg EchoSig echoPrim :=
  { prim := (), now := (echoToP cfg).now,
    cur := some (FSlot.returning { fiber := tR.fiber, call := tR.call } EchoResult.pong),
    parked := [] ++ [],
    runq := (echoToP cfg).runq ++ List.map (fun p : Pnd EchoSig =>
      { fiber := p.fiber, call := p.call, fresh := false }) [],
    retired := (echoToP cfg).retired, nextFiber := (echoToP cfg).nextFiber,
    exts := (echoToP cfg).exts }

/-- The encoding configuration after echo's `complete`. -/
def echoFin (cfg : SysCfg BaseOpsSig.none EchoSig) (tR : Running BaseOpsSig.none EchoSig) :
    SysCfg BaseOpsSig.none EchoSig :=
  { st := cfg.st, bst := cfg.bst, cur := none, parked := cfg.parked, runq := cfg.runq,
    retired := if tR.fiber ∈ cfg.retired then cfg.retired else tR.fiber :: cfg.retired,
    nextFiber := cfg.nextFiber, exts := cfg.exts }

/-- Split an append that equals the empty list. -/
theorem append_eq_nil_split {α : Type u} {as bs : List α} (h : as ++ bs = []) :
    as = [] ∧ bs = [] := by
  cases as with
  | nil => exact ⟨rfl, h⟩
  | cons a l =>
      rw [List.cons_append] at h
      exact absurd h (by simp)

theorem echoToE_primInit :
    echoToE (primInit EchoSig echoPrim) = encInit BaseOpsSig.none EchoSig := rfl

theorem echoToP_encInit :
    echoToP (encInit BaseOpsSig.none EchoSig) = primInit EchoSig echoPrim := rfl

set_option linter.unusedVariables false in
set_option linter.unusedSimpArgs false in
/-- Every disciplined echo primitive run is matched, step for step and
observation for observation, by the encoding. -/
theorem sim_echo_fwd :
    ∀ (cfg : PrimCfg EchoSig echoPrim) (t : Trace EchoSig) (fin : PrimCfg EchoSig echoPrim),
      PrimRuns2 EchoSig echoPrim cfg t fin → PrimShapeEcho cfg →
      ∃ finE, SysRuns BaseOpsSig.none EchoSig echoEnc SubStep (echoToE cfg) t finE := by
  intro cfg t fin hrun
  induction hrun with
  | stop cfg => intro _; exact ⟨echoToE cfg, SysRuns.stop _⟩
  | step cfg cfg' o t2 fin2 hstep hrest ih =>
      intro hshape
      obtain ⟨hp, hx, hq, hcurS⟩ := hshape
      cases hstep with
      | submit f c hsub =>
          rename_i cfg
          obtain ⟨finE, hrunE⟩ := ih ⟨hp, hx, by
            intro x hxm
            rw [List.mem_append] at hxm
            rcases hxm with hxm | hxm
            · exact hq x hxm
            · simp only [List.mem_singleton] at hxm
              subst hxm
              rfl, hcurS⟩
          refine ⟨finE, SysRuns.step _ _ _ _ _ (SysStep.submit (echoToE cfg) f c hsub) ?_⟩
          simp only [echoToE, hp, List.map_append] at hrunE ⊢
          exact hrunE
      | dispatchFresh r rest s' hcur hrunq hfresh hadmit =>
          rename_i cfg
          obtain ⟨finE, hrunE⟩ := ih ⟨hp, hx, by
            intro x hxm
            refine hq x ?_
            rw [hrunq]
            exact List.Mem.tail _ hxm,
            fun d2 rr2 h2 => absurd (Option.some.inj h2) (by simp)⟩
          refine ⟨finE, SysRuns.step _ _ _ _ _
            (SysStep.dispatchFresh (echoToE cfg) (echoReady r) (rest.map echoReady)
              (by simp [echoToE, hcur])
              (by simp [echoToE, hrunq])
              hfresh) ?_⟩
          simp only [echoToE, hp] at hrunE ⊢
          exact hrunE
      | dispatchResumed r rest hcur hrunq hfresh =>
          rename_i cfg
          have hm : r ∈ cfg.runq := by rw [hrunq]; exact List.Mem.head _
          rw [hq r hm] at hfresh
          exact absurd hfresh (by decide)
      | fiberEffect d b ps rest r s' wk hcur hb hrunP hmap hwake =>
          rename_i cfg
          have h0 : echoPrim.run cfg.prim cfg.now d.fiber d.call
              = some (EchoResult.pong, cfg.prim, []) := rfl
          rw [h0] at hrunP
          obtain ⟨hr, htail⟩ := Prod.mk.inj (Option.some.inj hrunP)
          obtain ⟨hs, hw⟩ := Prod.mk.inj htail
          subst hs; subst hw
          have hps : ps = [] := by
            cases ps with
            | nil => rfl
            | cons p ps' => simp at hmap
          subst hps
          have hrest : rest = [] := by rw [Wakes_nil_eq hwake rfl, hp]
          subst hrest
          obtain ⟨finE, hrunE⟩ := ih ⟨rfl, hx, by
            intro x hxm
            rw [List.mem_append] at hxm
            rcases hxm with hxm | hxm
            · exact hq x hxm
            · exact absurd hxm (by simp),
            fun d2 rr2 h2 => by injection Option.some.inj h2 with _ hrr⟩
          -- the section is silent on the encoding side too: the maps erase
          -- the running/returning distinction
          refine ⟨finE, ?_⟩
          simp [echoToE, echoRun, FSlot.fiber, FSlot.call, hcur] at hrunE ⊢
          exact hrunE
      | fiberDone d r hcur =>
          rename_i cfg
          have hrp := hcurS d r hcur
          subst hrp
          obtain ⟨finE, hrunE⟩ := ih ⟨hp, hx, hq, fun d2 rr2 h2 => by simp at h2⟩
          refine ⟨finE, SysRuns.step _ _ _ _ _
            (SysStep.complete (echoToE cfg) (echoRun (FSlot.returning d EchoResult.pong))
              SubVal.unit EchoResult.pong (by simp [echoToE, hcur]) rfl rfl) ?_⟩
          simp [echoToE, echoRun, FSlot.fiber, FSlot.call] at hrunE ⊢
          exact hrunE
      | runPark _ _ _ _ _ _ _ hrunP _ _ _ =>
          exact absurd hrunP (by simp [echoPrim])
      | finishDone _ _ _ _ _ _ _ _ _ hfin _ _ =>
          exact absurd hfin (by simp [echoPrim])
      | extApply _ _ _ _ hcap _ =>
          simp [echoPrim] at hcap
      | extEffect _ _ _ _ _ _ _ _ _ _ hrunE _ _ =>
          exfalso
          simp [echoPrim] at hrunE
      | extDone _ _ _ _ hsplit _ =>
          exfalso
          rw [hx] at hsplit
          simp at hsplit
      | envTime tk hcur hle =>
          rename_i cfg
          obtain ⟨finE, hrunE⟩ := ih ⟨hp, hx, hq, hcurS⟩
          refine ⟨finE, SysRuns.step _ _ _ _ _ (SysStep.envTime (echoToE cfg) tk
            (by simp [echoToE, hcur]) (by simpa only [echoToE] using hle)) ?_⟩
          simp only [echoToE, hp, hcur] at hrunE ⊢
          exact hrunE
      | envExpire _ _ preP postP p _ _ hparked _ =>
          have h0 : preP ++ p :: postP = [] := by rw [← hparked]; exact hp
          obtain ⟨-, hcontra⟩ := append_eq_nil_split h0
          simp at hcontra

set_option linter.unusedVariables false in
set_option linter.unusedSimpArgs false in
/-- Every disciplined echo encoding run is matched, step for step and
observation for observation, by the primitive. -/
theorem sim_echo_bwd :
    ∀ (cfg : SysCfg BaseOpsSig.none EchoSig) (t : Trace EchoSig) (fin : SysCfg BaseOpsSig.none EchoSig),
      SysRuns BaseOpsSig.none EchoSig echoEnc SubStep cfg t fin → EncShapeEcho cfg →
      ∃ finP, PrimRuns2 EchoSig echoPrim (echoToP cfg) t finP := by
  intro cfg t fin hrun
  induction hrun with
  | stop cfg => intro _; exact ⟨echoToP cfg, PrimRuns2.stop _⟩
  | step cfg cfg' o t2 fin2 hstep hrest ih =>
      intro hshape
      obtain ⟨hp, hx, hq, hcur⟩ := hshape
      cases hstep with
      | submit f c hsub =>
          rename_i cfg
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, hx, by
            intro x hxm
            rw [List.mem_append] at hxm
            rcases hxm with hxm | hxm
            · exact hq x hxm
            · simp only [List.mem_singleton] at hxm
              subst hxm
              exact ⟨rfl, rfl⟩, hcur⟩
          refine ⟨finP, PrimRuns2.step _ _ _ _ _ (PrimStep2.submit (echoToP cfg) f c hsub) ?_⟩
          simp only [echoToP, hp, List.map_append] at hrunP ⊢
          exact hrunP
      | dispatchFresh r rest hcurN hrunq hfresh =>
          rename_i cfg
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, hx, by
            intro x hxm
            exact hq x (by rw [hrunq]; exact List.Mem.tail _ hxm), by
              intro tR ht
              injection ht with ht2
              obtain ⟨rfl⟩ := ht2
              exact (hq r (by rw [hrunq]; exact List.Mem.head _)).2⟩
          refine ⟨finP, PrimRuns2.step _ _ _ _ _
            (PrimStep2.dispatchFresh (echoToP cfg) (echoRunqP r) (rest.map echoRunqP) ()
              (by simp [echoToP, hcurN])
              (by simp [echoToP, hrunq])
              hfresh rfl) ?_⟩
          simp only [echoToP] at hrunP ⊢
          exact hrunP
      | dispatchResumed r rest hcurN hrunq hfresh =>
          rename_i cfg
          have hm : r ∈ cfg.runq := by rw [hrunq]; exact List.Mem.head _
          rw [(hq r hm).1] at hfresh
          exact absurd hfresh (by decide)
      | subOpStep tR _ _ _ _ hcurT hprog _ _ =>
          have hpc := hcur tR hcurT
          rw [hprog] at hpc
          simp at hpc
      | subOpWake tR _ _ _ _ _ _ _ _ _ _ hcurT hprog _ _ _ _ _ =>
          have hpc := hcur tR hcurT
          rw [hprog] at hpc
          simp at hpc
      | suspendBlock tR _ _ hcurT hprog _ =>
          have hpc := hcur tR hcurT
          rw [hprog] at hpc
          simp at hpc
      | suspendConsume tR _ _ _ hcurT hprog _ =>
          have hpc := hcur tR hcurT
          rw [hprog] at hpc
          simp at hpc
      | baseRunNone tR _ _ _ _ hcurT hprog _ =>
          have hpc := hcur tR hcurT
          rw [hprog] at hpc
          simp at hpc
      | baseRunWake1 tR _ _ _ _ _ _ _ _ _ _ _ hcurT hprog _ _ _ _ =>
          have hpc := hcur tR hcurT
          rw [hprog] at hpc
          simp at hpc
      | basePark tR _ _ _ hcurT hprog _ _ =>
          have hpc := hcur tR hcurT
          rw [hprog] at hpc
          simp at hpc
      | complete tR v r hcurT hprog hdec =>
          rename_i cfg
          have hpc := hcur tR hcurT
          rw [hpc] at hprog
          injection hprog with hv
          subst hv
          have h0 : echoEnc.decode tR.call SubVal.unit = some EchoResult.pong := rfl
          rw [h0] at hdec
          injection hdec with hr
          subst hr
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, hx, hq, by
            intro tR2 ht2
            exact absurd ht2 (by simp)⟩
          refine ⟨finP, ?_⟩
          have he1 : PrimStep2 EchoSig echoPrim (echoToP cfg) none (echoMid cfg tR) :=
            PrimStep2.fiberEffect (echoToP cfg) { fiber := tR.fiber, call := tR.call } false
              [] [] EchoResult.pong () []
              (by simp only [echoToP]; rw [hcurT]; rfl) rfl rfl rfl Wakes.nil
          have he2 : PrimStep2 EchoSig echoPrim (echoMid cfg tR)
              (some (compObs EchoSig (Caller.fiber tR.fiber) tR.call EchoResult.pong))
              { prim := (echoMid cfg tR).prim, now := (echoMid cfg tR).now, cur := none,
                parked := (echoMid cfg tR).parked, runq := (echoMid cfg tR).runq,
                retired := if tR.fiber ∈
                    (echoMid cfg tR).retired then (echoMid cfg tR).retired
                  else tR.fiber :: (echoMid cfg tR).retired,
                nextFiber := (echoMid cfg tR).nextFiber, exts := (echoMid cfg tR).exts } :=
            PrimStep2.fiberDone (echoMid cfg tR) { fiber := tR.fiber, call := tR.call }
              EchoResult.pong rfl
          have hfin : PrimRuns2 EchoSig echoPrim
              { prim := (echoMid cfg tR).prim, now := (echoMid cfg tR).now, cur := none,
                parked := (echoMid cfg tR).parked, runq := (echoMid cfg tR).runq,
                retired := if tR.fiber ∈
                    (echoMid cfg tR).retired then (echoMid cfg tR).retired
                  else tR.fiber :: (echoMid cfg tR).retired,
                nextFiber := (echoMid cfg tR).nextFiber, exts := (echoMid cfg tR).exts }
              t2 finP := by
            simpa [echoMid, echoToP, echoFin] using hrunP
          exact PrimRuns2.step (echoToP cfg) (echoMid cfg tR) none
            (compObs EchoSig (Caller.fiber tR.fiber) tR.call EchoResult.pong :: ([] ++ t2)) _
            he1 (PrimRuns2.step (echoMid cfg tR) _ (some (compObs EchoSig (Caller.fiber tR.fiber) tR.call EchoResult.pong)) _ _ he2 hfin)
      | extStart _ _ hcap _ =>
          simp [echoEnc] at hcap
      | extSubOpStep _ _ _ _ _ _ _ hsplit _ _ _ _ =>
          exfalso
          rw [hx] at hsplit
          simp at hsplit
      | extSubOpWake _ _ _ _ _ _ _ _ _ _ _ _ _ hsplit _ _ _ _ _ _ _ =>
          exfalso
          rw [hx] at hsplit
          simp at hsplit
      | extComplete _ _ _ _ _ hsplit _ _ =>
          exfalso
          rw [hx] at hsplit
          simp at hsplit
      | envTime tk hcurN hle =>
          rename_i cfg
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, hx, hq, by
            intro tR2 ht2
            exact absurd ht2 (by simp)⟩
          refine ⟨finP, PrimRuns2.step _ _ _ _ _ (PrimStep2.envTime (echoToP cfg) tk
            (by simp [echoToP, hcurN]) hle) ?_⟩
          simp only [echoToP, hcurN] at hrunP ⊢
          exact hrunP
      | envExpire _ _ _ preP postP p _ _ _ _ hparked _ =>
          have h0 : preP ++ p :: postP = [] := by rw [← hparked]; exact hp
          obtain ⟨-, hcontra⟩ := append_eq_nil_split h0
          simp at hcontra

/-- The stateless echo primitive is observationally equivalent, over the
disciplined trace languages, to its encoding over the bare substrate. -/
theorem echoRed : Reduction echoPrim echoEnc where
  fwd := by
    rintro t ⟨⟨fin, hrun⟩, hseq⟩
    obtain ⟨finE, hrunE⟩ :=
      sim_echo_fwd _ _ _ hrun (by
        refine ⟨rfl, rfl, ?_, ?_⟩
        · intro r hr
          cases hr
        · intro d rr h
          contradiction)
    exact ⟨⟨finE, by rw [echoToE_primInit] at hrunE; exact hrunE⟩, hseq⟩
  bwd := by
    rintro t ⟨⟨fin, hrun⟩, hseq⟩
    obtain ⟨finP, hrunP⟩ :=
      sim_echo_bwd _ _ _ hrun (by
        refine ⟨rfl, rfl, ?_, ?_⟩
        · intro r hr
          cases hr
        · intro tR ht
          cases ht)
    exact ⟨⟨finP, by rw [echoToP_encInit] at hrunP; exact hrunP⟩, hseq⟩

/-- The echo primitive is reducible to the bare substrate through `echoEnc`
with the empty obligation list. -/
theorem echo_reducibleTo : ReducibleTo echoPrim BaseOpsSig.none echoEnc [] :=
  ⟨echoRed, obligations_preserved_of_reduction echoRed []⟩

/-- `REDUCIBLE(echo, BASE(echo) = {})`. -/
theorem echo_reducible : Reducible echoPrim BaseOpsSig.none [] :=
  ⟨echoEnc, echo_reducibleTo⟩

/-- The echo primitive is not irreducible: a reduction over the base
witnesses the opposite. -/
theorem echo_not_irreducible : ¬ IrreducibleTo echoPrim BaseOpsSig.none :=
  fun h => not_reducible_of_irreducible h echo_reducible

/-! ## The execution-domain gate bites (V2.2)

An encoding that declares a fiber-only call externally capable over-produces
against the domain probe primitive: the machine lets an external caller run
the call, but the primitive has no external path for it.  The domain
declaration is therefore part of every stage's adjudicated surface. -/

/-- The lying encoding over the domain probe API: `extCap` claims every
call, including the fiber-only `go`. -/
def lieTrace : Trace DomSig :=
  [issueObs DomSig (Caller.ext 0) DomCall.go,
    compObs DomSig (Caller.ext 0) DomCall.go DomResult.done]

def le0 : SysCfg BaseOpsSig.none DomSig := encInit BaseOpsSig.none DomSig

def le1 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [],
    nextFiber := 0, exts := [{ x := 0, call := DomCall.go, prog := ExtProg.pure SubVal.unit }] }

def le2 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [],
    nextFiber := 0, exts := [] }

theorem lie1 : SysStep BaseOpsSig.none DomSig encLie SubStep le0
    (some (issueObs DomSig (Caller.ext 0) DomCall.go)) le1 :=
  SysStep.extStart le0 0 DomCall.go rfl
    (by show 0 ∉ ([] : List (ExtBusy BaseOpsSig.none DomSig)).map (fun e : ExtBusy BaseOpsSig.none DomSig => e.x); simp)

theorem lie2 : SysStep BaseOpsSig.none DomSig encLie SubStep le1
    (some (compObs DomSig (Caller.ext 0) DomCall.go DomResult.done)) le2 :=
  SysStep.extComplete le1 [] [] { x := 0, call := DomCall.go, prog := ExtProg.pure SubVal.unit }
    SubVal.unit DomResult.done rfl rfl rfl

theorem encLie_possesses_lie : TracesEnc BaseOpsSig.none DomSig encLie lieTrace :=
  ⟨le2, SysRuns.step le0 le1 _ _ le2 lie1
    (SysRuns.step le1 le2 _ [] le2 lie2 (SysRuns.stop le2))⟩

theorem seqOK_lieTrace : SeqOK DomSig lieTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

set_option linter.unusedVariables false in
/-- Inverting a primitive step that emits an external issue: it is an
`extApply` entry of a call in the primitive's external domain. -/
theorem extIssue_inv {m1 m2 : PrimCfg DomSig domPrim} {ob : Obs DomSig}
    (hstep : PrimStep2 DomSig domPrim m1 (some ob) m2)
    (hres : ob.result = none) (hcl : ob.caller = Caller.ext 0) :
    ∃ (x : ExternalId) (c : DomCall) (preE postE : List (ExtPend DomSig)),
      ob = issueObs DomSig (Caller.ext x) c ∧ domPrim.extCap c = true := by
  cases hstep with
  | dispatchFresh r0 rest s0 hcur hrq hfr hadm =>
      exact absurd hcl (by simp [issueObs])
  | fiberDone d r hcur =>
      exact absurd hcl (by simp [compObs])
  | finishDone _ _ _ _ _ _ _ _ _ _ _ =>
      exact absurd hcl (by simp [compObs])
  | extApply x c preE postE hcap hx =>
      exact ⟨x, c, preE, postE, rfl, hcap⟩
  | extDone preE postE e r hsplit hsome =>
      exact absurd hres (by simp [compObs])

/-- The primitive can never produce the lie: an external issue of `go`
would require `domPrim.extCap go = true`. -/
theorem prim_not_lieTrace : ¬ TracesPrim DomSig domPrim lieTrace := by
  rintro ⟨fin, hrun⟩
  obtain ⟨m1, m2, -, hstep, -⟩ :=
    primRuns_cons hrun (issueObs DomSig (Caller.ext 0) DomCall.go)
      [compObs DomSig (Caller.ext 0) DomCall.go DomResult.done] rfl
  obtain ⟨x, c, preE, postE, hob, hcap⟩ :=
    extIssue_inv hstep rfl rfl
  injection hob with hcaller hcall _
  injection hcaller with hx
  subst hx
  subst hcall
  simp [domPrim] at hcap

/-- The lying encoding over-produces: the domain declaration bites. -/
theorem encLie_overProduces : OverProduces domPrim BaseOpsSig.none encLie :=
  ⟨lieTrace, ⟨encLie_possesses_lie, seqOK_lieTrace⟩, fun h => prim_not_lieTrace h.1⟩

end Sluice.Formal
