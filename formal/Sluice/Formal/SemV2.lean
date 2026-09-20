/-
Sluice Semaphore core, Stage 2V2 (FCB1-METHOD-CORRECTIVE-1).

The counting semaphore (`include/sluice/async/semaphore.hpp`,
`src/async/scheduler_semaphore.cpp`).  The core surface is `acquire`
(void) and `release` (bool: whether a permit was granted).  `try_acquire`
is a non-suspending probe, and the deadline entry point (`acquire_until`)
and `cancel` are extensions outside the core model, exactly as Event's.

Call-domain census (v2.2 `extCap`/`extRun`), from the current code:

| entry           | code                            | worker read            | domain           | model                              |
| `acquire`       | scheduler_semaphore.cpp:34-71   | `g_worker` :36-37, `commit_suspend_locked` :61, `context_switch` :67-71 | fiber-bound | `extCap = false`, `extRun = none` |
| `release`       | scheduler_semaphore.cpp:147-161 | none (`global_mtx_` :149 only) | external-capable | `extCap = true`, `extRun = semRun` |
| `try_acquire`   | scheduler_semaphore.cpp:21-32   | none                   | external-capable | extension, outside the core surface |
| `cancel`        | scheduler_semaphore.cpp:134-145 | none                   | external-capable | extension, outside the core surface |
| `acquire_until` | scheduler_semaphore.cpp:73-132  | `g_worker` :75-76      | fiber-bound      | extension, outside the core surface |

The architecture question: `Scheduler::sem_release` is one atomic
critical section — hand a permit directly to the queue head, or store one
permit if the queue is empty and the ceiling allows, or refuse — all
under the scheduler lock, in the single inline step of the running call.
For fiber callers the modeled primitive fuses that section's issue,
effect, and completion into one step, so two fiber releases can never
interleave.  A fiber-wise encoding over the bare substrate would have to
serialize the check-then-act with a baton built from wait-queue cells and
spread the section over several steps; whether any such encoding stays
coherent is open here (RESEARCH/DEFER).  External callers are different
even for the primitive: their release separates issue from completion
(the window below).

The v2.2 window: an external `release` applies its grant at `extEffect`
but its completion observation lands only at `extDone`, and other steps
legitimately interleave in between (`global_mtx_` serialization is not
physical-return serialization).  An acquire may therefore complete while
the release that minted its permit is still in flight, so the strict
claim "completed takes < completed grants" is false for the primitive
itself (refuted by the possessed `windowTrace` below).  The honest trace
guarantee, for the modeled instance whose initial state holds zero
permits, is `completed takes ≤ issued releases`: every consumed permit
was minted by a release whose call had already been issued.  (With the
C++ constructor's `initial_permits > 0` the bound generalizes to
`completed takes ≤ issued releases + initial_permits`; the ceiling `max`
is immaterial to the claim.)  It is proved from a single permit-accounting
mirror that credits each release issue at its observation (fiber dispatch
or external entry) and discharges the credit when the release's section
takes effect.
-/
import Sluice.Formal.CalcV2
import Sluice.Formal.JudgeV2

namespace Sluice.Formal

/-! ## API and state -/

inductive SemCall : Type where
  | acquire
  | release
deriving instance DecidableEq for SemCall

inductive SemResult : Type where
  | acqDone
  | relRet (ok : Bool)
deriving instance DecidableEq for SemResult

/-- The counting-semaphore state: available permits, the ceiling, and the
parked acquirers in FIFO order. -/
structure SemState where
  available : Nat
  max : Nat
  waitq : List FiberId

deriving instance DecidableEq for SemState

abbrev SemSig : ApiSig := ⟨SemCall, SemResult⟩

/-! ## The primitive -/

/-- `sem_acquire`'s inline fast path (empty queue with a permit
available) and `sem_release`'s atomic section: hand off to the queue head,
or store one permit below the ceiling, or refuse. -/
def semRun (s : SemState) (c : SemCall) : Option (SemResult × SemState × List FiberId) :=
  match c with
  | SemCall.acquire =>
      if s.waitq = [] ∧ s.available > 0 then
        some (SemResult.acqDone, { s with available := s.available - 1 }, [])
      else none
  | SemCall.release =>
      match s.waitq with
      | [] =>
          if s.available < s.max then
            some (SemResult.relRet true, { s with available := s.available + 1 }, [])
          else some (SemResult.relRet false, s, [])
      | w :: rest => some (SemResult.relRet true, { s with waitq := rest }, [w])

/-- The counting semaphore parameterized by the C++ constructor's
`initial_permits` and `max_permits`. -/
def semPrimOf (initial max : Nat) : PrimLTS2 SemSig :=
  { State := SemState
    init := { available := initial, max := max, waitq := [] }
    admit := fun s _ _ => some s
    run := fun s _ _ c => semRun s c
    park := fun s f c =>
      match c with
      | SemCall.acquire => some ({ s with waitq := s.waitq ++ [f] }, [])
      | SemCall.release => none
    finish := fun s _ c =>
      match c with
      | SemCall.acquire => some (SemResult.acqDone, s, [])
      | SemCall.release => none
    extCap := fun c =>
      match c with
      | SemCall.acquire => false
      | SemCall.release => true
    extRun := fun c s _ =>
      match c with
      | SemCall.acquire => none
      | SemCall.release => semRun s SemCall.release
    onTick := fun s _ => s
    expire := fun _ _ _ => none }

/-- The modeled instance: zero initial permits, ceiling 1. -/
def semPrim : PrimLTS2 SemSig := semPrimOf 0 1

/-! ## Facet helpers -/

@[simp] theorem semPrimOf_admit (initial max : Nat) (s : SemState) (f : FiberId) (c : SemCall) :
    (semPrimOf initial max).admit s f c = some s := rfl

@[simp] theorem semPrimOf_run (initial max : Nat) (s : SemState) (t : Tick) (f : FiberId)
    (c : SemCall) : (semPrimOf initial max).run s t f c = semRun s c := rfl

@[simp] theorem semPrimOf_park_acquire (initial max : Nat) (s : SemState) (f : FiberId) :
    (semPrimOf initial max).park s f SemCall.acquire
      = some ({ s with waitq := s.waitq ++ [f] }, []) := rfl

@[simp] theorem semPrimOf_park_release (initial max : Nat) (s : SemState) (f : FiberId) :
    (semPrimOf initial max).park s f SemCall.release = none := rfl

@[simp] theorem semPrimOf_finish_acquire (initial max : Nat) (s : SemState) (f : FiberId) :
    (semPrimOf initial max).finish s f SemCall.acquire = some (SemResult.acqDone, s, []) := rfl

@[simp] theorem semPrimOf_finish_release (initial max : Nat) (s : SemState) (f : FiberId) :
    (semPrimOf initial max).finish s f SemCall.release = none := rfl

@[simp] theorem semPrimOf_extCap_acquire (initial max : Nat) :
    (semPrimOf initial max).extCap SemCall.acquire = false := rfl

@[simp] theorem semPrimOf_extCap_release (initial max : Nat) :
    (semPrimOf initial max).extCap SemCall.release = true := rfl

@[simp] theorem semPrimOf_extRun_acquire (initial max : Nat) (s : SemState) (t : Tick) :
    (semPrimOf initial max).extRun SemCall.acquire s t = none := rfl

@[simp] theorem semPrimOf_extRun_release (initial max : Nat) (s : SemState) (t : Tick) :
    (semPrimOf initial max).extRun SemCall.release s t = semRun s SemCall.release := rfl

@[simp] theorem semPrimOf_onTick (initial max : Nat) (s : SemState) (t : Tick) :
    (semPrimOf initial max).onTick s t = s := rfl

@[simp] theorem semPrimOf_expire (initial max : Nat) (s : SemState) (t : Tick) (f : FiberId) :
    (semPrimOf initial max).expire s t f = none := rfl

theorem semRun_none {s : SemState} {c : SemCall}
    (h : semRun s c = none) : c = SemCall.acquire := by
  cases c with
  | release =>
      cases hw : s.waitq with
      | nil =>
          by_cases hm : s.available < s.max
          · rw [semRun, hw, if_pos hm] at h; simp at h
          · rw [semRun, hw] at h
            rw [if_neg hm] at h
            simp at h
      | cons w rest => rw [semRun, hw] at h; simp at h
  | acquire => rfl

theorem semRun_pndMap_nil {ps : List (Pnd SemSig)}
    (h : ps.map (fun p : Pnd SemSig => p.fiber) = []) : ps = [] := by
  cases ps with
  | nil => rfl
  | cons p rest => simp at h

/-! ## Trace counts -/

/-- A completed acquire that consumed a permit. -/
def semIsTake (o : Obs SemSig) : Bool :=
  decide (o.call = SemCall.acquire ∧ o.result = some SemResult.acqDone)

/-- A release issue (fiber or external): the call's entry observation. -/
def semIsRelIssue (o : Obs SemSig) : Bool :=
  decide (o.call = SemCall.release ∧ o.result = none)

/-- A completed release that granted a permit (used only by the refuted
strict claim below). -/
def semIsGrant (o : Obs SemSig) : Bool :=
  decide (o.call = SemCall.release ∧ o.result = some (SemResult.relRet true))

def semAcqCount (t : Trace SemSig) : Nat := (t.filter semIsTake).length
def semRelIssueCount (t : Trace SemSig) : Nat := (t.filter semIsRelIssue).length
def semRelCount (t : Trace SemSig) : Nat := (t.filter semIsGrant).length

@[simp] theorem semAcqCount_nil : semAcqCount [] = 0 := rfl

@[simp] theorem semRelIssueCount_nil : semRelIssueCount [] = 0 := rfl

@[simp] theorem semAcqCount_issue (cl : Caller) (c : SemCall) (t : Trace SemSig) :
    semAcqCount (issueObs SemSig cl c :: t) = semAcqCount t := by
  simp [semAcqCount, semIsTake, issueObs]

@[simp] theorem semAcqCount_comp (cl : Caller) (c : SemCall) (r : SemResult)
    (t : Trace SemSig) :
    semAcqCount (compObs SemSig cl c r :: t) =
      semAcqCount t + (if c = SemCall.acquire ∧ r = SemResult.acqDone then 1 else 0) := by
  simp only [semAcqCount, List.filter_cons, List.length_cons]
  cases hc : c with
  | release => simp [hc, semIsTake, compObs]
  | acquire =>
      cases hr : r with
      | acqDone => simp [hc, hr, semIsTake, compObs]
      | relRet _ => simp [hc, hr, semIsTake, compObs]

@[simp] theorem semRelIssueCount_issue (cl : Caller) (c : SemCall) (t : Trace SemSig) :
    semRelIssueCount (issueObs SemSig cl c :: t) =
      semRelIssueCount t + (if c = SemCall.release then 1 else 0) := by
  simp only [semRelIssueCount, List.filter_cons, List.length_cons]
  cases hc : c <;> simp [hc, semIsRelIssue, issueObs]

@[simp] theorem semRelIssueCount_comp (cl : Caller) (c : SemCall) (r : SemResult)
    (t : Trace SemSig) :
    semRelIssueCount (compObs SemSig cl c r :: t) = semRelIssueCount t := by
  have hd : semIsRelIssue (compObs SemSig cl c r) = false := by
    simp [semIsRelIssue, compObs]
  simp [semRelIssueCount, hd]

/-! ## Held permits and in-flight release credits -/

/-- Acquire permits held by in-flight calls: a queue head handed a permit
by a release handoff, either still awaiting its dispatch (stale runq
entry) or already dispatched (resumed `running` slot); plus a returning
inline acquire whose physical return is pending (V2.3: the permit is
already consumed, the completion not yet observed).  Parked acquirers
hold nothing. -/
def semCurAq (d : Option (FSlot SemSig)) : Nat :=
  match d with
  | some (FSlot.running p true) => if p.call = SemCall.acquire then 1 else 0
  | some (FSlot.returning p rr) =>
      if p.call = SemCall.acquire ∧ rr = SemResult.acqDone then 1 else 0
  | _ => 0

def semStaleAq (Q : List (PReady SemSig)) : Nat :=
  (Q.filter fun r => decide (r.fresh = false ∧ r.call = SemCall.acquire)).length

variable {P : PrimLTS2 SemSig}

def semHeldAq (cfg : PrimCfg SemSig P) : Nat :=
  semCurAq cfg.cur + semStaleAq cfg.runq

@[simp] theorem semCurAq_none : semCurAq none = 0 := rfl

@[simp] theorem semCurAq_running_false (p : Pnd SemSig) :
    semCurAq (some (FSlot.running p false)) = 0 := rfl

@[simp] theorem semCurAq_running_true (p : Pnd SemSig) :
    semCurAq (some (FSlot.running p true)) = if p.call = SemCall.acquire then 1 else 0 := rfl

@[simp] theorem semCurAq_returning_acquire (p : Pnd SemSig) :
    semCurAq (some (FSlot.returning p SemResult.acqDone)) =
      if p.call = SemCall.acquire then 1 else 0 := by
    simp [semCurAq]

@[simp] theorem semCurAq_returning_release (p : Pnd SemSig) (b : Bool) :
    semCurAq (some (FSlot.returning p (SemResult.relRet b))) = 0 := by
    simp [semCurAq]

theorem semStaleAq_cons_head (r : PReady SemSig) (Q : List (PReady SemSig)) :
    semStaleAq (r :: Q) =
      semStaleAq Q + (if r.fresh = false ∧ r.call = SemCall.acquire then 1 else 0) := by
  simp only [semStaleAq, List.filter_cons, List.length_cons]
  cases hf : r.fresh with
  | false =>
      cases hc : r.call <;> simp [hf, hc, semStaleAq] <;> omega
  | true => simp [hf, semStaleAq]

theorem semStaleAq_append_fresh (Q : List (PReady SemSig)) (f : FiberId) (c : SemCall) :
    semStaleAq (Q ++ [{ fiber := f, call := c, fresh := true }]) = semStaleAq Q := by
  rw [semStaleAq, semStaleAq, List.filter_append]
  simp

theorem semStaleAq_append_stale (Q : List (PReady SemSig)) (f : FiberId) (c : SemCall) :
    semStaleAq (Q ++ [{ fiber := f, call := c, fresh := false }]) =
      semStaleAq Q + (if c = SemCall.acquire then 1 else 0) := by
  rw [semStaleAq, semStaleAq, List.filter_append, List.length_append]
  cases hc : c <;> simp [hc] <;> omega

/-- Fiber releases in flight between their dispatch and their section:
the credit for a release issue observed at a fiber dispatch.  The
`returning` slot carries no credit — the section (and with it the
discharge) has already run. -/
def semPendRel (cfg : PrimCfg SemSig P) : Nat :=
  match cfg.cur with
  | some (FSlot.running p _) => if p.call = SemCall.release then 1 else 0
  | _ => 0

/-- External release records entered but not yet effected (`result =
none`): the credit for a release issue observed at an external
entry. -/
def semPendExt (E : List (ExtPend SemSig)) : Nat :=
  (E.filter fun e : ExtPend SemSig =>
    decide (e.call = SemCall.release ∧ e.result = none)).length

theorem semPendExt_cons (E : List (ExtPend SemSig)) (e : ExtPend SemSig)
    (F : List (ExtPend SemSig)) :
    semPendExt (E ++ e :: F) =
      semPendExt (E ++ F) + (if e.call = SemCall.release ∧ e.result = none then 1 else 0) := by
  rw [semPendExt, semPendExt, List.filter_append, List.filter_append,
    List.length_append]
  cases hr : e.result with
  | none =>
      cases hc : e.call <;> simp [hr, hc] <;> omega
  | some _ => simp [hr]

/-! ## The permit-accounting mirror

Every run conserves permits against the release issues observed so far.
Fiber release issues are credited at their dispatch (`semPendRel`),
external ones at their entry (`semPendExt`); both credits are discharged
when the release's section takes effect (into `available` or into a held
permit) — a refused release simply discards its credit, which is why the
statement is an inequality in the safe direction.  The same direction
absorbs the calculus's re-suspending resumed acquire (`runPark` is
unguarded on the resumed bit), whose held permit leaves the accounted
pool.  The completing-acquire bound needs exactly this direction. -/

theorem semBalance_mirror {initial max : Nat}
    {cfg fin : PrimCfg SemSig (semPrimOf initial max)}
    {t : Trace SemSig}
    (hrun : PrimRuns2 SemSig (semPrimOf initial max) cfg t fin)
    (hstale : ∀ r ∈ cfg.runq, r.fresh = false → r.call = SemCall.acquire)
    (hparked : ∀ p ∈ cfg.parked, p.call = SemCall.acquire)
    (hexts : ∀ e ∈ cfg.exts, e.call = SemCall.release) :
    fin.prim.available + semHeldAq fin + semAcqCount t + semPendRel fin
      + semPendExt fin.exts
      ≤ semRelIssueCount t + semPendRel cfg + semPendExt cfg.exts
        + cfg.prim.available + semHeldAq cfg ∧
    (∀ r ∈ fin.runq, r.fresh = false → r.call = SemCall.acquire) ∧
    (∀ p ∈ fin.parked, p.call = SemCall.acquire) ∧
    (∀ e ∈ fin.exts, e.call = SemCall.release) := by
  induction hrun with
  | stop cfg =>
      refine ⟨by rw [semAcqCount_nil, semRelIssueCount_nil] <;> omega, hstale, hparked,
        hexts⟩
  | step cfg cfg' o t2 fin hstep hrest ih =>
      cases hstep with
      | submit f c hc =>
          have hstale' : ∀ r ∈ cfg.runq ++ [{ fiber := f, call := c, fresh := true }],
              r.fresh = false → r.call = SemCall.acquire := by
            intro r hr hfr
            rcases List.mem_append.mp hr with hm | hm
            · exact hstale r hm hfr
            · simp only [List.mem_singleton] at hm
              subst hm
              simp at hfr
          have hparked' := hparked
          have hexts' := hexts
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.nil_append, semHeldAq,
            semStaleAq_append_fresh, semPendRel, semCurAq] at hbal ⊢
          omega
      | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
          have hs' : s' = cfg.prim := by
            rw [show (semPrimOf initial max).admit cfg.prim rp.fiber rp.call
              = some cfg.prim from rfl] at hadmit
            injection hadmit with heq
            exact heq.symm
          have hstale' : ∀ r ∈ rest, r.fresh = false → r.call = SemCall.acquire :=
            fun r hr => hstale r (by rw [hrunqE]; exact List.Mem.tail _ hr)
          have hparked' := hparked
          have hexts' := hexts
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.cons_append, semHeldAq,
            semStaleAq_cons_head, semPendRel, semCurAq_none, semCurAq_running_false,
            hs', hrunqE, hfreshE, hcurE, reduceIte] at hbal ⊢
          cases hcall : rp.call <;> simp [hcall] at hbal ⊢ <;> omega
      | dispatchResumed r rest hcurE hrunqE hfreshE =>
          have hracq : r.call = SemCall.acquire :=
            hstale r (by rw [hrunqE]; exact List.Mem.head _) hfreshE
          have hstale' : ∀ x ∈ rest, x.fresh = false → x.call = SemCall.acquire :=
            fun x hx => hstale x (by rw [hrunqE]; exact List.Mem.tail _ hx)
          have hparked' := hparked
          have hexts' := hexts
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.nil_append, semHeldAq,
            semStaleAq_cons_head, semPendRel, semCurAq_none, semCurAq_running_true,
            hrunqE, hracq, hcurE, hfreshE, reduceIte] at hbal ⊢
          try simp at hbal ⊢
          omega
      | fiberEffect d b ps rest r s' wk hcurE hb hrunP hmap hwake =>
          rw [hb] at hcurE
          have hstale' : ∀ x ∈ cfg.runq ++ ps.map
              (fun p : Pnd SemSig => { fiber := p.fiber, call := p.call, fresh := false }),
              x.fresh = false → x.call = SemCall.acquire := by
            intro x hx hfr
            rcases List.mem_append.mp hx with hm | hm
            · exact hstale x hm hfr
            · rw [List.mem_map] at hm
              obtain ⟨p, hp, hx2⟩ := hm
              cases hx2
              exact hparked p (Wakes.mem_parked hwake p hp)
          have hparked' : ∀ p ∈ rest, p.call = SemCall.acquire := by
            intro p hp
            exact hparked p (Wakes.mem_rest hwake p hp)
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.cons_append, semHeldAq, semPendRel] at hbal ⊢
          cases hcall : d.call with
          | acquire =>
              rw [hcall] at hrunP
              have hrun2 : (if cfg.prim.waitq = [] ∧ cfg.prim.available > 0 then
                  some (SemResult.acqDone,
                    { cfg.prim with available := cfg.prim.available - 1 }, [])
                else none) = some (r, s', wk) := hrunP
              by_cases hcond : cfg.prim.waitq = [] ∧ cfg.prim.available > 0
              · rw [if_pos hcond] at hrun2
                injection hrun2 with h0
                injection h0 with hrq h1
                injection h1 with hs'eq hwkeq
                subst hrq
                subst hs'eq
                subst hwkeq
                have hps : ps = [] := by
                  cases ps with
                  | nil => rfl
                  | cons p ps' => simp at hmap
                subst hps
                simp only [Option.toList, List.cons_append, List.nil_append,
                  List.append_nil, List.map_nil, hcurE, hcall, reduceIte,
                  semCurAq_none,
                  semCurAq_running_false, semCurAq_returning_acquire,
                  semCurAq_returning_release] at hbal ⊢
                omega
              · rw [if_neg hcond] at hrun2
                simp at hrun2
          | release =>
              rw [hcall] at hrunP
              have hrun2 : (match cfg.prim.waitq with
                  | [] => if cfg.prim.available < cfg.prim.max then
                      some (SemResult.relRet true,
                        { cfg.prim with available := cfg.prim.available + 1 }, [])
                    else some (SemResult.relRet false, cfg.prim, [])
                  | w :: rest =>
                      some (SemResult.relRet true,
                        { cfg.prim with waitq := rest }, [w])) = some (r, s', wk) := hrunP
              cases hwq : cfg.prim.waitq with
              | nil =>
                  rw [hwq] at hrun2
                  have h3 : (if cfg.prim.available < cfg.prim.max then
                      some (SemResult.relRet true,
                        { cfg.prim with available := cfg.prim.available + 1 }, [])
                    else some (SemResult.relRet false, cfg.prim, []))
                      = some (r, s', wk) := hrun2
                  by_cases hm : cfg.prim.available < cfg.prim.max
                  · rw [if_pos hm] at h3
                    injection h3 with h0
                    injection h0 with hrret h1
                    injection h1 with hs'eq hwkeq
                    subst hrret
                    subst hs'eq
                    subst hwkeq
                    have hps : ps = [] := by
                      cases ps with
                      | nil => rfl
                      | cons p ps' => simp at hmap
                    subst hps
                    simp only [Option.toList, List.cons_append, List.nil_append,
                        List.append_nil, List.map_nil, hcurE, hcall, reduceIte,
                        semCurAq_none,
                        semCurAq_running_false, semCurAq_returning_acquire,
                        semCurAq_returning_release] at hbal ⊢
                    omega
                  · rw [if_neg hm] at h3
                    injection h3 with h0
                    injection h0 with hrret h1
                    injection h1 with hs'eq hwkeq
                    subst hrret
                    subst hs'eq
                    subst hwkeq
                    have hps : ps = [] := by
                      cases ps with
                      | nil => rfl
                      | cons p ps' => simp at hmap
                    subst hps
                    simp only [Option.toList, List.cons_append, List.nil_append,
                        List.append_nil, List.map_nil, hcurE, hcall, reduceIte,
                        semCurAq_none,
                        semCurAq_running_false, semCurAq_returning_acquire,
                        semCurAq_returning_release] at hbal ⊢
                    omega
              | cons w rest =>
                  rw [hwq] at hrun2
                  have h3 : some (SemResult.relRet true,
                      { cfg.prim with waitq := rest }, [w]) = some (r, s', wk) := hrun2
                  injection h3 with h0
                  injection h0 with hrret h1
                  injection h1 with hs'eq hwkeq
                  subst hrret
                  subst hs'eq
                  subst hwkeq
                  have hpsmap : ps.map (fun p : Pnd SemSig => p.fiber) = [w] := hmap
                  cases ps with
                  | nil => simp at hpsmap
                  | cons u ps' =>
                      cases ps' with
                      | nil =>
                          simp only [List.map_cons, List.map_nil] at hpsmap
                          simp only [Option.toList, List.nil_append, List.cons_append,
                            List.map_cons, List.map_nil, hcurE, hcall, reduceIte,
                            semCurAq_returning_release, semCurAq_none,
                            semCurAq_running_false] at hbal ⊢
                          cases hu : u.call <;>
                            simp only [hu, semStaleAq_append_stale, reduceIte] at hbal ⊢ <;>
                              try simp at hbal ⊢ <;>
                            omega
                      | cons v ps'' => simp at hpsmap
      | fiberDone d r hcurE =>
          have hstale' := hstale
          have hparked' := hparked
          have hexts' := hexts
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.cons_append, semHeldAq, semPendRel] at hbal ⊢
          simp only [hcurE] at hbal ⊢
          cases hcall : d.call <;> cases hr : r <;>
            simp only [Option.toList, List.nil_append, List.cons_append, hcall,
              hr, semCurAq_returning_acquire, semCurAq_returning_release,
              semAcqCount_comp, semRelIssueCount_comp, reduceIte] at hbal ⊢ <;>
            try simp at hbal ⊢ <;>
            omega
      | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
          have hc' : d.call = SemCall.acquire := semRun_none hrunE
          rw [hc'] at hparkE
          have hpe : (some ({ cfg.prim with waitq := cfg.prim.waitq ++ [d.fiber] },
              []) : Option (SemState × List FiberId)) = some (s', wk) := hparkE
          injection hpe with hs'eq
          obtain ⟨h4, hwk⟩ := Prod.mk.inj hs'eq
          subst hwk
          have hps : ps = [] := by
            cases ps with
            | nil => rfl
            | cons p ps' => simp at hmap
          subst hps
          subst h4
          have hparked' : ∀ p ∈ rest ++ [{ fiber := d.fiber, call := d.call }],
              p.call = SemCall.acquire := by
            intro p hp
            rcases List.mem_append.mp hp with hm | hm
            · exact hparked p (Wakes.mem_rest hwake p hm)
            · simp only [List.mem_singleton] at hm
              subst hm
              exact hc'
          have hstale' : ∀ x ∈ cfg.runq ++ ([] : List (PReady SemSig)),
              x.fresh = false → x.call = SemCall.acquire := by
            intro x hx hfr
            rcases List.mem_append.mp hx with hm | hm
            · exact hstale x hm hfr
            · simp at hm
          have hexts' := hexts
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.nil_append, List.map_nil, semHeldAq, semPendRel,
            semCurAq_none, semCurAq_running_true, hcurE, hc'] at hbal ⊢
          cases hb : b <;> simp [hb] at hbal ⊢ <;> omega
      | finishDone d b ps rest r s' wk hcurE hb hfinD hmap hwake =>
          rw [hb] at hcurE
          have hstale' : ∀ x ∈ cfg.runq ++ ps.map
              (fun p : Pnd SemSig => { fiber := p.fiber, call := p.call, fresh := false }),
              x.fresh = false → x.call = SemCall.acquire := by
            intro x hx hfr
            rcases List.mem_append.mp hx with hm | hm
            · exact hstale x hm hfr
            · rw [List.mem_map] at hm
              obtain ⟨p, hp, hx2⟩ := hm
              cases hx2
              exact hparked p (Wakes.mem_parked hwake p hp)
          have hparked' : ∀ p ∈ rest, p.call = SemCall.acquire := by
            intro p hp
            exact hparked p (Wakes.mem_rest hwake p hp)
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.cons_append, semHeldAq, semPendRel] at hbal ⊢
          cases hcall : d.call with
          | acquire =>
              rw [hcall] at hfinD
              have hfin2 : (semPrimOf initial max).finish cfg.prim d.fiber SemCall.acquire
                  = some (r, s', wk) := hfinD
              simp only [semPrimOf_finish_acquire initial max] at hfin2
              injection hfin2 with h0
              injection h0 with hrq h1
              injection h1 with hs'eq hwkeq
              subst hrq
              subst hs'eq
              subst hwkeq
              have hps : ps = [] := by
                cases ps with
                | nil => rfl
                | cons p ps' => simp at hmap
              subst hps
              simp only [List.append_nil, List.map_nil, semCurAq_running_true,
                hcurE, hcall, reduceIte] at hbal ⊢
              try simp at hbal ⊢
              omega
          | release =>
              rw [hcall] at hfinD
              have h4 : (none : Option (SemResult × SemState × List FiberId))
                  = some (r, s', wk) := hfinD
              simp at h4
      | extApply x c preE postE hcap hxfresh =>
          have hstale' := hstale
          have hparked' := hparked
          have hexts' : ∀ e ∈ cfg.exts ++ [{ x := x, call := c, result := none }],
              e.call = SemCall.release := by
            intro e he
            rcases List.mem_append.mp he with hm | hm
            · exact hexts e hm
            · simp only [List.mem_singleton] at hm
              subst hm
              cases hcc : c with
              | acquire =>
                  rw [hcc, semPrimOf_extCap_acquire initial max] at hcap
                  exact absurd hcap (by simp)
              | release => rfl
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.cons_append, semHeldAq, semPendRel,
            semCurAq, semPendExt_cons] at hbal ⊢
          cases hcall : c <;> simp [hcall] at hbal ⊢ <;> omega
      | extEffect preE postE e ps rest r s' wk hsplitE hnone hrunE hmap hwake =>
          have hec : e.call = SemCall.release := by
            cases hEc : e.call with
            | acquire =>
                rw [hEc] at hrunE
                have h4 : (none : Option (SemResult × SemState × List FiberId))
                    = some (r, s', wk) := hrunE
                simp at h4
            | release => rfl
          have hrun2 : (match cfg.prim.waitq with
              | [] => if cfg.prim.available < cfg.prim.max then
                  some (SemResult.relRet true,
                    { cfg.prim with available := cfg.prim.available + 1 }, [])
                else some (SemResult.relRet false, cfg.prim, [])
              | w :: rest =>
                  some (SemResult.relRet true,
                    { cfg.prim with waitq := rest }, [w])) = some (r, s', wk) := by
            rw [← hrunE, hec]
            rfl
          have hstale' : ∀ x ∈ cfg.runq ++ ps.map
              (fun p : Pnd SemSig => { fiber := p.fiber, call := p.call, fresh := false }),
              x.fresh = false → x.call = SemCall.acquire := by
            intro x hx hfr
            rcases List.mem_append.mp hx with hm | hm
            · exact hstale x hm hfr
            · rw [List.mem_map] at hm
              obtain ⟨p, hp, hx2⟩ := hm
              cases hx2
              exact hparked p (Wakes.mem_parked hwake p hp)
          have hparked' : ∀ p ∈ rest, p.call = SemCall.acquire := by
            intro p hp
            exact hparked p (Wakes.mem_rest hwake p hp)
          have hexts' : ∀ x ∈ preE ++ { e with result := some r } :: postE,
              x.call = SemCall.release := by
            intro x hx
            rw [List.mem_append] at hx
            rcases hx with hm | hm
            · have hx' : x ∈ cfg.exts := by
                rw [hsplitE]
                exact List.mem_append.mpr (Or.inl hm)
              exact hexts x hx'
            · simp only [List.mem_cons, List.mem_singleton] at hm
              rcases hm with hm | hm
              · subst hm
                exact hec
              · have hx' : x ∈ cfg.exts := by
                  rw [hsplitE]
                  exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ hm))
                exact hexts x hx'
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.nil_append, semHeldAq, semPendRel,
            semCurAq_none, semCurAq_running_false, semCurAq_running_true,
            semCurAq_returning_acquire, semCurAq_returning_release] at hbal ⊢
          rw [hsplitE] at ⊢
          rw [semPendExt_cons] at hbal ⊢
          rw [hnone] at ⊢
          simp only [hec, reduceIte] at hbal ⊢
          try simp at hbal ⊢
          cases hwq : cfg.prim.waitq with
          | nil =>
              rw [hwq] at hrun2
              have h3 : (if cfg.prim.available < cfg.prim.max then
                  some (SemResult.relRet true,
                    { cfg.prim with available := cfg.prim.available + 1 }, [])
                else some (SemResult.relRet false, cfg.prim, []))
                  = some (r, s', wk) := hrun2
              by_cases hm : cfg.prim.available < cfg.prim.max
              · rw [if_pos hm] at h3
                injection h3 with h0
                injection h0 with hrret h1
                injection h1 with hs'eq hwkeq
                subst hs'eq
                subst hwkeq
                have hps : ps = [] := by
                  cases ps with
                  | nil => rfl
                  | cons p ps' => simp at hmap
                subst hps
                simp only [List.append_nil, List.map_nil, reduceIte] at hbal ⊢
                omega
              · rw [if_neg hm] at h3
                injection h3 with h0
                injection h0 with hrret h1
                injection h1 with hs'eq hwkeq
                subst hs'eq
                subst hwkeq
                have hps : ps = [] := by
                  cases ps with
                  | nil => rfl
                  | cons p ps' => simp at hmap
                subst hps
                simp only [List.append_nil, List.map_nil, reduceIte] at hbal ⊢
                omega
          | cons w rest =>
              rw [hwq] at hrun2
              have h3 : some (SemResult.relRet true,
                  { cfg.prim with waitq := rest }, [w]) = some (r, s', wk) := hrun2
              injection h3 with h0
              injection h0 with hrret h1
              injection h1 with hs'eq hwkeq
              subst hs'eq
              subst hwkeq
              have hpsmap : ps.map (fun p : Pnd SemSig => p.fiber) = [w] := hmap
              cases ps with
              | nil => simp at hpsmap
              | cons u ps' =>
                  cases ps' with
                  | nil =>
                      simp only [List.map_cons, List.map_nil] at hpsmap
                      simp only [List.cons_append, List.map_cons, List.map_nil,
                        reduceIte] at hbal ⊢
                      cases hu : u.call <;>
                        simp only [hu, semStaleAq_append_stale, reduceIte] at hbal ⊢ <;>
                          try simp at hbal ⊢ <;>
                        omega
                  | cons v ps'' => simp at hpsmap
      | extDone preE postE e r hsplitE hsome =>
          have hec : e.call = SemCall.release :=
            hexts e (by
              rw [hsplitE]
              exact List.mem_append.mpr (Or.inr (List.Mem.head _)))
          have hstale' := hstale
          have hparked' := hparked
          have hexts' : ∀ x ∈ preE ++ postE, x.call = SemCall.release := by
            intro x hx
            have hx' : x ∈ cfg.exts := by
              rw [hsplitE]
              rcases List.mem_append.mp hx with h | h
              · exact List.mem_append.mpr (Or.inl h)
              · exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ h))
            exact hexts x hx'
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          rw [hsplitE] at ⊢
          rw [semPendExt_cons] at ⊢
          rw [hsome] at ⊢
          simp only [semHeldAq, semPendRel, semCurAq_none, semCurAq_running_false,
            semCurAq_running_true, semCurAq_returning_acquire,
            semCurAq_returning_release, hec, reduceIte] at hbal ⊢
          try simp at hbal ⊢
          omega
      | envTime tk hcurE hle =>
          have hstale' := hstale
          have hparked' := hparked
          have hexts' := hexts
          obtain ⟨hbal, hstaleF, hparkedF, hextsF⟩ := ih hstale' hparked' hexts'
          refine ⟨?_, hstaleF, hparkedF, hextsF⟩
          simp only [Option.toList, List.nil_append, semHeldAq, semPendRel,
            semCurAq] at hbal ⊢
          have h1 : ((semPrimOf initial max).onTick cfg.prim tk).available =
            cfg.prim.available := rfl
          omega
      | envExpire f' s' preP postP p hcurE hexp hparkedE hpf =>
          exfalso
          simp [semPrimOf] at hexp

/-! ## Run splitting -/

/-- Splitting a run at an arbitrary cut `pre ++ ob :: suf`. -/
theorem semRuns_split {initial max : Nat} {pc pc' : PrimCfg SemSig (semPrimOf initial max)}
    {t : Trace SemSig}
    (hrun : PrimRuns2 SemSig (semPrimOf initial max) pc t pc') :
    ∀ (pre : Trace SemSig) (ob : Obs SemSig) (suf : Trace SemSig),
      t = pre ++ ob :: suf →
      ∃ ma mb : PrimCfg SemSig (semPrimOf initial max),
        PrimRuns2 SemSig (semPrimOf initial max) pc pre ma ∧
        PrimStep2 SemSig (semPrimOf initial max) ma (some ob) mb ∧
        PrimRuns2 SemSig (semPrimOf initial max) mb suf pc' := by
  induction hrun with
  | stop cfg => intro pre ob suf hsplit; simp at hsplit
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro pre ob suf hsplit
      cases o with
      | none =>
          obtain ⟨ma, mb, h1, h2, h3⟩ :=
            ih pre ob suf (by simpa [Option.toList] using hsplit)
          exact ⟨ma, mb, PrimRuns2.step cfg cfg' none pre ma hstep h1, h2, h3⟩
      | some ob' =>
          have hto : Option.toList (some ob') ++ t2 = ob' :: t2 := rfl
          rw [hto] at hsplit
          cases pre with
          | nil =>
              have hEq : ob' :: t2 = ob :: suf := by simpa using hsplit
              injection hEq with h1 h2
              subst h1; subst h2
              exact ⟨cfg, cfg', PrimRuns2.stop cfg, hstep, hrest⟩
          | cons a tl =>
              have hEq : ob' :: t2 = a :: (tl ++ ob :: suf) := by
                simpa [List.cons_append] using hsplit
              injection hEq with h1a h1b
              subst h1a
              obtain ⟨ma, mb, h1, h2, h3⟩ := ih tl ob suf h1b
              exact ⟨ma, mb, PrimRuns2.step cfg cfg' (some ob') tl ma hstep h1, h2, h3⟩

/-! ## The completing-acquire credit -/

/-- A step that completes an acquire leaves at least one permit's worth
of accounting behind: a returning inline acquire still holds the permit it
consumed, a resumed take holds the permit a handoff delivered. -/
theorem semStep_take_credit {initial max : Nat}
    {ma mb : PrimCfg SemSig (semPrimOf initial max)} {ob : Obs SemSig}
    (hstep : PrimStep2 SemSig (semPrimOf initial max) ma (some ob) mb)
    (hc : ob.call = SemCall.acquire) (hr : ob.result = some SemResult.acqDone)
    (hexts : ∀ e ∈ ma.exts, e.call = SemCall.release) :
    1 ≤ ma.prim.available + semHeldAq ma := by
  cases hstep with
  | fiberDone d r hcurE =>
      have hr2 : r = SemResult.acqDone := Option.some.inj hr
      subst hr2
      have hc1 : d.call = SemCall.acquire := hc
      simp only [semHeldAq, hcurE, hc1, semCurAq_returning_acquire, if_pos]
      omega
  | finishDone d b ps rest r s' wk hcurE hb hfinD hmap hwake =>
      rw [hb] at hcurE
      have hc1 : d.call = SemCall.acquire := hc
      simp only [semHeldAq, hcurE, hc1, semCurAq_running_true, if_pos]
      omega
  | extDone preE postE e r hsplitE hsome =>
      have hc1 : e.call = SemCall.acquire := hc
      have hre : e.call = SemCall.release :=
        hexts e (by rw [hsplitE]; exact List.mem_append.mpr (Or.inr (List.Mem.head _)))
      rw [hc1] at hre
      exact absurd hre (by simp)
  | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
      exfalso
      have h4 : (none : Option SemResult) = some SemResult.acqDone := hr
      simp at h4
  | extApply x c preE postE hcap hxfresh =>
      exfalso
      have h4 : (none : Option SemResult) = some SemResult.acqDone := hr
      simp at h4
  all_goals
      exfalso
      rw [hc] at hr
      simp at hr

/-! ## The permit guarantee -/

/-- Trace form: at every completed acquire, the number of completed takes
is at most the number of release issues observed so far plus the initial
permit count — every consumed permit was minted by a release whose call
had already been issued, or drawn from the constructor's initial stock. -/
def semPermitsHonoredGen (initial : Nat) (t : Trace SemSig) : Prop :=
  ∀ (pre : Trace SemSig) (g : FiberId) (suf : Trace SemSig),
    t = pre ++ compObs SemSig (Caller.fiber g) SemCall.acquire SemResult.acqDone :: suf →
    semAcqCount pre ≤ semRelIssueCount pre + initial

/-- The modeled instance's guarantee: zero initial permits. -/
def semPermitsHonored (t : Trace SemSig) : Prop := semPermitsHonoredGen 0 t

theorem semPrimOf_guarantees (initial max : Nat) :
    Guarantees (semPrimOf initial max) (semPermitsHonoredGen initial) := by
  intro t ht pre g suf hsplit
  obtain ⟨⟨fin, hrun⟩, -⟩ := ht
  obtain ⟨ma, mb, h1, hstep, h2⟩ := semRuns_split hrun pre
    (compObs SemSig (Caller.fiber g) SemCall.acquire SemResult.acqDone) suf hsplit
  obtain ⟨hbal, -, -, hextsMA⟩ :=
    semBalance_mirror h1 (fun _ hx => by cases hx) (fun _ hx => by cases hx)
      (fun _ hx => by cases hx)
  have hcredit := semStep_take_credit hstep rfl rfl hextsMA
  have hpr : semPendRel (primInit SemSig (semPrimOf initial max)) = 0 := rfl
  have hpe : semPendExt (primInit SemSig (semPrimOf initial max)).exts = 0 := rfl
  have hh : semHeldAq (primInit SemSig (semPrimOf initial max)) = 0 := rfl
  have hpa : (primInit SemSig (semPrimOf initial max)).prim.available = initial := rfl
  simp only [hpr, hpe, hh, hpa] at hbal
  have hpend : 0 ≤ semPendRel ma + semPendExt ma.exts := Nat.zero_le _
  omega

/-! ## The strict completion-count claim is false in the effect/return window -/

/-- The stricter reading: every completed acquire is preceded by strictly
more completed grants than completed takes. -/
def semGrantsExceedTakes (t : Trace SemSig) : Prop :=
  ∀ (pre : Trace SemSig) (g : FiberId) (suf : Trace SemSig),
    t = pre ++ compObs SemSig (Caller.fiber g) SemCall.acquire SemResult.acqDone :: suf →
    semAcqCount pre < semRelCount pre

/-- The external-window witness: an external `release` admits a permit
(its critical section has run but it has not yet returned), a fiber
`acquire` takes it inline and returns, and only then does the external
release's completion land. -/
def windowTrace : Trace SemSig :=
  [issueObs SemSig (Caller.ext 0) SemCall.release,
   issueObs SemSig (Caller.fiber 0) SemCall.acquire,
   compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone,
   compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet true)]

def w0 : PrimCfg SemSig semPrim := primInit SemSig semPrim

def w1 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := SemCall.release, result := none }] }

def w2 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet true) }] }

def w3 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 0, call := SemCall.acquire, fresh := true }],
    retired := [], nextFiber := 1,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet true) }] }

def w4 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SemCall.acquire } false),
    parked := [], runq := [], retired := [], nextFiber := 1,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet true) }] }

/-- The acquire consumed the permit in its critical section (`fiberEffect`)
and now sits in the return window; the worker baton is still held, but
external callers may run whole calls across it. -/
def w4b : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SemCall.acquire } SemResult.acqDone),
    parked := [], runq := [], retired := [], nextFiber := 1,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet true) }] }

def w5 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet true) }] }

def w6 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem ws1 : PrimStep2 SemSig semPrim w0
    (some (issueObs SemSig (Caller.ext 0) SemCall.release)) w1 :=
  PrimStep2.extApply w0 0 SemCall.release [] [] rfl (by decide)

theorem ws2 : PrimStep2 SemSig semPrim w1 none w2 :=
  PrimStep2.extEffect w1 [] []
    { x := 0, call := SemCall.release, result := none } [] []
    (SemResult.relRet true) { available := 1, max := 1, waitq := [] } []
    rfl rfl (by rfl) rfl Wakes.nil

theorem ws3 : PrimStep2 SemSig semPrim w2 none w3 :=
  PrimStep2.submit w2 0 SemCall.acquire (Or.inl rfl)

theorem ws4 : PrimStep2 SemSig semPrim w3
    (some (issueObs SemSig (Caller.fiber 0) SemCall.acquire)) w4 :=
  PrimStep2.dispatchFresh w3 { fiber := 0, call := SemCall.acquire, fresh := true } []
    { available := 1, max := 1, waitq := [] } rfl rfl rfl rfl

theorem ws5a : PrimStep2 SemSig semPrim w4 none w4b :=
  PrimStep2.fiberEffect w4 { fiber := 0, call := SemCall.acquire } false [] []
    SemResult.acqDone { available := 0, max := 1, waitq := [] } [] rfl rfl (by rfl) rfl Wakes.nil

theorem ws5 : PrimStep2 SemSig semPrim w4b
    (some (compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone)) w5 :=
  PrimStep2.fiberDone w4b { fiber := 0, call := SemCall.acquire } SemResult.acqDone rfl

theorem ws6 : PrimStep2 SemSig semPrim w5
    (some (compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet true))) w6 :=
  PrimStep2.extDone w5 [] []
    { x := 0, call := SemCall.release, result := some (SemResult.relRet true) }
    (SemResult.relRet true) rfl rfl

theorem windowRun : PrimRuns2 SemSig semPrim w0 windowTrace w6 :=
  PrimRuns2.step w0 w1 _ _ w6 ws1
    (PrimRuns2.step w1 w2 none _ w6 ws2
      (PrimRuns2.step w2 w3 none _ w6 ws3
        (PrimRuns2.step w3 w4 _ _ w6 ws4
          (PrimRuns2.step w4 w4b none _ w6 ws5a
            (PrimRuns2.step w4b w5 _ _ w6 ws5
              (PrimRuns2.step w5 w6 _ [] w6 ws6 (PrimRuns2.stop w6)))))))

theorem seqOK_window : SeqOK SemSig windowTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem window_not_strict : ¬ semGrantsExceedTakes windowTrace := by
  intro h
  have h0 := h [issueObs SemSig (Caller.ext 0) SemCall.release,
      issueObs SemSig (Caller.fiber 0) SemCall.acquire] 0
      [compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet true)] rfl
  simp [semAcqCount, semIsTake, semRelCount, semIsGrant, issueObs] at h0

theorem semPrim_possesses_window :
    Possesses semPrim (fun t => t = windowTrace ∧ ¬ semGrantsExceedTakes t) :=
  ⟨windowTrace, ⟨⟨w6, windowRun⟩, seqOK_window⟩, rfl, window_not_strict⟩

/-! ## The fiber return window battery -/

/-- The fiber-window witness: a fiber `release` stores the last permit in
its critical section (`fiberEffect`), and an external caller's whole
`release` — entry, critical section, return — serializes inside the
fiber's return window: the external entry is refused (the fiber already
filled the semaphore), the external caller returns `false`, and only then
does the fiber physically return `true`.  A step relation that fuses a
fiber's critical section with its physical return cannot emit this trace. -/
def fiberWindowTrace : Trace SemSig :=
  [issueObs SemSig (Caller.fiber 0) SemCall.release,
   issueObs SemSig (Caller.ext 0) SemCall.release,
   compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet false),
   compObs SemSig (Caller.fiber 0) SemCall.release (SemResult.relRet true)]

def fw0 : PrimCfg SemSig semPrim := primInit SemSig semPrim

def fw1 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 0, call := SemCall.release, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def fw2 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SemCall.release } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def fw3 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SemCall.release } (SemResult.relRet true)),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def fw4 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SemCall.release } (SemResult.relRet true)),
    parked := [], runq := [], retired := [], nextFiber := 1,
    exts := [{ x := 0, call := SemCall.release, result := none }] }

def fw5 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SemCall.release } (SemResult.relRet true)),
    parked := [], runq := [], retired := [], nextFiber := 1,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet false) }] }

def fw6 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SemCall.release } (SemResult.relRet true)),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def fw7 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem fws1 : PrimStep2 SemSig semPrim fw0 none fw1 :=
  PrimStep2.submit fw0 0 SemCall.release (Or.inl rfl)

theorem fws2 : PrimStep2 SemSig semPrim fw1
    (some (issueObs SemSig (Caller.fiber 0) SemCall.release)) fw2 :=
  PrimStep2.dispatchFresh fw1 { fiber := 0, call := SemCall.release, fresh := true } []
    { available := 0, max := 1, waitq := [] } rfl rfl rfl rfl

theorem fws3 : PrimStep2 SemSig semPrim fw2 none fw3 :=
  PrimStep2.fiberEffect fw2 { fiber := 0, call := SemCall.release } false [] []
    (SemResult.relRet true) { available := 1, max := 1, waitq := [] } [] rfl rfl (by rfl) rfl Wakes.nil

theorem fws4 : PrimStep2 SemSig semPrim fw3
    (some (issueObs SemSig (Caller.ext 0) SemCall.release)) fw4 :=
  PrimStep2.extApply fw3 0 SemCall.release [] [] rfl (by decide)

theorem fws5 : PrimStep2 SemSig semPrim fw4 none fw5 :=
  PrimStep2.extEffect fw4 [] []
    { x := 0, call := SemCall.release, result := none } [] []
    (SemResult.relRet false) { available := 1, max := 1, waitq := [] } []
    rfl rfl (by rfl) rfl Wakes.nil

theorem fws6 : PrimStep2 SemSig semPrim fw5
    (some (compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet false))) fw6 :=
  PrimStep2.extDone fw5 [] []
    { x := 0, call := SemCall.release, result := some (SemResult.relRet false) }
    (SemResult.relRet false) rfl rfl

theorem fws7 : PrimStep2 SemSig semPrim fw6
    (some (compObs SemSig (Caller.fiber 0) SemCall.release (SemResult.relRet true))) fw7 :=
  PrimStep2.fiberDone fw6 { fiber := 0, call := SemCall.release } (SemResult.relRet true) rfl

theorem fiberWindowRun : PrimRuns2 SemSig semPrim fw0 fiberWindowTrace fw7 :=
  PrimRuns2.step fw0 fw1 none _ fw7 fws1
    (PrimRuns2.step fw1 fw2 _ _ fw7 fws2
      (PrimRuns2.step fw2 fw3 none _ fw7 fws3
        (PrimRuns2.step fw3 fw4 _ _ fw7 fws4
          (PrimRuns2.step fw4 fw5 none _ fw7 fws5
            (PrimRuns2.step fw5 fw6 _ _ fw7 fws6
              (PrimRuns2.step fw6 fw7 _ [] fw7 fws7 (PrimRuns2.stop fw7)))))))

theorem seqOK_fiberWindow : SeqOK SemSig fiberWindowTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- The external caller returns strictly before the fiber whose critical
section preceded it — the ordering the fused step relation could not
produce. -/
theorem fiberWindow_ext_returns_first :
    fiberWindowTrace = [issueObs SemSig (Caller.fiber 0) SemCall.release,
      issueObs SemSig (Caller.ext 0) SemCall.release] ++
      [compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet false),
       compObs SemSig (Caller.fiber 0) SemCall.release (SemResult.relRet true)] := rfl

theorem semPrim_possesses_fiberWindow : TracesPrim SemSig semPrim fiberWindowTrace :=
  ⟨fw7, fiberWindowRun⟩

/-! ## Possession batteries -/

/-- The external-release witness: the three-phase entry, critical
section, and return of `release` on an empty queue with ceiling room. -/
def extRelTrace : Trace SemSig :=
  [issueObs SemSig (Caller.ext 0) SemCall.release,
   compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet true)]

def er0 : PrimCfg SemSig semPrim := primInit SemSig semPrim

def er1 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := SemCall.release, result := none }] }

def er2 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet true) }] }

def er3 : PrimCfg SemSig semPrim :=
  { prim := { available := 1, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0, exts := [] }

theorem ers1 : PrimStep2 SemSig semPrim er0
    (some (issueObs SemSig (Caller.ext 0) SemCall.release)) er1 :=
  PrimStep2.extApply er0 0 SemCall.release [] [] rfl (by decide)

theorem ers2 : PrimStep2 SemSig semPrim er1 none er2 :=
  PrimStep2.extEffect er1 [] []
    { x := 0, call := SemCall.release, result := none } [] []
    (SemResult.relRet true) { available := 1, max := 1, waitq := [] } []
    rfl rfl (by rfl) rfl Wakes.nil

theorem ers3 : PrimStep2 SemSig semPrim er2
    (some (compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet true))) er3 :=
  PrimStep2.extDone er2 [] []
    { x := 0, call := SemCall.release, result := some (SemResult.relRet true) }
    (SemResult.relRet true) rfl rfl

theorem extRelRun : PrimRuns2 SemSig semPrim er0 extRelTrace er3 :=
  PrimRuns2.step er0 er1 _ _ er3 ers1
    (PrimRuns2.step er1 er2 none _ er3 ers2
      (PrimRuns2.step er2 er3 _ [] er3 ers3 (PrimRuns2.stop er3)))

theorem seqOK_extRel : SeqOK SemSig extRelTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem semPrim_possesses_extRel : TracesPrim SemSig semPrim extRelTrace :=
  ⟨er3, extRelRun⟩

/-- The fiber handoff witness: an acquire parks, a release hands the
permit to the queue head, and the parked acquire completes. -/
def handoffTrace : Trace SemSig :=
  [issueObs SemSig (Caller.fiber 0) SemCall.acquire,
   issueObs SemSig (Caller.fiber 1) SemCall.release,
   compObs SemSig (Caller.fiber 1) SemCall.release (SemResult.relRet true),
   compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone]

def h0 : PrimCfg SemSig semPrim := primInit SemSig semPrim

def h1 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 0, call := SemCall.acquire, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def h2 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SemCall.acquire } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def h3 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [0] }, now := 0, cur := none,
    parked := [{ fiber := 0, call := SemCall.acquire }], runq := [], retired := [],
    nextFiber := 1, exts := [] }

def h4 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [0] }, now := 0, cur := none,
    parked := [{ fiber := 0, call := SemCall.acquire }],
    runq := [{ fiber := 1, call := SemCall.release, fresh := true }], retired := [],
    nextFiber := 2, exts := [] }

def h5 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [0] }, now := 0,
    cur := some (FSlot.running { fiber := 1, call := SemCall.release } false),
    parked := [{ fiber := 0, call := SemCall.acquire }], runq := [], retired := [],
    nextFiber := 2, exts := [] }

/-- The release handed its permit to the queue head: the parked acquire
left the wait queue for a stale runnable entry, and the release sits in
its return window. -/
def h5b : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 1, call := SemCall.release } (SemResult.relRet true)),
    parked := [], runq := [{ fiber := 0, call := SemCall.acquire, fresh := false }],
    retired := [], nextFiber := 2, exts := [] }

def h6 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 0, call := SemCall.acquire, fresh := false }],
    retired := [1], nextFiber := 2, exts := [] }

def h7 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SemCall.acquire } true),
    parked := [], runq := [], retired := [1], nextFiber := 2, exts := [] }

def h8 : PrimCfg SemSig semPrim :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0, 1], nextFiber := 2, exts := [] }

theorem hs1 : PrimStep2 SemSig semPrim h0 none h1 :=
  PrimStep2.submit h0 0 SemCall.acquire (Or.inl rfl)

theorem hs2 : PrimStep2 SemSig semPrim h1
    (some (issueObs SemSig (Caller.fiber 0) SemCall.acquire)) h2 :=
  PrimStep2.dispatchFresh h1 { fiber := 0, call := SemCall.acquire, fresh := true } []
    { available := 0, max := 1, waitq := [] } rfl rfl rfl rfl

theorem hs3 : PrimStep2 SemSig semPrim h2 none h3 :=
  PrimStep2.runPark h2 { fiber := 0, call := SemCall.acquire } false [] []
    { available := 0, max := 1, waitq := [0] } [] rfl (by rfl) rfl rfl Wakes.nil

theorem hs4 : PrimStep2 SemSig semPrim h3 none h4 :=
  PrimStep2.submit h3 1 SemCall.release (Or.inl rfl)

theorem hs5 : PrimStep2 SemSig semPrim h4
    (some (issueObs SemSig (Caller.fiber 1) SemCall.release)) h5 :=
  PrimStep2.dispatchFresh h4 { fiber := 1, call := SemCall.release, fresh := true } []
    { available := 0, max := 1, waitq := [0] } rfl rfl rfl rfl

theorem hs6a : PrimStep2 SemSig semPrim h5 none h5b :=
  PrimStep2.fiberEffect h5 { fiber := 1, call := SemCall.release } false
    [{ fiber := 0, call := SemCall.acquire }] [] (SemResult.relRet true)
    { available := 0, max := 1, waitq := [] } [0] rfl rfl (by rfl) rfl
    (Wakes.drop _ Wakes.nil)

theorem hs6 : PrimStep2 SemSig semPrim h5b
    (some (compObs SemSig (Caller.fiber 1) SemCall.release (SemResult.relRet true))) h6 :=
  PrimStep2.fiberDone h5b { fiber := 1, call := SemCall.release } (SemResult.relRet true) rfl

theorem hs7 : PrimStep2 SemSig semPrim h6 none h7 :=
  PrimStep2.dispatchResumed h6 { fiber := 0, call := SemCall.acquire, fresh := false } []
    rfl rfl rfl

theorem hs8 : PrimStep2 SemSig semPrim h7
    (some (compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone)) h8 :=
  PrimStep2.finishDone h7 { fiber := 0, call := SemCall.acquire } true [] []
    SemResult.acqDone { available := 0, max := 1, waitq := [] } [] rfl rfl rfl rfl Wakes.nil

theorem handoffRun : PrimRuns2 SemSig semPrim h0 handoffTrace h8 :=
  PrimRuns2.step h0 h1 none _ h8 hs1
    (PrimRuns2.step h1 h2 _ _ h8 hs2
      (PrimRuns2.step h2 h3 none _ h8 hs3
        (PrimRuns2.step h3 h4 none _ h8 hs4
          (PrimRuns2.step h4 h5 _ _ h8 hs5
            (PrimRuns2.step h5 h5b none _ h8 hs6a
              (PrimRuns2.step h5b h6 _ _ h8 hs6
                (PrimRuns2.step h6 h7 none _ h8 hs7
                  (PrimRuns2.step h7 h8 _ [] h8 hs8 (PrimRuns2.stop h8)))))))))

theorem seqOK_handoff : SeqOK SemSig handoffTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem semPrim_possesses_handoff : TracesPrim SemSig semPrim handoffTrace :=
  ⟨h8, handoffRun⟩

/-- The handoff trace is tight for the guarantee: one release issue, one
acquire completion. -/
theorem handoff_tight :
    semAcqCount handoffTrace = 1 ∧ semRelIssueCount handoffTrace = 1 := by
  simp [handoffTrace, semAcqCount, semRelIssueCount, semIsTake, semIsRelIssue,
    issueObs, compObs]

/-! ## Parameterized-instance batteries -/

/-- The initial-stock witness (`initial = 1, max = 2`): a fiber acquire
consumes a constructor permit inline with no release anywhere. -/
def initialTakeTrace : Trace SemSig :=
  [issueObs SemSig (Caller.fiber 0) SemCall.acquire,
   compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone]

def iw0 : PrimCfg SemSig (semPrimOf 1 2) := primInit SemSig (semPrimOf 1 2)

def iw1 : PrimCfg SemSig (semPrimOf 1 2) :=
  { prim := { available := 1, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 0, call := SemCall.acquire, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def iw2 : PrimCfg SemSig (semPrimOf 1 2) :=
  { prim := { available := 1, max := 2, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SemCall.acquire } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def iw2b : PrimCfg SemSig (semPrimOf 1 2) :=
  { prim := { available := 0, max := 2, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SemCall.acquire } SemResult.acqDone),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def iw3 : PrimCfg SemSig (semPrimOf 1 2) :=
  { prim := { available := 0, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem iws1 : PrimStep2 SemSig (semPrimOf 1 2) iw0 none iw1 :=
  PrimStep2.submit iw0 0 SemCall.acquire (Or.inl rfl)

theorem iws2 : PrimStep2 SemSig (semPrimOf 1 2) iw1
    (some (issueObs SemSig (Caller.fiber 0) SemCall.acquire)) iw2 :=
  PrimStep2.dispatchFresh iw1 { fiber := 0, call := SemCall.acquire, fresh := true } []
    { available := 1, max := 2, waitq := [] } rfl rfl rfl rfl

theorem iws3 : PrimStep2 SemSig (semPrimOf 1 2) iw2 none iw2b :=
  PrimStep2.fiberEffect iw2 { fiber := 0, call := SemCall.acquire } false [] []
    SemResult.acqDone { available := 0, max := 2, waitq := [] } [] rfl rfl (by rfl) rfl Wakes.nil

theorem iws4 : PrimStep2 SemSig (semPrimOf 1 2) iw2b
    (some (compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone)) iw3 :=
  PrimStep2.fiberDone iw2b { fiber := 0, call := SemCall.acquire } SemResult.acqDone rfl

theorem initialTakeRun : PrimRuns2 SemSig (semPrimOf 1 2) iw0 initialTakeTrace iw3 :=
  PrimRuns2.step iw0 iw1 none _ iw3 iws1
    (PrimRuns2.step iw1 iw2 _ _ iw3 iws2
      (PrimRuns2.step iw2 iw2b none _ iw3 iws3
        (PrimRuns2.step iw2b iw3 _ [] iw3 iws4 (PrimRuns2.stop iw3))))

theorem seqOK_initialTake : SeqOK SemSig initialTakeTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem semPrimOf_possesses_initialTake :
    TracesPrim SemSig (semPrimOf 1 2) initialTakeTrace :=
  ⟨iw3, initialTakeRun⟩

/-- The take draws only on the constructor stock: one take, zero release
issues — exactly the slack `semPermitsHonoredGen 1` allows. -/
theorem initialTake_tight :
    semAcqCount initialTakeTrace = 1 ∧ semRelIssueCount initialTakeTrace = 0 := by
  simp [initialTakeTrace, semAcqCount, semRelIssueCount, semIsTake, semIsRelIssue,
    issueObs, compObs]

/-- The ceiling witness (`initial = 0, max = 2`): two external releases
store both permits, the third is refused at the ceiling. -/
def ceilingTrace : Trace SemSig :=
  [issueObs SemSig (Caller.ext 0) SemCall.release,
   compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet true),
   issueObs SemSig (Caller.ext 1) SemCall.release,
   compObs SemSig (Caller.ext 1) SemCall.release (SemResult.relRet true),
   issueObs SemSig (Caller.ext 2) SemCall.release,
   compObs SemSig (Caller.ext 2) SemCall.release (SemResult.relRet false)]

def mw0 : PrimCfg SemSig (semPrimOf 0 2) := primInit SemSig (semPrimOf 0 2)

def mw1 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 0, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := SemCall.release, result := none }] }

def mw2 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 1, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 0, call := SemCall.release, result := some (SemResult.relRet true) }] }

def mw3 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 1, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0, exts := [] }

def mw4 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 1, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 1, call := SemCall.release, result := none }] }

def mw5 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 2, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 1, call := SemCall.release, result := some (SemResult.relRet true) }] }

def mw6 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 2, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0, exts := [] }

def mw7 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 2, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 2, call := SemCall.release, result := none }] }

def mw8 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 2, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0,
    exts := [{ x := 2, call := SemCall.release, result := some (SemResult.relRet false) }] }

def mw9 : PrimCfg SemSig (semPrimOf 0 2) :=
  { prim := { available := 2, max := 2, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [], nextFiber := 0, exts := [] }

theorem mws1 : PrimStep2 SemSig (semPrimOf 0 2) mw0
    (some (issueObs SemSig (Caller.ext 0) SemCall.release)) mw1 :=
  PrimStep2.extApply mw0 0 SemCall.release [] [] rfl (by decide)

theorem mws2 : PrimStep2 SemSig (semPrimOf 0 2) mw1 none mw2 :=
  PrimStep2.extEffect mw1 [] []
    { x := 0, call := SemCall.release, result := none } [] []
    (SemResult.relRet true) { available := 1, max := 2, waitq := [] } []
    rfl rfl (by rfl) rfl Wakes.nil

theorem mws3 : PrimStep2 SemSig (semPrimOf 0 2) mw2
    (some (compObs SemSig (Caller.ext 0) SemCall.release (SemResult.relRet true))) mw3 :=
  PrimStep2.extDone mw2 [] []
    { x := 0, call := SemCall.release, result := some (SemResult.relRet true) }
    (SemResult.relRet true) rfl rfl

theorem mws4 : PrimStep2 SemSig (semPrimOf 0 2) mw3
    (some (issueObs SemSig (Caller.ext 1) SemCall.release)) mw4 :=
  PrimStep2.extApply mw3 1 SemCall.release [] [] rfl (by decide)

theorem mws5 : PrimStep2 SemSig (semPrimOf 0 2) mw4 none mw5 :=
  PrimStep2.extEffect mw4 [] []
    { x := 1, call := SemCall.release, result := none } [] []
    (SemResult.relRet true) { available := 2, max := 2, waitq := [] } []
    rfl rfl (by rfl) rfl Wakes.nil

theorem mws6 : PrimStep2 SemSig (semPrimOf 0 2) mw5
    (some (compObs SemSig (Caller.ext 1) SemCall.release (SemResult.relRet true))) mw6 :=
  PrimStep2.extDone mw5 [] []
    { x := 1, call := SemCall.release, result := some (SemResult.relRet true) }
    (SemResult.relRet true) rfl rfl

theorem mws7 : PrimStep2 SemSig (semPrimOf 0 2) mw6
    (some (issueObs SemSig (Caller.ext 2) SemCall.release)) mw7 :=
  PrimStep2.extApply mw6 2 SemCall.release [] [] rfl (by decide)

theorem mws8 : PrimStep2 SemSig (semPrimOf 0 2) mw7 none mw8 :=
  PrimStep2.extEffect mw7 [] []
    { x := 2, call := SemCall.release, result := none } [] []
    (SemResult.relRet false) { available := 2, max := 2, waitq := [] } []
    rfl rfl (by rfl) rfl Wakes.nil

theorem mws9 : PrimStep2 SemSig (semPrimOf 0 2) mw8
    (some (compObs SemSig (Caller.ext 2) SemCall.release (SemResult.relRet false))) mw9 :=
  PrimStep2.extDone mw8 [] []
    { x := 2, call := SemCall.release, result := some (SemResult.relRet false) }
    (SemResult.relRet false) rfl rfl

theorem ceilingRun : PrimRuns2 SemSig (semPrimOf 0 2) mw0 ceilingTrace mw9 :=
  PrimRuns2.step mw0 mw1 _ _ mw9 mws1
    (PrimRuns2.step mw1 mw2 none _ mw9 mws2
      (PrimRuns2.step mw2 mw3 _ _ mw9 mws3
        (PrimRuns2.step mw3 mw4 _ _ mw9 mws4
          (PrimRuns2.step mw4 mw5 none _ mw9 mws5
            (PrimRuns2.step mw5 mw6 _ _ mw9 mws6
              (PrimRuns2.step mw6 mw7 _ _ mw9 mws7
                (PrimRuns2.step mw7 mw8 none _ mw9 mws8
                  (PrimRuns2.step mw8 mw9 _ [] mw9 mws9 (PrimRuns2.stop mw9)))))))))

theorem seqOK_ceiling : SeqOK SemSig ceilingTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem semPrimOf_possesses_ceiling :
    TracesPrim SemSig (semPrimOf 0 2) ceilingTrace :=
  ⟨mw9, ceilingRun⟩

/-- Two stored grants, one ceiling refusal: three release issues, two
granted permits. -/
theorem ceiling_tight :
    semRelIssueCount ceilingTrace = 3 ∧ semRelCount ceilingTrace = 2 := by
  simp [ceilingTrace, semRelIssueCount, semRelCount, semIsRelIssue, semIsGrant,
    issueObs, compObs]

/-! ## The guarantee has teeth -/

/-- A mutant that lets `acquire` succeed without a permit. -/
def semMutantRun (s : SemState) (c : SemCall) :
    Option (SemResult × SemState × List FiberId) :=
  match c with
  | SemCall.acquire => some (SemResult.acqDone, s, [])
  | SemCall.release => semRun s SemCall.release

def semMutant : PrimLTS2 SemSig := { semPrim with run := fun s _ _ c => semMutantRun s c }

def semMutantTrace : Trace SemSig :=
  [issueObs SemSig (Caller.fiber 0) SemCall.acquire,
   compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone,
   issueObs SemSig (Caller.fiber 1) SemCall.acquire,
   compObs SemSig (Caller.fiber 1) SemCall.acquire SemResult.acqDone]

def sm0 : PrimCfg SemSig semMutant := primInit SemSig semMutant

def sm1 : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 0, call := SemCall.acquire, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def sm2 : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 0, call := SemCall.acquire } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def sm2b : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := SemCall.acquire } SemResult.acqDone),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def sm3 : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

def sm4 : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 1, call := SemCall.acquire, fresh := true }],
    retired := [0], nextFiber := 2, exts := [] }

def sm5 : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.running { fiber := 1, call := SemCall.acquire } false),
    parked := [], runq := [], retired := [0], nextFiber := 2, exts := [] }

def sm5b : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 1, call := SemCall.acquire } SemResult.acqDone),
    parked := [], runq := [], retired := [0], nextFiber := 2, exts := [] }

def sm6 : PrimCfg SemSig semMutant :=
  { prim := { available := 0, max := 1, waitq := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [1, 0], nextFiber := 2, exts := [] }

theorem sms1 : PrimStep2 SemSig semMutant sm0 none sm1 :=
  PrimStep2.submit sm0 0 SemCall.acquire (Or.inl rfl)

theorem sms2 : PrimStep2 SemSig semMutant sm1
    (some (issueObs SemSig (Caller.fiber 0) SemCall.acquire)) sm2 :=
  PrimStep2.dispatchFresh sm1 { fiber := 0, call := SemCall.acquire, fresh := true } []
    { available := 0, max := 1, waitq := [] } rfl rfl rfl rfl

theorem sms3a : PrimStep2 SemSig semMutant sm2 none sm2b :=
  PrimStep2.fiberEffect sm2 { fiber := 0, call := SemCall.acquire } false [] []
    SemResult.acqDone { available := 0, max := 1, waitq := [] } [] rfl rfl (by rfl) rfl Wakes.nil

theorem sms3 : PrimStep2 SemSig semMutant sm2b
    (some (compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone)) sm3 :=
  PrimStep2.fiberDone sm2b { fiber := 0, call := SemCall.acquire } SemResult.acqDone rfl

theorem sms4 : PrimStep2 SemSig semMutant sm3 none sm4 :=
  PrimStep2.submit sm3 1 SemCall.acquire (Or.inl rfl)

theorem sms5 : PrimStep2 SemSig semMutant sm4
    (some (issueObs SemSig (Caller.fiber 1) SemCall.acquire)) sm5 :=
  PrimStep2.dispatchFresh sm4 { fiber := 1, call := SemCall.acquire, fresh := true } []
    { available := 0, max := 1, waitq := [] } rfl rfl rfl rfl

theorem sms6a : PrimStep2 SemSig semMutant sm5 none sm5b :=
  PrimStep2.fiberEffect sm5 { fiber := 1, call := SemCall.acquire } false [] []
    SemResult.acqDone { available := 0, max := 1, waitq := [] } [] rfl rfl (by rfl) rfl Wakes.nil

theorem sms6 : PrimStep2 SemSig semMutant sm5b
    (some (compObs SemSig (Caller.fiber 1) SemCall.acquire SemResult.acqDone)) sm6 :=
  PrimStep2.fiberDone sm5b { fiber := 1, call := SemCall.acquire } SemResult.acqDone rfl

theorem mutantRun : PrimRuns2 SemSig semMutant sm0 semMutantTrace sm6 :=
  PrimRuns2.step sm0 sm1 none _ sm6 sms1
    (PrimRuns2.step sm1 sm2 _ _ sm6 sms2
      (PrimRuns2.step sm2 sm2b none _ sm6 sms3a
        (PrimRuns2.step sm2b sm3 _ _ sm6 sms3
          (PrimRuns2.step sm3 sm4 none _ sm6 sms4
            (PrimRuns2.step sm4 sm5 _ _ sm6 sms5
              (PrimRuns2.step sm5 sm5b none _ sm6 sms6a
                (PrimRuns2.step sm5b sm6 _ [] sm6 sms6 (PrimRuns2.stop sm6))))))))

theorem seqOK_mutant : SeqOK SemSig semMutantTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem semMutant_not_guarantees : ¬ Guarantees semMutant semPermitsHonored := by
  intro hguar
  have h := hguar semMutantTrace ⟨⟨sm6, mutantRun⟩, seqOK_mutant⟩
  have hmf : semMutantTrace = [issueObs SemSig (Caller.fiber 0) SemCall.acquire,
      compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone,
      issueObs SemSig (Caller.fiber 1) SemCall.acquire] ++
    [compObs SemSig (Caller.fiber 1) SemCall.acquire SemResult.acqDone] := by
    unfold semMutantTrace
    simp
  have h0 := h [issueObs SemSig (Caller.fiber 0) SemCall.acquire,
      compObs SemSig (Caller.fiber 0) SemCall.acquire SemResult.acqDone,
      issueObs SemSig (Caller.fiber 1) SemCall.acquire] 1
      [] hmf
  simp [semAcqCount, semIsTake, semRelIssueCount, semIsRelIssue, issueObs, compObs] at h0

end Sluice.Formal
