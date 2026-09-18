/-
Sluice Stage 5V2.3 — AsyncRwLock core.

This stage re-adjudicates the AsyncRwLock core on the post-#378
Stage-0-V2.3 authority as amended (`CalcV2.lean`: `park` publishes the
wakes its pre-suspension section readied, and the section-ending steps
take wakes as an order-preserving `Wakes` sublist), from the current
production code:

  `include/sluice/async/async_rwlock.hpp`,
  `src/async/scheduler_rwlock.cpp`.

Call-domain census (from the code):

  `try_read_lock`  external-capable (`rwlock_try_read_lock` :135-144,
                   only `global_mtx_` + the queue mutex; no `g_worker`
                   read); succeeds iff the writer bit is clear and the
                   queue is empty.
  `try_write_lock` fiber-bound (`rwlock_try_write_lock` :290-299 reads
                   `g_worker`); a recursive attempt is a plain `false`
                   (:305-306); grants only over a fully free lock with
                   an empty queue (:307).
  `read_lock`      fiber-bound (`rwlock_read_lock` :258-288); the
                   `admit` section resolves inline when the fresh node
                   lands at the head of an empty queue with the writer
                   bit clear (:165-172), else the caller suspends.
  `write_lock`     fiber-bound (`rwlock_write_lock` :315-344); a
                   recursive attempt is a caller-precondition abort
                   (:206-208); the inline grant additionally needs zero
                   readers (:223-230).
  `unlock_read`    external-capable (`rwlock_unlock_read` :346-356, no
                   `g_worker` read); the caller contract requires a held
                   share (:349); the grant pass runs only when the count
                   reaches zero (:352-355).
  `unlock_write`   fiber-bound (`rwlock_unlock_write` :358-366); both
                   the inactive-writer and the not-owner cases are
                   caller-precondition aborts (:372-377).
  `cancel`         external-capable (`rwlock_cancel` :384-400, only
                   `global_mtx_` + the queue mutex); removes the queued
                   node, re-runs the grant pass (:396), publishes the
                   cancelled node (:398).
  `read_lock_until` / `write_lock_until` / `rwlock_expire_wait`
                   timed extensions, outside the core surface (same
                   disposition as `acquire_until`/`lock_until`/`wait_until`).

The grant policy both releases share: `rwlock_grant_from_head_locked`
(:38-133) serves the queue head — a queued writer only when the lock is
fully free (:60-69); a queued reader drains the maximal leading run of
readers, stopping at the first queued writer (:71-111).

Modeling disclosures (each preserves the reachable trace language):

  * The modeled `read_lock`/`write_lock` result is `unit`: both C++
    entry points return `void`, so a cancelled waiter's completion is
    observably a completion; the grant itself is a silent state effect
    (the count/bit change in the granter's section).  What the model
    carries instead is the resolved-record backing discipline (every
    completion consumed a resolution record) and the state ledgers.
  * `unlock_read`'s caller contract (`active_readers > 0`, :349) is a
    dispatch-refusal (`admit` refuses the dispatch) for the fiber path,
    exactly as the
    mutex stage modeled its caller preconditions; for the external path
    the section is non-executable in that state (`extRun = none`: the
    fail-fast produces no completion observation).
  * `active_readers` counts shares, not holders: `unlock_read` carries
    no caller identity, so the model decrements the count without an
    owner check — the C++ does the same (:346-356).  The writer side is
    owner-checked end to end (`writer_owner_`).
  * A granted waiter's resume completes with no further state change
    (its grant was applied by the granter's section, :122-132); the
    model's `finish` consumes the resolved record.  The queue is the
    one `WaitQueue waiters_` shared by both modes; each parked call's
    mode rides in its node (`RwWaitCtx::Mode`).

Adjudication:

  * AsyncRwLock — THEOREM B (`rw_irreducible`): irreducible to
    `BASE(AsyncRwLock) = {Mutex}`.  The separating fact is the
    owner-gated *admission* of `unlock_write` (the two caller
    preconditions at scheduler_rwlock.cpp:372-377): the primitive never
    issues an `unlock_write` from a fiber that does not own the active
    writer — `rwAdmit` refuses the dispatch — while every encoding's
    fiber machine dispatches any submitted call unconditionally.  The
    witness `wunlockIssueTrace` (a bare `unlock_write` issue by a fresh
    fiber) is produced by every encoding (`rw_over_produces`) and by no
    run of the primitive (`rw_not_wunlockIssue`).  The recursive
    `write_lock` abort (:206-208) is a second refused-dispatch
    contract the model carries in the same gate.
  * Safety evidence: `rw_exclusion` — at every reachable state the
    modes exclude each other (`writing → readers = 0 ∧ wowner ≠ none`,
    `¬writing → wowner = none`), plus the possession facets (grants
    dequeue and record; completions consume; `unlock_read` pays one
    share; the grant pass fires only from a paying release or the
    owner-matched `unlock_write`/`cancel`).
-/

import Sluice.Formal.ConditionV2

namespace Sluice.Formal

/-! ## API and state -/

inductive RMode : Type where
  | rd
  | wr
deriving instance DecidableEq for RMode

inductive RwCall : Type where
  | rlock
  | runlock
  | wlock
  | wunlock
  | rtry
  | wtry
  | rcancel (f : FiberId)
deriving instance DecidableEq for RwCall

inductive RwRes : Type where
  | runit
  | rbool (b : Bool)
deriving instance DecidableEq for RwRes

abbrev RwSig : ApiSig := ⟨RwCall, RwRes⟩

/-- AsyncRwLock private state: the shared-ownership pair
(`active_readers_` / `writer_active_`), the writer's identity slot
(`writer_owner_`), the one waiter FIFO both modes queue on (each node's
`RwWaitCtx::Mode`), and the resolved-but-unconsumed outcomes of granted
or cancelled waiters awaiting their resume (`true` = granted, `false` =
cancelled). -/
structure RwState where
  readers : Nat
  writing : Bool
  wowner : Option FiberId
  waitq : List (FiberId × RMode)
  resolved : List (FiberId × Bool)

def rwInit : RwState := ⟨0, false, none, [], []⟩

/-- `f` has a queued node (queue membership by fiber). -/
def memQ (f : FiberId) (q : List (FiberId × RMode)) : Bool :=
  q.any (fun p => p.1 = f)

/-- Remove the first entry of `f` (the `WaitQueue::unlink_locked`
shape), or none if `f` is not queued. -/
def rwRemove : List (FiberId × RMode) → FiberId → Option (List (FiberId × RMode))
  | [], _ => none
  | (g, m) :: t, w => if g = w then some t else (fun r => (g, m) :: r) <$> rwRemove t w

/-- Remove the first record of `f`, yielding its outcome (the one-shot
consume of a resolved `WaitNode` at the resumed waiter's finish). -/
def rwConsume : List (FiberId × Bool) → FiberId → Option (Bool × List (FiberId × Bool))
  | [], _ => none
  | (g, b) :: rest, f => if g = f then some (b, rest)
      else (fun p => (p.1, (g, b) :: p.2)) <$> rwConsume rest f

/-- The maximal leading run of queued readers (the batch
`rwlock_grant_from_head_locked` drains, :75-107). -/
def readerPrefix : List (FiberId × RMode) → List (FiberId × RMode)
  | [] => []
  | p@(_, RMode.rd) :: t => p :: readerPrefix t
  | _ :: _ => []

/-- What stays queued after the batch drain. -/
def dropReaders : List (FiberId × RMode) → List (FiberId × RMode)
  | [] => []
  | (_, RMode.rd) :: t => dropReaders t
  | q => q

/-! ## The grant pass -/

/-- One grant pass over the queue head (`rwlock_grant_from_head_locked`,
scheduler_rwlock.cpp:38-133).  The head decides: a queued writer is
claimed only when the lock is fully free (no readers, no writer,
:60-69); a queued reader drains the whole leading reader run when no
writer is active (:71-111).  `none` = the queue is empty or the head is
blocked; the granted fibers are returned in queue order. -/
def rwGrantHead (s : RwState) : Option (RwState × List FiberId) :=
  match s.waitq with
  | [] => none
  | (f, RMode.wr) :: t =>
      if s.readers = 0 ∧ s.writing = false then
        some ({ s with writing := true, wowner := some f,
                       waitq := t, resolved := s.resolved ++ [(f, true)] }, [f])
      else none
  | q@(_ :: _) =>
      if s.writing = false then
        some ({ s with readers := s.readers + (readerPrefix q).length,
                       waitq := dropReaders q,
                       resolved := s.resolved ++ (readerPrefix q).map (fun g => (g.1, true)) },
              (readerPrefix q).map (fun g => g.1))
      else none

/-! ## The primitive -/

/-- Entry gate.  The recursive `write_lock` (:206-208) and the
`unlock_write` from a fiber that does not own the active writer
(:372-377) are caller precondition violations: such a call never legally
enters execution, so the dispatch is refused.  `unlock_read`'s
held-share contract (:349) refuses likewise.  The tries, `read_lock`,
and `cancel` are always admissible. -/
def rwAdmit : RwState → FiberId → RwCall → Option RwState
  | s, _, RwCall.rlock => some s
  | s, _, RwCall.runlock => if s.readers > 0 then some s else none
  | s, f, RwCall.wlock => if s.wowner = some f then none else some s
  | s, f, RwCall.wunlock => if s.wowner = some f then some s else none
  | s, _, RwCall.rtry => some s
  | s, _, RwCall.wtry => some s
  | s, _, RwCall.rcancel _ => some s

/-- The inline paths of a dispatched call.

  `read_lock` — the inline grant when the fresh node lands at the head
  of an empty queue with the writer bit clear (:165-172); anything else
  suspends.

  `write_lock` — the inline grant when the lock is fully free and the
  queue is empty (:223-230); anything else suspends.

  `unlock_read` — decrements; at zero the grant pass runs (:352-355).

  `unlock_write` — clears the writer slot and runs the grant pass
  (:378-381).

  `try_read` / `try_write` — grant only over a free lock with an empty
  queue (:139-143, :307); a recursive `try_write` is a plain `false`
  (:305-306). -/
def rwRun : RwState → Tick → FiberId → RwCall →
    Option (RwRes × RwState × List FiberId)
  | s, _, _, RwCall.rlock =>
      if s.writing = false ∧ s.waitq = [] then
        some (RwRes.runit, { s with readers := s.readers + 1 }, [])
      else none
  | s, _, _, RwCall.runlock =>
      if s.readers = 0 then none
      else if s.readers - 1 = 0 then
        match rwGrantHead { s with readers := s.readers - 1 } with
        | some (s2, ws) => some (RwRes.runit, s2, ws)
        | none => some (RwRes.runit, { s with readers := s.readers - 1 }, [])
      else some (RwRes.runit, { s with readers := s.readers - 1 }, [])
  | s, _, f, RwCall.wlock =>
      if s.readers = 0 ∧ s.writing = false ∧ s.waitq = [] then
        some (RwRes.runit, { s with writing := true, wowner := some f }, [])
      else none
  | s, _, _, RwCall.wunlock =>
      match rwGrantHead { s with writing := false, wowner := none } with
      | some (s2, ws) => some (RwRes.runit, s2, ws)
      | none => some (RwRes.runit, { s with writing := false, wowner := none }, [])
  | s, _, _, RwCall.rtry =>
      if s.writing = false ∧ s.waitq = [] then
        some (RwRes.rbool true, { s with readers := s.readers + 1 }, [])
      else some (RwRes.rbool false, s, [])
  | s, _, f, RwCall.wtry =>
      if s.wowner = some f then some (RwRes.rbool false, s, [])
      else if s.readers = 0 ∧ s.writing = false ∧ s.waitq = [] then
        some (RwRes.rbool true, { s with writing := true, wowner := some f }, [])
      else some (RwRes.rbool false, s, [])
  | _, _, _, RwCall.rcancel _ => none

/-- `read_lock` and `write_lock` suspend; parking appends the caller at
the queue tail with its mode (`register_wait_locked` +
`commit_suspend_locked`, :268-277, :325-334).  A fiber already queued is
not re-registered (one node per fiber; the one-in-flight discipline). -/
def rwPark : RwState → FiberId → RwCall →
    Option (RwState × List FiberId)
  | s, f, RwCall.rlock =>
      if memQ f s.waitq then none
      else some ({ s with waitq := s.waitq ++ [(f, RMode.rd)] }, [])
  | s, f, RwCall.wlock =>
      if memQ f s.waitq then none
      else some ({ s with waitq := s.waitq ++ [(f, RMode.wr)] }, [])
  | _, _, _ => none

/-- A resumed waiter consumes its recorded outcome; nothing else resumes
(the grant was applied by the granter's section, :122-132). -/
def rwFinish : RwState → FiberId → RwCall →
    Option (RwRes × RwState × List FiberId)
  | s, f, RwCall.rlock =>
      match rwConsume s.resolved f with
      | some (_, rest) => some (RwRes.runit, { s with resolved := rest }, [])
      | none => none
  | s, f, RwCall.wlock =>
      match rwConsume s.resolved f with
      | some (_, rest) => some (RwRes.runit, { s with resolved := rest }, [])
      | none => none
  | _, _, _ => none

/-- `unlock_read`, `try_read`, and `cancel` are external-capable
(no `g_worker` read in their entry points). -/
def rwExtCap : RwCall → Bool
  | RwCall.runlock => true
  | RwCall.rtry => true
  | RwCall.rcancel _ => true
  | _ => false

/-- The external critical sections.  `unlock_read`'s section is the
release side of `rwRun` (the :349 contract makes it non-executable at
zero readers: the fail-fast produces no completion observation).
`cancel` removes the queued node, re-runs the grant pass (:396), and
publishes the cancelled node (:398). -/
def rwExtRun : RwCall → RwState → Tick →
    Option (RwRes × RwState × List FiberId)
  | RwCall.runlock, s, _ =>
      if s.readers = 0 then none
      else if s.readers - 1 = 0 then
        match rwGrantHead { s with readers := s.readers - 1 } with
        | some (s2, ws) => some (RwRes.runit, s2, ws)
        | none => some (RwRes.runit, { s with readers := s.readers - 1 }, [])
      else some (RwRes.runit, { s with readers := s.readers - 1 }, [])
  | RwCall.rtry, s, _ =>
      if s.writing = false ∧ s.waitq = [] then
        some (RwRes.rbool true, { s with readers := s.readers + 1 }, [])
      else some (RwRes.rbool false, s, [])
  | RwCall.rcancel w, s, _ =>
      match rwRemove s.waitq w with
      | none => some (RwRes.rbool false, s, [])
      | some q1 =>
          match rwGrantHead { s with waitq := q1 } with
          | some (s2, gs) =>
              some (RwRes.rbool true,
                { s2 with resolved := s2.resolved ++ [(w, false)] },
                gs ++ [w])
          | none =>
              some (RwRes.rbool true,
                { s with waitq := q1, resolved := s.resolved ++ [(w, false)] }, [w])
  | _, _, _ => none

/-- The rwlock has no timers in the untimed core. -/
def rwExpire : RwState → Tick → FiberId → Option RwState :=
  fun _ _ _ => none

/-- The primitive, reducible so its facet equations reduce in proofs. -/
@[reducible] def rwPrim : PrimLTS2 RwSig :=
  { State := RwState
    init := rwInit
    admit := rwAdmit
    run := rwRun
    park := rwPark
    finish := rwFinish
    extCap := rwExtCap
    extRun := rwExtRun
    onTick := fun s _ => s
    expire := rwExpire }

/-! ## THEOREM B: irreducibility to the mutex base

The frozen base for this stage: the Stage-3 mutex core as base
capabilities (`BASE(AsyncRwLock) = {Mutex}`) — the same `MutexOps`
oracle the condition stage froze. -/

/-- Non-vacuity witness for the THEOREM-B universal: the closed encoding
class over the mutex substrate is inhabited.  `read_lock` maps to the
natural exclusive shape (take the mutex); everything else bottoms out
with no completed observation — the shared-reader mode is private state
this substrate's program language cannot hold.  Inhabitance is what
THEOREM B needs: the over-production fires at the encoding machine's
dispatch step, before any program runs. -/
def encRwMutex : Encoding MutexOps RwSig where
  prog
    | RwCall.rlock =>
        ExtProg.base MutexCall.mlock (fun _ => ExtProg.pure (SubVal.bool true))
    | _ => ExtProg.pure (SubVal.bool false)
  decode
    | RwCall.rlock, SubVal.bool b => some (RwRes.rbool b)
    | _, _ => none
  extCap
    | RwCall.runlock => true
    | RwCall.rtry => true
    | RwCall.rcancel _ => true
    | _ => false

theorem rw_encoding_class_inhabited :
    ∃ _enc : Encoding MutexOps RwSig, True := ⟨encRwMutex, trivial⟩

/-- The separating witness: a bare `unlock_write` issue by a fresh
fiber.  The owner-gated admission of `unlock_write` (the caller
preconditions the primitive enforces at dispatch,
scheduler_rwlock.cpp:372-377) refuses it, while every encoding's fiber
machine dispatches any submitted call unconditionally. -/
def wunlockIssueTrace : Trace RwSig :=
  [issueObs RwSig (Caller.fiber 0) RwCall.wunlock]

theorem seqOK_wunlockIssue : SeqOK RwSig wunlockIssueTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- A silent prefix from an idle configuration leaves this primitive's
private state untouched: with no fiber dispatched and no external call
in flight, the only silent steps are submits and the inert clock tick. -/
theorem rw_silent_prefix {cfg m : PrimCfg RwSig rwPrim} {t : Trace RwSig}
    (hrun : PrimRuns2 RwSig rwPrim cfg t m) (ht : t = [])
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
                exact absurd hexp (by simp [rwPrim, rwExpire])
          have hip := ih ht2 hfwd.2.1 hfwd.2.2.1 hfwd.2.2.2
          rw [hip]
          exact hfwd.1
      | some ob => simp at hnil

theorem rw_not_wunlockIssue : ¬ TracesPrim RwSig rwPrim wunlockIssueTrace := by
  rintro ⟨fin, hrun⟩
  obtain ⟨ob, t', heq⟩ : ∃ ob t', wunlockIssueTrace = ob :: t' := ⟨_, _, rfl⟩
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := primRuns_cons hrun ob t' heq
  have hprim := rw_silent_prefix h1 rfl rfl rfl (by intro r hr; cases hr)
  cases hstep with
  | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
      injection heq with h3 h4
      simp only [issueObs, Obs.mk.injEq, Caller.fiber.injEq] at h3
      obtain ⟨h5, h6, _⟩ := h3
      have hcall : rp.call = RwCall.wunlock := h6.symm
      have hfib : rp.fiber = 0 := h5.symm
      rw [hcall, hfib, hprim] at hadmit
      simp [rwAdmit, rwInit, rwPrim, primInit] at hadmit
  | extApply x c preE postE hcap hnovel =>
      injection heq with h3 h4
      simp [issueObs] at h3
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
def rwoMid (enc : Encoding MutexOps RwSig) : SysCfg MutexOps RwSig :=
  { st := subInit
    bst := MutexOps.baseInit,
    cur := none,
    parked := [],
    runq := [{ fiber := 0, call := RwCall.wunlock, prog := enc.prog RwCall.wunlock, fresh := true }],
    retired := [],
    nextFiber := 1,
    exts := [] }

/-- The post-dispatch configuration of the encoding witness. -/
def rwoFin (enc : Encoding MutexOps RwSig) : SysCfg MutexOps RwSig :=
  { st := subInit
    bst := MutexOps.baseInit,
    cur := some { fiber := 0, call := RwCall.wunlock, prog := enc.prog RwCall.wunlock },
    parked := [],
    runq := [],
    retired := [],
    nextFiber := 1,
    exts := [] }

theorem enc_wunlockIssue (enc : Encoding MutexOps RwSig) :
    TracesEnc MutexOps RwSig enc wunlockIssueTrace :=
  ⟨rwoFin enc,
    SysRuns.step (encInit MutexOps RwSig) (rwoMid enc) none _ (rwoFin enc)
      (SysStep.submit (encInit MutexOps RwSig) 0 RwCall.wunlock (Or.inl rfl))
      (SysRuns.step (rwoMid enc) (rwoFin enc)
        (some (issueObs RwSig (Caller.fiber 0) RwCall.wunlock)) [] (rwoFin enc)
        (SysStep.dispatchFresh (rwoMid enc)
          { fiber := 0, call := RwCall.wunlock, prog := enc.prog RwCall.wunlock,
            fresh := true } [] rfl rfl rfl)
        (SysRuns.stop (rwoFin enc)))⟩

/-- Every encoding over the mutex base produces the bare `unlock_write`
issue: the discipline's fiber machine submits and dispatches any call
unconditionally. -/
theorem rw_over_produces (enc : Encoding MutexOps RwSig) :
    OverProduces rwPrim MutexOps enc :=
  ⟨wunlockIssueTrace, ⟨enc_wunlockIssue enc, seqOK_wunlockIssue⟩,
    fun hP => rw_not_wunlockIssue hP.1⟩

/-- **THEOREM B** — AsyncRwLock is irreducible to
`BASE(AsyncRwLock) = {Mutex}`: every encoding over the base
over-produces the bare `unlock_write` issue that the primitive's
owner-gated admission forbids. -/
theorem rw_irreducible : IrreducibleTo rwPrim MutexOps :=
  irreducible_of_always_over rw_over_produces

/-! ## Possession facets -/

/-- Admission never modifies the state: a dispatched call enters with
the primitive state it was admitted in. -/
theorem rwAdmit_unchanged {s : RwState} {f : FiberId} {c : RwCall} {s' : RwState}
    (h : rwAdmit s f c = some s') : s' = s := by
  cases c <;> simp only [rwAdmit] at h
  all_goals first
    | simp_all
    | split at h <;> simp_all

/-- The `unlock_write` admission gate: a dispatched `unlock_write` is
issued by the fiber that owns the active writer (:372-377). -/
theorem rwAdmit_wunlock_owner_gated {s : RwState} {f : FiberId} {s' : RwState}
    (h : rwAdmit s f RwCall.wunlock = some s') : s.wowner = some f := by
  have hs : s' = s := rwAdmit_unchanged h
  simp only [rwAdmit, hs] at h
  by_cases hg : s.wowner = some f
  · exact hg
  · rw [if_neg hg] at h
    exact absurd h (by simp)

/-- A queued writer's claim takes the whole writer slot: the head is
dequeued, the slot is owned by the claimed fiber, and a granted record
is published (the writer branch of `rwlock_grant_from_head_locked`,
:60-69). -/
theorem rwGrantHead_claims_head_writer (res : List (FiberId × Bool)) :
    rwGrantHead ⟨0, false, none, [(1, RMode.wr)], res⟩ =
      some (⟨0, true, some 1, [], res ++ [(1, true)]⟩, [1]) := rfl

/-- The reader batch drains the maximal leading run of readers — here
the whole queue — incrementing the count once per granted reader
(:75-111). -/
theorem rwGrantHead_batch_drains (n : Nat) (o : Option FiberId)
    (res : List (FiberId × Bool)) :
    rwGrantHead ⟨n, false, o, [(1, RMode.rd), (2, RMode.rd)], res⟩ =
      some (⟨n + 2, false, o, [], res ++ [(1, true), (2, true)]⟩, [1, 2]) := rfl

/-- The batch stops at the first queued writer (:88-89). -/
theorem rwGrantHead_batch_stops_at_writer (n : Nat) (o : Option FiberId)
    (res : List (FiberId × Bool)) :
    rwGrantHead ⟨n, false, o, [(1, RMode.rd), (2, RMode.rd), (3, RMode.wr)], res⟩ =
      some (⟨n + 2, false, o, [(3, RMode.wr)],
              res ++ [(1, true), (2, true)]⟩, [1, 2]) := rfl

/-- A queued writer is blocked by any active reader (:61). -/
theorem rwGrantHead_writer_blocked_by_readers (o : Option FiberId)
    (res : List (FiberId × Bool)) :
    rwGrantHead ⟨1, false, o, [(2, RMode.wr)], res⟩ = none := rfl

/-- Any grant is blocked while a writer is active (:61, :72). -/
theorem rwGrantHead_blocked_by_writer (f : FiberId) (res : List (FiberId × Bool)) :
    rwGrantHead ⟨0, true, some f, [(1, RMode.rd)], res⟩ = none := rfl

/-- `unlock_read` pays one share (a release over a nonempty count without
the grant pass: the count strictly falls; :351). -/
theorem rwUnlockRead_pays :
    rwRun ⟨2, false, none, [], []⟩ 0 0 RwCall.runlock =
      some (RwRes.runit, ⟨1, false, none, [], []⟩, []) := rfl

/-- The release gate: at the last share the grant pass fires (:352-355). -/
theorem rwUnlockRead_zero_gate (o : Option FiberId) (res : List (FiberId × Bool)) :
    rwRun ⟨1, false, o, [], res⟩ 0 0 RwCall.runlock =
      some (RwRes.runit, ⟨0, false, o, [], res⟩, []) := rfl


/-! ## Possession facets: the backing disciplines -/

/-- Backing helper: a consumed record was a member of the resolved
list. -/
theorem rwConsume_mem : ∀ (l : List (FiberId × Bool)) (f : FiberId) (b : Bool)
    (rest : List (FiberId × Bool)), rwConsume l f = some (b, rest) → (f, b) ∈ l := by
  intro l
  induction l with
  | nil => intro f b rest h; simp [rwConsume] at h
  | cons hd t ih =>
      obtain ⟨g, gb⟩ := hd
      intro f b rest h
      simp only [rwConsume] at h
      by_cases hgf : g = f
      · rw [if_pos hgf] at h
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨hgf', hb⟩ := h
        have hpair : (f, b) = (g, gb) := by rw [← hgf', hgf]
        rw [hpair]
        exact List.mem_cons_self
      · rw [if_neg hgf] at h
        cases hc : rwConsume t f with
        | none => rw [hc] at h; simp at h
        | some p =>
            rw [hc] at h
            have h3 : (p.1, (g, gb) :: p.2) = (b, rest) := by simpa using h
            rw [Prod.mk.injEq] at h3
            have him := ih f p.1 p.2 hc
            rw [h3.1] at him
            exact List.mem_cons_of_mem _ him

/-- Backing helper: a consumed record shrinks the resolved list by
one. -/
theorem rwConsume_len : ∀ (l : List (FiberId × Bool)) (f : FiberId) (b : Bool)
    (rest : List (FiberId × Bool)),
    rwConsume l f = some (b, rest) → rest.length + 1 = l.length := by
  intro l
  induction l with
  | nil => intro f b rest h; simp [rwConsume] at h
  | cons hd t ih =>
      obtain ⟨g, gb⟩ := hd
      intro f b rest h
      simp only [rwConsume] at h
      by_cases hgf : g = f
      · rw [if_pos hgf] at h
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨_, hrest⟩ := h
        rw [hrest]
        simp
      · rw [if_neg hgf] at h
        cases hc : rwConsume t f with
        | none => rw [hc] at h; simp at h
        | some p =>
            rw [hc] at h
            have h3 : (p.1, (g, gb) :: p.2) = (b, rest) := by simpa using h
            rw [Prod.mk.injEq] at h3
            have him := ih f p.1 p.2 hc
            rw [← h3.2]
            simp only [List.length_cons]
            omega

/-- Safety discipline: a completed `read_lock` consumed a resolution
record — the grant or cancellation a resolver published — and the
record list shrank by one.  A completion without a backing record is
not producible. -/
theorem rwFinish_consumed (s : RwState) (f : FiberId) (s' : RwState)
    (h : rwFinish s f RwCall.rlock = some (RwRes.runit, s', [])) :
    ∃ b, (f, b) ∈ s.resolved ∧ s'.resolved.length + 1 = s.resolved.length := by
  simp only [rwFinish] at h
  cases hc : rwConsume s.resolved f with
  | none => rw [hc] at h; simp at h
  | some p =>
      rw [hc] at h
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨_, hst, _⟩ := h
      subst hst
      exact ⟨p.1, rwConsume_mem s.resolved f p.1 p.2 hc, by
        have hcon := rwConsume_len s.resolved f p.1 p.2 hc
        simp [hcon]⟩

/-- Safety discipline: same backing fact for a completed `write_lock`. -/
theorem rwFinishW_consumed (s : RwState) (f : FiberId) (s' : RwState)
    (h : rwFinish s f RwCall.wlock = some (RwRes.runit, s', [])) :
    ∃ b, (f, b) ∈ s.resolved ∧ s'.resolved.length + 1 = s.resolved.length := by
  simp only [rwFinish] at h
  cases hc : rwConsume s.resolved f with
  | none => rw [hc] at h; simp at h
  | some p =>
      rw [hc] at h
      simp only [Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨_, hst, _⟩ := h
      subst hst
      exact ⟨p.1, rwConsume_mem s.resolved f p.1 p.2 hc, by
        have hcon := rwConsume_len s.resolved f p.1 p.2 hc
        simp [hcon]⟩

/-! ## Mode exclusion -/

/-- The exclusion predicate: the modes exclude each other, and the
writer slot is owned exactly while a writer is active (the discipline
`rwlock_grant_from_head_locked` + the claim/clear sites maintain). -/
@[reducible] def rwExclP (s : RwState) : Prop :=
  (s.writing = true → s.readers = 0 ∧ s.wowner ≠ none) ∧
  (s.writing = false → s.wowner = none)

/-- The grant pass preserves exclusion. -/
theorem rwGrantHead_preserves {s s' : RwState} {ws : List FiberId}
    (h : rwGrantHead s = some (s', ws)) (hx : rwExclP s) : rwExclP s' := by
  rw [rwExclP] at hx
  rw [rwExclP]
  obtain ⟨h1, h2⟩ := hx
  simp only [rwGrantHead] at h
  split at h
  · exact absurd h (by simp)
  · rename_i f tl
    split at h
    · rename_i hgrd
      obtain ⟨hr0, _⟩ := hgrd
      simp at h
      obtain ⟨hp, _⟩ := h
      subst hp
      exact ⟨fun _ => ⟨hr0, by simp⟩, fun hwf' => by simp at hwf'⟩
    · exact absurd h (by simp)
  · split at h
    · rename_i hgrd
      simp at h
      obtain ⟨hp, _⟩ := h
      subst hp
      exact ⟨fun hw' => absurd hw' (by simp [hgrd]), h2⟩
    · exact absurd h (by simp)

/-- The inline sections preserve exclusion. -/
theorem rwRun_preserves {s s' : RwState} {t : Tick} {f : FiberId} {c : RwCall}
    {r : RwRes} {ws : List FiberId}
    (h : rwRun s t f c = some (r, s', ws)) (hx : rwExclP s) : rwExclP s' := by
  rw [rwExclP] at hx
  rw [rwExclP]
  obtain ⟨h1, h2⟩ := hx
  cases c with
  | rlock =>
      simp only [rwRun] at h
      by_cases hgrd : s.writing = false ∧ s.waitq = []
      · rw [if_pos hgrd] at h
        simp at h
        obtain ⟨_, hp, _⟩ := h
        subst hp
        exact ⟨fun hw' => absurd hw' (by simp [hgrd]), h2⟩
      · rw [if_neg hgrd] at h
        exact absurd h (by simp)
  | runlock =>
      simp only [rwRun] at h
      by_cases h0 : s.readers = 0
      · rw [if_pos h0] at h
        exact absurd h (by simp)
      · rw [if_neg h0] at h
        by_cases hz : s.readers - 1 = 0
        · rw [if_pos hz] at h
          have hx1 : rwExclP { s with readers := s.readers - 1 } := by
            refine ⟨fun hw' => ?_, fun hnw' => h2 hnw'⟩
            obtain ⟨hr0, hown⟩ := h1 hw'
            simp [hr0, hown]
          cases hgrant : rwGrantHead { s with readers := s.readers - 1 } with
          | none =>
              simp only [hgrant] at h
              simp at h
              obtain ⟨_, hp, _⟩ := h
              subst hp
              refine ⟨fun hw' => ?_, fun hnw' => h2 hnw'⟩
              obtain ⟨hr0, hown⟩ := h1 hw'
              simp [hr0, hown]
          | some p =>
              obtain ⟨s2, ws2⟩ := p
              simp only [hgrant] at h
              simp at h
              obtain ⟨_, hp, _⟩ := h
              subst hp
              exact rwGrantHead_preserves hgrant hx1
        · rw [if_neg hz] at h
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          refine ⟨fun hw' => ?_, fun hnw' => h2 hnw'⟩
          obtain ⟨hr0, hown⟩ := h1 hw'
          simp [hr0, hown]
  | wlock =>
      simp only [rwRun] at h
      by_cases hgrd : s.readers = 0 ∧ s.writing = false ∧ s.waitq = []
      · rw [if_pos hgrd] at h
        obtain ⟨hr0, hwf, _⟩ := hgrd
        simp at h
        obtain ⟨_, hp, _⟩ := h
        subst hp
        exact ⟨fun _ => ⟨hr0, by simp⟩, fun hwf' => by simp at hwf'⟩
      · rw [if_neg hgrd] at h
        exact absurd h (by simp)
  | wunlock =>
      simp only [rwRun] at h
      have hx1 : rwExclP { s with writing := false, wowner := none } :=
        ⟨fun hw' => absurd hw' (by simp), fun _ => rfl⟩
      cases hgrant : rwGrantHead { s with writing := false, wowner := none } with
      | none =>
          rw [hgrant] at h
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          exact hx1
      | some p =>
          obtain ⟨s2, ws2⟩ := p
          simp only [hgrant] at h
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          exact rwGrantHead_preserves hgrant hx1
  | rtry =>
      simp only [rwRun] at h
      by_cases hgrd : s.writing = false ∧ s.waitq = []
      · rw [if_pos hgrd] at h
        simp at h
        obtain ⟨_, hp, _⟩ := h
        subst hp
        exact ⟨fun hw' => absurd hw' (by simp [hgrd]), h2⟩
      · rw [if_neg hgrd] at h
        simp at h
        obtain ⟨_, hp, _⟩ := h
        subst hp
        exact ⟨h1, h2⟩
  | wtry =>
      simp only [rwRun] at h
      by_cases hrec : s.wowner = some f
      · rw [if_pos hrec] at h
        simp at h
        obtain ⟨_, hp, _⟩ := h
        subst hp
        exact ⟨h1, h2⟩
      · rw [if_neg hrec] at h
        by_cases hgrd : s.readers = 0 ∧ s.writing = false ∧ s.waitq = []
        · rw [if_pos hgrd] at h
          obtain ⟨hr0, hwf, _⟩ := hgrd
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          exact ⟨fun _ => ⟨hr0, by simp⟩, fun hwf' => by simp at hwf'⟩
        · rw [if_neg hgrd] at h
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          exact ⟨h1, h2⟩
  | rcancel w => simp only [rwRun] at h; exact absurd h (by simp)

/-- Parking appends the caller at the queue tail; the ownership modes
are untouched. -/
theorem rwPark_preserves {s s' : RwState} {f : FiberId} {c : RwCall}
    {wk : List FiberId}
    (h : rwPark s f c = some (s', wk)) (hx : rwExclP s) : rwExclP s' := by
  cases c with
  | rlock =>
      simp only [rwPark] at h
      by_cases hq : memQ f s.waitq
      · rw [if_pos hq] at h; exact absurd h (by simp)
      · rw [if_neg hq] at h; simp at h; obtain ⟨hp, _⟩ := h; subst hp; exact hx
  | wlock =>
      simp only [rwPark] at h
      by_cases hq : memQ f s.waitq
      · rw [if_pos hq] at h; exact absurd h (by simp)
      · rw [if_neg hq] at h; simp at h; obtain ⟨hp, _⟩ := h; subst hp; exact hx
  | _ => simp only [rwPark] at h; exact absurd h (by simp)

/-- A resumed waiter's finish replaces the resolved list only. -/
theorem rwFinish_preserves {s s' : RwState} {f : FiberId} {c : RwCall}
    {r : RwRes} {ws : List FiberId}
    (h : rwFinish s f c = some (r, s', ws)) (hx : rwExclP s) : rwExclP s' := by
  cases c with
  | rlock =>
      simp only [rwFinish] at h
      cases hc : rwConsume s.resolved f with
      | none => rw [hc] at h; simp at h
      | some p =>
          rw [hc] at h
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          exact hx
  | wlock =>
      simp only [rwFinish] at h
      cases hc : rwConsume s.resolved f with
      | none => rw [hc] at h; simp at h
      | some p =>
          rw [hc] at h
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          exact hx
  | _ => simp only [rwFinish] at h; exact absurd h (by simp)

/-- The external sections preserve exclusion (same release/try shapes;
`cancel` touches the queue and the record list, not the modes). -/
theorem rwExtRun_preserves {s s' : RwState} {c : RwCall} {t : Tick}
    {r : RwRes} {ws : List FiberId}
    (h : rwExtRun c s t = some (r, s', ws)) (hx : rwExclP s) : rwExclP s' := by
  rw [rwExclP] at hx
  rw [rwExclP]
  obtain ⟨h1, h2⟩ := hx
  cases c with
  | runlock =>
      simp only [rwExtRun] at h
      by_cases h0 : s.readers = 0
      · rw [if_pos h0] at h
        exact absurd h (by simp)
      · rw [if_neg h0] at h
        by_cases hz : s.readers - 1 = 0
        · rw [if_pos hz] at h
          have hx1 : rwExclP { s with readers := s.readers - 1 } := by
            refine ⟨fun hw' => ?_, fun hnw' => h2 hnw'⟩
            obtain ⟨hr0, hown⟩ := h1 hw'
            simp [hr0, hown]
          cases hgrant : rwGrantHead { s with readers := s.readers - 1 } with
          | none =>
              simp only [hgrant] at h
              simp at h
              obtain ⟨_, hp, _⟩ := h
              subst hp
              refine ⟨fun hw' => ?_, fun hnw' => h2 hnw'⟩
              obtain ⟨hr0, hown⟩ := h1 hw'
              simp [hr0, hown]
          | some p =>
              obtain ⟨s2, ws2⟩ := p
              simp only [hgrant] at h
              simp at h
              obtain ⟨_, hp, _⟩ := h
              subst hp
              exact rwGrantHead_preserves hgrant hx1
        · rw [if_neg hz] at h
          simp at h
          obtain ⟨_, hp, _⟩ := h
          subst hp
          refine ⟨fun hw' => ?_, fun hnw' => h2 hnw'⟩
          obtain ⟨hr0, hown⟩ := h1 hw'
          simp [hr0, hown]
  | rtry =>
      simp only [rwExtRun] at h
      by_cases hgrd : s.writing = false ∧ s.waitq = []
      · rw [if_pos hgrd] at h
        simp at h
        obtain ⟨_, hp, _⟩ := h
        subst hp
        exact ⟨fun hw' => absurd hw' (by simp [hgrd]), h2⟩
      · rw [if_neg hgrd] at h
        simp at h
        obtain ⟨_, hp, _⟩ := h
        subst hp
        exact ⟨h1, h2⟩
  | rcancel w =>
      simp only [rwExtRun] at h
      cases hr : rwRemove s.waitq w with
      | none => rw [hr] at h; simp at h; obtain ⟨_, hp, _⟩ := h; subst hp; exact ⟨h1, h2⟩
      | some q1 =>
          rw [hr] at h
          cases hgrant : rwGrantHead { s with waitq := q1 } with
          | none =>
              simp only [hgrant] at h
              simp at h
              obtain ⟨_, hp, _⟩ := h
              subst hp
              exact ⟨h1, h2⟩
          | some p =>
              obtain ⟨s2, gs⟩ := p
              simp only [hgrant] at h
              simp at h
              obtain ⟨_, hp, _⟩ := h
              subst hp
              have hx2 := rwGrantHead_preserves hgrant ⟨h1, h2⟩
              exact ⟨hx2.1, hx2.2⟩
  | _ => simp only [rwExtRun] at h; exact absurd h (by simp)

/-- The primitive never leaves a mixed-mode state: at every reachable
configuration the modes exclude each other and the writer slot is
owned exactly while a writer is active. -/
theorem rw_exclusion {cfg m : PrimCfg RwSig rwPrim} {t : Trace RwSig}
    (hrun : PrimRuns2 RwSig rwPrim cfg t m) :
    rwExclP cfg.prim → rwExclP m.prim := by
  induction hrun with
  | stop cfg => exact fun h => h
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro h
      refine ih ?_
      cases hstep with
      | submit f c hsub => exact h
      | fiberDone d r hcurE => exact h
      | extApply x c preE postE hcap hnovel => exact h
      | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
          rw [rwAdmit_unchanged hadmit]; exact h
      | dispatchResumed rp rest hcurE hrunqE hfreshE => exact h
      | fiberEffect d b ps rest r s' wk hcurE hb hrunE hmap hwake =>
          exact rwRun_preserves hrunE h
      | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
          exact rwPark_preserves hparkE h
      | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
          exact rwFinish_preserves hfinE h
      | extEffect preE postE e ps rest r s' wk hextsE hresE hrunE hmap hwake =>
          exact rwExtRun_preserves hrunE h
      | extDone preE postE e r hextsE hresE => exact h
      | envTime t0 hcurE hle => exact h
      | envExpire f s' preP postP p hcurE hexp hparE hf =>
          exact absurd hexp (by simp [rwPrim, rwExpire])

/-- The `primInit` state satisfies exclusion, so every run endpoint
does. -/
theorem rw_exclusion_holds {fin : PrimCfg RwSig rwPrim} {t : Trace RwSig}
    (hrun : PrimRuns2 RwSig rwPrim (primInit RwSig rwPrim) t fin) :
    rwExclP fin.prim := by
  refine rw_exclusion hrun ?_
  rw [rwExclP]
  constructor
  · intro hw
    simp [rwInit, primInit] at hw
  · intro _
    rfl

/-! ## Possession batteries (reachability witnesses) -/

/-- Battery A — the inline read grant: a fresh `read_lock` over a free,
unqueued lock issues, grants inline (no suspension), and completes; the
share is held. -/
theorem rw_battery_inline_read :
    ∃ fin : PrimCfg RwSig rwPrim,
      PrimRuns2 RwSig rwPrim
        { prim := rwInit,
          now := 0,
          cur := none,
          parked := ([] : List (Pnd RwSig)),
          runq := [{ fiber := 0, call := RwCall.rlock, fresh := true }],
          retired := ([] : List FiberId),
          nextFiber := 1,
          exts := ([] : List (ExtPend RwSig)) }
        [issueObs RwSig (Caller.fiber 0) RwCall.rlock,
          compObs RwSig (Caller.fiber 0) RwCall.rlock RwRes.runit]
        fin ∧
      fin.prim.readers = 1 ∧ fin.prim.writing = false ∧ fin.prim.wowner = none ∧
        fin.prim.waitq = [] ∧ fin.prim.resolved = [] := by
  refine ⟨{ prim := ⟨1, false, none, [], []⟩,
            now := 0, cur := none, parked := [], runq := [], retired := [0],
            nextFiber := 1, exts := [] },
    ?_, rfl, rfl, rfl, rfl, rfl⟩
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := rwInit,
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 0, call := RwCall.rlock, fresh := true }],
      retired := [], nextFiber := 1, exts := [] }
    { prim := rwInit,
      now := 0, cur := some (FSlot.running { fiber := 0, call := RwCall.rlock } false),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.dispatchFresh (A := RwSig) (P := rwPrim)
      { prim := rwInit,
        now := 0, cur := none, parked := [],
        runq := [{ fiber := 0, call := RwCall.rlock, fresh := true }],
        retired := [], nextFiber := 1, exts := [] }
      { fiber := 0, call := RwCall.rlock, fresh := true } [] rwInit
      rfl rfl rfl (by rfl)) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := rwInit,
      now := 0, cur := some (FSlot.running { fiber := 0, call := RwCall.rlock } false),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    { prim := ⟨1, false, none, [], []⟩,
      now := 0, cur := some (FSlot.returning { fiber := 0, call := RwCall.rlock } RwRes.runit),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.fiberEffect (A := RwSig) (P := rwPrim)
      _ { fiber := 0, call := RwCall.rlock } false [] [] RwRes.runit ⟨1, false, none, [], []⟩ []
      rfl rfl (by rfl) rfl Wakes.nil) ?_
  exact PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [], []⟩,
      now := 0, cur := some (FSlot.returning { fiber := 0, call := RwCall.rlock } RwRes.runit),
      parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }
    { prim := ⟨1, false, none, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [0],
      nextFiber := 1, exts := [] }
    _ _ _ (PrimStep2.fiberDone (A := RwSig) (P := rwPrim)
      _ { fiber := 0, call := RwCall.rlock } RwRes.runit rfl)
    (PrimRuns2.stop _)

/-- Battery B — the writer handoff: with one reader holding and a
writer queued, the reader's release reaches zero and the grant pass
claims the writer, which resumes and completes; the writer slot is
owned by the resumed writer. -/
theorem rw_battery_writer_handoff :
    ∃ fin : PrimCfg RwSig rwPrim,
      PrimRuns2 RwSig rwPrim
        { prim := ⟨1, false, none, [(1, RMode.wr)], []⟩,
          now := 0,
          cur := none,
          parked := [{ fiber := 1, call := RwCall.wlock }],
          runq := ([] : List (PReady RwSig)),
          retired := ([] : List FiberId),
          nextFiber := 2,
          exts := ([] : List (ExtPend RwSig)) }
        [issueObs RwSig (Caller.ext 0) RwCall.runlock,
          compObs RwSig (Caller.fiber 1) RwCall.wlock RwRes.runit,
          compObs RwSig (Caller.ext 0) RwCall.runlock RwRes.runit]
        fin ∧
      fin.prim.writing = true ∧ fin.prim.wowner = some 1 ∧ fin.prim.readers = 0 ∧
        fin.prim.waitq = [] ∧ fin.prim.resolved = [] := by
  refine ⟨{ prim := ⟨0, true, some 1, [], []⟩,
            now := 0, cur := none, parked := [], runq := [], retired := [1],
            nextFiber := 2, exts := [] },
    ?_, rfl, rfl, rfl, rfl, rfl⟩
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [(1, RMode.wr)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.wlock }],
      runq := [], retired := [], nextFiber := 2, exts := [] }
    { prim := ⟨1, false, none, [(1, RMode.wr)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.wlock }],
      runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := none }] }
    _ _ _ (PrimStep2.extApply (A := RwSig) (P := rwPrim)
      { prim := ⟨1, false, none, [(1, RMode.wr)], []⟩,
        now := 0, cur := none,
        parked := [{ fiber := 1, call := RwCall.wlock }],
        runq := [], retired := [], nextFiber := 2, exts := [] }
      0 RwCall.runlock [] [] (by rfl) (by simp)) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [(1, RMode.wr)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.wlock }],
      runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := none }] }
    { prim := ⟨0, true, some 1, [], [(1, true)]⟩,
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := RwCall.wlock, fresh := false }],
      retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.extEffect (A := RwSig) (P := rwPrim)
      _ [] [] { x := 0, call := RwCall.runlock, result := none } [{ fiber := 1, call := RwCall.wlock }]
      [] RwRes.runit ⟨0, true, some 1, [], [(1, true)]⟩ [1]
      rfl rfl (by rfl) rfl (Wakes.drop (A := RwSig) { fiber := 1, call := RwCall.wlock }
        Wakes.nil)) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨0, true, some 1, [], [(1, true)]⟩,
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := RwCall.wlock, fresh := false }],
      retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨0, true, some 1, [], [(1, true)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 1, call := RwCall.wlock } true),
      parked := [], runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.dispatchResumed (A := RwSig) (P := rwPrim)
      _ { fiber := 1, call := RwCall.wlock, fresh := false } [] rfl rfl rfl) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨0, true, some 1, [], [(1, true)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 1, call := RwCall.wlock } true),
      parked := [], runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨0, true, some 1, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [1], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.finishDone (A := RwSig) (P := rwPrim)
      _ { fiber := 1, call := RwCall.wlock } true [] [] RwRes.runit ⟨0, true, some 1, [], []⟩ []
      rfl rfl (by rfl) rfl Wakes.nil) ?_
  exact PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨0, true, some 1, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [1], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨0, true, some 1, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [1], nextFiber := 2,
      exts := [] }
    _ _ _ (PrimStep2.extDone (A := RwSig) (P := rwPrim)
      _ [] [] { x := 0, call := RwCall.runlock, result := some RwRes.runit } RwRes.runit rfl rfl)
    (PrimRuns2.stop _)

/-- Battery C — the reader batch: with one reader holding and two
readers queued, the release reaches zero and the grant pass readies
the whole (reader-only) queue; both resumes complete and both shares
are held. -/
theorem rw_battery_batch :
    ∃ fin : PrimCfg RwSig rwPrim,
      PrimRuns2 RwSig rwPrim
        { prim := ⟨1, false, none, [(1, RMode.rd), (2, RMode.rd)], []⟩,
          now := 0,
          cur := none,
          parked := [{ fiber := 1, call := RwCall.rlock }, { fiber := 2, call := RwCall.rlock }],
          runq := ([] : List (PReady RwSig)),
          retired := ([] : List FiberId),
          nextFiber := 3,
          exts := ([] : List (ExtPend RwSig)) }
        [issueObs RwSig (Caller.ext 0) RwCall.runlock,
          compObs RwSig (Caller.fiber 1) RwCall.rlock RwRes.runit,
          compObs RwSig (Caller.fiber 2) RwCall.rlock RwRes.runit,
          compObs RwSig (Caller.ext 0) RwCall.runlock RwRes.runit]
        fin ∧
      fin.prim.readers = 2 ∧ fin.prim.waitq = [] ∧ fin.prim.resolved = [] ∧
        fin.prim.writing = false := by
  refine ⟨{ prim := ⟨2, false, none, [], []⟩,
            now := 0, cur := none, parked := [], runq := [], retired := [2, 1],
            nextFiber := 3, exts := [] },
    ?_, rfl, rfl, rfl, rfl⟩
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [(1, RMode.rd), (2, RMode.rd)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.rlock }, { fiber := 2, call := RwCall.rlock }],
      runq := [], retired := [], nextFiber := 3, exts := [] }
    { prim := ⟨1, false, none, [(1, RMode.rd), (2, RMode.rd)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.rlock }, { fiber := 2, call := RwCall.rlock }],
      runq := [], retired := [], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := none }] }
    _ _ _ (PrimStep2.extApply (A := RwSig) (P := rwPrim)
      { prim := ⟨1, false, none, [(1, RMode.rd), (2, RMode.rd)], []⟩,
        now := 0, cur := none,
        parked := [{ fiber := 1, call := RwCall.rlock }, { fiber := 2, call := RwCall.rlock }],
        runq := [], retired := [], nextFiber := 3, exts := [] }
      0 RwCall.runlock [] [] (by rfl) (by simp)) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [(1, RMode.rd), (2, RMode.rd)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.rlock }, { fiber := 2, call := RwCall.rlock }],
      runq := [], retired := [], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := none }] }
    { prim := ⟨2, false, none, [], [(1, true), (2, true)]⟩,
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := RwCall.rlock, fresh := false },
        { fiber := 2, call := RwCall.rlock, fresh := false }],
      retired := [], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.extEffect (A := RwSig) (P := rwPrim)
      _ [] [] { x := 0, call := RwCall.runlock, result := none }
      [{ fiber := 1, call := RwCall.rlock }, { fiber := 2, call := RwCall.rlock }]
      [] RwRes.runit ⟨2, false, none, [], [(1, true), (2, true)]⟩ [1, 2]
      rfl rfl (by rfl) rfl (Wakes.drop (A := RwSig) { fiber := 1, call := RwCall.rlock }
        (Wakes.drop (A := RwSig) { fiber := 2, call := RwCall.rlock } Wakes.nil))) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨2, false, none, [], [(1, true), (2, true)]⟩,
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := RwCall.rlock, fresh := false },
        { fiber := 2, call := RwCall.rlock, fresh := false }],
      retired := [], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨2, false, none, [], [(1, true), (2, true)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 1, call := RwCall.rlock } true),
      parked := [], runq := [{ fiber := 2, call := RwCall.rlock, fresh := false }],
      retired := [], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.dispatchResumed (A := RwSig) (P := rwPrim)
      _ { fiber := 1, call := RwCall.rlock, fresh := false }
      [{ fiber := 2, call := RwCall.rlock, fresh := false }] rfl rfl rfl) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨2, false, none, [], [(1, true), (2, true)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 1, call := RwCall.rlock } true),
      parked := [], runq := [{ fiber := 2, call := RwCall.rlock, fresh := false }],
      retired := [], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨2, false, none, [], [(2, true)]⟩,
      now := 0, cur := none,
      parked := [], runq := [{ fiber := 2, call := RwCall.rlock, fresh := false }],
      retired := [1], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.finishDone (A := RwSig) (P := rwPrim)
      _ { fiber := 1, call := RwCall.rlock } true [] [] RwRes.runit ⟨2, false, none, [], [(2, true)]⟩ []
      rfl rfl (by rfl) rfl Wakes.nil) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨2, false, none, [], [(2, true)]⟩,
      now := 0, cur := none,
      parked := [], runq := [{ fiber := 2, call := RwCall.rlock, fresh := false }],
      retired := [1], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨2, false, none, [], [(2, true)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 2, call := RwCall.rlock } true),
      parked := [], runq := [], retired := [1], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.dispatchResumed (A := RwSig) (P := rwPrim)
      _ { fiber := 2, call := RwCall.rlock, fresh := false } [] rfl rfl rfl) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨2, false, none, [], [(2, true)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 2, call := RwCall.rlock } true),
      parked := [], runq := [], retired := [1], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨2, false, none, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [2, 1], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    _ _ _ (PrimStep2.finishDone (A := RwSig) (P := rwPrim)
      _ { fiber := 2, call := RwCall.rlock } true [] [] RwRes.runit ⟨2, false, none, [], []⟩ []
      rfl rfl (by rfl) rfl Wakes.nil) ?_
  exact PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨2, false, none, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [2, 1], nextFiber := 3,
      exts := [{ x := 0, call := RwCall.runlock, result := some RwRes.runit }] }
    { prim := ⟨2, false, none, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [2, 1], nextFiber := 3,
      exts := [] }
    _ _ _ (PrimStep2.extDone (A := RwSig) (P := rwPrim)
      _ [] [] { x := 0, call := RwCall.runlock, result := some RwRes.runit } RwRes.runit rfl rfl)
    (PrimRuns2.stop _)

/-- Battery D — the cancel: with one reader holding and another queued,
an external cancel removes the queued node, publishes its cancellation,
and readies it; the waiter resumes and completes. -/
theorem rw_battery_cancel :
    ∃ fin : PrimCfg RwSig rwPrim,
      PrimRuns2 RwSig rwPrim
        { prim := ⟨1, false, none, [(1, RMode.rd)], []⟩,
          now := 0,
          cur := none,
          parked := [{ fiber := 1, call := RwCall.rlock }],
          runq := ([] : List (PReady RwSig)),
          retired := ([] : List FiberId),
          nextFiber := 2,
          exts := ([] : List (ExtPend RwSig)) }
        [issueObs RwSig (Caller.ext 0) (RwCall.rcancel 1),
          compObs RwSig (Caller.fiber 1) RwCall.rlock RwRes.runit,
          compObs RwSig (Caller.ext 0) (RwCall.rcancel 1) (RwRes.rbool true)]
        fin ∧
      fin.prim.readers = 1 ∧ fin.prim.waitq = [] ∧ fin.prim.resolved = [] ∧
        fin.prim.writing = false ∧ fin.prim.wowner = none := by
  refine ⟨{ prim := ⟨1, false, none, [], []⟩,
            now := 0, cur := none, parked := [], runq := [], retired := [1],
            nextFiber := 2, exts := [] },
    ?_, rfl, rfl, rfl, rfl, rfl⟩
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [(1, RMode.rd)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.rlock }],
      runq := [], retired := [], nextFiber := 2, exts := [] }
    { prim := ⟨1, false, none, [(1, RMode.rd)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.rlock }],
      runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := none }] }
    _ _ _ (PrimStep2.extApply (A := RwSig) (P := rwPrim)
      { prim := ⟨1, false, none, [(1, RMode.rd)], []⟩,
        now := 0, cur := none,
        parked := [{ fiber := 1, call := RwCall.rlock }],
        runq := [], retired := [], nextFiber := 2, exts := [] }
      0 (RwCall.rcancel 1) [] [] (by rfl) (by simp)) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [(1, RMode.rd)], []⟩,
      now := 0, cur := none,
      parked := [{ fiber := 1, call := RwCall.rlock }],
      runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := none }] }
    { prim := ⟨1, false, none, [], [(1, false)]⟩,
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := RwCall.rlock, fresh := false }],
      retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := some (RwRes.rbool true) }] }
    _ _ _ (PrimStep2.extEffect (A := RwSig) (P := rwPrim)
      _ [] [] { x := 0, call := RwCall.rcancel 1, result := none } [{ fiber := 1, call := RwCall.rlock }]
      [] (RwRes.rbool true) ⟨1, false, none, [], [(1, false)]⟩ [1]
      rfl rfl (by rfl) rfl (Wakes.drop (A := RwSig) { fiber := 1, call := RwCall.rlock }
        Wakes.nil)) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [], [(1, false)]⟩,
      now := 0, cur := none, parked := [],
      runq := [{ fiber := 1, call := RwCall.rlock, fresh := false }],
      retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := some (RwRes.rbool true) }] }
    { prim := ⟨1, false, none, [], [(1, false)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 1, call := RwCall.rlock } true),
      parked := [], runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := some (RwRes.rbool true) }] }
    _ _ _ (PrimStep2.dispatchResumed (A := RwSig) (P := rwPrim)
      _ { fiber := 1, call := RwCall.rlock, fresh := false } [] rfl rfl rfl) ?_
  refine PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [], [(1, false)]⟩,
      now := 0, cur := some (FSlot.running { fiber := 1, call := RwCall.rlock } true),
      parked := [], runq := [], retired := [], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := some (RwRes.rbool true) }] }
    { prim := ⟨1, false, none, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [1], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := some (RwRes.rbool true) }] }
    _ _ _ (PrimStep2.finishDone (A := RwSig) (P := rwPrim)
      _ { fiber := 1, call := RwCall.rlock } true [] [] RwRes.runit ⟨1, false, none, [], []⟩ []
      rfl rfl (by rfl) rfl Wakes.nil) ?_
  exact PrimRuns2.step (A := RwSig) (P := rwPrim)
    { prim := ⟨1, false, none, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [1], nextFiber := 2,
      exts := [{ x := 0, call := RwCall.rcancel 1, result := some (RwRes.rbool true) }] }
    { prim := ⟨1, false, none, [], []⟩,
      now := 0, cur := none, parked := [], runq := [], retired := [1], nextFiber := 2,
      exts := [] }
    _ _ _ (PrimStep2.extDone (A := RwSig) (P := rwPrim)
      _ [] [] { x := 0, call := RwCall.rcancel 1, result := some (RwRes.rbool true) }
      (RwRes.rbool true) rfl rfl)
    (PrimRuns2.stop _)

/-! ## Mutant battery (independent fault classes)

Four single-facet mutants, each in a different fault class, each killed
by the evidence shape its fault class demands:

  * R-M1 `rwGrantHeadM1` — the writer claim ignores the reader count
    (the `active_readers > 0` guard of the writer branch is dropped).
    Killed by the exclusion discipline `rw_exclusion_holds`: every good
    endpoint satisfies `rwExclP`; the mutant's claim endpoint does not.
  * R-M2 `rwRunM2` — `unlock_read` skips the decrement (the release is
    unpaid).  Killed by refuting the release facet
    (`rwM2_unpaid` against `rwUnlockRead_pays`) and by the handoff
    separation (`rwM2_handoff_refuted` against battery B: with an
    unpaid release the count never reaches zero and the queued writer
    is never claimed).
  * R-M3 `rwGrantHeadM3` — the reader batch grants only the head.
    An under-production fault, so no safety invariant can catch it;
    killed by refuting the batch facet (`rwM3_undergrants` against
    `rwGrantHead_batch_drains`) and by the batch-witness separation
    against battery C (the TLA `MutBatchOne` certificate below).
  * R-M4 `rwAdmitM4` — `unlock_write`'s owner gate is dropped at
    admission.  Killed against the gate discipline
    `rwAdmit_wunlock_owner_gated` (`rwM4_nonowner_admitted`); the TLA
    mirror kills it on the writer-ownership invariant. -/

/-- R-M1 — the writer claim ignores the reader count. -/
def rwGrantHeadM1 (s : RwState) : Option (RwState × List FiberId) :=
  match s.waitq with
  | [] => none
  | (f, RMode.wr) :: t =>
      some ({ s with writing := true, wowner := some f,
                     waitq := t, resolved := s.resolved ++ [(f, true)] }, [f])
  | q@(_ :: _) =>
      if s.writing = false then
        some ({ s with readers := s.readers + (readerPrefix q).length,
                       waitq := dropReaders q,
                       resolved := s.resolved ++ (readerPrefix q).map (fun g => (g.1, true)) },
              (readerPrefix q).map (fun g => g.1))
      else none

theorem rwM1_exclusion_break :
    rwGrantHeadM1 ⟨1, false, none, [(1, RMode.wr)], []⟩ =
      some (⟨1, true, some 1, [], [(1, true)]⟩, [1]) ∧
    ¬ rwExclP ⟨1, true, some 1, [], [(1, true)]⟩ :=
  ⟨rfl, by simp [rwExclP]⟩

/-- R-M2 — `unlock_read` skips the decrement. -/
def rwRunM2 : RwState → Tick → FiberId → RwCall →
    Option (RwRes × RwState × List FiberId)
  | s, _, _, RwCall.runlock => some (RwRes.runit, s, [])
  | s, t, f, c => rwRun s t f c

theorem rwM2_unpaid :
    rwRunM2 ⟨2, false, none, [], []⟩ 0 0 RwCall.runlock =
      some (RwRes.runit, ⟨2, false, none, [], []⟩, []) := rfl

theorem rwM2_handoff_refuted :
    rwRunM2 ⟨1, false, none, [(1, RMode.wr)], []⟩ 0 0 RwCall.runlock =
      some (RwRes.runit, ⟨1, false, none, [(1, RMode.wr)], []⟩, []) := rfl

/-- R-M3 — the reader batch grants only the head. -/
def rwGrantHeadM3 (s : RwState) : Option (RwState × List FiberId) :=
  match s.waitq with
  | [] => none
  | (f, RMode.wr) :: t =>
      if s.readers = 0 ∧ s.writing = false then
        some ({ s with writing := true, wowner := some f,
                       waitq := t, resolved := s.resolved ++ [(f, true)] }, [f])
      else none
  | (f, RMode.rd) :: t =>
      if s.writing = false then
        some ({ s with readers := s.readers + 1,
                       waitq := t, resolved := s.resolved ++ [(f, true)] }, [f])
      else none

theorem rwM3_undergrants :
    rwGrantHeadM3 ⟨1, false, none, [(1, RMode.rd), (2, RMode.rd)], []⟩ =
      some (⟨2, false, none, [(2, RMode.rd)], [(1, true)]⟩, [1]) ∧
    rwGrantHead ⟨1, false, none, [(1, RMode.rd), (2, RMode.rd)], []⟩ =
      some (⟨3, false, none, [], [(1, true), (2, true)]⟩, [1, 2]) :=
  ⟨rfl, rfl⟩

/-- R-M4 — `unlock_write`'s owner gate is dropped at admission. -/
def rwAdmitM4 : RwState → FiberId → RwCall → Option RwState
  | s, _, RwCall.wunlock => some s
  | s, f, c => rwAdmit s f c

theorem rwM4_nonowner_admitted :
    rwAdmitM4 rwInit 1 RwCall.wunlock = some rwInit ∧
    rwAdmit rwInit 1 RwCall.wunlock = none :=
  ⟨rfl, rfl⟩
