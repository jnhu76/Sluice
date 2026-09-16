---------------- MODULE EventCore ----------------
(***************************************************************************)
(* Stage 1V2.2 (FCB1-METHOD-CORRECTIVE-1): the Event primitive's           *)
(* transition system, mapping `formal/Sluice/Formal/EventV2.lean`          *)
(* (`eventPrim`, `PrimStep2` instantiated for `EventSig`) one action per   *)
(* constructor, under the Stage-0-V2.2 execution-domain calculus.          *)
(*                                                                         *)
(* Call-domain census (from the code):                                     *)
(*   wait  = fiber-only   (`await_event_wait` dereferences `g_worker`)     *)
(*   set   = external-capable (`event_set_broadcast`: `global_mtx_` only)  *)
(*   reset = external-capable (`event_reset`: `global_mtx_` only)          *)
(*                                                                         *)
(* Fiber actions (one per `PrimStep2` constructor):                        *)
(*   Submit          - fiber entry into the runnable queue                 *)
(*   DispatchFresh   - dispatch of a fresh entry, emits the issue          *)
(*   DispatchResumed - silent dispatch of a published (drained) waiter     *)
(*   RunSet          - `Event::set`: latch + atomic drain of the whole     *)
(*                     wait queue (`Scheduler::event_set_broadcast`)       *)
(*   RunReset        - `Event::reset`: unlatch, waiters stay parked        *)
(*   RunWaitInline   - `Event::wait` with the latch set: completes inline  *)
(*   RunWaitPark      - `Event::wait` with the latch clear: parks          *)
(*                     (`event_wait_admit_locked` / `await_event_wait`)    *)
(*   FinishWait      - a resumed waiter completes one-shot                 *)
(*                                                                         *)
(* External actions (the V2.2 three-phase sequence; a `global_mtx_`-only   *)
(* entry point never enters the runnable FIFO, and its physical return is  *)
(* unordered with respect to fiber steps):                                 *)
(*   ExtApplySet/ExtApplyReset   - entry: emits the issue observation and  *)
(*                                 registers a result-less record          *)
(*   ExtEffectSet/ExtEffectReset - critical section: silent state effect   *)
(*                                 (the set drains the whole wait queue,   *)
(*                                 publishing every waiter)                *)
(*   ExtDoneSet/ExtDoneReset     - physical return: emits the completion   *)
(*                                                                         *)
(* `parked` of the Lean machine is dropped: only `wait` parks, `RunWait-   *)
(* Park` appends the fiber to `waitq`, and every set drains it, so the     *)
(* parked list is always the waiter list of `waitq`.  The Event instance's *)
(* environment steps do nothing (`onTick` = id, `expire` = none) and are   *)
(* omitted.                                                                *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANT MaxFiber,          \* fuel: highest fiber id Submit may mint
          MaxExt,           \* fuel: highest external caller id
          MaxHistory,       \* fuel: bound on recorded observations
          WaitInlineOnClear \* mutant switch: wait completes inline on a
                            \* clear flag (refutes the safety property)

VARIABLES flag,      \* the latch (EventState.flag)
          waitq,     \* parked waiters, FIFO (EventState.waitq)
          cur,       \* dispatched call [fiber, call, resumed], or NoCur
          runq,      \* runnable queue of [fiber, call, fresh] entries
          retired,   \* fibers whose calls completed (one-shot semantics)
          nextFiber, \* fresh-fiber counter
          exts,      \* in-flight external records (at most one per caller)
          history    \* observation trace: issue/comp records

vars == <<flag, waitq, cur, runq, retired, nextFiber, exts, history>>

Calls == {"wait", "set", "reset"}
ExtCalls == {"set", "reset"}
Fibers == 0..MaxFiber
Exts == 0..MaxExt

Entry(f, c, fr) == [fiber |-> f, call |-> c, fresh |-> fr]
ExtRec(x, c, p) == [x |-> x, call |-> c, phase |-> p]

\* Observations record the caller's execution domain: a scheduler fiber or
\* an external OS thread -- external callers are never faked as fibers.
IssueFib(f, c) == [type |-> "issue", src |-> "fib", id |-> f, call |-> c, result |-> "none"]
IssueExt(x, c) == [type |-> "issue", src |-> "ext", id |-> x, call |-> c, result |-> "none"]
CompFib(f, c)  == [type |-> "comp",  src |-> "fib", id |-> f, call |-> c, result |-> "done"]
CompExt(x, c)  == [type |-> "comp",  src |-> "ext", id |-> x, call |-> c, result |-> "done"]

NoCur == [fiber |-> MaxFiber + 1, call |-> "none", resumed |-> FALSE]

RECURSIVE WaiterEntries(_)
WaiterEntries(q) ==
  IF q = << >> THEN << >>
  ELSE (<< Entry(Head(q), "wait", FALSE) >> \o WaiterEntries(Tail(q)))

\* A completing fiber retires unless it already had (re-entry of a retired
\* fiber keeps its record).
RetiredAfter(f, R) == IF f \in R THEN R ELSE {f} \cup R

Init == flag = FALSE
  /\ waitq = << >>
  /\ cur = NoCur
  /\ runq = << >>
  /\ retired = {}
  /\ nextFiber = 0
  /\ exts = {}
  /\ history = << >>

\* PrimStep2.submit: silent, appends a fresh entry.  Minting a fresh fiber
\* consumes fuel; re-entering a retired fiber does not.
Submit(f, c) ==
  /\ ((f = nextFiber /\ nextFiber < MaxFiber) \/ (f \in retired /\ f # nextFiber))
  /\ Len(history) < MaxHistory
  /\ runq' = Append(runq, Entry(f, c, TRUE))
  /\ retired' = retired \ {f}
  /\ nextFiber' = IF f = nextFiber THEN nextFiber + 1 ELSE nextFiber
  /\ flag' = flag /\ waitq' = waitq /\ cur' = cur
  /\ exts' = exts /\ history' = history

\* PrimStep2.dispatchFresh: emits the issue observation.
DispatchFresh ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = TRUE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             resumed |-> FALSE]
  /\ runq' = Tail(runq)
  /\ history' = Append(history, IssueFib(Head(runq).fiber, Head(runq).call))
  /\ flag' = flag /\ waitq' = waitq /\ retired' = retired
  /\ nextFiber' = nextFiber /\ exts' = exts

\* PrimStep2.dispatchResumed: silent dispatch of a published waiter.
DispatchResumed ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = FALSE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             resumed |-> TRUE]
  /\ runq' = Tail(runq)
  /\ flag' = flag /\ waitq' = waitq /\ retired' = retired
  /\ nextFiber' = nextFiber /\ exts' = exts /\ history' = history

\* PrimStep2.runDone for `set`: the fiber call's fused critical section --
\* latch, drain the whole wait queue into the runnable tail ahead of any
\* later submit, complete inline.  The set's issue was its dispatch.
RunSet ==
  /\ cur # NoCur
  /\ cur.resumed = FALSE
  /\ cur.call = "set"
  /\ flag' = TRUE
  /\ waitq' = << >>
  /\ runq' = runq \o WaiterEntries(waitq)
  /\ cur' = NoCur
  /\ retired' = RetiredAfter(cur.fiber, retired)
  /\ nextFiber' = nextFiber
  /\ exts' = exts
  /\ history' = Append(history, CompFib(cur.fiber, "set"))

\* PrimStep2.runDone for `reset`: unlatch, nobody published.
RunReset ==
  /\ cur # NoCur
  /\ cur.resumed = FALSE
  /\ cur.call = "reset"
  /\ flag' = FALSE
  /\ cur' = NoCur
  /\ retired' = RetiredAfter(cur.fiber, retired)
  /\ runq' = runq /\ waitq' = waitq /\ nextFiber' = nextFiber
  /\ exts' = exts
  /\ history' = Append(history, CompFib(cur.fiber, "reset"))

\* PrimStep2.runDone for `wait` with the latch set: completes inline.
RunWaitInline ==
  /\ cur # NoCur
  /\ cur.resumed = FALSE
  /\ cur.call = "wait"
  /\ flag = TRUE
  /\ cur' = NoCur
  /\ retired' = RetiredAfter(cur.fiber, retired)
  /\ flag' = flag /\ waitq' = waitq /\ runq' = runq /\ nextFiber' = nextFiber
  /\ exts' = exts
  /\ history' = Append(history, CompFib(cur.fiber, "wait"))

\* PrimStep2.runPark for `wait` on a clear flag: parks at the wait tail.
RunWaitPark ==
  /\ cur # NoCur
  /\ cur.resumed = FALSE
  /\ cur.call = "wait"
  /\ flag = FALSE
  /\ WaitInlineOnClear = FALSE
  /\ cur' = NoCur
  /\ waitq' = Append(waitq, cur.fiber)
  /\ flag' = flag /\ runq' = runq /\ retired' = retired
  /\ nextFiber' = nextFiber /\ exts' = exts /\ history' = history

\* Negative mutant (Lean `eventMutant`): wait completing inline on a clear
\* flag.  With the switch on, TLC must violate NoWaitBeforeSet.
MutantWaitInline ==
  /\ cur # NoCur
  /\ cur.resumed = FALSE
  /\ cur.call = "wait"
  /\ flag = FALSE
  /\ WaitInlineOnClear = TRUE
  /\ cur' = NoCur
  /\ retired' = RetiredAfter(cur.fiber, retired)
  /\ flag' = flag /\ waitq' = waitq /\ runq' = runq /\ nextFiber' = nextFiber
  /\ exts' = exts
  /\ history' = Append(history, CompFib(cur.fiber, "wait"))

\* PrimStep2.finishDone: a resumed waiter completes one-shot; eventPrim's
\* finish publishes nobody.
FinishWait ==
  /\ cur # NoCur
  /\ cur.resumed = TRUE
  /\ cur.call = "wait"
  /\ cur' = NoCur
  /\ retired' = RetiredAfter(cur.fiber, retired)
  /\ flag' = flag /\ waitq' = waitq /\ runq' = runq /\ nextFiber' = nextFiber
  /\ exts' = exts
  /\ history' = Append(history, CompFib(cur.fiber, "wait"))

\* PrimStep2.extApply: an external caller ENTERS a `global_mtx_`-only call.
\* The issue observation is emitted at entry and a result-less record is
\* registered; entry is independent of the worker and the runnable FIFO.
ExtApply(x, c) ==
  /\ x \in Exts
  /\ c \in ExtCalls
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {ExtRec(x, c, "entered")}
  /\ history' = Append(history, IssueExt(x, c))
  /\ flag' = flag /\ waitq' = waitq /\ cur' = cur /\ runq' = runq
  /\ retired' = retired /\ nextFiber' = nextFiber

\* PrimStep2.extEffect for `set`: the external critical section latches and
\* drains the whole wait queue, publishing every waiter (silent step).
ExtEffectSet ==
  /\ \E e \in exts :
       /\ e.call = "set"
       /\ e.phase = "entered"
       /\ exts' = (exts \ {e}) \union {ExtRec(e.x, e.call, "effected")}
  /\ flag' = TRUE
  /\ waitq' = << >>
  /\ runq' = runq \o WaiterEntries(waitq)
  /\ cur' = cur /\ retired' = retired /\ nextFiber' = nextFiber
  /\ history' = history

\* PrimStep2.extEffect for `reset`: unlatch, nobody published (silent).
ExtEffectReset ==
  /\ \E e \in exts :
       /\ e.call = "reset"
       /\ e.phase = "entered"
       /\ exts' = (exts \ {e}) \union {ExtRec(e.x, e.call, "effected")}
  /\ flag' = FALSE
  /\ waitq' = waitq /\ runq' = runq
  /\ cur' = cur /\ retired' = retired /\ nextFiber' = nextFiber
  /\ history' = history

\* PrimStep2.extDone: the physical return (completion observation).  This
\* step is deliberately unordered with respect to fiber steps: a drained
\* waiter may complete before the draining external set physically returns.
ExtDone(x, c) ==
  /\ ExtRec(x, c, "effected") \in exts
  /\ Len(history) < MaxHistory
  /\ exts' = exts \ {ExtRec(x, c, "effected")}
  /\ history' = Append(history, CompExt(x, c))
  /\ flag' = flag /\ waitq' = waitq /\ cur' = cur /\ runq' = runq
  /\ retired' = retired /\ nextFiber' = nextFiber

Next ==
  \/ \E f \in Fibers, c \in Calls : Submit(f, c)
  \/ DispatchFresh
  \/ DispatchResumed
  \/ RunSet
  \/ RunReset
  \/ RunWaitInline
  \/ RunWaitPark
  \/ MutantWaitInline
  \/ FinishWait
  \/ \E x \in Exts, c \in ExtCalls : ExtApply(x, c)
  \/ ExtEffectSet
  \/ ExtEffectReset
  \/ \E x \in Exts, c \in ExtCalls : ExtDone(x, c)

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ flag \in BOOLEAN
  /\ waitq \in Seq(Fibers)
  /\ cur \in ({NoCur} \union
       {[fiber |-> f, call |-> c, resumed |-> r] :
          f \in Fibers, c \in Calls, r \in BOOLEAN})
  /\ runq \in Seq({[fiber |-> f, call |-> c, fresh |-> fr] :
                    f \in Fibers, c \in Calls, fr \in BOOLEAN})
  /\ retired \subseteq Fibers
  /\ nextFiber \in 0..(MaxFiber + 1)
  /\ exts \subseteq {ExtRec(x, c, p) :
                      x \in Exts, c \in ExtCalls, p \in {"entered", "effected"}}
  /\ \A e1 \in exts, e2 \in exts : e1.x = e2.x => e1 = e2
  /\ history \in Seq({[type |-> t, src |-> s, id |-> i, call |-> c, result |-> res] :
                       t \in {"issue", "comp"}, s \in {"fib", "ext"},
                       i \in 0..(MaxFiber + 1), c \in Calls,
                       res \in {"none", "done"}})

IsSetIssue(o) == o.type = "issue" /\ o.call = "set"
IsWaitDone(o) == o.type = "comp" /\ o.call = "wait" /\ o.result = "done"

\* `eventPrim_guarantees` / `eventNoWaitBeforeSet` (V2.2): every wait
\* completion is preceded by a set *issue* -- fiber dispatch or external
\* entry.  (V2.1 anchored on the set completion; under V2.2 a drained
\* waiter may complete before the draining external set physically
\* returns, so the issue is the observable anchor.)
NoWaitBeforeSet ==
  \A i \in 1..Len(history) :
    IsWaitDone(history[i]) => \E j \in 1..(i-1) : IsSetIssue(history[j])

=============================================================================
