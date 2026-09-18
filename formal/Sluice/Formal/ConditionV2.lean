/-
Sluice Stage 4V2.3 — AsyncCondition core (FCB1-METHOD-CORRECTIVE-1).

This stage re-adjudicates the AsyncCondition core on the post-#378
Stage-0-V2.3 authority as amended (`CalcV2.lean`: `park` publishes the
wakes its pre-suspension section readied, and the section-ending steps
take wakes as an order-preserving `Wakes` sublist — the Stage-4 brake
amendment), from the current production code:

  `include/sluice/async/condition.hpp`, `src/async/scheduler_condition.cpp`.

Call-domain census (from the code):

  `wait`        fiber-bound (`AsyncCondition::wait` reads `g_worker` via
                `condition_wait_prepare` :91-130; the owner precondition
                assert at :98); one call: register + release-mutex-with-
                handoff + suspend, then reacquire on wake.
  `notify_one`  external-capable (`condition_notify_one` :145-148, only
                `global_mtx_`); wakes at most the queue head.
  `notify_all`  external-capable (`condition_notify_all` :150-162, only
                `global_mtx_`); drains the queue (returns the count).
  `cancel`      external-capable (`condition_cancel_wait` :164-174, only
                `global_mtx_` + the queue mutex); publishes `cancelled`
                into the queued node.
  `wait_until`  fiber-bound timed extension, outside the core surface
                (same disposition as `acquire_until`/`lock_until`).

Modeling disclosures (each preserves the reachable trace language):

  * The modeled `wait` result is the `WaitNode` outcome the caller
    inspects after the call returns: `true` = woken (by notify), `false`
    = cancelled.  Without that bit the ownership accounting of the
    reacquire is not statable.
  * The modeled wait node is per-call: the C++ `resolved_inline_released`
    corner (`condition_wait_admit_locked` :55-57, a caller-supplied node
    already terminal when `wait` enters) requires a node shared across
    calls, which the one-in-flight-call discipline excludes; the model
    has no such trace and neither can its discipline produce one.
  * The condition's embedded mutex slot (`owner_`) is modeled only for
    the wait-reacquire discipline: the C++ `mutex_.lock(reacquire_node)`
    at condition.hpp:96-98 re-enters the *mutex* primitive, which has
    its own Stage-3 model.  Here the owner slot transfers at a
    `wait`-entry release (handoff to a reacquire-blocked waiter) and at
    a reacquire (take of a free slot); plain `lock`/`unlock` traffic is
    the mutex primitive's business, not this surface's.
  * A reacquire that blocks parks the waiter on the mutex queue as a
    *second suspension of the same call* (the Mesa reacquire); the
    per-fiber phase (`cwFresh` → `cwWaiting` → `cwReacq`) makes the
    call-keyed facets distinguish the two park sites.

Adjudication:

  * AsyncCondition — THEOREM B (`cond_irreducible`): irreducible to
    `BASE(AsyncCondition) = {Mutex}`.  The separating fact is the
    owner-gated *admission* of `wait` (the `assert(owner == me)` at
    scheduler_condition.cpp:98): the primitive never issues a `wait`
    from a non-owner — `condAdmit` refuses the dispatch — while every
    encoding's fiber machine dispatches any submitted call
    unconditionally.  The witness `waitIssueTrace` (a bare `wait` issue
    by a fresh fiber) is produced by every encoding
    (`cond_over_produces`) and by no run of the primitive
    (`cond_not_waitIssue`).
  * Safety evidence: `condWakeBacked` — at every prefix, completed
    `wait` calls are covered by the wakes their resolvers can have
    published (`condWakeCredit`: a `notify_one`/`cancel` completion
    credits at most one wake, a `notify_all` completion credits exactly
    its drained count, which its result observation carries) — plus the
    conservation mirror `cond_mirror` over the embedded mutex slot.
-/

import Sluice.Formal.JudgeV2
import Sluice.Formal.MutexV2

namespace Sluice.Formal

/-! ## API and state -/

inductive CondCall : Type where
  | cwait
  | cnotifyOne
  | cnotifyAll
  | ccancel (f : FiberId)
deriving instance DecidableEq for CondCall

inductive CondRes : Type where
  | rout (b : Bool)
  | runit
  | rall (k : Nat)
deriving instance DecidableEq for CondRes

abbrev CondSig : ApiSig := ⟨CondCall, CondRes⟩

/-- Where a `wait` call is in its two-suspension lifecycle. -/
inductive CPhase where
  | cwFresh
  | cwWaiting
  | cwReacq

/-- AsyncCondition private state: the shared mutex's owner slot
(`mutex_.owner_`), the mutex queue (reacquire-blocked waiters), the
condition queue (`AsyncCondition::waiters_`), the per-fiber wait phase,
and the resolved-but-unconsumed outcomes (the one-shot
`WaitNode::resolve_` states of woken or cancelled waiters awaiting their
resume). -/
structure CondState where
  owner : Option FiberId
  mwaitq : List FiberId
  cwaitq : List FiberId
  phase : FiberId → CPhase
  resolved : List (FiberId × Bool)

def condInit : CondState :=
  { owner := none, mwaitq := [], cwaitq := [], phase := fun _ => CPhase.cwFresh,
    resolved := [] }

/-- Remove the first occurrence of `w` from the condition queue (the
`WaitQueue::unlink_locked` shape). -/
def condRemove (l : List FiberId) (w : FiberId) : Option (List FiberId) :=
  match l with
  | [] => none
  | h :: t => if h = w then some t else (condRemove t w).map (h :: ·)

/-- Remove the first record of `f` from the resolved list, yielding the
record's outcome. -/
def condConsume (l : List (FiberId × Bool)) (f : FiberId) :
    Option (Bool × List (FiberId × Bool)) :=
  match l with
  | [] => none
  | (g, b) :: t => if g = f then some (b, t) else (condConsume t f).map (fun p => (p.1, (g, b) :: p.2))

/-! ## Admission -/

/-- Entry admission: only the mutex owner may `wait` (the
`assert(owner == me)`, scheduler_condition.cpp:98); notify and cancel
have no caller precondition. -/
def condAdmit : CondState → FiberId → CondCall → Option CondState
  | s, f, CondCall.cwait =>
      if s.owner = some f then
        some { s with phase := fun g => if g = f then CPhase.cwFresh else s.phase g }
      else none
  | s, _, _ => some s

/-! ## The inline paths (fresh dispatch) -/

/-- A fresh `wait` always suspends (its section runs in the park; see the
disclosures for the omitted pre-resolved corner).  Fiber-origin notify
and cancel complete inline, exactly as the C++ entry points serve any
caller without suspending. -/
def condRun : CondState → Tick → FiberId → CondCall →
    Option (CondRes × CondState × List FiberId)
  | s, _, _, CondCall.cwait => none
  | s, _, _, CondCall.cnotifyOne =>
      match s.cwaitq with
      | [] => some (CondRes.runit, s, [])
      | h :: t => some (CondRes.runit,
        { s with cwaitq := t, resolved := s.resolved ++ [(h, true)] }, [h])
  | s, _, _, CondCall.cnotifyAll =>
      some (CondRes.rall s.cwaitq.length,
        { s with cwaitq := [], resolved := s.resolved ++ s.cwaitq.map (fun g => (g, true)) },
        s.cwaitq)
  | s, _, _, CondCall.ccancel w =>
      match condRemove s.cwaitq w with
      | some t => some (CondRes.rout true,
        { s with cwaitq := t, resolved := s.resolved ++ [(w, false)] }, [w])
      | none => some (CondRes.rout false, s, [])

/-! ## The suspension (the release-handoff section) -/

/-- Parking a `wait`: in `cwFresh`, the section registers the waiter and
releases the mutex — handing it to the first reacquire-blocked waiter if
one sits in the mutex queue (the wake the amended `park` publishes) —
then suspends.  In `cwWaiting` (resumed but the mutex is held), the
Mesa reacquire parks on the mutex queue instead. -/
def condPark : CondState → FiberId → CondCall →
    Option (CondState × List FiberId)
  | s, f, CondCall.cwait =>
      match s.phase f with
      | CPhase.cwFresh =>
          let st :=
            { s with
              cwaitq := s.cwaitq ++ [f],
              phase := fun g => if g = f then CPhase.cwWaiting else s.phase g }
          match s.mwaitq with
          | [] => some (st, [])
          | g :: t => some ({ st with owner := some g, mwaitq := t }, [g])
      | CPhase.cwWaiting =>
          some ({ s with
            mwaitq := s.mwaitq ++ [f],
            phase := fun g => if g = f then CPhase.cwReacq else s.phase g },
            [])
      | CPhase.cwReacq => none
  | _, _, _ => none

/-! ## The resume (the reacquire) -/

/-- A resumed `wait` in `cwWaiting` reacquires: with a free mutex it
takes the slot and completes with its recorded outcome; with the mutex
held it must park again (the `condPark` `cwWaiting` branch).  In
`cwReacq` the mutex came with the handoff, so the call completes. -/
def condFinish : CondState → FiberId → CondCall →
    Option (CondRes × CondState × List FiberId)
  | s, f, CondCall.cwait =>
      match s.phase f with
      | CPhase.cwWaiting =>
          match condConsume s.resolved f with
          | some (b, rest) =>
              match s.owner with
              | none =>
                  some (CondRes.rout b,
                    { s with
                      owner := some f,
                      resolved := rest,
                      phase := fun g => if g = f then CPhase.cwFresh else s.phase g },
                    [])
              | some _ => none
          | none => none
      | CPhase.cwReacq =>
          match condConsume s.resolved f with
          | some (b, rest) =>
              some (CondRes.rout b,
                { s with
                  resolved := rest,
                  phase := fun g => if g = f then CPhase.cwFresh else s.phase g },
                [])
          | none => none
      | CPhase.cwFresh => none
  | _, _, _ => none

/-! ## The external surface -/

/-- `wait` never runs off a fiber; notify and cancel are
`global_mtx_`-only entry points. -/
def condExtCap : CondCall → Bool
  | CondCall.cwait => false
  | _ => true

/-- The external sections: `notify_one` wakes at most the head,
`notify_all` drains the queue, `cancel` publishes `cancelled` into the
named waiter's node. -/
def condExtRun : CondCall → CondState → Tick →
    Option (CondRes × CondState × List FiberId)
  | CondCall.cnotifyOne, s, _ =>
      match s.cwaitq with
      | [] => some (CondRes.runit, s, [])
      | h :: t => some (CondRes.runit,
        { s with cwaitq := t, resolved := s.resolved ++ [(h, true)] }, [h])
  | CondCall.cnotifyAll, s, _ =>
      some (CondRes.rall s.cwaitq.length,
        { s with cwaitq := [], resolved := s.resolved ++ s.cwaitq.map (fun g => (g, true)) },
        s.cwaitq)
  | CondCall.ccancel w, s, _ =>
      match condRemove s.cwaitq w with
      | some t => some (CondRes.rout true,
        { s with cwaitq := t, resolved := s.resolved ++ [(w, false)] }, [w])
      | none => some (CondRes.rout false, s, [])
  | CondCall.cwait, _, _ => none

/-- The condition has no timers in the untimed core. -/
def condExpire : CondState → Tick → FiberId → Option CondState :=
  fun _ _ _ => none

/-! ## The primitive -/

/-- The primitive, reducible-annotated so its facet equations reduce in
proofs. -/
@[reducible] def condPrim : PrimLTS2 CondSig :=
  { State := CondState
    init := condInit
    admit := condAdmit
    run := condRun
    park := condPark
    finish := condFinish
    extCap := condExtCap
    extRun := condExtRun
    onTick := fun s _ => s
    expire := condExpire }



/-! ## THEOREM B: irreducibility to the mutex base -/

/-- The frozen base for this stage: the Stage-3 mutex core as base
capabilities (`BASE(AsyncCondition) = {Mutex}`).  The oracle facets are
the adjudicated `mutexPrim` model's own relations; composition
faithfulness is the base stage's model-to-code mapping and verdict. -/
def MutexOps : BaseOpsSig where
  Op := MutexCall
  Res := fun _ => MutexRes
  St := MutexState
  baseInit := mutexPrim.init
  run := fun o s t f => mutexPrim.run s t f o
  -- the base-op park drops wakes: the encoding machine's base
  -- invocations readies nothing (irrelevant to the separator)
  park := fun o s f => (mutexPrim.park s f o).map (fun p => p.1)
  resume := fun o s f => mutexPrim.finish s f o

/-- Non-vacuity witness for the THEOREM-B universal: the closed encoding
class over the mutex substrate is inhabited.  `wait` maps to the
natural reacquire shape (take the mutex), notify and cancel bottom out
with no completed observation — the condition queue is private state
this substrate's program language cannot hold.  Inhabitance is what
THEOREM B needs: the over-production fires at the encoding machine's
dispatch step, before any program runs. -/
def encCondMutex : Encoding MutexOps CondSig where
  prog
    | CondCall.cwait =>
        ExtProg.base MutexCall.mlock (fun _ => ExtProg.pure (SubVal.bool true))
    | _ => ExtProg.pure (SubVal.bool false)
  decode
    | CondCall.cwait, SubVal.bool b => some (CondRes.rout b)
    | _, _ => none
  extCap
    | CondCall.ccancel _ => true
    | _ => false

theorem cond_encoding_class_inhabited :
    ∃ _enc : Encoding MutexOps CondSig, True := ⟨encCondMutex, trivial⟩

/-- The separating witness: a bare `wait` issue by a fresh fiber.  The
owner-gated admission of `wait` (the caller precondition the primitive
enforces at dispatch, scheduler_condition.cpp:98) refuses it, while
every encoding's fiber machine dispatches any submitted call
unconditionally. -/
def waitIssueTrace : Trace CondSig :=
  [issueObs CondSig (Caller.fiber 0) CondCall.cwait]

theorem seqOK_waitIssue : SeqOK CondSig waitIssueTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- A silent prefix from an idle configuration leaves this primitive's
private state untouched: with no fiber dispatched and no external call
in flight, the only silent steps are submits and the inert clock tick. -/
theorem cond_silent_prefix {cfg m : PrimCfg CondSig condPrim} {t : Trace CondSig}
    (hrun : PrimRuns2 CondSig condPrim cfg t m) (ht : t = [])
    (hcur : cfg.cur = none) (hexts : cfg.exts = [])
    (hq : ∀ r ∈ cfg.runq, r.fresh = true) :
    m.prim = cfg.prim := by
  induction hrun with
  | stop cfg => rfl
  | step cfg cfg' o t2 fin hstep hrest ih =>
      have hnil : Option.toList o ++ t2 = [] := ht
      cases o with
      | none =>
          have ht2 : t2 = [] := by simpa [Option.toList] using hnil
          have hfwd : cfg'.prim = cfg.prim ∧ cfg'.cur = none ∧ cfg'.exts = [] ∧
              ∀ r ∈ cfg'.runq, r.fresh = true := by
            cases hstep with
            | submit f c _ =>
                exact ⟨rfl, by rw [← hcur], by rw [← hexts], by
                  intro r hr
                  rcases List.mem_append.mp hr with hm | hm
                  · exact hq r hm
                  · rw [List.mem_singleton] at hm
                    subst hm
                    rfl⟩
            | dispatchResumed rp rest hcurE hrunqE hfreshE =>
                have hhead : rp ∈ cfg.runq := by
                  rw [hrunqE]; exact List.Mem.head _
                have hfr := hq rp hhead
                rw [hfreshE] at hfr
                simp at hfr
            | fiberEffect d b ps rest r s' wk hcurE hb hrunE hmap hwake =>
                rw [hcurE] at hcur; simp at hcur
            | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
                rw [hcurE] at hcur; simp at hcur
            | extEffect preE postE e ps rest r s' wk hextsE hresE hrunE hmap hwake =>
                rw [hextsE] at hexts; simp at hexts
            | envTime t0 hcurE hle =>
                exact ⟨rfl, by simp [hcurE], by simp [hexts], hq⟩
            | envExpire f s' preP postP p hcurE hexp hparE hf =>
                exact absurd hexp (by simp [condPrim, condExpire])
          have hip := ih ht2 hfwd.2.1 hfwd.2.2.1 hfwd.2.2.2
          rw [hip]
          exact hfwd.1
      | some ob => simp at hnil

theorem cond_not_waitIssue : ¬ TracesPrim CondSig condPrim waitIssueTrace := by
  rintro ⟨fin, hrun⟩
  obtain ⟨ob, t', heq⟩ : ∃ ob t', waitIssueTrace = ob :: t' := ⟨_, _, rfl⟩
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := primRuns_cons hrun ob t' heq
  have hprim := cond_silent_prefix h1 rfl rfl rfl (by intro r hr; cases hr)
  cases hstep with
  | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
      injection heq with h3 h4
      simp only [issueObs, Obs.mk.injEq, Caller.fiber.injEq] at h3
      obtain ⟨h5, h6, _⟩ := h3
      have hcall : rp.call = CondCall.cwait := h6.symm
      have hfib : rp.fiber = 0 := h5.symm
      rw [hcall, hfib, hprim] at hadmit
      simp [condAdmit, condInit, condPrim, primInit] at hadmit
  | extApply x c preE postE hcap hnovel =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3
  | fiberDone d r hcurE =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3
  | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3
  | extDone preE postE e r hextsE hresE =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3

/-- The post-submit configuration of the encoding witness. -/
def cowMid (enc : Encoding MutexOps CondSig) : SysCfg MutexOps CondSig :=
  { st := subInit
    bst := MutexOps.baseInit
    cur := none
    parked := []
    runq := [{ fiber := 0, call := CondCall.cwait, prog := enc.prog CondCall.cwait, fresh := true }]
    retired := []
    nextFiber := 1
    exts := [] }

/-- The post-dispatch configuration of the encoding witness. -/
def cowFin (enc : Encoding MutexOps CondSig) : SysCfg MutexOps CondSig :=
  { st := subInit
    bst := MutexOps.baseInit
    cur := some { fiber := 0, call := CondCall.cwait, prog := enc.prog CondCall.cwait }
    parked := []
    runq := []
    retired := []
    nextFiber := 1
    exts := [] }

theorem enc_waitIssue (enc : Encoding MutexOps CondSig) :
    TracesEnc MutexOps CondSig enc waitIssueTrace :=
  ⟨cowFin enc,
    SysRuns.step (encInit MutexOps CondSig) (cowMid enc) none _ (cowFin enc)
      (SysStep.submit (encInit MutexOps CondSig) 0 CondCall.cwait (Or.inl rfl))
      (SysRuns.step (cowMid enc) (cowFin enc)
        (some (issueObs CondSig (Caller.fiber 0) CondCall.cwait)) [] (cowFin enc)
        (SysStep.dispatchFresh (cowMid enc)
          { fiber := 0, call := CondCall.cwait, prog := enc.prog CondCall.cwait,
            fresh := true } [] rfl rfl rfl)
        (SysRuns.stop (cowFin enc)))⟩

/-- Every encoding over the mutex base produces the bare `wait` issue:
the discipline's fiber machine submits and dispatches any call
unconditionally. -/
theorem cond_over_produces (enc : Encoding MutexOps CondSig) :
    OverProduces condPrim MutexOps enc :=
  ⟨waitIssueTrace, ⟨enc_waitIssue enc, seqOK_waitIssue⟩,
    fun hP => cond_not_waitIssue hP.1⟩

/-- **THEOREM B** — AsyncCondition is irreducible to `BASE(AsyncCondition)
= {Mutex}`: every encoding over the base over-produces the bare `wait`
issue that the primitive's owner-gated admission forbids. -/
theorem cond_irreducible : IrreducibleTo condPrim MutexOps :=
  irreducible_of_always_over cond_over_produces

end Sluice.Formal
