/-
Stage 8 — the scheduler driver (`Scheduler::run` / `run_until_idle`),
re-adjudicated on the post-#378 Stage-0-V2.3 authority.

The driver is not a call surface in the frozen calculus's sense: its
execution IS the §4 discipline (the single worker loop), not a call
against it.  The drain call fits neither the fiber shape (run-to-block:
the drain yields to the fibers it dispatches) nor the fused external
shape (its critical section is the whole loop, and external spawns
interleave inside it).  This file therefore carries a bespoke driver
LTS over the frozen observation model: the observation records
(`Obs`/`Caller`), the per-caller discipline (`SeqOK`), the slot and
in-flight records (`FSlot`/`ExtPend`), and the encoding-side machine
(`Encoding`/`SysStep`/`SysRuns`/`TracesEncS`) are reused unchanged; the
step relation, the run relation, and the disciplined trace language are
stage-local.

The declared substrate extension (Stage-0 §9.3, recorded on the stage
card): the driver state the bare substrate does not carry is exactly
`DriverState` — the terminate flag, the in-flight drain record, and the
unclaimed-spawn queue.  `BASE(Scheduler::run) = {}` (Stage-0 §5).

Census anchors (current C++):
- `run` / `run_until_idle` — scheduler.cpp:194-196 (`run_until_idle() {
  run(1); }` per scheduler.hpp:151); `run_impl` in drain mode :210-309.
- `spawn` — scheduler.cpp:131-159: one fused critical section under
  `global_mtx_` (route to a worker inbox tail while a run is active and
  not terminated, `pending_spawn_` otherwise).  Thread-safe, no
  `g_worker` read: external-capable.  Fiber-path spawn is outside the
  core surface (a nested call inside the task body would violate the
  frozen per-caller alternation; disclosed).
- dispatch — worker_loop popping `local_runnable` then `pending_spawn_`
  (scheduler.cpp:332-347; no steal at one worker :349),
  `run_next_on` :731-745 (`make_running` fail-fast :732-734,
  `running_fiber_count_` ++ :736, the context switch in/out, -- :744).
- the task body — `fiber_entry_bridge` scheduler.cpp:25-34 (body,
  `make_done`, the final switch back): a plain task's driver-state
  effect is empty, so the V2.3 effect/return split is vacuous here and
  `workDone` goes straight from the running slot to retired.
- the idle branch — scheduler.cpp:381-391 (the cross-primitive wake-scan
  legs are earlier-stage surfaces; at the core the scan is a silent
  classify) and `classify_locked_impl` :1013-1039 (runnable or running
  → mw_s1; outstanding → mw_s2; wait legs → mw_s3_unresolved; else
  quiescent).
- the drain exit at one worker — the idle-dance terminate
  :642-663 (`still` ∈ {mw_s3_unresolved, quiescent} at the last idle
  worker → terminate + break) and the mw_s2 no-progress exit :554-580;
  the core instance reaches only the quiescent exit, so the return gate
  is: backlog empty ∧ worker free.  `global_terminate_` is committed at
  the exit and cleared at `run_impl` entry :243.
- `run_live` and the multi-worker topology (work stealing, the
  admission election, threaded workers) are recorded extensions, out of
  the core verdict (the multi-worker adjudication is a campaign-level
  question the final report addresses).
- the classifier's outstanding/parked legs (`mw_s2`/`mw_s3_unresolved`)
  are extensions: they need parked or backend-attached tasks, and the
  wake surfaces they scan belong to the earlier stages.

The capability record: per-encoding-class conditionals over the bare
substrate, stated with stage-local analogs of the frozen judgments (the
frozen `OverProduces`/`UnderProduces` are typed over `PrimLTS2`, which
the driver deliberately is not).  This is RESEARCH/DEFER territory, not
THEOREM B; the open boundary is on the stage card.
-/

import Sluice.Formal.CalcV2
import Sluice.Formal.JudgeV2

namespace Sluice.Formal

/-! ## The alphabet

Three calls: `spawn` (submit a task fiber — fiber-callable at the core
only through the external path's identity: see the card; modeled
external-capable), `work` (the task fiber's own call — fiber-bound), and
`drain` (the `run`/`run_until_idle` request — external-capable, executed
by the driver loop). -/

inductive RunCall : Type where
  | spawn | work | drain
  deriving BEq, DecidableEq, Repr

inductive RunRes : Type where
  | rSpawn | rDone | rReturned
  deriving BEq, DecidableEq, Repr

def RunSig : ApiSig where
  Call := RunCall
  Result := RunRes

/-- The call-domain declaration.  `spawn`: a `global_mtx_`-only entry
point, no `g_worker` read (scheduler.cpp:131-159).  `work`: the task
fiber's call — there is no legitimate external issue of a task body.
`drain`: the run entry point, issued from the calling application thread
(group.cpp:39, application_runtime.cpp:464); its execution is the
driver loop itself, not a fused critical section — the drain's loop
steps are the machine's own steps below. -/
def runExtCap : RunCall → Bool
  | .spawn => true
  | .work => false
  | .drain => true

/-! ## The declared substrate extension (Stage-0 §9.3)

The driver state the bare substrate does not carry, recorded here as
the sanctioned extension: the terminate flag (`global_terminate_`,
committed at the drain exit, cleared at `run_impl` entry), the
in-flight drain record (the single coordinated run), and the
unclaimed-spawn queue (`pending_spawn_` — spawns with no active run, or
routed back at the worker's retire epilogue, scheduler.cpp:708-714). -/

structure DriverState : Type where
  term : Bool
  drainCaller : Option ExternalId
  pending : List FiberId

/-- The driver configuration.  `runq` is the merged runnable backlog
(FIFO dispatch order); `cur` is the dispatched task's slot (the baton:
the worker is inside the task's context switch).  There is no `now`:
the core instance registers no timers, and time advances only at idle
points (Stage-0 rule 7), which are extension steps here. -/
structure RunCfg : Type where
  prim : DriverState
  cur : Option (FSlot RunSig)
  runq : List FiberId
  retired : List FiberId
  nextFiber : FiberId
  exts : List (ExtPend RunSig)

def runInit : RunCfg :=
  ⟨⟨false, none, []⟩, none, [], [], 0, []⟩

/-! ## The driver step relation

Eight constructors.  Spawn's three-phase external shape (entry → fused
section → return) mirrors the calculus's `extApply`/`extEffect`/
`extDone`; the section's enqueue target is gate-split (a run is active:
the worker inbox tail, i.e. `runq`; otherwise `pending_spawn_`), which
is exactly scheduler.cpp:141-158.  Dispatch is the pop plus the task's
issue observation (Stage-0 rule 2: a fresh dispatch emits the issue) —
at one worker the pop, `make_running`, and the count increment are one
driver-side unit.  `workDone` is the task's completion (the entry
bridge's `make_done` plus the final switch back).  `drainEnter`/`drainReturn`
are the run boundary: entry flushes the unclaimed spawns into the
backlog (run_impl :229-237); return commits the terminate flag and
frees the drain, gated on quiescence (the classifier's quiescent exit
at one worker, scheduler.cpp:1013-1039 + :642-663). -/

inductive RunStep : RunCfg → Option (Obs RunSig) → RunCfg → Prop where
  /-- An external spawn enters (the issue observation; no state effect:
  entry precedes `global_mtx_`). -/
  | extSpawnApply (cfg : RunCfg) (x : ExternalId)
      (hfresh : x ∉ cfg.exts.map (fun e : ExtPend RunSig => e.x)) :
      RunStep cfg (some (issueObs RunSig (Caller.ext x) RunCall.spawn))
        { cfg with
            exts := cfg.exts ++ [{ x := x, call := RunCall.spawn, result := none }] }

  /-- The spawn's fused critical section while a run is active: route to
  the backlog tail, minting the task fiber (scheduler.cpp:142-148). -/
  | extSpawnEffectIn (cfg : RunCfg) (e : ExtPend RunSig) (rest : List (ExtPend RunSig))
      (heq : cfg.exts = rest ++ [e]) (hres : e.result = none)
      (hdrain : cfg.prim.drainCaller.isSome = true) :
      RunStep cfg none
        { cfg with
            runq := cfg.runq ++ [cfg.nextFiber],
            nextFiber := cfg.nextFiber + 1,
            exts := rest ++ [{ e with result := some RunRes.rSpawn }] }

  /-- The spawn's fused critical section with no active run: the task is
  unclaimed (`pending_spawn_`, scheduler.cpp:157). -/
  | extSpawnEffectOut (cfg : RunCfg) (e : ExtPend RunSig) (rest : List (ExtPend RunSig))
      (heq : cfg.exts = rest ++ [e]) (hres : e.result = none)
      (hdrain : cfg.prim.drainCaller = none) :
      RunStep cfg none
        { cfg with
            prim := { cfg.prim with pending := cfg.prim.pending ++ [cfg.nextFiber] },
            nextFiber := cfg.nextFiber + 1,
            exts := rest ++ [{ e with result := some RunRes.rSpawn }] }

  /-- The spawn's physical return. -/
  | extSpawnDone (cfg : RunCfg) (e : ExtPend RunSig) (rest : List (ExtPend RunSig))
      (heq : cfg.exts = rest ++ [e]) (hres : e.result = some RunRes.rSpawn) :
      RunStep cfg (some (compObs RunSig (Caller.ext e.x) RunCall.spawn RunRes.rSpawn))
        { cfg with exts := rest }

  /-- Dispatch: the worker pops the backlog head; the task's issue
  observation is emitted and the baton passes to the task
  (scheduler.cpp:332-347, :731-736). -/
  | dispatch (cfg : RunCfg) (f : FiberId) (rest : List FiberId)
      (hrunq : cfg.runq = f :: rest) (hcur : cfg.cur = none)
      (hdrain : cfg.prim.drainCaller.isSome = true) :
      RunStep cfg (some (issueObs RunSig (Caller.fiber f) RunCall.work))
        { cfg with
            cur := some (FSlot.running { fiber := f, call := RunCall.work } false),
            runq := rest }

  /-- The task completes: the entry bridge's `make_done` plus the final
  switch back (scheduler.cpp:31-33, :743-744).  A plain task's
  driver-state effect is empty, so the slot goes straight from running
  to retired. -/
  | workDone (cfg : RunCfg) (d : Pnd RunSig)
      (hcur : cfg.cur = some (FSlot.running d false)) :
      RunStep cfg (some (compObs RunSig (Caller.fiber d.fiber) RunCall.work RunRes.rDone))
        { cfg with
            cur := none,
            retired := cfg.retired ++ [d.fiber] }

  /-- The drain enters: the run call's issue observation, the terminate
  flag cleared, and the unclaimed spawns flushed into the backlog
  (run_impl :229-247). -/
  | drainEnter (cfg : RunCfg) (x : ExternalId)
      (hnone : cfg.prim.drainCaller = none) :
      RunStep cfg (some (issueObs RunSig (Caller.ext x) RunCall.drain))
        { cfg with
            prim := { term := false, drainCaller := some x, pending := [] },
            runq := cfg.runq ++ cfg.prim.pending }

  /-- The drain returns at quiescence: backlog empty, worker free (the
  classifier's quiescent exit, scheduler.cpp:1013-1039 + :642-663).  The
  terminate flag is committed; the drain is freed. -/
  | drainReturn (cfg : RunCfg) (x : ExternalId)
      (hdrain : cfg.prim.drainCaller = some x)
      (hrunq : cfg.runq = []) (hcur : cfg.cur = none) :
      RunStep cfg (some (compObs RunSig (Caller.ext x) RunCall.drain RunRes.rReturned))
        { cfg with
            prim := { cfg.prim with term := true, drainCaller := none } }

/-- The driver's run relation, mirroring `PrimRuns2`'s shape. -/
inductive RunRuns : RunCfg → Trace RunSig → RunCfg → Prop where
  | stop (cfg : RunCfg) : RunRuns cfg [] cfg
  | step (cfg cfg' : RunCfg) (o : Option (Obs RunSig)) (t : Trace RunSig) (fin : RunCfg) :
      RunStep cfg o cfg' → RunRuns cfg' t fin →
      RunRuns cfg (o.toList ++ t) fin

/-- The driver's (undisciplined) observable language from the initial
configuration. -/
def TracesRun (t : Trace RunSig) : Prop :=
  ∃ fin, RunRuns runInit t fin

/-- A run whose trace starts with an observation splits into a silent
prefix, the one observation-emitting step, and the suffix run. -/
theorem runRuns_cons {cfg : RunCfg} {t : Trace RunSig} {fin : RunCfg}
    (hrun : RunRuns cfg t fin) :
    ∀ (ob : Obs RunSig) (t' : Trace RunSig), t = ob :: t' →
      ∃ m1 m2 : RunCfg,
        RunRuns cfg [] m1 ∧
        RunStep m1 (some ob) m2 ∧
        RunRuns m2 t' fin := by
  induction hrun with
  | stop cfg => intro ob t' h; simp at h
  | step cfg cfg' o t2 fin2 hstep hrest ih =>
      intro ob t' h
      cases o with
      | none =>
          obtain ⟨m1, m2, h1, h2, h3⟩ := ih ob t' (by simpa [Option.toList] using h)
          exact ⟨m1, m2, RunRuns.step cfg cfg' none [] m1 hstep h1, h2, h3⟩
      | some ob' =>
          have hEq : ob' :: t2 = ob :: t' := by simpa [Option.toList] using h
          injection hEq with h1 h2
          subst h1; subst h2
          exact ⟨cfg, cfg', RunRuns.stop cfg, hstep, hrest⟩

/-- The disciplined driver trace language: the run language restricted
by the frozen per-caller sequential-call discipline (the same `SeqOK`
every stage uses). -/
def TracesRunS (t : Trace RunSig) : Prop :=
  TracesRun t ∧ SeqOK RunSig t

/-! ## The driver discipline

`runSafe` carries the driver's accounting as a state predicate: task
conservation (every minted task is exactly one of queued, unclaimed,
running, retired — no lost or duplicated task), the id bounds (fresh
mints), worker occupancy (a task on the worker implies an active run),
the idle-backlog shape (no active run ⇒ empty backlog — the backlog
gains tasks only through in-run effects and the entry flush), the
terminate-quiescence gate (the terminate flag is committed only at the
quiescent exit), and the in-flight spawn records' shape. -/

/-- The worker-occupancy count of the dispatched task's slot. -/
def occCount (o : Option (FSlot RunSig)) : Nat :=
  match o with
  | none => 0
  | some _ => 1

@[simp] theorem occCount_none : occCount (none : Option (FSlot RunSig)) = 0 := rfl

@[simp] theorem occCount_some (s : FSlot RunSig) : occCount (some s) = 1 := rfl

def runSafe (cfg : RunCfg) : Prop :=
  cfg.runq.length + cfg.prim.pending.length
      + occCount cfg.cur + cfg.retired.length = cfg.nextFiber
    ∧ (∀ f ∈ cfg.runq, f < cfg.nextFiber)
    ∧ (∀ f ∈ cfg.prim.pending, f < cfg.nextFiber)
    ∧ (∀ f ∈ cfg.retired, f < cfg.nextFiber)
    ∧ (∀ s : FSlot RunSig, cfg.cur = some s → FSlot.fiber s < cfg.nextFiber)
    ∧ (cfg.cur.isSome = true → cfg.prim.drainCaller.isSome = true)
    ∧ (cfg.prim.drainCaller = none → cfg.runq = [])
    ∧ (cfg.prim.term = true →
        cfg.prim.drainCaller = none ∧ cfg.runq = [] ∧ cfg.cur = none)
    ∧ (∀ e : ExtPend RunSig, e ∈ cfg.exts →
        e.call = RunCall.spawn ∧ (e.result = none ∨ e.result = some RunRes.rSpawn))

theorem runApply_preserves (cfg : RunCfg) (x : ExternalId)
    (_hfresh : x ∉ cfg.exts.map (fun e : ExtPend RunSig => e.x))
    (h : runSafe cfg) :
    runSafe { cfg with
      exts := cfg.exts ++ [{ x := x, call := RunCall.spawn, result := none }] } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcur, hocc, hidle, hterm, hext⟩ := h
  refine ⟨hcnt, hrq, hpd, hrt, hcur, hocc, hidle, hterm, ?_⟩
  intro e he
  rcases List.mem_append.mp he with hm | hm
  · exact hext e hm
  · have he1 : e = { x := x, call := RunCall.spawn, result := none } :=
      List.mem_singleton.mp hm
    subst he1
    exact ⟨rfl, Or.inl rfl⟩

theorem runEffectIn_preserves (cfg : RunCfg) (e : ExtPend RunSig) (rest : List (ExtPend RunSig))
    (heq : cfg.exts = rest ++ [e]) (hres : e.result = none)
    (hdrain : cfg.prim.drainCaller.isSome = true) (h : runSafe cfg) :
    runSafe { cfg with
      runq := cfg.runq ++ [cfg.nextFiber],
      nextFiber := cfg.nextFiber + 1,
      exts := rest ++ [{ e with result := some RunRes.rSpawn }] } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcur, hocc, hidle, hterm, hext⟩ := h
  have heMem : e ∈ cfg.exts := by
    rw [heq]
    exact List.mem_append.mpr (Or.inr (List.mem_singleton.mpr rfl))
  obtain ⟨heCall, _⟩ := hext e heMem
  have hsub : rest ⊆ cfg.exts := by
    intro a ha
    rw [heq]
    exact List.mem_append.mpr (Or.inl ha)
  unfold runSafe
  dsimp only []
  refine ⟨?_, ?_, ?_, ?_, ?_, hocc, ?_, ?_, ?_⟩
  · simp only [List.length_append, List.length_singleton]
    omega
  · intro f hf
    rcases List.mem_append.mp hf with hm | hm
    · exact Nat.lt_succ_of_lt (hrq f hm)
    · have hf1 : f = cfg.nextFiber := List.mem_singleton.mp hm
      subst hf1
      exact Nat.lt_succ_self cfg.nextFiber
  · intro f hf
    exact Nat.lt_succ_of_lt (hpd f hf)
  · intro f hf
    exact Nat.lt_succ_of_lt (hrt f hf)
  · intro s hs
    exact Nat.lt_succ_of_lt (hcur s hs)
  · intro hnone
    rw [hnone] at hdrain
    exact absurd hdrain (by simp)
  · intro ht
    have hd : cfg.prim.drainCaller = none := (hterm ht).1
    rw [hd] at hdrain
    exact absurd hdrain (by simp)
  · intro e' he'
    rcases List.mem_append.mp he' with hm | hm
    · exact hext e' (hsub hm)
    · have he1 : e' = { e with result := some RunRes.rSpawn } :=
        List.mem_singleton.mp hm
      subst he1
      exact ⟨heCall, Or.inr rfl⟩

theorem runEffectOut_preserves (cfg : RunCfg) (e : ExtPend RunSig) (rest : List (ExtPend RunSig))
    (heq : cfg.exts = rest ++ [e]) (hres : e.result = none)
    (_hdrain : cfg.prim.drainCaller = none) (h : runSafe cfg) :
    runSafe { cfg with
      prim := { cfg.prim with pending := cfg.prim.pending ++ [cfg.nextFiber] },
      nextFiber := cfg.nextFiber + 1,
      exts := rest ++ [{ e with result := some RunRes.rSpawn }] } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcur, hocc, hidle, hterm, hext⟩ := h
  have heMem : e ∈ cfg.exts := by
    rw [heq]
    exact List.mem_append.mpr (Or.inr (List.mem_singleton.mpr rfl))
  obtain ⟨heCall, _⟩ := hext e heMem
  have hsub : rest ⊆ cfg.exts := by
    intro a ha
    rw [heq]
    exact List.mem_append.mpr (Or.inl ha)
  unfold runSafe
  dsimp only []
  refine ⟨?_, ?_, ?_, ?_, ?_, hocc, hidle, hterm, ?_⟩
  · simp only [List.length_append, List.length_singleton]
    omega
  · intro f hf
    exact Nat.lt_succ_of_lt (hrq f hf)
  · intro f hf
    rcases List.mem_append.mp hf with hm | hm
    · exact Nat.lt_succ_of_lt (hpd f hm)
    · have hf1 : f = cfg.nextFiber := List.mem_singleton.mp hm
      subst hf1
      exact Nat.lt_succ_self cfg.nextFiber
  · intro f hf
    exact Nat.lt_succ_of_lt (hrt f hf)
  · intro s hs
    exact Nat.lt_succ_of_lt (hcur s hs)
  · intro e' he'
    rcases List.mem_append.mp he' with hm | hm
    · exact hext e' (hsub hm)
    · have he1 : e' = { e with result := some RunRes.rSpawn } :=
        List.mem_singleton.mp hm
      subst he1
      exact ⟨heCall, Or.inr rfl⟩

theorem runSpawnDone_preserves (cfg : RunCfg) (e : ExtPend RunSig) (rest : List (ExtPend RunSig))
    (heq : cfg.exts = rest ++ [e]) (_hres : e.result = some RunRes.rSpawn) (h : runSafe cfg) :
    runSafe { cfg with exts := rest } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcur, hocc, hidle, hterm, hext⟩ := h
  have hsub : rest ⊆ cfg.exts := by
    intro a ha
    rw [heq]
    exact List.mem_append.mpr (Or.inl ha)
  exact ⟨hcnt, hrq, hpd, hrt, hcur, hocc, hidle, hterm,
    fun e' he' => hext e' (hsub he')⟩

theorem runDispatch_preserves (cfg : RunCfg) (f : FiberId) (rest : List FiberId)
    (hrunq : cfg.runq = f :: rest) (hcur : cfg.cur = none)
    (hdrain : cfg.prim.drainCaller.isSome = true) (h : runSafe cfg) :
    runSafe { cfg with
      cur := some (FSlot.running { fiber := f, call := RunCall.work } false),
      runq := rest } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcurB, hocc, hidle, hterm, hext⟩ := h
  have hfMem : f ∈ cfg.runq := by rw [hrunq]; exact List.mem_cons_self
  have hfLt : f < cfg.nextFiber := hrq f hfMem
  unfold runSafe
  dsimp only []
  refine ⟨?_, ?_, hpd, hrt, ?_, ?_, ?_, ?_, hext⟩
  · rw [hrunq] at hcnt
    rw [hcur] at hcnt
    simp only [occCount_none, occCount_some, List.length_cons] at hcnt ⊢
    omega
  · intro g hg
    exact hrq g (by rw [hrunq]; exact List.mem_cons_of_mem _ hg)
  · intro s hs
    injection hs with h1
    subst h1
    exact hfLt
  · intro _
    exact hdrain
  · intro hnone
    rw [hnone] at hdrain
    exact absurd hdrain (by simp)
  · intro ht
    have hd : cfg.prim.drainCaller = none := (hterm ht).1
    rw [hd] at hdrain
    exact absurd hdrain (by simp)

theorem runWorkDone_preserves (cfg : RunCfg) (d : Pnd RunSig)
    (hcur : cfg.cur = some (FSlot.running d false)) (h : runSafe cfg) :
    runSafe { cfg with
      cur := none,
      retired := cfg.retired ++ [d.fiber] } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcurB, hocc, hidle, hterm, hext⟩ := h
  have hfLt : d.fiber < cfg.nextFiber := hcurB (FSlot.running d false) hcur
  unfold runSafe
  dsimp only []
  refine ⟨?_, hrq, hpd, ?_, ?_, ?_, hidle, ?_, hext⟩
  · rw [hcur] at hcnt
    simp only [occCount_some, occCount_none, List.length_append, List.length_singleton]
      at hcnt ⊢
    omega
  · intro g hg
    rcases List.mem_append.mp hg with hm | hm
    · exact hrt g hm
    · have hg1 : g = d.fiber := List.mem_singleton.mp hm
      subst hg1
      exact hfLt
  · intro s hs
    exact absurd hs (by simp)
  · intro hc
    rw [Option.isSome_none] at hc
    exact Bool.noConfusion hc
  · intro ht
    have hc0 : cfg.cur = none := (hterm ht).2.2
    rw [hcur] at hc0
    exact absurd hc0 (by simp)

theorem runDrainEnter_preserves (cfg : RunCfg) (x : ExternalId)
    (hnone : cfg.prim.drainCaller = none) (h : runSafe cfg) :
    runSafe { cfg with
      prim := { term := false, drainCaller := some x, pending := [] },
      runq := cfg.runq ++ cfg.prim.pending } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcur, hocc, hidle, hterm, hext⟩ := h
  unfold runSafe
  dsimp only []
  refine ⟨?_, ?_, ?_, hrt, hcur, ?_, ?_, ?_, hext⟩
  · simp only [List.length_append, List.length_nil, occCount_none]
    omega
  · intro g hg
    rcases List.mem_append.mp hg with hm | hm
    · exact hrq g hm
    · exact hpd g hm
  · intro g hg
    cases hg
  · intro _
    exact rfl
  · intro hfalse
    exact absurd hfalse (by simp)
  · intro ht
    exact Bool.noConfusion ht

theorem runDrainReturn_preserves (cfg : RunCfg) (x : ExternalId)
    (_hdrain : cfg.prim.drainCaller = some x)
    (hrunq : cfg.runq = []) (hcur : cfg.cur = none) (h : runSafe cfg) :
    runSafe { cfg with
      prim := { cfg.prim with term := true, drainCaller := none } } := by
  obtain ⟨hcnt, hrq, hpd, hrt, hcurB, hocc, hidle, hterm, hext⟩ := h
  unfold runSafe
  dsimp only []
  refine ⟨hcnt, hrq, hpd, hrt, hcurB, ?_, ?_, ?_, hext⟩
  · intro hc
    rw [hcur] at hc
    exact absurd hc (by simp)
  · intro _
    exact hrunq
  · intro _
    exact ⟨rfl, hrunq, hcur⟩

theorem run_safe {cfg fin : RunCfg} {t : Trace RunSig}
    (hrun : RunRuns cfg t fin) : runSafe cfg → runSafe fin := by
  induction hrun with
  | stop cfg => exact fun h => h
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro h
      refine ih ?_
      cases hstep with
      | extSpawnApply x hfresh => exact runApply_preserves cfg x hfresh h
      | extSpawnEffectIn e rest heq hres hdrain =>
          exact runEffectIn_preserves cfg e rest heq hres hdrain h
      | extSpawnEffectOut e rest heq hres hdrain =>
          exact runEffectOut_preserves cfg e rest heq hres hdrain h
      | extSpawnDone e rest heq hres => exact runSpawnDone_preserves cfg e rest heq hres h
      | dispatch f rest hrunq hcur hdrain =>
          exact runDispatch_preserves cfg f rest hrunq hcur hdrain h
      | workDone d hcur => exact runWorkDone_preserves cfg d hcur h
      | drainEnter x hnone => exact runDrainEnter_preserves cfg x hnone h
      | drainReturn x hdrain hrunq hcur =>
          exact runDrainReturn_preserves cfg x hdrain hrunq hcur h

theorem runSafe_runInit : runSafe runInit := by
  refine ⟨?_, ?_, ?_, ?_, ?_, ?_, ?_, ?_, ?_⟩
  · rfl
  · intro f hf; cases hf
  · intro f hf; cases hf
  · intro f hf; cases hf
  · intro s hs; cases hs
  · intro hc; exact Bool.noConfusion hc
  · intro _; rfl
  · intro ht; exact Bool.noConfusion ht
  · intro e he; cases he

theorem run_safe_holds {fin : RunCfg} {t : Trace RunSig}
    (hrun : RunRuns runInit t fin) : runSafe fin :=
  run_safe hrun runSafe_runInit

/-! ## Possession batteries

Six reachability chains from `runInit`, one per externally meaningful
behavior class: the canonical drain, FIFO dispatch order, the
mid-drain spawn (the spawn's section landing inside the run), the
stale-classify window (a spawn entering before the drain's final
return, its section landing after — the task waits unclaimed), the
sequential drains (the post-terminate `pending_spawn_` path and the
re-entry flush), and the empty drain. -/

def ri (cl : Caller) (c : RunCall) : Obs RunSig := issueObs RunSig cl c
def rc (cl : Caller) (c : RunCall) (r : RunRes) : Obs RunSig := compObs RunSig cl c r

/-- Battery 1 — the canonical drain: a pre-run spawn, the drain, one
dispatched task, the return at quiescence. -/
def b1 : Trace RunSig :=
  [ri (Caller.ext 0) RunCall.spawn,
   rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn,
   ri (Caller.ext 1) RunCall.drain,
   ri (Caller.fiber 0) RunCall.work,
   rc (Caller.fiber 0) RunCall.work RunRes.rDone,
   rc (Caller.ext 1) RunCall.drain RunRes.rReturned]

abbrev dr1 : RunCfg := { prim := ⟨false, none, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [{ x := 0, call := RunCall.spawn, result := none }] }
abbrev dr2 : RunCfg := { prim := ⟨false, none, [0]⟩, cur := none, runq := [], retired := [], nextFiber := 1, exts := [{ x := 0, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev dr3 : RunCfg := { prim := ⟨false, none, [0]⟩, cur := none, runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev dr4 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := none, runq := [0], retired := [], nextFiber := 1, exts := [] }
abbrev dr5 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev dr6 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := none, runq := [], retired := [0], nextFiber := 1, exts := [] }
abbrev dr7 : RunCfg := { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem b1t1 : RunStep runInit (some (ri (Caller.ext 0) RunCall.spawn)) dr1 :=
  RunStep.extSpawnApply runInit 0 (by simp [runInit])
theorem b1t2 : RunStep dr1 none dr2 :=
  RunStep.extSpawnEffectOut dr1 ⟨0, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b1t3 : RunStep dr2 (some (rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn)) dr3 :=
  RunStep.extSpawnDone dr2 ⟨0, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl
theorem b1t4 : RunStep dr3 (some (ri (Caller.ext 1) RunCall.drain)) dr4 :=
  RunStep.drainEnter dr3 1 rfl
theorem b1t5 : RunStep dr4 (some (ri (Caller.fiber 0) RunCall.work)) dr5 :=
  RunStep.dispatch dr4 0 [] rfl rfl rfl
theorem b1t6 : RunStep dr5 (some (rc (Caller.fiber 0) RunCall.work RunRes.rDone)) dr6 :=
  RunStep.workDone dr5 ⟨0, RunCall.work⟩ rfl
theorem b1t7 : RunStep dr6 (some (rc (Caller.ext 1) RunCall.drain RunRes.rReturned)) dr7 :=
  RunStep.drainReturn dr6 1 rfl rfl rfl

theorem battery_canonical :
    ∃ fin : RunCfg,
      RunRuns runInit b1 fin ∧ fin.prim.term = true ∧ fin.retired = [0] := by
  refine ⟨dr7, ?_, rfl, rfl⟩
  exact RunRuns.step runInit dr1 _ _ dr7 b1t1
    (RunRuns.step dr1 dr2 none _ dr7 b1t2
      (RunRuns.step dr2 dr3 _ _ dr7 b1t3
        (RunRuns.step dr3 dr4 _ _ dr7 b1t4
          (RunRuns.step dr4 dr5 _ _ dr7 b1t5
            (RunRuns.step dr5 dr6 _ _ dr7 b1t6
              (RunRuns.step dr6 dr7 _ _ dr7 b1t7 (RunRuns.stop dr7)))))))

theorem seqOK_b1 : SeqOK RunSig b1 := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- Battery 2 — FIFO dispatch: two spawns mint in order, the drain
dispatches them in that order. -/
def b2 : Trace RunSig :=
  [ri (Caller.ext 0) RunCall.spawn,
   rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn,
   ri (Caller.ext 0) RunCall.spawn,
   rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn,
   ri (Caller.ext 1) RunCall.drain,
   ri (Caller.fiber 0) RunCall.work,
   rc (Caller.fiber 0) RunCall.work RunRes.rDone,
   ri (Caller.fiber 1) RunCall.work,
   rc (Caller.fiber 1) RunCall.work RunRes.rDone,
   rc (Caller.ext 1) RunCall.drain RunRes.rReturned]

abbrev fb1 : RunCfg := { prim := ⟨false, none, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [{ x := 0, call := RunCall.spawn, result := none }] }
abbrev fb2 : RunCfg := { prim := ⟨false, none, [0]⟩, cur := none, runq := [], retired := [], nextFiber := 1, exts := [{ x := 0, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev fb3 : RunCfg := { prim := ⟨false, none, [0]⟩, cur := none, runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev fb4 : RunCfg := { prim := ⟨false, none, [0]⟩, cur := none, runq := [], retired := [], nextFiber := 1, exts := [{ x := 0, call := RunCall.spawn, result := none }] }
abbrev fb5 : RunCfg := { prim := ⟨false, none, [0, 1]⟩, cur := none, runq := [], retired := [], nextFiber := 2, exts := [{ x := 0, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev fb6 : RunCfg := { prim := ⟨false, none, [0, 1]⟩, cur := none, runq := [], retired := [], nextFiber := 2, exts := [] }
abbrev fb7 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := none, runq := [0, 1], retired := [], nextFiber := 2, exts := [] }
abbrev fb8 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [1], retired := [], nextFiber := 2, exts := [] }
abbrev fb9 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := none, runq := [1], retired := [0], nextFiber := 2, exts := [] }
abbrev fb10 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := some (FSlot.running { fiber := 1, call := RunCall.work } false), runq := [], retired := [0], nextFiber := 2, exts := [] }
abbrev fb11 : RunCfg := { prim := ⟨false, some 1, []⟩, cur := none, runq := [], retired := [0, 1], nextFiber := 2, exts := [] }
abbrev fb12 : RunCfg := { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [0, 1], nextFiber := 2, exts := [] }

theorem b2t1 : RunStep runInit (some (ri (Caller.ext 0) RunCall.spawn)) fb1 :=
  RunStep.extSpawnApply runInit 0 (by simp [runInit])
theorem b2t2 : RunStep fb1 none fb2 :=
  RunStep.extSpawnEffectOut fb1 ⟨0, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b2t3 : RunStep fb2 (some (rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn)) fb3 :=
  RunStep.extSpawnDone fb2 ⟨0, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl
theorem b2t4 : RunStep fb3 (some (ri (Caller.ext 0) RunCall.spawn)) fb4 :=
  RunStep.extSpawnApply fb3 0 (by simp)
theorem b2t5 : RunStep fb4 none fb5 :=
  RunStep.extSpawnEffectOut fb4 ⟨0, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b2t6 : RunStep fb5 (some (rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn)) fb6 :=
  RunStep.extSpawnDone fb5 ⟨0, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl
theorem b2t7 : RunStep fb6 (some (ri (Caller.ext 1) RunCall.drain)) fb7 :=
  RunStep.drainEnter fb6 1 rfl
theorem b2t8 : RunStep fb7 (some (ri (Caller.fiber 0) RunCall.work)) fb8 :=
  RunStep.dispatch fb7 0 [1] rfl rfl rfl
theorem b2t9 : RunStep fb8 (some (rc (Caller.fiber 0) RunCall.work RunRes.rDone)) fb9 :=
  RunStep.workDone fb8 ⟨0, RunCall.work⟩ rfl
theorem b2t10 : RunStep fb9 (some (ri (Caller.fiber 1) RunCall.work)) fb10 :=
  RunStep.dispatch fb9 1 [] rfl rfl rfl
theorem b2t11 : RunStep fb10 (some (rc (Caller.fiber 1) RunCall.work RunRes.rDone)) fb11 :=
  RunStep.workDone fb10 ⟨1, RunCall.work⟩ rfl
theorem b2t12 : RunStep fb11 (some (rc (Caller.ext 1) RunCall.drain RunRes.rReturned)) fb12 :=
  RunStep.drainReturn fb11 1 rfl rfl rfl

theorem battery_fifo :
    ∃ fin : RunCfg,
      RunRuns runInit b2 fin ∧ fin.retired = [0, 1] := by
  refine ⟨fb12, ?_, rfl⟩
  exact RunRuns.step runInit fb1 _ _ fb12 b2t1
    (RunRuns.step fb1 fb2 none _ fb12 b2t2
      (RunRuns.step fb2 fb3 _ _ fb12 b2t3
        (RunRuns.step fb3 fb4 _ _ fb12 b2t4
          (RunRuns.step fb4 fb5 none _ fb12 b2t5
            (RunRuns.step fb5 fb6 _ _ fb12 b2t6
              (RunRuns.step fb6 fb7 _ _ fb12 b2t7
                (RunRuns.step fb7 fb8 _ _ fb12 b2t8
                  (RunRuns.step fb8 fb9 _ _ fb12 b2t9
                    (RunRuns.step fb9 fb10 _ _ fb12 b2t10
                      (RunRuns.step fb10 fb11 _ _ fb12 b2t11
                        (RunRuns.step fb11 fb12 _ _ fb12 b2t12 (RunRuns.stop fb12))))))))))))

theorem seqOK_b2 : SeqOK RunSig b2 := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- Battery 3 — the mid-drain spawn: the spawn's section lands while a
task is on the worker (the routed enqueue) and again while the next
task is queued; the drain picks both up. -/
def b3 : Trace RunSig :=
  [ri (Caller.ext 0) RunCall.drain,
   ri (Caller.ext 1) RunCall.spawn,
   rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn,
   ri (Caller.fiber 0) RunCall.work,
   ri (Caller.ext 1) RunCall.spawn,
   rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn,
   rc (Caller.fiber 0) RunCall.work RunRes.rDone,
   ri (Caller.fiber 1) RunCall.work,
   rc (Caller.fiber 1) RunCall.work RunRes.rDone,
   rc (Caller.ext 0) RunCall.drain RunRes.rReturned]

abbrev md0 : RunCfg := runInit
abbrev md1 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }
abbrev md2 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [{ x := 1, call := RunCall.spawn, result := none }] }
abbrev md3 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [0], retired := [], nextFiber := 1, exts := [{ x := 1, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev md4 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [0], retired := [], nextFiber := 1, exts := [] }
abbrev md5 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev md6 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [], retired := [], nextFiber := 1, exts := [{ x := 1, call := RunCall.spawn, result := none }] }
abbrev md7 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [1], retired := [], nextFiber := 2, exts := [{ x := 1, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev md8 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [1], retired := [], nextFiber := 2, exts := [] }
abbrev md9 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [1], retired := [0], nextFiber := 2, exts := [] }
abbrev md10 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 1, call := RunCall.work } false), runq := [], retired := [0], nextFiber := 2, exts := [] }
abbrev md11 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [0, 1], nextFiber := 2, exts := [] }
abbrev md12 : RunCfg := { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [0, 1], nextFiber := 2, exts := [] }

theorem b3t1 : RunStep md0 (some (ri (Caller.ext 0) RunCall.drain)) md1 :=
  RunStep.drainEnter md0 0 rfl
theorem b3t2 : RunStep md1 (some (ri (Caller.ext 1) RunCall.spawn)) md2 :=
  RunStep.extSpawnApply md1 1 (by simp [md1])
theorem b3t3 : RunStep md2 none md3 :=
  RunStep.extSpawnEffectIn md2 ⟨1, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b3t4 : RunStep md3 (some (rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn)) md4 :=
  RunStep.extSpawnDone md3 ⟨1, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl
theorem b3t5 : RunStep md4 (some (ri (Caller.fiber 0) RunCall.work)) md5 :=
  RunStep.dispatch md4 0 [] rfl rfl rfl
theorem b3t6 : RunStep md5 (some (ri (Caller.ext 1) RunCall.spawn)) md6 :=
  RunStep.extSpawnApply md5 1 (by simp [md5])
theorem b3t7 : RunStep md6 none md7 :=
  RunStep.extSpawnEffectIn md6 ⟨1, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b3t8 : RunStep md7 (some (rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn)) md8 :=
  RunStep.extSpawnDone md7 ⟨1, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl
theorem b3t9 : RunStep md8 (some (rc (Caller.fiber 0) RunCall.work RunRes.rDone)) md9 :=
  RunStep.workDone md8 ⟨0, RunCall.work⟩ rfl
theorem b3t10 : RunStep md9 (some (ri (Caller.fiber 1) RunCall.work)) md10 :=
  RunStep.dispatch md9 1 [] rfl rfl rfl
theorem b3t11 : RunStep md10 (some (rc (Caller.fiber 1) RunCall.work RunRes.rDone)) md11 :=
  RunStep.workDone md10 ⟨1, RunCall.work⟩ rfl
theorem b3t12 : RunStep md11 (some (rc (Caller.ext 0) RunCall.drain RunRes.rReturned)) md12 :=
  RunStep.drainReturn md11 0 rfl rfl rfl

theorem battery_midDrain :
    ∃ fin : RunCfg,
      RunRuns runInit b3 fin ∧ fin.retired = [0, 1] := by
  refine ⟨md12, ?_, rfl⟩
  exact RunRuns.step md0 md1 _ _ md12 b3t1
    (RunRuns.step md1 md2 _ _ md12 b3t2
      (RunRuns.step md2 md3 none _ md12 b3t3
        (RunRuns.step md3 md4 _ _ md12 b3t4
          (RunRuns.step md4 md5 _ _ md12 b3t5
            (RunRuns.step md5 md6 _ _ md12 b3t6
              (RunRuns.step md6 md7 none _ md12 b3t7
                (RunRuns.step md7 md8 _ _ md12 b3t8
                  (RunRuns.step md8 md9 _ _ md12 b3t9
                    (RunRuns.step md9 md10 _ _ md12 b3t10
                      (RunRuns.step md10 md11 _ _ md12 b3t11
                        (RunRuns.step md11 md12 _ _ md12 b3t12 (RunRuns.stop md12))))))))))))

theorem seqOK_b3 : SeqOK RunSig b3 := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- Battery 4 — the stale-classify window: a spawn enters before the
drain's final return; the drain's quiescence gate does not see it, the
task waits unclaimed, and its section lands after the return. -/
def b4 : Trace RunSig :=
  [ri (Caller.ext 0) RunCall.drain,
   ri (Caller.ext 1) RunCall.spawn,
   rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn,
   ri (Caller.fiber 0) RunCall.work,
   rc (Caller.fiber 0) RunCall.work RunRes.rDone,
   ri (Caller.ext 1) RunCall.spawn,
   rc (Caller.ext 0) RunCall.drain RunRes.rReturned,
   rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn]

abbrev sw0 : RunCfg := runInit
abbrev sw1 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }
abbrev sw2 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [{ x := 1, call := RunCall.spawn, result := none }] }
abbrev sw3 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [0], retired := [], nextFiber := 1, exts := [{ x := 1, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev sw4 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [0], retired := [], nextFiber := 1, exts := [] }
abbrev sw5 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sw6 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [0], nextFiber := 1, exts := [] }
abbrev sw7 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [0], nextFiber := 1, exts := [{ x := 1, call := RunCall.spawn, result := none }] }
abbrev sw8 : RunCfg := { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [0], nextFiber := 1, exts := [{ x := 1, call := RunCall.spawn, result := none }] }
abbrev sw9 : RunCfg := { prim := ⟨true, none, [1]⟩, cur := none, runq := [], retired := [0], nextFiber := 2, exts := [{ x := 1, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev sw10 : RunCfg := { prim := ⟨true, none, [1]⟩, cur := none, runq := [], retired := [0], nextFiber := 2, exts := [] }

theorem b4t1 : RunStep sw0 (some (ri (Caller.ext 0) RunCall.drain)) sw1 :=
  RunStep.drainEnter sw0 0 rfl
theorem b4t2 : RunStep sw1 (some (ri (Caller.ext 1) RunCall.spawn)) sw2 :=
  RunStep.extSpawnApply sw1 1 (by simp [sw1])
theorem b4t3 : RunStep sw2 none sw3 :=
  RunStep.extSpawnEffectIn sw2 ⟨1, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b4t4 : RunStep sw3 (some (rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn)) sw4 :=
  RunStep.extSpawnDone sw3 ⟨1, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl
theorem b4t5 : RunStep sw4 (some (ri (Caller.fiber 0) RunCall.work)) sw5 :=
  RunStep.dispatch sw4 0 [] rfl rfl rfl
theorem b4t6 : RunStep sw5 (some (rc (Caller.fiber 0) RunCall.work RunRes.rDone)) sw6 :=
  RunStep.workDone sw5 ⟨0, RunCall.work⟩ rfl
theorem b4t7 : RunStep sw6 (some (ri (Caller.ext 1) RunCall.spawn)) sw7 :=
  RunStep.extSpawnApply sw6 1 (by simp [sw6])
theorem b4t8 : RunStep sw7 (some (rc (Caller.ext 0) RunCall.drain RunRes.rReturned)) sw8 :=
  RunStep.drainReturn sw7 0 rfl rfl rfl
theorem b4t9 : RunStep sw8 none sw9 :=
  RunStep.extSpawnEffectOut sw8 ⟨1, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b4t10 : RunStep sw9 (some (rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn)) sw10 :=
  RunStep.extSpawnDone sw9 ⟨1, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl

theorem battery_staleWindow :
    ∃ fin : RunCfg,
      RunRuns runInit b4 fin ∧ fin.prim.term = true ∧ fin.prim.pending = [1] := by
  refine ⟨sw10, ?_, rfl, rfl⟩
  exact RunRuns.step sw0 sw1 _ _ sw10 b4t1
    (RunRuns.step sw1 sw2 _ _ sw10 b4t2
      (RunRuns.step sw2 sw3 none _ sw10 b4t3
        (RunRuns.step sw3 sw4 _ _ sw10 b4t4
          (RunRuns.step sw4 sw5 _ _ sw10 b4t5
            (RunRuns.step sw5 sw6 _ _ sw10 b4t6
              (RunRuns.step sw6 sw7 _ _ sw10 b4t7
                (RunRuns.step sw7 sw8 _ _ sw10 b4t8
                  (RunRuns.step sw8 sw9 none _ sw10 b4t9
                    (RunRuns.step sw9 sw10 _ _ sw10 b4t10 (RunRuns.stop sw10))))))))))

theorem seqOK_b4 : SeqOK RunSig b4 := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- Battery 5 — sequential drains: the first drain returns empty; the
spawn lands unclaimed (post-terminate `pending_spawn_`); the second
drain flushes it and runs it. -/
def b5 : Trace RunSig :=
  [ri (Caller.ext 0) RunCall.drain,
   rc (Caller.ext 0) RunCall.drain RunRes.rReturned,
   ri (Caller.ext 1) RunCall.spawn,
   rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn,
   ri (Caller.ext 0) RunCall.drain,
   ri (Caller.fiber 0) RunCall.work,
   rc (Caller.fiber 0) RunCall.work RunRes.rDone,
   rc (Caller.ext 0) RunCall.drain RunRes.rReturned]

abbrev sq0 : RunCfg := runInit
abbrev sq1 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }
abbrev sq2 : RunCfg := { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }
abbrev sq3 : RunCfg := { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [{ x := 1, call := RunCall.spawn, result := none }] }
abbrev sq4 : RunCfg := { prim := ⟨true, none, [0]⟩, cur := none, runq := [], retired := [], nextFiber := 1, exts := [{ x := 1, call := RunCall.spawn, result := some RunRes.rSpawn }] }
abbrev sq5 : RunCfg := { prim := ⟨true, none, [0]⟩, cur := none, runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sq6 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [0], retired := [], nextFiber := 1, exts := [] }
abbrev sq7 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [], retired := [], nextFiber := 1, exts := [] }
abbrev sq8 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [0], nextFiber := 1, exts := [] }
abbrev sq9 : RunCfg := { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem b5t1 : RunStep sq0 (some (ri (Caller.ext 0) RunCall.drain)) sq1 :=
  RunStep.drainEnter sq0 0 rfl
theorem b5t2 : RunStep sq1 (some (rc (Caller.ext 0) RunCall.drain RunRes.rReturned)) sq2 :=
  RunStep.drainReturn sq1 0 rfl rfl rfl
theorem b5t3 : RunStep sq2 (some (ri (Caller.ext 1) RunCall.spawn)) sq3 :=
  RunStep.extSpawnApply sq2 1 (by simp [sq2])
theorem b5t4 : RunStep sq3 none sq4 :=
  RunStep.extSpawnEffectOut sq3 ⟨1, RunCall.spawn, none⟩ [] rfl rfl rfl
theorem b5t5 : RunStep sq4 (some (rc (Caller.ext 1) RunCall.spawn RunRes.rSpawn)) sq5 :=
  RunStep.extSpawnDone sq4 ⟨1, RunCall.spawn, some RunRes.rSpawn⟩ [] rfl rfl
theorem b5t6 : RunStep sq5 (some (ri (Caller.ext 0) RunCall.drain)) sq6 :=
  RunStep.drainEnter sq5 0 rfl
theorem b5t7 : RunStep sq6 (some (ri (Caller.fiber 0) RunCall.work)) sq7 :=
  RunStep.dispatch sq6 0 [] rfl rfl rfl
theorem b5t8 : RunStep sq7 (some (rc (Caller.fiber 0) RunCall.work RunRes.rDone)) sq8 :=
  RunStep.workDone sq7 ⟨0, RunCall.work⟩ rfl
theorem b5t9 : RunStep sq8 (some (rc (Caller.ext 0) RunCall.drain RunRes.rReturned)) sq9 :=
  RunStep.drainReturn sq8 0 rfl rfl rfl

theorem battery_seqDrains :
    ∃ fin : RunCfg,
      RunRuns runInit b5 fin ∧ fin.prim.term = true ∧ fin.retired = [0] := by
  refine ⟨sq9, ?_, rfl, rfl⟩
  exact RunRuns.step sq0 sq1 _ _ sq9 b5t1
    (RunRuns.step sq1 sq2 _ _ sq9 b5t2
      (RunRuns.step sq2 sq3 _ _ sq9 b5t3
        (RunRuns.step sq3 sq4 none _ sq9 b5t4
          (RunRuns.step sq4 sq5 _ _ sq9 b5t5
            (RunRuns.step sq5 sq6 _ _ sq9 b5t6
              (RunRuns.step sq6 sq7 _ _ sq9 b5t7
                (RunRuns.step sq7 sq8 _ _ sq9 b5t8
                  (RunRuns.step sq8 sq9 _ _ sq9 b5t9 (RunRuns.stop sq9)))))))))

theorem seqOK_b5 : SeqOK RunSig b5 := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- Battery 6 — the empty drain: enter and return with nothing queued. -/
def b6 : Trace RunSig :=
  [ri (Caller.ext 0) RunCall.drain,
   rc (Caller.ext 0) RunCall.drain RunRes.rReturned]

abbrev edn1 : RunCfg := { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }

theorem battery_empty :
    ∃ fin : RunCfg,
      RunRuns runInit b6 fin ∧ fin.prim.term = true := by
  refine ⟨{ prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }, ?_, rfl⟩
  exact RunRuns.step runInit edn1 _ _ _ (RunStep.drainEnter runInit 0 rfl)
    (RunRuns.step edn1 _ _ _ _ (RunStep.drainReturn edn1 0 rfl rfl rfl) (RunRuns.stop _))

theorem seqOK_b6 : SeqOK RunSig b6 := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-! ## The capability record

Per-encoding-class conditionals over the bare substrate
(`BaseOpsSig.none` — `BASE(Scheduler::run) = {}`, Stage-0 §5), stated
with stage-local analogs of the frozen judgments: the frozen
`OverProduces`/`UnderProduces` are typed over `PrimLTS2`, and the
driver deliberately is not a `PrimLTS2` (its execution IS the §4
discipline, not a call against it).  The encoding-side machine
(`Encoding`/`SysStep`/`TracesEncS`) is reused unchanged.  These are NOT
the stage verdict and must not be read as THEOREM B: each conditional
is stated for one encoding class only, no universal separation is
claimed, and none is refuted.  The open boundary — whether any
encoding's disciplined trace language is observationally equivalent to
the driver's under Stage-0 V2.3 — is recorded on the stage card. -/

def RunUnderProduces (O : BaseOpsSig) (enc : Encoding O RunSig) : Prop :=
  ∃ t, TracesRunS t ∧ ¬ TracesEncS O RunSig enc t

def RunOverProduces (O : BaseOpsSig) (enc : Encoding O RunSig) : Prop :=
  ∃ t, TracesEncS O RunSig enc t ∧ ¬ TracesRunS t

/-- The natural driver-shaped encoding over the bare substrate:
call domains declared faithfully (`spawn`/`drain` external, `work`
fiber-bound), programs that bottom out with a value no decoding
accepts. -/
def encRun : Encoding BaseOpsSig.none RunSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := runExtCap

theorem run_encoding_class_inhabited :
    ∃ _enc : Encoding BaseOpsSig.none RunSig, True := ⟨encRun, trivial⟩

/-- The over-production separator: a bare external `work` issue. -/
def extWorkTrace : Trace RunSig :=
  [issueObs RunSig (Caller.ext 0) RunCall.work]

theorem seqOK_extWork : SeqOK RunSig extWorkTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- The driver never produces the external `work` issue: the only
step that emits a `work` issue is `dispatch`, and it addresses the
dispatched task's own fiber. -/
theorem run_not_extWork : ¬ TracesRun extWorkTrace := by
  rintro ⟨fin, hrun⟩
  obtain ⟨ob, t', heq⟩ : ∃ ob t', extWorkTrace = ob :: t' := ⟨_, _, rfl⟩
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := runRuns_cons hrun ob t' heq
  cases hstep with
  | extSpawnApply x _ =>
      injection heq with h3 h4
      have h5 : RunCall.work = RunCall.spawn := congrArg Obs.call h3
      exact absurd h5 (by simp)
  | extSpawnDone e rest heq' hres =>
      injection heq with h3 h4
      have h5 : RunCall.work = RunCall.spawn := congrArg Obs.call h3
      exact absurd h5 (by simp)
  | dispatch f rest hrunq hcur hdrain =>
      injection heq with h3 h4
      have h5 : Caller.ext 0 = Caller.fiber f := congrArg Obs.caller h3
      exact absurd h5 (by simp)
  | workDone d hcur =>
      injection heq with h3 h4
      have h5 : Caller.ext 0 = Caller.fiber d.fiber := congrArg Obs.caller h3
      exact absurd h5 (by simp)
  | drainEnter x hnone =>
      injection heq with h3 h4
      have h5 : RunCall.work = RunCall.drain := congrArg Obs.call h3
      exact absurd h5 (by simp)
  | drainReturn x hdrain hrunq hcur =>
      injection heq with h3 h4
      have h5 : RunCall.work = RunCall.drain := congrArg Obs.call h3
      exact absurd h5 (by simp)

/-- Every encoding declaring the task's call external emits the bare
issue at entry (`SysStep.extStart`). -/
theorem enc_extWork (enc : Encoding BaseOpsSig.none RunSig)
    (hcap : enc.extCap RunCall.work = true) :
    TracesEnc BaseOpsSig.none RunSig enc extWorkTrace := by
  refine ⟨{ st := subInit, bst := (), cur := none, parked := [], runq := ([] : List (Ready BaseOpsSig.none RunSig)), retired := ([] : List FiberId), nextFiber := 0, exts := [{ x := 0, call := RunCall.work, prog := enc.prog RunCall.work }] },
    SysRuns.step (encInit BaseOpsSig.none RunSig) _ _ [] _
      (SysStep.extStart (encInit BaseOpsSig.none RunSig) 0 RunCall.work hcap
        (by show (0 : ExternalId) ∉ ([] : List (ExtBusy BaseOpsSig.none RunSig)).map
              (fun e : ExtBusy BaseOpsSig.none RunSig => e.x)
            simp [encInit]))
      (SysRuns.stop _)⟩

/-- Auxiliary, per-encoding-class result — NOT the stage verdict: an
encoding that declares the task's call external over-produces. -/
theorem run_over_produces_of_extCap (enc : Encoding BaseOpsSig.none RunSig)
    (hcap : enc.extCap RunCall.work = true) :
    RunOverProduces BaseOpsSig.none enc :=
  ⟨extWorkTrace, ⟨enc_extWork enc hcap, seqOK_extWork⟩,
    fun hP => run_not_extWork hP.1⟩

/-- Inverting the step that emits the external `drain` issue. -/
theorem drainCap_true_of_drainIssue {enc : Encoding BaseOpsSig.none RunSig}
    {cfg m2 : SysCfg BaseOpsSig.none RunSig} {ob : Obs RunSig}
    (hstep : SysStep BaseOpsSig.none RunSig enc SubStep cfg (some ob) m2)
    (hx : ob.caller = Caller.ext 0) (hc : ob.call = RunCall.drain)
    (hr : ob.result = Option.none) :
    enc.extCap RunCall.drain = true := by
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
      have hc1 : c = RunCall.drain := hc
      rw [hc1] at hc'
      exact hc'

/-- Auxiliary, per-encoding-class result — NOT the stage verdict: an
encoding that declares the run entry point internal under-produces (it
can never emit the external `drain` issue the driver produces). -/
theorem run_under_produces_of_no_extCap (enc : Encoding BaseOpsSig.none RunSig)
    (hcap : enc.extCap RunCall.drain = false) :
    RunUnderProduces BaseOpsSig.none enc := by
  obtain ⟨fin0, hrun0, _⟩ := battery_empty
  refine ⟨[ri (Caller.ext 0) RunCall.drain, rc (Caller.ext 0) RunCall.drain RunRes.rReturned],
    ⟨⟨fin0, hrun0⟩, seqOK_b6⟩, ?_⟩
  rintro ⟨hrun, -⟩
  obtain ⟨fin, hrun⟩ :
      ∃ fin, EncRuns BaseOpsSig.none RunSig enc
        (encInit BaseOpsSig.none RunSig) _ fin := hrun
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := sysRuns_cons hrun _ _ rfl
  have hcap' := drainCap_true_of_drainIssue hstep rfl rfl rfl
  simp [hcap] at hcap'

/-- The Stage-8 capability record: the two class conditionals the
frozen method supports.  This is RESEARCH/DEFER territory, not
THEOREM B. -/
theorem run_capability_defer :
    (∀ enc : Encoding BaseOpsSig.none RunSig,
        enc.extCap RunCall.work = true →
          RunOverProduces BaseOpsSig.none enc) ∧
    (∀ enc : Encoding BaseOpsSig.none RunSig,
        enc.extCap RunCall.drain = false →
          RunUnderProduces BaseOpsSig.none enc) :=
  ⟨run_over_produces_of_extCap, run_under_produces_of_no_extCap⟩

/-- Non-vacuity instance for the under-production conditional. -/
def encNoDrain : Encoding BaseOpsSig.none RunSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := fun _ => false

theorem encNoDrain_under : RunUnderProduces BaseOpsSig.none encNoDrain :=
  run_under_produces_of_no_extCap encNoDrain rfl

/-- Non-vacuity instance for the over-production conditional. -/
def encExtWork : Encoding BaseOpsSig.none RunSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := fun c =>
    match c with
    | RunCall.work => true
    | _ => false

theorem encExtWork_over : RunOverProduces BaseOpsSig.none encExtWork :=
  run_over_produces_of_extCap encExtWork rfl

/-! ## The mutant battery

Five hand-written faults, one per independent discipline the model
carries.  M1 and M2 are SAFETY kills (a state invariant breaks); M3
and M5 are RESULT-SEMANTICS kills (the state stays safe; the public
result contradicts the outcome authority or its backing effect); M4 is
a TRACE-REMOVAL kill (every invariant still holds — every other facet
is the model's — but the work-issue witness becomes unreachable; the
mutant machine's language is defined below and the TLA separation
configuration carries that formal certificate). -/

/-- Silent runs emit no observations: only the two spawn-section
constructors are silent.  With no in-flight spawn record at all, a
silent run cannot move. -/
theorem silent_run_nil {a b : RunCfg} {t : Trace RunSig} (hr : RunRuns a t b) :
    t = [] → a.exts = [] → b = a := by
  induction hr with
  | stop cfg => intro _ _; rfl
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro ht he0
      cases o with
      | none =>
          have ht2 : t2 = [] := by simpa [Option.toList] using ht
          cases hstep with
          | extSpawnEffectIn e rest heq hres hdrain =>
              exfalso
              rw [he0] at heq
              exact absurd heq (by simp)
          | extSpawnEffectOut e rest heq hres hdrain =>
              exfalso
              rw [he0] at heq
              exact absurd heq (by simp)
      | some ob =>
          exfalso
          have hL := congrArg List.length ht
          simp only [Option.toList_some, List.length_cons, List.length_append,
            List.length_nil] at hL
          omega

/-- With a completed spawn record (its result fixed), a silent run
cannot move: the spawn section ran at most once. -/
theorem silent_run_some {a b : RunCfg} {t : Trace RunSig} (hr : RunRuns a t b) :
    t = [] → a.exts = [(⟨0, RunCall.spawn, some RunRes.rSpawn⟩ : ExtPend RunSig)] → b = a := by
  induction hr with
  | stop cfg => intro _ _; rfl
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro ht he0
      cases o with
      | none =>
          have ht2 : t2 = [] := by simpa [Option.toList] using ht
          cases hstep with
          | extSpawnEffectIn e rest heq hres hdrain =>
              exfalso
              rw [he0] at heq
              cases rest with
              | nil =>
                  injection heq with hE1 hE2
                  cases hE1
                  exact absurd hres (by simp)
              | cons r rs =>
                  injection heq with hE1 hE2
                  have hL := congrArg List.length hE2
                  simp at hL
          | extSpawnEffectOut e rest heq hres hdrain =>
              exfalso
              rw [he0] at heq
              cases rest with
              | nil =>
                  injection heq with hE1 hE2
                  cases hE1
                  exact absurd hres (by simp)
              | cons r rs =>
                  injection heq with hE1 hE2
                  have hL := congrArg List.length hE2
                  simp at hL
      | some ob =>
          exfalso
          have hL := congrArg List.length ht
          simp only [Option.toList_some, List.length_cons, List.length_append,
            List.length_nil] at hL
          omega

/-- With an entered spawn record (its section pending) and a drain in
flight, a silent run either does nothing or runs the routed spawn
section: the task is minted and lands on the backlog tail. -/
theorem silent_run_none {a b : RunCfg} {t : Trace RunSig} (hr : RunRuns a t b) :
    t = [] →
    a.exts = [(⟨0, RunCall.spawn, none⟩ : ExtPend RunSig)] →
    a.prim.drainCaller.isSome = true →
    b = a ∨
      (b.runq = a.runq ++ [a.nextFiber] ∧
       b.nextFiber = a.nextFiber + 1 ∧
       b.prim.pending = a.prim.pending ∧
       b.exts = [(⟨0, RunCall.spawn, some RunRes.rSpawn⟩ : ExtPend RunSig)]) := by
  induction hr with
  | stop cfg => intro _ _ _; exact Or.inl rfl
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro ht he0 hdr
      cases o with
      | none =>
          have ht2 : t2 = [] := by simpa [Option.toList] using ht
          cases hstep with
          | extSpawnEffectIn e rest heq hres hdrain =>
              rw [he0] at heq
              cases rest with
              | nil =>
                  injection heq with hE1 hE2
                  cases hE1
                  have hfin := silent_run_some hrest ht2 rfl
                  subst hfin
                  refine Or.inr ⟨rfl, rfl, rfl, ?_⟩
                  simp only [List.nil_append]
              | cons r rs =>
                  exfalso
                  injection heq with hE1 hE2
                  have hL := congrArg List.length hE2
                  simp at hL
          | extSpawnEffectOut e rest heq hres hdrain =>
              exfalso
              rw [hdrain] at hdr
              exact absurd hdr (by simp)
      | some ob =>
          exfalso
          have hL := congrArg List.length ht
          simp only [Option.toList_some, List.length_cons, List.length_append,
            List.length_nil] at hL
          omega

/-- M1 (SAFETY — the quiescence gate): the drain returns while a task
is still on the worker.  The terminate flag commits with the worker
occupied — the terminate-quiescence conjunct breaks. -/
abbrev rsM1 : RunCfg :=
  { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [], retired := [], nextFiber := 1, exts := [] }

theorem runM1_breaks :
    runSafe rsM1 ∧
    ¬ runSafe { rsM1 with prim := { rsM1.prim with term := true, drainCaller := none } } := by
  refine ⟨?_, ?_⟩
  · refine ⟨rfl, ?_, ?_, ?_, ?_, ?_, ?_, ?_, ?_⟩
    · intro f hf; cases hf
    · intro f hf; cases hf
    · intro f hf; cases hf
    · intro s hs
      injection hs with h1
      subst h1
      simp only [FSlot.fiber]
      exact by decide
    · intro _; rfl
    · intro hnone; exact absurd hnone (by simp)
    · intro ht; exact Bool.noConfusion ht
    · intro e he; cases he
  · intro h
    dsimp only [] at h
    have hc := h.2.2.2.2.2.2.2.1 rfl
    exact absurd hc.2.2 (by simp)

/-- M2 (SAFETY — duplicate dequeue): dispatch without the worker-free
gate — a second task is popped onto an occupied worker and the first
vanishes from the accounting. -/
abbrev rsM2 : RunCfg :=
  { prim := ⟨false, some 0, []⟩, cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := [1], retired := [], nextFiber := 2, exts := [] }

theorem runM2_breaks :
    runSafe rsM2 ∧
    ¬ runSafe { rsM2 with
                cur := some (FSlot.running { fiber := 1, call := RunCall.work } false),
                runq := [] } := by
  refine ⟨?_, ?_⟩
  · refine ⟨rfl, ?_, ?_, ?_, ?_, ?_, ?_, ?_, ?_⟩
    · intro f hf
      rcases List.mem_cons.mp hf with hm | hm
      · subst hm; exact by decide
      · cases hm
    · intro f hf; cases hf
    · intro f hf; cases hf
    · intro s hs
      injection hs with h1
      subst h1
      simp only [FSlot.fiber]
      exact by decide
    · intro _; rfl
    · intro hnone; exact absurd hnone (by simp)
    · intro ht; exact Bool.noConfusion ht
    · intro e he; cases he
  · intro h
    have hcnt := h.1
    simp at hcnt

/-- M3 (RESULT-SEMANTICS — the drain's return value): the run returns
the task's result instead of `returned`.  The state stays safe; the
public result contradicts the outcome authority — the model's only
drain completion carries `rReturned`. -/
abbrev rsM3 : RunCfg :=
  { prim := ⟨false, some 0, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }

abbrev rsM3end : RunCfg :=
  { prim := ⟨true, none, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [] }

def runDrainM3 (cfg : RunCfg) (x : ExternalId)
    (_hdrain : cfg.prim.drainCaller = some x) (_hrunq : cfg.runq = []) (_hcur : cfg.cur = none) :
    Option (Obs RunSig) × RunCfg :=
  (some (rc (Caller.ext x) RunCall.drain RunRes.rDone),
   { cfg with prim := { cfg.prim with term := true, drainCaller := none } })

theorem runM3_wrong_result :
    runDrainM3 rsM3 0 rfl rfl rfl = (some (rc (Caller.ext 0) RunCall.drain RunRes.rDone), rsM3end) ∧
    runSafe rsM3 ∧
    runSafe rsM3end ∧
    ¬ TracesRun [ri (Caller.ext 0) RunCall.drain, rc (Caller.ext 0) RunCall.drain RunRes.rDone] := by
  refine ⟨rfl, ?_, ?_, ?_⟩
  · refine ⟨rfl, ?_, ?_, ?_, ?_, ?_, ?_, ?_, ?_⟩
    · intro f hf; cases hf
    · intro f hf; cases hf
    · intro f hf; cases hf
    · intro s hs; cases hs
    · intro hc; exact Bool.noConfusion hc
    · intro hnone; exact absurd hnone (by simp)
    · intro ht; exact Bool.noConfusion ht
    · intro e he; cases he
  · refine ⟨rfl, ?_, ?_, ?_, ?_, ?_, ?_, ?_, ?_⟩
    · intro f hf; cases hf
    · intro f hf; cases hf
    · intro f hf; cases hf
    · intro s hs; cases hs
    · intro hc; exact Bool.noConfusion hc
    · intro _; rfl
    · intro ht
      exact ⟨rfl, rfl, rfl⟩
    · intro e he; cases he
  · rintro ⟨fin, hrun⟩
    obtain ⟨ob1, t1', heq1⟩ :
        ∃ ob t1', [ri (Caller.ext 0) RunCall.drain,
                  rc (Caller.ext 0) RunCall.drain RunRes.rDone] = ob :: t1' :=
      ⟨_, _, rfl⟩
    obtain ⟨m1, m2, h1, hstep1, hrest1⟩ := runRuns_cons hrun ob1 t1' heq1
    cases hstep1 with
    | extSpawnApply x hfresh =>
        injection heq1 with hE1 hE2
        have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hE1
        exact absurd hE (by simp)
    | extSpawnDone e rest heq' hres =>
        injection heq1 with hE1 hE2
        have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hE1
        exact absurd hE (by simp)
    | dispatch f rest hrunq hcur hdrain =>
        injection heq1 with hE1 hE2
        have hE : RunCall.drain = RunCall.work := congrArg Obs.call hE1
        exact absurd hE (by simp)
    | workDone d hcur =>
        injection heq1 with hE1 hE2
        have hE : RunCall.drain = RunCall.work := congrArg Obs.call hE1
        exact absurd hE (by simp)
    | drainReturn x hdrain hrunq hcur =>
        injection heq1 with hE1 hE2
        have hE : Option.none = Option.some RunRes.rReturned := congrArg Obs.result hE1
        exact absurd hE (by simp)
    | drainEnter y hnone =>
        injection heq1 with _ hE2
        subst hE2
        obtain ⟨ob2, t2', heq2⟩ :
            ∃ ob t2', [rc (Caller.ext 0) RunCall.drain RunRes.rDone] = ob :: t2' :=
          ⟨_, _, rfl⟩
        obtain ⟨m3, m4, h2, hstep2, hrest2⟩ := runRuns_cons hrest1 ob2 t2' heq2
        cases hstep2 with
        | extSpawnApply x hfresh =>
            injection heq2 with hE3 hE4
            have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hE3
            exact absurd hE (by simp)
        | extSpawnDone e rest heq' hres =>
            injection heq2 with hE3 hE4
            have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hE3
            exact absurd hE (by simp)
        | dispatch f rest hrunq hcur hdrain =>
            injection heq2 with hE3 hE4
            have hE : RunCall.drain = RunCall.work := congrArg Obs.call hE3
            exact absurd hE (by simp)
        | workDone d hcur =>
            injection heq2 with hE3 hE4
            have hE : RunCall.drain = RunCall.work := congrArg Obs.call hE3
            exact absurd hE (by simp)
        | drainEnter z hnone2 =>
            injection heq2 with hE3 hE4
            have hE : Option.some RunRes.rDone = Option.none := congrArg Obs.result hE3
            exact absurd hE (by simp)
        | drainReturn z hdrz hrq hcz =>
            injection heq2 with hE3 hE4
            have hE : Option.some RunRes.rDone = Option.some RunRes.rReturned :=
              congrArg Obs.result hE3
            exact absurd hE (by simp)

/- M4 (TRACE-REMOVAL): the dispatched task's issue observation dropped —
dispatch goes silent.  Every state invariant still holds (every other
facet is the model's), but the task's issue observation is unreachable:
no other step emits a `work` issue.  The Lean side carries the
definitional removal; the TLA separation configuration carries the
formal trace-removal certificate. -/

/-- The mutant's dispatch: the same state effect, no observation. -/
def runDispatchM4 (cfg : RunCfg) (f : FiberId) (rest : List FiberId)
    (_hrunq : cfg.runq = f :: rest) (_hcur : cfg.cur = none)
    (_hdrain : cfg.prim.drainCaller.isSome = true) :
    Option (Obs RunSig) × RunCfg :=
  (none, { cfg with cur := some (FSlot.running { fiber := f, call := RunCall.work } false), runq := rest })

/-- A backlog with one queued task and a run in flight: dispatch fires. -/
abbrev rsM4cfg : RunCfg :=
  { prim := ⟨false, some 0, []⟩, cur := none, runq := [0], retired := [], nextFiber := 1, exts := [] }

theorem runM4_removes :
    RunStep rsM4cfg (some (issueObs RunSig (Caller.fiber 0) RunCall.work))
      { rsM4cfg with cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := ([] : List FiberId) } ∧
    runDispatchM4 rsM4cfg 0 [] rfl rfl rfl
      = (none, { rsM4cfg with cur := some (FSlot.running { fiber := 0, call := RunCall.work } false), runq := ([] : List FiberId) }) :=
  ⟨RunStep.dispatch rsM4cfg 0 [] rfl rfl rfl, rfl⟩

/- M5 (RESULT-BINDING — the spawn section's mint): the section fixes the
spawn's result without minting the task fiber.  The returned `rSpawn` is
then unbacked — no task exists behind it — and the drain's quiescence
gate is satisfied with the model's minted task vaporized.  The Lean side
carries the definitional removal and the model-side separation; the TLA
separation configuration carries the mutant's reachability. -/

/-- The mutant's spawn section with a run in flight: the result is
fixed, no task is minted. -/
def runEffectM5 (cfg : RunCfg) (e : ExtPend RunSig) (rest : List (ExtPend RunSig))
    (_heq : cfg.exts = rest ++ [e]) (_hres : e.result = none)
    (_hdrain : cfg.prim.drainCaller.isSome = true) : RunCfg :=
  { cfg with exts := rest ++ [{ e with result := some RunRes.rSpawn }] }

/-- Drain issued, spawn record entered, backlog empty: the model's
section mints the task onto the backlog; the mutant's does not. -/
abbrev rsM5cfg : RunCfg :=
  { prim := ⟨false, some 1, []⟩, cur := none, runq := [], retired := [], nextFiber := 0, exts := [(⟨0, RunCall.spawn, none⟩ : ExtPend RunSig)] }

theorem runM5_unbacked :
    (∃ cfg' : RunCfg,
        RunStep rsM5cfg none cfg' ∧
        cfg'.runq = [(0 : FiberId)] ∧ cfg'.nextFiber = 1 ∧
        cfg'.exts = [(⟨0, RunCall.spawn, some RunRes.rSpawn⟩ : ExtPend RunSig)]) ∧
    (runEffectM5 rsM5cfg (⟨0, RunCall.spawn, none⟩ : ExtPend RunSig) [] rfl rfl rfl).runq
      = ([] : List FiberId) ∧
    (runEffectM5 rsM5cfg (⟨0, RunCall.spawn, none⟩ : ExtPend RunSig) [] rfl rfl rfl).nextFiber
      = 0 ∧
    ¬ TracesRun [ri (Caller.ext 1) RunCall.drain,
                 ri (Caller.ext 0) RunCall.spawn,
                 rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn,
                 rc (Caller.ext 1) RunCall.drain RunRes.rReturned] := by
  refine ⟨⟨_, RunStep.extSpawnEffectIn rsM5cfg _ [] rfl rfl rfl, rfl, rfl, rfl⟩, rfl, rfl, ?_⟩
  rintro ⟨fin, hrun⟩
  obtain ⟨ob1, t1, heq1⟩ :
      ∃ ob t1, [ri (Caller.ext 1) RunCall.drain,
                ri (Caller.ext 0) RunCall.spawn,
                rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn,
                rc (Caller.ext 1) RunCall.drain RunRes.rReturned] = ob :: t1 :=
    ⟨_, _, rfl⟩
  obtain ⟨m1, m2, h1, hstep1, hrest1⟩ := runRuns_cons hrun ob1 t1 heq1
  cases hstep1 with
  | extSpawnApply x hfresh =>
      injection heq1 with hE1 hE2
      have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hE1
      exact absurd hE (by simp)
  | extSpawnDone e rest heq' hres =>
      injection heq1 with hE1 hE2
      have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hE1
      exact absurd hE (by simp)
  | dispatch f rest hrunq hcur hdrain =>
      injection heq1 with hE1 hE2
      have hE : RunCall.drain = RunCall.work := congrArg Obs.call hE1
      exact absurd hE (by simp)
  | workDone d hcur =>
      injection heq1 with hE1 hE2
      have hE : RunCall.drain = RunCall.work := congrArg Obs.call hE1
      exact absurd hE (by simp)
  | drainReturn x hdrain hrunq hcur =>
      injection heq1 with hE1 hE2
      have hE : Option.none = Option.some RunRes.rReturned := congrArg Obs.result hE1
      exact absurd hE (by simp)
  | drainEnter x hnone =>
      injection heq1 with _ hE2
      subst hE2
      have hEq1 : m1 = runInit := silent_run_nil h1 rfl rfl
      subst hEq1
      obtain ⟨ob2, t2, heq2⟩ :
          ∃ ob t2, [ri (Caller.ext 0) RunCall.spawn,
                    rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn,
                    rc (Caller.ext 1) RunCall.drain RunRes.rReturned] = ob :: t2 :=
        ⟨_, _, rfl⟩
      obtain ⟨m3, m4, h2, hstep2, hrest2⟩ := runRuns_cons hrest1 ob2 t2 heq2
      have hEq3 := silent_run_nil h2 rfl rfl
      subst hEq3
      cases hstep2 with
      | extSpawnDone e rest heq' hres =>
          injection heq2 with hF1 hF2
          have hE : Option.none = Option.some RunRes.rSpawn := congrArg Obs.result hF1
          exact absurd hE (by simp)
      | dispatch f rest hrunq hcur hdrain =>
          injection heq2 with hF1 hF2
          have hE : RunCall.spawn = RunCall.work := congrArg Obs.call hF1
          exact absurd hE (by simp)
      | workDone d hcur =>
          injection heq2 with hF1 hF2
          have hE : RunCall.spawn = RunCall.work := congrArg Obs.call hF1
          exact absurd hE (by simp)
      | drainEnter y hnone2 =>
          injection heq2 with hF1 hF2
          have hE : RunCall.spawn = RunCall.drain := congrArg Obs.call hF1
          exact absurd hE (by simp)
      | drainReturn y hdrain hrunq hcur =>
          injection heq2 with hF1 hF2
          have hE : RunCall.spawn = RunCall.drain := congrArg Obs.call hF1
          exact absurd hE (by simp)
      | extSpawnApply x' hfresh =>
          injection heq2 with hF1 hF2
          have hF1' : Caller.ext 0 = Caller.ext x' := congrArg Obs.caller hF1
          injection hF1' with hx0
          subst hx0
          subst hF2
          obtain ⟨ob3, t3, heq3⟩ :
              ∃ ob t3, [rc (Caller.ext 0) RunCall.spawn RunRes.rSpawn,
                        rc (Caller.ext 1) RunCall.drain RunRes.rReturned] = ob :: t3 :=
            ⟨_, _, rfl⟩
          obtain ⟨mA, mB, h3, hstep3, hrest3⟩ := runRuns_cons hrest2 ob3 t3 heq3
          rcases silent_run_none h3 rfl rfl rfl with hEq5 | ⟨hrq5, hnf5, hpd5, hex5⟩
          · subst hEq5
            cases hstep3 with
            | extSpawnApply x2 hfresh =>
                injection heq3 with hG1 hG2
                have hE : Option.some RunRes.rSpawn = Option.none := congrArg Obs.result hG1
                exact absurd hE (by simp)
            | dispatch f rest hrunq hcur hdrain =>
                injection heq3 with hG1 hG2
                have hE : RunCall.spawn = RunCall.work := congrArg Obs.call hG1
                exact absurd hE (by simp)
            | workDone d hcur =>
                injection heq3 with hG1 hG2
                have hE : RunCall.spawn = RunCall.work := congrArg Obs.call hG1
                exact absurd hE (by simp)
            | drainEnter y hnone2 =>
                injection heq3 with hG1 hG2
                have hE : RunCall.spawn = RunCall.drain := congrArg Obs.call hG1
                exact absurd hE (by simp)
            | drainReturn y hdrain hrunq hcur =>
                injection heq3 with hG1 hG2
                have hE : Option.some RunRes.rSpawn = Option.some RunRes.rReturned :=
                  congrArg Obs.result hG1
                exact absurd hE (by simp)
            | extSpawnDone e rest heq' hres =>
                simp only [runInit, List.nil_append] at heq'
                cases rest with
                | nil =>
                    simp only [List.nil_append] at heq'
                    injection heq' with hG3 hG4
                    rw [← hG3] at hres
                    simp at hres
                | cons r rs =>
                    exfalso
                    injection heq' with hG3 hG4
                    have hL := congrArg List.length hG4
                    simp at hL
          · cases hstep3 with
            | extSpawnApply x2 hfresh =>
                injection heq3 with hG1 hG2
                have hE : Option.some RunRes.rSpawn = Option.none := congrArg Obs.result hG1
                exact absurd hE (by simp)
            | dispatch f rest hrunq hcur hdrain =>
                injection heq3 with hG1 hG2
                have hE : RunCall.spawn = RunCall.work := congrArg Obs.call hG1
                exact absurd hE (by simp)
            | workDone d hcur =>
                injection heq3 with hG1 hG2
                have hE : RunCall.spawn = RunCall.work := congrArg Obs.call hG1
                exact absurd hE (by simp)
            | drainEnter y hnone2 =>
                injection heq3 with hG1 hG2
                have hE : RunCall.spawn = RunCall.drain := congrArg Obs.call hG1
                exact absurd hE (by simp)
            | drainReturn y hdrain hrunq hcur =>
                injection heq3 with hG1 hG2
                have hE : Option.some RunRes.rSpawn = Option.some RunRes.rReturned :=
                  congrArg Obs.result hG1
                exact absurd hE (by simp)
            | extSpawnDone e rest heq' hres =>
                rw [hex5] at heq'
                cases rest with
                | nil =>
                    injection heq3 with _ hG2
                    subst hG2
                    obtain ⟨ob4, t4, heq4⟩ :
                        ∃ ob t4, [rc (Caller.ext 1) RunCall.drain RunRes.rReturned] = ob :: t4 :=
                      ⟨_, _, rfl⟩
                    obtain ⟨mC, mD, h4, hstep4, hrest4⟩ := runRuns_cons hrest3 ob4 t4 heq4
                    have hEqC := silent_run_nil h4 rfl rfl
                    rw [hEqC] at hstep4
                    have hA0 : mA.runq = [(0 : FiberId)] := by rw [hrq5]; rfl
                    cases hstep4 with
                    | extSpawnApply x2 hfresh =>
                        injection heq4 with hI1 hI2
                        have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hI1
                        exact absurd hE (by simp)
                    | extSpawnDone e2 rest2 heq2' hres2 =>
                        injection heq4 with hI1 hI2
                        have hE : RunCall.drain = RunCall.spawn := congrArg Obs.call hI1
                        exact absurd hE (by simp)
                    | dispatch f rest2 hrunq hcur hdrain =>
                        injection heq4 with hI1 hI2
                        have hE : RunCall.drain = RunCall.work := congrArg Obs.call hI1
                        exact absurd hE (by simp)
                    | workDone d hcur =>
                        injection heq4 with hI1 hI2
                        have hE : RunCall.drain = RunCall.work := congrArg Obs.call hI1
                        exact absurd hE (by simp)
                    | drainEnter y hnone2 =>
                        injection heq4 with hI1 hI2
                        have hE : Option.some RunRes.rReturned = Option.none :=
                          congrArg Obs.result hI1
                        exact absurd hE (by simp)
                    | drainReturn x2 hdrain hrunq hcur =>
                        have hrunq' : mA.runq = [] := hrunq
                        rw [hA0] at hrunq'
                        exact absurd hrunq' (by simp)
                | cons r rs =>
                    exfalso
                    injection heq' with hG3 hG4
                    have hL := congrArg List.length hG4
                    simp at hL

end Sluice.Formal
