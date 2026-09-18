---------------- MODULE CondCore ----------------
(***************************************************************************)
(* Stage 4V2.3 (FCB1-POST-V23-STACK-379-385): the AsyncCondition           *)
(* primitive's transition system, mapping                                 *)
(* `formal/Sluice/Formal/ConditionV2.lean` (`condPrim`, `PrimStep2`       *)
(* instantiated for `CondSig`) one action per constructor, under the      *)
(* post-#378 Stage-0-V2.3 execution-domain calculus.                      *)
(*                                                                         *)
(* Call-domain census (from the code, scheduler_condition.cpp):           *)
(*   wait        = fiber-only, owner-gated admission (:79-80 the          *)
(*                 `assert(owner == me)`; register + release-with-        *)
(*                 handoff + suspend, reacquire on wake)                  *)
(*   notify_one  = external-capable (:145-148; wakes at most the head)    *)
(*   notify_all  = external-capable (:150-162; drains, returns the count) *)
(*   cancel      = external-capable (:164-174; publishes `cancelled`)     *)
(*                                                                         *)
(* Reachability disclosure: a `wait` is admitted only from the embedded   *)
(* slot's holder, and acquiring that slot is the caller side's mutex      *)
(* traffic (the Stage-3 surface).  `Boot` selects the initial state:      *)
(*   "prim"  - the `primInit`-faithful start (no wait is admittable)      *)
(*   "own0"  - f0 holds the embedded slot (the battery-A start)           *)
(*   "wait3" - f0, f1 parked past their release sections (the broadcast   *)
(*             battery's parked pair; the three-waiter drain lives in     *)
(*             the Lean battery B)                                        *)
(*                                                                         *)
(* Fiber actions (one per PrimStep2 constructor):                         *)
(*   FiberSubmit / FiberDispatch / FiberResume                            *)
(*   WaitPark        - runPark, cwFresh: register at the condition-queue  *)
(*                     tail and release the slot -- handoff to the mutex- *)
(*                     queue head if one waits, else free (:63-65)        *)
(*   WaitReacqPark   - runPark, cwWaiting: the Mesa reacquire parks on    *)
(*                     the mutex queue instead                            *)
(*   NotifyOneEmpty / NotifyOneTake - fiberEffect notify_one's outcomes   *)
(*   NotifyAllTake   - fiberEffect notify_all: drain, publish the count   *)
(*   CancelHit / CancelMiss - fiberEffect cancel's outcomes               *)
(*   FiberDone       - fiberDone: the physical return                     *)
(*   FinishReacq / FinishTake - finishDone's two phase branches: a        *)
(*                     cwReacq waiter completes on the handed-off slot;   *)
(*                     a cwWaiting waiter takes the free slot             *)
(*                                                                         *)
(* External actions (three-phase sequence, notify_one/notify_all/cancel): *)
(*   ExtIssue(x, ...) - extApply: entry, emits the issue observation      *)
(*   ExtNotifyOne / ExtNotifyAll / ExtCancelHit / ExtCancelMiss        *)
(*                   - extEffect: the section, silent                     *)
(*   ExtDone(x)      - extDone: the physical return                       *)
(*                                                                         *)
(* Mutants (one per Lean fault class, killed by the same evidence shape): *)
(*   MutSpurious   - M1: the resumed finish ignores the resolution record *)
(*                  (killed by InvFinishBacked, the facet kill)           *)
(*   MutDrainOne   - M2: notify_all readies only the head while still     *)
(*                  publishing the full count (under-production: no       *)
(*                  safety invariant can catch it; the separation cfg     *)
(*                  proves the broadcast witness unreachable)             *)
(*   MutNoTake     - M3: the cwWaiting reacquire skips the slot take      *)
(*                  (killed by InvFinishOwned, the safety kill)           *)
(*   MutParkHolds  - M4: the release is skipped, the slot stays held      *)
(*                  (under-production lost wakeup: witness separation)    *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANT MaxHistory,   \* fuel: bound on recorded observations
          Boot,        \* initial state: "prim" | "own0" | "wait3"
          MutSpurious, \* M1: the finish ignores the record
          MutDrainOne, \* M2: the drain readies only the head
          MutNoTake,   \* M3: the reacquire skips the slot take
          MutParkHolds \* M4: the release is skipped

ASSUME Boot \in {"prim", "own0", "wait3"}

Fibers == {"f0", "f1"}
Exts == {"e0"}
Calls == {"wait", "notify1", "notifyall", "cancel"}
Results == {"none", "t", "f", "unit"}
Ralls == {"rall0", "rall1", "rall2", "rall3"}
ResAll == Results \cup Ralls
RallTag(k) == <<"rall0", "rall1", "rall2", "rall3">>[k + 1]
RallVal(r) == IF r = "rall0" THEN 0 ELSE IF r = "rall1" THEN 1
            ELSE IF r = "rall2" THEN 2 ELSE 3

VARIABLES owner,    \* the embedded slot (CondState.owner)
          mwaitq,   \* reacquire-blocked waiters, FIFO (CondState.mwaitq)
          cwaitq,   \* the condition queue, FIFO (CondState.cwaitq)
          phase,    \* per-fiber lifecycle: "fresh" | "waiting" | "reacq"
          resolved, \* one-shot outcomes (CondState.resolved): [f, b]
          cur,      \* the worker slot; "run" | "ret" | NoCur
          runq,     \* runnable entries [fiber, call, w, fresh]
          exts,     \* in-flight external records
          history,  \* observation trace
          hit_park_free,  \* coverage: a release-with-free-slot section ran
          hit_handoff,    \* coverage: a release handoff to a mutex waiter
          hit_notify_empty, \* coverage: notify_one on an empty queue
          hit_cancel_hit, \* coverage: a cancel of a queued waiter ran
          hit_cancel_miss, \* coverage: a cancel of a non-waiter ran
          hit_resume_t,   \* coverage: a resumed waiter completed true
          hit_resume_f,   \* coverage: a resumed waiter completed false
          hit_reacq_fin,  \* coverage: a cwReacq waiter completed
          bcast_ready,  \* ghost: waiters readied by the last notify_all
          fin_backed,   \* ghost: every finish consumed its record
          fin_owned     \* ghost: every cwWaiting finish left the caller
                        \* owning the slot

vars == <<owner, mwaitq, cwaitq, phase, resolved, cur, runq, exts,
          history, hit_park_free, hit_handoff, hit_notify_empty,
          hit_cancel_hit, hit_cancel_miss, hit_resume_t, hit_resume_f,
          hit_reacq_fin, bcast_ready, fin_backed, fin_owned>>

NoCur == [fiber |-> "none", call |-> "none", w |-> "none",
          phase |-> "off", resumed |-> FALSE, result |-> "none"]

AllFresh == [f \in Fibers |-> "fresh"]

IssueFib(f, c) == [type |-> "issue", src |-> "fib", id |-> f,
                   call |-> c, result |-> "none"]
IssueExt(x, c) == [type |-> "issue", src |-> "ext", id |-> x,
                   call |-> c, result |-> "none"]
CompFib(f, c, r) == [type |-> "comp", src |-> "fib", id |-> f,
                     call |-> c, result |-> r]
CompExt(x, c, r) == [type |-> "comp", src |-> "ext", id |-> x,
                     call |-> c, result |-> r]

Running(c) == c.phase = "run"
Returning(c) == c.phase = "ret"
Fresh(c) == c.phase = "run" /\ c.resumed = FALSE

Init ==
  /\ owner = IF Boot = "own0" THEN "f0" ELSE "none"
  /\ mwaitq = << >>
  /\ cwaitq = IF Boot = "wait3" THEN <<"f0", "f1">> ELSE << >>
  /\ phase = IF Boot = "wait3" THEN [f \in Fibers |-> "waiting"]
                               ELSE AllFresh
  /\ resolved = << >>
  /\ cur = NoCur
  /\ runq = << >>
  /\ exts = {}
  /\ history = << >>
  /\ hit_park_free = FALSE
  /\ hit_handoff = FALSE
  /\ hit_notify_empty = FALSE
  /\ hit_cancel_hit = FALSE
  /\ hit_cancel_miss = FALSE
  /\ hit_resume_t = FALSE
  /\ hit_resume_f = FALSE
  /\ hit_reacq_fin = FALSE
  /\ bcast_ready = 0
  /\ fin_backed = TRUE
  /\ fin_owned = TRUE

RECURSIVE InRunq(_, _), InCwaitq(_, _), RemoveFiber(_, _),
          ConsumeRec(_, _)
InRunq(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).fiber = f
       \/ InRunq(Tail(q), f)

InCwaitq(q, w) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q) = w
       \/ InCwaitq(Tail(q), w)

\* removeFiber: drop the FIRST occurrence of w (WaitQueue::unlink).
RemoveFiber(s, w) ==
  IF s = << >> THEN << >>
  ELSE IF Head(s) = w THEN Tail(s)
  ELSE <<Head(s)>> \o RemoveFiber(Tail(s), w)

\* consumeRec: consume the FIRST record of f, yielding its outcome and
\* the remaining records; `found` is FALSE when f has no record.
ConsumeRec(f, s) ==
  IF s = << >> THEN [found |-> FALSE, b |-> FALSE, rest |-> << >>]
  ELSE IF Head(s).f = f
    THEN [found |-> TRUE, b |-> Head(s).b, rest |-> Tail(s)]
    ELSE LET tl == ConsumeRec(f, Tail(s))
         IN [found |-> tl.found, b |-> tl.b,
             rest |-> IF tl.found THEN <<Head(s)>> \o tl.rest ELSE s]

\* condAdmit: only the slot holder may wait (the caller precondition);
\* notify and cancel have none.
Admits(c, f) == IF c = "wait" THEN owner = f ELSE TRUE

\* The wake a notify_one section publishes: the head (if any) and the
\* record it leaves.
ReadyHead ==
  /\ Len(cwaitq) > 0
  /\ cwaitq' = Tail(cwaitq)
  /\ resolved' = Append(resolved, [f |-> Head(cwaitq), b |-> TRUE])
  /\ runq' = Append(runq, [fiber |-> Head(cwaitq), call |-> "wait",
                           w |-> "none", fresh |-> FALSE])

\* PrimStep2.submit: silent; a between-calls fiber enters the runnable
\* queue.  `w` is cancel's target ("none" otherwise).
FiberSubmit(f, c, w) ==
  /\ f \in Fibers /\ c \in Calls
  /\ c # "cancel" \/ w \in Fibers
  /\ f # cur.fiber
  /\ ~InRunq(runq, f)
  /\ \A i \in 1..Len(mwaitq) : mwaitq[i] # f
  /\ ~InCwaitq(cwaitq, f)
  /\ Len(history) < MaxHistory
  /\ runq' = Append(runq, [fiber |-> f, call |-> c, w |-> w,
                           fresh |-> TRUE])
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, cur, exts,
                 history, hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, bcast_ready, fin_backed,
                 fin_owned>>

\* PrimStep2.dispatchFresh: emits the issue observation; requires the
\* baton free and the admission gate.
FiberDispatch ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = TRUE
  /\ Admits(Head(runq).call, Head(runq).fiber)
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             w |-> Head(runq).w, phase |-> "run", resumed |-> FALSE,
             result |-> "none"]
  /\ runq' = Tail(runq)
  /\ history' = Append(history,
      IssueFib(Head(runq).fiber, Head(runq).call))
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, exts,
                 hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, bcast_ready, fin_backed,
                 fin_owned>>

\* PrimStep2.runPark, cwFresh: the section registers the waiter and
\* releases the slot -- handoff to the mutex-queue head if one waits,
\* else free (scheduler_condition.cpp:63-65).  Silent.  MutParkHolds
\* skips the release.
WaitPark ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "wait"
  /\ phase[cur.fiber] = "fresh"
  /\ phase' = [phase EXCEPT ![cur.fiber] = "waiting"]
  /\ IF mwaitq = << >>
       THEN /\ owner' = IF MutParkHolds THEN owner ELSE "none"
            /\ mwaitq' = mwaitq
            /\ runq' = runq
            /\ resolved' = resolved
            /\ hit_park_free' = TRUE
            /\ hit_handoff' = hit_handoff
       ELSE /\ owner' = Head(mwaitq)
            /\ mwaitq' = Tail(mwaitq)
            /\ runq' = Append(runq, [fiber |-> Head(mwaitq),
                                     call |-> "wait", w |-> "none",
                                     fresh |-> FALSE])
            /\ resolved' = resolved
            /\ hit_park_free' = hit_park_free
            /\ hit_handoff' = TRUE
  /\ cwaitq' = Append(cwaitq, cur.fiber)
  /\ cur' = NoCur
  /\ UNCHANGED <<exts, history, hit_notify_empty, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 hit_reacq_fin, bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.runPark, cwWaiting: the Mesa reacquire parks on the mutex
\* queue.  The second suspension of the same call: a RESUMED waiter
\* whose finish is blocked by the held slot re-parks here (runPark has
\* no freshness premise in the calculus).  Silent.
WaitReacqPark ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed /\ cur.call = "wait"
  /\ phase[cur.fiber] = "waiting"
  /\ mwaitq' = Append(mwaitq, cur.fiber)
  /\ phase' = [phase EXCEPT ![cur.fiber] = "reacq"]
  /\ cur' = NoCur
  /\ UNCHANGED <<owner, cwaitq, resolved, runq, exts, history,
                 hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, bcast_ready, fin_backed,
                 fin_owned>>

\* PrimStep2.fiberEffect, notify_one on an empty queue (runit).
NotifyOneEmpty ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "notify1"
  /\ cwaitq = << >>
  /\ hit_notify_empty' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, runq, exts,
                 history, hit_park_free, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 hit_reacq_fin, bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.fiberEffect, notify_one with a waiter (:145-148): resolve
\* the head, publish it.
NotifyOneTake ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "notify1"
  /\ Len(cwaitq) > 0
  /\ ReadyHead
  /\ hit_notify_empty' = hit_notify_empty
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<owner, mwaitq, phase, exts, history, hit_park_free,
                 hit_handoff, hit_cancel_hit, hit_cancel_miss,
                 hit_resume_t, hit_resume_f, hit_reacq_fin,
                 bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.fiberEffect, notify_all (:150-162): drain the queue,
\* resolve every waiter, publish the count.  MutDrainOne readies only
\* the head while still publishing the full count.
NotifyAllTake ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "notifyall"
  /\ cwaitq # << >>
  /\ resolved' = IF MutDrainOne
       THEN Append(resolved, [f |-> Head(cwaitq), b |-> TRUE])
       ELSE resolved \o
            [i \in 1..Len(cwaitq) |-> [f |-> cwaitq[i], b |-> TRUE]]
  /\ cwaitq' = IF MutDrainOne THEN Tail(cwaitq) ELSE << >>
  /\ runq' = IF MutDrainOne
       THEN Append(runq, [fiber |-> Head(cwaitq), call |-> "wait",
                          w |-> "none", fresh |-> FALSE])
       ELSE runq \o
            [i \in 1..Len(cwaitq) |-> [fiber |-> cwaitq[i],
                                       call |-> "wait", w |-> "none",
                                       fresh |-> FALSE]]
  /\ bcast_ready' = IF MutDrainOne THEN 1 ELSE Len(cwaitq)
  /\ cur' = [cur EXCEPT !.phase = "ret",
             !.result = RallTag(Len(cwaitq))]
  /\ UNCHANGED <<owner, mwaitq, phase, exts, history, hit_park_free,
                 hit_handoff, hit_notify_empty, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 hit_reacq_fin, fin_backed, fin_owned>>

\* PrimStep2.fiberEffect, notify_all on an empty queue (rall 0).
NotifyAllEmpty ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "notifyall"
  /\ cwaitq = << >>
  /\ bcast_ready' = 0
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "rall0"]
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, runq, exts,
                 history, hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, fin_backed, fin_owned>>

\* PrimStep2.fiberEffect, cancel of a queued waiter (:164-174): resolve
\* cancelled, unlink, publish; the cancel caller's result is TRUE.
\* (The waiter's own outcome is the FALSE record.)
CancelHit ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "cancel"
  /\ InCwaitq(cwaitq, cur.w)
  /\ cwaitq' = RemoveFiber(cwaitq, cur.w)
  /\ resolved' = Append(resolved, [f |-> cur.w, b |-> FALSE])
  /\ runq' = Append(runq, [fiber |-> cur.w, call |-> "wait",
                           w |-> "none", fresh |-> FALSE])
  /\ hit_cancel_hit' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "t"]
  /\ UNCHANGED <<owner, mwaitq, phase, exts, history, hit_park_free,
                 hit_handoff, hit_notify_empty, hit_cancel_miss,
                 hit_resume_t, hit_resume_f, hit_reacq_fin,
                 bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.fiberEffect, cancel of a non-waiter: plain false.
CancelMiss ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "cancel"
  /\ ~InCwaitq(cwaitq, cur.w)
  /\ hit_cancel_miss' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "f"]
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, runq, exts,
                 history, hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_resume_t, hit_resume_f,
                 hit_reacq_fin, bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.fiberDone: the physical return (completion observation).
FiberDone ==
  /\ cur # NoCur /\ Returning(cur)
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call, cur.result))
  /\ cur' = NoCur
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, runq, exts,
                 hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, bcast_ready, fin_backed,
                 fin_owned>>

\* PrimStep2.dispatchResumed: silent dispatch of a published waiter.
FiberResume ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = FALSE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> "wait",
             w |-> "none", phase |-> "run", resumed |-> TRUE,
             result |-> "none"]
  /\ runq' = Tail(runq)
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, exts, history,
                 hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, bcast_ready, fin_backed,
                 fin_owned>>

\* PrimStep2.finishDone, cwReacq: the handoff brought the slot, so the
\* call completes with its recorded outcome (no take).
FinishReacq ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed /\ cur.call = "wait"
  /\ phase[cur.fiber] = "reacq"
  /\ (ConsumeRec(cur.fiber, resolved)).found
  /\ Len(history) < MaxHistory
  /\ LET c == ConsumeRec(cur.fiber, resolved)
     IN /\ history' = Append(history,
            CompFib(cur.fiber, "wait", IF c.b THEN "t" ELSE "f"))
        /\ resolved' = c.rest
        /\ phase' = [phase EXCEPT ![cur.fiber] = "fresh"]
  /\ hit_reacq_fin' = TRUE
  /\ cur' = NoCur
  /\ UNCHANGED <<owner, mwaitq, cwaitq, runq, exts, hit_park_free,
                 hit_handoff, hit_notify_empty, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.finishDone, cwWaiting: the reacquire -- consume the record
\* and take the free slot (:91-130).  MutNoTake skips the take;
\* MutSpurious ignores the record entirely.
FinishTake ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed /\ cur.call = "wait"
  /\ phase[cur.fiber] = "waiting"
  /\ owner = "none"
  /\ \/ /\ ~MutSpurious
        /\ (ConsumeRec(cur.fiber, resolved)).found
     \/ MutSpurious
  /\ Len(history) < MaxHistory
  /\ LET c == ConsumeRec(cur.fiber, resolved)
     IN /\ history' = Append(history,
            CompFib(cur.fiber, "wait", IF MutSpurious THEN "t"
                                       ELSE IF c.b THEN "t" ELSE "f"))
        /\ resolved' = IF MutSpurious THEN resolved ELSE c.rest
        /\ phase' = [phase EXCEPT ![cur.fiber] = "fresh"]
        /\ owner' = IF MutNoTake THEN owner ELSE cur.fiber
  /\ fin_backed' = IF MutSpurious THEN FALSE ELSE fin_backed
  /\ fin_owned' = IF MutNoTake THEN FALSE ELSE fin_owned
  /\ hit_resume_t' = IF MutSpurious \/ (ConsumeRec(cur.fiber, resolved)).b
                       THEN TRUE ELSE hit_resume_t
  /\ hit_resume_f' = IF ~MutSpurious
                        /\ ~(ConsumeRec(cur.fiber, resolved)).b
                       THEN TRUE ELSE hit_resume_f
  /\ cur' = NoCur
  /\ UNCHANGED <<mwaitq, cwaitq, runq, exts, hit_park_free,
                 hit_handoff, hit_notify_empty, hit_cancel_hit,
                 hit_cancel_miss, hit_reacq_fin, bcast_ready>>

\* PrimStep2.extApply: an external caller ENTERS a call.  The issue
\* observation is emitted at entry; entry owns no condition state.
ExtIssue(x, c, w) ==
  /\ x \in Exts /\ c \in {"notify1", "notifyall", "cancel"}
  /\ c # "cancel" \/ w \in Fibers
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {[x |-> x, call |-> c, w |-> w,
                          phase |-> "ent", result |-> "none"]}
  /\ history' = Append(history, IssueExt(x, c))
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, cur, runq,
                 hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, bcast_ready, fin_backed,
                 fin_owned>>

\* PrimStep2.extEffect, notify_one (silent).
ExtNotifyOne ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "notify1"
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "unit"]}
       /\ IF cwaitq = << >>
            THEN /\ hit_notify_empty' = TRUE
                 /\ cwaitq' = cwaitq
                 /\ resolved' = resolved
                 /\ runq' = runq
            ELSE /\ ReadyHead
                 /\ hit_notify_empty' = hit_notify_empty
  /\ UNCHANGED <<owner, mwaitq, phase, cur, history, hit_park_free,
                 hit_handoff, hit_cancel_hit, hit_cancel_miss,
                 hit_resume_t, hit_resume_f, hit_reacq_fin,
                 bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.extEffect, notify_all (silent; the drain, as NotifyAllTake).
ExtNotifyAll ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "notifyall"
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff",
                          result |-> RallTag(Len(cwaitq))]}
       /\ IF cwaitq = << >>
            THEN /\ resolved' = resolved
                 /\ cwaitq' = cwaitq
                 /\ runq' = runq
                 /\ bcast_ready' = 0
            ELSE /\ resolved' = IF MutDrainOne
                      THEN Append(resolved,
                            [f |-> Head(cwaitq), b |-> TRUE])
                      ELSE resolved \o
                           [i \in 1..Len(cwaitq) |-> [f |-> cwaitq[i],
                                                      b |-> TRUE]]
                 /\ cwaitq' = IF MutDrainOne THEN Tail(cwaitq)
                                              ELSE << >>
                 /\ runq' = IF MutDrainOne
                      THEN Append(runq,
                            [fiber |-> Head(cwaitq), call |-> "wait",
                             w |-> "none", fresh |-> FALSE])
                      ELSE runq \o
                           [i \in 1..Len(cwaitq) |->
                              [fiber |-> cwaitq[i], call |-> "wait",
                               w |-> "none", fresh |-> FALSE]]
                 /\ bcast_ready' = IF MutDrainOne THEN 1
                                                  ELSE Len(cwaitq)
  /\ UNCHANGED <<owner, mwaitq, phase, cur, history, hit_park_free,
                 hit_handoff, hit_notify_empty, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 hit_reacq_fin, fin_backed, fin_owned>>

\* PrimStep2.extEffect, cancel of a queued waiter (silent).
ExtCancelHit ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "cancel"
       /\ InCwaitq(cwaitq, e.w)
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "t"]}
       /\ cwaitq' = RemoveFiber(cwaitq, e.w)
       /\ resolved' = Append(resolved, [f |-> e.w, b |-> FALSE])
       /\ runq' = Append(runq, [fiber |-> e.w, call |-> "wait",
                                w |-> "none", fresh |-> FALSE])
       /\ hit_cancel_hit' = TRUE
  /\ UNCHANGED <<owner, mwaitq, phase, cur, history, hit_park_free,
                 hit_handoff, hit_notify_empty, hit_cancel_miss,
                 hit_resume_t, hit_resume_f, hit_reacq_fin,
                 bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.extEffect, cancel of a non-waiter (silent; no state change).
ExtCancelMiss ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "cancel"
       /\ ~InCwaitq(cwaitq, e.w)
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, call |-> e.call, w |-> e.w,
                          phase |-> "eff", result |-> "f"]}
       /\ hit_cancel_miss' = TRUE
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, runq, cur,
                 history, hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_resume_t, hit_resume_f,
                 hit_reacq_fin, bcast_ready, fin_backed, fin_owned>>

\* PrimStep2.extDone: the physical return, deliberately unordered with
\* respect to the other steps.
ExtDone(x) ==
  /\ \E e \in exts : e.x = x /\ e.phase = "eff"
  /\ Len(history) < MaxHistory
  /\ LET e == CHOOSE e \in exts : e.x = x /\ e.phase = "eff"
     IN /\ exts' = exts \ {e}
        /\ history' = Append(history, CompExt(x, e.call, e.result))
  /\ UNCHANGED <<owner, mwaitq, cwaitq, phase, resolved, cur, runq,
                 hit_park_free, hit_handoff, hit_notify_empty,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, hit_reacq_fin, bcast_ready, fin_backed,
                 fin_owned>>

Next ==
  \/ \E f \in Fibers, c \in Calls, w \in Fibers \cup {"none"} :
       FiberSubmit(f, c, w)
  \/ FiberDispatch
  \/ WaitPark
  \/ WaitReacqPark
  \/ NotifyOneEmpty
  \/ NotifyOneTake
  \/ NotifyAllTake
  \/ NotifyAllEmpty
  \/ CancelHit
  \/ CancelMiss
  \/ FiberDone
  \/ FiberResume
  \/ FinishReacq
  \/ FinishTake
  \/ \E x \in Exts, c \in {"notify1", "notifyall", "cancel"},
       w \in Fibers \cup {"none"} : ExtIssue(x, c, w)
  \/ ExtNotifyOne
  \/ ExtNotifyAll
  \/ ExtCancelHit
  \/ ExtCancelMiss
  \/ \E x \in Exts : ExtDone(x)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type and safety invariants                                              *)
(***************************************************************************)

TypeOK ==
  /\ owner \in Fibers \cup {"none"}
  /\ mwaitq \in Seq(Fibers)
  /\ cwaitq \in Seq(Fibers)
  /\ phase \in [Fibers -> {"fresh", "waiting", "reacq"}]
  /\ resolved \in Seq({[f |-> g, b |-> bo] : g \in Fibers, bo \in BOOLEAN})
  /\ cur \in ({NoCur} \union
       {[fiber |-> f, call |-> c, w |-> w, phase |-> p, resumed |-> r,
         result |-> s] :
          f \in Fibers, c \in Calls, w \in Fibers \cup {"none"},
          p \in {"run", "ret"}, r \in BOOLEAN,
          s \in ResAll})
  /\ runq \in Seq({[fiber |-> f, call |-> c, w |-> w, fresh |-> fr] :
                    f \in Fibers, c \in Calls,
                    w \in Fibers \cup {"none"}, fr \in BOOLEAN})
  /\ exts \subseteq {[x |-> x, call |-> c, w |-> w, phase |-> p,
                      result |-> r] :
                      x \in Exts,
                      c \in {"notify1", "notifyall", "cancel"},
                      w \in Fibers \cup {"none"},
                      p \in {"ent", "eff"},
                      r \in ResAll}
  /\ \A e1 \in exts, e2 \in exts : e1.x = e2.x => e1 = e2
  /\ history \in Seq({[type |-> t, src |-> s, id |-> i, call |-> c,
                       result |-> r] :
                       t \in {"issue", "comp"}, s \in {"fib", "ext"},
                       i \in (Fibers \cup Exts), c \in Calls,
                       r \in ResAll})
  /\ hit_park_free \in BOOLEAN /\ hit_handoff \in BOOLEAN
  /\ hit_notify_empty \in BOOLEAN
  /\ hit_cancel_hit \in BOOLEAN /\ hit_cancel_miss \in BOOLEAN
  /\ hit_resume_t \in BOOLEAN /\ hit_resume_f \in BOOLEAN
  /\ hit_reacq_fin \in BOOLEAN
  /\ bcast_ready \in 0..2
  /\ fin_backed \in BOOLEAN /\ fin_owned \in BOOLEAN

RECURSIVE WakeCredit(_), WaitCompCount(_)
\* The frozen credit rules: a notify_one completion credits at most one
\* wake, a notify_all completion credits exactly its drained count (the
\* count its result observation carries), a cancel completion credits
\* one iff it hit (result t), a cancel miss credits nothing.
WakeCredit(h) ==
  IF h = << >> THEN 0
  ELSE LET o == Head(h)
       IN (IF o.type = "comp" /\ o.call = "notify1" /\ o.result = "unit"
           THEN 1 ELSE 0)
        + (IF o.type = "comp" /\ o.call = "notifyall"
           /\ o.result \in Ralls THEN RallVal(o.result) ELSE 0)
        + (IF o.type = "comp" /\ o.call = "cancel" /\ o.result = "t"
           THEN 1 ELSE 0)
        + WakeCredit(Tail(h))

WaitCompCount(h) ==
  IF h = << >> THEN 0
  ELSE (IF Head(h).type = "comp" /\ Head(h).src = "fib"
           /\ Head(h).call = "wait"
           /\ Head(h).result \in {"t", "f"}
        THEN 1 ELSE 0) + WaitCompCount(Tail(h))

\* The in-window credit (the frozen accounting): a resolver's effect
\* already publishes its wakes, so from the effect to the physical
\* return the credit sits in the window -- the returning worker's fixed
\* result, and external records past their section (phase "eff") -- and
\* moves into the completed observations at the return.  No credit at
\* entry (phase "ent") or at issue.  A pending-to-completed transition
\* only moves credit between locations; it never double-counts.
ExtWk(e) ==
  IF e.phase = "eff" /\ e.call = "notify1" THEN 1
  ELSE IF e.phase = "eff" /\ e.call = "notifyall" THEN RallVal(e.result)
  ELSE IF e.phase = "eff" /\ e.call = "cancel" /\ e.result = "t" THEN 1
  ELSE 0

RECURSIVE SetSum(_)
SetSum(S) ==
  IF S = {} THEN 0
  ELSE LET e == CHOOSE e \in S : TRUE
       IN ExtWk(e) + SetSum(S \ {e})

RetWk(c) ==
  IF c # NoCur /\ Returning(c) /\ c.call = "notify1"
       /\ c.result = "unit" THEN 1
  ELSE IF c # NoCur /\ Returning(c) /\ c.call = "notifyall"
       /\ c.result \in Ralls THEN RallVal(c.result)
  ELSE IF c # NoCur /\ Returning(c) /\ c.call = "cancel"
       /\ c.result = "t" THEN 1
  ELSE 0

\* No spurious completion (`condWakeBacked`): at every prefix, completed
\* waits are covered by the wakes their resolvers have published --
\* completed resolver observations plus the in-window credit.
InvWakeBacked ==
  WaitCompCount(history) <= WakeCredit(history) + SetSum(exts) + RetWk(cur)

\* Finish disciplines (the mutant kills): every completed wait consumed
\* a record, and a cwWaiting completion left the caller owning the slot.
InvFinishBacked == fin_backed
InvFinishOwned == fin_owned

InvQueueNoDup ==
  /\ \A i \in 1..Len(cwaitq), j \in 1..Len(cwaitq) :
       i # j => cwaitq[i] # cwaitq[j]
  /\ \A i \in 1..Len(mwaitq), j \in 1..Len(mwaitq) :
       i # j => mwaitq[i] # mwaitq[j]
  /\ \A i \in 1..Len(runq), j \in 1..Len(runq) :
       i # j => runq[i].fiber # runq[j].fiber

\* Queue ownership: one registration per fiber; parked, mutex-blocked,
\* and dispatched are disjoint; the slot holder is never parked or
\* queued; only published waiters sit stale in the runnable queue.
InvQueueOwnership ==
  /\ \A i \in 1..Len(cwaitq) :
       /\ ~InRunq(runq, cwaitq[i])
       /\ cwaitq[i] # cur.fiber
       /\ owner # cwaitq[i]
       /\ phase[cwaitq[i]] = "waiting"
  /\ \A i \in 1..Len(mwaitq) :
       /\ ~InRunq(runq, mwaitq[i])
       /\ mwaitq[i] # cur.fiber
       /\ phase[mwaitq[i]] = "reacq"
  /\ \A i \in 1..Len(runq) : runq[i].fiber # cur.fiber
  /\ \A i \in 1..Len(runq) :
       runq[i].fresh = FALSE => runq[i].call = "wait"

SameCaller(o1, o2) == o1.src = o2.src /\ o1.id = o2.id

\* Completion discipline: every completion matches a prior issue of the
\* same call by the same caller, with no intervening completion of that
\* caller -- EXCEPT the boot waiters' single pre-Init call instance (the
\* instrumented configurations start with waiters parked past their
\* issues, the documented battery-B disclosure); such a completion has
\* no in-trace issue of that call up to its own position (the exception
\* is prefix-scoped, so the invariant is monotone).
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

SafetyInvariants == <<TypeOK, InvWakeBacked, InvFinishBacked,
                      InvFinishOwned, InvQueueNoDup, InvQueueOwnership,
                      InvCompDiscipline>>

(***************************************************************************)
(* Coverage predicates and the witness separations                         *)
(*                                                                         *)
(* All are monotone (history append or set-once flags/ghost counters),     *)
(* so a single TLC run reaching the conjunctive state certifies every      *)
(* component; each coverage cfg checks the negated conjunction and must    *)
(* be VIOLATED.  The mutant separation cfgs assert the witness is          *)
(* UNreachable and must HOLD.                                              *)
(***************************************************************************)

\* Battery A: a slot-holder's release freed the slot and a resumed
\* waiter completed true through it.
CovFastPath == hit_park_free /\ hit_resume_t

\* Battery B: a notify_all readied two or more waiters (the drain) and
\* a resumed waiter completed (the Mesa window).
CovBroadcast == bcast_ready >= 2 /\ hit_resume_t

\* Battery C: a cancel of a queued waiter and the cancelled waiter's
\* completion false.
CovCancel == hit_cancel_hit /\ hit_resume_f

\* The Mesa handoff chain: a release handed the slot to a mutex-blocked
\* waiter and that waiter completed through its cwReacq finish.
CovHandoff == hit_handoff /\ hit_reacq_fin

\* The empty-surface corners.
CovNotifyEmpty == hit_notify_empty
CovCancelMiss == hit_cancel_miss

\* The witness separations: under MutDrainOne no drain readies two
\* waiters; under MutParkHolds no freed-slot fast path completes.
NotBcastWitness == ~CovBroadcast
NotFastPathWitness == ~CovFastPath

\* A state constraint for the witness cfgs: prune every behavior that
\* leaves the witness's own call mix (one external resolver kind, wait
\* submissions only).  A constraint can only SHRINK the explored space,
\* so a violation found under it is still a witness; the PASS cfgs run
\* unconstrained.
WitConstraint ==
  (\A e \in exts : e.call # "cancel") /\ (\A i \in 1..Len(runq) : runq[i].call = "wait")

CancelConstraint ==
  (\A e \in exts : e.call = "cancel") /\ (\A i \in 1..Len(runq) : runq[i].call = "wait")

InvCovFastPath == ~CovFastPath
InvCovBroadcast == ~CovBroadcast
InvCovCancel == ~CovCancel
InvCovHandoff == ~CovHandoff
InvCovNotifyEmpty == ~CovNotifyEmpty
InvCovCancelMiss == ~CovCancelMiss

=============================================================================
