/-
Sluice Stage-0-V2.3 base calculus (FCB1-METHOD-CORRECTIVE-1).

V2 replaced the Stage-0 calculus after the method-corrective review.  The
correctives it implements:

  MAJOR A (symmetric run-to-block/run-to-return discipline).  The encoding
  side and the primitive side run under the *same* call-execution
  discipline: a submitted call is dispatched FIFO from a single runnable
  queue, then runs without fiber interleaving until it suspends or
  physically returns.  A completion is emitted exactly at the running
  call's return step.  The V1 completion-shadow projection is *not* valid
  under this discipline, and `tracesEnc_shadow_false` below proves its
  failure formally.

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
  `docs/formal/stage-0-v2-base-calculus.md`, is the whole Stage-0 freeze.
  Later stages may not change it without BRAKE-1 and downstream invalidation.

  V2.2 (execution domains / call origin).  The code distinguishes
  fiber-bound calls (`await_event_wait`: `g_worker`, `ws->current`, may
  suspend) from external-capable synchronous calls
  (`Scheduler::event_set_broadcast`, `event_reset`: `global_mtx_` only,
  no `g_worker` -- executable by an OS thread outside the scheduler).  The
  calculus now carries both execution domains:

  * a caller is a `Caller`: a scheduler `Fiber` or an external thread id;
    observations record the caller, never a faked fiber;
  * fiber-origin calls keep the V2 discipline verbatim (submit / FIFO
    dispatch / run-to-block / physical return);
  * an external-capable call executes in three phases, mirroring the code's
    shape (entry → critical section → return): the entry step (`extApply`
    primitive-side, `extStart` encoding-side) emits the issue observation
    and registers an in-flight record with no result; the critical section
    (`extEffect` primitive-side, `extSubOpStep`/`extSubOpWake` encoding-side)
    applies the state effect silently and fixes the result; the return
    (`extDone` / `extComplete`) emits the completion.  Entry precedes
    `global_mtx_` acquisition in the code, so entry order may differ from
    critical-section order -- the phases are therefore independent steps;
  * an external call's physical return is unordered with respect to other
    steps: holding `global_mtx_` serializes state effects, not physical
    returns.  The V1 completion-shadow defect was exactly this confusion,
    so the completion of an external call is deliberately unordered, and
    the issue is deliberately unfused with the effect.

BRAKE-1 ledger (see the freeze document, §7.1):

  v2.1 -- Stage 1V2 validation found `subOpStep`'s original freshness
  premise (no new resolutions at all) incapable of executing the code's
  inline-resolve behavior: a fiber may resolve a registration no parked
  fiber is suspended on (the already-set branch of
  `event_wait_admit_locked` resolves the caller's own node and returns
  without suspending).  The premise now forbids only resolutions that a
  parked fiber is waiting on; those still go through `subOpWake`.
  Downstream invalidation: none -- no downstream verdicts existed; the
  calc-internal certificates and the vacuity reduction were re-verified
  by the gate.

  v2.2 -- the calculus treated every API call as a scheduler/fiber
  submission.  The production code contradicts this: `Event::set`/`reset`
  (and later Semaphore's `release`/`try_acquire`/`cancel`) enter scheduler
  state under `global_mtx_` without any fiber context, so an external OS
  thread can issue them while scheduler fibers are queued or running.
  Amendment: `Caller`-identified observations, `PrimLTS2.extRun` (per-call
  external domain: the call's single fused critical section, run off the
  scheduler), `Encoding.extCap` (the encoding side declares the same
  domains), the `exts` in-flight records on both configurations, and the
  external step rules on both machines -- a three-phase sequence: entry
  (issue observation, `extApply`/`extStart`), critical section (silent
  state effect fixing the result, `extEffect`/`extSubOpStep`+`extSubOpWake`),
  physical return (completion observation, `extDone`/`extComplete`).  Entry
  precedes mutex acquisition in the code, so the issue is never fused with
  the effect: two external callers may enter in one order and run their
  critical sections in the other.  External callers may execute only
  substrate operations that never register or suspend (`extOpAllowed`);
  external invocation of base operations is deferred (no rule; see the
  freeze document's open assumptions).  Downstream invalidation: the
  Stage 1V2 Event verdict (PR #377) rested on "set/reset must enter the
  scheduler FIFO", which V2.2 removes; the Event stage is re-adjudicated
  on V2.2.

  v2.2.1 -- `extSubOpStep`/`extSubOpWake` left the external in-flight
  record untouched (`exts := cfg.exts`), so an external program's
  continuation was computed and discarded: any external-capable call whose
  program contains at least one substrate operation could never reach
  `pure` and never physically return.  This contradicted the freeze
  document's rule 9 ("the encoding side executes its program stepwise") --
  an implementation slip, found during the Stage 1V2.2 Event
  re-adjudication while proving the natural chained encoding's external
  blockade.  Amendment: both steps now advance the record
  (`exts := preE ++ { e with prog := k v } :: postE`).  Downstream
  invalidation: none -- no merged certificate exercised a multi-step
  external program (the domain battery and the vacuity lie use `pure`
  external programs); the calc gate was re-verified in full.

  v2.3 -- the fiber-origin inline call fused its critical-section effect,
  wake publication, and physical return into one step (`runDone`).  The
  production code contradicts this for every fiber-origin call, exactly as
  v2.2's external callers did: the critical section ends when the API's
  internal lock (`global_mtx_`) is released -- the `LockGuard` destructor
  at the end of e.g. `Scheduler::sem_release` -- and the fiber still
  executes its return path afterward, with the worker baton in hand.  An
  external caller's whole call can serialize in that window.  Concrete
  witness (semaphore, `available = 0`, `max = 1`): a fiber `release`
  stores the permit and unlocks; an external `release` then enters, sees
  the full ceiling, refuses, and physically returns `false` BEFORE the
  fiber physically returns `true` -- a legal C++ trace `runDone` cannot
  express, since it forces the fiber's completion to coincide with its
  effect (model under-production).  This is the same effect/return
  confusion the V2 repair (completion shadow) and the v2.2 three-phase
  external split addressed, surviving in the last fused fiber step.
  Amendment: `PrimCfg.cur` becomes a `FSlot` -- `running` (the call's
  inline paths are pending) or `returning` (result fixed, state effect
  applied, wakes published; physical return pending) -- and `runDone`
  splits into `fiberEffect` (silent) and `fiberDone` (the completion
  observation; the fiber retires).  Between them the worker keeps the
  baton: dispatch, the park/finish paths, and the environment steps stay
  blocked (`cur ≠ none`), while the external steps and the return itself
  remain legal.  The encoding side already ran the split discipline
  (substrate-operation steps vs `complete`), so the two machines are
  granularly symmetric again.  Downstream invalidation: the Event (PR
  #377) and Semaphore (PR #378) trace languages, batteries, and carried
  invariants are re-derived on V2.3 and their verdicts re-adjudicated;
  the echo reduction and the domain batteries were re-verified by the
  gate.

Every construct carries a comment naming its C++ counterpart where one exists.
-/

namespace Sluice.Formal

/-! ## Code-level identities

`FiberId`    -- `Fiber*` identity of a coroutine; persistent across the
                calls the fiber issues (`ws->current`, `WaitResume::fiber(me)`).
`ExternalId` -- an external OS-thread caller (no `Fiber*`; the code reads
                no `g_worker` on the external-capable paths).
`Tick`       -- `deadline_tick_t` (`Scheduler::deadline_t`).
`QueueId`    -- a distinct `WaitQueue&`.
`WaiterId`   -- a distinct `WaitNode&` registration token. -/

abbrev FiberId := Nat
abbrev ExternalId := Nat
abbrev Tick := Nat
abbrev QueueId := Nat
abbrev WaiterId := Nat

/-- The execution domain of a caller: a scheduler fiber or an external
thread.  External threads are never faked as fibers. -/
inductive Caller : Type where
  /-- A scheduler coroutine (`Fiber*`). -/
  | fiber : FiberId → Caller
  /-- An external synchronous caller (an OS thread holding no fiber). -/
  | ext : ExternalId → Caller
deriving instance DecidableEq for Caller

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

/-- The substrate operations an *external* caller can execute: exactly the
entry points that take only `global_mtx_` and never register or suspend the
caller (`Scheduler::wake_wait_one_locked` behind `event_set_broadcast`,
`Scheduler::cancel_wait`, `Scheduler::monotonic_now`).  `attach` and
`suspend` live inside `await_wait*`, which reads `g_worker` and switches
fibers -- impossible off a fiber. -/
def extOpAllowed : SubOp → Bool :=
  fun o =>
    match o with
    | SubOp.attach _ _ => false
    | SubOp.suspend _ => false
    | _ => true

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

The caller-observable events are *API observations* `Obs (caller, call,
result)`: a call's *issue* (`result = none`, emitted when the call enters
execution) and its *completion* (`result = some r`, emitted at the call's
physical return to its caller).  The caller is a `Caller` -- a scheduler
fiber or an external thread; external threads are never faked as fibers.
Internal events (registrations, wakes, resolutions, dispatches) are not
observable; they become visible only through the issues and completions
they enable.  This mirrors the public headers: callers see call entry and
return values, never scheduler internals.

Invocation identity is per-caller alternation: one caller has at most one
call in flight, and its calls are strictly sequential, so its k-th issue is
followed by exactly its k-th completion.  For fibers this is the blocking
`await_*` + `context_switch` contract; an external caller's sequencing is
its own thread's program order.  No invocation counter is surfaced. -/

/-- A primitive API signature: call and result shapes. -/
structure ApiSig : Type 1 where
  Call : Type
  Result : Type

/-- One caller-observable event. -/
structure Obs (A : ApiSig) : Type where
  caller : Caller
  call : A.Call
  result : Option A.Result

abbrev Trace (A : ApiSig) := List (Obs A)

def issueObs (A : ApiSig) (cl : Caller) (c : A.Call) : Obs A :=
  { caller := cl, call := c, result := none }

def compObs (A : ApiSig) (cl : Caller) (c : A.Call) (r : A.Result) : Obs A :=
  { caller := cl, call := c, result := some r }

/-! ## Base-capability composition (MAJOR B)

`BASE(P)` is given to the encoding side as a `BaseOpsSig`: one operation per
base call, a joint private state for the invoked base cores, and the base
cores' frozen execution facets.  A base operation is executed as an abstract
oracle whose transition facets are the adjudicated base core's own
relations (the composition-faithfulness obligation is recorded per stage:
the oracle is exactly the base stage's model, whose model-to-code mapping
and verdict carry its fidelity). -/

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
substrate value to the API result, and the call-domain declaration
`extCap`: the calls an external thread can issue against this
implementation.  The declaration must match the primitive's own domains
(a lying declaration surfaces as over-production; see the vacuity
certificate). -/
structure Encoding (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  prog : A.Call → ExtProg O A
  decode : A.Call → SubVal → Option A.Result
  /-- `extCap c = true`: an external caller may issue `c` against this
  implementation; it then runs `prog c` stepwise outside the FIFO with no
  fiber identity, using only `extOpAllowed` substrate operations. -/
  extCap : A.Call → Bool

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
  * the dispatched fiber runs without fiber interleaving until it suspends
    (`commit_suspend_locked` + `context_switch`), until its critical section
    ends (the state effect and wake publication; the API's internal lock is
    released), or until it physically returns (the completion observation).
    After the critical section ends and before the physical return the
    fiber holds the worker baton: no other fiber can act (the worker is
    busy) and the environment does not step; external callers are not
    fibers and are not ordered by this baton, so their steps interleave in
    that window, exactly as after their own critical sections.
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

/-- An external caller with a call in flight against the encoding: the
caller entered the call (the issue observation is emitted at entry) and is
executing its program stepwise on its own thread. -/
structure ExtBusy (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  x : ExternalId
  call : A.Call
  prog : ExtProg O A

/-- Global configuration of an encoding under execution.  `retired` lists
the fibers whose most recent call returned: a fiber may submit its next call
only when it is freshly minted (`f = nextFiber`) or retired.  `exts` lists
the external calls in flight (at most one per external caller). -/
structure SysCfg (O : BaseOpsSig) (A : ApiSig) : Type 1 where
  st : SubState
  bst : O.St
  cur : Option (Running O A)
  parked : List (Parked O A)
  runq : List (Ready O A)
  retired : List FiberId
  nextFiber : FiberId
  exts : List (ExtBusy O A)

/-- Initial encoding configuration (empty substrate, no fibers). -/
def subInit : SubState := ⟨fun _ => [], [], 0, 0⟩

def encInit (O : BaseOpsSig) (A : ApiSig) : SysCfg O A :=
  ⟨subInit, O.baseInit, none, [], [], [], 0, []⟩

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
          nextFiber := if f = cfg.nextFiber then cfg.nextFiber + 1 else cfg.nextFiber
          exts := cfg.exts }
  /-- The worker dispatches a fresh caller: the issue observation is the
  fiber entering the call. -/
  | dispatchFresh (cfg : SysCfg O A) (r : Ready O A) (rest : List (Ready O A)) :
      cfg.cur = none → cfg.runq = r :: rest → r.fresh = true →
      SysStep O A enc step cfg (some (issueObs A (Caller.fiber r.fiber) r.call))
        { st := cfg.st
          bst := cfg.bst
          cur := some { fiber := r.fiber, call := r.call, prog := r.prog }
          parked := cfg.parked
          runq := rest
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- The running fiber performs a non-blocking substrate operation.  A
  step whose operation resolves new waiter tokens is executable here only
  when no parked fiber is suspended on any newly resolved token (such
  resolutions record the outcome for the token's own `WaitNode`, exactly
  like the code's inline-resolve paths, e.g. the already-set branch of
  `event_wait_admit_locked`); resolving a token a parked fiber is suspended
  on goes through `subOpWake` and publishes it. -/
  | subOpStep (cfg : SysCfg O A) (t : Running O A) (o : SubOp) (k : SubVal → ExtProg O A)
      (v : SubVal) (st' : SubState) :
      cfg.cur = some t → t.prog = ExtProg.eff o k →
      step t.fiber cfg.st o v st' →
      (∀ x ∈ st'.resolved, x ∈ cfg.st.resolved ∨
        ∀ p ∈ cfg.parked, ∀ kw, p.site ≠ ParkSite.susp x.1 kw) →
      SysStep O A enc step cfg none
        { st := st'
          bst := cfg.bst
          cur := some { t with prog := k v }
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- The running fiber finished its program: the completion observation is
  the call's physical return, and the fiber retires until its next call.
  The only fiber completion-emitting step. -/
  | complete (cfg : SysCfg O A) (t : Running O A) (v : SubVal) (r : A.Result) :
      cfg.cur = some t → t.prog = ExtProg.pure v →
      enc.decode t.call v = some r →
      SysStep O A enc step cfg (some (compObs A (Caller.fiber t.fiber) t.call r))
        { st := cfg.st
          bst := cfg.bst
          cur := none
          parked := cfg.parked
          runq := cfg.runq
          retired := if t.fiber ∈ cfg.retired then cfg.retired else t.fiber :: cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- An external caller enters an external-capable call: the issue
  observation is emitted at entry and the caller executes its program
  stepwise on its own thread.  It never enters the runnable FIFO and never
  waits for the worker: the code's external-capable paths take only
  `global_mtx_` (`Scheduler::event_set_broadcast`, `event_reset`), so the
  call may begin while fibers are queued or while a fiber is between its
  critical sections. -/
  | extStart (cfg : SysCfg O A) (x : ExternalId) (c : A.Call) :
      enc.extCap c = true →
      x ∉ cfg.exts.map (fun e : ExtBusy O A => e.x) →
      SysStep O A enc step cfg (some (issueObs A (Caller.ext x) c))
        { st := cfg.st
          bst := cfg.bst
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts ++ [{ x := x, call := c, prog := enc.prog c }] }
  /-- An external caller performs a non-blocking substrate operation.
  Only `extOpAllowed` operations are executable (`attach`/`suspend` live in
  `await_wait*`, which requires `g_worker`); the operation's substrate rule
  is fiber-independent by `extOpAllowed`, so the derivation is required for
  every fiber id.  The same no-new-parked-resolution premise as `subOpStep`
  applies.  The external caller holds no worker baton: the step is allowed
  regardless of `cfg.cur`. -/
  | extSubOpStep (cfg : SysCfg O A) (preE postE : List (ExtBusy O A)) (e : ExtBusy O A)
      (o : SubOp) (k : SubVal → ExtProg O A) (v : SubVal) (st' : SubState) :
      cfg.exts = preE ++ e :: postE → e.prog = ExtProg.eff o k →
      extOpAllowed o = true →
      (∀ f, step f cfg.st o v st') →
      (∀ x ∈ st'.resolved, x ∈ cfg.st.resolved ∨
        ∀ p ∈ cfg.parked, ∀ kw, p.site ≠ ParkSite.susp x.1 kw) →
      SysStep O A enc step cfg none
        { st := st'
          bst := cfg.bst
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := preE ++ { e with prog := k v } :: postE }
  /-- An external caller's operation resolves waiter token `w` to `ores`;
  the parked fiber owning `w` is published runnable with its continuation
  (the drain path of `event_set_broadcast`). -/
  | extSubOpWake (cfg : SysCfg O A) (preE postE : List (ExtBusy O A)) (e : ExtBusy O A)
      (o : SubOp) (k : SubVal → ExtProg O A) (v : SubVal) (st' : SubState)
      (w : WaiterId) (ores : Outcome)
      (preP postP : List (Parked O A)) (p : Parked O A) (kw : SubVal → ExtProg O A) :
      cfg.exts = preE ++ e :: postE → e.prog = ExtProg.eff o k →
      extOpAllowed o = true →
      (∀ f, step f cfg.st o v st') →
      (w, ores) ∉ cfg.st.resolved → (w, ores) ∈ st'.resolved →
      cfg.parked = preP ++ p :: postP → p.site = ParkSite.susp w kw →
      SysStep O A enc step cfg none
        { st := st'
          bst := cfg.bst
          cur := cfg.cur
          parked := preP ++ postP
          runq := cfg.runq ++
            [{ fiber := p.fiber, call := p.call, prog := kw (SubVal.outcome ores), fresh := false }]
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := preE ++ { e with prog := k v } :: postE }
  /-- An external call physically returns: the completion observation.
  This is deliberately unordered with respect to the other steps -- the
  caller's critical section has ended, and holding `global_mtx_` serializes
  state effects, not physical returns (another caller's critical section
  may serialize between this call's effect and its return). -/
  | extComplete (cfg : SysCfg O A) (preE postE : List (ExtBusy O A)) (e : ExtBusy O A)
      (v : SubVal) (r : A.Result) :
      cfg.exts = preE ++ e :: postE → e.prog = ExtProg.pure v →
      enc.decode e.call v = some r →
      SysStep O A enc step cfg (some (compObs A (Caller.ext e.x) e.call r))
        { st := cfg.st
          bst := cfg.bst
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := preE ++ postE }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }

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

  * `admit` -- the entry critical section of a fiber call, applied atomically
    with the issue observation at fresh dispatch.
  * `run` -- the fused inline paths of the dispatched call: `some (r, s', woken)`
    ends the call's critical section -- it fixes the result, applies the
    state effect, and readies `woken` parked fibers (in order); the call's
    physical return is the later, separate `fiberDone` step.  `none` sends
    the machine to `park`.
  * `park` -- the suspension point's state effect (register on private
    queues, release a bound mutex, ...).  A resumed call may park again
    (Mesa reacquire in `AsyncCondition::wait`).
  * `finish` -- a resumed parked call's completion at its dispatch (its
    outcome was determined by an earlier waker step).
  * `extRun` -- the *external* execution of a call: its single fused
    critical section under `global_mtx_`, run by an external thread with no
    fiber context (`Scheduler::event_set_broadcast`, `event_reset`,
    `sem_release`).  `some (r, s', woken)` fixes the result, applies the
    state effect, and readies the woken parked fibers, atomically with the
    issue observation; `none` marks the call fiber-only.
  * `onTick` -- the clock-mirror update at idle points.
  * `expire` -- the environment's expiry of one parked call's deadline;
    the state records the outcome the later `finish` reports. -/

/-- An in-flight primitive call (a scheduler fiber). -/
structure Pnd (A : ApiSig) : Type where
  fiber : FiberId
  call : A.Call

/-- The dispatched fiber call's slot in `cur`.  `running d b`: the call is
executing its inline paths (`run`/`park`/`finish` still apply); `b = false`
is a fresh dispatch, `b = true` a resumed parked call.  `returning d r`:
the call's critical section has ended -- the result `r` is fixed, the state
effect is applied, and the wakes are published -- but the fiber has not
physically returned; the worker keeps the baton and only the external steps
and the return itself remain legal (V2.3: `global_mtx_` serialization is
not physical-return serialization, for fiber callers too). -/
inductive FSlot (A : ApiSig) : Type where
  | running : Pnd A → Bool → FSlot A
  | returning : Pnd A → A.Result → FSlot A

/-- The fiber of a slot. -/
def FSlot.fiber {A : ApiSig} : FSlot A → FiberId
  | FSlot.running d _ => d.fiber
  | FSlot.returning d _ => d.fiber

/-- The call of a slot. -/
def FSlot.call {A : ApiSig} : FSlot A → A.Call
  | FSlot.running d _ => d.call
  | FSlot.returning d _ => d.call

/-- A runnable primitive call. -/
structure PReady (A : ApiSig) : Type where
  fiber : FiberId
  call : A.Call
  fresh : Bool

/-- An external call in flight against the primitive.  `result = none`:
entered, its critical section has not run.  `result = some r`: the critical
section ran and fixed the result; the physical return is pending. -/
structure ExtPend (A : ApiSig) : Type where
  x : ExternalId
  call : A.Call
  result : Option A.Result

structure PrimLTS2 (A : ApiSig) : Type 1 where
  /-- The primitive's private state (its C++ members). -/
  State : Type
  init : State
  /-- Entry critical section at fresh fiber dispatch (atomic with the issue
  observation). -/
  admit : State → FiberId → A.Call → Option State
  /-- Inline execution of the dispatched fiber call: `some (r, s', woken)`
  ends the call's critical section -- fixes the result, applies the state
  effect, and readies `woken` parked fibers (in order); the physical return
  is the later `fiberDone` step. -/
  run : State → Tick → FiberId → A.Call → Option (A.Result × State × List FiberId)
  /-- The call suspends instead; state effect of parking. -/
  park : State → FiberId → A.Call → Option State
  /-- A resumed parked call completes at its dispatch. -/
  finish : State → FiberId → A.Call → Option (A.Result × State × List FiberId)
  /-- The external call domains: `extCap c = true` iff `c` can be issued by
  an external thread (a `global_mtx_`-only entry point with no `g_worker`
  read).  Must agree with `extRun`: `extRun c = none` whenever
  `extCap c = false` (the per-stage card records the census). -/
  extCap : A.Call → Bool
  /-- External synchronous execution of call `c`: the call's single fused
  critical section, run off the scheduler by an external thread.  `none` =
  the call has no external critical section (it is fiber-only). -/
  extRun : A.Call → State → Tick → Option (A.Result × State × List FiberId)
  /-- Clock-mirror update at idle points. -/
  onTick : State → Tick → State
  /-- Environment expiry of one parked call's deadline. -/
  expire : State → Tick → FiberId → Option State

structure PrimCfg (A : ApiSig) (P : PrimLTS2 A) : Type 1 where
  prim : P.State
  now : Tick
  cur : Option (FSlot A)
  parked : List (Pnd A)
  runq : List (PReady A)
  retired : List FiberId
  nextFiber : FiberId
  /-- External calls in flight (at most one per external caller). -/
  exts : List (ExtPend A)

def primInit (A : ApiSig) (P : PrimLTS2 A) : PrimCfg A P :=
  ⟨P.init, 0, none, [], [], [], 0, []⟩

/-- Fiber `f` has a call in flight in `cfg`. -/
def InFlightPrim (A : ApiSig) (P : PrimLTS2 A) (cfg : PrimCfg A P) (f : FiberId) : Prop :=
  (∃ d b, cfg.cur = some (FSlot.running d b) ∧ d.fiber = f) ∨
  (∃ d r, cfg.cur = some (FSlot.returning d r) ∧ d.fiber = f) ∨
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
          nextFiber := if f = cfg.nextFiber then cfg.nextFiber + 1 else cfg.nextFiber
          exts := cfg.exts }
  | dispatchFresh (cfg : PrimCfg A P) (r : PReady A) (rest : List (PReady A)) (s' : P.State) :
      cfg.cur = none → cfg.runq = r :: rest → r.fresh = true →
      P.admit cfg.prim r.fiber r.call = some s' →
      PrimStep2 A P cfg (some (issueObs A (Caller.fiber r.fiber) r.call))
        { prim := s'
          now := cfg.now
          cur := some (FSlot.running { fiber := r.fiber, call := r.call } false)
          parked := cfg.parked
          runq := rest
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  | dispatchResumed (cfg : PrimCfg A P) (r : PReady A) (rest : List (PReady A)) :
      cfg.cur = none → cfg.runq = r :: rest → r.fresh = false →
      PrimStep2 A P cfg none
        { prim := cfg.prim
          now := cfg.now
          cur := some (FSlot.running { fiber := r.fiber, call := r.call } true)
          parked := cfg.parked
          runq := rest
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- The dispatched fiber call's critical section ends: the fused `run`
  facet fixes the result, applies the state effect, and readies the woken
  parked fibers.  Silent; the physical return is the later `fiberDone`
  (V2.3 split: the API's internal lock is released here, but the fiber
  still owns the worker baton and executes its return path afterward). -/
  | fiberEffect (cfg : PrimCfg A P) (d : Pnd A) (b : Bool)
      (preP postP : List (Pnd A)) (ps : List (Pnd A))
      (r : A.Result) (s' : P.State) (wk : List FiberId) :
      cfg.cur = some (FSlot.running d b) → b = false →
      P.run cfg.prim cfg.now d.fiber d.call = some (r, s', wk) →
      cfg.parked = preP ++ ps ++ postP →
      ps.map (fun p : Pnd A => p.fiber) = wk →
      PrimStep2 A P cfg none
        { prim := s'
          now := cfg.now
          cur := some (FSlot.returning d r)
          parked := preP ++ postP
          runq := cfg.runq ++ ps.map (fun p : Pnd A => { fiber := p.fiber, call := p.call, fresh := false })
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- The returning fiber call physically returns: the completion
  observation, and the fiber retires until its next call.  Unordered with
  respect to the external steps -- between the critical section
  (`fiberEffect`) and the return, an external caller's whole call may
  serialize, exactly as in the code. -/
  | fiberDone (cfg : PrimCfg A P) (d : Pnd A) (r : A.Result) :
      cfg.cur = some (FSlot.returning d r) →
      PrimStep2 A P cfg (some (compObs A (Caller.fiber d.fiber) d.call r))
        { prim := cfg.prim
          now := cfg.now
          cur := none
          parked := cfg.parked
          runq := cfg.runq
          retired := if d.fiber ∈ cfg.retired then cfg.retired else d.fiber :: cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- The dispatched fiber call suspends instead: park state effect, silent. -/
  | runPark (cfg : PrimCfg A P) (d : Pnd A) (b : Bool) (s' : P.State) :
      cfg.cur = some (FSlot.running d b) →
      P.run cfg.prim cfg.now d.fiber d.call = none →
      P.park cfg.prim d.fiber d.call = some s' →
      PrimStep2 A P cfg none
        { prim := s'
          now := cfg.now
          cur := none
          parked := cfg.parked ++ [{ fiber := d.fiber, call := d.call }]
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- A resumed parked call completes at its dispatch (its inline paths are
  empty: the outcome was fixed and the state effects were applied by the
  earlier waker step, so effect and return do not come apart here). -/
  | finishDone (cfg : PrimCfg A P) (d : Pnd A) (b : Bool)
      (preP postP : List (Pnd A)) (ps : List (Pnd A))
      (r : A.Result) (s' : P.State) (wk : List FiberId) :
      cfg.cur = some (FSlot.running d b) → b = true →
      P.finish cfg.prim d.fiber d.call = some (r, s', wk) →
      cfg.parked = preP ++ ps ++ postP →
      ps.map (fun p : Pnd A => p.fiber) = wk →
      PrimStep2 A P cfg (some (compObs A (Caller.fiber d.fiber) d.call r))
        { prim := s'
          now := cfg.now
          cur := none
          parked := preP ++ postP
          runq := cfg.runq ++ ps.map (fun p : Pnd A => { fiber := p.fiber, call := p.call, fresh := false })
          retired := if d.fiber ∈ cfg.retired then cfg.retired else d.fiber :: cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
  /-- An external caller *enters* a call: the issue observation is emitted
  at entry and a result-less record is registered.  Entry precedes
  `global_mtx_` acquisition in the code (`scheduler_event.cpp`: function
  body, then `LockGuard`), so the step is independent of the worker and the
  runnable FIFO, and the critical section is a *later*, separate step. -/
  | extApply (cfg : PrimCfg A P) (x : ExternalId) (c : A.Call)
      (preE postE : List (ExtPend A)) :
      P.extCap c = true →
      x ∉ cfg.exts.map (fun e : ExtPend A => e.x) →
      PrimStep2 A P cfg (some (issueObs A (Caller.ext x) c))
        { prim := cfg.prim
          now := cfg.now
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := cfg.exts ++ [{ x := x, call := c, result := none }] }
  /-- An external call's *critical section* runs: the fused `extRun` facet
  fixes the result, applies the state effect, and readies the woken parked
  fibers exactly as in `fiberEffect`.  Silent; the entry must be present and
  not yet applied. -/
  | extEffect (cfg : PrimCfg A P) (preE postE : List (ExtPend A)) (e : ExtPend A)
      (r : A.Result) (s' : P.State) (wk : List FiberId)
      (preP postP : List (Pnd A)) (ps : List (Pnd A)) :
      cfg.exts = preE ++ e :: postE → e.result = none →
      P.extRun e.call cfg.prim cfg.now = some (r, s', wk) →
      cfg.parked = preP ++ ps ++ postP →
      ps.map (fun p : Pnd A => p.fiber) = wk →
      PrimStep2 A P cfg none
        { prim := s'
          now := cfg.now
          cur := cfg.cur
          parked := preP ++ postP
          runq := cfg.runq ++ ps.map (fun p : Pnd A => { fiber := p.fiber, call := p.call, fresh := false })
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := preE ++ { e with result := some r } :: postE }
  /-- An external call physically returns: the completion observation.
  Deliberately unordered with respect to the other steps: the caller's
  critical section has ended, and `global_mtx_` serialization is not
  physical-return serialization (another caller's critical section may
  serialize between this call's effect and its return). -/
  | extDone (cfg : PrimCfg A P) (preE postE : List (ExtPend A)) (e : ExtPend A) (r : A.Result) :
      cfg.exts = preE ++ e :: postE → e.result = some r →
      PrimStep2 A P cfg (some (compObs A (Caller.ext e.x) e.call r))
        { prim := cfg.prim
          now := cfg.now
          cur := cfg.cur
          parked := cfg.parked
          runq := cfg.runq
          retired := cfg.retired
          nextFiber := cfg.nextFiber
          exts := preE ++ postE }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }
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
          nextFiber := cfg.nextFiber
          exts := cfg.exts }

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
directly: per caller -- fiber or external thread -- issues and completions
strictly alternate, starting with an issue (a caller has at most one call
in flight, and its next call starts only after the previous one returned;
for fibers this is the C++ blocking-call contract, `await_*` +
`context_switch`, for external callers it is the calling thread's program
order).  Both trace languages below (encoding side and primitive side) are
restricted by the same `SeqOK` predicate, so the discipline is symmetric by
construction and state-checkable on the trace alone. -/

/-- `m cl = true` records that caller `cl` currently has a call in flight. -/
inductive SeqOKFrom (A : ApiSig) : (Caller → Bool) → Trace A → Prop where
  | nil (m : Caller → Bool) : SeqOKFrom A m []
  | consIssue (m : Caller → Bool) (t : Trace A) (cl : Caller) (c : A.Call) :
      m cl = false →
      SeqOKFrom A (fun g => if g = cl then true else m g) t →
      SeqOKFrom A m (issueObs A cl c :: t)
  | consComp (m : Caller → Bool) (t : Trace A) (cl : Caller) (c : A.Call) (r : A.Result) :
      m cl = true →
      SeqOKFrom A (fun g => if g = cl then false else m g) t →
      SeqOKFrom A m (compObs A cl c r :: t)

/-- A trace obeys the per-caller sequential-call discipline from the
all-idle state. -/
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
    decode := fun _ _ => some ProbeResult.done
    extCap := fun _ => false }

/-- The two sequential calls of fiber 0, with completions. -/
def probeTrace : Trace ProbeSig :=
  [issueObs ProbeSig (Caller.fiber 0) ProbeCall.go,
    compObs ProbeSig (Caller.fiber 0) ProbeCall.go ProbeResult.done,
    issueObs ProbeSig (Caller.fiber 0) ProbeCall.go,
    compObs ProbeSig (Caller.fiber 0) ProbeCall.go ProbeResult.done]

theorem seqOK_probeTrace : SeqOK ProbeSig probeTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- The keeper erasing every completion. -/
def probeKeep : ProbeCall → ProbeResult → Bool := fun _ _ => false

theorem probeShadow_eq :
    shadowTrace probeKeep probeTrace = [issueObs ProbeSig (Caller.fiber 0) ProbeCall.go,
      issueObs ProbeSig (Caller.fiber 0) ProbeCall.go] := by
  rfl

/-- The shadow of `probeTrace` violates the sequential-call discipline: its
second issue finds fiber 0 already in flight. -/
theorem seqOK_not_shadow : ¬ SeqOK ProbeSig (shadowTrace probeKeep probeTrace) := by
  rw [probeShadow_eq]
  intro h
  cases h with
  | consIssue m t cl c _ hrest =>
      cases hrest with
      | consIssue _ _ _ _ mf2 _ => simp at mf2

/-! ### The probe run

The six configurations of the probe run, and its step proofs. -/

def pc0 : SysCfg BaseOpsSig.none ProbeSig := encInit BaseOpsSig.none ProbeSig

def pc1 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def pc2 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := some { fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit },
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def pc3 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
    exts := [] }

def pc4 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def pc5 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := some { fiber := 0, call := ProbeCall.go, prog := ExtProg.pure SubVal.unit },
    parked := [], runq := [], retired := [], nextFiber := 1, exts := [] }

def pc6 : SysCfg BaseOpsSig.none ProbeSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [0], nextFiber := 1,
    exts := [] }

theorem s1 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc0 none pc1 :=
  SysStep.submit pc0 0 ProbeCall.go (Or.inl rfl)

theorem s2 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc1
    (some (issueObs ProbeSig (Caller.fiber 0) ProbeCall.go)) pc2 :=
  SysStep.dispatchFresh pc1 _ [] rfl rfl rfl

theorem s3 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc2
    (some (compObs ProbeSig (Caller.fiber 0) ProbeCall.go ProbeResult.done)) pc3 :=
  SysStep.complete pc2 _ SubVal.unit ProbeResult.done rfl rfl rfl

theorem s4 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc3 none pc4 :=
  SysStep.submit pc3 0 ProbeCall.go (Or.inr (List.Mem.head _))

theorem s5 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc4
    (some (issueObs ProbeSig (Caller.fiber 0) ProbeCall.go)) pc5 :=
  SysStep.dispatchFresh pc4 _ [] rfl rfl rfl

theorem s6 : SysStep BaseOpsSig.none ProbeSig probeEnc SubStep pc5
    (some (compObs ProbeSig (Caller.fiber 0) ProbeCall.go ProbeResult.done)) pc6 :=
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

/-! ## The execution domains are distinct (V2.2)

The domain distinction is semantic, not notational: an external call is not
ordered behind runnable fibers.  The certificate below produces, on the
primitive side, the code-shaped schedule -- two fibers queued ahead of an
external caller, whose whole call (issue and completion) serializes between
the first fiber's return and the second fiber's issue, bypassing the FIFO.
The fiber-only probe primitive cannot produce it (`fiberOnlyPrim` has no
external domain), so a lying encoding that declares a fiber-only call
externally capable over-produces -- the domain declaration is adjudicable. -/

/-- A two-call probe API: `go` (fiber-only shape) and `beep` (the
external-capable shape). -/
inductive DomCall : Type where
  | go
  | beep
deriving instance DecidableEq for DomCall

inductive DomResult : Type where
  | done
deriving instance DecidableEq for DomResult

abbrev DomSig : ApiSig := ⟨DomCall, DomResult⟩

/-- A probe primitive with both execution domains: `go` is fiber-only
(`run` completes inline; no external domain), `beep` is external-capable
(`extRun` completes inline). -/
def domPrim : PrimLTS2 DomSig :=
  { State := Unit
    init := ()
    admit := fun s _ _ => some s
    run := fun s _ _ c =>
      match c with
      | DomCall.go => some (DomResult.done, s, [])
      | DomCall.beep => none
    park := fun _ _ _ => none
    finish := fun _ _ _ => none
    extCap := fun c =>
      match c with
      | DomCall.go => false
      | DomCall.beep => true
    extRun := fun c s _ =>
      match c with
      | DomCall.go => none
      | DomCall.beep => some (DomResult.done, s, [])
    onTick := fun s _ => s
    expire := fun _ _ _ => none }

/-! ### The external call bypasses the runnable FIFO (primitive side)

Two fibers are submitted before the external caller; the external call's
issue *and* completion serialize between the first fiber's return and the
second fiber's issue.  The production code allows this: the
external-capable entry points consult only `global_mtx_`, never the
runnable queue. -/

def domTrace : Trace DomSig :=
  [issueObs DomSig (Caller.fiber 0) DomCall.go,
    compObs DomSig (Caller.fiber 0) DomCall.go DomResult.done,
    issueObs DomSig (Caller.ext 0) DomCall.beep,
    compObs DomSig (Caller.ext 0) DomCall.beep DomResult.done,
    issueObs DomSig (Caller.fiber 1) DomCall.go,
    compObs DomSig (Caller.fiber 1) DomCall.go DomResult.done]

def dp0 : PrimCfg DomSig domPrim := primInit DomSig domPrim

def dp1 : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := DomCall.go, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def dp2 : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := none, parked := [],
    runq := [{ fiber := 0, call := DomCall.go, fresh := true },
             { fiber := 1, call := DomCall.go, fresh := true }],
    retired := [], nextFiber := 2, exts := [] }

def dp3 : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := some (FSlot.running { fiber := 0, call := DomCall.go } false),
    parked := [], runq := [{ fiber := 1, call := DomCall.go, fresh := true }],
    retired := [], nextFiber := 2, exts := [] }

def dp3b : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := some (FSlot.returning { fiber := 0, call := DomCall.go } DomResult.done),
    parked := [], runq := [{ fiber := 1, call := DomCall.go, fresh := true }],
    retired := [], nextFiber := 2, exts := [] }

def dp4 : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := none, parked := [],
    runq := [{ fiber := 1, call := DomCall.go, fresh := true }],
    retired := [0], nextFiber := 2, exts := [] }

def dp5a : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := none, parked := [],
    runq := [{ fiber := 1, call := DomCall.go, fresh := true }],
    retired := [0], nextFiber := 2,
    exts := [{ x := 0, call := DomCall.beep, result := none }] }

def dp5b : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := none, parked := [],
    runq := [{ fiber := 1, call := DomCall.go, fresh := true }],
    retired := [0], nextFiber := 2,
    exts := [{ x := 0, call := DomCall.beep, result := some DomResult.done }] }

def dp6 : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := none, parked := [],
    runq := [{ fiber := 1, call := DomCall.go, fresh := true }],
    retired := [0], nextFiber := 2, exts := [] }

def dp7 : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := some (FSlot.running { fiber := 1, call := DomCall.go } false),
    parked := [], runq := [], retired := [0], nextFiber := 2, exts := [] }

def dp7b : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := some (FSlot.returning { fiber := 1, call := DomCall.go } DomResult.done),
    parked := [], runq := [], retired := [0], nextFiber := 2, exts := [] }

def dp8 : PrimCfg DomSig domPrim :=
  { prim := (), now := 0, cur := none, parked := [], runq := [], retired := [1, 0],
    nextFiber := 2, exts := [] }

theorem d1 : PrimStep2 DomSig domPrim dp0 none dp1 :=
  PrimStep2.submit dp0 0 DomCall.go (Or.inl rfl)

theorem d2 : PrimStep2 DomSig domPrim dp1 none dp2 :=
  PrimStep2.submit dp1 1 DomCall.go (Or.inl rfl)

theorem d3 : PrimStep2 DomSig domPrim dp2
    (some (issueObs DomSig (Caller.fiber 0) DomCall.go)) dp3 := by
  refine PrimStep2.dispatchFresh dp2 { fiber := 0, call := DomCall.go, fresh := true }
    [{ fiber := 1, call := DomCall.go, fresh := true }] () ?_ ?_ ?_ ?_
  all_goals rfl

theorem d4a : PrimStep2 DomSig domPrim dp3 none dp3b :=
  PrimStep2.fiberEffect dp3 { fiber := 0, call := DomCall.go } false [] [] []
    DomResult.done () [] rfl rfl rfl rfl rfl

theorem d4 : PrimStep2 DomSig domPrim dp3b
    (some (compObs DomSig (Caller.fiber 0) DomCall.go DomResult.done)) dp4 :=
  PrimStep2.fiberDone dp3b { fiber := 0, call := DomCall.go } DomResult.done rfl

theorem d5 : PrimStep2 DomSig domPrim dp4
    (some (issueObs DomSig (Caller.ext 0) DomCall.beep)) dp5a :=
  PrimStep2.extApply dp4 0 DomCall.beep [] [] rfl
    (by show 0 ∉ ([] : List (ExtPend DomSig)).map (fun e : ExtPend DomSig => e.x); simp)

theorem d5b : PrimStep2 DomSig domPrim dp5a none dp5b :=
  PrimStep2.extEffect dp5a [] [] { x := 0, call := DomCall.beep, result := none }
    DomResult.done () [] [] [] [] rfl rfl rfl rfl rfl

theorem d6 : PrimStep2 DomSig domPrim dp5b
    (some (compObs DomSig (Caller.ext 0) DomCall.beep DomResult.done)) dp6 :=
  PrimStep2.extDone dp5b [] [] { x := 0, call := DomCall.beep, result := some DomResult.done }
    DomResult.done rfl rfl

theorem d7 : PrimStep2 DomSig domPrim dp6
    (some (issueObs DomSig (Caller.fiber 1) DomCall.go)) dp7 := by
  refine PrimStep2.dispatchFresh dp6 { fiber := 1, call := DomCall.go, fresh := true } [] () ?_ ?_ ?_ ?_
  all_goals rfl

theorem d8a : PrimStep2 DomSig domPrim dp7 none dp7b :=
  PrimStep2.fiberEffect dp7 { fiber := 1, call := DomCall.go } false [] [] []
    DomResult.done () [] rfl rfl rfl rfl rfl

theorem d8 : PrimStep2 DomSig domPrim dp7b
    (some (compObs DomSig (Caller.fiber 1) DomCall.go DomResult.done)) dp8 :=
  PrimStep2.fiberDone dp7b { fiber := 1, call := DomCall.go } DomResult.done rfl

theorem seqOK_domTrace : SeqOK DomSig domTrace := by
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ rfl ?_
  refine SeqOKFrom.consComp _ _ _ _ _ rfl ?_
  refine SeqOKFrom.consIssue _ _ _ _ (by simp) ?_
  refine SeqOKFrom.consComp _ _ _ _ _ (by simp) ?_
  exact SeqOKFrom.nil _

/-- The probe primitive produces the FIFO-bypassing external schedule. -/
theorem domPrim_possesses : TracesPrim DomSig domPrim domTrace :=
  ⟨dp8, PrimRuns2.step dp0 dp1 none _ dp8 d1
    (PrimRuns2.step dp1 dp2 none _ dp8 d2
      (PrimRuns2.step dp2 dp3 _ _ dp8 d3
        (PrimRuns2.step dp3 dp3b none _ dp8 d4a
          (PrimRuns2.step dp3b dp4 _ _ dp8 d4
            (PrimRuns2.step dp4 dp5a _ _ dp8 d5
              (PrimRuns2.step dp5a dp5b none _ dp8 d5b
                (PrimRuns2.step dp5b dp6 _ _ dp8 d6
                  (PrimRuns2.step dp6 dp7 _ _ dp8 d7
                    (PrimRuns2.step dp7 dp7b none _ dp8 d8a
                      (PrimRuns2.step dp7b dp8 _ _ dp8 d8 (PrimRuns2.stop dp8)))))))))))⟩

/-! ### The encoding side runs the same external schedule

The matching probe encoding declares the same call domains; its external
caller executes the program stepwise outside the FIFO, with the issue and
completion in the same positions. -/

def domEnc : Encoding BaseOpsSig.none DomSig :=
  { prog := fun _ => ExtProg.pure SubVal.unit
    decode := fun _ _ => some DomResult.done
    extCap := fun c =>
      match c with
      | DomCall.go => false
      | DomCall.beep => true }

def de0 : SysCfg BaseOpsSig.none DomSig := encInit BaseOpsSig.none DomSig

def de1 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 0, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [], nextFiber := 1, exts := [] }

def de2 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 0, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true },
             { fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [], nextFiber := 2, exts := [] }

def de3 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (),
    cur := some { fiber := 0, call := DomCall.go, prog := ExtProg.pure SubVal.unit },
    parked := [],
    runq := [{ fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [], nextFiber := 2, exts := [] }

def de4 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [0], nextFiber := 2, exts := [] }

def de5 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [0], nextFiber := 2,
    exts := [{ x := 0, call := DomCall.beep, prog := ExtProg.pure SubVal.unit }] }

def de6 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [],
    runq := [{ fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }],
    retired := [0], nextFiber := 2, exts := [] }

def de7 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (),
    cur := some { fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit },
    parked := [], runq := [], retired := [0], nextFiber := 2, exts := [] }

def de8 : SysCfg BaseOpsSig.none DomSig :=
  { st := subInit, bst := (), cur := none, parked := [], runq := [], retired := [1, 0],
    nextFiber := 2, exts := [] }

theorem e1 : SysStep BaseOpsSig.none DomSig domEnc SubStep de0 none de1 :=
  SysStep.submit de0 0 DomCall.go (Or.inl rfl)

theorem e2 : SysStep BaseOpsSig.none DomSig domEnc SubStep de1 none de2 :=
  SysStep.submit de1 1 DomCall.go (Or.inl rfl)

theorem e3 : SysStep BaseOpsSig.none DomSig domEnc SubStep de2
    (some (issueObs DomSig (Caller.fiber 0) DomCall.go)) de3 := by
  refine SysStep.dispatchFresh de2
    { fiber := 0, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }
    [{ fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true }] ?_ ?_ ?_
  all_goals rfl

theorem e4 : SysStep BaseOpsSig.none DomSig domEnc SubStep de3
    (some (compObs DomSig (Caller.fiber 0) DomCall.go DomResult.done)) de4 :=
  SysStep.complete de3 _ SubVal.unit DomResult.done rfl rfl rfl

theorem e5 : SysStep BaseOpsSig.none DomSig domEnc SubStep de4
    (some (issueObs DomSig (Caller.ext 0) DomCall.beep)) de5 :=
  SysStep.extStart de4 0 DomCall.beep rfl
    (by show 0 ∉ ([] : List (ExtBusy BaseOpsSig.none DomSig)).map (fun e : ExtBusy BaseOpsSig.none DomSig => e.x); simp)

theorem e6 : SysStep BaseOpsSig.none DomSig domEnc SubStep de5
    (some (compObs DomSig (Caller.ext 0) DomCall.beep DomResult.done)) de6 :=
  SysStep.extComplete de5 [] [] { x := 0, call := DomCall.beep, prog := ExtProg.pure SubVal.unit }
    SubVal.unit DomResult.done rfl rfl rfl

theorem e7 : SysStep BaseOpsSig.none DomSig domEnc SubStep de6
    (some (issueObs DomSig (Caller.fiber 1) DomCall.go)) de7 := by
  refine SysStep.dispatchFresh de6
    { fiber := 1, call := DomCall.go, prog := ExtProg.pure SubVal.unit, fresh := true } [] ?_ ?_ ?_
  all_goals rfl

theorem e8 : SysStep BaseOpsSig.none DomSig domEnc SubStep de7
    (some (compObs DomSig (Caller.fiber 1) DomCall.go DomResult.done)) de8 :=
  SysStep.complete de7 _ SubVal.unit DomResult.done rfl rfl rfl

/-- The encoding side produces the same FIFO-bypassing external schedule:
the execution domains are symmetric. -/
theorem domEnc_possesses : TracesEnc BaseOpsSig.none DomSig domEnc domTrace :=
  ⟨de8, SysRuns.step de0 de1 none _ de8 e1
    (SysRuns.step de1 de2 none _ de8 e2
      (SysRuns.step de2 de3 _ _ de8 e3
        (SysRuns.step de3 de4 _ _ de8 e4
          (SysRuns.step de4 de5 _ _ de8 e5
            (SysRuns.step de5 de6 _ _ de8 e6
              (SysRuns.step de6 de7 _ _ de8 e7
                (SysRuns.step de7 de8 _ [] de8 e8 (SysRuns.stop de8))))))))⟩

theorem domEnc_possesses_domTrace : TracesEncS BaseOpsSig.none DomSig domEnc domTrace :=
  ⟨domEnc_possesses, seqOK_domTrace⟩

/-- A lying encoding -- one that declares the fiber-only `go` externally
capable -- over-produces against `domPrim`; see `VacuityV2.lean`
(`encLie_overProduces`), where the adjudication layer closes the argument. -/
def encLie : Encoding BaseOpsSig.none DomSig :=
  { prog := fun _ => ExtProg.pure SubVal.unit
    decode := fun _ _ => some DomResult.done
    extCap := fun _ => true }

end Sluice.Formal
