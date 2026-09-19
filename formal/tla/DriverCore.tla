---------------- MODULE DriverCore ----------------
(***************************************************************************)
(* Stage 8V2.3 (FCB1-POST-V23-STACK-379-385): the scheduler driver's       *)
(* transition system, mapping `formal/Sluice/Formal/DriverV2.lean`         *)
(* (`RunStep`, the bespoke driver LTS) one action per constructor, under   *)
(* the post-#378 Stage-0-V2.3 execution-domain calculus.  Rebuilt from     *)
(* the production sections of `src/async/scheduler.cpp` and the fiber      *)
(* bridge in `src/async/fiber.cpp`.                                        *)
(*                                                                         *)
(* The driver is NOT a `PrimLTS2` (the drain loop fits neither the         *)
(* run-to-block fiber shape nor the fused external section: external       *)
(* spawns interleave INSIDE the run loop).  It is a declared substrate     *)
(* extension (Stage-0 section 9.3): the driver state the bare substrate    *)
(* does not carry is exactly `DriverState` -- the terminate flag           *)
(* (`global_terminate_`, committed at the drain exit, cleared at           *)
(* `run_impl` entry), the in-flight drain record (the single coordinated   *)
(* run), and the unclaimed-spawn queue (`pending_spawn_`).                 *)
(*                                                                         *)
(* Frozen core instance: `run(1)` / `run_until_idle` at ONE worker over    *)
(* plain tasks.  The extensions outside this instance, recorded in the     *)
(* stage card and NOT modeled here: multi-worker run (the serialized       *)
(* single-worker discipline is the frozen core; the multi-worker           *)
(* adjudication is a campaign-level question, stage-0 section 9.4),        *)
(* `run_live`, the mw_s2/mw_s3 classifier legs, the wake-scan legs, and    *)
(* the fiber-path spawn (a nested blocking call, excluded as a SeqOK       *)
(* conflict).                                                              *)
(*                                                                         *)
(* Action census (RunStep's eight constructors, from the code):            *)
(*   extSpawnApply    - an external spawn ENTERS (the issue observation;   *)
(*                      no state effect; scheduler.cpp:131-134)            *)
(*   extSpawnEffectIn - the spawn's fused section with a run active: the   *)
(*                      task is minted onto the backlog tail               *)
(*                      (scheduler.cpp:142-148) -- silent                  *)
(*   extSpawnEffectOut- the section with no active run: the task is        *)
(*                      unclaimed (`pending_spawn_`, scheduler.cpp:157)    *)
(*                      -- silent                                          *)
(*   extSpawnDone     - the spawn's physical return (the completion        *)
(*                      observation)                                       *)
(*   dispatch         - the worker pops the backlog head; the task's       *)
(*                      issue observation is emitted (the pop,             *)
(*                      `make_running`, and the count increment are one    *)
(*                      driver-side unit, scheduler.cpp:332-347,           *)
(*                      :731-736)                                          *)
(*   workDone         - the task completes (the entry bridge's `make_done` *)
(*                      plus the final switch back, fiber.cpp:25-34)       *)
(*   drainEnter       - the run call's issue observation, the terminate    *)
(*                      flag cleared, the unclaimed spawns flushed into    *)
(*                      the backlog (run_impl :229-247)                    *)
(*   drainReturn      - the drain returns at quiescence (backlog empty,    *)
(*                      worker free; the classifier's quiescent exit at    *)
(*                      one worker, scheduler.cpp:1013-1039 + :642-663);   *)
(*                      the terminate flag is committed, the drain is      *)
(*                      freed, and the only drain completion carries       *)
(*                      `rReturned`                                        *)
(*                                                                         *)
(* Per-caller blocking, folded into the gates (the raw Lean run language   *)
(* relies on the SeqOK trace filter; here the discipline is                *)
(* state-checkable, so the gates the C++ calling threads obey are in the   *)
(* actions): a caller with an in-flight drain cannot spawn (the run call   *)
(* blocks the calling thread), and a caller with an un-returned spawn      *)
(* record cannot issue the drain.                                          *)
(*                                                                         *)
(* Fuel bounds (finitization, disclosed): at most two minted task fibers   *)
(* (`nextFiber < 2` at the mint sites) and `MaxHistory` recorded           *)
(* observations.                                                           *)
(*                                                                         *)
(* Reachability disclosure: the `loaded` boot is an instrumented battery   *)
(* start (relation-reachable, not runInit-reachable -- the drain is        *)
(* already in flight over a queued task, the b2 battery's mid-run start).  *)
(*   "prim"   - the runInit-faithful start                                 *)
(*   "loaded" - drain in flight, one task queued                           *)
(*                                                                         *)
(* Mutant switches (all FALSE in the reference configuration):              *)
(*   MutQuiescence        - the drain-return quiescence gates dropped      *)
(*                          (SAFETY: the terminate flag commits with the   *)
(*                          worker occupied; killed by InvTermQuiet)       *)
(*   MutDuplicateDispatch - the worker-free gate dropped from dispatch     *)
(*                          (SAFETY: a second task pops onto an occupied   *)
(*                          worker and the first vanishes from the         *)
(*                          accounting; killed by InvConservation)         *)
(*   MutDrainResult       - the drain returns `rDone` (RESULT-SEMANTICS:   *)
(*                          killed by InvDrainResult -- the model's only   *)
(*                          drain completion carries `rReturned`)          *)
(*   MutSilentDispatch    - the task's issue observation dropped from      *)
(*                          dispatch (TRACE-REMOVAL: no step emits a       *)
(*                          `work` issue; every state invariant holds;     *)
(*                          the completion discipline is excluded from     *)
(*                          its configuration because the removed issue    *)
(*                          orphans the completion -- that break IS the    *)
(*                          removal, not an independent fault)             *)
(*   MutUnbackedMint      - the spawn section fixes the result without     *)
(*                          minting the task (RESULT-BINDING: the state    *)
(*                          stays safe and the drain's quiescence gate     *)
(*                          is satisfied with the minted task vaporized;   *)
(*                          the unbacked four-observation trace becomes    *)
(*                          reachable -- killed by NotUnbacked, which the  *)
(*                          correct model cannot violate; the Lean side    *)
(*                          carries that model-side separation)            *)
(***************************************************************************)
EXTENDS Naturals, Sequences
CONSTANT MaxHistory,   \* fuel: bound on recorded observations
          Boot,        \* initial state: see the disclosure above
          MutQuiescence,
          MutDuplicateDispatch,
          MutDrainResult,
          MutSilentDispatch,
          MutUnbackedMint

ASSUME Boot \in {"prim", "loaded"}

Fibers == {"f0", "f1"}
Exts == {"e0", "e1", "e2"}
Calls == {"spawn", "work", "drain"}
ExtCalls == {"spawn", "drain"}
Results == {"none", "rSpawn", "rDone", "rReturned"}

\* The minted task fiber for backlog position n (fuel-capped at 2).
FiberOf(n) == IF n = 0 THEN "f0" ELSE "f1"
FibIndex(f) == IF f = "f0" THEN 0 ELSE 1

VARIABLES term,        \* global_terminate_: committed at the drain exit
          drainCaller, \* the in-flight drain record, or "none"
          pending,     \* pending_spawn_: unclaimed tasks
          cur,         \* the dispatched task's slot
          runq,        \* the merged runnable backlog (FIFO)
          retired,     \* completed task fibers
          nextFiber,   \* the next minted fiber id
          exts,        \* in-flight spawn records
          history,     \* observation trace
          hit_drain_ret,  \* coverage: a drain returned
          hit_dispatch,   \* coverage: a task was dispatched
          hit_work_done,  \* coverage: a task completed
          hit_effect_in,  \* coverage: a spawn section minted onto the backlog
          hit_effect_out, \* coverage: a spawn section left the task unclaimed
          hit_flush,      \* coverage: the entry flush moved a pending task
          hit_stale,      \* coverage: the drain returned over an un-sectioned spawn
          hit_post_term   \* coverage: a post-terminate spawn went unclaimed

vars == <<term, drainCaller, pending, cur, runq, retired, nextFiber,
          exts, history, hit_drain_ret, hit_dispatch, hit_work_done,
          hit_effect_in, hit_effect_out, hit_flush, hit_stale,
          hit_post_term>>

NoCur == [fiber |-> "none", call |-> "none", phase |-> "off",
          result |-> "none"]

IssueFib(f, c) == [type |-> "issue", src |-> "fib", id |-> f,
                   call |-> c, result |-> "none"]
IssueExt(x, c) == [type |-> "issue", src |-> "ext", id |-> x,
                   call |-> c, result |-> "none"]
CompFib(f, c, r) == [type |-> "comp", src |-> "fib", id |-> f,
                     call |-> c, result |-> r]
CompExt(x, c, r) == [type |-> "comp", src |-> "ext", id |-> x,
                     call |-> c, result |-> r]

Init ==
  /\ term = FALSE
  /\ drainCaller = IF Boot = "prim" THEN "none" ELSE "e0"
  /\ pending = << >>
  /\ cur = NoCur
  /\ runq = IF Boot = "prim" THEN << >> ELSE <<"f0">>
  /\ retired = << >>
  /\ nextFiber = IF Boot = "prim" THEN 0 ELSE 1
  /\ exts = {}
  /\ history = << >>
  /\ hit_drain_ret = FALSE
  /\ hit_dispatch = FALSE
  /\ hit_work_done = FALSE
  /\ hit_effect_in = FALSE
  /\ hit_effect_out = FALSE
  /\ hit_flush = FALSE
  /\ hit_stale = FALSE
  /\ hit_post_term = FALSE

\* RunStep.extSpawnApply: an external spawn enters (the issue
\* observation; no state effect).  The calling thread is blocked in
\* `run` while its drain is in flight, and a spawn record of the same
\* caller is still pending its physical return.
ExtSpawnApply(x) ==
  /\ x \in Exts
  /\ \A e \in exts : e.x # x
  /\ x # drainCaller
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {[x |-> x, call |-> "spawn", result |-> "none"]}
  /\ history' = Append(history, IssueExt(x, "spawn"))
  /\ UNCHANGED <<term, drainCaller, pending, cur, runq, retired,
                 nextFiber, hit_drain_ret, hit_dispatch, hit_work_done,
                 hit_effect_in, hit_effect_out, hit_flush, hit_stale,
                 hit_post_term>>

\* RunStep.extSpawnEffectIn: the fused section with a run active -- the
\* task is minted onto the backlog tail (scheduler.cpp:142-148).  Silent.
\* MutUnbackedMint fixes the result without the mint -- the kill site.
ExtSpawnEffectIn ==
  /\ \E e \in exts :
       /\ e.result = "none"
       /\ drainCaller # "none"
       /\ nextFiber < 2
       /\ exts' = (exts \ {e})
             \union {[x |-> e.x, call |-> "spawn", result |-> "rSpawn"]}
       /\ runq' = IF MutUnbackedMint THEN runq
                  ELSE Append(runq, FiberOf(nextFiber))
       /\ nextFiber' = IF MutUnbackedMint THEN nextFiber
                       ELSE nextFiber + 1
  /\ hit_effect_in' = TRUE
  /\ UNCHANGED <<term, drainCaller, pending, cur, retired, history,
                 hit_drain_ret, hit_dispatch, hit_work_done,
                 hit_effect_out, hit_flush, hit_stale, hit_post_term>>

\* RunStep.extSpawnEffectOut: the section with no active run -- the task
\* is unclaimed (`pending_spawn_`, scheduler.cpp:157).  Silent.
ExtSpawnEffectOut ==
  /\ \E e \in exts :
       /\ e.result = "none"
       /\ drainCaller = "none"
       /\ nextFiber < 2
       /\ exts' = (exts \ {e})
             \union {[x |-> e.x, call |-> "spawn", result |-> "rSpawn"]}
       /\ pending' = Append(pending, FiberOf(nextFiber))
       /\ nextFiber' = nextFiber + 1
  /\ hit_effect_out' = TRUE
  /\ hit_post_term' = (hit_post_term \/ term)
  /\ UNCHANGED <<term, drainCaller, cur, runq, retired, history,
                 hit_drain_ret, hit_dispatch, hit_work_done,
                 hit_effect_in, hit_flush, hit_stale>>

\* RunStep.extSpawnDone: the spawn's physical return (the completion
\* observation), deliberately unordered with respect to the other steps.
ExtSpawnDone ==
  /\ \E e \in exts : e.result = "rSpawn"
  /\ Len(history) < MaxHistory
  /\ LET e == CHOOSE e \in exts : e.result = "rSpawn"
     IN /\ exts' = exts \ {e}
        /\ history' = Append(history, CompExt(e.x, "spawn", "rSpawn"))
  /\ UNCHANGED <<term, drainCaller, pending, cur, runq, retired,
                 nextFiber, hit_drain_ret, hit_dispatch, hit_work_done,
                 hit_effect_in, hit_effect_out, hit_flush, hit_stale,
                 hit_post_term>>

\* RunStep.dispatch: the worker pops the backlog head; the task's issue
\* observation is emitted (scheduler.cpp:332-347, :731-736).  The pop,
\* `make_running`, and the count increment are one driver-side unit.
\* MutDuplicateDispatch drops the worker-free gate -- the kill site.
\* MutSilentDispatch drops the observation -- the trace-removal site.
Dispatch ==
  /\ Len(runq) > 0
  /\ cur = NoCur \/ MutDuplicateDispatch
  /\ drainCaller # "none"
  /\ cur' = [fiber |-> Head(runq), call |-> "work", phase |-> "run",
             result |-> "none"]
  /\ runq' = Tail(runq)
  /\ history' = IF MutSilentDispatch THEN history
                ELSE Append(history, IssueFib(Head(runq), "work"))
  /\ hit_dispatch' = (hit_dispatch \/ ~MutSilentDispatch)
  /\ UNCHANGED <<term, drainCaller, pending, retired, nextFiber, exts,
                 hit_drain_ret, hit_work_done, hit_effect_in,
                 hit_effect_out, hit_flush, hit_stale, hit_post_term>>

\* RunStep.workDone: the task completes (fiber.cpp:25-34 -- the entry
\* bridge's `make_done` plus the final switch back).  A plain task's
\* driver-state effect is empty: the slot goes straight to retired.
WorkDone ==
  /\ cur # NoCur /\ cur.phase = "run"
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, "work", "rDone"))
  /\ retired' = Append(retired, cur.fiber)
  /\ cur' = NoCur
  /\ hit_work_done' = TRUE
  /\ UNCHANGED <<term, drainCaller, pending, runq, nextFiber,
                 exts, hit_drain_ret, hit_dispatch, hit_effect_in,
                 hit_effect_out, hit_flush, hit_stale, hit_post_term>>

\* RunStep.drainEnter: the run call's issue observation, the terminate
\* flag cleared, the unclaimed spawns flushed into the backlog
\* (run_impl :229-247).  The calling thread is blocked while its spawn
\* record is un-returned.
DrainEnter(x) ==
  /\ x \in Exts
  /\ drainCaller = "none"
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ term' = FALSE
  /\ drainCaller' = x
  /\ runq' = runq \o pending
  /\ pending' = << >>
  /\ history' = Append(history, IssueExt(x, "drain"))
  /\ hit_flush' = (hit_flush \/ pending # << >>)
  /\ UNCHANGED <<cur, retired, nextFiber, exts, hit_drain_ret,
                 hit_dispatch, hit_work_done, hit_effect_in,
                 hit_effect_out, hit_stale, hit_post_term>>

\* RunStep.drainReturn: the drain returns at quiescence -- backlog
\* empty, worker free (scheduler.cpp:1013-1039 + :642-663) -- the
\* terminate flag is committed and the drain is freed.  The only drain
\* completion carries `rReturned` (MutDrainResult's kill site); the
\* quiescence gates are MutQuiescence's kill site.
DrainReturn(x) ==
  /\ x \in Exts
  /\ drainCaller = x
  /\ (runq = << >> /\ cur = NoCur) \/ MutQuiescence
  /\ term' = TRUE
  /\ drainCaller' = "none"
  /\ history' = Append(history,
       CompExt(x, "drain", IF MutDrainResult THEN "rDone" ELSE "rReturned"))
  /\ hit_drain_ret' = TRUE
  /\ hit_stale' = (hit_stale \/ \E e \in exts : e.result = "none")
  /\ UNCHANGED <<pending, cur, runq, retired, nextFiber, exts,
                 hit_dispatch, hit_work_done, hit_effect_in,
                 hit_effect_out, hit_flush, hit_post_term>>

Next ==
  \/ \E x \in Exts : ExtSpawnApply(x)
  \/ ExtSpawnEffectIn
  \/ ExtSpawnEffectOut
  \/ ExtSpawnDone
  \/ Dispatch
  \/ WorkDone
  \/ \E x \in Exts : DrainEnter(x)
  \/ \E x \in Exts : DrainReturn(x)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type and safety invariants (the `runSafe` discipline, one invariant     *)
(* per conjunct family)                                                    *)
(***************************************************************************)

TypeOK ==
  /\ term \in BOOLEAN
  /\ drainCaller \in (Exts \cup {"none"})
  /\ pending \in Seq(Fibers) /\ runq \in Seq(Fibers)
  /\ retired \in Seq(Fibers)
  /\ nextFiber \in Nat
  /\ cur \in ({NoCur} \union
       {[fiber |-> f, call |-> "work", phase |-> "run", result |-> "none"] :
        f \in Fibers})
  /\ exts \subseteq {[x |-> x, call |-> "spawn", result |-> r] :
                     x \in Exts, r \in {"none", "rSpawn"}}
  /\ \A e1 \in exts, e2 \in exts : e1.x = e2.x => e1 = e2
  /\ history \in Seq({[type |-> t, src |-> s, id |-> i, call |-> c,
                       result |-> r] :
                       t \in {"issue", "comp"}, s \in {"fib", "ext"},
                       i \in (Fibers \cup Exts), c \in Calls,
                       r \in Results})
  /\ hit_drain_ret \in BOOLEAN /\ hit_dispatch \in BOOLEAN
  /\ hit_work_done \in BOOLEAN /\ hit_effect_in \in BOOLEAN
  /\ hit_effect_out \in BOOLEAN /\ hit_flush \in BOOLEAN
  /\ hit_stale \in BOOLEAN /\ hit_post_term \in BOOLEAN

\* Task conservation: every minted task is exactly one of queued,
\* unclaimed, running, or retired -- no lost or duplicated task.
InvConservation ==
  Len(runq) + Len(pending)
    + (IF cur # NoCur THEN 1 ELSE 0) + Len(retired) = nextFiber

\* Fresh mints: every recorded fiber id is below the mint counter.
InvIdBounds ==
  /\ \A i \in 1..Len(runq) : FibIndex(runq[i]) < nextFiber
  /\ \A i \in 1..Len(pending) : FibIndex(pending[i]) < nextFiber
  /\ \A i \in 1..Len(retired) : FibIndex(retired[i]) < nextFiber
  /\ cur # NoCur => FibIndex(cur.fiber) < nextFiber

\* Worker occupancy: a task on the worker implies an active run.
InvOccupied == cur # NoCur => drainCaller # "none"

\* Idle-backlog shape: no active run => empty backlog (the backlog
\* gains tasks only through in-run effects and the entry flush).
InvIdleBacklog == drainCaller = "none" => runq = << >>

\* Terminate quiescence: the terminate flag is committed only at the
\* quiescent exit (MutQuiescence's kill).
InvTermQuiet ==
  term => /\ drainCaller = "none"
          /\ runq = << >>
          /\ cur = NoCur

\* In-flight spawn records' shape.
InvExtShape ==
  \A e \in exts : e.call = "spawn" /\ e.result \in {"none", "rSpawn"}

\* The drain's result binding: every drain completion carries
\* `rReturned` (MutDrainResult's kill).
InvDrainResult ==
  \A i \in 1..Len(history) :
    history[i].type = "comp" /\ history[i].call = "drain" =>
      history[i].result = "rReturned"

SameCaller(o1, o2) == o1.src = o2.src /\ o1.id = o2.id

\* Completion discipline: every completion matches a prior issue of the
\* same call by the same caller, with no intervening completion of that
\* caller -- EXCEPT the loaded boot's pre-Init drain instance (the
\* instrumented configuration starts with the drain already in flight,
\* the documented battery disclosure; the exception is prefix-scoped, so
\* the invariant is monotone).
InvCompDiscipline ==
  \A i \in 1..Len(history) :
    history[i].type = "comp" =>
      \/ \E j \in 1..(i - 1) :
            /\ history[j].type = "issue"
            /\ SameCaller(history[j], history[i])
            /\ history[j].call = history[i].call
            /\ \A k \in (j + 1)..(i - 1) :
                 ~SameCaller(history[k], history[i])
      \/ ~\E k \in 1..i :
            history[k].type = "issue"
            /\ SameCaller(history[k], history[i])
            /\ history[k].call = history[i].call

SafetyInvariants == <<TypeOK, InvConservation, InvIdBounds, InvOccupied,
                      InvIdleBacklog, InvTermQuiet, InvExtShape,
                      InvCompDiscipline>>

(***************************************************************************)
(* Coverage predicates and the witness separations                         *)
(*                                                                         *)
(* All are monotone (history append or set-once flags), so a single TLC    *)
(* run reaching the conjunctive state certifies every component; each      *)
(* coverage cfg checks the negated conjunction and must be VIOLATED.       *)
(* The MutSilentDispatch separation cfg asserts its witness is UNreachable *)
(* and must HOLD.  The MutUnbackedMint separation cfg asserts the          *)
(* unbacked trace IS reachable there and must be VIOLATED (the correct     *)
(* model's unreachability is the Lean separation).                         *)
(***************************************************************************)

\* An empty drain: the run entered and returned with nothing dispatched.
CovEmptyDrain == hit_drain_ret /\ ~hit_dispatch
\* Two tasks through the backlog and the worker in one run.
CovFifoTwo == Len(retired) = 2
\* A spawn section minted onto the backlog mid-run.
CovEffectIn == hit_effect_in
\* The entry flush moved a pending task onto the backlog.
CovFlush == hit_flush
\* The stale-classify window: the drain returned over an un-sectioned
\* spawn record (the section lands after the run).
CovStale == hit_stale
\* A post-terminate spawn went unclaimed.
CovPostTerm == hit_post_term
\* A task's issue observation (the MutSilentDispatch pairing).
CovDispatch == hit_dispatch
\* The unbacked-mint trace: drain issue, spawn issue, spawn completion,
\* drain completion -- no work observation between (the section's mint
\* never happened).  Adjacent in the observation history.
NotUnbacked ==
  ~\E i \in 1..(Len(history) - 3) :
       /\ history[i] = IssueExt("e0", "drain")
       /\ history[i + 1] = IssueExt("e1", "spawn")
       /\ history[i + 2] = CompExt("e1", "spawn", "rSpawn")
       /\ history[i + 3] = CompExt("e0", "drain", "rReturned")
\* No fiber issue observation at all (the MutSilentDispatch witness:
\* no step emits the task's issue; the pairing coverage cfg certifies
\* the issue IS reachable in the correct model).
NotWorkIssue ==
  ~\E i \in 1..Len(history) :
       history[i].src = "fib" /\ history[i].type = "issue"

InvCovEmptyDrain == ~CovEmptyDrain
InvCovFifoTwo == ~CovFifoTwo
InvCovEffectIn == ~CovEffectIn
InvCovFlush == ~CovFlush
InvCovStale == ~CovStale
InvCovPostTerm == ~CovPostTerm
InvCovDispatch == ~CovDispatch

=============================================================================
