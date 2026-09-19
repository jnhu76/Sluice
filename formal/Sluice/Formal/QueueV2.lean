/-
Stage 6V2.3 (FCB1-POST-V23-STACK-379-385): the AsyncQueue primitive's
transition system, mapping `src/async/scheduler_queue.cpp`,
`src/async/queue_port.cpp`, and `include/sluice/async/async_queue.hpp`
one section per call, under the post-#378 Stage-0-V2.3 execution-domain
calculus (`CalcV2.lean`, `PrimStep2`).  Chain: Stage-0 V2.3 → Event →
Semaphore (#378) → Mutex (#379) → Condition (#380) → RwLock (#381) →
this queue replay (#382).

Capability disposition.  The pre-V2.3 stage-6 verdict (THEOREM B)
rested on the completion-shadow projection, which
`tracesEnc_shadow_false` (CalcV2.lean) proves invalid under the
symmetric V2.3 per-caller discipline; the re-adjudication disposition
retires that proof and records the capability verdict RESEARCH/DEFER.
This file therefore establishes the queue's own semantics and does NOT
claim `Reducible` or `IrreducibleTo`.  The judgment-layer section at
the bottom proves the two conditional (per-encoding-class) mismatch
statements the frozen method does support, and records that neither
closes the universal question.  `BASE(AsyncQueue) = {Semaphore}` -- the
Stage-0 §8 default, unchanged.

Call-domain census (from the code):

  push v      = fiber-bound (`queue_push_admit` :199-200 asserts a
                running Fiber; the section: inline commit when open, not
                full, and the fresh node lands at the head of the
                producer queue (:56-73), completing `committed` and then
                running the consumer cross-grant (:216-218); inline
                `closed` when the bit stands (:75-87); suspend on a full
                open queue)
  pop         = fiber-bound (`queue_pop_admit` :243-244; inline delivery
                when the ring is non-empty and the fresh node lands at
                the consumer queue's head (:128-146), completing
                `item v` and then running the producer cross-grant
                (:258-259); inline `closed` when empty and closed
                (:148-160); suspend on an empty open queue)
  try_push v  = external-capable (QueuePort::try_push, no worker read,
                queue_port.cpp:125-164; `closed` :147-150,
                `would_block` :152-155, commit :157-160 plus the
                consumer cross-grant :162 -- no producer-queue check:
                the try barges past suspended producers)
  try_pop     = external-capable (queue_port.cpp:166-198; item +
                producer cross-grant :180-190, `closed` :193-195,
                `would_block` :197)
  close       = external-capable (queue_port.cpp:200-218; sets the bit
                :214, then drains consumers then producers :216-217 --
                the drain wakes every suspended consumer with the ring
                head while items remain (`queue_grant_consumer_locked`
                :391-397) and every suspended producer reads closed
                (the :417 gate on the bit))

`Scheduler::queue_cancel` (scheduler_queue.cpp:367-379) takes the
caller's own `WaitNode&` and is reached only from
`scheduler_test_access.hpp` -- no public `AsyncQueue` path issues it,
so it is outside the core surface.  The timed paths
(`push_until`/`pop_until` and the timed admission legs,
scheduler_queue.cpp:283-365, :37-99, :108-172), the teardown session
(queue_port.cpp:311-361), the lease location machine, the snapshot
reads, and the re-entrancy audits are likewise outside the untimed
core verdict.

Modeling disclosures:

  * The role queues are two separate FIFOs in the C++
    (`waiters_[0]`/`waiters_[1]`); the model keeps `waitqP`/`waitqC`.
  * Suspended producers carry their items on the queue (the lease rides
    the node; `QueueWaitCtx::prod_lease`).
  * the entry facet is total (always `some`): registrations cannot
    reject (a fresh detached node), and closed/full/empty are *results*,
    not caller precondition aborts.
  * An inline grant additionally requires the fresh node to land at
    the role queue's head (`node.prev_ == nullptr`, :56/:128) -- in
    the model, the role queue being empty.
  * `try_push` barges: it never registers, so it commits past
    suspended producers.
  * `clog`/`dlog` are ghost logs: the ring-insertion order of committed
    items and the delivery order of handed-off items.  The C++ spreads
    this bookkeeping across `ring_count_` moves and the lease handoffs;
    every model step that commits or delivers appends to exactly the
    log the code's move corresponds to.
-/
import Sluice.Formal.RwLockV2

namespace Sluice.Formal

/-! ## API and state -/

inductive QItem : Type where
  | a
  | b
deriving instance DecidableEq, Inhabited for QItem

inductive QCall : Type where
  | qpush (v : QItem)
  | qpop
  | qtrypush (v : QItem)
  | qtrypop
  | qclose
deriving instance DecidableEq for QCall

inductive QRes : Type where
  | qCommitted
  | qWouldBlock
  | qClosed
  | qItem (v : QItem)
  | qDone
deriving instance DecidableEq for QRes

abbrev QSig : ApiSig := ⟨QCall, QRes⟩

/-- The outcome a granted waiter finds published at its resume: a
producer's `committed` (its lease entered the ring,
scheduler_queue.cpp:417-423), a consumer's delivered item (:391-397),
or the closed outcome both read when the grant found the queue closed
or unservable. -/
inductive QOut : Type where
  | oCommitted
  | oClosed
  | oItem (v : QItem)
deriving instance DecidableEq for QOut

/-- AsyncQueue private state: the ring in FIFO order (head first;
`ring_head_`/`ring_count_` are the C++ encoding), the closed bit, the
two role wait queues (suspended producers carry their items), the
resolved-but-unconsumed outcomes of granted waiters, and the ghost
commit/delivery logs. -/
structure QState where
  ring : List QItem
  closed : Bool
  waitqP : List (FiberId × QItem)
  waitqC : List FiberId
  resolved : List (FiberId × QOut)
  clog : List QItem
  dlog : List QItem

/-- The frozen core capacity: 2 (`QueuePort` rejects capacity 0,
queue_port.cpp:61-63; capacity is fixed per instance). -/
def qCap : Nat := 2

def qInit : QState := ⟨[], false, [], [], [], [], []⟩

/-- Remove the first record of `f`, yielding its outcome (the one-shot
consume of a resolved `WaitNode` at the resumed waiter's finish). -/
def qConsume : List (FiberId × QOut) → FiberId → Option (QOut × List (FiberId × QOut))
  | [], _ => none
  | (g, o) :: rest, f => if g = f then some (o, rest)
      else (fun p => (p.1, (g, o) :: p.2)) <$> qConsume rest f

/-! ## The cross-grants -/

/-- `queue_grant_consumer_locked` (scheduler_queue.cpp:381-405): wake
the head suspended consumer; the ring head hands off while items remain
(:391-397) and the delivery is logged, else the consumer's outcome
reads closed.  No suspended consumer = no effect. -/
def qGrantConsumer (s : QState) : QState × List FiberId :=
  match s.waitqC with
  | [] => (s, [])
  | f :: t =>
      match s.ring with
      | [] => ({ s with waitqC := t, resolved := s.resolved ++ [(f, QOut.oClosed)] }, [f])
      | v :: r => ({ s with ring := r, waitqC := t, dlog := s.dlog ++ [v], resolved := s.resolved ++ [(f, QOut.oItem v)] }, [f])

/-- `queue_grant_producer_locked` (scheduler_queue.cpp:407-432): wake
the head suspended producer; its lease enters the ring when the queue
stands open and not full (:417-423) and the commit is logged, else the
push reads closed.  No suspended producer = no effect. -/
def qGrantProducer (s : QState) : QState × List FiberId :=
  match s.waitqP with
  | [] => (s, [])
  | (f, v) :: t =>
      if s.closed = false ∧ s.ring.length < qCap then
        ({ s with waitqP := t, ring := s.ring ++ [v], clog := s.clog ++ [v], resolved := s.resolved ++ [(f, QOut.oCommitted)] }, [f])
      else
        ({ s with waitqP := t, resolved := s.resolved ++ [(f, QOut.oClosed)] }, [f])

/-! ## The close drain -/

/-- The consumer drain of `close` (queue_port.cpp:216): each woken
consumer receives the ring head while items remain, else the closed
outcome. -/
def qDrainC : List QItem → List FiberId → List (FiberId × QOut) →
    List QItem × List FiberId × List (FiberId × QOut)
  | ring, [], res => (ring, [], res)
  | [], f :: t, res => qDrainC [] t (res ++ [(f, QOut.oClosed)])
  | v :: r, f :: t, res => qDrainC r t (res ++ [(f, QOut.oItem v)])

/-- The producer drain of `close` (queue_port.cpp:217): the bit stands
set, so no suspended lease commits (:417-423); every woken producer
reads closed. -/
def qDrainP : List (FiberId × QItem) → List (FiberId × QOut) →
    List (FiberId × QItem) × List (FiberId × QOut)
  | [], res => ([], res)
  | (f, _) :: t, res => qDrainP t (res ++ [(f, QOut.oClosed)])

/-- The drain removes ring items only from the front: the residual ring
is a suffix `ring.drop n` (with `n` at most the drained ring's length),
and consumers survive the drain only while the ring is empty -- the
pairing order of the recursion hands items and consumers out together. -/
theorem qDrainC_drop : ∀ (cons : List FiberId) (ring : List QItem)
    (res res' : List (FiberId × QOut)) (r' : List QItem) (c' : List FiberId),
    qDrainC ring cons res = (r', c', res') →
      ∃ n, r' = ring.drop n ∧ n ≤ ring.length ∧ (c' ≠ [] → ring.drop n = []) := by
  intro cons
  induction cons with
  | nil =>
      intro ring res res' r' c' h
      simp only [qDrainC, Prod.mk.injEq] at h
      obtain ⟨h1, h2, h3⟩ := h
      subst h1
      subst h2
      exact ⟨0, rfl, Nat.zero_le _, by intro hc; exact absurd rfl hc⟩
  | cons g t ih =>
      intro ring res res' r' c' h
      cases ring with
      | nil =>
          simp only [qDrainC] at h
          obtain ⟨n, hn, hle, hd⟩ :=
            ih [] (res ++ [(g, QOut.oClosed)]) res' r' c' h
          refine ⟨0, by rw [hn]; simp, Nat.zero_le _, ?_⟩
          intro hc
          simpa using hd hc
      | cons v r =>
          simp only [qDrainC] at h
          obtain ⟨n, hn, hle, hd⟩ :=
            ih r (res ++ [(g, QOut.oItem v)]) res' r' c' h
          refine ⟨n + 1, by rw [hn]; simp, by simp only [List.length_cons]; omega, ?_⟩
          intro hc
          simpa using hd hc

/-! ## The primitive -/

/-- Entry gate: no caller-precondition aborts exist on this surface.
Registrations cannot reject (a fresh detached node), and closed /
full / empty are results, not aborts -- every call is admissible. -/
def qAdmit : QState → FiberId → QCall → Option QState
  | s, _, _ => some s

/-- The inline paths of a dispatched call.

  `push v` -- inline `committed` when open, not full, and the fresh
  node lands at the producer queue's head (:56-73), the consumer
  cross-grant following (:216-218); inline `closed` when the bit
  stands (:75-87); suspend on a full open queue.

  `pop` -- inline `item v` when the ring is non-empty and the fresh
  node lands at the consumer queue's head (:128-146), the producer
  cross-grant following (:258-259); inline `closed` when empty and
  closed (:148-160); suspend on an empty open queue.

  `try_push` / `try_pop` / `close` -- the caller-agnostic sections
  (queue_port.cpp:147-164, :180-198, :214-217). -/
def qRun : QState → Tick → FiberId → QCall →
    Option (QRes × QState × List FiberId)
  | s, _, _, QCall.qpush v =>
      if s.closed then some (QRes.qClosed, s, [])
      else if s.ring.length < qCap ∧ s.waitqP = [] then
        match qGrantConsumer { s with ring := s.ring ++ [v], clog := s.clog ++ [v] } with
        | (s2, ws2) => some (QRes.qCommitted, s2, ws2)
      else none
  | s, _, _, QCall.qpop =>
      match s.ring with
      | [] => if s.closed then some (QRes.qClosed, s, []) else none
      | v :: r =>
          if s.waitqC = [] then
            match qGrantProducer { s with ring := r, dlog := s.dlog ++ [v] } with
            | (s2, ws2) => some (QRes.qItem v, s2, ws2)
          else none
  | s, _, _, QCall.qtrypush v =>
      if s.closed then some (QRes.qClosed, s, [])
      else if s.ring.length < qCap then
        match qGrantConsumer { s with ring := s.ring ++ [v], clog := s.clog ++ [v] } with
        | (s2, ws2) => some (QRes.qCommitted, s2, ws2)
      else some (QRes.qWouldBlock, s, [])
  | s, _, _, QCall.qtrypop =>
      match s.ring with
      | [] =>
          if s.closed then some (QRes.qClosed, s, [])
          else some (QRes.qWouldBlock, s, [])
      | v :: r =>
          if s.waitqC = [] then
            match qGrantProducer { s with ring := r, dlog := s.dlog ++ [v] } with
            | (s2, ws2) => some (QRes.qItem v, s2, ws2)
          else none
  | s, _, _, QCall.qclose =>
      let d1 := qDrainC s.ring s.waitqC []
      let d2 := qDrainP s.waitqP d1.2.2
      some (QRes.qDone,
        { s with ring := d1.1, waitqC := d1.2.1, waitqP := d2.1, closed := true, resolved := s.resolved ++ d2.2, dlog := s.dlog ++ s.ring.take (s.ring.length - d1.1.length) },
        -- the drain publishes every drained waiter runnable, consumers
      -- before producers (the resolutions of the drain loop)
      s.waitqC ++ s.waitqP.map (fun p : FiberId × QItem => p.1))

/-- `push` and `pop` suspend; parking appends the caller to its role
queue (producers carry their items; `register_wait_locked` +
`commit_suspend_locked`, :40-56/:117-126 plus :223/:265).  A fiber
already queued is not re-registered (one node per fiber; the
one-in-flight discipline). -/
def qPark : QState → FiberId → QCall →
    Option (QState × List FiberId)
  | s, f, QCall.qpush v =>
      if (s.waitqP.any fun p => p.1 = f) ∨ (s.waitqC.any fun g => g = f) then none
      else some ({ s with waitqP := s.waitqP ++ [(f, v)] }, [])
  | s, f, QCall.qpop =>
      if (s.waitqP.any fun p => p.1 = f) ∨ (s.waitqC.any fun g => g = f) then none
      else some ({ s with waitqC := s.waitqC ++ [f] }, [])
  | _, _, _ => none

/-- A resumed waiter consumes its recorded outcome; nothing else
resumes (the grant was applied by the granter's section). -/
def qFinish : QState → FiberId → QCall →
    Option (QRes × QState × List FiberId)
  | s, f, (QCall.qpush _) =>
      match qConsume s.resolved f with
      | some (QOut.oCommitted, rest) =>
          some (QRes.qCommitted, { s with resolved := rest }, [])
      | some (QOut.oClosed, rest) =>
          some (QRes.qClosed, { s with resolved := rest }, [])
      | _ => none
  | s, f, QCall.qpop =>
      match qConsume s.resolved f with
      | some (QOut.oItem v, rest) =>
          some (QRes.qItem v, { s with resolved := rest }, [])
      | some (QOut.oClosed, rest) =>
          some (QRes.qClosed, { s with resolved := rest }, [])
      | _ => none
  | _, _, _ => none

/-- `try_push`, `try_pop`, and `close` are external-capable (no
`g_worker` read in their entry points); `push` and `pop` assert a
running Fiber (:199-200, :243-244). -/
def qExtCap : QCall → Bool
  | QCall.qtrypush _ => true
  | QCall.qtrypop => true
  | QCall.qclose => true
  | _ => false

/-- The external critical sections: the caller-agnostic try and close
bodies (identical to the fiber paths above; the sections read no
worker state). -/
def qExtRun : QCall → QState → Tick →
    Option (QRes × QState × List FiberId)
  | QCall.qtrypush v, s, _ =>
      if s.closed then some (QRes.qClosed, s, [])
      else if s.ring.length < qCap then
        match qGrantConsumer { s with ring := s.ring ++ [v], clog := s.clog ++ [v] } with
        | (s2, ws2) => some (QRes.qCommitted, s2, ws2)
      else some (QRes.qWouldBlock, s, [])
  | QCall.qtrypop, s, _ =>
      match s.ring with
      | [] =>
          if s.closed then some (QRes.qClosed, s, [])
          else some (QRes.qWouldBlock, s, [])
      | v :: r =>
          if s.waitqC = [] then
            match qGrantProducer { s with ring := r, dlog := s.dlog ++ [v] } with
            | (s2, ws2) => some (QRes.qItem v, s2, ws2)
          else none
  | QCall.qclose, s, _ =>
      let d1 := qDrainC s.ring s.waitqC []
      let d2 := qDrainP s.waitqP d1.2.2
      some (QRes.qDone,
        { s with ring := d1.1, waitqC := d1.2.1, waitqP := d2.1, closed := true, resolved := s.resolved ++ d2.2, dlog := s.dlog ++ s.ring.take (s.ring.length - d1.1.length) },
        -- the drain publishes every drained waiter runnable, consumers
      -- before producers (the resolutions of the drain loop)
      s.waitqC ++ s.waitqP.map (fun p : FiberId × QItem => p.1))
  | _, _, _ => none

/-- The queue has no timers in the untimed core. -/
def qExpire : QState → Tick → FiberId → Option QState :=
  fun _ _ _ => none

/-- The primitive, reducible so its facet equations reduce in proofs. -/
@[reducible] def qPrim : PrimLTS2 QSig :=
  { State := QState
    init := qInit
    admit := qAdmit
    run := qRun
    park := qPark
    finish := qFinish
    extCap := qExtCap
    extRun := qExtRun
    onTick := fun s _ => s
    expire := qExpire }

/-! ## Shared arithmetic of the safety proofs -/

/-- Committing one item keeps the ring within capacity. -/
theorem qLen_commit {l : List QItem} {w : QItem} (h : l.length < qCap) :
    (l ++ [w]).length ≤ qCap := by
  have h2 : (l ++ [w]).length = l.length + 1 := by simp
  omega

/-- Dropping the head keeps the ring within capacity. -/
theorem qLen_tail {v : QItem} {r : List QItem} (h : (v :: r).length ≤ qCap) :
    r.length ≤ qCap := by
  have h2 : (v :: r).length = r.length + 1 := by simp
  omega

/-- The ledger under a bare commit. -/
theorem qLedger_push {cl dl ring : List QItem} {v : QItem}
    (h : cl = dl ++ ring) : cl ++ [v] = dl ++ (ring ++ [v]) := by
  simp [h, List.append_assoc]

/-- The ledger under a delivery. -/
theorem qLedger_pop {cl dl : List QItem} {v : QItem} {r2 : List QItem}
    (h : cl = dl ++ v :: r2) : cl = (dl ++ [v]) ++ r2 := by
  simp [h, List.append_assoc]

/-- The ledger under a delivery followed by a producer-grant commit. -/
theorem qLedger_pop_grant {cl dl : List QItem} {v w : QItem} {r2 : List QItem}
    (h : cl = dl ++ v :: r2) : cl ++ [w] = (dl ++ [v]) ++ (r2 ++ [w]) := by
  simp [h, List.append_assoc]

/-! ## The result-semantics authority

The queue contract's own outcome function, written independently of the
primitive's sections: no grant lists, no resolved records, no wake
lists.  Wherever a public result exists, the primitive's result must be
exactly this one (the Semaphore lesson: a wrong result can preserve
every state invariant, so the agreement is its own check). -/

/-- The outcome authority for the inline sections. -/
def qSpecRun : QState → QCall → Option (QRes × QState)
  | s, QCall.qpush v =>
      if s.closed then some (QRes.qClosed, s)
      else if s.ring.length < qCap then
        some (QRes.qCommitted, { s with ring := s.ring ++ [v] })
      else none
  | s, QCall.qpop =>
      match s.ring with
      | [] => if s.closed then some (QRes.qClosed, s) else none
      | v :: r => some (QRes.qItem v, { s with ring := r })
  | s, QCall.qtrypush v =>
      if s.closed then some (QRes.qClosed, s)
      else if s.ring.length < qCap then
        some (QRes.qCommitted, { s with ring := s.ring ++ [v] })
      else some (QRes.qWouldBlock, s)
  | s, QCall.qtrypop =>
      match s.ring with
      | [] => if s.closed then some (QRes.qClosed, s) else some (QRes.qWouldBlock, s)
      | v :: r => some (QRes.qItem v, { s with ring := r })
  | s, QCall.qclose => some (QRes.qDone, { s with closed := true })

/-- The outcome authority for a granted waiter's published outcome. -/
def qSpecFin : QOut → QRes
  | QOut.oCommitted => QRes.qCommitted
  | QOut.oClosed => QRes.qClosed
  | QOut.oItem v => QRes.qItem v

/-- Every inline result is the authority's result. -/
theorem qRun_result_agrees : ∀ (s : QState) (t : Tick) (f : FiberId) (c : QCall)
    (r : QRes) (s' : QState) (ws : List FiberId),
    qRun s t f c = some (r, s', ws) → (qSpecRun s c).map Prod.fst = some r := by
  intro s t f c r s' ws h
  rcases s with ⟨ring, closed, wp, wc, res, cl, dl⟩
  cases c with
  | qpush v =>
      simp only [qRun] at h
      by_cases hcl : closed = true
      · rw [if_pos hcl] at h
        simp at h
        obtain ⟨h1, hst, -⟩ := h
        subst hst
        subst h1
        simp [qSpecRun, hcl]
      · rw [if_neg hcl] at h
        by_cases hg : ring.length < qCap ∧ wp = []
        · rw [if_pos hg] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst
          subst h1
          simp [qSpecRun, hcl, hg.1]
        · rw [if_neg hg] at h
          simp at h
  | qpop =>
      simp only [qRun] at h
      cases ring with
      | nil =>
          simp only at h
          by_cases hcl : closed = true
          · rw [if_pos hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun, hcl]
          · rw [if_neg hcl] at h
            simp at h
      | cons v r2 =>
          simp only at h
          by_cases hq : wc = []
          · rw [if_pos hq] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun]
          · rw [if_neg hq] at h
            simp at h
  | qtrypush v =>
      simp only [qRun] at h
      by_cases hcl : closed = true
      · rw [if_pos hcl] at h
        simp at h
        obtain ⟨h1, hst, -⟩ := h
        subst hst
        subst h1
        simp [qSpecRun, hcl]
      · rw [if_neg hcl] at h
        by_cases hs : ring.length < qCap
        · rw [if_pos hs] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst
          subst h1
          simp [qSpecRun, hcl, hs]
        · rw [if_neg hs] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst
          subst h1
          simp [qSpecRun, hcl, hs]
  | qtrypop =>
      simp only [qRun] at h
      cases ring with
      | nil =>
          simp only at h
          by_cases hcl : closed = true
          · rw [if_pos hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun, hcl]
          · rw [if_neg hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun, hcl]
      | cons v r2 =>
          simp only at h
          by_cases hq : wc = []
          · rw [if_pos hq] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun]
          · rw [if_neg hq] at h
            simp at h
  | qclose =>
      simp only [qRun] at h
      simp at h
      obtain ⟨h1, hst, -⟩ := h
      subst hst
      subst h1
      simp [qSpecRun]

/-- Every external result is the authority's result (the sections are
the same code bodies). -/
theorem qExtRun_result_agrees : ∀ (c : QCall) (s : QState) (t : Tick)
    (r : QRes) (s' : QState) (ws : List FiberId),
    qExtRun c s t = some (r, s', ws) → (qSpecRun s c).map Prod.fst = some r := by
  intro c s t r s' ws h
  rcases s with ⟨ring, closed, wp, wc, res, cl, dl⟩
  cases c with
  | qpush v => simp only [qExtRun] at h; simp at h
  | qpop => simp only [qExtRun] at h; simp at h
  | qtrypush v =>
      simp only [qExtRun] at h
      by_cases hcl : closed = true
      · rw [if_pos hcl] at h
        simp at h
        obtain ⟨h1, hst, -⟩ := h
        subst hst
        subst h1
        simp [qSpecRun, hcl]
      · rw [if_neg hcl] at h
        by_cases hs : ring.length < qCap
        · rw [if_pos hs] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst
          subst h1
          simp [qSpecRun, hcl, hs]
        · rw [if_neg hs] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst
          subst h1
          simp [qSpecRun, hcl, hs]
  | qtrypop =>
      simp only [qExtRun] at h
      cases ring with
      | nil =>
          simp only at h
          by_cases hcl : closed = true
          · rw [if_pos hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun, hcl]
          · rw [if_neg hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun, hcl]
      | cons v r2 =>
          simp only at h
          by_cases hq : wc = []
          · rw [if_pos hq] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst
            subst h1
            simp [qSpecRun]
          · rw [if_neg hq] at h
            simp at h
  | qclose =>
      simp only [qExtRun] at h
      simp at h
      obtain ⟨h1, hst, -⟩ := h
      subst hst
      subst h1
      simp [qSpecRun]

/-- A resumed waiter's completion is the authority's image of the
outcome the granter published. -/
theorem qFinish_result_agrees : ∀ (s : QState) (f : FiberId) (c : QCall)
    (r : QRes) (s' : QState) (ws : List FiberId),
    qFinish s f c = some (r, s', ws) →
      ∃ o rest, qConsume s.resolved f = some (o, rest) ∧ qSpecFin o = r := by
  intro s f c r s' ws h
  cases c with
  | qpush v =>
      simp only [qFinish] at h
      cases hc : qConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          cases o with
          | oCommitted =>
              simp at h
              obtain ⟨h1, h2, -⟩ := h
              subst h1
              exact ⟨QOut.oCommitted, rest, rfl, rfl⟩
          | oClosed =>
              simp at h
              obtain ⟨h1, h2, -⟩ := h
              subst h1
              exact ⟨QOut.oClosed, rest, rfl, rfl⟩
          | oItem w => simp at h
  | qpop =>
      simp only [qFinish] at h
      cases hc : qConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          cases o with
          | oCommitted => simp at h
          | oClosed =>
              simp at h
              obtain ⟨h1, h2, -⟩ := h
              subst h1
              exact ⟨QOut.oClosed, rest, rfl, rfl⟩
          | oItem w =>
              simp at h
              obtain ⟨h1, h2, -⟩ := h
              subst h1
              exact ⟨QOut.oItem w, rest, rfl, rfl⟩
  | qtrypush v => simp [qFinish] at h
  | qtrypop => simp [qFinish] at h
  | qclose => simp [qFinish] at h

/-! ## Safety: capacity, conservation, external FIFO, drained consumers

The state discipline the sections maintain, as one conjunction:

  * capacity -- the ring never holds more than `qCap` items (every
    commit is guarded by `ring_full_locked`'s negation);
  * the commit/delivery ledger -- `clog = dlog ++ ring`: every
    committed item is exactly the delivered prefix followed by the
    buffered suffix, which is both item conservation and the external
    FIFO (the delivery log is always a prefix of the commit log);
  * drained consumers -- a suspended consumer never coexists with a
    non-empty ring: every commit's cross-grant serves the head
    consumer, so a consumer can only be queued on an empty ring (this
    is what makes the close drain's item branch exact). -/
@[reducible] def qSafe (s : QState) : Prop :=
  s.ring.length ≤ qCap ∧ s.clog = s.dlog ++ s.ring ∧ (s.waitqC ≠ [] → s.ring = [])

theorem qAdmit_unchanged {s : QState} {f : FiberId} {c : QCall} {s' : QState}
    (h : qAdmit s f c = some s') : s' = s := by
  simp only [qAdmit, Option.some.injEq] at h
  exact h.symm

/-- The consumer cross-grant preserves the discipline. -/
theorem qGrantConsumer_preserves {s : QState} {s2 : QState} {ws2 : List FiberId}
    (h : qGrantConsumer s = (s2, ws2)) (hx : qSafe s) : qSafe s2 := by
  obtain ⟨hcap, hlog, hdr⟩ := hx
  cases hc : s.waitqC with
  | nil =>
      simp only [hc, qGrantConsumer] at h
      simp at h
      obtain ⟨hst, -⟩ := h
      subst hst
      exact ⟨hcap, hlog, hdr⟩
  | cons g t =>
      have hne : s.waitqC ≠ [] := by rw [hc]; exact List.cons_ne_nil g t
      have hr0 : s.ring = [] := hdr hne
      cases hr : s.ring with
      | nil =>
          simp only [hr, hc, qGrantConsumer] at h
          simp at h
          obtain ⟨hst, -⟩ := h
          subst hst
          rw [hr] at hcap hlog
          exact ⟨hcap, hlog, by intro _; rfl⟩
      | cons v r =>
          rw [hr] at hr0
          exact absurd hr0 (List.cons_ne_nil v r)

/-- The producer cross-grant preserves the discipline. -/
theorem qGrantProducer_preserves {s : QState} {s2 : QState} {ws2 : List FiberId}
    (h : qGrantProducer s = (s2, ws2)) (hq : s.waitqC = []) (hx : qSafe s) :
    qSafe s2 := by
  obtain ⟨hcap, hlog, hdr⟩ := hx
  cases hp0 : s.waitqP with
  | nil =>
      simp only [hp0, qGrantProducer] at h
      simp at h
      obtain ⟨hst, -⟩ := h
      subst hst
      exact ⟨hcap, hlog, hdr⟩
  | cons pr t =>
      obtain ⟨f, v⟩ := pr
      simp only [hp0, qGrantProducer] at h
      by_cases hg : s.closed = false ∧ s.ring.length < qCap
      · rw [if_pos hg] at h
        simp at h
        obtain ⟨hst, -⟩ := h
        subst hst
        refine ⟨qLen_commit hg.2, qLedger_push hlog, ?_⟩
        rw [hq]
        intro hcc
        exact absurd rfl hcc
      · rw [if_neg hg] at h
        simp at h
        obtain ⟨hst, -⟩ := h
        subst hst
        exact ⟨hcap, hlog, hdr⟩

/-- The inline sections preserve the discipline. -/
theorem qRun_preserves {s : QState} {t : Tick} {f : FiberId} {c : QCall}
    {r : QRes} {s' : QState} {ws : List FiberId}
    (h : qRun s t f c = some (r, s', ws)) (hx : qSafe s) : qSafe s' := by
  obtain ⟨hcap, hlog, hdr⟩ := hx
  cases c with
  | qpush v =>
      simp only [qRun] at h
      by_cases hcl : s.closed = true
      · rw [if_pos hcl] at h
        simp at h
        obtain ⟨h1, hst, -⟩ := h
        subst hst; subst h1
        exact ⟨hcap, hlog, hdr⟩
      · rw [if_neg hcl] at h
        by_cases hg : s.ring.length < qCap ∧ s.waitqP = []
        · rw [if_pos hg] at h
          simp only [qGrantConsumer] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          cases hcq : s.waitqC with
          | nil =>
              refine ⟨qLen_commit hg.1, qLedger_push hlog, by simp⟩
          | cons g t2 =>
              have hne : s.waitqC ≠ [] := by rw [hcq]; exact List.cons_ne_nil g t2
              have hr0 : s.ring = [] := hdr hne
              rw [hr0]
              simp only []
              refine ⟨by simp, ?_, by intro hcc; rfl⟩
              simp [hlog, hr0]
        · rw [if_neg hg] at h
          simp at h
  | qpop =>
      simp only [qRun] at h
      cases hr : s.ring with
      | nil =>
          simp only [hr] at h
          by_cases hcl : s.closed = true
          · rw [if_pos hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst; subst h1
            exact ⟨hcap, hlog, hdr⟩
          · rw [if_neg hcl] at h
            simp at h
      | cons v r2 =>
          simp only [hr] at h
          rw [hr] at hcap hlog
          by_cases hq : s.waitqC = []
          · rw [if_pos hq] at h
            simp only [qGrantProducer] at h
            simp at h
            cases hp0 : s.waitqP with
            | nil =>
                simp only [hp0] at h
                simp at h
                obtain ⟨h1, hst, -⟩ := h
                subst hst; subst h1
                refine ⟨qLen_tail hcap, qLedger_pop hlog, ?_⟩
                rw [hq]; intro hcc; exact absurd rfl hcc
            | cons pr t2 =>
                obtain ⟨f2, w2⟩ := pr
                simp only [hp0] at h
                by_cases hg : s.closed = false ∧ r2.length < qCap
                · rw [if_pos hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  refine ⟨qLen_commit hg.2, qLedger_pop_grant hlog, ?_⟩
                  rw [hq]; intro hcc; exact absurd rfl hcc
                · rw [if_neg hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  refine ⟨qLen_tail hcap, qLedger_pop hlog, ?_⟩
                  rw [hq]; intro hcc; exact absurd rfl hcc
          · rw [if_neg hq] at h
            simp at h
  | qtrypush v =>
      simp only [qRun] at h
      by_cases hcl : s.closed = true
      · rw [if_pos hcl] at h
        simp at h
        obtain ⟨h1, hst, -⟩ := h
        subst hst; subst h1
        exact ⟨hcap, hlog, hdr⟩
      · rw [if_neg hcl] at h
        by_cases hs : s.ring.length < qCap
        · rw [if_pos hs] at h
          simp only [qGrantConsumer] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          cases hcq : s.waitqC with
          | nil =>
              refine ⟨qLen_commit hs, qLedger_push hlog, by simp⟩
          | cons g t2 =>
              have hne : s.waitqC ≠ [] := by rw [hcq]; exact List.cons_ne_nil g t2
              have hr0 : s.ring = [] := hdr hne
              rw [hr0]
              simp only []
              refine ⟨by simp, ?_, by intro hcc; rfl⟩
              simp [hlog, hr0]
        · rw [if_neg hs] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          exact ⟨hcap, hlog, hdr⟩
  | qtrypop =>
      simp only [qRun] at h
      cases hr : s.ring with
      | nil =>
          simp only [hr] at h
          by_cases hcl : s.closed = true
          · rw [if_pos hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst; subst h1
            exact ⟨hcap, hlog, hdr⟩
          · rw [if_neg hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst; subst h1
            exact ⟨hcap, hlog, hdr⟩
      | cons v r2 =>
          simp only [hr] at h
          rw [hr] at hcap hlog
          by_cases hq : s.waitqC = []
          · rw [if_pos hq] at h
            simp only [qGrantProducer] at h
            simp at h
            cases hp0 : s.waitqP with
            | nil =>
                simp only [hp0] at h
                simp at h
                obtain ⟨h1, hst, -⟩ := h
                subst hst; subst h1
                refine ⟨qLen_tail hcap, qLedger_pop hlog, ?_⟩
                rw [hq]; intro hcc; exact absurd rfl hcc
            | cons pr t2 =>
                obtain ⟨f2, w2⟩ := pr
                simp only [hp0] at h
                by_cases hg : s.closed = false ∧ r2.length < qCap
                · rw [if_pos hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  refine ⟨qLen_commit hg.2, qLedger_pop_grant hlog, ?_⟩
                  rw [hq]; intro hcc; exact absurd rfl hcc
                · rw [if_neg hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  refine ⟨qLen_tail hcap, qLedger_pop hlog, ?_⟩
                  rw [hq]; intro hcc; exact absurd rfl hcc
          · rw [if_neg hq] at h
            simp at h
  | qclose =>
      simp only [qRun] at h
      simp at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      obtain ⟨n, hn, hle, hd⟩ :=
        qDrainC_drop s.waitqC s.ring [] (qDrainC s.ring s.waitqC []).2.2
          (qDrainC s.ring s.waitqC []).1 (qDrainC s.ring s.waitqC []).2.1 rfl
      refine ⟨by rw [hn]; simp [List.length_drop]; omega, ?_, ?_⟩
      · rw [hlog, hn]
        have htake : s.ring.length - (s.ring.drop n).length = n := by
          rw [List.length_drop]; omega
        rw [htake]
        show s.dlog ++ s.ring = (s.dlog ++ s.ring.take n) ++ List.drop n s.ring
        rw [List.append_assoc, List.take_append_drop]
      · intro hcc
        rw [hn]
        exact hd hcc

/-- Parking a `push` appends to the producer queue; parking a `pop`
is only reachable on an empty open ring or behind another queued
consumer (the `run = none` premise), which keeps the drained-consumer
conjunct. -/
theorem qPark_preserves {s : QState} {t : Tick} {f : FiberId} {c : QCall}
    {wk : List FiberId} (hr : qRun s t f c = none)
    (h : qPark s f c = some (s', wk)) (hx : qSafe s) :
    qSafe s' := by
  obtain ⟨hcap, hlog, hdr⟩ := hx
  cases c with
  | qpush v =>
      simp only [qPark] at h
      by_cases hq : (s.waitqP.any fun p => p.1 = f) ∨ (s.waitqC.any fun g => g = f)
      · rw [if_pos hq] at h; simp at h
      · rw [if_neg hq] at h
        simp at h
        obtain ⟨hst, -⟩ := h
        subst hst
        exact ⟨hcap, hlog, hdr⟩
  | qpop =>
      simp only [qRun] at hr
      cases hr2 : s.ring with
      | nil =>
          simp only [hr2] at hr
          simp only [qPark] at h
          by_cases hq : (s.waitqP.any fun p => p.1 = f) ∨ (s.waitqC.any fun g => g = f)
          · rw [if_pos hq] at h; simp at h
          · rw [if_neg hq] at h
            simp at h
            obtain ⟨hst, -⟩ := h
            subst hst
            refine ⟨hcap, hlog, ?_⟩
            intro _
            exact hr2
      | cons v r2 =>
          simp only [hr2] at hr
          have hwc : s.waitqC ≠ [] := by
            intro hc0
            rw [hc0] at hr
            simp at hr
          exact absurd (hdr hwc) (by rw [hr2]; exact List.cons_ne_nil v r2)
  | qtrypush v => simp [qPark] at h
  | qtrypop => simp [qPark] at h
  | qclose => simp [qPark] at h

/-- A resumed waiter's finish replaces the resolved list only. -/
theorem qFinish_preserves {s : QState} {f : FiberId} {c : QCall}
    {r : QRes} {s' : QState} {ws : List FiberId}
    (h : qFinish s f c = some (r, s', ws)) (hx : qSafe s) : qSafe s' := by
  obtain ⟨hcap, hlog, hdr⟩ := hx
  cases c with
  | qpush v =>
      simp only [qFinish] at h
      cases hc : qConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          cases o with
          | oCommitted =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨hcap, hlog, hdr⟩
          | oClosed =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨hcap, hlog, hdr⟩
          | oItem w => simp at h
  | qpop =>
      simp only [qFinish] at h
      cases hc : qConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          cases o with
          | oCommitted => simp at h
          | oClosed =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨hcap, hlog, hdr⟩
          | oItem w =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨hcap, hlog, hdr⟩
  | qtrypush v => simp [qFinish] at h
  | qtrypop => simp [qFinish] at h
  | qclose => simp [qFinish] at h

/-- The external sections preserve the discipline (same bodies). -/
theorem qExtRun_preserves {s : QState} {c : QCall} {t : Tick}
    {r : QRes} {s' : QState} {ws : List FiberId}
    (h : qExtRun c s t = some (r, s', ws)) (hx : qSafe s) : qSafe s' := by
  obtain ⟨hcap, hlog, hdr⟩ := hx
  cases c with
  | qpush v => simp only [qExtRun] at h; simp at h
  | qpop => simp only [qExtRun] at h; simp at h
  | qtrypush v =>
      simp only [qExtRun] at h
      by_cases hcl : s.closed = true
      · rw [if_pos hcl] at h
        simp at h
        obtain ⟨h1, hst, -⟩ := h
        subst hst; subst h1
        exact ⟨hcap, hlog, hdr⟩
      · rw [if_neg hcl] at h
        by_cases hs : s.ring.length < qCap
        · rw [if_pos hs] at h
          simp only [qGrantConsumer] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          cases hcq : s.waitqC with
          | nil =>
              refine ⟨qLen_commit hs, qLedger_push hlog, by simp⟩
          | cons g t2 =>
              have hne : s.waitqC ≠ [] := by rw [hcq]; exact List.cons_ne_nil g t2
              have hr0 : s.ring = [] := hdr hne
              rw [hr0]
              simp only []
              refine ⟨by simp, ?_, by intro hcc; rfl⟩
              simp [hlog, hr0]
        · rw [if_neg hs] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          exact ⟨hcap, hlog, hdr⟩
  | qtrypop =>
      simp only [qExtRun] at h
      cases hr : s.ring with
      | nil =>
          simp only [hr] at h
          by_cases hcl : s.closed = true
          · rw [if_pos hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst; subst h1
            exact ⟨hcap, hlog, hdr⟩
          · rw [if_neg hcl] at h
            simp at h
            obtain ⟨h1, hst, -⟩ := h
            subst hst; subst h1
            exact ⟨hcap, hlog, hdr⟩
      | cons v r2 =>
          simp only [hr] at h
          rw [hr] at hcap hlog
          by_cases hq : s.waitqC = []
          · rw [if_pos hq] at h
            simp only [qGrantProducer] at h
            simp at h
            cases hp0 : s.waitqP with
            | nil =>
                simp only [hp0] at h
                simp at h
                obtain ⟨h1, hst, -⟩ := h
                subst hst; subst h1
                refine ⟨qLen_tail hcap, qLedger_pop hlog, ?_⟩
                rw [hq]; intro hcc; exact absurd rfl hcc
            | cons pr t2 =>
                obtain ⟨f2, w2⟩ := pr
                simp only [hp0] at h
                by_cases hg : s.closed = false ∧ r2.length < qCap
                · rw [if_pos hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  refine ⟨qLen_commit hg.2, qLedger_pop_grant hlog, ?_⟩
                  rw [hq]; intro hcc; exact absurd rfl hcc
                · rw [if_neg hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  refine ⟨qLen_tail hcap, qLedger_pop hlog, ?_⟩
                  rw [hq]; intro hcc; exact absurd rfl hcc
          · rw [if_neg hq] at h
            simp at h
  | qclose =>
      simp only [qExtRun] at h
      simp at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      obtain ⟨n, hn, hle, hd⟩ :=
        qDrainC_drop s.waitqC s.ring [] (qDrainC s.ring s.waitqC []).2.2
          (qDrainC s.ring s.waitqC []).1 (qDrainC s.ring s.waitqC []).2.1 rfl
      refine ⟨by rw [hn]; simp [List.length_drop]; omega, ?_, ?_⟩
      · rw [hlog, hn]
        have htake : s.ring.length - (s.ring.drop n).length = n := by
          rw [List.length_drop]; omega
        rw [htake]
        show s.dlog ++ s.ring = (s.dlog ++ s.ring.take n) ++ List.drop n s.ring
        rw [List.append_assoc, List.take_append_drop]
      · intro hcc
        rw [hn]
        exact hd hcc

/-- The primitive never breaks the discipline: at every reachable
configuration the ring is within capacity, the commit/delivery ledger
balances, and no suspended consumer coexists with a non-empty ring. -/
theorem q_safe {cfg m : PrimCfg QSig qPrim} {t : Trace QSig}
    (hrun : PrimRuns2 QSig qPrim cfg t m) :
    qSafe cfg.prim → qSafe m.prim := by
  induction hrun with
  | stop cfg => exact fun h => h
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro h
      refine ih ?_
      cases hstep with
      | submit f c hsub => exact h
      | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
          have hss : s' = cfg.prim :=
            qAdmit_unchanged (s := cfg.prim) (f := rp.fiber) (c := rp.call) hadmit
          rw [hss]
          exact h
      | dispatchResumed rp rest hcurE hrunqE hfreshE => exact h
      | fiberEffect d b ps rest r s' wk hcurE hb hrunE hmap hwake =>
          exact qRun_preserves hrunE h
      | fiberDone d r hcurE => exact h
      | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
          exact qPark_preserves hrunE hparkE h
      | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
          exact qFinish_preserves hfinE h
      | extApply x c preE postE hcap hnovel => exact h
      | extEffect preE postE e ps rest r s' wk hextsE hresE hrunE hmap hwake =>
          exact qExtRun_preserves hrunE h
      | extDone preE postE e r hextsE hresE => exact h
      | envTime t0 hcurE hle => exact h
      | envExpire f s' preP postP p hcurE hexp hparE hf =>
          exact absurd hexp (by simp [qPrim, qExpire])

/-- `primInit`'s state satisfies the discipline, so every run endpoint
does. -/
theorem q_safe_holds {fin : PrimCfg QSig qPrim} {t : Trace QSig}
    (hrun : PrimRuns2 QSig qPrim (primInit QSig qPrim) t fin) :
    qSafe fin.prim := by
  refine q_safe hrun ?_
  rw [qSafe]
  simp [qInit, primInit, qCap]

/-- Capacity holds at every reachable endpoint. -/
theorem q_capacity_holds {fin : PrimCfg QSig qPrim} {t : Trace QSig}
    (hrun : PrimRuns2 QSig qPrim (primInit QSig qPrim) t fin) :
    fin.prim.ring.length ≤ qCap := (q_safe_holds hrun).1

/-- External FIFO: the delivery log is always a prefix of the commit
log -- completed successful deliveries respect committed item order. -/
theorem q_fifo_holds {fin : PrimCfg QSig qPrim} {t : Trace QSig}
    (hrun : PrimRuns2 QSig qPrim (primInit QSig qPrim) t fin) :
    fin.prim.dlog <+: fin.prim.clog := by
  obtain ⟨-, hlog, -⟩ := q_safe_holds hrun
  exact ⟨fin.prim.ring, hlog.symm⟩

/-- Drained consumers: a suspended consumer only ever coexists with an
empty ring. -/
theorem q_drained_holds {fin : PrimCfg QSig qPrim} {t : Trace QSig}
    (hrun : PrimRuns2 QSig qPrim (primInit QSig qPrim) t fin) :
    fin.prim.waitqC ≠ [] → fin.prim.ring = [] := (q_safe_holds hrun).2.2

/-! ## The close discipline

From a closed state no section ever commits again: the commit log is
frozen and the ring never grows (items may still drain to consumers or
read `closed`). -/

theorem qRun_closed_inert {s : QState} {t : Tick} {f : FiberId} {c : QCall}
    {r : QRes} {s' : QState} {ws : List FiberId}
    (hc : s.closed = true) (h : qRun s t f c = some (r, s', ws)) :
    s'.clog = s.clog ∧ s'.ring.length ≤ s.ring.length ∧ s'.closed = true := by
  cases c with
  | qpush v =>
      simp only [qRun] at h
      rw [if_pos hc] at h
      simp at h
      obtain ⟨h1, hst, -⟩ := h
      subst hst; subst h1
      exact ⟨rfl, Nat.le_refl _, hc⟩
  | qpop =>
      simp only [qRun] at h
      cases hr : s.ring with
      | nil =>
          simp only [hr] at h
          rw [if_pos hc] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          exact ⟨rfl, by rw [hr]; exact Nat.le_refl _, hc⟩
      | cons v r2 =>
          simp only [hr] at h
          by_cases hq : s.waitqC = []
          · rw [if_pos hq] at h
            simp only [qGrantProducer] at h
            simp at h
            cases hp0 : s.waitqP with
            | nil =>
                simp only [hp0] at h
                simp at h
                obtain ⟨h1, hst, -⟩ := h
                subst hst; subst h1
                exact ⟨rfl, by simp, hc⟩
            | cons pr t2 =>
                obtain ⟨f2, w2⟩ := pr
                simp only [hp0] at h
                have hng : ¬(s.closed = false ∧ r2.length < qCap) := by
                  simp [hc]
                by_cases hg : s.closed = false ∧ r2.length < qCap
                · rw [if_pos hg] at h
                  exact absurd hg hng
                · rw [if_neg hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  exact ⟨rfl, by simp, hc⟩
          · rw [if_neg hq] at h
            simp at h
  | qtrypush v =>
      simp only [qRun] at h
      rw [if_pos hc] at h
      simp at h
      obtain ⟨h1, hst, -⟩ := h
      subst hst; subst h1
      exact ⟨rfl, Nat.le_refl _, hc⟩
  | qtrypop =>
      simp only [qRun] at h
      cases hr : s.ring with
      | nil =>
          simp only [hr] at h
          rw [if_pos hc] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          exact ⟨rfl, by rw [hr]; exact Nat.le_refl _, hc⟩
      | cons v r2 =>
          simp only [hr] at h
          by_cases hq : s.waitqC = []
          · rw [if_pos hq] at h
            simp only [qGrantProducer] at h
            simp at h
            cases hp0 : s.waitqP with
            | nil =>
                simp only [hp0] at h
                simp at h
                obtain ⟨h1, hst, -⟩ := h
                subst hst; subst h1
                exact ⟨rfl, by simp, hc⟩
            | cons pr t2 =>
                obtain ⟨f2, w2⟩ := pr
                simp only [hp0] at h
                have hng : ¬(s.closed = false ∧ r2.length < qCap) := by
                  simp [hc]
                by_cases hg : s.closed = false ∧ r2.length < qCap
                · rw [if_pos hg] at h
                  exact absurd hg hng
                · rw [if_neg hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  exact ⟨rfl, by simp, hc⟩
          · rw [if_neg hq] at h
            simp at h
  | qclose =>
      simp only [qRun] at h
      simp at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      obtain ⟨n, hn, hle, -⟩ :=
        qDrainC_drop s.waitqC s.ring [] (qDrainC s.ring s.waitqC []).2.2
          (qDrainC s.ring s.waitqC []).1 (qDrainC s.ring s.waitqC []).2.1 rfl
      exact ⟨rfl, by rw [hn]; simp [List.length_drop], rfl⟩

theorem qExtRun_closed_inert {c : QCall} {s : QState} {t : Tick}
    {r : QRes} {s' : QState} {ws : List FiberId}
    (hc : s.closed = true) (h : qExtRun c s t = some (r, s', ws)) :
    s'.clog = s.clog ∧ s'.ring.length ≤ s.ring.length ∧ s'.closed = true := by
  cases c with
  | qpush v => simp only [qExtRun] at h; simp at h
  | qpop => simp only [qExtRun] at h; simp at h
  | qtrypush v =>
      simp only [qExtRun] at h
      rw [if_pos hc] at h
      simp at h
      obtain ⟨h1, hst, -⟩ := h
      subst hst; subst h1
      exact ⟨rfl, Nat.le_refl _, hc⟩
  | qtrypop =>
      simp only [qExtRun] at h
      cases hr : s.ring with
      | nil =>
          simp only [hr] at h
          rw [if_pos hc] at h
          simp at h
          obtain ⟨h1, hst, -⟩ := h
          subst hst; subst h1
          exact ⟨rfl, by rw [hr]; exact Nat.le_refl _, hc⟩
      | cons v r2 =>
          simp only [hr] at h
          by_cases hq : s.waitqC = []
          · rw [if_pos hq] at h
            simp only [qGrantProducer] at h
            simp at h
            cases hp0 : s.waitqP with
            | nil =>
                simp only [hp0] at h
                simp at h
                obtain ⟨h1, hst, -⟩ := h
                subst hst; subst h1
                exact ⟨rfl, by simp, hc⟩
            | cons pr t2 =>
                obtain ⟨f2, w2⟩ := pr
                simp only [hp0] at h
                have hng : ¬(s.closed = false ∧ r2.length < qCap) := by
                  simp [hc]
                by_cases hg : s.closed = false ∧ r2.length < qCap
                · rw [if_pos hg] at h
                  exact absurd hg hng
                · rw [if_neg hg] at h
                  simp at h
                  obtain ⟨h1, hst, -⟩ := h
                  subst hst; subst h1
                  exact ⟨rfl, by simp, hc⟩
          · rw [if_neg hq] at h
            simp at h
  | qclose =>
      simp only [qExtRun] at h
      simp at h
      obtain ⟨-, hst, -⟩ := h
      subst hst
      obtain ⟨n, hn, hle, -⟩ :=
        qDrainC_drop s.waitqC s.ring [] (qDrainC s.ring s.waitqC []).2.2
          (qDrainC s.ring s.waitqC []).1 (qDrainC s.ring s.waitqC []).2.1 rfl
      exact ⟨rfl, by rw [hn]; simp [List.length_drop], rfl⟩

/-- Parking and finishing never touch the commit log, the ring, or the
closed bit. -/
theorem qPark_inert {s : QState} {f : FiberId} {c : QCall}
    {wk : List FiberId} (h : qPark s f c = some (s', wk)) :
    s'.clog = s.clog ∧ s'.ring = s.ring ∧ s'.closed = s.closed := by
  cases c with
  | qpush v =>
      simp only [qPark] at h
      by_cases hq : (s.waitqP.any fun p => p.1 = f) ∨ (s.waitqC.any fun g => g = f)
      · rw [if_pos hq] at h; simp at h
      · rw [if_neg hq] at h
        simp at h
        obtain ⟨hst, -⟩ := h
        subst hst
        exact ⟨rfl, rfl, rfl⟩
  | qpop =>
      simp only [qPark] at h
      by_cases hq : (s.waitqP.any fun p => p.1 = f) ∨ (s.waitqC.any fun g => g = f)
      · rw [if_pos hq] at h; simp at h
      · rw [if_neg hq] at h
        simp at h
        obtain ⟨hst, -⟩ := h
        subst hst
        exact ⟨rfl, rfl, rfl⟩
  | qtrypush v => simp [qPark] at h
  | qtrypop => simp [qPark] at h
  | qclose => simp [qPark] at h

theorem qFinish_inert {s : QState} {f : FiberId} {c : QCall}
    {r : QRes} {s' : QState} {ws : List FiberId}
    (h : qFinish s f c = some (r, s', ws)) :
    s'.clog = s.clog ∧ s'.ring = s.ring ∧ s'.closed = s.closed := by
  cases c with
  | qpush v =>
      simp only [qFinish] at h
      cases hc : qConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          cases o with
          | oCommitted =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨rfl, rfl, rfl⟩
          | oClosed =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨rfl, rfl, rfl⟩
          | oItem w => simp at h
  | qpop =>
      simp only [qFinish] at h
      cases hc : qConsume s.resolved f with
      | none => simp only [hc] at h; simp at h
      | some p =>
          obtain ⟨o, rest⟩ := p
          simp only [hc] at h
          cases o with
          | oCommitted => simp at h
          | oClosed =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨rfl, rfl, rfl⟩
          | oItem w =>
              simp at h
              obtain ⟨-, hst, -⟩ := h
              subst hst
              exact ⟨rfl, rfl, rfl⟩
  | qtrypush v => simp [qFinish] at h
  | qtrypop => simp [qFinish] at h
  | qclose => simp [qFinish] at h

/-- From a closed configuration the commit log stays frozen and the
ring never grows, whatever the run does. -/
theorem q_closed_no_commit {cfg m : PrimCfg QSig qPrim} {t : Trace QSig}
    (hrun : PrimRuns2 QSig qPrim cfg t m) :
    cfg.prim.closed = true →
      m.prim.clog = cfg.prim.clog ∧ m.prim.ring.length ≤ cfg.prim.ring.length := by
  induction hrun with
  | stop cfg => intro _; exact ⟨rfl, Nat.le_refl _⟩
  | step cfg cfg' o t2 fin hstep hrest ih =>
      intro hc
      have hstep' : cfg'.prim.clog = cfg.prim.clog ∧
          cfg'.prim.ring.length ≤ cfg.prim.ring.length ∧
          cfg'.prim.closed = true := by
        cases hstep with
        | submit f c hsub => exact ⟨rfl, Nat.le_refl _, hc⟩
        | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
            have hss : s' = cfg.prim :=
              qAdmit_unchanged (s := cfg.prim) (f := rp.fiber) (c := rp.call) hadmit
            rw [hss]
            exact ⟨rfl, Nat.le_refl _, hc⟩
        | dispatchResumed rp rest hcurE hrunqE hfreshE => exact ⟨rfl, Nat.le_refl _, hc⟩
        | fiberEffect d b ps rest r s' wk hcurE hb hrunE hmap hwake =>
            exact qRun_closed_inert hc hrunE
        | fiberDone d r hcurE => exact ⟨rfl, Nat.le_refl _, hc⟩
        | runPark d b ps rest s' wk hcurE hrunE hparkE hmap hwake =>
            obtain ⟨h1, h2, h3⟩ := qPark_inert hparkE
            exact ⟨h1, by rw [h2]; exact Nat.le_refl _, h3.trans hc⟩
        | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
            obtain ⟨h1, h2, h3⟩ := qFinish_inert hfinE
            exact ⟨h1, by rw [h2]; exact Nat.le_refl _, h3.trans hc⟩
        | extApply x c preE postE hcap hnovel => exact ⟨rfl, Nat.le_refl _, hc⟩
        | extEffect preE postE e ps rest r s' wk hextsE hresE hrunE hmap hwake =>
            exact qExtRun_closed_inert hc hrunE
        | extDone preE postE e r hextsE hresE => exact ⟨rfl, Nat.le_refl _, hc⟩
        | envTime t0 hcurE hle => exact ⟨rfl, Nat.le_refl _, hc⟩
        | envExpire f s' preP postP p hcurE hexp hparE hf =>
            exact absurd hexp (by simp [qPrim, qExpire])
      obtain ⟨h1, h2, h3⟩ := hstep'
      have hih := ih h3
      exact ⟨hih.1.trans h1, Nat.le_trans hih.2 h2⟩

/-! ## Scenario batteries (reachability witnesses)

Six batteries, one per externally meaningful behavior class, each an
explicit step chain (the Stage-5 idiom).  Batteries B–F start from an
intermediate configuration; each start state is a constructed state
proved safe by `qSafe`, and its shape is one the TLA mirror explores
from `primInit` (the boot states `full2`/`emptyc` and the
park-coverage witnesses). -/

/-- Battery A — the inline cycle: `push a` commits inline and returns
`committed`; `pop` then delivers `a` in commit order. -/
abbrev qa0 : PrimCfg QSig qPrim := primInit QSig qPrim

abbrev qa1 : PrimCfg QSig qPrim :=
  { prim := qInit, now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := QCall.qpush QItem.a, fresh := true }],
    retired := ([] : List FiberId), nextFiber := 1, exts := [] }

abbrev qa2 : PrimCfg QSig qPrim :=
  { prim := qInit, now := 0,
    cur := some (FSlot.running { fiber := 0, call := QCall.qpush QItem.a } false),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qa3 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [],
              resolved := [], clog := [QItem.a], dlog := [] },
    now := 0,
    cur := some (FSlot.returning { fiber := 0, call := QCall.qpush QItem.a }
      QRes.qCommitted),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qa4 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

abbrev qa5 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpop, fresh := true }], retired := [], nextFiber := 1, exts := [] }

abbrev qa6 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := some (FSlot.running { fiber := 0, call := QCall.qpop } false), parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qa7 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := false, waitqP := [], waitqC := [],
              resolved := [], clog := [QItem.a], dlog := [QItem.a] },
    now := 0,
    cur := some (FSlot.returning { fiber := 0, call := QCall.qpop }
      (QRes.qItem QItem.a)),
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qa8 : PrimCfg QSig qPrim :=
{ prim := { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem qad1 : PrimStep2 QSig qPrim qa0 none qa1 :=
  PrimStep2.submit qa0 0 (QCall.qpush QItem.a) (Or.inl rfl)

theorem qad2 : PrimStep2 QSig qPrim qa1
    (some (issueObs QSig (Caller.fiber 0) (QCall.qpush QItem.a))) qa2 := by
  refine PrimStep2.dispatchFresh qa1
    { fiber := 0, call := QCall.qpush QItem.a, fresh := true } [] qInit ?_ ?_ ?_ ?_
  all_goals rfl

theorem qad3 : PrimStep2 QSig qPrim qa2 none qa3 :=
  PrimStep2.fiberEffect qa2 { fiber := 0, call := QCall.qpush QItem.a } false [] []
    QRes.qCommitted { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] } []
    rfl rfl rfl rfl Wakes.nil

theorem qad4 : PrimStep2 QSig qPrim qa3
    (some (compObs QSig (Caller.fiber 0) (QCall.qpush QItem.a) QRes.qCommitted)) qa4 :=
  PrimStep2.fiberDone qa3 { fiber := 0, call := QCall.qpush QItem.a }
    QRes.qCommitted rfl

theorem qad5 : PrimStep2 QSig qPrim qa4 none qa5 :=
  PrimStep2.submit qa4 0 QCall.qpop (Or.inr (by simp))

theorem qad6 : PrimStep2 QSig qPrim qa5
    (some (issueObs QSig (Caller.fiber 0) QCall.qpop)) qa6 := by
  refine PrimStep2.dispatchFresh qa5
    { fiber := 0, call := QCall.qpop, fresh := true } []
    { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] } ?_ ?_ ?_ ?_
  all_goals rfl

theorem qad7 : PrimStep2 QSig qPrim qa6 none qa7 :=
  PrimStep2.fiberEffect qa6 { fiber := 0, call := QCall.qpop } false [] []
    (QRes.qItem QItem.a) { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] } []
    rfl rfl rfl rfl Wakes.nil

theorem qad8 : PrimStep2 QSig qPrim qa7
    (some (compObs QSig (Caller.fiber 0) QCall.qpop (QRes.qItem QItem.a))) qa8 :=
  PrimStep2.fiberDone qa7 { fiber := 0, call := QCall.qpop }
    (QRes.qItem QItem.a) rfl

theorem q_battery_inline :
    ∃ fin : PrimCfg QSig qPrim,
      PrimRuns2 QSig qPrim qa0
        [issueObs QSig (Caller.fiber 0) (QCall.qpush QItem.a),
         compObs QSig (Caller.fiber 0) (QCall.qpush QItem.a) QRes.qCommitted,
         issueObs QSig (Caller.fiber 0) QCall.qpop,
         compObs QSig (Caller.fiber 0) QCall.qpop (QRes.qItem QItem.a)]
        fin ∧
      fin.prim.ring = [] ∧ fin.prim.clog = [QItem.a] ∧ fin.prim.dlog = [QItem.a] ∧
        fin.prim.resolved = [] := by
  refine ⟨qa8, ?_, rfl, rfl, rfl, rfl⟩
  exact PrimRuns2.step qa0 qa1 none _ qa8 qad1
    (PrimRuns2.step qa1 qa2 _ _ qa8 qad2
      (PrimRuns2.step qa2 qa3 none _ qa8 qad3
        (PrimRuns2.step qa3 qa4 _ _ qa8 qad4
          (PrimRuns2.step qa4 qa5 none _ qa8 qad5
            (PrimRuns2.step qa5 qa6 _ _ qa8 qad6
              (PrimRuns2.step qa6 qa7 none _ qa8 qad7
                (PrimRuns2.step qa7 qa8 _ _ qa8 qad8 (PrimRuns2.stop qa8))))))))

/-- Battery B — the producer handoff: with the ring full and a producer
suspended on its lease, an external `try_pop` delivers the ring head and
the cross-grant commits the suspended producer's item, which the resumed
producer then reports. -/
abbrev qb0 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a, QItem.b], closed := false, waitqP := [(0, QItem.a)], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 0, call := QCall.qpush QItem.a }], runq := ([] : List (PReady QSig)), retired := ([] : List FiberId), nextFiber := 1, exts := [] }

abbrev qb1 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a, QItem.b], closed := false, waitqP := [(0, QItem.a)], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 0, call := QCall.qpush QItem.a }], runq := [], retired := [], nextFiber := 1, exts := [{ x := 0, call := QCall.qtrypop, result := none }] }

abbrev qb2 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.b, QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [(0, QOut.oCommitted)], clog := [QItem.a, QItem.b, QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpush QItem.a, fresh := false }], retired := ([] : List FiberId), nextFiber := 1, exts := [{ x := 0, call := QCall.qtrypop, result := some (QRes.qItem QItem.a) }] }

abbrev qb3 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.b, QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [(0, QOut.oCommitted)], clog := [QItem.a, QItem.b, QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpush QItem.a, fresh := false }], retired := [], nextFiber := 1, exts := [] }

abbrev qb4 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.b, QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [(0, QOut.oCommitted)], clog := [QItem.a, QItem.b, QItem.a], dlog := [QItem.a] }, now := 0, cur := some (FSlot.running { fiber := 0, call := QCall.qpush QItem.a } true), parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qb5 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.b, QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b, QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem qbd1 : PrimStep2 QSig qPrim qb0
    (some (issueObs QSig (Caller.ext 0) QCall.qtrypop)) qb1 :=
  PrimStep2.extApply qb0 0 QCall.qtrypop [] [] rfl (by simp)

theorem qbd2 : PrimStep2 QSig qPrim qb1 none qb2 := by
  refine PrimStep2.extEffect qb1 [] []
    { x := 0, call := QCall.qtrypop, result := none }
    [{ fiber := 0, call := QCall.qpush QItem.a }] [] (QRes.qItem QItem.a)
    { ring := [QItem.b, QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [(0, QOut.oCommitted)], clog := [QItem.a, QItem.b, QItem.a], dlog := [QItem.a] } [0] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.drop _ Wakes.nil
    | exact Wakes.nil
    | simp

theorem qbd3 : PrimStep2 QSig qPrim qb2
    (some (compObs QSig (Caller.ext 0) QCall.qtrypop (QRes.qItem QItem.a))) qb3 :=
  PrimStep2.extDone qb2 [] []
    { x := 0, call := QCall.qtrypop, result := some (QRes.qItem QItem.a) }
    (QRes.qItem QItem.a) rfl rfl

theorem qbd4 : PrimStep2 QSig qPrim qb3 none qb4 := by
  refine PrimStep2.dispatchResumed qb3
    { fiber := 0, call := QCall.qpush QItem.a, fresh := false } [] ?_ ?_ ?_
  all_goals rfl

theorem qbd5 : PrimStep2 QSig qPrim qb4
    (some (compObs QSig (Caller.fiber 0) (QCall.qpush QItem.a) QRes.qCommitted)) qb5 :=
  PrimStep2.finishDone qb4 { fiber := 0, call := QCall.qpush QItem.a } true [] []
    QRes.qCommitted { ring := [QItem.b, QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b, QItem.a], dlog := [QItem.a] } [] rfl rfl rfl rfl Wakes.nil

theorem q_battery_handoff_pc :
    ∃ fin : PrimCfg QSig qPrim,
      PrimRuns2 QSig qPrim qb0
        [issueObs QSig (Caller.ext 0) QCall.qtrypop,
         compObs QSig (Caller.ext 0) QCall.qtrypop (QRes.qItem QItem.a),
         compObs QSig (Caller.fiber 0) (QCall.qpush QItem.a) QRes.qCommitted]
        fin ∧
      fin.prim.ring = [QItem.b, QItem.a] ∧ fin.prim.dlog = [QItem.a] ∧
        fin.prim.clog = [QItem.a, QItem.b, QItem.a] ∧ fin.prim.resolved = [] ∧
        fin.prim.waitqP = [] := by
  refine ⟨qb5, ?_, rfl, rfl, rfl, rfl, rfl⟩
  exact PrimRuns2.step qb0 qb1 _ _ qb5 qbd1
    (PrimRuns2.step qb1 qb2 none _ qb5 qbd2
      (PrimRuns2.step qb2 qb3 _ _ qb5 qbd3
        (PrimRuns2.step qb3 qb4 none _ qb5 qbd4
          (PrimRuns2.step qb4 qb5 _ _ qb5 qbd5 (PrimRuns2.stop qb5)))))

/-- Battery C — the consumer handoff: with a consumer suspended on an
empty ring, an inline `push` commits and the cross-grant delivers the
committed item to the suspended consumer, which the resumed consumer
reports. -/
abbrev qc0 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 1, call := QCall.qpop }], runq := ([] : List (PReady QSig)), retired := [0], nextFiber := 2, exts := [] }

abbrev qc1 : PrimCfg QSig qPrim :=
{ prim := { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 1, call := QCall.qpop }], runq := [{ fiber := 0, call := QCall.qpush QItem.a, fresh := true }], retired := [], nextFiber := 2, exts := [] }

abbrev qc2 : PrimCfg QSig qPrim :=
{ prim := { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] }, now := 0, cur := some (FSlot.running { fiber := 0, call := QCall.qpush QItem.a } false), parked := [{ fiber := 1, call := QCall.qpop }], runq := [], retired := [], nextFiber := 2, exts := [] }

abbrev qc3 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [(1, QOut.oItem QItem.a)], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := some (FSlot.returning { fiber := 0, call := QCall.qpush QItem.a } QRes.qCommitted), parked := [], runq := [{ fiber := 1, call := QCall.qpop, fresh := false }], retired := ([] : List FiberId), nextFiber := 2, exts := [] }

abbrev qc4 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [(1, QOut.oItem QItem.a)], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [{ fiber := 1, call := QCall.qpop, fresh := false }], retired := [0], nextFiber := 2, exts := [] }

abbrev qc5 : PrimCfg QSig qPrim :=
{ prim := { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [(1, QOut.oItem QItem.a)], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := some (FSlot.running { fiber := 1, call := QCall.qpop } true), parked := [], runq := [], retired := [0], nextFiber := 2, exts := [] }

abbrev qc6 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [], retired := [1, 0], nextFiber := 2, exts := [] }

theorem qcd1 : PrimStep2 QSig qPrim qc0 none qc1 :=
  PrimStep2.submit qc0 0 (QCall.qpush QItem.a) (Or.inr (by simp))

theorem qcd2 : PrimStep2 QSig qPrim qc1
    (some (issueObs QSig (Caller.fiber 0) (QCall.qpush QItem.a))) qc2 := by
  refine PrimStep2.dispatchFresh qc1
    { fiber := 0, call := QCall.qpush QItem.a, fresh := true } []
    { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] } ?_ ?_ ?_ ?_
  all_goals rfl

theorem qcd3 : PrimStep2 QSig qPrim qc2 none qc3 := by
  refine PrimStep2.fiberEffect qc2 { fiber := 0, call := QCall.qpush QItem.a } false
    [{ fiber := 1, call := QCall.qpop }] [] QRes.qCommitted
    { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [(1, QOut.oItem QItem.a)], clog := [QItem.a], dlog := [QItem.a] }
    [1] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.drop _ Wakes.nil
    | exact Wakes.nil
    | simp

theorem qcd4 : PrimStep2 QSig qPrim qc3
    (some (compObs QSig (Caller.fiber 0) (QCall.qpush QItem.a) QRes.qCommitted)) qc4 :=
  PrimStep2.fiberDone qc3 { fiber := 0, call := QCall.qpush QItem.a }
    QRes.qCommitted rfl

theorem qcd5 : PrimStep2 QSig qPrim qc4 none qc5 := by
  refine PrimStep2.dispatchResumed qc4
    { fiber := 1, call := QCall.qpop, fresh := false } [] ?_ ?_ ?_
  all_goals rfl

theorem qcd6 : PrimStep2 QSig qPrim qc5
    (some (compObs QSig (Caller.fiber 1) QCall.qpop (QRes.qItem QItem.a))) qc6 :=
  PrimStep2.finishDone qc5 { fiber := 1, call := QCall.qpop } true [] []
    (QRes.qItem QItem.a) { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] } []
    rfl rfl rfl rfl Wakes.nil

theorem q_battery_handoff_cp :
    ∃ fin : PrimCfg QSig qPrim,
      PrimRuns2 QSig qPrim qc0
        [issueObs QSig (Caller.fiber 0) (QCall.qpush QItem.a),
         compObs QSig (Caller.fiber 0) (QCall.qpush QItem.a) QRes.qCommitted,
         compObs QSig (Caller.fiber 1) QCall.qpop (QRes.qItem QItem.a)]
        fin ∧
      fin.prim.ring = [] ∧ fin.prim.dlog = [QItem.a] ∧ fin.prim.clog = [QItem.a] ∧
        fin.prim.resolved = [] ∧ fin.prim.waitqC = [] := by
  refine ⟨qc6, ?_, rfl, rfl, rfl, rfl, rfl⟩
  exact PrimRuns2.step qc0 qc1 none _ qc6 qcd1
    (PrimRuns2.step qc1 qc2 _ _ qc6 qcd2
      (PrimRuns2.step qc2 qc3 none _ qc6 qcd3
        (PrimRuns2.step qc3 qc4 _ _ qc6 qcd4
          (PrimRuns2.step qc4 qc5 none _ qc6 qcd5
            (PrimRuns2.step qc5 qc6 _ _ qc6 qcd6 (PrimRuns2.stop qc6))))))

/-- Battery D — `close` over a buffered item: the bit stands, the item
stays in the ring for later pops (the first `pop` still delivers it),
and a further `pop` reads `closed`. -/
abbrev qd0 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := ([] : List (PReady QSig)), retired := [0], nextFiber := 1, exts := [] }

abbrev qd1 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [{ x := 0, call := QCall.qclose, result := none }] }

abbrev qd2 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [{ x := 0, call := QCall.qclose, result := some QRes.qDone }] }

abbrev qd3 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

abbrev qd4 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpop, fresh := true }], retired := [], nextFiber := 1, exts := [] }

abbrev qd5 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := some (FSlot.running { fiber := 0, call := QCall.qpop } false), parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qd6 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := some (FSlot.returning { fiber := 0, call := QCall.qpop } (QRes.qItem QItem.a)), parked := [], runq := [], retired := ([] : List FiberId), nextFiber := 1, exts := [] }

abbrev qd7 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

abbrev qd8 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpop, fresh := true }], retired := [], nextFiber := 1, exts := [] }

abbrev qd9 : PrimCfg QSig qPrim :=
{ prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := some (FSlot.running { fiber := 0, call := QCall.qpop } false), parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def qd10 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := some (FSlot.returning { fiber := 0, call := QCall.qpop } QRes.qClosed), parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def qd11 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem qdd1 : PrimStep2 QSig qPrim qd0
    (some (issueObs QSig (Caller.ext 0) QCall.qclose)) qd1 :=
  PrimStep2.extApply qd0 0 QCall.qclose [] [] rfl (by simp)

theorem qdd2 : PrimStep2 QSig qPrim qd1 none qd2 := by
  refine PrimStep2.extEffect qd1 [] []
    { x := 0, call := QCall.qclose, result := none } [] [] QRes.qDone
    { ring := [QItem.a], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] } [] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.nil
    | simp

theorem qdd3 : PrimStep2 QSig qPrim qd2
    (some (compObs QSig (Caller.ext 0) QCall.qclose QRes.qDone)) qd3 :=
  PrimStep2.extDone qd2 [] []
    { x := 0, call := QCall.qclose, result := some QRes.qDone } QRes.qDone rfl rfl

theorem qdd4 : PrimStep2 QSig qPrim qd3 none qd4 :=
  PrimStep2.submit qd3 0 QCall.qpop (Or.inr (by simp))

theorem qdd5 : PrimStep2 QSig qPrim qd4
    (some (issueObs QSig (Caller.fiber 0) QCall.qpop)) qd5 := by
  refine PrimStep2.dispatchFresh qd4
    { fiber := 0, call := QCall.qpop, fresh := true } []
    { ring := [QItem.a], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] } ?_ ?_ ?_ ?_
  all_goals rfl

theorem qdd6 : PrimStep2 QSig qPrim qd5 none qd6 :=
  PrimStep2.fiberEffect qd5 { fiber := 0, call := QCall.qpop } false [] []
    (QRes.qItem QItem.a) { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] } []
    rfl rfl rfl rfl Wakes.nil

theorem qdd7 : PrimStep2 QSig qPrim qd6
    (some (compObs QSig (Caller.fiber 0) QCall.qpop (QRes.qItem QItem.a))) qd7 :=
  PrimStep2.fiberDone qd6 { fiber := 0, call := QCall.qpop }
    (QRes.qItem QItem.a) rfl

theorem qdd8 : PrimStep2 QSig qPrim qd7 none qd8 :=
  PrimStep2.submit qd7 0 QCall.qpop (Or.inr (by simp))

theorem qdd9 : PrimStep2 QSig qPrim qd8
    (some (issueObs QSig (Caller.fiber 0) QCall.qpop)) qd9 := by
  refine PrimStep2.dispatchFresh qd8
    { fiber := 0, call := QCall.qpop, fresh := true } []
    { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] } ?_ ?_ ?_ ?_
  all_goals rfl

theorem qdd10 : PrimStep2 QSig qPrim qd9 none qd10 :=
  PrimStep2.fiberEffect qd9 { fiber := 0, call := QCall.qpop } false [] []
    QRes.qClosed { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] } []
    rfl rfl rfl rfl Wakes.nil

theorem qdd11 : PrimStep2 QSig qPrim qd10
    (some (compObs QSig (Caller.fiber 0) QCall.qpop QRes.qClosed)) qd11 :=
  PrimStep2.fiberDone qd10 { fiber := 0, call := QCall.qpop } QRes.qClosed rfl

theorem q_battery_close_buffered :
    ∃ fin : PrimCfg QSig qPrim,
      PrimRuns2 QSig qPrim qd0
        [issueObs QSig (Caller.ext 0) QCall.qclose,
         compObs QSig (Caller.ext 0) QCall.qclose QRes.qDone,
         issueObs QSig (Caller.fiber 0) QCall.qpop,
         compObs QSig (Caller.fiber 0) QCall.qpop (QRes.qItem QItem.a),
         issueObs QSig (Caller.fiber 0) QCall.qpop,
         compObs QSig (Caller.fiber 0) QCall.qpop QRes.qClosed]
        fin ∧
      fin.prim.closed = true ∧ fin.prim.ring = [] ∧ fin.prim.dlog = [QItem.a] ∧
        fin.prim.clog = [QItem.a] := by
  refine ⟨qd11, ?_, rfl, rfl, rfl, rfl⟩
  exact PrimRuns2.step qd0 qd1 _ _ qd11 qdd1
    (PrimRuns2.step qd1 qd2 none _ qd11 qdd2
      (PrimRuns2.step qd2 qd3 _ _ qd11 qdd3
        (PrimRuns2.step qd3 qd4 none _ qd11 qdd4
          (PrimRuns2.step qd4 qd5 _ _ qd11 qdd5
            (PrimRuns2.step qd5 qd6 none _ qd11 qdd6
              (PrimRuns2.step qd6 qd7 _ _ qd11 qdd7
                (PrimRuns2.step qd7 qd8 none _ qd11 qdd8
                  (PrimRuns2.step qd8 qd9 _ _ qd11 qdd9
                    (PrimRuns2.step qd9 qd10 none _ qd11 qdd10
                      (PrimRuns2.step qd10 qd11 _ _ qd11 qdd11
                        (PrimRuns2.stop qd11)))))))))))

/-- Battery E — `close` with a suspended consumer: the drain resolves
the consumer's outcome to `closed`, which the resumed consumer reports. -/
abbrev qe0 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 1, call := QCall.qpop }], runq := ([] : List (PReady QSig)), retired := ([] : List FiberId), nextFiber := 2, exts := [] }

abbrev qe1 : PrimCfg QSig qPrim :=
{ prim := { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 1, call := QCall.qpop }], runq := [], retired := [], nextFiber := 2, exts := [{ x := 0, call := QCall.qclose, result := none }] }

abbrev qe2 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [(1, QOut.oClosed)], clog := [], dlog := [] }, now := 0, cur := none, parked := [], runq := [{ fiber := 1, call := QCall.qpop, fresh := false }], retired := ([] : List FiberId), nextFiber := 2, exts := [{ x := 0, call := QCall.qclose, result := some QRes.qDone }] }

abbrev qe3 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [(1, QOut.oClosed)], clog := [], dlog := [] }, now := 0, cur := none, parked := [], runq := [{ fiber := 1, call := QCall.qpop, fresh := false }], retired := [], nextFiber := 2, exts := [] }

abbrev qe4 : PrimCfg QSig qPrim :=
{ prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [(1, QOut.oClosed)], clog := [], dlog := [] }, now := 0, cur := some (FSlot.running { fiber := 1, call := QCall.qpop } true), parked := [], runq := [], retired := [], nextFiber := 2, exts := [] }

abbrev qe5 : PrimCfg QSig qPrim :=
  { prim := { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := [1], nextFiber := 2, exts := [] }

theorem qed1 : PrimStep2 QSig qPrim qe0
    (some (issueObs QSig (Caller.ext 0) QCall.qclose)) qe1 :=
  PrimStep2.extApply qe0 0 QCall.qclose [] [] rfl (by simp)

theorem qed2 : PrimStep2 QSig qPrim qe1 none qe2 := by
  refine PrimStep2.extEffect qe1 [] []
    { x := 0, call := QCall.qclose, result := none }
    [{ fiber := 1, call := QCall.qpop }] [] QRes.qDone
    { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [(1, QOut.oClosed)], clog := [], dlog := [] } [1] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.drop _ Wakes.nil
    | exact Wakes.nil
    | simp

theorem qed3 : PrimStep2 QSig qPrim qe2
    (some (compObs QSig (Caller.ext 0) QCall.qclose QRes.qDone)) qe3 :=
  PrimStep2.extDone qe2 [] []
    { x := 0, call := QCall.qclose, result := some QRes.qDone } QRes.qDone rfl rfl

theorem qed4 : PrimStep2 QSig qPrim qe3 none qe4 := by
  refine PrimStep2.dispatchResumed qe3
    { fiber := 1, call := QCall.qpop, fresh := false } [] ?_ ?_ ?_
  all_goals rfl

theorem qed5 : PrimStep2 QSig qPrim qe4
    (some (compObs QSig (Caller.fiber 1) QCall.qpop QRes.qClosed)) qe5 :=
  PrimStep2.finishDone qe4 { fiber := 1, call := QCall.qpop } true [] []
    QRes.qClosed { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] } [] rfl rfl rfl rfl Wakes.nil

theorem q_battery_close_consumer :
    ∃ fin : PrimCfg QSig qPrim,
      PrimRuns2 QSig qPrim qe0
        [issueObs QSig (Caller.ext 0) QCall.qclose,
         compObs QSig (Caller.ext 0) QCall.qclose QRes.qDone,
         compObs QSig (Caller.fiber 1) QCall.qpop QRes.qClosed]
        fin ∧
      fin.prim.closed = true ∧ fin.prim.waitqC = [] ∧ fin.prim.resolved = [] := by
  refine ⟨qe5, ?_, rfl, rfl, rfl⟩
  exact PrimRuns2.step qe0 qe1 _ _ qe5 qed1
    (PrimRuns2.step qe1 qe2 none _ qe5 qed2
      (PrimRuns2.step qe2 qe3 _ _ qe5 qed3
        (PrimRuns2.step qe3 qe4 none _ qe5 qed4
          (PrimRuns2.step qe4 qe5 _ _ qe5 qed5 (PrimRuns2.stop qe5)))))

/-- Battery F — `close` with a suspended producer and buffered items:
the drain resolves the producer's outcome to `closed` (the bit refuses
the lease), the buffered items stay poppable, and the retired producer
pops one. -/
abbrev qf0 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a, QItem.b], closed := false, waitqP := [(0, QItem.b)], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 0, call := QCall.qpush QItem.b }], runq := ([] : List (PReady QSig)), retired := ([] : List FiberId), nextFiber := 1, exts := [] }

abbrev qf1 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a, QItem.b], closed := false, waitqP := [(0, QItem.b)], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [{ fiber := 0, call := QCall.qpush QItem.b }], runq := [], retired := [], nextFiber := 1, exts := [{ x := 0, call := QCall.qclose, result := none }] }

abbrev qf2 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [(0, QOut.oClosed)], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpush QItem.b, fresh := false }], retired := ([] : List FiberId), nextFiber := 1, exts := [{ x := 0, call := QCall.qclose, result := some QRes.qDone }] }

abbrev qf3 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [(0, QOut.oClosed)], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpush QItem.b, fresh := false }], retired := [], nextFiber := 1, exts := [] }

abbrev qf4 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [(0, QOut.oClosed)], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := some (FSlot.running { fiber := 0, call := QCall.qpush QItem.b } true), parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qf5 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

abbrev qf6 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := none, parked := [], runq := [{ fiber := 0, call := QCall.qpop, fresh := true }], retired := [], nextFiber := 1, exts := [] }

abbrev qf7 : PrimCfg QSig qPrim :=
{ prim := { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] }, now := 0, cur := some (FSlot.running { fiber := 0, call := QCall.qpop } false), parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

abbrev qf8 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [QItem.a] }, now := 0, cur := some (FSlot.returning { fiber := 0, call := QCall.qpop } (QRes.qItem QItem.a)), parked := [], runq := [], retired := ([] : List FiberId), nextFiber := 1, exts := [] }

abbrev qf9 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [QItem.a] }, now := 0, cur := none, parked := [], runq := [], retired := [0], nextFiber := 1, exts := [] }

theorem qfd1 : PrimStep2 QSig qPrim qf0
    (some (issueObs QSig (Caller.ext 0) QCall.qclose)) qf1 :=
  PrimStep2.extApply qf0 0 QCall.qclose [] [] rfl (by simp)

theorem qfd2 : PrimStep2 QSig qPrim qf1 none qf2 := by
  refine PrimStep2.extEffect qf1 [] []
    { x := 0, call := QCall.qclose, result := none }
    [{ fiber := 0, call := QCall.qpush QItem.b }] [] QRes.qDone
    { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [(0, QOut.oClosed)], clog := [QItem.a, QItem.b], dlog := [] }
    [0] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.drop _ Wakes.nil
    | exact Wakes.nil
    | simp

theorem qfd3 : PrimStep2 QSig qPrim qf2
    (some (compObs QSig (Caller.ext 0) QCall.qclose QRes.qDone)) qf3 :=
  PrimStep2.extDone qf2 [] []
    { x := 0, call := QCall.qclose, result := some QRes.qDone } QRes.qDone rfl rfl

theorem qfd4 : PrimStep2 QSig qPrim qf3 none qf4 := by
  refine PrimStep2.dispatchResumed qf3
    { fiber := 0, call := QCall.qpush QItem.b, fresh := false } [] ?_ ?_ ?_
  all_goals rfl

theorem qfd5 : PrimStep2 QSig qPrim qf4
    (some (compObs QSig (Caller.fiber 0) (QCall.qpush QItem.b) QRes.qClosed)) qf5 :=
  PrimStep2.finishDone qf4 { fiber := 0, call := QCall.qpush QItem.b } true [] []
    QRes.qClosed { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] } []
    rfl rfl rfl rfl Wakes.nil

theorem qfd6 : PrimStep2 QSig qPrim qf5 none qf6 :=
  PrimStep2.submit qf5 0 QCall.qpop (Or.inr (by simp))

theorem qfd7 : PrimStep2 QSig qPrim qf6
    (some (issueObs QSig (Caller.fiber 0) QCall.qpop)) qf7 := by
  refine PrimStep2.dispatchFresh qf6
    { fiber := 0, call := QCall.qpop, fresh := true } []
    { ring := [QItem.a, QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] } ?_ ?_ ?_ ?_
  all_goals rfl

theorem qfd8 : PrimStep2 QSig qPrim qf7 none qf8 :=
  PrimStep2.fiberEffect qf7 { fiber := 0, call := QCall.qpop } false [] []
    (QRes.qItem QItem.a) { ring := [QItem.b], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [QItem.a] }
    [] rfl rfl rfl rfl Wakes.nil

theorem qfd9 : PrimStep2 QSig qPrim qf8
    (some (compObs QSig (Caller.fiber 0) QCall.qpop (QRes.qItem QItem.a))) qf9 :=
  PrimStep2.fiberDone qf8 { fiber := 0, call := QCall.qpop }
    (QRes.qItem QItem.a) rfl

theorem q_battery_close_producer :
    ∃ fin : PrimCfg QSig qPrim,
      PrimRuns2 QSig qPrim qf0
        [issueObs QSig (Caller.ext 0) QCall.qclose,
         compObs QSig (Caller.ext 0) QCall.qclose QRes.qDone,
         compObs QSig (Caller.fiber 0) (QCall.qpush QItem.b) QRes.qClosed,
         issueObs QSig (Caller.fiber 0) QCall.qpop,
         compObs QSig (Caller.fiber 0) QCall.qpop (QRes.qItem QItem.a)]
        fin ∧
      fin.prim.closed = true ∧ fin.prim.ring = [QItem.b] ∧
        fin.prim.dlog = [QItem.a] ∧ fin.prim.clog = [QItem.a, QItem.b] := by
  refine ⟨qf9, ?_, rfl, rfl, rfl, rfl⟩
  exact PrimRuns2.step qf0 qf1 _ _ qf9 qfd1
    (PrimRuns2.step qf1 qf2 none _ qf9 qfd2
      (PrimRuns2.step qf2 qf3 _ _ qf9 qfd3
        (PrimRuns2.step qf3 qf4 none _ qf9 qfd4
          (PrimRuns2.step qf4 qf5 _ _ qf9 qfd5
            (PrimRuns2.step qf5 qf6 none _ qf9 qfd6
              (PrimRuns2.step qf6 qf7 _ _ qf9 qfd7
                (PrimRuns2.step qf7 qf8 none _ qf9 qfd8
                  (PrimRuns2.step qf8 qf9 _ _ qf9 qfd9 (PrimRuns2.stop qf9)))))))))

/-! ## The capability adjudication: RESEARCH / DEFER

The judgment layer, over the frozen base
`BASE(AsyncQueue) = {Semaphore}` (`SemaphoreOps`).  The pre-V2.3 stage-6
THEOREM B is retired: its completion-shadow projection is invalid under
the V2.3 discipline (`tracesEnc_shadow_false`).  What is provable here
are two conditional, per-encoding-class statements.  Neither is named
THEOREM B, and neither closes the universal question:

  * Over-production for the class that declares the fiber-bound `push`
    externally callable: the primitive never emits an external `push`
    issue (`qExtCap (push _) = false`), while every such encoding does --
    at the encoding machine's entry step, before any program runs.
  * Under-production for the class that declares `try_push` not
    externally callable: the primitive emits the external `try_push`
    issue (the code's `try_push` reads no `g_worker`), while no such
    encoding can.

The class itself is inhabited (the natural shapes below bottom out with
no completed observation: one semaphore cannot carry an item through
the two-slot FIFO discipline).  Whether SOME encoding over the frozen
base is observationally equivalent to `qPrim` remains open in both
directions; see the stage card for the exact open-boundary wording. -/

/-- The natural queue-shaped encoding over the semaphore substrate:
call domains declared faithfully (`try_push`/`try_pop`/`close`
external), programs that bottom out with a value no decoding accepts. -/
def encQ : Encoding SemaphoreOps QSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := qExtCap

theorem q_encoding_class_inhabited :
    ∃ _enc : Encoding SemaphoreOps QSig, True := ⟨encQ, trivial⟩

/-- The over-production separator: a bare external `push` issue. -/
def extPushTrace : Trace QSig :=
  [issueObs QSig (Caller.ext 0) (QCall.qpush QItem.a)]

theorem seqOK_extPush : SeqOK QSig extPushTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

/-- The primitive never produces the external `push` issue: the only
step that emits an external issue is `extApply`, and it requires
`qExtCap (push _) = true`. -/
theorem q_not_extPush : ¬ TracesPrimS QSig qPrim extPushTrace := by
  rintro ⟨⟨fin, hrun⟩, -⟩
  obtain ⟨ob, t', heq⟩ : ∃ ob t', extPushTrace = ob :: t' := ⟨_, _, rfl⟩
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := primRuns_cons hrun ob t' heq
  cases hstep with
  | dispatchFresh rp rest s' hcurE hrunqE hfreshE hadmit =>
      injection heq with h3 h4
      simp [issueObs] at h3
  | extApply x c preE postE hcap hnovel =>
      injection heq with h3 h4
      simp only [issueObs, Obs.mk.injEq] at h3
      rw [← h3.2.1] at hcap
      simp [qExtCap] at hcap
  | fiberDone d r hcurE =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3
  | finishDone d b ps rest r s' wk hcurE hb hfinE hmap hwake =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3
  | extDone preE postE e r hextsE hresE =>
      injection heq with h3 h4
      simp [issueObs, compObs] at h3

/-- Every encoding declaring `push` external emits the bare issue at
entry (`SysStep.extStart`). -/
theorem enc_extPush (enc : Encoding SemaphoreOps QSig)
    (hcap : enc.extCap (QCall.qpush QItem.a) = true) :
    TracesEnc SemaphoreOps QSig enc extPushTrace :=
  ⟨{ st := subInit, bst := SemaphoreOps.baseInit, cur := none, parked := [], runq := ([] : List (Ready SemaphoreOps QSig)), retired := ([] : List FiberId), nextFiber := 0, exts := [{ x := 0, call := QCall.qpush QItem.a, prog := enc.prog (QCall.qpush QItem.a) }] },
    SysRuns.step (encInit SemaphoreOps QSig) _ _ [] _
      (SysStep.extStart (encInit SemaphoreOps QSig) 0 (QCall.qpush QItem.a) hcap
        (by show (0 : ExternalId) ∉ ([] : List (ExtBusy SemaphoreOps QSig)).map
              (fun e : ExtBusy SemaphoreOps QSig => e.x)
            simp))
      (SysRuns.stop _)⟩

/-- Auxiliary, per-encoding-class result — NOT the stage verdict: an
encoding that declares the fiber-bound `push` external over-produces. -/
theorem q_over_produces_of_extCap (enc : Encoding SemaphoreOps QSig)
    (hcap : enc.extCap (QCall.qpush QItem.a) = true) :
    OverProduces qPrim SemaphoreOps enc :=
  ⟨extPushTrace, ⟨enc_extPush enc hcap, seqOK_extPush⟩,
    fun hP => q_not_extPush hP⟩

/-- The under-production separator: the external `try_push` issue plus
its committed completion, produced from the initial configuration. -/
def extTryPushTrace : Trace QSig :=
  [issueObs QSig (Caller.ext 0) (QCall.qtrypush QItem.a),
   compObs QSig (Caller.ext 0) (QCall.qtrypush QItem.a) QRes.qCommitted]

abbrev qpc0 : PrimCfg QSig qPrim := primInit QSig qPrim

abbrev qpc1 : PrimCfg QSig qPrim :=
  { prim := qInit, now := 0, cur := none, parked := [], runq := ([] : List (PReady QSig)), retired := ([] : List FiberId), nextFiber := 0, exts := [{ x := 0, call := QCall.qtrypush QItem.a, result := none }] }

abbrev qpc2 : PrimCfg QSig qPrim :=
  { prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := ([] : List FiberId), nextFiber := 0, exts := [{ x := 0, call := QCall.qtrypush QItem.a, result := some QRes.qCommitted }] }

abbrev qpc3 : PrimCfg QSig qPrim := { prim := { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, now := 0, cur := none, parked := [], runq := [], retired := [], nextFiber := 0, exts := [] }

theorem qpd1 : PrimStep2 QSig qPrim qpc0
    (some (issueObs QSig (Caller.ext 0) (QCall.qtrypush QItem.a))) qpc1 :=
  PrimStep2.extApply qpc0 0 (QCall.qtrypush QItem.a) [] [] rfl
    (by show (0 : ExternalId) ∉ ([] : List (ExtPend QSig)).map (fun e : ExtPend QSig => e.x)
        simp)

theorem qpd2 : PrimStep2 QSig qPrim qpc1 none qpc2 := by
  refine PrimStep2.extEffect qpc1 [] []
    { x := 0, call := QCall.qtrypush QItem.a, result := none } [] []
    QRes.qCommitted
    { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] } [] ?_ ?_ ?_ ?_ ?_
  all_goals first
    | rfl
    | exact Wakes.nil

theorem qpd3 : PrimStep2 QSig qPrim qpc2
    (some (compObs QSig (Caller.ext 0) (QCall.qtrypush QItem.a)
      QRes.qCommitted)) qpc3 :=
  PrimStep2.extDone qpc2 [] []
    { x := 0, call := QCall.qtrypush QItem.a, result := some QRes.qCommitted }
    QRes.qCommitted rfl rfl

theorem seqOK_extTryPush : SeqOK QSig extTryPushTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  exact SeqOKFrom.nil _

theorem q_possesses_extTryPush : TracesPrimS QSig qPrim extTryPushTrace :=
  ⟨⟨qpc3, PrimRuns2.step qpc0 qpc1 _ _ qpc3 qpd1
      (PrimRuns2.step qpc1 qpc2 none _ qpc3 qpd2
        (PrimRuns2.step qpc2 qpc3 _ _ qpc3 qpd3 (PrimRuns2.stop qpc3)))⟩,
    seqOK_extTryPush⟩

/-- Inverting the step that emits the external `try_push` issue. -/
theorem extCap_true_of_extTryPushIssue {enc : Encoding SemaphoreOps QSig}
    {cfg m2 : SysCfg SemaphoreOps QSig} {ob : Obs QSig}
    (hstep : SysStep SemaphoreOps QSig enc SubStep cfg (some ob) m2)
    (hx : ob.caller = Caller.ext 0) (hc : ob.call = QCall.qtrypush QItem.a)
    (hr : ob.result = Option.none) :
    enc.extCap (QCall.qtrypush QItem.a) = true := by
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
      have hc1 : c = QCall.qtrypush QItem.a := hc
      rw [hc1] at hc'
      exact hc'

/-- Auxiliary, per-encoding-class result — NOT the stage verdict: an
encoding that declares `try_push` internal under-produces (it can never
emit the external `try_push` issue the primitive produces). -/
theorem q_under_produces_of_no_extCap (enc : Encoding SemaphoreOps QSig)
    (hcap : enc.extCap (QCall.qtrypush QItem.a) = false) :
    UnderProduces qPrim SemaphoreOps enc := by
  refine ⟨extTryPushTrace, q_possesses_extTryPush, ?_⟩
  rintro ⟨hrun, -⟩
  obtain ⟨fin, hrun⟩ := hrun
  obtain ⟨m1, m2, h1, hstep, hrest⟩ := sysRuns_cons hrun _ _ rfl
  have hcap' := extCap_true_of_extTryPushIssue hstep rfl rfl rfl
  simp [hcap] at hcap'

/-- The Stage-6 capability record: the two class conditionals the frozen
method supports.  This is RESEARCH/DEFER, not THEOREM B: no universal
separation is claimed, and none is refuted. -/
theorem q_capability_defer :
    (∀ enc : Encoding SemaphoreOps QSig,
        enc.extCap (QCall.qpush QItem.a) = true →
          OverProduces qPrim SemaphoreOps enc) ∧
    (∀ enc : Encoding SemaphoreOps QSig,
        enc.extCap (QCall.qtrypush QItem.a) = false →
          UnderProduces qPrim SemaphoreOps enc) :=
  ⟨q_over_produces_of_extCap, q_under_produces_of_no_extCap⟩

/-- Non-vacuity instance for the under-production conditional. -/
def encNoTry : Encoding SemaphoreOps QSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := fun _ => false

theorem encNoTry_under : UnderProduces qPrim SemaphoreOps encNoTry :=
  q_under_produces_of_no_extCap encNoTry rfl

/-- Non-vacuity instance for the over-production conditional. -/
def encExtPush : Encoding SemaphoreOps QSig where
  prog := fun _ => ExtProg.pure SubVal.unit
  decode := fun _ _ => none
  extCap := fun c =>
    match c with
    | QCall.qpush _ => true
    | _ => false

theorem encExtPush_over : OverProduces qPrim SemaphoreOps encExtPush :=
  q_over_produces_of_extCap encExtPush rfl

/-! ## The mutant battery

Four hand-written faults, one per independent discipline the model
carries.  M1 and M2 are SAFETY kills (a state invariant breaks);
M3 and M5 are TRACE-REMOVAL kills (every invariant still holds, but the
handoff witnesses become unreachable — the TLA certificates run them
clean with `NotWitness`); M4 is a result-semantics kill (the state does
not move and stays safe; the public result lies). -/

/-- M1 (SAFETY — the close discipline): the `closed` gate dropped from
`push`.  A push on a closed queue commits and grows the commit log;
the model reads `closed`. -/
def qRunM1 : QState → Tick → FiberId → QCall →
    Option (QRes × QState × List FiberId)
  | s, _, _, QCall.qpush v =>
      if s.ring.length < qCap ∧ s.waitqP = [] then
        match qGrantConsumer { s with ring := s.ring ++ [v], clog := s.clog ++ [v] } with
        | (s2, ws2) => some (QRes.qCommitted, s2, ws2)
      else none
  | s, t, f, c => qRun s t f c

theorem qM1_breaks :
    qRunM1 { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] } 0 0 (QCall.qpush QItem.a)
      = some (QRes.qCommitted,
        { ring := [QItem.a], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }, []) ∧
    qRun { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] } 0 0 (QCall.qpush QItem.a)
      = some (QRes.qClosed,
        { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] }, []) :=
  ⟨rfl, rfl⟩

/-- M2 (SAFETY — external FIFO): `pop` delivers the second ring item.
The commit ledger `clog = dlog ++ ring` breaks. -/
def qRunM2 : QState → Tick → FiberId → QCall →
    Option (QRes × QState × List FiberId)
  | s, t, f, QCall.qpop =>
      match s.ring with
      | v :: w :: r =>
          if s.waitqC = [] then
            match qGrantProducer { s with ring := v :: r, dlog := s.dlog ++ [w] } with
            | (s2, ws2) => some (QRes.qItem w, s2, ws2)
          else none
      | [] => qRun s t f QCall.qpop
      | [v] => qRun s t f QCall.qpop
  | s, t, f, c => qRun s t f c

theorem qM2_fifo_break :
    qRunM2 { ring := [QItem.a, QItem.b], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] } 0 0 QCall.qpop
      = some (QRes.qItem QItem.b,
        { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [QItem.b] }, []) ∧
    qSafe { ring := [QItem.a, QItem.b], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [] } ∧
    ¬ qSafe { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [], clog := [QItem.a, QItem.b], dlog := [QItem.b] } :=
  ⟨rfl, by simp [qSafe, qCap], by simp [qSafe, qCap]⟩

/-- M3 (TRACE-REMOVAL — the producer handoff): `pop` delivers without
the producer cross-grant.  The state stays safe, but a suspended
producer is never granted: the handoff witness is unreachable. -/
def qRunM3 : QState → Tick → FiberId → QCall →
    Option (QRes × QState × List FiberId)
  | s, _, _, QCall.qpop =>
      match s.ring with
      | v :: r =>
          if s.waitqC = [] then
            some (QRes.qItem v, { s with ring := r, dlog := s.dlog ++ [v] }, [])
          else none
      | [] => none
  | s, t, f, c => qRun s t f c

theorem qM3_handoff_refuted :
    qRunM3 { ring := [QItem.a], closed := false, waitqP := [(0, QItem.a)], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }
        0 0 QCall.qpop
      = some (QRes.qItem QItem.a,
        { ring := [], closed := false, waitqP := [(0, QItem.a)], waitqC := [], resolved := [], clog := [QItem.a], dlog := [QItem.a] }, []) ∧
    qRun { ring := [QItem.a], closed := false, waitqP := [(0, QItem.a)], waitqC := [], resolved := [], clog := [QItem.a], dlog := [] }
        0 0 QCall.qpop
      = some (QRes.qItem QItem.a,
        { ring := [QItem.a], closed := false, waitqP := [], waitqC := [], resolved := [(0, QOut.oCommitted)], clog := [QItem.a, QItem.a], dlog := [QItem.a] }, [0]) :=
  ⟨rfl, rfl⟩

/-- M4 (RESULT-SEMANTICS): `try_pop` on an empty closed queue returns
`item a`.  The state does not move and stays safe; the public result
contradicts the outcome authority. -/
def qRunM4 : QState → Tick → FiberId → QCall →
    Option (QRes × QState × List FiberId)
  | s, t, f, QCall.qtrypop =>
      match s.ring with
      | [] =>
          if s.closed then some (QRes.qItem QItem.a, s, [])
          else some (QRes.qWouldBlock, s, [])
      | _ => qRun s t f QCall.qtrypop
  | s, t, f, c => qRun s t f c

theorem qM4_wrong_result :
    qRunM4 { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] } 0 0 QCall.qtrypop
      = some (QRes.qItem QItem.a,
        { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] }, []) ∧
    qSafe { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] } ∧
    (qSpecRun { ring := [], closed := true, waitqP := [], waitqC := [], resolved := [], clog := [], dlog := [] } QCall.qtrypop).map Prod.fst
      = some QRes.qClosed :=
  ⟨rfl, by simp [qSafe], rfl⟩

/-- M5 (TRACE-REMOVAL — the consumer handoff): `push` commits without
the consumer cross-grant.  The state stays safe, but a suspended
consumer is never granted: the handoff witness is unreachable. -/
def qRunM5 : QState → Tick → FiberId → QCall →
    Option (QRes × QState × List FiberId)
  | s, _, _, QCall.qpush v =>
      if s.closed then some (QRes.qClosed, s, [])
      else if s.ring.length < qCap ∧ s.waitqP = [] then
        some (QRes.qCommitted, { s with ring := s.ring ++ [v], clog := s.clog ++ [v] }, [])
      else none
  | s, t, f, c => qRun s t f c

theorem qM5_handoff_refuted :
    qRunM5 { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] } 0 0 (QCall.qpush QItem.a)
      = some (QRes.qCommitted,
        { ring := [QItem.a], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [QItem.a], dlog := [] }, []) ∧
    qRun { ring := [], closed := false, waitqP := [], waitqC := [1], resolved := [], clog := [], dlog := [] } 0 0 (QCall.qpush QItem.a)
      = some (QRes.qCommitted,
        { ring := [], closed := false, waitqP := [], waitqC := [], resolved := [(1, QOut.oItem QItem.a)], clog := [QItem.a], dlog := [QItem.a] }, [1]) :=
  ⟨rfl, rfl⟩

end Sluice.Formal
