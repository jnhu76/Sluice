---------------- MODULE SemCore ----------------
(***************************************************************************)
(* Stage 2V2.3 (FCB1-METHOD-CORRECTIVE-1): the counting semaphore's        *)
(* transition system, mapping `formal/Sluice/Formal/SemV2.lean`            *)
(* (`semPrimOf`, `PrimStep2` instantiated for `SemSig`) one action per      *)
(* constructor, under the Stage-0-V2.3 calculus: a fiber-origin call's     *)
(* critical section (`fiberEffect`) is a separate silent step from its     *)
(* physical return (`fiberDone`), and the returning fiber still holds the  *)
(* worker baton -- so no second fiber may dispatch, but external callers   *)
(* may enter, run their critical section, and physically return inside     *)
(* that window.                                                            *)
(*                                                                         *)
(* Authority: `include/sluice/async/semaphore.hpp`,                        *)
(* `src/async/scheduler_semaphore.cpp`.                                    *)
(*                                                                         *)
(* Call-domain census (from the code):                                     *)
(*   acquire - fiber-only   (`sem_acquire` dereferences `g_worker` :36-37, *)
(*                           parks via `commit_suspend_locked` :61 and     *)
(*                           `context_switch` :67-71; after the switch the *)
(*                           fiber simply returns -- no re-check loop, so  *)
(*                           a resumed acquire never re-parks here)        *)
(*   release - external-capable (`sem_release` :147-161 takes              *)
(*                           `global_mtx_` only; the `LockGuard`           *)
(*                           destructor releases the mutex at section end, *)
(*                           before the caller physically returns)         *)
(*                                                                         *)
(* Fiber actions (one per `PrimStep2` constructor):                        *)
(*   FiberSubmit        - PrimStep2.submit: entry into the runnable queue  *)
(*   FiberDispatch      - PrimStep2.dispatchFresh: emits the issue         *)
(*   FiberEffect*       - PrimStep2.fiberEffect: the silent critical       *)
(*                        section -- AcqTakeFast / FibRelHandoff /         *)
(*                        FibRelStore / FibRelRefuse; the fiber enters     *)
(*                        `phase = "ret"` with its result fixed and KEEPS  *)
(*                        the baton                                        *)
(*   AcqPark            - PrimStep2.runPark: the acquire suspends instead  *)
(*                        (its `P.run` facet is none); silent, baton gone  *)
(*                        with the parked fiber                            *)
(*   FiberDone          - PrimStep2.fiberDone: the physical return, emits  *)
(*                        the completion observation, releases the baton   *)
(*   FiberResume        - PrimStep2.dispatchResumed: silent dispatch of a  *)
(*                        handoff-published acquire                        *)
(*   FinishResumed      - PrimStep2.finishDone: a resumed acquire's        *)
(*                        effect and return do not come apart (the outcome *)
(*                        was fixed by the handoff)                        *)
(*                                                                         *)
(* External actions (the three-phase sequence; `extCap release = true`):   *)
(*   ExtIssue           - PrimStep2.extApply: entry, emits the issue       *)
(*   ExtEffect*         - PrimStep2.extEffect: the silent critical section *)
(*                        (same three release branches as the fiber side)  *)
(*   ExtDone            - PrimStep2.extDone: the physical return           *)
(*                                                                         *)
(* Linearization (Stage-2 review rule): the only semantically mutually     *)
(* exclusive actions are the state-effect sections (the FiberEffect and    *)
(* ExtEffect families); each is one atomic TLA+ transition, so no two      *)
(* state effects ever overlap, while issue and done steps own no          *)
(* semaphore state and interleave freely across both windows -- fiber     *)
(* effect-to-return and external effect-to-return.                        *)
(*                                                                         *)
(* `semPrimOf`'s environment steps are omitted: `onTick` is the identity   *)
(* and `expire` is none for the semaphore.  `PrimCfg`'s `nextFiber`        *)
(* allocation is dropped: the caller set is the fixed constants Fibers and *)
(* Extss, and a fiber may re-call as soon as its previous call completed.  *)
(*                                                                         *)
(* Negative mutants (constant switches, all default FALSE):                *)
(*   MutCreatePermit   - AcqPark mints a permit while parking              *)
(*   MutLosePermit     - a granted release stores nothing                  *)
(*   MutDoubleConsume  - the acquire fast path does not decrement          *)
(*   MutFifoBypass     - a handoff wakes the queue tail, not the head      *)
(*   MutWrongFull      - a release at the ceiling increments and reports   *)
(*                       true                                              *)
(*   MutFusedReturn    - the V2.2 regression: a fresh fiber call's effect  *)
(*                       and physical return collapse into one transition  *)
(*                       (FiberEffect* + FiberDone fused).  Safety still   *)
(*                       holds (it skips states), but the V2.3 witness     *)
(*                       becomes unreachable.                              *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANT Initial,        \* constructor permits: 0 <= Initial <= Max
          Max,           \* ceiling: Max > 0
          MaxHistory,    \* fuel: bound on recorded observations
          MutCreatePermit, MutLosePermit, MutDoubleConsume,
          MutFifoBypass, MutWrongFull, MutFusedReturn

ASSUME /\ Max > 0
      /\ 0 <= Initial /\ Initial <= Max

Fibers == {"f0", "f1"}
Exts == {"e0", "e1"}
Calls == {"acq", "rel"}

VARIABLES available,   \* stored permits (SemState.available)
          waitq,       \* parked acquirers, FIFO (SemState.waitq)
          cur,         \* the worker slot [fiber, call, phase, resumed,
                       \* result]: "run" (critical section not yet run) or
                       \* "ret" (result fixed, physical return pending --
                       \* the baton is still held), or NoCur
          runq,        \* runnable entries [fiber, call, fresh]: fresh
                       \* submissions and handoff-published acquirers
          exts,        \* in-flight external records (at most one per x)
          history,     \* observation trace: issue/comp records
          granted,     \* release sections that handed off or stored a
                       \* permit (refusals are not grants)
          hit_store,   \* coverage: a stored-permit release section ran
          hit_handoff, \* coverage: a FIFO handoff section ran
          lastHead,    \* FIFO ghost: queue head at the last handoff
          lastChosen,  \* FIFO ghost: waiter the last handoff published
          wseq,        \* V2.3 witness ghost: 0..5, the seam state machine
          wf,          \* witness ghost: fiber whose storing release section
                       \* fixed step 1 (its call instance, via wlive)
          wx,          \* witness ghost: external caller that entered at
                       \* step 2
          wlive        \* witness ghost: wf's recorded call instance is
                       \* still in flight toward its physical return

vars == <<available, waitq, cur, runq, exts, history, granted,
          hit_store, hit_handoff, lastHead, lastChosen,
          wseq, wf, wx, wlive>>

NoCur == [fiber |-> "none", call |-> "none", phase |-> "off",
          resumed |-> FALSE, result |-> "none"]

\* Observations record the caller's execution domain; external callers
\* are never faked as fibers.  Release completions carry their boolean.
IssueFib(f, c) == [type |-> "issue", src |-> "fib", id |-> f,
                   call |-> c, result |-> "none"]
IssueExt(x)    == [type |-> "issue", src |-> "ext", id |-> x,
                   call |-> "rel", result |-> "none"]
CompFib(f, c, r) == [type |-> "comp", src |-> "fib", id |-> f,
                     call |-> c, result |-> r]
CompExt(x, r)  == [type |-> "comp", src |-> "ext", id |-> x,
                   call |-> "rel", result |-> r]

Init == available = Initial
  /\ waitq = << >>
  /\ cur = NoCur
  /\ runq = << >>
  /\ exts = {}
  /\ history = << >>
  /\ granted = 0
  /\ hit_store = FALSE
  /\ hit_handoff = FALSE
  /\ lastHead = "none"
  /\ lastChosen = "none"
  /\ wseq = 0
  /\ wf = "none"
  /\ wx = "none"
  /\ wlive = FALSE

Running(c) == c.phase = "run"
Returning(c) == c.phase = "ret"
Fresh(c) == c.phase = "run" /\ c.resumed = FALSE

RECURSIVE InRunq(_, _)
InRunq(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).fiber = f
       \/ InRunq(Tail(q), f)

\* PrimStep2.submit: silent; a between-calls fiber enters the runnable
\* queue.  Legal even while another fiber holds the baton.
FiberSubmit(f, c) ==
  /\ f \in Fibers /\ c \in Calls
  /\ f # cur.fiber
  /\ ~InRunq(runq, f)
  /\ \A i \in 1..Len(waitq) : waitq[i] # f
  /\ Len(history) < MaxHistory
  /\ runq' = Append(runq, [fiber |-> f, call |-> c, fresh |-> TRUE])
  /\ wlive' = IF f = wf /\ wseq >= 1 THEN FALSE ELSE wlive
  /\ UNCHANGED <<available, waitq, cur, exts, history, granted,
                 hit_store, hit_handoff, lastHead, lastChosen,
                 wseq, wf, wx>>

\* PrimStep2.dispatchFresh: emits the issue observation; requires the
\* baton free (cur = none in the Lean machine).
FiberDispatch ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = TRUE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             phase |-> "run", resumed |-> FALSE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ history' = Append(history,
      IssueFib(Head(runq).fiber, Head(runq).call))
  /\ UNCHANGED <<available, waitq, exts, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

\* PrimStep2.fiberEffect, acquire fast path (`sem_acquire` :46-54: first
\* registrant with a permit available): consume inline, result fixed,
\* enter the return window holding the baton.  Silent.
AcqTakeFast ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "acq"
  /\ waitq = << >> /\ available > 0
  /\ ~MutFusedReturn
  /\ available' = IF MutDoubleConsume THEN available ELSE available - 1
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "done"]
  /\ UNCHANGED <<waitq, runq, exts, history, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

\* PrimStep2.runPark, acquire park (:41, :61: register and suspend).
\* Silent.  Parking has no completion, so fusion does not touch it.
AcqPark ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "acq"
  /\ ~(waitq = << >> /\ available > 0)
  /\ cur' = NoCur
  /\ waitq' = Append(waitq, cur.fiber)
  /\ available' = IF MutCreatePermit THEN available + 1 ELSE available
  /\ UNCHANGED <<runq, exts, history, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

\* PrimStep2.fiberEffect, release with waiters (`sem_release` :151-153:
\* wake exactly the FIFO head, hand the permit to it, do not store).
\* Silent; the release itself enters its return window.
FibRelHandoff ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rel"
  /\ Len(waitq) > 0
  /\ ~MutFusedReturn
  /\ LET LetChosen == IF MutFifoBypass
                         THEN waitq[Len(waitq)]
                         ELSE Head(waitq)
     IN /\ waitq' = IF MutFifoBypass
                       THEN SubSeq(waitq, 1, Len(waitq) - 1)
                       ELSE Tail(waitq)
        /\ runq' = Append(runq, [fiber |-> LetChosen, call |-> "acq",
                                 fresh |-> FALSE])
        /\ granted' = granted + 1
        /\ hit_handoff' = TRUE
        /\ lastHead' = Head(waitq)
        /\ lastChosen' = LetChosen
        /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "t"]
  /\ UNCHANGED <<available, exts, history, hit_store, wseq, wf, wx, wlive>>

\* PrimStep2.fiberEffect, release storing below the ceiling
\* (:155-159).  Silent.
FibRelStore ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rel"
  /\ waitq = << >> /\ available < Max
  /\ ~MutFusedReturn
  /\ available' = IF MutLosePermit THEN available ELSE available + 1
  /\ granted' = granted + 1
  /\ hit_store' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "t"]
  /\ wseq' = IF wseq = 0 THEN 1 ELSE wseq
  /\ wf' = IF wseq = 0 THEN cur.fiber ELSE wf
  /\ wlive' = IF wseq = 0 THEN TRUE ELSE wlive
  /\ UNCHANGED <<waitq, runq, exts, history, hit_handoff,
                 lastHead, lastChosen, wx>>

\* PrimStep2.fiberEffect, release refused at the ceiling (:156-158).
\* Silent; no state change, no grant.
FibRelRefuse ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rel"
  /\ waitq = << >> /\ available >= Max
  /\ ~MutFusedReturn
  /\ available' = IF MutWrongFull THEN available + 1 ELSE available
  /\ granted' = IF MutWrongFull THEN granted + 1 ELSE granted
  /\ cur' = [cur EXCEPT !.phase = "ret",
             !.result = IF MutWrongFull THEN "t" ELSE "f"]
  /\ UNCHANGED <<waitq, runq, exts, history,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

\* PrimStep2.fiberDone: the physical return (completion observation).
\* Frees the baton.  This is the step the V2.2 fusion deleted.
WitnessStep5 ==
  wseq = 4 /\ cur # NoCur /\ cur.fiber = wf /\ cur.call = "rel"
  /\ cur.result = "t" /\ wlive

FiberDone ==
  /\ cur # NoCur /\ Returning(cur)
  /\ ~MutFusedReturn
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call, cur.result))
  /\ cur' = NoCur
  /\ wseq' = IF WitnessStep5 THEN 5 ELSE wseq
  /\ wlive' = IF WitnessStep5 THEN wlive
               ELSE IF cur.fiber = wf /\ wseq >= 1 THEN FALSE ELSE wlive
  /\ UNCHANGED <<available, waitq, runq, exts, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wf, wx>>

\* PrimStep2.dispatchResumed: silent dispatch of a handoff-published
\* acquire; requires the baton free.
FiberResume ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = FALSE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> "acq",
             phase |-> "run", resumed |-> TRUE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ UNCHANGED <<available, waitq, exts, history, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

\* PrimStep2.finishDone: a resumed acquire completes at its dispatch --
\* the handoff already fixed the outcome, so effect and return do not
\* come apart here.  Observable.
FinishResumed ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed /\ cur.call = "acq"
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, "acq", "done"))
  /\ cur' = NoCur
  /\ UNCHANGED <<available, waitq, runq, exts, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

\* MutFusedReturn: the V2.2 `runDone` regression -- a fresh fiber call's
\* critical section and physical return in ONE transition (acquire park
\* has no completion and stays separate).  The fiber never occupies a
\* "ret" state, so nothing can interleave between its effect and its
\* completion.
FusedAcqTake ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "acq"
  /\ waitq = << >> /\ available > 0
  /\ MutFusedReturn
  /\ available' = IF MutDoubleConsume THEN available ELSE available - 1
  /\ history' = Append(history, CompFib(cur.fiber, "acq", "done"))
  /\ cur' = NoCur
  /\ UNCHANGED <<waitq, runq, exts, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

FusedRelHandoff ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rel"
  /\ Len(waitq) > 0
  /\ MutFusedReturn
  /\ LET LetChosen == IF MutFifoBypass
                         THEN waitq[Len(waitq)]
                         ELSE Head(waitq)
     IN /\ waitq' = IF MutFifoBypass
                       THEN SubSeq(waitq, 1, Len(waitq) - 1)
                       ELSE Tail(waitq)
        /\ runq' = Append(runq, [fiber |-> LetChosen, call |-> "acq",
                                 fresh |-> FALSE])
        /\ granted' = granted + 1
        /\ hit_handoff' = TRUE
        /\ lastHead' = Head(waitq)
        /\ lastChosen' = LetChosen
        /\ history' = Append(history, CompFib(cur.fiber, "rel", "t"))
        /\ cur' = NoCur
  /\ UNCHANGED <<available, exts, hit_store, wseq, wf, wx, wlive>>

FusedRelStore ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rel"
  /\ waitq = << >> /\ available < Max
  /\ MutFusedReturn
  /\ available' = IF MutLosePermit THEN available ELSE available + 1
  /\ granted' = granted + 1
  /\ hit_store' = TRUE
  /\ history' = Append(history, CompFib(cur.fiber, "rel", "t"))
  /\ cur' = NoCur
  /\ wseq' = IF wseq = 0 THEN 1 ELSE wseq
  /\ wf' = IF wseq = 0 THEN cur.fiber ELSE wf
  /\ wlive' = FALSE
  /\ UNCHANGED <<waitq, runq, exts, hit_handoff,
                 lastHead, lastChosen, wx>>

FusedRelRefuse ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "rel"
  /\ waitq = << >> /\ available >= Max
  /\ MutFusedReturn
  /\ available' = IF MutWrongFull THEN available + 1 ELSE available
  /\ granted' = IF MutWrongFull THEN granted + 1 ELSE granted
  /\ history' = Append(history,
      CompFib(cur.fiber, "rel", IF MutWrongFull THEN "t" ELSE "f"))
  /\ cur' = NoCur
  /\ UNCHANGED <<waitq, runq, exts,
                 hit_store, hit_handoff, lastHead, lastChosen, wseq, wf, wx, wlive>>

\* PrimStep2.extApply: an external caller ENTERS `release`.  The issue
\* observation is emitted at entry; entry owns no semaphore state and is
\* legal while a fiber holds the baton (running or returning).
ExtIssue(x) ==
  /\ x \in Exts
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {[x |-> x, call |-> "rel",
                          phase |-> "ent", result |-> "none"]}
  /\ history' = Append(history, IssueExt(x))
  /\ wseq' = IF wseq = 1 THEN 2 ELSE wseq
  /\ wx' = IF wseq = 1 THEN x ELSE wx
  /\ UNCHANGED <<available, waitq, cur, runq, granted,
                 hit_store, hit_handoff, lastHead, lastChosen, wf, wlive>>

\* PrimStep2.extEffect, release with waiters.  Silent; the record's
\* result is fixed and the caller still owes its physical return.
ExtRelHandoff(x) ==
  /\ [x |-> x, call |-> "rel", phase |-> "ent", result |-> "none"] \in exts
  /\ Len(waitq) > 0
  /\ LET LetChosen == IF MutFifoBypass
                         THEN waitq[Len(waitq)]
                         ELSE Head(waitq)
     IN /\ waitq' = IF MutFifoBypass
                       THEN SubSeq(waitq, 1, Len(waitq) - 1)
                       ELSE Tail(waitq)
        /\ runq' = Append(runq, [fiber |-> LetChosen, call |-> "acq",
                                 fresh |-> FALSE])
        /\ granted' = granted + 1
        /\ hit_handoff' = TRUE
        /\ lastHead' = Head(waitq)
        /\ lastChosen' = LetChosen
        /\ exts' = (exts \ {[x |-> x, call |-> "rel",
                             phase |-> "ent", result |-> "none"]})
                   \union {[x |-> x, call |-> "rel",
                            phase |-> "eff", result |-> "t"]}
  /\ UNCHANGED <<available, cur, history, hit_store, wseq, wf, wx, wlive>>

\* PrimStep2.extEffect, release storing below the ceiling.  Silent.
ExtRelStore(x) ==
  /\ [x |-> x, call |-> "rel", phase |-> "ent", result |-> "none"] \in exts
  /\ waitq = << >> /\ available < Max
  /\ available' = IF MutLosePermit THEN available ELSE available + 1
  /\ granted' = granted + 1
  /\ hit_store' = TRUE
  /\ exts' = (exts \ {[x |-> x, call |-> "rel",
                       phase |-> "ent", result |-> "none"]})
             \union {[x |-> x, call |-> "rel",
                      phase |-> "eff", result |-> "t"]}
  /\ UNCHANGED <<waitq, cur, runq, history, hit_handoff,
                 lastHead, lastChosen, wseq, wf, wx, wlive>>

\* PrimStep2.extEffect, release refused at the ceiling.  Silent.
ExtRelRefuse(x) ==
  /\ [x |-> x, call |-> "rel", phase |-> "ent", result |-> "none"] \in exts
  /\ waitq = << >> /\ available >= Max
  /\ available' = IF MutWrongFull THEN available + 1 ELSE available
  /\ granted' = IF MutWrongFull THEN granted + 1 ELSE granted
  /\ exts' = (exts \ {[x |-> x, call |-> "rel",
                       phase |-> "ent", result |-> "none"]})
             \union {[x |-> x, call |-> "rel", phase |-> "eff",
                      result |-> IF MutWrongFull THEN "t" ELSE "f"]}
  /\ wseq' = IF wseq = 2 /\ x = wx
                /\ ~MutWrongFull THEN 3 ELSE wseq
  /\ UNCHANGED <<waitq, cur, runq, history,
                 hit_store, hit_handoff, lastHead, lastChosen,
                 wf, wx, wlive>>

\* PrimStep2.extDone: the physical return, deliberately unordered with
\* respect to the other steps -- a fiber may run a whole call between
\* this caller's section and its return.
ExtDone(x) ==
  /\ \E r \in {"t", "f"} :
       [x |-> x, call |-> "rel", phase |-> "eff", result |-> r] \in exts
  /\ Len(history) < MaxHistory
  /\ exts' = exts \ {[x |-> x, call |-> "rel", phase |-> "eff",
                      result |-> CHOOSE r \in {"t", "f"} :
                        [x |-> x, call |-> "rel", phase |-> "eff",
                         result |-> r] \in exts]}
  /\ history' = Append(history,
      CompExt(x, CHOOSE r \in {"t", "f"} :
        [x |-> x, call |-> "rel", phase |-> "eff", result |-> r] \in exts))
  /\ wseq' = IF wseq = 3 /\ x = wx THEN 4 ELSE wseq
  /\ UNCHANGED <<available, waitq, cur, runq, granted,
                 hit_store, hit_handoff, lastHead, lastChosen,
                 wf, wx, wlive>>

Next ==
  \/ \E f \in Fibers, c \in Calls : FiberSubmit(f, c)
  \/ FiberDispatch
  \/ AcqTakeFast
  \/ AcqPark
  \/ FibRelHandoff
  \/ FibRelStore
  \/ FibRelRefuse
  \/ FiberDone
  \/ FiberResume
  \/ FinishResumed
  \/ FusedAcqTake
  \/ FusedRelHandoff
  \/ FusedRelStore
  \/ FusedRelRefuse
  \/ \E x \in Exts : ExtIssue(x)
  \/ \E x \in Exts : ExtRelHandoff(x)
  \/ \E x \in Exts : ExtRelStore(x)
  \/ \E x \in Exts : ExtRelRefuse(x)
  \/ \E x \in Exts : ExtDone(x)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type and safety invariants                                              *)
(***************************************************************************)

TypeOK ==
  /\ available \in 0..Max
  /\ waitq \in Seq(Fibers)
  /\ cur \in ({NoCur} \union
       {[fiber |-> f, call |-> c, phase |-> p, resumed |-> r, result |-> s] :
          f \in Fibers, c \in Calls,
          p \in {"run", "ret"}, r \in BOOLEAN,
          s \in {"none", "t", "f", "done"}})
  /\ runq \in Seq({[fiber |-> f, call |-> c, fresh |-> fr] :
                    f \in Fibers, c \in Calls, fr \in BOOLEAN})
  /\ exts \subseteq {[x |-> x, call |-> "rel", phase |-> p, result |-> r] :
                      x \in Exts, p \in {"ent", "eff"}, r \in {"none", "t", "f"}}
  /\ \A e1 \in exts, e2 \in exts : e1.x = e2.x => e1 = e2
  /\ history \in Seq({[type |-> t, src |-> s, id |-> i, call |-> c,
                       result |-> r] :
                       t \in {"issue", "comp"}, s \in {"fib", "ext"},
                       i \in (Fibers \cup Exts), c \in Calls,
                       r \in {"none", "done", "t", "f"}})
  /\ granted \in Nat
  /\ hit_store \in BOOLEAN /\ hit_handoff \in BOOLEAN
  /\ lastHead \in Fibers \cup {"none"}
  /\ lastChosen \in Fibers \cup {"none"}
  /\ wseq \in 0..5
  /\ wf \in Fibers \cup {"none"}
  /\ wx \in Exts \cup {"none"}
  /\ wlive \in BOOLEAN

\* Capacity: the ceiling is never exceeded, nothing below zero.
InvCapacity == 0 <= available /\ available <= Max

RECURSIVE StaleAcq(_)
StaleAcq(q) ==
  IF q = << >> THEN 0
  ELSE (IF Head(q).fresh = FALSE THEN 1 ELSE 0) + StaleAcq(Tail(q))

RECURSIVE TakesCount(_), RelIssueCount(_)
TakesCount(h) ==
  IF h = << >> THEN 0
  ELSE (IF Head(h).type = "comp" /\ Head(h).src = "fib"
           /\ Head(h).call = "acq" /\ Head(h).result = "done"
        THEN 1 ELSE 0) + TakesCount(Tail(h))
RelIssueCount(h) ==
  IF h = << >> THEN 0
  ELSE (IF Head(h).type = "issue" /\ Head(h).call = "rel"
        THEN 1 ELSE 0) + RelIssueCount(Tail(h))

\* Permits exist in exactly four places: stored (`available`), owned by
\* a handoff-published or resumed acquire, consumed by a returning
\* acquire whose completion is not yet observed, or already observed as
\* a completed take.  Every one of them came from the constructor stock
\* or from a granted release section.  (Refused releases grant
\* nothing.)  This is the TLA form of `semBalance_mirror`, strengthened
\* to an equality: the TLA machine is C++-faithful and omits the Lean
\* calculus's resumed-repark over-approximation (`sem_acquire` :67-71
\* has no re-check loop), so exact conservation holds and create/lose
\* mutants are caught.
Holders ==
  StaleAcq(runq)
  + (IF cur # NoCur /\ Returning(cur) /\ cur.call = "acq" THEN 1 ELSE 0)
  + (IF cur # NoCur /\ cur.phase = "run" /\ cur.resumed THEN 1 ELSE 0)

InvPermitPool ==
  available + Holders + TakesCount(history) = Initial + granted

\* `semPermitsHonoredGen`: every completed take was issued a release
\* first or drew constructor stock.
InvTakeBound ==
  TakesCount(history) <= RelIssueCount(history) + Initial

\* Queue ownership: no fiber is parked twice, and a parked or queued
\* fiber is never simultaneously the dispatched one.
InvQueueNoDup ==
  /\ \A i \in 1..Len(waitq), j \in 1..Len(waitq) :
       i # j => waitq[i] # waitq[j]
  /\ \A i \in 1..Len(runq), j \in 1..Len(runq) :
       i # j => runq[i].fiber # runq[j].fiber

InvQueueOwnership ==
  /\ \A i \in 1..Len(waitq) :
       /\ ~InRunq(runq, waitq[i])
       /\ waitq[i] # cur.fiber
  /\ \A i \in 1..Len(runq) : runq[i].fiber # cur.fiber
  /\ \A i \in 1..Len(runq) :
       runq[i].fresh = FALSE => runq[i].call = "acq"

\* FIFO: a handoff publishes exactly the waiter that was the queue head
\* at that section (ghost pair updated only by handoff actions).
InvFifo == lastHead = lastChosen

SameCaller(o1, o2) == o1.src = o2.src /\ o1.id = o2.id

\* Completion discipline: every completion matches a prior issue of the
\* same call by the same caller, with no intervening completion of that
\* caller -- no completion before issue, no double completion.
InvCompDiscipline ==
  \A i \in 1..Len(history) :
    history[i].type = "comp" =>
      \E j \in 1..(i - 1) :
        /\ history[j].type = "issue"
        /\ SameCaller(history[j], history[i])
        /\ history[j].call = history[i].call
        /\ \A k \in (j + 1)..(i - 1) :
             ~SameCaller(history[k], history[i])

SafetyInvariants == <<TypeOK, InvCapacity, InvPermitPool, InvTakeBound,
                      InvQueueNoDup, InvQueueOwnership, InvFifo,
                      InvCompDiscipline>>

(***************************************************************************)
(* The V2.3 seam certificate and coverage predicates                       *)
(*                                                                         *)
(* All are monotone over `history` (and the set-once coverage flags), so   *)
(* a single TLC run reaching the conjunctive state certifies every         *)
(* component; each coverage cfg checks the negated conjunction and must    *)
(* be VIOLATED.                                                            *)
(***************************************************************************)

IsRelIssue(o) == o.type = "issue" /\ o.call = "rel"
IsCompFib(o) == o.type = "comp" /\ o.src = "fib"
IsCompExt(o) == o.type = "comp" /\ o.src = "ext"

\* The V2.3 seam certificate (`semPrim_possesses_fiberWindow`): a fiber
\* release's critical section stored the last permit (step 1, silent),
\* an external release entered after it (step 2), was refused at the
\* ceiling the fiber had just filled (step 3), and physically returned
\* (step 4) BEFORE the fiber's own physical return (step 5).  The ghost
\* `wlive` anchors steps 1 and 5 to the SAME call instance: a fiber
\* re-submitting or physically returning early kills the recording, so a
\* later call of the same fiber cannot fake the window.  The V2.2 fused
\* step cannot reach 5: the fused store fixes the result and completes
\* the call in one transition (wlive goes FALSE immediately), and no
\* FiberDone exists to take step 5.
InvWitness == wseq # 5

\* An external release's whole call straddled by a fiber completion:
\* issue(ext x) < comp(fiber) < comp(ext x).
CovExtReturnWindow ==
  \E x \in Exts, i2, i3, i4 \in 1..Len(history) :
    /\ i2 < i3 /\ i3 < i4
    /\ history[i2] = IssueExt(x)
    /\ IsCompFib(history[i3])
    /\ history[i4].type = "comp" /\ history[i4].src = "ext"
      /\ history[i4].id = x

CovAcqReturnWindow ==
  \E f \in Fibers, x \in Exts, i1, i2, i3, i4 \in 1..Len(history) :
    /\ i1 < i2 /\ i2 < i3 /\ i3 < i4
    /\ history[i1] = IssueFib(f, "acq")
    /\ history[i2] = IssueExt(x)
    /\ IsCompExt(history[i3]) /\ history[i3].id = x
    /\ history[i4] = CompFib(f, "acq", "done")

CovRelReturnWindow ==
  \E f \in Fibers, x \in Exts, i1, i2, i3, i4 \in 1..Len(history) :
    /\ i1 < i2 /\ i2 < i3 /\ i3 < i4
    /\ history[i1] = IssueFib(f, "rel")
    /\ history[i2] = IssueExt(x)
    /\ IsCompExt(history[i3]) /\ history[i3].id = x
    /\ history[i4] = CompFib(f, "rel", "t")

\* A refused release exists (ceiling reached).
CovRefuse ==
  \E i \in 1..Len(history) :
    history[i].type = "comp" /\ history[i].call = "rel"
      /\ history[i].result = "f"

\* A take with no release issue ever before it: constructor stock.
CovInitialTake ==
  \E i \in 1..Len(history) :
    /\ history[i] = CompFib(history[i].id, "acq", "done")
    /\ ~\E j \in 1..(i - 1) : IsRelIssue(history[j])

\* Two externals entered in one order but serialized in the other: the
\* first entrant refused, the later one granted.
CovExtReorder ==
  \E x, y \in Exts, i1, i2 \in 1..Len(history) :
    /\ x # y
    /\ i1 < i2
    /\ history[i1] = IssueExt(x)
    /\ history[i2] = IssueExt(y)
    /\ \E i3 \in 1..Len(history) : history[i3] = CompExt(x, "f")
    /\ \E i4 \in 1..Len(history) : history[i4] = CompExt(y, "t")

\* A parked acquire completed after a release issue: the FIFO handoff
\* chain (park -> handoff -> published -> resumed completion).  In an
\* initial = 0 configuration an acquire can only complete this way.
CovResume ==
  \E f \in Fibers, i, j, k \in 1..Len(history) :
    /\ i < j /\ j < k
    /\ history[i] = IssueFib(f, "acq")
    /\ IsRelIssue(history[j])
    /\ history[k] = CompFib(f, "acq", "done")

CovMax2 == available = 2

\* Per-run coverage conjunctions.  Each coverage cfg checks ONE negated
\* conjunction and must be VIOLATED: the violation is the reachability
\* certificate.  The conjunctions are sized so a single execution can
\* satisfy every component within the cfg's MaxHistory fuel.
\*
\*   InvCovW1        - a stored release, a refusal, a release completion
\*                     with an external call inside its window, and an
\*                     external return straddled by a fiber completion
\*   InvCovW2        - an acquire completion with an external call inside
\*                     its window
\*   InvCovQ         - a FIFO handoff, the parked-acquire resumption
\*                     chain, and two externals entering in one order but
\*                     serializing in the other
\*   InvCovQ1        - the FIFO handoff and parked-acquire resumption
\*                     chain alone (the (1,2) coverage slot)
\*   InvCovInitial   - W2 with the take drawn from constructor stock
\*   InvCovMax2      - W1 with the ceiling (available = 2) reached
\*   InvCovMax2State - the ceiling (available = 2) reached at all
CovW1 ==
  hit_store /\ CovRefuse /\ CovRelReturnWindow /\ CovExtReturnWindow
CovW2 == CovAcqReturnWindow
CovQ == hit_handoff /\ CovResume /\ CovExtReorder

\* The handoff-resume component of CovQ alone.  It is the (1,2) slot:
\* at that constructor domain the full CovQ conjunction needs a longer
\* single execution than the domain's state space can afford to search.
CovQ1 == hit_handoff /\ CovResume

InvCovW1 == ~CovW1
InvCovW2 == ~CovW2
InvCovQ == ~CovQ
InvCovQ1 == ~CovQ1
InvCovInitial == ~(CovW2 /\ CovInitialTake)
InvCovMax2 == ~(CovW1 /\ CovMax2)
InvCovMax2State == ~CovMax2

=============================================================================
