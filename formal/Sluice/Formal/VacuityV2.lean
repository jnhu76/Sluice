/-
Sluice Stage-0-V2 vacuity certificate.

The stateless echo primitive (`echoPrim`) is observationally equivalent,
over the disciplined trace languages (`TracesPrimS` / `TracesEncS`), to its
encoding over the bare substrate (`echoEnc`, `BASE(echo) = {}`).  Both
sides run under the same call-execution discipline; the equivalence is a
pair of step-by-step simulations between the configurations reachable in
echo runs, composed with the initial-configuration agreement of the two
configuration maps.
-/

import Sluice.Formal.JudgeV2

namespace Sluice.Formal

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
dispatch, nothing parks, and the clock mirror is the identity. -/
def echoPrim : PrimLTS2 EchoSig :=
  { State := Unit
    init := ()
    admit := fun s _ _ => some s
    run := fun s _ _ _ => some (EchoResult.pong, s, [])
    park := fun _ _ _ => none
    finish := fun _ _ _ => none
    onTick := fun s _ => s
    expire := fun _ _ _ => none }

/-- The echo encoding over the bare substrate: the trivial immediately-
returning program, decoded to `pong`. -/
def echoEnc : Encoding BaseOpsSig.none EchoSig :=
  { prog := fun _ => ExtProg.pure SubVal.unit
    decode := fun _ _ => some EchoResult.pong }

/-! ## Reachable configuration shapes

In an echo run nothing ever parks, so no runnable entry is ever resumed and
the only reachable steps are `submit`, `dispatchFresh`, the inline
completion (`runDone` on the primitive side, `complete` on the encoding
side) and the idle `envTime`. -/

/-- Shape of a primitive echo configuration: nothing parked, and every
runnable entry is a fresh submission. -/
def PrimShapeEcho (cfg : PrimCfg EchoSig echoPrim) : Prop :=
  cfg.parked = [] ∧ ∀ r ∈ cfg.runq, r.fresh = true

/-- Shape of an encoding echo configuration: nothing parked, and every
program (queued or running) is the trivial immediately-returning one. -/
def EncShapeEcho (cfg : SysCfg BaseOpsSig.none EchoSig) : Prop :=
  cfg.parked = [] ∧
  (∀ r ∈ cfg.runq, r.fresh = true ∧ r.prog = ExtProg.pure SubVal.unit) ∧
  (∀ t : Running BaseOpsSig.none EchoSig, cfg.cur = some t → t.prog = ExtProg.pure SubVal.unit)

/-- A queued primitive call, as an encoding-side runnable entry. -/
def echoReady (r : PReady EchoSig) : Ready BaseOpsSig.none EchoSig :=
  { fiber := r.fiber, call := r.call, prog := ExtProg.pure SubVal.unit, fresh := r.fresh }

/-- The in-flight primitive call, as the encoding-side running fiber. -/
def echoRun (d : Pnd EchoSig × Bool) : Running BaseOpsSig.none EchoSig :=
  { fiber := d.1.fiber, call := d.1.call, prog := ExtProg.pure SubVal.unit }

/-- An encoding-side runnable entry, as a queued primitive call. -/
def echoRunqP (r : Ready BaseOpsSig.none EchoSig) : PReady EchoSig :=
  { fiber := r.fiber, call := r.call, fresh := r.fresh }

/-- The encoding-side running fiber, as the in-flight primitive call; the
call is always a freshly dispatched one (echo never resumes). -/
def echoCur (t : Running BaseOpsSig.none EchoSig) : Pnd EchoSig × Bool :=
  ({ fiber := t.fiber, call := t.call }, false)

/-- Map a primitive echo configuration to the encoding side. -/
def echoToE (cfg : PrimCfg EchoSig echoPrim) : SysCfg BaseOpsSig.none EchoSig :=
  { st := { subInit with now := cfg.now }
    bst := ()
    cur := cfg.cur.map echoRun
    parked := []
    runq := cfg.runq.map echoReady
    retired := cfg.retired
    nextFiber := cfg.nextFiber }

/-- Map an encoding echo configuration back to the primitive side. -/
def echoToP (cfg : SysCfg BaseOpsSig.none EchoSig) : PrimCfg EchoSig echoPrim :=
  { prim := ()
    now := cfg.st.now
    cur := cfg.cur.map echoCur
    parked := []
    runq := cfg.runq.map echoRunqP
    retired := cfg.retired
    nextFiber := cfg.nextFiber }

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
      obtain ⟨hp, hq⟩ := hshape
      cases hstep with
      | submit f c hsub =>
          rename_i cfg
          obtain ⟨finE, hrunE⟩ := ih ⟨hp, by
            intro x hx
            rw [List.mem_append] at hx
            rcases hx with hx | hx
            · exact hq x hx
            · simp only [List.mem_singleton] at hx
              subst hx
              rfl⟩
          refine ⟨finE, SysRuns.step _ _ _ _ _ (SysStep.submit (echoToE cfg) f c hsub) ?_⟩
          simp only [echoToE, hp, List.map_append] at hrunE ⊢
          exact hrunE
      | dispatchFresh r rest s' hcur hrunq hfresh hadmit =>
          rename_i cfg
          obtain ⟨finE, hrunE⟩ := ih ⟨hp, by
            intro x hx
            refine hq x ?_
            rw [hrunq]
            exact List.Mem.tail _ hx⟩
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
      | runDone d preP postP ps r s' wk hcur hf hrunP hparked hmap =>
          rename_i cfg
          have hnil : preP ++ ps ++ postP = [] := by rw [← hparked]; exact hp
          obtain ⟨hprePps, hpostP⟩ := append_eq_nil_split hnil
          obtain ⟨hpreP, hps⟩ := append_eq_nil_split hprePps
          subst hpreP; subst hps; subst hpostP
          have h0 : echoPrim.run cfg.prim cfg.now d.1.fiber d.1.call
              = some (EchoResult.pong, cfg.prim, []) := rfl
          rw [h0] at hrunP
          obtain ⟨hr, htail⟩ := Prod.mk.inj (Option.some.inj hrunP)
          obtain ⟨hs, hw⟩ := Prod.mk.inj htail
          subst hs; subst hw
          rw [← hr]
          obtain ⟨finE, hrunE⟩ := ih ⟨rfl, by
            intro x hx
            rw [List.mem_append] at hx
            rcases hx with hx | hx
            · exact hq x hx
            · exact absurd hx (by simp)⟩
          refine ⟨finE, SysRuns.step _ _ _ _ _
            (SysStep.complete (echoToE cfg) (echoRun d) SubVal.unit EchoResult.pong
              (by simp [echoToE, hcur]) rfl rfl) ?_⟩
          simp only [echoToE, List.map_append, List.map_nil, List.append_nil, Option.map_none] at hrunE ⊢
          exact hrunE
      | runPark _ _ _ hrunP _ =>
          exact absurd hrunP (by simp [echoPrim])
      | finishDone _ _ _ _ _ _ _ _ _ hfin _ _ =>
          exact absurd hfin (by simp [echoPrim])
      | envTime tk hcur hle =>
          rename_i cfg
          obtain ⟨finE, hrunE⟩ := ih ⟨hp, hq⟩
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
      obtain ⟨hp, hq, hcur⟩ := hshape
      cases hstep with
      | submit f c hsub =>
          rename_i cfg
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, by
            intro x hx
            rw [List.mem_append] at hx
            rcases hx with hx | hx
            · exact hq x hx
            · simp only [List.mem_singleton] at hx
              subst hx
              exact ⟨rfl, rfl⟩, hcur⟩
          refine ⟨finP, PrimRuns2.step _ _ _ _ _ (PrimStep2.submit (echoToP cfg) f c hsub) ?_⟩
          simp only [echoToP, hp, List.map_append] at hrunP ⊢
          exact hrunP
      | dispatchFresh r rest hcurN hrunq hfresh =>
          rename_i cfg
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, by
            intro x hx
            exact hq x (by rw [hrunq]; exact List.Mem.tail _ hx), by
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
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, hq, by
            intro tR2 ht2
            exact absurd ht2 (by simp)⟩
          refine ⟨finP, PrimRuns2.step _ _ _ _ _
            (PrimStep2.runDone (echoToP cfg) (echoCur tR) [] [] [] EchoResult.pong () []
              (by simp [echoToP, hcurT]) rfl rfl rfl rfl) ?_⟩
          simp only [echoToP, List.map_nil, List.append_nil, Option.map_none] at hrunP ⊢
          exact hrunP
      | envTime tk hcurN hle =>
          rename_i cfg
          obtain ⟨finP, hrunP⟩ := ih ⟨hp, hq, by
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
      sim_echo_fwd _ _ _ hrun ⟨rfl, fun r hr => nomatch hr⟩
    exact ⟨⟨finE, by rw [echoToE_primInit] at hrunE; exact hrunE⟩, hseq⟩
  bwd := by
    rintro t ⟨⟨fin, hrun⟩, hseq⟩
    obtain ⟨finP, hrunP⟩ :=
      sim_echo_bwd _ _ _ hrun (by
        refine ⟨rfl, ?_, ?_⟩
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

end Sluice.Formal
