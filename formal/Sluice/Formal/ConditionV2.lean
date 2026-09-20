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
                `condition_wait_prepare` :73-109; the owner precondition
                assert at :79-80); one call: register + release-mutex-with-
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
  * The modeled wait node is per-call: the C++ inline-resolution
    corners — `resolved_inline_released` (`condition_wait_admit_locked`
    :67-69, a caller-supplied node already terminal when `wait` enters)
    and its sibling `rejected_retain` (:34-39, `register_wait_locked`
    rejecting a non-detached node) — both require a node shared across
    calls, which the one-in-flight-call discipline excludes; the model
    has no such trace and neither can its discipline produce one.
  * The condition's embedded mutex slot (`owner_`) is modeled only for
    the wait-reacquire discipline: the C++ `mutex_.lock(reacquire_node)`
    at condition.hpp:75 (`wait`; :91 for `wait_until`) re-enters the
    *mutex* primitive, which has
    its own Stage-3 model.  Here the owner slot transfers at a
    `wait`-entry release (handoff to a reacquire-blocked waiter, else
    freed — `owner = nullptr` at scheduler_condition.cpp:63-65) and at
    a reacquire (take of a free slot); plain `lock`/`unlock` traffic is
    the mutex primitive's business, not this surface's.  A `wait` is
    admitted only from the slot holder (the caller precondition), so
    acquiring the slot for a first `wait` is the caller side's mutex
    traffic: multi-waiter configurations are relation-reachable but not
    `primInit`-reachable, and the batteries that need them start from
    instrumented configurations.
  * A reacquire that blocks parks the waiter on the mutex queue as a
    *second suspension of the same call* (the Mesa reacquire); the
    per-fiber phase (`cwFresh` → `cwWaiting` → `cwReacq`) makes the
    call-keyed facets distinguish the two park sites.

Adjudication:

  * AsyncCondition — THEOREM B (`cond_irreducible`): irreducible to
    `BASE(AsyncCondition) = {Mutex}`.  The separating fact is the
    owner-gated *admission* of `wait` (the `assert(owner == me)` at
    scheduler_condition.cpp:79-80): the primitive never issues a `wait`
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
one sits in the mutex queue, else freeing the slot (`owner = nullptr`,
scheduler_condition.cpp:63-65) — then suspends.  In `cwWaiting` (resumed
but the mutex is held), the Mesa reacquire parks on the mutex queue
instead. -/
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
          | [] => some ({ st with owner := none }, [])
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

/-! ## Wake-backedness: the no-spurious-wake ledger

`condComps t` counts the trace's completed `wait` outcomes;
`condWakeCredit t` counts the resolver credit its completed resolver calls
carry: a `notify_one` completion credits 1 (its unit result cannot
distinguish "woke nobody" from "woke the head", so the safe upper bound is
used), a `notify_all` completion credits exactly its drained count, which
the `rall k` result observation carries, and a `cancel` completion credits
1 iff it found the named waiter (`rout true`).  The V2.3 effect/return
split makes resolver credit exist from the *effect* step on, not from the
completion: a returning fiber slot and an effect-done external record each
carry the same credit their later completion will contribute, so a
resolver's pending-to-completed transition only moves its credit between
locations.  No credit arises at entry (`extApply` records hold `none`),
at issue, or at dispatch (`running` slots credit 0).  `cond_mirror` is the
conservation statement: completed waits plus resolved-but-unconsumed
outcomes are covered by completed credit plus in-window credit.  Two side
conditions make the ledger statable: a returning slot never carries a
`wait` (only `run` builds one, and `wait` has no run path), and no
external record carries a `wait` (`condExtCap` refuses it). -/

/-- The wake credit a resolver result carries. -/
def wakeOfRes (c : CondCall) (ro : Option CondRes) : Nat :=
  match ro with
  | none => 0
  | some r =>
      match c, r with
      | CondCall.cnotifyOne, _ => 1
      | CondCall.cnotifyAll, CondRes.rall k => k
      | CondCall.ccancel _, CondRes.rout true => 1
      | _, _ => 0

/-- Completed `wait` outcomes contributed by one observation. -/
def condObsComps (o : Obs CondSig) : Nat :=
  match o.result with
  | none => 0
  | some _ =>
      match o.call with
      | CondCall.cwait => 1
      | _ => 0

/-- Resolver credit contributed by one observation. -/
def condObsWakes (o : Obs CondSig) : Nat := wakeOfRes o.call o.result

/-- Completed `wait` outcomes in a trace. -/
def condComps : Trace CondSig → Nat
  | [] => 0
  | o :: t => condObsComps o + condComps t

/-- Completed resolver credit in a trace. -/
def condWakeCredit : Trace CondSig → Nat
  | [] => 0
  | o :: t => condObsWakes o + condWakeCredit t

/-- Wake credit held by the returning fiber slot (0 while `running`). -/
def retWakeCredit : Option (FSlot CondSig) → Nat
  | some (FSlot.returning d r) => wakeOfRes d.call (some r)
  | _ => 0

/-- Wake credit held by an external in-flight record (0 before its
effect). -/
def extWakeCredit (e : ExtPend CondSig) : Nat := wakeOfRes e.call e.result

def extsWake : List (ExtPend CondSig) → Nat
  | [] => 0
  | e :: l => extWakeCredit e + extsWake l

theorem extsWake_append (l1 l2 : List (ExtPend CondSig)) :
    extsWake (l1 ++ l2) = extsWake l1 + extsWake l2 := by
  induction l1 with
  | nil => simp [extsWake]
  | cons e t ih => simp only [List.cons_append, extsWake, ih]; omega

theorem condComps_append (t1 t2 : Trace CondSig) :
    condComps (t1 ++ t2) = condComps t1 + condComps t2 := by
  induction t1 with
  | nil => simp [condComps]
  | cons o t ih => simp only [List.cons_append, condComps, ih]; omega

theorem condWakeCredit_append (t1 t2 : Trace CondSig) :
    condWakeCredit (t1 ++ t2) = condWakeCredit t1 + condWakeCredit t2 := by
  induction t1 with
  | nil => simp [condWakeCredit]
  | cons o t ih => simp only [List.cons_append, condWakeCredit, ih]; omega

theorem condAdmit_resolved (s : CondState) (f : FiberId) (c : CondCall) (s' : CondState)
    (h : condAdmit s f c = some s') : s'.resolved = s.resolved := by
  cases c with
  | cwait =>
      simp only [condAdmit] at h
      by_cases hown : s.owner = some f
      · rw [if_pos hown] at h
        injection h with h'
        subst h'
        simp
      · rw [if_neg hown] at h
        simp at h
  | cnotifyOne => simp only [condAdmit] at h; injection h with h'; subst h'; rfl
  | cnotifyAll => simp only [condAdmit] at h; injection h with h'; subst h'; rfl
  | ccancel w => simp only [condAdmit] at h; injection h with h'; subst h'; rfl

theorem condRun_cwait_none (s : CondState) (t : Tick) (f : FiberId) :
    condRun s t f CondCall.cwait = none := rfl

/-- A fiber-origin resolver effect publishes at most as many outcomes as
its result observation credits. -/
theorem condRun_wake_len (s : CondState) (t : Tick) (f : FiberId) (c : CondCall)
    (r : CondRes) (s' : CondState) (wk : List FiberId)
    (h : condRun s t f c = some (r, s', wk)) :
    s'.resolved.length ≤ s.resolved.length + wakeOfRes c (some r) := by
  cases c with
  | cwait => rw [condRun_cwait_none] at h; simp at h
  | cnotifyOne =>
      simp only [condRun] at h
      cases hq : s.cwaitq with
      | nil =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          exact Nat.le_add_right _ _
      | cons g tl =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨hr, hst, _⟩ := h
          subst hr
          subst hst
          show (s.resolved ++ [(g, true)]).length ≤ s.resolved.length + 1
          simp only [List.length_append, List.length_singleton]
          omega
  | cnotifyAll =>
      simp only [condRun] at h
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨hr, hst, _⟩ := h
      subst hr
      subst hst
      show (s.resolved ++ s.cwaitq.map (fun g => (g, true))).length ≤
        s.resolved.length + s.cwaitq.length
      simp only [List.length_append, List.length_map]
      omega
  | ccancel w =>
      simp only [condRun] at h
      cases hc : condRemove s.cwaitq w with
      | none =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          exact Nat.le_add_right _ _
      | some tl =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨hr, hst, _⟩ := h
          subst hr
          subst hst
          show (s.resolved ++ [(w, false)]).length ≤ s.resolved.length + 1
          simp only [List.length_append, List.length_singleton]
          omega

/-- Same bound for the external critical section (identical wake bodies). -/
theorem condExtRun_wake_len (c : CondCall) (s : CondState) (t : Tick)
    (r : CondRes) (s' : CondState) (wk : List FiberId)
    (h : condExtRun c s t = some (r, s', wk)) :
    s'.resolved.length ≤ s.resolved.length + wakeOfRes c (some r) := by
  cases c with
  | cwait => simp only [condExtRun] at h; simp at h
  | cnotifyOne =>
      simp only [condExtRun] at h
      cases hq : s.cwaitq with
      | nil =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          exact Nat.le_add_right _ _
      | cons g tl =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨hr, hst, _⟩ := h
          subst hr
          subst hst
          show (s.resolved ++ [(g, true)]).length ≤ s.resolved.length + 1
          simp only [List.length_append, List.length_singleton]
          omega
  | cnotifyAll =>
      simp only [condExtRun] at h
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨hr, hst, _⟩ := h
      subst hr
      subst hst
      show (s.resolved ++ s.cwaitq.map (fun g => (g, true))).length ≤
        s.resolved.length + s.cwaitq.length
      simp only [List.length_append, List.length_map]
      omega
  | ccancel w =>
      simp only [condExtRun] at h
      cases hc : condRemove s.cwaitq w with
      | none =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          exact Nat.le_add_right _ _
      | some tl =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨hr, hst, _⟩ := h
          subst hr
          subst hst
          show (s.resolved ++ [(w, false)]).length ≤ s.resolved.length + 1
          simp only [List.length_append, List.length_singleton]
          omega

/-- Resolver effects only append to the resolved list. -/
theorem condRun_resolved_mono (s : CondState) (t : Tick) (f : FiberId) (c : CondCall)
    (r : CondRes) (s' : CondState) (wk : List FiberId)
    (h : condRun s t f c = some (r, s', wk)) :
    s.resolved.length ≤ s'.resolved.length := by
  cases c with
  | cwait => rw [condRun_cwait_none] at h; simp at h
  | cnotifyOne =>
      simp only [condRun] at h
      cases hq : s.cwaitq with
      | nil =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          omega
      | cons g tl =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          show s.resolved.length ≤ (s.resolved ++ [(g, true)]).length
          simp only [List.length_append, List.length_singleton]
          omega
  | cnotifyAll =>
      simp only [condRun] at h
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨_, hst, _⟩ := h
      subst hst
      show s.resolved.length ≤ (s.resolved ++ s.cwaitq.map (fun g => (g, true))).length
      simp only [List.length_append]
      omega
  | ccancel w =>
      simp only [condRun] at h
      cases hc : condRemove s.cwaitq w with
      | none =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          omega
      | some tl =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          show s.resolved.length ≤ (s.resolved ++ [(w, false)]).length
          simp only [List.length_append, List.length_singleton]
          omega

/-- Same monotonicity for the external critical section. -/
theorem condExtRun_resolved_mono (c : CondCall) (s : CondState) (t : Tick)
    (r : CondRes) (s' : CondState) (wk : List FiberId)
    (h : condExtRun c s t = some (r, s', wk)) :
    s.resolved.length ≤ s'.resolved.length := by
  cases c with
  | cwait => simp only [condExtRun] at h; simp at h
  | cnotifyOne =>
      simp only [condExtRun] at h
      cases hq : s.cwaitq with
      | nil =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          omega
      | cons g tl =>
          rw [hq] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          show s.resolved.length ≤ (s.resolved ++ [(g, true)]).length
          simp only [List.length_append, List.length_singleton]
          omega
  | cnotifyAll =>
      simp only [condExtRun] at h
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨_, hst, _⟩ := h
      subst hst
      show s.resolved.length ≤ (s.resolved ++ s.cwaitq.map (fun g => (g, true))).length
      simp only [List.length_append]
      omega
  | ccancel w =>
      simp only [condExtRun] at h
      cases hc : condRemove s.cwaitq w with
      | none =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          omega
      | some tl =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          show s.resolved.length ≤ (s.resolved ++ [(w, false)]).length
          simp only [List.length_append, List.length_singleton]
          omega

theorem condConsume_len : ∀ (l : List (FiberId × Bool)) (f : FiberId) (b : Bool)
    (rest : List (FiberId × Bool)), condConsume l f = some (b, rest) →
    l.length = rest.length + 1 := by
  intro l
  induction l with
  | nil => intro f b rest h; simp [condConsume] at h
  | cons hd t ih =>
      obtain ⟨g, gb⟩ := hd
      intro f b rest h
      simp only [condConsume] at h
      by_cases hgf : g = f
      · rw [if_pos hgf] at h
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨_, h2⟩ := h
        subst h2
        rfl
      · rw [if_neg hgf] at h
        cases hc : condConsume t f with
        | none => rw [hc] at h; simp at h
        | some p =>
            rw [hc] at h
            simp only [Option.map_some] at h
            have h3 : (p.1, (g, gb) :: p.2) = (b, rest) := Option.some.inj h
            rw [Prod.mk.injEq] at h3
            have hil := ih f p.1 p.2 hc
            have h2 : (g, gb) :: p.2 = rest := h3.2
            subst h2
            simp [hil]

theorem condPark_resolved (s : CondState) (f : FiberId) (c : CondCall) (s' : CondState)
    (wk : List FiberId) (h : condPark s f c = some (s', wk)) : s'.resolved = s.resolved := by
  cases c with
  | cwait =>
      simp only [condPark] at h
      cases hph : s.phase f with
      | cwFresh =>
          rw [hph] at h
          cases hm : s.mwaitq with
          | nil =>
              rw [hm] at h
              simp only [Option.some.injEq, Prod.mk.injEq] at h
              obtain ⟨hst, _⟩ := h
              subst hst
              simp
          | cons g t =>
              rw [hm] at h
              simp only [Option.some.injEq, Prod.mk.injEq] at h
              obtain ⟨hst, _⟩ := h
              subst hst
              simp
      | cwWaiting =>
          rw [hph] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨hst, _⟩ := h
          subst hst
          simp
      | cwReacq =>
          rw [hph] at h
          simp at h
  | _ => simp [condPark] at h

/-- A completing `wait` consumes exactly one resolved outcome. -/
theorem condFinish_cwait_len (s : CondState) (f : FiberId) (r : CondRes)
    (s' : CondState) (wk : List FiberId)
    (h : condFinish s f CondCall.cwait = some (r, s', wk)) :
    s.resolved.length = s'.resolved.length + 1 := by
  simp only [condFinish] at h
  cases hph : s.phase f with
  | cwFresh => rw [hph] at h; simp at h
  | cwWaiting =>
      rw [hph] at h
      cases hc : condConsume s.resolved f with
      | none => rw [hc] at h; simp at h
      | some p =>
          rw [hc] at h
          cases hown : s.owner with
          | none =>
              rw [hown] at h
              simp only [Option.some.injEq, Prod.mk.injEq] at h
              obtain ⟨_, hst, _⟩ := h
              subst hst
              have hcon := condConsume_len s.resolved f p.1 p.2 hc
              simp [hcon]
          | some o' =>
              rw [hown] at h
              simp at h
  | cwReacq =>
      rw [hph] at h
      cases hc : condConsume s.resolved f with
      | none => rw [hc] at h; simp at h
      | some p =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          have hcon := condConsume_len s.resolved f p.1 p.2 hc
          simp [hcon]

/-- **Wake-backedness conservation** — along any run, the consumed-plus-
unconsumed `wait` outcomes stay covered by the completed-plus-in-window
resolver credit.  `acc` counts the run's completed `wait` outcomes so far
and `acr` its completed resolver credit; the premise is the invariant at
the run's start configuration (at `primInit`: `0 + 0 ≤ 0 + 0 + 0`), the
conclusion the invariant at its end.  A resolver's effect grows the window
credit exactly as it grows the resolved list, its completion moves that
credit from the window into `acr`, and a completing `wait` moves one unit
from `cfg.prim.resolved` into `acc`.  `hret` says a returning slot never
carries a `wait` (only `run` builds one, and `wait` has no run path);
`hext` says no external record carries a `wait` (`condExtCap` refuses it). -/
theorem cond_mirror {cfg0 fin : PrimCfg CondSig condPrim} {t : Trace CondSig}
    (hrun : PrimRuns2 CondSig condPrim cfg0 t fin)
    (hret : ∀ d r, cfg0.cur = some (FSlot.returning d r) → d.call ≠ CondCall.cwait)
    (hext : ∀ e ∈ cfg0.exts, e.call ≠ CondCall.cwait) :
    ∀ (acc acr : Nat),
      acc + cfg0.prim.resolved.length ≤ acr + retWakeCredit cfg0.cur + extsWake cfg0.exts →
      acc + condComps t + fin.prim.resolved.length ≤
        acr + condWakeCredit t + retWakeCredit fin.cur + extsWake fin.exts := by
  induction hrun with
  | stop cfg =>
      intro acc acr h0
      simpa [condComps, condWakeCredit] using h0
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro acc acr h0
      cases hstep with
      | submit f c hsub =>
          show acc + condComps t2 + fin.prim.resolved.length ≤
            acr + condWakeCredit t2 + retWakeCredit fin.cur + extsWake fin.exts
          exact ih hret hext acc acr h0
      | dispatchFresh r rest s' hcurE hrunqE hfreshE hadmit =>
          have hs : s'.resolved = cfg.prim.resolved :=
            condAdmit_resolved cfg.prim r.fiber r.call s' hadmit
          have hsl : s'.resolved.length = cfg.prim.resolved.length := by rw [hs]
          have hcur0 : retWakeCredit cfg.cur = 0 := by rw [hcurE]; rfl
          have h0' : acc + s'.resolved.length ≤
              acr + retWakeCredit (some (FSlot.running { fiber := r.fiber, call := r.call } false)) +
                extsWake cfg.exts := by
            simp only [retWakeCredit, hsl]
            omega
          have hih := ih (by intro d r0 heq; cases Option.some.inj heq) hext acc acr h0'
          simp only [Option.toList, List.cons_append, List.nil_append, issueObs, condComps,
            condWakeCredit, condObsComps, condObsWakes, wakeOfRes] at hih ⊢
          omega
      | dispatchResumed r rest hcurE hrunqE hfreshE =>
          have hcur0 : retWakeCredit cfg.cur = 0 := by rw [hcurE]; rfl
          have h0' : acc + cfg.prim.resolved.length ≤
              acr + retWakeCredit (some (FSlot.running { fiber := r.fiber, call := r.call } true)) +
                extsWake cfg.exts := by
            simp only [retWakeCredit]
            omega
          show acc + condComps t2 + fin.prim.resolved.length ≤
            acr + condWakeCredit t2 + retWakeCredit fin.cur + extsWake fin.exts
          exact ih (by intro d r0 heq; cases Option.some.inj heq) hext acc acr h0'
      | fiberEffect d b ps rest r0 s' wk hcurE hb hrunE hmap hwake =>
          have hcw : d.call ≠ CondCall.cwait := by
            cases hcall : d.call with
            | cwait =>
                rw [hcall] at hrunE
                have hw : condRun cfg.prim cfg.now d.fiber CondCall.cwait =
                    some (r0, s', wk) := hrunE
                rw [condRun_cwait_none] at hw
                simp at hw
            | cnotifyOne => simp
            | cnotifyAll => simp
            | ccancel w => simp
          have hrunE' : condRun cfg.prim cfg.now d.fiber d.call = some (r0, s', wk) := hrunE
          have hstep := condRun_wake_len cfg.prim cfg.now d.fiber d.call r0 s' wk hrunE'
          have hcur0 : retWakeCredit cfg.cur = 0 := by rw [hcurE]; rfl
          have h0' : acc + s'.resolved.length ≤
              acr + wakeOfRes d.call (some r0) + extsWake cfg.exts := by
            omega
          have hih := ih
            (by
                intro d' r' heq
                have h1 : FSlot.returning d r0 = FSlot.returning d' r' := Option.some.inj heq
                injection h1 with h2 _
                subst h2
                exact hcw) hext acc acr h0'
          show acc + condComps t2 + fin.prim.resolved.length ≤
            acr + condWakeCredit t2 + retWakeCredit fin.cur + extsWake fin.exts
          exact hih
      | fiberDone d r0 hcurE =>
          have hcw := hret d r0 hcurE
          have hobs0 : condObsComps
              { caller := Caller.fiber d.fiber, call := d.call, result := some r0 } = 0 := by
            cases hcall : d.call with
            | cwait => rw [hcall] at hcw; exact absurd rfl hcw
            | _ => rfl
          have hcurW : retWakeCredit cfg.cur = wakeOfRes d.call (some r0) := by rw [hcurE]; rfl
          have h0' : acc + cfg.prim.resolved.length ≤
              acr + wakeOfRes d.call (some r0) +
                retWakeCredit (none : Option (FSlot CondSig)) + extsWake cfg.exts := by
            simp only [retWakeCredit]
            omega
          have hih := ih (by intro d' r' heq; simp at heq) hext acc
            (acr + condObsWakes (compObs CondSig (Caller.fiber d.fiber) d.call r0)) h0'
          simp only [Option.toList, List.cons_append, List.nil_append, compObs, condComps,
            condWakeCredit, condObsWakes] at hih ⊢
          rw [hobs0]
          omega
      | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
          have hparkE' : condPark cfg.prim d.fiber d.call = some (s', wk) := hparkE
          have hs := condPark_resolved cfg.prim d.fiber d.call s' wk hparkE'
          have hsl : s'.resolved.length = cfg.prim.resolved.length := by rw [hs]
          have hcur0 : retWakeCredit cfg.cur = 0 := by rw [hcurE]; rfl
          have h0' : acc + s'.resolved.length ≤
              acr + retWakeCredit (none : Option (FSlot CondSig)) + extsWake cfg.exts := by
            simp only [retWakeCredit, hsl]
            omega
          have hih := ih (by intro d' r' heq; simp at heq) hext acc acr h0'
          show acc + condComps t2 + fin.prim.resolved.length ≤
            acr + condWakeCredit t2 + retWakeCredit fin.cur + extsWake fin.exts
          exact hih
      | finishDone d b ps rest r0 s' wk hcurE hb hfinE hmap hwake =>
          have hfinE' : condFinish cfg.prim d.fiber d.call = some (r0, s', wk) := hfinE
          have hcur0 : retWakeCredit cfg.cur = 0 := by rw [hcurE]; rfl
          cases hcall : d.call with
          | cwait =>
              rw [hcall] at hfinE'
              have hcon := condFinish_cwait_len cfg.prim d.fiber r0 s' wk hfinE'
              have h0' : acc + 1 + s'.resolved.length ≤
                  acr + retWakeCredit (none : Option (FSlot CondSig)) + extsWake cfg.exts := by
                simp only [retWakeCredit]
                omega
              have hih := ih (by intro d' r' heq; simp at heq) hext (acc + 1) acr h0'
              simp only [Option.toList, List.cons_append, List.nil_append, compObs, condComps,
                condWakeCredit, condObsComps, condObsWakes, wakeOfRes] at hih ⊢
              omega
          | cnotifyOne => rw [hcall] at hfinE'; simp [condFinish] at hfinE'
          | cnotifyAll => rw [hcall] at hfinE'; simp [condFinish] at hfinE'
          | ccancel w => rw [hcall] at hfinE'; simp [condFinish] at hfinE'
      | extApply x c preE postE hcap hnovel =>
          have hcw : c ≠ CondCall.cwait := by
            cases hcall : c with
            | cwait => rw [hcall] at hcap; simp [condExtCap] at hcap
            | _ => simp
          have h0' : acc + cfg.prim.resolved.length ≤
              acr + retWakeCredit cfg.cur +
                extsWake (cfg.exts ++ [{ x := x, call := c, result := (none : Option CondRes) }]) := by
            rw [extsWake_append]
            simp only [extsWake, extWakeCredit, wakeOfRes]
            omega
          have hih := ih hret
            (by
                intro e' he'
                rcases List.mem_append.mp he' with hm | hm
                · exact hext e' hm
                · rw [List.mem_singleton] at hm
                  subst hm
                  exact hcw) acc acr h0'
          simp only [Option.toList, List.cons_append, List.nil_append, issueObs, condComps,
            condWakeCredit, condObsComps, condObsWakes, wakeOfRes] at hih ⊢
          omega
      | extEffect preE postE e ps rest r0 s' wk hextsE hresE hrunE hmap hwake =>
          have hein : e ∈ cfg.exts := by
            rw [hextsE]
            exact List.mem_append.mpr (Or.inr List.mem_cons_self)
          have hcw := hext e hein
          have hrunE' : condExtRun e.call cfg.prim cfg.now = some (r0, s', wk) := hrunE
          have hstep := condExtRun_wake_len e.call cfg.prim cfg.now r0 s' wk hrunE'
          have hsplit : extsWake cfg.exts = extsWake preE + extsWake postE := by
            rw [hextsE, extsWake_append]
            simp only [extsWake, extWakeCredit, hresE, wakeOfRes]
            omega
          have h0' : acc + s'.resolved.length ≤
              acr + retWakeCredit cfg.cur +
                extsWake (preE ++ { e with result := some r0 } :: postE) := by
            rw [extsWake_append]
            simp only [extsWake, extWakeCredit]
            omega
          have hih := ih hret
            (by
                intro e' he'
                rcases List.mem_append.mp he' with hm | hm
                · exact hext e' (by rw [hextsE]; exact List.mem_append.mpr (Or.inl hm))
                · rw [List.mem_cons] at hm
                  rcases hm with rfl | hm
                  · exact hcw
                  · exact hext e' (by
                      rw [hextsE]
                      exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ hm)))) acc acr h0'
          show acc + condComps t2 + fin.prim.resolved.length ≤
            acr + condWakeCredit t2 + retWakeCredit fin.cur + extsWake fin.exts
          exact hih
      | extDone preE postE e r0 hextsE hresE =>
          have hein : e ∈ cfg.exts := by
            rw [hextsE]
            exact List.mem_append.mpr (Or.inr List.mem_cons_self)
          have hcw := hext e hein
          have hobs0 : condObsComps
              { caller := Caller.ext e.x, call := e.call, result := some r0 } = 0 := by
            cases hcall : e.call with
            | cwait => rw [hcall] at hcw; exact absurd rfl hcw
            | _ => rfl
          have hsplit : extsWake cfg.exts =
              extsWake (preE ++ postE) + wakeOfRes e.call (some r0) := by
            rw [hextsE, extsWake_append, extsWake_append]
            simp only [extsWake, extWakeCredit, hresE, wakeOfRes]
            omega
          have h0' : acc + cfg.prim.resolved.length ≤
              (acr + condObsWakes (compObs CondSig (Caller.ext e.x) e.call r0)) +
                retWakeCredit cfg.cur + extsWake (preE ++ postE) := by
            simp only [condObsWakes, compObs]
            omega
          have hih := ih hret
            (by
                intro e' he'
                rcases List.mem_append.mp he' with hm | hm
                · exact hext e' (by rw [hextsE]; exact List.mem_append.mpr (Or.inl hm))
                · exact hext e' (by
                    rw [hextsE]
                    exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ hm)))) acc
            (acr + condObsWakes (compObs CondSig (Caller.ext e.x) e.call r0)) h0'
          simp only [Option.toList, List.cons_append, List.nil_append, compObs, condComps,
            condWakeCredit, condObsWakes] at hih ⊢
          rw [hobs0]
          omega
      | envTime t0 hcurE hle =>
          show acc + condComps t2 + fin.prim.resolved.length ≤
            acr + condWakeCredit t2 + retWakeCredit fin.cur + extsWake fin.exts
          exact ih hret hext acc acr h0
      | envExpire f0 s' preP postP p hcurE hexp hparE hf =>
          exact absurd hexp (by simp [condPrim, condExpire])

/-- The no-spurious-wake certificate: from the primitive's initial
configuration, completed `wait` outcomes plus unresolved wake records are
covered by resolver credit — completed, returning-slot, or
external-in-window. -/
theorem condWakeBacked (t : Trace CondSig) (fin : PrimCfg CondSig condPrim)
    (hrun : PrimRuns2 CondSig condPrim (primInit CondSig condPrim) t fin) :
    condComps t + fin.prim.resolved.length ≤
      condWakeCredit t + retWakeCredit fin.cur + extsWake fin.exts := by
  have hcur : (primInit CondSig condPrim).cur = none := rfl
  have hne : (primInit CondSig condPrim).exts = ([] : List (ExtPend CondSig)) := rfl
  have h0 : 0 + (primInit CondSig condPrim).prim.resolved.length ≤
      0 + retWakeCredit (none : Option (FSlot CondSig)) + extsWake [] := by
    show (0 : Nat) ≤ 0 + 0 + 0
    omega
  have h := cond_mirror hrun (by
      intro d r heq
      rw [hcur] at heq
      simp at heq) (by
      intro e he
      rw [hne] at he
      cases he) 0 0 h0
  simpa using h

/-! ## Possession batteries

The batteries are reachability witnesses for the model's own transition
relation.  A `wait` is admitted only from the embedded slot's holder (the
caller precondition), and acquiring that slot is the caller side's mutex
traffic (Stage-3 surface), so the multi-waiter batteries start from
instrumented configurations rather than `primInit`. -/

/-- Battery A: the fast-path `wait` — a slot-holder parks (the release
frees the slot), a `notify_one` resolves its node, and the resume takes
the free slot and completes `rout true`. -/
theorem cond_battery_fast_path :
    ∃ fin : PrimCfg CondSig condPrim,
      PrimRuns2 CondSig condPrim
        { prim := { condInit with owner := some 0 },
          now := 0
          cur := none
          parked := ([] : List (Pnd CondSig))
          runq := [{ fiber := 0, call := CondCall.cwait, fresh := true }]
          retired := ([] : List FiberId)
          nextFiber := 1
          exts := ([] : List (ExtPend CondSig)) }
        [issueObs CondSig (Caller.fiber 0) CondCall.cwait,
          issueObs CondSig (Caller.ext 0) CondCall.cnotifyOne,
          compObs CondSig (Caller.fiber 0) CondCall.cwait (CondRes.rout true),
          compObs CondSig (Caller.ext 0) CondCall.cnotifyOne CondRes.runit]
        fin ∧
      fin.prim.owner = some 0 ∧ fin.prim.resolved = [] ∧ fin.prim.cwaitq = [] := by
  refine ⟨{ prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
            now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
            exts := [] },
    ?_, rfl, rfl, rfl⟩
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun _ => CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 0, call := CondCall.cwait, fresh := true }],
      retired := [], nextFiber := 1, exts := [] }
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } false),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.dispatchFresh (A := CondSig) (P := condPrim)
      { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun _ => CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [],
        runq := [{ fiber := 0, call := CondCall.cwait, fresh := true }],
        retired := [], nextFiber := 1, exts := [] }
      { fiber := 0, call := CondCall.cwait, fresh := true } []
      { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] }
      rfl rfl rfl (by rfl)) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } false),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.runPark (A := CondSig) (P := condPrim)
      { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } false),
        parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
      { fiber := 0, call := CondCall.cwait } false [] []
      { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] }
      [] rfl (by rfl) (by rfl) rfl Wakes.nil) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1, exts := [] }
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := none }] }
    _ _ _ (PrimStep2.extApply (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
        runq := [], retired := [], nextFiber := 1, exts := [] }
      0 CondCall.cnotifyOne [] [] (by rfl) (by simp)) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := none }] }
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, true)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 0, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
    _ _ _ (PrimStep2.extEffect (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
        runq := [], retired := [], nextFiber := 1,
        exts := [{ x := 0, call := CondCall.cnotifyOne, result := none }] }
      [] [] { x := 0, call := CondCall.cnotifyOne, result := none }
      [{ fiber := 0, call := CondCall.cwait }] []
      CondRes.runit
      { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, true)] }
      [0] rfl rfl (by rfl) rfl
      (Wakes.drop (A := CondSig) { fiber := 0, call := CondCall.cwait } Wakes.nil)) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, true)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 0, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, true)] },
      now := 0,
      cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } true),
      parked := [], runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
    _ _ _ (PrimStep2.dispatchResumed (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, true)] },
        now := 0, cur := none, parked := [],
        runq := [{ fiber := 0, call := CondCall.cwait, fresh := false }],
        retired := [], nextFiber := 1,
        exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
      { fiber := 0, call := CondCall.cwait, fresh := false } [] rfl rfl rfl) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, true)] },
      now := 0,
      cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } true),
      parked := [], runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
    _ _ _ (PrimStep2.finishDone (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, true)] },
        now := 0,
        cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } true),
        parked := [], runq := [], retired := [], nextFiber := 1,
        exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
      { fiber := 0, call := CondCall.cwait } true [] []
      (CondRes.rout true)
      { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] }
      [] rfl rfl (by rfl) rfl Wakes.nil) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.extDone (A := CondSig) (P := condPrim)
      { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
        exts := [{ x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }] }
      [] [] { x := 0, call := CondCall.cnotifyOne, result := some CondRes.runit }
      CondRes.runit rfl rfl)
    (PrimRuns2.stop (A := CondSig) (P := condPrim) _)

/-- Battery C: `cancel` — a slot-holder parks, an external `cancel`
publishes `cancelled` into the queued node, and the resume completes
`rout false` with the slot retaken. -/
theorem cond_battery_cancel :
    ∃ fin : PrimCfg CondSig condPrim,
      PrimRuns2 CondSig condPrim
        { prim := { condInit with owner := some 0 },
          now := 0
          cur := none
          parked := ([] : List (Pnd CondSig))
          runq := [{ fiber := 0, call := CondCall.cwait, fresh := true }]
          retired := ([] : List FiberId)
          nextFiber := 1
          exts := ([] : List (ExtPend CondSig)) }
        [issueObs CondSig (Caller.fiber 0) CondCall.cwait,
          issueObs CondSig (Caller.ext 0) (CondCall.ccancel 0),
          compObs CondSig (Caller.fiber 0) CondCall.cwait (CondRes.rout false),
          compObs CondSig (Caller.ext 0) (CondCall.ccancel 0) (CondRes.rout true)]
        fin ∧
      fin.prim.owner = some 0 ∧ fin.prim.resolved = [] ∧ fin.prim.cwaitq = [] := by
  refine ⟨{ prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
            now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
            exts := [] },
    ?_, rfl, rfl, rfl⟩
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun _ => CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 0, call := CondCall.cwait, fresh := true }],
      retired := [], nextFiber := 1, exts := [] }
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } false),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.dispatchFresh (A := CondSig) (P := condPrim)
      { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun _ => CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [],
        runq := [{ fiber := 0, call := CondCall.cwait, fresh := true }],
        retired := [], nextFiber := 1, exts := [] }
      { fiber := 0, call := CondCall.cwait, fresh := true } []
      { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] }
      rfl rfl rfl (by rfl)) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } false),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.runPark (A := CondSig) (P := condPrim)
      { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } false),
        parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
      { fiber := 0, call := CondCall.cwait } false [] []
      { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] }
      [] rfl (by rfl) (by rfl) rfl Wakes.nil) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1, exts := [] }
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.ccancel 0, result := none }] }
    _ _ _ (PrimStep2.extApply (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
        runq := [], retired := [], nextFiber := 1, exts := [] }
      0 (CondCall.ccancel 0) [] [] (by rfl) (by simp)) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.ccancel 0, result := none }] }
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, false)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 0, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 1,
      exts := [{ x := 0, call := CondCall.ccancel 0, result := some (CondRes.rout true) }] }
    _ _ _ (PrimStep2.extEffect (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [0], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [{ fiber := 0, call := CondCall.cwait }],
        runq := [], retired := [], nextFiber := 1,
        exts := [{ x := 0, call := CondCall.ccancel 0, result := none }] }
      [] [] { x := 0, call := CondCall.ccancel 0, result := none }
      [{ fiber := 0, call := CondCall.cwait }] []
      (CondRes.rout true)
      { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, false)] }
      [0] rfl rfl (by rfl) rfl
      (Wakes.drop (A := CondSig) { fiber := 0, call := CondCall.cwait } Wakes.nil)) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, false)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 0, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 1,
      exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, false)] },
      now := 0,
      cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } true),
      parked := [], runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
    _ _ _ (PrimStep2.dispatchResumed (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, false)] },
        now := 0, cur := none, parked := [],
        runq := [{ fiber := 0, call := CondCall.cwait, fresh := false }],
        retired := [], nextFiber := 1,
        exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
      { fiber := 0, call := CondCall.cwait, fresh := false } [] rfl rfl rfl) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, false)] },
      now := 0,
      cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } true),
      parked := [], runq := [], retired := [], nextFiber := 1,
      exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
      exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
    _ _ _ (PrimStep2.finishDone (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [(0, false)] },
        now := 0,
        cur := some (FSlot.running { fiber := 0, call := CondCall.cwait } true),
        parked := [], runq := [], retired := [], nextFiber := 1,
        exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
      { fiber := 0, call := CondCall.cwait } true [] []
      (CondRes.rout false)
      { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] }
      [] rfl rfl (by rfl) rfl Wakes.nil) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
      exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
    { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
      now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.extDone (A := CondSig) (P := condPrim)
      { prim := { owner := some 0, mwaitq := [], cwaitq := [], phase := (fun g => if g = 0 then CPhase.cwFresh else if g = 0 then CPhase.cwWaiting else if g = 0 then CPhase.cwFresh else CPhase.cwFresh), resolved := [] },
        now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
        exts := [{ x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }] }
      [] [] { x := 0, call := (CondCall.ccancel 0), result := some (CondRes.rout true) }
      (CondRes.rout true) rfl rfl)
    (PrimRuns2.stop (A := CondSig) (P := condPrim) _)

/-- Battery B: a three-waiter `notify_all` — the drain readies every
waiter (all three surface in the run queue), the result observation
carries the drained count, and the first resume takes the slot and
completes `rout true` while the other two sit in the Mesa window (still
runnable, their resolutions unconsumed).  Started from an instrumented
configuration: three waiters parked past their release sections. -/
theorem cond_battery_broadcast :
    ∃ fin : PrimCfg CondSig condPrim,
      PrimRuns2 CondSig condPrim
        { prim := { owner := none, mwaitq := [], cwaitq := [1, 2, 3],
                    phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                    resolved := [] },
          now := 0
          cur := none
          parked := [{ fiber := 1, call := CondCall.cwait },
                     { fiber := 2, call := CondCall.cwait },
                     { fiber := 3, call := CondCall.cwait }]
          runq := ([] : List (PReady CondSig))
          retired := ([] : List FiberId)
          nextFiber := 4
          exts := ([] : List (ExtPend CondSig)) }
        [issueObs CondSig (Caller.ext 0) CondCall.cnotifyAll,
          compObs CondSig (Caller.ext 0) CondCall.cnotifyAll (CondRes.rall 3),
          compObs CondSig (Caller.fiber 1) CondCall.cwait (CondRes.rout true)]
        fin ∧
      fin.prim.cwaitq = [] ∧ fin.prim.owner = some 1 ∧
      fin.runq.length = 2 ∧ fin.prim.resolved.length = 2 := by
  refine ⟨{ prim := { owner := some 1, mwaitq := [], cwaitq := [],
                      phase := (fun g => if g = 1 then CPhase.cwFresh else if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                      resolved := [(2, true), (3, true)] },
            now := 0, cur := none, parked := [],
            runq := [{ fiber := 2, call := CondCall.cwait, fresh := false },
                    { fiber := 3, call := CondCall.cwait, fresh := false }],
            retired := [1], nextFiber := 4, exts := [] },
    ?_, rfl, rfl, rfl, rfl⟩
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [1, 2, 3],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [] },
      now := 0, cur := none,
      parked := [{ fiber := 1, call := CondCall.cwait },
                 { fiber := 2, call := CondCall.cwait },
                 { fiber := 3, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 4, exts := [] }
    { prim := { owner := none, mwaitq := [], cwaitq := [1, 2, 3],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [] },
      now := 0, cur := none,
      parked := [{ fiber := 1, call := CondCall.cwait },
                 { fiber := 2, call := CondCall.cwait },
                 { fiber := 3, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 4,
      exts := [{ x := 0, call := CondCall.cnotifyAll, result := none }] }
    _ _ _ (PrimStep2.extApply (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [1, 2, 3],
                  phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                  resolved := [] },
        now := 0, cur := none,
        parked := [{ fiber := 1, call := CondCall.cwait },
                   { fiber := 2, call := CondCall.cwait },
                   { fiber := 3, call := CondCall.cwait }],
        runq := [], retired := [], nextFiber := 4, exts := [] }
      0 CondCall.cnotifyAll [] [] (by rfl) (by simp)) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [1, 2, 3],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [] },
      now := 0, cur := none,
      parked := [{ fiber := 1, call := CondCall.cwait },
                 { fiber := 2, call := CondCall.cwait },
                 { fiber := 3, call := CondCall.cwait }],
      runq := [], retired := [], nextFiber := 4,
      exts := [{ x := 0, call := CondCall.cnotifyAll, result := none }] }
    { prim := { owner := none, mwaitq := [], cwaitq := [],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [(1, true), (2, true), (3, true)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := CondCall.cwait, fresh := false },
               { fiber := 2, call := CondCall.cwait, fresh := false },
               { fiber := 3, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 4,
      exts := [{ x := 0, call := CondCall.cnotifyAll, result := some (CondRes.rall 3) }] }
    _ _ _ (PrimStep2.extEffect (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [1, 2, 3],
                  phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                  resolved := [] },
        now := 0, cur := none,
        parked := [{ fiber := 1, call := CondCall.cwait },
                   { fiber := 2, call := CondCall.cwait },
                   { fiber := 3, call := CondCall.cwait }],
        runq := [], retired := [], nextFiber := 4,
        exts := [{ x := 0, call := CondCall.cnotifyAll, result := none }] }
      [] [] { x := 0, call := CondCall.cnotifyAll, result := none }
      [{ fiber := 1, call := CondCall.cwait },
       { fiber := 2, call := CondCall.cwait },
       { fiber := 3, call := CondCall.cwait }] []
      (CondRes.rall 3)
      { owner := none, mwaitq := [], cwaitq := [],
        phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
        resolved := [(1, true), (2, true), (3, true)] }
      [1, 2, 3] rfl rfl (by rfl) rfl
      (Wakes.drop (A := CondSig) { fiber := 1, call := CondCall.cwait }
        (Wakes.drop (A := CondSig) { fiber := 2, call := CondCall.cwait }
            (Wakes.drop (A := CondSig) { fiber := 3, call := CondCall.cwait } Wakes.nil)))) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [(1, true), (2, true), (3, true)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := CondCall.cwait, fresh := false },
               { fiber := 2, call := CondCall.cwait, fresh := false },
               { fiber := 3, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 4,
      exts := [{ x := 0, call := CondCall.cnotifyAll, result := some (CondRes.rall 3) }] }
    { prim := { owner := none, mwaitq := [], cwaitq := [],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [(1, true), (2, true), (3, true)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := CondCall.cwait, fresh := false },
               { fiber := 2, call := CondCall.cwait, fresh := false },
               { fiber := 3, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 4, exts := [] }
    _ _ _ (PrimStep2.extDone (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [],
                  phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                  resolved := [(1, true), (2, true), (3, true)] },
        now := 0, cur := none, parked := [],
        runq := [{ fiber := 1, call := CondCall.cwait, fresh := false },
                 { fiber := 2, call := CondCall.cwait, fresh := false },
                 { fiber := 3, call := CondCall.cwait, fresh := false }],
        retired := [], nextFiber := 4,
        exts := [{ x := 0, call := CondCall.cnotifyAll, result := some (CondRes.rall 3) }] }
      [] [] { x := 0, call := CondCall.cnotifyAll, result := some (CondRes.rall 3) }
      (CondRes.rall 3) rfl rfl) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [(1, true), (2, true), (3, true)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := CondCall.cwait, fresh := false },
               { fiber := 2, call := CondCall.cwait, fresh := false },
               { fiber := 3, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 4, exts := [] }
    { prim := { owner := none, mwaitq := [], cwaitq := [],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [(1, true), (2, true), (3, true)] },
      now := 0,
      cur := some (FSlot.running { fiber := 1, call := CondCall.cwait } true),
      parked := [],
      runq := [{ fiber := 2, call := CondCall.cwait, fresh := false },
               { fiber := 3, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 4, exts := [] }
    _ _ _ (PrimStep2.dispatchResumed (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [],
                  phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                  resolved := [(1, true), (2, true), (3, true)] },
        now := 0, cur := none, parked := [],
        runq := [{ fiber := 1, call := CondCall.cwait, fresh := false },
                 { fiber := 2, call := CondCall.cwait, fresh := false },
                 { fiber := 3, call := CondCall.cwait, fresh := false }],
        retired := [], nextFiber := 4, exts := [] }
      { fiber := 1, call := CondCall.cwait, fresh := false }
      [{ fiber := 2, call := CondCall.cwait, fresh := false },
       { fiber := 3, call := CondCall.cwait, fresh := false }] rfl rfl rfl) ?_
  refine PrimRuns2.step (A := CondSig) (P := condPrim)
    { prim := { owner := none, mwaitq := [], cwaitq := [],
                phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [(1, true), (2, true), (3, true)] },
      now := 0,
      cur := some (FSlot.running { fiber := 1, call := CondCall.cwait } true),
      parked := [],
      runq := [{ fiber := 2, call := CondCall.cwait, fresh := false },
               { fiber := 3, call := CondCall.cwait, fresh := false }],
      retired := [], nextFiber := 4, exts := [] }
    { prim := { owner := some 1, mwaitq := [], cwaitq := [],
                phase := (fun g => if g = 1 then CPhase.cwFresh else if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                resolved := [(2, true), (3, true)] },
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 2, call := CondCall.cwait, fresh := false },
               { fiber := 3, call := CondCall.cwait, fresh := false }],
      retired := [1], nextFiber := 4, exts := [] }
    _ _ _ (PrimStep2.finishDone (A := CondSig) (P := condPrim)
      { prim := { owner := none, mwaitq := [], cwaitq := [],
                  phase := (fun g => if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
                  resolved := [(1, true), (2, true), (3, true)] },
        now := 0,
        cur := some (FSlot.running { fiber := 1, call := CondCall.cwait } true),
        parked := [],
        runq := [{ fiber := 2, call := CondCall.cwait, fresh := false },
                 { fiber := 3, call := CondCall.cwait, fresh := false }],
        retired := [], nextFiber := 4, exts := [] }
      { fiber := 1, call := CondCall.cwait } true [] []
      (CondRes.rout true)
      { owner := some 1, mwaitq := [], cwaitq := [],
        phase := (fun g => if g = 1 then CPhase.cwFresh else if g = 1 then CPhase.cwWaiting else if g = 2 then CPhase.cwWaiting else if g = 3 then CPhase.cwWaiting else CPhase.cwFresh),
        resolved := [(2, true), (3, true)] }
      [] rfl rfl (by rfl) rfl Wakes.nil)
    (PrimRuns2.stop (A := CondSig) (P := condPrim) _)

/-! ## notify_all completeness (evidence direction B)

`condWakeBacked` rules out spurious completions; it does not rule out a
mutant that *under*-wakes (a `notify_all` waking only the head still
never completes an uncovered waiter).  The under-wake seam is closed at
the facet level by two defequations — the drain's wake list *is* the
whole queue and the queue is left empty — which the battery-B run then
shows carries through to actual completions.  A mutant `notify_all`
failing either equation is killed outright; a mutant keeping the facet
but readying fewer waiters is killed by battery B's witness (its fin
has all three waiters runnable), which the mutant cannot reach. -/

theorem condRun_notifyAll_drains (s : CondState) (t : Tick) (f : FiberId) :
    condRun s t f CondCall.cnotifyAll =
      some (CondRes.rall s.cwaitq.length,
        { s with cwaitq := [], resolved := s.resolved ++ s.cwaitq.map (fun g => (g, true)) },
        s.cwaitq) := rfl

theorem condExtRun_notifyAll_drains (s : CondState) (t : Tick) :
    condExtRun CondCall.cnotifyAll s t =
      some (CondRes.rall s.cwaitq.length,
        { s with cwaitq := [], resolved := s.resolved ++ s.cwaitq.map (fun g => (g, true)) },
        s.cwaitq) := rfl

/-! ## Mutant battery (independent fault classes)

Four single-facet mutants, each in a different fault class, each killed
by the evidence shape its fault class demands:

  * M1 `condFinishM1` — spurious completion (the resolution record is
    not required).  Killed directly by the backing discipline
    `condFinish_consumed`: every modeled completion consumed a record
    whose outcome is the completed bit.  M1 completes with an empty
    record list — the `condFinish_cwait_len` shape fails it.
  * M2 `condExtRunM2` — `notify_all` wakes only the head while still
    publishing the full drained count.  An under-production mutant, so
    no safety invariant can catch it; killed by refuting the drain
    theorem (`condM2_drains_refuted`) and by the credit overclaim
    (`condM2_overclaims`: published wakes are fewer than the count the
    result observation carries — the frozen rule is that `notify_all`
    credits exactly its drained count).  The reachable GOOD witness is
    battery B.
  * M3 `condFinishM3` — the reacquire is skipped (the resumed waiter
    never takes the embedded slot).  Killed directly by the safety
    discipline `condFinish_takes_owner`: a `cwWaiting` completion
    leaves the caller owning the slot.  M3 completes with the slot
    free.
  * M4 `condParkM4` — the wait's register+release section is not
    atomic (the release is skipped, the slot stays held).  An
    under-production fault (lost wakeup), killed by witness
    separation: `condM4_keeps_slot` and `condM4_lost_wakeup` (the
    resolved waiter's resume deadlocks) against battery A, whose
    completion the model reaches through the same shape. -/

/-- Backing helper: a consumed record was a member of the resolved
list. -/
theorem condConsume_mem : ∀ (l : List (FiberId × Bool)) (f : FiberId) (b : Bool)
    (rest : List (FiberId × Bool)), condConsume l f = some (b, rest) → (f, b) ∈ l := by
  intro l
  induction l with
  | nil => intro f b rest h; simp [condConsume] at h
  | cons hd t ih =>
      obtain ⟨g, gb⟩ := hd
      intro f b rest h
      simp only [condConsume] at h
      by_cases hgf : g = f
      · rw [if_pos hgf] at h
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨hgf', hb⟩ := h
        have hpair : (f, b) = (g, gb) := by rw [← hgf', hgf]
        rw [hpair]
        exact List.mem_cons_self
      · rw [if_neg hgf] at h
        cases hc : condConsume t f with
        | none => rw [hc] at h; simp at h
        | some p =>
            rw [hc] at h
            simp only [Option.map_some] at h
            have h3 : (p.1, (g, gb) :: p.2) = (b, rest) := Option.some.inj h
            rw [Prod.mk.injEq] at h3
            have him := ih f p.1 p.2 hc
            rw [h3.1] at him
            exact List.mem_cons_of_mem _ him

/-- Safety discipline (M1's kill): a completed `wait` consumed a
resolution record whose outcome is the completed bit. -/
theorem condFinish_consumed (s : CondState) (f : FiberId) (b : Bool)
    (s' : CondState) (wk : List FiberId)
    (h : condFinish s f CondCall.cwait = some (CondRes.rout b, s', wk)) :
    (f, b) ∈ s.resolved ∧ s'.resolved.length + 1 = s.resolved.length := by
  simp only [condFinish] at h
  cases hph : s.phase f with
  | cwFresh => rw [hph] at h; simp at h
  | cwWaiting =>
      rw [hph] at h
      cases hc : condConsume s.resolved f with
      | none => rw [hc] at h; simp at h
      | some p =>
          rw [hc] at h
          cases hown : s.owner with
          | none =>
              rw [hown] at h
              simp only [Option.some.injEq, Prod.mk.injEq, CondRes.rout.injEq] at h
              obtain ⟨hb, hst, _⟩ := h
              subst hst
              refine ⟨?_, ?_⟩
              · have hm := condConsume_mem s.resolved f p.1 p.2 hc
                rwa [hb] at hm
              · have hcon := condConsume_len s.resolved f p.1 p.2 hc
                simp [hcon]
          | some o' => rw [hown] at h; simp at h
  | cwReacq =>
      rw [hph] at h
      cases hc : condConsume s.resolved f with
      | none => rw [hc] at h; simp at h
      | some p =>
          rw [hc] at h
          simp only [Option.some.injEq, Prod.mk.injEq, CondRes.rout.injEq] at h
          obtain ⟨hb, hst, _⟩ := h
          subst hst
          refine ⟨?_, ?_⟩
          · have hm := condConsume_mem s.resolved f p.1 p.2 hc
            rwa [hb] at hm
          · have hcon := condConsume_len s.resolved f p.1 p.2 hc
            simp [hcon]

/-- Safety discipline (M3's kill): a `cwWaiting` completion reacquires
— the caller leaves with the embedded slot. -/
theorem condFinish_takes_owner (s : CondState) (f : FiberId) (b : Bool)
    (s' : CondState) (wk : List FiberId)
    (h : condFinish s f CondCall.cwait = some (CondRes.rout b, s', wk))
    (hph : s.phase f = CPhase.cwWaiting) :
    s'.owner = some f := by
  simp only [condFinish] at h
  rw [hph] at h
  cases hc : condConsume s.resolved f with
  | none => rw [hc] at h; simp at h
  | some p =>
      rw [hc] at h
      cases hown : s.owner with
      | none =>
          rw [hown] at h
          simp only [Option.some.injEq, Prod.mk.injEq, CondRes.rout.injEq] at h
          obtain ⟨_, hst, _⟩ := h
          subst hst
          rfl
      | some o' => rw [hown] at h; simp at h

/-- Release discipline (M4's kill): a fresh `wait`'s section over an
empty mutex queue frees the embedded slot (scheduler_condition.cpp:63-65). -/
theorem condPark_frees_slot (s : CondState) (f : FiberId)
    (s' : CondState) (wk : List FiberId)
    (h : condPark s f CondCall.cwait = some (s', wk))
    (hph : s.phase f = CPhase.cwFresh) (hmq : s.mwaitq = []) :
    s'.owner = none := by
  simp only [condPark, hph, hmq, Option.some.injEq, Prod.mk.injEq] at h
  obtain ⟨hst, _⟩ := h
  subst hst
  rfl

/-- M1: spurious completion — the record is never consulted. -/
def condFinishM1 : CondState → FiberId → CondCall →
    Option (CondRes × CondState × List FiberId)
  | s, f, CondCall.cwait =>
      match s.phase f with
      | CPhase.cwWaiting =>
          match s.owner with
          | none =>
              some (CondRes.rout true,
                { s with
                  owner := some f,
                  phase := (fun g => if g = f then CPhase.cwFresh else s.phase g) },
                [])
          | some _ => none
      | CPhase.cwReacq =>
          some (CondRes.rout true,
            { s with phase := (fun g => if g = f then CPhase.cwFresh else s.phase g) },
            [])
      | CPhase.cwFresh => none
  | _, _, _ => none

/-- M1 killed: the mutant completes a `cwWaiting` waiter whose resolved
list is empty — no record existed, none was consumed. -/
theorem condM1_spurious :
    ∃ s' : CondState,
      condFinishM1 { owner := none, mwaitq := [], cwaitq := [], phase := fun _ => CPhase.cwWaiting, resolved := [] }
          0 CondCall.cwait = some (CondRes.rout true, s', []) ∧
      s'.resolved = [] := by
  refine ⟨{ owner := some 0, mwaitq := [], cwaitq := [], phase := fun g => if g = 0 then CPhase.cwFresh else CPhase.cwWaiting, resolved := [] }, ?_, rfl⟩
  rfl

/-- M2: `notify_all` readies only the queue head but still publishes
the full drained count (the fiber-origin facet is the same fault). -/
def condExtRunM2 : CondCall → CondState → Tick →
    Option (CondRes × CondState × List FiberId)
  | CondCall.cnotifyAll, s, _ =>
      match s.cwaitq with
      | [] => some (CondRes.rall 0, s, [])
      | h :: t => some (CondRes.rall s.cwaitq.length,
        { s with cwaitq := t, resolved := s.resolved ++ [(h, true)] }, [h])
  | c, s, t => condExtRun c s t

/-- M2 killed (drain theorem refuted): the mutant's drain leaves all
but the head queued. -/
theorem condM2_drains_refuted :
    ¬ ∀ (s : CondState) (t : Tick),
        condExtRunM2 CondCall.cnotifyAll s t =
          some (CondRes.rall s.cwaitq.length,
            { s with cwaitq := [], resolved := s.resolved ++ s.cwaitq.map (fun g => (g, true)) },
            s.cwaitq) := by
  intro hex
  have h := hex { owner := none, mwaitq := [], cwaitq := [1, 2, 3], phase := fun _ => CPhase.cwFresh, resolved := [] } 0
  simp [condExtRunM2] at h

/-- M2 killed (credit overclaim): the result observation carries the
full drained count while only one wake was published. -/
theorem condM2_overclaims :
    ∃ (s : CondState) (v : CondState) (wk : List FiberId),
      condExtRunM2 CondCall.cnotifyAll s 0 = some (CondRes.rall s.cwaitq.length, v, wk) ∧
      wk.length < s.cwaitq.length := by
  refine ⟨{ owner := none, mwaitq := [], cwaitq := [1, 2, 3], phase := fun _ => CPhase.cwFresh, resolved := [] },
    { owner := none, mwaitq := [], cwaitq := [2, 3], phase := fun _ => CPhase.cwFresh, resolved := [(1, true)] },
    [1], ?_, ?_⟩
  · rfl
  · decide

/-- M3: the reacquire is skipped — the record is consumed but the
embedded slot is never taken. -/
def condFinishM3 : CondState → FiberId → CondCall →
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
                      resolved := rest,
                      phase := (fun g => if g = f then CPhase.cwFresh else s.phase g) },
                    [])
              | some _ => none
          | none => none
      | CPhase.cwReacq =>
          match condConsume s.resolved f with
          | some (b, rest) =>
              some (CondRes.rout b,
                { s with
                  resolved := rest,
                  phase := (fun g => if g = f then CPhase.cwFresh else s.phase g) },
                [])
          | none => none
      | CPhase.cwFresh => none
  | _, _, _ => none

/-- M3 killed: the mutant completes the waiter with the slot still
free — `condFinish_takes_owner` fails it. -/
theorem condM3_reacquire_skipped :
    ∃ s' : CondState,
      condFinishM3 { owner := none, mwaitq := [], cwaitq := [], phase := fun _ => CPhase.cwWaiting, resolved := [(0, true)] }
          0 CondCall.cwait = some (CondRes.rout true, s', []) ∧
      s'.owner = none := by
  refine ⟨{ owner := none, mwaitq := [], cwaitq := [], phase := fun g => if g = 0 then CPhase.cwFresh else CPhase.cwWaiting, resolved := [] }, ?_, rfl⟩
  rfl

/-- M4: the wait section is not atomic — the register happens but the
release-handoff is skipped, so the slot stays held. -/
def condParkM4 : CondState → FiberId → CondCall →
    Option (CondState × List FiberId)
  | s, f, CondCall.cwait =>
      match s.phase f with
      | CPhase.cwFresh =>
          match s.mwaitq with
          | [] =>
              some
                ({ s with
                    cwaitq := s.cwaitq ++ [f],
                    phase := (fun g => if g = f then CPhase.cwWaiting else s.phase g) },
                  [])
          | g :: t =>
              some
                ({ s with
                    cwaitq := s.cwaitq ++ [f],
                    phase := (fun g => if g = f then CPhase.cwWaiting else s.phase g),
                    owner := some g,
                    mwaitq := t },
                  [g])
      | CPhase.cwWaiting =>
          some
            ({ s with
                mwaitq := s.mwaitq ++ [f],
                phase := (fun g => if g = f then CPhase.cwReacq else s.phase g) },
              [])
      | CPhase.cwReacq => none
  | _, _, _ => none

/-- M4 killed (slot discipline): the mutant parks with the slot still
held — `condPark_frees_slot` fails it. -/
theorem condM4_keeps_slot :
    ∃ p : CondState,
      condParkM4 { owner := some 0, mwaitq := [], cwaitq := [], phase := fun _ => CPhase.cwFresh, resolved := [] }
          0 CondCall.cwait = some (p, []) ∧
      p.owner = some 0 ∧ p.phase 0 = CPhase.cwWaiting ∧ p.cwaitq = [0] := by
  refine ⟨{ owner := some 0, mwaitq := [], cwaitq := [0], phase := fun g => if g = 0 then CPhase.cwWaiting else CPhase.cwFresh, resolved := [] }, ?_, rfl, rfl, rfl⟩
  rfl

/-- M4 killed (lost wakeup): with the slot stuck held, the notified
waiter's resume consumes nothing and deadlocks — no completion exists
on this path, while battery A reaches it in the model. -/
theorem condM4_lost_wakeup :
    ∃ p : CondState,
      condParkM4 { owner := some 0, mwaitq := [], cwaitq := [], phase := fun _ => CPhase.cwFresh, resolved := [] }
          0 CondCall.cwait = some (p, []) ∧
      condFinish { p with resolved := [(0, true)] } 0 CondCall.cwait = none := by
  obtain ⟨p, hp, hown, hph, hcq⟩ := condM4_keeps_slot
  refine ⟨p, hp, ?_⟩
  show condFinish { p with resolved := [(0, true)] } 0 CondCall.cwait = none
  simp only [condFinish, hph, hcq]
  simp [condConsume, hown]

end Sluice.Formal