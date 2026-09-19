---------------- MODULE SelectCore ----------------
(***************************************************************************)
(* Stage 7V2.3 (FCB1-POST-V23-STACK-379-385): the select core's transition *)
(* system, mapping `formal/Sluice/Formal/SelectV2.lean` (`selPrim`,        *)
(* `PrimStep2` instantiated for `SelSig`) one action per constructor,      *)
(* under the post-#378 Stage-0-V2.3 execution-domain calculus.  Rebuilt    *)
(* from the production sections of `src/async/select.cpp`,                 *)
(* `select_event.cpp`, `select_timer.cpp`, and `scheduler_event.cpp`.      *)
(*                                                                         *)
(* Call-domain census (from the code):                                     *)
(*   evSet    = fiber-callable AND external-capable (event_set_broadcast,  *)
(*              scheduler_event.cpp:21-37, takes only global_mtx_; the     *)
(*              fiber path is the same critical section; the set resolves  *)
(*              an armed select group in the same section :390-466)        *)
(*   evReset  = fiber-callable AND external-capable (event_reset; never    *)
(*              resolves -- the reset blindness, scheduler_event.cpp       *)
(*              :39-42: a reset cannot see armed groups)                   *)
(*   sel      = fiber-bound (select_admit :538-547 asserts a running       *)
(*              Fiber; the admission scan :691-714 takes the first ready   *)
(*              arm -- the event arm at the lower index; atomic one-shot   *)
(*              resolution :149-180, claim CAS :164-168; suspend :758-759; *)
(*              the resumed caller consumes the committed winner           *)
(*              :775-808)                                                  *)
(*   selT     = fiber-bound, timer arm at the lower index (the symmetric   *)
(*              scan order)                                                *)
(*   Level semantics: an event win does not consume the latch              *)
(*   (select_event.cpp:26-28); a fresh select sees it again.               *)
(*                                                                         *)
(* The single group slot: one select group at a time.  A select issued     *)
(* while the slot is busy parks UNTRACKED (never woken) -- the disclosed   *)
(* concurrent-group boundary (the production group list is the Stage-1     *)
(* core's discipline).  Timer due-ness is an environment given: EnvTime    *)
(* raises `timDue` (the clock is elided; onTick), EnvExpire is the pump    *)
(* (select_timer.cpp:38-55, registration consumed :143).                   *)
(*                                                                         *)
(* Reachability disclosure: the armed/due boots are instrumented battery   *)
(* starts (relation-reachable, not primInit-reachable -- arming needs a    *)
(* fiber's select in-window).  `Boot` selects the start:                   *)
(*   "prim"  - the primInit-faithful start (idle, clear, not due)          *)
(*   "armed" - f0 parked on sel (the event-handoff battery start)          *)
(*   "due"   - f0 armed with the timer already due (the timer-handoff      *)
(*             battery start)                                              *)
(*                                                                         *)
(* Mutant switches (all FALSE in the reference configuration):             *)
(*   MutReResolve   - the done-group gate dropped from the set section     *)
(*                    (SAFETY: a done group resolves again, growing the    *)
(*                    completion window; killed by InvWindow)              *)
(*   MutFinishLie   - the resumed select delivers wonEv regardless of the  *)
(*                    consumed record (RESULT-SEMANTICS: killed by         *)
(*                    InvResultAgree)                                      *)
(*   MutParkReady   - the scan's ready-source gate dropped from the park   *)
(*                    (SAFETY: a select arms with the latch set; killed    *)
(*                    by InvQuietArmed)                                    *)
(*   MutNoTimer     - the timer pump omitted (TRACE-REMOVAL: the timer     *)
(*                    handoff witness is unreachable; every safety         *)
(*                    invariant holds)                                     *)
(*   MutM5          - the scan order flipped (RESULT-SEMANTICS: with both  *)
(*                    arms ready the wrong arm wins; killed by             *)
(*                    InvResultAgree)                                      *)
(***************************************************************************)
EXTENDS Naturals, Sequences
CONSTANT MaxHistory,   \* fuel: bound on recorded observations
          Boot,        \* initial state: see the disclosure above
          MutReResolve,
          MutFinishLie,
          MutParkReady,
          MutNoTimer,
          MutM5

ASSUME Boot \in {"prim", "armed", "due"}

Fibers == {"f0", "f1"}
Exts == {"e0"}
Calls == {"evSet", "evReset", "sel", "selT"}
FibCalls == {"evSet", "evReset", "sel", "selT"}
ExtCalls == {"evSet", "evReset"}
SelCalls == {"sel", "selT"}
Results == {"none", "rEvSet", "rEvReset", "wonEv", "wonTim"}
Outs == {"wonEv", "wonTim"}

VARIABLES flag,      \* the event latch; level semantics (not consumed)
          timDue,    \* the timer arm's due-ness (environment raised)
          phase,     \* the group slot: "idle" | "armed" | "done"
          winner,    \* the committed winner: "none" | "wonEv" | "wonTim"
          order,     \* TRUE = the event arm has the lower index (sel)
          caller,    \* the tracked group caller, or "none"
          winEvOK,   \* ghost: the event win read the latch
          winTimOK,  \* ghost: the timer win read the due timer
          resolved,  \* the completion window (at most one record)
          parkCall,  \* per fiber: the select call it armed with
          phaseF,    \* per fiber: "fresh" | "waiting" (armed)
          cur,       \* the worker slot; "run" | "ret" | NoCur
          runq,      \* runnable entries [fiber, call, fresh]
          exts,      \* in-flight external records
          history,   \* observation trace
          finishes,  \* ghost: completed select groups
          res_agree, \* ghost: every public result matched the authority
          hit_inline_ev,   \* coverage: a select won the event arm inline
          hit_inline_tim,  \* coverage: a select won the timer arm inline
          hit_park,        \* coverage: a select armed
          hit_handoff_ev,  \* coverage: a set section resolved an armed group
          hit_handoff_tim, \* coverage: the pump resolved an armed group
          hit_prio_ev,     \* coverage: both arms ready, event won
          hit_prio_tim,    \* coverage: both arms ready, timer won
          hit_reset_blind, \* coverage: evReset ran over an armed group
          hit_rearm,       \* coverage: a re-arm won inline on the held latch
          hit_resume       \* coverage: a resumed select completed

vars == <<flag, timDue, phase, winner, order, caller, winEvOK, winTimOK,
          resolved, parkCall, phaseF, cur, runq, exts, history, finishes,
          res_agree, hit_inline_ev, hit_inline_tim, hit_park,
          hit_handoff_ev, hit_handoff_tim, hit_prio_ev, hit_prio_tim,
          hit_reset_blind, hit_rearm, hit_resume>>

NoCur == [fiber |-> "none", call |-> "none", phase |-> "off",
          resumed |-> FALSE, result |-> "none"]

AllFresh == [f \in Fibers |-> "fresh"]
AllNone == [f \in Fibers |-> "none"]

IssueFib(f, c) == [type |-> "issue", src |-> "fib", id |-> f,
                   call |-> c, result |-> "none"]
IssueExt(x, c) == [type |-> "issue", src |-> "ext", id |-> x,
                   call |-> c, result |-> "none"]
CompFib(f, c, r) == [type |-> "comp", src |-> "fib", id |-> f,
                     call |-> c, result |-> r]
CompExt(x, c, r) == [type |-> "comp", src |-> "ext", id |-> x,
                     call |-> c, result |-> r]

Fresh(c) == c.phase = "run" /\ c.resumed = FALSE
Returning(c) == c.phase = "ret"

Init ==
  /\ flag = FALSE
  /\ timDue = (Boot = "due")
  /\ phase = IF Boot = "prim" THEN "idle" ELSE "armed"
  /\ winner = "none"
  /\ order = TRUE
  /\ caller = IF Boot = "prim" THEN "none" ELSE "f0"
  /\ winEvOK = FALSE
  /\ winTimOK = FALSE
  /\ resolved = << >>
  /\ parkCall = IF Boot = "prim" THEN AllNone
                ELSE [f \in Fibers |-> IF f = "f0" THEN "sel" ELSE "none"]
  /\ phaseF = IF Boot = "prim" THEN AllFresh
              ELSE [f \in Fibers |-> IF f = "f0" THEN "waiting"
                                     ELSE "fresh"]
  /\ cur = NoCur
  /\ runq = << >>
  /\ exts = {}
  /\ history = << >>
  /\ finishes = 0
  /\ res_agree = TRUE
  /\ hit_inline_ev = FALSE
  /\ hit_inline_tim = FALSE
  /\ hit_park = FALSE
  /\ hit_handoff_ev = FALSE
  /\ hit_handoff_tim = FALSE
  /\ hit_prio_ev = FALSE
  /\ hit_prio_tim = FALSE
  /\ hit_reset_blind = FALSE
  /\ hit_rearm = FALSE
  /\ hit_resume = FALSE

RECURSIVE InRunq(_, _), ConsumeRec(_, _)
InRunq(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).fiber = f
       \/ InRunq(Tail(q), f)

\* consumeRec: consume the FIRST record of f, yielding its outcome and
\* the remaining records; `found` is FALSE when f has no record.
ConsumeRec(f, s) ==
  IF s = << >> THEN [found |-> FALSE, out |-> "wonEv", rest |-> << >>]
  ELSE IF Head(s).f = f
    THEN [found |-> TRUE, out |-> Head(s).out, rest |-> Tail(s)]
    ELSE LET tl == ConsumeRec(f, Tail(s))
         IN [found |-> tl.found, out |-> tl.out,
             rest |-> IF tl.found THEN <<Head(s)>> \o tl.rest ELSE s]

\* The runq entry published for a resolved group's tracked caller.
PubEntry(f) == [fiber |-> f, call |-> parkCall[f], fresh |-> FALSE]

\* The scan orders.  The AUTHORITY is the frozen production scan (the
\* lowest-index ready arm wins; written independently of MutM5).  The
\* action scan honors MutM5 -- the flip is the mutant's kill site.
EvFirst(c) == (c = "sel") # MutM5
ScanIsEv(c) == flag /\ (EvFirst(c) \/ ~timDue)
ScanIsTim(c) == timDue /\ (~EvFirst(c) \/ ~flag)
AuthIsEv(c) == IF c = "sel" THEN flag ELSE (flag /\ ~timDue)
AuthIsTim(c) == IF c = "sel" THEN (~flag /\ timDue) ELSE timDue

\* PrimStep2.submit: silent; a between-calls fiber enters the runnable
\* queue.
FiberSubmit(f, c) ==
  /\ f \in Fibers /\ c \in FibCalls
  /\ f # cur.fiber
  /\ ~InRunq(runq, f)
  /\ phaseF[f] = "fresh"
  /\ Len(history) < MaxHistory
  /\ runq' = Append(runq, [fiber |-> f, call |-> c, fresh |-> TRUE])
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, cur, exts, history,
                 finishes, res_agree, hit_inline_ev, hit_inline_tim,
                 hit_park, hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_reset_blind, hit_rearm, hit_resume>>

\* PrimStep2.dispatchFresh: emits the issue observation; admission is
\* total on this surface (arm-shape validation is the template
\* constraint, not a runtime rejection).
FiberDispatch ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = TRUE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             phase |-> "run", resumed |-> FALSE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ history' = Append(history,
      IssueFib(Head(runq).fiber, Head(runq).call))
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, exts, finishes,
                 res_agree, hit_inline_ev, hit_inline_tim, hit_park,
                 hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_reset_blind, hit_rearm, hit_resume>>

\* The set section's resolution shape (select_resolve_event_locked ->
\* select_process_group_locked, :390-466 + :149-180): the latch stands,
\* the event arm wins, the ghost records the read, the tracked caller is
\* published runnable.  MutReResolve drops the done-group gate -- the
\* InvWindow kill site.
GroupResolveEv ==
  /\ phase = "armed" \/ (MutReResolve /\ phase = "done")
  /\ caller # "none"
  /\ flag' = TRUE
  /\ phase' = "done"
  /\ winner' = "wonEv"
  /\ winEvOK' = TRUE
  /\ resolved' = Append(resolved, [f |-> caller, out |-> "wonEv"])
  /\ runq' = Append(runq, PubEntry(caller))
  /\ phaseF' = [phaseF EXCEPT ![caller] = "fresh"]
  /\ hit_handoff_ev' = TRUE
  /\ hit_prio_ev' = (hit_prio_ev \/ timDue)

\* PrimStep2.fiberEffect, evSet with no armed group (the plain
\* broadcast).  MutReResolve confines this to the non-armed shapes.
FiberEvSetPlain ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "evSet"
  /\ phase # "armed" /\ (~MutReResolve \/ phase # "done")
  /\ flag' = TRUE
  /\ res_agree' = res_agree
  /\ hit_reset_blind' = hit_reset_blind
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "rEvSet"]
  /\ UNCHANGED <<timDue, phase, winner, order, caller, winEvOK, winTimOK,
                 resolved, parkCall, phaseF, runq, exts, history,
                 finishes, hit_inline_ev, hit_inline_tim, hit_park,
                 hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_rearm, hit_resume>>

FiberEvSetResolve ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "evSet"
  /\ GroupResolveEv
  /\ res_agree' = res_agree
  /\ hit_reset_blind' = hit_reset_blind
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "rEvSet"]
  /\ hit_rearm' = hit_rearm
  /\ hit_resume' = hit_resume
  /\ UNCHANGED <<timDue, order, caller, winTimOK, parkCall, exts,
                 history, finishes, hit_inline_ev, hit_inline_tim,
                 hit_park, hit_prio_tim, hit_handoff_tim>>

\* PrimStep2.fiberEffect, evReset: the latch falls; no group state is
\* touched (the reset blindness, scheduler_event.cpp:39-42).
FiberEvReset ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "evReset"
  /\ flag' = FALSE
  /\ hit_reset_blind' = (hit_reset_blind \/ (phase = "armed"))
  /\ res_agree' = res_agree
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "rEvReset"]
  /\ UNCHANGED <<timDue, phase, winner, order, caller, winEvOK, winTimOK,
                 resolved, parkCall, phaseF, runq, exts, history,
                 finishes, hit_inline_ev, hit_inline_tim, hit_park,
                 hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_rearm, hit_resume>>

\* PrimStep2.fiberEffect, the select scan winning inline (the admission
\* scan :691-714).  The group slot is untouched; the result must match
\* the frozen scan order (the MutM5 kill site).
SelInline(c) ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = c
  /\ phase = "idle"
  /\ ScanIsEv(c) \/ ScanIsTim(c)
  /\ res_agree' = (res_agree /\
       ((IF ScanIsEv(c) THEN "wonEv" ELSE "wonTim") =
        (IF AuthIsEv(c) THEN "wonEv" ELSE "wonTim")))
  /\ hit_inline_ev' = (hit_inline_ev \/ ScanIsEv(c))
  /\ hit_inline_tim' = (hit_inline_tim \/ ScanIsTim(c))
  /\ hit_prio_ev' = (hit_prio_ev \/ (ScanIsEv(c) /\ timDue))
  /\ hit_prio_tim' = (hit_prio_tim \/ (ScanIsTim(c) /\ flag))
  /\ hit_rearm' = (hit_rearm \/ (finishes > 0 /\ flag))
  /\ cur' = [cur EXCEPT !.phase = "ret",
             !.result = IF ScanIsEv(c) THEN "wonEv" ELSE "wonTim"]
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, runq, exts,
                 history, finishes, hit_park, hit_handoff_ev,
                 hit_handoff_tim, hit_reset_blind, hit_resume>>

\* PrimStep2.runPark, the select arms: no arm ready (the scan's
\* ready-source gate; MutParkReady drops it), the group slot stands
\* empty, the caller and scan order are recorded.
SelPark(c) ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = c
  /\ phase = "idle"
  /\ (~flag /\ ~timDue) \/ MutParkReady
  /\ phase' = "armed"
  /\ order' = (c = "sel")
  /\ caller' = cur.fiber
  /\ parkCall' = [parkCall EXCEPT ![cur.fiber] = c]
  /\ phaseF' = [phaseF EXCEPT ![cur.fiber] = "waiting"]
  /\ hit_park' = TRUE
  /\ cur' = NoCur
  /\ UNCHANGED <<flag, timDue, winner, winEvOK, winTimOK, resolved, runq,
                 exts, history, finishes, res_agree, hit_inline_ev,
                 hit_inline_tim, hit_handoff_ev, hit_handoff_tim,
                 hit_prio_ev, hit_prio_tim, hit_reset_blind, hit_rearm,
                 hit_resume>>

\* The disclosed concurrent-group boundary: a select issued while the
\* slot is busy parks untracked and is never woken.  Silent.
ParkUntracked ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call \in SelCalls
  /\ phase # "idle"
  /\ phaseF' = [phaseF EXCEPT ![cur.fiber] = "waiting"]
  /\ cur' = NoCur
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, runq, exts, history,
                 finishes, res_agree, hit_inline_ev, hit_inline_tim,
                 hit_park, hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_reset_blind, hit_rearm, hit_resume>>

\* PrimStep2.fiberDone: the physical return (completion observation).
FiberDone ==
  /\ cur # NoCur /\ Returning(cur)
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call, cur.result))
  /\ cur' = NoCur
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, runq, exts,
                 finishes, res_agree, hit_inline_ev, hit_inline_tim,
                 hit_park, hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_reset_blind, hit_rearm, hit_resume>>

\* PrimStep2.dispatchResumed: silent dispatch of a published select.
FiberResume ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = FALSE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             phase |-> "run", resumed |-> TRUE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, exts, history,
                 finishes, res_agree, hit_inline_ev, hit_inline_tim,
                 hit_park, hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_reset_blind, hit_rearm, hit_resume>>

\* PrimStep2.finishDone: the resumed select consumes the committed
\* winner (:775-808) -- the result is exactly the record's outcome, the
\* slot is released, and the group ends consumed.  MutFinishLie delivers
\* wonEv regardless (the InvResultAgree kill site).
WaiterFinish ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed
  /\ cur.call \in SelCalls
  /\ LET c == ConsumeRec(cur.fiber, resolved)
     IN /\ c.found
        /\ resolved' = c.rest
        /\ phase' = "idle"
        /\ winner' = "none"
        /\ winEvOK' = FALSE
        /\ winTimOK' = FALSE
        /\ caller' = "none"
        /\ parkCall' = [parkCall EXCEPT ![cur.fiber] = "none"]
        /\ finishes' = finishes + 1
        /\ hit_resume' = TRUE
        /\ res_agree' = (res_agree /\
             ((IF MutFinishLie THEN "wonEv" ELSE c.out) = c.out))
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call,
                       ConsumeRec(cur.fiber, resolved).out))
  /\ phaseF' = [phaseF EXCEPT ![cur.fiber] = "fresh"]
  /\ cur' = NoCur
  /\ UNCHANGED <<flag, timDue, order, runq, exts, hit_inline_ev,
                 hit_inline_tim, hit_park, hit_handoff_ev,
                 hit_handoff_tim, hit_prio_ev, hit_prio_tim,
                 hit_reset_blind, hit_rearm>>

\* PrimStep2.extApply: an external caller ENTERS an event section.
ExtIssue(x, c) ==
  /\ x \in Exts /\ c \in ExtCalls
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {[x |-> x, call |-> c, phase |-> "ent",
                           result |-> "none"]}
  /\ history' = Append(history, IssueExt(x, c))
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, cur, runq,
                 finishes, res_agree, hit_inline_ev, hit_inline_tim,
                 hit_park, hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_reset_blind, hit_rearm, hit_resume>>

\* PrimStep2.extEffect, evSet over no armed group.
ExtSetPlain(x) ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "evSet" /\ e.x = x
       /\ phase # "armed" /\ (~MutReResolve \/ phase # "done")
       /\ exts' = (exts \ {e})
             \union {[x |-> x, call |-> "evSet", phase |-> "eff",
                     result |-> "rEvSet"]}
  /\ flag' = TRUE
  /\ res_agree' = res_agree
  /\ hit_reset_blind' = hit_reset_blind
  /\ UNCHANGED <<timDue, phase, winner, order, caller, winEvOK, winTimOK,
                 resolved, parkCall, phaseF, cur, runq, history, finishes,
                 hit_inline_ev, hit_inline_tim, hit_park, hit_handoff_ev,
                 hit_handoff_tim, hit_prio_ev, hit_prio_tim, hit_rearm,
                 hit_resume>>

\* PrimStep2.extEffect, evSet resolving the armed group in the same
\* critical section (event_set_broadcast, scheduler_event.cpp:21-37).
ExtSetResolve(x) ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "evSet" /\ e.x = x
       /\ GroupResolveEv
       /\ exts' = (exts \ {e})
             \union {[x |-> x, call |-> "evSet", phase |-> "eff",
                     result |-> "rEvSet"]}
  /\ res_agree' = res_agree
  /\ hit_reset_blind' = hit_reset_blind
  /\ hit_rearm' = hit_rearm
  /\ hit_resume' = hit_resume
  /\ UNCHANGED <<timDue, order, caller, winTimOK, parkCall, cur, history,
                 finishes, hit_inline_ev, hit_inline_tim, hit_park,
                 hit_prio_tim, hit_handoff_tim>>

\* PrimStep2.extEffect, evReset.
ExtReset(x) ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "evReset" /\ e.x = x
       /\ exts' = (exts \ {e})
             \union {[x |-> x, call |-> "evReset", phase |-> "eff",
                     result |-> "rEvReset"]}
  /\ flag' = FALSE
  /\ hit_reset_blind' = (hit_reset_blind \/ (phase = "armed"))
  /\ res_agree' = res_agree
  /\ UNCHANGED <<timDue, phase, winner, order, caller, winEvOK, winTimOK,
                 resolved, parkCall, phaseF, cur, runq, history, finishes,
                 hit_inline_ev, hit_inline_tim, hit_park, hit_handoff_ev,
                 hit_handoff_tim, hit_prio_ev, hit_prio_tim, hit_rearm,
                 hit_resume>>

\* PrimStep2.extDone: the physical return, deliberately unordered with
\* respect to the other steps.
ExtDone(x) ==
  /\ \E e \in exts : e.x = x /\ e.phase = "eff"
  /\ Len(history) < MaxHistory
  /\ LET e == CHOOSE e \in exts : e.x = x /\ e.phase = "eff"
     IN /\ exts' = exts \ {e}
        /\ history' = Append(history, CompExt(x, e.call, e.result))
  /\ UNCHANGED <<flag, timDue, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, cur, runq,
                 finishes, res_agree, hit_inline_ev, hit_inline_tim,
                 hit_park, hit_handoff_ev, hit_handoff_tim, hit_prio_ev,
                 hit_prio_tim, hit_reset_blind, hit_rearm, hit_resume>>

\* PrimStep2.envTime: time passes at an idle point; due-ness rises and
\* persists (a fresh select with a past deadline is legal).  Silent.
EnvTime ==
  /\ cur = NoCur
  /\ timDue' = TRUE
  /\ UNCHANGED <<cur, flag, phase, winner, order, caller, winEvOK,
                 winTimOK, resolved, parkCall, phaseF, runq, exts,
                 history, finishes, res_agree, hit_inline_ev,
                 hit_inline_tim, hit_park, hit_handoff_ev,
                 hit_handoff_tim, hit_prio_ev, hit_prio_tim,
                 hit_reset_blind, hit_rearm, hit_resume>>

\* PrimStep2.envExpire: the timer pump (select_timer_pump_entry_locked,
\* select_timer.cpp:38-55): the due timer wins, the ghost records the
\* read, the registration is consumed (:143), the tracked caller is
\* published runnable.  MutNoTimer drops the pump -- the TRACE-REMOVAL
\* kill site.
EnvExpire ==
  /\ ~MutNoTimer
  /\ cur = NoCur
  /\ phase = "armed"
  /\ caller # "none"
  /\ timDue
  /\ phase' = "done"
  /\ winner' = "wonTim"
  /\ winTimOK' = TRUE
  /\ resolved' = Append(resolved, [f |-> caller, out |-> "wonTim"])
  /\ runq' = Append(runq, PubEntry(caller))
  /\ phaseF' = [phaseF EXCEPT ![caller] = "fresh"]
  /\ hit_handoff_tim' = TRUE
  /\ hit_prio_tim' = (hit_prio_tim \/ flag)
  /\ UNCHANGED <<flag, timDue, order, caller, winEvOK, parkCall, exts,
                 history, finishes, res_agree, cur, hit_inline_ev,
                 hit_inline_tim, hit_park, hit_handoff_ev, hit_prio_ev,
                 hit_reset_blind, hit_rearm, hit_resume>>

Next ==
  \/ \E f \in Fibers, c \in FibCalls : FiberSubmit(f, c)
  \/ FiberDispatch
  \/ FiberEvSetPlain
  \/ FiberEvSetResolve
  \/ FiberEvReset
  \/ \E c \in SelCalls : SelInline(c)
  \/ \E c \in SelCalls : SelPark(c)
  \/ ParkUntracked
  \/ FiberDone
  \/ FiberResume
  \/ WaiterFinish
  \/ \E x \in Exts, c \in ExtCalls : ExtIssue(x, c)
  \/ \E x \in Exts : ExtSetPlain(x)
  \/ \E x \in Exts : ExtSetResolve(x)
  \/ \E x \in Exts : ExtReset(x)
  \/ \E x \in Exts : ExtDone(x)
  \/ EnvTime
  \/ EnvExpire

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type and safety invariants                                              *)
(***************************************************************************)

TypeOK ==
  /\ flag \in BOOLEAN /\ timDue \in BOOLEAN
  /\ phase \in {"idle", "armed", "done"}
  /\ winner \in {"none", "wonEv", "wonTim"}
  /\ order \in BOOLEAN
  /\ caller \in (Fibers \cup {"none"})
  /\ winEvOK \in BOOLEAN /\ winTimOK \in BOOLEAN
  /\ resolved \in Seq({[f |-> g, out |-> o] : g \in Fibers,
                        o \in Outs})
  /\ parkCall \in [Fibers -> {"none", "sel", "selT"}]
  /\ phaseF \in [Fibers -> {"fresh", "waiting"}]
  /\ cur \in ({NoCur} \union
       {[fiber |-> f, call |-> c, phase |-> p, resumed |-> r,
         result |-> s] : f \in Fibers, c \in FibCalls,
          p \in {"run", "ret"}, r \in BOOLEAN, s \in Results})
  /\ runq \in Seq({[fiber |-> f, call |-> c, fresh |-> fr] :
                    f \in Fibers, c \in FibCalls, fr \in BOOLEAN})
  /\ exts \subseteq {[x |-> x, call |-> c, phase |-> p, result |-> r] :
                      x \in Exts, c \in ExtCalls,
                      p \in {"ent", "eff"}, r \in Results}
  /\ \A e1 \in exts, e2 \in exts : e1.x = e2.x => e1 = e2
  /\ history \in Seq({[type |-> t, src |-> s, id |-> i, call |-> c,
                       result |-> r] :
                       t \in {"issue", "comp"}, s \in {"fib", "ext"},
                       i \in (Fibers \cup Exts), c \in Calls,
                       r \in Results})
  /\ finishes \in Nat
  /\ res_agree \in BOOLEAN
  /\ hit_inline_ev \in BOOLEAN /\ hit_inline_tim \in BOOLEAN
  /\ hit_park \in BOOLEAN
  /\ hit_handoff_ev \in BOOLEAN /\ hit_handoff_tim \in BOOLEAN
  /\ hit_prio_ev \in BOOLEAN /\ hit_prio_tim \in BOOLEAN
  /\ hit_reset_blind \in BOOLEAN /\ hit_rearm \in BOOLEAN
  /\ hit_resume \in BOOLEAN

\* Armed quiet (MutParkReady's kill): a suspended group never coexists
\* with a set latch, and its caller is tracked.
InvQuietArmed == phase = "armed" => (~flag /\ caller # "none")

\* Phase/winner agreement and source-read ghosts: the committed winner
\* exists exactly while the group is done, an event win saw the latch, a
\* timer win saw the due timer, and a done group keeps its caller.
InvWinnerDone ==
  /\ (phase = "done") <=> (winner # "none")
  /\ winner = "wonEv" => winEvOK
  /\ winner = "wonTim" => winTimOK
  /\ phase = "done" => caller # "none"

\* The completion window (MutReResolve's kill): at most one record, only
\* a done group holds one, and the record is exactly the committed
\* winner for the tracked caller.
InvWindow ==
  /\ Len(resolved) <= 1
  /\ resolved = << >> \/ (phase = "done" /\ resolved[1].f = caller
                            /\ resolved[1].out = winner)

\* Group ownership: the armed caller is never runnable or dispatched and
\* sits in phase "waiting"; only published select calls sit stale in the
\* runnable queue.
InvGroupOwnership ==
  /\ phase = "armed" =>
       /\ ~InRunq(runq, caller)
       /\ caller # cur.fiber
       /\ phaseF[caller] = "waiting"
  /\ \A i \in 1..Len(runq) : runq[i].fiber # cur.fiber
  /\ \A i \in 1..Len(runq) :
       runq[i].fresh = FALSE => runq[i].call \in SelCalls

\* Every public result matched the outcome authority (the scan order is
\* the frozen production order; the finish delivers exactly the consumed
\* record).
InvResultAgree == res_agree

SameCaller(o1, o2) == o1.src = o2.src /\ o1.id = o2.id

\* Completion discipline: every completion matches a prior issue of the
\* same call by the same caller, with no intervening completion of that
\* caller -- EXCEPT the boot group's single pre-Init call instance (the
\* instrumented configurations start with the select armed past its
\* issue, the documented battery disclosure); such a completion has no
\* in-trace issue of that call up to its own position (the exception is
\* prefix-scoped, so the invariant is monotone).
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

SafetyInvariants == <<TypeOK, InvQuietArmed, InvWinnerDone, InvWindow,
                      InvGroupOwnership, InvResultAgree,
                      InvCompDiscipline>>

(***************************************************************************)
(* Coverage predicates and the witness separation                          *)
(*                                                                         *)
(* All are monotone (history append or set-once flags), so a single TLC    *)
(* run reaching the conjunctive state certifies every component; each      *)
(* coverage cfg checks the negated conjunction and must be VIOLATED.       *)
(* The MutNoTimer separation cfg asserts its witness is UNreachable and    *)
(* must HOLD.                                                              *)
(***************************************************************************)

CovInlineEv == hit_inline_ev
CovInlineTim == hit_inline_tim
CovHandoffEv == hit_handoff_ev /\ hit_resume
CovHandoffTim == hit_handoff_tim /\ hit_resume
CovPrioEv == hit_prio_ev
CovPrioTim == hit_prio_tim
CovResetBlind == hit_reset_blind
CovRearm == hit_rearm

NotHandoffTim == ~CovHandoffTim

InvCovInlineEv == ~CovInlineEv
InvCovInlineTim == ~CovInlineTim
InvCovHandoffEv == ~CovHandoffEv
InvCovHandoffTim == ~CovHandoffTim
InvCovPrioEv == ~CovPrioEv
InvCovPrioTim == ~CovPrioTim
InvCovResetBlind == ~CovResetBlind
InvCovRearm == ~CovRearm

=============================================================================
