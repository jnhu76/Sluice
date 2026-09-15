/-
Sluice Stage-0-V2 base calculus (FCB1-METHOD-CORRECTIVE-1).

V2 replaces the Stage-0 calculus after the method-corrective review.  The
correctives it implements:

  MAJOR A (symmetric completion discipline).  The encoding side and the
  primitive side now run under the *same* call-execution discipline: a
  submitted call is dispatched FIFO from a single runnable queue, then runs
  without interleaving until it suspends or physically returns.  A completion
  is emitted exactly at the running call's return step; no fiber can act
  between a call's last internal step and its return.  The V1
  completion-shadow projection is *not* valid under this discipline, and
  `tracesEnc_shadow_false` below proves its failure formally.

  MAJOR C (persistent fiber identity).  `FiberId` is a persistent execution
  identity (the `Fiber*` of the code).  The environment submits calls for
  idle fibers; one fiber has at most one call in flight, and the same fiber
  issues its later calls after its earlier call returned (the `retired`
  record and the `SeqOK` trace discipline below).

  MAJOR B (base-parameterized encodings).  The encoding language is
  parameterized by `BaseOpsSig` -- the frozen capability set `BASE(P)` an
  encoding may invoke.  `BaseOpsSig.none` is the bare substrate; a stage
  whose frozen base contains earlier primitives instantiates the signature
  with one operation per base call, executed as an abstract oracle over the
  base core's frozen semantics.

  MAJOR D (real freeze).  This file, with
  `docs/formal/stage-0-v2-base-calculus.md`, is the whole Stage-0-V2 freeze.
  Later stages may not change it without BRAKE-1 and downstream invalidation.

Every construct carries a comment naming its C++ counterpart where one exists.
-/

namespace Sluice.Formal

/-! ## Code-level identities

`FiberId` -- `Fiber*` identity of a coroutine; persistent across the calls
the fiber issues (`ws->current`, `WaitResume::fiber(me)`).
`Tick`     -- `deadline_tick_t` (`Scheduler::deadline_t`).
`QueueId`  -- a distinct `WaitQueue&`.
`WaiterId` -- a distinct `WaitNode&` registration token. -/

abbrev FiberId := Nat
abbrev Tick := Nat
abbrev QueueId := Nat
abbrev WaiterId := Nat

/-- Terminal wait outcomes (`WaitOutcome` in `include/sluice/async/wait_node.hpp`). -/
inductive Outcome : Type where
  | woken
  | cancelled
  | expired
deriving instance DecidableEq for Outcome

/-- A live waiter registration (a `WaitNode` linked into a `WaitQueue`). -/
structure Registration : Type where
  waiter : WaiterId
  owner : FiberId
  deadline : Option Tick

/-- Frozen substrate state: exactly the shared scheduler machinery
(`WaitQueue` FIFOs, resolved `WaitNode` outcomes, the monotonic clock). -/
structure SubState : Type where
  /-- Live registrations per queue; list head = queue head
  (`WaitQueue::head_`, `register_wait_locked` appends at the tail). -/
  queue : QueueId → List Registration
  /-- Resolved-but-unconsumed one-shot outcomes (`WaitNode::resolve_`). -/
  resolved : List (WaiterId × Outcome)
  /-- Monotonic clock (`Scheduler::monotonic_now`). -/
  now : Tick
  /-- Waiter token supply (fresh `WaitNode` addresses in the code). -/
  nextWaiter : WaiterId

/-- Update one queue, leaving every other queue untouched. -/
def SubState.updQueue (st : SubState) (q : QueueId) (f : List Registration → List Registration) :
    SubState :=
  { st with queue := fun q' => if q' = q then f (st.queue q') else st.queue q' }

/-- Remove the first registration carrying waiter token `w`.
Returns the remaining list and the removed registration. -/
def removeWaiter : List Registration → WaiterId → Option (List Registration × Registration)
  | [], _ => none
  | r :: l, w =>
      if r.waiter = w then some (l, r)
      else (fun p => (r :: p.1, p.2)) <$> removeWaiter l w

/-- Remove the first `(w, o)` pair from a resolved list (one-shot consumption). -/
def removeResolved : List (WaiterId × Outcome) → WaiterId → Outcome → List (WaiterId × Outcome)
  | [], _, _ => []
  | (w', o') :: xs, w, o =>
      if w' = w then
        if o' = o then xs else (w', o') :: removeResolved xs w o
      else (w', o') :: removeResolved xs w o

/-! ## Substrate operations

Each fiber-executable operation maps 1:1 onto a scheduler entry point. -/

/-- Values returned by substrate operations. -/
inductive SubVal : Type where
  | unit
  | bool (b : Bool)
  | tick (t : Tick)
  | waiter (w : WaiterId)
  | outcome (o : Outcome)
deriving instance DecidableEq for SubVal

inductive SubOp : Type where
  /-- `WaitQueue::register_wait_locked` via `Scheduler::await_wait*`:
  enqueue the calling fiber with an optional deadline; yields the fresh token. -/
  | attach (q : QueueId) (dl : Option Tick)
  /-- The fiber switch inside `Scheduler::await_wait`: block the calling fiber
  on its own registration until the token is resolved; yields the outcome.
  A suspended fiber makes no progress until its token is resolved. -/
  | suspend (w : WaiterId)
  /-- `Scheduler::wake_wait_one_locked`: resolve the queue-head registration
  with `woken`; yields whether anyone was woken. -/
  | wakeOne (q : QueueId)
  /-- `Scheduler::cancel_wait`: resolve the named registration with
  `cancelled`; yields whether the token was live. -/
  | cancelTok (q : QueueId) (w : WaiterId)
  /-- `Scheduler::monotonic_now`. -/
  | nowTick
deriving instance DecidableEq for SubOp

/-- One substrate operation step performed by fiber `me`.  Frozen: later
stages may not add or weaken clauses (BRAKE-1 governs changes). -/
inductive SubStep : FiberId → SubState → SubOp → SubVal → SubState → Prop where
  | attach (st : SubState) (me : FiberId) (q : QueueId) (dl : Option Tick) :
      SubStep me st (SubOp.attach q dl) (SubVal.waiter st.nextWaiter)
        { (st.updQueue q (· ++ [{ waiter := st.nextWaiter, owner := me, deadline := dl }])) with
          nextWaiter := st.nextWaiter + 1 }
  | wakeOne (st : SubState) (me : FiberId) (q : QueueId) (r : Registration)
      (tl : List Registration) :
      st.queue q = r :: tl →
      SubStep me st (SubOp.wakeOne q) (SubVal.bool true)
        { (st.updQueue q (fun _ => tl)) with
          resolved := st.resolved ++ [(r.waiter, Outcome.woken)] }
  | wakeOneEmpty (st : SubState) (me : FiberId) (q : QueueId) :
      st.queue q = [] →
      SubStep me st (SubOp.wakeOne q) (SubVal.bool false) st
  | cancelHit (st : SubState) (me : FiberId) (q : QueueId) (w : WaiterId)
      (l' : List Registration) (r : Registration) :
      removeWaiter (st.queue q) w = some (l', r) →
      SubStep me st (SubOp.cancelTok q w) (SubVal.bool true)
        { (st.updQueue q (fun _ => l')) with
          resolved := st.resolved ++ [(w, Outcome.cancelled)] }
  | cancelMiss (st : SubState) (me : FiberId) (q : QueueId) (w : WaiterId) :
      removeWaiter (st.queue q) w = none →
      SubStep me st (SubOp.cancelTok q w) (SubVal.bool false) st
  | nowTick (st : SubState) (me : FiberId) :
      SubStep me st SubOp.nowTick (SubVal.tick st.now) st

/-- Environment operations owned by the scheduler, not by any fiber. -/
inductive EnvOp : Type where
  /-- Monotonic time moves forward. -/
  | timeAdvances (t : Tick)
  /-- `Scheduler::expire_wait` domain: a timed registration whose deadline
  has passed is resolved with `expired`. -/
  | expire (q : QueueId) (w : WaiterId)

inductive EnvStep : SubState → EnvOp → SubState → Prop where
  | time (st : SubState) (t : Tick) : st.now ≤ t →
      EnvStep st (EnvOp.timeAdvances t) { st with now := t }
  | expire (st : SubState) (q : QueueId) (w : WaiterId) (dl : Tick)
      (l' : List Registration) (r : Registration) :
      removeWaiter (st.queue q) w = some (l', r) →
      r.deadline = some dl → st.now ≥ dl →
      EnvStep st (EnvOp.expire q w)
        { (st.updQueue q (fun _ => l')) with
          resolved := st.resolved ++ [(w, Outcome.expired)] }

/-! ## Programs over the substrate

The frozen implementation language for "a primitive rebuilt from the
substrate": a finite sequence of substrate operations with pure control
flow.  No recursion, no cross-fiber shared state, no external persistent
state. -/

inductive SubProg : Type where
  /-- Finish with a value. -/
  | pure : SubVal → SubProg
  /-- Perform one substrate operation, then continue with its value. -/
  | eff : SubOp → (SubVal → SubProg) → SubProg

/-! ## Observation model

The caller-observable events are *API observations* `Obs (fiber, call,
result)`: a call's *issue* (`result = none`, emitted when the dispatched
fiber enters the call) and its *completion* (`result = some r`, emitted at
the call's physical return to its caller).  Internal events (registrations,
wakes, resolutions, dispatches) are not observable; they become visible only
through the issues and completions they enable.  This mirrors the public
headers: callers see call entry and return values, never scheduler
internals.

Invocation identity is per-fiber alternation: one fiber has at most one call
in flight, and a fiber's calls are strictly sequential, so its k-th issue is
followed by exactly its k-th completion.  No invocation counter is surfaced. -/

/-- A primitive API signature: call and result shapes. -/
structure ApiSig : Type 1 where
  Call : Type
  Result : Type

/-- One caller-observable event. -/
structure Obs (A : ApiSig) : Type where
  fiber : FiberId
  call : A.Call
  result : Option A.Result

abbrev Trace (A : ApiSig) := List (Obs A)

def issueObs (A : ApiSig) (f : FiberId) (c : A.Call) : Obs A :=
  { fiber := f, call := c, result := none }

def compObs (A : ApiSig) (f : FiberId) (c : A.Call) (r : A.Result) : Obs A :=
  { fiber := f, call := c, result := some r }

/-! ## Base-capability composition (MAJOR B)

`BASE(P)` is given to the encoding side as a `BaseOpsSig`: one operation per
base call, a joint private state for the invoked base cores, and the base
cores' frozen execution facets.  A base operation is executed as an abstract
semantic oracle whose transition facets are the adjudicated base core's own
relations (the composition-faithfulness obligation is recorded per stage:
the oracle is exactly the base stage's model, whose model-to-code mapping and
verdict carry its fidelity). -/

structure BaseOpsSig : Type 1 where
  /-- One base operation = one call into an earlier adjudicated primitive. -/
  Op : Type
  /-- The base call's result. -/
  Res : Op → Type
  /-- Joint private state of the base capabilities (each base core's State). -/
  St : Type
  baseInit : St
  /-- Non-blocking execution of a base call by a fiber (the fused admission
  + inline completion of the base core): `some (r, st', woken)` continues
  the caller with `r` and readies the woken parked base calls. -/
  run : (o : Op) → St → Tick → FiberId → Option (Res o × St × List FiberId)
  /-- The base call suspends instead; state effect of parking. -/
  park : (o : Op) → St → FiberId → Option St
  /-- A parked base call completes (its outcome was determined by an earlier
  waker step); readies `woken` chained base calls (one level per step). -/
  resume : (o : Op) → St → FiberId → Option (Res o × St × List FiberId)

/-- The bare substrate: `BASE(P) = {}`. -/
def BaseOpsSig.none : BaseOpsSig where
  Op := Empty
  Res := fun o => Empty.elim o
  St := Unit
  baseInit := ()
  run := fun o => Empty.elim o
  park := fun o => Empty.elim o
  resume := fun o => Empty.elim o

/-- The encoding language under base `O`: substrate operations, base
operations, pure control flow, no recursion. -/
inductive ExtProg (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  /-- Finish with a substrate value. -/
  | pure : SubVal → ExtProg O A
  /-- Perform one substrate operation, then continue with its value. -/
  | eff : SubOp → (SubVal → ExtProg O A) → ExtProg O A
  /-- Invoke one base operation, then continue with its result. -/
  | base : (o : O.Op) → (O.Res o → ExtProg O A) → ExtProg O A

/-- The frozen shape of an implementation of a primitive under base `O`:
one program per API call plus a pure decoder from the program's final
substrate value to the API result. -/
structure Encoding (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  prog : A.Call → ExtProg O A
  decode : A.Call → SubVal → Option A.Result

/-! ## The call-execution system (encoding side)

The machine that executes encodings under the frozen discipline.  The same
discipline structure (submit / FIFO dispatch / run-to-block / physical
return) is instantiated once on the encoding side and once on the primitive
side below; `docs/formal/stage-0-v2-base-calculus.md` freezes the shared
discipline both instantiations obey.

Discipline (C++ counterparts):

  * `submit` -- a caller fiber becomes runnable (`pending_spawn_` /
    `route_runnable_locked` push onto `local_runnable`).  Silent.  Allowed
    only for a freshly minted or retired fiber: one fiber has one in-flight
    call, and a fiber's next call starts only after its previous call
    returned (the `retired` record).
  * `dispatchFresh` / `dispatchResumed` -- the single worker pops the FIFO
    head of the runnable queue (`worker_loop` pops `local_runnable` front).
    A fresh dispatch emits the issue observation (the fiber enters the API
    call); a resumed dispatch (a woken parked fiber) is silent (the fiber
    continues inside `await_wait*`).
  * the dispatched fiber runs without interleaving until it suspends
    (`commit_suspend_locked` + `context_switch`) or physically returns (the
    completion observation).  No other fiber can act in between: the worker
    is busy.
  * resolutions (wake/cancel/expire) publish the woken fiber runnable
    immediately (`publish_wait_winner_locked`), appending it at the tail of
    the runnable queue (FIFO).
  * environment steps (time advance, deadline expiry) run only while no
    fiber is dispatched: the single worker services the timer wheel between
    fibers (`worker_loop` idle path, `pump_deadlines_locked`). -/

/-- A fiber currently executing one API call. -/
structure Running (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  fiber : FiberId
  call : A.Call
  prog : ExtProg O A

/-- Where a parked call waits: on a substrate waiter token, or inside a
base operation. -/
inductive ParkSite (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  /-- Suspended on waiter token `w`; `k` receives the outcome. -/
  | susp : (w : WaiterId) → (SubVal → ExtProg O A) → ParkSite O A
  /-- Parked inside base operation `o`; `k` receives its result. -/
  | baseOp : (o : O.Op) → (O.Res o → ExtProg O A) → ParkSite O A

/-- A suspended call (a parked fiber). -/
structure Parked (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  fiber : FiberId
  call : A.Call
  site : ParkSite O A

/-- A runnable entry: a fresh caller awaiting its first dispatch, or a
resumed parked fiber (silent dispatch, `fresh = false`). -/
structure Ready (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  fiber : FiberId
  call : A.Call
  prog : ExtProg O A
  fresh : Bool

/-- Global configuration of an encoding under execution.  `retired` lists
the fibers whose most recent call returned: a fiber may submit its next call
only when it is freshly minted (`f = nextFiber`) or retired. -/
structure SysCfg (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  st : SubState
  bst : O.St
  cur : Option (Running O A)
  parked : List (Parked O A)
  runq : List (Ready O A)
  retired : List FiberId
  nextFiber : FiberId

/-- Initial encoding configuration (empty substrate, no fibers). -/
def subInit : SubState := ⟨fun _ => [], [], 0, 0⟩

def encInit (O : BaseOpsSig) (A : ApiSig) : SysCfg O A :=
  ⟨subInit, O.baseInit, none, [], [], [], 0⟩

/-- Fiber `f` has a call in flight in `cfg` (running, parked, or queued). -/
def InFlightSys (O : BaseOpsSig) (A : ApiSig) (cfg : SysCfg O A) (f : FiberId) : Prop :=
  (∃ t : Running O A, cfg.cur = some t ∧ t.fiber = f) ∨
  (∃ p ∈ cfg.parked, p.fiber = f) ∨
  (∃ r ∈ cfg.runq, r.fiber = f)

/-- One system step of an encoding.  `step` is the substrate operation
relation in use (the faithful `SubStep`, or a mutant in negative-witness
assets).  Steps are either silent or emit exactly one API observation. -/
inductive SysStep (O : BaseOpsSig) (A : ApiSig) (enc : Encoding O A)
    (step : FiberId → SubState → SubOp → SubVal → SubState → Prop) :
    SysCfg O A → Option (Obs A) → SysCfg O A → Prop where
  /-- An idle fiber's caller submits an API call (silent; queued).  The
  fiber must be freshly minted or retired (its previous call returned); a
  retired fiber leaves the retired list for the duration of the call. -/
  | submit (cfg : SysCfg O A) (f : FiberId) (c : A.Call) :
      f = cfg.nextFiber ∨ f ∈ cfg.retired →
      SysStep O A enc step cfg none
        { st := cfg.st
          bst := cfg.bst
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq ++ [{ fiber := f, call := c, prog := enc.prog c, fresh := true }]
          retired := cfg.retired.erase f
          nextFiber := if f = cfg.nextFiber then cfg.nextFiber + 1 else cfg.nextFiber }
  /-- The worker dispatches a fresh caller: the issue observation is the
  fiber entering the call. -/
  | dispatchFresh (cfg : SysCfg O A) (r : Ready O A) (rest : List (Ready O A)) :
      cfg.cur = none → cfg.runq = r :: rest → r.fresh = true →
      SysStep O A enc step cfg (some (issueObs A r.fiber r.call))
        { st := cfg.st
          bst := cfg.bst
          cur := some { fiber := r.fiber, call := r.call, prog := r.prog }
          parked := cfg.parked
          runq := rest
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The worker dispatches a resumed (woken) parked fiber: silent. -/
  | dispatchResumed (cfg : SysCfg O A) (r : Ready O A) (rest : List (Ready O A)) :
      cfg.cur = none → cfg.runq = r :: rest → r.fresh = false →
      SysStep O A enc step cfg none
        { st := cfg.st
          bst := cfg.bst
          cur := some { fiber := r.fiber, call := r.call, prog := r.prog }
          parked := cfg.parked
          runq := rest
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The running fiber performs a non-blocking substrate operation that
  resolves nothing new. -/
  | subOpStep (cfg : SysCfg O A) (t : Running O A) (o : SubOp) (k : SubVal → ExtProg O A)
      (v : SubVal) (st' : SubState) :
      cfg.cur = some t → t.prog = ExtProg.eff o k →
      step t.fiber cfg.st o v st' →
      (∀ x ∈ st'.resolved, x ∈ cfg.st.resolved) →
      SysStep O A enc step cfg none
        { st := st'
          bst := cfg.bst
          cur := some { t with prog := k v }
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The running fiber's operation resolves waiter token `w` to `ores`;
  the parked fiber owning `w` is published runnable with its continuation. -/
  | subOpWake (cfg : SysCfg O A) (t : Running O A) (o : SubOp) (k : SubVal → ExtProg O A)
      (v : SubVal) (st' : SubState) (w : WaiterId) (ores : Outcome)
      (preP postP : List (Parked O A)) (p : Parked O A) (kw : SubVal → ExtProg O A) :
      cfg.cur = some t → t.prog = ExtProg.eff o k →
      step t.fiber cfg.st o v st' →
      (w, ores) ∉ cfg.st.resolved → (w, ores) ∈ st'.resolved →
      cfg.parked = preP ++ p :: postP → p.site = ParkSite.susp w kw →
      SysStep O A enc step cfg none
        { st := st'
          bst := cfg.bst
          cur := some { t with prog := k v }
          parked := preP ++ postP
          runq := cfg.runq ++
            [{ fiber := p.fiber, call := p.call, prog := kw (SubVal.outcome ores), fresh := false }]
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The running fiber suspends on an unresolved token: it parks and the
  worker becomes idle. -/
  | suspendBlock (cfg : SysCfg O A) (t : Running O A) (w : WaiterId)
      (k : SubVal → ExtProg O A) :
      cfg.cur = some t → t.prog = ExtProg.eff (SubOp.suspend w) k →
      (∀ o : Outcome, (w, o) ∉ cfg.st.resolved) →
      SysStep O A enc step cfg none
        { st := cfg.st
          bst := cfg.bst
          cur := none
          parked := cfg.parked ++
            [{ fiber := t.fiber, call := t.call, site := ParkSite.susp w k }]
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The running fiber suspends on an already-resolved token: the
  admission-time terminal check of the code; it consumes inline. -/
  | suspendConsume (cfg : SysCfg O A) (t : Running O A) (w : WaiterId)
      (k : SubVal → ExtProg O A) (o : Outcome) :
      cfg.cur = some t → t.prog = ExtProg.eff (SubOp.suspend w) k →
      (w, o) ∈ cfg.st.resolved →
      SysStep O A enc step cfg none
        { st := { cfg.st with resolved := removeResolved cfg.st.resolved w o }
          bst := cfg.bst
          cur := some { t with prog := k (SubVal.outcome o) }
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- A base operation executes inline (fused admission + inline completion
  of the base core) and wakes no parked base call. -/
  | baseRunNone (cfg : SysCfg O A) (t : Running O A) (o : O.Op) (k : O.Res o → ExtProg O A)
      (r : O.Res o) (bst' : O.St) :
      cfg.cur = some t → t.prog = ExtProg.base o k →
      O.run o cfg.bst cfg.st.now t.fiber = some (r, bst', []) →
      SysStep O A enc step cfg none
        { st := cfg.st
          bst := bst'
          cur := some { t with prog := k r }
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- A base operation executes inline and wakes one parked base call,
  which is published runnable with its computed continuation (frozen
  one-level rule: chained wakes of base resumes are not readied here). -/
  | baseRunWake1 (cfg : SysCfg O A) (t : Running O A) (o : O.Op) (k : O.Res o → ExtProg O A)
      (r : O.Res o) (bst' : O.St)
      (preP postP : List (Parked O A)) (p : Parked O A)
      (o' : O.Op) (k' : O.Res o' → ExtProg O A) (r' : O.Res o') (bst'' : O.St) :
      cfg.cur = some t → t.prog = ExtProg.base o k →
      O.run o cfg.bst cfg.st.now t.fiber = some (r, bst', [p.fiber]) →
      cfg.parked = preP ++ p :: postP → p.site = ParkSite.baseOp o' k' →
      O.resume o' bst' p.fiber = some (r', bst'', []) →
      SysStep O A enc step cfg none
        { st := cfg.st
          bst := bst''
          cur := some { t with prog := k r }
          parked := preP ++ postP
          runq := cfg.runq ++ [{ fiber := p.fiber, call := p.call, prog := k' r', fresh := false }]
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- A base operation suspends instead of executing: the calling fiber
  parks inside the base call. -/
  | basePark (cfg : SysCfg O A) (t : Running O A) (o : O.Op) (k : O.Res o → ExtProg O A)
      (bst' : O.St) :
      cfg.cur = some t → t.prog = ExtProg.base o k →
      O.run o cfg.bst cfg.st.now t.fiber = none →
      O.park o cfg.bst t.fiber = some bst' →
      SysStep O A enc step cfg none
        { st := cfg.st
          bst := bst'
          cur := none
          parked := cfg.parked ++
            [{ fiber := t.fiber, call := t.call, site := ParkSite.baseOp o k }]
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The running fiber finished its program: the completion observation is
  the call's physical return, and the fiber retires until its next call.
  The only completion-emitting step. -/
  | complete (cfg : SysCfg O A) (t : Running O A) (v : SubVal) (r : A.Result) :
      cfg.cur = some t → t.prog = ExtProg.pure v →
      enc.decode t.call v = some r →
      SysStep O A enc step cfg (some (compObs A t.fiber t.call r))
        { st := cfg.st
          bst := cfg.bst
          cur := none
          parked := cfg.parked
          runq := cfg.runq
          retired := if t.fiber ∈ cfg.retired then cfg.retired else t.fiber :: cfg.retired
          nextFiber := cfg.nextFiber }
  /-- Environment: time advances while the worker is idle. -/
  | envTime (cfg : SysCfg O A) (t : Tick) :
      cfg.cur = none → cfg.st.now ≤ t →
      SysStep O A enc step cfg none
        { st := { cfg.st with now := t }
          bst := cfg.bst
          cur := none
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- Environment: a due timed registration expires while the worker is
  idle; the parked fiber owning the token is published runnable with the
  expired outcome. -/
  | envExpire (cfg : SysCfg O A) (q : QueueId) (w : WaiterId) (st' : SubState)
      (preP postP : List (Parked O A)) (p : Parked O A) (kw : SubVal → ExtProg O A) :
      cfg.cur = none →
      EnvStep cfg.st (EnvOp.expire q w) st' → (w, Outcome.expired) ∈ st'.resolved →
      cfg.parked = preP ++ p :: postP → p.site = ParkSite.susp w kw →
      SysStep O A enc step cfg none
        { st := st'
          bst := cfg.bst
          cur := none
          parked := preP ++ postP
          runq := cfg.runq ++
            [{ fiber := p.fiber, call := p.call,
               prog := kw (SubVal.outcome Outcome.expired), fresh := false }]
          retired := cfg.retired
          nextFiber := cfg.nextFiber }

/-- Finite runs of an encoding: any prefix of any execution (runs may stop at
any time, so trace languages are prefix-closed). -/
inductive SysRuns (O : BaseOpsSig) (A : ApiSig) (enc : Encoding O A)
    (step : FiberId → SubState → SubOp → SubVal → SubState → Prop) :
    SysCfg O A → Trace A → SysCfg O A → Prop where
  | stop (cfg : SysCfg O A) : SysRuns O A enc step cfg [] cfg
  | step (cfg cfg' : SysCfg O A) (o : Option (Obs A)) (t : Trace A) (fin : SysCfg O A) :
      SysStep O A enc step cfg o cfg' → SysRuns O A enc step cfg' t fin →
      SysRuns O A enc step cfg (o.toList ++ t) fin

abbrev EncRuns (O : BaseOpsSig) (A : ApiSig) (enc : Encoding O A) :=
  SysRuns O A enc SubStep

/-- Observable traces of an encoding over the faithful substrate. -/
def TracesEnc (O : BaseOpsSig) (A : ApiSig) (enc : Encoding O A) (t : Trace A) : Prop :=
  ∃ fin, EncRuns O A enc (encInit O A) t fin

/-! ## The call-execution system (primitive side)

The primitive under judgment is an open system with its own private state,
executed under the same discipline.  The LTS facets are the code's decision
points:

  * `admit` -- the entry critical section of a call, applied atomically with
    the issue observation at fresh dispatch.
  * `run` -- the fused inline paths of the dispatched call: `some` completes
    the call at its physical return (state effect + result + fibers woken);
    `none` sends the machine to `park`.
  * `park` -- the suspension point's state effect (register on private
    queues, release a bound mutex, ...).  A resumed call may park again
    (Mesa reacquire in `AsyncCondition::wait`).
  * `finish` -- a resumed parked call's completion at its dispatch (its
    outcome was determined by an earlier waker step).
  * `onTick` -- the clock-mirror update at idle points.
  * `expire` -- the environment's expiry of one parked call's deadline;
    the state records the outcome the later `finish` reports. -/

/-- An in-flight primitive call. -/
structure Pnd (A : ApiSig) : Type where
  fiber : FiberId
  call : A.Call

/-- A runnable primitive call. -/
structure PReady (A : ApiSig) : Type where
  fiber : FiberId
  call : A.Call
  fresh : Bool

structure PrimLTS2 (A : ApiSig) : Type 1 where
  /-- The primitive's private state (its C++ members). -/
  State : Type
  init : State
  /-- Entry critical section at fresh dispatch (atomic with the issue
  observation). -/
  admit : State → FiberId → A.Call → Option State
  /-- Inline completion of the dispatched call: `some (r, s', woken)` emits
  the completion, applies the state effect, and readies `woken` parked
  fibers (in order). -/
  run : State → Tick → FiberId → A.Call → Option (A.Result × State × List FiberId)
  /-- The call suspends instead; state effect of parking. -/
  park : State → FiberId → A.Call → Option State
  /-- A resumed parked call completes at its dispatch. -/
  finish : State → FiberId → A.Call → Option (A.Result × State × List FiberId)
  /-- Clock-mirror update at idle points. -/
  onTick : State → Tick → State
  /-- Environment expiry of one parked call's deadline. -/
  expire : State → Tick → FiberId → Option State

structure PrimCfg (A : ApiSig) (P : PrimLTS2 A) : Type 1 where
  prim : P.State
  now : Tick
  cur : Option (Pnd A × Bool)
  parked : List (Pnd A)
  runq : List (PReady A)
  retired : List FiberId
  nextFiber : FiberId
  /-- `cur`'s boolean: `false` = freshly dispatched (inline paths allowed),
  `true` = resumed parked call (only `finish`/`park` apply).  `retired` is
  the same persistent-identity record as on the encoding side. -/

def primInit (A : ApiSig) (P : PrimLTS2 A) : PrimCfg A P :=
  ⟨P.init, 0, none, [], [], [], 0⟩

/-- Fiber `f` has a call in flight in `cfg`. -/
def InFlightPrim (A : ApiSig) (P : PrimLTS2 A) (cfg : PrimCfg A P) (f : FiberId) : Prop :=
  (∃ d : Pnd A × Bool, cfg.cur = some d ∧ d.1.fiber = f) ∨
  (∃ p ∈ cfg.parked, p.fiber = f) ∨
  (∃ r ∈ cfg.runq, r.fiber = f)

inductive PrimStep2 (A : ApiSig) (P : PrimLTS2 A) :
    PrimCfg A P → Option (Obs A) → PrimCfg A P → Prop where
  | submit (cfg : PrimCfg A P) (f : FiberId) (c : A.Call) :
      f = cfg.nextFiber ∨ f ∈ cfg.retired →
      PrimStep2 A P cfg none
        { prim := cfg.prim
          now := cfg.now
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq ++ [{ fiber := f, call := c, fresh := true }]
          retired := cfg.retired.erase f
          nextFiber := if f = cfg.nextFiber then cfg.nextFiber + 1 else cfg.nextFiber }
  | dispatchFresh (cfg : PrimCfg A P) (r : PReady A) (rest : List (PReady A)) (s' : P.State) :
      cfg.cur = none → cfg.runq = r :: rest → r.fresh = true →
      P.admit cfg.prim r.fiber r.call = some s' →
      PrimStep2 A P cfg (some (issueObs A r.fiber r.call))
        { prim := s'
          now := cfg.now
          cur := some ({ fiber := r.fiber, call := r.call }, false)
          parked := cfg.parked
          runq := rest
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  | dispatchResumed (cfg : PrimCfg A P) (r : PReady A) (rest : List (PReady A)) :
      cfg.cur = none → cfg.runq = r :: rest → r.fresh = false →
      PrimStep2 A P cfg none
        { prim := cfg.prim
          now := cfg.now
          cur := some ({ fiber := r.fiber, call := r.call }, true)
          parked := cfg.parked
          runq := rest
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The dispatched call completes inline: physical return, state effect,
  and wake publication in one step. -/
  | runDone (cfg : PrimCfg A P) (d : Pnd A × Bool)
      (preP postP : List (Pnd A)) (ps : List (Pnd A))
      (r : A.Result) (s' : P.State) (wk : List FiberId) :
      cfg.cur = some d → d.2 = false →
      P.run cfg.prim cfg.now d.1.fiber d.1.call = some (r, s', wk) →
      cfg.parked = preP ++ ps ++ postP →
      ps.map (fun p : Pnd A => p.fiber) = wk →
      PrimStep2 A P cfg (some (compObs A d.1.fiber d.1.call r))
        { prim := s'
          now := cfg.now
          cur := none
          parked := preP ++ postP
          runq := cfg.runq ++ ps.map (fun p : Pnd A => { fiber := p.fiber, call := p.call, fresh := false })
          retired := if d.1.fiber ∈ cfg.retired then cfg.retired else d.1.fiber :: cfg.retired
          nextFiber := cfg.nextFiber }
  /-- The dispatched call suspends instead: park state effect, silent. -/
  | runPark (cfg : PrimCfg A P) (d : Pnd A × Bool) (s' : P.State) :
      cfg.cur = some d →
      P.run cfg.prim cfg.now d.1.fiber d.1.call = none →
      P.park cfg.prim d.1.fiber d.1.call = some s' →
      PrimStep2 A P cfg none
        { prim := s'
          now := cfg.now
          cur := none
          parked := cfg.parked ++ [{ fiber := d.1.fiber, call := d.1.call }]
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- A resumed parked call completes at its dispatch. -/
  | finishDone (cfg : PrimCfg A P) (d : Pnd A × Bool)
      (preP postP : List (Pnd A)) (ps : List (Pnd A))
      (r : A.Result) (s' : P.State) (wk : List FiberId) :
      cfg.cur = some d → d.2 = true →
      P.finish cfg.prim d.1.fiber d.1.call = some (r, s', wk) →
      cfg.parked = preP ++ ps ++ postP →
      ps.map (fun p : Pnd A => p.fiber) = wk →
      PrimStep2 A P cfg (some (compObs A d.1.fiber d.1.call r))
        { prim := s'
          now := cfg.now
          cur := none
          parked := preP ++ postP
          runq := cfg.runq ++ ps.map (fun p : Pnd A => { fiber := p.fiber, call := p.call, fresh := false })
          retired := if d.1.fiber ∈ cfg.retired then cfg.retired else d.1.fiber :: cfg.retired
          nextFiber := cfg.nextFiber }
  /-- Environment: time advances while the worker is idle (clock mirror). -/
  | envTime (cfg : PrimCfg A P) (t : Tick) :
      cfg.cur = none → cfg.now ≤ t →
      PrimStep2 A P cfg none
        { prim := P.onTick cfg.prim t
          now := t
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber }
  /-- Environment: one parked call's deadline expires while the worker is
  idle; the fiber is published runnable and the recorded outcome is
  reported by its later `finish`. -/
  | envExpire (cfg : PrimCfg A P) (f : FiberId) (s' : P.State)
      (preP postP : List (Pnd A)) (p : Pnd A) :
      cfg.cur = none →
      P.expire cfg.prim cfg.now f = some s' →
      cfg.parked = preP ++ p :: postP → p.fiber = f →
      PrimStep2 A P cfg none
        { prim := s'
          now := cfg.now
          cur := cfg.cur
          parked := preP ++ postP
          runq := cfg.runq ++ [{ fiber := p.fiber, call := p.call, fresh := false }]
          retired := cfg.retired
          nextFiber := cfg.nextFiber }

inductive PrimRuns2 (A : ApiSig) (P : PrimLTS2 A) :
    PrimCfg A P → Trace A → PrimCfg A P → Prop where
  | stop (cfg : PrimCfg A P) : PrimRuns2 A P cfg [] cfg
  | step (cfg cfg' : PrimCfg A P) (o : Option (Obs A)) (t : Trace A) (fin : PrimCfg A P) :
      PrimStep2 A P cfg o cfg' → PrimRuns2 A P cfg' t fin →
      PrimRuns2 A P cfg (o.toList ++ t) fin

/-- Observable traces of the primitive itself. -/
def TracesPrim (A : ApiSig) (P : PrimLTS2 A) (t : Trace A) : Prop :=
  ∃ fin, PrimRuns2 A P (primInit A P) t fin

/-! ## Discipline lemmas

Splitting runs at observations; the trace-level facts the per-stage
adjudications use. -/

/-- A run whose trace starts with an observation splits into a silent
prefix, the one observation-emitting step, and the suffix run. -/
theorem sysRuns_cons {O : BaseOpsSig} {A : ApiSig} {enc : Encoding O A}
    {step : FiberId → SubState → SubOp → SubVal → SubState → Prop}
    {cfg : SysCfg O A} {t : Trace A} {fin : SysCfg O A}
    (hrun : SysRuns O A enc step cfg t fin) :
    ∀ (ob : Obs A) (t' : Trace A), t = ob :: t' →
      ∃ m1 m2 : SysCfg O A,
        SysRuns O A enc step cfg [] m1 ∧
        SysStep O A enc step m1 (some ob) m2 ∧
        SysRuns O A enc step m2 t' fin := by
  induction hrun with
  | stop cfg => intro ob t' h; simp at h
  | step cfg cfg' o t2 fin2 hstep hrest ih =>
      intro ob t' h
      cases o with
      | none =>
          obtain ⟨m1, m2, h1, h2, h3⟩ := ih ob t' (by simpa [Option.toList] using h)
          exact ⟨m1, m2, SysRuns.step cfg cfg' none [] m1 hstep h1, h2, h3⟩
      | some ob' =>
          have hEq : ob' :: t2 = ob :: t' := by simpa [Option.toList] using h
          injection hEq with h1 h2
          subst h1; subst h2
          exact ⟨cfg, cfg', SysRuns.stop cfg, hstep, hrest⟩

/-- Same split for the primitive side. -/
theorem primRuns_cons {A : ApiSig} {P : PrimLTS2 A} {cfg : PrimCfg A P}
    {t : Trace A} {fin : PrimCfg A P}
    (hrun : PrimRuns2 A P cfg t fin) :
    ∀ (ob : Obs A) (t' : Trace A), t = ob :: t' →
      ∃ m1 m2 : PrimCfg A P,
        PrimRuns2 A P cfg [] m1 ∧
        PrimStep2 A P m1 (some ob) m2 ∧
        PrimRuns2 A P m2 t' fin := by
  induction hrun with
  | stop cfg => intro ob t' h; simp at h
  | step cfg cfg' o t2 fin2 hstep hrest ih =>
      intro ob t' h
      cases o with
      | none =>
          obtain ⟨m1, m2, h1, h2, h3⟩ := ih ob t' (by simpa [Option.toList] using h)
          exact ⟨m1, m2, PrimRuns2.step cfg cfg' none [] m1 hstep h1, h2, h3⟩
      | some ob' =>
          have hEq : ob' :: t2 = ob :: t' := by simpa [Option.toList] using h
          injection hEq with h1 h2
          subst h1; subst h2
          exact ⟨cfg, cfg', PrimRuns2.stop cfg, hstep, hrest⟩

/-! ## Sequential-call observation discipline (MAJOR C)

The observation language carries the persistent-identity discipline
directly: per fiber, issues and completions strictly alternate, starting
with an issue (a fiber has at most one call in flight, and its next call
starts only after the previous one returned -- the C++ blocking-call
contract, `await_*` + `context_switch`).  Both trace languages below
(encoding side and primitive side) are restricted by the same `SeqOK`
predicate, so the discipline is symmetric by construction and state-checkable
on the trace alone. -/

/-- `m f = true` records that fiber `f` currently has a call in flight. -/
inductive SeqOKFrom (A : ApiSig) : (FiberId → Bool) → Trace A → Prop where
  | nil (m : FiberId → Bool) : SeqOKFrom A m []
  | consIssue (m : FiberId → Bool) (t : Trace A) (f : FiberId) (c : A.Call) :
      m f = false →
      SeqOKFrom A (fun g => if g = f then true else m g) t →
      SeqOKFrom A m (issueObs A f c :: t)
  | consComp (m : FiberId → Bool) (t : Trace A) (f : FiberId) (c : A.Call) (r : A.Result) :
      m f = true →
      SeqOKFrom A (fun g => if g = f then false else m g) t →
      SeqOKFrom A m (compObs A f c r :: t)

/-- A trace obeys the per-fiber sequential-call discipline from the all-idle
state. -/
def SeqOK (A : ApiSig) (t : Trace A) : Prop :=
  SeqOKFrom A (fun _ => false) t

/-- The disciplined encoding trace language. -/
def TracesEncS (O : BaseOpsSig) (A : ApiSig) (enc : Encoding O A) (t : Trace A) : Prop :=
  TracesEnc O A enc t ∧ SeqOK A t

/-- The disciplined primitive trace language. -/
def TracesPrimS (A : ApiSig) (P : PrimLTS2 A) (t : Trace A) : Prop :=
  TracesPrim A P t ∧ SeqOK A t

/-! ## The completion shadow is not valid under the V2 discipline (MAJOR A)

The V1 calculus admitted erasing completion observations from any encoding
trace (the completion-shadow projection), because a `complete` step emitted
an observation and changed nothing else.  Under the V2 discipline the
corresponding statement is *false*: the retired-fiber submit rule and the
`SeqOK` trace discipline forbid the erased trace.  The following is the
formal certificate: an encoding produces a two-call trace of one fiber, but
does not produce its completion-erased shadow, whose second issue would find
the fiber still in flight.  Every V1 THEOREM-B proof rested on the shadow
projection; under V2 the verdicts are rederived without it. -/

/-- Which observations the shadow keeps: every issue, plus the completions
`keep` selects. -/
def keepObs {A : ApiSig} (keep : A.Call → A.Result → Bool) (o : Obs A) : Bool :=
  match o.result with
  | none => true
  | some r => keep o.call r

/-- The completion shadow of a trace. -/
def shadowTrace {A : ApiSig} (keep : A.Call → A.Result → Bool) : Trace A → Trace A
  | [] => []
  | o :: t =>
      if keepObs keep o = true then o :: shadowTrace keep t
      else shadowTrace keep t

/-- A concrete API signature with one call and one result. -/
inductive ProbeCall : Type where
  | go
deriving instance DecidableEq for ProbeCall

inductive ProbeResult : Type where
  | done
deriving instance DecidableEq for ProbeResult

abbrev ProbeSig : ApiSig := ⟨ProbeCall, ProbeResult⟩

/-- The trivial immediately-returning encoding. -/
def probeEnc : Encoding BaseOpsSig.none ProbeSig :=
  { prog := fun _ => ExtProg.pure SubVal.unit
    decode := fun _ _ => some ProbeResult.done }

/-- The two sequential calls of fiber 0, with completions. -/
def probeTrace : Trace ProbeSig :=
  [issueObs ProbeSig 0 ProbeCall.go, compObs ProbeSig 0 ProbeCall.go ProbeResult.done,
    issueObs ProbeSig 0 ProbeCall.go, compObs ProbeSig 0 ProbeCall.go ProbeResult.done]

theorem seqOK_probeTrace : SeqOK ProbeSig probeTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- The keeper erasing every completion. -/
def probeKeep : ProbeCall → ProbeResult → Bool := fun _ _ => false

theorem probeShadow_eq :
    shadowTrace probeKeep probeTrace = [issueObs ProbeSig 0 ProbeCall.go,
      issueObs ProbeSig 0 ProbeCall.go] := by
  rfl

/-- The shadow of `probeTrace` violates the sequential-call discipline: its
second issue finds fiber 0 already in flight. -/
theorem seqOK_not_shadow : ¬ SeqOK ProbeSig (shadowTrace probeKeep probeTrace) := by
  rw [probeShadow_eq]
  intro h
  cases h with
  | consIssue m t f c _ hrest =>
      cases hrest with
      | consIssue _ _ _ _ mf2 _ => simp at mf2

/-! ### The probe run

The six configurations of the probe run, and its step proofs. -/

def pc0 : SysCfg BaseOpsSig.none ProbeSig := encInit BaseOpsSig.none ProbeSig

def pc1 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [], nextFiber := 1 }

def pc2 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := some { fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit },
    parked := [], runq := [], retired := [], nextFiber := 1 }

def pc3 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [0], nextFiber := 1 }

def pc4 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [], nextFiber := 1 }

def pc5 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := some { fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit },
    parked := [], runq := [], retired := [], nextFiber := 1 }

def pc6 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [0], nextFiber := 1 }

theorem s1 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc0 none pc1 :=
  SysStep.submit pc0 0 ProbeCall.go (Or.inl rfl)

theorem s2 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc1 (some (issueObs ProbeSig 0 ProbeCall.go)) pc2 :=
  SysStep.dispatchFresh pc1 _ [] rfl rfl rfl

theorem s3 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc2
    (some (compObs ProbeSig 0 ProbeCall.go ProbeResult.done)) pc3 :=
  SysStep.complete pc2 _ SubVal.unit ProbeResult.done rfl rfl rfl

theorem s4 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc3 none pc4 :=
  SysStep.submit pc3 0 ProbeCall.go (Or.inr (List.Mem.head _))

theorem s5 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc4 (some (issueObs ProbeSig 0 ProbeCall.go)) pc5 :=
  SysStep.dispatchFresh pc4 _ [] rfl rfl rfl

theorem s6 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc5
    (some (compObs ProbeSig 0 ProbeCall.go ProbeResult.done)) pc6 :=
  SysStep.complete pc5 _ SubVal.unit ProbeResult.done rfl rfl rfl

/-- The probe encoding produces the two sequential calls of fiber 0:
submit; dispatch (issue); return (completion); submit again (the fiber is
retired); dispatch; return.  Persistent fiber identity in action. -/
theorem probeEnc_possesses : TracesEnc BaseOpsSig.none ProbeSig probeEnc probeTrace :=
  ⟨pc6, SysRuns.step pc0 pc1 none _ pc6 s1
    (SysRuns.step pc1 pc2 _ _ pc6 s2
      (SysRuns.step pc2 pc3 _ _ pc6 s3
        (SysRuns.step pc3 pc4 none _ pc6 s4
          (SysRuns.step pc4 pc5 _ _ pc6 s5
            (SysRuns.step pc5 pc6 _ [] pc6 s6 (SysRuns.stop pc6))))))⟩

/-- The V2 discipline refutes the completion-shadow principle: the probe
encoding produces a trace whose completion-erased shadow it cannot produce
(the shadow violates the sequential-call discipline). -/
theorem tracesEnc_shadow_false :
    ¬ ∀ (keep : ProbeCall → ProbeResult → Bool) (t : Trace ProbeSig),
        TracesEncS BaseOpsSig.none ProbeSig probeEnc t →
        TracesEncS BaseOpsSig.none ProbeSig probeEnc (shadowTrace keep t) := by
  intro h
  have hfull : TracesEncS BaseOpsSig.none ProbeSig probeEnc probeTrace :=
    ⟨probeEnc_possesses, seqOK_probeTrace⟩
  exact seqOK_not_shadow (h probeKeep probeTrace hfull).2

end Sluice.Formal
