/-
Sluice Stage 3V2.3 — AsyncMutex core (FCB1-METHOD-CORRECTIVE-1).

This stage re-adjudicates the AsyncMutex core on the post-#378 Stage-0-V2.3
authority (`CalcV2.lean`, `JudgeV2.lean`), from the current production code:

  `include/sluice/async/async_mutex.hpp`, `src/async/scheduler_mutex.cpp`.

Call-domain census (from the code):

  `try_lock`  fiber-bound (`mutex_try_lock` reads `g_worker`/`ws->current`,
              scheduler_mutex.cpp:22-24); bool result.
  `lock`      fiber-bound (`mutex_lock` :38-40, parks via
              `commit_suspend_locked` :70 and `context_switch` :76-79); the
              inline grant runs exactly when the fresh node lands at the
              queue head and the owner slot is free (:51-58).
  `unlock`    fiber-bound (`mutex_unlock` dereferences `g_worker` :184-186);
              owner-gated caller precondition (:188); handoff to the queue
              head (`mutex_handoff_one_locked` :158-181) or free.
  `cancel`    external-capable (`mutex_cancel` :145-156 takes `global_mtx_`
              and the queue mutex only, no `g_worker`); bool result; the
              cancelled locker is published and its `lock` call returns.
  `lock_until` fiber-bound timed extension, outside the core surface
              (same disposition as Semaphore's `acquire_until`).

Modeling disclosures (each preserves the reachable trace language):

  * The modeled `lock` result is the `WaitNode` outcome the caller can
    inspect after the call returns (`node.was_woken()` / `was_cancelled()`,
    wait_node.hpp): `true` = granted (inline or handed off), `false` =
    cancelled.  Without that bit the completion surface cannot distinguish
    a granted `lock` from a cancelled one and the ownership accounting is
    not statable.
  * `mutexPark` refuses a duplicate registration and `mutexRun` routes a
    lock whose caller already owns the mutex to the completion path (the
    handed-off winner's resume).  Both guards sit on transitions the frozen
    machine cannot reach for this primitive (one in-flight call per fiber;
    a winner's owner is already recorded), so no reachable trace changes;
    they make `waitq` exactly the set the code's `WaitQueue` is (one
    registration per fiber).  As in the semaphore stage, the calculus's
    `runPark` is unguarded on the resumed bit, so a resumed *cancelled*
    locker may re-run its inline paths in this model — an
    over-approximation confined to that corner; the TLA mirror
    (`MutexCore.tla`) has no such step and the stage card discloses the
    difference.

Adjudication:

  * AsyncMutex — **THEOREM B** (`mutex_irreducible`): irreducible to
    `BASE(AsyncMutex) = {Semaphore}`.  The separating fact is the
    owner-gated *admission* of `unlock`: a non-owner `unlock` is a caller
    precondition violation (the `assert` at scheduler_mutex.cpp:188), so
    the primitive never issues it — `mutexAdmit` refuses the dispatch —
    while every encoding's fiber machine dispatches any submitted call
    unconditionally.  The witness `unlockIssueTrace` (a bare `unlock`
    issue by a fresh fiber) is produced by every encoding and by no run
    of the primitive (`mutex_over_produces`, `mutex_not_unlockIssue`).
    The old #379 THEOREM B rested on the V1 completion shadow, which the
    V2 calculus refutes; nothing is inherited.
  * LockGuard's THEOREM A over the synchronous `Mutex` (the freeze's
    mandated first nonempty-`BASE(P)` re-export test) is a charter
    obligation still open; this stage neither relies on it nor delivers
    it.

Safety evidence: `mutexPrim_guarantees` proves the mutual-exclusion half
in Lean — every completed grant completes only when the completed
releases have caught up, and the global prefix bound
`grantsAll ≤ releasesAll + 1` follows since the counts move only at
completions.  The release-side ownership bounds (no `unlock` without a
backing grant, one owner) are the TLA mirror's invariants
(`MutexCore.tla`), which the stage card adjudicates against this
calculus.  The conservation mirror `mutex_mirror` is the Lean-side
ledger: per fiber and globally, trace grants plus in-window credit
equal trace releases plus the owner slot, at every run endpoint.
-/

import Sluice.Formal.JudgeV2
import Sluice.Formal.SemV2

namespace Sluice.Formal

/-! ## API and state -/

inductive MutexCall : Type where
  | mlock
  | munlock
  | mtrylock
  | mcancel (f : FiberId)
deriving instance DecidableEq for MutexCall

inductive MutexRes : Type where
  | runit
  | rbool (b : Bool)
deriving instance DecidableEq for MutexRes

abbrev MutexSig : ApiSig := ⟨MutexCall, MutexRes⟩

/-- AsyncMutex private state: the owner slot (`owner_`, async_mutex.hpp:45),
the waiter FIFO (`waiters_`, head = queue head), and the resolved-but-
unconsumed outcomes of published winners (the one-shot `WaitNode::resolve_`
state of a handed-off or cancelled locker awaiting its resume). -/
structure MutexState where
  owner : Option FiberId
  waitq : List FiberId
  resolved : List (FiberId × Bool)

def mutexInit : MutexState := ⟨none, [], []⟩

/-- Remove the first occurrence of `w` (the `WaitQueue::unlink_locked`
shape). -/
def removeFiber : List FiberId → FiberId → List FiberId
  | [], _ => []
  | x :: xs, w => if x = w then xs else x :: removeFiber xs w

/-- Remove the first record of `f`, yielding its outcome (the one-shot
consume of a resolved `WaitNode` at the resumed locker's finish). -/
def consumeRec : List (FiberId × Bool) → FiberId → Option (Bool × List (FiberId × Bool))
  | [], _ => none
  | (g, b) :: rest, f => if g = f then some (b, rest)
      else (fun p => (p.1, (g, b) :: p.2)) <$> consumeRec rest f

/-! ## The primitive -/

/-- Entry gate.  The recursive `lock` and the non-owner `unlock` are caller
precondition violations (the asserts at scheduler_mutex.cpp:41 and :188):
such a call never legally enters execution, so the dispatch is refused.
`try_lock` and `cancel` are always admissible (scheduler_mutex.cpp:27
makes a recursive attempt a plain `false`). -/
def mutexAdmit : MutexState → FiberId → MutexCall → Option MutexState
  | s, f, MutexCall.mlock => if s.owner = some f then none else some s
  | s, f, MutexCall.munlock => if s.owner = some f then some s else none
  | s, _, MutexCall.mtrylock => some s
  | s, _, MutexCall.mcancel _ => some s

/-- The inline paths of a dispatched call.

  `lock` — three shapes (scheduler_mutex.cpp:46-70): the inline grant when
  the fresh node lands at the head of an empty queue with the owner slot
  free (:51-58); the handed-off winner's resume completes with no further
  state change (its grant was applied by the unlocker's section,
  :167-179); anything else suspends (:70, :76-79).

  `unlock` — handoff to the queue head or free (:158-181, :192-196).

  `try_lock` — grants only when free and no waiter is queued (:30-33); a
  recursive attempt is a plain `false` (:27-29). -/
def mutexRun : MutexState → Tick → FiberId → MutexCall →
    Option (MutexRes × MutexState × List FiberId)
  | s, _, f, MutexCall.mlock =>
      if s.owner = none ∧ s.waitq = [] then
        some (MutexRes.rbool true, { owner := some f, waitq := [], resolved := s.resolved }, [])
      else if s.owner = some f then some (MutexRes.rbool true, s, [])
      else none
  | s, _, _, MutexCall.munlock =>
      match s.waitq with
      | [] => some (MutexRes.runit, { owner := none, waitq := [], resolved := s.resolved }, [])
      | w :: tl =>
          some (MutexRes.runit, { owner := some w, waitq := tl, resolved := s.resolved ++ [(w, true)] }, [w])
  | s, _, f, MutexCall.mtrylock =>
      if s.owner = none ∧ s.waitq = [] then
        some (MutexRes.rbool true, { owner := some f, waitq := [], resolved := s.resolved }, [])
      else some (MutexRes.rbool false, s, [])
  | s, _, _, MutexCall.mcancel _ => none

/-- Only `lock` suspends; parking appends the caller at the queue tail
(`register_wait_locked` + `commit_suspend_locked`, scheduler_mutex.cpp:46,
:70).  A fiber already parked is not re-registered (the queue holds one
node per fiber; see the modeling disclosures).  The mutex has no
pre-suspension wakes: its handoff happens in the *waker's* section. -/
def mutexPark : MutexState → FiberId → MutexCall →
    Option (MutexState × List FiberId)
  | s, f, MutexCall.mlock =>
      if f ∈ s.waitq then none
      else some ({ owner := s.owner, waitq := s.waitq ++ [f], resolved := s.resolved }, [])
  | _, _, _ => none

/-- A resumed `lock` consumes its recorded outcome (the node's one-shot
`resolve_` result); nothing else resumes. -/
def mutexFinish : MutexState → FiberId → MutexCall →
    Option (MutexRes × MutexState × List FiberId)
  | s, f, MutexCall.mlock =>
      match consumeRec s.resolved f with
      | some (b, rest) => some (MutexRes.rbool b, { owner := s.owner, waitq := s.waitq, resolved := rest }, [])
      | none => none
  | _, _, _ => none

/-- Only `cancel` is external-capable (census above). -/
def mutexExtCap : MutexCall → Bool
  | MutexCall.mcancel _ => true
  | _ => false

/-- The external critical section of `cancel`: a parked locker is resolved
`cancelled` and published (`cancel_primitive_wait_locked` +
`publish_wait_winner_locked`, scheduler_mutex.cpp:149-155); a non-waiter
yields `false`. -/
def mutexExtRun : MutexCall → MutexState → Tick →
    Option (MutexRes × MutexState × List FiberId)
  | MutexCall.mcancel w, s, _ =>
      if w ∈ s.waitq then
        some (MutexRes.rbool true,
          { owner := s.owner, waitq := removeFiber s.waitq w, resolved := s.resolved ++ [(w, false)] }, [w])
      else some (MutexRes.rbool false, s, [])
  | _, _, _ => none

/-- The primitive, reducible so its `State` field unfolds at instances
transparency inside simp instantiations. -/
@[reducible] def mutexPrim : PrimLTS2 MutexSig :=
  { State := MutexState
    init := mutexInit
    admit := mutexAdmit
    run := mutexRun
    park := mutexPark
    finish := mutexFinish
    extCap := mutexExtCap
    extRun := mutexExtRun
    onTick := fun s _ => s
    expire := fun _ _ _ => none }

/-! ## Trace counters -/

/-- A completed grant: a `lock` or `try_lock` completing `true`. -/
def isGrant (o : Obs MutexSig) : Bool :=
  match o.call, o.result with
  | MutexCall.mlock, some (MutexRes.rbool true) => true
  | MutexCall.mtrylock, some (MutexRes.rbool true) => true
  | _, _ => false

/-- A completed `unlock`. -/
def isRelease (o : Obs MutexSig) : Bool :=
  match o.call, o.result with
  | MutexCall.munlock, some MutexRes.runit => true
  | _, _ => false

def grantsOf (f : FiberId) (t : Trace MutexSig) : Nat :=
  (t.filter (fun o => isGrant o && o.caller = Caller.fiber f)).length

def releasesOf (f : FiberId) (t : Trace MutexSig) : Nat :=
  (t.filter (fun o => isRelease o && o.caller = Caller.fiber f)).length

def grantsAll (t : Trace MutexSig) : Nat := (t.filter isGrant).length

def releasesAll (t : Trace MutexSig) : Nat := (t.filter isRelease).length

@[simp] theorem grantsAll_nil : grantsAll [] = 0 := rfl

@[simp] theorem releasesAll_nil : releasesAll [] = 0 := rfl

@[simp] theorem grantsOf_nil (f : FiberId) : grantsOf f [] = 0 := rfl

@[simp] theorem releasesOf_nil (f : FiberId) : releasesOf f [] = 0 := rfl

@[simp] theorem grantsAll_issue (cl : Caller) (c : MutexCall) (t : Trace MutexSig) :
    grantsAll (issueObs MutexSig cl c :: t) = grantsAll t := by
  simp [grantsAll, isGrant, issueObs]

@[simp] theorem releasesAll_issue (cl : Caller) (c : MutexCall) (t : Trace MutexSig) :
    releasesAll (issueObs MutexSig cl c :: t) = releasesAll t := by
  simp [releasesAll, isRelease, issueObs]

@[simp] theorem grantsOf_issue (f : FiberId) (cl : Caller) (c : MutexCall)
    (t : Trace MutexSig) :
    grantsOf f (issueObs MutexSig cl c :: t) = grantsOf f t := by
  simp [grantsOf, isGrant, issueObs]

@[simp] theorem releasesOf_issue (f : FiberId) (cl : Caller) (c : MutexCall)
    (t : Trace MutexSig) :
    releasesOf f (issueObs MutexSig cl c :: t) = releasesOf f t := by
  simp [releasesOf, isRelease, issueObs]

theorem grantsAll_comp (cl : Caller) (c : MutexCall) (r : MutexRes) (t : Trace MutexSig) :
    grantsAll (compObs MutexSig cl c r :: t) =
      grantsAll t + (if isGrant (compObs MutexSig cl c r) then 1 else 0) := by
  simp only [grantsAll, List.filter_cons, List.length_cons, compObs]
  split <;> rename_i h <;> simp [h]

theorem releasesAll_comp (cl : Caller) (c : MutexCall) (r : MutexRes) (t : Trace MutexSig) :
    releasesAll (compObs MutexSig cl c r :: t) =
      releasesAll t + (if isRelease (compObs MutexSig cl c r) then 1 else 0) := by
  simp only [releasesAll, List.filter_cons, List.length_cons, compObs]
  split <;> rename_i h <;> simp [h]

theorem grantsOf_comp (f : FiberId) (cl : Caller) (c : MutexCall) (r : MutexRes)
    (t : Trace MutexSig) :
    grantsOf f (compObs MutexSig cl c r :: t) =
      grantsOf f t +
        (if isGrant (compObs MutexSig cl c r) && decide (cl = Caller.fiber f) then 1 else 0) := by
  simp only [grantsOf, List.filter_cons, List.length_cons, compObs]
  split <;> rename_i h <;> simp [h]

theorem releasesOf_comp (f : FiberId) (cl : Caller) (c : MutexCall) (r : MutexRes)
    (t : Trace MutexSig) :
    releasesOf f (compObs MutexSig cl c r :: t) =
      releasesOf f t +
        (if isRelease (compObs MutexSig cl c r) && decide (cl = Caller.fiber f) then 1 else 0) := by
  simp only [releasesOf, List.filter_cons, List.length_cons, compObs]
  split <;> rename_i h <;> simp [h]

/-- The completion of a granted `lock`/`try_lock` by the fiber itself. -/
@[simp] theorem isGrant_true_comp (f : FiberId) :
    isGrant (compObs MutexSig (Caller.fiber f) MutexCall.mlock (MutexRes.rbool true)) = true := rfl

@[simp] theorem isGrant_trylock_true_comp (f : FiberId) :
    isGrant (compObs MutexSig (Caller.fiber f) MutexCall.mtrylock (MutexRes.rbool true)) = true := rfl

/-- The completion of an `unlock`. -/
@[simp] theorem isRelease_comp (f : FiberId) :
    isRelease (compObs MutexSig (Caller.fiber f) MutexCall.munlock MutexRes.runit) = true := rfl

/-! ## State-bit ledgers -/

/-- 1 if `f` is the recorded owner. -/
def ownerIs (f : FiberId) (s : MutexState) : Nat := if s.owner = some f then 1 else 0

/-- 1 if any fiber owns the mutex. -/
def ownerHeld (s : MutexState) : Nat := if s.owner = none then 0 else 1

/-- The in-return-window grant of `f` (inline `lock`/`try_lock` grant or a
winner completion whose physical return is pending — the V2.3 window). -/
def retGrant (f : FiberId) (d : Option (FSlot MutexSig)) : Nat :=
  match d with
  | some (FSlot.returning p (MutexRes.rbool true)) =>
      if (p.call = MutexCall.mlock ∨ p.call = MutexCall.mtrylock) ∧ p.fiber = f then 1 else 0
  | _ => 0

/-- Any in-return-window grant. -/
def retGrantAll (d : Option (FSlot MutexSig)) : Nat :=
  match d with
  | some (FSlot.returning p (MutexRes.rbool true)) =>
      if p.call = MutexCall.mlock ∨ p.call = MutexCall.mtrylock then 1 else 0
  | _ => 0

/-- The in-return-window release by `f` (the section ran, the completion
is pending).  The result condition mirrors the trace predicate exactly:
an `unlock` completion releases iff it returned `runit` (which the real
machine always does — `void AsyncMutex::unlock`). -/
def retUnlock (f : FiberId) (d : Option (FSlot MutexSig)) : Nat :=
  match d with
  | some (FSlot.returning p MutexRes.runit) =>
      if p.call = MutexCall.munlock ∧ p.fiber = f then 1 else 0
  | _ => 0

/-- Any in-return-window release. -/
def retUnlockAll (d : Option (FSlot MutexSig)) : Nat :=
  match d with
  | some (FSlot.returning p MutexRes.runit) =>
      if p.call = MutexCall.munlock then 1 else 0
  | _ => 0

/-- Pending delivered grants of `f` (true records in `resolved`: the
one-shot `WaitNode` outcomes of handed-off lockers awaiting resume). -/
def recOf (f : FiberId) (s : MutexState) : Nat :=
  (s.resolved.filter (fun r : FiberId × Bool => r.1 = f && r.2)).length

/-- All pending delivered grants. -/
def recAll (s : MutexState) : Nat :=
  (s.resolved.filter (fun r : FiberId × Bool => r.2)).length

@[simp] theorem ownerIs_init (f : FiberId) : ownerIs f mutexInit = 0 := rfl

@[simp] theorem ownerHeld_init : ownerHeld mutexInit = 0 := rfl

@[simp] theorem recOf_init (f : FiberId) : recOf f mutexInit = 0 := rfl

@[simp] theorem recAll_init : recAll mutexInit = 0 := rfl

@[simp] theorem retGrant_running (f : FiberId) (p : Pnd MutexSig) (b : Bool) :
    retGrant f (some (FSlot.running p b)) = 0 := rfl

@[simp] theorem retGrant_none (f : FiberId) : retGrant f none = 0 := rfl

@[simp] theorem retUnlock_running (f : FiberId) (p : Pnd MutexSig) (b : Bool) :
    retUnlock f (some (FSlot.running p b)) = 0 := rfl

@[simp] theorem retUnlock_none (f : FiberId) : retUnlock f none = 0 := rfl

@[simp] theorem retGrantAll_running (p : Pnd MutexSig) (b : Bool) :
    retGrantAll (some (FSlot.running p b)) = 0 := rfl

@[simp] theorem retGrantAll_none : retGrantAll none = 0 := rfl

@[simp] theorem retUnlockAll_running (p : Pnd MutexSig) (b : Bool) :
    retUnlockAll (some (FSlot.running p b)) = 0 := rfl

@[simp] theorem retUnlockAll_none : retUnlockAll none = 0 := rfl

@[simp] theorem retGrant_returning (f : FiberId) (p : Pnd MutexSig) (r : MutexRes) :
    retGrant f (some (FSlot.returning p r)) =
      if (p.call = MutexCall.mlock ∨ p.call = MutexCall.mtrylock) ∧ p.fiber = f ∧
          r = MutexRes.rbool true then 1 else 0 := by
  cases r with
  | runit => simp [retGrant]
  | rbool b => cases b <;> simp [retGrant]

@[simp] theorem retGrantAll_returning (p : Pnd MutexSig) (r : MutexRes) :
    retGrantAll (some (FSlot.returning p r)) =
      if (p.call = MutexCall.mlock ∨ p.call = MutexCall.mtrylock) ∧
          r = MutexRes.rbool true then 1 else 0 := by
  cases r with
  | runit => simp [retGrantAll]
  | rbool b => cases b <;> simp [retGrantAll]

@[simp] theorem retUnlock_returning (f : FiberId) (p : Pnd MutexSig) (r : MutexRes) :
    retUnlock f (some (FSlot.returning p r)) =
      if p.call = MutexCall.munlock ∧ p.fiber = f ∧ r = MutexRes.runit then 1 else 0 := by
  cases r with
  | runit => simp [retUnlock]
  | rbool b => cases b <;> simp [retUnlock]

@[simp] theorem retUnlockAll_returning (p : Pnd MutexSig) (r : MutexRes) :
    retUnlockAll (some (FSlot.returning p r)) =
      if p.call = MutexCall.munlock ∧ r = MutexRes.runit then 1 else 0 := by
  cases r with
  | runit => simp [retUnlockAll]
  | rbool b => cases b <;> simp [retUnlockAll]

theorem recAll_cons (s : MutexState) (f : FiberId) (b : Bool) :
    recAll { s with resolved := s.resolved ++ [(f, b)] } =
      recAll s + (if b then 1 else 0) := by
  simp only [recAll, List.filter_append, List.length_append]
  by_cases hb : b
  · have h1 : List.filter (fun r : FiberId × Bool => r.2) [(f, b)] = [(f, b)] := by
      simp [hb]
    simp [h1, hb]
  · have h1 : List.filter (fun r : FiberId × Bool => r.2) [(f, b)] = [] := by
      simp [hb]
    simp [h1, hb]

/-- Bridge: `recAll` is the true-record count of the resolved list. -/
theorem recAll_eq (s : MutexState) :
    recAll s = (s.resolved.filter (fun r : FiberId × Bool => r.2)).length := rfl

/-- Bridge: `recOf` is the per-fiber true-record count of the resolved list. -/
theorem recOf_eq (f : FiberId) (s : MutexState) :
    recOf f s = (s.resolved.filter (fun r : FiberId × Bool => r.1 = f && r.2)).length := rfl

/-- Length of a filtered cons, with the head's membership as an opaque
atom — the shape the consume-mirrors close by `omega`. -/
theorem filter_cons_len (p : FiberId × Bool → Bool) (x : FiberId × Bool)
    (l : List (FiberId × Bool)) :
    ((x :: l).filter p).length = (l.filter p).length + (if p x = true then 1 else 0) := by
  simp only [List.filter_cons, List.length_cons]
  split <;> rfl

/-- `consumeRec` unrolling: the head record is the caller's own. -/
theorem consumeRec_head_true (g : FiberId) (b0 : Bool) (tl : List (FiberId × Bool))
    (f : FiberId) (hgf : g = f) :
    consumeRec ((g, b0) :: tl) f = some (b0, tl) := by
  simp only [consumeRec, if_pos hgf]

/-- `consumeRec` unrolling: no record of `f` anywhere in the tail. -/
theorem consumeRec_head_none (g : FiberId) (b0 : Bool) (tl : List (FiberId × Bool))
    (f : FiberId) (hgf : ¬(g = f)) (hc : consumeRec tl f = none) :
    consumeRec ((g, b0) :: tl) f = none := by
  simp [consumeRec, if_neg hgf, hc]

/-- `consumeRec` unrolling: the first record of `f` is the tail's head
record, with outcome `b'` and remaining state `rest'`. -/
theorem consumeRec_head_some (g : FiberId) (b0 : Bool) (tl : List (FiberId × Bool))
    (f : FiberId) (hgf : ¬(g = f)) (b' : Bool) (rest' : List (FiberId × Bool))
    (hc : consumeRec tl f = some (b', rest')) :
    consumeRec ((g, b0) :: tl) f = some (b', (g, b0) :: rest') := by
  simp [consumeRec, if_neg hgf, hc]

/-- Consuming a record credits exactly that record's truth on the list. -/
theorem recAll_consume (l : List (FiberId × Bool)) (f : FiberId) (b : Bool)
    (rest : List (FiberId × Bool))
    (h : consumeRec l f = some (b, rest)) :
    (rest.filter (fun r : FiberId × Bool => r.2)).length + (if b then 1 else 0) =
      (l.filter (fun r : FiberId × Bool => r.2)).length := by
  induction l generalizing b rest with
  | nil => simp [consumeRec] at h
  | cons pr tl ih =>
      obtain ⟨g, b0⟩ := pr
      by_cases hgf : g = f
      · rw [consumeRec_head_true g b0 tl f hgf] at h
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl⟩ := h
        simp only [filter_cons_len]

      · cases hc : consumeRec tl f with
        | none =>
            rw [consumeRec_head_none g b0 tl f hgf hc] at h
            simp at h
        | some p =>
            obtain ⟨b', rest'⟩ := p
            have ihh := ih b' rest' hc
            rw [consumeRec_head_some g b0 tl f hgf b' rest' hc] at h
            simp only [Option.some.injEq, Prod.mk.injEq] at h
            obtain ⟨rfl, rfl⟩ := h
            simp only [filter_cons_len]
            omega

/-- Consuming one's own record moves exactly its truth from the pending
per-fiber ledger. -/
theorem recOf_consume_self (l : List (FiberId × Bool)) (f : FiberId) (b : Bool)
    (rest : List (FiberId × Bool))
    (h : consumeRec l f = some (b, rest)) :
    (rest.filter (fun r : FiberId × Bool => r.1 = f && r.2)).length +
        (if b then 1 else 0) =
      (l.filter (fun r : FiberId × Bool => r.1 = f && r.2)).length := by
  induction l generalizing b rest with
  | nil => simp [consumeRec] at h
  | cons pr tl ih =>
      obtain ⟨g, b0⟩ := pr
      by_cases hgf : g = f
      · rw [consumeRec_head_true g b0 tl f hgf] at h
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl⟩ := h
        simp only [filter_cons_len]
        by_cases hb0 : b0 <;> simp [hb0, hgf] <;> omega
      · cases hc : consumeRec tl f with
        | none =>
            rw [consumeRec_head_none g b0 tl f hgf hc] at h
            simp at h
        | some p =>
            obtain ⟨b', rest'⟩ := p
            have ihh := ih b' rest' hc
            rw [consumeRec_head_some g b0 tl f hgf b' rest' hc] at h
            simp only [Option.some.injEq, Prod.mk.injEq] at h
            obtain ⟨rfl, rfl⟩ := h
            simp only [filter_cons_len]
            omega

/-- Consuming one fiber's record leaves every other fiber's per-fiber
ledger untouched. -/
theorem recOf_consume_other (l : List (FiberId × Bool)) (f' f : FiberId)
    (hf'f : f' ≠ f) (b : Bool) (rest : List (FiberId × Bool))
    (h : consumeRec l f = some (b, rest)) :
    (rest.filter (fun r : FiberId × Bool => r.1 = f' && r.2)).length =
      (l.filter (fun r : FiberId × Bool => r.1 = f' && r.2)).length := by
  induction l generalizing b rest with
  | nil => simp [consumeRec] at h
  | cons pr tl ih =>
      obtain ⟨g, b0⟩ := pr
      by_cases hgf : g = f
      · rw [consumeRec_head_true g b0 tl f hgf] at h
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl⟩ := h
        simp only [filter_cons_len]
        have hc0 : (g = f' && b0) = false := by simp [hgf, Ne.symm hf'f]
        simp [hc0]
      · cases hc : consumeRec tl f with
        | none =>
            rw [consumeRec_head_none g b0 tl f hgf hc] at h
            simp at h
        | some p =>
            obtain ⟨b', rest'⟩ := p
            have ihh := ih b' rest' hc
            rw [consumeRec_head_some g b0 tl f hgf b' rest' hc] at h
            simp only [Option.some.injEq, Prod.mk.injEq] at h
            obtain ⟨rfl, rfl⟩ := h
            simp only [filter_cons_len]
            omega

/-- Membership survives `removeFiber` (for the other elements). -/
theorem mem_removeFiber_of_ne {l : List FiberId} {w g : FiberId} (hgw : g ≠ w)
    (h : g ∈ l) : g ∈ removeFiber l w := by
  induction l with
  | nil => cases h
  | cons x xs ih =>
      by_cases hxw : x = w
      · subst hxw
        have hrw : removeFiber (x :: xs) x = xs := by simp [removeFiber]
        rcases List.mem_cons.mp h with rfl | h
        · exact absurd rfl hgw
        · rw [hrw]; exact h
      · rw [show removeFiber (x :: xs) w = x :: removeFiber xs w from by
          simp [removeFiber, hxw]]
        rcases List.mem_cons.mp h with rfl | h
        · exact List.Mem.head _
        · exact List.Mem.tail _ (ih h)

/-- Membership in `removeFiber` implies membership in the original list. -/
theorem mem_of_mem_removeFiber {l : List FiberId} {w g : FiberId}
    (h : g ∈ removeFiber l w) : g ∈ l := by
  induction l with
  | nil => exact h
  | cons x xs ih =>
      by_cases hxw : x = w
      · subst hxw
        have hrw : removeFiber (x :: xs) x = xs := by simp [removeFiber]
        rw [hrw] at h
        exact List.Mem.tail _ h
      · rw [show removeFiber (x :: xs) w = x :: removeFiber xs w from by
          simp [removeFiber, hxw]] at h
        rcases List.mem_cons.mp h with rfl | h
        · exact List.Mem.head _
        · exact List.mem_cons_of_mem _ (ih h)

/-- `removeFiber` drops its argument: with no duplicates, the dropped
occurrence was the only one. -/
theorem not_mem_removeFiber_nd {l : List FiberId} {w : FiberId}
    (hnd : l.Nodup) : w ∉ removeFiber l w := by
  induction l with
  | nil => intro h; cases h
  | cons x xs ih =>
      obtain ⟨hxn, hxs⟩ := List.nodup_cons.mp hnd
      by_cases hxw : x = w
      · subst hxw
        have hrw : removeFiber (x :: xs) x = xs := by simp [removeFiber]
        intro h
        rw [hrw] at h
        exact hxn h
      · intro h
        rw [show removeFiber (x :: xs) w = x :: removeFiber xs w from by
          simp [removeFiber, hxw]] at h
        rcases List.mem_cons.mp h with rfl | h
        · exact hxw rfl
        · exact ih hxs h

/-- `removeFiber` preserves duplicate-freeness. -/
theorem nodup_removeFiber {l : List FiberId} {w : FiberId} (h : l.Nodup) :
    (removeFiber l w).Nodup := by
  induction l with
  | nil => exact List.nodup_nil
  | cons x xs ih =>
      obtain ⟨hxn, hxs⟩ := List.nodup_cons.mp h
      by_cases hxw : x = w
      · subst hxw
        have hrw : removeFiber (x :: xs) x = xs := by simp [removeFiber]
        rw [hrw]
        exact hxs
      · rw [show removeFiber (x :: xs) w = x :: removeFiber xs w from by
          simp [removeFiber, hxw]]
        refine List.nodup_cons.mpr ⟨?_, ih hxs⟩
        intro hm
        exact hxn (mem_of_mem_removeFiber hm)

/-! ## Admission and resolved-ledger helpers -/

/-- A successful admission leaves the state unchanged and reports the
owner-gate facts for the gated calls. -/
theorem mutexAdmit_intro {s : MutexState} {f : FiberId} {c : MutexCall}
    {s' : MutexState} (h : mutexAdmit s f c = some s') :
    s' = s ∧ ((c = MutexCall.mlock → s.owner ≠ some f) ∧
      (c = MutexCall.munlock → s.owner = some f)) := by
  cases c with
  | mlock =>
      simp only [mutexAdmit] at h
      by_cases hc : s.owner = some f
      · rw [if_pos hc] at h
        exact absurd h (by simp)
      · rw [if_neg hc] at h
        injection h with he
        refine ⟨he.symm, fun _ => hc, fun hc2 => absurd hc2 (by simp)⟩
  | munlock =>
      simp only [mutexAdmit] at h
      by_cases hc : s.owner = some f
      · rw [if_pos hc] at h
        injection h with he
        exact ⟨he.symm, fun hc2 => absurd hc2 (by simp), fun _ => hc⟩
      · rw [if_neg hc] at h
        exact absurd h (by simp)
  | mtrylock =>
      simp only [mutexAdmit] at h
      injection h with he
      exact ⟨he.symm, fun hc2 => absurd hc2 (by simp), fun hc2 => absurd hc2 (by simp)⟩
  | mcancel _ =>
      simp only [mutexAdmit] at h
      injection h with he
      exact ⟨he.symm, fun hc2 => absurd hc2 (by simp), fun hc2 => absurd hc2 (by simp)⟩

theorem recOf_append_true (s : MutexState) (w f : FiberId) :
    recOf f { s with resolved := s.resolved ++ [(w, true)] } =
      recOf f s + (if w = f then 1 else 0) := by
  simp only [recOf, List.filter_append, List.length_append]
  by_cases hwf : w = f
  · have h1 : List.filter (fun r : FiberId × Bool => r.1 = f && r.2) [(w, true)] =
        [(w, true)] := by simp [hwf]
    simp [h1, hwf]
  · have h1 : List.filter (fun r : FiberId × Bool => r.1 = f && r.2) [(w, true)] =
        ([] : List (FiberId × Bool)) := by
      simp [hwf]
    simp [h1, hwf]

theorem recOf_append_false (s : MutexState) (w f : FiberId) :
    recOf f { s with resolved := s.resolved ++ [(w, false)] } = recOf f s := by
  simp only [recOf, List.filter_append, List.length_append]
  have h1 : List.filter (fun r : FiberId × Bool => r.1 = f && r.2) [(w, false)] =
      ([] : List (FiberId × Bool)) := by
    by_cases hwf : w = f <;> simp [hwf]
  simp [h1]

theorem recAll_append_false (s : MutexState) (w : FiberId) :
    recAll { s with resolved := s.resolved ++ [(w, false)] } = recAll s := by
  have h := recAll_cons s w false
  simpa using h

/-- The private state of a mutex configuration, at `MutexState` type
(the configuration stores it at `mutexPrim.State`). -/
@[reducible] def mstate (c : PrimCfg MutexSig mutexPrim) : MutexState := c.prim

/-! ## The conservation mirror

Every run conserves the grant/release balance against the ownership
state, per fiber and globally.  Grants are credited at their completion
observation, or earlier while still in the fiber's return window
(`retGrant`) or as a delivered `WaitNode` outcome awaiting resume
(`recOf`); releases likewise via `retUnlock`.  The `cfg`-side bits make
the statement symmetric, so the empty run is trivial and each step only
has to conserve the step's own delta.

Carried facts: `waitq` has no duplicates (one registration per fiber);
no queued fiber is the recorded owner; a freshly dispatched `lock` is
admitted only with a free owner slot and a freshly dispatched `unlock`
only by the owner (both preserved until the call's inline paths run,
since `cancel` never touches the owner slot); stale runq entries and
parked calls are `lock` calls; an external entry whose result is fixed
is a `cancel` (the only external critical section), so an external
completion never adds grant or release credit. -/

theorem mutex_mirror {cfg fin : PrimCfg MutexSig mutexPrim} {t : Trace MutexSig}
    (hrun : PrimRuns2 MutexSig mutexPrim cfg t fin)
    (hnd : cfg.prim.waitq.Nodup)
    (hown : ∀ g ∈ cfg.prim.waitq, cfg.prim.owner ≠ some g)
    (hM3 : ∀ d : Pnd MutexSig, cfg.cur = some (FSlot.running d false) →
      (d.call = MutexCall.mlock → cfg.prim.owner ≠ some d.fiber) ∧
      (d.call = MutexCall.munlock → cfg.prim.owner = some d.fiber))
    (hstale : ∀ r ∈ cfg.runq, r.fresh = false → r.call = MutexCall.mlock)
    (hparked : ∀ p ∈ cfg.parked, p.call = MutexCall.mlock)
    (hext : ∀ e ∈ cfg.exts, e.result.isSome → ∃ w, e.call = MutexCall.mcancel w) :
    (∀ f : FiberId,
        grantsOf f t + retGrant f fin.cur + recOf f (mstate fin)
          + retUnlock f cfg.cur + ownerIs f (mstate cfg) =
        releasesOf f t + retGrant f cfg.cur + recOf f (mstate cfg)
          + retUnlock f fin.cur + ownerIs f (mstate fin)) ∧
    (grantsAll t + retGrantAll fin.cur + recAll (mstate fin)
        + retUnlockAll cfg.cur + ownerHeld (mstate cfg) =
      releasesAll t + retGrantAll cfg.cur + recAll (mstate cfg)
        + retUnlockAll fin.cur + ownerHeld (mstate fin)) ∧
    fin.prim.waitq.Nodup ∧
    (∀ g ∈ fin.prim.waitq, fin.prim.owner ≠ some g) ∧
    (∀ d : Pnd MutexSig, fin.cur = some (FSlot.running d false) →
      (d.call = MutexCall.mlock → fin.prim.owner ≠ some d.fiber) ∧
      (d.call = MutexCall.munlock → fin.prim.owner = some d.fiber)) ∧
    (∀ r ∈ fin.runq, r.fresh = false → r.call = MutexCall.mlock) ∧
    (∀ p ∈ fin.parked, p.call = MutexCall.mlock) ∧
    (∀ e ∈ fin.exts, e.result.isSome → ∃ w, e.call = MutexCall.mcancel w) := by
  induction hrun with
  | stop cfg =>
      refine ⟨?_, ?_, hnd, hown, hM3, hstale, hparked, hext⟩
      · intro f
        simp only [grantsOf_nil, releasesOf_nil]
      · simp only [grantsAll_nil, releasesAll_nil]
  | step cfg cfg' o t2 fin hstep hrest ih =>
  cases hstep with
  | submit f c _ =>
      have hstale' : ∀ r ∈ cfg.runq ++ [{ fiber := f, call := c, fresh := true }],
          r.fresh = false → r.call = MutexCall.mlock := by
        intro r hr hfr
        rcases List.mem_append.mp hr with hm | hm
        · exact hstale r hm hfr
        · rw [List.mem_singleton] at hm
          subst hm
          simp at hfr
      obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked', hext'⟩ :=
        ih hnd hown hM3 hstale' hparked hext
      refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked', hext'⟩
      · intro f
        simpa [mstate, recOf, ownerIs] using hbal f
      · simpa [mstate, recAll, ownerHeld] using hbalA
  | dispatchFresh rp rest s' hcurE hrunqE _ hadmit =>
      obtain ⟨hsp, hmlk, hmlk2⟩ := mutexAdmit_intro hadmit
      subst hsp
      have hM3n : ∀ d2 : Pnd MutexSig,
          some (FSlot.running { fiber := rp.fiber, call := rp.call } false) =
            some (FSlot.running d2 false) →
          (d2.call = MutexCall.mlock → cfg.prim.owner ≠ some d2.fiber) ∧
          (d2.call = MutexCall.munlock → cfg.prim.owner = some d2.fiber) := by
        intro d2 hd
        injection hd with hs
        injection hs with h1 _
        subst h1
        exact ⟨fun hc => hmlk hc, fun hc => hmlk2 hc⟩
      have hstale' : ∀ r ∈ rest, r.fresh = false → r.call = MutexCall.mlock :=
        fun r hr => hstale r (by rw [hrunqE]; exact List.Mem.tail _ hr)
      obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked', hext'⟩ :=
        ih hnd hown hM3n hstale' hparked hext
      refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked', hext'⟩
      · intro f
        rw [hcurE]
        simpa [mstate, recOf, ownerIs] using hbal f
      · rw [hcurE]
        simpa [mstate, recAll, ownerHeld] using hbalA
  | dispatchResumed rp rest hcurE hrunqE _ =>
      have hM3n : ∀ d2 : Pnd MutexSig,
          some (FSlot.running { fiber := rp.fiber, call := rp.call } true) =
            some (FSlot.running d2 false) →
          (d2.call = MutexCall.mlock → cfg.prim.owner ≠ some d2.fiber) ∧
          (d2.call = MutexCall.munlock → cfg.prim.owner = some d2.fiber) := by
        intro d2 hd
        simp at hd
      have hstale' : ∀ r ∈ rest, r.fresh = false → r.call = MutexCall.mlock :=
        fun r hr => hstale r (by rw [hrunqE]; exact List.Mem.tail _ hr)
      obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked', hext'⟩ :=
        ih hnd hown hM3n hstale' hparked hext
      refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked', hext'⟩
      · intro f
        rw [hcurE]
        simpa [mstate, recOf, ownerIs] using hbal f
      · rw [hcurE]
        simpa [mstate, recAll, ownerHeld] using hbalA
  | fiberEffect d b ps rest r s' wk hcurE hb hrunE hmap hwake =>
      subst hb
      have hstale' : ∀ x ∈ cfg.runq ++ ps.map
          (fun p : Pnd MutexSig => { fiber := p.fiber, call := p.call, fresh := false }),
          x.fresh = false → x.call = MutexCall.mlock := by
        intro x hx hfr
        rcases List.mem_append.mp hx with hm | hm
        · exact hstale x hm hfr
        · rw [List.mem_map] at hm
          obtain ⟨p, hps, hx2⟩ := hm
          cases hx2
          exact hparked p (Wakes.mem_parked hwake p hps)
      have hparked' : ∀ p ∈ rest, p.call = MutexCall.mlock := by
        intro p hp
        exact hparked p (Wakes.mem_rest hwake p hp)
      cases hdcall : d.call with
      | mlock =>
          rw [hdcall] at hrunE
          simp only [mutexPrim, mutexRun] at hrunE
          by_cases h1 : cfg.prim.owner = none ∧ cfg.prim.waitq = []
          · rw [if_pos h1] at hrunE
            simp only [Option.some.injEq, Prod.mk.injEq] at hrunE
            obtain ⟨rfl, rfl, rfl⟩ := hrunE
            obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked'', hext'⟩ :=
              ih List.nodup_nil (fun g hg => absurd hg (by simp))
                (fun d2 hd => absurd hd (by simp)) hstale' hparked' hext
            refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked'', hext'⟩
            · intro f
              rw [hcurE]
              have hb := hbal f
              by_cases hfd : d.fiber = f <;>
                simp [h1.1, hfd, hdcall, mstate, recOf, ownerIs] at hb ⊢ <;> omega
            · simp [hcurE, h1.1, hdcall, mstate, recAll, ownerHeld] at hbalA ⊢
              omega
          · rw [if_neg h1] at hrunE
            by_cases h2 : cfg.prim.owner = some d.fiber
            · exact absurd h2 ((hM3 d hcurE).1 hdcall)
            · rw [if_neg h2] at hrunE
              simp at hrunE
      | munlock =>
          rw [hdcall] at hrunE
          simp only [mutexPrim, mutexRun] at hrunE
          cases hwq : cfg.prim.waitq with
          | nil =>
              rw [hwq] at hrunE
              simp only at hrunE
              simp only [Option.some.injEq, Prod.mk.injEq] at hrunE
              obtain ⟨rfl, rfl, rfl⟩ := hrunE
              obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked'', hext'⟩ :=
                ih List.nodup_nil (fun g hg => absurd hg (by simp))
                  (fun d2 hd => absurd hd (by simp)) hstale' hparked' hext
              have hgr := (hM3 d hcurE).2 hdcall
              refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked'', hext'⟩
              · intro f
                rw [hcurE]
                have hb := hbal f
                by_cases hfd : d.fiber = f <;>
                  simp [hgr, hfd, hdcall, mstate, recOf, ownerIs] at hb ⊢ <;> omega
              · simp [hcurE, hgr, hdcall, mstate, recAll, ownerHeld] at hbalA ⊢
                omega
          | cons w tl =>
              rw [hwq] at hrunE
              simp only at hrunE
              simp only [Option.some.injEq, Prod.mk.injEq] at hrunE
              obtain ⟨rfl, rfl, rfl⟩ := hrunE
              have hgr := (hM3 d hcurE).2 hdcall
              have hndw : (w :: tl).Nodup := by rw [← hwq]; exact hnd
              have hwgtl : w ∉ tl := (List.nodup_cons.mp hndw).1
              have hwne : ¬(w = d.fiber) := by
                intro hc
                exact hown w (by rw [hwq]; exact List.Mem.head _)
                  (by rw [hc]; exact hgr)
              have hown' : ∀ g ∈ tl, some w ≠ some g := by
                intro g hg hc
                have hwg : w = g := by injection hc
                exact hwgtl (by rw [hwg]; exact hg)
              obtain ⟨hbal, hbalA, hnd'', hown'', hM3'', hstale''', hparked''', hext''⟩ :=
                ih (List.nodup_cons.mp hndw).2 hown' (fun d2 hd => absurd hd (by simp))
                  hstale' hparked' hext
              refine ⟨?_, ?_, hnd'', hown'', hM3'', hstale''', hparked''', hext''⟩
              · intro f
                rw [hcurE]
                have hb := hbal f
                by_cases hfd : d.fiber = f <;>
                  by_cases hwf : w = f <;>
                    simp [hgr, hwf, hfd, hdcall, mstate, recOf, List.filter_append,
                      List.length_append, ownerIs] at hb ⊢ <;> omega
              · simp [hcurE, hgr, hdcall, mstate, recAll, List.filter_append,
                  List.length_append, ownerHeld] at hbalA ⊢
                omega
      | mtrylock =>
          rw [hdcall] at hrunE
          simp only [mutexPrim, mutexRun] at hrunE
          by_cases h1 : cfg.prim.owner = none ∧ cfg.prim.waitq = []
          · rw [if_pos h1] at hrunE
            simp only [Option.some.injEq, Prod.mk.injEq] at hrunE
            obtain ⟨rfl, rfl, rfl⟩ := hrunE
            obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked'', hext'⟩ :=
              ih List.nodup_nil (fun g hg => absurd hg (by simp))
                (fun d2 hd => absurd hd (by simp)) hstale' hparked' hext
            refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked'', hext'⟩
            · intro f
              rw [hcurE]
              have hb := hbal f
              by_cases hfd : d.fiber = f <;>
                simp [h1.1, hfd, hdcall, mstate, recOf, ownerIs] at hb ⊢ <;> omega
            · simp [hcurE, h1.1, hdcall, mstate, recAll, ownerHeld] at hbalA ⊢
              omega
          · rw [if_neg h1] at hrunE
            simp only [Option.some.injEq, Prod.mk.injEq] at hrunE
            obtain ⟨rfl, rfl, rfl⟩ := hrunE
            obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale', hparked', hext''⟩ :=
              ih hnd hown (fun d2 hd => absurd hd (by simp)) hstale' hparked' hext
            refine ⟨?_, ?_, hnd', hown', hM3', hstale', hparked', hext''⟩
            · intro f
              rw [hcurE]
              have hb := hbal f
              simp [hdcall, mstate, recOf, ownerIs] at hb ⊢ <;> omega
            · simp [hcurE, hdcall, mstate, recAll, ownerHeld] at hbalA ⊢ <;> omega
      | mcancel w0 =>
          rw [hdcall] at hrunE
          simp only [mutexPrim, mutexRun] at hrunE
          simp at hrunE
  | fiberDone d r hcurE =>
      have hM3n : ∀ d2 : Pnd MutexSig,
          (none : Option (FSlot MutexSig)) = some (FSlot.running d2 false) →
          (d2.call = MutexCall.mlock → cfg.prim.owner ≠ some d2.fiber) ∧
          (d2.call = MutexCall.munlock → cfg.prim.owner = some d2.fiber) := by
        intro d2 hd
        simp at hd
      obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale', hparked', hext'⟩ :=
        ih hnd hown hM3n hstale hparked hext
      refine ⟨?_, ?_, hnd', hown', hM3', hstale', hparked', hext'⟩
      · intro f
        simp only [hcurE, Option.toList, List.cons_append, List.nil_append,
          grantsOf_comp, releasesOf_comp]
        have hb := hbal f
        clear hbalA
        by_cases hfd : d.fiber = f <;>
          cases hdcall : d.call <;> cases r with
          | runit =>
              simp [hfd, hdcall, isGrant, isRelease, compObs, mstate, recOf, ownerIs] at hb ⊢ <;>
                omega
          | rbool b =>
              cases b <;>
                simp [hfd, hdcall, isGrant, isRelease, compObs, mstate, recOf, ownerIs] at hb ⊢ <;>
                  omega
      · simp only [hcurE, Option.toList, List.cons_append, List.nil_append, grantsAll_comp,
          releasesAll_comp]
        cases hdcall : d.call <;> cases r with
        | runit =>
            simp [hdcall, isGrant, isRelease, compObs, mstate, recAll, ownerHeld] at hbalA ⊢ <;>
              omega
        | rbool b =>
            cases b <;>
              simp [hdcall, isGrant, isRelease, compObs, mstate, recAll, ownerHeld] at hbalA ⊢ <;>
                omega
  | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
      have hM3n : ∀ d2 : Pnd MutexSig,
          (none : Option (FSlot MutexSig)) = some (FSlot.running d2 false) →
          (d2.call = MutexCall.mlock → cfg.prim.owner ≠ some d2.fiber) ∧
          (d2.call = MutexCall.munlock → cfg.prim.owner = some d2.fiber) := by
        intro d2 hd
        simp at hd
      cases hdcall : d.call with
      | mlock =>
          rw [hdcall] at hparkE
          simp only [mutexPrim, mutexPark] at hparkE
          by_cases hmem : d.fiber ∈ cfg.prim.waitq
          · rw [if_pos hmem] at hparkE
            exact absurd hparkE (by simp)
          · rw [if_neg hmem] at hparkE
            simp only [Option.some.injEq] at hparkE
            rw [hdcall] at hrunE
            simp only [mutexPrim, mutexRun] at hrunE
            by_cases h1 : cfg.prim.owner = none ∧ cfg.prim.waitq = []
            · rw [if_pos h1] at hrunE
              exact absurd hrunE (by simp)
            · rw [if_neg h1] at hrunE
              by_cases h2 : cfg.prim.owner = some d.fiber
              · rw [if_pos h2] at hrunE
                exact absurd hrunE (by simp)
              · rw [if_neg h2] at hrunE
                obtain ⟨hst', hwk⟩ := Prod.mk.inj hparkE
                subst hwk
                subst hst'
                have hps : ps = [] := by
                  cases ps with
                  | nil => rfl
                  | cons p ps' => simp at hmap
                subst hps
                have hnd2 : (cfg.prim.waitq ++ [d.fiber]).Nodup := by
                  rw [List.nodup_append]
                  refine ⟨hnd, by simp [List.Nodup], ?_⟩
                  intro a ha1 b hb2
                  rw [List.mem_singleton] at hb2
                  subst hb2
                  intro hac
                  apply hmem
                  rw [← hac]
                  exact ha1
                have hown2 : ∀ g ∈ cfg.prim.waitq ++ [d.fiber],
                    cfg.prim.owner ≠ some g := by
                  intro g hg
                  rcases List.mem_append.mp hg with hm | hm
                  · exact hown g hm
                  · rw [List.mem_singleton] at hm
                    subst hm
                    exact h2
                have hparked2 : ∀ p ∈ rest ++ [d], p.call = MutexCall.mlock := by
                  intro p hp
                  rcases List.mem_append.mp hp with hm | hm
                  · exact hparked p (Wakes.mem_rest hwake p hm)
                  · rw [List.mem_singleton] at hm
                    subst hm
                    exact hdcall
                have hstale2 : ∀ x ∈ cfg.runq ++ (List.map
                    (fun p : Pnd MutexSig => { fiber := p.fiber, call := p.call, fresh := false }) []),
                  x.fresh = false → x.call = MutexCall.mlock := by
                  intro x hx hfr
                  rcases List.mem_append.mp hx with hm | hm
                  · exact hstale x hm hfr
                  · simp at hm
                obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale', hparked', hext'⟩ :=
                  ih hnd2 hown2 hM3n hstale2 hparked2 hext
                refine ⟨?_, ?_, hnd', hown', hM3', hstale', hparked', hext'⟩
                · intro f
                  rw [hcurE]
                  simpa [mstate, recOf, ownerIs] using hbal f
                · rw [hcurE]
                  simpa [mstate, recAll, ownerHeld] using hbalA
      | munlock => rw [hdcall] at hparkE; simp [mutexPrim, mutexPark, hdcall] at hparkE
      | mtrylock => rw [hdcall] at hparkE; simp [mutexPrim, mutexPark, hdcall] at hparkE
      | mcancel _ => rw [hdcall] at hparkE; simp [mutexPrim, mutexPark, hdcall] at hparkE
  | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
      subst hb
      have hstale' : ∀ x ∈ cfg.runq ++ ps.map
          (fun p : Pnd MutexSig => { fiber := p.fiber, call := p.call, fresh := false }),
          x.fresh = false → x.call = MutexCall.mlock := by
        intro x hx hfr
        rcases List.mem_append.mp hx with hm | hm
        · exact hstale x hm hfr
        · rw [List.mem_map] at hm
          obtain ⟨p, hps, hx2⟩ := hm
          cases hx2
          exact hparked p (Wakes.mem_parked hwake p hps)
      have hparked' : ∀ p ∈ rest, p.call = MutexCall.mlock := by
        intro p hp
        exact hparked p (Wakes.mem_rest hwake p hp)
      have hM3n : ∀ d2 : Pnd MutexSig,
          (none : Option (FSlot MutexSig)) = some (FSlot.running d2 false) →
          (d2.call = MutexCall.mlock → cfg.prim.owner ≠ some d2.fiber) ∧
          (d2.call = MutexCall.munlock → cfg.prim.owner = some d2.fiber) := by
        intro d2 hd
        simp at hd
      cases hdcall : d.call with
      | mlock =>
          rw [hdcall] at hfinE
          simp only [mutexPrim, mutexFinish] at hfinE
          cases hc : consumeRec cfg.prim.resolved d.fiber with
          | none =>
              rw [hc] at hfinE
              simp at hfinE
          | some p =>
              obtain ⟨b0, rest⟩ := p
              rw [hc] at hfinE
              dsimp only at hfinE
              simp only [Option.some.injEq, Prod.mk.injEq] at hfinE
              obtain ⟨rfl, rfl, rfl⟩ := hfinE
              obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked'', hext'⟩ :=
                ih hnd hown (fun d2 hd => absurd hd (by simp)) hstale' hparked' hext
              refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked'', hext'⟩
              · intro f
                simp only [hcurE, Option.toList, List.cons_append, List.nil_append,
                  grantsOf_comp, releasesOf_comp]
                have hb := hbal f
                clear hbalA
                by_cases hfd : d.fiber = f
                · have hrc := recOf_consume_self cfg.prim.resolved d.fiber b0 rest hc
                  rw [hfd] at hrc
                  simp only [mstate, recOf_eq, ownerIs] at hb ⊢
                  rw [← hrc]
                  cases b0 <;>
                    simp [hfd, isGrant, isRelease, compObs] at hb ⊢ <;> omega
                · have hrc := recOf_consume_other cfg.prim.resolved f d.fiber
                    (fun hc2 => hfd hc2.symm) b0 rest hc
                  simp only [mstate, recOf_eq, ownerIs] at hb ⊢
                  rw [← hrc]
                  simp [hfd, isGrant, isRelease, compObs] at hb ⊢ <;> omega
              · simp only [hcurE, Option.toList, List.cons_append, List.nil_append,
                  grantsAll_comp, releasesAll_comp]
                have hrc := recAll_consume cfg.prim.resolved d.fiber b0 rest hc
                simp only [mstate, recAll_eq, ownerHeld] at hbalA ⊢
                rw [← hrc]
                cases b0 <;>
                  simp [isGrant, isRelease, compObs] at hbalA ⊢ <;> omega
      | munlock => rw [hdcall] at hfinE; simp [mutexPrim, mutexFinish, hdcall] at hfinE
      | mtrylock => rw [hdcall] at hfinE; simp [mutexPrim, mutexFinish, hdcall] at hfinE
      | mcancel _ => rw [hdcall] at hfinE; simp [mutexPrim, mutexFinish, hdcall] at hfinE
  | extApply x c preE postE hcap hnovel =>
      have hext' : ∀ e' ∈ cfg.exts ++ [{ x := x, call := c, result := none }],
          e'.result.isSome → ∃ w, e'.call = MutexCall.mcancel w := by
        intro e' hm his
        rcases List.mem_append.mp hm with hm2 | hm2
        · exact hext e' hm2 his
        · rw [List.mem_singleton] at hm2
          subst hm2
          simp at his
      obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale', hparked', hext''⟩ :=
        ih hnd hown hM3 hstale hparked hext'
      refine ⟨?_, ?_, hnd', hown', hM3', hstale', hparked', hext''⟩
      · intro f
        simpa [mstate, recOf, ownerIs] using hbal f
      · simpa [mstate, recAll, ownerHeld] using hbalA
  | extEffect preE postE e ps rest r s' wk hextsE hresE hrunE hmap hwake =>
      cases hec : e.call with
      | mcancel w =>
          rw [hec] at hrunE
          simp only [mutexPrim, mutexExtRun] at hrunE
          by_cases hw : w ∈ cfg.prim.waitq
          · rw [if_pos hw] at hrunE
            simp only [Option.some.injEq, Prod.mk.injEq] at hrunE
            obtain ⟨rfl, rfl, rfl⟩ := hrunE
            have hstale' : ∀ x ∈ cfg.runq ++ ps.map
                (fun p : Pnd MutexSig => { fiber := p.fiber, call := p.call, fresh := false }),
                x.fresh = false → x.call = MutexCall.mlock := by
              intro x hx hfr
              rcases List.mem_append.mp hx with hm | hm
              · exact hstale x hm hfr
              · rw [List.mem_map] at hm
                obtain ⟨p, hps, hx2⟩ := hm
                cases hx2
                exact hparked p (Wakes.mem_parked hwake p hps)
            have hparked' : ∀ p ∈ rest, p.call = MutexCall.mlock := by
              intro p hp
              exact hparked p (Wakes.mem_rest hwake p hp)
            have hnd' : (removeFiber cfg.prim.waitq w).Nodup := nodup_removeFiber hnd
            have hown' : ∀ g ∈ removeFiber cfg.prim.waitq w, cfg.prim.owner ≠ some g :=
              fun g hg => hown g (mem_of_mem_removeFiber hg)
            have hext' : ∀ e' ∈ preE ++ { e with result := some (MutexRes.rbool true) } :: postE,
                e'.result.isSome → ∃ w', e'.call = MutexCall.mcancel w' := by
              intro e' hm his
              rcases List.mem_append.mp hm with h1 | h2
              · exact hext e' (by
                  rw [hextsE]
                  exact List.mem_append.mpr (Or.inl h1)) his
              · rcases List.mem_cons.mp h2 with rfl | h3
                · exact ⟨w, hec⟩
                · exact hext e' (by
                    rw [hextsE]
                    exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ h3))) his
            obtain ⟨hbal, hbalA, hnd'', hown'', hM3', hstale'', hparked'', hext''⟩ :=
              ih hnd' hown' hM3 hstale' hparked' hext'
            refine ⟨?_, ?_, hnd'', hown'', hM3', hstale'', hparked'', hext''⟩
            · intro f
              have hb := hbal f
              simpa [mstate, recOf, List.filter_append, List.length_append, ownerIs] using hb
            · simpa [mstate, recAll, List.filter_append, List.length_append, ownerHeld]
                using hbalA
          · rw [if_neg hw] at hrunE
            simp only [Option.some.injEq, Prod.mk.injEq] at hrunE
            obtain ⟨rfl, rfl, rfl⟩ := hrunE
            have hstale' : ∀ x ∈ cfg.runq ++ ps.map
                (fun p : Pnd MutexSig => { fiber := p.fiber, call := p.call, fresh := false }),
                x.fresh = false → x.call = MutexCall.mlock := by
              intro x hx hfr
              rcases List.mem_append.mp hx with hm | hm
              · exact hstale x hm hfr
              · rw [List.mem_map] at hm
                obtain ⟨p, hps, hx2⟩ := hm
                cases hx2
                exact hparked p (Wakes.mem_parked hwake p hps)
            have hparked' : ∀ p ∈ rest, p.call = MutexCall.mlock := by
              intro p hp
              exact hparked p (Wakes.mem_rest hwake p hp)
            have hext' : ∀ e' ∈ preE ++ { e with result := some (MutexRes.rbool false) } :: postE,
                e'.result.isSome → ∃ w', e'.call = MutexCall.mcancel w' := by
              intro e' hm his
              rcases List.mem_append.mp hm with h1 | h2
              · exact hext e' (by
                  rw [hextsE]
                  exact List.mem_append.mpr (Or.inl h1)) his
              · rcases List.mem_cons.mp h2 with rfl | h3
                · exact ⟨w, hec⟩
                · exact hext e' (by
                    rw [hextsE]
                    exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ h3))) his
            obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale'', hparked'', hext''⟩ :=
              ih hnd hown hM3 hstale' hparked' hext'
            refine ⟨?_, ?_, hnd', hown', hM3', hstale'', hparked'', hext''⟩
            · intro f
              simpa [mstate, recOf, ownerIs] using hbal f
            · simpa [mstate, recAll, ownerHeld] using hbalA
      | mlock => simp [mutexPrim, mutexExtRun, hec] at hrunE
      | munlock => simp [mutexPrim, mutexExtRun, hec] at hrunE
      | mtrylock => simp [mutexPrim, mutexExtRun, hec] at hrunE
  | extDone preE postE e r hextsE hresE =>
      obtain ⟨w0, hw0⟩ :=
        hext e (by
          rw [hextsE]
          exact List.mem_append.mpr (Or.inr List.mem_cons_self))
          (by rw [hresE]; simp)
      have hext' : ∀ e' ∈ preE ++ postE,
          e'.result.isSome → ∃ w, e'.call = MutexCall.mcancel w := by
        intro e' hm his
        rcases List.mem_append.mp hm with h1 | h2
        · exact hext e' (by
            rw [hextsE]
            exact List.mem_append.mpr (Or.inl h1)) his
        · exact hext e' (by
            rw [hextsE]
            exact List.mem_append.mpr (Or.inr (List.mem_cons_of_mem _ h2))) his
      obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale', hparked', hext''⟩ :=
        ih hnd hown hM3 hstale hparked hext'
      refine ⟨?_, ?_, hnd', hown', hM3', hstale', hparked', hext''⟩
      · intro f
        simp only [Option.toList, List.cons_append, List.nil_append, grantsOf_comp,
          releasesOf_comp]
        have hb := hbal f
        simp [compObs] at hb ⊢ <;> omega
      · simp only [Option.toList, List.cons_append, List.nil_append, grantsAll_comp,
          releasesAll_comp]
        cases hec : e.call with
        | mcancel w => simp [isGrant, isRelease, compObs, hec] at hbalA ⊢ <;> omega
        | mlock => rw [hw0] at hec; simp at hec
        | munlock => rw [hw0] at hec; simp at hec
        | mtrylock => rw [hw0] at hec; simp at hec
  | envTime t0 hcurE hle =>
      obtain ⟨hbal, hbalA, hnd', hown', hM3', hstale', hparked', hext'⟩ :=
        ih hnd hown hM3 hstale hparked hext
      refine ⟨?_, ?_, hnd', hown', hM3', hstale', hparked', hext'⟩
      · intro f
        simpa [mutexPrim, mstate, recOf, ownerIs] using hbal f
      · simpa [mutexPrim, mstate, recAll, ownerHeld] using hbalA
  | envExpire _ _ _ _ _ _ hexp _ _ =>
      simp only [mutexPrim] at hexp
      simp at hexp

/-! ## The ownership guarantee -/

/-- Splitting a primitive run at an arbitrary cut `pre ++ ob :: suf`. -/
theorem mutexRuns_split {pc pc' : PrimCfg MutexSig mutexPrim} {t : Trace MutexSig}
    (hrun : PrimRuns2 MutexSig mutexPrim pc t pc') :
    ∀ (pre : Trace MutexSig) (ob : Obs MutexSig) (suf : Trace MutexSig),
      t = pre ++ ob :: suf →
      ∃ ma mb : PrimCfg MutexSig mutexPrim,
        PrimRuns2 MutexSig mutexPrim pc pre ma ∧
        PrimStep2 MutexSig mutexPrim ma (some ob) mb ∧
        PrimRuns2 MutexSig mutexPrim mb suf pc' := by
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

/-- A step that completes a grant leaves the ownership books showing the
mutex was free at the completion: the release-window and owner slot cover
whatever grant-window or delivered record still holds credit. -/
theorem mutexStep_grant_credit {ma mb : PrimCfg MutexSig mutexPrim} {ob : Obs MutexSig}
    (hstep : PrimStep2 MutexSig mutexPrim ma (some ob) mb)
    (hc : ob.call = MutexCall.mlock ∨ ob.call = MutexCall.mtrylock)
    (hr : ob.result = some (MutexRes.rbool true))
    (hext : ∀ e ∈ ma.exts, e.result.isSome → ∃ w, e.call = MutexCall.mcancel w) :
    retUnlockAll ma.cur + ownerHeld ma.prim ≤ retGrantAll ma.cur + recAll ma.prim := by
  have hoh : ownerHeld ma.prim ≤ 1 := by
    by_cases ho : ma.prim.owner = none
    · simp [ownerHeld, ho]
    · simp [ownerHeld, ho]
  cases hstep with
  | fiberDone d r hcurE =>
      have hc1 : d.call = MutexCall.mlock ∨ d.call = MutexCall.mtrylock := hc
      have hr2 : r = MutexRes.rbool true := Option.some.inj hr
      subst hr2
      rcases hc1 with h2 | h2 <;> simp [hcurE, h2] <;> omega
  | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
      have hr2 : r = MutexRes.rbool true := Option.some.inj hr
      subst hr2
      have hcur2 : ma.cur = some (FSlot.running d true) := by
        rw [hcurE, hb]
      cases hc2 : d.call with
      | mlock =>
          simp only [mutexPrim, mutexFinish, hc2] at hfinE
          cases hcc : consumeRec ma.prim.resolved d.fiber with
          | none =>
              rw [hcc] at hfinE
              simp at hfinE
          | some p =>
              obtain ⟨b0, rest⟩ := p
              rw [hcc] at hfinE
              dsimp only at hfinE
              simp only [Option.some.injEq, Prod.mk.injEq] at hfinE
              obtain ⟨he1, he2, he3⟩ := hfinE
              have hb0 : b0 = true := by injection he1
              subst hb0
              subst he2
              subst he3
              have hrc := recAll_consume ma.prim.resolved d.fiber true rest hcc
              have hone : (if true = true then 1 else 0) = 1 := rfl
              simp only [hcur2]
              simp
              rw [recAll_eq]
              omega
      | munlock => simp [mutexPrim, mutexFinish, hc2] at hfinE
      | mtrylock => simp [mutexPrim, mutexFinish, hc2] at hfinE
      | mcancel w0 => simp [mutexPrim, mutexFinish, hc2] at hfinE
  | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
      exfalso
      have h4 : (none : Option MutexRes) = some (MutexRes.rbool true) := hr
      simp at h4
  | extApply x c preE postE hcap hnovel =>
      exfalso
      have h4 : (none : Option MutexRes) = some (MutexRes.rbool true) := hr
      simp at h4
  | extDone preE postE e r hextsE hresE =>
      have hc1 : e.call = MutexCall.mlock ∨ e.call = MutexCall.mtrylock := hc
      obtain ⟨w0, hw0⟩ :=
        hext e (by
          rw [hextsE]
          exact List.mem_append.mpr (Or.inr List.mem_cons_self))
          (by rw [hresE]; simp)
      rcases hc1 with h2 | h2 <;> rw [hw0] at h2 <;> simp at h2

/-- Trace form: every completed grant completes only when the completed
releases have caught up — the mutex was free at the completion.  The
global prefix bound `grantsAll ≤ releasesAll + 1` (mutual exclusion)
follows: the counts move only at completion observations. -/
def mutexExclHonored (t : Trace MutexSig) : Prop :=
  ∀ (pre : Trace MutexSig) (g : FiberId) (c : MutexCall) (suf : Trace MutexSig),
    t = pre ++ compObs MutexSig (Caller.fiber g) c (MutexRes.rbool true) :: suf →
      (c = MutexCall.mlock ∨ c = MutexCall.mtrylock) →
      grantsAll pre ≤ releasesAll pre

theorem mutexPrim_guarantees : Guarantees mutexPrim mutexExclHonored := by
  intro t ht pre g c suf hsplit hc
  obtain ⟨⟨fin, hrun⟩, -⟩ := ht
  obtain ⟨ma, mb, h1, hstep, h2⟩ :=
    mutexRuns_split hrun pre (compObs MutexSig (Caller.fiber g) c (MutexRes.rbool true)) suf
      hsplit
  obtain ⟨-, hbalA, -, -, -, -, -, hextMA⟩ :=
    mutex_mirror h1 (by simp [primInit, mutexInit]) (fun _ hx => by cases hx)
      (fun d hd => by simp [primInit] at hd) (fun r hr _ => by cases hr)
      (fun p hp => by cases hp) (fun e he his => by cases he)
  have hcredit := mutexStep_grant_credit hstep hc rfl hextMA
  simp only [primInit, retGrantAll_none, retUnlockAll_none, mstate, recAll_init,
    ownerHeld_init] at hbalA
  omega

/-! ## THEOREM B: irreducibility to the semaphore base -/

/-- The frozen base for this stage: the Stage-2 semaphore core as base
capabilities (`BASE(AsyncMutex) = {Semaphore}`).  The oracle facets are
the adjudicated `semPrim` model's own relations; composition
faithfulness is the base stage's model-to-code mapping and verdict. -/
def SemaphoreOps : BaseOpsSig where
  Op := SemCall
  Res := fun _ => SemResult
  St := SemState
  baseInit := semPrim.init
  run := fun o s t f => semPrim.run s t f o
  -- the base-op park drops wakes: the encoding machine's base invocations
  -- readies nothing (irrelevant to the over-production separator)
  park := fun o s f => (semPrim.park s f o).map (fun p => p.1)
  resume := fun o s f => semPrim.finish s f o

/-- Non-vacuity witness for the THEOREM-B universal: the closed encoding
class over the semaphore substrate is inhabited.  The programs are the
natural binary-semaphore shapes (`lock` → `acquire`, `unlock` →
`release`); the base has no operation for `try_lock`, and `cancel` —
whose observable is the queued waiter's outcome, not a value this
substrate can carry — has no decoding, so those programs bottom out with
no completed observation.  Inhabitance is what THEOREM B needs: the
over-production fires at the encoding machine's dispatch step, before
any program runs, so it binds this encoding — the closest an
implementation can get to a mutex over the base — exactly as it binds
every other element of the class. -/
def encSemMutex : Encoding SemaphoreOps MutexSig where
  prog
    | MutexCall.mlock => ExtProg.base SemCall.acquire (fun _ => ExtProg.pure (SubVal.bool true))
    | MutexCall.munlock => ExtProg.base SemCall.release (fun _ => ExtProg.pure (SubVal.bool true))
    | _ => ExtProg.pure (SubVal.bool false)
  decode
    | MutexCall.mlock, SubVal.bool true => some (MutexRes.rbool true)
    | MutexCall.munlock, SubVal.bool true => some MutexRes.runit
    | _, _ => none
  extCap
    | MutexCall.mcancel _ => true
    | _ => false

theorem encoding_class_inhabited :
    ∃ _enc : Encoding SemaphoreOps MutexSig, True := ⟨encSemMutex, trivial⟩

/-- The separating witness: a bare `unlock` issue by a fresh fiber.  The
owner-gated admission of `unlock` (the caller precondition the primitive
enforces at dispatch) refuses it, while every encoding's fiber machine
dispatches any submitted call unconditionally. -/
def unlockIssueTrace : Trace MutexSig :=
  [issueObs MutexSig (Caller.fiber 0) MutexCall.munlock]

theorem seqOK_unlockIssue : SeqOK MutexSig unlockIssueTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- A silent prefix from an idle configuration leaves this primitive's
private state untouched: with no fiber dispatched and no external call in
flight, the only silent steps are submits and the inert clock tick. -/
theorem mutex_silent_prefix {cfg m : PrimCfg MutexSig mutexPrim} {t : Trace MutexSig}
    (hrun : PrimRuns2 MutexSig mutexPrim cfg t m) (ht : t = [])
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
                exact absurd hexp (by simp [mutexPrim])
          have hip := ih ht2 hfwd.2.1 hfwd.2.2.1 hfwd.2.2.2
          rw [hip]
          exact hfwd.1
      | some ob => simp at hnil

theorem mutex_not_unlockIssue : ¬ TracesPrim MutexSig mutexPrim unlockIssueTrace := by
  rintro ⟨fin, hrun⟩
  obtain ⟨ob, t', heq⟩ : ∃ ob t', unlockIssueTrace = ob :: t' := ⟨_, _, rfl⟩
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := primRuns_cons hrun ob t' heq
  have hprim := mutex_silent_prefix h1 rfl rfl rfl (by intro r hr; cases hr)
  cases hstep with
  | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
      injection heq with h3 h4
      simp only [issueObs, Obs.mk.injEq, Caller.fiber.injEq] at h3
      obtain ⟨h5, h6, _⟩ := h3
      have hcall : rp.call = MutexCall.munlock := h6.symm
      have hfib : rp.fiber = 0 := h5.symm
      rw [hcall, hfib, hprim] at hadmit
      simp [mutexAdmit, mutexInit, mutexPrim, primInit] at hadmit
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
def owMid (enc : Encoding SemaphoreOps MutexSig) : SysCfg SemaphoreOps MutexSig :=
  { st := subInit
    bst := SemaphoreOps.baseInit
    cur := none
    parked := []
    runq := [{ fiber := 0, call := MutexCall.munlock, prog := enc.prog MutexCall.munlock, fresh := true }]
    retired := []
    nextFiber := 1
    exts := [] }

/-- The post-dispatch configuration of the encoding witness. -/
def owFin (enc : Encoding SemaphoreOps MutexSig) : SysCfg SemaphoreOps MutexSig :=
  { st := subInit
    bst := SemaphoreOps.baseInit
    cur := some { fiber := 0, call := MutexCall.munlock, prog := enc.prog MutexCall.munlock }
    parked := []
    runq := []
    retired := []
    nextFiber := 1
    exts := [] }

theorem enc_unlockIssue (enc : Encoding SemaphoreOps MutexSig) :
    TracesEnc SemaphoreOps MutexSig enc unlockIssueTrace :=
  ⟨owFin enc,
    SysRuns.step (encInit SemaphoreOps MutexSig) (owMid enc) none _ (owFin enc)
      (SysStep.submit (encInit SemaphoreOps MutexSig) 0 MutexCall.munlock (Or.inl rfl))
      (SysRuns.step (owMid enc) (owFin enc)
        (some (issueObs MutexSig (Caller.fiber 0) MutexCall.munlock)) [] (owFin enc)
        (SysStep.dispatchFresh (owMid enc)
          { fiber := 0, call := MutexCall.munlock, prog := enc.prog MutexCall.munlock,
            fresh := true } [] rfl rfl rfl)
        (SysRuns.stop (owFin enc)))⟩

/-- Every encoding over the semaphore base produces the bare `unlock`
issue: the discipline's fiber machine submits and dispatches any call
unconditionally. -/
theorem mutex_over_produces (enc : Encoding SemaphoreOps MutexSig) :
    OverProduces mutexPrim SemaphoreOps enc :=
  ⟨unlockIssueTrace, ⟨enc_unlockIssue enc, seqOK_unlockIssue⟩,
    fun hP => mutex_not_unlockIssue hP.1⟩

/-- **THEOREM B** — AsyncMutex is irreducible to `BASE(AsyncMutex) =
{Semaphore}`: every encoding over the base over-produces the bare
`unlock` issue that the primitive's owner-gated admission forbids. -/
theorem mutex_irreducible : IrreducibleTo mutexPrim SemaphoreOps :=
  irreducible_of_always_over mutex_over_produces

/-! ## Possession batteries -/

/-- The inline-grant witness: a fresh `lock` on the free mutex issues and
completes `true`, taking ownership. -/
def grantTrace : Trace MutexSig :=
  [issueObs MutexSig (Caller.fiber 0) MutexCall.mlock,
   compObs MutexSig (Caller.fiber 0) MutexCall.mlock (MutexRes.rbool true)]

def gw1 : PrimCfg MutexSig mutexPrim := primInit MutexSig mutexPrim

def gw2 : PrimCfg MutexSig mutexPrim :=
  { prim := mutexInit, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := MutexCall.mlock, fresh := true }], retired := [],
    nextFiber := 1, exts := [] }

def gw3 : PrimCfg MutexSig mutexPrim :=
  { prim := mutexInit, now := 0,
    cur := some (FSlot.running { fiber := 0, call := MutexCall.mlock } false), parked := [],
    runq := [], retired := [], nextFiber := 1, exts := [] }

def gw4 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := MutexCall.mlock } (MutexRes.rbool true)),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def gw5 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem gws1 : PrimStep2 MutexSig mutexPrim gw1 none gw2 :=
  PrimStep2.submit gw1 0 MutexCall.mlock (Or.inl rfl)

theorem gws2 : PrimStep2 MutexSig mutexPrim gw2
    (some (issueObs MutexSig (Caller.fiber 0) MutexCall.mlock)) gw3 :=
  PrimStep2.dispatchFresh gw2 { fiber := 0, call := MutexCall.mlock, fresh := true } []
    mutexInit rfl rfl rfl rfl

theorem gws3 : PrimStep2 MutexSig mutexPrim gw3 none gw4 :=
  PrimStep2.fiberEffect gw3 { fiber := 0, call := MutexCall.mlock } false [] []
    (MutexRes.rbool true) { owner := some 0, waitq := [], resolved := [] } []
    rfl rfl rfl rfl Wakes.nil

theorem gws4 : PrimStep2 MutexSig mutexPrim gw4
    (some (compObs MutexSig (Caller.fiber 0) MutexCall.mlock (MutexRes.rbool true))) gw5 :=
  PrimStep2.fiberDone gw4 { fiber := 0, call := MutexCall.mlock } (MutexRes.rbool true) rfl

theorem grantRun : PrimRuns2 MutexSig mutexPrim gw1 grantTrace gw5 :=
  PrimRuns2.step gw1 gw2 none _ gw5 gws1
    (PrimRuns2.step gw2 gw3 _ _ gw5 gws2
      (PrimRuns2.step gw3 gw4 none _ gw5 gws3
        (PrimRuns2.step gw4 gw5 _ [] gw5 gws4 (PrimRuns2.stop gw5))))

theorem seqOK_grant : SeqOK MutexSig grantTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- The modeled core is not vacuous: it realizes the two-observation
inline grant. -/
theorem mutex_possesses_grant : Possesses mutexPrim fun t => t = grantTrace :=
  ⟨grantTrace, ⟨⟨gw5, grantRun⟩, seqOK_grant⟩, rfl⟩

/-- The cancel witness: a held lock, a queued locker cancelled by an
external caller, and the locker's resumed `false` completion — the
park / external-entry / external-section / resume-consume spine. -/
def cancelTrace : Trace MutexSig :=
  [issueObs MutexSig (Caller.fiber 0) MutexCall.mlock,
   compObs MutexSig (Caller.fiber 0) MutexCall.mlock (MutexRes.rbool true),
   issueObs MutexSig (Caller.fiber 1) MutexCall.mlock,
   issueObs MutexSig (Caller.ext 0) (MutexCall.mcancel 1),
   compObs MutexSig (Caller.ext 0) (MutexCall.mcancel 1) (MutexRes.rbool true),
   compObs MutexSig (Caller.fiber 1) MutexCall.mlock (MutexRes.rbool false)]

def cw0 : PrimCfg MutexSig mutexPrim := primInit MutexSig mutexPrim

def cw1 : PrimCfg MutexSig mutexPrim :=
  { prim := mutexInit, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := MutexCall.mlock, fresh := true }], retired := [],
    nextFiber := 1, exts := [] }

def cw2 : PrimCfg MutexSig mutexPrim :=
  { prim := mutexInit, now := 0,
    cur := some (FSlot.running { fiber := 0, call := MutexCall.mlock } false), parked := [],
    runq := [], retired := [], nextFiber := 1, exts := [] }

def cw3 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := MutexCall.mlock } (MutexRes.rbool true)),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def cw4 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

def cw5 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 1, call := MutexCall.mlock, fresh := true }],
    retired := [0], nextFiber := 2, exts := [] }

def cw6 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0,
    cur := some (FSlot.running { fiber := 1, call := MutexCall.mlock } false), parked := [],
    runq := [], retired := [0], nextFiber := 2, exts := [] }

def cw7 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [1], resolved := [] }, now := 0, cur := none,
    parked := [{ fiber := 1, call := MutexCall.mlock }], runq := [], retired := [0],
    nextFiber := 2, exts := [] }

def cw8 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [1], resolved := [] }, now := 0, cur := none,
    parked := [{ fiber := 1, call := MutexCall.mlock }], runq := [], retired := [0],
    nextFiber := 2, exts := [{ x := 0, call := MutexCall.mcancel 1, result := none }] }

def cw9 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [(1, false)] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 1, call := MutexCall.mlock, fresh := false }],
    retired := [0], nextFiber := 2,
    exts := [{ x := 0, call := MutexCall.mcancel 1, result := some (MutexRes.rbool true) }] }

def cw9b : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [(1, false)] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 1, call := MutexCall.mlock, fresh := false }],
    retired := [0], nextFiber := 2, exts := [] }

def cw10 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [(1, false)] }, now := 0,
    cur := some (FSlot.running { fiber := 1, call := MutexCall.mlock } true), parked := [],
    runq := [], retired := [0], nextFiber := 2, exts := [] }

def cw11 : PrimCfg MutexSig mutexPrim :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [1, 0], nextFiber := 2, exts := [] }

theorem cws1 : PrimStep2 MutexSig mutexPrim cw0 none cw1 :=
  PrimStep2.submit cw0 0 MutexCall.mlock (Or.inl rfl)

theorem cws2 : PrimStep2 MutexSig mutexPrim cw1
    (some (issueObs MutexSig (Caller.fiber 0) MutexCall.mlock)) cw2 :=
  PrimStep2.dispatchFresh cw1 { fiber := 0, call := MutexCall.mlock, fresh := true } []
    mutexInit rfl rfl rfl rfl

theorem cws3 : PrimStep2 MutexSig mutexPrim cw2 none cw3 :=
  PrimStep2.fiberEffect cw2 { fiber := 0, call := MutexCall.mlock } false [] []
    (MutexRes.rbool true) { owner := some 0, waitq := [], resolved := [] } []
    rfl rfl rfl rfl Wakes.nil

theorem cws4 : PrimStep2 MutexSig mutexPrim cw3
    (some (compObs MutexSig (Caller.fiber 0) MutexCall.mlock (MutexRes.rbool true))) cw4 :=
  PrimStep2.fiberDone cw3 { fiber := 0, call := MutexCall.mlock } (MutexRes.rbool true) rfl

theorem cws5 : PrimStep2 MutexSig mutexPrim cw4 none cw5 :=
  PrimStep2.submit cw4 1 MutexCall.mlock (Or.inl rfl)

theorem cws6 : PrimStep2 MutexSig mutexPrim cw5
    (some (issueObs MutexSig (Caller.fiber 1) MutexCall.mlock)) cw6 :=
  PrimStep2.dispatchFresh cw5 { fiber := 1, call := MutexCall.mlock, fresh := true } []
    { owner := some 0, waitq := [], resolved := [] } rfl rfl rfl rfl

theorem cws7 : PrimStep2 MutexSig mutexPrim cw6 none cw7 :=
  PrimStep2.runPark cw6 { fiber := 1, call := MutexCall.mlock } false [] []
    { owner := some 0, waitq := [1], resolved := [] } [] rfl
    rfl rfl rfl Wakes.nil

theorem cws8 : PrimStep2 MutexSig mutexPrim cw7
    (some (issueObs MutexSig (Caller.ext 0) (MutexCall.mcancel 1))) cw8 :=
  PrimStep2.extApply cw7 0 (MutexCall.mcancel 1) [] [] rfl
    (by show (0 : ExternalId) ∉ ([] : List (ExtPend MutexSig)).map
          (fun e : ExtPend MutexSig => e.x)
        simp)

theorem cws9 : PrimStep2 MutexSig mutexPrim cw8 none cw9 :=
  PrimStep2.extEffect cw8 [] []
    { x := 0, call := MutexCall.mcancel 1, result := none }
    [{ fiber := 1, call := MutexCall.mlock }] []
    (MutexRes.rbool true) { owner := some 0, waitq := [], resolved := [(1, false)] } [1]
    rfl rfl rfl rfl (Wakes.drop _ Wakes.nil)

theorem cws9b : PrimStep2 MutexSig mutexPrim cw9
    (some (compObs MutexSig (Caller.ext 0) (MutexCall.mcancel 1) (MutexRes.rbool true))) cw9b :=
  PrimStep2.extDone cw9 [] []
    { x := 0, call := MutexCall.mcancel 1, result := some (MutexRes.rbool true) }
    (MutexRes.rbool true) rfl rfl

theorem cws10 : PrimStep2 MutexSig mutexPrim cw9b none cw10 :=
  PrimStep2.dispatchResumed cw9b
    { fiber := 1, call := MutexCall.mlock, fresh := false } [] rfl rfl rfl

theorem cws11 : PrimStep2 MutexSig mutexPrim cw10
    (some (compObs MutexSig (Caller.fiber 1) MutexCall.mlock (MutexRes.rbool false))) cw11 :=
  PrimStep2.finishDone cw10 { fiber := 1, call := MutexCall.mlock } true [] []
    (MutexRes.rbool false) { owner := some 0, waitq := [], resolved := [] } []
    rfl rfl rfl rfl Wakes.nil

theorem cancelRun : PrimRuns2 MutexSig mutexPrim cw0 cancelTrace cw11 :=
  PrimRuns2.step cw0 cw1 none _ cw11 cws1
    (PrimRuns2.step cw1 cw2 _ _ cw11 cws2
      (PrimRuns2.step cw2 cw3 none _ cw11 cws3
        (PrimRuns2.step cw3 cw4 _ _ cw11 cws4
          (PrimRuns2.step cw4 cw5 none _ cw11 cws5
            (PrimRuns2.step cw5 cw6 _ _ cw11 cws6
              (PrimRuns2.step cw6 cw7 none _ cw11 cws7
                (PrimRuns2.step cw7 cw8 _ _ cw11 cws8
                  (PrimRuns2.step cw8 cw9 none _ cw11 cws9
                    (PrimRuns2.step cw9 cw9b _ _ cw11 cws9b
                      (PrimRuns2.step cw9b cw10 none _ cw11 cws10
                        (PrimRuns2.step cw10 cw11 _ [] cw11 cws11
                          (PrimRuns2.stop cw11))))))))))))

theorem seqOK_cancel : SeqOK MutexSig cancelTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem mutex_possesses_cancel : Possesses mutexPrim fun t => t = cancelTrace :=
  ⟨cancelTrace, ⟨⟨cw11, cancelRun⟩, seqOK_cancel⟩, rfl⟩

/-! ## The negative mutant -/

/-- The mutant: the inline `lock` grant ignores the owner slot — it
grants whenever the queue is empty, even over a held mutex (fault
class: ownership gating). -/
def mutexMutantRun : MutexState → Tick → FiberId → MutexCall →
    Option (MutexRes × MutexState × List FiberId)
  | s, _, f, MutexCall.mlock =>
      if s.waitq = [] then
        some (MutexRes.rbool true, { owner := some f, waitq := [], resolved := s.resolved }, [])
      else if s.owner = some f then some (MutexRes.rbool true, s, [])
      else none
  | s, t, f, c => mutexRun s t f c

def mutexMutant : PrimLTS2 MutexSig := { mutexPrim with run := mutexMutantRun }

def mutexMutantTrace : Trace MutexSig :=
  [issueObs MutexSig (Caller.fiber 0) MutexCall.mlock,
   compObs MutexSig (Caller.fiber 0) MutexCall.mlock (MutexRes.rbool true),
   issueObs MutexSig (Caller.fiber 1) MutexCall.mlock,
   compObs MutexSig (Caller.fiber 1) MutexCall.mlock (MutexRes.rbool true)]

def xw0 : PrimCfg MutexSig mutexMutant := primInit MutexSig mutexMutant

def xw1 : PrimCfg MutexSig mutexMutant :=
  { prim := mutexInit, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := MutexCall.mlock, fresh := true }], retired := [],
    nextFiber := 1, exts := [] }

def xw2 : PrimCfg MutexSig mutexMutant :=
  { prim := mutexInit, now := 0,
    cur := some (FSlot.running { fiber := 0, call := MutexCall.mlock } false), parked := [],
    runq := [], retired := [], nextFiber := 1, exts := [] }

def xw3 : PrimCfg MutexSig mutexMutant :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 0, call := MutexCall.mlock } (MutexRes.rbool true)),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def xw4 : PrimCfg MutexSig mutexMutant :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

def xw5 : PrimCfg MutexSig mutexMutant :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0, cur := none,
    parked := [], runq := [{ fiber := 1, call := MutexCall.mlock, fresh := true }],
    retired := [0], nextFiber := 2, exts := [] }

def xw6 : PrimCfg MutexSig mutexMutant :=
  { prim := { owner := some 0, waitq := [], resolved := [] }, now := 0,
    cur := some (FSlot.running { fiber := 1, call := MutexCall.mlock } false), parked := [],
    runq := [], retired := [0], nextFiber := 2, exts := [] }

def xw7 : PrimCfg MutexSig mutexMutant :=
  { prim := { owner := some 1, waitq := [], resolved := [] }, now := 0,
    cur := some (FSlot.returning { fiber := 1, call := MutexCall.mlock } (MutexRes.rbool true)),
    parked := [], runq := [], retired := [0], nextFiber := 2, exts := [] }

def xw8 : PrimCfg MutexSig mutexMutant :=
  { prim := { owner := some 1, waitq := [], resolved := [] }, now := 0, cur := none,
    parked := [], runq := [], retired := [1, 0], nextFiber := 2, exts := [] }

theorem xws1 : PrimStep2 MutexSig mutexMutant xw0 none xw1 :=
  PrimStep2.submit xw0 0 MutexCall.mlock (Or.inl rfl)

theorem xws2 : PrimStep2 MutexSig mutexMutant xw1
    (some (issueObs MutexSig (Caller.fiber 0) MutexCall.mlock)) xw2 :=
  PrimStep2.dispatchFresh xw1 { fiber := 0, call := MutexCall.mlock, fresh := true } []
    mutexInit rfl rfl rfl rfl

theorem xws3 : PrimStep2 MutexSig mutexMutant xw2 none xw3 :=
  PrimStep2.fiberEffect xw2 { fiber := 0, call := MutexCall.mlock } false [] []
    (MutexRes.rbool true) { owner := some 0, waitq := [], resolved := [] } []
    rfl rfl rfl rfl Wakes.nil

theorem xws4 : PrimStep2 MutexSig mutexMutant xw3
    (some (compObs MutexSig (Caller.fiber 0) MutexCall.mlock (MutexRes.rbool true))) xw4 :=
  PrimStep2.fiberDone xw3 { fiber := 0, call := MutexCall.mlock } (MutexRes.rbool true) rfl

theorem xws5 : PrimStep2 MutexSig mutexMutant xw4 none xw5 :=
  PrimStep2.submit xw4 1 MutexCall.mlock (Or.inl rfl)

theorem xws6 : PrimStep2 MutexSig mutexMutant xw5
    (some (issueObs MutexSig (Caller.fiber 1) MutexCall.mlock)) xw6 :=
  PrimStep2.dispatchFresh xw5 { fiber := 1, call := MutexCall.mlock, fresh := true } []
    { owner := some 0, waitq := [], resolved := [] } rfl rfl rfl rfl

theorem xws7 : PrimStep2 MutexSig mutexMutant xw6 none xw7 :=
  PrimStep2.fiberEffect xw6 { fiber := 1, call := MutexCall.mlock } false [] []
    (MutexRes.rbool true) { owner := some 1, waitq := [], resolved := [] } []
    rfl rfl rfl rfl Wakes.nil

theorem xws8 : PrimStep2 MutexSig mutexMutant xw7
    (some (compObs MutexSig (Caller.fiber 1) MutexCall.mlock (MutexRes.rbool true))) xw8 :=
  PrimStep2.fiberDone xw7 { fiber := 1, call := MutexCall.mlock } (MutexRes.rbool true) rfl

theorem mutantWitnessRun : PrimRuns2 MutexSig mutexMutant xw0 mutexMutantTrace xw8 :=
  PrimRuns2.step xw0 xw1 none _ xw8 xws1
    (PrimRuns2.step xw1 xw2 _ _ xw8 xws2
      (PrimRuns2.step xw2 xw3 none _ xw8 xws3
        (PrimRuns2.step xw3 xw4 _ _ xw8 xws4
          (PrimRuns2.step xw4 xw5 none _ xw8 xws5
            (PrimRuns2.step xw5 xw6 _ _ xw8 xws6
              (PrimRuns2.step xw6 xw7 none _ xw8 xws7
                (PrimRuns2.step xw7 xw8 _ [] xw8 xws8 (PrimRuns2.stop xw8))))))))

theorem seqOK_mutexMutant : SeqOK MutexSig mutexMutantTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

theorem mutexMutant_produces : TracesPrimS MutexSig mutexMutant mutexMutantTrace :=
  ⟨⟨xw8, mutantWitnessRun⟩, seqOK_mutexMutant⟩

def mutantPre : Trace MutexSig :=
  [issueObs MutexSig (Caller.fiber 0) MutexCall.mlock,
   compObs MutexSig (Caller.fiber 0) MutexCall.mlock (MutexRes.rbool true),
   issueObs MutexSig (Caller.fiber 1) MutexCall.mlock]

theorem mutantPre_grants : grantsAll mutantPre = 1 := by decide

theorem mutantPre_releases : releasesAll mutantPre = 0 := by decide

/-- The mutant breaks the exclusion guarantee: the second grant completes
with one grant and no release behind it. -/
theorem mutexMutant_not_guarantees : ¬ Guarantees mutexMutant mutexExclHonored := by
  intro hgu
  have h := hgu mutexMutantTrace mutexMutant_produces mutantPre 1 MutexCall.mlock []
    (by rfl) (Or.inl rfl)
  have hg := mutantPre_grants
  have hr := mutantPre_releases
  omega

end Sluice.Formal
