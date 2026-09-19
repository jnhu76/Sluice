---------------- MODULE QueueCore ----------------
(***************************************************************************)
(* Stage 6V2.3 (FCB1-POST-V23-STACK-379-385): the AsyncQueue primitive's   *)
(* transition system, mapping `formal/Sluice/Formal/QueueV2.lean`          *)
(* (`qPrim`, `PrimStep2` instantiated for `QSig`) one action per           *)
(* constructor, under the post-#378 Stage-0-V2.3 execution-domain          *)
(* calculus.  Rebuilt from the production sections of                      *)
(* `src/async/scheduler_queue.cpp` and `src/async/queue_port.cpp`.         *)
(*                                                                         *)
(* Call-domain census (from the code):                                     *)
(*   push      = fiber-bound (queue_push_admit :199-200 asserts a running  *)
(*               Fiber; inline commit when open, not full, and the fresh   *)
(*               node lands at the producer queue's head; the consumer     *)
(*               cross-grant follows :216-218; suspend on a full open      *)
(*               queue)                                                    *)
(*   pop       = fiber-bound (:243-244; inline delivery when the ring is   *)
(*               non-empty and the fresh node lands at the consumer        *)
(*               queue's head; the producer cross-grant follows :258-259;  *)
(*               suspend on an empty open queue)                           *)
(*   try_push  = external-capable (QueuePort::try_push :125-164; the try   *)
(*               barges past suspended producers)                          *)
(*   try_pop   = external-capable (:166-198)                               *)
(*   close     = external-capable (:200-218; sets the bit :214, drains     *)
(*               consumers then producers :216-217, and publishes every    *)
(*               drained waiter runnable)                                  *)
(*   cancel / the timed paths / the teardown session = outside the core    *)
(*   surface (the Lean census excludes them; no public AsyncQueue path     *)
(*   issues cancel).                                                       *)
(*                                                                         *)
(* The fiber call domain carries only push/pop: the try calls and close    *)
(* are external-capable only (their entry points read no `g_worker`),      *)
(* and close is exercised through its external caller, keeping the state   *)
(* space at two fibers.                                                    *)
(*                                                                         *)
(* Ghost ledger: `clog` records the ring-insertion order of every          *)
(* committed item, `dlog` the delivery order of every handed-off item.     *)
(* InvLog `clog = dlog \o ring` compresses conservation and external FIFO  *)
(* into one state equation (the Lean `qSafe`).                             *)
(*                                                                         *)
(* Reachability disclosure: the boots with a full ring or a parked         *)
(* waiter are the instrumented battery starts (relation-reachable, not     *)
(* primInit-reachable -- filling the ring or parking the first waiter      *)
(* needs a second fiber's traffic in-window).  `Boot` selects the start:   *)
(*   "prim"   - the primInit-faithful start (empty, open, none queued)     *)
(*   "full2"  - ring <<A,B>>, f0 parked pushing A                          *)
(*   "emptyc" - f0 parked popping on an empty open ring                    *)
(*                                                                         *)
(* Mutant switches (all FALSE in the reference configuration):             *)
(*   MutCloseCommit      - the closed gate dropped from push (SAFETY:      *)
(*                         killed by InvNoCommitClosed)                    *)
(*   MutFifo             - pop delivers the second ring item (SAFETY:      *)
(*                         killed by InvLog)                               *)
(*   MutNoConsumerGrant  - push skips the consumer cross-grant             *)
(*                         (TRACE-REMOVAL: the consumer-handoff witness    *)
(*                         is unreachable; every safety invariant holds)   *)
(*   MutNoProducerGrant  - delivery skips the producer cross-grant         *)
(*                         (TRACE-REMOVAL: the producer-handoff witness    *)
(*                         is unreachable; every safety invariant holds)   *)
(*   MutWrongResult      - try_pop on an empty closed queue returns        *)
(*                         itemA (RESULT-SEMANTICS: killed by              *)
(*                         InvResultAgree; the state does not move)        *)
(***************************************************************************)
EXTENDS Naturals, Sequences
CONSTANT MaxHistory,   \* fuel: bound on recorded observations
          Boot,        \* initial state: see the disclosure above
          MutCloseCommit,
          MutFifo,
          MutNoConsumerGrant,
          MutNoProducerGrant,
          MutWrongResult

ASSUME Boot \in {"prim", "full2", "emptyc"}

Items == {"A", "B"}
Cap == 2
Fibers == {"f0", "f1"}
Exts == {"e0"}
Calls == {"pushA", "pushB", "pop", "trypushA", "trypushB", "trypop",
          "close"}
\* The fiber-bound calls: the try calls and close are external-capable
\* only (their entry points read no `g_worker`), so a fiber can never
\* submit one.
FibCalls == {"pushA", "pushB", "pop"}
ExtCalls == {"trypushA", "trypushB", "trypop", "close"}
Results == {"none", "committed", "wouldblock", "closed", "done",
            "itemA", "itemB"}
Outs == {"committed", "closed", "itemA", "itemB"}

VARIABLES ring,      \* buffered items, FIFO, head first (ring_head_)
          closed,    \* the closed bit (closed_)
          waitqP,    \* suspended producers, carrying their leases (waiters_[0])
          waitqC,    \* suspended consumers (waiters_[1])
          resolved,  \* one-shot outcomes of granted waiters
          phase,     \* per-fiber: "fresh" | "waiting" (queued)
          cur,       \* the worker slot; "run" | "ret" | NoCur
          runq,      \* runnable entries [fiber, call, fresh]
          exts,      \* in-flight external records
          history,   \* observation trace
          clog,      \* ghost: commit log (ring-insertion order)
          dlog,      \* ghost: delivery log (handoff order)
          commits_open, \* ghost: every commit ran while the queue was open
          res_agree, \* ghost: every public result matched the authority
          hit_inline_push,  \* coverage: a fiber push committed inline
          hit_inline_pop,   \* coverage: a fiber pop delivered inline
          hit_push_park,    \* coverage: a push suspended on a full queue
          hit_pop_park,     \* coverage: a pop suspended on an empty queue
          hit_handoff_pc,   \* coverage: a delivery granted a parked producer
          hit_handoff_cp,   \* coverage: a commit granted a parked consumer
          hit_close_buffered, \* coverage: close ran over a buffered item
          hit_close_pushpark, \* coverage: close drained a parked producer
          hit_close_poppark,  \* coverage: close drained a parked consumer
          hit_fifo_multi,     \* coverage: two deliveries in commit order
          hit_resume        \* coverage: a resumed waiter completed

vars == <<ring, closed, waitqP, waitqC, resolved, phase, cur, runq,
          exts, history, clog, dlog, commits_open, res_agree,
          hit_inline_push, hit_inline_pop, hit_push_park, hit_pop_park,
          hit_handoff_pc, hit_handoff_cp, hit_close_buffered,
          hit_close_pushpark, hit_close_poppark, hit_fifo_multi,
          hit_resume>>

NoCur == [fiber |-> "none", call |-> "none", phase |-> "off",
          resumed |-> FALSE, result |-> "none"]

AllFresh == [f \in Fibers |-> "fresh"]

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

CallItem(c) == IF c = "pushA" THEN "A" ELSE "B"
CallOf(it) == IF it = "A" THEN "pushA" ELSE "pushB"
ItemOut(it) == IF it = "A" THEN "itemA" ELSE "itemB"

Init ==
  /\ ring = IF Boot = "full2" THEN <<"A", "B">> ELSE << >>
  /\ closed = FALSE
  /\ waitqP = IF Boot = "full2"
                THEN <<[f |-> "f0", item |-> "A"]>>
                ELSE << >>
  /\ waitqC = IF Boot = "emptyc" THEN <<"f0">> ELSE << >>
  /\ resolved = << >>
  /\ phase = IF Boot = "prim" THEN AllFresh
                               ELSE [f \in Fibers |-> "waiting"]
  /\ cur = NoCur
  /\ runq = << >>
  /\ exts = {}
  /\ history = << >>
  /\ clog = IF Boot = "full2" THEN <<"A", "B">> ELSE << >>
  /\ dlog = << >>
  /\ commits_open = TRUE
  /\ res_agree = TRUE
  /\ hit_inline_push = FALSE
  /\ hit_inline_pop = FALSE
  /\ hit_push_park = FALSE
  /\ hit_pop_park = FALSE
  /\ hit_handoff_pc = FALSE
  /\ hit_handoff_cp = FALSE
  /\ hit_close_buffered = FALSE
  /\ hit_close_pushpark = FALSE
  /\ hit_close_poppark = FALSE
  /\ hit_fifo_multi = FALSE
  /\ hit_resume = FALSE

RECURSIVE InRunq(_, _), InWP(_, _), InWC(_, _), ConsumeRec(_, _), Build(_, _), Cat(_, _)
InRunq(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).fiber = f
       \/ InRunq(Tail(q), f)

InWP(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q).f = f
       \/ InWP(Tail(q), f)

InWC(q, f) ==
  IF q = << >> THEN FALSE
  ELSE \/ Head(q) = f
       \/ InWC(Tail(q), f)

\* consumeRec: consume the FIRST record of f, yielding its outcome and
\* the remaining records; `found` is FALSE when f has no record.
ConsumeRec(f, s) ==
  IF s = << >> THEN [found |-> FALSE, out |-> "closed", rest |-> << >>]
  ELSE IF Head(s).f = f
    THEN [found |-> TRUE, out |-> Head(s).out, rest |-> Tail(s)]
    ELSE LET tl == ConsumeRec(f, Tail(s))
         IN [found |-> tl.found, out |-> tl.out,
             rest |-> IF tl.found THEN <<Head(s)>> \o tl.rest ELSE s]

\* Build(F, n): <<F(1), ..., F(n)>> -- the literal `<< >>` for n = 0: a
\* function constructor over a computed empty domain yields a value that
\* fails `Seq` membership in TLC.
Build(F(_), n) == IF n = 0 THEN << >> ELSE [i \in 1..n |-> F(i)]

\* Cat(a, b): sequence concatenation that tolerates empty operands.
Cat(a, b) ==
  IF a = << >> THEN b
  ELSE IF b = << >> THEN a
  ELSE a \o b

\* The committed-item gate: open, not full, and the fresh node lands at
\* the producer queue's head (:56-73).  MutCloseCommit drops the open
\* half.
CommitGate(item) ==
  /\ Len(ring) < Cap
  /\ waitqP = << >>
  /\ (~closed \/ MutCloseCommit)

\* The runq entry published for a granted or drained waiter.
PubEntry(f, c) == [fiber |-> f, call |-> c, fresh |-> FALSE]

\* PrimStep2.submit: silent; a between-calls fiber enters the runnable
\* queue.
FiberSubmit(f, c) ==
  /\ f \in Fibers /\ c \in FibCalls
  /\ f # cur.fiber
  /\ ~InRunq(runq, f)
  /\ ~InWP(waitqP, f) /\ ~InWC(waitqC, f)
  /\ Len(history) < MaxHistory
  /\ runq' = Append(runq, [fiber |-> f, call |-> c, fresh |-> TRUE])
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, cur,
                 exts, history, clog, dlog, commits_open, res_agree,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.dispatchFresh: emits the issue observation; admission is
\* total on this surface (registrations cannot reject).
FiberDispatch ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = TRUE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             phase |-> "run", resumed |-> FALSE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ history' = Append(history,
      IssueFib(Head(runq).fiber, Head(runq).call))
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, exts,
                 clog, dlog, commits_open, res_agree,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.fiberEffect, push's inline commit with no consumer queued.
\* The commit keeps the ledger: clog takes the item, the ring grows at
\* the tail.
PushInline ==
  /\ cur # NoCur /\ Fresh(cur)
  /\ cur.call \in {"pushA", "pushB"}
  /\ CommitGate(CallItem(cur.call))
  /\ waitqC = << >>
  /\ ring' = Append(ring, CallItem(cur.call))
  /\ clog' = Append(clog, CallItem(cur.call))
  /\ commits_open' = (commits_open /\ ~closed)
  /\ res_agree' = res_agree
  /\ hit_inline_push' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "committed"]
  /\ UNCHANGED <<closed, waitqP, waitqC, resolved, phase, runq, exts,
                 history, dlog, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.fiberEffect, push's inline commit granting the queued
\* consumer (:216-218, :381-405): the commit and the handoff are one
\* section; under InvDrained the ring is empty exactly when a consumer
\* waits, so the handed-off item is the one this section committed.
PushInlineGrant ==
  /\ cur # NoCur /\ Fresh(cur)
  /\ cur.call \in {"pushA", "pushB"}
  /\ CommitGate(CallItem(cur.call))
  /\ waitqC # << >> /\ ~MutNoConsumerGrant
  /\ ring' = << >>
  /\ clog' = Append(clog, CallItem(cur.call))
  /\ dlog' = Append(dlog, CallItem(cur.call))
  /\ waitqC' = Tail(waitqC)
  /\ resolved' = Append(resolved,
       [f |-> Head(waitqC), out |-> ItemOut(CallItem(cur.call))])
  /\ runq' = Append(runq, PubEntry(Head(waitqC), "pop"))
  /\ phase' = [phase EXCEPT ![Head(waitqC)] = "fresh"]
  /\ commits_open' = (commits_open /\ ~closed)
  /\ res_agree' = res_agree
  /\ hit_inline_push' = TRUE
  /\ hit_handoff_cp' = TRUE
  /\ hit_fifo_multi' = hit_fifo_multi
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "committed"]
  /\ UNCHANGED <<closed, waitqP, exts, history, hit_inline_pop,
                 hit_push_park, hit_pop_park, hit_handoff_pc,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_resume>>

\* PrimStep2.runPark, push suspends on a full open queue or behind a
\* suspended producer.  Silent.
PushPark ==
  /\ cur # NoCur /\ Fresh(cur)
  /\ cur.call \in {"pushA", "pushB"}
  /\ ~closed
  /\ CommitGate(CallItem(cur.call)) = FALSE
  /\ waitqP' = Append(waitqP,
       [f |-> cur.fiber, item |-> CallItem(cur.call)])
  /\ phase' = [phase EXCEPT ![cur.fiber] = "waiting"]
  /\ cur' = NoCur
  /\ hit_push_park' = TRUE
  /\ UNCHANGED <<ring, closed, waitqC, resolved, runq, exts, history,
                 clog, dlog, commits_open, res_agree, hit_inline_push,
                 hit_inline_pop, hit_pop_park, hit_handoff_pc,
                 hit_handoff_cp, hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.fiberEffect, push reads the closed bit (:75-87).  Under
\* MutCloseCommit this is unreachable for a full-open... for a closed
\* queue the commit gate admits instead (the mutant's kill site).
PushClosed ==
  /\ cur # NoCur /\ Fresh(cur)
  /\ cur.call \in {"pushA", "pushB"}
  /\ closed
  /\ CommitGate(CallItem(cur.call)) = FALSE
  /\ res_agree' = res_agree
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "closed"]
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, runq,
                 exts, history, clog, dlog, commits_open,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* The delivery shape of pop: the head item leaves the ring into the
\* delivery log.  MutFifo delivers the SECOND item and keeps the head in
\* front (the InvLog kill site).
DelivItem == IF MutFifo /\ Len(ring) >= 2 THEN ring[2] ELSE Head(ring)
DelivRest == IF MutFifo /\ Len(ring) >= 2
               THEN <<Head(ring)>> \o Tail(Tail(ring))
               ELSE Tail(ring)

\* PrimStep2.fiberEffect, pop's inline delivery with no producer to
\* grant (:128-146 without a :258-259 cross-grant).
PopDeliverNoP ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "pop"
  /\ ring # << >> /\ waitqC = << >> /\ waitqP = << >>
  /\ ring' = DelivRest
  /\ dlog' = Append(dlog, DelivItem)
  /\ hit_fifo_multi' = (hit_fifo_multi \/ Len(dlog) >= 1)
  /\ res_agree' = (res_agree /\ (DelivItem = Head(ring)))
  /\ hit_inline_pop' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = ItemOut(DelivItem)]
  /\ UNCHANGED <<closed, waitqP, waitqC, resolved, phase, runq, exts,
                 history, clog, commits_open, hit_inline_push,
                 hit_push_park, hit_pop_park, hit_handoff_pc,
                 hit_handoff_cp, hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_resume>>

\* PrimStep2.fiberEffect, pop's inline delivery granting the head
\* suspended producer (:258-259, :407-432): the lease commits while the
\* queue stands open (the capacity guard is implied: the ring just lost
\* its head).  MutNoProducerGrant drops the grant.
PopDeliverGrant ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "pop"
  /\ ring # << >> /\ waitqC = << >> /\ waitqP # << >>
  /\ ~closed /\ ~MutNoProducerGrant
  /\ ring' = Append(DelivRest, Head(waitqP).item)
  /\ clog' = Append(clog, Head(waitqP).item)
  /\ dlog' = Append(dlog, DelivItem)
  /\ waitqP' = Tail(waitqP)
  /\ resolved' = Append(resolved,
       [f |-> Head(waitqP).f, out |-> "committed"])
  /\ runq' = Append(runq, PubEntry(Head(waitqP).f,
                                   CallOf(Head(waitqP).item)))
  /\ phase' = [phase EXCEPT ![Head(waitqP).f] = "fresh"]
  /\ commits_open' = (commits_open /\ ~closed)
  /\ hit_fifo_multi' = (hit_fifo_multi \/ Len(dlog) >= 1)
  /\ res_agree' = (res_agree /\ (DelivItem = Head(ring)))
  /\ hit_inline_pop' = TRUE
  /\ hit_handoff_pc' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = ItemOut(DelivItem)]
  /\ UNCHANGED <<closed, waitqC, exts, history, hit_inline_push,
                 hit_push_park, hit_pop_park, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_resume>>

\* PrimStep2.fiberEffect, pop's delivery over a CLOSED queue granting
\* the head suspended producer its closed outcome (the drain semantics:
\* buffered items stay poppable, the bit refuses the lease).
PopDeliverGrantClosed ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "pop"
  /\ ring # << >> /\ waitqC = << >> /\ waitqP # << >>
  /\ closed /\ ~MutNoProducerGrant
  /\ ring' = DelivRest
  /\ dlog' = Append(dlog, DelivItem)
  /\ waitqP' = Tail(waitqP)
  /\ resolved' = Append(resolved,
       [f |-> Head(waitqP).f, out |-> "closed"])
  /\ runq' = Append(runq, PubEntry(Head(waitqP).f,
                                   CallOf(Head(waitqP).item)))
  /\ phase' = [phase EXCEPT ![Head(waitqP).f] = "fresh"]
  /\ commits_open' = commits_open
  /\ hit_fifo_multi' = (hit_fifo_multi \/ Len(dlog) >= 1)
  /\ res_agree' = (res_agree /\ (DelivItem = Head(ring)))
  /\ hit_inline_pop' = TRUE
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = ItemOut(DelivItem)]
  /\ UNCHANGED <<clog, closed, waitqC, exts, history,
                 hit_inline_push, hit_push_park, hit_pop_park,
                 hit_handoff_pc, hit_handoff_cp, hit_close_buffered,
                 hit_close_pushpark, hit_close_poppark, hit_resume>>

\* PrimStep2.fiberEffect, pop reads the closed bit on an empty ring
\* (:148-160).
PopClosed ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "pop"
  /\ ring = << >> /\ closed
  /\ res_agree' = res_agree
  /\ cur' = [cur EXCEPT !.phase = "ret", !.result = "closed"]
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, runq,
                 exts, history, clog, dlog, commits_open,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.runPark, pop suspends on an empty open queue.  Silent.
PopPark ==
  /\ cur # NoCur /\ Fresh(cur) /\ cur.call = "pop"
  /\ ring = << >> /\ ~closed
  /\ waitqC' = Append(waitqC, cur.fiber)
  /\ phase' = [phase EXCEPT ![cur.fiber] = "waiting"]
  /\ cur' = NoCur
  /\ hit_pop_park' = TRUE
  /\ UNCHANGED <<ring, closed, waitqP, resolved, runq, exts, history,
                 clog, dlog, commits_open, res_agree, hit_inline_push,
                 hit_inline_pop, hit_push_park, hit_handoff_pc,
                 hit_handoff_cp, hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.fiberDone: the physical return (completion observation).
FiberDone ==
  /\ cur # NoCur /\ Returning(cur)
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call, cur.result))
  /\ cur' = NoCur
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, runq,
                 exts, clog, dlog, commits_open, res_agree,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.dispatchResumed: silent dispatch of a published waiter.
FiberResume ==
  /\ cur = NoCur
  /\ Len(runq) > 0
  /\ Head(runq).fresh = FALSE
  /\ cur' = [fiber |-> Head(runq).fiber, call |-> Head(runq).call,
             phase |-> "run", resumed |-> TRUE, result |-> "none"]
  /\ runq' = Tail(runq)
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, exts,
                 history, clog, dlog, commits_open, res_agree,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.finishDone: a resumed waiter consumes its recorded outcome
\* (the grant was applied by the granter's section) and completes with
\* exactly that outcome.
WaiterFinish ==
  /\ cur # NoCur /\ cur.phase = "run" /\ cur.resumed
  /\ cur.call \in FibCalls
  /\ LET c == ConsumeRec(cur.fiber, resolved)
     IN /\ c.found
        /\ resolved' = c.rest
        /\ hit_resume' = TRUE
  /\ res_agree' = res_agree
  /\ Len(history) < MaxHistory
  /\ history' = Append(history, CompFib(cur.fiber, cur.call,
                       ConsumeRec(cur.fiber, resolved).out))
  /\ phase' = [phase EXCEPT ![cur.fiber] = "fresh"]
  /\ cur' = NoCur
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, runq, exts, clog, dlog,
                 commits_open, hit_inline_push, hit_inline_pop,
                 hit_push_park, hit_pop_park, hit_handoff_pc,
                 hit_handoff_cp, hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi>>

\* PrimStep2.extApply: an external caller ENTERS a call.  Only the try
\* calls and close are external-capable.
ExtIssue(x, c) ==
  /\ x \in Exts /\ c \in ExtCalls
  /\ \A e \in exts : e.x # x
  /\ Len(history) < MaxHistory
  /\ exts' = exts \union {[x |-> x, call |-> c, phase |-> "ent",
                           result |-> "none"]}
  /\ history' = Append(history, IssueExt(x, c))
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, cur,
                 runq, clog, dlog, commits_open, res_agree,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

\* PrimStep2.extEffect, try_push: the try barges (no producer-queue
\* check), committing while the gate stands, refusing when full,
\* reading closed when closed.  The consumer cross-grant follows the
\* commit exactly as at the fiber push.
ExtTryPush(x, item) ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = CallOf(item) /\ e.x = x
       /\ LET committed == CommitGate(item)
              res == IF committed THEN "committed"
                     ELSE IF closed THEN "closed"
                     ELSE "wouldblock"
          IN /\ exts' = (exts \ {e})
                    \union {[x |-> x, call |-> e.call, phase |-> "eff",
                             result |-> res]}
             /\ res_agree' = (res_agree /\
                  (res = (IF CommitGate(item) THEN "committed"
                         ELSE IF closed THEN "closed"
                         ELSE "wouldblock")))
             /\ IF committed
                  THEN /\ clog' = Append(clog, item)
                       /\ commits_open' = (commits_open /\ ~closed)
                       /\ IF waitqC = << >>
                            THEN /\ ring' = Append(ring, item)
                                 /\ dlog' = dlog
                                 /\ waitqC' = waitqC
                                 /\ resolved' = resolved
                                 /\ runq' = runq
                                 /\ phase' = phase
                                 /\ hit_handoff_cp' = hit_handoff_cp
                                 /\ hit_fifo_multi' = hit_fifo_multi
                            ELSE /\ ring' = << >>
                                 /\ dlog' = Append(dlog, item)
                                 /\ waitqC' = Tail(waitqC)
                                 /\ resolved' = Append(resolved,
                                      [f |-> Head(waitqC),
                                       out |-> ItemOut(item)])
                                 /\ runq' = Append(runq,
                                      PubEntry(Head(waitqC), "pop"))
                                 /\ phase' = [phase EXCEPT
                                                ![Head(waitqC)] = "fresh"]
                                 /\ hit_handoff_cp' = TRUE
                                 /\ hit_fifo_multi' = hit_fifo_multi
                  ELSE /\ ring' = ring /\ clog' = clog /\ dlog' = dlog
                       /\ waitqC' = waitqC /\ resolved' = resolved
                       /\ runq' = runq /\ phase' = phase
                       /\ commits_open' = commits_open
                       /\ hit_handoff_cp' = hit_handoff_cp
                       /\ hit_fifo_multi' = hit_fifo_multi
  /\ closed' = closed
  /\ waitqP' = waitqP
  /\ cur' = cur
  /\ history' = history
  /\ hit_inline_push' = hit_inline_push
  /\ hit_inline_pop' = hit_inline_pop
  /\ hit_push_park' = hit_push_park
  /\ hit_pop_park' = hit_pop_park
  /\ hit_handoff_pc' = hit_handoff_pc
  /\ hit_close_buffered' = hit_close_buffered
  /\ hit_close_pushpark' = hit_close_pushpark
  /\ hit_close_poppark' = hit_close_poppark
  /\ hit_resume' = hit_resume

\* PrimStep2.extEffect, try_pop: delivery (with the producer cross-grant
\* cases), refusal, or the closed read.  MutWrongResult returns itemA
\* where the code reads closed (the InvResultAgree kill site).
ExtTryPop(x) ==
  /\ \E e \in exts :
       /\ e.phase = "ent" /\ e.call = "trypop" /\ e.x = x
       /\ LET deliver == ring # << >> /\ waitqC = << >>
              res == IF deliver THEN ItemOut(Head(ring))
                     ELSE IF closed THEN IF MutWrongResult
                                            THEN "itemA" ELSE "closed"
                     ELSE "wouldblock"
              auth == IF deliver THEN ItemOut(Head(ring))
                      ELSE IF closed THEN "closed"
                      ELSE "wouldblock"
          IN /\ exts' = (exts \ {e})
                    \union {[x |-> x, call |-> e.call, phase |-> "eff",
                             result |-> res]}
             /\ res_agree' = (res_agree /\ (res = auth))
             /\ IF deliver
                  THEN /\ dlog' = Append(dlog, Head(ring))
                       /\ hit_fifo_multi' = (hit_fifo_multi \/ Len(dlog) >= 1)
                       /\ IF waitqP = << >> \/ MutNoProducerGrant
                            THEN /\ ring' = Tail(ring)
                                 /\ clog' = clog
                                 /\ waitqP' = waitqP
                                 /\ resolved' = resolved
                                 /\ runq' = runq
                                 /\ phase' = phase
                                 /\ commits_open' = commits_open
                                 /\ hit_handoff_pc' = hit_handoff_pc
                            ELSE IF ~closed
                                 THEN /\ ring' = Append(Tail(ring),
                                        Head(waitqP).item)
                                      /\ clog' = Append(clog,
                                        Head(waitqP).item)
                                      /\ waitqP' = Tail(waitqP)
                                      /\ resolved' = Append(resolved,
                                        [f |-> Head(waitqP).f,
                                         out |-> "committed"])
                                      /\ runq' = Append(runq,
                                        PubEntry(Head(waitqP).f,
                                          CallOf(Head(waitqP).item)))
                                      /\ phase' = [phase EXCEPT
                                        ![Head(waitqP).f] = "fresh"]
                                      /\ commits_open' = (commits_open /\ ~closed)
                                      /\ hit_handoff_pc' = TRUE
                                 ELSE /\ ring' = Tail(ring)
                                      /\ clog' = clog
                                      /\ waitqP' = Tail(waitqP)
                                      /\ resolved' = Append(resolved,
                                        [f |-> Head(waitqP).f,
                                         out |-> "closed"])
                                      /\ runq' = Append(runq,
                                        PubEntry(Head(waitqP).f,
                                          CallOf(Head(waitqP).item)))
                                      /\ phase' = [phase EXCEPT
                                        ![Head(waitqP).f] = "fresh"]
                                      /\ commits_open' = commits_open
                                      /\ hit_handoff_pc' = hit_handoff_pc
                  ELSE /\ ring' = ring /\ clog' = clog /\ dlog' = dlog
                       /\ waitqP' = waitqP /\ waitqC' = waitqC
                       /\ resolved' = resolved
                       /\ runq' = runq /\ phase' = phase
                       /\ commits_open' = commits_open
                       /\ hit_fifo_multi' = hit_fifo_multi
                       /\ hit_handoff_pc' = hit_handoff_pc
  /\ closed' = closed
  /\ waitqC' = waitqC
  /\ cur' = cur
  /\ history' = history
  /\ hit_inline_push' = hit_inline_push
  /\ hit_inline_pop' = hit_inline_pop
  /\ hit_push_park' = hit_push_park
  /\ hit_pop_park' = hit_pop_park
  /\ hit_handoff_cp' = hit_handoff_cp
  /\ hit_close_buffered' = hit_close_buffered
  /\ hit_close_pushpark' = hit_close_pushpark
  /\ hit_close_poppark' = hit_close_poppark
  /\ hit_resume' = hit_resume

\* The close drain's per-waiter outcomes and publications.
CloseConsOut(i) ==
  [f |-> waitqC[i], out |-> IF i <= Len(ring) THEN ItemOut(ring[i])
                              ELSE "closed"]
CloseProdOut(j) == [f |-> waitqP[j].f, out |-> "closed"]
CloseConsPub(i) == PubEntry(waitqC[i], "pop")
CloseProdPub(j) == PubEntry(waitqP[j].f, CallOf(waitqP[j].item))
CloseItem(i) == ring[i]

\* PrimStep2.extEffect, close (:200-218): the bit stands, every suspended
\* consumer is handed a ring head while items remain (else the closed
\* outcome), every suspended producer reads closed, and every drained
\* waiter is published runnable -- consumers before producers, in FIFO
\* order.
CloseK == IF Len(ring) < Len(waitqC) THEN Len(ring) ELSE Len(waitqC)

ExtClose(x) ==
  /\ \E e \in exts :
       /\ e.phase = "ent"
       /\ e.call = "close"
       /\ e.x = x
       /\ exts' = (exts \ {e})
                    \union {[x |-> x, call |-> "close", phase |-> "eff",
                             result |-> "done"]}
  /\ res_agree' = res_agree
  /\ ring' = IF CloseK >= Len(ring) THEN << >> ELSE SubSeq(ring, CloseK + 1, Len(ring))
  /\ clog' = clog
  /\ dlog' = Cat(dlog, Build(CloseItem, CloseK))
  /\ waitqC' = << >>
  /\ waitqP' = << >>
  /\ resolved' = Cat(Cat(resolved, Build(CloseConsOut, Len(waitqC))),
                     Build(CloseProdOut, Len(waitqP)))
  /\ runq' = Cat(Cat(runq, Build(CloseConsPub, Len(waitqC))),
                 Build(CloseProdPub, Len(waitqP)))
  /\ phase' = [g \in Fibers |->
                 IF InWC(waitqC, g) \/ InWP(waitqP, g)
                   THEN "fresh" ELSE phase[g]]
  /\ commits_open' = commits_open
  /\ closed' = TRUE
  /\ cur' = cur
  /\ history' = history
  /\ hit_close_buffered' = (hit_close_buffered \/ ring # << >>)
  /\ hit_close_poppark' = (hit_close_poppark \/ waitqC # << >>)
  /\ hit_close_pushpark' = (hit_close_pushpark \/ waitqP # << >>)
  /\ hit_inline_push' = hit_inline_push
  /\ hit_inline_pop' = hit_inline_pop
  /\ hit_push_park' = hit_push_park
  /\ hit_pop_park' = hit_pop_park
  /\ hit_handoff_pc' = hit_handoff_pc
  /\ hit_handoff_cp' = hit_handoff_cp
  /\ hit_fifo_multi' = hit_fifo_multi
  /\ hit_resume' = hit_resume

\* PrimStep2.extDone: the physical return, deliberately unordered with
\* respect to the other steps.
ExtDone(x) ==
  /\ \E e \in exts : e.x = x /\ e.phase = "eff"
  /\ Len(history) < MaxHistory
  /\ LET e == CHOOSE e \in exts : e.x = x /\ e.phase = "eff"
     IN /\ exts' = exts \ {e}
        /\ history' = Append(history, CompExt(x, e.call, e.result))
  /\ UNCHANGED <<ring, closed, waitqP, waitqC, resolved, phase, cur,
                 runq, clog, dlog, commits_open, res_agree,
                 hit_inline_push, hit_inline_pop, hit_push_park,
                 hit_pop_park, hit_handoff_pc, hit_handoff_cp,
                 hit_close_buffered, hit_close_pushpark,
                 hit_close_poppark, hit_fifo_multi, hit_resume>>

Next ==
  \/ \E f \in Fibers, c \in FibCalls : FiberSubmit(f, c)
  \/ FiberDispatch
  \/ PushInline
  \/ PushInlineGrant
  \/ PushPark
  \/ PushClosed
  \/ PopDeliverNoP
  \/ PopDeliverGrant
  \/ PopDeliverGrantClosed
  \/ PopClosed
  \/ PopPark
  \/ FiberDone
  \/ FiberResume
  \/ WaiterFinish
  \/ \E x \in Exts, c \in ExtCalls : ExtIssue(x, c)
  \/ \E x \in Exts : ExtTryPush(x, "A")
  \/ \E x \in Exts : ExtTryPush(x, "B")
  \/ \E x \in Exts : ExtTryPop(x)
  \/ \E x \in Exts : ExtClose(x)
  \/ \E x \in Exts : ExtDone(x)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type and safety invariants                                              *)
(***************************************************************************)

TypeOK ==
  /\ ring \in Seq(Items)
  /\ closed \in BOOLEAN
  /\ waitqP \in Seq({[f |-> g, item |-> it] : g \in Fibers,
                      it \in Items})
  /\ waitqC \in Seq(Fibers)
  /\ resolved \in Seq({[f |-> g, out |-> o] : g \in Fibers,
                        o \in Outs})
  /\ phase \in [Fibers -> {"fresh", "waiting"}]
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
  /\ clog \in Seq(Items) /\ dlog \in Seq(Items)
  /\ commits_open \in BOOLEAN /\ res_agree \in BOOLEAN
  /\ hit_inline_push \in BOOLEAN /\ hit_inline_pop \in BOOLEAN
  /\ hit_push_park \in BOOLEAN /\ hit_pop_park \in BOOLEAN
  /\ hit_handoff_pc \in BOOLEAN /\ hit_handoff_cp \in BOOLEAN
  /\ hit_close_buffered \in BOOLEAN /\ hit_close_pushpark \in BOOLEAN
  /\ hit_close_poppark \in BOOLEAN /\ hit_fifo_multi \in BOOLEAN
  /\ hit_resume \in BOOLEAN

\* Capacity: the ring never exceeds the frozen ceiling of 2.
InvCap == Len(ring) <= Cap

\* The commit/delivery ledger (the Lean `qSafe` equation): conservation
\* of items plus the external FIFO -- every delivered item was committed
\* before it, and the still-buffered items are exactly the undelivered
\* tail of the commit order.
InvLog == clog = Cat(dlog, ring)

\* Drained consumers: a suspended consumer only ever coexists with an
\* empty ring.
InvDrained == waitqC # << >> => ring = << >>

\* No commit after close (the close discipline; MutCloseCommit's kill):
\* the commit log is frozen once the bit stands.
InvNoCommitClosed == commits_open

\* Every public result matched the outcome authority (the Semaphore
\* lesson: a wrong result preserves every state invariant).
InvResultAgree == res_agree

InvQueueNoDup ==
  /\ \A i \in 1..Len(waitqP), j \in 1..Len(waitqP) :
       i # j => waitqP[i].f # waitqP[j].f
  /\ \A i \in 1..Len(waitqC), j \in 1..Len(waitqC) :
       i # j => waitqC[i] # waitqC[j]
  /\ \A i \in 1..Len(runq), j \in 1..Len(runq) :
       i # j => runq[i].fiber # runq[j].fiber
  /\ \A i \in 1..Len(waitqP), j \in 1..Len(waitqC) :
       waitqP[i].f # waitqC[j]

\* Queue ownership: a queued fiber is never runnable or dispatched and
\* sits in phase "waiting"; only published queue calls sit stale in the
\* runnable queue.
InvQueueOwnership ==
  /\ \A i \in 1..Len(waitqP) :
       /\ ~InRunq(runq, waitqP[i].f)
       /\ waitqP[i].f # cur.fiber
       /\ phase[waitqP[i].f] = "waiting"
  /\ \A i \in 1..Len(waitqC) :
       /\ ~InRunq(runq, waitqC[i])
       /\ waitqC[i] # cur.fiber
       /\ phase[waitqC[i]] = "waiting"
  /\ \A i \in 1..Len(runq) : runq[i].fiber # cur.fiber
  /\ \A i \in 1..Len(runq) :
       runq[i].fresh = FALSE => runq[i].call \in FibCalls

SameCaller(o1, o2) == o1.src = o2.src /\ o1.id = o2.id

\* Completion discipline: every completion matches a prior issue of the
\* same call by the same caller, with no intervening completion of that
\* caller -- EXCEPT the boot waiter's single pre-Init call instance (the
\* instrumented configurations start with a waiter parked past its
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

SafetyInvariants == <<TypeOK, InvCap, InvLog, InvDrained,
                      InvNoCommitClosed, InvResultAgree, InvQueueNoDup,
                      InvQueueOwnership, InvCompDiscipline>>

(***************************************************************************)
(* Coverage predicates and the witness separation                          *)
(*                                                                         *)
(* All are monotone (history append or set-once flags), so a single TLC    *)
(* run reaching the conjunctive state certifies every component; each      *)
(* coverage cfg checks the negated conjunction and must be VIOLATED.       *)
(* The mutant separation cfgs assert their witness is UNreachable and      *)
(* must HOLD.                                                              *)
(***************************************************************************)

CovInlinePush == hit_inline_push
CovInlinePop == hit_inline_pop
CovPushPark == hit_push_park
CovPopPark == hit_pop_park

\* The producer handoff: a delivery granted a parked producer (its lease
\* committed) and a resumed waiter completed.
CovHandoffPC == hit_handoff_pc /\ hit_resume

\* The consumer handoff: a commit granted a parked consumer (the item
\* handed off) and a resumed waiter completed.
CovHandoffCP == hit_handoff_cp /\ hit_resume

CovCloseBuffered == hit_close_buffered
CovClosePushPark == hit_close_pushpark /\ hit_resume
CovClosePopPark == hit_close_poppark /\ hit_resume

\* Capacity > 1 FIFO: two deliveries completed in commit order.
CovFifoMulti == hit_fifo_multi

NotHandoffPC == ~CovHandoffPC
NotHandoffCP == ~CovHandoffCP

\* A state constraint for the witness cfgs: prune every behavior that
\* leaves the witness's own call mix.  A constraint can only SHRINK the
\* explored space, so a violation found under it is still a witness; the
\* PASS cfgs run unconstrained.
WitConstraint ==
  (\A e \in exts : e.call \in {"trypop", "close"}) /\
  (\A i \in 1..Len(runq) : runq[i].call \in FibCalls)

InvCovInlinePush == ~CovInlinePush
InvCovInlinePop == ~CovInlinePop
InvCovPushPark == ~CovPushPark
InvCovPopPark == ~CovPopPark
InvCovHandoffPC == ~CovHandoffPC
InvCovHandoffCP == ~CovHandoffCP
InvCovCloseBuffered == ~CovCloseBuffered
InvCovClosePushPark == ~CovClosePushPark
InvCovClosePopPark == ~CovClosePopPark
InvCovFifoMulti == ~CovFifoMulti

=============================================================================
