---------------- MODULE MutexCore ----------------
(***************************************************************************)
(* Stage 3V2.3 (FCB1-POST-V23-STACK-379-385): the AsyncMutex primitive's   *)
(* transition system, mapping `formal/Sluice/Formal/MutexV2.lean`          *)
(* (`mutexPrim`, `PrimStep2` instantiated for `MutexSig`) one action per   *)
(* constructor, under the post-#378 Stage-0-V2.3 execution-domain         *)
(* calculus.                                                               *)
(*                                                                         *)
(* Call-domain census (from the code, scheduler_mutex.cpp):               *)
(*   lock   = fiber-only   (:38-40, parks via commit_suspend_locked,      *)
(*                         inline grant exactly when the fresh node       *)
(*                         lands at the head of an empty queue with the   *)
(*                         owner slot free :51-58)                        *)
(*   unlock = fiber-only, owner-gated caller precondition (:188)          *)
(*   try    = fiber-only   (:22-33; grants only when free and no waiter   *)
(*                         is queued; a recursive attempt is false :27)   *)
(*   cancel = external-capable (:145-156; `global_mtx_` and the queue     *)
(*                         mutex only, no `g_worker`)                     *)
(*                                                                         *)
(* Fiber actions (one per PrimStep2 constructor):                         *)
(*   FiberSubmit / FiberDispatch / FiberResume                            *)
(*   LockTake      - fiberEffect lock, inline grant (admit refused a      *)
(*                   recursive attempt; run grants only a free owner      *)
(*                   slot and an empty queue)                             *)
(*   TryTake / TryRefuse - fiberEffect try's two outcomes                 *)
(*   LockPark      - runPark: the lock suspends at the queue tail         *)
(*   UnlockHandoff / UnlockFree - fiberEffect unlock's two branches:      *)
(*                   hand the mutex to the FIFO head (recording the       *)
(*                   winner's one-shot outcome and publishing it), or     *)
(*                   free the slot.  Each enters phase = "ret" keeping    *)
(*                   the baton                                             *)
(*   FiberDone     - fiberDone: the physical return, emits the completion *)
(*   FinishResumed - finishDone: a resumed locker consumes its recorded   *)
(*                   outcome (true = handed-off grant, false = cancelled) *)
(*                                                                         *)
(* External actions (the three-phase sequence, `cancel` only):            *)
(*   ExtIssue(x, w)  - extApply: entry, emits the issue observation       *)
(*   ExtCancelHit    - extEffect: w is parked: unlink it, record          *)
(*                     (w, false), publish it runnable                    *)
(*   ExtCancelMiss   - extEffect: w is not a waiter: plain false          *)
(*   ExtDone(x)      - extDone: the physical return                       *)
(*                                                                         *)
(* The Lean model's disclosed over-approximation (a resumed cancelled     *)
(* locker re-running inline paths) is absent here, as in Stage 2: this    *)
(* machine is C++-faithful and the conservation invariant is the exact    *)
(* equality.  `onTick` is the identity and `expire` is none; both are     *)
(* omitted.                                                                *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANT MaxHistory,     \* fuel: bound on recorded observations
          MutGrantHeld,  \* mutant: the inline lock grant ignores the owner
                         \* slot (fault class: ownership gating)
          MutHandoffOwner, \* mutant: the handoff forgets to transfer the
                         \* owner slot to the queue head
          MutCancelTrue, \* mutant: cancel resolves the waiter with the
                         \* granted outcome instead of the cancelled one
          MutFusedReturn \* mutant: the V2.2 regression -- a fresh fiber
                         \* call's critical section and physical return in
                         \* ONE transition

ASSUME MaxHistory \in Nat

Fibers == {"f0", "f1"}
Exts == {"e0", "e1"}
Calls == {"lock", "try", "unlock"}
Results == {"none", "t", "f", "unit"}

VARIABLES owner,     \* the owner slot (MutexState.owner), "none" when free
          waitq,     \* parked lockers, FIFO (MutexState.waitq)
          cur,       \* the worker slot [fiber, call, phase, resumed,
                     \* result]: "run" (critical section not yet run) or
                     \* "ret" (result fixed, physical return pending --
                     \* the baton is still held), or NoCur
          resolved,  \* one-shot WaitNode outcomes (MutexState.resolved):
                     \* sequence of [f, b] records, appended by handoff
                     \* (b = true) and cancel (b = false), consumed at the
                     \* winner's resumed finish
          runq,      \* runnable entries [fiber, call, fresh]: fresh
                     \* submissions and published winners
          exts,      \* in-flight external cancel records
          history,   \* observation trace: issue/comp records
          hit_grant,     \* coverage: an inline grant section ran
          hit_try_refuse, \* coverage: a refused try section ran
          hit_handoff,   \* coverage: a handoff section ran
          hit_cancel_hit,  \* coverage: a cancel-of-a-waiter section ran
          hit_cancel_miss, \* coverage: a cancel-of-a-non-waiter ran
          hit_resume_t,  \* coverage: a resumed locker completed true
          hit_resume_f,  \* coverage: a resumed locker completed false
          wseq,      \* V2.3 witness ghost: 0..5, the seam state machine
          wf,        \* witness ghost: fiber whose unlock handoff section
                     \* fixed step 1 (its call instance, via wlive)
          wx,        \* witness ghost: external caller that entered at
                     \* step 2
          wlive      \* witness ghost: wf's recorded call instance is
                     \* still in flight toward its physical return

vars == <<owner, waitq, cur, resolved, runq, exts, history,
          hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
          hit_cancel_miss, hit_resume_t, hit_resume_f,
          wseq, wf, wx, wlive>>

NoCur == [fiber |-> "none", call |-> "none", phase |-> "off",
          resumed |-> FALSE, result |-> "none"]

\* Observations record the caller's execution domain; external callers
\* are never faked as fibers.
IssueFib(f, c) == [type |-> "issue", src |-> "fib", id |-> f,
                   call |-> c, result |-> "none"]
IssueExt(x) == [type |-> "issue", src |-> "ext", id |-> x,
                call |-> "cancel", result |-> "none"]
CompFib(f, c, r) == [type |-> "comp", src |-> "fib", id |-> f,
                     call |-> c, result |-> r]
CompExt(x, r)    == [type |-> "comp", src |-> "ext", id |-> x,
                     call |-> "cancel", result |-> r]

Running(c) == c.phase = "run"
Returning(c) == c.phase = "ret"
Fresh(c) == c.phase = "run" /\ c.resumed = FALSE

Init == owner = "none"
  /\ waitq = << >>
  /\ cur = NoCur
  /\ resolved = << >>
  /\ runq = << >>
  /\ exts = {}
  /\ history = << >>
  /\ hit_grant = FALSE
  /\ hit_try_refuse = FALSE
  /\ hit_handoff = FALSE
  /\ hit_cancel_hit = FALSE
  /\ hit_cancel_miss = FALSE
  /\ hit_resume_t = FALSE
  /\ hit_resume_f = FALSE
  /\ wseq = 0
  /\ wf = "none"
  /\ wx = "none"
  /\ wlive = FALSE

RECURSIVE InRunq(_, _)
InRunq(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).fiber = f
       \/ InRunq(Tail(q), f)

\* removeFiber: drop the FIRST occurrence of w (WaitQueue::unlink).
RECURSIVE RemoveFiber(_, _)
RemoveFiber(s, w) ==
  IF s = << >> THEN << >>
  ELSE IF Head(s) = w THEN Tail(s)
  ELSE <<Head(s)>> \o RemoveFiber(Tail(s), w)

\* consumeRec: consume the FIRST record of f, yielding its outcome and
\* the remaining records (the one-shot consume at the resumed finish).
\* `found` is FALSE when f has no record.
RECURSIVE ConsumeRec(_, _)
ConsumeRec(f, s) ==
  IF s = << >> THEN [found |-> FALSE, b |-> FALSE, rest |-> << >>]
  ELSE IF Head(s).f = f
    THEN [found |-> TRUE, b |-> Head(s).b, rest |-> Tail(s)]
    ELSE LET tl == ConsumeRec(f, Tail(s))
         IN [found |-> tl.found, b |-> tl.b,
             rest |-> IF tl.found THEN <<Head(s)>> \o tl.rest ELSE s]

\* mutexAdmit at fresh dispatch: a recursive lock is refused, an unlock
\* is admitted only from the owner (the caller precondition the
\* THEOREM B separator rests on).
Admits(c, f) ==
  IF c = "lock" THEN owner # f
  ELSE IF c = "unlock" THEN owner = f
  ELSE TRUE

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
  /\ UNCHANGED <<owner, waitq, cur, resolved, exts, history,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx>>

\* PrimStep2.dispatchFresh: emits the issue observation; requires the
\* baton free and the admission gate.
FiberDispatch ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = TRUE
  /\ Admits(Head(runq).call, Head(runq).fiber)
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             phase |-> "run", resumed |-> FALSE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ history' = Append(history,
      IssueFib(Head(runq).fiber, Head(runq).call))
  /\ UNCHANGED <<owner, waitq, resolved, exts,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

\* PrimStep2.fiberEffect, lock inline grant (:51-58: the queue empty and
\* the owner slot free).  Silent; enters the return window holding the
\* baton.  MutGrantHeld drops the owner-slot guard.
LockTake ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "lock"
  /\ waitq = << >>
  /\ \/ owner = "none"
     \/ MutGrantHeld
  /\ ~MutFusedReturn
  /\ owner' = cur.fiber
  /\ hit_grant' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "t"]
  /\ UNCHANGED <<waitq, resolved, runq, exts, history,
                 hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

\* PrimStep2.fiberEffect, try granted (:30-33: free and no waiter).
TryTake ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "try"
  /\ owner = "none" /\ waitq = << >>
  /\ ~MutFusedReturn
  /\ owner' = cur.fiber
  /\ hit_grant' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "t"]
  /\ UNCHANGED <<waitq, resolved, runq, exts, history,
                 hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

\* PrimStep2.fiberEffect, try refused (:27-29).
TryRefuse ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "try"
  /\ ~(owner = "none" /\ waitq = << >>)
  /\ ~MutFusedReturn
  /\ hit_try_refuse' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "f"]
  /\ UNCHANGED <<owner, waitq, resolved, runq, exts, history,
                 hit_grant, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

\* PrimStep2.runPark, lock (:46, :70: register and suspend).  Silent.
\* mutexRun's lock facet is none exactly off the grant branch and off
\* the caller's own ownership (the handed-off winner's resume), which
\* admission already refused.
LockPark ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "lock"
  /\ ~(owner = "none" /\ waitq = << >>)
  /\ owner # cur.fiber
  /\ cur' = NoCur
  /\ waitq' = Append(waitq, cur.fiber)
  /\ UNCHANGED <<owner, resolved, runq, exts, history,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

\* PrimStep2.fiberEffect, unlock with waiters (:158-181: hand the mutex
\* to the FIFO head, record the winner's one-shot outcome, publish it).
\* Silent; the unlocker enters its return window.  MutHandoffOwner
\* forgets the owner transfer.
UnlockHandoff ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "unlock"
  /\ Len(waitq) > 0
  /\ ~MutFusedReturn
  /\ waitq' = Tail(waitq)
  /\ resolved' = Append(resolved, [f |-> Head(waitq), b |-> TRUE])
  /\ runq' = Append(runq, [fiber |-> Head(waitq), call |-> "lock",
                           fresh |-> FALSE])
  /\ owner' = IF MutHandoffOwner THEN owner ELSE Head(waitq)
  /\ hit_handoff' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ wseq' = IF wseq = 0 /\ ~MutHandoffOwner THEN 1 ELSE wseq
  /\ wf' = IF wseq = 0 /\ ~MutHandoffOwner THEN cur.fiber ELSE wf
  /\ wlive' = IF wseq = 0 /\ ~MutHandoffOwner THEN TRUE ELSE wlive
  /\ UNCHANGED <<exts, history, hit_grant, hit_try_refuse,
                 hit_cancel_hit, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, wx>>

\* PrimStep2.fiberEffect, unlock with no waiters (:192-196: free).
UnlockFree ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "unlock"
  /\ waitq = << >>
  /\ ~MutFusedReturn
  /\ owner' = "none"
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "unit"]
  /\ UNCHANGED <<waitq, resolved, runq, exts, history,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

\* The witness's step 5: the unlocking fiber itself physically returns,
\* its call instance still live.
WitnessStep5 ==
  wseq = 4 /\ cur # NoCur /\ cur.fiber = wf /\ cur.call = "unlock"
  /\ cur.result = "unit" /\ wlive

\* PrimStep2.fiberDone: the physical return (completion observation).
\* Frees the baton.  This is the step the V2.2 fusion deleted.
FiberDone ==
  /\ cur # NoCur /\ Returning(cur)
  /\ ~MutFusedReturn
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call, cur.result))
  /\ cur' = NoCur
  /\ wseq' = IF WitnessStep5 THEN 5 ELSE wseq
  /\ wlive' = IF WitnessStep5 THEN wlive
               ELSE IF cur.fiber = wf /\ wseq >= 1 THEN FALSE ELSE wlive
  /\ UNCHANGED <<owner, waitq, resolved, runq, exts,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f, wf, wx>>

\* PrimStep2.dispatchResumed: silent dispatch of a published winner;
\* requires the baton free.
FiberResume ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = FALSE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> "lock",
             phase |-> "run", resumed |-> TRUE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ UNCHANGED <<owner, waitq, resolved, exts, history,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

\* PrimStep2.finishDone: a resumed locker completes at its dispatch --
\* the handoff or cancel already fixed the outcome, so effect and
\* return do not come apart here.  Observable.
FinishResumed ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed /\ cur.call = "lock"
  /\ (ConsumeRec(cur.fiber, resolved)).found
  /\ Len(history) < MaxHistory
  /\ LET c == ConsumeRec(cur.fiber, resolved)
     IN /\ history' = Append(history,
            CompFib(cur.fiber, "lock", IF c.b THEN "t" ELSE "f"))
        /\ resolved' = c.rest
  /\ cur' = NoCur
  /\ hit_resume_t' = IF (ConsumeRec(cur.fiber, resolved)).b
                       THEN TRUE ELSE hit_resume_t
  /\ hit_resume_f' = IF (ConsumeRec(cur.fiber, resolved)).b
                       THEN hit_resume_f ELSE TRUE
  /\ UNCHANGED <<owner, waitq, runq, exts,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, wseq, wf, wx, wlive>>

\* PrimStep2.extApply: an external caller ENTERS cancel(w).  The issue
\* observation is emitted at entry; entry owns no mutex state and is
\* legal while a fiber holds the baton (running or returning).
ExtIssue(x, w) ==
  /\ x \in Exts /\ w \in Fibers
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {[x |-> x, w |-> w, phase |-> "ent",
                          result |-> "none"]}
  /\ history' = Append(history, IssueExt(x))
  /\ wseq' = IF wseq = 1 THEN 2 ELSE wseq
  /\ wx' = IF wseq = 1 THEN x ELSE wx
  /\ UNCHANGED <<owner, waitq, cur, resolved, runq,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wf, wlive>>

InWaitq(w) == \E i \in 1..Len(waitq) : waitq[i] = w

\* PrimStep2.extEffect, cancel of a parked waiter (:149-155: resolve
\* cancelled, unlink, publish).  Silent.  MutCancelTrue records the
\* granted outcome instead.
ExtCancelHit ==
  /\ \E e \in exts :
       /\ e.phase = "ent"
       /\ InWaitq(e.w)
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, w |-> e.w, phase |-> "eff",
                          result |-> "t"]}
       /\ waitq' = RemoveFiber(waitq, e.w)
       /\ resolved' = Append(resolved,
           [f |-> e.w, b |-> IF MutCancelTrue THEN TRUE ELSE FALSE])
       /\ runq' = Append(runq, [fiber |-> e.w, call |-> "lock",
                                fresh |-> FALSE])
       /\ hit_cancel_hit' = TRUE
       /\ wseq' = IF wseq = 2 /\ e.x = wx THEN 3 ELSE wseq
  /\ UNCHANGED <<owner, cur, history, hit_grant, hit_try_refuse,
                 hit_handoff, hit_cancel_miss, hit_resume_t,
                 hit_resume_f, wf, wx, wlive>>

\* PrimStep2.extEffect, cancel of a non-waiter (:152: plain false).
\* Silent; no state change at all.
ExtCancelMiss ==
  /\ \E e \in exts :
       /\ e.phase = "ent"
       /\ ~InWaitq(e.w)
       /\ exts' = (exts \ {e})
                 \union {[x |-> e.x, w |-> e.w, phase |-> "eff",
                          result |-> "f"]}
       /\ hit_cancel_miss' = TRUE
       /\ wseq' = IF wseq = 2 /\ e.x = wx THEN 3 ELSE wseq
  /\ UNCHANGED <<owner, waitq, cur, resolved, runq, history,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_resume_t, hit_resume_f, wf, wx, wlive>>

\* PrimStep2.extDone: the physical return, deliberately unordered with
\* respect to the other steps -- a fiber may run a whole call between
\* this caller's section and its return.
ExtDone(x) ==
  /\ \E e \in exts : e.x = x /\ e.phase = "eff"
  /\ Len(history) < MaxHistory
  /\ LET e == CHOOSE e \in exts : e.x = x /\ e.phase = "eff"
     IN /\ exts' = exts \ {e}
        /\ history' = Append(history, CompExt(x, e.result))
  /\ wseq' = IF wseq = 3 /\ x = wx THEN 4 ELSE wseq
  /\ UNCHANGED <<owner, waitq, cur, resolved, runq,
                 hit_grant, hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wf, wx, wlive>>

(***************************************************************************)
(* MutFusedReturn: the V2.2 `runDone` regression -- a fresh fiber call's   *)
(* critical section and physical return in ONE transition.  The fiber      *)
(* never occupies a "ret" state, so nothing can interleave between its     *)
(* effect and its completion.  (LockPark has no completion and stays       *)
(* separate.)                                                              *)
(***************************************************************************)

FusedLockTake ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "lock"
  /\ waitq = << >>
  /\ \/ owner = "none"
     \/ MutGrantHeld
  /\ MutFusedReturn
  /\ owner' = cur.fiber
  /\ hit_grant' = TRUE
  /\ history' = Append(history, CompFib(cur.fiber, "lock", "t"))
  /\ cur' = NoCur
  /\ UNCHANGED <<waitq, resolved, runq, exts,
                 hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

FusedTryTake ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "try"
  /\ owner = "none" /\ waitq = << >>
  /\ MutFusedReturn
  /\ owner' = cur.fiber
  /\ hit_grant' = TRUE
  /\ history' = Append(history, CompFib(cur.fiber, "try", "t"))
  /\ cur' = NoCur
  /\ UNCHANGED <<waitq, resolved, runq, exts,
                 hit_try_refuse, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

FusedTryRefuse ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "try"
  /\ ~(owner = "none" /\ waitq = << >>)
  /\ MutFusedReturn
  /\ hit_try_refuse' = TRUE
  /\ history' = Append(history, CompFib(cur.fiber, "try", "f"))
  /\ cur' = NoCur
  /\ UNCHANGED <<owner, waitq, resolved, runq, exts,
                 hit_grant, hit_handoff, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

FusedUnlockHandoff ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "unlock"
  /\ Len(waitq) > 0
  /\ MutFusedReturn
  /\ waitq' = Tail(waitq)
  /\ resolved' = Append(resolved, [f |-> Head(waitq), b |-> TRUE])
  /\ runq' = Append(runq, [fiber |-> Head(waitq), call |-> "lock",
                           fresh |-> FALSE])
  /\ owner' = IF MutHandoffOwner THEN owner ELSE Head(waitq)
  /\ hit_handoff' = TRUE
  /\ history' = Append(history, CompFib(cur.fiber, "unlock", "unit"))
  /\ cur' = NoCur
  /\ wseq' = IF wseq = 0 /\ ~MutHandoffOwner THEN 1 ELSE wseq
  /\ wf' = IF wseq = 0 /\ ~MutHandoffOwner THEN cur.fiber ELSE wf
  /\ wlive' = FALSE
  /\ UNCHANGED <<exts, hit_grant, hit_try_refuse, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f, wx>>

FusedUnlockFree ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "unlock"
  /\ waitq = << >>
  /\ MutFusedReturn
  /\ owner' = "none"
  /\ hit_handoff' = hit_handoff
  /\ history' = Append(history, CompFib(cur.fiber, "unlock", "unit"))
  /\ cur' = NoCur
  /\ UNCHANGED <<waitq, resolved, runq, exts,
                 hit_grant, hit_try_refuse, hit_cancel_hit,
                 hit_cancel_miss, hit_resume_t, hit_resume_f,
                 wseq, wf, wx, wlive>>

Next ==
  \/ \E f \in Fibers, c \in Calls : FiberSubmit(f, c)
  \/ FiberDispatch
  \/ LockTake
  \/ TryTake
  \/ TryRefuse
  \/ LockPark
  \/ UnlockHandoff
  \/ UnlockFree
  \/ FiberDone
  \/ FiberResume
  \/ FinishResumed
  \/ \E x \in Exts, w \in Fibers : ExtIssue(x, w)
  \/ ExtCancelHit
  \/ ExtCancelMiss
  \/ \E x \in Exts : ExtDone(x)
  \/ FusedLockTake
  \/ FusedTryTake
  \/ FusedTryRefuse
  \/ FusedUnlockHandoff
  \/ FusedUnlockFree

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type and safety invariants                                              *)
(***************************************************************************)

TypeOK ==
  /\ owner \in Fibers \cup {"none"}
  /\ waitq \in Seq(Fibers)
  /\ cur \in ({NoCur} \union
       {[fiber |-> f, call |-> c, phase |-> p, resumed |-> r, result |-> s] :
          f \in Fibers, c \in Calls,
          p \in {"run", "ret"}, r \in BOOLEAN, s \in Results})
  /\ resolved \in Seq({[f |-> g, b |-> bo] : g \in Fibers, bo \in BOOLEAN})
  /\ runq \in Seq({[fiber |-> f, call |-> c, fresh |-> fr] :
                    f \in Fibers, c \in Calls, fr \in BOOLEAN})
  /\ exts \subseteq {[x |-> x, w |-> w, phase |-> p, result |-> r] :
                      x \in Exts, w \in Fibers,
                      p \in {"ent", "eff"}, r \in {"none", "t", "f"}}
  /\ \A e1 \in exts, e2 \in exts : e1.x = e2.x => e1 = e2
  /\ history \in Seq({[type |-> t, src |-> s, id |-> i, call |-> c,
                       result |-> r] :
                       t \in {"issue", "comp"}, s \in {"fib", "ext"},
                       i \in (Fibers \cup Exts),
                       c \in Calls \cup {"cancel"},
                       r \in {"none", "t", "f", "unit"}})
  /\ hit_grant \in BOOLEAN /\ hit_try_refuse \in BOOLEAN
  /\ hit_handoff \in BOOLEAN
  /\ hit_cancel_hit \in BOOLEAN /\ hit_cancel_miss \in BOOLEAN
  /\ hit_resume_t \in BOOLEAN /\ hit_resume_f \in BOOLEAN
  /\ wseq \in 0..5
  /\ wf \in Fibers \cup {"none"}
  /\ wx \in Exts \cup {"none"}
  /\ wlive \in BOOLEAN

RECURSIVE GrantsCount(_), ReleasesCount(_), TrueRecs(_)
GrantsCount(h) ==
  IF h = << >> THEN 0
  ELSE (IF Head(h).type = "comp" /\ Head(h).src = "fib"
           /\ Head(h).call \in {"lock", "try"} /\ Head(h).result = "t"
        THEN 1 ELSE 0) + GrantsCount(Tail(h))
ReleasesCount(h) ==
  IF h = << >> THEN 0
  ELSE (IF Head(h).type = "comp" /\ Head(h).call = "unlock"
           /\ Head(h).result = "unit"
        THEN 1 ELSE 0) + ReleasesCount(Tail(h))
TrueRecs(s) ==
  IF s = << >> THEN 0
  ELSE (IF Head(s).b THEN 1 ELSE 0) + TrueRecs(Tail(s))

\* Mutual exclusion (the TLA half of the stage's safety split): at
\* every prefix, completed grants are at most completed releases + 1.
InvGrantBound == GrantsCount(history) <= ReleasesCount(history) + 1

\* No phantom unlock: a release completes only with a grant behind it.
InvReleaseBacked == ReleasesCount(history) <= GrantsCount(history)

\* The conservation mirror (`mutex_mirror`), exact-equality form (this
\* machine omits the Lean calculus's resumed-repark
\* over-approximation): trace grants plus in-window grant credit (the
\* returning slot, the delivered true records) equal trace releases
\* plus the owner slot.
InvBalance ==
  GrantsCount(history)
  + (IF cur # NoCur /\ Returning(cur)
        /\ cur.call \in {"lock", "try"} /\ cur.result = "t"
     THEN 1 ELSE 0)
  + TrueRecs(resolved)
  = ReleasesCount(history)
  + (IF cur # NoCur /\ Returning(cur) /\ cur.call = "unlock"
     THEN 1 ELSE 0)
  + (IF owner # "none" THEN 1 ELSE 0)

InvQueueNoDup ==
  /\ \A i \in 1..Len(waitq), j \in 1..Len(waitq) :
       i # j => waitq[i] # waitq[j]
  /\ \A i \in 1..Len(runq), j \in 1..Len(runq) :
       i # j => runq[i].fiber # runq[j].fiber

\* Queue ownership: one registration per fiber; parked, queued, and
\* dispatched are disjoint; the owner is never parked or queued; only
\* published winners sit stale in the runnable queue.
InvQueueOwnership ==
  /\ \A i \in 1..Len(waitq) :
       /\ ~InRunq(runq, waitq[i])
       /\ waitq[i] # cur.fiber
       /\ owner # waitq[i]
  /\ \A i \in 1..Len(runq) : runq[i].fiber # cur.fiber
  /\ \A i \in 1..Len(runq) :
       runq[i].fresh = FALSE => runq[i].call = "lock"

\* Delivered outcomes: at most one unconsumed handed-off grant, and it
\* belongs to the current owner (the winner holds the slot from its
\* handoff until its resumed finish consumes the record).
InvRecordOwner ==
  /\ TrueRecs(resolved) <= 1
  /\ \A i \in 1..Len(resolved) :
       resolved[i].b = TRUE => resolved[i].f = owner

SameCaller(o1, o2) == o1.src = o2.src /\ o1.id = o2.id

\* Completion discipline: every completion matches a prior issue of the
\* same call by the same caller, with no intervening completion of that
\* caller.
InvCompDiscipline ==
  \A i \in 1..Len(history) :
    history[i].type = "comp" =>
      \E j \in 1..(i - 1) :
        /\ history[j].type = "issue"
        /\ SameCaller(history[j], history[i])
        /\ history[j].call = history[i].call
        /\ \A k \in (j + 1)..(i - 1) :
             ~SameCaller(history[k], history[i])

SafetyInvariants == <<TypeOK, InvGrantBound, InvReleaseBacked,
                      InvBalance, InvQueueNoDup, InvQueueOwnership,
                      InvRecordOwner, InvCompDiscipline>>

(***************************************************************************)
(* The V2.3 seam certificate and coverage predicates                       *)
(*                                                                         *)
(* All are monotone over `history` (and the set-once coverage flags), so   *)
(* a single TLC run reaching the conjunctive state certifies every         *)
(* component; each coverage cfg checks the negated conjunction and must    *)
(* be VIOLATED.                                                            *)
(***************************************************************************)

\* The V2.3 seam certificate (`mutex_possesses_cancel`'s window, the
\* unlock side): a fiber unlock's handoff section ran (step 1, silent),
\* an external cancel entered after it (step 2), effected (step 3) and
\* physically returned (step 4) BEFORE the unlocker's own physical
\* return (step 5).  The ghost `wlive` anchors steps 1 and 5 to the SAME
\* call instance.  The V2.2 fused step cannot reach 5: the fused
\* handoff completes the call in one transition (wlive goes FALSE
\* immediately), and no FiberDone exists to take step 5.
InvWitness == wseq # 5

IsCompFib(o) == o.type = "comp" /\ o.src = "fib"
IsCompExt(o) == o.type = "comp" /\ o.src = "ext"

\* An external cancel's whole call straddled by a fiber completion:
\* issue(ext x) < comp(fiber) < comp(ext x).
CovExtWindow ==
  \E x \in Exts, i2, i3, i4 \in 1..Len(history) :
    /\ i2 < i3 /\ i3 < i4
    /\ history[i2] = IssueExt(x)
    /\ IsCompFib(history[i3])
    /\ history[i4].type = "comp" /\ history[i4].src = "ext"
      /\ history[i4].id = x

RECURSIVE TryTCount(_), TryFCount(_)
TryTCount(h) ==
  IF h = << >> THEN 0
  ELSE (IF Head(h).type = "comp" /\ Head(h).call = "try"
           /\ Head(h).result = "t" THEN 1 ELSE 0) + TryTCount(Tail(h))
TryFCount(h) ==
  IF h = << >> THEN 0
  ELSE (IF Head(h).type = "comp" /\ Head(h).call = "try"
           /\ Head(h).result = "f" THEN 1 ELSE 0) + TryFCount(Tail(h))

\* Coverage predicates:
\*   CovW1          - an inline grant and a completed unlock (the
\*                    free-mutex exercise)
\*   CovQ           - the handoff chain (handoff section, resumed
\*                    winner completing true)
\*   CovTryBoth     - both try outcomes completed
\*   CovCancelChain - the cancel chain (cancel-of-a-waiter, resumed
\*                    locker completing false)
\*   CovCancelMiss  - a cancel of a non-waiter
\*   CovExtWindow   - the straddled external return window
CovW1 == hit_grant /\ ReleasesCount(history) >= 1
CovQ == hit_handoff /\ hit_resume_t
CovTryBoth == TryTCount(history) >= 1 /\ TryFCount(history) >= 1
CovCancelChain == hit_cancel_hit /\ hit_resume_f
CovCancelMiss == hit_cancel_miss

InvCovW1 == ~CovW1
InvCovQ == ~CovQ
InvCovTryBoth == ~CovTryBoth
InvCovCancelChain == ~CovCancelChain
InvCovCancelMiss == ~CovCancelMiss
InvCovExtWindow == ~CovExtWindow

=============================================================================

